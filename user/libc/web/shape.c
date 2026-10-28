/* The shape values: <basic-shape> (css-shapes-1 §3: inset(), rect(), xywh(), circle(), ellipse(),
   polygon(), path(), shape()), a reference box alone, ray() (motion-1 §2.1.1) and <url>, which
   clip-path (css-masking-1 §5.1), shape-outside (css-shapes-1 §5.1) and offset-path (motion-1 §2.1)
   hold; the other Motion Path values (offset-position, offset-anchor, offset-rotate, the offset
   shorthand); and the CSSOM specified values of these and of offset-distance, shape-margin and
   shape-image-threshold. A value is parsed once into struct css_shape. Its computed value
   serializes, compares, interpolates and adds from that (css-shapes-1 §3.4, motion-1 §2.1), and
   becomes geometry for clipping, float wrapping and the offset transform (the last part). */
#include <stdio.h>
#include <math.h>
#include <ctype.h>
#include "webi.h"
#include "svg_geometry.h"

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

/* ---------------------------------------------------------------- words */
static void trim(const char **s, size_t *n) {
    while (*n && is_space((unsigned char)**s)) (*s)++, (*n)--;
    while (*n && is_space((unsigned char)(*s)[*n - 1])) (*n)--;
}
static bool kw(const char *s, size_t n, const char *k) { return strn_ieq(s, k, n); }
/* name(arguments): the arguments, NULL for anything else */
static const char *fn_args(const char *v, size_t n, const char *name, size_t *size) {
    size_t k = strlen(name);
    if (n < k + 2 || !strn_ieq(v, name, k) || v[k] != '(' || v[n - 1] != ')') return NULL;
    *size = n - k - 2;
    return v + k + 1;
}
/* The end of the comma-separated item at s: the next top-level comma, or end. */
static const char *item_end(const char *s, const char *end) {
    int depth = 0;
    char quote = 0;
    for (; s < end; s++) {
        char c = *s;
        if (quote) { if (c == '\\' && s + 1 < end) s++; else if (c == quote) quote = 0; }
        else if (c == '"' || c == '\'') quote = c;
        else if (c == '(') depth++;
        else if (c == ')') depth--;
        else if (c == ',' && depth <= 0) break;
    }
    return s;
}
#define MAXWORDS 24
/* The top-level words of s, a '/' a word of its own; -1 for more than max, unbalanced
   parentheses or a top-level comma (callers split items first). */
static int words(const char *s, size_t n, const char **w, size_t *wn, int max) {
    int k = 0;
    size_t i = 0;
    while (i < n) {
        while (i < n && is_space((unsigned char)s[i])) i++;
        if (i >= n) break;
        if (k == max) return -1;
        size_t start = i;
        if (s[i] == '/') i++;
        else {
            int depth = 0;
            char quote = 0;
            for (; i < n; i++) {
                char c = s[i];
                if (quote) { if (c == '\\' && i + 1 < n) i++; else if (c == quote) quote = 0; }
                else if (c == '"' || c == '\'') quote = c;
                else if (c == '(') depth++;
                else if (c == ')') { if (!depth) return -1; depth--; }
                else if (!depth && c == ',') return -1;
                else if (!depth && (is_space((unsigned char)c) || c == '/')) break;
            }
            if (quote || depth) return -1;
        }
        w[k] = s + start;
        wn[k++] = i - start;
    }
    return k;
}
static bool slash(const char *w, size_t n) { return n == 1 && *w == '/'; }

/* ---------------------------------------------------------------- numbers and lengths */
/* The shortest decimal that reads back as the same float, in fixed notation (CSSOM §6.7.2). */
static void put_num(sbuf *o, double v) {
    float f = (float)v;
    if (!isfinite(f)) f = 0;
    if (f == 0) f = 0;
    char t[80];
    int p = 1;
    for (; p < 9; p++) {
        snprintf(t, sizeof t, "%.*g", p, (double)f);
        if ((float)strtod(t, NULL) == f) break;
    }
    if (p == 9) snprintf(t, sizeof t, "%.9g", (double)f);
    if (strchr(t, 'e')) {
        int len = snprintf(t, sizeof t, fabsf(f) >= 1 ? "%.0f" : "%.12f", (double)f);
        if (strchr(t, '.')) {
            while (len > 1 && t[len - 1] == '0') t[--len] = 0;
            if (t[len - 1] == '.') t[--len] = 0;
        }
        if (!strcmp(t, "-0")) strcpy(t, "0");
    }
    sb_puts(o, t);
}
/* six significant digits, as a specified calc() keeps its folded value */
static void put_num6(sbuf *o, double v) {
    if (!isfinite(v) || fabs(v) < 5e-7) v = 0;
    char t[80];
    int len = snprintf(t, sizeof t, "%.6g", v);
    if (strchr(t, 'e')) {
        len = snprintf(t, sizeof t, fabs(v) >= 1 ? "%.0f" : "%.9f", v);
        if (strchr(t, '.')) {
            while (len > 1 && t[len - 1] == '0') t[--len] = 0;
            if (t[len - 1] == '.') t[--len] = 0;
        }
    }
    if (!strcmp(t, "-0")) strcpy(t, "0");
    sb_puts(o, t);
}
/* A zero percentage that stays one (0%, a keyword's, a difference that cancels) is pct -0. */
static bool has_pct(const len_t *l) { return l->kind == LK_EXPR || (l->kind == LK_LEN && (l->pct != 0 || signbit(l->pct))); }
static len_t percent(float p) { len_t l = {.kind = LK_LEN, .pct = p == 0 ? -0.0f : p}; return l; }
static len_t pixels(double px) { len_t l = {.kind = LK_LEN, .px = (float)px}; return l; }
static bool len_same(const len_t *a, const len_t *b) {
    return css_length_equal(a, b) && (a->kind != LK_LEN || a->pct != 0 || !signbit(a->pct) == !signbit(b->pct));
}
static bool len_zero(const len_t *l) { return l->kind == LK_LEN && l->px == 0 && l->pct == 0; }
static bool put_len(sbuf *o, const len_t *l, web_doc *d) {
    if (l->kind != LK_LEN) return css_length_serialize(l, o, d);
    if (!isfinite(l->px) || !isfinite(l->pct)) return false;
    if (l->pct == 0 && !signbit(l->pct)) { put_num(o, l->px); sb_puts(o, "px"); return true; }
    if (l->px == 0) { put_num(o, l->pct); sb_putc(o, '%'); return true; }
    sb_puts(o, "calc(");
    put_num(o, l->pct);
    sb_puts(o, l->px < 0 ? "% - " : "% + ");
    put_num(o, fabsf(l->px));
    sb_puts(o, "px)");
    return true;
}
/* A <length-percentage> word (neg: negative values too). */
static bool get_len(const char *t, size_t n, struct cx *cx, len_t *out, bool neg) {
    if (!css_parse_length(t, n, cx, out, LF_LENGTH | (neg ? LF_NEG : 0)) || (out->kind != LK_LEN && out->kind != LK_EXPR))
        return false;
    if (out->kind == LK_LEN) {
        if (!isfinite(out->px) || !isfinite(out->pct)) return false;
        if (out->pct == 0) out->pct = memchr(t, '%', n) ? -0.0f : 0.0f;
    }
    return true;
}
/* 100% - x - y */
static len_t complement(struct cx *cx, const len_t *x, const len_t *y) {
    len_t l;
    css_length_complement(cx->a, x, y, &l);
    if (l.kind == LK_LEN && l.pct == 0) l.pct = -0.0f;
    return l;
}
/* A math function's text as written, its white space collapsed */
static void put_authored(sbuf *o, const char *t, size_t n) {
    bool space = false, any = false;
    for (size_t i = 0; i < n; i++) {
        char c = t[i];
        if (is_space((unsigned char)c)) { space = true; continue; }
        if (space && any && t[i - 1] != '(' && c != ')') {
            size_t j = i; while (j > 0 && is_space((unsigned char)t[j - 1])) j--;
            if (t[j - 1] != '(') sb_putc(o, ' ');
        }
        space = false; any = true;
        sb_putc(o, c);
    }
}
/* The specified value of a <length-percentage> (angle: an <angle>) word: a number keeps its unit,
   unitless zero is 0px, a math function is its folded sum when it needs nothing but percentages
   (calc(100% / 4) is calc(25%)) and otherwise as written. */
static void spec_len(sbuf *o, const char *t, size_t n, struct cx *cx, bool angle) {
    if (!o) return;
    trim(&t, &n);
    const char *p = t;
    float v;
    if (css_number_token(&p, t + n, &v)) {
        put_num(o, v);
        if (p == t + n) sb_puts(o, angle ? "deg" : "px");
        for (; p < t + n; p++) sb_putc(o, (char)tolower((unsigned char)*p));
        return;
    }
    struct cx probe = *cx;
    probe.absolute_units = !angle;
    probe.angles = angle;
    probe.supports_probe = false;
    probe.invalid = false;
    len_t l;
    if (css_parse_length(t, n, &probe, &l, LF_NEG | LF_LENGTH | (angle ? LF_NOPCT : 0)) && l.kind == LK_LEN &&
        isfinite(l.px) && isfinite(l.pct)) {
        bool pct = l.pct != 0 || (!angle && l.px == 0 && memchr(t, '%', n));
        sb_puts(o, "calc(");
        if (pct) { put_num6(o, l.pct); sb_putc(o, '%'); }
        if (l.px != 0 || !pct) {
            if (pct) sb_puts(o, l.px < 0 ? " - " : " + ");
            put_num6(o, pct ? fabsf(l.px) : l.px);
            sb_puts(o, angle ? "deg" : "px");
        }
        sb_putc(o, ')');
        return;
    }
    put_authored(o, t, n);
}
static void spec_word(sbuf *o, const char *t, size_t n) {
    if (o) for (size_t i = 0; i < n; i++) sb_putc(o, (char)tolower((unsigned char)t[i]));
}
static void spec_puts(sbuf *o, const char *s) { if (o) sb_puts(o, s); }
/* An <angle> word, in degrees; a unitless number is not one here (offset-rotate, ray(), rotate). */
static bool get_angle(const char *t, size_t n, struct cx *cx, float *deg) {
    trim(&t, &n);
    if (!n || (!isalpha((unsigned char)t[n - 1]) && t[n - 1] != ')')) return false;
    struct cx a = *cx;
    a.angles = true;
    a.absolute_units = false;
    len_t l;
    if (!css_parse_length(t, n, &a, &l, LF_NEG | LF_LENGTH | LF_NOPCT) || l.kind != LK_LEN || l.pct != 0 || !isfinite(l.px))
        return false;
    *deg = l.px;
    return true;
}
static void put_angle(sbuf *o, double deg) { put_num(o, deg); sb_puts(o, "deg"); }
/* A CSS string's contents (CSSOM §6.6.2 "serialize a string"), without the quotes */
static void put_string(sbuf *o, const char *s) {
    for (; *s; s++) {
        unsigned char c = (unsigned char)*s;
        char escape[16];
        if (c < 0x20 || c == 0x7f) { snprintf(escape, sizeof escape, "\\%x ", c); sb_puts(o, escape); }
        else if (c == '"' || c == '\\') { sb_putc(o, '\\'); sb_putc(o, (char)c); }
        else sb_putc(o, (char)c);
    }
}

/* ---------------------------------------------------------------- <position> */
enum { W_LEFT, W_RIGHT, W_TOP, W_BOTTOM, W_CENTER, W_LEN, W_OTHER };
static int pos_word(const char *t, size_t n, struct cx *cx, len_t *l) {
    if (kw(t, n, "left")) return W_LEFT;
    if (kw(t, n, "right")) return W_RIGHT;
    if (kw(t, n, "top")) return W_TOP;
    if (kw(t, n, "bottom")) return W_BOTTOM;
    if (kw(t, n, "center")) return W_CENTER;
    len_t scratch;
    return get_len(t, n, cx, l ? l : &scratch, true) ? W_LEN : W_OTHER;
}
static bool is_x(int c) { return c == W_LEFT || c == W_RIGHT; }
static bool is_y(int c) { return c == W_TOP || c == W_BOTTOM; }
static len_t keyword_offset(int c) { return percent(c == W_CENTER ? 50 : c == W_RIGHT || c == W_BOTTOM ? 100 : 0); }
static void spec_pos_word(sbuf *o, const char *t, size_t n, int c, struct cx *cx) {
    if (c == W_LEN) spec_len(o, t, n, cx, false);
    else spec_word(o, t, n);
}
/* <position> (css-values-4 §9.1: one, two or four words) as offsets from the top left; its specified
   value pads one keyword with center and puts keywords horizontal first */
static bool position(const char **w, const size_t *wn, int k, struct cx *cx, len_t at[2], sbuf *spec) {
    int c[4];
    len_t l[4];
    if (k != 1 && k != 2 && k != 4) return false;
    for (int i = 0; i < k; i++) if ((c[i] = pos_word(w[i], wn[i], cx, &l[i])) == W_OTHER) return false;
    if (k == 1) {
        if (is_y(c[0])) {
            at[0] = percent(50); at[1] = keyword_offset(c[0]);
            spec_puts(spec, "center "); spec_word(spec, w[0], wn[0]);
        } else {
            at[0] = c[0] == W_LEN ? l[0] : keyword_offset(c[0]); at[1] = percent(50);
            spec_pos_word(spec, w[0], wn[0], c[0], cx); spec_puts(spec, " center");
        }
        return true;
    }
    if (k == 2) {
        bool x_first = (is_x(c[0]) || c[0] == W_CENTER || c[0] == W_LEN) && (is_y(c[1]) || c[1] == W_CENTER || c[1] == W_LEN);
        if (!x_first && !((is_y(c[0]) || c[0] == W_CENTER) && (is_x(c[1]) || c[1] == W_CENTER))) return false;
        int x = x_first ? 0 : 1, y = 1 - x;
        at[0] = c[x] == W_LEN ? l[x] : keyword_offset(c[x]);
        at[1] = c[y] == W_LEN ? l[y] : keyword_offset(c[y]);
        spec_pos_word(spec, w[x], wn[x], c[x], cx); spec_puts(spec, " "); spec_pos_word(spec, w[y], wn[y], c[y], cx);
        return true;
    }
    if (!(is_x(c[0]) || is_y(c[0])) || !(is_x(c[2]) || is_y(c[2])) || c[1] != W_LEN || c[3] != W_LEN || is_x(c[0]) == is_x(c[2]))
        return false;
    int x = is_x(c[0]) ? 0 : 2, y = 2 - x;
    at[0] = c[x] == W_LEFT ? l[x + 1] : complement(cx, &l[x + 1], NULL);
    at[1] = c[y] == W_TOP ? l[y + 1] : complement(cx, &l[y + 1], NULL);
    spec_word(spec, w[x], wn[x]); spec_puts(spec, " "); spec_len(spec, w[x + 1], wn[x + 1], cx, false); spec_puts(spec, " ");
    spec_word(spec, w[y], wn[y]); spec_puts(spec, " "); spec_len(spec, w[y + 1], wn[y + 1], cx, false);
    return true;
}
static bool put_pos(sbuf *o, const len_t at[2], web_doc *d) {
    if (!put_len(o, &at[0], d)) return false;
    sb_putc(o, ' ');
    return put_len(o, &at[1], d);
}

/* ---------------------------------------------------------------- boxes and keywords */
static const char *const box_names[] = {"border-box", "padding-box", "content-box", "margin-box", "fill-box", "stroke-box", "view-box"};
static int box_word(enum css_shape_property which, const char *t, size_t n) {
    for (int i = 0; i < 7; i++)
        if (kw(t, n, box_names[i])) {
            if (which == CSS_SHAPE_OUTSIDE && i > CSS_BOX_MARGIN) return -1;     /* <shape-box> */
            if (which == CSS_SHAPE_OFFSET_PATH && i == CSS_BOX_MARGIN) return -1; /* <coord-box> */
            return i;
        }
    return -1;
}
static uint8_t default_box(enum css_shape_property which) { return which == CSS_SHAPE_OUTSIDE ? CSS_BOX_MARGIN : CSS_BOX_BORDER; }
static const char *const extent_names[] = {"", "closest-side", "farthest-side", "closest-corner", "farthest-corner", "sides"};
static int extent_word(const char *t, size_t n, bool sides) {
    for (int i = CSS_EXTENT_CLOSEST_SIDE; i <= (sides ? CSS_EXTENT_SIDES : CSS_EXTENT_FARTHEST_CORNER); i++)
        if (kw(t, n, extent_names[i])) return i;
    return -1;
}
static int fill_rule(const char *t, size_t n) { return kw(t, n, "nonzero") ? 0 : kw(t, n, "evenodd") ? 1 : -1; }

/* ---------------------------------------------------------------- corner radii */
static const int pick[5][4] = {{0}, {0, 0, 0, 0}, {0, 1, 0, 1}, {0, 1, 2, 1}, {0, 1, 2, 3}};
/* the count of a TL TR BR BL list's shortest form, by same(i, j) */
static int shortest(bool (*same)(const void *, int, int), const void *list) {
    if (!same(list, 1, 3)) return 4;
    if (!same(list, 0, 2)) return 3;
    return same(list, 0, 1) ? 1 : 2;
}
static bool texts_same(const void *list, int i, int j) {
    const sbuf *t = list;
    return t[i].n == t[j].n && (!t[i].n || !memcmp(t[i].p, t[j].p, t[i].n));
}
static bool lens_same(const void *list, int i, int j) { const len_t *l = list; return len_same(&l[i], &l[j]); }
/* <'border-radius'> after round: 1-4 radii, and after a / 1-4 vertical ones, non-negative */
static bool radii(const char **w, const size_t *wn, int k, struct cx *cx, struct css_corner_radius corner[4], sbuf *spec) {
    int at = -1;
    for (int i = 0; i < k; i++) if (slash(w[i], wn[i])) { if (at >= 0) return false; at = i; }
    int count[2] = {at < 0 ? k : at, at < 0 ? 0 : k - at - 1};
    if (count[0] < 1 || count[0] > 4 || (at >= 0 && (count[1] < 1 || count[1] > 4))) return false;
    len_t v[2][4];
    for (int axis = 0; axis < 2; axis++) {
        int base = axis ? at + 1 : 0;
        for (int i = 0; i < count[axis]; i++) if (!get_len(w[base + i], wn[base + i], cx, &v[axis][i], false)) return false;
    }
    for (int c = 0; c < 4; c++) {
        corner[c].axis[0] = v[0][pick[count[0]][c]];
        corner[c].axis[1] = count[1] ? v[1][pick[count[1]][c]] : corner[c].axis[0];
    }
    if (!spec) return true;
    sbuf text[2][4] = {{{0}}};
    for (int axis = 0; axis < 2; axis++) {
        int n = count[axis ? 1 : 0] ? count[axis] : count[0], base = axis && count[1] ? at + 1 : 0;
        for (int c = 0; c < 4; c++) {
            int i = base + pick[n][c];
            spec_len(&text[axis][c], w[i], wn[i], cx, false);
        }
    }
    bool equal = true;
    for (int c = 0; c < 4; c++) equal &= texts_same(text, c, 4 + c);
    for (int axis = 0; axis < (equal ? 1 : 2); axis++) {
        if (axis) sb_puts(spec, " /");
        int n = shortest(texts_same, text[axis]);
        for (int c = 0; c < n; c++) { if (axis || c) sb_putc(spec, ' '); sb_put(spec, text[axis][c].p ? text[axis][c].p : "", text[axis][c].n); }
    }
    for (int axis = 0; axis < 2; axis++) for (int c = 0; c < 4; c++) sb_free(&text[axis][c]);
    return true;
}
static bool radii_zero(const struct css_corner_radius c[4]) {
    for (int i = 0; i < 4; i++) if (!len_zero(&c[i].axis[0]) || !len_zero(&c[i].axis[1])) return false;
    return true;
}
static bool put_radii(sbuf *o, const struct css_corner_radius c[4], web_doc *d) {
    len_t l[2][4];
    bool equal = true;
    for (int i = 0; i < 4; i++) { l[0][i] = c[i].axis[0]; l[1][i] = c[i].axis[1]; equal &= len_same(&l[0][i], &l[1][i]); }
    for (int axis = 0; axis < (equal ? 1 : 2); axis++) {
        if (axis) sb_puts(o, " /");
        int n = shortest(lens_same, l[axis]);
        for (int i = 0; i < n; i++) { if (axis || i) sb_putc(o, ' '); if (!put_len(o, &l[axis][i], d)) return false; }
    }
    return true;
}

/* ---------------------------------------------------------------- the shape functions */
/* [round <'border-radius'>]? closing inset(), rect(), xywh(): the number of words before it */
static bool rect_round(const char **w, const size_t *wn, int k, struct cx *cx, struct css_shape *s, int *before, sbuf *spec) {
    *before = k;
    for (int i = 0; i < k; i++)
        if (kw(w[i], wn[i], "round")) {
            *before = i;
            sbuf text = {0};
            bool ok = i + 1 < k && radii(w + i + 1, wn + i + 1, k - i - 1, cx, s->corner, spec ? &text : NULL);
            if (ok && spec && !radii_zero(s->corner)) { sb_puts(spec, " round "); sb_put(spec, text.p ? text.p : "", text.n); }
            sb_free(&text);
            return ok;
        }
    for (int c = 0; c < 4; c++) s->corner[c].axis[0] = s->corner[c].axis[1] = pixels(0);
    return true;
}
/* inset( <length-percentage>{1,4} [round <'border-radius'>]? ), and rect() and xywh(), which compute to it */
static bool rect_fn(int form, const char *args, size_t n, struct cx *cx, struct css_shape *s, sbuf *spec) {
    const char *w[MAXWORDS];
    size_t wn[MAXWORDS];
    int k = words(args, n, w, wn, MAXWORDS), sides;
    if (k < 1) return false;
    sbuf round = {0};
    if (!rect_round(w, wn, k, cx, s, &sides, spec ? &round : NULL)) { sb_free(&round); return false; }
    bool ok = false;
    len_t v[4];
    s->kind = CSS_SHAPE_INSET;
    if (form == 'i') {
        if (sides >= 1 && sides <= 4) {
            ok = true;
            for (int i = 0; i < sides && ok; i++) ok = get_len(w[i], wn[i], cx, &v[i], true);
            for (int i = 0; i < 4 && ok; i++) s->inset[i] = v[pick[sides][i]];
            if (ok && spec) {
                sbuf text[4] = {{0}};
                for (int i = 0; i < 4; i++) spec_len(&text[i], w[pick[sides][i]], wn[pick[sides][i]], cx, false);
                int count = shortest(texts_same, text);
                sb_puts(spec, "inset(");
                for (int i = 0; i < count; i++) { if (i) sb_putc(spec, ' '); sb_put(spec, text[i].p ? text[i].p : "", text[i].n); }
                for (int i = 0; i < 4; i++) sb_free(&text[i]);
            }
        }
    } else if (sides == 4) {
        ok = true;
        bool automatic[4] = {false};
        for (int i = 0; i < 4 && ok; i++) {
            if (form == 'r' && kw(w[i], wn[i], "auto")) { automatic[i] = true; continue; }
            ok = get_len(w[i], wn[i], cx, &v[i], form == 'r' || i < 2); /* xywh()'s width and height are non-negative */
        }
        if (ok && form == 'r') { /* rect(top right bottom left): inset(top, 100% - right, 100% - bottom, left) */
            s->inset[0] = automatic[0] ? percent(0) : v[0];
            s->inset[1] = automatic[1] ? percent(0) : complement(cx, &v[1], NULL);
            s->inset[2] = automatic[2] ? percent(0) : complement(cx, &v[2], NULL);
            s->inset[3] = automatic[3] ? percent(0) : v[3];
        } else if (ok) { /* xywh(x y w h): inset(y, 100% - x - w, 100% - y - h, x) */
            s->inset[0] = v[1];
            s->inset[1] = complement(cx, &v[0], &v[2]);
            s->inset[2] = complement(cx, &v[1], &v[3]);
            s->inset[3] = v[0];
        }
        if (ok && spec) {
            sb_puts(spec, form == 'r' ? "rect(" : "xywh(");
            for (int i = 0; i < 4; i++) {
                if (i) sb_putc(spec, ' ');
                if (automatic[i]) sb_puts(spec, "auto"); else spec_len(spec, w[i], wn[i], cx, false);
            }
        }
    }
    if (ok && spec) { sb_put(spec, round.p ? round.p : "", round.n); sb_putc(spec, ')'); }
    sb_free(&round);
    return ok;
}
/* circle( <radial-size>? [at <position>]? ), ellipse( [<radial-size>{2}]? [at <position>]? ) */
static bool radial_fn(int radii_n, const char *args, size_t n, struct cx *cx, struct css_shape *s, sbuf *spec) {
    const char *w[MAXWORDS];
    size_t wn[MAXWORDS];
    int k = words(args, n, w, wn, MAXWORDS);
    if (k < 0) return false;
    int at = k;
    for (int i = 0; i < k; i++) if (kw(w[i], wn[i], "at")) { at = i; break; }
    if (at && at != radii_n) return false;
    s->kind = radii_n == 1 ? CSS_SHAPE_CIRCLE : CSS_SHAPE_ELLIPSE;
    s->extent[0] = s->extent[1] = CSS_EXTENT_CLOSEST_SIDE;
    for (int i = 0; i < at; i++) {
        int e = extent_word(w[i], wn[i], false);
        if (e >= 0) s->extent[i] = (uint8_t)e;
        else if (get_len(w[i], wn[i], cx, &s->radius[i], false)) s->extent[i] = CSS_EXTENT_LENGTH;
        else return false;
    }
    bool closest = s->extent[0] == CSS_EXTENT_CLOSEST_SIDE && (radii_n == 1 || s->extent[1] == CSS_EXTENT_CLOSEST_SIDE);
    spec_puts(spec, radii_n == 1 ? "circle(" : "ellipse(");
    for (int i = 0; i < at && !closest; i++) {
        if (i) spec_puts(spec, " ");
        if (s->extent[i] == CSS_EXTENT_LENGTH) spec_len(spec, w[i], wn[i], cx, false);
        else spec_puts(spec, extent_names[s->extent[i]]);
    }
    s->at[0] = s->at[1] = percent(50);
    if (at < k) {
        s->at_given = true;
        spec_puts(spec, closest ? "at " : " at ");
        if (!position(w + at + 1, wn + at + 1, k - at - 1, cx, s->at, spec)) return false;
    }
    spec_puts(spec, ")");
    return true;
}
/* polygon( <'fill-rule'>? [round <length>]? , [<length-percentage> <length-percentage>]# ) */
static bool polygon_fn(const char *args, size_t n, struct cx *cx, struct css_shape *s, sbuf *spec) {
    const char *end = args + n, *first = item_end(args, end), *v = args;
    size_t items = 1;
    for (const char *q = first; q < end; q = item_end(q + 1, end)) items++;
    const char *w[MAXWORDS];
    size_t wn[MAXWORDS];
    int k = words(args, (size_t)(first - args), w, wn, MAXWORDS);
    s->kind = CSS_SHAPE_POLYGON;
    const char *round_word = NULL;
    size_t round_n = 0;
    if (k > 0 && (fill_rule(w[0], wn[0]) >= 0 || kw(w[0], wn[0], "round"))) {
        int i = 0;
        if (fill_rule(w[0], wn[0]) >= 0) { s->evenodd = fill_rule(w[0], wn[0]); i = 1; }
        if (i < k) {
            len_t r;
            if (!kw(w[i], wn[i], "round") || i + 2 != k || memchr(w[i + 1], '%', wn[i + 1]) ||
                !css_parse_length(w[i + 1], wn[i + 1], cx, &r, LF_LENGTH | LF_NOPCT) || r.kind != LK_LEN || !isfinite(r.px))
                return false;
            s->round = r.px;
            round_word = w[i + 1]; round_n = wn[i + 1];
            i += 2;
        }
        if (i != k || first == end) return false;
        v = first + 1;
        items--;
    }
    if (items > n) return false;
    len_t (*point)[2] = ar_alloc(cx->a, items * sizeof *point);
    spec_puts(spec, "polygon(");
    if (s->evenodd) spec_puts(spec, "evenodd");
    if (s->round > 0) { spec_puts(spec, s->evenodd ? " round " : "round "); spec_len(spec, round_word, round_n, cx, false); }
    if (s->evenodd || s->round > 0) spec_puts(spec, ", ");
    for (size_t i = 0; i < items; i++) {
        const char *e = item_end(v, end), *xy[3];
        size_t xyn[3];
        if (words(v, (size_t)(e - v), xy, xyn, 3) != 2 || !get_len(xy[0], xyn[0], cx, &point[i][0], true) ||
            !get_len(xy[1], xyn[1], cx, &point[i][1], true)) return false;
        if (spec) {
            if (i) sb_puts(spec, ", ");
            spec_len(spec, xy[0], xyn[0], cx, false); sb_putc(spec, ' '); spec_len(spec, xy[1], xyn[1], cx, false);
        }
        if (!(i & 63) && !cx->supports_probe && cx->node && !web_native_checkpoint(cx->node->owner)) return false;
        v = e + 1;
    }
    spec_puts(spec, ")");
    s->point = (const len_t (*)[2])point;
    s->count = items;
    return true;
}
static void put_path_data(sbuf *o, const struct svg_path_seg *seg, size_t count) {
    for (size_t i = 0; i < count; i++) {
        if (i) sb_putc(o, ' ');
        sb_putc(o, seg[i].cmd);
        for (int j = 0, arity = svg_path_arity(seg[i].cmd); j < arity; j++) { sb_putc(o, ' '); put_num(o, seg[i].v[j]); }
    }
}
/* path( [<'fill-rule'>,]? <string> ), the string SVG path data; offset-path's takes no fill rule */
static bool path_fn(enum css_shape_property which, const char *args, size_t n, struct cx *cx, struct css_shape *s, sbuf *spec) {
    const char *end = args + n, *e = item_end(args, end), *t = args;
    s->kind = CSS_SHAPE_PATH;
    if (e != end) {
        const char *rule = args;
        size_t size = (size_t)(e - args);
        trim(&rule, &size);
        int f = fill_rule(rule, size);
        if (f < 0 || which == CSS_SHAPE_OFFSET_PATH) return false;
        s->evenodd = f;
        t = e + 1;
        if (item_end(t, end) != end) return false;
    }
    size_t size = (size_t)(end - t);
    trim(&t, &size);
    if (size < 2 || (t[0] != '"' && t[0] != '\'') || t[size - 1] != t[0]) return false;
    for (size_t i = 1; i + 1 < size; i++) {
        if (t[i] == '\\') i++;
        else if (t[i] == t[0]) return false;
    }
    sbuf data = {0};
    css_unescape(&data, t + 1, size - 2);
    const char *text = sb_cstr(&data);
    long count = svg_path_data_parse(text, NULL, 0);
    if (count <= 0) { sb_free(&data); return false; }
    struct svg_path_seg *seg = ar_alloc(cx->a, (size_t)count * sizeof *seg);
    svg_path_data_parse(text, seg, (size_t)count);
    sb_free(&data);
    s->seg = seg;
    s->count = (size_t)count;
    if (spec) {
        sb_puts(spec, s->evenodd ? "path(evenodd, \"" : "path(\"");
        put_path_data(spec, seg, s->count);
        sb_puts(spec, "\")");
    }
    return true;
}
/* shape()'s command words (css-shapes-1 §3.1.5) */
static const char *const command_names[] = {"move", "line", "hline", "vline", "curve", "smooth", "arc", "close"};
static bool stop_word(const char *t, size_t n) {
    static const char *const stops[] = {"with", "of", "cw", "ccw", "large", "small", "rotate"};
    if (slash(t, n)) return true;
    for (size_t i = 0; i < sizeof stops / sizeof *stops; i++) if (kw(t, n, stops[i])) return true;
    return false;
}
static bool pair(const char **w, const size_t *wn, struct cx *cx, len_t out[2], sbuf *spec) {
    if (!get_len(w[0], wn[0], cx, &out[0], true) || !get_len(w[1], wn[1], cx, &out[1], true)) return false;
    if (spec) { spec_len(spec, w[0], wn[0], cx, false); sb_putc(spec, ' '); spec_len(spec, w[1], wn[1], cx, false); }
    return true;
}
/* <control-point> = <position> | <relative-control-point>; in a by command only the latter, its
   coordinates from the start unless from end or from origin says */
static bool control(const char **w, const size_t *wn, int k, struct cx *cx, struct css_shape_command *c, int index, sbuf *spec) {
    uint8_t anchor;
    if (k == 4 && kw(w[2], wn[2], "from")) {
        if (kw(w[3], wn[3], "start")) anchor = CSS_ANCHOR_START;
        else if (kw(w[3], wn[3], "end")) anchor = CSS_ANCHOR_END;
        else if (kw(w[3], wn[3], "origin")) anchor = c->by ? CSS_ANCHOR_ORIGIN : CSS_ANCHOR_POSITION;
        else return false;
        if (!pair(w, wn, cx, c->control[index], spec)) return false;
    } else if (c->by) {
        if (k != 2 || !pair(w, wn, cx, c->control[index], spec)) return false;
        anchor = CSS_ANCHOR_START;
    } else {
        if (!position(w, wn, k, cx, c->control[index], spec)) return false;
        anchor = CSS_ANCHOR_POSITION;
    }
    c->anchor[index] = anchor;
    if (anchor == CSS_ANCHOR_END) spec_puts(spec, " from end");
    else if (anchor == CSS_ANCHOR_ORIGIN) spec_puts(spec, " from origin");
    else if (anchor == CSS_ANCHOR_START && !c->by) spec_puts(spec, " from start");
    return true;
}
static bool command(const char **w, const size_t *wn, int k, struct cx *cx, struct css_shape_command *c, sbuf *spec) {
    int op = -1;
    for (int i = 0; i <= CSS_CMD_CLOSE; i++) if (kw(w[0], wn[0], command_names[i])) op = i;
    if (op < 0) return false;
    c->op = (uint8_t)op;
    spec_puts(spec, command_names[op]);
    if (op == CSS_CMD_CLOSE) return k == 1;
    if (k < 3) return false;
    if (kw(w[1], wn[1], "by")) c->by = true;
    else if (!kw(w[1], wn[1], "to")) return false;
    spec_puts(spec, c->by ? " by " : " to ");
    if (op == CSS_CMD_HLINE || op == CSS_CMD_VLINE) {
        if (k != 3) return false;
        int axis = op == CSS_CMD_VLINE;
        static const char *const ends[2][5] = {{"left", "x-start", "center", "right", "x-end"},
                                              {"top", "y-start", "center", "bottom", "y-end"}};
        static const float at[5] = {0, 0, 50, 100, 100};
        for (int i = 0; i < 5 && !c->by; i++)
            if (kw(w[2], wn[2], ends[axis][i])) { c->end[axis] = percent(at[i]); spec_word(spec, w[2], wn[2]); return true; }
        if (!get_len(w[2], wn[2], cx, &c->end[axis], true)) return false;
        spec_len(spec, w[2], wn[2], cx, false);
        return true;
    }
    int i = 2, j = 2;
    while (j < k && !stop_word(w[j], wn[j])) j++;
    if (c->by ? j - i != 2 || !pair(w + i, wn + i, cx, c->end, spec) : !position(w + i, wn + i, j - i, cx, c->end, spec)) return false;
    i = j;
    if (op == CSS_CMD_MOVE || op == CSS_CMD_LINE) return i == k;
    if (op == CSS_CMD_CURVE || op == CSS_CMD_SMOOTH) {
        if (i == k) return op == CSS_CMD_SMOOTH;
        if (!kw(w[i], wn[i], "with")) return false;
        spec_puts(spec, " with ");
        int max = op == CSS_CMD_CURVE ? 2 : 1;
        for (i++;;) {
            j = i;
            while (j < k && !slash(w[j], wn[j])) j++;
            if (c->controls == max || !control(w + i, wn + i, j - i, cx, c, c->controls, spec)) return false;
            c->controls++;
            if (j == k) return true;
            spec_puts(spec, " / ");
            i = j + 1;
        }
    }
    /* arc: [of <length-percentage>{1,2}] && <arc-sweep>? && <arc-size>? && [rotate <angle>]? */
    bool of = false, sweep = false, size = false, rotate = false;
    int radii_at = 0, rotate_at = 0;
    while (i < k) {
        if (kw(w[i], wn[i], "of")) {
            if (of) return false;
            of = true;
            radii_at = ++i;
            while (i < k && c->radii < 2 && !stop_word(w[i], wn[i])) {
                if (!get_len(w[i], wn[i], cx, &c->radius[c->radii], true)) return false;
                c->radii++; i++;
            }
            if (!c->radii) return false;
        } else if (kw(w[i], wn[i], "cw") || kw(w[i], wn[i], "ccw")) {
            if (sweep) return false;
            sweep = true; c->cw = kw(w[i], wn[i], "cw"); i++;
        } else if (kw(w[i], wn[i], "large") || kw(w[i], wn[i], "small")) {
            if (size) return false;
            size = true; c->large = kw(w[i], wn[i], "large"); i++;
        } else if (kw(w[i], wn[i], "rotate")) {
            if (rotate || i + 1 >= k || !get_angle(w[i + 1], wn[i + 1], cx, &c->rotate)) return false;
            rotate = true; rotate_at = i + 1; i += 2;
        } else return false;
    }
    if (!of) return false;
    if (c->radii == 1) c->radius[1] = c->radius[0];
    if (spec) {
        sb_puts(spec, " of ");
        for (int r = 0; r < c->radii; r++) { if (r) sb_putc(spec, ' '); spec_len(spec, w[radii_at + r], wn[radii_at + r], cx, false); }
        if (c->cw) sb_puts(spec, " cw");
        if (c->large) sb_puts(spec, " large");
        if (rotate && c->rotate != 0) { sb_puts(spec, " rotate "); spec_len(spec, w[rotate_at], wn[rotate_at], cx, true); }
    }
    return true;
}
/* shape( <'fill-rule'>? from <position>, <shape-command># ) */
static bool shape_fn(const char *args, size_t n, struct cx *cx, struct css_shape *s, sbuf *spec) {
    const char *end = args + n, *first = item_end(args, end);
    const char *w[MAXWORDS];
    size_t wn[MAXWORDS];
    int k = words(args, (size_t)(first - args), w, wn, MAXWORDS), i = 0;
    s->kind = CSS_SHAPE_SHAPE;
    if (k > 0 && fill_rule(w[0], wn[0]) >= 0) { s->evenodd = fill_rule(w[0], wn[0]); i = 1; }
    if (i >= k || !kw(w[i], wn[i], "from") || first == end) return false;
    spec_puts(spec, s->evenodd ? "shape(evenodd from " : "shape(from ");
    if (!position(w + i + 1, wn + i + 1, k - i - 1, cx, s->at, spec)) return false;
    size_t count = 0;
    for (const char *q = first; q < end; q = item_end(q + 1, end)) count++;
    if (count > n) return false;
    struct css_shape_command *c = ar_alloc(cx->a, count * sizeof *c);
    const char *v = first + 1;
    for (size_t j = 0; j < count; j++) {
        const char *e = item_end(v, end);
        spec_puts(spec, ", ");
        if ((k = words(v, (size_t)(e - v), w, wn, MAXWORDS)) < 1 || !command(w, wn, k, cx, &c[j], spec)) return false;
        if (!(j & 63) && !cx->supports_probe && cx->node && !web_native_checkpoint(cx->node->owner)) return false;
        v = e + 1;
    }
    spec_puts(spec, ")");
    s->command = c;
    s->count = count;
    return true;
}
/* ray( <angle> && <ray-size>? && contain? && [at <position>]? ) */
static bool ray_fn(const char *args, size_t n, struct cx *cx, struct css_shape *s, sbuf *spec) {
    const char *w[MAXWORDS];
    size_t wn[MAXWORDS];
    int k = words(args, n, w, wn, MAXWORDS), angle_at = -1, at = -1, at_n = 0;
    bool size = false;
    s->kind = CSS_SHAPE_RAY;
    s->extent[0] = CSS_EXTENT_CLOSEST_SIDE;
    s->at[0] = s->at[1] = percent(50);
    if (k < 1) return false;
    for (int i = 0; i < k;) {
        int e;
        if (kw(w[i], wn[i], "contain")) {
            if (s->contain) return false;
            s->contain = true; i++;
        } else if (kw(w[i], wn[i], "at")) {
            if (at >= 0) return false;
            at = ++i;
            while (i < k && i - at < 4 && pos_word(w[i], wn[i], cx, NULL) != W_OTHER) i++;
            at_n = i - at;
            if (!position(w + at, wn + at, at_n, cx, s->at, NULL)) return false;
            s->at_given = true;
        } else if ((e = extent_word(w[i], wn[i], true)) >= 0) {
            if (size) return false;
            size = true; s->extent[0] = (uint8_t)e; i++;
        } else if (angle_at < 0 && get_angle(w[i], wn[i], cx, &s->angle)) {
            angle_at = i++;
        } else return false;
    }
    if (angle_at < 0) return false;
    if (spec) {
        sb_puts(spec, "ray(");
        spec_len(spec, w[angle_at], wn[angle_at], cx, true);
        if (s->extent[0] != CSS_EXTENT_CLOSEST_SIDE) { sb_putc(spec, ' '); sb_puts(spec, extent_names[s->extent[0]]); }
        if (s->contain) sb_puts(spec, " contain");
        if (at >= 0) { len_t ignored[2]; sb_puts(spec, " at "); position(w + at, wn + at, at_n, cx, ignored, spec); }
        sb_putc(spec, ')');
    }
    return true;
}
static bool shape_word(enum css_shape_property which, const char *t, size_t n, struct cx *cx, struct css_shape *s, sbuf *spec) {
    size_t an;
    const char *a;
    if (n >= 5 && strn_ieq(t, "url(", 4) && t[n - 1] == ')') {
        if (!(s->url = css_parse_url(t, n, cx->a))) return false;
        s->kind = CSS_SHAPE_URL;
        if (spec) { sb_puts(spec, "url(\""); put_string(spec, s->url); sb_puts(spec, "\")"); }
        return true;
    }
    if ((a = fn_args(t, n, "inset", &an))) return rect_fn('i', a, an, cx, s, spec);
    if ((a = fn_args(t, n, "rect", &an))) return rect_fn('r', a, an, cx, s, spec);
    if ((a = fn_args(t, n, "xywh", &an))) return rect_fn('x', a, an, cx, s, spec);
    if ((a = fn_args(t, n, "circle", &an))) return radial_fn(1, a, an, cx, s, spec);
    if ((a = fn_args(t, n, "ellipse", &an))) return radial_fn(2, a, an, cx, s, spec);
    if ((a = fn_args(t, n, "polygon", &an))) return polygon_fn(a, an, cx, s, spec);
    if ((a = fn_args(t, n, "path", &an))) return path_fn(which, a, an, cx, s, spec);
    if ((a = fn_args(t, n, "shape", &an))) return shape_fn(a, an, cx, s, spec);
    if ((a = fn_args(t, n, "ray", &an))) return which == CSS_SHAPE_OFFSET_PATH && ray_fn(a, an, cx, s, spec);
    return false;
}
/* clip-path: none | <url> | <basic-shape> || <geometry-box>; shape-outside: none | <basic-shape> ||
   <shape-box> | <image> (url() here); offset-path: none | <offset-path> || <coord-box> */
bool css_shape_parse(enum css_shape_property which, const char *v, size_t n, struct cx *cx,
                     const struct css_shape **out, sbuf *spec) {
    trim(&v, &n);
    if (kw(v, n, "none")) { *out = NULL; spec_puts(spec, "none"); return true; }
    const char *w[3];
    size_t wn[3];
    int k = words(v, n, w, wn, 2), shape = -1;
    if (k < 1) return false;
    struct css_shape *s = ar_alloc(cx->a, sizeof *s);
    s->box = default_box(which);
    bool box = false;
    for (int i = 0; i < k; i++) {
        int b = box_word(which, w[i], wn[i]);
        if (b >= 0) { if (box) return false; box = true; s->box = (uint8_t)b; }
        else if (shape >= 0) return false;
        else shape = i;
    }
    size_t begin = spec ? spec->n : 0;
    if (shape >= 0 && !shape_word(which, w[shape], wn[shape], cx, s, spec)) {
        if (spec) spec->n = begin;
        return false;
    }
    if (shape < 0) s->kind = CSS_SHAPE_BOX;
    if (s->kind == CSS_SHAPE_URL && box && which != CSS_SHAPE_OFFSET_PATH) { if (spec) spec->n = begin; return false; }
    if (spec && box && (shape < 0 || s->box != default_box(which))) {
        if (shape >= 0) sb_putc(spec, ' ');
        sb_puts(spec, box_names[s->box]);
    }
    *out = s;
    return true;
}

/* ---------------------------------------------------------------- computed values */
static bool put_shape_function(sbuf *o, const struct css_shape *s, web_doc *d) {
    switch (s->kind) {
    case CSS_SHAPE_URL: sb_puts(o, "url(\""); put_string(o, s->url); sb_puts(o, "\")"); return true;
    case CSS_SHAPE_BOX: sb_puts(o, box_names[s->box]); return true;
    case CSS_SHAPE_INSET: {
        sb_puts(o, "inset(");
        int count = shortest(lens_same, s->inset);
        for (int i = 0; i < count; i++) { if (i) sb_putc(o, ' '); if (!put_len(o, &s->inset[i], d)) return false; }
        if (!radii_zero(s->corner)) { sb_puts(o, " round "); if (!put_radii(o, s->corner, d)) return false; }
        sb_putc(o, ')');
        return true;
    }
    case CSS_SHAPE_CIRCLE: case CSS_SHAPE_ELLIPSE: {
        int radii_n = s->kind == CSS_SHAPE_CIRCLE ? 1 : 2;
        bool closest = s->extent[0] == CSS_EXTENT_CLOSEST_SIDE && (radii_n == 1 || s->extent[1] == CSS_EXTENT_CLOSEST_SIDE);
        sb_puts(o, radii_n == 1 ? "circle(" : "ellipse(");
        for (int i = 0; i < radii_n && !closest; i++) {
            if (i) sb_putc(o, ' ');
            if (s->extent[i] != CSS_EXTENT_LENGTH) sb_puts(o, extent_names[s->extent[i]]);
            else if (!put_len(o, &s->radius[i], d)) return false;
        }
        if (s->at_given) { sb_puts(o, closest ? "at " : " at "); if (!put_pos(o, s->at, d)) return false; }
        sb_putc(o, ')');
        return true;
    }
    case CSS_SHAPE_POLYGON:
        sb_puts(o, "polygon(");
        if (s->evenodd) sb_puts(o, "evenodd");
        if (s->round > 0) { sb_puts(o, s->evenodd ? " round " : "round "); put_num(o, s->round); sb_puts(o, "px"); }
        if (s->evenodd || s->round > 0) sb_puts(o, ", ");
        for (size_t i = 0; i < s->count; i++) {
            if (!(i & 63) && d && !web_native_checkpoint(d)) return false;
            if (i) sb_puts(o, ", ");
            if (!put_pos(o, s->point[i], d)) return false;
        }
        sb_putc(o, ')');
        return true;
    case CSS_SHAPE_PATH:
        sb_puts(o, s->evenodd ? "path(evenodd, \"" : "path(\"");
        put_path_data(o, s->seg, s->count);
        sb_puts(o, "\")");
        return true;
    case CSS_SHAPE_SHAPE:
        sb_puts(o, s->evenodd ? "shape(evenodd from " : "shape(from ");
        if (!put_pos(o, s->at, d)) return false;
        for (size_t i = 0; i < s->count; i++) {
            const struct css_shape_command *c = &s->command[i];
            if (!(i & 63) && d && !web_native_checkpoint(d)) return false;
            sb_puts(o, ", ");
            sb_puts(o, command_names[c->op]);
            if (c->op == CSS_CMD_CLOSE) continue;
            sb_puts(o, c->by ? " by " : " to ");
            if (c->op == CSS_CMD_HLINE || c->op == CSS_CMD_VLINE) {
                if (!put_len(o, &c->end[c->op == CSS_CMD_VLINE], d)) return false;
                continue;
            }
            if (!put_pos(o, c->end, d)) return false;
            for (int j = 0; j < c->controls; j++) {
                sb_puts(o, j ? " / " : " with ");
                if (!put_pos(o, c->control[j], d)) return false;
                uint8_t a = c->anchor[j];
                if (a == CSS_ANCHOR_END) sb_puts(o, " from end");
                else if (a == CSS_ANCHOR_ORIGIN) sb_puts(o, " from origin");
                else if (a == CSS_ANCHOR_START && !c->by) sb_puts(o, " from start");
            }
            if (c->op == CSS_CMD_ARC) {
                sb_puts(o, " of ");
                for (int r = 0; r < c->radii; r++) { if (r) sb_putc(o, ' '); if (!put_len(o, &c->radius[r], d)) return false; }
                if (c->cw) sb_puts(o, " cw");
                if (c->large) sb_puts(o, " large");
                if (c->rotate != 0) { sb_puts(o, " rotate "); put_angle(o, c->rotate); }
            }
        }
        sb_putc(o, ')');
        return true;
    case CSS_SHAPE_RAY:
        sb_puts(o, "ray(");
        put_angle(o, s->angle);
        if (s->extent[0] != CSS_EXTENT_CLOSEST_SIDE) { sb_putc(o, ' '); sb_puts(o, extent_names[s->extent[0]]); }
        if (s->contain) sb_puts(o, " contain");
        if (s->at_given) { sb_puts(o, " at "); if (!put_pos(o, s->at, d)) return false; }
        sb_putc(o, ')');
        return true;
    }
    return false;
}
/* the shape, then its reference box unless it is the default one (css-shapes-1 §3.3) */
bool css_shape_serialize(enum css_shape_property which, const struct css_shape *s, sbuf *out, web_doc *d) {
    size_t begin = out->n;
    bool ok = true;
    if (!s) sb_puts(out, "none");
    else {
        ok = put_shape_function(out, s, d);
        if (ok && s->kind != CSS_SHAPE_BOX && s->box != default_box(which)) { sb_putc(out, ' '); sb_puts(out, box_names[s->box]); }
    }
    if (!ok) out->n = begin;
    sb_cstr(out);
    return ok;
}
static bool commands_equal(const struct css_shape_command *x, const struct css_shape_command *y) {
    if (x->op != y->op || x->controls != y->controls || x->radii != y->radii || x->anchor[0] != y->anchor[0] ||
        x->anchor[1] != y->anchor[1] || x->by != y->by || x->cw != y->cw || x->large != y->large || x->rotate != y->rotate)
        return false;
    for (int i = 0; i < 2; i++)
        if (!len_same(&x->end[i], &y->end[i]) || !len_same(&x->radius[i], &y->radius[i]) ||
            !len_same(&x->control[0][i], &y->control[0][i]) || !len_same(&x->control[1][i], &y->control[1][i])) return false;
    return true;
}
bool css_shape_equal(const struct css_shape *a, const struct css_shape *b) {
    if (a == b) return true;
    if (!a || !b || a->kind != b->kind || a->box != b->box || a->evenodd != b->evenodd || a->at_given != b->at_given ||
        a->contain != b->contain || a->extent[0] != b->extent[0] || a->extent[1] != b->extent[1] || a->angle != b->angle ||
        a->round != b->round || a->count != b->count) return false;
    if ((a->url || b->url) && (!a->url || !b->url || strcmp(a->url, b->url))) return false;
    for (int i = 0; i < 2; i++) if (!len_same(&a->radius[i], &b->radius[i]) || !len_same(&a->at[i], &b->at[i])) return false;
    for (int i = 0; i < 4; i++)
        if (!len_same(&a->inset[i], &b->inset[i]) || !len_same(&a->corner[i].axis[0], &b->corner[i].axis[0]) ||
            !len_same(&a->corner[i].axis[1], &b->corner[i].axis[1])) return false;
    for (size_t i = 0; i < a->count; i++) {
        bool same = a->kind == CSS_SHAPE_POLYGON ? len_same(&a->point[i][0], &b->point[i][0]) && len_same(&a->point[i][1], &b->point[i][1]) :
            a->kind == CSS_SHAPE_PATH ? !memcmp(&a->seg[i], &b->seg[i], sizeof a->seg[i]) :
            a->kind == CSS_SHAPE_SHAPE ? commands_equal(&a->command[i], &b->command[i]) : true;
        if (!same) return false;
    }
    return true;
}

/* ---------------------------------------------------------------- interpolation and addition */
struct mix { arena_t *a; double ka, kb, t; bool add; };
/* ka·x + kb·y; a zero percentage stays one when both had one */
static void mix_len(const struct mix *m, const len_t *x, const len_t *y, bool nonnegative, len_t *out) {
    css_length_mix(m->a, x, y, m->ka, m->kb, out);
    if (out->kind != LK_LEN) return;
    if (nonnegative && fabsf(out->pct) < 1e-6f && out->px < 0) out->px = 0;
    if (nonnegative && fabsf(out->px) < 1e-6f && out->pct < 0) out->pct = 0;
    if (out->pct == 0) out->pct = has_pct(x) || has_pct(y) ? -0.0f : 0.0f;
}
static void mix_pos(const struct mix *m, const len_t x[2], const len_t y[2], len_t out[2]) {
    mix_len(m, &x[0], &y[0], false, &out[0]);
    mix_len(m, &x[1], &y[1], false, &out[1]);
}
static double mix_num(const struct mix *m, double x, double y) { return x * m->ka + y * m->kb; }
/* two different flags: the first's at or before 0, the second's at or after 1, set between (css-shapes-1 §3.4) */
static bool mix_flag(const struct mix *m, bool x, bool y, bool *out) {
    if (x == y) { *out = x; return true; }
    if (m->add) return false;
    *out = m->t <= 0 ? x : m->t >= 1 ? y : true;
    return true;
}
/* path data with absolute commands only (SVG 2 §9.3), the reference box's origin at 0 0 */
static struct svg_path_seg *absolute_path(arena_t *a, const struct svg_path_seg *seg, size_t count) {
    struct svg_path_seg *out = ar_alloc(a, count * sizeof *out);
    double x = 0, y = 0, sx = 0, sy = 0;
    for (size_t i = 0; i < count; i++) {
        struct svg_path_seg s = seg[i];
        bool rel = s.cmd >= 'a' && s.cmd <= 'z';
        char up = (char)toupper((unsigned char)s.cmd);
        if (rel) {
            switch (up) {
            case 'H': s.v[0] += (float)x; break;
            case 'V': s.v[0] += (float)y; break;
            case 'A': s.v[5] += (float)x; s.v[6] += (float)y; break;
            default: for (int j = 0, arity = svg_path_arity(s.cmd); j + 1 < arity; j += 2) { s.v[j] += (float)x; s.v[j + 1] += (float)y; }
            }
        }
        s.cmd = up;
        switch (up) {
        case 'Z': x = sx; y = sy; break;
        case 'H': x = s.v[0]; break;
        case 'V': y = s.v[0]; break;
        case 'A': x = s.v[5]; y = s.v[6]; break;
        default: { int arity = svg_path_arity(up); x = s.v[arity - 2]; y = s.v[arity - 1]; }
        }
        if (up == 'M') { sx = x; sy = y; }
        out[i] = s;
    }
    return out;
}
/* path data as shape() commands (css-shapes-1 §3.4: they interpolate as one another) */
static bool path_as_shape(arena_t *a, const struct css_shape *p, struct css_shape *out) {
    if (!p->count || (p->seg[0].cmd | 32) != 'm') return false;
    *out = *p;
    out->kind = CSS_SHAPE_SHAPE;
    out->at[0] = pixels(p->seg[0].v[0]);
    out->at[1] = pixels(p->seg[0].v[1]);
    struct css_shape_command *c = ar_alloc(a, (p->count - 1 + 1) * sizeof *c);
    for (size_t i = 1; i < p->count; i++) {
        const struct svg_path_seg *s = &p->seg[i];
        struct css_shape_command *k = &c[i - 1];
        const float *v = s->v;
        k->by = s->cmd >= 'a' && s->cmd <= 'z';
        uint8_t anchor = k->by ? CSS_ANCHOR_START : CSS_ANCHOR_POSITION;
        k->anchor[0] = k->anchor[1] = anchor;
        switch (s->cmd | 32) {
        case 'z': k->op = CSS_CMD_CLOSE; k->by = false; break;
        case 'm': case 'l': k->op = (s->cmd | 32) == 'm' ? CSS_CMD_MOVE : CSS_CMD_LINE; k->end[0] = pixels(v[0]); k->end[1] = pixels(v[1]); break;
        case 'h': k->op = CSS_CMD_HLINE; k->end[0] = pixels(v[0]); break;
        case 'v': k->op = CSS_CMD_VLINE; k->end[1] = pixels(v[0]); break;
        case 'c': k->op = CSS_CMD_CURVE; k->controls = 2;
            k->control[0][0] = pixels(v[0]); k->control[0][1] = pixels(v[1]); k->control[1][0] = pixels(v[2]); k->control[1][1] = pixels(v[3]);
            k->end[0] = pixels(v[4]); k->end[1] = pixels(v[5]); break;
        case 'q': case 's': k->op = (s->cmd | 32) == 'q' ? CSS_CMD_CURVE : CSS_CMD_SMOOTH; k->controls = 1;
            k->control[0][0] = pixels(v[0]); k->control[0][1] = pixels(v[1]); k->end[0] = pixels(v[2]); k->end[1] = pixels(v[3]); break;
        case 't': k->op = CSS_CMD_SMOOTH; k->end[0] = pixels(v[0]); k->end[1] = pixels(v[1]); break;
        case 'a': k->op = CSS_CMD_ARC; k->radii = 2; k->radius[0] = pixels(v[0]); k->radius[1] = pixels(v[1]);
            k->rotate = v[2]; k->large = v[3] != 0; k->cw = v[4] != 0; k->end[0] = pixels(v[5]); k->end[1] = pixels(v[6]); break;
        default: return false;
        }
    }
    out->command = c;
    out->count = p->count - 1;
    return true;
}
static bool mix_commands(const struct mix *m, const struct css_shape *a, const struct css_shape *b, struct css_shape *r) {
    if (a->evenodd != b->evenodd || a->count != b->count) return false;
    struct css_shape_command *c = ar_alloc(m->a, (a->count + 1) * sizeof *c);
    mix_pos(m, a->at, b->at, r->at);
    for (size_t i = 0; i < a->count; i++) {
        const struct css_shape_command *x = &a->command[i], *y = &b->command[i];
        struct css_shape_command z = *x;
        if (x->op != y->op || x->by != y->by || x->controls != y->controls) return false;
        for (int j = 0; j < x->controls; j++) {
            if (x->anchor[j] != y->anchor[j]) return false;
            mix_pos(m, x->control[j], y->control[j], z.control[j]);
        }
        mix_pos(m, x->end, y->end, z.end);
        if (x->op == CSS_CMD_ARC) {
            if (x->radii != y->radii) z.radii = 2;
            mix_len(m, &x->radius[0], &y->radius[0], false, &z.radius[0]);
            mix_len(m, &x->radius[1], &y->radius[1], false, &z.radius[1]);
            z.rotate = (float)mix_num(m, x->rotate, y->rotate);
            if (!mix_flag(m, x->cw, y->cw, &z.cw) || !mix_flag(m, x->large, y->large, &z.large)) return false;
        }
        c[i] = z;
    }
    r->kind = CSS_SHAPE_SHAPE;
    r->command = c;
    return true;
}
static bool mix_shapes(const struct mix *m, const struct css_shape *a, const struct css_shape *b, struct css_shape *r) {
    *r = *a;
    bool pa = a->kind == CSS_SHAPE_PATH, pb = b->kind == CSS_SHAPE_PATH;
    if ((pa || a->kind == CSS_SHAPE_SHAPE) && (pb || b->kind == CSS_SHAPE_SHAPE)) {
        if (pa && pb) { /* both path(): absolute path data, command by command */
            if (a->evenodd != b->evenodd || a->count != b->count) return false;
            struct svg_path_seg *x = absolute_path(m->a, a->seg, a->count), *y = absolute_path(m->a, b->seg, b->count);
            for (size_t i = 0; i < a->count; i++) {
                if (x[i].cmd != y[i].cmd) return false;
                for (int j = 0, arity = svg_path_arity(x[i].cmd); j < arity; j++) {
                    if (x[i].cmd == 'A' && (j == 3 || j == 4)) {
                        bool flag;
                        if (!mix_flag(m, x[i].v[j] != 0, y[i].v[j] != 0, &flag)) return false;
                        x[i].v[j] = flag;
                    } else x[i].v[j] = (float)mix_num(m, x[i].v[j], y[i].v[j]);
                }
            }
            r->seg = x;
            return true;
        }
        struct css_shape sa, sb;
        if (pa ? !path_as_shape(m->a, a, &sa) : (sa = *a, false)) return false;
        if (pb ? !path_as_shape(m->a, b, &sb) : (sb = *b, false)) return false;
        *r = sa;
        return mix_commands(m, &sa, &sb, r);
    }
    if (a->kind != b->kind) return false;
    switch (a->kind) {
    case CSS_SHAPE_INSET:
        for (int i = 0; i < 4; i++) {
            mix_len(m, &a->inset[i], &b->inset[i], false, &r->inset[i]);
            for (int axis = 0; axis < 2; axis++) mix_len(m, &a->corner[i].axis[axis], &b->corner[i].axis[axis], true, &r->corner[i].axis[axis]);
        }
        return true;
    case CSS_SHAPE_CIRCLE: case CSS_SHAPE_ELLIPSE:
        for (int i = 0; i < (a->kind == CSS_SHAPE_CIRCLE ? 1 : 2); i++) {
            if (a->extent[i] != CSS_EXTENT_LENGTH || b->extent[i] != CSS_EXTENT_LENGTH) return false;
            mix_len(m, &a->radius[i], &b->radius[i], true, &r->radius[i]);
        }
        mix_pos(m, a->at, b->at, r->at);
        r->at_given = a->at_given || b->at_given;
        return true;
    case CSS_SHAPE_POLYGON: {
        if (a->count != b->count || a->evenodd != b->evenodd) return false;
        len_t (*point)[2] = ar_alloc(m->a, (a->count + 1) * sizeof *point);
        for (size_t i = 0; i < a->count; i++) mix_pos(m, a->point[i], b->point[i], point[i]);
        r->point = (const len_t (*)[2])point;
        r->round = (float)fmax(0, mix_num(m, a->round, b->round));
        return true;
    }
    case CSS_SHAPE_RAY:
        if (a->extent[0] != b->extent[0] || a->contain != b->contain || a->at_given != b->at_given) return false;
        r->angle = (float)mix_num(m, a->angle, b->angle);
        mix_pos(m, a->at, b->at, r->at);
        return true;
    }
    return false;
}
bool css_shape_combine(enum css_shape_property which, const struct css_shape *a, const struct css_shape *b, double t,
                       bool add, sbuf *out, web_doc *d) {
    if (!a || !b || a->box != b->box) return false;
    arena_t arena = {.chunk_size = 4096};
    jmp_buf trap;
    arena.trap = &trap;
    size_t begin = out->n;
    volatile bool ok = false;
    if (!setjmp(trap)) {
        struct mix m = {&arena, add ? 1 : 1 - t, add ? 1 : t, t, add};
        struct css_shape r;
        if (mix_shapes(&m, a, b, &r)) ok = css_shape_serialize(which, &r, out, d);
    }
    ar_free(&arena);
    if (!ok) { out->n = begin; sb_cstr(out); }
    return ok;
}

/* ---------------------------------------------------------------- the style's groups */
/* CSS Shapes 1: shape-outside none, shape-margin 0, shape-image-threshold 0 */
const struct css_float_shape css_float_shape_initial = {.margin = {.kind = LK_LEN}};
/* motion-1 §2: offset-path none, offset-distance 0, offset-position normal, offset-anchor auto,
   offset-rotate auto */
const struct css_offset css_offset_initial = {
    .distance = {.kind = LK_LEN},
    .position = {{.kind = LK_NORMAL}, {.kind = LK_NORMAL}},
    .anchor = {{.kind = LK_AUTO}, {.kind = LK_AUTO}},
    .rotate = {0, true},
};
bool css_float_shape_equal(const struct css_float_shape *a, const struct css_float_shape *b) {
    if (a == b) return true;
    if (!a) a = &css_float_shape_initial;
    if (!b) b = &css_float_shape_initial;
    return css_shape_equal(a->outside, b->outside) && len_same(&a->margin, &b->margin) &&
           a->image_threshold == b->image_threshold;
}
bool css_offset_equal(const struct css_offset *a, const struct css_offset *b) {
    if (a == b) return true;
    if (!a) a = &css_offset_initial;
    if (!b) b = &css_offset_initial;
    if (!css_shape_equal(a->path, b->path) || !len_same(&a->distance, &b->distance) ||
        a->rotate.angle != b->rotate.angle || a->rotate.auto_rotate != b->rotate.auto_rotate) return false;
    for (int i = 0; i < 2; i++)
        if (!len_same(&a->position[i], &b->position[i]) || !len_same(&a->anchor[i], &b->anchor[i])) return false;
    return true;
}

/* ---------------------------------------------------------------- Motion Path values */
static void put_rotate(sbuf *o, const struct css_offset_rotate *r) {
    if (r->auto_rotate) { sb_puts(o, "auto"); if (r->angle != 0) { sb_putc(o, ' '); put_angle(o, r->angle); } }
    else put_angle(o, r->angle);
}
/* offset-rotate: [auto | reverse] || <angle>; reverse is auto 180deg */
static bool rotate_value(const char **w, const size_t *wn, int k, struct cx *cx, struct css_offset_rotate *r, sbuf *spec) {
    int keyword = -1, angle = -1;
    float deg = 0;
    if (k < 1 || k > 2) return false;
    for (int i = 0; i < k; i++) {
        if (kw(w[i], wn[i], "auto") || kw(w[i], wn[i], "reverse")) { if (keyword >= 0) return false; keyword = i; }
        else if (angle < 0 && get_angle(w[i], wn[i], cx, &deg)) angle = i;
        else return false;
    }
    bool reverse = keyword >= 0 && kw(w[keyword], wn[keyword], "reverse");
    r->auto_rotate = keyword >= 0;
    r->angle = deg + (reverse ? 180 : 0);
    if (keyword >= 0) spec_word(spec, w[keyword], wn[keyword]);
    if (angle >= 0) { if (keyword >= 0) spec_puts(spec, " "); spec_len(spec, w[angle], wn[angle], cx, true); }
    return true;
}
/* offset-position: normal | auto | <position>; offset-anchor: auto | <position> */
static bool point_value(enum css_offset_property which, const char **w, const size_t *wn, int k, struct cx *cx, len_t at[2], sbuf *spec) {
    if (k == 1 && kw(w[0], wn[0], "auto")) { at[0] = at[1] = (len_t){.kind = LK_AUTO}; spec_puts(spec, "auto"); return true; }
    if (k == 1 && which == CSS_OFFSET_POSITION && kw(w[0], wn[0], "normal")) {
        at[0] = at[1] = (len_t){.kind = LK_NORMAL};
        spec_puts(spec, "normal");
        return true;
    }
    return position(w, wn, k, cx, at, spec);
}
bool css_offset_parse(enum css_offset_property which, const char *v, size_t n, struct cx *cx, void *out, sbuf *spec) {
    const char *w[MAXWORDS];
    size_t wn[MAXWORDS];
    int k = words(v, n, w, wn, MAXWORDS);
    if (k < 1) return false;
    for (int i = 0; i < k; i++) if (slash(w[i], wn[i])) return false;
    if (which == CSS_OFFSET_ROTATE) return rotate_value(w, wn, k, cx, out, spec);
    return point_value(which, w, wn, k, cx, out, spec);
}
bool css_offset_serialize(enum css_offset_property which, const void *value, sbuf *out, web_doc *d) {
    size_t begin = out->n;
    bool ok = true;
    if (which == CSS_OFFSET_ROTATE) put_rotate(out, value);
    else {
        const len_t *at = value;
        if (at[0].kind == LK_AUTO || at[0].kind == LK_NORMAL) sb_puts(out, at[0].kind == LK_AUTO ? "auto" : "normal");
        else ok = put_pos(out, at, d);
    }
    if (!ok) out->n = begin;
    sb_cstr(out);
    return ok;
}
bool css_offset_combine(enum css_offset_property which, const void *a, const void *b, double t, bool add, sbuf *out, web_doc *d) {
    size_t begin = out->n;
    if (which == CSS_OFFSET_ROTATE) {
        const struct css_offset_rotate *x = a, *y = b;
        if (x->auto_rotate != y->auto_rotate) return false;
        struct css_offset_rotate r = {(float)(add ? x->angle + y->angle : x->angle * (1 - t) + y->angle * t), x->auto_rotate};
        put_rotate(out, &r);
        sb_cstr(out);
        return true;
    }
    const len_t *x = a, *y = b;
    if (x[0].kind == LK_AUTO || x[0].kind == LK_NORMAL || y[0].kind == LK_AUTO || y[0].kind == LK_NORMAL) return false;
    arena_t arena = {.chunk_size = 1024};
    jmp_buf trap;
    arena.trap = &trap;
    volatile bool ok = false;
    if (!setjmp(trap)) {
        struct mix m = {&arena, add ? 1 : 1 - t, add ? 1 : t, t, add};
        len_t r[2];
        mix_pos(&m, x, y, r);
        ok = put_pos(out, r, d);
    }
    ar_free(&arena);
    if (!ok) out->n = begin;
    sb_cstr(out);
    return ok;
}
/* the offset shorthand's value from its longhands' computed values (CSSOM §6.7.2: the shortest
   form that sets them; motion-1 §2.6's grammar needs a path before a distance or a rotation) */
bool css_offset_shorthand_serialize(const struct css_offset *o, sbuf *out, web_doc *d) {
    size_t begin = out->n;
    bool position = o->position[0].kind != LK_NORMAL;
    bool distance = !len_zero(&o->distance);
    bool rotate = !(o->rotate.auto_rotate && o->rotate.angle == 0);
    bool ok = !position || css_offset_serialize(CSS_OFFSET_POSITION, o->position, out, d);
    if (ok && (o->path || distance || rotate || !position)) {
        if (out->n > begin) sb_putc(out, ' ');
        ok = css_shape_serialize(CSS_SHAPE_OFFSET_PATH, o->path, out, d);
    }
    if (ok && distance) { sb_putc(out, ' '); ok = put_len(out, &o->distance, d); }
    if (ok && rotate) { sb_putc(out, ' '); put_rotate(out, &o->rotate); }
    if (ok && o->anchor[0].kind != LK_AUTO) { sb_puts(out, " / "); ok = put_pos(out, o->anchor, d); }
    if (!ok) out->n = begin;
    sb_cstr(out);
    return ok;
}
static bool path_word(const char *t, size_t n) {
    static const char *const names[] = {"inset(", "rect(", "xywh(", "circle(", "ellipse(", "polygon(", "path(", "shape(", "ray(", "url("};
    if (kw(t, n, "none") || box_word(CSS_SHAPE_OFFSET_PATH, t, n) >= 0) return true;
    for (size_t i = 0; i < sizeof names / sizeof *names; i++) {
        size_t k = strlen(names[i]);
        if (n > k && strn_ieq(t, names[i], k)) return true;
    }
    return false;
}
/* offset: [<'offset-position'>? [<'offset-path'> [<'offset-distance'> || <'offset-rotate'>]?]?]! [/ <'offset-anchor'>]?
   (motion-1 §2.6); the specified value leaves out what is initial */
bool css_offset_shorthand(const char *v, size_t n, struct cx *cx, struct css_offset *o, sbuf *spec) {
    const char *w[MAXWORDS];
    size_t wn[MAXWORDS];
    int k = words(v, n, w, wn, MAXWORDS), end = k;
    if (k < 1) return false;
    for (int i = 0; i < k; i++) if (slash(w[i], wn[i])) { if (end < k) return false; end = i; }
    if (end == 0 || end == k - 1) return false;
    o->path = NULL;
    o->distance = pixels(0);
    o->position[0] = o->position[1] = (len_t){.kind = LK_NORMAL};
    o->anchor[0] = o->anchor[1] = (len_t){.kind = LK_AUTO};
    o->rotate = (struct css_offset_rotate){0, true};
    sbuf part[5] = {{0}}; /* position, path, distance, rotate, anchor */
    bool ok = false, path = false, distance = false, rotate = false;
    int i = 0, j;
    if (kw(w[0], wn[0], "normal") || kw(w[0], wn[0], "auto")) {
        if (!point_value(CSS_OFFSET_POSITION, w, wn, 1, cx, o->position, &part[0])) goto done;
        i = 1;
    } else {
        for (j = 0; j < end && j < 4 && !path_word(w[j], wn[j]) && pos_word(w[j], wn[j], cx, NULL) != W_OTHER; j++) {}
        if (j && !position(w, wn, j, cx, o->position, &part[0])) goto done;
        i = j;
    }
    for (j = i; j < end && j - i < 2 && path_word(w[j], wn[j]) && !(j > i && kw(w[j], wn[j], "none")); j++) {}
    if (j > i) {
        if (!css_shape_parse(CSS_SHAPE_OFFSET_PATH, w[i], (size_t)(w[j - 1] + wn[j - 1] - w[i]), cx, &o->path, &part[1])) goto done;
        path = true;
        i = j;
    }
    while (i < end) {
        len_t l;
        float deg;
        if (!path) goto done;
        if (!distance && get_len(w[i], wn[i], cx, &l, true)) {
            distance = true;
            o->distance = l;
            spec_len(&part[2], w[i], wn[i], cx, false);
            i++;
            continue;
        }
        bool keyword = kw(w[i], wn[i], "auto") || kw(w[i], wn[i], "reverse");
        if (rotate || (!keyword && !get_angle(w[i], wn[i], cx, &deg))) goto done;
        int count = 1;
        if (i + 1 < end && (keyword ? get_angle(w[i + 1], wn[i + 1], cx, &deg) : kw(w[i + 1], wn[i + 1], "auto") || kw(w[i + 1], wn[i + 1], "reverse")))
            count = 2;
        if (!rotate_value(w + i, wn + i, count, cx, &o->rotate, &part[3])) goto done;
        rotate = true;
        i += count;
    }
    if (end < k && !point_value(CSS_OFFSET_ANCHOR, w + end + 1, wn + end + 1, k - end - 1, cx, o->anchor, &part[4])) goto done;
    ok = true;
    if (spec) {
        bool show_position = o->position[0].kind != LK_NORMAL;
        bool show_distance = distance && !len_zero(&o->distance);
        bool show_rotate = rotate && !(o->rotate.auto_rotate && o->rotate.angle == 0);
        bool show_path = path && (o->path || show_distance || show_rotate || !show_position);
        size_t begin = spec->n;
        if (show_position) sb_put(spec, part[0].p, part[0].n);
        if (show_path) { if (spec->n > begin) sb_putc(spec, ' '); sb_put(spec, part[1].p, part[1].n); }
        if (show_distance) { sb_putc(spec, ' '); sb_put(spec, part[2].p, part[2].n); }
        if (show_rotate) { sb_putc(spec, ' '); sb_put(spec, part[3].p, part[3].n); }
        if (spec->n == begin) sb_puts(spec, part[0].n ? "normal" : "none");
        if (end < k && o->anchor[0].kind != LK_AUTO) { sb_puts(spec, " / "); sb_put(spec, part[4].p, part[4].n); }
    }
done:
    for (int p = 0; p < 5; p++) sb_free(&part[p]);
    return ok;
}

/* ---------------------------------------------------------------- CSSOM specified values */
bool css_shape_specified(const char *name, const char *v, size_t n, sbuf *out) {
    trim(&v, &n);
    if (!n || memchr(v, '\\', n)) return false;
    for (size_t i = 0; i + 4 <= n; i++) if (strn_ieq(v + i, "var(", 4)) return false;
    static const char *const wide[] = {"inherit", "initial", "unset", "revert", "revert-layer"};
    for (size_t i = 0; i < sizeof wide / sizeof *wide; i++) if (kw(v, n, wide[i])) return false;
    arena_t arena = {.chunk_size = 4096};
    jmp_buf trap;
    arena.trap = &trap;
    size_t begin = out->n;
    volatile bool ok = false;
    if (!setjmp(trap)) {
        struct cx cx = {.em = 16, .rem = 16, .vw = 800, .vh = 600, .a = &arena};
        const struct css_shape *shape;
        struct css_offset offset;
        len_t l;
        if (!strcmp(name, "clip-path")) ok = css_shape_parse(CSS_SHAPE_CLIP_PATH, v, n, &cx, &shape, out);
        else if (!strcmp(name, "shape-outside")) ok = css_shape_parse(CSS_SHAPE_OUTSIDE, v, n, &cx, &shape, out);
        else if (!strcmp(name, "offset-path")) ok = css_shape_parse(CSS_SHAPE_OFFSET_PATH, v, n, &cx, &shape, out);
        else if (!strcmp(name, "offset-position")) ok = css_offset_parse(CSS_OFFSET_POSITION, v, n, &cx, offset.position, out);
        else if (!strcmp(name, "offset-anchor")) ok = css_offset_parse(CSS_OFFSET_ANCHOR, v, n, &cx, offset.anchor, out);
        else if (!strcmp(name, "offset-rotate")) ok = css_offset_parse(CSS_OFFSET_ROTATE, v, n, &cx, &offset.rotate, out);
        else if (!strcmp(name, "offset")) ok = css_offset_shorthand(v, n, &cx, &offset, out);
        else if (!strcmp(name, "offset-distance") || !strcmp(name, "shape-margin")) {
            bool neg = name[0] == 'o';
            if ((ok = get_len(v, n, &cx, &l, neg))) spec_len(out, v, n, &cx, false);
        } else if (!strcmp(name, "shape-image-threshold")) { /* <opacity-value>: a number, a percentage as one */
            const char *p = v;
            float x;
            if (css_number_token(&p, v + n, &x) && (p == v + n || (p + 1 == v + n && *p == '%'))) {
                put_num(out, p == v + n ? x : x / 100);
                ok = true;
            }
        }
    }
    ar_free(&arena);
    if (!ok) out->n = begin;
    sb_cstr(out);
    return ok;
}

/* ---------------------------------------------------------------- geometry */
static double resolve(const len_t *l, double base) { return len_resolve(l, (float)base); }
void css_shape_ellipse(const struct css_shape *s, const float r[4], double c[2], double radii[2]) {
    double x = r[0], y = r[1], w = r[2], h = r[3];
    c[0] = x + resolve(&s->at[0], w);
    c[1] = y + resolve(&s->at[1], h);
    double side[4] = {fabs(c[0] - x), fabs(x + w - c[0]), fabs(c[1] - y), fabs(y + h - c[1])};
    double near_x = fmin(side[0], side[1]), far_x = fmax(side[0], side[1]), near_y = fmin(side[2], side[3]), far_y = fmax(side[2], side[3]);
    for (int i = 0; i < 2; i++) {
        int e = s->extent[s->kind == CSS_SHAPE_CIRCLE ? 0 : i];
        double base = s->kind == CSS_SHAPE_CIRCLE ? sqrt(w * w + h * h) / sqrt(2) : i ? h : w;
        double nearest = s->kind == CSS_SHAPE_CIRCLE ? fmin(near_x, near_y) : i ? near_y : near_x;
        double farthest = s->kind == CSS_SHAPE_CIRCLE ? fmax(far_x, far_y) : i ? far_y : far_x;
        switch (e) {
        case CSS_EXTENT_LENGTH: radii[i] = fmax(0, resolve(&s->radius[s->kind == CSS_SHAPE_CIRCLE ? 0 : i], base)); break;
        case CSS_EXTENT_CLOSEST_SIDE: radii[i] = nearest; break;
        case CSS_EXTENT_FARTHEST_SIDE: radii[i] = farthest; break;
        default: { /* to a corner (css-images-3 §3.2.2's sizing of a radial gradient's ending shape) */
            double cx = e == CSS_EXTENT_CLOSEST_CORNER ? near_x : far_x, cy = e == CSS_EXTENT_CLOSEST_CORNER ? near_y : far_y;
            radii[i] = s->kind == CSS_SHAPE_CIRCLE ? hypot(cx, cy) : (i ? cy : cx) * sqrt(2);
        }
        }
    }
}
void css_shape_inset(const struct css_shape *s, const float r[4], double rect[4], float rx[4], float ry[4]) {
    double w = r[2], h = r[3];
    double top = resolve(&s->inset[0], h), right = resolve(&s->inset[1], w), bottom = resolve(&s->inset[2], h), left = resolve(&s->inset[3], w);
    /* opposing insets that pass each other shrink in proportion (css-shapes-1 §3.1.1) */
    if (left + right > w && left + right > 0) { double f = w / (left + right); left *= f; right *= f; }
    if (top + bottom > h && top + bottom > 0) { double f = h / (top + bottom); top *= f; bottom *= f; }
    rect[0] = r[0] + left; rect[1] = r[1] + top; rect[2] = fmax(0, w - left - right); rect[3] = fmax(0, h - top - bottom);
    double f = 1;
    for (int i = 0; i < 4; i++) {
        rx[i] = (float)fmax(0, resolve(&s->corner[i].axis[0], rect[2]));
        ry[i] = (float)fmax(0, resolve(&s->corner[i].axis[1], rect[3]));
    }
    /* css-backgrounds-3 §5.5: corners that overlap scale down together */
    double sums[4] = {rx[0] + rx[1], ry[1] + ry[2], rx[2] + rx[3], ry[3] + ry[0]}, sides[4] = {rect[2], rect[3], rect[2], rect[3]};
    for (int i = 0; i < 4; i++) if (sums[i] > sides[i] && sums[i] > 0) f = fmin(f, sides[i] / sums[i]);
    for (int i = 0; i < 4; i++) { rx[i] = (float)(rx[i] * f); ry[i] = (float)(ry[i] * f); }
}
/* A pen the outline is drawn with, in absolute coordinates. */
struct pen {
    void (*move)(struct pen *, double x, double y);
    void (*line)(struct pen *, double x, double y);
    void (*cubic)(struct pen *, double x1, double y1, double x2, double y2, double x, double y);
    void (*quad)(struct pen *, double x1, double y1, double x, double y);
    void (*arc)(struct pen *, double rx, double ry, double rotate, bool large, bool sweep, double x, double y);
    void (*close)(struct pen *);
    double x, y; /* the current point, kept by the drawing functions */
};
static void draw_rect(struct pen *p, const double q[4], const float rx[4], const float ry[4]) {
    double x = q[0], y = q[1], w = q[2], h = q[3];
    /* css-shapes-1 §3.1.1: from the left end of the top edge, clockwise */
    p->move(p, x + rx[0], y);
    p->line(p, x + w - rx[1], y);
    p->arc(p, rx[1], ry[1], 0, false, true, x + w, y + ry[1]);
    p->line(p, x + w, y + h - ry[2]);
    p->arc(p, rx[2], ry[2], 0, false, true, x + w - rx[2], y + h);
    p->line(p, x + rx[3], y + h);
    p->arc(p, rx[3], ry[3], 0, false, true, x, y + h - ry[3]);
    p->line(p, x, y + ry[0]);
    p->arc(p, rx[0], ry[0], 0, false, true, x + rx[0], y);
    p->close(p);
}
static void polygon_point(const struct css_shape *s, size_t i, const float r[4], double *x, double *y) {
    *x = r[0] + resolve(&s->point[i][0], r[2]);
    *y = r[1] + resolve(&s->point[i][1], r[3]);
}
/* A polygon, its corners rounded by round (css-shapes-1 §3.1.4): each corner an arc tangent to both
   edges, its tangent points no farther from the corner than half of either edge. */
static void draw_polygon(struct pen *p, const struct css_shape *s, const float r[4]) {
    for (size_t i = 0; i < s->count; i++) {
        double px, py, ax, ay, bx, by;
        polygon_point(s, i, r, &px, &py);
        if (!(s->round > 0)) { if (i) p->line(p, px, py); else p->move(p, px, py); continue; }
        polygon_point(s, (i + s->count - 1) % s->count, r, &ax, &ay);
        polygon_point(s, (i + 1) % s->count, r, &bx, &by);
        double ux = ax - px, uy = ay - py, vx = bx - px, vy = by - py, lu = hypot(ux, uy), lv = hypot(vx, vy);
        double angle = lu > 0 && lv > 0 ? acos(fmax(-1, fmin(1, (ux * vx + uy * vy) / (lu * lv)))) : 0, t = 0, radius = 0;
        if (angle > 1e-6 && angle < M_PI - 1e-6) { t = fmin(s->round / tan(angle / 2), fmin(lu, lv) / 2); radius = t * tan(angle / 2); }
        double x1 = px + (lu > 0 ? ux / lu * t : 0), y1 = py + (lu > 0 ? uy / lu * t : 0);
        double x2 = px + (lv > 0 ? vx / lv * t : 0), y2 = py + (lv > 0 ? vy / lv * t : 0);
        if (i) p->line(p, x1, y1); else p->move(p, x1, y1);
        if (radius > 0) p->arc(p, radius, radius, 0, false, (-ux) * vy - (-uy) * vx > 0, x2, y2);
    }
    if (s->count) p->close(p);
}
/* path data, its coordinates from the reference box's top left (SVG 2 §9.3 with smooth curves' reflection) */
static void draw_path(struct pen *p, const struct svg_path_seg *seg, size_t count, const float r[4]) {
    double sx = r[0], sy = r[1], cx2 = 0, cy2 = 0;
    char last = 0;
    p->x = r[0]; p->y = r[1];
    for (size_t i = 0; i < count; i++) {
        const struct svg_path_seg *s = &seg[i];
        double ox = s->cmd >= 'a' ? p->x : r[0], oy = s->cmd >= 'a' ? p->y : r[1];
        const float *v = s->v;
        char up = (char)toupper((unsigned char)s->cmd);
        double x0 = p->x, y0 = p->y;
        switch (up) {
        case 'M': p->move(p, ox + v[0], oy + v[1]); sx = p->x; sy = p->y; break;
        case 'L': p->line(p, ox + v[0], oy + v[1]); break;
        case 'H': p->line(p, (s->cmd == 'h' ? p->x : r[0]) + v[0], p->y); break;
        case 'V': p->line(p, p->x, (s->cmd == 'v' ? p->y : r[1]) + v[0]); break;
        case 'C': p->cubic(p, ox + v[0], oy + v[1], ox + v[2], oy + v[3], ox + v[4], oy + v[5]); cx2 = ox + v[2]; cy2 = oy + v[3]; break;
        case 'S': {
            double x1 = last == 'C' || last == 'S' ? 2 * x0 - cx2 : x0, y1 = last == 'C' || last == 'S' ? 2 * y0 - cy2 : y0;
            p->cubic(p, x1, y1, ox + v[0], oy + v[1], ox + v[2], oy + v[3]); cx2 = ox + v[0]; cy2 = oy + v[1];
            break;
        }
        case 'Q': p->quad(p, ox + v[0], oy + v[1], ox + v[2], oy + v[3]); cx2 = ox + v[0]; cy2 = oy + v[1]; break;
        case 'T': {
            double x1 = last == 'Q' || last == 'T' ? 2 * x0 - cx2 : x0, y1 = last == 'Q' || last == 'T' ? 2 * y0 - cy2 : y0;
            p->quad(p, x1, y1, ox + v[0], oy + v[1]); cx2 = x1; cy2 = y1;
            break;
        }
        case 'A': p->arc(p, v[0], v[1], v[2], v[3] != 0, v[4] != 0, ox + v[5], oy + v[6]); break;
        case 'Z': p->close(p); p->x = sx; p->y = sy; break;
        }
        last = up;
    }
}
/* shape() commands (css-shapes-1 §3.1.5) */
static void draw_commands(struct pen *p, const struct css_shape *s, const float r[4]) {
    double w = r[2], h = r[3], sx, sy, cx2 = 0, cy2 = 0;
    int last = -1, last_controls = 0;
    p->move(p, sx = r[0] + resolve(&s->at[0], w), sy = r[1] + resolve(&s->at[1], h));
    for (size_t i = 0; i < s->count; i++) {
        const struct css_shape_command *c = &s->command[i];
        double x0 = p->x, y0 = p->y, x, y;
        if (c->op == CSS_CMD_CLOSE) { p->close(p); p->x = sx; p->y = sy; last = c->op; continue; }
        if (c->op == CSS_CMD_HLINE) { x = (c->by ? x0 : r[0]) + resolve(&c->end[0], w); y = y0; }
        else if (c->op == CSS_CMD_VLINE) { x = x0; y = (c->by ? y0 : r[1]) + resolve(&c->end[1], h); }
        else { x = (c->by ? x0 : r[0]) + resolve(&c->end[0], w); y = (c->by ? y0 : r[1]) + resolve(&c->end[1], h); }
        double k[2][2];
        for (int j = 0; j < c->controls; j++) {
            double bx = c->anchor[j] == CSS_ANCHOR_START ? x0 : c->anchor[j] == CSS_ANCHOR_END ? x : r[0];
            double by = c->anchor[j] == CSS_ANCHOR_START ? y0 : c->anchor[j] == CSS_ANCHOR_END ? y : r[1];
            k[j][0] = bx + resolve(&c->control[j][0], w);
            k[j][1] = by + resolve(&c->control[j][1], h);
        }
        switch (c->op) {
        case CSS_CMD_MOVE: p->move(p, x, y); sx = x; sy = y; break;
        case CSS_CMD_LINE: case CSS_CMD_HLINE: case CSS_CMD_VLINE: p->line(p, x, y); break;
        case CSS_CMD_CURVE:
            if (c->controls == 2) { p->cubic(p, k[0][0], k[0][1], k[1][0], k[1][1], x, y); cx2 = k[1][0]; cy2 = k[1][1]; }
            else { p->quad(p, k[0][0], k[0][1], x, y); cx2 = k[0][0]; cy2 = k[0][1]; }
            break;
        case CSS_CMD_SMOOTH: {
            /* the first control point reflects the previous curve's last one through the start */
            bool curve = last == CSS_CMD_CURVE || last == CSS_CMD_SMOOTH;
            bool cubic = c->controls == 1;
            bool same = curve && (cubic ? last_controls == 2 || (last == CSS_CMD_SMOOTH && last_controls == 1) :
                                          last_controls == 1 || (last == CSS_CMD_SMOOTH && last_controls == 0));
            double x1 = same ? 2 * x0 - cx2 : x0, y1 = same ? 2 * y0 - cy2 : y0;
            if (cubic) { p->cubic(p, x1, y1, k[0][0], k[0][1], x, y); cx2 = k[0][0]; cy2 = k[0][1]; }
            else { p->quad(p, x1, y1, x, y); cx2 = x1; cy2 = y1; }
            break;
        }
        case CSS_CMD_ARC: {
            /* one radius resolves against the box's normalized diagonal, two against its width and height */
            double rx = resolve(&c->radius[0], c->radii == 1 ? sqrt(w * w + h * h) / sqrt(2) : w);
            double ry = c->radii == 1 ? rx : resolve(&c->radius[1], h);
            p->arc(p, fabs(rx), fabs(ry), c->rotate, c->large, c->cw, x, y);
            break;
        }
        }
        last = c->op;
        last_controls = c->controls;
    }
}
static bool draw_shape(struct pen *p, const struct css_shape *s, const float r[4]) {
    switch (s->kind) {
    case CSS_SHAPE_BOX: {
        double q[4] = {r[0], r[1], r[2], r[3]};
        const float zero[4] = {0};
        draw_rect(p, q, zero, zero);
        return true;
    }
    case CSS_SHAPE_INSET: {
        double q[4];
        float rx[4], ry[4];
        css_shape_inset(s, r, q, rx, ry);
        draw_rect(p, q, rx, ry);
        return true;
    }
    case CSS_SHAPE_CIRCLE: case CSS_SHAPE_ELLIPSE: {
        double c[2], radii[2];
        css_shape_ellipse(s, r, c, radii);
        /* from the rightmost point, clockwise (motion-1 §3) */
        p->move(p, c[0] + radii[0], c[1]);
        p->arc(p, radii[0], radii[1], 0, false, true, c[0] - radii[0], c[1]);
        p->arc(p, radii[0], radii[1], 0, false, true, c[0] + radii[0], c[1]);
        p->close(p);
        return true;
    }
    case CSS_SHAPE_POLYGON: if (!s->count) return false; draw_polygon(p, s, r); return true;
    case CSS_SHAPE_PATH: draw_path(p, s->seg, s->count, r); return true;
    case CSS_SHAPE_SHAPE: draw_commands(p, s, r); return true;
    }
    return false;
}
/* the text pen: SVG path data */
struct text_pen { struct pen pen; sbuf *out; double ox, oy; };
static void text_point(struct text_pen *t, double x, double y) {
    char b[64];
    snprintf(b, sizeof b, " %.9g %.9g", x - t->ox, y - t->oy);
    sb_puts(t->out, b);
}
static void text_move(struct pen *p, double x, double y) { struct text_pen *t = (void *)p; sb_puts(t->out, " M"); text_point(t, x, y); p->x = x; p->y = y; }
static void text_line(struct pen *p, double x, double y) { struct text_pen *t = (void *)p; sb_puts(t->out, " L"); text_point(t, x, y); p->x = x; p->y = y; }
static void text_cubic(struct pen *p, double x1, double y1, double x2, double y2, double x, double y) {
    struct text_pen *t = (void *)p;
    sb_puts(t->out, " C"); text_point(t, x1, y1); text_point(t, x2, y2); text_point(t, x, y); p->x = x; p->y = y;
}
static void text_quad(struct pen *p, double x1, double y1, double x, double y) {
    struct text_pen *t = (void *)p;
    sb_puts(t->out, " Q"); text_point(t, x1, y1); text_point(t, x, y); p->x = x; p->y = y;
}
static void text_arc(struct pen *p, double rx, double ry, double rotate, bool large, bool sweep, double x, double y) {
    struct text_pen *t = (void *)p;
    if (!(rx > 0 && ry > 0)) { text_line(p, x, y); return; }
    char b[96];
    snprintf(b, sizeof b, " A %.9g %.9g %.9g %d %d", rx, ry, rotate, large, sweep);
    sb_puts(t->out, b); text_point(t, x, y); p->x = x; p->y = y;
}
static void text_close(struct pen *p) { struct text_pen *t = (void *)p; sb_puts(t->out, " Z"); }
bool css_shape_path_data(const struct css_shape *s, const float r[4], double ox, double oy, sbuf *out) {
    struct text_pen t = {{text_move, text_line, text_cubic, text_quad, text_arc, text_close, 0, 0}, out, ox, oy};
    return s && draw_shape(&t.pen, s, r);
}
/* the flattening pen: curves as lines, each line given to seg() */
struct flat_pen {
    struct pen pen;
    void (*seg)(struct flat_pen *, double x0, double y0, double x1, double y1);
    double sx, sy;
    int subpaths;
    bool closed; /* the last subpath closed */
};
static void flat_move(struct pen *p, double x, double y) {
    struct flat_pen *f = (void *)p;
    p->x = f->sx = x; p->y = f->sy = y; f->subpaths++; f->closed = false;
}
static void flat_line(struct pen *p, double x, double y) {
    struct flat_pen *f = (void *)p;
    f->seg(f, p->x, p->y, x, y);
    p->x = x; p->y = y; f->closed = false;
}
static void flat_cubic(struct pen *p, double x1, double y1, double x2, double y2, double x, double y) {
    double x0 = p->x, y0 = p->y;
    for (int i = 1; i <= 16; i++) {
        double t = i / 16.0, u = 1 - t;
        flat_line(p, u * u * u * x0 + 3 * u * u * t * x1 + 3 * u * t * t * x2 + t * t * t * x,
                  u * u * u * y0 + 3 * u * u * t * y1 + 3 * u * t * t * y2 + t * t * t * y);
    }
}
static void flat_quad(struct pen *p, double x1, double y1, double x, double y) {
    double x0 = p->x, y0 = p->y;
    for (int i = 1; i <= 12; i++) {
        double t = i / 12.0, u = 1 - t;
        flat_line(p, u * u * x0 + 2 * u * t * x1 + t * t * x, u * u * y0 + 2 * u * t * y1 + t * t * y);
    }
}
static double vector_angle(double ux, double uy, double vx, double vy) {
    return atan2(ux * vy - uy * vx, ux * vx + uy * vy);
}
/* SVG 2 §9.5.1's endpoint arc as its centre form (SVG 1.1 implementation notes F.6.5), then lines */
static void flat_arc(struct pen *p, double rx, double ry, double rotate, bool large, bool sweep, double x, double y) {
    double x1 = p->x, y1 = p->y;
    rx = fabs(rx); ry = fabs(ry);
    if (!(rx > 0 && ry > 0) || (x1 == x && y1 == y)) { flat_line(p, x, y); return; }
    double phi = rotate * M_PI / 180, cs = cos(phi), sn = sin(phi);
    double dx = (x1 - x) / 2, dy = (y1 - y) / 2, xp = cs * dx + sn * dy, yp = -sn * dx + cs * dy;
    double lambda = xp * xp / (rx * rx) + yp * yp / (ry * ry);
    if (lambda > 1) { rx *= sqrt(lambda); ry *= sqrt(lambda); }
    double num = rx * rx * ry * ry - rx * rx * yp * yp - ry * ry * xp * xp, den = rx * rx * yp * yp + ry * ry * xp * xp;
    double coef = den > 0 ? sqrt(fmax(0, num / den)) : 0;
    if (large == sweep) coef = -coef;
    double cxp = coef * rx * yp / ry, cyp = -coef * ry * xp / rx;
    double cx = cs * cxp - sn * cyp + (x1 + x) / 2, cy = sn * cxp + cs * cyp + (y1 + y) / 2;
    double theta = vector_angle(1, 0, (xp - cxp) / rx, (yp - cyp) / ry);
    double delta = vector_angle((xp - cxp) / rx, (yp - cyp) / ry, (-xp - cxp) / rx, (-yp - cyp) / ry);
    if (!sweep && delta > 0) delta -= 2 * M_PI;
    else if (sweep && delta < 0) delta += 2 * M_PI;
    int steps = (int)ceil(fabs(delta) / (M_PI / 16));
    if (steps < 1) steps = 1;
    for (int i = 1; i < steps; i++) {
        double a = theta + delta * i / steps;
        flat_line(p, cx + rx * cs * cos(a) - ry * sn * sin(a), cy + rx * sn * cos(a) + ry * cs * sin(a));
    }
    flat_line(p, x, y);
}
static void flat_close(struct pen *p) {
    struct flat_pen *f = (void *)p;
    flat_line(p, f->sx, f->sy);
    f->closed = true;
}
static void flat_init(struct flat_pen *f, void (*seg)(struct flat_pen *, double, double, double, double)) {
    *f = (struct flat_pen){{flat_move, flat_line, flat_cubic, flat_quad, flat_arc, flat_close, 0, 0}, seg, 0, 0, 0, false};
}
struct bounds_pen { struct flat_pen f; double b[4]; bool any; };
static void bounds_seg(struct flat_pen *f, double x0, double y0, double x1, double y1) {
    struct bounds_pen *b = (void *)f;
    if (!b->any) { b->b[0] = b->b[2] = x0; b->b[1] = b->b[3] = y0; b->any = true; }
    b->b[0] = fmin(b->b[0], fmin(x0, x1)); b->b[1] = fmin(b->b[1], fmin(y0, y1));
    b->b[2] = fmax(b->b[2], fmax(x0, x1)); b->b[3] = fmax(b->b[3], fmax(y0, y1));
}
bool css_shape_bounds(const struct css_shape *s, const float r[4], double out[4]) {
    struct bounds_pen b = {0};
    flat_init(&b.f, bounds_seg);
    if (!s || !draw_shape(&b.f.pen, s, r) || !b.any) return false;
    memcpy(out, b.b, sizeof b.b);
    return true;
}
struct band_pen { struct flat_pen f; double y0, y1, x0, x1; bool any; };
static void band_seg(struct flat_pen *f, double x0, double y0, double x1, double y1) {
    struct band_pen *b = (void *)f;
    double lo = fmin(y0, y1), hi = fmax(y0, y1);
    if (hi < b->y0 || lo > b->y1) return;
    /* the part of the segment within the band */
    double ta = 0, tb = 1;
    if (y1 != y0) {
        double t0 = (b->y0 - y0) / (y1 - y0), t1 = (b->y1 - y0) / (y1 - y0);
        ta = fmax(0, fmin(t0, t1)); tb = fmin(1, fmax(t0, t1));
    }
    double xa = x0 + (x1 - x0) * ta, xb = x0 + (x1 - x0) * tb;
    if (!b->any) { b->x0 = b->x1 = xa; b->any = true; }
    b->x0 = fmin(b->x0, fmin(xa, xb));
    b->x1 = fmax(b->x1, fmax(xa, xb));
}
bool css_shape_band(const struct css_shape *s, const float r[4], float margin, float y0, float y1, float *x0, float *x1) {
    if (margin < 0) margin = 0;
    struct band_pen b = {.y0 = y0 - margin, .y1 = y1 + margin};
    flat_init(&b.f, band_seg);
    if (!s || !draw_shape(&b.f.pen, s, r) || !b.any) return false;
    *x0 = (float)(b.x0 - margin);
    *x1 = (float)(b.x1 + margin);
    return true;
}
/* the length of the outline, and the point and direction a distance along it */
struct walk_pen { struct flat_pen f; double length, target, x, y, direction; bool found; };
static void walk_seg(struct flat_pen *f, double x0, double y0, double x1, double y1) {
    struct walk_pen *w = (void *)f;
    double l = hypot(x1 - x0, y1 - y0);
    if (!w->found && l > 0 && w->length + l >= w->target) {
        double t = (w->target - w->length) / l;
        w->x = x0 + (x1 - x0) * t; w->y = y0 + (y1 - y0) * t;
        w->direction = atan2(y1 - y0, x1 - x0) * 180 / M_PI;
        w->found = true;
    }
    if (l > 0 && !w->found) { w->x = x1; w->y = y1; w->direction = atan2(y1 - y0, x1 - x0) * 180 / M_PI; }
    w->length += l;
}
bool css_offset_place(const struct css_offset *o, const float cb[4], const float element[4], double *x, double *y, double *direction) {
    const struct css_shape *s = o->path;
    if (!s || s->kind == CSS_SHAPE_URL) return false;
    /* the offset starting position (motion-1 §2.3) */
    bool start = o->position[0].kind != LK_NORMAL;
    double sx = cb[0] + cb[2] / 2.0, sy = cb[1] + cb[3] / 2.0;
    if (o->position[0].kind == LK_AUTO) { sx = element[0]; sy = element[1]; }
    else if (start) { sx = cb[0] + resolve(&o->position[0], cb[2]); sy = cb[1] + resolve(&o->position[1], cb[3]); }
    if (s->kind == CSS_SHAPE_RAY) {
        if (s->at_given) { sx = cb[0] + resolve(&s->at[0], cb[2]); sy = cb[1] + resolve(&s->at[1], cb[3]); }
        double a = s->angle * M_PI / 180, ux = sin(a), uy = -cos(a), length = 0;
        double side[4] = {fabs(sx - cb[0]), fabs(cb[0] + cb[2] - sx), fabs(sy - cb[1]), fabs(cb[1] + cb[3] - sy)};
        switch (s->extent[0]) {
        case CSS_EXTENT_CLOSEST_SIDE: length = fmin(fmin(side[0], side[1]), fmin(side[2], side[3])); break;
        case CSS_EXTENT_FARTHEST_SIDE: length = fmax(fmax(side[0], side[1]), fmax(side[2], side[3])); break;
        case CSS_EXTENT_CLOSEST_CORNER: length = hypot(fmin(side[0], side[1]), fmin(side[2], side[3])); break;
        case CSS_EXTENT_FARTHEST_CORNER: length = hypot(fmax(side[0], side[1]), fmax(side[2], side[3])); break;
        case CSS_EXTENT_SIDES: { /* to where the ray leaves the containing block; 0 from outside it */
            if (sx < cb[0] || sx > cb[0] + cb[2] || sy < cb[1] || sy > cb[1] + cb[3]) break;
            double tx = ux > 0 ? (cb[0] + cb[2] - sx) / ux : ux < 0 ? (cb[0] - sx) / ux : INFINITY;
            double ty = uy > 0 ? (cb[1] + cb[3] - sy) / uy : uy < 0 ? (cb[1] - sy) / uy : INFINITY;
            length = fmin(tx, ty);
            break;
        }
        }
        /* contain (motion-1 §2.1.1): the length shrinks by half the box's larger side */
        if (s->contain) length = fmax(0, length - fmax(element[2], element[3]) / 2);
        double d = resolve(&o->distance, length);
        if (s->contain) d = fmax(0, fmin(length, d));
        *x = sx + ux * d; *y = sy + uy * d;
        *direction = s->angle - 90;
        return isfinite(*x) && isfinite(*y);
    }
    struct css_shape at = *s;
    /* an omitted centre is the offset starting position (motion-1 §2.1) */
    if ((s->kind == CSS_SHAPE_CIRCLE || s->kind == CSS_SHAPE_ELLIPSE) && !s->at_given && start) {
        at.at[0] = pixels(sx - cb[0]); at.at[1] = pixels(sy - cb[1]);
    }
    /* path()'s coordinates start at the element's own top left, as in every engine and WPT
       (css/motion/offset-path-string-*, offset-distance-*), though motion-1 §2.1 sizes a
       <basic-shape> into the containing block's <coord-box> */
    const float *ref = s->kind == CSS_SHAPE_PATH ? element : cb;
    struct walk_pen w = {.target = -1};
    flat_init(&w.f, walk_seg);
    if (!draw_shape(&w.f.pen, &at, ref)) return false;
    double length = w.length, d = resolve(&o->distance, length);
    /* motion-1 §2.2: the basic shapes are closed loops, which wrap; path data is one when its last
       command closes it, and otherwise the distance clamps */
    bool closed = s->kind != CSS_SHAPE_PATH && s->kind != CSS_SHAPE_SHAPE ? true : w.f.closed;
    if (!(length > 0)) d = 0;
    else if (closed) { d = fmod(d, length); if (d < 0) d += length; }
    else d = fmax(0, fmin(length, d));
    struct walk_pen v = {.target = d};
    flat_init(&v.f, walk_seg);
    draw_shape(&v.f.pen, &at, ref);
    if (!v.found) { /* a zero-length path stays at its start */
        if (!(v.length > 0)) { v.x = v.f.pen.x; v.y = v.f.pen.y; v.direction = 0; }
    }
    *x = v.x; *y = v.y; *direction = v.direction;
    return isfinite(*x) && isfinite(*y);
}
