/* Nocturne shared 2D graphics library. Freestanding: only needs memcpy. */
#include "gfx.h"
#include <stddef.h>
#include "font8x16.h"
#include "font16x32.h"

void *memcpy(void *d, const void *s, size_t n);

void gfx_init(canvas_t *c, uint32_t *px, int w, int h, int pitch) {
    c->px = px;
    c->w = w;
    c->h = h;
    c->pitch = pitch;
    gfx_noclip(c);
}

void gfx_noclip(canvas_t *c) {
    c->cx0 = 0;
    c->cy0 = 0;
    c->cx1 = c->w;
    c->cy1 = c->h;
}

void gfx_clip(canvas_t *c, int x, int y, int w, int h) {
    if (x > c->cx0) c->cx0 = x;
    if (y > c->cy0) c->cy0 = y;
    if (x + w < c->cx1) c->cx1 = x + w;
    if (y + h < c->cy1) c->cy1 = y + h;
    if (c->cx1 < c->cx0) c->cx1 = c->cx0;
    if (c->cy1 < c->cy0) c->cy1 = c->cy0;
}

static inline int clip_rect(const canvas_t *c, int *x, int *y, int *w, int *h) {
    int x0 = *x, y0 = *y, x1 = *x + *w, y1 = *y + *h;
    if (x0 < c->cx0) x0 = c->cx0;
    if (y0 < c->cy0) y0 = c->cy0;
    if (x1 > c->cx1) x1 = c->cx1;
    if (y1 > c->cy1) y1 = c->cy1;
    if (x1 <= x0 || y1 <= y0) return 0;
    *x = x0; *y = y0; *w = x1 - x0; *h = y1 - y0;
    return 1;
}

void gfx_pixel(canvas_t *c, int x, int y, uint32_t col) {
    if (x < c->cx0 || y < c->cy0 || x >= c->cx1 || y >= c->cy1) return;
    c->px[y * c->pitch + x] = col;
}

void gfx_blend_pixel(canvas_t *c, int x, int y, uint32_t argb) {
    if (x < c->cx0 || y < c->cy0 || x >= c->cx1 || y >= c->cy1) return;
    uint32_t a = argb >> 24;
    uint32_t *p = &c->px[y * c->pitch + x];
    if (a == 255) *p = argb;
    else if (a) *p = gfx_mix(*p, argb, a);
}

void gfx_fill(canvas_t *c, int x, int y, int w, int h, uint32_t col) {
    if (!clip_rect(c, &x, &y, &w, &h)) return;
    for (int j = 0; j < h; j++) {
        uint32_t *row = &c->px[(y + j) * c->pitch + x];
        for (int i = 0; i < w; i++) row[i] = col;
    }
}

void gfx_fill_blend(canvas_t *c, int x, int y, int w, int h, uint32_t argb) {
    int a = argb >> 24;
    if (a == 255) { gfx_fill(c, x, y, w, h, argb); return; }
    if (a == 0) return;
    if (!clip_rect(c, &x, &y, &w, &h)) return;
    for (int j = 0; j < h; j++) {
        uint32_t *row = &c->px[(y + j) * c->pitch + x];
        for (int i = 0; i < w; i++) row[i] = gfx_mix(row[i], argb, a);
    }
}

void gfx_hline(canvas_t *c, int x, int y, int w, uint32_t col) { gfx_fill(c, x, y, w, 1, col); }
void gfx_vline(canvas_t *c, int x, int y, int h, uint32_t col) { gfx_fill(c, x, y, 1, h, col); }

void gfx_rect(canvas_t *c, int x, int y, int w, int h, uint32_t col) {
    gfx_hline(c, x, y, w, col);
    gfx_hline(c, x, y + h - 1, w, col);
    gfx_vline(c, x, y, h, col);
    gfx_vline(c, x + w - 1, y, h, col);
}

void gfx_line(canvas_t *c, int x0, int y0, int x1, int y1, uint32_t col) {
    int dx = x1 > x0 ? x1 - x0 : x0 - x1;
    int dy = y1 > y0 ? y0 - y1 : y1 - y0;
    int sx = x0 < x1 ? 1 : -1, sy = y0 < y1 ? 1 : -1;
    int err = dx + dy;
    for (;;) {
        gfx_pixel(c, x0, y0, col);
        if (x0 == x1 && y0 == y1) break;
        int e2 = 2 * err;
        if (e2 >= dy) { err += dy; x0 += sx; }
        if (e2 <= dx) { err += dx; y0 += sy; }
    }
}

void gfx_circle(canvas_t *c, int cx, int cy, int r, uint32_t col) {
    int x = r, y = 0, err = 1 - r;
    while (x >= y) {
        gfx_pixel(c, cx + x, cy + y, col); gfx_pixel(c, cx + y, cy + x, col);
        gfx_pixel(c, cx - y, cy + x, col); gfx_pixel(c, cx - x, cy + y, col);
        gfx_pixel(c, cx - x, cy - y, col); gfx_pixel(c, cx - y, cy - x, col);
        gfx_pixel(c, cx + y, cy - x, col); gfx_pixel(c, cx + x, cy - y, col);
        y++;
        if (err < 0) err += 2 * y + 1;
        else { x--; err += 2 * (y - x) + 1; }
    }
}

void gfx_fill_circle(canvas_t *c, int cx, int cy, int r, uint32_t col) {
    for (int y = -r; y <= r; y++) {
        int w = 0;
        while ((w + 1) * (w + 1) + y * y <= r * r) w++;
        gfx_hline(c, cx - w, cy + y, 2 * w + 1, col);
    }
}

void gfx_fill_round(canvas_t *c, int x, int y, int w, int h, int r, uint32_t col) {
    if (r * 2 > h) r = h / 2;
    if (r * 2 > w) r = w / 2;
    gfx_fill(c, x, y + r, w, h - 2 * r, col);
    for (int j = 0; j < r; j++) {
        int dy = r - j;
        int dx = 0;
        while ((dx + 1) * (dx + 1) + (dy - 1) * (dy - 1) < r * r) dx++;
        int inset = r - dx;
        gfx_hline(c, x + inset, y + j, w - 2 * inset, col);
        gfx_hline(c, x + inset, y + h - 1 - j, w - 2 * inset, col);
    }
}

void gfx_gradient_v(canvas_t *c, int x, int y, int w, int h, uint32_t top, uint32_t bottom) {
    for (int j = 0; j < h; j++) {
        int t = h > 1 ? j * 255 / (h - 1) : 0;
        gfx_hline(c, x, y + j, w, gfx_mix(top, bottom, t));
    }
}

void gfx_blit(canvas_t *dst, int dx, int dy, const canvas_t *src, int sx, int sy, int w, int h) {
    int ox = dx, oy = dy;
    if (!clip_rect(dst, &dx, &dy, &w, &h)) return;
    sx += dx - ox;
    sy += dy - oy;
    if (sx < 0) { dx -= sx; w += sx; sx = 0; }
    if (sy < 0) { dy -= sy; h += sy; sy = 0; }
    if (sx + w > src->w) w = src->w - sx;
    if (sy + h > src->h) h = src->h - sy;
    if (w <= 0 || h <= 0) return;
    for (int j = 0; j < h; j++) {
        memcpy(&dst->px[(dy + j) * dst->pitch + dx], &src->px[(sy + j) * src->pitch + sx], (size_t)w * 4);
    }
}

void gfx_blit_alpha(canvas_t *dst, int dx, int dy, const canvas_t *src, int sx, int sy, int w, int h) {
    int ox = dx, oy = dy;
    if (!clip_rect(dst, &dx, &dy, &w, &h)) return;
    sx += dx - ox;
    sy += dy - oy;
    if (sx < 0) { dx -= sx; w += sx; sx = 0; }
    if (sy < 0) { dy -= sy; h += sy; sy = 0; }
    if (sx + w > src->w) w = src->w - sx;
    if (sy + h > src->h) h = src->h - sy;
    for (int j = 0; j < h; j++) {
        uint32_t *d = &dst->px[(dy + j) * dst->pitch + dx];
        const uint32_t *s = &src->px[(sy + j) * src->pitch + sx];
        for (int i = 0; i < w; i++) {
            uint32_t a = s[i] >> 24;
            if (a == 255) d[i] = s[i];
            else if (a) d[i] = gfx_mix(d[i], s[i], a);
        }
    }
}

void gfx_triangle(canvas_t *c, int x0, int y0, int x1, int y1, int x2, int y2, uint32_t col) {
    /* sort by y */
    int t;
    if (y1 < y0) { t = y0; y0 = y1; y1 = t; t = x0; x0 = x1; x1 = t; }
    if (y2 < y0) { t = y0; y0 = y2; y2 = t; t = x0; x0 = x2; x2 = t; }
    if (y2 < y1) { t = y1; y1 = y2; y2 = t; t = x1; x1 = x2; x2 = t; }
    if (y2 == y0) return;
    for (int y = y0; y <= y2; y++) {
        int xa = x0 + (int)((int64_t)(x2 - x0) * (y - y0) / (y2 - y0));
        int xb;
        if (y < y1) xb = y1 == y0 ? x1 : x0 + (int)((int64_t)(x1 - x0) * (y - y0) / (y1 - y0));
        else xb = y2 == y1 ? x1 : x1 + (int)((int64_t)(x2 - x1) * (y - y1) / (y2 - y1));
        if (xa > xb) { t = xa; xa = xb; xb = t; }
        gfx_hline(c, xa, y, xb - xa + 1, col);
    }
}

/* ---- text ---- */

int gfx_font_w(int font) { return font == FONT_LARGE ? FONT16X32_W : FONT8X16_W; }
int gfx_font_h(int font) { return font == FONT_LARGE ? FONT16X32_H : FONT8X16_H; }

uint8_t gfx_glyph_for(uint32_t cp) {
    if (cp < 256 && !(cp < 32 || (cp >= 127 && cp < 160))) return (uint8_t)cp;
    int lo = 0, hi = FONT8X16_UMAP_COUNT - 1;
    while (lo <= hi) {
        int mid = (lo + hi) / 2;
        if (font8x16_umap[mid][0] == cp) return (uint8_t)font8x16_umap[mid][1];
        if (font8x16_umap[mid][0] < cp) lo = mid + 1;
        else hi = mid - 1;
    }
    /* typographic punctuation (common in AI and web text) the font lacks: nearest ASCII */
    switch (cp) {
    case 0x2010: case 0x2011: case 0x2012: case 0x2013: case 0x2014: case 0x2015: case 0x2212:
        return '-';
    case 0x2018: case 0x2019: case 0x201A: case 0x2032: return '\'';
    case 0x201C: case 0x201D: case 0x201E: case 0x2033: return '"';
    case 0x2026: case 0x22EF: return '.';
    case 0x2039: return '<';
    case 0x203A: return '>';
    case 0x2713: case 0x2714: case 0x2705: return 'v';
    case 0x2717: case 0x2718: case 0x274C: return 'x';
    case 0x2002: case 0x2003: case 0x2009: case 0x200A: case 0x202F: return ' ';
    case 0x200B: case 0x200C: case 0x200D: case 0xFEFF: return ' ';
    }
    return '?';
}

int gfx_utf8_decode(const char *s, uint32_t *cp) {
    const uint8_t *u = (const uint8_t *)s;
    if (u[0] < 0x80) { *cp = u[0]; return 1; }
    if ((u[0] & 0xE0) == 0xC0 && (u[1] & 0xC0) == 0x80) {
        *cp = ((u[0] & 0x1F) << 6) | (u[1] & 0x3F);
        return 2;
    }
    if ((u[0] & 0xF0) == 0xE0 && (u[1] & 0xC0) == 0x80 && (u[2] & 0xC0) == 0x80) {
        *cp = ((u[0] & 0x0F) << 12) | ((u[1] & 0x3F) << 6) | (u[2] & 0x3F);
        return 3;
    }
    if ((u[0] & 0xF8) == 0xF0 && (u[1] & 0xC0) == 0x80 && (u[2] & 0xC0) == 0x80 && (u[3] & 0xC0) == 0x80) {
        *cp = ((u[0] & 0x07) << 18) | ((u[1] & 0x3F) << 12) | ((u[2] & 0x3F) << 6) | (u[3] & 0x3F);
        return 4;
    }
    *cp = 0xFFFD;
    return 1;
}

int gfx_char(canvas_t *c, int x, int y, uint8_t glyph, uint32_t fg, uint32_t bg, int font) {
    int fw = gfx_font_w(font), fh = gfx_font_h(font);
    if (x >= c->cx1 || y >= c->cy1 || x + fw <= c->cx0 || y + fh <= c->cy0) return fw;
    int bgop = (bg >> 24) != 0;
    if (font == FONT_LARGE) {
        const uint8_t *g = font16x32_data[glyph];
        for (int j = 0; j < fh; j++) {
            int yy = y + j;
            if (yy < c->cy0 || yy >= c->cy1) continue;
            uint16_t bits = (uint16_t)((g[j * 2] << 8) | g[j * 2 + 1]);
            uint32_t *row = &c->px[yy * c->pitch];
            for (int i = 0; i < fw; i++) {
                int xx = x + i;
                if (xx < c->cx0 || xx >= c->cx1) continue;
                if (bits & (0x8000 >> i)) row[xx] = fg;
                else if (bgop) row[xx] = bg;
            }
        }
    } else {
        const uint8_t *g = font8x16_data[glyph];
        for (int j = 0; j < fh; j++) {
            int yy = y + j;
            if (yy < c->cy0 || yy >= c->cy1) continue;
            uint8_t bits = g[j];
            uint32_t *row = &c->px[yy * c->pitch];
            for (int i = 0; i < fw; i++) {
                int xx = x + i;
                if (xx < c->cx0 || xx >= c->cx1) continue;
                if (bits & (0x80 >> i)) row[xx] = fg;
                else if (bgop) row[xx] = bg;
            }
        }
    }
    return fw;
}

int gfx_text(canvas_t *c, int x, int y, const char *s, uint32_t fg, uint32_t bg, int font) {
    int x0 = x;
    while (*s) {
        uint32_t cp;
        s += gfx_utf8_decode(s, &cp);
        if (cp == '\n') {
            x = x0;
            y += gfx_font_h(font);
            continue;
        }
        x += gfx_char(c, x, y, gfx_glyph_for(cp), fg, bg, font);
    }
    return x - x0;
}

int gfx_text_width(const char *s, int font) {
    int n = 0;
    while (*s) {
        uint32_t cp;
        s += gfx_utf8_decode(s, &cp);
        n++;
    }
    return n * gfx_font_w(font);
}
