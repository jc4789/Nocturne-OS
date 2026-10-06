/* TrueType text with stb_truetype (compiled in third_party_ttf.c). Glyphs are rasterized once per
   (font, size, glyph, quarter-pixel x offset) and kept in a cache. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include "font.h"
#include "stb_truetype.h"

struct font {
    stbtt_fontinfo info;
    unsigned char *data;
    int id;
    int ascent, descent, line_gap; /* font units; descent is negative */
    float unit_scale;              /* pixels per font unit at 1 px/em */
    int16_t *adv;                  /* advance width per glyph in font units, INT16_MIN = not looked up */
    uint16_t lo_glyph[0x250];      /* glyph index for code points below 0x250, 0xFFFF = not looked up */
    bool ui_fallback;             /* only the default family uses the bundled CN fallback */
};

static int next_font_id = 1;

font_t *font_open(const char *path) {
    FILE *fp = fopen(path, "rb");
    if (!fp) return NULL;
    fseek(fp, 0, SEEK_END);
    long n = ftell(fp);
    fseek(fp, 0, SEEK_SET);
    unsigned char *data = n > 0 ? malloc((size_t)n) : NULL;
    if (!data || fread(data, 1, (size_t)n, fp) != (size_t)n) {
        fclose(fp);
        free(data);
        return NULL;
    }
    fclose(fp);
    font_t *f = calloc(1, sizeof *f);
    if (!f || !stbtt_InitFont(&f->info, data, stbtt_GetFontOffsetForIndex(data, 0))) {
        free(f);
        free(data);
        return NULL;
    }
    f->data = data;
    f->id = next_font_id++;
    stbtt_GetFontVMetrics(&f->info, &f->ascent, &f->descent, &f->line_gap);
    f->unit_scale = stbtt_ScaleForMappingEmToPixels(&f->info, 1.0f);
    f->adv = malloc(sizeof(int16_t) * (size_t)(f->info.numGlyphs > 0 ? f->info.numGlyphs : 1));
    if (!f->adv) { free(f->data); free(f); return NULL; }
    for (int i = 0; i < f->info.numGlyphs; i++) f->adv[i] = INT16_MIN;
    memset(f->lo_glyph, 0xFF, sizeof f->lo_glyph);
    return f;
}

static font_t *open_bundled(const char *name) {
    const char *dir = getenv("FONT_DIR");
    char path[256];
    snprintf(path, sizeof path, "%s/%s", dir && *dir ? dir : "/usr/share/fonts", name);
    return font_open(path);
}

font_t *font_ui(int style) {
    static font_t *cache[4];
    static bool tried[4];
    style &= 3;
    if (!tried[style]) {
        static const char *names[4] = {"MapleMono-NF-Regular.ttf", "MapleMono-NF-Bold.ttf",
                                      "MapleMono-NF-Italic.ttf", "MapleMono-NF-BoldItalic.ttf"};
        cache[style] = open_bundled(names[style]);
        if (cache[style]) cache[style]->ui_fallback = true;
        tried[style] = true;
        if (!cache[style] && style) cache[style] = font_ui(FONT_REGULAR); /* a missing style falls back */
    }
    return cache[style];
}

static int glyph_of(font_t *f, uint32_t cp) {
    if (cp < 0x250) {
        if (f->lo_glyph[cp] == 0xFFFF) f->lo_glyph[cp] = (uint16_t)stbtt_FindGlyphIndex(&f->info, (int)cp);
        return f->lo_glyph[cp];
    }
    return stbtt_FindGlyphIndex(&f->info, (int)cp);
}

/* Measurement and rasterization must resolve exactly the same face. Keep the
   20 MB CN face shared and lazy instead of duplicating it for every style.
   font_open() remains a single-face API; this is not downloaded webfont support. */
static font_t *glyph_face(font_t *f, uint32_t cp, int *glyph) {
    *glyph = glyph_of(f, cp);
    if (!*glyph && f->ui_fallback && cp > ' ') {
        static font_t *cn;
        static bool tried;
        if (!tried) { tried = true; cn = open_bundled("MapleMono-NF-CN-Regular.ttf"); }
        if (cn) {
            int g = glyph_of(cn, cp);
            if (g) { *glyph = g; return cn; }
        }
    }
    return f;
}

static int glyph_adv(font_t *f, int g) {
    if (g < 0 || g >= f->info.numGlyphs) return 0;
    if (f->adv[g] == INT16_MIN) {
        int a, lsb;
        stbtt_GetGlyphHMetrics(&f->info, g, &a, &lsb);
        f->adv[g] = (int16_t)a;
    }
    return f->adv[g];
}

void font_metrics(font_t *f, float px, float *ascent, float *descent, float *line_gap) {
    float s = f->unit_scale * px;
    if (ascent) *ascent = f->ascent * s;
    if (descent) *descent = -f->descent * s;
    if (line_gap) *line_gap = f->line_gap * s;
}

bool font_has(font_t *f, uint32_t cp) { int g; glyph_face(f, cp, &g); return g != 0; }

/* the advance of a missing glyph: an empty box */
static float tofu_adv(float px) { return px * 0.6f; }

float font_advance(font_t *f, float px, uint32_t cp) {
    int g;
    f = glyph_face(f, cp, &g);
    if (!g && cp > ' ') return tofu_adv(px);
    return glyph_adv(f, g) * f->unit_scale * px;
}

float font_width(font_t *f, float px, const char *s, size_t n) {
    float w = 0;
    const char *end = s + n;
    while (s < end) {
        uint32_t cp;
        s += gfx_utf8_decode(s, &cp);
        w += font_advance(f, px, cp);
    }
    return w;
}

/* ---- glyph cache ---- */
#define GC_SIZE 8192 /* slots, a power of two */
#define GC_MAX  6000 /* flush everything beyond this many glyphs */

struct gent {
    int font;
    int glyph;
    uint16_t size4; /* px * 4 */
    uint8_t phase;  /* x offset in quarter pixels */
    uint8_t used;
    int16_t x0, y0;
    uint16_t w, h;
    uint8_t *bmp;
};

static struct gent *gc;
static int gc_count;
static uint8_t gamma_tab[256];

static void gc_flush(void) {
    for (int i = 0; i < GC_SIZE; i++)
        if (gc[i].used) free(gc[i].bmp);
    memset(gc, 0, sizeof(struct gent) * GC_SIZE);
    gc_count = 0;
}

static struct gent *gc_get(font_t *f, int g, float px, int phase) {
    if (!gc) {
        gc = calloc(GC_SIZE, sizeof(struct gent));
        if (!gc) return NULL;
        /* coverage -> alpha: a little heavier than linear, so text does not look washed out */
        for (int i = 0; i < 256; i++) gamma_tab[i] = (uint8_t)(255.0 * pow(i / 255.0, 0.78) + 0.5);
    }
    uint16_t size4 = (uint16_t)(px * 4 + 0.5f);
    uint32_t h = (uint32_t)f->id * 2654435761u ^ (uint32_t)g * 40503u ^ (uint32_t)size4 * 977u ^ (uint32_t)phase * 7u;
    for (uint32_t i = 0;; i++) {
        struct gent *e = &gc[(h + i) & (GC_SIZE - 1)];
        if (!e->used) {
            if (gc_count >= GC_MAX) {
                gc_flush();
                return gc_get(f, g, px, phase);
            }
            float scale = f->unit_scale * size4 / 4.0f, shift = phase / 4.0f;
            int x0, y0, x1, y1;
            stbtt_GetGlyphBitmapBoxSubpixel(&f->info, g, scale, scale, shift, 0, &x0, &y0, &x1, &y1);
            e->font = f->id;
            e->glyph = g;
            e->size4 = size4;
            e->phase = (uint8_t)phase;
            e->used = 1;
            e->x0 = (int16_t)x0;
            e->y0 = (int16_t)y0;
            e->w = (uint16_t)(x1 > x0 ? x1 - x0 : 0);
            e->h = (uint16_t)(y1 > y0 ? y1 - y0 : 0);
            e->bmp = NULL;
            if (e->w && e->h && (e->bmp = malloc((size_t)e->w * e->h)))
                stbtt_MakeGlyphBitmapSubpixel(&f->info, e->bmp, e->w, e->h, e->w, scale, scale, shift, 0, g);
            gc_count++;
            return e;
        }
        if (e->font == f->id && e->glyph == g && e->size4 == size4 && e->phase == phase) return e;
    }
}

static void blit_glyph(canvas_t *c, const struct gent *e, int x, int y, uint32_t color) {
    if (!e->bmp) return;
    int alpha = color >> 24;
    for (int j = 0; j < e->h; j++) {
        int py = y + e->y0 + j;
        if (py < c->cy0 || py >= c->cy1) continue;
        const uint8_t *row = e->bmp + (size_t)j * e->w;
        uint32_t *dst = c->px + (size_t)py * c->pitch;
        for (int i = 0; i < e->w; i++) {
            int px = x + e->x0 + i;
            if (!row[i] || px < c->cx0 || px >= c->cx1) continue;
            int a = gamma_tab[row[i]] * alpha / 255;
            dst[px] = a >= 255 ? (color | 0xFF000000u) : gfx_mix(dst[px], color, a);
        }
    }
}

float font_draw(canvas_t *c, font_t *f, float px, float x, int y, const char *s, size_t n, uint32_t color) {
    const char *end = s + n;
    while (s < end) {
        uint32_t cp;
        s += gfx_utf8_decode(s, &cp);
        int g;
        font_t *face = glyph_face(f, cp, &g);
        if (!g) {
            if (cp > ' ') {
                float a, d;
                font_metrics(f, px, &a, &d, NULL);
                int bw = (int)(px * 0.5f), bh = (int)(a * 0.75f);
                gfx_rect(c, (int)(x + px * 0.05f), y - bh, bw, bh, color | 0xFF000000u);
                x += tofu_adv(px);
            }
            continue;
        }
        if (cp > ' ') {
            float fx = floorf(x);
            int phase = (int)((x - fx) * 4) & 3;
            struct gent *e = gc_get(face, g, px, phase);
            if (e) blit_glyph(c, e, (int)fx, y, color);
        }
        x += glyph_adv(face, g) * face->unit_scale * px;
    }
    return x;
}

/* Existing application chrome uses 8x16 / 16x32 cells. Preserve that geometry
   and its ABI, but draw the default TrueType face rather than changing the OS.
   Wide bundled glyphs occupy two cells. The kernel retains its bitmap renderer. */
int font_cell_width(uint32_t cp, int size) {
    font_t *f = font_ui(FONT_REGULAR);
    if (!f) return -1;
    float a = font_advance(f, 10, cp);
    int cells = a <= 0 ? 0 : a > 9 ? 2 : 1;
    return cells * (size == FONT_LARGE ? 16 : 8);
}

int font_cell_draw(canvas_t *c, int x, int y, uint32_t cp, uint32_t fg, uint32_t bg, int size) {
    font_t *f = font_ui(FONT_REGULAR);
    if (!f) return -1;
    int scale = size == FONT_LARGE ? 2 : 1, w = font_cell_width(cp, size);
    if (bg >> 24) gfx_fill(c, x, y, w, 16 * scale, bg);
    char text[4]; int n;
    if (cp < 0x80) { text[0] = (char)cp; n = 1; }
    else if (cp < 0x800) { text[0] = (char)(0xc0 | cp >> 6); text[1] = (char)(0x80 | (cp & 63)); n = 2; }
    else if (cp < 0x10000) {
        text[0] = (char)(0xe0 | cp >> 12); text[1] = (char)(0x80 | (cp >> 6 & 63));
        text[2] = (char)(0x80 | (cp & 63)); n = 3;
    } else {
        text[0] = (char)(0xf0 | cp >> 18); text[1] = (char)(0x80 | (cp >> 12 & 63));
        text[2] = (char)(0x80 | (cp >> 6 & 63)); text[3] = (char)(0x80 | (cp & 63)); n = 4;
    }
    float px = 12.0f * scale, advance = font_advance(f, px, cp);
    font_draw(c, f, px, x + (w - advance) * 0.5f, y + 12 * scale, text, (size_t)n, fg);
    return w;
}
