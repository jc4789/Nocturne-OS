/* CSS properties: the table of supported properties, value parsing (lengths, calc(), colours,
   keywords), shorthand expansion, and the user agent stylesheet. */
#include <stdio.h>
#include <math.h>
#include <stddef.h>
#include <ctype.h>
#include "webi.h"

/* ---------------------------------------------------------------- small parsing helpers */
static void trim(const char **s, size_t *n) {
    while (*n && is_space((unsigned char)**s)) (*s)++, (*n)--;
    while (*n && is_space((unsigned char)(*s)[*n - 1])) (*n)--;
}

static bool ident_is(const char *s, size_t n, const char *kw) { return strn_ieq(s, kw, n); }

/* split at top-level separators (whitespace when sep is ' '), keeping () and quotes together */
static int split(const char *s, size_t n, const char **tok, size_t *tl, int max, char sep) {
    int cnt = 0;
    size_t i = 0;
    while (i < n && cnt < max) {
        if (sep == ' ') {
            while (i < n && is_space((unsigned char)s[i])) i++;
            if (i >= n) break;
        }
        size_t st = i;
        int depth = 0;
        char q = 0;
        while (i < n) {
            char c = s[i];
            if (q) {
                if (c == '\\') i++;
                else if (c == q) q = 0;
            } else if (c == '"' || c == '\'') q = c;
            else if (c == '(') depth++;
            else if (c == ')') depth--;
            else if (depth <= 0 && (sep == ' ' ? is_space((unsigned char)c) : c == sep)) break;
            i++;
        }
        const char *ts = s + st;
        size_t tn = (i < n ? i : n) - st;
        trim(&ts, &tn);
        tok[cnt] = ts;
        tl[cnt++] = tn;
        if (i < n && sep != ' ') i++;
    }
    return cnt;
}

void css_unescape(sbuf *out, const char *s, size_t n) {
    for (size_t i = 0; i < n; i++) {
        if (s[i] != '\\') {
            sb_putc(out, s[i]);
            continue;
        }
        if (++i >= n) break;
        if (s[i] == '\n') continue;
        uint32_t cp = 0;
        int k = 0;
        while (k < 6 && i < n && isxdigit((unsigned char)s[i])) {
            int c = s[i] | 32;
            cp = cp * 16 + (uint32_t)(c <= '9' ? c - '0' : c - 'a' + 10);
            i++, k++;
        }
        if (k) {
            if (i < n && is_space((unsigned char)s[i])) i++;
            i--;
            if (cp == 0 || cp > 0x10FFFF) cp = 0xFFFD;
            /* private-use characters are icon-font glyphs we cannot show */
            if ((cp >= 0xE000 && cp <= 0xF8FF) || cp >= 0xF0000) continue;
            sb_utf8(out, cp);
        } else sb_putc(out, s[i]);
    }
}

static bool parse_number(const char **ps, const char *e, float *out) {
    const char *s = *ps;
    double sign = 1, v = 0;
    if (s < e && (*s == '+' || *s == '-')) sign = *s++ == '-' ? -1 : 1;
    bool digits = false;
    while (s < e && *s >= '0' && *s <= '9') v = v * 10 + (*s++ - '0'), digits = true;
    if (s < e && *s == '.' && s + 1 < e && s[1] >= '0' && s[1] <= '9') {
        s++;
        double f = 0.1;
        while (s < e && *s >= '0' && *s <= '9') v += (*s++ - '0') * f, f /= 10, digits = true;
    }
    if (!digits) return false;
    if (s + 1 < e && (*s | 32) == 'e' &&
        ((s[1] >= '0' && s[1] <= '9') || ((s[1] == '-' || s[1] == '+') && s + 2 < e && s[2] >= '0' && s[2] <= '9'))) {
        s++;
        int es = 1, ev = 0;
        if (*s == '-' || *s == '+') es = *s++ == '-' ? -1 : 1;
        while (s < e && *s >= '0' && *s <= '9') ev = ev * 10 + (*s++ - '0');
        v *= pow(10, es * ev);
    }
    *out = (float)(sign * v);
    *ps = s;
    return true;
}

/* ---------------------------------------------------------------- lengths and calc() */
struct cexpr {
    char op; /* 'v' value, + - * /, 'm' min, 'M' max */
    bool num;
    float px, pct;
    struct cexpr *a, *b;
};

enum { LF_AUTO = 1, LF_NONE = 2, LF_NEG = 4, LF_NORMAL = 8, LF_NOPCT = 16 };

/* a dimension: number + unit; false if the unit is unknown */
static bool unit_px(const char *u, size_t un, float v, const struct cx *cx, float *px, float *pct, bool *num) {
    *px = *pct = 0;
    *num = false;
    if (!un) {
        *px = v;
        *num = true;
        return true;
    }
    if (un == 1 && u[0] == '%') {
        *pct = v;
        return true;
    }
    char b[8];
    if (un >= sizeof b) return false;
    for (size_t i = 0; i < un; i++) b[i] = (char)lower((unsigned char)u[i]);
    b[un] = 0;
    float em = cx->em;
    static const struct {
        const char *u;
        int kind; /* 0 absolute factor, 1 em, 2 rem, 3 vw, 4 vh, 5 vmin, 6 vmax */
        float f;
    } units[] = {
        {"px", 0, 1},     {"em", 1, 1},     {"rem", 2, 1},    {"ex", 1, 0.5f}, {"ch", 1, 0.6f},
        {"lh", 1, 1.2f},  {"rlh", 2, 1.2f}, {"cap", 1, 0.7f}, {"ic", 1, 1},    {"vw", 3, 1},
        {"vh", 4, 1},     {"svw", 3, 1},    {"svh", 4, 1},    {"lvw", 3, 1},   {"lvh", 4, 1},
        {"dvw", 3, 1},    {"dvh", 4, 1},    {"cqw", 3, 1},    {"cqh", 4, 1},   {"vi", 3, 1},
        {"vb", 4, 1},     {"vmin", 5, 1},   {"vmax", 6, 1},   {"cqmin", 5, 1}, {"cqmax", 6, 1},
        {"pt", 0, 4.0f / 3}, {"pc", 0, 16}, {"in", 0, 96},    {"cm", 0, 96 / 2.54f}, {"mm", 0, 96 / 25.4f},
        {"q", 0, 96 / 101.6f},
    };
    for (size_t i = 0; i < sizeof units / sizeof *units; i++) {
        if (strcmp(units[i].u, b)) continue;
        float f = units[i].f;
        switch (units[i].kind) {
        case 0: *px = v * f; break;
        case 1: *px = v * f * em; break;
        case 2: *px = v * f * cx->rem; break;
        case 3: *px = v * f * cx->vw / 100; break;
        case 4: *px = v * f * cx->vh / 100; break;
        case 5: *px = v * f * (cx->vw < cx->vh ? cx->vw : cx->vh) / 100; break;
        case 6: *px = v * f * (cx->vw > cx->vh ? cx->vw : cx->vh) / 100; break;
        }
        return true;
    }
    return false;
}

static struct cexpr *calc_sum(const char **s, const char *e, struct cx *cx);

static void cskip(const char **s, const char *e) {
    while (*s < e && is_space((unsigned char)**s)) (*s)++;
}

static struct cexpr *cnode(struct cx *cx, char op, struct cexpr *a, struct cexpr *b) {
    struct cexpr *x = ar_alloc(cx->a, sizeof *x);
    x->op = op;
    x->a = a;
    x->b = b;
    return x;
}

/* the arguments of min(), max() or clamp() up to the closing parenthesis */
static struct cexpr *calc_args(const char **s, const char *e, struct cx *cx, char op, bool clamp) {
    struct cexpr *args[16];
    int n = 0;
    for (;;) {
        cskip(s, e);
        struct cexpr *x = calc_sum(s, e, cx);
        if (!x || n >= 16) return NULL;
        args[n++] = x;
        cskip(s, e);
        if (*s < e && **s == ',') {
            (*s)++;
            continue;
        }
        if (*s < e && **s == ')') {
            (*s)++;
            break;
        }
        return NULL;
    }
    if (clamp) {
        if (n != 3) return NULL;
        return cnode(cx, 'M', args[0], cnode(cx, 'm', args[1], args[2]));
    }
    struct cexpr *r = args[0];
    for (int i = 1; i < n; i++) r = cnode(cx, op, r, args[i]);
    return r;
}

static struct cexpr *calc_value(const char **s, const char *e, struct cx *cx) {
    cskip(s, e);
    if (*s >= e) return NULL;
    if (**s == '(') {
        (*s)++;
        struct cexpr *x = calc_sum(s, e, cx);
        cskip(s, e);
        if (!x || *s >= e || **s != ')') return NULL;
        (*s)++;
        return x;
    }
    const char *st = *s;
    while (*s < e && (isalpha((unsigned char)**s) || **s == '-') && !(**s == '-' && *s + 1 < e && isdigit((unsigned char)(*s)[1])))
        (*s)++;
    if (*s > st && *s < e && **s == '(') {
        size_t fl = (size_t)(*s - st);
        (*s)++;
        if (ident_is(st, fl, "calc") || ident_is(st, fl, "-webkit-calc")) {
            struct cexpr *x = calc_sum(s, e, cx);
            cskip(s, e);
            if (!x || *s >= e || **s != ')') return NULL;
            (*s)++;
            return x;
        }
        if (ident_is(st, fl, "min")) return calc_args(s, e, cx, 'm', false);
        if (ident_is(st, fl, "max")) return calc_args(s, e, cx, 'M', false);
        if (ident_is(st, fl, "clamp")) return calc_args(s, e, cx, 0, true);
        return NULL;
    }
    *s = st;
    float v;
    if (!parse_number(s, e, &v)) return NULL;
    const char *u = *s;
    while (*s < e && (isalpha((unsigned char)**s) || **s == '%')) (*s)++;
    struct cexpr *x = cnode(cx, 'v', NULL, NULL);
    if (!unit_px(u, (size_t)(*s - u), v, cx, &x->px, &x->pct, &x->num)) return NULL;
    return x;
}

static struct cexpr *calc_product(const char **s, const char *e, struct cx *cx) {
    struct cexpr *x = calc_value(s, e, cx);
    for (;;) {
        if (!x) return NULL;
        cskip(s, e);
        if (*s < e && (**s == '*' || **s == '/')) {
            char op = *(*s)++;
            x = cnode(cx, op, x, calc_value(s, e, cx));
            if (!x->b) return NULL;
        } else return x;
    }
}

static struct cexpr *calc_sum(const char **s, const char *e, struct cx *cx) {
    struct cexpr *x = calc_product(s, e, cx);
    for (;;) {
        if (!x) return NULL;
        cskip(s, e);
        if (*s < e && (**s == '+' || **s == '-')) {
            char op = *(*s)++;
            x = cnode(cx, op, x, calc_product(s, e, cx));
            if (!x->b) return NULL;
        } else return x;
    }
}

/* fold to px + pct% when possible */
static bool cfold(const struct cexpr *x, bool *num, float *px, float *pct) {
    if (x->op == 'v') {
        *num = x->num;
        *px = x->px;
        *pct = x->pct;
        return true;
    }
    bool an, bn;
    float apx, apc, bpx, bpc;
    if (!cfold(x->a, &an, &apx, &apc) || !cfold(x->b, &bn, &bpx, &bpc)) return false;
    switch (x->op) {
    case '+': case '-': {
        float sg = x->op == '-' ? -1 : 1;
        if (an != bn) return false;
        *num = an;
        *px = apx + sg * bpx;
        *pct = apc + sg * bpc;
        return true;
    }
    case '*':
        if (an) {
            *num = bn;
            *px = apx * bpx;
            *pct = apx * bpc;
            return true;
        }
        if (bn) {
            *num = false;
            *px = apx * bpx;
            *pct = apc * bpx;
            return true;
        }
        return false;
    case '/':
        if (!bn || bpx == 0) return false;
        *num = an;
        *px = apx / bpx;
        *pct = apc / bpx;
        return true;
    case 'm': case 'M':
        if (an != bn || apc != 0 || bpc != 0) return false;
        *num = an;
        *pct = 0;
        *px = x->op == 'm' ? (apx < bpx ? apx : bpx) : (apx > bpx ? apx : bpx);
        return true;
    }
    return false;
}

static float ceval(const struct cexpr *x, float base) {
    switch (x->op) {
    case 'v': return x->px + x->pct * base / 100;
    case '+': return ceval(x->a, base) + ceval(x->b, base);
    case '-': return ceval(x->a, base) - ceval(x->b, base);
    case '*': return ceval(x->a, base) * ceval(x->b, base);
    case '/': {
        float d = ceval(x->b, base);
        return d ? ceval(x->a, base) / d : 0;
    }
    case 'm': {
        float a = ceval(x->a, base), b = ceval(x->b, base);
        return a < b ? a : b;
    }
    case 'M': {
        float a = ceval(x->a, base), b = ceval(x->b, base);
        return a > b ? a : b;
    }
    }
    return 0;
}

/* lengths are kept within +-LEN_MAX px, so that sums of them stay finite and fit an int */
#define LEN_MAX 1e7f
static float len_clamp(float v) { return v != v ? 0 : v > LEN_MAX ? LEN_MAX : v < -LEN_MAX ? -LEN_MAX : v; }

float len_resolve(const len_t *l, float base) {
    switch (l->kind) {
    case LK_LEN: return len_clamp(l->px + (l->pct ? l->pct * base / 100 : 0));
    case LK_NUMBER: return len_clamp(l->px);
    case LK_EXPR: return len_clamp(ceval(l->expr, base));
    }
    return 0;
}

static bool parse_len(const char *s, size_t n, struct cx *cx, len_t *out, int flags) {
    trim(&s, &n);
    if (!n) return false;
    memset(out, 0, sizeof *out);
    if (isalpha((unsigned char)s[0]) || (s[0] == '-' && n > 1 && isalpha((unsigned char)s[1]))) {
        size_t k = 0;
        while (k < n && s[k] != '(') k++;
        if (k == n) {
            if (ident_is(s, n, "auto")) {
                if (!(flags & LF_AUTO)) return false;
                out->kind = LK_AUTO;
                return true;
            }
            if (ident_is(s, n, "none")) {
                if (!(flags & LF_NONE)) return false;
                out->kind = LK_NONE;
                return true;
            }
            if (ident_is(s, n, "normal")) {
                if (!(flags & LF_NORMAL)) return false;
                out->kind = LK_NORMAL;
                return true;
            }
            if (ident_is(s, n, "min-content") || ident_is(s, n, "max-content") || ident_is(s, n, "fit-content") ||
                ident_is(s, n, "-webkit-fill-available") || ident_is(s, n, "-moz-available") ||
                ident_is(s, n, "stretch") || ident_is(s, n, "-webkit-fit-content") || ident_is(s, n, "-moz-fit-content")) {
                if (flags & LF_AUTO) out->kind = LK_AUTO;
                else if (flags & LF_NONE) out->kind = LK_NONE;
                else return false;
                return true;
            }
            return false;
        }
        if (ident_is(s, k, "fit-content")) {
            if (!(flags & LF_AUTO)) return false;
            out->kind = LK_AUTO;
            return true;
        }
        const char *p = s;
        struct cexpr *x = calc_value(&p, s + n, cx);
        cskip(&p, s + n);
        if (!x || p != s + n) return false;
        bool num;
        float px, pct;
        if (cfold(x, &num, &px, &pct)) {
            if ((flags & LF_NOPCT) && pct) return false;
            if (!(flags & LF_NEG) && pct == 0 && px < 0) px = 0; /* calc() clamps to the range */
            out->kind = LK_LEN;
            out->px = px;
            out->pct = pct;
            return true;
        }
        out->kind = LK_EXPR;
        out->expr = x;
        return true;
    }
    const char *p = s;
    float v;
    if (!parse_number(&p, s + n, &v)) return false;
    bool num;
    if (!unit_px(p, (size_t)(s + n - p), v, cx, &out->px, &out->pct, &num)) return false;
    if ((flags & LF_NOPCT) && out->pct) return false;
    if (!(flags & LF_NEG) && v < 0) return false;
    out->kind = LK_LEN;
    return true;
}

/* ---------------------------------------------------------------- colours */
static const struct {
    const char *name;
    uint32_t rgb;
} named[] = {
    {"aliceblue", 0xF0F8FF}, {"antiquewhite", 0xFAEBD7}, {"aqua", 0x00FFFF}, {"aquamarine", 0x7FFFD4},
    {"azure", 0xF0FFFF}, {"beige", 0xF5F5DC}, {"bisque", 0xFFE4C4}, {"black", 0x000000},
    {"blanchedalmond", 0xFFEBCD}, {"blue", 0x0000FF}, {"blueviolet", 0x8A2BE2}, {"brown", 0xA52A2A},
    {"burlywood", 0xDEB887}, {"buttonface", 0xEFEFEF}, {"buttontext", 0x000000}, {"cadetblue", 0x5F9EA0},
    {"canvas", 0xFFFFFF}, {"canvastext", 0x000000}, {"chartreuse", 0x7FFF00}, {"chocolate", 0xD2691E},
    {"coral", 0xFF7F50}, {"cornflowerblue", 0x6495ED}, {"cornsilk", 0xFFF8DC}, {"crimson", 0xDC143C},
    {"cyan", 0x00FFFF}, {"darkblue", 0x00008B}, {"darkcyan", 0x008B8B}, {"darkgoldenrod", 0xB8860B},
    {"darkgray", 0xA9A9A9}, {"darkgreen", 0x006400}, {"darkgrey", 0xA9A9A9}, {"darkkhaki", 0xBDB76B},
    {"darkmagenta", 0x8B008B}, {"darkolivegreen", 0x556B2F}, {"darkorange", 0xFF8C00}, {"darkorchid", 0x9932CC},
    {"darkred", 0x8B0000}, {"darksalmon", 0xE9967A}, {"darkseagreen", 0x8FBC8F}, {"darkslateblue", 0x483D8B},
    {"darkslategray", 0x2F4F4F}, {"darkslategrey", 0x2F4F4F}, {"darkturquoise", 0x00CED1}, {"darkviolet", 0x9400D3},
    {"deeppink", 0xFF1493}, {"deepskyblue", 0x00BFFF}, {"dimgray", 0x696969}, {"dimgrey", 0x696969},
    {"dodgerblue", 0x1E90FF}, {"field", 0xFFFFFF}, {"fieldtext", 0x000000}, {"firebrick", 0xB22222},
    {"floralwhite", 0xFFFAF0}, {"forestgreen", 0x228B22}, {"fuchsia", 0xFF00FF}, {"gainsboro", 0xDCDCDC},
    {"ghostwhite", 0xF8F8FF}, {"gold", 0xFFD700}, {"goldenrod", 0xDAA520}, {"gray", 0x808080},
    {"graytext", 0x808080}, {"green", 0x008000}, {"greenyellow", 0xADFF2F}, {"grey", 0x808080},
    {"highlight", 0x3390FF}, {"highlighttext", 0xFFFFFF}, {"honeydew", 0xF0FFF0}, {"hotpink", 0xFF69B4},
    {"indianred", 0xCD5C5C}, {"indigo", 0x4B0082}, {"ivory", 0xFFFFF0}, {"khaki", 0xF0E68C},
    {"lavender", 0xE6E6FA}, {"lavenderblush", 0xFFF0F5}, {"lawngreen", 0x7CFC00}, {"lemonchiffon", 0xFFFACD},
    {"lightblue", 0xADD8E6}, {"lightcoral", 0xF08080}, {"lightcyan", 0xE0FFFF}, {"lightgoldenrodyellow", 0xFAFAD2},
    {"lightgray", 0xD3D3D3}, {"lightgreen", 0x90EE90}, {"lightgrey", 0xD3D3D3}, {"lightpink", 0xFFB6C1},
    {"lightsalmon", 0xFFA07A}, {"lightseagreen", 0x20B2AA}, {"lightskyblue", 0x87CEFA}, {"lightslategray", 0x778899},
    {"lightslategrey", 0x778899}, {"lightsteelblue", 0xB0C4DE}, {"lightyellow", 0xFFFFE0}, {"lime", 0x00FF00},
    {"limegreen", 0x32CD32}, {"linen", 0xFAF0E6}, {"linktext", 0x0000EE}, {"magenta", 0xFF00FF},
    {"maroon", 0x800000}, {"mediumaquamarine", 0x66CDAA}, {"mediumblue", 0x0000CD}, {"mediumorchid", 0xBA55D3},
    {"mediumpurple", 0x9370DB}, {"mediumseagreen", 0x3CB371}, {"mediumslateblue", 0x7B68EE},
    {"mediumspringgreen", 0x00FA9A}, {"mediumturquoise", 0x48D1CC}, {"mediumvioletred", 0xC71585},
    {"midnightblue", 0x191970}, {"mintcream", 0xF5FFFA}, {"mistyrose", 0xFFE4E1}, {"moccasin", 0xFFE4B5},
    {"navajowhite", 0xFFDEAD}, {"navy", 0x000080}, {"oldlace", 0xFDF5E6}, {"olive", 0x808000},
    {"olivedrab", 0x6B8E23}, {"orange", 0xFFA500}, {"orangered", 0xFF4500}, {"orchid", 0xDA70D6},
    {"palegoldenrod", 0xEEE8AA}, {"palegreen", 0x98FB98}, {"paleturquoise", 0xAFEEEE}, {"palevioletred", 0xDB7093},
    {"papayawhip", 0xFFEFD5}, {"peachpuff", 0xFFDAB9}, {"peru", 0xCD853F}, {"pink", 0xFFC0CB},
    {"plum", 0xDDA0DD}, {"powderblue", 0xB0E0E6}, {"purple", 0x800080}, {"rebeccapurple", 0x663399},
    {"red", 0xFF0000}, {"rosybrown", 0xBC8F8F}, {"royalblue", 0x4169E1}, {"saddlebrown", 0x8B4513},
    {"salmon", 0xFA8072}, {"sandybrown", 0xF4A460}, {"seagreen", 0x2E8B57}, {"seashell", 0xFFF5EE},
    {"sienna", 0xA0522D}, {"silver", 0xC0C0C0}, {"skyblue", 0x87CEEB}, {"slateblue", 0x6A5ACD},
    {"slategray", 0x708090}, {"slategrey", 0x708090}, {"snow", 0xFFFAFA}, {"springgreen", 0x00FF7F},
    {"steelblue", 0x4682B4}, {"tan", 0xD2B48C}, {"teal", 0x008080}, {"thistle", 0xD8BFD8},
    {"tomato", 0xFF6347}, {"turquoise", 0x40E0D0}, {"violet", 0xEE82EE}, {"wheat", 0xF5DEB3},
    {"white", 0xFFFFFF}, {"whitesmoke", 0xF5F5F5}, {"window", 0xFFFFFF}, {"windowtext", 0x000000},
    {"yellow", 0xFFFF00}, {"yellowgreen", 0x9ACD32},
};

static int hexd(int c) {
    c |= 32;
    return c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : -1;
}

static uint8_t clamp255(float v) { return v <= 0 ? 0 : v >= 255 ? 255 : (uint8_t)(v + 0.5f); }

/* a colour component: a number scaled by full (for 100%), or 'none' */
static bool comp(const char *s, size_t n, float full, float *out) {
    if (ident_is(s, n, "none")) {
        *out = 0;
        return true;
    }
    const char *p = s;
    float v;
    if (!parse_number(&p, s + n, &v)) return false;
    size_t un = (size_t)(s + n - p);
    if (un == 1 && *p == '%') v = v * full / 100;
    else if (un) {
        /* angles */
        if (ident_is(p, un, "deg")) {
        } else if (ident_is(p, un, "turn")) v *= 360;
        else if (ident_is(p, un, "rad")) v = v * 180 / (float)M_PI;
        else if (ident_is(p, un, "grad")) v *= 0.9f;
        else return false;
    }
    *out = v;
    return true;
}

static float hue2rgb(float p, float q, float t) {
    if (t < 0) t += 1;
    if (t > 1) t -= 1;
    if (t < 1.0f / 6) return p + (q - p) * 6 * t;
    if (t < 0.5f) return q;
    if (t < 2.0f / 3) return p + (q - p) * (2.0f / 3 - t) * 6;
    return p;
}

static float srgb_gamma(float x) {
    if (x <= 0.0031308f) return 12.92f * x;
    return 1.055f * (float)pow(x, 1 / 2.4) - 0.055f;
}

bool css_color(const char *s, size_t n, uint32_t *out) {
    trim(&s, &n);
    if (!n) return false;
    if (s[0] == '#') {
        int d[8];
        size_t k = n - 1;
        if (k != 3 && k != 4 && k != 6 && k != 8) return false;
        for (size_t i = 0; i < k; i++)
            if ((d[i] = hexd((unsigned char)s[1 + i])) < 0) return false;
        uint32_t r, g, b, a = 255;
        if (k <= 4) {
            r = (uint32_t)d[0] * 17, g = (uint32_t)d[1] * 17, b = (uint32_t)d[2] * 17;
            if (k == 4) a = (uint32_t)d[3] * 17;
        } else {
            r = (uint32_t)(d[0] * 16 + d[1]), g = (uint32_t)(d[2] * 16 + d[3]), b = (uint32_t)(d[4] * 16 + d[5]);
            if (k == 8) a = (uint32_t)(d[6] * 16 + d[7]);
        }
        *out = ARGB(a, r, g, b);
        return true;
    }
    if (s[n - 1] == ')') {
        size_t fl = 0;
        while (fl < n && s[fl] != '(') fl++;
        if (fl == n) return false;
        const char *args = s + fl + 1;
        size_t an = n - fl - 2;
        if (ident_is(s, fl, "light-dark")) {
            const char *t[2];
            size_t tl[2];
            if (split(args, an, t, tl, 2, ',') < 1) return false;
            return css_color(t[0], tl[0], out);
        }
        if (ident_is(s, fl, "color-mix")) {
            const char *t[3];
            size_t tl[3];
            if (split(args, an, t, tl, 3, ',') != 3) return false;
            uint32_t c[2];
            float p[2] = {-1, -1};
            for (int i = 0; i < 2; i++) {
                const char *u[2];
                size_t ul[2];
                int k = split(t[i + 1], tl[i + 1], u, ul, 2, ' ');
                int ci = 0;
                if (k == 2 && u[0][ul[0] - 1] == '%') ci = 1;
                if (!k || !css_color(u[ci], ul[ci], &c[i])) return false;
                if (k == 2) {
                    const char *q = u[1 - ci];
                    if (!parse_number(&q, u[1 - ci] + ul[1 - ci], &p[i])) return false;
                }
            }
            if (p[0] < 0 && p[1] < 0) p[0] = p[1] = 50;
            else if (p[0] < 0) p[0] = 100 - p[1];
            else if (p[1] < 0) p[1] = 100 - p[0];
            float tot = p[0] + p[1];
            if (tot <= 0) return false;
            float w = p[1] / tot;
            float ch[4];
            for (int k = 0; k < 4; k++) {
                float a = (float)(c[0] >> (24 - 8 * k) & 255), b = (float)(c[1] >> (24 - 8 * k) & 255);
                ch[k] = a + (b - a) * w;
            }
            float alpha = ch[0] * (tot < 100 ? tot / 100 : 1);
            *out = ARGB(clamp255(alpha), clamp255(ch[1]), clamp255(ch[2]), clamp255(ch[3]));
            return true;
        }
        /* rgb(), hsl(), oklch(), oklab(): components separated by commas or spaces, alpha after / */
        char buf[256];
        if (an >= sizeof buf) return false;
        for (size_t i = 0; i < an; i++) buf[i] = args[i] == ',' || args[i] == '/' ? ' ' : args[i];
        const char *t[4];
        size_t tl[4];
        int k = split(buf, an, t, tl, 4, ' ');
        if (k < 3) return false;
        float alpha = 1;
        if (k == 4 && !comp(t[3], tl[3], 1, &alpha)) return false;
        float c0, c1, c2;
        if (ident_is(s, fl, "rgb") || ident_is(s, fl, "rgba")) {
            if (!comp(t[0], tl[0], 255, &c0) || !comp(t[1], tl[1], 255, &c1) || !comp(t[2], tl[2], 255, &c2))
                return false;
        } else if (ident_is(s, fl, "hsl") || ident_is(s, fl, "hsla")) {
            float h, sat, l;
            if (!comp(t[0], tl[0], 360, &h) || !comp(t[1], tl[1], 100, &sat) || !comp(t[2], tl[2], 100, &l))
                return false;
            h = (float)fmod(h, 360) / 360;
            if (h < 0) h += 1;
            sat /= 100, l /= 100;
            float q = l < 0.5f ? l * (1 + sat) : l + sat - l * sat, p = 2 * l - q;
            c0 = hue2rgb(p, q, h + 1.0f / 3) * 255;
            c1 = hue2rgb(p, q, h) * 255;
            c2 = hue2rgb(p, q, h - 1.0f / 3) * 255;
        } else if (ident_is(s, fl, "oklch") || ident_is(s, fl, "oklab")) {
            float L, a, b;
            bool lch = ident_is(s, fl, "oklch");
            if (!comp(t[0], tl[0], 1, &L)) return false;
            if (lch) {
                float C, H;
                if (!comp(t[1], tl[1], 0.4f, &C) || !comp(t[2], tl[2], 360, &H)) return false;
                a = C * cosf(H * (float)M_PI / 180);
                b = C * sinf(H * (float)M_PI / 180);
            } else if (!comp(t[1], tl[1], 0.4f, &a) || !comp(t[2], tl[2], 0.4f, &b)) return false;
            float l_ = L + 0.3963377774f * a + 0.2158037573f * b;
            float m_ = L - 0.1055613458f * a - 0.0638541728f * b;
            float s_ = L - 0.0894841775f * a - 1.2914855480f * b;
            float l3 = l_ * l_ * l_, m3 = m_ * m_ * m_, s3 = s_ * s_ * s_;
            c0 = srgb_gamma(4.0767416621f * l3 - 3.3077115913f * m3 + 0.2309699292f * s3) * 255;
            c1 = srgb_gamma(-1.2684380046f * l3 + 2.6097574011f * m3 - 0.3413193965f * s3) * 255;
            c2 = srgb_gamma(-0.0041960863f * l3 - 0.7034186147f * m3 + 1.7076147010f * s3) * 255;
        } else return false;
        *out = ARGB(clamp255(alpha * 255), clamp255(c0), clamp255(c1), clamp255(c2));
        return true;
    }
    if (ident_is(s, n, "transparent")) {
        *out = 0;
        return true;
    }
    if (ident_is(s, n, "currentcolor")) {
        *out = COLOR_CURRENT;
        return true;
    }
    int lo = 0, hi = (int)(sizeof named / sizeof *named) - 1;
    while (lo <= hi) {
        int mid = (lo + hi) / 2;
        const char *nm = named[mid].name;
        int c = 0;
        size_t i = 0;
        for (; i < n && nm[i]; i++)
            if ((c = lower((unsigned char)s[i]) - nm[i])) break;
        if (!c) c = i < n ? 1 : nm[i] ? -1 : 0;
        if (!c) {
            *out = 0xFF000000u | named[mid].rgb;
            return true;
        }
        if (c < 0) hi = mid - 1;
        else lo = mid + 1;
    }
    return false;
}

/* ---------------------------------------------------------------- the property table */
struct kw {
    const char *name;
    uint8_t v;
};

enum { PT_SHORT, PT_KW, PT_DISPLAY, PT_LEN, PT_PX, PT_COLOR, PT_NUM, PT_INT, PT_FONT_SIZE, PT_FONT_WEIGHT,
       PT_FONT_FAMILY, PT_LINE_HEIGHT, PT_VALIGN, PT_CONTENT, PT_LIST_TYPE, PT_BG_IMAGE, PT_TEXT_DECO,
       PT_OVERFLOW, PT_OPACITY, PT_RADIUS, PT_ZINDEX, PT_BG_POS, PT_BG_SIZE, PT_GTEMPLATE, PT_GAREAS, PT_GLINE, PT_MASK_IMAGE, PT_MASK_SIZE };

enum { SH_MARGIN = 1, SH_PADDING, SH_INSET, SH_BORDER, SH_BORDER_TOP, SH_BORDER_RIGHT, SH_BORDER_BOTTOM,
       SH_BORDER_LEFT, SH_BORDER_WIDTH, SH_BORDER_STYLE, SH_BORDER_COLOR, SH_BORDER_INLINE, SH_BORDER_BLOCK,
       SH_MARGIN_INLINE, SH_MARGIN_BLOCK, SH_PADDING_INLINE, SH_PADDING_BLOCK, SH_INSET_INLINE, SH_INSET_BLOCK,
       SH_LIST_STYLE, SH_FONT, SH_BACKGROUND, SH_FLEX, SH_FLEX_FLOW, SH_GAP, SH_OVERFLOW, SH_TEXT_DECORATION,
       SH_PLACE_ITEMS, SH_PLACE_SELF, SH_PLACE_CONTENT, SH_GRID_AREA, SH_GRID_ROW, SH_GRID_COLUMN,
       SH_GRID_TEMPLATE, SH_GRID };

struct propdef {
    const char *name;
    uint8_t type, flags; /* flags: LF_* for lengths */
    bool inherited;
    uint16_t off, size;
    const struct kw *kws;
    uint8_t sh;
};

static const struct kw kw_position[] = {{"static", POS_STATIC}, {"relative", POS_RELATIVE}, {"absolute", POS_ABSOLUTE},
                                        {"fixed", POS_FIXED}, {"sticky", POS_STICKY}, {"-webkit-sticky", POS_STICKY}, {0}};
static const struct kw kw_float[] = {{"none", FL_NONE}, {"left", FL_LEFT}, {"right", FL_RIGHT},
                                     {"inline-start", FL_LEFT}, {"inline-end", FL_RIGHT}, {0}};
static const struct kw kw_clear[] = {{"none", CL_NONE}, {"left", CL_LEFT}, {"right", CL_RIGHT}, {"both", CL_BOTH},
                                     {"inline-start", CL_LEFT}, {"inline-end", CL_RIGHT}, {0}};
static const struct kw kw_ws[] = {{"normal", WS_NORMAL}, {"pre", WS_PRE}, {"nowrap", WS_NOWRAP},
                                  {"pre-wrap", WS_PRE_WRAP}, {"pre-line", WS_PRE_LINE},
                                  {"break-spaces", WS_PRE_WRAP}, {"-moz-pre-wrap", WS_PRE_WRAP}, {0}};
static const struct kw kw_textwrap[] = {{"wrap", WS_NORMAL}, {"nowrap", WS_NOWRAP}, {"balance", WS_NORMAL},
                                        {"pretty", WS_NORMAL}, {"stable", WS_NORMAL}, {0}};
static const struct kw kw_talign[] = {{"left", TA_LEFT}, {"right", TA_RIGHT}, {"center", TA_CENTER},
                                      {"justify", TA_JUSTIFY}, {"start", TA_LEFT}, {"end", TA_RIGHT},
                                      {"-webkit-center", TA_WCENTER}, {"-moz-center", TA_WCENTER},
                                      {"-webkit-left", TA_LEFT}, {"-webkit-right", TA_RIGHT}, {"-moz-left", TA_LEFT},
                                      {"-moz-right", TA_RIGHT}, {"match-parent", TA_LEFT}, {"justify-all", TA_JUSTIFY}, {0}};
static const struct kw kw_lspos[] = {{"outside", 0}, {"inside", 1}, {0}};
static const struct kw kw_fstyle[] = {{"normal", 0}, {"italic", 1}, {"oblique", 1}, {0}};
static const struct kw kw_ttrans[] = {{"none", TT_NONE}, {"uppercase", TT_UPPER}, {"lowercase", TT_LOWER},
                                      {"capitalize", TT_CAPITALIZE}, {"full-width", TT_NONE}, {0}};
static const struct kw kw_overflow[] = {{"visible", OV_VISIBLE}, {"hidden", OV_HIDDEN}, {"scroll", OV_SCROLL},
                                        {"auto", OV_AUTO}, {"clip", OV_CLIP}, {"overlay", OV_AUTO},
                                        {"-moz-hidden-unscrollable", OV_HIDDEN}, {0}};
static const struct kw kw_boxsizing[] = {{"content-box", 0}, {"border-box", 1}, {0}};
static const struct kw kw_bg_repeat[] = {{"repeat", BR_REPEAT}, {"repeat-x", BR_REPEAT_X}, {"repeat-y", BR_REPEAT_Y},
                                         {"no-repeat", BR_NO_REPEAT}, {"space", BR_REPEAT}, {"round", BR_REPEAT},
                                         {0}};
static const struct kw kw_visibility[] = {{"visible", 0}, {"hidden", 1}, {"collapse", 1}, {0}};
static const struct kw kw_bcollapse[] = {{"separate", 0}, {"collapse", 1}, {0}};
static const struct kw kw_fdir[] = {{"row", FD_ROW}, {"row-reverse", FD_ROW_REVERSE}, {"column", FD_COLUMN},
                                    {"column-reverse", FD_COLUMN_REVERSE}, {0}};
static const struct kw kw_fwrap[] = {{"nowrap", 0}, {"wrap", 1}, {"wrap-reverse", 1}, {0}};
static const struct kw kw_justify[] = {{"flex-start", JC_START}, {"start", JC_START}, {"left", JC_START},
                                       {"normal", JC_START}, {"stretch", JC_START}, {"flex-end", JC_END},
                                       {"end", JC_END}, {"right", JC_END}, {"center", JC_CENTER},
                                       {"space-between", JC_BETWEEN}, {"space-around", JC_AROUND},
                                       {"space-evenly", JC_EVENLY}, {"safe center", JC_CENTER}, {0}};
static const struct kw kw_align[] = {{"stretch", AI_STRETCH}, {"normal", AI_STRETCH}, {"flex-start", AI_START},
                                     {"start", AI_START}, {"self-start", AI_START}, {"flex-end", AI_END},
                                     {"end", AI_END}, {"self-end", AI_END}, {"center", AI_CENTER},
                                     {"safe center", AI_CENTER}, {"baseline", AI_BASELINE},
                                     {"first baseline", AI_BASELINE}, {"last baseline", AI_BASELINE},
                                     {"auto", 255}, {0}};
static const struct kw kw_jalign[] = {{"stretch", AI_STRETCH}, {"normal", AI_STRETCH}, {"legacy", AI_STRETCH},
                                      {"start", AI_START}, {"self-start", AI_START}, {"flex-start", AI_START},
                                      {"left", AI_START}, {"end", AI_END}, {"self-end", AI_END},
                                      {"flex-end", AI_END}, {"right", AI_END}, {"center", AI_CENTER},
                                      {"safe center", AI_CENTER}, {"legacy center", AI_CENTER},
                                      {"baseline", AI_START}, {"auto", 255}, {0}};
static const struct kw kw_gflow[] = {{"row", 0}, {"column", 1}, {"dense", 0}, {"row dense", 0},
                                     {"column dense", 1}, {"dense row", 0}, {"dense column", 1}, {0}};
static const struct kw kw_capside[] = {{"top", 0}, {"bottom", 1}, {"block-start", 0}, {"block-end", 1}, {0}};
static const struct kw kw_tlayout[] = {{"auto", 0}, {"fixed", 1}, {0}};
static const struct kw kw_bstyle[] = {{"none", BS_NONE}, {"hidden", BS_HIDDEN}, {"solid", BS_SOLID},
                                      {"dashed", BS_DASHED}, {"dotted", BS_DOTTED}, {"double", BS_DOUBLE},
                                      {"groove", BS_GROOVE}, {"ridge", BS_RIDGE}, {"inset", BS_INSET},
                                      {"outset", BS_OUTSET}, {0}};
static const struct kw kw_bwidth[] = {{"thin", 1}, {"medium", 3}, {"thick", 5}, {0}};
static const struct kw kw_normal0[] = {{"normal", 0}, {0}};
static const struct kw kw_valign[] = {{"baseline", VA_BASELINE}, {"sub", VA_SUB}, {"super", VA_SUPER},
                                      {"top", VA_TOP}, {"middle", VA_MIDDLE}, {"bottom", VA_BOTTOM},
                                      {"text-top", VA_TEXT_TOP}, {"text-bottom", VA_TEXT_BOTTOM}, {0}};

#define O(f) (uint16_t) offsetof(style_t, f), (uint16_t)sizeof(((style_t *)0)->f)
#define SH(name, id) {name, PT_SHORT, 0, false, 0, 0, NULL, id}

/* sorted by name at first use */
static struct propdef props[] = {
    {"align-items", PT_KW, 0, false, O(align_items), kw_align, 0},
    {"align-self", PT_KW, 0, false, O(align_self), kw_align, 0},
    SH("background", SH_BACKGROUND),
    {"background-color", PT_COLOR, 0, false, O(bg_color), NULL, 0},
    {"background-image", PT_BG_IMAGE, 0, false, O(bg_image), NULL, 0},
    {"background-repeat", PT_KW, 0, false, O(bg_repeat), kw_bg_repeat, 0},
    {"background-position", PT_BG_POS, 0, false, O(bg_pos), NULL, 0},
    {"background-size", PT_BG_SIZE, 0, false, O(bg_size), NULL, 0},
    SH("border", SH_BORDER),
    SH("border-block", SH_BORDER_BLOCK),
    SH("border-bottom", SH_BORDER_BOTTOM),
    {"border-bottom-color", PT_COLOR, 0, false, O(border_color[2]), NULL, 0},
    {"border-bottom-style", PT_KW, 0, false, O(border_style[2]), kw_bstyle, 0},
    {"border-bottom-width", PT_PX, LF_NOPCT, false, O(border_width[2]), kw_bwidth, 0},
    {"border-collapse", PT_KW, 0, true, O(border_collapse), kw_bcollapse, 0},
    SH("border-color", SH_BORDER_COLOR),
    SH("border-inline", SH_BORDER_INLINE),
    SH("border-left", SH_BORDER_LEFT),
    {"border-left-color", PT_COLOR, 0, false, O(border_color[3]), NULL, 0},
    {"border-left-style", PT_KW, 0, false, O(border_style[3]), kw_bstyle, 0},
    {"border-left-width", PT_PX, LF_NOPCT, false, O(border_width[3]), kw_bwidth, 0},
    {"border-radius", PT_RADIUS, 0, false, O(border_radius), NULL, 0},
    SH("border-right", SH_BORDER_RIGHT),
    {"border-right-color", PT_COLOR, 0, false, O(border_color[1]), NULL, 0},
    {"border-right-style", PT_KW, 0, false, O(border_style[1]), kw_bstyle, 0},
    {"border-right-width", PT_PX, LF_NOPCT, false, O(border_width[1]), kw_bwidth, 0},
    {"border-spacing", PT_PX, LF_NOPCT, true, O(border_spacing), NULL, 0},
    SH("border-style", SH_BORDER_STYLE),
    SH("border-top", SH_BORDER_TOP),
    {"border-top-color", PT_COLOR, 0, false, O(border_color[0]), NULL, 0},
    {"border-top-style", PT_KW, 0, false, O(border_style[0]), kw_bstyle, 0},
    {"border-top-width", PT_PX, LF_NOPCT, false, O(border_width[0]), kw_bwidth, 0},
    SH("border-width", SH_BORDER_WIDTH),
    {"bottom", PT_LEN, LF_AUTO | LF_NEG, false, O(inset[2]), NULL, 0},
    {"box-sizing", PT_KW, 0, false, O(box_sizing), kw_boxsizing, 0},
    {"caption-side", PT_KW, 0, true, O(caption_bottom), kw_capside, 0},
    {"clear", PT_KW, 0, false, O(clear), kw_clear, 0},
    {"color", PT_COLOR, 0, true, O(color), NULL, 0},
    {"column-gap", PT_PX, LF_NOPCT, false, O(gap_col), kw_normal0, 0},
    {"content", PT_CONTENT, 0, false, O(content), NULL, 0},
    {"display", PT_DISPLAY, 0, false, O(display), NULL, 0},
    SH("flex", SH_FLEX),
    {"flex-basis", PT_LEN, LF_AUTO, false, O(flex_basis), NULL, 0},
    {"flex-direction", PT_KW, 0, false, O(flex_direction), kw_fdir, 0},
    SH("flex-flow", SH_FLEX_FLOW),
    {"flex-grow", PT_NUM, 0, false, O(flex_grow), NULL, 0},
    {"flex-shrink", PT_NUM, 0, false, O(flex_shrink), NULL, 0},
    {"flex-wrap", PT_KW, 0, false, O(flex_wrap), kw_fwrap, 0},
    {"float", PT_KW, 0, false, O(float_), kw_float, 0},
    SH("font", SH_FONT),
    {"font-family", PT_FONT_FAMILY, 0, true, O(monospace), NULL, 0},
    {"font-size", PT_FONT_SIZE, 0, true, O(font_size), NULL, 0},
    {"font-style", PT_KW, 0, true, O(font_style), kw_fstyle, 0},
    {"font-weight", PT_FONT_WEIGHT, 0, true, O(font_weight), NULL, 0},
    SH("gap", SH_GAP),
    SH("grid", SH_GRID),
    SH("grid-area", SH_GRID_AREA),
    {"grid-auto-columns", PT_GTEMPLATE, 0, false, O(grid_auto_cols), NULL, 0},
    {"grid-auto-flow", PT_KW, 0, false, O(grid_flow_col), kw_gflow, 0},
    {"grid-auto-rows", PT_GTEMPLATE, 0, false, O(grid_auto_rows), NULL, 0},
    SH("grid-column", SH_GRID_COLUMN),
    {"grid-column-end", PT_GLINE, 0, false, O(grid_place[3]), NULL, 0},
    {"grid-column-start", PT_GLINE, 0, false, O(grid_place[1]), NULL, 0},
    SH("grid-row", SH_GRID_ROW),
    {"grid-row-end", PT_GLINE, 0, false, O(grid_place[2]), NULL, 0},
    {"grid-row-start", PT_GLINE, 0, false, O(grid_place[0]), NULL, 0},
    SH("grid-template", SH_GRID_TEMPLATE),
    {"grid-template-areas", PT_GAREAS, 0, false, O(grid_areas), NULL, 0},
    {"grid-template-columns", PT_GTEMPLATE, 0, false, O(grid_cols), NULL, 0},
    {"grid-template-rows", PT_GTEMPLATE, 0, false, O(grid_rows), NULL, 0},
    {"height", PT_LEN, LF_AUTO, false, O(height), NULL, 0},
    SH("inset", SH_INSET),
    SH("inset-block", SH_INSET_BLOCK),
    SH("inset-inline", SH_INSET_INLINE),
    {"justify-content", PT_KW, 0, false, O(justify_content), kw_justify, 0},
    {"justify-items", PT_KW, 0, false, O(justify_items), kw_jalign, 0},
    {"justify-self", PT_KW, 0, false, O(justify_self), kw_jalign, 0},
    {"left", PT_LEN, LF_AUTO | LF_NEG, false, O(inset[3]), NULL, 0},
    {"letter-spacing", PT_PX, LF_NEG | LF_NOPCT, true, O(letter_spacing), kw_normal0, 0},
    {"line-height", PT_LINE_HEIGHT, 0, true, O(line_height), NULL, 0},
    SH("list-style", SH_LIST_STYLE),
    {"list-style-position", PT_KW, 0, true, O(list_style_inside), kw_lspos, 0},
    {"list-style-type", PT_LIST_TYPE, 0, true, O(list_style), NULL, 0},
    SH("margin", SH_MARGIN),
    SH("margin-block", SH_MARGIN_BLOCK),
    {"margin-bottom", PT_LEN, LF_AUTO | LF_NEG, false, O(margin[2]), NULL, 0},
    SH("margin-inline", SH_MARGIN_INLINE),
    {"margin-left", PT_LEN, LF_AUTO | LF_NEG, false, O(margin[3]), NULL, 0},
    {"margin-right", PT_LEN, LF_AUTO | LF_NEG, false, O(margin[1]), NULL, 0},
    {"margin-top", PT_LEN, LF_AUTO | LF_NEG, false, O(margin[0]), NULL, 0},
    {"mask-image", PT_MASK_IMAGE, 0, false, O(mask_image), NULL, 0},
    {"mask-position", PT_BG_POS, 0, false, O(mask_pos), NULL, 0},
    {"mask-repeat", PT_KW, 0, false, O(mask_repeat), kw_bg_repeat, 0},
    {"mask-size", PT_MASK_SIZE, 0, false, O(mask_size), NULL, 0},
    {"max-height", PT_LEN, LF_NONE, false, O(max_height), NULL, 0},
    {"max-width", PT_LEN, LF_NONE, false, O(max_width), NULL, 0},
    {"min-height", PT_LEN, LF_AUTO, false, O(min_height), NULL, 0},
    {"min-width", PT_LEN, LF_AUTO, false, O(min_width), NULL, 0},
    {"opacity", PT_OPACITY, 0, false, O(opacity), NULL, 0},
    {"order", PT_INT, 0, false, O(order), NULL, 0},
    SH("overflow", SH_OVERFLOW),
    {"overflow-x", PT_OVERFLOW, 0, false, O(overflow), kw_overflow, 0},
    {"overflow-y", PT_OVERFLOW, 0, false, O(overflow), kw_overflow, 0},
    SH("padding", SH_PADDING),
    SH("padding-block", SH_PADDING_BLOCK),
    {"padding-bottom", PT_LEN, 0, false, O(padding[2]), NULL, 0},
    SH("padding-inline", SH_PADDING_INLINE),
    {"padding-left", PT_LEN, 0, false, O(padding[3]), NULL, 0},
    {"padding-right", PT_LEN, 0, false, O(padding[1]), NULL, 0},
    {"padding-top", PT_LEN, 0, false, O(padding[0]), NULL, 0},
    SH("place-content", SH_PLACE_CONTENT),
    SH("place-items", SH_PLACE_ITEMS),
    SH("place-self", SH_PLACE_SELF),
    {"position", PT_KW, 0, false, O(position), kw_position, 0},
    {"right", PT_LEN, LF_AUTO | LF_NEG, false, O(inset[1]), NULL, 0},
    {"row-gap", PT_PX, LF_NOPCT, false, O(gap_row), kw_normal0, 0},
    {"table-layout", PT_KW, 0, false, O(table_layout), kw_tlayout, 0},
    {"text-align", PT_KW, 0, true, O(text_align), kw_talign, 0},
    SH("text-decoration", SH_TEXT_DECORATION),
    {"text-decoration-line", PT_TEXT_DECO, 0, true, O(text_decoration), NULL, 0},
    {"text-indent", PT_LEN, LF_NEG, true, O(text_indent), NULL, 0},
    {"text-transform", PT_KW, 0, true, O(text_transform), kw_ttrans, 0},
    {"text-wrap-mode", PT_KW, 0, true, O(white_space), kw_textwrap, 0},
    {"top", PT_LEN, LF_AUTO | LF_NEG, false, O(inset[0]), NULL, 0},
    {"vertical-align", PT_VALIGN, 0, false, O(vertical_align), kw_valign, 0},
    {"visibility", PT_KW, 0, true, O(visibility), kw_visibility, 0},
    {"white-space", PT_KW, 0, true, O(white_space), kw_ws, 0},
    {"width", PT_LEN, LF_AUTO, false, O(width), NULL, 0},
    {"word-spacing", PT_PX, LF_NEG | LF_NOPCT, true, O(word_spacing), kw_normal0, 0},
    {"z-index", PT_ZINDEX, 0, false, O(z_index), NULL, 0},
};
#define NPROPS (int)(sizeof props / sizeof *props)

/* other names for the same properties (logical properties map to left-to-right, horizontal text) */
static const struct {
    const char *alias, *name;
} aliases[] = {
    {"block-size", "height"}, {"border-block-end", "border-bottom"}, {"border-block-start", "border-top"},
    {"border-block-end-color", "border-bottom-color"}, {"border-block-start-color", "border-top-color"},
    {"border-block-end-width", "border-bottom-width"}, {"border-block-start-width", "border-top-width"},
    {"border-block-end-style", "border-bottom-style"}, {"border-block-start-style", "border-top-style"},
    {"border-inline-end", "border-right"}, {"border-inline-start", "border-left"},
    {"border-inline-end-color", "border-right-color"}, {"border-inline-start-color", "border-left-color"},
    {"border-inline-end-width", "border-right-width"}, {"border-inline-start-width", "border-left-width"},
    {"border-inline-end-style", "border-right-style"}, {"border-inline-start-style", "border-left-style"},
    {"grid-column-gap", "column-gap"}, {"grid-gap", "gap"}, {"grid-row-gap", "row-gap"},
    {"inline-size", "width"}, {"inset-block-end", "bottom"}, {"inset-block-start", "top"},
    {"inset-inline-end", "right"}, {"inset-inline-start", "left"}, {"margin-block-end", "margin-bottom"},
    {"margin-block-start", "margin-top"}, {"margin-inline-end", "margin-right"},
    {"margin-inline-start", "margin-left"}, {"max-block-size", "max-height"}, {"max-inline-size", "max-width"},
    {"min-block-size", "min-height"}, {"min-inline-size", "min-width"}, {"padding-block-end", "padding-bottom"},
    {"padding-block-start", "padding-top"}, {"padding-inline-end", "padding-right"},
    {"padding-inline-start", "padding-left"}, {"word-wrap", ""}, {"-webkit-box-orient", ""},
};

static bool props_sorted;
static style_t initial;

static int cmp_prop(const void *a, const void *b) {
    return strcmp(((const struct propdef *)a)->name, ((const struct propdef *)b)->name);
}

static void init_initial(void) {
    style_t *s = &initial;
    s->display = D_INLINE;
    s->color = 0xFF000000u;
    for (int i = 0; i < 4; i++) {
        s->border_color[i] = COLOR_CURRENT;
        s->border_width[i] = 3;
        s->inset[i].kind = LK_AUTO;
        s->margin[i].kind = LK_LEN;
        s->padding[i].kind = LK_LEN;
    }
    s->font_size = 16;
    s->font_weight = 400;
    s->line_height.kind = LK_NORMAL;
    s->width.kind = s->height.kind = LK_AUTO;
    s->min_width.kind = s->min_height.kind = LK_AUTO;
    s->max_width.kind = s->max_height.kind = LK_NONE;
    s->text_indent.kind = LK_LEN;
    s->flex_basis.kind = LK_AUTO;
    s->flex_shrink = 1;
    s->opacity = 1;
    s->align_self = 255;
    s->justify_self = 255;
    s->z_auto = true;
}

static void props_init(void) {
    if (props_sorted) return;
    qsort(props, NPROPS, sizeof *props, cmp_prop);
    init_initial();
    props_sorted = true;
}

static const struct propdef *find_prop(const char *name, size_t n) {
    int lo = 0, hi = NPROPS - 1;
    while (lo <= hi) {
        int mid = (lo + hi) / 2;
        int c = strncmp(name, props[mid].name, n);
        if (!c) c = props[mid].name[n] ? -1 : 0;
        if (!c) return &props[mid];
        if (c < 0) hi = mid - 1;
        else lo = mid + 1;
    }
    return NULL;
}

const struct propdef *css_prop_lookup(const char *name, size_t n) {
    props_init();
    char low[64];
    if (n >= sizeof low) return NULL;
    for (size_t i = 0; i < n; i++) low[i] = (char)lower((unsigned char)name[i]);
    low[n] = 0;
    const struct propdef *p = find_prop(low, n);
    if (p) return p;
    for (size_t i = 0; i < sizeof aliases / sizeof *aliases; i++)
        if (!strcmp(aliases[i].alias, low)) return aliases[i].name[0] ? find_prop(aliases[i].name, strlen(aliases[i].name)) : NULL;
    /* prefixed versions of standard properties */
    static const char *const pre[] = {"-webkit-", "-moz-", "-ms-"};
    for (int k = 0; k < 3; k++) {
        size_t pl = strlen(pre[k]);
        if (n > pl && !strncmp(low, pre[k], pl)) return css_prop_lookup(low + pl, n - pl);
    }
    return NULL;
}

int css_prop_count(void) { return NPROPS; }
int css_prop_index(const struct propdef *p) { return (int)(p - props); }
bool css_prop_is_font(const struct propdef *p) {
    return (p->type == PT_FONT_SIZE) || (p->type == PT_SHORT && p->sh == SH_FONT);
}

/* ---------------------------------------------------------------- longhand values */
static bool kw_find(const struct kw *k, const char *s, size_t n, uint8_t *out) {
    for (; k->name; k++)
        if (ident_is(s, n, k->name)) {
            *out = k->v;
            return true;
        }
    return false;
}

static bool parse_display(const char *s, size_t n, uint8_t *out) {
    const char *t[3];
    size_t tl[3];
    int k = split(s, n, t, tl, 3, ' ');
    if (k == 1) {
        static const struct kw kd[] = {
            {"none", D_NONE}, {"inline", D_INLINE}, {"block", D_BLOCK}, {"list-item", D_LIST_ITEM},
            {"inline-block", D_INLINE_BLOCK}, {"table", D_TABLE}, {"inline-table", D_INLINE_TABLE},
            {"table-row-group", D_TABLE_ROW_GROUP}, {"table-header-group", D_TABLE_HEADER_GROUP},
            {"table-footer-group", D_TABLE_FOOTER_GROUP}, {"table-row", D_TABLE_ROW}, {"table-cell", D_TABLE_CELL},
            {"table-column", D_TABLE_COLUMN}, {"table-column-group", D_TABLE_COLUMN_GROUP},
            {"table-caption", D_TABLE_CAPTION}, {"flex", D_FLEX}, {"inline-flex", D_INLINE_FLEX},
            {"grid", D_GRID}, {"inline-grid", D_INLINE_GRID}, {"contents", D_CONTENTS}, {"flow-root", D_FLOW_ROOT},
            {"run-in", D_BLOCK}, {"-webkit-box", D_BLOCK}, {"-moz-box", D_BLOCK}, {"-webkit-inline-box", D_INLINE_BLOCK},
            {"-webkit-flex", D_FLEX}, {"-ms-flexbox", D_FLEX}, {"-webkit-inline-flex", D_INLINE_FLEX},
            {"-ms-grid", D_GRID}, {"ruby", D_INLINE}, {"ruby-text", D_INLINE}, {"ruby-base", D_INLINE}, {0}};
        return kw_find(kd, t[0], tl[0], out);
    }
    /* two-value syntax: outer and inner display, and list-item */
    bool inl = false, block = false, li = false;
    int inner = -1;
    for (int i = 0; i < k; i++) {
        if (ident_is(t[i], tl[i], "inline")) inl = true;
        else if (ident_is(t[i], tl[i], "block")) block = true;
        else if (ident_is(t[i], tl[i], "list-item")) li = true;
        else if (ident_is(t[i], tl[i], "flow")) inner = 0;
        else if (ident_is(t[i], tl[i], "flow-root")) inner = 1;
        else if (ident_is(t[i], tl[i], "flex")) inner = 2;
        else if (ident_is(t[i], tl[i], "grid")) inner = 3;
        else if (ident_is(t[i], tl[i], "table")) inner = 4;
        else return false;
    }
    if (inl && block) return false;
    if (li) *out = D_LIST_ITEM;
    else if (inner == 2) *out = inl ? D_INLINE_FLEX : D_FLEX;
    else if (inner == 3) *out = inl ? D_INLINE_GRID : D_GRID;
    else if (inner == 4) *out = inl ? D_INLINE_TABLE : D_TABLE;
    else if (inner == 1) *out = inl ? D_INLINE_BLOCK : D_FLOW_ROOT;
    else *out = inl ? D_INLINE : D_BLOCK;
    return true;
}

static const float font_kw_px[] = {9, 10, 13, 16, 18, 24, 32, 48};

static bool parse_font_size(const char *s, size_t n, struct cx *cx, float *out) {
    static const char *const kws[] = {"xx-small", "x-small", "small", "medium", "large", "x-large", "xx-large", "xxx-large"};
    for (int i = 0; i < 8; i++)
        if (ident_is(s, n, kws[i])) {
            *out = font_kw_px[i];
            return true;
        }
    float pfs = cx->parent ? cx->parent->font_size : 16;
    if (ident_is(s, n, "smaller")) {
        *out = pfs / 1.2f;
        return true;
    }
    if (ident_is(s, n, "larger")) {
        *out = pfs * 1.2f;
        return true;
    }
    if (ident_is(s, n, "math")) {
        *out = pfs;
        return true;
    }
    len_t l;
    if (!parse_len(s, n, cx, &l, 0)) return false;
    float v = len_resolve(&l, pfs);
    if (v < 0) return false;
    *out = v;
    return true;
}

static bool parse_font_weight(const char *s, size_t n, struct cx *cx, uint16_t *out) {
    int pw = cx->parent ? cx->parent->font_weight : 400;
    if (ident_is(s, n, "normal")) *out = 400;
    else if (ident_is(s, n, "bold")) *out = 700;
    else if (ident_is(s, n, "bolder")) *out = pw < 350 ? 400 : pw < 550 ? 700 : 900;
    else if (ident_is(s, n, "lighter")) *out = pw < 550 ? 100 : pw < 750 ? 400 : 700;
    else {
        const char *p = s;
        float v;
        if (!parse_number(&p, s + n, &v) || p != s + n || v < 1 || v > 1000) return false;
        *out = (uint16_t)v;
    }
    return true;
}

static bool mono_family(const char *s, size_t n, bool *out) {
    const char *t[16];
    size_t tl[16];
    int k = split(s, n, t, tl, 16, ',');
    static const char *const mono[] = {"monospace", "ui-monospace", "courier", "courier new", "consolas",
                                       "menlo", "monaco", "sfmono-regular", "sf mono", "fira code", "fira mono",
                                       "source code pro", "dejavu sans mono", "liberation mono", "roboto mono",
                                       "jetbrains mono", "lucida console", "andale mono", "ubuntu mono",
                                       "cascadia code", "cascadia mono", "ibm plex mono", "noto sans mono",
                                       "inconsolata", "hack", "go mono", "lucida sans typewriter", NULL};
    static const char *const prop[] = {"sans-serif", "serif", "system-ui", "-apple-system", "blinkmacsystemfont",
                                       "ui-sans-serif", "ui-serif", "arial", "helvetica", "helvetica neue",
                                       "inter", "roboto", "segoe ui", "verdana", "georgia", "times",
                                       "times new roman", "tahoma", "cursive", "fantasy", "noto sans", "open sans",
                                       "ubuntu", "cantarell", "lato", NULL};
    for (int i = 0; i < k; i++) {
        const char *f = t[i];
        size_t fl = tl[i];
        if (fl >= 2 && (f[0] == '"' || f[0] == '\'')) f++, fl -= 2;
        for (int j = 0; mono[j]; j++)
            if (ident_is(f, fl, mono[j])) {
                *out = true;
                return true;
            }
        for (int j = 0; prop[j]; j++)
            if (ident_is(f, fl, prop[j])) {
                *out = false;
                return true;
            }
    }
    *out = false;
    return k > 0;
}

static bool parse_list_type(const char *s, size_t n, struct cx *cx, uint8_t *out, const char **str) {
    static const struct kw kl[] = {
        {"disc", LS_DISC}, {"circle", LS_CIRCLE}, {"square", LS_SQUARE}, {"decimal", LS_DECIMAL},
        {"decimal-leading-zero", LS_DECIMAL_LZ}, {"lower-alpha", LS_LOWER_ALPHA}, {"lower-latin", LS_LOWER_ALPHA},
        {"upper-alpha", LS_UPPER_ALPHA}, {"upper-latin", LS_UPPER_ALPHA}, {"lower-roman", LS_LOWER_ROMAN},
        {"upper-roman", LS_UPPER_ROMAN}, {"lower-greek", LS_LOWER_GREEK}, {"none", LS_NONE}, {0}};
    *str = NULL;
    if (kw_find(kl, s, n, out)) return true;
    if (n >= 2 && (s[0] == '"' || s[0] == '\'') && s[n - 1] == s[0]) {
        sbuf b = {0};
        css_unescape(&b, s + 1, n - 2);
        *str = ar_strndup(cx->a, b.p ? b.p : "", b.n);
        sb_free(&b);
        *out = LS_STRING;
        return true;
    }
    if (ident_is(s, n, "disclosure-open") || ident_is(s, n, "disclosure-closed")) {
        *str = ident_is(s, n, "disclosure-open") ? "\xe2\x96\xbe " : "\xe2\x96\xb8 ";
        *out = LS_STRING;
        return true;
    }
    for (size_t i = 0; i < n; i++)
        if (!isalnum((unsigned char)s[i]) && s[i] != '-') return false;
    *out = LS_DECIMAL; /* other counter styles: numbers will do */
    return true;
}

/* url(...) contents, unescaped */
static const char *parse_url(const char *s, size_t n, arena_t *a) {
    if (n < 5 || !strn_ieq(s, "url(", 4) || s[n - 1] != ')') {
        if (n >= 2 && (s[0] == '"' || s[0] == '\'') && s[n - 1] == s[0]) { /* @import "x" */
            sbuf b = {0};
            css_unescape(&b, s + 1, n - 2);
            char *r = ar_strndup(a, b.p ? b.p : "", b.n);
            sb_free(&b);
            return r;
        }
        return NULL;
    }
    s += 4, n -= 5;
    trim(&s, &n);
    if (n >= 2 && (s[0] == '"' || s[0] == '\'') && s[n - 1] == s[0]) s++, n -= 2;
    sbuf b = {0};
    css_unescape(&b, s, n);
    char *r = ar_strndup(a, b.p ? b.p : "", b.n);
    sb_free(&b);
    return r;
}

/* background-position: one or two values (keywords, lengths, percentages); the first layer only */
static bool parse_bg_pos(const char *s, size_t n, struct cx *cx, len_t *out) {
    const char *layer[8];
    size_t ll[8];
    if (split(s, n, layer, ll, 8, ',') < 1) return false;
    const char *t[4];
    size_t tl[4];
    int k = split(layer[0], ll[0], t, tl, 4, ' ');
    if (k < 1 || k > 4) return false;
    len_t v[2];
    memset(v, 0, sizeof v);
    v[0].kind = v[1].kind = LK_LEN;
    v[0].pct = v[1].pct = 50;
    bool have[2] = {false, false};
    int pending = -1; /* a length after a keyword offsets from that side */
    /* "center" takes the axis the other keywords leave free: center right, top center */
    bool hx = false, hy = false;
    for (int i = 0; i < k; i++) {
        hx |= ident_is(t[i], tl[i], "left") || ident_is(t[i], tl[i], "right");
        hy |= ident_is(t[i], tl[i], "top") || ident_is(t[i], tl[i], "bottom");
    }
    for (int i = 0; i < k; i++) {
        int axis = -1;
        float pct = -1;
        if (ident_is(t[i], tl[i], "left")) axis = 0, pct = 0;
        else if (ident_is(t[i], tl[i], "right")) axis = 0, pct = 100;
        else if (ident_is(t[i], tl[i], "top")) axis = 1, pct = 0;
        else if (ident_is(t[i], tl[i], "bottom")) axis = 1, pct = 100;
        else if (ident_is(t[i], tl[i], "center")) axis = -2, pct = 50;
        if (pct >= 0) {
            if (axis == -2) axis = hx && !hy ? 1 : hy && !hx ? 0 : !have[0] ? 0 : 1;
            if (have[axis]) axis = !axis;
            v[axis].px = 0;
            v[axis].pct = pct;
            have[axis] = true;
            pending = axis;
            continue;
        }
        len_t l;
        if (!parse_len(t[i], tl[i], cx, &l, LF_NEG)) return false;
        if (l.kind != LK_LEN) l.px = len_resolve(&l, 0), l.pct = 0, l.kind = LK_LEN;
        int a = pending >= 0 && k > 2 ? pending : (!have[0] ? 0 : 1);
        if (pending >= 0 && k > 2) {
            if (v[a].pct == 100) l.px = -l.px, l.pct = 100 - l.pct;
            else l.pct += v[a].pct;
        }
        v[a] = l;
        have[a] = true;
        pending = -1;
    }
    out[0] = v[0];
    out[1] = v[1];
    return true;
}

static bool parse_bg_size(const char *s, size_t n, struct cx *cx, len_t *size, uint8_t *kind) {
    const char *layer[8];
    size_t ll[8];
    if (split(s, n, layer, ll, 8, ',') < 1) return false;
    s = layer[0];
    n = ll[0];
    memset(size, 0, 2 * sizeof *size);
    if (ident_is(s, n, "cover")) *kind = BSZ_COVER;
    else if (ident_is(s, n, "contain")) *kind = BSZ_CONTAIN;
    else {
        const char *t[2];
        size_t tl[2];
        int k = split(s, n, t, tl, 2, ' ');
        if (k < 1) return false;
        for (int i = 0; i < k; i++)
            if (!parse_len(t[i], tl[i], cx, &size[i], LF_AUTO)) return false;
        *kind = size[0].kind == LK_AUTO && size[1].kind == LK_AUTO ? BSZ_AUTO : BSZ_LEN;
    }
    return true;
}

static bool parse_mask_image(const char *s, size_t n, struct cx *cx) {
    if (ident_is(s, n, "none")) {
        cx->s->mask_image = NULL;
        return true;
    }
    const char *t[8];
    size_t tl[8];
    int k = split(s, n, t, tl, 8, ',');
    for (int i = 0; i < k; i++) {
        const char *u = parse_url(t[i], tl[i], cx->a);
        if (u) {
            cx->s->mask_image = u;
            return true;
        }
    }
    return false;
}

/* linear-gradient(), radial-gradient() and their repeating- forms; NULL if it is not one */
static const struct gradient *parse_gradient(const char *s, size_t n, struct cx *cx) {
    size_t fl = 0;
    while (fl < n && s[fl] != '(') fl++;
    if (fl >= n || s[n - 1] != ')') return NULL;
    const char *name = s;
    size_t nl = fl;
    if (nl > 8 && strn_ieq(name, "-webkit-", 8)) name += 8, nl -= 8;
    struct gradient g;
    memset(&g, 0, sizeof g);
    g.angle = 180;
    if (nl > 10 && strn_ieq(name, "repeating-", 10)) {
        g.repeating = true;
        name += 10;
        nl -= 10;
    }
    if (ident_is(name, nl, "radial-gradient")) g.radial = true;
    else if (!ident_is(name, nl, "linear-gradient")) return NULL;
    g.at[0].kind = g.at[1].kind = LK_LEN;
    g.at[0].pct = g.at[1].pct = 50;
    const char *a[GRAD_MAX + 2];
    size_t al[GRAD_MAX + 2];
    int m = split(s + fl + 1, n - fl - 2, a, al, GRAD_MAX + 2, ',');
    for (int j = 0; j < m; j++) {
        const char *c[8];
        size_t cl[8];
        int q = split(a[j], al[j], c, cl, 8, ' ');
        uint32_t col;
        if (q >= 1 && css_color(c[0], cl[0], &col)) { /* a colour stop: colour [pos [pos]] */
            for (int z = 0; z < 2 && g.n < GRAD_MAX; z++) {
                if (z == 1 && q < 3) break;
                g.col[g.n] = col;
                g.pos[g.n].kind = LK_AUTO;
                if (q > 1 + z && !parse_len(c[1 + z], cl[1 + z], cx, &g.pos[g.n], 0)) g.pos[g.n].kind = LK_AUTO;
                g.n++;
            }
            continue;
        }
        if (q == 1 && g.n && j < m - 1) { /* a colour hint: ignored */
            len_t hint;
            if (parse_len(c[0], cl[0], cx, &hint, 0)) continue;
        }
        if (j) return NULL;
        /* the first argument: direction or shape */
        if (!g.radial) {
            float deg;
            if (q >= 2 && ident_is(c[0], cl[0], "to")) {
                for (int z = 1; z < q; z++) {
                    if (ident_is(c[z], cl[z], "left")) g.to_x = -1;
                    else if (ident_is(c[z], cl[z], "right")) g.to_x = 1;
                    else if (ident_is(c[z], cl[z], "top")) g.to_y = -1;
                    else if (ident_is(c[z], cl[z], "bottom")) g.to_y = 1;
                    else return NULL;
                }
                if (!g.to_x || !g.to_y) { /* a side is just an angle */
                    g.angle = g.to_y < 0 ? 0 : g.to_x > 0 ? 90 : g.to_y > 0 ? 180 : 270;
                    g.to_x = g.to_y = 0;
                }
            } else if (q == 1 && comp(c[0], cl[0], 0, &deg)) g.angle = deg;
            else return NULL;
            continue;
        }
        for (int z = 0; z < q; z++) {
            if (ident_is(c[z], cl[z], "circle")) g.circle = true;
            else if (ident_is(c[z], cl[z], "ellipse")) g.circle = false;
            else if (ident_is(c[z], cl[z], "closest-side")) g.rsize = RG_CLOSEST_SIDE;
            else if (ident_is(c[z], cl[z], "farthest-side")) g.rsize = RG_FARTHEST_SIDE;
            else if (ident_is(c[z], cl[z], "closest-corner")) g.rsize = RG_CLOSEST_CORNER;
            else if (ident_is(c[z], cl[z], "farthest-corner")) g.rsize = RG_FARTHEST_CORNER;
            else if (ident_is(c[z], cl[z], "at") && z + 1 < q) {
                const char *p0 = c[z + 1];
                if (!parse_bg_pos(p0, (size_t)(a[j] + al[j] - p0), cx, g.at)) return NULL;
                break;
            } else {
                len_t l; /* an explicit size: approximated by the default */
                if (!parse_len(c[z], cl[z], cx, &l, 0)) return NULL;
            }
        }
    }
    if (g.n < 1) return NULL;
    if (g.n == 1) { /* one stop: a solid colour */
        g.col[1] = g.col[0];
        g.pos[1].kind = LK_AUTO;
        g.n = 2;
    }
    struct gradient *r = ar_alloc(cx->a, sizeof *r);
    *r = g;
    return r;
}

static bool parse_bg_image(const char *s, size_t n, struct cx *cx) {
    style_t *st = cx->s;
    if (ident_is(s, n, "none")) {
        st->bg_image = NULL;
        st->has_grad = false;
        st->gradient = NULL;
        return true;
    }
    /* several layers: the first one that we can use */
    const char *t[8];
    size_t tl[8];
    int k = split(s, n, t, tl, 8, ',');
    for (int i = 0; i < k; i++) {
        const char *u = parse_url(t[i], tl[i], cx->a);
        if (u) {
            st->bg_image = u;
            return true;
        }
        const struct gradient *g = parse_gradient(t[i], tl[i], cx);
        if (g) {
            st->gradient = g;
            st->grad[0] = g->col[0];
            st->grad[1] = g->col[g->n - 1];
            st->has_grad = true;
            return true;
        }
    }
    return k > 0;
}

static bool parse_content(const char *s, size_t n, struct cx *cx) {
    if (ident_is(s, n, "none") || ident_is(s, n, "normal")) {
        cx->s->content = NULL;
        return true;
    }
    /* "text" / alt text after a slash is dropped */
    for (size_t i = 0, q = 0; i < n; i++) {
        if (q) {
            if (s[i] == '\\') i++;
            else if (s[i] == (char)q) q = 0;
        } else if (s[i] == '"' || s[i] == '\'') q = (unsigned char)s[i];
        else if (s[i] == '/') {
            n = i;
            break;
        }
    }
    const char *t[32];
    size_t tl[32];
    int k = split(s, n, t, tl, 32, ' ');
    sbuf b = {0};
    for (int i = 0; i < k; i++) {
        const char *v = t[i];
        size_t vn = tl[i];
        if (vn >= 2 && (v[0] == '"' || v[0] == '\'') && v[vn - 1] == v[0]) css_unescape(&b, v + 1, vn - 2);
        else if (ident_is(v, vn, "open-quote")) sb_puts(&b, "\xe2\x80\x9c");
        else if (ident_is(v, vn, "close-quote")) sb_puts(&b, "\xe2\x80\x9d");
        else if (ident_is(v, vn, "no-open-quote") || ident_is(v, vn, "no-close-quote")) {
        } else if (vn > 5 && strn_ieq(v, "attr(", 5) && v[vn - 1] == ')') {
            const char *an = v + 5;
            size_t anl = vn - 6;
            trim(&an, &anl);
            node_t *e = cx->node;
            if (e)
                for (int j = 0; j < e->nattrs; j++)
                    if (strn_ieq(an, e->attrs[j].name, anl)) sb_puts(&b, e->attrs[j].value);
        } else if (vn > 8 && (strn_ieq(v, "counter(", 8) || strn_ieq(v, "counters(", 9))) {
        } else if (vn > 4 && (strn_ieq(v, "url(", 4) || strn_ieq(v, "image-set(", 10) ||
                              strstr(v, "gradient("))) {
        } else {
            sb_free(&b);
            return false;
        }
    }
    cx->s->content = ar_strndup(cx->a, b.p ? b.p : "", b.n);
    sb_free(&b);
    return true;
}

/* copy a longhand's value (all the fields it sets) */
/* ---------------------------------------------------------------- grid values */
/* one breadth: a length, auto, min-content, max-content, or (for the max) <n>fr */
static bool parse_breadth(const char *s, size_t n, struct cx *cx, uint8_t *kind, len_t *l, bool allow_fr) {
    memset(l, 0, sizeof *l);
    if (ident_is(s, n, "auto")) return *kind = GT_AUTO, true;
    if (ident_is(s, n, "min-content")) return *kind = GT_MIN, true;
    if (ident_is(s, n, "max-content")) return *kind = GT_MAX, true;
    if (n > 2 && strn_ieq(s + n - 2, "fr", 2)) {
        const char *q = s;
        float x;
        if (!parse_number(&q, s + n - 2, &x) || q != s + n - 2 || x < 0) return false;
        if (!allow_fr) return false;
        *kind = GT_FR;
        l->kind = LK_LEN;
        l->px = x;
        return true;
    }
    if (!parse_len(s, n, cx, l, 0)) return false;
    *kind = GT_LEN;
    return true;
}

static bool parse_track(const char *s, size_t n, struct cx *cx, struct gtrack *t) {
    memset(t, 0, sizeof *t);
    size_t k = 0;
    while (k < n && s[k] != '(') k++;
    if (k < n && s[n - 1] == ')' && (ident_is(s, k, "minmax") || ident_is(s, k, "fit-content"))) {
        const char *a[2];
        size_t al[2];
        int m = split(s + k + 1, n - k - 2, a, al, 2, ',');
        if (ident_is(s, k, "fit-content")) {
            if (m != 1 || !parse_len(a[0], al[0], cx, &t->max, 0)) return false;
            t->min_kind = GT_AUTO;
            t->max_kind = GT_FIT;
            return true;
        }
        if (m != 2) return false;
        if (!parse_breadth(a[0], al[0], cx, &t->min_kind, &t->min, false)) return false;
        return parse_breadth(a[1], al[1], cx, &t->max_kind, &t->max, true);
    }
    if (!parse_breadth(s, n, cx, &t->max_kind, &t->max, true)) return false;
    if (t->max_kind == GT_FR) t->min_kind = GT_AUTO;
    else {
        t->min_kind = t->max_kind;
        t->min = t->max;
    }
    return true;
}

/* a track list into v (at most max tracks); [line names] are skipped. rep_at, rep_n mark an
   auto-fill/auto-fit repeat (only one is allowed). */
static int parse_track_list(const char *s, size_t n, struct cx *cx, struct gtrack *v, int max, int *rep_at,
                            int *rep_n, bool *fit) {
    const char *t[64];
    size_t tl[64];
    int k = split(s, n, t, tl, 64, ' '), cnt = 0;
    bool in_names = false;
    for (int i = 0; i < k; i++) {
        if (in_names || t[i][0] == '[') {
            in_names = t[i][tl[i] - 1] != ']';
            continue;
        }
        size_t f = 0;
        while (f < tl[i] && t[i][f] != '(') f++;
        if (f < tl[i] && ident_is(t[i], f, "repeat") && t[i][tl[i] - 1] == ')') {
            const char *a[2];
            size_t al[2];
            if (split(t[i] + f + 1, tl[i] - f - 2, a, al, 2, ',') != 2) return -1;
            struct gtrack inner[32];
            int m = rep_at ? parse_track_list(a[1], al[1], cx, inner, 32, NULL, NULL, NULL) : -1;
            if (m <= 0) return -1;
            int times;
            if (ident_is(a[0], al[0], "auto-fill") || ident_is(a[0], al[0], "auto-fit")) {
                if (*rep_n) return -1;
                *rep_at = cnt;
                *rep_n = m;
                *fit = ident_is(a[0], al[0], "auto-fit");
                times = 1;
            } else {
                times = atoi(a[0]);
                if (times < 1) return -1;
            }
            for (int r = 0; r < times; r++)
                for (int j = 0; j < m; j++) {
                    if (cnt >= max) return -1;
                    v[cnt++] = inner[j];
                }
            continue;
        }
        if (cnt >= max || !parse_track(t[i], tl[i], cx, &v[cnt])) return -1;
        cnt++;
    }
    return cnt;
}

static bool parse_gtemplate(const char *s, size_t n, struct cx *cx, const struct gtemplate **out) {
    if (ident_is(s, n, "none")) {
        *out = NULL;
        return true;
    }
    struct gtrack v[256];
    int ra = 0, rn = 0;
    bool fit = false;
    int k = parse_track_list(s, n, cx, v, 256, &ra, &rn, &fit);
    if (k <= 0) return false;
    struct gtemplate *g = ar_alloc(cx->a, sizeof *g);
    g->t = ar_alloc(cx->a, sizeof *g->t * (size_t)k);
    memcpy(g->t, v, sizeof *g->t * (size_t)k);
    g->n = k;
    g->rep_at = ra;
    g->rep_n = rn;
    g->rep_fit = fit;
    *out = g;
    return true;
}

static bool parse_gareas(const char *s, size_t n, struct cx *cx, const struct gareas **out) {
    if (ident_is(s, n, "none")) {
        *out = NULL;
        return true;
    }
    const char *t[64];
    size_t tl[64];
    int rows = split(s, n, t, tl, 64, ' ');
    if (rows < 1) return false;
    const char *names[64][32];
    int cols = 0;
    for (int r = 0; r < rows; r++) {
        if (tl[r] < 2 || (t[r][0] != '"' && t[r][0] != '\'') || t[r][tl[r] - 1] != t[r][0]) return false;
        const char *p = t[r] + 1, *e = t[r] + tl[r] - 1;
        int c = 0;
        while (p < e) {
            if (is_space((unsigned char)*p)) {
                p++;
                continue;
            }
            const char *st = p;
            if (*p == '.') {
                while (p < e && *p == '.') p++;
                if (c < 32) names[r][c] = NULL;
            } else {
                while (p < e && !is_space((unsigned char)*p) && *p != '.') p++;
                if (c < 32) names[r][c] = ar_strndup(cx->a, st, (size_t)(p - st));
            }
            c++;
        }
        if (c > 32 || c < 1 || (r && c != cols)) return false;
        cols = c;
    }
    struct gareas *g = ar_alloc(cx->a, sizeof *g);
    g->rows = rows;
    g->cols = cols;
    g->cell = ar_alloc(cx->a, sizeof *g->cell * (size_t)(rows * cols));
    for (int r = 0; r < rows; r++)
        for (int c = 0; c < cols; c++) g->cell[r * cols + c] = names[r][c];
    *out = g;
    return true;
}

static bool is_custom_ident(const char *s, size_t n) {
    if (!n || isdigit((unsigned char)s[0]) || s[0] == '+' || (s[0] == '-' && n > 1 && isdigit((unsigned char)s[1])))
        return false;
    return !ident_is(s, n, "auto") && !ident_is(s, n, "span");
}

static bool parse_gline(const char *s, size_t n, struct cx *cx, struct gline *g) {
    memset(g, 0, sizeof *g);
    if (ident_is(s, n, "auto")) return true;
    const char *t[3];
    size_t tl[3];
    int k = split(s, n, t, tl, 3, ' ');
    bool span = false;
    int num = 0;
    const char *name = NULL;
    size_t name_n = 0;
    for (int i = 0; i < k; i++) {
        if (ident_is(t[i], tl[i], "span")) span = true;
        else if (isdigit((unsigned char)t[i][0]) || t[i][0] == '-' || t[i][0] == '+') {
            const char *q = t[i];
            float x;
            if (!parse_number(&q, t[i] + tl[i], &x) || q != t[i] + tl[i] || x == 0) return false;
            num = (int)x;
        } else if (is_custom_ident(t[i], tl[i])) {
            name = t[i];
            name_n = tl[i];
        } else return false;
    }
    if (span) {
        g->kind = GL_SPAN;
        g->n = (int16_t)(num > 0 ? (num > 1000 ? 1000 : num) : 1);
        return num >= 0;
    }
    if (name) {
        g->kind = GL_NAME;
        g->name = ar_strndup(cx->a, name, name_n);
        g->n = (int16_t)(num ? num : 1);
        return true;
    }
    if (!num) return false;
    g->kind = GL_LINE;
    g->n = (int16_t)(num > 1000 ? 1000 : num < -1000 ? -1000 : num);
    return true;
}

static void copy_prop(const struct propdef *p, style_t *dst, const style_t *src) {
    memcpy((char *)dst + p->off, (const char *)src + p->off, p->size);
    switch (p->type) {
    case PT_VALIGN: dst->vertical_align_px = src->vertical_align_px; break;
    case PT_LIST_TYPE: dst->list_style_string = src->list_style_string; break;
    case PT_BG_IMAGE:
        dst->has_grad = src->has_grad;
        dst->gradient = src->gradient;
        dst->grad[0] = src->grad[0];
        dst->grad[1] = src->grad[1];
        break;
    case PT_ZINDEX: dst->z_auto = src->z_auto; break;
    case PT_BG_SIZE: dst->bg_size_kind = src->bg_size_kind; break;
    case PT_MASK_SIZE: dst->mask_size_kind = src->mask_size_kind; break;
    }
}

static bool apply_long(const struct propdef *p, const char *v, size_t n, struct cx *cx) {
    style_t *s = cx->s;
    char *field = (char *)s + p->off;
    const style_t *par = cx->parent ? cx->parent : &initial;
    if (ident_is(v, n, "inherit")) {
        copy_prop(p, s, par);
        return true;
    }
    if (ident_is(v, n, "initial")) {
        copy_prop(p, s, &initial);
        return true;
    }
    if (ident_is(v, n, "unset") || ident_is(v, n, "revert") || ident_is(v, n, "revert-layer")) {
        copy_prop(p, s, p->inherited ? par : &initial);
        return true;
    }
    switch (p->type) {
    case PT_KW: {
        uint8_t k;
        if (!kw_find(p->kws, v, n, &k)) return false;
        *(uint8_t *)field = k;
        return true;
    }
    case PT_OVERFLOW: {
        uint8_t k;
        if (!kw_find(p->kws, v, n, &k)) return false;
        if (k != OV_VISIBLE) *(uint8_t *)field = k;
        return true;
    }
    case PT_DISPLAY: return parse_display(v, n, (uint8_t *)field);
    case PT_LEN: {
        len_t l;
        if (!parse_len(v, n, cx, &l, p->flags)) return false;
        *(len_t *)field = l;
        return true;
    }
    case PT_PX: {
        uint8_t k;
        if (p->kws && kw_find(p->kws, v, n, &k)) {
            *(float *)field = k;
            return true;
        }
        const char *t[2];
        size_t tl[2];
        if (split(v, n, t, tl, 2, ' ') < 1) return false; /* border-spacing: the first value */
        len_t l;
        if (!parse_len(t[0], tl[0], cx, &l, p->flags)) return false;
        *(float *)field = len_resolve(&l, 0);
        return true;
    }
    case PT_RADIUS: {
        const char *t[4];
        size_t tl[4];
        char buf[128];
        size_t bn = n < sizeof buf - 1 ? n : sizeof buf - 1;
        memcpy(buf, v, bn);
        for (size_t i = 0; i < bn; i++)
            if (buf[i] == '/') buf[i] = ' ';
        if (split(buf, bn, t, tl, 4, ' ') < 1) return false;
        len_t l;
        if (!parse_len(t[0], tl[0], cx, &l, 0)) return false;
        /* a percentage is stored negated and resolved against the box at paint time */
        *(float *)field = l.kind == LK_LEN && l.pct ? -l.pct : len_resolve(&l, 0);
        return true;
    }
    case PT_COLOR: {
        uint32_t c;
        if (!css_color(v, n, &c)) return false;
        if (c == COLOR_CURRENT && p->off == offsetof(style_t, color)) c = par->color;
        *(uint32_t *)field = c;
        return true;
    }
    case PT_NUM: {
        const char *q = v;
        float x;
        if (!parse_number(&q, v + n, &x) || q != v + n || x < 0) return false;
        *(float *)field = x;
        return true;
    }
    case PT_OPACITY: {
        const char *q = v;
        float x;
        if (!parse_number(&q, v + n, &x)) return false;
        if (q < v + n && *q == '%') x /= 100, q++;
        if (q != v + n) return false;
        *(float *)field = x < 0 ? 0 : x > 1 ? 1 : x;
        return true;
    }
    case PT_INT: case PT_ZINDEX: {
        if (p->type == PT_ZINDEX && ident_is(v, n, "auto")) {
            s->z_index = 0;
            s->z_auto = true;
            return true;
        }
        const char *q = v;
        float x;
        if (!parse_number(&q, v + n, &x) || q != v + n) return false;
        *(int *)field = (int)x;
        if (p->type == PT_ZINDEX) s->z_auto = false;
        return true;
    }
    case PT_FONT_SIZE: return parse_font_size(v, n, cx, &s->font_size);
    case PT_FONT_WEIGHT: return parse_font_weight(v, n, cx, &s->font_weight);
    case PT_FONT_FAMILY: return mono_family(v, n, &s->monospace);
    case PT_LINE_HEIGHT: {
        len_t l;
        if (ident_is(v, n, "normal")) {
            s->line_height.kind = LK_NORMAL;
            return true;
        }
        const char *q = v;
        float x;
        if (parse_number(&q, v + n, &x) && q == v + n) {
            if (x < 0) return false;
            s->line_height.kind = LK_NUMBER;
            s->line_height.px = x;
            s->line_height.pct = 0;
            return true;
        }
        if (!parse_len(v, n, cx, &l, 0)) return false;
        s->line_height.kind = LK_LEN;
        s->line_height.px = len_resolve(&l, s->font_size);
        s->line_height.pct = 0;
        return true;
    }
    case PT_VALIGN: {
        uint8_t k;
        if (kw_find(p->kws, v, n, &k)) {
            s->vertical_align = k;
            return true;
        }
        len_t l;
        if (!parse_len(v, n, cx, &l, LF_NEG)) return false;
        s->vertical_align = VA_LEN;
        s->vertical_align_px = len_resolve(&l, s->font_size * 1.2f);
        return true;
    }
    case PT_CONTENT: return parse_content(v, n, cx);
    case PT_LIST_TYPE: return parse_list_type(v, n, cx, &s->list_style, &s->list_style_string);
    case PT_BG_IMAGE: return parse_bg_image(v, n, cx);
    case PT_BG_POS: return parse_bg_pos(v, n, cx, (len_t *)field);
    case PT_BG_SIZE: return parse_bg_size(v, n, cx, s->bg_size, &s->bg_size_kind);
    case PT_MASK_SIZE: return parse_bg_size(v, n, cx, s->mask_size, &s->mask_size_kind);
    case PT_MASK_IMAGE: return parse_mask_image(v, n, cx);
    case PT_GTEMPLATE: return parse_gtemplate(v, n, cx, (const struct gtemplate **)field);
    case PT_GAREAS: return parse_gareas(v, n, cx, &s->grid_areas);
    case PT_GLINE: return parse_gline(v, n, cx, (struct gline *)field);
    case PT_TEXT_DECO: {
        const char *t[4];
        size_t tl[4];
        int k = split(v, n, t, tl, 4, ' ');
        uint8_t d = 0;
        for (int i = 0; i < k; i++) {
            if (ident_is(t[i], tl[i], "underline")) d |= TD_UNDERLINE;
            else if (ident_is(t[i], tl[i], "overline")) d |= TD_OVERLINE;
            else if (ident_is(t[i], tl[i], "line-through")) d |= TD_LINE_THROUGH;
            else if (ident_is(t[i], tl[i], "none") || ident_is(t[i], tl[i], "blink")) {
            } else return false;
        }
        s->text_decoration = d;
        return true;
    }
    }
    return false;
}

/* ---------------------------------------------------------------- shorthands */
static bool set_long(const char *name, const char *v, size_t n, struct cx *cx) {
    const struct propdef *p = find_prop(name, strlen(name));
    if (!p) return false;
    int i = css_prop_index(p);
    if (cx->set[i]) return true;
    if (cx->font_pass && p->type != PT_FONT_SIZE) return true;
    if (!apply_long(p, v, n, cx)) return false;
    cx->set[i] = 1;
    return true;
}

static const char *const side_names[4] = {"top", "right", "bottom", "left"};

/* margin: 1-4 values for the four sides */
static bool set_box(const char *fmt, const char *v, size_t n, struct cx *cx, const int *sides, int nsides) {
    const char *t[4];
    size_t tl[4];
    int k = split(v, n, t, tl, 4, ' ');
    if (k < 1) return false;
    char name[64];
    if (nsides == 4) {
        static const int pick[5][4] = {{0}, {0, 0, 0, 0}, {0, 1, 0, 1}, {0, 1, 2, 1}, {0, 1, 2, 3}};
        bool ok = true;
        for (int i = 0; i < 4; i++) {
            snprintf(name, sizeof name, fmt, side_names[i]);
            ok &= set_long(name, t[pick[k][i]], tl[pick[k][i]], cx);
        }
        return ok;
    }
    /* logical pairs: start end */
    bool ok = true;
    for (int i = 0; i < nsides; i++) {
        int ti = k > 1 ? i : 0;
        snprintf(name, sizeof name, fmt, side_names[sides[i]]);
        ok &= set_long(name, t[ti], tl[ti], cx);
    }
    return ok;
}

static bool is_bwidth(const char *s, size_t n, struct cx *cx) {
    uint8_t k;
    len_t l;
    return kw_find(kw_bwidth, s, n, &k) || parse_len(s, n, cx, &l, LF_NOPCT);
}

/* border: width style color, for the given sides */
static bool set_border(const char *v, size_t n, struct cx *cx, const int *sides, int nsides) {
    const char *t[3];
    size_t tl[3];
    int k = split(v, n, t, tl, 3, ' ');
    const char *w = "medium", *st = "none", *c = "currentcolor";
    size_t wl = 6, sl = 4, cl = 12;
    uint8_t dummy;
    uint32_t col;
    for (int i = 0; i < k; i++) {
        if (kw_find(kw_bstyle, t[i], tl[i], &dummy)) st = t[i], sl = tl[i];
        else if (is_bwidth(t[i], tl[i], cx)) w = t[i], wl = tl[i];
        else if (css_color(t[i], tl[i], &col)) c = t[i], cl = tl[i];
        else return false;
    }
    char name[64];
    for (int i = 0; i < nsides; i++) {
        const char *sd = side_names[sides[i]];
        snprintf(name, sizeof name, "border-%s-width", sd);
        set_long(name, w, wl, cx);
        snprintf(name, sizeof name, "border-%s-style", sd);
        set_long(name, st, sl, cx);
        snprintf(name, sizeof name, "border-%s-color", sd);
        set_long(name, c, cl, cx);
    }
    return true;
}

static bool set_font(const char *v, size_t n, struct cx *cx) {
    const char *t[16];
    size_t tl[16];
    int k = split(v, n, t, tl, 16, ' ');
    static const char *const sys[] = {"caption", "icon", "menu", "message-box", "small-caption", "status-bar",
                                      "-webkit-small-control", "-apple-system-body", NULL};
    if (k == 1)
        for (int i = 0; sys[i]; i++)
            if (ident_is(t[0], tl[0], sys[i])) {
                set_long("font-size", "13px", 4, cx);
                set_long("font-weight", "normal", 6, cx);
                set_long("font-style", "normal", 6, cx);
                set_long("line-height", "normal", 6, cx);
                set_long("font-family", "sans-serif", 10, cx);
                return true;
            }
    const char *style = "normal", *weight = "normal";
    size_t stl = 6, wl = 6;
    int i = 0;
    for (; i < k; i++) {
        uint8_t d;
        uint16_t w;
        if (kw_find(kw_fstyle, t[i], tl[i], &d) && !ident_is(t[i], tl[i], "normal")) style = t[i], stl = tl[i];
        else if (ident_is(t[i], tl[i], "normal") || ident_is(t[i], tl[i], "small-caps")) {
        } else if (!isdigit((unsigned char)t[i][0]) && parse_font_weight(t[i], tl[i], cx, &w)) weight = t[i], wl = tl[i];
        else if (isdigit((unsigned char)t[i][0]) && parse_font_weight(t[i], tl[i], cx, &w) && i + 1 < k)
            weight = t[i], wl = tl[i];
        else if (ident_is(t[i], tl[i], "condensed") || ident_is(t[i], tl[i], "expanded") ||
                 ident_is(t[i], tl[i], "semi-condensed") || ident_is(t[i], tl[i], "semi-expanded")) {
        } else break;
    }
    if (i >= k) return false;
    /* size[/line-height] */
    const char *sz = t[i];
    size_t szl = tl[i];
    const char *lh = "normal";
    size_t lhl = 6;
    const char *slash = memchr(sz, '/', szl);
    int fam = i + 1;
    if (slash) {
        lh = slash + 1;
        lhl = (size_t)(sz + szl - lh);
        szl = (size_t)(slash - sz);
        if (!lhl && fam < k) lh = t[fam], lhl = tl[fam], fam++;
    } else if (fam < k && t[fam][0] == '/') {
        if (tl[fam] > 1) lh = t[fam] + 1, lhl = tl[fam] - 1, fam++;
        else if (fam + 1 < k) lh = t[fam + 1], lhl = tl[fam + 1], fam += 2;
    }
    float fs;
    if (!parse_font_size(sz, szl, cx, &fs)) return false;
    if (fam >= k) return false;
    set_long("font-size", sz, szl, cx);
    if (cx->font_pass) return true;
    set_long("font-style", style, stl, cx);
    set_long("font-weight", weight, wl, cx);
    set_long("line-height", lh, lhl, cx);
    set_long("font-family", t[fam], (size_t)(v + n - t[fam]), cx);
    return true;
}

static bool set_background(const char *v, size_t n, struct cx *cx) {
    const char *layers[8];
    size_t ll[8];
    int k = split(v, n, layers, ll, 8, ',');
    if (k < 1) return false;
    /* the colour may only be in the last layer */
    const char *t[12];
    size_t tl[12];
    int m = split(layers[k - 1], ll[k - 1], t, tl, 12, ' ');
    const char *col = "transparent";
    size_t cl = 11;
    uint32_t c;
    for (int i = 0; i < m; i++)
        if (css_color(t[i], tl[i], &c)) col = t[i], cl = tl[i];
    set_long("background-color", col, cl, cx);
    /* images: any url() or gradient in any layer */
    bool img = false;
    for (int L = 0; L < k && !img; L++) {
        m = split(layers[L], ll[L], t, tl, 12, ' ');
        for (int i = 0; i < m; i++)
            if (strn_ieq(t[i], "url(", 4) || memchr(t[i], '(', tl[i])) {
                if (strn_ieq(t[i], "url(", 4) || strstr(t[i], "gradient(")) {
                    size_t len = tl[i];
                    const char *e = t[i];
                    /* the token ends at its closing parenthesis */
                    set_long("background-image", e, len, cx);
                    img = true;
                    break;
                }
            }
    }
    if (!img) set_long("background-image", "none", 4, cx);
    /* repeat, position and size of the first layer */
    m = split(layers[0], ll[0], t, tl, 12, ' ');
    const char *rep = "repeat";
    size_t rl = 6;
    char pos[256], size[128];
    size_t pn = 0, zn = 0;
    bool after_slash = false;
    for (int i = 0; i < m; i++) {
        uint8_t kv;
        if (kw_find(kw_bg_repeat, t[i], tl[i], &kv)) {
            rep = t[i], rl = tl[i];
            continue;
        }
        const char *tok = t[i];
        size_t tn = tl[i];
        if (strn_ieq(tok, "url(", 4) || memchr(tok, '(', tn) || css_color(tok, tn, &c)) continue;
        if (ident_is(tok, tn, "fixed") || ident_is(tok, tn, "scroll") || ident_is(tok, tn, "local") ||
            ident_is(tok, tn, "border-box") || ident_is(tok, tn, "padding-box") || ident_is(tok, tn, "content-box") ||
            ident_is(tok, tn, "text") || ident_is(tok, tn, "none"))
            continue;
        /* "pos/size" may be written with or without spaces around the slash */
        const char *slash = memchr(tok, '/', tn);
        if (slash) {
            size_t a = (size_t)(slash - tok);
            if (a && pn + a + 1 < sizeof pos) memcpy(pos + pn, tok, a), pn += a, pos[pn++] = ' ';
            size_t b = tn - a - 1;
            if (b && zn + b + 1 < sizeof size) memcpy(size + zn, slash + 1, b), zn += b, size[zn++] = ' ';
            after_slash = true;
            continue;
        }
        if (after_slash) {
            if (zn + tn + 1 < sizeof size) memcpy(size + zn, tok, tn), zn += tn, size[zn++] = ' ';
        } else if (pn + tn + 1 < sizeof pos) memcpy(pos + pn, tok, tn), pn += tn, pos[pn++] = ' ';
    }
    set_long("background-repeat", rep, rl, cx);
    if (pn) set_long("background-position", pos, pn - 1, cx);
    else set_long("background-position", "0% 0%", 5, cx);
    if (zn) set_long("background-size", size, zn - 1, cx);
    else set_long("background-size", "auto", 4, cx);
    return true;
}

static bool set_flex(const char *v, size_t n, struct cx *cx) {
    const char *t[3];
    size_t tl[3];
    int k = split(v, n, t, tl, 3, ' ');
    if (k == 1 && ident_is(t[0], tl[0], "none"))
        return set_long("flex-grow", "0", 1, cx), set_long("flex-shrink", "0", 1, cx),
               set_long("flex-basis", "auto", 4, cx);
    if (k == 1 && ident_is(t[0], tl[0], "auto"))
        return set_long("flex-grow", "1", 1, cx), set_long("flex-shrink", "1", 1, cx),
               set_long("flex-basis", "auto", 4, cx);
    const char *g = "1", *sh = "1", *b = "0%";
    size_t gl = 1, shl = 1, bl = 2;
    int nums = 0;
    for (int i = 0; i < k; i++) {
        const char *q = t[i];
        float x;
        if (parse_number(&q, t[i] + tl[i], &x) && q == t[i] + tl[i]) {
            if (nums == 0) g = t[i], gl = tl[i];
            else if (nums == 1) sh = t[i], shl = tl[i];
            else return false;
            nums++;
        } else b = t[i], bl = tl[i];
    }
    if (!nums) g = "1", gl = 1;
    set_long("flex-grow", g, gl, cx);
    set_long("flex-shrink", sh, shl, cx);
    set_long("flex-basis", b, bl, cx);
    return true;
}

static bool apply_short(const struct propdef *p, const char *v, size_t n, struct cx *cx) {
    static const int all[4] = {0, 1, 2, 3}, lr[2] = {3, 1}, tb[2] = {0, 2};
    bool global = ident_is(v, n, "inherit") || ident_is(v, n, "initial") || ident_is(v, n, "unset") ||
                  ident_is(v, n, "revert") || ident_is(v, n, "revert-layer");
    switch (p->sh) {
    case SH_MARGIN: return set_box("margin-%s", v, n, cx, all, 4);
    case SH_PADDING: return set_box("padding-%s", v, n, cx, all, 4);
    case SH_INSET: {
        static const char *const nm[4] = {"top", "right", "bottom", "left"};
        const char *t[4];
        size_t tl[4];
        int k = split(v, n, t, tl, 4, ' ');
        if (k < 1) return false;
        static const int pick[5][4] = {{0}, {0, 0, 0, 0}, {0, 1, 0, 1}, {0, 1, 2, 1}, {0, 1, 2, 3}};
        for (int i = 0; i < 4; i++) set_long(nm[i], t[pick[k][i]], tl[pick[k][i]], cx);
        return true;
    }
    case SH_MARGIN_INLINE: return set_box("margin-%s", v, n, cx, lr, 2);
    case SH_MARGIN_BLOCK: return set_box("margin-%s", v, n, cx, tb, 2);
    case SH_PADDING_INLINE: return set_box("padding-%s", v, n, cx, lr, 2);
    case SH_PADDING_BLOCK: return set_box("padding-%s", v, n, cx, tb, 2);
    case SH_INSET_INLINE: case SH_INSET_BLOCK: {
        const char *t[2];
        size_t tl[2];
        int k = split(v, n, t, tl, 2, ' ');
        if (k < 1) return false;
        const char *a = p->sh == SH_INSET_INLINE ? "left" : "top", *b = p->sh == SH_INSET_INLINE ? "right" : "bottom";
        set_long(a, t[0], tl[0], cx);
        set_long(b, t[k - 1], tl[k - 1], cx);
        return true;
    }
    case SH_BORDER: case SH_BORDER_TOP: case SH_BORDER_RIGHT: case SH_BORDER_BOTTOM: case SH_BORDER_LEFT:
    case SH_BORDER_INLINE: case SH_BORDER_BLOCK: {
        int side = p->sh - SH_BORDER_TOP;
        const int *sides = p->sh == SH_BORDER ? all : p->sh == SH_BORDER_INLINE ? lr : p->sh == SH_BORDER_BLOCK ? tb : &all[side];
        int ns = p->sh == SH_BORDER ? 4 : (p->sh == SH_BORDER_INLINE || p->sh == SH_BORDER_BLOCK) ? 2 : 1;
        if (global) {
            char name[64];
            for (int i = 0; i < ns; i++) {
                static const char *const f[3] = {"width", "style", "color"};
                for (int j = 0; j < 3; j++) {
                    snprintf(name, sizeof name, "border-%s-%s", side_names[sides[i]], f[j]);
                    set_long(name, v, n, cx);
                }
            }
            return true;
        }
        return set_border(v, n, cx, sides, ns);
    }
    case SH_BORDER_WIDTH: return set_box("border-%s-width", v, n, cx, all, 4);
    case SH_BORDER_STYLE: return set_box("border-%s-style", v, n, cx, all, 4);
    case SH_BORDER_COLOR: return set_box("border-%s-color", v, n, cx, all, 4);
    case SH_LIST_STYLE: {
        if (global) {
            set_long("list-style-type", v, n, cx);
            set_long("list-style-position", v, n, cx);
            return true;
        }
        const char *t[3];
        size_t tl[3];
        int k = split(v, n, t, tl, 3, ' ');
        const char *type = "disc", *pos = "outside";
        size_t typel = 4, posl = 7;
        bool none = false;
        for (int i = 0; i < k; i++) {
            uint8_t d;
            if (kw_find(kw_lspos, t[i], tl[i], &d)) pos = t[i], posl = tl[i];
            else if (ident_is(t[i], tl[i], "none")) none = true;
            else if (strn_ieq(t[i], "url(", 4)) {
            } else type = t[i], typel = tl[i];
        }
        if (none && typel == 4 && !strncmp(type, "disc", 4)) type = "none";
        set_long("list-style-type", type, typel, cx);
        set_long("list-style-position", pos, posl, cx);
        return true;
    }
    case SH_FONT:
        if (global) {
            set_long("font-size", v, n, cx);
            if (!cx->font_pass) {
                set_long("font-style", v, n, cx);
                set_long("font-weight", v, n, cx);
                set_long("line-height", v, n, cx);
                set_long("font-family", v, n, cx);
            }
            return true;
        }
        return set_font(v, n, cx);
    case SH_BACKGROUND:
        if (global) {
            set_long("background-color", v, n, cx);
            set_long("background-image", v, n, cx);
            set_long("background-repeat", v, n, cx);
            set_long("background-position", v, n, cx);
            set_long("background-size", v, n, cx);
            return true;
        }
        if (ident_is(v, n, "none")) {
            set_long("background-color", "transparent", 11, cx);
            set_long("background-image", "none", 4, cx);
            set_long("background-repeat", "repeat", 6, cx);
            set_long("background-position", "0% 0%", 5, cx);
            set_long("background-size", "auto", 4, cx);
            return true;
        }
        return set_background(v, n, cx);
    case SH_FLEX:
        if (global) {
            set_long("flex-grow", v, n, cx);
            set_long("flex-shrink", v, n, cx);
            set_long("flex-basis", v, n, cx);
            return true;
        }
        return set_flex(v, n, cx);
    case SH_FLEX_FLOW: {
        const char *t[2];
        size_t tl[2];
        int k = split(v, n, t, tl, 2, ' ');
        for (int i = 0; i < k; i++) {
            uint8_t d;
            if (kw_find(kw_fdir, t[i], tl[i], &d)) set_long("flex-direction", t[i], tl[i], cx);
            else if (kw_find(kw_fwrap, t[i], tl[i], &d)) set_long("flex-wrap", t[i], tl[i], cx);
            else if (global) set_long("flex-direction", v, n, cx), set_long("flex-wrap", v, n, cx);
            else return false;
        }
        return true;
    }
    case SH_GAP: {
        const char *t[2];
        size_t tl[2];
        int k = split(v, n, t, tl, 2, ' ');
        if (k < 1) return false;
        set_long("row-gap", t[0], tl[0], cx);
        set_long("column-gap", t[k - 1], tl[k - 1], cx);
        return true;
    }
    case SH_OVERFLOW: {
        const char *t[2];
        size_t tl[2];
        int k = split(v, n, t, tl, 2, ' ');
        if (k < 1) return false;
        set_long("overflow-x", t[0], tl[0], cx);
        set_long("overflow-y", t[k - 1], tl[k - 1], cx);
        return true;
    }
    case SH_TEXT_DECORATION: {
        if (global) return set_long("text-decoration-line", v, n, cx);
        const char *t[6];
        size_t tl[6];
        int k = split(v, n, t, tl, 6, ' ');
        char line[64] = "none";
        size_t ln = 4;
        for (int i = 0; i < k; i++)
            if (ident_is(t[i], tl[i], "underline") || ident_is(t[i], tl[i], "overline") ||
                ident_is(t[i], tl[i], "line-through")) {
                if (ln == 4 && !strcmp(line, "none")) ln = 0;
                if (ln + tl[i] + 1 < sizeof line) {
                    if (ln) line[ln++] = ' ';
                    memcpy(line + ln, t[i], tl[i]);
                    ln += tl[i];
                    line[ln] = 0;
                }
            }
        return set_long("text-decoration-line", line, ln, cx);
    }
    case SH_PLACE_SELF: case SH_PLACE_CONTENT: {
        const char *t[2];
        size_t tl[2];
        int k = split(v, n, t, tl, 2, ' ');
        if (k < 1) return false;
        if (p->sh == SH_PLACE_CONTENT) return set_long("justify-content", t[k - 1], tl[k - 1], cx);
        set_long("align-self", t[0], tl[0], cx);
        return set_long("justify-self", t[k - 1], tl[k - 1], cx);
    }
    case SH_GRID_AREA: case SH_GRID_ROW: case SH_GRID_COLUMN: {
        /* start / end lines: a name given alone stands for both edges of that area */
        static const char *const area[4] = {"grid-row-start", "grid-column-start", "grid-row-end", "grid-column-end"};
        static const char *const row[2] = {"grid-row-start", "grid-row-end"};
        static const char *const col[2] = {"grid-column-start", "grid-column-end"};
        const char *const *nm = p->sh == SH_GRID_AREA ? area : p->sh == SH_GRID_ROW ? row : col;
        int want = p->sh == SH_GRID_AREA ? 4 : 2;
        const char *t[4];
        size_t tl[4];
        int k = split(v, n, t, tl, 4, '/');
        if (k < 1 || k > want) return false;
        const char *s[4];
        size_t sl[4];
        for (int i = 0; i < want; i++) {
            s[i] = "auto", sl[i] = 4;
            if (i < k) s[i] = t[i], sl[i] = tl[i];
            else if (global) s[i] = v, sl[i] = n;
            else {
                int from = want == 4 && i == 3 ? 1 : 0;
                if (is_custom_ident(s[from], sl[from])) s[i] = s[from], sl[i] = sl[from];
            }
        }
        for (int i = 0; i < want; i++)
            if (!set_long(nm[i], s[i], sl[i], cx)) return false;
        return true;
    }
    case SH_GRID_TEMPLATE: case SH_GRID: {
        if (global || ident_is(v, n, "none")) {
            set_long("grid-template-rows", v, n, cx);
            set_long("grid-template-columns", v, n, cx);
            set_long("grid-template-areas", v, n, cx);
            return true;
        }
        const char *t[2];
        size_t tl[2];
        if (split(v, n, t, tl, 2, '/') != 2) return false;
        /* grid: auto-flow forms */
        if (p->sh == SH_GRID) {
            for (int side = 0; side < 2; side++) {
                const char *w[4];
                size_t wl[4];
                int k = split(t[side], tl[side], w, wl, 4, ' ');
                int a = -1;
                for (int i = 0; i < k; i++)
                    if (ident_is(w[i], wl[i], "auto-flow")) a = i;
                if (a < 0) continue;
                const char *rest = w[k - 1] + wl[k - 1];
                for (int i = 0; i < k; i++)
                    if (!ident_is(w[i], wl[i], "auto-flow") && !ident_is(w[i], wl[i], "dense")) {
                        rest = w[i];
                        break;
                    }
                size_t rn = (size_t)(t[side] + tl[side] - rest);
                set_long(side ? "grid-template-columns" : "grid-template-rows", "none", 4, cx);
                set_long("grid-template-areas", "none", 4, cx);
                set_long(side ? "grid-auto-columns" : "grid-auto-rows", rn ? rest : "auto", rn ? rn : 4, cx);
                set_long("grid-auto-flow", side ? "column" : "row", side ? 6 : 3, cx);
                return set_long(side ? "grid-template-rows" : "grid-template-columns", t[1 - side], tl[1 - side], cx);
            }
        }
        if (!memchr(t[0], '"', tl[0]) && !memchr(t[0], '\'', tl[0])) {
            set_long("grid-template-areas", "none", 4, cx);
            return set_long("grid-template-rows", t[0], tl[0], cx) &&
                   set_long("grid-template-columns", t[1], tl[1], cx);
        }
        /* "a b" size "c d" size ... / columns */
        char areas[1024], rows[1024];
        size_t an = 0, rn = 0;
        const char *w[64];
        size_t wl[64];
        int k = split(t[0], tl[0], w, wl, 64, ' ');
        bool pending = false; /* a string without its size yet */
        for (int i = 0; i < k; i++) {
            if (w[i][0] == '[') continue;
            if (w[i][0] == '"' || w[i][0] == '\'') {
                if (pending) {
                    if (rn + 6 > sizeof rows) return false;
                    memcpy(rows + rn, "auto ", 5), rn += 5;
                }
                if (an + wl[i] + 2 > sizeof areas) return false;
                memcpy(areas + an, w[i], wl[i]), an += wl[i];
                areas[an++] = ' ';
                pending = true;
            } else if (w[i][wl[i] - 1] != ']') {
                if (!pending || rn + wl[i] + 2 > sizeof rows) return false;
                memcpy(rows + rn, w[i], wl[i]), rn += wl[i];
                rows[rn++] = ' ';
                pending = false;
            }
        }
        if (pending) {
            if (rn + 6 > sizeof rows) return false;
            memcpy(rows + rn, "auto ", 5), rn += 5;
        }
        return set_long("grid-template-areas", areas, an, cx) && set_long("grid-template-rows", rows, rn, cx) &&
               set_long("grid-template-columns", t[1], tl[1], cx);
    }
    case SH_PLACE_ITEMS: {
        const char *t[2];
        size_t tl[2];
        int k = split(v, n, t, tl, 2, ' ');
        if (k < 1) return false;
        set_long("justify-items", t[k - 1], tl[k - 1], cx);
        return set_long("align-items", t[0], tl[0], cx);
    }
    }
    return false;
}

bool css_apply(const struct propdef *p, const char *v, size_t n, struct cx *cx) {
    trim(&v, &n);
    if (!n) return false;
    if (p->type == PT_SHORT) return apply_short(p, v, n, cx);
    int i = css_prop_index(p);
    if (cx->set[i]) return true;
    if (cx->font_pass && p->type != PT_FONT_SIZE) return true;
    if (!apply_long(p, v, n, cx)) return false;
    cx->set[i] = 1;
    return true;
}

void css_style_init(style_t *s, const style_t *parent) {
    props_init();
    if (!parent) {
        *s = initial;
        return;
    }
    *s = *parent;
    for (int i = 0; i < NPROPS; i++)
        if (props[i].type != PT_SHORT && !props[i].inherited) copy_prop(&props[i], s, &initial);
    s->before = s->after = NULL;
}

void css_style_finish(style_t *s, const style_t *parent, bool root) {
    if (s->color == COLOR_CURRENT) s->color = parent ? parent->color : 0xFF000000u;
    if (s->bg_color == COLOR_CURRENT) s->bg_color = s->color;
    for (int i = 0; i < 4; i++) {
        if (s->border_color[i] == COLOR_CURRENT) s->border_color[i] = s->color;
        if (s->border_style[i] == BS_NONE || s->border_style[i] == BS_HIDDEN) s->border_width[i] = 0;
    }
    if (s->has_grad) {
        for (int i = 0; i < 2; i++)
            if (s->grad[i] == COLOR_CURRENT) s->grad[i] = s->color;
    }
    if (s->position == POS_ABSOLUTE || s->position == POS_FIXED) s->float_ = FL_NONE;
    /* floated, absolutely positioned, root and flex items are blockified */
    bool blockify = root || s->float_ != FL_NONE || s->position == POS_ABSOLUTE || s->position == POS_FIXED;
    if (parent && (parent->display == D_FLEX || parent->display == D_INLINE_FLEX || parent->display == D_GRID ||
                   parent->display == D_INLINE_GRID))
        blockify = true;
    if (blockify) {
        switch (s->display) {
        case D_INLINE: case D_INLINE_BLOCK: case D_TABLE_ROW_GROUP: case D_TABLE_HEADER_GROUP:
        case D_TABLE_FOOTER_GROUP: case D_TABLE_ROW: case D_TABLE_CELL: case D_TABLE_CAPTION:
        case D_TABLE_COLUMN: case D_TABLE_COLUMN_GROUP:
            s->display = D_BLOCK;
            break;
        case D_INLINE_TABLE: s->display = D_TABLE; break;
        case D_INLINE_FLEX: s->display = D_FLEX; break;
        case D_INLINE_GRID: s->display = D_GRID; break;
        }
    }
    if (root && s->display == D_CONTENTS) s->display = D_BLOCK;
}

/* ---------------------------------------------------------------- the user agent stylesheet */
const char *css_ua_sheet(void) {
    return "html, address, blockquote, body, center, dialog, div, figure, figcaption, footer, form, header, hr, "
           "legend, listing, main, p, plaintext, pre, search, xmp, article, aside, h1, h2, h3, h4, h5, h6, hgroup, "
           "nav, section, dir, dd, dl, dt, menu, ol, ul, details, summary, fieldset, optgroup { display: block; }\n"
           "head, script, style, title, meta, link, base, template, datalist, param, noembed, noframes, area, map, "
           "track, source, rp, [hidden]:not([hidden=until-found]), input[type=hidden], dialog:not([open]) "
           "{ display: none; }\n"
           "li { display: list-item; }\n"
           "table { display: table; border-spacing: 2px; border-collapse: separate; box-sizing: border-box; "
           "text-indent: 0; }\n"
           "caption { display: table-caption; text-align: center; }\n"
           "colgroup { display: table-column-group; } col { display: table-column; }\n"
           "thead { display: table-header-group; vertical-align: middle; }\n"
           "tbody { display: table-row-group; vertical-align: middle; }\n"
           "tfoot { display: table-footer-group; vertical-align: middle; }\n"
           "tr { display: table-row; vertical-align: inherit; }\n"
           "td, th { display: table-cell; vertical-align: inherit; padding: 1px; }\n"
           "th { font-weight: bold; text-align: center; }\n"
           "body { margin: 8px; line-height: normal; }\n"
           "p, dl, multicol { margin: 1em 0; }\n"
           "dd { margin-left: 40px; }\n"
           "blockquote, figure { margin: 1em 40px; }\n"
           "address { font-style: italic; }\n"
           "center { text-align: -webkit-center; }\n"
           "h1 { font-size: 2em; margin: 0.67em 0; font-weight: bold; }\n"
           "h2 { font-size: 1.5em; margin: 0.83em 0; font-weight: bold; }\n"
           "h3 { font-size: 1.17em; margin: 1em 0; font-weight: bold; }\n"
           "h4 { margin: 1.33em 0; font-weight: bold; }\n"
           "h5 { font-size: 0.83em; margin: 1.67em 0; font-weight: bold; }\n"
           "h6 { font-size: 0.67em; margin: 2.33em 0; font-weight: bold; }\n"
           "article h1, aside h1, nav h1, section h1 { font-size: 1.5em; margin: 0.83em 0; }\n"
           "pre, listing, xmp, plaintext { white-space: pre; margin: 1em 0; font-family: monospace; }\n"
           "code, kbd, samp, tt, var { font-family: monospace; }\n"
           "textarea { white-space: pre-wrap; font-family: monospace; }\n"
           "ul, menu, dir { list-style-type: disc; margin: 1em 0; padding-left: 40px; }\n"
           "ol { list-style-type: decimal; margin: 1em 0; padding-left: 40px; }\n"
           "ul ul, ol ul, ul ol, ol ol, menu ul, ul menu { margin-top: 0; margin-bottom: 0; }\n"
           "ul ul, ol ul, menu ul { list-style-type: circle; }\n"
           "ul ul ul, ul ol ul, ol ul ul, ol ol ul { list-style-type: square; }\n"
           "hr { color: gray; border-style: inset; border-width: 1px; margin: 0.5em auto; overflow: hidden; }\n"
           "b, strong { font-weight: bolder; }\n"
           "i, cite, em, var, dfn { font-style: italic; }\n"
           "big { font-size: larger; } small { font-size: smaller; }\n"
           "sub { vertical-align: sub; font-size: smaller; } sup { vertical-align: super; font-size: smaller; }\n"
           "u, ins { text-decoration: underline; } s, strike, del { text-decoration: line-through; }\n"
           "mark { background-color: yellow; color: black; }\n"
           "a:link { color: #0645ad; text-decoration: underline; }\n"
           "abbr[title] { text-decoration: underline; }\n"
           "q::before { content: open-quote; } q::after { content: close-quote; }\n"
           "img, video, canvas, iframe, embed, object, svg, input, select, textarea, button, meter, progress "
           "{ display: inline-block; }\n"
           "iframe { border: 2px inset; }\n"
           "video, audio, canvas, iframe, embed, object { background-color: #e8e8e8; }\n"
           "fieldset { margin: 0 2px; padding: 0.35em 0.75em 0.625em; border: 2px groove #c0c0c0; }\n"
           "legend { padding: 0 2px; }\n"
           "input, select, textarea, button { font-size: 13.33px; font-family: sans-serif; color: black; "
           "letter-spacing: normal; text-align: start; text-indent: 0; text-transform: none; line-height: normal; "
           "box-sizing: border-box; }\n"
           "input, textarea, select { background-color: white; border: 1px solid #767676; padding: 2px 3px; "
           "border-radius: 2px; }\n"
           "button, input[type=submit], input[type=button], input[type=reset] { background-color: #efefef; "
           "border: 1px solid #767676; padding: 2px 8px; border-radius: 3px; text-align: center; }\n"
           "input[type=checkbox], input[type=radio] { border: none; padding: 0; margin: 3px 4px; "
           "background-color: transparent; }\n"
           "input[type=image] { border: none; padding: 0; }\n"
           "select { padding-right: 18px; }\n"
           "option, optgroup { display: none; }\n"
           "summary { display: list-item; list-style: disclosure-closed inside; }\n"
           "details[open] > summary { list-style-type: disclosure-open; }\n"
           "details:not([open]) > :not(summary) { display: none; }\n"
           "ruby { display: ruby; } rt { font-size: 50%; }\n"
           "nobr { white-space: nowrap; }\n"
           "wbr { display: inline; }\n"
           "marquee { display: inline-block; }\n"
           "math { display: inline; }\n"
           "svg:not(:root) { overflow: hidden; }\n"
           "noscript { display: block; }\n";
}
