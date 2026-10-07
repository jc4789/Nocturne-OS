#include "media.h"
#include <limits.h>

static int64_t maximum(int64_t a, int64_t b) { return a > b ? a : b; }
static int64_t minimum(int64_t a, int64_t b) { return a < b ? a : b; }
void nmedia_draw(canvas_t *c, const uint32_t *pixels, int sw, int sh, int x, int y, int w, int h) {
    if (!c || !c->px || !pixels || sw <= 0 || sh <= 0 || (uint64_t)sw * sh > NMEDIA_MAX_PIXELS ||
        w <= 0 || h <= 0 || c->w <= 0 || c->h <= 0 || c->pitch < c->w) return;
    int64_t dw = w, dh = (int64_t)sh * w / sw;
    if (dh > h) dh = h, dw = (int64_t)sw * h / sh;
    if (!dw || !dh) return;
    int64_t dx = (int64_t)x + ((int64_t)w - dw) / 2, dy = (int64_t)y + ((int64_t)h - dh) / 2;
    int64_t x0 = maximum(maximum(dx, c->cx0), 0), y0 = maximum(maximum(dy, c->cy0), 0);
    int64_t x1 = minimum(minimum(dx + dw, c->cx1), c->w), y1 = minimum(minimum(dy + dh, c->cy1), c->h);
    for (int64_t yy = y0; yy < y1; yy++) {
        size_t sy = (size_t)((yy - dy) * sh / dh);
        for (int64_t xx = x0; xx < x1; xx++) {
            size_t sx = (size_t)((xx - dx) * sw / dw);
            c->px[(size_t)yy * c->pitch + (size_t)xx] = pixels[sy * sw + sx];
        }
    }
}
