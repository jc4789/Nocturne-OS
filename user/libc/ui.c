/* The UI engine (ui.h): smooth shapes, text, and the windows that apps describe as views. */
#include <string.h>
#include "nocturne.h"

/* ---- smooth shapes (ui.h) ----
   Each shape is a signed distance d from its edge (negative inside); a pixel
   is covered by clamp(0.5 - d) of its area. Along a row the coverage only
   grows towards the middle, so each row computes its two ends pixel by pixel
   and fills the run between them in one go. */
#include <limits.h>
#include <math.h>
#include "ui.h"
#include "font.h"
#include "display.h"

struct box { float cx, cy, hw, hh, r; };

static struct box box_of(float x, float y, float w, float h, float r) {
    struct box b = {x + w / 2, y + h / 2, w / 2, h / 2, r};
    float m = b.hw < b.hh ? b.hw : b.hh;
    if (b.r > m) b.r = m;
    if (b.r < 0) b.r = 0;
    return b;
}

static float box_distance(const struct box *b, float px, float py) {
    float qx = fabsf(px - b->cx) - (b->hw - b->r), qy = fabsf(py - b->cy) - (b->hh - b->r);
    float ox = qx > 0 ? qx : 0, oy = qy > 0 ? qy : 0;
    float inside = qx > qy ? qx : qy;
    return sqrtf(ox * ox + oy * oy) + (inside < 0 ? inside : 0) - b->r;
}

static inline float clamp01(float v) { return v < 0 ? 0 : v > 1 ? 1 : v; }

static inline void put(canvas_t *c, int x, int y, uint32_t argb, float cover) {
    uint32_t a = (uint32_t)((argb >> 24) * cover + 0.5f);
    if (a) gfx_blend_pixel(c, x, y, (a << 24) | (argb & 0xFFFFFFu));
}

static inline void run(canvas_t *c, int x0, int x1, int y, uint32_t argb, float cover) {
    if (x1 <= x0) return;
    uint32_t a = (uint32_t)((argb >> 24) * cover + 0.5f);
    if (a) gfx_fill_blend(c, x0, y, x1 - x0, 1, (a << 24) | (argb & 0xFFFFFFu));
}

/* cover(x) for one row is symmetric about the middle and grows towards it. */
typedef float (*cover_fn)(const void *shape, float px, float py);

static void rows(canvas_t *c, const void *shape, cover_fn cover, float x0f, float y0f, float x1f, float y1f,
                 float mid, uint32_t argb, uint32_t bottom) {
    int y0 = (int)floorf(y0f), y1 = (int)ceilf(y1f), x0 = (int)floorf(x0f), x1 = (int)ceilf(x1f);
    if (y0 < c->cy0) y0 = c->cy0;
    if (y1 > c->cy1) y1 = c->cy1;
    float span = y1f - y0f > 1 ? y1f - y0f : 1;
    for (int y = y0; y < y1; y++) {
        uint32_t col = argb;
        if (bottom != argb) {
            int t = (int)(clamp01((y + 0.5f - y0f) / span) * 255 + 0.5f);
            col = (gfx_mix(argb, bottom, t) & 0xFFFFFFu) |
                  ((uint32_t)(((argb >> 24) * (255 - t) + (bottom >> 24) * t) / 255) << 24);
        }
        float py = y + 0.5f, inner = cover(shape, mid, py);
        int l = x0, r = x1 - 1;
        while (l <= r) {
            float v = cover(shape, l + 0.5f, py);
            if (v == inner && l + 0.5f < mid) break;
            put(c, l, y, col, v);
            l++;
        }
        while (r >= l) {
            float v = cover(shape, r + 0.5f, py);
            if (v == inner && r + 0.5f > mid) break;
            put(c, r, y, col, v);
            r--;
        }
        run(c, l, r + 1, y, col, inner);
    }
}

static float fill_cover(const void *s, float px, float py) { return clamp01(0.5f - box_distance(s, px, py)); }

void ui_round_gradient(canvas_t *c, float x, float y, float w, float h, float r, uint32_t top, uint32_t bottom) {
    if (w <= 0 || h <= 0) return;
    struct box b = box_of(x, y, w, h, r);
    rows(c, &b, fill_cover, x, y, x + w, y + h, b.cx, top, bottom);
}

void ui_round_fill(canvas_t *c, float x, float y, float w, float h, float r, uint32_t argb) {
    ui_round_gradient(c, x, y, w, h, r, argb, argb);
}

struct ring { struct box outer, inner; };

static float ring_cover(const void *s, float px, float py) {
    const struct ring *g = s;
    float o = clamp01(0.5f - box_distance(&g->outer, px, py));
    float i = g->inner.hw > 0 && g->inner.hh > 0 ? clamp01(0.5f - box_distance(&g->inner, px, py)) : 0;
    return o > i ? o - i : 0;
}

void ui_round_stroke(canvas_t *c, float x, float y, float w, float h, float r, float width, uint32_t argb) {
    if (w <= 0 || h <= 0 || width <= 0) return;
    struct ring g = {box_of(x, y, w, h, r), box_of(x + width, y + width, w - 2 * width, h - 2 * width, r - width)};
    /* the middle of a row inside the hole is empty: walk both ends only */
    int y0 = (int)floorf(y), y1 = (int)ceilf(y + h), x0 = (int)floorf(x), x1 = (int)ceilf(x + w);
    if (y0 < c->cy0) y0 = c->cy0;
    if (y1 > c->cy1) y1 = c->cy1;
    for (int yy = y0; yy < y1; yy++) {
        float py = yy + 0.5f, inner = ring_cover(&g, g.outer.cx, py);
        if (inner > 0) { rows(c, &g, ring_cover, x, (float)yy, x + w, (float)yy + 1, g.outer.cx, argb, argb); continue; }
        int l = x0;
        for (; l < x1 && l + 0.5f < g.outer.cx; l++) {
            float v = ring_cover(&g, l + 0.5f, py);
            if (v == 0 && clamp01(0.5f - box_distance(&g.inner, l + 0.5f, py)) >= 1) break;
            put(c, l, yy, argb, v);
        }
        for (int r2 = x1 - 1; r2 > l && r2 + 0.5f > g.outer.cx; r2--) {
            float v = ring_cover(&g, r2 + 0.5f, py);
            if (v == 0 && clamp01(0.5f - box_distance(&g.inner, r2 + 0.5f, py)) >= 1) break;
            put(c, r2, yy, argb, v);
        }
    }
}

struct shadow { struct box b; float blur; };

static float shadow_cover(const void *s, float px, float py) {
    const struct shadow *g = s;
    float t = clamp01((box_distance(&g->b, px, py) + g->blur) / (2 * g->blur));
    float v = 1 - t * t * (3 - 2 * t); /* smoothstep */
    return (float)(int)(v * 64 + 0.5f) / 64; /* steps let a row's middle run start early */
}

void ui_round_shadow(canvas_t *c, float x, float y, float w, float h, float r, float blur, uint32_t argb) {
    if (w <= 0 || h <= 0) return;
    if (blur < 0.5f) { ui_round_fill(c, x, y, w, h, r, argb); return; }
    struct shadow g = {box_of(x, y, w, h, r), blur};
    rows(c, &g, shadow_cover, x - blur, y - blur, x + w + blur, y + h + blur, g.b.cx, argb, argb);
}

void ui_line(canvas_t *c, float x0, float y0, float x1, float y1, float width, uint32_t argb) {
    float hw = width / 2, dx = x1 - x0, dy = y1 - y0, len2 = dx * dx + dy * dy;
    int bx0 = (int)floorf(fminf(x0, x1) - hw - 1), bx1 = (int)ceilf(fmaxf(x0, x1) + hw + 1);
    int by0 = (int)floorf(fminf(y0, y1) - hw - 1), by1 = (int)ceilf(fmaxf(y0, y1) + hw + 1);
    for (int y = by0; y < by1; y++)
        for (int x = bx0; x < bx1; x++) {
            float px = x + 0.5f - x0, py = y + 0.5f - y0;
            float t = len2 > 0 ? clamp01((px * dx + py * dy) / len2) : 0;
            float ex = px - t * dx, ey = py - t * dy;
            float v = clamp01(0.5f - (sqrtf(ex * ex + ey * ey) - hw));
            if (v > 0) put(c, x, y, argb, v);
        }
}

/* ---- interface text ---- */
static font_t *ui_face(int weight) {
    return font_family(weight & UI_TEXT_MONO ? FONT_FAMILY_MONO : FONT_FAMILY_SANS, weight & UI_TEXT_BOLD ? FONT_BOLD : FONT_REGULAR);
}

float ui_line_height(float size, int weight, float *baseline) {
    float a = size * 0.97f, d = size * 0.24f;
    font_t *f = ui_face(weight);
    if (f) font_metrics(f, size, &a, &d, NULL);
    if (baseline) *baseline = ceilf(a);
    return ceilf(a) + ceilf(d);
}

float ui_text(canvas_t *c, float x, float y, const char *s, size_t n, float size, int weight, uint32_t argb) {
    font_t *f = ui_face(weight);
    if (!f) return 0;
    float base;
    ui_line_height(size, weight, &base);
    return font_draw(c, f, size, x, (int)floorf(y + base + 0.5f), s, n, argb) - x;
}

float ui_text_width(const char *s, size_t n, float size, int weight) {
    font_t *f = ui_face(weight);
    return f ? font_width(f, size, s, n) : 0;
}

/* ---- the engine (ui.h: sessions) ----
   A view arrives as bytes and is kept as it came: the nodes point into the
   copy for their strings. Every render places the tree, paints it and works
   out which widget is under the pointer; identities (from a node's place
   under its parent, or its key) carry hover, presses, focus, scrolling and
   animations from one view to the next. */
#include <stdlib.h>

const struct ui_theme ui_theme = {
#define UI_THEME_VALUE(name, value) .name = value,
    UI_THEME_COLOURS(UI_THEME_VALUE) UI_THEME_SIZES(UI_THEME_VALUE)
#undef UI_THEME_VALUE
};

static const struct { const char *name; double value; } theme_values[] = {
#define UI_THEME_ENTRY(name, value) {#name, (double)(value)},
    UI_THEME_COLOURS(UI_THEME_ENTRY) UI_THEME_SIZES(UI_THEME_ENTRY)
#undef UI_THEME_ENTRY
};

/* -- bytes -- */
struct str { const char *p; uint32_t n; };
struct rd { const uint8_t *p, *end; bool bad; };

static const uint8_t *rd_take(struct rd *r, size_t n) {
    if (r->bad || (size_t)(r->end - r->p) < n) { r->bad = true; return NULL; }
    const uint8_t *at = r->p;
    r->p += n;
    return at;
}
static uint8_t rd_u8(struct rd *r) { const uint8_t *p = rd_take(r, 1); return p ? *p : 0; }
static uint16_t rd_u16(struct rd *r) { uint16_t v = 0; const uint8_t *p = rd_take(r, 2); if (p) memcpy(&v, p, 2); return v; }
static uint32_t rd_u32(struct rd *r) { uint32_t v = 0; const uint8_t *p = rd_take(r, 4); if (p) memcpy(&v, p, 4); return v; }
static double rd_f64(struct rd *r) { double v = 0; const uint8_t *p = rd_take(r, 8); if (p) memcpy(&v, p, 8); return v; }
static struct str rd_str(struct rd *r) {
    uint32_t n = rd_u32(r);
    const uint8_t *p = rd_take(r, n);
    return p ? (struct str){(const char *)p, n} : (struct str){"", 0};
}

static void wr_put(struct ui_bytes *w, const void *d, size_t n) {
    if (w->bad) return;
    if (w->n + n > w->cap) {
        size_t cap = w->cap ? w->cap * 2 : 64;
        while (cap < w->n + n) cap *= 2;
        uint8_t *p = realloc(w->p, cap);
        if (!p) { w->bad = true; return; }
        w->p = p;
        w->cap = cap;
    }
    memcpy(w->p + w->n, d, n);
    w->n += n;
}
static void wr_u8(struct ui_bytes *w, uint8_t v) { wr_put(w, &v, 1); }
static void wr_u32(struct ui_bytes *w, uint32_t v) { wr_put(w, &v, 4); }
static void wr_f64(struct ui_bytes *w, double v) { wr_put(w, &v, 8); }
static void wr_str(struct ui_bytes *w, const char *s, size_t n) { wr_u32(w, (uint32_t)n); wr_put(w, s, n); }

static bool str_is(struct str s, const char *z) { size_t n = strlen(z); return s.n == n && !memcmp(s.p, z, n); }

static uint64_t mix(uint64_t a, uint64_t b) {
    uint64_t z = a * 0x9E3779B97F4A7C15ull + b + 0x632BE59BD9B4E019ull;
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
    z ^= z >> 31;
    return z ? z : 1;
}
static uint64_t hash_bytes(const void *d, size_t n) {
    uint64_t h = 0xCBF29CE484222325ull;
    for (size_t i = 0; i < n; i++) h = (h ^ ((const uint8_t *)d)[i]) * 0x100000001B3ull;
    return h;
}

static uint32_t colour_of(double v) { return v < 0 ? (uint32_t)(int64_t)v : (uint32_t)(uint64_t)v; }
static uint32_t with_alpha(uint32_t c, float a) { return ((uint32_t)lroundf(clamp01(a) * 255) << 24) | (c & 0xFFFFFFu); }
/* a and b mixed channel by channel, alpha too (t = 0 is a) */
static uint32_t mixc(uint32_t a, uint32_t b, float t) {
    if (t <= 0) return a;
    if (t >= 1) return b;
    uint32_t out = 0;
    for (int shift = 0; shift < 32; shift += 8) {
        float ca = (float)((a >> shift) & 255), cb = (float)((b >> shift) & 255);
        out |= (uint32_t)lroundf(ca + (cb - ca) * t) << shift;
    }
    return out;
}

/* -- views -- */
enum { K_NONE, K_COLUMN, K_ROW, K_SCROLL, K_CARD, K_TEXT, K_BUTTON, K_TOGGLE, K_CHECKBOX, K_SLIDER, K_PROGRESS,
       K_SEGMENTED, K_FIELD, K_ITEM, K_ICON, K_DIVIDER, K_SPACER, K_CANVAS, K_SURFACE, K_COUNT };
static const char *const kind_names[K_COUNT] = {
    [K_COLUMN] = "column", [K_ROW] = "row", [K_SCROLL] = "scroll", [K_CARD] = "card", [K_TEXT] = "text",
    [K_BUTTON] = "button", [K_TOGGLE] = "toggle", [K_CHECKBOX] = "checkbox", [K_SLIDER] = "slider",
    [K_PROGRESS] = "progress", [K_SEGMENTED] = "segmented", [K_FIELD] = "field", [K_ITEM] = "item",
    [K_ICON] = "icon", [K_DIVIDER] = "divider", [K_SPACER] = "spacer", [K_CANVAS] = "canvas", [K_SURFACE] = "surface",
};
enum { A_NONE, A_START, A_CENTER, A_END, A_STRETCH, A_BETWEEN };
enum { SZ_NONE, SZ_NUM, SZ_FILL };
enum { V_SECONDARY, V_PRIMARY, V_GHOST };
enum { S_NONE, S_HEADING, S_TITLE, S_CAPTION };
/* which optional colours a node has */
enum { HAS_BACKGROUND = 1, HAS_BORDER = 2, HAS_HOVER = 4, HAS_PRESSED = 8, HAS_COLOUR = 16 };

struct node {
    uint8_t kind, align, self, justify, wmode, hmode, variant, text_align, style, weight, has;
    bool disabled, shadow, clickable, compact, secret, on, wrap, padded, pointer, select_on_focus, ellipsis, mono;
    uint32_t background, border, hover, pressed, colour, surface;
    float width, height, min_w, min_h, grow, pad[4], gap, radius, max_h, size, line_height;
    float step, wheel_step, length, value, min, max, selected, drawn_w, drawn_h;
    float focus_req, reveal;        /* "focus" and "reveal": act when they change (see ui.h) */
    struct str text, icon, placeholder, draw;
    uint32_t opt, nopts;            /* options: w->strs[opt .. opt + nopts) */
    uint32_t first, next, count;    /* first child, next sibling (0: none), number of children */
    uint64_t key, id;
    float x, y, w, h, scroll_max;
    float vis[4];                   /* the part of the window it can be seen in: x0, y0, x1, y1 */
    float nat_in, nat_w, nat_h;     /* the last natural size and the width it was asked for */
    bool nat_ok;
    float lay_w, lay_h, lay_g;      /* scratch while its parent places it */
};

struct state {
    uint64_t id;
    float scroll, drag_from, drag_y;
    int req_w, req_h;
    float shift;                    /* a focused field: how far its text is scrolled left */
    float focus_seen, reveal_seen;  /* the last "focus" and "reveal" acted on */
};
struct anim { uint64_t key, node; float from, to, ms; uint64_t start; bool seen; }; /* node: whose it is */

struct surface {
    uint32_t id;
    n_handle shared;
    size_t bytes;
    bool deep;   /* a deep plane (one byte a pixel, see display.h) follows the pixels */
    canvas_t c;
};

struct uiwin {
    struct ui_session *s;
    uint32_t id, view, background;
    window_t *win;
    uint8_t *tree;           /* the view as it came; nodes point into it */
    size_t tree_n;
    struct node *nodes;
    uint32_t n, cap;
    struct str *strs;
    uint32_t nstrs, capstrs;
    struct state *states;    /* open addressing by id */
    uint32_t nstates, capstates;
    struct anim *anims;
    uint32_t nanims, capanims;
    struct str *lines;       /* scratch for wrapped text */
    uint32_t nlines, caplines;
    uint64_t hot, pressed, dragging, focus, over; /* over: the pointer canvas or surface under the pointer */
    uint8_t *deep;           /* the window's deep plane while it is painted, or NULL */
    float px, py;
    uint64_t now, redraw_at, caret_at;
    bool dirty, animating, caret_on, hold;
    bool frame_wanted;       /* a surface changed, or the app asked ('F'), since the last UI_EVENT_FRAME */
    /* The focused field's text as the engine edits it: ahead of the app's views, so keys
       that come before the app has sent back the last value build on it, not on the view. */
    char *edit;
    uint32_t edit_n, edit_cap;
    uint32_t caret, anchor;  /* byte offsets in edit: the caret and the other end of the selection */
    uint64_t edit_app;       /* the hash of the value the app last gave the focused field */
    uint64_t sent[8];        /* hashes of the values sent in text events not yet seen back */
    uint32_t nsent;
    bool selecting;          /* the pointer is dragging out a selection in the focused field */
    uint32_t keys[8], nkeys; /* keys down whose press went to the app (their release goes too) */
    /* What the window shows, so that a picture paints only what changed: each node's signature
       (all its painting depends on) and box, by id; the boxes surfaces changed meanwhile; and
       the canvas and deep plane the last picture went into (a new one is painted whole). */
    struct shown { uint64_t id, sig; float box[4]; } *shown;
    uint32_t capshown;
    float damage[4];         /* x0, y0, x1, y1; nothing when x0 >= x1 */
    uint32_t *shown_px;
    bool shown_deep;
};

struct msg { struct msg *next; size_t n; uint8_t data[]; };

struct ui_session {
    struct uiwin **wins;
    uint32_t nwins, capwins;
    struct surface **surfaces;
    uint32_t nsurfaces, capsurfaces;
    struct msg *head, *tail, *replies; /* replies: the last reply at the front of the queue */
    n_handle queue;
};

static bool grow_array(void **p, uint32_t *cap, uint32_t need, size_t size) {
    if (need <= *cap) return true;
    uint32_t c = *cap ? *cap : 16;
    while (c < need) c *= 2;
    void *q = realloc(*p, (size_t)c * size);
    if (!q) return false;
    *p = q;
    *cap = c;
    return true;
}

/* Replies go after the replies already waiting, ahead of events. */
static void post(struct ui_session *s, struct ui_bytes *w, bool reply) {
    struct msg *m = w->bad ? NULL : malloc(sizeof *m + w->n);
    if (m) {
        m->n = w->n;
        memcpy(m->data, w->p, w->n);
        if (reply) {
            struct msg **at = s->replies ? &s->replies->next : &s->head;
            m->next = *at;
            *at = m;
            if (!m->next) s->tail = m;
            s->replies = m;
        } else {
            m->next = NULL;
            if (s->tail) s->tail->next = m;
            else s->head = m;
            s->tail = m;
        }
    }
    free(w->p);
}

static void emit(struct uiwin *w, uint32_t type, const struct node *n, uint32_t mods, uint32_t flags, double a,
                 double b, const char *text, size_t tn) {
    struct ui_bytes m = {0};
    wr_u8(&m, 'e');
    wr_u32(&m, w->id);
    wr_u32(&m, w->view);
    wr_u32(&m, type);
    wr_u32(&m, n ? (uint32_t)(n - w->nodes) : UINT32_MAX);
    wr_u32(&m, mods);
    wr_u32(&m, flags);
    wr_f64(&m, a);
    wr_f64(&m, b);
    wr_str(&m, text ? text : "", text ? tn : 0);
    post(w->s, &m, false);
}

/* -- per-identity state and animations -- */
static struct state *state_of(struct uiwin *w, uint64_t id) {
    if ((w->nstates + 1) * 4 > w->capstates * 3) {
        uint32_t cap = w->capstates ? w->capstates * 2 : 64;
        struct state *t = calloc(cap, sizeof *t);
        if (!t) return NULL;
        for (uint32_t i = 0; i < w->capstates; i++)
            if (w->states[i].id) {
                uint32_t j = (uint32_t)w->states[i].id & (cap - 1);
                while (t[j].id) j = (j + 1) & (cap - 1);
                t[j] = w->states[i];
            }
        free(w->states);
        w->states = t;
        w->capstates = cap;
    }
    uint32_t j = (uint32_t)id & (w->capstates - 1);
    while (w->states[j].id && w->states[j].id != id) j = (j + 1) & (w->capstates - 1);
    if (!w->states[j].id) {
        w->states[j] = (struct state){.id = id, .req_w = -1, .req_h = -1};
        w->nstates++;
    }
    return &w->states[j];
}

static struct anim *anim_slot(struct anim *t, uint32_t cap, uint64_t key) {
    uint32_t j = (uint32_t)key & (cap - 1);
    while (t[j].key && t[j].key != key) j = (j + 1) & (cap - 1);
    return &t[j];
}

static float ease(float t) { return 1 - (1 - t) * (1 - t) * (1 - t); }

/* Where an animation is at now. */
static float anim_value(const struct anim *a, uint64_t now) {
    return a->from + (a->to - a->from) * ease(clamp01((float)(now - a->start) / a->ms));
}

/* A value of node n (what tells its values apart) that eases towards target over ms;
   its first use starts at target. */
static float animate(struct uiwin *w, const struct node *n, char what, float target, float ms) {
    uint64_t key = mix(n->id, (uint8_t)what);
    if ((w->nanims + 1) * 4 > w->capanims * 3) {
        uint32_t cap = w->capanims ? w->capanims * 2 : 64;
        struct anim *t = calloc(cap, sizeof *t);
        if (!t) return target;
        for (uint32_t i = 0; i < w->capanims; i++)
            if (w->anims[i].key) *anim_slot(t, cap, w->anims[i].key) = w->anims[i];
        free(w->anims);
        w->anims = t;
        w->capanims = cap;
    }
    struct anim *a = anim_slot(w->anims, w->capanims, key);
    if (!a->key) {
        *a = (struct anim){.key = key, .node = n->id, .from = target, .to = target, .ms = ms, .start = w->now, .seen = true};
        w->nanims++;
        return target;
    }
    a->seen = true;
    if (a->to != target) {
        a->from = anim_value(a, w->now);
        a->to = target;
        a->start = w->now;
        a->ms = ms;
    }
    float t = clamp01((float)(w->now - a->start) / a->ms);
    if (t < 1) w->animating = true;
    return a->from + (a->to - a->from) * ease(t);
}

/* Drops the animations of nodes that are gone (find_damage and paint mark the others seen). */
static void prune_anims(struct uiwin *w) {
    if (!w->capanims) return;
    struct anim *t = calloc(w->capanims, sizeof *t);
    if (!t) return;
    uint32_t n = 0;
    for (uint32_t i = 0; i < w->capanims; i++)
        if (w->anims[i].key && w->anims[i].seen) {
            struct anim *a = anim_slot(t, w->capanims, w->anims[i].key);
            *a = w->anims[i];
            a->seen = false;
            n++;
        }
    free(w->anims);
    w->anims = t;
    w->nanims = n;
}

/* -- parsing a view -- */
static uint8_t word(struct str s, const char *const *names, int count) {
    for (int i = 0; i < count; i++)
        if (names[i] && str_is(s, names[i])) return (uint8_t)i;
    return 0;
}
static const char *const align_names[] = {NULL, "start", "center", "end", "stretch", "between"};
static const char *const variant_names[] = {"secondary", "primary", "ghost"};
static const char *const style_names[] = {NULL, "heading", "title", "caption"};

static void node_init(struct node *n, uint8_t kind) {
    memset(n, 0, sizeof *n);
    n->kind = kind;
    n->weight = 255;
    n->wrap = true;
    n->width = n->height = NAN;
    n->grow = n->gap = n->radius = n->max_h = n->size = n->line_height = NAN;
    n->step = n->wheel_step = n->length = n->min = n->max = n->focus_req = n->reveal = NAN;
    n->selected = -1;
    n->text = n->icon = n->placeholder = n->draw = (struct str){"", 0};
}

static void parse_prop(struct uiwin *w, struct node *n, struct rd *r) {
    uint8_t nl = rd_u8(r);
    const uint8_t *np = rd_take(r, nl);
    struct str name = np ? (struct str){(const char *)np, nl} : (struct str){"", 0};
    uint8_t type = rd_u8(r);
    double num = 0;
    struct str s = {"", 0};
    uint32_t count = 0, first = w->nstrs;
    const uint8_t *nums = NULL;
    switch (type) {
    case 1: num = rd_f64(r); break;
    case 2: case 5: s = rd_str(r); break;
    case 3:
        count = rd_u32(r);
        nums = rd_take(r, (size_t)count * 8);
        break;
    case 4:
        count = rd_u32(r);
        for (uint32_t i = 0; i < count && !r->bad; i++) {
            struct str v = rd_str(r);
            if (!grow_array((void **)&w->strs, &w->capstrs, w->nstrs + 1, sizeof *w->strs)) { r->bad = true; break; }
            w->strs[w->nstrs++] = v;
        }
        break;
    default: r->bad = true;
    }
    if (r->bad) return;
    float f = (float)num;
#define NUMBER(prop, field) else if (str_is(name, prop)) { if (type == 1) n->field = f; }
#define COLOUR(prop, field, flag) else if (str_is(name, prop)) { if (type == 1) { n->field = colour_of(num); n->has |= flag; } }
#define FLAG(prop, field) else if (str_is(name, prop)) { if (type == 1) n->field = num != 0; }
#define TEXT(prop, field) else if (str_is(name, prop)) { if (type == 2) n->field = s; }
    if (str_is(name, "width") || str_is(name, "height")) {
        uint8_t mode = type == 1 ? SZ_NUM : type == 2 && str_is(s, "fill") ? SZ_FILL : SZ_NONE;
        if (name.p[0] == 'w') { n->wmode = mode; n->width = f; }
        else { n->hmode = mode; n->height = f; }
    }
    else if (str_is(name, "padding")) {
        if (type == 1) n->pad[0] = n->pad[1] = n->pad[2] = n->pad[3] = f;
        else if (type == 3 && (count == 2 || count == 4)) {
            double v[4];
            memcpy(v, nums, count * 8);
            if (count == 2) v[2] = v[0], v[3] = v[1];
            for (int i = 0; i < 4; i++) n->pad[i] = (float)v[i];
        } else return;
        n->padded = true;
    }
    else if (str_is(name, "align")) { if (type == 2) n->align = word(s, align_names, 5); }
    else if (str_is(name, "self")) { if (type == 2) n->self = word(s, align_names, 5); }
    else if (str_is(name, "justify")) { if (type == 2) n->justify = word(s, align_names, 6); }
    else if (str_is(name, "textAlign")) { if (type == 2) n->text_align = word(s, align_names, 4); }
    else if (str_is(name, "variant")) { if (type == 2) n->variant = word(s, variant_names, 3); }
    else if (str_is(name, "style")) { if (type == 2) n->style = word(s, style_names, 4); }
    else if (str_is(name, "weight")) { if (type == 1) n->weight = num >= 1 ? 1 : 0; }
    else if (str_is(name, "key")) {
        if (type == 1) n->key = mix(0x6B6579, hash_bytes(&num, sizeof num));
        else if (type == 2) n->key = mix(0x6B6579 ^ 1, hash_bytes(s.p, s.n));
    }
    else if (str_is(name, "value")) {
        if (type == 1) n->value = f;
        else if (type == 2) n->text = s;
    }
    else if (str_is(name, "options")) { if (type == 4) { n->opt = first; n->nopts = count; } }
    else if (str_is(name, "draw")) { if (type == 5) n->draw = s; }
    else if (str_is(name, "drawn")) {
        if (type == 3 && count == 2) {
            double v[2];
            memcpy(v, nums, 16);
            n->drawn_w = (float)v[0];
            n->drawn_h = (float)v[1];
        }
    }
    else if (str_is(name, "surface")) { if (type == 1 && num >= 0) n->surface = (uint32_t)num; }
    NUMBER("minWidth", min_w) NUMBER("minHeight", min_h) NUMBER("maxHeight", max_h) NUMBER("lineHeight", line_height)
    NUMBER("wheelStep", wheel_step) NUMBER("grow", grow) NUMBER("gap", gap) NUMBER("radius", radius)
    NUMBER("size", size) NUMBER("step", step) NUMBER("length", length) NUMBER("min", min) NUMBER("max", max)
    NUMBER("selected", selected) NUMBER("focus", focus_req) NUMBER("reveal", reveal)
    COLOUR("background", background, HAS_BACKGROUND) COLOUR("border", border, HAS_BORDER)
    COLOUR("colour", colour, HAS_COLOUR) COLOUR("hoverBackground", hover, HAS_HOVER)
    COLOUR("pressedBackground", pressed, HAS_PRESSED)
    FLAG("disabled", disabled) FLAG("shadow", shadow) FLAG("clickable", clickable) FLAG("compact", compact)
    FLAG("secret", secret) FLAG("on", on) FLAG("wrap", wrap) FLAG("pointer", pointer)
    FLAG("selectOnFocus", select_on_focus) FLAG("ellipsis", ellipsis) FLAG("mono", mono)
    TEXT("text", text) TEXT("icon", icon) TEXT("placeholder", placeholder)
#undef NUMBER
#undef COLOUR
#undef FLAG
#undef TEXT
}

/* What a node is when the app left a property out. */
static void node_defaults(struct node *n, const struct node *parent) {
    const struct ui_theme *t = &ui_theme;
    if (parent && parent->disabled) n->disabled = true;
    switch (n->kind) {
    case K_CARD:
        n->kind = K_COLUMN;
        if (!n->padded) n->pad[0] = n->pad[1] = n->pad[2] = n->pad[3] = 16;
        if (isnan(n->gap)) n->gap = 12;
        if (!(n->has & HAS_BACKGROUND)) { n->background = t->surface; n->has |= HAS_BACKGROUND; }
        if (!(n->has & HAS_BORDER)) { n->border = t->border; n->has |= HAS_BORDER; }
        if (isnan(n->radius)) n->radius = t->radius;
        break;
    case K_TEXT:
        if (n->style == S_HEADING || n->style == S_TITLE) {
            if (isnan(n->size)) n->size = n->style == S_TITLE ? t->titleSize : t->headingSize;
            if (n->weight == 255) n->weight = 1;
        } else if (n->style == S_CAPTION) {
            if (isnan(n->size)) n->size = t->smallSize;
            if (!(n->has & HAS_COLOUR)) { n->colour = t->textDim; n->has |= HAS_COLOUR; }
        }
        if (isnan(n->line_height)) n->line_height = 1.25f;
        break;
    case K_BUTTON:
        if (isnan(n->radius)) n->radius = t->controlRadius;
        break;
    case K_SEGMENTED:
        if (isnan(n->size)) n->size = 13;
        break;
    case K_SLIDER:
        if (isnan(n->step)) n->step = 1;
        break;
    case K_ICON:
        if (isnan(n->size)) n->size = 18;
        break;
    case K_SPACER:
        if (isnan(n->size) && isnan(n->grow)) n->grow = 1;
        break;
    case K_CANVAS:
        if (n->wmode == SZ_NONE) { n->wmode = SZ_NUM; n->width = 100; }
        if (n->hmode == SZ_NONE) { n->hmode = SZ_NUM; n->height = 100; }
        break;
    }
    if (n->kind == K_COLUMN || n->kind == K_ROW || n->kind == K_SCROLL) {
        if (isnan(n->gap)) n->gap = n->kind == K_ROW ? 8 : 0;
        if (!n->align) n->align = n->kind == K_ROW ? A_CENTER : A_STRETCH;
        if (isnan(n->radius)) n->radius = 0;
    }
    if (n->kind != K_SPACER && isnan(n->size)) n->size = t->fontSize;
    if (n->kind != K_CANVAS && n->kind != K_SURFACE) n->pointer = false;
    if (n->weight == 255) n->weight = 0;
    if (n->mono) n->weight |= UI_TEXT_MONO;
    if (isnan(n->min)) n->min = 0;
    if (isnan(n->max)) n->max = 1;
}

static uint32_t parse_node(struct uiwin *w, struct rd *r, uint32_t parent, int depth) {
    if (depth > 256) { r->bad = true; return 0; }
    uint8_t kl = rd_u8(r);
    const uint8_t *kp = rd_take(r, kl);
    uint16_t nprops = rd_u16(r), nkids = rd_u16(r);
    if (r->bad || !grow_array((void **)&w->nodes, &w->cap, w->n + 1, sizeof *w->nodes)) { r->bad = true; return 0; }
    uint32_t i = w->n++;
    node_init(&w->nodes[i], word((struct str){(const char *)kp, kl}, kind_names, K_COUNT));
    for (uint16_t k = 0; k < nprops && !r->bad; k++) parse_prop(w, &w->nodes[i], r);
    node_defaults(&w->nodes[i], depth ? &w->nodes[parent] : NULL);
    uint32_t last = 0;
    for (uint16_t k = 0; k < nkids && !r->bad; k++) {
        uint32_t c = parse_node(w, r, i, depth + 1);
        if (r->bad) break;
        if (last) w->nodes[last].next = c;
        else w->nodes[i].first = c;
        last = c;
        w->nodes[i].count++;
    }
    return i;
}

/* -- text -- */
static bool breakable(uint32_t cp) {
    return (cp >= 0x2E80 && cp <= 0x9FFF) || (cp >= 0xAC00 && cp <= 0xD7AF) || (cp >= 0xF900 && cp <= 0xFAFF) ||
           (cp >= 0xFF00 && cp <= 0xFFEF) || (cp >= 0x20000 && cp <= 0x3FFFF);
}

static uint32_t utf8_next(const char *s, uint32_t n, uint32_t *i) {
    uint8_t c = (uint8_t)s[*i];
    int len = c < 0x80 ? 1 : c < 0xE0 ? 2 : c < 0xF0 ? 3 : 4;
    uint32_t cp = len == 1 ? c : len == 2 ? c & 0x1F : len == 3 ? c & 0x0F : c & 0x07;
    for (int k = 1; k < len && *i + k < n; k++) cp = cp << 6 | ((uint8_t)s[*i + k] & 0x3F);
    *i += (uint32_t)len;
    if (*i > n) *i = n;
    return cp;
}

/* Byte offsets of characters in UTF-8 text. */
static uint32_t char_start(const char *s, uint32_t i) {
    while (i > 0 && ((uint8_t)s[i] & 0xC0) == 0x80) i--;
    return i;
}
static uint32_t prev_char(const char *s, uint32_t i) { return i ? char_start(s, i - 1) : 0; }
static uint32_t next_char(const char *s, uint32_t n, uint32_t i) {
    if (i < n) i++;
    while (i < n && ((uint8_t)s[i] & 0xC0) == 0x80) i++;
    return i;
}
/* The length of the first line of a text. */
static uint32_t first_line(struct str t) {
    uint32_t n = 0;
    while (n < t.n && t.p[n] != '\n') n++;
    return n;
}

static float measure(const char *s, uint32_t n, float size, int weight) { return ui_text_width(s, n, size, weight); }

/* Where a byte offset of the focused field's text is drawn, from the start of the text. */
static float field_x(const struct node *n, const char *s, uint32_t off) {
    float size = ui_theme.fontSize;
    if (!n->secret) return measure(s, off, size, n->weight);
    uint32_t chars = 0;
    for (uint32_t i = 0; i < off; i++) chars += ((uint8_t)s[i] & 0xC0) != 0x80;
    return (float)chars * measure("\xE2\x80\xA2", 3, size, 0);
}

static void push_line(struct uiwin *w, const char *p, uint32_t a, uint32_t b) {
    while (b > a && p[b - 1] == ' ') b--;
    if (grow_array((void **)&w->lines, &w->caplines, w->nlines + 1, sizeof *w->lines))
        w->lines[w->nlines++] = (struct str){p + a, b - a};
}

/* The lines of a text at a width, into w->lines: greedy breaks at spaces, and
   between any two characters of CJK text. */
static uint32_t wrap(struct uiwin *w, struct str t, float size, int weight, bool brk, float width) {
    w->nlines = 0;
    const char *p = t.p;
    for (uint32_t start = 0; start <= t.n;) {
        uint32_t end = start;
        while (end < t.n && p[end] != '\n') end++;
        if (!brk || !(width > 0) || measure(p + start, end - start, size, weight) <= width) {
            push_line(w, p, start, end);
        } else {
            uint32_t line = start, i = start;
            float line_w = 0;
            while (i < end) {
                /* the next piece: a word and the spaces after it, or one CJK character */
                uint32_t a = i, cp = utf8_next(p, end, &i);
                if (cp != ' ' && !breakable(cp))
                    while (i < end) {
                        uint32_t j = i, c2 = utf8_next(p, end, &j);
                        if (breakable(c2)) break;
                        i = j;
                        if (c2 == ' ') break;
                    }
                while (i < end && p[i] == ' ') i++;
                uint32_t b = i;
                while (b > a && p[b - 1] == ' ') b--;
                if (line < a && line_w + measure(p + a, b - a, size, weight) > width) {
                    push_line(w, p, line, a);
                    line = a;
                    line_w = measure(p + a, i - a, size, weight);
                } else {
                    line_w += measure(p + a, i - a, size, weight);
                }
            }
            push_line(w, p, line, end);
        }
        start = end + 1;
    }
    return w->nlines;
}

/* -- layout -- */
static const float control_height = 32;

static float growth(const struct node *n) { return isnan(n->grow) ? 0 : n->grow; }

/* The size a node wants when it may use width. */
static void natural(struct uiwin *w, struct node *n, float width, float *ow, float *oh) {
    if (n->nat_ok && n->nat_in == width) { *ow = n->nat_w; *oh = n->nat_h; return; }
    const struct ui_theme *t = &ui_theme;
    float sw = 0, sh = 0, cw, ch;
    switch (n->kind) {
    case K_COLUMN: case K_ROW: case K_SCROLL: {
        float l = n->pad[3], r = n->pad[1], gap = n->gap;
        float inner = fmaxf(0, (n->wmode == SZ_NUM ? n->width : width) - l - r);
        if (n->kind == K_ROW) {
            float left = inner, grow = 0;
            for (uint32_t c = n->first; c; c = w->nodes[c].next) {
                struct node *k = &w->nodes[c];
                if (growth(k) > 0) { grow += growth(k); continue; }
                natural(w, k, left, &cw, &ch);
                left = fmaxf(0, left - cw - gap);
                sw += cw;
                sh = fmaxf(sh, ch);
            }
            float each = grow > 0 ? fmaxf(0, left) / grow : 0;
            for (uint32_t c = n->first; c; c = w->nodes[c].next) {
                struct node *k = &w->nodes[c];
                if (!(growth(k) > 0)) continue;
                natural(w, k, each * growth(k), &cw, &ch);
                if (k->kind == K_SPACER) cw = 0;
                sw += cw;
                sh = fmaxf(sh, ch);
            }
            if (n->count > 1) sw += gap * (float)(n->count - 1);
        } else {
            for (uint32_t c = n->first; c; c = w->nodes[c].next) {
                natural(w, &w->nodes[c], inner, &cw, &ch);
                sw = fmaxf(sw, cw);
                sh += ch;
            }
            if (n->count > 1) sh += gap * (float)(n->count - 1);
        }
        sw += l + r;
        sh += n->pad[0] + n->pad[2];
        if (n->kind == K_SCROLL && !isnan(n->max_h)) sh = fminf(sh, n->max_h);
        break;
    }
    case K_TEXT: {
        float lh = ui_line_height(n->size, n->weight, NULL), spacing = roundf(lh * n->line_height);
        if (n->ellipsis) { /* one line, no wider than it may be: what does not fit is cut */
            sw = fminf(ceilf(measure(n->text.p, first_line(n->text), n->size, n->weight)), width);
            sh = ceilf(lh);
            break;
        }
        uint32_t lines = wrap(w, n->text, n->size, n->weight, n->wrap, width);
        for (uint32_t i = 0; i < lines; i++)
            sw = fmaxf(sw, measure(w->lines[i].p, w->lines[i].n, n->size, n->weight));
        sw = ceilf(sw);
        sh = (float)lines * spacing - (spacing - ceilf(lh));
        break;
    }
    case K_BUTTON: {
        float icon_w = n->icon.n ? 18 + (n->text.n ? 8 : 0) : 0;
        sw = ceilf(measure(n->text.p, n->text.n, n->size, 1)) + icon_w + (n->compact ? 20 : 32);
        sh = control_height;
        break;
    }
    case K_SEGMENTED:
        for (uint32_t i = 0; i < n->nopts; i++)
            sw += ceilf(measure(w->strs[n->opt + i].p, w->strs[n->opt + i].n, n->size, 1)) + 28;
        sw += 6;
        sh = control_height + 2;
        break;
    case K_TOGGLE: sw = 44; sh = 24; break;
    case K_CHECKBOX: sw = 26 + ceilf(measure(n->text.p, n->text.n, t->fontSize, 0)); sh = 22; break;
    case K_SLIDER: case K_PROGRESS: case K_FIELD:
        sw = fminf(width, isnan(n->length) ? 280 : n->length);
        sh = n->kind == K_SLIDER ? 28 : n->kind == K_PROGRESS ? 8 : control_height + 2;
        break;
    case K_ITEM:
        sw = ceilf(measure(n->text.p, n->text.n, t->fontSize, n->selected > 0)) + (n->icon.n ? 28 : 0) + 28;
        sh = 38;
        break;
    case K_ICON: sw = sh = n->size; break;
    case K_DIVIDER: sw = sh = 1; break;
    case K_SPACER: sw = sh = isnan(n->size) ? 0 : n->size; break;
    }
    if (n->wmode == SZ_NUM) sw = n->width;
    if (n->hmode == SZ_NUM) sh = n->height;
    if (n->min_w > sw) sw = n->min_w;
    if (n->min_h > sh) sh = n->min_h;
    n->nat_in = width;
    n->nat_w = *ow = sw;
    n->nat_h = *oh = sh;
    n->nat_ok = true;
}

static void place(struct uiwin *w, struct node *n, float x, float y, float ww, float hh, uint64_t parent,
                  uint32_t index, const float *clip);

/* Lays the children of n along a column (vertical) or a row inside the box. */
static void place_children(struct uiwin *w, struct node *n, bool vertical, float ix, float iy, float iw, float ih,
                           uint64_t id, const float *clip) {
    float used = 0, grow = 0, gap = n->gap;
    for (uint32_t c = n->first; c; c = w->nodes[c].next) {
        struct node *k = &w->nodes[c];
        float cw, ch;
        natural(w, k, iw, &cw, &ch);
        if (!vertical && growth(k) > 0) cw = 0;
        bool fill = vertical ? k->hmode == SZ_FILL : k->wmode == SZ_FILL;
        float g = !isnan(k->grow) ? k->grow : fill ? 1 : 0;
        k->lay_w = cw;
        k->lay_h = ch;
        k->lay_g = g;
        grow += g;
        if (!(g > 0)) used += vertical ? ch : cw;
    }
    if (n->count > 1) used += gap * (float)(n->count - 1);
    float free = fmaxf(0, (vertical ? ih : iw) - used), pos = vertical ? iy : ix, between = gap;
    if (!(grow > 0)) {
        if (n->justify == A_CENTER) pos += free / 2;
        else if (n->justify == A_END) pos += free;
        else if (n->justify == A_BETWEEN && n->count > 1) between += free / (float)(n->count - 1);
    }
    uint32_t i = 0;
    for (uint32_t c = n->first; c; c = w->nodes[c].next, i++) {
        struct node *k = &w->nodes[c];
        float main = k->lay_g > 0 ? free * k->lay_g / grow : vertical ? k->lay_h : k->lay_w;
        float space = vertical ? iw : ih;
        uint8_t own = k->self ? k->self : n->align;
        bool fill = vertical ? k->wmode == SZ_FILL : k->hmode == SZ_FILL;
        float cross = own == A_STRETCH || fill ? space : fminf(vertical ? k->lay_w : k->lay_h, space);
        if (own == A_STRETCH && !fill) {
            if (vertical && k->wmode == SZ_NUM) cross = fminf(k->width, space);
            if (!vertical && k->hmode == SZ_NUM) cross = fminf(k->height, space);
        }
        float off = own == A_CENTER ? (space - cross) / 2 : own == A_END ? space - cross : 0;
        float px = roundf(vertical ? ix + off : pos), py = roundf(vertical ? pos : iy + off);
        float cw = roundf(vertical ? cross : main), ch = roundf(vertical ? main : cross);
        place(w, k, px, py, cw, ch, id, i, clip);
        pos += main + between;
    }
}

/* Gives a node its box, identity and visible part; then its children theirs. */
static void place(struct uiwin *w, struct node *n, float x, float y, float ww, float hh, uint64_t parent,
                  uint32_t index, const float *clip) {
    n->id = n->key ? mix(parent, n->key) : mix(parent, index + 1);
    n->x = x;
    n->y = y;
    n->w = ww;
    n->h = hh;
    memcpy(n->vis, clip, sizeof n->vis);
    float t = n->pad[0], r = n->pad[1], b = n->pad[2], l = n->pad[3];
    if (n->kind == K_SCROLL) {
        float inner = ww - l - r, needed = 0, cw, ch;
        for (uint32_t c = n->first; c; c = w->nodes[c].next) {
            natural(w, &w->nodes[c], inner, &cw, &ch);
            needed += ch;
        }
        if (n->count > 1) needed += n->gap * (float)(n->count - 1);
        struct state *st = state_of(w, n->id);
        float max = fmaxf(0, needed - (hh - t - b)), scroll = st ? fminf(fmaxf(0, st->scroll), max) : 0;
        if (st) st->scroll = scroll;
        n->scroll_max = max;
        float seen[4] = {fmaxf(clip[0], x), fmaxf(clip[1], y), fminf(clip[2], x + ww), fminf(clip[3], y + hh)};
        place_children(w, n, true, x + l, y + t - scroll, inner, fmaxf(needed, hh - t - b), mix(n->id, 0xC), seen);
        return;
    }
    if (n->kind == K_COLUMN || n->kind == K_ROW)
        place_children(w, n, n->kind == K_COLUMN, x + l, y + t, fmaxf(0, ww - l - r), fmaxf(0, hh - t - b), n->id, clip);
}

/* -- painting -- */
static void fill_rect(canvas_t *c, float x, float y, float w, float h, uint32_t col) {
    int x0 = (int)lroundf(x), y0 = (int)lroundf(y);
    gfx_fill_blend(c, x0, y0, (int)lroundf(x + w) - x0, (int)lroundf(y + h) - y0, col);
}

static void set_clip(canvas_t *c, const float *clip) {
    gfx_noclip(c);
    int x = (int)floorf(clip[0]), y = (int)floorf(clip[1]);
    gfx_clip(c, x, y, (int)ceilf(clip[2]) - x, (int)ceilf(clip[3]) - y);
}

/* Line icons drawn on a 24-unit grid. */
static void draw_icon(canvas_t *c, struct str name, float x, float y, float size, uint32_t col) {
    float k = size / 24;
#define L(x0, y0, x1, y1, wd) ui_line(c, x + (x0) * k, y + (y0) * k, x + (x1) * k, y + (y1) * k, (wd) * k, col)
#define BOX(x0, y0, w0, h0, r) ui_round_stroke(c, x + (x0) * k, y + (y0) * k, (w0) * k, (h0) * k, (r) * k, 1.8f * k, col)
#define DOT(cx, cy, r) ui_round_fill(c, x + ((cx) - (r)) * k, y + ((cy) - (r)) * k, 2 * (r) * k, 2 * (r) * k, (r) * k, col)
    if (str_is(name, "display")) { BOX(2.5, 4, 19, 13, 2.5); L(12, 17, 12, 20.5, 1.8); L(8, 20.5, 16, 20.5, 1.8); }
    else if (str_is(name, "mouse")) { BOX(6.5, 2.5, 11, 19, 5.5); L(12, 6, 12, 9.5, 1.8); }
    else if (str_is(name, "sound")) {
        L(4, 9.5, 7.5, 9.5, 1.8); L(4, 14.5, 7.5, 14.5, 1.8); L(4, 9.5, 4, 14.5, 1.8);
        L(7.5, 9.5, 12, 5.5, 1.8); L(7.5, 14.5, 12, 18.5, 1.8); L(12, 5.5, 12, 18.5, 1.8);
        L(15.5, 9.5, 16.5, 12, 1.8); L(16.5, 12, 15.5, 14.5, 1.8); L(18.5, 7, 20, 12, 1.8); L(20, 12, 18.5, 17, 1.8);
    }
    else if (str_is(name, "info")) { BOX(3, 3, 18, 18, 9); DOT(12, 8, 1.2f); L(12, 11, 12, 16.5, 2); }
    else if (str_is(name, "check")) { L(5, 12.5, 10, 17.5, 2.2); L(10, 17.5, 19.5, 7, 2.2); }
    else if (str_is(name, "plus")) { L(12, 5, 12, 19, 2); L(5, 12, 19, 12, 2); }
    else if (str_is(name, "minus")) L(5, 12, 19, 12, 2);
    else if (str_is(name, "close")) { L(6, 6, 18, 18, 2); L(18, 6, 6, 18, 2); }
    else if (str_is(name, "chevron-right")) { L(9, 5.5, 15.5, 12, 2); L(15.5, 12, 9, 18.5, 2); }
    else if (str_is(name, "chevron-left")) { L(15, 5.5, 8.5, 12, 2); L(8.5, 12, 15, 18.5, 2); }
    else if (str_is(name, "chevron-up")) { L(5.5, 15, 12, 8.5, 2); L(12, 8.5, 18.5, 15, 2); }
    else if (str_is(name, "chevron-down")) { L(5.5, 9, 12, 15.5, 2); L(12, 15.5, 18.5, 9, 2); }
    else if (str_is(name, "language")) {
        BOX(3, 3, 18, 18, 9); BOX(8, 3, 8, 18, 4);
        L(3.5, 12, 20.5, 12, 1.8); L(5, 7.5, 19, 7.5, 1.5); L(5, 16.5, 19, 16.5, 1.5);
    }
    else if (str_is(name, "settings")) {
        BOX(8.5, 8.5, 7, 7, 3.5);
        for (int i = 0; i < 8; i++) {
            float a = (float)i * 3.14159265f / 4, cs = cosf(a), sn = sinf(a);
            L(12 + cs * 6.5f, 12 + sn * 6.5f, 12 + cs * 9.5f, 12 + sn * 9.5f, 2.6);
        }
        BOX(5.5, 5.5, 13, 13, 6.5);
    }
    else if (str_is(name, "folder")) { BOX(2.5, 5, 19, 14.5, 2.5); L(2.5, 8.5, 21.5, 8.5, 1.8); }
    else if (str_is(name, "clock")) { BOX(3, 3, 18, 18, 9); L(12, 7, 12, 12, 1.8); L(12, 12, 15.5, 14, 1.8); }
    else if (str_is(name, "refresh")) { BOX(4.5, 4.5, 15, 15, 7.5); L(16, 3.5, 19.5, 5.5, 2); L(19.5, 5.5, 17, 8.5, 2); }
    else if (str_is(name, "home")) {
        L(3.5, 11.5, 12, 4, 1.8); L(12, 4, 20.5, 11.5, 1.8); L(6, 9.5, 6, 20, 1.8); L(18, 9.5, 18, 20, 1.8);
        L(6, 20, 18, 20, 1.8); L(10, 20, 10, 14.5, 1.8); L(10, 14.5, 14, 14.5, 1.8); L(14, 14.5, 14, 20, 1.8);
    }
    else if (str_is(name, "search")) { BOX(4, 4, 12, 12, 6); L(14.5, 14.5, 20, 20, 2.2); }
    else if (str_is(name, "backspace")) {
        L(2.5, 12, 8, 5.5, 1.8); L(8, 5.5, 21.5, 5.5, 1.8); L(21.5, 5.5, 21.5, 18.5, 1.8);
        L(21.5, 18.5, 8, 18.5, 1.8); L(8, 18.5, 2.5, 12, 1.8); L(11.5, 9, 17.5, 15, 1.8); L(17.5, 9, 11.5, 15, 1.8);
    }
    else DOT(12, 12, 3);
#undef L
#undef BOX
#undef DOT
}

/* What a canvas node draws: u8 operation, its f64 arguments and, for text and icons, a string. */
static void paint_ops(canvas_t *c, struct str ops, float ox, float oy) {
    static const uint8_t args[] = {0, 5, 7, 7, 7, 6, 5, 4};
    struct rd r = {(const uint8_t *)ops.p, (const uint8_t *)ops.p + ops.n, false};
    while (r.p < r.end) {
        uint8_t op = rd_u8(&r);
        if (!op || op >= sizeof args) break;
        double a[7];
        for (int i = 0; i < args[op]; i++) a[i] = rd_f64(&r);
        struct str s = op >= 6 ? rd_str(&r) : (struct str){"", 0};
        if (r.bad) break;
        float f[7];
        for (int i = 0; i < args[op]; i++) f[i] = (float)a[i];
        switch (op) {
        case 1: fill_rect(c, ox + f[0], oy + f[1], f[2], f[3], colour_of(a[4])); break;
        case 2: ui_round_gradient(c, ox + f[0], oy + f[1], f[2], f[3], f[4], colour_of(a[5]), colour_of(a[6])); break;
        case 3: ui_round_stroke(c, ox + f[0], oy + f[1], f[2], f[3], f[4], f[5], colour_of(a[6])); break;
        case 4: ui_round_shadow(c, ox + f[0], oy + f[1], f[2], f[3], f[4], f[5], colour_of(a[6])); break;
        case 5: ui_line(c, ox + f[0], oy + f[1], ox + f[2], oy + f[3], f[4], colour_of(a[5])); break;
        case 6: ui_text(c, ox + f[0], oy + f[1], s.p, s.n, f[2], (int)f[3] & (UI_TEXT_BOLD | UI_TEXT_MONO), colour_of(a[4])); break;
        case 7: draw_icon(c, s, ox + f[0], oy + f[1], f[2], colour_of(a[3])); break;
        }
    }
}

static struct surface *surface_of(struct ui_session *s, uint32_t id) {
    for (uint32_t i = 0; i < s->nsurfaces; i++)
        if (s->surfaces[i]->id == id) return s->surfaces[i];
    return NULL;
}

static float seg_width(struct uiwin *w, const struct node *n, uint32_t i) {
    struct str o = w->strs[n->opt + i];
    return ceilf(measure(o.p, o.n, n->size, 1)) + 28;
}

/* Where option i of a segmented control sits. */
static void seg_box(struct uiwin *w, const struct node *n, uint32_t i, float *bx, float *bw) {
    float total = 0, ox = n->x + 3;
    for (uint32_t j = 0; j < n->nopts; j++) total += seg_width(w, n, j);
    float scale = total > 0 ? (n->w - 6) / total : 0;
    for (uint32_t j = 0; j < i; j++) ox += seg_width(w, n, j) * scale;
    *bx = ox;
    *bw = seg_width(w, n, i) * scale;
}
static uint64_t seg_id(uint64_t id, uint32_t i) { return mix(id ^ 0x5E6D, i + 1); }

/* Copies the part x0..x1, y0..y1 of the window (clipped to it) from a surface placed at sx, sy, with its deep
   plane when both have one. */
static void copy_surface(struct uiwin *w, const struct surface *sf, int sx, int sy, int x0, int y0, int x1, int y1) {
    canvas_t *c = &w->win->c;
    if (x0 < 0) x0 = 0;
    if (y0 < 0) y0 = 0;
    if (x1 > c->w) x1 = c->w;
    if (y1 > c->h) y1 = c->h;
    if (x1 <= x0 || y1 <= y0) return;
    for (int yy = y0; yy < y1; yy++)
        memcpy(c->px + (size_t)yy * (size_t)c->pitch + x0,
               sf->c.px + (size_t)(yy - sy) * (size_t)sf->c.pitch + (x0 - sx), (size_t)(x1 - x0) * 4);
    if (w->deep && sf->deep) {
        const uint8_t *from = (const uint8_t *)(sf->c.px + (size_t)sf->c.w * (size_t)sf->c.h);
        for (int yy = y0; yy < y1; yy++)
            memcpy(w->deep + (size_t)yy * (size_t)c->w + x0, from + (size_t)(yy - sy) * (size_t)sf->c.w + (x0 - sx),
                   (size_t)(x1 - x0));
    }
}

/* A canvas or surface that is not the size it was made for asks the app for one that is. */
static void ask_size(struct uiwin *w, struct node *n, float have_w, float have_h) {
    int pw = (int)lroundf(n->w), ph = (int)lroundf(n->h);
    if ((int)lroundf(have_w) == pw && (int)lroundf(have_h) == ph) return;
    struct state *st = state_of(w, n->id);
    if (!st || (st->req_w == pw && st->req_h == ph)) return;
    st->req_w = pw;
    st->req_h = ph;
    emit(w, UI_EVENT_CANVAS, n, 0, 0, pw, ph, NULL, 0);
    w->hold = true;
}

static void paint(struct uiwin *w, struct node *n, const float *clip) {
    const struct ui_theme *t = &ui_theme;
    canvas_t *c = &w->win->c;
    float x = n->x, y = n->y, ww = n->w, hh = n->h;
    if (x > clip[2] || y > clip[3] || x + ww < clip[0] || y + hh < clip[1]) return;
    bool hot = w->hot == n->id, pressed = hot && w->pressed == n->id, disabled = n->disabled;
    switch (n->kind) {
    case K_COLUMN: case K_ROW: case K_SCROLL: {
        float radius = n->radius;
        if (n->shadow) ui_round_shadow(c, x, y + 4, ww, hh, radius, 14, t->shadow);
        uint32_t bg = n->has & HAS_BACKGROUND ? n->background : 0;
        if (n->clickable && !disabled) {
            float h = animate(w, n, 'h', hot ? 1 : 0, 110);
            bg = mixc(n->has & HAS_BACKGROUND ? bg : with_alpha(t->surfaceHover, 0),
                      n->has & HAS_HOVER ? n->hover : t->surfaceHover, h);
            if (pressed) bg = n->has & HAS_PRESSED ? n->pressed : t->surfaceActive;
        }
        if (bg >> 24) ui_round_fill(c, x, y, ww, hh, radius, bg);
        if (n->has & HAS_BORDER) ui_round_stroke(c, x, y, ww, hh, radius, 1, n->border);
        if (n->kind == K_SCROLL) {
            float seen[4] = {fmaxf(clip[0], x), fmaxf(clip[1], y), fminf(clip[2], x + ww), fminf(clip[3], y + hh)};
            set_clip(c, seen);
            for (uint32_t k = n->first; k; k = w->nodes[k].next) paint(w, &w->nodes[k], seen);
            if (n->scroll_max > 0) {
                struct state *st = state_of(w, n->id);
                float track = hh - 8, thumb = fmaxf(28, track * hh / (hh + n->scroll_max));
                float ty = y + 4 + (track - thumb) * (st ? st->scroll : 0) / n->scroll_max;
                float a = animate(w, n, 'b', hot || w->dragging == n->id ? 1 : 0.55f, 110);
                ui_round_fill(c, x + ww - 7, ty, 4, thumb, 2, with_alpha(t->textDim, 0.35f + 0.35f * a));
            }
            set_clip(c, clip);
            return;
        }
        for (uint32_t k = n->first; k; k = w->nodes[k].next) paint(w, &w->nodes[k], clip);
        return;
    }
    case K_TEXT: {
        float lh = ui_line_height(n->size, n->weight, NULL), spacing = roundf(lh * n->line_height);
        uint32_t col = disabled ? t->textFaint : n->has & HAS_COLOUR ? n->colour : t->text;
        if (n->ellipsis) {
            struct str s = {n->text.p, first_line(n->text)};
            float full = measure(s.p, s.n, n->size, n->weight), lx = x;
            if (full > ww + 0.5f) {
                /* the longest start that fits with "…" after it */
                float dots = measure("\xE2\x80\xA6", 3, n->size, n->weight), at = 0;
                uint32_t cut = 0;
                for (uint32_t i = 0; i < s.n;) {
                    uint32_t j = next_char(s.p, s.n, i);
                    at += measure(s.p + i, j - i, n->size, n->weight);
                    if (at + dots > ww + 0.5f) break;
                    cut = i = j;
                }
                ui_text(c, x, y, s.p, cut, n->size, n->weight, col);
                ui_text(c, x + measure(s.p, cut, n->size, n->weight), y, "\xE2\x80\xA6", 3, n->size, n->weight, col);
                return;
            }
            if (n->text_align == A_CENTER) lx = x + (ww - full) / 2;
            else if (n->text_align == A_END) lx = x + ww - full;
            ui_text(c, roundf(lx), y, s.p, s.n, n->size, n->weight, col);
            return;
        }
        uint32_t lines = wrap(w, n->text, n->size, n->weight, n->wrap, ww + 0.5f);
        for (uint32_t i = 0; i < lines; i++) {
            struct str s = w->lines[i];
            float lx = x;
            if (n->text_align == A_CENTER) lx = x + (ww - measure(s.p, s.n, n->size, n->weight)) / 2;
            else if (n->text_align == A_END) lx = x + ww - measure(s.p, s.n, n->size, n->weight);
            ui_text(c, roundf(lx), y + (float)i * spacing, s.p, s.n, n->size, n->weight, col);
        }
        return;
    }
    case K_BUTTON: {
        float h = animate(w, n, 'h', hot && !disabled ? 1 : 0, 110), r = n->radius;
        uint32_t bg, fg = n->has & HAS_COLOUR ? n->colour : t->text, border = 0;
        if (n->has & HAS_BACKGROUND) {
            uint32_t hover = n->has & HAS_HOVER ? n->hover : mixc(n->background, 0xFFFFFFFFu, 0.1f);
            bg = !pressed ? mixc(n->background, hover, h)
                 : n->has & HAS_PRESSED ? n->pressed : mixc(n->background, 0xFF000000u, 0.15f);
        } else if (n->variant == V_PRIMARY) {
            bg = pressed ? t->accentPressed : mixc(t->accent, t->accentHover, h);
            if (!(n->has & HAS_COLOUR)) fg = t->accentText;
            ui_round_shadow(c, x, y + 2, ww, hh, r, 6, with_alpha(t->accent, 0.25f + 0.15f * h));
        } else if (n->variant == V_GHOST) {
            bg = pressed ? t->surfaceActive : with_alpha(t->surfaceHover, h);
        } else {
            bg = pressed ? t->surfaceActive : mixc(t->control, t->controlHover, h);
            border = t->border;
        }
        if (disabled) { bg = n->variant == V_GHOST ? 0 : t->control; fg = t->textFaint; }
        if (bg >> 24) ui_round_fill(c, x, y, ww, hh, r, bg);
        if (border) ui_round_stroke(c, x, y, ww, hh, r, 1, border);
        float lh = ui_line_height(n->size, 1, NULL), lw = n->text.n ? measure(n->text.p, n->text.n, n->size, 1) : 0;
        float icon_w = n->icon.n ? 18 + (n->text.n ? 8 : 0) : 0, cx = x + (ww - lw - icon_w) / 2, dy = pressed ? 1 : 0;
        if (n->icon.n) { draw_icon(c, n->icon, cx, y + (hh - 18) / 2 + dy, 18, fg); cx += icon_w; }
        if (n->text.n) ui_text(c, roundf(cx), roundf(y + (hh - lh) / 2) + dy, n->text.p, n->text.n, n->size, 1, fg);
        return;
    }
    case K_SEGMENTED: {
        float lh = ui_line_height(n->size, 1, NULL);
        ui_round_fill(c, x, y, ww, hh, t->controlRadius + 2, t->control);
        ui_round_stroke(c, x, y, ww, hh, t->controlRadius + 2, 1, t->border);
        int sel = n->selected >= 0 && n->selected < (float)n->nopts ? (int)n->selected : -1;
        if (sel >= 0) {
            float bx, bw;
            seg_box(w, n, (uint32_t)sel, &bx, &bw);
            float tx = animate(w, n, 'x', bx - x, 160), tw = animate(w, n, 'w', bw, 160);
            ui_round_shadow(c, x + tx, y + 4, tw, hh - 6, t->controlRadius, 4, 0x40000000u);
            ui_round_fill(c, x + tx, y + 3, tw, hh - 6, t->controlRadius, disabled ? t->controlHover : t->accent);
        }
        for (uint32_t i = 0; i < n->nopts; i++) {
            float bx, bw;
            seg_box(w, n, i, &bx, &bw);
            bool item_hot = w->hot == seg_id(n->id, i) && !disabled;
            if ((int)i != sel && item_hot) ui_round_fill(c, bx + 1, y + 3, bw - 2, hh - 6, t->controlRadius, t->controlHover);
            uint32_t fg = (int)i == sel ? (disabled ? t->textDim : t->accentText)
                        : disabled ? t->textFaint : item_hot ? t->text : t->textDim;
            struct str o = w->strs[n->opt + i];
            float lw = measure(o.p, o.n, n->size, 1);
            ui_text(c, roundf(bx + (bw - lw) / 2), roundf(y + (hh - lh) / 2), o.p, o.n, n->size, 1, fg);
        }
        return;
    }
    case K_TOGGLE: {
        float on = animate(w, n, 'o', n->on ? 1 : 0, 140), h = animate(w, n, 'h', hot ? 1 : 0, 110);
        uint32_t track = disabled ? t->control : mixc(mixc(t->control, t->controlHover, h), t->accent, on);
        ui_round_fill(c, x, y, ww, hh, hh / 2, track);
        if (on < 1) ui_round_stroke(c, x, y, ww, hh, hh / 2, 1, with_alpha(t->border, 1 - on));
        float k = hh - 6, kx = x + 3 + (ww - 6 - k) * on;
        ui_round_shadow(c, kx, y + 4, k, k, k / 2, 3, 0x50000000u);
        ui_round_fill(c, kx, y + 3, k, k, k / 2, disabled ? t->textFaint : 0xFFFFFFFFu);
        return;
    }
    case K_CHECKBOX: {
        float on = animate(w, n, 'o', n->on ? 1 : 0, 120), h = animate(w, n, 'h', hot ? 1 : 0, 110);
        float by = y + (hh - 18) / 2;
        ui_round_fill(c, x, by, 18, 18, 5, mixc(mixc(t->control, t->controlHover, h), t->accent, on));
        if (on < 1) ui_round_stroke(c, x, by, 18, 18, 5, 1.2f, with_alpha(t->textFaint, 1 - on));
        if (on > 0) {
            uint32_t mark = with_alpha(0xFFFFFF, on);
            ui_line(c, x + 4.5f, by + 9.5f, x + 7.8f, by + 12.8f, 2, mark);
            ui_line(c, x + 7.8f, by + 12.8f, x + 13.5f, by + 5.8f, 2, mark);
        }
        float lh = ui_line_height(t->fontSize, 0, NULL);
        ui_text(c, x + 26, roundf(y + (hh - lh) / 2), n->text.p, n->text.n, t->fontSize, 0, disabled ? t->textFaint : t->text);
        return;
    }
    case K_SLIDER: case K_PROGRESS: {
        bool slider = n->kind == K_SLIDER;
        float value = fminf(n->max, fmaxf(n->min, n->value));
        float f = n->max > n->min ? (value - n->min) / (n->max - n->min) : 0;
        float knob = slider ? 18 : 0, th = slider ? 6 : hh, ty = y + (hh - th) / 2, left = x + knob / 2, span = ww - knob;
        ui_round_fill(c, x, ty, ww, th, th / 2, t->control);
        float shown = slider && w->dragging == n->id ? f : animate(w, n, 'v', f, 120);
        if (shown > 0) ui_round_fill(c, x, ty, left - x + span * shown, th, th / 2, disabled ? t->textFaint : t->accent);
        if (slider) {
            float h = animate(w, n, 'h', hot || w->dragging == n->id ? 1 : 0, 110);
            float kx = left + span * shown - knob / 2, ky = y + (hh - knob) / 2;
            if (h > 0)
                ui_round_fill(c, kx - 5 * h, ky - 5 * h, knob + 10 * h, knob + 10 * h, knob / 2 + 5 * h,
                              with_alpha(t->accent, 0.22f * h));
            ui_round_shadow(c, kx, ky + 1, knob, knob, knob / 2, 4, 0x60000000u);
            ui_round_fill(c, kx, ky, knob, knob, knob / 2, 0xFFFFFFFFu);
        }
        return;
    }
    case K_FIELD: {
        bool focused = w->focus == n->id;
        float h = animate(w, n, 'h', hot ? 1 : 0, 110), f = animate(w, n, 'f', focused ? 1 : 0, 120);
        ui_round_fill(c, x, y, ww, hh, t->controlRadius, mixc(t->control, t->controlHover, h * (1 - f)));
        ui_round_stroke(c, x, y, ww, hh, t->controlRadius, 1 + f * 0.5f, mixc(t->border, t->accent, f));
        float lh = ui_line_height(t->fontSize, n->weight, NULL), ty = roundf(y + (hh - lh) / 2);
        float seen[4] = {fmaxf(clip[0], x + 10), clip[1], fminf(clip[2], x + ww - 10), clip[3]};
        set_clip(c, seen);
        /* the focused field shows the engine's copy of its text, which may be ahead of the view */
        struct str src = focused ? (struct str){w->edit, w->edit_n} : n->text, shown = src;
        char *dots = NULL;
        if (n->secret && src.n) {
            uint32_t chars = 0;
            for (uint32_t i = 0; i < src.n; i++) chars += ((uint8_t)src.p[i] & 0xC0) != 0x80;
            dots = malloc((size_t)chars * 3);
            for (uint32_t i = 0; dots && i < chars; i++) memcpy(dots + i * 3, "\xE2\x80\xA2", 3);
            shown = dots ? (struct str){dots, chars * 3} : (struct str){"", 0};
        }
        float room = ww - 24, shift = 0, caret = 0;
        if (focused) {
            /* scrolled just enough to keep the caret in sight */
            struct state *st = state_of(w, n->id);
            float total = field_x(n, src.p, src.n);
            caret = field_x(n, src.p, w->caret);
            shift = st ? st->shift : 0;
            if (caret - shift > room) shift = caret - room;
            if (caret < shift) shift = caret;
            shift = fmaxf(0, fminf(shift, total - room));
            if (st) st->shift = shift;
            if (w->anchor != w->caret) {
                float a = field_x(n, src.p, w->anchor);
                float x0 = fminf(a, caret), x1 = fmaxf(a, caret);
                fill_rect(c, x + 12 - shift + x0, ty, x1 - x0, lh, with_alpha(t->accent, 0.35f));
            }
        }
        if (shown.n) ui_text(c, x + 12 - shift, ty, shown.p, shown.n, t->fontSize, n->weight, t->text);
        else if (n->placeholder.n) ui_text(c, x + 12, ty, n->placeholder.p, n->placeholder.n, t->fontSize, n->weight, t->textFaint);
        if (focused && w->caret_on && w->anchor == w->caret)
            fill_rect(c, x + 12 - shift + caret, ty + 1, 1.5f, lh - 2, t->accent);
        free(dots);
        set_clip(c, clip);
        return;
    }
    case K_ITEM: {
        bool selected = n->selected > 0;
        float h = animate(w, n, 'h', hot ? 1 : 0, 110), s = animate(w, n, 's', selected ? 1 : 0, 140);
        uint32_t bg = mixc(with_alpha(t->surfaceHover, 0.75f * h), t->surfaceActive, s);
        if (bg >> 24) ui_round_fill(c, x, y, ww, hh, t->controlRadius, pressed ? t->surfaceActive : bg);
        if (s > 0) ui_round_fill(c, x + 2, y + hh / 2 - 8 * s, 3, 16 * s, 1.5f, t->accent);
        float lx = x + 14;
        uint32_t fg = mixc(t->textDim, t->text, fmaxf(s, h));
        if (n->icon.n) { draw_icon(c, n->icon, lx, y + (hh - 18) / 2, 18, selected ? t->accent : fg); lx += 28; }
        float lh = ui_line_height(t->fontSize, 0, NULL);
        ui_text(c, lx, roundf(y + (hh - lh) / 2), n->text.p, n->text.n, t->fontSize, selected, fg);
        return;
    }
    case K_ICON:
        draw_icon(c, n->icon, x, y, fminf(ww, hh), n->has & HAS_COLOUR ? n->colour : t->textDim);
        return;
    case K_DIVIDER:
        fill_rect(c, x, y, ww, hh, n->has & HAS_COLOUR ? n->colour : t->divider);
        return;
    case K_CANVAS: {
        float seen[4] = {fmaxf(clip[0], x), fmaxf(clip[1], y), fminf(clip[2], x + ww), fminf(clip[3], y + hh)};
        set_clip(c, seen);
        paint_ops(c, n->draw, x, y);
        set_clip(c, clip);
        ask_size(w, n, n->drawn_w, n->drawn_h);
        return;
    }
    case K_SURFACE: {
        struct surface *sf = surface_of(w->s, n->surface);
        if (!sf) { ask_size(w, n, 0, 0); return; } /* not made yet: the app learns the size to make it */
        int sx = (int)x, sy = (int)y;
        int x0 = (int)fmaxf(clip[0], x), y0 = (int)fmaxf(clip[1], y);
        int x1 = (int)fminf(fminf(clip[2], x + ww), (float)(sx + sf->c.w));
        int y1 = (int)fminf(fminf(clip[3], y + hh), (float)(sy + sf->c.h));
        copy_surface(w, sf, sx, sy, x0, y0, x1, y1);
        ask_size(w, n, (float)sf->c.w, (float)sf->c.h);
        return;
    }
    }
}

/* -- input -- */
struct hit { struct node *n; uint64_t id; int index; };

static bool inside(const struct node *n, float x, float y) {
    return x >= n->x && y >= n->y && x < n->x + n->w && y < n->y + n->h && x >= n->vis[0] && y >= n->vis[1] &&
           x < n->vis[2] && y < n->vis[3];
}

/* The widget under the pointer that takes input, and the option within it. */
static struct hit hit_test(struct uiwin *w, float x, float y) {
    for (uint32_t i = w->n; i-- > 0;) {
        struct node *n = &w->nodes[i];
        if (!inside(n, x, y)) continue;
        if (n->kind == K_SEGMENTED && n->nopts) {
            if (n->disabled) return (struct hit){0};
            for (uint32_t j = 0; j < n->nopts; j++) {
                float bx, bw;
                seg_box(w, n, j, &bx, &bw);
                if (x < bx + bw || j == n->nopts - 1) return (struct hit){n, seg_id(n->id, j), (int)j};
            }
        }
        bool takes = n->kind == K_BUTTON || n->kind == K_TOGGLE || n->kind == K_CHECKBOX || n->kind == K_SLIDER ||
                     n->kind == K_FIELD || n->kind == K_ITEM || n->clickable || n->pointer;
        if (takes) return n->disabled ? (struct hit){0} : (struct hit){n, n->id, -1};
    }
    return (struct hit){0};
}

static struct node *find(struct uiwin *w, uint64_t id) {
    if (!id) return NULL;
    for (uint32_t i = 0; i < w->n; i++)
        if (w->nodes[i].id == id) return &w->nodes[i];
    return NULL;
}

static void sync_hot(struct uiwin *w) {
    uint64_t id = w->dragging ? w->dragging : hit_test(w, w->px, w->py).id;
    if (id != w->hot) { w->hot = id; w->dirty = true; }
}

static void slide(struct uiwin *w, struct node *n, float x, bool done) {
    float knob = 18, f = n->w > knob ? clamp01((x - n->x - knob / 2) / (n->w - knob)) : 0;
    float v = n->min + f * (n->max - n->min);
    if (n->step > 0) v = roundf(v / n->step) * n->step;
    v = fminf(n->max, fmaxf(n->min, v));
    emit(w, UI_EVENT_SLIDE, n, 0, done ? UI_DONE : 0, v, 0, NULL, 0);
}

static void end_drag(struct uiwin *w) {
    struct node *n = find(w, w->dragging);
    w->dragging = 0;
    if (n && n->kind == K_SLIDER) slide(w, n, w->px, true);
}

static size_t utf8_put(char *out, uint32_t cp) {
    if (cp < 0x80) { out[0] = (char)cp; return 1; }
    if (cp < 0x800) { out[0] = (char)(0xC0 | cp >> 6); out[1] = (char)(0x80 | (cp & 63)); return 2; }
    if (cp < 0x10000) {
        out[0] = (char)(0xE0 | cp >> 12); out[1] = (char)(0x80 | (cp >> 6 & 63)); out[2] = (char)(0x80 | (cp & 63));
        return 3;
    }
    out[0] = (char)(0xF0 | cp >> 18); out[1] = (char)(0x80 | (cp >> 12 & 63));
    out[2] = (char)(0x80 | (cp >> 6 & 63)); out[3] = (char)(0x80 | (cp & 63));
    return 4;
}

/* "Escape", "ArrowUp", "F5"... or the character typed (buf holds at least 8 bytes). */
static const char *key_name(uint32_t key, char *buf) {
    static const char *const named[] = {"ArrowUp", "ArrowDown", "ArrowLeft", "ArrowRight", "Home", "End",
                                        "PageUp", "PageDown", "Insert", "Delete"};
    switch (key) {
    case NKEY_ESC: return "Escape";
    case NKEY_BACKSPACE: return "Backspace";
    case NKEY_TAB: return "Tab";
    case NKEY_ENTER: return "Enter";
    }
    if (key >= NKEY_UP && key <= NKEY_DELETE) return named[key - NKEY_UP];
    if (key >= NKEY_F1 && key <= NKEY_F12) {
        int f = (int)(key - NKEY_F1) + 1;
        buf[0] = 'F';
        buf[1] = (char)(f >= 10 ? '1' : '0' + f);
        buf[2] = f >= 10 ? (char)('0' + f - 10) : 0;
        buf[3] = 0;
        return buf;
    }
    if ((key >= 32 && key < 0x100) || (key > 0x1FF && key <= 0x10FFFF)) {
        buf[utf8_put(buf, key)] = 0;
        return buf;
    }
    return "";
}

/* -- text fields -- */
static bool edit_reserve(struct uiwin *w, uint32_t need) {
    if (need <= w->edit_cap) return true;
    uint32_t cap = w->edit_cap ? w->edit_cap : 64;
    while (cap < need) {
        if (cap > UINT32_MAX / 2) return false;
        cap *= 2;
    }
    char *p = realloc(w->edit, cap);
    if (!p) return false;
    w->edit = p;
    w->edit_cap = cap;
    return true;
}

/* Words for Ctrl+arrows: letters, digits, '_' and anything beyond ASCII. */
static bool word_byte(uint8_t c) { return c >= 0x80 || c == '_' || (c >= '0' && c <= '9') || ((c | 32) >= 'a' && (c | 32) <= 'z'); }
static uint32_t word_left(const char *s, uint32_t i) {
    while (i > 0 && !word_byte((uint8_t)s[i - 1])) i--;
    while (i > 0 && word_byte((uint8_t)s[i - 1])) i--;
    return i;
}
static uint32_t word_right(const char *s, uint32_t n, uint32_t i) {
    while (i < n && !word_byte((uint8_t)s[i])) i++;
    while (i < n && word_byte((uint8_t)s[i])) i++;
    return i;
}

/* Takes the field's value as the app gave it into the edit buffer. */
static bool edit_take(struct uiwin *w, const struct node *n) {
    if (!edit_reserve(w, n->text.n + 1)) return false;
    memcpy(w->edit, n->text.p, n->text.n);
    w->edit_n = n->text.n;
    w->edit_app = hash_bytes(n->text.p, n->text.n);
    w->nsent = 0;
    if (w->caret > w->edit_n) w->caret = w->edit_n;
    if (w->anchor > w->edit_n) w->anchor = w->edit_n;
    w->caret = char_start(w->edit, w->caret);
    w->anchor = char_start(w->edit, w->anchor);
    return true;
}

/* Gives the keyboard to a field (NULL: to nobody), with all of its text selected or the caret at its end;
   lost: the flags of the focus event to the field that had it (UI_CANCEL: Escape gave it up). */
static void move_focus(struct uiwin *w, struct node *n, bool all, uint32_t lost) {
    if (n && w->focus == n->id) {
        if (all) { w->anchor = 0; w->caret = w->edit_n; w->dirty = true; }
        return;
    }
    if (!n && !w->focus) return;
    struct node *old = find(w, w->focus);
    w->focus = 0;
    w->selecting = false;
    if (old && old->kind == K_FIELD) emit(w, UI_EVENT_FOCUS, old, 0, lost, 0, 0, NULL, 0);
    w->caret = w->anchor = UINT32_MAX;
    if (n && edit_take(w, n)) {
        w->focus = n->id;
        if (!all) w->anchor = w->caret;
        else w->anchor = 0;
        emit(w, UI_EVENT_FOCUS, n, 0, 0, 1, 0, NULL, 0);
    }
    w->caret_on = true;
    w->caret_at = w->now + 530;
    w->dirty = true;
}
static void set_focus(struct uiwin *w, struct node *n, bool all) { move_focus(w, n, all, 0); }

/* A new view: the focused field may be gone, or the app may have changed its value. A value the
   engine sent and the app only sent back is not a change: later keys have already built on it. */
static void sync_focus(struct uiwin *w) {
    if (!w->focus) return;
    struct node *n = find(w, w->focus);
    if (!n || n->kind != K_FIELD || n->disabled) {
        w->focus = 0;
        w->selecting = false;
        return;
    }
    uint64_t h = hash_bytes(n->text.p, n->text.n);
    if (h == w->edit_app) return;
    w->edit_app = h;
    for (uint32_t i = 0; i < w->nsent; i++)
        if (w->sent[i] == h) {
            w->nsent -= i + 1;
            memmove(w->sent, w->sent + i + 1, w->nsent * sizeof *w->sent);
            return;
        }
    if (!edit_take(w, n)) set_focus(w, NULL, false);
}

/* Replaces the selection with text, puts the caret after it and tells the app the field's new value. */
static void field_replace(struct uiwin *w, struct node *n, const char *add, uint32_t an, uint32_t mods) {
    uint32_t a = w->caret < w->anchor ? w->caret : w->anchor, b = w->caret ^ w->anchor ^ a;
    uint32_t len = w->edit_n - (b - a);
    if (an > UINT32_MAX - 1 - len || !edit_reserve(w, len + an + 1)) return;
    memmove(w->edit + a + an, w->edit + b, w->edit_n - b);
    memcpy(w->edit + a, add, an);
    w->edit_n = len + an;
    w->caret = w->anchor = a + an;
    if (w->nsent == 8) memmove(w->sent, w->sent + 1, (--w->nsent) * sizeof *w->sent);
    w->sent[w->nsent++] = hash_bytes(w->edit, w->edit_n);
    emit(w, UI_EVENT_TEXT, n, mods, 0, 0, 0, w->edit, w->edit_n);
}

/* Ctrl+V: the clipboard's text, on one line. */
static void field_paste(struct uiwin *w, struct node *n, uint32_t mods) {
    int need = clipboard_get(NULL, 0);
    if (need <= 0 || need == INT_MAX) return;
    char *clip = malloc((size_t)need + 1);
    if (!clip) return;
    int got = clipboard_get(clip, (size_t)need + 1);
    if (got > 0 && got <= need) {
        for (int i = 0; i < got; i++)
            if ((uint8_t)clip[i] < 32) clip[i] = ' ';
        field_replace(w, n, clip, (uint32_t)got, mods);
    }
    free(clip);
}

/* A key pressed in the focused field; false when the field has no use for it (it then goes to the app). */
static bool field_key(struct uiwin *w, struct node *n, const struct gui_event *e) {
    uint32_t k = e->key, mods = e->mods, len = w->edit_n;
    bool ctrl = mods & NMOD_CTRL, shift = mods & NMOD_SHIFT;
    const char *s = w->edit;
    uint32_t lo = w->caret < w->anchor ? w->caret : w->anchor, hi = w->caret ^ w->anchor ^ lo;
    if (mods & NMOD_ALT) return false;
    if (ctrl && k >= 'A' && k <= 'Z') k += 32;
    switch (k) {
    case NKEY_ENTER: emit(w, UI_EVENT_SUBMIT, n, mods, 0, 0, 0, s, len); break;
    case NKEY_ESC: move_focus(w, NULL, false, UI_CANCEL); return true;
    case NKEY_LEFT: case NKEY_RIGHT: case NKEY_HOME: case NKEY_END: {
        uint32_t to;
        if (k == NKEY_HOME) to = 0;
        else if (k == NKEY_END) to = len;
        else if (lo != hi && !shift) to = k == NKEY_LEFT ? lo : hi; /* the selection collapses to that side */
        else if (k == NKEY_LEFT) to = ctrl ? word_left(s, w->caret) : prev_char(s, w->caret);
        else to = ctrl ? word_right(s, len, w->caret) : next_char(s, len, w->caret);
        w->caret = to;
        if (!shift) w->anchor = to;
        break;
    }
    case NKEY_BACKSPACE: case NKEY_DELETE:
        if (lo == hi) {
            if (k == NKEY_BACKSPACE) w->anchor = ctrl ? word_left(s, w->caret) : prev_char(s, w->caret);
            else w->anchor = ctrl ? word_right(s, len, w->caret) : next_char(s, len, w->caret);
        }
        if (w->anchor != w->caret) field_replace(w, n, "", 0, mods);
        break;
    default:
        if (ctrl && k == 'a') { w->anchor = 0; w->caret = len; }
        else if (ctrl && (k == 'c' || k == 'x')) {
            if (lo == hi || n->secret) break;
            clipboard_set(s + lo, hi - lo);
            if (k == 'x') field_replace(w, n, "", 0, mods);
        } else if (ctrl && k == 'v') field_paste(w, n, mods);
        else if (!ctrl && ((k >= 32 && k < 0x100 && k != 127) || (k > 0x1FF && k <= 0x10FFFF))) {
            char ch[4];
            field_replace(w, n, ch, (uint32_t)utf8_put(ch, k), mods);
        } else return false;
    }
    w->caret_on = true;
    w->caret_at = w->now + 530;
    w->dirty = true;
    return true;
}

/* The byte offset in the focused field's text nearest to x (in the window). */
static uint32_t field_offset(struct uiwin *w, struct node *n, float x) {
    struct state *st = state_of(w, n->id);
    float pos = x - (n->x + 12) + (st ? st->shift : 0), at = 0;
    for (uint32_t i = 0; i < w->edit_n;) {
        uint32_t j = next_char(w->edit, w->edit_n, i);
        float cw = field_x(n, w->edit + i, j - i);
        if (pos < at + cw / 2) return i;
        at += cw;
        i = j;
    }
    return w->edit_n;
}

/* The pointer over (or held on) a canvas or surface that asked for it, in its own coordinates. */
static void pointer(struct uiwin *w, struct node *n, uint32_t action, const struct gui_event *e) {
    emit(w, UI_EVENT_POINTER, n, e->mods, action | (e->buttons & 0xFF) << 8, w->px - n->x, w->py - n->y, NULL, 0);
}

static void win_input(struct uiwin *w, const struct gui_event *e) {
    w->now = uptime_ms();
    switch (e->type) {
    case EV_MOUSE_MOVE: {
        w->px = (float)e->x;
        w->py = (float)e->y;
        struct node *held = find(w, w->pressed), *target = held && held->pointer ? held : NULL;
        if (!held) {
            struct hit h = hit_test(w, w->px, w->py);
            if (h.n && h.n->pointer) target = h.n;
        }
        if (w->over && (!target || target->id != w->over)) {
            struct node *left = find(w, w->over);
            if (left) pointer(w, left, 0, e); /* a position outside it: the pointer has left */
        }
        w->over = target ? target->id : 0;
        if (target) pointer(w, target, 0, e);
        struct node *n = find(w, w->dragging);
        if (w->selecting && w->pressed == w->focus) {
            struct node *f = find(w, w->focus);
            uint32_t at = f ? field_offset(w, f, w->px) : w->caret;
            if (at != w->caret) { w->caret = at; w->dirty = true; }
        }
        if (n && n->kind == K_SLIDER) slide(w, n, w->px, false);
        else if (n && n->kind == K_SCROLL && n->h > 0) {
            struct state *st = state_of(w, n->id);
            if (st) st->scroll = st->drag_from + (w->py - st->drag_y) * (n->h + n->scroll_max) / n->h;
            w->dirty = true;
        }
        sync_hot(w);
        break;
    }
    case EV_MOUSE_DOWN: {
        w->px = (float)e->x;
        w->py = (float)e->y;
        struct node *held = find(w, w->pressed);
        struct hit h = held && held->pointer ? (struct hit){held, held->id, -1} : hit_test(w, w->px, w->py);
        if (h.n && h.n->pointer) {
            /* every button goes to it, and the pointer stays its until the last is let go */
            w->pressed = h.id;
            set_focus(w, NULL, false);
            pointer(w, h.n, UI_DOWN, e);
            break;
        }
        if (!(e->buttons & 1)) break;
        w->pressed = h.id;
        if (h.n && h.n->kind == K_FIELD) {
            /* a click places the caret (Shift: extends the selection) and a drag selects; a field with
               "selectOnFocus" selects all of its text on the click that focuses it */
            bool had = w->focus == h.id;
            if (!had) set_focus(w, h.n, h.n->select_on_focus);
            if (w->focus == h.id && (had || !h.n->select_on_focus)) {
                w->caret = field_offset(w, h.n, w->px);
                if (!had || !(e->mods & NMOD_SHIFT)) w->anchor = w->caret;
                w->selecting = true;
                w->caret_on = true;
                w->caret_at = w->now + 530;
            }
        } else set_focus(w, NULL, false);
        if (h.n && h.n->kind == K_SLIDER) { w->dragging = h.id; slide(w, h.n, w->px, false); }
        for (uint32_t i = w->n; !h.n && i-- > 0;) {
            struct node *n = &w->nodes[i];
            if (n->kind == K_SCROLL && n->scroll_max > 0 && inside(n, w->px, w->py) && w->px >= n->x + n->w - 12) {
                struct state *st = state_of(w, n->id);
                if (st) {
                    w->dragging = n->id;
                    st->drag_from = st->scroll;
                    st->drag_y = w->py;
                }
                break;
            }
        }
        w->dirty = true;
        break;
    }
    case EV_MOUSE_UP: {
        w->px = (float)e->x;
        w->py = (float)e->y;
        struct node *held = find(w, w->pressed);
        if (held && held->pointer) {
            pointer(w, held, UI_UP, e);
            if (!(e->buttons & 0xFF)) w->pressed = 0;
            sync_hot(w);
            break;
        }
        struct hit h = hit_test(w, w->px, w->py);
        uint64_t pressed = w->pressed;
        w->pressed = 0;
        w->selecting = false;
        if (w->dragging) end_drag(w);
        else if (h.n && h.id == pressed) {
            struct node *n = h.n;
            if (n->kind == K_SEGMENTED) {
                if ((float)h.index != n->selected) emit(w, UI_EVENT_CHOOSE, n, e->mods, 0, h.index, 0, NULL, 0);
            } else if (n->kind == K_TOGGLE || n->kind == K_CHECKBOX) {
                emit(w, UI_EVENT_TOGGLE, n, e->mods, 0, n->on ? 0 : 1, 0, NULL, 0);
            } else if (n->kind != K_FIELD && n->kind != K_SLIDER) {
                emit(w, UI_EVENT_CLICK, n, e->mods, 0, 0, 0, NULL, 0);
            }
        }
        sync_hot(w);
        w->dirty = true;
        break;
    }
    case EV_WHEEL:
        for (uint32_t i = w->n; i-- > 0;) {
            struct node *n = &w->nodes[i];
            if (!inside(n, (float)e->x, (float)e->y) || n->disabled) continue;
            if (n->pointer) {
                emit(w, UI_EVENT_WHEEL, n, e->mods, 0, e->wheel, 0, NULL, 0);
                return; /* the app draws what it changes */
            }
            if (n->kind == K_SLIDER) {
                float step = !isnan(n->wheel_step) ? n->wheel_step : n->step > 0 ? n->step : (n->max - n->min) / 20;
                float v = fminf(n->max, fmaxf(n->min, n->value - (float)e->wheel * step));
                emit(w, UI_EVENT_SLIDE, n, e->mods, UI_DONE, v, 0, NULL, 0);
                break;
            }
            if (n->kind == K_SCROLL && n->scroll_max > 0) {
                struct state *st = state_of(w, n->id);
                if (st) st->scroll += (float)e->wheel * 48;
                break;
            }
        }
        w->dirty = true;
        break;
    case EV_KEY: {
        char buf[8];
        const char *name = key_name(e->key, buf);
        struct node *field = find(w, w->focus);
        bool field_on = field && field->kind == K_FIELD;
        /* the keys a field takes are its own, presses and releases; the rest go to the app */
        uint32_t i = 0;
        while (i < w->nkeys && w->keys[i] != e->key) i++;
        if (e->pressed) {
            if (field_on && field_key(w, field, e)) break;
            if (i == w->nkeys && w->nkeys < sizeof w->keys / sizeof *w->keys) w->keys[w->nkeys++] = e->key;
        } else if (i < w->nkeys) {
            w->keys[i] = w->keys[--w->nkeys];
        } else if (field_on) {
            break;
        }
        emit(w, UI_EVENT_KEY, NULL, e->mods, e->pressed ? 0 : UI_UP, e->key, 0, name, strlen(name));
        break;
    }
    case EV_FOCUS:
        emit(w, UI_EVENT_FOCUS, NULL, 0, 0, 1, 0, NULL, 0);
        break;
    case EV_UNFOCUS: {
        emit(w, UI_EVENT_FOCUS, NULL, 0, 0, 0, 0, NULL, 0);
        w->nkeys = 0;
        w->selecting = false;
        if (w->dragging) end_drag(w);
        struct node *held = find(w, w->pressed);
        if (held && held->pointer) emit(w, UI_EVENT_POINTER, held, 0, UI_UP, w->px - held->x, w->py - held->y, NULL, 0);
        w->pressed = 0;
        w->px = w->py = -1;
        sync_hot(w);
        w->dirty = true;
        break;
    }
    case EV_RESIZE:
        emit(w, UI_EVENT_RESIZE, NULL, 0, 0, w->win->w, w->win->h, NULL, 0);
        w->shown_px = NULL; /* painted whole */
        w->dirty = true;
        break;
    case EV_CLOSE:
        emit(w, UI_EVENT_CLOSE, NULL, 0, 0, 0, 0, NULL, 0);
        break;
    case EV_DIRECT:
        emit(w, UI_EVENT_DIRECT, NULL, 0, 0, e->key ? 1 : 0, 0, NULL, 0);
        if (!e->key) {
            w->shown_px = NULL; /* the window is shown again: painted whole */
            w->dirty = true;
        }
        break;
    case EV_FRAME: /* also after pictures of widgets alone, which nobody waits for */
        if (w->frame_wanted) {
            w->frame_wanted = false;
            emit(w, UI_EVENT_FRAME, NULL, 0, 0, e->y / 1000.0, 0, NULL, 0);
        }
        break;
    }
}

/* What a view asks for by changing a field's "focus" or a node's "reveal"; true when a scroll box moved. */
static bool requests(struct uiwin *w, struct node *n, struct node *scroll) {
    bool moved = false;
    if (!isnan(n->focus_req) && n->kind == K_FIELD) {
        struct state *st = state_of(w, n->id);
        if (st && st->focus_seen != n->focus_req) {
            st->focus_seen = n->focus_req;
            if (n->focus_req > 0 && !n->disabled) set_focus(w, n, n->select_on_focus);
            else if (w->focus == n->id) set_focus(w, NULL, false);
        }
    }
    if (!isnan(n->reveal) && scroll) {
        struct state *st = state_of(w, n->id);
        if (st && st->reveal_seen != n->reveal) {
            st->reveal_seen = n->reveal;
            float top = scroll->y + scroll->pad[0], bottom = scroll->y + scroll->h - scroll->pad[2];
            struct state *ss = state_of(w, scroll->id);
            if (ss && n->y < top) { ss->scroll -= top - n->y; moved = true; }
            else if (ss && n->y + n->h > bottom) { ss->scroll += fminf(n->y + n->h - bottom, n->y - top); moved = true; }
        }
    }
    for (uint32_t c = n->first; c; c = w->nodes[c].next)
        moved |= requests(w, &w->nodes[c], n->kind == K_SCROLL ? n : scroll);
    return moved;
}

/* Gives every node its box and identity, then acts on what the view asked for. */
static void layout(struct uiwin *w) {
    if (!w->n || !w->win) return;
    float clip[4] = {0, 0, (float)w->win->w, (float)w->win->h};
    place(w, &w->nodes[0], 0, 0, clip[2], clip[3], 0x726F6F74, 0, clip);
    sync_focus(w);
    if (requests(w, &w->nodes[0], NULL)) place(w, &w->nodes[0], 0, 0, clip[2], clip[3], 0x726F6F74, 0, clip);
}

/* -- what changed --
   A picture repaints only the boxes of the nodes whose signature changed (or that came or went),
   with what is around them painted again under a clip, so it comes out as a whole repaint would. */
static void add_damage(float *d, float x0, float y0, float x1, float y1) {
    if (x0 >= x1 || y0 >= y1) return;
    if (d[0] >= d[2]) { d[0] = x0, d[1] = y0, d[2] = x1, d[3] = y1; return; }
    d[0] = fminf(d[0], x0), d[1] = fminf(d[1], y0), d[2] = fmaxf(d[2], x1), d[3] = fmaxf(d[3], y1);
}

static uint64_t mix_float(uint64_t h, float f) {
    uint32_t u;
    memcpy(&u, &f, sizeof u);
    return mix(h, u);
}
static uint64_t mix_str(uint64_t h, struct str s) { return mix(h, hash_bytes(s.p, s.n)); }

static const struct state *state_find(const struct uiwin *w, uint64_t id) {
    if (!w->capstates) return NULL;
    uint32_t j = (uint32_t)id & (w->capstates - 1);
    while (w->states[j].id && w->states[j].id != id) j = (j + 1) & (w->capstates - 1);
    return w->states[j].id ? &w->states[j] : NULL;
}

static struct shown *shown_slot(struct shown *t, uint32_t cap, uint64_t id) {
    uint32_t j = (uint32_t)id & (cap - 1);
    while (t[j].id && t[j].id != id) j = (j + 1) & (cap - 1);
    return &t[j];
}

/* All that paint() reads for a node, but for its animations: its properties (node_init zeroes
   the padding), its place, the part of the window it is seen in, the pointer and the keyboard on
   it, its scroll and the surface it shows. */
static uint64_t node_sig(struct uiwin *w, const struct node *n) {
    uint64_t h = hash_bytes(n, offsetof(struct node, text));
    h = mix_str(mix_str(mix_str(mix_str(h, n->text), n->icon), n->placeholder), n->draw);
    for (uint32_t i = 0; i < n->nopts; i++) h = mix_str(h, w->strs[n->opt + i]);
    h = mix(h, n->key);
    const float geometry[] = {n->x, n->y, n->w, n->h, n->scroll_max, n->vis[0], n->vis[1], n->vis[2], n->vis[3]};
    h = mix(h, hash_bytes(geometry, sizeof geometry));
    h = mix(h, (uint64_t)(w->hot == n->id) | (uint64_t)(w->pressed == n->id) << 1 |
                   (uint64_t)(w->dragging == n->id) << 2 | (uint64_t)(w->focus == n->id) << 3);
    if (w->focus == n->id) {
        h = mix(h, hash_bytes(w->edit, w->edit_n));
        h = mix(h, (uint64_t)w->caret << 32 | w->anchor);
        h = mix(h, w->caret_on);
    }
    const struct state *st = state_find(w, n->id);
    if (st) h = mix_float(mix_float(h, st->scroll), st->shift);
    if (n->kind == K_SURFACE) {
        const struct surface *sf = surface_of(w->s, n->surface);
        h = mix(h, sf ? (uint64_t)(uintptr_t)sf->c.px ^ (uint64_t)sf->c.w << 32 ^ (uint64_t)sf->c.h : 0);
    }
    return h;
}

/* Records what the nodes now look like and puts the boxes that changed into w->damage. Every
   animation of a node still there counts towards its signature and is kept (seen). False when
   it cannot keep the record (the whole window is then painted). */
static bool find_damage(struct uiwin *w) {
    uint32_t cap = 16;
    while (cap < w->n * 2 + 2) cap *= 2;
    struct shown *t = calloc(cap, sizeof *t);
    if (!t) {
        free(w->shown);
        w->shown = NULL;
        w->capshown = 0;
        return false;
    }
    /* a margin for what a node paints around its box: shadows, anti-aliased edges */
    const float m = 20;
    for (uint32_t i = 0; i < w->n; i++) {
        const struct node *n = &w->nodes[i];
        struct shown *e = shown_slot(t, cap, n->id);
        e->id = n->id;
        e->sig = mix(e->sig, node_sig(w, n));
        e->box[0] = fmaxf(n->x - m, n->vis[0]);
        e->box[1] = fmaxf(n->y - m, n->vis[1]);
        e->box[2] = fminf(n->x + n->w + m, n->vis[2]);
        e->box[3] = fminf(n->y + n->h + m, n->vis[3]);
    }
    for (uint32_t i = 0; i < w->capanims; i++) {
        struct anim *a = &w->anims[i];
        if (!a->key) continue;
        struct shown *e = shown_slot(t, cap, a->node);
        if (!e->id) continue;
        e->sig = mix_float(e->sig, anim_value(a, w->now));
        if (a->from != a->to && (float)(w->now - a->start) < a->ms) w->animating = true;
        a->seen = true;
    }
    float *d = w->damage;
    for (uint32_t i = 0; i < cap; i++) {
        const struct shown *e = &t[i];
        if (!e->id) continue;
        const struct shown *was = w->shown ? shown_slot(w->shown, w->capshown, e->id) : NULL;
        if (was && was->id && was->sig == e->sig) continue;
        add_damage(d, e->box[0], e->box[1], e->box[2], e->box[3]);
        if (was && was->id) add_damage(d, was->box[0], was->box[1], was->box[2], was->box[3]);
    }
    for (uint32_t i = 0; i < w->capshown; i++) { /* the nodes that went */
        const struct shown *was = &w->shown[i];
        if (was->id && !shown_slot(t, cap, was->id)->id) add_damage(d, was->box[0], was->box[1], was->box[2], was->box[3]);
    }
    free(w->shown);
    w->shown = t;
    w->capshown = cap;
    return true;
}

static void render(struct uiwin *w) {
    canvas_t *c = &w->win->c;
    w->now = uptime_ms();
    w->dirty = false;
    w->hold = false;
    w->animating = false;
    if (win_begin_frame(w->win) < 0 || !c->px) { /* a double-buffered window waits for a free buffer: ask again soon, the picture is not lost */
        w->redraw_at = w->now + 8;
        return;
    }
    if (w->n) layout(w);
    w->deep = win_deep(w->win);
    /* a new canvas, a deep plane come or gone and the other buffer of a double-buffered window
       hold older pictures, so they are painted whole */
    bool whole = !find_damage(w) || c->px != w->shown_px || !!w->deep != w->shown_deep ||
                 (w->win->flags & WIN_DOUBLE_BUFFER);
    w->shown_px = c->px;
    w->shown_deep = w->deep;
    float *d = w->damage;
    float clip[4] = {0, 0, (float)w->win->w, (float)w->win->h};
    if (!whole) {
        if (d[0] >= d[2]) { /* nothing to paint */
            prune_anims(w);
            w->redraw_at = w->animating ? w->now + 16 : 0;
            sync_hot(w);
            return;
        }
        clip[0] = fmaxf(floorf(d[0]), 0), clip[1] = fmaxf(floorf(d[1]), 0);
        clip[2] = fminf(ceilf(d[2]), clip[2]), clip[3] = fminf(ceilf(d[3]), clip[3]);
        whole = clip[0] >= clip[2] || clip[1] >= clip[3] ||
                (clip[2] - clip[0]) * (clip[3] - clip[1]) * 4 > (float)w->win->w * (float)w->win->h * 3;
        if (whole) clip[0] = clip[1] = 0, clip[2] = (float)w->win->w, clip[3] = (float)w->win->h;
    }
    d[0] = d[2] = 0;
    int x0 = (int)clip[0], y0 = (int)clip[1], x1 = (int)clip[2], y1 = (int)clip[3];
    if (w->deep) /* widgets are 8-bit content */
        for (int y = y0; y < y1; y++) memset(w->deep + (size_t)y * (size_t)c->w + x0, 0, (size_t)(x1 - x0));
    set_clip(c, clip);
    gfx_fill(c, x0, y0, x1 - x0, y1 - y0, w->background);
    if (w->n) paint(w, &w->nodes[0], clip);
    prune_anims(w);
    gfx_noclip(c);
    /* a canvas that asked for its size is shown when the app has drawn it again (or does not answer);
       until then the record says nothing was shown */
    if (w->hold) w->shown_px = NULL;
    else if (whole) win_update(w->win);
    else win_update_rect(w->win, x0, y0, x1 - x0, y1 - y0);
    w->redraw_at = w->hold ? w->now + 100 : w->animating ? w->now + 16 : 0;
    sync_hot(w);
}

/* -- sessions -- */
struct ui_session *ui_session_new(void) {
    struct ui_session *s = calloc(1, sizeof *s);
    if (!s) return NULL;
    s->queue = n_events_create();
    if (s->queue < 0) { free(s); return NULL; }
    return s;
}

static struct uiwin *win_of(struct ui_session *s, uint32_t id) {
    for (uint32_t i = 0; i < s->nwins; i++)
        if (s->wins[i]->id == id) return s->wins[i];
    return NULL;
}

static void win_free(struct ui_session *s, struct uiwin *w) {
    for (uint32_t i = 0; i < s->nwins; i++)
        if (s->wins[i] == w) { s->wins[i] = s->wins[--s->nwins]; break; }
    n_events_unwatch(s->queue, w->id);
    if (w->win) win_close(w->win);
    free(w->tree);
    free(w->nodes);
    free(w->strs);
    free(w->states);
    free(w->anims);
    free(w->lines);
    free(w->edit);
    free(w->shown);
    free(w);
}

static void surface_free(struct surface *sf) {
    if (sf->c.px) n_memory_release(sf->c.px, sf->bytes);
    if (sf->shared >= 0) n_handle_close(sf->shared);
    free(sf);
}

void ui_session_free(struct ui_session *s) {
    if (!s) return;
    while (s->nwins) win_free(s, s->wins[0]);
    for (uint32_t i = 0; i < s->nsurfaces; i++) surface_free(s->surfaces[i]);
    while (s->head) ui_session_pop(s);
    n_handle_close(s->queue);
    free(s->wins);
    free(s->surfaces);
    free(s);
}

static void open_window(struct ui_session *s, uint32_t id, uint32_t width, uint32_t height, uint32_t flags,
                        uint32_t background, struct str title) {
    n_result result = 0;
    struct uiwin *w = NULL;
    char *name = malloc(title.n + 1);
    if (win_of(s, id)) result = -N_ERR_EXISTS;
    else if (!width || !height || width > 16384 || height > 16384) result = -N_ERR_INVALID;
    else if (!name || !grow_array((void **)&s->wins, &s->capwins, s->nwins + 1, sizeof *s->wins) ||
             !(w = calloc(1, sizeof *w))) result = -N_ERR_NO_MEMORY;
    else {
        memcpy(name, title.p, title.n);
        name[title.n] = 0;
        /* frame events come only after presents; they answer surface changes (UI_EVENT_FRAME) */
        result = win_open((int)width, (int)height, name, (int)(flags & UI_WINDOW_FLAGS) | WIN_FRAME_EVENTS, &w->win);
        if (result < 0) { free(w); w = NULL; }
    }
    free(name);
    if (w) {
        w->s = s;
        w->id = id;
        w->background = background ? background : ui_theme.background;
        w->px = w->py = -1;
        w->dirty = true;
        s->wins[s->nwins++] = w;
        n_events_watch(s->queue, w->win->handle, N_EVENT_RECEIVABLE, id);
    }
    struct ui_bytes m = {0};
    wr_u8(&m, 'o');
    wr_u32(&m, id);
    wr_u32(&m, (uint32_t)(int32_t)result);
    wr_u32(&m, w ? (uint32_t)w->win->w : 0);
    wr_u32(&m, w ? (uint32_t)w->win->h : 0);
    post(s, &m, true);
}

static n_result show_view(struct uiwin *w, const uint8_t *p, size_t n) {
    uint8_t *tree = malloc(n ? n : 1);
    if (!tree) return -N_ERR_NO_MEMORY;
    memcpy(tree, p, n);
    w->n = 0;
    w->nstrs = 0;
    struct rd r = {tree, tree + n, false};
    parse_node(w, &r, 0, 0);
    if (r.bad || r.p != r.end) {
        /* keep showing the last good view */
        free(tree);
        w->n = 0;
        w->nstrs = 0;
        if (w->tree) {
            struct rd again = {w->tree, w->tree + w->tree_n, false};
            parse_node(w, &again, 0, 0);
        }
        w->dirty = true;
        return -N_ERR_INVALID;
    }
    free(w->tree);
    w->tree = tree;
    w->tree_n = n;
    w->view++;
    w->dirty = true;
    layout(w); /* input that comes before the next picture finds the new nodes by identity */
    return 0;
}

static void set_surface(struct ui_session *s, uint32_t id, uint32_t width, uint32_t height, uint32_t flags) {
    struct surface *sf = surface_of(s, id);
    n_result result = 0;
    bool deep = flags & UI_SURFACE_DEEP;
    /* the request may come from another process (uihost): no surface of more than 2^27 pixels (512 MiB) */
    if (!width || !height || width > 16384 || height > 16384 || (uint64_t)width * height > (1u << 27) ||
        (flags & ~UI_SURFACE_DEEP)) result = -N_ERR_INVALID;
    else if (!sf || sf->c.w != (int)width || sf->c.h != (int)height || sf->deep != deep) {
        struct surface *fresh = calloc(1, sizeof *fresh);
        size_t bytes = ((size_t)width * height * (deep ? 5 : 4) + 4095) & ~(size_t)4095;
        n_handle shared = fresh ? n_shared_create(bytes) : -1;
        void *mapping = NULL;
        if (shared >= 0) n_shared_map(shared, bytes, N_MEMORY_READ_WRITE, &mapping);
        uint32_t *px = mapping;
        if (!px ||(!sf && !grow_array((void **)&s->surfaces, &s->capsurfaces, s->nsurfaces + 1, sizeof *s->surfaces))) {
            if (px) n_memory_release(px, bytes);
            if (shared >= 0) n_handle_close(shared);
            free(fresh);
            result = -N_ERR_NO_MEMORY;
        } else {
            fresh->id = id;
            fresh->shared = shared;
            fresh->bytes = bytes;
            fresh->deep = deep;
            gfx_init(&fresh->c, px, (int)width, (int)height, (int)width);
            if (sf) {
                for (uint32_t i = 0; i < s->nsurfaces; i++)
                    if (s->surfaces[i] == sf) s->surfaces[i] = fresh;
                surface_free(sf);
            } else {
                s->surfaces[s->nsurfaces++] = fresh;
            }
        }
    }
    struct ui_bytes m = {0};
    wr_u8(&m, 's');
    wr_u32(&m, id);
    wr_u32(&m, (uint32_t)(int32_t)result);
    wr_u32(&m, result ? 0 : width);
    wr_u32(&m, result ? 0 : height);
    post(s, &m, true);
}

/* Shows a part (x, y, width, height) of a surface the app drew again by copying only that part into the window,
   when the rest of the window's picture is up to date; false when the window has to be painted again instead. */
static bool show_surface(struct uiwin *w, const struct node *n, const struct surface *sf, const int *rect) {
    /* the other buffer of a double-buffered window holds an older picture, so it is painted whole */
    if (w->dirty || w->hold || !w->win || !w->win->c.px || (w->win->flags & WIN_DOUBLE_BUFFER)) return false;
    if ((int)lroundf(n->w) != sf->c.w || (int)lroundf(n->h) != sf->c.h) return false;
    for (uint32_t i = 0; i < w->n; i++) { /* the bar of a scroll box is painted over what it holds */
        const struct node *b = &w->nodes[i];
        if (b->kind == K_SCROLL && b->scroll_max > 0 && n->x < b->x + b->w && n->x + n->w > b->x + b->w - 12 &&
            n->y < b->y + b->h && n->y + n->h > b->y)
            return false;
    }
    if (rect[0] >= sf->c.w || rect[1] >= sf->c.h) return true;
    int sx = (int)n->x, sy = (int)n->y;
    /* the rectangle within the surface, then the part of it that can be seen */
    int x0 = sx + rect[0], y0 = sy + rect[1];
    int x1 = sx + (rect[2] < sf->c.w - rect[0] ? rect[0] + rect[2] : sf->c.w);
    int y1 = sy + (rect[3] < sf->c.h - rect[1] ? rect[1] + rect[3] : sf->c.h);
    x0 = (int)fmaxf(fmaxf((float)x0, n->vis[0]), 0);
    y0 = (int)fmaxf(fmaxf((float)y0, n->vis[1]), 0);
    x1 = (int)fminf(fminf((float)x1, n->vis[2]), (float)w->win->w);
    y1 = (int)fminf(fminf((float)y1, n->vis[3]), (float)w->win->h);
    w->deep = win_deep(w->win);
    copy_surface(w, sf, sx, sy, x0, y0, x1, y1);
    if (x1 > x0 && y1 > y0) win_update_rect(w->win, x0, y0, x1 - x0, y1 - y0);
    return true;
}

n_result ui_session_send(struct ui_session *s, const void *msg, size_t n) {
    struct rd r = {msg, (const uint8_t *)msg + n, false};
    uint8_t op = rd_u8(&r);
    switch (op) {
    case 'O': {
        uint32_t id = rd_u32(&r), width = rd_u32(&r), height = rd_u32(&r), flags = rd_u32(&r), bg = rd_u32(&r);
        struct str title = rd_str(&r);
        if (r.bad || r.p != r.end) return -N_ERR_INVALID;
        open_window(s, id, width, height, flags, bg, title);
        return 0;
    }
    case 'V': {
        struct uiwin *w = win_of(s, rd_u32(&r));
        if (r.bad) return -N_ERR_INVALID;
        return w ? show_view(w, r.p, (size_t)(r.end - r.p)) : 0; /* a window closed meanwhile */
    }
    case 'T': {
        struct uiwin *w = win_of(s, rd_u32(&r));
        struct str title = rd_str(&r);
        if (r.bad || r.p != r.end) return -N_ERR_INVALID;
        char *name = malloc(title.n + 1);
        if (!name) return -N_ERR_NO_MEMORY;
        memcpy(name, title.p, title.n);
        name[title.n] = 0;
        if (w) win_set_title(w->win, name);
        free(name);
        return 0;
    }
    case 'C': {
        struct uiwin *w = win_of(s, rd_u32(&r));
        if (r.bad || r.p != r.end) return -N_ERR_INVALID;
        if (w) win_free(s, w);
        return 0;
    }
    case 'F': {
        struct uiwin *w = win_of(s, rd_u32(&r));
        if (r.bad || r.p != r.end) return -N_ERR_INVALID;
        if (!w) return 0;
        n_result asked = win_frame(w->win);
        if (asked == 0) w->frame_wanted = true;
        return asked;
    }
    case 'M': {
        uint32_t id = rd_u32(&r);
        double size = rd_f64(&r);
        int weight = (int)(rd_u32(&r) & (UI_TEXT_BOLD | UI_TEXT_MONO));
        struct str text = rd_str(&r);
        if (r.bad || r.p != r.end || !(size > 0 && size <= 1000)) return -N_ERR_INVALID;
        float base, lh = ui_line_height((float)size, weight, &base);
        struct ui_bytes m = {0};
        wr_u8(&m, 'm');
        wr_u32(&m, id);
        wr_f64(&m, measure(text.p, text.n, (float)size, weight));
        wr_f64(&m, lh);
        wr_f64(&m, base);
        post(s, &m, true);
        return 0;
    }
    case 'Q': {
        if (r.p != r.end) return -N_ERR_INVALID;
        struct ui_bytes m = {0};
        uint32_t count = sizeof theme_values / sizeof *theme_values;
        wr_u8(&m, 'q');
        wr_u32(&m, count);
        for (uint32_t i = 0; i < count; i++) {
            size_t len = strlen(theme_values[i].name);
            wr_u8(&m, (uint8_t)len);
            wr_put(&m, theme_values[i].name, len);
            wr_f64(&m, theme_values[i].value);
        }
        post(s, &m, true);
        return 0;
    }
    case 'S': {
        uint32_t id = rd_u32(&r), width = rd_u32(&r), height = rd_u32(&r), flags = r.p != r.end ? rd_u32(&r) : 0;
        if (r.bad || r.p != r.end) return -N_ERR_INVALID;
        set_surface(s, id, width, height, flags);
        return 0;
    }
    case 'P': {
        uint32_t id = rd_u32(&r);
        int rect[4] = {0, 0, INT32_MAX, INT32_MAX};
        if (r.p != r.end)
            for (int i = 0; i < 4; i++) rect[i] = (int)rd_u32(&r);
        if (r.bad || r.p != r.end || rect[0] < 0 || rect[1] < 0 || rect[2] < 0 || rect[3] < 0) return -N_ERR_INVALID;
        struct surface *sf = surface_of(s, id);
        if (sf)
            for (uint32_t i = 0; i < s->nwins; i++) {
                struct uiwin *w = s->wins[i];
                for (uint32_t j = 0; j < w->n; j++) {
                    const struct node *n = &w->nodes[j];
                    if (n->kind != K_SURFACE || n->surface != id) continue;
                    w->frame_wanted = true;
                    if (show_surface(w, n, sf, rect)) continue;
                    /* the next picture paints the part that changed */
                    float x0 = n->x + (float)rect[0], y0 = n->y + (float)rect[1];
                    float x1 = n->x + fminf((float)rect[0] + (float)rect[2], n->w);
                    float y1 = n->y + fminf((float)rect[1] + (float)rect[3], n->h);
                    add_damage(w->damage, fmaxf(x0, n->vis[0]), fmaxf(y0, n->vis[1]), fminf(x1, n->vis[2]),
                               fminf(y1, n->vis[3]));
                    w->dirty = true;
                }
            }
        return 0;
    }
    case 'D': {
        uint32_t id = rd_u32(&r);
        if (r.bad || r.p != r.end) return -N_ERR_INVALID;
        for (uint32_t i = 0; i < s->nsurfaces; i++)
            if (s->surfaces[i]->id == id) {
                surface_free(s->surfaces[i]);
                s->surfaces[i] = s->surfaces[--s->nsurfaces];
                break;
            }
        return 0;
    }
    }
    return -N_ERR_INVALID;
}

canvas_t *ui_session_surface(struct ui_session *s, uint32_t id) {
    struct surface *sf = surface_of(s, id);
    return sf ? &sf->c : NULL;
}

window_t *ui_session_window(struct ui_session *s, uint32_t id) {
    struct uiwin *w = win_of(s, id);
    return w ? w->win : NULL;
}

uint8_t *ui_session_surface_deep(struct ui_session *s, uint32_t id) {
    struct surface *sf = surface_of(s, id);
    if (!sf || !sf->deep || display_bits_per_channel() < 10) return NULL;
    return (uint8_t *)(sf->c.px + (size_t)sf->c.w * (size_t)sf->c.h);
}

n_handle ui_session_surface_handle(struct ui_session *s, uint32_t id) {
    struct surface *sf = surface_of(s, id);
    return sf ? sf->shared : -N_ERR_NOT_FOUND;
}

const void *ui_session_peek(struct ui_session *s, size_t *n) {
    if (!s->head) return NULL;
    if (n) *n = s->head->n;
    return s->head->data;
}

void ui_session_pop(struct ui_session *s) {
    struct msg *m = s->head;
    if (!m) return;
    s->head = m->next;
    if (!s->head) s->tail = NULL;
    if (s->replies == m) s->replies = NULL;
    free(m);
}

n_handle ui_session_queue(struct ui_session *s) { return s->queue; }

bool ui_session_open(struct ui_session *s) { return s->nwins > 0; }

int ui_session_timeout(struct ui_session *s) {
    uint64_t now = uptime_ms();
    int64_t best = -1;
    for (uint32_t i = 0; i < s->nwins; i++) {
        struct uiwin *w = s->wins[i];
        int64_t t = -1;
        if (w->dirty) t = 0;
        else {
            if (w->redraw_at) t = w->redraw_at > now ? (int64_t)(w->redraw_at - now) : 0;
            if (w->focus) {
                int64_t c = w->caret_at > now ? (int64_t)(w->caret_at - now) : 0;
                if (t < 0 || c < t) t = c;
            }
        }
        if (t >= 0 && (best < 0 || t < best)) best = t;
    }
    return (int)best;
}

void ui_session_pump(struct ui_session *s) {
    for (uint32_t i = 0; i < s->nwins; i++) {
        struct uiwin *w = s->wins[i];
        struct gui_event e;
        int r;
        while ((r = win_event(w->win, &e, 0)) > 0) win_input(w, &e);
        if (r < 0) {
            /* the window is gone (its app is ending, or the desktop closed it) */
            emit(w, UI_EVENT_CLOSE, NULL, 0, UI_GONE, 0, 0, NULL, 0);
            w->win = NULL;
            win_free(s, w);
            i--;
            continue;
        }
        uint64_t now = uptime_ms();
        if (w->focus && now >= w->caret_at) {
            w->caret_on = !w->caret_on;
            w->caret_at = now + 530;
            w->dirty = true;
        }
        if (w->redraw_at && now >= w->redraw_at) w->dirty = true;
        if (w->dirty) render(w);
    }
}

bool ui_session_wait(struct ui_session *s, int timeout) {
    int t = ui_session_timeout(s);
    if (t < 0 || (timeout >= 0 && timeout < t)) t = timeout;
    if (t != 0 && s->nwins) {
        struct n_event ev[8];
        n_events_wait(s->queue, ev, 8, t);
    }
    ui_session_pump(s);
    return s->head != NULL;
}

/* -- the session from C -- */
void ui_view(struct ui_msg *m, uint32_t window) {
    *m = (struct ui_msg){0};
    wr_u8(&m->b, 'V');
    wr_u32(&m->b, window);
}

static struct ui_open *top(struct ui_msg *m) { return m->depth ? &m->open[m->depth - 1] : NULL; }

uint32_t ui_node(struct ui_msg *m, const char *kind) {
    struct ui_open *parent = top(m);
    size_t kl = strlen(kind);
    if (parent) parent->kids++;
    else if (m->nodes) m->b.bad = true; /* one root */
    if (kl > 255 || !grow_array((void **)&m->open, &m->capdepth, m->depth + 1, sizeof *m->open)) m->b.bad = true;
    if (m->b.bad) return m->nodes++;
    wr_u8(&m->b, (uint8_t)kl);
    wr_put(&m->b, kind, kl);
    m->open[m->depth++] = (struct ui_open){m->b.n, 0, 0};
    wr_put(&m->b, &(uint32_t){0}, 4); /* the counts, written by ui_end */
    return m->nodes++;
}

void ui_end(struct ui_msg *m) {
    struct ui_open *o = top(m);
    if (!o) { m->b.bad = true; return; }
    if (!m->b.bad) {
        memcpy(m->b.p + o->at, &o->props, 2);
        memcpy(m->b.p + o->at + 2, &o->kids, 2);
    }
    m->depth--;
}

/* The start of a property of the open node (which must not have children yet). */
static bool prop_head(struct ui_msg *m, const char *name, uint8_t type) {
    struct ui_open *o = top(m);
    size_t nl = strlen(name);
    if (!o || o->kids || nl > 255) m->b.bad = true;
    if (m->b.bad) return false;
    o->props++;
    wr_u8(&m->b, (uint8_t)nl);
    wr_put(&m->b, name, nl);
    wr_u8(&m->b, type);
    return true;
}

void ui_num(struct ui_msg *m, const char *name, double value) {
    if (prop_head(m, name, 1)) wr_f64(&m->b, value);
}

void ui_strn(struct ui_msg *m, const char *name, const char *s, size_t n) {
    if (prop_head(m, name, 2)) wr_str(&m->b, s, n);
}

void ui_str(struct ui_msg *m, const char *name, const char *s) { ui_strn(m, name, s, strlen(s)); }

void ui_nums(struct ui_msg *m, const char *name, const double *v, uint32_t n) {
    if (!prop_head(m, name, 3)) return;
    wr_u32(&m->b, n);
    wr_put(&m->b, v, (size_t)n * 8);
}

void ui_strs(struct ui_msg *m, const char *name, const char *const *v, uint32_t n) {
    if (!prop_head(m, name, 4)) return;
    wr_u32(&m->b, n);
    for (uint32_t i = 0; i < n; i++) wr_str(&m->b, v[i], strlen(v[i]));
}

void ui_blob(struct ui_msg *m, const char *name, const void *p, uint32_t n) {
    if (prop_head(m, name, 5)) wr_str(&m->b, p, n);
}

n_result ui_send(struct ui_session *s, struct ui_msg *m) {
    n_result r = m->b.bad || m->depth || !m->nodes ? -N_ERR_INVALID : ui_session_send(s, m->b.p, m->b.n);
    free(m->b.p);
    free(m->open);
    *m = (struct ui_msg){0};
    return r;
}

/* Sends a request and takes its reply (which is at the front of the queue). */
static struct rd request(struct ui_session *s, struct ui_bytes *w, uint8_t want) {
    n_result r = w->bad ? -N_ERR_NO_MEMORY : ui_session_send(s, w->p, w->n);
    free(w->p);
    size_t n;
    const uint8_t *p = r == 0 ? ui_session_peek(s, &n) : NULL;
    if (!p || p[0] != want) return (struct rd){NULL, NULL, true};
    return (struct rd){p + 1, p + n, false};
}

n_result ui_window(struct ui_session *s, uint32_t window, int width, int height, uint32_t flags, const char *title,
                   int *out_width, int *out_height) {
    struct ui_bytes w = {0};
    wr_u8(&w, 'O');
    wr_u32(&w, window);
    wr_u32(&w, (uint32_t)width);
    wr_u32(&w, (uint32_t)height);
    wr_u32(&w, flags);
    wr_u32(&w, 0);
    wr_str(&w, title, strlen(title));
    struct rd r = request(s, &w, 'o');
    if (r.bad) return -N_ERR_INVALID;
    rd_u32(&r);
    n_result result = (int32_t)rd_u32(&r);
    int ww = (int)rd_u32(&r), hh = (int)rd_u32(&r);
    ui_session_pop(s);
    if (out_width) *out_width = ww;
    if (out_height) *out_height = hh;
    return result;
}

void ui_window_close(struct ui_session *s, uint32_t window) {
    struct ui_bytes w = {0};
    wr_u8(&w, 'C');
    wr_u32(&w, window);
    if (!w.bad) ui_session_send(s, w.p, w.n);
    free(w.p);
}

n_result ui_window_frame(struct ui_session *s, uint32_t window) {
    struct ui_bytes w = {0};
    wr_u8(&w, 'F');
    wr_u32(&w, window);
    n_result asked = w.bad ? -N_ERR_NO_MEMORY : ui_session_send(s, w.p, w.n);
    free(w.p);
    return asked;
}

void ui_window_title(struct ui_session *s, uint32_t window, const char *title) {
    struct ui_bytes w = {0};
    wr_u8(&w, 'T');
    wr_u32(&w, window);
    wr_str(&w, title, strlen(title));
    if (!w.bad) ui_session_send(s, w.p, w.n);
    free(w.p);
}

canvas_t *ui_surface(struct ui_session *s, uint32_t surface, int width, int height, uint32_t flags) {
    struct ui_bytes w = {0};
    wr_u8(&w, 'S');
    wr_u32(&w, surface);
    wr_u32(&w, (uint32_t)width);
    wr_u32(&w, (uint32_t)height);
    wr_u32(&w, flags);
    struct rd r = request(s, &w, 's');
    if (r.bad) return NULL;
    rd_u32(&r);
    int32_t result = (int32_t)rd_u32(&r);
    ui_session_pop(s);
    return result == 0 ? ui_session_surface(s, surface) : NULL;
}

void ui_surface_changed(struct ui_session *s, uint32_t surface) {
    struct ui_bytes w = {0};
    wr_u8(&w, 'P');
    wr_u32(&w, surface);
    if (!w.bad) ui_session_send(s, w.p, w.n);
    free(w.p);
}

void ui_surface_changed_rect(struct ui_session *s, uint32_t surface, int x, int y, int width, int height) {
    if (x < 0) width += x, x = 0;
    if (y < 0) height += y, y = 0;
    if (width <= 0 || height <= 0) return;
    struct ui_bytes w = {0};
    wr_u8(&w, 'P');
    wr_u32(&w, surface);
    wr_u32(&w, (uint32_t)x);
    wr_u32(&w, (uint32_t)y);
    wr_u32(&w, (uint32_t)width);
    wr_u32(&w, (uint32_t)height);
    if (!w.bad) ui_session_send(s, w.p, w.n);
    free(w.p);
}

bool ui_event_read(const void *msg, size_t n, struct ui_event *e) {
    struct rd r = {msg, (const uint8_t *)msg + n, false};
    if (rd_u8(&r) != 'e') return false;
    e->window = rd_u32(&r);
    e->view = rd_u32(&r);
    e->type = rd_u32(&r);
    e->node = rd_u32(&r);
    e->mods = rd_u32(&r);
    e->flags = rd_u32(&r);
    e->a = rd_f64(&r);
    e->b = rd_f64(&r);
    struct str t = rd_str(&r);
    e->text = t.p;
    e->text_n = t.n;
    return !r.bad;
}

bool ui_next_event(struct ui_session *s, struct ui_event *e, char *text, size_t cap) {
    size_t n;
    const void *p;
    while ((p = ui_session_peek(s, &n))) {
        bool ok = ui_event_read(p, n, e);
        if (ok) {
            size_t tn = cap ? (e->text_n < cap - 1 ? e->text_n : cap - 1) : 0;
            if (cap) {
                memcpy(text, e->text, tn);
                text[tn] = 0;
            }
            e->text = cap ? text : "";
            e->text_n = tn;
        }
        ui_session_pop(s);
        if (ok) return true;
    }
    return false;
}
