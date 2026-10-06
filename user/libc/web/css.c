/* CSS: parsing stylesheets (rules, @media, @import, @supports, nesting), selectors, an index of
   the rules by id, class and tag, and the cascade that computes every element's style. */
#include <stdio.h>
#include <ctype.h>
#include <stdarg.h>
#include "webi.h"

/* ---------------------------------------------------------------- data structures */
enum { SK_TAG, SK_ID, SK_CLASS, SK_ATTR, SK_PC };
enum { AO_EXISTS, AO_EQ, AO_INCL, AO_DASH, AO_PREFIX, AO_SUFFIX, AO_SUBSTR };
enum { PC_FIRST_CHILD, PC_LAST_CHILD, PC_ONLY_CHILD, PC_NTH_CHILD, PC_NTH_LAST_CHILD, PC_FIRST_OF_TYPE,
       PC_LAST_OF_TYPE, PC_ONLY_OF_TYPE, PC_NTH_OF_TYPE, PC_NTH_LAST_OF_TYPE, PC_NOT, PC_IS, PC_NEVER,
       PC_ALWAYS, PC_LINK, PC_CHECKED, PC_DISABLED, PC_ENABLED, PC_ROOT, PC_EMPTY, PC_REQUIRED, PC_OPTIONAL,
       PC_LANG, PC_PLACEHOLDER_SHOWN, PC_READ_WRITE, PC_READ_ONLY, PC_OPEN };

struct sellist;
struct simple {
    uint8_t kind, op, pc;
    bool icase;
    uint16_t tag;
    const char *name, *value;
    int a, b;
    struct sellist *args;
};
struct compound {
    struct simple *s;
    int n;
    char comb; /* how it relates to the compound on its left: ' ', '>', '+', '~' */
    struct compound *left;
};
struct selector {
    struct compound *right;
    uint32_t spec;
    uint8_t pseudo;
};
struct sellist {
    struct selector **v;
    int n;
};

struct decl {
    const struct propdef *p; /* NULL: a custom property */
    const char *name;        /* custom properties */
    const char *value;
    bool important;
};

struct mcond {
    const char *q;
    const struct mcond *up;
};

struct rule {
    struct sellist *sel;
    struct decl *decls;
    int ndecls;
    const struct mcond *media;
    struct rule *next;
};

struct sheet {
    struct rule *first, *last;
    double order;
    bool ua;
};

struct pctx {
    arena_t *a;
    sheet_t *sh;
    const char *base;
    pvec *imports;
    int nimports;
};

/* ---------------------------------------------------------------- scanning */
/* the first of stops at nesting depth 0, skipping strings, escapes, () and [] */
static const char *scan_to(const char *s, const char *e, const char *stops) {
    int depth = 0;
    while (s < e) {
        char c = *s;
        if (c == '\\') {
            s += 2;
            continue;
        }
        if (c == '"' || c == '\'') {
            s++;
            while (s < e && *s != c) {
                if (*s == '\\') s++;
                else if (*s == '\n') break;
                s++;
            }
            s++;
            continue;
        }
        if (depth == 0 && c && strchr(stops, c)) return s;
        if (c == '(' || c == '[') depth++;
        else if ((c == ')' || c == ']') && depth) depth--;
        s++;
    }
    return e;
}

/* s is just after '{': the matching '}' (or e) */
static const char *block_end(const char *s, const char *e) {
    int depth = 1;
    while (s < e) {
        char c = *s;
        if (c == '\\') {
            s += 2;
            continue;
        }
        if (c == '"' || c == '\'') {
            s++;
            while (s < e && *s != c) {
                if (*s == '\\') s++;
                else if (*s == '\n') break;
                s++;
            }
            s++;
            continue;
        }
        if (c == '{') depth++;
        else if (c == '}' && --depth == 0) return s;
        s++;
    }
    return e;
}

static const char *skip_ws(const char *s, const char *e) {
    while (s < e && is_space((unsigned char)*s)) s++;
    return s;
}

static void trim_r(const char *s, const char **e) {
    while (*e > s && is_space((unsigned char)(*e)[-1])) (*e)--;
}

static bool ident_char(unsigned char c) { return isalnum(c) || c == '-' || c == '_' || c >= 0x80; }

/* an identifier with escapes resolved, or NULL */
static const char *read_ident(arena_t *a, const char **ps, const char *e, bool lowercase) {
    const char *s = *ps;
    char buf[256];
    size_t n = 0;
    while (s < e) {
        unsigned char c = (unsigned char)*s;
        if (ident_char(c)) {
            if (n < sizeof buf - 1) buf[n++] = lowercase ? (char)lower(c) : (char)c;
            s++;
        } else if (c == '\\' && s + 1 < e && s[1] != '\n') {
            s++;
            if (isxdigit((unsigned char)*s)) {
                uint32_t cp = 0;
                int k = 0;
                while (k < 6 && s < e && isxdigit((unsigned char)*s)) {
                    int d = *s | 32;
                    cp = cp * 16 + (uint32_t)(d <= '9' ? d - '0' : d - 'a' + 10);
                    s++, k++;
                }
                if (s < e && is_space((unsigned char)*s)) s++;
                char u[4];
                int ul = utf8_put(u, cp ? cp : 0xFFFD);
                if (n + (size_t)ul < sizeof buf - 1) memcpy(buf + n, u, (size_t)ul), n += (size_t)ul;
            } else {
                if (n < sizeof buf - 1) buf[n++] = *s;
                s++;
            }
        } else break;
    }
    if (!n) return NULL;
    *ps = s;
    return ar_strndup(a, buf, n);
}

/* ---------------------------------------------------------------- selectors */
static struct sellist *parse_sellist(struct pctx *pc, const char *s, const char *e, bool forgiving);

static bool parse_anb(const char *s, const char *e, int *a, int *b) {
    char buf[64];
    size_t n = 0;
    for (; s < e && n < sizeof buf - 1; s++)
        if (!is_space((unsigned char)*s)) buf[n++] = (char)lower((unsigned char)*s);
    buf[n] = 0;
    if (!strcmp(buf, "odd")) return *a = 2, *b = 1, true;
    if (!strcmp(buf, "even")) return *a = 2, *b = 0, true;
    const char *p = buf;
    char *np = strchr(buf, 'n');
    if (np) {
        if (p == np) *a = 1;
        else if (np - p == 1 && *p == '-') *a = -1;
        else if (np - p == 1 && *p == '+') *a = 1;
        else {
            char *end;
            *a = (int)strtol(p, &end, 10);
            if (end != np) return false;
        }
        p = np + 1;
        if (!*p) return *b = 0, true;
        if (*p != '+' && *p != '-') return false;
    } else *a = 0;
    char *end;
    *b = (int)strtol(p, &end, 10);
    return *end == 0 && end != p;
}

static uint32_t spec_add(uint32_t x, uint32_t y) {
    uint32_t a = (x >> 20) + (y >> 20), b = ((x >> 10) & 1023) + ((y >> 10) & 1023), c = (x & 1023) + (y & 1023);
    if (a > 1023) a = 1023;
    if (b > 1023) b = 1023;
    if (c > 1023) c = 1023;
    return a << 20 | b << 10 | c;
}
#define SPEC_A (1u << 20)
#define SPEC_B (1u << 10)
#define SPEC_C 1u

static uint32_t sellist_max_spec(const struct sellist *l) {
    uint32_t m = 0;
    for (int i = 0; l && i < l->n; i++)
        if (l->v[i]->spec > m) m = l->v[i]->spec;
    return m;
}

static const struct {
    const char *name;
    uint8_t pc;
} simple_pcs[] = {
    {"first-child", PC_FIRST_CHILD}, {"last-child", PC_LAST_CHILD}, {"only-child", PC_ONLY_CHILD},
    {"first-of-type", PC_FIRST_OF_TYPE}, {"last-of-type", PC_LAST_OF_TYPE}, {"only-of-type", PC_ONLY_OF_TYPE},
    {"link", PC_LINK}, {"any-link", PC_LINK}, {"checked", PC_CHECKED}, {"disabled", PC_DISABLED},
    {"enabled", PC_ENABLED}, {"root", PC_ROOT}, {"scope", PC_ROOT}, {"empty", PC_EMPTY},
    {"required", PC_REQUIRED}, {"optional", PC_OPTIONAL}, {"placeholder-shown", PC_PLACEHOLDER_SHOWN},
    {"read-write", PC_READ_WRITE}, {"read-only", PC_READ_ONLY}, {"open", PC_OPEN}, {"defined", PC_ALWAYS},
    {"visited", PC_NEVER}, {"hover", PC_NEVER}, {"active", PC_NEVER}, {"focus", PC_NEVER},
    {"focus-within", PC_NEVER}, {"focus-visible", PC_NEVER}, {"target", PC_NEVER}, {"target-within", PC_NEVER},
    {"indeterminate", PC_NEVER}, {"invalid", PC_NEVER}, {"valid", PC_ALWAYS}, {"user-invalid", PC_NEVER},
    {"user-valid", PC_NEVER}, {"default", PC_NEVER}, {"fullscreen", PC_NEVER}, {"modal", PC_NEVER},
    {"popover-open", PC_NEVER}, {"autofill", PC_NEVER}, {"in-range", PC_ALWAYS}, {"out-of-range", PC_NEVER},
    {"playing", PC_NEVER}, {"paused", PC_NEVER}, {"current", PC_NEVER}, {"past", PC_NEVER}, {"future", PC_NEVER},
    {"first", PC_NEVER}, {"left", PC_NEVER}, {"right", PC_NEVER}, {"blank", PC_NEVER}, {"local-link", PC_NEVER},
    {"host", PC_NEVER}, {"state", PC_NEVER}, {"picture-in-picture", PC_NEVER}, {"-webkit-autofill", PC_NEVER},
    {"-moz-focusring", PC_NEVER}, {"-moz-ui-invalid", PC_NEVER},
};

/* one compound selector; *ps advanced past it. pseudo receives a pseudo-element. */
static struct compound *parse_compound(struct pctx *pc, const char **ps, const char *e, uint32_t *spec, uint8_t *pseudo) {
    struct simple tmp[32];
    int n = 0;
    const char *s = *ps;
    while (s < e && n < 32) {
        char c = *s;
        struct simple *x = &tmp[n];
        memset(x, 0, sizeof *x);
        if (*pseudo && c != ':') return NULL; /* nothing may follow a pseudo-element but pseudo-classes */
        if (c == '*') {
            s++;
            if (s < e && *s == '|') return NULL;
            continue;
        }
        if (ident_char((unsigned char)c) || c == '\\') {
            if (n) return NULL;
            const char *name = read_ident(pc->a, &s, e, true);
            if (!name || (s < e && *s == '|')) return NULL;
            x->kind = SK_TAG;
            x->name = name;
            x->tag = (uint16_t)tag_lookup(name, strlen(name));
            *spec = spec_add(*spec, SPEC_C);
            n++;
            continue;
        }
        if (c == '#' || c == '.') {
            s++;
            const char *name = read_ident(pc->a, &s, e, false);
            if (!name) return NULL;
            x->kind = c == '#' ? SK_ID : SK_CLASS;
            x->name = name;
            *spec = spec_add(*spec, c == '#' ? SPEC_A : SPEC_B);
            n++;
            continue;
        }
        if (c == '[') {
            const char *close = scan_to(s + 1, e, "]");
            if (close >= e) return NULL;
            const char *p = skip_ws(s + 1, close);
            const char *name = read_ident(pc->a, &p, close, true);
            if (!name) return NULL;
            x->kind = SK_ATTR;
            x->name = name;
            p = skip_ws(p, close);
            if (p < close) {
                static const char ops[] = "=~|^$*";
                const char *o = strchr(ops, *p);
                if (!o || !*p) return NULL;
                if (*p == '=') x->op = AO_EQ, p++;
                else {
                    if (p + 1 >= close || p[1] != '=') return NULL;
                    x->op = (uint8_t)(AO_INCL + (o - ops - 1));
                    p += 2;
                }
                p = skip_ws(p, close);
                if (p < close && (*p == '"' || *p == '\'')) {
                    char q = *p++;
                    const char *vs = p;
                    while (p < close && *p != q) p += *p == '\\' ? 2 : 1;
                    sbuf b = {0};
                    css_unescape(&b, vs, (size_t)(p - vs));
                    x->value = ar_strndup(pc->a, b.p ? b.p : "", b.n);
                    sb_free(&b);
                    if (p < close) p++;
                } else {
                    x->value = read_ident(pc->a, &p, close, false);
                    if (!x->value) {
                        /* bare numbers like [colspan=2] */
                        const char *vs = p;
                        while (p < close && !is_space((unsigned char)*p)) p++;
                        if (p == vs) return NULL;
                        x->value = ar_strndup(pc->a, vs, (size_t)(p - vs));
                    }
                }
                p = skip_ws(p, close);
                if (p < close && (*p | 32) == 'i') x->icase = true, p++;
                else if (p < close && (*p | 32) == 's') p++;
                if (skip_ws(p, close) != close) return NULL;
            }
            if (!strcmp(name, "type")) x->icase = true;
            *spec = spec_add(*spec, SPEC_B);
            s = close + 1;
            n++;
            continue;
        }
        if (c == ':') {
            bool elem = s + 1 < e && s[1] == ':';
            s += elem ? 2 : 1;
            const char *name = read_ident(pc->a, &s, e, true);
            if (!name) return NULL;
            const char *args = NULL, *args_end = NULL;
            if (s < e && *s == '(') {
                args = s + 1;
                args_end = scan_to(args, e, ")");
                if (args_end >= e) return NULL;
                s = args_end + 1;
            }
            if (elem || (!args && (!strcmp(name, "before") || !strcmp(name, "after") ||
                                   !strcmp(name, "first-line") || !strcmp(name, "first-letter")))) {
                if (*pseudo) return NULL;
                if (!strcmp(name, "before")) *pseudo = PE_BEFORE;
                else if (!strcmp(name, "after")) *pseudo = PE_AFTER;
                else if (!strncmp(name, "-webkit-", 8) || !strncmp(name, "-moz-", 5) || !strncmp(name, "-ms-", 4))
                    return NULL;
                else *pseudo = PE_OTHER;
                *spec = spec_add(*spec, SPEC_C);
                continue;
            }
            x->kind = SK_PC;
            if (args) {
                if (!strcmp(name, "not") || !strcmp(name, "is") || !strcmp(name, "where") ||
                    !strcmp(name, "matches") || !strcmp(name, "-webkit-any") || !strcmp(name, "-moz-any") ||
                    !strcmp(name, "any")) {
                    bool forgiving = strcmp(name, "not") != 0;
                    x->args = parse_sellist(pc, args, args_end, forgiving);
                    if (!x->args) return NULL;
                    x->pc = !strcmp(name, "not") ? PC_NOT : PC_IS;
                    if (strcmp(name, "where")) *spec = spec_add(*spec, sellist_max_spec(x->args));
                } else if (!strcmp(name, "has")) {
                    x->pc = PC_NEVER;
                } else if (!strncmp(name, "nth-", 4)) {
                    static const char *const nth[] = {"nth-child", "nth-last-child", "nth-of-type", "nth-last-of-type"};
                    static const uint8_t nthpc[] = {PC_NTH_CHILD, PC_NTH_LAST_CHILD, PC_NTH_OF_TYPE, PC_NTH_LAST_OF_TYPE};
                    int k = 0;
                    while (k < 4 && strcmp(name, nth[k])) k++;
                    if (k == 4) return NULL;
                    x->pc = nthpc[k];
                    const char *ae = args_end;
                    for (const char *q = args; q + 3 < ae; q++) /* "An+B of S": the S is ignored */
                        if (is_space((unsigned char)q[0]) && strn_ieq(q + 1, "of", 2) && is_space((unsigned char)q[3])) {
                            ae = q;
                            break;
                        }
                    if (!parse_anb(args, ae, &x->a, &x->b)) return NULL;
                    *spec = spec_add(*spec, SPEC_B);
                } else if (!strcmp(name, "lang")) {
                    const char *p = skip_ws(args, args_end);
                    const char *ve = args_end;
                    trim_r(p, &ve);
                    if (p < ve && (*p == '"' || *p == '\'')) p++, ve--;
                    x->pc = PC_LANG;
                    x->value = ar_strndup(pc->a, p, (size_t)(ve > p ? ve - p : 0));
                    *spec = spec_add(*spec, SPEC_B);
                } else if (!strcmp(name, "dir")) {
                    const char *p = skip_ws(args, args_end);
                    x->pc = strn_ieq(p, "ltr", 3) ? PC_ALWAYS : PC_NEVER;
                    *spec = spec_add(*spec, SPEC_B);
                } else if (!strcmp(name, "host") || !strcmp(name, "host-context") || !strcmp(name, "state") ||
                           !strcmp(name, "active-view-transition-type")) {
                    x->pc = PC_NEVER;
                } else return NULL;
                n++;
                continue;
            }
            size_t k = 0;
            while (k < sizeof simple_pcs / sizeof *simple_pcs && strcmp(simple_pcs[k].name, name)) k++;
            if (k == sizeof simple_pcs / sizeof *simple_pcs) return NULL;
            x->pc = simple_pcs[k].pc;
            *spec = spec_add(*spec, SPEC_B);
            n++;
            continue;
        }
        break;
    }
    if (s == *ps) return NULL;
    *ps = s;
    struct compound *cp = ar_alloc(pc->a, sizeof *cp);
    cp->n = n;
    cp->s = ar_alloc(pc->a, sizeof(struct simple) * (size_t)(n ? n : 1));
    memcpy(cp->s, tmp, sizeof(struct simple) * (size_t)n);
    return cp;
}

static struct selector *parse_complex(struct pctx *pc, const char *s, const char *e) {
    struct selector *sel = ar_alloc(pc->a, sizeof *sel);
    struct compound *cur = NULL;
    char pending = 0;
    s = skip_ws(s, e);
    trim_r(s, &e);
    if (s >= e) return NULL;
    while (s < e) {
        if (is_space((unsigned char)*s)) {
            s = skip_ws(s, e);
            if (cur && !pending) pending = ' ';
            continue;
        }
        if (*s == '>' || *s == '+' || *s == '~') {
            if (!cur || (pending && pending != ' ')) return NULL;
            pending = *s++;
            continue;
        }
        if (sel->pseudo) return NULL;
        struct compound *c = parse_compound(pc, &s, e, &sel->spec, &sel->pseudo);
        if (!c) return NULL;
        c->left = cur;
        c->comb = cur ? (pending ? pending : ' ') : 0;
        cur = c;
        pending = 0;
    }
    if (pending && pending != ' ') return NULL;
    sel->right = cur;
    return sel;
}

static struct sellist *parse_sellist(struct pctx *pc, const char *s, const char *e, bool forgiving) {
    struct selector *tmp[64];
    int n = 0;
    while (s < e) {
        const char *c = scan_to(s, e, ",");
        struct selector *x = parse_complex(pc, s, c);
        if (x) {
            if (n < 64) tmp[n++] = x;
        } else if (!forgiving) return NULL;
        s = c < e ? c + 1 : e;
    }
    if (!n && !forgiving) return NULL;
    struct sellist *l = ar_alloc(pc->a, sizeof *l);
    l->n = n;
    l->v = ar_alloc(pc->a, sizeof(struct selector *) * (size_t)(n ? n : 1));
    memcpy(l->v, tmp, sizeof(struct selector *) * (size_t)n);
    return l;
}

/* ---------------------------------------------------------------- matching */
static bool match_list(const struct sellist *l, node_t *e);

static node_t *prev_elem(node_t *e) {
    for (e = e->prev; e; e = e->prev)
        if (e->type == N_ELEM) return e;
    return NULL;
}

static node_t *next_elem(node_t *e) {
    for (e = e->next; e; e = e->next)
        if (e->type == N_ELEM) return e;
    return NULL;
}

static bool nth(int a, int b, int idx) {
    if (a == 0) return idx == b;
    int d = idx - b;
    return d / a >= 0 && d % a == 0;
}

static bool form_control(const node_t *e) {
    return e->tag == T_input || e->tag == T_button || e->tag == T_select || e->tag == T_textarea ||
           e->tag == T_option || e->tag == T_optgroup || e->tag == T_fieldset;
}

static bool icase_eq(const char *a, const char *b, bool icase) { return icase ? str_ieq(a, b) : !strcmp(a, b); }

static bool match_attr(const struct simple *x, const node_t *e) {
    const char *v = node_attr(e, x->name);
    if (!v) return false;
    if (x->op == AO_EXISTS) return true;
    const char *w = x->value;
    size_t vl = strlen(v), wl = strlen(w);
    bool ic = x->icase;
    switch (x->op) {
    case AO_EQ: return icase_eq(v, w, ic);
    case AO_INCL:
        if (!wl) return false;
        for (const char *p = v; *p;) {
            while (is_space((unsigned char)*p)) p++;
            const char *st = p;
            while (*p && !is_space((unsigned char)*p)) p++;
            if ((size_t)(p - st) == wl && (ic ? strn_ieq(st, w, wl) : !strncmp(st, w, wl))) return true;
        }
        return false;
    case AO_DASH:
        return (ic ? strn_ieq(v, w, wl > vl ? vl : wl) && vl >= wl : !strncmp(v, w, wl)) && (v[wl] == 0 || v[wl] == '-');
    case AO_PREFIX: return wl && vl >= wl && (ic ? !strncasecmp(v, w, wl) : !strncmp(v, w, wl));
    case AO_SUFFIX: return wl && vl >= wl && (ic ? !strncasecmp(v + vl - wl, w, wl) : !strcmp(v + vl - wl, w));
    case AO_SUBSTR:
        if (!wl) return false;
        for (size_t i = 0; i + wl <= vl; i++)
            if (ic ? !strncasecmp(v + i, w, wl) : !strncmp(v + i, w, wl)) return true;
        return false;
    }
    return false;
}

static bool match_pc(const struct simple *x, node_t *e) {
    switch (x->pc) {
    case PC_FIRST_CHILD: return !prev_elem(e);
    case PC_LAST_CHILD: return !next_elem(e);
    case PC_ONLY_CHILD: return !prev_elem(e) && !next_elem(e);
    case PC_NTH_CHILD: return nth(x->a, x->b, e->elem_index);
    case PC_NTH_LAST_CHILD: {
        int k = 1;
        for (node_t *s = next_elem(e); s; s = next_elem(s)) k++;
        return nth(x->a, x->b, k);
    }
    case PC_FIRST_OF_TYPE: case PC_LAST_OF_TYPE: case PC_ONLY_OF_TYPE: case PC_NTH_OF_TYPE:
    case PC_NTH_LAST_OF_TYPE: {
        int before = 1, after = 1;
        for (node_t *s = prev_elem(e); s; s = prev_elem(s))
            if (!strcmp(s->name, e->name)) before++;
        for (node_t *s = next_elem(e); s; s = next_elem(s))
            if (!strcmp(s->name, e->name)) after++;
        switch (x->pc) {
        case PC_FIRST_OF_TYPE: return before == 1;
        case PC_LAST_OF_TYPE: return after == 1;
        case PC_ONLY_OF_TYPE: return before == 1 && after == 1;
        case PC_NTH_OF_TYPE: return nth(x->a, x->b, before);
        default: return nth(x->a, x->b, after);
        }
    }
    case PC_NOT: return !match_list(x->args, e);
    case PC_IS: return match_list(x->args, e);
    case PC_NEVER: return false;
    case PC_ALWAYS: return true;
    case PC_LINK: return (e->tag == T_a || e->tag == T_area) && node_attr(e, "href");
    case PC_CHECKED:
        return (e->tag == T_input && e->checked) || doc_option_selected(e);
    case PC_DISABLED: return form_control(e) && node_attr(e, "disabled");
    case PC_ENABLED: return form_control(e) && !node_attr(e, "disabled");
    case PC_ROOT: return e->parent && e->parent->type == N_DOC;
    case PC_EMPTY:
        for (node_t *c = e->first; c; c = c->next)
            if (c->type == N_ELEM || (c->type == N_TEXT && c->textlen)) return false;
        return true;
    case PC_REQUIRED: return form_control(e) && node_attr(e, "required");
    case PC_OPTIONAL: return form_control(e) && !node_attr(e, "required");
    case PC_LANG:
        for (node_t *p = e; p && p->type == N_ELEM; p = p->parent) {
            const char *l = node_attr(p, "lang");
            if (l) {
                size_t k = strlen(x->value);
                return !strncasecmp(l, x->value, k) && (l[k] == 0 || l[k] == '-');
            }
        }
        return false;
    case PC_PLACEHOLDER_SHOWN:
        return (e->tag == T_input || e->tag == T_textarea) && node_attr(e, "placeholder") && (!e->value || !*e->value);
    case PC_READ_WRITE:
        return (e->tag == T_input || e->tag == T_textarea) && !node_attr(e, "readonly") && !node_attr(e, "disabled");
    case PC_READ_ONLY:
        return !((e->tag == T_input || e->tag == T_textarea) && !node_attr(e, "readonly") && !node_attr(e, "disabled"));
    case PC_OPEN: return (e->tag == T_details || e->tag == T_dialog) && node_attr(e, "open");
    }
    return false;
}

static bool match_compound(const struct compound *c, node_t *e) {
    for (int i = 0; i < c->n; i++) {
        const struct simple *x = &c->s[i];
        switch (x->kind) {
        case SK_TAG:
            if (x->tag ? e->tag != x->tag : strcmp(e->name, x->name)) return false;
            break;
        case SK_ID:
            if (!e->id || strcmp(e->id, x->name)) return false;
            break;
        case SK_CLASS:
            if (!node_has_class(e, x->name)) return false;
            break;
        case SK_ATTR:
            if (!match_attr(x, e)) return false;
            break;
        case SK_PC:
            if (!match_pc(x, e)) return false;
            break;
        }
    }
    return true;
}

static bool match_from(const struct compound *c, node_t *e) {
    if (!match_compound(c, e)) return false;
    if (!c->left) return true;
    switch (c->comb) {
    case '>': return e->parent && e->parent->type == N_ELEM && match_from(c->left, e->parent);
    case ' ':
        for (node_t *p = e->parent; p && p->type == N_ELEM; p = p->parent)
            if (match_from(c->left, p)) return true;
        return false;
    case '+': {
        node_t *p = prev_elem(e);
        return p && match_from(c->left, p);
    }
    case '~':
        for (node_t *p = prev_elem(e); p; p = prev_elem(p))
            if (match_from(c->left, p)) return true;
        return false;
    }
    return false;
}

static bool match_list(const struct sellist *l, node_t *e) {
    for (int i = 0; l && i < l->n; i++)
        if (!l->v[i]->pseudo && match_from(l->v[i]->right, e)) return true;
    return false;
}

/* DOM queries use exactly the cascade's parser and matcher, with a short-lived
   bounded arena so repeated querySelector calls do not grow the document arena. */
static void select_walk(node_t *scope, const struct sellist *sel, pvec *out) {
    for (node_t *n = scope->first; n; n = n->next) {
        if (n->type == N_ELEM && match_list(sel, n)) pv_push(out, n);
        select_walk(n, sel, out);
    }
}

bool css_select(web_doc *d, node_t *scope, const char *selector, pvec *out) {
    (void)d;
    if (!selector || !scope || strlen(selector) > 65536) return false;
    size_t len = strlen(selector);
    while (len && is_space((unsigned char)selector[len - 1])) len--;
    if (!len || selector[len - 1] == ',') return false;
    arena_t *arena = calloc(1, sizeof *arena);
    if (!arena) return false;
    arena->limit = 1u << 20;
    jmp_buf trap;
    arena->trap = &trap;
    if (setjmp(trap)) { ar_free(arena); free(arena); return false; }
    struct pctx p = {.a = arena};
    struct sellist *sel = parse_sellist(&p, selector, selector + len, false);
    if (sel) select_walk(scope, sel, out);
    bool valid = sel != NULL;
    ar_free(arena);
    free(arena);
    return valid;
}

bool css_matches(node_t *node, const char *selector, bool *valid) {
    if (valid) *valid = false;
    if (!node || node->type != N_ELEM || !selector || strlen(selector) > 65536) return false;
    size_t len = strlen(selector);
    while (len && is_space((unsigned char)selector[len - 1])) len--;
    if (!len || selector[len - 1] == ',') return false;
    arena_t *arena = calloc(1, sizeof *arena);
    if (!arena) return false;
    arena->limit = 1u << 20;
    jmp_buf trap;
    arena->trap = &trap;
    if (setjmp(trap)) { ar_free(arena); free(arena); return false; }
    struct pctx p = {.a = arena};
    struct sellist *sel = parse_sellist(&p, selector, selector + len, false);
    if (valid) *valid = sel != NULL;
    bool matched = sel && match_list(sel, node);
    ar_free(arena);
    free(arena);
    return matched;
}

/* ---------------------------------------------------------------- parsing stylesheets */
/* a declaration value with its url()s made absolute against the stylesheet's URL */
static char *absolute_urls(struct pctx *pc, const char *v, const char *ve) {
    const char *u = NULL;
    for (const char *p = v; p + 4 <= ve; p++)
        if (strn_ieq(p, "url(", 4)) {
            u = p;
            break;
        }
    if (!u || !pc->base || !strncmp(pc->base, "about:", 6)) return ar_strndup(pc->a, v, (size_t)(ve - v));
    sbuf b = {0};
    const char *p = v;
    while (p < ve) {
        if (p + 4 <= ve && strn_ieq(p, "url(", 4)) {
            const char *s = p + 4, *e = s;
            while (e < ve && *e != ')') e++;
            if (e >= ve) break;
            const char *a = s, *z = e;
            while (a < z && is_space((unsigned char)*a)) a++;
            while (z > a && is_space((unsigned char)z[-1])) z--;
            if (z - a >= 2 && (*a == '"' || *a == '\'') && z[-1] == *a) a++, z--;
            char rel[2048], abs[2048];
            size_t rn = (size_t)(z - a) < sizeof rel - 1 ? (size_t)(z - a) : sizeof rel - 1;
            memcpy(rel, a, rn);
            rel[rn] = 0;
            if (rn && !strchr(rel, '"') && url_resolve(pc->base, rel, abs, sizeof abs)) {
                sb_puts(&b, "url(\"");
                sb_puts(&b, abs);
                sb_puts(&b, "\")");
            } else sb_put(&b, p, (size_t)(e + 1 - p));
            p = e + 1;
            continue;
        }
        sb_putc(&b, *p++);
    }
    if (p < ve) sb_put(&b, p, (size_t)(ve - p));
    char *r = ar_strndup(pc->a, b.p ? b.p : "", b.n);
    sb_free(&b);
    return r;
}

static struct decl *parse_decl(struct pctx *pc, const char *s, const char *e, struct decl *d) {
    s = skip_ws(s, e);
    trim_r(s, &e);
    const char *colon = scan_to(s, e, ":");
    if (colon >= e) return NULL;
    const char *ne = colon;
    trim_r(s, &ne);
    if (ne <= s) return NULL;
    const char *v = skip_ws(colon + 1, e);
    const char *ve = e;
    memset(d, 0, sizeof *d);
    /* !important */
    const char *bang = ve;
    while (bang > v && bang[-1] != '!') bang--;
    if (bang > v) {
        const char *imp = skip_ws(bang, ve);
        if ((size_t)(ve - imp) == 9 && strn_ieq(imp, "important", 9)) {
            d->important = true;
            ve = bang - 1;
            trim_r(v, &ve);
        }
    }
    if (ne - s > 2 && s[0] == '-' && s[1] == '-') {
        d->name = ar_strndup(pc->a, s, (size_t)(ne - s));
        d->value = ar_strndup(pc->a, v, (size_t)(ve - v));
        return d;
    }
    d->p = css_prop_lookup(s, (size_t)(ne - s));
    if (!d->p || ve <= v) return NULL;
    d->value = absolute_urls(pc, v, ve);
    return d;
}

static void parse_rules(struct pctx *pc, const char *s, const char *e, const struct mcond *media);
static void parse_style_rule(struct pctx *pc, const char *ps, const char *pe, const char *bs, const char *be,
                             const struct mcond *media, const char *parent);

/* a nested rule's selector, relative to its parent's: & is the parent */
static char *nest_selector(struct pctx *pc, const char *ps, const char *pe, const char *parent) {
    sbuf b = {0};
    const char *s = ps;
    while (s < pe) {
        const char *c = scan_to(s, pe, ",");
        const char *ts = skip_ws(s, c), *te = c;
        trim_r(ts, &te);
        if (b.n) sb_puts(&b, ", ");
        bool amp = false;
        for (const char *q = ts; q < te; q++)
            if (*q == '&') amp = true;
        if (!amp) {
            sb_puts(&b, ":is(");
            sb_puts(&b, parent);
            sb_puts(&b, ") ");
        }
        for (const char *q = ts; q < te; q++) {
            if (*q == '&') {
                sb_puts(&b, ":is(");
                sb_puts(&b, parent);
                sb_puts(&b, ")");
            } else sb_putc(&b, *q);
        }
        s = c < pe ? c + 1 : pe;
    }
    char *r = ar_strndup(pc->a, b.p ? b.p : "", b.n);
    sb_free(&b);
    return r;
}

/* the declarations and nested rules of a block */
static void parse_body(struct pctx *pc, const char *s, const char *e, const char *sel, const struct mcond *media,
                       struct decl **out, int *nout) {
    sbuf d = {0}; /* struct decl array */
    while (s < e) {
        while (s < e && (is_space((unsigned char)*s) || *s == ';')) s++;
        if (s >= e) break;
        if (*s == '@') {
            const char *p = scan_to(s, e, "{;}");
            if (p >= e || *p != '{') {
                s = p + 1;
                continue;
            }
            const char *be = block_end(p + 1, e);
            if (sel) {
                const char *ns = s + 1;
                const char *name = read_ident(pc->a, &ns, p, true);
                const char *qs = skip_ws(ns, p), *qe = p;
                trim_r(qs, &qe);
                if (name && (!strcmp(name, "media") || !strcmp(name, "supports") || !strcmp(name, "container") ||
                             !strcmp(name, "layer") || !strcmp(name, "scope"))) {
                    const struct mcond *m = media;
                    if (!strcmp(name, "media")) {
                        struct mcond *mc = ar_alloc(pc->a, sizeof *mc);
                        mc->q = ar_strndup(pc->a, qs, (size_t)(qe - qs));
                        mc->up = media;
                        m = mc;
                    }
                    if (strcmp(name, "supports") || !strn_ieq(qs, "not", 3)) {
                        static const char amp[] = "&";
                        parse_style_rule(pc, amp, amp + 1, p + 1, be, m, sel);
                    }
                }
            }
            s = be < e ? be + 1 : e;
            continue;
        }
        const char *p = scan_to(s, e, ";{}");
        if (p < e && *p == '{') {
            const char *be = block_end(p + 1, e);
            if (sel) parse_style_rule(pc, s, p, p + 1, be, media, sel);
            s = be < e ? be + 1 : e;
            continue;
        }
        struct decl x;
        if (parse_decl(pc, s, p, &x)) sb_put(&d, (const char *)&x, sizeof x);
        s = p < e ? p + 1 : e;
    }
    *nout = (int)(d.n / sizeof(struct decl));
    *out = NULL;
    if (*nout) {
        *out = ar_alloc(pc->a, d.n);
        memcpy(*out, d.p, d.n);
    }
    sb_free(&d);
}

static void parse_style_rule(struct pctx *pc, const char *ps, const char *pe, const char *bs, const char *be,
                             const struct mcond *media, const char *parent) {
    const char *sel_text;
    ps = skip_ws(ps, pe);
    trim_r(ps, &pe);
    if (parent) sel_text = nest_selector(pc, ps, pe, parent);
    else sel_text = ar_strndup(pc->a, ps, (size_t)(pe - ps));
    struct sellist *sl = parse_sellist(pc, sel_text, sel_text + strlen(sel_text), false);
    if (!sl) return;
    struct rule *r = ar_alloc(pc->a, sizeof *r);
    r->sel = sl;
    r->media = media;
    /* append before parsing the body, so nested rules come after their parent */
    if (pc->sh->last) pc->sh->last->next = r;
    else pc->sh->first = r;
    pc->sh->last = r;
    parse_body(pc, bs, be, sel_text, media, &r->decls, &r->ndecls);
}

static void parse_rules(struct pctx *pc, const char *s, const char *e, const struct mcond *media) {
    while (s < e) {
        s = skip_ws(s, e);
        if (s >= e) break;
        if (*s == '@') {
            const char *ns = s + 1;
            const char *name = read_ident(pc->a, &ns, e, true);
            const char *p = scan_to(ns, e, "{;");
            const char *qs = skip_ws(ns, p), *qe = p;
            trim_r(qs, &qe);
            if (p >= e || *p == ';') {
                if (name && !strcmp(name, "import") && pc->imports) {
                    /* @import url [media]: fetched later, ordered just before this sheet */
                    const char *ue = scan_to(qs, qe, " \t\n");
                    const char *t = qs;
                    size_t tl = (size_t)(ue - qs);
                    sbuf u = {0};
                    if (tl > 4 && strn_ieq(t, "url(", 4)) {
                        const char *a = t + 4, *b = t + tl - 1;
                        a = skip_ws(a, b);
                        trim_r(a, &b);
                        if (b > a && (*a == '"' || *a == '\'')) a++, b--;
                        css_unescape(&u, a, (size_t)(b - a));
                    } else if (tl >= 2 && (t[0] == '"' || t[0] == '\'')) css_unescape(&u, t + 1, tl - 2);
                    const char *rest = skip_ws(ue, qe);
                    bool ok = u.n > 0;
                    if (rest < qe) { /* a media list or layer()/supports() */
                        char buf[256];
                        size_t bl = (size_t)(qe - rest) < sizeof buf - 1 ? (size_t)(qe - rest) : sizeof buf - 1;
                        memcpy(buf, rest, bl);
                        buf[bl] = 0;
                        if (strstr(buf, "print") && !strstr(buf, "screen")) ok = false;
                    }
                    char abs[2048];
                    if (ok && url_resolve(pc->base, sb_cstr(&u), abs, sizeof abs)) {
                        struct css_import *im = malloc(sizeof *im);
                        im->url = strdup(abs);
                        im->order = pc->sh->order - 0.1 + 0.001 * pc->nimports++;
                        pv_push(pc->imports, im);
                    }
                    sb_free(&u);
                }
                s = p + 1;
                continue;
            }
            const char *be = block_end(p + 1, e);
            if (name) {
                if (!strcmp(name, "media")) {
                    struct mcond *mc = ar_alloc(pc->a, sizeof *mc);
                    mc->q = ar_strndup(pc->a, qs, (size_t)(qe - qs));
                    mc->up = media;
                    parse_rules(pc, p + 1, be, mc);
                } else if (!strcmp(name, "supports")) {
                    if (!strn_ieq(qs, "not", 3)) parse_rules(pc, p + 1, be, media);
                } else if (!strcmp(name, "layer") || !strcmp(name, "container") || !strcmp(name, "scope") ||
                           !strcmp(name, "document") || !strcmp(name, "-moz-document")) {
                    parse_rules(pc, p + 1, be, media);
                }
                /* @font-face, @keyframes, @page, @property, @starting-style ... are skipped */
            }
            s = be < e ? be + 1 : e;
            continue;
        }
        const char *p = scan_to(s, e, "{;}");
        if (p >= e) break;
        if (*p != '{') { /* garbage: skip it */
            s = p + 1;
            continue;
        }
        const char *be = block_end(p + 1, e);
        parse_style_rule(pc, s, p, p + 1, be, media, NULL);
        s = be < e ? be + 1 : e;
    }
}

/* comments out, <!-- --> out */
static char *strip_comments(arena_t *a, const char *css, size_t n, size_t *out_n) {
    char *o = ar_alloc(a, n + 1);
    size_t k = 0;
    for (size_t i = 0; i < n;) {
        char c = css[i];
        if (c == '/' && i + 1 < n && css[i + 1] == '*') {
            i += 2;
            while (i + 1 < n && !(css[i] == '*' && css[i + 1] == '/')) i++;
            i += 2;
            o[k++] = ' ';
            continue;
        }
        if (c == '"' || c == '\'') {
            o[k++] = css[i++];
            while (i < n && css[i] != c && css[i] != '\n') {
                if (css[i] == '\\' && i + 1 < n) o[k++] = css[i++];
                o[k++] = css[i++];
            }
            if (i < n) o[k++] = css[i++];
            continue;
        }
        if (c == '<' && i + 3 < n && !memcmp(css + i, "<!--", 4)) {
            i += 4;
            o[k++] = ' ';
            continue;
        }
        if (c == '-' && i + 2 < n && !memcmp(css + i, "-->", 3)) {
            i += 3;
            o[k++] = ' ';
            continue;
        }
        o[k++] = c ? c : ' ';
        i++;
    }
    o[k] = 0;
    *out_n = k;
    return o;
}

sheet_t *css_parse_sheet(arena_t *a, const char *css, size_t n, const char *base_url, double order, pvec *imports) {
    struct pctx pc = {a, NULL, base_url, imports, 0};
    pc.sh = ar_alloc(a, sizeof(sheet_t));
    pc.sh->order = order;
    size_t cn;
    char *c = strip_comments(a, css, n, &cn);
    parse_rules(&pc, c, c + cn, NULL);
    return pc.sh;
}

/* ---------------------------------------------------------------- media queries */
/* CSS and matchMedia share this bounded parser and device model. Unknown
   features use Kleene logic: negating an unsupported feature is not a match. */
enum { MQ_UNKNOWN = -1, MQ_FALSE, MQ_TRUE };
enum { MQ_LENGTH = 1, MQ_RATIO, MQ_RESOLUTION, MQ_INTEGER, MQ_NUMBER };
struct mq { const char *s, *e; int vw, vh, depth; bool scripting, valid; };
static int mq_cond(struct mq *m, bool allow_or);
static int mq_not(int a) { return a < 0 ? a : !a; }
static int mq_and(int a, int b) { return !a || !b ? 0 : a < 0 || b < 0 ? -1 : 1; }
static int mq_or(int a, int b) { return a == 1 || b == 1 ? 1 : a < 0 || b < 0 ? -1 : 0; }
static bool mq_word(struct mq *m, const char *w) {
    m->s = skip_ws(m->s, m->e); size_t n = strlen(w);
    if ((size_t)(m->e-m->s) >= n && strn_ieq(m->s,w,n) &&
        (m->s+n == m->e || (!ident_char((unsigned char)m->s[n]) && m->s[n]!='('))) { m->s += n; return true; }
    return false;
}
static bool mq_ident(const char *s, const char *e) {
    if (s == e || !((*s >= 'a' && *s <= 'z') || *s == '_' || *s == '-')) return false;
    for (; s < e; s++) if (!ident_char((unsigned char)*s)) return false;
    return true;
}
/* Unitless nonzero lengths, mixed dimensions, nonfinite values and unconsumed
   suffixes are rejected; strtod is never used as a permissive prefix parser. */
static bool mq_number(const char *s, const char *e, const char **end, double *out) {
    const char *p=s; if (p<e && (*p=='+' || *p=='-')) p++;
    const char *digits=p; while(p<e && *p>='0' && *p<='9') p++;
    bool any=p>digits;
    if(p<e && *p=='.'){ p++; digits=p; while(p<e && *p>='0' && *p<='9')p++; if(p==digits)return false; any=true; }
    if(!any)return false;
    if(p<e && (*p=='e'||*p=='E')) {
        const char *exp=p+1; if(exp<e && (*exp=='+'||*exp=='-'))exp++;
        if(exp<e && *exp>='0'&&*exp<='9'){p=exp+1;while(p<e&&*p>='0'&&*p<='9')p++;}
    }
    size_t n=(size_t)(p-s); if(n>=64)return false; char b[64]; memcpy(b,s,n);b[n]=0;
    double v=strtod(b,NULL); if(!(v>=-1e30 && v<=1e30))return false;
    *out=v;*end=p;return true;
}
static bool mq_value(const char *s,const char *e,int kind,const struct mq *m,double *out) {
    s=skip_ws(s,e);trim_r(s,&e);const char *p;double v;
    if(!mq_number(s,e,&p,&v))return false;
    if(kind==MQ_RATIO){
        p=skip_ws(p,e);if(p<e&&*p=='/'){double den;p=skip_ws(p+1,e);if(!mq_number(p,e,&p,&den)||den<=0)return false;v/=den;}
        if(skip_ws(p,e)!=e||v<0)return false;*out=v;return true;
    }
    size_t n=(size_t)(e-p);double scale=1;
    if(kind==MQ_INTEGER||kind==MQ_NUMBER){
        if(n || (kind==MQ_INTEGER && (v<0 || v>2147483647 || v!=(int)v)))return false;
    }else if(kind==MQ_RESOLUTION){
        if(strn_ieq(p,"dppx",n)||strn_ieq(p,"x",n))scale=1;
        else if(strn_ieq(p,"dpi",n))scale=1.0/96;
        else if(strn_ieq(p,"dpcm",n))scale=2.54/96;
        else return false;if(v<=0)return false;
    }else {
        if(!n){if(v!=0)return false;}
        else if(strn_ieq(p,"px",n))scale=1;
        else if(strn_ieq(p,"em",n)||strn_ieq(p,"rem",n))scale=16;
        else if(strn_ieq(p,"pt",n))scale=96.0/72;
        else if(strn_ieq(p,"pc",n))scale=16;
        else if(strn_ieq(p,"in",n))scale=96;
        else if(strn_ieq(p,"cm",n))scale=96/2.54;
        else if(strn_ieq(p,"mm",n))scale=96/25.4;
        else if(strn_ieq(p,"q",n))scale=96/101.6;
        else if(strn_ieq(p,"vw",n))scale=m->vw/100.0;
        else if(strn_ieq(p,"vh",n))scale=m->vh/100.0;
        else if(strn_ieq(p,"vmin",n))scale=(m->vw<m->vh?m->vw:m->vh)/100.0;
        else if(strn_ieq(p,"vmax",n))scale=(m->vw>m->vh?m->vw:m->vh)/100.0;
        else return false;
    }
    *out=v*scale;return true;
}
static int mq_numeric(const char *s,size_t n,const struct mq *m,double *v) {
    /* Screen dimensions are not supplied by the embedder. Deprecated device-*
       features stay unknown rather than falsely equating screen and window. */
    if(strn_ieq(s,"width",n)){*v=m->vw;return MQ_LENGTH;}
    if(strn_ieq(s,"height",n)){*v=m->vh;return MQ_LENGTH;}
    if(strn_ieq(s,"aspect-ratio",n)){*v=m->vh?(double)m->vw/m->vh:0;return MQ_RATIO;}
    if(strn_ieq(s,"resolution",n)){*v=1;return MQ_RESOLUTION;}
    if(strn_ieq(s,"-webkit-device-pixel-ratio",n)||strn_ieq(s,"device-pixel-ratio",n)){*v=1;return MQ_NUMBER;}
    if(strn_ieq(s,"color",n)){*v=8;return MQ_INTEGER;}
    if(strn_ieq(s,"monochrome",n)||strn_ieq(s,"color-index",n)){*v=0;return MQ_INTEGER;}
    return 0;
}
static const char *mq_discrete(const char *s,size_t n,const struct mq *m,const char **allowed) {
#define FEATURE(name,value,choices) if(strn_ieq(s,name,n)){*allowed=choices;return value;}
    FEATURE("orientation",m->vw>m->vh?"landscape":"portrait","portrait landscape")
    FEATURE("prefers-color-scheme","light","light dark")
    FEATURE("prefers-reduced-motion","no-preference","no-preference reduce")
    FEATURE("prefers-reduced-transparency","no-preference","no-preference reduce")
    FEATURE("prefers-reduced-data","no-preference","no-preference reduce")
    FEATURE("prefers-contrast","no-preference","no-preference more less custom")
    FEATURE("hover","hover","hover none") FEATURE("any-hover","hover","hover none")
    FEATURE("pointer","fine","none coarse fine") FEATURE("any-pointer","fine","none coarse fine")
    FEATURE("scripting",m->scripting?"enabled":"none","none initial-only enabled")
    FEATURE("forced-colors","none","none active") FEATURE("inverted-colors","none","none inverted")
    FEATURE("display-mode","browser","browser fullscreen standalone minimal-ui picture-in-picture window-controls-overlay")
    FEATURE("update","fast","none slow fast") FEATURE("color-gamut","srgb","srgb p3 rec2020")
    FEATURE("dynamic-range","standard","standard high")
    FEATURE("overflow-block","scroll","none scroll paged") FEATURE("overflow-inline","scroll","none scroll")
    FEATURE("grid","0","0 1")
#undef FEATURE
    return NULL;
}
static bool mq_choice(const char *s,size_t n,const char *choices) {
    while(*choices){const char *e=strchr(choices,' ');if(!e)e=choices+strlen(choices);
        if((size_t)(e-choices)==n&&!memcmp(s,choices,n))return true;choices=*e?e+1:e;}
    return false;
}
static bool mq_compare(double a,const char *op,double b) {
    if(!strcmp(op,"<"))return a<b;if(!strcmp(op,"<="))return a<=b;
    if(!strcmp(op,">"))return a>b;if(!strcmp(op,">="))return a>=b;return a==b;
}
static int mq_feature(const char *s,const char *e,struct mq *m) {
    s=skip_ws(s,e);trim_r(s,&e);if(s==e)return MQ_UNKNOWN;
    const char *colon=memchr(s,':',(size_t)(e-s));
    if(colon){
        const char *ne=colon;trim_r(s,&ne);const char *v=skip_ws(colon+1,e);
        if(!mq_ident(s,ne)||v==e)return MQ_UNKNOWN;
        size_t n=(size_t)(ne-s);int mode=0;
        if(n>4&&!memcmp(s,"min-",4)){mode=1;s+=4;n-=4;}
        else if(n>4&&!memcmp(s,"max-",4)){mode=2;s+=4;n-=4;}
        else if(n>12&&!memcmp(s,"-webkit-min-",12)){mode=1;s+=12;n-=12;}
        else if(n>12&&!memcmp(s,"-webkit-max-",12)){mode=2;s+=12;n-=12;}
        double have,want;int kind=mq_numeric(s,n,m,&have);
        if(kind){if(!mq_value(v,e,kind,m,&want))return MQ_UNKNOWN;return mode==1?have>=want:mode==2?have<=want:have==want;}
        if(mode)return MQ_UNKNOWN;
        const char *allowed,*value=mq_discrete(s,n,m,&allowed);
        if(!value||!mq_choice(v,(size_t)(e-v),allowed))return MQ_UNKNOWN;
        return strn_ieq(v,value,(size_t)(e-v));
    }
    const char *op=s;while(op<e&&!strchr("<>=",*op))op++;
    if(op<e){
        const char *tokens[3],*ends[3];char ops[2][3];int count=0;const char *p=s;
        while(count<3){
            tokens[count]=skip_ws(p,e);while(p<e&&!strchr("<>=",*p))p++;ends[count]=p;trim_r(tokens[count],&ends[count]);count++;
            if(p==e)break;if(count==3)return MQ_UNKNOWN;
            char *o=ops[count-1];*o++=*p++;if(p<e&&*p=='=')*o++=*p++;*o=0;
            if(!strcmp(ops[count-1],"==")||(p<e&&strchr("<>=",*p)))return MQ_UNKNOWN;
        }
        double a,b,c;int kind;
        if(count==2){
            if((kind=mq_numeric(tokens[0],(size_t)(ends[0]-tokens[0]),m,&a))&&mq_value(tokens[1],ends[1],kind,m,&b))return mq_compare(a,ops[0],b);
            if((kind=mq_numeric(tokens[1],(size_t)(ends[1]-tokens[1]),m,&b))&&mq_value(tokens[0],ends[0],kind,m,&a))return mq_compare(a,ops[0],b);
        }else if(count==3 && ops[0][0]!='=' && ops[0][0]==ops[1][0] &&
                 (kind=mq_numeric(tokens[1],(size_t)(ends[1]-tokens[1]),m,&b)) &&
                 mq_value(tokens[0],ends[0],kind,m,&a)&&mq_value(tokens[2],ends[2],kind,m,&c))
            return mq_compare(a,ops[0],b)&&mq_compare(b,ops[1],c);
        return MQ_UNKNOWN;
    }
    if(!mq_ident(s,e))return MQ_UNKNOWN;
    double v;if(mq_numeric(s,(size_t)(e-s),m,&v))return v!=0;
    const char *allowed,*value=mq_discrete(s,(size_t)(e-s),m,&allowed);
    return value ? strcmp(value,"none")&&strcmp(value,"no-preference")&&strcmp(value,"0") : MQ_UNKNOWN;
}
static int mq_term(struct mq *m) {
    m->s=skip_ws(m->s,m->e);if(m->depth>=64||m->s==m->e){m->valid=false;return MQ_UNKNOWN;}
    /* General-enclosed functions are unknown, never guessed true. */
    bool function=*m->s!='(';const char *open=m->s;
    if(function){while(open<m->e&&ident_char((unsigned char)*open))open++;}
    if(open==m->e||*open!='('){m->valid=false;return MQ_UNKNOWN;}
    int level=1;const char *close=open+1;
    for(;close<m->e;close++){if(*close=='(')level++;else if(*close==')'&&!--level)break;}
    if(close==m->e){m->valid=false;return MQ_UNKNOWN;}
    m->s=close+1;if(function)return MQ_UNKNOWN;
    const char *in=skip_ws(open+1,close);struct mq sub={in,close,m->vw,m->vh,m->depth+1,m->scripting,true};
    int r;
    if(in<close&&(*in=='('||((size_t)(close-in)>=3&&strn_ieq(in,"not",3)&&
       (in+3==close||!ident_char((unsigned char)in[3]))))) {
        r=mq_cond(&sub,true);if(!sub.valid||skip_ws(sub.s,sub.e)!=sub.e)m->valid=false;
    }else r=mq_feature(in,close,m);
    return r;
}
static int mq_cond(struct mq *m,bool allow_or) {
    if(mq_word(m,"not"))return mq_not(mq_term(m));
    int r=mq_term(m),mode=0;
    for(;;){
        int next=mq_word(m,"and")?1:mq_word(m,"or")?2:0;if(!next)break;
        if((mode&&mode!=next)||(next==2&&!allow_or)){m->valid=false;return MQ_UNKNOWN;}mode=next;
        int rhs=mq_term(m);r=mode==1?mq_and(r,rhs):mq_or(r,rhs);
    }return r;
}
static int mq_query(struct mq *m) {
    m->s=skip_ws(m->s,m->e);const char *start=m->s;
    if(start==m->e){m->valid=false;return MQ_UNKNOWN;}
    bool neg=mq_word(m,"not"),only=!neg&&mq_word(m,"only");m->s=skip_ws(m->s,m->e);
    const char *look=m->s;while(look<m->e&&ident_char((unsigned char)*look))look++;
    if(m->s<m->e&&(*m->s=='('||(look<m->e&&*look=='('))){m->s=start;return mq_cond(m,true);}
    const char *type=m->s;while(m->s<m->e&&ident_char((unsigned char)*m->s))m->s++;
    size_t n=(size_t)(m->s-type);
    if(!mq_ident(type,m->s)||strn_ieq(type,"not",n)||strn_ieq(type,"only",n)||strn_ieq(type,"and",n)||strn_ieq(type,"or",n)){
        m->valid=false;return MQ_UNKNOWN;
    }
    (void)only;int r=strn_ieq(type,"screen",n)||strn_ieq(type,"all",n);
    if(mq_word(m,"and"))r=mq_and(r,mq_cond(m,false));
    return neg?mq_not(r):r;
}
static bool mq_append(char *out,size_t cap,size_t *used,const char *s,size_t n) {
    if(!out)return true;if(n>=cap-*used)return false;memcpy(out+*used,s,n);*used+=n;out[*used]=0;return true;
}
/* Each empty comma-delimited query can expand to "not all, ". */
bool css_media_evaluate(const char *query,int vw,int vh,bool scripting,char *out,size_t cap) {
    if(out&&cap)out[0]=0;if(!query||(out&&!cap))return false;
    size_t length=strlen(query);if(length>16384)return false;
    char *text=malloc(length+1);if(!text)return false;size_t n=0;
    for(size_t i=0;i<length;i++){
        if(query[i]=='/'&&i+1<length&&query[i+1]=='*'){
            i+=2;while(i+1<length&&!(query[i]=='*'&&query[i+1]=='/'))i++;if(i+1>=length)break;i++;text[n++]=' ';
        }else text[n++]=(char)lower((unsigned char)query[i]);
    }text[n]=0;
    const char *s=text,*e=text+n;size_t used=0;bool any=false,first=true,ok=true;
    if(skip_ws(s,e)==e){free(text);return true;}
    for(;;){
        const char *end=s;int depth=0;bool bad=false;
        while(end<e){char c=*end;if(c=='(')depth++;else if(c==')'){if(!depth)bad=true;else depth--;}
            if(!depth&&c==',')break;if((unsigned char)c<32&&!is_space(c))bad=true;
            if(c=='\\'||c=='\''||c=='"'||c=='{'||c=='}'||c==';'||c=='['||c==']')bad=true;end++;}
        struct mq m={s,end,vw,vh,0,scripting,!bad&&!depth};int r=mq_query(&m);
        bool valid=m.valid&&skip_ws(m.s,m.e)==m.e;any|=valid&&r==MQ_TRUE;
        if(!first)ok=ok&&mq_append(out,cap,&used,", ",2);first=false;
        if(!valid||r==MQ_UNKNOWN)ok=ok&&mq_append(out,cap,&used,"not all",7);
        else {
            const char *p=skip_ws(s,end),*z=end;trim_r(p,&z);bool space=false;
            for(;p<z;p++){
                if(is_space((unsigned char)*p)){space=true;continue;}
                char c=*p,prev=out&&used?out[used-1]:0;
                bool before=c=='<'||c=='>'||c=='='||c=='/'||(space&&c!=')'&&c!=':'&&prev!='(');
                if(out&&used&&prev!=' '&&before&&!(c=='='&&(prev=='<'||prev=='>')))ok=ok&&mq_append(out,cap,&used," ",1);
                ok=ok&&mq_append(out,cap,&used,&c,1);space=false;
                if(c==':'||c=='/'||c=='='||((c=='<'||c=='>')&&(p+1==z||p[1]!='=')))ok=ok&&mq_append(out,cap,&used," ",1);
            }
        }
        if(end==e)break;s=end+1;
    }
    free(text);if(!ok&&out&&cap)out[0]=0;return ok&&any;
}
static bool media_chain_ok(const struct mcond *m,int vw,int vh,bool scripting) {
    for(;m;m=m->up)if(!css_media_evaluate(m->q,vw,vh,scripting,NULL,0))return false;return true;
}

/* ---------------------------------------------------------------- the rule index */
struct ient {
    const struct selector *sel;
    const struct rule *r;
    uint32_t order;
    bool ua;
    struct ient *next;
};

struct bucket {
    const char *key;
    struct ient *list;
    struct bucket *next;
};

struct htab {
    struct bucket **v;
    uint32_t cap;
};

struct css_ctx {
    arena_t a;
    struct htab ids, classes, tags;
    struct ient *univ;
};

static uint32_t hash_str(const char *s) {
    uint32_t h = 2166136261u;
    for (; *s; s++) h = (h ^ (unsigned char)*s) * 16777619u;
    return h;
}

static void ht_init(arena_t *a, struct htab *t, uint32_t cap) {
    t->cap = cap;
    t->v = ar_alloc(a, sizeof(struct bucket *) * cap);
}

static struct ient **ht_slot(arena_t *a, struct htab *t, const char *key, bool create) {
    uint32_t h = hash_str(key) & (t->cap - 1);
    for (struct bucket *b = t->v[h]; b; b = b->next)
        if (!strcmp(b->key, key)) return &b->list;
    if (!create) return NULL;
    struct bucket *b = ar_alloc(a, sizeof *b);
    b->key = key;
    b->next = t->v[h];
    t->v[h] = b;
    return &b->list;
}

static void index_rule(struct css_ctx *x, const struct rule *r, const struct selector *s, uint32_t order, bool ua) {
    if (s->pseudo == PE_OTHER) return;
    struct ient *ie = ar_alloc(&x->a, sizeof *ie);
    ie->sel = s;
    ie->r = r;
    ie->order = order;
    ie->ua = ua;
    const struct compound *c = s->right;
    const char *id = NULL, *cls = NULL, *tag = NULL;
    for (int i = 0; i < c->n; i++) {
        if (c->s[i].kind == SK_ID && !id) id = c->s[i].name;
        else if (c->s[i].kind == SK_CLASS && !cls) cls = c->s[i].name;
        else if (c->s[i].kind == SK_TAG) tag = c->s[i].name;
    }
    struct ient **slot = id ? ht_slot(&x->a, &x->ids, id, true)
                       : cls ? ht_slot(&x->a, &x->classes, cls, true)
                       : tag ? ht_slot(&x->a, &x->tags, tag, true)
                             : &x->univ;
    ie->next = *slot;
    *slot = ie;
}

static sheet_t *ua_sheet, *quirks_sheet;
static arena_t ua_arena;

static struct css_ctx *build_index(web_doc *d, int vw, int vh) {
    struct css_ctx *x = calloc(1, sizeof *x);
    ht_init(&x->a, &x->ids, 1024);
    ht_init(&x->a, &x->classes, 4096);
    ht_init(&x->a, &x->tags, 256);
    if (!ua_sheet) {
        const char *ua = css_ua_sheet();
        ua_sheet = css_parse_sheet(&ua_arena, ua, strlen(ua), "about:blank", -1, NULL);
        ua_sheet->ua = true;
        /* quirks mode: tables do not inherit text alignment or font properties */
        const char *q = "table { text-align: left; white-space: normal; line-height: normal; font-weight: normal;"
                        " font-size: medium; font-style: normal; }";
        quirks_sheet = css_parse_sheet(&ua_arena, q, strlen(q), "about:blank", -1, NULL);
        quirks_sheet->ua = true;
    }
    /* sheets in cascade order */
    pvec *sh = &d->sty.sheets;
    for (int i = 1; i < sh->n; i++)
        for (int j = i; j > 0 && ((sheet_t *)sh->v[j - 1])->order > ((sheet_t *)sh->v[j])->order; j--) {
            void *t = sh->v[j];
            sh->v[j] = sh->v[j - 1];
            sh->v[j - 1] = t;
        }
    uint32_t order = 1;
    for (int i = -2; i < sh->n; i++) {
        sheet_t *s = i == -2 ? ua_sheet : i == -1 ? quirks_sheet : sh->v[i];
        if (i == -1 && !d->quirks) continue;
        for (struct rule *r = s->first; r; r = r->next, order++) {
            if (!r->ndecls || !media_chain_ok(r->media, vw, vh, web_js_enabled(d))) continue;
            for (int k = 0; k < r->sel->n; k++) index_rule(x, r, r->sel->v[k], order, s->ua);
        }
    }
    return x;
}

void css_styling_free(struct styling *st) {
    if (st->ctx) {
        ar_free(&st->ctx->a);
        free(st->ctx);
        st->ctx = NULL;
    }
    pv_free(&st->sheets);
}

/* ---------------------------------------------------------------- presentational hints */
struct hints {
    struct decl d[24];
    int n;
    arena_t *a;
};

static void hint(struct hints *h, const char *prop, const char *fmt, ...) {
    if (h->n >= 24) return;
    char buf[256];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    const struct propdef *p = css_prop_lookup(prop, strlen(prop));
    if (!p) return;
    struct decl *d = &h->d[h->n++];
    memset(d, 0, sizeof *d);
    d->p = p;
    d->value = ar_strdup(h->a, buf);
}

/* an HTML length attribute: "100", "50%", "100px" */
static bool html_len(const char *v, char *out, size_t n) {
    if (!v) return false;
    while (is_space((unsigned char)*v)) v++;
    char *end;
    double x = strtod(v, &end);
    if (end == v || x < 0) return false;
    if (*end == '%') snprintf(out, n, "%g%%", x);
    else snprintf(out, n, "%gpx", x);
    return true;
}

/* a legacy colour attribute: names, #rgb, or bare hex */
static bool html_color(const char *v, char *out, size_t n) {
    if (!v || !*v) return false;
    uint32_t c;
    if (css_color(v, strlen(v), &c)) {
        snprintf(out, n, "%s", v);
        return true;
    }
    char buf[16];
    snprintf(buf, sizeof buf, "#%.8s", v);
    if (css_color(buf, strlen(buf), &c)) {
        snprintf(out, n, "%s", buf);
        return true;
    }
    return false;
}

static void pres_hints(node_t *e, struct hints *h) {
    char b[64];
    const char *v;
    int t = e->tag;
    if ((v = node_attr(e, "align"))) {
        bool block_align = t == T_div || t == T_p || (t >= T_h1 && t <= T_h6) || t == T_caption || t == T_td ||
                           t == T_th || t == T_tr || t == T_thead || t == T_tbody || t == T_tfoot || t == T_legend;
        if (block_align) {
            if (str_ieq(v, "middle") || str_ieq(v, "center") || str_ieq(v, "absmiddle"))
                hint(h, "text-align", "center");
            else if (str_ieq(v, "left") || str_ieq(v, "right") || str_ieq(v, "justify")) hint(h, "text-align", "%s", v);
        } else if (t == T_table) {
            if (str_ieq(v, "center")) hint(h, "margin-left", "auto"), hint(h, "margin-right", "auto");
            else if (str_ieq(v, "left") || str_ieq(v, "right")) hint(h, "float", "%s", v);
        } else if (t == T_img || t == T_iframe || t == T_object || t == T_embed || t == T_input) {
            if (str_ieq(v, "left") || str_ieq(v, "right")) hint(h, "float", "%s", v);
            else if (str_ieq(v, "middle") || str_ieq(v, "absmiddle")) hint(h, "vertical-align", "middle");
            else if (str_ieq(v, "top") || str_ieq(v, "bottom")) hint(h, "vertical-align", "%s", v);
        } else if (t == T_hr) {
            if (str_ieq(v, "left")) hint(h, "margin-left", "0");
            else if (str_ieq(v, "right")) hint(h, "margin-right", "0");
        }
    }
    if ((t == T_body || t == T_table || t == T_tr || t == T_td || t == T_th) && html_color(node_attr(e, "bgcolor"), b, sizeof b))
        hint(h, "background-color", "%s", b);
    if ((t == T_body || t == T_table || t == T_td || t == T_th) && (v = node_attr(e, "background")) && *v)
        hint(h, "background-image", "url(\"%s\")", v);
    if (t == T_body) {
        if (html_color(node_attr(e, "text"), b, sizeof b)) hint(h, "color", "%s", b);
        if (html_len(node_attr(e, "marginwidth"), b, sizeof b) || html_len(node_attr(e, "leftmargin"), b, sizeof b))
            hint(h, "margin-left", "%s", b), hint(h, "margin-right", "%s", b);
        if (html_len(node_attr(e, "marginheight"), b, sizeof b) || html_len(node_attr(e, "topmargin"), b, sizeof b))
            hint(h, "margin-top", "%s", b), hint(h, "margin-bottom", "%s", b);
    }
    bool sized = t == T_table || t == T_td || t == T_th || t == T_img || t == T_iframe || t == T_embed ||
                 t == T_object || t == T_video || t == T_canvas || t == T_col || t == T_hr || t == T_svg ||
                 (t == T_input && (v = node_attr(e, "type")) && str_ieq(v, "image"));
    if (t == T_svg) {
        /* SVG geometry attributes are CSS presentation attributes: preserve
           units, explicit zero and the normal author/inline cascade. */
        if ((v = node_attr(e, "width"))) hint(h, "width", "%s", v);
        if ((v = node_attr(e, "height"))) hint(h, "height", "%s", v);
    } else if (sized) {
        if (html_len(node_attr(e, "width"), b, sizeof b) && strcmp(b, "0px")) hint(h, "width", "%s", b);
        if (t != T_hr && t != T_col && html_len(node_attr(e, "height"), b, sizeof b) && strcmp(b, "0px"))
            hint(h, "height", "%s", b);
    }
    if (t == T_td || t == T_th) {
        if ((v = node_attr(e, "valign"))) hint(h, "vertical-align", "%s", v);
        if (node_attr(e, "nowrap")) hint(h, "white-space", "nowrap");
        node_t *tab = node_ancestor(e, T_table);
        if (tab) {
            if (html_len(node_attr(tab, "cellpadding"), b, sizeof b)) hint(h, "padding", "%s", b);
            if ((v = node_attr(tab, "border")) && atoi(v) > 0) hint(h, "border", "1px inset #888");
            else if (v && !*v) hint(h, "border", "1px inset #888");
        }
    }
    if (t == T_tr && (v = node_attr(e, "valign"))) hint(h, "vertical-align", "%s", v);
    if (t == T_table) {
        if ((v = node_attr(e, "border"))) {
            int bw = *v ? atoi(v) : 1;
            if (bw > 0) hint(h, "border", "%dpx outset #888", bw);
        }
        if (html_len(node_attr(e, "cellspacing"), b, sizeof b)) hint(h, "border-spacing", "%s", b);
    }
    if (t == T_img || t == T_object) {
        if ((v = node_attr(e, "border"))) hint(h, "border", "%dpx solid", atoi(v));
        if (html_len(node_attr(e, "hspace"), b, sizeof b)) hint(h, "margin-left", "%s", b), hint(h, "margin-right", "%s", b);
        if (html_len(node_attr(e, "vspace"), b, sizeof b)) hint(h, "margin-top", "%s", b), hint(h, "margin-bottom", "%s", b);
    }
    if (t == T_font) {
        if (html_color(node_attr(e, "color"), b, sizeof b)) hint(h, "color", "%s", b);
        if ((v = node_attr(e, "face"))) hint(h, "font-family", "%s", v);
        if ((v = node_attr(e, "size")) && *v) {
            int sz = atoi(v);
            if (*v == '+' || *v == '-') sz += 3;
            if (sz < 1) sz = 1;
            if (sz > 7) sz = 7;
            static const char *const px[] = {"", "10px", "13px", "16px", "18px", "24px", "32px", "48px"};
            hint(h, "font-size", "%s", px[sz]);
        }
    }
    if (t == T_hr) {
        if ((v = node_attr(e, "size")) && atoi(v) > 0) hint(h, "height", "%dpx", atoi(v) > 2 ? atoi(v) - 2 : 0);
        if (html_color(node_attr(e, "color"), b, sizeof b)) hint(h, "border-color", "%s", b), hint(h, "background-color", "%s", b);
        if (node_attr(e, "noshade")) hint(h, "border-style", "solid"), hint(h, "background-color", "gray");
    }
    if ((t == T_ul || t == T_ol || t == T_li) && (v = node_attr(e, "type")) && *v) {
        const char *ls = NULL;
        if (!strcmp(v, "1")) ls = "decimal";
        else if (!strcmp(v, "a")) ls = "lower-alpha";
        else if (!strcmp(v, "A")) ls = "upper-alpha";
        else if (!strcmp(v, "i")) ls = "lower-roman";
        else if (!strcmp(v, "I")) ls = "upper-roman";
        else if (str_ieq(v, "disc") || str_ieq(v, "circle") || str_ieq(v, "square")) ls = v;
        if (ls) hint(h, "list-style-type", "%s", ls);
    }
    if ((t == T_caption) && (v = node_attr(e, "valign")) && str_ieq(v, "bottom")) {
    }
}

/* ---------------------------------------------------------------- the cascade */
struct dent {
    const struct decl *d;
    uint64_t key;
    uint8_t pseudo;
};

static struct dent *dents;
static int ndents, capdents;
static uint8_t *setbits;

static void add_dent(const struct decl *d, uint64_t key, uint8_t pseudo) {
    if (ndents == capdents) {
        capdents = capdents ? capdents * 2 : 256;
        dents = realloc(dents, sizeof *dents * (size_t)capdents);
    }
    dents[ndents].d = d;
    /* Important origins reverse the normal order: UA rules such as scripting
       noscript suppression outrank even an author's important inline style. */
    dents[ndents].key = d->important ? (key ^ (1ull << 62)) | (1ull << 63) : key;
    dents[ndents].pseudo = pseudo;
    ndents++;
}

static void add_rule_decls(const struct rule *r, uint32_t spec, uint32_t order, bool author, uint8_t pseudo) {
    for (int i = 0; i < r->ndecls; i++) {
        uint64_t key = (author ? 1ull << 62 : 0) | (uint64_t)(spec & 0x3FFFFFFF) << 32 |
                       (uint64_t)(order & 0xFFFFFF) << 8 | (uint64_t)(i < 255 ? i : 255);
        add_dent(&r->decls[i], key, pseudo);
    }
}

static void collect(const struct ient *l, node_t *e) {
    for (; l; l = l->next)
        if (match_from(l->sel->right, e)) add_rule_decls(l->r, l->sel->spec, l->order, !l->ua, l->sel->pseudo);
}

static int cmp_dent(const void *a, const void *b) {
    uint64_t x = ((const struct dent *)a)->key, y = ((const struct dent *)b)->key;
    return x < y ? 1 : x > y ? -1 : 0;
}

static const char *find_var(const struct custom_prop *v, const char *name, size_t n) {
    for (; v; v = v->next)
        if (strlen(v->name) == n && !memcmp(v->name, name, n)) return v->value;
    return NULL;
}

/* replace var() and env() references; false if a variable is missing without a fallback */
static bool subst(const char *s, size_t n, const struct custom_prop *vars, sbuf *out, int depth) {
    if (depth > 16) return false;
    size_t i = 0;
    while (i < n) {
        bool is_var = i + 4 <= n && strn_ieq(s + i, "var(", 4) && (i == 0 || !ident_char((unsigned char)s[i - 1]));
        bool is_env = !is_var && i + 4 <= n && strn_ieq(s + i, "env(", 4) && (i == 0 || !ident_char((unsigned char)s[i - 1]));
        if (!is_var && !is_env) {
            sb_putc(out, s[i++]);
            continue;
        }
        const char *a = s + i + 4, *e = s + n;
        const char *close = scan_to(a, e, ")");
        if (close >= e) return false;
        const char *comma = scan_to(a, close, ",");
        const char *ns = skip_ws(a, comma), *ne = comma;
        trim_r(ns, &ne);
        const char *val = is_var ? find_var(vars, ns, (size_t)(ne - ns)) : NULL;
        if (val) {
            if (!subst(val, strlen(val), vars, out, depth + 1)) return false;
        } else if (comma < close) {
            if (!subst(comma + 1, (size_t)(close - comma - 1), vars, out, depth + 1)) return false;
        } else if (is_env) {
            sb_puts(out, "0px");
        } else return false;
        i = (size_t)(close + 1 - s);
    }
    return true;
}

struct cascade {
    web_doc *d;
    float rem;
    int vw, vh;
    sbuf vbuf;
};

static void apply_decl(struct cascade *c, struct cx *cx, const struct decl *d) {
    const char *v = d->value;
    size_t n = strlen(v);
    if (strstr(v, "var(") || strstr(v, "env(")) {
        c->vbuf.n = 0;
        if (!subst(v, n, cx->s->vars, &c->vbuf, 0)) return;
        v = sb_cstr(&c->vbuf);
        n = c->vbuf.n;
    }
    css_apply(d->p, v, n, cx);
}

static style_t *compute(struct cascade *c, node_t *e, const style_t *parent, uint8_t pseudo) {
    web_doc *d = c->d;
    style_t *s = ar_alloc(&d->smem, sizeof *s);
    css_style_init(s, parent);
    /* custom properties: the most important declaration of each name */
    const struct custom_prop *inherited = s->vars;
    struct custom_prop *mine = NULL;
    for (int i = 0; i < ndents; i++) {
        const struct decl *x = dents[i].d;
        if (dents[i].pseudo != pseudo || x->p) continue;
        bool dup = false;
        for (struct custom_prop *m = mine; m; m = m->next)
            if (!strcmp(m->name, x->name)) dup = true;
        if (dup) continue;
        struct custom_prop *cp = ar_alloc(&d->smem, sizeof *cp);
        cp->name = x->name;
        cp->value = x->value;
        cp->next = mine;
        mine = cp;
    }
    if (mine) {
        struct custom_prop *last = mine;
        while (last->next) last = last->next;
        last->next = (struct custom_prop *)inherited;
        s->vars = mine;
        /* resolve references now, so descendants see this element's values */
        for (struct custom_prop *m = mine; m != inherited; m = m->next) {
            if (!strstr(m->value, "var(")) continue;
            sbuf b = {0};
            if (subst(m->value, strlen(m->value), s->vars, &b, 0)) m->value = ar_strndup(&d->smem, b.p ? b.p : "", b.n);
            else m->value = "";
            sb_free(&b);
        }
    }
    memset(setbits, 0, (size_t)css_prop_count());
    struct cx cx = {s, parent, e, parent ? parent->font_size : 16, c->rem, (float)c->vw, (float)c->vh,
                    &d->smem, setbits, true};
    for (int i = 0; i < ndents; i++)
        if (dents[i].pseudo == pseudo && dents[i].d->p && css_prop_is_font(dents[i].d->p)) apply_decl(c, &cx, dents[i].d);
    cx.font_pass = false;
    cx.em = s->font_size;
    for (int i = 0; i < ndents; i++)
        if (dents[i].pseudo == pseudo && dents[i].d->p) apply_decl(c, &cx, dents[i].d);
    css_style_finish(s, parent, e == d->html && !pseudo);
    return s;
}

static void clear_styles(node_t *n) {
    for (node_t *c = n->first; c; c = c->next) {
        c->style = NULL;
        clear_styles(c);
    }
}

static void cascade_node(struct cascade *c, node_t *e, const style_t *parent) {
    web_doc *d = c->d;
    struct css_ctx *x = d->sty.ctx;
    ndents = 0;
    if (e->id) {
        struct ient **l = ht_slot(NULL, &x->ids, e->id, false);
        if (l) collect(*l, e);
    }
    for (int i = 0; i < e->nclasses; i++) {
        bool dup = false;
        for (int j = 0; j < i; j++)
            if (!strcmp(e->classes[i], e->classes[j])) dup = true;
        if (dup) continue;
        struct ient **l = ht_slot(NULL, &x->classes, e->classes[i], false);
        if (l) collect(*l, e);
    }
    struct ient **l = ht_slot(NULL, &x->tags, e->name, false);
    if (l) collect(*l, e);
    collect(x->univ, e);
    /* presentational hints and the style attribute */
    struct hints h = {.n = 0, .a = &d->smem};
    if (!e->foreign || e->tag == T_svg) pres_hints(e, &h);
    for (int i = 0; i < h.n; i++) add_dent(&h.d[i], 1ull << 62, PE_NONE);
    const char *sa = node_attr(e, "style");
    if (sa && *sa) {
        struct pctx pc = {&d->smem, NULL, d->base, NULL, 0};
        struct decl *ds;
        int nd;
        parse_body(&pc, sa, sa + strlen(sa), NULL, NULL, &ds, &nd);
        for (int i = 0; i < nd; i++) add_dent(&ds[i], 1ull << 62 | (uint64_t)0x3FFFFFFF << 32 | (uint64_t)(i < 255 ? i : 255), PE_NONE);
    }
    qsort(dents, (size_t)ndents, sizeof *dents, cmp_dent);
    bool has_before = false, has_after = false;
    for (int i = 0; i < ndents; i++) {
        if (dents[i].pseudo == PE_BEFORE) has_before = true;
        if (dents[i].pseudo == PE_AFTER) has_after = true;
    }
    style_t *s = compute(c, e, parent, PE_NONE);
    e->style = s;
    if (e == d->html) c->rem = s->font_size;
    if (s->display != D_NONE && !(e->tag == T_img || e->tag == T_input || e->tag == T_br || e->tag == T_hr)) {
        if (has_before) {
            style_t *b = compute(c, e, s, PE_BEFORE);
            if (b->content && b->display != D_NONE) s->before = b;
        }
        if (has_after) {
            style_t *a = compute(c, e, s, PE_AFTER);
            if (a->content && a->display != D_NONE) s->after = a;
        }
    }
    if (s->display == D_NONE) {
        clear_styles(e);
        return;
    }
    for (node_t *ch = e->first; ch; ch = ch->next) {
        if (ch->type == N_ELEM) cascade_node(c, ch, s);
        else ch->style = NULL;
    }
}

void css_cascade(web_doc *d, int vw, int vh) {
    if (d->sty.ctx) {
        ar_free(&d->sty.ctx->a);
        free(d->sty.ctx);
    }
    d->sty.ctx = build_index(d, vw, vh);
    d->sty.index_w = vw;
    d->sty.index_h = vh;
    if (!setbits) setbits = malloc((size_t)css_prop_count());
    ar_free(&d->smem);
    struct cascade c = {d, 16, vw, vh, {0}};
    if (d->html) cascade_node(&c, d->html, NULL);
    sb_free(&c.vbuf);
    d->styled_w = vw;
    d->styled_h = vh;
}
