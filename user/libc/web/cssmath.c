/* CSS math functions and units (CSS Values 4 §6-§7 and §10, CSS Values 5 clamp(none)): the one owner.

   Every value written as a math function goes through here, whatever it is: a length, a percentage,
   an angle, a time, a frequency, a resolution, a flex, a number, an integer or a colour's channel
   (color.c reads channels with keys: identifiers that stand for numbers). A parse reads the grammar
   (§10.1), types each node as CSS Typed OM types them (§10.7: the exponents of the base types and the
   percent hint, the percentage's type given by what it resolves against in the context), simplifies
   the tree as it is built (§10.10.1), and checks the result against what the context accepts.

   A specified value keeps its relative units (em, vw, ...); a computed value (the parse has a struct
   cx) resolves them to pixels, so the tree that stays is one that still needs a percentage's basis
   or a key. Evaluation follows IEEE-754 with the argument ranges of each function (§10.9, signed
   zeros and infinities included); the callers censor a top-level NaN to 0 and clamp to their range
   (§10.12). Serialization is §10.13's, with the children sorted.

   The trees live in the parse's arena. Simplification never changes a node it did not just make, so
   a tree can share subtrees with the ones it was built from (css_length_mix). */
#include <stdio.h>
#include <math.h>
#include <float.h>
#include <ctype.h>
#include "webi.h"

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif
#ifndef M_E
#define M_E 2.7182818284590452354
#endif

/* ---------------------------------------------------------------- units (§6.1, §7) */
enum { T_LENGTH, T_ANGLE, T_TIME, T_FREQUENCY, T_RESOLUTION, T_FLEX, T_PERCENT, T_BASES };
/* what a relative length is a fraction of */
enum { R_ABS, R_EM, R_REM, R_VW, R_VH, R_VMIN, R_VMAX, R_CQW, R_CQH, R_CQMIN, R_CQMAX };
struct unit { const char *name; int8_t base; uint8_t rel; double f; };
enum { U_NUMBER, U_PERCENT, U_PX, U_DEG, U_S, U_HZ, U_DPPX, U_FR };
static const struct unit units[] = {
    [U_NUMBER] = {"", -1, R_ABS, 1}, [U_PERCENT] = {"%", T_PERCENT, R_ABS, 1},
    /* the canonical units (§10.7.2) */
    [U_PX] = {"px", T_LENGTH, R_ABS, 1}, [U_DEG] = {"deg", T_ANGLE, R_ABS, 1}, [U_S] = {"s", T_TIME, R_ABS, 1},
    [U_HZ] = {"hz", T_FREQUENCY, R_ABS, 1}, [U_DPPX] = {"dppx", T_RESOLUTION, R_ABS, 1}, [U_FR] = {"fr", T_FLEX, R_ABS, 1},
    {"cm", T_LENGTH, R_ABS, 96 / 2.54}, {"mm", T_LENGTH, R_ABS, 96 / 25.4}, {"q", T_LENGTH, R_ABS, 96 / 101.6},
    {"in", T_LENGTH, R_ABS, 96}, {"pt", T_LENGTH, R_ABS, 4.0 / 3}, {"pc", T_LENGTH, R_ABS, 16},
    /* font-relative lengths: the font's metrics as fractions of its size */
    {"em", T_LENGTH, R_EM, 1}, {"rem", T_LENGTH, R_REM, 1}, {"ex", T_LENGTH, R_EM, 0.5}, {"rex", T_LENGTH, R_REM, 0.5},
    {"ch", T_LENGTH, R_EM, 0.6}, {"rch", T_LENGTH, R_REM, 0.6}, {"cap", T_LENGTH, R_EM, 0.7},
    {"rcap", T_LENGTH, R_REM, 0.7}, {"ic", T_LENGTH, R_EM, 1}, {"ric", T_LENGTH, R_REM, 1},
    {"lh", T_LENGTH, R_EM, 1.2}, {"rlh", T_LENGTH, R_REM, 1.2},
    /* viewport-percentage lengths: the small, large and dynamic viewports are the one viewport */
    {"vw", T_LENGTH, R_VW, 1}, {"vh", T_LENGTH, R_VH, 1}, {"vi", T_LENGTH, R_VW, 1}, {"vb", T_LENGTH, R_VH, 1},
    {"vmin", T_LENGTH, R_VMIN, 1}, {"vmax", T_LENGTH, R_VMAX, 1},
    {"svw", T_LENGTH, R_VW, 1}, {"svh", T_LENGTH, R_VH, 1}, {"svi", T_LENGTH, R_VW, 1}, {"svb", T_LENGTH, R_VH, 1},
    {"svmin", T_LENGTH, R_VMIN, 1}, {"svmax", T_LENGTH, R_VMAX, 1},
    {"lvw", T_LENGTH, R_VW, 1}, {"lvh", T_LENGTH, R_VH, 1}, {"lvi", T_LENGTH, R_VW, 1}, {"lvb", T_LENGTH, R_VH, 1},
    {"lvmin", T_LENGTH, R_VMIN, 1}, {"lvmax", T_LENGTH, R_VMAX, 1},
    {"dvw", T_LENGTH, R_VW, 1}, {"dvh", T_LENGTH, R_VH, 1}, {"dvi", T_LENGTH, R_VW, 1}, {"dvb", T_LENGTH, R_VH, 1},
    {"dvmin", T_LENGTH, R_VMIN, 1}, {"dvmax", T_LENGTH, R_VMAX, 1},
    /* container query lengths: the query container's size, else the viewport's */
    {"cqw", T_LENGTH, R_CQW, 1}, {"cqh", T_LENGTH, R_CQH, 1}, {"cqi", T_LENGTH, R_CQW, 1}, {"cqb", T_LENGTH, R_CQH, 1},
    {"cqmin", T_LENGTH, R_CQMIN, 1}, {"cqmax", T_LENGTH, R_CQMAX, 1},
    {"grad", T_ANGLE, R_ABS, 0.9}, {"rad", T_ANGLE, R_ABS, 180 / M_PI}, {"turn", T_ANGLE, R_ABS, 360},
    {"ms", T_TIME, R_ABS, 0.001}, {"khz", T_FREQUENCY, R_ABS, 1000},
    {"x", T_RESOLUTION, R_ABS, 1}, {"dpi", T_RESOLUTION, R_ABS, 1 / 96.0}, {"dpcm", T_RESOLUTION, R_ABS, 2.54 / 96},
};
#define UNIT_COUNT (int)(sizeof units / sizeof *units)
static const uint8_t canonical[T_PERCENT] = {U_PX, U_DEG, U_S, U_HZ, U_DPPX, U_FR};
static const unsigned base_kind[T_BASES] = {CSS_CALC_LENGTH, CSS_CALC_ANGLE, CSS_CALC_TIME, CSS_CALC_FREQUENCY,
                                            CSS_CALC_RESOLUTION, CSS_CALC_FLEX, CSS_CALC_PERCENT};

int css_unit_find(const char *u, size_t n) {
    if (!n) return U_NUMBER;
    if (n == 1 && *u == '%') return U_PERCENT;
    if (n > 5) return -1;
    for (int i = U_PX; i < UNIT_COUNT; i++) {
        const char *name = units[i].name;
        size_t k = 0;
        while (k < n && name[k] && lower((unsigned char)u[k]) == name[k]) k++;
        if (k == n && !name[k]) return i;
    }
    return -1;
}
unsigned css_unit_kind(int unit) {
    if (unit < 0 || unit >= UNIT_COUNT) return 0;
    return units[unit].base < 0 ? CSS_CALC_NUMBER : base_kind[units[unit].base];
}
bool css_unit_relative(int unit) { return unit >= 0 && unit < UNIT_COUNT && units[unit].rel != R_ABS; }
const char *css_unit_name(int unit) { return unit >= 0 && unit < UNIT_COUNT ? units[unit].name : ""; }
/* an element's sizes that relative lengths are fractions of; no element: the initial font and an
   800x600 viewport */
static double rel_px(uint8_t rel, const struct cx *cx) {
    double em = cx ? cx->em : 16, rem = cx ? cx->rem : 16, vw = cx ? cx->vw : 800, vh = cx ? cx->vh : 600;
    double cw = cx && cx->cq_width_set ? cx->cq_width : vw, ch = cx && cx->cq_height_set ? cx->cq_height : vh;
    switch (rel) {
    case R_EM: return em;
    case R_REM: return rem;
    case R_VW: return vw / 100;
    case R_VH: return vh / 100;
    case R_VMIN: return (vw < vh ? vw : vh) / 100;
    case R_VMAX: return (vw > vh ? vw : vh) / 100;
    case R_CQW: return cw / 100;
    case R_CQH: return ch / 100;
    case R_CQMIN: return (cw < ch ? cw : ch) / 100;
    case R_CQMAX: return (cw > ch ? cw : ch) / 100;
    }
    return 1;
}
double css_unit_canonical(int unit, double v, const struct cx *cx) {
    if (unit < 0 || unit >= UNIT_COUNT) return 0;
    const struct unit *d = &units[unit];
    return d->rel == R_ABS ? (d->f == 1 ? v : v * d->f) : v * d->f * rel_px(d->rel, cx);
}

/* ---------------------------------------------------------------- the tree */
/* N_ANCHOR: anchor() or anchor-size() (css-anchor-position-1), a <length> that layout resolves;
   N_TREE: sibling-index() (fn 0) or sibling-count() (fn 1), an <integer> the element resolves */
enum { N_VALUE, N_KEY, N_SUM, N_PRODUCT, N_NEGATE, N_INVERT, N_FN, N_ANCHOR, N_TREE };
enum { F_MIN, F_MAX, F_CLAMP, F_ROUND, F_MOD, F_REM, F_SIN, F_COS, F_TAN, F_ASIN, F_ACOS, F_ATAN, F_ATAN2, F_POW,
       F_SQRT, F_HYPOT, F_LOG, F_EXP, F_ABS, F_SIGN, F_COUNT };
static const char *const fn_names[F_COUNT] = {"min", "max", "clamp", "round", "mod", "rem", "sin", "cos", "tan",
    "asin", "acos", "atan", "atan2", "pow", "sqrt", "hypot", "log", "exp", "abs", "sign"};
enum { RS_NEAREST, RS_UP, RS_DOWN, RS_TO_ZERO };
static const char *const round_names[] = {"nearest", "up", "down", "to-zero"};
/* a type (§10.7): the exponent of each base type and the percent hint (a base, or -1) */
_Static_assert(T_BASES == CSS_TYPE_BASES, "the base types are webi.h's");
struct css_calc {
    uint8_t op, fn;
    uint8_t unit; /* N_VALUE: U_* or another unit of the table */
    uint8_t key;  /* N_KEY: the key's index; F_ROUND: the strategy */
    uint16_t n;
    struct css_calc_type type;
    double v;
    struct css_calc **k; /* the children; clamp()'s none is NULL */
    const struct css_anchor_function *anchor; /* N_ANCHOR: its arguments (cssprop.c) */
};
#define CALC_NODES 4096
#define CALC_DEPTH 64

/* the state of one parse or build */
struct mp {
    struct css_calc_parse *P;
    const char *p, *e;
    int8_t pct; /* the base a percentage has here: T_PERCENT for its own type, -1: percentages fail */
    bool own;   /* T_PERCENT: the context says a percentage is a fraction, not of a basis it did not name */
    struct css_calc *made; /* P->raw: the last operator node the grammar made (a calc() without one wraps) */
};
static struct css_calc *node(struct mp *m, uint8_t op, unsigned n) {
    if (++m->P->nodes > CALC_NODES || n > 0xffff) return NULL;
    struct css_calc *c = ar_alloc(m->P->a, sizeof *c);
    c->op = op;
    c->n = (uint16_t)n;
    c->type.hint = -1;
    if (n) c->k = ar_alloc(m->P->a, n * sizeof *c->k);
    return c;
}
static struct css_calc *mk_leaf(struct mp *m, double v, uint8_t unit, struct css_calc_type type) {
    struct css_calc *c = node(m, N_VALUE, 0);
    if (!c) return NULL;
    c->v = v;
    c->unit = unit;
    c->type = type;
    return c;
}
static bool is_leaf(const struct css_calc *c) { return c && c->op == N_VALUE; }
static bool is_number(const struct css_calc *c) { return is_leaf(c) && c->unit == U_NUMBER; }
/* a percentage that is a type of its own: its value is known without a basis */
/* a percentage that is a type of its own whose value is known: a fraction (opacity, a colour's channel), not
   a percentage of a basis the context did not say (key 1 marks it) */
static bool own_percent(const struct css_calc *c) { return c->unit == U_PERCENT && c->type.e[T_PERCENT] == 1 && c->key == 1; }
/* a leaf of value v in like's unit and type: a percentage of its own type stays one (simplification
   folds calc(100% + 100%) where the percentage is the value's own, as in font-width or opacity) */
static struct css_calc *mk_like(struct mp *m, double v, const struct css_calc *like) {
    struct css_calc *c = mk_leaf(m, v, like->unit, like->type);
    if (c && own_percent(like)) c->key = 1;
    return c;
}
/* a value of type t computed from known leaves (percentages only of their own type): its percentage,
   if it is one, is its own too */
static struct css_calc *mk_known(struct mp *m, double v, uint8_t unit, struct css_calc_type t) {
    struct css_calc *c = mk_leaf(m, v, unit, t);
    if (c && unit == U_PERCENT && t.e[T_PERCENT] == 1) c->key = 1;
    return c;
}
/* a leaf whose value needs nothing: a number, a canonical dimension, a percentage of its own type */
static bool known(const struct css_calc *c) {
    if (!is_leaf(c)) return false;
    if (c->unit == U_PERCENT) return own_percent(c);
    return units[c->unit].rel == R_ABS;
}
static bool zero_type(const struct css_calc_type *t) {
    for (int i = 0; i < T_BASES; i++) if (t->e[i]) return false;
    return true;
}

/* ---------------------------------------------------------------- types (§10.7, CSS Typed OM §4) */
static void t_hint(struct css_calc_type *t, int8_t hint) {
    if (t->e[T_PERCENT]) { t->e[hint] = (int8_t)(t->e[hint] + t->e[T_PERCENT]); t->e[T_PERCENT] = 0; }
    t->hint = hint;
}
static bool t_eq(const struct css_calc_type *a, const struct css_calc_type *b) { return !memcmp(a->e, b->e, sizeof a->e); }
static bool t_hints(struct css_calc_type *a, struct css_calc_type *b) {
    if (a->hint >= 0 && b->hint >= 0 && a->hint != b->hint) return false;
    if (a->hint >= 0 && b->hint < 0) t_hint(b, a->hint);
    else if (b->hint >= 0 && a->hint < 0) t_hint(a, b->hint);
    return true;
}
/* "add two types" */
static bool t_add(struct css_calc_type a, struct css_calc_type b, struct css_calc_type *out) {
    if (!t_hints(&a, &b)) return false;
    if (t_eq(&a, &b)) { *out = a; return true; }
    bool pct = a.e[T_PERCENT] || b.e[T_PERCENT], other = false;
    for (int i = 0; i < T_PERCENT; i++) other |= a.e[i] || b.e[i];
    if (pct && other)
        for (int8_t h = 0; h < T_PERCENT; h++) {
            struct css_calc_type x = a, y = b;
            t_hint(&x, h);
            t_hint(&y, h);
            if (t_eq(&x, &y)) { *out = x; return true; }
        }
    return false;
}
/* "multiply two types" */
static bool t_mul(struct css_calc_type a, struct css_calc_type b, struct css_calc_type *out) {
    if (!t_hints(&a, &b)) return false;
    for (int i = 0; i < T_BASES; i++) {
        int e = a.e[i] + b.e[i];
        if (e > 100 || e < -100) return false;
        a.e[i] = (int8_t)e;
    }
    *out = a;
    return true;
}
static struct css_calc_type t_inv(struct css_calc_type a) {
    for (int i = 0; i < T_BASES; i++) a.e[i] = (int8_t)-a.e[i];
    return a;
}
/* what the type is in a context that accepts accept (whose percentages are pct): a CSS_CALC_* bit, or 0 */
static unsigned t_match(const struct css_calc_type *t, unsigned accept, int8_t pct) {
    int count = 0, base = -1;
    for (int i = 0; i < T_BASES; i++) if (t->e[i]) { count++; base = i; }
    /* a percent hint is only for a context whose percentages resolve against that type */
    bool hint = t->hint < 0 || t->hint == pct ||
                ((accept & CSS_CALC_PERCENT) && t->hint < T_PERCENT && (accept & base_kind[t->hint]));
    if (!hint) return 0;
    if (!count) return accept & CSS_CALC_NUMBER;
    if (count > 1 || t->e[base] != 1) return 0;
    return accept & base_kind[base];
}
/* "made consistent with" another type: the number keeps its percent hint */
static struct css_calc_type t_number(const struct css_calc_type *from) {
    struct css_calc_type t = {{0}, from->hint};
    return t;
}
static int8_t pct_base(unsigned accept);
bool css_calc_type_add(const struct css_calc_type *a, const struct css_calc_type *b, struct css_calc_type *out) {
    return t_add(*a, *b, out);
}
bool css_calc_type_multiply(const struct css_calc_type *a, const struct css_calc_type *b, struct css_calc_type *out) {
    return t_mul(*a, *b, out);
}
unsigned css_calc_type_match(const struct css_calc_type *t, unsigned accept) { return t_match(t, accept, pct_base(accept)); }
int css_unit_count(void) { return UNIT_COUNT; }
void css_unit_info(int unit, const char **name, int *base, bool *relative, double *factor) {
    const struct unit *d = &units[unit >= 0 && unit < UNIT_COUNT ? unit : U_NUMBER];
    *name = d->name;
    *base = d->base;
    *relative = d->rel != R_ABS;
    *factor = d->f;
}

/* ---------------------------------------------------------------- simplification (§10.10.1) */
static struct css_calc *simplify(struct mp *m, struct css_calc *c);
static double fn_eval(uint8_t fn, uint8_t strategy, const double *x, const bool *present, unsigned n, bool angle);
static int order_unit(const struct css_calc *c) { return c->unit == U_NUMBER ? 0 : c->unit == U_PERCENT ? 1 : 2; }
/* the children of c with those of the same operator in them */
static bool flatten(struct mp *m, struct css_calc *c, struct css_calc ***out, unsigned *count) {
    unsigned total = 0;
    for (unsigned i = 0; i < c->n; i++) total += c->k[i]->op == c->op ? c->k[i]->n : 1;
    if (total > 0xffff) return false;
    struct css_calc **k = ar_alloc(m->P->a, (total ? total : 1) * sizeof *k);
    unsigned j = 0;
    for (unsigned i = 0; i < c->n; i++)
        if (c->k[i]->op == c->op) for (unsigned q = 0; q < c->k[i]->n; q++) k[j++] = c->k[i]->k[q];
        else k[j++] = c->k[i];
    *out = k;
    *count = j;
    return true;
}
static struct css_calc *with_children(struct mp *m, const struct css_calc *c, struct css_calc **k, unsigned n) {
    struct css_calc *r = node(m, c->op, 0);
    if (!r) return NULL;
    r->fn = c->fn;
    r->key = c->key;
    r->type = c->type;
    r->n = (uint16_t)n;
    r->k = k;
    return r;
}
static struct css_calc *simplify_sum(struct mp *m, struct css_calc *c) {
    struct css_calc **k;
    unsigned n;
    if (!flatten(m, c, &k, &n)) return NULL;
    /* numeric values of the same unit add up */
    for (unsigned i = 0; i < n; i++) {
        if (!is_leaf(k[i])) continue;
        struct css_calc *sum = k[i];
        for (unsigned q = i + 1; q < n;) {
            if (is_leaf(k[q]) && k[q]->unit == sum->unit && (sum->unit != U_PERCENT || t_eq(&k[q]->type, &sum->type))) {
                struct css_calc *x = mk_like(m, sum->v + k[q]->v, sum);
                if (!x) return NULL;
                sum = x;
                memmove(&k[q], &k[q + 1], (n - q - 1) * sizeof *k);
                n--;
            } else q++;
        }
        k[i] = sum;
    }
    if (n == 1) return k[0];
    return with_children(m, c, k, n);
}
static struct css_calc *simplify_product(struct mp *m, struct css_calc *c) {
    struct css_calc **k;
    unsigned n;
    if (!flatten(m, c, &k, &n)) return NULL;
    /* its numbers are one number */
    double number = 1;
    unsigned numbers = 0, w = 0;
    struct css_calc *first_number = NULL;
    for (unsigned i = 0; i < n; i++) {
        if (is_number(k[i])) { number = numbers ? number * k[i]->v : k[i]->v; if (!numbers++) first_number = k[i]; }
        else k[w++] = k[i];
    }
    struct css_calc_type none = {{0}, -1};
    if (!w) return numbers == 1 ? first_number : mk_leaf(m, number, U_NUMBER, t_number(&c->type));
    /* a number times a sum of numeric values: into each of them */
    if (w == 1 && numbers && k[0]->op == N_SUM) {
        const struct css_calc *s = k[0];
        bool all = true;
        for (unsigned i = 0; i < s->n; i++) all &= is_leaf(s->k[i]);
        if (all) {
            struct css_calc **t = ar_alloc(m->P->a, s->n * sizeof *t);
            for (unsigned i = 0; i < s->n; i++)
                if (!(t[i] = mk_like(m, s->k[i]->v * number, s->k[i]))) return NULL;
            return with_children(m, s, t, s->n);
        }
    }
    /* numeric values (and inverses of them) whose product has a type: one value in its canonical unit */
    bool all = true;
    for (unsigned i = 0; i < w && all; i++) all = known(k[i]) || (k[i]->op == N_INVERT && known(k[i]->k[0]));
    if (all) {
        unsigned kind = t_match(&c->type, CSS_CALC_ANY, T_PERCENT);
        if (kind && c->type.hint < 0) {
            double v = numbers ? number : 1;
            for (unsigned i = 0; i < w; i++) v = k[i]->op == N_INVERT ? v / k[i]->k[0]->v : v * k[i]->v;
            int base = -1;
            for (int i = 0; i < T_BASES; i++) if (c->type.e[i]) base = i;
            uint8_t unit = base < 0 ? U_NUMBER : base == T_PERCENT ? U_PERCENT : canonical[base];
            return mk_known(m, v, unit, c->type);
        }
    }
    /* a number times one numeric value: the value scaled, in its unit */
    if (w == 1 && is_leaf(k[0])) {
        if (!numbers) return k[0];
        return mk_like(m, k[0]->v * number, k[0]);
    }
    if (w == 1 && !numbers) return k[0];
    /* a number times one numeric value beside terms that are not: the value scaled (the engines and WPT
       serialize calc(sibling-index() * 2rad * pi) as calc(360deg * sibling-index())) */
    if (numbers) {
        unsigned leaves = 0, at = 0;
        for (unsigned i = 0; i < w; i++) if (is_leaf(k[i])) { leaves++; at = i; }
        if (leaves == 1) {
            if (!(k[at] = mk_leaf(m, k[at]->v * number, k[at]->unit, k[at]->type))) return NULL;
            numbers = 0;
        }
    }
    if (numbers) {
        struct css_calc **t = ar_alloc(m->P->a, (w + 1) * sizeof *t);
        if (!(t[0] = numbers == 1 ? first_number : mk_leaf(m, number, U_NUMBER, none))) return NULL;
        memcpy(t + 1, k, w * sizeof *t);
        return with_children(m, c, t, w + 1);
    }
    return with_children(m, c, k, w);
}
/* the result's unit for a function of known values */
static uint8_t type_unit(const struct css_calc_type *t) {
    for (int i = 0; i < T_BASES; i++)
        if (t->e[i]) return i == T_PERCENT ? U_PERCENT : canonical[i];
    return U_NUMBER;
}
static struct css_calc *simplify_fn(struct mp *m, struct css_calc *c) {
    bool all = true;
    for (unsigned i = 0; i < c->n; i++) if (c->k[i] && !known(c->k[i])) all = false;
    if (all && (c->fn != F_CLAMP || c->k[1])) {
        double x[3], v;
        bool present[3] = {true, true, true};
        bool angle = c->n && c->k[0] && c->k[0]->type.e[T_ANGLE] == 1;
        if (c->fn == F_MIN || c->fn == F_MAX || c->fn == F_HYPOT) {
            v = c->k[0]->v;
            for (unsigned i = 1; i < c->n; i++) {
                double pair[2] = {v, c->k[i]->v};
                v = fn_eval(c->fn, 0, pair, present, 2, false);
            }
            if (c->fn == F_HYPOT && c->n == 1) v = fabs(v);
        } else {
            for (unsigned i = 0; i < c->n && i < 3; i++) {
                present[i] = c->k[i] != NULL;
                x[i] = present[i] ? c->k[i]->v : 0;
            }
            v = fn_eval(c->fn, c->key, x, present, c->n, angle);
        }
        return mk_known(m, v, type_unit(&c->type), c->type);
    }
    if (c->fn == F_MIN || c->fn == F_MAX) {
        /* partly: the children of one unit compare among themselves */
        struct css_calc **k = ar_alloc(m->P->a, c->n * sizeof *k);
        unsigned n = 0;
        bool changed = false;
        for (unsigned i = 0; i < c->n; i++) {
            struct css_calc *x = c->k[i];
            bool merged = false;
            if (is_leaf(x) && x->unit != U_PERCENT)
                for (unsigned q = 0; q < n; q++)
                    if (is_leaf(k[q]) && k[q]->unit == x->unit) {
                        double pair[2] = {k[q]->v, x->v};
                        bool present[2] = {true, true};
                        double v = fn_eval(c->fn, 0, pair, present, 2, false);
                        if (v != k[q]->v || signbit(v) != signbit(k[q]->v) || isnan(v) != isnan(k[q]->v))
                            if (!(k[q] = mk_leaf(m, v, x->unit, k[q]->type))) return NULL;
                        merged = changed = true;
                        break;
                    }
            if (!merged) k[n++] = x;
        }
        if (n == 1) return k[0];
        if (changed) return with_children(m, c, k, n);
    }
    return c;
}
static struct css_calc *simplify(struct mp *m, struct css_calc *c) {
    if (!c || m->P->raw) return c; /* Typed OM reifies the tree as written */
    switch (c->op) {
    case N_NEGATE: {
        struct css_calc *x = c->k[0];
        if (is_leaf(x)) return mk_like(m, -x->v, x);
        if (x->op == N_NEGATE) return x->k[0];
        return c;
    }
    case N_INVERT: {
        struct css_calc *x = c->k[0];
        if (is_number(x)) return mk_leaf(m, 1 / x->v, U_NUMBER, x->type);
        if (x->op == N_INVERT) return x->k[0];
        return c;
    }
    case N_SUM: return simplify_sum(m, c);
    case N_PRODUCT: return simplify_product(m, c);
    case N_FN: return simplify_fn(m, c);
    }
    return c;
}

/* ---------------------------------------------------------------- evaluation (§10.9) */
static double fmin_signed(double a, double b) {
    if (isnan(a) || isnan(b)) return NAN;
    if (a == b) return signbit(a) ? a : b; /* -0 is less than 0 */
    return a < b ? a : b;
}
static double fmax_signed(double a, double b) {
    if (isnan(a) || isnan(b)) return NAN;
    if (a == b) return signbit(a) ? b : a;
    return a > b ? a : b;
}
/* round(): the multiples of b on either side of a (§10.4.1) */
static double round_to(double a, double b, int strategy) {
    if (isnan(a) || isnan(b) || b == 0) return NAN;
    if (isinf(a)) return isinf(b) ? NAN : a;
    if (isinf(b)) {
        if (strategy == RS_UP) return a > 0 ? INFINITY : copysign(0, a);
        if (strategy == RS_DOWN) return a < 0 ? -INFINITY : copysign(0, a);
        return copysign(0, a);
    }
    b = fabs(b);
    if (fmod(a, b) == 0) return a;
    double q = floor(a / b), lo = q * b, hi = (q + 1) * b;
    if (lo > a) { lo -= b; hi -= b; }
    else if (hi < a) { lo += b; hi += b; }
    if (lo == 0) lo = 0.0;
    if (hi == 0) hi = -0.0;
    switch (strategy) {
    case RS_UP: return hi;
    case RS_DOWN: return lo;
    case RS_TO_ZERO: return fabs(lo) < fabs(hi) ? lo : hi;
    default: return a - lo < hi - a ? lo : hi;
    }
}
/* the trigonometric functions of degrees, exact where the result is (sin(180deg) is 0) */
static double trig(uint8_t fn, double x, bool degrees) {
    if (!degrees) return fn == F_SIN ? sin(x) : fn == F_COS ? cos(x) : tan(x);
    if (!isfinite(x)) return NAN;
    double r = fmod(x, 360);
    if (r < 0) r += 360;
    if (r == 0 || r == 90 || r == 180 || r == 270) {
        int q = (int)(r / 90);
        static const double s[4] = {0, 1, 0, -1}, c[4] = {1, 0, -1, 0};
        if (fn == F_COS) return c[q];
        if (fn == F_TAN && (q & 1)) return q == 1 ? INFINITY : -INFINITY;
        if (q & 1) return s[q];
        return x == 0 ? x : 0; /* sin(-0) and tan(-0) are -0 */
    }
    double rad = x * M_PI / 180;
    return fn == F_SIN ? sin(rad) : fn == F_COS ? cos(rad) : tan(rad);
}
static double fn_eval(uint8_t fn, uint8_t strategy, const double *x, const bool *present, unsigned n, bool angle) {
    double a = x[0], b = n > 1 ? x[1] : 1;
    switch (fn) {
    case F_MIN: return fmin_signed(a, b);
    case F_MAX: return fmax_signed(a, b);
    case F_CLAMP: {
        /* max(MIN, min(VAL, MAX)); a bound that is none is absent */
        double v = x[1];
        if ((present[0] && isnan(x[0])) || (present[2] && isnan(x[2]))) return NAN;
        if (present[2]) v = fmin_signed(v, x[2]);
        if (present[0]) v = fmax_signed(x[0], v);
        return v;
    }
    case F_ROUND: return round_to(a, b, strategy);
    case F_MOD: {
        if (isnan(a) || isnan(b) || b == 0 || isinf(a)) return NAN;
        if (isinf(b)) return signbit(a) == signbit(b) ? a : NAN;
        double r = fmod(a, b);
        if (r != 0 && (r < 0) != (b < 0)) r += b;
        return r == 0 ? copysign(0, b) : r;
    }
    case F_REM:
        if (isnan(a) || isnan(b) || b == 0 || isinf(a)) return NAN;
        return isinf(b) ? a : fmod(a, b);
    case F_SIN: case F_COS: case F_TAN: return trig(fn, a, angle);
    case F_ASIN: return asin(a) * 180 / M_PI;
    case F_ACOS: return acos(a) * 180 / M_PI;
    case F_ATAN: return atan(a) * 180 / M_PI;
    case F_ATAN2: return atan2(a, b) * 180 / M_PI;
    case F_POW:
        /* NaN in, NaN out, and 1 to an infinite power is NaN (CSS Values 4 §10.9, not C's 1) */
        if (isnan(a) || isnan(b) || (fabs(a) == 1 && isinf(b))) return NAN;
        return pow(a, b);
    case F_SQRT: return sqrt(a);
    case F_HYPOT: return isnan(a) || isnan(b) ? NAN : n > 1 ? hypot(a, b) : fabs(a);
    case F_LOG: {
        if (n < 2) return log(a);
        if (b == 1 || isnan(a) || isnan(b)) return NAN; /* no base 1 */
        double r = log(a) / log(b);
        return r == 0 ? 0.0 : r; /* log(1, B) is 0, whatever B's side of 1 */
    }
    case F_EXP: return exp(a);
    case F_ABS: return fabs(a);
    case F_SIGN: return a > 0 ? 1 : a < 0 ? -1 : a; /* keeps the sign of a zero, and NaN */
    }
    return NAN;
}
/* css-values-5 §10 the tree-counting functions: the element's index among its inclusive element
   siblings (1-based, as :nth-child() counts), or their number. A pseudo-element's are its originating
   element's (cx->node). A declaration from an outer tree than the element's (::part()) is a
   tree-scoped reference that fails to match: 1, as WPT reads it (tree-scoped-sibling-function.html). */
static double tree_count(const struct cx *cx, bool count) {
    node_t *e = cx->node;
    if (cx->tree_mismatch) return 1;
    if (e->parent) __atomic_store_n(&e->parent->style_counts_children, true, __ATOMIC_RELAXED);
    if (!count) return e->elem_index > 0 ? e->elem_index : 1;
    const node_t *last = e->parent ? e->parent->last : e;
    while (last && last->type != N_ELEM) last = last->prev;
    return last && last->elem_index > 0 ? last->elem_index : 1;
}
struct ev { double basis; const double *keys; const struct cx *cx; };
static double ev(const struct css_calc *c, const struct ev *E) {
    switch (c->op) {
    case N_VALUE:
        if (c->unit == U_PERCENT) return own_percent(c) || isnan(E->basis) ? c->v : c->v * E->basis / 100;
        return css_unit_canonical(c->unit, c->v, E->cx);
    case N_KEY: return E->keys ? E->keys[c->key] : 0;
    case N_NEGATE: return -ev(c->k[0], E);
    case N_INVERT: return 1 / ev(c->k[0], E);
    case N_SUM: {
        double r = ev(c->k[0], E);
        for (unsigned i = 1; i < c->n; i++) r += ev(c->k[i], E);
        return r;
    }
    case N_PRODUCT: {
        double r = ev(c->k[0], E);
        for (unsigned i = 1; i < c->n; i++) r *= ev(c->k[i], E);
        return r;
    }
    case N_ANCHOR: /* not resolved against an anchor (css_length_resolve_anchors): its fallback */
        return c->anchor->fallback_given ? len_resolve(&c->anchor->fallback, isnan(E->basis) ? 0 : (float)E->basis) : 0;
    case N_TREE: return E->cx && E->cx->node ? tree_count(E->cx, c->fn) : 0;
    }
    if (c->fn == F_MIN || c->fn == F_MAX || c->fn == F_HYPOT) {
        double r = ev(c->k[0], E);
        if (c->fn == F_HYPOT && c->n == 1) return fabs(r);
        bool present[2] = {true, true};
        for (unsigned i = 1; i < c->n; i++) {
            double pair[2] = {r, ev(c->k[i], E)};
            r = fn_eval(c->fn, 0, pair, present, 2, false);
        }
        return r;
    }
    double x[3] = {0, 0, 0};
    bool present[3] = {true, true, true};
    for (unsigned i = 0; i < c->n && i < 3; i++) {
        present[i] = c->k[i] != NULL;
        if (present[i]) x[i] = ev(c->k[i], E);
    }
    return fn_eval(c->fn, c->key, x, present, c->n, c->k[0] && c->k[0]->type.e[T_ANGLE] == 1);
}
double css_calc_value(const struct css_calc *c, double basis, const double *keys, const struct cx *cx) {
    struct ev E = {basis, keys, cx};
    return c ? ev(c, &E) : 0;
}

/* ---------------------------------------------------------------- the grammar (§10.1) */
static void sp(struct mp *m) {
    for (;;) {
        while (m->p < m->e && is_space((unsigned char)*m->p)) m->p++;
        if (m->e - m->p >= 2 && m->p[0] == '/' && m->p[1] == '*') {
            const char *q = m->p + 2;
            while (q + 1 < m->e && !(q[0] == '*' && q[1] == '/')) q++;
            m->p = q + 1 < m->e ? q + 2 : m->e;
            continue;
        }
        return;
    }
}
static bool name_char(unsigned c) { return isalnum(c) || c == '-' || c == '_' || c >= 0x80; }
static bool ident_start(const char *p, const char *e) {
    unsigned c = (unsigned char)*p;
    if (isalpha(c) || c == '_' || c >= 0x80) return true;
    if (c == '-' && p + 1 < e) {
        unsigned d = (unsigned char)p[1];
        return isalpha(d) || d == '_' || d == '-' || d >= 0x80;
    }
    return false;
}
static bool number_start(const char *p, const char *e) {
    if (p < e && (*p == '+' || *p == '-')) p++;
    if (p < e && *p >= '0' && *p <= '9') return true;
    return p + 1 < e && *p == '.' && p[1] >= '0' && p[1] <= '9';
}
static bool word_is(const char *s, size_t n, const char *w) {
    size_t k = 0;
    while (k < n && w[k] && lower((unsigned char)s[k]) == w[k]) k++;
    return k == n && !w[k];
}
/* a <number-token>, <percentage-token> or <dimension-token> at *ps (CSS Syntax §4.3.12) */
bool css_numeric_token(const char **ps, const char *e, double *v, const char **unit, size_t *unit_n) {
    const char *p = *ps, *q = p;
    if (!number_start(p, e)) return false;
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
    if (k >= sizeof buf) return false; /* longer than any double needs: not read */
    memcpy(buf, p, k);
    buf[k] = 0;
    *v = strtod(buf, NULL);
    const char *u = q;
    if (q < e && *q == '%') q++;
    else if (q < e && ident_start(q, e)) while (q < e && name_char((unsigned char)*q)) q++;
    *unit = u;
    *unit_n = (size_t)(q - u);
    *ps = q;
    return true;
}

static struct css_calc *m_sum(struct mp *m);
static struct css_calc *m_value(struct mp *m);
/* a growing list of children in the parse's arena */
struct list { struct css_calc **v; unsigned n, cap; };
static bool push_slot(struct mp *m, struct list *l, struct css_calc *c) {
    if (l->n == l->cap) {
        unsigned cap = l->cap ? l->cap * 2 : 4;
        if (cap > 0xffff) return false;
        struct css_calc **v = ar_alloc(m->P->a, cap * sizeof *v);
        if (l->n) memcpy(v, l->v, l->n * sizeof *v);
        l->v = v;
        l->cap = cap;
    }
    l->v[l->n++] = c;
    return true;
}
static bool push(struct mp *m, struct list *l, struct css_calc *c) { return c && push_slot(m, l, c); }
static struct css_calc *m_leaf(struct mp *m, double v, int unit) {
    struct css_calc_type t = {{0}, -1};
    const struct cx *cx = m->P->cx;
    if (m->P->raw) { /* "create a type" from the unit as written (CSS Typed OM §4.3.2) */
        if (unit != U_NUMBER) t.e[unit == U_PERCENT ? T_PERCENT : units[unit].base] = 1;
        return mk_leaf(m, v, (uint8_t)unit, t);
    }
    if (unit == U_PERCENT) {
        if (m->pct < 0 || (cx && cx->absolute_lengths)) return NULL;
        t.e[m->pct] = 1;
        if (m->pct != T_PERCENT) t.hint = m->pct;
        struct css_calc *c = mk_leaf(m, v, U_PERCENT, t);
        if (c && m->pct == T_PERCENT && m->own) c->key = 1;
        return c;
    }
    if (unit == U_NUMBER) return mk_leaf(m, v, U_NUMBER, t);
    const struct unit *d = &units[unit];
    t.e[d->base] = 1;
    /* in the canonical unit when that needs no element; relative lengths too in a computed value */
    if (d->rel == R_ABS) return mk_leaf(m, v * d->f, canonical[d->base], t);
    if (cx) {
        if (cx->absolute_lengths) return NULL;
        return mk_leaf(m, css_unit_canonical(unit, v, cx), U_PX, t);
    }
    return mk_leaf(m, v, (uint8_t)unit, t);
}
/* the arguments of a function up to its ')'; none: clamp()'s first and last may be none */
static bool m_args(struct mp *m, struct list *args, bool none) {
    for (;;) {
        sp(m);
        const char *at = m->p;
        if (none && m->e - at >= 4 && word_is(at, 4, "none") && (at + 4 == m->e || !name_char((unsigned char)at[4]))) {
            m->p += 4;
            if (!push_slot(m, args, NULL)) return false;
        } else if (!push(m, args, m_sum(m))) return false;
        sp(m);
        if (m->p < m->e && *m->p == ',') { m->p++; continue; }
        /* the end of the value closes what is open (CSS Syntax §5.4.8) */
        if (m->p >= m->e || *m->p == ')') { if (m->p < m->e) m->p++; return true; }
        return false;
    }
}
/* a tree-counting function: its <integer> when the element is known (a computed value), else the
   function itself (a specified value keeps it) */
static struct css_calc *tree_leaf(struct mp *m, bool count) {
    const struct cx *cx = m->P->cx;
    struct css_calc_type t = {{0}, -1};
    if (cx && cx->node && !cx->supports_probe) return mk_leaf(m, tree_count(cx, count), U_NUMBER, t);
    struct css_calc *c = node(m, N_TREE, 0);
    if (c) c->fn = count;
    return c;
}
/* an anchor function's leaf: a <length> */
static struct css_calc *anchor_leaf(struct mp *m, const struct css_anchor_function *f) {
    struct css_calc *c = f ? node(m, N_ANCHOR, 0) : NULL;
    if (!c) return NULL;
    c->type.e[T_LENGTH] = 1;
    c->anchor = f;
    return c;
}
static struct css_calc *m_function(struct mp *m, const char *name, size_t name_n) {
    if (++m->P->depth > CALC_DEPTH) return NULL;
    struct css_calc *result = NULL;
    if (word_is(name, name_n, "calc") || word_is(name, name_n, "-webkit-calc")) {
        struct css_calc *x = m_sum(m);
        sp(m);
        if (x && (m->p >= m->e || *m->p == ')')) { if (m->p < m->e) m->p++; result = x; }
        if (result && m->P->raw && result != m->made) { /* a calc() without an operator: a sum of one */
            struct css_calc *sum = node(m, N_SUM, 1);
            if (sum) { sum->k[0] = result; sum->type = result->type; }
            result = sum;
        }
        goto done;
    }
    bool size = word_is(name, name_n, "anchor-size");
    if (size || word_is(name, name_n, "anchor")) {
        if (m->P->anchors & (size ? LF_ANCHOR_SIZE : LF_ANCHOR) && m->P->cx)
            result = anchor_leaf(m, css_anchor_function_read(m->P, &m->p, m->e, size));
        goto done;
    }
    bool count = word_is(name, name_n, "sibling-count");
    if (count || word_is(name, name_n, "sibling-index")) {
        sp(m); /* no arguments */
        if (m->p < m->e && *m->p != ')') goto done;
        if (m->p < m->e) m->p++;
        if (m->P->raw) { m->P->unrepresentable = true; goto done; } /* no CSSMathValue (CSS Typed OM §4) */
        result = tree_leaf(m, count);
        goto done;
    }
    int fn = -1;
    for (int i = 0; i < F_COUNT; i++) if (word_is(name, name_n, fn_names[i])) fn = i;
    if (fn < 0) goto done;
    if (m->P->raw && fn != F_MIN && fn != F_MAX && fn != F_CLAMP) { m->P->unrepresentable = true; goto done; }
    int strategy = RS_NEAREST;
    if (fn == F_ROUND) {
        sp(m);
        for (int i = 0; i < 4; i++) {
            size_t k = strlen(round_names[i]);
            if ((size_t)(m->e - m->p) > k && word_is(m->p, k, round_names[i]) && !name_char((unsigned char)m->p[k])) {
                const char *save = m->p;
                m->p += k;
                sp(m);
                if (m->p < m->e && *m->p == ',') { m->p++; strategy = i; }
                else m->p = save;
                break;
            }
        }
    }
    struct list args = {0};
    if (!m_args(m, &args, fn == F_CLAMP)) goto done;
    static const unsigned min_args[F_COUNT] = {1, 1, 3, 1, 2, 2, 1, 1, 1, 1, 1, 1, 2, 2, 1, 1, 1, 1, 1, 1};
    static const unsigned max_args[F_COUNT] = {0xffff, 0xffff, 3, 2, 2, 2, 1, 1, 1, 1, 1, 1, 2, 2, 1, 0xffff, 2, 1, 1, 1};
    unsigned n = args.n;
    if (n < min_args[fn] || n > max_args[fn]) goto done;
    for (unsigned i = 0; i < n; i++) if (!args.v[i] && (fn != F_CLAMP || i == 1)) goto done;
    if (m->P->raw) for (unsigned i = 0; i < n; i++) if (!args.v[i]) { m->P->unrepresentable = true; goto done; }
    struct css_calc *c = node(m, N_FN, 0);
    if (!c) goto done;
    c->fn = (uint8_t)fn;
    c->key = (uint8_t)strategy;
    c->n = (uint16_t)n;
    c->k = args.v;
    /* the argument types and the function's (§10.7.1) */
    struct css_calc_type t = args.v[fn == F_CLAMP ? 1 : 0]->type;
    switch (fn) {
    case F_MIN: case F_MAX: case F_CLAMP: case F_HYPOT: case F_MOD: case F_REM: case F_ATAN2: case F_ROUND:
        if (fn == F_ROUND && n == 1 && !zero_type(&t)) goto done; /* the step is 1 only for a number */
        for (unsigned i = 0; i < n; i++)
            if (args.v[i] && !t_add(t, args.v[i]->type, &t)) goto done;
        if (fn == F_ATAN2) {
            struct css_calc_type a = t_number(&t);
            a.e[T_ANGLE] = 1;
            t = a;
        }
        break;
    case F_SIN: case F_COS: case F_TAN: {
        struct css_calc_type x = t;
        if (x.hint >= 0) t_hint(&x, x.hint);
        bool angle = true;
        for (int i = 0; i < T_BASES; i++) if (x.e[i] && (i != T_ANGLE || x.e[i] != 1)) angle = false;
        if (!zero_type(&x) && !angle) goto done;
        t = t_number(&t);
        break;
    }
    case F_ASIN: case F_ACOS: case F_ATAN:
        if (!zero_type(&t)) goto done;
        t = t_number(&t);
        t.e[T_ANGLE] = 1;
        break;
    case F_POW: case F_LOG: case F_SQRT: case F_EXP:
        for (unsigned i = 0; i < n; i++) {
            if (!zero_type(&args.v[i]->type) || !t_add(t, args.v[i]->type, &t)) goto done;
        }
        t = t_number(&t);
        break;
    case F_ABS: break;
    case F_SIGN: t = t_number(&t); break;
    }
    c->type = t;
    result = simplify(m, c);
done:
    m->P->depth--;
    return result;
}
static struct css_calc *m_value(struct mp *m) {
    sp(m);
    if (m->p >= m->e) return NULL;
    if (*m->p == '(') {
        m->p++;
        if (++m->P->depth > CALC_DEPTH) return NULL;
        struct css_calc *x = m_sum(m);
        sp(m);
        m->P->depth--;
        if (!x || (m->p < m->e && *m->p != ')')) return NULL;
        if (m->p < m->e) m->p++;
        return x;
    }
    if (number_start(m->p, m->e)) {
        double v;
        const char *u;
        size_t un;
        if (!css_numeric_token(&m->p, m->e, &v, &u, &un)) return NULL;
        int unit = css_unit_find(u, un);
        return unit < 0 ? NULL : m_leaf(m, v, unit);
    }
    if (!ident_start(m->p, m->e)) return NULL;
    const char *name = m->p;
    while (m->p < m->e && name_char((unsigned char)*m->p)) m->p++;
    size_t name_n = (size_t)(m->p - name);
    if (m->p < m->e && *m->p == '(') {
        m->p++;
        return m_function(m, name, name_n);
    }
    for (unsigned i = 0; i < m->P->nkeys; i++)
        if (m->P->keys && m->P->keys[i] && word_is(name, name_n, m->P->keys[i])) {
            struct css_calc *c = node(m, N_KEY, 0);
            if (c) c->key = (uint8_t)i;
            return c;
        }
    static const struct { const char *name; double v; } constants[] = {
        {"e", M_E}, {"pi", M_PI}, {"infinity", INFINITY}, {"-infinity", -INFINITY}, {"nan", NAN}};
    for (size_t i = 0; i < sizeof constants / sizeof *constants; i++)
        if (word_is(name, name_n, constants[i].name)) return m_leaf(m, constants[i].v, U_NUMBER);
    return NULL;
}
static struct css_calc *m_product(struct mp *m) {
    struct list items = {0};
    struct css_calc *x = m_value(m);
    if (!push(m, &items, x)) return NULL;
    struct css_calc_type t = x->type;
    for (;;) {
        const char *save = m->p;
        sp(m);
        if (m->p >= m->e || (*m->p != '*' && *m->p != '/')) { m->p = save; break; }
        bool divide = *m->p++ == '/';
        struct css_calc *v = m_value(m);
        if (!v) return NULL;
        if (divide) {
            struct css_calc *inv = node(m, N_INVERT, 1);
            if (!inv) return NULL;
            inv->k[0] = v;
            inv->type = t_inv(v->type);
            v = simplify(m, inv);
        }
        if (!v || !t_mul(t, v->type, &t) || !push(m, &items, v)) return NULL;
    }
    if (items.n == 1) return items.v[0];
    struct css_calc *c = node(m, N_PRODUCT, 0);
    if (!c) return NULL;
    c->n = (uint16_t)items.n;
    c->k = items.v;
    c->type = t;
    m->made = c;
    return simplify(m, c);
}
static struct css_calc *m_sum(struct mp *m) {
    struct list items = {0};
    struct css_calc *x = m_product(m);
    if (!push(m, &items, x)) return NULL;
    struct css_calc_type t = x->type;
    for (;;) {
        const char *before = m->p;
        sp(m);
        if (m->p >= m->e || (*m->p != '+' && *m->p != '-')) { m->p = before; break; }
        /* + and - need white space on both sides (§10.1) */
        if (m->p == before || m->p + 1 >= m->e || !is_space((unsigned char)m->p[1])) return NULL;
        bool minus = *m->p++ == '-';
        struct css_calc *v = m_product(m);
        if (!v) return NULL;
        if (minus) {
            struct css_calc *neg = node(m, N_NEGATE, 1);
            if (!neg) return NULL;
            neg->k[0] = v;
            neg->type = v->type;
            v = simplify(m, neg);
        }
        if (!v || !t_add(t, v->type, &t) || !push(m, &items, v)) return NULL;
    }
    if (items.n == 1) return items.v[0];
    struct css_calc *c = node(m, N_SUM, 0);
    if (!c) return NULL;
    c->n = (uint16_t)items.n;
    c->k = items.v;
    c->type = t;
    m->made = c;
    return simplify(m, c);
}
/* the base percentages have in a context that accepts accept */
static int8_t pct_base(unsigned accept) {
    if (!(accept & CSS_CALC_PERCENT)) return -1;
    int8_t base = T_PERCENT;
    unsigned dims = 0;
    for (int8_t i = 0; i < T_PERCENT; i++)
        if (accept & base_kind[i]) { dims++; base = i; }
    return dims == 1 ? base : T_PERCENT;
}
bool css_calc_is_math(const char *s, size_t n) {
    const char *p = s, *e = s + n;
    while (p < e && is_space((unsigned char)*p)) p++;
    const char *name = p;
    while (p < e && name_char((unsigned char)*p)) p++;
    if (p == name || p >= e || *p != '(') return false;
    size_t k = (size_t)(p - name);
    if (word_is(name, k, "calc") || word_is(name, k, "-webkit-calc")) return true;
    if (word_is(name, k, "sibling-index") || word_is(name, k, "sibling-count")) return true;
    for (int i = 0; i < F_COUNT; i++) if (word_is(name, k, fn_names[i])) return true;
    return false;
}
struct css_calc *css_calc_read(struct css_calc_parse *P, unsigned accept, const char *s, size_t n, unsigned *kind) {
    struct mp m = {P, s, s + n, pct_base(accept), accept != CSS_CALC_ANY, NULL};
    sp(&m);
    const char *name = m.p;
    while (m.p < m.e && name_char((unsigned char)*m.p)) m.p++;
    if (m.p == name || m.p >= m.e || *m.p != '(') return NULL;
    size_t name_n = (size_t)(m.p - name);
    m.p++;
    struct css_calc *c = m_function(&m, name, name_n);
    if (!c) return NULL;
    sp(&m);
    if (m.p != m.e) return NULL;
    unsigned k = t_match(&c->type, accept, m.pct);
    if (!k) return NULL;
    if (kind) *kind = k;
    return c;
}
struct css_calc *css_calc_key(arena_t *a, unsigned index) {
    struct css_calc *c = ar_alloc(a, sizeof *c);
    c->op = N_KEY;
    c->key = (uint8_t)index;
    c->type.hint = -1;
    return c;
}
int css_calc_key_of(const struct css_calc *c) { return c && c->op == N_KEY ? c->key : -1; }
bool css_calc_function_root(const struct css_calc *c) { return c && (c->op == N_FN || c->op == N_ANCHOR || c->op == N_TREE); }
/* anchor() and anchor-size() have no Typed OM object: OTHER */
int css_calc_shape(const struct css_calc *c, double *v, int *unit, unsigned *n, struct css_calc *const **children) {
    *n = c ? c->n : 0;
    *children = c ? c->k : NULL;
    if (!c) return CSS_MATH_OTHER;
    switch (c->op) {
    case N_VALUE: *v = c->v; *unit = c->unit; return CSS_MATH_VALUE;
    case N_SUM: return CSS_MATH_SUM;
    case N_PRODUCT: return CSS_MATH_PRODUCT;
    case N_NEGATE: return CSS_MATH_NEGATE;
    case N_INVERT: return CSS_MATH_INVERT;
    case N_FN: return c->fn == F_MIN ? CSS_MATH_MIN : c->fn == F_MAX ? CSS_MATH_MAX : c->fn == F_CLAMP ? CSS_MATH_CLAMP :
                      CSS_MATH_OTHER;
    }
    return CSS_MATH_OTHER;
}
bool css_calc_constant(const struct css_calc *c, double *v) {
    if (!known(c)) return false;
    *v = c->v;
    return true;
}
bool css_calc_linear(const struct css_calc *c, double *px, double *pct) {
    *px = *pct = 0;
    const struct css_calc *leaves[2] = {c, NULL};
    if (c->op == N_SUM) {
        if (c->n != 2) return false;
        leaves[0] = c->k[0];
        leaves[1] = c->k[1];
    }
    bool have_px = false, have_pct = false;
    for (int i = 0; i < 2 && leaves[i]; i++) {
        const struct css_calc *x = leaves[i];
        if (!is_leaf(x) || !isfinite(x->v)) return false;
        if (x->unit == U_PX && !have_px) { *px = x->v; have_px = true; }
        else if (x->unit == U_PERCENT && x->type.hint == T_LENGTH && !have_pct) { *pct = x->v; have_pct = true; }
        else return false;
    }
    return true;
}
bool css_calc_context_free(const struct css_calc *c) {
    if (!c) return true;
    if (c->op == N_KEY || c->op == N_TREE) return false;
    if (c->op == N_VALUE) return units[c->unit].rel == R_ABS;
    for (unsigned i = 0; i < c->n; i++) if (!css_calc_context_free(c->k[i])) return false;
    return true;
}
bool css_calc_equal(const struct css_calc *a, const struct css_calc *b) {
    if (a == b) return true;
    if (!a || !b || a->op != b->op || a->fn != b->fn || a->unit != b->unit || a->key != b->key || a->n != b->n) return false;
    if (a->op == N_VALUE && !(a->v == b->v ? signbit(a->v) == signbit(b->v) : isnan(a->v) && isnan(b->v))) return false;
    if (a->op == N_ANCHOR && !css_anchor_function_equal(a->anchor, b->anchor)) return false;
    for (unsigned i = 0; i < a->n; i++) if (!css_calc_equal(a->k[i], b->k[i])) return false;
    return true;
}
/* a copy of c with its relative lengths in pixels for cx, simplified again */
static struct css_calc *rebuild(struct mp *m, const struct css_calc *c) {
    if (!c) return NULL;
    if (c->op == N_VALUE) {
        if (units[c->unit].rel == R_ABS) return (struct css_calc *)c;
        return mk_leaf(m, css_unit_canonical(c->unit, c->v, m->P->cx), U_PX, c->type);
    }
    if (c->op == N_KEY || c->op == N_ANCHOR) return (struct css_calc *)c;
    if (c->op == N_TREE) return m->P->cx && m->P->cx->node ? tree_leaf(m, c->fn) : (struct css_calc *)c;
    struct css_calc **k = ar_alloc(m->P->a, c->n * sizeof *k);
    for (unsigned i = 0; i < c->n; i++)
        if (c->k[i] && !(k[i] = rebuild(m, c->k[i]))) return NULL;
    return simplify(m, with_children(m, c, k, c->n));
}
struct css_calc *css_calc_bind(arena_t *a, const struct css_calc *c, const struct cx *cx) {
    if (css_calc_context_free(c)) return (struct css_calc *)c;
    struct css_calc_parse P = {.a = a, .cx = cx};
    struct mp m = {&P, NULL, NULL, T_PERCENT, false, NULL};
    return rebuild(&m, c);
}
bool css_calc_anchored(const struct css_calc *c) {
    if (!c) return false;
    if (c->op == N_ANCHOR) return true;
    for (unsigned i = 0; i < c->n; i++) if (css_calc_anchored(c->k[i])) return true;
    return false;
}
static struct css_calc *map_anchors(struct mp *m, const struct css_calc *c, css_calc_anchor_map map, void *context) {
    if (c->op == N_ANCHOR) return map(context, m->P->a, c->anchor);
    if (++m->P->depth > CALC_DEPTH * 4) return NULL;
    struct css_calc *r = node(m, c->op, c->n);
    if (!r) return NULL;
    r->fn = c->fn; r->unit = c->unit; r->key = c->key; r->type = c->type; r->v = c->v;
    for (unsigned i = 0; i < c->n; i++)
        if (c->k[i] && !(r->k[i] = map_anchors(m, c->k[i], map, context))) return NULL;
    m->P->depth--;
    return simplify(m, r);
}
struct css_calc *css_calc_map_anchors(arena_t *a, const struct css_calc *c, css_calc_anchor_map map, void *context) {
    if (!c) return NULL;
    struct css_calc_parse P = {.a = a};
    struct mp m = {&P, NULL, NULL, T_LENGTH, false, NULL};
    return map_anchors(&m, c, map, context);
}
struct css_calc *css_calc_anchor(arena_t *a, const struct css_anchor_function *f) {
    struct css_calc_parse P = {.a = a};
    struct mp m = {&P, NULL, NULL, T_LENGTH, false, NULL};
    return anchor_leaf(&m, f);
}

/* ---------------------------------------------------------------- serialization (§10.13) */
/* d: the document a computed value belongs to, whose native work the walk checkpoints every 64
   nodes (a cancelled document stops it and the output rolls back); NULL: none */
struct out { sbuf *b; bool ok; web_doc *d; unsigned work; };
static void put(struct out *o, const char *s, size_t n) {
    sbuf *b = o->b;
    if (!o->ok) return;
    if (b->n == SIZE_MAX || n > SIZE_MAX - b->n - 1) { o->ok = false; return; }
    size_t need = b->n + n + 1;
    if (need > b->cap) {
        size_t cap = b->cap ? b->cap : 64;
        while (cap < need) {
            if (cap > SIZE_MAX / 2) { cap = need; break; }
            cap *= 2;
        }
        char *p = realloc(b->p, cap);
        if (!p) { o->ok = false; return; }
        b->p = p;
        b->cap = cap;
    }
    memcpy(b->p + b->n, s, n);
    b->n += n;
    b->p[b->n] = 0;
}
static void puts_(struct out *o, const char *s) { put(o, s, strlen(s)); }
/* a <number> as CSS serializes it: at most six significant figures, no exponent, no negative zero */
static void put_number(struct out *o, double v) {
    char t[400];
    if (isnan(v) || fabs(v) < 5e-7) v = 0;
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
    puts_(o, t);
}
void css_number_serialize(sbuf *b, double v) {
    struct out o = {b, true, NULL, 0};
    put_number(&o, v);
}
static void put_leaf(struct out *o, const struct css_calc *c) {
    if (!isfinite(c->v)) {
        puts_(o, isnan(c->v) ? "NaN" : c->v > 0 ? "infinity" : "-infinity");
        if (c->unit != U_NUMBER) {
            puts_(o, " * 1");
            puts_(o, units[c->unit].name);
        }
        return;
    }
    put_number(o, c->v);
    puts_(o, units[c->unit].name);
}
/* sort a calculation's children (§10.13): a number, a percentage, the dimensions by unit, the rest */
static void sorted(const struct css_calc *c, const struct css_calc **out) {
    unsigned k = 0;
    for (int pass = 0; pass < 2; pass++)
        for (unsigned i = 0; i < c->n; i++)
            if (is_leaf(c->k[i]) && order_unit(c->k[i]) == pass) out[k++] = c->k[i];
    unsigned dims = k;
    for (unsigned i = 0; i < c->n; i++) if (is_leaf(c->k[i]) && order_unit(c->k[i]) == 2) out[k++] = c->k[i];
    for (unsigned i = dims + 1; i < k; i++)
        for (unsigned j = i; j > dims && strcmp(units[out[j - 1]->unit].name, units[out[j]->unit].name) > 0; j--) {
            const struct css_calc *x = out[j];
            out[j] = out[j - 1];
            out[j - 1] = x;
        }
    for (unsigned i = 0; i < c->n; i++) if (!is_leaf(c->k[i])) out[k++] = c->k[i];
}
static void put_tree(struct out *o, const struct css_calc *c, const char *const *keys, bool bare);
static void put_fn(struct out *o, const struct css_calc *c, const char *const *keys) {
    puts_(o, fn_names[c->fn]);
    put(o, "(", 1);
    if (c->fn == F_ROUND && c->key != RS_NEAREST) { puts_(o, round_names[c->key]); put(o, ", ", 2); }
    for (unsigned i = 0; i < c->n; i++) {
        if (i) put(o, ", ", 2);
        if (c->k[i]) put_tree(o, c->k[i], keys, true);
        else puts_(o, "none");
    }
    put(o, ")", 1);
}
/* "serialize a calculation tree"; bare: directly in calc( or a function's argument, without parentheses */
static void put_tree(struct out *o, const struct css_calc *c, const char *const *keys, bool bare) {
    if (!o->ok) return;
    if (o->d && !(++o->work & 63) && !web_native_checkpoint(o->d)) { o->ok = false; return; }
    switch (c->op) {
    case N_VALUE: put_leaf(o, c); return;
    case N_KEY: puts_(o, keys && keys[c->key] ? keys[c->key] : "none"); return;
    case N_FN: put_fn(o, c, keys); return;
    case N_ANCHOR: if (o->ok && !css_anchor_function_text(o->b, c->anchor)) o->ok = false; return;
    case N_TREE: puts_(o, c->fn ? "sibling-count()" : "sibling-index()"); return;
    case N_NEGATE: case N_INVERT:
        if (!bare) put(o, "(", 1);
        puts_(o, c->op == N_NEGATE ? "-1 * " : "1 / ");
        put_tree(o, c->k[0], keys, false);
        if (!bare) put(o, ")", 1);
        return;
    }
    const struct css_calc *local[16], **k = c->n <= 16 ? local : malloc(c->n * sizeof *k);
    if (!k) { o->ok = false; return; }
    sorted(c, k);
    if (!bare) put(o, "(", 1);
    for (unsigned i = 0; i < c->n; i++) {
        const struct css_calc *x = k[i];
        if (!i) put_tree(o, x, keys, false);
        else if (c->op == N_SUM && x->op == N_NEGATE) { put(o, " - ", 3); put_tree(o, x->k[0], keys, false); }
        else if (c->op == N_SUM && is_leaf(x) && (x->v < 0 || (x->v == 0 && signbit(x->v)))) {
            struct css_calc neg = *x;
            neg.v = -x->v;
            put(o, " - ", 3);
            put_leaf(o, &neg);
        } else if (c->op == N_PRODUCT && x->op == N_INVERT) { put(o, " / ", 3); put_tree(o, x->k[0], keys, false); }
        else { put(o, c->op == N_SUM ? " + " : " * ", 3); put_tree(o, x, keys, false); }
    }
    if (!bare) put(o, ")", 1);
    if (k != local) free((void *)k);
}
static bool calc_serialize(sbuf *b, const struct css_calc *c, const char *const *keys, web_doc *d) {
    if (!b || !c) return false;
    size_t begin = b->n;
    struct out o = {b, true, d, 0};
    if (c->op == N_FN) put_fn(&o, c, keys);
    else if (c->op == N_ANCHOR || c->op == N_TREE) put_tree(&o, c, keys, true); /* a function of its own, not in calc() */
    else {
        put(&o, "calc(", 5);
        put_tree(&o, c, keys, true);
        put(&o, ")", 1);
    }
    if (!o.ok) {
        b->n = begin;
        if (b->p && begin < b->cap) b->p[begin] = 0;
    }
    return o.ok;
}
bool css_calc_serialize(sbuf *b, const struct css_calc *c, const char *const *keys) {
    return calc_serialize(b, c, keys, NULL);
}

/* ---------------------------------------------------------------- values that use it */
/* one numeric value: a literal or a math function (see webi.h) */
bool css_numeric_read(const char *s, size_t n, unsigned accept, const struct cx *cx, arena_t *a, double basis, double *out,
                      unsigned *kind) {
    while (n && is_space((unsigned char)*s)) { s++; n--; }
    while (n && is_space((unsigned char)s[n - 1])) n--;
    const char *p = s;
    double v;
    const char *u;
    size_t un;
    if (css_numeric_token(&p, s + n, &v, &u, &un)) {
        if (p != s + n) return false;
        int unit = css_unit_find(u, un);
        unsigned k = css_unit_kind(unit);
        if (!k || !(accept & k)) return false;
        if (unit == U_PERCENT) {
            if (cx && cx->absolute_lengths) return false;
            if (pct_base(accept) != T_PERCENT) {
                if (isnan(basis)) return false;
                v = v * basis / 100;
                k = base_kind[pct_base(accept)];
            }
        } else if (css_unit_relative(unit) && cx && cx->absolute_lengths) return false;
        else v = css_unit_canonical(unit, v, cx);
        *out = v;
        if (kind) *kind = k;
        return true;
    }
    if (!css_calc_is_math(s, n)) return false;
    /* the tree is not kept: a's memory, or a scratch arena when the caller has none */
    arena_t own = {.chunk_size = 1024};
    jmp_buf trap;
    if (!a) {
        a = &own;
        own.trap = &trap;
        if (setjmp(trap)) { ar_free(&own); return false; }
    }
    struct css_calc_parse P = {.a = a, .cx = cx};
    unsigned k = 0;
    struct css_calc *c = css_calc_read(&P, accept, s, n, &k);
    /* a percentage that needs a basis the caller does not have fails */
    bool ok = c && (!isnan(basis) || css_calc_percent_free(c));
    if (ok) {
        {
            v = css_calc_value(c, basis, NULL, cx);
            *out = isnan(v) ? 0 : v; /* §10.9: a top-level NaN is 0 */
            if (kind) *kind = k;
        }
    }
    if (a == &own) ar_free(&own);
    return ok;
}
bool css_calc_percent_free(const struct css_calc *c) {
    if (!c) return true;
    if (c->op == N_VALUE) return c->unit != U_PERCENT || own_percent(c);
    for (unsigned i = 0; i < c->n; i++) if (!css_calc_percent_free(c->k[i])) return false;
    return true;
}
/* the specified value of text with math functions in it (see webi.h) */
bool css_calc_specified(const char *s, size_t n, unsigned accept, sbuf *out) {
    const char *p = s, *e = s + n, *copied = s;
    bool any = false, ok = true;
    size_t begin = out->n;
    arena_t a = {.chunk_size = 1024};
    jmp_buf trap;
    a.trap = &trap;
    if (setjmp(trap)) { ar_free(&a); out->n = begin; if (out->p && begin < out->cap) out->p[begin] = 0; return false; }
    struct out o = {out, true, NULL, 0};
    while (p < e && ok) {
        char c = *p;
        if (c == '"' || c == '\'') {
            for (p++; p < e && *p != c; p++) if (*p == '\\' && p + 1 < e) p++;
            if (p < e) p++;
            continue;
        }
        if (c == '#') { for (p++; p < e && name_char((unsigned char)*p); p++) {} continue; } /* a hash, not a number */
        /* a number, percentage or dimension outside a math function serializes as CSSOM's "serialize a
           CSS component value" does (.5em is 0.5em, -0 is 0, +5.0E1PX is 50px); an unknown unit's as written */
        if (number_start(p, e) && !(p > s && (name_char((unsigned char)p[-1]) || p[-1] == '.'))) {
            const char *token = p, *unit;
            size_t unit_n;
            double v;
            if (!css_numeric_token(&p, e, &v, &unit, &unit_n)) { p++; continue; }
            int u = css_unit_find(unit, unit_n);
            if (u < 0 || !isfinite(v)) continue;
            put(&o, copied, (size_t)(token - copied));
            put_number(&o, v);
            puts_(&o, units[u].name);
            any = true;
            copied = p;
            continue;
        }
        if (!ident_start(p, e) || (p > s && name_char((unsigned char)p[-1]))) { p++; continue; }
        const char *name = p;
        while (p < e && name_char((unsigned char)*p)) p++;
        if (p >= e || *p != '(') continue;
        size_t name_n = (size_t)(p - name);
        /* a value with var(), env() or attr() is a token stream until it is computed: as written */
        if (word_is(name, name_n, "var") || word_is(name, name_n, "env") || word_is(name, name_n, "attr")) { ok = false; break; }
        bool url = word_is(name, name_n, "url"), math = css_calc_is_math(name, (size_t)(e - name));
        if (!url && !math) { p++; continue; }
        /* the function's text: up to its ')' */
        const char *q = p + 1;
        unsigned depth = 0;
        for (; q < e; q++) {
            if (*q == '"' || *q == '\'') { char d = *q; for (q++; q < e && *q != d; q++) if (*q == '\\' && q + 1 < e) q++; continue; }
            if (*q == '\\' && q + 1 < e) { q++; continue; }
            if (*q == '(') depth++;
            else if (*q == ')') { if (!depth) break; depth--; }
        }
        if (q >= e) q = e - 1; /* the end of the value closes it */
        if (url) { p = q + 1; continue; }
        struct css_calc_parse P = {.a = &a};
        struct css_calc *x = css_calc_read(&P, accept, name, (size_t)(q + 1 - name), NULL);
        if (!x && accept != CSS_CALC_ANY) x = css_calc_read(&P, CSS_CALC_ANY, name, (size_t)(q + 1 - name), NULL);
        if (!x) { ok = false; break; }
        put(&o, copied, (size_t)(name - copied));
        if (!css_calc_serialize(out, x, NULL)) { ok = false; break; }
        any = true;
        p = copied = q + 1;
    }
    if (ok && any) put(&o, copied, (size_t)(e - copied));
    ar_free(&a);
    if (!ok || !any || !o.ok) {
        out->n = begin;
        if (out->p && begin < out->cap) out->p[begin] = 0;
        return false;
    }
    return true;
}

/* ---------------------------------------------------------------- <length-percentage> (len_t) */
/* lengths are kept within +-LEN_MAX px, so that sums of them stay finite and fit an int */
#define LEN_MAX 1e7f
static float len_clamp(double v) { return v != v ? 0 : v > LEN_MAX ? LEN_MAX : v < -LEN_MAX ? -LEN_MAX : (float)v; }
float len_resolve(const len_t *l, float base) {
    switch (l->kind) {
    case LK_LEN: return len_clamp(l->px + (l->pct ? (double)l->pct * base / 100 : 0));
    case LK_NUMBER: return len_clamp(l->px);
    case LK_EXPR: return len_clamp(css_calc_value(l->expr, base, NULL, NULL));
    }
    return 0;
}
bool css_length_equal(const len_t *a, const len_t *b) {
    return a == b || (a && b && a->kind == b->kind && a->px == b->px && a->pct == b->pct && css_calc_equal(a->expr, b->expr));
}
bool css_length_from_calc(const struct css_calc *c, bool negative, len_t *out) {
    double px, pct;
    memset(out, 0, sizeof *out);
    if (css_calc_linear(c, &px, &pct)) {
        /* a computed value in range (§10.12): what the property forbids is clamped */
        if (!negative && pct == 0 && px < 0) px = 0;
        out->kind = LK_LEN;
        out->px = (float)px;
        out->pct = (float)pct;
        return true;
    }
    if (is_leaf(c) && c->unit == U_PX) {
        /* infinite or NaN: censored (§10.9) and clamped to the range lengths keep */
        double v = isnan(c->v) ? 0 : c->v;
        if (!negative && v < 0) v = 0;
        out->kind = LK_LEN;
        out->px = len_clamp(v);
        return true;
    }
    out->kind = LK_EXPR;
    out->expr = (struct css_calc *)c;
    return true;
}
/* a computed <length-percentage> as a tree */
static struct css_calc *length_tree(struct mp *m, const len_t *l) {
    if (l->kind == LK_EXPR) return l->expr;
    struct css_calc_type px = {{0}, -1}, pct = {{0}, T_LENGTH};
    px.e[T_LENGTH] = pct.e[T_LENGTH] = 1;
    struct css_calc *a = mk_leaf(m, l->px, U_PX, px), *b = mk_leaf(m, l->pct, U_PERCENT, pct);
    if (!a || !b) return NULL;
    if (l->pct == 0 && !signbit(l->pct)) return a;
    if (l->px == 0) return b;
    struct css_calc *s = node(m, N_SUM, 2);
    if (!s) return NULL;
    s->k[0] = b;
    s->k[1] = a;
    s->type = pct;
    return s;
}
struct css_calc *css_calc_length(arena_t *a, const len_t *l) {
    struct css_calc_parse P = {.a = a};
    struct mp m = {&P, NULL, NULL, T_LENGTH, false, NULL};
    return length_tree(&m, l);
}
static struct css_calc *scaled(struct mp *m, struct css_calc *x, double k) {
    struct css_calc_type none = {{0}, -1};
    struct css_calc *f = mk_leaf(m, k, U_NUMBER, none), *p = node(m, N_PRODUCT, 2);
    if (!f || !p || !x) return NULL;
    p->k[0] = f;
    p->k[1] = x;
    p->type = x->type;
    return simplify(m, p);
}
static void length_combine(arena_t *a, struct css_calc *const *terms, unsigned n, len_t *out) {
    struct css_calc_parse P = {.a = a};
    struct mp m = {&P, NULL, NULL, T_LENGTH, false, NULL};
    struct css_calc *s = node(&m, N_SUM, n);
    bool ok = s != NULL;
    struct css_calc_type t = {{0}, T_LENGTH};
    t.e[T_LENGTH] = 1;
    for (unsigned i = 0; ok && i < n; i++) ok = (s->k[i] = terms[i]) != NULL;
    if (ok) {
        s->type = t;
        s = simplify(&m, s);
        ok = s != NULL;
    }
    if (!ok || !css_length_from_calc(s, true, out)) *out = (len_t){.kind = LK_LEN};
}
void css_length_complement(arena_t *a, const len_t *x, const len_t *y, len_t *out) {
    if (x->kind == LK_LEN && (!y || y->kind == LK_LEN)) {
        *out = (len_t){.kind = LK_LEN, .px = -x->px - (y ? y->px : 0), .pct = 100 - x->pct - (y ? y->pct : 0)};
        return;
    }
    struct css_calc_parse P = {.a = a};
    struct mp m = {&P, NULL, NULL, T_LENGTH, false, NULL};
    len_t hundred = {.kind = LK_LEN, .pct = 100};
    struct css_calc *terms[3] = {length_tree(&m, &hundred), scaled(&m, length_tree(&m, x), -1),
                                 y ? scaled(&m, length_tree(&m, y), -1) : NULL};
    length_combine(a, terms, y ? 3 : 2, out);
}
void css_length_mix(arena_t *a, const len_t *x, const len_t *y, double ka, double kb, len_t *out) {
    if (x->kind == LK_LEN && y->kind == LK_LEN) {
        *out = (len_t){.kind = LK_LEN, .px = (float)(x->px * ka + y->px * kb), .pct = (float)(x->pct * ka + y->pct * kb)};
        return;
    }
    struct css_calc_parse P = {.a = a};
    struct mp m = {&P, NULL, NULL, T_LENGTH, false, NULL};
    struct css_calc *terms[2] = {scaled(&m, length_tree(&m, x), ka), scaled(&m, length_tree(&m, y), kb)};
    length_combine(a, terms, 2, out);
}
/* a computed <length-percentage>'s serialization (CSSOM: percentages and math kept, never a zero made up) */
bool css_length_serialize(const len_t *l, sbuf *out, web_doc *d) {
    if (!l || !out || out->n > out->cap || (!out->p && (out->n || out->cap))) return false;
    size_t begin = out->n;
    struct out o = {out, true, NULL, 0};
    char t[64];
    switch (l->kind) {
    case LK_AUTO: puts_(&o, "auto"); break;
    case LK_NONE: puts_(&o, "none"); break;
    case LK_NORMAL: puts_(&o, "normal"); break;
    case LK_NUMBER: case LK_LEN: {
        if (!isfinite(l->px) || !isfinite(l->pct) || (l->kind == LK_NUMBER && l->pct)) { o.ok = false; break; }
        float px = l->px == 0 ? 0 : l->px, pct = l->pct == 0 ? 0 : l->pct;
        if (l->kind == LK_NUMBER) { snprintf(t, sizeof t, "%.9g", (double)px); puts_(&o, t); break; }
        if (pct == 0) { snprintf(t, sizeof t, "%.9gpx", (double)px); puts_(&o, t); break; }
        if (px == 0) { snprintf(t, sizeof t, "%.9g%%", (double)pct); puts_(&o, t); break; }
        /* §10.13 sorts a sum's terms: the percentage before the dimensions */
        snprintf(t, sizeof t, "calc(%.9g%% %c %.9gpx)", (double)pct, px < 0 ? '-' : '+', (double)fabsf(px));
        puts_(&o, t);
        break;
    }
    case LK_EXPR: if (!l->expr || !calc_serialize(out, l->expr, NULL, d)) o.ok = false; break;
    default: o.ok = false;
    }
    if (!o.ok) {
        out->n = begin;
        if (out->p && begin < out->cap) out->p[begin] = 0;
    }
    return o.ok;
}
/* Media feature lengths have no percentage basis or property range clamp. */
bool css_query_length(const char *text, size_t length, float em, float rem, float vw, float vh, double *value) {
    if (!text || !value) return false;
    struct cx context = {.em = em, .rem = rem, .vw = vw, .vh = vh};
    double v;
    unsigned kind;
    if (!css_calc_is_math(text, length) || !css_numeric_read(text, length, CSS_CALC_LENGTH, &context, NULL, NAN, &v, &kind) ||
        !isfinite(v)) return false;
    *value = v;
    return true;
}
