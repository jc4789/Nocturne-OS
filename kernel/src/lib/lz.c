/* LZ77 blocks (lib/lz.h). */
#include "kernel.h"
#include "lib/lz.h"

#define HASH_BITS 12
#define MIN_MATCH 4
#define TAIL      5  /* the last bytes of a block are always literals: matches never reach them */

static inline uint32_t read32(const uint8_t *p) {
    uint32_t v;
    memcpy(&v, p, 4);
    return v;
}

static inline uint32_t hash4(uint32_t v) { return (v * 2654435761u) >> (32 - HASH_BITS); }

/* the bytes after a token's 15: runs of 255 and the rest */
static uint8_t *put_count(uint8_t *o, const uint8_t *end, size_t n) {
    for (; n >= 255; n -= 255) {
        if (o >= end) return NULL;
        *o++ = 255;
    }
    if (o >= end) return NULL;
    *o++ = (uint8_t)n;
    return o;
}

static uint8_t *put_sequence(uint8_t *o, const uint8_t *end, const uint8_t *lit, size_t nlit,
                             size_t dist, size_t match) {
    if (o >= end) return NULL;
    uint8_t *token = o++;
    *token = (uint8_t)((nlit >= 15 ? 15 : nlit) << 4);
    if (nlit >= 15 && !(o = put_count(o, end, nlit - 15))) return NULL;
    if ((size_t)(end - o) < nlit) return NULL;
    memcpy(o, lit, nlit);
    o += nlit;
    if (!match) return o;
    if (end - o < 2) return NULL;
    *o++ = (uint8_t)dist;
    *o++ = (uint8_t)(dist >> 8);
    size_t m = match - MIN_MATCH;
    *token |= (uint8_t)(m >= 15 ? 15 : m);
    if (m >= 15 && !(o = put_count(o, end, m - 15))) return NULL;
    return o;
}

size_t lz_compress(const uint8_t *src, size_t n, uint8_t *dst, size_t cap, uint32_t *table) {
    if (n > LZ_BLOCK_MAX) return 0;
    memset(table, 0, LZ_TABLE_BYTES);
    uint8_t *o = dst;
    const uint8_t *end = dst + cap;
    size_t anchor = 0, i = 0, limit = n > 12 ? n - 12 : 0;
    while (i < limit) {
        uint32_t v = read32(src + i), h = hash4(v);
        size_t ref = table[h];
        table[h] = (uint32_t)i;
        if (ref >= i || read32(src + ref) != v) {
            i += 1 + ((i - anchor) >> 6); /* data that does not repeat is crossed faster */
            continue;
        }
        size_t len = MIN_MATCH, most = n - TAIL - i;
        while (len < most && src[ref + len] == src[i + len]) len++;
        while (i > anchor && ref && src[i - 1] == src[ref - 1]) {
            i--;
            ref--;
            len++;
        }
        if (!(o = put_sequence(o, end, src + anchor, i - anchor, i - ref, len))) return 0;
        i += len;
        anchor = i;
    }
    if (!(o = put_sequence(o, end, src + anchor, n - anchor, 0, 0))) return 0;
    return (size_t)(o - dst);
}

static bool get_count(const uint8_t **p, const uint8_t *end, size_t *n) {
    uint8_t b;
    do {
        if (*p >= end) return false;
        b = *(*p)++;
        *n += b;
    } while (b == 255);
    return true;
}

long lz_decompress(const uint8_t *src, size_t n, uint8_t *dst, size_t cap) {
    const uint8_t *p = src, *end = src + n;
    size_t o = 0;
    while (p < end) {
        uint8_t token = *p++;
        size_t lit = token >> 4;
        if (lit == 15 && !get_count(&p, end, &lit)) return -1;
        if ((size_t)(end - p) < lit || cap - o < lit) return -1;
        memcpy(dst + o, p, lit);
        p += lit;
        o += lit;
        if (p == end) break; /* the last sequence */
        if (end - p < 2) return -1;
        size_t dist = p[0] | (size_t)p[1] << 8, len = token & 15;
        p += 2;
        if (len == 15 && !get_count(&p, end, &len)) return -1;
        len += MIN_MATCH;
        if (!dist || dist > o || cap - o < len) return -1;
        if (dist >= len) {
            memcpy(dst + o, dst + o - dist, len);
        } else { /* the copy reads what it writes: a repeating pattern */
            for (size_t k = 0; k < len; k++) dst[o + k] = dst[o + k - dist];
        }
        o += len;
    }
    return (long)o;
}
