/* DEFLATE compression (RFC 1951): LZ77 over a 32 KiB window, with lazy matching at the
   higher levels (a match waits one byte in case the next one starts a longer match),
   then for each block the cheapest of stored bytes, the fixed codes and codes built
   from the block's own frequencies. Wrappers: zlib (RFC 1950) and gzip (RFC 1952). */
#include <stdlib.h>
#include <string.h>
#include "deflate.h"

#define WSIZE 32768u
#define WMASK (WSIZE - 1)
#define WBUF (2 * WSIZE)                       /* the window and the bytes ahead of it */
#define MIN_MATCH 3
#define MAX_MATCH 258
#define MIN_LOOKAHEAD (MAX_MATCH + MIN_MATCH + 1)
#define MAX_DIST (WSIZE - MIN_LOOKAHEAD)       /* farthest match: older bytes may be gone */
#define HASH_BITS 15
#define HASH_SIZE (1u << HASH_BITS)
#define BLOCK_SYMBOLS 16384                    /* symbols a block gathers before it is sent */
#define LITLEN_CODES 286
#define DIST_CODES 30
#define CODELEN_CODES 19
#define MAX_BITS 15
#define MAX_CODELEN_BITS 7

static const uint16_t length_base[29] = {3, 4, 5, 6, 7, 8, 9, 10, 11, 13, 15, 17, 19, 23, 27,
                                         31, 35, 43, 51, 59, 67, 83, 99, 115, 131, 163, 195, 227, 258};
static const uint8_t length_extra[29] = {0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 2,
                                         3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0};
static const uint16_t dist_base[30] = {1, 2, 3, 4, 5, 7, 9, 13, 17, 25, 33, 49, 65, 97, 129,
                                       193, 257, 385, 513, 769, 1025, 1537, 2049, 3073, 4097,
                                       6145, 8193, 12289, 16385, 24577};
static const uint8_t dist_extra[30] = {0, 0, 0, 0, 1, 1, 2, 2, 3, 3, 4, 4, 5, 5, 6, 6,
                                       7, 7, 8, 8, 9, 9, 10, 10, 11, 11, 12, 12, 13, 13};
/* the order the code length code's lengths are sent in */
static const uint8_t codelen_order[CODELEN_CODES] = {16, 17, 18, 0, 8, 7, 9, 6, 10, 5, 11, 4, 12, 3, 13, 2, 14, 1, 15};

/* zlib's level table: a match this long is good enough to search less, long enough
   not to wait for a better one, nice enough to stop searching; chain is how many
   earlier places with the same three bytes are tried */
static const struct { uint16_t good, lazy, nice, chain; bool wait; } levels[10] = {
    {0, 0, 0, 0, false},
    {4, 4, 8, 4, false}, {4, 5, 16, 8, false}, {4, 6, 32, 32, false},
    {4, 4, 16, 16, true}, {8, 16, 32, 32, true}, {8, 16, 128, 128, true},
    {8, 32, 128, 256, true}, {32, 128, 258, 1024, true}, {32, 258, 258, 4096, true},
};

struct deflate_stream {
    enum deflate_wrap wrap;
    int level;
    unsigned good, lazy, nice, chain;
    bool wait;
    uint8_t window[WBUF];
    int32_t head[HASH_SIZE];  /* newest window place of each three-byte hash, or -1 */
    int32_t prev[WSIZE];      /* the place before it with the same hash, by place mod WSIZE */
    size_t strstart, lookahead;
    size_t block_start;       /* the window place of the block's first byte */
    size_t tally_end;         /* the block's symbols cover block_start..tally_end */
    bool match_available;     /* a match (or literal) at strstart - 1 waits for this place */
    unsigned prev_length, prev_dist;
    uint16_t sym_value[BLOCK_SYMBOLS];  /* a literal byte, or a match's length */
    uint16_t sym_dist[BLOCK_SYMBOLS];   /* 0 for a literal */
    unsigned symbols;
    uint32_t freq_lit[LITLEN_CODES], freq_dist[DIST_CODES];
    uint16_t fixed_lit_code[288];
    uint8_t fixed_lit_len[288];
    uint16_t fixed_dist_code[DIST_CODES];
    uint64_t bitbuf;
    unsigned bitcount;
    uint32_t adler_a, adler_b, crc, crc_table[256];
    uint64_t total_in;
    bool started, finished, failed;
};

size_t deflate_bound(size_t n) {
    /* A block ends after BLOCK_SYMBOLS symbols (each a byte or more), before the window
       slides (every WSIZE bytes) or at the end: at most three blocks in WSIZE bytes and the
       final one. Each costs at most its bytes stored in two pieces, 6 bytes of header each. */
    size_t blocks = 3 * (n / WSIZE + 1) + 1;
    size_t extra = blocks * 12 + 18 + 8;
    return n > SIZE_MAX - extra ? 0 : n + extra;
}

static void put_byte(struct deflate_stream *s, struct deflate_out *o, uint8_t b) {
    if (s->failed) return;
    if (o->len == o->cap) {
        if (!o->grow) { s->failed = true; return; }
        size_t cap = o->cap ? o->cap * 2 : 4096;
        uint8_t *p = cap > o->cap ? realloc(o->p, cap) : NULL;
        if (!p) { s->failed = true; return; }
        o->p = p;
        o->cap = cap;
    }
    o->p[o->len++] = b;
}

static void put_bits(struct deflate_stream *s, struct deflate_out *o, uint32_t value, unsigned count) {
    s->bitbuf |= (uint64_t)value << s->bitcount;
    s->bitcount += count;
    while (s->bitcount >= 8) {
        put_byte(s, o, (uint8_t)s->bitbuf);
        s->bitbuf >>= 8;
        s->bitcount -= 8;
    }
}

static void align_byte(struct deflate_stream *s, struct deflate_out *o) {
    if (s->bitcount) put_bits(s, o, 0, 8 - s->bitcount);
}

static unsigned reverse_bits(unsigned code, unsigned len) {
    unsigned r = 0;
    for (unsigned i = 0; i < len; i++) r = r << 1 | (code >> i & 1);
    return r;
}

/* Canonical codes (RFC 1951 3.2.2) for the lengths, bit-reversed: Huffman codes are
   sent most significant bit first into a stream packed from the least. */
static void code_words(const uint8_t *len, unsigned n, uint16_t *code) {
    unsigned count[MAX_BITS + 1] = {0}, next[MAX_BITS + 1];
    for (unsigned i = 0; i < n; i++) count[len[i]]++;
    count[0] = 0;
    unsigned c = 0;
    for (unsigned b = 1; b <= MAX_BITS; b++) {
        c = (c + count[b - 1]) << 1;
        next[b] = c;
    }
    for (unsigned i = 0; i < n; i++) code[i] = len[i] ? (uint16_t)reverse_bits(next[len[i]]++, len[i]) : 0;
}

/* Code lengths of at most limit bits for the frequencies: a Huffman code whose lengths
   over the limit are cut to it, then made complete again by lengthening shorter codes;
   the least frequent symbols take the longest codes. At least two symbols get codes,
   as zlib does for decoders that reject a code of one symbol. */
static void code_lengths(const uint32_t *freq, unsigned n, unsigned limit, uint8_t *len) {
    unsigned leaf[LITLEN_CODES];
    uint32_t weight[2 * LITLEN_CODES];
    uint16_t parent[2 * LITLEN_CODES];
    uint8_t depth[2 * LITLEN_CODES];
    unsigned m = 0;
    memset(len, 0, n);
    for (unsigned i = 0; i < n; i++)
        if (freq[i]) leaf[m++] = i;
    for (unsigned i = 0; m < 2 && i < n; i++)
        if (!freq[i] && (m == 0 || leaf[0] != i)) leaf[m++] = i;
    /* leaves by frequency, ties by symbol (insertion sort: at most 286 entries) */
    for (unsigned i = 1; i < m; i++) {
        unsigned v = leaf[i], j = i;
        while (j && (freq[leaf[j - 1]] > freq[v] || (freq[leaf[j - 1]] == freq[v] && leaf[j - 1] > v))) {
            leaf[j] = leaf[j - 1];
            j--;
        }
        leaf[j] = v;
    }
    /* two queues: the sorted leaves, and the joined nodes, which are made in order */
    for (unsigned i = 0; i < m; i++) weight[i] = freq[leaf[i]];
    unsigned next_leaf = 0, next_node = m, made = m;
    for (unsigned k = 0; k + 1 < m; k++) {
        unsigned pick[2];
        for (int t = 0; t < 2; t++) {
            if (next_leaf < m && (next_node >= made || weight[next_leaf] <= weight[next_node])) pick[t] = next_leaf++;
            else pick[t] = next_node++;
        }
        weight[made] = weight[pick[0]] + weight[pick[1]];
        parent[pick[0]] = parent[pick[1]] = (uint16_t)made;
        made++;
    }
    depth[made - 1] = 0;
    for (unsigned i = made - 1; i-- > 0;) depth[i] = depth[parent[i]] + 1 > 63 ? 63 : depth[parent[i]] + 1;
    unsigned count[64] = {0};
    for (unsigned i = 0; i < m; i++) count[depth[i] > limit ? limit : depth[i]]++;
    uint32_t total = 0;
    for (unsigned b = 1; b <= limit; b++) total += (uint32_t)count[b] << (limit - b);
    while (total > (1u << limit)) {
        count[limit]--;
        for (unsigned b = limit - 1; b > 0; b--)
            if (count[b]) {
                count[b]--;
                count[b + 1] += 2;
                break;
            }
        total--;
    }
    unsigned k = 0;
    for (unsigned b = limit; b >= 1; b--)
        for (unsigned c = 0; c < count[b]; c++) len[leaf[k++]] = (uint8_t)b;
}

static unsigned length_code(unsigned length) {
    unsigned i = 28;
    while (length_base[i] > length) i--;
    return i;
}

static unsigned dist_code(unsigned dist) {
    unsigned i = 29;
    while (dist_base[i] > dist) i--;
    return i;
}

/* The code lengths of both codes, run-length coded with the code length alphabet:
   16 repeats the previous length 3 to 6 times, 17 and 18 send 3 to 10 and 11 to 138 zeros. */
static unsigned codelen_runs(const uint8_t *lens, unsigned n, uint8_t *sym, uint8_t *extra, uint32_t *freq) {
    unsigned k = 0;
    for (unsigned i = 0; i < n;) {
        uint8_t v = lens[i];
        unsigned run = 1;
        while (i + run < n && lens[i + run] == v) run++;
        i += run;
        if (!v) {
            while (run >= 11) {
                unsigned r = run > 138 ? 138 : run;
                sym[k] = 18; extra[k++] = (uint8_t)(r - 11); freq[18]++;
                run -= r;
            }
            if (run >= 3) {
                sym[k] = 17; extra[k++] = (uint8_t)(run - 3); freq[17]++;
                run = 0;
            }
        } else {
            sym[k] = v; extra[k++] = 0; freq[v]++;
            run--;
            while (run >= 3) {
                unsigned r = run > 6 ? 6 : run;
                sym[k] = 16; extra[k++] = (uint8_t)(r - 3); freq[16]++;
                run -= r;
            }
        }
        while (run--) { sym[k] = v; extra[k++] = 0; freq[v]++; }
    }
    return k;
}

static void send_symbols(struct deflate_stream *s, struct deflate_out *o, const uint16_t *lit_code, const uint8_t *lit_len,
                         const uint16_t *dcode, const uint8_t *dlen) {
    for (unsigned k = 0; k < s->symbols && !s->failed; k++) {
        unsigned v = s->sym_value[k], d = s->sym_dist[k];
        if (!d) {
            put_bits(s, o, lit_code[v], lit_len[v]);
            continue;
        }
        unsigned lc = length_code(v), dc = dist_code(d);
        put_bits(s, o, lit_code[257 + lc], lit_len[257 + lc]);
        if (length_extra[lc]) put_bits(s, o, v - length_base[lc], length_extra[lc]);
        put_bits(s, o, dcode[dc], dlen[dc]);
        if (dist_extra[dc]) put_bits(s, o, d - dist_base[dc], dist_extra[dc]);
    }
    put_bits(s, o, lit_code[256], lit_len[256]);
}

/* Sends the block of window[block_start..tally_end) and its symbols in the cheapest form. */
static void flush_block(struct deflate_stream *s, struct deflate_out *o, bool final) {
    s->freq_lit[256]++;
    uint8_t lit_len[LITLEN_CODES], dlen[DIST_CODES], cl_len[CODELEN_CODES];
    uint16_t lit_code[LITLEN_CODES], dcode[DIST_CODES], cl_code[CODELEN_CODES];
    code_lengths(s->freq_lit, LITLEN_CODES, MAX_BITS, lit_len);
    code_lengths(s->freq_dist, DIST_CODES, MAX_BITS, dlen);
    unsigned hlit = LITLEN_CODES, hdist = DIST_CODES;
    while (hlit > 257 && !lit_len[hlit - 1]) hlit--;
    while (hdist > 1 && !dlen[hdist - 1]) hdist--;
    uint8_t all[LITLEN_CODES + DIST_CODES], run_sym[LITLEN_CODES + DIST_CODES], run_extra[LITLEN_CODES + DIST_CODES];
    memcpy(all, lit_len, hlit);
    memcpy(all + hlit, dlen, hdist);
    uint32_t cl_freq[CODELEN_CODES] = {0};
    unsigned runs = codelen_runs(all, hlit + hdist, run_sym, run_extra, cl_freq);
    code_lengths(cl_freq, CODELEN_CODES, MAX_CODELEN_BITS, cl_len);
    unsigned hclen = CODELEN_CODES;
    while (hclen > 4 && !cl_len[codelen_order[hclen - 1]]) hclen--;

    uint64_t extra_bits = 0, dynamic = 3 + 5 + 5 + 4 + 3 * (uint64_t)hclen, fixed = 3;
    for (unsigned i = 0; i < runs; i++)
        dynamic += cl_len[run_sym[i]] + (run_sym[i] == 16 ? 2 : run_sym[i] == 17 ? 3 : run_sym[i] == 18 ? 7 : 0);
    for (unsigned i = 0; i < LITLEN_CODES; i++) {
        dynamic += (uint64_t)s->freq_lit[i] * lit_len[i];
        fixed += (uint64_t)s->freq_lit[i] * s->fixed_lit_len[i];
        if (i > 256) extra_bits += (uint64_t)s->freq_lit[i] * length_extra[i - 257];
    }
    for (unsigned i = 0; i < DIST_CODES; i++) {
        dynamic += (uint64_t)s->freq_dist[i] * dlen[i];
        fixed += (uint64_t)s->freq_dist[i] * 5;
        extra_bits += (uint64_t)s->freq_dist[i] * dist_extra[i];
    }
    dynamic += extra_bits;
    fixed += extra_bits;
    size_t raw = s->tally_end - s->block_start;
    uint64_t stored = 0;
    unsigned pad = (8 - (s->bitcount + 3) % 8) % 8;
    for (size_t at = 0;; pad = 5) {
        size_t piece = raw - at > 65535 ? 65535 : raw - at;
        stored += 3 + pad + 32 + 8 * (uint64_t)piece;
        at += piece;
        if (at == raw) break;
    }

    if (stored <= fixed && stored <= dynamic) {
        for (size_t at = 0;;) {
            size_t piece = raw - at > 65535 ? 65535 : raw - at;
            put_bits(s, o, final && at + piece == raw, 1);
            put_bits(s, o, 0, 2);
            align_byte(s, o);
            put_bits(s, o, (uint32_t)piece, 16);
            put_bits(s, o, (uint32_t)~piece & 0xFFFF, 16);
            for (size_t i = 0; i < piece; i++) put_byte(s, o, s->window[s->block_start + at + i]);
            at += piece;
            if (at == raw) break;
        }
    } else if (fixed <= dynamic) {
        put_bits(s, o, final, 1);
        put_bits(s, o, 1, 2);
        send_symbols(s, o, s->fixed_lit_code, s->fixed_lit_len, s->fixed_dist_code, (const uint8_t[DIST_CODES]){
            5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5});
    } else {
        code_words(lit_len, LITLEN_CODES, lit_code);
        code_words(dlen, DIST_CODES, dcode);
        code_words(cl_len, CODELEN_CODES, cl_code);
        put_bits(s, o, final, 1);
        put_bits(s, o, 2, 2);
        put_bits(s, o, hlit - 257, 5);
        put_bits(s, o, hdist - 1, 5);
        put_bits(s, o, hclen - 4, 4);
        for (unsigned i = 0; i < hclen; i++) put_bits(s, o, cl_len[codelen_order[i]], 3);
        for (unsigned i = 0; i < runs; i++) {
            put_bits(s, o, cl_code[run_sym[i]], cl_len[run_sym[i]]);
            if (run_sym[i] >= 16) put_bits(s, o, run_extra[i], run_sym[i] == 16 ? 2 : run_sym[i] == 17 ? 3 : 7);
        }
        send_symbols(s, o, lit_code, lit_len, dcode, dlen);
    }
    s->symbols = 0;
    memset(s->freq_lit, 0, sizeof s->freq_lit);
    memset(s->freq_dist, 0, sizeof s->freq_dist);
    s->block_start = s->tally_end;
}

static void tally(struct deflate_stream *s, struct deflate_out *o, unsigned value, unsigned dist) {
    s->sym_value[s->symbols] = (uint16_t)value;
    s->sym_dist[s->symbols++] = (uint16_t)dist;
    if (dist) {
        s->freq_lit[257 + length_code(value)]++;
        s->freq_dist[dist_code(dist)]++;
        s->tally_end += value;
    } else {
        s->freq_lit[value]++;
        s->tally_end++;
    }
    if (s->symbols == BLOCK_SYMBOLS) flush_block(s, o, false);
}

static uint32_t hash3(const uint8_t *p) {
    return ((uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16) * 2654435761u >> (32 - HASH_BITS);
}

/* Enters place at in the hash chains; the previous place with its three bytes, or -1. */
static int32_t insert(struct deflate_stream *s, size_t at) {
    uint32_t h = hash3(s->window + at);
    int32_t previous = s->head[h];
    s->prev[at & WMASK] = previous;
    s->head[h] = (int32_t)at;
    return previous;
}

/* The longest match at place at, longer than shorter, among the chain from candidate;
   0 when there is none. *dist is its distance. */
static unsigned longest_match(struct deflate_stream *s, size_t at, int32_t candidate, unsigned shorter, unsigned *dist) {
    unsigned chain = s->chain, nice = s->nice;
    size_t most = s->lookahead < MAX_MATCH ? s->lookahead : MAX_MATCH;
    if (shorter >= s->good) chain >>= 2;
    if (nice > most) nice = (unsigned)most;
    unsigned best = shorter < MIN_MATCH - 1 ? MIN_MATCH - 1 : shorter, found = 0;
    const uint8_t *scan = s->window + at;
    while (candidate >= 0 && best < most && at - (size_t)candidate <= MAX_DIST) {
        const uint8_t *m = s->window + candidate;
        if (m[best] == scan[best] && m[0] == scan[0] && m[1] == scan[1]) {
            unsigned l = 2;
            while (l < most && m[l] == scan[l]) l++;
            if (l > best) {
                best = found = l;
                *dist = (unsigned)(at - (size_t)candidate);
                if (l >= nice) break;
            }
        }
        if (!--chain) break;
        int32_t next = s->prev[candidate & WMASK];
        if (next >= candidate) break;
        candidate = next;
    }
    return found;
}

/* Turns the window's bytes into symbols while enough lie ahead for a longest match,
   or, at the end of the input, all of them. */
static void compress(struct deflate_stream *s, struct deflate_out *o, bool ending) {
    while (s->lookahead && !s->failed && (ending || (s->lookahead >= MIN_LOOKAHEAD && s->strstart < WBUF - MIN_LOOKAHEAD))) {
        size_t at = s->strstart;
        unsigned len = 0, dist = 0;
        int32_t candidate = s->lookahead >= MIN_MATCH ? insert(s, at) : -1;
        if (!s->wait) {
            if (candidate >= 0) len = longest_match(s, at, candidate, 0, &dist);
            if (len >= MIN_MATCH) {
                tally(s, o, len, dist);
                if (len <= s->lazy)
                    for (size_t k = at + 1; k < at + len && k + MIN_MATCH <= at + s->lookahead; k++) insert(s, k);
                s->strstart += len;
                s->lookahead -= len;
            } else {
                tally(s, o, s->window[at], 0);
                s->strstart++;
                s->lookahead--;
            }
            continue;
        }
        if (candidate >= 0 && (!s->match_available || s->prev_length < s->lazy))
            len = longest_match(s, at, candidate, s->match_available ? s->prev_length : 0, &dist);
        if (s->match_available && s->prev_length >= MIN_MATCH && len <= s->prev_length) {
            /* the waiting match covers at - 1 .. at + prev_length - 2; at is entered already */
            size_t end = at - 1 + s->prev_length;
            tally(s, o, s->prev_length, s->prev_dist);
            for (size_t k = at + 1; k < end && k + MIN_MATCH <= at + s->lookahead; k++) insert(s, k);
            s->lookahead -= end - at;
            s->strstart = end;
            s->match_available = false;
            s->prev_length = 0;
        } else {
            if (s->match_available) tally(s, o, s->window[at - 1], 0);
            s->match_available = true;
            s->prev_length = len;
            s->prev_dist = dist;
            s->strstart++;
            s->lookahead--;
        }
    }
}

/* Drops the window's older half to make room ahead. The block in progress is sent
   first when its bytes would go, so a block can always be sent stored. */
static void slide(struct deflate_stream *s, struct deflate_out *o) {
    if (s->block_start < WSIZE) flush_block(s, o, false);
    memmove(s->window, s->window + WSIZE, s->strstart + s->lookahead - WSIZE);
    s->strstart -= WSIZE;
    s->block_start -= WSIZE;
    s->tally_end -= WSIZE;
    for (unsigned i = 0; i < HASH_SIZE; i++) s->head[i] = s->head[i] >= (int32_t)WSIZE ? s->head[i] - (int32_t)WSIZE : -1;
    for (unsigned i = 0; i < WSIZE; i++) s->prev[i] = s->prev[i] >= (int32_t)WSIZE ? s->prev[i] - (int32_t)WSIZE : -1;
}

static void checksum(struct deflate_stream *s, const uint8_t *p, size_t n) {
    if (s->wrap == DEFLATE_ZLIB) {
        uint32_t a = s->adler_a, b = s->adler_b;
        while (n) {
            size_t chunk = n < 5552 ? n : 5552; /* the most bytes before b can overflow */
            n -= chunk;
            while (chunk--) {
                a += *p++;
                b += a;
            }
            a %= 65521;
            b %= 65521;
        }
        s->adler_a = a;
        s->adler_b = b;
    } else if (s->wrap == DEFLATE_GZIP) {
        uint32_t c = ~s->crc;
        while (n--) c = s->crc_table[(c ^ *p++) & 0xFF] ^ (c >> 8);
        s->crc = ~c;
    }
}

struct deflate_stream *deflate_begin(int level, enum deflate_wrap wrap) {
    struct deflate_stream *s = malloc(sizeof *s);
    if (!s) return NULL;
    memset(s, 0, offsetof(struct deflate_stream, window));
    if (level < 1) level = 1;
    if (level > 9) level = 9;
    s->level = level;
    s->wrap = wrap;
    s->good = levels[level].good;
    s->lazy = levels[level].lazy;
    s->nice = levels[level].nice;
    s->chain = levels[level].chain;
    s->wait = levels[level].wait;
    memset(s->head, 0xFF, sizeof s->head);
    memset(s->prev, 0xFF, sizeof s->prev);
    s->strstart = s->lookahead = s->block_start = s->tally_end = 0;
    s->match_available = false;
    s->prev_length = s->prev_dist = 0;
    s->symbols = 0;
    memset(s->freq_lit, 0, sizeof s->freq_lit);
    memset(s->freq_dist, 0, sizeof s->freq_dist);
    for (unsigned i = 0; i < 288; i++) s->fixed_lit_len[i] = i < 144 ? 8 : i < 256 ? 9 : i < 280 ? 7 : 8;
    code_words(s->fixed_lit_len, 288, s->fixed_lit_code);
    uint8_t five[DIST_CODES];
    memset(five, 5, sizeof five);
    code_words(five, DIST_CODES, s->fixed_dist_code);
    s->bitbuf = 0;
    s->bitcount = 0;
    s->adler_a = 1;
    s->adler_b = 0;
    s->crc = 0;
    for (uint32_t i = 0; i < 256; i++) {
        uint32_t v = i;
        for (int k = 0; k < 8; k++) v = v & 1 ? 0xEDB88320u ^ (v >> 1) : v >> 1;
        s->crc_table[i] = v;
    }
    s->total_in = 0;
    s->started = s->finished = s->failed = false;
    return s;
}

int deflate_feed(struct deflate_stream *s, const void *in, size_t n, bool last, struct deflate_out *out) {
    if (!s || s->finished || s->failed || (n && !in)) return -1;
    if (!s->started) {
        s->started = true;
        if (s->wrap == DEFLATE_ZLIB) {
            unsigned flevel = s->level == 1 ? 0 : s->level < 6 ? 1 : s->level == 6 ? 2 : 3;
            unsigned cmf = 0x78, flg = flevel << 6;
            flg += 31 - (cmf * 256 + flg) % 31;
            put_byte(s, out, (uint8_t)cmf);
            put_byte(s, out, (uint8_t)flg);
        } else if (s->wrap == DEFLATE_GZIP) {
            static const uint8_t header[10] = {0x1F, 0x8B, 8, 0, 0, 0, 0, 0, 0, 255};
            for (int i = 0; i < 10; i++) put_byte(s, out, i == 8 ? (s->level == 9 ? 2 : s->level == 1 ? 4 : 0) : header[i]);
        }
    }
    const uint8_t *p = in;
    checksum(s, p, n);
    s->total_in += n;
    for (;;) {
        size_t room = WBUF - (s->strstart + s->lookahead), take = n < room ? n : room;
        if (take) memcpy(s->window + s->strstart + s->lookahead, p, take);
        s->lookahead += take;
        p += take;
        n -= take;
        bool ending = last && !n;
        compress(s, out, ending);
        if (s->failed) return -1;
        if (ending || (!n && s->strstart < WBUF - MIN_LOOKAHEAD)) break;
        slide(s, out);
    }
    if (last) {
        if (s->match_available) tally(s, out, s->window[s->strstart - 1], 0);
        s->match_available = false;
        flush_block(s, out, true);
        align_byte(s, out);
        if (s->wrap == DEFLATE_ZLIB) {
            uint32_t adler = s->adler_b << 16 | s->adler_a;
            for (int shift = 24; shift >= 0; shift -= 8) put_byte(s, out, (uint8_t)(adler >> shift));
        } else if (s->wrap == DEFLATE_GZIP) {
            uint32_t size = (uint32_t)s->total_in;
            for (int shift = 0; shift < 32; shift += 8) put_byte(s, out, (uint8_t)(s->crc >> shift));
            for (int shift = 0; shift < 32; shift += 8) put_byte(s, out, (uint8_t)(size >> shift));
        }
        s->finished = true;
    }
    return s->failed ? -1 : 0;
}

void deflate_end(struct deflate_stream *s) { free(s); }
