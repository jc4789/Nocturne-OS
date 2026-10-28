/* Layout: block formatting (margin collapsing, floats, clearance), inline formatting (line
   breaking, alignment, vertical-align, inline backgrounds), tables (auto layout with colspan and
   rowspan), flexbox (single and multi-line, grow/shrink, justify/align), replaced elements and
   form controls, and absolute/fixed/relative positioning.

   Every box's x, y is its content box's position relative to the content box of b->cb (the box
   it was laid out in); w, h are its content size. */
#include <stdio.h>
#include <math.h>
#include <limits.h>
#include <float.h>
#include <nocturne.h>
#include <parallel.h>
#include "webi.h"
#include "web_dialog.h"
#include "form_value.h"
#include "js_canvas.h"
#include "avmedia.h"
#include "svg_geometry.h"

struct layout_text_cache;
struct layout_context {
    web_doc *document;
    float viewport_w, viewport_h;
    uint32_t generation;
    struct layout_text_cache *text_cache;
    bool pure;
    unsigned clamp_depth; /* copied by value into independent layout helpers */
    arena_t scratch; /* helper-owned allocation trap, never the caller stack */
};
static struct layout_context default_layout;
static struct layout_context *layout_current(void) {
    struct layout_context *c = thread_local_get(1);
    return c ? c : &default_layout;
}
#define D (layout_current()->document)
#define VW (layout_current()->viewport_w)
#define VH (layout_current()->viewport_h)
#define GEN (layout_current()->generation)
static arena_t *layout_scratch(void) {
    struct layout_context *c = layout_current();
    return c->pure ? &c->scratch : &D->lmem;
}
static bool layout_checkpoint(void) {
    return layout_current()->pure && parallel_worker_index() ?
        !__atomic_load_n(&D->native_cancelled, __ATOMIC_ACQUIRE) : web_native_checkpoint(D);
}
static void layout_media_checkpoint(void) {
    if (!layout_current()->pure) web_avmedia_checkpoint();
}

#define INF 1e30f

/* ---------------------------------------------------------------- fonts */
wfont style_font(const style_t *st) {
    wfont f = {.px=st->font_size,.bold=st->font_weight>=600,.choices=st->font_choices};
    int idx = (f.bold ? FONT_BOLD : 0) | (st->font_style ? FONT_ITALIC : 0);
    f.ttf = st->named_font ? st->named_font : font_family(st->font_family, idx);
    if (!f.ttf) f.ttf = font_ui(FONT_REGULAR);
    return f;
}

static int mono_font(const wfont *f) { return f->px >= 24 ? FONT_LARGE : FONT_SMALL; }
static font_t *wf_face(const wfont *f,uint32_t cp) {
    for(const struct web_font_choice *choice=f->choices;choice;choice=choice->next){
        if(choice->resource->failed)continue;
        size_t lo=0,hi=choice->range_count;
        while(lo<hi){size_t mid=lo+(hi-lo)/2;if(cp>choice->ranges[mid].last)lo=mid+1;else hi=mid;}
        if(choice->range_count&&(lo==choice->range_count||cp<choice->ranges[lo].first))continue;
        /* Demand only a subset actually used by a text run. The resource and
           ranges are already owned; worker glyph selection never allocates or
           submits network work, and snapshot probes stay read-only. */
        if(!web_avmedia_snapshot_is_probe())__atomic_store_n(&choice->resource->wanted,true,__ATOMIC_RELAXED);
        font_t *face=choice->resource->face;
        if(face&&font_has_primary(face,cp))return face;
    }
    return f->ttf;
}

/* ---------------------------------------------------------------- grapheme boundaries */
#include "unicode_grapheme_data.h"

static uint8_t wf_grapheme_properties(uint32_t cp) {
    /* ASCII の通常textは UCD の探索や allocation を必要としない。 */
    if (cp < 0x80) {
        if (cp == '\r') return WF_GCB_CR;
        if (cp == '\n') return WF_GCB_LF;
        return cp < 0x20 || cp == 0x7F ? WF_GCB_CONTROL : WF_GCB_OTHER;
    }
    if (cp >= 0xAC00 && cp <= 0xD7A3)
        return (cp - 0xAC00) % 28 ? WF_GCB_LVT : WF_GCB_LV;
    size_t lo = 0, hi = sizeof(wf_grapheme_ranges) / sizeof(*wf_grapheme_ranges);
    while (lo < hi) {
        size_t mid = lo + (hi - lo) / 2;
        if (cp > wf_grapheme_ranges[mid].last) lo = mid + 1;
        else hi = mid;
    }
    return lo < sizeof(wf_grapheme_ranges) / sizeof(*wf_grapheme_ranges) &&
        cp >= wf_grapheme_ranges[lo].first ? wf_grapheme_ranges[lo].properties : WF_GCB_OTHER;
}

size_t wf_grapheme_decode(const char *s, size_t n, uint32_t *cp) {
    if (!n) return 0;
    const uint8_t *p = (const uint8_t *)s;
    if (p[0] < 0x80) { *cp = p[0]; return 1; }
    unsigned length;
    uint32_t value, minimum;
    if (p[0] >= 0xC2 && p[0] <= 0xDF) { length = 2; value = p[0] & 31; minimum = 0x80; }
    else if (p[0] >= 0xE0 && p[0] <= 0xEF) { length = 3; value = p[0] & 15; minimum = 0x800; }
    else if (p[0] >= 0xF0 && p[0] <= 0xF4) { length = 4; value = p[0] & 7; minimum = 0x10000; }
    else goto malformed;
    if (length > n) goto malformed;
    for (unsigned i = 1; i < length; ++i) {
        if ((p[i] & 0xC0) != 0x80) goto malformed;
        value = (value << 6) | (p[i] & 63);
    }
    if (value < minimum || value > 0x10FFFF || (value >= 0xD800 && value <= 0xDFFF)) goto malformed;
    *cp = value;
    return length;
malformed:
    *cp = 0xFFFD;
    return 1;
}

bool wf_grapheme_break(wf_grapheme_state *state, uint32_t cp) {
    uint8_t properties = wf_grapheme_properties(cp);
    uint8_t current = properties & 15, previous = state->previous;
    uint8_t incb = properties & 48;
    bool boundary = true;
    /* UAX #29 revision 49、extended GB1..GB999。規則の順序を保つ。 */
    if (!state->have_previous) boundary = true;                                       /* GB1 */
    else if (previous == WF_GCB_CR && current == WF_GCB_LF) boundary = false;           /* GB3 */
    else if (previous == WF_GCB_CR || previous == WF_GCB_LF || previous == WF_GCB_CONTROL ||
             current == WF_GCB_CR || current == WF_GCB_LF || current == WF_GCB_CONTROL) boundary = true; /* GB4/5 */
    else if (previous == WF_GCB_L && (current == WF_GCB_L || current == WF_GCB_V ||
             current == WF_GCB_LV || current == WF_GCB_LVT)) boundary = false;          /* GB6 */
    else if ((previous == WF_GCB_LV || previous == WF_GCB_V) &&
             (current == WF_GCB_V || current == WF_GCB_T)) boundary = false;            /* GB7 */
    else if ((previous == WF_GCB_LVT || previous == WF_GCB_T) && current == WF_GCB_T) boundary = false; /* GB8 */
    else if (current == WF_GCB_EXTEND || current == WF_GCB_ZWJ || current == WF_GCB_SPACINGMARK) boundary = false; /* GB9/9a */
    else if (previous == WF_GCB_PREPEND) boundary = false;                              /* GB9b */
    else if (state->indic_linker && incb == WF_INCB_CONSONANT) boundary = false;         /* GB9c (18.0) */
    else if (state->pictograph_zwj && (properties & WF_EXTENDED_PICTOGRAPHIC)) boundary = false; /* GB11 */
    else if (previous == WF_GCB_REGIONAL_INDICATOR && current == WF_GCB_REGIONAL_INDICATOR &&
             state->ri_odd) boundary = false;                                         /* GB12/13 */

    state->pictograph_zwj = current == WF_GCB_ZWJ && state->pictograph_extend;
    state->pictograph_extend = (properties & WF_EXTENDED_PICTOGRAPHIC) ||
        (current == WF_GCB_EXTEND && state->pictograph_extend);
    state->indic_linker = incb == WF_INCB_LINKER ||
        (incb == WF_INCB_EXTEND && state->indic_linker);
    state->ri_odd = current == WF_GCB_REGIONAL_INDICATOR &&
        (previous != WF_GCB_REGIONAL_INDICATOR || !state->ri_odd);
    state->previous = current;
    state->have_previous = true;
    return boundary;
}

size_t wf_grapheme_next(const char *s, size_t n, size_t at) {
    if (at >= n) return n;
    uint8_t first = (uint8_t)s[at];
    if (first < 0x80) {
        if (first == '\r' && n - at > 1 && s[at + 1] == '\n') return at + 2;
        if (first < 0x20 || first == 0x7F || n - at == 1 || (uint8_t)s[at + 1] < 0x80)
            return at + 1;
    }
    wf_grapheme_state state = {0};
    uint32_t cp;
    size_t length = wf_grapheme_decode(s + at, n - at, &cp);
    wf_grapheme_break(&state, cp);
    for (size_t next = at + length; next < n;) {
        length = wf_grapheme_decode(s + next, n - next, &cp);
        if (wf_grapheme_break(&state, cp)) return next;
        next += length;
    }
    return n;
}

/* A following variation selector selects the presentation of its base, so it
   travels with the base to the chosen face. An emoji grapheme (ZWJ, modifier,
   flag, tag or keycap sequence) goes whole, for the face to form its ligature. */
static size_t wf_cluster(const char *s, size_t n, size_t at, int bytes) {
    size_t end = at + (size_t)bytes;
    uint32_t cp;
    wf_grapheme_decode(s + at, n - at, &cp);
    uint8_t properties = wf_grapheme_properties(cp);
    if ((properties & WF_EXTENDED_PICTOGRAPHIC) || (properties & 15) == WF_GCB_REGIONAL_INDICATOR ||
        (end < n && (cp == '#' || cp == '*' || (cp >= '0' && cp <= '9'))))
        return wf_grapheme_next(s, n, at) - at;
    if (end < n) {
        uint32_t next;
        int more = gfx_utf8_decode(s + end, &next);
        if ((size_t)more <= n - end && ((next >= 0xfe00 && next <= 0xfe0f) || (next >= 0xe0100 && next <= 0xe01ef)))
            end += (size_t)more;
    }
    return end - at;
}

float wf_width(const wfont *f, const char *s, size_t n) {
    if (f->px < 1) return 0;
    if (f->ttf&&!f->choices) return font_width(f->ttf, f->px, s, n);
    if(f->choices){
        float width=0;
        for(size_t i=0;i<n;){uint32_t cp;int bytes=gfx_utf8_decode(s+i,&cp);if((size_t)bytes>n-i)break;
            size_t span=wf_cluster(s,n,i,bytes);
            font_t *face=wf_face(f,cp);width+=face?font_width(face,f->px,s+i,span):gfx_codepoint_width(cp,mono_font(f));
            i+=span;}
        return width;
    }
    int width = 0;
    for (size_t i = 0; i < n;) {
        uint32_t cp;
        int bytes = gfx_utf8_decode(s + i, &cp);
        if ((size_t)bytes > n - i) break;
        width += gfx_codepoint_width(cp, mono_font(f));
        i += (size_t)bytes;
    }
    return (float)width;
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
    if (f->ttf&&!f->choices) return font_draw(c, f->ttf, f->px, x, baseline, s, n, color);
    if(f->choices){
        for(size_t i=0;i<n;){uint32_t cp;int bytes=gfx_utf8_decode(s+i,&cp);if((size_t)bytes>n-i)break;
            size_t span=wf_cluster(s,n,i,bytes);
            font_t *face=wf_face(f,cp);
            if(face)x=font_draw(c,face,f->px,x,baseline,s+i,span,color);
            else {int top=baseline-gfx_font_h(mono_font(f))*3/4;x+=gfx_char(c,(int)x,top,cp,color,0,mono_font(f));}
            i+=span;
        }
        return x;
    }
    int font = mono_font(f);
    int fh = gfx_font_h(font);
    int top = baseline - (int)(fh * 0.75f);
    for (size_t i = 0; i < n;) {
        uint32_t cp;
        int bytes = gfx_utf8_decode(s + i, &cp);
        if ((size_t)bytes > n - i) break;
        i += (size_t)bytes;
        int advance = gfx_char(c, (int)x, top, cp, color, 0, font);
        if (f->bold) gfx_char(c, (int)x + 1, top, cp, color, 0, font);
        x += (float)advance;
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
static float text_width_uncached(const style_t *st, const wfont *f, const char *s, size_t n) {
    float w = wf_width(f, s, n);
    if (st->letter_spacing != 0 || st->word_spacing != 0) {
        for (size_t i = 0; i < n; i++) {
            if (((unsigned char)s[i] & 0xC0) != 0x80) w += st->letter_spacing;
            if (s[i] == ' ') w += st->word_spacing;
        }
    }
    /* Negative spacing may close a run up but never gives it a negative advance:
       a collapsed space between inline-blocks under letter-spacing: -0.4em is 0 wide. */
    return w < 0 ? 0 : w;
}

float style_text_width(const style_t *st, const char *s, size_t n) {
    wfont f = style_font(st);
    return text_width_uncached(st, &f, s, n);
}

#include "layout_text_cache.h"

void layout_cache_free(web_doc *d) {
    if (!d) return;
    layout_text_cache_free(d->layout_text_cache);
    d->layout_text_cache = NULL;
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

/* A non-replaced absolute/fixed box with auto height and both vertical
   insets specified has a definite content height before its descendants are
   laid out. Applying that height only after layout makes height:100% children
   incorrectly see an indefinite containing block and collapse to zero. */
static float resolved_content_height(const box_t *b,float cbh) {
    float h=spec_h(b,&b->st->height,cbh);
    if(h>=0 || !len_auto(&b->st->height) || !b->abspos || cbh<0 ||
       (b->kind==B_ATOMIC && b->atomic!=AT_INLINE_BLOCK) ||
       len_auto(&b->st->inset[0]) || len_auto(&b->st->inset[2]) ||
       (b->node && b->node->box==b && web_dialog_is_modal(D,b->node)))return h;
    h=cbh-len_resolve(&b->st->inset[0],cbh)-len_resolve(&b->st->inset[2],cbh)-
      vext(b)-b->m[0]-b->m[2];
    return clamp_h(b,h,cbh);
}

static float mcollapse(float a, float b) {
    if (a >= 0 && b >= 0) return a > b ? a : b;
    if (a < 0 && b < 0) return a < b ? a : b;
    return a + b;
}

static bool is_flex_item(const box_t *b) {
    return b->parent && (b->parent->kind == B_FLEX || b->parent->kind == B_GRID) && !b->abspos;
}

static bool size_container(const box_t *b) {
    return !b->anon && b->st->container_type!=CT_NORMAL &&
        b->st->display!=D_INLINE && b->st->display!=D_INLINE_TABLE &&
        (b->kind==B_BLOCK || b->kind==B_FLEX || b->kind==B_GRID || b->kind==B_ATOMIC);
}

static bool is_bfc_root(const box_t *b) {
    return !b->parent || b->floated || b->abspos || b->kind == B_ATOMIC || b->kind == B_CELL ||
           b->kind == B_CAPTION || b->kind == B_TABLE || b->kind == B_FLEX || b->kind == B_GRID ||
           (b->st->overflow != OV_VISIBLE && !doc_viewport_overflow_box(D,b)) ||
           b->st->display == D_FLOW_ROOT || (!b->anon && b->st->legacy_box) ||
           size_container(b) || is_flex_item(b) || b->is_bfc;
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

bool box_element_scrollable(const box_t *b) {
    return b && b->node && b->node->type == N_ELEM && b->node->box == b &&
           !b->anon && b->kind != B_INLINE && b->kind != B_TEXT && b->st &&
           (b->st->overflow == OV_HIDDEN || b->st->overflow == OV_AUTO || b->st->overflow == OV_SCROLL) &&
           b->node->owner && !doc_viewport_overflow_box(b->node->owner, b);
}

static double scroll_limit(double value, double maximum) {
    if (!isfinite(value) || !isfinite(maximum) || value < 0 || maximum <= 0) return 0;
    return value > maximum ? maximum : value;
}

void box_scroll_clamp(box_t *b) {
    if (!b || !b->node || b->node->box != b || b->anon) return;
    node_t *n = b->node;
    bool scrollable = box_element_scrollable(b);
    double mx = scrollable ? (double)b->scroll_w - b->w - b->p[1] - b->p[3] : 0;
    double my = scrollable ? (double)b->scroll_h - b->h - b->p[0] - b->p[2] : 0;
    n->scroll_x = scroll_limit(n->scroll_x, mx);
    n->scroll_y = scroll_limit(n->scroll_y, my);
}

/* the transform properties, or an offset transform (motion-1 §2.7) */
static bool transform_properties(const style_t *s){return s->transform||s->translate.active||s->rotate||s->scale;}
static bool offset_path(const style_t *s){return s->offset&&s->offset->path;}
bool css_translation_active(const style_t *s){return s&&(transform_properties(s)||offset_path(s));}
/* Row-major 4x4 of one function; translation percentages use w/h. depth: perspective too,
   which orthographic flattening otherwise discards. */
static void transform_function_matrix(const struct css_transform *t,float w,float h,bool depth,double m[16]){
    for(int i=0;i<16;i++)m[i]=i%5?0:1;
    switch(t->kind){
    case CT_TRANSLATE:m[3]=len_resolve(&t->xy[0],w);m[7]=len_resolve(&t->xy[1],h);m[11]=t->v[0];break;
    case CT_SCALE:m[0]=t->v[0];m[5]=t->v[1];m[10]=t->v[2];break;
    case CT_SKEW:m[1]=tan(t->v[0]);m[4]=tan(t->v[1]);break;
    case CT_MATRIX:for(int i=0;i<16;i++)m[i]=t->m[i];break;
    case CT_ROTATE:{
        double x=t->v[0],y=t->v[1],z=t->v[2],length=sqrt(x*x+y*y+z*z),a=t->v[3];
        if(!(length>0)||!isfinite(length))break;
        x/=length;y/=length;z/=length;
        double s=sin(a),c=cos(a),k=1-c;
        m[0]=c+x*x*k;   m[1]=x*y*k-z*s; m[2]=x*z*k+y*s;
        m[4]=y*x*k+z*s; m[5]=c+y*y*k;   m[6]=y*z*k-x*s;
        m[8]=z*x*k-y*s; m[9]=z*y*k+x*s; m[10]=c+z*z*k;
        break;
    }
    case CT_PERSPECTIVE:
        /* CSS Transforms 2: perspective(none) is the identity; lengths under 1px act as 1px. */
        if(depth&&t->v[1]!=1)m[14]=-1.0/(t->v[0]<1?1:t->v[0]);
        break;
    default:break;
    }
}
static void matrix4_multiply(double out[16],const double a[16],const double b[16]){
    double r[16];
    for(int i=0;i<4;i++)for(int j=0;j<4;j++)
        r[i*4+j]=a[i*4]*b[j]+a[i*4+1]*b[4+j]+a[i*4+2]*b[8+j]+a[i*4+3]*b[12+j];
    memcpy(out,r,sizeof r);
}
void css_transform_list_matrix(const struct css_transform *t,float w,float h,double m[16]){
    for(int i=0;i<16;i++)m[i]=i%5?0:1;
    for(;t;t=t->next){double f[16];transform_function_matrix(t,w,h,false,f);matrix4_multiply(m,m,f);}
}
void css_transform_list_matrix_3d(const struct css_transform *t,double m[16]){
    for(int i=0;i<16;i++)m[i]=i%5?0:1;
    for(;t;t=t->next){double f[16];transform_function_matrix(t,0,0,true,f);matrix4_multiply(m,m,f);}
}
/* A function whose x/y linear part is the identity only translates. */
static bool transform_function_translates(const struct css_transform *t){
    switch(t->kind){
    case CT_TRANSLATE:case CT_PERSPECTIVE:return true;
    case CT_SCALE:return t->v[0]==1&&t->v[1]==1;
    case CT_SKEW:return t->v[0]==0&&t->v[1]==0;
    case CT_ROTATE:return t->v[3]==0||(t->v[0]==0&&t->v[1]==0&&t->v[2]==0);
    case CT_MATRIX:return t->m[0]==1&&t->m[1]==0&&t->m[4]==0&&t->m[5]==1;
    }
    return false;
}
bool css_transform_general(const style_t *s){
    if(!s)return false;
    if(offset_path(s))return true; /* the offset transform is box_transform_matrix's */
    if(s->rotate&&!transform_function_translates(s->rotate))return true;
    if(s->scale&&!transform_function_translates(s->scale))return true;
    for(const struct css_transform *t=s->transform;t;t=t->next)if(!transform_function_translates(t))return true;
    return false;
}
bool box_transformed(const box_t *b){
    return b&&b->st&&b->kind!=B_TEXT&&b->kind!=B_BR&&b->kind!=B_INLINE&&css_transform_general(b->st);
}
float box_translation_axis(const box_t *b,unsigned axis,unsigned sources){
    if(!b||!b->st||axis>1||b->kind==B_TEXT||b->kind==B_BR||b->kind==B_INLINE)return 0;
    if(css_transform_general(b->st))return 0; /* box_transform_matrix owns every component */
    float base=axis?b->h+b->p[0]+b->p[2]+b->b[0]+b->b[2]:b->w+b->p[1]+b->p[3]+b->b[1]+b->b[3];
    double value=(sources&CSS_TRANSLATE_INDIVIDUAL)&&b->st->translate.active?len_resolve(&b->st->translate.xy[axis],base):0;
    if(sources&CSS_TRANSLATE_TRANSFORM)for(const struct css_transform *t=b->st->transform;t;t=t->next)
        value+=t->kind==CT_TRANSLATE?len_resolve(&t->xy[axis],base):t->kind==CT_MATRIX?t->m[axis?7:3]:0;
    return value>FLT_MAX?FLT_MAX:value< -FLT_MAX? -FLT_MAX:(float)value;
}
/* The offset transform's parts in document coordinates: the offset position (px, py), the anchor
   point (ax, ay) and the rotation in degrees. */
struct offset_used { double px, py, ax, ay, angle; };
/* A reference box as the outline motion-1 §2.1 and css-shapes-1 §3.3 give it: the rectangle with
   the corners its border radii round, r its rectangle from b's border box's top left. */
static void box_outline(const box_t *b,uint8_t which,float r[4],struct css_shape *s){
    float rx[4],ry[4];
    box_reference_box(b,which,r,rx,ry);
    memset(s,0,sizeof *s);
    s->kind=CSS_SHAPE_INSET;s->box=which;
    for(int i=0;i<4;i++){
        s->inset[i].kind=LK_LEN;
        s->corner[i].axis[0]=(len_t){.kind=LK_LEN,.px=rx[i]};s->corner[i].axis[1]=(len_t){.kind=LK_LEN,.px=ry[i]};
    }
}
/* motion-1 §2.7: where offset-path, offset-distance, offset-position, offset-anchor and offset-rotate
   put b, whose transform reference box is (x y w h) and origin (ox, oy). The path's reference box is
   the <coord-box> of the box that establishes b's containing block. */
static bool box_offset_used(const box_t *b,double x,double y,float w,float h,double ox,double oy,struct offset_used *u){
    const struct css_offset *o=css_style_offset(b->st);
    const struct css_shape *path=o->path;
    float element[4]={box_visual_x(b)-b->p[3]-b->b[3],box_visual_y(b)-b->p[0]-b->b[0],
                      b->w+b->p[1]+b->p[3]+b->b[1]+b->b[3],b->h+b->p[0]+b->p[2]+b->b[0]+b->b[2]};
    const box_t *c=b->cb?b->cb:b->parent;
    float cb[4];
    struct css_shape outline;
    struct css_offset offset=*o;
    if(c&&c->kind!=B_TEXT&&c->kind!=B_BR) {
        uint8_t which=path->box==CSS_BOX_MARGIN?CSS_BOX_BORDER:path->box;
        if(path->kind==CSS_SHAPE_BOX){box_outline(c,which,cb,&outline);offset.path=&outline;}
        else{float rx[4],ry[4];box_reference_box(c,which,cb,rx,ry);}
        double cx=box_visual_x(c)-c->p[3]-c->b[3],cy=box_visual_y(c)-c->p[0]-c->b[0];
        /* the path is in the containing block's content, which scrolls */
        if(c->node&&box_element_scrollable(c)){cx-=c->node->scroll_x;cy-=c->node->scroll_y;}
        cb[0]+=(float)cx;cb[1]+=(float)cy;
    } else { /* the initial containing block */
        const web_doc *d=b->node?b->node->owner:NULL;
        cb[0]=cb[1]=0;cb[2]=d?(float)d->width:0;cb[3]=d?(float)d->height:0;
        if(path->kind==CSS_SHAPE_BOX){
            memset(&outline,0,sizeof outline);outline.kind=CSS_SHAPE_INSET;
            for(int i=0;i<4;i++)outline.inset[i].kind=outline.corner[i].axis[0].kind=outline.corner[i].axis[1].kind=LK_LEN;
            offset.path=&outline;
        }
    }
    double direction;
    if(!css_offset_place(&offset,cb,element,&u->px,&u->py,&direction))return false;
    /* offset-anchor (motion-1 §2.4): auto is transform-origin's point */
    if(o->anchor[0].kind==LK_AUTO){u->ax=ox;u->ay=oy;}
    else{u->ax=x+len_resolve(&o->anchor[0],w);u->ay=y+len_resolve(&o->anchor[1],h);}
    u->angle=(o->rotate.auto_rotate?direction:0)+o->rotate.angle;
    return isfinite(u->ax)&&isfinite(u->ay)&&isfinite(u->angle);
}
static bool style_transform_matrix(const style_t *st,double x,double y,float w,float h,const box_t *b,double out[6]);
bool box_transform_matrix(const box_t *b,double out[6]){
    if(!box_transformed(b))return false;
    const style_t *st=b->st;
    bool content=st->transform_box==TB_FILL_BOX||st->transform_box==TB_CONTENT_BOX;
    float w=content?b->w:b->w+b->p[1]+b->p[3]+b->b[1]+b->b[3],h=content?b->h:b->h+b->p[0]+b->p[2]+b->b[0]+b->b[2];
    double x=box_visual_x(b)-(content?0:b->p[3]+b->b[3]),y=box_visual_y(b)-(content?0:b->p[0]+b->b[0]);
    return style_transform_matrix(st,x,y,w,h,b,out);
}
/* SVG graphics take the transform properties only; the offset transform is the CSS boxes' */
bool css_style_transform_matrix(const style_t *st,double x,double y,float w,float h,double out[6]){
    return st&&transform_properties(st)&&style_transform_matrix(st,x,y,w,h,NULL,out);
}
static bool style_transform_matrix(const style_t *st,double x,double y,float w,float h,const box_t *b,double out[6]){
    if(!css_translation_active(st))return false;
    double ox=x+len_resolve(&st->transform_origin[0],w),oy=y+len_resolve(&st->transform_origin[1],h);
    /* CSS Transforms 2: translate, rotate, scale, the offset transform (motion-1 §2.7), then transform,
       around the origin. */
    double m[16],f[16];
    for(int i=0;i<16;i++)m[i]=i%5?0:1;
    m[3]=ox;m[7]=oy;
    if(st->translate.active){
        struct css_transform t={.kind=CT_TRANSLATE};t.xy[0]=st->translate.xy[0];t.xy[1]=st->translate.xy[1];
        transform_function_matrix(&t,w,h,false,f);matrix4_multiply(m,m,f);
    }
    if(st->rotate){transform_function_matrix(st->rotate,w,h,false,f);matrix4_multiply(m,m,f);}
    if(st->scale){transform_function_matrix(st->scale,w,h,false,f);matrix4_multiply(m,m,f);}
    struct offset_used off;
    if(b&&offset_path(st)&&box_offset_used(b,x,y,w,h,ox,oy,&off)){
        /* the anchor point to the offset position, rotated around it */
        double a=off.angle*M_PI/180;
        for(int i=0;i<16;i++)f[i]=i%5?0:1;
        f[3]=off.px-ox;f[7]=off.py-oy;matrix4_multiply(m,m,f);
        for(int i=0;i<16;i++)f[i]=i%5?0:1;
        f[0]=cos(a);f[1]=-sin(a);f[4]=sin(a);f[5]=cos(a);matrix4_multiply(m,m,f);
        for(int i=0;i<16;i++)f[i]=i%5?0:1;
        f[3]=ox-off.ax;f[7]=oy-off.ay;matrix4_multiply(m,m,f);
    }
    css_transform_list_matrix(st->transform,w,h,f);matrix4_multiply(m,m,f);
    for(int i=0;i<16;i++)f[i]=i%5?0:1;
    f[3]=-ox;f[7]=-oy;matrix4_multiply(m,m,f);
    out[0]=m[0];out[1]=m[4];out[2]=m[1];out[3]=m[5];out[4]=m[3];out[5]=m[7];
    for(int i=0;i<6;i++)if(!isfinite(out[i]))return false;
    return true;
}
void box_transform_rect(const box_t *b,double rect[4]){
    for(;b;b=b->parent){
        double m[6];if(!box_transform_matrix(b,m))continue;
        double x0=rect[0],y0=rect[1],x1=rect[0]+rect[2],y1=rect[1]+rect[3];
        double xs[4]={x0,x1,x0,x1},ys[4]={y0,y0,y1,y1},l=INFINITY,t=INFINITY,r=-INFINITY,bt=-INFINITY;
        for(int i=0;i<4;i++){
            double x=m[0]*xs[i]+m[2]*ys[i]+m[4],y=m[1]*xs[i]+m[3]*ys[i]+m[5];
            l=fmin(l,x);t=fmin(t,y);r=fmax(r,x);bt=fmax(bt,y);
        }
        rect[0]=l;rect[1]=t;rect[2]=r-l;rect[3]=bt-t;
    }
}
bool box_filter_containing_block(const box_t *b) {
    const box_t *effect = b;
    /* wrapped() paints an element's effect on the outer box once, but the
       inner box owns its real padding/border containing-block geometry. */
    if (b && b->wrapped_inner && b->parent) effect = b->parent;
    return effect && effect->st && b->kind != B_TEXT && b->kind != B_BR &&
        ((effect->kind!=B_INLINE&&css_translation_active(effect->st))||(effect->st->filter&&
         !(b->node&&b->node->owner&&b->node==b->node->owner->html)));
}
bool box_viewport_fixed(const box_t *b) {
    return b && b->st && b->st->position == POS_FIXED &&
        (!b->cb || !b->cb->parent);
}
void box_abs_cb_rect(const box_t *cb, const web_doc *d, float rect[4]) {
    if (!cb || cb == d->root_box) {
        rect[0] = rect[1] = 0; rect[2] = (float)d->width; rect[3] = (float)d->height;
    } else if (cb->kind == B_INLINE && cb->inline_cb_valid) {
        rect[0] = cb->inline_cb_left; rect[1] = cb->inline_cb_top;
        rect[2] = fmaxf_(0, cb->inline_cb_right - cb->inline_cb_left);
        rect[3] = fmaxf_(0, cb->inline_cb_bottom - cb->inline_cb_top);
    } else {
        rect[0] = -cb->p[3]; rect[1] = -cb->p[0];
        rect[2] = cb->w + cb->p[1] + cb->p[3]; rect[3] = cb->h + cb->p[0] + cb->p[2];
    }
}
/* Nonzero only inside a paint. Paint visits every box several times and each
   visual coordinate walks its containing-block and parent chains, so a deep
   tree repainted every frame paid O(boxes x depth) repeatedly. Parallel
   raster workers may memoize concurrently; they store identical values and
   publish the generation after the value. */
static uint32_t visual_memo_generation, visual_memo_counter;
uint32_t box_visual_memo_begin(void) {
    uint32_t previous = visual_memo_generation;
    if (++visual_memo_counter == 0) ++visual_memo_counter;
    __atomic_store_n(&visual_memo_generation, visual_memo_counter, __ATOMIC_RELEASE);
    return previous;
}
void box_visual_memo_end(uint32_t previous) {
    __atomic_store_n(&visual_memo_generation, previous, __ATOMIC_RELEASE);
}
static float box_visual_x_compute(const box_t *b);
static float box_visual_y_compute(const box_t *b);
float box_visual_x(const box_t *b) {
    uint32_t generation = __atomic_load_n(&visual_memo_generation, __ATOMIC_ACQUIRE);
    if (!generation) return box_visual_x_compute(b);
    box_t *memo = (box_t *)b;
    if (__atomic_load_n(&memo->visual_memo_x, __ATOMIC_ACQUIRE) == generation) return memo->visual_x;
    float x = box_visual_x_compute(b);
    memo->visual_x = x;
    __atomic_store_n(&memo->visual_memo_x, generation, __ATOMIC_RELEASE);
    return x;
}
float box_visual_y(const box_t *b) {
    uint32_t generation = __atomic_load_n(&visual_memo_generation, __ATOMIC_ACQUIRE);
    if (!generation) return box_visual_y_compute(b);
    box_t *memo = (box_t *)b;
    if (__atomic_load_n(&memo->visual_memo_y, __ATOMIC_ACQUIRE) == generation) return memo->visual_y;
    float y = box_visual_y_compute(b);
    memo->visual_y = y;
    __atomic_store_n(&memo->visual_memo_y, generation, __ATOMIC_RELEASE);
    return y;
}
/* position: sticky (css-position-3 §3.4). The box shifts to keep its border box inside its
   nearest scrollport, inset by its own insets, as far as its margin box stays inside its
   containing block. The shift follows scrolling, so like a scroll container's it is visual
   only: layout and box_abs keep the box in flow, and a wrapped element's inner box moves with
   the outer one, which its node names. */
static bool box_sticky(const box_t *b) {
    return b->st && b->st->position == POS_STICKY && b->node && b->node->box == b &&
           b->kind != B_INLINE && b->kind != B_TEXT;
}
static void sticky_offset(const box_t *s, float shift[2]);

/* b's visual coordinate on one axis (0: x, 1: y), with its sticky ancestors' shifts but not its own */
static double visual_unstuck(const box_t *b, unsigned axis) {
    double v = axis ? box_abs_y(b) : box_abs_x(b);
    for(const box_t *a=b;a;a=a->parent){v+=box_translation_axis(a,axis,CSS_TRANSLATE_INDIVIDUAL|CSS_TRANSLATE_TRANSFORM);if(box_viewport_fixed(a))break;}
    if(b->node && b->node->owner && b->node->owner->marquees.n)
        for(const box_t *a=b;a;a=a->parent){
            if(a!=b&&web_marquee_box(a))v+=axis?a->node->marquee_y:a->node->marquee_x;
            if(box_viewport_fixed(a))break;
        }
    for (const box_t *c = b; c; c = c->cb) {
        if (c != b) {
            double scroll = c->node ? axis ? c->node->scroll_y : c->node->scroll_x : 0;
            if (scroll != 0 && box_element_scrollable(c)) v -= scroll;
            if (box_sticky(c)) { float shift[2]; sticky_offset(c, shift); v += shift[axis]; }
        }
        if (box_viewport_fixed(c)) {
            /* Visual coordinates are document coordinates. Layout keeps the
               fixed containing block in viewport coordinates, so publish the
               real viewport offset once, also for every fixed descendant. */
            if (c->node && c->node->owner) v += axis ? c->node->owner->view_y : c->node->owner->view_x;
            break;
        }
    }
    return v;
}

static void sticky_offset(const box_t *s, float shift[2]) {
    uint32_t generation = __atomic_load_n(&visual_memo_generation, __ATOMIC_ACQUIRE);
    box_t *memo = (box_t *)s;
    if (generation && __atomic_load_n(&memo->sticky_memo, __ATOMIC_ACQUIRE) == generation) {
        shift[0] = memo->sticky_dx; shift[1] = memo->sticky_dy;
        return;
    }
    /* the scrollport: the nearest scroll container on the containing-block chain, else the viewport */
    const box_t *sc = NULL;
    for (const box_t *c = s->cb; c && !sc; c = c->cb) {
        if (box_element_scrollable(c)) sc = c;
        else if (box_viewport_fixed(c)) break;
    }
    const web_doc *d = s->node->owner;
    double port[4];
    if (sc) {
        port[0] = box_visual_x(sc) - sc->p[3]; port[1] = box_visual_y(sc) - sc->p[0];
        port[2] = sc->w + sc->p[1] + sc->p[3]; port[3] = sc->h + sc->p[0] + sc->p[2];
    } else {
        port[0] = d ? d->view_x : 0; port[1] = d ? d->view_y : 0;
        port[2] = d ? d->width : 0; port[3] = d ? d->height : 0;
    }
    /* the content box of its containing block: a table part's is its table's, and a scroll
       container's is its scrollable area, which moves with the scroll */
    const box_t *cb = s->cb;
    if (s->kind == B_CELL || s->kind == B_ROW || s->kind == B_ROW_GROUP)
        for (const box_t *t = cb; t; t = t->cb)
            if (t->kind == B_TABLE) { cb = t; break; }
    double area[4] = {0};
    if (cb && cb == sc) {
        area[0] = port[0] + sc->p[3] - sc->node->scroll_x; area[1] = port[1] + sc->p[0] - sc->node->scroll_y;
        area[2] = fmaxf_(sc->w, sc->scroll_w - sc->p[1] - sc->p[3]);
        area[3] = fmaxf_(sc->h, sc->scroll_h - sc->p[0] - sc->p[2]);
    } else if (cb) {
        area[0] = box_visual_x(cb); area[1] = box_visual_y(cb) + cb->content_dy; area[2] = cb->w; area[3] = cb->h;
    }
    /* its border box where it would be */
    bool row = s->kind == B_ROW || s->kind == B_ROW_GROUP;
    double x = visual_unstuck(s, 0), y = visual_unstuck(s, 1);
    double lo[2] = {row ? x : x - s->p[3] - s->b[3], row ? y : y - s->p[0] - s->b[0]};
    double hi[2] = {lo[0] + (row ? s->w : s->w + s->p[1] + s->p[3] + s->b[1] + s->b[3]),
                    lo[1] + (row ? s->h : s->h + s->p[0] + s->p[2] + s->b[0] + s->b[2])};
    for (unsigned axis = 0; axis < 2; axis++) {
        /* inset and m are top, right, bottom, left: the start side is the top or the left */
        const len_t *start = &s->st->inset[axis ? 0 : 3], *end = &s->st->inset[axis ? 2 : 1];
        double size = port[axis + 2], v = 0;
        double back = cb ? lo[axis] - s->m[axis ? 0 : 3] - area[axis] : INF;
        double forth = cb ? area[axis] + area[axis + 2] - hi[axis] - s->m[axis ? 2 : 1] : INF;
        if (!len_auto(end)) {
            double edge = port[axis] + size - len_resolve(end, (float)size);
            if (hi[axis] > edge) v = -fmin(hi[axis] - edge, fmax(0, back));
        }
        /* the start side wins when the scrollport is too small for both */
        if (!len_auto(start)) {
            double edge = port[axis] + len_resolve(start, (float)size);
            if (lo[axis] < edge) v = fmin(edge - lo[axis], fmax(0, forth));
        }
        shift[axis] = isfinite(v) ? (float)v : 0;
    }
    if (generation) {
        memo->sticky_dx = shift[0]; memo->sticky_dy = shift[1];
        __atomic_store_n(&memo->sticky_memo, generation, __ATOMIC_RELEASE);
    }
}

static float box_visual_x_compute(const box_t *b) {
    double x = visual_unstuck(b, 0);
    if (box_sticky(b)) { float shift[2]; sticky_offset(b, shift); x += shift[0]; }
    return (float)x;
}

static float box_visual_y_compute(const box_t *b) {
    double y = visual_unstuck(b, 1);
    if (box_sticky(b)) { float shift[2]; sticky_offset(b, shift); y += shift[1]; }
    return (float)y;
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
    const box_t *box; /* the float; its shape-outside narrows line boxes only */
    float shape_margin;
};
struct bfc {
    struct fl *v;
    int n, cap;
    float last_top;
};

static void bfc_free(struct bfc *f) { free(f->v); }

static void bfc_add(struct bfc *f, float x, float y, float w, float h, int side, const box_t *box, float shape_margin) {
    if (f->n == f->cap) {
        f->cap = f->cap ? f->cap * 2 : 8;
        f->v = realloc(f->v, sizeof *f->v * (size_t)f->cap);
    }
    f->v[f->n++] = (struct fl){x, y, w, h, side, box, shape_margin};
    f->last_top = y;
}

/* css-shapes-1 §4: the float area of a's shape-outside within the band y .. y+h, clipped to its
   margin box; false when the band misses it, and the float leaves those lines alone. */
static bool float_area(const struct fl *a, float y, float h, float *x0, float *x1) {
    const box_t *b = a->box;
    const struct css_shape *s = css_style_float_shape(b->st)->outside;
    float r[4];
    struct css_shape outline;
    if (s->kind == CSS_SHAPE_BOX) { box_outline(b, s->box, r, &outline); s = &outline; }
    else { float rx[4], ry[4]; box_reference_box(b, s->box, r, rx, ry); }
    r[0] += a->x + b->m[3]; r[1] += a->y + b->m[0];
    if (!css_shape_band(s, r, a->shape_margin, y, y + h, x0, x1)) return false;
    *x0 = fmaxf_(*x0, a->x); *x1 = fminf_(*x1, a->x + a->w);
    return *x1 > *x0;
}

/* the free interval within [x0, x1] for the band y .. y+h; lines: for line boxes, which floats'
   shape-outside narrows */
static void bfc_space(const struct bfc *f, float y, float h, float x0, float x1, float *l, float *r, bool lines) {
    *l = x0;
    *r = x1;
    if (h < 1) h = 1;
    for (int i = 0; f && i < f->n; i++) {
        const struct fl *a = &f->v[i];
        if (a->y >= y + h || a->y + a->h <= y || a->h <= 0) continue;
        float left = a->x, right = a->x + a->w;
        const struct css_shape *shape = lines && a->box && a->box->st ? css_style_float_shape(a->box->st)->outside : NULL;
        if (shape && shape->kind != CSS_SHAPE_URL && !float_area(a, y, h, &left, &right)) continue;
        if (a->side == FL_LEFT) *l = fmaxf_(*l, right);
        else *r = fminf_(*r, left);
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
static void layout_inner_used(box_t *b, struct bfc *f, float ox, float oy, float cbh, float usedh);
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

static void replaced_natural(box_t *c, float *iw, float *ih, double *ratio) {
    node_t *n = c->node;
    *iw = *ih = *ratio = 0;
    switch (c->atomic) {
    case AT_IMG: {
        struct web_image *im = n->image >= 0 && n->image < D->images.n ? D->images.v[n->image] : NULL;
        if (im && im->img) {
            *iw = (float)im->img->w;
            *ih = (float)im->img->h;
            *ratio = *ih > 0 ? (double)*iw / *ih : 0;
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
        if (*iw > 0 && *ih > 0) *ratio = (double)*iw / *ih;
        else if (view == 1) *ratio = (double)vb[2] / vb[3];
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
        *ratio = *ih>0 ? (double)*iw / *ih : 0;
        break;
    }
}

static void size_replaced(box_t *c, float cbw, float cbh) {
    float iw, ih;
    /* Round only the final used dimension to the layout float. A float ratio
       such as 3/5 makes 60/(3/5) fall just below 100 before CSSOM/pixel integer
       conversion, losing a pixel of image and subsequent flow geometry. */
    double ratio;
    replaced_natural(c, &iw, &ih, &ratio);
    if(size_container(c)) {
        iw=0;ratio=0;
        if(c->st->container_type==CT_SIZE)ih=0;
    }
    float w = spec_w(c, &c->st->width, cbw), h = spec_h(c, &c->st->height, cbh);
    bool derive_w = w < 0, derive_h = h < 0;
    if (c->atomic == AT_SVG && !size_container(c)) {
        /* CSS embedding uses the SVG intrinsic ratio before viewport auto
           fallback. A definite CSS width with auto height (or the converse)
           must not acquire the containing block's unrelated other dimension.
           Preserve the existing outermost-auto width negotiation only when
           neither CSS axis supplies a size; intrinsic passes have no base. */
        if (ratio > 0) {
            if (w < 0 && h < 0 && len_auto(&c->st->width) && cbw >= 0) w = cbw;
        } else {
            if (len_auto(&c->st->width) && cbw >= 0) w = cbw;
            if (len_auto(&c->st->height) && cbh >= 0) h = cbh;
        }
    }
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
    if (ratio > 0 && !size_container(c)) {
        /* Transfer min constraints too when the opposite axis is automatic.
           The final independent clamps retain both author limits when those
           limits cannot be satisfied at the same aspect ratio. */
        float cw = clamp_w(c, w, cbw);
        if (derive_h && cw != w) h = cw / ratio;
        w = cw;
        float ch = clamp_h(c, h, cbh);
        if (derive_w && ch != h) w = ch * ratio;
        h = ch;
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
    if(size_container(c)){w=0;if(st->container_type==CT_SIZE)h=0;}
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
    float descent; /* native font metric collected during line sizing */
    bool float_sized; /* IT_FLOAT already sized by a line-fit test */
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
    box_t *paint_owner;
    bool collect_paint;
};

static float va_shift(const style_t *st) {
    switch (st->vertical_align) {
    case VA_SUB: return -st->font_size * 0.25f;
    case VA_SUPER: return st->font_size * 0.4f;
    case VA_LEN: return st->vertical_align_px;
    }
    return 0;
}

/* Common East Asian soft-wrap opportunities. These supplement whitespace
   breaks, not break-all: Latin words stay intact, and Japanese opening/
   closing punctuation, small kana and combining marks stay with their base.
   This is not the full Unicode line-breaking algorithm (Thai dictionary and
   grapheme/emoji tailoring remain separate work). Both layout and intrinsic
   measurement use these same items, including text inside inline elements. */
static bool east_asian_letter(uint32_t c) {
    return (c>=0x3041 && c<=0x3096) || (c>=0x30a1 && c<=0x30fa) ||
           (c>=0x31f0 && c<=0x31ff) || (c>=0x3400 && c<=0x9fff) ||
           (c>=0xac00 && c<=0xd7a3) || (c>=0xf900 && c<=0xfaff) ||
           (c>=0xff66 && c<=0xff9d) || (c>=0x20000 && c<=0x3ffff);
}
static bool no_break_after(uint32_t c) {
    switch(c) {
    case '(': case '[': case '{': case 0x2018: case 0x201c:
    case 0x3008: case 0x300a: case 0x300c: case 0x300e: case 0x3010:
    case 0x3014: case 0x3016: case 0x3018: case 0x301a:
    case 0xff08: case 0xff3b: case 0xff5b: case 0xff5f: case 0xff62:
        return true;
    }
    return c==0x00a0 || c==0x202f || c==0x2060 || c==0xfeff || c==0x200d;
}
static bool no_break_before(uint32_t c) {
    switch(c) {
    case ')': case ']': case '}': case ',': case '.': case ':': case ';': case '!': case '?':
    case 0x2019: case 0x201d: case 0x3001: case 0x3002: case 0x3005:
    case 0x3009: case 0x300b: case 0x300d: case 0x300f: case 0x3011:
    case 0x3015: case 0x3017: case 0x3019: case 0x301b: case 0x303b:
    case 0x3041: case 0x3043: case 0x3045: case 0x3047: case 0x3049:
    case 0x3063: case 0x3083: case 0x3085: case 0x3087: case 0x308e:
    case 0x3095: case 0x3096: case 0x309d: case 0x309e:
    case 0x30a1: case 0x30a3: case 0x30a5: case 0x30a7: case 0x30a9:
    case 0x30c3: case 0x30e3: case 0x30e5: case 0x30e7: case 0x30ee:
    case 0x30f5: case 0x30f6: case 0x30fc: case 0x30fd: case 0x30fe:
    case 0xff01: case 0xff09: case 0xff0c: case 0xff0e: case 0xff1a:
    case 0xff1b: case 0xff1f: case 0xff3d: case 0xff5d: case 0xff60:
    case 0xff61: case 0xff63: case 0xff64: case 0xff9e: case 0xff9f:
        return true;
    }
    return (c>=0x0300 && c<=0x036f) || (c>=0x1ab0 && c<=0x1aff) ||
           (c>=0x1dc0 && c<=0x1dff) || (c>=0x20d0 && c<=0x20ff) ||
           (c>=0xfe00 && c<=0xfe0f) || (c>=0xfe20 && c<=0xfe2f) ||
           (c>=0xe0100 && c<=0xe01ef) || c==0x3099 || c==0x309a ||
           c==0x00a0 || c==0x202f || c==0x2060 || c==0xfeff || c==0x200d;
}
static bool east_asian_break(uint32_t left,uint32_t right) {
    return (east_asian_letter(left) || east_asian_letter(right)) &&
           !no_break_after(left) && !no_break_before(right);
}

static float control_tab_width(const style_t *st, const wfont *font, float x) {
    float stop = 8 * text_width_uncached(st, font, " ", 1);
    return stop > 0 ? stop - fmodf(fmaxf_(0, x), stop) : 0;
}
float control_text_width(const style_t *st, const char *s, size_t n) {
    wfont font = style_font(st);
    float width = 0;
    size_t first = 0;
    for (size_t at = 0; at < n; at++) if (s[at] == '\t') {
        width += text_width_uncached(st, &font, s + first, at - first);
        width += control_tab_width(st, &font, width); first = at + 1;
    }
    return width + text_width_uncached(st, &font, s + first, n - first);
}
static float control_measure(const void *opaque, const char *s, size_t at, size_t next, float x) {
    const style_t *st = opaque;
    wfont font = style_font(st);
    return next == at + 1 && s[at] == '\t' ? control_tab_width(st, &font, x) : text_width_uncached(st, &font, s + at, next - at);
}
size_t control_text_boundary(const style_t *st, const char *s, size_t n, float x) {
    return layout_text_boundary(s, n, -1, x, control_measure, st);
}
bool control_text_lines(const box_t *b, const style_t *st, const char *text,
                        control_text_line_fn visit, void *opaque, float *width, float *height) {
    if (!b || !st || !text || !width || !height) return false;
    bool soft = st->white_space != WS_PRE && st->white_space != WS_NOWRAP;
    wfont font = style_font(st);
    float limit = fmaxf_(0, b->w), lh = style_line_height(st), top = 0, widest = 0;
    size_t length = strlen(text), first = 0, work = 0;
    for (;;) {
        size_t at = first, end = first, next = first, opportunity = first;
        float used = 0, opportunity_width = 0;
        uint32_t previous = 0;
        bool hard = false;
        while (at < length) {
            if (!(work++ & 63) && b->node) {
                /* Pure AP layout helpers may only observe cancellation;
                   paint/ordinary layout retain the owner's safe checkpoint. */
                struct layout_context *context = thread_local_get(1);
                if (!(context ? layout_checkpoint() : web_native_checkpoint(b->node->owner))) return false;
            }
            size_t after = wf_grapheme_next(text, length, at);
            uint32_t cp; wf_grapheme_decode(text + at, after - at, &cp);
            if (cp == '\n' || cp == '\r') { end = at; next = after; hard = true; break; }
            if (soft && at > first && east_asian_break(previous, cp)) {
                opportunity = at; opportunity_width = used;
            }
            float advance = cp == '\t' ? control_tab_width(st, &font, used) :
                text_width_uncached(st, &font, text + at, after - at);
            /* Preserve a word where possible; an oversized word wraps only
               at extended grapheme boundaries. Every emitted line advances,
               even when CSS gives the control a zero content width. */
            if (soft && at > first && used + advance > limit) {
                end = next = opportunity > first ? opportunity : at;
                if (opportunity > first) used = opportunity_width;
                break;
            }
            used += advance; at = after; end = next = at; previous = cp;
            if (soft && (cp == ' ' || cp == '\t' || cp == '-')) {
                opportunity = at; opportunity_width = used;
            }
        }
        if (!isfinite(used) || !isfinite(top + lh)) return false;
        struct control_text_line line = {first, end - first, next, used, top, lh, hard};
        if (visit) visit(&line, opaque);
        widest = fmaxf_(widest, used); top += lh;
        if (!hard && next == length) break;
        first = next; /* a final LF contributes the following empty line */
    }
    *width = widest; *height = top; return true;
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
        bool soft=false;
        while (we < e && *we != ' ' && *we != '\n') {
            uint32_t left,right;
            int bytes=gfx_utf8_decode(we,&left);
            if(bytes<1)bytes=1;
            if(bytes>e-we)bytes=(int)(e-we);
            const char *next=we+bytes;
            if(wrap && next<e && *next!=' ' && *next!='\n') {
                gfx_utf8_decode(next,&right);
                if(east_asian_break(left,right)){we=next;soft=true;break;}
            }
            we=next;
        }
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
        it->brk = wrap && (se > we || soft);
        it->link = s->link;
        it->shift = s->shift;
        s->last_space = se > we;
        p = se;
    }
}

static void build_items(box_t *parent, struct ibuild *s) {
    for (box_t *c = parent->first; c; c = c->next) {
        if (!layout_checkpoint()) break;
        layout_media_checkpoint();
        if (s->collect_paint) {
            c->inline_paint_owner = s->paint_owner;
            c->frag_first = c->frag_end = 0; /* this line layout records them afresh */
        }
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
            box_t *saved_owner = s->paint_owner;
            if (c->st->filter || c->st->opacity < 1) s->paint_owner = c;
            if (s->collect_paint) {
                c->inline_paint_owner = s->paint_owner;
                c->inline_run_first = c->inline_run_last = c->inline_deco_first = c->inline_deco_last = -1;
                c->inline_paint_source = NULL; c->inline_cb_valid = false;
            }
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
            s->paint_owner = saved_owner;
            break;
        }
        default: {
            struct item *it = ipush(s->v, IT_ATOMIC);
            it->box = c;
            it->st = c->st;
            it->link = s->link;
            it->shift = s->shift + va_shift(c->st);
            /* CSS Text 3 §5.1: a soft wrap opportunity before and after an atomic inline,
               allowed by the white-space of the nearest common ancestor of the atomic and its
               neighbour. Its own white-space only governs its contents. */
            it->brk = parent->st->white_space != WS_NOWRAP && parent->st->white_space != WS_PRE;
            /* Before it: after the item ahead of the inline boxes it is the first content of,
               so their start edges move to the next line with it. */
            size_t at = s->v->n - 1;
            box_t *ancestor = parent;
            while (at > 0 && s->v->v[at - 1].kind == IT_OPEN) { at--; ancestor = s->v->v[at].box->parent; }
            if (at > 0 && ancestor && ancestor->st->white_space != WS_NOWRAP && ancestor->st->white_space != WS_PRE) {
                struct item *pv = &s->v->v[at - 1];
                if (pv->kind == IT_TEXT || pv->kind == IT_ATOMIC || pv->kind == IT_CLOSE) pv->brk = true;
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
    float ox, oy, cbh;
    struct ivec *items;
    sbuf runs, decos, lines;
    struct open_box *open;
    int nopen;
    float y;           /* top of the current line */
    float left, right; /* available space of the current line */
    bool first_line;
    float first_bl, last_bl;
    float strut_lh;
    int *pending_floats;
    int npending;
};

static void place_float(box_t *c, box_t *parent, struct bfc *f, float ox, float oy, float y_rel, float cbh);
static void position_float(box_t *c, float cw, struct bfc *f, float ox, float oy, float y_rel);
static bool float_fits_line(struct iline *L, box_t *c, float used);

static void line_space(struct iline *L) {
    box_t *b = L->blk;
    bfc_space(L->f, L->oy + L->y, L->strut_lh, L->ox, L->ox + b->w, &L->left, &L->right, true);
    L->left -= L->ox;
    L->right -= L->ox;
    if (L->first_line) {
        float ind = len_resolve(&b->st->text_indent, b->w);
        L->left += ind;
    }
}

static void add_deco(struct iline *L, box_t *ib, float x0, float x1, float baseline, bool first, bool last) {
    const style_t *st = ib->st;
    /* These are also the element's real line-fragment border boxes. A plain
       link still has geometry and is hit-testable without a painted background. */
    wfont f = style_font(st);
    float asc, desc;
    wf_metrics(&f, &asc, &desc);
    struct deco d = {0};
    d.x = x0;
    d.w = x1 - x0;
    d.y = baseline - asc - ib->p[0] - ib->b[0];
    d.h = asc + desc + ib->p[0] + ib->p[2] + ib->b[0] + ib->b[2];
    d.st = (style_t *)st;
    d.first = first;
    d.last = last;
    d.node = ib->node;
    d.paint_owner = ib->inline_paint_owner; d.paint_next = -1;
    /* Keep the first/last line padding-edge rectangle for an inline filter
       containing block without making the inline an atomic flow box. */
    ib->cb = L->blk;
    if (!ib->inline_cb_valid) {
        ib->inline_cb_left = d.x + ib->b[3]; ib->inline_cb_top = d.y + ib->b[0];
        ib->inline_cb_valid = true;
    }
    ib->inline_cb_right = d.x + d.w - ib->b[1];
    ib->inline_cb_bottom = d.y + d.h - ib->b[2];
    int index = (int)(L->decos.n / sizeof d);
    if (!ib->frag_end) ib->frag_first = index;
    ib->frag_end = index + 1;
    sb_put(&L->decos, (const char *)&d, sizeof d);
}

/* vertical extent of an item above and below the baseline */
static void item_vext(struct item *it, float *above, float *below) {
    if (it->kind == IT_ATOMIC) {
        box_t *c = it->box;
        float h = c->h + vext(c) + c->m[0] + c->m[2];
        /* css-align-3 §9.1: only a block container that is a scroll container
           keeps the legacy block-end margin edge; inline-flex, inline-grid and
           inline-table use their content baseline whatever their overflow, and
           HTML button layout derives a button's baseline from its contents. */
        uint8_t display = c->st->display;
        bool button = c->node && !c->node->foreign && c->node->tag == T_button;
        bool legacy_end = (c->st->overflow == OV_HIDDEN || c->st->overflow == OV_SCROLL ||
                           c->st->overflow == OV_AUTO) && !button &&
                          display != D_INLINE_FLEX && display != D_INLINE_GRID && display != D_INLINE_TABLE;
        float bl = c->last_baseline >= 0 && !legacy_end
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
    it->descent = desc;
    float lh = line_height_px(st);
    float half = (lh - (asc + desc)) / 2;
    *above = asc + half + it->shift;
    *below = desc + half - it->shift;
}

static void finish_line(struct iline *L, int ls, int le, bool forced) {
    struct item *v = L->items->v;
    box_t *b = L->blk;
    int first_run = (int)(L->runs.n / sizeof(struct run));
    int first_deco = (int)(L->decos.n / sizeof(struct deco));
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
            L->open[L->nopen].box = ib;
            L->open[L->nopen].x = x + ib->m[3];
            L->open[L->nopen].first = true;
            L->nopen++;
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
                            it->box ? it->box->node : NULL, it->box ? it->box->inline_paint_owner : NULL, -1, it->descent, 0, 0};
            if (n > 0) {
                int index = (int)(L->runs.n / sizeof r);
                if (it->box && !it->box->frag_end) it->box->frag_first = index;
                if (it->box) it->box->frag_end = index + 1;
                sb_put(&L->runs, (const char *)&r, sizeof r);
            }
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
            struct run r = {x, top, it->w, NULL, 0, it->st, it->link, c, false, c->node, c->inline_paint_owner, -1, 0, 0, 0};
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
        if (layout_current()->clamp_depth || b->st->text_overflow == TO_ELLIPSIS) {
            struct line_fragment line = {first_run, (int)(L->runs.n / sizeof(struct run)),
                first_deco, (int)(L->decos.n / sizeof(struct deco)),
                L->y, L->y + lh, baseline, L->left, L->right, false, 0, 0, 0, 0, 0};
            sb_put(&L->lines, (const char *)&line, sizeof line);
        }
    }
    L->y += lh;
    /* floats that did not fit on the line just finished */
    for (int k = 0; k < L->npending; k++) {
        struct item *it = &v[L->pending_floats[k]];
        if (it->float_sized) position_float(it->box, b->w, L->f, L->ox, L->oy, L->y);
        else place_float(it->box, b, L->f, L->ox, L->oy, L->y, L->cbh);
    }
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

static node_t *inline_link_ancestor(const box_t *b) {
    /* Anonymous boxes have no extra DOM ancestry. The closest native box's
       flat-tree ancestors already include display:contents and shadow/slot
       hosts; restarting this walk at every outer box is quadratic. Top-layer
       reparenting also keeps this native DOM ancestry, not the paint parent. */
    while(b && !b->node)b=b->parent;
    for(node_t *n=b?b->node:NULL;n;n=doc_flat_parent(n))
        if(n->type==N_ELEM && n->tag==T_a && node_attr(n,"href"))return n;
    return NULL;
}
static void layout_inline(box_t *b, struct bfc *f, float ox, float oy, float cbh) {
    struct ivec iv = {0};
    struct ibuild bs = {&iv, true, NULL, 0, NULL, true};
    bs.link=inline_link_ancestor(b);
    build_items(b, &bs);
    /* widths of inline box edges and atomic inlines */
    for (int i = 0; i < iv.n; i++) {
        struct item *it = &iv.v[i];
        /* Cached boxes survive a reflow; their first-line anchor does not.
         * Clear only anchors owned by this formatting context, leaving reused
         * sibling contexts and split-inline first fragments untouched. */
        if(it->kind==IT_OPEN && it->box->node && it->box->node->anchor_block==b) {
            it->box->node->anchor_block=NULL;it->box->node->anchor_dy=0;
        }
        if (it->kind == IT_OPEN) it->w = inline_edge(it->box, true, b->w);
        else if (it->kind == IT_CLOSE) it->w = inline_edge(it->box, false, b->w);
        else if (it->kind == IT_ATOMIC) {
            box_t *c = it->box;
            /* Inline replaced elements share the containing block's definite
               height. An img inside inline picture must resolve height:100%
               against its figure, not discard it and crop a natural-height
               image at the figure's bottom. Intrinsic passes still use -1. */
            size_atomic(c, b->w, cbh);
            it->w = c->w + hext(c) + c->m[1] + c->m[3];
        }
    }
    struct iline L;
    memset(&L, 0, sizeof L);
    size_t opens=0,floats=0;
    for(int i=0;i<iv.n;i++){
        if(iv.v[i].kind==IT_OPEN)opens++;
        else if(iv.v[i].kind==IT_FLOAT)floats++;
    }
    if(opens>SIZE_MAX/sizeof *L.open || floats>SIZE_MAX/sizeof *L.pending_floats)ar_alloc(layout_scratch(),SIZE_MAX);
    if(opens)L.open=ar_alloc(&b->layout_mem,opens*sizeof *L.open);
    if(floats)L.pending_floats=ar_alloc(&b->layout_mem,floats*sizeof *L.pending_floats);
    L.blk = b;
    L.f = f;
    L.ox = ox;
    L.oy = oy;
    L.cbh = cbh;
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
                place_float(c, b, f, ox, oy, L.y, cbh);
                line_space(&L);
            } else if (f && !L.npending) {
                /* content so far, without a space that would hang at the break */
                float used = line_w + chunk_w - (i > 0 && iv.v[i - 1].space_end ? iv.v[i - 1].sw : 0);
                it->float_sized = true;
                if (float_fits_line(&L, c, used)) {
                    position_float(c, b->w, f, ox, oy, L.y);
                    line_space(&L);
                } else L.pending_floats[L.npending++] = i;
            } else L.pending_floats[L.npending++] = i;
            continue;
        }
        if (it->kind == IT_ABS) continue;
        chunk_w += it->w;
        bool end = it->kind == IT_BR || it->brk || i == iv.n - 1;
        if (!end) continue;
        /* commit the chunk cs..i */
        float trim = it->space_end ? it->sw : 0;
        float avail = L.right - L.left;
        if (line_w > 0 && line_w + chunk_w - trim > avail + LAYOUT_FIT_SLOP) {
            finish_line(&L, ls, cs, false);
            ls = cs;
            line_w = 0;
            avail = L.right - L.left;
        }
        if (line_w == 0 && chunk_w - trim > avail + LAYOUT_FIT_SLOP) {
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
            if (chunk_w - trim > avail + LAYOUT_FIT_SLOP && cs == i && it->kind == IT_TEXT && st_ws != WS_NOWRAP && st_ws != WS_PRE &&
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
        b->runs = ar_alloc(&b->layout_mem, L.runs.n);
        memcpy(b->runs, L.runs.p, L.runs.n);
    }
    b->ndecos = (int)(L.decos.n / sizeof(struct deco));
    b->decos = NULL;
    if (b->ndecos) {
        b->decos = ar_alloc(&b->layout_mem, L.decos.n);
        memcpy(b->decos, L.decos.p, L.decos.n);
    }
    /* Link only this group's existing flat line results. Nested groups own
       their own lists; paint never scans the whole formatting root per inline. */
    for (int i = 0; i < b->nruns; i++) {
        struct run *r = &b->runs[i]; box_t *owner = r->paint_owner;
        if (!owner) continue;
        owner->inline_paint_source = b;
        if (owner->inline_run_last >= 0) b->runs[owner->inline_run_last].paint_next = i;
        else owner->inline_run_first = i;
        owner->inline_run_last = i;
    }
    for (int i = 0; i < b->ndecos; i++) {
        struct deco *r = &b->decos[i]; box_t *owner = r->paint_owner;
        if (!owner) continue;
        owner->inline_paint_source = b;
        if (owner->inline_deco_last >= 0) b->decos[owner->inline_deco_last].paint_next = i;
        else owner->inline_deco_first = i;
        owner->inline_deco_last = i;
    }
    b->nlines = (int)(L.lines.n / sizeof(struct line_fragment));
    b->lines = NULL;
    if (b->nlines) {
        b->lines = ar_alloc(&b->layout_mem, L.lines.n);
        memcpy(b->lines, L.lines.p, L.lines.n);
    }
    sb_free(&L.runs);
    sb_free(&L.decos);
    sb_free(&L.lines);
    free(iv.v);
    b->baseline = L.first_bl;
    b->last_baseline = L.last_bl;
    b->h = L.y; /* the caller applies height and min/max */
}

/* ---------------------------------------------------------------- text geometry */
/* A line layout keeps, for each run, its position and the bytes of its text box's processed
   text it shows; for each text and inline box, the range of its block's runs or decos that
   holds its fragments (frag_first, frag_end). box_text_source_map (box.c) gives the bytes of
   the Text node's data those bytes came from. Together they map a DOM position to a line
   position and back, for every caller, without walking other nodes' fragments. Readers run
   between layouts (script, paint, native input), never inside one. */
void layout_runs_origin(const box_t *b, bool visual, float *x, float *y) {
    if (!visual) { *x = box_abs_x(b); *y = box_abs_y(b) + b->content_dy; return; }
    *x = box_visual_x(b); *y = box_visual_y(b) + b->content_dy;
    if (web_marquee_box(b)) { *x += b->node->marquee_x; *y += b->node->marquee_y; }
    if (box_element_scrollable(b)) { *x -= (float)b->node->scroll_x; *y -= (float)b->node->scroll_y; }
}
void layout_run_extent(const struct run *r, float *top, float *bottom) {
    wfont f = style_font(r->st);
    float ascent, descent;
    wf_metrics(&f, &ascent, &descent);
    *top = r->y - ascent; *bottom = r->y + descent;
}
float layout_run_x(const struct run *r, size_t at) {
    if (!r->s || r->n <= 0 || !at) return 0;
    if (at >= (size_t)r->n) return r->w; /* the width the line placed */
    return style_text_width(r->st, r->s, at);
}
size_t layout_text_boundary(const char *s, size_t n, float width, float dx, layout_measure_fn measure, const void *opaque) {
    if (!s || !n || !(dx > 0)) return 0;
    size_t at = 0;
    float x = 0;
    while (at < n) {
        size_t next = wf_grapheme_next(s, n, at);
        if (next <= at || next > n) next = n;
        float w = next == n && width >= 0 ? width - x : measure(opaque, s, at, next, x);
        if (dx <= x + w / 2) return at; /* a boundary at the middle is the earlier one */
        x += w; at = next;
    }
    return n;
}
static float run_measure(const void *opaque, const char *s, size_t at, size_t next, float x) {
    (void)x;
    return style_text_width(opaque, s + at, next - at);
}
size_t layout_run_boundary(const struct run *r, float dx) {
    if (!r->s || r->n <= 0) return 0;
    return layout_text_boundary(r->s, (size_t)r->n, r->w, dx, run_measure, r->st);
}
box_t *layout_inline_root(const box_t *b) {
    const box_t *p = b ? b->parent : NULL;
    while (p && p->kind == B_INLINE) p = p->parent;
    return p && p->inline_ctx ? (box_t *)p : NULL;
}
/* r is one of text box t's runs: its node and a part of t's current text. */
static bool text_run_of(const struct run *r, const box_t *t) {
    return !r->atomic && r->s && r->node == t->node && r->n > 0 &&
           r->s >= t->text && (size_t)(r->s - t->text) + (size_t)r->n <= t->len;
}
/* The first processed byte whose source byte is at or after byte (map is nondecreasing). */
static size_t source_lower_bound(const uint32_t *map, size_t length, size_t byte) {
    size_t low = 0, high = length;
    while (low < high) { size_t mid = low + (high - low) / 2; if (map[mid] < byte) low = mid + 1; else high = mid; }
    return low;
}
static bool text_checkpoint(const box_t *t, unsigned *work) {
    return (++*work & 63) || !t->node || !t->node->owner || web_native_checkpoint(t->node->owner);
}
bool layout_text_rects(node_t *n, uint32_t start, uint32_t end, bool visual, layout_rect_fn visit, void *opaque) {
    box_t *t = n && n->type == N_TEXT ? n->box : NULL;
    if (!t || t->kind != B_TEXT || t->node != n || !t->frag_end) return true;
    box_t *b = layout_inline_root(t);
    if (!b || !b->runs) return true;
    int first = t->frag_first, last = t->frag_end < b->nruns ? t->frag_end : b->nruns;
    if (first < 0 || first >= last) return true;
    if (end != UINT32_MAX && start > end) start = end;
    size_t ps = 0, pe = t->len;
    unsigned work = 0;
    if (start || end != UINT32_MAX) {
        arena_t scratch = {.chunk_size = 4096};
        jmp_buf trap; scratch.trap = &trap;
        if (setjmp(trap)) { ar_free(&scratch); return false; }
        uint32_t *map = box_text_source_map(&scratch, t);
        if (!map) { ar_free(&scratch); return true; }
        /* Inside a surrogate pair the whole pair counts (CSSOM View §9). */
        size_t from = doc_text_byte(n->text, n->textlen, start, false);
        size_t to = end == UINT32_MAX ? n->textlen : doc_text_byte(n->text, n->textlen, end, true);
        ps = source_lower_bound(map, t->len, from); pe = source_lower_bound(map, t->len, to);
        ar_free(&scratch);
        if (pe < ps) pe = ps;
        /* So does a whole grapheme cluster, the typographic character unit here. */
        size_t down = ps, up = pe;
        for (size_t at = 0; at < t->len && at < pe;) {
            if (!text_checkpoint(t, &work)) return false;
            size_t next = wf_grapheme_next(t->text, t->len, at);
            if (next <= at || next > t->len) next = t->len;
            if (at < ps && ps < next) down = at;
            if (at < pe && pe < next) up = next;
            at = next;
        }
        bool collapsed = ps == pe;
        ps = down; pe = collapsed ? down : up;
    }
    float ox, oy;
    layout_runs_origin(b, visual, &ox, &oy);
    bool drawn = false;
    int inside = -1, ending = -1, after = -1, before = -1;
    for (int i = first; i < last; i++) {
        if (!text_checkpoint(t, &work)) return false;
        const struct run *r = &b->runs[i];
        if (!text_run_of(r, t)) continue;
        size_t rs = (size_t)(r->s - t->text), re = rs + (size_t)r->n;
        if (ps < pe) {
            size_t a = ps > rs ? ps : rs, z = pe < re ? pe : re;
            if (a < z) {
                float x0 = layout_run_x(r, a - rs), x1 = layout_run_x(r, z - rs), top, bottom;
                layout_run_extent(r, &top, &bottom);
                drawn = true;
                if (!visit(opaque, b, (double)ox + r->x + x0, (double)oy + top, (double)x1 - x0, (double)bottom - top)) return false;
                continue;
            }
        }
        if (inside < 0 && rs <= ps && ps < re) inside = i;
        if (re == ps) ending = i;
        if (after < 0 && rs > ps) after = i;
        if (re < ps) before = i;
    }
    if (drawn) return true;
    /* A collapsed point, or a selection none of whose characters are rendered: a zero-width
       fragment where it lies, at a fragment's start before the end of the one ahead. */
    int at = inside >= 0 ? inside : ending >= 0 ? ending : after >= 0 ? after : before;
    if (at < 0) return true;
    const struct run *r = &b->runs[at];
    size_t rs = (size_t)(r->s - t->text);
    float x = at == after ? 0 : at == before ? r->w : layout_run_x(r, ps - rs), top, bottom;
    layout_run_extent(r, &top, &bottom);
    return visit(opaque, b, (double)ox + r->x + x, (double)oy + top, 0, (double)bottom - top);
}
bool layout_inline_rects(node_t *n, bool visual, layout_rect_fn visit, void *opaque) {
    box_t *ib = n ? n->box : NULL;
    if (!ib || ib->kind != B_INLINE || ib->node != n || !ib->frag_end) return true;
    box_t *a = layout_inline_root(ib);
    if (!a || !a->decos) return true;
    int last = ib->frag_end < a->ndecos ? ib->frag_end : a->ndecos;
    float ox, oy;
    layout_runs_origin(a, visual, &ox, &oy);
    const style_t *own = n->style;
    for (int i = ib->frag_first; i >= 0 && i < last; i++) {
        const struct deco *span = &a->decos[i];
        /* The element's ::before/::after inline boxes have decos with its node too. */
        if (span->node != n || (own && (span->st == own->before || span->st == own->after))) continue;
        if (!visit(opaque, a, (double)ox + span->x, (double)oy + span->y, span->w, span->h)) return false;
    }
    return true;
}
static uint32_t child_count(const node_t *n) {
    uint32_t count = 0;
    for (const node_t *c = n->first; c; c = c->next) count++;
    return count;
}
bool layout_run_position(const box_t *block, int run, float dx, node_t **node, uint32_t *offset) {
    if (!block || run < 0 || run >= block->nruns) return false;
    const struct run *r = &block->runs[run];
    node_t *n = r->node;
    if (r->atomic || !n) return false;
    if (n->type == N_ELEM) {
        /* Generated content is not in the DOM: the point is at its edge of the element. */
        const style_t *own = n->style;
        *node = n; *offset = own && r->st == own->after ? child_count(n) : 0;
        return true;
    }
    box_t *t = n->type == N_TEXT ? n->box : NULL;
    if (!t || t->kind != B_TEXT || !text_run_of(r, t)) return false;
    size_t at = (size_t)(r->s - t->text) + layout_run_boundary(r, dx);
    arena_t scratch = {.chunk_size = 4096};
    jmp_buf trap; scratch.trap = &trap;
    if (setjmp(trap)) { ar_free(&scratch); return false; }
    uint32_t *map = box_text_source_map(&scratch, t);
    bool ok = map != NULL;
    if (ok) { *node = n; *offset = doc_text_utf16(n->text, n->textlen, map[at]); }
    ar_free(&scratch);
    return ok;
}
static float axis_distance(float v, float low, float high) { return v < low ? low - v : v > high ? v - high : 0; }
/* The DOM position before (or, right of its middle, after) replaced element box a. */
static bool replaced_position(const box_t *a, float x, node_t **node, uint32_t *offset) {
    node_t *n = a->node;
    if (!n) return false;
    node_t *p = n->parent;
    if (!p) { *node = n; *offset = 0; return true; }
    uint32_t index = 0;
    for (const node_t *c = p->first; c && c != n; c = c->next) index++;
    float left = box_visual_x(a) - a->p[3] - a->b[3];
    *node = p; *offset = index + (x > left + (a->w + hext(a)) / 2);
    return true;
}
bool layout_position_at(const box_t *b, float x, float y, node_t **node, uint32_t *offset) {
    unsigned work = 0;
    web_doc *d = NULL;
    for (const box_t *p = b; p && !d; p = p->parent) if (p->node) d = p->node->owner;
#define POSITION_CHECKPOINT() do { if (!(++work & 63) && d && !web_native_checkpoint(d)) return false; } while (0)
    while (b) {
        POSITION_CHECKPOINT();
        if (b->kind == B_TEXT || b->kind == B_INLINE || b->kind == B_BR) {
            box_t *root = layout_inline_root(b);
            if (!root) break;
            b = root; continue;
        }
        if (b->kind == B_ATOMIC && b->atomic != AT_INLINE_BLOCK) return replaced_position(b, x, node, offset);
        if (b->inline_ctx && b->nruns) {
            float ox, oy;
            layout_runs_origin(b, true, &ox, &oy);
            int best = -1; float best_dy = INFINITY, best_dx = INFINITY;
            for (int i = 0; i < b->nruns; i++) {
                POSITION_CHECKPOINT();
                const struct run *r = &b->runs[i];
                float left, top, right, bottom;
                if (r->atomic) {
                    const box_t *a = r->atomic;
                    if (a->clamp_hidden) continue;
                    left = box_visual_x(a) - a->p[3] - a->b[3]; top = box_visual_y(a) - a->p[0] - a->b[0];
                    right = left + a->w + hext(a); bottom = top + a->h + vext(a);
                } else {
                    if (!r->node || !r->s) continue;
                    layout_run_extent(r, &top, &bottom);
                    left = ox + r->x; right = left + r->w; top += oy; bottom += oy;
                }
                float dy = axis_distance(y, top, bottom), dx = axis_distance(x, left, right);
                if (dy < best_dy || (dy == best_dy && dx < best_dx)) { best = i; best_dy = dy; best_dx = dx; }
            }
            if (best >= 0) {
                const struct run *r = &b->runs[best];
                if (r->atomic) { b = r->atomic; continue; }
                return layout_run_position(b, best, x - (ox + r->x), node, offset);
            }
        }
        const box_t *pick = NULL; float best_dy = INFINITY, best_dx = INFINITY;
        for (const box_t *c = b->first; c; c = c->next) {
            POSITION_CHECKPOINT();
            /* Text, inline and line-break boxes lie only in an inline formatting context,
               whose runs hold their fragments. */
            if (c->clamp_hidden || !c->st || c->st->display == D_NONE || c->kind == B_TEXT ||
                c->kind == B_BR || c->kind == B_INLINE) continue;
            float left = box_visual_x(c) - c->p[3] - c->b[3], top = box_visual_y(c) - c->p[0] - c->b[0];
            float dy = axis_distance(y, top, top + c->h + vext(c)), dx = axis_distance(x, left, left + c->w + hext(c));
            if (dy < best_dy || (dy == best_dy && dx < best_dx)) { pick = c; best_dy = dy; best_dx = dx; }
        }
        if (pick) { b = pick; continue; }
        break;
    }
#undef POSITION_CHECKPOINT
    /* No fragment below: the start of the nearest node's contents. */
    for (; b; b = b->parent) if (b->node) { *node = b->node; *offset = 0; return true; }
    return false;
}

/* ---------------------------------------------------------------- floats */
static void size_float(box_t *c, box_t *parent, float cbh) {
    float cw = parent->w;
    resolve_edges(c, cw);
    c->cb = parent;
    /* Floating establishes an independent formatting context, not an
       indefinite containing-block height. Preserve the reference supplied
       by block/inline layout, including deferred placement after a line. */
    if (c->kind == B_ATOMIC && c->atomic != AT_INLINE_BLOCK) size_atomic(c, cw, cbh);
    else if (c->kind == B_TABLE) {
        table_width(c, cw);
        layout_inner(c, NULL, 0, 0, cbh);
    } else {
        float w = spec_w(c, &c->st->width, cw);
        if (w < 0) w = stf_width(c, cw);
        c->w = clamp_w(c, w, cw);
        layout_inner(c, NULL, 0, 0, cbh);
    }
}

/* the BFC y where a sized float's margin box may go, given its order and clearance */
static float float_min_y(const box_t *c, const struct bfc *f, float y) {
    if (f) {
        if (c->st->clear) y = fmaxf_(y, bfc_clear(f, c->st->clear));
        y = fmaxf_(y, f->last_top);
    }
    return y;
}

static void position_float(box_t *c, float cw, struct bfc *f, float ox, float oy, float y_rel) {
    float mw = c->w + hext(c) + c->m[1] + c->m[3];
    float mh = c->h + vext(c) + c->m[0] + c->m[2];
    float y = float_min_y(c, f, oy + y_rel);
    float l = ox, r = ox + cw;
    for (int guard = 0; f && guard < 200; guard++) {
        bfc_space(f, y, mh, ox, ox + cw, &l, &r, false);
        if (r - l >= mw - 0.05f || (l <= ox && r >= ox + cw)) break;
        float nb = bfc_next(f, y);
        if (nb < 0) break;
        y = nb;
    }
    float x = c->st->float_ == FL_LEFT ? l : r - mw;
    if (f) bfc_add(f, x, y, mw, mh, c->st->float_, c, fmaxf_(0, len_resolve(&css_style_float_shape(c->st)->margin, cw)));
    c->x = x - ox + c->m[3] + c->b[3] + c->p[3];
    c->y = y - oy + c->m[0] + c->b[0] + c->p[0];
}

static void place_float(box_t *c, box_t *parent, struct bfc *f, float ox, float oy, float y_rel, float cbh) {
    size_float(c, parent, cbh);
    position_float(c, parent->w, f, ox, oy, y_rel);
}

/* CSS 2.1 9.5.1: a float met inside a line goes beside that line's content
   when its margin box fits in the line's remaining width at the line's top;
   the line box shortens around it. Otherwise it waits for the line to end. */
static bool float_fits_line(struct iline *L, box_t *c, float used) {
    box_t *b = L->blk;
    size_float(c, b, L->cbh);
    float y = L->oy + L->y;
    if (float_min_y(c, L->f, y) > y + 0.01f) return false;
    float mw = c->w + hext(c) + c->m[1] + c->m[3];
    float mh = c->h + vext(c) + c->m[0] + c->m[2];
    float l, r;
    bfc_space(L->f, y, fmaxf_(mh, L->strut_lh), L->ox, L->ox + b->w, &l, &r, false);
    if (L->first_line) l += len_resolve(&b->st->text_indent, b->w);
    return r - l - mw >= used - 0.01f;
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
static float top_chain(box_t *c, float cbw) {
    /* A collapsed margin group takes its greatest positive and most negative
       member. Native box trees are acyclic; a first-child chain needs neither
       a depth quota nor a C call frame for each descendant. */
    float positive = 0, negative = 0;
    for (; c; c = collapses_top(c) ? first_inflow(c) : NULL) {
        float m = len_auto(&c->st->margin[0]) ? 0 : len_resolve(&c->st->margin[0], cbw);
        if (m > positive) positive = m;
        if (m < negative) negative = m;
    }
    return positive + negative;
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
            if(c->node && c->node->tag==T_button) {
                /* HTML button layout's auto inline size is fit-content even
                   for a block-level flex/grid button. */
                w=clamp_w(c,stf_width(c,cw),cw);
            } else {
            /* auto width fills the line; auto margins only matter if max-width made it narrower */
            float avail = cw - c->m[1] - c->m[3] - ext;
            w = clamp_w(c, avail, cw);
            if (w >= avail - 0.01f) mla = mra = false;
            }
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
        if (!layout_checkpoint()) break;
        if (c->abspos) {
            c->static_cb = b;
            c->static_x = 0;
            c->static_y = cursor + pend;
            list_abs(c);
            continue;
        }
        if (c->floated) {
            place_float(c, b, f, ox, oy, cursor + (pend > 0 ? pend : 0), cbh);
            continue;
        }
        resolve_edges(c, cw);
        /* The first child's collapsed top margin is already hoisted to b's
           parent. Do not rescan its entire first-child chain for a value we
           discard; repeating that at every nested block is quadratic. */
        if (!(first && hoisted)) pend = mcollapse(pend, top_chain(c, cw));
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
                bfc_space(f, oy + y, bh, ox, ox + cw, &l, &r, false);
                float need = c->kind == B_ATOMIC || !len_auto(&c->st->width) ? c->w + hext(c) + c->m[1] + c->m[3] : 0;
                if (r - l >= need || need == 0) break;
                float nb = bfc_next(f, oy + y);
                if (nb < 0) break;
                y = nb - oy;
            }
            bfc_space(f, oy + y, bh, ox, ox + cw, &l, &r, false);
            l -= ox;
            r -= ox;
            if (l > 0 || r < cw) {
                if (len_auto(&c->st->width) && c->kind != B_ATOMIC && c->kind != B_TABLE &&
                    !(c->node&&c->node->tag==T_button)) {
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
        b->cached_end_margin = pend;
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
    pvec columns; /* flat-tree column declarations, borrowed immutable styles */
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
    pv_free(&g->columns);
}
static unsigned table_column_span(const node_t *n) {
    /* HTML col/colgroup span is a positive integer limited by HTML to 1000.
       CSS column boxes without an HTML span represent one column. */
    if(!n || (n->tag!=T_col && n->tag!=T_colgroup))return 1;
    const char *v=node_attr(n,"span");if(!v)return 1;
    while(is_space((unsigned char)*v))v++;if(*v=='+')v++;
    unsigned value=0;bool digit=false;
    for(;*v>='0'&&*v<='9';v++){digit=true;if(value<=1000)value=value*10+(unsigned)(*v-'0');}
    return !digit||!value?1:value>1000?1000:value;
}
static void table_columns(box_t *t,pvec *out) {
    if(!t->node)return;
    pvec children={0};doc_flat_children(t->node,&children);
    for(int i=0;i<children.n;i++) {
        node_t *n=children.v[i];if(!n->style)continue;
        if(n->style->display==D_TABLE_COLUMN) {
            for(unsigned j=0;j<table_column_span(n);j++)pv_push(out,(void *)n->style);
        }else if(n->style->display==D_TABLE_COLUMN_GROUP) {
            pvec cols={0};doc_flat_children(n,&cols);bool found=false;
            for(int j=0;j<cols.n;j++) {
                node_t *c=cols.v[j];if(!c->style || c->style->display!=D_TABLE_COLUMN)continue;
                found=true;
                const style_t *s=len_auto(&c->style->width)?n->style:c->style;
                for(unsigned k=0;k<table_column_span(c);k++)pv_push(out,(void *)s);
            }
            if(!found)for(unsigned j=0;j<table_column_span(n);j++)pv_push(out,(void *)n->style);
            pv_free(&cols);
        }
    }
    pv_free(&children);
}
static bool table_fixed(const box_t *t) {
    return t->st->table_layout && !len_auto(&t->st->width);
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
    table_columns(t,&g->columns);
    if(g->columns.n>maxc)maxc=g->columns.n;
    g->ncols = maxc;
    int n = maxc ? maxc : 1;
    g->mn = calloc((size_t)n, sizeof(float));
    g->mx = calloc((size_t)n, sizeof(float));
    g->pct = calloc((size_t)n, sizeof(float));
    g->w = calloc((size_t)n, sizeof(float));
    g->fixed = calloc((size_t)n, sizeof(bool));
    if(table_fixed(t))return; /* fixed tracks never inspect later cell content */
    for(int j=0;j<g->columns.n;j++) {
        const style_t *s=g->columns.v[j];const len_t *w=&s->width;
        if(w->kind==LK_LEN && !w->pct){g->mn[j]=g->mx[j]=fmaxf_(0,w->px);g->fixed[j]=true;}
        else if(w->kind==LK_LEN && w->pct>0 && !w->px)g->pct[j]=w->pct;
    }
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
            /* A nonpercentage cell width raises its minimum contribution;
               it cannot be squeezed to zero by a neighbouring 100% column.
               Retain intrinsic max-content rather than replacing it with W. */
            if(fixed>=0){mn=fmaxf_(mn,fixed);mx=fmaxf_(mx,mn);}
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
    float sw = spec_w(t, &t->st->width, cw);
    if(table_fixed(t) && sw>=0){t->w=clamp_w(t,sw,cw);return;}
    float mn, mx;
    table_intrinsic(t, &mn, &mx);
    float ext = hext(t);
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
static void table_fixed_tracks(box_t *t,struct tgrid *g) {
    float space=g->spacing*(float)(g->ncols+1),available=fmaxf_(0,t->w-space);
    for(int j=0;j<g->ncols;j++)g->w[j]=-1;
    for(int j=0;j<g->columns.n;j++) {
        const style_t *s=g->columns.v[j];
        if(!len_auto(&s->width)){float w=len_resolve(&s->width,t->w);if(w>=0)g->w[j]=w;}
    }
    /* Explicit columns precede first-row cells. A spanning first-row width
       supplies only columns still unspecified; later rows cannot resize them. */
    for(int i=0;i<g->ncells;i++) {
        struct tcell *cell=&g->cells[i];if(cell->row)continue;
        box_t *c=cell->b;resolve_edges(c,t->w);
        float w=spec_w(c,&c->st->width,t->w);if(w<0)continue;
        w+=hext(c);int span=c->colspan,start=cell->col;
        if(span>g->ncols-start)span=g->ncols-start;
        float known=g->spacing*(float)(span-1);int missing=0;
        for(int j=start;j<start+span;j++)if(g->w[j]<0)missing++;else known+=g->w[j];
        if(missing){float each=fmaxf_(0,w-known)/(float)missing;
            for(int j=start;j<start+span;j++)if(g->w[j]<0)g->w[j]=each;}
    }
    float used=0;int missing=0;
    for(int j=0;j<g->ncols;j++)if(g->w[j]<0)missing++;else used+=g->w[j];
    float extra=fmaxf_(0,available-used);
    for(int j=0;j<g->ncols;j++) {
        if(g->w[j]<0)g->w[j]=missing?extra/(float)missing:0;
        else if(!missing && g->ncols)g->w[j]+=extra/(float)g->ncols;
    }
}

/* CSS Tables 3 3.9.3: interpolate consecutive intrinsic sizing guesses.
   A percent track is preferred, not permission to consume the whole table
   in addition to the other tracks' required minimum widths. */
static bool table_auto_tracks(struct tgrid *g,float available) {
    double sums[4]={0},percent=0;
    for(int j=0;j<g->ncols;j++) {
        if(!(j&63)&&!layout_checkpoint())return false;
        g->pct[j]=fmaxf_(0,fminf_(g->pct[j],(float)(100-percent)));
        percent+=g->pct[j];
        double mn=g->mn[j],preferred=fmax(mn,g->pct[j]*(double)available/100);
        sums[0]+=mn;sums[1]+=g->pct[j]>0?preferred:mn;
        sums[2]+=g->pct[j]>0?preferred:g->fixed[j]?g->mx[j]:mn;
        sums[3]+=g->pct[j]>0?preferred:g->mx[j];
    }
    int lo=0,hi=0;double weight=0;
    if(available>=sums[3])lo=hi=3;
    else if(available>sums[0]) {
        hi=1;while(hi<3 && available>sums[hi])hi++;
        lo=hi-1;
        if(sums[hi]>sums[lo])weight=(available-sums[lo])/(sums[hi]-sums[lo]);
    }
    double excess=fmax(0,(double)available-sums[3]),basis=0;int growth=0;
    if(excess>0) {
        /* Ordered excess rules: non-constrained nonpercent, then constrained
           nonpercent, then percentage, then truly empty tracks. */
        for(int j=0;j<g->ncols;j++)if(!g->fixed[j]&&!g->pct[j]&&g->mx[j]>0)basis+=g->mx[j];
        if(basis>0)growth=1;
        else {
            for(int j=0;j<g->ncols;j++)if(!g->fixed[j]&&!g->pct[j])basis++;
            if(basis>0)growth=2;
            else {
                for(int j=0;j<g->ncols;j++)if(g->fixed[j]&&!g->pct[j]&&g->mx[j]>0)basis+=g->mx[j];
                if(basis>0)growth=3;
                else if(percent>0){growth=4;basis=percent;}
                else {growth=5;basis=g->ncols;}
            }
        }
    }
    for(int j=0;j<g->ncols;j++) {
        if(!(j&63)&&!layout_checkpoint())return false;
        double mn=g->mn[j],preferred=fmax(mn,g->pct[j]*(double)available/100);
        double guess[4]={mn,g->pct[j]>0?preferred:mn,
            g->pct[j]>0?preferred:g->fixed[j]?g->mx[j]:mn,
            g->pct[j]>0?preferred:g->mx[j]};
        double w=guess[lo]+(guess[hi]-guess[lo])*weight,share=0;
        if(growth==1 && !g->fixed[j]&&!g->pct[j])share=g->mx[j];
        else if(growth==2 && !g->fixed[j]&&!g->pct[j])share=1;
        else if(growth==3 && g->fixed[j]&&!g->pct[j])share=g->mx[j];
        else if(growth==4)share=g->pct[j];
        else if(growth==5)share=1;
        if(basis>0)w+=excess*share/basis;
        g->w[j]=(float)w;
    }
    return true;
}
static void layout_table(box_t *t, float cbh) {
    struct tgrid g;
    grid_build(t, &g);
    float sp = g.spacing;
    int nc = g.ncols;
    float avail = t->w - sp * (float)(nc + 1);
    if (avail < 0) avail = 0;
    /* column widths */
    if(table_fixed(t))table_fixed_tracks(t,&g);
    else if(!table_auto_tracks(&g,avail)){grid_free(&g);return;}
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

/* Once flex/grid widths are resolved, ordinary sibling formatting contexts
 * own disjoint boxes, line arrays and node anchors. Ancestor placement and
 * cross-axis reductions wait for every worker. Floats, absolute descendants,
 * tables, media and nested flex/grid retain the ordered dependency path. */
static bool child_layout_safe(box_t *b) {
    if ((b->kind==B_INLINE && b->node && b->node->box!=b) ||
        b->abspos || b->floated || (b->kind != B_BLOCK && b->kind != B_INLINE &&
        b->kind != B_TEXT && b->kind != B_BR) ||
        (b->node && b->node->tag == T_frameset)) return false;
    for (box_t *c=b->first;c;c=c->next) if (!child_layout_safe(c)) return false;
    return true;
}
static void child_layout_traps_clear(box_t *b) {
    b->layout_mem.trap=NULL;
    for(box_t *c=b->first;c;c=c->next) child_layout_traps_clear(c);
}
struct child_layout_job {
    box_t **boxes;
    float cbh;
    struct layout_context context;
    volatile uint32_t failed;
};
static void child_layout_run(size_t i,void *context) {
    struct child_layout_job *j=context;
    struct layout_context *local=calloc(1,sizeof *local);
    if(!local){__atomic_store_n(&j->failed,3,__ATOMIC_RELEASE);return;}
    *local=j->context;local->pure=true;local->text_cache=NULL;
    memset(&local->scratch,0,sizeof local->scratch);
    jmp_buf trap;local->scratch.trap=&trap;
    void *previous=thread_local_get(1);thread_local_set(1,local);
    int failed=setjmp(trap);
    if(!failed) layout_inner(j->boxes[i],NULL,0,0,j->cbh);
    else __atomic_store_n(&j->failed,(uint32_t)failed,__ATOMIC_RELEASE);
    child_layout_traps_clear(j->boxes[i]);
    ar_free(&local->scratch);thread_local_set(1,previous);free(local);
}
static void child_layout_service(void *context) { web_native_checkpoint(context); }
static bool layout_children_parallel(box_t **boxes,int n,float cbh) {
    if(n<8 || layout_current()->pure)return false;
    for(int i=0;i<n;i++)if(!child_layout_safe(boxes[i]))return false;
    struct child_layout_job j={.boxes=boxes,.cbh=cbh,.context=*layout_current()};
    parallel_for_stage(PARALLEL_LAYOUT,(size_t)n,child_layout_run,&j,child_layout_service,D);
    /* Allocation failure may only unwind the supervisor after all helpers
     * stopped borrowing its DOM; never longjmp across thread stacks. */
    uint32_t failed=__atomic_load_n(&j.failed,__ATOMIC_ACQUIRE);
    if(failed){arena_t *a=layout_scratch();if(a->trap)longjmp(*a->trap,(int)failed);abort();}
    return true;
}

/* ---------------------------------------------------------------- gap decorations (css-gaps-1 §3) */
/* Grid and flex layout describe their gaps here, and one algorithm turns them into the segments
   paint.c draws (box_t's gap_decorations): §3.1.2's segments, §3.4's visibility, §3.3's insets. A
   gap runs along its axis (0: a column gap, 1: a row gap) from its start bound to its end bound,
   between lo and hi across it, in the container's content box (coordinates grow rightwards and
   downwards). Its cells lie between its bounds: bound k ends cell k - 1 at end_at and starts cell k
   at start_at, so a gap of n cells has n + 1 bounds, the first and the last at its edges. */
struct gd_bound {
    float end_at, start_at;
    float width;            /* the crossing gap width (§3.3): the junction's size, 0 at a container edge */
    uint32_t cross, ncross; /* the gaps abutting or crossing it here: gd.cross[cross, cross + ncross) */
    bool flanked;           /* items on opposing sides span every crossing gap here (§3.1.2) */
};
enum { GD_CROSSED = 1, GD_BEFORE = 2, GD_AFTER = 4 }; /* a cell: an item crosses the gap; the areas beside hold items */
struct gd_gap {
    float lo, hi;
    size_t cell, bound, ncell; /* gd.cell[cell, cell + ncell), gd.bound[bound, bound + ncell + 1) */
    size_t run, nrun;          /* its runs of painted cells: gd.run[run, run + nrun) */
    uint32_t order;            /* its place among its axis' gaps, the index §4.6 assigns values by */
    uint8_t axis;
};
struct gd_run { size_t k0, k1; }; /* cells k0 to k1: a segment before its insets */
struct gd {
    struct gd_gap *gap;
    uint8_t *cell;
    struct gd_bound *bound;
    uint32_t *cross;
    struct gd_run *run;
    size_t ngap, ncell, nbound, ncross, nrun;
    size_t capgap, capcell, capbound, capcross, caprun;
    uint32_t count[2];                /* the gaps of each axis */
    uint8_t rule_break[2], visibility[2]; /* the used values, which depend on the container's type */
};
static void gd_free(struct gd *g) {
    free(g->gap); free(g->cell); free(g->bound); free(g->cross); free(g->run);
}
static bool gd_room(void **p, size_t *cap, size_t need, size_t size) {
    if (need <= *cap) return true;
    size_t c = *cap ? *cap : 16;
    while (c < need) {
        if (c > SIZE_MAX / 2 / size) return false;
        c *= 2;
    }
    void *q = realloc(*p, c * size);
    if (!q) return false;
    *p = q;
    *cap = c;
    return true;
}
#define GD_ROOM(g, field, cap, need) gd_room((void **)&(g)->field, &(g)->cap, (need), sizeof *(g)->field)
/* a gap of ncell (>= 1) cells, whose zeroed cells and bounds the caller fills before adding the next
   gap; NULL out of memory */
static struct gd_gap *gd_gap_add(struct gd *g, uint8_t axis, float lo, float hi, size_t ncell) {
    if (ncell >= SIZE_MAX / 2 || !GD_ROOM(g, gap, capgap, g->ngap + 1) || !GD_ROOM(g, cell, capcell, g->ncell + ncell) ||
        !GD_ROOM(g, bound, capbound, g->nbound + ncell + 1)) return NULL;
    struct gd_gap *x = &g->gap[g->ngap++];
    *x = (struct gd_gap){.lo = lo, .hi = hi, .cell = g->ncell, .bound = g->nbound, .ncell = ncell,
                         .order = g->count[axis]++, .axis = axis};
    memset(g->cell + g->ncell, 0, ncell);
    memset(g->bound + g->nbound, 0, (ncell + 1) * sizeof *g->bound);
    g->ncell += ncell;
    g->nbound += ncell + 1;
    return x;
}
/* gap y crosses or abuts bound b of the gap being filled (each bound's gaps added together) */
static bool gd_cross(struct gd *g, struct gd_bound *b, size_t y) {
    if (!GD_ROOM(g, cross, capcross, g->ncross + 1)) return false;
    if (!b->ncross) b->cross = (uint32_t)g->ncross;
    g->cross[g->ncross++] = (uint32_t)y;
    b->ncross++;
    return true;
}
static bool gd_shown(uint8_t cell, uint8_t visibility) {
    if (visibility == CSS_RULE_ITEMS_AROUND) return cell & (GD_BEFORE | GD_AFTER);
    if (visibility == CSS_RULE_ITEMS_BETWEEN) return (cell & (GD_BEFORE | GD_AFTER)) == (GD_BEFORE | GD_AFTER);
    return true;
}
/* §3.1.2 "determine gap decoration segments" for every gap, as runs of cells. none: one segment
   from one end of the gap to the other; normal: each stretch no item crosses; intersection: also
   broken at each junction not flanked by spanning items. A cell §3.4 does not paint ends a run. */
static bool gd_runs(struct gd *g) {
    for (size_t i = 0; i < g->ngap; i++) {
        struct gd_gap *x = &g->gap[i];
        const uint8_t *cell = g->cell + x->cell;
        const struct gd_bound *bound = g->bound + x->bound;
        uint8_t brk = g->rule_break[x->axis], vis = g->visibility[x->axis];
        x->run = g->nrun;
        x->nrun = 0;
        for (size_t k = 0; k < x->ncell; k++) {
            if (!(k & 1023) && !layout_checkpoint()) return false;
            if (!gd_shown(cell[k], vis) || (brk != CSS_RULE_BREAK_NONE && (cell[k] & GD_CROSSED))) continue;
            size_t k1 = k;
            while (k1 + 1 < x->ncell && gd_shown(cell[k1 + 1], vis) &&
                   (brk == CSS_RULE_BREAK_NONE ||
                    (!(cell[k1 + 1] & GD_CROSSED) && (brk != CSS_RULE_BREAK_INTERSECTION || bound[k1 + 1].flanked))))
                k1++;
            if (!GD_ROOM(g, run, caprun, g->nrun + 1)) return false;
            g->run[g->nrun++] = (struct gd_run){k, k1};
            x->nrun++;
            k = k1;
        }
    }
    return true;
}
/* whether a segment of gap y, before its insets, reaches into [lo, hi] along y */
static bool gd_present(const struct gd *g, size_t y, float lo, float hi) {
    const struct gd_gap *x = &g->gap[y];
    const struct gd_bound *bound = g->bound + x->bound;
    size_t a = 0, b = x->nrun;
    while (a < b) { /* the first whose end reaches lo */
        size_t m = a + (b - a) / 2;
        if (bound[g->run[x->run + m].k1 + 1].end_at < lo) a = m + 1;
        else b = m;
    }
    return a < x->nrun && bound[g->run[x->run + a].k0].start_at <= hi;
}
/* §3.3: how far the endpoint a run makes at bound k of gap x moves into the run (start: the run's
   start). It is a junction segment endpoint when another segment is present at the junction: the
   gap's own next one (neighbour) or a crossing gap's. overlap-join reaches into the junction by
   half its size and half the crossing gap's rule width. */
static float gd_inset(const struct gd *g, const struct css_gap_rules *rules, const struct gd_gap *x, size_t k, bool start,
                      bool neighbour) {
    const struct gd_bound *b = &g->bound[x->bound + k];
    bool junction = neighbour;
    for (uint32_t i = 0; i < b->ncross && !junction; i++) junction = gd_present(g, g->cross[b->cross + i], x->lo, x->hi);
    const struct css_rule_inset *in =
        &rules->axis[x->axis].inset[junction ? start ? CSS_RULE_JUNCTION_START : CSS_RULE_JUNCTION_END :
                                    start ? CSS_RULE_CAP_START : CSS_RULE_CAP_END];
    if (!in->join) return len_resolve(&in->length, b->width);
    if (!junction) return 0;
    const struct css_rule_list *widths = &rules->axis[!x->axis].list[CSS_GAP_WIDTH];
    float rule = 0;
    for (uint32_t i = 0; i < b->ncross; i++) {
        const struct gd_gap *y = &g->gap[g->cross[b->cross + i]];
        rule = fmaxf_(rule, css_rule_value_at(widths, y->order, g->count[y->axis]).width);
    }
    return -(b->width / 2 + rule / 2);
}
/* the segments of the gaps g holds, into b's layout memory; g is freed */
static void gd_finish(box_t *b, struct gd *g) {
    const struct css_gap_rules *rules = css_style_gap_rules(b->st);
    struct gap_segment *seg = NULL;
    size_t n = 0;
    bool ok = gd_runs(g) && (!g->nrun || (seg = malloc(g->nrun * sizeof *seg)));
    for (size_t i = 0; ok && i < g->ngap; i++) {
        const struct gd_gap *x = &g->gap[i];
        const struct gd_bound *bound = g->bound + x->bound;
        size_t first = n;
        for (size_t t = 0; t < x->nrun; t++) {
            const struct gd_run *r = &g->run[x->run + t];
            bool before = t > 0 && g->run[x->run + t - 1].k1 + 1 == r->k0;
            bool after = t + 1 < x->nrun && g->run[x->run + t + 1].k0 == r->k1 + 1;
            /* positive start insets move towards the end, positive end insets towards the start */
            float s = bound[r->k0].start_at + gd_inset(g, rules, x, r->k0, true, before);
            float e = bound[r->k1 + 1].end_at - gd_inset(g, rules, x, r->k1 + 1, false, after);
            if (!(e > s)) continue;
            if (n > first && s <= seg[n - 1].e) { /* overlapping segments of one gap paint as one */
                if (s < seg[n - 1].s) seg[n - 1].s = s;
                if (e > seg[n - 1].e) seg[n - 1].e = e;
                continue;
            }
            seg[n++] = (struct gap_segment){s, e, (x->lo + x->hi) / 2, x->order, x->axis};
        }
    }
    uint32_t count[2] = {g->count[0], g->count[1]};
    gd_free(g);
    if (!ok) {
        free(seg);
        if (!__atomic_load_n(&D->native_cancelled, __ATOMIC_ACQUIRE)) ar_alloc(layout_scratch(), SIZE_MAX);
        return;
    }
    /* the layout memory's trap would leave seg behind */
    jmp_buf trap, *previous = b->layout_mem.trap;
    b->layout_mem.trap = &trap;
    if (setjmp(trap)) {
        b->layout_mem.trap = previous;
        free(seg);
        ar_alloc(layout_scratch(), SIZE_MAX);
        return;
    }
    struct gap_decorations *out = ar_alloc(&b->layout_mem, sizeof *out);
    struct gap_segment *kept = n ? ar_alloc(&b->layout_mem, n * sizeof *kept) : NULL;
    b->layout_mem.trap = previous;
    if (n) memcpy(kept, seg, n * sizeof *kept);
    free(seg);
    *out = (struct gap_decorations){kept, (uint32_t)n, {count[0], count[1]}};
    b->gap_decorations = out;
}

/* ---------------------------------------------------------------- flexbox */
struct fitem {
    box_t *b;
    float base, hypo, mn, mx, ext, size, cross;
    int align;
    bool frozen;
    double violation;
};
struct fline {
    int first, end;
    float cross, ascent, descent;
    float origin; /* where its cross extent starts (flex_cross_lines) */
};

/* CSS Flexbox 9.7: clamped items leave the flexible pool, and their unused
   share is redistributed. A one-pass clamp loses that share and can overflow
   even when the remaining item has ample shrink capacity. */
static bool flex_resolve_row(struct fitem *it,int first,int end,float available,float gap) {
    double gaps=(double)gap*(end-first-1),hypothetical=gaps;
    for(int i=first;i<end;i++)hypothetical+=it[i].hypo+it[i].ext;
    bool growing=hypothetical<available;
    size_t remaining=0;
    for(int i=first;i<end;i++) {
        double factor=growing?it[i].b->st->flex_grow:it[i].b->st->flex_shrink;
        it[i].frozen=factor==0 || (growing?it[i].base>it[i].hypo:it[i].base<it[i].hypo);
        it[i].size=it[i].frozen?it[i].hypo:it[i].base;
        if(!it[i].frozen)remaining++;
    }
    double initial=available-gaps;
    for(int i=first;i<end;i++)initial-=it[i].size+it[i].ext;
    while(remaining) {
        if(!layout_checkpoint())return false;
        double free_space=available-gaps,factors=0,scaled=0;
        for(int i=first;i<end;i++) {
            free_space-=(it[i].frozen?it[i].size:it[i].base)+it[i].ext;
            if(!it[i].frozen) {
                factors+=growing?it[i].b->st->flex_grow:it[i].b->st->flex_shrink;
                scaled+=(double)it[i].b->st->flex_shrink*it[i].base;
            }
        }
        if(factors<1 && fabs(initial*factors)<fabs(free_space))free_space=initial*factors;
        double violation=0;
        for(int i=first;i<end;i++)if(!it[i].frozen) {
            if(!(i&63)&&!layout_checkpoint())return false;
            double target=it[i].base;
            if(growing&&factors>0)target+=free_space*it[i].b->st->flex_grow/factors;
            else if(!growing&&scaled>0)target-=fabs(free_space)*(double)it[i].b->st->flex_shrink*it[i].base/scaled;
            double clamped=fmin(fmax(fmax(target,0),it[i].mn),it[i].mx);
            it[i].size=(float)clamped;it[i].violation=clamped-target;violation+=it[i].violation;
        }
        size_t frozen=0;
        for(int i=first;i<end;i++)if(!it[i].frozen &&
            (violation==0 || (violation>0?it[i].violation>0:it[i].violation<0))) {
            it[i].frozen=true;frozen++;
        }
        /* IEEE cancellation may leave a tiny sum although individual
           violations are all zero; the already-clamped targets are final. */
        if(!frozen)for(int i=first;i<end;i++)if(!it[i].frozen){it[i].frozen=true;frozen++;}
        remaining-=frozen;
    }
    return true;
}

static bool stretches_height(const box_t *b, int align) {
    return align == AI_STRETCH && len_auto(&b->st->height) &&
           !len_auto(&b->st->margin[0]) && !len_auto(&b->st->margin[2]) &&
           !(b->kind == B_ATOMIC && b->atomic != AT_INLINE_BLOCK);
}

static void layout_stretched_height(box_t *b, float usedh, float cbh) {
    /* The final flex line / grid area, not an intrinsic measurement, supplies
       this definite content height. Keep CSS height:auto and never feed the
       resulting percentage content back into line or track sizing. Tables
       retain their separate row-height model; this is not a table-cell fix. */
    /* Without definite-height consumers an unchanged stretch is a no-op.
       Re-entering every nested auto-height row otherwise alternates -1/usedh
       cache inputs and doubles the subtree work at every depth. */
    if (usedh == b->h && !b->stretch_height_dependent) return;
    if (b->kind == B_TABLE) {
        if (usedh > b->h) b->h = usedh;
    } else if (b->first) layout_inner_used(b, NULL, 0, 0, cbh, usedh);
    else b->h = usedh;
}

/* Sizes and main-axis placement are fixed before this pass. Reverse wrapping
   swaps the cross edges; it never reverses order or mirrors rendered content. */
static void flex_cross_lines(box_t *b, struct fitem *it, struct fline *lines,
                             int count, bool column, bool single, float defh, float gap) {
    bool flip = b->st->flex_wrap == FW_WRAP_REVERSE;
    float sum = gap * (float)(count > 0 ? count - 1 : 0);
    for (int l = 0; l < count; l++) sum += lines[l].cross;
    float available = column ? b->w : defh >= 0 ? defh : sum;
    float left = available - sum, start = 0, between = gap;
    if (single && count) lines[0].cross = available;
    else if (count) {
        switch (b->st->align_content) {
        case AC_NORMAL: case AC_STRETCH:
            if (left > 0) for (int l = 0; l < count; l++) lines[l].cross += left / (float)count;
            break;
        case AC_FLEX_END: start = left; break;
        case AC_START: start = flip ? left : 0; break;
        case AC_END: start = flip ? 0 : left; break;
        case AC_CENTER: start = left / 2; break;
        case AC_BETWEEN: if (left > 0 && count > 1) between += left / (float)(count - 1); break;
        case AC_AROUND:
            if (left > 0) start = left / (float)count / 2, between += left / (float)count;
            break;
        case AC_EVENLY:
            if (left > 0) start = left / ((float)count + 1), between += left / ((float)count + 1);
            break;
        }
    }
    float cursor = start, first_bl = -1;
    for (int l = 0; l < count; l++) {
        struct fline *line = &lines[l];
        float origin = flip ? available - cursor - line->cross : cursor;
        line->origin = origin;
        for (int i = line->first; i < line->end; i++) {
            if (!layout_checkpoint()) break;
            layout_media_checkpoint();
            box_t *c = it[i].b;
            int lo = column ? 3 : 0, hi = column ? 1 : 2;
            bool auto_lo = len_auto(&c->st->margin[lo]), auto_hi = len_auto(&c->st->margin[hi]);
            if (!auto_lo && !auto_hi && it[i].align == AI_STRETCH) {
                if (!column && stretches_height(c, it[i].align)) {
                    float want = clamp_h(c, line->cross - vext(c) - c->m[0] - c->m[2], defh);
                    layout_stretched_height(c, want, defh);
                } else if (column && len_auto(&c->st->width) && c->kind != B_TABLE &&
                           !(c->kind == B_ATOMIC && c->atomic != AT_INLINE_BLOCK)) {
                    float want = clamp_w(c, line->cross - hext(c) - c->m[1] - c->m[3], b->w);
                    if (want != c->w) {
                        c->w = want;
                        /* A wrapped column has a definite main reference. Its
                           final cross stretch cannot change its flexed height. */
                        if (defh >= 0) layout_inner_used(c, NULL, 0, 0, defh, c->h);
                        else layout_inner(c, NULL, 0, 0, defh);
                    }
                }
            }
            float outer = column ? c->w + hext(c) + c->m[1] + c->m[3] :
                                   c->h + vext(c) + c->m[0] + c->m[2];
            float off = 0, extra = line->cross - outer;
            if (auto_lo || auto_hi) {
                /* Auto margins refer to physical sides, not reversed edges.
                   Oversized items overflow the physical block/inline end. */
                if (extra > 0 && auto_lo) off = auto_hi ? extra / 2 : extra;
            } else if (it[i].align == AI_CENTER) off = extra / 2;
            else if (it[i].align == AI_END) off = flip ? 0 : extra;
            else if (!column && it[i].align == AI_BASELINE && c->baseline >= 0) {
                float baseline = c->m[0] + c->b[0] + c->p[0] + c->baseline;
                off = (flip ? line->cross - line->descent : line->ascent) - baseline;
            } else off = flip ? extra : 0;
            float coordinate = origin + off + c->m[lo] + c->b[lo] + c->p[lo];
            if (column) c->x = coordinate;
            else c->y = coordinate;
            if (first_bl < 0 && c->baseline >= 0) first_bl = c->y + c->baseline;
        }
        cursor += line->cross + between;
    }
    if (!column) b->h = available;
    b->baseline = b->last_baseline = first_bl;
}

/* css-gaps-1 §2 in a flex container: a gap between adjacent items of a line runs across the line,
   a gap between adjacent lines from one edge of the container to the other. Values are assigned
   in placement order, the items' gaps on through the lines (§4.6); normal breaks behave as none,
   and the visibility properties do not apply. */
struct gd_span { float lo, hi; uint32_t gap; };
static int gd_span_order(const void *a, const void *b) {
    float x = ((const struct gd_span *)a)->lo, y = ((const struct gd_span *)b)->lo;
    return x < y ? -1 : x > y;
}
/* an item's margin box along the main axis */
static void flex_main_range(const box_t *c, bool column, float *lo, float *hi) {
    *lo = column ? c->y - c->p[0] - c->b[0] - c->m[0] : c->x - c->p[3] - c->b[3] - c->m[3];
    *hi = *lo + (column ? c->h + vext(c) + c->m[0] + c->m[2] : c->w + hext(c) + c->m[1] + c->m[3]);
}
static size_t flex_line_gaps(const struct fline *line) {
    return line->end - line->first > 1 ? (size_t)(line->end - line->first - 1) : 0;
}
static void flex_gap_decorations(box_t *b, const struct fitem *it, const struct fline *lines, int count, bool column,
                                 bool reverse) {
    const struct css_gap_rules *rules = b->st->gap_rules;
    if (!rules || count <= 0) return;
    struct gd g = {0};
    for (int a = 0; a < 2; a++) {
        g.rule_break[a] = rules->axis[a].rule_break == CSS_RULE_BREAK_NORMAL ? CSS_RULE_BREAK_NONE : rules->axis[a].rule_break;
        g.visibility[a] = CSS_RULE_ITEMS_ALL;
    }
    bool flip = b->st->flex_wrap == FW_WRAP_REVERSE;
    uint8_t item_axis = column ? 1 : 0;
    float main_size = column ? b->h : b->w;
    size_t items_gaps = 0;
    for (int l = 0; l < count; l++) items_gaps += flex_line_gaps(&lines[l]);
    /* the gutters between lines, across: line l + 1 follows line l, before it when wrap-reverse */
    float (*gutter)[2] = count > 1 ? malloc((size_t)(count - 1) * sizeof *gutter) : NULL;
    /* the items' gaps along the main axis (spans, by gap) and two lines' of them sorted (pair) */
    struct gd_span *spans = items_gaps ? malloc(items_gaps * sizeof *spans) : NULL;
    struct gd_span *pair = items_gaps ? malloc(items_gaps * sizeof *pair) : NULL;
    bool ok = (count < 2 || gutter) && (!items_gaps || (spans && pair));
    for (int l = 0; ok && l + 1 < count; l++) {
        float e0 = lines[l].origin, e1 = e0 + lines[l].cross, f0 = lines[l + 1].origin, f1 = f0 + lines[l + 1].cross;
        float lo = flip ? f1 : e1, hi = flip ? e0 : f0;
        if (hi < lo) lo = hi = (lo + hi) / 2;
        gutter[l][0] = lo;
        gutter[l][1] = hi;
    }
    /* the gaps between items, line by line in placement order (gap ids 0 .. items_gaps - 1) */
    for (int l = 0; ok && l < count; l++) {
        const struct fline *line = &lines[l];
        /* the gaps between lines on the line's near (top or left) and far side */
        int near = flip ? (l + 1 < count ? l : -1) : l - 1, far = flip ? l - 1 : (l + 1 < count ? l : -1);
        for (int i = line->first; ok && i + 1 < line->end; i++) {
            if (!(i & 1023) && !layout_checkpoint()) { ok = false; break; }
            float a0, a1, b0, b1;
            flex_main_range(it[i].b, column, &a0, &a1);
            flex_main_range(it[i + 1].b, column, &b0, &b1);
            float lo = reverse ? b1 : a1, hi = reverse ? a0 : b0;
            if (hi < lo) lo = hi = (lo + hi) / 2;
            struct gd_gap *x = gd_gap_add(&g, item_axis, lo, hi, 1);
            if (!x) { ok = false; break; }
            g.cell[x->cell] = GD_BEFORE | GD_AFTER;
            struct gd_bound *bound = g.bound + x->bound;
            bound[0].start_at = line->origin;
            bound[1].end_at = line->origin + line->cross;
            /* where a gap between lines abuts it, its bound is a junction as wide as that gap */
            if (near >= 0) {
                bound[0].width = gutter[near][1] - gutter[near][0];
                ok = gd_cross(&g, &bound[0], items_gaps + (size_t)near);
            }
            if (ok && far >= 0) {
                bound[1].width = gutter[far][1] - gutter[far][0];
                ok = gd_cross(&g, &bound[1], items_gaps + (size_t)far);
            }
            spans[g.ngap - 1] = (struct gd_span){lo, hi, (uint32_t)(g.ngap - 1)};
        }
    }
    /* the gaps between lines (ids from items_gaps): the items' gaps of the lines on either side abut
       it, those that overlap or touch making one junction */
    size_t first = 0;
    for (int l = 0; ok && l + 1 < count; l++) {
        size_t here = flex_line_gaps(&lines[l]), ns = here + flex_line_gaps(&lines[l + 1]);
        if (ns) {
            memcpy(pair, spans + first, ns * sizeof *pair);
            qsort(pair, ns, sizeof *pair, gd_span_order);
        }
        first += here;
        size_t junctions = 0;
        for (size_t i = 0; i < ns; junctions++) {
            float hi = pair[i].hi;
            for (i++; i < ns && pair[i].lo <= hi; i++) hi = fmaxf_(hi, pair[i].hi);
        }
        struct gd_gap *x = gd_gap_add(&g, (uint8_t)!item_axis, gutter[l][0], gutter[l][1], junctions + 1);
        if (!x) { ok = false; break; }
        memset(g.cell + x->cell, GD_BEFORE | GD_AFTER, junctions + 1);
        struct gd_bound *bound = g.bound + x->bound;
        bound[0].start_at = 0;
        bound[junctions + 1].end_at = main_size;
        for (size_t i = 0, j = 1; ok && i < ns; j++) {
            struct gd_bound *at = &bound[j];
            float hi = pair[i].hi;
            at->end_at = pair[i].lo;
            ok = gd_cross(&g, at, pair[i].gap);
            for (i++; ok && i < ns && pair[i].lo <= hi; i++) {
                hi = fmaxf_(hi, pair[i].hi);
                ok = gd_cross(&g, at, pair[i].gap);
            }
            at->start_at = hi;
            at->width = hi - at->end_at;
        }
    }
    free(gutter);
    free(spans);
    free(pair);
    if (!ok) {
        gd_free(&g);
        if (!__atomic_load_n(&D->native_cancelled, __ATOMIC_ACQUIRE)) ar_alloc(layout_scratch(), SIZE_MAX);
        return;
    }
    gd_finish(b, &g);
}

static void layout_flex(box_t *b, float defh) {
    const style_t *st = b->st;
    bool column = st->flex_direction == FD_COLUMN || st->flex_direction == FD_COLUMN_REVERSE;
    bool reverse = st->flex_direction == FD_ROW_REVERSE || st->flex_direction == FD_COLUMN_REVERSE;
    float cw = b->w;
    int n = 0;
    for (box_t *c = b->first; c; c = c->next) {
        if (n == INT_MAX) { ar_alloc(layout_scratch(), SIZE_MAX); return; }
        n++;
    }
    struct fitem *it = calloc((size_t)n + 1, sizeof *it);
    struct fline *lines = calloc((size_t)n + 1, sizeof *lines);
    if (!it || !lines) {
        free(it); free(lines);
        ar_alloc(layout_scratch(), SIZE_MAX);
        return;
    }
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
            if (!layout_checkpoint()) { free(lines); free(it); return; }
            struct fitem t = it[j];
            it[j] = it[j - 1];
            it[j - 1] = t;
        }
    float gap_main = column ? st->gap_row : st->gap_col, gap_cross = column ? st->gap_col : st->gap_row;
    int line_count = 0;
    bool single = st->flex_wrap == FW_NOWRAP;

    if (column) {
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
            } else if (w < 0) w = single && al == AI_STRETCH &&
                !len_auto(&c->st->margin[1]) && !len_auto(&c->st->margin[3]) ?
                cw - hext(c) - c->m[1] - c->m[3] : stf_width(c, cw);
            c->w = clamp_w(c, w, cw);
            if (c->kind != B_ATOMIC || c->atomic == AT_INLINE_BLOCK) layout_inner(c, NULL, 0, 0, defh);
            it[i].size = c->h + vext(c) + c->m[0] + c->m[2];
            it[i].align = al;
        }
        int i0 = 0;
        float content_height = 0;
        while (i0 < n) {
            int i1 = i0;
            float total = 0;
            /* Indefinite height cannot create arbitrary column breaks. */
            while (i1 < n) {
                float add = it[i1].size + (i1 > i0 ? gap_main : 0);
                if (!single && defh >= 0 && i1 > i0 && total + add > defh + 0.01f) break;
                total += add;
                i1++;
            }
            float grow = 0;
            for (int i = i0; i < i1; i++) grow += it[i].b->st->flex_grow;
            float free_space = defh >= 0 ? defh - total : 0;
            if (free_space > 0 && grow > 0) {
                float share = free_space * fminf_(grow, 1);
                for (int i = i0; i < i1; i++) {
                    box_t *c = it[i].b;
                    float old = c->h, want = clamp_h(c, old + share * c->st->flex_grow / grow, defh);
                    layout_stretched_height(c, want, defh);
                    it[i].size += c->h - old;
                    total += c->h - old;
                }
                free_space = defh - total;
            }
            float start = 0, between = gap_main, auto_m = 0;
            size_t nauto = 0;
            for (int i = i0; i < i1; i++)
                nauto += len_auto(&it[i].b->st->margin[0]) + len_auto(&it[i].b->st->margin[2]);
            if (free_space > 0 && nauto) auto_m = free_space / (float)nauto;
            else if (free_space > 0) {
                int count = i1 - i0;
                switch (st->justify_content) {
                case JC_END: start = free_space; break;
                case JC_CENTER: start = free_space / 2; break;
                case JC_BETWEEN: if (count > 1) between += free_space / (float)(count - 1); break;
                case JC_AROUND: start = free_space / (float)count / 2; between += free_space / (float)count; break;
                case JC_EVENLY: start = free_space / ((float)count + 1); between += free_space / ((float)count + 1); break;
                }
            }
            struct fline *line = &lines[line_count++];
            *line = (struct fline){.first=i0, .end=i1};
            float y = start, extent = defh >= 0 ? defh : total;
            for (int i = i0; i < i1; i++) {
                box_t *c = it[i].b;
                int lead = reverse ? 2 : 0, trail = reverse ? 0 : 2;
                if (len_auto(&c->st->margin[lead])) y += auto_m;
                float by = reverse ? extent - y - it[i].size : y;
                c->y = by + c->m[0] + c->b[0] + c->p[0];
                line->cross = fmaxf_(line->cross, c->w + hext(c) + c->m[1] + c->m[3]);
                y += it[i].size + between;
                if (len_auto(&c->st->margin[trail])) y += auto_m;
            }
            content_height = fmaxf_(content_height, extent);
            i0 = i1;
        }
        b->h = defh >= 0 ? defh : content_height;
        flex_cross_lines(b, it, lines, line_count, true, single, defh, gap_cross);
        flex_gap_decorations(b, it, lines, line_count, true, reverse);
        free(lines); free(it);
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
        /* The content-based automatic minimum is also capped by a definite
           maximum. Otherwise an unspaced Japanese paragraph can make a
           max-width:720px article wider than its flex container. */
        float smax = spec_w(c, &c->st->max_width, cw);
        if (smax >= 0 && smax < auto_min) auto_min = smax;
        it[i].mn = smin >= 0 ? smin : auto_min;
        it[i].mx = smax >= 0 ? smax : INF;
        if (it[i].mx < it[i].mn) it[i].mx = it[i].mn;
        it[i].base = base;
        it[i].hypo = fminf_(fmaxf_(base, it[i].mn), it[i].mx);
        it[i].align = c->st->align_self != 255 ? c->st->align_self : st->align_items;
    }
    /* lines */
    int i0 = 0;
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
        if(!flex_resolve_row(it,i0,i1,cw,gap_main)){free(lines);free(it);return;}
        /* lay out the items at their sizes */
        struct fline *line = &lines[line_count++];
        *line = (struct fline){.first=i0, .end=i1};
        pvec children={0};
        for(int i=i0;i<i1;i++){it[i].b->w=it[i].size;pv_push(&children,it[i].b);}
        bool laid_out=layout_children_parallel((box_t **)children.v,children.n,defh);
        pv_free(&children);
        for (int i = i0; i < i1; i++) {
            box_t *c = it[i].b;
            if (!laid_out && !(c->kind == B_ATOMIC && c->atomic != AT_INLINE_BLOCK))
                layout_inner(c, NULL, 0, 0, defh);
            it[i].cross = c->h + vext(c) + c->m[0] + c->m[2];
            line->cross = fmaxf_(line->cross, it[i].cross);
            if (it[i].align == AI_BASELINE && c->baseline >= 0 &&
                !len_auto(&c->st->margin[0]) && !len_auto(&c->st->margin[2])) {
                float ascent = c->m[0] + c->b[0] + c->p[0] + c->baseline;
                line->ascent = fmaxf_(line->ascent, ascent);
                line->descent = fmaxf_(line->descent, it[i].cross - ascent);
            }
        }
        line->cross = fmaxf_(line->cross, line->ascent + line->descent);
        /* main axis positions */
        float total = gap_main * (float)(i1 - i0 - 1);
        size_t nauto = 0;
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
            case JC_EVENLY: start = left / ((float)(i1 - i0) + 1); between += left / ((float)(i1 - i0) + 1); break;
            }
        }
        float x = start;
        for (int i = i0; i < i1; i++) {
            box_t *c = it[i].b;
            int lead = reverse ? 1 : 3, trail = reverse ? 3 : 1;
            if (len_auto(&c->st->margin[lead])) x += auto_m;
            float bx = (reverse ? cw - x - it[i].size - it[i].ext : x) + c->m[3];
            c->x = bx + c->b[3] + c->p[3];
            x += it[i].size + it[i].ext + between;
            if (len_auto(&c->st->margin[trail])) x += auto_m;
        }
        i0 = i1;
    }
    flex_cross_lines(b, it, lines, line_count, false, single, defh, gap_cross);
    flex_gap_decorations(b, it, lines, line_count, false, reverse);
    free(lines); free(it);
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
    float cmin, rmin;   /* the column and row minimum contributions (gr_minimum) */
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

/* An item's minimum contribution to the tracks [s0, s1) of one axis (css-grid-2 §6.6). content is
   its min-content contribution, which a definite preferred size keeps. Otherwise the used minimum
   size decides: lo (-1 for none) plus ext, the axis' padding, border and margins. An auto minimum is
   content-based only for an item that is no scroll container and spans an auto-minimum track, but no
   flexible track when it spans several: a sidebar spanning a 1fr row scrolls inside it. */
static float gr_minimum(const box_t *c, const struct gtr *tr, int s0, int s1, const len_t *size, const len_t *min,
                        float lo, float content, float ext) {
    if ((size->kind == LK_LEN || size->kind == LK_EXPR) && !len_has_pct(size)) return content;
    if (!len_auto(min)) return (lo > 0 ? lo : 0) + ext;
    if (c->st->overflow != OV_VISIBLE && c->st->overflow != OV_CLIP) return ext;
    bool auto_min = false, flex = false;
    for (int k = s0; k < s1; k++) {
        auto_min |= tr[k].t.min_kind == GT_AUTO;
        flex |= tr[k].t.max_kind == GT_FR;
    }
    return auto_min && (s1 - s0 == 1 || !flex) ? content : ext;
}

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
        if (!layout_checkpoint()) goto placement_done;
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
        if (!layout_checkpoint()) goto placement_done;
        int rspan = it[i].r1;
        if (it[i].c0 >= 0) {
            if (it[i].c0 < cc) cr++;
            cc = it[i].c0;
            while (cr < GR_MAX_ROWS && !gr_free_at(occ, rows, nc, cr, cr + rspan, it[i].c0, it[i].c1)) {
                if (!layout_checkpoint()) goto placement_done;
                cr++;
            }
        } else {
            int span = it[i].c1 > nc ? nc : it[i].c1;
            for (;;) {
                if (!layout_checkpoint()) goto placement_done;
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
placement_done:
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
            if (!layout_checkpoint()) return;
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
    /* Auto-placement may leave later items unresolved after a stop. Never
       use their negative coordinates to index the track arrays. */
    if (D->native_cancelled) { free(cx); free(rx); return; }
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

/* What a track's auto minimum takes from an item (css-grid-2 §12.5): its minimum contribution lo,
   or, while the grid is sized under a min-/max-content constraint, its min-content contribution mn
   limited by a fixed maximum and floored by lo. Min-content and max-content minimums take mn and
   the max-content contribution themselves. */
static float gr_auto_minimum(const struct gtrack *t, float mn, float lo, bool constrained, float pct) {
    if (!constrained) return lo;
    if (t->max_kind == GT_LEN || t->max_kind == GT_FIT) mn = fminf_(mn, len_resolve(&t->max, pct < 0 ? 0 : pct));
    return fmaxf_(mn, lo);
}

/* size the tracks of one axis. avail: the definite space, or -1 (then sized for max-content, or
   min-content when min is set). pct: the base for percentages, or -1. */
static void gr_size(struct gtr *tr, int nt, float gap, struct gitem *it, int n, bool col, float avail, bool min,
                    float pct) {
    for (int i = 0; i < nt; i++) {
        if (!layout_checkpoint()) return;
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
    bool constrained = avail < 0;
    int maxspan = 1;
    for (int i = 0; i < n; i++) {
        int s0 = col ? it[i].c0 : it[i].r0, s1 = col ? it[i].c1 : it[i].r1;
        float mn = col ? it[i].mn : it[i].h, mx = col ? it[i].mx : it[i].h, lo = col ? it[i].cmin : it[i].rmin;
        if (s1 - s0 > maxspan) maxspan = s1 - s0;
        if (s1 - s0 != 1) continue;
        struct gtr *t = &tr[s0];
        if (t->collapsed) continue;
        if (t->t.min_kind == GT_AUTO) t->base = fmaxf_(t->base, gr_auto_minimum(&t->t, mn, lo, constrained, pct));
        else if (t->t.min_kind == GT_MIN) t->base = fmaxf_(t->base, mn);
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
            float mn = col ? it[i].mn : it[i].h, mx = col ? it[i].mx : it[i].h, lo = col ? it[i].cmin : it[i].rmin;
            /* intrinsic minimums take the minimum contribution (the min-content one under a constraint) */
            float need = constrained ? fmaxf_(mn, lo) : lo;
            float sb = gap * (float)(span - 1), sl = sb;
            int nflex = 0, nint = 0, ncontent = 0, nlim = 0;
            for (int k = s0; k < s1; k++) {
                sb += tr[k].base;
                sl += tr[k].limit;
                nflex += tr[k].flex;
                nint += gr_intrinsic_kind(tr[k].t.min_kind) && !tr[k].flex;
                ncontent += (tr[k].t.min_kind == GT_MIN || tr[k].t.min_kind == GT_MAX) && !tr[k].flex;
                nlim += !tr[k].flex && tr[k].t.max_kind != GT_LEN;
            }
            if (nflex) { /* grow the flexible tracks */
                for (int k = s0; k < s1; k++)
                    if (tr[k].flex) tr[k].fmax = fmaxf_(tr[k].fmax, (mx - sl) / (float)nflex + tr[k].base);
                if (need > sb)
                    for (int k = s0; k < s1; k++)
                        if (tr[k].flex && tr[k].t.min_kind != GT_LEN) tr[k].base += (need - sb) / (float)nflex;
                continue;
            }
            if (need > sb && nint)
                for (int k = s0; k < s1; k++)
                    if (gr_intrinsic_kind(tr[k].t.min_kind)) {
                        tr[k].base += (need - sb) / (float)nint;
                        if (tr[k].limit < tr[k].base) tr[k].limit = tr[k].base;
                    }
            /* then min-content and max-content minimums take the min-content contribution */
            sb = gap * (float)(span - 1);
            for (int k = s0; k < s1; k++) sb += tr[k].base;
            if (mn > sb && ncontent)
                for (int k = s0; k < s1; k++)
                    if (tr[k].t.min_kind == GT_MIN || tr[k].t.min_kind == GT_MAX) {
                        tr[k].base += (mn - sb) / (float)ncontent;
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
        if (!layout_checkpoint()) return;
        struct gitem *t = &g->it[i];
        box_t *c = t->b;
        outer_intrinsic(c, &t->mn, &t->mx);
        t->cmin = gr_minimum(c, g->col, t->c0, t->c1, &c->st->width, &c->st->min_width,
                             spec_w(c, &c->st->min_width, -1), t->mn, hext(c) + c->m[1] + c->m[3]);
    }
}

static void grid_intrinsic(box_t *b, float *mn, float *mx) {
    struct grid g;
    gr_setup(&g, b, -1);
    if (D->native_cancelled) { *mn = *mx = 0; gr_free(&g); return; }
    gr_contributions(&g);
    if (D->native_cancelled) { *mn = *mx = 0; gr_free(&g); return; }
    float gap = b->st->gap_col;
    gr_size(g.col, g.nc, gap, g.it, g.n, true, -1, true, -1);
    *mn = gr_positions(g.col, g.nc, gap, 0, 0);
    gr_size(g.col, g.nc, gap, g.it, g.n, true, -1, false, -1);
    *mx = gr_positions(g.col, g.nc, gap, 0, 0);
    gr_free(&g);
}

/* css-gaps-1 §2 in a grid: the gutters between adjacent tracks that are not collapsed (§4.6 counts
   a run of collapsed gutters as one gap), each running along all the tracks of the other axis with
   a junction at every gutter crossing it. Which areas hold items comes from the placed items. */
static void grid_gap_decorations(box_t *b, const struct grid *gr) {
    const struct css_gap_rules *rules = b->st->gap_rules;
    if (!rules) return;
    struct gd g = {0};
    for (int a = 0; a < 2; a++) {
        g.rule_break[a] = rules->axis[a].rule_break;
        g.visibility[a] = rules->axis[a].visibility == CSS_RULE_ITEMS_NORMAL ? CSS_RULE_ITEMS_ALL : rules->axis[a].visibility;
    }
    /* the tracks that are not collapsed (rows, then columns), and each track's index among them */
    size_t nr = (size_t)(gr->nr > 0 ? gr->nr : 0), nc = (size_t)(gr->nc > 0 ? gr->nc : 0);
    int *track = malloc((nr + nc) * 2 * sizeof *track + 1);
    if (!track) { ar_alloc(layout_scratch(), SIZE_MAX); return; }
    int *rows = track, *cols = track + nr, *row_at = track + nr + nc, *col_at = row_at + nr;
    size_t vr = 0, vc = 0;
    for (size_t i = 0; i < nr; i++) { row_at[i] = gr->row[i].collapsed ? -1 : (int)vr; if (!gr->row[i].collapsed) rows[vr++] = (int)i; }
    for (size_t i = 0; i < nc; i++) { col_at[i] = gr->col[i].collapsed ? -1 : (int)vc; if (!gr->col[i].collapsed) cols[vc++] = (int)i; }
    if (!vr || !vc || (vr < 2 && vc < 2)) { free(track); return; }
    /* the item (index + 1) each area holds; an area two items share is the first one's */
    uint32_t *area = vr > SIZE_MAX / sizeof *area / vc ? NULL : calloc(vr * vc, sizeof *area);
    bool ok = area != NULL;
    for (int i = 0; ok && i < gr->n; i++) {
        if (!(i & 255) && !layout_checkpoint()) { ok = false; break; }
        const struct gitem *t = &gr->it[i];
        for (int r = t->r0; r < t->r1 && r < (int)nr; r++) {
            if (r < 0 || row_at[r] < 0) continue;
            for (int c = t->c0; c < t->c1 && c < (int)nc; c++)
                if (c >= 0 && col_at[c] >= 0 && !area[(size_t)row_at[r] * vc + (size_t)col_at[c]])
                    area[(size_t)row_at[r] * vc + (size_t)col_at[c]] = (uint32_t)i + 1;
        }
    }
#define AREA(r, c) area[(size_t)(r) * vc + (size_t)(c)]
    size_t column_gaps = vc - 1;
    /* column gaps (ids 0 .. vc - 2), along the rows; the row gaps (ids from vc - 1) cross them */
    for (size_t k = 0; ok && k + 1 < vc; k++) {
        const struct gtr *left = &gr->col[cols[k]], *right = &gr->col[cols[k + 1]];
        struct gd_gap *x = gd_gap_add(&g, 0, left->pos + left->size, right->pos, vr);
        if (!x) { ok = false; break; }
        uint8_t *cell = g.cell + x->cell;
        struct gd_bound *bound = g.bound + x->bound;
        for (size_t j = 0; j < vr; j++) {
            uint32_t l = AREA(j, k), r = AREA(j, k + 1);
            cell[j] = (l && l == r ? GD_CROSSED : 0) | (l ? GD_BEFORE : 0) | (r ? GD_AFTER : 0);
        }
        bound[0].start_at = gr->row[rows[0]].pos;
        bound[vr].end_at = gr->row[rows[vr - 1]].pos + gr->row[rows[vr - 1]].size;
        for (size_t j = 1; ok && j < vr; j++) {
            const struct gtr *above = &gr->row[rows[j - 1]];
            bound[j].end_at = above->pos + above->size;
            bound[j].start_at = gr->row[rows[j]].pos;
            bound[j].width = fmaxf_(0, bound[j].start_at - bound[j].end_at);
            uint32_t l0 = AREA(j - 1, k), l1 = AREA(j, k), r0 = AREA(j - 1, k + 1), r1 = AREA(j, k + 1);
            bound[j].flanked = l0 && l0 == l1 && r0 && r0 == r1;
            ok = gd_cross(&g, &bound[j], column_gaps + j - 1);
        }
    }
    /* row gaps, along the columns; the column gaps cross them */
    for (size_t j = 0; ok && j + 1 < vr; j++) {
        const struct gtr *above = &gr->row[rows[j]], *below = &gr->row[rows[j + 1]];
        struct gd_gap *x = gd_gap_add(&g, 1, above->pos + above->size, below->pos, vc);
        if (!x) { ok = false; break; }
        uint8_t *cell = g.cell + x->cell;
        struct gd_bound *bound = g.bound + x->bound;
        for (size_t k = 0; k < vc; k++) {
            uint32_t a = AREA(j, k), c = AREA(j + 1, k);
            cell[k] = (a && a == c ? GD_CROSSED : 0) | (a ? GD_BEFORE : 0) | (c ? GD_AFTER : 0);
        }
        bound[0].start_at = gr->col[cols[0]].pos;
        bound[vc].end_at = gr->col[cols[vc - 1]].pos + gr->col[cols[vc - 1]].size;
        for (size_t k = 1; ok && k < vc; k++) {
            const struct gtr *left = &gr->col[cols[k - 1]];
            bound[k].end_at = left->pos + left->size;
            bound[k].start_at = gr->col[cols[k]].pos;
            bound[k].width = fmaxf_(0, bound[k].start_at - bound[k].end_at);
            uint32_t a0 = AREA(j, k - 1), a1 = AREA(j, k), c0 = AREA(j + 1, k - 1), c1 = AREA(j + 1, k);
            bound[k].flanked = a0 && a0 == a1 && c0 && c0 == c1;
            ok = gd_cross(&g, &bound[k], k - 1);
        }
    }
#undef AREA
    free(area);
    free(track);
    if (!ok) {
        gd_free(&g);
        if (!__atomic_load_n(&D->native_cancelled, __ATOMIC_ACQUIRE)) ar_alloc(layout_scratch(), SIZE_MAX);
        return;
    }
    gd_finish(b, &g);
}

static void layout_grid(box_t *b, float defh) {
    const style_t *st = b->st;
    float cw = b->w;
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
    if (D->native_cancelled) { gr_free(&g); return; }
    gr_contributions(&g);
    /* columns */
    if (D->native_cancelled) { gr_free(&g); return; }
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
    /* Resolve every independent column width before worker publication. */
    pvec children={0};
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
        }
        pv_push(&children,c);
    }
    bool laid_out=layout_children_parallel((box_t **)children.v,children.n,-1);
    pv_free(&children);
    for(int i=0;i<g.n;i++){
        box_t *c=g.it[i].b;
        if(!laid_out && !(c->kind==B_ATOMIC && c->atomic!=AT_INLINE_BLOCK))
            layout_inner(c,NULL,0,0,-1);
        g.it[i].h=c->h+vext(c)+c->m[0]+c->m[2];
        g.it[i].rmin=gr_minimum(c,g.row,g.it[i].r0,g.it[i].r1,&c->st->height,&c->st->min_height,
                                spec_h(c,&c->st->min_height,-1),g.it[i].h,vext(c)+c->m[0]+c->m[2]);
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
        if (len_auto(&c->st->margin[0]) && len_auto(&c->st->margin[2])) yoff = (ah - t->h) / 2;
        else if (len_auto(&c->st->margin[0])) yoff = ah - t->h;
        else if (as == AI_CENTER) yoff = (ah - t->h) / 2;
        else if (as == AI_END) yoff = ah - t->h;
        else if (stretches_height(c, as)) {
            float want = clamp_h(c, ah - vext(c) - c->m[0] - c->m[2], ah);
            layout_stretched_height(c, want, ah);
            t->h = c->h + vext(c) + c->m[0] + c->m[2];
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
    grid_gap_decorations(b, &g);
    gr_free(&g);
}

/* ---------------------------------------------------------------- intrinsic sizes */
static void inline_intrinsic(box_t *b, float *mn, float *mx) {
    struct ivec iv = {0};
    struct ibuild bs = {&iv, true, NULL, 0, NULL, false};
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
    parallel_record_work(PARALLEL_INTRINSIC, 1);
    uint64_t generation = font_metrics_generation();
    if (b->intrinsic_done && generation && b->intrinsic_font_generation == generation) {
        *mn = b->min_cw;
        *mx = b->max_cw;
        return;
    }
    float a = 0, z = 0;
    if (size_container(b)) {
        /* Inline-axis intrinsic sizes are those of an empty principal box.
           Explicit width/min/max and its edges are applied by outer_intrinsic. */
        a=z=0;
    } else if (b->kind == B_ATOMIC && b->atomic != AT_INLINE_BLOCK) {
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
    b->intrinsic_font_generation = font_metrics_generation();
    b->intrinsic_done = generation && generation == b->intrinsic_font_generation;
    *mn = a;
    *mx = z;
}

/* margin-box contributions */
static void outer_intrinsic(box_t *c, float *mn, float *mx) {
    resolve_edges(c, 0);
    float ext = hext(c) + c->m[1] + c->m[3];
    const len_t *wl = &c->st->width;
    /* Replaced atomics size themselves in intrinsic(); an inline-block is a
       non-replaced box whose definite width is its contribution. */
    if ((c->kind != B_ATOMIC || c->atomic == AT_INLINE_BLOCK || size_container(c)) &&
        wl->kind == LK_LEN && wl->pct == 0) {
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
/* Legacy frameset track grammar: integer pixels, percentages and weighted *.
   It lays out native child viewport boxes, never inserts their DOM into the
   containing document. Oversubscribed fixed tracks shrink proportionally. */
struct frameset_track { double value, weight; };
static struct frameset_track *frameset_tracks(const char *text,float extent,size_t *length) {
    size_t capacity=1;
    if(text)for(const char *p=text;*p;p++)if(*p==',' && p[1]){
        if(capacity==SIZE_MAX)ar_alloc(layout_scratch(),SIZE_MAX);
        capacity++;
    }
    if(capacity>SIZE_MAX/sizeof(struct frameset_track))ar_alloc(layout_scratch(),SIZE_MAX);
    struct frameset_track *tracks=ar_alloc(layout_scratch(),capacity*sizeof *tracks);
    size_t count=0;double fixed=0,stars=0;
    double available=isfinite(extent) && extent>0?extent:0;
    if(!text || !*text){tracks[0].value=available;*length=1;return tracks;}
    const char *p=text;
    while(*p){
        while(is_space(*p))p++;
        char *end;double value=strtof(p,&end);if(end==p || !isfinite(value) || value<0)value=0;
        p=end;while(is_space(*p))p++;
        if(*p=='*'){tracks[count].weight=value>0?value:1;stars+=tracks[count].weight;p++;}
        else {if(*p=='%'){value=available*(value/100);p++;}tracks[count].value=value;fixed+=value;}
        count++;while(*p && *p!=',')p++;if(*p)p++;
    }
    if(fixed>available && fixed>0)for(size_t i=0;i<count;i++)tracks[i].value*=available/fixed;
    else if(stars>0)for(size_t i=0;i<count;i++)tracks[i].value+=(available-fixed)*tracks[i].weight/stars;
    else if(fixed>0)for(size_t i=0;i<count;i++)tracks[i].value*=available/fixed;
    else for(size_t i=0;i<count;i++)tracks[i].value=available/count;
    *length=count;return tracks;
}
struct frameset_layout {
    struct frameset_layout *previous;
    box_t *box,*next;
    struct frameset_track *rows,*cols;
    size_t nr,nc,row,col;
    float x,y;
};
static struct frameset_layout *frameset_layout_push(box_t *b,float height,struct frameset_layout *previous) {
    struct frameset_layout *s=ar_alloc(layout_scratch(),sizeof *s);
    s->previous=previous;s->box=b;s->next=b->first;
    s->rows=frameset_tracks(node_attr(b->node,"rows"),height,&s->nr);
    s->cols=frameset_tracks(node_attr(b->node,"cols"),b->w,&s->nc);
    b->h=height;b->inline_ctx=false;return s;
}
static void layout_frameset(box_t *b,float height) {
    /* Both track storage and nested frameset traversal follow actual demand;
       no native stack recursion or fixed number of accepted rows/columns. */
    struct frameset_layout *s=frameset_layout_push(b,height,NULL);
    while(s){
        box_t *child=s->next;
        if(!child){s=s->previous;continue;}
        s->next=child->next;
        if(s->row>=s->nr){child->w=child->h=0;continue;}
        child->cb=s->box;child->x=s->x;child->y=s->y;
        child->w=(float)s->cols[s->col].value;child->h=(float)s->rows[s->row].value;
        memset(child->m,0,sizeof child->m);memset(child->p,0,sizeof child->p);memset(child->b,0,sizeof child->b);
        child->baseline=child->last_baseline=child->h;
        s->x+=child->w;
        if(++s->col==s->nc){s->col=0;s->x=0;s->y+=(float)s->rows[s->row++].value;}
        if(child->node && child->node->tag==T_frameset)s=frameset_layout_push(child,child->h,s);
    }
}

/* Legacy line clamping consumes real line boxes in this BFC, including
 * anonymous inline contexts between paragraphs. Complete independent child
 * layouts first, then reduce their immutable line results before the parent's
 * size reduction. No shared line counter is borrowed by an AP. Intrinsic
 * widths intentionally continue to see the original DOM/text. */
struct clamp_state {
    box_t *root, *last;
    uint32_t limit, used;
    int last_line;
    bool truncated;
};
static const char clamp_ellipsis[] = "\xe2\x80\xa6";

static void clamp_hide(box_t *b) {
    b->clamp_hidden = true;
    for (box_t *c = b->first; c; c = c->next) clamp_hide(c);
}
static void clamp_visibility_reset(box_t *b) {
    if (!layout_checkpoint()) return;
    b->clamp_hidden = false;
    for (box_t *c = b->first; c; c = c->next) clamp_visibility_reset(c);
}
static void truncate_decos(box_t *b, int count) {
    /* Truncation changes the published count, so retirement cannot discover
       discarded fragments later. Release their owned masks before hiding them. */
    for (int i=count;i<b->ndecos;i++) {
        web_shadow_cache_free(&b->decos[i].shadow_cache);
        for(int j=0;j<2;j++)web_radius_cache_free(&b->decos[i].radius_clip[j]);
    }
    b->ndecos=count;
}
static void clamp_scan(struct clamp_state *state, box_t *b) {
    if (b->clamp_hidden || !layout_checkpoint()) return;
    if (state->truncated) { clamp_hide(b); return; }
    if (b != state->root && (b->abspos || b->floated)) return;
    if (b != state->root && state->used == state->limit && b->h + vext(b) > 0) {
        state->truncated = true; clamp_hide(b); return;
    }
    /* Other BFCs (inline blocks, grid/flex, floats, overflow contexts) do not
       consume the enclosing BFC's line budget. They can still occur after its
       clamp point and must not resurrect elided content. */
    if (b != state->root && is_bfc_root(b)) {
        if (state->used == state->limit && b->h + vext(b) > 0) {
            state->truncated = true; clamp_hide(b);
        }
        return;
    }
    if (b->inline_ctx && b->nlines) {
        uint32_t remain = state->limit - state->used;
        int keep = remain < (uint32_t)b->nlines ? (int)remain : b->nlines;
        if (keep) {
            state->used += (uint32_t)keep;
            state->last = b; state->last_line = keep - 1;
        }
        if (keep < b->nlines) {
            state->truncated = true;
            if (!keep) clamp_hide(b);
            else {
                b->nruns = b->lines[keep - 1].end_run;
                truncate_decos(b,b->lines[keep - 1].end_deco);
                b->nlines = keep;
            }
        }
        return; /* atomic inline descendants belong to one outer line */
    }
    for (box_t *c = b->first; c; c = c->next) clamp_scan(state, c);
}

/* Only surviving inline fragments keep their native paint/hit ancestry.
   Atomic boxes keep their own private clamp state when their outer run fits. */
static void clamp_inline_hide(box_t *b, box_t *owner, float bottom) {
    for (box_t *c = b->first; c; c = c->next) {
        if (c->abspos || c->floated) {
            float y = c->abspos ? c->static_y : c->y;
            if (y >= bottom) clamp_hide(c);
            continue;
        }
        c->clamp_hidden = true;
        if (c->kind == B_INLINE) clamp_inline_hide(c, owner, bottom);
    }
    (void)owner;
}
static void clamp_show_path(box_t *b, box_t *owner) {
    box_t *p = b;
    while (p && p != owner) p = p->parent;
    if (!p) return; /* split inline's principal box may belong to another line context */
    for (; b && b != owner; b = b->parent) b->clamp_hidden = false;
}
static int clamp_prefix(const struct run *r, float available, float *width) {
    wfont font = style_font(r->st);
    int count = 0;
    for (int i = 0; i < r->n; i++) if (((unsigned char)r->s[i] & 0xc0) != 0x80) count++;
    int low = 0, high = count;
    while (low < high) {
        int middle = low + (high - low + 1) / 2, bytes = 0, chars = 0;
        while (bytes < r->n && chars < middle) {
            bytes++; while (bytes < r->n && ((unsigned char)r->s[bytes] & 0xc0) == 0x80) bytes++;
            chars++;
        }
        float w = text_width_uncached(r->st, &font, r->s, (size_t)bytes);
        if (w <= available) low = middle; else high = middle - 1;
    }
    int bytes = 0, chars = 0;
    while (bytes < r->n && chars < low) {
        bytes++; while (bytes < r->n && ((unsigned char)r->s[bytes] & 0xc0) == 0x80) bytes++;
        chars++;
    }
    while (bytes > 0 && r->s[bytes - 1] == ' ') bytes--;
    *width = text_width_uncached(r->st, &font, r->s, (size_t)bytes);
    return bytes;
}
static float clamp_local_y(box_t *b, box_t *root) {
    float y = 0;
    for (; b && b != root; b = b->cb)
        y += b->y + (b->cb ? b->cb->content_dy : 0);
    return y; /* normal-flow position, not relative/scroll/viewport transforms */
}
static void clamp_apply(box_t *root) {
    struct clamp_state state = {.root = root, .limit = root->st->line_clamp};
    clamp_scan(&state, root);
    struct clamp_trace *trace = root->clamp_trace;
    if (trace) { trace->full_h = root->h; trace->cut_h = root->h; trace->lines = state.used; }
    if (trace && state.last) {
        box_t *p = state.last;
        while (p && p != root) p = p->cb;
        trace->cb_reached = p == root;
        trace->last_kind = state.last->kind; trace->last_anon = state.last->anon;
        trace->last_y = clamp_local_y(state.last, root);
        trace->line_bottom = state.last->lines[state.last_line].bottom;
    }
    if (!state.truncated || !state.last || !layout_checkpoint()) return;
    box_t *b = state.last;
    struct line_fragment *line = &b->lines[state.last_line];
    b->nruns = line->end_run; truncate_decos(b,line->end_deco); b->nlines = state.last_line + 1;
    wfont font = style_font(b->st);
    float ellipsis_width = text_width_uncached(b->st, &font, clamp_ellipsis, sizeof clamp_ellipsis - 1);
    float edge = fmaxf_(line->left, line->right - ellipsis_width), x = line->left;
    int end = line->first_run;
    for (int i = line->first_run; i < b->nruns; i++) {
        struct run *r = &b->runs[i];
        if (r->s == clamp_ellipsis) break;
        if (r->x >= edge) break;
        if (r->x + r->w > edge) {
            if (r->atomic) break;
            r->n = clamp_prefix(r, edge - r->x, &r->w);
            if (r->n) { x = r->x + r->w; end = i + 1; }
            break;
        }
        x = r->x + r->w; end = i + 1;
    }
    b->nruns = end;
    /* Ellipsis is anonymous content, not a synthetic DOM node or a link. */
    jmp_buf *previous_trap = b->layout_mem.trap;
    b->layout_mem.trap = layout_scratch()->trap;
    struct run *runs = ar_alloc(&b->layout_mem, ((size_t)end + 1) * sizeof *runs);
    if (end) memcpy(runs, b->runs, (size_t)end * sizeof *runs);
    runs[end] = (struct run){x, line->baseline, ellipsis_width, clamp_ellipsis,
        sizeof clamp_ellipsis - 1, b->st, NULL, NULL, false, NULL, NULL, -1, 0, 0, 0};
    float ellipsis_ascent;
    wf_metrics(&font, &ellipsis_ascent, &runs[end].descent);
    b->layout_mem.trap = previous_trap;
    b->runs = runs; b->nruns++; line->end_run = b->nruns;
    for (int i = line->first_deco; i < b->ndecos; i++) {
        struct deco *deco = &b->decos[i];
        deco->w = fmaxf_(0, fminf_(deco->x + deco->w, x) - deco->x);
    }
    clamp_inline_hide(b, b, line->bottom);
    for (int i = 0; i < b->nruns; i++) {
        struct run *r = &b->runs[i];
        if (r->atomic) clamp_show_path(r->atomic, b);
        if (r->node) clamp_show_path(r->node->box, b);
    }
    for (int i = 0; i < b->ndecos; i++)
        if (b->decos[i].node && b->decos[i].w > 0) clamp_show_path(b->decos[i].node->box, b);
    float bottom = clamp_local_y(b, root) + line->bottom;
    if (trace) trace->applied = true;
    for (box_t *p = b; p && p != root; p = p->cb) {
        if (len_auto(&p->st->height)) p->h = fminf_(p->h, fmaxf_(0, bottom - clamp_local_y(p, root)));
        p->last_baseline = p == b ? line->baseline : b->last_baseline + clamp_local_y(b, p);
    }
    root->h = fminf_(root->h, fmaxf_(0, bottom));
    if (trace) trace->cut_h = root->h;
    root->last_baseline = line->baseline + clamp_local_y(b, root);
    root->clamp_truncated = true;
}

static void layout_inner(box_t *b, struct bfc *f, float ox, float oy, float cbh) {
    parallel_record_work(PARALLEL_LAYOUT, 1);
    layout_inner_used(b, f, ox, oy, cbh, -1);
}

static void layout_inner_body(box_t *b, struct bfc *f, float ox, float oy, float cbh, float usedh);
/* debug_js only: inclusive cycle cost per box for the slow-layout report. */
static void layout_inner_used(box_t *b, struct bfc *f, float ox, float oy, float cbh, float usedh) {
    if (!D->profile_enabled) { layout_inner_body(b, f, ox, oy, cbh, usedh); return; }
    uint64_t start = __builtin_ia32_rdtsc();
    layout_inner_body(b, f, ox, oy, cbh, usedh);
    b->profile_cycles += __builtin_ia32_rdtsc() - start; b->profile_calls++;
}
static void layout_inner_body(box_t *b, struct bfc *f, float ox, float oy, float cbh, float usedh) {
    if (!layout_checkpoint()) return;
    layout_media_checkpoint();
    float input_width = b->w;
    uint64_t font_generation = font_metrics_generation();
    bool clamping = box_line_clamp(b);
    bool reusable = !layout_current()->clamp_depth && !clamping &&
        (!f || is_bfc_root(b) || (!f->n&&!b->layout_cache_exports_floats)) && b->layout_cache_safe && font_generation;
    if (reusable && b->layout_cache_valid && !b->layout_dirty &&
        b->cached_width == input_width && b->cached_cbh == cbh &&
        b->cached_usedh == usedh && b->cached_font_generation == font_generation &&
        !memcmp(b->cached_padding, b->p, sizeof b->p) &&
        !memcmp(b->cached_border, b->b, sizeof b->b)) {
        b->h = b->cached_height;
        /* resolve_edges just reset the bottom margin to its own */
        b->m[2] = mcollapse(b->m[2], b->cached_end_margin);
        b->baseline = b->cached_baseline;
        b->last_baseline = b->cached_last_baseline;
        b->content_dy = b->cached_content_dy;
        return;
    }
    b->layout_post_valid = false;
    for(int i=0;i<b->ndecos;i++) {
        web_shadow_cache_free(&b->decos[i].shadow_cache);
        for(int j=0;j<2;j++)web_radius_cache_free(&b->decos[i].radius_clip[j]);
    }
    ar_free(&b->layout_mem);
    b->decos=NULL;b->ndecos=0; /* including early-return frameset layout */
    b->gap_decorations = NULL;
    b->layout_mem.chunk_size = 4096;
    b->layout_mem.trap = layout_scratch()->trap;
    b->clamp_trace = NULL;
    if (!b->anon && b->st->line_clamp && D->profile_enabled) {
        b->clamp_trace = ar_alloc(&b->layout_mem, sizeof *b->clamp_trace);
        *b->clamp_trace = (struct clamp_trace){.usedh = usedh, .limit = b->st->line_clamp,
            .eligible = clamping};
    }
    if(b->node && !b->node->foreign && b->node->tag==T_frameset){
        layout_frameset(b,usedh>=0?usedh:cbh>=0?cbh:VH);
        b->layout_mem.trap = NULL;
        b->layout_cache_valid = false;
        return;
    }
    struct bfc own = {0};
    bool root = is_bfc_root(b);
    if (root) {
        f = &own;
        ox = oy = 0;
    }
    /* box.c represents an inline split around block children by a block
       proxy retaining display:inline. That proxy is not a block container
       element for percentage heights, and height/min/max-height do not
       apply to the original non-replaced inline (CSS 2.2 9.2.1.1, 10.5). */
    bool split_inline = b->kind == B_BLOCK && b->st->display == D_INLINE;
    float sh = split_inline ? -1 : usedh >= 0 ? usedh : resolved_content_height(b, cbh);
    if(!split_inline && sh<0 && size_container(b) && b->st->container_type==CT_SIZE)sh=0;
    if (b->clamp_trace) b->clamp_trace->sh = sh;
    /* The anonymous initial containing block supplies the viewport height,
       even though its own auto height follows document content. Otherwise
       html/body height:100% lose their definite reference at the first box.
       A definite height's min/max constraints also apply to descendants;
       normal-flow auto heights (including min-height-only boxes) remain
       indefinite. Inset-constrained absolute heights are already definite. */
    float child_cbh = b == D->root_box ? VH : sh >= 0 ? clamp_h(b, sh, cbh) : -1;
    /* Anonymous wrappers around mixed inline/block content do not establish
       a percentage-height reference (CSS 2.2 9.2.1.1). Keep their own height
       content-sized, but let inline descendants see the non-anonymous
       container's definite or indefinite height. Explicit flex/grid used
       heights, when supplied, still define that item's sizing reference. */
    if (split_inline || (b->anon && !b->node && b->kind == B_BLOCK && b->inline_ctx && usedh < 0 && b != D->root_box))
        child_cbh = cbh;
    b->baseline = b->last_baseline = -1;
    b->cached_end_margin = 0;
    b->nruns = b->ndecos = 0;
    b->nlines = 0; b->lines = NULL; b->clamp_truncated = false;
    if (clamping) {
        /* Flex/grid measurement and final placement can enter this subtree
           more than once within one layout_doc, after its old clamp already
           hid tail boxes. A fresh line result must not scan that stale paint
           visibility: otherwise it consumes exactly the budget, skips the
           hidden overflow, and publishes full height without an ellipsis.
           This root owns the reset subtree; clamp_depth prevents its child
           layouts (including independent nested clamps) reusing old runs. */
        clamp_visibility_reset(b);
        layout_current()->clamp_depth++;
    }
    switch (b->kind) {
    case B_TABLE: layout_table(b, cbh); break;
    case B_FLEX: layout_flex(b, sh >= 0 ? clamp_h(b, sh, cbh) : sh); break;
    case B_GRID: layout_grid(b, sh); break;
    default:
        if (b->inline_ctx) layout_inline(b, f, ox, oy, child_cbh);
        else layout_blocks(b, f, ox, oy, child_cbh);
    }
    if (root && f->n) { /* a new formatting context contains its floats */
        float fb = bfc_bottom(f);
        if (fb > b->h) b->h = fb;
    }
    if (b->clamp_trace) b->clamp_trace->full_h = b->clamp_trace->cut_h = b->h;
    if (clamping) {
        clamp_apply(b);
        layout_current()->clamp_depth--;
    }
    if (b->kind != B_TABLE) {
        float content = b->h;
        if (sh >= 0) b->h = sh;
        if (!split_inline) b->h = clamp_h(b, b->h, cbh);
        /* HTML rendering §15.5.3: a button's anonymous content box (not a flex or grid
           button's) is centred vertically when it does not overflow the button. */
        if (b->node && !b->anon && !b->node->foreign && b->node->tag == T_button) {
            float dy = b->kind != B_FLEX && b->kind != B_GRID && b->h > content ? (b->h - content) / 2 : 0;
            if (b->baseline >= 0) b->baseline += dy;
            if (b->last_baseline >= 0) b->last_baseline += dy;
            b->content_dy = dy;
        }
    }
    if (b->clamp_trace) b->clamp_trace->post_h = b->h;
    if (root) bfc_free(&own);
    b->layout_mem.trap = NULL;
    uint64_t final_generation = font_metrics_generation();
    b->layout_cache_valid = reusable &&
        !__atomic_load_n(&D->native_cancelled, __ATOMIC_ACQUIRE) && font_generation == final_generation;
    b->layout_dirty = false;
    b->cached_width = input_width;
    b->cached_cbh = cbh;
    b->cached_usedh = usedh;
    b->cached_font_generation = final_generation;
    memcpy(b->cached_padding, b->p, sizeof b->p);
    memcpy(b->cached_border, b->b, sizeof b->b);
    b->cached_height = b->h;
    b->cached_baseline = b->baseline;
    b->cached_last_baseline = b->last_baseline;
    b->cached_content_dy = b->content_dy;
}

/* Called only by the supervisor after every helper joined and container
 * settling finished. No author strings, node IDs, URLs or script callbacks.
 * With debug_js off neither the extra traversal nor console call occurs. */
static void replaced_trace_report_box(web_doc *d, box_t *b) {
    if (d->replaced_trace_reported >= 2 || b->kind != B_ATOMIC || b->atomic != AT_IMG ||
        !b->node || !b->st || b->st->visibility || b->clamp_hidden || !(b->w > 0) || b->h != 0)
        return;
    int image = b->node->image;
    struct web_image *im = image >= 0 && image < d->images.n ? d->images.v[image] : NULL;
    if (!im || !im->img || im->img->w <= 0 || im->img->h <= 0) return;
    unsigned trace = ++d->replaced_trace_reported;
    char message[1280];
    snprintf(message, sizeof message, "Native replaced size trace: #%u image=%d natural=%u/%u used=%.3f/%.3f",
        trace, image, (unsigned)im->img->w, (unsigned)im->img->h, b->w, b->h);
    web_js_trace(d, message);
    unsigned depth = 0;
    for (box_t *a = b; a && depth < 10; a = a->parent, depth++) {
        const style_t *s = a->st;
        if (!s) break;
        snprintf(message, sizeof message,
            "Native replaced size trace: #%u depth=%u kind=%u atomic=%u tag=%u anon=%u inline_ctx=%u "
            "display=%u position=%u box_sizing=%u overflow=%u visibility=%u "
            "used=%.3f/%.3f xy=%.3f/%.3f width=%u/%.3f/%.3f height=%u/%.3f/%.3f "
            "minw=%u/%.3f/%.3f maxw=%u/%.3f/%.3f minh=%u/%.3f/%.3f maxh=%u/%.3f/%.3f "
            "padding=%.3f/%.3f/%.3f/%.3f cache_valid=%u cache_width=%.3f cbh=%.3f usedh=%.3f "
            "cb_kind=%d cb_h=%.3f",
            trace, depth, (unsigned)a->kind, (unsigned)a->atomic, a->node ? (unsigned)a->node->tag : 0,
            a->anon, a->inline_ctx, (unsigned)s->display, (unsigned)s->position, s->box_sizing,
            (unsigned)s->overflow, s->visibility, a->w, a->h, a->x, a->y,
            (unsigned)s->width.kind, s->width.px, s->width.pct,
            (unsigned)s->height.kind, s->height.px, s->height.pct,
            (unsigned)s->min_width.kind, s->min_width.px, s->min_width.pct,
            (unsigned)s->max_width.kind, s->max_width.px, s->max_width.pct,
            (unsigned)s->min_height.kind, s->min_height.px, s->min_height.pct,
            (unsigned)s->max_height.kind, s->max_height.px, s->max_height.pct,
            a->p[0], a->p[1], a->p[2], a->p[3], a->layout_cache_valid,
            a->cached_width, a->cached_cbh, a->cached_usedh,
            a->cb ? (int)a->cb->kind : -1, a->cb ? a->cb->h : -1);
        web_js_trace(d, message);
    }
}
static void clamp_trace_report_box(web_doc *d, box_t *b, bool replaced_pending) {
    if (!b || (d->clamp_trace_reported >= 4 && (!replaced_pending || d->replaced_trace_reported >= 2))) return;
    if (replaced_pending) replaced_trace_report_box(d, b);
    const struct clamp_trace *t = b->clamp_trace;
    if (d->clamp_trace_reported < 4 && t && !b->clamp_hidden && (t->applied ||
        t->full_h > line_height_px(b->st) * (float)t->limit)) {
        const style_t *s = b->st;
        box_t *p = b->parent;
        char message[768];
        snprintf(message, sizeof message,
            "Native clamp trace: #%u gate=%u eligible=%u applied=%u kind=%u legacy=%u "
            "orient=%u display=%u inline_ctx=%u nlines=%d limit=%u lines=%u "
            "height=%u/%.2f/%.2f min=%u/%.2f/%.2f max=%u/%.2f/%.2f "
            "usedh=%.2f sh=%.2f full=%.2f cut=%.2f post=%.2f final=%.2f "
            "last=%u/%u y=%.2f bottom=%.2f cb_reached=%u parent=%d/%.2f flow_bottom=%.2f next_y=%.2f",
            ++d->clamp_trace_reported, d->profile_enabled, t->eligible, t->applied,
            (unsigned)b->kind, s->legacy_box, (unsigned)s->box_orient, (unsigned)s->display,
            b->inline_ctx, b->nlines, t->limit, t->lines,
            (unsigned)s->height.kind, s->height.px, s->height.pct,
            (unsigned)s->min_height.kind, s->min_height.px, s->min_height.pct,
            (unsigned)s->max_height.kind, s->max_height.px, s->max_height.pct,
            t->usedh, t->sh, t->full_h, t->cut_h, t->post_h, b->h,
            (unsigned)t->last_kind, t->last_anon, t->last_y, t->line_bottom, t->cb_reached,
            p ? (int)p->kind : -1, p ? p->h : -1,
            b->y + b->h + b->p[2] + b->b[2], b->next ? b->next->y : -1);
        web_js_trace(d, message);
    }
    for (box_t *c = b->first; c && (d->clamp_trace_reported < 4 ||
            (replaced_pending && d->replaced_trace_reported < 2)); c = c->next)
        clamp_trace_report_box(d, c, replaced_pending);
}
void layout_clamp_trace_report(web_doc *d) {
    if (!d || !d->profile_enabled || !d->js) return;
    /* A missing symptom must not keep this diagnostic alive on every layout.
       Image completion arms one post-layout inspection; both a hit and a
       miss consume it. Do not force geometry work merely to produce a log. */
    bool replaced_pending = d->replaced_trace_pending && d->replaced_trace_reported < 2;
    d->replaced_trace_pending = false;
    if (d->clamp_trace_reported < 4 || replaced_pending)
        clamp_trace_report_box(d, d->root_box, replaced_pending);
}

void layout_invalidate(box_t *b) {
    if (!b) return;
    /* A changed box and ancestor aggregates are dirty. Unchanged siblings
       and descendants retain their intrinsic/layout results: their actual
       width/cbh/usedh/edges/font-generation inputs are checked on reuse.
       Inherited style changes dirty each affected child's own box at the
       cascade join; parent size changes naturally miss those input keys. */
    b->layout_cache_valid=false;
    b->layout_prepare_valid = b->layout_post_valid = false;
    b->layout_dirty=true;b->intrinsic_done=false;
    for (b = b->parent; b; b = b->parent) {
        b->layout_dirty = true;
        b->layout_prepare_valid = b->layout_post_valid = false;
        b->intrinsic_done = false;
    }
}
void layout_invalidate_fonts(box_t *b){
    if(!b)return;
    /* A successful font load advances the global metrics generation. Existing
       shaping/min-content borrows are no longer committed measurements, even
       where selecting that face will be deferred to the next cascade join. */
    box_t *root=b;
    for(;;){
        b->layout_cache_valid=false;b->layout_dirty=true;b->intrinsic_done=false;
        b->layout_prepare_valid = b->layout_post_valid = false;
        if(b->first){b=b->first;continue;}
        while(b!=root&&!b->next)b=b->parent;
        if(b==root)break;
        b=b->next;
    }
}

static bool grid_template_has_pct(const struct gtemplate *t) {
    for (int i = 0; t && i < t->n; i++) {
        const struct gtrack *k = &t->t[i];
        if ((k->min_kind == GT_LEN && len_has_pct(&k->min)) ||
            ((k->max_kind == GT_LEN || k->max_kind == GT_FIT) && len_has_pct(&k->max))) return true;
    }
    return false;
}
static bool layout_cache_prepare(box_t *b) {
    if (b->layout_prepare_valid && !b->layout_dirty) return b->layout_cache_safe;
    b->layout_prepare_valid = false;
    if (!layout_checkpoint()) return false;
    b->clamp_hidden = false;
    bool safe = !box_line_clamp(b) && !b->abspos && !(b->node && !b->node->foreign &&
        (b->node->tag == T_frameset || b->node->tag == T_iframe || b->node->tag == T_embed));
    const style_t *s=b->st;
    bool exports_floats=false;
    /* Only consumers of a definite height make an unchanged stretch observable.
       Auto/fr rows and column flex lines given exactly their content height
       distribute zero free space; absolute min/max sizes were clamped by the
       caller. Over-reporting re-lays whole nested grid subtrees at each depth. */
    bool dependent=b->kind==B_TABLE ||
        (b->kind==B_GRID && (grid_template_has_pct(s->grid_rows) || grid_template_has_pct(s->grid_auto_rows))) ||
        len_has_pct(&s->height) || len_has_pct(&s->flex_basis) ||
        len_has_pct(&s->min_height) || len_has_pct(&s->max_height) ||
        len_has_pct(&s->inset[0]) || len_has_pct(&s->inset[2]);
    for (box_t *c = b->first; c; c = c->next) {
        /* A cached parent must not suppress list_abs/static-position work.
           Its unaffected sibling formatting contexts can still be reused. */
        if (!layout_cache_prepare(c)) safe = false;
        /* Reusing a shared, empty BFC must not suppress float registration.
           Floats inside an independent BFC remain contained by that child;
           the child being floated itself still contributes to this BFC. */
        if(!c->abspos&&(c->floated||(!is_bfc_root(c)&&c->layout_cache_exports_floats)))exports_floats=true;
        if (c->stretch_height_dependent) dependent=true;
    }
    b->layout_cache_safe = safe;
    b->layout_cache_exports_floats=exports_floats;
    b->stretch_height_dependent=dependent;
    b->layout_prepare_valid = true;
    return safe;
}

/* Intrinsic contributions of sibling flex/grid subtrees are independent.
 * Placement, float negotiation and ancestor-size reductions remain ordered.
 * Tables, replaced/media controls and embedded grids use the serial path. */
static bool intrinsic_parallel_safe(box_t *b) {
    if (b->abspos || b->floated || b->kind == B_ATOMIC || b->kind == B_TABLE || b->kind == B_GRID ||
        (b->node && b->node->tag == T_frameset)) return false;
    for (box_t *c = b->first; c; c = c->next) if (!intrinsic_parallel_safe(c)) return false;
    return true;
}
struct intrinsic_job { box_t **boxes; struct layout_context context; };
static void intrinsic_job_run(size_t i, void *context) {
    struct intrinsic_job *j = context;
    struct layout_context local = j->context;
    local.pure = true; local.text_cache = NULL;
    void *previous = thread_local_get(1);
    thread_local_set(1, &local);
    float min_width, max_width;
    intrinsic(j->boxes[i], &min_width, &max_width);
    thread_local_set(1, previous);
}
static void intrinsic_service(void *context) { web_native_checkpoint(context); }
static void intrinsic_collect(box_t *b, pvec *jobs) {
    if (b->layout_post_valid && !b->layout_dirty &&
        b->cached_font_generation == font_metrics_generation()) return;
    if (!layout_checkpoint()) return;
    for (box_t *c = b->first; c; c = c->next) {
        if ((b->kind == B_FLEX || b->kind == B_GRID) &&
            (!c->intrinsic_done || c->intrinsic_font_generation != font_metrics_generation()) && intrinsic_parallel_safe(c))
            pv_push(jobs, c);
        else intrinsic_collect(c, jobs);
    }
}
static void intrinsic_parallel_prepare(box_t *root) {
    pvec work = {0};
    intrinsic_collect(root, &work);
    if (work.n >= 8) {
        struct intrinsic_job j = {(box_t **)work.v, *layout_current()};
        parallel_for_stage(PARALLEL_INTRINSIC,(size_t)work.n, intrinsic_job_run, &j, intrinsic_service, D);
    }
    pv_free(&work);
}

/* relative positioning offsets, now that containing blocks have their sizes */
static void relative_offsets(box_t *b) {
    /* Placement in the parent may change while this box's inner layout is
       reused. Refresh its own percentage inset against its actual cb first;
       descendant offsets depend only on the unchanged inner dimensions. */
    if (b->parent) {
        b->rel_dx = b->rel_dy = 0;
        /* a wrapped element moves once, with its outer box */
        if (b->st && b->st->position == POS_RELATIVE && b->kind != B_INLINE && b->kind != B_TEXT && !b->wrapped_inner) {
            const len_t *in = b->st->inset;
            float cw = b->cb ? b->cb->w : VW, ch = b->cb ? b->cb->h : VH;
            if (!len_auto(&in[3])) b->rel_dx = len_resolve(&in[3], cw);
            else if (!len_auto(&in[1])) b->rel_dx = -len_resolve(&in[1], cw);
            if (!len_auto(&in[0])) b->rel_dy = len_has_pct(&in[0]) && !b->cb ? 0 : len_resolve(&in[0], ch);
            else if (!len_auto(&in[2])) b->rel_dy = -len_resolve(&in[2], ch);
        }
    }
    if (b->layout_post_valid && b->layout_cache_valid && b->layout_cache_safe &&
        b->layout_post_w == b->w && b->layout_post_h == b->h &&
        b->layout_post_content_dy == b->content_dy) return;
    if (!layout_checkpoint()) return;
    for (box_t *c = b->first; c; c = c->next) {
        relative_offsets(c);
    }
}

/* ---------------------------------------------------------------- absolute positioning */
static box_t *abs_containing_block(box_t *a) {
    if (web_dialog_layer_box(D,a)) return D->root_box;
    for (box_t *p = a->parent; p; p = p->parent)
        if (box_filter_containing_block(p) ||
            (a->st->position != POS_FIXED && p->st && p->st->position != POS_STATIC && p->kind != B_INLINE && p->kind != B_TEXT)) return p;
    return D->root_box;
}

static void abs_flex_static_position(const box_t *a, float *x, float *y) {
    const box_t *f = a->static_cb;
    if (!f || f->kind != B_FLEX || !f->st) return;
    const style_t *s = f->st;
    bool column = s->flex_direction == FD_COLUMN || s->flex_direction == FD_COLUMN_REVERSE;
    bool reverse = s->flex_direction == FD_ROW_REVERSE || s->flex_direction == FD_COLUMN_REVERSE;
    float mw = a->w + hext(a) + a->m[1] + a->m[3];
    float mh = a->h + vext(a) + a->m[0] + a->m[2];
    float main_space = column ? f->h - mh : f->w - mw;
    float cross_space = column ? f->w - mw : f->h - mh;
    /* CSS Flexbox 4.1: an abspos child's static rectangle uses its sole-item
       margin position in the main axis and the container's content edges in
       the cross axis. Size it first, after flex layout, rather than treating
       it as a zero-sized item or letting siblings participate in placement. */
    float main = 0;
    switch (s->justify_content) {
    case JC_END: main = main_space; break;
    case JC_CENTER: main = main_space / 2; break;
    case JC_AROUND: case JC_EVENLY:
        main = main_space >= 0 ? main_space / 2 : reverse ? main_space : 0; break;
    }
    if (reverse) main = main_space - main;
    int align = a->st->align_self == 255 ? s->align_items : a->st->align_self;
    float cross = align == AI_END ? cross_space : align == AI_CENTER ? cross_space / 2 : 0;
    if (s->flex_wrap == FW_WRAP_REVERSE) cross = cross_space - cross;
    *x += column ? cross : main;
    *y += column ? main : cross;
}

static void layout_abs(box_t *a) {
    if (a->clamp_hidden) return;
    box_t *cb = abs_containing_block(a);
    bool icb = cb == D->root_box;
    float rect[4]; box_abs_cb_rect(cb, D, rect);
    float pl = -rect[0], pt = -rect[1], cbw = rect[2], cbh = rect[3];
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
        float sh = resolved_content_height(a, cbh);
        layout_inner(a, NULL, 0, 0, cbh);
        if (sh >= 0 && a->kind != B_TABLE) a->h = clamp_h(a, sh, cbh);
    }
    /* Publish used auto margins, not just an adjusted paint position. With
       both insets definite the abspos constraint determines either auto
       margin; the horizontal negative-remainder rule follows our LTR layout
       direction, while two vertical auto margins remain equal. */
    if (!la && !ra) {
        bool ml = len_auto(&a->st->margin[3]), mr = len_auto(&a->st->margin[1]);
        float remaining = cbw - l - r - (a->w + ext);
        if (ml && mr) { a->m[3] = remaining >= 0 ? remaining / 2 : 0; a->m[1] = remaining - a->m[3]; }
        else if (ml) a->m[3] = remaining - a->m[1];
        else if (mr) a->m[1] = remaining - a->m[3];
    }
    if (!ta && !ba) {
        bool mt = len_auto(&a->st->margin[0]), mb = len_auto(&a->st->margin[2]);
        float remaining = cbh - t - bo - (a->h + vext(a));
        if (mt && mb) a->m[0] = a->m[2] = remaining / 2;
        else if (mt) a->m[0] = remaining - a->m[2];
        else if (mb) a->m[2] = remaining - a->m[0];
    }
    /* the static position, in the containing block's coordinates */
    float sx = 0, sy = 0;
    if (a->static_cb) {
        sx = box_abs_x(a->static_cb) + a->static_x - (icb ? 0 : box_abs_x(cb));
        sy = box_abs_y(a->static_cb) + a->static_y - (icb ? 0 : box_abs_y(cb) + cb->content_dy);
        abs_flex_static_position(a, &sx, &sy);
    }
    float mw = a->w + ext + a->m[1] + a->m[3];
    float x;
    if (!la) x = -pl + l + a->m[3];
    else if (!ra) x = -pl + cbw - r - mw + a->m[3];
    else x = sx + a->m[3];
    float mh = a->h + vext(a) + a->m[0] + a->m[2];
    float y;
    if (!ta) y = -pt + t + a->m[0];
    else if (!ba) y = -pt + cbh - bo - mh + a->m[0];
    else y = sy + a->m[0];
    a->x = x + a->b[3] + a->p[3];
    a->y = y + a->b[0] + a->p[0];
}

/* ---------------------------------------------------------------- entry */
/* Build each scrolling area's bounds once per layout, never from its current
   scroll position. Propagate through the actual containing block: fixed boxes
   and absolute boxes with an outside containing block do not inflate an
   unrelated intermediate scroller. Clipped children's private overflow does
   not leak into their ancestor's scrolling area. */
static float scroll_extent(float value) {
    return !isfinite(value) || value < 0 ? 0 : value;
}

static int document_extent(float value) {
    /* Compare in double: (float)INT_MAX rounds up to 2147483648. Never cast
       that rounded value, NaN, or infinity to the public API's signed int. */
    double extent=ceil((double)value);
    return !(extent>=0) ? 0 : extent>(double)INT_MAX ? INT_MAX : (int)extent;
}

static void scroll_area_publish(box_t *b) {
    box_t *cb = b->cb;
    if (b->clamp_hidden || !cb || (b->kind == B_INLINE && !box_filter_containing_block(b)) || b->kind == B_TEXT || b->kind == B_BR ||
        box_viewport_fixed(b) || web_dialog_layer_box(D, b)) return;
    /* Native multiline text is confined to the control's padding clip, not
       an outer CSS child box. Its private scroll area must not enlarge an
       ancestor even if the author changes the outer overflow property. */
    bool clipped = b->clamp_truncated || b->atomic == AT_TEXTAREA ||
        (b->st && b->st->overflow != OV_VISIBLE && !doc_viewport_overflow_box(D, b));
    float bw = clipped ? b->w + b->p[1] + b->p[3] : b->scroll_w;
    float bh = clipped ? b->h + b->p[0] + b->p[2] : b->scroll_h;
    /* cb is b's actual coordinate parent. The shared cb chain cancels:
       x(b)-x(cb) = b.x+b.rel_dx; y(b)-y(cb) additionally has cb.content_dy.
       Do not walk that same ancestor chain four times for every box. Local
       coordinates also avoid losing small offsets beside a huge ancestor. */
    /* "Scrollable overflow area" (CSS Overflow 3): b's border box with the overflow it carries,
       and b's margin box followed by cb's end-side padding. A negative margin shrinks only the
       second; a border box of zero area adds nothing of its own. */
    float box_right = b->x + b->rel_dx - b->p[3] + bw + b->b[1] + cb->p[3];
    float box_bottom = b->y + b->rel_dy + cb->content_dy - b->p[0] + bh + b->b[2] + cb->p[0];
    float right = box_right + b->m[1] + cb->p[1];
    float bottom = box_bottom + b->m[2] + cb->p[2];
    if (bw + b->b[1] + b->b[3] > 0 && bh + b->b[0] + b->b[2] > 0) {
        right = fmaxf_(right, box_right);
        bottom = fmaxf_(bottom, box_bottom);
    }
    /* CSS transforms extend, never shrink, the union of original and visual
       overflow. Descendant overflow has already propagated into b; add only
       b's own translation here, not its ancestors a second time. */
    right+=fmaxf_(0,box_translation_axis(b,0,CSS_TRANSLATE_INDIVIDUAL|CSS_TRANSLATE_TRANSFORM));
    bottom+=fmaxf_(0,box_translation_axis(b,1,CSS_TRANSLATE_INDIVIDUAL|CSS_TRANSLATE_TRANSFORM));
    cb->scroll_w = fmaxf_(cb->scroll_w, scroll_extent(right));
    cb->scroll_h = fmaxf_(cb->scroll_h, scroll_extent(bottom));
}

static void scroll_areas(box_t *b) {
    if (b->layout_post_valid && b->layout_post_safe && b->layout_cache_valid &&
        b->layout_post_w == b->w && b->layout_post_h == b->h &&
        b->layout_post_content_dy == b->content_dy) {
        /* Cached bounds are local, so publish the box's new placement into
           the recomputed parent; do not add an old global union twice. */
        box_scroll_clamp(b); scroll_area_publish(b); return;
    }
    b->layout_post_valid = false;
    if (!layout_checkpoint()) return;
    if (b->clamp_hidden) {
        b->scroll_w = b->scroll_h = b->layout_bottom = 0;
        b->layout_post_valid = false; return;
    }
    b->scroll_w = scroll_extent(b->w + b->p[1] + b->p[3]);
    b->scroll_h = scroll_extent(b->h + b->p[0] + b->p[2]);
    if (b->atomic == AT_TEXTAREA && b->node) {
        const char *text = web_input_edit_text(b->node);
        const style_t *st = b->st;
        if (!*text) { text = node_attr(b->node, "placeholder"); if (st->placeholder) st = st->placeholder; }
        if (!text) text = "";
        float width, height;
        if (!control_text_lines(b, st, text, NULL, NULL, &width, &height)) return;
        b->scroll_w = fmaxf_(b->scroll_w, scroll_extent(b->p[3] + width + b->p[1]));
        b->scroll_h = fmaxf_(b->scroll_h, scroll_extent(b->p[0] + height + b->p[2]));
    }
    for (int i = 0; i < b->nruns; i++) {
        if (!(i & 63) && !layout_checkpoint()) return;
        const struct run *r = &b->runs[i];
        if (r->atomic) continue; /* its native box contributes below */
        b->scroll_w = fmaxf_(b->scroll_w, scroll_extent(b->p[3] + r->x + r->w + b->p[1]));
        b->scroll_h = fmaxf_(b->scroll_h, scroll_extent(b->p[0] + b->content_dy + r->y + r->descent + b->p[2]));
    }
    for (int i = 0; i < b->ndecos; i++) {
        if (!(i & 63) && !layout_checkpoint()) return;
        const struct deco *r = &b->decos[i];
        b->scroll_w = fmaxf_(b->scroll_w, scroll_extent(b->p[3] + r->x + r->w + b->p[1]));
        b->scroll_h = fmaxf_(b->scroll_h, scroll_extent(b->p[0] + b->content_dy + r->y + r->h + b->p[2]));
    }
    /* Document bottom and overflow consume the same final geometry. Compute
       both on this postorder walk rather than a second max_bottom tree walk. */
    b->layout_bottom = b->h + b->p[2] + b->b[2];
    bool clipped = b->clamp_truncated || (b->st && b->st->overflow != OV_VISIBLE &&
        b->kind != B_TEXT && !doc_viewport_overflow_box(D, b));
    for (box_t *c = b->first; c; c = c->next) {
        scroll_areas(c);
        if (clipped || c->abspos || c->kind == B_TEXT || c->kind == B_INLINE ||
            c->kind == B_BR || c->cb != b) continue;
        float bottom = b->content_dy;
        if (!c->clamp_hidden) bottom += c->y + c->rel_dy + c->layout_bottom;
        b->layout_bottom = fmaxf_(b->layout_bottom, bottom);
    }
    if (b->clamp_truncated) {
        b->scroll_w = scroll_extent(b->w + b->p[1] + b->p[3]);
        b->scroll_h = scroll_extent(b->h + b->p[0] + b->p[2]);
    }
    box_scroll_clamp(b);
    scroll_area_publish(b);
    b->layout_post_w = b->w; b->layout_post_h = b->h;
    b->layout_post_content_dy = b->content_dy;
    b->layout_post_safe = b->layout_cache_safe;
    b->layout_post_valid = !D->native_cancelled;
}

static float max_bottom(box_t *b, float base) {
    if (b->clamp_hidden) return base;
    return base + b->y + b->rel_dy + b->layout_bottom;
}

/* debug_js only: zero per-box cost, then name the hot chain of a slow layout. */
static void layout_profile_reset(box_t *b) {
    for (; b; b = b->next) { b->profile_cycles = 0; b->profile_calls = 0; layout_profile_reset(b->first); }
}
static void layout_profile_describe(const box_t *b, char *out, size_t size) {
    const node_t *n = b->node;
    const char *cls = n && n->type == N_ELEM ? node_attr((node_t *)n, "class") : NULL;
    snprintf(out, size, "%s%s%.48s kind %d display %d w %.0f h %.0f", n && n->type == N_ELEM ? n->name : b->anon ? "anon" : "text",
        cls ? "." : "", cls ? cls : "", b->kind, b->st ? b->st->display : -1, (double)b->w, (double)b->h);
}
static void layout_profile_report(web_doc *d, box_t *root, uint64_t cycles[5], uint64_t elapsed_ms) {
    uint64_t total = cycles[0] + cycles[1] + cycles[2] + cycles[3] + cycles[4];
    if (!total) return;
    char line[600], name[200];
    snprintf(line, sizeof line, "Layout phases %llu ms: intrinsic %llu%%, flow %llu%%, abspos %llu%% (%d boxes), relative %llu%%, scroll %llu%%",
        (unsigned long long)elapsed_ms, (unsigned long long)(cycles[0] * 100 / total), (unsigned long long)(cycles[1] * 100 / total),
        (unsigned long long)(cycles[2] * 100 / total), d->abs_boxes.n, (unsigned long long)(cycles[3] * 100 / total),
        (unsigned long long)(cycles[4] * 100 / total));
    web_js_console(d, 1, line);
    /* Follow the child holding most of the parent's inclusive cost; print where
       the cost stops concentrating and the boxes laid out most often. */
    const box_t *b = root;
    for (int depth = 0; b && depth < 400; depth++) {
        const box_t *best = NULL; uint64_t sum = 0;
        for (const box_t *c = b->first; c; c = c->next) { sum += c->profile_cycles; if (!best || c->profile_cycles > best->profile_cycles) best = c; }
        bool report = !best || best->profile_cycles * 10 < b->profile_cycles * 6 || b->profile_calls > 2;
        if (report && b->profile_cycles * 20 >= cycles[1]) {
            layout_profile_describe(b, name, sizeof name);
            snprintf(line, sizeof line, "Layout hot depth %d: %s, calls %u, inclusive %llu%%, children %llu%%", depth, name, b->profile_calls,
                (unsigned long long)(b->profile_cycles * 100 / total), (unsigned long long)(sum * 100 / total));
            web_js_console(d, 1, line);
        }
        if (!best || best->profile_cycles * 20 < cycles[1]) break;
        b = best;
    }
    for (int rank = 0; rank < 3; rank++) { /* absolute boxes are laid out outside their ancestors' cost */
        const box_t *top = NULL;
        for (int i = 0; i < d->abs_boxes.n; i++) {
            const box_t *a = d->abs_boxes.v[i];
            if (a->profile_cycles && (!top || a->profile_cycles > top->profile_cycles)) top = a;
        }
        /* When placement dominates, name its top boxes even if each is small. */
        if (!top || (top->profile_cycles * 20 < total && cycles[2] * 2 < total)) break;
        layout_profile_describe(top, name, sizeof name);
        snprintf(line, sizeof line, "Layout hot abspos: %s, calls %u, inclusive %llu%%", name, top->profile_calls,
            (unsigned long long)(top->profile_cycles * 100 / total));
        web_js_console(d, 1, line);
        ((box_t *)top)->profile_cycles = 0; /* report the next one */
    }
}

void layout_doc(web_doc *d, int width, int height) {
    static uint32_t next_generation;
    uint64_t profile_start = d->profile_enabled ? uptime_ms() : 0, phase_cycles[5] = {0}, phase_mark = 0;
#define LAYOUT_PHASE(i) do { if (d->profile_enabled) { uint64_t now_ = __builtin_ia32_rdtsc(); if (phase_mark) phase_cycles[i] += now_ - phase_mark; phase_mark = now_; } } while (0)
    struct layout_context context = {.document = d, .viewport_w = (float)width,
        .viewport_h = (float)height, .generation = ++next_generation};
    if (!context.generation) context.generation = ++next_generation;
    void *previous_context = thread_local_get(1);
    thread_local_set(1, &context);
    ar_free(&d->lmem);
    d->abs_boxes.n = 0;
    box_t *root = d->root_box;
    if (!root) { thread_local_set(1, previous_context); return; }
    /* A document keeps owned measurements between layouts. Same-document
       reentrancy must not age/change the active outer cache; its temporary
       table follows the old per-invocation lifetime, including longjmp. */
    bool temporary_cache = d->layout_text_cache && d->layout_text_cache->busy;
    struct layout_text_cache *cache = temporary_cache ? calloc(1, sizeof *cache) : d->layout_text_cache;
    if (!temporary_cache && !cache) cache = d->layout_text_cache = calloc(1, sizeof *cache);
    layout_text_begin(cache);
    struct layout_text_cache *previous_cache = layout_text_active;
    jmp_buf cleanup_trap;
    jmp_buf *outer_trap = d->lmem.trap;
    layout_text_active = cache;
    d->lmem.trap = &cleanup_trap;
    int arena_failed = setjmp(cleanup_trap);
    if (arena_failed) {
        layout_text_active = previous_cache;
        if (cache) cache->busy = false;
        if (temporary_cache) layout_text_cache_free(cache);
        d->lmem.trap = outer_trap;
        thread_local_set(1, previous_context);
        if (outer_trap) longjmp(*outer_trap, arena_failed);
        abort();
    }
    root->x = root->y = 0;
    root->w = VW;
    root->cb = NULL;
    if (d->profile_enabled) layout_profile_reset(root);
    LAYOUT_PHASE(0);
    layout_cache_prepare(root);
    intrinsic_parallel_prepare(root);
    if (d->native_cancelled) goto finished;
    LAYOUT_PHASE(0);
    layout_inner(root, NULL, 0, 0, VH);
    LAYOUT_PHASE(1);
    if (d->native_cancelled) goto finished;
    for (int i = 0; i < d->abs_boxes.n; i++) {
        box_t *a = d->abs_boxes.v[i];
        if (!d->profile_enabled) { layout_abs(a); continue; }
        /* The whole placement (sizing, containing block, static position), not
           only its inner layout: the hot-abspos report ranks by this. */
        uint64_t start = __builtin_ia32_rdtsc(), inner = a->profile_cycles;
        layout_abs(a);
        a->profile_cycles = inner + (__builtin_ia32_rdtsc() - start);
    }
    LAYOUT_PHASE(2);
    relative_offsets(root);
    LAYOUT_PHASE(3);
    if (d->native_cancelled) goto finished;
    scroll_areas(root);
    LAYOUT_PHASE(4);
    if (d->profile_enabled && uptime_ms() - profile_start >= 250) layout_profile_report(d, root, phase_cycles, uptime_ms() - profile_start);
    if (d->native_cancelled) goto finished;
    float h = fmaxf_(root->h, max_bottom(root, 0));
    /* The containing-block overflow union already includes escaping absolute
       descendants, borders, margins and transforms. A second global absolute
       union leaks a private scrollport's clipped contents into root scrolling. */
    h = fmaxf_(h, root->scroll_h);
    d->doc_h = document_extent(h);
    d->doc_w = width;
finished:
    layout_text_active = previous_cache;
    if (cache) cache->busy = false;
    if (temporary_cache) layout_text_cache_free(cache);
    d->lmem.trap = outer_trap;
    thread_local_set(1, previous_context);
}
