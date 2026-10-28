/* Box generation: turns the styled DOM into a tree of boxes. Inline content that sits next to
   block boxes is wrapped in anonymous blocks, table parts get the anonymous rows and cells they
   need, list items get markers, ::before/::after become boxes, and replaced elements (images,
   form controls, inline SVG) become atomic boxes. */
#include <stdio.h>
#include <limits.h>
#include <nocturne.h>
#include "webi.h"
#include "web_dialog.h"
#include "elements.h"
#include "svg_geometry.h"
#include "avmedia.h"

struct bctx {
    web_doc *d;
    arena_t *a;
    node_t *top;
    uint32_t epoch;
};

static box_t *nbox(struct bctx *b, int kind, node_t *n, style_t *st) {
    box_t *x = ar_alloc(b->a, sizeof *x);
    x->kind = (uint8_t)kind;
    x->node = n;
    x->st = st;
    x->paint_alpha = kind != B_TEXT && kind != B_BR && st && (st->opacity * (st->filter ? st->filter->opacity : 1) < 1 ||
        (kind != B_INLINE && css_transform_general(st)));
    x->baseline = -1;
    x->colspan = x->rowspan = 1;
    x->epoch = b->epoch;
    return x;
}

static void append(box_t *p, box_t *c) {
    c->parent = p;
    c->next = NULL;
    if (p->last) p->last->next = c;
    else p->first = c;
    p->last = c;
    p->paint_alpha |= c->paint_alpha;
}

static style_t *anon_style(struct bctx *b, const style_t *parent, int display) {
    style_t *s = ar_alloc(b->a, sizeof *s);
    css_style_init(s, parent);
    s->display = (uint8_t)display;
    css_style_finish(s, NULL, false);
    s->display = (uint8_t)display;
    return s;
}

bool box_block_level(const box_t *b) {
    if (b->floated || b->abspos) return false;
    switch (b->kind) {
    case B_BLOCK: case B_TABLE: case B_FLEX: case B_GRID: case B_ROW_GROUP: case B_ROW: case B_CELL: case B_CAPTION: return true;
    case B_ATOMIC: {
        int d = b->st->display;
        return d == D_BLOCK || d == D_LIST_ITEM || d == D_FLOW_ROOT || d == D_TABLE || d == D_FLEX || d == D_GRID;
    }
    }
    return false;
}

static bool preserves_space(const style_t *st) {
    return st->white_space == WS_PRE || st->white_space == WS_PRE_WRAP || st->white_space == WS_BREAK_SPACES;
}

/* text that would collapse away between blocks */
static bool ws_only(const box_t *b) {
    if (b->kind != B_TEXT || preserves_space(b->st)) return false;
    for (size_t i = 0; i < b->len; i++)
        if (b->text[i] != ' ' && b->text[i] != '\n') return false;
    return true;
}

/* ---------------------------------------------------------------- text */
static void transform(char *s, size_t n, int tt) {
    bool start = true;
    for (size_t i = 0; i < n; i++) {
        unsigned char c = (unsigned char)s[i];
        bool up = tt == TT_UPPER || (tt == TT_CAPITALIZE && start);
        if (c < 0x80) {
            if (up && c >= 'a' && c <= 'z') s[i] = (char)(c - 32);
            else if (tt == TT_LOWER && c >= 'A' && c <= 'Z') s[i] = (char)(c + 32);
            start = c == ' ' || c == '\n' || c == '-' || c == '(' || c == '"';
        } else {
            if (c == 0xC3 && i + 1 < n) { /* Latin-1 letters */
                unsigned char d = (unsigned char)s[i + 1];
                if (up && d >= 0xA0 && d <= 0xBE && d != 0xB7) s[i + 1] = (char)(d - 0x20);
                else if (tt == TT_LOWER && d >= 0x80 && d <= 0x9E && d != 0x97) s[i + 1] = (char)(d + 0x20);
            }
            if ((c & 0xC0) != 0x80) start = false;
        }
    }
}

/* white-space processing and text-transform. With source, also the byte of s each output
   byte came from (source has room for one entry per output byte and one for the end, which
   is n): the one record of how a text box's text corresponds to its Text node's data. */
static const char *ws_process(arena_t *a, const char *s, size_t n, const style_t *st, size_t *out_n,
                              uint32_t *source) {
    char *o = ar_alloc(a, n * 8 + 1); /* tabs may expand */
    size_t k = 0;
    int ws = st->white_space;
#define WS_EMIT(c, from) do { if (source) source[k] = (uint32_t)(from); o[k++] = (c); } while (0)
    if (ws == WS_PRE || ws == WS_PRE_WRAP || ws == WS_BREAK_SPACES) {
        int col = 0;
        for (size_t i = 0; i < n; i++) {
            char c = s[i];
            if (c == '\t') {
                int sp = 8 - col % 8;
                for (int j = 0; j < sp; j++) WS_EMIT(' ', i);
                col += sp;
            } else {
                WS_EMIT(c, i);
                col = c == '\n' ? 0 : col + 1;
            }
        }
    } else {
        bool keep_nl = ws == WS_PRE_LINE;
        bool space = false;
        for (size_t i = 0; i < n; i++) {
            char c = s[i];
            if (c == '\n' && keep_nl) {
                while (k > 0 && o[k - 1] == ' ') k--;
                WS_EMIT('\n', i);
                space = true; /* drops the spaces after it */
                continue;
            }
            if (c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f') {
                if (!space) WS_EMIT(' ', i);
                space = true;
                continue;
            }
            /* soft hyphens are invisible */
            if ((unsigned char)c == 0xC2 && i + 1 < n && (unsigned char)s[i + 1] == 0xAD) {
                i++;
                continue;
            }
            WS_EMIT(c, i);
            space = false;
        }
    }
#undef WS_EMIT
    o[k] = 0;
    if (source) source[k] = (uint32_t)n;
    /* In place and byte for byte: the source map stays exact. */
    if (st->text_transform) transform(o, k, st->text_transform);
    *out_n = k;
    return o;
}

uint32_t *box_text_source_map(arena_t *a, const box_t *t) {
    const node_t *n = t ? t->node : NULL;
    if (!n || n->type != N_TEXT || t->kind != B_TEXT || n->box != t || !t->st || !t->text ||
        n->textlen > UINT32_MAX - 1 || t->len > SIZE_MAX / sizeof(uint32_t) - 1) return NULL;
    /* t->len is what this data made when the box was built; a different result means a
       text change the next layout has not published yet. */
    uint32_t *source = ar_alloc(a, (t->len + 1) * sizeof *source);
    size_t length = 0;
    const char *text = ws_process(a, n->text ? n->text : "", n->textlen, t->st, &length, NULL);
    if (length != t->len || memcmp(text, t->text, length)) return NULL;
    ws_process(a, n->text ? n->text : "", n->textlen, t->st, &length, source);
    return source;
}

/* ---------------------------------------------------------------- list markers */
static void roman(char *out, int v, bool upper) {
    static const int val[] = {1000, 900, 500, 400, 100, 90, 50, 40, 10, 9, 5, 4, 1};
    static const char *const sym[] = {"m", "cm", "d", "cd", "c", "xc", "l", "xl", "x", "ix", "v", "iv", "i"};
    out[0] = 0;
    if (v <= 0 || v >= 4000) {
        sprintf(out, "%d", v);
        return;
    }
    for (int i = 0; i < 13; i++)
        while (v >= val[i]) {
            strcat(out, sym[i]);
            v -= val[i];
        }
    if (upper)
        for (char *p = out; *p; p++) *p = (char)(*p - 32);
}

static void alpha(char *out, int v, bool upper) {
    if (v <= 0) {
        sprintf(out, "%d", v);
        return;
    }
    char t[16];
    int n = 0;
    while (v > 0 && n < 15) {
        v--;
        t[n++] = (char)((upper ? 'A' : 'a') + v % 26);
        v /= 26;
    }
    for (int i = 0; i < n; i++) out[i] = t[n - 1 - i];
    out[n] = 0;
}

static bool is_li(node_t *n) {
    if (n->type != N_ELEM || !n->style || n->style->display != D_LIST_ITEM) return false;
    for (node_t *c = n, *p; c; c = p) {
        if (c->style && c->style->display == D_NONE) return false;
        p = doc_flat_parent(c);
        if (p && !p->foreign && p->tag == T_details && !node_attr(p, "open") && doc_details_summary(p) != c)
            return false;
    }
    return true;
}

/* Unlike atoi, HTML integer parsing distinguishes invalid/missing values from
   a real zero and never overflows on author-provided attribute strings. */
static bool list_integer(const char *s, int *out) {
    if (!s) return false;
    while (is_space((unsigned char)*s)) s++;
    bool negative = *s == '-';
    if (*s == '-' || *s == '+') s++;
    if (*s < '0' || *s > '9') return false;
    uint64_t value = 0, limit = negative ? 2147483648ull : 2147483647ull;
    while (*s >= '0' && *s <= '9') {
        unsigned digit = (unsigned)(*s++ - '0');
        if (value > (limit - digit) / 10) return false;
        value = value * 10 + digit;
    }
    *out = negative ? (int)(-(int64_t)value) : (int)value;
    return true;
}

static node_t *list_owner(node_t *item) {
    node_t *ancestor = item->parent;
    for (node_t *p = item->parent; p; p = p->parent)
        if (!p->foreign && (p->tag == T_ol || p->tag == T_ul || p->tag == T_menu)) { ancestor = p; break; }
    for (; ancestor; ancestor = ancestor->parent)
        if (ancestor->style && ancestor->style->display != D_NONE && ancestor->style->display != D_CONTENTS)
            return ancestor;
    return NULL;
}

static int owned_count(node_t *subtree, node_t *owner) {
    int count = is_li(subtree) && list_owner(subtree) == owner ? 1 : 0;
    for (node_t *n = subtree->first; n; n = n->next) count += owned_count(n, owner);
    return count;
}

static bool owned_ordinal(node_t *subtree, node_t *owner, node_t *item, int step, int64_t *number) {
    if (is_li(subtree) && list_owner(subtree) == owner) {
        int explicit_value;
        if (!subtree->foreign && subtree->tag == T_li && list_integer(node_attr(subtree, "value"), &explicit_value))
            *number = explicit_value;
        if (subtree == item) return true;
        *number += step;
    }
    for (node_t *n = subtree->first; n; n = n->next)
        if (owned_ordinal(n, owner, item, step, number)) return true;
    return false;
}

/* The owning list, not merely siblings, supplies reversed/start/value order.
   Nested lists are independent; display:none items never affect numbering. */
static int list_ordinal(node_t *li) {
    node_t *owner = list_owner(li);
    if (!owner) return 1;
    bool ordered = !owner->foreign && owner->tag == T_ol;
    bool reversed = ordered && node_attr(owner, "reversed");
    int start;
    if (!ordered || !list_integer(node_attr(owner, "start"), &start)) start = reversed ? owned_count(owner, owner) : 1;
    int64_t number = start;
    owned_ordinal(owner, owner, li, reversed ? -1 : 1, &number);
    return number < -2147483648ll ? -2147483647 - 1 : number > 2147483647ll ? 2147483647 : (int)number;
}

static void make_marker(struct bctx *b, box_t *x, node_t *n, style_t *st) {
    char buf[64];
    int ls = st->list_style;
    if (ls == LS_NONE) return;
    if (ls == LS_DISC || ls == LS_CIRCLE || ls == LS_SQUARE) {
        x->marker_shape = (uint8_t)(ls == LS_DISC ? 1 : ls == LS_CIRCLE ? 2 : 3);
        x->marker = ls == LS_DISC ? "\xe2\x80\xa2 " : ls == LS_CIRCLE ? "\xe2\x97\xa6 " : "\xe2\x96\xaa ";
        return;
    }
    if (ls == LS_STRING) {
        x->marker = st->list_style_string ? st->list_style_string : "";
        return;
    }
    int v = list_ordinal(n);
    char num[48];
    switch (ls) {
    case LS_DECIMAL_LZ: sprintf(num, "%02d", v); break;
    case LS_LOWER_ALPHA: alpha(num, v, false); break;
    case LS_UPPER_ALPHA: alpha(num, v, true); break;
    case LS_LOWER_ROMAN: roman(num, v, false); break;
    case LS_UPPER_ROMAN: roman(num, v, true); break;
    case LS_LOWER_GREEK:
        if (v >= 1 && v <= 24) {
            char g[4];
            g[utf8_put(g, 0x3B1 + (uint32_t)(v - 1) + (v > 17 ? 1 : 0))] = 0;
            sprintf(num, "%s", g);
        } else sprintf(num, "%d", v);
        break;
    default: sprintf(num, "%d", v);
    }
    snprintf(buf, sizeof buf, "%s. ", num);
    x->marker = ar_strdup(b->a, buf);
}

/* ---------------------------------------------------------------- inline SVG */
static node_t *find_id(node_t *n, const char *id) {
    for (node_t *c = n->first; c;) {
        if (c->type == N_ELEM) {
            if (c->id && !strcmp(c->id, id)) return c;
            if (c->first) { c = c->first; continue; }
        }
        while (c != n && !c->next) c = c->parent;
        if (c == n) break;
        c = c->next;
    }
    return NULL;
}

static void xml_escape(sbuf *b, const char *s, size_t n, bool attr) {
    for (size_t i = 0; i < n; i++) {
        char c = s[i];
        if (c == '&') sb_puts(b, "&amp;");
        else if (c == '<') sb_puts(b, "&lt;");
        else if (c == '>') sb_puts(b, "&gt;");
        else if (c == '"' && attr) sb_puts(b, "&quot;");
        else sb_putc(b, c);
    }
}

static void svg_paint_value(sbuf *b, const char *name, const struct svg_paint *paint, uint32_t current) {
    sb_puts(b, name); sb_putc(b, ':');
    if (paint->kind == SVG_PAINT_NONE) sb_puts(b, "none");
    else if (paint->kind == SVG_PAINT_REF) {
        sb_puts(b, "url("); xml_escape(b, paint->ref, strlen(paint->ref), true); sb_putc(b, ')');
    } else {
        uint32_t c = paint->color == COLOR_CURRENT ? current : paint->color;
        char value[16]; snprintf(value, sizeof value, "#%06x", c & 0xffffff);
        sb_puts(b, value);
    }
    sb_putc(b, ';');
}

static void svg_computed_style(sbuf *b, node_t *n, bool root) {
    const style_t *st = n->style;
    if (!st) return;
    sb_puts(b, " style=\"");
    const char *original = n->csp_style_attribute == 2 ? NULL : node_attr(n, "style");
    /* Preserve codec-supported properties not yet represented by native CSS
       (e.g. stroke dashes), then override only our computed painting subset. */
    if (original) xml_escape(b, original, strlen(original), true);
    sb_putc(b, ';');
    svg_paint_value(b, "fill", &st->svg_fill, st->color);
    svg_paint_value(b, "stroke", &st->svg_stroke, st->color);
    uint32_t fc = st->svg_fill.color == COLOR_CURRENT ? st->color : st->svg_fill.color;
    uint32_t sc = st->svg_stroke.color == COLOR_CURRENT ? st->color : st->svg_stroke.color;
    float fa = st->svg_fill.kind == SVG_PAINT_COLOR ? (float)(fc >> 24) / 255 : 1;
    float sa = st->svg_stroke.kind == SVG_PAINT_COLOR ? (float)(sc >> 24) / 255 : 1;
    uint32_t stop = st->svg_stop_color == COLOR_CURRENT ? st->color : st->svg_stop_color;
    char value[256];
    static const char *const caps[] = {"butt", "round", "square"};
    static const char *const joins[] = {"miter", "round", "bevel"};
    char width[48];
    snprintf(width, sizeof width, "%g%s", (double)(st->svg_stroke_width.pct ? st->svg_stroke_width.pct : st->svg_stroke_width.px), st->svg_stroke_width.pct ? "%" : "");
    snprintf(value, sizeof value,
        "fill-opacity:%g;stroke-opacity:%g;stroke-width:%s;fill-rule:%s;stroke-linecap:%s;stroke-linejoin:%s;stop-color:#%06x;stop-opacity:%g;",
        (double)(fa * st->svg_fill_opacity), (double)(sa * st->svg_stroke_opacity), width,
        st->svg_fill_rule ? "evenodd" : "nonzero", caps[st->svg_stroke_cap < 3 ? st->svg_stroke_cap : 0],
        joins[st->svg_stroke_join < 3 ? st->svg_stroke_join : 0], stop & 0xffffff,
        (double)(st->svg_stop_opacity * (float)(stop >> 24) / 255));
    sb_puts(b, value);
    /* Root opacity is composited once by paint.c, outside this atomic image. */
    if (root) sb_puts(b, "opacity:1;");
    sb_putc(b, '"');
}

/* ---- CSS transforms of SVG graphics (no CSS layout box): the reference box
   is the nearest viewport's view box, or the object bounding box for
   fill-box/stroke-box. Lengths are user units; a unit suffix is ignored. */
static float svg_attr_number(node_t *n, const char *name, float fallback) {
    const char *v = node_attr(n, name); float out;
    if (!v) return fallback;
    while (svg_space(*v)) v++;
    return svg_number(&v, &out) ? out : fallback;
}
static void svg_bbox_add(float box[4], bool *any, float x, float y) {
    if (!*any) { box[0] = box[2] = x; box[1] = box[3] = y; *any = true; return; }
    box[0] = fminf(box[0], x); box[1] = fminf(box[1], y); box[2] = fmaxf(box[2], x); box[3] = fmaxf(box[3], y);
}
static bool svg_list_number(const char **p, float *v) {
    while (svg_space(**p) || **p == ',') (*p)++;
    return svg_number(p, v);
}
/* Bounds of a path's end and control points: never smaller than the curve. */
static void svg_path_bbox(const char *d, float box[4], bool *any) {
    static const char commands[] = "MLHVCSQTAZ";
    static const unsigned char counts[] = {2, 2, 1, 1, 6, 4, 4, 2, 7, 0};
    float cx = 0, cy = 0, sx = 0, sy = 0;
    char command = 0;
    for (const char *p = d; p && *p;) {
        while (svg_space(*p) || *p == ',') p++;
        if (!*p) break;
        char upper = (char)(*p >= 'a' && *p <= 'z' ? *p - 32 : *p);
        const char *slot = strchr(commands, upper);
        if (slot && *slot) { command = *p++; if (upper == 'Z') { cx = sx; cy = sy; command = 0; } continue; }
        if (!command) break;
        char kind = (char)(command >= 'a' && command <= 'z' ? command - 32 : command);
        bool relative = command != kind;
        unsigned count = counts[strchr(commands, kind) - commands];
        float a[7];
        for (unsigned i = 0; i < count; i++) if (!svg_list_number(&p, &a[i])) return;
        float ox = relative ? cx : 0, oy = relative ? cy : 0;
        if (kind == 'H') { cx = a[0] + ox; svg_bbox_add(box, any, cx, cy); continue; }
        if (kind == 'V') { cy = a[0] + oy; svg_bbox_add(box, any, cx, cy); continue; }
        unsigned first = kind == 'A' ? 5 : 0;
        for (unsigned i = first; i + 1 < count; i += 2) svg_bbox_add(box, any, a[i] + ox, a[i + 1] + oy);
        cx = a[count - 2] + ox; cy = a[count - 1] + oy;
        if (kind == 'M') { sx = cx; sy = cy; command = relative ? 'l' : 'L'; }
    }
}
static void svg_fill_bbox(node_t *n, float box[4], bool *any, unsigned depth) {
    if (!n || n->type != N_ELEM || depth > 32) return;
    const char *name = n->name;
    if (!strcmp(name, "rect") || !strcmp(name, "image") || !strcmp(name, "use") || !strcmp(name, "foreignobject")) {
        float x = svg_attr_number(n, "x", 0), y = svg_attr_number(n, "y", 0);
        svg_bbox_add(box, any, x, y);
        svg_bbox_add(box, any, x + svg_attr_number(n, "width", 0), y + svg_attr_number(n, "height", 0));
    } else if (!strcmp(name, "circle") || !strcmp(name, "ellipse")) {
        float x = svg_attr_number(n, "cx", 0), y = svg_attr_number(n, "cy", 0);
        float rx = svg_attr_number(n, name[0] == 'c' ? "r" : "rx", 0), ry = svg_attr_number(n, name[0] == 'c' ? "r" : "ry", 0);
        svg_bbox_add(box, any, x - rx, y - ry); svg_bbox_add(box, any, x + rx, y + ry);
    } else if (!strcmp(name, "line")) {
        svg_bbox_add(box, any, svg_attr_number(n, "x1", 0), svg_attr_number(n, "y1", 0));
        svg_bbox_add(box, any, svg_attr_number(n, "x2", 0), svg_attr_number(n, "y2", 0));
    } else if (!strcmp(name, "polygon") || !strcmp(name, "polyline")) {
        const char *p = node_attr(n, "points"); float x, y;
        while (p && svg_list_number(&p, &x) && svg_list_number(&p, &y)) svg_bbox_add(box, any, x, y);
    } else if (!strcmp(name, "path")) svg_path_bbox(node_attr(n, "d"), box, any);
    else if (!strcmp(name, "g") || !strcmp(name, "a") || !strcmp(name, "switch"))
        for (node_t *c = n->first; c; c = c->next) svg_fill_bbox(c, box, any, depth + 1);
}
/* The used CSS transform of an SVG graphic as a transform attribute value.
   It replaces the transform presentation attribute (CSS Transforms 1, 4). */
static bool svg_css_transform(node_t *n, char out[160]) {
    const style_t *st = n->style;
    if (!st || !css_translation_active(st)) return false;
    float ref[4] = {0, 0, 0, 0};
    if (st->transform_box == TB_FILL_BOX || st->transform_box == TB_STROKE_BOX) {
        bool any = false; float box[4];
        svg_fill_bbox(n, box, &any, 0);
        if (any) { ref[0] = box[0]; ref[1] = box[1]; ref[2] = box[2] - box[0]; ref[3] = box[3] - box[1]; }
        if (any && st->transform_box == TB_STROKE_BOX && st->svg_stroke.kind != SVG_PAINT_NONE && !st->svg_stroke_width.pct) {
            float half = st->svg_stroke_width.px / 2;
            ref[0] -= half; ref[1] -= half; ref[2] += 2 * half; ref[3] += 2 * half;
        }
    } else {
        node_t *v = n->parent;
        while (v && !(v->type == N_ELEM && v->namespace_id == NS_SVG && v->tag == T_svg)) v = v->parent;
        if (v && svg_viewbox(node_attr(v, "viewBox"), ref) != 1) {
            ref[0] = ref[1] = 0;
            ref[2] = svg_attr_number(v, "width", v->box ? v->box->w : 0);
            ref[3] = svg_attr_number(v, "height", v->box ? v->box->h : 0);
        }
    }
    double m[6];
    if (!css_style_transform_matrix(st, ref[0], ref[1], ref[2], ref[3], m)) return false;
    snprintf(out, 160, "matrix(%.6g %.6g %.6g %.6g %.6g %.6g)", m[0], m[1], m[2], m[3], m[4], m[5]);
    return true;
}

static void svg_open(sbuf *b, node_t *n, const char *color, int view, bool root, bool computed) {
    sb_putc(b, '<');
    sb_puts(b, n->raw_name);
    bool xmlns = false;
    char own_color[16];
    if (computed && n->style) {
        snprintf(own_color, sizeof own_color, "#%06x", n->style->color & 0xffffff);
        color = own_color;
    }
    /* The outermost <svg> is transformed as a CSS box by the painter. */
    char css_transform[160];
    bool transformed = computed && !root && svg_css_transform(n, css_transform);
    for (int i = 0; i < n->nattrs; i++) {
        const char *attribute_name = n->attrs[i].raw;
        const char *ns = n->attrs[i].namespace_uri, *local = n->attrs[i].local;
        /* The codec recognizes literal prefixes, not DOM namespaces. A colon
           in an ordinary DOM attribute must not fabricate an XML/XLink one. */
        if (!ns && strchr(attribute_name, ':')) continue;
        if (computed && n->style && !ns && !strcmp(attribute_name, "style")) continue;
        if (transformed && !ns && !strcmp(n->attrs[i].name, "transform")) continue;
        /* Lexbor already adjusted parsed SVG names. Preserve DOM spelling and
           never serialize arbitrary namespaced fill/viewBox as ordinary SVG. */
        if (ns) {
            if (!strcmp(ns, "http://www.w3.org/1999/xlink") && local && !strcmp(local, "href")) attribute_name = "xlink:href";
            else if (!strcmp(ns, "http://www.w3.org/XML/1998/namespace") && local && !strcmp(local, "lang")) attribute_name = "xml:lang";
            else if (!strcmp(ns, "http://www.w3.org/XML/1998/namespace") && local && !strcmp(local, "space")) attribute_name = "xml:space";
            else if (strcmp(ns, "http://www.w3.org/2000/xmlns/")) continue;
        }
        if (!strcmp(n->attrs[i].name, "viewbox") && view == 0 && n->tag == T_svg) continue;
        const char *v = n->attrs[i].value;
        if (!strcmp(n->attrs[i].name, "xmlns")) xmlns = true;
        sb_putc(b, ' ');
        sb_puts(b, attribute_name);
        sb_puts(b, "=\"");
        /* currentColor is resolved here; nanosvg does not know it */
        for (const char *p = v; *p;) {
            const char *cc = strstr(p, "currentColor");
            if (!cc) cc = strstr(p, "currentcolor");
            if (!cc) {
                xml_escape(b, p, strlen(p), true);
                break;
            }
            xml_escape(b, p, (size_t)(cc - p), true);
            sb_puts(b, color);
            p = cc + 12;
        }
        sb_putc(b, '"');
    }
    if (transformed) { sb_puts(b, " transform=\""); sb_puts(b, css_transform); sb_putc(b, '"'); }
    if (computed) svg_computed_style(b, n, root);
    if (root) {
        if (!xmlns) sb_puts(b, " xmlns=\"http://www.w3.org/2000/svg\"");
        if (!node_attr(n, "fill")) {
            sb_puts(b, " fill=\"");
            sb_puts(b, "black");
            sb_puts(b, "\"");
        }
    }
    sb_putc(b, '>');
}

struct svg_walk {
    struct svg_walk *parent;
    node_t *node, *next;
    unsigned kind; /* 0=entry, 1=element, 2=use group, 3=virtual symbol */
    bool instance; /* virtual use descendants retain their instance inheritance */
};

static bool svg_use_cycle(const struct svg_walk *f, node_t *target) {
    /* Reject actual shadow-including ancestor references as well as a virtual
       use-instance ancestor. A sibling can refer to the same graphic again. */
    for (node_t *n = f->node; n; n = n->parent ? n->parent : n->shadow_host)
        if (n == target) return true;
    for (; f; f = f->parent) if (f->node == target) return true;
    return false;
}

static bool svg_ser(web_doc *d, sbuf *b, node_t *root, const char *color) {
    (void)d;
    struct svg_walk *active = calloc(1, sizeof *active);
    if (!active) return false;
    active->node = root;
    bool failed = false;
    while (active) {
        struct svg_walk *f = active;
        node_t *n = f->node;
        if (!f->kind) {
            if (n->type == N_TEXT) xml_escape(b, n->text, n->textlen, false);
            if (n->type != N_ELEM) goto pop;
            if (!f->instance && n->style && n->style->display == D_NONE) goto pop;
            float viewbox[4];
            int view = n->tag == T_svg ? svg_viewbox(node_attr(n, "viewBox"), viewbox) : 0;
            if (view == 2) goto pop;
            if (!strcmp(n->name, "use")) {
                const char *h = node_attr(n, "href");
                if (!h) {
                    int index = doc_attr_index(n, "http://www.w3.org/1999/xlink", "href", true);
                    if (index >= 0) h = n->attrs[index].value;
                }
                node_t *scope = doc_node_root(n, false);
                node_t *t = h && h[0] == '#' && scope ? find_id(scope, h + 1) : NULL;
                if (!t || svg_use_cycle(f, t)) goto pop;
                struct svg_walk *child = calloc(1, sizeof *child);
                if (!child) { failed = true; break; }
                child->parent = f; child->node = t;
                child->instance = true;
                if (!strcmp(t->name, "symbol")) { child->kind = 3; child->next = t->first; }
                sb_puts(b, "<g");
                const char *x = node_attr(n, "x"), *y = node_attr(n, "y");
                /* The use element's transform precedes its x/y translation. */
                char css_transform[160];
                bool transformed = !f->instance && svg_css_transform(n, css_transform);
                const char *own = transformed ? css_transform : node_attr(n, "transform");
                if (x || y || own) {
                    sb_puts(b, " transform=\"");
                    if (own) { xml_escape(b, own, strlen(own), true); sb_putc(b, ' '); }
                    sb_puts(b, "translate(");
                    xml_escape(b, x ? x : "0", strlen(x ? x : "0"), true);
                    sb_putc(b, ' ');
                    xml_escape(b, y ? y : "0", strlen(y ? y : "0"), true);
                    sb_puts(b, ")\"");
                }
                const char *fill = node_attr(n, "fill");
                if (fill) {
                    const char *value = strstr(fill, "currentColor") ? color : fill;
                    sb_puts(b, " fill=\""); xml_escape(b, value, strlen(value), true); sb_putc(b, '"');
                }
                if (!f->instance) svg_computed_style(b, n, false);
                sb_putc(b, '>'); f->kind = 2; active = child;
                continue;
            }
            svg_open(b, n, color, view, !f->parent, !f->instance);
            f->kind = 1; f->next = n->first;
        }
        if (f->next) {
            struct svg_walk *child = calloc(1, sizeof *child);
            if (!child) { failed = true; break; }
            child->node = f->next; child->parent = f;
            child->instance = f->instance;
            f->next = f->next->next; active = child;
            continue;
        }
        if (f->kind == 1) { sb_puts(b, "</"); sb_puts(b, n->raw_name); sb_putc(b, '>'); }
        else if (f->kind == 2) sb_puts(b, "</g>");
pop:
        active = f->parent; free(f);
    }
    while (active) { struct svg_walk *parent = active->parent; free(active); active = parent; }
    return !failed;
}

struct svg_cache *doc_svg(web_doc *d, node_t *svg, uint32_t color) {
    svg->svg_snapshot_dirty = false;
    struct svg_cache *cached = NULL;
    for (int i = 0; i < d->svgs.n; i++) {
        struct svg_cache *c = d->svgs.v[i];
        if (c->node == svg) { cached = c; break; }
    }
    char hex[16];
    snprintf(hex, sizeof hex, "#%06x", color & 0xFFFFFF);
    sbuf b = {0};
    if (!svg_ser(d, &b, svg, hex)) {
        sb_free(&b); web_js_console(d, 2, "SVG serialization traversal allocation failed"); return NULL;
    }
    if (cached) {
        if (cached->n == b.n && (!b.n || !memcmp(cached->src, b.p, b.n))) {
            cached->used = true;
            sb_free(&b); return cached;
        }
        char *source = b.p ? b.p : strdup("");
        if (!source) { web_js_console(d, 2, "SVG source allocation failed"); return NULL; }
        free(cached->src);
        if (cached->img) image_free(cached->img);
        cached->src = source; cached->n = b.n; cached->img = NULL;
        cached->w = cached->h = 0;
        cached->used = true;
        return cached;
    }
    struct svg_cache *c = calloc(1, sizeof *c);
    if (!c) { sb_free(&b); web_js_console(d, 2, "SVG cache allocation failed"); return NULL; }
    c->node = svg;
    c->used = true;
    c->n = b.n;
    c->src = b.p ? b.p : strdup("");
    pv_push(&d->svgs, c);
    return c;
}

bool doc_svg_clip_markup(node_t *n, const char *ref, const float box[4], sbuf *out) {
    web_doc *d = n ? n->owner : NULL;
    const char *fragment = ref ? strchr(ref, '#') : NULL;
    if (!d || !fragment || !fragment[1]) return false;
    if (fragment != ref) { /* this document's own URL with a fragment names one of its elements */
        size_t prefix = (size_t)(fragment - ref), own = d->url ? strcspn(d->url, "#") : 0;
        if (!d->url || prefix != own || memcmp(ref, d->url, prefix)) return false;
    }
    node_t *scope = doc_node_root(n, false);
    node_t *clip = scope ? find_id(scope, fragment + 1) : NULL;
    if (!clip || !clip->foreign || strcmp(clip->name, "clippath")) return false;
    const char *units = node_attr(clip, "clipPathUnits");
    bool bbox = units && !strcmp(units, "objectBoundingBox");
    char head[320];
    snprintf(head, sizeof head, "<svg xmlns=\"http://www.w3.org/2000/svg\" width=\"%.9g\" height=\"%.9g\"><g transform=\"matrix(%.9g 0 0 %.9g %.9g %.9g)",
             (double)box[2], (double)box[3], bbox ? (double)box[2] : 1.0, bbox ? (double)box[3] : 1.0, (double)box[0], (double)box[1]);
    sb_puts(out, head);
    char css_transform[160];
    const char *own = svg_css_transform(clip, css_transform) ? css_transform : node_attr(clip, "transform");
    if (own) { sb_putc(out, ' '); xml_escape(out, own, strlen(own), true); }
    sb_puts(out, "\">");
    const char *clip_rule = node_attr(clip, "clip-rule");
    static const char *const shapes[] = {"rect", "circle", "ellipse", "polygon", "polyline", "path", NULL};
    static const char *const geometry[] = {"x", "y", "width", "height", "rx", "ry", "cx", "cy", "r", "points", "d", NULL};
    for (node_t *c = clip->first; c; c = c->next) {
        if (c->type != N_ELEM || !c->foreign || (c->style && (c->style->display == D_NONE || c->style->visibility))) continue;
        int shape = 0;
        while (shapes[shape] && strcmp(c->name, shapes[shape])) shape++;
        if (!shapes[shape]) continue; /* <text> and <use> do not clip yet */
        sb_putc(out, '<'); sb_puts(out, shapes[shape]);
        for (int i = 0; geometry[i]; i++) {
            const char *v = node_attr(c, geometry[i]);
            if (!v) continue;
            sb_putc(out, ' '); sb_puts(out, geometry[i]); sb_puts(out, "=\""); xml_escape(out, v, strlen(v), true); sb_putc(out, '"');
        }
        const char *transform = svg_css_transform(c, css_transform) ? css_transform : node_attr(c, "transform");
        if (transform) { sb_puts(out, " transform=\""); xml_escape(out, transform, strlen(transform), true); sb_putc(out, '"'); }
        const char *rule = node_attr(c, "clip-rule");
        if (!rule) rule = clip_rule;
        sb_puts(out, rule && !strcmp(rule, "evenodd") ? " fill=\"#000\" stroke=\"none\" fill-rule=\"evenodd\"/>" :
                                                        " fill=\"#000\" stroke=\"none\"/>");
    }
    sb_puts(out, "</g></svg>");
    return true;
}

/* ---------------------------------------------------------------- generation */
static int atomic_kind(node_t *n) {
    const char *t;
    switch (n->tag) {
    case T_img: return AT_IMG;
    case T_input:
        t = node_attr(n, "type");
        if (!t) return AT_INPUT;
        if (str_ieq(t, "checkbox")) return AT_CHECKBOX;
        if (str_ieq(t, "radio")) return AT_RADIO;
        if (str_ieq(t, "submit") || str_ieq(t, "reset") || str_ieq(t, "button") || str_ieq(t, "file") ||
            str_ieq(t, "color"))
            return AT_BUTTON_INPUT;
        if (str_ieq(t, "image")) return AT_IMG;
        if (str_ieq(t, "range")) return AT_PLACEHOLDER;
        return AT_INPUT;
    case T_select: return AT_SELECT;
    case T_textarea: return AT_TEXTAREA;
    case T_button:
        /* Button layout preserves an author's flex/grid formatting context.
           Native activation belongs to the element, not to a forced flow box. */
        if(n->style && (n->style->display==D_FLEX || n->style->display==D_INLINE_FLEX ||
                       n->style->display==D_GRID || n->style->display==D_INLINE_GRID))return AT_NONE;
        return AT_INLINE_BLOCK;
    case T_svg: return n->parent && n->parent->foreign ? AT_NONE : AT_SVG;
    /* No object resource renderer is installed. Resource-less and unsupported
       objects therefore render their fallback children in ordinary CSS flow,
       rather than swallowing them in a synthetic 300x150 replaced box. */
    case T_object: return AT_NONE;
    case T_iframe: case T_frame: case T_video: case T_canvas: case T_embed: case T_applet: case T_meter:
    case T_progress:
        return AT_PLACEHOLDER;
    case T_audio: return node_attr(n, "controls") ? AT_PLACEHOLDER : AT_NONE;
    }
    return AT_NONE;
}
bool box_pseudo_generated(node_t *node, const style_t *origin, const style_t *pseudo) {
    if (!node || !origin || !pseudo || origin->display == D_NONE ||
        pseudo->display == D_NONE || !pseudo->content) return false;
    if (node->tag == T_br || node->tag == T_wbr || node->tag == T_hr || node->tag == T_frameset) return false;
    int atomic = atomic_kind(node);
    return atomic == AT_NONE || atomic == AT_INLINE_BLOCK;
}

static void gen(struct bctx *b, box_t *pb, node_t *n, style_t *pst);
static void fixup(struct bctx *b, box_t *x);

static void text_box(struct bctx *b, box_t *pb, node_t *n, const char *s, size_t len, style_t *st) {
    size_t k;
    const char *t = ws_process(b->a, s, len, st, &k, NULL);
    if (!k) return;
    box_t *x = nbox(b, B_TEXT, n, st);
    x->text = t;
    x->len = k;
    if (n && n->type == N_TEXT) n->box = x;
    append(pb, x);
}

static void pseudo_box(struct bctx *b, box_t *pb, node_t *e, style_t *ps) {
    int disp = ps->display;
    box_t *x;
    if (disp == D_INLINE) x = nbox(b, B_INLINE, e, ps);
    else if (disp == D_INLINE_BLOCK || disp == D_INLINE_FLEX || disp == D_INLINE_GRID || disp == D_INLINE_TABLE) {
        x = nbox(b, B_ATOMIC, e, ps);
        x->atomic = AT_INLINE_BLOCK;
    } else if (disp == D_FLEX) x = nbox(b, B_FLEX, e, ps);
    else if (disp == D_GRID) x = nbox(b, B_GRID, e, ps);
    else x = nbox(b, B_BLOCK, e, ps);
    x->floated = ps->float_ != FL_NONE;
    x->abspos = ps->position == POS_ABSOLUTE || ps->position == POS_FIXED;
    if (ps->content[0]) text_box(b, x, e, ps->content, strlen(ps->content), ps);
    if (x->kind != B_INLINE) fixup(b, x);
    append(pb, x);
}

static void children(struct bctx *b, box_t *pb, node_t *e, style_t *st) {
    /* Keep the principal box/background/border, but not the skipped contents.
       The flat DOM and its computed styles remain available to find-in-page. */
    if(st->content_visibility==CV_HIDDEN)return;
    if (st->before_generated && st->before) pseudo_box(b, pb, e, st->before);
    pvec list = {0};
    doc_flat_children(e, &list);
    bool details = !e->foreign && e->tag == T_details;
    node_t *summary = details ? doc_details_summary(e) : NULL;
    if (details && !summary) {
        /* The default legend is a renderer-owned box, never an invented DOM
           child. Hit testing associates this box with the native details. */
        style_t *legend = anon_style(b, st, D_LIST_ITEM);
        legend->list_style = LS_STRING;
        legend->list_style_string = node_attr(e, "open") ? "\xe2\x96\xbe " : "\xe2\x96\xb8 ";
        legend->list_style_inside = true;
        box_t *x = nbox(b, B_BLOCK, e, legend);
        x->anon = true;
        make_marker(b, x, e, legend);
        if (x->marker) text_box(b, x, e, x->marker, strlen(x->marker), legend);
        x->marker = NULL;
        text_box(b, x, e, "Details", 7, legend);
        fixup(b, x);
        append(pb, x);
    }
    if (details && summary) gen(b, pb, summary, st);
    for (int i = 0; i < list.n; i++) {
        node_t *child = list.v[i];
        if (details && (child == summary || !node_attr(e, "open"))) continue;
        gen(b, pb, child, st);
    }
    pv_free(&list);
    if (st->after_generated && st->after) pseudo_box(b, pb, e, st->after);
}

/* inline-table, inline-flex and inline-grid: an atomic inline box around the table, flex or grid box. The wrapper
   keeps the margins; the inner box the borders, padding and background. */
static box_t *wrapped(struct bctx *b, node_t *n, style_t *st, int inner_kind) {
    style_t *ws = ar_alloc(b->a, sizeof *ws);
    *ws = *st;
    for (int i = 0; i < 4; i++) {
        ws->border_width[i] = 0;
        ws->padding[i].kind = LK_LEN;
        ws->padding[i].px = ws->padding[i].pct = 0;
    }
    ws->bg_color = 0;
    ws->color_current &= (uint16_t)~(1u << CSS_SLOT_BACKGROUND);
    ws->bg_image = NULL;
    ws->has_grad = false;
    ws->gradient = NULL;
    ws->box_shadow = NULL; /* background decorations belong to the inner border box */
    ws->border_radius=0;memset(ws->corner_radius,0,sizeof ws->corner_radius);
    ws->width.kind = LK_AUTO;
    ws->height.kind = LK_AUTO;
    style_t *is = ar_alloc(b->a, sizeof *is);
    *is = *st;
    is->filter = NULL; is->opacity = 1; /* the element's outer group composites once, and clips once */
    is->clip_path = NULL; memset(&is->clip, 0, sizeof is->clip);
    is->transform=is->rotate=is->scale=NULL;memset(&is->translate,0,sizeof is->translate);is->offset=NULL;
    is->display = inner_kind == B_TABLE ? D_TABLE : inner_kind == B_GRID ? D_GRID : D_FLEX;
    for (int i = 0; i < 4; i++) {
        is->margin[i].kind = LK_LEN;
        is->margin[i].px = is->margin[i].pct = 0;
    }
    box_t *w = nbox(b, B_ATOMIC, n, ws);
    w->atomic = AT_INLINE_BLOCK;
    box_t *in = nbox(b, inner_kind, n, is);
    in->wrapped_inner = true;
    children(b, in, n, is);
    fixup(b, in);
    append(w, in);
    fixup(b, w);
    return w;
}

static void gen(struct bctx *b, box_t *pb, node_t *n, style_t *pst) {
    if (!web_native_checkpoint(b->d)) return;
    web_avmedia_checkpoint();
    if (n->type == N_TEXT) {
        if (pst && n->textlen) text_box(b, pb, n, n->text, n->textlen, pst);
        return;
    }
    if (n->type != N_ELEM) return;
    if (web_dialog_is_modal(b->d,n) && b->top != n) return;
    style_t *st = n->style;
    if (!st || st->display == D_NONE) return;
    if (n->tag == T_input) { /* a hidden input is never rendered, whatever its display */
        const char *t = node_attr(n, "type");
        if (t && str_ieq(t, "hidden")) return;
    }
    if (st->display == D_CONTENTS) {
        children(b, pb, n, st);
        return;
    }
    if (n->tag == T_br || n->tag == T_wbr) {
        box_t *line = nbox(b, B_BR, n, st);
        n->box = line;
        append(pb, line);
        return;
    }
    box_t *x;
    int at = atomic_kind(n);
    if(!n->foreign && n->tag==T_frameset){
        x=nbox(b,B_BLOCK,n,st);
        for(node_t *child=n->first;child;child=child->next)
            if(child->type==N_ELEM && !child->foreign && (child->tag==T_frame || child->tag==T_frameset))gen(b,x,child,st);
        append(pb,x);return;
    }
    if (at) {
        x = nbox(b, B_ATOMIC, n, st);
        x->atomic = (uint8_t)at;
        if (at == AT_SVG) x->svg = doc_svg(b->d, n, st->color);
        if (at == AT_INLINE_BLOCK) {
            children(b, x, n, st);
            fixup(b, x);
        }
    } else {
        switch (st->display) {
        case D_INLINE: {
            x = nbox(b, B_INLINE, n, st);
            children(b, x, n, st);
            bool block = false;
            for (box_t *c = x->first; c; c = c->next)
                if (box_block_level(c)) block = true;
            if (block) { /* block-in-inline: the inline becomes a block */
                x->kind = B_BLOCK;
                fixup(b, x);
            }
            break;
        }
        case D_INLINE_BLOCK:
            x = nbox(b, B_ATOMIC, n, st);
            x->atomic = AT_INLINE_BLOCK;
            children(b, x, n, st);
            fixup(b, x);
            break;
        case D_INLINE_TABLE: x = wrapped(b, n, st, B_TABLE); break;
        case D_INLINE_FLEX: x = wrapped(b, n, st, B_FLEX); break;
        case D_INLINE_GRID: x = wrapped(b, n, st, B_GRID); break;
        case D_TABLE: x = nbox(b, B_TABLE, n, st); goto container;
        case D_FLEX: x = nbox(b, B_FLEX, n, st); goto container;
        case D_GRID: x = nbox(b, B_GRID, n, st); goto container;
        case D_TABLE_ROW_GROUP: case D_TABLE_HEADER_GROUP: case D_TABLE_FOOTER_GROUP:
            x = nbox(b, B_ROW_GROUP, n, st);
            goto container;
        case D_TABLE_ROW: x = nbox(b, B_ROW, n, st); goto container;
        case D_TABLE_CELL: {
            x = nbox(b, B_CELL, n, st);
            const char *v;
            if ((v = node_attr(n, "colspan")) && atoi(v) > 1) x->colspan = atoi(v) > 1000 ? 1000 : atoi(v);
            if ((v = node_attr(n, "rowspan")) && atoi(v) > 1) x->rowspan = atoi(v) > 1000 ? 1000 : atoi(v);
            goto container;
        }
        case D_TABLE_CAPTION: x = nbox(b, B_CAPTION, n, st); goto container;
        case D_TABLE_COLUMN: case D_TABLE_COLUMN_GROUP: return;
        default:
            x = nbox(b, B_BLOCK, n, st);
        container:
            if (st->display == D_LIST_ITEM) {
                make_marker(b, x, n, st);
                if (x->marker && st->list_style_inside) {
                    text_box(b, x, n, x->marker, strlen(x->marker), st);
                    x->marker = NULL;
                    x->marker_shape = 0;
                }
            }
            children(b, x, n, st);
            fixup(b, x);
        }
    }
    x->floated = st->float_ != FL_NONE;
    x->abspos = st->position == POS_ABSOLUTE || st->position == POS_FIXED;
    if(st->content_visibility!=CV_VISIBLE && x->kind!=B_INLINE)x->is_bfc=true;
    /* Every node is generated once per publication. A moved node may still
       name its former box, which its former parent retires afterwards. */
    n->box = x; n->anchor_block = NULL;
    append(pb, x);
}

/* ---------------------------------------------------------------- fixups */
static bool table_part(const box_t *c) {
    return !c->floated && !c->abspos &&
           (c->kind == B_ROW_GROUP || c->kind == B_ROW || c->kind == B_CELL || c->kind == B_CAPTION);
}

/* Collapsed white space between blocks generates no box; its text node must
   not keep naming one. */
static void drop(box_t *c) {
    if (c->node && c->node->box == c) c->node->box = NULL;
}

/* detach the children of x into a list */
static box_t *take(box_t *x) {
    box_t *l = x->first;
    x->first = x->last = NULL;
    return l;
}

static void fix_rows(struct bctx *b, box_t *g) { /* a row group: everything must be in rows */
    box_t *row = NULL;
    for (box_t *c = take(g), *nx; c; c = nx) {
        nx = c->next;
        if (c->kind == B_ROW) {
            append(g, c);
            row = NULL;
        } else if (ws_only(c) && !row) {
            drop(c);
        } else {
            if (!row) {
                row = nbox(b, B_ROW, NULL, anon_style(b, g->st, D_TABLE_ROW));
                row->anon = true;
                append(g, row);
            }
            append(row, c);
        }
    }
    for (box_t *r = g->first; r; r = r->next)
        if (r->anon) fixup(b, r);
}

static void fix_cells(struct bctx *b, box_t *r) { /* a row: everything must be in cells */
    box_t *cell = NULL;
    for (box_t *c = take(r), *nx; c; c = nx) {
        nx = c->next;
        if (c->kind == B_CELL) {
            append(r, c);
            cell = NULL;
        } else if (ws_only(c) && !cell) {
            drop(c);
        } else {
            if (!cell) {
                cell = nbox(b, B_CELL, NULL, anon_style(b, r->st, D_TABLE_CELL));
                cell->anon = true;
                append(r, cell);
            }
            append(cell, c);
        }
    }
    for (box_t *c = r->first; c; c = c->next)
        if (c->anon) fixup(b, c);
}

static void fix_table(struct bctx *b, box_t *t) {
    box_t *group = NULL;
    for (box_t *c = take(t), *nx; c; c = nx) {
        nx = c->next;
        if (c->kind == B_CAPTION || c->kind == B_ROW_GROUP) {
            append(t, c);
            group = NULL;
        } else if (ws_only(c) && !group) {
            drop(c);
        } else {
            if (!group) {
                group = nbox(b, B_ROW_GROUP, NULL, anon_style(b, t->st, D_TABLE_ROW_GROUP));
                group->anon = true;
                append(t, group);
            }
            append(group, c);
        }
    }
    for (box_t *g = t->first; g; g = g->next)
        if (g->anon) fix_rows(b, g);
}

static bool real_inline(const box_t *c) { return !c->floated && !c->abspos && !ws_only(c); }

static void fixup(struct bctx *b, box_t *x) {
    switch (x->kind) {
    case B_TABLE: fix_table(b, x); return;
    case B_ROW_GROUP: fix_rows(b, x); return;
    case B_ROW: fix_cells(b, x); return;
    case B_INLINE: case B_TEXT: case B_BR: return;
    }
    if (x->kind == B_ATOMIC && x->atomic != AT_INLINE_BLOCK) return;
    /* table parts outside a table get an anonymous table */
    bool parts = false;
    for (box_t *c = x->first; c; c = c->next)
        if (table_part(c)) parts = true;
    if (parts) {
        box_t *tab = NULL;
        for (box_t *c = take(x), *nx; c; c = nx) {
            nx = c->next;
            if (table_part(c) || (tab && ws_only(c))) {
                if (!tab) {
                    tab = nbox(b, B_TABLE, NULL, anon_style(b, x->st, D_TABLE));
                    tab->anon = true;
                    append(x, tab);
                }
                append(tab, c);
            } else {
                if (tab) fix_table(b, tab);
                tab = NULL;
                append(x, c);
            }
        }
        if (tab) fix_table(b, tab);
    }
    bool has_block = x->kind == B_FLEX || x->kind == B_GRID;
    for (box_t *c = x->first; c; c = c->next)
        if (box_block_level(c)) has_block = true;
    if (!has_block) {
        x->inline_ctx = x->first != NULL;
        return;
    }
    /* wrap runs of inline content in anonymous blocks */
    box_t *run = NULL, *run_last = NULL;
    bool run_real = false;
    box_t *list = take(x);
    for (box_t *c = list, *nx;; c = nx) {
        nx = c ? c->next : NULL;
        if (!c || box_block_level(c) || ((x->kind == B_FLEX || x->kind == B_GRID) && c->abspos)) {
            if (run) {
                if (run_real) {
                    box_t *an = nbox(b, B_BLOCK, NULL, anon_style(b, x->st, D_BLOCK));
                    an->anon = true;
                    an->inline_ctx = true;
                    for (box_t *r = run, *rn; r; r = rn) {
                        rn = r->next;
                        append(an, r);
                    }
                    append(x, an);
                } else {
                    for (box_t *r = run, *rn; r; r = rn) {
                        rn = r->next;
                        if (!ws_only(r)) append(x, r);
                        else drop(r);
                    }
                }
                run = run_last = NULL;
                run_real = false;
            }
            if (!c) break;
            append(x, c);
            continue;
        }
        c->next = NULL;
        if (run_last) run_last->next = c;
        else run = c;
        run_last = c;
        if (real_inline(c)) run_real = true;
    }
}

static void clear_boxes(node_t *n) {
    if (n->shadow_root) clear_boxes(n->shadow_root);
    for (node_t *c = n->first; c; c = c->next) {
        c->box = NULL;
        c->anchor_block = NULL;
        clear_boxes(c);
    }
}

static void unpublish_boxes(box_t *b) {
    if(!b)return;
    pv_free(&b->paint_layers);
    web_shadow_cache_free(&b->shadow_cache);
    for(int i=0;i<3;i++)web_radius_cache_free(&b->radius_clip[i]);
    web_clip_cache_free(&b->clip_cache);
    for (int i = 0; i < b->ndecos; i++) {
        web_shadow_cache_free(&b->decos[i].shadow_cache);
        for(int j=0;j<2;j++)web_radius_cache_free(&b->decos[i].radius_clip[j]);
    }
    ar_free(&b->layout_mem);
    ar_free(&b->text_mem);
    /* an anchor-positioned box's record goes with it; the others may name this box until the next layout */
    box_anchor_free(b);
    if(b->node && b->node->owner)b->node->owner->anchor_epoch++;
    if(b->node && b->node->box==b){b->node->box=NULL;b->node->anchor_block=NULL;}
    for(box_t *child=b->first,*next;child;child=next){next=child->next;unpublish_boxes(child);}
    arena_t *branch=b->branch_mem;
    if(branch){ar_free(branch);free(branch);} /* b itself belonged to this last-released arena */
}
/* ------------------------------------------------------- local publication */
/* A whole build regenerates every box and so drops every layout cache.
   Between builds, mutations record the nodes they touched (boxes_note_*),
   box-changing restyles mark box_regen, and the next publication regenerates
   only those parts, in two shapes:
   - a splice regenerates the changed children of one container and keeps
     the others with their layout, where the container's own fixup would
     place the new children without anonymous wrappers;
   - a region regenerates one element's box subtree, where the new box takes
     the old one's place in its parent's fixup.
   A shape that does not hold escalates to the next enclosing one; the root
   element, the top layer and slot reassignment rebuild everything. gen gives
   every generated node its new box and every retired box clears the nodes
   still naming it, so the order of the units decides only the work done. */
static void changes_forget(web_doc *d) {
    pvec *p=&d->box_changes;
    for(int i=0;i+1<p->n;i+=2)if(!p->v[i+1])((node_t *)p->v[i])->box_regen=false;
    p->n=0;
}
void boxes_changes_clear(web_doc *d){if(d){changes_forget(d);d->box_changes_full=false;d->box_full_reason=NULL;}}
void boxes_require_full(web_doc *d,const char *reason){
    if(!d)return;
    changes_forget(d);
    if(!d->box_changes_full)d->box_full_reason=reason;
    d->box_changes_full=true;
}
static bool changes_push(web_doc *d,node_t *a,node_t *b){
    if(!d->root_box || d->box_changes_full)return false;
    pvec *p=&d->box_changes;
    if(p->n>INT_MAX-2){boxes_require_full(d,"change records");return false;}
    if(p->n+2>p->cap){
        int capacity=p->cap && p->cap<=INT_MAX/2?p->cap*2: p->cap?INT_MAX:16;
        void **values=(size_t)capacity>SIZE_MAX/sizeof *p->v?NULL:realloc(p->v,(size_t)capacity*sizeof *p->v);
        if(!values){boxes_require_full(d,"change records");return false;}
        p->v=values;p->cap=capacity;
    }
    p->v[p->n++]=a;p->v[p->n++]=b;
    return true;
}
static node_t *foreign_root(node_t *n){
    node_t *root=NULL;
    for(;n;n=n->parent)if(n->foreign && (!n->parent || !n->parent->foreign))root=n;
    return root;
}
void boxes_note_foreign(web_doc *d,node_t *n){
    node_t *root=d?foreign_root(n):NULL;
    if(!root)return;
    d->need_boxes=true;d->layout_valid=false;
    if(root->tag!=T_svg){boxes_require_full(d,"foreign content");return;}
    /* No descendant of an outermost <svg> generates a box: its atomic box owns
       a snapshot serialized from them, which the publication refreshes. One
       without that box may be a sprite sheet that <use> reaches from others. */
    box_t *b=root->box;
    if(b && b->node==root && b->kind==B_ATOMIC && b->atomic==AT_SVG){root->svg_snapshot_dirty=true;layout_invalidate(b);}
    else d->svg_snapshots_dirty=true;
}
void boxes_note_child_list(web_doc *d,node_t *parent,node_t *subtree){
    if(!d || !d->root_box || d->box_changes_full)return;
    if(!parent || !subtree){boxes_require_full(d,"child list");return;}
    if(foreign_root(parent)){boxes_note_foreign(d,parent);return;}
    changes_push(d,parent,subtree);
}
void boxes_note_regen(web_doc *d,node_t *n){
    if(!d || !n || !d->root_box || d->box_changes_full || n->box_regen)return;
    if(foreign_root(n)){boxes_note_foreign(d,n);return;}
    if(changes_push(d,n,NULL))n->box_regen=true;
}

/* --debug-js: box publications between web_box_report() calls (main thread only). */
#define BOX_WHOLE_REASONS 6
static struct {
    uint64_t locals, splices, regions, escalations, whole;
    struct { const char *reason; uint64_t count; } reasons[BOX_WHOLE_REASONS];
} box_profile;
static void box_profile_whole(const char *reason) {
    box_profile.whole++;
    int slot=0;
    for(int i=0;i<BOX_WHOLE_REASONS;i++){
        if(box_profile.reasons[i].reason && !strcmp(box_profile.reasons[i].reason,reason)){slot=i;goto found;}
        if(box_profile.reasons[i].count<box_profile.reasons[slot].count)slot=i;
    }
    box_profile.reasons[slot].reason=reason;box_profile.reasons[slot].count=0;
found:
    box_profile.reasons[slot].count++;
}
int web_box_report(char *out,size_t size) {
    if(!box_profile.locals && !box_profile.whole)return 0;
    int n=snprintf(out,size,"Native box profile: local publications %lu (splices %lu, regions %lu, escalations %lu), whole builds %lu",
        (unsigned long)box_profile.locals,(unsigned long)box_profile.splices,(unsigned long)box_profile.regions,
        (unsigned long)box_profile.escalations,(unsigned long)box_profile.whole);
    const char *separator=" by ";
    for(int i=0;i<BOX_WHOLE_REASONS && n>=0 && (size_t)n<size;i++)
        if(box_profile.reasons[i].count){
            n+=snprintf(out+n,size-(size_t)n,"%s%s %lu",separator,box_profile.reasons[i].reason,
                (unsigned long)box_profile.reasons[i].count);
            separator=", ";
        }
    memset(&box_profile,0,sizeof box_profile);
    return n;
}

/* The snapshot of every boxed <svg> marked dirty, or of all after a change
   no single snapshot owns. The same cache entry keeps its raster unless the
   serialized source changed. */
static void refresh_svgs(web_doc *d) {
    bool all=__atomic_exchange_n(&d->svg_snapshots_dirty,false,__ATOMIC_RELAXED);
    for (int i = 0; i < d->svgs.n; i++) {
        struct svg_cache *cache = d->svgs.v[i];
        node_t *n = cache->node;
        box_t *b = n ? n->box : NULL;
        if (!b || b->node != n || b->atomic != AT_SVG || b->svg != cache) continue;
        if (!all && !n->svg_snapshot_dirty) continue;
        b->svg = doc_svg(d, n, b->st->color);
    }
}
/* After a local publication: entries no published box owns any more. */
static void sweep_svgs(web_doc *d) {
    int retained=0;
    for(int i=0;i<d->svgs.n;i++){
        struct svg_cache *cache=d->svgs.v[i];
        node_t *n=cache->node;
        box_t *b=n?n->box:NULL;
        if(b && b->node==n && b->kind==B_ATOMIC && b->atomic==AT_SVG && b->svg==cache)d->svgs.v[retained++]=cache;
        else{
            if(cache->img)image_free(cache->img);
            free(cache->src);
            free(cache);
        }
    }
    d->svgs.n=retained;
}

struct local {
    web_doc *d;
    uint32_t epoch;
    jmp_buf *trap;
    arena_t *staging; /* the arena gen is filling; a trapped allocation frees it */
    pvec flat, all, gone;
    unsigned changed, splices, regions, escalations;
    const char *full; /* why every box must be rebuilt instead */
};
enum local_step { STEP_DONE, STEP_SPLICE, STEP_REGION, STEP_FULL };
enum { UNIT_REGION, UNIT_SPLICE }; /* a region first: a splice of the same node then finds it current */
struct local_unit { node_t *node; unsigned depth, kind; };
struct local_units { struct local_unit *v; size_t n, cap; };

static bool wrapped_display(int display) {
    return display == D_INLINE_TABLE || display == D_INLINE_FLEX || display == D_INLINE_GRID;
}
static bool local_connected(web_doc *d,node_t *n){return n && doc_node_root(n,true)==d->root;}
static node_t *local_flat_parent(node_t *n){return n->shadow_host?n->shadow_host:doc_flat_parent(n);}
static arena_t *local_stage(struct local *L){
    arena_t *a=calloc(1,sizeof *a);
    if(!a)return NULL;
    a->chunk_size=4096;a->trap=L->trap;L->staging=a;
    return a;
}
/* Boxes gen staged that the tree does not take: the nodes now naming them
   are cleared, then the one arena they were built in is freed. */
static void local_discard(box_t *first,arena_t *arena){
    for(box_t *x=first,*next;x;x=next){next=x->next;x->branch_mem=NULL;unpublish_boxes(x);}
    ar_free(arena);free(arena);
}
static void local_discard_box(box_t *x){
    arena_t *arena=x->branch_mem;
    x->next=NULL;local_discard(x,arena);
}
/* A changed box and everything above it: layout, stacking lists, borrows. */
static void local_dirty(box_t *b){
    layout_invalidate(b);
    for(;b;b=b->parent){b->paint_layers_valid=b->flow_ink_valid=false;pv_free(&b->paint_layers);b->style_sync_dirty=true;}
}
/* The parent's fixup sorts a child only by these, so two boxes that agree
   take the same place, inside the same anonymous wrapper. */
static bool local_same_place(const box_t *old,const box_t *box){
    if(box_block_level(old)!=box_block_level(box) || old->floated!=box->floated || old->abspos!=box->abspos ||
       table_part(old)!=table_part(box))return false;
    int parent=old->parent->kind;
    return !(table_part(old) || parent==B_TABLE || parent==B_ROW_GROUP || parent==B_ROW) || old->kind==box->kind;
}
/* What a block container's fixup wraps in an anonymous block. */
static bool inline_content(const box_t *container,const box_t *c){
    if(box_block_level(c) || ((container->kind==B_FLEX || container->kind==B_GRID) && c->abspos))return false;
    return real_inline(c);
}
/* Text of white space alone, which collapses away between blocks. */
static bool local_collapses(const node_t *n,const style_t *st){
    if(preserves_space(st))return false;
    for(size_t i=0;i<n->textlen;i++)if(!is_space((unsigned char)n->text[i]))return false;
    return true;
}

static bool is_list(const node_t *n){
    return n->type==N_ELEM && !n->foreign && (n->tag==T_ol || n->tag==T_ul || n->tag==T_menu);
}
static bool listish(const node_t *n){
    if(n->type!=N_ELEM)return false;
    const box_t *b=n->box && n->box->node==n?n->box:NULL;
    return (!n->foreign && n->tag==T_li) || (n->style && n->style->display==D_LIST_ITEM) ||
        (n->style_previous && n->style_previous->display==D_LIST_ITEM) || (b && b->st->display==D_LIST_ITEM);
}
/* Whether its marker is a number. A removed item has no computed style left;
   its retiring box still names the last one. */
static bool list_numbered(const node_t *n){
    const style_t *s=n->style?n->style: n->box && n->box->node==n?n->box->st:NULL;
    if(!s)return false;
    int ls=s->list_style;
    return ls!=LS_NONE && ls!=LS_DISC && ls!=LS_CIRCLE && ls!=LS_SQUARE && ls!=LS_STRING;
}
/* Markers count the items of their owning list (list_owner). Adding,
   removing or re-displaying an item owned outside the changed subtree
   renumbers that list, when any of its markers is a number. */
static node_t *local_list_scope(node_t *parent,node_t *subtree){
    bool escapes=false,numbered=false;
    for(node_t *n=subtree;n;){
        if(listish(n)){
            bool owned=false;
            for(node_t *a=n;a && a!=subtree;){a=a->parent;if(a && is_list(a)){owned=true;break;}}
            if(!owned){escapes=true;numbered=numbered || list_numbered(n);}
        }
        if(n->first){n=n->first;continue;}
        while(n!=subtree && !n->next)n=n->parent;
        if(n==subtree)break;
        n=n->next;
    }
    if(!escapes)return NULL;
    node_t *owner=parent;
    for(node_t *a=parent;a;a=a->parent)if(is_list(a)){owner=a;break;}
    for(node_t *n=owner->first;n && !numbered;){
        if(listish(n))numbered=list_numbered(n);
        if(n->first && !is_list(n)){n=n->first;continue;}
        while(n!=owner && !n->next)n=n->parent;
        if(n==owner)break;
        n=n->next;
    }
    return numbered?owner:NULL;
}

/* The nearest flat ancestor-or-self whose own box a region can replace;
   STEP_DONE where nothing renders (display:none, unslotted, detached). */
static enum local_step local_resolve(struct local *L,node_t *n,node_t **out){
    if(!local_connected(L->d,n))return STEP_DONE;
    for(node_t *a=n;a;a=local_flat_parent(a)){
        if(a->type!=N_ELEM)continue;
        if(a->style && a->style->display==D_NONE)return STEP_DONE;
        if(a->box && a->box->node==a){*out=a;return STEP_REGION;}
    }
    return STEP_DONE;
}

static enum local_step local_region(struct local *L,node_t *r,node_t **next){
    web_doc *d=L->d;
    box_t *old=r->box;
    if(!local_connected(d,r))return STEP_DONE;
    if(!old || old->node!=r){*next=r;return STEP_REGION;}
    if(old->epoch==L->epoch)return STEP_DONE;
    if(r==d->html || !old->parent){L->full="root element";return STEP_FULL;}
    if(web_dialog_count(d) && web_dialog_is_modal(d,r)){L->full="top layer";return STEP_FULL;}
    arena_t *arena=local_stage(L);
    if(!arena){L->full="memory";return STEP_FULL;}
    box_t holder={.st=old->parent->st};
    struct bctx b={.d=d,.a=arena,.epoch=L->epoch};
    gen(&b,&holder,r,NULL);
    arena->trap=NULL;L->staging=NULL;
    box_t *box=holder.first;
    if(d->native_cancelled){local_discard(box,arena);L->full="cancelled";return STEP_FULL;}
    if(!box || box->next || !local_same_place(old,box)){
        /* Its parent's fixup would place it differently: splice it there.
           The old box stays linked for that splice to retire. */
        local_discard(box,arena);
        r->box=NULL;r->anchor_block=NULL;
        if(!(*next=local_flat_parent(r))){L->full="no flat parent";return STEP_FULL;}
        return STEP_SPLICE;
    }
    box->branch_mem=arena;
    box_t *parent=old->parent,*prev=NULL;
    for(box_t *x=parent->first;x && x!=old;x=x->next)prev=x;
    box->parent=parent;box->next=old->next;
    if(prev)prev->next=box;else parent->first=box;
    if(parent->last==old)parent->last=box;
    local_dirty(box);
    unpublish_boxes(old);
    L->changed++;L->regions++;
    return STEP_DONE;
}

static enum local_step local_splice(struct local *L,node_t *p,node_t **next){
    web_doc *d=L->d;
    if(!local_connected(d,p))return STEP_DONE;
    if(p->type!=N_ELEM){
        if(!p->shadow_host){L->full="document children";return STEP_FULL;}
        p=p->shadow_host; /* a shadow root's children are its host's flat children */
    }
    box_t *c=p->box;
    const style_t *ps=p->style;
    if(!c || c->node!=p || !ps){*next=p;return STEP_REGION;}
    if(c->epoch==L->epoch)return STEP_DONE;
    if(c->kind==B_ATOMIC && wrapped_display(c->st->display)){
        if(!c->first || c->first!=c->last || c->first->node!=p){*next=p;return STEP_REGION;}
        c=c->first; /* inline-flex/grid/table: the children belong to the inner box */
    }
    /* A replaced element renders its children itself, not as boxes. */
    if(c->kind==B_ATOMIC && c->atomic!=AT_INLINE_BLOCK){local_dirty(c);L->changed++;return STEP_DONE;}
    if(ps->content_visibility==CV_HIDDEN)return STEP_DONE;
    if((!p->foreign && (p->tag==T_details || p->tag==T_frameset)) ||
       !(c->kind==B_BLOCK || c->kind==B_CELL || c->kind==B_CAPTION || c->kind==B_FLEX || c->kind==B_GRID ||
         c->kind==B_INLINE || c->kind==B_ATOMIC)){*next=p;return STEP_REGION;}
    /* Its own generated boxes stay first (::before, an inside marker) and
       last (::after); an anonymous wrapper means the fixup must run again. */
    int phase=0;
    for(box_t *x=c->first;x;x=x->next){
        bool own=x->node==p,after=own && ps->after && x->st==ps->after;
        if(x->anon || !x->node || (own && !after && phase) || (!own && phase==2)){*next=p;return STEP_REGION;}
        phase=after?2:own?phase:1;
    }
    bool flexgrid=c->kind==B_FLEX || c->kind==B_GRID,inline_box=c->kind==B_INLINE;
    enum { MODE_EMPTY, MODE_INLINE, MODE_BLOCK } mode=flexgrid?MODE_BLOCK:inline_box?MODE_INLINE:
        !c->first?MODE_EMPTY:c->inline_ctx?MODE_INLINE:MODE_BLOCK;
    pvec *all=&L->all,*flat=&L->flat;
    all->n=flat->n=L->gone.n=0;
    for(box_t *x=c->first;x && x->node==p && !(ps->after && x->st==ps->after);x=x->next)pv_push(all,x);
    doc_flat_children(p,flat);
    bool failed=false;
    for(int i=0;i<flat->n && !failed;i++){
        node_t *x=flat->v[i];
        if(!(i&63)){
            if(!web_native_checkpoint(d)){failed=true;L->full="cancelled";break;}
            web_avmedia_checkpoint();
        }
        if(x->type!=N_ELEM && x->type!=N_TEXT)continue;
        box_t *k=x->box;
        if(k && k->parent==c && k->node==x && (!x->box_regen || k->epoch==L->epoch)){k->local_kept=true;pv_push(all,k);continue;}
        if(x->type==N_ELEM && web_dialog_count(d) && web_dialog_is_modal(d,x))continue; /* its box is in the top layer */
        if(x->type==N_TEXT && mode==MODE_BLOCK && local_collapses(x,c->st))continue;
        arena_t *arena=local_stage(L);
        if(!arena){failed=true;L->full="memory";break;}
        box_t holder={.st=c->st};
        struct bctx b={.d=d,.a=arena,.epoch=L->epoch};
        gen(&b,&holder,x,c->st);
        arena->trap=NULL;L->staging=NULL;
        box_t *y=holder.first;
        if(d->native_cancelled){local_discard(y,arena);failed=true;L->full="cancelled";break;}
        if(!y){ar_free(arena);free(arena);continue;}
        if(y->next){local_discard(y,arena);failed=true;break;} /* display:contents: one arena, several owners */
        if(mode==MODE_BLOCK && ws_only(y)){local_discard(y,arena);continue;}
        y->branch_mem=arena;
        pv_push(all,y);
    }
    for(box_t *x=c->first;x;x=x->next)if(x->node==p && ps->after && x->st==ps->after)pv_push(all,x);
    /* Valid only where the container's fixup would decide what it decided
       before: no new anonymous wrapper and no white space it would keep. */
    bool any_block=false,any_inline=false,any_ws=false,any_table=false;
    for(int i=0;i<all->n && !failed;i++){
        box_t *x=all->v[i];
        if(box_block_level(x))any_block=true;
        else if(inline_content(c,x))any_inline=true;
        any_ws=any_ws || ws_only(x);
        any_table=any_table || table_part(x);
    }
    bool inline_ctx=c->inline_ctx;
    if(failed || any_table)failed=true;
    else if(inline_box)failed=any_block;
    else if(flexgrid)failed=any_inline || any_ws;
    else if(any_block){failed=mode==MODE_INLINE || any_inline;inline_ctx=false;}
    else{failed=mode==MODE_BLOCK;inline_ctx=all->n>0;}
    if(failed){
        for(int i=0;i<all->n;i++){
            box_t *x=all->v[i];
            if(x->local_kept)x->local_kept=false;
            else if(x->node!=p)local_discard_box(x);
        }
        if(L->full)return STEP_FULL;
        *next=p;return STEP_REGION;
    }
    if(any_block && any_ws && !flexgrid && !inline_box){ /* only new boxes of a container that was empty */
        int kept=0;
        for(int i=0;i<all->n;i++){box_t *x=all->v[i];if(ws_only(x))local_discard_box(x);else all->v[kept++]=x;}
        all->n=kept;
    }
    for(box_t *x=c->first;x;x=x->next)if(x->node!=p && !x->local_kept)pv_push(&L->gone,x);
    c->first=c->last=NULL;
    for(int i=0;i<all->n;i++){box_t *x=all->v[i];x->local_kept=false;append(c,x);}
    if(!flexgrid && !inline_box)c->inline_ctx=inline_ctx;
    for(int i=0;i<L->gone.n;i++)unpublish_boxes(L->gone.v[i]);
    local_dirty(c);
    L->changed++;L->splices++;
    return STEP_DONE;
}

/* One unit and the enclosing ones it escalates to, strictly upward. */
static bool local_unit(struct local *L,node_t *node,unsigned kind){
    for(unsigned steps=0;steps<65536;steps++){
        node_t *next=NULL;
        enum local_step s=kind==UNIT_SPLICE?local_splice(L,node,&next):local_region(L,node,&next);
        if(s==STEP_DONE)return true;
        if(s==STEP_FULL)return false;
        L->escalations++;
        if(s==STEP_SPLICE){node=next;kind=UNIT_SPLICE;continue;}
        if(local_resolve(L,next,&node)==STEP_DONE)return true;
        kind=UNIT_REGION;
    }
    L->full="escalation";
    return false;
}
static bool local_add(struct local_units *u,node_t *node,unsigned kind){
    if(u->n==u->cap){
        size_t cap=u->cap?u->cap*2:16;
        struct local_unit *v=cap>SIZE_MAX/sizeof *v?NULL:realloc(u->v,cap*sizeof *v);
        if(!v)return false;
        u->v=v;u->cap=cap;
    }
    unsigned depth=0;
    for(node_t *n=node;n;n=n->parent?n->parent:n->shadow_host)depth++;
    u->v[u->n++]=(struct local_unit){node,depth,kind};
    return true;
}
/* A child-list record (a, its changed child b) or a node a whose own box
   changes (b NULL), as the unit that regenerates it. */
static bool local_request(struct local *L,struct local_units *u,node_t *a,node_t *b){
    web_doc *d=L->d;
    if(!local_connected(d,a))return true;
    if(b){
        if(a->type==N_DOC){L->full="document children";return false;}
        node_t *list=local_list_scope(a,b);
        if(!(list?local_add(u,list,UNIT_REGION):local_add(u,a,UNIT_SPLICE)))L->full="memory";
        return !L->full;
    }
    if(a==d->html){L->full="root element";return false;}
    if(web_dialog_count(d) && web_dialog_is_modal(d,a)){L->full="top layer";return false;}
    node_t *list=a->type==N_ELEM && a->parent && listish(a)?local_list_scope(a->parent,a):NULL,*p;
    bool added=list?local_add(u,list,UNIT_REGION):
        a->type==N_ELEM && a->box && a->box->node==a?local_add(u,a,UNIT_REGION):
        !(p=local_flat_parent(a)) || local_add(u,p,UNIT_SPLICE);
    if(!added)L->full="memory";
    return added;
}
static void restyle_boxes(box_t *b,bool inherited_borrow);
static int local_unit_order(const void *a,const void *b){
    const struct local_unit *x=a,*y=b;
    if(x->depth!=y->depth)return x->depth<y->depth?-1:1;
    if(x->kind!=y->kind)return x->kind<y->kind?-1:1;
    uintptr_t m=(uintptr_t)x->node,n=(uintptr_t)y->node;
    return m<n?-1:m>n;
}
bool boxes_rebuild_local(web_doc *d){
    if(!d || d->box_changes_full || !d->root_box)return false;
    struct local *L=calloc(1,sizeof *L);
    if(!L){d->box_full_reason="memory";return false;}
    L->d=d;
    if(!++d->box_epoch)d->box_epoch=1;
    L->epoch=d->box_epoch;
    struct local_units u={0};
    bool ok=true;
    for(int i=0;ok && i+1<d->box_changes.n;i+=2)ok=local_request(L,&u,d->box_changes.v[i],d->box_changes.v[i+1]);
    for(node_t *e=d->style_retired_first;ok && e;e=e->style_retired_next)if(e->box_regen)ok=local_request(L,&u,e,NULL);
    if(ok){
        qsort(u.v,u.n,sizeof *u.v,local_unit_order);
        jmp_buf trap;
        L->trap=&trap;
        if(setjmp(trap)){
            /* Nodes may name boxes of the trapped arena: the whole rebuild
               that follows clears every node of the document first. */
            if(L->staging){ar_free(L->staging);free(L->staging);L->staging=NULL;}
            L->full="memory";ok=false;
        }else for(size_t i=0;ok && i<u.n;i++){
            if(i && u.v[i].node==u.v[i-1].node && u.v[i].kind==u.v[i-1].kind)continue;
            ok=local_unit(L,u.v[i].node,u.v[i].kind);
        }
    }
    if(ok){
        if(L->changed){
            web_hit_index_free(d);d->paint_order_valid=false;d->abs_boxes.n=0;d->visual_revision++;d->style_revision++;
            restyle_boxes(d->root_box,false); /* one dirty-path join also recomputes ancestor paint_alpha */
            sweep_svgs(d);
        }
        refresh_svgs(d);
        if(d->profile_enabled){
            box_profile.locals++;box_profile.splices+=L->splices;box_profile.regions+=L->regions;
            box_profile.escalations+=L->escalations;
        }
        boxes_changes_clear(d);
    }else d->box_full_reason=L->full?L->full:"local publication";
    pv_free(&L->flat);pv_free(&L->all);pv_free(&L->gone);free(u.v);free(L);
    return ok;
}
void boxes_discard(web_doc *d) {
    if(!d)return;
    web_hit_index_free(d);
    d->paint_order_valid=false;
    /* Walk the OLD published tree too: a removed/adopted node need not occur
       in the current DOM, but may still borrow this document's prior box. */
    unpublish_boxes(d->root_box);d->root_box=NULL;d->abs_boxes.n=0;
    if(d->root)clear_boxes(d->root); /* also clears an unpublished failed rebuild */
    for(int i=0;i<web_dialog_count(d);i++)web_dialog_set_backdrop(d,web_dialog_at(d,i),NULL);
    ar_free(&d->bmem);
    changes_forget(d);pv_free(&d->box_changes);d->box_changes_full=false; /* the reason stays for the build */
}

static style_t *replacement_style(node_t *n, style_t *old) {
    for (; n; n=doc_flat_parent(n)) {
        style_t *previous=n->style_previous,*next=n->style;
        if (!previous || !next) continue;
        if (old==previous) return next;
        if (old==previous->before) return next->before;
        if (old==previous->after) return next->after;
        if (old==previous->backdrop) return next->backdrop;
    }
    return old;
}
static node_t *box_style_node(box_t *b) {
    for (; b; b=b->parent) if(b->node)return b->node;
    return NULL;
}
static bool restyle_derived(box_t *b, style_t *old) {
    node_t *n=b->node;
    if (!n || !n->style || !n->style_previous || old == n->style_previous) return false;
    bool outer=b->kind == B_ATOMIC && b->atomic == AT_INLINE_BLOCK && wrapped_display(old->display) &&
        b->first && b->first->node == n &&
        (b->first->kind == B_TABLE || b->first->kind == B_GRID || b->first->kind == B_FLEX);
    bool inner=b->parent && b->parent->node == n && b->parent->kind == B_ATOMIC &&
        wrapped_display(b->parent->st->display) &&
        (b->kind == B_TABLE || b->kind == B_GRID || b->kind == B_FLEX);
    if (!outer && !inner) return false;
    /* wrapped() owns derived shallow copies in bmem. Replace every borrowed
       value too, not merely the principal node style, before arena retirement. */
    style_t next=*n->style;
    if (outer) {
        for (int i=0;i<4;i++) {
            next.border_width[i]=0;
            next.padding[i].kind=LK_LEN; next.padding[i].px=next.padding[i].pct=0;
        }
        next.bg_color=0; next.color_current&=(uint16_t)~(1u<<CSS_SLOT_BACKGROUND); next.bg_image=NULL; next.has_grad=false; next.gradient=NULL;
        next.box_shadow=NULL;
        next.border_radius=0;memset(next.corner_radius,0,sizeof next.corner_radius);
        next.width.kind=next.height.kind=LK_AUTO;
    } else {
        next.display=b->kind == B_TABLE ? D_TABLE : b->kind == B_GRID ? D_GRID : D_FLEX;
        next.filter=NULL; next.opacity=1; next.clip_path=NULL; memset(&next.clip,0,sizeof next.clip);
        next.transform=next.rotate=next.scale=NULL;memset(&next.translate,0,sizeof next.translate);next.offset=NULL;
        for (int i=0;i<4;i++) {
            next.margin[i].kind=LK_LEN; next.margin[i].px=next.margin[i].pct=0;
        }
    }
    if (!css_style_layout_equal(old,&next)) layout_invalidate(b);
    *old=next;
    return true;
}
static void restyle_boxes(box_t *b,bool inherited_borrow) {
    if (!b||(!inherited_borrow&&!b->style_sync_dirty)) return;
    b->paint_layers_valid=b->flow_ink_valid=false; /* paint-only effects can change stacking and ink without new geometry */
    bool own_borrow=(b->node&&b->node->style_previous)||b->style_sync_subtree;
    style_t *old = b->st;
    if (restyle_derived(b,old)) {
        /* The stable derived-style slot also updates existing run borrows. */
    } else if ((b->st=replacement_style(box_style_node(b),old)) != old) {
        /* a node's style, or the one an anonymous ::backdrop box borrows */
        if (!css_style_layout_equal(old,b->st)) layout_invalidate(b);
    } else if (b->anon && b->parent) {
        /* the box arena's own anonymous style, inherited from the parent box's */
        int display = old->display;
        style_t next;
        css_style_init(&next, b->parent->st);
        css_style_finish(&next, NULL, false); next.display = (uint8_t)display;
        if (b->node && !b->node->foreign && b->node->tag == T_details) {
            next.list_style=old->list_style; next.list_style_string=old->list_style_string;
            next.list_style_inside=old->list_style_inside;
        }
        if (!css_style_layout_equal(old,&next)) layout_invalidate(b);
        *old = next;
    }
    /* An unchanged element may keep cached geometry even when its new style
       has exactly the same values. Cached paint runs/decorations still borrow
       that element's OLD arena, so update those references before retirement. */
    node_t *fallback=box_style_node(b);
    for(int i=0;i<b->nruns;i++) {
        struct run *run=&b->runs[i];
        run->st=replacement_style(run->node?run->node:fallback,run->st);
    }
    for(int i=0;i<b->ndecos;i++) {
        struct deco *deco=&b->decos[i];
        deco->st=replacement_style(deco->node?deco->node:fallback,deco->st);
    }
    b->paint_alpha = b->kind != B_TEXT && b->kind != B_BR && b->st &&
        (b->st->opacity * (b->st->filter ? b->st->filter->opacity : 1) < 1 || box_transformed(b));
    for (box_t *c = b->first; c; c = c->next) { restyle_boxes(c,inherited_borrow||own_borrow); b->paint_alpha |= c->paint_alpha; }
    b->style_sync_dirty=b->style_sync_subtree=false;
}
void boxes_restyle(web_doc *d) {
    restyle_boxes(d->root_box,false);
    refresh_svgs(d);
    /* Geometry reuse does not mean hit/paint reuse: opacity, visibility and
       pointer-events can change with a native effect and no DOM revision. */
    d->visual_revision++;d->style_revision++;d->paint_order_valid=false;
}

bool boxes_update_text(web_doc *d, node_t *n) {
    if (!d || !n || !n->box || n->box->kind != B_TEXT || n->box->node != n) return false;
    box_t *b = n->box, *block = b->parent;
    while (block && block->kind == B_INLINE) block = block->parent;
    if (!block || !block->inline_ctx) return false;
    arena_t next = {.chunk_size=4096}; jmp_buf trap; next.trap = &trap;
    if (setjmp(trap)) { ar_free(&next); return false; }
    size_t length;
    const char *text = ws_process(&next,n->text,n->textlen,b->st,&length,NULL);
    next.trap = NULL; ar_free(&b->text_mem); b->text_mem = next;
    b->text=text; b->len=length;
    layout_invalidate(b); return true;
}
void boxes_build(web_doc *d, arena_t *a) {
    struct bctx b = {.d=d, .a=a};
    if (d->profile_enabled) box_profile_whole(d->box_full_reason ? d->box_full_reason : "no box tree");
    for (int i = 0; i < d->svgs.n; i++) ((struct svg_cache *)d->svgs.v[i])->used = false;
    d->svg_snapshots_dirty = false; /* every boxed <svg> below is serialized afresh */
    if (d->root) clear_boxes(d->root);
    box_t *root = nbox(&b, B_BLOCK, NULL, anon_style(&b, NULL, D_BLOCK));
    root->anon = true;
    root->is_bfc = true;
    if (d->html) gen(&b, root, d->html, NULL);
    fixup(&b, root);
    d->root_box = root;
    for (int i=0;i<web_dialog_count(d);i++) {
        node_t *n=web_dialog_at(d,i); if(!n)continue;
        web_dialog_set_backdrop(d,n,NULL);
        if(!web_dialog_rendered(n))continue;
        /* Unlike ::before/::after, a top-layer ::backdrop does not depend on
           the content property generating a box. */
        if(n->style->backdrop && n->style->backdrop->display!=D_NONE) {
            box_t *back=nbox(&b,B_BLOCK,n,n->style->backdrop);
            back->abspos=true; back->anon=true; append(root,back);
            web_dialog_set_backdrop(d,n,back);
        }
        b.top=n; gen(&b,root,n,NULL); b.top=NULL;
    }
    /* The new tree now owns only entries marked by real SVG generation.
       Compact in place; no quota, replacement array, or stale-node retention. */
    int retained = 0;
    for (int i = 0; i < d->svgs.n; i++) {
        struct svg_cache *cache = d->svgs.v[i];
        if (cache->used) d->svgs.v[retained++] = cache;
        else {
            if (cache->img) image_free(cache->img);
            free(cache->src);
            free(cache);
        }
    }
    d->svgs.n = retained;
}
