/* XPath 1.0 parsing and evaluation over the native DOM (see xpath.h).
   Lexical structure, grammar, axes, node tests, the expression semantics and
   the core function library follow REC-xpath-19991116 sections 2 to 4; the
   DOM mapping follows DOM Level 3 XPath 1.2; HTML documents follow HTML
   "Interactions with XPath and XSLT". Evaluation is native and synchronous:
   it never runs script, so the DOM cannot change under it. */
#include "xpath.h"
#include <math.h>
#include <stdio.h>
#include "dtoa.h"

#define XP_NS_HTML "http://www.w3.org/1999/xhtml"
#define XP_NS_SVG "http://www.w3.org/2000/svg"
#define XP_NS_MATHML "http://www.w3.org/1998/Math/MathML"
#define XP_NS_XML "http://www.w3.org/XML/1998/namespace"
#define XP_NS_XMLNS "http://www.w3.org/2000/xmlns/"
/* Native stack the recursive parser and evaluator may use below their entry
   frame; deeper nesting is refused instead of overflowing the thread stack. */
#define XP_STACK_BUDGET (256u * 1024u)

static bool xp_stack_exhausted(const char *base) {
    const char *here = __builtin_frame_address(0);
    return base > here && (size_t)(base - here) > XP_STACK_BUDGET;
}

/* ---------------------------------------------------------------- the DOM's names */
/* An element's namespace, prefix, local name and qualified name as the DOM
   reports them (js.c namespaceURI and localName). Elements carry no DOM prefix
   in this DOM, so an element's qualified name is its local name. */
static const char *xp_element_namespace(const node_t *n) {
    switch (n->namespace_id) {
    case NS_HTML: return XP_NS_HTML;
    case NS_SVG: return XP_NS_SVG;
    case NS_MATHML: return XP_NS_MATHML;
    default: return NULL;
    }
}
static const char *xp_element_prefix(const node_t *n) { (void)n; return NULL; }
static const char *xp_element_local(const node_t *n) { return n->foreign ? n->raw_name : n->name; }
static const char *xp_element_qualified(const node_t *n) { return xp_element_local(n); }
/* DOM "HTML document". Every Document this DOM creates is one: contentType is
   text/html, and DOMImplementation.createDocument and the XML parser are not
   implemented. An XML document's type is to be read here. */
static bool xp_html_document(const web_doc *d) { (void)d; return true; }
/* DOM: names given to an HTML-namespace element of an HTML document, its tag
   and attribute names, are compared in ASCII lowercase. */
static bool xp_html_names(const node_t *n) { return n->namespace_id == NS_HTML && xp_html_document(n->owner); }
static const char *xp_attr_local(const struct attr *a) { return a->local ? a->local : a->raw; }
/* DOM Level 3 XPath 1.2.2: namespace declarations are not attribute nodes. */
static bool xp_namespace_declaration(const struct attr *a) {
    return a->namespace_uri && !strcmp(a->namespace_uri, XP_NS_XMLNS);
}
/* Equal nullable strings: namespaces and namespace prefixes. */
static bool xp_same(const char *a, const char *b) { return a ? b && !strcmp(a, b) : !b; }
static bool xp_name_is(const char *have, const char *want, size_t length) {
    return have && !strncmp(have, want, length) && !have[length];
}

/* ---------------------------------------------------------------- characters */
/* One UTF-8 sequence of s[0..n): its code point and *size; an invalid byte is
   one character of its own (UINT32_MAX). QuickJS strings arrive as WTF-8. */
static uint32_t xp_utf8(const unsigned char *s, size_t n, size_t *size) {
    unsigned c = s[0];
    *size = 1;
    if (c < 0x80) return c;
    size_t length = c >= 0xC2 && c <= 0xDF ? 2 : c >= 0xE0 && c <= 0xEF ? 3 : c >= 0xF0 && c <= 0xF4 ? 4 : 0;
    if (!length || length > n) return UINT32_MAX;
    uint32_t cp = c & (0x7Fu >> length);
    for (size_t i = 1; i < length; i++) {
        if ((s[i] & 0xC0) != 0x80) return UINT32_MAX;
        cp = cp << 6 | (s[i] & 0x3Fu);
    }
    *size = length;
    return cp;
}
static size_t xp_char_size(const char *p, size_t n) {
    size_t size;
    xp_utf8((const unsigned char *)p, n, &size);
    return size;
}
/* ExprWhitespace and the whitespace of normalize-space() and id(): S of XML. */
static bool xp_space(unsigned char c) { return c == ' ' || c == '\t' || c == '\r' || c == '\n'; }
static bool xp_digit(unsigned char c) { return c >= '0' && c <= '9'; }
/* NCName characters: XML NameStartChar and NameChar without ':'. */
static bool xp_name_start(uint32_t c) {
    return (c >= 'A' && c <= 'Z') || c == '_' || (c >= 'a' && c <= 'z') ||
           (c >= 0xC0 && c <= 0xD6) || (c >= 0xD8 && c <= 0xF6) || (c >= 0xF8 && c <= 0x2FF) ||
           (c >= 0x370 && c <= 0x37D) || (c >= 0x37F && c <= 0x1FFF) || (c >= 0x200C && c <= 0x200D) ||
           (c >= 0x2070 && c <= 0x218F) || (c >= 0x2C00 && c <= 0x2FEF) || (c >= 0x3001 && c <= 0xD7FF) ||
           (c >= 0xF900 && c <= 0xFDCF) || (c >= 0xFDF0 && c <= 0xFFFD) || (c >= 0x10000 && c <= 0xEFFFF);
}
static bool xp_name_char(uint32_t c) {
    return xp_name_start(c) || c == '-' || c == '.' || (c >= '0' && c <= '9') || c == 0xB7 ||
           (c >= 0x300 && c <= 0x36F) || (c >= 0x203F && c <= 0x2040);
}
/* The end of the NCName starting at s[i], or i when none starts there. */
static size_t xp_ncname(const char *s, size_t n, size_t i) {
    if (i >= n) return i;
    size_t size;
    if (!xp_name_start(xp_utf8((const unsigned char *)s + i, n - i, &size))) return i;
    size_t j = i + size;
    while (j < n && xp_name_char(xp_utf8((const unsigned char *)s + j, n - j, &size))) j += size;
    return j;
}

/* ---------------------------------------------------------------- numbers */
/* The end of an XPath Number (Digits ('.' Digits?)? | '.' Digits) at s[i], or i. */
static size_t xp_number_end(const char *s, size_t n, size_t i) {
    size_t j = i;
    while (j < n && xp_digit((unsigned char)s[j])) j++;
    bool digits = j > i;
    if (j < n && s[j] == '.') {
        size_t k = j + 1;
        while (k < n && xp_digit((unsigned char)s[k])) k++;
        if (!digits && k == j + 1) return i;
        return k;
    }
    return digits ? j : i;
}
/* The IEEE 754 double nearest the decimal s[0..n), a valid Number. False only
   when a long number found no memory. */
static bool xp_decimal(const char *s, size_t n, bool negative, double *out) {
    char local[96];
    char *text = n + 2 <= sizeof local ? local : malloc(n + 2);
    if (!text) return false;
    size_t k = 0;
    if (negative) text[k++] = '-';
    memcpy(text + k, s, n);
    text[k + n] = 0;
    JSATODTempMem scratch;
    const char *end = NULL;
    *out = js_atod(text, &end, 10, 0, &scratch);
    if (text != local) free(text);
    return true;
}
/* number() of a string: optional whitespace, an optional minus, a Number and
   optional whitespace; anything else is NaN. */
static bool xp_string_number(const char *s, size_t n, double *out) {
    size_t i = 0;
    while (i < n && xp_space((unsigned char)s[i])) i++;
    bool negative = i < n && s[i] == '-';
    if (negative) i++;
    size_t end = xp_number_end(s, n, i), j = end;
    while (j < n && xp_space((unsigned char)s[j])) j++;
    if (end == i || j != n) { *out = NAN; return true; }
    return xp_decimal(s + i, end - i, negative, out);
}
/* round(): the nearest integer, a tie toward positive infinity; -0.5 <= x < 0 is -0. */
static double xp_round(double d) {
    if (isnan(d) || isinf(d) || d == 0) return d;
    if (d < 0 && d >= -0.5) return -0.0;
    double r = floor(d);
    return d - r >= 0.5 ? r + 1 : r;
}

/* ---------------------------------------------------------------- strings */
/* A string, borrowed (own NULL) from the expression, the DOM or a constant, or
   owning the allocation own (p may point into it). */
struct xs { const char *p; size_t n; char *own; };
static struct xs xs_borrow(const char *p, size_t n) { return (struct xs){p ? p : "", p ? n : 0, NULL}; }
static bool xs_equal(const struct xs *a, const struct xs *b) { return a->n == b->n && !memcmp(a->p, b->p, a->n); }
struct xbuf { char *p; size_t n, cap; };

/* string() of a number: no exponent and only as many digits as identify the double. */
static bool xp_number_string(double d, struct xs *out) {
    if (isnan(d)) { *out = xs_borrow("NaN", 3); return true; }
    if (isinf(d)) { *out = d > 0 ? xs_borrow("Infinity", 8) : xs_borrow("-Infinity", 9); return true; }
    if (d == 0) { *out = xs_borrow("0", 1); return true; }
    int flags = JS_DTOA_FORMAT_FREE | JS_DTOA_EXP_DISABLED;
    int max = js_dtoa_max_len(d, 10, 0, flags);
    char *p = malloc((size_t)max + 1);
    if (!p) return false;
    JSDTOATempMem scratch;
    int length = js_dtoa(p, d, 10, 0, flags, &scratch);
    p[length] = 0;
    *out = (struct xs){p, (size_t)length, p};
    return true;
}

/* ---------------------------------------------------------------- tokens */
enum xp_token {
    K_END, K_LPAREN, K_RPAREN, K_LBRACKET, K_RBRACKET, K_DOT, K_DOTDOT, K_AT, K_COMMA, K_COLONCOLON,
    K_NAME,     /* NameTest: '*' (no prefix or local), prefix ':' '*' (no local) or a QName */
    K_NODETYPE, /* NodeType before '(' */
    K_FUNCTION, /* FunctionName before '(' */
    K_AXIS,     /* AxisName before '::' */
    K_LITERAL, K_NUMBER, K_VARIABLE,
    /* Operator: K_AND and every kind after it. */
    K_AND, K_OR, K_MOD, K_DIV, K_MUL, K_SLASH, K_DSLASH, K_PIPE, K_PLUS, K_MINUS,
    K_EQ, K_NE, K_LT, K_LE, K_GT, K_GE
};
struct xp_tok {
    uint8_t kind;
    size_t offset;
    const char *prefix, *local; /* into the source */
    size_t prefix_length, local_length;
    double number;
};
struct xp_tokens { struct xp_tok *v; size_t n, cap; };

static bool xp_token_push(struct xp_tokens *t, const struct xp_tok *token) {
    if (t->n == t->cap) {
        size_t cap = t->cap ? t->cap * 2 : 32;
        if (cap > SIZE_MAX / sizeof *t->v) return false;
        struct xp_tok *v = realloc(t->v, cap * sizeof *v);
        if (!v) return false;
        t->v = v; t->cap = cap;
    }
    t->v[t->n++] = *token;
    return true;
}
/* Section 3.7's first rule: with a preceding token that is not @, ::, (, [, ','
   or an Operator, '*' is the MultiplyOperator and an NCName an OperatorName. */
static bool xp_operand_expected(const struct xp_tokens *t) {
    if (!t->n) return true;
    uint8_t k = t->v[t->n - 1].kind;
    return k == K_AT || k == K_COLONCOLON || k == K_LPAREN || k == K_LBRACKET || k == K_COMMA || k >= K_AND;
}
static bool xp_node_type(const char *s, size_t n) {
    return (n == 7 && !memcmp(s, "comment", 7)) || (n == 4 && !memcmp(s, "text", 4)) ||
           (n == 4 && !memcmp(s, "node", 4)) || (n == 22 && !memcmp(s, "processing-instruction", 22));
}
static enum xpath_status xp_lex_error(char *detail, size_t size, const char *what, size_t offset) {
    if (size) snprintf(detail, size, "%s at offset %zu", what, offset);
    return XPATH_SYNTAX;
}
static enum xpath_status xp_lex(const char *s, size_t n, struct xp_tokens *out, char *detail, size_t detail_size) {
    size_t i = 0;
    for (;;) {
        while (i < n && xp_space((unsigned char)s[i])) i++;
        struct xp_tok t = {.offset = i};
        if (i == n) {
            t.kind = K_END;
            return xp_token_push(out, &t) ? XPATH_OK : XPATH_OOM;
        }
        unsigned char c = (unsigned char)s[i];
        bool operand = xp_operand_expected(out);
        if (xp_digit(c) || (c == '.' && i + 1 < n && xp_digit((unsigned char)s[i + 1]))) {
            size_t end = xp_number_end(s, n, i);
            if (!xp_decimal(s + i, end - i, false, &t.number)) return XPATH_OOM;
            t.kind = K_NUMBER; i = end;
        } else switch (c) {
        case '(': t.kind = K_LPAREN; i++; break;
        case ')': t.kind = K_RPAREN; i++; break;
        case '[': t.kind = K_LBRACKET; i++; break;
        case ']': t.kind = K_RBRACKET; i++; break;
        case '@': t.kind = K_AT; i++; break;
        case ',': t.kind = K_COMMA; i++; break;
        case '|': t.kind = K_PIPE; i++; break;
        case '+': t.kind = K_PLUS; i++; break;
        case '-': t.kind = K_MINUS; i++; break;
        case '=': t.kind = K_EQ; i++; break;
        case '!':
            if (i + 1 < n && s[i + 1] == '=') { t.kind = K_NE; i += 2; break; }
            return xp_lex_error(detail, detail_size, "'!' without '='", i);
        case '<':
            if (i + 1 < n && s[i + 1] == '=') { t.kind = K_LE; i += 2; } else { t.kind = K_LT; i++; }
            break;
        case '>':
            if (i + 1 < n && s[i + 1] == '=') { t.kind = K_GE; i += 2; } else { t.kind = K_GT; i++; }
            break;
        case '/':
            if (i + 1 < n && s[i + 1] == '/') { t.kind = K_DSLASH; i += 2; } else { t.kind = K_SLASH; i++; }
            break;
        case ':':
            if (i + 1 < n && s[i + 1] == ':') { t.kind = K_COLONCOLON; i += 2; break; }
            return xp_lex_error(detail, detail_size, "unexpected ':'", i);
        case '.':
            if (i + 1 < n && s[i + 1] == '.') { t.kind = K_DOTDOT; i += 2; } else { t.kind = K_DOT; i++; }
            break;
        case '*': t.kind = operand ? K_NAME : K_MUL; i++; break;
        case '"': case '\'': {
            const char *close = memchr(s + i + 1, c, n - i - 1);
            if (!close) return xp_lex_error(detail, detail_size, "unterminated literal", i);
            t.kind = K_LITERAL;
            t.local = s + i + 1; t.local_length = (size_t)(close - (s + i + 1));
            i = (size_t)(close - s) + 1;
            break;
        }
        case '$': {
            size_t start = i + 1, end = xp_ncname(s, n, start);
            if (end == start) return xp_lex_error(detail, detail_size, "'$' without a variable name", i);
            if (end + 1 < n && s[end] == ':') {
                size_t local = xp_ncname(s, n, end + 1);
                if (local > end + 1) end = local;
            }
            t.kind = K_VARIABLE; t.local = s + start; t.local_length = end - start; i = end;
            break;
        }
        default: {
            size_t end = xp_ncname(s, n, i);
            if (end == i) return xp_lex_error(detail, detail_size, "unexpected character", i);
            if (!operand) {
                size_t length = end - i;
                if (length == 3 && !memcmp(s + i, "and", 3)) t.kind = K_AND;
                else if (length == 2 && !memcmp(s + i, "or", 2)) t.kind = K_OR;
                else if (length == 3 && !memcmp(s + i, "mod", 3)) t.kind = K_MOD;
                else if (length == 3 && !memcmp(s + i, "div", 3)) t.kind = K_DIV;
                else return xp_lex_error(detail, detail_size, "expected an operator", i);
                i = end;
                break;
            }
            t.local = s + i; t.local_length = end - i;
            if (end + 1 < n && s[end] == ':' && s[end + 1] == '*') {
                t.prefix = t.local; t.prefix_length = t.local_length;
                t.local = NULL; t.local_length = 0;
                end += 2;
            } else if (end + 1 < n && s[end] == ':' && s[end + 1] != ':') {
                size_t local = xp_ncname(s, n, end + 1);
                if (local == end + 1) return xp_lex_error(detail, detail_size, "expected a local name after ':'", end);
                t.prefix = t.local; t.prefix_length = t.local_length;
                t.local = s + end + 1; t.local_length = local - end - 1;
                end = local;
            }
            i = end;
            size_t j = i;
            while (j < n && xp_space((unsigned char)s[j])) j++;
            if (t.local && j < n && s[j] == '(')
                t.kind = !t.prefix && xp_node_type(t.local, t.local_length) ? K_NODETYPE : K_FUNCTION;
            else if (t.local && !t.prefix && j + 1 < n && s[j] == ':' && s[j + 1] == ':') t.kind = K_AXIS;
            else t.kind = K_NAME;
            break;
        }
        }
        if (!xp_token_push(out, &t)) return XPATH_OOM;
    }
}

/* ---------------------------------------------------------------- expressions */
enum xp_op {
    X_OR, X_AND, X_EQ, X_NE, X_LT, X_LE, X_GT, X_GE, X_ADD, X_SUB, X_MUL, X_DIV, X_MOD,
    X_NEG, X_UNION, X_LITERAL, X_NUMBER, X_FUNCTION, X_PATH, X_FILTER
};
/* Every XPath 1.0 expression has a static type: no variables are bound, and
   each core function has one result type. */
enum xp_type { XT_NODESET, XT_BOOLEAN, XT_NUMBER, XT_STRING };
enum xp_axis {
    A_ANCESTOR, A_ANCESTOR_OR_SELF, A_ATTRIBUTE, A_CHILD, A_DESCENDANT, A_DESCENDANT_OR_SELF,
    A_FOLLOWING, A_FOLLOWING_SIBLING, A_NAMESPACE, A_PARENT, A_PRECEDING, A_PRECEDING_SIBLING, A_SELF
};
static const char *const xp_axis_names[] = {
    "ancestor", "ancestor-or-self", "attribute", "child", "descendant", "descendant-or-self",
    "following", "following-sibling", "namespace", "parent", "preceding", "preceding-sibling", "self"
};
enum xp_test { NT_NAME, NT_ANY, NT_NS_ANY, NT_NODE, NT_TEXT, NT_COMMENT, NT_PI };
enum xp_fn {
    F_LAST, F_POSITION, F_COUNT, F_ID, F_LOCAL_NAME, F_NAMESPACE_URI, F_NAME, F_STRING, F_CONCAT,
    F_STARTS_WITH, F_CONTAINS, F_SUBSTRING_BEFORE, F_SUBSTRING_AFTER, F_SUBSTRING, F_STRING_LENGTH,
    F_NORMALIZE_SPACE, F_TRANSLATE, F_BOOLEAN, F_NOT, F_TRUE, F_FALSE, F_LANG, F_NUMBER, F_SUM,
    F_FLOOR, F_CEILING, F_ROUND, F_COUNT_OF
};
/* The core function library (section 4): arity (max -1: any), result type,
   and whether the argument must be a node-set. */
static const struct xp_function {
    const char *name;
    int min, max;
    uint8_t result;
    bool node_set;
} xp_functions[F_COUNT_OF] = {
    [F_LAST] = {"last", 0, 0, XT_NUMBER, false},
    [F_POSITION] = {"position", 0, 0, XT_NUMBER, false},
    [F_COUNT] = {"count", 1, 1, XT_NUMBER, true},
    [F_ID] = {"id", 1, 1, XT_NODESET, false},
    [F_LOCAL_NAME] = {"local-name", 0, 1, XT_STRING, true},
    [F_NAMESPACE_URI] = {"namespace-uri", 0, 1, XT_STRING, true},
    [F_NAME] = {"name", 0, 1, XT_STRING, true},
    [F_STRING] = {"string", 0, 1, XT_STRING, false},
    [F_CONCAT] = {"concat", 2, -1, XT_STRING, false},
    [F_STARTS_WITH] = {"starts-with", 2, 2, XT_BOOLEAN, false},
    [F_CONTAINS] = {"contains", 2, 2, XT_BOOLEAN, false},
    [F_SUBSTRING_BEFORE] = {"substring-before", 2, 2, XT_STRING, false},
    [F_SUBSTRING_AFTER] = {"substring-after", 2, 2, XT_STRING, false},
    [F_SUBSTRING] = {"substring", 2, 3, XT_STRING, false},
    [F_STRING_LENGTH] = {"string-length", 0, 1, XT_NUMBER, false},
    [F_NORMALIZE_SPACE] = {"normalize-space", 0, 1, XT_STRING, false},
    [F_TRANSLATE] = {"translate", 3, 3, XT_STRING, false},
    [F_BOOLEAN] = {"boolean", 1, 1, XT_BOOLEAN, false},
    [F_NOT] = {"not", 1, 1, XT_BOOLEAN, false},
    [F_TRUE] = {"true", 0, 0, XT_BOOLEAN, false},
    [F_FALSE] = {"false", 0, 0, XT_BOOLEAN, false},
    [F_LANG] = {"lang", 1, 1, XT_BOOLEAN, false},
    [F_NUMBER] = {"number", 0, 1, XT_NUMBER, false},
    [F_SUM] = {"sum", 1, 1, XT_NUMBER, true},
    [F_FLOOR] = {"floor", 1, 1, XT_NUMBER, false},
    [F_CEILING] = {"ceiling", 1, 1, XT_NUMBER, false},
    [F_ROUND] = {"round", 1, 1, XT_NUMBER, false},
};

struct xp_expr;
struct xp_step {
    uint8_t axis, test;
    bool prefixed;              /* the name test has a prefix; ns is its binding (NULL: no namespace) */
    const char *ns;
    const char *local, *lower;  /* NT_NAME: the local name and its ASCII lowercase; NT_PI: the target or NULL */
    size_t local_length;
    struct xp_expr **predicates;
    int npredicates;
};
struct xp_expr {
    uint8_t op, type, fn;
    struct xp_expr *left, *right;  /* operands; X_FILTER: left is the primary expression */
    struct xp_expr **items;        /* X_FUNCTION arguments, X_FILTER predicates */
    int count;
    const char *text;              /* X_LITERAL */
    size_t length;
    double number;                 /* X_NUMBER */
    struct xp_expr *start;         /* X_PATH: the filter expression it continues, or NULL */
    bool absolute;                 /* X_PATH without start: from the root node */
    struct xp_step **steps;
    int nsteps;
};
struct xpath_expr {
    arena_t arena;
    struct xp_expr *root;
};

/* ---------------------------------------------------------------- parser */
struct xp_parser {
    const struct xp_tok *tok;
    size_t at;
    arena_t *arena;
    xpath_resolve_fn resolve;
    void *opaque;
    enum xpath_status status;
    char *detail;
    size_t detail_size;
    const char *stack_base;
};
struct xp_vec { void **v; size_t n, cap; };

static const struct xp_tok *xp_peek(const struct xp_parser *p) { return &p->tok[p->at]; }
static bool xp_accept(struct xp_parser *p, uint8_t kind) {
    if (p->tok[p->at].kind != kind) return false;
    if (kind != K_END) p->at++;
    return true;
}
static void *xp_syntax(struct xp_parser *p, const char *what) {
    if (!p->status) {
        p->status = XPATH_SYNTAX;
        if (p->detail_size) snprintf(p->detail, p->detail_size, "%s at offset %zu", what, p->tok[p->at].offset);
    }
    return NULL;
}
static void *xp_alloc(struct xp_parser *p, size_t n) {
    jmp_buf trap;
    p->arena->trap = &trap;
    if (setjmp(trap)) {
        p->arena->trap = NULL;
        if (!p->status) p->status = XPATH_OOM;
        return NULL;
    }
    void *made = ar_alloc(p->arena, n);
    p->arena->trap = NULL;
    return made;
}
static char *xp_strndup(struct xp_parser *p, const char *s, size_t n) {
    char *copy = n < SIZE_MAX ? xp_alloc(p, n + 1) : NULL;
    if (copy) { memcpy(copy, s, n); copy[n] = 0; }
    else if (!p->status) p->status = XPATH_OOM;
    return copy;
}
static bool xp_vec_push(struct xp_parser *p, struct xp_vec *v, void *item) {
    if (v->n == v->cap) {
        size_t cap = v->cap ? v->cap * 2 : 4;
        void **grown = cap <= SIZE_MAX / sizeof *grown ? realloc(v->v, cap * sizeof *grown) : NULL;
        if (!grown) { if (!p->status) p->status = XPATH_OOM; return false; }
        v->v = grown; v->cap = cap;
    }
    v->v[v->n++] = item;
    return true;
}
/* Moves v's items into the arena; v is freed either way. */
static void **xp_vec_finish(struct xp_parser *p, struct xp_vec *v, int *count) {
    void **items = NULL;
    *count = 0;
    if (v->n > (size_t)INT32_MAX) { xp_syntax(p, "too many items"); free(v->v); return NULL; }
    if (v->n && (items = xp_alloc(p, v->n * sizeof *items))) {
        memcpy(items, v->v, v->n * sizeof *items);
        *count = (int)v->n;
    }
    free(v->v);
    *v = (struct xp_vec){0};
    return items;
}
static struct xp_expr *xp_node(struct xp_parser *p, uint8_t op, uint8_t type) {
    struct xp_expr *x = xp_alloc(p, sizeof *x);
    if (x) { x->op = op; x->type = type; }
    return x;
}
static struct xp_expr *xp_binary(struct xp_parser *p, uint8_t op, uint8_t type, struct xp_expr *left, struct xp_expr *right) {
    if (!left || !right) return NULL;
    struct xp_expr *x = xp_node(p, op, type);
    if (x) { x->left = left; x->right = right; }
    return x;
}

static struct xp_expr *xp_parse_expr(struct xp_parser *p);

/* Whether x's value can depend on its context position or size. Predicates of
   steps and filters have contexts of their own. */
static bool xp_positional(const struct xp_expr *x) {
    switch (x->op) {
    case X_LITERAL: case X_NUMBER: return false;
    case X_FUNCTION:
        if (x->fn == F_POSITION || x->fn == F_LAST) return true;
        for (int i = 0; i < x->count; i++) if (xp_positional(x->items[i])) return true;
        return false;
    case X_PATH: return x->start && xp_positional(x->start);
    case X_FILTER: return xp_positional(x->left);
    case X_NEG: return xp_positional(x->left);
    default: return xp_positional(x->left) || xp_positional(x->right);
    }
}
/* '//' is /descendant-or-self::node()/. Followed by a child step whose
   predicates hold independently of proximity position, the pair selects
   exactly the step's matches among the descendants: one descendant step. */
static void xp_merge_descendants(struct xp_expr *path) {
    int out = 0;
    for (int i = 0; i < path->nsteps; i++) {
        struct xp_step *s = path->steps[i];
        if (s->axis == A_DESCENDANT_OR_SELF && s->test == NT_NODE && !s->npredicates && i + 1 < path->nsteps &&
            path->steps[i + 1]->axis == A_CHILD) {
            struct xp_step *next = path->steps[i + 1];
            bool independent = true;
            for (int k = 0; independent && k < next->npredicates; k++)
                independent = next->predicates[k]->type != XT_NUMBER && !xp_positional(next->predicates[k]);
            if (independent) { next->axis = A_DESCENDANT; continue; }
        }
        path->steps[out++] = s;
    }
    path->nsteps = out;
}

static bool xp_unbound(struct xp_parser *p, const struct xp_tok *t) {
    if (!p->status) {
        p->status = XPATH_NAMESPACE;
        if (p->detail_size) {
            size_t length = t->prefix_length < p->detail_size - 1 ? t->prefix_length : p->detail_size - 1;
            memcpy(p->detail, t->prefix, length);
            p->detail[length] = 0;
        }
    }
    return false;
}
/* The QName's prefix expanded with the resolver, the only namespace
   declarations of the DOM's expression context. */
static bool xp_bind_prefix(struct xp_parser *p, const struct xp_tok *t, const char **ns) {
    if (!p->resolve) return xp_unbound(p, t);
    char *uri = NULL;
    size_t length = 0;
    int found = p->resolve(p->opaque, t->prefix, t->prefix_length, &uri, &length);
    if (found < 0) { if (!p->status) p->status = XPATH_EXCEPTION; return false; }
    if (!found) return xp_unbound(p, t);
    *ns = length ? xp_strndup(p, uri, length) : NULL;
    free(uri);
    return !length || *ns;
}
static bool xp_name_test(struct xp_parser *p, struct xp_step *s, const struct xp_tok *t) {
    if (!t->prefix && !t->local) { s->test = NT_ANY; return true; }
    if (t->prefix) {
        s->prefixed = true;
        if (!xp_bind_prefix(p, t, &s->ns)) return false;
    }
    if (!t->local) { s->test = NT_NS_ANY; return true; }
    s->test = NT_NAME;
    s->local_length = t->local_length;
    char *local = xp_strndup(p, t->local, t->local_length), *lower_name = xp_strndup(p, t->local, t->local_length);
    if (!local || !lower_name) return false;
    for (char *c = lower_name; *c; c++) *c = (char)lower((unsigned char)*c);
    s->local = local; s->lower = lower_name;
    return true;
}
static struct xp_expr **xp_predicates(struct xp_parser *p, int *count) {
    struct xp_vec v = {0};
    *count = 0;
    while (xp_accept(p, K_LBRACKET)) {
        struct xp_expr *predicate = xp_parse_expr(p);
        if (!predicate) { free(v.v); return NULL; }
        if (!xp_accept(p, K_RBRACKET)) { free(v.v); return xp_syntax(p, "expected ']'"); }
        if (!xp_vec_push(p, &v, predicate)) { free(v.v); return NULL; }
    }
    struct xp_expr **items = (struct xp_expr **)xp_vec_finish(p, &v, count);
    return items || !*count ? items : NULL;
}
static struct xp_step *xp_step(struct xp_parser *p) {
    struct xp_step *s = xp_alloc(p, sizeof *s);
    if (!s) return NULL;
    if (xp_accept(p, K_DOT)) { s->axis = A_SELF; s->test = NT_NODE; return s; }
    if (xp_accept(p, K_DOTDOT)) { s->axis = A_PARENT; s->test = NT_NODE; return s; }
    s->axis = A_CHILD;
    const struct xp_tok *t = xp_peek(p);
    if (t->kind == K_AXIS) {
        int axis = -1;
        for (int i = 0; i < (int)(sizeof xp_axis_names / sizeof *xp_axis_names); i++)
            if (strlen(xp_axis_names[i]) == t->local_length && !memcmp(xp_axis_names[i], t->local, t->local_length)) axis = i;
        if (axis < 0) return xp_syntax(p, "unknown axis");
        p->at++;
        if (!xp_accept(p, K_COLONCOLON)) return xp_syntax(p, "expected '::'");
        s->axis = (uint8_t)axis;
    } else if (xp_accept(p, K_AT)) s->axis = A_ATTRIBUTE;
    t = xp_peek(p);
    if (t->kind == K_NAME) {
        p->at++;
        if (!xp_name_test(p, s, t)) return NULL;
    } else if (t->kind == K_NODETYPE) {
        p->at++;
        if (!xp_accept(p, K_LPAREN)) return xp_syntax(p, "expected '('");
        if (t->local_length == 22) {
            s->test = NT_PI;
            const struct xp_tok *target = xp_peek(p);
            if (target->kind == K_LITERAL) {
                p->at++;
                if (!(s->local = xp_strndup(p, target->local, target->local_length))) return NULL;
                s->local_length = target->local_length;
            }
        } else s->test = t->local[0] == 'c' ? NT_COMMENT : t->local[0] == 't' ? NT_TEXT : NT_NODE;
        if (!xp_accept(p, K_RPAREN)) return xp_syntax(p, "expected ')'");
    } else return xp_syntax(p, "expected a node test");
    s->predicates = xp_predicates(p, &s->npredicates);
    if (!s->predicates && p->status) return NULL;
    return s;
}
static bool xp_step_starts(uint8_t kind) {
    return kind == K_NAME || kind == K_NODETYPE || kind == K_AXIS || kind == K_AT || kind == K_DOT || kind == K_DOTDOT;
}
/* Steps separated by '/' and '//'. With separator_first the path continues
   after a separator; otherwise it begins with a step. */
static bool xp_steps(struct xp_parser *p, struct xp_expr *path, bool separator_first) {
    struct xp_vec steps = {0};
    for (bool first = true;; first = false) {
        if (!first || separator_first) {
            if (xp_accept(p, K_DSLASH)) {
                struct xp_step *any = xp_alloc(p, sizeof *any);
                if (!any || !xp_vec_push(p, &steps, any)) { free(steps.v); return false; }
                any->axis = A_DESCENDANT_OR_SELF; any->test = NT_NODE;
            } else if (!xp_accept(p, K_SLASH)) break;
        }
        struct xp_step *s = xp_step(p);
        if (!s || !xp_vec_push(p, &steps, s)) { free(steps.v); return false; }
    }
    path->steps = (struct xp_step **)xp_vec_finish(p, &steps, &path->nsteps);
    if (!path->steps) return false;
    xp_merge_descendants(path);
    return true;
}
static struct xp_expr *xp_call(struct xp_parser *p) {
    const struct xp_tok *t = xp_peek(p);
    if (t->prefix) return xp_syntax(p, "no extension functions are available");
    int fn = -1;
    for (int i = 0; i < F_COUNT_OF; i++)
        if (strlen(xp_functions[i].name) == t->local_length && !memcmp(xp_functions[i].name, t->local, t->local_length)) fn = i;
    if (fn < 0) return xp_syntax(p, "unknown function");
    p->at++;
    if (!xp_accept(p, K_LPAREN)) return xp_syntax(p, "expected '('");
    struct xp_vec args = {0};
    if (!xp_accept(p, K_RPAREN)) {
        do {
            struct xp_expr *arg = xp_parse_expr(p);
            if (!arg || !xp_vec_push(p, &args, arg)) { free(args.v); return NULL; }
        } while (xp_accept(p, K_COMMA));
        if (!xp_accept(p, K_RPAREN)) { free(args.v); return xp_syntax(p, "expected ')'"); }
    }
    const struct xp_function *f = &xp_functions[fn];
    if (args.n < (size_t)f->min || (f->max >= 0 && args.n > (size_t)f->max)) {
        free(args.v); return xp_syntax(p, "wrong number of function arguments");
    }
    if (f->node_set && args.n && ((struct xp_expr *)args.v[0])->type != XT_NODESET) {
        free(args.v); return xp_syntax(p, "the function argument must be a node-set");
    }
    struct xp_expr *x = xp_node(p, X_FUNCTION, f->result);
    if (!x) { free(args.v); return NULL; }
    x->fn = (uint8_t)fn;
    x->items = (struct xp_expr **)xp_vec_finish(p, &args, &x->count);
    return x->items || !x->count ? x : NULL;
}
static struct xp_expr *xp_primary(struct xp_parser *p) {
    const struct xp_tok *t = xp_peek(p);
    switch (t->kind) {
    case K_VARIABLE: return xp_syntax(p, "variable references have no bindings");
    case K_LPAREN: {
        p->at++;
        struct xp_expr *inner = xp_parse_expr(p);
        if (!inner) return NULL;
        return xp_accept(p, K_RPAREN) ? inner : xp_syntax(p, "expected ')'");
    }
    case K_LITERAL: {
        struct xp_expr *x = xp_node(p, X_LITERAL, XT_STRING);
        if (!x || !(x->text = xp_strndup(p, t->local, t->local_length))) return NULL;
        x->length = t->local_length;
        p->at++;
        return x;
    }
    case K_NUMBER: {
        struct xp_expr *x = xp_node(p, X_NUMBER, XT_NUMBER);
        if (x) { x->number = t->number; p->at++; }
        return x;
    }
    case K_FUNCTION: return xp_call(p);
    default: return xp_syntax(p, "expected an expression");
    }
}
static struct xp_expr *xp_filter_expr(struct xp_parser *p) {
    struct xp_expr *primary = xp_primary(p);
    if (!primary || xp_peek(p)->kind != K_LBRACKET) return primary;
    if (primary->type != XT_NODESET) return xp_syntax(p, "a predicate needs a node-set");
    struct xp_expr *x = xp_node(p, X_FILTER, XT_NODESET);
    if (!x) return NULL;
    x->left = primary;
    x->items = xp_predicates(p, &x->count);
    return x->items || !p->status ? x : NULL;
}
static struct xp_expr *xp_path_expr(struct xp_parser *p) {
    uint8_t kind = xp_peek(p)->kind;
    if (kind == K_VARIABLE || kind == K_LPAREN || kind == K_LITERAL || kind == K_NUMBER || kind == K_FUNCTION) {
        struct xp_expr *filter = xp_filter_expr(p);
        if (!filter) return NULL;
        kind = xp_peek(p)->kind;
        if (kind != K_SLASH && kind != K_DSLASH) return filter;
        if (filter->type != XT_NODESET) return xp_syntax(p, "a location path needs a node-set");
        struct xp_expr *path = xp_node(p, X_PATH, XT_NODESET);
        if (!path) return NULL;
        path->start = filter;
        return xp_steps(p, path, true) ? path : NULL;
    }
    struct xp_expr *path = xp_node(p, X_PATH, XT_NODESET);
    if (!path) return NULL;
    if (kind == K_SLASH) {
        p->at++;
        path->absolute = true;
        if (!xp_step_starts(xp_peek(p)->kind)) return path;
        return xp_steps(p, path, false) ? path : NULL;
    }
    if (kind == K_DSLASH) {
        path->absolute = true;
        return xp_steps(p, path, true) ? path : NULL;
    }
    return xp_steps(p, path, false) ? path : NULL;
}
static struct xp_expr *xp_union_expr(struct xp_parser *p) {
    struct xp_expr *left = xp_path_expr(p);
    while (left && xp_accept(p, K_PIPE)) {
        struct xp_expr *right = xp_path_expr(p);
        if (!right) return NULL;
        if (left->type != XT_NODESET || right->type != XT_NODESET) return xp_syntax(p, "'|' needs node-sets");
        left = xp_binary(p, X_UNION, XT_NODESET, left, right);
    }
    return left;
}
static struct xp_expr *xp_unary_expr(struct xp_parser *p) {
    size_t negations = 0;
    while (xp_accept(p, K_MINUS)) negations++;
    struct xp_expr *x = xp_union_expr(p);
    while (x && negations--) {
        struct xp_expr *negated = xp_node(p, X_NEG, XT_NUMBER);
        if (negated) negated->left = x;
        x = negated;
    }
    return x;
}
static struct xp_expr *xp_multiplicative_expr(struct xp_parser *p) {
    struct xp_expr *x = xp_unary_expr(p);
    for (;;) {
        uint8_t op = xp_accept(p, K_MUL) ? X_MUL : xp_accept(p, K_DIV) ? X_DIV : xp_accept(p, K_MOD) ? X_MOD : 0;
        if (!x || !op) return x;
        x = xp_binary(p, op, XT_NUMBER, x, xp_unary_expr(p));
    }
}
static struct xp_expr *xp_additive_expr(struct xp_parser *p) {
    struct xp_expr *x = xp_multiplicative_expr(p);
    for (;;) {
        uint8_t op = xp_accept(p, K_PLUS) ? X_ADD : xp_accept(p, K_MINUS) ? X_SUB : 0;
        if (!x || !op) return x;
        x = xp_binary(p, op, XT_NUMBER, x, xp_multiplicative_expr(p));
    }
}
static struct xp_expr *xp_relational_expr(struct xp_parser *p) {
    struct xp_expr *x = xp_additive_expr(p);
    for (;;) {
        uint8_t op = xp_accept(p, K_LT) ? X_LT : xp_accept(p, K_LE) ? X_LE :
                     xp_accept(p, K_GT) ? X_GT : xp_accept(p, K_GE) ? X_GE : 0;
        if (!x || !op) return x;
        x = xp_binary(p, op, XT_BOOLEAN, x, xp_additive_expr(p));
    }
}
static struct xp_expr *xp_equality_expr(struct xp_parser *p) {
    struct xp_expr *x = xp_relational_expr(p);
    for (;;) {
        uint8_t op = xp_accept(p, K_EQ) ? X_EQ : xp_accept(p, K_NE) ? X_NE : 0;
        if (!x || !op) return x;
        x = xp_binary(p, op, XT_BOOLEAN, x, xp_relational_expr(p));
    }
}
static struct xp_expr *xp_and_expr(struct xp_parser *p) {
    struct xp_expr *x = xp_equality_expr(p);
    while (x && xp_accept(p, K_AND)) x = xp_binary(p, X_AND, XT_BOOLEAN, x, xp_equality_expr(p));
    return x;
}
static struct xp_expr *xp_parse_expr(struct xp_parser *p) {
    if (xp_stack_exhausted(p->stack_base)) {
        if (!p->status) {
            p->status = XPATH_DEPTH;
            if (p->detail_size) snprintf(p->detail, p->detail_size, "nested too deeply");
        }
        return NULL;
    }
    struct xp_expr *x = xp_and_expr(p);
    while (x && xp_accept(p, K_OR)) x = xp_binary(p, X_OR, XT_BOOLEAN, x, xp_and_expr(p));
    return x;
}

enum xpath_status xpath_compile(const char *source, size_t length, xpath_resolve_fn resolve, void *opaque,
                                struct xpath_expr **out, char *detail, size_t detail_size) {
    *out = NULL;
    if (detail_size) detail[0] = 0;
    struct xp_tokens tokens = {0};
    enum xpath_status status = xp_lex(source, length, &tokens, detail, detail_size);
    if (status) { free(tokens.v); return status; }
    struct xpath_expr *expr = calloc(1, sizeof *expr);
    if (!expr) { free(tokens.v); return XPATH_OOM; }
    expr->arena.chunk_size = 4096;
    struct xp_parser p = {.tok = tokens.v, .arena = &expr->arena, .resolve = resolve, .opaque = opaque,
                          .detail = detail, .detail_size = detail_size, .stack_base = __builtin_frame_address(0)};
    struct xp_expr *root = xp_parse_expr(&p);
    if (root && !xp_accept(&p, K_END)) root = xp_syntax(&p, "unexpected token");
    free(tokens.v);
    if (!root) {
        ar_free(&expr->arena);
        free(expr);
        return p.status ? p.status : XPATH_OOM;
    }
    expr->root = root;
    *out = expr;
    return XPATH_OK;
}
void xpath_free(struct xpath_expr *expr) {
    if (!expr) return;
    ar_free(&expr->arena);
    free(expr);
}

/* ---------------------------------------------------------------- the data model */
/* An XPath node: a DOM node, or an attribute (the element's attrs[index]) or a
   namespace node (prefix and URI, index its order) of the element node. The
   kinds are ordered as document order orders them on one element. */
enum { XN_NODE, XN_NAMESPACE, XN_ATTR };
struct xp_node {
    node_t *node;
    int32_t index;
    uint8_t kind;
    const char *prefix, *uri;
};
/* A node-set value is kept in document order without duplicates. */
struct xp_set { struct xp_node *v; size_t n, cap; };
struct xp_value {
    uint8_t type;
    bool boolean;
    double number;
    struct xs string;
    struct xp_set set;
};
/* Document order ranks of the context tree, built only for large sorts. */
struct xp_order { node_t **keys; uint32_t *ranks; size_t mask; bool built; };
struct xp_eval {
    web_doc *doc;  /* the context node's document: cancellation */
    node_t *root;  /* the root of the context node's tree */
    bool html;     /* the context node is from an HTML DOM: the default element namespace is HTML */
    enum xpath_status status;
    const char *stack_base;
    struct xp_order order;
    /* An axis walk stops at axis_limit matches (0: none), when the step's
       first predicate is a proximity position; axis_full says it stopped. */
    size_t axis_limit;
    bool axis_full;
};
struct xp_ctx { struct xp_node node; size_t position, size; };

static struct xp_node xp_plain(node_t *n) { return (struct xp_node){.node = n, .index = -1, .kind = XN_NODE}; }
static bool xp_visit(struct xp_eval *e) {
    if (web_native_checkpoint(e->doc)) return true;
    e->status = XPATH_CANCELLED;
    return false;
}
static bool xp_oom(struct xp_eval *e) { e->status = XPATH_OOM; return false; }
static bool xb_put(struct xp_eval *e, struct xbuf *b, const char *s, size_t n) {
    if (!n) return true;
    if (n > SIZE_MAX - b->n - 1) return xp_oom(e);
    if (b->n + n + 1 > b->cap) {
        size_t cap = b->cap ? b->cap : 64;
        while (cap < b->n + n + 1) cap = cap > SIZE_MAX / 2 ? b->n + n + 1 : cap * 2;
        char *grown = realloc(b->p, cap);
        if (!grown) return xp_oom(e);
        b->p = grown; b->cap = cap;
    }
    memcpy(b->p + b->n, s, n);
    b->n += n;
    b->p[b->n] = 0;
    return true;
}
static struct xs xb_take(struct xbuf *b) {
    if (!b->p) return xs_borrow("", 0);
    struct xs s = {b->p, b->n, b->p};
    *b = (struct xbuf){0};
    return s;
}
static bool xp_push(struct xp_eval *e, struct xp_set *s, struct xp_node x) {
    if (s->n == s->cap) {
        size_t cap = s->cap ? s->cap * 2 : 8;
        struct xp_node *grown = cap <= SIZE_MAX / sizeof *grown ? realloc(s->v, cap * sizeof *grown) : NULL;
        if (!grown) return xp_oom(e);
        s->v = grown; s->cap = cap;
    }
    s->v[s->n++] = x;
    return true;
}
static void xv_free(struct xp_value *v) {
    if (v->type == XT_NODESET) free(v->set.v);
    else if (v->type == XT_STRING) free(v->string.own);
    v->set = (struct xp_set){0};
    v->string = xs_borrow("", 0);
}

/* DOM Level 3 XPath 1.2.4: of adjacent Text siblings, the first non-empty one
   is the XPath text node; empty and later ones are not XPath nodes. */
static bool xp_text_first(const node_t *n) {
    if (n->type != N_TEXT || !n->textlen) return false;
    for (const node_t *p = n->prev; p && p->type == N_TEXT; p = p->prev) if (p->textlen) return false;
    return true;
}
/* A DOM node met by an axis that is an XPath node (a DocumentType is none). */
static bool xp_modelled(const node_t *n) {
    switch (n->type) {
    case N_ELEM: case N_COMMENT: case N_PI: case N_DOC: case N_FRAGMENT: return true;
    case N_TEXT: return xp_text_first(n);
    default: return false;
    }
}
/* The next node in document order within root's subtree, after n. */
static node_t *xp_next_in(node_t *n, node_t *root) {
    if (n->first) return n->first;
    for (; n && n != root; n = n->parent) if (n->next) return n->next;
    return NULL;
}
static node_t *xp_previous(node_t *n) {
    if (!n->prev) return n->parent;
    for (n = n->prev; n->last; n = n->last) {}
    return n;
}
static struct xp_node xp_root(const struct xp_node *x) {
    node_t *n = x->node;
    while (n->parent) n = n->parent;
    return xp_plain(n);
}

/* ---------------------------------------------------------------- document order */
static size_t xp_hash(const node_t *n, size_t mask) {
    uint64_t x = (uint64_t)(uintptr_t)n;
    x ^= x >> 29;
    x *= 0x9E3779B97F4A7C15ull;
    return (size_t)(x >> 32) & mask;
}
static bool xp_order_build(struct xp_eval *e) {
    if (e->order.built) return true;
    size_t count = 0;
    for (node_t *n = e->root; n; n = xp_next_in(n, e->root)) {
        if (!xp_visit(e)) return false;
        count++;
    }
    if (count > UINT32_MAX || count > SIZE_MAX / 4 / sizeof(node_t *)) return true; /* tree walks instead */
    size_t cap = 16;
    while (cap < count * 2) cap *= 2;
    node_t **keys = calloc(cap, sizeof *keys);
    uint32_t *ranks = malloc(cap * sizeof *ranks);
    if (!keys || !ranks) { free(keys); free(ranks); return xp_oom(e); }
    uint32_t rank = 0;
    for (node_t *n = e->root; n; n = xp_next_in(n, e->root)) {
        size_t h = xp_hash(n, cap - 1);
        while (keys[h]) h = (h + 1) & (cap - 1);
        keys[h] = n; ranks[h] = rank++;
    }
    e->order = (struct xp_order){keys, ranks, cap - 1, true};
    return true;
}
static bool xp_order_rank(const struct xp_order *o, const node_t *n, uint32_t *rank) {
    for (size_t h = xp_hash(n, o->mask); o->keys[h]; h = (h + 1) & o->mask)
        if (o->keys[h] == n) { *rank = o->ranks[h]; return true; }
    return false;
}
/* Document order of two distinct DOM nodes: an ancestor first, then the order
   of the siblings under the deepest common ancestor. Separate trees are
   ordered by address, consistently. */
static int xp_tree_order(const node_t *a, const node_t *b) {
    size_t da = 0, db = 0;
    for (const node_t *x = a; x->parent; x = x->parent) da++;
    for (const node_t *x = b; x->parent; x = x->parent) db++;
    const node_t *pa = a, *pb = b;
    while (da > db) { pa = pa->parent; da--; }
    while (db > da) { pb = pb->parent; db--; }
    if (pa == pb) return pa == a ? -1 : 1;
    while (pa->parent != pb->parent) { pa = pa->parent; pb = pb->parent; }
    if (!pa->parent) return (uintptr_t)a < (uintptr_t)b ? -1 : 1;
    for (const node_t *x = pa->next; x; x = x->next) if (x == pb) return -1;
    return 1;
}
/* DOM Level 3 XPath 1.2.8: an element, then its namespace nodes, then its
   attributes, then its children. */
static int xp_cmp(const struct xp_eval *e, const struct xp_node *a, const struct xp_node *b) {
    if (a->node == b->node) {
        if (a->kind != b->kind) return a->kind < b->kind ? -1 : 1;
        return a->index < b->index ? -1 : a->index > b->index;
    }
    uint32_t ra, rb;
    if (e->order.built && xp_order_rank(&e->order, a->node, &ra) && xp_order_rank(&e->order, b->node, &rb))
        return ra < rb ? -1 : 1;
    return xp_tree_order(a->node, b->node);
}
/* Sorts s into document order and removes duplicates. */
static bool xp_sort(struct xp_eval *e, struct xp_set *s) {
    if (s->n < 2) return true;
    /* A tree comparison walks to the common ancestor: many nodes rank once. */
    if (s->n > 32 && !xp_order_build(e)) return false;
    size_t i = 1;
    while (i < s->n && xp_cmp(e, &s->v[i - 1], &s->v[i]) < 0) i++;
    if (i == s->n) return true;
    struct xp_node *scratch = s->n <= SIZE_MAX / sizeof *scratch ? malloc(s->n * sizeof *scratch) : NULL;
    if (!scratch) return xp_oom(e);
    for (size_t width = 1; width < s->n; width *= 2) {
        if (!xp_visit(e)) { free(scratch); return false; }
        for (size_t lo = 0; lo < s->n; lo += 2 * width) {
            size_t mid = lo + width < s->n ? lo + width : s->n, hi = lo + 2 * width < s->n ? lo + 2 * width : s->n;
            size_t a = lo, b = mid, k = lo;
            while (a < mid && b < hi) scratch[k++] = xp_cmp(e, &s->v[b], &s->v[a]) < 0 ? s->v[b++] : s->v[a++];
            while (a < mid) scratch[k++] = s->v[a++];
            while (b < hi) scratch[k++] = s->v[b++];
        }
        memcpy(s->v, scratch, s->n * sizeof *scratch);
        if (width > SIZE_MAX / 2) break;
    }
    free(scratch);
    size_t kept = 1;
    for (size_t j = 1; j < s->n; j++) if (xp_cmp(e, &s->v[kept - 1], &s->v[j])) s->v[kept++] = s->v[j];
    s->n = kept;
    return true;
}
/* The union of two node-sets in document order; a and b are consumed. */
static bool xp_merge(struct xp_eval *e, struct xp_set *a, struct xp_set *b, struct xp_set *out) {
    *out = (struct xp_set){0};
    if (!a->n || !b->n) {
        *out = a->n ? *a : *b;
        free(a->n ? b->v : a->v);
        *a = *b = (struct xp_set){0};
        return true;
    }
    bool ok = a->n + b->n <= 64 || xp_order_build(e);
    size_t i = 0, j = 0;
    while (ok && (i < a->n || j < b->n)) {
        int order = i == a->n ? 1 : j == b->n ? -1 : xp_cmp(e, &a->v[i], &b->v[j]);
        ok = xp_push(e, out, order <= 0 ? a->v[i] : b->v[j]);
        if (order <= 0) i++;
        if (order >= 0) j++;
    }
    free(a->v); free(b->v);
    *a = *b = (struct xp_set){0};
    if (!ok) { free(out->v); *out = (struct xp_set){0}; }
    return ok;
}

/* ---------------------------------------------------------------- node tests and axes */
static bool xp_test(const struct xp_eval *e, const struct xp_step *s, const struct xp_node *x) {
    const node_t *n = x->node;
    switch (s->test) {
    case NT_NODE: return true;
    case NT_TEXT: return x->kind == XN_NODE && n->type == N_TEXT;
    case NT_COMMENT: return x->kind == XN_NODE && n->type == N_COMMENT;
    case NT_PI: return x->kind == XN_NODE && n->type == N_PI && (!s->local || xp_name_is(n->name, s->local, s->local_length));
    default: break;
    }
    /* A name test selects nodes of the axis's principal node type. */
    if (s->axis == A_ATTRIBUTE) {
        if (x->kind != XN_ATTR) return false;
        const struct attr *a = &n->attrs[x->index];
        if (s->test == NT_ANY) return true;
        if (s->test == NT_NS_ANY) return xp_same(a->namespace_uri, s->ns);
        return xp_same(a->namespace_uri, s->prefixed ? s->ns : NULL) &&
               xp_name_is(xp_attr_local(a), xp_html_names(n) ? s->lower : s->local, s->local_length);
    }
    if (s->axis == A_NAMESPACE) {
        /* A namespace node's expanded-name is its prefix in no namespace. */
        if (x->kind != XN_NAMESPACE) return false;
        if (s->test == NT_ANY) return true;
        if (s->test == NT_NS_ANY) return !s->ns;
        return (!s->prefixed || !s->ns) && x->prefix && xp_name_is(x->prefix, s->local, s->local_length);
    }
    if (x->kind != XN_NODE || n->type != N_ELEM) return false;
    if (s->test == NT_ANY) return true;
    const char *ns = xp_element_namespace(n);
    if (s->test == NT_NS_ANY) return xp_same(ns, s->ns);
    /* HTML: an unprefixed name test means the default element namespace. */
    return xp_same(ns, s->prefixed ? s->ns : e->html ? XP_NS_HTML : NULL) &&
           xp_name_is(xp_element_local(n), xp_html_names(n) ? s->lower : s->local, s->local_length);
}
/* Adds an axis match; false ends the walk, with axis_full set when the walk
   has all the matches the step can use. */
static bool xp_hit(struct xp_eval *e, struct xp_set *out, struct xp_node x) {
    if (!xp_push(e, out, x)) return false;
    if (!e->axis_limit || out->n < e->axis_limit) return true;
    e->axis_full = true;
    return false;
}
/* Takes the DOM node n into out when it is an XPath node passing the test. */
static bool xp_take(struct xp_eval *e, const struct xp_step *s, node_t *n, struct xp_set *out) {
    if (!xp_visit(e)) return false;
    if (!xp_modelled(n)) return true;
    struct xp_node x = xp_plain(n);
    return !xp_test(e, s, &x) || xp_hit(e, out, x);
}
static bool xp_descendants(struct xp_eval *e, const struct xp_step *s, node_t *root, struct xp_set *out) {
    for (node_t *d = root->first; d; d = xp_next_in(d, root)) if (!xp_take(e, s, d, out)) return false;
    return true;
}
static bool xp_ancestors(struct xp_eval *e, const struct xp_step *s, const struct xp_node *x, struct xp_set *out) {
    for (node_t *p = x->kind == XN_NODE ? x->node->parent : x->node; p; p = p->parent) {
        if (!xp_visit(e)) return false;
        struct xp_node y = xp_plain(p);
        if (xp_test(e, s, &y) && !xp_hit(e, out, y)) return false;
    }
    return true;
}
struct xp_binding { const char *prefix, *uri; };
static bool xp_bind(struct xp_eval *e, struct xp_binding **v, size_t *n, size_t *cap, const char *prefix, const char *uri) {
    for (size_t i = 0; i < *n; i++) if (xp_same((*v)[i].prefix, prefix)) return true; /* a nearer binding */
    if (*n == *cap) {
        size_t grown_cap = *cap ? *cap * 2 : 8;
        struct xp_binding *grown = realloc(*v, grown_cap * sizeof *grown);
        if (!grown) return xp_oom(e);
        *v = grown; *cap = grown_cap;
    }
    (*v)[(*n)++] = (struct xp_binding){prefix, uri};
    return true;
}
/* DOM Level 3 XPath 1.2.3: the element's namespace nodes are the xml binding
   and, nearest first, each element's own namespace and the namespace
   declaration attributes of the element and its ancestors. An empty URI
   undeclares the prefix; xmlns is never a namespace node. */
static bool xp_namespace_nodes(struct xp_eval *e, const struct xp_step *s, node_t *element, struct xp_set *out) {
    struct xp_binding *v = NULL;
    size_t n = 0, cap = 0;
    bool ok = true;
    for (node_t *x = element; ok && x && x->type == N_ELEM; x = x->parent) {
        ok = xp_bind(e, &v, &n, &cap, xp_element_prefix(x), xp_element_namespace(x));
        for (int i = 0; ok && i < x->nattrs; i++) {
            const struct attr *a = &x->attrs[i];
            if (!xp_namespace_declaration(a)) continue;
            const char *local = xp_attr_local(a);
            if (a->prefix && !strcmp(a->prefix, "xmlns")) ok = xp_bind(e, &v, &n, &cap, local, a->value);
            else if (!a->prefix && !strcmp(local, "xmlns")) ok = xp_bind(e, &v, &n, &cap, NULL, a->value);
        }
    }
    if (ok) ok = xp_bind(e, &v, &n, &cap, "xml", XP_NS_XML);
    int32_t index = 0;
    for (size_t i = 0; ok && i < n; i++) {
        if (!v[i].uri || !*v[i].uri || (v[i].prefix && !strcmp(v[i].prefix, "xmlns"))) continue;
        struct xp_node y = {.node = element, .index = index++, .kind = XN_NAMESPACE, .prefix = v[i].prefix, .uri = v[i].uri};
        if (xp_test(e, s, &y)) ok = xp_hit(e, out, y);
    }
    free(v);
    return ok;
}
/* The step's axis from x, with its node test, in axis order (reverse axes in
   reverse document order). */
static bool xp_axis(struct xp_eval *e, const struct xp_step *s, const struct xp_node *x, struct xp_set *out) {
    node_t *n = x->node;
    bool plain = x->kind == XN_NODE;
    switch (s->axis) {
    case A_SELF: return !xp_test(e, s, x) || xp_hit(e, out, *x);
    case A_CHILD:
        if (plain) for (node_t *c = n->first; c; c = c->next) if (!xp_take(e, s, c, out)) return false;
        return true;
    case A_DESCENDANT: return !plain || xp_descendants(e, s, n, out);
    case A_DESCENDANT_OR_SELF:
        if (xp_test(e, s, x) && !xp_hit(e, out, *x)) return false;
        return !plain || xp_descendants(e, s, n, out);
    case A_PARENT: {
        node_t *parent = plain ? n->parent : n;
        if (!parent) return true;
        struct xp_node y = xp_plain(parent);
        return !xp_test(e, s, &y) || xp_hit(e, out, y);
    }
    case A_ANCESTOR: return xp_ancestors(e, s, x, out);
    case A_ANCESTOR_OR_SELF:
        if (xp_test(e, s, x) && !xp_hit(e, out, *x)) return false;
        return xp_ancestors(e, s, x, out);
    case A_FOLLOWING_SIBLING:
        if (plain) for (node_t *c = n->next; c; c = c->next) if (!xp_take(e, s, c, out)) return false;
        return true;
    case A_PRECEDING_SIBLING:
        if (plain) for (node_t *c = n->prev; c; c = c->prev) if (!xp_take(e, s, c, out)) return false;
        return true;
    case A_FOLLOWING:
        /* An attribute or namespace node precedes its element's children. */
        if (!plain && !xp_descendants(e, s, n, out)) return false;
        for (node_t *a = n; a; a = a->parent)
            for (node_t *sibling = a->next; sibling; sibling = sibling->next)
                if (!xp_take(e, s, sibling, out) || !xp_descendants(e, s, sibling, out)) return false;
        return true;
    case A_PRECEDING: {
        node_t *ancestor = n->parent;
        for (node_t *p = xp_previous(n); p; p = xp_previous(p)) {
            if (p == ancestor) { ancestor = ancestor->parent; continue; }
            if (!xp_take(e, s, p, out)) return false;
        }
        return true;
    }
    case A_ATTRIBUTE:
        if (plain && n->type == N_ELEM) for (int i = 0; i < n->nattrs; i++) {
            if (xp_namespace_declaration(&n->attrs[i])) continue;
            struct xp_node y = {.node = n, .index = i, .kind = XN_ATTR};
            if (xp_test(e, s, &y) && !xp_hit(e, out, y)) return false;
        }
        return true;
    case A_NAMESPACE: return !(plain && n->type == N_ELEM) || xp_namespace_nodes(e, s, n, out);
    default: return true;
    }
}

/* ---------------------------------------------------------------- string-values */
/* Concatenation that borrows a single piece and copies only from a second. */
struct xp_acc { struct xs first; struct xbuf buffer; bool copying; };
static bool xp_acc_add(struct xp_eval *e, struct xp_acc *a, const char *p, size_t n) {
    if (!n) return true;
    if (!a->copying) {
        if (!a->first.n) { a->first = xs_borrow(p, n); return true; }
        a->copying = true;
        if (!xb_put(e, &a->buffer, a->first.p, a->first.n)) return false;
    }
    return xb_put(e, &a->buffer, p, n);
}
static struct xs xp_acc_take(struct xp_acc *a) {
    return a->copying ? xb_take(&a->buffer) : a->first.n ? a->first : xs_borrow("", 0);
}
static bool xp_string_value(struct xp_eval *e, const struct xp_node *x, struct xs *out) {
    node_t *n = x->node;
    if (x->kind == XN_ATTR) {
        const char *value = n->attrs[x->index].value;
        *out = xs_borrow(value, value ? strlen(value) : 0);
        return true;
    }
    if (x->kind == XN_NAMESPACE) { *out = xs_borrow(x->uri, strlen(x->uri)); return true; }
    struct xp_acc a = {0};
    switch (n->type) {
    case N_ATTR: {
        const char *value = n->attribute ? n->attribute->value : NULL;
        *out = xs_borrow(value, value ? strlen(value) : 0);
        return true;
    }
    case N_COMMENT: case N_PI: *out = xs_borrow(n->text, n->textlen); return true;
    case N_TEXT:
        for (node_t *t = n; t && t->type == N_TEXT; t = t->next)
            if (!xp_acc_add(e, &a, t->text, t->textlen)) goto failed;
        break;
    default:
        for (node_t *d = n->first; d; d = xp_next_in(d, n)) {
            if (!xp_visit(e)) goto failed;
            if (d->type == N_TEXT && !xp_acc_add(e, &a, d->text, d->textlen)) goto failed;
        }
    }
    *out = xp_acc_take(&a);
    return true;
failed:
    free(a.buffer.p);
    return false;
}
static bool xp_number_of(struct xp_eval *e, const char *p, size_t n, double *out) {
    return xp_string_number(p, n, out) || xp_oom(e);
}
static bool xv_boolean(const struct xp_value *v) {
    switch (v->type) {
    case XT_BOOLEAN: return v->boolean;
    case XT_NUMBER: return !(isnan(v->number) || v->number == 0);
    case XT_STRING: return v->string.n != 0;
    default: return v->set.n != 0;
    }
}
/* string() of a value; borrows from v only for a string value. */
static bool xv_string(struct xp_eval *e, const struct xp_value *v, struct xs *out) {
    switch (v->type) {
    case XT_STRING: *out = xs_borrow(v->string.p, v->string.n); return true;
    case XT_BOOLEAN: *out = v->boolean ? xs_borrow("true", 4) : xs_borrow("false", 5); return true;
    case XT_NUMBER: return xp_number_string(v->number, out) || xp_oom(e);
    default:
        if (!v->set.n) { *out = xs_borrow("", 0); return true; }
        return xp_string_value(e, &v->set.v[0], out);
    }
}
static bool xv_number(struct xp_eval *e, const struct xp_value *v, double *out) {
    switch (v->type) {
    case XT_NUMBER: *out = v->number; return true;
    case XT_BOOLEAN: *out = v->boolean ? 1 : 0; return true;
    case XT_STRING: return xp_number_of(e, v->string.p, v->string.n, out);
    default: {
        struct xs s;
        if (!xv_string(e, v, &s)) return false;
        bool ok = xp_number_of(e, s.p, s.n, out);
        free(s.own);
        return ok;
    }
    }
}

/* ---------------------------------------------------------------- evaluation */
static bool xp_eval(struct xp_eval *e, const struct xp_expr *x, const struct xp_ctx *c, struct xp_value *out);

static bool xp_eval_number(struct xp_eval *e, const struct xp_expr *x, const struct xp_ctx *c, double *out) {
    struct xp_value v;
    if (!xp_eval(e, x, c, &v)) return false;
    bool ok = xv_number(e, &v, out);
    xv_free(&v);
    return ok;
}
/* The argument converted as if by string(); the result owns what it needs. */
static bool xp_eval_string(struct xp_eval *e, const struct xp_expr *x, const struct xp_ctx *c, struct xs *out) {
    struct xp_value v;
    if (!xp_eval(e, x, c, &v)) return false;
    if (v.type == XT_STRING) { *out = v.string; return true; }
    bool ok = xv_string(e, &v, out);
    xv_free(&v);
    return ok;
}
/* A function's optional string argument, else the context node's string-value. */
static bool xp_string_or_context(struct xp_eval *e, const struct xp_expr *x, const struct xp_ctx *c, struct xs *out) {
    return x->count ? xp_eval_string(e, x->items[0], c, out) : xp_string_value(e, &c->node, out);
}
static bool xp_eval_set(struct xp_eval *e, const struct xp_expr *x, const struct xp_ctx *c, struct xp_set *out) {
    struct xp_value v;
    if (!xp_eval(e, x, c, &v)) return false;
    *out = v.set; /* statically a node-set */
    return true;
}

static bool xp_num_compare(uint8_t op, double a, double b) {
    switch (op) {
    case X_EQ: return a == b;
    case X_NE: return a != b;
    case X_LT: return a < b;
    case X_LE: return a <= b;
    case X_GT: return a > b;
    default: return a >= b;
    }
}
static uint8_t xp_flip(uint8_t op) {
    return op == X_LT ? X_GT : op == X_GT ? X_LT : op == X_LE ? X_GE : op == X_GE ? X_LE : op;
}
/* Section 3.4 for two node-sets: some pair of string-values compares true. */
static bool xp_compare_sets(struct xp_eval *e, uint8_t op, const struct xp_set *l, const struct xp_set *r, bool *result) {
    if (!l->n || !r->n) return true;
    if (op != X_EQ && op != X_NE) {
        /* Relational: some number of l against some of r; NaN compares false. */
        double lmin = INFINITY, lmax = -INFINITY, rmin = INFINITY, rmax = -INFINITY;
        bool lany = false, rany = false;
        for (int side = 0; side < 2; side++) {
            const struct xp_set *set = side ? r : l;
            for (size_t i = 0; i < set->n; i++) {
                struct xs s;
                double d;
                if (!xp_string_value(e, &set->v[i], &s)) return false;
                bool ok = xp_number_of(e, s.p, s.n, &d);
                free(s.own);
                if (!ok) return false;
                if (isnan(d)) continue;
                if (side) { rany = true; if (d < rmin) rmin = d; if (d > rmax) rmax = d; }
                else { lany = true; if (d < lmin) lmin = d; if (d > lmax) lmax = d; }
            }
        }
        if (lany && rany)
            *result = op == X_LT ? lmin < rmax : op == X_LE ? lmin <= rmax : op == X_GT ? lmax > rmin : lmax >= rmin;
        return true;
    }
    struct xs *right = r->n <= SIZE_MAX / sizeof *right ? calloc(r->n, sizeof *right) : NULL;
    if (!right) return xp_oom(e);
    size_t made = 0;
    bool ok = true;
    for (; ok && made < r->n; made++) ok = xp_string_value(e, &r->v[made], &right[made]);
    if (!ok) made--;
    for (size_t i = 0; ok && !*result && i < l->n; i++) {
        struct xs s;
        if (!(ok = xp_string_value(e, &l->v[i], &s))) break;
        for (size_t j = 0; !*result && j < r->n; j++) *result = xs_equal(&s, &right[j]) == (op == X_EQ);
        free(s.own);
    }
    for (size_t j = 0; j < made; j++) free(right[j].own);
    free(right);
    return ok;
}
/* Section 3.4: =, !=, <, <=, > and >=. */
static bool xp_compare(struct xp_eval *e, uint8_t op, const struct xp_value *l, const struct xp_value *r, bool *result) {
    bool equality = op == X_EQ || op == X_NE;
    *result = false;
    if (l->type == XT_NODESET && r->type == XT_NODESET) return xp_compare_sets(e, op, &l->set, &r->set, result);
    if (r->type == XT_NODESET) { const struct xp_value *t = l; l = r; r = t; op = xp_flip(op); }
    if (l->type == XT_NODESET) {
        const struct xp_set *set = &l->set;
        if (r->type == XT_BOOLEAN) {
            bool a = set->n != 0;
            *result = equality ? (a == r->boolean) == (op == X_EQ) : xp_num_compare(op, a, r->boolean);
            return true;
        }
        bool numeric = r->type == XT_NUMBER || !equality;
        double want = 0;
        if (numeric && !xv_number(e, r, &want)) return false;
        for (size_t i = 0; i < set->n && !*result; i++) {
            struct xs s;
            if (!xp_string_value(e, &set->v[i], &s)) return false;
            bool ok = true;
            if (numeric) {
                double d;
                ok = xp_number_of(e, s.p, s.n, &d);
                *result = ok && xp_num_compare(op, d, want);
            } else *result = xs_equal(&s, &r->string) == (op == X_EQ);
            free(s.own);
            if (!ok) return false;
        }
        return true;
    }
    if (equality) {
        bool same;
        if (l->type == XT_BOOLEAN || r->type == XT_BOOLEAN) same = xv_boolean(l) == xv_boolean(r);
        else if (l->type == XT_NUMBER || r->type == XT_NUMBER) {
            double a, b;
            if (!xv_number(e, l, &a) || !xv_number(e, r, &b)) return false;
            *result = xp_num_compare(op, a, b);
            return true;
        } else same = xs_equal(&l->string, &r->string);
        *result = same == (op == X_EQ);
        return true;
    }
    double a, b;
    if (!xv_number(e, l, &a) || !xv_number(e, r, &b)) return false;
    *result = xp_num_compare(op, a, b);
    return true;
}

/* Keeps the nodes of set (in axis order) for which the predicate is true at
   their proximity position: a number equals the position, else boolean(). */
static bool xp_predicate(struct xp_eval *e, const struct xp_expr *predicate, struct xp_set *set) {
    size_t size = set->n, kept = 0;
    if (predicate->op == X_NUMBER) {
        double want = predicate->number;
        if (want >= 1 && want <= (double)size && want == floor(want)) { set->v[0] = set->v[(size_t)want - 1]; set->n = 1; }
        else set->n = 0;
        return true;
    }
    for (size_t i = 0; i < size; i++) {
        struct xp_ctx c = {set->v[i], i + 1, size};
        struct xp_value v;
        if (!xp_eval(e, predicate, &c, &v)) return false;
        bool keep = v.type == XT_NUMBER ? v.number == (double)(i + 1) : xv_boolean(&v);
        xv_free(&v);
        if (keep) set->v[kept++] = c.node;
    }
    set->n = kept;
    return true;
}
static bool xp_reverse_axis(uint8_t axis) {
    return axis == A_ANCESTOR || axis == A_ANCESTOR_OR_SELF || axis == A_PRECEDING || axis == A_PRECEDING_SIBLING;
}
/* Whether the step's predicates hold of a node independently of its proximity
   position and the context size: then the step's result from y is contained
   in its result from x whenever y's axis is contained in x's. */
static bool xp_step_independent(const struct xp_step *s) {
    for (int k = 0; k < s->npredicates; k++)
        if (s->predicates[k]->type == XT_NUMBER || xp_positional(s->predicates[k])) return false;
    return true;
}
/* Whether y lies within x's subtree, x excluded; an attribute or namespace
   node lies within its element. */
static bool xp_within(const struct xp_node *y, const struct xp_node *x) {
    if (x->kind != XN_NODE) return false;
    for (const node_t *n = y->kind == XN_NODE ? y->node->parent : y->node; n; n = n->parent)
        if (n == x->node) return true;
    return false;
}
/* A set of DOM node addresses. */
struct xp_pointers { const node_t **v; size_t mask, n; };
static int xp_pointers_add(struct xp_eval *e, struct xp_pointers *p, const node_t *key) {
    if (p->n * 2 >= p->mask) {
        size_t cap = p->mask ? (p->mask + 1) * 2 : 64;
        const node_t **v = cap <= SIZE_MAX / sizeof *v ? calloc(cap, sizeof *v) : NULL;
        if (!v) { xp_oom(e); return -1; }
        for (size_t i = 0; p->mask && i <= p->mask; i++) if (p->v[i]) {
            size_t h = xp_hash(p->v[i], cap - 1);
            while (v[h]) h = (h + 1) & (cap - 1);
            v[h] = p->v[i];
        }
        free(p->v);
        p->v = v; p->mask = cap - 1;
    }
    size_t h = xp_hash(key, p->mask);
    for (; p->v[h]; h = (h + 1) & p->mask) if (p->v[h] == key) return 0;
    p->v[h] = key;
    p->n++;
    return 1;
}
/* The ancestor axes from x without the nodes visited from earlier inputs:
   the ancestors of a visited node were visited with it. */
static bool xp_new_ancestors(struct xp_eval *e, const struct xp_step *s, const struct xp_node *x,
                             struct xp_pointers *visited, struct xp_set *out) {
    if (s->axis == A_ANCESTOR_OR_SELF) {
        if (x->kind == XN_NODE) {
            int added = xp_pointers_add(e, visited, x->node);
            if (added <= 0) return added == 0;
        }
        if (xp_test(e, s, x) && !xp_push(e, out, *x)) return false;
    }
    for (node_t *p = x->kind == XN_NODE ? x->node->parent : x->node; p; p = p->parent) {
        if (!xp_visit(e)) return false;
        int added = xp_pointers_add(e, visited, p);
        if (added <= 0) return added == 0;
        struct xp_node y = xp_plain(p);
        if (xp_test(e, s, &y) && !xp_push(e, out, y)) return false;
    }
    return true;
}
/* One location step from every node of in, the union in document order.
   With position-independent predicates, inputs whose results an earlier
   input's already contain are skipped: the descendants of a node inside an
   input walked before, the siblings of a node whose parent was covered, the
   following nodes of all but the input whose subtree ends first, the
   preceding nodes of all but the last input, and the ancestors met from an
   earlier input. A first predicate that is a proximity position k stops each
   axis walk at its k-th match. */
static bool xp_step_eval(struct xp_eval *e, const struct xp_step *s, const struct xp_set *in, struct xp_set *out) {
    bool reverse = xp_reverse_axis(s->axis), ok = true, covering = in->n > 1 && xp_step_independent(s);
    size_t from = 0, to = in->n, limit = 0;
    if (covering && s->axis == A_FOLLOWING) {
        size_t first = 0;
        for (size_t i = 1; i < in->n && xp_within(&in->v[i], &in->v[first]); i++) first = i;
        from = first; to = first + 1;
    } else if (covering && s->axis == A_PRECEDING) from = in->n - 1;
    if (s->npredicates && s->predicates[0]->op == X_NUMBER) {
        double k = s->predicates[0]->number;
        if (k >= 1 && k == floor(k) && k < 4294967296.0) limit = (size_t)k;
    }
    bool descendants = covering && (s->axis == A_DESCENDANT || s->axis == A_DESCENDANT_OR_SELF);
    bool siblings = covering && (s->axis == A_FOLLOWING_SIBLING || s->axis == A_PRECEDING_SIBLING);
    bool ancestors = covering && (s->axis == A_ANCESTOR || s->axis == A_ANCESTOR_OR_SELF);
    const struct xp_node *walked = NULL;
    struct xp_pointers seen = {0};
    struct xp_set part = {0};
    for (size_t i = from; ok && i < to; i++) {
        /* Preceding siblings: the last input under each parent covers the others. */
        const struct xp_node *x = &in->v[siblings && s->axis == A_PRECEDING_SIBLING ? in->n - 1 - i : i];
        if (descendants && x->kind == XN_NODE) {
            if (walked && xp_within(x, walked)) continue;
            walked = x;
        }
        if (siblings && x->kind == XN_NODE && x->node->parent) {
            int added = xp_pointers_add(e, &seen, x->node->parent);
            if (added < 0) { ok = false; break; }
            if (!added) continue;
        }
        part.n = 0;
        if (ancestors) ok = xp_new_ancestors(e, s, x, &seen, &part);
        else {
            e->axis_limit = limit;
            e->axis_full = false;
            ok = xp_axis(e, s, x, &part) || e->axis_full;
            e->axis_limit = 0;
        }
        for (int k = 0; ok && k < s->npredicates; k++) ok = xp_predicate(e, s->predicates[k], &part);
        if (!ok) break;
        if (reverse)
            for (size_t a = 0, b = part.n; a + 1 < b; a++, b--) {
                struct xp_node t = part.v[a]; part.v[a] = part.v[b - 1]; part.v[b - 1] = t;
            }
        if (!out->n) { free(out->v); *out = part; part = (struct xp_set){0}; continue; }
        for (size_t j = 0; ok && j < part.n; j++) ok = xp_push(e, out, part.v[j]);
    }
    free(part.v);
    free(seen.v);
    return ok && (in->n < 2 || xp_sort(e, out));
}
static bool xp_names(const struct xp_node *x, const char **local, const char **ns, const char **qname) {
    *local = *ns = *qname = "";
    const node_t *n = x->node;
    const struct attr *a = NULL;
    if (x->kind == XN_ATTR) a = &n->attrs[x->index];
    else if (x->kind == XN_NAMESPACE) { *local = *qname = x->prefix ? x->prefix : ""; return true; }
    else if (n->type == N_ATTR) a = n->attribute;
    else if (n->type == N_ELEM) {
        *local = xp_element_local(n);
        *qname = xp_element_qualified(n);
        const char *uri = xp_element_namespace(n);
        if (uri) *ns = uri;
        return true;
    } else if (n->type == N_PI) { *local = *qname = n->name ? n->name : ""; return true; }
    if (a) {
        *local = xp_attr_local(a);
        *qname = a->raw ? a->raw : *local;
        if (a->namespace_uri) *ns = a->namespace_uri;
    }
    return true;
}
/* lang(): the nearest xml:lang on the context node or its ancestors equals the
   argument, or begins with it and '-', ASCII case-insensitively. */
static bool xp_lang(const struct xp_node *x, const struct xs *want) {
    const node_t *n = x->node;
    if (x->kind == XN_NODE && n->type != N_ELEM) n = n->type == N_ATTR ? n->attr_owner : n->parent;
    for (; n; n = n->parent) {
        if (n->type != N_ELEM) continue;
        for (int i = 0; i < n->nattrs; i++) {
            const struct attr *a = &n->attrs[i];
            if (!a->namespace_uri || strcmp(a->namespace_uri, XP_NS_XML) || strcmp(xp_attr_local(a), "lang")) continue;
            const char *value = a->value ? a->value : "";
            size_t length = strlen(value);
            if (length < want->n) return false;
            for (size_t k = 0; k < want->n; k++)
                if (lower((unsigned char)value[k]) != lower((unsigned char)want->p[k])) return false;
            return length == want->n || value[want->n] == '-';
        }
    }
    return false;
}
struct xp_id_token { const char *p; size_t n; bool found; };
static int xp_id_order(const void *a, const void *b) {
    const struct xp_id_token *x = a, *y = b;
    if (x->n != y->n) return x->n < y->n ? -1 : 1;
    return memcmp(x->p, y->p, x->n);
}
/* id(): the elements whose DOM ID is one of the whitespace-separated tokens,
   the first in tree order of each ID, in the context node's tree. */
static bool xp_id(struct xp_eval *e, const struct xp_expr *arg, const struct xp_ctx *c, struct xp_value *out) {
    struct xp_value v;
    if (!xp_eval(e, arg, c, &v)) return false;
    struct xbuf text = {0};
    bool ok = true;
    if (v.type == XT_NODESET) {
        for (size_t i = 0; ok && i < v.set.n; i++) {
            struct xs s;
            if (!(ok = xp_string_value(e, &v.set.v[i], &s))) break;
            ok = xb_put(e, &text, s.p, s.n) && xb_put(e, &text, " ", 1);
            free(s.own);
        }
    } else {
        struct xs s;
        if ((ok = xv_string(e, &v, &s))) { ok = xb_put(e, &text, s.p, s.n); free(s.own); }
    }
    xv_free(&v);
    struct xp_id_token *tokens = NULL;
    size_t count = 0, cap = 0, remaining = 0;
    for (size_t i = 0; ok && i < text.n;) {
        while (i < text.n && xp_space((unsigned char)text.p[i])) i++;
        size_t start = i;
        while (i < text.n && !xp_space((unsigned char)text.p[i])) i++;
        if (i == start) break;
        if (count == cap) {
            size_t grown_cap = cap ? cap * 2 : 8;
            struct xp_id_token *grown = realloc(tokens, grown_cap * sizeof *grown);
            if (!grown) { ok = xp_oom(e); break; }
            tokens = grown; cap = grown_cap;
        }
        tokens[count++] = (struct xp_id_token){text.p + start, i - start, false};
    }
    if (ok && count) {
        qsort(tokens, count, sizeof *tokens, xp_id_order);
        size_t unique = 1;
        for (size_t i = 1; i < count; i++) if (xp_id_order(&tokens[unique - 1], &tokens[i])) tokens[unique++] = tokens[i];
        count = remaining = unique;
    }
    out->type = XT_NODESET;
    for (node_t *d = e->root; ok && remaining && d; d = xp_next_in(d, e->root)) {
        if (!(ok = xp_visit(e))) break;
        if (d->type != N_ELEM || !d->id) continue;
        struct xp_id_token key = {d->id, strlen(d->id), false};
        struct xp_id_token *hit = bsearch(&key, tokens, count, sizeof *tokens, xp_id_order);
        if (!hit || hit->found) continue;
        hit->found = true;
        remaining--;
        ok = xp_push(e, &out->set, xp_plain(d));
    }
    free(tokens);
    free(text.p);
    return ok;
}
static bool xp_substring(struct xp_eval *e, const struct xp_expr *x, const struct xp_ctx *c, struct xp_value *out) {
    struct xs s;
    double start, length = INFINITY;
    if (!xp_eval_string(e, x->items[0], c, &s)) return false;
    if (!xp_eval_number(e, x->items[1], c, &start) || (x->count > 2 && !xp_eval_number(e, x->items[2], c, &length))) {
        free(s.own); return false;
    }
    /* The characters at positions p with round(start) <= p < round(start) + round(length). */
    double first = xp_round(start), end = x->count > 2 ? first + xp_round(length) : INFINITY;
    size_t from = SIZE_MAX, to = 0;
    double position = 1;
    for (size_t i = 0; i < s.n; position++) {
        size_t size = xp_char_size(s.p + i, s.n - i);
        if (position >= first && position < end) { if (from == SIZE_MAX) from = i; to = i + size; }
        else if (from != SIZE_MAX) break;
        i += size;
    }
    out->type = XT_STRING;
    if (from == SIZE_MAX) { free(s.own); out->string = xs_borrow("", 0); return true; }
    out->string = (struct xs){s.p + from, to - from, s.own};
    return true;
}
static const char *xp_find(const char *h, size_t hn, const char *needle, size_t nn) {
    if (!nn) return h;
    for (size_t i = 0; nn <= hn && i <= hn - nn; i++)
        if (h[i] == needle[0] && !memcmp(h + i, needle, nn)) return h + i;
    return NULL;
}
static bool xp_translate(struct xp_eval *e, const struct xs *s, const struct xs *from, const struct xs *to, struct xs *out) {
    struct xbuf b = {0};
    for (size_t i = 0; i < s->n;) {
        size_t size = xp_char_size(s->p + i, s->n - i);
        const char *replacement = s->p + i;
        size_t replacement_size = size;
        /* The first occurrence in from picks the character at the same position in to. */
        size_t index = 0;
        for (size_t j = 0; j < from->n; index++) {
            size_t from_size = xp_char_size(from->p + j, from->n - j);
            if (from_size == size && !memcmp(from->p + j, s->p + i, size)) {
                replacement = NULL; replacement_size = 0;
                size_t k = 0, at = 0;
                while (k < to->n && at < index) { k += xp_char_size(to->p + k, to->n - k); at++; }
                if (k < to->n) { replacement = to->p + k; replacement_size = xp_char_size(to->p + k, to->n - k); }
                break;
            }
            j += from_size;
        }
        if (replacement && !xb_put(e, &b, replacement, replacement_size)) { free(b.p); return false; }
        i += size;
    }
    *out = xb_take(&b);
    return true;
}
static bool xp_normalize_space(struct xp_eval *e, const struct xs *s, struct xs *out) {
    struct xbuf b = {0};
    bool space = false;
    for (size_t i = 0; i < s->n; i++) {
        if (xp_space((unsigned char)s->p[i])) { space = b.n != 0; continue; }
        if (space && !xb_put(e, &b, " ", 1)) { free(b.p); return false; }
        space = false;
        if (!xb_put(e, &b, s->p + i, 1)) { free(b.p); return false; }
    }
    *out = xb_take(&b);
    return true;
}
static bool xp_function(struct xp_eval *e, const struct xp_expr *x, const struct xp_ctx *c, struct xp_value *out) {
    out->type = x->type;
    switch (x->fn) {
    case F_LAST: out->number = (double)c->size; return true;
    case F_POSITION: out->number = (double)c->position; return true;
    case F_COUNT: {
        struct xp_set set;
        if (!xp_eval_set(e, x->items[0], c, &set)) return false;
        out->number = (double)set.n;
        free(set.v);
        return true;
    }
    case F_ID: return xp_id(e, x->items[0], c, out);
    case F_LOCAL_NAME: case F_NAMESPACE_URI: case F_NAME: {
        struct xp_set set = {0};
        const struct xp_node *node = &c->node;
        if (x->count) {
            if (!xp_eval_set(e, x->items[0], c, &set)) return false;
            node = set.n ? &set.v[0] : NULL;
        }
        const char *local = "", *ns = "", *qname = "";
        if (node) xp_names(node, &local, &ns, &qname);
        const char *text = x->fn == F_LOCAL_NAME ? local : x->fn == F_NAMESPACE_URI ? ns : qname;
        out->string = xs_borrow(text, strlen(text)); /* the DOM's strings outlive the evaluation */
        free(set.v);
        return true;
    }
    case F_STRING: return xp_string_or_context(e, x, c, &out->string);
    case F_CONCAT: {
        struct xbuf b = {0};
        for (int i = 0; i < x->count; i++) {
            struct xs s;
            if (!xp_eval_string(e, x->items[i], c, &s)) { free(b.p); return false; }
            bool ok = xb_put(e, &b, s.p, s.n);
            free(s.own);
            if (!ok) { free(b.p); return false; }
        }
        out->string = xb_take(&b);
        return true;
    }
    case F_STARTS_WITH: case F_CONTAINS: case F_SUBSTRING_BEFORE: case F_SUBSTRING_AFTER: {
        struct xs a, b;
        if (!xp_eval_string(e, x->items[0], c, &a)) return false;
        if (!xp_eval_string(e, x->items[1], c, &b)) { free(a.own); return false; }
        const char *at = x->fn == F_STARTS_WITH ? (b.n <= a.n && !memcmp(a.p, b.p, b.n) ? a.p : NULL) : xp_find(a.p, a.n, b.p, b.n);
        free(b.own);
        if (x->fn == F_STARTS_WITH || x->fn == F_CONTAINS) { out->boolean = at != NULL; free(a.own); return true; }
        if (!at) { free(a.own); out->string = xs_borrow("", 0); return true; }
        /* The result shares a's allocation. */
        out->string = x->fn == F_SUBSTRING_BEFORE ? (struct xs){a.p, (size_t)(at - a.p), a.own} :
                                                     (struct xs){at + b.n, a.n - (size_t)(at - a.p) - b.n, a.own};
        return true;
    }
    case F_SUBSTRING: return xp_substring(e, x, c, out);
    case F_STRING_LENGTH: {
        struct xs s;
        if (!xp_string_or_context(e, x, c, &s)) return false;
        size_t length = 0;
        for (size_t i = 0; i < s.n; length++) i += xp_char_size(s.p + i, s.n - i);
        free(s.own);
        out->number = (double)length;
        return true;
    }
    case F_NORMALIZE_SPACE: {
        struct xs s;
        if (!xp_string_or_context(e, x, c, &s)) return false;
        bool ok = xp_normalize_space(e, &s, &out->string);
        free(s.own);
        return ok;
    }
    case F_TRANSLATE: {
        struct xs s[3];
        int made = 0;
        bool ok = true;
        for (; ok && made < 3; made++) ok = xp_eval_string(e, x->items[made], c, &s[made]);
        if (!ok) made--;
        if (ok) ok = xp_translate(e, &s[0], &s[1], &s[2], &out->string);
        for (int i = 0; i < made; i++) free(s[i].own);
        return ok;
    }
    case F_BOOLEAN: case F_NOT: {
        struct xp_value v;
        if (!xp_eval(e, x->items[0], c, &v)) return false;
        out->boolean = xv_boolean(&v) != (x->fn == F_NOT);
        xv_free(&v);
        return true;
    }
    case F_TRUE: out->boolean = true; return true;
    case F_FALSE: out->boolean = false; return true;
    case F_LANG: {
        struct xs s;
        if (!xp_eval_string(e, x->items[0], c, &s)) return false;
        out->boolean = xp_lang(&c->node, &s);
        free(s.own);
        return true;
    }
    case F_NUMBER: {
        if (x->count) return xp_eval_number(e, x->items[0], c, &out->number);
        struct xs s;
        if (!xp_string_value(e, &c->node, &s)) return false;
        bool ok = xp_number_of(e, s.p, s.n, &out->number);
        free(s.own);
        return ok;
    }
    case F_SUM: {
        struct xp_set set;
        if (!xp_eval_set(e, x->items[0], c, &set)) return false;
        double total = 0;
        bool ok = true;
        for (size_t i = 0; ok && i < set.n; i++) {
            struct xs s;
            double d;
            if (!(ok = xp_string_value(e, &set.v[i], &s))) break;
            ok = xp_number_of(e, s.p, s.n, &d);
            free(s.own);
            total += d;
        }
        free(set.v);
        out->number = total;
        return ok;
    }
    case F_FLOOR: case F_CEILING: case F_ROUND: {
        double d;
        if (!xp_eval_number(e, x->items[0], c, &d)) return false;
        out->number = x->fn == F_FLOOR ? floor(d) : x->fn == F_CEILING ? ceil(d) : xp_round(d);
        return true;
    }
    default: return true;
    }
}
static bool xp_eval(struct xp_eval *e, const struct xp_expr *x, const struct xp_ctx *c, struct xp_value *out) {
    *out = (struct xp_value){.string = xs_borrow("", 0)};
    if (xp_stack_exhausted(e->stack_base)) { e->status = XPATH_DEPTH; return false; }
    switch (x->op) {
    case X_OR: case X_AND: {
        struct xp_value v;
        if (!xp_eval(e, x->left, c, &v)) return false;
        bool decided = xv_boolean(&v);
        xv_free(&v);
        out->type = XT_BOOLEAN;
        /* The right operand is evaluated only when the left one does not decide. */
        if (decided == (x->op == X_OR)) { out->boolean = decided; return true; }
        if (!xp_eval(e, x->right, c, &v)) return false;
        out->boolean = xv_boolean(&v);
        xv_free(&v);
        return true;
    }
    case X_EQ: case X_NE: case X_LT: case X_LE: case X_GT: case X_GE: {
        struct xp_value l, r;
        if (!xp_eval(e, x->left, c, &l)) return false;
        if (!xp_eval(e, x->right, c, &r)) { xv_free(&l); return false; }
        bool result, ok = xp_compare(e, x->op, &l, &r, &result);
        xv_free(&l); xv_free(&r);
        out->type = XT_BOOLEAN; out->boolean = result;
        return ok;
    }
    case X_ADD: case X_SUB: case X_MUL: case X_DIV: case X_MOD: {
        double a, b;
        if (!xp_eval_number(e, x->left, c, &a) || !xp_eval_number(e, x->right, c, &b)) return false;
        out->type = XT_NUMBER;
        out->number = x->op == X_ADD ? a + b : x->op == X_SUB ? a - b : x->op == X_MUL ? a * b :
                      x->op == X_DIV ? a / b : fmod(a, b);
        return true;
    }
    case X_NEG: {
        double a;
        if (!xp_eval_number(e, x->left, c, &a)) return false;
        out->type = XT_NUMBER; out->number = -a;
        return true;
    }
    case X_UNION: {
        struct xp_set a, b;
        if (!xp_eval_set(e, x->left, c, &a)) return false;
        if (!xp_eval_set(e, x->right, c, &b)) { free(a.v); return false; }
        out->type = XT_NODESET;
        return xp_merge(e, &a, &b, &out->set);
    }
    case X_LITERAL: out->type = XT_STRING; out->string = xs_borrow(x->text, x->length); return true;
    case X_NUMBER: out->type = XT_NUMBER; out->number = x->number; return true;
    case X_FUNCTION: return xp_function(e, x, c, out);
    case X_FILTER: {
        struct xp_set set;
        if (!xp_eval_set(e, x->left, c, &set)) return false;
        /* A filter's predicates count positions along the child axis: document order. */
        for (int k = 0; k < x->count; k++) if (!xp_predicate(e, x->items[k], &set)) { free(set.v); return false; }
        out->type = XT_NODESET; out->set = set;
        return true;
    }
    case X_PATH: {
        struct xp_set current = {0};
        if (x->start) { if (!xp_eval_set(e, x->start, c, &current)) return false; }
        else if (!xp_push(e, &current, x->absolute ? xp_root(&c->node) : c->node)) return false;
        for (int i = 0; i < x->nsteps; i++) {
            struct xp_set next = {0};
            bool ok = xp_step_eval(e, x->steps[i], &current, &next);
            free(current.v);
            current = next;
            if (!ok) { free(current.v); return false; }
        }
        out->type = XT_NODESET; out->set = current;
        return true;
    }
    default: return true;
    }
}

/* DOM Level 3 XPath: the context node is a Document, Element, Attr, Text,
   Comment or ProcessingInstruction; a Text stands for its whole logical text
   node, which must not be empty. */
static bool xp_context_node(node_t *n, struct xp_node *out) {
    switch (n->type) {
    case N_DOC: case N_ELEM: case N_COMMENT: case N_PI: *out = xp_plain(n); return true;
    case N_TEXT: {
        while (n->prev && n->prev->type == N_TEXT) n = n->prev;
        while (n && n->type == N_TEXT && !n->textlen) n = n->next;
        if (!n || n->type != N_TEXT) return false;
        *out = xp_plain(n);
        return true;
    }
    case N_ATTR: {
        node_t *owner = n->attr_owner;
        if (owner) for (int i = 0; i < owner->nattrs; i++)
            if (owner->attrs[i].node == n) { *out = (struct xp_node){.node = owner, .index = i, .kind = XN_ATTR}; return true; }
        *out = xp_plain(n); /* no element: no parent */
        return true;
    }
    default: return false;
    }
}
enum xpath_status xpath_evaluate(const struct xpath_expr *expr, node_t *context, unsigned type, struct xpath_result *out) {
    memset(out, 0, sizeof *out);
    struct xp_node start;
    if (!expr || !context || !xp_context_node(context, &start)) return XPATH_CONTEXT;
    struct xp_eval e = {.doc = context->owner, .html = xp_html_document(context->owner),
                        .stack_base = __builtin_frame_address(0)};
    e.root = xp_root(&start).node;
    struct xp_ctx c = {start, 1, 1};
    struct xp_value v;
    enum xpath_status status = XPATH_OK;
    if (!xp_eval(&e, expr->root, &c, &v)) {
        status = e.status ? e.status : XPATH_OOM;
        goto finished;
    }
    if (type > XPATH_FIRST_ORDERED_NODE_TYPE) { status = XPATH_RESULT_TYPE; goto release; }
    if (type == XPATH_ANY_TYPE)
        type = v.type == XT_NODESET ? XPATH_UNORDERED_NODE_ITERATOR_TYPE : v.type == XT_NUMBER ? XPATH_NUMBER_TYPE :
               v.type == XT_STRING ? XPATH_STRING_TYPE : XPATH_BOOLEAN_TYPE;
    out->type = type;
    switch (type) {
    case XPATH_NUMBER_TYPE:
        if (!xv_number(&e, &v, &out->number)) status = e.status;
        break;
    case XPATH_STRING_TYPE: {
        struct xs s;
        if (!xv_string(&e, &v, &s)) { status = e.status; break; }
        out->string = s.n < SIZE_MAX ? malloc(s.n + 1) : NULL;
        if (out->string) { memcpy(out->string, s.p, s.n); out->string[s.n] = 0; out->string_length = s.n; }
        else status = XPATH_OOM;
        free(s.own);
        break;
    }
    case XPATH_BOOLEAN_TYPE: out->boolean = xv_boolean(&v); break;
    default: {
        if (v.type != XT_NODESET) { status = XPATH_TYPE; break; }
        size_t count = type >= XPATH_ANY_UNORDERED_NODE_TYPE ? (v.set.n ? 1 : 0) : v.set.n;
        if (!count) break;
        out->nodes = count <= SIZE_MAX / sizeof *out->nodes ? malloc(count * sizeof *out->nodes) : NULL;
        if (!out->nodes) { status = XPATH_OOM; break; }
        for (size_t i = 0; i < count; i++) {
            const struct xp_node *x = &v.set.v[i];
            if (x->kind == XN_NAMESPACE) { status = XPATH_NAMESPACE_NODE; break; }
            node_t *n = x->kind == XN_ATTR ? doc_attr_node(x->node->owner, x->node, x->index) : x->node;
            if (!n) { status = XPATH_OOM; break; }
            out->nodes[i] = n;
        }
        if (!status) out->count = count;
    }
    }
release:
    xv_free(&v);
finished:
    free(e.order.keys);
    free(e.order.ranks);
    if (status) xpath_result_free(out);
    return status;
}
void xpath_result_free(struct xpath_result *result) {
    free(result->string);
    free(result->nodes);
    memset(result, 0, sizeof *result);
}
