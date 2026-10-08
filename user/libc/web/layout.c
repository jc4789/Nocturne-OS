/* Layout: block formatting (margin collapsing, floats, clearance), inline formatting (line
   breaking, alignment, vertical-align, inline backgrounds), tables (auto layout with colspan and
   rowspan), flexbox (single and multi-line, grow/shrink, justify/align), replaced elements and
   form controls, and absolute/fixed/relative positioning.

   Every box's x, y is its content box's position relative to the content box of b->cb (the box
   it was laid out in); w, h are its content size. */
#include <stdio.h>
#include <math.h>
#include "webi.h"
#include "web_dialog.h"
#include "js_canvas.h"
#include "avmedia.h"
#include "svg_geometry.h"

static web_doc *D;
static float VW, VH;
static uint32_t GEN;

#define INF 1e30f

/* ---------------------------------------------------------------- fonts */
wfont style_font(const style_t *st) {
    wfont f = {NULL, st->font_size, st->font_weight >= 600};
    int idx = (f.bold ? FONT_BOLD : 0) | (st->font_style ? FONT_ITALIC : 0);
    f.ttf = font_family(st->font_family, idx);
    if (!f.ttf) f.ttf = font_ui(FONT_REGULAR);
    return f;
}

static int mono_font(const wfont *f) { return f->px >= 24 ? FONT_LARGE : FONT_SMALL; }

float wf_width(const wfont *f, const char *s, size_t n) {
    if (f->px < 1) return 0;
    if (f->ttf) return font_width(f->ttf, f->px, s, n);
    int k = 0;
    for (size_t i = 0; i < n; i++)
        if (((unsigned char)s[i] & 0xC0) != 0x80) k++;
    return (float)(k * gfx_font_w(mono_font(f)));
}

void wf_metrics(const wfont *f, float *asc, float *desc) {
    if (f->ttf) {
        float gap;
        font_metrics(f->ttf, f->px, asc, desc, &gap);
        return;
    }
    int fh = gfx_font_h(mono_font(f));
    *asc = fh * 0.75f;
    *desc = fh * 0.25f;
}

float wf_draw(canvas_t *c, const wfont *f, float x, int baseline, const char *s, size_t n, uint32_t color) {
    if (f->px < 1) return x;
    if (f->ttf) return font_draw(c, f->ttf, f->px, x, baseline, s, n, color);
    int font = mono_font(f);
    int fh = gfx_font_h(font), cw = gfx_font_w(font);
    int top = baseline - (int)(fh * 0.75f);
    for (size_t i = 0; i < n;) {
        uint32_t cp;
        i += (size_t)gfx_utf8_decode(s + i, &cp);
        uint8_t g = gfx_glyph_for(cp);
        gfx_char(c, (int)x, top, g, color, 0, font);
        if (f->bold) gfx_char(c, (int)x + 1, top, g, color, 0, font);
        x += (float)cw;
    }
    return x;
}

static float line_height_px(const style_t *st) {
    float lh;
    switch (st->line_height.kind) {
    case LK_NUMBER: lh = st->line_height.px * st->font_size; break;
    case LK_LEN: lh = st->line_height.px; break;
    default:
        lh = st->font_size * 1.2f;
        wfont f = style_font(st);
        float asc, desc, gap = 0;
        wf_metrics(&f, &asc, &desc);
        if (f.ttf) font_metrics(f.ttf, f.px, NULL, NULL, &gap);
        if (asc + desc + gap > lh) lh = asc + desc + gap;
    }
    return lh < 0 ? 0 : lh;
}

float style_line_height(const style_t *st) { return line_height_px(st); }

/* letter- and word-spacing aware width */
static float text_width(const style_t *st, const wfont *f, const char *s, size_t n) {
    float w = wf_width(f, s, n);
    if (st->letter_spacing != 0 || st->word_spacing != 0) {
        for (size_t i = 0; i < n; i++) {
            if (((unsigned char)s[i] & 0xC0) != 0x80) w += st->letter_spacing;
            if (s[i] == ' ') w += st->word_spacing;
        }
    }
    return w;
}

/* ---------------------------------------------------------------- geometry helpers */
static float hext(const box_t *b) { return b->p[1] + b->p[3] + b->b[1] + b->b[3]; }
static float vext(const box_t *b) { return b->p[0] + b->p[2] + b->b[0] + b->b[2]; }
static float fmaxf_(float a, float b) { return a > b ? a : b; }
static float fminf_(float a, float b) { return a < b ? a : b; }

static void resolve_edges(box_t *b, float cbw) {
    const style_t *s = b->st;
    if (cbw < 0) cbw = 0;
    for (int i = 0; i < 4; i++) {
        float p = len_resolve(&s->padding[i], cbw);
        b->p[i] = p > 0 ? p : 0;
        b->b[i] = s->border_width[i];
        b->m[i] = len_auto(&s->margin[i]) ? 0 : len_resolve(&s->margin[i], cbw);
    }
    if (b->kind == B_ROW || b->kind == B_ROW_GROUP || b->kind == B_TEXT || b->kind == B_BR) {
        memset(b->p, 0, sizeof b->p);
        memset(b->b, 0, sizeof b->b);
        memset(b->m, 0, sizeof b->m);
    }
    if (b->kind == B_CELL) memset(b->m, 0, sizeof b->m);
}

/* a content width from a width-like length, or -1 for auto/none */
static float spec_w(const box_t *b, const len_t *l, float cbw) {
    if (l->kind != LK_LEN && l->kind != LK_EXPR) return -1;
    if (len_has_pct(l) && cbw < 0) return -1;
    float w = len_resolve(l, cbw < 0 ? 0 : cbw);
    if (b->st->box_sizing) w -= hext(b);
    return w < 0 ? 0 : w;
}

static float spec_h(const box_t *b, const len_t *l, float cbh) {
    if (l->kind != LK_LEN && l->kind != LK_EXPR) return -1;
    if (len_has_pct(l) && cbh < 0) return -1;
    float h = len_resolve(l, cbh < 0 ? 0 : cbh);
    if (b->st->box_sizing) h -= vext(b);
    return h < 0 ? 0 : h;
}

static float clamp_w(const box_t *b, float w, float cbw) {
    float mx = spec_w(b, &b->st->max_width, cbw);
    if (mx >= 0 && w > mx) w = mx;
    float mn = spec_w(b, &b->st->min_width, cbw);
    if (mn >= 0 && w < mn) w = mn;
    return w < 0 ? 0 : w;
}

static float clamp_h(const box_t *b, float h, float cbh) {
    float mx = spec_h(b, &b->st->max_height, cbh);
    if (mx >= 0 && h > mx) h = mx;
    float mn = spec_h(b, &b->st->min_height, cbh);
    if (mn >= 0 && h < mn) h = mn;
    return h < 0 ? 0 : h;
}

static float mcollapse(float a, float b) {
    if (a >= 0 && b >= 0) return a > b ? a : b;
    if (a < 0 && b < 0) return a < b ? a : b;
    return a + b;
}

static bool is_flex_item(const box_t *b) {
    return b->parent && (b->parent->kind == B_FLEX || b->parent->kind == B_GRID) && !b->abspos;
}

static bool is_bfc_root(const box_t *b) {
    return !b->parent || b->floated || b->abspos || b->kind == B_ATOMIC || b->kind == B_CELL ||
           b->kind == B_CAPTION || b->kind == B_TABLE || b->kind == B_FLEX || b->kind == B_GRID || b->st->overflow != OV_VISIBLE ||
           b->st->display == D_FLOW_ROOT || is_flex_item(b) || b->is_bfc;
}

float box_abs_x(const box_t *b) {
    float x = 0;
    for (; b; b = b->cb) x += b->x + b->rel_dx;
    return x;
}

float box_abs_y(const box_t *b) {
    float y = b->y + b->rel_dy;
    for (const box_t *c = b->cb; c; c = c->cb) y += c->y + c->rel_dy + c->content_dy;
    return y;
}

static void list_abs(box_t *b) {
    if (b->gen == GEN) return;
    b->gen = GEN;
    pv_push(&D->abs_boxes, b);
}

/* ---------------------------------------------------------------- floats */
struct fl {
    float x, y, w, h; /* margin box, in BFC coordinates */
    int side;
};
struct bfc {
    struct fl *v;
    int n, cap;
    float last_top;
};

static void bfc_free(struct bfc *f) { free(f->v); }

static void bfc_add(struct bfc *f, float x, float y, float w, float h, int side) {
    if (f->n == f->cap) {
        f->cap = f->cap ? f->cap * 2 : 8;
        f->v = realloc(f->v, sizeof *f->v * (size_t)f->cap);
    }
    f->v[f->n++] = (struct fl){x, y, w, h, side};
    f->last_top = y;
}

/* the free interval within [x0, x1] for the band y .. y+h */
static void bfc_space(const struct bfc *f, float y, float h, float x0, float x1, float *l, float *r) {
    *l = x0;
    *r = x1;
    if (h < 1) h = 1;
    for (int i = 0; f && i < f->n; i++) {
        const struct fl *a = &f->v[i];
        if (a->y >= y + h || a->y + a->h <= y || a->h <= 0) continue;
        if (a->side == FL_LEFT) *l = fmaxf_(*l, a->x + a->w);
        else *r = fminf_(*r, a->x);
    }
}

/* the lowest float bottom below y, or -1 */
static float bfc_next(const struct bfc *f, float y) {
    float best = -1;
    for (int i = 0; f && i < f->n; i++) {
        float b = f->v[i].y + f->v[i].h;
        if (b > y + 0.01f && (best < 0 || b < best)) best = b;
    }
    return best;
}

static float bfc_clear(const struct bfc *f, int sides) {
    float y = -INF;
    for (int i = 0; f && i < f->n; i++)
        if ((f->v[i].side == FL_LEFT && (sides & CL_LEFT)) || (f->v[i].side == FL_RIGHT && (sides & CL_RIGHT)))
            y = fmaxf_(y, f->v[i].y + f->v[i].h);
    return y;
}

static float bfc_bottom(const struct bfc *f) { return bfc_clear(f, CL_BOTH); }

/* ---------------------------------------------------------------- forward declarations */
static void layout_inner(box_t *b, struct bfc *f, float ox, float oy, float cbh);
static void intrinsic(box_t *b, float *mn, float *mx);
static void outer_intrinsic(box_t *c, float *mn, float *mx);
static void size_atomic(box_t *c, float cbw, float cbh);
static void table_width(box_t *t, float cw);

/* shrink-to-fit content width for the space avail (margin box) */
static float stf_width(box_t *c, float avail) {
    float mn, mx;
    intrinsic(c, &mn, &mx);
    float a = avail - c->m[1] - c->m[3] - hext(c);
    return fminf_(fmaxf_(mn, a), mx);
}

/* ---------------------------------------------------------------- replaced elements */
static const char *input_type(node_t *n) {
    const char *t = node_attr(n, "type");
    return t ? t : "text";
}

const char *web_button_label(node_t *n) {
    const char *v = node_attr(n, "value");
    if (v) return v;
    const char *t = input_type(n);
    if (str_ieq(t, "reset")) return "Reset";
    if (str_ieq(t, "file")) return "Choose file";
    if (str_ieq(t, "submit")) return "Submit";
    return "";
}

static void replaced_natural(box_t *c, float *iw, float *ih, float *ratio) {
    node_t *n = c->node;
    *iw = *ih = *ratio = 0;
    switch (c->atomic) {
    case AT_IMG: {
        struct web_image *im = n->image >= 0 && n->image < D->images.n ? D->images.v[n->image] : NULL;
        if (im && im->img) {
            *iw = (float)im->img->w;
            *ih = (float)im->img->h;
            *ratio = *ih > 0 ? *iw / *ih : 0;
        } else if (!im || im->failed) {
            const char *alt = node_attr(n, "alt");
            if (alt && *alt) {
                wfont f = style_font(c->st);
                *iw = wf_width(&f, alt, strlen(alt)) + 4;
                *ih = line_height_px(c->st);
            }
        }
        break;
    }
    case AT_SVG: {
        float vb[4];
        int view = svg_viewbox(node_attr(n, "viewBox"), vb);
        /* Presentation attributes and author CSS have already cascaded. Only
           absolute computed lengths are intrinsic dimensions (not %/auto). */
        *iw = spec_w(c, &c->st->width, -1);
        *ih = spec_h(c, &c->st->height, -1);
        if (*iw > 0 && *ih > 0) *ratio = *iw / *ih;
        else if (view == 1) *ratio = vb[2] / vb[3];
        if (*iw < 0 && *ih < 0) {
            /* No intrinsic dimensions: contain the ratio in the default
               300x150 object rectangle; viewBox is not a pixel size. */
            *iw = 300; *ih = 150;
            if (*ratio > 0) {
                if (*ratio > 2) *ih = *iw / *ratio;
                else *iw = *ih * *ratio;
            }
        } else if (*iw < 0) *iw = *ratio > 0 ? *ih * *ratio : 300;
        else if (*ih < 0) *ih = *ratio > 0 ? *iw / *ratio : 150;
        break;
    }
    case AT_PLACEHOLDER:
        if (n->tag == T_canvas) {
            *iw=(float)web_canvas_dimension(n,"width");
            *ih=(float)web_canvas_dimension(n,"height");
        } else if(n->tag==T_audio)*iw=300,*ih=node_attr(n,"controls")?32:0;
        else if(n->tag==T_video){int vw=300,vh=150;web_avmedia_size(n,&vw,&vh);*iw=(float)vw;*ih=(float)vh;}
        else if (n->tag == T_meter || n->tag == T_progress) *iw = 80, *ih = 16;
        else if (str_ieq(n->name, "input")) *iw = 129, *ih = 16; /* range */
        else *iw = 300, *ih = 150;
        *ratio = *ih>0 ? *iw / *ih : 0;
        break;
    }
}

static void size_replaced(box_t *c, float cbw, float cbh) {
    float iw, ih, ratio;
    replaced_natural(c, &iw, &ih, &ratio);
    float w = spec_w(c, &c->st->width, cbw), h = spec_h(c, &c->st->height, cbh);
    if (c->atomic == AT_SVG) {
        /* Outermost inline SVG auto acts as 100% where the containing axis is
           definite. Intrinsic passes use -1; do not invent a percentage base. */
        if (len_auto(&c->st->width) && cbw >= 0) w = cbw;
        if (len_auto(&c->st->height) && cbh >= 0) h = cbh;
    }
    bool derive_w = w < 0, derive_h = h < 0;
    if (w < 0 && h < 0) w = iw, h = ih;
    else if (w < 0) w = ratio > 0 ? h * ratio : iw;
    else if (h < 0) h = ratio > 0 ? w / ratio : ih;
    float mx = spec_w(c, &c->st->max_width, cbw);
    if (mx >= 0 && w > mx) {
        if (ratio > 0 && derive_h) h = mx / ratio;
        w = mx;
    }
    float mxh = spec_h(c, &c->st->max_height, cbh);
    if (mxh >= 0 && h > mxh) {
        if (ratio > 0 && derive_w) w = mxh * ratio;
        h = mxh;
    }
    w = clamp_w(c, w, cbw);
    h = clamp_h(c, h, cbh);
    c->w = w;
    c->h = h;
    c->baseline = c->last_baseline = -1;
}

static void size_control(box_t *c, float cbw, float cbh) {
    const style_t *st = c->st;
    wfont f = style_font(st);
    float lh = line_height_px(st);
    float asc, desc;
    wf_metrics(&f, &asc, &desc);
    float cw = st->font_size * 0.55f;
    float w = 0, h = lh;
    node_t *n = c->node;
    switch (c->atomic) {
    case AT_INPUT: {
        const char *sz = node_attr(n, "size");
        int k = sz && atoi(sz) > 0 ? atoi(sz) : 20;
        w = k * cw;
        break;
    }
    case AT_BUTTON_INPUT: {
        const char *l = web_button_label(n);
        w = wf_width(&f, l, strlen(l));
        break;
    }
    case AT_CHECKBOX: case AT_RADIO: w = h = 13; break;
    case AT_SELECT: {
        float best = 0;
        for (node_t *o = n->first; o; o = o->next) {
            node_t *opts[2] = {o, NULL};
            if (o->type == N_ELEM && o->tag == T_optgroup) opts[0] = o->first;
            for (node_t *q = opts[0]; q; q = o->tag == T_optgroup ? q->next : NULL) {
                if (q->type == N_ELEM && q->tag == T_option) {
                    sbuf b = {0};
                    const char *lab = node_attr(q, "label");
                    if (lab) sb_puts(&b, lab);
                    else node_text_content(q, &b);
                    best = fmaxf_(best, wf_width(&f, b.p ? b.p : "", b.n));
                    sb_free(&b);
                }
                if (o->tag != T_optgroup) break;
            }
        }
        w = best + 4 + st->font_size * 1.3f; /* room for the arrow */
        break;
    }
    case AT_TEXTAREA: {
        const char *co = node_attr(n, "cols"), *ro = node_attr(n, "rows");
        int cols = co && atoi(co) > 0 ? atoi(co) : 20, rows = ro && atoi(ro) > 0 ? atoi(ro) : 2;
        w = cols * wf_width(&f, "m", 1);
        h = rows * lh;
        break;
    }
    }
    float sw = spec_w(c, &st->width, cbw), sh = spec_h(c, &st->height, cbh);
    if (sw >= 0) w = sw;
    if (sh >= 0) h = sh;
    c->w = clamp_w(c, w, cbw);
    c->h = clamp_h(c, h, cbh);
    if (c->atomic == AT_CHECKBOX || c->atomic == AT_RADIO) c->baseline = c->last_baseline = c->h - 2;
    else c->baseline = c->last_baseline = (c->atomic == AT_TEXTAREA ? 0 : (c->h - (asc + desc)) / 2) + asc;
}

/* size and lay out an atomic inline (or a block-level replaced element) for a containing block */
static void size_atomic(box_t *c, float cbw, float cbh) {
    resolve_edges(c, cbw);
    switch (c->atomic) {
    case AT_IMG: case AT_SVG: case AT_PLACEHOLDER: size_replaced(c, cbw, cbh); return;
    case AT_INPUT: case AT_BUTTON_INPUT: case AT_CHECKBOX: case AT_RADIO: case AT_SELECT: case AT_TEXTAREA:
        size_control(c, cbw, cbh);
        return;
    }
    /* inline-block (and buttons): shrink-to-fit, then laid out as a block */
    float w = spec_w(c, &c->st->width, cbw);
    if (w < 0) w = stf_width(c, cbw < 0 ? INF : cbw);
    c->w = clamp_w(c, w, cbw);
    layout_inner(c, NULL, 0, 0, cbh);
}

/* ---------------------------------------------------------------- inline items */
enum { IT_TEXT, IT_ATOMIC, IT_BR, IT_OPEN, IT_CLOSE, IT_FLOAT, IT_ABS, IT_WBR };

struct item {
    uint8_t kind;
    bool brk;       /* a line may break after it */
    bool space_end; /* ends in a collapsible space (hangs at the end of a line) */
    box_t *box;
    const char *s;
    int n;
    float w, sw; /* width, and the width of the trailing space */
    style_t *st;
    node_t *link;
    float shift; /* baseline shift upwards */
};

struct ivec {
    struct item *v;
    int n, cap;
};

static struct item *ipush(struct ivec *v, int kind) {
    if (v->n == v->cap) {
        v->cap = v->cap ? v->cap * 2 : 64;
        v->v = realloc(v->v, sizeof *v->v * (size_t)v->cap);
    }
    struct item *it = &v->v[v->n++];
    memset(it, 0, sizeof *it);
    it->kind = (uint8_t)kind;
    return it;
}

struct ibuild {
    struct ivec *v;
    bool last_space;
    node_t *link;
    float shift;
};

static float va_shift(const style_t *st) {
    switch (st->vertical_align) {
    case VA_SUB: return -st->font_size * 0.25f;
    case VA_SUPER: return st->font_size * 0.4f;
    case VA_LEN: return st->vertical_align_px;
    }
    return 0;
}

static void text_items(box_t *t, struct ibuild *s) {
    style_t *st = t->st;
    wfont f = style_font(st);
    int ws = st->white_space;
    bool collapse = ws == WS_NORMAL || ws == WS_NOWRAP || ws == WS_PRE_LINE;
    bool wrap = ws != WS_NOWRAP && ws != WS_PRE;
    const char *p = t->text, *e = p + t->len;
    while (p < e) {
        if (*p == '\n') {
            struct item *it = ipush(s->v, IT_BR);
            it->st = st;
            it->box = t;
            s->last_space = true;
            p++;
            continue;
        }
        if (collapse && *p == ' ' && s->last_space) {
            p++;
            continue;
        }
        const char *we = p;
        while (we < e && *we != ' ' && *we != '\n') we++;
        const char *se = we;
        while (se < e && *se == ' ') se++;
        struct item *it = ipush(s->v, IT_TEXT);
        it->box = t;
        it->st = st;
        it->s = p;
        it->n = (int)(se - p);
        it->sw = se > we ? text_width(st, &f, we, (size_t)(se - we)) : 0;
        it->w = text_width(st, &f, p, (size_t)(we - p)) + it->sw;
        it->space_end = se > we;
        it->brk = wrap && se > we;
        it->link = s->link;
        it->shift = s->shift;
        s->last_space = se > we;
        p = se;
    }
}

static void build_items(box_t *parent, struct ibuild *s) {
    for (box_t *c = parent->first; c; c = c->next) {
        if (c->abspos) {
            ipush(s->v, IT_ABS)->box = c;
            continue;
        }
        if (c->floated) {
            ipush(s->v, IT_FLOAT)->box = c;
            continue;
        }
        switch (c->kind) {
        case B_TEXT: text_items(c, s); break;
        case B_BR: {
            bool soft = c->node && c->node->tag == T_wbr;
            struct item *it = ipush(s->v, soft ? IT_WBR : IT_BR);
            it->box = c;
            it->st = c->st;
            if (soft) it->brk = c->st->white_space != WS_NOWRAP && c->st->white_space != WS_PRE;
            else s->last_space = true;
            break;
        }
        case B_INLINE: {
            node_t *save_link = s->link;
            float save_shift = s->shift;
            if (c->node && c->node->tag == T_a && node_attr(c->node, "href")) s->link = c->node;
            s->shift += va_shift(c->st);
            if (c->st->position == POS_RELATIVE) {
                if (!len_auto(&c->st->inset[0])) s->shift -= len_resolve(&c->st->inset[0], 0);
                else if (!len_auto(&c->st->inset[2])) s->shift += len_resolve(&c->st->inset[2], 0);
            }
            struct item *it = ipush(s->v, IT_OPEN);
            it->box = c;
            it->st = c->st;
            it->link = s->link;
            it->shift = s->shift;
            build_items(c, s);
            it = ipush(s->v, IT_CLOSE);
            it->box = c;
            it->st = c->st;
            it->link = s->link;
            it->shift = s->shift;
            s->link = save_link;
            s->shift = save_shift;
            break;
        }
        default: {
            struct item *it = ipush(s->v, IT_ATOMIC);
            it->box = c;
            it->st = c->st;
            it->link = s->link;
            it->shift = s->shift + va_shift(c->st);
            it->brk = c->st->white_space != WS_NOWRAP && c->st->white_space != WS_PRE;
            /* a break is also allowed before it */
            if (s->v->n >= 2) {
                struct item *pv = &s->v->v[s->v->n - 2];
                if (pv->kind == IT_TEXT || pv->kind == IT_ATOMIC || pv->kind == IT_CLOSE) {
                    if (parent->st->white_space != WS_NOWRAP && parent->st->white_space != WS_PRE) pv->brk = true;
                }
            }
            s->last_space = false;
        }
        }
    }
}

/* the inline padding+border+margin an inline box adds at its start (or end) */
static float inline_edge(box_t *b, bool start, float cbw) {
    resolve_edges(b, cbw);
    return start ? b->m[3] + b->b[3] + b->p[3] : b->p[1] + b->b[1] + b->m[1];
}

/* ---------------------------------------------------------------- inline layout */
struct open_box {
    box_t *box;
    float x; /* border-box start on this line */
    bool first;
};

struct iline {
    box_t *blk;
    struct bfc *f;
    float ox, oy;
    struct ivec *items;
    sbuf runs, decos;
    struct open_box open[64];
    int nopen;
    float y;           /* top of the current line */
    float left, right; /* available space of the current line */
    bool first_line;
    float first_bl, last_bl;
    float strut_lh;
    int pending_floats[64];
    int npending;
};

static void place_float(box_t *c, box_t *parent, struct bfc *f, float ox, float oy, float y_rel);

static void line_space(struct iline *L) {
    box_t *b = L->blk;
    bfc_space(L->f, L->oy + L->y, L->strut_lh, L->ox, L->ox + b->w, &L->left, &L->right);
    L->left -= L->ox;
    L->right -= L->ox;
    if (L->first_line) {
        float ind = len_resolve(&b->st->text_indent, b->w);
        L->left += ind;
    }
}

static void add_deco(struct iline *L, box_t *ib, float x0, float x1, float baseline, bool first, bool last) {
    const style_t *st = ib->st;
    bool visible = (st->bg_color >> 24) || st->has_grad || st->bg_img || st->mask_img || ib->b[0] || ib->b[1] || ib->b[2] || ib->b[3];
    if (!visible) return;
    wfont f = style_font(st);
    float asc, desc;
    wf_metrics(&f, &asc, &desc);
    struct deco d;
    d.x = x0;
    d.w = x1 - x0;
    d.y = baseline - asc - ib->p[0] - ib->b[0];
    d.h = asc + desc + ib->p[0] + ib->p[2] + ib->b[0] + ib->b[2];
    d.st = (style_t *)st;
    d.first = first;
    d.last = last;
    d.node = ib->node;
    sb_put(&L->decos, (const char *)&d, sizeof d);
}

/* vertical extent of an item above and below the baseline */
static void item_vext(const struct item *it, float *above, float *below) {
    if (it->kind == IT_ATOMIC) {
        box_t *c = it->box;
        float h = c->h + vext(c) + c->m[0] + c->m[2];
        float bl = c->last_baseline >= 0 && c->st->overflow == OV_VISIBLE
                       ? c->m[0] + c->b[0] + c->p[0] + c->last_baseline
                       : h;
        if (c->atomic == AT_CHECKBOX || c->atomic == AT_RADIO || c->atomic == AT_INPUT ||
            c->atomic == AT_BUTTON_INPUT || c->atomic == AT_SELECT || c->atomic == AT_TEXTAREA)
            bl = c->m[0] + c->b[0] + c->p[0] + c->last_baseline;
        if (c->st->vertical_align == VA_MIDDLE) {
            float xh = c->parent ? c->parent->st->font_size * 0.27f : 4;
            *above = h / 2 + xh;
            *below = h / 2 - xh;
        } else {
            *above = bl + it->shift;
            *below = h - bl - it->shift;
        }
        return;
    }
    const style_t *st = it->st;
    wfont f = style_font(st);
    float asc, desc;
    wf_metrics(&f, &asc, &desc);
    float lh = line_height_px(st);
    float half = (lh - (asc + desc)) / 2;
    *above = asc + half + it->shift;
    *below = desc + half - it->shift;
}

static void finish_line(struct iline *L, int ls, int le, bool forced) {
    struct item *v = L->items->v;
    box_t *b = L->blk;
    /* drop collapsible spaces at the start and end of the line */
    while (ls < le && v[ls].kind == IT_TEXT && v[ls].n > 0 && v[ls].space_end && v[ls].w == v[ls].sw &&
           (v[ls].st->white_space == WS_NORMAL || v[ls].st->white_space == WS_NOWRAP || v[ls].st->white_space == WS_PRE_LINE))
        ls++;
    int last_text = -1;
    for (int i = le - 1; i >= ls; i--) {
        if (v[i].kind == IT_TEXT) {
            last_text = i;
            break;
        }
        if (v[i].kind == IT_ATOMIC) break;
    }
    float W = 0;
    int spaces = 0;
    bool content = false;
    for (int i = ls; i < le; i++) {
        struct item *it = &v[i];
        if (it->kind == IT_FLOAT || it->kind == IT_ABS) continue;
        float w = it->w;
        if (i == last_text && it->space_end) w -= it->sw;
        W += w;
        if (it->kind == IT_TEXT || it->kind == IT_ATOMIC || it->kind == IT_BR) content = true;
        if (it->kind == IT_OPEN || it->kind == IT_CLOSE) {
            if (it->w > 0 || it->box->p[0] > 0 || it->box->b[0] > 0) content = true;
        }
        if (it->kind == IT_TEXT && it->space_end && i != last_text) spaces++;
    }
    /* metrics: the strut, then every item */
    const style_t *bst = b->st;
    wfont bf = style_font(bst);
    float asc, desc;
    wf_metrics(&bf, &asc, &desc);
    float half = (L->strut_lh - (asc + desc)) / 2;
    float above = asc + half, below = desc + half;
    float tall = 0; /* vertical-align top/bottom items */
    for (int i = ls; i < le; i++) {
        struct item *it = &v[i];
        if (it->kind == IT_FLOAT || it->kind == IT_ABS || it->kind == IT_CLOSE || it->kind == IT_WBR) continue;
        if (it->kind == IT_OPEN) continue;
        float a, d;
        item_vext(it, &a, &d);
        int va = it->kind == IT_ATOMIC ? it->box->st->vertical_align : VA_BASELINE;
        if (va == VA_TOP || va == VA_BOTTOM || va == VA_TEXT_TOP || va == VA_TEXT_BOTTOM) {
            tall = fmaxf_(tall, a + d);
            continue;
        }
        above = fmaxf_(above, a);
        below = fmaxf_(below, d);
    }
    float lh = above + below;
    if (tall > lh) {
        below += tall - lh;
        lh = tall;
    }
    if (!content) {
        lh = 0;
        above = below = 0;
    }
    float baseline = L->y + above;
    /* horizontal alignment */
    float avail = L->right - L->left;
    float x = L->left, extra_space = 0;
    int ta = bst->text_align;
    if (ta == TA_RIGHT) x += avail - W;
    else if (ta == TA_CENTER || ta == TA_WCENTER) x += (avail - W) / 2;
    else if (ta == TA_JUSTIFY && !forced && le < L->items->n && spaces > 0 && W < avail)
        extra_space = (avail - W) / (float)spaces;
    if (x < L->left && ta != TA_LEFT) x = L->left;
    /* inline boxes still open from the previous line restart at the line start */
    for (int k = 0; k < L->nopen; k++) {
        L->open[k].x = x;
        L->open[k].first = false;
    }
    for (int i = ls; i < le; i++) {
        struct item *it = &v[i];
        switch (it->kind) {
        case IT_OPEN: {
            box_t *ib = it->box;
            if (L->nopen < 64) {
                L->open[L->nopen].box = ib;
                L->open[L->nopen].x = x + ib->m[3];
                L->open[L->nopen].first = true;
                L->nopen++;
            }
            if (ib->node && !ib->node->anchor_block) {
                ib->node->anchor_block = b;
                ib->node->anchor_dy = L->y;
            }
            x += it->w;
            break;
        }
        case IT_CLOSE: {
            box_t *ib = it->box;
            for (int k = L->nopen - 1; k >= 0; k--)
                if (L->open[k].box == ib) {
                    add_deco(L, ib, L->open[k].x, x + ib->p[1] + ib->b[1], baseline - it->shift + ib->st->font_size * 0 , L->open[k].first, true);
                    memmove(&L->open[k], &L->open[k + 1], sizeof L->open[0] * (size_t)(L->nopen - k - 1));
                    L->nopen--;
                    break;
                }
            x += it->w;
            break;
        }
        case IT_TEXT: {
            int n = it->n;
            float w = it->w;
            if (i == last_text && it->space_end) {
                w -= it->sw;
                while (n > 0 && it->s[n - 1] == ' ') n--;
            }
            struct run r = {x, baseline - it->shift, w, it->s, n, it->st, it->link, NULL, false,
                            it->box ? it->box->node : NULL};
            if (n > 0) sb_put(&L->runs, (const char *)&r, sizeof r);
            x += w;
            if (it->space_end && i != last_text) x += extra_space;
            break;
        }
        case IT_ATOMIC: {
            box_t *c = it->box;
            float a, d;
            item_vext(it, &a, &d);
            float top; /* margin-box top */
            int va = c->st->vertical_align;
            if (va == VA_TOP || va == VA_TEXT_TOP) top = L->y;
            else if (va == VA_BOTTOM || va == VA_TEXT_BOTTOM) top = L->y + lh - (a + d);
            else top = baseline - a;
            c->cb = b;
            c->x = x + c->m[3] + c->b[3] + c->p[3];
            c->y = top + c->m[0] + c->b[0] + c->p[0];
            struct run r = {x, top, it->w, NULL, 0, it->st, it->link, c, false, c->node};
            sb_put(&L->runs, (const char *)&r, sizeof r);
            x += it->w;
            break;
        }
        case IT_ABS: {
            box_t *c = it->box;
            c->static_cb = b;
            c->static_x = x;
            c->static_y = L->y;
            list_abs(c);
            break;
        }
        }
    }
    /* backgrounds of inline boxes that continue on the next line */
    for (int k = 0; k < L->nopen; k++) add_deco(L, L->open[k].box, L->open[k].x, x, baseline, L->open[k].first, false);
    if (content) {
        if (L->first_bl < 0) L->first_bl = baseline;
        L->last_bl = baseline;
        L->first_line = false;
    }
    L->y += lh;
    /* floats that did not fit on the line just finished */
    for (int k = 0; k < L->npending; k++) place_float(v[L->pending_floats[k]].box, b, L->f, L->ox, L->oy, L->y);
    L->npending = 0;
    line_space(L);
}

/* split a too-long word over lines at character boundaries */
static int split_word(struct iline *L, int i, float avail) {
    struct ivec *iv = L->items;
    struct item it = iv->v[i];
    wfont f = style_font(it.st);
    int k = 0;
    float w = 0;
    while (k < it.n) {
        int cl = 1;
        while (k + cl < it.n && ((unsigned char)it.s[k + cl] & 0xC0) == 0x80) cl++;
        float cw = text_width(it.st, &f, it.s + k, (size_t)cl);
        if (w + cw > avail && k > 0) break;
        w += cw;
        k += cl;
    }
    if (k >= it.n) return 0;
    /* the item becomes two: the part that fits, and the rest */
    if (iv->n == iv->cap) {
        iv->cap *= 2;
        iv->v = realloc(iv->v, sizeof *iv->v * (size_t)iv->cap);
    }
    memmove(&iv->v[i + 2], &iv->v[i + 1], sizeof *iv->v * (size_t)(iv->n - i - 1));
    iv->n++;
    struct item *a = &iv->v[i], *b = &iv->v[i + 1];
    *a = it;
    *b = it;
    a->n = k;
    a->w = w;
    a->sw = 0;
    a->space_end = false;
    a->brk = true;
    b->s = it.s + k;
    b->n = it.n - k;
    b->w = it.w - w;
    return 1;
}

static void layout_inline(box_t *b, struct bfc *f, float ox, float oy, float cbh) {
    struct ivec iv = {0};
    struct ibuild bs = {&iv, true, NULL, 0};
    if (b->node && b->node->tag == T_a && node_attr(b->node, "href")) bs.link = b->node;
    for (node_t *n = b->node; n && !bs.link; n = doc_flat_parent(n))
        if (n->type == N_ELEM && n->tag == T_a && node_attr(n, "href")) bs.link = n;
    if (!bs.link)
        for (box_t *p = b->parent; p && !bs.link; p = p->parent)
            if (p->node)
                for (node_t *n = p->node; n; n = doc_flat_parent(n))
                    if (n->type == N_ELEM && n->tag == T_a && node_attr(n, "href")) {
                        bs.link = n;
                        break;
                    }
    build_items(b, &bs);
    /* widths of inline box edges and atomic inlines */
    for (int i = 0; i < iv.n; i++) {
        struct item *it = &iv.v[i];
        if (it->kind == IT_OPEN) it->w = inline_edge(it->box, true, b->w);
        else if (it->kind == IT_CLOSE) it->w = inline_edge(it->box, false, b->w);
        else if (it->kind == IT_ATOMIC) {
            box_t *c = it->box;
            size_atomic(c, b->w, c->atomic == AT_SVG ? cbh : -1);
            it->w = c->w + hext(c) + c->m[1] + c->m[3];
        }
    }
    struct iline L;
    memset(&L, 0, sizeof L);
    L.blk = b;
    L.f = f;
    L.ox = ox;
    L.oy = oy;
    L.items = &iv;
    L.first_line = true;
    L.first_bl = L.last_bl = -1;
    L.strut_lh = line_height_px(b->st);
    line_space(&L);
    int ls = 0, cs = 0;
    float line_w = 0, chunk_w = 0;
    for (int i = 0; i < iv.n; i++) {
        struct item *it = &iv.v[i];
        if (it->kind == IT_FLOAT) {
            box_t *c = it->box;
            if (line_w == 0 && chunk_w == 0) {
                place_float(c, b, f, ox, oy, L.y);
                line_space(&L);
            } else if (L.npending < 64) L.pending_floats[L.npending++] = i;
            continue;
        }
        if (it->kind == IT_ABS) continue;
        chunk_w += it->w;
        bool end = it->kind == IT_BR || it->brk || i == iv.n - 1;
        if (!end) continue;
        /* commit the chunk cs..i */
        float trim = it->space_end ? it->sw : 0;
        float avail = L.right - L.left;
        if (line_w > 0 && line_w + chunk_w - trim > avail + 0.01f) {
            finish_line(&L, ls, cs, false);
            ls = cs;
            line_w = 0;
            avail = L.right - L.left;
        }
        if (line_w == 0 && chunk_w - trim > avail + 0.01f) {
            /* move below floats if they are what makes it too narrow */
            float y = L.y;
            while (L.right - L.left < chunk_w - trim && L.f) {
                float nb = bfc_next(L.f, L.oy + y);
                if (nb < 0) break;
                y = nb - L.oy;
                L.y = y;
                line_space(&L);
            }
            avail = L.right - L.left;
            int st_ws = iv.v[cs].kind == IT_TEXT ? iv.v[cs].st->white_space : WS_PRE;
            if (chunk_w - trim > avail + 0.01f && cs == i && it->kind == IT_TEXT && st_ws != WS_NOWRAP && st_ws != WS_PRE &&
                avail > 0) {
                if (split_word(&L, i, avail)) {
                    it = &iv.v[i];
                    chunk_w = it->w;
                    line_w = chunk_w;
                    finish_line(&L, ls, i + 1, false);
                    ls = cs = i + 1;
                    line_w = chunk_w = 0;
                    continue; /* the rest is item i+1 */
                }
            }
        }
        line_w += chunk_w;
        chunk_w = 0;
        cs = i + 1;
        if (it->kind == IT_BR) {
            finish_line(&L, ls, i + 1, true);
            ls = cs;
            line_w = 0;
        }
    }
    if (ls < iv.n) finish_line(&L, ls, iv.n, true);
    else if (L.npending) finish_line(&L, iv.n, iv.n, true);
    /* abspos items that never reached a line */
    for (int i = 0; i < iv.n; i++)
        if (iv.v[i].kind == IT_ABS && iv.v[i].box->gen != GEN) {
            box_t *c = iv.v[i].box;
            c->static_cb = b;
            c->static_x = L.left;
            c->static_y = L.y;
            list_abs(c);
        }
    b->nruns = (int)(L.runs.n / sizeof(struct run));
    b->runs = NULL;
    if (b->nruns) {
        b->runs = ar_alloc(&D->lmem, L.runs.n);
        memcpy(b->runs, L.runs.p, L.runs.n);
    }
    b->ndecos = (int)(L.decos.n / sizeof(struct deco));
    b->decos = NULL;
    if (b->ndecos) {
        b->decos = ar_alloc(&D->lmem, L.decos.n);
        memcpy(b->decos, L.decos.p, L.decos.n);
    }
    sb_free(&L.runs);
    sb_free(&L.decos);
    free(iv.v);
    b->baseline = L.first_bl;
    b->last_baseline = L.last_bl;
    b->h = L.y; /* the caller applies height and min/max */
}

/* ---------------------------------------------------------------- floats */
static void place_float(box_t *c, box_t *parent, struct bfc *f, float ox, float oy, float y_rel) {
    float cw = parent->w;
    resolve_edges(c, cw);
    c->cb = parent;
    if (c->kind == B_ATOMIC && c->atomic != AT_INLINE_BLOCK) size_atomic(c, cw, -1);
    else if (c->kind == B_TABLE) {
        table_width(c, cw);
        layout_inner(c, NULL, 0, 0, -1);
    } else {
        float w = spec_w(c, &c->st->width, cw);
        if (w < 0) w = stf_width(c, cw);
        c->w = clamp_w(c, w, cw);
        layout_inner(c, NULL, 0, 0, -1);
    }
    float mw = c->w + hext(c) + c->m[1] + c->m[3];
    float mh = c->h + vext(c) + c->m[0] + c->m[2];
    float y = oy + y_rel;
    if (f) {
        if (c->st->clear) y = fmaxf_(y, bfc_clear(f, c->st->clear));
        y = fmaxf_(y, f->last_top);
    }
    float l = ox, r = ox + cw;
    for (int guard = 0; f && guard < 200; guard++) {
        bfc_space(f, y, mh, ox, ox + cw, &l, &r);
        if (r - l >= mw - 0.05f || (l <= ox && r >= ox + cw)) break;
        float nb = bfc_next(f, y);
        if (nb < 0) break;
        y = nb;
    }
    float x = c->st->float_ == FL_LEFT ? l : r - mw;
    if (f) bfc_add(f, x, y, mw, mh, c->st->float_);
    c->x = x - ox + c->m[3] + c->b[3] + c->p[3];
    c->y = y - oy + c->m[0] + c->b[0] + c->p[0];
}

/* ---------------------------------------------------------------- block layout */
static bool collapses_top(const box_t *b) {
    return b->kind == B_BLOCK && !is_bfc_root(b) && b->st->border_width[0] == 0 &&
           len_resolve(&b->st->padding[0], 0) == 0 && b->st->padding[0].pct == 0 && !b->inline_ctx;
}

static bool collapses_bottom(const box_t *b) {
    return b->kind == B_BLOCK && !is_bfc_root(b) && b->st->border_width[2] == 0 &&
           len_resolve(&b->st->padding[2], 0) == 0 && b->st->padding[2].pct == 0 && !b->inline_ctx &&
           len_auto(&b->st->height) && (b->st->min_height.kind == LK_AUTO || len_resolve(&b->st->min_height, 0) == 0);
}

static box_t *first_inflow(const box_t *b) {
    for (box_t *c = b->first; c; c = c->next)
        if (!c->floated && !c->abspos) return box_block_level(c) ? c : NULL;
    return NULL;
}

/* the top margin of c, collapsed with its first child's if they touch */
static float top_chain(box_t *c, float cbw, int depth) {
    float m = len_auto(&c->st->margin[0]) ? 0 : len_resolve(&c->st->margin[0], cbw);
    if (depth < 32 && collapses_top(c)) {
        box_t *f = first_inflow(c);
        if (f) m = mcollapse(m, top_chain(f, cbw, depth + 1));
    }
    return m;
}

/* the width of a block-level box in normal flow; sets w, x and auto margins */
static void block_width(box_t *c, float cw, float cbh) {
    const style_t *s = c->st;
    bool mla = len_auto(&s->margin[3]), mra = len_auto(&s->margin[1]);
    float ext = hext(c);
    float w;
    if (c->kind == B_TABLE) {
        table_width(c, cw - c->m[1] - c->m[3]);
        w = c->w;
    } else if (c->kind == B_ATOMIC) {
        size_atomic(c, cw, cbh);
        w = c->w;
    } else {
        w = spec_w(c, &s->width, cw);
        if (w < 0) {
            /* auto width fills the line; auto margins only matter if max-width made it narrower */
            float avail = cw - c->m[1] - c->m[3] - ext;
            w = clamp_w(c, avail, cw);
            if (w >= avail - 0.01f) mla = mra = false;
        } else w = clamp_w(c, w, cw);
    }
    float rem = cw - w - ext - c->m[1] - c->m[3];
    /* inside <center> (text-align: -webkit-center), narrower blocks are centered too */
    if (!mla && !mra && rem > 0 && c->parent && c->parent->st->text_align == TA_WCENTER && c->m[1] == 0 && c->m[3] == 0)
        mla = mra = true;
    if (mla && mra) {
        c->m[3] += rem > 0 ? rem / 2 : 0;
        c->m[1] += rem > 0 ? rem / 2 : rem;
    } else if (mla) c->m[3] += rem;
    else if (mra) c->m[1] += rem;
    c->w = w;
    c->x = c->m[3] + c->b[3] + c->p[3];
}

static void layout_blocks(box_t *b, struct bfc *f, float ox, float oy, float cbh) {
    float cw = b->w;
    float cursor = 0, pend = 0;
    bool first = true;
    bool hoisted = collapses_top(b);
    float first_bl = -1, last_bl = -1;
    for (box_t *c = b->first; c; c = c->next) {
        c->cb = b;
        if (c->abspos) {
            c->static_cb = b;
            c->static_x = 0;
            c->static_y = cursor + pend;
            list_abs(c);
            continue;
        }
        if (c->floated) {
            place_float(c, b, f, ox, oy, cursor + (pend > 0 ? pend : 0));
            continue;
        }
        resolve_edges(c, cw);
        float mt = top_chain(c, cw, 0);
        if (!(first && hoisted)) pend = mcollapse(pend, mt);
        float y = cursor + pend;
        bool cleared = false;
        if (c->st->clear && f) {
            float cl = bfc_clear(f, c->st->clear) - oy;
            if (cl > y) {
                y = cl;
                pend = 0;
                cleared = true;
            }
        }
        block_width(c, cw, cbh);
        /* boxes that make a new formatting context stay clear of floats */
        if (f && f->n && is_bfc_root(c)) {
            float bh = c->kind == B_ATOMIC ? c->h + vext(c) : 1;
            float l, r;
            for (int guard = 0; guard < 100; guard++) {
                bfc_space(f, oy + y, bh, ox, ox + cw, &l, &r);
                float need = c->kind == B_ATOMIC || !len_auto(&c->st->width) ? c->w + hext(c) + c->m[1] + c->m[3] : 0;
                if (r - l >= need || need == 0) break;
                float nb = bfc_next(f, oy + y);
                if (nb < 0) break;
                y = nb - oy;
            }
            bfc_space(f, oy + y, bh, ox, ox + cw, &l, &r);
            l -= ox;
            r -= ox;
            if (l > 0 || r < cw) {
                if (len_auto(&c->st->width) && c->kind != B_ATOMIC && c->kind != B_TABLE) {
                    c->w = clamp_w(c, r - l - hext(c) - c->m[1] - c->m[3], cw);
                    c->x = l + c->m[3] + c->b[3] + c->p[3];
                } else if (c->x - c->m[3] - c->b[3] - c->p[3] < l) c->x = l + c->m[3] + c->b[3] + c->p[3];
            }
        }
        c->y = y + c->b[0] + c->p[0];
        /* layout_inner resolves c's own height against its containing block's: ours */
        if (c->kind != B_ATOMIC) layout_inner(c, f, ox + c->x, oy + c->y, cbh);
        float bh = c->h + vext(c);
        /* an empty block collapses through, unless clearance moved it below floats (clearfix) */
        bool empty = bh <= 0 && c->kind == B_BLOCK && !c->nruns && !cleared;
        float mb = c->m[2];
        if (empty) pend = mcollapse(pend, mb);
        else {
            cursor = y + bh;
            pend = mb;
            first = false;
        }
        if (c->baseline >= 0 && first_bl < 0) first_bl = c->y + c->baseline;
        if (c->last_baseline >= 0) last_bl = c->y + c->last_baseline;
        else if (c->baseline >= 0) last_bl = c->y + c->baseline;
    }
    b->baseline = first_bl;
    b->last_baseline = last_bl;
    if (collapses_bottom(b)) {
        b->m[2] = mcollapse(b->m[2], pend);
        b->h = cursor;
    } else b->h = cursor + (pend > 0 ? pend : 0);
    /* a block whose first child's margin was hoisted keeps the combined margin */
    (void)first;
}

/* ---------------------------------------------------------------- tables */
struct tcell {
    box_t *b;
    int row, col;
};

struct tgrid {
    box_t **rows;
    int nrows, ncols;
    struct tcell *cells;
    int ncells;
    box_t **row_group; /* the group of each row */
    box_t *caption;
    float *mn, *mx, *pct, *w;
    bool *fixed;
    float spacing;
};

static void grid_free(struct tgrid *g) {
    free(g->rows);
    free(g->cells);
    free(g->row_group);
    free(g->mn);
    free(g->mx);
    free(g->pct);
    free(g->w);
    free(g->fixed);
}

static void grid_build(box_t *t, struct tgrid *g) {
    memset(g, 0, sizeof *g);
    g->spacing = t->st->border_collapse ? 0 : t->st->border_spacing;
    /* rows in display order: header groups, bodies, footer groups */
    int nr = 0;
    for (int pass = 0; pass < 3; pass++)
        for (box_t *grp = t->first; grp; grp = grp->next) {
            if (grp->kind == B_CAPTION) {
                if (!g->caption) g->caption = grp;
                continue;
            }
            if (grp->kind != B_ROW_GROUP) continue;
            int want = grp->st->display == D_TABLE_HEADER_GROUP ? 0 : grp->st->display == D_TABLE_FOOTER_GROUP ? 2 : 1;
            if (want != pass) continue;
            for (box_t *r = grp->first; r; r = r->next)
                if (r->kind == B_ROW) nr++;
        }
    g->rows = calloc((size_t)nr + 1, sizeof(box_t *));
    g->row_group = calloc((size_t)nr + 1, sizeof(box_t *));
    int ri = 0, ncells = 0;
    for (int pass = 0; pass < 3; pass++)
        for (box_t *grp = t->first; grp; grp = grp->next) {
            if (grp->kind != B_ROW_GROUP) continue;
            int want = grp->st->display == D_TABLE_HEADER_GROUP ? 0 : grp->st->display == D_TABLE_FOOTER_GROUP ? 2 : 1;
            if (want != pass) continue;
            for (box_t *r = grp->first; r; r = r->next)
                if (r->kind == B_ROW) {
                    g->row_group[ri] = grp;
                    g->rows[ri++] = r;
                    for (box_t *c = r->first; c; c = c->next) ncells++;
                }
        }
    g->nrows = nr;
    g->cells = calloc((size_t)ncells + 1, sizeof *g->cells);
    /* place cells, skipping slots taken by rowspans */
    int cap = 16;
    int *busy = calloc((size_t)cap, sizeof(int)); /* rows remaining for each column */
    int maxc = 0;
    for (int r = 0; r < nr; r++) {
        int col = 0;
        for (box_t *c = g->rows[r]->first; c; c = c->next) {
            if (c->kind != B_CELL) continue;
            while (col < cap && busy[col] > 0) col++;
            int span = c->colspan, rs = c->rowspan;
            if (rs > nr - r) rs = nr - r;
            if (rs < 1) rs = 1;
            while (col + span > cap) {
                busy = realloc(busy, sizeof(int) * (size_t)cap * 2);
                memset(busy + cap, 0, sizeof(int) * (size_t)cap);
                cap *= 2;
            }
            for (int k = 0; k < span; k++) busy[col + k] = rs;
            c->col = col;
            c->row = r;
            c->rowspan = rs;
            g->cells[g->ncells++] = (struct tcell){c, r, col};
            col += span;
            if (col > maxc) maxc = col;
        }
        for (int k = 0; k < cap; k++)
            if (busy[k] > 0) busy[k]--;
    }
    free(busy);
    g->ncols = maxc;
    int n = maxc ? maxc : 1;
    g->mn = calloc((size_t)n, sizeof(float));
    g->mx = calloc((size_t)n, sizeof(float));
    g->pct = calloc((size_t)n, sizeof(float));
    g->w = calloc((size_t)n, sizeof(float));
    g->fixed = calloc((size_t)n, sizeof(bool));
    /* column minimum and maximum widths */
    for (int pass = 0; pass < 2; pass++)
        for (int i = 0; i < g->ncells; i++) {
            box_t *c = g->cells[i].b;
            if ((pass == 0) != (c->colspan == 1)) continue;
            float mn, mx;
            resolve_edges(c, 0);
            intrinsic(c, &mn, &mx);
            float ext = hext(c);
            mn += ext;
            mx += ext;
            const len_t *wl = &c->st->width;
            float fixed = -1, pct = 0;
            if (wl->kind == LK_LEN && wl->pct == 0) fixed = wl->px + (c->st->box_sizing ? 0 : ext);
            else if (wl->kind == LK_LEN && wl->pct > 0 && wl->px == 0) pct = wl->pct;
            if (fixed >= 0) mx = fmaxf_(mn, fixed);
            int c0 = g->cells[i].col, span = c->colspan;
            if (c0 + span > g->ncols) span = g->ncols - c0;
            if (span <= 1) {
                g->mn[c0] = fmaxf_(g->mn[c0], mn);
                g->mx[c0] = fmaxf_(g->mx[c0], mx);
                if (fixed >= 0) g->fixed[c0] = true;
                if (pct > g->pct[c0]) g->pct[c0] = pct;
                continue;
            }
            /* spanning cells: spread what the columns lack in proportion to their maxima */
            float smn = g->spacing * (float)(span - 1), smx = smn, tot = 0;
            for (int k = 0; k < span; k++) {
                smn += g->mn[c0 + k];
                smx += g->mx[c0 + k];
                tot += g->mx[c0 + k];
            }
            if (mn > smn)
                for (int k = 0; k < span; k++)
                    g->mn[c0 + k] += (mn - smn) * (tot > 0 ? g->mx[c0 + k] / tot : 1.0f / (float)span);
            if (mx > smx)
                for (int k = 0; k < span; k++)
                    g->mx[c0 + k] += (mx - smx) * (tot > 0 ? g->mx[c0 + k] / tot : 1.0f / (float)span);
            if (pct > 0)
                for (int k = 0; k < span; k++)
                    if (g->pct[c0 + k] == 0) g->pct[c0 + k] = pct / (float)span;
        }
    for (int j = 0; j < g->ncols; j++)
        if (g->mx[j] < g->mn[j]) g->mx[j] = g->mn[j];
}

static void table_intrinsic(box_t *t, float *mn, float *mx) {
    struct tgrid g;
    grid_build(t, &g);
    float sp = g.spacing * (float)(g.ncols + 1);
    *mn = *mx = sp;
    for (int j = 0; j < g.ncols; j++) {
        *mn += g.mn[j];
        *mx += g.mx[j];
    }
    if (g.caption) {
        float cmn, cmx;
        outer_intrinsic(g.caption, &cmn, &cmx);
        *mn = fmaxf_(*mn, cmn);
    }
    grid_free(&g);
}

/* the table's content width for an available width */
static void table_width(box_t *t, float cw) {
    resolve_edges(t, cw);
    float mn, mx;
    table_intrinsic(t, &mn, &mx);
    float ext = hext(t);
    float sw = spec_w(t, &t->st->width, cw);
    float w;
    if (sw >= 0) w = fmaxf_(sw, mn);
    else {
        float avail = cw - ext - t->m[1] - t->m[3];
        w = mx <= avail ? mx : fmaxf_(avail, mn);
        /* percentage columns make the table as wide as it may be */
        (void)0;
    }
    t->w = clamp_w(t, w, cw);
    if (t->w < mn) t->w = mn;
}

static void layout_table(box_t *t, float cbh) {
    struct tgrid g;
    grid_build(t, &g);
    float sp = g.spacing;
    int nc = g.ncols;
    float avail = t->w - sp * (float)(nc + 1);
    if (avail < 0) avail = 0;
    /* column widths */
    float used = 0, smn = 0, smx = 0, auto_mx = 0;
    for (int j = 0; j < nc; j++) {
        if (g.pct[j] > 0) {
            g.w[j] = fmaxf_(g.mn[j], g.pct[j] * avail / 100);
            used += g.w[j];
        } else {
            smn += g.mn[j];
            smx += g.mx[j];
            if (!g.fixed[j]) auto_mx += g.mx[j];
        }
    }
    float rem = avail - used;
    int nauto = 0;
    for (int j = 0; j < nc; j++)
        if (g.pct[j] == 0 && !g.fixed[j]) nauto++;
    for (int j = 0; j < nc; j++) {
        if (g.pct[j] > 0) continue;
        if (rem >= smx) {
            float extra = rem - smx;
            if (g.fixed[j]) g.w[j] = g.mx[j] + (nauto ? 0 : extra * (smx > 0 ? g.mx[j] / smx : 1.0f / (float)nc));
            else g.w[j] = g.mx[j] + (auto_mx > 0 ? extra * g.mx[j] / auto_mx : extra / (float)nauto);
        } else if (rem >= smn && smx > smn) g.w[j] = g.mn[j] + (g.mx[j] - g.mn[j]) * (rem - smn) / (smx - smn);
        else g.w[j] = g.mn[j];
    }
    float *cx = calloc((size_t)nc + 1, sizeof(float));
    float x = sp;
    for (int j = 0; j < nc; j++) {
        cx[j] = x;
        x += g.w[j] + sp;
    }
    cx[nc] = x;
    if (x > t->w) t->w = x;
    /* caption: above the rows, or below them with caption-side: bottom */
    float y = 0, cap_h = 0;
    bool cap_bottom = g.caption && g.caption->st->caption_bottom;
    if (g.caption) {
        box_t *c = g.caption;
        c->cb = t;
        resolve_edges(c, t->w);
        c->w = fmaxf_(0, t->w - hext(c) - c->m[1] - c->m[3]);
        c->x = c->m[3] + c->b[3] + c->p[3];
        c->y = c->m[0] + c->b[0] + c->p[0];
        layout_inner(c, NULL, 0, 0, -1);
        cap_h = c->h + vext(c) + c->m[0] + c->m[2];
        if (!cap_bottom) y = cap_h;
    }
    /* lay out cells at their widths */
    float *rh = calloc((size_t)g.nrows + 1, sizeof(float));
    for (int r = 0; r < g.nrows; r++) {
        float h = spec_h(g.rows[r], &g.rows[r]->st->height, -1);
        rh[r] = h > 0 ? h : 0;
    }
    for (int i = 0; i < g.ncells; i++) {
        box_t *c = g.cells[i].b;
        int c0 = g.cells[i].col, span = c->colspan;
        if (c0 + span > nc) span = nc - c0;
        float w = cx[c0 + span] - sp - cx[c0];
        c->cb = t;
        resolve_edges(c, t->w);
        c->w = fmaxf_(0, w - hext(c));
        c->content_dy = 0;
        layout_inner(c, NULL, 0, 0, -1);
        float sh = spec_h(c, &c->st->height, -1);
        float bh = fmaxf_(c->h, sh) + vext(c);
        c->h = fmaxf_(c->h, sh);
        if (c->rowspan == 1 && bh > rh[g.cells[i].row]) rh[g.cells[i].row] = bh;
    }
    /* rowspans: grow the last spanned row if the cell does not fit */
    for (int i = 0; i < g.ncells; i++) {
        box_t *c = g.cells[i].b;
        if (c->rowspan <= 1) continue;
        int r0 = g.cells[i].row, r1 = r0 + c->rowspan - 1;
        float have = sp * (float)(c->rowspan - 1);
        for (int r = r0; r <= r1; r++) have += rh[r];
        float need = c->h + vext(c);
        if (need > have) rh[r1] += need - have;
    }
    float *ry = calloc((size_t)g.nrows + 1, sizeof(float));
    y += sp;
    for (int r = 0; r < g.nrows; r++) {
        ry[r] = y;
        y += rh[r] + sp;
    }
    if (!g.nrows) y = g.caption ? y : 0;
    /* a specified table height stretches the rows */
    float th = spec_h(t, &t->st->height, cbh);
    if (th > y && g.nrows) {
        float extra = (th - y) / (float)g.nrows;
        float acc = 0;
        for (int r = 0; r < g.nrows; r++) {
            ry[r] += acc;
            rh[r] += extra;
            acc += extra;
        }
        y = th;
    }
    /* place cells, stretched to their rows, with vertical-align */
    for (int i = 0; i < g.ncells; i++) {
        box_t *c = g.cells[i].b;
        int r0 = g.cells[i].row, r1 = r0 + c->rowspan - 1;
        float h = ry[r1] + rh[r1] - ry[r0];
        float content = c->h;
        c->x = cx[g.cells[i].col] + c->b[3] + c->p[3];
        c->y = ry[r0] + c->b[0] + c->p[0];
        c->h = fmaxf_(0, h - vext(c));
        int va = c->st->vertical_align;
        if (va == VA_MIDDLE) c->content_dy = (c->h - content) / 2;
        else if (va == VA_BOTTOM) c->content_dy = c->h - content;
        if (c->content_dy < 0) c->content_dy = 0;
    }
    /* rows and groups get geometry for their backgrounds */
    for (int r = 0; r < g.nrows; r++) {
        box_t *row = g.rows[r];
        row->cb = t;
        row->x = sp;
        row->y = ry[r];
        row->w = cx[nc] - 2 * sp;
        row->h = rh[r];
    }
    for (box_t *grp = t->first; grp; grp = grp->next) {
        if (grp->kind != B_ROW_GROUP) continue;
        float y0 = INF, y1 = -INF;
        for (int r = 0; r < g.nrows; r++)
            if (g.row_group[r] == grp) {
                y0 = fminf_(y0, ry[r]);
                y1 = fmaxf_(y1, ry[r] + rh[r]);
            }
        grp->cb = t;
        grp->x = sp;
        grp->w = cx[nc] - 2 * sp;
        if (y0 > y1) y0 = y1 = 0;
        grp->y = y0;
        grp->h = y1 - y0;
    }
    if (cap_bottom) {
        box_t *c = g.caption;
        c->y = y + c->m[0] + c->b[0] + c->p[0];
        y += cap_h;
    }
    t->h = y;
    if (g.nrows) {
        box_t *c0 = NULL;
        for (int i = 0; i < g.ncells; i++)
            if (g.cells[i].row == 0 && g.cells[i].b->baseline >= 0) {
                c0 = g.cells[i].b;
                break;
            }
        t->baseline = c0 ? c0->y + c0->content_dy + c0->baseline : -1;
    }
    t->last_baseline = t->baseline;
    free(cx);
    free(rh);
    free(ry);
    grid_free(&g);
}

/* ---------------------------------------------------------------- flexbox */
struct fitem {
    box_t *b;
    float base, hypo, mn, mx, ext, size, cross;
    int align;
};

static void layout_flex(box_t *b, float cbh) {
    const style_t *st = b->st;
    bool column = st->flex_direction == FD_COLUMN || st->flex_direction == FD_COLUMN_REVERSE;
    bool reverse = st->flex_direction == FD_ROW_REVERSE || st->flex_direction == FD_COLUMN_REVERSE;
    float cw = b->w;
    float defh = spec_h(b, &st->height, cbh);
    int n = 0;
    for (box_t *c = b->first; c; c = c->next) n++;
    struct fitem *it = calloc((size_t)n + 1, sizeof *it);
    int k = 0;
    for (box_t *c = b->first; c; c = c->next) {
        c->cb = b;
        if (c->abspos) {
            c->static_cb = b;
            c->static_x = 0;
            c->static_y = 0;
            list_abs(c);
            continue;
        }
        it[k++].b = c;
    }
    n = k;
    /* stable sort by order */
    for (int i = 1; i < n; i++)
        for (int j = i; j > 0 && it[j - 1].b->st->order > it[j].b->st->order; j--) {
            struct fitem t = it[j];
            it[j] = it[j - 1];
            it[j - 1] = t;
        }
    float gap_main = column ? st->gap_row : st->gap_col, gap_cross = column ? st->gap_col : st->gap_row;
    float first_bl = -1;

    if (column) {
        float y = 0;
        for (int i = 0; i < n; i++) {
            box_t *c = it[i].b;
            resolve_edges(c, cw);
            int al = c->st->align_self != 255 ? c->st->align_self : st->align_items;
            float w = spec_w(c, &c->st->width, cw);
            if (c->kind == B_ATOMIC && c->atomic != AT_INLINE_BLOCK) {
                size_atomic(c, cw, -1);
                w = c->w;
            } else if (c->kind == B_TABLE) {
                table_width(c, cw);
                w = c->w;
            } else if (w < 0) w = al == AI_STRETCH ? cw - hext(c) - c->m[1] - c->m[3] : stf_width(c, cw);
            c->w = clamp_w(c, w, cw);
            if (c->kind != B_ATOMIC || c->atomic == AT_INLINE_BLOCK) layout_inner(c, NULL, 0, 0, defh);
            it[i].size = c->h + vext(c) + c->m[0] + c->m[2];
            it[i].align = al;
        }
        /* grow into a definite height */
        float total = gap_main * (float)(n > 0 ? n - 1 : 0);
        float grow = 0;
        for (int i = 0; i < n; i++) total += it[i].size, grow += it[i].b->st->flex_grow;
        float free_space = defh >= 0 ? defh - total : 0;
        if (free_space > 0 && grow > 0) {
            for (int i = 0; i < n; i++) {
                float add = free_space * it[i].b->st->flex_grow / grow;
                it[i].b->h += add;
                it[i].size += add;
            }
            free_space = 0;
        }
        float start = 0, between = gap_main;
        if (free_space > 0) {
            switch (st->justify_content) {
            case JC_END: start = free_space; break;
            case JC_CENTER: start = free_space / 2; break;
            case JC_BETWEEN: if (n > 1) between += free_space / (float)(n - 1); break;
            case JC_AROUND: start = free_space / (float)n / 2; between += free_space / (float)n; break;
            case JC_EVENLY: start = free_space / (float)(n + 1); between += free_space / (float)(n + 1); break;
            }
        }
        y = start;
        for (int ii = 0; ii < n; ii++) {
            int i = reverse ? n - 1 - ii : ii;
            box_t *c = it[i].b;
            float mw = c->w + hext(c) + c->m[1] + c->m[3];
            float xoff = 0;
            if (len_auto(&c->st->margin[3]) && len_auto(&c->st->margin[1])) xoff = (cw - mw) / 2;
            else if (len_auto(&c->st->margin[3])) xoff = cw - mw;
            else if (it[i].align == AI_CENTER) xoff = (cw - mw) / 2;
            else if (it[i].align == AI_END) xoff = cw - mw;
            c->x = xoff + c->m[3] + c->b[3] + c->p[3];
            c->y = y + c->m[0] + c->b[0] + c->p[0];
            if (first_bl < 0 && c->baseline >= 0) first_bl = c->y + c->baseline;
            y += it[i].size + between;
        }
        b->h = n ? y - between : 0;
        if (defh >= 0) b->h = defh;
        b->baseline = b->last_baseline = first_bl;
        free(it);
        return;
    }

    /* row: hypothetical main sizes */
    for (int i = 0; i < n; i++) {
        box_t *c = it[i].b;
        resolve_edges(c, cw);
        it[i].ext = hext(c) + c->m[1] + c->m[3];
        float mn, mx;
        if (c->kind == B_ATOMIC && c->atomic != AT_INLINE_BLOCK) {
            size_atomic(c, cw, defh);
            mn = mx = c->w;
        } else if (c->kind == B_TABLE) {
            table_intrinsic(c, &mn, &mx);
        } else intrinsic(c, &mn, &mx);
        const len_t *fb = &c->st->flex_basis;
        float base = -1;
        if (fb->kind == LK_LEN || fb->kind == LK_EXPR) {
            base = len_resolve(fb, cw);
            if (c->st->box_sizing) base -= hext(c);
        }
        if (base < 0) base = spec_w(c, &c->st->width, cw);
        if (base < 0) base = mx;
        if (base < 0) base = 0;
        float smin = spec_w(c, &c->st->min_width, cw);
        float auto_min = c->st->overflow == OV_VISIBLE ? mn : 0;
        float sw = spec_w(c, &c->st->width, cw);
        if (sw >= 0 && sw < auto_min) auto_min = sw;
        it[i].mn = smin >= 0 ? smin : auto_min;
        float smax = spec_w(c, &c->st->max_width, cw);
        it[i].mx = smax >= 0 ? smax : INF;
        if (it[i].mx < it[i].mn) it[i].mx = it[i].mn;
        it[i].base = base;
        it[i].hypo = fminf_(fmaxf_(base, it[i].mn), it[i].mx);
        it[i].align = c->st->align_self != 255 ? c->st->align_self : st->align_items;
    }
    /* lines */
    float y = 0;
    int i0 = 0;
    bool single = !st->flex_wrap;
    while (i0 < n) {
        int i1 = i0;
        float used = 0;
        if (single) i1 = n;
        else {
            while (i1 < n) {
                float add = it[i1].hypo + it[i1].ext + (i1 > i0 ? gap_main : 0);
                if (i1 > i0 && used + add > cw + 0.01f) break;
                used += add;
                i1++;
            }
        }
        used = 0;
        float grow = 0, shrink_w = 0;
        for (int i = i0; i < i1; i++) {
            used += it[i].hypo + it[i].ext + (i > i0 ? gap_main : 0);
            grow += it[i].b->st->flex_grow;
            shrink_w += it[i].b->st->flex_shrink * it[i].base;
        }
        float free_space = cw - used;
        for (int i = i0; i < i1; i++) {
            float s = it[i].hypo;
            if (free_space > 0 && grow > 0) {
                float share = grow < 1 ? free_space * grow : free_space;
                s += share * it[i].b->st->flex_grow / grow;
            } else if (free_space < 0 && shrink_w > 0)
                s += free_space * it[i].b->st->flex_shrink * it[i].base / shrink_w;
            it[i].size = fminf_(fmaxf_(s, it[i].mn), it[i].mx);
        }
        /* lay out the items at their sizes */
        float cross = 0, line_bl = -1;
        for (int i = i0; i < i1; i++) {
            box_t *c = it[i].b;
            if (!(c->kind == B_ATOMIC && c->atomic != AT_INLINE_BLOCK)) {
                c->w = it[i].size;
                layout_inner(c, NULL, 0, 0, defh);
            } else c->w = it[i].size;
            it[i].cross = c->h + vext(c) + c->m[0] + c->m[2];
            cross = fmaxf_(cross, it[i].cross);
            if (it[i].align == AI_BASELINE && c->baseline >= 0)
                line_bl = fmaxf_(line_bl, c->m[0] + c->b[0] + c->p[0] + c->baseline);
        }
        if (single && defh >= 0) cross = defh;
        /* main axis positions */
        float total = gap_main * (float)(i1 - i0 - 1);
        int nauto = 0;
        for (int i = i0; i < i1; i++) {
            total += it[i].size + it[i].ext;
            nauto += len_auto(&it[i].b->st->margin[3]) + len_auto(&it[i].b->st->margin[1]);
        }
        float left = cw - total;
        float start = 0, between = gap_main, auto_m = 0;
        if (left > 0 && nauto) auto_m = left / (float)nauto;
        else if (left > 0) {
            switch (st->justify_content) {
            case JC_END: start = left; break;
            case JC_CENTER: start = left / 2; break;
            case JC_BETWEEN: if (i1 - i0 > 1) between += left / (float)(i1 - i0 - 1); break;
            case JC_AROUND: start = left / (float)(i1 - i0) / 2; between += left / (float)(i1 - i0); break;
            case JC_EVENLY: start = left / (float)(i1 - i0 + 1); between += left / (float)(i1 - i0 + 1); break;
            }
        }
        float x = start;
        for (int ii = i0; ii < i1; ii++) {
            int i = reverse ? i1 - 1 - (ii - i0) : ii;
            box_t *c = it[i].b;
            if (len_auto(&c->st->margin[3])) x += auto_m;
            float bx = x + c->m[3];
            float off = 0;
            switch (it[i].align) {
            case AI_CENTER: off = (cross - it[i].cross) / 2; break;
            case AI_END: off = cross - it[i].cross; break;
            case AI_BASELINE:
                if (line_bl >= 0 && c->baseline >= 0) off = line_bl - (c->m[0] + c->b[0] + c->p[0] + c->baseline);
                break;
            case AI_STRETCH:
                if (len_auto(&c->st->height) && !(c->kind == B_ATOMIC && c->atomic != AT_INLINE_BLOCK)) {
                    float want = cross - vext(c) - c->m[0] - c->m[2];
                    want = clamp_h(c, want, defh);
                    if (want > c->h) c->h = want;
                }
                break;
            }
            c->x = bx + c->b[3] + c->p[3];
            c->y = y + off + c->m[0] + c->b[0] + c->p[0];
            if (first_bl < 0 && c->baseline >= 0) first_bl = c->y + c->baseline;
            x += it[i].size + it[i].ext + between;
            if (len_auto(&c->st->margin[1])) x += auto_m;
        }
        y += cross + gap_cross;
        i0 = i1;
    }
    b->h = n ? y - gap_cross : 0;
    if (defh >= 0) b->h = defh;
    b->baseline = b->last_baseline = first_bl;
    free(it);
}

/* ---------------------------------------------------------------- grid */
/* A simplified CSS grid: explicit tracks (lengths, percentages, fr, auto, min/max-content, minmax(),
   fit-content(), repeat() including auto-fill/auto-fit), named areas, line and span placement,
   sparse auto-placement by rows or columns, gaps, and justify/align of items and of the column
   tracks. Spanning items grow the tracks they cross evenly. */
#define GR_MAX_TRACKS 1000
#define GR_MAX_ROWS 10000

struct gitem {
    box_t *b;
    int r0, r1, c0, c1;  /* rows [r0, r1) and columns [c0, c1); r0/c0 -1 while auto */
    float mn, mx, h;    /* column contributions (margin box) and the laid-out margin-box height */
};

struct gtr {
    struct gtrack t;
    float base, limit, fmax, size, pos;
    bool flex, collapsed, used;
};

struct grid {
    int n, nr, nc;
    struct gitem *it;
    struct gtr *row, *col;
};

/* an area edge: the first line of name (or of name-start), or the line after it for an end edge */
static int gr_area_line(const struct gareas *a, const char *name, bool col, bool end) {
    if (!a) return -1;
    size_t nl = strlen(name);
    if (nl > 6 && !strcmp(name + nl - 6, "-start")) nl -= 6, end = false;
    else if (nl > 4 && !strcmp(name + nl - 4, "-end")) nl -= 4, end = true;
    int lo = (1 << 30), hi = -1;
    for (int r = 0; r < a->rows; r++)
        for (int c = 0; c < a->cols; c++) {
            const char *s = a->cell[r * a->cols + c];
            if (!s || strlen(s) != nl || strncmp(s, name, nl)) continue;
            int k = col ? c : r;
            if (k < lo) lo = k;
            if (k > hi) hi = k;
        }
    if (hi < 0) return -1;
    return end ? hi + 1 : lo;
}

static int gr_line(const struct gline *l, const struct gareas *a, bool col, bool end, int nexp) {
    if (l->kind == GL_LINE) {
        int k = l->n > 0 ? l->n - 1 : nexp + 1 + l->n;
        return k < 0 ? 0 : k > GR_MAX_TRACKS ? GR_MAX_TRACKS : k;
    }
    if (l->kind == GL_NAME) return gr_area_line(a, l->name, col, end);
    return -1;
}

/* one axis of an item's placement: [*s, *e), or *s = -1 and *e the span when it is auto-placed */
static void gr_resolve(const struct gline *ls, const struct gline *le, const struct gareas *a, bool col, int nexp,
                       int *s, int *e) {
    int sl = gr_line(ls, a, col, false, nexp), el = gr_line(le, a, col, true, nexp);
    int ss = ls->kind == GL_SPAN ? ls->n : 0, es = le->kind == GL_SPAN ? le->n : 0;
    if (sl >= 0 && el >= 0) {
        if (el < sl) {
            int t = sl;
            sl = el;
            el = t;
        }
        if (el == sl) el = sl + 1;
    } else if (sl >= 0) el = sl + (es ? es : 1);
    else if (el >= 0) {
        sl = el - (ss ? ss : 1);
        if (sl < 0) sl = 0;
        if (el <= sl) el = sl + 1;
    } else {
        *s = -1;
        *e = ss ? ss : es ? es : 1;
        return;
    }
    if (el > GR_MAX_TRACKS) el = GR_MAX_TRACKS;
    if (sl >= el) sl = el - 1;
    *s = sl;
    *e = el;
}

/* the explicit tracks of a template, with an auto-fill/auto-fit repeat expanded to fit avail */
static struct gtrack *gr_explicit(const struct gtemplate *g, float avail, float gap, int *n, int *rep_at, int *rep_cnt) {
    *n = 0;
    *rep_at = *rep_cnt = 0;
    if (!g) return NULL;
    int reps = 1;
    if (g->rep_n && avail >= 0) {
        float fixed = 0, rep = 0;
        bool definite = true;
        for (int i = 0; i < g->n; i++) {
            const struct gtrack *t = &g->t[i];
            float s = t->max_kind == GT_LEN ? len_resolve(&t->max, avail) : t->min_kind == GT_LEN ? len_resolve(&t->min, avail) : -1;
            if (t->max_kind == GT_LEN && t->min_kind == GT_LEN) s = fmaxf_(s, len_resolve(&t->min, avail));
            bool in_rep = i >= g->rep_at && i < g->rep_at + g->rep_n;
            if (s < 0) {
                if (in_rep) definite = false;
                s = 0;
            }
            if (in_rep) rep += s + gap;
            else fixed += s + gap;
        }
        if (definite && rep > 0) {
            reps = (int)floorf((avail + gap - fixed) / rep);
            if (reps < 1) reps = 1;
            if (reps * g->rep_n > GR_MAX_TRACKS - g->n) reps = (GR_MAX_TRACKS - g->n) / g->rep_n;
        }
    }
    int total = g->n + (g->rep_n ? (reps - 1) * g->rep_n : 0);
    struct gtrack *v = malloc(sizeof *v * (size_t)(total > 0 ? total : 1));
    int k = 0;
    for (int i = 0; i < g->n; i++) {
        if (g->rep_n && i == g->rep_at) {
            *rep_at = k;
            for (int r = 0; r < reps; r++)
                for (int j = 0; j < g->rep_n; j++) v[k++] = g->t[g->rep_at + j];
            *rep_cnt = reps * g->rep_n;
            i += g->rep_n - 1;
            continue;
        }
        v[k++] = g->t[i];
    }
    *n = k;
    return v;
}

static bool gr_free_at(const uint8_t *occ, int rows, int nc, int r0, int r1, int c0, int c1) {
    for (int r = r0; r < r1 && r < rows; r++)
        for (int c = c0; c < c1; c++)
            if (occ[r * nc + c]) return false;
    return true;
}

/* auto-placement along rows (columns are handled by swapping the axes first) */
static int gr_place(struct gitem *it, int n, int nc, int nr) {
    for (int i = 0; i < n; i++) {
        if (it[i].c0 < 0 && it[i].c1 > nc) nc = it[i].c1;
        if (it[i].c0 >= 0 && it[i].c1 > nc) nc = it[i].c1;
    }
    if (nc < 1) nc = 1;
    if (nc > GR_MAX_TRACKS) nc = GR_MAX_TRACKS;
    int cap = 64;
    uint8_t *occ = calloc((size_t)cap * nc, 1);
    int rows = 0;
#define GR_MARK(I)                                                                                                     \
    do {                                                                                                               \
        if (it[I].r1 > cap) {                                                                                          \
            int nc2 = cap;                                                                                             \
            while (nc2 < it[I].r1) nc2 *= 2;                                                                           \
            occ = realloc(occ, (size_t)nc2 * nc);                                                                      \
            memset(occ + (size_t)cap * nc, 0, (size_t)(nc2 - cap) * nc);                                               \
            cap = nc2;                                                                                                 \
        }                                                                                                              \
        for (int r = it[I].r0; r < it[I].r1; r++)                                                                      \
            for (int c = it[I].c0; c < it[I].c1 && c < nc; c++) occ[r * nc + c] = 1;                                   \
        if (it[I].r1 > rows) rows = it[I].r1;                                                                          \
    } while (0)
    /* fully placed items, then those locked to rows */
    for (int i = 0; i < n; i++)
        if (it[i].r0 >= 0 && it[i].c0 >= 0) GR_MARK(i);
    for (int i = 0; i < n; i++) {
        if (it[i].r0 < 0 || it[i].c0 >= 0) continue;
        int span = it[i].c1 > nc ? nc : it[i].c1, c = 0;
        while (c + span < nc && !gr_free_at(occ, rows, nc, it[i].r0, it[i].r1, c, c + span)) c++;
        it[i].c0 = c;
        it[i].c1 = c + span;
        GR_MARK(i);
    }
    /* the rest, with a cursor */
    int cr = 0, cc = 0;
    for (int i = 0; i < n; i++) {
        if (it[i].r0 >= 0) continue;
        int rspan = it[i].r1;
        if (it[i].c0 >= 0) {
            if (it[i].c0 < cc) cr++;
            cc = it[i].c0;
            while (cr < GR_MAX_ROWS && !gr_free_at(occ, rows, nc, cr, cr + rspan, it[i].c0, it[i].c1)) cr++;
        } else {
            int span = it[i].c1 > nc ? nc : it[i].c1;
            for (;;) {
                if (cc + span > nc) {
                    cr++;
                    cc = 0;
                }
                if (cr >= GR_MAX_ROWS || gr_free_at(occ, rows, nc, cr, cr + rspan, cc, cc + span)) break;
                cc++;
            }
            it[i].c0 = cc;
            it[i].c1 = cc + span;
        }
        if (cr >= GR_MAX_ROWS) cr = GR_MAX_ROWS - rspan;
        it[i].r0 = cr;
        it[i].r1 = cr + rspan;
        GR_MARK(i);
        cc = it[i].c1;
    }
#undef GR_MARK
    free(occ);
    return rows > nr ? rows : nr;
}

static void gr_init_tracks(struct gtr *tr, int nt, const struct gtrack *ex, int nex, const struct gtemplate *autot) {
    for (int i = 0; i < nt; i++) {
        memset(&tr[i], 0, sizeof tr[i]);
        if (i < nex) tr[i].t = ex[i];
        else if (autot && autot->n) tr[i].t = autot->t[(i - nex) % autot->n];
        else tr[i].t.min_kind = tr[i].t.max_kind = GT_AUTO;
    }
}

static void gr_free(struct grid *g) {
    free(g->it);
    free(g->row);
    free(g->col);
}

/* collect and place the items of b; cw is the content width (-1 while measuring) */
static void gr_setup(struct grid *g, box_t *b, float cw) {
    const style_t *st = b->st;
    memset(g, 0, sizeof *g);
    int n = 0;
    for (box_t *c = b->first; c; c = c->next) n++;
    g->it = calloc((size_t)n + 1, sizeof *g->it);
    for (box_t *c = b->first; c; c = c->next)
        if (!c->abspos) g->it[g->n++].b = c;
    n = g->n;
    for (int i = 1; i < n; i++)
        for (int j = i; j > 0 && g->it[j - 1].b->st->order > g->it[j].b->st->order; j--) {
            struct gitem t = g->it[j];
            g->it[j] = g->it[j - 1];
            g->it[j - 1] = t;
        }
    int ncx, nrx, cra, crn, rra, rrn;
    struct gtrack *cx = gr_explicit(st->grid_cols, cw, st->gap_col, &ncx, &cra, &crn);
    struct gtrack *rx = gr_explicit(st->grid_rows, -1, st->gap_row, &nrx, &rra, &rrn);
    const struct gareas *a = st->grid_areas;
    int ec = ncx, er = nrx;
    if (a && a->cols > ec) ec = a->cols;
    if (a && a->rows > er) er = a->rows;
    for (int i = 0; i < n; i++) {
        const style_t *s = g->it[i].b->st;
        gr_resolve(&s->grid_place[0], &s->grid_place[2], a, false, er, &g->it[i].r0, &g->it[i].r1);
        gr_resolve(&s->grid_place[1], &s->grid_place[3], a, true, ec, &g->it[i].c0, &g->it[i].c1);
    }
    if (st->grid_flow_col) {
        for (int i = 0; i < n; i++) {
            struct gitem *t = &g->it[i];
            int a0 = t->r0, a1 = t->r1;
            t->r0 = t->c0, t->r1 = t->c1, t->c0 = a0, t->c1 = a1;
        }
        g->nc = gr_place(g->it, n, er, ec);
        for (int i = 0; i < n; i++) {
            struct gitem *t = &g->it[i];
            int a0 = t->r0, a1 = t->r1;
            t->r0 = t->c0, t->r1 = t->c1, t->c0 = a0, t->c1 = a1;
        }
        g->nr = er;
        for (int i = 0; i < n; i++)
            if (g->it[i].r1 > g->nr) g->nr = g->it[i].r1;
    } else {
        g->nr = gr_place(g->it, n, ec, er);
        g->nc = ec;
        for (int i = 0; i < n; i++)
            if (g->it[i].c1 > g->nc) g->nc = g->it[i].c1;
    }
    g->col = calloc((size_t)g->nc + 1, sizeof *g->col);
    g->row = calloc((size_t)g->nr + 1, sizeof *g->row);
    gr_init_tracks(g->col, g->nc, cx, ncx, st->grid_auto_cols);
    gr_init_tracks(g->row, g->nr, rx, nrx, st->grid_auto_rows);
    for (int i = 0; i < n; i++) {
        for (int c = g->it[i].c0; c < g->it[i].c1; c++) g->col[c].used = true;
        for (int r = g->it[i].r0; r < g->it[i].r1; r++) g->row[r].used = true;
    }
    if (st->grid_cols && st->grid_cols->rep_fit)
        for (int c = cra; c < cra + crn && c < g->nc; c++) g->col[c].collapsed = !g->col[c].used;
    if (st->grid_rows && st->grid_rows->rep_fit)
        for (int r = rra; r < rra + rrn && r < g->nr; r++) g->row[r].collapsed = !g->row[r].used;
    free(cx);
    free(rx);
}

static bool gr_intrinsic_kind(int k) { return k == GT_AUTO || k == GT_MIN || k == GT_MAX; }

/* size the tracks of one axis. avail: the definite space, or -1 (then sized for max-content, or
   min-content when min is set). pct: the base for percentages, or -1. */
static void gr_size(struct gtr *tr, int nt, float gap, struct gitem *it, int n, bool col, float avail, bool min,
                    float pct) {
    for (int i = 0; i < nt; i++) {
        struct gtr *t = &tr[i];
        t->base = 0;
        t->limit = -1;
        t->fmax = 0;
        if ((t->t.min_kind == GT_LEN && len_has_pct(&t->t.min) && pct < 0)) t->t.min_kind = GT_AUTO;
        if ((t->t.max_kind == GT_LEN && len_has_pct(&t->t.max) && pct < 0)) t->t.max_kind = GT_AUTO;
        if (t->t.min_kind == GT_LEN) t->base = fmaxf_(0, len_resolve(&t->t.min, pct));
        t->flex = t->t.max_kind == GT_FR;
        if (t->t.max_kind == GT_LEN) t->limit = fmaxf_(t->base, len_resolve(&t->t.max, pct));
        if (t->collapsed) t->base = t->limit = 0;
    }
    /* contributions of items spanning one track, then of spanning items by increasing span */
    int maxspan = 1;
    for (int i = 0; i < n; i++) {
        int s0 = col ? it[i].c0 : it[i].r0, s1 = col ? it[i].c1 : it[i].r1;
        float mn = col ? it[i].mn : it[i].h, mx = col ? it[i].mx : it[i].h;
        if (s1 - s0 > maxspan) maxspan = s1 - s0;
        if (s1 - s0 != 1) continue;
        struct gtr *t = &tr[s0];
        if (t->collapsed) continue;
        if (t->t.min_kind == GT_AUTO || t->t.min_kind == GT_MIN) t->base = fmaxf_(t->base, mn);
        else if (t->t.min_kind == GT_MAX) t->base = fmaxf_(t->base, mx);
        switch (t->t.max_kind) {
        case GT_AUTO: case GT_MAX: t->limit = fmaxf_(t->limit, mx); break;
        case GT_MIN: t->limit = fmaxf_(t->limit, mn); break;
        case GT_FIT: t->limit = fmaxf_(t->limit, fminf_(mx, len_resolve(&t->t.max, pct < 0 ? 0 : pct))); break;
        case GT_FR: t->fmax = fmaxf_(t->fmax, mx); break;
        }
    }
    for (int i = 0; i < nt; i++)
        if (tr[i].limit < tr[i].base) tr[i].limit = tr[i].base;
    for (int span = 2; span <= maxspan; span++)
        for (int i = 0; i < n; i++) {
            int s0 = col ? it[i].c0 : it[i].r0, s1 = col ? it[i].c1 : it[i].r1;
            if (s1 - s0 != span) continue;
            float mn = col ? it[i].mn : it[i].h, mx = col ? it[i].mx : it[i].h;
            float sb = gap * (float)(span - 1), sl = sb;
            int nflex = 0, nint = 0, nlim = 0;
            for (int k = s0; k < s1; k++) {
                sb += tr[k].base;
                sl += tr[k].limit;
                nflex += tr[k].flex;
                nint += gr_intrinsic_kind(tr[k].t.min_kind) && !tr[k].flex;
                nlim += !tr[k].flex && tr[k].t.max_kind != GT_LEN;
            }
            if (nflex) { /* grow the flexible tracks */
                for (int k = s0; k < s1; k++)
                    if (tr[k].flex) tr[k].fmax = fmaxf_(tr[k].fmax, (mx - sl) / (float)nflex + tr[k].base);
                if (mn > sb)
                    for (int k = s0; k < s1; k++)
                        if (tr[k].flex && tr[k].t.min_kind != GT_LEN) tr[k].base += (mn - sb) / (float)nflex;
                continue;
            }
            if (mn > sb && nint)
                for (int k = s0; k < s1; k++)
                    if (gr_intrinsic_kind(tr[k].t.min_kind)) {
                        tr[k].base += (mn - sb) / (float)nint;
                        if (tr[k].limit < tr[k].base) tr[k].limit = tr[k].base;
                    }
            sl = gap * (float)(span - 1);
            for (int k = s0; k < s1; k++) sl += tr[k].limit;
            if (mx > sl && nlim)
                for (int k = s0; k < s1; k++)
                    if (!tr[k].flex && tr[k].t.max_kind != GT_LEN) tr[k].limit += (mx - sl) / (float)nlim;
        }
    int ngaps = -1;
    for (int i = 0; i < nt; i++) ngaps += !tr[i].collapsed;
    float gaps = ngaps > 0 ? gap * (float)ngaps : 0;
    if (avail < 0) {
        /* intrinsic: min-content takes the bases; max-content the limits, and an fr gets the
           largest share any of its items needs */
        float frac = 0;
        if (!min)
            for (int i = 0; i < nt; i++)
                if (tr[i].flex) frac = fmaxf_(frac, tr[i].fmax / fmaxf_(tr[i].t.max.px, 1));
        for (int i = 0; i < nt; i++) {
            struct gtr *t = &tr[i];
            if (min) t->size = t->base;
            else if (t->flex) t->size = fmaxf_(t->base, frac * t->t.max.px);
            else t->size = t->limit;
        }
        return;
    }
    /* maximize the inflexible tracks, then share what is left among the fr ones */
    float used = gaps;
    for (int i = 0; i < nt; i++) used += tr[i].base;
    float free_space = avail - used;
    for (int iter = 0; free_space > 0.01f && iter < 16; iter++) {
        int k = 0;
        for (int i = 0; i < nt; i++)
            if (!tr[i].flex && tr[i].limit > tr[i].base + 0.01f) k++;
        if (!k) break;
        float share = free_space / (float)k;
        for (int i = 0; i < nt; i++) {
            struct gtr *t = &tr[i];
            if (t->flex || t->limit <= t->base + 0.01f) continue;
            float add = fminf_(share, t->limit - t->base);
            t->base += add;
            free_space -= add;
        }
    }
    for (int i = 0; i < nt; i++) tr[i].size = tr[i].base;
    bool any_flex = false;
    for (int i = 0; i < nt; i++) any_flex |= tr[i].flex;
    if (any_flex) {
        float left = avail - gaps, sum = 0;
        for (int i = 0; i < nt; i++) {
            if (tr[i].flex) sum += tr[i].t.max.px;
            else left -= tr[i].base;
        }
        bool *fixed = calloc((size_t)nt, 1);
        float fr = 0;
        for (int iter = 0; iter < nt + 1; iter++) {
            fr = left / fmaxf_(sum, 1);
            bool again = false;
            for (int i = 0; i < nt; i++)
                if (tr[i].flex && !fixed[i] && tr[i].base > fr * tr[i].t.max.px) {
                    fixed[i] = true;
                    sum -= tr[i].t.max.px;
                    left -= tr[i].base;
                    again = true;
                }
            if (!again) break;
        }
        for (int i = 0; i < nt; i++)
            if (tr[i].flex) tr[i].size = fixed[i] || fr < 0 ? tr[i].base : fmaxf_(tr[i].base, fr * tr[i].t.max.px);
        free(fixed);
    } else if (free_space > 0.01f) {
        /* normal (stretch) content distribution: auto tracks share the rest */
        int k = 0;
        for (int i = 0; i < nt; i++) k += tr[i].t.max_kind == GT_AUTO && !tr[i].collapsed;
        for (int i = 0; i < nt && k; i++)
            if (tr[i].t.max_kind == GT_AUTO && !tr[i].collapsed) tr[i].size += free_space / (float)k;
    }
}

/* track offsets from start; returns the total extent */
static float gr_positions(struct gtr *tr, int nt, float gap, float start, float between) {
    float p = start, end = start;
    bool first = true;
    for (int i = 0; i < nt; i++) {
        if (!tr[i].collapsed) {
            if (!first) p += gap + between;
            first = false;
        }
        tr[i].pos = p;
        p += tr[i].size;
        end = p;
    }
    return end - start;
}

static void gr_contributions(struct grid *g) {
    for (int i = 0; i < g->n; i++) {
        box_t *c = g->it[i].b;
        outer_intrinsic(c, &g->it[i].mn, &g->it[i].mx);
        /* scroll containers have no content-based minimum */
        if (c->st->overflow != OV_VISIBLE && !(c->st->width.kind == LK_LEN && !len_has_pct(&c->st->width)) &&
            spec_w(c, &c->st->min_width, -1) < 0)
            g->it[i].mn = hext(c) + c->m[1] + c->m[3];
    }
}

static void grid_intrinsic(box_t *b, float *mn, float *mx) {
    struct grid g;
    gr_setup(&g, b, -1);
    gr_contributions(&g);
    float gap = b->st->gap_col;
    gr_size(g.col, g.nc, gap, g.it, g.n, true, -1, true, -1);
    *mn = gr_positions(g.col, g.nc, gap, 0, 0);
    gr_size(g.col, g.nc, gap, g.it, g.n, true, -1, false, -1);
    *mx = gr_positions(g.col, g.nc, gap, 0, 0);
    gr_free(&g);
}

static void layout_grid(box_t *b, float cbh) {
    const style_t *st = b->st;
    float cw = b->w;
    float defh = spec_h(b, &st->height, cbh);
    for (box_t *c = b->first; c; c = c->next) {
        c->cb = b;
        if (c->abspos) {
            c->static_cb = b;
            c->static_x = 0;
            c->static_y = 0;
            list_abs(c);
        }
    }
    struct grid g;
    gr_setup(&g, b, cw);
    gr_contributions(&g);
    /* columns */
    gr_size(g.col, g.nc, st->gap_col, g.it, g.n, true, cw, false, cw);
    float total = 0, start = 0, between = 0;
    total = gr_positions(g.col, g.nc, st->gap_col, 0, 0);
    float left = cw - total;
    int nvis = 0;
    for (int i = 0; i < g.nc; i++) nvis += !g.col[i].collapsed;
    if (left > 0 && nvis) {
        switch (st->justify_content) {
        case JC_END: start = left; break;
        case JC_CENTER: start = left / 2; break;
        case JC_BETWEEN: if (nvis > 1) between = left / (float)(nvis - 1); break;
        case JC_AROUND: start = left / (float)nvis / 2; between = left / (float)nvis; break;
        case JC_EVENLY: start = between = left / (float)(nvis + 1); break;
        }
    }
    gr_positions(g.col, g.nc, st->gap_col, start, between);
    /* lay the items out in their columns */
    for (int i = 0; i < g.n; i++) {
        struct gitem *t = &g.it[i];
        box_t *c = t->b;
        float aw = g.col[t->c1 - 1].pos + g.col[t->c1 - 1].size - g.col[t->c0].pos;
        resolve_edges(c, aw);
        float ext = hext(c) + c->m[1] + c->m[3];
        int js = c->st->justify_self != 255 ? c->st->justify_self : st->justify_items;
        if (c->kind == B_ATOMIC && c->atomic != AT_INLINE_BLOCK) size_atomic(c, aw, -1);
        else {
            if (c->kind == B_TABLE) table_width(c, aw);
            else {
                float w = spec_w(c, &c->st->width, aw);
                bool am = len_auto(&c->st->margin[1]) || len_auto(&c->st->margin[3]);
                if (w < 0) w = js == AI_STRETCH && !am ? aw - ext : stf_width(c, aw);
                c->w = clamp_w(c, w, aw);
            }
            layout_inner(c, NULL, 0, 0, -1);
        }
        t->h = c->h + vext(c) + c->m[0] + c->m[2];
    }
    /* rows */
    gr_size(g.row, g.nr, st->gap_row, g.it, g.n, false, defh, false, defh);
    float rows_h = gr_positions(g.row, g.nr, st->gap_row, 0, 0);
    /* place and align the items in their areas */
    float first_bl = -1;
    int first_row = (1 << 30);
    for (int i = 0; i < g.n; i++) {
        struct gitem *t = &g.it[i];
        box_t *c = t->b;
        float ax = g.col[t->c0].pos, aw = g.col[t->c1 - 1].pos + g.col[t->c1 - 1].size - ax;
        float ay = g.row[t->r0].pos, ah = g.row[t->r1 - 1].pos + g.row[t->r1 - 1].size - ay;
        bool replaced = c->kind == B_ATOMIC && c->atomic != AT_INLINE_BLOCK;
        if (!replaced && len_has_pct(&c->st->height)) { /* percentages of the area's height */
            layout_inner(c, NULL, 0, 0, ah);
            t->h = c->h + vext(c) + c->m[0] + c->m[2];
        }
        int js = c->st->justify_self != 255 ? c->st->justify_self : st->justify_items;
        int as = c->st->align_self != 255 ? c->st->align_self : st->align_items;
        float ow = c->w + hext(c) + c->m[1] + c->m[3], xoff = 0;
        if (len_auto(&c->st->margin[3]) && len_auto(&c->st->margin[1])) xoff = (aw - ow) / 2;
        else if (len_auto(&c->st->margin[3])) xoff = aw - ow;
        else if (js == AI_CENTER) xoff = (aw - ow) / 2;
        else if (js == AI_END) xoff = aw - ow;
        float yoff = 0;
        bool vam = len_auto(&c->st->margin[0]) || len_auto(&c->st->margin[2]);
        if (len_auto(&c->st->margin[0]) && len_auto(&c->st->margin[2])) yoff = (ah - t->h) / 2;
        else if (len_auto(&c->st->margin[0])) yoff = ah - t->h;
        else if (as == AI_CENTER) yoff = (ah - t->h) / 2;
        else if (as == AI_END) yoff = ah - t->h;
        else if (as == AI_STRETCH && !replaced && !vam && len_auto(&c->st->height)) {
            float want = clamp_h(c, ah - vext(c) - c->m[0] - c->m[2], ah);
            if (want > c->h) c->h = want;
        }
        c->x = ax + xoff + c->m[3] + c->b[3] + c->p[3];
        c->y = ay + yoff + c->m[0] + c->b[0] + c->p[0];
        if (c->baseline >= 0 && t->r0 < first_row) {
            first_row = t->r0;
            first_bl = c->y + c->baseline;
        }
    }
    b->h = defh >= 0 ? defh : rows_h;
    b->baseline = b->last_baseline = first_bl;
    gr_free(&g);
}

/* ---------------------------------------------------------------- intrinsic sizes */
static void inline_intrinsic(box_t *b, float *mn, float *mx) {
    struct ivec iv = {0};
    struct ibuild bs = {&iv, true, NULL, 0};
    build_items(b, &bs);
    float line = 0, chunk = 0;
    *mn = *mx = 0;
    for (int i = 0; i < iv.n; i++) {
        struct item *it = &iv.v[i];
        switch (it->kind) {
        case IT_TEXT:
            chunk += it->w;
            line += it->w;
            if (it->brk || i == iv.n - 1) {
                *mn = fmaxf_(*mn, chunk - (it->space_end ? it->sw : 0));
                chunk = 0;
            }
            break;
        case IT_OPEN: case IT_CLOSE: {
            float w = inline_edge(it->box, it->kind == IT_OPEN, 0);
            chunk += w;
            line += w;
            break;
        }
        case IT_ATOMIC: {
            float a, z;
            outer_intrinsic(it->box, &a, &z);
            *mn = fmaxf_(*mn, chunk);
            *mn = fmaxf_(*mn, a);
            chunk = 0;
            line += z;
            break;
        }
        case IT_FLOAT: {
            float a, z;
            outer_intrinsic(it->box, &a, &z);
            *mn = fmaxf_(*mn, a);
            line += z;
            break;
        }
        case IT_BR:
            *mx = fmaxf_(*mx, line);
            *mn = fmaxf_(*mn, chunk);
            line = chunk = 0;
            break;
        case IT_WBR:
            if (it->brk) {
                *mn = fmaxf_(*mn, chunk);
                chunk = 0;
            }
            break;
        }
    }
    *mn = fmaxf_(*mn, chunk);
    *mx = fmaxf_(*mx, line);
    float ind = len_resolve(&b->st->text_indent, 0);
    if (ind > 0) {
        *mx += ind;
        *mn += ind;
    }
    free(iv.v);
}

static void intrinsic(box_t *b, float *mn, float *mx) {
    if (b->intrinsic_done) {
        *mn = b->min_cw;
        *mx = b->max_cw;
        return;
    }
    float a = 0, z = 0;
    if (b->kind == B_ATOMIC && b->atomic != AT_INLINE_BLOCK) {
        resolve_edges(b, 0);
        size_atomic(b, -1, -1);
        a = z = b->w;
        if (len_has_pct(&b->st->width) || len_has_pct(&b->st->max_width)) a = 0;
    } else if (b->kind == B_TABLE) {
        table_intrinsic(b, &a, &z);
        a -= 0;
    } else if (b->kind == B_FLEX) {
        bool column = b->st->flex_direction == FD_COLUMN || b->st->flex_direction == FD_COLUMN_REVERSE;
        int n = 0;
        for (box_t *c = b->first; c; c = c->next) {
            if (c->abspos) continue;
            float cm, cx;
            outer_intrinsic(c, &cm, &cx);
            if (column) {
                a = fmaxf_(a, cm);
                z = fmaxf_(z, cx);
            } else {
                a = b->st->flex_wrap ? fmaxf_(a, cm) : a + cm;
                z += cx;
                if (n) z += b->st->gap_col;
            }
            n++;
        }
    } else if (b->kind == B_GRID) {
        grid_intrinsic(b, &a, &z);
    } else if (b->inline_ctx) {
        inline_intrinsic(b, &a, &z);
    } else {
        float fl = 0;
        for (box_t *c = b->first; c; c = c->next) {
            if (c->abspos) continue;
            float cm, cx;
            outer_intrinsic(c, &cm, &cx);
            a = fmaxf_(a, cm);
            if (c->floated) {
                fl += cx;
                z = fmaxf_(z, fl);
            } else {
                fl = 0;
                z = fmaxf_(z, cx);
            }
        }
    }
    if (z < a) z = a;
    b->min_cw = a;
    b->max_cw = z;
    b->intrinsic_done = true;
    *mn = a;
    *mx = z;
}

/* margin-box contributions */
static void outer_intrinsic(box_t *c, float *mn, float *mx) {
    resolve_edges(c, 0);
    float ext = hext(c) + c->m[1] + c->m[3];
    const len_t *wl = &c->st->width;
    if (c->kind != B_ATOMIC && wl->kind == LK_LEN && wl->pct == 0) {
        float w = wl->px - (c->st->box_sizing ? hext(c) : 0);
        w = clamp_w(c, w < 0 ? 0 : w, -1);
        *mn = *mx = w + ext;
        return;
    }
    intrinsic(c, mn, mx);
    float lo = spec_w(c, &c->st->min_width, -1), hi = spec_w(c, &c->st->max_width, -1);
    if (hi >= 0) {
        if (*mx > hi) *mx = hi;
        if (*mn > hi) *mn = hi;
    }
    if (lo >= 0) {
        if (*mn < lo) *mn = lo;
        if (*mx < lo) *mx = lo;
    }
    *mn += ext;
    *mx += ext;
}

/* ---------------------------------------------------------------- dispatch */
/* lay out the inside of b, whose width (b->w) and position are already known */
static void layout_inner(box_t *b, struct bfc *f, float ox, float oy, float cbh) {
    struct bfc own = {0};
    bool root = is_bfc_root(b);
    if (root) {
        f = &own;
        ox = oy = 0;
    }
    float sh = spec_h(b, &b->st->height, cbh);
    /* The anonymous initial containing block supplies the viewport height,
       even though its own auto height follows document content. Otherwise
       html/body height:100% lose their definite reference at the first box.
       A definite height's min/max constraints also apply to descendants;
       auto heights (including min-height-only boxes) remain indefinite. */
    float child_cbh = b == D->root_box ? VH : sh >= 0 ? clamp_h(b, sh, cbh) : -1;
    b->baseline = b->last_baseline = -1;
    b->nruns = b->ndecos = 0;
    switch (b->kind) {
    case B_TABLE: layout_table(b, cbh); break;
    case B_FLEX: layout_flex(b, cbh); break;
    case B_GRID: layout_grid(b, cbh); break;
    default:
        if (b->inline_ctx) layout_inline(b, f, ox, oy, child_cbh);
        else layout_blocks(b, f, ox, oy, child_cbh);
    }
    if (root && f->n) { /* a new formatting context contains its floats */
        float fb = bfc_bottom(f);
        if (fb > b->h) b->h = fb;
    }
    if (b->kind != B_TABLE) {
        if (sh >= 0) b->h = sh;
        b->h = clamp_h(b, b->h, cbh);
    }
    if (root) bfc_free(&own);
}

/* relative positioning offsets, now that containing blocks have their sizes */
static void relative_offsets(box_t *b) {
    for (box_t *c = b->first; c; c = c->next) {
        c->rel_dx = c->rel_dy = 0;
        if (c->st && c->st->position == POS_RELATIVE && c->kind != B_INLINE && c->kind != B_TEXT) {
            const len_t *in = c->st->inset;
            float cw = c->cb ? c->cb->w : VW, ch = c->cb ? c->cb->h : VH;
            if (!len_auto(&in[3])) c->rel_dx = len_resolve(&in[3], cw);
            else if (!len_auto(&in[1])) c->rel_dx = -len_resolve(&in[1], cw);
            if (!len_auto(&in[0])) c->rel_dy = len_has_pct(&in[0]) && !c->cb ? 0 : len_resolve(&in[0], ch);
            else if (!len_auto(&in[2])) c->rel_dy = -len_resolve(&in[2], ch);
        }
        relative_offsets(c);
    }
}

/* ---------------------------------------------------------------- absolute positioning */
static box_t *abs_containing_block(box_t *a) {
    if (a->st->position == POS_FIXED || web_dialog_layer_box(D,a)) return D->root_box;
    for (box_t *p = a->parent; p; p = p->parent)
        if (p->st && p->st->position != POS_STATIC && p->kind != B_INLINE && p->kind != B_TEXT) return p;
    return D->root_box;
}

static void layout_abs(box_t *a) {
    box_t *cb = abs_containing_block(a);
    bool icb = cb == D->root_box;
    float pl = icb ? 0 : cb->p[3], pt = icb ? 0 : cb->p[0];
    float cbw = icb ? VW : cb->w + cb->p[1] + cb->p[3];
    float cbh = icb ? VH : cb->h + cb->p[0] + cb->p[2];
    resolve_edges(a, cbw);
    const len_t *in = a->st->inset;
    bool la = len_auto(&in[3]), ra = len_auto(&in[1]), ta = len_auto(&in[0]), ba = len_auto(&in[2]);
    float l = la ? 0 : len_resolve(&in[3], cbw), r = ra ? 0 : len_resolve(&in[1], cbw);
    float t = ta ? 0 : len_resolve(&in[0], cbh), bo = ba ? 0 : len_resolve(&in[2], cbh);
    float ext = hext(a);
    a->cb = cb;
    if (a->kind == B_ATOMIC && a->atomic != AT_INLINE_BLOCK) size_atomic(a, cbw, cbh);
    else {
        float w = spec_w(a, &a->st->width, cbw);
        if (w < 0) {
            if (!la && !ra && !(a->node && a->node->box==a && web_dialog_is_modal(D,a->node))) w = cbw - l - r - ext - a->m[1] - a->m[3];
            else w = stf_width(a, cbw - l - r);
        }
        if (a->kind == B_TABLE) {
            table_width(a, cbw - l - r);
            w = fmaxf_(w, a->w);
        }
        a->w = clamp_w(a, w, cbw);
        float sh = spec_h(a, &a->st->height, cbh);
        if (sh < 0 && !ta && !ba && !(a->node && a->node->box==a && web_dialog_is_modal(D,a->node))) sh = clamp_h(a, cbh - t - bo - vext(a) - a->m[0] - a->m[2], cbh);
        layout_inner(a, NULL, 0, 0, cbh);
        if (sh >= 0 && a->kind != B_TABLE) a->h = clamp_h(a, sh, cbh);
    }
    /* the static position, in the containing block's coordinates */
    float sx = 0, sy = 0;
    if (a->static_cb) {
        sx = box_abs_x(a->static_cb) + a->static_x - (icb ? 0 : box_abs_x(cb));
        sy = box_abs_y(a->static_cb) + a->static_y - (icb ? 0 : box_abs_y(cb) + cb->content_dy);
    }
    float mw = a->w + ext + a->m[1] + a->m[3];
    float x;
    if (!la) x = -pl + l + a->m[3];
    else if (!ra) x = -pl + cbw - r - mw + a->m[3];
    else x = sx + a->m[3];
    if (!la && !ra && len_auto(&a->st->margin[3]) && len_auto(&a->st->margin[1]))
        x = -pl + l + (cbw - l - r - (a->w + ext)) / 2;
    float mh = a->h + vext(a) + a->m[0] + a->m[2];
    float y;
    if (!ta) y = -pt + t + a->m[0];
    else if (!ba) y = -pt + cbh - bo - mh + a->m[0];
    else y = sy + a->m[0];
    if (!ta && !ba && len_auto(&a->st->margin[0]) && len_auto(&a->st->margin[2]))
        y = -pt + t + (cbh - t - bo - (a->h + vext(a))) / 2;
    a->x = x + a->b[3] + a->p[3];
    a->y = y + a->b[0] + a->p[0];
}

/* ---------------------------------------------------------------- entry */
static float max_bottom(box_t *b, float base) {
    float y = base + b->y + b->rel_dy;
    float bottom = y + b->h + b->p[2] + b->b[2];
    if (b->st && b->st->overflow != OV_VISIBLE && b->kind != B_TEXT) return bottom;
    for (box_t *c = b->first; c; c = c->next) {
        if (c->abspos || c->kind == B_TEXT || c->kind == B_INLINE || c->kind == B_BR) continue;
        if (c->cb != b) continue;
        bottom = fmaxf_(bottom, max_bottom(c, y + b->content_dy));
    }
    return bottom;
}

void layout_doc(web_doc *d, int width, int height) {
    D = d;
    VW = (float)width;
    VH = (float)height;
    GEN++;
    if (!GEN) GEN = 1;
    ar_free(&d->lmem);
    d->abs_boxes.n = 0;
    box_t *root = d->root_box;
    if (!root) return;
    root->x = root->y = 0;
    root->w = VW;
    root->cb = NULL;
    layout_inner(root, NULL, 0, 0, VH);
    for (int i = 0; i < d->abs_boxes.n; i++) layout_abs(d->abs_boxes.v[i]);
    relative_offsets(root);
    float h = fmaxf_(root->h, max_bottom(root, 0));
    for (int i = 0; i < d->abs_boxes.n; i++) {
        box_t *a = d->abs_boxes.v[i];
        if (a->st->position == POS_FIXED) continue;
        float ay = box_abs_y(a);
        h = fmaxf_(h, ay + a->h + a->p[2] + a->b[2]);
    }
    d->doc_h = h != h || h < 0 ? 0 : h > 1e8f ? 100000000 : (int)ceilf(h);
    d->doc_w = width;
}
