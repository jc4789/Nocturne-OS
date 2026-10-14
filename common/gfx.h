/* Nocturne shared 2D graphics library (used by kernel compositor and user apps). */
#pragma once
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif

/* Immutable during a raster job. Stack links are borrowed for the draw call;
   alpha storage belongs to the caller's retained geometry cache. */
struct gfx_mask_span { int x0,x1; }; /* retained all-255 interval, half-open */
struct gfx_clip_span { int x0,x1,opaque0,opaque1; };
struct gfx_mask_clip {
    const struct gfx_mask_clip *next;
    const uint8_t *alpha;
    int x,y,w,h,pitch;
    const struct gfx_mask_span *opaque; /* optional per-row proof, same owner as alpha */
};
typedef struct {
    uint32_t *px;
    int w, h, pitch;                 /* pitch in pixels */
    int cx0, cy0, cx1, cy1;          /* clip rectangle [cx0,cx1) x [cy0,cy1) */
    const struct gfx_mask_clip *mask_clip;
} canvas_t;
uint8_t gfx_clip_alpha(const canvas_t *,int,int);
int gfx_clip_row(const canvas_t *,int y,int x0,int x1,struct gfx_clip_span *);

#define RGB(r, g, b)     (0xFF000000u | ((uint32_t)(r) << 16) | ((uint32_t)(g) << 8) | (uint32_t)(b))
#define ARGB(a, r, g, b) (((uint32_t)(a) << 24) | ((uint32_t)(r) << 16) | ((uint32_t)(g) << 8) | (uint32_t)(b))
#define TRANSPARENT 0x00000000u

enum { FONT_SMALL = 0, FONT_LARGE = 1 };

void gfx_init(canvas_t *c, uint32_t *px, int w, int h, int pitch);
void gfx_clip(canvas_t *c, int x, int y, int w, int h);  /* intersect clip with rect */
void gfx_noclip(canvas_t *c);
void gfx_pixel(canvas_t *c, int x, int y, uint32_t col);
void gfx_blend_pixel(canvas_t *c, int x, int y, uint32_t argb);
void gfx_fill(canvas_t *c, int x, int y, int w, int h, uint32_t col);
void gfx_fill_blend(canvas_t *c, int x, int y, int w, int h, uint32_t argb);
void gfx_rect(canvas_t *c, int x, int y, int w, int h, uint32_t col);
void gfx_hline(canvas_t *c, int x, int y, int w, uint32_t col);
void gfx_vline(canvas_t *c, int x, int y, int h, uint32_t col);
void gfx_line(canvas_t *c, int x0, int y0, int x1, int y1, uint32_t col);
void gfx_circle(canvas_t *c, int cx, int cy, int r, uint32_t col);
void gfx_fill_circle(canvas_t *c, int cx, int cy, int r, uint32_t col);
void gfx_fill_round(canvas_t *c, int x, int y, int w, int h, int r, uint32_t col);
void gfx_gradient_v(canvas_t *c, int x, int y, int w, int h, uint32_t top, uint32_t bottom);
void gfx_blit(canvas_t *dst, int dx, int dy, const canvas_t *src, int sx, int sy, int w, int h);
void gfx_blit_alpha(canvas_t *dst, int dx, int dy, const canvas_t *src, int sx, int sy, int w, int h);
void gfx_triangle(canvas_t *c, int x0, int y0, int x1, int y1, int x2, int y2, uint32_t col);

int gfx_char(canvas_t *c, int x, int y, uint32_t codepoint, uint32_t fg, uint32_t bg, int font);
int gfx_text(canvas_t *c, int x, int y, const char *utf8, uint32_t fg, uint32_t bg, int font);
int gfx_text_width(const char *utf8, int font);
int gfx_font_w(int font);
int gfx_font_h(int font);
/* Width of a Unicode glyph, including full-width cells and fallback. */
int gfx_codepoint_width(uint32_t codepoint, int font);
uint32_t gfx_glyph_for(uint32_t codepoint);
/* decode one UTF-8 sequence; returns bytes consumed (>=1) */
int gfx_utf8_decode(const char *s, uint32_t *cp);

static inline uint32_t gfx_mix(uint32_t a, uint32_t b, int t /* 0..255 weight of b */) {
    /* 8-bit RGB lanes, weights summing to 255. R/B have independent 16-bit
       products: each is <=255*255+127, so neither the rounding correction nor
       the shift can carry into its neighbour. For n in [0, 255*255+127],
       floor(n/255) == (n + 1 + (n >> 8)) >> 8. This preserves the previous
       rounded /255 result exactly; /256 blending would darken equal colors. */
    uint32_t w = (uint32_t)t, inv = 255 - w;
    const uint32_t rb_mask = 0x00FF00FFu;
    uint32_t rb = (a & rb_mask) * inv + (b & rb_mask) * w + 0x007F007Fu;
    rb = (rb + 0x00010001u + ((rb >> 8) & rb_mask)) >> 8;
    uint32_t g = ((a >> 8) & 255) * inv + ((b >> 8) & 255) * w + 127;
    g = (g + 1 + (g >> 8)) >> 8;
    return 0xFF000000u | (rb & rb_mask) | (g << 8);
}
#ifdef __cplusplus
}
#endif
