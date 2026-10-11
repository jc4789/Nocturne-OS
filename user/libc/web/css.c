/* CSS: parsing stylesheets (rules, @media, @import, @supports, nesting), selectors, an index of
   the rules by id, class and tag, and the cascade that computes every element's style. */
#include <stdio.h>
#include <ctype.h>
#include <stdarg.h>
#include <math.h>
#include <limits.h>
#include <nocturne.h>
#include <parallel.h>
#include "webi.h"
#include "web_dialog.h"
#include "form_validation.h"
#include "form_value.h"
#include "avmedia.h"

/* Slot 2 is reserved for CSS. The supervisor and each subtree worker own
   independent declaration/variable scratch; rule indexes and DOM are frozen. */
struct dent;
struct var_index_entry;
struct css_scratch {
    struct dent *declarations;
    int declaration_count, declaration_capacity;
    uint8_t *properties;
    const struct custom_prop *variable_head;
    struct var_index_entry *variables;
    size_t variable_capacity;
    arena_t *arena;
    bool worker;
    bool form_snapshot;
};
static struct css_scratch css_main_scratch;
static struct css_scratch *css_scratch_get(void) {
    struct css_scratch *s = thread_local_get(2);
    return s ? s : &css_main_scratch;
}
static bool css_worker(void) { return css_scratch_get()->worker; }
/* Rank 0 also executes subtree jobs, with isolated worker scratch. Its native
   chrome service must remain responsive while APs only observe cancellation. */
static bool css_can_service(void) { return !css_worker() || parallel_worker_index() == 0; }
#define dents (css_scratch_get()->declarations)
#define ndents (css_scratch_get()->declaration_count)
#define capdents (css_scratch_get()->declaration_capacity)
#define setbits (css_scratch_get()->properties)
#define var_index_head (css_scratch_get()->variable_head)
#define var_index (css_scratch_get()->variables)
#define var_index_capacity (css_scratch_get()->variable_capacity)

/* ---------------------------------------------------------------- data structures */
enum { SK_TAG, SK_ID, SK_CLASS, SK_ATTR, SK_PC, SK_SLOTTED };
/* Internal selector marker, never a generated pseudo box. */
#define PE_SLOTTED 5
enum { AO_EXISTS, AO_EQ, AO_INCL, AO_DASH, AO_PREFIX, AO_SUFFIX, AO_SUBSTR };
enum { PC_FIRST_CHILD, PC_LAST_CHILD, PC_ONLY_CHILD, PC_NTH_CHILD, PC_NTH_LAST_CHILD, PC_FIRST_OF_TYPE,
       PC_LAST_OF_TYPE, PC_ONLY_OF_TYPE, PC_NTH_OF_TYPE, PC_NTH_LAST_OF_TYPE, PC_NOT, PC_IS, PC_NEVER,
       PC_ALWAYS, PC_LINK, PC_CHECKED, PC_DISABLED, PC_ENABLED, PC_ROOT, PC_EMPTY, PC_REQUIRED, PC_OPTIONAL,
       PC_LANG, PC_PLACEHOLDER_SHOWN, PC_READ_WRITE, PC_READ_ONLY, PC_OPEN, PC_HOST, PC_FOCUS, PC_FOCUS_WITHIN, PC_HOVER, PC_ACTIVE,
       PC_VALID, PC_INVALID, PC_IN_RANGE, PC_OUT_OF_RANGE, PC_INDETERMINATE, PC_MODAL };

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
    bool universal; /* share the existing padding before the pointer */
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
    const char *name; /* escaped, case-sensitive container identifier */
    uint8_t axes;    /* 1 = inline, 2 = both (horizontal writing mode) */
    bool container;
};

struct font_source {const char *url;struct font_source *next;};
struct font_face {
    const char *family;
    struct font_source *sources;
    unsigned weight_lo,weight_hi;
    bool italic;
    const struct mcond *media;
    struct font_face *next;
};

struct rule {
    struct sellist *sel;
    struct decl *decls;
    int ndecls;
    const struct mcond *media;
    struct rule *next;
    const char *selector_text;
};

struct opacity_frame {
    struct opacity_frame *next;
    float offset;
    struct decl *decls;
    int count;
};
struct opacity_keyframes {
    struct opacity_keyframes *next;
    const char *name;
    const struct mcond *media;
    struct opacity_frame *first, *last;
};

struct sheet {
    struct rule *first, *last;
    struct font_face *fonts,*fonts_last;
    struct opacity_keyframes *motions, *motions_last;
    double order;
    bool ua;
    node_t *scope; /* NULL: document author sheet; otherwise its shadow root */
    const char *owner_media;
    struct css_rule_info *cssom_first, *cssom_last;
    uint32_t cssom_count;
    const char *cssom_source;
    size_t cssom_length;
};

struct pctx {
    arena_t *a;
    sheet_t *sh;
    const char *base;
    pvec *imports;
    int nimports;
    bool supports_probe; /* reject accepted-but-unimplemented selector fallbacks */
    unsigned rule_depth;
};

/* ---------------------------------------------------------------- scanning */
/* the first of stops at nesting depth 0, skipping strings, escapes, () and [] */
static const char *scan_to(const char *s, const char *e, const char *stops) {
    size_t depth = 0;
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

/* Container names must not acquire a length cap or allocate scratch memory
   while selectors are matched on APs. Decode one codepoint at a time. */
static unsigned container_hex(unsigned char c) {
    return c >= '0' && c <= '9' ? c-'0' : c >= 'a' && c <= 'f' ? c-'a'+10 :
           c >= 'A' && c <= 'F' ? c-'A'+10 : 16;
}
static uint32_t container_char(const char **ps,const char *e) {
    const unsigned char *s=(const unsigned char *)*ps;
    uint32_t cp=*s++; unsigned count=0;
    if(cp=='\\' && (const char *)s<e) {
        cp=0;
        while((const char *)s<e && container_hex(*s)<16 && count++<6)cp=cp*16+container_hex(*s++);
        if(!count)cp=*s++;
        else if((const char *)s<e && is_space(*s)) { if(*s++=='\r' && (const char *)s<e && *s=='\n')s++; }
        if(!cp || cp>0x10ffff || (cp>=0xd800 && cp<=0xdfff))cp=0xfffd;
    } else if(cp>=0xc0) {
        unsigned more=cp<0xe0?1:cp<0xf0?2:3;
        cp &= more==1?31:more==2?15:7;
        while(more-- && (const char *)s<e && (*s&0xc0)==0x80)cp=(cp<<6)|(*s++&63);
    }
    *ps=(const char *)s;return cp;
}
static const char *container_ident_end(const char *s,const char *e) {
    while(s<e) {
        if(ident_char((unsigned char)*s)){s++;continue;}
        if(*s!='\\' || s+1==e || s[1]=='\n' || s[1]=='\r' || s[1]=='\f')break;
        container_char(&s,e);
    }
    return s;
}
static bool container_ident_equal(const char *s,const char *e,const char *t,const char *te,bool insensitive) {
    while(s<e && t<te) {
        uint32_t a=container_char(&s,e),b=container_char(&t,te);
        if(insensitive && a<128 && b<128){a=lower(a);b=lower(b);}
        if(a!=b)return false;
    }
    return s==e && t==te;
}
static bool container_reserved(const char *s,const char *e) {
    static const char *reserved[]={"none","and","or","not","initial","inherit","unset","revert","revert-layer","default"};
    for(size_t i=0;i<sizeof reserved/sizeof *reserved;i++)
        if(container_ident_equal(s,e,reserved[i],reserved[i]+strlen(reserved[i]),true))return true;
    return false;
}
static bool container_ident_valid(const char *s,const char *e) {
    if(s==e || container_ident_end(s,e)!=e || container_reserved(s,e))return false;
    /* Escaping a digit is valid; an unescaped digit at the start is not. */
    const char *p=s;if(*p=='-'){if(++p==e)return false;if(*p=='-')return true;}
    return *p=='\\' || *p=='_' || isalpha((unsigned char)*p) || (unsigned char)*p>=0x80;
}
bool css_container_names_valid(const char *s,size_t n) {
    const char *e=s+n;bool any=false;
    while((s=skip_ws(s,e))<e) {
        const char *p=container_ident_end(s,e);
        if(!container_ident_valid(s,p) || (p<e && !is_space((unsigned char)*p)))return false;
        any=true;s=p;
    }
    return any;
}
static bool container_preserve_names(const char *property,size_t pn,const char *v,size_t vn) {
    if(strn_ieq(property,"container-name",pn))return css_container_names_valid(v,vn);
    if(!strn_ieq(property,"container",pn))return false;
    const char *s=v,*e=v+vn;
    while((s=skip_ws(s,e))<e && *s!='/') {
        const char *p=container_ident_end(s,e);
        if(p==s || (p<e && !is_space((unsigned char)*p) && *p!='/'))return false;
        s=p;
    }
    return css_container_names_valid(v,(size_t)(s-v));
}
static bool container_names_match(const char *names,const char *name) {
    if(!name)return true;if(!names)return false;
    const char *s=names,*e=names+strlen(names),*ne=name+strlen(name);
    while((s=skip_ws(s,e))<e) {
        const char *p=container_ident_end(s,e);
        if(p==s)break;
        if(container_ident_equal(s,p,name,ne,false))return true;
        s=p;
    }
    return false;
}

/* an identifier with escapes resolved, or NULL */
static const char *read_ident(arena_t *a, const char **ps, const char *e, bool lowercase) {
    const char *s = *ps;
    char buf[256];
    size_t n = 0;
    bool truncated = false;
    while (s < e) {
        unsigned char c = (unsigned char)*s;
        if (ident_char(c)) {
            if (n < sizeof buf - 1) buf[n++] = lowercase ? (char)lower(c) : (char)c;
            else truncated = true;
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
                if (!cp || cp > 0x10ffff || (cp >= 0xd800 && cp <= 0xdfff)) cp = 0xfffd;
                if (lowercase && cp < 128) cp = (uint32_t)lower((int)cp);
                char u[4];
                int ul = utf8_put(u, cp ? cp : 0xFFFD);
                if (n + (size_t)ul < sizeof buf) memcpy(buf + n, u, (size_t)ul), n += (size_t)ul;
                else truncated = true;
            } else {
                if (n < sizeof buf - 1) buf[n++] = lowercase ? (char)lower((unsigned char)*s) : *s;
                else truncated = true;
                s++;
            }
        } else break;
    }
    if (!n || truncated) return NULL;
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
    {"visited", PC_NEVER}, {"hover", PC_HOVER}, {"active", PC_ACTIVE}, {"focus", PC_FOCUS},
    {"focus-within", PC_FOCUS_WITHIN}, {"focus-visible", PC_NEVER}, {"target", PC_NEVER}, {"target-within", PC_NEVER},
    {"indeterminate", PC_INDETERMINATE}, {"invalid", PC_INVALID}, {"valid", PC_VALID}, {"user-invalid", PC_NEVER},
    {"user-valid", PC_NEVER}, {"default", PC_NEVER}, {"fullscreen", PC_NEVER}, {"modal", PC_MODAL},
    {"popover-open", PC_NEVER}, {"autofill", PC_NEVER}, {"in-range", PC_IN_RANGE}, {"out-of-range", PC_OUT_OF_RANGE},
    {"playing", PC_NEVER}, {"paused", PC_NEVER}, {"current", PC_NEVER}, {"past", PC_NEVER}, {"future", PC_NEVER},
    {"first", PC_NEVER}, {"left", PC_NEVER}, {"right", PC_NEVER}, {"blank", PC_NEVER}, {"local-link", PC_NEVER},
    {"host", PC_HOST}, {"state", PC_NEVER}, {"picture-in-picture", PC_NEVER}, {"-webkit-autofill", PC_NEVER},
    {"-moz-focusring", PC_NEVER}, {"-moz-ui-invalid", PC_NEVER},
};

/* one compound selector; *ps advanced past it. pseudo receives a pseudo-element. */
static struct compound *parse_compound(struct pctx *pc, const char **ps, const char *e, uint32_t *spec, uint8_t *pseudo) {
    struct simple tmp[32];
    int n = 0;
    const char *s = *ps;
    bool type_seen = false, universal = false;
    while (s < e && n < 32) {
        char c = *s;
        struct simple *x = &tmp[n];
        memset(x, 0, sizeof *x);
        if (*pseudo && c != ':') return NULL; /* nothing may follow a pseudo-element but pseudo-classes */
        if (c == '*') {
            if (pc->supports_probe && (type_seen || n)) return NULL;
            type_seen = true;
            universal = true;
            s++;
            if (s < e && *s == '|') return NULL;
            continue;
        }
        if (ident_char((unsigned char)c) || c == '\\') {
            if (pc->supports_probe && (type_seen || isdigit((unsigned char)c) ||
                (c == '-' && s + 1 < e && isdigit((unsigned char)s[1])))) return NULL;
            type_seen = true;
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
            if (pc->supports_probe && s < e && (isdigit((unsigned char)*s) ||
                (*s == '-' && s + 1 < e && isdigit((unsigned char)s[1])))) return NULL;
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
                if (!strcmp(name, "slotted") && args) {
                    x->args = parse_sellist(pc, args, args_end, false);
                    if (!x->args || x->args->n != 1 || x->args->v[0]->pseudo || x->args->v[0]->right->left) return NULL;
                    x->kind = SK_SLOTTED;
                    *pseudo = PE_SLOTTED;
                    *spec = spec_add(*spec, spec_add(SPEC_C, sellist_max_spec(x->args)));
                    n++;
                    continue;
                }
                if (!strcmp(name, "before")) *pseudo = PE_BEFORE;
                else if (!strcmp(name, "after")) *pseudo = PE_AFTER;
                else if (!strcmp(name, "backdrop")) *pseudo = PE_BACKDROP;
                else if (!strncmp(name, "-webkit-", 8) || !strncmp(name, "-moz-", 5) || !strncmp(name, "-ms-", 4))
                    return NULL;
                else if (pc->supports_probe) return NULL;
                else *pseudo = PE_OTHER;
                *spec = spec_add(*spec, SPEC_C);
                continue;
            }
            x->kind = SK_PC;
            if (args) {
                if (!strcmp(name, "not") || !strcmp(name, "is") || !strcmp(name, "where") ||
                    !strcmp(name, "matches") || !strcmp(name, "-webkit-any") || !strcmp(name, "-moz-any") ||
                    !strcmp(name, "any")) {
                    bool forgiving = !pc->supports_probe && strcmp(name, "not") != 0;
                    x->args = parse_sellist(pc, args, args_end, forgiving);
                    if (!x->args) return NULL;
                    x->pc = !strcmp(name, "not") ? PC_NOT : PC_IS;
                    if (strcmp(name, "where")) *spec = spec_add(*spec, sellist_max_spec(x->args));
                } else if (!strcmp(name, "has")) {
                    if (pc->supports_probe) return NULL;
                    x->pc = PC_NEVER;
                } else if (!strncmp(name, "nth-", 4)) {
                    if (pc->supports_probe && args_end - args >= 64) return NULL;
                    static const char *const nth[] = {"nth-child", "nth-last-child", "nth-of-type", "nth-last-of-type"};
                    static const uint8_t nthpc[] = {PC_NTH_CHILD, PC_NTH_LAST_CHILD, PC_NTH_OF_TYPE, PC_NTH_LAST_OF_TYPE};
                    int k = 0;
                    while (k < 4 && strcmp(name, nth[k])) k++;
                    if (k == 4) return NULL;
                    x->pc = nthpc[k];
                    const char *ae = args_end;
                    for (const char *q = args; q + 3 < ae; q++) /* "An+B of S": the S is ignored */
                        if (is_space((unsigned char)q[0]) && strn_ieq(q + 1, "of", 2) && is_space((unsigned char)q[3])) {
                            if (pc->supports_probe) return NULL;
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
                    if (pc->supports_probe) return NULL;
                    const char *p = skip_ws(args, args_end);
                    x->pc = strn_ieq(p, "ltr", 3) ? PC_ALWAYS : PC_NEVER;
                    *spec = spec_add(*spec, SPEC_B);
                } else if (!strcmp(name, "host")) {
                    x->args = parse_sellist(pc, args, args_end, false);
                    if (!x->args || x->args->n != 1 || x->args->v[0]->pseudo || x->args->v[0]->right->left) return NULL;
                    x->pc = PC_HOST;
                    *spec = spec_add(*spec, spec_add(SPEC_B, sellist_max_spec(x->args)));
                } else if (!strcmp(name, "host-context") || !strcmp(name, "state") ||
                           !strcmp(name, "active-view-transition-type")) {
                    if (pc->supports_probe) return NULL;
                    x->pc = PC_NEVER;
                } else return NULL;
                n++;
                continue;
            }
            size_t k = 0;
            while (k < sizeof simple_pcs / sizeof *simple_pcs && strcmp(simple_pcs[k].name, name)) k++;
            if (k == sizeof simple_pcs / sizeof *simple_pcs) return NULL;
            x->pc = simple_pcs[k].pc;
            if (pc->supports_probe && (x->pc == PC_NEVER || x->pc == PC_ALWAYS || !strcmp(name, "scope"))) return NULL;
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
    cp->universal = universal;
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
        if (sel->pseudo || (pc->supports_probe && cur && !pending)) return NULL;
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
    if (pc->supports_probe) {
        const char *last = e;
        trim_r(s, &last);
        if (last == s || last[-1] == ',') return NULL;
    }
    struct selector *tmp[64];
    int n = 0;
    while (s < e) {
        const char *c = scan_to(s, e, ",");
        struct selector *x = parse_complex(pc, s, c);
        if (x) {
            if (n < 64) tmp[n++] = x;
            else if (pc->supports_probe) return NULL;
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
static bool match_list(const struct sellist *l, node_t *e, node_t *scope);

/* Selector ancestry stays in the DOM tree. Only a stylesheet's own shadow
   host replaces its root, and that host is featureless outside :host(). */
static node_t *selector_parent(node_t *e, node_t *scope) {
    if (scope && e == scope->shadow_host) return NULL;
    node_t *p = e->parent;
    return scope && p == scope ? scope->shadow_host : p;
}

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
    return !e->foreign && (e->tag == T_input || e->tag == T_button || e->tag == T_select || e->tag == T_textarea ||
           e->tag == T_option || e->tag == T_optgroup || e->tag == T_fieldset);
}

static bool read_write(const node_t *e) {
    if (e->tag == T_input || e->tag == T_textarea) return web_control_read_write(e);
    for (const node_t *n = e; n; n = n->parent) {
        const char *editable = node_attr(n, "contenteditable");
        if (!editable) continue;
        if (!*editable || str_ieq(editable, "true") || str_ieq(editable, "plaintext-only")) return true;
        if (str_ieq(editable, "false")) return false;
    }
    return false;
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

static bool css_hovered(node_t *e) {
    web_doc *d=e?e->owner:NULL;node_t *target=d?d->hover_target:NULL;
    if(!d || !d->live || !target || target->owner!=d || doc_node_root(target,true)!=d->root)return false;
    for(node_t *p=target;p;p=doc_flat_parent(p))if(p==e)return true;
    return false;
}
static bool css_active(node_t *e) {
    for(node_t *p=doc_active_target(e?e->owner:NULL);p;p=doc_flat_parent(p))if(p==e)return true;
    return false;
}
static bool match_pc(const struct simple *x, node_t *e, node_t *scope) {
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
    case PC_NOT: return !match_list(x->args, e, scope);
    case PC_IS: return match_list(x->args, e, scope);
    case PC_HOST: return scope && scope->shadow_host == e && (!x->args || match_list(x->args, e, NULL));
    case PC_FOCUS:
        if (!e->owner) return false;
        for (node_t *f = e->owner->focus; f;) {
            if (f == e) return true;
            node_t *r = doc_node_root(f, false);
            f = r ? r->shadow_host : NULL;
        }
        return false;
    case PC_FOCUS_WITHIN:
        for (node_t *f = e->owner ? e->owner->focus : NULL; f; f = doc_flat_parent(f))
            if (f == e) return true;
        return false;
    case PC_HOVER: return css_hovered(e);
    case PC_ACTIVE: return css_active(e);
    case PC_NEVER: return false;
    case PC_ALWAYS: return true;
    case PC_LINK: return (e->tag == T_a || e->tag == T_area) && node_attr(e, "href");
    case PC_INDETERMINATE:
        return !e->foreign && ((e->tag == T_input && web_input_type(e) == WEB_INPUT_CHECKBOX && e->indeterminate) || (e->tag == T_progress && !node_attr(e, "value")));
    case PC_CHECKED:
        if (css_scratch_get()->form_snapshot) return (e->style_form_state & WEB_STYLE_CHECKED) != 0;
        return (e->tag == T_input && e->checked) || doc_option_selected(e);
    case PC_DISABLED: return form_control(e) && web_control_disabled(e);
    case PC_ENABLED: return form_control(e) && !web_control_disabled(e);
    case PC_ROOT: return e->parent && e->parent->type == N_DOC;
    case PC_EMPTY:
        for (node_t *c = e->first; c; c = c->next)
            if (c->type == N_ELEM || (c->type == N_TEXT && c->textlen)) return false;
        return true;
    case PC_REQUIRED: return web_control_required(e);
    case PC_OPTIONAL: return web_control_required_applicable(e) && !web_control_required(e);
    case PC_VALID: case PC_INVALID: {
        if (css_scratch_get()->form_snapshot)
            return (e->style_form_state & WEB_STYLE_VALIDATABLE) &&
                ((e->style_form_state & WEB_STYLE_INVALID) != 0) == (x->pc == PC_INVALID);
        if (!e->foreign && (e->tag == T_form || e->tag == T_fieldset)) {
            bool valid = web_form_constraints_valid(e->owner, e);
            return x->pc == PC_VALID ? valid : !valid;
        }
        if (!web_control_will_validate(e)) return false;
        bool valid = web_control_validity(e->owner, e) == 0;
        return x->pc == PC_VALID ? valid : !valid;
    }
    case PC_IN_RANGE: case PC_OUT_OF_RANGE: {
        if (css_scratch_get()->form_snapshot)
            return (e->style_form_state & WEB_STYLE_RANGE) &&
                ((e->style_form_state & WEB_STYLE_IN_RANGE) != 0) == (x->pc == PC_IN_RANGE);
        int range = web_control_in_range(e->owner, e);
        return range >= 0 && (x->pc == PC_IN_RANGE ? range == 1 : range == 0);
    }
    case PC_LANG:
        for (node_t *p = e; p; p = doc_shadow_parent(p)) {
            if (p->type != N_ELEM) continue;
            const char *l = node_attr(p, "lang");
            if (l) {
                size_t k = strlen(x->value);
                return !strncasecmp(l, x->value, k) && (l[k] == 0 || l[k] == '-');
            }
        }
        return false;
    case PC_PLACEHOLDER_SHOWN:
        if (css_scratch_get()->form_snapshot) return (e->style_form_state & WEB_STYLE_PLACEHOLDER) != 0;
        return !e->foreign && (e->tag == T_input || e->tag == T_textarea) &&
               node_attr(e, "placeholder") && !*web_input_edit_text(e);
    case PC_READ_WRITE: return read_write(e);
    case PC_READ_ONLY: return !read_write(e);
    case PC_MODAL: return web_dialog_is_modal(e->owner,e);
    case PC_OPEN: return (e->tag == T_details || e->tag == T_dialog) && node_attr(e, "open");
    }
    return false;
}

static bool match_compound(const struct compound *c, node_t *e, node_t *scope) {
    if (scope && e == scope->shadow_host) {
        if (!c->n || c->universal) return false; /* '*' cannot select a featureless host */
        for (int i = 0; i < c->n; i++)
            if (c->s[i].kind != SK_PC || c->s[i].pc != PC_HOST) return false;
    }
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
            if (!match_pc(x, e, scope)) return false;
            break;
        case SK_SLOTTED: break; /* matched against its originating slot below */
        }
    }
    return true;
}

static bool match_from(const struct compound *c, node_t *e, node_t *scope) {
    if (e) {
        if (css_can_service()) { if (!web_native_checkpoint(e->owner)) return false; }
        else if (__atomic_load_n(&e->owner->native_cancelled, __ATOMIC_ACQUIRE)) return false;
    }
    for (int i = 0; i < c->n; i++) if (c->s[i].kind == SK_SLOTTED) {
        if (!scope || !match_list(c->s[i].args, e, NULL)) return false;
        node_t *slot = doc_assigned_slot(e, false);
        /* A slot assigned into another slot can expose the same flattened
           element. Follow assignment links without changing DOM parents. */
        for (; slot; slot = doc_assigned_slot(slot, false))
            if (doc_node_root(slot, false) == scope && match_compound(c, slot, scope)) break;
        if (!slot) return false;
        e = slot;
        break;
    }
    if (!match_compound(c, e, scope)) return false;
    if (!c->left) return true;
    switch (c->comb) {
    case '>': {
        node_t *p = selector_parent(e, scope);
        return p && p->type == N_ELEM && match_from(c->left, p, scope);
    }
    case ' ':
        for (node_t *p = selector_parent(e, scope); p && p->type == N_ELEM; p = selector_parent(p, scope))
            if (match_from(c->left, p, scope)) return true;
        return false;
    case '+': {
        node_t *p = prev_elem(e);
        return p && match_from(c->left, p, scope);
    }
    case '~':
        for (node_t *p = prev_elem(e); p; p = prev_elem(p))
            if (match_from(c->left, p, scope)) return true;
        return false;
    }
    return false;
}

static bool match_list(const struct sellist *l, node_t *e, node_t *scope) {
    for (int i = 0; l && i < l->n; i++)
        if (!l->v[i]->pseudo && match_from(l->v[i]->right, e, scope)) return true;
    return false;
}

/* DOM queries use exactly the cascade's parser and matcher, with a short-lived
   bounded arena so repeated querySelector calls do not grow the document arena. */
static void select_walk(node_t *scope, const struct sellist *sel, pvec *out) {
    for (node_t *n = scope->first; n; n = n->next) {
        if (n->type == N_ELEM && match_list(sel, n, NULL)) pv_push(out, n);
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
    bool matched = sel && match_list(sel, node, NULL);
    ar_free(arena);
    free(arena);
    return matched;
}

/* ---------------------------------------------------------------- feature queries
   The same evaluator serves CSS.supports and @supports. Invalid syntax is kept
   separate from unsupported features, so `not` cannot turn a parse error true.
   Scratch arenas and explicit continuations make queries independent of the DOM. */
struct supports_result { bool valid, value; };

/* Validate component values, remove comments, and preserve strings/escapes.
   Unlike the stylesheet recovery parser, a query must consume the whole input. */
static char *supports_clean(arena_t *a, const char *s, size_t n, size_t *out) {
    if (!s || n == SIZE_MAX) return NULL;
    char *stack = ar_alloc(a, n ? n : 1), quote = 0;
    size_t depth = 0;
    char *b = ar_alloc(a, n + 1);
    size_t k = 0;
    for (size_t i = 0; i < n; i++) {
        unsigned char c = (unsigned char)s[i];
        if (!c || c == 0x7f || (c < 32 && !is_space(c))) return NULL;
        if (c == '\\') {
            if (i + 1 == n) return NULL;
            if (!s[i + 1] || (unsigned char)s[i + 1] == 0x7f ||
                ((unsigned char)s[i + 1] < 32 && !is_space((unsigned char)s[i + 1]))) return NULL;
            if (!quote && (s[i + 1] == '\n' || s[i + 1] == '\r' || s[i + 1] == '\f')) return NULL;
            b[k++] = (char)c;
            b[k++] = s[++i];
            continue;
        }
        if (quote) {
            if (c == '\n' || c == '\r' || c == '\f') return NULL;
            if (c == (unsigned char)quote) quote = 0;
        } else if (c == '"' || c == '\'') quote = (char)c;
        else if (c == '/' && i + 1 < n && s[i + 1] == '*') {
            i += 2;
            while (i + 1 < n && !(s[i] == '*' && s[i + 1] == '/')) i++;
            if (i + 1 >= n) return NULL;
            i++;
            b[k++] = ' ';
            continue;
        } else if (c == '(' || c == '[' || c == '{') {
            stack[depth++] = c == '(' ? ')' : c == '[' ? ']' : '}';
        } else if (c == ')' || c == ']' || c == '}') {
            if (!depth || c != (unsigned char)stack[--depth]) return NULL;
        }
        b[k++] = (char)c;
    }
    if (depth || quote) return NULL;
    b[k] = 0;
    *out = k;
    return b;
}

/* Find a top-level delimiter; input was already balanced by supports_clean. */
static const char *supports_delim(const char *s, const char *e, const char *delims) {
    size_t depth = 0;
    char q = 0;
    for (; s < e; s++) {
        char c = *s;
        if (c == '\\') { if (s + 1 < e) s++; continue; }
        if (q) { if (c == q) q = 0; continue; }
        if (c == '"' || c == '\'') { q = c; continue; }
        if (!depth && strchr(delims, c)) return s;
        if (c == '(' || c == '[' || c == '{') depth++;
        else if (c == ')' || c == ']' || c == '}') depth--;
    }
    return e;
}

static bool supports_custom_name(const char *s, size_t n) {
    if (n <= 2 || s[0] != '-' || s[1] != '-') return false;
    for (size_t i = 2; i < n; i++) if (!ident_char((unsigned char)s[i])) return false;
    return true;
}

/* A var() declaration is deferred by the existing substitution engine, not
   parsed as its fallback alone. Invalid variable references must still fail.
   Strings are not scanned as functions. env() is not advertised: its current
   substitution fallback is not an environment-variable implementation. */
static bool supports_vars(arena_t *a, const char *s, const char *e, bool *has) {
    while (s < e) {
        /* Hash/at-keyword tokens consume their name: #var( and @var( are not
           var() function tokens. An actual function inside a later block is
           still visited by the normal component-value scan. */
        if ((*s == '#' || *s == '@') && s + 1 < e &&
            (ident_char((unsigned char)s[1]) || s[1] == '\\')) {
            s++;
            if (!read_ident(a, &s, e, false)) return false;
            continue;
        }
        if (*s == '"' || *s == '\'') {
            char q = *s++;
            while (s < e && *s != q) { if (*s == '\\') s++; s++; }
            if (s < e) s++;
            continue;
        }
        if (ident_char((unsigned char)*s) || *s == '\\') {
            const char *st = s;
            const char *name = read_ident(a, &s, e, true);
            if (!name || s == st) return false;
            if (s < e && *s == '(') {
                const char *end = supports_delim(s + 1, e, ")");
                if (end == e) return false;
                if (!strcmp(name, "url")) {
                    const char *q = skip_ws(s + 1, end);
                    if (q < end && (*q == '"' || *q == '\'')) {
                        char quote = *q++;
                        while (q < end && *q != quote) { if (*q == '\\' && q + 1 < end) q++; q++; }
                        if (q == end || skip_ws(q + 1, end) != end) return false;
                    } else {
                        for (; q < end; q++) {
                            if (*q == '\\') { if (++q == end) return false; continue; }
                            if (is_space((unsigned char)*q)) { if (skip_ws(q, end) != end) return false; break; }
                            if (*q == '(' || *q == '"' || *q == '\'') return false;
                        }
                    }
                    /* An unquoted URL is one URL token, not nested functions;
                       a quoted URL is a literal string, not a var reference. */
                    s = end + 1;
                    continue;
                }
                if (!strcmp(name, "env")) return false;
                if (!strcmp(name, "var")) {
                    const char *comma = supports_delim(s + 1, end, ",");
                    const char *p = skip_ws(s + 1, comma), *pe = comma;
                    trim_r(p, &pe);
                    const char *id = read_ident(a, &p, pe, false);
                    if (p != pe || !id || !supports_custom_name(id, strlen(id))) return false;
                    *has = true;
                    /* The balanced component stream can be visited in place:
                       descend into fallback without a C call/return stack. */
                    s = comma < end ? comma + 1 : end + 1;
                } else s++;
            }
        } else s++;
    }
    return true;
}

static bool supports_decl(arena_t *a, const char *property, size_t pn, const char *v, size_t vn) {
    if (!property || !pn || memchr(property, 0, pn)) return false;
    bool custom = supports_custom_name(property, pn);
    const struct propdef *p = custom ? NULL : css_prop_lookup(property, pn);
    if (!custom && !p) return false;
    if(!container_preserve_names(property,pn,v,vn))v = css_value_canonical(a, v, vn, &vn);
    while (vn && is_space((unsigned char)*v)) v++, vn--;
    while (vn && is_space((unsigned char)v[vn - 1])) vn--;
    if (supports_delim(v, v + vn, ";!") != v + vn) return false;
    if (!custom && supports_delim(v, v + vn, "{}") != v + vn) return false;
    bool has = false;
    if (!supports_vars(a, v, v + vn, &has)) return false;
    if (custom) return true;
    if (!vn) return false;
    return has || css_value_supported(p, v, vn);
}

bool css_supports_declaration(const char *property, size_t pn, const char *value, size_t vn) {
    if (!property || !value || pn == SIZE_MAX || vn == SIZE_MAX) return false;
    arena_t *a = calloc(1, sizeof *a);
    if (!a) return false;
    jmp_buf trap;
    a->trap = &trap;
    if (setjmp(trap)) { ar_free(a); free(a); return false; }
    size_t n;
    char *v = supports_clean(a, value, vn, &n);
    bool result = v && supports_decl(a, property, pn, v, n);
    ar_free(a);
    free(a);
    return result;
}

struct supports_walk {
    struct supports_walk *parent;
    const char *s, *e, *p, *next;
    struct supports_result value;
    unsigned stage, op;
    bool term, negate;
};

static struct supports_result supports_expr(arena_t *a, const char *s, const char *e) {
    struct supports_result bad = {false, false}, result = bad;
    struct supports_walk *f = ar_alloc(a, sizeof *f);
    f->s = s; f->e = e;
    while (f) {
        if (!f->stage && !f->term) {
            f->s = skip_ws(f->s, f->e); trim_r(f->s, &f->e);
            f->p = f->s;
            const char *word = read_ident(a, &f->p, f->e, true);
            f->negate = word && !strcmp(word, "not");
            if (f->negate && (f->p == f->e || !is_space((unsigned char)*f->p))) { result = bad; goto done; }
            if (!f->negate) f->p = f->s;
            f->stage = 1;
            goto term;
        }
        if (!f->stage && f->term) {
            const char *p = skip_ws(f->s, f->e);
            if (p == f->e) { result = bad; goto done; }
            if (*p != '(') {
                const char *name = read_ident(a, &p, f->e, true);
                if (!name || p == f->e || *p != '(') { result = bad; goto done; }
                const char *end = supports_delim(p + 1, f->e, ")");
                if (end == f->e) { result = bad; goto done; }
                f->next = end + 1;
                bool yes = false;
                if (!strcmp(name, "selector")) {
                    struct pctx pc = {.a = a, .supports_probe = true};
                    yes = supports_delim(p + 1, end, ",") == end && parse_complex(&pc, p + 1, end) != NULL;
                }
                result = (struct supports_result){true, yes}; goto done;
            }
            const char *end = supports_delim(p + 1, f->e, ")");
            if (end == f->e) { result = bad; goto done; }
            f->next = end + 1; f->s = skip_ws(p + 1, end); f->e = end; trim_r(f->s, &f->e);
            p = f->s;
            if (p == f->e) { result = bad; goto done; }
            const char *colon = supports_delim(p, f->e, ":");
            if (colon < f->e) {
                const char *ne = colon; trim_r(p, &ne);
                const char *name = read_ident(a, &p, ne, false);
                if (!name || p != ne) { result = bad; goto done; }
                const char *v = skip_ws(colon + 1, f->e), *ve = f->e;
                if (supports_delim(v, ve, ";") != ve) { result = bad; goto done; }
                const char *bang = supports_delim(v, ve, "!");
                if (bang < ve) {
                    const char *q = skip_ws(bang + 1, ve), *important = read_ident(a, &q, ve, true);
                    if (!important || strcmp(important, "important") || skip_ws(q, ve) != ve) { result = bad; goto done; }
                    ve = bang;
                }
                result = (struct supports_result){true, supports_decl(a, name, strlen(name), v, (size_t)(ve-v))}; goto done;
            }
            f->stage = 1;
            struct supports_walk *child = ar_alloc(a, sizeof *child);
            child->parent = f; child->s = f->s; child->e = f->e; f = child; continue;
        }
        if (f->term) {
            if (!result.valid) {
                const char *p = f->s, *name = read_ident(a, &p, f->e, true);
                if (*f->s != '(' && !(name && (!strcmp(name,"not") || !strcmp(name,"and") || !strcmp(name,"or"))))
                    result = (struct supports_result){true, false};
            }
            goto done;
        }
        if (!result.valid) goto done; /* Validate RHS even when boolean short-circuits. */
        if (f->stage == 1) f->value = result;
        else f->value.value = f->op == 1 ? f->value.value && result.value : f->value.value || result.value;
        f->p = skip_ws(f->p, f->e);
        if (f->negate) {
            result = f->p == f->e ? (struct supports_result){true, !f->value.value} : bad; goto done;
        }
        if (f->p == f->e) { result = f->value; goto done; }
        const char *word = read_ident(a, &f->p, f->e, true);
        unsigned op = word && !strcmp(word,"and") ? 1 : word && !strcmp(word,"or") ? 2 : 0;
        if (!op || (f->op && f->op != op) || f->p == f->e || !is_space((unsigned char)*f->p)) { result = bad; goto done; }
        f->op = op; f->stage = 2;
term:;
        struct supports_walk *child = ar_alloc(a, sizeof *child);
        child->parent = f; child->term = true; child->s = f->p; child->e = f->e; f = child;
        continue;
done:;
        struct supports_walk *parent = f->parent;
        if (parent && f->term) parent->p = f->next;
        f = parent;
    }
    return result;
}

bool css_supports_condition(const char *condition, size_t n, bool implied_parens) {
    if (!condition || n > SIZE_MAX - 3) return false;
    arena_t *a = calloc(1, sizeof *a);
    if (!a) return false;
    jmp_buf trap;
    a->trap = &trap;
    if (setjmp(trap)) { ar_free(a); free(a); return false; }
    size_t len;
    char *s = supports_clean(a, condition, n, &len);
    bool result = false;
    if (s) {
        struct supports_result r = supports_expr(a, s, s + len);
        result = r.valid && r.value;
        if (!result && implied_parens) {
            char *wrapped = ar_alloc(a, len + 3);
            wrapped[0] = '(';
            memcpy(wrapped + 1, s, len);
            wrapped[len + 1] = ')'; wrapped[len + 2] = 0;
            r = supports_expr(a, wrapped, wrapped + len + 2);
            result = r.valid && r.value;
        }
    }
    ar_free(a);
    free(a);
    return result;
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
            size_t rn = (size_t)(z - a);
            char *rel = ar_strndup(pc->a, a, rn), *absolute = NULL;
            int result = rn && !strchr(rel, '"') ? web_resolve_url_owned(pc->base, rel, &absolute) : 0;
            if (result < 0) { sb_free(&b); ar_alloc(pc->a, SIZE_MAX); }
            if (result == 1) {
                sb_puts(&b, "url(\"");
                sb_puts(&b, absolute);
                sb_puts(&b, "\")");
            } else sb_put(&b, p, (size_t)(e + 1 - p));
            free(absolute);
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
    const char *name = s;
    size_t namelen = (size_t)(ne - s);
    if (memchr(s, '\\', namelen)) {
        const char *p = s;
        name = read_ident(pc->a, &p, ne, false);
        if (!name || p != ne) return NULL;
        namelen = strlen(name);
    }
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
    size_t vn = (size_t)(ve - v);
    if(!container_preserve_names(name,namelen,v,vn))v = css_value_canonical(pc->a, v, vn, &vn);
    ve = v + vn;
    if (namelen > 2 && name[0] == '-' && name[1] == '-') {
        d->name = ar_strndup(pc->a, name, namelen);
        d->value = ar_strndup(pc->a, v, vn);
        return d;
    }
    d->p = css_prop_lookup(name, namelen);
    if (!d->p || ve <= v) return NULL;
    d->value = absolute_urls(pc, v, ve);
    return d;
}

static void parse_rules(struct pctx *pc, const char *s, const char *e, const struct mcond *media);
static void parse_style_rule(struct pctx *pc, const char *ps, const char *pe, const char *bs, const char *be,
                             const struct mcond *media, const char *parent);

static const struct mcond *parse_condition(struct pctx *pc,const char *qs,const char *qe,
                                           const struct mcond *up,bool container) {
    struct mcond *mc=ar_alloc(pc->a,sizeof *mc);
    mc->up=up;mc->container=container;mc->axes=1;
    if(container && qs<qe && *qs!='(') {
        const char *p=container_ident_end(qs,qe),*rest=skip_ws(p,qe);
        /* not/style()/unknown functions belong to the condition, not a name. */
        if(p>qs && rest>p && container_ident_valid(qs,p)) {
            mc->name=ar_strndup(pc->a,qs,(size_t)(p-qs));qs=rest;
        }
    }
    char *q=ar_strndup(pc->a,qs,(size_t)(qe-qs));mc->q=q;
    if(container) {
        for(char *p=q;*p;p++)*p=(char)lower((unsigned char)*p);
        const char *s=q,*e=q+strlen(q);
        while(s<e) {
            if(!ident_char((unsigned char)*s)){s++;continue;}
            const char *p=s;while(s<e && ident_char((unsigned char)*s))s++;
            size_t n=(size_t)(s-p);
            if(n>=4 && (!memcmp(p,"min-",4) || !memcmp(p,"max-",4)))p+=4,n-=4;
            if(strn_ieq(p,"height",n) || strn_ieq(p,"block-size",n) ||
               strn_ieq(p,"aspect-ratio",n) || strn_ieq(p,"orientation",n))mc->axes=2;
        }
    }
    return mc;
}

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
                    if (!strcmp(name,"media") || !strcmp(name,"container"))
                        m=parse_condition(pc,qs,qe,media,!strcmp(name,"container"));
                    if (strcmp(name, "supports") || css_supports_condition(qs, (size_t)(p - qs), false)) {
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
    r->selector_text = sel_text;
    r->media = media;
    /* append before parsing the body, so nested rules come after their parent */
    if (pc->sh->last) pc->sh->last->next = r;
    else pc->sh->first = r;
    pc->sh->last = r;
    parse_body(pc, bs, be, sel_text, media, &r->decls, &r->ndecls);
}
static struct css_rule_info *cssom_record(struct pctx *pc, uint32_t type, const char *s, const char *e) {
    /* New grouping rules have no legacy CSSRule numeric constant (type 0).
       They still need source metadata: replaceSync and CSSOM edits rebuild
       the native AST from this list, not from its already-flattened rules. */
    if (pc->rule_depth != 1) return NULL;
    if (pc->sh->cssom_count == UINT32_MAX) {
        if (pc->a->trap) longjmp(*pc->a->trap, 1);
        abort();
    }
    struct css_rule_info *info = ar_alloc(pc->a, sizeof *info);
    info->type = type; info->text = ar_strndup(pc->a, s, (size_t)(e - s));
    if (pc->sh->cssom_last) pc->sh->cssom_last->next = info;
    else pc->sh->cssom_first = info;
    pc->sh->cssom_last = info; pc->sh->cssom_count++; return info;
}

static struct font_source *font_sources(struct pctx *pc,const char *s,const char *e) {
    struct font_source *first=NULL,**tail=&first;
    while(s<e) {
        const char *end=scan_to(s,e,",");s=skip_ws(s,end);
        if(end-s>=4 && strn_ieq(s,"url(",4)) {
            const char *a=skip_ws(s+4,end),*z=scan_to(a,end,")");
            if(z<end) {
                const char *finish=z;trim_r(a,&finish);
                if(finish-a>=2 && (*a=='"' || *a=='\'') && finish[-1]==*a){a++;finish--;}
                sbuf decoded={0};css_unescape(&decoded,a,(size_t)(finish-a));
                const char *rel=sb_cstr(&decoded);char *absolute=NULL;
                int resolved=decoded.n?web_resolve_url_owned(pc->base,rel,&absolute):0;
                if(resolved<0){sb_free(&decoded);ar_alloc(pc->a,SIZE_MAX);}
                if(resolved==1) {
                    struct font_source *src=ar_alloc(pc->a,sizeof *src);
                    src->url=ar_strdup(pc->a,absolute);src->next=NULL;*tail=src;tail=&src->next;
                }
                free(absolute);sb_free(&decoded);
            }
        }
        /* local() is not falsely resolved to a class-substitute font. Unknown
           binary formats are decoded only by the actual native resource gate. */
        s=end<e?end+1:e;
    }
    return first;
}
static void parse_font_face(struct pctx *pc,const char *s,const char *e,const struct mcond *media) {
    struct font_face *f=ar_alloc(pc->a,sizeof *f);memset(f,0,sizeof *f);
    f->weight_lo=f->weight_hi=400;f->media=media;
    while(s<e) {
        const char *end=scan_to(s,e,";"),*colon=scan_to(s,end,":"),*name_end=colon;
        const char *name=skip_ws(s,name_end);trim_r(name,&name_end);
        if(colon<end) {
            const char *value=skip_ws(colon+1,end),*ve=end;trim_r(value,&ve);
            size_t n=(size_t)(name_end-name);
            if(n==11 && strn_ieq(name,"font-family",n)) {
                const char *cursor=value,*family=css_font_family_next(pc->a,&cursor,ve);
                if(family && cursor==ve)f->family=family;
            } else if(n==3 && strn_ieq(name,"src",n))f->sources=font_sources(pc,value,ve);
            else if(n==10 && strn_ieq(name,"font-style",n))f->italic=(ve-value>=6 && strn_ieq(value,"italic",6)) || (ve-value>=7 && strn_ieq(value,"oblique",7));
            else if(n==11 && strn_ieq(name,"font-weight",n)) {
                if(ve-value==6 && strn_ieq(value,"normal",6))f->weight_lo=f->weight_hi=400;
                else if(ve-value==4 && strn_ieq(value,"bold",4))f->weight_lo=f->weight_hi=700;
                else {
                    char *text=ar_strndup(pc->a,value,(size_t)(ve-value)),*tail;
                    unsigned long lo=strtoul(text,&tail,10),hi=lo;
                    if(tail!=text){while(is_space((unsigned char)*tail))tail++;if(*tail)hi=strtoul(tail,&tail,10);
                        while(is_space((unsigned char)*tail))tail++;
                        if(!*tail && lo>=1 && lo<=1000 && hi>=lo && hi<=1000){f->weight_lo=(unsigned)lo;f->weight_hi=(unsigned)hi;}}
                }
            }
        }
        s=end<e?end+1:e;
    }
    if(!f->family || !f->sources)return;
    if(pc->sh->fonts_last)pc->sh->fonts_last->next=f;else pc->sh->fonts=f;
    pc->sh->fonts_last=f;
}

static void parse_opacity_keyframes(struct pctx *pc,const char *ns,const char *ne,
    const char *s,const char *e,const struct mcond *media) {
    const char *cursor=ns,*name=read_ident(pc->a,&cursor,ne,false);
    if(!name || skip_ws(cursor,ne)!=ne || !strcasecmp(name,"none") || !strcasecmp(name,"initial") ||
        !strcasecmp(name,"inherit") || !strcasecmp(name,"unset") || !strcasecmp(name,"default") ||
        !strcasecmp(name,"revert") || !strcasecmp(name,"revert-layer"))return;
    struct opacity_keyframes *key=ar_alloc(pc->a,sizeof *key);key->name=name;key->media=media;
    while((s=skip_ws(s,e))<e) {
        const char *p=scan_to(s,e,"{;}");if(p==e)break;
        if(*p!='{'){s=p+1;continue;}
        const char *end=block_end(p+1,e);
        struct decl *decls=NULL;int count=0;parse_body(pc,p+1,end,NULL,NULL,&decls,&count);
        /* One frame block may target several percentages. Invalid selector
           lists discard that block; important declarations are ignored later. */
        float offsets[64];int no=0;const char *q=s;bool valid=true;
        while(q<p) {
            const char *comma=scan_to(q,p,",");const char *a=skip_ws(q,comma),*z=comma;trim_r(a,&z);
            float off=-1;
            if(z-a==4 && strn_ieq(a,"from",4))off=0;
            else if(z-a==2 && strn_ieq(a,"to",2))off=1;
            else if(z>a && z[-1]=='%') {
                char *text=ar_strndup(pc->a,a,(size_t)(z-a-1)),*tail;double v=strtod(text,&tail);
                if(tail!=text && !*tail && isfinite(v) && v>=0 && v<=100)off=(float)(v/100);
            }
            if(off<0 || no==64){valid=false;break;}offsets[no++]=off;
            q=comma<p?comma+1:p;
            if(comma<p && q==p){valid=false;break;}
        }
        if(valid)for(int i=0;i<no;i++) {
            /* Duplicate offsets are one keyframe cascade, including timing
               declarations in a later block which omits opacity. */
            struct opacity_frame *same=NULL;
            for(struct opacity_frame *f=key->first;f;f=f->next)if(f->offset==offsets[i]){same=f;break;}
            if(same) {
                struct decl *joined=ar_alloc(pc->a,sizeof *joined*(size_t)(same->count+count));
                memcpy(joined,same->decls,sizeof *joined*(size_t)same->count);
                memcpy(joined+same->count,decls,sizeof *joined*(size_t)count);
                same->decls=joined;same->count+=count;continue;
            }
            struct opacity_frame *f=ar_alloc(pc->a,sizeof *f);f->offset=offsets[i];f->decls=decls;f->count=count;
            if(key->last)key->last->next=f;else key->first=f;key->last=f;
        }
        s=end<e?end+1:e;
    }
    if(pc->sh->motions_last)pc->sh->motions_last->next=key;else pc->sh->motions=key;
    pc->sh->motions_last=key;
}

static void parse_rules(struct pctx *pc, const char *s, const char *e, const struct mcond *media) {
    pc->rule_depth++;
    while (s < e) {
        s = skip_ws(s, e);
        if (s >= e) break;
        if (*s == '@') {
            const char *ns = s + 1;
            const char *name = read_ident(pc->a, &ns, e, true);
            const char *p = scan_to(ns, e, "{;");
            const char *qs = skip_ws(ns, p), *qe = p;
            trim_r(qs, &qe);
            uint32_t type = !name ? 0 : !strcmp(name,"import") ? 3 : !strcmp(name,"media") ? 4 :
                !strcmp(name,"font-face") ? 5 : !strcmp(name,"page") ? 6 :
                (!strcmp(name,"keyframes") || !strcmp(name,"-webkit-keyframes")) ? 7 :
                !strcmp(name,"namespace") ? 10 : !strcmp(name,"supports") ? 12 : 0;
            bool known_group = name && (!strcmp(name,"container") || !strcmp(name,"layer") ||
                !strcmp(name,"scope") || !strcmp(name,"document") || !strcmp(name,"-moz-document"));
            if (p >= e || *p == ';') {
                struct css_rule_info *info = p < e && (type || known_group) ? cssom_record(pc, type, s, p + 1) : NULL;
                if (name && !strcmp(name, "import")) {
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
                        if (info) info->import_url = ar_strdup(pc->a, abs);
                        if (pc->imports) {
                            struct css_import *im = malloc(sizeof *im);
                            im->url = strdup(abs);
                            im->order = pc->sh->order - 0.1 + 0.001 * pc->nimports++;
                            pv_push(pc->imports, im);
                        }
                    }
                    sb_free(&u);
                }
                s = p + 1;
                continue;
            }
            const char *be = block_end(p + 1, e);
            if(type || known_group)cssom_record(pc, type, s, be < e ? be + 1 : e);
            if (name) {
                if (!strcmp(name, "font-face")) parse_font_face(pc,p+1,be,media);
                else if (!strcmp(name,"keyframes") || !strcmp(name,"-webkit-keyframes"))
                    parse_opacity_keyframes(pc,qs,qe,p+1,be,media);
                else if (!strcmp(name,"media") || !strcmp(name,"container")) {
                    const struct mcond *mc=parse_condition(pc,qs,qe,media,!strcmp(name,"container"));
                    parse_rules(pc,p+1,be,mc);
                } else if (!strcmp(name, "supports")) {
                    if (css_supports_condition(qs, (size_t)(p - qs), false)) parse_rules(pc, p + 1, be, media);
                } else if (!strcmp(name, "layer") || !strcmp(name, "scope") ||
                           !strcmp(name, "document") || !strcmp(name, "-moz-document")) {
                    parse_rules(pc, p + 1, be, media);
                }
                /* @page, @property, @starting-style ... remain unsupported */
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
        struct rule *before = pc->sh->last;
        parse_style_rule(pc, s, p, p + 1, be, media, NULL);
        struct rule *native = before ? before->next : pc->sh->first;
        if (native) {
            struct css_rule_info *info = cssom_record(pc, 1, s, be < e ? be + 1 : e);
            if (info) { info->native_rule = native; info->selector = native->selector_text; }
        }
        s = be < e ? be + 1 : e;
    }
    pc->rule_depth--;
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

void css_sheet_scope(sheet_t *sheet, node_t *shadow_root) { if (sheet) sheet->scope = shadow_root; }
void css_sheet_media(sheet_t *sheet, const char *media) { if (sheet) sheet->owner_media = media; }
struct css_rule_info *css_sheet_rules(sheet_t *sheet, uint32_t *length) {
    if (length) *length = sheet ? sheet->cssom_count : 0;
    return sheet ? sheet->cssom_first : NULL;
}
bool css_sheet_single_style(sheet_t *sheet, const char *source, size_t length) {
    (void)source; (void)length;
    uint32_t count = 0; struct css_rule_info *info = css_sheet_rules(sheet, &count);
    if (count != 1 || !info || info->type != 1 || !info->native_rule) return false;
    /* Native parser recovery may accept a prefix/missing close. insertRule
       requires one entire qualified rule, not forgiving stylesheet recovery. */
    size_t n = sheet->cssom_length; const char *clean = sheet->cssom_source;
    const char *s = skip_ws(clean, clean + n), *p = scan_to(s, clean + n, "{;}");
    bool ok = p < clean + n && *p == '{';
    if (ok) { const char *end = block_end(p + 1, clean + n); ok = end < clean + n && skip_ws(end + 1, clean + n) == clean + n; }
    return ok;
}

/* Rules/selectors/declarations are immutable after parsing. Each adoption in
   the native cascade still needs its OWN scope and source-order identity. */
sheet_t *css_sheet_instance(arena_t *a, const sheet_t *source, double order, node_t *scope) {
    sheet_t *sheet = ar_alloc(a, sizeof *sheet);
    *sheet = *source;
    sheet->order = order; sheet->scope = scope;
    return sheet;
}

sheet_t *css_parse_sheet(arena_t *a, const char *css, size_t n, const char *base_url, double order, pvec *imports) {
    struct pctx pc = {a, NULL, base_url, imports, 0, false, 0};
    pc.sh = ar_alloc(a, sizeof(sheet_t));
    pc.sh->order = order;
    size_t cn;
    char *c = strip_comments(a, css, n, &cn);
    pc.sh->cssom_source = c; pc.sh->cssom_length = cn;
    parse_rules(&pc, c, c + cn, NULL);
    return pc.sh;
}

struct sheet_parse_job {
    arena_t *arena;
    const char *text, *base;
    size_t length;
    double order;
    pvec *imports;
    sheet_t *result;
    int failed;
};
static void sheet_parse_worker(size_t index, void *context) {
    (void)index;
    struct sheet_parse_job *job = context;
    parallel_record_work(PARALLEL_CSS_PARSE, job->length);
    jmp_buf trap; jmp_buf *outer = job->arena->trap;
    job->arena->trap = &trap;
    job->failed = setjmp(trap);
    if (!job->failed) job->result = css_parse_sheet(job->arena, job->text, job->length, job->base, job->order, job->imports);
    job->arena->trap = outer;
}
static void sheet_parse_service(void *context) { web_native_checkpoint(context); }
sheet_t *css_parse_sheet_serviced(web_doc *d, arena_t *a, const char *css, size_t n, const char *base_url, double order, pvec *imports) {
    if (!d || !d->live || n < 16384 || parallel_active())
        return css_parse_sheet(a,css,n,base_url,order,imports);
    struct sheet_parse_job job = {a,css,base_url,n,order,imports,NULL,0};
    parallel_call_stage(PARALLEL_CSS_PARSE,sheet_parse_worker,&job,sheet_parse_service,d);
    if (job.failed) {
        if (a->trap) longjmp(*a->trap,job.failed);
        ar_alloc(a,SIZE_MAX); return NULL;
    }
    return job.result;
}

/* ---------------------------------------------------------------- media queries */
/* CSS and matchMedia share this explicit-stack parser and device model. Unknown
   features use Kleene logic: negating an unsupported feature is not a match. */
enum { MQ_UNKNOWN = -1, MQ_FALSE, MQ_TRUE };
enum { MQ_LENGTH = 1, MQ_RATIO, MQ_RESOLUTION, MQ_INTEGER, MQ_NUMBER };
struct mq {
    const char *s,*e; float vw,vh; bool scripting,valid;
    bool container; float em,rem,viewport_width,viewport_height;
};
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
    size_t n=(size_t)(p-s); if(n==SIZE_MAX)return false;char *b=malloc(n+1);if(!b)return false;memcpy(b,s,n);b[n]=0;
    double v=strtod(b,NULL);free(b);if(!isfinite(v))return false;
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
        else if(strn_ieq(p,"em",n))scale=m->container?m->em:16;
        else if(strn_ieq(p,"rem",n))scale=m->container?m->rem:16;
        else if(strn_ieq(p,"pt",n))scale=96.0/72;
        else if(strn_ieq(p,"pc",n))scale=16;
        else if(strn_ieq(p,"in",n))scale=96;
        else if(strn_ieq(p,"cm",n))scale=96/2.54;
        else if(strn_ieq(p,"mm",n))scale=96/25.4;
        else if(strn_ieq(p,"q",n))scale=96/101.6;
        else if(strn_ieq(p,"vw",n))scale=(m->container?m->viewport_width:m->vw)/100.0;
        else if(strn_ieq(p,"vh",n))scale=(m->container?m->viewport_height:m->vh)/100.0;
        else if(strn_ieq(p,"vmin",n))scale=m->container?fminf(m->viewport_width,m->viewport_height)/100.0:fminf(m->vw,m->vh)/100.0;
        else if(strn_ieq(p,"vmax",n))scale=m->container?fmaxf(m->viewport_width,m->viewport_height)/100.0:fmaxf(m->vw,m->vh)/100.0;
        else return false;
    }
    *out=v*scale;return true;
}
static int mq_numeric(const char *s,size_t n,const struct mq *m,double *v) {
    /* Screen dimensions are not supplied by the embedder. Deprecated device-*
       features stay unknown rather than falsely equating screen and window. */
    if(strn_ieq(s,"width",n) || (m->container && strn_ieq(s,"inline-size",n))){*v=m->vw;return MQ_LENGTH;}
    if(strn_ieq(s,"height",n) || (m->container && strn_ieq(s,"block-size",n))){*v=m->vh;return MQ_LENGTH;}
    if(strn_ieq(s,"aspect-ratio",n)){*v=m->vh?(double)m->vw/m->vh:0;return MQ_RATIO;}
    if(m->container)return 0;
    if(strn_ieq(s,"resolution",n)){*v=1;return MQ_RESOLUTION;}
    if(strn_ieq(s,"-webkit-device-pixel-ratio",n)||strn_ieq(s,"device-pixel-ratio",n)){*v=1;return MQ_NUMBER;}
    if(strn_ieq(s,"color",n)){*v=8;return MQ_INTEGER;}
    if(strn_ieq(s,"monochrome",n)||strn_ieq(s,"color-index",n)){*v=0;return MQ_INTEGER;}
    return 0;
}
static const char *mq_discrete(const char *s,size_t n,const struct mq *m,const char **allowed) {
#define FEATURE(name,value,choices) if(strn_ieq(s,name,n)){*allowed=choices;return value;}
    FEATURE("orientation",m->vw>m->vh?"landscape":"portrait","portrait landscape")
    if(m->container)return NULL;
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
struct mq_walk {
    struct mq_walk *parent;
    struct mq m;
    int value, mode;
    unsigned stage;
    bool allow_or, negate;
};
static int mq_cond(struct mq *m,bool allow_or) {
    struct mq_walk *f=calloc(1,sizeof*f);
    if(!f){m->valid=false;return MQ_UNKNOWN;}
    f->m=*m;f->allow_or=allow_or;int result=MQ_UNKNOWN;
    while(f){
        if(!f->stage){f->negate=mq_word(&f->m,"not");f->stage=1;goto term;}
        goto term_done;
term:;
        struct mq *q=&f->m;
        q->s=skip_ws(q->s,q->e);
        if(q->s==q->e){q->valid=false;result=MQ_UNKNOWN;goto term_done;}
        bool function=*q->s!='(';const char *open=q->s;
        if(function)while(open<q->e&&ident_char((unsigned char)*open))open++;
        if(open==q->e||*open!='('){q->valid=false;result=MQ_UNKNOWN;goto term_done;}
        size_t level=1;const char *close=open+1;
        for(;close<q->e;close++){if(*close=='(')level++;else if(*close==')'&&!--level)break;}
        if(close==q->e){q->valid=false;result=MQ_UNKNOWN;goto term_done;}
        q->s=close+1;
        if(function){result=MQ_UNKNOWN;goto term_done;}
        const char *in=skip_ws(open+1,close);
        if(in<close&&(*in=='('||((size_t)(close-in)>=3&&strn_ieq(in,"not",3)&&
           (in+3==close||!ident_char((unsigned char)in[3]))))){
            struct mq_walk *child=calloc(1,sizeof*child);
            if(!child){m->valid=false;while(f){struct mq_walk *p=f->parent;free(f);f=p;}return MQ_UNKNOWN;}
            child->parent=f;child->m=*q;child->m.s=in;child->m.e=close;child->m.valid=true;child->allow_or=true;
            f=child;continue;
        }
        result=mq_feature(in,close,q);
term_done:
        if(f->stage==1)f->value=result;
        else f->value=f->mode==1?mq_and(f->value,result):mq_or(f->value,result);
        if(f->negate){result=mq_not(f->value);goto done;}
        int next=mq_word(&f->m,"and")?1:mq_word(&f->m,"or")?2:0;
        if(!next){result=f->value;goto done;}
        if((f->mode&&f->mode!=next)||(next==2&&!f->allow_or)){f->m.valid=false;result=MQ_UNKNOWN;goto done;}
        f->mode=next;f->stage=2;goto term;
done:;
        struct mq_walk *parent=f->parent;
        if(parent){if(!f->m.valid||skip_ws(f->m.s,f->m.e)!=f->m.e)parent->m.valid=false;}
        else {m->s=f->m.s;m->valid=m->valid&&f->m.valid;}
        free(f);f=parent;
    }
    return result;
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
    size_t length=strlen(query);if(length==SIZE_MAX)return false;
    char *text=malloc(length+1);if(!text)return false;size_t n=0;
    for(size_t i=0;i<length;i++){
        if(query[i]=='/'&&i+1<length&&query[i+1]=='*'){
            i+=2;while(i+1<length&&!(query[i]=='*'&&query[i+1]=='/'))i++;if(i+1>=length)break;i++;text[n++]=' ';
        }else text[n++]=(char)lower((unsigned char)query[i]);
    }text[n]=0;
    const char *s=text,*e=text+n;size_t used=0;bool any=false,first=true,ok=true;
    if(skip_ws(s,e)==e){free(text);return true;}
    for(;;){
        const char *end=s;size_t depth=0;bool bad=false;
        while(end<e){char c=*end;if(c=='(')depth++;else if(c==')'){if(!depth)bad=true;else depth--;}
            if(!depth&&c==',')break;if((unsigned char)c<32&&!is_space(c))bad=true;
            if(c=='\\'||c=='\''||c=='"'||c=='{'||c=='}'||c==';'||c=='['||c==']')bad=true;end++;}
        struct mq m={.s=s,.e=end,.vw=vw,.vh=vh,.scripting=scripting,.valid=!bad&&!depth};int r=mq_query(&m);
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
    for(;m;m=m->up)if(!m->container && !css_media_evaluate(m->q,vw,vh,scripting,NULL,0))return false;return true;
}
static bool font_conditions_ok(const struct mcond *m,int vw,int vh,bool scripting) {
    /* Global font resources have no element against which a size condition
       can be tested. Unsupported conditional non-style rules are not enabled. */
    for(const struct mcond *p=m;p;p=p->up)if(p->container)return false;
    return media_chain_ok(m,vw,vh,scripting);
}

static node_t *query_container(node_t *e,const char *name,unsigned axes,bool inclusive) {
    if(inclusive && e->container_valid && (axes==1 || e->container_type==CT_SIZE) &&
       container_names_match(e->container_names,name))return e;
    if(!name)return axes==1?e->query_inline_container:e->query_size_container;
    for(node_t *n=doc_flat_parent(e);n;n=doc_flat_parent(n))
        if(n->container_valid && (axes==1 || n->container_type==CT_SIZE) &&
           container_names_match(n->container_names,name))return n;
    return NULL;
}
static bool container_chain_ok(const struct mcond *mc,node_t *e,bool inclusive) {
    for(;mc;mc=mc->up) {
        if(!mc->container)continue;
        node_t *n=query_container(e,mc->name,mc->axes,inclusive);
        if(!n)return false; /* unavailable/unsupported is unknown, even under not */
        struct mq m={.s=mc->q,.e=mc->q+strlen(mc->q),.vw=n->container_width,.vh=n->container_height,
            .valid=true,.container=true,.em=n->container_em,.rem=e->owner->container_rem,
            .viewport_width=e->owner->width,.viewport_height=e->owner->height};
        int value=mq_cond(&m,true);
        if(!m.valid || skip_ws(m.s,m.e)!=m.e || value!=MQ_TRUE)return false;
    }
    return true;
}

struct font_binding {
    const char *names;node_t *scope;unsigned weight;bool italic;font_t *face;
};
static struct font_binding *font_bindings;
static size_t font_binding_count,font_binding_capacity;
static bool font_faces_available;
static void font_bindings_clear(void) {
    free(font_bindings);font_bindings=NULL;font_binding_count=font_binding_capacity=0;
}
static struct font_face *scoped_font_face(web_doc *d, node_t *scope, const char *family, unsigned weight, bool italic, int vw, int vh);
static font_t *select_web_font_uncached(web_doc *d,node_t *node,style_t *style,int vw,int vh,arena_t *a) {
    if(!style->font_names)return NULL;
    const char *cursor=style->font_names,*end=cursor+strlen(cursor);
    node_t *scope=doc_node_root(node,false);
    while(cursor<end) {
        const char *token=skip_ws(cursor,end);bool quoted=token<end && (*token=='"' || *token=='\'');
        const char *family=css_font_family_next(a,&cursor,end);if(!family || !cursor)return NULL;
        /* Generic keywords terminate named lookup in the authored fallback
           order. A quoted name with that spelling is still a named family. */
        if(!quoted && (!strcasecmp(family,"serif") || !strcasecmp(family,"sans-serif") || !strcasecmp(family,"monospace") ||
            !strcasecmp(family,"system-ui") || !strcasecmp(family,"cursive") || !strcasecmp(family,"fantasy")))return NULL;
        struct font_face *best=scoped_font_face(d,scope,family,style->font_weight,style->font_style!=0,vw,vh);
        if(!best)continue;
        for(struct font_source *src=best->sources;src;src=src->next) {
            struct web_font_resource *r=NULL;
            if(css_worker()) { for(r=d->fonts;r && strcmp(r->url,src->url);r=r->next) {} }
            else r=doc_font_resource(d,src->url);
            if(!r){ar_alloc(a,SIZE_MAX);return NULL;}
            if(r->failed)continue;
            __atomic_store_n(&r->wanted,true,__ATOMIC_RELAXED);return r->face;
        }
    }
    return NULL;
}
static font_t *select_web_font(web_doc *d,node_t *node,style_t *style,int vw,int vh,arena_t *a) {
    if(!font_faces_available || !style->font_names)return NULL;
    if(css_worker())return select_web_font_uncached(d,node,style,vw,vh,a);
    node_t *scope=doc_node_root(node,false);unsigned weight=style->font_weight;bool italic=style->font_style!=0;
    for(size_t i=0;i<font_binding_count;i++) {
        const struct font_binding *b=&font_bindings[i];
        if(b->scope==scope && b->weight==weight && b->italic==italic &&
           (b->names==style->font_names || !strcmp(b->names,style->font_names)))return b->face;
    }
    font_t *face=select_web_font_uncached(d,node,style,vw,vh,a);
    if(font_binding_count==font_binding_capacity) {
        size_t cap=font_binding_capacity?font_binding_capacity:16;
        if(cap<=SIZE_MAX/2)cap*=2;
        if(cap<=SIZE_MAX/sizeof *font_bindings) {
            void *p=realloc(font_bindings,cap*sizeof *font_bindings);
            if(p){font_bindings=p;font_binding_capacity=cap;}
        }
    }
    /* Optional acceleration only: allocator failure never rejects a font. */
    if(font_binding_count<font_binding_capacity)
        font_bindings[font_binding_count++]=(struct font_binding){style->font_names,scope,weight,italic,face};
    return face;
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

struct css_index {
    struct htab ids, classes, tags;
    struct ient *univ;
    const struct rule *source;
    struct font_face *fonts;
    struct opacity_keyframes *motions;
    bool ua;
    uint32_t rules;
    unsigned form_features;
    bool containers;
    struct css_index *next;
};
struct css_binding {
    struct css_index *index;
    uint32_t order;
    struct css_binding *next;
};
struct css_scope {
    node_t *root;
    struct css_binding *first, *last;
    struct css_scope *next;
};
struct css_ctx {
    arena_t a;
    struct css_index *indexes[256];
    struct css_scope *scopes;
    struct css_binding *ua;
    bool relational, siblings, parallel_safe, containers;
    unsigned form_features;
};

/* Custom-property declarations can feed a container unit to an ordinary
   property through var(), so inspect all declaration values, not just lengths. */
static bool container_units_used(const char *s) {
    while(*s) {
        if(lower((unsigned char)s[0])=='c' && lower((unsigned char)s[1])=='q') {
            const char *p=s+2;while(ident_char((unsigned char)*p))p++;
            size_t n=(size_t)(p-s);
            if(strn_ieq(s,"cqw",n)||strn_ieq(s,"cqh",n)||strn_ieq(s,"cqi",n)||
               strn_ieq(s,"cqb",n)||strn_ieq(s,"cqmin",n)||strn_ieq(s,"cqmax",n))return true;
        }
        s++;
    }
    return false;
}

static node_t *container_first(node_t *n) {
    if(n->shadow_root)n=n->shadow_root;
    if(n->type==N_ELEM && !n->foreign && n->tag==T_slot && n->slot_assigned_first &&
       doc_node_root(n,false)->shadow_host)return n->slot_assigned_first;
    return n->first;
}
static node_t *container_next(node_t *n) {
    node_t *p=doc_flat_parent(n);
    if(p && p->type==N_ELEM && !p->foreign && p->tag==T_slot && p->slot_assigned_first &&
       doc_node_root(p,false)->shadow_host)return n->assigned_next;
    return n->next;
}

bool css_containers_update(web_doc *d) {
    struct css_ctx *x=d?d->sty.ctx:NULL;
    if(!x || !__atomic_load_n(&x->containers,__ATOMIC_ACQUIRE) || !d->html)return false;
    bool changed=false;float rem=d->html->style?d->html->style->font_size:16;
    if(d->container_rem!=rem){changed=true;d->container_rem=rem;}
    for(node_t *n=d->html;n;) {
        if(!web_native_checkpoint(d))return false;
        node_t *parent=doc_flat_parent(n);
        node_t *in=parent?(parent->container_valid?parent:parent->query_inline_container):NULL;
        node_t *block=parent?(parent->container_valid && parent->container_type==CT_SIZE?parent:parent->query_size_container):NULL;
        if(n->query_inline_container!=in || n->query_size_container!=block)changed=true;
        n->query_inline_container=in;n->query_size_container=block;
        const style_t *st=n->style;box_t *b=n->box;
        bool valid=st && st->container_type!=CT_NORMAL && b && !b->anon &&
            st->display!=D_INLINE && st->display!=D_INLINE_TABLE &&
            (b->kind==B_BLOCK || b->kind==B_FLEX || b->kind==B_GRID || b->kind==B_ATOMIC);
        unsigned type=valid?st->container_type:CT_NORMAL;
        float width=valid?b->w:0,height=valid && type==CT_SIZE?b->h:0,em=valid?st->font_size:0;
        const char *names=valid?st->container_names:NULL;
        bool names_equal=(!names && !n->container_names) || (names && n->container_names && !strcmp(names,n->container_names));
        if(!names_equal) {
            char *copy=names?strdup(names):NULL;
            if(names && !copy){__atomic_store_n(&d->native_cancelled,true,__ATOMIC_RELEASE);return false;}
            free(n->container_names);n->container_names=copy;changed=true;
        }
        if(n->container_valid!=valid || n->container_type!=type || n->container_width!=width ||
           n->container_height!=height || n->container_em!=em)changed=true;
        n->container_valid=valid;n->container_type=type;
        n->container_width=width;n->container_height=height;n->container_em=em;
        node_t *child=container_first(n);
        if(child){n=child;continue;}
        while(n && n!=d->html && !container_next(n))n=doc_flat_parent(n);
        n=n && n!=d->html?container_next(n):NULL;
    }
    return changed;
}

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

static void index_rule(arena_t *a, struct css_index *x, const struct rule *r, const struct selector *s, uint32_t order, bool ua) {
    if (s->pseudo == PE_OTHER) return;
    struct ient *ie = ar_alloc(a, sizeof *ie);
    ie->sel = s;
    ie->r = r;
    ie->order = order;
    ie->ua = ua;
    const struct compound *c = s->right;
    const char *id = NULL, *cls = NULL, *tag = NULL;
    for (int i = 0; s->pseudo != PE_SLOTTED && i < c->n; i++) {
        if (c->s[i].kind == SK_ID && !id) id = c->s[i].name;
        else if (c->s[i].kind == SK_CLASS && !cls) cls = c->s[i].name;
        else if (c->s[i].kind == SK_TAG) tag = c->s[i].name;
    }
    struct ient **slot = id ? ht_slot(a, &x->ids, id, true)
                       : cls ? ht_slot(a, &x->classes, cls, true)
                       : tag ? ht_slot(a, &x->tags, tag, true)
                             : &x->univ;
    ie->next = *slot;
    *slot = ie;
}

static sheet_t *ua_sheet, *quirks_sheet;
static arena_t ua_arena;

static unsigned selector_form_features(const struct compound *compound) {
    unsigned features = 0;
    for (const struct compound *c=compound;c;c=c->left)
        for(int i=0;i<c->n;i++) {
            const struct simple *s=&c->s[i];
            if(s->kind==SK_PC) {
                if(s->pc==PC_CHECKED)features |= WEB_STYLE_FORM_CHECKED;
                if(s->pc==PC_VALID || s->pc==PC_INVALID)features |= WEB_STYLE_FORM_VALIDITY;
                if(s->pc==PC_IN_RANGE || s->pc==PC_OUT_OF_RANGE)features |= WEB_STYLE_FORM_RANGE;
                if(s->pc==PC_PLACEHOLDER_SHOWN)features |= WEB_STYLE_FORM_PLACEHOLDER;
            }
            if(s->args)for(int k=0;k<s->args->n;k++)features |= selector_form_features(s->args->v[k]->right);
        }
    return features;
}
static bool selector_siblings(const struct compound *compound) {
    for(const struct compound *c=compound;c;c=c->left) {
        if(c->comb=='+' || c->comb=='~')return true;
        for(int i=0;i<c->n;i++)if(c->s[i].args)
            for(int k=0;k<c->s[i].args->n;k++)if(selector_siblings(c->s[i].args->v[k]->right))return true;
    }
    return false;
}

static struct css_ctx *build_index(web_doc *d, int vw, int vh) {
    struct css_ctx *x = calloc(1, sizeof *x);
    if (!x) { ar_alloc(&d->smem, SIZE_MAX); return NULL; }
    x->parallel_safe = true;
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
            if (!web_native_checkpoint(d)) return x;
            void *t = sh->v[j];
            sh->v[j] = sh->v[j - 1];
            sh->v[j - 1] = t;
        }
    uint32_t order = 1;
    for (int i = -2; i < sh->n; i++) {
        sheet_t *s = i == -2 ? ua_sheet : i == -1 ? quirks_sheet : sh->v[i];
        if (s->owner_media && !css_media_evaluate(s->owner_media, vw, vh, web_js_enabled(d), NULL, 0)) continue;
        if (i == -1 && !d->quirks) continue;
        unsigned bucket = ((uintptr_t)s->first >> 4) & 255;
        struct css_index *index = x->indexes[bucket];
        while (index && (index->source != s->first || index->fonts != s->fonts || index->motions != s->motions || index->ua != s->ua)) index = index->next;
        if (!index) {
            index = ar_alloc(&x->a, sizeof *index);
            index->source = s->first; index->fonts=s->fonts; index->motions=s->motions; index->ua = s->ua;
            index->next = x->indexes[bucket]; x->indexes[bucket] = index;
            /* Small component sheets no longer allocate a document-sized hash
               table, nor duplicate selector entries for every adoption. */
            ht_init(&x->a, &index->ids, 64);
            ht_init(&x->a, &index->classes, 128);
            ht_init(&x->a, &index->tags, 64);
            /* Registry identities are published before workers start. Worker
               font selection only reads this list and atomically marks demand. */
            for(struct font_face *font=s->fonts;font;font=font->next)
                for(struct font_source *source=font->sources;source;source=source->next)
                    if(!doc_font_resource(d,source->url))x->parallel_safe=false;
            for (struct rule *r = s->first; r; r = r->next, index->rules++) {
                if (!web_native_checkpoint(d)) return x;
                if (r->selector_text && strstr(r->selector_text, ":has(")) x->relational = true;
                for(const struct mcond *m=r->media;m;m=m->up)if(m->container)index->containers=true;
                for(int k=0;k<r->ndecls;k++)if(container_units_used(r->decls[k].value))index->containers=true;
                if (!r->ndecls || !media_chain_ok(r->media, vw, vh, web_js_enabled(d))) continue;
                for (int k = 0; k < r->sel->n; k++) {
                    index->form_features |= selector_form_features(r->sel->v[k]->right);
                    if(selector_siblings(r->sel->v[k]->right))x->siblings=true;
                    index_rule(&x->a, index, r, r->sel->v[k], index->rules, s->ua);
                    if(d->profile_enabled)d->profile.style_index_entries++;
                }
            }
        }
        x->form_features |= index->form_features;
        x->containers |= index->containers;
        struct css_binding *binding = ar_alloc(&x->a, sizeof *binding);
        binding->index = index; binding->order = order; order += index->rules;
        if (s->ua) { binding->next = x->ua; x->ua = binding; continue; }
        node_t *root = s->scope ? s->scope : d->root;
        struct css_scope *scope = root ? root->style_scope : NULL;
        if (!scope) {
            scope = ar_alloc(&x->a, sizeof *scope); scope->root = root;
            scope->next = x->scopes; x->scopes = scope;
            if (root) root->style_scope = scope;
        }
        if (scope->last) scope->last->next = binding; else scope->first = binding;
        scope->last = binding;
    }
    return x;
}

static struct font_face *scoped_font_face(web_doc *d,node_t *root,const char *family,unsigned weight,bool italic,int vw,int vh) {
    struct css_scope *scope=root?root->style_scope:NULL;
    struct font_face *best=NULL;unsigned score=UINT_MAX;
    bool scripting=d->sty.index_scripting;
    for(struct css_binding *binding=scope?scope->first:NULL;binding;binding=binding->next)
        for(struct font_face *f=binding->index->fonts;f;f=f->next) {
            if(strcasecmp(family,f->family)||!font_conditions_ok(f->media,vw,vh,scripting))continue;
            unsigned distance=weight<f->weight_lo?f->weight_lo-weight:weight>f->weight_hi?weight-f->weight_hi:0;
            unsigned candidate=distance+(italic!=f->italic?10000u:0u);
            if(candidate<=score){best=f;score=candidate;}
        }
    return best;
}

/* Use the same flat tree as the cascade: shadow children and assigned nodes,
   but not unassigned light children or inactive slot fallback content. Unlike
   owned_nodes this also visits nodes adopted from another allocation arena. */
static node_t *font_selection_next(node_t *n,node_t *root) {
    node_t *child=container_first(n);
    if(child)return child;
    while(n && n!=root && !container_next(n))n=doc_flat_parent(n);
    return n && n!=root?container_next(n):NULL;
}

void css_refresh_font_selection(web_doc *d) {
    if(!d || d->need_style)return; /* A queued cascade already retries sources. */
    if(!d->sty.ctx || d->sty.index_dirty || d->resources_dirty ||
       d->style_full_dirty || d->style_pending_dirty || !d->root ||
       d->sty.index_w!=d->styled_w || d->sty.index_h!=d->styled_h ||
       d->sty.index_scripting!=web_js_enabled(d) || d->sty.index_quirks!=d->quirks) {
        css_mark_dirty(d,NULL);
        return;
    }
    /* Keep mutable arena state on the heap: it remains defined after longjmp.
       Family tokens are scratch only; selected faces/resources are doc-owned. */
    arena_t *scratch=calloc(1,sizeof *scratch);
    if(!scratch){css_mark_dirty(d,NULL);return;}
    jmp_buf trap;scratch->trap=&trap;scratch->chunk_size=4096;
    if(setjmp(trap)) {
        ar_free(scratch);free(scratch);css_mark_dirty(d,NULL);return;
    }
    for(node_t *n=d->root;n;n=font_selection_next(n,d->root)) {
        if(!web_native_checkpoint(d))break;
        if(n->type!=N_ELEM || n->owner!=d || !n->style)continue;
        style_t *s=n->style,*base=n->animation_base_style;
        style_t *styles[]={s,s->before,s->after,s->backdrop,base,
            base?base->before:NULL,base?base->after:NULL,base?base->backdrop:NULL};
        bool changed=false;
        for(size_t i=0;i<sizeof styles/sizeof *styles;i++)if(styles[i]) {
            font_t *face=select_web_font_uncached(d,n,styles[i],d->styled_w,d->styled_h,scratch);
            if(face!=styles[i]->named_font)changed=true;
        }
        /* NULL -> NULL still marks the next source wanted, without recascading
           unchanged SVG snapshots or reshaping existing inline formatting. */
        if(changed) {
            css_mark_dirty(d,n);
            /* A partial cascade resets wanted flags, including font users
               outside its dirty subtree. Retry their sources after publication. */
            d->font_selection_dirty=true;
        }
        ar_free(scratch);
    }
    ar_free(scratch);free(scratch);
}

static void css_ctx_free(struct css_ctx *x) {
    if (!x) return;
    for (struct css_scope *s = x->scopes; s; s = s->next)
        if (s->root && s->root->style_scope == s) s->root->style_scope = NULL;
    ar_free(&x->a); free(x);
}

void css_styling_free(struct styling *st) {
    if (st->ctx) {
        css_ctx_free(st->ctx);
        st->ctx = NULL;
    }
    pv_free(&st->sheets);
}

/* ---------------------------------------------------------------- presentational hints */
struct hints {
    struct decl *d;
    int n, cap;
    arena_t *a;
};

static void hint_value(struct hints *h, const char *prop, const char *value) {
    const struct propdef *p = css_prop_lookup(prop, strlen(prop));
    if (!p) return;
    if (h->n == h->cap) {
        if (h->cap == INT_MAX) ar_alloc(h->a, SIZE_MAX);
        int cap = h->cap ? h->cap > INT_MAX / 2 ? INT_MAX : h->cap * 2 : 16;
        if ((size_t)cap > SIZE_MAX / sizeof(struct decl)) ar_alloc(h->a, SIZE_MAX);
        struct decl *next = ar_alloc(h->a, (size_t)cap * sizeof *next);
        if (h->n) memcpy(next, h->d, (size_t)h->n * sizeof *next);
        h->d = next; h->cap = cap;
    }
    struct decl *d = &h->d[h->n++];
    memset(d, 0, sizeof *d);
    d->p = p;
    d->value = ar_strdup(h->a, value);
}

static void hint(struct hints *h, const char *prop, const char *fmt, ...) {
    va_list ap, count;
    va_start(ap, fmt); va_copy(count, ap);
    int n = vsnprintf(NULL, 0, fmt, count);
    va_end(count);
    if (n < 0) { va_end(ap); return; }
    char *value = ar_alloc(h->a, (size_t)n + 1);
    vsnprintf(value, (size_t)n + 1, fmt, ap); va_end(ap);
    hint_value(h, prop, value);
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
struct css_animation {
    struct css_animation *next;
    uint32_t id;
    uint8_t pseudo;
    char *text;
};
/* Playback owns only a name/time/identity, never an AST or computed-style
   pointer. The supervisor ticks after joins; workers publish disjoint nodes
   onto the document list with CAS and read one supervisor clock snapshot. */
struct css_motion_state {
    struct css_motion_state *next;
    web_doc *document;
    node_t *node;
    char *name;
    uint64_t last;
    double elapsed;
    bool paused, running;
};
static void css_motion_reset(node_t *n) {
    if(n && n->css_motion){n->css_motion->node=NULL;n->css_motion->running=false;n->css_motion=NULL;}
}
void css_motion_doc_free(web_doc *d) {
    while(d->css_motions) {
        struct css_motion_state *s=d->css_motions;d->css_motions=s->next;
        if(s->node && s->node->css_motion==s)s->node->css_motion=NULL;
        free(s->name);free(s);
    }
}
void css_motion_tick(web_doc *d,uint64_t now) {
    if(!d || __atomic_load_n(&d->native_cancelled,__ATOMIC_ACQUIRE))return;
    struct css_motion_state **slot=&d->css_motions;
    while(*slot) {
        struct css_motion_state *s=*slot;
        if(!s->node){*slot=s->next;free(s->name);free(s);continue;}
        slot=&s->next;
        if(!s->node || !s->running || now<s->last || now-s->last<16)continue;
        if(doc_node_root(s->node,true)!=d->root){css_motion_reset(s->node);continue;}
        css_mark_dirty(d,s->node);d->need_style=d->dirty=true;
    }
}
int64_t css_motion_deadline(web_doc *d,uint64_t now) {
    if(!d || __atomic_load_n(&d->native_cancelled,__ATOMIC_ACQUIRE))return -1;
    uint64_t deadline=UINT64_MAX;
    for(struct css_motion_state *s=d->css_motions;s;s=s->next)if(s->node && s->running) {
        uint64_t due=s->last>UINT64_MAX-16?UINT64_MAX:s->last+16;
        if(due<deadline)deadline=due;
    }
    return deadline==UINT64_MAX?-1:deadline<=now?0:(int64_t)(deadline-now);
}
int css_animation_set(node_t *n, uint8_t pseudo, uint32_t id, const char *text) {
    struct css_animation **slot = &n->animations;
    while (*slot && ((*slot)->id != id || (*slot)->pseudo != pseudo)) {
        slot = &(*slot)->next;
    }
    struct css_animation *old = *slot;
    if (!text || !*text) {
        if (!old) return 0;
        *slot = old->next; free(old->text); free(old); return 1;
    }
    if (old && !strcmp(old->text, text)) return 0;
    char *copy = strdup(text);
    if (!copy) return -1;
    if (old) { free(old->text); old->text = copy; return 1; }
    struct css_animation *a = malloc(sizeof *a);
    if (!a) { free(copy); return -1; }
    *a = (struct css_animation){NULL, id, pseudo, copy};
    *slot = a; return 1;
}
void css_animation_free(node_t *n) {
    css_motion_reset(n);
    while (n->animations) {
        struct css_animation *a = n->animations;
        n->animations = a->next;
        free(a->text); free(a);
    }
}
struct dent {
    const struct decl *d;
    uint64_t key;
    uint8_t pseudo;
    int scope_depth;
    bool animation;
};


static int scope_depth(node_t *root) {
    int depth = 0;
    for (; root && root->shadow_host; root = doc_node_root(root->shadow_host, false)) depth++;
    return depth;
}

static void add_dent(const struct decl *d, uint64_t key, uint8_t pseudo, int depth) {
    if (ndents == capdents) {
        if(capdents>INT_MAX/2)ar_alloc(css_scratch_get()->arena,SIZE_MAX);
        int capacity = capdents ? capdents * 2 : 256;
        struct dent *next = realloc(dents, sizeof *dents * (size_t)capacity);
        if(!next)ar_alloc(css_scratch_get()->arena,SIZE_MAX);
        dents=next;capdents=capacity;
    }
    dents[ndents].d = d;
    /* Important origins reverse the normal order: UA rules such as scripting
       noscript suppression outrank even an author's important inline style. */
    dents[ndents].key = d->important ? (key ^ (1ull << 62)) | (1ull << 63) : key;
    dents[ndents].pseudo = pseudo;
    dents[ndents].scope_depth = depth;
    dents[ndents].animation = false;
    ndents++;
}

static void add_rule_decls(const struct rule *r, uint32_t spec, uint32_t order, bool author, uint8_t pseudo, int depth) {
    for (int i = 0; i < r->ndecls; i++) {
        uint64_t key = (author ? 1ull << 62 : 0) | (uint64_t)(spec & 0x3FFFFFFF) << 32 |
                       (uint64_t)(order & 0xFFFFFF) << 8 | (uint64_t)(i < 255 ? i : 255);
        add_dent(&r->decls[i], key, pseudo, depth);
    }
}

static void collect(const struct ient *l, node_t *e, node_t *scope, uint32_t order) {
    node_t *root = doc_node_root(e, false);
    for (; l; l = l->next) {
        if (css_worker() && __atomic_load_n(&e->owner->native_cancelled, __ATOMIC_ACQUIRE)) return;
        if (css_can_service() && !web_native_checkpoint(e->owner)) return;
        if (css_can_service()) web_avmedia_checkpoint();
        if(e->owner->profile_enabled)__atomic_fetch_add(&e->owner->profile.style_rule_checks, 1, __ATOMIC_RELAXED);
        if (!l->ua) {
            if (!scope && root && root->shadow_host) continue;
            if (scope && l->sel->pseudo != PE_SLOTTED && root != scope && e != scope->shadow_host) continue;
        }
        if (!container_chain_ok(l->r->media,e,l->sel->pseudo==PE_BEFORE || l->sel->pseudo==PE_AFTER)) continue;
        /* Only cascade rule matching reads the published form snapshot. Live
           matches()/querySelector() outside this call keep their normal API. */
        bool previous = css_scratch_get()->form_snapshot;
        css_scratch_get()->form_snapshot = true;
        bool matches = match_from(l->sel->right, e, scope);
        css_scratch_get()->form_snapshot = previous;
        if (matches)
            add_rule_decls(l->r, l->sel->spec, l->order + order, !l->ua,
                           l->sel->pseudo == PE_SLOTTED ? PE_NONE : l->sel->pseudo, scope_depth(scope));
    }
}

static void collect_index(struct css_binding *binding, node_t *e, node_t *scope) {
    for (; binding; binding = binding->next) {
        struct css_index *x = binding->index;
        if (e->id) {
            struct ient **l = ht_slot(NULL, &x->ids, e->id, false);
            if (l) collect(*l, e, scope, binding->order);
        }
        for (int i = 0; i < e->nclasses; i++) {
            bool duplicate = false;
            for (int j = 0; j < i; j++) if (!strcmp(e->classes[i], e->classes[j])) duplicate = true;
            if (duplicate) continue;
            struct ient **l = ht_slot(NULL, &x->classes, e->classes[i], false);
            if (l) collect(*l, e, scope, binding->order);
        }
        struct ient **l = ht_slot(NULL, &x->tags, e->name, false);
        if (l) collect(*l, e, scope, binding->order);
        collect(x->univ, e, scope, binding->order);
    }
}

static void collect_scope(node_t *root, node_t *e) {
    if (root && root->style_scope)
        collect_index(root->style_scope->first, e, root->shadow_host ? root : NULL);
}

static int cmp_dent(const void *a, const void *b) {
    const struct dent *da = a, *db = b;
    /* Animation origin beats normal styles, never !important. It must not
       acquire author inline specificity or the shadow encapsulation order. */
    if (da->animation != db->animation) {
        const struct dent *normal = da->animation ? db : da;
        bool animated_first = !normal->d->important;
        return da->animation == animated_first ? -1 : 1;
    }
    if (da->animation) return da->key < db->key ? 1 : da->key > db->key ? -1 : 0;
    uint64_t x = da->key, y = db->key;
    /* Encapsulation context precedes specificity: outer normal declarations
       win; inner !important declarations win, including over inline styles. */
    if ((x >> 62) == (y >> 62) && da->scope_depth != db->scope_depth) {
        bool inner_wins = (x & (1ull << 63)) != 0;
        return (da->scope_depth > db->scope_depth) == inner_wins ? -1 : 1;
    }
    return x < y ? 1 : x > y ? -1 : 0;
}

struct var_index_entry {
    const struct custom_prop *property;
    size_t length;
    uint32_t hash;
};

static void var_index_clear(void) {
    free(var_index);
    var_index = NULL;
    var_index_head = NULL;
    var_index_capacity = 0;
}

static uint32_t var_name_hash(const char *name, size_t n) {
    uint32_t hash = 2166136261u;
    for (size_t i = 0; i < n; i++) hash = (hash ^ (unsigned char)name[i]) * 16777619u;
    return hash;
}

static void var_index_prepare(const struct custom_prop *head) {
    if (head == var_index_head) return;
    var_index_clear();
    var_index_head = head;
    size_t count = 0, capacity = 1;
    for (const struct custom_prop *v = head; v; v = v->next) {
        if (count == SIZE_MAX / 2) return;
        count++;
    }
    if (!count) return;
    while (capacity < count * 2) {
        if (capacity > SIZE_MAX / 2) return;
        capacity *= 2;
    }
    if (capacity > SIZE_MAX / sizeof *var_index) return;
    struct var_index_entry *entries = calloc(capacity, sizeof *entries);
    if (!entries) return; /* An optional accelerator, not a style/OOM policy. */
    for (const struct custom_prop *v = head; v; v = v->next) {
        size_t length = strlen(v->name);
        uint32_t hash = var_name_hash(v->name, length);
        size_t slot = hash & (capacity - 1);
        while (entries[slot].property) {
            if (entries[slot].hash == hash && entries[slot].length == length &&
                !memcmp(entries[slot].property->name, v->name, length)) break;
            slot = (slot + 1) & (capacity - 1);
        }
        /* The first property in the immutable inheritance list wins. Values
           may resolve in place, so retain the property, not a value snapshot. */
        if (!entries[slot].property)
            entries[slot] = (struct var_index_entry){v, length, hash};
    }
    var_index = entries;
    var_index_capacity = capacity;
}

static const struct custom_prop *find_var(const struct custom_prop *v, const char *name, size_t n) {
    var_index_prepare(v);
    if (var_index) {
        uint32_t hash = var_name_hash(name, n);
        size_t slot = hash & (var_index_capacity - 1);
        while (var_index[slot].property) {
            const struct var_index_entry *entry = &var_index[slot];
            if (entry->hash == hash && entry->length == n &&
                !memcmp(entry->property->name, name, n)) return entry->property;
            slot = (slot + 1) & (var_index_capacity - 1);
        }
        return NULL;
    }
    for (; v; v = v->next)
        if (strlen(v->name) == n && !memcmp(v->name, name, n)) return v;
    return NULL;
}

static bool subst_put(sbuf *b, const char *s, size_t n) {
    if (b->n == SIZE_MAX || n > SIZE_MAX - b->n - 1) return false;
    size_t need = b->n + n + 1;
    if (need > b->cap) {
        size_t cap = b->cap ? b->cap : 64;
        while (cap < need) {
            if (cap > SIZE_MAX / 2) { cap = need; break; }
            cap *= 2;
        }
        char *p = realloc(b->p, cap);
        if (!p) return false;
        b->p = p; b->cap = cap;
    }
    memcpy(b->p + b->n, s, n); b->n += n; b->p[b->n] = 0;
    return true;
}

struct subst_walk {
    struct subst_walk *parent;
    const struct custom_prop *property;
    const char *s;
    size_t n, i;
};

/* No C recursion: real allocation/output overflow bounds expansion. Cyclic
   computed custom properties are represented by NULL, distinct from an empty
   but valid value; a defensive active-property check also protects raw callers. */
static bool subst(const char *s, size_t n, const struct custom_prop *vars, sbuf *out) {
    size_t original = out->n;
    struct subst_walk *f = calloc(1, sizeof *f);
    if (!f) return false;
    f->s = s; f->n = n;
    bool ok = true;
    while (f) {
        s = f->s; n = f->n; size_t i = f->i;
        if (i == n) { struct subst_walk *parent = f->parent; free(f); f = parent; continue; }
        if (s[i] == '"' || s[i] == '\'') {
            size_t start = i; char quote = s[i++];
            while (i < n && s[i] != quote) { if (s[i] == '\\' && i + 1 < n) i++; i++; }
            if (i < n) i++;
            if (!subst_put(out, s + start, i - start)) { ok = false; break; }
            f->i = i;
            continue;
        }
        bool token = i == 0 || (!ident_char((unsigned char)s[i-1]) && s[i-1] != '#' && s[i-1] != '@');
        bool is_var = n - i >= 4 && strn_ieq(s + i, "var(", 4) && token;
        bool is_env = !is_var && n - i >= 4 && strn_ieq(s + i, "env(", 4) && token;
        if (!is_var && !is_env) {
            if (!subst_put(out, s + i, 1)) { ok = false; break; }
            f->i++;
            continue;
        }
        const char *a = s + i + 4, *e = s + n;
        const char *close = scan_to(a, e, ")");
        if (close >= e) { ok = false; break; }
        const char *comma = scan_to(a, close, ",");
        const char *ns = skip_ws(a, comma), *ne = comma;
        trim_r(ns, &ne);
        if (is_var && !supports_custom_name(ns, (size_t)(ne-ns))) { ok = false; break; }
        const struct custom_prop *property = is_var ? find_var(vars, ns, (size_t)(ne - ns)) : NULL;
        const char *val = property ? property->value : NULL, *text;
        size_t length;
        if (val) {
            for (struct subst_walk *p = f; p; p = p->parent)
                if (p->property == property) { ok = false; break; }
            if (!ok) break;
            text = val; length = strlen(val);
        } else if (comma < close) {
            text = comma + 1; length = (size_t)(close - comma - 1); property = NULL;
        } else if (is_env) {
            if (!subst_put(out, "0px", 3)) { ok = false; break; }
            f->i = (size_t)(close + 1 - s); continue;
        } else { ok = false; break; }
        struct subst_walk *child = calloc(1, sizeof *child);
        if (!child) { ok = false; break; }
        child->parent = f; child->property = property; child->s = text; child->n = length;
        f->i = (size_t)(close + 1 - s); f = child;
    }
    while (f) { struct subst_walk *parent = f->parent; free(f); f = parent; }
    if (!ok) { out->n = original; if (out->p && out->cap > original) out->p[original] = 0; }
    return ok;
}

struct var_walk {
    struct var_walk *next, *parent, *component;
    struct custom_prop *property;
    const char *s, *e;
    size_t index, low;
    bool seen, on_stack, self_edge;
};

static struct var_walk *var_lookup(struct var_walk *v, const char *name, size_t n) {
    for (; v; v = v->next)
        if (strlen(v->property->name) == n && !memcmp(v->property->name, name, n)) return v;
    return NULL;
}

/* Iterate every var() token, including references in an unused fallback.
   Strings and hash/at-keywords are not variable function tokens. */
static struct var_walk *var_dependency(struct var_walk *f, struct var_walk *all) {
    const char *s = f->s, *e = f->e;
    while (s < e) {
        if (*s == '"' || *s == '\'') {
            char quote = *s++;
            while (s < e && *s != quote) { if (*s == '\\' && s + 1 < e) s++; s++; }
            if (s < e) s++;
            continue;
        }
        if ((*s == '#' || *s == '@') && s + 1 < e && ident_char((unsigned char)s[1])) {
            s++; while (s < e && ident_char((unsigned char)*s)) s++; continue;
        }
        if ((size_t)(e-s) >= 4 && strn_ieq(s,"var(",4) &&
            (s == f->property->value || !ident_char((unsigned char)s[-1]))) {
            const char *close = scan_to(s + 4, e, ")");
            if (close == e) { f->s = e; return NULL; }
            const char *comma = scan_to(s + 4, close, ","), *ns = skip_ws(s + 4, comma), *ne = comma;
            trim_r(ns,&ne); s += 4; /* Scan fallback tokens on the next call too. */
            struct var_walk *dep = var_lookup(all, ns, (size_t)(ne-ns));
            if (dep) { f->s = s; return dep; }
        } else s++;
    }
    f->s = e; return NULL;
}

static bool value_references(const char *s) {
    if (!s) return false;
    size_t n = strlen(s);
    for (size_t i=0; n-i>=4; i++)
        if (strn_ieq(s+i,"var(",4) || strn_ieq(s+i,"env(",4)) return true;
    return false;
}

/* Iterative Tarjan: only declarations on this element form graph vertices.
   Inherited properties are already computed, so cannot form child cycles.
   Components are resolved in dependency order before mutating raw values. */
static void resolve_vars(web_doc *d, arena_t *a, struct custom_prop *mine, const struct custom_prop *inherited) {
    bool references=false;
    for (struct custom_prop *m=mine;m!=inherited;m=m->next)
        if (value_references(m->value)) { references=true; break; }
    if (!references) return; /* Retain immutable literal AST values, no scratch/copy. */
    struct var_workspace { arena_t arena; sbuf output; } *work = calloc(1,sizeof *work);
    if (!work) {
        if (css_worker()) { ar_alloc(a, SIZE_MAX); return; }
        for (struct custom_prop *m=mine;m!=inherited;m=m->next) m->value=NULL;
        web_js_console(d,2,"Custom property graph allocation failed"); return;
    }
    jmp_buf trap; jmp_buf *outer = a->trap;
    work->arena.trap = a->trap = &trap;
    int failed = setjmp(trap);
    if (failed) {
        sb_free(&work->output); ar_free(&work->arena); free(work); a->trap=outer;
        if (outer) longjmp(*outer,failed);
        for (struct custom_prop *m=mine;m!=inherited;m=m->next) m->value=NULL;
        web_js_console(d,2,"Custom property graph/value allocation failed"); return;
    }
    struct var_walk *all=NULL;
    for (struct custom_prop *m=mine;m!=inherited;m=m->next) {
        struct var_walk *v=ar_alloc(&work->arena,sizeof *v);
        v->property=m;v->s=m->value?m->value:"";v->e=v->s+strlen(v->s);v->next=all;all=v;
    }
    size_t index=0;
    struct var_walk *components=NULL;
    for (struct var_walk *root=all;root;root=root->next) {
        if (root->seen) continue;
        struct var_walk *f=root;
        while (f) {
            if (!f->seen) {
                if (index==SIZE_MAX) longjmp(trap,1);
                f->seen=f->on_stack=true;f->index=f->low=++index;
                f->component=components;components=f;
            }
            struct var_walk *dep=var_dependency(f,all);
            if (dep) {
                if (dep==f) f->self_edge=true;
                if (!dep->seen) { dep->parent=f;f=dep;continue; }
                if (dep->on_stack && dep->index<f->low) f->low=dep->index;
                continue;
            }
            if (f->low==f->index) {
                bool cycle=components!=f || f->self_edge;
                struct var_walk *v;
                do {
                    v=components;components=v->component;v->on_stack=false;
                    if (cycle) v->property->value=NULL;
                    else if (value_references(v->property->value)) {
                        work->output.n=0;
                        if (subst(v->property->value,strlen(v->property->value),mine,&work->output))
                            v->property->value=ar_strndup(a,work->output.p?work->output.p:"",work->output.n);
                        else v->property->value=NULL;
                    }
                } while (v!=f);
            }
            struct var_walk *parent=f->parent;
            if (parent && f->low<parent->low) parent->low=f->low;
            f=parent;
        }
    }
    sb_free(&work->output); ar_free(&work->arena); free(work); a->trap=outer;
}

struct cascade {
    web_doc *d;
    float rem;
    int vw, vh;
    sbuf vbuf;
    arena_t *a;
    bool full;
    jmp_buf *trap;
    struct cascade_frontier *frontier;
};

static void apply_decl(struct cascade *c, struct cx *cx, const struct decl *d) {
    /* A winning longhand cannot consume this value. Skip var lookup / scratch
       expansion before css_apply discards it. Shorthand indices are never
       marked: their individual longhands may still need this declaration. */
    if (cx->set[css_prop_index(d->p)]) return;
    const char *v = d->value;
    size_t n = strlen(v);
    if (value_references(v)) {
        c->vbuf.n = 0;
        if (!subst(v, n, cx->s->vars, &c->vbuf)) return;
        v = sb_cstr(&c->vbuf);
        n = c->vbuf.n;
    }
    css_apply(d->p, v, n, cx);
}

static style_t *compute(struct cascade *c, node_t *e, const style_t *parent, uint8_t pseudo, bool underlying) {
    web_doc *d = c->d;
    style_t *s = ar_alloc(c->a, sizeof *s);
    css_style_init(s, parent);
    /* custom properties: the most important declaration of each name */
    const struct custom_prop *inherited = s->vars;
    struct custom_prop *mine = NULL;
    for (int i = 0; i < ndents; i++) {
        const struct decl *x = dents[i].d;
        if (dents[i].pseudo != pseudo || x->p || (underlying && dents[i].animation)) continue;
        bool dup = false;
        for (struct custom_prop *m = mine; m; m = m->next)
            if (!strcmp(m->name, x->name)) dup = true;
        if (dup) continue;
        struct custom_prop *cp = ar_alloc(c->a, sizeof *cp);
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
        resolve_vars(d,c->a,mine,inherited);
    }
    memset(setbits, 0, (size_t)css_prop_count());
    struct cx cx = {s, parent, e, parent ? parent->font_size : 16, c->rem, (float)c->vw, (float)c->vh,
                    c->a, setbits, true, false, false, 0, 0, 0, false, false};
    struct css_ctx *index=d->sty.ctx;
    if(index && __atomic_load_n(&index->containers,__ATOMIC_ACQUIRE)) {
        bool inclusive=pseudo==PE_BEFORE || pseudo==PE_AFTER;
        node_t *inline_container=query_container(e,NULL,1,inclusive),*block_container=query_container(e,NULL,2,inclusive);
        if(inline_container){cx.cq_width=inline_container->container_width;cx.cq_width_set=true;}
        if(block_container){cx.cq_height=block_container->container_height;cx.cq_height_set=true;}
    }
    for (int i = 0; i < ndents; i++)
        if ((!underlying || !dents[i].animation) && dents[i].pseudo == pseudo && dents[i].d->p && css_prop_is_font(dents[i].d->p)) apply_decl(c, &cx, dents[i].d);
    cx.font_pass = false;
    cx.em = s->font_size;
    for (int i = 0; i < ndents; i++)
        if ((!underlying || !dents[i].animation) && dents[i].pseudo == pseudo && dents[i].d->p) apply_decl(c, &cx, dents[i].d);
    if (!pseudo) web_dialog_style(d,e,s);
    if(pseudo==PE_BACKDROP) {
        if(s->position!=POS_FIXED && s->position!=POS_ABSOLUTE)s->position=POS_ABSOLUTE;
        if(s->display==D_CONTENTS)s->display=D_BLOCK; s->float_=FL_NONE;
    }
    css_style_finish(s, parent, e == d->html && !pseudo);
    s->named_font=select_web_font(d,e,s,c->vw,c->vh,c->a);
    if (parent && parent->display == D_CONTENTS) {
        /* Inheritance uses the slot, while flex/grid blockification uses the
           nearest box-generating flat ancestor. A contents slot is not an
           anonymous flex item and cannot prevent its assigned items from
           becoming the host's flex/grid items. */
        node_t *p = pseudo ? e : doc_flat_parent(e);
        while (p && p->style && p->style->display == D_CONTENTS) p = doc_flat_parent(p);
        if (p && p->style && (p->style->display == D_FLEX || p->style->display == D_INLINE_FLEX ||
                             p->style->display == D_GRID || p->style->display == D_INLINE_GRID))
            css_style_finish(s, p->style, false);
    }
    return s;
}

static double motion_eased(const struct css_easing *e,double x) {
    if(e->kind==CM_STEP_START)return 1;
    if(e->kind==CM_STEP_END)return x>=1?1:0;
    if(e->kind==CM_LINEAR || x<=0 || x>=1)return x;
    double lo=0,hi=1,t=x;
    for(unsigned i=0;i<30;i++) {
        double u=1-t,z=3*u*u*t*e->x1+3*u*t*t*e->x2+t*t*t;
        if(fabs(z-x)<1e-9)break;
        if(z<x)lo=t;else hi=t;t=(lo+hi)/2;
    }
    double u=1-t;return 3*u*u*t*e->y1+3*u*t*t*e->y2+t*t*t;
}
static const struct opacity_keyframes *motion_keyframes(struct cascade *c,node_t *n,const char *name) {
    node_t *root=doc_node_root(n,false);
    /* Bindings are already ordered (including imports) and filtered by owner
       media. Search the current tree first, then only its nearest ancestor
       trees if unresolved; never scan unrelated shadow roots globally. */
    while(root) {
        const struct opacity_keyframes *found=NULL;
        struct css_scope *scope=root->style_scope;
        for(struct css_binding *b=scope?scope->first:NULL;b;b=b->next)
            for(const struct opacity_keyframes *k=b->index->motions;k;k=k->next) {
                if(__atomic_load_n(&c->d->native_cancelled,__ATOMIC_ACQUIRE) ||
                    (css_can_service() && !web_native_checkpoint(c->d)))return NULL;
                if(!strcmp(k->name,name) && font_conditions_ok(k->media,c->vw,c->vh,web_js_enabled(c->d)))found=k;
            }
        if(found)return found;
        root=root->shadow_host?doc_node_root(root->shadow_host,false):NULL;
    }
    return NULL;
}
static struct css_motion_state *motion_state(struct cascade *c,node_t *n,const char *name) {
    struct css_motion_state *state=n->css_motion;
    if(state && (state->document!=c->d || strcmp(state->name,name)))css_motion_reset(n),state=NULL;
    if(!state) {
        state=calloc(1,sizeof *state);char *copy=strdup(name);
        if(!state || !copy){free(state);free(copy);ar_alloc(c->a,SIZE_MAX);return NULL;}
        state->node=n;state->document=c->d;state->name=copy;state->last=c->d->css_motion_now;
        struct css_motion_state *head;
        do { head=__atomic_load_n(&c->d->css_motions,__ATOMIC_RELAXED);state->next=head; }
        while(!__atomic_compare_exchange_n(&c->d->css_motions,&head,state,false,__ATOMIC_RELEASE,__ATOMIC_RELAXED));
        n->css_motion=state;
    }
    return state;
}
static bool motion_frame_value(struct cascade *c,node_t *n,const style_t *base,const style_t *parent,
    const struct opacity_frame *frame,float *opacity,struct css_easing *easing) {
    uint8_t bits[css_prop_count()];memset(bits,0,sizeof bits);
    style_t temp=*base;
    struct cx cx={.s=&temp,.parent=parent,.node=n,.em=base->font_size,.rem=c->rem,
        .vw=(float)c->vw,.vh=(float)c->vh,.a=c->a,.set=bits};
    bool has=false;
    const struct propdef *opacity_prop=css_prop_lookup("opacity",7),
        *timing_prop=css_prop_lookup("animation-timing-function",25);
    /* Same-offset blocks cascade in source order; the last valid declaration
       in each block wins. Invalid later values do not erase valid earlier ones. */
    for(int i=frame->count-1;i>=0;i--) {
        const struct decl *x=&frame->decls[i];if(x->important || !x->p)continue;
        if(x->p==opacity_prop){int at=css_prop_index(x->p);bool before=bits[at];apply_decl(c,&cx,x);if(!before && bits[at])has=true;}
        else if(x->p==timing_prop)apply_decl(c,&cx,x);
    }
    *opacity=temp.opacity;*easing=temp.motion.easing;return has;
}
static bool motion_opacity(struct cascade *c,node_t *n,const style_t *base,const style_t *parent,float *out) {
    const struct css_motion *spec=&base->motion;
    if(!spec->name || base->display==D_NONE || doc_node_root(n,true)!=c->d->root) {
        css_motion_reset(n);return false;
    }
    const struct opacity_keyframes *key=motion_keyframes(c,n,spec->name);
    if(!key){css_motion_reset(n);return false;} /* unresolved names do not start */
    struct css_motion_state *state=motion_state(c,n,spec->name);if(!state)return false;
    uint64_t now=c->d->css_motion_now;
    if(!state->paused && now>=state->last)state->elapsed+=(double)(now-state->last);
    state->last=now;state->paused=spec->paused;
    double local=state->elapsed-spec->delay;
    double ad=spec->duration==0 || spec->iterations==0?0:(double)spec->duration*spec->iterations;
    state->running=!spec->paused && state->elapsed<(double)spec->delay+ad;
    double overall;
    if(local<0) {
        if(spec->fill!=CM_FILL_BACKWARDS && spec->fill!=CM_FILL_BOTH)return false;
        overall=0;
    } else if(local>=ad) {
        if(spec->fill!=CM_FILL_FORWARDS && spec->fill!=CM_FILL_BOTH)return false;
        overall=spec->iterations; if(!isfinite(overall))overall=1;
    } else overall=spec->duration>0?local/spec->duration:0;
    double iteration=floor(overall),progress=overall-iteration;
    if(local>=ad && overall>0 && progress==0){progress=1;iteration-=1;}
    bool reverse=spec->direction==CM_REVERSE ||
        (spec->direction==CM_ALTERNATE && fmod(iteration,2)!=0) ||
        (spec->direction==CM_ALTERNATE_REVERSE && fmod(iteration,2)==0);
    if(reverse)progress=1-progress;
    float left=0,right=1,lv=base->opacity,rv=base->opacity;
    struct css_easing ease=spec->easing;bool any=false;
    for(const struct opacity_frame *f=key->first;f;f=f->next) {
        if(__atomic_load_n(&c->d->native_cancelled,__ATOMIC_ACQUIRE) ||
            (css_can_service() && !web_native_checkpoint(c->d)))return false;
        float value;struct css_easing frame_ease;
        if(!motion_frame_value(c,n,base,parent,f,&value,&frame_ease))continue;
        any=true;
        if(f->offset<=progress && f->offset>=left){left=f->offset;lv=value;ease=frame_ease;}
        if(f->offset>progress && f->offset<=right){right=f->offset;rv=value;}
    }
    if(!any){state->running=false;return false;}
    /* Backwards fill during the delay is the directed first endpoint, not
       an easing sample: step-start must not jump before the active phase. */
    double fraction=right>left?(progress-left)/(right-left):0;
    double p=local<0?fraction:motion_eased(&ease,fraction);
    double value=lv+(rv-lv)*p;
    *out=(float)(value<0?0:value>1?1:value);return true;
}

static void clear_styles(node_t *n) {
    n->animation_base_style = NULL;
    if (n->shadow_root) clear_styles(n->shadow_root);
    for (node_t *c = n->first; c; c = c->next) {
        c->style = NULL;
        clear_styles(c);
    }
}
static void motion_hide_subtree(node_t *n) {
    css_motion_reset(n);
    if(n->shadow_root)motion_hide_subtree(n->shadow_root);
    for(node_t *ch=n->first;ch;ch=ch->next)motion_hide_subtree(ch);
}

void css_node_style_free(node_t *n) {
    if (!n) return;
    css_motion_reset(n);
    ar_free(&n->style_mem); ar_free(&n->style_previous_mem);
    n->style = n->animation_base_style = n->style_previous = NULL;
}

void css_node_style_unpublish(node_t *n) {
    if (!n) return;
    css_motion_reset(n);
    /* Detached inherited styles can borrow an ancestor's variable/font data.
       Keep their arenas alive until published box borrows have been retired,
       but never let a detached tree expose those ancestor-dependent styles. */
    n->style = n->animation_base_style = NULL;
    n->container_valid = false;
    n->query_inline_container=n->query_size_container=NULL;
    if (n->shadow_root) css_node_style_unpublish(n->shadow_root);
    if (n->template_content) css_node_style_unpublish(n->template_content);
    for (node_t *c = n->first; c; c = c->next) css_node_style_unpublish(c);
}

void css_styles_release(web_doc *d) {
    node_t *n = d->style_retired_first; d->style_retired_first = NULL;
    while (n) {
        node_t *next = n->style_retired_next;
        /* A cancelled worker may have retired this arena but not published a
           replacement. Never retain its old style or stack-local OOM target. */
        if(n->style==n->style_previous)n->style=NULL;
        n->style_mem.trap=NULL;
        ar_free(&n->style_previous_mem); n->style_previous = NULL; n->style_retired_next = NULL;
        n = next;
    }
}

/* A mutation's sibling set is the conservative local boundary for +/-/~ and
   nth/empty selectors. Descendants are restyled because inheritance and
   ancestor combinators may change. Ancestor-dependent relational rules retain
   a full invalidation fallback, as do viewport/resource/UI transactions. */
static node_t *dirty_flat_parent(node_t *n) {
    /* Mutation invalidation must NOT flush slot assignment: construction can
       enqueue thousands of changes before any geometry/assignment read. The
       last published snapshot is sufficient to propagate dirty ancestors;
       an assignment/topology transaction separately requests a full cascade. */
    if(!n || n->shadow_host)return NULL;
    node_t *p=n->parent;
    if(!p)return NULL;
    if(p->shadow_root)return n->assigned_slot;
    if(p->shadow_host)return p->shadow_host;
    if(p->type==N_ELEM && !p->foreign && p->tag==T_slot && p->slot_assigned_first &&
        doc_node_root(p,false)->shadow_host)return NULL;
    return p;
}
void css_mark_dirty(web_doc *d, node_t *n) {
    if (!d) return;
    d->need_style = true;
    if (!n || !d->sty.ctx || d->sty.ctx->relational) { d->style_full_dirty = true; return; }
    if (n->type != N_ELEM) n = dirty_flat_parent(n);
    if (!n) { d->style_full_dirty = true; return; }
    node_t *parent = dirty_flat_parent(n);
    if (d->sty.ctx->siblings && parent && parent->type == N_ELEM) n = parent;
    n->style_dirty = true; d->style_pending_dirty = true;
    for (node_t *p = dirty_flat_parent(n); p; p = dirty_flat_parent(p)) p->style_children_dirty = true;
}

static bool equal_string(const char *a,const char *b) { return a==b || (a && b && !strcmp(a,b)); }
static bool equal_tracks(const struct gtemplate *a,const struct gtemplate *b) {
    if(a==b)return true;
    return a && b && a->n==b->n && a->rep_at==b->rep_at && a->rep_n==b->rep_n && a->rep_fit==b->rep_fit &&
        (!a->n || !memcmp(a->t,b->t,(size_t)a->n*sizeof *a->t));
}
static bool equal_areas(const struct gareas *a,const struct gareas *b) {
    if(a==b)return true;
    if(!a || !b || a->rows!=b->rows || a->cols!=b->cols)return false;
    for(int i=0;i<a->rows*a->cols;i++)if(!equal_string(a->cell[i],b->cell[i]))return false;
    return true;
}
bool css_style_layout_equal(const style_t *a,const style_t *b) {
    if(a==b)return true;
    if(!a || !b)return false;
    if(!equal_string(a->font_names,b->font_names) || !equal_string(a->content,b->content) ||
        !equal_string(a->motion.name,b->motion.name) ||
        !equal_string(a->container_names,b->container_names) ||
        !equal_string(a->list_style_string,b->list_style_string) ||
        !equal_tracks(a->grid_cols,b->grid_cols) || !equal_tracks(a->grid_rows,b->grid_rows) ||
        !equal_tracks(a->grid_auto_cols,b->grid_auto_cols) || !equal_tracks(a->grid_auto_rows,b->grid_auto_rows) ||
        !equal_areas(a->grid_areas,b->grid_areas))return false;
    for(int i=0;i<4;i++)if(!equal_string(a->grid_place[i].name,b->grid_place[i].name))return false;
    if(!css_style_layout_equal(a->before,b->before) || !css_style_layout_equal(a->after,b->after) ||
        !css_style_layout_equal(a->backdrop,b->backdrop))return false;
    style_t x=*a,y=*b;
    x.motion.name=y.motion.name=NULL;
    x.font_names=y.font_names=NULL; x.content=y.content=NULL; x.list_style_string=y.list_style_string=NULL;
    x.container_names=y.container_names=NULL;
    x.grid_cols=y.grid_cols=x.grid_rows=y.grid_rows=x.grid_auto_cols=y.grid_auto_cols=x.grid_auto_rows=y.grid_auto_rows=NULL;
    x.grid_areas=y.grid_areas=NULL;
    for(int i=0;i<4;i++)x.grid_place[i].name=y.grid_place[i].name=NULL;
    x.before=y.before=x.after=y.after=x.backdrop=y.backdrop=NULL;
    /* Custom properties have already been resolved into real properties. Paint
       data/colors are replaced in boxes/runs, but cannot change geometry. */
    x.vars=y.vars=NULL; x.gradient=y.gradient=NULL;
    x.bg_image=y.bg_image=x.mask_image=y.mask_image=NULL;
    x.bg_img=y.bg_img=x.mask_img=y.mask_img=0;
    x.color=y.color=x.bg_color=y.bg_color=0;
    memset(x.border_color,0,sizeof x.border_color);memset(y.border_color,0,sizeof y.border_color);
    memset(&x.svg_fill,0,sizeof x.svg_fill);memset(&y.svg_fill,0,sizeof y.svg_fill);
    memset(&x.svg_stroke,0,sizeof x.svg_stroke);memset(&y.svg_stroke,0,sizeof y.svg_stroke);
    x.svg_stop_color=y.svg_stop_color=0;
    x.opacity=y.opacity=x.svg_fill_opacity=y.svg_fill_opacity=x.svg_stroke_opacity=y.svg_stroke_opacity=x.svg_stop_opacity=y.svg_stop_opacity=0;
    memset(x.grad,0,sizeof x.grad);memset(y.grad,0,sizeof y.grad);x.has_grad=y.has_grad=false;
    x.bg_repeat=y.bg_repeat=x.bg_size_kind=y.bg_size_kind=x.mask_repeat=y.mask_repeat=x.mask_size_kind=y.mask_size_kind=0;
    memset(x.bg_pos,0,sizeof x.bg_pos);memset(y.bg_pos,0,sizeof y.bg_pos);
    memset(x.bg_size,0,sizeof x.bg_size);memset(y.bg_size,0,sizeof y.bg_size);
    memset(x.mask_pos,0,sizeof x.mask_pos);memset(y.mask_pos,0,sizeof y.mask_pos);
    memset(x.mask_size,0,sizeof x.mask_size);memset(y.mask_size,0,sizeof y.mask_size);
    return !memcmp(&x,&y,sizeof x);
}

static bool style_box_compatible(node_t *e, const style_t *old, const style_t *next) {
    if (!old || !next) return false;
    if(e->foreign)return false; /* SVG snapshots serialize computed paint. */
    if (css_style_layout_equal(old,next)) return true;
    if (e->foreign || e->tag == T_input || e->tag == T_textarea || e->tag == T_select || e->tag == T_img ||
        e->tag == T_canvas || e->tag == T_video || e->tag == T_iframe || e->tag == T_frameset ||
        old->before || old->after || old->backdrop || next->before || next->after || next->backdrop) return false;
    if (old->display != next->display || old->position != next->position || old->float_ != next->float_ ||
        old->container_type != next->container_type ||
        old->overflow != next->overflow || old->content_visibility != next->content_visibility ||
        old->white_space != next->white_space || old->text_transform != next->text_transform ||
        old->list_style != next->list_style || old->list_style_inside != next->list_style_inside) return false;
    return old->display == D_BLOCK || old->display == D_FLOW_ROOT || old->display == D_INLINE || old->display == D_NONE;
}

static void retire_style(struct cascade *c, node_t *e) {
    e->style_previous = e->style; e->style_previous_mem = e->style_mem;
    memset(&e->style_mem, 0, sizeof e->style_mem);
    e->style_mem.chunk_size = 4096;
    e->style_mem.trap = c->trap;
    node_t *head;
    do { head = __atomic_load_n(&c->d->style_retired_first, __ATOMIC_RELAXED); e->style_retired_next = head; }
    while (!__atomic_compare_exchange_n(&c->d->style_retired_first, &head, e, false, __ATOMIC_RELEASE, __ATOMIC_RELAXED));
    c->a = &e->style_mem;
}

static void cascade_node(struct cascade *c, node_t *e, const style_t *parent);
struct cascade_job { node_t *node; const style_t *parent; float rem; bool full; };
struct cascade_frontier { struct cascade_job *v; size_t n, cap; };
struct cascade_batch { struct cascade parent; struct cascade_job *jobs; };
static void cascade_enqueue(struct cascade *c, node_t *e, const style_t *parent) {
    if (!c->full && !e->style_dirty && !e->style_children_dirty && e->style) return;
    struct cascade_frontier *f = c->frontier;
    if (f->n == f->cap) {
        size_t capacity = f->cap ? f->cap * 2 : 32;
        if (capacity < f->cap || capacity > SIZE_MAX / sizeof *f->v) ar_alloc(c->a, SIZE_MAX);
        struct cascade_job *v = realloc(f->v, capacity * sizeof *v);
        if (!v) ar_alloc(c->a, SIZE_MAX);
        f->v = v; f->cap = capacity;
    }
    f->v[f->n++] = (struct cascade_job){e, parent, c->rem, c->full};
}
static void cascade_worker(size_t i, void *context) {
    struct cascade_batch *batch = context;
    struct cascade_job *job = &batch->jobs[i];
    node_t *e = job->node;
    if (e->type != N_ELEM || __atomic_load_n(&batch->parent.d->native_cancelled, __ATOMIC_ACQUIRE)) return;
    struct css_scratch scratch = {.worker=true};
    void *previous = thread_local_get(2); thread_local_set(2, &scratch);
    struct cascade c = batch->parent; c.vbuf = (sbuf){0};
    c.rem = job->rem; c.full = job->full; c.frontier = NULL;
    jmp_buf trap; c.trap = &trap;
    if (setjmp(trap)) __atomic_store_n(&c.d->native_cancelled, true, __ATOMIC_RELEASE);
    else {
        setbits = malloc((size_t)css_prop_count());
        if (!setbits) __atomic_store_n(&c.d->native_cancelled, true, __ATOMIC_RELEASE);
        else cascade_node(&c, e, job->parent);
    }
    var_index_clear(); free(dents); free(setbits); sb_free(&c.vbuf);
    thread_local_set(2, previous);
}
static void cascade_service(void *context) {
    web_doc *d = context;
    web_native_checkpoint(d); web_avmedia_checkpoint();
}

static void cascade_node(struct cascade *c, node_t *e, const style_t *parent) {
    /* Native-only supply may run while this tree is being rebuilt. It never
       executes author code or reads the incomplete style/box tree. */
    if (css_can_service()) web_avmedia_checkpoint();
    web_doc *d = c->d;
    if (css_can_service()) { if (!web_native_checkpoint(d)) return; }
    else if (__atomic_load_n(&d->native_cancelled, __ATOMIC_ACQUIRE)) return;
    bool redo = c->full || e->style_dirty || !e->style;
    if (!redo && !e->style_children_dirty) return;
    bool inherited_full = c->full;
    if (!redo) goto descend;
    retire_style(c, e);
    css_scratch_get()->arena=c->a;
    if(d->profile_enabled) {
        parallel_record_work(PARALLEL_STYLE, 1);
        __atomic_fetch_add(&d->profile.style_visits, 1, __ATOMIC_RELAXED);
        if (css_worker() && parallel_worker_index()) __atomic_fetch_add(&d->profile.style_parallel_nodes, 1, __ATOMIC_RELAXED);
    }
    struct css_ctx *x = d->sty.ctx;
    ndents = 0;
    collect_index(x->ua, e, NULL);
    collect_scope(doc_node_root(e, false), e);
    if (e->shadow_root) collect_scope(e->shadow_root, e); /* :host */
    for (node_t *slot = e->assigned_slot; slot; slot = slot->assigned_slot)
        collect_scope(doc_node_root(slot, false), e); /* ::slotted */
    if (__atomic_load_n(&d->native_cancelled, __ATOMIC_ACQUIRE)) return;
    /* presentational hints and the style attribute */
    struct hints h = {.n = 0, .a = c->a};
    if (!e->foreign || e->tag == T_svg) pres_hints(e, &h);
    if (e->namespace_id == NS_SVG) {
        static const char *const svg_properties[] = {
            "fill", "fill-opacity", "fill-rule", "stroke", "stroke-opacity", "stroke-width",
            "stroke-linecap", "stroke-linejoin", "stop-color", "stop-opacity", "color", "opacity",
            "display", "visibility"
        };
        /* SVG presentation attributes participate at author specificity zero.
           Only null-namespace, exact-case attributes are presentation attributes. */
        for (size_t k = 0; k < sizeof svg_properties / sizeof *svg_properties; k++)
            for (int i = 0; i < e->nattrs; i++) {
                const struct attr *a = &e->attrs[i];
                if (!a->namespace_uri && !strcmp(a->raw, svg_properties[k]))
                    hint_value(&h, svg_properties[k], a->value);
            }
    }
    int depth = scope_depth(doc_node_root(e, false));
    for (int i = 0; i < h.n; i++) add_dent(&h.d[i], 1ull << 62, PE_NONE, depth);
    const char *sa = node_attr(e, "style");
    if (sa && *sa) {
        struct pctx pc = {c->a, NULL, d->base, NULL, 0, false, 0};
        struct decl *ds;
        int nd;
        parse_body(&pc, sa, sa + strlen(sa), NULL, NULL, &ds, &nd);
        for(int i=0;i<nd;i++)if(container_units_used(ds[i].value))
            __atomic_store_n(&((struct css_ctx *)d->sty.ctx)->containers,true,__ATOMIC_RELEASE);
        for (int i = 0; i < nd; i++) add_dent(&ds[i], 1ull << 62 | (uint64_t)0x3FFFFFFF << 32 | (uint64_t)(i < 255 ? i : 255), PE_NONE, depth);
    }
    /* CSS animations precede higher-priority script-created WAAPI effects.
       Their omitted endpoints use the underlying cascade, never last frame's
       computed opacity. The same normal declarations still outrank at the
       important origin in cmp_dent(). */
    bool motion_decl=e->css_motion!=NULL;
    for(int i=0;!motion_decl && i<ndents;i++)if(css_prop_is_motion(dents[i].d->p))motion_decl=true;
    style_t *motion_base=NULL;float opacity;
    if(motion_decl) {
        qsort(dents,(size_t)ndents,sizeof *dents,cmp_dent);
        motion_base=compute(c,e,parent,PE_NONE,true);
    }
    if(motion_base && motion_opacity(c,e,motion_base,parent,&opacity)) {
        char text[48];snprintf(text,sizeof text,"%.9g",(double)opacity);
        struct decl *sample=ar_alloc(c->a,sizeof *sample);
        *sample=(struct decl){.p=css_prop_lookup("opacity",7),.value=ar_strdup(c->a,text)};
        add_dent(sample,0,PE_NONE,0);dents[ndents-1].animation=true;
    }
    for (struct css_animation *a = e->animations; a; a = a->next) {
        struct pctx pc = {c->a, NULL, d->base, NULL, 0, false, 0};
        struct decl *ds; int nd;
        parse_body(&pc, a->text, a->text + strlen(a->text), NULL, NULL, &ds, &nd);
        for(int i=0;i<nd;i++)if(container_units_used(ds[i].value))
            __atomic_store_n(&((struct css_ctx *)d->sty.ctx)->containers,true,__ATOMIC_RELEASE);
        for (int i = 0; i < nd; i++) {
            if (!ds[i].p || ds[i].important) continue;
            add_dent(&ds[i], a->id, a->pseudo, 0);
            dents[ndents - 1].animation = true;
        }
    }
    qsort(dents, (size_t)ndents, sizeof *dents, cmp_dent);
    bool has_before = false, has_after = false;
    for (int i = 0; i < ndents; i++) {
        if (dents[i].pseudo == PE_BEFORE) has_before = true;
        if (dents[i].pseudo == PE_AFTER) has_after = true;
    }
    style_t *s = compute(c, e, parent, PE_NONE, false);
    e->style = s;
    e->animation_base_style = NULL;
    if (e->animations || (motion_base && motion_base->motion.name)) {
        style_t *base = motion_base?motion_base:compute(c,e,parent,PE_NONE,true);
        if (has_before) base->before = compute(c, e, s, PE_BEFORE, true);
        if (has_after) base->after = compute(c, e, s, PE_AFTER, true);
        e->animation_base_style = base;
    }
    if (web_dialog_is_modal(d,e)) s->backdrop = compute(c,e,NULL,PE_BACKDROP,false);
    if (e == d->html) c->rem = s->font_size;
    if (s->display != D_NONE && !(e->tag == T_img || e->tag == T_input || e->tag == T_br || e->tag == T_hr)) {
        if (has_before) {
            style_t *b = compute(c, e, s, PE_BEFORE, false);
            if (b->content && b->display != D_NONE) s->before = b;
        }
        if (has_after) {
            style_t *a = compute(c, e, s, PE_AFTER, false);
            if (a->content && a->display != D_NONE) s->after = a;
        }
    }
    if (!style_box_compatible(e, e->style_previous, s))
        __atomic_store_n(&d->style_boxes_changed, true, __ATOMIC_RELAXED);
    e->style_mem.trap = NULL;
descend:
    e->style_dirty = e->style_children_dirty = false;
    s = e->style;
    if (s->display == D_NONE) {
        motion_hide_subtree(e);
        clear_styles(e);
        return;
    }
    c->full = redo;
    pvec children = {0};
    doc_flat_children(e, &children);
    if (c->frontier) {
        for (int i = 0; i < children.n; i++) {
            node_t *ch = children.v[i];
            if (ch->type == N_ELEM) cascade_enqueue(c, ch, s);
            else ch->style = NULL;
        }
    } else if (!css_worker() && children.n >= 8 && d->sty.ctx->parallel_safe) {
        struct cascade_frontier f = {0};
        c->frontier = &f;
        for (int i = 0; i < children.n; i++) {
            node_t *ch = children.v[i];
            if (ch->type == N_ELEM) cascade_enqueue(c, ch, s);
            else ch->style = NULL;
        }
        c->frontier = NULL;
        struct cascade_batch batch = {*c, f.v};
        parallel_for_stage(PARALLEL_STYLE,f.n,cascade_worker,&batch,cascade_service,d);
        free(f.v);
    } else {
        for (int i = 0; i < children.n && !__atomic_load_n(&d->native_cancelled, __ATOMIC_ACQUIRE); i++) {
            node_t *ch = children.v[i];
            if (ch->type == N_ELEM) cascade_node(c, ch, s);
            else ch->style = NULL;
        }
    }
    pv_free(&children);
    c->full = inherited_full;
}

static uint32_t cascade_work_count(web_doc *d, node_t *root) {
    pvec nodes = {0}; pv_push(&nodes, root);
    /* Parent-first enumeration, followed by a reverse accumulation, avoids
       recursive counting on deeply nested flex/DOM chains. This is only done
       for full cascades; leaf updates never add another document-wide pass. */
    for (int i = 0; i < nodes.n; i++) {
        if (!web_native_checkpoint(d)) { pv_free(&nodes); return 0; }
        node_t *n = nodes.v[i]; n->style_subtree_work = n->type == N_ELEM;
        if (n->type == N_ELEM) doc_flat_children(n, &nodes);
    }
    for (int i = nodes.n - 1; i > 0; i--) {
        node_t *n = nodes.v[i], *parent = doc_flat_parent(n);
        if (!parent) continue;
        uint64_t sum = (uint64_t)parent->style_subtree_work + n->style_subtree_work;
        parent->style_subtree_work = sum > UINT32_MAX ? UINT32_MAX : (uint32_t)sum;
    }
    pv_free(&nodes); return root->style_subtree_work;
}

static void cascade_frontier_run(struct cascade *c, node_t *root) {
    uint32_t total = cascade_work_count(c->d, root);
    if (__atomic_load_n(&c->d->native_cancelled, __ATOMIC_ACQUIRE)) return;
    if (total < 128 || !c->d->sty.ctx->parallel_safe) { cascade_node(c, root, NULL); return; }
    struct cascade_frontier f = {0};
    c->frontier = &f; cascade_enqueue(c, root, NULL);
    /* Resolve common ancestors on the supervisor, splitting the largest
       remaining branch rather than stopping at the first eight siblings.
       This exposes a feed hidden under one large wrapper alongside tiny nav
       siblings. Every job inherits a finalized parent style/rem, and jobs are
       disjoint flat subtrees; no worker observes a half-computed ancestor. */
    uint32_t target = total / 32; if (target < 16) target = 16;
    while (f.n && !__atomic_load_n(&c->d->native_cancelled, __ATOMIC_ACQUIRE)) {
        size_t largest = 0;
        for (size_t i = 1; i < f.n; i++)
            if (f.v[i].node->style_subtree_work > f.v[largest].node->style_subtree_work) largest = i;
        if (f.v[largest].node->style_subtree_work <= target) break;
        struct cascade_job job = f.v[largest]; f.v[largest] = f.v[--f.n];
        c->rem = job.rem; c->full = job.full;
        cascade_node(c, job.node, job.parent);
    }
    c->frontier = NULL;
    struct cascade_batch batch = {*c, f.v};
    if (!__atomic_load_n(&c->d->native_cancelled, __ATOMIC_ACQUIRE))
        parallel_for_stage(PARALLEL_STYLE,f.n,cascade_worker,&batch,cascade_service,c->d);
    free(f.v);
}

void css_cascade(web_doc *d, int vw, int vh) {
    d->css_motion_now=uptime_ms(); /* exactly one supervisor snapshot per cascade */
    /* Direct native callers may recascade without the document publication
       helper. Do not overwrite a prior retirement generation while boxes
       still borrow it. The ordinary web_layout path releases after each join. */
    if(d->style_retired_first) {
        if(d->root_box){boxes_discard(d);d->need_boxes=true;d->layout_valid=false;}
        css_styles_release(d);
    }
    /* Keys borrow computed-style names. Never carry a previous document or
       style-arena generation into this cascade; release on normal exit too. */
    var_index_clear();
    font_bindings_clear();font_faces_available=false;
    for(int i=0;i<d->sty.sheets.n;i++)if(((sheet_t *)d->sty.sheets.v[i])->fonts){font_faces_available=true;break;}
    for(struct web_font_resource *f=d->fonts;f;f=f->next)f->wanted=false;
    bool scripting = web_js_enabled(d);
    /* Rule indexing depends on the stylesheet snapshot/media environment,
       not on the current hover/active target, class attribute or inline style.
       Those changes still recompute every real selector and computed style.
       Resource/CSSOM replacement unpublishes ctx before releasing its AST;
       new sheets mark index_dirty at their publication boundary. */
    if (!d->sty.ctx || d->sty.index_dirty || d->sty.index_w != vw || d->sty.index_h != vh ||
        d->sty.index_scripting != scripting || d->sty.index_quirks != d->quirks) {
        css_ctx_free(d->sty.ctx); d->sty.ctx = NULL;
        d->sty.ctx = build_index(d, vw, vh);
        d->sty.index_w = vw;
        d->sty.index_h = vh;
        d->sty.index_scripting = scripting;
        d->sty.index_quirks = d->quirks;
        d->sty.index_dirty = false;
        d->style_full_dirty = true;
    }
    if (!web_form_style_snapshot(d, d->sty.ctx->form_features)) {
        __atomic_store_n(&d->native_cancelled, true, __ATOMIC_RELEASE);
        return;
    }
    if (!setbits) { setbits = malloc((size_t)css_prop_count()); if(!setbits)ar_alloc(&d->smem,SIZE_MAX); }
    bool full = d->style_full_dirty || !d->style_pending_dirty || !d->html || !d->html->style;
    if(full && (d->need_boxes || !d->root_box) && d->root)
        clear_styles(d->root); /* retire styles of nodes no longer in the flat tree */
    d->style_boxes_changed = false;
    struct cascade c = {.d=d, .rem=d->html && d->html->style ? d->html->style->font_size : 16,
        .vw=vw, .vh=vh, .a=&d->smem, .full=full, .trap=d->smem.trap};
    if (d->html) {
        if (full) cascade_frontier_run(&c, d->html);
        else cascade_node(&c, d->html, NULL);
    }
    var_index_clear();
    font_bindings_clear();
    sb_free(&c.vbuf);
    d->styled_w = vw;
    d->styled_h = vh;
    d->style_full_dirty = d->style_pending_dirty = false;
}
