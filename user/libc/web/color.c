/* CSS colours (CSS Color 4 and 5): the one owner of the colour value model.

   A <color> is parsed into a value tree (struct css_color_value): absolute
   colours, currentcolor, system colours, colour functions with relative
   syntax and math, color-mix(), light-dark() and contrast-color(). Computing
   it against an element resolves lengths, the colour scheme and everything
   but currentcolor; the result is either an absolute colour (struct
   css_color: a space, float components, none components and alpha, the
   spec's model) or a smaller tree that still names currentcolor. The used
   value resolves currentcolor and is gamut mapped to sRGB for painting.

   Painting reads the 8-bit sRGB fields of style_t, as before. A legacy sRGB
   colour (hex, named, rgb(), hsl(), hwb(), system colours) is defined in
   8 bits and lives only there. A computed value the field cannot hold (a
   non-legacy or wide-gamut space, a none component, an expression of
   currentcolor) also lives in the style's colour slots, which are shared
   copy-on-write: one block for the inherited colour properties, one for the
   element's own. A page of legacy colours allocates nothing.

   Conversions, gamut mapping, interpolation and serialization follow the
   spec text and the matrices of its sample code (conversions.js). */
#include <stdio.h>
#include <math.h>
#include <ctype.h>
#include "webi.h"

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

/* ---------------------------------------------------------------- tokens */
enum { TK_EOF, TK_IDENT, TK_FUNC, TK_NUM, TK_HASH, TK_COMMA, TK_SLASH, TK_DELIM, TK_PAREN, TK_BAD };
struct tok {
    uint8_t kind;
    char delim;
    bool pct;
    const char *s, *e;   /* the whole token */
    const char *name;    /* TK_IDENT, TK_FUNC: the name; TK_HASH: after '#'; TK_NUM: the unit */
    size_t name_n;
    const char *in;      /* TK_FUNC, TK_PAREN: the inside of the parentheses */
    size_t in_n;
    double num;
    /* an identifier or function name written with escapes: its value (long ones match nothing) */
    bool escaped;
    uint8_t esc_n;
    char esc[32];
};
#define TK_NAME(t) ((t)->escaped ? (t)->esc : (t)->name)
#define TK_LEN(t) ((t)->escaped ? (size_t)(t)->esc_n : (t)->name_n)
struct lex { const char *p, *e; };

static void lx_space(struct lex *l) {
    for (;;) {
        while (l->p < l->e && is_space((unsigned char)*l->p)) l->p++;
        if (l->e - l->p >= 2 && l->p[0] == '/' && l->p[1] == '*') {
            const char *q = l->p + 2;
            while (q + 1 < l->e && !(q[0] == '*' && q[1] == '/')) q++;
            l->p = q + 1 < l->e ? q + 2 : l->e;
            continue;
        }
        return;
    }
}
static bool name_char(unsigned c) { return isalnum(c) || c == '-' || c == '_' || c >= 0x80; }
/* CSS Syntax 4.3.8: a backslash not before a newline starts an escape */
static bool escape_at(const char *p, const char *e) { return p + 1 < e && *p == '\\' && p[1] != '\n'; }
static bool ident_start(const char *p, const char *e) {
    unsigned c = (unsigned char)*p;
    if (isalpha(c) || c == '_' || c >= 0x80 || escape_at(p, e)) return true;
    if (c == '-' && p + 1 < e) {
        unsigned d = (unsigned char)p[1];
        return isalpha(d) || d == '_' || d == '-' || d >= 0x80 || escape_at(p + 1, e);
    }
    return false;
}
static const char *escape_end(const char *p, const char *e, uint32_t *cp) {
    p++;
    if (!isxdigit((unsigned char)*p)) { *cp = (unsigned char)*p; return p + 1; }
    uint32_t v = 0;
    for (int k = 0; k < 6 && p < e && isxdigit((unsigned char)*p); k++, p++) v = v * 16 + (uint32_t)web_hex_digit((unsigned char)*p);
    if (p < e && is_space((unsigned char)*p)) p++;
    *cp = !v || v > 0x10FFFF || (v >= 0xD800 && v <= 0xDFFF) ? 0xFFFD : v;
    return p;
}
static const char *name_end(const char *p, const char *e) {
    while (p < e) {
        if (escape_at(p, e)) { uint32_t cp; p = escape_end(p, e, &cp); }
        else if (name_char((unsigned char)*p)) p++;
        else break;
    }
    return p;
}
/* the value of a name with escapes */
static void name_unescape(struct tok *t) {
    if (!memchr(t->name, '\\', t->name_n)) return;
    const char *p = t->name, *e = t->name + t->name_n;
    size_t n = 0;
    t->escaped = true;
    while (p < e) {
        char bytes[4];
        int k = 1;
        if (escape_at(p, e)) {
            uint32_t cp;
            p = escape_end(p, e, &cp);
            k = utf8_put(bytes, cp);
        } else bytes[0] = *p++;
        if (n + (size_t)k >= sizeof t->esc) { t->esc_n = sizeof t->esc - 1; return; }
        memcpy(t->esc + n, bytes, (size_t)k);
        n += (size_t)k;
    }
    t->esc_n = (uint8_t)n;
}
static bool number_start(const char *p, const char *e) {
    if (p < e && (*p == '+' || *p == '-')) p++;
    if (p < e && *p >= '0' && *p <= '9') return true;
    return p + 1 < e && *p == '.' && p[1] >= '0' && p[1] <= '9';
}
/* the ')' that closes a block whose '(' is just before p, or NULL */
static const char *block_end(const char *p, const char *e) {
    size_t depth = 0;
    for (; p < e; p++) {
        char c = *p;
        if (c == '"' || c == '\'') {
            for (p++; p < e && *p != c; p++)
                if (*p == '\\' && p + 1 < e) p++;
            if (p == e) return NULL;
        } else if (c == '(') depth++;
        else if (c == ')') {
            if (!depth) return p;
            depth--;
        }
    }
    return NULL;
}
static void lx_next(struct lex *l, struct tok *t) {
    lx_space(l);
    memset(t, 0, sizeof *t);
    t->s = t->e = l->p;
    if (l->p >= l->e) return;
    const char *p = l->p, *e = l->e;
    if (number_start(p, e)) {
        const char *q = p;
        if (*q == '+' || *q == '-') q++;
        while (q < e && *q >= '0' && *q <= '9') q++;
        if (q + 1 < e && *q == '.' && q[1] >= '0' && q[1] <= '9')
            for (q++; q < e && *q >= '0' && *q <= '9'; q++) {}
        if (q + 1 < e && (*q | 32) == 'e' &&
            ((q[1] >= '0' && q[1] <= '9') || ((q[1] == '+' || q[1] == '-') && q + 2 < e && q[2] >= '0' && q[2] <= '9'))) {
            q += 2;
            while (q < e && *q >= '0' && *q <= '9') q++;
        }
        char buf[80];
        size_t k = (size_t)(q - p);
        if (k >= sizeof buf) { t->kind = TK_BAD; l->p = q; t->e = q; return; }
        memcpy(buf, p, k);
        buf[k] = 0;
        t->num = strtod(buf, NULL);
        t->kind = TK_NUM;
        if (q < e && *q == '%') { t->pct = true; t->name = q; t->name_n = 1; q++; }
        else if (q < e && ident_start(q, e)) { t->name = q; q = name_end(q, e); t->name_n = (size_t)(q - t->name); }
        l->p = t->e = q;
        return;
    }
    if (ident_start(p, e)) {
        const char *q = name_end(p, e);
        t->name = p;
        t->name_n = (size_t)(q - p);
        name_unescape(t);
        if (q < e && *q == '(') {
            const char *close = block_end(q + 1, e);
            if (!close) { t->kind = TK_BAD; l->p = t->e = e; return; }
            t->kind = TK_FUNC;
            t->in = q + 1;
            t->in_n = (size_t)(close - q - 1);
            l->p = t->e = close + 1;
            return;
        }
        t->kind = TK_IDENT;
        l->p = t->e = q;
        return;
    }
    char c = *p;
    l->p = t->e = p + 1;
    if (c == '#') {
        const char *q = name_end(p + 1, e);
        t->kind = q > p + 1 ? TK_HASH : TK_BAD;
        t->name = p + 1;
        t->name_n = (size_t)(q - p - 1);
        l->p = t->e = q;
    } else if (c == ',') t->kind = TK_COMMA;
    else if (c == '/') t->kind = TK_SLASH;
    else if (c == '+' || c == '-' || c == '*') { t->kind = TK_DELIM; t->delim = c; }
    else if (c == '(') {
        const char *close = block_end(p + 1, e);
        if (!close) { t->kind = TK_BAD; l->p = t->e = e; return; }
        t->kind = TK_PAREN;
        t->in = p + 1;
        t->in_n = (size_t)(close - p - 1);
        l->p = t->e = close + 1;
    } else t->kind = TK_BAD;
}
#define TOKS_MAX 48 /* tokens a list keeps on the stack; a longer one goes to the parse's arena */
struct pctx;
static void *p_alloc(struct pctx *P, size_t n);
/* every token of s: in buf when they fit, else in memory from P; NULL if one is bad */
static const struct tok *lx_all(struct pctx *P, const char *s, size_t n, struct tok *buf, int *count) {
    struct lex l = {s, s + n};
    int k = 0;
    for (;;) {
        struct tok t;
        lx_next(&l, &t);
        if (t.kind == TK_EOF) break;
        if (t.kind == TK_BAD) return NULL;
        if (k < TOKS_MAX) buf[k] = t;
        k++;
    }
    *count = k;
    if (k <= TOKS_MAX) return buf;
    struct tok *all = p_alloc(P, (size_t)k * sizeof *all);
    l = (struct lex){s, s + n};
    for (int i = 0; i < k; i++) lx_next(&l, &all[i]);
    return all;
}
static bool tok_ident(const struct tok *t, const char *kw) {
    return t->kind == TK_IDENT && strn_ieq(TK_NAME(t), kw, TK_LEN(t));
}
static bool name_is(const char *s, size_t n, const char *kw) { return strn_ieq(s, kw, n); }

/* ---------------------------------------------------------------- math functions (CSS Values 4, 10) */
enum { MT_NUM, MT_PCT, MT_ANGLE, MT_LEN };
enum { MN_LEAF, MN_KEY, MN_SUM, MN_PRODUCT, MN_NEGATE, MN_INVERT, MN_FN };
enum { MF_MIN, MF_MAX, MF_CLAMP, MF_ROUND, MF_MOD, MF_REM, MF_SIN, MF_COS, MF_TAN, MF_ASIN, MF_ACOS, MF_ATAN,
       MF_ATAN2, MF_POW, MF_SQRT, MF_HYPOT, MF_LOG, MF_EXP, MF_ABS, MF_SIGN, MF_COUNT };
static const char *const math_names[MF_COUNT] = {"min", "max", "clamp", "round", "mod", "rem", "sin", "cos", "tan",
    "asin", "acos", "atan", "atan2", "pow", "sqrt", "hypot", "log", "exp", "abs", "sign"};
enum { RS_NEAREST, RS_UP, RS_DOWN, RS_TO_ZERO };
static const char *const round_names[] = {"nearest", "up", "down", "to-zero"};
struct cm {
    uint8_t op, type, fn, n;
    uint8_t key;     /* MN_KEY: the channel 0..3; MF_ROUND: the strategy */
    char unit[7];    /* MN_LEAF: "" number, "%" percentage, else a lowercase unit */
    double v;
    struct cm **k;
};
static const struct cx unit_probe = {.em = 16, .rem = 16, .vw = 100, .vh = 100};
static const struct cx no_element = {.em = 16, .rem = 16, .vw = 800, .vh = 600};

static double angle_unit(const char *u, size_t n) {
    if (name_is(u, n, "deg")) return 1;
    if (name_is(u, n, "grad")) return 0.9;
    if (name_is(u, n, "rad")) return 180 / M_PI;
    if (name_is(u, n, "turn")) return 360;
    return 0;
}
static double absolute_length(const char *u) {
    static const struct { const char *u; double px; } units[] = {
        {"px", 1}, {"in", 96}, {"cm", 96 / 2.54}, {"mm", 96 / 25.4}, {"q", 96 / 101.6}, {"pt", 4.0 / 3}, {"pc", 16}};
    for (size_t i = 0; i < sizeof units / sizeof *units; i++)
        if (!strcmp(units[i].u, u)) return units[i].px;
    return 0;
}
/* a leaf's type from its unit; false for an unknown unit */
static bool unit_type(const char *u, size_t n, bool pct, uint8_t *type) {
    if (pct) { *type = MT_PCT; return true; }
    if (!n) { *type = MT_NUM; return true; }
    if (angle_unit(u, n)) { *type = MT_ANGLE; return true; }
    float px, p;
    bool number;
    if (n < 7 && css_unit_px(u, n, 1, &unit_probe, &px, &p, &number) && !number) { *type = MT_LEN; return true; }
    return false;
}
/* a leaf in its canonical unit: degrees, pixels (context-free lengths only), or as is */
static double leaf_value(const struct cm *m, const struct cx *cx, bool *resolved) {
    if (m->type == MT_ANGLE) return m->v * angle_unit(m->unit, strlen(m->unit));
    if (m->type != MT_LEN) return m->v;
    double abs = absolute_length(m->unit);
    if (abs) return m->v * abs;
    if (!cx) {
        /* no element (canvas, attributes): the initial font and viewport */
        if (resolved) *resolved = false;
        cx = &no_element;
    }
    float px, pct;
    bool number;
    if (!css_unit_px(m->unit, strlen(m->unit), (float)m->v, cx, &px, &pct, &number)) return 0;
    return px;
}

struct pctx {
    arena_t *a;
    int depth;
    unsigned nodes;
    const char *const *keys; /* relative colour channel keywords (4, the last is alpha), or NULL */
    bool in_math;
};
static void *p_alloc(struct pctx *P, size_t n) { return ar_alloc(P->a, n); }
static struct cm *m_new(struct pctx *P, uint8_t op, unsigned n) {
    if (++P->nodes > 4096) return NULL;
    struct cm *m = p_alloc(P, sizeof *m);
    m->op = op;
    m->n = (uint8_t)n;
    if (n) m->k = p_alloc(P, n * sizeof *m->k);
    return m;
}
static struct cm *m_leaf(struct pctx *P, double v, const char *unit, uint8_t type) {
    struct cm *m = m_new(P, MN_LEAF, 0);
    if (!m) return NULL;
    m->v = v;
    m->type = type;
    snprintf(m->unit, sizeof m->unit, "%s", unit);
    return m;
}
static bool m_numeric(const struct cm *m) { return m->op == MN_LEAF; }
/* a leaf whose value needs no element: not a font, viewport or container length */
static bool m_free(const struct cm *m) {
    return m->op == MN_LEAF && (m->type != MT_LEN || absolute_length(m->unit));
}
static bool m_same_unit(const struct cm *a, const struct cm *b) {
    if (a->type != b->type) return false;
    if (a->type == MT_LEN) return !strcmp(a->unit, b->unit) || (absolute_length(a->unit) && absolute_length(b->unit));
    return true; /* numbers, percentages, and angles in degrees */
}
static void m_canon(struct cm *m) {
    /* inside a math function, angles and absolute lengths are in their canonical unit */
    if (m->op != MN_LEAF) return;
    if (m->type == MT_ANGLE && strcmp(m->unit, "deg")) { m->v *= angle_unit(m->unit, strlen(m->unit)); strcpy(m->unit, "deg"); }
    else if (m->type == MT_LEN && absolute_length(m->unit) && strcmp(m->unit, "px")) {
        m->v *= absolute_length(m->unit);
        strcpy(m->unit, "px");
    }
}

static double m_eval(const struct cm *m, const double *ch, const struct cx *cx, bool *resolved);
static double round_to(double a, double b, int strategy) {
    if (!isfinite(a) || b == 0 || isnan(b)) return NAN;
    if (isinf(b)) {
        if (strategy == RS_UP) return a > 0 ? INFINITY : copysign(0, a);
        if (strategy == RS_DOWN) return a < 0 ? -INFINITY : copysign(0, a);
        return copysign(0, a);
    }
    double q = a / b, lo = floor(q) * b, hi = ceil(q) * b;
    if (lo > hi) { double t = lo; lo = hi; hi = t; }
    switch (strategy) {
    case RS_UP: return hi;
    case RS_DOWN: return lo;
    case RS_TO_ZERO: return fabs(lo) < fabs(hi) ? lo : hi;
    default: return a - lo < hi - a ? lo : hi;
    }
}
static double m_fn(const struct cm *m, const double *x) {
    double a = x[0], b = m->n > 1 ? x[1] : 0;
    switch (m->fn) {
    case MF_MIN: { double r = a; for (unsigned i = 1; i < m->n; i++) if (isnan(x[i]) || x[i] < r) r = x[i]; return r; }
    case MF_MAX: { double r = a; for (unsigned i = 1; i < m->n; i++) if (isnan(x[i]) || x[i] > r) r = x[i]; return r; }
    case MF_CLAMP: { double v = b < a ? a : b; return x[2] < v ? (x[2] > a ? x[2] : a) : v; }
    case MF_ROUND: return round_to(a, m->n > 1 ? b : 1, m->key);
    case MF_MOD: { if (b == 0 || isinf(a)) return NAN; if (isinf(b)) return (a < 0) == (b < 0) ? a : NAN;
        double r = fmod(a, b); if (r != 0 && (r < 0) != (b < 0)) r += b; return r; }
    case MF_REM: { if (b == 0 || isinf(a)) return NAN; if (isinf(b)) return a; return fmod(a, b); }
    case MF_SIN: return sin(m->k[0]->type == MT_ANGLE ? a * M_PI / 180 : a);
    case MF_COS: return cos(m->k[0]->type == MT_ANGLE ? a * M_PI / 180 : a);
    case MF_TAN: return tan(m->k[0]->type == MT_ANGLE ? a * M_PI / 180 : a);
    case MF_ASIN: return asin(a) * 180 / M_PI;
    case MF_ACOS: return acos(a) * 180 / M_PI;
    case MF_ATAN: return atan(a) * 180 / M_PI;
    case MF_ATAN2: return atan2(a, b) * 180 / M_PI;
    case MF_POW: return pow(a, b);
    case MF_SQRT: return sqrt(a);
    case MF_HYPOT: { double r = 0; for (unsigned i = 0; i < m->n; i++) r = hypot(r, x[i]); return r; }
    case MF_LOG: return m->n > 1 ? log(a) / log(b) : log(a);
    case MF_EXP: return exp(a);
    case MF_ABS: return fabs(a);
    case MF_SIGN: return a > 0 ? 1 : a < 0 ? -1 : a; /* keeps the sign of zero and NaN */
    }
    return NAN;
}
/* the value in its type's canonical unit; channel keywords read ch (none is 0 in math) */
static double m_eval(const struct cm *m, const double *ch, const struct cx *cx, bool *resolved) {
    switch (m->op) {
    case MN_LEAF: return leaf_value(m, cx, resolved);
    case MN_KEY: return ch ? ch[m->key] : 0;
    case MN_NEGATE: return -m_eval(m->k[0], ch, cx, resolved);
    case MN_INVERT: return 1 / m_eval(m->k[0], ch, cx, resolved);
    case MN_SUM: { double r = 0; for (unsigned i = 0; i < m->n; i++) r += m_eval(m->k[i], ch, cx, resolved); return r; }
    case MN_PRODUCT: { double r = 1; for (unsigned i = 0; i < m->n; i++) r *= m_eval(m->k[i], ch, cx, resolved); return r; }
    default: {
        double x[64];
        for (unsigned i = 0; i < m->n && i < 64; i++) x[i] = m_eval(m->k[i], ch, cx, resolved);
        return m_fn(m, x);
    }
    }
}
/* CSS Values 4, 10.10: simplify an operator node whose children are simplified */
static struct cm *m_simplify(struct pctx *P, struct cm *m) {
    if (!m) return NULL;
    if (m->op == MN_NEGATE) {
        struct cm *c = m->k[0];
        if (m_numeric(c)) { c->v = -c->v; return c; }
        if (c->op == MN_NEGATE) return c->k[0];
        return m;
    }
    if (m->op == MN_INVERT) {
        struct cm *c = m->k[0];
        if (m_numeric(c) && c->type == MT_NUM) { c->v = 1 / c->v; return c; }
        if (c->op == MN_INVERT) return c->k[0];
        return m;
    }
    if (m->op == MN_SUM || m->op == MN_PRODUCT) {
        /* flatten nested nodes of the same operator */
        unsigned count = 0;
        for (unsigned i = 0; i < m->n; i++) count += m->k[i]->op == m->op ? m->k[i]->n : 1;
        struct cm **k = p_alloc(P, (count ? count : 1) * sizeof *k);
        unsigned j = 0;
        for (unsigned i = 0; i < m->n; i++)
            if (m->k[i]->op == m->op) for (unsigned q = 0; q < m->k[i]->n; q++) k[j++] = m->k[i]->k[q];
            else k[j++] = m->k[i];
        if (count > 255) return NULL;
        m->k = k;
        m->n = (uint8_t)count;
        if (m->op == MN_SUM) {
            for (unsigned i = 0; i < m->n; i++) {
                if (!m_free(m->k[i])) continue;
                for (unsigned q = i + 1; q < m->n;) {
                    if (m_free(m->k[q]) && m_same_unit(m->k[i], m->k[q])) {
                        bool r = true;
                        m->k[i]->v = leaf_value(m->k[i], NULL, &r) + leaf_value(m->k[q], NULL, &r);
                        if (m->k[i]->type == MT_LEN) strcpy(m->k[i]->unit, "px");
                        if (m->k[i]->type == MT_ANGLE) strcpy(m->k[i]->unit, "deg");
                        memmove(&m->k[q], &m->k[q + 1], (m->n - q - 1) * sizeof *m->k);
                        m->n--;
                    } else q++;
                }
            }
            return m->n == 1 ? m->k[0] : m;
        }
        /* a product: multiply its numbers, then into one other numeric value */
        double number = 1;
        unsigned numbers = 0, w = 0;
        for (unsigned i = 0; i < m->n; i++) {
            struct cm *c = m->k[i];
            if (c->op == MN_LEAF && c->type == MT_NUM) { number *= c->v; numbers++; }
            else if (c->op == MN_INVERT && m_numeric(c->k[0]) && c->k[0]->type == MT_NUM) { number /= c->k[0]->v; numbers++; }
            else m->k[w++] = c;
        }
        m->n = (uint8_t)w;
        if (!w) return m_leaf(P, number, "", MT_NUM);
        if (w == 1 && m_numeric(m->k[0])) { m->k[0]->v *= number; return m->k[0]; }
        if (w == 1 && m->k[0]->op == MN_SUM) {
            bool all = true;
            for (unsigned i = 0; i < m->k[0]->n; i++) all &= m_numeric(m->k[0]->k[i]);
            if (all) { for (unsigned i = 0; i < m->k[0]->n; i++) m->k[0]->k[i]->v *= number; return m->k[0]; }
        }
        if (numbers && number != 1) {
            struct cm **k2 = p_alloc(P, (w + 1) * sizeof *k2);
            k2[0] = m_leaf(P, number, "", MT_NUM);
            if (!k2[0]) return NULL;
            memcpy(k2 + 1, m->k, w * sizeof *k2);
            m->k = k2;
            m->n = (uint8_t)(w + 1);
        } else if (w == 1) return m->k[0];
        return m;
    }
    if (m->op == MN_FN) {
        bool all = true;
        for (unsigned i = 0; i < m->n; i++) all &= m_free(m->k[i]);
        if (!all) return m;
        double x[64];
        bool r = true;
        for (unsigned i = 0; i < m->n && i < 64; i++) x[i] = leaf_value(m->k[i], NULL, &r);
        double v = m_fn(m, x);
        const char *unit = m->type == MT_PCT ? "%" : m->type == MT_ANGLE ? "deg" : m->type == MT_LEN ? "px" : "";
        return m_leaf(P, v, unit, m->type);
    }
    return m;
}

static struct cm *m_sum(struct pctx *P, const struct tok *t, int n);
/* one value of a calculation: a numeric literal, a keyword, a nested block or math function */
static struct cm *m_value(struct pctx *P, const struct tok *t);
static struct cm *m_function(struct pctx *P, const struct tok *t, bool *calc);
static struct cm **m_args(struct pctx *P, const char *s, size_t n, unsigned *count) {
    struct tok buf[TOKS_MAX];
    int k;
    const struct tok *t = lx_all(P, s, n, buf, &k);
    if (!t) return NULL;
    unsigned c = 1;
    for (int i = 0; i < k; i++) c += t[i].kind == TK_COMMA;
    struct cm **out = p_alloc(P, c * sizeof *out);
    c = 0;
    int start = 0;
    for (int i = 0; i <= k; i++) {
        if (i < k && t[i].kind != TK_COMMA) continue;
        if (i == start) return NULL;
        if (!(out[c++] = m_sum(P, t + start, i - start))) return NULL;
        start = i + 1;
    }
    *count = c;
    return out;
}
static struct cm *m_function(struct pctx *P, const struct tok *t, bool *calc) {
    if (++P->depth > 32) return NULL;
    struct cm *result = NULL;
    bool outer = P->in_math;
    P->in_math = true;
    *calc = false;
    if (name_is(TK_NAME(t), TK_LEN(t), "calc")) {
        struct tok buf[TOKS_MAX];
        int n;
        const struct tok *k = lx_all(P, t->in, t->in_n, buf, &n);
        if (k && n) result = m_sum(P, k, n);
        *calc = true;
        goto done;
    }
    int fn = -1;
    for (int i = 0; i < MF_COUNT; i++)
        if (name_is(TK_NAME(t), TK_LEN(t), math_names[i])) fn = i;
    if (fn < 0) goto done;
    struct cm **args;
    unsigned n = 0;
    int strategy = RS_NEAREST;
    const char *in = t->in;
    size_t in_n = t->in_n;
    if (fn == MF_ROUND) {
        struct lex l = {in, in + in_n};
        struct tok first, comma;
        lx_next(&l, &first);
        if (first.kind == TK_IDENT)
            for (int i = 0; i < 4; i++)
                if (tok_ident(&first, round_names[i])) {
                    lx_next(&l, &comma);
                    if (comma.kind != TK_COMMA) goto done;
                    strategy = i;
                    in = l.p;
                    in_n = (size_t)(t->in + t->in_n - l.p);
                }
    }
    if (!(args = m_args(P, in, in_n, &n))) goto done;
    static const unsigned min_args[MF_COUNT] = {1, 1, 3, 1, 2, 2, 1, 1, 1, 1, 1, 1, 2, 2, 1, 1, 1, 1, 1, 1};
    static const unsigned max_args[MF_COUNT] = {~0u, ~0u, 3, 2, 2, 2, 1, 1, 1, 1, 1, 1, 2, 2, 1, ~0u, 2, 1, 1, 1};
    if (n < min_args[fn] || n > max_args[fn]) goto done;
    struct cm *m = m_new(P, MN_FN, n);
    if (!m) goto done;
    m->fn = (uint8_t)fn;
    m->key = (uint8_t)strategy;
    memcpy(m->k, args, n * sizeof *args);
    uint8_t t0 = args[0]->type;
    switch (fn) {
    case MF_MIN: case MF_MAX: case MF_CLAMP: case MF_HYPOT: case MF_MOD: case MF_REM: case MF_ATAN2:
        for (unsigned i = 1; i < n; i++) if (args[i]->type != t0) goto done;
        m->type = fn == MF_ATAN2 ? MT_ANGLE : t0;
        break;
    case MF_ROUND:
        if (n == 1 && t0 != MT_NUM) goto done; /* the step defaults to the number 1 */
        if (n == 2 && args[1]->type != t0) goto done;
        m->type = t0;
        break;
    case MF_SIN: case MF_COS: case MF_TAN:
        if (t0 != MT_NUM && t0 != MT_ANGLE) goto done;
        m->type = MT_NUM;
        break;
    case MF_ASIN: case MF_ACOS: case MF_ATAN:
        if (t0 != MT_NUM) goto done;
        m->type = MT_ANGLE;
        break;
    case MF_POW: case MF_LOG:
        for (unsigned i = 0; i < n; i++) if (args[i]->type != MT_NUM) goto done;
        m->type = MT_NUM;
        break;
    case MF_SQRT: case MF_EXP:
        if (t0 != MT_NUM) goto done;
        m->type = MT_NUM;
        break;
    case MF_ABS: m->type = t0; break;
    case MF_SIGN: m->type = MT_NUM; break;
    }
    result = m_simplify(P, m);
done:
    P->in_math = outer;
    P->depth--;
    return result;
}
static struct cm *m_value(struct pctx *P, const struct tok *t) {
    if (t->kind == TK_NUM) {
        char unit[8] = {0};
        uint8_t type;
        if (t->name_n >= sizeof unit || !unit_type(t->name, t->name_n, t->pct, &type)) return NULL;
        for (size_t i = 0; i < t->name_n; i++) unit[i] = (char)lower((unsigned char)t->name[i]);
        struct cm *m = m_leaf(P, t->num, unit, type);
        if (m && P->in_math) m_canon(m);
        return m;
    }
    if (t->kind == TK_IDENT) {
        if (P->keys)
            for (int i = 0; i < 4; i++)
                if (P->keys[i] && name_is(TK_NAME(t), TK_LEN(t), P->keys[i])) {
                    struct cm *m = m_new(P, MN_KEY, 0);
                    if (m) { m->key = (uint8_t)i; m->type = MT_NUM; }
                    return m;
                }
        if (!P->in_math) return NULL;
        static const struct { const char *name; double v; } constants[] = {
            {"e", M_E}, {"pi", M_PI}, {"infinity", INFINITY}, {"-infinity", -INFINITY}, {"nan", NAN}};
        for (size_t i = 0; i < sizeof constants / sizeof *constants; i++)
            if (name_is(TK_NAME(t), TK_LEN(t), constants[i].name)) return m_leaf(P, constants[i].v, "", MT_NUM);
        return NULL;
    }
    if (t->kind == TK_PAREN && P->in_math) {
        if (++P->depth > 32) return NULL;
        struct tok buf[TOKS_MAX];
        int n;
        const struct tok *k = lx_all(P, t->in, t->in_n, buf, &n);
        struct cm *m = k && n ? m_sum(P, k, n) : NULL;
        P->depth--;
        return m;
    }
    if (t->kind == TK_FUNC) {
        bool calc;
        return m_function(P, t, &calc);
    }
    return NULL;
}
static struct cm *m_product(struct pctx *P, const struct tok *t, int n) {
    if (n <= 0) return NULL;
    struct cm *first = m_value(P, &t[0]);
    if (!first) return NULL;
    if (n == 1) return first;
    struct cm *items[64];
    unsigned k = 0;
    items[k++] = first;
    for (int i = 1; i < n; i += 2) {
        if (i + 1 >= n || (t[i].kind != TK_DELIM && t[i].kind != TK_SLASH)) return NULL;
        if (t[i].kind == TK_DELIM && t[i].delim != '*') return NULL;
        struct cm *v = m_value(P, &t[i + 1]);
        if (!v || k == 64) return NULL;
        if (t[i].kind == TK_SLASH) {
            struct cm *inv = m_new(P, MN_INVERT, 1);
            if (!inv) return NULL;
            inv->k[0] = v;
            inv->type = v->type;
            items[k++] = m_simplify(P, inv);
        } else items[k++] = v;
    }
    /* typed multiplication: at most one non-number factor that is not divided by its own type */
    uint8_t type = MT_NUM;
    for (unsigned i = 0; i < k; i++) {
        struct cm *c = items[i];
        bool inverse = c->op == MN_INVERT;
        uint8_t ct = c->type;
        if (ct == MT_NUM) continue;
        if (inverse) {
            if (type != ct) return NULL;
            type = MT_NUM;
        } else {
            if (type != MT_NUM) return NULL;
            type = ct;
        }
    }
    struct cm *m = m_new(P, MN_PRODUCT, k);
    if (!m) return NULL;
    memcpy(m->k, items, k * sizeof *items);
    m->type = type;
    return m_simplify(P, m);
}
static struct cm *m_sum(struct pctx *P, const struct tok *t, int n) {
    struct cm *items[64];
    unsigned k = 0;
    int start = 0;
    char sign = '+';
    for (int i = 0; i <= n; i++) {
        bool op = i < n && t[i].kind == TK_DELIM && (t[i].delim == '+' || t[i].delim == '-');
        if (i < n && !op) continue;
        if (op) {
            /* + and - need whitespace on both sides */
            if (!i || i + 1 >= n || t[i - 1].e == t[i].s || t[i].e == t[i + 1].s) return NULL;
        }
        struct cm *v = m_product(P, t + start, i - start);
        if (!v || k == 64) return NULL;
        if (sign == '-') {
            struct cm *neg = m_new(P, MN_NEGATE, 1);
            if (!neg) return NULL;
            neg->k[0] = v;
            neg->type = v->type;
            v = m_simplify(P, neg);
        }
        if (k && v->type != items[0]->type) return NULL;
        items[k++] = v;
        if (i < n) sign = t[i].delim;
        start = i + 1;
    }
    if (k == 1) return items[0];
    struct cm *m = m_new(P, MN_SUM, k);
    if (!m) return NULL;
    memcpy(m->k, items, k * sizeof *items);
    m->type = items[0]->type;
    return m_simplify(P, m);
}
/* a copy of m with lengths of the element in pixels, simplified again */
static struct cm *m_bind(struct pctx *P, const struct cm *m, const struct cx *cx) {
    if (m->op == MN_LEAF) {
        struct cm *c = m_leaf(P, m->v, m->unit, m->type);
        if (c && c->type == MT_LEN && cx) {
            bool r = true;
            c->v = leaf_value(m, cx, &r);
            strcpy(c->unit, "px");
        }
        return c;
    }
    struct cm *c = m_new(P, m->op, m->n);
    if (!c) return NULL;
    c->type = m->type;
    c->fn = m->fn;
    c->key = m->key;
    for (unsigned i = 0; i < m->n; i++)
        if (!(c->k[i] = m_bind(P, m->k[i], cx))) return NULL;
    return c->op == MN_KEY ? c : m_simplify(P, c);
}
static bool m_context_free(const struct cm *m) {
    if (m->op == MN_KEY) return false;
    if (m->op == MN_LEAF) return m_free(m);
    for (unsigned i = 0; i < m->n; i++) if (!m_context_free(m->k[i])) return false;
    return true;
}

/* ---------------------------------------------------------------- numbers */
/* a <number> as CSS serializes it: at most six significant figures, no exponent */
static void num_put(sbuf *b, double v) {
    char t[64];
    if (isnan(v)) v = 0;
    if (fabs(v) < 5e-7) v = 0;
    snprintf(t, sizeof t, "%.6g", v);
    if (strchr(t, 'e')) {
        snprintf(t, sizeof t, fabs(v) >= 1 ? "%.0f" : "%.6f", v);
        if (strchr(t, '.')) {
            size_t n = strlen(t);
            while (n && t[n - 1] == '0') t[--n] = 0;
            if (n && t[n - 1] == '.') t[--n] = 0;
        }
    }
    if (!strcmp(t, "-0")) strcpy(t, "0");
    sb_puts(b, t);
}
static void m_put(sbuf *b, const struct cm *m, const char *const *keys, bool top);
static void m_put_leaf(sbuf *b, const struct cm *m) {
    if (!isfinite(m->v)) {
        sb_puts(b, isnan(m->v) ? "NaN" : m->v > 0 ? "infinity" : "-infinity");
        if (m->type != MT_NUM) sb_puts(b, m->type == MT_PCT ? " * 1%" : m->type == MT_ANGLE ? " * 1deg" : " * 1px");
        return;
    }
    num_put(b, m->v);
    sb_puts(b, m->unit);
}
/* sort a calculation's children: number, percentage, dimensions by unit, then the rest */
static void m_sorted(const struct cm *m, const struct cm **out) {
    unsigned k = 0;
    for (unsigned i = 0; i < m->n; i++) if (m->k[i]->op == MN_LEAF && m->k[i]->type == MT_NUM) out[k++] = m->k[i];
    for (unsigned i = 0; i < m->n; i++) if (m->k[i]->op == MN_LEAF && m->k[i]->type == MT_PCT) out[k++] = m->k[i];
    unsigned dims = k;
    for (unsigned i = 0; i < m->n; i++)
        if (m->k[i]->op == MN_LEAF && m->k[i]->type != MT_NUM && m->k[i]->type != MT_PCT) out[k++] = m->k[i];
    for (unsigned i = dims + 1; i < k; i++)
        for (unsigned j = i; j > dims && strcmp(out[j - 1]->unit, out[j]->unit) > 0; j--) {
            const struct cm *x = out[j]; out[j] = out[j - 1]; out[j - 1] = x;
        }
    for (unsigned i = 0; i < m->n; i++) if (m->k[i]->op != MN_LEAF) out[k++] = m->k[i];
}
static void m_put_child(sbuf *b, const struct cm *m, const char *const *keys, bool strip) {
    sbuf t = {0};
    m_put(&t, m, keys, false);
    const char *s = t.p ? t.p : "";
    size_t n = t.n;
    if (strip && n >= 2 && s[0] == '(' && s[n - 1] == ')') { s++; n -= 2; }
    sb_put(b, s, n);
    sb_free(&t);
}
/* CSS Values 4, 10.13: serialize a calculation tree; top: the root of a channel's math function */
static void m_put(sbuf *b, const struct cm *m, const char *const *keys, bool top) {
    if (top && m->op != MN_FN) {
        sb_puts(b, "calc(");
        m_put_child(b, m, keys, true);
        sb_putc(b, ')');
        return;
    }
    switch (m->op) {
    case MN_LEAF: m_put_leaf(b, m); return;
    case MN_KEY: sb_puts(b, keys && keys[m->key] ? keys[m->key] : "none"); return;
    case MN_NEGATE: sb_puts(b, "(-1 * "); m_put_child(b, m->k[0], keys, false); sb_putc(b, ')'); return;
    case MN_INVERT: sb_puts(b, "(1 / "); m_put_child(b, m->k[0], keys, false); sb_putc(b, ')'); return;
    case MN_SUM: case MN_PRODUCT: {
        const struct cm *k[256];
        m_sorted(m, k);
        sb_putc(b, '(');
        for (unsigned i = 0; i < m->n; i++) {
            const struct cm *c = k[i];
            if (!i) m_put_child(b, c, keys, false);
            else if (m->op == MN_SUM && c->op == MN_NEGATE) { sb_puts(b, " - "); m_put_child(b, c->k[0], keys, false); }
            else if (m->op == MN_SUM && c->op == MN_LEAF && (c->v < 0 || (c->v == 0 && signbit(c->v)))) {
                struct cm neg = *c;
                neg.v = -c->v;
                sb_puts(b, " - ");
                m_put_leaf(b, &neg);
            } else if (m->op == MN_PRODUCT && c->op == MN_INVERT) { sb_puts(b, " / "); m_put_child(b, c->k[0], keys, false); }
            else { sb_puts(b, m->op == MN_SUM ? " + " : " * "); m_put_child(b, c, keys, false); }
        }
        sb_putc(b, ')');
        return;
    }
    default:
        sb_puts(b, math_names[m->fn]);
        sb_putc(b, '(');
        if (m->fn == MF_ROUND && m->key != RS_NEAREST) { sb_puts(b, round_names[m->key]); sb_puts(b, ", "); }
        for (unsigned i = 0; i < m->n; i++) {
            if (i) sb_puts(b, ", ");
            m_put_child(b, m->k[i], keys, true);
        }
        sb_putc(b, ')');
    }
}

/* ---------------------------------------------------------------- the value tree */
enum { CV_ABS, CV_CURRENT, CV_SYSTEM, CV_FN, CV_MIX, CV_LIGHT_DARK, CV_CONTRAST, CV_LAYERS };
enum { F_RGB, F_HSL, F_HWB, F_LAB, F_LCH, F_OKLAB, F_OKLCH, F_COLOR, F_ALPHA };
enum { CH_ABSENT, CH_NONE, CH_LEAF, CH_MATH };
struct chan {
    uint8_t kind, type;
    bool wrapped;   /* CH_MATH: a math function whose root is not one: serialized in calc() */
    char unit[7];   /* CH_LEAF: the authored unit */
    double v;       /* CH_LEAF: the authored value */
    struct cm *m;   /* CH_MATH */
};
struct mix_item {
    const struct css_color_value *color;
    struct chan pct; /* CH_ABSENT: omitted */
};
struct css_color_value {
    uint8_t kind;
    uint8_t fn;       /* CV_FN: F_*; CV_SYSTEM: the system colour */
    uint8_t space;    /* CV_FN: the processing space; CV_MIX: the interpolation space */
    uint8_t hue;      /* CV_MIX: enum css_hue_method */
    bool legacy;      /* CV_FN: commas */
    bool method;      /* CV_MIX: an interpolation method was authored */
    bool hue_given;   /* CV_MIX: a hue method was authored */
    uint8_t blend;    /* CV_LAYERS: the blend mode (BLEND_*) */
    uint32_t argb;    /* CV_ABS kept in a style: the used value its 8-bit field had */
    struct css_color color; /* CV_ABS */
    const char *name;       /* CV_ABS: the named colour or transparent keyword */
    const struct css_color_value *origin, *pair[2];
    struct chan ch[4];
    struct mix_item *items; /* CV_MIX; CV_LAYERS, the top one first, without percentages */
    unsigned count;
};
static const struct css_color_value current_value = {.kind = CV_CURRENT};

static const char *const space_names[CSS_SPACES] = {"srgb", "srgb-linear", "display-p3", "display-p3-linear",
    "a98-rgb", "prophoto-rgb", "rec2020", "xyz-d50", "xyz-d65", "lab", "lch", "oklab", "oklch", "hsl", "hwb"};
static const char *const hue_names[] = {"shorter", "longer", "increasing", "decreasing"};
/* <blend-mode> (Compositing 1, 10) */
enum { BLEND_NORMAL, BLEND_MULTIPLY, BLEND_SCREEN, BLEND_OVERLAY, BLEND_DARKEN, BLEND_LIGHTEN, BLEND_COLOR_DODGE,
       BLEND_COLOR_BURN, BLEND_HARD_LIGHT, BLEND_SOFT_LIGHT, BLEND_DIFFERENCE, BLEND_EXCLUSION, BLEND_HUE,
       BLEND_SATURATION, BLEND_COLOR, BLEND_LUMINOSITY, BLEND_COUNT };
static const char *const blend_names[BLEND_COUNT] = {"normal", "multiply", "screen", "overlay", "darken", "lighten",
    "color-dodge", "color-burn", "hard-light", "soft-light", "difference", "exclusion", "hue", "saturation", "color",
    "luminosity"};
static bool space_polar(uint8_t s) { return s == CSS_SPACE_LCH || s == CSS_SPACE_OKLCH || s == CSS_SPACE_HSL || s == CSS_SPACE_HWB; }
static int space_hue(uint8_t s) {
    return s == CSS_SPACE_LCH || s == CSS_SPACE_OKLCH ? 2 : s == CSS_SPACE_HSL || s == CSS_SPACE_HWB ? 0 : -1;
}
static int space_named(const char *s, size_t n) {
    if (name_is(s, n, "xyz")) return CSS_SPACE_XYZ_D65; /* serialized as xyz-d65 */
    for (int i = 0; i < CSS_SPACES; i++)
        if (name_is(s, n, space_names[i])) return i;
    return -1;
}

/* named colours (CSS Color 4, 6.1), sorted */
static const struct { const char *name; uint32_t rgb; } named[] = {
    {"aliceblue", 0xF0F8FF}, {"antiquewhite", 0xFAEBD7}, {"aqua", 0x00FFFF}, {"aquamarine", 0x7FFFD4},
    {"azure", 0xF0FFFF}, {"beige", 0xF5F5DC}, {"bisque", 0xFFE4C4}, {"black", 0x000000},
    {"blanchedalmond", 0xFFEBCD}, {"blue", 0x0000FF}, {"blueviolet", 0x8A2BE2}, {"brown", 0xA52A2A},
    {"burlywood", 0xDEB887}, {"cadetblue", 0x5F9EA0}, {"chartreuse", 0x7FFF00}, {"chocolate", 0xD2691E},
    {"coral", 0xFF7F50}, {"cornflowerblue", 0x6495ED}, {"cornsilk", 0xFFF8DC}, {"crimson", 0xDC143C},
    {"cyan", 0x00FFFF}, {"darkblue", 0x00008B}, {"darkcyan", 0x008B8B}, {"darkgoldenrod", 0xB8860B},
    {"darkgray", 0xA9A9A9}, {"darkgreen", 0x006400}, {"darkgrey", 0xA9A9A9}, {"darkkhaki", 0xBDB76B},
    {"darkmagenta", 0x8B008B}, {"darkolivegreen", 0x556B2F}, {"darkorange", 0xFF8C00}, {"darkorchid", 0x9932CC},
    {"darkred", 0x8B0000}, {"darksalmon", 0xE9967A}, {"darkseagreen", 0x8FBC8F}, {"darkslateblue", 0x483D8B},
    {"darkslategray", 0x2F4F4F}, {"darkslategrey", 0x2F4F4F}, {"darkturquoise", 0x00CED1}, {"darkviolet", 0x9400D3},
    {"deeppink", 0xFF1493}, {"deepskyblue", 0x00BFFF}, {"dimgray", 0x696969}, {"dimgrey", 0x696969},
    {"dodgerblue", 0x1E90FF}, {"firebrick", 0xB22222}, {"floralwhite", 0xFFFAF0}, {"forestgreen", 0x228B22},
    {"fuchsia", 0xFF00FF}, {"gainsboro", 0xDCDCDC}, {"ghostwhite", 0xF8F8FF}, {"gold", 0xFFD700},
    {"goldenrod", 0xDAA520}, {"gray", 0x808080}, {"green", 0x008000}, {"greenyellow", 0xADFF2F},
    {"grey", 0x808080}, {"honeydew", 0xF0FFF0}, {"hotpink", 0xFF69B4}, {"indianred", 0xCD5C5C},
    {"indigo", 0x4B0082}, {"ivory", 0xFFFFF0}, {"khaki", 0xF0E68C}, {"lavender", 0xE6E6FA},
    {"lavenderblush", 0xFFF0F5}, {"lawngreen", 0x7CFC00}, {"lemonchiffon", 0xFFFACD}, {"lightblue", 0xADD8E6},
    {"lightcoral", 0xF08080}, {"lightcyan", 0xE0FFFF}, {"lightgoldenrodyellow", 0xFAFAD2}, {"lightgray", 0xD3D3D3},
    {"lightgreen", 0x90EE90}, {"lightgrey", 0xD3D3D3}, {"lightpink", 0xFFB6C1}, {"lightsalmon", 0xFFA07A},
    {"lightseagreen", 0x20B2AA}, {"lightskyblue", 0x87CEFA}, {"lightslategray", 0x778899},
    {"lightslategrey", 0x778899}, {"lightsteelblue", 0xB0C4DE}, {"lightyellow", 0xFFFFE0}, {"lime", 0x00FF00},
    {"limegreen", 0x32CD32}, {"linen", 0xFAF0E6}, {"magenta", 0xFF00FF}, {"maroon", 0x800000},
    {"mediumaquamarine", 0x66CDAA}, {"mediumblue", 0x0000CD}, {"mediumorchid", 0xBA55D3},
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
    {"white", 0xFFFFFF}, {"whitesmoke", 0xF5F5F5}, {"yellow", 0xFFFF00}, {"yellowgreen", 0x9ACD32},
};
static int named_find(const char *s, size_t n) {
    int lo = 0, hi = (int)(sizeof named / sizeof *named) - 1;
    while (lo <= hi) {
        int mid = (lo + hi) / 2;
        const char *nm = named[mid].name;
        int c = 0;
        size_t i = 0;
        for (; i < n && nm[i]; i++)
            if ((c = lower((unsigned char)s[i]) - nm[i])) break;
        if (!c) c = i < n ? 1 : nm[i] ? -1 : 0;
        if (!c) return mid;
        if (c < 0) hi = mid - 1;
        else lo = mid + 1;
    }
    return -1;
}

/* System colours (CSS Color 4, 6.2): this user agent's light and dark
   palettes. The deprecated keywords compute to the colour they name. */
enum { SYS_ACCENTCOLOR, SYS_ACCENTCOLORTEXT, SYS_ACTIVETEXT, SYS_BUTTONBORDER, SYS_BUTTONFACE, SYS_BUTTONTEXT,
       SYS_CANVAS, SYS_CANVASTEXT, SYS_FIELD, SYS_FIELDTEXT, SYS_GRAYTEXT, SYS_HIGHLIGHT, SYS_HIGHLIGHTTEXT,
       SYS_LINKTEXT, SYS_MARK, SYS_MARKTEXT, SYS_SELECTEDITEM, SYS_SELECTEDITEMTEXT, SYS_VISITEDTEXT, SYS_COUNT };
static const struct { const char *name; uint32_t light, dark; } system_colors[SYS_COUNT] = {
    {"accentcolor", 0x2A6BD8, 0x7AA7FF}, {"accentcolortext", 0xFFFFFF, 0x000000},
    {"activetext", 0xEE0000, 0xFF7B7B}, {"buttonborder", 0x767676, 0x8C8C8C},
    {"buttonface", 0xEFEFEF, 0x3B3B3B}, {"buttontext", 0x000000, 0xFFFFFF},
    {"canvas", 0xFFFFFF, 0x121212}, {"canvastext", 0x000000, 0xFFFFFF},
    {"field", 0xFFFFFF, 0x2B2B2B}, {"fieldtext", 0x000000, 0xFFFFFF},
    {"graytext", 0x808080, 0x9A9A9A}, {"highlight", 0x3390FF, 0x2F6FCC},
    {"highlighttext", 0xFFFFFF, 0xFFFFFF}, {"linktext", 0x0000EE, 0x9E9EFF},
    {"mark", 0xFFFF00, 0x7A6A00}, {"marktext", 0x000000, 0xFFFFFF},
    {"selecteditem", 0x3390FF, 0x2F6FCC}, {"selecteditemtext", 0xFFFFFF, 0xFFFFFF},
    {"visitedtext", 0x551A8B, 0xD0ADF0},
};
static const struct { const char *name; uint8_t as; } deprecated_colors[] = {
    {"activeborder", SYS_BUTTONBORDER}, {"activecaption", SYS_CANVAS}, {"appworkspace", SYS_CANVAS},
    {"background", SYS_CANVAS}, {"buttonhighlight", SYS_BUTTONFACE}, {"buttonshadow", SYS_BUTTONFACE},
    {"captiontext", SYS_CANVASTEXT}, {"inactiveborder", SYS_BUTTONBORDER}, {"inactivecaption", SYS_CANVAS},
    {"inactivecaptiontext", SYS_GRAYTEXT}, {"infobackground", SYS_CANVAS}, {"infotext", SYS_CANVASTEXT},
    {"menu", SYS_CANVAS}, {"menutext", SYS_CANVASTEXT}, {"scrollbar", SYS_CANVAS},
    {"threeddarkshadow", SYS_BUTTONBORDER}, {"threedface", SYS_BUTTONFACE}, {"threedhighlight", SYS_BUTTONBORDER},
    {"threedlightshadow", SYS_BUTTONBORDER}, {"threedshadow", SYS_BUTTONBORDER}, {"window", SYS_CANVAS},
    {"windowframe", SYS_BUTTONBORDER}, {"windowtext", SYS_CANVASTEXT},
};

struct css_color css_color_from_argb(uint32_t argb) {
    struct css_color c = {{((argb >> 16) & 255) / 255.0, ((argb >> 8) & 255) / 255.0, (argb & 255) / 255.0},
                          (argb >> 24) / 255.0, CSS_SPACE_SRGB, 0, true};
    return c;
}
static struct css_color system_color(int index, bool dark) {
    return css_color_from_argb(0xFF000000u | (dark ? system_colors[index].dark : system_colors[index].light));
}

/* ---------------------------------------------------------------- colour function parsing */
static const char *const keys_rgb[4] = {"r", "g", "b", "alpha"}, *const keys_hsl[4] = {"h", "s", "l", "alpha"},
    *const keys_hwb[4] = {"h", "w", "b", "alpha"}, *const keys_lab[4] = {"l", "a", "b", "alpha"},
    *const keys_lch[4] = {"l", "c", "h", "alpha"}, *const keys_xyz[4] = {"x", "y", "z", "alpha"},
    *const keys_alpha[4] = {NULL, NULL, NULL, "alpha"};
static const char *const *fn_keys(uint8_t fn, uint8_t space) {
    switch (fn) {
    case F_RGB: return keys_rgb;
    case F_HSL: return keys_hsl;
    case F_HWB: return keys_hwb;
    case F_LAB: case F_OKLAB: return keys_lab;
    case F_LCH: case F_OKLCH: return keys_lch;
    case F_ALPHA: return keys_alpha;
    default: return space == CSS_SPACE_XYZ_D50 || space == CSS_SPACE_XYZ_D65 ? keys_xyz : keys_rgb;
    }
}
static const char *const fn_names[] = {"rgb", "hsl", "hwb", "lab", "lch", "oklab", "oklch", "color", "alpha"};
/* the types channel i of fn accepts: bit 1<<MT_* */
static unsigned chan_types(uint8_t fn, int i, bool legacy) {
    const unsigned NP = 1u << MT_NUM | 1u << MT_PCT, HUE = 1u << MT_NUM | 1u << MT_ANGLE;
    if (i == 3) return NP;
    switch (fn) {
    case F_HSL: return !i ? HUE : legacy ? 1u << MT_PCT : NP;
    case F_HWB: return !i ? HUE : NP;
    case F_LCH: case F_OKLCH: return i == 2 ? HUE : NP;
    default: return NP;
    }
}
static double chan_raw(const struct chan *c, const double *keys, const struct cx *cx) {
    if (c->kind == CH_LEAF) {
        if (c->type == MT_ANGLE) return c->v * angle_unit(c->unit, strlen(c->unit));
        return c->v;
    }
    bool r = true;
    double v = m_eval(c->m, keys, cx, &r);
    return v;
}
static double hue_normal(double h) {
    if (!isfinite(h)) return 0;
    h = fmod(h, 360);
    if (h < 0) h += 360;
    if (h >= 360) h = 0;
    return h;
}
/* a channel's component value in the model's units; relative: no clamping but alpha's */
static double chan_component(uint8_t fn, uint8_t space, int i, uint8_t type, double x, bool relative) {
    if (isnan(x)) x = 0;
    if (i == 3) {
        double a = type == MT_PCT ? x / 100 : x;
        return a < 0 ? 0 : a > 1 ? 1 : a;
    }
    bool pct = type == MT_PCT;
    double v = x;
    switch (fn) {
    case F_RGB:
        v = pct ? x / 100 : x / 255;
        if (!relative) v = v < 0 ? 0 : v > 1 ? 1 : v;
        return v;
    case F_HSL: case F_HWB:
        if (!i) return relative ? (isfinite(x) ? x : 0) : hue_normal(x);
        if (!relative && fn == F_HSL && i == 1 && v < 0) v = 0;
        return v;
    /* lightness and chroma are clamped in relative colours too */
    case F_LAB: case F_LCH:
        if (!i) return v < 0 ? 0 : v > 100 ? 100 : v;
        if (fn == F_LCH && i == 2) return relative ? (isfinite(x) ? x : 0) : hue_normal(x);
        if (pct) v = x * (fn == F_LAB ? 1.25 : 1.5);
        if (fn == F_LCH && v < 0) v = 0;
        return v;
    case F_OKLAB: case F_OKLCH:
        if (!i) { if (pct) v = x / 100; return v < 0 ? 0 : v > 1 ? 1 : v; }
        if (fn == F_OKLCH && i == 2) return relative ? (isfinite(x) ? x : 0) : hue_normal(x);
        if (pct) v = x * 0.004;
        if (fn == F_OKLCH && v < 0) v = 0;
        return v;
    default:
        (void)space;
        return pct ? x / 100 : x;
    }
}
static uint8_t fn_space(uint8_t fn) {
    static const uint8_t s[] = {CSS_SPACE_SRGB, CSS_SPACE_HSL, CSS_SPACE_HWB, CSS_SPACE_LAB, CSS_SPACE_LCH,
                                CSS_SPACE_OKLAB, CSS_SPACE_OKLCH};
    return fn < sizeof s ? s[fn] : CSS_SPACE_SRGB;
}
static bool cv_parse(struct pctx *P, const struct tok *t, struct css_color_value *out);
static struct css_color_value *cv_new(struct pctx *P) { return p_alloc(P, sizeof(struct css_color_value)); }
/* one channel token: none, a literal, a keyword or a math function */
static bool parse_chan(struct pctx *P, const struct tok *t, unsigned types, bool allow_none, struct chan *c) {
    memset(c, 0, sizeof *c);
    if (tok_ident(t, "none")) {
        if (!allow_none) return false;
        c->kind = CH_NONE;
        return true;
    }
    if (t->kind == TK_NUM) {
        uint8_t type;
        if (t->name_n >= sizeof c->unit || !unit_type(t->name, t->name_n, t->pct, &type)) return false;
        if (!(types & 1u << type)) return false;
        c->kind = CH_LEAF;
        c->type = type;
        c->v = t->num;
        for (size_t i = 0; i < t->name_n; i++) c->unit[i] = (char)lower((unsigned char)t->name[i]);
        return true;
    }
    if (t->kind != TK_IDENT && t->kind != TK_FUNC) return false;
    struct cm *m;
    if (t->kind == TK_FUNC) {
        bool calc;
        if (!(m = m_function(P, t, &calc))) return false;
        c->wrapped = m->op != MN_FN; /* serialized inside calc() */
    } else if (!(m = m_value(P, t))) return false;
    if (!(types & 1u << m->type)) return false;
    c->kind = CH_MATH;
    c->type = m->type;
    c->m = m;
    return true;
}
static bool chan_context_free(const struct chan *c) {
    return c->kind != CH_MATH || m_context_free(c->m);
}
/* evaluate a function's channels into an absolute colour: origin (converted into the
   function's space) gives the keywords; NULL: absolute syntax */
static struct css_color convert_carry(const struct css_color *c, uint8_t space);
static struct css_color fn_evaluate(const struct css_color_value *v, const struct css_color *origin, const struct cx *cx) {
    struct css_color out = {{0, 0, 0}, 1, v->space, 0, false};
    double keys[4] = {0, 0, 0, 0};
    uint8_t key_none = 0;
    struct css_color base;
    if (origin) {
        base = v->fn == F_ALPHA ? *origin : convert_carry(origin, v->space);
        double scale = v->fn == F_RGB ? 255 : 1;
        for (int i = 0; i < 3; i++) {
            keys[i] = base.none & 1u << i ? 0 : base.c[i] * scale;
            if (space_hue(v->space) == i) keys[i] = hue_normal(keys[i]);
        }
        keys[3] = base.none & CSS_COLOR_NONE_ALPHA ? 0 : base.alpha;
        key_none = base.none;
        if (v->fn == F_ALPHA) { out = base; out.legacy = false; }
    }
    for (int i = 0; i < 4; i++) {
        const struct chan *c = &v->ch[i];
        uint8_t bit = i == 3 ? CSS_COLOR_NONE_ALPHA : (uint8_t)(1u << i);
        if (v->fn == F_ALPHA && i < 3) continue;
        if (c->kind == CH_ABSENT) {
            /* an omitted alpha: the origin's in relative syntax, else opaque */
            if (origin) {
                out.alpha = keys[3];
                if (key_none & CSS_COLOR_NONE_ALPHA) out.none |= CSS_COLOR_NONE_ALPHA;
            }
            continue;
        }
        if (c->kind == CH_NONE || (c->kind == CH_MATH && c->m->op == MN_KEY && !c->wrapped &&
                                   (key_none & (c->m->key == 3 ? CSS_COLOR_NONE_ALPHA : 1u << c->m->key)))) {
            out.none |= bit;
            if (i == 3) out.alpha = 0;
            else out.c[i] = 0;
            continue;
        }
        double value = chan_component(v->fn, v->space, i, c->type, chan_raw(c, keys, cx), origin != NULL);
        if (i == 3) out.alpha = value;
        else out.c[i] = value;
    }
    if (!origin && v->fn <= F_HWB) out.legacy = true;
    return out;
}
/* rgb() rgba() hsl() hsla() hwb() lab() lch() oklab() oklch() color() alpha() */
static bool parse_fn(struct pctx *P, uint8_t fn, const struct tok *ft, struct css_color_value *out) {
    struct tok buf[TOKS_MAX];
    int n;
    const struct tok *t = lx_all(P, ft->in, ft->in_n, buf, &n);
    if (!t || !n) return false;
    memset(out, 0, sizeof *out);
    out->kind = CV_FN;
    out->fn = fn;
    out->space = fn_space(fn);
    int i = 0;
    const struct css_color_value *origin = NULL;
    if (tok_ident(&t[0], "from")) {
        if (n < 2) return false;
        struct css_color_value *o = cv_new(P);
        if (!cv_parse(P, &t[1], o)) return false;
        origin = o;
        i = 2;
    } else if (fn == F_ALPHA) return false;
    if (fn == F_COLOR) {
        if (i >= n || t[i].kind != TK_IDENT) return false;
        int s = space_named(TK_NAME(&t[i]), TK_LEN(&t[i]));
        if (s < 0 || s > CSS_SPACE_XYZ_D65) return false;
        out->space = (uint8_t)s;
        i++;
    }
    out->origin = origin;
    bool commas = false;
    for (int k = i; k < n; k++) if (t[k].kind == TK_COMMA) commas = true;
    const char *const *keys = origin ? fn_keys(fn, out->space) : NULL;
    const char *const *saved = P->keys;
    P->keys = keys;
    bool ok = false;
    const struct tok *chan_tok[4] = {NULL, NULL, NULL, NULL};
    if (fn == F_ALPHA) {
        if (i + 2 != n || t[i].kind != TK_SLASH) goto done;
        chan_tok[3] = &t[i + 1];
    } else if (commas) {
        /* legacy syntax: rgb() and hsl() only, value, value, value [, alpha] */
        if (origin || (fn != F_RGB && fn != F_HSL)) goto done;
        int count = 0;
        for (int k = i; k < n; k += 2) {
            if (count == 4 || (k + 1 < n && t[k + 1].kind != TK_COMMA) || t[k].kind == TK_COMMA) goto done;
            if (k + 1 == n - 1) goto done; /* trailing comma */
            chan_tok[count++] = &t[k];
        }
        if (count < 3) goto done;
        out->legacy = true;
    } else {
        int count = 0, k = i;
        for (; k < n && t[k].kind != TK_SLASH; k++) {
            if (count == 3) goto done;
            chan_tok[count++] = &t[k];
        }
        if (count != 3) goto done;
        if (k < n) {
            if (k + 2 != n) goto done;
            chan_tok[3] = &t[k + 1];
        }
    }
    for (int c = 0; c < 4; c++) {
        if (!chan_tok[c]) continue;
        if (!parse_chan(P, chan_tok[c], chan_types(fn, c, out->legacy), !out->legacy, &out->ch[c])) goto done;
    }
    if (out->legacy && fn == F_RGB) {
        /* all numbers or all percentages */
        uint8_t type = out->ch[0].type;
        if (out->ch[1].type != type || out->ch[2].type != type) goto done;
    }
    ok = true;
done:
    P->keys = saved;
    if (!ok) return false;
    if (!origin) {
        /* An absolute colour: computed now unless a channel needs the element. The
           specified value of a non-legacy function keeps its math functions. */
        bool free = true;
        for (int c = 0; c < 4; c++)
            free &= chan_context_free(&out->ch[c]) && (fn <= F_HWB || out->ch[c].kind != CH_MATH);
        if (free) {
            struct css_color color = fn_evaluate(out, NULL, NULL);
            memset(out, 0, sizeof *out);
            out->kind = CV_ABS;
            out->color = color;
        }
    }
    return true;
}
static bool parse_hex(const struct tok *t, struct css_color_value *out) {
    int d[8];
    size_t k = t->name_n;
    if (k != 3 && k != 4 && k != 6 && k != 8) return false;
    for (size_t i = 0; i < k; i++)
        if ((d[i] = web_hex_digit((unsigned char)t->name[i])) < 0) return false;
    uint32_t r, g, b, a = 255;
    if (k <= 4) {
        r = (uint32_t)d[0] * 17, g = (uint32_t)d[1] * 17, b = (uint32_t)d[2] * 17;
        if (k == 4) a = (uint32_t)d[3] * 17;
    } else {
        r = (uint32_t)(d[0] * 16 + d[1]), g = (uint32_t)(d[2] * 16 + d[3]), b = (uint32_t)(d[4] * 16 + d[5]);
        if (k == 8) a = (uint32_t)(d[6] * 16 + d[7]);
    }
    memset(out, 0, sizeof *out);
    out->kind = CV_ABS;
    out->color = css_color_from_argb(ARGB(a, r, g, b));
    return true;
}
/* color-mix( [ <color-interpolation-method> , ]? [ <color> && <percentage [0,100]>? ]# ) */
static bool parse_mix(struct pctx *P, const struct tok *ft, struct css_color_value *out) {
    struct tok buf[TOKS_MAX];
    int n;
    const struct tok *t = lx_all(P, ft->in, ft->in_n, buf, &n);
    if (!t || !n) return false;
    memset(out, 0, sizeof *out);
    out->kind = CV_MIX;
    out->space = CSS_SPACE_OKLAB;
    out->hue = CSS_HUE_SHORTER;
    int i = 0;
    if (tok_ident(&t[0], "in")) {
        if (n < 3 || t[1].kind != TK_IDENT) return false;
        int s = space_named(TK_NAME(&t[1]), TK_LEN(&t[1]));
        if (s < 0) return false;
        out->space = (uint8_t)s;
        out->method = true;
        i = 2;
        if (i < n && t[i].kind == TK_IDENT) {
            int h = -1;
            for (int k = 0; k < 4; k++) if (tok_ident(&t[i], hue_names[k])) h = k;
            if (h < 0 || !space_polar(out->space) || i + 1 >= n || !tok_ident(&t[i + 1], "hue")) return false;
            out->hue = (uint8_t)h;
            out->hue_given = true;
            i += 2;
        }
        if (i >= n || t[i].kind != TK_COMMA) return false;
        i++;
    }
    unsigned count = 1;
    for (int k = i; k < n; k++) if (t[k].kind == TK_COMMA) count++;
    out->items = p_alloc(P, count * sizeof *out->items);
    out->count = 0;
    int start = i;
    for (int k = i; k <= n; k++) {
        if (k < n && t[k].kind != TK_COMMA) continue;
        int len = k - start;
        if (len < 1 || len > 2) return false;
        struct mix_item *item = &out->items[out->count++];
        const struct tok *color_tok = &t[start], *pct_tok = NULL;
        if (len == 2) {
            /* the percentage may come first or second */
            if (parse_chan(P, &t[start], 1u << MT_PCT, false, &item->pct)) { pct_tok = &t[start]; color_tok = &t[start + 1]; }
            else { pct_tok = &t[start + 1]; color_tok = &t[start]; }
            if (pct_tok != &t[start] && !parse_chan(P, pct_tok, 1u << MT_PCT, false, &item->pct)) return false;
            if (item->pct.kind == CH_LEAF && (item->pct.v < 0 || item->pct.v > 100)) return false;
        }
        struct css_color_value *c = cv_new(P);
        if (!cv_parse(P, color_tok, c)) return false;
        item->color = c;
        start = k + 1;
    }
    return out->count >= 1;
}
/* CSS Color 6, 3: color-layers( [ <blend-mode> , ]? <color># ) */
static bool parse_layers(struct pctx *P, const struct tok *ft, struct css_color_value *out) {
    struct tok buf[TOKS_MAX];
    int n;
    const struct tok *t = lx_all(P, ft->in, ft->in_n, buf, &n);
    if (!t || !n) return false;
    memset(out, 0, sizeof *out);
    out->kind = CV_LAYERS;
    int i = 0;
    if (n > 1 && t[0].kind == TK_IDENT && t[1].kind == TK_COMMA)
        for (int k = 0; k < BLEND_COUNT; k++)
            if (tok_ident(&t[0], blend_names[k])) { out->blend = (uint8_t)k; i = 2; break; }
    unsigned count = 1;
    for (int k = i; k < n; k++) count += t[k].kind == TK_COMMA;
    out->items = p_alloc(P, count * sizeof *out->items);
    /* each layer is one <color> token */
    for (int k = i; k <= n; k += 2) {
        if (k >= n || t[k].kind == TK_COMMA || (k + 1 < n && t[k + 1].kind != TK_COMMA) || k + 1 == n - 1) return false;
        struct css_color_value *c = cv_new(P);
        if (!cv_parse(P, &t[k], c)) return false;
        out->items[out->count++].color = c;
        if (k + 1 >= n) break;
    }
    return out->count == count;
}
static bool cv_parse(struct pctx *P, const struct tok *t, struct css_color_value *out) {
    if (++P->depth > 32) return false;
    bool ok = false;
    memset(out, 0, sizeof *out);
    if (t->kind == TK_HASH) ok = parse_hex(t, out);
    else if (t->kind == TK_IDENT) {
        const char *s = TK_NAME(t);
        size_t n = TK_LEN(t);
        int k;
        if (name_is(s, n, "currentcolor")) { out->kind = CV_CURRENT; ok = true; }
        else if (name_is(s, n, "transparent")) {
            out->kind = CV_ABS;
            out->color = css_color_from_argb(0);
            out->name = "transparent";
            ok = true;
        } else if ((k = named_find(s, n)) >= 0) {
            out->kind = CV_ABS;
            out->color = css_color_from_argb(0xFF000000u | named[k].rgb);
            out->name = named[k].name;
            ok = true;
        } else {
            for (int i = 0; i < SYS_COUNT && !ok; i++)
                if (name_is(s, n, system_colors[i].name)) { out->kind = CV_SYSTEM; out->fn = (uint8_t)i; out->name = system_colors[i].name; ok = true; }
            for (size_t i = 0; i < sizeof deprecated_colors / sizeof *deprecated_colors && !ok; i++)
                if (name_is(s, n, deprecated_colors[i].name)) {
                    out->kind = CV_SYSTEM;
                    out->fn = deprecated_colors[i].as;
                    out->name = deprecated_colors[i].name;
                    ok = true;
                }
        }
    } else if (t->kind == TK_FUNC) {
        const char *s = TK_NAME(t);
        size_t n = TK_LEN(t);
        if (name_is(s, n, "rgb") || name_is(s, n, "rgba")) ok = parse_fn(P, F_RGB, t, out);
        else if (name_is(s, n, "hsl") || name_is(s, n, "hsla")) ok = parse_fn(P, F_HSL, t, out);
        else if (name_is(s, n, "hwb")) ok = parse_fn(P, F_HWB, t, out);
        else if (name_is(s, n, "lab")) ok = parse_fn(P, F_LAB, t, out);
        else if (name_is(s, n, "lch")) ok = parse_fn(P, F_LCH, t, out);
        else if (name_is(s, n, "oklab")) ok = parse_fn(P, F_OKLAB, t, out);
        else if (name_is(s, n, "oklch")) ok = parse_fn(P, F_OKLCH, t, out);
        else if (name_is(s, n, "color")) ok = parse_fn(P, F_COLOR, t, out);
        else if (name_is(s, n, "alpha")) ok = parse_fn(P, F_ALPHA, t, out);
        else if (name_is(s, n, "color-mix")) ok = parse_mix(P, t, out);
        else if (name_is(s, n, "color-layers")) ok = parse_layers(P, t, out);
        else if (name_is(s, n, "light-dark") || name_is(s, n, "contrast-color")) {
            struct tok buf[TOKS_MAX];
            int c;
            bool pair = name_is(s, n, "light-dark");
            const struct tok *k = lx_all(P, t->in, t->in_n, buf, &c);
            if (k && c == (pair ? 3 : 1) && (!pair || k[1].kind == TK_COMMA)) {
                out->kind = pair ? CV_LIGHT_DARK : CV_CONTRAST;
                struct css_color_value *a = cv_new(P), *b = pair ? cv_new(P) : NULL;
                ok = cv_parse(P, &k[0], a) && (!pair || cv_parse(P, &k[2], b));
                out->pair[0] = a;
                out->pair[1] = b;
            }
        }
    }
    P->depth--;
    return ok;
}

static bool color_parse(arena_t *a, const char *s, size_t n, struct css_color_value *out) {
    struct tok t[2];
    struct lex l = {s, s + n};
    lx_next(&l, &t[0]);
    lx_next(&l, &t[1]);
    if (t[0].kind == TK_EOF || t[1].kind != TK_EOF) return false;
    struct pctx P = {.a = a};
    return cv_parse(&P, &t[0], out);
}

/* ---------------------------------------------------------------- conversions (CSS Color 4, 11 and 17) */
static void mul3(const double m[3][3], const double v[3], double out[3]) {
    double r[3];
    for (int i = 0; i < 3; i++) r[i] = m[i][0] * v[0] + m[i][1] * v[1] + m[i][2] * v[2];
    memcpy(out, r, sizeof r);
}
static const double D50[3] = {0.3457 / 0.3585, 1.0, (1.0 - 0.3457 - 0.3585) / 0.3585};
static const double M_SRGB[3][3] = {{506752.0 / 1228815, 87881.0 / 245763, 12673.0 / 70218},
                                    {87098.0 / 409605, 175762.0 / 245763, 12673.0 / 175545},
                                    {7918.0 / 409605, 87881.0 / 737289, 1001167.0 / 1053270}};
static const double M_SRGB_INV[3][3] = {{12831.0 / 3959, -329.0 / 214, -1974.0 / 3959},
                                        {-851781.0 / 878810, 1648619.0 / 878810, 36519.0 / 878810},
                                        {705.0 / 12673, -2585.0 / 12673, 705.0 / 667}};
static const double M_P3[3][3] = {{608311.0 / 1250200, 189793.0 / 714400, 198249.0 / 1000160},
                                  {35783.0 / 156275, 247089.0 / 357200, 198249.0 / 2500400},
                                  {0, 32229.0 / 714400, 5220557.0 / 5000800}};
static const double M_P3_INV[3][3] = {{446124.0 / 178915, -333277.0 / 357830, -72051.0 / 178915},
                                      {-14852.0 / 17905, 63121.0 / 35810, 423.0 / 17905},
                                      {11844.0 / 330415, -50337.0 / 660830, 316169.0 / 330415}};
static const double M_PRO[3][3] = {{0.79776664490064230, 0.13518129740053308, 0.03134773412839220},
                                   {0.28807482881940130, 0.71183523424187300, 0.00008993693872564},
                                   {0.0, 0.0, 0.82510460251046020}};
static const double M_PRO_INV[3][3] = {{1.34578688164715830, -0.25557208737979464, -0.05110186497554526},
                                       {-0.54463070512490190, 1.50824774284514680, 0.02052744743642139},
                                       {0.0, 0.0, 1.21196754563894520}};
static const double M_A98[3][3] = {{573536.0 / 994567, 263643.0 / 1420810, 187206.0 / 994567},
                                   {591459.0 / 1989134, 6239551.0 / 9945670, 374412.0 / 4972835},
                                   {53769.0 / 1989134, 351524.0 / 4972835, 4929758.0 / 4972835}};
static const double M_A98_INV[3][3] = {{1829569.0 / 896150, -506331.0 / 896150, -308931.0 / 896150},
                                       {-851781.0 / 878810, 1648619.0 / 878810, 36519.0 / 878810},
                                       {16779.0 / 1248040, -147721.0 / 1248040, 1266979.0 / 1248040}};
static const double M_2020[3][3] = {{63426534.0 / 99577255, 20160776.0 / 139408157, 47086771.0 / 278816314},
                                    {26158966.0 / 99577255, 472592308.0 / 697040785, 8267143.0 / 139408157},
                                    {0, 19567812.0 / 697040785, 295819943.0 / 278816314}};
static const double M_2020_INV[3][3] = {{30757411.0 / 17917100, -6372589.0 / 17917100, -4539589.0 / 17917100},
                                        {-19765991.0 / 29648200, 47925759.0 / 29648200, 467509.0 / 29648200},
                                        {792561.0 / 44930125, -1921689.0 / 44930125, 42328811.0 / 44930125}};
static const double M_D65_D50[3][3] = {{1.0479297925449969, 0.022946870601609652, -0.05019226628920524},
                                       {0.02962780877005599, 0.9904344267538799, -0.017073799063418826},
                                       {-0.009243040646204504, 0.015055191490298152, 0.7518742814281371}};
static const double M_D50_D65[3][3] = {{0.955473421488075, -0.02309845494876471, 0.06325924320057072},
                                       {-0.0283697093338637, 1.0099953980813041, 0.021041441191917323},
                                       {0.012314014864481998, -0.020507649298898964, 1.330365926242124}};
static const double M_XYZ_LMS[3][3] = {{0.8190224379967030, 0.3619062600528904, -0.1288737815209879},
                                       {0.0329836539323885, 0.9292868615863434, 0.0361446663506424},
                                       {0.0481771893596242, 0.2642395317527308, 0.6335478284694309}};
static const double M_LMS_OKLAB[3][3] = {{0.2104542683093140, 0.7936177747023054, -0.0040720430116193},
                                         {1.9779985324311684, -2.4285922420485799, 0.4505937096174110},
                                         {0.0259040424655478, 0.7827717124575296, -0.8086757549230774}};
static const double M_LMS_XYZ[3][3] = {{1.2268798758459243, -0.5578149944602171, 0.2813910456659647},
                                       {-0.0405757452148008, 1.1122868032803170, -0.0717110580655164},
                                       {-0.0763729366746601, -0.4214933324022432, 1.5869240198367816}};
static const double M_OKLAB_LMS[3][3] = {{1.0, 0.3963377773761749, 0.2158037573099136},
                                         {1.0, -0.1055613458156586, -0.0638541728258133},
                                         {1.0, -0.0894841775298119, -1.2914855480194092}};

static double lin_srgb(double v) { double a = fabs(v); return a <= 0.04045 ? v / 12.92 : copysign(pow((a + 0.055) / 1.055, 2.4), v); }
static double gam_srgb(double v) { double a = fabs(v); return a > 0.0031308 ? copysign(1.055 * pow(a, 1 / 2.4) - 0.055, v) : 12.92 * v; }
static double lin_pro(double v) { double a = fabs(v); return a <= 16.0 / 512 ? v / 16 : copysign(pow(a, 1.8), v); }
static double gam_pro(double v) { double a = fabs(v); return a >= 1.0 / 512 ? copysign(pow(a, 1 / 1.8), v) : 16 * v; }
static double lin_a98(double v) { return copysign(pow(fabs(v), 563.0 / 256), v); }
static double gam_a98(double v) { return copysign(pow(fabs(v), 256.0 / 563), v); }
static double lin_2020(double v) { return copysign(pow(fabs(v), 2.4), v); }
static double gam_2020(double v) { return copysign(pow(fabs(v), 1 / 2.4), v); }
static void each(double v[3], double (*f)(double)) { for (int i = 0; i < 3; i++) v[i] = f(v[i]); }

static const double LAB_E = 216.0 / 24389, LAB_K = 24389.0 / 27;
static void xyz50_to_lab(const double xyz[3], double out[3]) {
    double f[3];
    for (int i = 0; i < 3; i++) {
        double v = xyz[i] / D50[i];
        f[i] = v > LAB_E ? cbrt(v) : (LAB_K * v + 16) / 116;
    }
    out[0] = 116 * f[1] - 16;
    out[1] = 500 * (f[0] - f[1]);
    out[2] = 200 * (f[1] - f[2]);
}
static void lab_to_xyz50(const double lab[3], double out[3]) {
    double f1 = (lab[0] + 16) / 116, f0 = lab[1] / 500 + f1, f2 = f1 - lab[2] / 200;
    double x = f0 * f0 * f0 > LAB_E ? f0 * f0 * f0 : (116 * f0 - 16) / LAB_K;
    double y = lab[0] > LAB_K * LAB_E ? f1 * f1 * f1 : lab[0] / LAB_K;
    double z = f2 * f2 * f2 > LAB_E ? f2 * f2 * f2 : (116 * f2 - 16) / LAB_K;
    out[0] = x * D50[0];
    out[1] = y * D50[1];
    out[2] = z * D50[2];
}
static void rect_to_polar(const double in[3], double out[3], double epsilon) {
    double c = sqrt(in[1] * in[1] + in[2] * in[2]);
    double h = atan2(in[2], in[1]) * 180 / M_PI;
    if (h < 0) h += 360;
    out[0] = in[0];
    out[1] = c;
    out[2] = c <= epsilon ? NAN : h;
}
static void polar_to_rect(const double in[3], double out[3]) {
    double h = in[2] * M_PI / 180;
    double l = in[0], c = in[1];
    out[0] = l;
    out[1] = c * cos(h);
    out[2] = c * sin(h);
}
static void hsl_to_srgb(const double hsl[3], double out[3]) {
    double h = hue_normal(hsl[0]), s = hsl[1] / 100, l = hsl[2] / 100;
    double a = s * (l < 1 - l ? l : 1 - l);
    static const int ns[3] = {0, 8, 4};
    for (int i = 0; i < 3; i++) {
        double k = fmod(ns[i] + h / 30, 12);
        double m = k - 3 < 9 - k ? k - 3 : 9 - k;
        if (m > 1) m = 1;
        if (m < -1) m = -1;
        out[i] = l - a * m;
    }
}
/* rgbToHue: the hue of sRGB components, NaN for a grey. A spread at the
   level of the conversions' rounding (white through Lab, say) is a grey. */
static double rgb_hue(const double rgb[3], double *spread) {
    double r = rgb[0], g = rgb[1], b = rgb[2];
    double max = fmax(fmax(r, g), b), min = fmin(fmin(r, g), b), d = max - min;
    *spread = d;
    if (!(d > 1e-12)) { *spread = 0; return NAN; }
    double h = max == r ? (g - b) / d + (g < b ? 6 : 0) : max == g ? (b - r) / d + 2 : (r - g) / d + 4;
    h *= 60;
    return h >= 360 ? h - 360 : h;
}
static void srgb_to_hsl(const double rgb[3], double out[3]) {
    double max = fmax(fmax(rgb[0], rgb[1]), rgb[2]), min = fmin(fmin(rgb[0], rgb[1]), rgb[2]);
    double d, h = rgb_hue(rgb, &d), s = 0, l = (min + max) / 2;
    if (d != 0) s = l == 0 || l == 1 ? 0 : (max - l) / fmin(l, 1 - l);
    /* very out of gamut colours: a negative saturation turns the hue around */
    if (s < 0) {
        h += 180;
        s = fabs(s);
        if (h >= 360) h -= 360;
    }
    if (s <= 1e-5) { h = NAN; s = 0; }
    out[0] = h;
    out[1] = s * 100;
    out[2] = l * 100;
}
static void hwb_to_srgb(const double hwb[3], double out[3]) {
    double w = hwb[1], b = hwb[2]; /* percentages: whole numbers stay exact */
    if (w + b >= 100) {
        double gray = w / (w + b);
        out[0] = out[1] = out[2] = gray;
        return;
    }
    double pure[3], full[3] = {hwb[0], 100, 50};
    hsl_to_srgb(full, pure);
    for (int i = 0; i < 3; i++) out[i] = (pure[i] * (100 - w - b) + w) / 100;
}
static void srgb_to_hwb(const double rgb[3], double out[3]) {
    double d, h = rgb_hue(rgb, &d);
    double w = fmin(fmin(rgb[0], rgb[1]), rgb[2]), b = 1 - fmax(fmax(rgb[0], rgb[1]), rgb[2]);
    if (w + b >= 1 - 1e-5) h = NAN;
    out[0] = h;
    out[1] = w * 100;
    out[2] = b * 100;
}
/* the colour's components (missing ones zero) in CIE XYZ D65 */
static void to_xyz65(const struct css_color *c, double out[3]) {
    double v[3] = {c->c[0], c->c[1], c->c[2]};
    for (int i = 0; i < 3; i++) if (c->none & 1u << i) v[i] = 0;
    switch (c->space) {
    case CSS_SPACE_HSL: hsl_to_srgb(v, v); /* fall through */
    case CSS_SPACE_SRGB: each(v, lin_srgb); /* fall through */
    case CSS_SPACE_SRGB_LINEAR: mul3(M_SRGB, v, out); return;
    case CSS_SPACE_HWB: hwb_to_srgb(v, v); each(v, lin_srgb); mul3(M_SRGB, v, out); return;
    case CSS_SPACE_DISPLAY_P3: each(v, lin_srgb); /* fall through */
    case CSS_SPACE_DISPLAY_P3_LINEAR: mul3(M_P3, v, out); return;
    case CSS_SPACE_A98_RGB: each(v, lin_a98); mul3(M_A98, v, out); return;
    case CSS_SPACE_PROPHOTO_RGB: each(v, lin_pro); mul3(M_PRO, v, v); mul3(M_D50_D65, v, out); return;
    case CSS_SPACE_REC2020: each(v, lin_2020); mul3(M_2020, v, out); return;
    case CSS_SPACE_XYZ_D50: mul3(M_D50_D65, v, out); return;
    case CSS_SPACE_XYZ_D65: memcpy(out, v, sizeof v); return;
    case CSS_SPACE_LCH: polar_to_rect(v, v); /* fall through */
    case CSS_SPACE_LAB: lab_to_xyz50(v, v); mul3(M_D50_D65, v, out); return;
    case CSS_SPACE_OKLCH: polar_to_rect(v, v); /* fall through */
    case CSS_SPACE_OKLAB:
        mul3(M_OKLAB_LMS, v, v);
        for (int i = 0; i < 3; i++) v[i] = v[i] * v[i] * v[i];
        mul3(M_LMS_XYZ, v, out);
        return;
    }
    memcpy(out, v, sizeof v);
}
static void from_xyz65(uint8_t space, const double xyz[3], double out[3]) {
    double v[3] = {xyz[0], xyz[1], xyz[2]};
    switch (space) {
    case CSS_SPACE_SRGB: case CSS_SPACE_HSL: case CSS_SPACE_HWB:
        mul3(M_SRGB_INV, v, v);
        each(v, gam_srgb);
        if (space == CSS_SPACE_HSL) srgb_to_hsl(v, v);
        else if (space == CSS_SPACE_HWB) srgb_to_hwb(v, v);
        break;
    case CSS_SPACE_SRGB_LINEAR: mul3(M_SRGB_INV, v, v); break;
    case CSS_SPACE_DISPLAY_P3: mul3(M_P3_INV, v, v); each(v, gam_srgb); break;
    case CSS_SPACE_DISPLAY_P3_LINEAR: mul3(M_P3_INV, v, v); break;
    case CSS_SPACE_A98_RGB: mul3(M_A98_INV, v, v); each(v, gam_a98); break;
    case CSS_SPACE_PROPHOTO_RGB: mul3(M_D65_D50, v, v); mul3(M_PRO_INV, v, v); each(v, gam_pro); break;
    case CSS_SPACE_REC2020: mul3(M_2020_INV, v, v); each(v, gam_2020); break;
    case CSS_SPACE_XYZ_D50: mul3(M_D65_D50, v, v); break;
    case CSS_SPACE_XYZ_D65: break;
    case CSS_SPACE_LAB: case CSS_SPACE_LCH:
        mul3(M_D65_D50, v, v);
        xyz50_to_lab(v, v);
        if (space == CSS_SPACE_LCH) rect_to_polar(v, v, 0.0015);
        break;
    case CSS_SPACE_OKLAB: case CSS_SPACE_OKLCH:
        mul3(M_XYZ_LMS, v, v);
        for (int i = 0; i < 3; i++) v[i] = cbrt(v[i]);
        mul3(M_LMS_OKLAB, v, v);
        if (space == CSS_SPACE_OKLCH) rect_to_polar(v, v, 0.000004);
        break;
    }
    memcpy(out, v, sizeof v);
}
/* CSS Color 4, 11.2: missing components are zero; a hue the result cannot
   have (an achromatic colour) is missing */
static struct css_color convert(const struct css_color *c, uint8_t space) {
    if (c->space == space) return *c;
    double xyz[3];
    struct css_color out = {{0, 0, 0}, c->alpha, space, (uint8_t)(c->none & CSS_COLOR_NONE_ALPHA), false};
    if (c->space <= CSS_SPACE_SRGB_LINEAR && space <= CSS_SPACE_SRGB_LINEAR) {
        /* sRGB and its linear form: no trip through XYZ */
        for (int i = 0; i < 3; i++) {
            double v = c->none & 1u << i ? 0 : c->c[i];
            out.c[i] = space == CSS_SPACE_SRGB_LINEAR ? lin_srgb(v) : gam_srgb(v);
        }
        return out;
    }
    if ((c->space == CSS_SPACE_SRGB || c->space == CSS_SPACE_HSL || c->space == CSS_SPACE_HWB) &&
        (space == CSS_SPACE_SRGB || space == CSS_SPACE_HSL || space == CSS_SPACE_HWB)) {
        double v[3] = {c->c[0], c->c[1], c->c[2]};
        for (int i = 0; i < 3; i++) if (c->none & 1u << i) v[i] = 0;
        if (c->space == CSS_SPACE_HSL) hsl_to_srgb(v, v);
        else if (c->space == CSS_SPACE_HWB) hwb_to_srgb(v, v);
        if (space == CSS_SPACE_HSL) srgb_to_hsl(v, v);
        else if (space == CSS_SPACE_HWB) srgb_to_hwb(v, v);
        memcpy(out.c, v, sizeof v);
    } else {
        to_xyz65(c, xyz);
        from_xyz65(space, xyz, out.c);
    }
    int h = space_hue(space);
    if (h >= 0 && isnan(out.c[h])) {
        out.c[h] = 0;
        out.none |= (uint8_t)(1u << h);
        /* LCh and OkLCh: a chroma within epsilon of zero is zero, the hue powerless */
        if (h == 2) out.c[1] = 0;
    }
    return out;
}

/* ---------------------------------------------------------------- missing and powerless components (13.2, 13.3) */
enum { K_NONE, K_RED, K_GREEN, K_BLUE, K_LIGHT, K_COLORFUL, K_HUE, K_OPP_A, K_OPP_B };
static const uint8_t categories[CSS_SPACES][3] = {
    [CSS_SPACE_SRGB] = {K_RED, K_GREEN, K_BLUE}, [CSS_SPACE_SRGB_LINEAR] = {K_RED, K_GREEN, K_BLUE},
    [CSS_SPACE_DISPLAY_P3] = {K_RED, K_GREEN, K_BLUE}, [CSS_SPACE_DISPLAY_P3_LINEAR] = {K_RED, K_GREEN, K_BLUE},
    [CSS_SPACE_A98_RGB] = {K_RED, K_GREEN, K_BLUE}, [CSS_SPACE_PROPHOTO_RGB] = {K_RED, K_GREEN, K_BLUE},
    [CSS_SPACE_REC2020] = {K_RED, K_GREEN, K_BLUE}, [CSS_SPACE_XYZ_D50] = {K_RED, K_GREEN, K_BLUE},
    [CSS_SPACE_XYZ_D65] = {K_RED, K_GREEN, K_BLUE}, [CSS_SPACE_LAB] = {K_LIGHT, K_OPP_A, K_OPP_B},
    [CSS_SPACE_LCH] = {K_LIGHT, K_COLORFUL, K_HUE}, [CSS_SPACE_OKLAB] = {K_LIGHT, K_OPP_A, K_OPP_B},
    [CSS_SPACE_OKLCH] = {K_LIGHT, K_COLORFUL, K_HUE}, [CSS_SPACE_HSL] = {K_HUE, K_COLORFUL, K_LIGHT},
    [CSS_SPACE_HWB] = {K_HUE, K_NONE, K_NONE},
};
/* the components of space dst that carry forward c's missing ones */
static uint8_t carried(const struct css_color *c, uint8_t dst) {
    const uint8_t *from = categories[c->space], *to = categories[dst];
    uint8_t mask = c->none & CSS_COLOR_NONE_ALPHA;
    unsigned src_rest = 0, dst_rest = 0;
    for (int i = 0; i < 3; i++) {
        bool match = false;
        for (int j = 0; j < 3; j++)
            if (from[i] != K_NONE && from[i] == to[j]) {
                match = true;
                if (c->none & 1u << i) mask |= (uint8_t)(1u << j);
            }
        if (!match) src_rest |= 1u << i;
    }
    for (int j = 0; j < 3; j++) {
        bool match = false;
        for (int i = 0; i < 3; i++) if (to[j] != K_NONE && to[j] == from[i]) match = true;
        if (!match) dst_rest |= 1u << j;
    }
    /* the components left without an analogue carry forward as a set */
    if (src_rest && (c->none & src_rest) == src_rest) mask |= (uint8_t)dst_rest;
    return mask;
}
static void powerless_to_missing(struct css_color *c) {
    switch (c->space) {
    case CSS_SPACE_HSL:
        if (!(c->none & 2) && fabs(c->c[1]) <= 0.001) c->none |= 1;
        break;
    case CSS_SPACE_HWB: {
        double w = c->none & 2 ? 0 : c->c[1], b = c->none & 4 ? 0 : c->c[2];
        if (w + b >= 99.999) {
            c->none |= 1;
            if (w + b < 100) {
                if (!(c->none & 2) && !(c->none & 4)) c->c[2] = 100 - c->c[1];
                else if (!(c->none & 2)) c->c[1] = 100;
                else if (!(c->none & 4)) c->c[2] = 100;
            }
        }
        break;
    }
    case CSS_SPACE_LCH: case CSS_SPACE_OKLCH:
        if (!(c->none & 2) && c->c[1] <= (c->space == CSS_SPACE_LCH ? 0.0015 : 0.000004)) {
            c->none |= 4;
            c->c[1] = 0;
        }
        break;
    }
}
/* conversion with powerless components made missing first and missing
   components carried forward (13.3; relative colours, 4.1 of CSS Color 5) */
static struct css_color convert_carry(const struct css_color *c, uint8_t space) {
    if (c->space == space) return *c;
    struct css_color x = *c;
    powerless_to_missing(&x);
    uint8_t mask = carried(&x, space);
    struct css_color out = convert(&x, space);
    out.none |= mask;
    for (int i = 0; i < 3; i++) if (out.none & 1u << i) out.c[i] = 0;
    if (out.none & CSS_COLOR_NONE_ALPHA) out.alpha = 0;
    return out;
}

/* ---------------------------------------------------------------- gamut mapping (CSS Color 4, 13.2.x) */
static bool in_gamut(const double v[3]) {
    for (int i = 0; i < 3; i++) if (!(v[i] >= -1e-6 && v[i] <= 1 + 1e-6)) return false;
    return true;
}
static double delta_eok(const struct css_color *a, const struct css_color *b) {
    struct css_color x = convert(a, CSS_SPACE_OKLAB), y = convert(b, CSS_SPACE_OKLAB);
    double d0 = x.c[0] - y.c[0], d1 = x.c[1] - y.c[1], d2 = x.c[2] - y.c[2];
    return sqrt(d0 * d0 + d1 * d1 + d2 * d2);
}
/* CSS gamut mapping into sRGB: binary search of OkLCh chroma with local MINDE */
static void gamut_map_srgb(const struct css_color *c, double out[3]) {
    struct css_color origin = *c;
    for (int i = 0; i < 3; i++) if (origin.none & 1u << i) origin.c[i] = 0;
    origin.none &= CSS_COLOR_NONE_ALPHA;
    struct css_color srgb = convert(&origin, CSS_SPACE_SRGB);
    if (in_gamut(srgb.c)) { memcpy(out, srgb.c, sizeof srgb.c); return; }
    struct css_color lch = convert(&origin, CSS_SPACE_OKLCH);
    lch.none = 0;
    if (lch.c[0] >= 1) { out[0] = out[1] = out[2] = 1; return; }
    if (!(lch.c[0] > 0)) { out[0] = out[1] = out[2] = 0; return; }
    /* calc(infinity) components: the search starts from a chroma beyond every gamut */
    if (!isfinite(lch.c[2])) lch.c[2] = 0;
    if (isnan(lch.c[1])) lch.c[1] = 0;
    else if (isinf(lch.c[1])) lch.c[1] = 1;
    if (!isfinite(srgb.c[0]) || !isfinite(srgb.c[1]) || !isfinite(srgb.c[2])) srgb = convert(&lch, CSS_SPACE_SRGB);
    const double jnd = 0.02, epsilon = 0.0001;
    struct css_color clipped = srgb;
    for (int i = 0; i < 3; i++) clipped.c[i] = clipped.c[i] < 0 ? 0 : clipped.c[i] > 1 ? 1 : clipped.c[i];
    if (delta_eok(&clipped, &lch) < jnd) { memcpy(out, clipped.c, sizeof clipped.c); return; }
    double min = 0, max = lch.c[1];
    bool min_in_gamut = true;
    struct css_color current = lch;
    while (max - min > epsilon) {
        current.c[1] = (min + max) / 2;
        struct css_color candidate = convert(&current, CSS_SPACE_SRGB);
        if (min_in_gamut && in_gamut(candidate.c)) { min = current.c[1]; continue; }
        clipped = candidate;
        for (int i = 0; i < 3; i++) clipped.c[i] = clipped.c[i] < 0 ? 0 : clipped.c[i] > 1 ? 1 : clipped.c[i];
        double e = delta_eok(&clipped, &current);
        if (e < jnd) {
            if (jnd - e < epsilon) break;
            min_in_gamut = false;
            min = current.c[1];
        } else max = current.c[1];
    }
    memcpy(out, clipped.c, sizeof clipped.c);
}
static uint32_t byte(double v) {
    if (!(v > 0)) return 0;
    if (v >= 1) return 255;
    return (uint32_t)floor(v * 255 + 0.5);
}
uint32_t css_color_argb(const struct css_color *c) {
    double rgb[3];
    if (c->legacy || c->space == CSS_SPACE_SRGB) {
        struct css_color s = convert(c, CSS_SPACE_SRGB);
        if (c->legacy || in_gamut(s.c)) memcpy(rgb, s.c, sizeof rgb);
        else gamut_map_srgb(c, rgb);
    } else gamut_map_srgb(c, rgb);
    double a = c->none & CSS_COLOR_NONE_ALPHA ? 0 : c->alpha;
    return ARGB(byte(a), byte(rgb[0]), byte(rgb[1]), byte(rgb[2]));
}

/* ---------------------------------------------------------------- interpolation (CSS Color 4, 13) */
struct css_color_interpolation css_color_default_interpolation(const struct css_color *a, const struct css_color *b) {
    struct css_color_interpolation m = {CSS_SPACE_OKLAB, CSS_HUE_SHORTER, false};
    if (a->legacy && b->legacy) { m.space = CSS_SPACE_SRGB; m.legacy = true; }
    return m;
}
static struct css_color interpolate(const struct css_color *a, const struct css_color *b, double p,
                                    const struct css_color_interpolation *m) {
    uint8_t space = m->space;
    /* 1-4: convert into the interpolation space, carrying missing components forward */
    struct css_color x = convert_carry(a, space), y = convert_carry(b, space);
    /* 5: a component missing in only one colour takes the other's value */
    for (int i = 0; i < 4; i++) {
        uint8_t bit = i == 3 ? CSS_COLOR_NONE_ALPHA : (uint8_t)(1u << i);
        double *xv = i == 3 ? &x.alpha : &x.c[i], *yv = i == 3 ? &y.alpha : &y.c[i];
        if ((x.none & bit) && !(y.none & bit)) { *xv = *yv; x.none &= (uint8_t)~bit; }
        else if ((y.none & bit) && !(x.none & bit)) { *yv = *xv; y.none &= (uint8_t)~bit; }
    }
    /* 6: hue fixup */
    int h = space_hue(space);
    if (h >= 0 && !(x.none & 1u << h)) {
        double t1 = hue_normal(x.c[h]), t2 = hue_normal(y.c[h]), d = t2 - t1;
        switch (m->hue) {
        case CSS_HUE_SHORTER: if (d > 180) t1 += 360; else if (d < -180) t2 += 360; break;
        case CSS_HUE_LONGER: if (0 < d && d < 180) t1 += 360; else if (-180 < d && d <= 0) t2 += 360; break;
        case CSS_HUE_INCREASING: if (d < 0) t2 += 360; break;
        case CSS_HUE_DECREASING: if (0 < d) t1 += 360; break;
        }
        x.c[h] = t1;
        y.c[h] = t2;
    }
    /* 7: premultiply (not the hue) unless alpha is missing */
    bool premultiply = !(x.none & CSS_COLOR_NONE_ALPHA);
    if (premultiply)
        for (int i = 0; i < 3; i++) {
            if (i == h || (x.none & 1u << i)) continue;
            x.c[i] *= x.alpha;
            y.c[i] *= y.alpha;
        }
    /* 8: linear interpolation */
    struct css_color r = {{0, 0, 0}, 0, space, (uint8_t)(x.none & y.none), m->legacy};
    for (int i = 0; i < 3; i++)
        if (!(r.none & 1u << i)) r.c[i] = x.c[i] + (y.c[i] - x.c[i]) * p;
    if (!(r.none & CSS_COLOR_NONE_ALPHA)) r.alpha = x.alpha + (y.alpha - x.alpha) * p;
    if (r.alpha < 0) r.alpha = 0;
    if (r.alpha > 1) r.alpha = 1;
    /* 9: undo premultiplication */
    if (premultiply && r.alpha != 0)
        for (int i = 0; i < 3; i++)
            if (i != h && !(r.none & 1u << i)) r.c[i] /= r.alpha;
    if (h >= 0 && !(r.none & 1u << h)) r.c[h] = hue_normal(r.c[h]);
    if (m->legacy) {
        /* legacy sRGB results stay in the legacy 8-bit gamut */
        for (int i = 0; i < 3; i++) r.c[i] = r.c[i] < 0 ? 0 : r.c[i] > 1 ? 1 : r.c[i];
    }
    return r;
}
/* animation: an interpolated value outside a property's range is clamped to it (CSS Values 4,
   2.4.1), here lightness and chroma; color-mix() keeps what interpolation gives */
struct css_color css_color_interpolate(const struct css_color *a, const struct css_color *b, double p,
                                       const struct css_color_interpolation *m) {
    struct css_color r = interpolate(a, b, p, m);
    double max_l = r.space == CSS_SPACE_LAB || r.space == CSS_SPACE_LCH ? 100 :
                   r.space == CSS_SPACE_OKLAB || r.space == CSS_SPACE_OKLCH ? 1 : 0;
    if (max_l) r.c[0] = r.c[0] < 0 ? 0 : r.c[0] > max_l ? max_l : r.c[0];
    if ((r.space == CSS_SPACE_LCH || r.space == CSS_SPACE_OKLCH) && r.c[1] < 0) r.c[1] = 0;
    return r;
}
/* one colour of a color-mix(): its value, its percentage (NAN: omitted) and, while it
   names currentcolor, its expression */
struct mixed { struct css_color color; double pct; const struct css_color_value *tree; };
static double pct_clamp(double v) { return v < 0 ? 0 : v > 100 ? 100 : v; }
/* CSS Color 5, 3: color-mix() of absolute colours with their percentages */
static struct css_color mix_colors(const struct mixed *items, unsigned count, const struct css_color_interpolation *m) {
    double sum = 0;
    unsigned omitted = 0;
    for (unsigned i = 0; i < count; i++) {
        if (isnan(items[i].pct)) omitted++;
        else sum += pct_clamp(items[i].pct);
    }
    double fill = omitted ? (sum >= 100 ? 0 : (100 - sum) / omitted) : 0, total = sum + fill * omitted;
    double leftover = 100 - total;
    double alpha_mult = leftover > 0 ? 1 - leftover / 100 : 1, scale = total > 0 ? 100 / total : 1;
#define GIVEN(i) ((isnan(items[i].pct) ? fill : pct_clamp(items[i].pct)) * scale)
    struct css_color result = convert_carry(&items[0].color, m->space);
    result.legacy = false;
    double weight = GIVEN(0);
    for (unsigned i = 1; i < count; i++) {
        double w = weight + GIVEN(i);
        double progress = w > 0 ? GIVEN(i) / w : 0.5;
        result = interpolate(&result, &items[i].color, progress, m);
        weight = w;
    }
#undef GIVEN
    result.legacy = false;
    if (!(result.none & CSS_COLOR_NONE_ALPHA)) result.alpha *= alpha_mult;
    return result;
}

/* Compositing 1, 10.1: a separable blend mode B(Cb, Cs) */
static double blend_hard_light(double b, double s) { return s <= 0.5 ? b * 2 * s : b + (2 * s - 1) - b * (2 * s - 1); }
static double blend_one(uint8_t mode, double b, double s) {
    switch (mode) {
    case BLEND_MULTIPLY: return b * s;
    case BLEND_SCREEN: return b + s - b * s;
    case BLEND_OVERLAY: return blend_hard_light(s, b);
    case BLEND_DARKEN: return b < s ? b : s;
    case BLEND_LIGHTEN: return b > s ? b : s;
    case BLEND_COLOR_DODGE: return b == 0 ? 0 : s == 1 ? 1 : fmin(1, b / (1 - s));
    case BLEND_COLOR_BURN: return b == 1 ? 1 : s == 0 ? 0 : 1 - fmin(1, (1 - b) / s);
    case BLEND_HARD_LIGHT: return blend_hard_light(b, s);
    case BLEND_SOFT_LIGHT: {
        if (s <= 0.5) return b - (1 - 2 * s) * b * (1 - b);
        double d = b <= 0.25 ? ((16 * b - 12) * b + 4) * b : sqrt(b);
        return b + (2 * s - 1) * (d - b);
    }
    case BLEND_DIFFERENCE: return fabs(b - s);
    case BLEND_EXCLUSION: return b + s - 2 * b * s;
    default: return s;
    }
}
/* 10.2: the non-separable modes */
static double lum(const double c[3]) { return 0.3 * c[0] + 0.59 * c[1] + 0.11 * c[2]; }
static void set_lum(double c[3], double l) {
    double d = l - lum(c);
    for (int i = 0; i < 3; i++) c[i] += d;
    /* ClipColor */
    double L = lum(c), lo = fmin(c[0], fmin(c[1], c[2])), hi = fmax(c[0], fmax(c[1], c[2]));
    for (int i = 0; i < 3; i++) {
        if (lo < 0) c[i] = L + (c[i] - L) * L / (L - lo);
        if (hi > 1) c[i] = L + (c[i] - L) * (1 - L) / (hi - L);
    }
}
static double sat(const double c[3]) { return fmax(c[0], fmax(c[1], c[2])) - fmin(c[0], fmin(c[1], c[2])); }
static void set_sat(double c[3], double s) {
    int lo = 0, hi = 0;
    for (int i = 1; i < 3; i++) { if (c[i] < c[lo]) lo = i; if (c[i] >= c[hi]) hi = i; }
    if (lo == hi) hi = (lo + 1) % 3;
    int mid = 3 - lo - hi;
    if (c[hi] > c[lo]) {
        c[mid] = (c[mid] - c[lo]) * s / (c[hi] - c[lo]);
        c[hi] = s;
    } else c[mid] = c[hi] = 0;
    c[lo] = 0;
}
static void blend(uint8_t mode, const double b[3], const double s[3], double out[3]) {
    memcpy(out, mode == BLEND_SATURATION || mode == BLEND_LUMINOSITY ? b : s, 3 * sizeof *out);
    switch (mode) {
    case BLEND_HUE: set_sat(out, sat(b)); set_lum(out, lum(b)); return;
    case BLEND_SATURATION: set_sat(out, sat(s)); set_lum(out, lum(b)); return;
    case BLEND_COLOR: set_lum(out, lum(b)); return;
    case BLEND_LUMINOSITY: set_lum(out, lum(s)); return;
    }
    for (int i = 0; i < 3; i++) out[i] = blend_one(mode, b[i], s[i]);
}
/* CSS Color 6, 3: the layers composited from the bottom (the last) up with the blend mode and
   source-over (Compositing 1, 5.8, 9.1.4), in sRGB, each colour clamped to it */
static struct css_color layers_composite(const struct mixed *items, unsigned count, uint8_t mode) {
    double cb[3] = {0, 0, 0}, ab = 0;
    bool legacy = true;
    for (unsigned i = count; i-- > 0;) {
        struct css_color s = convert(&items[i].color, CSS_SPACE_SRGB);
        legacy &= items[i].color.legacy;
        double cs[3], as = s.none & CSS_COLOR_NONE_ALPHA ? 0 : s.alpha, mixed[3];
        for (int k = 0; k < 3; k++) {
            double v = s.none & 1u << k ? 0 : s.c[k];
            cs[k] = v < 0 ? 0 : v > 1 ? 1 : v;
        }
        blend(mode, cb, cs, mixed);
        double ao = as + ab * (1 - as);
        for (int k = 0; k < 3; k++) {
            double cr = (1 - ab) * cs[k] + ab * mixed[k];
            cb[k] = ao > 0 ? (as * cr + ab * cb[k] * (1 - as)) / ao : 0;
        }
        ab = ao;
    }
    struct css_color r = {{cb[0], cb[1], cb[2]}, ab, CSS_SPACE_SRGB, 0, legacy};
    return r;
}

/* ---------------------------------------------------------------- computing and resolving */
struct cenv {
    const struct cx *cx;              /* lengths; NULL outside an element */
    bool dark;                        /* the used colour scheme */
    const struct css_color *current;  /* NULL: currentcolor stays in the value */
    arena_t *a;                       /* for a computed value that keeps currentcolor */
};
static bool cv_compute(const struct css_color_value *v, const struct cenv *E, struct css_color *out,
                       const struct css_color_value **tree);
static struct css_color contrast_of(const struct css_color *c) {
    double xyz[3];
    struct css_color x = *c;
    to_xyz65(&x, xyz);
    double y = xyz[1];
    double white = 1.05 / (y + 0.05), black = (y + 0.05) / 0.05;
    return css_color_from_argb(white >= black ? 0xFFFFFFFFu : 0xFF000000u);
}
/* a computed value that still names currentcolor: lengths in pixels, the rest absolute */
static bool chan_bind(struct pctx *P, const struct chan *c, const struct cx *cx, struct chan *out) {
    *out = *c;
    if (c->kind == CH_MATH && !(out->m = m_bind(P, c->m, cx))) return false;
    return true;
}
static bool cv_compute(const struct css_color_value *v, const struct cenv *E, struct css_color *out,
                       const struct css_color_value **tree) {
    *tree = NULL;
    switch (v->kind) {
    case CV_ABS: *out = v->color; return true;
    case CV_CURRENT:
        if (E->current) { *out = *E->current; return true; }
        *tree = &current_value;
        return true;
    case CV_SYSTEM: *out = system_color(v->fn, E->dark); return true;
    case CV_LIGHT_DARK: return cv_compute(v->pair[E->dark ? 1 : 0], E, out, tree);
    case CV_CONTRAST: {
        struct css_color c;
        const struct css_color_value *t;
        if (!cv_compute(v->pair[0], E, &c, &t)) return false;
        if (!t) { *out = contrast_of(&c); return true; }
        if (!E->a) return false;
        struct css_color_value *n = ar_alloc(E->a, sizeof *n);
        *n = *v;
        n->pair[0] = t;
        *tree = n;
        return true;
    }
    case CV_FN: {
        struct css_color origin;
        const struct css_color_value *t = NULL;
        if (v->origin && !cv_compute(v->origin, E, &origin, &t)) return false;
        if (!t) { *out = fn_evaluate(v, v->origin ? &origin : NULL, E->cx); return true; }
        if (!E->a) return false;
        struct pctx P = {.a = E->a};
        struct css_color_value *n = ar_alloc(E->a, sizeof *n);
        *n = *v;
        n->origin = t;
        for (int i = 0; i < 4; i++) if (!chan_bind(&P, &v->ch[i], E->cx, &n->ch[i])) return false;
        *tree = n;
        return true;
    }
    case CV_MIX: case CV_LAYERS: {
        struct mixed few[8];
        struct mixed *items = v->count <= 8 ? few : malloc(v->count * sizeof *items);
        if (!items) return false;
        bool dependent = false, ok = true;
        for (unsigned i = 0; i < v->count && ok; i++) {
            if (!(ok = cv_compute(v->items[i].color, E, &items[i].color, &items[i].tree))) break;
            dependent |= items[i].tree != NULL;
            const struct chan *c = &v->items[i].pct;
            items[i].pct = c->kind == CH_ABSENT ? NAN : chan_raw(c, NULL, E->cx);
            if (!isnan(items[i].pct) && !(items[i].pct >= 0)) items[i].pct = 0;
        }
        struct css_color_interpolation m = {v->space, v->hue, false};
        if (ok && !dependent) *out = v->kind == CV_MIX ? mix_colors(items, v->count, &m) : layers_composite(items, v->count, v->blend);
        else if (ok && E->a) {
            struct css_color_value *n = ar_alloc(E->a, sizeof *n);
            *n = *v;
            n->items = ar_alloc(E->a, v->count * sizeof *n->items);
            for (unsigned i = 0; i < v->count; i++) {
                const struct css_color_value *t = items[i].tree;
                struct css_color_value *item = t ? (struct css_color_value *)t : ar_alloc(E->a, sizeof *item);
                if (!t) { item->kind = CV_ABS; item->color = items[i].color; }
                n->items[i].color = item;
                double pct = items[i].pct;
                n->items[i].pct = (struct chan){.kind = isnan(pct) ? CH_ABSENT : CH_LEAF, .type = MT_PCT, .v = pct};
                if (!isnan(pct)) strcpy(n->items[i].pct.unit, "%");
            }
            *tree = n;
        } else ok = false;
        if (items != few) free(items);
        return ok;
    }
    }
    return false;
}

/* ---------------------------------------------------------------- serialization (CSS Color 4, 16; CSS Color 5, 11) */
static void alpha_put(sbuf *b, const struct css_color *c, bool legacy) {
    if (c->none & CSS_COLOR_NONE_ALPHA) { sb_puts(b, " / none"); return; }
    double a = c->alpha < 0 ? 0 : c->alpha > 1 ? 1 : c->alpha;
    if (isnan(a)) a = 0;
    if (a == 1) return;
    sb_puts(b, legacy ? ", " : " / ");
    num_put(b, a);
}
/* a hue in [0, 360): one that six figures would round up to 360 (and read
   back as 0) gets the figures that keep it below */
static void hue_put(sbuf *b, const struct css_color *c, int i) {
    double h = hue_normal(c->c[i]);
    char t[32];
    if (c->none & 1u << i) { sb_puts(b, "none"); return; }
    if (h < 359.9995) { num_put(b, h); return; }
    snprintf(t, sizeof t, "%.10g", h);
    sb_puts(b, strcmp(t, "360") ? t : "0");
}
static void comp_put(sbuf *b, const struct css_color *c, int i, double v, const char *unit) {
    if (c->none & 1u << i) { sb_puts(b, "none"); return; }
    if (isinf(v)) { sb_puts(b, v > 0 ? "calc(infinity)" : "calc(-infinity)"); return; }
    num_put(b, isnan(v) ? 0 : v);
    sb_puts(b, unit);
}
/* the 8-bit legacy alpha (16.1.1) */
static double legacy_alpha(double alpha) {
    unsigned a = byte(alpha);
    /* in integers: two decimals when they round trip through the byte, else three */
    unsigned percent = (a * 100 + 127) / 255;
    if ((percent * 255 + 50) / 100 == a) return percent / 100.0;
    return ((a * 1000 + 127) / 255) / 1000.0;
}
static void abs_put(sbuf *b, const struct css_color *c, bool html) {
    uint8_t s = c->space;
    bool missing = (c->none & 7) != 0 || (c->none & CSS_COLOR_NONE_ALPHA);
    if (c->legacy && !missing && (s == CSS_SPACE_SRGB || s == CSS_SPACE_HSL || s == CSS_SPACE_HWB)) {
        struct css_color x = convert(c, CSS_SPACE_SRGB);
        unsigned r = byte(x.c[0]), g = byte(x.c[1]), bl = byte(x.c[2]), a = byte(c->alpha);
        char t[64];
        if (a == 255) {
            if (html) snprintf(t, sizeof t, "#%02x%02x%02x", r, g, bl);
            else snprintf(t, sizeof t, "rgb(%u, %u, %u)", r, g, bl);
            sb_puts(b, t);
            return;
        }
        snprintf(t, sizeof t, "rgba(%u, %u, %u, ", r, g, bl);
        sb_puts(b, t);
        num_put(b, legacy_alpha(c->alpha));
        sb_putc(b, ')');
        return;
    }
    if ((s == CSS_SPACE_HSL || s == CSS_SPACE_HWB) && missing) {
        sb_puts(b, s == CSS_SPACE_HSL ? "hsl(" : "hwb(");
        hue_put(b, c, 0);
        sb_putc(b, ' ');
        comp_put(b, c, 1, c->c[1], "%");
        sb_putc(b, ' ');
        comp_put(b, c, 2, c->c[2], "%");
        alpha_put(b, c, false);
        sb_putc(b, ')');
        return;
    }
    struct css_color x = *c;
    if (s == CSS_SPACE_HSL || s == CSS_SPACE_HWB) { x = convert(c, CSS_SPACE_SRGB); s = CSS_SPACE_SRGB; }
    if (s <= CSS_SPACE_XYZ_D65) {
        sb_puts(b, "color(");
        sb_puts(b, space_names[s]);
        for (int i = 0; i < 3; i++) { sb_putc(b, ' '); comp_put(b, &x, i, x.c[i], ""); }
    } else {
        sb_puts(b, space_names[s]);
        sb_putc(b, '(');
        /* parsing clamps L and C; mixing and conversion may leave them outside */
        comp_put(b, &x, 0, x.c[0], "");
        sb_putc(b, ' ');
        if (s == CSS_SPACE_LCH || s == CSS_SPACE_OKLCH) {
            comp_put(b, &x, 1, x.c[1], "");
            sb_putc(b, ' ');
            hue_put(b, &x, 2);
        } else {
            comp_put(b, &x, 1, x.c[1], "");
            sb_putc(b, ' ');
            comp_put(b, &x, 2, x.c[2], "");
        }
    }
    alpha_put(b, &x, false);
    sb_putc(b, ')');
}
bool css_color_serialize(const struct css_color *c, bool html, sbuf *out) {
    abs_put(out, c, html);
    return true;
}
/* a channel of a specified value; resolved: the absolute syntax, whose plain values are numbers */
static void chan_put(sbuf *b, const struct css_color_value *v, int i, const char *const *keys) {
    const struct chan *c = &v->ch[i];
    switch (c->kind) {
    case CH_NONE: sb_puts(b, "none"); return;
    case CH_LEAF:
        if (!v->origin) {
            /* an absolute colour kept for its math: its other values are numbers */
            double x = chan_component(v->fn, v->space, i, c->type, chan_raw(c, NULL, NULL), false);
            if (i == 3) num_put(b, x);
            else if (v->fn == F_RGB) num_put(b, x * 255);
            else num_put(b, x);
            return;
        }
        num_put(b, c->v);
        sb_puts(b, c->unit);
        return;
    case CH_MATH:
        /* a channel keyword, or a math function */
        m_put(b, c->m, keys, c->wrapped || c->m->op == MN_FN);
        return;
    }
}
static void spec_put(sbuf *b, const struct css_color_value *v);
static void mix_put(sbuf *b, const struct css_color_value *v) {
    sb_puts(b, "color-mix(");
    if (v->space != CSS_SPACE_OKLAB) {
        sb_puts(b, "in ");
        sb_puts(b, space_names[v->space]);
        if (v->hue != CSS_HUE_SHORTER) { sb_putc(b, ' '); sb_puts(b, hue_names[v->hue]); sb_puts(b, " hue"); }
        sb_puts(b, ", ");
    }
    /* percentages: omitted ones are filled in, unless one is a math function; all are
       omitted when each is the even share */
    bool math = false;
    double sum = 0;
    unsigned omitted = 0;
    for (unsigned i = 0; i < v->count; i++) {
        const struct chan *c = &v->items[i].pct;
        if (c->kind == CH_MATH) math = true;
        if (c->kind == CH_ABSENT) omitted++;
        else if (c->kind == CH_LEAF) sum += c->v;
    }
    double fill = omitted && sum < 100 ? (100 - sum) / omitted : 0;
    double share = 100.0 / v->count;
    bool even = !math;
    for (unsigned i = 0; i < v->count && even; i++) {
        const struct chan *c = &v->items[i].pct;
        double p = c->kind == CH_ABSENT ? fill : c->v;
        if (fabs(p - share) > 1e-9) even = false;
    }
    for (unsigned i = 0; i < v->count; i++) {
        if (i) sb_puts(b, ", ");
        spec_put(b, v->items[i].color);
        const struct chan *c = &v->items[i].pct;
        if (even) continue;
        if (c->kind == CH_ABSENT) {
            if (math) continue;
            sb_putc(b, ' ');
            num_put(b, fill);
            sb_putc(b, '%');
        } else if (c->kind == CH_LEAF) {
            sb_putc(b, ' ');
            num_put(b, c->v);
            sb_putc(b, '%');
        } else {
            sb_putc(b, ' ');
            m_put(b, c->m, NULL, true);
        }
    }
    sb_putc(b, ')');
}
static void spec_put(sbuf *b, const struct css_color_value *v) {
    switch (v->kind) {
    case CV_ABS:
        if (v->name) sb_puts(b, v->name);
        else abs_put(b, &v->color, false);
        return;
    case CV_CURRENT: sb_puts(b, "currentcolor"); return;
    case CV_SYSTEM: sb_puts(b, v->name ? v->name : system_colors[v->fn].name); return;
    case CV_LIGHT_DARK:
        sb_puts(b, "light-dark(");
        spec_put(b, v->pair[0]);
        sb_puts(b, ", ");
        spec_put(b, v->pair[1]);
        sb_putc(b, ')');
        return;
    case CV_CONTRAST:
        sb_puts(b, "contrast-color(");
        spec_put(b, v->pair[0]);
        sb_putc(b, ')');
        return;
    case CV_MIX: mix_put(b, v); return;
    case CV_LAYERS:
        /* CSS Color 6, 5.2 leaves this open; normal, the default, is omitted */
        sb_puts(b, "color-layers(");
        if (v->blend != BLEND_NORMAL) { sb_puts(b, blend_names[v->blend]); sb_puts(b, ", "); }
        for (unsigned i = 0; i < v->count; i++) {
            if (i) sb_puts(b, ", ");
            spec_put(b, v->items[i].color);
        }
        sb_putc(b, ')');
        return;
    case CV_FN: {
        const char *const *keys = fn_keys(v->fn, v->space);
        sb_puts(b, fn_names[v->fn]);
        sb_putc(b, '(');
        if (v->origin) { sb_puts(b, "from "); spec_put(b, v->origin); sb_putc(b, ' '); }
        if (v->fn == F_COLOR) { sb_puts(b, space_names[v->space]); sb_putc(b, ' '); }
        if (v->fn != F_ALPHA)
            for (int i = 0; i < 3; i++) {
                if (i) sb_putc(b, ' ');
                chan_put(b, v, i, keys);
            }
        if (v->ch[3].kind != CH_ABSENT) {
            bool opaque = !v->origin && v->ch[3].kind == CH_LEAF &&
                          chan_component(v->fn, v->space, 3, v->ch[3].type, chan_raw(&v->ch[3], NULL, NULL), false) == 1;
            if (!opaque) {
                sb_puts(b, v->fn == F_ALPHA ? "/ " : " / ");
                chan_put(b, v, 3, keys);
            }
        }
        sb_putc(b, ')');
        return;
    }
    }
}

/* ---------------------------------------------------------------- the cascade's entry points */
static bool scheme_dark(const style_t *s) { return s && (s->color_scheme & CSS_SCHEME_DARK) && !(s->color_scheme & CSS_SCHEME_LIGHT); }
/* the computed colour of slot k: *exact the absolute value, or a tree that names currentcolor */
static const struct css_color_value *slot_get(const style_t *s, int k) {
    if (k < CSS_SLOT_INHERITED) return s->color_inherited ? s->color_inherited[k] : NULL;
    return s->color_own ? s->color_own[k - CSS_SLOT_INHERITED] : NULL;
}
static void slot_set(style_t *s, int k, const struct css_color_value *v, arena_t *a) {
    if (slot_get(s, k) == v) return;
    bool inherited = k < CSS_SLOT_INHERITED;
    const struct css_color_value *const *old = inherited ? s->color_inherited : s->color_own;
    unsigned n = inherited ? CSS_SLOT_INHERITED : CSS_SLOTS - CSS_SLOT_INHERITED, at = inherited ? (unsigned)k : (unsigned)k - CSS_SLOT_INHERITED;
    bool any = v != NULL;
    for (unsigned i = 0; i < n && !any; i++) if (i != at && old && old[i]) any = true;
    const struct css_color_value **block = NULL;
    if (any) {
        /* blocks are shared by copies of a style: never written after this */
        block = ar_alloc(a, n * sizeof *block);
        if (old) memcpy(block, old, n * sizeof *block);
        block[at] = v;
    }
    if (inherited) s->color_inherited = block;
    else s->color_own = block;
}
static uint32_t *slot_field(style_t *s, int k) {
    switch (k) {
    case CSS_SLOT_COLOR: return &s->color;
    case CSS_SLOT_FILL: return &s->svg_fill.color;
    case CSS_SLOT_STROKE: return &s->svg_stroke.color;
    case CSS_SLOT_BACKGROUND: return &s->bg_color;
    case CSS_SLOT_OUTLINE: return &s->outline_color;
    case CSS_SLOT_STOP: return &s->svg_stop_color;
    default: return &s->border_color[k - CSS_SLOT_BORDER_TOP];
    }
}
void css_style_color(const style_t *st, enum css_color_slot k, struct css_color_computed *out) {
    const struct css_color_value *v = slot_get(st, k);
    out->current = NULL;
    /* code that writes a style's 8-bit colour directly (a UA default) overrides the slot */
    if (v && v->kind == CV_ABS && v->argb == *slot_field((style_t *)st, k)) { out->color = v->color; return; }
    if (v && v->kind != CV_ABS) {
        out->current = v;
        out->color = css_color_from_argb(*slot_field((style_t *)st, k));
        return;
    }
    uint32_t c = *slot_field((style_t *)st, k);
    if (c == COLOR_CURRENT) { out->current = &current_value; out->color = css_color_from_argb(st->color); return; }
    out->color = css_color_from_argb(c);
}
static struct css_color style_current(const style_t *st) {
    struct css_color_computed c;
    css_style_color(st, CSS_SLOT_COLOR, &c);
    return c.color;
}
void css_color_resolve(const struct css_color_computed *v, const struct css_color *current, struct css_color *out) {
    if (!v->current) { *out = v->color; return; }
    struct cenv E = {.current = current};
    const struct css_color_value *tree;
    if (!cv_compute(v->current, &E, out, &tree) || tree) *out = *current;
}
/* the stored form of a computed value: NULL when the 8-bit field holds it */
static const struct css_color_value *stored(const struct css_color *c, const struct css_color_value *tree, arena_t *a) {
    if (tree) return tree == &current_value ? NULL : tree;
    if (c->legacy && !c->none) return NULL;
    struct css_color_value *v = ar_alloc(a, sizeof *v);
    v->kind = CV_ABS;
    v->color = *c;
    v->argb = css_color_argb(c);
    return v;
}
bool css_style_set_color(style_t *st, enum css_color_slot k, const struct css_color_computed *v, arena_t *a) {
    struct css_color resolved;
    if (v->current) {
        if (k == CSS_SLOT_COLOR) return false; /* color's currentcolor is its inherited value */
        *slot_field(st, k) = COLOR_CURRENT;
        slot_set(st, k, v->current == &current_value ? NULL : v->current, a);
        if (v->current != &current_value) {
            struct css_color current = style_current(st);
            css_color_resolve(v, &current, &resolved);
            *slot_field(st, k) = css_color_argb(&resolved);
        }
        return true;
    }
    *slot_field(st, k) = css_color_argb(&v->color);
    slot_set(st, k, stored(&v->color, NULL, a), a);
    return true;
}
static struct cenv style_env(struct cx *cx) {
    struct cenv E = {.cx = cx, .dark = scheme_dark(cx->s), .a = cx->a};
    return E;
}
bool css_color_value_compute(const char *s, size_t n, struct cx *cx, const struct css_color *current, uint32_t *argb,
                             const struct css_color_value **computed) {
    struct css_color_value root;
    if (!color_parse(cx->a, s, n, &root)) return false;
    if (computed) *computed = NULL;
    if (cx->supports_probe) { *argb = 0; return true; }
    struct cenv E = style_env(cx);
    E.current = current;
    struct css_color c;
    const struct css_color_value *tree;
    if (!cv_compute(&root, &E, &c, &tree)) return false;
    if (tree) {
        *argb = COLOR_CURRENT;
        if (computed) *computed = tree == &current_value ? NULL : tree;
        return true;
    }
    *argb = css_color_argb(&c);
    if (computed) *computed = stored(&c, NULL, cx->a);
    return true;
}
bool css_color_apply(const char *s, size_t n, struct cx *cx, enum css_color_slot k) {
    style_t *st = cx->s;
    struct css_color parent_color;
    const struct css_color *current = NULL;
    if (k == CSS_SLOT_COLOR) {
        /* currentcolor in color is the inherited value; the root's is the initial CanvasText */
        parent_color = cx->parent ? style_current(cx->parent) : system_color(SYS_CANVASTEXT, scheme_dark(st));
        current = &parent_color;
    }
    uint32_t argb;
    const struct css_color_value *v;
    if (!css_color_value_compute(s, n, cx, current, &argb, &v)) return false;
    if (cx->supports_probe) return true;
    *slot_field(st, k) = argb;
    slot_set(st, k, v, cx->a);
    return true;
}
void css_color_slot_copy(style_t *dst, const style_t *src, enum css_color_slot k, arena_t *a) {
    slot_set(dst, k, slot_get(src, k), a);
}
void css_color_slot_set(style_t *s, enum css_color_slot k, const struct css_color_value *v, arena_t *a) {
    slot_set(s, k, v, a);
}
/* after the cascade: currentcolor expressions resolve against the element's colour */
void css_color_finish(style_t *s) {
    if (!s->color_inherited && !s->color_own) return;
    struct css_color current = style_current(s);
    for (int k = CSS_SLOT_FILL; k < CSS_SLOTS; k++) {
        const struct css_color_value *v = slot_get(s, k);
        if (!v || v->kind == CV_ABS) continue;
        struct css_color_computed c = {.current = v};
        struct css_color resolved;
        css_color_resolve(&c, &current, &resolved);
        *slot_field(s, k) = css_color_argb(&resolved);
    }
}
uint32_t css_color_value_argb(const struct css_color_value *v, uint32_t current) {
    struct css_color cur = css_color_from_argb(current), out;
    struct css_color_computed c = {.current = v};
    css_color_resolve(&c, &cur, &out);
    return css_color_argb(&out);
}
/* a computed colour that may be kept beside its 8-bit used value: the exact one when there is one */
bool css_color_value_text(const struct css_color_value *v, uint32_t argb, sbuf *out) {
    struct css_color c = v && v->kind == CV_ABS ? v->color : css_color_from_argb(argb);
    abs_put(out, &c, false);
    return true;
}
/* a value as a declaration would write it: an interpolated value that names currentcolor goes back
   through the cascade this way */
bool css_color_value_specified(const struct css_color_value *v, sbuf *out) {
    if (!v) return false;
    spec_put(out, v);
    return true;
}
bool css_color_value_equal(const struct css_color_value *a, const struct css_color_value *b) {
    if (a == b) return true;
    if (!a || !b) return false;
    sbuf x = {0}, y = {0};
    spec_put(&x, a);
    spec_put(&y, b);
    bool equal = x.n == y.n && (!x.n || !memcmp(x.p, y.p, x.n));
    sb_free(&x);
    sb_free(&y);
    return equal;
}
bool css_color_slots_equal(const style_t *a, const style_t *b) {
    for (int k = 0; k < CSS_SLOTS; k++)
        if (!css_color_value_equal(slot_get(a, k), slot_get(b, k))) return false;
    return true;
}

/* ---------------------------------------------------------------- CSSOM and other callers */
static bool parse_scratch(const char *s, size_t n, bool (*use)(const struct css_color_value *, void *), void *context) {
    arena_t a = {.chunk_size = 8192};
    jmp_buf trap;
    a.trap = &trap;
    bool ok = false;
    if (!setjmp(trap)) {
        struct css_color_value root;
        if (color_parse(&a, s, n, &root)) ok = use(&root, context);
    }
    ar_free(&a);
    return ok;
}
static bool use_specified(const struct css_color_value *v, void *context) {
    spec_put(context, v);
    return true;
}
bool css_color_specified(const char *s, size_t n, sbuf *out) {
    size_t original = out->n;
    while (n && is_space((unsigned char)*s)) s++, n--;
    while (n && is_space((unsigned char)s[n - 1])) n--;
    bool ok = parse_scratch(s, n, use_specified, out);
    if (!ok) { out->n = original; if (out->p && original < out->cap) out->p[original] = 0; }
    return ok;
}
/* whether a value needs an element: currentcolor, or a colour the scheme decides */
static bool needs_element(const struct css_color_value *v) {
    if (!v) return false;
    if (v->kind == CV_CURRENT || v->kind == CV_SYSTEM || v->kind == CV_LIGHT_DARK) return true;
    if (needs_element(v->origin) || needs_element(v->pair[0])) return true;
    if (v->kind == CV_MIX || v->kind == CV_LAYERS)
        for (unsigned i = 0; i < v->count; i++) if (needs_element(v->items[i].color)) return true;
    return false;
}
struct used_request { const style_t *(*element)(void *); void *context; struct css_color *out; };
static bool use_used(const struct css_color_value *v, void *context) {
    struct used_request *r = context;
    const style_t *st = r->element && needs_element(v) ? r->element(r->context) : NULL;
    struct css_color current = st ? style_current(st) : css_color_from_argb(0xFF000000u);
    struct cenv E = {.current = &current, .dark = scheme_dark(st)};
    const struct css_color_value *tree;
    return cv_compute(v, &E, r->out, &tree) && !tree;
}
/* CSS Color 4, 4.5: parse a CSS <color> value outside a style sheet */
bool css_color_parse_used(const char *s, size_t n, const style_t *(*element)(void *), void *context, struct css_color *out) {
    struct used_request r = {element, context, out};
    return parse_scratch(s, n, use_used, &r);
}
bool css_color(const char *s, size_t n, uint32_t *out) {
    struct css_color c;
    /* the legacy entry point keeps currentcolor as its placeholder */
    struct css_color_value probe;
    arena_t a = {.chunk_size = 8192};
    jmp_buf trap;
    a.trap = &trap;
    bool ok = false;
    if (!setjmp(trap)) {
        while (n && is_space((unsigned char)*s)) s++, n--;
        while (n && is_space((unsigned char)s[n - 1])) n--;
        if (color_parse(&a, s, n, &probe)) {
            struct cenv E = {0};
            const struct css_color_value *tree;
            if (cv_compute(&probe, &E, &c, &tree)) {
                *out = tree ? COLOR_CURRENT : css_color_argb(&c);
                ok = true;
            }
        }
    }
    ar_free(&a);
    return ok;
}
bool css_style_color_text(const style_t *st, enum css_color_slot k, sbuf *out) {
    struct css_color_computed v;
    css_style_color(st, k, &v);
    struct css_color resolved;
    if (v.current) {
        struct css_color current = style_current(st);
        css_color_resolve(&v, &current, &resolved);
    } else resolved = v.color;
    abs_put(out, &resolved, false);
    return true;
}
const struct css_color_value *css_color_mix_value(arena_t *a, const struct css_color_computed *from,
                                                  const struct css_color_computed *to, double p,
                                                  const struct css_color_interpolation *m) {
    struct css_color_value *v = ar_alloc(a, sizeof *v);
    v->kind = CV_MIX;
    v->space = m->space;
    v->hue = m->hue;
    v->method = true;
    v->count = 2;
    v->items = ar_alloc(a, 2 * sizeof *v->items);
    if (p < 0) p = 0;
    if (p > 1) p = 1;
    const struct css_color_computed *ends[2] = {from, to};
    for (int i = 0; i < 2; i++) {
        const struct css_color_value *c = ends[i]->current;
        if (!c) {
            struct css_color_value *abs = ar_alloc(a, sizeof *abs);
            abs->kind = CV_ABS;
            abs->color = ends[i]->color;
            c = abs;
        }
        v->items[i].color = c;
        v->items[i].pct = (struct chan){.kind = CH_LEAF, .type = MT_PCT, .v = (i ? p : 1 - p) * 100, .unit = "%"};
    }
    return v;
}
