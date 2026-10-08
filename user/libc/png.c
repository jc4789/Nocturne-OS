/* PNG encoder: 8-bit RGB, one "Sub"-filtered IDAT compressed with LZ77 + fixed Huffman codes.
   That is far from optimal, but screens (flat colours, repeated rows) compress well with it. */
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include "png.h"

/* ---- growing output buffer + LSB-first bit writer ---- */
struct out {
    uint8_t *p;
    size_t len, cap;
    bool oom;
    uint32_t bits;
    int nbits;
};

static void put(struct out *o, const void *src, size_t n) {
    if (o->oom) return;
    if (o->len + n > o->cap) {
        size_t nc = o->cap ? o->cap * 2 : 65536;
        while (nc < o->len + n) nc *= 2;
        uint8_t *np = realloc(o->p, nc);
        if (!np) {
            o->oom = true;
            return;
        }
        o->p = np;
        o->cap = nc;
    }
    memcpy(o->p + o->len, src, n);
    o->len += n;
}

static void put_be32(struct out *o, uint32_t v) {
    uint8_t b[4] = {(uint8_t)(v >> 24), (uint8_t)(v >> 16), (uint8_t)(v >> 8), (uint8_t)v};
    put(o, b, 4);
}

static void bits(struct out *o, uint32_t v, int n) {
    o->bits |= v << o->nbits;
    o->nbits += n;
    while (o->nbits >= 8) {
        uint8_t b = (uint8_t)o->bits;
        put(o, &b, 1);
        o->bits >>= 8;
        o->nbits -= 8;
    }
}

static void flush_bits(struct out *o) {
    if (o->nbits) bits(o, 0, 8 - o->nbits);
}

/* Huffman codes are sent most significant bit first */
static void huff(struct out *o, uint32_t code, int len) {
    uint32_t r = 0;
    for (int i = 0; i < len; i++) r |= ((code >> i) & 1) << (len - 1 - i);
    bits(o, r, len);
}

static void lit(struct out *o, int v) {
    if (v < 144) huff(o, 0x30 + v, 8);
    else if (v < 256) huff(o, 0x190 + v - 144, 9);
    else if (v < 280) huff(o, v - 256, 7);
    else huff(o, 0xC0 + v - 280, 8);
}

static const uint16_t len_base[29] = {3,  4,  5,  6,  7,  8,  9,  10, 11,  13,  15,  17,  19,  23, 27,
                                      31, 35, 43, 51, 59, 67, 83, 99, 115, 131, 163, 195, 227, 258};
static const uint8_t len_extra[29] = {0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 2, 3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0};
static const uint16_t dist_base[30] = {1,   2,   3,   4,   5,   7,    9,    13,   17,   25,   33,   49,   65,    97,    129,
                                       193, 257, 385, 513, 769, 1025, 1537, 2049, 3073, 4097, 6145, 8193, 12289, 16385, 24577};
static const uint8_t dist_extra[30] = {0, 0, 0, 0, 1, 1, 2, 2, 3, 3, 4, 4, 5, 5, 6, 6, 7, 7, 8, 8, 9, 9, 10, 10, 11, 11, 12, 12, 13, 13};

static void match(struct out *o, int len, int dist) {
    int i = 28;
    while (len_base[i] > len) i--;
    lit(o, 257 + i);
    if (len_extra[i]) bits(o, (uint32_t)(len - len_base[i]), len_extra[i]);
    int d = 29;
    while (dist_base[d] > dist) d--;
    huff(o, (uint32_t)d, 5);
    if (dist_extra[d]) bits(o, (uint32_t)(dist - dist_base[d]), dist_extra[d]);
}

#define WIN 32768
#define HASH_BITS 15
#define MAX_CHAIN 48

/* zlib stream of src: one fixed-Huffman block */
static void deflate_zlib(struct out *o, const uint8_t *src, size_t n) {
    uint8_t hdr[2] = {0x78, 0x01};
    put(o, hdr, 2);
    bits(o, 1, 1); /* final block */
    bits(o, 1, 2); /* fixed Huffman */
    int32_t *head = malloc(sizeof(int32_t) << HASH_BITS);
    int32_t *prev = malloc(sizeof(int32_t) * WIN);
    if (!head || !prev) {
        o->oom = true;
        free(head);
        free(prev);
        return;
    }
    memset(head, 0xFF, sizeof(int32_t) << HASH_BITS);
    size_t i = 0;
#define HASH(p) ((((uint32_t)(p)[0] << 10) ^ ((uint32_t)(p)[1] << 5) ^ (p)[2]) & ((1 << HASH_BITS) - 1))
    while (i < n) {
        int best = 0, bdist = 0;
        if (i + 3 <= n) {
            uint32_t h = HASH(src + i);
            int32_t cand = head[h];
            int chain = MAX_CHAIN;
            size_t maxlen = n - i < 258 ? n - i : 258;
            while (cand >= 0 && i - (size_t)cand <= WIN - 1 && chain--) {
                const uint8_t *a = src + cand, *b = src + i;
                if (a[best] == b[best]) {
                    size_t l = 0;
                    while (l < maxlen && a[l] == b[l]) l++;
                    if ((int)l > best) {
                        best = (int)l;
                        bdist = (int)(i - (size_t)cand);
                        if (l == maxlen) break;
                    }
                }
                int32_t nx = prev[cand % WIN];
                if (nx >= cand) break;
                cand = nx;
            }
        }
        int adv = best >= 3 ? best : 1;
        if (best >= 3) match(o, best, bdist);
        else lit(o, src[i]);
        for (int k = 0; k < adv; k++, i++) {
            if (i + 3 <= n) {
                uint32_t h = HASH(src + i);
                prev[i % WIN] = head[h];
                head[h] = (int32_t)i;
            }
        }
    }
    lit(o, 256);
    flush_bits(o);
    free(head);
    free(prev);
    uint32_t a = 1, b = 0;
    for (size_t k = 0; k < n; k++) {
        a = (a + src[k]) % 65521;
        b = (b + a) % 65521;
    }
    put_be32(o, b << 16 | a);
}

static uint32_t crc_table[256];

static uint32_t crc32_update(uint32_t c, const uint8_t *p, size_t n) {
    if (!crc_table[1])
        for (uint32_t i = 0; i < 256; i++) {
            uint32_t v = i;
            for (int k = 0; k < 8; k++) v = v & 1 ? 0xEDB88320u ^ (v >> 1) : v >> 1;
            crc_table[i] = v;
        }
    c = ~c;
    while (n--) c = crc_table[(c ^ *p++) & 0xFF] ^ (c >> 8);
    return ~c;
}

static void chunk(struct out *o, const char *type, const uint8_t *data, size_t n) {
    put_be32(o, (uint32_t)n);
    size_t at = o->len;
    put(o, type, 4);
    put(o, data, n);
    if (!o->oom) put_be32(o, crc32_update(0, o->p + at, n + 4));
}

int png_encode(const uint32_t *px, int w, int h, int stride, uint8_t **out, size_t *outlen) {
    size_t row = (size_t)w * 3 + 1;
    uint8_t *raw = malloc(row * (size_t)h);
    if (!raw) return -1;
    for (int y = 0; y < h; y++) {
        uint8_t *r = raw + row * (size_t)y;
        const uint32_t *s = px + (size_t)y * (size_t)stride;
        r[0] = 1; /* Sub: each byte minus the one a pixel to the left */
        uint8_t pr = 0, pg = 0, pb = 0;
        for (int x = 0; x < w; x++) {
            uint8_t R = (uint8_t)(s[x] >> 16), G = (uint8_t)(s[x] >> 8), B = (uint8_t)s[x];
            r[1 + x * 3] = (uint8_t)(R - pr);
            r[2 + x * 3] = (uint8_t)(G - pg);
            r[3 + x * 3] = (uint8_t)(B - pb);
            pr = R, pg = G, pb = B;
        }
    }
    struct out o = {0};
    put(&o, "\x89PNG\r\n\x1a\n", 8);
    uint8_t ihdr[13] = {(uint8_t)(w >> 24), (uint8_t)(w >> 16), (uint8_t)(w >> 8), (uint8_t)w,
                        (uint8_t)(h >> 24), (uint8_t)(h >> 16), (uint8_t)(h >> 8), (uint8_t)h,
                        8, 2, 0, 0, 0};
    chunk(&o, "IHDR", ihdr, 13);
    struct out z = {0};
    deflate_zlib(&z, raw, row * (size_t)h);
    free(raw);
    if (z.oom) {
        free(z.p);
        free(o.p);
        return -1;
    }
    chunk(&o, "IDAT", z.p, z.len);
    free(z.p);
    chunk(&o, "IEND", NULL, 0);
    if (o.oom) {
        free(o.p);
        return -1;
    }
    *out = o.p;
    *outlen = o.len;
    return 0;
}

size_t png_encode_rgba_bound(int w, int h) {
    if (w <= 0 || h <= 0 || (size_t)w > (SIZE_MAX-1)/4) return 0;
    size_t row = (size_t)w*4+1;
    if ((size_t)h > SIZE_MAX/row) return 0;
    size_t raw = row*(size_t)h;
    /* The IDAT length is a 32-bit field. This also bounds the arithmetic. */
    if (raw > UINT32_MAX-6 || (raw+65534)/65535 > (UINT32_MAX-raw-6)/5) return 0;
    size_t z = raw + ((raw+65534)/65535)*5 + 6;
    return z > SIZE_MAX-78 ? 0 : z+78;
}
static void fixed_be32(uint8_t *b, uint32_t v) {
    b[0]=(uint8_t)(v>>24); b[1]=(uint8_t)(v>>16); b[2]=(uint8_t)(v>>8); b[3]=(uint8_t)v;
}
static uint8_t *fixed_chunk(uint8_t *at, const char *type, const uint8_t *data, size_t n) {
    fixed_be32(at,(uint32_t)n); memcpy(at+4,type,4);
    if (n) memcpy(at+8,data,n);
    fixed_be32(at+8+n,crc32_update(0,at+4,n+4)); return at+12+n;
}
int png_encode_rgba(const uint32_t *px, int w, int h, int stride, uint8_t **out, size_t *outlen) {
    if (!out || !outlen) return -1;
    *out=NULL; *outlen=0;
    size_t bound=png_encode_rgba_bound(w,h);
    if (!bound || !px || stride < w || (size_t)(h-1) > (SIZE_MAX/4-(size_t)w)/(size_t)stride) return -1;
    uint8_t *png=malloc(bound); if (!png) return -1;
    memcpy(png,"\x89PNG\r\n\x1a\n",8);
    uint8_t ihdr[13]={0}; fixed_be32(ihdr,(uint32_t)w); fixed_be32(ihdr+4,(uint32_t)h);
    ihdr[8]=8; ihdr[9]=6; /* 8-bit RGBA, not premultiplied */
    uint8_t *at=fixed_chunk(png+8,"IHDR",ihdr,13);
    uint8_t phys[9]; fixed_be32(phys,3780); fixed_be32(phys+4,3780); phys[8]=1;
    at=fixed_chunk(at,"pHYs",phys,9);
    size_t row=(size_t)w*4+1,raw=row*(size_t)h,zlen=bound-78;
    fixed_be32(at,(uint32_t)zlen); memcpy(at+4,"IDAT",4);
    uint8_t *z=at+8; *z++=0x78; *z++=0x01;
    uint32_t a=1,b=0; size_t done=0;
    while (done < raw) {
        size_t length=raw-done; if (length>65535) length=65535;
        *z++=(uint8_t)(done+length == raw); /* BFINAL, BTYPE=stored */
        *z++=(uint8_t)length; *z++=(uint8_t)(length>>8);
        *z++=(uint8_t)~length; *z++=(uint8_t)(~length>>8);
        for (size_t i=0;i<length;i++,done++) {
            size_t column=done%row; uint8_t byte=0; /* filter None */
            if (column) {
                size_t pixel=(column-1)/4,channel=(column-1)%4;
                uint32_t value=px[(done/row)*(size_t)stride+pixel];
                unsigned shift=channel==3?24:(unsigned)(2-channel)*8;
                byte=(uint8_t)(value>>shift);
            }
            *z++=byte; a+=byte; if (a>=65521) a-=65521;
            b+=a; if (b>=65521) b-=65521;
        }
    }
    fixed_be32(z,(b<<16)|a); z+=4;
    fixed_be32(z,crc32_update(0,at+4,zlen+4)); z+=4;
    z=fixed_chunk(z,"IEND",NULL,0);
    if ((size_t)(z-png)!=bound) { free(png); return -1; }
    *out=png; *outlen=bound; return 0;
}

/* box-filter downscale by an integer factor (2 = half size) */
uint32_t *img_shrink(const uint32_t *px, int w, int h, int stride, int f, int *nw, int *nh) {
    if (f < 1) f = 1;
    int W = w / f, H = h / f;
    uint32_t *d = malloc(sizeof(uint32_t) * (size_t)W * (size_t)H);
    if (!d) return NULL;
    for (int y = 0; y < H; y++)
        for (int x = 0; x < W; x++) {
            uint32_t r = 0, g = 0, b = 0;
            for (int j = 0; j < f; j++)
                for (int i = 0; i < f; i++) {
                    uint32_t p = px[(size_t)(y * f + j) * (size_t)stride + (size_t)(x * f + i)];
                    r += p >> 16 & 0xFF, g += p >> 8 & 0xFF, b += p & 0xFF;
                }
            uint32_t n = (uint32_t)(f * f);
            d[(size_t)y * (size_t)W + (size_t)x] = 0xFF000000u | (r / n) << 16 | (g / n) << 8 | (b / n);
        }
    *nw = W;
    *nh = H;
    return d;
}

char *base64_encode(const void *data, size_t n, size_t *outlen) {
    static const char tab[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    const uint8_t *s = data;
    size_t len = (n + 2) / 3 * 4;
    char *o = malloc(len + 1), *p = o;
    if (!o) return NULL;
    size_t i = 0;
    for (; i + 2 < n; i += 3) {
        uint32_t v = (uint32_t)s[i] << 16 | (uint32_t)s[i + 1] << 8 | s[i + 2];
        *p++ = tab[v >> 18];
        *p++ = tab[v >> 12 & 63];
        *p++ = tab[v >> 6 & 63];
        *p++ = tab[v & 63];
    }
    if (i < n) {
        uint32_t v = (uint32_t)s[i] << 16 | (i + 1 < n ? (uint32_t)s[i + 1] << 8 : 0);
        *p++ = tab[v >> 18];
        *p++ = tab[v >> 12 & 63];
        *p++ = i + 1 < n ? tab[v >> 6 & 63] : '=';
        *p++ = '=';
    }
    *p = 0;
    if (outlen) *outlen = len;
    return o;
}
