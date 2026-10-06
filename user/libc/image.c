/* Image decoding and scaling. The codecs live in third_party_img.c. */
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include "image.h"
#include "stb_image.h"
#include "jebp.h"
#include "nanosvg.h"
#include "nanosvgrast.h"

static image_t *image_new(int w, int h) {
    if (w <= 0 || h <= 0 || (long)w * h > IMAGE_MAX_PIXELS) return NULL;
    image_t *img = malloc(sizeof *img);
    if (!img) return NULL;
    img->w = w;
    img->h = h;
    img->px = malloc((size_t)w * h * 4);
    if (!img->px) {
        free(img);
        return NULL;
    }
    return img;
}

/* RGBA bytes -> 0xAARRGGBB */
static image_t *from_rgba(const uint8_t *p, int w, int h) {
    image_t *img = image_new(w, h);
    if (!img) return NULL;
    for (long i = 0; i < (long)w * h; i++, p += 4)
        img->px[i] = (uint32_t)p[3] << 24 | (uint32_t)p[0] << 16 | (uint32_t)p[1] << 8 | p[2];
    return img;
}

static bool looks_like_svg(const char *d, size_t n) {
    size_t lim = n < 1024 ? n : 1024;
    for (size_t i = 0; i + 4 <= lim; i++)
        if (d[i] == '<' && (strncmp(d + i, "<svg", 4) == 0 || strncmp(d + i, "<SVG", 4) == 0)) return true;
    return false;
}

/* tw, th: the size to draw at (0: the SVG's own size); the drawing is scaled to fit and centred */
static image_t *decode_svg(const char *data, size_t n, int tw, int th) {
    char *text = malloc(n + 1); /* nanosvg parses in place */
    if (!text) return NULL;
    memcpy(text, data, n);
    text[n] = 0;
    NSVGimage *svg = nsvgParse(text, "px", 96);
    free(text);
    if (!svg) return NULL;
    float w = svg->width > 0 ? svg->width : 300, h = svg->height > 0 ? svg->height : 150;
    float scale = 1, ox = 0, oy = 0;
    int iw, ih;
    if (tw > 0 && th > 0) {
        if (tw > 4096) tw = 4096;
        if (th > 4096) th = 4096;
        float sx = tw / w, sy = th / h;
        scale = sx < sy ? sx : sy;
        ox = (tw - w * scale) / 2;
        oy = (th - h * scale) / 2;
        iw = tw, ih = th;
    } else {
        if (w > 2048 || h > 2048) scale = 2048 / (w > h ? w : h);
        iw = (int)(w * scale + 0.5f), ih = (int)(h * scale + 0.5f);
    }
    image_t *img = NULL;
    NSVGrasterizer *r = nsvgCreateRasterizer();
    uint8_t *rgba = r && iw > 0 && ih > 0 ? calloc((size_t)iw * ih, 4) : NULL;
    if (rgba) {
        nsvgRasterize(r, svg, ox, oy, scale, rgba, iw, ih, iw * 4); /* straight alpha */
        img = from_rgba(rgba, iw, ih);
        free(rgba);
    }
    if (r) nsvgDeleteRasterizer(r);
    nsvgDelete(svg);
    return img;
}

image_t *image_decode_svg(const char *svg, size_t n, int w, int h) {
    if (!svg || !looks_like_svg(svg, n)) return NULL;
    return decode_svg(svg, n, w, h);
}

bool image_is_svg(const void *data, size_t n) { return data && looks_like_svg(data, n); }

image_t *image_decode(const void *data, size_t n) {
    const uint8_t *d = data;
    if (!data || n < 8 || n > 0x7FFFFFFF) return NULL;
    if (n >= 12 && !memcmp(d, "RIFF", 4) && !memcmp(d + 8, "WEBP", 4)) {
        jebp_image_t ji;
        if (jebp_decode_size(&ji, n, data) != JEBP_OK || (long)ji.width * ji.height > IMAGE_MAX_PIXELS) return NULL;
        if (jebp_decode(&ji, n, data) != JEBP_OK) return NULL;
        image_t *img = from_rgba((const uint8_t *)ji.pixels, ji.width, ji.height);
        jebp_free_image(&ji);
        return img;
    }
    int w, h, comp;
    if (stbi_info_from_memory(d, (int)n, &w, &h, &comp)) {
        if ((long)w * h > IMAGE_MAX_PIXELS) return NULL;
        uint8_t *rgba = stbi_load_from_memory(d, (int)n, &w, &h, &comp, 4);
        if (!rgba) return NULL;
        image_t *img = from_rgba(rgba, w, h);
        stbi_image_free(rgba);
        return img;
    }
    if (looks_like_svg((const char *)d, n)) return decode_svg((const char *)d, n, 0, 0);
    return NULL;
}

void image_free(image_t *img) {
    if (!img) return;
    free(img->px);
    free(img);
}

/* average of the source rectangle [x0,x1) x [y0,y1), weighting colour by alpha */
static uint32_t box_avg(const image_t *s, int x0, int y0, int x1, int y1) {
    uint32_t a = 0, r = 0, g = 0, b = 0, n = 0;
    for (int y = y0; y < y1; y++)
        for (int x = x0; x < x1; x++) {
            uint32_t p = s->px[(size_t)y * s->w + x], pa = p >> 24;
            a += pa;
            r += (p >> 16 & 255) * pa;
            g += (p >> 8 & 255) * pa;
            b += (p & 255) * pa;
            n++;
        }
    if (!n || !a) return 0;
    return (a / n) << 24 | (r / a) << 16 | (g / a) << 8 | (b / a);
}

image_t *image_scale(const image_t *s, int w, int h) {
    image_t *d = image_new(w, h);
    if (!d) return NULL;
    for (int y = 0; y < h; y++) {
        for (int x = 0; x < w; x++) {
            uint32_t out;
            if (w <= s->w && h <= s->h) {
                int x0 = (int)((long)x * s->w / w), x1 = (int)((long)(x + 1) * s->w / w);
                int y0 = (int)((long)y * s->h / h), y1 = (int)((long)(y + 1) * s->h / h);
                if (x1 <= x0) x1 = x0 + 1;
                if (y1 <= y0) y1 = y0 + 1;
                /* very large reductions: sample at most 8x8 source pixels */
                if (x1 - x0 > 8) x0 += (x1 - x0 - 8) / 2, x1 = x0 + 8;
                if (y1 - y0 > 8) y0 += (y1 - y0 - 8) / 2, y1 = y0 + 8;
                out = box_avg(s, x0, y0, x1, y1);
            } else {
                /* bilinear */
                float fx = (x + 0.5f) * s->w / w - 0.5f, fy = (y + 0.5f) * s->h / h - 0.5f;
                if (fx < 0) fx = 0;
                if (fy < 0) fy = 0;
                int ix = (int)fx, iy = (int)fy;
                int ix1 = ix + 1 < s->w ? ix + 1 : ix, iy1 = iy + 1 < s->h ? iy + 1 : iy;
                int tx = (int)((fx - ix) * 256), ty = (int)((fy - iy) * 256);
                uint32_t p00 = s->px[(size_t)iy * s->w + ix], p01 = s->px[(size_t)iy * s->w + ix1];
                uint32_t p10 = s->px[(size_t)iy1 * s->w + ix], p11 = s->px[(size_t)iy1 * s->w + ix1];
                out = 0;
                for (int sh = 0; sh < 32; sh += 8) {
                    int c0 = (int)(p00 >> sh & 255) * (256 - tx) + (int)(p01 >> sh & 255) * tx;
                    int c1 = (int)(p10 >> sh & 255) * (256 - tx) + (int)(p11 >> sh & 255) * tx;
                    out |= (uint32_t)((c0 * (256 - ty) + c1 * ty) >> 16) << sh;
                }
            }
            d->px[(size_t)y * w + x] = out;
        }
    }
    return d;
}

void image_draw(canvas_t *c, const image_t *img, int x, int y) {
    int y0 = y < c->cy0 ? c->cy0 : y, y1 = y + img->h > c->cy1 ? c->cy1 : y + img->h;
    int x0 = x < c->cx0 ? c->cx0 : x, x1 = x + img->w > c->cx1 ? c->cx1 : x + img->w;
    for (int py = y0; py < y1; py++) {
        const uint32_t *src = img->px + (size_t)(py - y) * img->w - x;
        uint32_t *dst = c->px + (size_t)py * c->pitch;
        for (int px = x0; px < x1; px++) {
            uint32_t p = src[px], a = p >> 24;
            if (a == 255) dst[px] = p;
            else if (a) dst[px] = gfx_mix(dst[px], p, (int)a);
        }
    }
}
