/* CSS properties: the table of supported properties, value parsing (lengths, calc(), colours,
   keywords), shorthand expansion, and the user agent stylesheet. */
#include <stdio.h>
#include <math.h>
#include <stddef.h>
#include <ctype.h>
#include "webi.h"
#include "inspect_private.h"
#include "svg_geometry.h"

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

/* Rendering retains its existing recovery behavior; feature queries must not
   report success after a parser silently drops extra tokens. */
static int split_checked(const char *s, size_t n, const char **tok, size_t *tl, int max, char sep, struct cx *cx) {
    int k = split(s, n, tok, tl, max, sep);
    if (cx->supports_probe && k) {
        const char *end = tok[k - 1] + tl[k - 1];
        while (end < s + n && is_space((unsigned char)*end)) end++;
        if (end != s + n) cx->invalid = true;
        for (int i = 0; i < k; i++) if (!tl[i]) cx->invalid = true;
    }
    return k;
}

/* Resolve name-character escapes without turning escaped punctuation into CSS
   syntax. Used by both the cascade and queries. Quoted strings remain encoded
   for the existing string parser. Canonical var() spelling also makes the
   cascade's substitution dispatch case-insensitive. */
const char *css_value_canonical(arena_t *a, const char *s, size_t n, size_t *out_n) {
    *out_n = n;
    bool needed = memchr(s, '\\', n) != NULL;
    for (size_t i = 0; !needed && i + 4 <= n; i++)
        if (strn_ieq(s + i, "var(", 4) && (s[i] != 'v' || s[i + 1] != 'a' || s[i + 2] != 'r')) needed = true;
    if (!needed) return s;
    char *out = ar_alloc(a, n * 3 + 1);
    size_t k = 0;
    char quote = 0;
    for (size_t i = 0; i < n;) {
        char c = s[i];
        if (quote) {
            out[k++] = s[i++];
            if (c == '\\' && i < n) out[k++] = s[i++];
            else if (c == quote) quote = 0;
            continue;
        }
        if (c == '"' || c == '\'') { quote = c; out[k++] = s[i++]; continue; }
        if (c == '\\' && i + 1 < n) {
            size_t start = i++;
            uint32_t cp = 0;
            int digits = 0;
            while (i < n && digits < 6 && isxdigit((unsigned char)s[i])) {
                int d = s[i] | 32;
                cp = cp * 16 + (uint32_t)(d <= '9' ? d - '0' : d - 'a' + 10);
                i++; digits++;
            }
            if (!digits) cp = (unsigned char)s[i++];
            else if (i < n && is_space((unsigned char)s[i])) {
                if (s[i++] == '\r' && i < n && s[i] == '\n') i++;
            }
            if (!cp || cp > 0x10ffff || (cp >= 0xd800 && cp <= 0xdfff)) cp = 0xfffd;
            if (cp >= 128 || isalnum((unsigned char)cp) || cp == '-' || cp == '_') {
                char u[4]; int len = utf8_put(u, cp);
                memcpy(out + k, u, (size_t)len); k += (size_t)len;
            } else { memcpy(out + k, s + start, i - start); k += i - start; }
            continue;
        }
        out[k++] = s[i++];
    }
    out[k] = 0;
    /* At a function-token boundary only; names such as myVAR() are unchanged. */
    for (size_t i = 0; i + 4 <= k;) {
        char c = out[i];
        if (c == '"' || c == '\'') {
            i++; while (i < k && out[i] != c) { if (out[i] == '\\') i++; i++; } i++;
        } else if (strn_ieq(out + i, "var(", 4) && (!i || !(isalnum((unsigned char)out[i - 1]) ||
                     out[i - 1] == '-' || out[i - 1] == '_' || (unsigned char)out[i - 1] >= 128))) {
            memcpy(out + i, "var", 3); i += 4;
        } else i++;
    }
    *out_n = k;
    return out;
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
            if (cp >= 0xD800 && cp <= 0xDFFF) cp = 0xFFFD;
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
        while (s < e && *s >= '0' && *s <= '9') {
            /* A huge exponent is still a number token, not signed-int UB. */
            if (ev < 10000) ev = ev * 10 + (*s - '0');
            s++;
        }
        v *= pow(10, es * ev);
    }
    *out = (float)(sign * v);
    *ps = s;
    return true;
}

/* The implemented filter backend is a real opacity function list. Preserve
   computed numbers/percentages (including values above one), while clipping
   only their used amounts. Unknown effects invalidate the whole declaration. */
static bool parse_filter(const char *v, size_t n, struct cx *cx, const struct css_filter **out) {
    if (ident_is(v, n, "none")) { *out = NULL; return true; }
    size_t count = 0;
    const char *end = v + n;
    struct css_filter *result = NULL;
    for (unsigned pass = 0; pass < 2; pass++) {
        const char *q = v;
        size_t i = 0;
        float used = 1;
        while (q < end) {
            while (q < end && is_space((unsigned char)*q)) q++;
            if (q == end) break;
            if ((size_t)(end - q) < 8 || !strn_ieq(q, "opacity", 7) || q[7] != '(') return false;
            q += 8;
            while (q < end && is_space((unsigned char)*q)) q++;
            float value = 1;
            bool percentage = false;
            if (q == end) return false;
            if (*q != ')') {
                if (!parse_number(&q, end, &value) || !isfinite(value) || value < 0) return false;
                if (q < end && *q == '%') { percentage = true; q++; }
                while (q < end && is_space((unsigned char)*q)) q++;
            }
            if (q == end || *q++ != ')') return false;
            if (i == SIZE_MAX) return false;
            if (pass) result->steps[i] = (struct css_filter_step){value, percentage};
            i++;
            float amount = percentage ? value / 100 : value;
            used *= amount > 1 ? 1 : amount;
            if (!(i & 63) && !cx->supports_probe && cx->node && !web_native_checkpoint(cx->node->owner)) return false;
        }
        if (!i) return false;
        if (!pass) {
            count = i;
            if (count > (SIZE_MAX - sizeof *result) / sizeof *result->steps) return false;
            result = ar_alloc(cx->a, sizeof *result + count * sizeof *result->steps);
        } else { result->count = count; result->opacity = used; }
    }
    *out = result;
    return true;
}

/* ---------------------------------------------------------------- lengths and calc() */
struct cexpr {
    char op; /* 'v' value, + - * /, 'm' min, 'M' max */
    bool num;
    float px, pct;
    struct cexpr *a, *b;
};
static bool expression_equal(const struct cexpr *a,const struct cexpr *b){
    struct expression_pair {const struct cexpr *a,*b;} local[32],*pending=local;
    size_t count=0,capacity=32;bool equal=true;
    for(;;){
        if(a!=b){
            if(!a||!b||a->op!=b->op||(a->op=='v'&&(a->num!=b->num||a->px!=b->px||a->pct!=b->pct))){equal=false;break;}
            if(a->b!=b->b){
                if(count==capacity){size_t n=capacity*2;
                    if(n<capacity||n>SIZE_MAX/sizeof *pending){equal=false;break;}
                    struct expression_pair *v=malloc(n*sizeof *v);if(!v){equal=false;break;}
                    memcpy(v,pending,count*sizeof *v);if(pending!=local)free(pending);pending=v;capacity=n;
                }
                pending[count++]=(struct expression_pair){a->b,b->b};
            }
            a=a->a;b=b->a;continue;
        }
        if(!count)break;struct expression_pair p=pending[--count];a=p.a;b=p.b;
    }
    if(pending!=local)free(pending);return equal;
}
bool css_length_equal(const len_t *a,const len_t *b){
    return a==b||(a&&b&&a->kind==b->kind&&a->px==b->px&&a->pct==b->pct&&expression_equal(a->expr,b->expr));
}

enum { LF_AUTO = 1, LF_NONE = 2, LF_NEG = 4, LF_NORMAL = 8, LF_NOPCT = 16, LF_LENGTH = 32 };

/* a dimension: number + unit; false if the unit is unknown */
bool css_unit_px(const char *u, size_t un, float v, const struct cx *cx, float *px, float *pct, bool *num) {
    *px = *pct = 0;
    *num = false;
    if (!un) {
        *px = v;
        *num = true;
        return true;
    }
    if (un == 1 && u[0] == '%') {
        if (cx->absolute_lengths) return false;
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
        int kind; /* 0 absolute, 1 em, 2 rem, 3..6 viewport, 7..10 container */
        float f;
    } units[] = {
        {"px", 0, 1},     {"em", 1, 1},     {"rem", 2, 1},    {"ex", 1, 0.5f}, {"ch", 1, 0.6f},
        {"lh", 1, 1.2f},  {"rlh", 2, 1.2f}, {"cap", 1, 0.7f}, {"ic", 1, 1},    {"vw", 3, 1},
        {"vh", 4, 1},     {"svw", 3, 1},    {"svh", 4, 1},    {"lvw", 3, 1},   {"lvh", 4, 1},
        {"dvw", 3, 1},    {"dvh", 4, 1},    {"cqw", 7, 1},    {"cqh", 8, 1},   {"vi", 3, 1},
        {"vb", 4, 1},     {"vmin", 5, 1},   {"vmax", 6, 1},   {"cqmin", 9, 1}, {"cqmax", 10, 1},
        {"cqi", 7, 1},    {"cqb", 8, 1},
        {"pt", 0, 4.0f / 3}, {"pc", 0, 16}, {"in", 0, 96},    {"cm", 0, 96 / 2.54f}, {"mm", 0, 96 / 25.4f},
        {"q", 0, 96 / 101.6f},
    };
    for (size_t i = 0; i < sizeof units / sizeof *units; i++) {
        if (strcmp(units[i].u, b)) continue;
        if (cx->absolute_lengths && units[i].kind) return false;
        float f = units[i].f;
        switch (units[i].kind) {
        case 0: *px = v * f; break;
        case 1: *px = v * f * em; break;
        case 2: *px = v * f * cx->rem; break;
        case 3: *px = v * f * cx->vw / 100; break;
        case 4: *px = v * f * cx->vh / 100; break;
        case 5: *px = v * f * (cx->vw < cx->vh ? cx->vw : cx->vh) / 100; break;
        case 6: *px = v * f * (cx->vw > cx->vh ? cx->vw : cx->vh) / 100; break;
        case 7: *px = v * f * (cx->cq_width_set ? cx->cq_width : cx->vw) / 100; break;
        case 8: *px = v * f * (cx->cq_height_set ? cx->cq_height : cx->vh) / 100; break;
        case 9: {
            float w=cx->cq_width_set?cx->cq_width:cx->vw,h=cx->cq_height_set?cx->cq_height:cx->vh;
            *px=v*f*(w<h?w:h)/100;break;
        }
        case 10: {
            float w=cx->cq_width_set?cx->cq_width:cx->vw,h=cx->cq_height_set?cx->cq_height:cx->vh;
            *px=v*f*(w>h?w:h)/100;break;
        }
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
    if (cx->supports_probe && ++cx->supports_nodes > 512) cx->invalid = true;
    struct cexpr *x = ar_alloc(cx->a, sizeof *x);
    x->op = op;
    x->a = a;
    x->b = b;
    return x;
}

/* the arguments of min(), max() or clamp() up to the closing parenthesis */
static struct cexpr *calc_args(const char **s, const char *e, struct cx *cx, char op, bool clamp) {
    struct cexpr *result = NULL, *first = NULL, *second = NULL;
    /* min/max accept an author-sized list. Keep only clamp's grammatical
       arity, not an arbitrary fixed argument array or quota. Arena OOM is
       handled by the same allocation trap as the expression nodes. */
    unsigned clamp_count = 0;
    for (;;) {
        cskip(s, e);
        struct cexpr *x = calc_sum(s, e, cx);
        if (!x) return NULL;
        if (clamp) {
            if (clamp_count == 3) return NULL;
            if (clamp_count == 0) first = x;
            else if (clamp_count == 1) second = x;
            else result = cnode(cx, 'M', first, cnode(cx, 'm', second, x));
            clamp_count++;
        } else result = result ? cnode(cx, op, result, x) : x;
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
    if (clamp && clamp_count != 3) return NULL;
    return result;
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
    if (!css_unit_px(u, (size_t)(*s - u), v, cx, &x->px, &x->pct, &x->num)) return NULL;
    return x;
}

static struct cexpr *calc_product(const char **s, const char *e, struct cx *cx) {
    struct cexpr *x = calc_value(s, e, cx);
    for (;;) {
        if (!x) return NULL;
        const char *before_space = *s;
        cskip(s, e);
        if (*s < e && (**s == '*' || **s == '/')) {
            char op = *(*s)++;
            x = cnode(cx, op, x, calc_value(s, e, cx));
            if (!x->b) return NULL;
        } else { *s = before_space; return x; }
    }
}

static struct cexpr *calc_sum(const char **s, const char *e, struct cx *cx) {
    struct cexpr *x = calc_product(s, e, cx);
    for (;;) {
        if (!x) return NULL;
        const char *before_space = *s;
        cskip(s, e);
        if (*s < e && (**s == '+' || **s == '-')) {
            /* CSS calc-sum requires whitespace on both sides. Product does
               not consume that whitespace when it returns its final value. */
            if (*s == before_space || *s + 1 == e || !is_space((unsigned char)(*s)[1])) return NULL;
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

/* CSSOM must preserve unresolved percentages and comparison expressions,
   not turn opaque calculation nodes into a fabricated zero. Keep this next
   to the native calculation representation; callers never mirror cexpr.
   Unlike the general sb_put API, this author-readable path reports OOM. */
static bool length_append(sbuf *out, const char *text, size_t length) {
    if (out->n == SIZE_MAX || length > SIZE_MAX - out->n - 1) return false;
    size_t need = out->n + length + 1;
    if (need > out->cap) {
        size_t capacity = out->cap ? out->cap : 64;
        while (capacity < need) {
            if (capacity > SIZE_MAX / 2) { capacity = need; break; }
            capacity *= 2;
        }
        char *p = realloc(out->p, capacity);
        if (!p) return false;
        out->p = p; out->cap = capacity;
    }
    memcpy(out->p + out->n, text, length);
    out->n += length; out->p[out->n] = 0;
    return true;
}
static bool length_number(sbuf *out, float value, const char *unit) {
    if (!isfinite(value)) return false;
    if (value == 0) value = 0; /* CSS serialization has no negative zero. */
    char number[64];
    int n = snprintf(number, sizeof number, "%.9g%s", (double)value, unit);
    return n > 0 && (size_t)n < sizeof number && length_append(out, number, (size_t)n);
}
bool css_shadow_serialize(const struct css_box_shadow *shadow, sbuf *out, web_doc *d) {
    size_t original = out->n; bool success = true;
    if (!shadow) success = length_append(out, "none", 4);
    size_t count = 0;
    for (; shadow && success; shadow = shadow->next, count++) {
        if (!(count & 63) && d && !web_native_checkpoint(d)) { success = false; break; }
        success = (!count || length_append(out, ", ", 2)) &&
            css_color_value_text(shadow->color_value, shadow->color, out) && length_append(out, " ", 1) &&
            length_number(out, shadow->x, "px") && length_append(out, " ", 1) && length_number(out, shadow->y, "px") &&
            length_append(out, " ", 1) && length_number(out, shadow->blur, "px") && length_append(out, " ", 1) &&
            length_number(out, shadow->spread, "px") && (!shadow->inset || length_append(out, " inset", 6));
    }
    if (!success) { out->n = original; if (out->p && original < out->cap) out->p[original] = 0; }
    return success;
}
bool css_filter_serialize(const struct css_filter *filter, sbuf *out, web_doc *d) {
    size_t original = out->n;
    bool success = false;
    if (!filter) success = length_append(out, "none", 4);
    else if (filter->count) {
        success = true;
        for (size_t i = 0; i < filter->count; i++) {
            if (!(i & 63) && d && !web_native_checkpoint(d)) { success = false; break; }
            const struct css_filter_step *step = &filter->steps[i];
            if (step->value < 0 || (i && !length_append(out, " ", 1)) ||
                !length_append(out, "opacity(", 8) || !length_number(out, step->value, step->percentage ? "%" : "") ||
                !length_append(out, ")", 1)) { success = false; break; }
        }
    }
    if (!success) {
        out->n = original;
        if (out->p && original < out->cap) out->p[original] = 0;
    }
    return success;
}
static bool length_coefficients(sbuf *out, float px, float pct, bool number) {
    if (!isfinite(px) || !isfinite(pct)) return false;
    if (number) return pct == 0 && length_number(out, px, "");
    if (pct == 0) return length_number(out, px, "px");
    if (px == 0) return length_number(out, pct, "%");
    /* css-values-4 §10.13 sorts a sum's terms: the percentage before the dimensions. */
    return length_append(out, "calc(", 5) && length_number(out, pct, "%") &&
        length_append(out, px < 0 ? " - " : " + ", 3) && length_number(out, fabsf(px), "px") &&
        length_append(out, ")", 1);
}
struct length_frame { const struct cexpr *node; unsigned phase; };
static bool length_expression(sbuf *out, const struct cexpr *expression, web_doc *d) {
    struct length_frame local[32], *stack = local;
    size_t capacity = sizeof local / sizeof local[0], count = 1;
    local[0] = (struct length_frame){expression, 0};
    bool success = false;
    unsigned work = 0;
    while (count) {
        struct length_frame *frame = &stack[count - 1];
        const struct cexpr *x = frame->node, *child = NULL;
        if (!x) goto done;
        if (frame->phase == 0 && d && !(++work & 63) && !web_native_checkpoint(d)) goto done;
        if (x->op == 'v') {
            if (!length_coefficients(out, x->px, x->pct, x->num)) goto done;
            count--; continue;
        }
        bool comparison = x->op == 'm' || x->op == 'M';
        if (!comparison && x->op != '+' && x->op != '-' && x->op != '*' && x->op != '/') goto done;
        if (!x->a || !x->b) goto done;
        if (frame->phase == 0) {
            const char *open = comparison ? (x->op == 'm' ? "min(" : "max(") : count == 1 ? "calc(" : "(";
            if (!length_append(out, open, strlen(open))) goto done;
            frame->phase = 1; child = x->a;
        } else if (frame->phase == 1) {
            char op[3] = {' ', x->op, ' '};
            if (!length_append(out, comparison ? ", " : op, comparison ? 2 : sizeof op)) goto done;
            frame->phase = 2; child = x->b;
        } else {
            if (!length_append(out, ")", 1)) goto done;
            count--; continue;
        }
        if (count == capacity) {
            if (capacity > SIZE_MAX / 2 / sizeof *stack) goto done;
            size_t next = capacity * 2;
            struct length_frame *grown = stack == local ? malloc(next * sizeof *stack) : realloc(stack, next * sizeof *stack);
            if (!grown) goto done;
            if (stack == local) memcpy(grown, local, count * sizeof *stack);
            stack = grown; capacity = next;
        }
        stack[count++] = (struct length_frame){child, 0};
    }
    success = true;
done:
    if (stack != local) free(stack);
    return success;
}
bool css_length_serialize(const len_t *l, sbuf *out, web_doc *d) {
    if (!l || !out || out->n > out->cap || (!out->p && (out->n || out->cap))) return false;
    size_t begin = out->n;
    bool success = false;
    switch (l->kind) {
    case LK_AUTO: success = length_append(out, "auto", 4); break;
    case LK_NONE: success = length_append(out, "none", 4); break;
    case LK_NORMAL: success = length_append(out, "normal", 6); break;
    case LK_NUMBER: success = length_coefficients(out, l->px, l->pct, true); break;
    case LK_LEN: success = length_coefficients(out, l->px, l->pct, false); break;
    case LK_EXPR: success = length_expression(out, l->expr, d); break;
    }
    if (!success) {
        out->n = begin;
        if (out->p && begin < out->cap) out->p[begin] = 0;
    }
    return success;
}

static bool calc_dimension(const struct cexpr *x,bool *number){
    if(!x)return false;if(x->op=='v'){*number=x->num;return true;}
    bool a,b;if(!calc_dimension(x->a,&a)||!calc_dimension(x->b,&b))return false;
    if(x->op=='*'){if(!a&&!b)return false;*number=a&&b;return true;}
    if(x->op=='/'){if(!b)return false;*number=a;return true;}
    if(a!=b)return false;*number=a;return true;
}
/* Media feature lengths have no percentage basis or property range clamp.
   Share the actual CSS arithmetic grammar/type checker instead of using a
   permissive strtod prefix or rewriting calc() in the media-query parser. */
bool css_query_length(const char *text, size_t length, float em, float rem, float vw, float vh, double *value) {
    if (!text || !value || memchr(text, '%', length)) return false;
    arena_t *arena = calloc(1, sizeof *arena); if (!arena) return false;
    jmp_buf trap; arena->trap = &trap;
    if (setjmp(trap)) { ar_free(arena); free(arena); return false; }
    struct cx context = {.a = arena, .em = em, .rem = rem, .vw = vw, .vh = vh};
    const char *cursor = text, *end = text + length;
    struct cexpr *expression = calc_value(&cursor, end, &context); cskip(&cursor, end);
    bool number; float px, percentage; bool success = expression && cursor == end &&
        calc_dimension(expression, &number) && !number && cfold(expression, &number, &px, &percentage) &&
        !number && percentage == 0 && isfinite(px);
    if (success) *value = px;
    ar_free(arena); free(arena); return success;
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
                if (cx->supports_probe) return false; /* intrinsic sizing is currently an auto/none fallback */
                if (flags & LF_AUTO) out->kind = LK_AUTO;
                else if (flags & LF_NONE) out->kind = LK_NONE;
                else return false;
                return true;
            }
            return false;
        }
        if (ident_is(s, k, "fit-content")) {
            if (cx->supports_probe) return false;
            if (!(flags & LF_AUTO)) return false;
            out->kind = LK_AUTO;
            return true;
        }
        const char *p = s;
        struct cexpr *x = calc_value(&p, s + n, cx);
        cskip(&p, s + n);
        if (!x || p != s + n || (cx->supports_probe && cx->invalid)) return false;
        if(flags&LF_LENGTH){bool number;if(!calc_dimension(x,&number)||number)return false;}
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
    if (!css_unit_px(p, (size_t)(s + n - p), v, cx, &out->px, &out->pct, &num)) return false;
    if((flags&LF_LENGTH)&&num&&v!=0)return false;
    if ((flags & LF_NOPCT) && out->pct) return false;
    if (!(flags & LF_NEG) && v < 0) return false;
    out->kind = LK_LEN;
    return true;
}
/* A <number>: a number token, or a math function whose type is <number>.
   calc(20 / 16) is the multiplier 1.25 wherever a number is accepted, never
   1.25px. */
static bool parse_number_value(const char *v, size_t n, struct cx *cx, float *out) {
    trim(&v, &n);
    const char *q = v;
    if (parse_number(&q, v + n, out)) return q == v + n;
    if (!n || !isalpha((unsigned char)v[0])) return false;
    struct cexpr *x = calc_value(&q, v + n, cx);
    cskip(&q, v + n);
    bool number;
    float pct;
    return x && q == v + n && !(cx->supports_probe && cx->invalid) && calc_dimension(x, &number) && number &&
           cfold(x, &number, out, &pct) && number && isfinite(*out);
}

/* ---------------------------------------------------------------- colours (color.c owns them) */
/* a <color> for a property that keeps only the 8-bit used value (COLOR_CURRENT for currentcolor) */
static bool css_color_checked(const char *s, size_t n, uint32_t *out, struct cx *cx) {
    return css_color_value_compute(s, n, cx, NULL, out, NULL);
}

/* ---------------------------------------------------------------- the property table */
struct kw {
    const char *name;
    uint8_t v;
};

static bool shadow_specified_length(sbuf *out, const char *token, size_t size) {
    /* Preserve authored calculation/relative-unit expressions: this is a
       specified value, not a computed pixel value. Unitless zero is a length. */
    const char *p = token; float number;
    if (parse_number(&p, token + size, &number) && p == token + size && number == 0)
        return length_append(out, "0px", 3);
    return length_append(out, token, size);
}
static bool parse_box_shadows(const char *value, size_t length, struct cx *cx, const struct css_box_shadow **out,
                              sbuf *specified) {
    if (ident_is(value, length, "none")) { *out = NULL; return !specified || length_append(specified,"none",4); }
    const char *cursor = value, *end = value + length;
    struct css_box_shadow *first = NULL, **tail = &first;
    while (cursor < end) {
        const char *stop = cursor; size_t depth = 0;
        for (; stop < end; stop++) {
            if (*stop == '(') depth++;
            else if (*stop == ')') { if (!depth) return false; depth--; }
            else if (*stop == ',' && !depth) break;
        }
        if (depth) return false;
        const char *tokens[6]; size_t sizes[6];
        int count = split_checked(cursor, (size_t)(stop - cursor), tokens, sizes, 6, ' ', cx);
        if (count < 2) return false;
        const char *consumed = tokens[count-1] + sizes[count-1];
        while (consumed < stop && is_space((unsigned char)*consumed)) consumed++;
        if (consumed != stop) return false; /* strict in cascade and CSSOM too */
        struct css_box_shadow shadow = {.color = COLOR_CURRENT};
        bool color = false, inset = false, lengths_closed = false; unsigned lengths = 0;
        float dimensions[4] = {0};
        int color_token = -1, length_tokens[4];
        for (int i = 0; i < count; i++) {
            uint32_t candidate;
            const struct css_color_value *exact;
            if (ident_is(tokens[i], sizes[i], "inset")) {
                if (inset) return false; inset = shadow.inset = true; lengths_closed |= lengths != 0;
            } else if (css_color_value_compute(tokens[i], sizes[i], cx, NULL, &candidate, &exact)) {
                if (color) return false; color = true; shadow.color = candidate; shadow.color_value = exact;
                color_token=i; lengths_closed |= lengths != 0;
            } else {
                if (lengths_closed || lengths == 4 || memchr(tokens[i], '%', sizes[i])) return false;
                len_t dimension;
                unsigned flags = LF_NOPCT | LF_LENGTH | (lengths == 2 ? 0 : LF_NEG);
                if (!parse_len(tokens[i], sizes[i], cx, &dimension, flags) || dimension.kind != LK_LEN || dimension.pct ||
                    !isfinite(dimension.px)) return false;
                length_tokens[lengths] = i; dimensions[lengths++] = dimension.px;
            }
        }
        if (lengths < 2) return false;
        if (specified) {
            if (first && !length_append(specified,", ",2)) return false;
            if (color_token >= 0 && (!css_color_specified(tokens[color_token],sizes[color_token],specified) ||
                !length_append(specified," ",1))) return false;
            for (unsigned i=0;i<lengths;i++) {
                int token=length_tokens[i];
                if ((i && !length_append(specified," ",1)) ||
                    !shadow_specified_length(specified,tokens[token],sizes[token])) return false;
            }
            if (inset && !length_append(specified," inset",6)) return false;
        }
        shadow.x = dimensions[0]; shadow.y = dimensions[1]; shadow.blur = dimensions[2]; shadow.spread = dimensions[3];
        struct css_box_shadow *node = ar_alloc(cx->a, sizeof *node); *node = shadow; *tail = node; tail = &node->next;
        if (stop == end) break;
        cursor = stop + 1;
        const char *remaining = cursor; size_t n = (size_t)(end - cursor); trim(&remaining, &n); if (!n) return false;
    }
    if (!first) return false; *out = first; return true;
}
bool css_shadow_specified(const char *value, size_t size, sbuf *out) {
    size_t original=out->n;
    arena_t *arena=calloc(1,sizeof *arena);if(!arena)return false;
    jmp_buf trap;arena->trap=&trap;
    bool success=false;
    if (!setjmp(trap)) {
        struct cx cx={.em=16,.rem=16,.vw=800,.vh=600,.a=arena};
        const struct css_box_shadow *ignored;
        trim(&value,&size);
        success=parse_box_shadows(value,size,&cx,&ignored,out);
    }
    ar_free(arena);free(arena);
    if(!success){out->n=original;if(out->p&&original<out->cap)out->p[original]=0;}
    return success;
}
/* A <border-radius> value of four corners (TL TR BR BL) per axis, in its shortest form. */
static bool radius_lengths_serialize(const len_t lengths[2][4],sbuf *out,web_doc *doc) {
    bool ok=true,equal=true;for(int i=0;i<4;i++)equal&=css_length_equal(&lengths[0][i],&lengths[1][i]);
    for(int axis=0;axis<(equal?1:2)&&ok;axis++) {
        int count=4;if(css_length_equal(&lengths[axis][1],&lengths[axis][3])){count=3;if(css_length_equal(&lengths[axis][0],&lengths[axis][2])){count=2;if(css_length_equal(&lengths[axis][0],&lengths[axis][1]))count=1;}}
        if(axis)ok=length_append(out," / ",3);
        for(int i=0;i<count&&ok;i++)ok=(!i||length_append(out," ",1))&&css_length_serialize(&lengths[axis][i],out,doc);
    }
    return ok;
}
bool css_radius_serialize(const style_t *style,int corner,sbuf *out,web_doc *doc) {
    size_t original=out->n;bool ok=true;len_t lengths[2][4];
    for(int i=0;i<4;i++)for(int axis=0;axis<2;axis++) {
        if(style->corner_radius[i].axis[axis].kind!=LK_AUTO)lengths[axis][i]=style->corner_radius[i].axis[axis];
        else lengths[axis][i]=(len_t){.kind=LK_LEN,.px=style->border_radius>=0?style->border_radius:0,.pct=style->border_radius<0?-style->border_radius:0};
    }
    if(corner>=0&&corner<4) {
        ok=css_length_serialize(&lengths[0][corner],out,doc);
        if(ok&&!css_length_equal(&lengths[0][corner],&lengths[1][corner]))ok=length_append(out," ",1)&&css_length_serialize(&lengths[1][corner],out,doc);
    } else ok=radius_lengths_serialize(lengths,out,doc);
    if(!ok){out->n=original;if(out->p&&original<out->cap)out->p[original]=0;}return ok;
}
/* A CSS string's contents (CSSOM §6.6.2 "serialize a string"), without the quotes */
static bool string_escaped(sbuf *out,const char *s) {
    for(;*s;s++) {
        unsigned char c=(unsigned char)*s;char escape[16];
        if(c<0x20||c==0x7f){int n=snprintf(escape,sizeof escape,"\\%x ",c);if(!length_append(out,escape,(size_t)n))return false;}
        else if(c=='"'||c=='\\'){escape[0]='\\';escape[1]=(char)c;if(!length_append(out,escape,2))return false;}
        else if(!length_append(out,(const char *)s,1))return false;
    }
    return true;
}
static bool clip_side_zero_percent(const struct css_clip_path *c,int side) {
    const len_t *l=&c->inset[side];
    return l->kind==LK_LEN&&l->px==0&&l->pct==0&&(c->inset_percent>>side&1);
}
static bool clip_sides_equal(const struct css_clip_path *c,int a,int b) {
    return css_length_equal(&c->inset[a],&c->inset[b])&&clip_side_zero_percent(c,a)==clip_side_zero_percent(c,b);
}
static const char *const clip_boxes[]={"border-box","padding-box","content-box","margin-box","fill-box","stroke-box","view-box"};
bool css_clip_path_serialize(const struct css_clip_path *c,sbuf *out,web_doc *d) {
    size_t original=out->n;bool ok=true;
    if(!c)ok=length_append(out,"none",4);
    else switch(c->shape) {
    case CLIP_SHAPE_URL: ok=length_append(out,"url(\"",5)&&string_escaped(out,c->text)&&length_append(out,"\")",2);break;
    case CLIP_SHAPE_BOX: ok=length_append(out,clip_boxes[c->box],strlen(clip_boxes[c->box]));break;
    case CLIP_SHAPE_INSET: {
        int count=4;
        if(clip_sides_equal(c,1,3)){count=3;if(clip_sides_equal(c,0,2)){count=2;if(clip_sides_equal(c,0,1))count=1;}}
        ok=length_append(out,"inset(",6);
        for(int i=0;i<count&&ok;i++)ok=(!i||length_append(out," ",1))&&(clip_side_zero_percent(c,i)?length_append(out,"0%",2):css_length_serialize(&c->inset[i],out,d));
        len_t lengths[2][4];bool round=false;
        for(int i=0;i<4;i++)for(int axis=0;axis<2;axis++){const len_t *r=&c->corner[i].axis[axis];lengths[axis][i]=*r;round|=r->kind!=LK_LEN||r->px!=0||r->pct!=0;}
        if(ok&&round)ok=length_append(out," round ",7)&&radius_lengths_serialize(lengths,out,d);
        ok=ok&&length_append(out,")",1);
        break;
    }
    case CLIP_SHAPE_CIRCLE: case CLIP_SHAPE_ELLIPSE: {
        int radii=c->shape==CLIP_SHAPE_CIRCLE?1:2;bool closest=true;
        ok=radii==1?length_append(out,"circle(",7):length_append(out,"ellipse(",8);
        for(int i=0;i<radii;i++)closest&=c->extent[i]==CLIP_EXTENT_CLOSEST;
        for(int i=0;i<radii&&ok&&!closest;i++)
            ok=(!i||length_append(out," ",1))&&(c->extent[i]==CLIP_EXTENT_CLOSEST?length_append(out,"closest-side",12):
               c->extent[i]==CLIP_EXTENT_FARTHEST?length_append(out,"farthest-side",13):css_length_serialize(&c->radius[i],out,d));
        ok=ok&&(closest||length_append(out," ",1))&&length_append(out,"at ",3)&&css_length_serialize(&c->at[0],out,d)&&
           length_append(out," ",1)&&css_length_serialize(&c->at[1],out,d)&&length_append(out,")",1);
        break;
    }
    case CLIP_SHAPE_POLYGON:
        ok=length_append(out,"polygon(",8)&&(!c->evenodd||length_append(out,"evenodd",7));
        if(ok&&c->round>0)ok=(!c->evenodd||length_append(out," ",1))&&length_append(out,"round ",6)&&length_number(out,c->round,"px");
        if(ok&&(c->evenodd||c->round>0))ok=length_append(out,", ",2);
        for(size_t i=0;i<c->points&&ok;i++) {
            if(!(i&63)&&d&&!web_native_checkpoint(d)){ok=false;break;}
            ok=(!i||length_append(out,", ",2))&&css_length_serialize(&c->point[i][0],out,d)&&length_append(out," ",1)&&
               css_length_serialize(&c->point[i][1],out,d);
        }
        ok=ok&&length_append(out,")",1);
        break;
    case CLIP_SHAPE_PATH:
        ok=length_append(out,"path(",5)&&(!c->evenodd||length_append(out,"evenodd, ",9))&&length_append(out,"\"",1)&&
           string_escaped(out,c->text)&&length_append(out,"\")",2);
        break;
    default: ok=false;
    }
    /* A shape's default box is border-box (css-masking-1 §5.1): the shortest form leaves it out. */
    if(ok&&c&&c->shape!=CLIP_SHAPE_BOX&&c->shape!=CLIP_SHAPE_URL&&c->box!=CLIP_BOX_BORDER)
        ok=length_append(out," ",1)&&length_append(out,clip_boxes[c->box],strlen(clip_boxes[c->box]));
    if(!ok){out->n=original;if(out->p&&original<out->cap)out->p[original]=0;}
    return ok;
}
bool css_clip_rect_serialize(const struct css_clip_rect *clip,sbuf *out) {
    size_t original=out->n;bool ok=true;
    if(!clip->rect)ok=length_append(out,"auto",4);
    else {
        ok=length_append(out,"rect(",5);
        for(int i=0;i<4&&ok;i++)ok=(!i||length_append(out,", ",2))&&(clip->auto_edges>>i&1?length_append(out,"auto",4):length_number(out,clip->edge[i],"px"));
        ok=ok&&length_append(out,")",1);
    }
    if(!ok){out->n=original;if(out->p&&original<out->cap)out->p[original]=0;}
    return ok;
}
bool css_radius_specified(const char *property,const char *value,size_t size,sbuf *out) {
    if(!css_valid_declaration(property,strlen(property),value,size))return false;
    if(memchr(value,'\\',size))return false;
    for(size_t i=0;i+4<=size;i++)if(strn_ieq(value+i,"var(",4))return false;
    bool shorthand=!strcasecmp(property,"border-radius");size_t original=out->n;
    struct cx cx={0};const char *axes[3];size_t axis_n[3];
    int count=shorthand?split_checked(value,size,axes,axis_n,3,'/',&cx):1;
    if(!shorthand){axes[0]=value;axis_n[0]=size;}
    if(count<1||count>2)return false;
    const char *tokens[2][5];size_t token_n[2][5];int lengths[2];
    for(int axis=0;axis<count;axis++) {
        lengths[axis]=split_checked(axes[axis],axis_n[axis],tokens[axis],token_n[axis],5,' ',&cx);
        if(lengths[axis]<1||lengths[axis]>(shorthand?4:2))return false;
    }
    if(shorthand&&count==2) {
        static const int pick[5][4]={{0},{0,0,0,0},{0,1,0,1},{0,1,2,1},{0,1,2,3}};bool equal=true;
        for(int i=0;i<4;i++){int x=pick[lengths[0]][i],y=pick[lengths[1]][i];equal&=token_n[0][x]==token_n[1][y]&&!memcmp(tokens[0][x],tokens[1][y],token_n[0][x]);}
        if(equal)count=1;
    }
    bool ok=true;
    for(int axis=0;axis<count&&ok;axis++) {
        int n=lengths[axis];
        if(shorthand) {
            while(n>1) {
                int other=n==4?1:n==3?0:0;
                if(token_n[axis][n-1]!=token_n[axis][other]||memcmp(tokens[axis][n-1],tokens[axis][other],token_n[axis][other]))break;
                n--;
            }
        } else if(n==2&&token_n[axis][0]==token_n[axis][1]&&!memcmp(tokens[axis][0],tokens[axis][1],token_n[axis][0]))n=1;
        if(axis)ok=length_append(out," / ",3);
        for(int i=0;i<n&&ok;i++)ok=(!i||length_append(out," ",1))&&shadow_specified_length(out,tokens[axis][i],token_n[axis][i]);
    }
    if(!ok){out->n=original;if(out->p&&original<out->cap)out->p[original]=0;}return ok;
}

enum { PT_SHORT, PT_KW, PT_DISPLAY, PT_LEN, PT_PX, PT_COLOR, PT_NUM, PT_INT, PT_FONT_SIZE, PT_FONT_WEIGHT,
       PT_FONT_FAMILY, PT_LINE_HEIGHT, PT_VALIGN, PT_CONTENT, PT_LIST_TYPE, PT_BG_IMAGE, PT_TEXT_DECO,
       PT_OVERFLOW, PT_OPACITY, PT_FILTER, PT_BOX_SHADOW, PT_RADIUS, PT_ZINDEX, PT_BG_POS, PT_BG_SIZE, PT_GTEMPLATE, PT_GAREAS, PT_GLINE, PT_MASK_IMAGE, PT_MASK_SIZE, PT_SVG_PAINT, PT_SVG_WIDTH, PT_CONTAINER_NAMES, PT_LINE_CLAMP,
       PT_MOTION_LIST, PT_TRANSFORM, PT_TRANSLATE,
       PT_ROTATE, PT_SCALE, PT_TRANSFORM_ORIGIN, PT_CLIP_PATH, PT_CLIP, PT_COLOR_SCHEME };

enum { SH_MARGIN = 1, SH_PADDING, SH_INSET, SH_BORDER_RADIUS, SH_BORDER, SH_BORDER_TOP, SH_BORDER_RIGHT, SH_BORDER_BOTTOM,
       SH_BORDER_LEFT, SH_BORDER_WIDTH, SH_BORDER_STYLE, SH_BORDER_COLOR, SH_BORDER_INLINE, SH_BORDER_BLOCK,
       SH_MARGIN_INLINE, SH_MARGIN_BLOCK, SH_PADDING_INLINE, SH_PADDING_BLOCK, SH_INSET_INLINE, SH_INSET_BLOCK,
       SH_LIST_STYLE, SH_FONT, SH_BACKGROUND, SH_FLEX, SH_FLEX_FLOW, SH_GAP, SH_OVERFLOW, SH_TEXT_DECORATION, SH_ALL,
       SH_PLACE_ITEMS, SH_PLACE_SELF, SH_PLACE_CONTENT, SH_GRID_AREA, SH_GRID_ROW, SH_GRID_COLUMN,
       SH_GRID_TEMPLATE, SH_GRID, SH_CONTAINER, SH_ANIMATION, SH_TRANSITION, SH_OUTLINE };

struct propdef {
    const char *name;
    uint8_t type, flags; /* flags: LF_* for lengths */
    bool inherited;
    uint16_t off, size;
    const struct kw *kws;
    uint8_t sh;
    uint8_t anim; /* AN_*: the animation type when it is not the value type's (css_prop_animation) */
};
/* Web Animations 1 §5.3 animation types that differ from a value type's default */
enum { AN_TYPE, AN_NONE, AN_DISCRETE, AN_VISIBILITY, AN_DISPLAY };

static const struct kw kw_position[] = {{"static", POS_STATIC}, {"relative", POS_RELATIVE}, {"absolute", POS_ABSOLUTE},
                                        {"fixed", POS_FIXED}, {"sticky", POS_STICKY}, {"-webkit-sticky", POS_STICKY}, {0}};
static const struct kw kw_box_orient[] = {{"horizontal", BO_HORIZONTAL}, {"inline-axis", BO_HORIZONTAL},
                                        {"vertical", BO_VERTICAL}, {"block-axis", BO_VERTICAL}, {0}};
static const struct kw kw_motion_direction[] = {{"normal",CM_NORMAL},{"reverse",CM_REVERSE},
    {"alternate",CM_ALTERNATE},{"alternate-reverse",CM_ALTERNATE_REVERSE},{0}};
static const struct kw kw_motion_fill[] = {{"none",CM_FILL_NONE},{"forwards",CM_FILL_FORWARDS},
    {"backwards",CM_FILL_BACKWARDS},{"both",CM_FILL_BOTH},{0}};
static const struct kw kw_motion_play[] = {{"running",0},{"paused",1},{0}};
static const struct kw kw_motion_composition[] = {{"replace",CM_COMPOSITE_REPLACE},{"add",CM_COMPOSITE_ADD},
    {"accumulate",CM_COMPOSITE_ACCUMULATE},{0}};
static const struct kw kw_transition_behavior[] = {{"normal",0},{"allow-discrete",1},{0}};
static const struct kw kw_float[] = {{"none", FL_NONE}, {"left", FL_LEFT}, {"right", FL_RIGHT},
                                     {"inline-start", FL_LEFT}, {"inline-end", FL_RIGHT}, {0}};
static const struct kw kw_clear[] = {{"none", CL_NONE}, {"left", CL_LEFT}, {"right", CL_RIGHT}, {"both", CL_BOTH},
                                     {"inline-start", CL_LEFT}, {"inline-end", CL_RIGHT}, {0}};
static const struct kw kw_ws[] = {{"normal", WS_NORMAL}, {"pre", WS_PRE}, {"nowrap", WS_NOWRAP},
                                  {"pre-wrap", WS_PRE_WRAP}, {"pre-line", WS_PRE_LINE},
                                  {"break-spaces", WS_PRE_WRAP}, {"-moz-pre-wrap", WS_PRE_WRAP}, {0}};
static const struct kw kw_textwrap[] = {{"wrap", WS_NORMAL}, {"nowrap", WS_NOWRAP}, {"balance", WS_NORMAL},
                                        {"pretty", WS_NORMAL}, {"stable", WS_NORMAL}, {0}};
static const struct kw kw_text_overflow[] = {{"clip",TO_CLIP},{"ellipsis",TO_ELLIPSIS},{0}};
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
static const struct kw kw_object_fit[] = {{"fill", OF_FILL}, {"contain", OF_CONTAIN}, {"cover", OF_COVER},
                                         {"none", OF_NONE}, {"scale-down", OF_SCALE_DOWN}, {0}};
static const struct kw kw_bg_repeat[] = {{"repeat", BR_REPEAT}, {"repeat-x", BR_REPEAT_X}, {"repeat-y", BR_REPEAT_Y},
                                         {"no-repeat", BR_NO_REPEAT}, {"space", BR_REPEAT}, {"round", BR_REPEAT},
                                         {0}};
/* HTML hit testing supports auto/none; SVG painted/geometry keywords remain unsupported. */
static const struct kw kw_pointer_events[] = {{"auto", 0}, {"none", 1}, {0}};
static const struct kw kw_transform_box[] = {{"view-box", TB_VIEW_BOX}, {"fill-box", TB_FILL_BOX}, {"stroke-box", TB_STROKE_BOX},
                                           {"content-box", TB_CONTENT_BOX}, {"border-box", TB_BORDER_BOX}, {0}};
static const struct kw kw_visibility[] = {{"visible", 0}, {"hidden", 1}, {"collapse", 1}, {0}};
static const struct kw kw_content_visibility[] = {{"visible", CV_VISIBLE}, {"auto", CV_AUTO}, {"hidden", CV_HIDDEN}, {0}};
static const struct kw kw_container_type[] = {{"normal", CT_NORMAL}, {"inline-size", CT_INLINE_SIZE}, {"size", CT_SIZE}, {0}};
static const struct kw kw_bcollapse[] = {{"separate", 0}, {"collapse", 1}, {0}};
static const struct kw kw_fdir[] = {{"row", FD_ROW}, {"row-reverse", FD_ROW_REVERSE}, {"column", FD_COLUMN},
                                    {"column-reverse", FD_COLUMN_REVERSE}, {0}};
static const struct kw kw_fwrap[] = {{"nowrap", FW_NOWRAP}, {"wrap", FW_WRAP}, {"wrap-reverse", FW_WRAP_REVERSE}, {0}};
static const struct kw kw_acontent[] = {{"normal", AC_NORMAL}, {"stretch", AC_STRETCH},
    {"flex-start", AC_FLEX_START}, {"flex-end", AC_FLEX_END}, {"center", AC_CENTER},
    {"space-between", AC_BETWEEN}, {"space-around", AC_AROUND}, {"space-evenly", AC_EVENLY},
    {"start", AC_START}, {"end", AC_END}, {0}};
static const struct kw kw_justify[] = {{"normal", JC_START}, {"flex-start", JC_START}, {"start", JC_START},
                                       {"left", JC_START}, {"stretch", JC_START}, {"flex-end", JC_END},
                                       {"end", JC_END}, {"right", JC_END}, {"center", JC_CENTER},
                                       {"space-between", JC_BETWEEN}, {"space-around", JC_AROUND},
                                       {"space-evenly", JC_EVENLY}, {"safe center", JC_CENTER}, {0}};
static const struct kw kw_align[] = {{"normal", AI_STRETCH}, {"stretch", AI_STRETCH}, {"flex-start", AI_START},
                                     {"start", AI_START}, {"self-start", AI_START}, {"flex-end", AI_END},
                                     {"end", AI_END}, {"self-end", AI_END}, {"center", AI_CENTER},
                                     {"safe center", AI_CENTER}, {"baseline", AI_BASELINE},
                                     {"first baseline", AI_BASELINE}, {"last baseline", AI_BASELINE},
                                     {"auto", 255}, {0}};
static const struct kw kw_jalign[] = {{"normal", AI_STRETCH}, {"stretch", AI_STRETCH}, {"legacy", AI_STRETCH},
                                      {"start", AI_START}, {"self-start", AI_START}, {"flex-start", AI_START},
                                      {"left", AI_START}, {"end", AI_END}, {"self-end", AI_END},
                                      {"flex-end", AI_END}, {"right", AI_END}, {"center", AI_CENTER},
                                      {"safe center", AI_CENTER}, {"legacy center", AI_CENTER},
                                      {"baseline", AI_START}, {"auto", 255}, {0}};
static const struct kw kw_gflow[] = {{"row", 0}, {"column", 1}, {"dense", 0}, {"row dense", 0},
                                     {"column dense", 1}, {"dense row", 0}, {"dense column", 1}, {0}};
static const struct kw kw_capside[] = {{"top", 0}, {"bottom", 1}, {"block-start", 0}, {"block-end", 1}, {0}};
static const struct kw kw_tlayout[] = {{"auto", 0}, {"fixed", 1}, {0}};
static const struct kw kw_svg_fill_rule[] = {{"nonzero", 0}, {"evenodd", 1}, {0}};
static const struct kw kw_svg_cap[] = {{"butt", 0}, {"round", 1}, {"square", 2}, {0}};
static const struct kw kw_svg_join[] = {{"miter", 0}, {"round", 1}, {"bevel", 2}, {0}};
static const struct kw kw_bstyle[] = {{"none", BS_NONE}, {"hidden", BS_HIDDEN}, {"solid", BS_SOLID},
                                      {"dashed", BS_DASHED}, {"dotted", BS_DOTTED}, {"double", BS_DOUBLE},
                                      {"groove", BS_GROOVE}, {"ridge", BS_RIDGE}, {"inset", BS_INSET},
                                      {"outset", BS_OUTSET}, {0}};
static const struct kw kw_bwidth[] = {{"thin", 1}, {"medium", 3}, {"thick", 5}, {0}};
static const struct kw kw_ostyle[] = {{"none", BS_NONE}, {"solid", BS_SOLID}, {"auto", BS_SOLID}, {"dashed", BS_DASHED},
                                      {"dotted", BS_DOTTED}, {"double", BS_DOUBLE}, {"groove", BS_GROOVE},
                                      {"ridge", BS_RIDGE}, {"inset", BS_INSET}, {"outset", BS_OUTSET}, {0}};
static const struct kw kw_normal0[] = {{"normal", 0}, {0}};
static const struct kw kw_valign[] = {{"baseline", VA_BASELINE}, {"sub", VA_SUB}, {"super", VA_SUPER},
                                      {"top", VA_TOP}, {"middle", VA_MIDDLE}, {"bottom", VA_BOTTOM},
                                      {"text-top", VA_TEXT_TOP}, {"text-bottom", VA_TEXT_BOTTOM}, {0}};

#define O(f) (uint16_t) offsetof(style_t, f), (uint16_t)sizeof(((style_t *)0)->f)
#define SH(name, id) {name, PT_SHORT, 0, false, 0, 0, NULL, id, AN_NONE}

/* sorted by name at first use */
static struct propdef props[] = {
    {"-webkit-box-orient", PT_KW, 0, false, O(box_orient), kw_box_orient, 0, AN_TYPE},
    {"-webkit-line-clamp", PT_LINE_CLAMP, 0, false, O(line_clamp), NULL, 0, AN_TYPE},
    SH("animation", SH_ANIMATION),
    /* flags: which ML_* list a motion longhand is */
    {"animation-name",PT_MOTION_LIST,ML_NAME,false,O(motion.list[ML_NAME]),NULL,0,AN_NONE},
    {"animation-duration",PT_MOTION_LIST,ML_DURATION,false,O(motion.list[ML_DURATION]),NULL,0,AN_NONE},
    {"animation-delay",PT_MOTION_LIST,ML_DELAY,false,O(motion.list[ML_DELAY]),NULL,0,AN_NONE},
    {"animation-timing-function",PT_MOTION_LIST,ML_EASING,false,O(motion.list[ML_EASING]),NULL,0,AN_NONE},
    {"animation-iteration-count",PT_MOTION_LIST,ML_ITERATIONS,false,O(motion.list[ML_ITERATIONS]),NULL,0,AN_NONE},
    {"animation-direction",PT_MOTION_LIST,ML_DIRECTION,false,O(motion.list[ML_DIRECTION]),kw_motion_direction,0,AN_NONE},
    {"animation-fill-mode",PT_MOTION_LIST,ML_FILL,false,O(motion.list[ML_FILL]),kw_motion_fill,0,AN_NONE},
    {"animation-play-state",PT_MOTION_LIST,ML_PLAY,false,O(motion.list[ML_PLAY]),kw_motion_play,0,AN_NONE},
    {"animation-composition",PT_MOTION_LIST,ML_COMPOSITION,false,O(motion.list[ML_COMPOSITION]),kw_motion_composition,0,AN_NONE},
    SH("transition", SH_TRANSITION),
    {"transition-property",PT_MOTION_LIST,ML_TPROPERTY,false,O(motion.list[ML_TPROPERTY]),NULL,0,AN_NONE},
    {"transition-duration",PT_MOTION_LIST,ML_TDURATION,false,O(motion.list[ML_TDURATION]),NULL,0,AN_NONE},
    {"transition-delay",PT_MOTION_LIST,ML_TDELAY,false,O(motion.list[ML_TDELAY]),NULL,0,AN_NONE},
    {"transition-timing-function",PT_MOTION_LIST,ML_TEASING,false,O(motion.list[ML_TEASING]),NULL,0,AN_NONE},
    {"transition-behavior",PT_MOTION_LIST,ML_TBEHAVIOR,false,O(motion.list[ML_TBEHAVIOR]),kw_transition_behavior,0,AN_NONE},
    {"align-content", PT_KW, 0, false, O(align_content), kw_acontent, 0, AN_TYPE},
    {"align-items", PT_KW, 0, false, O(align_items), kw_align, 0, AN_TYPE},
    {"align-self", PT_KW, 0, false, O(align_self), kw_align, 0, AN_TYPE},
    SH("background", SH_BACKGROUND),
    {"background-color", PT_COLOR, 0, false, O(bg_color), NULL, 0, AN_TYPE},
    {"background-image", PT_BG_IMAGE, 0, false, O(bg_image), NULL, 0, AN_TYPE},
    {"box-shadow", PT_BOX_SHADOW, 0, false, O(box_shadow), NULL, 0, AN_TYPE},
    {"background-repeat", PT_KW, 0, false, O(bg_repeat), kw_bg_repeat, 0, AN_TYPE},
    {"background-position", PT_BG_POS, 0, false, O(bg_pos), NULL, 0, AN_TYPE},
    {"background-size", PT_BG_SIZE, 0, false, O(bg_size), NULL, 0, AN_TYPE},
    SH("border", SH_BORDER),
    SH("border-block", SH_BORDER_BLOCK),
    SH("border-bottom", SH_BORDER_BOTTOM),
    {"border-bottom-left-radius", PT_RADIUS, 0, false, O(corner_radius[3]), NULL, 0, AN_TYPE},
    {"border-bottom-right-radius", PT_RADIUS, 0, false, O(corner_radius[2]), NULL, 0, AN_TYPE},
    {"border-bottom-color", PT_COLOR, 0, false, O(border_color[2]), NULL, 0, AN_TYPE},
    {"border-bottom-style", PT_KW, 0, false, O(border_style[2]), kw_bstyle, 0, AN_TYPE},
    {"border-bottom-width", PT_PX, LF_NOPCT, false, O(border_width[2]), kw_bwidth, 0, AN_TYPE},
    {"border-collapse", PT_KW, 0, true, O(border_collapse), kw_bcollapse, 0, AN_TYPE},
    SH("border-color", SH_BORDER_COLOR),
    SH("border-inline", SH_BORDER_INLINE),
    SH("border-left", SH_BORDER_LEFT),
    {"border-left-color", PT_COLOR, 0, false, O(border_color[3]), NULL, 0, AN_TYPE},
    {"border-left-style", PT_KW, 0, false, O(border_style[3]), kw_bstyle, 0, AN_TYPE},
    {"border-left-width", PT_PX, LF_NOPCT, false, O(border_width[3]), kw_bwidth, 0, AN_TYPE},
    SH("border-radius", SH_BORDER_RADIUS),
    SH("border-right", SH_BORDER_RIGHT),
    {"border-right-color", PT_COLOR, 0, false, O(border_color[1]), NULL, 0, AN_TYPE},
    {"border-right-style", PT_KW, 0, false, O(border_style[1]), kw_bstyle, 0, AN_TYPE},
    {"border-right-width", PT_PX, LF_NOPCT, false, O(border_width[1]), kw_bwidth, 0, AN_TYPE},
    {"border-spacing", PT_PX, LF_NOPCT, true, O(border_spacing), NULL, 0, AN_TYPE},
    SH("border-style", SH_BORDER_STYLE),
    SH("border-top", SH_BORDER_TOP),
    {"border-top-left-radius", PT_RADIUS, 0, false, O(corner_radius[0]), NULL, 0, AN_TYPE},
    {"border-top-right-radius", PT_RADIUS, 0, false, O(corner_radius[1]), NULL, 0, AN_TYPE},
    {"border-top-color", PT_COLOR, 0, false, O(border_color[0]), NULL, 0, AN_TYPE},
    {"border-top-style", PT_KW, 0, false, O(border_style[0]), kw_bstyle, 0, AN_TYPE},
    {"border-top-width", PT_PX, LF_NOPCT, false, O(border_width[0]), kw_bwidth, 0, AN_TYPE},
    SH("border-width", SH_BORDER_WIDTH),
    {"bottom", PT_LEN, LF_AUTO | LF_NEG, false, O(inset[2]), NULL, 0, AN_TYPE},
    {"box-sizing", PT_KW, 0, false, O(box_sizing), kw_boxsizing, 0, AN_TYPE},
    {"caption-side", PT_KW, 0, true, O(caption_bottom), kw_capside, 0, AN_TYPE},
    {"clear", PT_KW, 0, false, O(clear), kw_clear, 0, AN_TYPE},
    {"color", PT_COLOR, 0, true, O(color), NULL, 0, AN_TYPE},
    {"color-scheme", PT_COLOR_SCHEME, 0, true, O(color_scheme), NULL, 0, AN_DISCRETE},
    {"column-gap", PT_PX, LF_NOPCT, false, O(gap_col), kw_normal0, 0, AN_TYPE},
    {"content", PT_CONTENT, 0, false, O(content), NULL, 0, AN_TYPE},
    {"content-visibility", PT_KW, 0, false, O(content_visibility), kw_content_visibility, 0, AN_DISPLAY},
    SH("container", SH_CONTAINER),
    {"container-name", PT_CONTAINER_NAMES, 0, false, O(container_names), NULL, 0, AN_TYPE},
    {"container-type", PT_KW, 0, false, O(container_type), kw_container_type, 0, AN_NONE},
    {"display", PT_DISPLAY, 0, false, O(display), NULL, 0, AN_TYPE},
    SH("flex", SH_FLEX),
    {"flex-basis", PT_LEN, LF_AUTO | LF_LENGTH, false, O(flex_basis), NULL, 0, AN_TYPE},
    {"flex-direction", PT_KW, 0, false, O(flex_direction), kw_fdir, 0, AN_TYPE},
    SH("flex-flow", SH_FLEX_FLOW),
    {"flex-grow", PT_NUM, 0, false, O(flex_grow), NULL, 0, AN_TYPE},
    {"flex-shrink", PT_NUM, 0, false, O(flex_shrink), NULL, 0, AN_TYPE},
    {"flex-wrap", PT_KW, 0, false, O(flex_wrap), kw_fwrap, 0, AN_TYPE},
    {"float", PT_KW, 0, false, O(float_), kw_float, 0, AN_TYPE},
    {"fill", PT_SVG_PAINT, 0, true, O(svg_fill), NULL, 0, AN_TYPE},
    {"fill-opacity", PT_OPACITY, 0, true, O(svg_fill_opacity), NULL, 0, AN_TYPE},
    {"filter", PT_FILTER, 0, false, O(filter), NULL, 0, AN_TYPE},
    {"clip", PT_CLIP, 0, false, O(clip), NULL, 0, AN_TYPE},
    {"clip-path", PT_CLIP_PATH, 0, false, O(clip_path), NULL, 0, AN_TYPE},
    {"fill-rule", PT_KW, 0, true, O(svg_fill_rule), kw_svg_fill_rule, 0, AN_TYPE},
    SH("font", SH_FONT),
    SH("all", SH_ALL),
    {"font-family", PT_FONT_FAMILY, 0, true, O(font_family), NULL, 0, AN_TYPE},
    {"font-size", PT_FONT_SIZE, 0, true, O(font_size), NULL, 0, AN_TYPE},
    {"font-style", PT_KW, 0, true, O(font_style), kw_fstyle, 0, AN_TYPE},
    {"font-weight", PT_FONT_WEIGHT, 0, true, O(font_weight), NULL, 0, AN_TYPE},
    SH("gap", SH_GAP),
    SH("grid", SH_GRID),
    SH("grid-area", SH_GRID_AREA),
    {"grid-auto-columns", PT_GTEMPLATE, 0, false, O(grid_auto_cols), NULL, 0, AN_TYPE},
    {"grid-auto-flow", PT_KW, 0, false, O(grid_flow_col), kw_gflow, 0, AN_TYPE},
    {"grid-auto-rows", PT_GTEMPLATE, 0, false, O(grid_auto_rows), NULL, 0, AN_TYPE},
    SH("grid-column", SH_GRID_COLUMN),
    {"grid-column-end", PT_GLINE, 0, false, O(grid_place[3]), NULL, 0, AN_TYPE},
    {"grid-column-start", PT_GLINE, 0, false, O(grid_place[1]), NULL, 0, AN_TYPE},
    SH("grid-row", SH_GRID_ROW),
    {"grid-row-end", PT_GLINE, 0, false, O(grid_place[2]), NULL, 0, AN_TYPE},
    {"grid-row-start", PT_GLINE, 0, false, O(grid_place[0]), NULL, 0, AN_TYPE},
    SH("grid-template", SH_GRID_TEMPLATE),
    {"grid-template-areas", PT_GAREAS, 0, false, O(grid_areas), NULL, 0, AN_TYPE},
    {"grid-template-columns", PT_GTEMPLATE, 0, false, O(grid_cols), NULL, 0, AN_TYPE},
    {"grid-template-rows", PT_GTEMPLATE, 0, false, O(grid_rows), NULL, 0, AN_TYPE},
    {"height", PT_LEN, LF_AUTO, false, O(height), NULL, 0, AN_TYPE},
    SH("inset", SH_INSET),
    SH("inset-block", SH_INSET_BLOCK),
    SH("inset-inline", SH_INSET_INLINE),
    {"justify-content", PT_KW, 0, false, O(justify_content), kw_justify, 0, AN_TYPE},
    {"justify-items", PT_KW, 0, false, O(justify_items), kw_jalign, 0, AN_TYPE},
    {"justify-self", PT_KW, 0, false, O(justify_self), kw_jalign, 0, AN_TYPE},
    {"left", PT_LEN, LF_AUTO | LF_NEG, false, O(inset[3]), NULL, 0, AN_TYPE},
    {"letter-spacing", PT_PX, LF_NEG | LF_NOPCT, true, O(letter_spacing), kw_normal0, 0, AN_TYPE},
    {"line-height", PT_LINE_HEIGHT, 0, true, O(line_height), NULL, 0, AN_TYPE},
    SH("list-style", SH_LIST_STYLE),
    {"list-style-position", PT_KW, 0, true, O(list_style_inside), kw_lspos, 0, AN_TYPE},
    {"list-style-type", PT_LIST_TYPE, 0, true, O(list_style), NULL, 0, AN_TYPE},
    SH("margin", SH_MARGIN),
    SH("margin-block", SH_MARGIN_BLOCK),
    {"margin-bottom", PT_LEN, LF_AUTO | LF_NEG, false, O(margin[2]), NULL, 0, AN_TYPE},
    SH("margin-inline", SH_MARGIN_INLINE),
    {"margin-left", PT_LEN, LF_AUTO | LF_NEG, false, O(margin[3]), NULL, 0, AN_TYPE},
    {"margin-right", PT_LEN, LF_AUTO | LF_NEG, false, O(margin[1]), NULL, 0, AN_TYPE},
    {"margin-top", PT_LEN, LF_AUTO | LF_NEG, false, O(margin[0]), NULL, 0, AN_TYPE},
    {"mask-image", PT_MASK_IMAGE, 0, false, O(mask_image), NULL, 0, AN_TYPE},
    {"mask-position", PT_BG_POS, 0, false, O(mask_pos), NULL, 0, AN_TYPE},
    {"object-fit", PT_KW, 0, false, O(object_fit), kw_object_fit, 0, AN_TYPE},
    {"object-position", PT_BG_POS, 0, true, O(object_pos), NULL, 0, AN_TYPE},
    {"mask-repeat", PT_KW, 0, false, O(mask_repeat), kw_bg_repeat, 0, AN_TYPE},
    {"mask-size", PT_MASK_SIZE, 0, false, O(mask_size), NULL, 0, AN_TYPE},
    {"max-height", PT_LEN, LF_NONE, false, O(max_height), NULL, 0, AN_TYPE},
    {"max-width", PT_LEN, LF_NONE, false, O(max_width), NULL, 0, AN_TYPE},
    {"min-height", PT_LEN, LF_AUTO, false, O(min_height), NULL, 0, AN_TYPE},
    {"min-width", PT_LEN, LF_AUTO, false, O(min_width), NULL, 0, AN_TYPE},
    {"opacity", PT_OPACITY, 0, false, O(opacity), NULL, 0, AN_TYPE},
    {"order", PT_INT, 0, false, O(order), NULL, 0, AN_TYPE},
    SH("outline", SH_OUTLINE),
    {"outline-color", PT_COLOR, 0, false, O(outline_color), NULL, 0, AN_TYPE},
    {"outline-offset", PT_PX, LF_NEG | LF_NOPCT, false, O(outline_offset), NULL, 0, AN_TYPE},
    {"outline-style", PT_KW, 0, false, O(outline_style), kw_ostyle, 0, AN_TYPE},
    {"outline-width", PT_PX, LF_NOPCT, false, O(outline_width), kw_bwidth, 0, AN_TYPE},
    SH("overflow", SH_OVERFLOW),
    {"overflow-x", PT_OVERFLOW, 0, false, O(overflow), kw_overflow, 0, AN_TYPE},
    {"overflow-y", PT_OVERFLOW, 0, false, O(overflow), kw_overflow, 0, AN_TYPE},
    SH("padding", SH_PADDING),
    SH("padding-block", SH_PADDING_BLOCK),
    {"padding-bottom", PT_LEN, 0, false, O(padding[2]), NULL, 0, AN_TYPE},
    SH("padding-inline", SH_PADDING_INLINE),
    {"padding-left", PT_LEN, 0, false, O(padding[3]), NULL, 0, AN_TYPE},
    {"padding-right", PT_LEN, 0, false, O(padding[1]), NULL, 0, AN_TYPE},
    {"padding-top", PT_LEN, 0, false, O(padding[0]), NULL, 0, AN_TYPE},
    SH("place-content", SH_PLACE_CONTENT),
    SH("place-items", SH_PLACE_ITEMS),
    SH("place-self", SH_PLACE_SELF),
    {"pointer-events", PT_KW, 0, true, O(pointer_events), kw_pointer_events, 0, AN_TYPE},
    {"position", PT_KW, 0, false, O(position), kw_position, 0, AN_TYPE},
    {"right", PT_LEN, LF_AUTO | LF_NEG, false, O(inset[1]), NULL, 0, AN_TYPE},
    {"row-gap", PT_PX, LF_NOPCT, false, O(gap_row), kw_normal0, 0, AN_TYPE},
    {"stroke", PT_SVG_PAINT, 0, true, O(svg_stroke), NULL, 0, AN_TYPE},
    {"stroke-opacity", PT_OPACITY, 0, true, O(svg_stroke_opacity), NULL, 0, AN_TYPE},
    {"stroke-width", PT_SVG_WIDTH, 0, true, O(svg_stroke_width), NULL, 0, AN_TYPE},
    {"stroke-linecap", PT_KW, 0, true, O(svg_stroke_cap), kw_svg_cap, 0, AN_TYPE},
    {"stroke-linejoin", PT_KW, 0, true, O(svg_stroke_join), kw_svg_join, 0, AN_TYPE},
    {"stop-color", PT_COLOR, 0, false, O(svg_stop_color), NULL, 0, AN_TYPE},
    {"stop-opacity", PT_OPACITY, 0, false, O(svg_stop_opacity), NULL, 0, AN_TYPE},
    {"table-layout", PT_KW, 0, false, O(table_layout), kw_tlayout, 0, AN_TYPE},
    {"text-align", PT_KW, 0, true, O(text_align), kw_talign, 0, AN_TYPE},
    {"text-overflow",PT_KW,0,false,O(text_overflow),kw_text_overflow,0, AN_TYPE},
    SH("text-decoration", SH_TEXT_DECORATION),
    {"text-decoration-line", PT_TEXT_DECO, 0, true, O(text_decoration), NULL, 0, AN_TYPE},
    {"text-indent", PT_LEN, LF_NEG, true, O(text_indent), NULL, 0, AN_TYPE},
    {"text-transform", PT_KW, 0, true, O(text_transform), kw_ttrans, 0, AN_TYPE},
    {"text-wrap-mode", PT_KW, 0, true, O(white_space), kw_textwrap, 0, AN_TYPE},
    {"top", PT_LEN, LF_AUTO | LF_NEG, false, O(inset[0]), NULL, 0, AN_TYPE},
    {"transform", PT_TRANSFORM, 0, false, O(transform), NULL, 0, AN_TYPE},
    {"transform-origin", PT_TRANSFORM_ORIGIN, 0, false, O(transform_origin), NULL, 0, AN_TYPE},
    {"transform-box", PT_KW, 0, false, O(transform_box), kw_transform_box, 0, AN_TYPE},
    {"rotate", PT_ROTATE, 0, false, O(rotate), NULL, 0, AN_TYPE},
    {"scale", PT_SCALE, 0, false, O(scale), NULL, 0, AN_TYPE},
    {"translate", PT_TRANSLATE, 0, false, O(translate), NULL, 0, AN_TYPE},
    {"vertical-align", PT_VALIGN, 0, false, O(vertical_align), kw_valign, 0, AN_TYPE},
    {"visibility", PT_KW, 0, true, O(visibility), kw_visibility, 0, AN_VISIBILITY},
    {"white-space", PT_KW, 0, true, O(white_space), kw_ws, 0, AN_TYPE},
    {"width", PT_LEN, LF_AUTO, false, O(width), NULL, 0, AN_TYPE},
    {"word-spacing", PT_PX, LF_NEG | LF_NOPCT, true, O(word_spacing), kw_normal0, 0, AN_TYPE},
    {"z-index", PT_ZINDEX, 0, false, O(z_index), NULL, 0, AN_TYPE},
};
#define NPROPS (int)(sizeof props / sizeof *props)

/* other names for the same properties (logical properties map to left-to-right, horizontal text) */
static const struct {
    const char *alias, *name;
} aliases[] = {
    {"-webkit-transform", "transform"},
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
    {"padding-inline-start", "padding-left"}, {"word-wrap", ""},
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
    s->svg_fill.kind = SVG_PAINT_COLOR;
    s->svg_fill.color = s->svg_stop_color = 0xFF000000u;
    s->svg_stroke.kind = SVG_PAINT_NONE;
    s->svg_fill_opacity = s->svg_stroke_opacity = s->svg_stop_opacity = 1;
    s->svg_stroke_width.kind = LK_LEN; s->svg_stroke_width.px = 1;
    s->outline_color = COLOR_CURRENT;
    s->outline_width = 3;
    for (int i = 0; i < 4; i++) {
        s->border_color[i] = COLOR_CURRENT;
        s->border_width[i] = 3;
        s->inset[i].kind = LK_AUTO;
        s->margin[i].kind = LK_LEN;
        s->padding[i].kind = LK_LEN;
    }
    s->font_size = 16;
    s->font_weight = 400;
    s->font_family = FONT_FAMILY_SERIF;
    s->line_height.kind = LK_NORMAL;
    s->width.kind = s->height.kind = LK_AUTO;
    s->min_width.kind = s->min_height.kind = LK_AUTO;
    s->max_width.kind = s->max_height.kind = LK_NONE;
    s->text_indent.kind = LK_LEN;
    s->flex_basis.kind = LK_AUTO;
    s->flex_shrink = 1;
    s->align_content = AC_NORMAL;
    s->opacity = 1;
    s->object_pos[0].kind=s->object_pos[1].kind=LK_LEN;
    s->object_pos[0].pct=s->object_pos[1].pct=50;
    s->transform_origin[0].kind=s->transform_origin[1].kind=LK_LEN;
    s->transform_origin[0].pct=s->transform_origin[1].pct=50;
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
const char *css_prop_name(int index,bool *longhand) {
    if(index<0||index>=NPROPS)return NULL;
    props_init();
    if(longhand)*longhand=props[index].type!=PT_SHORT;
    return props[index].name;
}
int css_prop_index(const struct propdef *p) { return (int)(p - props); }
const struct propdef *css_prop_at(int index) { return index >= 0 && index < NPROPS ? &props[index] : NULL; }
/* font-size gives em, color-scheme the system colours and light-dark() */
static bool early(const struct propdef *p) { return p->type == PT_FONT_SIZE || p->type == PT_COLOR_SCHEME; }
bool css_prop_is_early(const struct propdef *p) {
    return early(p) || (p->type == PT_SHORT && (p->sh == SH_FONT || p->sh == SH_ALL));
}
bool css_prop_is_motion(const struct propdef *p) {
    return p && (p->type == PT_MOTION_LIST || (p->type == PT_SHORT && (p->sh == SH_ANIMATION || p->sh == SH_TRANSITION)));
}
bool css_prop_inherited(const struct propdef *p) { return p && p->inherited; }
/* the colour slot (color.c) of a PT_COLOR or PT_SVG_PAINT property */
static enum css_color_slot color_slot(const struct propdef *p) {
    size_t off = p->off;
    if (off == offsetof(style_t, color)) return CSS_SLOT_COLOR;
    if (off == offsetof(style_t, svg_fill)) return CSS_SLOT_FILL;
    if (off == offsetof(style_t, svg_stroke)) return CSS_SLOT_STROKE;
    if (off == offsetof(style_t, bg_color)) return CSS_SLOT_BACKGROUND;
    if (off == offsetof(style_t, outline_color)) return CSS_SLOT_OUTLINE;
    if (off == offsetof(style_t, svg_stop_color)) return CSS_SLOT_STOP;
    return (enum css_color_slot)(CSS_SLOT_BORDER_TOP + (off - offsetof(style_t, border_color)) / sizeof(uint32_t));
}
int css_computed_longhand(const style_t *st, const char *name, sbuf *out, web_doc *d) {
    const struct propdef *p = css_prop_lookup(name, strlen(name));
    if (!p) return 0;
    const void *field = (const char *)st + p->off;
    char text[32];
    switch (p->type) {
    case PT_KW: case PT_OVERFLOW: case PT_VALIGN: {
        if (p->type == PT_VALIGN && st->vertical_align == VA_LEN) return length_number(out, st->vertical_align_px, "px") ? 1 : -1;
        /* Each table names a stored value's canonical keyword first. */
        uint8_t v = *(const uint8_t *)field;
        for (const struct kw *k = p->kws; k && k->name; k++)
            if (k->v == v) { sb_puts(out, k->name); return 1; }
        return 0;
    }
    case PT_LEN:
        return css_length_serialize((const len_t *)field, out, d) ? 1 : -1;
    case PT_PX: {
        if (p->size != sizeof(float)) return 0;
        float v = *(const float *)field;
        if (p->kws == kw_normal0 && v == 0) { sb_puts(out, "normal"); return 1; }
        return length_number(out, v, "px") ? 1 : -1;
    }
    case PT_NUM: case PT_OPACITY:
        snprintf(text, sizeof text, "%g", (double)*(const float *)field); sb_puts(out, text); return 1;
    case PT_INT:
        if (p->size != sizeof(int32_t)) return 0;
        snprintf(text, sizeof text, "%d", (int)*(const int32_t *)field); sb_puts(out, text); return 1;
    case PT_COLOR: return css_style_color_text(st, color_slot(p), out) ? 1 : -1;
    case PT_COLOR_SCHEME: sb_puts(out, st->color_scheme_text ? st->color_scheme_text : "normal"); return 1;
    case PT_MOTION_LIST: return css_computed_value(st, p, out, d) ? 1 : -1;
    }
    return 0;
}
/* CSSOM: a declared value's specified serialization for the properties whose
   values the table serializes itself; false leaves the authored text */
static bool apply_long(const struct propdef *p, const char *v, size_t n, struct cx *cx);
bool css_specified_longhand(const char *name, const char *v, size_t n, sbuf *out) {
    const struct propdef *p = css_prop_lookup(name, strlen(name));
    if (!p) return false;
    trim(&v, &n);
    if (p->type == PT_COLOR || (p->type == PT_SVG_PAINT && !ident_is(v, n, "none"))) return css_color_specified(v, n, out);
    if (p->type != PT_COLOR_SCHEME) return false;
    arena_t a = {.chunk_size = 1024};
    jmp_buf trap;
    a.trap = &trap;
    bool ok = false;
    if (!setjmp(trap)) {
        style_t scratch = {0};
        uint8_t set[1] = {0};
        struct cx cx = {.s = &scratch, .a = &a, .set = set, .supports_probe = true};
        if (apply_long(p, v, n, &cx)) {
            sb_puts(out, scratch.color_scheme_text ? scratch.color_scheme_text : "normal");
            ok = true;
        }
    }
    ar_free(&a);
    return ok;
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

static bool parse_display(const char *s, size_t n, uint8_t *out, struct cx *cx) {
    const char *t[3];
    size_t tl[3];
    int k = split_checked(s, n, t, tl, 3, ' ', cx);
    if (k == 1) {
        if (cx->supports_probe && (ident_is(s,n,"run-in") || ident_is(s,n,"ruby") ||
            ident_is(s,n,"ruby-base") || ident_is(s,n,"ruby-text") ||
            ident_is(s,n,"-moz-box"))) return false;
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
        if (cx->supports_probe && inner >= 0 && (ident_is(t[i],tl[i],"flow") || ident_is(t[i],tl[i],"flow-root") ||
            ident_is(t[i],tl[i],"flex") || ident_is(t[i],tl[i],"grid") || ident_is(t[i],tl[i],"table"))) return false;
        if (ident_is(t[i], tl[i], "inline")) { if (cx->supports_probe && (inl || block)) return false; inl = true; }
        else if (ident_is(t[i], tl[i], "block")) { if (cx->supports_probe && (inl || block)) return false; block = true; }
        else if (ident_is(t[i], tl[i], "list-item")) { if (cx->supports_probe && li) return false; li = true; }
        else if (ident_is(t[i], tl[i], "flow")) inner = 0;
        else if (ident_is(t[i], tl[i], "flow-root")) inner = 1;
        else if (ident_is(t[i], tl[i], "flex")) inner = 2;
        else if (ident_is(t[i], tl[i], "grid")) inner = 3;
        else if (ident_is(t[i], tl[i], "table")) inner = 4;
        else return false;
    }
    if (inl && block) return false;
    if (cx->supports_probe && li && inner > 1) return false;
    if (li) *out = D_LIST_ITEM;
    else if (inner == 2) *out = inl ? D_INLINE_FLEX : D_FLEX;
    else if (inner == 3) *out = inl ? D_INLINE_GRID : D_GRID;
    else if (inner == 4) *out = inl ? D_INLINE_TABLE : D_TABLE;
    else if (inner == 1) *out = inl ? D_INLINE_BLOCK : D_FLOW_ROOT;
    else *out = inl ? D_INLINE : D_BLOCK;
    return true;
}

static const float font_kw_px[] = {9, 10, 13, 16, 18, 24, 32, 48};
float css_font_keyword_px(unsigned keyword) {
    return font_kw_px[keyword < sizeof font_kw_px / sizeof *font_kw_px ? keyword : 3];
}

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

const char *css_font_family_next(arena_t *a,const char **cursor,const char *end) {
    const char *s=*cursor;while(s<end && is_space((unsigned char)*s))s++;
    const char *begin=s,*finish=s;bool quoted=s<end && (*s=='"' || *s=='\'');
    if(quoted) {
        char quote=*s++;begin=s;
        while(s<end && *s!=quote) {if(*s=='\\' && s+1<end)s++;s++;}
        if(s==end)goto bad;finish=s++;
        while(s<end && is_space((unsigned char)*s))s++;
        if(s<end && *s!=',')goto bad;
    } else {
        while(s<end && *s!=',') {
            if(*s=='\\') {s++;if(s==end || *s=='\n' || *s=='\r' || *s=='\f')goto bad;
                if(isxdigit((unsigned char)*s)){unsigned n=0;while(s<end && n<6 && isxdigit((unsigned char)*s)){s++;n++;}if(s<end && is_space((unsigned char)*s))s++;}
                else s++;
                continue;
            }
            if(!isalnum((unsigned char)*s) && *s!='-' && *s!='_' && (unsigned char)*s<128 && !is_space((unsigned char)*s))goto bad;
            if((s==begin || is_space((unsigned char)s[-1])) && isdigit((unsigned char)*s))goto bad;
            s++;
        }
        finish=s;while(finish>begin && is_space((unsigned char)finish[-1]))finish--;
    }
    /* A <string> family name may be empty; a <custom-ident>+ one may not. */
    if(!quoted && finish==begin)goto bad;
    sbuf b={0};css_unescape(&b,begin,(size_t)(finish-begin));
    if(!quoted) {
        size_t write=0;bool space=false;
        for(size_t i=0;i<b.n;i++)if(is_space((unsigned char)b.p[i]))space=true;
        else {if(space && write)b.p[write++]=' ';b.p[write++]=b.p[i];space=false;}
        b.n=write;
    }
    const char *name=ar_strndup(a,b.p?b.p:"",b.n);sb_free(&b);
    /* <custom-ident> excludes the CSS-wide keywords and default (CSS Values 4). */
    if(!quoted && (!strcasecmp(name,"inherit") || !strcasecmp(name,"initial") || !strcasecmp(name,"unset") ||
       !strcasecmp(name,"revert") || !strcasecmp(name,"revert-layer") || !strcasecmp(name,"default")))goto bad;
    if(s<end){s++;const char *tail=s;while(tail<end && is_space((unsigned char)*tail))tail++;if(tail==end)goto bad;}
    *cursor=s;return name;
bad:*cursor=NULL;return NULL;
}

static bool choose_family(const char *s, size_t n, uint8_t *out, struct cx *cx) {
    const char *cursor=s,*end=s+n;bool any=false,chosen=false;
    *out=FONT_FAMILY_SERIF;
    static const char *const mono[] = {"monospace", "ui-monospace", "courier", "courier new", "consolas",
                                       "menlo", "monaco", "sfmono-regular", "sf mono", "fira code", "fira mono",
                                       "source code pro", "dejavu sans mono", "liberation mono", "roboto mono",
                                       "jetbrains mono", "lucida console", "andale mono", "ubuntu mono",
                                       "cascadia code", "cascadia mono", "ibm plex mono", "noto sans mono",
                                       "inconsolata", "hack", "go mono", "lucida sans typewriter",
                                       "maple mono", "maple mono nf", "maple mono nf cn", NULL};
    /* Named families in these lists use the bundled class substitute; this is
       not a claim that Arial/Times or downloaded webfonts are installed. */
    static const char *const serif[] = {"serif", "ui-serif", "georgia", "times", "times new roman", "noto serif", NULL};
    static const char *const sans[] = {"sans-serif", "system-ui", "-apple-system", "blinkmacsystemfont",
                                       "ui-sans-serif", "arial", "helvetica", "helvetica neue",
                                       "inter", "roboto", "segoe ui", "verdana",
                                       "tahoma", "cursive", "fantasy", "noto sans", "open sans",
                                       "ubuntu", "cantarell", "lato", NULL};
    while(cursor<end) {
        const char *f=css_font_family_next(cx->a,&cursor,end);
        if(!f || !cursor)return false;any=true;size_t fl=strlen(f);
        if(chosen)continue;
        for(int j=0;mono[j];j++)if(ident_is(f,fl,mono[j])){*out=FONT_FAMILY_MONO;chosen=true;break;}
        if(chosen)continue;
        for(int j=0;serif[j];j++)if(ident_is(f,fl,serif[j])){*out=FONT_FAMILY_SERIF;chosen=true;break;}
        if(chosen)continue;
        for(int j=0;sans[j];j++)if(ident_is(f,fl,sans[j])){*out=FONT_FAMILY_SANS;chosen=true;break;}
    }
    return any;

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
    int layers = split_checked(s, n, layer, ll, 8, ',', cx);
    if (layers < 1 || (cx->supports_probe && layers != 1)) return false;
    const char *t[4];
    size_t tl[4];
    int k = split_checked(layer[0], ll[0], t, tl, 4, ' ', cx);
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
    int layers = split_checked(s, n, layer, ll, 8, ',', cx);
    if (layers < 1 || (cx->supports_probe && layers != 1)) return false;
    s = layer[0];
    n = ll[0];
    memset(size, 0, 2 * sizeof *size);
    if (ident_is(s, n, "cover")) *kind = BSZ_COVER;
    else if (ident_is(s, n, "contain")) *kind = BSZ_CONTAIN;
    else {
        const char *t[2];
        size_t tl[2];
        int k = split_checked(s, n, t, tl, 2, ' ', cx);
        if (k < 1) return false;
        for (int i = 0; i < k; i++)
            if (!parse_len(t[i], tl[i], cx, &size[i], LF_AUTO)) return false;
        *kind = size[0].kind == LK_AUTO && size[1].kind == LK_AUTO ? BSZ_AUTO : BSZ_LEN;
    }
    return true;
}

/* ---------------------------------------------------------------- clip-path and clip */
/* A <border-radius> value: one to four horizontal radii and, after a /, one to four vertical ones,
   expanded to each axis's four corners (TL TR BR BL). */
static bool parse_radius_list(const char *v, size_t n, struct cx *cx, len_t lengths[2][4]) {
    const char *axes[3];size_t sizes[3];int count=split_checked(v,n,axes,sizes,3,'/',cx);
    if(count<1||count>2||!sizes[0]||(count==2&&!sizes[1]))return false;
    const char *consumed=axes[count-1]+sizes[count-1];while(consumed<v+n&&is_space((unsigned char)*consumed))consumed++;
    if(consumed!=v+n)return false;
    static const int pick[5][4]={{0},{0,0,0,0},{0,1,0,1},{0,1,2,1},{0,1,2,3}};
    for(int axis=0;axis<count;axis++) {
        const char *tokens[5];size_t lengths_n[5];int k=split_checked(axes[axis],sizes[axis],tokens,lengths_n,5,' ',cx);
        if(k<1||k>4)return false;
        len_t values[4];for(int i=0;i<k;i++)if(!parse_len(tokens[i],lengths_n[i],cx,&values[i],LF_LENGTH))return false;
        for(int i=0;i<4;i++)lengths[axis][i]=values[pick[k][i]];
    }
    if(count==1)memcpy(lengths[1],lengths[0],sizeof lengths[0]);
    return true;
}
static const struct kw kw_clip_box[]={{"border-box",CLIP_BOX_BORDER},{"padding-box",CLIP_BOX_PADDING},
    {"content-box",CLIP_BOX_CONTENT},{"margin-box",CLIP_BOX_MARGIN},{"fill-box",CLIP_BOX_FILL},
    {"stroke-box",CLIP_BOX_STROKE},{"view-box",CLIP_BOX_VIEW},{0}};
/* name(arguments): the arguments, NULL for any other token */
static const char *function_args(const char *v,size_t n,const char *name,size_t *size) {
    size_t k=strlen(name);
    if(n<k+2||!strn_ieq(v,name,k)||v[k]!='('||v[n-1]!=')')return NULL;
    *size=n-k-2;return v+k+1;
}
/* The end of the comma-separated item at s: the next top-level comma, or end. */
static const char *item_end(const char *s,const char *end) {
    int depth=0;char quote=0;
    for(;s<end;s++) {
        char c=*s;
        if(quote){if(c=='\\'&&s+1<end)s++;else if(c==quote)quote=0;}
        else if(c=='"'||c=='\'')quote=c;
        else if(c=='(')depth++;
        else if(c==')')depth--;
        else if(c==','&&depth<=0)break;
    }
    return s;
}
static bool ends_percent(const char *t,size_t n){return n&&t[n-1]=='%';}
/* 100% - a - b: inset()'s far edge for rect()'s and xywh()'s (css-shapes-1 §3.2). */
static void clip_far_edge(struct cx *cx,const len_t *a,const len_t *b,len_t *out) {
    if(a->kind==LK_LEN&&(!b||b->kind==LK_LEN)) {
        *out=(len_t){.kind=LK_LEN,.px=-a->px-(b?b->px:0),.pct=100-a->pct-(b?b->pct:0)};return;
    }
    struct cexpr *x=ar_alloc(cx->a,sizeof *x);*x=(struct cexpr){.op='v',.pct=100};
    const len_t *terms[2]={a,b};
    for(int i=0;i<2&&terms[i];i++) {
        struct cexpr *term=terms[i]->expr;
        if(terms[i]->kind!=LK_EXPR){term=ar_alloc(cx->a,sizeof *term);*term=(struct cexpr){.op='v',.px=terms[i]->px,.pct=terms[i]->pct};}
        struct cexpr *difference=ar_alloc(cx->a,sizeof *difference);*difference=(struct cexpr){.op='-',.a=x,.b=term};x=difference;
    }
    *out=(len_t){.kind=LK_EXPR,.expr=x};
}
/* [round <'border-radius'>]? closing a rectangle function: the corners (zero without it), and in
   *before how much of the arguments precedes it */
static bool parse_clip_round(const char *v,size_t n,struct cx *cx,struct css_clip_path *c,size_t *before) {
    const char *t[16];size_t tl[16];int k=split_checked(v,n,t,tl,16,' ',cx);
    *before=n;
    for(int i=0;i<4;i++)for(int axis=0;axis<2;axis++)c->corner[i].axis[axis]=(len_t){.kind=LK_LEN};
    for(int i=0;i<k;i++)if(ident_is(t[i],tl[i],"round")) {
        *before=(size_t)(t[i]-v);
        const char *radii=t[i]+tl[i];len_t lengths[2][4];
        if(!parse_radius_list(radii,(size_t)(v+n-radii),cx,lengths))return false;
        for(int corner=0;corner<4;corner++)for(int axis=0;axis<2;axis++)c->corner[corner].axis[axis]=lengths[axis][corner];
        break;
    }
    return true;
}
static bool parse_clip_sides(const char *v,size_t n,struct cx *cx,struct css_clip_path *c) {
    const char *t[5];size_t tl[5];int k=split_checked(v,n,t,tl,5,' ',cx);
    if(k<1||k>4)return false;
    static const int pick[5][4]={{0},{0,0,0,0},{0,1,0,1},{0,1,2,1},{0,1,2,3}};
    len_t values[4];unsigned percent=0;
    for(int i=0;i<k;i++) {
        if(!parse_len(t[i],tl[i],cx,&values[i],LF_NEG|LF_LENGTH))return false;
        if(ends_percent(t[i],tl[i]))percent|=1u<<i;
    }
    for(int i=0;i<4;i++){c->inset[i]=values[pick[k][i]];if(percent>>pick[k][i]&1)c->inset_percent|=(uint8_t)(1u<<i);}
    return true;
}
/* xywh(x y w h): inset(y, 100% - x - w, 100% - y - h, x) */
static bool parse_clip_xywh(const char *v,size_t n,struct cx *cx,struct css_clip_path *c) {
    const char *t[5];size_t tl[5];
    if(split_checked(v,n,t,tl,5,' ',cx)!=4)return false;
    len_t x[4];
    for(int i=0;i<4;i++)if(!parse_len(t[i],tl[i],cx,&x[i],(i<2?LF_NEG:0)|LF_LENGTH))return false;
    c->inset[0]=x[1];c->inset[3]=x[0];
    clip_far_edge(cx,&x[0],&x[2],&c->inset[1]);clip_far_edge(cx,&x[1],&x[3],&c->inset[2]);
    c->inset_percent=(uint8_t)(1u<<1|1u<<2|(ends_percent(t[1],tl[1])?1u:0)|(ends_percent(t[0],tl[0])?1u<<3:0));
    return true;
}
/* rect(top right bottom left), auto edges the reference box's: inset(top, 100% - right, 100% - bottom, left) */
static bool parse_clip_rect_shape(const char *v,size_t n,struct cx *cx,struct css_clip_path *c) {
    const char *t[5];size_t tl[5];
    if(split_checked(v,n,t,tl,5,' ',cx)!=4)return false;
    len_t edge[4];unsigned percent=1u<<1|1u<<2;
    for(int i=0;i<4;i++) {
        if(ident_is(t[i],tl[i],"auto")){edge[i]=(len_t){.kind=LK_LEN,.pct=i==1||i==2?100:0};percent|=1u<<i;}
        else if(!parse_len(t[i],tl[i],cx,&edge[i],LF_NEG|LF_LENGTH))return false;
        else if(ends_percent(t[i],tl[i]))percent|=1u<<i;
    }
    c->inset[0]=edge[0];c->inset[3]=edge[3];
    clip_far_edge(cx,&edge[1],NULL,&c->inset[1]);clip_far_edge(cx,&edge[2],NULL,&c->inset[2]);
    c->inset_percent=(uint8_t)percent;
    return true;
}
/* circle( <radial-size>? [at <position>]? ), ellipse( [<radial-size>{2}]? [at <position>]? ) */
static bool parse_clip_radial(const char *v,size_t n,struct cx *cx,struct css_clip_path *c,int radii) {
    const char *t[8];size_t tl[8];int k=split_checked(v,n,t,tl,8,' ',cx);
    if(k==8)return false;
    int at=k;for(int i=0;i<k;i++)if(ident_is(t[i],tl[i],"at")){at=i;break;}
    if(at&&at!=radii)return false;
    c->extent[0]=c->extent[1]=CLIP_EXTENT_CLOSEST;
    for(int i=0;i<at;i++) {
        if(ident_is(t[i],tl[i],"closest-side"))c->extent[i]=CLIP_EXTENT_CLOSEST;
        else if(ident_is(t[i],tl[i],"farthest-side"))c->extent[i]=CLIP_EXTENT_FARTHEST;
        else if(parse_len(t[i],tl[i],cx,&c->radius[i],LF_LENGTH))c->extent[i]=CLIP_EXTENT_LENGTH;
        else return false;
    }
    c->at[0]=c->at[1]=(len_t){.kind=LK_LEN,.pct=50};
    if(at==k)return true;
    if(at+1==k)return false;
    const char *position=t[at+1];size_t size=(size_t)(v+n-position);trim(&position,&size);
    return !memchr(position,',',size)&&parse_bg_pos(position,size,cx,c->at);
}
/* polygon( [<fill-rule> || round <length>]?, [<length-percentage> <length-percentage>]# ) */
static bool parse_clip_polygon(const char *v,size_t n,struct cx *cx,struct css_clip_path **out) {
    const char *end=v+n,*first=item_end(v,end);
    size_t items=1;for(const char *q=first;q<end;q=item_end(q+1,end))items++;
    const char *t[5];size_t tl[5];int k=split_checked(v,(size_t)(first-v),t,tl,5,' ',cx);
    bool evenodd=false;float round=0;
    if(k>0&&(ident_is(t[0],tl[0],"nonzero")||ident_is(t[0],tl[0],"evenodd")||ident_is(t[0],tl[0],"round"))) {
        int i=0;
        if(!ident_is(t[0],tl[0],"round")){evenodd=ident_is(t[0],tl[0],"evenodd");i=1;}
        if(i<k) {
            len_t r;
            if(!ident_is(t[i],tl[i],"round")||i+1>=k||!parse_len(t[i+1],tl[i+1],cx,&r,LF_LENGTH|LF_NOPCT)||r.kind!=LK_LEN)return false;
            round=r.px;i+=2;
        }
        if(i!=k||first==end)return false;
        v=first+1;items--;
    }
    if(items>(SIZE_MAX-sizeof(struct css_clip_path))/sizeof(len_t[2]))return false;
    struct css_clip_path *c=ar_alloc(cx->a,sizeof *c+items*sizeof c->point[0]);
    memset(c,0,sizeof *c);
    c->shape=CLIP_SHAPE_POLYGON;c->evenodd=evenodd;c->round=round;c->points=items;
    for(size_t i=0;i<items;i++) {
        const char *e=item_end(v,end),*xy[3];size_t xyn[3];
        if(split_checked(v,(size_t)(e-v),xy,xyn,3,' ',cx)!=2||!parse_len(xy[0],xyn[0],cx,&c->point[i][0],LF_NEG|LF_LENGTH)||
           !parse_len(xy[1],xyn[1],cx,&c->point[i][1],LF_NEG|LF_LENGTH))return false;
        if(!(i&63)&&!cx->supports_probe&&cx->node&&!web_native_checkpoint(cx->node->owner))return false;
        v=e+1;
    }
    *out=c;return true;
}
/* path( [<fill-rule>,]? <string> ), the string SVG path data */
static bool parse_clip_path_data(const char *v,size_t n,struct cx *cx,struct css_clip_path **out) {
    const char *end=v+n,*e=item_end(v,end);bool evenodd=false;
    if(e!=end) {
        const char *rule=v;size_t size=(size_t)(e-v);trim(&rule,&size);
        if(ident_is(rule,size,"evenodd"))evenodd=true;
        else if(!ident_is(rule,size,"nonzero"))return false;
        v=e+1;
        if(item_end(v,end)!=end)return false;
    }
    size_t size=(size_t)(end-v);trim(&v,&size);
    if(size<2||(v[0]!='"'&&v[0]!='\'')||v[size-1]!=v[0])return false;
    sbuf data={0};css_unescape(&data,v+1,size-2);
    bool valid=data.p&&svg_path_data_valid(data.p);
    const char *text=valid?ar_strndup(cx->a,data.p,data.n):NULL;
    sb_free(&data);
    if(!valid)return false;
    struct css_clip_path *c=ar_alloc(cx->a,sizeof *c);memset(c,0,sizeof *c);
    c->shape=CLIP_SHAPE_PATH;c->evenodd=evenodd;c->text=text;*out=c;return true;
}
static bool parse_clip_shape(const char *v,size_t n,struct cx *cx,struct css_clip_path **out) {
    size_t size,before;const char *args;
    if((args=function_args(v,n,"polygon",&size)))return parse_clip_polygon(args,size,cx,out);
    if((args=function_args(v,n,"path",&size)))return parse_clip_path_data(args,size,cx,out);
    struct css_clip_path *c=ar_alloc(cx->a,sizeof *c);memset(c,0,sizeof *c);*out=c;
    if((args=function_args(v,n,"circle",&size))){c->shape=CLIP_SHAPE_CIRCLE;return parse_clip_radial(args,size,cx,c,1);}
    if((args=function_args(v,n,"ellipse",&size))){c->shape=CLIP_SHAPE_ELLIPSE;return parse_clip_radial(args,size,cx,c,2);}
    c->shape=CLIP_SHAPE_INSET;
    if((args=function_args(v,n,"inset",&size)))return parse_clip_round(args,size,cx,c,&before)&&parse_clip_sides(args,before,cx,c);
    if((args=function_args(v,n,"xywh",&size)))return parse_clip_round(args,size,cx,c,&before)&&parse_clip_xywh(args,before,cx,c);
    if((args=function_args(v,n,"rect",&size)))return parse_clip_round(args,size,cx,c,&before)&&parse_clip_rect_shape(args,before,cx,c);
    return false;
}
/* clip-path: none | <url> | [<basic-shape> || <geometry-box>] */
static bool parse_clip_path(const char *v,size_t n,struct cx *cx,const struct css_clip_path **out) {
    trim(&v,&n);
    if(ident_is(v,n,"none")){*out=NULL;return true;}
    const char *t[3];size_t tl[3];int k=split_checked(v,n,t,tl,3,' ',cx);
    if(k<1||k>2)return false;
    if(k==1&&n>=5&&strn_ieq(v,"url(",4)&&v[n-1]==')') {
        const char *ref=parse_url(v,n,cx->a);if(!ref)return false;
        struct css_clip_path *c=ar_alloc(cx->a,sizeof *c);memset(c,0,sizeof *c);
        c->shape=CLIP_SHAPE_URL;c->text=ref;*out=c;return true;
    }
    struct css_clip_path *shape=NULL;uint8_t box=CLIP_BOX_BORDER;bool box_given=false;
    for(int i=0;i<k;i++) {
        if(kw_find(kw_clip_box,t[i],tl[i],&box)){if(box_given)return false;box_given=true;continue;}
        if(shape||!parse_clip_shape(t[i],tl[i],cx,&shape))return false;
    }
    if(!shape){shape=ar_alloc(cx->a,sizeof *shape);memset(shape,0,sizeof *shape);shape->shape=CLIP_SHAPE_BOX;}
    shape->box=box;*out=shape;return true;
}
/* clip: auto | rect(<top>, <right>, <bottom>, <left>), each a length or auto; CSS 2.1's spaces still parse */
static bool parse_clip_rect(const char *v,size_t n,struct cx *cx,struct css_clip_rect *out) {
    trim(&v,&n);
    struct css_clip_rect r;memset(&r,0,sizeof r);
    if(ident_is(v,n,"auto")){*out=r;return true;}
    size_t size;const char *args=function_args(v,n,"rect",&size);
    if(!args)return false;
    const char *t[5];size_t tl[5];int k=split_checked(args,size,t,tl,5,',',cx);
    if(k==1)k=split_checked(args,size,t,tl,5,' ',cx);
    if(k!=4)return false;
    r.rect=true;
    for(int i=0;i<4;i++) {
        if(ident_is(t[i],tl[i],"auto")){r.auto_edges|=(uint8_t)(1u<<i);continue;}
        len_t edge;
        if(!parse_len(t[i],tl[i],cx,&edge,LF_NEG|LF_LENGTH|LF_NOPCT)||edge.kind!=LK_LEN)return false;
        r.edge[i]=edge.px;
    }
    *out=r;return true;
}

static bool parse_mask_image(const char *s, size_t n, struct cx *cx) {
    if (ident_is(s, n, "none")) {
        cx->s->mask_image = NULL;
        cx->s->mask_img = 0;
        return true;
    }
    const char *t[8];
    size_t tl[8];
    int k = split_checked(s, n, t, tl, 8, ',', cx);
    if (cx->supports_probe && (k != 1 || !strn_ieq(s, "url(", 4))) return false;
    for (int i = 0; i < k; i++) {
        const char *u = parse_url(t[i], tl[i], cx->a);
        if (u) {
            cx->s->mask_image = u;
            cx->s->mask_img = 0;
            return true;
        }
    }
    return false;
}

static bool transform_angle(const char *s, size_t n, float *out);
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
    int m = split_checked(s + fl + 1, n - fl - 2, a, al, GRAD_MAX + 2, ',', cx);
    for (int j = 0; j < m; j++) {
        const char *c[8];
        size_t cl[8];
        int q = split_checked(a[j], al[j], c, cl, 8, ' ', cx);
        uint32_t col;
        const struct css_color_value *exact;
        if (q >= 1 && css_color_value_compute(c[0], cl[0], cx, NULL, &col, &exact)) { /* a colour stop: colour [pos [pos]] */
            if (cx->supports_probe && q > 3) return NULL;
            for (int z = 0; z < 2 && g.n < GRAD_MAX; z++) {
                if (z == 1 && q < 3) break;
                g.col[g.n] = col;
                g.col_value[g.n] = col == COLOR_CURRENT ? exact : NULL;
                g.pos[g.n].kind = LK_AUTO;
                if (q > 1 + z && !parse_len(c[1 + z], cl[1 + z], cx, &g.pos[g.n], 0)) {
                    if (cx->supports_probe) return NULL;
                    g.pos[g.n].kind = LK_AUTO;
                }
                g.n++;
            }
            continue;
        }
        if (q == 1 && g.n && j < m - 1) { /* a colour hint: ignored */
            len_t hint;
            if (parse_len(c[0], cl[0], cx, &hint, 0)) { if (cx->supports_probe) return NULL; continue; }
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
            } else if (q == 1 && transform_angle(c[0], cl[0], &deg)) g.angle = deg * 180 / (float)M_PI;
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
                if (cx->supports_probe) return NULL;
                if (!parse_len(c[z], cl[z], cx, &l, 0)) return NULL;
            }
        }
    }
    if (g.n < 1 || (cx->supports_probe && g.n < 2)) return NULL;
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
        st->bg_img = 0;
        st->has_grad = false;
        st->gradient = NULL;
        return true;
    }
    /* several layers: the first one that we can use */
    const char *t[8];
    size_t tl[8];
    int k = split_checked(s, n, t, tl, 8, ',', cx);
    if (cx->supports_probe && k != 1) return false; /* no layered painting */
    for (int i = 0; i < k; i++) {
        const char *u = parse_url(t[i], tl[i], cx->a);
        if (u) {
            st->bg_image = u;
            st->bg_img = 0;st->has_grad=false;st->gradient=NULL;
            return true;
        }
        const struct gradient *g = parse_gradient(t[i], tl[i], cx);
        if (g) {
            st->bg_image=NULL;st->bg_img=0;
            st->gradient = g;
            st->grad[0] = g->col[0];
            st->grad[1] = g->col[g->n - 1];
            st->has_grad = true;
            return true;
        }
    }
    return !cx->supports_probe && k > 0;
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
            if (cx->supports_probe) return false;
            n = i;
            break;
        }
    }
    const char *t[32];
    size_t tl[32];
    int k = split_checked(s, n, t, tl, 32, ' ', cx);
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
                    if (!e->attrs[j].namespace_uri && strlen(e->attrs[j].raw) == anl &&
                        !memcmp(an, e->attrs[j].raw, anl)) sb_puts(&b, e->attrs[j].value);
        } else if (vn > 8 && (strn_ieq(v, "counter(", 8) || strn_ieq(v, "counters(", 9))) {
            if (cx->supports_probe) { sb_free(&b); return false; }
        } else if (vn > 4 && (strn_ieq(v, "url(", 4) || strn_ieq(v, "image-set(", 10) ||
                              strstr(v, "gradient("))) {
            if (cx->supports_probe) { sb_free(&b); return false; }
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
        int m = split_checked(s + k + 1, n - k - 2, a, al, 2, ',', cx);
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
    int k = split_checked(s, n, t, tl, 64, ' ', cx), cnt = 0;
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
            if (split_checked(t[i] + f + 1, tl[i] - f - 2, a, al, 2, ',', cx) != 2) return -1;
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
    int rows = split_checked(s, n, t, tl, 64, ' ', cx);
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

/* animation-name and transition-property identifiers: <custom-ident> */
static bool motion_name(const char *s, size_t n) {
    if (!n || ident_is(s,n,"initial") || ident_is(s,n,"inherit") || ident_is(s,n,"unset") ||
        ident_is(s,n,"revert") || ident_is(s,n,"revert-layer") || ident_is(s,n,"default") ||
        isdigit((unsigned char)s[0]) || s[0]=='+') return false;
    if (s[0]=='-' && (n==1 || isdigit((unsigned char)s[1]))) return false;
    for (size_t i=0;i<n;i++) if ((unsigned char)s[i]<128 && !isalnum((unsigned char)s[i]) &&
        s[i]!='_' && s[i]!='-') return false;
    return true;
}
static bool motion_time(const char *s,size_t n,float *out) {
    const char *p=s;float v;
    if (!parse_number(&p,s+n,&v) || !isfinite(v)) return false;
    if (s+n-p==1 && (*p|32)=='s') v*=1000;
    else if (s+n-p!=2 || (p[0]|32)!='m' || (p[1]|32)!='s') return false;
    if (!isfinite(v)) return false;
    *out=v;return true;
}
/* <easing-function> (css-easing-1 §2): linear, the cubic-bezier() keywords and function,
   step-start/step-end and steps(<integer>[, <step-position>]). */
bool css_easing_parse(const char *s,size_t n,struct css_easing *out) {
    trim(&s,&n);
    struct css_easing v={.kind=CM_BEZIER,.y2=1};
    static const struct {const char *name;float x1,y1,x2;uint8_t keyword;} named[]={
        {"ease",.25f,.1f,.25f,CM_EASE},{"ease-in",.42f,0,1,CM_EASE_IN},{"ease-out",0,0,.58f,CM_EASE_OUT},
        {"ease-in-out",.42f,0,.58f,CM_EASE_IN_OUT}};
    for(size_t i=0;i<sizeof named/sizeof *named;i++)if(ident_is(s,n,named[i].name)){
        v.x1=named[i].x1;v.y1=named[i].y1;v.x2=named[i].x2;v.keyword=named[i].keyword;*out=v;return true;
    }
    if(ident_is(s,n,"linear")){*out=(struct css_easing){.kind=CM_LINEAR};return true;}
    if(ident_is(s,n,"step-start")){*out=(struct css_easing){.kind=CM_STEPS,.steps=1,.position=CM_STEP_START};return true;}
    if(ident_is(s,n,"step-end")){*out=(struct css_easing){.kind=CM_STEPS,.steps=1,.position=CM_STEP_END};return true;}
    if(n>7 && strn_ieq(s,"steps(",6) && s[n-1]==')') {
        const char *t[3];size_t tl[3];int k=split(s+6,n-7,t,tl,3,',');
        if(k<1 || k>2)return false;
        const char *p=t[0];float count;
        if(!parse_number(&p,t[0]+tl[0],&count) || p!=t[0]+tl[0] || count!=floorf(count) || count<1 || count>1e9f)return false;
        uint8_t position=CM_STEP_END;
        if(k==2) {
            static const struct kw positions[]={{"end",CM_STEP_END},{"jump-end",CM_STEP_END},{"start",CM_STEP_START},
                {"jump-start",CM_STEP_JUMP_START},{"jump-both",CM_STEP_JUMP_BOTH},{"jump-none",CM_STEP_JUMP_NONE},{0}};
            if(!kw_find(positions,t[1],tl[1],&position))return false;
        }
        if(position==CM_STEP_JUMP_NONE && count<2)return false;
        *out=(struct css_easing){.kind=CM_STEPS,.steps=(uint32_t)count,.position=position};return true;
    }
    if(n<15 || !strn_ieq(s,"cubic-bezier(",13) || s[n-1]!=')')return false;
    const char *p=s+13,*e=s+n-1;float a[4];
    for(int i=0;i<4;i++) {
        while(p<e && is_space((unsigned char)*p))p++;
        if(!parse_number(&p,e,&a[i]) || !isfinite(a[i]))return false;
        while(p<e && is_space((unsigned char)*p))p++;
        if(i<3){if(p==e || *p++!=',')return false;}else if(p!=e)return false;
    }
    if(a[0]<0 || a[0]>1 || a[2]<0 || a[2]>1)return false;
    v.x1=a[0];v.y1=a[1];v.x2=a[2];v.y2=a[3];
    *out=v;return true;
}
static double bezier_at(double t,double p1,double p2){double u=1-t;return 3*u*u*t*p1+3*u*t*t*p2+t*t*t;}
double css_easing_apply(const struct css_easing *e,double x,bool before) {
    if(!e || e->kind==CM_LINEAR || !isfinite(x))return x;
    if(e->kind==CM_STEPS) {
        /* css-easing-1 §2.3 step easing function */
        double steps=e->steps?e->steps:1,current=floor(x*steps);
        if(e->position==CM_STEP_START || e->position==CM_STEP_JUMP_START || e->position==CM_STEP_JUMP_BOTH)current+=1;
        if(before && fmod(x*steps,1)==0)current-=1;
        if(x>=0 && current<0)current=0;
        double jumps=e->position==CM_STEP_JUMP_BOTH?steps+1:e->position==CM_STEP_JUMP_NONE?steps-1:steps;
        if(x<=1 && current>jumps)current=jumps;
        return current/jumps;
    }
    double x1=e->x1,y1=e->y1,x2=e->x2,y2=e->y2;
    /* §2.2: outside [0, 1] the curve continues along its end tangents */
    if(x<0) {
        if(x1>0)return y1/x1*x;
        if(y1==0 && x2>0)return y2/x2*x;
        return 0;
    }
    if(x>1) {
        if(x2<1)return 1+(y2-1)/(x2-1)*(x-1);
        if(y2==1 && x1<1)return 1+(y1-1)/(x1-1)*(x-1);
        return 1;
    }
    if(x==0 || x==1)return x;
    double t=x;
    for(int i=0;i<8;i++) {
        double err=bezier_at(t,x1,x2)-x,u=1-t,slope=3*u*u*x1+6*u*t*(x2-x1)+3*t*t*(1-x2);
        if(fabs(err)<1e-9)return bezier_at(t,y1,y2);
        if(fabs(slope)<1e-6)break;
        t-=err/slope;
    }
    double lo=0,hi=1;t=x;
    for(int i=0;i<60;i++){double z=bezier_at(t,x1,x2);if(fabs(z-x)<1e-9)break;if(z<x)lo=t;else hi=t;t=(lo+hi)/2;}
    return bezier_at(t,y1,y2);
}
bool css_easing_serialize(const struct css_easing *e,sbuf *out) {
    static const char *const keywords[]={NULL,"ease","ease-in","ease-out","ease-in-out"};
    char text[160];
    if(e->kind==CM_LINEAR)snprintf(text,sizeof text,"linear");
    else if(e->kind==CM_STEPS) {
        static const char *const positions[]={"",", start",", jump-start",", jump-both",", jump-none"};
        snprintf(text,sizeof text,"steps(%u%s)",(unsigned)e->steps,positions[e->position<5?e->position:0]);
    } else if(e->keyword && e->keyword<5)snprintf(text,sizeof text,"%s",keywords[e->keyword]);
    else snprintf(text,sizeof text,"cubic-bezier(%.7g, %.7g, %.7g, %.7g)",(double)e->x1,(double)e->y1,(double)e->x2,(double)e->y2);
    sb_puts(out,text);return true;
}
static union css_motion_item motion_initial(int list) {
    union css_motion_item v;
    memset(&v, 0, sizeof v);
    if (list == ML_EASING || list == ML_TEASING) v.e = (struct css_easing){.x1=.25f,.y1=.1f,.x2=.25f,.y2=1,.kind=CM_BEZIER,.keyword=CM_EASE};
    else if (list == ML_ITERATIONS) v.f = 1;
    else if (list == ML_TPROPERTY) v.s = "all";
    return v;
}
union css_motion_item css_motion_item(const style_t *st, int list, uint32_t i) {
    const struct css_motion_list *l = st->motion.list[list];
    return l && l->count ? l->v[i % l->count] : motion_initial(list);
}
uint32_t css_motion_count(const style_t *st, int list) {
    const struct css_motion_list *l = st->motion.list[list];
    return l && l->count ? l->count : 1;
}
static bool motion_item_equal(int list, union css_motion_item a, union css_motion_item b) {
    switch (list) {
    case ML_NAME: case ML_TPROPERTY: return a.s == b.s || (a.s && b.s && !strcmp(a.s, b.s));
    case ML_EASING: case ML_TEASING:
        return a.e.kind == b.e.kind && a.e.x1 == b.e.x1 && a.e.y1 == b.e.y1 && a.e.x2 == b.e.x2 && a.e.y2 == b.e.y2 &&
            a.e.steps == b.e.steps && a.e.position == b.e.position && a.e.keyword == b.e.keyword;
    case ML_DURATION: case ML_DELAY: case ML_ITERATIONS: case ML_TDURATION: case ML_TDELAY:
        return a.f == b.f || (isnan(a.f) && isnan(b.f));
    }
    return a.k == b.k;
}
bool css_motion_equal(const struct css_motion *a, const struct css_motion *b) {
    style_t x, y;
    x.motion = *a; y.motion = *b;
    for (int list = 0; list < ML_COUNT; list++) {
        if (a->list[list] == b->list[list]) continue;
        uint32_t count = css_motion_count(&x, list);
        if (count != css_motion_count(&y, list)) return false;
        for (uint32_t i = 0; i < count; i++)
            if (!motion_item_equal(list, css_motion_item(&x, list, i), css_motion_item(&y, list, i))) return false;
    }
    return true;
}
/* one item of an animation-* or transition-* list */
static bool motion_item_parse(int list, const struct kw *kws, const char *s, size_t n, struct cx *cx, union css_motion_item *out) {
    trim(&s, &n);
    if (!n) return false;
    *out = motion_initial(list);
    uint8_t k;
    const char *q = s;
    switch (list) {
    case ML_NAME:
        if (ident_is(s, n, "none")) { out->s = NULL; return true; }
        if (n >= 2 && (s[0] == '"' || s[0] == '\'') && s[n - 1] == s[0]) {
            sbuf b = {0}; css_unescape(&b, s + 1, n - 2);
            out->s = ar_strdup(cx->a, b.n ? sb_cstr(&b) : ""); sb_free(&b);
            return true;
        }
        if (!motion_name(s, n)) return false;
        out->s = ar_strndup(cx->a, s, n);
        return true;
    case ML_DURATION:
        if (ident_is(s, n, "auto")) { out->f = NAN; return true; }
        return motion_time(s, n, &out->f) && out->f >= 0;
    case ML_TDURATION: return motion_time(s, n, &out->f) && out->f >= 0;
    case ML_DELAY: case ML_TDELAY: return motion_time(s, n, &out->f);
    case ML_EASING: case ML_TEASING: return css_easing_parse(s, n, &out->e);
    case ML_ITERATIONS:
        if (ident_is(s, n, "infinite")) { out->f = INFINITY; return true; }
        return parse_number(&q, s + n, &out->f) && q == s + n && isfinite(out->f) && out->f >= 0;
    case ML_TPROPERTY: {
        if (ident_is(s, n, "all")) { out->s = "all"; return true; }
        if (ident_is(s, n, "none") || !motion_name(s, n)) return false;
        /* a supported property keeps its lowercase name; any other identifier is kept as written */
        const struct propdef *p = n > 2 && s[0] == '-' && s[1] == '-' ? NULL : css_prop_lookup(s, n);
        char *copy = ar_strndup(cx->a, s, n);
        if (p) for (size_t i = 0; i < n; i++) copy[i] = (char)lower((unsigned char)copy[i]);
        out->s = copy;
        return true;
    }
    }
    if (!kw_find(kws, s, n, &k)) return false;
    out->k = k;
    return true;
}
/* the top-level comma-separated items of v: their count, and the first max of them */
static size_t motion_items(const char *v, size_t n, const char **items, size_t *lengths, size_t max) {
    size_t count = 0, start = 0;
    int depth = 0; char quote = 0;
    for (size_t i = 0; i <= n; i++) {
        char c = i < n ? v[i] : ',';
        if (quote) { if (c == '\\') i++; else if (c == quote) quote = 0; continue; }
        if (c == '"' || c == '\'') quote = c;
        else if (c == '(') depth++;
        else if (c == ')') depth--;
        else if (c == ',' && depth <= 0) {
            if (count < max) { items[count] = v + start; lengths[count] = i - start; }
            count++; start = i + 1;
        }
    }
    return count;
}
static struct css_motion_list *motion_list_new(struct cx *cx, size_t count) {
    struct css_motion_list *l = ar_alloc(cx->a, sizeof *l + count * sizeof *l->v);
    l->count = (uint32_t)count;
    return l;
}
static bool parse_motion_list(const struct propdef *p, const char *v, size_t n, struct cx *cx, const struct css_motion_list **out) {
    size_t count = motion_items(v, n, NULL, NULL, 0);
    if (!count || count > 1u << 20) return false;
    const char **items = ar_alloc(cx->a, count * sizeof *items);
    size_t *lengths = ar_alloc(cx->a, count * sizeof *lengths);
    motion_items(v, n, items, lengths, count);
    struct css_motion_list *l = motion_list_new(cx, count);
    for (size_t i = 0; i < count; i++) {
        const char *s = items[i]; size_t len = lengths[i];
        trim(&s, &len);
        /* transition-property: none stands alone */
        if (p->flags == ML_TPROPERTY && ident_is(s, len, "none")) {
            if (count != 1) return false;
            l->v[0].s = NULL;
            continue;
        }
        if (!motion_item_parse(p->flags, p->kws, s, len, cx, &l->v[i])) return false;
    }
    *out = l;
    return true;
}
/* <single-animation> (CSS Animations 1 §3.11): the first <time> is the duration, the second the
   delay; a keyword another longhand takes is that longhand's before it can be a name */
static bool animation_single(const char *s, size_t n, struct cx *cx, union css_motion_item *v) {
    const char *t[10]; size_t tl[10];
    int k = split(s, n, t, tl, 10, ' ');
    if (!k || k > 8) return false;
    for (int list = ML_NAME; list <= ML_PLAY; list++) v[list] = motion_initial(list);
    unsigned seen = 0, times = 0;
    for (int i = 0; i < k; i++) {
        union css_motion_item item;
        if (motion_item_parse(ML_TDELAY, NULL, t[i], tl[i], cx, &item)) {
            if (times == 0) { if (item.f < 0) return false; v[ML_DURATION] = item; }
            else if (times == 1) v[ML_DELAY] = item;
            else return false;
            times++; continue;
        }
        if (!(seen & 1) && motion_item_parse(ML_EASING, NULL, t[i], tl[i], cx, &item)) { v[ML_EASING] = item; seen |= 1; continue; }
        if (!(seen & 2) && motion_item_parse(ML_ITERATIONS, NULL, t[i], tl[i], cx, &item)) { v[ML_ITERATIONS] = item; seen |= 2; continue; }
        if (!(seen & 4) && motion_item_parse(ML_DIRECTION, kw_motion_direction, t[i], tl[i], cx, &item)) { v[ML_DIRECTION] = item; seen |= 4; continue; }
        if (!(seen & 8) && motion_item_parse(ML_FILL, kw_motion_fill, t[i], tl[i], cx, &item)) { v[ML_FILL] = item; seen |= 8; continue; }
        if (!(seen & 16) && motion_item_parse(ML_PLAY, kw_motion_play, t[i], tl[i], cx, &item)) { v[ML_PLAY] = item; seen |= 16; continue; }
        if (!(seen & 32) && motion_item_parse(ML_NAME, NULL, t[i], tl[i], cx, &item)) { v[ML_NAME] = item; seen |= 32; continue; }
        return false;
    }
    return true;
}
/* <single-transition> (CSS Transitions 1 §2.5, 2 §2): [none | <single-transition-property>] || <time> ||
   <easing-function> || <time> || <transition-behavior-value> */
static bool transition_single(const char *s, size_t n, struct cx *cx, union css_motion_item *v, bool *none) {
    const char *t[8]; size_t tl[8];
    int k = split(s, n, t, tl, 8, ' ');
    if (!k || k > 5) return false;
    for (int list = ML_TPROPERTY; list <= ML_TBEHAVIOR; list++) v[list] = motion_initial(list);
    unsigned seen = 0, times = 0;
    *none = false;
    for (int i = 0; i < k; i++) {
        union css_motion_item item;
        if (motion_item_parse(ML_TDELAY, NULL, t[i], tl[i], cx, &item)) {
            if (times == 0) { if (item.f < 0) return false; v[ML_TDURATION] = item; }
            else if (times == 1) v[ML_TDELAY] = item;
            else return false;
            times++; continue;
        }
        if (!(seen & 1) && motion_item_parse(ML_TEASING, NULL, t[i], tl[i], cx, &item)) { v[ML_TEASING] = item; seen |= 1; continue; }
        if (!(seen & 2) && motion_item_parse(ML_TBEHAVIOR, kw_transition_behavior, t[i], tl[i], cx, &item)) { v[ML_TBEHAVIOR] = item; seen |= 2; continue; }
        if (!(seen & 4) && ident_is(t[i], tl[i], "none")) { v[ML_TPROPERTY].s = NULL; *none = true; seen |= 4; continue; }
        if (!(seen & 4) && motion_item_parse(ML_TPROPERTY, NULL, t[i], tl[i], cx, &item)) { v[ML_TPROPERTY] = item; seen |= 4; continue; }
        return false;
    }
    return true;
}
/* the animation or transition shorthand: every longhand gets a list of the same length */
static bool motion_shorthand(bool animation, const char *v, size_t n, struct cx *cx, struct css_motion *out) {
    size_t count = motion_items(v, n, NULL, NULL, 0);
    if (!count || count > 1u << 20) return false;
    const char **items = ar_alloc(cx->a, count * sizeof *items);
    size_t *lengths = ar_alloc(cx->a, count * sizeof *lengths);
    motion_items(v, n, items, lengths, count);
    int first = animation ? ML_NAME : ML_TPROPERTY, last = animation ? ML_PLAY : ML_TBEHAVIOR;
    struct css_motion_list *lists[ML_COUNT] = {0};
    for (int list = first; list <= last; list++) lists[list] = motion_list_new(cx, count);
    for (size_t i = 0; i < count; i++) {
        union css_motion_item item[ML_COUNT];
        const char *s = items[i]; size_t len = lengths[i];
        trim(&s, &len);
        bool none = false;
        if (!len || !(animation ? animation_single(s, len, cx, item) : transition_single(s, len, cx, item, &none))) return false;
        if (none && count != 1) return false;
        for (int list = first; list <= last; list++) lists[list]->v[i] = item[list];
    }
    memset(out, 0, sizeof *out);
    for (int list = first; list <= last; list++) out->list[list] = lists[list];
    return true;
}
static bool parse_gline(const char *s, size_t n, struct cx *cx, struct gline *g) {
    memset(g, 0, sizeof *g);
    if (ident_is(s, n, "auto")) return true;
    const char *t[3];
    size_t tl[3];
    int k = split_checked(s, n, t, tl, 3, ' ', cx);
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
    case PT_DISPLAY: dst->legacy_box = src->legacy_box; break;
    case PT_FONT_FAMILY: dst->font_names=src->font_names;dst->named_font=src->named_font;dst->font_choices=src->font_choices;break;
    case PT_VALIGN: dst->vertical_align_px = src->vertical_align_px; break;
    case PT_LIST_TYPE: dst->list_style_string = src->list_style_string; break;
    case PT_BG_IMAGE:
        dst->bg_img = src->bg_img;
        dst->has_grad = src->has_grad;
        dst->gradient = src->gradient;
        dst->grad[0] = src->grad[0];
        dst->grad[1] = src->grad[1];
        break;
    case PT_MASK_IMAGE: dst->mask_img=src->mask_img;break;
    case PT_ZINDEX: dst->z_auto = src->z_auto; break;
    case PT_BG_SIZE: dst->bg_size_kind = src->bg_size_kind; break;
    case PT_MASK_SIZE: dst->mask_size_kind = src->mask_size_kind; break;
    case PT_COLOR_SCHEME: dst->color_scheme_text = src->color_scheme_text; break;
    }
}

static bool parse_translate_value(const char *v,size_t n,struct cx *cx,struct css_translate *out){
    struct css_translate t={.xy={{.kind=LK_LEN},{.kind=LK_LEN}}};
    if(ident_is(v,n,"none")){*out=t;return true;}
    const char *parts[3];size_t lengths[3];int count=split_checked(v,n,parts,lengths,3,' ',cx);
    if(count<1||count>2)return false; /* no invented 3D support */
    for(int i=0;i<count;i++)if(!parse_len(parts[i],lengths[i],cx,&t.xy[i],LF_NEG|LF_LENGTH))return false;
    t.active=true;*out=t;return true;
}
static bool transform_number(const char *s,size_t n,bool percentage,float *out){
    trim(&s,&n);const char *p=s,*e=s+n;float v;
    if(!parse_number(&p,e,&v)||!isfinite(v))return false;
    if(p==e){*out=v;return true;}
    if(percentage&&p+1==e&&*p=='%'){*out=v/100;return true;}
    return false;
}
static bool transform_angle(const char *s,size_t n,float *out){
    trim(&s,&n);const char *p=s,*e=s+n;float v;
    if(!parse_number(&p,e,&v))return false;
    size_t unit=(size_t)(e-p);double r;
    if(!unit){if(v!=0)return false;r=0;} /* unitless zero only */
    else if(ident_is(p,unit,"deg"))r=v*M_PI/180;
    else if(ident_is(p,unit,"rad"))r=v;
    else if(ident_is(p,unit,"grad"))r=v*M_PI/200;
    else if(ident_is(p,unit,"turn"))r=v*2*M_PI;
    else return false;
    *out=(float)r;return isfinite(*out);
}
static struct css_transform transform_identity(uint8_t kind){
    struct css_transform t={.xy={{.kind=LK_LEN},{.kind=LK_LEN}},.kind=kind};
    if(kind==CT_SCALE)t.v[0]=t.v[1]=t.v[2]=1;
    if(kind==CT_ROTATE)t.v[2]=1;
    for(int i=0;i<4;i++)t.m[i*5]=1;
    return t;
}
/* One transform function from its name and comma-separated arguments. */
static bool transform_function(const char *name,size_t nn,const char **a,const size_t *al,int count,struct cx *cx,struct css_transform *t){
#define IS(s) ident_is(name,nn,s)
    if(IS("matrix")||IS("matrix3d")){
        bool three=IS("matrix3d");float v[16];
        if(count!=(three?16:6))return false;
        for(int i=0;i<count;i++)if(!transform_number(a[i],al[i],false,&v[i]))return false;
        *t=transform_identity(CT_MATRIX);
        if(three){for(int r=0;r<4;r++)for(int c=0;c<4;c++)t->m[r*4+c]=v[c*4+r];} /* column-major input */
        else{t->m[0]=v[0];t->m[4]=v[1];t->m[1]=v[2];t->m[5]=v[3];t->m[3]=v[4];t->m[7]=v[5];}
        return true;
    }
    if(IS("translate")||IS("translateX")||IS("translateY")||IS("translateZ")||IS("translate3d")){
        *t=transform_identity(CT_TRANSLATE);
        bool both=IS("translate"),three=IS("translate3d"),y=IS("translateY"),z=IS("translateZ");
        if(three?count!=3:both?(count<1||count>2):count!=1)return false;
        int planar=z?0:three?2:count;
        for(int i=0;i<planar;i++)if(!parse_len(a[i],al[i],cx,&t->xy[y?1:i],LF_NEG|LF_LENGTH))return false;
        if(z||three){
            len_t depth;int at=z?0:2;
            if(memchr(a[at],'%',al[at])||!parse_len(a[at],al[at],cx,&depth,LF_NEG|LF_NOPCT|LF_LENGTH))return false;
            t->v[0]=len_resolve(&depth,0);
        }
        return true;
    }
    if(IS("scale")||IS("scaleX")||IS("scaleY")||IS("scaleZ")||IS("scale3d")){
        *t=transform_identity(CT_SCALE);
        bool both=IS("scale"),three=IS("scale3d");float v[3];
        if(three?count!=3:both?(count<1||count>2):count!=1)return false;
        for(int i=0;i<count;i++)if(!transform_number(a[i],al[i],true,&v[i]))return false;
        if(three){t->v[0]=v[0];t->v[1]=v[1];t->v[2]=v[2];}
        else if(both){t->v[0]=v[0];t->v[1]=count>1?v[1]:v[0];}
        else t->v[IS("scaleX")?0:IS("scaleY")?1:2]=v[0];
        return true;
    }
    if(IS("rotate")||IS("rotateZ")||IS("rotateX")||IS("rotateY")||IS("rotate3d")){
        *t=transform_identity(CT_ROTATE);
        if(IS("rotate3d")){
            if(count!=4)return false;
            for(int i=0;i<3;i++)if(!transform_number(a[i],al[i],false,&t->v[i]))return false;
            return transform_angle(a[3],al[3],&t->v[3]);
        }
        if(count!=1||!transform_angle(a[0],al[0],&t->v[3]))return false;
        if(IS("rotateX")){t->v[0]=1;t->v[2]=0;}
        else if(IS("rotateY")){t->v[1]=1;t->v[2]=0;}
        return true;
    }
    if(IS("skew")||IS("skewX")||IS("skewY")){
        *t=transform_identity(CT_SKEW);
        if(IS("skew")?(count<1||count>2):count!=1)return false;
        for(int i=0;i<count;i++)if(!transform_angle(a[i],al[i],&t->v[IS("skewY")?1:i]))return false;
        return true;
    }
    if(IS("perspective")){
        *t=transform_identity(CT_PERSPECTIVE);
        if(count!=1)return false;
        if(ident_is(a[0],al[0],"none")){t->v[1]=1;return true;}
        len_t depth;if(!parse_len(a[0],al[0],cx,&depth,LF_NOPCT|LF_LENGTH))return false;
        t->v[0]=len_resolve(&depth,0);return true;
    }
#undef IS
    return false;
}
static bool parse_transform_value(const char *v,size_t n,struct cx *cx,const struct css_transform **out){
    if(ident_is(v,n,"none")){*out=NULL;return true;}
    const char *p=v,*end=v+n;struct css_transform *first=NULL,**tail=&first;
    while(p<end){
        while(p<end&&isspace((unsigned char)*p))p++;
        if(p==end)break;
        const char *name=p;while(p<end&&isalnum((unsigned char)*p))p++;
        size_t name_length=(size_t)(p-name);
        if(!name_length||p==end||*p!='(')return false;
        const char *args=++p;unsigned depth=1;
        while(p<end&&depth){if(*p=='(')depth++;else if(*p==')')depth--;if(depth)p++;}
        if(depth)return false;
        const char *parts[17];size_t lengths[17];int count=split_checked(args,(size_t)(p-args),parts,lengths,17,',',cx);
        struct css_transform value;
        if(!transform_function(name,name_length,parts,lengths,count,cx,&value))return false;
        struct css_transform *node=ar_alloc(cx->a,sizeof *node);*node=value;*tail=node;tail=&node->next;p++;
    }
    if(!first)return false;*out=first;return true;
}
bool css_transform_string_matrix(const char *s,size_t n,double m[16],bool *two_d){
    static const char identity2d[]="matrix(1, 0, 0, 1, 0, 0)";
    if(!n){s=identity2d;n=sizeof identity2d-1;}
    if(n>65536)return false;
    /* A comment separates tokens as white space does: "none" followed by a comment is none, a
       comment between a function's name and its parenthesis leaves no function. */
    char *text=malloc(n+1);if(!text)return false;
    size_t len=0;
    for(size_t i=0;i<n;){
        if(s[i]=='/'&&i+1<n&&s[i+1]=='*'){
            const char *close=NULL;for(size_t j=i+2;j+1<n;j++)if(s[j]=='*'&&s[j+1]=='/'){close=s+j;break;}
            text[len++]=' ';i=close?(size_t)(close-s)+2:n;continue;
        }
        text[len++]=s[i++];
    }
    const char *v=text;size_t vn=len;trim(&v,&vn);
    bool ok=false,flat=true;
    arena_t *a=calloc(1,sizeof *a);
    jmp_buf trap;
    if(a){
        a->limit=1u<<20;a->trap=&trap;
        if(!setjmp(trap)){
            style_t style,parent;css_style_init(&parent,NULL);css_style_init(&style,&parent);
            struct cx cx={.s=&style,.parent=&parent,.em=16,.rem=16,.vw=800,.vh=600,.a=a,
                .set=ar_alloc(a,(size_t)css_prop_count()),.supports_probe=true,.absolute_lengths=true};
            const struct css_transform *t=NULL;
            if(parse_transform_value(v,vn,&cx,&t)&&!cx.invalid){
                double r[16];css_transform_list_matrix_3d(t,r);
                for(int c=0;c<4;c++)for(int row=0;row<4;row++)m[c*4+row]=r[row*4+c];
                ok=true;
                /* 2D-ness is by function name, not by value: translate3d(0, 0, 0) is 3D. */
                static const char *const three[]={"matrix3d","translate3d","translateZ","scale3d","scaleZ","rotate3d","rotateX","rotateY","perspective"};
                for(size_t i=0;i<vn;){
                    if(!isalpha((unsigned char)v[i])){i++;continue;}
                    size_t start=i;while(i<vn&&(isalnum((unsigned char)v[i])||v[i]=='-'))i++;
                    if(i<vn&&v[i]=='(')for(size_t k=0;k<sizeof three/sizeof *three;k++)if(ident_is(v+start,i-start,three[k]))flat=false;
                }
            }
        }
        ar_free(a);free(a);
    }
    free(text);
    if(ok)*two_d=flat;
    return ok;
}
/* rotate: none | <angle> | [x | y | z | <number>{3}] && <angle> */
static bool parse_rotate_value(const char *v,size_t n,struct cx *cx,const struct css_transform **out){
    if(ident_is(v,n,"none")){*out=NULL;return true;}
    const char *t[5];size_t tl[5];int k=split_checked(v,n,t,tl,5,' ',cx);
    struct css_transform value=transform_identity(CT_ROTATE);
    if(k==1){if(!transform_angle(t[0],tl[0],&value.v[3]))return false;}
    else if(k==2||k==4){
        int angle=transform_angle(t[0],tl[0],&value.v[3])?0:k-1;
        if(angle&&!transform_angle(t[angle],tl[angle],&value.v[3]))return false;
        int axis=angle?0:1;
        if(k==2){
            value.v[2]=0;
            if(ident_is(t[axis],tl[axis],"x"))value.v[0]=1;
            else if(ident_is(t[axis],tl[axis],"y"))value.v[1]=1;
            else if(ident_is(t[axis],tl[axis],"z"))value.v[2]=1;
            else return false;
        }else for(int i=0;i<3;i++)if(!transform_number(t[axis+i],tl[axis+i],false,&value.v[i]))return false;
    }else return false;
    struct css_transform *node=ar_alloc(cx->a,sizeof *node);*node=value;*out=node;return true;
}
/* scale: none | [<number> | <percentage>]{1,3} */
static bool parse_scale_value(const char *v,size_t n,struct cx *cx,const struct css_transform **out){
    if(ident_is(v,n,"none")){*out=NULL;return true;}
    const char *t[4];size_t tl[4];int k=split_checked(v,n,t,tl,4,' ',cx);
    if(k<1||k>3)return false;
    struct css_transform value=transform_identity(CT_SCALE);
    for(int i=0;i<k;i++)if(!transform_number(t[i],tl[i],true,&value.v[i]))return false;
    if(k==1)value.v[1]=value.v[0];
    struct css_transform *node=ar_alloc(cx->a,sizeof *node);*node=value;*out=node;return true;
}
/* transform-origin: one or two position components plus an ignored z length. */
static bool parse_transform_origin(const char *v,size_t n,struct cx *cx,len_t out[2]){
    const char *t[4];size_t tl[4];int k=split_checked(v,n,t,tl,4,' ',cx);
    if(k<1||k>3)return false;
    len_t r[2]={{.kind=LK_LEN,.pct=50},{.kind=LK_LEN,.pct=50}};
    int planar=k>2?2:k;
    bool vertical_first=ident_is(t[0],tl[0],"top")||ident_is(t[0],tl[0],"bottom")||
        (planar==2&&(ident_is(t[1],tl[1],"left")||ident_is(t[1],tl[1],"right")));
    for(int i=0;i<planar;i++){
        int axis=vertical_first?!i:i;len_t *l=&r[axis];
        if(ident_is(t[i],tl[i],"center")){*l=(len_t){.kind=LK_LEN,.pct=50};continue;}
        if(ident_is(t[i],tl[i],"left")||ident_is(t[i],tl[i],"top")||ident_is(t[i],tl[i],"right")||ident_is(t[i],tl[i],"bottom")){
            bool x=ident_is(t[i],tl[i],"left")||ident_is(t[i],tl[i],"right");
            if(x!=(axis==0))return false;
            *l=(len_t){.kind=LK_LEN,.pct=ident_is(t[i],tl[i],"left")||ident_is(t[i],tl[i],"top")?0:100};continue;
        }
        if(!parse_len(t[i],tl[i],cx,l,LF_NEG|LF_LENGTH))return false;
    }
    if(k==3){len_t depth;if(!parse_len(t[2],tl[2],cx,&depth,LF_NEG|LF_NOPCT|LF_LENGTH))return false;}
    out[0]=r[0];out[1]=r[1];return true;
}

static bool apply_long(const struct propdef *p, const char *v, size_t n, struct cx *cx) {
    style_t *s = cx->s;
    char *field = (char *)s + p->off;
    const style_t *par = cx->parent ? cx->parent : &initial;
    bool colour = p->type == PT_COLOR || p->type == PT_SVG_PAINT;
    const style_t *from = ident_is(v, n, "inherit") ? par : ident_is(v, n, "initial") ? &initial :
        ident_is(v, n, "unset") || ident_is(v, n, "revert") || ident_is(v, n, "revert-layer") ?
        p->inherited ? par : &initial : NULL;
    /* the initial colour is CanvasText, which follows color-scheme */
    if (from == &initial && colour && color_slot(p) == CSS_SLOT_COLOR) return css_color_apply("canvastext", 10, cx, CSS_SLOT_COLOR);
    if (from) {
        copy_prop(p, s, from);
        if (colour && !cx->supports_probe) css_color_slot_copy(s, from, color_slot(p), cx->a);
        if (from == par && !p->inherited && ident_is(v, n, "inherit")) s->explicit_inherit = true;
        return true;
    }
    switch (p->type) {
    case PT_TRANSFORM:return parse_transform_value(v,n,cx,&s->transform);
    case PT_TRANSLATE:return parse_translate_value(v,n,cx,&s->translate);
    case PT_ROTATE:return parse_rotate_value(v,n,cx,&s->rotate);
    case PT_SCALE:return parse_scale_value(v,n,cx,&s->scale);
    case PT_TRANSFORM_ORIGIN:return parse_transform_origin(v,n,cx,s->transform_origin);
    case PT_MOTION_LIST: return parse_motion_list(p, v, n, cx, (const struct css_motion_list **)field);
    case PT_CONTAINER_NAMES:
        if (ident_is(v, n, "none")) { s->container_names = NULL; return true; }
        if (!css_container_names_valid(v,n)) return false;
        s->container_names = ar_strndup(cx->a,v,n);
        return true;
    case PT_KW: {
        uint8_t k;
        if (cx->supports_probe && ((p->kws == kw_textwrap && !ident_is(v,n,"wrap") && !ident_is(v,n,"nowrap")) ||
            (p->kws == kw_ttrans && ident_is(v,n,"full-width")))) return false;
        if (!kw_find(p->kws, v, n, &k)) return false;
        *(uint8_t *)field = k;
        return true;
    }
    case PT_OVERFLOW: {
        uint8_t k;
        if (!kw_find(p->kws, v, n, &k)) return false;
        *(uint8_t *)field = k;
        return true;
    }
    case PT_DISPLAY:
        if (!parse_display(v, n, (uint8_t *)field, cx)) return false;
        s->legacy_box = ident_is(v, n, "-webkit-box") || ident_is(v, n, "-webkit-inline-box");
        return true;
    case PT_LINE_CLAMP: {
        if (ident_is(v, n, "none")) { s->line_clamp = 0; return true; }
        size_t i = n && v[0] == '+' ? 1 : 0;
        uint32_t value = 0;
        if (i == n) return false;
        for (; i < n; i++) {
            if (v[i] < '0' || v[i] > '9') return false;
            unsigned digit = (unsigned)(v[i] - '0');
            if (value > (UINT32_MAX - digit) / 10) return false;
            value = value * 10 + digit;
        }
        if (!value) return false;
        s->line_clamp = value;
        return true;
    }
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
        int count = split_checked(v, n, t, tl, 2, ' ', cx);
        if (count < 1 || (cx->supports_probe && count != 1)) return false; /* only one stored axis */
        len_t l;
        if (!parse_len(t[0], tl[0], cx, &l, p->flags)) return false;
        *(float *)field = len_resolve(&l, 0);
        return true;
    }
    case PT_RADIUS: {
        const char *t[3];size_t tl[3];int count=split_checked(v,n,t,tl,3,' ',cx);
        if(count<1||count>2||t[count-1]+tl[count-1]!=v+n)return false;
        struct css_corner_radius parsed;
        if(!parse_len(t[0],tl[0],cx,&parsed.axis[0],LF_LENGTH))return false;
        parsed.axis[1]=parsed.axis[0];
        if(count==2&&!parse_len(t[1],tl[1],cx,&parsed.axis[1],LF_LENGTH))return false;
        *(struct css_corner_radius *)field=parsed;
        return true;
    }
    case PT_COLOR: return css_color_apply(v, n, cx, color_slot(p));
    case PT_COLOR_SCHEME: {
        /* CSS Color Adjust 1, 2.1: normal | [ light | dark | <custom-ident> ]+ && only? */
        if (ident_is(v, n, "normal")) {
            s->color_scheme = 0;
            s->color_scheme_text = NULL;
        } else {
            uint8_t bits = 0;
            size_t names = 0;
            char *text = ar_alloc(cx->a, n + 6), *t = text;
            for (const char *q = v, *e = v + n; q < e;) {
                while (q < e && is_space((unsigned char)*q)) q++;
                const char *w = q;
                while (q < e && !is_space((unsigned char)*q)) q++;
                size_t wn = (size_t)(q - w);
                if (!wn) break;
                /* only once, first or last */
                if (ident_is(w, wn, "only")) {
                    if ((bits & CSS_SCHEME_ONLY) || (names && q != e)) return false;
                    bits |= CSS_SCHEME_ONLY;
                    continue;
                }
                if (ident_is(w, wn, "normal") || !motion_name(w, wn)) return false; /* a <custom-ident> */
                bool light = ident_is(w, wn, "light"), dark = ident_is(w, wn, "dark");
                bits |= light ? CSS_SCHEME_LIGHT : dark ? CSS_SCHEME_DARK : 0;
                if (names++) *t++ = ' ';
                if (light || dark) { memcpy(t, light ? "light" : "dark", light ? 5 : 4); t += light ? 5 : 4; }
                else { memcpy(t, w, wn); t += wn; }
            }
            if (!names) return false;
            if (bits & CSS_SCHEME_ONLY) { memcpy(t, " only", 5); t += 5; }
            *t = 0;
            s->color_scheme = bits;
            s->color_scheme_text = text;
        }
        /* the root's initial colour follows its scheme until color is declared */
        if (!cx->parent && !cx->supports_probe && !cx->set[css_prop_index(find_prop("color", 5))])
            css_color_apply("canvastext", 10, cx, CSS_SLOT_COLOR);
        return true;
    }
    case PT_SVG_WIDTH: {
        len_t width;
        if (!parse_len(v, n, cx, &width, 0) || width.kind != LK_LEN || (width.pct && width.px)) return false;
        /* Pure percentages must reach the SVG viewport, not the HTML parent.
           The codec cannot yet evaluate mixed length/percentage expressions. */
        *(len_t *)field = width;
        return true;
    }
    case PT_SVG_PAINT: {
        struct svg_paint paint = {0};
        const struct css_color_value *exact = NULL;
        if (ident_is(v, n, "none")) paint.kind = SVG_PAINT_NONE;
        else if (css_color_value_compute(v, n, cx, NULL, &paint.color, &exact)) paint.kind = SVG_PAINT_COLOR;
        else {
            /* The rasterizer can resolve local SVG gradients, not external
               paint servers or context paints. Do not claim those in supports(). */
            const char *ref = n >= 5 && strn_ieq(v, "url(", 4) && v[n-1] == ')' ? parse_url(v, n, cx->a) : NULL;
            if (!ref) return false;
            if (ref[0] != '#') {
                const char *fragment = strchr(ref, '#');
                web_doc *doc = cx->node ? cx->node->owner : NULL;
                const char *document_url = doc ? doc->url : NULL;
                size_t prefix = fragment ? (size_t)(fragment - ref) : 0;
                size_t document_prefix = document_url ? strcspn(document_url, "#") : 0;
                if (!fragment || !document_url || prefix != document_prefix || memcmp(ref, document_url, prefix)) return false;
                ref = fragment;
            }
            if (!ref[1]) return false;
            for (const char *q = ref + 1; *q; q++)
                if (is_space((unsigned char)*q) || strchr("()\"';\\", *q)) return false;
            paint.kind = SVG_PAINT_REF; paint.ref = ref;
        }
        *(struct svg_paint *)field = paint;
        if (!cx->supports_probe) css_color_slot_set(s, color_slot(p), exact, cx->a);
        return true;
    }
    case PT_NUM: {
        float x;
        if (!parse_number_value(v, n, cx, &x) || x < 0) return false;
        *(float *)field = x;
        return true;
    }
    case PT_FILTER: return parse_filter(v, n, cx, &s->filter);
    case PT_CLIP_PATH: return parse_clip_path(v, n, cx, &s->clip_path);
    case PT_CLIP: return parse_clip_rect(v, n, cx, &s->clip);
    case PT_BOX_SHADOW: return parse_box_shadows(v, n, cx, &s->box_shadow, NULL);
    case PT_OPACITY: {
        const char *q = v;
        float x;
        if (parse_number(&q, v + n, &x) && q + 1 == v + n && *q == '%') x /= 100;
        else if (!parse_number_value(v, n, cx, &x)) return false;
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
        bool math = !parse_number(&q, v + n, &x) || q != v + n;
        if (math && !parse_number_value(v, n, cx, &x)) return false;
        if (math) x = roundf(x); /* a math function resolves to the nearest integer */
        if (cx->supports_probe && (x != floorf(x) || x > 2147483520.0f || x < -2147483648.0f)) return false;
        *(int *)field = (int)x;
        if (p->type == PT_ZINDEX) s->z_auto = false;
        return true;
    }
    case PT_FONT_SIZE: return parse_font_size(v, n, cx, &s->font_size);
    case PT_FONT_WEIGHT: return parse_font_weight(v, n, cx, &s->font_weight);
    case PT_FONT_FAMILY:
        if(!choose_family(v,n,&s->font_family,cx))return false;
        s->font_names=ar_strndup(cx->a,v,n);s->named_font=NULL;s->font_choices=NULL;return true;
    case PT_LINE_HEIGHT: {
        len_t l;
        if (ident_is(v, n, "normal")) {
            s->line_height.kind = LK_NORMAL;
            return true;
        }
        float x;
        if (parse_number_value(v, n, cx, &x)) {
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
        int k = split_checked(v, n, t, tl, 4, ' ', cx);
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
    if (!p) { if (cx->supports_probe) cx->invalid = true; return false; }
    int i = css_prop_index(p);
    if (cx->set[i]) return true;
    if (cx->font_pass && !early(p)) return true;
    if (!apply_long(p, v, n, cx)) { if (cx->supports_probe) cx->invalid = true; return false; }
    cx->set[i] = 1;
    return true;
}

static const char *const side_names[4] = {"top", "right", "bottom", "left"};

/* margin: 1-4 values for the four sides */
static bool set_box(const char *fmt, const char *v, size_t n, struct cx *cx, const int *sides, int nsides) {
    const char *t[4];
    size_t tl[4];
    int k = split_checked(v, n, t, tl, 4, ' ', cx);
    if (k < 1 || (cx->supports_probe && nsides == 2 && k > 2)) return false;
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
    int k = split_checked(v, n, t, tl, 3, ' ', cx);
    const char *w = "medium", *st = "none", *c = "currentcolor";
    size_t wl = 6, sl = 4, cl = 12;
    uint8_t dummy;
    uint32_t col;
    unsigned seen = 0;
    for (int i = 0; i < k; i++) {
        unsigned bit;
        if (kw_find(kw_bstyle, t[i], tl[i], &dummy)) { bit = 1; st = t[i]; sl = tl[i]; }
        else if (is_bwidth(t[i], tl[i], cx)) { bit = 2; w = t[i]; wl = tl[i]; }
        else if (css_color_checked(t[i], tl[i], &col, cx)) { bit = 4; c = t[i]; cl = tl[i]; }
        else return false;
        if (cx->supports_probe && (seen & bit)) return false;
        seen |= bit;
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
    int k = split_checked(v, n, t, tl, 16, ' ', cx);
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
    /* The shorthand takes <'font-family'>, the property's own grammar: a
       CSS-wide keyword there is not a family list (CSS Fonts 4 font). */
    const char *family = t[fam];
    size_t family_n = (size_t)(v + n - t[fam]);
    if (ident_is(family, family_n, "inherit") || ident_is(family, family_n, "initial") || ident_is(family, family_n, "unset") ||
        ident_is(family, family_n, "revert") || ident_is(family, family_n, "revert-layer") || ident_is(family, family_n, "default")) {
        if (cx->supports_probe) cx->invalid = true;
        return false;
    }
    set_long("font-size", sz, szl, cx);
    if (cx->font_pass) return true;
    set_long("font-style", style, stl, cx);
    set_long("font-weight", weight, wl, cx);
    set_long("line-height", lh, lhl, cx);
    set_long("font-family", family, family_n, cx);
    return true;
}

static bool set_background(const char *v, size_t n, struct cx *cx) {
    const char *layers[8];
    size_t ll[8];
    int k = split_checked(v, n, layers, ll, 8, ',', cx);
    if (k < 1 || (cx->supports_probe && k != 1)) return false;
    /* the colour may only be in the last layer */
    const char *t[12];
    size_t tl[12];
    int m = split_checked(layers[k - 1], ll[k - 1], t, tl, 12, ' ', cx);
    const char *col = "transparent";
    size_t cl = 11;
    uint32_t c;
    for (int i = 0; i < m; i++)
        if (css_color_checked(t[i], tl[i], &c, cx)) col = t[i], cl = tl[i];
    set_long("background-color", col, cl, cx);
    /* images: any url() or gradient in any layer */
    bool img = false;
    for (int L = 0; L < k && !img; L++) {
        m = split_checked(layers[L], ll[L], t, tl, 12, ' ', cx);
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
    m = split_checked(layers[0], ll[0], t, tl, 12, ' ', cx);
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
        if (strn_ieq(tok, "url(", 4) || strstr(tok, "gradient(") ||
            (!cx->supports_probe && memchr(tok, '(', tn)) || css_color_checked(tok, tn, &c, cx)) continue;
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
    const char *t[4];
    size_t tl[4];
    int k = split_checked(v, n, t, tl, 4, ' ', cx);
    if (k < 1 || k > 3) return false;
    if (k == 1 && ident_is(t[0], tl[0], "none"))
        return set_long("flex-grow", "0", 1, cx), set_long("flex-shrink", "0", 1, cx),
               set_long("flex-basis", "auto", 4, cx);
    if (k == 1 && ident_is(t[0], tl[0], "auto"))
        return set_long("flex-grow", "1", 1, cx), set_long("flex-shrink", "1", 1, cx),
               set_long("flex-basis", "auto", 4, cx);
    const char *g = "1", *sh = "1", *b = "0%";
    size_t gl = 1, shl = 1, bl = 2;
    int nums = 0, first_number = -1;
    bool basis = false;
    for (int i = 0; i < k; i++) {
        const char *q = t[i];
        float x;
        if (parse_number(&q, t[i] + tl[i], &x) && q == t[i] + tl[i]) {
            if (!isfinite(x) || x < 0) return false;
            /* A unitless zero is a factor until both factors are supplied.
               The third zero is then the definite zero-length basis, not an
               illegal third factor (Flexbox 7.1). */
            if (nums == 2) {
                if (basis || x != 0) return false;
                basis = true; b = t[i]; bl = tl[i];
            } else {
                if (nums == 0) { g = t[i]; gl = tl[i]; first_number = i; }
                else {
                    if (i != first_number + 1) return false;
                    sh = t[i]; shl = tl[i];
                }
                nums++;
            }
        } else {
            if (basis) return false;
            basis = true; b = t[i]; bl = tl[i];
        }
    }
    if (!nums) g = "1", gl = 1;
    /* Validate before publishing any longhand: an invalid basis must not
       overwrite the grow/shrink of an earlier valid declaration. */
    len_t parsed;
    if (!parse_len(b, bl, cx, &parsed, LF_AUTO | LF_LENGTH)) return false;
    return set_long("flex-grow", g, gl, cx) && set_long("flex-shrink", sh, shl, cx) &&
           set_long("flex-basis", b, bl, cx);
}

static bool apply_short(const struct propdef *p, const char *v, size_t n, struct cx *cx) {
    static const int all[4] = {0, 1, 2, 3}, lr[2] = {3, 1}, tb[2] = {0, 2};
    bool global = ident_is(v, n, "inherit") || ident_is(v, n, "initial") || ident_is(v, n, "unset") ||
                  ident_is(v, n, "revert") || ident_is(v, n, "revert-layer");
    switch (p->sh) {
    case SH_ALL: {
        if(!global)return false;
        for(int i=0;i<NPROPS;i++)if(props[i].type!=PT_SHORT && strcmp(props[i].name,"direction") &&
            strcmp(props[i].name,"unicode-bidi"))set_long(props[i].name,v,n,cx);
        return true;
    }
    case SH_ANIMATION: case SH_TRANSITION: {
        /* the longhands each shorthand sets; animation-composition is reset only */
        static const char *const animation[] = {"animation-name", "animation-duration", "animation-delay",
            "animation-timing-function", "animation-iteration-count", "animation-direction",
            "animation-fill-mode", "animation-play-state", "animation-composition"};
        static const char *const transition[] = {"transition-property", "transition-duration", "transition-delay",
            "transition-timing-function", "transition-behavior"};
        bool is_animation = p->sh == SH_ANIMATION;
        const char *const *names = is_animation ? animation : transition;
        unsigned count = is_animation ? 9 : 5;
        if (global) { for (unsigned i = 0; i < count; i++) set_long(names[i], v, n, cx); return true; }
        struct css_motion motion;
        if (!motion_shorthand(is_animation, v, n, cx, &motion)) return false;
        /* The shorthand resets all longhands, but cannot override a higher
           priority longhand already selected by this cascade. */
        style_t temp = *cx->s;
        temp.motion = motion;
        for (unsigned i = 0; i < count; i++) {
            const struct propdef *q = find_prop(names[i], strlen(names[i]));
            int at = css_prop_index(q);
            if (!cx->set[at]) { copy_prop(q, cx->s, &temp); cx->set[at] = 1; }
        }
        return true;
    }
    case SH_CONTAINER: {
        if (global) {
            set_long("container-name",v,n,cx);
            set_long("container-type",v,n,cx);
            return true;
        }
        const char *slash=NULL,*names=v;
        for(size_t i=0;i<n;i++) {
            if(v[i]=='\\' && i+1<n) {
                i++;unsigned count=0;
                while(i<n && isxdigit((unsigned char)v[i]) && count<6){i++;count++;}
                if(count && i<n && !is_space((unsigned char)v[i]))i--;
                continue;
            }
            if(v[i]=='/'){slash=v+i;break;}
        }
        size_t nn=slash?(size_t)(slash-v):n; trim(&names,&nn);
        const char *type=slash?slash+1:"normal";
        size_t tn=slash?(size_t)(v+n-type):6; trim(&type,&tn);
        type=css_value_canonical(cx->a,type,tn,&tn);
        uint8_t dummy;
        if ((!ident_is(names,nn,"none") && !css_container_names_valid(names,nn)) ||
            !kw_find(kw_container_type,type,tn,&dummy)) return false;
        set_long("container-name",names,nn,cx);
        set_long("container-type",type,tn,cx);
        return true;
    }
    case SH_BORDER_RADIUS: {
        static const char *const names[]={"border-top-left-radius","border-top-right-radius","border-bottom-right-radius","border-bottom-left-radius"};
        if(global){bool ok=true;for(int i=0;i<4;i++)ok&=set_long(names[i],v,n,cx);return ok;}
        len_t lengths[2][4];
        if(!parse_radius_list(v,n,cx,lengths))return false;
        for(int i=0;i<4;i++) {
            const struct propdef *corner=css_prop_lookup(names[i],strlen(names[i]));int index=css_prop_index(corner);
            if(cx->set[index])continue;
            cx->s->corner_radius[i].axis[0]=lengths[0][i];cx->s->corner_radius[i].axis[1]=lengths[1][i];cx->set[index]=1;
        }
        return true;
    }
    case SH_MARGIN: return set_box("margin-%s", v, n, cx, all, 4);
    case SH_PADDING: return set_box("padding-%s", v, n, cx, all, 4);
    case SH_INSET: {
        static const char *const nm[4] = {"top", "right", "bottom", "left"};
        const char *t[4];
        size_t tl[4];
        int k = split_checked(v, n, t, tl, 4, ' ', cx);
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
        int k = split_checked(v, n, t, tl, 2, ' ', cx);
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
    case SH_OUTLINE: {
        if (global) {
            set_long("outline-width", v, n, cx);
            set_long("outline-style", v, n, cx);
            set_long("outline-color", v, n, cx);
            return true;
        }
        const char *t[3];
        size_t tl[3];
        int k = split_checked(v, n, t, tl, 3, ' ', cx);
        const char *w = "medium", *st = "none", *c = "currentcolor";
        size_t wl = 6, sl = 4, cl = 12;
        uint8_t style;
        uint32_t col;
        unsigned seen = 0;
        for (int i = 0; i < k; i++) {
            unsigned bit;
            if (kw_find(kw_ostyle, t[i], tl[i], &style)) { bit = 1; st = t[i]; sl = tl[i]; }
            else if (is_bwidth(t[i], tl[i], cx)) { bit = 2; w = t[i]; wl = tl[i]; }
            else if (css_color_checked(t[i], tl[i], &col, cx)) { bit = 4; c = t[i]; cl = tl[i]; }
            else return false;
            if (seen & bit) return false;
            seen |= bit;
        }
        if (k < 1) return false;
        set_long("outline-width", w, wl, cx);
        set_long("outline-style", st, sl, cx);
        set_long("outline-color", c, cl, cx);
        return true;
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
        int k = split_checked(v, n, t, tl, 3, ' ', cx);
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
        int k = split_checked(v, n, t, tl, 2, ' ', cx);
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
        int k = split_checked(v, n, t, tl, 2, ' ', cx);
        if (k < 1) return false;
        set_long("row-gap", t[0], tl[0], cx);
        set_long("column-gap", t[k - 1], tl[k - 1], cx);
        return true;
    }
    case SH_OVERFLOW: {
        const char *t[2];
        size_t tl[2];
        int k = split_checked(v, n, t, tl, 2, ' ', cx);
        if (k < 1) return false;
        set_long("overflow-x", t[0], tl[0], cx);
        set_long("overflow-y", t[k - 1], tl[k - 1], cx);
        return true;
    }
    case SH_TEXT_DECORATION: {
        if (global) return set_long("text-decoration-line", v, n, cx);
        const char *t[6];
        size_t tl[6];
        int k = split_checked(v, n, t, tl, 6, ' ', cx);
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
            } else if (cx->supports_probe && !ident_is(t[i],tl[i],"none")) return false; /* other decoration fields aren't painted */
        return set_long("text-decoration-line", line, ln, cx);
    }
    case SH_PLACE_SELF: case SH_PLACE_CONTENT: {
        const char *t[2];
        size_t tl[2];
        int k = split_checked(v, n, t, tl, 2, ' ', cx);
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
        int k = split_checked(v, n, t, tl, 4, '/', cx);
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
        if (split_checked(v, n, t, tl, 2, '/', cx) != 2) return false;
        /* grid: auto-flow forms */
        if (p->sh == SH_GRID) {
            for (int side = 0; side < 2; side++) {
                const char *w[4];
                size_t wl[4];
                int k = split_checked(t[side], tl[side], w, wl, 4, ' ', cx);
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
        int k = split_checked(t[0], tl[0], w, wl, 64, ' ', cx);
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
        int k = split_checked(v, n, t, tl, 2, ' ', cx);
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
    if (cx->font_pass && !early(p)) return true;
    if (!apply_long(p, v, n, cx)) return false;
    cx->set[i] = 1;
    return true;
}

static bool value_parse(const struct propdef *p, const char *v, size_t n, bool probe) {
    if (!p || !v || n == SIZE_MAX) return false;
    arena_t *a = calloc(1, sizeof *a);
    if (!a) return false;
    if(probe)a->limit = 1u << 20;
    jmp_buf trap;
    a->trap = &trap;
    if (setjmp(trap)) { ar_free(a); free(a); return false; }
    style_t style, parent;
    css_style_init(&parent, NULL);
    css_style_init(&style, &parent);
    uint8_t *set = ar_alloc(a, (size_t)css_prop_count());
    struct cx cx = {.s=&style, .parent=&parent, .em=16, .rem=16, .vw=800, .vh=600,
                    .a=a, .set=set, .supports_probe=probe};
    bool result = css_apply(p, v, n, &cx) && !cx.invalid;
    ar_free(a);
    free(a);
    return result;
}
bool css_value_supported(const struct propdef *p,const char *v,size_t n) {
    return n <= 65536 && value_parse(p,v,n,true);
}
bool css_value_valid(const struct propdef *p,const char *v,size_t n) {
    return value_parse(p,v,n,false);
}

void css_style_init(style_t *s, const style_t *parent) {
    props_init();
    if (!parent) {
        *s = initial;
        return;
    }
    *s = *parent;
    s->border_radius=0; /* legacy native-only radius is not inherited */
    for (int i = 0; i < NPROPS; i++)
        if (props[i].type != PT_SHORT && !props[i].inherited) copy_prop(&props[i], s, &initial);
    s->color_own = NULL;
    s->before = s->after = s->backdrop = s->placeholder = NULL;
    s->before_generated = s->after_generated = s->explicit_inherit = false;
}

/* The inverse split of css_style_init: what it copies comes from from, what
   it resets stays s's own. */
void css_style_adopt_inherited(style_t *s, const style_t *from) {
    style_t own = *s;
    css_style_init(s, from);
    for (int i = 0; i < NPROPS; i++)
        if (props[i].type != PT_SHORT && !props[i].inherited) copy_prop(&props[i], s, &own);
    s->border_radius = own.border_radius;
    s->color_own = own.color_own;
    s->before = own.before; s->after = own.after; s->backdrop = own.backdrop; s->placeholder = own.placeholder;
    s->before_generated = own.before_generated; s->after_generated = own.after_generated;
    s->explicit_inherit = own.explicit_inherit;
}

void css_style_finish(style_t *s, const style_t *parent, bool root) {
    if (s->color == COLOR_CURRENT) s->color = parent ? parent->color : 0xFF000000u;
    css_color_finish(s);
    if (s->bg_color == COLOR_CURRENT) s->bg_color = s->color;
    for (struct css_box_shadow *shadow = (struct css_box_shadow *)s->box_shadow; shadow; shadow = shadow->next)
        if (shadow->color == COLOR_CURRENT)
            shadow->color = shadow->color_value ? css_color_value_argb(shadow->color_value, s->color) : s->color;
    for (int i = 0; i < 4; i++) {
        if (s->border_color[i] == COLOR_CURRENT) s->border_color[i] = s->color;
        if (s->border_style[i] == BS_NONE || s->border_style[i] == BS_HIDDEN) s->border_width[i] = 0;
    }
    if (s->outline_color == COLOR_CURRENT) s->outline_color = s->color;
    if (s->outline_style == BS_NONE) s->outline_width = 0;
    if (s->has_grad) {
        const struct gradient *g = s->gradient;
        for (int i = 0; i < 2; i++) {
            const struct css_color_value *exact = g && g->n ? g->col_value[i ? g->n - 1 : 0] : NULL;
            if (s->grad[i] == COLOR_CURRENT) s->grad[i] = exact ? css_color_value_argb(exact, s->color) : s->color;
        }
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
    /* Legacy vertical clamping is a block formatting context, not a flex
       row. Retain the authored display flag for CSS-wide copies/activation. */
    if (s->legacy_box && s->box_orient == BO_VERTICAL && s->line_clamp && s->display == D_BLOCK)
        s->display = D_FLOW_ROOT;
}

/* ---------------------------------------------------------------- the user agent stylesheet */
const char *css_ua_sheet(void) {
    /* Slots remain real DOM nodes but have no wrapper box by default. */
    return "slot { display: contents; }\n"
           "dialog { position: absolute; left: 0; right: 0; width: auto; height: auto; margin: auto; "
           "border: 3px solid black; padding: 1em; background-color: white; color: black; }\n"
           "dialog:modal { position: fixed; inset: 0; max-width: calc(100% - 2em - 6px); "
           "max-height: calc(100% - 2em - 6px); overflow: auto; }\n"
           "dialog::backdrop { display: block; position: fixed; inset: 0; background-color: rgba(0,0,0,0.1); }\n"
           /* The rest of the fullscreen UA rule is forced (web_presentation_style); this part is not. */
           ":not(:root):fullscreen { object-fit: contain; }\n"
           "html, address, blockquote, body, center, dialog, div, figure, figcaption, footer, form, header, hr, "
           "legend, listing, main, p, plaintext, pre, search, xmp, article, aside, h1, h2, h3, h4, h5, h6, hgroup, "
           "nav, section, dir, dd, dl, dt, menu, ol, ul, details, summary, fieldset, optgroup { display: block; }\n"
           "head, script, style, title, meta, link, base, template, datalist, param, noembed, noframes, area, map, "
           "track, source, rp, [hidden]:not([hidden=until-found i]), input[type=hidden], dialog:not([open]) "
           "{ display: none; }\n"
           "[hidden=until-found i] { content-visibility: hidden; }\n"
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
           "img, video, canvas, iframe, embed, svg, input, select, textarea, button, meter, progress "
           "{ display: inline-block; }\n"
           "iframe { border: 2px inset; }\n"
           "frameset, frame { display:block; margin:0; padding:0; border:0; }\n"
           /* HTML canvas starts with transparent black pixels. Giving replaced
              elements an invented gray CSS background made an empty overlay
              canvas hide the real video underneath it. */
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
           "summary { display: block; }\n"
           "details > summary:first-of-type { display: list-item; list-style: disclosure-closed inside; }\n"
           "details[open] > summary:first-of-type { list-style-type: disclosure-open; }\n"
           "ruby { display: ruby; } rt { font-size: 50%; }\n"
           "nobr { white-space: nowrap; }\n"
           "wbr { display: inline; }\n"
           "marquee { display: inline-block; width: 100%; overflow: hidden; white-space: nowrap; }\n"
           "math { display: inline; }\n"
           "svg:not(:root) { overflow: hidden; }\n"
           "@media (scripting:enabled) { noscript { display: none !important; } }\n";
}

/* ---------------------------------------------------------------- animation values
   The one place computed values combine (CSS Values 4 §3 "Combining values", Web Animations 1
   §5.3 "Animation types"). CSS Transitions, CSS Animations and script animations all reach it
   through animation.c. The property table gives each longhand its animation type
   (propdef.anim, else its value type's default); below, each value type has its interpolation,
   addition and accumulation. Values come in as computed values in scratch copies of the
   element's base style (css_anim_parse) and leave as computed-value text, which the cascade
   applies at the animation or transition origin so that em, currentcolor and inheritance
   follow the animated values. */
#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif
int css_prop_animation(const struct propdef *p) {
    if (!p || p->type == PT_SHORT) return CSS_ANIM_NONE;
    switch (p->anim) {
    case AN_NONE: return CSS_ANIM_NONE;
    case AN_DISCRETE: return CSS_ANIM_DISCRETE;
    case AN_VISIBILITY: return CSS_ANIM_VISIBILITY;
    case AN_DISPLAY: return CSS_ANIM_DISPLAY;
    }
    switch (p->type) {
    case PT_MOTION_LIST: case PT_CONTAINER_NAMES: return CSS_ANIM_NONE;
    case PT_DISPLAY: return CSS_ANIM_DISPLAY;
    case PT_KW: case PT_OVERFLOW: case PT_FONT_FAMILY: case PT_CONTENT: case PT_LIST_TYPE: case PT_BG_IMAGE:
    case PT_TEXT_DECO: case PT_GTEMPLATE: case PT_GAREAS: case PT_GLINE: case PT_MASK_IMAGE: case PT_LINE_CLAMP:
    case PT_CLIP_PATH: return CSS_ANIM_DISCRETE;
    }
    return CSS_ANIM_VALUE;
}
/* A shorthand's longhands are what its own parser sets for "initial" (the same expansion
   revert uses), computed once per shorthand. */
static uint8_t longhand_sets[NPROPS][NPROPS];
static uint8_t longhand_ready[NPROPS];
void css_prop_longhands(const struct propdef *p, uint8_t *set) {
    memset(set, 0, NPROPS);
    if (!p) return;
    int i = css_prop_index(p);
    if (p->type != PT_SHORT) { set[i] = 1; return; }
    if (!__atomic_load_n(&longhand_ready[i], __ATOMIC_ACQUIRE)) {
        arena_t a = {0}; jmp_buf trap; a.trap = &trap;
        uint8_t bits[NPROPS]; memset(bits, 0, sizeof bits);
        if (!setjmp(trap)) {
            style_t st, parent; css_style_init(&parent, NULL); css_style_init(&st, &parent);
            struct cx cx = {.s=&st, .parent=&parent, .em=16, .rem=16, .vw=800, .vh=600, .a=&a, .set=bits};
            css_apply(p, "initial", 7, &cx);
        }
        ar_free(&a);
        memcpy(longhand_sets[i], bits, NPROPS);
        __atomic_store_n(&longhand_ready[i], 1, __ATOMIC_RELEASE);
    }
    memcpy(set, longhand_sets[i], NPROPS);
}

/* Fixed notation within the CSS number grammar, to the precision of the stored floats
   (seven significant digits) so a value read back serializes as it was written. */
static void anim_number(sbuf *b, double v) {
    if (!isfinite(v) || fabs(v) < 5e-10) v = 0;
    if (v > 1e15) v = 1e15; else if (v < -1e15) v = -1e15;
    char text[64]; snprintf(text, sizeof text, "%.7g", v);
    if (strchr(text, 'e')) {
        int len = snprintf(text, sizeof text, "%.6f", v);
        while (len > 1 && text[len - 1] == '0') text[--len] = 0;
        if (len > 1 && text[len - 1] == '.') text[--len] = 0;
    }
    if (!strcmp(text, "-0")) strcpy(text, "0");
    sb_puts(b, text);
}
static void anim_unit(sbuf *b, double v, const char *unit) { anim_number(b, v); sb_puts(b, unit); }
static double anim_clamp(double v, double lo, double hi) { return v < lo ? lo : v > hi ? hi : v; }
/* the combinations: interpolation, addition, accumulation */
enum { MIX, ADD, ACCUMULATE };
/* <color>: computed values of color.c's type (exact, or an expression of currentcolor) */
static void anim_shadow_color(const style_t *st, const struct css_box_shadow *s, struct css_color_computed *out) {
    css_color_held(s->color, s->color_value, st->color, out);
}
/* the used value a computed colour names: currentcolor is the style's color (CSS Color 4 §13) */
static struct css_color anim_color_used(const style_t *st, const struct css_color_computed *v) {
    if (!v->current) return v->color;
    struct css_color_computed current;
    css_style_color(st, CSS_SLOT_COLOR, &current);
    struct css_color out;
    css_color_resolve(v, &current.color, &out);
    return out;
}
/* currentcolor and expressions of it as written, for the cascade to resolve against the element */
static bool anim_color_text(sbuf *b, const struct css_color_computed *v) {
    return v->current ? css_color_value_specified(v->current, b) : css_color_animation_text(&v->color, b);
}
/* x interpolated to y at p (MIX: CSS Color 4 §13, in the space §13.2 gives the two), or y added to
   x (ADD and ACCUMULATE alike: CSS Values 4 §3.3); x's and y's currentcolor are their styles' */
static bool anim_color(sbuf *b, const style_t *sx, const struct css_color_computed *x, const style_t *sy,
                       const struct css_color_computed *y, double p, int mode) {
    /* between one expression of currentcolor the value stays it, resolved after the cascade */
    if (mode == MIX && x->current && css_color_computed_equal(x, y)) return anim_color_text(b, x);
    struct css_color u = anim_color_used(sx, x), v = anim_color_used(sy, y);
    struct css_color_interpolation m = css_color_default_interpolation(&u, &v);
    struct css_color r = mode == MIX ? css_color_interpolate(&u, &v, p, &m) : css_color_add(&u, &v, &m);
    return css_color_animation_text(&r, b);
}
static bool anim_is_length(const len_t *l) { return l->kind == LK_LEN || l->kind == LK_EXPR; }
/* ka·x + kb·y of two <length-percentage>s; a keyword (auto, none, normal) combines only with
   itself. A property without negative values clamps (calc() ranges clamp when parsed). */
static bool anim_len(sbuf *b, const len_t *x, const len_t *y, double ka, double kb, int flags, web_doc *d) {
    if (!anim_is_length(x) || !anim_is_length(y)) {
        if (x->kind != y->kind || anim_is_length(x)) return false;
        if (x->kind == LK_NUMBER) { anim_number(b, x->px * ka + y->px * kb); return true; }
        return css_length_serialize(x, b, d);
    }
    if (x->kind == LK_LEN && y->kind == LK_LEN) {
        double px = x->px * ka + y->px * kb, pct = x->pct * ka + y->pct * kb;
        if (!(flags & LF_NEG)) { if (fabs(pct) < 1e-9 && px < 0) px = 0; if (fabs(px) < 1e-9 && pct < 0) pct = 0; }
        if (fabs(pct) < 1e-9) { anim_unit(b, px, "px"); return true; }
        if (fabs(px) < 1e-9) { anim_unit(b, pct, "%"); return true; }
        sb_puts(b, "calc("); anim_unit(b, pct, "%"); sb_puts(b, " + "); anim_unit(b, px, "px"); sb_putc(b, ')');
        return true;
    }
    /* calc() keeps percentages unresolved: combine the expressions */
    sbuf ta = {0}, tb = {0};
    bool ok = css_length_serialize(x, &ta, d) && css_length_serialize(y, &tb, d);
    if (ok) {
        sb_puts(b, "calc("); sb_puts(b, sb_cstr(&ta)); sb_puts(b, " * "); anim_number(b, ka);
        sb_puts(b, " + "); sb_puts(b, sb_cstr(&tb)); sb_puts(b, " * "); anim_number(b, kb); sb_putc(b, ')');
    }
    sb_free(&ta); sb_free(&tb);
    return ok;
}

/* -------- transform lists (CSS Transforms 1 §9, CSS Transforms 2 §16) */
static bool tf_axis(const struct css_transform *t, double axis[3]) {
    double x = t->v[0], y = t->v[1], z = t->v[2], length = sqrt(x * x + y * y + z * z);
    if (!(length > 0) || !isfinite(length)) return false;
    axis[0] = x / length; axis[1] = y / length; axis[2] = z / length;
    return true;
}
static bool tf_same_axis(const struct css_transform *a, const struct css_transform *b) {
    if (a->v[3] == 0 || b->v[3] == 0) return true;
    double p[3], q[3];
    if (!tf_axis(a, p) || !tf_axis(b, q)) return false;
    return fabs(p[0] - q[0]) <= 1e-6 && fabs(p[1] - q[1]) <= 1e-6 && fabs(p[2] - q[2]) <= 1e-6;
}
/* the neutral function of t's kind, which pads the shorter list */
static struct css_transform tf_identity(const struct css_transform *t) {
    struct css_transform r = {.xy = {{.kind = LK_LEN}, {.kind = LK_LEN}}, .kind = t->kind};
    if (t->kind == CT_SCALE) r.v[0] = r.v[1] = r.v[2] = 1;
    if (t->kind == CT_ROTATE) { r.v[0] = t->v[0]; r.v[1] = t->v[1]; r.v[2] = t->v[2]; }
    for (int i = 0; i < 4; i++) r.m[i * 5] = 1;
    return r;
}
/* Lists combine function by function when every pair shares a primitive (rotations a
   common axis); matrices and perspective only meet themselves. */
static bool tf_lists_match(const struct css_transform *a, const struct css_transform *b) {
    for (; a || b; a = a ? a->next : NULL, b = b ? b->next : NULL) {
        const struct css_transform *t = a ? a : b;
        if (t->kind == CT_MATRIX || t->kind == CT_PERSPECTIVE) return false;
        if (!a || !b) continue;
        if (a->kind != b->kind) return false;
        if (a->kind == CT_ROTATE && !tf_same_axis(a, b)) return false;
    }
    return true;
}
/* ka·x + kb·y function by function (accumulate: scales add their excess over 1). With x == y,
   ka = 1, kb = 0 it serializes any list exactly, matrices and perspective included. */
static bool tf_pairs(sbuf *b, const struct css_transform *x, const struct css_transform *y, double ka, double kb,
                     int mode, web_doc *d) {
    bool first = true;
    for (; x || y; x = x ? x->next : NULL, y = y ? y->next : NULL) {
        struct css_transform ix, iy;
        const struct css_transform *s = x, *e = y;
        if (!s) { ix = tf_identity(e); s = &ix; }
        if (!e) { iy = tf_identity(s); e = &iy; }
        if (!first) sb_putc(b, ' ');
        first = false;
        double v[4];
        for (int i = 0; i < 4; i++) v[i] = s->v[i] * ka + e->v[i] * kb;
        if (mode == ACCUMULATE && s->kind == CT_SCALE) for (int i = 0; i < 3; i++) v[i] = s->v[i] + e->v[i] - 1;
        switch (s->kind) {
        case CT_TRANSLATE:
            sb_puts(b, v[0] ? "translate3d(" : "translate(");
            if (!anim_len(b, &s->xy[0], &e->xy[0], ka, kb, LF_NEG, d)) return false;
            sb_puts(b, ", ");
            if (!anim_len(b, &s->xy[1], &e->xy[1], ka, kb, LF_NEG, d)) return false;
            if (v[0]) { sb_puts(b, ", "); anim_unit(b, v[0], "px"); }
            sb_putc(b, ')');
            break;
        case CT_SCALE:
            sb_puts(b, v[2] != 1 ? "scale3d(" : "scale(");
            anim_number(b, v[0]); sb_puts(b, ", "); anim_number(b, v[1]);
            if (v[2] != 1) { sb_puts(b, ", "); anim_number(b, v[2]); }
            sb_putc(b, ')');
            break;
        case CT_ROTATE: {
            double axis[3] = {0, 0, 1};
            if (!tf_axis(s->v[3] != 0 || e->v[3] == 0 ? s : e, axis)) { axis[0] = axis[1] = 0; axis[2] = 1; }
            double angle = s->v[3] * ka + e->v[3] * kb;
            if (axis[0] == 0 && axis[1] == 0 && axis[2] > 0) { sb_puts(b, "rotate("); anim_unit(b, angle, "rad)"); }
            else {
                sb_puts(b, "rotate3d(");
                for (int i = 0; i < 3; i++) { anim_number(b, axis[i]); sb_puts(b, ", "); }
                anim_unit(b, angle, "rad)");
            }
            break;
        }
        case CT_SKEW:
            sb_puts(b, "skew("); anim_unit(b, v[0], "rad, "); anim_unit(b, v[1], "rad)");
            break;
        case CT_PERSPECTIVE:
            if (!(v[0] > 0)) { sb_puts(b, "perspective(none)"); break; }
            sb_puts(b, "perspective("); anim_unit(b, v[0], "px)");
            break;
        case CT_MATRIX:
            sb_puts(b, "matrix3d(");
            for (int col = 0; col < 4; col++)
                for (int row = 0; row < 4; row++) {
                    anim_number(b, s->m[row * 4 + col] * ka + e->m[row * 4 + col] * kb);
                    if (col < 3 || row < 3) sb_puts(b, ", ");
                }
            sb_putc(b, ')');
            break;
        default: return false;
        }
    }
    if (first) sb_puts(b, "none");
    return true;
}
struct tf_2d { double tx, ty, sx, sy, angle, m11, m12, m21, m22; };
/* CSS Transforms 1 §10 "Interpolation of matrices", 2D decomposition. Percentages need the
   used box; without one the list does not decompose. */
static bool tf_decompose(const struct css_transform *t, const struct box *box, struct tf_2d *out) {
    float w = 0, h = 0;
    if (box) {
        w = box->w + box->p[1] + box->p[3] + box->b[1] + box->b[3];
        h = box->h + box->p[0] + box->p[2] + box->b[0] + box->b[2];
    } else for (const struct css_transform *x = t; x; x = x->next)
        if (x->kind == CT_TRANSLATE && (len_has_pct(&x->xy[0]) || len_has_pct(&x->xy[1]))) return false;
    double m[16]; css_transform_list_matrix(t, w, h, m);
    double r0x = m[0], r0y = m[4], r1x = m[1], r1y = m[5];
    out->tx = m[3]; out->ty = m[7];
    out->sx = sqrt(r0x * r0x + r0y * r0y); out->sy = sqrt(r1x * r1x + r1y * r1y);
    if (r0x * r1y - r0y * r1x < 0) { if (r0x < r1y) out->sx = -out->sx; else out->sy = -out->sy; }
    if (out->sx) { r0x /= out->sx; r0y /= out->sx; }
    if (out->sy) { r1x /= out->sy; r1y /= out->sy; }
    out->angle = atan2(r0y, r0x);
    if (out->angle) {
        double sn = -r0y, cs = r0x, a = r0x, b = r0y, cc = r1x, dd = r1y;
        r0x = cs * a + sn * cc; r0y = cs * b + sn * dd; r1x = -sn * a + cs * cc; r1y = -sn * b + cs * dd;
    }
    out->m11 = r0x; out->m12 = r0y; out->m21 = r1x; out->m22 = r1y;
    const double *f = &out->tx;
    for (int i = 0; i < 9; i++) if (!isfinite(f[i])) return false;
    return true;
}
static bool tf_matrix_mix(sbuf *b, const struct css_transform *x, const struct css_transform *y, const struct box *box, double t) {
    struct tf_2d A, B;
    if (!tf_decompose(x, box, &A) || !tf_decompose(y, box, &B)) return false;
    if ((A.sx < 0 && B.sy < 0) || (A.sy < 0 && B.sx < 0)) { A.sx = -A.sx; A.sy = -A.sy; A.angle += A.angle < 0 ? M_PI : -M_PI; }
    if (!A.angle) A.angle = 2 * M_PI;
    if (!B.angle) B.angle = 2 * M_PI;
    if (fabs(A.angle - B.angle) > M_PI) { if (A.angle > B.angle) A.angle -= 2 * M_PI; else B.angle -= 2 * M_PI; }
    struct tf_2d R; double *r = &R.tx; const double *a = &A.tx, *q = &B.tx;
    for (int i = 0; i < 9; i++) r[i] = a[i] + (q[i] - a[i]) * t;
    /* rows = diag(sx, sy) · rotate · remainder */
    double cs = cos(R.angle), sn = sin(R.angle);
    double n11 = cs * R.m11 + sn * R.m21, n12 = cs * R.m12 + sn * R.m22, n21 = -sn * R.m11 + cs * R.m21, n22 = -sn * R.m12 + cs * R.m22;
    double values[6] = {n11 * R.sx, n12 * R.sx, n21 * R.sy, n22 * R.sy, R.tx, R.ty};
    sb_puts(b, "matrix(");
    for (int i = 0; i < 6; i++) { anim_number(b, values[i]); if (i < 5) sb_puts(b, ", "); }
    sb_putc(b, ')');
    return true;
}
static bool tf_list(sbuf *b, const struct css_transform *x, const struct css_transform *y, double ka, double kb,
                    int mode, const struct box *box, web_doc *d) {
    if (mode == ADD || (mode == ACCUMULATE && !tf_lists_match(x, y))) {
        /* addition appends the value's functions to the underlying list */
        if (!x) return tf_pairs(b, y, y, 1, 0, MIX, d);
        if (!tf_pairs(b, x, x, 1, 0, MIX, d)) return false;
        if (y) { sb_putc(b, ' '); return tf_pairs(b, y, y, 1, 0, MIX, d); }
        return true;
    }
    if (tf_lists_match(x, y)) return tf_pairs(b, x, y, ka, kb, mode, d);
    return tf_matrix_mix(b, x, y, box, kb);
}
static void tf_rotate_text(sbuf *b, const struct css_transform *t) {
    if (!t) { sb_puts(b, "none"); return; }
    double deg = t->v[3] * 180 / M_PI;
    if (t->v[0] == 0 && t->v[1] == 0 && t->v[2] > 0) { anim_unit(b, deg, "deg"); return; }
    for (int i = 0; i < 3; i++) { anim_number(b, t->v[i]); sb_putc(b, ' '); }
    anim_unit(b, deg, "deg");
}
static void tf_scale_text(sbuf *b, const struct css_transform *t) {
    if (!t) { sb_puts(b, "none"); return; }
    anim_number(b, t->v[0]); sb_putc(b, ' '); anim_number(b, t->v[1]);
    if (t->v[2] != 1) { sb_putc(b, ' '); anim_number(b, t->v[2]); }
}
static bool tf_equal(const struct css_transform *a, const struct css_transform *b) {
    for (; a && b; a = a->next, b = b->next)
        if (a->kind != b->kind || memcmp(a->v, b->v, sizeof a->v) || memcmp(a->m, b->m, sizeof a->m) ||
            !css_length_equal(&a->xy[0], &b->xy[0]) || !css_length_equal(&a->xy[1], &b->xy[1])) return false;
    return !a && !b;
}

/* -------- shadow and filter lists */
/* one shadow's computed value: its colour first, then the four lengths (and inset) */
static void anim_shadow_lengths(sbuf *b, double x, double y, double blur, double spread, bool inset) {
    anim_unit(b, x, "px "); anim_unit(b, y, "px ");
    anim_unit(b, anim_clamp(blur, 0, INFINITY), "px ");
    anim_unit(b, spread, "px");
    if (inset) sb_puts(b, " inset");
}
static void anim_shadow_text(sbuf *b, const style_t *st, const struct css_box_shadow *s) {
    struct css_color_computed c;
    anim_shadow_color(st, s, &c);
    anim_color_text(b, &c); sb_putc(b, ' ');
    anim_shadow_lengths(b, s->x, s->y, s->blur, s->spread, s->inset);
}
static void anim_shadow_list_text(sbuf *b, const style_t *st) {
    if (!st->box_shadow) { sb_puts(b, "none"); return; }
    for (const struct css_box_shadow *s = st->box_shadow; s; s = s->next) {
        if (s != st->box_shadow) sb_puts(b, ", ");
        anim_shadow_text(b, st, s);
    }
}
/* a pair of shadows component by component (Web Animations 1, A: interpolation, and accumulation
   with each component accumulated); the shorter list pads with transparent shadows of no length */
static bool anim_shadow_item(sbuf *b, const style_t *sa, const struct css_box_shadow *s, const style_t *sv,
                             const struct css_box_shadow *e, double ka, double kb, int mode) {
    struct css_box_shadow zs = {0}, ze = {0};
    if (!s) { zs.inset = e->inset; s = &zs; }
    if (!e) { ze.inset = s->inset; e = &ze; }
    if (s->inset != e->inset) return false;
    struct css_color_computed cs, ce;
    anim_shadow_color(sa, s, &cs);
    anim_shadow_color(sv, e, &ce);
    if (!anim_color(b, sa, &cs, sv, &ce, kb, mode)) return false;
    sb_putc(b, ' ');
    anim_shadow_lengths(b, s->x * ka + e->x * kb, s->y * ka + e->y * kb, s->blur * ka + e->blur * kb,
                        s->spread * ka + e->spread * kb, s->inset);
    return true;
}
static bool anim_shadows(sbuf *b, const style_t *sa, const style_t *sv, double ka, double kb, int mode) {
    const struct css_box_shadow *x = sa->box_shadow, *y = sv->box_shadow;
    if (!x && !y) { sb_puts(b, "none"); return true; }
    bool first = true;
    if (mode == ADD) {
        /* addition is the concatenation of the lists */
        for (const struct css_box_shadow *s = x; s; s = s->next, first = false) {
            if (!first) sb_puts(b, ", ");
            anim_shadow_text(b, sa, s);
        }
        for (const struct css_box_shadow *s = y; s; s = s->next, first = false) {
            if (!first) sb_puts(b, ", ");
            anim_shadow_text(b, sv, s);
        }
        return true;
    }
    for (; x || y; x = x ? x->next : NULL, y = y ? y->next : NULL, first = false) {
        if (!first) sb_puts(b, ", ");
        if (!anim_shadow_item(b, sa, x, sv, y, ka, kb, mode)) return false;
    }
    return true;
}
static double filter_amount(const struct css_filter_step *s) { return s->percentage ? s->value / 100 : s->value; }
static bool anim_filters(sbuf *b, const struct css_filter *x, const struct css_filter *y, double ka, double kb, int mode) {
    size_t nx = x ? x->count : 0, ny = y ? y->count : 0;
    if (!nx && !ny) { sb_puts(b, "none"); return true; }
    if (mode == ADD) {
        for (size_t i = 0; i < nx + ny; i++) {
            const struct css_filter_step *s = i < nx ? &x->steps[i] : &y->steps[i - nx];
            if (i) sb_putc(b, ' ');
            sb_puts(b, "opacity("); anim_number(b, filter_amount(s)); sb_putc(b, ')');
        }
        return true;
    }
    /* the shorter list pads with each function's initial value, opacity(1) */
    size_t n = nx > ny ? nx : ny;
    for (size_t i = 0; i < n; i++) {
        double a = i < nx ? filter_amount(&x->steps[i]) : 1, c = i < ny ? filter_amount(&y->steps[i]) : 1;
        double v = mode == ACCUMULATE ? a + c - 1 : a * ka + c * kb;
        if (i) sb_putc(b, ' ');
        sb_puts(b, "opacity("); anim_number(b, anim_clamp(v, 0, INFINITY)); sb_putc(b, ')');
    }
    return true;
}

/* -------- serialization of computed values */
static const char *display_keyword(const style_t *st) {
    static const char *const names[] = {"none", "inline", "block", "list-item", "inline-block", "table",
        "inline-table", "table-row-group", "table-header-group", "table-footer-group", "table-row", "table-cell",
        "table-column", "table-column-group", "table-caption", "flex", "inline-flex", "grid", "inline-grid",
        "contents", "flow-root"};
    if (st->legacy_box) return st->display == D_INLINE_BLOCK || st->display == D_INLINE_FLEX ? "-webkit-inline-box" : "-webkit-box";
    return st->display < sizeof names / sizeof *names ? names[st->display] : NULL;
}
static bool anim_pair_text(sbuf *b, const len_t *l, web_doc *d) {
    return css_length_serialize(&l[0], b, d) && length_append(b, " ", 1) && css_length_serialize(&l[1], b, d);
}
static bool same_text(const char *a, const char *b) { return a == b || (a && b && !strcmp(a, b)); }
bool css_computed_value(const style_t *st, const struct propdef *p, sbuf *out, web_doc *d) {
    if (!st || !p || p->type == PT_SHORT) return false;
    const char *field = (const char *)st + p->off;
    size_t begin = out->n;
    char text[64];
    bool ok = true;
    switch (p->type) {
    case PT_KW: case PT_OVERFLOW: case PT_VALIGN: {
        if (p->type == PT_VALIGN && st->vertical_align == VA_LEN) { anim_unit(out, st->vertical_align_px, "px"); break; }
        uint8_t v = *(const uint8_t *)field;
        const struct kw *k = p->kws;
        while (k && k->name && k->v != v) k++;
        if (!k || !k->name) return false;
        sb_puts(out, k->name);
        break;
    }
    case PT_DISPLAY: {
        const char *name = display_keyword(st);
        if (!name) return false;
        sb_puts(out, name);
        break;
    }
    case PT_LEN: case PT_SVG_WIDTH: ok = css_length_serialize((const len_t *)field, out, d); break;
    case PT_PX: anim_unit(out, *(const float *)field, "px"); break;
    case PT_FONT_SIZE: anim_unit(out, st->font_size, "px"); break;
    case PT_NUM: case PT_OPACITY: anim_number(out, *(const float *)field); break;
    case PT_INT: snprintf(text, sizeof text, "%d", *(const int *)field); sb_puts(out, text); break;
    case PT_ZINDEX:
        if (st->z_auto) sb_puts(out, "auto");
        else { snprintf(text, sizeof text, "%d", st->z_index); sb_puts(out, text); }
        break;
    case PT_FONT_WEIGHT: snprintf(text, sizeof text, "%u", (unsigned)st->font_weight); sb_puts(out, text); break;
    case PT_LINE_HEIGHT:
        if (st->line_height.kind == LK_NORMAL) sb_puts(out, "normal");
        else if (st->line_height.kind == LK_NUMBER) anim_number(out, st->line_height.px);
        else anim_unit(out, st->line_height.px, "px");
        break;
    case PT_COLOR: case PT_SVG_PAINT: {
        if (p->type == PT_SVG_PAINT && ((const struct svg_paint *)field)->kind != SVG_PAINT_COLOR) {
            if (((const struct svg_paint *)field)->kind != SVG_PAINT_NONE) return false;
            sb_puts(out, "none");
            break;
        }
        struct css_color_computed c;
        css_style_color(st, color_slot(p), &c);
        ok = anim_color_text(out, &c);
        break;
    }
    case PT_FONT_FAMILY: if (!st->font_names) return false; sb_puts(out, st->font_names); break;
    case PT_CONTAINER_NAMES: sb_puts(out, st->container_names ? st->container_names : "none"); break;
    case PT_LINE_CLAMP:
        if (!st->line_clamp) sb_puts(out, "none");
        else { snprintf(text, sizeof text, "%u", (unsigned)st->line_clamp); sb_puts(out, text); }
        break;
    case PT_TEXT_DECO:
        if (!st->text_decoration) sb_puts(out, "none");
        else {
            bool first = true;
            if (st->text_decoration & TD_UNDERLINE) { sb_puts(out, "underline"); first = false; }
            if (st->text_decoration & TD_OVERLINE) { if (!first) sb_putc(out, ' '); sb_puts(out, "overline"); first = false; }
            if (st->text_decoration & TD_LINE_THROUGH) { if (!first) sb_putc(out, ' '); sb_puts(out, "line-through"); }
        }
        break;
    case PT_BG_IMAGE: case PT_MASK_IMAGE: {
        const char *url = p->type == PT_BG_IMAGE ? st->bg_image : st->mask_image;
        if (p->type == PT_BG_IMAGE && st->has_grad) return false;
        if (!url) { sb_puts(out, "none"); break; }
        sb_puts(out, "url(\"");
        for (const char *c = url; *c; c++) { if (*c == '"' || *c == '\\') sb_putc(out, '\\'); sb_putc(out, *c); }
        sb_puts(out, "\")");
        break;
    }
    case PT_GLINE: {
        const struct gline *g = (const struct gline *)field;
        if (g->kind == GL_AUTO) sb_puts(out, "auto");
        else if (g->kind == GL_SPAN) { snprintf(text, sizeof text, "span %d", g->n); sb_puts(out, text); }
        else if (g->kind == GL_NAME) {
            sb_puts(out, g->name);
            if (g->n != 1) { snprintf(text, sizeof text, " %d", g->n); sb_puts(out, text); }
        } else { snprintf(text, sizeof text, "%d", g->n); sb_puts(out, text); }
        break;
    }
    case PT_FILTER: ok = css_filter_serialize(st->filter, out, d); break;
    case PT_BOX_SHADOW: anim_shadow_list_text(out, st); break;
    case PT_RADIUS: ok = anim_pair_text(out, ((const struct css_corner_radius *)field)->axis, d); break;
    case PT_BG_POS: case PT_TRANSFORM_ORIGIN: ok = anim_pair_text(out, (const len_t *)field, d); break;
    case PT_BG_SIZE: case PT_MASK_SIZE: {
        uint8_t kind = p->type == PT_BG_SIZE ? st->bg_size_kind : st->mask_size_kind;
        if (kind == BSZ_COVER) sb_puts(out, "cover");
        else if (kind == BSZ_CONTAIN) sb_puts(out, "contain");
        else if (kind == BSZ_AUTO) sb_puts(out, "auto");
        else ok = anim_pair_text(out, p->type == PT_BG_SIZE ? st->bg_size : st->mask_size, d);
        break;
    }
    case PT_TRANSFORM: ok = tf_pairs(out, st->transform, st->transform, 1, 0, MIX, d); break;
    case PT_TRANSLATE:
        if (!st->translate.active) sb_puts(out, "none");
        else ok = anim_pair_text(out, st->translate.xy, d);
        break;
    case PT_ROTATE: tf_rotate_text(out, st->rotate); break;
    case PT_SCALE: tf_scale_text(out, st->scale); break;
    case PT_CLIP_PATH:
        if (!st->clip_path) sb_puts(out, "none");
        else ok = css_clip_path_serialize(st->clip_path, out, d);
        break;
    case PT_CLIP: ok = css_clip_rect_serialize(&st->clip, out); break;
    case PT_MOTION_LIST: {
        int list = p->flags;
        uint32_t count = css_motion_count(st, list);
        for (uint32_t i = 0; i < count; i++) {
            union css_motion_item item = css_motion_item(st, list, i);
            if (i) sb_puts(out, ", ");
            switch (list) {
            case ML_NAME: case ML_TPROPERTY:
                if (!item.s) sb_puts(out, "none");
                else if (list == ML_TPROPERTY || (motion_name(item.s, strlen(item.s)) && !ident_is(item.s, strlen(item.s), "none")))
                    sb_puts(out, item.s);
                else {
                    sb_putc(out, '"');
                    for (const char *c = item.s; *c; c++) { if (*c == '"' || *c == '\\') sb_putc(out, '\\'); sb_putc(out, *c); }
                    sb_putc(out, '"');
                }
                break;
            case ML_DURATION: case ML_DELAY: case ML_TDURATION: case ML_TDELAY:
                if (isnan(item.f)) sb_puts(out, "auto");
                else anim_unit(out, item.f / 1000, "s");
                break;
            case ML_EASING: case ML_TEASING: css_easing_serialize(&item.e, out); break;
            case ML_ITERATIONS:
                if (isinf(item.f)) sb_puts(out, "infinite");
                else anim_number(out, item.f);
                break;
            default: {
                const struct kw *k = p->kws;
                while (k && k->name && k->v != item.k) k++;
                if (!k || !k->name) return false;
                sb_puts(out, k->name);
            }
            }
        }
        break;
    }

    default: return false;
    }
    if (!ok) { out->n = begin; if (out->p && begin < out->cap) out->p[begin] = 0; }
    return ok;
}

bool css_anim_equal(const struct propdef *p, const style_t *a, const style_t *b) {
    if (!p || p->type == PT_SHORT) return true;
    const char *fa = (const char *)a + p->off, *fb = (const char *)b + p->off;
    switch (p->type) {
    case PT_LEN: case PT_SVG_WIDTH: return css_length_equal((const len_t *)fa, (const len_t *)fb);
    case PT_RADIUS: case PT_BG_POS: case PT_TRANSFORM_ORIGIN: {
        const len_t *x = p->type == PT_RADIUS ? ((const struct css_corner_radius *)fa)->axis : (const len_t *)fa;
        const len_t *y = p->type == PT_RADIUS ? ((const struct css_corner_radius *)fb)->axis : (const len_t *)fb;
        return css_length_equal(&x[0], &y[0]) && css_length_equal(&x[1], &y[1]);
    }
    case PT_BG_SIZE: return a->bg_size_kind == b->bg_size_kind && css_length_equal(&a->bg_size[0], &b->bg_size[0]) &&
        css_length_equal(&a->bg_size[1], &b->bg_size[1]);
    case PT_MASK_SIZE: return a->mask_size_kind == b->mask_size_kind && css_length_equal(&a->mask_size[0], &b->mask_size[0]) &&
        css_length_equal(&a->mask_size[1], &b->mask_size[1]);
    case PT_TRANSFORM: case PT_ROTATE: case PT_SCALE:
        return tf_equal(*(const struct css_transform *const *)fa, *(const struct css_transform *const *)fb);
    case PT_TRANSLATE: return a->translate.active == b->translate.active && css_length_equal(&a->translate.xy[0], &b->translate.xy[0]) &&
        css_length_equal(&a->translate.xy[1], &b->translate.xy[1]);
    case PT_BOX_SHADOW: {
        const struct css_box_shadow *x = a->box_shadow, *y = b->box_shadow;
        for (; x && y; x = x->next, y = y->next) {
            if (x->x != y->x || x->y != y->y || x->blur != y->blur || x->spread != y->spread || x->inset != y->inset)
                return false;
            struct css_color_computed u, v;
            anim_shadow_color(a, x, &u);
            anim_shadow_color(b, y, &v);
            if (!css_color_computed_equal(&u, &v)) return false;
        }
        return !x && !y;
    }
    case PT_FILTER: {
        const struct css_filter *x = a->filter, *y = b->filter;
        if (!x || !y) return x == y;
        return x->count == y->count && !memcmp(x->steps, y->steps, x->count * sizeof *x->steps);
    }
    case PT_COLOR: case PT_SVG_PAINT: {
        if (p->type == PT_SVG_PAINT) {
            const struct svg_paint *x = (const struct svg_paint *)fa, *y = (const struct svg_paint *)fb;
            if (x->kind != y->kind || !same_text(x->ref, y->ref)) return false;
            if (x->kind != SVG_PAINT_COLOR) return true;
        }
        struct css_color_computed x, y;
        css_style_color(a, color_slot(p), &x);
        css_style_color(b, color_slot(p), &y);
        return css_color_computed_equal(&x, &y);
    }
    case PT_VALIGN: return a->vertical_align == b->vertical_align && (a->vertical_align != VA_LEN || a->vertical_align_px == b->vertical_align_px);
    case PT_ZINDEX: return a->z_auto == b->z_auto && (a->z_auto || a->z_index == b->z_index);
    case PT_DISPLAY: return a->display == b->display && a->legacy_box == b->legacy_box;
    case PT_FONT_FAMILY: return a->font_family == b->font_family && same_text(a->font_names, b->font_names);
    case PT_CONTAINER_NAMES: return same_text(a->container_names, b->container_names);
    case PT_CONTENT: return same_text(a->content, b->content);
    case PT_LIST_TYPE: return a->list_style == b->list_style && same_text(a->list_style_string, b->list_style_string);
    case PT_BG_IMAGE: return same_text(a->bg_image, b->bg_image) && a->has_grad == b->has_grad && a->gradient == b->gradient;
    case PT_MASK_IMAGE: return same_text(a->mask_image, b->mask_image);
    case PT_GLINE: {
        const struct gline *x = (const struct gline *)fa, *y = (const struct gline *)fb;
        return x->kind == y->kind && x->n == y->n && same_text(x->name, y->name);
    }
    case PT_CLIP_PATH: {
        if (!a->clip_path || !b->clip_path) return a->clip_path == b->clip_path;
        sbuf x = {0}, y = {0};
        bool same = css_clip_path_serialize(a->clip_path, &x, NULL) && css_clip_path_serialize(b->clip_path, &y, NULL) &&
            x.n == y.n && !memcmp(x.p, y.p, x.n);
        sb_free(&x); sb_free(&y);
        return same;
    }
    case PT_GTEMPLATE: case PT_GAREAS:
        return *(const void *const *)fa == *(const void *const *)fb;
    case PT_MOTION_LIST: return css_motion_equal(&a->motion, &b->motion);
    }
    return !memcmp(fa, fb, p->size);
}

bool css_anim_parse(const struct css_anim_ctx *c, const struct propdef *p, const char *v, size_t n, bool computed,
                    style_t *out, uint8_t *set) {
    if (!p || !v) return false;
    *out = *c->base;
    uint8_t local[NPROPS];
    if (!set) set = local;
    memset(set, 0, NPROPS);
    const style_t *parent = c->parent;
    struct cx cx = {.s = out, .parent = parent, .node = c->node, .em = parent ? parent->font_size : 16, .rem = c->rem,
                    .vw = c->vw, .vh = c->vh, .a = c->a, .set = set, .font_pass = true, .computed_text = computed};
    /* font-size first, against the parent's, as the cascade does; then the rest against it */
    if (css_prop_is_early(p) && !css_apply(p, v, n, &cx)) return false;
    cx.font_pass = false;
    cx.em = out->font_size;
    return css_apply(p, v, n, &cx) && !cx.invalid;
}

bool css_anim_discrete_second(const struct propdef *p, const style_t *a, const style_t *b, double t) {
    int type = css_prop_animation(p);
    if (type == CSS_ANIM_DISPLAY || type == CSS_ANIM_VISIBILITY) {
        if (t <= 0) return false;
        if (t >= 1) return true;
        bool hides_a, hides_b;
        if (type == CSS_ANIM_VISIBILITY) hides_a = a->visibility != 0, hides_b = b->visibility != 0;
        else if (p->type == PT_DISPLAY) hides_a = a->display == D_NONE, hides_b = b->display == D_NONE;
        else hides_a = a->content_visibility == CV_HIDDEN, hides_b = b->content_visibility == CV_HIDDEN;
        /* inside the interval the showing value wins */
        if (hides_a != hides_b) return hides_a;
    }
    return t >= .5;
}

/* ka·a + kb·b for each value type (mode MIX), or b added / accumulated onto a */
static bool anim_combine(const struct css_anim_ctx *c, const struct propdef *p, const style_t *a, const style_t *b,
                         double ka, double kb, int mode, sbuf *out) {
    const char *fa = (const char *)a + p->off, *fb = (const char *)b + p->off;
    web_doc *d = c->d;
    switch (p->type) {
    case PT_LEN: case PT_SVG_WIDTH:
        return anim_len(out, (const len_t *)fa, (const len_t *)fb, ka, kb, p->type == PT_SVG_WIDTH ? 0 : p->flags, d);
    case PT_PX: {
        double v = *(const float *)fa * ka + *(const float *)fb * kb;
        anim_unit(out, p->flags & LF_NEG ? v : anim_clamp(v, 0, INFINITY), "px");
        return true;
    }
    case PT_FONT_SIZE: anim_unit(out, anim_clamp(a->font_size * ka + b->font_size * kb, 0, INFINITY), "px"); return true;
    case PT_NUM: anim_number(out, anim_clamp(*(const float *)fa * ka + *(const float *)fb * kb, 0, INFINITY)); return true;
    case PT_OPACITY: anim_number(out, anim_clamp(*(const float *)fa * ka + *(const float *)fb * kb, 0, 1)); return true;
    case PT_FONT_WEIGHT: anim_number(out, anim_clamp(a->font_weight * ka + b->font_weight * kb, 1, 1000)); return true;
    case PT_INT: case PT_ZINDEX: {
        if (p->type == PT_ZINDEX && (a->z_auto || b->z_auto)) return false;
        /* <integer>: as a real number, then to the nearest integer, halves towards +infinity */
        char text[32]; snprintf(text, sizeof text, "%.0f", floor(*(const int *)fa * ka + *(const int *)fb * kb + .5));
        sb_puts(out, text);
        return true;
    }
    case PT_LINE_HEIGHT:
        if (a->line_height.kind != b->line_height.kind || a->line_height.kind == LK_NORMAL) return false;
        if (a->line_height.kind == LK_NUMBER) anim_number(out, anim_clamp(a->line_height.px * ka + b->line_height.px * kb, 0, INFINITY));
        else anim_unit(out, anim_clamp(a->line_height.px * ka + b->line_height.px * kb, 0, INFINITY), "px");
        return true;
    case PT_VALIGN:
        if (a->vertical_align != VA_LEN || b->vertical_align != VA_LEN) return false;
        anim_unit(out, a->vertical_align_px * ka + b->vertical_align_px * kb, "px");
        return true;
    case PT_COLOR: case PT_SVG_PAINT: {
        if (p->type == PT_SVG_PAINT && (((const struct svg_paint *)fa)->kind != SVG_PAINT_COLOR ||
                                        ((const struct svg_paint *)fb)->kind != SVG_PAINT_COLOR)) return false;
        struct css_color_computed x, y;
        css_style_color(a, color_slot(p), &x);
        css_style_color(b, color_slot(p), &y);
        return anim_color(out, a, &x, b, &y, kb, mode);
    }
    case PT_RADIUS: {
        const len_t *x = ((const struct css_corner_radius *)fa)->axis, *y = ((const struct css_corner_radius *)fb)->axis;
        return anim_len(out, &x[0], &y[0], ka, kb, 0, d) && length_append(out, " ", 1) && anim_len(out, &x[1], &y[1], ka, kb, 0, d);
    }
    case PT_BG_POS: case PT_TRANSFORM_ORIGIN: {
        const len_t *x = (const len_t *)fa, *y = (const len_t *)fb;
        return anim_len(out, &x[0], &y[0], ka, kb, LF_NEG, d) && length_append(out, " ", 1) && anim_len(out, &x[1], &y[1], ka, kb, LF_NEG, d);
    }
    case PT_BG_SIZE: case PT_MASK_SIZE: {
        bool bg = p->type == PT_BG_SIZE;
        uint8_t kx = bg ? a->bg_size_kind : a->mask_size_kind, ky = bg ? b->bg_size_kind : b->mask_size_kind;
        if (kx != BSZ_LEN || ky != BSZ_LEN) {
            if (kx != ky) return false;
            return css_computed_value(a, p, out, d);
        }
        const len_t *x = bg ? a->bg_size : a->mask_size, *y = bg ? b->bg_size : b->mask_size;
        return anim_len(out, &x[0], &y[0], ka, kb, 0, d) && length_append(out, " ", 1) && anim_len(out, &x[1], &y[1], ka, kb, 0, d);
    }
    case PT_TRANSFORM: return tf_list(out, a->transform, b->transform, ka, kb, mode, c->box, d);
    case PT_TRANSLATE:
        if (!a->translate.active && !b->translate.active) { sb_puts(out, "none"); return true; }
        /* none is translate(0) */
        return anim_len(out, &a->translate.xy[0], &b->translate.xy[0], ka, kb, LF_NEG, d) && length_append(out, " ", 1) &&
            anim_len(out, &a->translate.xy[1], &b->translate.xy[1], ka, kb, LF_NEG, d);
    case PT_ROTATE: {
        const struct css_transform *x = a->rotate, *y = b->rotate;
        if (!x && !y) { sb_puts(out, "none"); return true; }
        struct css_transform ix, iy;
        if (!x) { ix = tf_identity(y); x = &ix; }
        if (!y) { iy = tf_identity(x); y = &iy; }
        if (!tf_same_axis(x, y)) return false;
        struct css_transform r = x->v[3] != 0 || y->v[3] == 0 ? *x : *y;
        r.v[3] = x->v[3] * ka + y->v[3] * kb;
        tf_rotate_text(out, &r);
        return true;
    }
    case PT_SCALE: {
        const struct css_transform *x = a->scale, *y = b->scale;
        if (!x && !y) { sb_puts(out, "none"); return true; }
        struct css_transform ix, iy;
        if (!x) { ix = tf_identity(y); x = &ix; }
        if (!y) { iy = tf_identity(x); y = &iy; }
        struct css_transform r = *x;
        /* CSS Transforms 2 §16: scales multiply when added, add their excess over 1 when accumulated */
        for (int i = 0; i < 3; i++)
            r.v[i] = mode == ADD ? x->v[i] * y->v[i] : mode == ACCUMULATE ? x->v[i] + y->v[i] - 1 : x->v[i] * ka + y->v[i] * kb;
        tf_scale_text(out, &r);
        return true;
    }
    case PT_BOX_SHADOW: return anim_shadows(out, a, b, ka, kb, mode);
    case PT_FILTER: return anim_filters(out, a->filter, b->filter, ka, kb, mode);
    case PT_CLIP:
        if (!a->clip.rect || !b->clip.rect || a->clip.auto_edges != b->clip.auto_edges) return false;
        sb_puts(out, "rect(");
        for (int i = 0; i < 4; i++) {
            if (i) sb_puts(out, ", ");
            if (a->clip.auto_edges >> i & 1) sb_puts(out, "auto");
            else anim_unit(out, a->clip.edge[i] * ka + b->clip.edge[i] * kb, "px");
        }
        sb_putc(out, ')');
        return true;
    }
    return false;
}

bool css_anim_interpolate(const struct css_anim_ctx *c, const struct propdef *p, const style_t *a, const style_t *b,
                          double t, sbuf *out) {
    int type = css_prop_animation(p);
    if (type == CSS_ANIM_VISIBILITY)
        return css_computed_value(a->visibility == b->visibility || !css_anim_discrete_second(p, a, b, t) ? a : b, p, out, c->d);
    if (type != CSS_ANIM_VALUE) return false;
    size_t begin = out->n;
    if (anim_combine(c, p, a, b, 1 - t, t, MIX, out)) return true;
    out->n = begin;
    if (out->p && begin < out->cap) out->p[begin] = 0;
    return false;
}

bool css_anim_compose(const struct css_anim_ctx *c, const struct propdef *p, const style_t *underlying,
                      const style_t *value, bool accumulate, sbuf *out) {
    if (css_prop_animation(p) != CSS_ANIM_VALUE) return false;
    size_t begin = out->n;
    if (anim_combine(c, p, underlying, value, 1, 1, accumulate ? ACCUMULATE : ADD, out)) return true;
    out->n = begin;
    if (out->p && begin < out->cap) out->p[begin] = 0;
    return false;
}
