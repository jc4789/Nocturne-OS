/* Box generation: turns the styled DOM into a tree of boxes. Inline content that sits next to
   block boxes is wrapped in anonymous blocks, table parts get the anonymous rows and cells they
   need, list items get markers, ::before/::after become boxes, and replaced elements (images,
   form controls, inline SVG) become atomic boxes. */
#include <stdio.h>
#include "webi.h"
#include "web_dialog.h"
#include "elements.h"
#include "svg_geometry.h"

struct bctx {
    web_doc *d;
    arena_t *a;
    node_t *top;
};

static box_t *nbox(struct bctx *b, int kind, node_t *n, style_t *st) {
    box_t *x = ar_alloc(b->a, sizeof *x);
    x->kind = (uint8_t)kind;
    x->node = n;
    x->st = st;
    x->baseline = -1;
    x->colspan = x->rowspan = 1;
    return x;
}

static void append(box_t *p, box_t *c) {
    c->parent = p;
    c->next = NULL;
    if (p->last) p->last->next = c;
    else p->first = c;
    p->last = c;
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

/* white-space processing and text-transform */
static const char *ws_process(arena_t *a, const char *s, size_t n, const style_t *st, size_t *out_n) {
    char *o = ar_alloc(a, n * 8 + 1); /* tabs may expand */
    size_t k = 0;
    int ws = st->white_space;
    if (ws == WS_PRE || ws == WS_PRE_WRAP || ws == WS_BREAK_SPACES) {
        int col = 0;
        for (size_t i = 0; i < n; i++) {
            char c = s[i];
            if (c == '\t') {
                int sp = 8 - col % 8;
                for (int j = 0; j < sp; j++) o[k++] = ' ';
                col += sp;
            } else {
                o[k++] = c;
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
                o[k++] = '\n';
                space = true; /* drops the spaces after it */
                continue;
            }
            if (c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f') {
                if (!space) o[k++] = ' ';
                space = true;
                continue;
            }
            /* soft hyphens are invisible */
            if ((unsigned char)c == 0xC2 && i + 1 < n && (unsigned char)s[i + 1] == 0xAD) {
                i++;
                continue;
            }
            o[k++] = c;
            space = false;
        }
    }
    o[k] = 0;
    if (st->text_transform) transform(o, k, st->text_transform);
    *out_n = k;
    return o;
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
    for (node_t *c = n->first; c; c = c->next) {
        if (c->type != N_ELEM) continue;
        if (c->id && !strcmp(c->id, id)) return c;
        node_t *r = find_id(c, id);
        if (r) return r;
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

static void svg_ser(web_doc *d, sbuf *b, node_t *n, const char *color, int depth) {
    if (depth > 64) return;
    if (n->type == N_TEXT) {
        xml_escape(b, n->text, n->textlen, false);
        return;
    }
    if (n->type != N_ELEM) return;
    float viewbox[4];
    int view = n->tag == T_svg ? svg_viewbox(node_attr(n, "viewBox"), viewbox) : 0;
    if (view == 2) return; /* valid zero viewport suppresses its contents */
    if (!strcmp(n->name, "use")) { /* inline what it refers to: nanosvg has no <use> */
        const char *h = node_attr(n, "href");
        if (!h) {
            int index = doc_attr_index(n, "http://www.w3.org/1999/xlink", "href", true);
            if (index >= 0) h = n->attrs[index].value;
        }
        node_t *scope = doc_node_root(n, false);
        node_t *t = h && h[0] == '#' && scope ? find_id(scope, h + 1) : NULL;
        if (t && depth < 32) {
            sb_puts(b, "<g");
            const char *x = node_attr(n, "x"), *y = node_attr(n, "y");
            if (x || y) {
                char tr[96];
                snprintf(tr, sizeof tr, " transform=\"translate(%s %s)\"", x ? x : "0", y ? y : "0");
                sb_puts(b, tr);
            }
            const char *f = node_attr(n, "fill");
            if (f) {
                sb_puts(b, " fill=\"");
                xml_escape(b, strstr(f, "currentColor") ? color : f, strlen(strstr(f, "currentColor") ? color : f), true);
                sb_puts(b, "\"");
            }
            sb_puts(b, ">");
            if (!strcmp(t->name, "symbol"))
                for (node_t *c = t->first; c; c = c->next) svg_ser(d, b, c, color, depth + 1);
            else svg_ser(d, b, t, color, depth + 1);
            sb_puts(b, "</g>");
        }
        return;
    }
    sb_putc(b, '<');
    sb_puts(b, n->raw_name);
    bool xmlns = false;
    for (int i = 0; i < n->nattrs; i++) {
        const char *attribute_name = n->attrs[i].raw;
        const char *ns = n->attrs[i].namespace_uri, *local = n->attrs[i].local;
        /* The codec recognizes literal prefixes, not DOM namespaces. A colon
           in an ordinary DOM attribute must not fabricate an XML/XLink one. */
        if (!ns && strchr(attribute_name, ':')) continue;
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
    if (depth == 0) {
        if (!xmlns) sb_puts(b, " xmlns=\"http://www.w3.org/2000/svg\"");
        if (!node_attr(n, "fill")) {
            sb_puts(b, " fill=\"");
            sb_puts(b, "black");
            sb_puts(b, "\"");
        }
    }
    sb_putc(b, '>');
    for (node_t *c = n->first; c; c = c->next) svg_ser(d, b, c, color, depth + 1);
    sb_puts(b, "</");
    sb_puts(b, n->raw_name);
    sb_putc(b, '>');
}

struct svg_cache *doc_svg(web_doc *d, node_t *svg, uint32_t color) {
    for (int i = 0; i < d->svgs.n; i++) {
        struct svg_cache *c = d->svgs.v[i];
        if (c->node == svg) return c;
    }
    char hex[16];
    snprintf(hex, sizeof hex, "#%06x", color & 0xFFFFFF);
    sbuf b = {0};
    svg_ser(d, &b, svg, hex, 0);
    struct svg_cache *c = calloc(1, sizeof *c);
    c->node = svg;
    c->n = b.n;
    c->src = b.p ? b.p : strdup("");
    pv_push(&d->svgs, c);
    return c;
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
    case T_button: return AT_INLINE_BLOCK;
    case T_svg: return n->parent && n->parent->foreign ? AT_NONE : AT_SVG;
    case T_iframe: case T_video: case T_canvas: case T_embed: case T_object: case T_applet: case T_meter:
    case T_progress:
        return AT_PLACEHOLDER;
    case T_audio: return node_attr(n, "controls") ? AT_PLACEHOLDER : AT_NONE;
    }
    return AT_NONE;
}

static void gen(struct bctx *b, box_t *pb, node_t *n, style_t *pst);
static void fixup(struct bctx *b, box_t *x);

static void text_box(struct bctx *b, box_t *pb, node_t *n, const char *s, size_t len, style_t *st) {
    size_t k;
    const char *t = ws_process(b->a, s, len, st, &k);
    if (!k) return;
    box_t *x = nbox(b, B_TEXT, n, st);
    x->text = t;
    x->len = k;
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
    if (st->before) pseudo_box(b, pb, e, st->before);
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
    if (st->after) pseudo_box(b, pb, e, st->after);
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
    ws->bg_image = NULL;
    ws->has_grad = false;
    ws->gradient = NULL;
    ws->width.kind = LK_AUTO;
    ws->height.kind = LK_AUTO;
    style_t *is = ar_alloc(b->a, sizeof *is);
    *is = *st;
    is->display = inner_kind == B_TABLE ? D_TABLE : inner_kind == B_GRID ? D_GRID : D_FLEX;
    for (int i = 0; i < 4; i++) {
        is->margin[i].kind = LK_LEN;
        is->margin[i].px = is->margin[i].pct = 0;
    }
    box_t *w = nbox(b, B_ATOMIC, n, ws);
    w->atomic = AT_INLINE_BLOCK;
    box_t *in = nbox(b, inner_kind, n, is);
    children(b, in, n, is);
    fixup(b, in);
    append(w, in);
    fixup(b, w);
    return w;
}

static void gen(struct bctx *b, box_t *pb, node_t *n, style_t *pst) {
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
        append(pb, nbox(b, B_BR, n, st));
        return;
    }
    box_t *x;
    int at = atomic_kind(n);
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
    if (!n->box) n->box = x;
    append(pb, x);
}

/* ---------------------------------------------------------------- fixups */
static bool table_part(const box_t *c) {
    return !c->floated && !c->abspos &&
           (c->kind == B_ROW_GROUP || c->kind == B_ROW || c->kind == B_CELL || c->kind == B_CAPTION);
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

void boxes_build(web_doc *d, arena_t *a) {
    struct bctx b = {.d=d, .a=a};
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
}
