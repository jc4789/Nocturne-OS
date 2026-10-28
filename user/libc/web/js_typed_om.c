/* CSS Typed OM Level 1 (https://drafts.css-houdini.org/css-typed-om-1/): the native half of
   js_typed_om.js (see js_typed_om.h). Nothing here keeps state between calls: a value's type
   algebra and units are cssmath.c's, a property's grammar cssprop.c's, a colour color.c's, and
   the tokens CSS Syntax's through Lexbor's tokenizer, one per call. */
#include "js_typed_om.h"
#include "webi.h"
#include <lexbor/css/syntax/tokenizer.h>
#include <lexbor/css/syntax/token.h>
#include <setjmp.h>
#include <stdlib.h>
#include <string.h>

static const char *text_arg(JSContext *ctx, int argc, JSValueConst *argv, int i, size_t *n) {
    if (i >= argc) return JS_ToCStringLen(ctx, n, JS_UNDEFINED);
    return JS_ToCStringLen(ctx, n, argv[i]);
}
static void trim(const char **s, size_t *n) {
    while (*n && is_space((unsigned char)**s)) (*s)++, (*n)--;
    while (*n && is_space((unsigned char)(*s)[*n - 1])) (*n)--;
}

/* ---------------------------------------------------------------- units and types */
static JSValue units(JSContext *ctx, JSValueConst self, int argc, JSValueConst *argv) {
    (void)self; (void)argc; (void)argv;
    JSValue list = JS_NewArray(ctx);
    for (int i = 0; i < css_unit_count(); i++) {
        const char *name;
        int base;
        bool relative;
        double factor;
        css_unit_info(i, &name, &base, &relative, &factor);
        JSValue row = JS_NewArray(ctx);
        JS_SetPropertyUint32(ctx, row, 0, JS_NewString(ctx, name));
        JS_SetPropertyUint32(ctx, row, 1, JS_NewInt32(ctx, base));
        JS_SetPropertyUint32(ctx, row, 2, JS_NewBool(ctx, relative));
        JS_SetPropertyUint32(ctx, row, 3, JS_NewFloat64(ctx, factor));
        JS_SetPropertyUint32(ctx, list, (uint32_t)i, row);
    }
    return list;
}
static bool type_from(JSContext *ctx, JSValueConst v, struct css_calc_type *t) {
    for (int i = 0; i <= CSS_TYPE_BASES; i++) {
        int32_t x;
        JSValue e = JS_GetPropertyUint32(ctx, v, (uint32_t)i);
        int bad = JS_ToInt32(ctx, &x, e);
        JS_FreeValue(ctx, e);
        if (bad) return false;
        if (x < -127 || x > 127 || (i == CSS_TYPE_BASES && (x < -1 || x >= CSS_TYPE_BASES))) return false;
        if (i < CSS_TYPE_BASES) t->e[i] = (int8_t)x;
        else t->hint = (int8_t)x;
    }
    return true;
}
static JSValue type_op(JSContext *ctx, JSValueConst self, int argc, JSValueConst *argv) {
    (void)self;
    int32_t op;
    struct css_calc_type a, b, out;
    if (argc < 3 || JS_ToInt32(ctx, &op, argv[0])) return JS_EXCEPTION;
    if (!type_from(ctx, argv[1], &a) || !type_from(ctx, argv[2], &b)) return JS_HasException(ctx) ? JS_EXCEPTION : JS_NULL;
    if (!(op ? css_calc_type_multiply(&a, &b, &out) : css_calc_type_add(&a, &b, &out))) return JS_NULL;
    JSValue r = JS_NewArray(ctx);
    for (int i = 0; i < CSS_TYPE_BASES; i++) JS_SetPropertyUint32(ctx, r, (uint32_t)i, JS_NewInt32(ctx, out.e[i]));
    JS_SetPropertyUint32(ctx, r, CSS_TYPE_BASES, JS_NewInt32(ctx, out.hint));
    return r;
}
static JSValue type_match(JSContext *ctx, JSValueConst self, int argc, JSValueConst *argv) {
    (void)self;
    uint32_t accept;
    struct css_calc_type t;
    if (argc < 2 || JS_ToUint32(ctx, &accept, argv[1])) return JS_EXCEPTION;
    if (!type_from(ctx, argv[0], &t)) return JS_HasException(ctx) ? JS_EXCEPTION : JS_NewInt32(ctx, 0);
    return JS_NewUint32(ctx, css_calc_type_match(&t, accept));
}
static JSValue number_text(JSContext *ctx, JSValueConst self, int argc, JSValueConst *argv) {
    (void)self;
    double v;
    if (argc < 1 || JS_ToFloat64(ctx, &v, argv[0])) return JS_EXCEPTION;
    sbuf b = {0};
    css_number_serialize(&b, v);
    JSValue r = JS_NewStringLen(ctx, b.p ? b.p : "", b.n);
    sb_free(&b);
    return r;
}

/* ---------------------------------------------------------------- numeric values */
static JSValue leaf(JSContext *ctx, double v, int unit) {
    JSValue r = JS_NewArray(ctx);
    JS_SetPropertyUint32(ctx, r, 0, JS_NewFloat64(ctx, v));
    JS_SetPropertyUint32(ctx, r, 1, JS_NewInt32(ctx, unit));
    return r;
}
static JSValue tree(JSContext *ctx, const struct css_calc *c) {
    static const char *const names[] = {[CSS_MATH_SUM] = "sum", [CSS_MATH_PRODUCT] = "product",
        [CSS_MATH_NEGATE] = "negate", [CSS_MATH_INVERT] = "invert", [CSS_MATH_MIN] = "min", [CSS_MATH_MAX] = "max",
        [CSS_MATH_CLAMP] = "clamp"};
    double v = 0;
    int unit = 0;
    unsigned n;
    struct css_calc *const *k;
    int shape = css_calc_shape(c, &v, &unit, &n, &k);
    if (shape == CSS_MATH_VALUE) return leaf(ctx, v, unit);
    if (shape == CSS_MATH_OTHER) return JS_FALSE;
    JSValue r = JS_NewArray(ctx);
    JS_SetPropertyUint32(ctx, r, 0, JS_NewString(ctx, names[shape]));
    for (unsigned i = 0; i < n; i++) {
        JSValue child = k[i] ? tree(ctx, k[i]) : JS_FALSE;
        if (JS_IsBool(child)) { JS_FreeValue(ctx, r); return JS_FALSE; }
        JS_SetPropertyUint32(ctx, r, i + 1, child);
    }
    return r;
}
static JSValue math(JSContext *ctx, JSValueConst self, int argc, JSValueConst *argv) {
    (void)self;
    size_t n;
    const char *text = text_arg(ctx, argc, argv, 0, &n);
    uint32_t accept = CSS_CALC_ANY;
    if (!text) return JS_EXCEPTION;
    if (argc > 1 && !JS_IsUndefined(argv[1]) && JS_ToUint32(ctx, &accept, argv[1])) { JS_FreeCString(ctx, text); return JS_EXCEPTION; }
    const char *s = text;
    trim(&s, &n);
    JSValue result = JS_NULL;
    const char *p = s;
    double v;
    const char *u;
    size_t un;
    if (css_numeric_token(&p, s + n, &v, &u, &un)) {
        int unit = p == s + n ? css_unit_find(u, un) : -1;
        if (unit >= 0) result = leaf(ctx, v, unit);
    } else if (css_calc_is_math(s, n)) {
        arena_t a = {.chunk_size = 4096};
        jmp_buf trap;
        a.trap = &trap;
        sbuf simplified = {0};
        if (!setjmp(trap)) {
            /* the specified value (§10.10.1), whose serialization (§10.13) Typed OM reads as written */
            struct css_calc_parse P = {.a = &a};
            unsigned kind;
            struct css_calc *c = css_calc_read(&P, accept ? accept : CSS_CALC_ANY, s, n, &kind);
            if (c && css_calc_serialize(&simplified, c, NULL)) {
                struct css_calc_parse R = {.a = &a, .raw = true};
                struct css_calc *r = css_calc_read(&R, CSS_CALC_ANY, simplified.p, simplified.n, &kind);
                if (r) result = tree(ctx, r);
                else if (R.unrepresentable) result = JS_FALSE;
            } else if (P.unrepresentable) result = JS_FALSE;
        } else result = JS_ThrowOutOfMemory(ctx);
        sb_free(&simplified);
        ar_free(&a);
    }
    JS_FreeCString(ctx, text);
    return result;
}

/* ---------------------------------------------------------------- properties */
static JSValue property(JSContext *ctx, JSValueConst self, int argc, JSValueConst *argv) {
    (void)self;
    size_t n;
    const char *name = text_arg(ctx, argc, argv, 0, &n);
    if (!name) return JS_EXCEPTION;
    struct css_reify r;
    bool known = css_prop_reify(name, n, &r);
    JS_FreeCString(ctx, name);
    if (!known) return JS_NULL;
    JSValue out = JS_NewArray(ctx);
    JS_SetPropertyUint32(ctx, out, 0, JS_NewInt32(ctx, r.kind));
    JS_SetPropertyUint32(ctx, out, 1, JS_NewBool(ctx, r.shorthand));
    JS_SetPropertyUint32(ctx, out, 2, JS_NewBool(ctx, r.list));
    JS_SetPropertyUint32(ctx, out, 3, JS_NewUint32(ctx, r.accept));
    JS_SetPropertyUint32(ctx, out, 4, JS_NewBool(ctx, r.typed));
    JS_SetPropertyUint32(ctx, out, 5, JS_NewBool(ctx, r.integer));
    return out;
}
static JSValue valid(JSContext *ctx, JSValueConst self, int argc, JSValueConst *argv) {
    (void)self;
    size_t pn, vn;
    const char *name = text_arg(ctx, argc, argv, 0, &pn);
    if (!name) return JS_EXCEPTION;
    const char *value = text_arg(ctx, argc, argv, 1, &vn);
    if (!value) { JS_FreeCString(ctx, name); return JS_EXCEPTION; }
    bool ok = css_valid_declaration(name, pn, value, vn);
    JS_FreeCString(ctx, name);
    JS_FreeCString(ctx, value);
    return JS_NewBool(ctx, ok);
}

/* ---------------------------------------------------------------- tokens (CSS Syntax, Lexbor) */
struct scan {
    lxb_css_syntax_tokenizer_t *tkz;
    const char *s;
    size_t n;
};
static bool scan_open(struct scan *sc, const char *s, size_t n) {
    sc->s = s;
    sc->n = n;
    sc->tkz = lxb_css_syntax_tokenizer_create();
    if (!sc->tkz || lxb_css_syntax_tokenizer_init(sc->tkz) != LXB_STATUS_OK) {
        if (sc->tkz) lxb_css_syntax_tokenizer_destroy(sc->tkz);
        sc->tkz = NULL;
        return false;
    }
    sc->tkz->with_comment = true;
    lxb_css_syntax_tokenizer_buffer_set(sc->tkz, (const lxb_char_t *)s, n);
    return true;
}
static void scan_close(struct scan *sc) {
    if (sc->tkz) lxb_css_syntax_tokenizer_destroy(sc->tkz);
    sc->tkz = NULL;
}
/* the next token (EOF at the end); its source run is [*at, *end) */
static const lxb_css_syntax_token_t *scan_next(struct scan *sc, size_t *at, size_t *end) {
    const lxb_css_syntax_token_t *t = lxb_css_syntax_token(sc->tkz);
    if (!t) return NULL;
    const lxb_css_syntax_token_base_t *b = lxb_css_syntax_token_base(t);
    *at = (size_t)((const char *)b->begin - sc->s);
    *end = *at + b->length;
    if (*at > sc->n) *at = sc->n;
    if (*end > sc->n) *end = sc->n;
    return t;
}
static bool opens(lxb_css_syntax_token_type_t type) {
    return type == LXB_CSS_SYNTAX_TOKEN_FUNCTION || type == LXB_CSS_SYNTAX_TOKEN_L_PARENTHESIS ||
           type == LXB_CSS_SYNTAX_TOKEN_LS_BRACKET || type == LXB_CSS_SYNTAX_TOKEN_LC_BRACKET;
}
static bool closes(lxb_css_syntax_token_type_t type) {
    return type == LXB_CSS_SYNTAX_TOKEN_R_PARENTHESIS || type == LXB_CSS_SYNTAX_TOKEN_RS_BRACKET ||
           type == LXB_CSS_SYNTAX_TOKEN_RC_BRACKET;
}
static bool name_is(const lxb_css_syntax_token_t *t, const char *name) {
    const lxb_css_syntax_token_string_t *str = lxb_css_syntax_token_ident(t);
    size_t k = strlen(name);
    if (str->length != k) return false;
    for (size_t i = 0; i < k; i++) if (lower(str->data[i]) != name[i]) return false;
    return true;
}
/* skips past the block an opening token began: the source offset after its closing token (the end of
   the input closes what is open, CSS Syntax §5.4.8) */
static bool skip_block(struct scan *sc, size_t *after) {
    unsigned depth = 1;
    for (;;) {
        size_t at, end;
        const lxb_css_syntax_token_t *t = scan_next(sc, &at, &end);
        if (!t) return false;
        if (t->type == LXB_CSS_SYNTAX_TOKEN__EOF) { *after = sc->n; return true; }
        lxb_css_syntax_token_type_t type = t->type;
        lxb_css_syntax_token_consume(sc->tkz);
        if (opens(type)) depth++;
        else if (closes(type) && !--depth) { *after = end; return true; }
    }
}

/* CSS Typed OM §5.3 "reify a list of component values" and §5.4 "reify a var() reference" */
#define SEGMENT_DEPTH 32
static JSValue segments_of(JSContext *ctx, const char *s, size_t n, unsigned depth);
static bool push_text(JSContext *ctx, JSValue list, uint32_t *count, const char *s, size_t n) {
    if (!n) return true;
    return JS_SetPropertyUint32(ctx, list, (*count)++, JS_NewStringLen(ctx, s, n)) >= 0;
}
static JSValue segments_of(JSContext *ctx, const char *s, size_t n, unsigned depth) {
    if (depth > SEGMENT_DEPTH) return JS_ThrowRangeError(ctx, "var() fallbacks nested too deeply");
    struct scan sc;
    if (!scan_open(&sc, s, n)) return JS_ThrowOutOfMemory(ctx);
    JSValue list = JS_NewArray(ctx);
    uint32_t count = 0;
    size_t run = 0;
    bool ok = true;
    for (;;) {
        size_t at, end;
        const lxb_css_syntax_token_t *t = scan_next(&sc, &at, &end);
        if (!t) { ok = false; break; }
        if (t->type == LXB_CSS_SYNTAX_TOKEN__EOF) break;
        bool is_var = t->type == LXB_CSS_SYNTAX_TOKEN_FUNCTION && name_is(t, "var");
        lxb_css_syntax_token_consume(sc.tkz);
        if (!is_var) continue;
        /* var( <custom-property-name> [, <declaration-value>?]? ) */
        size_t fallback_at = 0, close = n, after = n;
        bool fallback = false;
        JSValue name = JS_UNDEFINED;
        for (;;) {
            size_t a, e;
            const lxb_css_syntax_token_t *x = scan_next(&sc, &a, &e);
            if (!x) { ok = false; break; }
            if (x->type == LXB_CSS_SYNTAX_TOKEN__EOF) { close = after = n; break; }
            lxb_css_syntax_token_type_t type = x->type;
            if (type == LXB_CSS_SYNTAX_TOKEN_IDENT && JS_IsUndefined(name) && !fallback) {
                /* the name's serialization: the identifier's value, escapes resolved */
                const lxb_css_syntax_token_string_t *str = lxb_css_syntax_token_ident(x);
                name = JS_NewStringLen(ctx, (const char *)str->data, str->length);
            }
            lxb_css_syntax_token_consume(sc.tkz);
            if (type == LXB_CSS_SYNTAX_TOKEN_COMMA && !fallback) { fallback = true; fallback_at = e; continue; }
            if (type == LXB_CSS_SYNTAX_TOKEN_R_PARENTHESIS) { close = a; after = e; break; }
            if (opens(type)) {
                size_t past;
                if (!skip_block(&sc, &past)) { ok = false; break; }
                if (past == n) { close = after = n; break; }
            }
        }
        if (!ok || !push_text(ctx, list, &count, s + run, at - run)) { JS_FreeValue(ctx, name); ok = false; break; }
        JSValue ref = JS_NewArray(ctx);
        JS_SetPropertyUint32(ctx, ref, 0, JS_IsUndefined(name) ? JS_NewString(ctx, "") : name);
        JSValue inner = fallback ? segments_of(ctx, s + fallback_at, close - fallback_at, depth + 1) : JS_NULL;
        if (JS_IsException(inner)) { JS_FreeValue(ctx, ref); ok = false; break; }
        JS_SetPropertyUint32(ctx, ref, 1, inner);
        JS_SetPropertyUint32(ctx, list, count++, ref);
        run = after;
    }
    scan_close(&sc);
    if (ok && !push_text(ctx, list, &count, s + run, n - run)) ok = false;
    if (!ok) {
        JS_FreeValue(ctx, list);
        return JS_HasException(ctx) ? JS_EXCEPTION : JS_ThrowOutOfMemory(ctx);
    }
    return list;
}
static JSValue segments(JSContext *ctx, JSValueConst self, int argc, JSValueConst *argv) {
    (void)self;
    size_t n;
    const char *text = text_arg(ctx, argc, argv, 0, &n);
    if (!text) return JS_EXCEPTION;
    JSValue r = segments_of(ctx, text, n, 0);
    JS_FreeCString(ctx, text);
    return r;
}

/* CSS Syntax §9 "Serialization": which token pairs would tokenize as something else when written one
   after the other. The classes of the table's rows and columns: */
enum { K_NONE, K_IDENT, K_FUNCTION, K_URL, K_AT, K_HASH, K_NUMBER, K_PERCENT, K_DIMENSION, K_CDC, K_PAREN,
       K_MINUS, K_HASH_DELIM, K_AT_DELIM, K_DOT, K_PLUS, K_SLASH, K_STAR, K_PERCENT_DELIM };
static int token_class(const lxb_css_syntax_token_t *t) {
    switch (t->type) {
    case LXB_CSS_SYNTAX_TOKEN_IDENT: return K_IDENT;
    case LXB_CSS_SYNTAX_TOKEN_FUNCTION: return K_FUNCTION;
    case LXB_CSS_SYNTAX_TOKEN_URL: case LXB_CSS_SYNTAX_TOKEN_BAD_URL: return K_URL;
    case LXB_CSS_SYNTAX_TOKEN_AT_KEYWORD: return K_AT;
    case LXB_CSS_SYNTAX_TOKEN_HASH: return K_HASH;
    case LXB_CSS_SYNTAX_TOKEN_NUMBER: return K_NUMBER;
    case LXB_CSS_SYNTAX_TOKEN_PERCENTAGE: return K_PERCENT;
    case LXB_CSS_SYNTAX_TOKEN_DIMENSION: return K_DIMENSION;
    case LXB_CSS_SYNTAX_TOKEN_CDC: return K_CDC;
    case LXB_CSS_SYNTAX_TOKEN_L_PARENTHESIS: return K_PAREN;
    case LXB_CSS_SYNTAX_TOKEN_DELIM:
        switch (lxb_css_syntax_token_delim_char(t)) {
        case '-': return K_MINUS;
        case '#': return K_HASH_DELIM;
        case '@': return K_AT_DELIM;
        case '.': return K_DOT;
        case '+': return K_PLUS;
        case '/': return K_SLASH;
        case '*': return K_STAR;
        case '%': return K_PERCENT_DELIM;
        }
        return K_NONE;
    default: return K_NONE;
    }
}
static bool needs_comment(int left, int right) {
    unsigned r = 1u << right;
    const unsigned words = 1u << K_IDENT | 1u << K_FUNCTION | 1u << K_URL;
    const unsigned numbers = 1u << K_NUMBER | 1u << K_PERCENT | 1u << K_DIMENSION;
    switch (left) {
    case K_IDENT: return r & (words | numbers | 1u << K_MINUS | 1u << K_CDC | 1u << K_PAREN);
    case K_AT: case K_HASH: case K_DIMENSION: return r & (words | numbers | 1u << K_MINUS | 1u << K_CDC);
    case K_HASH_DELIM: case K_MINUS: return r & (words | numbers | 1u << K_MINUS);
    case K_NUMBER: return r & (words | numbers | 1u << K_PERCENT_DELIM);
    case K_AT_DELIM: return r & (words | 1u << K_MINUS);
    case K_DOT: case K_PLUS: return r & numbers;
    case K_SLASH: return r & 1u << K_STAR;
    }
    return false;
}
/* the class of the last (last) or first token of s; K_NONE for whitespace, comments or nothing */
static int edge_class(const char *s, size_t n, bool last, bool *ok) {
    struct scan sc;
    if (!scan_open(&sc, s, n)) { *ok = false; return K_NONE; }
    int k = K_NONE;
    for (;;) {
        size_t at, end;
        const lxb_css_syntax_token_t *t = scan_next(&sc, &at, &end);
        if (!t) { *ok = false; break; }
        if (t->type == LXB_CSS_SYNTAX_TOKEN__EOF) break;
        k = token_class(t);
        lxb_css_syntax_token_consume(sc.tkz);
        if (!last) break;
    }
    scan_close(&sc);
    return k;
}
static JSValue boundary(JSContext *ctx, JSValueConst self, int argc, JSValueConst *argv) {
    (void)self;
    size_t ln, rn;
    const char *left = text_arg(ctx, argc, argv, 0, &ln);
    if (!left) return JS_EXCEPTION;
    const char *right = text_arg(ctx, argc, argv, 1, &rn);
    if (!right) { JS_FreeCString(ctx, left); return JS_EXCEPTION; }
    bool ok = true;
    int a = edge_class(left, ln, true, &ok), b = edge_class(right, rn, false, &ok);
    JS_FreeCString(ctx, left);
    JS_FreeCString(ctx, right);
    if (!ok) return JS_ThrowOutOfMemory(ctx);
    return JS_NewBool(ctx, needs_comment(a, b));
}

/* the top-level component values of a value (CSS Syntax §5.4.9) */
static JSValue part(JSContext *ctx, const char *kind, const char *s, size_t n) {
    JSValue r = JS_NewArray(ctx);
    JS_SetPropertyUint32(ctx, r, 0, JS_NewString(ctx, kind));
    if (s) JS_SetPropertyUint32(ctx, r, 1, JS_NewStringLen(ctx, s, n));
    return r;
}
static JSValue function_arguments(JSContext *ctx, const char *s, size_t n, bool *ok) {
    struct scan sc;
    JSValue list = JS_NewArray(ctx);
    if (!scan_open(&sc, s, n)) { *ok = false; return list; }
    uint32_t count = 0;
    size_t from = 0;
    for (;;) {
        size_t at, end;
        const lxb_css_syntax_token_t *t = scan_next(&sc, &at, &end);
        if (!t) { *ok = false; break; }
        bool eof = t->type == LXB_CSS_SYNTAX_TOKEN__EOF;
        lxb_css_syntax_token_type_t type = t->type;
        if (!eof) lxb_css_syntax_token_consume(sc.tkz);
        if (eof || type == LXB_CSS_SYNTAX_TOKEN_COMMA) {
            const char *a = s + from;
            size_t k = (eof ? n : at) - from;
            trim(&a, &k);
            if (!eof || k || count) JS_SetPropertyUint32(ctx, list, count++, JS_NewStringLen(ctx, a, k));
            from = end;
            if (eof) break;
        } else if (opens(type)) {
            size_t past;
            if (!skip_block(&sc, &past)) { *ok = false; break; }
        }
    }
    scan_close(&sc);
    return list;
}
static JSValue components(JSContext *ctx, JSValueConst self, int argc, JSValueConst *argv) {
    (void)self;
    size_t n;
    const char *text = text_arg(ctx, argc, argv, 0, &n);
    if (!text) return JS_EXCEPTION;
    struct scan sc;
    if (!scan_open(&sc, text, n)) { JS_FreeCString(ctx, text); return JS_ThrowOutOfMemory(ctx); }
    JSValue list = JS_NewArray(ctx);
    uint32_t count = 0;
    bool ok = true;
    for (;;) {
        size_t at, end;
        const lxb_css_syntax_token_t *t = scan_next(&sc, &at, &end);
        if (!t) { ok = false; break; }
        if (t->type == LXB_CSS_SYNTAX_TOKEN__EOF) break;
        lxb_css_syntax_token_type_t type = t->type;
        JSValue item = JS_UNDEFINED;
        if (type == LXB_CSS_SYNTAX_TOKEN_IDENT) {
            const lxb_css_syntax_token_string_t *str = lxb_css_syntax_token_ident(t);
            item = part(ctx, "ident", (const char *)str->data, str->length);
        } else if (type == LXB_CSS_SYNTAX_TOKEN_FUNCTION) {
            const lxb_css_syntax_token_string_t *str = lxb_css_syntax_token_function(t);
            char name[32];
            size_t k = str->length < sizeof name - 1 ? str->length : sizeof name - 1;
            for (size_t i = 0; i < k; i++) name[i] = (char)lower(str->data[i]);
            name[k] = 0;
            lxb_css_syntax_token_consume(sc.tkz);
            size_t past;
            if (!skip_block(&sc, &past)) { ok = false; break; }
            size_t inner_end = past > end && text[past - 1] == ')' ? past - 1 : past;
            item = part(ctx, "function", name, k);
            JS_SetPropertyUint32(ctx, item, 2, JS_NewStringLen(ctx, text + at, past - at));
            JS_SetPropertyUint32(ctx, item, 3, function_arguments(ctx, text + end, inner_end - end, &ok));
            JS_SetPropertyUint32(ctx, list, count++, item);
            continue;
        } else if (type == LXB_CSS_SYNTAX_TOKEN_NUMBER || type == LXB_CSS_SYNTAX_TOKEN_PERCENTAGE ||
                   type == LXB_CSS_SYNTAX_TOKEN_DIMENSION) item = part(ctx, "numeric", text + at, end - at);
        else if (type == LXB_CSS_SYNTAX_TOKEN_URL) item = part(ctx, "url", text + at, end - at);
        else if (type == LXB_CSS_SYNTAX_TOKEN_COMMA) item = part(ctx, ",", NULL, 0);
        else if (type != LXB_CSS_SYNTAX_TOKEN_WHITESPACE && type != LXB_CSS_SYNTAX_TOKEN_COMMENT) {
            lxb_css_syntax_token_consume(sc.tkz);
            size_t past = end;
            if (opens(type) && !skip_block(&sc, &past)) { ok = false; break; }
            JS_SetPropertyUint32(ctx, list, count++, part(ctx, "other", text + at, past - at));
            continue;
        }
        lxb_css_syntax_token_consume(sc.tkz);
        if (!JS_IsUndefined(item)) JS_SetPropertyUint32(ctx, list, count++, item);
    }
    scan_close(&sc);
    JS_FreeCString(ctx, text);
    if (!ok) { JS_FreeValue(ctx, list); return JS_ThrowOutOfMemory(ctx); }
    return list;
}

/* ---------------------------------------------------------------- colours */
static JSValue color(JSContext *ctx, JSValueConst self, int argc, JSValueConst *argv) {
    (void)self;
    static const char *const kinds[] = {[CSS_COLOR_REIFY_RGB] = "rgb", [CSS_COLOR_REIFY_HSL] = "hsl",
        [CSS_COLOR_REIFY_HWB] = "hwb", [CSS_COLOR_REIFY_LAB] = "lab", [CSS_COLOR_REIFY_LCH] = "lch",
        [CSS_COLOR_REIFY_OKLAB] = "oklab", [CSS_COLOR_REIFY_OKLCH] = "oklch", [CSS_COLOR_REIFY_COLOR] = "color"};
    size_t n;
    const char *text = text_arg(ctx, argc, argv, 0, &n);
    if (!text) return JS_EXCEPTION;
    struct css_color_reified c;
    sbuf m = {0};
    bool ok = css_color_reify(text, n, &c, &m);
    JS_FreeCString(ctx, text);
    JSValue r = JS_NULL;
    if (ok && c.kind == CSS_COLOR_REIFY_KEYWORD) r = part(ctx, "keyword", c.name, strlen(c.name));
    else if (ok && c.kind == CSS_COLOR_REIFY_OTHER) r = part(ctx, "other", NULL, 0);
    else if (ok) {
        r = JS_NewArray(ctx);
        JS_SetPropertyUint32(ctx, r, 0, JS_NewString(ctx, kinds[c.kind]));
        JS_SetPropertyUint32(ctx, r, 1, c.space ? JS_NewString(ctx, c.space) : JS_NULL);
        for (int i = 0; i < 4; i++) {
            const struct css_color_channel *ch = &c.ch[i];
            JSValue v = JS_NULL;
            if (ch->kind == CSS_COLOR_CHANNEL_NONE) v = JS_NewString(ctx, "none");
            else if (ch->kind == CSS_COLOR_CHANNEL_VALUE) {
                v = JS_NewArray(ctx);
                JS_SetPropertyUint32(ctx, v, 0, JS_NewFloat64(ctx, ch->v));
                JS_SetPropertyUint32(ctx, v, 1, JS_NewString(ctx, ch->unit));
            } else if (ch->kind == CSS_COLOR_CHANNEL_MATH) v = part(ctx, "math", m.p + ch->math_at, ch->math_n);
            JS_SetPropertyUint32(ctx, r, (uint32_t)i + 2, v);
        }
    }
    sb_free(&m);
    return r;
}

void web_js_typed_om_init(JSContext *ctx, JSValue host) {
    static const JSCFunctionListEntry functions[] = {
        JS_CFUNC_DEF("typedUnits", 0, units),
        JS_CFUNC_DEF("typedType", 3, type_op),
        JS_CFUNC_DEF("typedMatch", 2, type_match),
        JS_CFUNC_DEF("typedNumber", 1, number_text),
        JS_CFUNC_DEF("typedMath", 2, math),
        JS_CFUNC_DEF("typedProperty", 1, property),
        JS_CFUNC_DEF("typedValid", 2, valid),
        JS_CFUNC_DEF("typedSegments", 1, segments),
        JS_CFUNC_DEF("typedBoundary", 2, boundary),
        JS_CFUNC_DEF("typedComponents", 1, components),
        JS_CFUNC_DEF("typedColor", 1, color),
    };
    JS_SetPropertyFunctionList(ctx, host, functions, sizeof functions / sizeof *functions);
}
