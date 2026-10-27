/* PNG encoder: 8-bit RGB or straight-alpha RGBA. Each row takes the filter whose bytes,
   read as signed, have the smallest sum of magnitudes (as libpng chooses), and the rows
   are DEFLATE-compressed (deflate.h) into one IDAT. */
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include "deflate.h"
#include "png.h"

#define LEVEL 6
#define HEAD (8 + 25 + 21 + 8)  /* the signature, IHDR, pHYs and the IDAT's length and type */
#define TAIL (4 + 12)           /* the IDAT's CRC and IEND */

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

static void be32(uint8_t *b, uint32_t v) {
    b[0] = (uint8_t)(v >> 24);
    b[1] = (uint8_t)(v >> 16);
    b[2] = (uint8_t)(v >> 8);
    b[3] = (uint8_t)v;
}

/* Writes the chunk of n bytes of data (already in place when data is NULL) at at. */
static uint8_t *chunk(uint8_t *at, const char *type, const uint8_t *data, size_t n) {
    be32(at, (uint32_t)n);
    memcpy(at + 4, type, 4);
    if (data && n) memcpy(at + 8, data, n);
    be32(at + 8 + n, crc32_update(0, at + 4, n + 4));
    return at + 12 + n;
}

static size_t bound(int w, int h, unsigned channels) {
    if (w <= 0 || h <= 0 || (size_t)w > (SIZE_MAX - 1) / channels) return 0;
    size_t row = (size_t)w * channels + 1;
    if ((size_t)h > SIZE_MAX / row) return 0;
    size_t z = deflate_bound(row * (size_t)h);
    /* The IDAT length is a 32-bit field. This also bounds the arithmetic. */
    return z && z <= UINT32_MAX ? z + HEAD + TAIL : 0;
}

static unsigned paeth(unsigned a, unsigned b, unsigned c) {
    int p = (int)(a + b) - (int)c, pa = abs(p - (int)a), pb = abs(p - (int)b), pc = abs(p - (int)c);
    return pa <= pb && pa <= pc ? a : pb <= pc ? b : c;
}

/* The row cur (n bytes, bpp a pixel) filtered against prev, the row above, with its
   filter type in front. */
static void filter_row(const uint8_t *cur, const uint8_t *prev, size_t n, unsigned bpp, uint8_t *out) {
    uint32_t sum[5] = {0};
    for (size_t i = 0; i < n; i++) {
        unsigned x = cur[i], a = i >= bpp ? cur[i - bpp] : 0, b = prev[i], c = i >= bpp ? prev[i - bpp] : 0;
        uint8_t v[5] = {(uint8_t)x, (uint8_t)(x - a), (uint8_t)(x - b), (uint8_t)(x - ((a + b) >> 1)), (uint8_t)(x - paeth(a, b, c))};
        for (int f = 0; f < 5; f++) sum[f] += v[f] < 128 ? v[f] : 256u - v[f];
    }
    unsigned best = 0;
    for (unsigned f = 1; f < 5; f++)
        if (sum[f] < sum[best]) best = f;
    out[0] = (uint8_t)best;
    for (size_t i = 0; i < n; i++) {
        unsigned x = cur[i], a = i >= bpp ? cur[i - bpp] : 0, b = prev[i], c = i >= bpp ? prev[i - bpp] : 0;
        unsigned predict = best == 0 ? 0 : best == 1 ? a : best == 2 ? b : best == 3 ? (a + b) >> 1 : paeth(a, b, c);
        out[1 + i] = (uint8_t)(x - predict);
    }
}

static int encode(const uint32_t *px, int w, int h, int stride, bool alpha, uint8_t **out, size_t *outlen) {
    if (!out || !outlen) return -1;
    *out = NULL;
    *outlen = 0;
    unsigned bpp = alpha ? 4 : 3;
    size_t most = bound(w, h, bpp);
    if (!most || !px || stride < w || (size_t)(h - 1) > (SIZE_MAX / 4 - (size_t)w) / (size_t)stride) return -1;
    size_t n = (size_t)w * bpp;
    uint8_t *rows = calloc(3, n + 1);
    struct deflate_stream *z = deflate_begin(LEVEL, DEFLATE_ZLIB);
    /* The output grows from a guess, so a well compressed image never holds its bound. */
    struct deflate_out o = {.cap = most < 65536 ? most : 65536, .grow = true};
    o.p = malloc(o.cap);
    if (!rows || !z || !o.p) goto fail;
    uint8_t *cur = rows, *prev = rows + n + 1, *filtered = rows + 2 * (n + 1);
    memcpy(o.p, "\x89PNG\r\n\x1a\n", 8);
    uint8_t ihdr[13] = {0};
    be32(ihdr, (uint32_t)w);
    be32(ihdr + 4, (uint32_t)h);
    ihdr[8] = 8;
    ihdr[9] = alpha ? 6 : 2; /* 8-bit RGBA, not premultiplied, or RGB */
    uint8_t *at = chunk(o.p + 8, "IHDR", ihdr, 13);
    uint8_t phys[9];
    be32(phys, 3780); /* 96 dpi */
    be32(phys + 4, 3780);
    phys[8] = 1;
    chunk(at, "pHYs", phys, 9);
    o.len = HEAD;
    for (int y = 0; y < h; y++) {
        const uint32_t *s = px + (size_t)y * (size_t)stride;
        uint8_t *c = cur;
        for (int x = 0; x < w; x++) {
            *c++ = (uint8_t)(s[x] >> 16);
            *c++ = (uint8_t)(s[x] >> 8);
            *c++ = (uint8_t)s[x];
            if (alpha) *c++ = (uint8_t)(s[x] >> 24);
        }
        filter_row(cur, prev, n, bpp, filtered);
        if (deflate_feed(z, filtered, n + 1, y == h - 1, &o)) goto fail;
        uint8_t *t = prev;
        prev = cur;
        cur = t;
    }
    size_t data = o.len - HEAD;
    if (o.cap - o.len < TAIL) {
        uint8_t *p = realloc(o.p, o.len + TAIL);
        if (!p) goto fail;
        o.p = p;
    }
    at = chunk(o.p + HEAD - 8, "IDAT", NULL, data);
    chunk(at, "IEND", NULL, 0);
    deflate_end(z);
    free(rows);
    *out = o.p;
    *outlen = o.len + TAIL;
    return 0;
fail:
    deflate_end(z);
    free(rows);
    free(o.p);
    return -1;
}

int png_encode(const uint32_t *px, int w, int h, int stride, uint8_t **out, size_t *outlen) {
    return encode(px, w, h, stride, false, out, outlen);
}

size_t png_encode_rgba_bound(int w, int h) { return bound(w, h, 4); }

int png_encode_rgba(const uint32_t *px, int w, int h, int stride, uint8_t **out, size_t *outlen) {
    return encode(px, w, h, stride, true, out, outlen);
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
