/* Fuzzes downloaded-font handling: user/libc/font.c font_open_memory (EOT wrapper, native TrueType
   gate in font_sfnt.h, WOFF/WOFF2/CFF via the FreeType subset) followed by glyph measurement and
   rasterization (stb_truetype, composite renderer, GSUB emoji ligatures, COLR). */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <nocturne.h>
#include <font.h>

n_result wait_on_address(volatile uint32_t *p, uint32_t value, unsigned timeout) { return 0; }
n_result wake_address(volatile uint32_t *p, unsigned count) { return 0; }
int gfx_utf8_decode(const char *p, uint32_t *cp) {
    const unsigned char *s = (const unsigned char *)p;
    if (s[0] < 0x80) { *cp = s[0]; return 1; }
    if ((s[0] & 0xe0) == 0xc0 && (s[1] & 0xc0) == 0x80) { *cp = (s[0] & 0x1f) << 6 | (s[1] & 0x3f); return 2; }
    if ((s[0] & 0xf0) == 0xe0 && (s[1] & 0xc0) == 0x80 && (s[2] & 0xc0) == 0x80) { *cp = (s[0] & 15) << 12 | (s[1] & 0x3f) << 6 | (s[2] & 0x3f); return 3; }
    if ((s[0] & 0xf8) == 0xf0 && (s[1] & 0xc0) == 0x80 && (s[2] & 0xc0) == 0x80 && (s[3] & 0xc0) == 0x80) {
        *cp = (s[0] & 7) << 18 | (s[1] & 0x3f) << 12 | (s[2] & 0x3f) << 6 | (s[3] & 0x3f); return 4; }
    *cp = 0xfffd; return 1;
}
void gfx_fill(canvas_t *c, int x, int y, int w, int h, uint32_t color) {
    for (int j = y; j < y + h; j++) for (int i = x; i < x + w; i++)
        if (i >= c->cx0 && i < c->cx1 && j >= c->cy0 && j < c->cy1) c->px[(size_t)j * c->pitch + i] = color;
}
void gfx_rect(canvas_t *c, int x, int y, int w, int h, uint32_t color) {
    gfx_fill(c, x, y, w, 1, color); gfx_fill(c, x, y + h - 1, w, 1, color);
    gfx_fill(c, x, y, 1, h, color); gfx_fill(c, x + w - 1, y, 1, h, color);
}

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
    if (size < 4) return 0;
    font_t *face = font_open_memory(data, size);
    if (!face) return 0;
    static const char text[] = "AWi\xe3\x81\x82\xe6\xbc\xa2 \xf0\x9f\x91\xa8\xe2\x80\x8d\xf0\x9f\x91\xa9\xe2\x80\x8d\xf0\x9f\x91\xa7 \xf0\x9f\x87\xaf\xf0\x9f\x87\xb5\xea\xb0\x80\xcf\x80\xc3\xa9\xef\xac\x81";
    static uint32_t pixels[256 * 64];
    canvas_t c = {.px = pixels, .w = 256, .h = 64, .pitch = 256, .cx0 = 0, .cy0 = 0, .cx1 = 256, .cy1 = 64};
    static const float sizes[] = {9, 16, 37.5f};
    for (unsigned i = 0; i < 3; i++) {
        float ascent, descent, gap;
        font_metrics(face, sizes[i], &ascent, &descent, &gap);
        font_width(face, sizes[i], text, sizeof text - 1);
        font_draw(&c, face, sizes[i], 2, 40, text, sizeof text - 1, 0xffffffffu);
    }
    font_close(face);
    return 0;
}

/* Only reached when canvas_t.mask_clip is set; the harness canvas never sets it. */
int gfx_clip_row(const canvas_t *c, int y, int x0, int x1, struct gfx_clip_span *out) { __builtin_trap(); }
void gfx_blend_pixel(canvas_t *c, int x, int y, uint32_t argb) { __builtin_trap(); }
