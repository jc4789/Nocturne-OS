/* CSS: parsing stylesheets (rules, @media, @import, @supports, nesting), selectors, an index of
   the rules by id, class and tag, and the cascade that computes every element's style. */
#include <stdio.h>
#include <ctype.h>
#include <stdarg.h>
#include <math.h>
#include <limits.h>
#include <nocturne.h>
#include <display.h>
#include <parallel.h>
#include "webi.h"
#include "animation.h"
#include "web_dialog.h"
#include "web_presentation.h"
#include "form_validation.h"
#include "form_value.h"
#include "avmedia.h"
#include "inspect_private.h"

/* Slot 2 is reserved for CSS. The supervisor and each subtree worker own
   independent declaration/variable scratch; rule indexes and DOM are frozen. */
struct dent;
struct css_scratch {
    struct dent *declarations;
    int declaration_count, declaration_capacity;
    uint8_t *properties;
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

/* ---------------------------------------------------------------- data structures */
enum { SK_TAG, SK_ID, SK_CLASS, SK_ATTR, SK_PC, SK_SLOTTED, SK_ANCHOR };
_Static_assert(PE_PLACEHOLDER != PE_SLOTTED, "pseudo-element overlaps assigned-element selector marker");
enum { AO_EXISTS, AO_EQ, AO_INCL, AO_DASH, AO_PREFIX, AO_SUFFIX, AO_SUBSTR };
enum { PC_FIRST_CHILD, PC_LAST_CHILD, PC_ONLY_CHILD, PC_NTH_CHILD, PC_NTH_LAST_CHILD, PC_FIRST_OF_TYPE,
       PC_LAST_OF_TYPE, PC_ONLY_OF_TYPE, PC_NTH_OF_TYPE, PC_NTH_LAST_OF_TYPE, PC_NOT, PC_IS, PC_NEVER,
       PC_ALWAYS, PC_LINK, PC_CHECKED, PC_DISABLED, PC_ENABLED, PC_ROOT, PC_EMPTY, PC_REQUIRED, PC_OPTIONAL,
       PC_LANG, PC_PLACEHOLDER_SHOWN, PC_READ_WRITE, PC_READ_ONLY, PC_OPEN, PC_HOST, PC_FOCUS, PC_FOCUS_WITHIN, PC_HOVER, PC_ACTIVE,
       PC_VALID, PC_INVALID, PC_IN_RANGE, PC_OUT_OF_RANGE, PC_INDETERMINATE, PC_MODAL,PC_FULLSCREEN, PC_HAS, PC_SCOPE };

struct sellist;
struct simple {
    uint8_t kind, op, pc;
    bool icase;
    uint16_t tag;
    const char *name;
    union {
        const char *value; /* [attr] and :lang() */
        struct { int a, b; }; /* :nth-*(An+B) */
    };
    struct sellist *args;
};
struct compound {
    struct compound *left;
    int n;
    char comb; /* how it relates to the compound on its left: ' ', '>', '+', '~' */
    bool universal; /* share the existing padding before the array */
    struct simple s[];
};
/* Up to four ids, classes or type names that some ancestor of the subject
   must carry (from compounds joined to it through a descendant or child
   combinator), hashed by ancestor_feature_hash; 0 ends the list. Only an
   index entry keeps them: the selectors inside :is() and :has() never use them. */
#define SELECTOR_ANCESTOR_HASHES 4
struct selector {
    struct compound *right;
    uint32_t spec;
    uint8_t pseudo;
};
struct sellist {
    int n;
    struct selector *v[];
};

struct decl {
    const struct propdef *p; /* NULL: a custom property */
    const char *name;        /* custom properties */
    const char *value;
    bool important;
};

/* One immutable parsed block per live inline style/effect, not one per
   cascade. Compare owned bytes as well as length: DOM/CSSOM may reuse an
   address, and a changed document base changes relative URL resolution. */
struct css_declarations {
    arena_t arena;
    const char *text, *base;
    size_t length;
    struct decl *values;
    int count;
};

/* Fixed computed styles share slabs. Only variable values use node arenas.
   Slot ownership follows the cascade document, not DOM allocation ownership;
   an adopted/detached node can outlive that document, so orphaned pools stay
   alive until their last current/retired slot has been returned. */
struct css_style_slot {
    style_t style;
    struct css_style_slot *next;
    struct css_style_pool *pool;
};
enum { CSS_STYLE_SLAB_SLOTS = 64 };
struct css_style_slab {
    struct css_style_slab *next;
    struct css_style_slot slots[CSS_STYLE_SLAB_SLOTS];
};
struct css_style_pool {
    struct css_style_slab *slabs;
    struct css_style_slot *free;
    size_t live;
    unsigned lock;
    bool owned;
};
static void style_pool_lock(struct css_style_pool *p) {
    while (__atomic_exchange_n(&p->lock, 1, __ATOMIC_ACQUIRE)) __asm__ volatile("pause");
}
static void style_pool_unlock(struct css_style_pool *p) {
    __atomic_store_n(&p->lock, 0, __ATOMIC_RELEASE);
}
static void style_pool_destroy(struct css_style_pool *p) {
    while (p->slabs) {
        struct css_style_slab *slab = p->slabs; p->slabs = slab->next; free(slab);
    }
    free(p);
}
static style_t *style_slot_alloc(web_doc *d, node_t *n, arena_t *tail) {
    struct css_style_pool *p = d->style_pool;
    style_pool_lock(p);
    if (!p->free) {
        struct css_style_slab *slab = malloc(sizeof *slab);
        if (!slab) { style_pool_unlock(p); ar_alloc(tail, SIZE_MAX); return NULL; }
        slab->next = p->slabs; p->slabs = slab;
        for (size_t i = 0; i < CSS_STYLE_SLAB_SLOTS; i++) {
            slab->slots[i].pool = p;
            slab->slots[i].next = p->free; p->free = &slab->slots[i];
        }
    }
    struct css_style_slot *slot = p->free; p->free = slot->next; p->live++;
    style_pool_unlock(p);
    slot->next = n->style_slots; n->style_slots = slot;
    return &slot->style; /* css_style_init resets the complete POD body */
}
static void style_slots_free(struct css_style_slot **head) {
    while (*head) {
        struct css_style_slot *slot = *head; *head = slot->next;
        struct css_style_pool *p = slot->pool;
        style_pool_lock(p);
        slot->next = p->free; p->free = slot; p->live--;
        bool destroy = !p->owned && !p->live;
        style_pool_unlock(p);
        if (destroy) style_pool_destroy(p);
    }
}
void css_style_doc_free(web_doc *d) {
    struct css_style_pool *p = d->style_pool; d->style_pool = NULL;
    if (!p) return;
    style_pool_lock(p); p->owned = false;
    bool destroy = !p->live;
    style_pool_unlock(p);
    if (destroy) style_pool_destroy(p);
}
static void declarations_free(struct css_declarations **slot) {
    struct css_declarations *cache = *slot; *slot = NULL;
    if (cache) { ar_free(&cache->arena); free(cache); }
}

struct mcond {
    const char *q;
    const struct mcond *up;
    const char *name; /* escaped, case-sensitive container identifier */
    uint8_t axes;    /* 1 = inline, 2 = both (horizontal writing mode) */
    bool container;
};

/* resource: a FontFace's own source, resolved when its source was set (the
   document owns it); NULL for an @font-face rule, found by url on demand. */
struct font_source {const char *url;struct font_source *next;struct web_font_resource *resource;};
struct font_face {
    const char *family;
    struct font_source *sources;
    const struct web_font_range *ranges;
    size_t range_count;
    unsigned weight_lo,weight_hi;
    bool italic;
    const struct mcond *media;
    struct font_face *next;
    const struct css_layer_decl *layer;
    /* CSS Font Loading: the FontFace identity (never reused), the CSSOM
       serialization of each descriptor (enum css_font_descriptor) its
       attributes reflect, and the FontFace record when no rule owns it. */
    uint64_t id;
    const char *desc[CSS_FONT_DESCRIPTORS];
    struct css_font_face *author;
};
/* A FontFace the page constructed (CSS Font Loading 2.1). Its JS wrapper holds
   one reference and every document font source (document.fonts) holding it
   another; selection reads it only through those documents' author_fonts. */
struct css_font_face {
    struct font_face face;
    unsigned refs;
    bool started; /* load() or a rendering use: as if load() had been called */
    char *names[CSS_FONT_DESCRIPTORS]; /* face.desc's malloc storage */
    char *family;                      /* the literal family name it matches */
    struct web_font_range *ranges;     /* face.ranges, sorted and merged */
    struct web_font_resource *data;    /* decoded BufferSource, document-owned */
};

/* Immutable syntax identities. Resolved layer ordering belongs to each
   document/scope index, never a shared/adopted stylesheet AST. */
struct css_layer_decl {
    const char *name;
    struct css_layer_decl *parent;
    size_t id;
};
struct css_layer_event {
    struct css_layer_event *next;
    const struct css_layer_decl *layer;
    const struct mcond *media;
    struct css_rule_info *import;
};
struct rule {
    struct sellist *sel;
    struct decl *decls;
    int ndecls;
    const struct mcond *media;
    struct rule *next;
    const char *selector_text;
    const struct css_layer_decl *layer;
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
    const struct css_layer_decl *layer;
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
    uint32_t import_urls; /* rules above with an import_url: a rescan skips the walk when 0 */
    const char *cssom_source;
    size_t cssom_length;
    struct css_layer_event *layers_first,*layers_last;
    size_t layer_count;
    /* Instance-only import topology preserves declaration encounters before
       imported rules, although the style vector uses import postorder. */
    sheet_t *import_parent,*import_first,*import_last,*import_next;
    const struct css_rule_info *import_owner;
    const char *href; /* instance request URL for DevTools; NULL: inline */
    /* The parsed AST every instance shares, the arena that owns it, its
       identity across rescans (never reused, unlike its address), and its
       rule index, built at its first cascade. */
    sheet_t *ast;
    arena_t *arena;
    uint64_t serial;
    struct css_index *index;
};

struct pctx {
    arena_t *a;
    sheet_t *sh;
    const char *base;
    pvec *imports;
    int nimports;
    bool supports_probe; /* reject accepted-but-unimplemented selector fallbacks */
    unsigned rule_depth;
    bool in_has;
    struct css_layer_decl *layer;
    bool import_allowed;
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
    {"enabled", PC_ENABLED}, {"root", PC_ROOT}, {"scope", PC_SCOPE}, {"empty", PC_EMPTY},
    {"required", PC_REQUIRED}, {"optional", PC_OPTIONAL}, {"placeholder-shown", PC_PLACEHOLDER_SHOWN},
    {"read-write", PC_READ_WRITE}, {"read-only", PC_READ_ONLY}, {"open", PC_OPEN}, {"defined", PC_ALWAYS},
    {"visited", PC_NEVER}, {"hover", PC_HOVER}, {"active", PC_ACTIVE}, {"focus", PC_FOCUS},
    {"focus-within", PC_FOCUS_WITHIN}, {"focus-visible", PC_NEVER}, {"target", PC_NEVER}, {"target-within", PC_NEVER},
    {"indeterminate", PC_INDETERMINATE}, {"invalid", PC_INVALID}, {"valid", PC_VALID}, {"user-invalid", PC_NEVER},
    {"user-valid", PC_NEVER}, {"default", PC_NEVER}, {"fullscreen", PC_FULLSCREEN}, {"modal", PC_MODAL},
    {"popover-open", PC_NEVER}, {"autofill", PC_NEVER}, {"in-range", PC_IN_RANGE}, {"out-of-range", PC_OUT_OF_RANGE},
    {"playing", PC_NEVER}, {"paused", PC_NEVER}, {"current", PC_NEVER}, {"past", PC_NEVER}, {"future", PC_NEVER},
    {"first", PC_NEVER}, {"left", PC_NEVER}, {"right", PC_NEVER}, {"blank", PC_NEVER}, {"local-link", PC_NEVER},
    {"host", PC_HOST}, {"state", PC_NEVER}, {"picture-in-picture", PC_NEVER}, {"-webkit-autofill", PC_NEVER},
    {"-moz-focusring", PC_NEVER}, {"-moz-ui-invalid", PC_NEVER},
};

/* one compound selector; *ps advanced past it. pseudo receives a pseudo-element. */
static struct sellist *parse_relative_list(struct pctx *pc, const char *s, const char *e);
/* The growing lists of a selector start on the stack and reach the arena
   only at their final length, in the array that ends their compound or list:
   an arena keeps every capacity it ever gave. The stack parts stay small,
   since :is() and :has() nest these frames; a longer list grows in the arena. */
static void *selector_list_grow(struct pctx *pc, const void *list, size_t n, size_t *capacity, size_t size) {
    if (*capacity > INT_MAX / 2 || *capacity > SIZE_MAX / (2 * size)) ar_alloc(pc->a, SIZE_MAX);
    *capacity *= 2;
    void *next = ar_alloc(pc->a, *capacity * size);
    memcpy(next, list, n * size);
    return next;
}
static struct compound *parse_compound(struct pctx *pc, const char **ps, const char *e, uint32_t *spec, uint8_t *pseudo) {
    struct simple local[4], *tmp = local;
    size_t capacity = sizeof local / sizeof *local;
    int n = 0;
    const char *s = *ps;
    bool type_seen = false, universal = false;
    while (s < e) {
        if ((size_t)n == capacity) tmp = selector_list_grow(pc, tmp, (size_t)n, &capacity, sizeof *tmp);
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
                if (pc->in_has) return NULL; /* no :has-allowed pseudo-elements are defined */
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
                else if (!strcmp(name, "placeholder")) {
                    if (args) return NULL;
                    *pseudo = PE_PLACEHOLDER;
                }
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
                    if (pc->in_has) return NULL;
                    pc->in_has = true;
                    x->args = parse_relative_list(pc, args, args_end);
                    pc->in_has = false;
                    if (!x->args) return NULL;
                    x->pc = PC_HAS;
                    *spec = spec_add(*spec, sellist_max_spec(x->args));
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
            if (pc->supports_probe && (x->pc == PC_NEVER || x->pc == PC_ALWAYS)) return NULL;
            *spec = spec_add(*spec, SPEC_B);
            n++;
            continue;
        }
        break;
    }
    if (s == *ps) return NULL;
    *ps = s;
    struct compound *cp = ar_alloc(pc->a, sizeof *cp + (size_t)n * sizeof *tmp);
    cp->n = n;
    cp->universal = universal;
    memcpy(cp->s, tmp, (size_t)n * sizeof *tmp);
    return cp;
}

/* ---------------------------------------------------------------- the ancestor filter
   A Bloom filter over the ids, classes and type names of an element's
   ancestors. A selector whose ancestor compounds require a feature that is
   absent cannot match, and is rejected without walking the ancestor chain.
   The cascade builds it along the flat tree: a superset of every chain that
   selector_parent() follows (a slotted element's flat ancestors include its
   DOM parent, the host), so it never rejects a selector that can match.
   A walk counts each element's features in on the way down and out on the
   way up; a queued subtree job gets a bit snapshot of its ancestors, and
   counts only what lies below its root. A saturated count stays set. */
#define ANCESTOR_FILTER_BITS 2048
struct ancestor_filter { uint64_t bits[ANCESTOR_FILTER_BITS / 64]; };
struct ancestor_counts { uint8_t count[ANCESTOR_FILTER_BITS]; };

static uint32_t ancestor_feature_hash(char kind, const char *name, size_t n) {
    uint32_t h = (2166136261u ^ (unsigned char)kind) * 16777619u;
    for (size_t i = 0; i < n; i++) h = (h ^ (unsigned char)name[i]) * 16777619u;
    return h ? h : 1;
}
static uint32_t ancestor_tag_hash(uint16_t tag) {
    char id[2] = {(char)(tag & 255), (char)(tag >> 8)};
    return ancestor_feature_hash('=', id, 2);
}
static void ancestor_count(struct ancestor_counts *counts, uint32_t h, int delta) {
    unsigned at[2] = {h & (ANCESTOR_FILTER_BITS - 1), (h >> 11) & (ANCESTOR_FILTER_BITS - 1)};
    for (int i = 0; i < 2; i++) {
        uint8_t *count = &counts->count[at[i]];
        if (*count != UINT8_MAX) *count = (uint8_t)(*count + delta);
    }
}
/* delta +1 before styling e's flat children, -1 after them. */
static void ancestor_counts_apply(struct ancestor_counts *counts, const node_t *e, int delta) {
    if (!counts || e->type != N_ELEM) return;
    if (e->name) ancestor_count(counts, ancestor_feature_hash('<', e->name, strlen(e->name)), delta);
    if (e->tag) ancestor_count(counts, ancestor_tag_hash(e->tag), delta);
    if (e->id) ancestor_count(counts, ancestor_feature_hash('#', e->id, strlen(e->id)), delta);
    for (int i = 0; i < e->nclasses; i++)
        ancestor_count(counts, ancestor_feature_hash('.', e->classes[i], strlen(e->classes[i])), delta);
}
static void ancestor_snapshot(struct ancestor_filter *out, const struct ancestor_filter *base, const struct ancestor_counts *counts) {
    if (base) *out = *base; else memset(out, 0, sizeof *out);
    for (unsigned i = 0; counts && i < ANCESTOR_FILTER_BITS; i++)
        if (counts->count[i]) out->bits[i / 64] |= 1ull << (i % 64);
}
static bool ancestor_bit(const struct ancestor_filter *base, const struct ancestor_counts *counts, unsigned at) {
    return (base && (base->bits[at / 64] >> (at % 64) & 1)) || counts->count[at];
}
/* Hashes exactly what match_compound() compares: ids and classes by their
   exact names, a type by its tag number or, for an unknown type, its name.
   A compound left of ' ' or '>' is an ancestor of the subject (siblings share
   ancestors); one left of '+' or '~' is a sibling and adds nothing, nor do
   the arguments of :is()/:not()/:has(). */
static void selector_ancestor_hashes(const struct selector *sel, uint32_t hashes[SELECTOR_ANCESTOR_HASHES]) {
    if (sel->pseudo == PE_SLOTTED) return; /* its left compounds match the slot's ancestors */
    int n = 0;
    for (int pass = 0; pass < 2; pass++) {
        for (const struct compound *c = sel->right; c && c->left && n < SELECTOR_ANCESTOR_HASHES; c = c->left) {
            if (c->comb != ' ' && c->comb != '>') continue;
            const struct compound *a = c->left;
            for (int i = 0; i < a->n && n < SELECTOR_ANCESTOR_HASHES; i++) {
                const struct simple *x = &a->s[i];
                uint32_t h = 0;
                if (pass == 0 && (x->kind == SK_ID || x->kind == SK_CLASS))
                    h = ancestor_feature_hash(x->kind == SK_ID ? '#' : '.', x->name, strlen(x->name));
                else if (pass == 1 && x->kind == SK_TAG)
                    h = x->tag ? ancestor_tag_hash(x->tag) : ancestor_feature_hash('<', x->name, strlen(x->name));
                if (h) hashes[n++] = h;
            }
        }
    }
}
/* The filter of a walk: its job's snapshot (or none) and its own counts. */
struct ancestor_view { const struct ancestor_filter *base; const struct ancestor_counts *counts; };
static bool ancestor_filter_rejects(const struct ancestor_view *v, const uint32_t hashes[SELECTOR_ANCESTOR_HASHES]) {
    if (!v || !v->counts) return false;
    for (int i = 0; i < SELECTOR_ANCESTOR_HASHES && hashes[i]; i++) {
        uint32_t h = hashes[i];
        if (!ancestor_bit(v->base, v->counts, h & (ANCESTOR_FILTER_BITS - 1)) ||
            !ancestor_bit(v->base, v->counts, (h >> 11) & (ANCESTOR_FILTER_BITS - 1))) return true;
    }
    return false;
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
        if (sel->pseudo || (cur && !pending)) return NULL;
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
    if (!forgiving) {
        const char *last = e;
        trim_r(s, &last);
        if (last == s || last[-1] == ',') return NULL;
    }
    struct selector *local[8], **tmp = local;
    size_t capacity = sizeof local / sizeof *local;
    int n = 0;
    while (s < e) {
        const char *c = scan_to(s, e, ",");
        struct selector *x = parse_complex(pc, s, c);
        if (x) {
            if ((size_t)n == capacity) tmp = selector_list_grow(pc, tmp, (size_t)n, &capacity, sizeof *tmp);
            tmp[n++] = x;
        } else if (!forgiving) return NULL;
        s = c < e ? c + 1 : e;
    }
    if (!n && !forgiving) return NULL;
    struct sellist *l = ar_alloc(pc->a, sizeof *l + (size_t)n * sizeof *tmp);
    l->n = n;
    memcpy(l->v, tmp, (size_t)n * sizeof *tmp);
    return l;
}

/* Relative selectors retain an internal, zero-specificity anchor. It is not
   :scope: a nested :scope still refers to the API's scoping root. */
static struct sellist *parse_relative_list(struct pctx *pc, const char *s, const char *e) {
    const char *last = e; trim_r(s, &last);
    if (s == last || last[-1] == ',') return NULL;
    struct selector *local[8], **v = local;
    size_t capacity = sizeof local / sizeof *local; int n = 0;
    while (s < e) {
        const char *end = scan_to(s, e, ","), *p = skip_ws(s, end);
        char combinator = ' ';
        if (p < end && strchr(">+~", *p)) combinator = *p++;
        struct selector *selector = parse_complex(pc, p, end);
        if (!selector || selector->pseudo) return NULL;
        struct compound *first = selector->right;
        while (first->left) first = first->left;
        struct compound *anchor = ar_alloc(pc->a, sizeof *anchor + sizeof *anchor->s);
        anchor->n = 1; anchor->s[0].kind = SK_ANCHOR;
        first->left = anchor; first->comb = combinator;
        if ((size_t)n == capacity) v = selector_list_grow(pc, v, (size_t)n, &capacity, sizeof *v);
        v[n++] = selector; s = end < e ? end + 1 : e;
    }
    struct sellist *list = ar_alloc(pc->a, sizeof *list + (size_t)n * sizeof *v);
    memcpy(list->v, v, (size_t)n * sizeof *v); list->n = n;
    return list;
}

/* ---------------------------------------------------------------- matching */
struct selector_match { node_t *shadow, *reference, *anchor; };
static bool match_list_bound(const struct sellist *l, node_t *e, const struct selector_match *context);
static bool match_from_bound(const struct compound *c, node_t *e, const struct selector_match *context);
static bool match_has(const struct sellist *list, node_t *subject, const struct selector_match *context);

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
static bool match_pc(const struct simple *x, node_t *e, const struct selector_match *context) {
    node_t *scope = context->shadow;
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
    case PC_NOT: return !match_list_bound(x->args, e, context);
    case PC_IS: return match_list_bound(x->args, e, context);
    case PC_HAS: return match_has(x->args, e, context);
    case PC_SCOPE: return context->reference ? e == context->reference : e->parent && e->parent->type == N_DOC;
    case PC_HOST: {
        struct selector_match host = *context; host.shadow = NULL;
        return scope && scope->shadow_host == e && (!x->args || match_list_bound(x->args, e, &host));
    }
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
    case PC_FULLSCREEN: return web_presentation_matches(e);
    case PC_OPEN: return (e->tag == T_details || e->tag == T_dialog) && node_attr(e, "open");
    }
    return false;
}

static bool match_compound(const struct compound *c, node_t *e, const struct selector_match *context) {
    node_t *scope = context->shadow;
    if (scope && e == scope->shadow_host) {
        if (!c->n || c->universal) return false; /* '*' cannot select a featureless host */
        for (int i = 0; i < c->n; i++)
            if (c->s[i].kind != SK_PC || c->s[i].pc != PC_HOST) return false;
    }
    for (int i = 0; i < c->n; i++) {
        const struct simple *x = &c->s[i];
        switch (x->kind) {
        case SK_ANCHOR:
            if (e != context->anchor) return false;
            break;
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
            if (!match_pc(x, e, context)) return false;
            break;
        case SK_SLOTTED: break; /* matched against its originating slot below */
        }
    }
    return true;
}

static bool match_from_bound(const struct compound *c, node_t *e, const struct selector_match *context) {
    node_t *scope = context->shadow;
    if (!e || e->type != N_ELEM) return false;
    if (e) {
        if (css_can_service()) { if (!web_native_checkpoint(e->owner)) return false; }
        else if (__atomic_load_n(&e->owner->native_cancelled, __ATOMIC_ACQUIRE)) return false;
    }
    for (int i = 0; i < c->n; i++) if (c->s[i].kind == SK_SLOTTED) {
        struct selector_match slotted = *context; slotted.shadow = NULL;
        if (!scope || !match_list_bound(c->s[i].args, e, &slotted)) return false;
        node_t *slot = doc_assigned_slot(e, false);
        /* A slot assigned into another slot can expose the same flattened
           element. Follow assignment links without changing DOM parents. */
        for (; slot; slot = doc_assigned_slot(slot, false))
            if (doc_node_root(slot, false) == scope && match_compound(c, slot, context)) break;
        if (!slot) return false;
        e = slot;
        break;
    }
    if (!match_compound(c, e, context)) return false;
    if (!c->left) return true;
    switch (c->comb) {
    case '>': {
        node_t *p = selector_parent(e, scope);
        return p && p->type == N_ELEM && match_from_bound(c->left, p, context);
    }
    case ' ':
        for (node_t *p = selector_parent(e, scope); p && p->type == N_ELEM; p = selector_parent(p, scope))
            if (match_from_bound(c->left, p, context)) return true;
        return false;
    case '+': {
        node_t *p = prev_elem(e);
        return p && match_from_bound(c->left, p, context);
    }
    case '~':
        for (node_t *p = prev_elem(e); p; p = prev_elem(p))
            if (match_from_bound(c->left, p, context)) return true;
        return false;
    }
    return false;
}

static bool match_list_bound(const struct sellist *l, node_t *e, const struct selector_match *context) {
    for (int i = 0; l && i < l->n; i++)
        if (!l->v[i]->pseudo && match_from_bound(l->v[i]->right, e, context)) return true;
    return false;
}

static bool match_from(const struct compound *c, node_t *e, node_t *scope) {
    const struct selector_match context = {.shadow = scope};
    return match_from_bound(c, e, &context);
}

/* Search only the relative selector's reachable DOM region. '+ a + b' may
   terminate past the immediate sibling, so sibling arguments inspect the
   following forest. The anchor compound supplies the exact combinator test.
   No flat-tree/shadow traversal or generated boxes participates in :has(). */
static bool match_has(const struct sellist *list, node_t *subject, const struct selector_match *context) {
    struct selector_match relative = *context; relative.anchor = subject;
    for (int i = 0; list && i < list->n; i++) {
        const struct compound *right = list->v[i]->right, *first = right;
        while (first->left && first->left->left) first = first->left;
        bool siblings = first->comb == '+' || first->comb == '~';
        /* One compound after a combinator other than a descendant one selects
           the children, the next sibling or the following siblings alone. */
        bool level = first == right && first->comb != ' ';
        node_t *root = siblings ? subject->parent : subject;
        node_t *node = siblings ? next_elem(subject) : subject->first;
        for (; node;) {
            if (node->type == N_ELEM && match_from_bound(right, node, &relative)) return true;
            if (level) {
                if (first->comb == '+') break;
                node = node->next;
                continue;
            }
            if (node->first) node = node->first;
            else {
                while (node && node != root && !node->next) node = node->parent;
                if (!node || node == root) break;
                node = node->next;
            }
            if (__atomic_load_n(&subject->owner->native_cancelled, __ATOMIC_ACQUIRE)) return false;
        }
    }
    return false;
}

/* DOM queries use exactly the cascade's parser and matcher, with a short-lived
   bounded arena so repeated querySelector calls do not grow the document arena. */
static void select_walk(node_t *scope, const struct sellist *sel, pvec *out) {
    const struct selector_match context = {.reference = scope->type == N_DOC ? scope->owner->html : scope};
    for (node_t *n = scope->first; n;) {
        if (n->type == N_ELEM && match_list_bound(sel, n, &context)) pv_push(out, n);
        if (n->first) n = n->first;
        else {
            while (n != scope && !n->next) n = n->parent;
            if (n == scope) break;
            n = n->next;
        }
    }
}

/* DOM "parse a selector" from a string (querySelector, matches, closest): CSS Syntax drops
   comments while tokenizing, so they go before the selector grammar sees the text. A comment
   between two code points of one name (".a", a comment, "b") separates two tokens that the text
   would join into one, and a selector never has two such tokens side by side, so that is a
   failure. Strings and escapes keep their contents. */
static struct sellist *parse_selector_string(struct pctx *p, const char *selector) {
    size_t n = strlen(selector), k = 0;
    char *text = ar_alloc(p->a, n + 1);
    for (size_t i = 0; i < n;) {
        char c = selector[i];
        if (c == '/' && i + 1 < n && selector[i + 1] == '*') {
            i += 2;
            while (i < n && !(selector[i] == '*' && i + 1 < n && selector[i + 1] == '/')) i++;
            i = i < n ? i + 2 : n;
            if (k && i < n && (ident_char((unsigned char)text[k - 1]) || text[k - 1] == '\\') &&
                (ident_char((unsigned char)selector[i]) || selector[i] == '\\')) return NULL;
            continue;
        }
        if (c == '\\' && i + 1 < n) { text[k++] = selector[i++]; text[k++] = selector[i++]; continue; }
        if (c == '"' || c == '\'') {
            text[k++] = selector[i++];
            while (i < n && selector[i] != c) {
                if (selector[i] == '\\' && i + 1 < n) text[k++] = selector[i++];
                text[k++] = selector[i++];
            }
            if (i < n) text[k++] = selector[i++];
            continue;
        }
        text[k++] = selector[i++];
    }
    while (k && is_space((unsigned char)text[k - 1])) k--;
    text[k] = 0;
    if (!k || text[k - 1] == ',') return NULL;
    return parse_sellist(p, text, text + k, false);
}

bool css_select(web_doc *d, node_t *scope, const char *selector, pvec *out) {
    (void)d;
    if (!selector || !scope) return false;
    arena_t *arena = calloc(1, sizeof *arena);
    if (!arena) return false;
    jmp_buf trap;
    arena->trap = &trap;
    if (setjmp(trap)) { ar_free(arena); free(arena); return false; }
    struct pctx p = {.a = arena};
    struct sellist *sel = parse_selector_string(&p, selector);
    if (sel) select_walk(scope, sel, out);
    bool valid = sel != NULL;
    ar_free(arena);
    free(arena);
    return valid;
}

bool css_matches(node_t *node, const char *selector, bool *valid) {
    if (valid) *valid = false;
    if (!node || node->type != N_ELEM || !selector) return false;
    arena_t *arena = calloc(1, sizeof *arena);
    if (!arena) return false;
    jmp_buf trap;
    arena->trap = &trap;
    if (setjmp(trap)) { ar_free(arena); free(arena); return false; }
    struct pctx p = {.a = arena};
    struct sellist *sel = parse_selector_string(&p, selector);
    if (valid) *valid = sel != NULL;
    const struct selector_match context = {.reference = node};
    bool matched = sel && match_list_bound(sel, node, &context);
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

static bool declaration_valid(arena_t *a, const char *property, size_t pn, const char *v, size_t vn, bool probe) {
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
    return has || (probe ? css_value_supported(p, v, vn) : css_value_valid(p, v, vn));
}
static bool supports_decl(arena_t *a,const char *property,size_t pn,const char *v,size_t vn) {
    return declaration_valid(a,property,pn,v,vn,true);
}
static bool declaration_parse(const char *property, size_t pn, const char *value, size_t vn, bool probe) {
    if (!property || !value || pn == SIZE_MAX || vn == SIZE_MAX) return false;
    arena_t *a = calloc(1, sizeof *a);
    if (!a) return false;
    jmp_buf trap;
    a->trap = &trap;
    if (setjmp(trap)) { ar_free(a); free(a); return false; }
    size_t n;
    char *v = supports_clean(a, value, vn, &n);
    bool result = v && declaration_valid(a, property, pn, v, n, probe);
    ar_free(a);
    free(a);
    return result;
}
bool css_supports_declaration(const char *property,size_t pn,const char *value,size_t vn) {
    return declaration_parse(property,pn,value,vn,true);
}
bool css_valid_declaration(const char *property,size_t pn,const char *value,size_t vn) {
    return declaration_parse(property,pn,value,vn,false);
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
static struct css_layer_decl *layer_name_parse(struct pctx *pc,const char *s,const char *e,bool anonymous) {
    struct css_layer_decl *parent=pc->layer;
    if(s==e&&!anonymous)return NULL;
    do {
        const char *name=NULL;
        if(s<e) {
            const char *start=s;if(*start=='-')start++;
            if(start==e || (*start!='-' && *start!='_' && *start!='\\' && !isalpha((unsigned char)*start) && (unsigned char)*start<0x80))return NULL;
            name=read_ident(pc->a,&s,e,false);
            if(!name || !strcmp(name,"-") || !strcasecmp(name,"initial") || !strcasecmp(name,"inherit") ||
               !strcasecmp(name,"unset") || !strcasecmp(name,"revert") || !strcasecmp(name,"revert-layer"))return NULL;
        }
        struct css_layer_decl *layer=ar_alloc(pc->a,sizeof *layer);
        if(pc->sh->layer_count==SIZE_MAX)ar_alloc(pc->a,SIZE_MAX);
        layer->name=name;layer->parent=parent;layer->id=pc->sh->layer_count++;parent=layer;
        if(s==e)break;
        if(*s++!='.' || s==e)return NULL; /* no intervening whitespace */
    } while(s<e);
    return parent;
}
static void layer_event(struct pctx *pc,const struct css_layer_decl *layer,const struct mcond *media,struct css_rule_info *import) {
    struct css_layer_event *event=ar_alloc(pc->a,sizeof *event);
    event->layer=layer;event->media=media;event->import=import;
    if(pc->sh->layers_last)pc->sh->layers_last->next=event;else pc->sh->layers_first=event;
    pc->sh->layers_last=event;
}
static bool layer_statement(struct pctx *pc,const char *s,const char *e,const struct mcond *media) {
    struct css_layer_event *first=NULL,*last=NULL;
    do {
        const char *end=scan_to(s,e,","),*start=skip_ws(s,end),*finish=end;trim_r(start,&finish);
        struct css_layer_decl *layer=layer_name_parse(pc,start,finish,false);if(!layer)return false;
        struct css_layer_event *event=ar_alloc(pc->a,sizeof *event);event->layer=layer;event->media=media;
        if(last)last->next=event;else first=event;last=event;
        if(end==e)break;s=end+1;if(s==e)return false;
    } while(s<e);
    if(pc->sh->layers_last)pc->sh->layers_last->next=first;else pc->sh->layers_first=first;
    pc->sh->layers_last=last;return true;
}
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
                const char *ns=s+1;const char *name=read_ident(pc->a,&ns,p,true);
                if(pc->sh && name&&!strcmp(name,"layer") && p<e&&*p==';')layer_statement(pc,skip_ws(ns,p),p,media);
                s = p < e ? p + 1 : e;
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
                        struct css_layer_decl *previous=pc->layer;
                        if(!strcmp(name,"layer")) {
                            pc->layer=layer_name_parse(pc,qs,qe,true);
                            if(!pc->layer){s=be<e?be+1:e;continue;}
                            layer_event(pc,pc->layer,media,NULL);
                        }
                        static const char amp[] = "&";
                        parse_style_rule(pc, amp, amp + 1, p + 1, be, m, sel);
                        pc->layer=previous;
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

static struct css_declarations *declarations_get(struct css_declarations **slot,
                                                const char *text, const char *base, arena_t *failure) {
    text = text ? text : ""; base = base ? base : "about:blank";
    size_t length = strlen(text);
    struct css_declarations *old = *slot;
    if (old && old->length == length && !memcmp(old->text, text, length) && !strcmp(old->base, base)) return old;
    if (!length) { declarations_free(slot); return NULL; }
    struct css_declarations *next = calloc(1, sizeof *next);
    if (!next) { ar_alloc(failure, SIZE_MAX); return NULL; }
    jmp_buf trap; next->arena.trap = &trap; next->arena.chunk_size = 512;
    if (setjmp(trap)) {
        ar_free(&next->arena); free(next); ar_alloc(failure, SIZE_MAX); return NULL;
    }
    next->length = length;
    next->text = ar_strndup(&next->arena, text, length);
    next->base = ar_strdup(&next->arena, base);
    struct pctx pc = {.a=&next->arena, .base=next->base};
    parse_body(&pc, next->text, next->text + length, NULL, NULL, &next->values, &next->count);
    next->arena.trap = NULL;
    declarations_free(slot); *slot = next;
    return next;
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
    r->layer=pc->layer;
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
    if (pc->sh->cssom_count == UINT32_MAX || (size_t)(e - s) > UINT32_MAX) {
        if (pc->a->trap) longjmp(*pc->a->trap, 1);
        abort();
    }
    struct css_rule_info *info = ar_alloc(pc->a, sizeof *info);
    /* The comment-free source lives as long as the sheet: no copy of the rule. */
    info->type = type; info->text = s; info->text_len = (uint32_t)(e - s);
    if (pc->sh->cssom_last) pc->sh->cssom_last->next = info;
    else pc->sh->cssom_first = info;
    pc->sh->cssom_last = info; pc->sh->cssom_count++; return info;
}

static bool font_source_hints(struct pctx *pc,const char *s,const char *e) {
    bool format_seen=false;
    while((s=skip_ws(s,e))<e) {
        const char *name=read_ident(pc->a,&s,e,true);
        if(!name || s==e || *s!='(')return false;
        const char *value=skip_ws(s+1,e),*end=scan_to(value,e,")");
        if(end==e)return false;
        /* Unsupported technology must skip this item before any download;
           accepting a font container does not implement shaping/variations. */
        if(strcmp(name,"format") || format_seen)return false;
        format_seen=true;
        const char *finish=end;trim_r(value,&finish);
        const char *format=NULL;
        if(finish-value>=2 && (*value=='"' || *value=='\'') && finish[-1]==*value) {
            sbuf b={0};css_unescape(&b,value+1,(size_t)(finish-value-2));
            format=ar_strndup(pc->a,b.p?b.p:"",b.n);sb_free(&b);
        }else {
            const char *cursor=value;format=read_ident(pc->a,&cursor,finish,true);
            if(!format || skip_ws(cursor,finish)!=finish)return false;
        }
        /* The memory decoder understands EOT-Lite wrappers, not the legacy
           MicroType Express compression in the embedded-opentype format.
           Do not advertise the complete format and download unsupported
           items before a usable WOFF/WOFF2 source (CSS Fonts 4 4.3.3).
           Unhinted resources still reach the byte-bounded container decoder. */
        if(strcmp(format,"truetype") && strcmp(format,"opentype") &&
           strcmp(format,"woff") && strcmp(format,"woff2"))return false;
        s=end+1;
    }
    return true;
}
static struct font_source *font_sources(struct pctx *pc,const char *s,const char *e) {
    struct font_source *first=NULL,**tail=&first;
    while(s<e) {
        const char *end=scan_to(s,e,",");s=skip_ws(s,end);
        if(end-s>=4 && strn_ieq(s,"url(",4)) {
            const char *a=skip_ws(s+4,end),*z=scan_to(a,end,")");
            if(z<end && font_source_hints(pc,z+1,end)) {
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
        /* local() is not falsely resolved to a class-substitute font. With no
           format hint, the byte-bounded native decoder is authoritative. */
        s=end<e?end+1:e;
    }
    return first;
}
/* ---------------------------------------------------------------- @font-face descriptors
   CSS Fonts 4 descriptor grammar (https://drafts.csswg.org/css-fonts-4/#font-face-rule),
   parsed once for both a face's matching fields and the CSSOM serialization
   that the CSS Font Loading FontFace attributes reflect. A rule and a FontFace
   setter use the same parser, so `font-feature-settings: "smcp" off` and
   face.featureSettings = "'smcp' off" both read back `"smcp" 0`. */
struct font_desc {
    unsigned weight_lo,weight_hi;
    bool italic;
    struct web_font_range *ranges; /* malloc: sorted and merged; NULL with 0: U+0-10FFFF */
    size_t range_count;
};
static int font_range_compare(const void *a,const void *b) {
    const struct web_font_range *x=a,*y=b;
    return x->first<y->first?-1:x->first>y->first?1:x->last<y->last?-1:x->last>y->last;
}
/* <urange>#, each range as written (malloc). 1, 0 syntax error, -1 allocator. */
static int font_ranges_parse(const char *s,const char *end,struct web_font_range **out,size_t *count) {
    struct web_font_range *ranges=NULL;size_t n=0,capacity=0;
    *out=NULL;*count=0;
    for(;;){
        s=skip_ws(s,end);
        if(end-s<3 || (*s!='u'&&*s!='U') || s[1]!='+')goto invalid;
        s+=2;uint32_t lo=0,hi=0;unsigned digits=0,wildcards=0;
        while(s<end && (isxdigit((unsigned char)*s)||*s=='?')){
            if(++digits>6)goto invalid;
            if(*s=='?'){wildcards++;lo<<=4;hi=(hi<<4)|15;}
            else {if(wildcards)goto invalid;unsigned v=*s>='0'&&*s<='9'?*s-'0':tolower((unsigned char)*s)-'a'+10;lo=(lo<<4)|v;hi=(hi<<4)|v;}
            s++;
        }
        if(!digits)goto invalid;
        if(s<end&&*s=='-'){
            if(wildcards)goto invalid;s++;hi=0;unsigned d=0;
            while(s<end&&isxdigit((unsigned char)*s)){
                if(++d>6)goto invalid;unsigned v=*s>='0'&&*s<='9'?*s-'0':tolower((unsigned char)*s)-'a'+10;hi=(hi<<4)|v;s++;
            }
            if(!d)goto invalid;
        }
        if(lo>hi||lo>0x10ffff)goto invalid;hi=MIN(hi,0x10ffffu);
        if(n==capacity){
            size_t cap=capacity?capacity*2:8;
            void *next=cap>capacity&&cap<=SIZE_MAX/sizeof *ranges?realloc(ranges,cap*sizeof *ranges):NULL;
            if(!next){free(ranges);return -1;}
            ranges=next;capacity=cap;
        }
        ranges[n++]=(struct web_font_range){lo,hi};s=skip_ws(s,end);
        if(s==end)break;
        if(*s++!=',')goto invalid;
    }
    *out=ranges;*count=n;return 1;
invalid:
    free(ranges);return 0;
}
/* Matching wants sorted, merged ranges; one covering every code point is no
   restriction at all, and is selected as an ordinary face. */
static void font_ranges_merge(struct web_font_range **ranges,size_t *count) {
    struct web_font_range *r=*ranges;size_t n=*count,merged=0;
    qsort(r,n,sizeof *r,font_range_compare);
    for(size_t i=0;i<n;i++){
        if(merged&&r[i].first<=r[merged-1].last+1)r[merged-1].last=MAX(r[merged-1].last,r[i].last);
        else r[merged++]=r[i];
    }
    if(merged==1&&r[0].first==0&&r[0].last>=0x10ffff){free(r);*ranges=NULL;merged=0;}
    *count=merged;
}
/* A keyword (lowercased, at most 31 bytes, without escapes), or false. */
static bool fd_keyword(const char **ps,const char *e,char word[32]) {
    const char *s=*ps;size_t n=0;
    if(s>=e || isdigit((unsigned char)*s) || !ident_char((unsigned char)*s))return false;
    if(*s=='-' && s+1<e && (isdigit((unsigned char)s[1]) || s[1]=='.'))return false;
    while(s<e && ident_char((unsigned char)*s)){if(n==31)return false;word[n++]=(char)lower((unsigned char)*s++);}
    if(s<e && (*s=='(' || *s=='\\'))return false;
    word[n]=0;*ps=s;return true;
}
/* A number, percentage or dimension token: its value, whether it is an
   <integer>, and its unit ("", "%" or a lowercase identifier). */
static bool fd_number(const char **ps,const char *e,double *value,bool *integer,char unit[8]) {
    const char *s=*ps,*start=s;bool digits=false,point=false,exponent=false;
    if(s<e && (*s=='+' || *s=='-'))s++;
    while(s<e && isdigit((unsigned char)*s)){s++;digits=true;}
    if(s+1<e && *s=='.' && isdigit((unsigned char)s[1])){point=digits=true;s++;while(s<e && isdigit((unsigned char)*s))s++;}
    if(!digits)return false;
    if(s<e && (*s=='e' || *s=='E')){
        const char *t=s+1;if(t<e && (*t=='+' || *t=='-'))t++;
        if(t<e && isdigit((unsigned char)*t)){exponent=true;s=t;while(s<e && isdigit((unsigned char)*s))s++;}
    }
    char text[64];size_t n=(size_t)(s-start);if(n>=sizeof text)return false;
    memcpy(text,start,n);text[n]=0;*value=strtod(text,NULL);
    if(!isfinite(*value))return false;
    *integer=!point && !exponent;unit[0]=0;
    if(s<e && *s=='%'){unit[0]='%';unit[1]=0;s++;}
    else if(s<e && ident_char((unsigned char)*s)){
        size_t k=0;
        while(s<e && ident_char((unsigned char)*s)){if(k==7)return false;unit[k++]=(char)lower((unsigned char)*s++);}
        unit[k]=0;
    }
    if(s<e && (*s=='(' || *s=='\\'))return false;
    *ps=s;return true;
}
static void fd_put_number(sbuf *b,double v) {char t[32];snprintf(t,sizeof t,"%.6g",v==0?0.0:v);sb_puts(b,t);}
static bool fd_string(const char **ps,const char *e,sbuf *out) {
    const char *s=*ps;
    if(s>=e || (*s!='"' && *s!='\''))return false;
    char quote=*s++;const char *begin=s;
    while(s<e && *s!=quote){if(*s=='\n')return false;if(*s=='\\' && s+1<e)s++;s++;}
    if(s>=e)return false;
    css_unescape(out,begin,(size_t)(s-begin));*ps=s+1;return true;
}
/* CSSOM "serialize a string". */
static void fd_put_string(sbuf *b,const char *s,size_t n) {
    sb_putc(b,'"');
    for(size_t i=0;i<n;i++){
        unsigned char c=(unsigned char)s[i];
        if(!c){sb_puts(b,"\xEF\xBF\xBD");continue;}
        if(c<0x20 || c==0x7f){char t[8];snprintf(t,sizeof t,"\\%x ",c);sb_puts(b,t);continue;}
        if(c=='"' || c=='\\')sb_putc(b,'\\');
        sb_putc(b,(char)c);
    }
    sb_putc(b,'"');
}
/* A family name serializes as identifiers when it is a sequence of them, one
   space apart, that is no generic family or CSS-wide keyword; otherwise as a
   string ("sans-serif", "a  b", "content:Segoe UI" are quoted). */
static bool fd_family_identifiers(const char *s,size_t n) {
    static const char *const reserved[]={"inherit","initial","unset","revert","revert-layer","default",
        "serif","sans-serif","cursive","fantasy","monospace","system-ui","emoji","math","fangsong",
        "ui-serif","ui-sans-serif","ui-monospace","ui-rounded",NULL};
    if(!n || memchr(s,0,n))return false;
    if(!memchr(s,' ',n))for(int i=0;reserved[i];i++)if(strlen(reserved[i])==n && strn_ieq(s,reserved[i],n))return false;
    for(const char *w=s,*e=s+n;;){
        const char *end=memchr(w,' ',(size_t)(e-w));if(!end)end=e;
        if(end==w)return false; /* leading, trailing or doubled space */
        unsigned char c=(unsigned char)*w;
        if(isdigit(c) || !ident_char(c) || (c=='-' && (end-w==1 || isdigit((unsigned char)w[1]))))return false;
        for(const char *p=w;p<end;p++)if(!ident_char((unsigned char)*p))return false;
        if(end==e)return true;
        w=end+1;
    }
}
static void fd_put_family(sbuf *b,const char *name,size_t n) {
    if(fd_family_identifiers(name,n))sb_put(b,name,n);else fd_put_string(b,name,n);
}
static bool fd_tag(const sbuf *tag) {
    if(tag->n!=4)return false;
    for(size_t i=0;i<4;i++)if((unsigned char)tag->p[i]<0x20 || (unsigned char)tag->p[i]>0x7e)return false;
    return true;
}
/* `normal`, or a list of <opentype-tag> with an integer, on/off (features) or
   a number (variations) after each. */
static int fd_tag_list(const char *s,const char *e,sbuf *out,bool features) {
    char word[32];double x;bool integer;char unit[8];
    const char *t=s;
    if(fd_keyword(&t,e,word)){
        if(strcmp(word,"normal") || skip_ws(t,e)!=e)return 0;
        sb_puts(out,"normal");return 1;
    }
    for(bool first=true;;first=false){
        sbuf tag={0};t=skip_ws(s,e);
        if(!fd_string(&t,e,&tag) || !fd_tag(&tag)){sb_free(&tag);return 0;}
        if(!first)sb_puts(out,", ");
        fd_put_string(out,tag.p,tag.n);sb_free(&tag);
        const char *u=skip_ws(t,e);double value=1;
        if(features){
            if(u<e && *u!=','){
                if(fd_keyword(&u,e,word)){
                    if(!strcmp(word,"on"))value=1;else if(!strcmp(word,"off"))value=0;else return 0;
                }else if(!fd_number(&u,e,&x,&integer,unit) || unit[0] || !integer || x<0)return 0;
                else value=x;
                t=u;
            }
            if(value!=1){sb_putc(out,' ');fd_put_number(out,value);}
        }else{
            if(!fd_number(&u,e,&x,&integer,unit) || unit[0])return 0;
            sb_putc(out,' ');fd_put_number(out,x);t=u;
        }
        t=skip_ws(t,e);
        if(t==e)return 1;
        if(*t!=',')return 0;
        s=t+1;
    }
}
/* One or two of normal, the keywords in names, or a non-negative percentage
   (font-width, the metrics overrides); auto alone where allowed. */
static int fd_percentages(const char *s,const char *e,sbuf *out,const char *const *names,int most,bool automatic) {
    char word[32];double x;bool integer;char unit[8];int count=0;
    for(;;){
        const char *t=skip_ws(s,e);
        if(t==e)break;
        if(count==most || (count && t==s))return 0;
        if(count)sb_putc(out,' ');
        if(fd_keyword(&t,e,word)){
            if(automatic && !count && !strcmp(word,"auto") && skip_ws(t,e)==e){sb_puts(out,word);return 1;}
            bool known=false;
            for(int i=0;names&&names[i];i++)if(!strcmp(word,names[i]))known=true;
            if(!known)return 0;
            sb_puts(out,word);
        }else{
            if(!fd_number(&t,e,&x,&integer,unit) || strcmp(unit,"%") || x<0)return 0;
            fd_put_number(out,x);sb_putc(out,'%');
        }
        count++;s=t;
    }
    return count>0;
}
/* 1: valid, the serialization appended to out and matching fields in m (if
   any); 0: syntax error (out is then garbage); -1: allocator. Not the family:
   a rule parses <family-name>, a FontFace takes its literal name. */
static int font_desc_parse(enum css_font_descriptor which,const char *v,size_t length,sbuf *out,struct font_desc *m) {
    static const char *const normal[]={"normal",NULL};
    static const char *const widths[]={"normal","ultra-condensed","extra-condensed","condensed","semi-condensed",
        "semi-expanded","expanded","extra-expanded","ultra-expanded",NULL};
    const char *s=skip_ws(v,v+length),*e=v+length;trim_r(s,&e);
    char word[32];double x;bool integer;char unit[8];
    if(s==e)return 0;
    switch(which){
    case CSS_FONT_STYLE: {
        if(!fd_keyword(&s,e,word))return 0;
        bool oblique=!strcmp(word,"oblique");
        if(!oblique && strcmp(word,"auto") && strcmp(word,"normal") && strcmp(word,"italic"))return 0;
        sb_puts(out,word);
        if(m)m->italic=oblique || !strcmp(word,"italic");
        for(int angles=0;oblique && skip_ws(s,e)<e;angles++){
            const char *t=skip_ws(s,e);
            if(angles==2 || t==s || !fd_number(&t,e,&x,&integer,unit))return 0;
            double degrees=!strcmp(unit,"deg")?x:!strcmp(unit,"grad")?x*0.9:!strcmp(unit,"rad")?x*57.29577951308232:
                           !strcmp(unit,"turn")?x*360:NAN;
            if(!(degrees>=-90 && degrees<=90))return 0;
            sb_putc(out,' ');fd_put_number(out,x);sb_puts(out,unit);s=t;
        }
        return skip_ws(s,e)==e;
    }
    case CSS_FONT_WEIGHT: {
        unsigned weights[2]={400,400};int count=0;
        for(;;){
            const char *t=skip_ws(s,e);
            if(t==e)break;
            if(count==2 || (count && t==s))return 0;
            if(count)sb_putc(out,' ');
            if(fd_keyword(&t,e,word)){
                if(!count && !strcmp(word,"auto") && skip_ws(t,e)==e){sb_puts(out,word);if(m)m->weight_lo=m->weight_hi=400;return 1;}
                if(!strcmp(word,"normal"))weights[count]=400;else if(!strcmp(word,"bold"))weights[count]=700;else return 0;
                sb_puts(out,word);
            }else{
                if(!fd_number(&t,e,&x,&integer,unit) || unit[0] || !(x>=1 && x<=1000))return 0;
                weights[count]=(unsigned)(x+0.5);fd_put_number(out,x);
            }
            count++;s=t;
        }
        if(!count)return 0;
        /* A reversed range is swapped where it is used (CSS Fonts 4 4.5). */
        if(m){m->weight_lo=MIN(weights[0],weights[count-1]);m->weight_hi=MAX(weights[0],weights[count-1]);}
        return 1;
    }
    case CSS_FONT_WIDTH: return fd_percentages(s,e,out,widths,2,true);
    case CSS_FONT_UNICODE_RANGE: {
        struct web_font_range *ranges;size_t count;
        int status=font_ranges_parse(s,e,&ranges,&count);
        if(status<=0)return status;
        for(size_t i=0;i<count;i++){
            char t[40];
            if(ranges[i].first==ranges[i].last)snprintf(t,sizeof t,"%sU+%X",i?", ":"",(unsigned)ranges[i].first);
            else snprintf(t,sizeof t,"%sU+%X-%X",i?", ":"",(unsigned)ranges[i].first,(unsigned)ranges[i].last);
            sb_puts(out,t);
        }
        if(m){font_ranges_merge(&ranges,&count);m->ranges=ranges;m->range_count=count;}else free(ranges);
        return 1;
    }
    case CSS_FONT_FEATURE_SETTINGS: return fd_tag_list(s,e,out,true);
    case CSS_FONT_VARIATION_SETTINGS: return fd_tag_list(s,e,out,false);
    case CSS_FONT_DISPLAY: {
        static const char *const displays[]={"auto","block","swap","fallback","optional",NULL};
        if(!fd_keyword(&s,e,word) || skip_ws(s,e)!=e)return 0;
        for(int i=0;displays[i];i++)if(!strcmp(word,displays[i])){sb_puts(out,word);return 1;}
        return 0;
    }
    case CSS_FONT_ASCENT_OVERRIDE: case CSS_FONT_DESCENT_OVERRIDE: case CSS_FONT_LINE_GAP_OVERRIDE:
        return fd_percentages(s,e,out,normal,2,false);
    case CSS_FONT_SIZE_ADJUST: return fd_percentages(s,e,out,NULL,1,false);
    default: return 0;
    }
}
/* Initial values: the FontFaceDescriptors defaults, as a rule without the
   descriptor also reads (CSS Font Loading 2.1). */
static const char *const font_desc_initial[CSS_FONT_DESCRIPTORS]={
    [CSS_FONT_FAMILY]="",[CSS_FONT_STYLE]="normal",[CSS_FONT_WEIGHT]="normal",[CSS_FONT_WIDTH]="normal",
    [CSS_FONT_UNICODE_RANGE]="U+0-10FFFF",[CSS_FONT_FEATURE_SETTINGS]="normal",[CSS_FONT_VARIATION_SETTINGS]="normal",
    [CSS_FONT_DISPLAY]="auto",[CSS_FONT_ASCENT_OVERRIDE]="normal",[CSS_FONT_DESCENT_OVERRIDE]="normal",
    [CSS_FONT_LINE_GAP_OVERRIDE]="normal",[CSS_FONT_SIZE_ADJUST]="100%"
};
static uint64_t font_face_serials;
int css_font_descriptor(enum css_font_descriptor which,const char *value,size_t n,char **serialized) {
    *serialized=NULL;
    if((unsigned)which>=CSS_FONT_DESCRIPTORS)return 0;
    sbuf out={0};int status=1;
    if(which==CSS_FONT_FAMILY)fd_put_family(&out,value,n);
    else status=font_desc_parse(which,value,n,&out,NULL);
    if(status>0)*serialized=sb_cstr(&out);else sb_free(&out);
    return status;
}
static void parse_font_face(struct pctx *pc,const char *s,const char *e,const struct mcond *media) {
    static const struct {const char *name;enum css_font_descriptor which;} names[]={
        {"font-style",CSS_FONT_STYLE},{"font-weight",CSS_FONT_WEIGHT},{"font-stretch",CSS_FONT_WIDTH},
        {"font-width",CSS_FONT_WIDTH},{"unicode-range",CSS_FONT_UNICODE_RANGE},
        {"font-feature-settings",CSS_FONT_FEATURE_SETTINGS},{"font-variation-settings",CSS_FONT_VARIATION_SETTINGS},
        {"font-display",CSS_FONT_DISPLAY},{"ascent-override",CSS_FONT_ASCENT_OVERRIDE},
        {"descent-override",CSS_FONT_DESCENT_OVERRIDE},{"line-gap-override",CSS_FONT_LINE_GAP_OVERRIDE},
        {"size-adjust",CSS_FONT_SIZE_ADJUST}};
    struct font_face *f=ar_alloc(pc->a,sizeof *f);memset(f,0,sizeof *f);
    f->weight_lo=f->weight_hi=400;f->media=media;f->layer=pc->layer;
    for(int i=0;i<CSS_FONT_DESCRIPTORS;i++)f->desc[i]=font_desc_initial[i];
    while(s<e) {
        const char *end=scan_to(s,e,";"),*colon=scan_to(s,end,":"),*name_end=colon;
        const char *name=skip_ws(s,name_end);trim_r(name,&name_end);
        if(colon<end) {
            const char *value=skip_ws(colon+1,end),*ve=end;trim_r(value,&ve);
            size_t n=(size_t)(name_end-name);
            if(n==11 && strn_ieq(name,"font-family",n)) {
                const char *cursor=value,*family=css_font_family_next(pc->a,&cursor,ve);
                if(family && cursor==ve){
                    f->family=family;
                    sbuf b={0};fd_put_family(&b,family,strlen(family));
                    f->desc[CSS_FONT_FAMILY]=ar_strndup(pc->a,b.p?b.p:"",b.n);sb_free(&b);
                }
            } else if(n==3 && strn_ieq(name,"src",n)) {
                struct font_source *sources=font_sources(pc,value,ve);
                /* A list without any supported component is a parse error,
                   not a valid empty override of an earlier src descriptor. */
                if(sources)f->sources=sources;
            } else for(size_t i=0;i<sizeof names/sizeof *names;i++) {
                if(strlen(names[i].name)!=n || !strn_ieq(name,names[i].name,n))continue;
                /* An invalid declaration leaves the previous valid one. */
                sbuf b={0};struct font_desc m={.weight_lo=f->weight_lo,.weight_hi=f->weight_hi,.italic=f->italic};
                int status=font_desc_parse(names[i].which,value,(size_t)(ve-value),&b,&m);
                if(status<0){sb_free(&b);free(m.ranges);ar_alloc(pc->a,SIZE_MAX);}
                if(status>0){
                    f->desc[names[i].which]=ar_strndup(pc->a,b.p?b.p:"",b.n);
                    f->weight_lo=m.weight_lo;f->weight_hi=m.weight_hi;f->italic=m.italic;
                    if(names[i].which==CSS_FONT_UNICODE_RANGE){
                        struct web_font_range *ranges=m.range_count?ar_alloc(pc->a,m.range_count*sizeof *ranges):NULL;
                        if(m.range_count)memcpy(ranges,m.ranges,m.range_count*sizeof *ranges);
                        f->ranges=ranges;f->range_count=m.range_count;
                    }
                }
                free(m.ranges);sb_free(&b);break;
            }
        }
        s=end<e?end+1:e;
    }
    if(!f->family || !f->sources)return;
    f->id=__atomic_add_fetch(&font_face_serials,1,__ATOMIC_RELAXED);
    if(pc->sh->fonts_last)pc->sh->fonts_last->next=f;else pc->sh->fonts=f;
    pc->sh->fonts_last=f;
}

static void parse_opacity_keyframes(struct pctx *pc,const char *ns,const char *ne,
    const char *s,const char *e,const struct mcond *media) {
    const char *cursor=ns,*name=read_ident(pc->a,&cursor,ne,false);
    if(!name || skip_ws(cursor,ne)!=ne || !strcasecmp(name,"none") || !strcasecmp(name,"initial") ||
        !strcasecmp(name,"inherit") || !strcasecmp(name,"unset") || !strcasecmp(name,"default") ||
        !strcasecmp(name,"revert") || !strcasecmp(name,"revert-layer"))return;
    struct opacity_keyframes *key=ar_alloc(pc->a,sizeof *key);key->name=name;key->media=media;key->layer=pc->layer;
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
            if(name&&!strcmp(name,"import") && (pc->rule_depth!=1 || !pc->import_allowed)) {
                s=p<e?p+1:e;continue;
            }
            if (p >= e || *p == ';') {
                bool layer=name&&!strcmp(name,"layer");
                if(layer && !layer_statement(pc,qs,qe,media)) {
                    s=p<e?p+1:e;continue;
                }
                struct css_rule_info *info = (p<e||layer) && (type || known_group) ? cssom_record(pc, type, s, p<e?p+1:e) : NULL;
                if(info&&layer)info->layer_kind=1;
                if(pc->rule_depth==1 && type==10)pc->import_allowed=false;
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
                    struct css_layer_decl *import_layer=NULL;
                    if((size_t)(qe-rest)>=5 && strn_ieq(rest,"layer",5) &&
                       (rest+5==qe || rest[5]=='(' || is_space((unsigned char)rest[5]))) {
                        rest+=5;
                        if(rest<qe&&*rest=='(') {
                            const char *close=scan_to(rest+1,qe,")");
                            const char *begin=skip_ws(rest+1,close),*end=close;trim_r(begin,&end);
                            import_layer=close<qe?layer_name_parse(pc,begin,end,false):NULL;
                            rest=close<qe?close+1:qe;ok&=import_layer!=NULL;
                        } else import_layer=layer_name_parse(pc,rest,rest,true);
                        rest=skip_ws(rest,qe);
                    }
                    if((size_t)(qe-rest)>=9 && strn_ieq(rest,"supports(",9)) {
                        const char *close=scan_to(rest+9,qe,")");
                        bool supported=false;
                        if(close<qe) {
                            supported=css_supports_condition(rest+9,(size_t)(close-rest-9),false);
                            if(!supported){sbuf q={0};sb_putc(&q,'(');sb_put(&q,rest+9,(size_t)(close-rest-9));sb_putc(&q,')');
                                supported=css_supports_condition(sb_cstr(&q),q.n,false);sb_free(&q);}
                        }
                        ok&=supported;rest=close<qe?skip_ws(close+1,qe):qe;
                    }
                    struct css_rule_import *record=info?ar_alloc(pc->a,sizeof *record):NULL;
                    if(record){record->media=rest<qe?ar_strndup(pc->a,rest,(size_t)(qe-rest)):NULL;info->import=record;}
                    const struct mcond *import_condition=rest<qe?parse_condition(pc,rest,qe,media,false):media;
                    if(ok)layer_event(pc,import_layer,import_condition,info);
                    char abs[2048];
                    if (ok && url_resolve(pc->base, sb_cstr(&u), abs, sizeof abs)) {
                        if (record) { record->url = ar_strdup(pc->a, abs); pc->sh->import_urls++; }
                        if (pc->imports) {
                            struct css_import *im = malloc(sizeof *im);
                            im->url = strdup(abs);
                            im->order = pc->sh->order - 0.1 + 0.001 * pc->nimports++;
                            im->owner=info;
                            pv_push(pc->imports, im);
                        }
                    }
                    sb_free(&u);
                }
                s = p < e ? p + 1 : e;
                continue;
            }
            const char *be = block_end(p + 1, e);
            struct css_layer_decl *block_layer=NULL;
            if(name&&!strcmp(name,"layer")) {
                block_layer=layer_name_parse(pc,qs,qe,true);
                if(!block_layer){s=be<e?be+1:e;continue;}
            }
            if(type || known_group) {
                struct css_rule_info *info=cssom_record(pc, type, s, be < e ? be + 1 : e);
                if(info&&block_layer)info->layer_kind=2;
                if(pc->rule_depth==1)pc->import_allowed=false;
            }
            if (name) {
                if (!strcmp(name, "font-face")) parse_font_face(pc,p+1,be,media);
                else if (!strcmp(name,"keyframes") || !strcmp(name,"-webkit-keyframes"))
                    parse_opacity_keyframes(pc,qs,qe,p+1,be,media);
                else if (!strcmp(name,"media") || !strcmp(name,"container")) {
                    const struct mcond *mc=parse_condition(pc,qs,qe,media,!strcmp(name,"container"));
                    parse_rules(pc,p+1,be,mc);
                } else if (!strcmp(name, "supports")) {
                    if (css_supports_condition(qs, (size_t)(p - qs), false)) parse_rules(pc, p + 1, be, media);
                } else if (!strcmp(name,"layer")) {
                    struct css_layer_decl *previous=pc->layer;pc->layer=block_layer;
                    layer_event(pc,block_layer,media,NULL);parse_rules(pc,p+1,be,media);pc->layer=previous;
                } else if (!strcmp(name, "scope") ||
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
            if(pc->rule_depth==1)pc->import_allowed=false;
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
void css_sheet_arena_moved(sheet_t *sheet, arena_t *arena) { if (sheet) sheet->ast->arena = arena; }
void css_sheet_media(sheet_t *sheet, const char *media) { if (sheet) sheet->owner_media = media; }
uint32_t css_sheet_import_urls(const sheet_t *sheet) { return sheet ? sheet->import_urls : 0; }
struct css_rule_info *css_sheet_rules(sheet_t *sheet, uint32_t *length) {
    if (length) *length = sheet ? sheet->cssom_count : 0;
    return sheet ? sheet->cssom_first : NULL;
}
bool css_sheet_single_rule(sheet_t *sheet, const char *source, size_t length) {
    (void)source; (void)length;
    uint32_t count = 0; struct css_rule_info *info = css_sheet_rules(sheet, &count);
    if (count != 1 || !info || ((!info->layer_kind) && (info->type != 1 || !info->native_rule))) return false;
    /* Native parser recovery may accept a prefix/missing close. insertRule
       requires one entire qualified rule, not forgiving stylesheet recovery. */
    size_t n = sheet->cssom_length; const char *clean = sheet->cssom_source;
    const char *s = skip_ws(clean, clean + n), *p = scan_to(s, clean + n, "{;}");
    if(info->layer_kind==1)return p==clean+n || (*p==';'&&skip_ws(p+1,clean+n)==clean+n);
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
    sheet->import_parent=sheet->import_first=sheet->import_last=sheet->import_next=NULL;sheet->import_owner=NULL;
    return sheet;
}
void css_sheet_import_context(sheet_t *sheet,sheet_t *parent,const struct css_rule_info *owner) {
    sheet->import_parent=parent;sheet->import_owner=owner;
    if(parent->import_last)parent->import_last->import_next=sheet;else parent->import_first=sheet;
    parent->import_last=sheet;
}

sheet_t *css_parse_sheet(arena_t *a, const char *css, size_t n, const char *base_url, double order, pvec *imports) {
    static uint64_t serials;
    struct pctx pc = {a, NULL, base_url, imports, 0, false, 0};
    pc.import_allowed=true;
    pc.sh = ar_alloc(a, sizeof(sheet_t));
    pc.sh->order = order;
    pc.sh->ast = pc.sh; pc.sh->arena = a;
    pc.sh->serial = __atomic_add_fetch(&serials, 1, __ATOMIC_RELAXED);
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
    if (kind == MQ_LENGTH && s < e && isalpha((unsigned char)*s))
        return css_query_length(s, (size_t)(e-s), m->container ? m->em : 16,
            m->container ? m->rem : 16, m->container ? m->viewport_width : m->vw,
            m->container ? m->viewport_height : m->vh, out);
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
    if(strn_ieq(s,"color",n)){*v=display_bits_per_channel();return MQ_INTEGER;} /* bits per component */
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
    const struct web_font_choice *choices;
};
static struct font_binding *font_bindings;
static size_t font_binding_count,font_binding_capacity;
static bool font_faces_available;
static void font_bindings_clear(void) {
    free(font_bindings);font_bindings=NULL;font_binding_count=font_binding_capacity=0;
}
static struct font_face *scoped_font_face(web_doc *d, node_t *scope, const char *family, unsigned weight, bool italic, int vw, int vh);
/* The resource of a source: a FontFace's own, or the document's for a rule
   URL (workers only find the identities build_ctx published). */
static struct web_font_resource *font_source_resource(web_doc *d,const struct font_source *src) {
    if(src->resource)return src->resource;
    if(!css_worker())return doc_font_resource(d,src->url);
    struct web_font_resource *r=d->fonts;
    while(r && strcmp(r->url,src->url))r=r->next;
    return r;
}
/* Rendering selected a FontFace: the user agent loads it as if load() had
   been called (CSS Font Loading 2.2), which its document's realm observes. */
static void font_face_used(web_doc *d,const struct font_face *f) {
    if(f->author && !__atomic_exchange_n(&f->author->started,true,__ATOMIC_RELAXED))
        __atomic_add_fetch(&d->font_generation,1,__ATOMIC_RELAXED);
}
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
        /* Restricted faces are selected per character below, never as the
           metrics/raster face for every character in the entire family. */
        if(best->range_count)continue;
        font_face_used(d,best);
        for(struct font_source *src=best->sources;src;src=src->next) {
            struct web_font_resource *r=font_source_resource(d,src);
            if(!r){ar_alloc(a,SIZE_MAX);return NULL;}
            if(r->failed)continue;
            __atomic_store_n(&r->wanted,true,__ATOMIC_RELAXED);return r->face;
        }
    }
    return NULL;
}
static const struct web_font_choice *select_font_choices(web_doc *,node_t *,style_t *,int,int,arena_t *);
static const struct web_font_choice *copy_font_choices(const struct web_font_choice *source,arena_t *a) {
    struct web_font_choice *first=NULL,**tail=&first;
    for(;source;source=source->next){
        struct web_font_choice *choice=ar_alloc(a,sizeof *choice);*choice=*source;choice->next=NULL;
        if(source->range_count){struct web_font_range *ranges=ar_alloc(a,source->range_count*sizeof *ranges);memcpy(ranges,source->ranges,source->range_count*sizeof *ranges);choice->ranges=ranges;}
        *tail=choice;tail=&choice->next;
    }
    return first;
}
static font_t *select_web_font(web_doc *d,node_t *node,style_t *style,int vw,int vh,arena_t *a) {
    style->font_choices=NULL;
    if(!font_faces_available || !style->font_names)return NULL;
    if(css_worker()){style->font_choices=select_font_choices(d,node,style,vw,vh,a);return select_web_font_uncached(d,node,style,vw,vh,a);}
    node_t *scope=doc_node_root(node,false);unsigned weight=style->font_weight;bool italic=style->font_style!=0;
    for(size_t i=0;i<font_binding_count;i++) {
        const struct font_binding *b=&font_bindings[i];
        if(b->scope==scope && b->weight==weight && b->italic==italic &&
           (b->names==style->font_names || !strcmp(b->names,style->font_names))){
            /* Computed styles own separate per-node arenas. A later partial
               restyle must not retire another node's composite-font list. */
            style->font_choices=copy_font_choices(b->choices,a);return b->face;
        }
    }
    font_t *face=select_web_font_uncached(d,node,style,vw,vh,a);
    style->font_choices=select_font_choices(d,node,style,vw,vh,a);
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
        font_bindings[font_binding_count++]=(struct font_binding){style->font_names,scope,weight,italic,face,style->font_choices};
    return face;
}

/* ---------------------------------------------------------------- the rule index */
struct has_subject;
struct ient {
    struct ient *next;
    const struct selector *sel;
    union {
        const struct rule *r;
        const struct has_subject *has; /* in a relational index */
    };
    uint32_t order;
    /* 0: unconditional; else its rule's @media is css_index.media[media - 1].
       A sheet within the source budget has far fewer than 2^24 @media. */
    uint32_t media : 24;
    uint32_t pseudo : 8;
    /* The selector's, here: most candidates are rejected on these alone,
       without reaching the selector. */
    uint32_t ancestor_hashes[SELECTOR_ANCESTOR_HASHES];
};

struct css_feature;
struct bucket {
    const char *key;
    uint32_t hash; /* hash_str(key): a lookup compares it before the name */
    union { struct ient *list; struct cascade_layer *layer; struct css_feature *feature; };
    struct bucket *next;
};

/* Grows to keep its chains about one bucket long: one sheet can name
   thousands of classes, and every element looks each of its own up in every
   sheet bound to its scope. */
struct htab {
    struct bucket **v;
    uint32_t cap, count;
};
struct cascade_layer {
    struct cascade_layer *parent;
    struct htab children;
    size_t depth,order,next_child;
};
struct layer_sheet_map {
    const sheet_t *sheet;
    struct cascade_layer *outer,**layers;
    struct layer_sheet_map *next;
};

/* One sheet AST's rules, by what an element must carry to match them. It does
   not depend on the viewport: a binding evaluates the rules' @media. */
struct css_index {
    struct htab ids, classes, tags;
    struct ient *univ;
    struct font_face *fonts;
    struct opacity_keyframes *motions;
    uint32_t rules;
    unsigned form_features;
    bool containers, siblings;
    const struct mcond **media; /* the distinct @media of its rules */
    uint32_t nmedia, media_cap;
    struct css_index *relational; /* :has() subjects (ient.has) by their compounds' keys */
    struct css_index *child_list_subjects; /* parent state can affect its outer siblings */
    struct css_index *child_list_parents; /* own state changed by a child-list mutation */
    struct css_index *child_list_siblings[4]; /* position, sibling combinator or sibling :has witness, by enum sibling_reach bit */
    bool relational_siblings;
    /* What changing a class, id, attribute or dynamic state can restyle */
    struct htab feature_ids, feature_classes, feature_attrs;
    struct css_feature *feature_states[CSS_STATES];
};
/* An index applied in one scope at one place of the cascade order. */
struct css_binding {
    struct css_index *index;
    const bool *media_ok; /* by ient.media, for this cascade's viewport */
    uint32_t order;
    bool ua;
    struct css_binding *next;
    struct layer_sheet_map *layers;
};
struct css_scope {
    node_t *root;
    struct css_binding *first, *last;
    struct css_scope *next;
    struct cascade_layer layers;
};
/* What a binding list was built from, one per sheet instance in cascade order:
   a rescan that rebuilds the same list keeps the bindings. */
struct sheet_signature {
    uint64_t serial;
    const node_t *scope;
    const char *media;
    int import_parent; /* its position in the list; -1: a root sheet */
    const struct css_rule_info *import_owner;
    bool ua;
};
struct index_media { const struct css_index *index; const bool *ok; struct index_media *next; };
struct css_ctx {
    arena_t a;
    struct sheet_signature *sheets;
    int nsheets;
    bool complete; /* not cancelled while it was built */
    struct index_media *media[64];
    struct css_scope *scopes;
    struct css_binding *ua;
    struct cascade_layer ua_layers;
    struct layer_sheet_map *layer_sheets[256];
    bool relational, relational_siblings, siblings, parallel_safe, containers;
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

/* A slot in a shadow tree whose flat children are the host's light children assigned to it,
   which are not its descendants. */
static bool flat_slot(node_t *n) {
    return n->type==N_ELEM && !n->foreign && n->tag==T_slot && n->slot_assigned_first &&
           doc_node_root(n,false)->shadow_host;
}
static node_t *container_first(node_t *n) {
    if(n->shadow_root)n=n->shadow_root;
    if(flat_slot(n))return n->slot_assigned_first;
    return n->first;
}
static node_t *container_next(node_t *n) {
    node_t *p=doc_flat_parent(n);
    if(p && flat_slot(p))return n->assigned_next;
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
    t->cap = cap; t->count = 0;
    t->v = ar_alloc(a, sizeof(struct bucket *) * cap);
}
static void ht_grow(arena_t *a, struct htab *t) {
    uint32_t cap = t->cap * 2;
    if (cap <= t->cap) return;
    struct bucket **v = ar_alloc(a, sizeof *v * cap);
    for (uint32_t i = 0; i < t->cap; i++)
        for (struct bucket *b = t->v[i], *next; b; b = next) {
            next = b->next;
            b->next = v[b->hash & (cap - 1)];
            v[b->hash & (cap - 1)] = b;
        }
    t->v = v; t->cap = cap;
}

static struct bucket *ht_bucket(arena_t *a, struct htab *t, const char *key, bool create) {
    uint32_t hash = hash_str(key);
    for (struct bucket *b = t->v[hash & (t->cap - 1)]; b; b = b->next)
        if (b->hash == hash && !strcmp(b->key, key)) return b;
    if (!create) return NULL;
    struct bucket *b = ar_alloc(a, sizeof *b);
    b->key = key; b->hash = hash;
    b->next = t->v[hash & (t->cap - 1)];
    t->v[hash & (t->cap - 1)] = b;
    if (++t->count > t->cap) ht_grow(a, t);
    return b;
}
static struct ient **ht_slot(arena_t *a, struct htab *t, const char *key, bool create) {
    struct bucket *b=ht_bucket(a,t,key,create);return b?&b->list:NULL;
}

static struct css_scope *index_scope(struct css_ctx *x,node_t *root) {
    struct css_scope *scope=root?root->style_scope:NULL;
    if(!scope){scope=ar_alloc(&x->a,sizeof *scope);scope->root=root;scope->next=x->scopes;x->scopes=scope;
        if(root)root->style_scope=scope;}
    return scope;
}
static struct layer_sheet_map *layer_sheet_find(struct css_ctx *x,const sheet_t *sheet,bool create) {
    uintptr_t value=(uintptr_t)sheet;size_t slot=((value>>4)^(value>>13))&255;
    for(struct layer_sheet_map *m=x->layer_sheets[slot];m;m=m->next)if(m->sheet==sheet)return m;
    if(!create)return NULL;
    struct layer_sheet_map *m=ar_alloc(&x->a,sizeof *m);m->sheet=sheet;
    if(sheet->layer_count>SIZE_MAX/sizeof *m->layers)ar_alloc(&x->a,SIZE_MAX);
    if(sheet->layer_count)m->layers=ar_alloc(&x->a,sheet->layer_count*sizeof *m->layers);
    m->next=x->layer_sheets[slot];x->layer_sheets[slot]=m;return m;
}
static struct cascade_layer *layer_resolve(struct css_ctx *x,struct layer_sheet_map *m,const struct css_layer_decl *decl) {
    struct pending_layer {const struct css_layer_decl *decl;struct pending_layer *next;} *pending=NULL;
    while(decl&&!m->layers[decl->id]) {
        struct pending_layer *p=ar_alloc(&x->a,sizeof *p);p->decl=decl;p->next=pending;pending=p;decl=decl->parent;
    }
    struct cascade_layer *parent=decl?m->layers[decl->id]:m->outer;
    while(pending) {
        decl=pending->decl;struct cascade_layer *layer=NULL;
        struct bucket *entry=NULL;
        if(decl->name){if(!parent->children.v)ht_init(&x->a,&parent->children,16);
            entry=ht_bucket(&x->a,&parent->children,decl->name,true);layer=entry->layer;}
        if(!layer){layer=ar_alloc(&x->a,sizeof *layer);layer->parent=parent;
            if(parent->depth==SIZE_MAX || parent->next_child==SIZE_MAX)ar_alloc(&x->a,SIZE_MAX);
            layer->depth=parent->depth+1;layer->order=parent->next_child++;
            if(entry)entry->layer=layer;}
        m->layers[decl->id]=layer;parent=layer;pending=pending->next;
    }
    return parent;
}
static bool sheet_media_ok(const sheet_t *sheet,int vw,int vh,bool scripting) {
    for(;sheet;sheet=sheet->import_parent) {
        if(sheet->owner_media && *sheet->owner_media&&!css_media_evaluate(sheet->owner_media,vw,vh,scripting,NULL,0))return false;
        const char *media=sheet->import_owner&&sheet->import_owner->import?sheet->import_owner->import->media:NULL;
        if(media&&*media&&!css_media_evaluate(media,vw,vh,scripting,NULL,0))return false;
    }
    return true;
}
static void layers_build(web_doc *d,struct css_ctx *x,int vw,int vh) {
    struct layer_frame {struct layer_frame *parent;const sheet_t *child;struct css_layer_event *event;struct layer_sheet_map *map;};
    bool scripting=web_js_enabled(d);
    for(int i=0;i<d->sty.sheets.n;i++) {
        sheet_t *sheet=d->sty.sheets.v[i];
        if(sheet->import_parent || !sheet_media_ok(sheet,vw,vh,scripting))continue;
        struct layer_frame *f=ar_alloc(&x->a,sizeof *f);f->child=sheet->import_first;f->event=sheet->layers_first;
        f->map=layer_sheet_find(x,sheet,true);
        f->map->outer=sheet->ua?&x->ua_layers:&index_scope(x,sheet->scope?sheet->scope:d->root)->layers;
        while(f) {
            if(!web_native_checkpoint(d))return;
            if(!f->event){f=f->parent;continue;}
            const struct css_layer_event *event=f->event;f->event=event->next;
            if(!media_chain_ok(event->media,vw,vh,scripting)) {
                if(event->import && f->child && f->child->import_owner==event->import)f->child=f->child->import_next;
                continue;
            }
            struct cascade_layer *layer=event->layer?layer_resolve(x,f->map,event->layer):f->map->outer;
            if(event->import && f->child && f->child->import_owner==event->import) {
                const sheet_t *child=f->child;f->child=child->import_next;
                struct layer_frame *next=ar_alloc(&x->a,sizeof *next);next->parent=f;
                next->child=child->import_first;next->event=child->layers_first;next->map=layer_sheet_find(x,child,true);
                next->map->outer=layer;f=next;
            }
        }
    }
}
/* Positive means a wins at normal priority. Direct declarations in a parent
   occupy its implicit final sublayer; !important reverses this order. */
static int layer_compare(struct cascade_layer *a,struct cascade_layer *b) {
    if(a==b)return 0;
    size_t ad=a?a->depth:0,bd=b?b->depth:0;
    struct cascade_layer *x=a,*y=b;
    while(ad>bd){x=x->parent;ad--;if(x==b)return -1;}
    while(bd>ad){y=y->parent;bd--;if(y==a)return 1;}
    if(!ad)return a? -1:1;
    while(x->parent!=y->parent){if(!x->parent||!y->parent)return 0;x=x->parent;y=y->parent;}
    return x->order>y->order?1:-1;
}

static struct ient *index_rule(arena_t *a, struct css_index *x, const struct rule *r, const struct selector *s, uint32_t order,
                               uint32_t media) {
    if (s->pseudo == PE_OTHER) return NULL;
    struct ient *ie = ar_alloc(a, sizeof *ie);
    ie->sel = s;
    ie->r = r;
    ie->order = order;
    ie->media = media;
    selector_ancestor_hashes(s, ie->ancestor_hashes);
    ie->pseudo = s->pseudo;
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
    return ie;
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
        for(int i=0;i<c->n;i++)if(c->s[i].args && c->s[i].pc != PC_HAS)
            for(int k=0;k<c->s[i].args->n;k++)if(selector_siblings(c->s[i].args->v[k]->right))return true;
    }
    return false;
}

static bool compound_has_siblings(const struct compound *c) {
    for (int i = 0; i < c->n; i++) {
        const struct simple *s = &c->s[i];
        for (int k = 0; s->args && k < s->args->n; k++) {
            if (s->kind == SK_PC && s->pc == PC_HAS) {
                const struct compound *first = s->args->v[k]->right;
                while (first->left && first->left->left) first = first->left;
                if (first->comb == '+' || first->comb == '~') return true;
            } else for (const struct compound *part = s->args->v[k]->right; part; part = part->left)
                if (compound_has_siblings(part)) return true;
        }
    }
    return false;
}
enum subject_dependency { SUBJECT_CHILD_OUTER, SUBJECT_CHILD_PARENT, SUBJECT_CHILD_SIBLING };
/* Which siblings of an inserted element can change a positional subject's
   match: the nearest following/preceding element (and nearest one of the
   inserted element's type), or every following/preceding element. */
enum sibling_reach { SIBLING_NEXT = 1, SIBLING_FOLLOWING = 2, SIBLING_PREVIOUS = 4, SIBLING_PRECEDING = 8 };
static unsigned index_child_list_subjects_inner(arena_t *a,struct css_index *index,const struct compound *part,bool publish,enum subject_dependency dependency) {
    bool sibling=dependency==SUBJECT_CHILD_SIBLING;
    unsigned sensitive=0;
    if(sibling) {
        if(part->comb=='+')sensitive|=SIBLING_NEXT;
        if(part->comb=='~')sensitive|=SIBLING_FOLLOWING;
        if(compound_has_siblings(part))sensitive|=SIBLING_FOLLOWING|SIBLING_PRECEDING;
    }
    for(int i=0;i<part->n;i++) {
        const struct simple *s=&part->s[i];
        if(s->kind==SK_PC) switch(s->pc) {
        case PC_EMPTY: case PC_FOCUS_WITHIN: case PC_HOVER: case PC_ACTIVE:
        case PC_CHECKED: case PC_DISABLED: case PC_ENABLED: case PC_VALID: case PC_INVALID:
        case PC_IN_RANGE: case PC_OUT_OF_RANGE: case PC_INDETERMINATE: case PC_PLACEHOLDER_SHOWN:
            if(!sibling)sensitive|=1;break;
        case PC_FIRST_CHILD: case PC_FIRST_OF_TYPE: if(sibling)sensitive|=SIBLING_NEXT;break;
        case PC_LAST_CHILD: case PC_LAST_OF_TYPE: if(sibling)sensitive|=SIBLING_PREVIOUS;break;
        case PC_ONLY_CHILD: case PC_ONLY_OF_TYPE: if(sibling)sensitive|=SIBLING_NEXT|SIBLING_PREVIOUS;break;
        case PC_NTH_CHILD: case PC_NTH_OF_TYPE: if(sibling)sensitive|=SIBLING_FOLLOWING;break;
        case PC_NTH_LAST_CHILD: case PC_NTH_LAST_OF_TYPE: if(sibling)sensitive|=SIBLING_PRECEDING;break;
        default:break;
        }
        /* :has() has its own ancestor/witness invalidator. Logical selector
           lists can put an empty/state subject inside a complex alternative. */
        if(!(s->kind==SK_PC&&s->pc==PC_HAS))for(int k=0;s->args&&k<s->args->n;k++) {
            const struct compound *nested=s->args->v[k]->right;
            /* The argument's rightmost compound matches THIS subject. Fold
               its state sensitivity into the outer stable keys: publishing
               :empty from .named:not(:empty) as a universal subject would
               promote every unrelated mutation to its grandparent again.
               Left compounds describe other nodes, so retain their own keys
               for complex logical alternatives. */
            sensitive|=index_child_list_subjects_inner(a,index,nested,false,dependency);
            for(nested=nested->left;nested;nested=nested->left)
                index_child_list_subjects_inner(a,index,nested,true,dependency);
        }
    }
    if(!sensitive||!publish)return sensitive;
    struct selector *subject=ar_alloc(a,sizeof *subject);subject->right=(struct compound *)part;
    for(unsigned k=0;k<4;k++) {
        if(sibling&&!(sensitive&1u<<k))continue;
        struct css_index **target=dependency==SUBJECT_CHILD_OUTER?&index->child_list_subjects:
            dependency==SUBJECT_CHILD_PARENT?&index->child_list_parents:&index->child_list_siblings[k];
        if(!*target) {
            *target=ar_alloc(a,sizeof **target);
            ht_init(a,&(*target)->ids,16);
            ht_init(a,&(*target)->classes,32);
            ht_init(a,&(*target)->tags,16);
        }
        index_rule(a,*target,NULL,subject,0,0);
        if(!sibling)break;
    }
    return sensitive;
}
static void index_child_list_subjects(arena_t *a,struct css_index *index,const struct compound *part) {
    index_child_list_subjects_inner(a,index,part,true,SUBJECT_CHILD_OUTER);
}

/* What changing one class, id, attribute or dynamic state of an element can
   restyle, from where selectors name it. A selector relates an element only to
   its descendants and to its following siblings' subtrees (:has() looks the
   other way: its subjects are indexed apart), so the parent's subtree bounds
   every reach. Which of those it is follows from the combinator right of the
   compound that names the feature: a descendant or child combinator keeps the
   subject inside the element's subtree (a sibling of a descendant is one too),
   a sibling combinator moves it to the following siblings or their subtrees. */
enum {
    REACH_SELF = 1,        /* in a subject compound: the element itself */
    REACH_DESCENDANTS = 2, /* descendants carrying a key */
    REACH_SUBTREE = 4,     /* the same, but some subject carries no key: every descendant */
    REACH_PARENT = 8,      /* in a complex or positional argument, or a keyless sibling subject */
    REACH_SIBLINGS = 16,   /* following siblings carrying a key */
    REACH_SIBLING_DESCENDANTS = 32, /* descendants of following siblings carrying a key */
    REACH_KEYS_OVERFLOW = 64,       /* more keys than FEATURE_KEYS: a keyed reach takes its whole bound */
};
#define REACH_KEYED (REACH_DESCENDANTS | REACH_SIBLINGS | REACH_SIBLING_DESCENDANTS)
/* An id, class, type or attribute name that a subject compound requires (SK_*);
   a known type also keeps its tag, which is how match_compound tests it. */
struct feature_key { const char *name; uint32_t hash; uint16_t tag; uint8_t kind; };
/* Past this many keys a walk would cost as much as restyling the subtree. */
#define FEATURE_KEYS 64
struct css_feature { unsigned reach, nkeys, cap; struct feature_key *keys; };

static bool feature_key_equal(const struct feature_key *a, const struct feature_key *b) {
    return a->kind == b->kind && a->hash == b->hash && !strcmp(a->name, b->name);
}
static void feature_note(arena_t *a, struct css_feature **slot, unsigned reach, const struct feature_key *key) {
    struct css_feature *f = *slot;
    if (!f) f = *slot = ar_alloc(a, sizeof *f);
    if ((reach & REACH_KEYED) && !key) reach = reach == REACH_DESCENDANTS ? REACH_SUBTREE : REACH_PARENT;
    f->reach |= reach;
    if (!(reach & REACH_KEYED) || (f->reach & REACH_KEYS_OVERFLOW)) return;
    for (unsigned i = 0; i < f->nkeys; i++) if (feature_key_equal(&f->keys[i], key)) return;
    if (f->nkeys == FEATURE_KEYS) { f->reach |= REACH_KEYS_OVERFLOW; return; }
    if (f->nkeys == f->cap) {
        unsigned cap = f->cap ? f->cap * 2 : 4;
        struct feature_key *keys = ar_alloc(a, cap * sizeof *keys);
        if (f->nkeys) memcpy(keys, f->keys, f->nkeys * sizeof *keys);
        f->keys = keys; f->cap = cap;
    }
    f->keys[f->nkeys++] = *key;
}
static struct css_feature **feature_slot(arena_t *a, struct htab *t, const char *name) {
    return &ht_bucket(a, t, name, true)->feature;
}
static bool subject_key(const struct compound *c, struct feature_key *key) {
    static const uint8_t preference[] = {SK_ID, SK_CLASS, SK_TAG, SK_ATTR};
    for (size_t p = 0; p < sizeof preference; p++)
        for (int i = 0; i < c->n; i++)
            if (c->s[i].kind == preference[p]) {
                *key = (struct feature_key){c->s[i].name, hash_str(c->s[i].name), c->s[i].tag, preference[p]};
                return true;
            }
    return false;
}
/* The dynamic state a pseudo-class reads, or -1. */
static int pseudo_state(const struct simple *s) {
    if (s->kind != SK_PC) return -1;
    if (s->pc == PC_HOVER) return CSS_STATE_HOVER;
    if (s->pc == PC_ACTIVE) return CSS_STATE_ACTIVE;
    if (s->pc == PC_FOCUS || s->pc == PC_FOCUS_WITHIN) return CSS_STATE_FOCUS;
    return -1;
}
static bool anchor_compound(const struct compound *c) { return c->n == 1 && c->s[0].kind == SK_ANCHOR; }
/* Whether matching a selector counts siblings: a positional or :empty
   pseudo-class, or a sibling combinator between two of its compounds. */
static bool selector_structural(const struct compound *right) {
    for (const struct compound *c = right; c && !anchor_compound(c); c = c->left) {
        if (c->left && !anchor_compound(c->left) && (c->comb == '+' || c->comb == '~')) return true;
        for (int i = 0; i < c->n; i++) {
            const struct simple *s = &c->s[i];
            if (s->kind == SK_PC && (s->pc <= PC_NTH_LAST_OF_TYPE || s->pc == PC_EMPTY)) return true;
            if (s->args && !(s->kind == SK_PC && s->pc == PC_HAS))
                for (int k = 0; k < s->args->n; k++) if (selector_structural(s->args->v[k]->right)) return true;
        }
    }
    return false;
}
/* One :has() that a compound tests, indexed by that compound's keys. What its
   arguments name decides which mutations can change its match; where the
   compound sits, what a changed match restyles. */
struct has_subject {
    const struct sellist *args;
    struct css_feature *reach; /* from the compound's element, as for a feature there */
    bool siblings;  /* an argument starts with + or ~: the subject precedes what changed */
    bool children;  /* every argument is one compound after >: only the parent of what changed */
    bool structure; /* an argument counts siblings: any child-list change below can change it */
};
static void index_has(arena_t *a, struct css_index *x, const struct compound *subject, const struct sellist *args,
                      unsigned reach, const struct feature_key *key) {
    if (!x->relational) {
        x->relational = ar_alloc(a, sizeof *x->relational);
        ht_init(a, &x->relational->ids, 16);
        ht_init(a, &x->relational->classes, 32);
        ht_init(a, &x->relational->tags, 16);
    }
    struct has_subject *h = ar_alloc(a, sizeof *h);
    h->args = args; h->children = true;
    feature_note(a, &h->reach, reach, key);
    for (int k = 0; k < args->n; k++) {
        const struct compound *right = args->v[k]->right, *first = right;
        while (first->left && first->left->left) first = first->left;
        h->siblings |= first->comb == '+' || first->comb == '~';
        h->children &= first == right && first->comb == '>';
        h->structure |= selector_structural(right);
    }
    struct selector *selector = ar_alloc(a, sizeof *selector); selector->right = (struct compound *)subject;
    struct ient *entry = index_rule(a, x->relational, NULL, selector, 0, 0);
    if (entry) entry->has = h;
    x->relational_siblings |= h->siblings;
}
/* subject: the compound whose keys the element c names must carry. */
static void index_compound_features(arena_t *a, struct css_index *x, const struct compound *subject, const struct compound *c,
                                    unsigned reach, const struct feature_key *key) {
    for (int i = 0; i < c->n; i++) {
        const struct simple *s = &c->s[i];
        struct css_feature **slot = NULL;
        int state = pseudo_state(s);
        if (s->kind == SK_ID) slot = feature_slot(a, &x->feature_ids, s->name);
        else if (s->kind == SK_CLASS) slot = feature_slot(a, &x->feature_classes, s->name);
        else if (s->kind == SK_ATTR) slot = feature_slot(a, &x->feature_attrs, s->name);
        else if (state >= 0) slot = &x->feature_states[state];
        if (slot) feature_note(a, slot, reach, key);
        if (s->kind == SK_PC && s->pc == PC_HAS) { index_has(a, x, subject, s->args, reach, key); continue; }
        if (!s->args) continue;
        bool alternatives = s->kind == SK_PC && (s->pc == PC_NOT || s->pc == PC_IS);
        for (int k = 0; k < s->args->n; k++) {
            const struct compound *arg = s->args->v[k]->right;
            /* A compound alternative names this same element. A complex one,
               nth-child's "of S" or ::slotted() names others. */
            if (alternatives && !arg->left) index_compound_features(a, x, subject, arg, reach, key);
            else for (const struct compound *part = arg; part; part = part->left)
                index_compound_features(a, x, part, part, REACH_PARENT, NULL);
        }
    }
}
static void index_features(arena_t *a, struct css_index *x, const struct compound *right) {
    struct feature_key key;
    bool keyed = subject_key(right, &key);
    unsigned reach = REACH_SELF;
    for (const struct compound *c = right; c; c = c->left) {
        index_compound_features(a, x, c, c, reach, keyed ? &key : NULL);
        if (c->comb != '+' && c->comb != '~') reach = REACH_DESCENDANTS;
        else if (reach & (REACH_SELF | REACH_SIBLINGS)) reach = REACH_SIBLINGS;
        else reach = REACH_SIBLING_DESCENDANTS;
    }
}

static uint32_t index_media(arena_t *a, struct css_index *x, const struct mcond *m) {
    if (x->nmedia >= (1u << 24) - 1) ar_alloc(a, SIZE_MAX); /* ient.media's width */
    if (x->nmedia == x->media_cap) {
        uint32_t cap = x->media_cap ? x->media_cap * 2 : 8;
        if (cap <= x->media_cap) ar_alloc(a, SIZE_MAX);
        const struct mcond **media = ar_alloc(a, (size_t)cap * sizeof *media);
        if (x->nmedia) memcpy(media, x->media, x->nmedia * sizeof *media);
        x->media = media; x->media_cap = cap;
    }
    x->media[x->nmedia++] = m;
    return x->nmedia;
}
/* NULL when cancelled: a partial index is never published to the AST. */
static struct css_index *index_sheet(web_doc *d, sheet_t *s) {
    arena_t *a = s->arena;
    struct css_index *index = ar_alloc(a, sizeof *index);
    index->fonts = s->fonts; index->motions = s->motions;
    /* Small component sheets no longer allocate a document-sized hash
       table, nor duplicate selector entries for every adoption. */
    ht_init(a, &index->ids, 64);
    ht_init(a, &index->classes, 128);
    ht_init(a, &index->tags, 64);
    ht_init(a, &index->feature_ids, 32);
    ht_init(a, &index->feature_classes, 128);
    ht_init(a, &index->feature_attrs, 32);
    /* The rules of one @media block share its condition and are adjacent. */
    const struct mcond *last = NULL; uint32_t media = 0;
    for (struct rule *r = s->first; r; r = r->next, index->rules++) {
        if (!web_native_checkpoint(d)) return NULL;
        for(const struct mcond *m=r->media;m;m=m->up)if(m->container)index->containers=true;
        for(int k=0;k<r->ndecls;k++)if(container_units_used(r->decls[k].value))index->containers=true;
        if (!r->ndecls) continue;
        if (r->media != last) { last = r->media; media = last ? index_media(a, index, last) : 0; }
        for (int k = 0; k < r->sel->n; k++) {
            index->form_features |= selector_form_features(r->sel->v[k]->right);
            if(selector_siblings(r->sel->v[k]->right)) {
                index->siblings=true;
                for(const struct compound *part=r->sel->v[k]->right;part;part=part->left)
                    index_child_list_subjects(a,index,part);
            }
            for(const struct compound *part=r->sel->v[k]->right;part;part=part->left) {
                index_child_list_subjects_inner(a,index,part,true,SUBJECT_CHILD_PARENT);
                index_child_list_subjects_inner(a,index,part,true,SUBJECT_CHILD_SIBLING);
            }
            index_rule(a, index, r, r->sel->v[k], index->rules, media);
            index_features(a, index, r->sel->v[k]->right);
            if (d->profile_enabled) d->profile.style_index_entries++;
        }
    }
    return index;
}
/* The rule index of a sheet's AST, shared by every instance of that AST and
   kept across rescans for as long as the AST lives. */
static struct css_index *sheet_index(web_doc *d, const sheet_t *s) {
    sheet_t *ast = s->ast;
    if (!ast->index) ast->index = index_sheet(d, ast);
    return ast->index;
}
/* The index's @media for this cascade's viewport, evaluated once per index. */
static const bool *index_media_ok(struct css_ctx *x, const struct css_index *index, int vw, int vh, bool scripting) {
    static const bool unconditional = true;
    if (!index->nmedia) return &unconditional;
    unsigned slot = ((uintptr_t)index >> 4) & 63;
    for (const struct index_media *m = x->media[slot]; m; m = m->next) if (m->index == index) return m->ok;
    bool *ok = ar_alloc(&x->a, index->nmedia + 1);
    ok[0] = true;
    for (uint32_t i = 0; i < index->nmedia; i++) ok[i + 1] = media_chain_ok(index->media[i], vw, vh, scripting);
    struct index_media *m = ar_alloc(&x->a, sizeof *m);
    m->index = index; m->ok = ok; m->next = x->media[slot]; x->media[slot] = m;
    return ok;
}

/* Sheets in cascade order: imports keep their parent's order key, so a stable
   sort leaves them before it. */
static bool sheets_sort(web_doc *d) {
    pvec *sh = &d->sty.sheets;
    for (int i = 1; i < sh->n; i++)
        for (int j = i; j > 0 && ((sheet_t *)sh->v[j - 1])->order > ((sheet_t *)sh->v[j])->order; j--) {
            if (!web_native_checkpoint(d)) return false;
            void *t = sh->v[j];
            sh->v[j] = sh->v[j - 1];
            sh->v[j - 1] = t;
        }
    return true;
}
static int sheet_position(const pvec *sh, const sheet_t *s) {
    for (int i = 0; s && i < sh->n; i++) if (sh->v[i] == s) return i;
    return -1;
}
static struct sheet_signature sign_sheet(arena_t *a, const pvec *sh, int i) {
    const sheet_t *s = sh->v[i];
    const char *media = s->owner_media ? s->owner_media : "";
    return (struct sheet_signature){s->ast->serial, s->scope, a ? ar_strdup(a, media) : media,
                                    sheet_position(sh, s->import_parent), s->import_owner, s->ua};
}

/* The bindings of the document's sheets for one viewport. */
static struct css_ctx *build_ctx(web_doc *d, int vw, int vh) {
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
    pvec *sh = &d->sty.sheets;
    if (!sheets_sort(d)) return x;
    if (sh->n) x->sheets = ar_alloc(&x->a, (size_t)sh->n * sizeof *x->sheets);
    for (int i = 0; i < sh->n; i++) x->sheets[i] = sign_sheet(&x->a, sh, i);
    x->nsheets = sh->n;
    layers_build(d,x,vw,vh);
    bool scripting = web_js_enabled(d);
    uint32_t order = 1;
    for (int i = -2; i < sh->n; i++) {
        sheet_t *s = i == -2 ? ua_sheet : i == -1 ? quirks_sheet : sh->v[i];
        if (!sheet_media_ok(s,vw,vh,scripting)) continue;
        if (i == -1 && !d->quirks) continue;
        struct css_index *index = sheet_index(d, s);
        if (!index) return x;
        /* Registry identities are published before workers start. Worker
           font selection only reads this list and atomically marks demand. */
        for(struct font_face *font=s->fonts;font;font=font->next)
            for(struct font_source *source=font->sources;source;source=source->next)
                if(!doc_font_resource(d,source->url))x->parallel_safe=false;
        x->form_features |= index->form_features;
        x->relational |= index->relational != NULL;
        x->relational_siblings |= index->relational_siblings;
        x->siblings |= index->siblings;
        x->containers |= index->containers;
        struct css_binding *binding = ar_alloc(&x->a, sizeof *binding);
        binding->index = index; binding->order = order; order += index->rules;
        binding->media_ok = index_media_ok(x, index, vw, vh, scripting);
        binding->ua = s->ua;
        binding->layers=layer_sheet_find(x,s,false);
        if (s->ua) { binding->next = x->ua; x->ua = binding; continue; }
        node_t *root = s->scope ? s->scope : d->root;
        struct css_scope *scope = index_scope(x,root);
        if (scope->last) scope->last->next = binding; else scope->first = binding;
        scope->last = binding;
    }
    x->complete = !__atomic_load_n(&d->native_cancelled, __ATOMIC_ACQUIRE);
    return x;
}

static struct cascade_layer *binding_layer(const struct css_binding *binding,const struct css_layer_decl *syntax) {
    struct layer_sheet_map *map=binding->layers;
    struct cascade_layer *layer=map?(syntax?map->layers[syntax->id]:map->outer):NULL;
    return layer&&layer->depth?layer:NULL;
}
/* Font matching distance of a face for a computed weight and slope. */
static unsigned font_face_score(const struct font_face *f,unsigned weight,bool italic) {
    unsigned distance=weight<f->weight_lo?f->weight_lo-weight:weight>f->weight_hi?weight-f->weight_hi:0;
    return distance+(italic!=f->italic?10000u:0u);
}
/* A FontFace of document.fonts takes part in matching once its family and
   descriptors parsed; it follows the document's rules, as its font source
   orders them (CSS Font Loading 3), and wins equal matches as the later face. */
static struct css_font_face *author_font_face(web_doc *d,int i) {
    struct css_font_face *a=d->author_fonts.v[i];
    return a->face.family?a:NULL;
}
static struct font_face *scoped_font_face(web_doc *d,node_t *root,const char *family,unsigned weight,bool italic,int vw,int vh) {
    struct css_scope *scope=root?root->style_scope:NULL;
    struct font_face *best=NULL;unsigned score=UINT_MAX;
    struct cascade_layer *best_layer=NULL;
    bool scripting=d->sty.index_scripting;
    for(struct css_binding *binding=scope?scope->first:NULL;binding;binding=binding->next)
        for(struct font_face *f=binding->index->fonts;f;f=f->next) {
            if(strcasecmp(family,f->family)||!font_conditions_ok(f->media,vw,vh,scripting))continue;
            unsigned candidate=font_face_score(f,weight,italic);
            struct cascade_layer *layer=binding_layer(binding,f->layer);
            if(candidate<score || (candidate==score && (!best || layer_compare(layer,best_layer)>=0))) {
                best=f;score=candidate;best_layer=layer;
            }
        }
    for(int i=0;i<d->author_fonts.n;i++){
        struct css_font_face *a=author_font_face(d,i);
        if(!a || strcasecmp(family,a->face.family))continue;
        unsigned candidate=font_face_score(&a->face,weight,italic);
        if(candidate<=score){best=&a->face;score=candidate;best_layer=NULL;}
    }
    return best;
}
struct ranked_font_choice {struct web_font_choice choice;struct cascade_layer *layer;};
/* Add f's first usable source to a family's composite, by layer priority. */
static bool font_choice_add(web_doc *d,struct font_face *f,struct cascade_layer *layer,
                            struct web_font_choice **family_first,bool *restricted,arena_t *a) {
    for(struct font_source *src=f->sources;src;src=src->next){
        struct web_font_resource *r=font_source_resource(d,src);
        if(!r){ar_alloc(a,SIZE_MAX);return false;}if(r->failed)continue;
        struct ranked_font_choice *ranked=ar_alloc(a,sizeof *ranked);
        struct web_font_choice *choice=&ranked->choice;ranked->layer=layer;
        *choice=(struct web_font_choice){.resource=r,.face=r->face,.range_count=f->range_count};
        if(f->range_count){struct web_font_range *ranges=ar_alloc(a,f->range_count*sizeof *ranges);memcpy(ranges,f->ranges,f->range_count*sizeof *ranges);choice->ranges=ranges;*restricted=true;}
        struct web_font_choice **position=family_first;
        while(*position && layer_compare(ranked->layer,((struct ranked_font_choice *)*position)->layer)<0)position=&(*position)->next;
        choice->next=*position;*position=choice;break;
    }
    return true;
}
static const struct web_font_choice *select_font_choices(web_doc *d,node_t *node,style_t *style,int vw,int vh,arena_t *a) {
    if(!style->font_names)return NULL;
    node_t *root=doc_node_root(node,false);struct css_scope *scope=root?root->style_scope:NULL;
    const char *cursor=style->font_names,*end=cursor+strlen(cursor);
    struct web_font_choice *first=NULL,**tail=&first;bool restricted=false,italic=style->font_style!=0;
    while(cursor<end){
        const char *token=skip_ws(cursor,end);bool quoted=token<end&&(*token=='"'||*token=='\'');
        const char *family=css_font_family_next(a,&cursor,end);if(!family||!cursor)break;
        if(!quoted&&(!strcasecmp(family,"serif")||!strcasecmp(family,"sans-serif")||!strcasecmp(family,"monospace")||
           !strcasecmp(family,"system-ui")||!strcasecmp(family,"cursive")||!strcasecmp(family,"fantasy")))break;
        struct font_face *best=scoped_font_face(d,root,family,style->font_weight,italic,vw,vh);
        if(!best)continue;
        unsigned score=font_face_score(best,style->font_weight,italic);
        struct web_font_choice *family_first=NULL;
        for(struct css_binding *b=scope?scope->first:NULL;b;b=b->next)for(struct font_face *f=b->index->fonts;f;f=f->next){
            if(strcasecmp(family,f->family)||!font_conditions_ok(f->media,vw,vh,d->sty.index_scripting))continue;
            if(font_face_score(f,style->font_weight,italic)!=score)continue;
            if(!font_choice_add(d,f,binding_layer(b,f->layer),&family_first,&restricted,a))return NULL;
        }
        for(int i=0;i<d->author_fonts.n;i++){
            struct css_font_face *author=author_font_face(d,i);
            if(!author || strcasecmp(family,author->face.family) || font_face_score(&author->face,style->font_weight,italic)!=score)continue;
            /* A text run that needs a code point in its range requests it. */
            if(!font_choice_add(d,&author->face,NULL,&family_first,&restricted,a))return NULL;
        }
        if(family_first){*tail=family_first;while(*tail)tail=&(*tail)->next;}
    }
    return restricted?first:NULL;
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

static void mark_style_own(web_doc *d, node_t *n);
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
        style_t *styles[]={s,s->before_generated?s->before:NULL,s->after_generated?s->after:NULL,s->backdrop,base,
            base&&base->before_generated?base->before:NULL,base&&base->after_generated?base->after:NULL,base?base->backdrop:NULL};
        bool changed=false;
        for(size_t i=0;i<sizeof styles/sizeof *styles;i++)if(styles[i]) {
            font_t *face=select_web_font_uncached(d,n,styles[i],d->styled_w,d->styled_h,scratch);
            if(face!=styles[i]->named_font)changed=true;
            for(const struct web_font_choice *choice=styles[i]->font_choices;choice;choice=choice->next)
                if(choice->face!=choice->resource->face || choice->resource->failed)changed=true;
        }
        /* NULL -> NULL still marks the next source wanted, without recascading
           unchanged SVG snapshots or reshaping existing inline formatting. */
        if(changed) {
            mark_style_own(d,n); /* its descendants check their own selection here */
            /* A partial cascade resets wanted flags, including font users
               outside its dirty subtree. Retry their sources after publication. */
            d->font_selection_dirty=true;
        }
        ar_free(scratch);
    }
    ar_free(scratch);free(scratch);
}

/* ---------------------------------------------------------------- CSS Font Loading
   The native half of https://drafts.csswg.org/css-font-loading-3/: the
   document's CSS-connected faces (its @font-face rules), FontFace records,
   their load status and "find the matching font faces". js_fonts.h binds it. */
static void font_viewport(web_doc *d,int *vw,int *vh) {
    *vw=d->styled_w>0?d->styled_w:d->width>0?d->width:800;
    *vh=d->styled_h>0?d->styled_h:d->height>0?d->height:600;
}
/* The document's font source begins with a FontFace per @font-face rule of
   its style sheets (not a shadow root's), in document order: the cascade's
   sheet order, imports before their importer's rules. 0: unavailable now
   (cancelled native work); the caller keeps what it knew. */
int css_font_rules(web_doc *d,struct css_font_rule **rules,size_t *count) {
    *rules=NULL;*count=0;
    if(!d)return 1;
    if(d->native_cancelled || !sheets_sort(d))return 0;
    int vw,vh;font_viewport(d,&vw,&vh);bool scripting=web_js_enabled(d);
    const pvec *sh=&d->sty.sheets;
    struct css_font_rule *v=NULL;size_t n=0,capacity=0;
    for(int i=0;i<sh->n;i++){
        const sheet_t *s=sh->v[i];
        if(s->ua || s->scope || !s->fonts || !sheet_media_ok(s,vw,vh,scripting))continue;
        uint32_t occurrence=0;
        for(int j=0;j<i;j++){
            const sheet_t *p=sh->v[j];
            if(p->ast==s->ast && !p->ua && !p->scope && sheet_media_ok(p,vw,vh,scripting))occurrence++;
        }
        for(struct font_face *f=s->fonts;f;f=f->next){
            if(!font_conditions_ok(f->media,vw,vh,scripting))continue;
            if(n==capacity){
                size_t cap=capacity?capacity*2:16;
                void *grown=cap<=SIZE_MAX/sizeof *v?realloc(v,cap*sizeof *v):NULL;
                if(!grown){free(v);return -1;}
                v=grown;capacity=cap;
            }
            /* The shared AST's arena owns the rule (cssom.c may move it). */
            v[n++]=(struct css_font_rule){f,s->ast?s->ast->arena:s->arena,f->id,occurrence};
        }
    }
    *rules=v;*count=n;return 1;
}
const char *css_font_rule_descriptor(const struct font_face *face,enum css_font_descriptor which) {
    return face && (unsigned)which<CSS_FONT_DESCRIPTORS?face->desc[which]:NULL;
}
/* Load status of a face's src list, as CSS Fonts' "load a font" walks it: a
   failed source moves on to the next. started (a load() or a rendering use)
   demands the first untried source; a rule whose earlier source failed has
   been started by whoever used that resource. */
static enum css_font_status font_sources_status(web_doc *d,struct font_source *src,bool started) {
    bool tried=started;
    for(;src;src=src->next){
        struct web_font_resource *r=font_source_resource(d,src);
        if(!r)return tried?CSS_FONT_LOADING:CSS_FONT_UNLOADED;
        if(r->failed){tried=true;continue;}
        if(r->done)return CSS_FONT_LOADED;
        if(r->loading)return CSS_FONT_LOADING;
        if(!tried && !r->demanded)return CSS_FONT_UNLOADED;
        r->demanded=true; /* request_fonts sends it at the end of this tick */
        return CSS_FONT_LOADING;
    }
    return tried?CSS_FONT_ERROR:CSS_FONT_UNLOADED;
}
enum css_font_status css_font_rule_status(web_doc *d,struct font_face *face,bool started) {
    return face?font_sources_status(d,face->sources,started):CSS_FONT_ERROR;
}
/* Changing a font source or a face in it re-matches every element's fonts;
   committed shaping and min-content measurements used the old selection. */
static void font_source_changed(web_doc *d) {
    if(!d || !d->live)return;
    css_mark_dirty(d,NULL);d->dirty=true;
    d->layout_valid=false;layout_invalidate_fonts(d->root_box);
}
/* A CSS-connected FontFace's attribute writes its rule's descriptor too (CSS
   Font Loading 2.3). The rule's sheet arena keeps the new text. */
int css_font_rule_set(web_doc *d,const struct css_font_rule *rule,enum css_font_descriptor which,const char *value,size_t n) {
    if(!rule || !rule->face || !rule->arena || (unsigned)which>=CSS_FONT_DESCRIPTORS)return 0;
    struct font_face *f=rule->face;
    sbuf b={0};struct font_desc m={.weight_lo=f->weight_lo,.weight_hi=f->weight_hi,.italic=f->italic};
    int status=1;
    if(which==CSS_FONT_FAMILY)fd_put_family(&b,value,n);
    else status=font_desc_parse(which,value,n,&b,&m);
    if(status<=0){sb_free(&b);free(m.ranges);return status;}
    arena_t *a=rule->arena;jmp_buf trap,*previous=a->trap;a->trap=&trap;
    if(setjmp(trap)){a->trap=previous;sb_free(&b);free(m.ranges);return -1;}
    const char *text=ar_strndup(a,b.p?b.p:"",b.n);
    const char *family=which==CSS_FONT_FAMILY?ar_strndup(a,value,n):NULL;
    struct web_font_range *ranges=which==CSS_FONT_UNICODE_RANGE&&m.range_count?ar_alloc(a,m.range_count*sizeof *ranges):NULL;
    a->trap=previous;
    if(ranges)memcpy(ranges,m.ranges,m.range_count*sizeof *ranges);
    f->desc[which]=text;
    if(family)f->family=family;
    if(which==CSS_FONT_UNICODE_RANGE){f->ranges=ranges;f->range_count=m.range_count;}
    f->weight_lo=m.weight_lo;f->weight_hi=m.weight_hi;f->italic=m.italic;
    sb_free(&b);free(m.ranges);
    font_source_changed(d);
    return 1;
}
struct css_font_face *css_font_face_new(void) {
    struct css_font_face *f=calloc(1,sizeof *f);
    if(!f)return NULL;
    f->refs=1;f->face.author=f;f->face.weight_lo=f->face.weight_hi=400;
    f->face.id=__atomic_add_fetch(&font_face_serials,1,__ATOMIC_RELAXED);
    for(int i=0;i<CSS_FONT_DESCRIPTORS;i++)f->face.desc[i]=font_desc_initial[i];
    return f;
}
static void font_face_sources_free(struct css_font_face *f) {
    while(f->face.sources){
        struct font_source *src=f->face.sources;f->face.sources=src->next;
        free((char *)src->url);free(src);
    }
}
/* Never touches the resources: the record may outlive their document. */
void css_font_face_release(struct css_font_face *f) {
    if(!f || --f->refs)return;
    for(int i=0;i<CSS_FONT_DESCRIPTORS;i++)free(f->names[i]);
    free(f->family);free(f->ranges);font_face_sources_free(f);free(f);
}
uint64_t css_font_face_id(const struct css_font_face *f) { return f?f->face.id:0; }
const char *css_font_face_descriptor(const struct css_font_face *f,enum css_font_descriptor which) {
    return f && (unsigned)which<CSS_FONT_DESCRIPTORS?f->face.desc[which]:NULL;
}
/* Whether some document font source holds f (its wrapper holds one reference). */
static bool font_face_in_set(const struct css_font_face *f) { return f->refs>1; }
int css_font_face_set(web_doc *d,struct css_font_face *f,enum css_font_descriptor which,const char *value,size_t n) {
    if(!f || (unsigned)which>=CSS_FONT_DESCRIPTORS)return 0;
    sbuf b={0};struct font_desc m={.weight_lo=f->face.weight_lo,.weight_hi=f->face.weight_hi,.italic=f->face.italic};
    int status=1;
    if(which==CSS_FONT_FAMILY)fd_put_family(&b,value,n);
    else status=font_desc_parse(which,value,n,&b,&m);
    if(status<=0){sb_free(&b);free(m.ranges);return status;}
    if(which==CSS_FONT_FAMILY){
        /* The FontFace family is the literal name it matches (an invalid
           <family-name> is that name as a string), not a parsed list. */
        char *name=malloc(n+1);
        if(!name){sb_free(&b);return -1;}
        memcpy(name,value,n);name[n]=0;
        free(f->family);f->family=name;f->face.family=name;
    }
    free(f->names[which]);f->names[which]=sb_cstr(&b);f->face.desc[which]=f->names[which];
    f->face.weight_lo=m.weight_lo;f->face.weight_hi=m.weight_hi;f->face.italic=m.italic;
    if(which==CSS_FONT_UNICODE_RANGE){free(f->ranges);f->ranges=m.ranges;f->face.ranges=m.ranges;f->face.range_count=m.range_count;}
    else free(m.ranges);
    if(font_face_in_set(f))font_source_changed(d);
    return 1;
}
/* format() and tech() hints after a url(), syntactically. */
static bool font_src_hints_syntax(const char *s,const char *e) {
    while((s=skip_ws(s,e))<e){
        const char *name=s;
        while(s<e && ident_char((unsigned char)*s))s++;
        size_t n=(size_t)(s-name);
        if(!n || s>=e || *s!='(' || !((n==6 && strn_ieq(name,"format",6)) || (n==4 && strn_ieq(name,"tech",4))))return false;
        const char *close=scan_to(s+1,e,")");
        if(close>=e)return false;
        s=close+1;
    }
    return true;
}
/* A FontFace string source parses as the src descriptor (CSS Fonts 4
   forgiving <font-src-list>): invalid components drop, and only a list with
   none valid is a syntax error. local() is valid but no native face answers
   it, so that component always fails to load. */
int css_font_face_source(web_doc *d,struct css_font_face *f,const char *base,const char *value,size_t n) {
    if(!d || !f)return 0;
    arena_t *scratch=calloc(1,sizeof *scratch);
    if(!scratch)return -1;
    struct font_source *first=NULL,**tail=&first;
    jmp_buf trap;scratch->trap=&trap;scratch->chunk_size=1024;
    if(setjmp(trap)){
        ar_free(scratch);free(scratch);
        return -1;
    }
    struct pctx pc={.a=scratch,.base=base};
    bool valid=false;
    struct font_source *found=NULL,**found_tail=&found;
    for(const char *s=value,*e=value+n;s<e;){
        const char *end=scan_to(s,e,","),*c=skip_ws(s,end),*z=end;trim_r(c,&z);
        if(z-c>=4 && strn_ieq(c,"url(",4)){
            const char *close=scan_to(c+4,z,")");
            if(close<z && font_src_hints_syntax(close+1,z)){
                valid=true;
                struct font_source *sources=font_sources(&pc,c,z);
                *found_tail=sources;while(*found_tail)found_tail=&(*found_tail)->next;
            }
        }else if(z-c>=6 && strn_ieq(c,"local(",6)){
            const char *close=scan_to(c+6,z,")");
            if(close==z-1 && skip_ws(c+6,close)<close)valid=true;
        }
        s=end<e?end+1:e;
    }
    scratch->trap=NULL;
    /* Copy out of the scratch arena; each URL is a document resource. */
    int status=valid?1:0;
    for(struct font_source *src=valid?found:NULL;src;src=src->next){
        struct font_source *copy=calloc(1,sizeof *copy);
        char *url=copy?strdup(src->url):NULL;
        struct web_font_resource *r=url?doc_font_resource(d,url):NULL;
        if(!r){free(url);free(copy);status=-1;break;}
        copy->url=url;copy->resource=r;*tail=copy;tail=&copy->next;
    }
    ar_free(scratch);free(scratch);
    if(status<=0){
        while(first){struct font_source *src=first;first=src->next;free((char *)src->url);free(src);}
        return status;
    }
    font_face_sources_free(f);f->face.sources=first;
    return 1;
}
int css_font_face_data(web_doc *d,struct css_font_face *f,const void *bytes,size_t n) {
    if(!d || !f)return 0;
    font_t *face=bytes && n?font_open_memory(bytes,n):NULL;
    if(!face)return 0;
    struct web_font_resource *r=calloc(1,sizeof *r);
    struct font_source *src=r?calloc(1,sizeof *src):NULL;
    if(!src){free(r);font_close(face);return -1;}
    /* Styles may borrow the face after the record goes: the document keeps it. */
    r->face=face;r->done=true;r->next=d->font_data;d->font_data=r;
    src->resource=r;font_face_sources_free(f);f->face.sources=src;f->data=r;
    if(font_face_in_set(f))font_source_changed(d);
    return 1;
}
enum css_font_status css_font_face_status(web_doc *d,struct css_font_face *f,bool load) {
    if(!f)return CSS_FONT_ERROR;
    if(load)__atomic_store_n(&f->started,true,__ATOMIC_RELAXED);
    if(f->data)return f->data->face?CSS_FONT_LOADED:CSS_FONT_ERROR;
    /* Until load() or a rendering use, a FontFace stays unloaded even when
       another face already fetched the same URL. */
    if(!__atomic_load_n(&f->started,__ATOMIC_RELAXED))return CSS_FONT_UNLOADED;
    return font_sources_status(d,f->face.sources,true);
}
bool css_font_set_add(web_doc *d,struct css_font_face *f) {
    if(!d || !f)return false;
    for(int i=0;i<d->author_fonts.n;i++)if(d->author_fonts.v[i]==f)return true;
    pv_push(&d->author_fonts,f);f->refs++;
    font_source_changed(d);
    return true;
}
void css_font_set_delete(web_doc *d,struct css_font_face *f) {
    for(int i=0;d && i<d->author_fonts.n;i++)if(d->author_fonts.v[i]==f){
        memmove(d->author_fonts.v+i,d->author_fonts.v+i+1,sizeof(void *)*(size_t)(d->author_fonts.n-i-1));
        d->author_fonts.n--;css_font_face_release(f);font_source_changed(d);return;
    }
}
void css_font_set_clear(web_doc *d) {
    if(!d || !d->author_fonts.n)return;
    for(int i=0;i<d->author_fonts.n;i++)css_font_face_release(d->author_fonts.v[i]);
    d->author_fonts.n=0;font_source_changed(d);
}
void css_font_set_free(web_doc *d) {
    for(int i=0;i<d->author_fonts.n;i++)css_font_face_release(d->author_fonts.v[i]);
    pv_free(&d->author_fonts);
}
/* CSS Font Loading 3.1: a CSS-wide keyword, or a var()/env()/attr() that
   nothing can substitute here, is a syntax error before the font shorthand
   grammar runs. Strings may still contain those words. */
static bool font_query_plain(const char *s,size_t n) {
    static const char *const wide[]={"inherit","initial","unset","revert","revert-layer","default",NULL};
    const char *e=s+n;s=skip_ws(s,e);trim_r(s,&e);
    for(int i=0;wide[i];i++)if((size_t)(e-s)==strlen(wide[i]) && strn_ieq(s,wide[i],(size_t)(e-s)))return false;
    for(const char *p=s;p<e;p++){
        if(*p=='"' || *p=='\''){char q=*p++;while(p<e && *p!=q){if(*p=='\\')p++;p++;}continue;}
        if(*p=='\\'){p++;continue;}
        if(p>s && ident_char((unsigned char)p[-1]))continue;
        static const char *const functions[]={"var(","env(","attr(",NULL};
        for(int i=0;functions[i];i++){
            size_t k=strlen(functions[i]);
            if((size_t)(e-p)>=k && !strncasecmp(p,functions[i],k))return false;
        }
    }
    return true;
}
static bool font_text_points(const char *text,size_t n,uint32_t **points,size_t *count) {
    *points=NULL;*count=0;
    uint32_t *v=n?malloc(n*sizeof *v):NULL;
    if(n && !v)return false;
    size_t k=0;
    for(size_t i=0;i<n;){
        unsigned char c=(unsigned char)text[i];uint32_t cp;size_t len;
        if(c<0x80){cp=c;len=1;}
        else if((c&0xe0)==0xc0 && i+1<n){cp=((c&0x1fu)<<6)|(text[i+1]&0x3fu);len=2;}
        else if((c&0xf0)==0xe0 && i+2<n){cp=((c&0x0fu)<<12)|((text[i+1]&0x3fu)<<6)|(text[i+2]&0x3fu);len=3;}
        else if((c&0xf8)==0xf0 && i+3<n){cp=((c&0x07u)<<18)|((text[i+1]&0x3fu)<<12)|((text[i+2]&0x3fu)<<6)|(text[i+3]&0x3fu);len=4;}
        else {cp=0xfffd;len=1;}
        v[k++]=cp;i+=len;
    }
    *points=v;*count=k;return true;
}
/* Whether a face's unicode-range covers any code point of the text. */
static bool font_face_covers(const struct font_face *f,const uint32_t *points,size_t count) {
    if(!count)return false;
    if(!f->range_count)return true;
    for(size_t i=0;i<count;i++){
        size_t lo=0,hi=f->range_count;
        while(lo<hi){size_t mid=lo+(hi-lo)/2;if(points[i]>f->ranges[mid].last)lo=mid+1;else hi=mid;}
        if(lo<f->range_count && points[i]>=f->ranges[lo].first)return true;
    }
    return false;
}
struct font_match_work {
    struct css_font_rule *rules;size_t rule_count;
    struct css_font_match *matches;size_t count,capacity;
    uint32_t *points;size_t point_count;
};
static bool font_match_push(struct font_match_work *w,struct css_font_match match) {
    for(size_t i=0;i<w->count;i++)
        if(w->matches[i].id==match.id && w->matches[i].occurrence==match.occurrence && w->matches[i].author==match.author)return true;
    if(w->count==w->capacity){
        size_t cap=w->capacity?w->capacity*2:8;
        void *grown=cap<=SIZE_MAX/sizeof *w->matches?realloc(w->matches,cap*sizeof *w->matches):NULL;
        if(!grown)return false;
        w->matches=grown;w->capacity=cap;
    }
    w->matches[w->count++]=match;return true;
}
/* For each family of the font list, the faces font matching selects (all of
   a composite face's unicode-range parts), then only those covering a code
   point of the text. System fonts are never listed: they are loaded. */
int css_font_match(web_doc *d,const char *font,size_t n,const char *text,size_t tn,
                   struct css_font_match **matches,size_t *count) {
    *matches=NULL;*count=0;
    if(!d || !font_query_plain(font,n))return 0;
    const struct propdef *p=css_prop_lookup("font",4);
    if(!p || !css_value_supported(p,font,n))return 0;
    struct font_match_work *w=calloc(1,sizeof *w);
    arena_t *arena=calloc(1,sizeof *arena);
    if(!w || !arena || css_font_rules(d,&w->rules,&w->rule_count)<0 || !font_text_points(text,tn,&w->points,&w->point_count)){
        if(w){free(w->rules);free(w);}
        free(arena);return -1;
    }
    int status=-1;
    jmp_buf trap;arena->trap=&trap;arena->chunk_size=4096;
    if(!setjmp(trap)){
        style_t parent,st;css_style_init(&parent,NULL);css_style_init(&st,&parent);
        int vw,vh;font_viewport(d,&vw,&vh);
        uint8_t *set=ar_alloc(arena,(size_t)css_prop_count());
        struct cx cx={.s=&st,.parent=&parent,.em=parent.font_size,.rem=16,.vw=(float)vw,.vh=(float)vh,.a=arena,.set=set};
        status=0;
        if(css_apply(p,font,n,&cx) && !cx.invalid && st.font_names) {
            status=1;
            bool italic=st.font_style!=0;
            const char *cursor=st.font_names,*end=cursor+strlen(cursor);
            while(status>0 && cursor<end){
                const char *token=skip_ws(cursor,end);bool quoted=token<end && (*token=='"' || *token=='\'');
                const char *family=css_font_family_next(arena,&cursor,end);
                if(!family || !cursor)break;
                if(!quoted && (!strcasecmp(family,"serif") || !strcasecmp(family,"sans-serif") || !strcasecmp(family,"monospace") ||
                   !strcasecmp(family,"system-ui") || !strcasecmp(family,"cursive") || !strcasecmp(family,"fantasy")))continue;
                unsigned best=UINT_MAX;
                for(size_t i=0;i<w->rule_count;i++)
                    if(!strcasecmp(family,w->rules[i].face->family))best=MIN(best,font_face_score(w->rules[i].face,st.font_weight,italic));
                for(int i=0;i<d->author_fonts.n;i++){
                    struct css_font_face *a=author_font_face(d,i);
                    if(a && !strcasecmp(family,a->face.family))best=MIN(best,font_face_score(&a->face,st.font_weight,italic));
                }
                if(best==UINT_MAX)continue;
                for(size_t i=0;i<w->rule_count && status>0;i++){
                    struct font_face *f=w->rules[i].face;
                    if(strcasecmp(family,f->family) || font_face_score(f,st.font_weight,italic)!=best ||
                       !font_face_covers(f,w->points,w->point_count))continue;
                    if(!font_match_push(w,(struct css_font_match){w->rules[i].id,w->rules[i].occurrence,NULL}))status=-1;
                }
                for(int i=0;i<d->author_fonts.n && status>0;i++){
                    struct css_font_face *a=author_font_face(d,i);
                    if(!a || strcasecmp(family,a->face.family) || font_face_score(&a->face,st.font_weight,italic)!=best ||
                       !font_face_covers(&a->face,w->points,w->point_count))continue;
                    if(!font_match_push(w,(struct css_font_match){a->face.id,0,a}))status=-1;
                }
            }
        }
    }
    arena->trap=NULL;ar_free(arena);free(arena);
    free(w->rules);free(w->points);
    if(status>0){*matches=w->matches;*count=w->count;}else free(w->matches);
    free(w);
    return status;
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
    /* HTML's legacy wrap=off is a presentational hint, below author CSS.
       The native control consumes this computed value, not the attribute. */
    if (!e->foreign && t == T_textarea && (v = node_attr(e,"wrap")) && str_ieq(v,"off"))
        hint_value(h,"white-space","pre");
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
        node_t *tab = node_ancestor(e, T_table); /* hint_reaches_descendants() */
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
/* The attributes of e whose hints above belong to its descendants: a table's
   cellpadding and border pad and border its cells. */
static bool hint_reaches_descendants(const node_t *e, const char *name) {
    return !e->foreign && e->tag == T_table && (!strcmp(name, "cellpadding") || !strcmp(name, "border"));
}

/* ---------------------------------------------------------------- the cascade */
struct dent {
    const struct decl *d;
    uint64_t key;
    uint8_t pseudo;
    int scope_depth;
    bool animation;  /* the animation or transition origin (CSS Cascade 5 §6.1) */
    bool transition; /* the transition origin, above every other */
    bool inline_style;
    struct cascade_layer *layer;
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
    dents[ndents].animation = dents[ndents].transition = false;
    dents[ndents].inline_style=false;dents[ndents].layer=NULL;
    ndents++;
}

static void add_rule_decls(const struct rule *r, uint32_t spec, uint32_t order, bool author, uint8_t pseudo, int depth,struct cascade_layer *layer) {
    for (int i = 0; i < r->ndecls; i++) {
        uint64_t key = (author ? 1ull << 62 : 0) | (uint64_t)(spec & 0x3FFFFFFF) << 32 |
                       (uint64_t)(order & 0xFFFFFF) << 8 | (uint64_t)(i < 255 ? i : 255);
        add_dent(&r->decls[i], key, pseudo, depth);
        dents[ndents-1].layer=layer;
    }
}

/* What collect() looks up for the element being styled, hashed once per
   element rather than once per stylesheet binding. */
#define ELEMENT_KEY_CLASSES 32
struct element_keys {
    node_t *e, *root;
    uint32_t id_hash, name_hash;
    int nclasses; /* hashed: the first ELEMENT_KEY_CLASSES of e->classes */
    uint32_t class_hash[ELEMENT_KEY_CLASSES];
    uint32_t class_repeat; /* bit i: classes[i] repeats an earlier class */
    struct ancestor_view ancestors;
    bool service; /* this thread runs the native checkpoints */
};
static void element_keys_init(struct element_keys *k, node_t *e, struct ancestor_view ancestors) {
    k->e = e; k->root = doc_node_root(e, false); k->ancestors = ancestors;
    k->id_hash = e->id ? hash_str(e->id) : 0;
    k->name_hash = hash_str(e->name);
    k->nclasses = e->nclasses < ELEMENT_KEY_CLASSES ? e->nclasses : ELEMENT_KEY_CLASSES;
    k->class_repeat = 0;
    for (int i = 0; i < k->nclasses; i++) {
        k->class_hash[i] = hash_str(e->classes[i]);
        for (int j = 0; j < i; j++)
            if (k->class_hash[j] == k->class_hash[i] && !strcmp(e->classes[i], e->classes[j])) {
                k->class_repeat |= 1u << i; break;
            }
    }
    k->service = css_can_service();
}
/* false: classes[i] repeats an earlier class, whose rules are collected once. */
static bool element_class_key(const struct element_keys *k, int i, uint32_t *hash) {
    const node_t *e = k->e;
    if (i < k->nclasses) { *hash = k->class_hash[i]; return !(k->class_repeat >> i & 1); }
    for (int j = 0; j < i; j++) if (!strcmp(e->classes[i], e->classes[j])) return false;
    *hash = hash_str(e->classes[i]); return true;
}
static const struct ient *ht_list(const struct htab *t, const char *key, uint32_t hash) {
    if (!t->cap) return NULL;
    for (const struct bucket *b = t->v[hash & (t->cap - 1)]; b; b = b->next)
        if (b->hash == hash && !strcmp(b->key, key)) return b->list;
    return NULL;
}

/* --debug-js counts for one element, added to the document's profile once:
   candidates by index bucket (id, class, tag, universal) and how many of them
   the ancestor filter rejected. Style workers share those counters. */
enum { COLLECT_ID, COLLECT_CLASS, COLLECT_TAG, COLLECT_UNIVERSAL };
struct collect_tally { uint64_t bucket[4], filtered, indexes; };

static void collect(const struct ient *l, const struct element_keys *k, node_t *scope, int depth, struct css_binding *binding,
                    struct collect_tally *tally, int bucket) {
    node_t *e = k->e, *root = k->root;
    web_doc *d = e->owner;
    struct css_scratch *scratch = css_scratch_get();
    /* A safepoint every 32 candidates: most cost a few loads each. */
    for (unsigned n = 0; l; l = l->next, n++) {
        if (n % 32 == 0) {
            if (k->service) {
                if (!web_native_checkpoint(d)) return;
                web_avmedia_checkpoint();
            } else if (__atomic_load_n(&d->native_cancelled, __ATOMIC_ACQUIRE)) return;
        }
        tally->bucket[bucket]++;
        if (!binding->ua) {
            if (!scope && root && root->shadow_host) continue;
            if (scope && l->pseudo != PE_SLOTTED && root != scope && e != scope->shadow_host) continue;
        }
        if (!binding->media_ok[l->media]) continue;
        if (ancestor_filter_rejects(&k->ancestors, l->ancestor_hashes)) { tally->filtered++; continue; }
        if (!container_chain_ok(l->r->media,e,l->pseudo==PE_BEFORE || l->pseudo==PE_AFTER)) continue;
        /* Only cascade rule matching reads the published form snapshot. Live
           matches()/querySelector() outside this call keep their normal API. */
        bool previous = scratch->form_snapshot;
        scratch->form_snapshot = true;
        bool matches = match_from(l->sel->right, e, scope);
        scratch->form_snapshot = previous;
        if (matches) {
            struct cascade_layer *layer=binding_layer(binding,l->r->layer);
            add_rule_decls(l->r, l->sel->spec, l->order + binding->order, !binding->ua,
                           l->pseudo == PE_SLOTTED ? PE_NONE : l->pseudo, depth,layer);
        }
    }
}

static void collect_index(struct css_binding *binding, const struct element_keys *k, node_t *scope) {
    const node_t *e = k->e;
    int depth = scope_depth(scope);
    struct collect_tally tally = {0};
    for (; binding; binding = binding->next) {
        struct css_index *x = binding->index;
        tally.indexes++;
        if (e->id) collect(ht_list(&x->ids, e->id, k->id_hash), k, scope, depth, binding, &tally, COLLECT_ID);
        for (int i = 0; i < e->nclasses; i++) {
            uint32_t hash;
            if (element_class_key(k, i, &hash))
                collect(ht_list(&x->classes, e->classes[i], hash), k, scope, depth, binding, &tally, COLLECT_CLASS);
        }
        collect(ht_list(&x->tags, e->name, k->name_hash), k, scope, depth, binding, &tally, COLLECT_TAG);
        collect(x->univ, k, scope, depth, binding, &tally, COLLECT_UNIVERSAL);
    }
    web_doc *d = e->owner;
    if (!d->profile_enabled) return;
    uint64_t checks = 0;
    for (int i = 0; i < 4; i++) {
        checks += tally.bucket[i];
        if (tally.bucket[i]) __atomic_fetch_add(&d->profile.style_rule_bucket[i], tally.bucket[i], __ATOMIC_RELAXED);
    }
    if (checks) __atomic_fetch_add(&d->profile.style_rule_checks, checks, __ATOMIC_RELAXED);
    if (tally.filtered) __atomic_fetch_add(&d->profile.style_rule_filtered, tally.filtered, __ATOMIC_RELAXED);
    __atomic_fetch_add(&d->profile.style_rule_indexes, tally.indexes, __ATOMIC_RELAXED);
}

static void collect_scope(node_t *root, const struct element_keys *k) {
    if (root && root->style_scope)
        collect_index(root->style_scope->first, k, root->shadow_host ? root : NULL);
}

static int cmp_dent(const void *a, const void *b) {
    const struct dent *da = a, *db = b;
    if (da->transition != db->transition) return da->transition ? -1 : 1;
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
    if((x>>62)==(y>>62)) {
        if(da->inline_style!=db->inline_style)return da->inline_style?-1:1;
        int layer=layer_compare(da->layer,db->layer);
        if(layer)return da->d->important?layer:-layer;
    }
    return x < y ? 1 : x > y ? -1 : 0;
}

/* Name tables for custom properties. A page may declare thousands of them on
   one element and reference them from every descendant: duplicate checks,
   cycle detection and var() lookup must not scan or rehash those chains. */
#define VAR_SEGMENT_INDEX_MIN 16
struct var_slot { const char *name; size_t length; uint32_t hash; void *value; };
struct var_segment { const struct custom_prop *end; size_t capacity; struct var_slot slots[]; };

static uint32_t var_name_hash(const char *name, size_t n) {
    uint32_t hash = 2166136261u;
    for (size_t i = 0; i < n; i++) hash = (hash ^ (unsigned char)name[i]) * 16777619u;
    return hash;
}
static size_t var_slot_capacity(size_t count) {
    size_t capacity = 32;
    while (capacity < count * 2) capacity *= 2;
    return capacity;
}
/* The capacity is at least twice the insertion count, so a free slot exists.
   The first value stored under a name is kept. */
static void var_slot_put(struct var_slot *slots, size_t capacity, const char *name, size_t length, void *value) {
    uint32_t hash = var_name_hash(name, length);
    for (size_t slot = hash & (capacity - 1);; slot = (slot + 1) & (capacity - 1)) {
        struct var_slot *s = &slots[slot];
        if (!s->name) { *s = (struct var_slot){name, length, hash, value}; return; }
        if (s->hash == hash && s->length == length && !memcmp(s->name, name, length)) return;
    }
}
static void *var_slot_get(const struct var_slot *slots, size_t capacity, const char *name, size_t length) {
    uint32_t hash = var_name_hash(name, length);
    for (size_t slot = hash & (capacity - 1); slots[slot].name; slot = (slot + 1) & (capacity - 1)) {
        const struct var_slot *s = &slots[slot];
        if (s->hash == hash && s->length == length && !memcmp(s->name, name, length)) return s->value;
    }
    return NULL;
}

/* The chain is one segment per declaring ancestor, nearest first. Indexed
   segments are immutable once published, so workers share them unlocked. */
static const struct custom_prop *find_var(const struct custom_prop *v, const char *name, size_t n) {
    while (v) {
        if (v->segment) {
            const struct custom_prop *found = var_slot_get(v->segment->slots, v->segment->capacity, name, n);
            if (found) return found;
            v = v->segment->end;
            continue;
        }
        if (strlen(v->name) == n && !memcmp(v->name, name, n)) return v;
        v = v->next;
    }
    return NULL;
}
const struct custom_prop *css_custom_property(const style_t *s, const char *name) {
    return s && name ? find_var(s->vars, name, strlen(name)) : NULL;
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
/* This element's declarations as graph vertices; indexed when numerous. */
struct var_walks { struct var_walk *all; struct var_slot *slots; size_t capacity; };

static struct var_walk *var_lookup(const struct var_walks *walks, const char *name, size_t n) {
    if (walks->slots) return var_slot_get(walks->slots, walks->capacity, name, n);
    for (struct var_walk *v = walks->all; v; v = v->next)
        if (strlen(v->property->name) == n && !memcmp(v->property->name, name, n)) return v;
    return NULL;
}

/* Iterate every var() token, including references in an unused fallback.
   Strings and hash/at-keywords are not variable function tokens. */
static struct var_walk *var_dependency(struct var_walk *f, const struct var_walks *all) {
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

static bool value_references(const char *s);
static bool subst(const char *s, size_t n, const struct custom_prop *vars, sbuf *out);
bool css_substitute(const style_t *st, const char *v, sbuf *out) {
    size_t n = strlen(v);
    if (!value_references(v)) { sb_put(out, v, n); return true; }
    return subst(v, n, st ? st->vars : NULL, out);
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
    struct var_walk *all=NULL;size_t count=0;
    for (struct custom_prop *m=mine;m!=inherited;m=m->next) {
        struct var_walk *v=ar_alloc(&work->arena,sizeof *v);
        v->property=m;v->s=m->value?m->value:"";v->e=v->s+strlen(v->s);v->next=all;all=v;count++;
    }
    struct var_walks walks={all,NULL,0};
    if (count>=VAR_SEGMENT_INDEX_MIN) {
        walks.capacity=var_slot_capacity(count);
        walks.slots=ar_alloc(&work->arena,walks.capacity*sizeof *walks.slots);
        for (struct var_walk *v=all;v;v=v->next)
            var_slot_put(walks.slots,walks.capacity,v->property->name,strlen(v->property->name),v);
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
            struct var_walk *dep=var_dependency(f,&walks);
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

/* Why an element restyles: the whole cascade, it has no style yet, it is in a
   marked subtree, an ancestor's inherited values changed, or its own match. */
/* What marked a subtree for restyle (node_t.style_mark_source). */
enum { MARK_BOUNDARY, MARK_REACH, MARK_INSERTED, MARK_POSITION, MARK_RELATIONAL, MARK_PARENT_STATE, MARK_SOURCES };
enum { REDO_CASCADE, REDO_NEW, REDO_INHERITED, REDO_SELF, REDO_MARKED, REDO_WHYS = REDO_MARKED + MARK_SOURCES };
_Static_assert(REDO_WHYS == sizeof ((struct web_profile *)0)->style_redo / sizeof (uint64_t), "one counter per REDO_*");
struct cascade {
    web_doc *d;
    float rem;
    int vw, vh;
    sbuf vbuf;
    arena_t *a;
    bool full;
    uint8_t why; /* --debug-js, while full: why the elements below restyle (REDO_*) */
    jmp_buf *trap;
    struct cascade_frontier *frontier;
    /* The ancestor filter of the element being styled (its job's snapshot
       plus this walk's counts), and the index of a snapshot of it in
       frontier->filters for its queued children (SIZE_MAX: not taken yet). */
    const struct ancestor_filter *ancestor_base;
    struct ancestor_counts *ancestor_counts;
    size_t ancestors_published;
};

struct css_rollback {const struct dent *from;bool origin;};
static bool dent_author(const struct dent *d) {return ((d->key>>62)&1)!=d->d->important;}
static bool rollback_blocks(const struct css_rollback *rollback,const struct dent *candidate) {
    const struct dent *from=rollback->from;if(!from)return false;
    if(from->animation)return candidate->animation;
    if(rollback->origin)return candidate->animation?dent_author(from):dent_author(candidate)==dent_author(from);
    if(from->inline_style) return candidate->animation?from->d->important:
        candidate->inline_style&&dent_author(candidate)==dent_author(from)&&candidate->scope_depth==from->scope_depth;
    if(candidate->animation)return from->d->important;
    if(dent_author(candidate)!=dent_author(from) || candidate->scope_depth!=from->scope_depth)return false;
    return layer_compare(candidate->layer,from->layer)>=0;
}
static void apply_decl(struct cascade *c, struct cx *cx, const struct dent *entry,struct css_rollback *rollback,bool *has_rollback) {
    const struct decl *d=entry->d;
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
    const char *end=v+n;v=skip_ws(v,end);trim_r(v,&end);n=(size_t)(end-v);
    bool revert_layer=strn_ieq(v,"revert-layer",n),revert_origin=strn_ieq(v,"revert",n);
    if(!*has_rollback&&!revert_layer&&!revert_origin){css_apply(d->p,v,n,cx);return;}
    int count=css_prop_count();uint8_t hidden[count];bool was_rollback=*has_rollback;
    if(!was_rollback)memset(rollback,0,(size_t)count*sizeof *rollback);
    if(was_rollback)for(int i=0;i<count;i++) {
        hidden[i]=!cx->set[i]&&rollback_blocks(&rollback[i],entry);
        if(hidden[i])cx->set[i]=1;
    }
    if(revert_layer||revert_origin) {
        /* Expand through the real shorthand parser into a scratch set-mask,
           so logical aliases/all/font reset exactly their native longhands. */
        uint8_t affected[count];memset(affected,0,sizeof affected);
        style_t scratch=*cx->s;struct cx probe=*cx;probe.s=&scratch;probe.set=affected;
        if(css_apply(d->p,"initial",7,&probe))for(int i=0;i<count;i++)
            if(affected[i]&&!cx->set[i]){rollback[i]=(struct css_rollback){entry,revert_origin};*has_rollback=true;}
    } else css_apply(d->p, v, n, cx);
    /* Restore only temporarily hidden bits. Real winners remain consumed. */
    if(was_rollback)for(int i=0;i<count;i++)if(hidden[i])cx->set[i]=0;
}

static style_t *compute(struct cascade *c, node_t *e, const style_t *parent, uint8_t pseudo) {
    web_doc *d = c->d;
    style_t *s = style_slot_alloc(d, e, c->a);
    css_style_init(s, parent);
    /* SVG 2 UA rule: graphics inside an <svg> have no CSS layout box and
       transform about the view-box origin. The outermost <svg> keeps the
       CSS default, as does HTML inside foreignObject (another namespace). */
    if (pseudo == PE_NONE && e->namespace_id == NS_SVG && e->parent && e->parent->type == N_ELEM &&
        e->parent->namespace_id == NS_SVG) {
        s->transform_origin[0].pct = s->transform_origin[1].pct = 0;
    }
    /* Native placeholder text has a UA colour, not the input's colour.
       Author declarations still cascade normally, including explicit inherit,
       currentColor, transparent and revert. No second rule scan at paint time. */
    if (pseudo == PE_PLACEHOLDER) s->color = RGB(117, 117, 117);
    /* custom properties: the most important declaration of each name */
    const struct custom_prop *inherited = s->vars;
    struct custom_prop *mine = NULL;
    struct custom_rollback {const char *name;struct css_rollback state;struct custom_rollback *next;} *custom_rollbacks=NULL;
    size_t own = 0;
    for (int i = 0; i < ndents; i++)
        if (dents[i].pseudo == pseudo && !dents[i].d->p) own++;
    struct var_segment *segment = NULL;
    if (own >= VAR_SEGMENT_INDEX_MIN) {
        size_t capacity = var_slot_capacity(own);
        segment = ar_alloc(c->a, sizeof *segment + capacity * sizeof *segment->slots);
        segment->end = inherited; segment->capacity = capacity;
    }
    for (int i = 0; i < ndents; i++) {
        const struct decl *x = dents[i].d;
        if (dents[i].pseudo != pseudo || x->p) continue;
        bool dup = false;
        if (segment) dup = var_slot_get(segment->slots, segment->capacity, x->name, strlen(x->name)) != NULL;
        else for (struct custom_prop *m = mine; m; m = m->next)
            if (!strcmp(m->name, x->name)) dup = true;
        if (dup) continue;
        struct custom_rollback *rolled=custom_rollbacks;
        while(rolled&&strcmp(rolled->name,x->name))rolled=rolled->next;
        if(rolled&&rollback_blocks(&rolled->state,&dents[i]))continue;
        bool revert_layer=!strcasecmp(x->value,"revert-layer"),revert_origin=!strcasecmp(x->value,"revert");
        if(revert_layer||revert_origin) {
            if(!rolled){rolled=ar_alloc(c->a,sizeof *rolled);rolled->name=x->name;rolled->next=custom_rollbacks;custom_rollbacks=rolled;}
            rolled->state=(struct css_rollback){&dents[i],revert_origin};continue;
        }
        struct custom_prop *cp = ar_alloc(c->a, sizeof *cp);
        /* Parsed inline/effect blocks may be replaced before the next
           cascade. Computed variable chains own their bytes, and inherited
           borrows still retire with the existing node-generation protocol. */
        cp->name = ar_strdup(c->a, x->name);
        cp->value = ar_strdup(c->a, x->value);
        cp->next = mine;
        mine = cp;
        if (segment) var_slot_put(segment->slots, segment->capacity, cp->name, strlen(cp->name), cp);
    }
    if (mine) {
        struct custom_prop *last = mine;
        while (last->next) last = last->next;
        last->next = (struct custom_prop *)inherited;
        mine->segment = segment;
        s->vars = mine;
        resolve_vars(d,c->a,mine,inherited);
    }
    memset(setbits, 0, (size_t)css_prop_count());
    struct css_rollback rollback[css_prop_count()];bool has_rollback=false; /* initialized only for a real rollback */
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
        if (dents[i].pseudo == pseudo && dents[i].d->p && css_prop_is_early(dents[i].d->p)) apply_decl(c, &cx, &dents[i],rollback,&has_rollback);
    cx.font_pass = false;
    cx.em = s->font_size;
    for (int i = 0; i < ndents; i++)
        if (dents[i].pseudo == pseudo && dents[i].d->p) apply_decl(c, &cx, &dents[i],rollback,&has_rollback);
    if (!pseudo) web_dialog_style(d,e,s);
    if(pseudo==PE_BACKDROP) {
        if(s->position!=POS_FIXED && s->position!=POS_ABSOLUTE)s->position=POS_ABSOLUTE;
        if(s->display==D_CONTENTS)s->display=D_BLOCK; s->float_=FL_NONE;
    }
    css_style_finish(s, parent, e == d->html && !pseudo);
    if ((pseudo != PE_BEFORE && pseudo != PE_AFTER) || box_pseudo_generated(e,parent,s))
        s->named_font=select_web_font(d,e,s,c->vw,c->vh,c->a);
    else { s->named_font=NULL; s->font_choices=NULL; }
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

static const struct opacity_keyframes *motion_keyframes(struct cascade *c,node_t *n,const char *name) {
    node_t *root=doc_node_root(n,false);
    /* Bindings are already ordered (including imports) and filtered by owner
       media. Search the current tree first, then only its nearest ancestor
       trees if unresolved; never scan unrelated shadow roots globally. */
    while(root) {
        const struct opacity_keyframes *found=NULL;
        struct cascade_layer *found_layer=NULL;
        struct css_scope *scope=root->style_scope;
        for(struct css_binding *b=scope?scope->first:NULL;b;b=b->next)
            for(const struct opacity_keyframes *k=b->index->motions;k;k=k->next) {
                if(__atomic_load_n(&c->d->native_cancelled,__ATOMIC_ACQUIRE) ||
                    (css_can_service() && !web_native_checkpoint(c->d)))return NULL;
                if(!strcmp(k->name,name) && font_conditions_ok(k->media,c->vw,c->vh,web_js_enabled(c->d))) {
                    struct cascade_layer *layer=binding_layer(b,k->layer);
                    if(!found || layer_compare(layer,found_layer)>=0){found=k;found_layer=layer;}
                }
            }
        if(found)return found;
        root=root->shadow_host?doc_node_root(root->shadow_host,false):NULL;
    }
    return NULL;
}
/* The animation and transition origins. animation.c owns the records, the time model and the
   sampling; the cascade gives it each site's styles and @keyframes and takes the sampled
   longhands back as declarations of those origins. */
struct motion_cascade { struct cascade *c; node_t *node; uint8_t pseudo; bool transition; };
/* CSS Animations 2 §4.3 the keyframes of a CSSAnimation: the rule's blocks by offset (blocks at
   one offset are one keyframe, already joined by the parser), a block's animation-timing-function
   its easing (else the element's) and its animation-composition its composite. */
static const struct web_keyframe *motion_frames(void *arg, const char *name, const struct css_easing *easing, uint32_t *count) {
    struct motion_cascade *m = arg;
    struct cascade *c = m->c;
    const struct opacity_keyframes *key = motion_keyframes(c, m->node, name);
    if (!key) return NULL;
    uint32_t n = 0;
    for (const struct opacity_frame *f = key->first; f; f = f->next) n++;
    struct web_keyframe *frames = ar_alloc(c->a, (n ? n : 1) * sizeof *frames);
    const struct propdef *timing = css_prop_lookup("animation-timing-function", 25),
                         *composition = css_prop_lookup("animation-composition", 21);
    uint32_t i = 0;
    for (const struct opacity_frame *f = key->first; f; f = f->next, i++) {
        struct web_keyframe *k = &frames[i];
        *k = (struct web_keyframe){.offset = f->offset, .easing = *easing, .composite = WA_COMPOSITE_AUTO};
        k->values = ar_alloc(c->a, (size_t)(f->count ? f->count : 1) * sizeof *k->values);
        for (int j = 0; j < f->count; j++) {
            const struct decl *x = &f->decls[j];
            if (!x->p || x->important) continue; /* CSS Animations 1 §3: !important in a keyframe is ignored */
            const char *v = x->value, *end = v + strlen(v);
            v = skip_ws(v, end); trim_r(v, &end);
            if (x->p == timing) {
                struct css_easing e;
                if (css_easing_parse(v, (size_t)(end - v), &e)) k->easing = e;
                continue;
            }
            if (x->p == composition) {
                size_t len = (size_t)(end - v);
                if (len == 7 && strn_ieq(v, "replace", 7)) k->composite = CM_COMPOSITE_REPLACE;
                else if (len == 3 && strn_ieq(v, "add", 3)) k->composite = CM_COMPOSITE_ADD;
                else if (len == 10 && strn_ieq(v, "accumulate", 10)) k->composite = CM_COMPOSITE_ACCUMULATE;
                continue;
            }
            if (css_prop_is_motion(x->p)) continue; /* animation-* and transition-* do not animate */
            k->values[k->count++] = (struct web_keyframe_value){.p = x->p, .text = (char *)x->value};
        }
    }
    /* by offset, blocks at one offset in source order (insertion sort: stable, and short) */
    for (uint32_t a = 1; a < n; a++) {
        struct web_keyframe f = frames[a];
        uint32_t b = a;
        for (; b > 0 && frames[b - 1].offset > f.offset; b--) frames[b] = frames[b - 1];
        frames[b] = f;
    }
    *count = n;
    return frames;
}
static void motion_emit(void *arg, const struct propdef *p, const char *value) {
    struct motion_cascade *m = arg;
    struct cascade *c = m->c;
    struct decl *x = ar_alloc(c->a, sizeof *x);
    *x = (struct decl){.p = p, .value = ar_strdup(c->a, value)};
    add_dent(x, 0, m->pseudo, 0);
    dents[ndents - 1].animation = true;
    dents[ndents - 1].transition = m->transition;
}
/* The style of one site with its effects. base: the style with none of them; CSS Animations
   update from it and the animation origin is sampled into it (the after-change style, which
   inherits from the parent's after-change style); CSS Transitions update from the before-change
   style to that one, and the transition origin is sampled into the final style. A site with no
   records, no animation-name and no transition duration costs one compute, as before. */
static style_t *motion_compute(struct cascade *c, node_t *e, const style_t *parent, const style_t *parent_after,
                               uint8_t pseudo, const style_t *before, style_t **base_out, style_t **after_out) {
    style_t *base = compute(c, e, parent, pseudo);
    *base_out = *after_out = NULL;
    if (!c->d->animations) return base;
    bool shown = base->display != D_NONE && (pseudo == PE_NONE || box_pseudo_generated(e, parent, base));
    int want = web_motion_wanted(e, pseudo, shown ? base : NULL);
    if (!want) return base;
    if (!shown) web_motion_cancel(e, pseudo);
    struct motion_cascade m = {c, e, pseudo, false};
    struct web_motion_site site = {
        .ctx = {.base = base, .parent = parent, .node = e, .box = e->box, .rem = c->rem, .vw = (float)c->vw,
                .vh = (float)c->vh, .a = c->a, .d = c->d},
        .pseudo = pseudo, .keyframes = motion_frames, .emit = motion_emit, .arg = &m};
    if (shown && (want & 1)) web_motion_animations(&site);
    uint8_t animated[css_prop_count()];
    memset(animated, 0, sizeof animated);
    size_t first = web_motion_sample(&site, false, animated), second = 0;
    if (first) qsort(dents, (size_t)ndents, sizeof *dents, cmp_dent);
    style_t *after = base;
    bool after_inherits = true; /* after inherits from parent, not from parent_after */
    if (shown && (want & 2)) {
        const style_t *pa = parent_after ? parent_after : parent;
        if (first || pa != parent) { after = compute(c, e, pa, pseudo); after_inherits = pa == parent; }
        web_motion_transitions(&site, before, after, animated);
        m.transition = true;
        second = web_motion_sample(&site, true, animated);
        if (second) qsort(dents, (size_t)ndents, sizeof *dents, cmp_dent);
    }
    style_t *final;
    if (!first && !second) final = base;
    else if (!second && after != base && after_inherits) final = after;
    else final = compute(c, e, parent, pseudo);
    final->animated = first || second;
    /* the base style only where effects apply: elsewhere the style is the base */
    if (final != base) *base_out = base;
    if (after != final && after != base) *after_out = after;
    return final;
}
/* What a display: none element hides is its flat subtree: also the light children its slots
   take from a host above it, which are not its descendants. */
static void clear_styles(node_t *n) {
    n->animation_base_style = n->animation_after_style = NULL;
    if (n->shadow_root) clear_styles(n->shadow_root);
    for (node_t *c = n->first; c; c = c->next) {
        c->style = NULL;
        clear_styles(c);
    }
    if (flat_slot(n))
        for (node_t *c = n->slot_assigned_first; c; c = c->assigned_next) {
            c->style = NULL;
            clear_styles(c);
        }
}
/* CSS Animations 2 §3, CSS Transitions 2 §3: display: none cancels what the subtree's style made */
static void motion_cancel_subtree(node_t *n) {
    if(n->owned)web_motion_cancel(n,-1);
    if(n->shadow_root)motion_cancel_subtree(n->shadow_root);
    for(node_t *ch=n->first;ch;ch=ch->next)motion_cancel_subtree(ch);
    if(flat_slot(n))for(node_t *ch=n->slot_assigned_first;ch;ch=ch->assigned_next)motion_cancel_subtree(ch);
}

void css_node_style_free(node_t *n) {
    if (!n) return;
    ar_free(&n->style_mem); ar_free(&n->style_previous_mem); ar_free(&n->style_inherit_mem);
    style_slots_free(&n->style_slots); style_slots_free(&n->style_previous_slots);
    declarations_free(&n->style_declarations);
    n->style = n->animation_base_style = n->animation_after_style = n->style_previous = NULL;
}

void css_node_style_unpublish(node_t *n) {
    if (!n) return;
    /* out of the document: its CSS animations and transitions are canceled */
    if (n->owned) web_motion_cancel(n, -1);
    /* Detached inherited styles can borrow an ancestor's variable/font data.
       Keep their arenas alive until published box borrows have been retired,
       but never let a detached tree expose those ancestor-dependent styles. */
    n->style = n->animation_base_style = n->animation_after_style = NULL;
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
        n->style_mem.trap=NULL;n->box_regen=false;
        ar_free(&n->style_previous_mem); n->style_previous = NULL; n->style_retired_next = NULL;
        style_slots_free(&n->style_previous_slots);
        n = next;
    }
}
static bool style_equal(const style_t *a, const style_t *b);
/* Between a cascade and css_styles_release: whether n's computed values differ from the
   retired ones its boxes still borrow. A kept style retires the equal new one. */
bool css_restyled(const node_t *n) {
    return n->style_previous && !style_equal(n->style_previous, n->style);
}

/* A mutation's sibling set is the conservative local boundary for +/-/~ and
   nth/empty selectors. Descendants are restyled because inheritance and
   ancestor combinators may change. Relational subjects are dirtied along the
   mutation's ancestors and their preceding sibling forest, not the document
   simply because a sheet contains :has(). */
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
    if(flat_slot(p))return NULL;
    return p;
}
static void mark_style_subject(web_doc *d, node_t *n, uint8_t source) {
    n->style_dirty = true; d->style_pending_dirty = true; n->style_mark_source = source;
    for (node_t *p = dirty_flat_parent(n); p; p = dirty_flat_parent(p)) p->style_children_dirty = true;
}
static bool relational_candidate(const struct compound *compound, node_t *node) {
    /* Stable outer subject keys prune unrelated ancestors/siblings. Dynamic
       pseudo-classes are deliberately not consulted: a removed witness must
       invalidate the OLD match too. Positive logical alternatives contribute
       only necessary stable subject keys, never their dynamic match state. */
    for (int i = 0; i < compound->n; i++) {
        const struct simple *s = &compound->s[i];
        if (s->kind == SK_TAG && (s->tag ? node->tag != s->tag : strcmp(node->name, s->name))) return false;
        if (s->kind == SK_ID && (!node->id || strcmp(node->id, s->name))) return false;
        if (s->kind == SK_CLASS && !node_has_class(node, s->name)) return false;
        if (s->kind == SK_ATTR && !node_attr(node, s->name)) return false;
        /* Positive logical alternatives still require one alternative's stable
           subject keys. Never evaluate its dynamic pseudo or left ancestors;
           :not()/:has() cannot safely impose those keys on this subject. */
        if(s->kind==SK_PC && s->pc==PC_IS && s->args){
            bool candidate=false;
            for(int k=0;k<s->args->n;k++)if(relational_candidate(s->args->v[k]->right,node)){candidate=true;break;}
            if(!candidate)return false;
        }
    }
    return true;
}
/* Calls visit with each entry of a subject index whose compound node's stable
   keys can select, until it returns true; whether one did. */
typedef bool subject_visit(void *context, const struct ient *entry, node_t *node);
static bool visit_subjects(struct ient *const *list, node_t *node, subject_visit *visit, void *context) {
    for (const struct ient *e = list ? *list : NULL; e; e = e->next)
        if (relational_candidate(e->sel->right, node) && visit(context, e, node)) return true;
    return false;
}
static bool index_subjects(struct css_index *index, node_t *node, subject_visit *visit, void *context) {
    if (!index) return false;
    if (node->id && visit_subjects(ht_slot(NULL, &index->ids, node->id, false), node, visit, context)) return true;
    for (int i = 0; i < node->nclasses; i++)
        if (visit_subjects(ht_slot(NULL, &index->classes, node->classes[i], false), node, visit, context)) return true;
    return visit_subjects(ht_slot(NULL, &index->tags, node->name, false), node, visit, context) ||
        visit_subjects(&index->univ, node, visit, context);
}
static bool subject_found(void *context, const struct ient *entry, node_t *node) {
    (void)context; (void)entry; (void)node;
    return true;
}
static bool index_subject_candidate(struct css_index *index,node_t *node) {
    return index_subjects(index, node, subject_found, NULL);
}
/* enum sibling_reach bits for which this element is a positional subject. */
static unsigned sibling_bindings_reach(node_t *node,struct css_binding *bindings,unsigned wanted) {
    unsigned reach = 0;
    for (struct css_binding *binding = bindings; binding && (reach & wanted) != wanted; binding = binding->next)
        for (unsigned k = 0; k < 4; k++)
            if ((wanted & ~reach & 1u << k) && index_subject_candidate(binding->index->child_list_siblings[k], node))
                reach |= 1u << k;
    return reach;
}
static bool subject_bindings_candidate(node_t *node,struct css_binding *bindings,enum subject_dependency dependency) {
    if (dependency == SUBJECT_CHILD_SIBLING) return sibling_bindings_reach(node, bindings, 15) != 0;
    for (struct css_binding *binding = bindings; binding; binding = binding->next) {
        struct css_index *index = dependency==SUBJECT_CHILD_OUTER?binding->index->child_list_subjects:
            binding->index->child_list_parents;
        if (index_subject_candidate(index, node)) return true;
    }
    return false;
}
/* --debug-js: marks whose reach the feature tables decided, and the others;
   how many of the decided reached the boundary or a whole subtree, and the
   names whose selectors sent them there most often. */
#define STYLE_MARK_NAMES 6
static struct {
    uint64_t precise, boundary, parent, subtree;
    struct { char name[40]; uint64_t count; } names[STYLE_MARK_NAMES];
} style_marks;
static void style_mark_name(const char *name, size_t length) {
    int slot = 0;
    for (int i = 0; i < STYLE_MARK_NAMES; i++) {
        if (!strncmp(style_marks.names[i].name, name, length) && !style_marks.names[i].name[length]) { slot = i; goto found; }
        if (style_marks.names[i].count < style_marks.names[slot].count) slot = i;
    }
    if (length >= sizeof style_marks.names[slot].name) length = sizeof style_marks.names[slot].name - 1;
    memcpy(style_marks.names[slot].name, name, length); style_marks.names[slot].name[length] = 0;
    style_marks.names[slot].count = 0;
found:
    style_marks.names[slot].count++;
}
/* Only n's own match changed; its descendants follow if its style does. */
static void mark_style_self(web_doc *d, node_t *n) {
    n->style_self_dirty = true; d->style_pending_dirty = true;
    for (node_t *p = dirty_flat_parent(n); p; p = dirty_flat_parent(p)) p->style_children_dirty = true;
}
/* n's own computed values changed without any selector matching differently. */
static void mark_style_own(web_doc *d, node_t *n) {
    d->need_style = true;
    mark_style_self(d, n);
}
void css_mark_style_own(web_doc *d, node_t *n) {
    if (!d || !n || n->type != N_ELEM || n->owner != d) return;
    mark_style_own(d, n);
    d->restyle_dirty = true;
}
/* The boundary when it is not known what a change reaches: n's subtree, or
   its parent's when some sheet has a sibling combinator. */
static void mark_style_boundary(web_doc *d, node_t *n) {
    node_t *parent = dirty_flat_parent(n);
    if (d->sty.ctx->siblings && parent && parent->type == N_ELEM) n = parent;
    mark_style_subject(d, n, MARK_BOUNDARY);
}

/* The union of what one change reaches, over every sheet that can match the
   element: the UA sheet and its tree's sheets. */
struct reach_set {
    unsigned reach, nkeys;
    struct feature_key keys[FEATURE_KEYS];
    uint64_t names[16]; /* bit filter of the key names' hashes */
};
static void reach_feature(struct reach_set *r, const struct css_feature *f) {
    if (!f) return;
    r->reach |= f->reach;
    for (unsigned i = 0; i < f->nkeys && !(r->reach & REACH_KEYS_OVERFLOW); i++) {
        const struct feature_key *key = &f->keys[i];
        bool seen = false;
        for (unsigned j = 0; j < r->nkeys && !seen; j++) seen = feature_key_equal(&r->keys[j], key);
        if (seen) continue;
        if (r->nkeys == FEATURE_KEYS) { r->reach |= REACH_KEYS_OVERFLOW; break; }
        r->keys[r->nkeys++] = *key;
        r->names[key->hash >> 6 & 15] |= 1ull << (key->hash & 63);
    }
}
enum { FEATURE_ID, FEATURE_CLASS, FEATURE_ATTR };
static const struct css_feature *index_feature(const struct css_index *x, int table, const char *name, size_t length,
                                               uint32_t hash) {
    const struct htab *t = table == FEATURE_ID ? &x->feature_ids : table == FEATURE_CLASS ? &x->feature_classes : &x->feature_attrs;
    if (!t->cap) return NULL;
    for (const struct bucket *b = t->v[hash & (t->cap - 1)]; b; b = b->next)
        if (b->hash == hash && !strncmp(b->key, name, length) && !b->key[length]) return b->feature;
    return NULL;
}
static void reach_named(web_doc *d, const struct css_scope *scope, int table, const char *name, size_t length,
                        struct reach_set *r) {
    uint32_t hash = 2166136261u; /* hash_str() of the first length bytes */
    for (size_t i = 0; i < length; i++) hash = (hash ^ (unsigned char)name[i]) * 16777619u;
    unsigned before = r->reach;
    for (const struct css_binding *b = d->sty.ctx->ua; b; b = b->next)
        reach_feature(r, index_feature(b->index, table, name, length, hash));
    for (const struct css_binding *b = scope ? scope->first : NULL; b; b = b->next)
        reach_feature(r, index_feature(b->index, table, name, length, hash));
    if (d->profile_enabled && (r->reach & ~before & (REACH_PARENT | REACH_SUBTREE))) style_mark_name(name, length);
}
static void reach_state(web_doc *d, const struct css_scope *scope, enum css_state state, struct reach_set *r) {
    for (const struct css_binding *b = d->sty.ctx->ua; b; b = b->next) reach_feature(r, b->index->feature_states[state]);
    for (const struct css_binding *b = scope ? scope->first : NULL; b; b = b->next)
        reach_feature(r, b->index->feature_states[state]);
}
static bool reach_key(const struct reach_set *r, uint8_t kind, const char *name) {
    uint32_t hash = hash_str(name);
    if (!(r->names[hash >> 6 & 15] >> (hash & 63) & 1)) return false;
    for (unsigned i = 0; i < r->nkeys; i++)
        if (r->keys[i].kind == kind && r->keys[i].hash == hash && !strcmp(r->keys[i].name, name)) return true;
    return false;
}
static bool reach_element(const node_t *e, const struct reach_set *r) {
    if (e->id && reach_key(r, SK_ID, e->id)) return true;
    for (int i = 0; i < e->nclasses; i++) if (reach_key(r, SK_CLASS, e->classes[i])) return true;
    for (int i = 0; i < e->nattrs; i++) if (reach_key(r, SK_ATTR, e->attrs[i].name)) return true;
    for (unsigned i = 0; i < r->nkeys; i++) {
        const struct feature_key *key = &r->keys[i];
        if (key->kind == SK_TAG && (key->tag ? e->tag == key->tag : !strcmp(e->name, key->name))) return true;
    }
    return false;
}
/* Restyle the elements below top that carry one of r's keys. */
static void mark_keyed(web_doc *d, node_t *top, const struct reach_set *r) {
    for (node_t *e = top->first; e;) {
        if (e->type == N_ELEM && reach_element(e, r)) mark_style_self(d, e);
        if (e->first) { e = e->first; continue; }
        while (e != top && !e->next) e = e->parent;
        if (e == top) break;
        e = e->next;
    }
}
/* Restyle what r reaches from the elements of a chain, the deepest first, up
   to top (one element when they are the same). Their descendants are top's;
   each has its own following siblings. */
static void mark_reach(web_doc *d, node_t *deepest, node_t *top, const struct reach_set *r, uint8_t source) {
    unsigned siblings = r->reach & (REACH_SIBLINGS | REACH_SIBLING_DESCENDANTS);
    bool parent = (r->reach & REACH_PARENT) || (siblings && (r->reach & REACH_KEYS_OVERFLOW));
    bool subtree = r->reach & (REACH_SUBTREE | REACH_KEYS_OVERFLOW);
    if (d->profile_enabled) { style_marks.parent += parent; style_marks.subtree += !parent && subtree; }
    if (parent) { mark_style_boundary(d, top); return; }
    if (r->reach & REACH_SELF)
        for (node_t *p = deepest; p; p = p == top ? NULL : dirty_flat_parent(p)) mark_style_self(d, p);
    if (subtree) mark_style_subject(d, top, source);
    else if (r->reach & REACH_DESCENDANTS) mark_keyed(d, top, r);
    if (siblings)
        for (node_t *p = deepest; p; p = p == top ? NULL : dirty_flat_parent(p))
            for (node_t *s = next_elem(p); s; s = next_elem(s)) {
                if ((siblings & REACH_SIBLINGS) && reach_element(s, r)) mark_style_self(d, s);
                if (siblings & REACH_SIBLING_DESCENDANTS) mark_keyed(d, s, r);
            }
}
/* Whether the feature tables describe every selector that can match n: it is
   in the document tree, neither a host nor slotted (other trees' :host() and
   ::slotted() name it), and classes and ids compare exactly (no quirks). */
static bool reach_known(web_doc *d, node_t *n) {
    node_t *root = n->type == N_ELEM ? doc_node_root(n, false) : NULL;
    return d->sty.ctx && !d->quirks && root && root->type == N_DOC && !n->foreign && !n->shadow_root && !n->assigned_slot;
}

static bool token_listed(const char *list, const char *token, size_t length) {
    for (const char *p = list; p && *p;) {
        while (is_space(*p)) p++;
        const char *start = p;
        while (*p && !is_space(*p)) p++;
        if ((size_t)(p - start) == length && !memcmp(start, token, length)) return true;
    }
    return false;
}
/* What changed for :has(): an element's attribute or dynamic state, or an
   inserted or removed subtree. */
struct has_change {
    node_t *node;   /* the changed element, or the subtree's root */
    node_t *parent; /* the one element an argument after > can see it from, if there is one */
    const struct css_attribute_change *attribute;
    enum css_state state; /* when it is neither an attribute nor a subtree */
    bool subtree;
};
/* Which elements a compound names a changed attribute, class or id of: 1 its
   own, 2 another's (a complex argument of its pseudo-classes names others). */
static unsigned compound_names(const struct compound *c, const struct css_attribute_change *change) {
    bool classes = !strcmp(change->name, "class"), id = !strcmp(change->name, "id");
    unsigned named = 0;
    for (int i = 0; i < c->n; i++) {
        const struct simple *s = &c->s[i];
        if (s->kind == SK_ATTR && !strcmp(s->name, change->name)) named |= 1;
        if (s->kind == SK_CLASS && classes) {
            size_t length = strlen(s->name);
            if (token_listed(change->old_value, s->name, length) != token_listed(change->new_value, s->name, length)) named |= 1;
        }
        if (s->kind == SK_ID && id && ((change->old_value && !strcmp(change->old_value, s->name)) ||
                                        (change->new_value && !strcmp(change->new_value, s->name)))) named |= 1;
        if (!s->args || (s->kind == SK_PC && s->pc == PC_HAS)) continue;
        bool alternatives = s->kind == SK_PC && (s->pc == PC_NOT || s->pc == PC_IS);
        for (int k = 0; k < s->args->n; k++) {
            const struct compound *arg = s->args->v[k]->right;
            for (const struct compound *part = arg; part; part = part->left) {
                unsigned inner = compound_names(part, change);
                named |= alternatives && !arg->left ? inner : inner ? 2 : 0;
            }
        }
    }
    return named;
}
static bool compound_names_state(const struct compound *c, enum css_state state) {
    for (int i = 0; i < c->n; i++) {
        const struct simple *s = &c->s[i];
        if (pseudo_state(s) == (int)state) return true;
        if (s->args && !(s->kind == SK_PC && s->pc == PC_HAS))
            for (int k = 0; k < s->args->n; k++)
                for (const struct compound *part = s->args->v[k]->right; part; part = part->left)
                    if (compound_names_state(part, state)) return true;
    }
    return false;
}
/* Whether e can match c whatever changed: the changed attribute (its classes
   or id) is not tested, and no element of an inserted or removed subtree is
   hovered, active or focused (moving one of those restyles its boundary). */
static bool compound_could_match(const struct compound *c, node_t *e, const struct has_change *change) {
    const char *skip = change->attribute ? change->attribute->name : "";
    for (int i = 0; i < c->n; i++) {
        const struct simple *s = &c->s[i];
        switch (s->kind) {
        case SK_TAG: if (s->tag ? e->tag != s->tag : strcmp(e->name, s->name)) return false; break;
        case SK_ID: if (strcmp(skip, "id") && (!e->id || strcmp(e->id, s->name))) return false; break;
        case SK_CLASS: if (strcmp(skip, "class") && !node_has_class(e, s->name)) return false; break;
        case SK_ATTR: if (strcmp(skip, s->name) && !match_attr(s, e)) return false; break;
        case SK_PC:
            if (s->pc == PC_NEVER || (change->subtree && pseudo_state(s) >= 0)) return false;
            if (s->pc == PC_IS && s->args) {
                bool any = false;
                for (int k = 0; k < s->args->n && !any; k++) any = compound_could_match(s->args->v[k]->right, e, change);
                if (!any) return false;
            }
            break;
        default: break;
        }
    }
    return true;
}
static bool argument_selects(const struct has_subject *h, node_t *e, const struct has_change *change) {
    for (int k = 0; k < h->args->n; k++)
        for (const struct compound *c = h->args->v[k]->right; c && !anchor_compound(c); c = c->left) {
            unsigned named = change->subtree ? 1 : change->attribute ? compound_names(c, change->attribute) :
                compound_names_state(c, change->state) ? 2 : 0;
            if ((named & 2) || ((named & 1) && compound_could_match(c, e, change))) return true;
        }
    return false;
}
/* Whether a change can alter what h's arguments match: an inserted or removed
   element one of their compounds can select (anything, when they count
   siblings), or a changed attribute or state of an element one names. */
static bool has_changed(const struct has_subject *h, const struct has_change *change) {
    if (!change) return true;
    if (!change->subtree) return argument_selects(h, change->node, change);
    if (h->structure) return true;
    node_t *top = change->node;
    for (node_t *e = top;;) {
        if (e->type == N_ELEM && argument_selects(h, e, change)) return true;
        if (h->children) return false; /* only the root is a child of the parent */
        if (e->first) { e = e->first; continue; }
        while (e != top && !e->next) e = e->parent;
        if (e == top) return false;
        e = e->next;
    }
}
/* One change's walk over the :has() subjects it can concern. */
struct relational_walk {
    web_doc *d;
    const struct has_change *change;
    bool sibling; /* the subject precedes the ancestor being walked */
    bool marked;
    node_t *top;  /* the highest ancestor subject whose match reaches below itself */
    struct reach_set below;
    struct { const struct has_subject *h; bool changed; } seen[16];
    unsigned nseen;
};
static bool relational_visit(void *context, const struct ient *entry, node_t *subject) {
    struct relational_walk *w = context;
    const struct has_subject *h = entry->has;
    const struct has_change *change = w->change;
    if (w->sibling ? !h->siblings : h->children && change && change->parent && subject != change->parent) return false;
    unsigned i = 0;
    while (i < w->nseen && w->seen[i].h != h) i++;
    bool changed = i < w->nseen ? w->seen[i].changed : has_changed(h, change);
    if (i == w->nseen && i < sizeof w->seen / sizeof *w->seen) { w->seen[i].h = h; w->seen[i].changed = changed; w->nseen++; }
    if (!changed) return false;
    w->marked = true;
    const struct css_feature *f = h->reach;
    unsigned beside = REACH_SIBLINGS | REACH_SIBLING_DESCENDANTS;
    if (f->reach & REACH_SELF) mark_style_self(w->d, subject);
    else if (w->sibling || (f->reach & beside)) {
        /* Everything a preceding sibling reaches, or an ancestor's own following siblings. */
        struct reach_set r = {0};
        reach_feature(&r, f);
        mark_reach(w->d, subject, subject, &r, MARK_RELATIONAL);
    } else {
        /* Below an ancestor: the highest one's subtree holds the others'. */
        reach_feature(&w->below, f);
        w->top = subject;
    }
    return false;
}
static void relational_scope(struct relational_walk *w, const struct css_scope *scope, node_t *subject) {
    for (const struct css_binding *b = scope ? scope->first : NULL; b; b = b->next)
        index_subjects(b->index->relational, subject, relational_visit, w);
}
/* Restyle what the :has() subjects a change can concern reach, and whether
   there were any: the change's ancestors (from its parent, or for a child
   list the parent itself) and, for arguments after + or ~, their preceding
   siblings. A changed element's own :has() does not look at itself. */
static bool mark_relational_dirty(web_doc *d, node_t *node, const struct has_change *change) {
    struct relational_walk w = {.d = d, .change = change};
    node_t *changed = change && !change->subtree ? change->node : NULL, *root = doc_node_root(node, false);
    const struct css_scope *scope = root ? root->style_scope : NULL;
    for (node_t *ancestor = node; ancestor && ancestor->type == N_ELEM; ancestor = ancestor->parent)
        for (node_t *subject = ancestor; subject; subject = d->sty.ctx->relational_siblings ? prev_elem(subject) : NULL) {
            if (subject == changed) continue;
            w.sibling = subject != ancestor;
            relational_scope(&w, scope, subject);
            if (subject->shadow_root) relational_scope(&w, subject->shadow_root->style_scope, subject);
        }
    if (w.top) mark_reach(d, w.top, w.top, &w.below, MARK_RELATIONAL);
    return w.marked;
}
void css_mark_dirty(web_doc *d, node_t *n) {
    if (!d) return;
    d->need_style = true;
    if (!n || !d->sty.ctx) { d->style_full_dirty = true; return; }
    if (n->type != N_ELEM) n = dirty_flat_parent(n);
    if (!n) { d->style_full_dirty = true; return; }
    if (d->sty.ctx->relational) mark_relational_dirty(d, n, NULL);
    mark_style_boundary(d, n);
    if (d->profile_enabled) style_marks.boundary++;
}

/* :hover, :active, :focus and :focus-within hold for a target and its flat
   ancestor elements, so a moved state changes only the part of each chain
   the other does not share, from the target up to that part's top. A target
   outside the document has no known former chain, so that move restyles the
   document. */
static node_t *state_parent(node_t *n) {
    node_t *p = dirty_flat_parent(n);
    return p && p->type == N_ELEM ? p : NULL;
}
static void mark_state_part(web_doc *d, enum css_state state, node_t *deepest, node_t *top) {
    if (!top) return;
    for (node_t *p = deepest; p; p = p == top ? NULL : state_parent(p))
        if (!reach_known(d, p)) { css_mark_dirty(d, top); return; }
    struct reach_set r = {0};
    reach_state(d, doc_node_root(top, false)->style_scope, state, &r);
    bool relational = d->sty.ctx->relational &&
        mark_relational_dirty(d, deepest, &(struct has_change){.node = deepest, .state = state});
    if (!r.reach && !relational) return; /* no selector reads this state here */
    d->need_style = true;
    if (d->profile_enabled && (r.reach & (REACH_PARENT | REACH_SUBTREE)))
        style_mark_name(state == CSS_STATE_HOVER ? ":hover" : state == CSS_STATE_ACTIVE ? ":active" : ":focus", state == CSS_STATE_HOVER ? 6 : 7);
    mark_reach(d, deepest, top, &r, MARK_REACH);
    if (d->profile_enabled) style_marks.precise++;
}
void css_mark_state_moved(web_doc *d, enum css_state state, node_t *from, node_t *to) {
    if (!d) return;
    if ((from && doc_node_root(from, true) != d->root) || (to && doc_node_root(to, true) != d->root)) {
        d->need_style = d->style_full_dirty = true;
        return;
    }
    size_t from_depth = 0, to_depth = 0;
    for (node_t *p = from; p; p = state_parent(p)) from_depth++;
    for (node_t *p = to; p; p = state_parent(p)) to_depth++;
    node_t *a = from, *b = to, *from_top = NULL, *to_top = NULL;
    for (; from_depth > to_depth; from_depth--) { from_top = a; a = state_parent(a); }
    for (; to_depth > from_depth; to_depth--) { to_top = b; b = state_parent(b); }
    while (a != b) {
        from_top = a; a = state_parent(a);
        to_top = b; b = state_parent(b);
    }
    mark_state_part(d, state, from, from_top);
    mark_state_part(d, state, to, to_top);
}

/* Attributes the pseudo-class matcher (match_pc and the form state snapshot)
   reads, of an element or of its ancestors: their changes keep the boundary. */
static bool state_attribute(const char *name) {
    static const char *const names[] = {
        "checked", "contenteditable", "dir", "disabled", "form", "href", "lang", "list", "max", "maxlength", "min",
        "minlength", "multiple", "name", "open", "pattern", "placeholder", "readonly", "required", "selected",
        "slot", "step", "type", "value",
    };
    for (size_t i = 0; i < sizeof names / sizeof *names; i++) if (!strcmp(name, names[i])) return true;
    return false;
}
/* The classes of one list missing from the other: only they changed. */
static void reach_classes(web_doc *d, const struct css_scope *scope, const char *list, const char *other,
                          struct reach_set *r) {
    for (const char *p = list; p && *p;) {
        while (is_space(*p)) p++;
        const char *start = p;
        while (*p && !is_space(*p)) p++;
        if (p > start && !token_listed(other, start, (size_t)(p - start)))
            reach_named(d, scope, FEATURE_CLASS, start, (size_t)(p - start), r);
    }
}
void css_mark_attribute_dirty(web_doc *d, node_t *n, const struct css_attribute_change *change) {
    if (!d) return;
    if (!change || !reach_known(d, n) || state_attribute(change->name)) {
        css_mark_dirty(d, n); return;
    }
    d->need_style = true;
    if (d->sty.ctx->relational)
        mark_relational_dirty(d, n, &(struct has_change){.node = n, .parent = n->parent, .attribute = change});
    const struct css_scope *scope = doc_node_root(n, false)->style_scope;
    /* The element itself always: presentational hints, attr() and the style attribute. */
    struct reach_set r = {.reach = hint_reaches_descendants(n, change->name) ? REACH_SUBTREE : REACH_SELF};
    reach_named(d, scope, FEATURE_ATTR, change->name, strlen(change->name), &r);
    if (!strcmp(change->name, "class")) {
        reach_classes(d, scope, change->old_value, change->new_value, &r);
        reach_classes(d, scope, change->new_value, change->old_value, &r);
    } else if (!strcmp(change->name, "id")) {
        if (change->old_value) reach_named(d, scope, FEATURE_ID, change->old_value, strlen(change->old_value), &r);
        if (change->new_value) reach_named(d, scope, FEATURE_ID, change->new_value, strlen(change->new_value), &r);
    }
    mark_reach(d, n, n, &r, MARK_REACH);
    if (d->profile_enabled) style_marks.precise++;
}
/* A control's value, checkedness or selection changed. The control restyles
   (its box shows the state); :valid/:invalid also aggregate into its form and
   fieldsets, and a radio's validity follows its whole group. */
void css_mark_form_state(web_doc *d, node_t *control) {
    if (!d || !control) return;
    css_mark_dirty(d, control);
    if (!d->sty.ctx || !(d->sty.ctx->form_features & WEB_STYLE_FORM_VALIDITY)) return;
    if (control->tag == T_input && !control->foreign && web_input_type(control) == WEB_INPUT_RADIO) {
        d->style_full_dirty = true;
        return;
    }
    node_t *form = web_form_owner(d, control);
    if (form) css_mark_dirty(d, form);
    for (node_t *p = control->parent; p; p = p->parent)
        if (p->type == N_ELEM && !p->foreign && p->tag == T_fieldset) css_mark_dirty(d, p);
}
static bool child_list_contains(node_t *subtree,node_t *node) {
    for(;node;node=node->parent?node->parent:node->shadow_host)if(node==subtree)return true;
    return false;
}
void css_mark_child_list_dirty(web_doc *d,node_t *parent,node_t *subtree,bool inserted) {
    if(!d)return;
    node_t *root=parent?doc_node_root(parent,false):NULL;
    /* Child-list callers already name the sibling set's parent. Promoting it
       once more merely because ANY sheet uses +/-/~ restyles its grandparent
       and unrelated forests on every insert-and-measure loop. Keep special
       flat-tree/shadow transactions on the existing conservative path. */
    if(!parent || !subtree || subtree->type==N_FRAGMENT || parent->type!=N_ELEM || parent->foreign || parent->shadow_root ||
       parent->tag==T_fieldset || parent->tag==T_select || parent->tag==T_optgroup ||
       parent->assigned_slot || parent->tag==T_slot || (root&&root->shadow_host) || !d->sty.ctx) {
        css_mark_dirty(d,parent);return;
    }
    /* Moving/removing the active interaction path can change ancestor state,
       not just :empty on this parent. Retain the old conservative boundary;
       ordinary inserts elsewhere do not perturb that path. Use DOM links,
       never flush assignment or inspect a half-published box tree here. */
    if(child_list_contains(subtree,d->focus) || child_list_contains(subtree,d->hover_target) ||
       child_list_contains(subtree,d->active_target)) {
        css_mark_dirty(d,parent);return;
    }
    d->need_style=true;
    /* Quirks classes and ids compare without case: what an argument names is not known exactly. */
    if(d->sty.ctx->relational)
        mark_relational_dirty(d,parent,d->quirks?NULL:&(struct has_change){.node=subtree,.parent=parent,.subtree=true});
    struct css_scope *scope=root?root->style_scope:NULL;
    bool outer_state=d->sty.ctx->siblings &&
        (subject_bindings_candidate(parent,scope?scope->first:NULL,SUBJECT_CHILD_OUTER) ||
         subject_bindings_candidate(parent,d->sty.ctx->ua,SUBJECT_CHILD_OUTER));
    node_t *boundary=outer_state?dirty_flat_parent(parent):NULL;
    if((boundary&&boundary->type==N_ELEM) ||
       subject_bindings_candidate(parent,scope?scope->first:NULL,SUBJECT_CHILD_PARENT) ||
       subject_bindings_candidate(parent,d->sty.ctx->ua,SUBJECT_CHILD_PARENT)) {
        /* Parent state can affect even descendants with no inherited changes
           (for example .parent:empty + .next .child). Keep that full boundary. */
        mark_style_subject(d,boundary&&boundary->type==N_ELEM?boundary:parent,MARK_PARENT_STATE);return;
    }
    /* Insertion does not itself change a parent's inherited/custom values.
       Publish the ancestor dirty path without turning it into a full subtree
       cascade. A moved/new connected subtree still receives the new parent;
       removal must not recascade the detached former child. */
    d->style_pending_dirty=true;
    for(node_t *p=parent;p;p=dirty_flat_parent(p))p->style_children_dirty=true;
    if(subtree->parent==parent && subtree->type==N_ELEM)mark_style_subject(d,subtree,MARK_INSERTED);
    struct css_binding *authored=scope?scope->first:NULL,*ua=d->sty.ctx->ua;
    if(!inserted || subtree->parent!=parent) {
        /* Removal and reordering lost the former position: any positional
           subject among the remaining children may have changed. */
        for(node_t *child=parent->first;child;child=child->next)if(child->type==N_ELEM &&
            (sibling_bindings_reach(child,authored,15) || sibling_bindings_reach(child,ua,15)))
            mark_style_subject(d,child,MARK_POSITION);
        return;
    }
    /* Positional pseudo-classes and sibling combinators count elements only. */
    if(subtree->type!=N_ELEM)return;
    /* An insertion changes only the element counts on each side of it. An
       append must not restyle every earlier sibling (list growth would be
       quadratic): :first-child/+ reach only the nearest following element,
       :last-child the nearest preceding one, nth/~ the whole side. */
    node_t *next=next_elem(subtree),*prev=prev_elem(subtree),*next_type=next,*prev_type=prev;
    while(next_type && strcmp(next_type->name,subtree->name))next_type=next_elem(next_type);
    while(prev_type && strcmp(prev_type->name,subtree->name))prev_type=prev_elem(prev_type);
    for(node_t *child=next;child;child=next_elem(child)) {
        bool near=child==next || child==next_type;
        unsigned wanted=near?SIBLING_NEXT|SIBLING_FOLLOWING:SIBLING_FOLLOWING;
        if(sibling_bindings_reach(child,authored,wanted) || sibling_bindings_reach(child,ua,wanted))
            mark_style_subject(d,child,MARK_POSITION);
    }
    for(node_t *child=prev;child;child=prev_elem(child)) {
        bool near=child==prev || child==prev_type;
        unsigned wanted=near?SIBLING_PREVIOUS|SIBLING_PRECEDING:SIBLING_PRECEDING;
        if(sibling_bindings_reach(child,authored,wanted) || sibling_bindings_reach(child,ua,wanted))
            mark_style_subject(d,child,MARK_POSITION);
    }
}

static bool equal_string(const char *a,const char *b) { return a==b || (a && b && !strcmp(a,b)); }
static bool equal_tracks(const struct gtemplate *a,const struct gtemplate *b) {
    if(a==b)return true;
    if(!a||!b||a->n!=b->n||a->rep_at!=b->rep_at||a->rep_n!=b->rep_n||a->rep_fit!=b->rep_fit)return false;
    for(int i=0;i<a->n;i++)if(a->t[i].min_kind!=b->t[i].min_kind||a->t[i].max_kind!=b->t[i].max_kind||
        !css_length_equal(&a->t[i].min,&b->t[i].min)||!css_length_equal(&a->t[i].max,&b->t[i].max))return false;
    return true;
}
static bool equal_areas(const struct gareas *a,const struct gareas *b) {
    if(a==b)return true;
    if(!a || !b || a->rows!=b->rows || a->cols!=b->cols)return false;
    for(int i=0;i<a->rows*a->cols;i++)if(!equal_string(a->cell[i],b->cell[i]))return false;
    return true;
}
static bool same_translation_length(const len_t *a,const len_t *b){return css_length_equal(a,b);}
/* Layout sees only whether a transform exists (containing blocks, stacking)
   and pure translations (scrollable overflow). Other transforms are applied
   by paint/hit testing, so animating them never forces a layout pass. */
static bool same_translation(const style_t *a,const style_t *b){
    if(css_translation_active(a)!=css_translation_active(b))return false;
    if(!css_translation_active(a))return true;
    /* A CSS animation samples its transform every frame. Paint and hit
       testing read the used translation live from the box style, so the
       samples only repaint; scrollable overflow follows at the next layout
       instead of relaying out a whole document sixty times a second. */
    if(a->animated && b->animated)return true;
    bool general=css_transform_general(a);
    if(general!=css_transform_general(b))return false;
    if(general)return true;
    if(a->translate.active!=b->translate.active)return false;
    if(a->translate.active)for(unsigned axis=0;axis<2;axis++)if(!same_translation_length(&a->translate.xy[axis],&b->translate.xy[axis]))return false;
    const struct css_transform *x=a->transform,*y=b->transform;
    while(x&&y){
        if(x->kind!=y->kind)return false;
        if(x->kind==CT_TRANSLATE)for(unsigned axis=0;axis<2;axis++)if(!same_translation_length(&x->xy[axis],&y->xy[axis]))return false;
        if(x->kind==CT_MATRIX&&(x->m[3]!=y->m[3]||x->m[7]!=y->m[7]))return false;
        x=x->next;y=y->next;
    }
    return !x&&!y;
}
bool css_style_layout_equal(const style_t *a,const style_t *b) {
    if(a==b)return true;
    if(!a || !b)return false;
    const struct web_font_choice *fa=a->font_choices,*fb=b->font_choices;
    for(;fa&&fb;fa=fa->next,fb=fb->next){
        if(fa->resource!=fb->resource||fa->face!=fb->face||fa->range_count!=fb->range_count||
           (fa->range_count&&memcmp(fa->ranges,fb->ranges,fa->range_count*sizeof *fa->ranges)))return false;
    }
    if(fa||fb)return false;
    if (!!a->filter != !!b->filter) return false; /* containing-block ownership changes */
    /* Values change scrollable overflow even though normal-flow boxes stay
       unchanged; identity versus none also changes fixed containing blocks. */
    if(!same_translation(a,b))return false;
    if ((a->display == D_INLINE || b->display == D_INLINE) && (a->opacity < 1) != (b->opacity < 1)) return false;
    if(!equal_string(a->font_names,b->font_names) || !equal_string(a->content,b->content) ||
        !equal_string(a->container_names,b->container_names) ||
        !equal_string(a->list_style_string,b->list_style_string) ||
        !equal_tracks(a->grid_cols,b->grid_cols) || !equal_tracks(a->grid_rows,b->grid_rows) ||
        !equal_tracks(a->grid_auto_cols,b->grid_auto_cols) || !equal_tracks(a->grid_auto_rows,b->grid_auto_rows) ||
        !equal_areas(a->grid_areas,b->grid_areas))return false;
    for(int i=0;i<4;i++)if(!equal_string(a->grid_place[i].name,b->grid_place[i].name))return false;
    if(a->before_generated!=b->before_generated || a->after_generated!=b->after_generated ||
        (a->before_generated&&!css_style_layout_equal(a->before,b->before)) ||
        (a->after_generated&&!css_style_layout_equal(a->after,b->after)) ||
        !css_style_layout_equal(a->backdrop,b->backdrop))return false;
    style_t x=*a,y=*b;
    /* Equivalent calc/min/max ASTs live in different retirement arenas.
       Compare their values structurally before removing address identity
       from the final field comparison. Geometry is not cached by pointers. */
    len_t *xl[]={&x.line_height,&x.svg_stroke_width,&x.width,&x.height,&x.min_width,&x.max_width,&x.min_height,&x.max_height,&x.text_indent,&x.flex_basis};
    len_t *yl[]={&y.line_height,&y.svg_stroke_width,&y.width,&y.height,&y.min_width,&y.max_width,&y.min_height,&y.max_height,&y.text_indent,&y.flex_basis};
    for(size_t i=0;i<sizeof xl/sizeof *xl;i++){if(!css_length_equal(xl[i],yl[i]))return false;xl[i]->expr=yl[i]->expr=NULL;}
    len_t *xa[]={x.margin,x.padding,x.inset,x.object_pos},*ya[]={y.margin,y.padding,y.inset,y.object_pos};
    for(unsigned group=0;group<4;group++)for(unsigned i=0;i<(group==3?2:4);i++){
        if(!css_length_equal(&xa[group][i],&ya[group][i]))return false;xa[group][i].expr=ya[group][i].expr=NULL;
    }
    x.filter=y.filter=NULL; /* effect amounts are paint-only arena borrows */
    x.clip_path=y.clip_path=NULL;memset(&x.clip,0,sizeof x.clip);memset(&y.clip,0,sizeof y.clip); /* clipping paints only */
    /* same_translation() decided every geometric effect of the transform
       properties; their values and arena addresses are paint state. */
    x.transform=y.transform=NULL;memset(&x.translate,0,sizeof x.translate);memset(&y.translate,0,sizeof y.translate);
    x.rotate=y.rotate=x.scale=y.scale=NULL;
    memset(x.transform_origin,0,sizeof x.transform_origin);memset(y.transform_origin,0,sizeof y.transform_origin);
    x.transform_box=y.transform_box=0;
    memset(&x.motion,0,sizeof x.motion);memset(&y.motion,0,sizeof y.motion); /* timing lists are not layout */
    x.animated=y.animated=false;
    x.font_names=y.font_names=NULL;x.font_choices=y.font_choices=NULL; x.content=y.content=NULL; x.list_style_string=y.list_style_string=NULL;
    x.container_names=y.container_names=NULL;
    x.grid_cols=y.grid_cols=x.grid_rows=y.grid_rows=x.grid_auto_cols=y.grid_auto_cols=x.grid_auto_rows=y.grid_auto_rows=NULL;
    x.grid_areas=y.grid_areas=NULL;
    for(int i=0;i<4;i++)x.grid_place[i].name=y.grid_place[i].name=NULL;
    x.before=y.before=x.after=y.after=x.backdrop=y.backdrop=x.placeholder=y.placeholder=NULL;
    /* Custom properties have already been resolved into real properties. Paint
       data/colors are replaced in boxes/runs, but cannot change geometry. */
    x.vars=y.vars=NULL; x.gradient=y.gradient=NULL; x.box_shadow=y.box_shadow=NULL;
    memset(x.corner_radius,0,sizeof x.corner_radius);memset(y.corner_radius,0,sizeof y.corner_radius);
    x.border_radius=y.border_radius=0; /* decoration clipping does not change layout */
    x.bg_image=y.bg_image=x.mask_image=y.mask_image=NULL;
    x.bg_img=y.bg_img=x.mask_img=y.mask_img=0;
    x.color=y.color=x.bg_color=y.bg_color=0;
    x.color_inherited=y.color_inherited=x.color_own=y.color_own=NULL;
    x.color_scheme=y.color_scheme=0;x.color_scheme_text=y.color_scheme_text=NULL;
    memset(x.border_color,0,sizeof x.border_color);memset(y.border_color,0,sizeof y.border_color);
    x.outline_color=y.outline_color=0;x.outline_width=y.outline_width=x.outline_offset=y.outline_offset=0;
    x.outline_style=y.outline_style=0; /* outlines paint outside layout */
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

static bool transforms_equal(const struct css_transform *x, const struct css_transform *y) {
    for (; x && y; x = x->next, y = y->next)
        if (x->kind != y->kind || !css_length_equal(&x->xy[0], &y->xy[0]) || !css_length_equal(&x->xy[1], &y->xy[1]) ||
            memcmp(x->v, y->v, sizeof x->v) || memcmp(x->m, y->m, sizeof x->m)) return false;
    return !x && !y;
}
static bool shadows_equal(const struct css_box_shadow *x, const struct css_box_shadow *y) {
    for (; x && y; x = x->next, y = y->next)
        if (x->x != y->x || x->y != y->y || x->blur != y->blur || x->spread != y->spread || x->color != y->color ||
            x->inset != y->inset || !css_color_value_equal(x->color_value, y->color_value)) return false;
    return !x && !y;
}
static bool filters_equal(const struct css_filter *x, const struct css_filter *y) {
    if (x == y) return true;
    if (!x || !y || x->count != y->count || x->opacity != y->opacity) return false;
    for (size_t i = 0; i < x->count; i++)
        if (x->steps[i].value != y->steps[i].value || x->steps[i].percentage != y->steps[i].percentage) return false;
    return true;
}
static bool clip_paths_equal(const struct css_clip_path *x, const struct css_clip_path *y) {
    if (x == y) return true;
    if (!x || !y || x->shape != y->shape || x->box != y->box || x->evenodd != y->evenodd || x->round != y->round ||
        x->inset_percent != y->inset_percent || x->points != y->points || memcmp(x->extent, y->extent, sizeof x->extent) ||
        !equal_string(x->text, y->text)) return false;
    for (int i = 0; i < 2; i++)
        if (!css_length_equal(&x->radius[i], &y->radius[i]) || !css_length_equal(&x->at[i], &y->at[i])) return false;
    for (int i = 0; i < 4; i++)
        if (!css_length_equal(&x->inset[i], &y->inset[i]) || !css_length_equal(&x->corner[i].axis[0], &y->corner[i].axis[0]) ||
            !css_length_equal(&x->corner[i].axis[1], &y->corner[i].axis[1])) return false;
    for (size_t i = 0; i < x->points; i++)
        if (!css_length_equal(&x->point[i][0], &y->point[i][0]) || !css_length_equal(&x->point[i][1], &y->point[i][1])) return false;
    return true;
}
static bool gradients_equal(const struct gradient *x, const struct gradient *y) {
    if (x == y) return true;
    if (!x || !y || x->radial != y->radial || x->repeating != y->repeating || x->circle != y->circle ||
        x->to_x != y->to_x || x->to_y != y->to_y || x->rsize != y->rsize || x->angle != y->angle || x->n != y->n ||
        !css_length_equal(&x->at[0], &y->at[0]) || !css_length_equal(&x->at[1], &y->at[1])) return false;
    for (int i = 0; i < x->n; i++)
        if (x->col[i] != y->col[i] || !css_color_value_equal(x->col_value[i], y->col_value[i]) ||
            !css_length_equal(&x->pos[i], &y->pos[i])) return false;
    return true;
}
static bool vars_equal(const struct custom_prop *x, const struct custom_prop *y) {
    /* An element without declarations of its own shares its parent's chain. */
    for (; x != y; x = x->next, y = y->next)
        if (!x || !y || !equal_string(x->name, y->name) || !equal_string(x->value, y->value)) return false;
    return true;
}
/* Every computed value of a and b is the same, so b may be dropped for a,
   which its element's descendants may borrow from. Pointed-to values are
   compared by content; a pointer this does not know compares by address,
   which can only report a difference. */
static bool style_equal(const style_t *a, const style_t *b) {
    if (a == b) return true;
    if (!a || !b) return false;
    const struct web_font_choice *fa = a->font_choices, *fb = b->font_choices;
    for (; fa && fb; fa = fa->next, fb = fb->next)
        if (fa->resource != fb->resource || fa->face != fb->face || fa->range_count != fb->range_count ||
            (fa->range_count && memcmp(fa->ranges, fb->ranges, fa->range_count * sizeof *fa->ranges))) return false;
    if (fa || fb) return false;
    if (!equal_string(a->container_names, b->container_names) || !equal_string(a->font_names, b->font_names) ||
        !equal_string(a->content, b->content) || !equal_string(a->list_style_string, b->list_style_string) ||
        !equal_string(a->bg_image, b->bg_image) || !equal_string(a->mask_image, b->mask_image) ||
        !css_motion_equal(&a->motion, &b->motion) || !equal_string(a->svg_fill.ref, b->svg_fill.ref) ||
        !equal_string(a->svg_stroke.ref, b->svg_stroke.ref) || !shadows_equal(a->box_shadow, b->box_shadow) ||
        !filters_equal(a->filter, b->filter) || !clip_paths_equal(a->clip_path, b->clip_path) ||
        !transforms_equal(a->transform, b->transform) ||
        !transforms_equal(a->rotate, b->rotate) || !transforms_equal(a->scale, b->scale) ||
        !gradients_equal(a->gradient, b->gradient) || !vars_equal(a->vars, b->vars) ||
        !equal_tracks(a->grid_cols, b->grid_cols) || !equal_tracks(a->grid_rows, b->grid_rows) ||
        !equal_tracks(a->grid_auto_cols, b->grid_auto_cols) || !equal_tracks(a->grid_auto_rows, b->grid_auto_rows) ||
        !equal_areas(a->grid_areas, b->grid_areas) || !style_equal(a->before, b->before) ||
        !style_equal(a->after, b->after) || !style_equal(a->backdrop, b->backdrop) ||
        !style_equal(a->placeholder, b->placeholder) || !css_color_slots_equal(a, b) ||
        !equal_string(a->color_scheme_text, b->color_scheme_text)) return false;
    for (int i = 0; i < 4; i++) if (!equal_string(a->grid_place[i].name, b->grid_place[i].name)) return false;
    style_t x = *a, y = *b;
    len_t *xl[] = {&x.object_pos[0], &x.object_pos[1], &x.line_height, &x.svg_stroke_width, &x.width, &x.height,
                   &x.min_width, &x.max_width, &x.min_height, &x.max_height, &x.text_indent, &x.flex_basis,
                   &x.translate.xy[0], &x.translate.xy[1], &x.transform_origin[0], &x.transform_origin[1],
                   &x.bg_pos[0], &x.bg_pos[1], &x.bg_size[0], &x.bg_size[1], &x.mask_pos[0], &x.mask_pos[1],
                   &x.mask_size[0], &x.mask_size[1]};
    len_t *yl[] = {&y.object_pos[0], &y.object_pos[1], &y.line_height, &y.svg_stroke_width, &y.width, &y.height,
                   &y.min_width, &y.max_width, &y.min_height, &y.max_height, &y.text_indent, &y.flex_basis,
                   &y.translate.xy[0], &y.translate.xy[1], &y.transform_origin[0], &y.transform_origin[1],
                   &y.bg_pos[0], &y.bg_pos[1], &y.bg_size[0], &y.bg_size[1], &y.mask_pos[0], &y.mask_pos[1],
                   &y.mask_size[0], &y.mask_size[1]};
    for (size_t i = 0; i < sizeof xl / sizeof *xl; i++) {
        if (!css_length_equal(xl[i], yl[i])) return false;
        xl[i]->expr = yl[i]->expr = NULL;
    }
    for (int i = 0; i < 4; i++) {
        len_t *xs[] = {&x.margin[i], &x.padding[i], &x.inset[i], &x.corner_radius[i].axis[0], &x.corner_radius[i].axis[1]};
        len_t *ys[] = {&y.margin[i], &y.padding[i], &y.inset[i], &y.corner_radius[i].axis[0], &y.corner_radius[i].axis[1]};
        for (size_t k = 0; k < sizeof xs / sizeof *xs; k++) {
            if (!css_length_equal(xs[k], ys[k])) return false;
            xs[k]->expr = ys[k]->expr = NULL;
        }
    }
    x.font_choices = y.font_choices = NULL;
    x.container_names = y.container_names = x.font_names = y.font_names = x.content = y.content = NULL;
    x.list_style_string = y.list_style_string = x.bg_image = y.bg_image = x.mask_image = y.mask_image = NULL;
    memset(&x.motion, 0, sizeof x.motion); memset(&y.motion, 0, sizeof y.motion);
    x.svg_fill.ref = y.svg_fill.ref = x.svg_stroke.ref = y.svg_stroke.ref = NULL;
    x.box_shadow = y.box_shadow = NULL; x.filter = y.filter = NULL; x.clip_path = y.clip_path = NULL;
    x.transform = y.transform = x.rotate = y.rotate = x.scale = y.scale = NULL;
    x.gradient = y.gradient = NULL; x.vars = y.vars = NULL;
    x.color_inherited = y.color_inherited = x.color_own = y.color_own = NULL; x.color_scheme_text = y.color_scheme_text = NULL;
    x.grid_cols = y.grid_cols = x.grid_rows = y.grid_rows = NULL;
    x.grid_auto_cols = y.grid_auto_cols = x.grid_auto_rows = y.grid_auto_rows = NULL;
    x.grid_areas = y.grid_areas = NULL;
    for (int i = 0; i < 4; i++) x.grid_place[i].name = y.grid_place[i].name = NULL;
    x.before = y.before = x.after = y.after = x.backdrop = y.backdrop = x.placeholder = y.placeholder = NULL;
    x.bg_img = y.bg_img = x.mask_img = y.mask_img = 0; /* found from bg_image and mask_image after the cascade */
    return !memcmp(&x, &y, sizeof x);
}
static bool style_box_compatible(node_t *e, const style_t *old, const style_t *next);
/* debug_js only: names the element whose restyle forced a box rebuild. */
void css_box_change_report(web_doc *d) {
    node_t *e=__atomic_exchange_n(&d->box_change_node,NULL,__ATOMIC_RELAXED);
    if(!e || e->owner!=d || !e->style || !e->style_previous)return;
    const style_t *a=e->style_previous,*b=e->style;
    const char *id=node_attr(e,"id"),*cls=node_attr(e,"class");
    char line[400];
    snprintf(line,sizeof line,"Box rebuild from <%s id=%.40s class=%.80s>: layout-equal %d, display %d>%d, position %d>%d, overflow %d>%d, before %d>%d, after %d>%d, transform %d>%d general %d>%d, opacity %.3g>%.3g, animation %s",
        e->name?e->name:"?",id?id:"",cls?cls:"",css_style_layout_equal(a,b),a->display,b->display,a->position,b->position,
        a->overflow,b->overflow,a->before_generated,b->before_generated,a->after_generated,b->after_generated,
        css_translation_active(a),css_translation_active(b),css_transform_general(a),css_transform_general(b),
        (double)a->opacity,(double)b->opacity,b->animated?"sampled":"none");
    web_js_console(d,1,line);
}
/* Descendants of an outermost <svg> generate no boxes: the root's atomic box
   owns a snapshot serialized from their computed paint. A restyle there only
   needs that snapshot re-serialized (doc_svg keeps the raster if unchanged). */
static bool svg_snapshot_restyle(node_t *e, const style_t *old, const style_t *next) {
    node_t *root=e;
    while(root->parent && root->parent->type==N_ELEM && root->parent->foreign)root=root->parent;
    if(root->tag!=T_svg)return false;
    if(root==e && !css_style_layout_equal(old,next))return false;
    if(root->box && root->box->atomic==AT_SVG)__atomic_store_n(&root->svg_snapshot_dirty,true,__ATOMIC_RELAXED);
    else __atomic_store_n(&e->owner->svg_snapshots_dirty,true,__ATOMIC_RELAXED); /* sprite sheets reached by <use> */
    return true;
}
static bool style_box_compatible(node_t *e, const style_t *old, const style_t *next) {
    if (!old || !next) return false;
    if(e->foreign)return svg_snapshot_restyle(e,old,next);
    if (css_style_layout_equal(old,next)) return true;
    if (e->foreign || e->tag == T_input || e->tag == T_textarea || e->tag == T_select || e->tag == T_img ||
        e->tag == T_canvas || e->tag == T_video || e->tag == T_iframe || e->tag == T_frameset ||
        old->before_generated || old->after_generated || old->backdrop ||
        next->before_generated || next->after_generated || next->backdrop) return false;
    if (old->display != next->display || old->position != next->position || old->float_ != next->float_ ||
        !!old->filter != !!next->filter ||
        ((old->display == D_INLINE || next->display == D_INLINE) && (old->opacity < 1) != (next->opacity < 1)) ||
        old->container_type != next->container_type ||
        old->overflow != next->overflow || old->content_visibility != next->content_visibility ||
        old->white_space != next->white_space || old->text_transform != next->text_transform ||
        old->list_style != next->list_style || old->list_style_inside != next->list_style_inside) return false;
    return old->display == D_BLOCK || old->display == D_FLOW_ROOT || old->display == D_INLINE || old->display == D_NONE ||
        old->display==D_FLEX||old->display==D_INLINE_FLEX||old->display==D_GRID||old->display==D_INLINE_GRID;
}

/* The boxes that borrow e's computed style, for the supervisor's join to refresh: e's own
   box and its subtree, reached down the path marked dirty. A display:contents element has
   no box; its text and ::before/::after boxes are below its nearest boxed flat ancestor's,
   which the join then refreshes whole. A modal dialog's ::backdrop box is the root's child. */
static void mark_style_borrowers(web_doc *d, node_t *e) {
    box_t *b = e->box;
    if (!b && e->style && e->style->display == D_CONTENTS) {
        node_t *a = doc_flat_parent(e);
        while (a && !a->box && a->style && a->style->display == D_CONTENTS) a = doc_flat_parent(a);
        if ((b = a ? a->box : NULL)) __atomic_store_n(&b->style_sync_subtree, true, __ATOMIC_RELAXED);
    }
    for (; b; b = b->parent) __atomic_store_n(&b->style_sync_dirty, true, __ATOMIC_RELAXED);
    if (e->style && e->style->backdrop)
        for (b = web_dialog_backdrop(d, e); b; b = b->parent) __atomic_store_n(&b->style_sync_dirty, true, __ATOMIC_RELAXED);
}

static void retire_style(struct cascade *c, node_t *e) {
    /* Boxes are stable during the parallel cascade; only monotone dirty
       publication happens here. The supervisor refreshes borrows after join. */
    mark_style_borrowers(c->d, e);
    e->style_previous = e->style; e->style_previous_mem = e->style_mem;
    e->style_previous_slots = e->style_slots; e->style_slots = NULL;
    memset(&e->style_mem, 0, sizeof e->style_mem);
    e->style_mem.chunk_size = 512; /* allocated only if a computed value needs a tail */
    e->style_mem.trap = c->trap;
    node_t *head;
    do { head = __atomic_load_n(&c->d->style_retired_first, __ATOMIC_RELAXED); e->style_retired_next = head; }
    while (!__atomic_compare_exchange_n(&c->d->style_retired_first, &head, e, false, __ATOMIC_RELEASE, __ATOMIC_RELAXED));
    c->a = &e->style_mem;
}

/* The recomputed style equals the retired one: keep the retired object, which
   descendants' styles may borrow from, and retire the new one in its place. */
static void keep_previous_style(node_t *e) {
    arena_t mem = e->style_mem; e->style_mem = e->style_previous_mem; e->style_previous_mem = mem;
    struct css_style_slot *slots = e->style_slots; e->style_slots = e->style_previous_slots; e->style_previous_slots = slots;
    style_t *s = e->style; e->style = e->style_previous; e->style_previous = s;
    e->style_previous_mem.trap = NULL;
}

/* Only e's own values changed, not what its children inherit (the template
   css_style_init makes from it, and the display their blockification reads):
   the new style takes the retired one's inherited values, which the children's
   styles borrow, so the children keep their styles. When those values were the
   retired style's own, its memory becomes e's inherit arena; otherwise they
   are already there or an ancestor's. Not inlined: the templates stay off the
   recursive walk's frames. */
static __attribute__((noinline)) bool adopt_inherited(node_t *e) {
    const style_t *was = e->style_previous;
    style_t *s = e->style;
    if (s->display != was->display) return false;
    style_t from_was, from_new;
    css_style_init(&from_was, was); css_style_init(&from_new, s);
    if (!style_equal(&from_was, &from_new)) return false;
    css_style_adopt_inherited(s, was);
    if (!e->style_inherit_mem.head) {
        e->style_inherit_mem = e->style_previous_mem; e->style_inherit_mem.trap = NULL;
        memset(&e->style_previous_mem, 0, sizeof e->style_previous_mem);
    }
    return true;
}

static void cascade_node(struct cascade *c, node_t *e, const style_t *parent);
/* Siblings queued together share one copy of their ancestor filter. */
struct cascade_job { node_t *node; const style_t *parent; float rem; bool full; uint8_t why; size_t ancestors; };
struct cascade_frontier {
    struct cascade_job *v; size_t n, cap;
    struct ancestor_filter *filters; size_t filter_count, filter_cap;
};
struct cascade_batch { struct cascade parent; struct cascade_job *jobs; const struct ancestor_filter *filters; };
static void cascade_enqueue(struct cascade *c, node_t *e, const style_t *parent) {
    if (!c->full && !e->style_dirty && !e->style_self_dirty && !e->style_children_dirty && e->style) return;
    struct cascade_frontier *f = c->frontier;
    if (f->n == f->cap) {
        size_t capacity = f->cap ? f->cap * 2 : 32;
        if (capacity < f->cap || capacity > SIZE_MAX / sizeof *f->v) ar_alloc(c->a, SIZE_MAX);
        struct cascade_job *v = realloc(f->v, capacity * sizeof *v);
        if (!v) ar_alloc(c->a, SIZE_MAX);
        f->v = v; f->cap = capacity;
    }
    if (c->ancestors_published == SIZE_MAX) {
        if (f->filter_count == f->filter_cap) {
            size_t capacity = f->filter_cap ? f->filter_cap * 2 : 8;
            if (capacity < f->filter_cap || capacity > SIZE_MAX / sizeof *f->filters) ar_alloc(c->a, SIZE_MAX);
            struct ancestor_filter *v = realloc(f->filters, capacity * sizeof *v);
            if (!v) ar_alloc(c->a, SIZE_MAX);
            f->filters = v; f->filter_cap = capacity;
        }
        ancestor_snapshot(&f->filters[f->filter_count], c->ancestor_base, c->ancestor_counts);
        c->ancestors_published = f->filter_count++;
    }
    f->v[f->n++] = (struct cascade_job){e, parent, c->rem, c->full, c->why, c->ancestors_published};
}
static void cascade_worker(size_t i, void *context) {
    struct cascade_batch *batch = context;
    struct cascade_job *job = &batch->jobs[i];
    node_t *e = job->node;
    if (e->type != N_ELEM || __atomic_load_n(&batch->parent.d->native_cancelled, __ATOMIC_ACQUIRE)) return;
    struct css_scratch scratch = {.worker=true};
    void *previous = thread_local_get(2); thread_local_set(2, &scratch);
    struct cascade c = batch->parent; c.vbuf = (sbuf){0};
    c.rem = job->rem; c.full = job->full; c.why = job->why; c.frontier = NULL;
    struct ancestor_counts counts = {0};
    c.ancestor_base = &batch->filters[job->ancestors]; c.ancestor_counts = &counts;
    c.ancestors_published = SIZE_MAX;
    jmp_buf trap; c.trap = &trap;
    if (setjmp(trap)) __atomic_store_n(&c.d->native_cancelled, true, __ATOMIC_RELEASE);
    else {
        setbits = malloc((size_t)css_prop_count());
        if (!setbits) __atomic_store_n(&c.d->native_cancelled, true, __ATOMIC_RELEASE);
        else cascade_node(&c, e, job->parent);
    }
    free(dents); free(setbits); sb_free(&c.vbuf);
    thread_local_set(2, previous);
}
static void cascade_service(void *context) {
    web_doc *d = context;
    web_native_checkpoint(d); web_avmedia_checkpoint();
}

/* The rules matching e, as declarations. Not inlined: its keys stay out of
   the frames of the recursive tree walk on the helpers' smaller stacks. */
static __attribute__((noinline)) void cascade_collect(struct cascade *c, node_t *e) {
    struct element_keys keys;
    element_keys_init(&keys, e, (struct ancestor_view){c->ancestor_base, c->ancestor_counts});
    collect_index(c->d->sty.ctx->ua, &keys, NULL);
    collect_scope(keys.root, &keys);
    if (e->shadow_root) collect_scope(e->shadow_root, &keys); /* :host */
    for (node_t *slot = e->assigned_slot; slot; slot = slot->assigned_slot)
        collect_scope(doc_node_root(slot, false), &keys); /* ::slotted */
}

static void cascade_node(struct cascade *c, node_t *e, const style_t *parent) {
    /* Native-only supply may run while this tree is being rebuilt. It never
       executes author code or reads the incomplete style/box tree. */
    if (css_can_service()) web_avmedia_checkpoint();
    web_doc *d = c->d;
    if (css_can_service()) { if (!web_native_checkpoint(d)) return; }
    else if (__atomic_load_n(&d->native_cancelled, __ATOMIC_ACQUIRE)) return;
    bool redo = c->full || e->style_dirty || e->style_self_dirty || !e->style;
    if (!redo && !e->style_children_dirty) return;
    bool inherited_full = c->full;
    /* Only this element's own match changed: its descendants restyle when what
       they inherit from it does, since they borrow those values from it. */
    bool self_only = redo && !c->full && !e->style_dirty && e->style;
    bool children_redo = redo, adopted = false;
    uint8_t inherited_why = c->why, why = c->full ? c->why : !e->style ? REDO_NEW :
        e->style_dirty ? REDO_MARKED + e->style_mark_source : REDO_SELF;
    if (!redo) goto descend;
    retire_style(c, e);
    css_scratch_get()->arena=c->a;
    if(d->profile_enabled) {
        __atomic_fetch_add(&d->profile.style_redo[why], 1, __ATOMIC_RELAXED);
        parallel_record_work(PARALLEL_STYLE, 1);
        __atomic_fetch_add(&d->profile.style_visits, 1, __ATOMIC_RELAXED);
        if (css_worker() && parallel_worker_index()) __atomic_fetch_add(&d->profile.style_parallel_nodes, 1, __ATOMIC_RELAXED);
    }
    ndents = 0;
    cascade_collect(c, e);
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
    struct css_declarations *inline_block = declarations_get(&e->style_declarations, sa, d->base, c->a);
    if (inline_block) {
        struct decl *ds = inline_block->values; int nd = inline_block->count;
        for(int i=0;i<nd;i++)if(container_units_used(ds[i].value))
            __atomic_store_n(&((struct css_ctx *)d->sty.ctx)->containers,true,__ATOMIC_RELEASE);
        for (int i = 0; i < nd; i++) {
            add_dent(&ds[i], 1ull << 62 | (uint64_t)0x3FFFFFFF << 32 | (uint64_t)(i < 255 ? i : 255), PE_NONE, depth);
            dents[ndents-1].inline_style=true;
        }
    }
    qsort(dents, (size_t)ndents, sizeof *dents, cmp_dent);
    bool has_before = false, has_after = false, has_placeholder = false;
    for (int i = 0; i < ndents; i++) {
        if (dents[i].pseudo == PE_BEFORE) has_before = true;
        if (dents[i].pseudo == PE_AFTER) has_after = true;
        if (dents[i].pseudo == PE_PLACEHOLDER) has_placeholder = true;
    }
    /* CSS Transitions 1 §3: the after-change style inherits from the parent's */
    node_t *flat_parent = doc_flat_parent(e);
    const style_t *parent_after = flat_parent && flat_parent->style == parent ? flat_parent->animation_after_style : NULL;
    style_t *base = NULL, *after = NULL;
    style_t *s = motion_compute(c, e, parent, parent_after, PE_NONE, e->style_previous, &base, &after);
    e->style = s;
    e->animation_base_style = base;
    e->animation_after_style = after;
    /* Only controls with matching authored declarations get a pseudo body.
       Its slot retires with the originating node's other computed styles. */
    if (has_placeholder && !e->foreign && (e->tag == T_textarea ||
        (e->tag == T_input && web_input_type(e) <= WEB_INPUT_PASSWORD)))
        s->placeholder = compute(c, e, s, PE_PLACEHOLDER);
    if (web_dialog_is_modal(d,e)) s->backdrop = compute(c,e,NULL,PE_BACKDROP);
    if (e == d->html) c->rem = s->font_size;
    /* CSSOM exposes resolved pseudo styles even when content:none, display:none
       or the originating replaced element prevents a generated box. Keep the
       body in the existing style lifetime, not in a second queried-style cache. */
    const style_t *previous = e->style_previous;
    style_t *pseudo_base = NULL, *pseudo_after = NULL;
    if (has_before) {
        s->before = motion_compute(c, e, s, after, PE_BEFORE, previous ? previous->before : NULL, &pseudo_base, &pseudo_after);
        if (base) base->before = pseudo_base ? pseudo_base : s->before;
    }
    if (has_after) {
        s->after = motion_compute(c, e, s, after, PE_AFTER, previous ? previous->after : NULL, &pseudo_base, &pseudo_after);
        if (base) base->after = pseudo_base ? pseudo_base : s->after;
    }
    s->before_generated=box_pseudo_generated(e,s,s->before);
    s->after_generated=box_pseudo_generated(e,s,s->after);
    if (base) {
        base->before_generated=box_pseudo_generated(e,base,base->before);
        base->after_generated=box_pseudo_generated(e,base,base->after);
    }
    if (self_only && !e->animation_base_style && style_equal(e->style_previous, s)) {
        keep_previous_style(e);
        children_redo = false;
        if (d->profile_enabled) __atomic_fetch_add(&d->profile.style_kept, 1, __ATOMIC_RELAXED);
    } else {
        if (!style_box_compatible(e, e->style_previous, s)) {
            __atomic_store_n(&d->style_boxes_changed, true, __ATOMIC_RELAXED);
            /* This worker alone writes e; the publication after the join reads
               box_regen from the retired list, and the release clears it. A moved
               element has no previous style (its removal unpublished it), but its
               box, still in the tree, borrows the arena that retires now. */
            bool boxed = e->style_previous || e->box;
            if(boxed){e->box_regen=true;__atomic_store_n(&d->style_existing_boxes_changed,true,__ATOMIC_RELAXED);}
            if(d->profile_enabled && boxed)__atomic_store_n(&d->box_change_node,e,__ATOMIC_RELAXED);
        }
        if (self_only && !e->animation_base_style && adopt_inherited(e)) {
            children_redo = false; adopted = true;
            if (d->profile_enabled) __atomic_fetch_add(&d->profile.style_adopted, 1, __ATOMIC_RELAXED);
        } else {
            /* The children restyle from the new values; what they borrowed
               retires with the old style. */
            ar_adopt(&e->style_previous_mem, &e->style_inherit_mem);
        }
    }
    e->style_mem.trap = NULL;
descend:
    e->style_dirty = e->style_children_dirty = e->style_self_dirty = false;
    s = e->style;
    if (s->display == D_NONE) {
        motion_cancel_subtree(e);
        clear_styles(e);
        return;
    }
    c->full = children_redo;
    c->why = why == REDO_SELF ? REDO_INHERITED : why;
    pvec children = {0};
    doc_flat_children(e, &children);
    /* A child that took a value it does not inherit from e reads e's own. */
    if (adopted)
        for (int i = 0; i < children.n; i++) {
            node_t *ch = children.v[i];
            if (ch->type == N_ELEM && ch->style && ch->style->explicit_inherit) ch->style_self_dirty = true;
        }
    size_t above_published = c->ancestors_published;
    ancestor_counts_apply(c->ancestor_counts, e, 1);
    c->ancestors_published = SIZE_MAX;
    if (c->frontier) {
        for (int i = 0; i < children.n; i++) {
            node_t *ch = children.v[i];
            if (ch->type == N_ELEM) cascade_enqueue(c, ch, s);
            else ch->style = NULL;
        }
    } else {
        for (int i = 0; i < children.n && !__atomic_load_n(&d->native_cancelled, __ATOMIC_ACQUIRE); i++) {
            node_t *ch = children.v[i];
            if (ch->type == N_ELEM) cascade_node(c, ch, s);
            else ch->style = NULL;
        }
    }
    ancestor_counts_apply(c->ancestor_counts, e, -1);
    c->ancestors_published = above_published;
    pv_free(&children);
    c->full = inherited_full; c->why = inherited_why;
}

/* The elements a cascade from root restyles, at most: every element below a
   restyled one, and in a partial cascade the marked ones along their paths.
   Only those paths are walked, so a small update never costs a document-wide
   pass. Before the accumulation an element's count is 1 exactly when it is
   restyled, which is what its children inherit. */
static uint32_t cascade_work_count(web_doc *d, node_t *root, bool full) {
    pvec nodes = {0}; pv_push(&nodes, root);
    /* Parent-first enumeration, followed by a reverse accumulation, avoids
       recursive counting on deeply nested flex/DOM chains. */
    for (int i = 0; i < nodes.n; i++) {
        if (!web_native_checkpoint(d)) { pv_free(&nodes); return 0; }
        node_t *n = nodes.v[i], *parent = i ? doc_flat_parent(n) : NULL;
        n->style_subtree_work = 0;
        if (n->type != N_ELEM) continue;
        bool inherited = parent ? parent->style_subtree_work != 0 : full;
        bool redo = inherited || n->style_dirty || n->style_self_dirty || !n->style;
        n->style_subtree_work = redo;
        if (redo || n->style_children_dirty) doc_flat_children(n, &nodes);
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
    uint32_t total = cascade_work_count(c->d, root, c->full);
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
        c->rem = job.rem; c->full = job.full; c->why = job.why;
        /* A copy: enqueueing the branch's children may move f.filters. The
           walk's counts are balanced, so they are empty between branches. */
        struct ancestor_filter base = f.filters[job.ancestors];
        c->ancestor_base = &base; c->ancestors_published = SIZE_MAX;
        cascade_node(c, job.node, job.parent);
    }
    c->frontier = NULL; c->ancestor_base = NULL;
    struct cascade_batch batch = {*c, f.v, f.filters};
    if (!__atomic_load_n(&c->d->native_cancelled, __ATOMIC_ACQUIRE))
        parallel_for_stage(PARALLEL_STYLE,f.n,cascade_worker,&batch,cascade_service,c->d);
    free(f.v); free(f.filters);
}

/* --debug-js: cascades between web_style_report() calls (main thread only). */
/* Why a cascade restyled the whole document, and why its rule index was rebuilt. */
enum { FULL_BY_INDEX, FULL_BY_MARK, FULL_BY_FIRST, FULL_BY_UNMARKED };
enum { INDEX_BY_SHEETS, INDEX_BY_ENVIRONMENT };
static struct {
    uint64_t cascades, full, full_by[4], ms, indexes, index_by[2], index_ms, rescans[2], elements, redo[REDO_WHYS], kept, adopted, candidates, filtered,
        bucket[4], lookups;
} style_profile;
int web_style_report(char *out, size_t size) {
    if (!style_profile.cascades) return 0;
    int n = snprintf(out, size, "Native style profile: cascades %lu/%lu ms (full %lu: index %lu, marked %lu, first %lu, unmarked %lu), bindings %lu (sheets %lu, environment %lu)/%lu ms, rescans %lu changed/%lu same, elements %lu (whole %lu, new %lu, inherited %lu, own %lu, marked: boundary %lu, reach %lu, inserted %lu, position %lu, relational %lu, parent state %lu; kept %lu, adopted %lu), marks %lu precise (parent %lu, subtree %lu)/%lu boundary, rule candidates %lu (id %lu, class %lu, tag %lu, universal %lu), filtered by ancestors %lu, bindings looked up %lu (inclusive)",
        (unsigned long)style_profile.cascades, (unsigned long)style_profile.ms, (unsigned long)style_profile.full,
        (unsigned long)style_profile.full_by[FULL_BY_INDEX], (unsigned long)style_profile.full_by[FULL_BY_MARK],
        (unsigned long)style_profile.full_by[FULL_BY_FIRST],
        (unsigned long)style_profile.full_by[FULL_BY_UNMARKED], (unsigned long)style_profile.indexes,
        (unsigned long)style_profile.index_by[INDEX_BY_SHEETS], (unsigned long)style_profile.index_by[INDEX_BY_ENVIRONMENT],
        (unsigned long)style_profile.index_ms, (unsigned long)style_profile.rescans[0], (unsigned long)style_profile.rescans[1],
        (unsigned long)style_profile.elements, (unsigned long)style_profile.redo[REDO_CASCADE],
        (unsigned long)style_profile.redo[REDO_NEW], (unsigned long)style_profile.redo[REDO_INHERITED],
        (unsigned long)style_profile.redo[REDO_SELF], (unsigned long)style_profile.redo[REDO_MARKED + MARK_BOUNDARY],
        (unsigned long)style_profile.redo[REDO_MARKED + MARK_REACH], (unsigned long)style_profile.redo[REDO_MARKED + MARK_INSERTED],
        (unsigned long)style_profile.redo[REDO_MARKED + MARK_POSITION], (unsigned long)style_profile.redo[REDO_MARKED + MARK_RELATIONAL],
        (unsigned long)style_profile.redo[REDO_MARKED + MARK_PARENT_STATE],
        (unsigned long)style_profile.kept, (unsigned long)style_profile.adopted,
        (unsigned long)style_marks.precise, (unsigned long)style_marks.parent, (unsigned long)style_marks.subtree,
        (unsigned long)style_marks.boundary, (unsigned long)style_profile.candidates,
        (unsigned long)style_profile.bucket[0], (unsigned long)style_profile.bucket[1],
        (unsigned long)style_profile.bucket[2], (unsigned long)style_profile.bucket[3],
        (unsigned long)style_profile.filtered, (unsigned long)style_profile.lookups);
    const char *separator = "; wide ";
    for (int i = 0; i < STYLE_MARK_NAMES && n >= 0 && (size_t)n < size; i++)
        if (style_marks.names[i].count) {
            n += snprintf(out + n, size - (size_t)n, "%s%s %lu", separator, style_marks.names[i].name,
                          (unsigned long)style_marks.names[i].count);
            separator = ", ";
        }
    memset(&style_profile, 0, sizeof style_profile);
    memset(&style_marks, 0, sizeof style_marks);
    return n;
}

/* A rescan rebuilds the sheet list from the tree; most rescans are for other
   resources and rebuild the same list. The bindings and the styles computed
   from them stay valid when it is the list they were built from: the same
   ASTs, in the same scopes, media and import structure, in the same order. */
void css_sheets_rescan(struct styling *st) {
    if (st->ctx && (st->index_dirty || !st->ctx->complete)) { css_ctx_free(st->ctx); st->ctx = NULL; }
    st->sheets.n = 0;
}
bool css_sheets_rescanned(web_doc *d) {
    struct css_ctx *x = d->sty.ctx;
    bool same = x && sheets_sort(d) && x->nsheets == d->sty.sheets.n;
    for (int i = 0; same && i < x->nsheets; i++) {
        struct sheet_signature now = sign_sheet(NULL, &d->sty.sheets, i);
        const struct sheet_signature *was = &x->sheets[i];
        same = now.serial == was->serial && now.scope == was->scope && now.ua == was->ua && !strcmp(now.media, was->media) &&
               now.import_parent == was->import_parent && now.import_owner == was->import_owner;
    }
    if (d->profile_enabled) style_profile.rescans[same]++;
    if (same) { d->sty.index_dirty = false; return true; }
    css_ctx_free(x); d->sty.ctx = NULL; d->sty.index_dirty = true;
    return false;
}

void css_cascade(web_doc *d, int vw, int vh) {
    uint64_t profile_start = d->profile_enabled ? uptime_ms() : 0;
    struct web_profile profile_before = d->profile;
    /* Publish pool ownership before spawning disjoint subtree jobs. */
    if (!d->style_pool) {
        d->style_pool = calloc(1, sizeof *d->style_pool);
        if (!d->style_pool) { ar_alloc(&d->smem, SIZE_MAX); return; }
        d->style_pool->owned = true;
    }
    /* Direct native callers may recascade without the document publication
       helper. Do not overwrite a prior retirement generation while boxes
       still borrow it. The ordinary web_layout path releases after each join. */
    if(d->style_retired_first) {
        if(d->root_box){boxes_discard(d);d->need_boxes=true;d->layout_valid=false;}
        css_styles_release(d);
    }
    /* Keys borrow computed-style names. Never carry a previous document or
       style-arena generation into this cascade; release on normal exit too. */
    font_bindings_clear();font_faces_available=d->author_fonts.n>0;
    for(int i=0;i<d->sty.sheets.n;i++)if(((sheet_t *)d->sty.sheets.v[i])->fonts){font_faces_available=true;break;}
    for(struct web_font_resource *f=d->fonts;f;f=f->next)f->wanted=false;
    bool scripting = web_js_enabled(d);
    /* Rule indexing depends on the stylesheet snapshot/media environment,
       not on the current hover/active target, class attribute or inline style.
       Those changes still recompute every real selector and computed style.
       Resource/CSSOM replacement unpublishes ctx before releasing its AST;
       new sheets mark index_dirty at their publication boundary. */
    bool marked_full = d->style_full_dirty;
    int index_by = -1;
    if (!d->sty.ctx || d->sty.index_dirty || d->sty.index_w != vw || d->sty.index_h != vh ||
        d->sty.index_scripting != scripting || d->sty.index_quirks != d->quirks) {
        index_by = !d->sty.ctx || d->sty.index_dirty ? INDEX_BY_SHEETS : INDEX_BY_ENVIRONMENT;
        uint64_t index_start = d->profile_enabled ? uptime_ms() : 0;
        css_ctx_free(d->sty.ctx); d->sty.ctx = NULL;
        d->sty.ctx = build_ctx(d, vw, vh);
        d->sty.index_w = vw;
        d->sty.index_h = vh;
        d->sty.index_scripting = scripting;
        d->sty.index_quirks = d->quirks;
        d->sty.index_dirty = !d->sty.ctx->complete;
        d->style_full_dirty = true;
        if (d->profile_enabled) {
            style_profile.indexes++; style_profile.index_by[index_by]++;
            style_profile.index_ms += uptime_ms() - index_start;
        }
    }
    /* Decided before the form snapshot: its lazy control initialization marks
       those controls, which must join this cascade, not turn it partial. */
    bool full = d->style_full_dirty || !d->style_pending_dirty || !d->html || !d->html->style;
    int full_by = index_by >= 0 ? FULL_BY_INDEX : marked_full ? FULL_BY_MARK :
                  !d->html || !d->html->style ? FULL_BY_FIRST : FULL_BY_UNMARKED;
    if (!web_form_style_snapshot(d, d->sty.ctx->form_features)) {
        __atomic_store_n(&d->native_cancelled, true, __ATOMIC_RELEASE);
        return;
    }
    if (!setbits) { setbits = malloc((size_t)css_prop_count()); if(!setbits)ar_alloc(&d->smem,SIZE_MAX); }
    /* Nodes leave the flat tree only by slot reassignment, which rebuilds
       every box. Otherwise boxes still borrow the styles they name, and the
       element styles this full cascade replaces retire through style_previous
       for the local publication to follow. */
    if(full && (d->need_boxes || !d->root_box) && d->root && (!d->root_box || d->box_changes_full))
        clear_styles(d->root); /* retire styles of nodes no longer in the flat tree */
    d->style_boxes_changed = false;
    d->style_existing_boxes_changed = false;
    d->box_change_node = NULL;
    struct ancestor_counts ancestors = {0};
    struct cascade c = {.d=d, .rem=d->html && d->html->style ? d->html->style->font_size : 16,
        .vw=vw, .vh=vh, .a=&d->smem, .full=full, .why=REDO_CASCADE, .trap=d->smem.trap,
        .ancestor_counts=&ancestors, .ancestors_published=SIZE_MAX};
    /* the timeline's time holds still through the cascade; workers push new records, joined after */
    web_animations_cascading(d, true);
    if (d->html) cascade_frontier_run(&c, d->html);
    web_animations_cascading(d, false);
    font_bindings_clear();
    sb_free(&c.vbuf);
    d->styled_w = vw;
    d->styled_h = vh;
    d->style_full_dirty = d->style_pending_dirty = false;
    if (d->profile_enabled) {
        style_profile.cascades++; style_profile.full += full;
        if (full) style_profile.full_by[full_by]++;
        style_profile.ms += uptime_ms() - profile_start;
        style_profile.elements += d->profile.style_visits - profile_before.style_visits;
        style_profile.kept += d->profile.style_kept - profile_before.style_kept;
        style_profile.adopted += d->profile.style_adopted - profile_before.style_adopted;
        for (int i = 0; i < REDO_WHYS; i++) style_profile.redo[i] += d->profile.style_redo[i] - profile_before.style_redo[i];
        style_profile.candidates += d->profile.style_rule_checks - profile_before.style_rule_checks;
        style_profile.filtered += d->profile.style_rule_filtered - profile_before.style_rule_filtered;
        style_profile.lookups += d->profile.style_rule_indexes - profile_before.style_rule_indexes;
        for (int i = 0; i < 4; i++)
            style_profile.bucket[i] += d->profile.style_rule_bucket[i] - profile_before.style_rule_bucket[i];
    }
}

/* ---------------------------------------------------------------- DevTools */
void css_sheet_href(sheet_t *sheet, const char *href) { if (sheet) sheet->href = href; }

static bool inspect_decls(const struct decl *v, int count, const struct css_inspect_sink *sink) {
    for (int i = 0; i < count; i++) {
        const struct decl *x = &v[i];
        const char *name = x->p ? css_prop_name(css_prop_index(x->p), NULL) : x->name;
        if (!name) continue;
        if (!sink->decl(sink->ctx, name, x->value ? x->value : "", x->important,
                        x->p ? css_prop_inherited(x->p) : true)) return false;
    }
    return true;
}
/* A scoped sheet applies inside its shadow tree and to its host; document
   sheets apply to the document tree. The matcher handles :host/::slotted. */
static bool inspect_scope_ok(const sheet_t *s, node_t *e) {
    node_t *root = e;
    while (root->parent) root = root->parent;
    if (!s->scope) return root->type == N_DOC;
    return root == s->scope || e == s->scope->shadow_host;
}
static bool inspect_sheet(web_doc *d, const sheet_t *s, node_t *e, uint32_t *order, const struct css_inspect_sink *sink) {
    int vw = d->sty.index_w > 0 ? d->sty.index_w : d->styled_w, vh = d->sty.index_h > 0 ? d->sty.index_h : d->styled_h;
    bool scripting = web_js_enabled(d);
    if (!sheet_media_ok(s, vw, vh, scripting) || !inspect_scope_ok(s, e)) { for (struct rule *r = s->first; r; r = r->next) (*order)++; return true; }
    const struct selector_match context = {.shadow = s->scope};
    for (struct rule *r = s->first; r; r = r->next) {
        uint32_t at = (*order)++;
        if (!r->ndecls || !r->sel || !media_chain_ok(r->media, vw, vh, scripting)) continue;
        bool matched = false; uint32_t spec = 0;
        for (int k = 0; k < r->sel->n; k++) {
            const struct selector *x = r->sel->v[k];
            if (x->pseudo || !match_from_bound(x->right, e, &context)) continue;
            if (!matched || x->spec > spec) spec = x->spec;
            matched = true;
        }
        if (!matched) continue;
        if (!sink->rule(sink->ctx, r->selector_text ? r->selector_text : "", s->href, s->ua, spec, at)) return false;
        if (!inspect_decls(r->decls, r->ndecls, sink)) return false;
    }
    return true;
}
bool css_inspect_rules(web_doc *d, node_t *e, const struct css_inspect_sink *sink) {
    if (!d || !e || e->type != N_ELEM || !sink) return false;
    uint32_t order = 0;
    if (ua_sheet && !inspect_sheet(d, ua_sheet, e, &order, sink)) return false;
    if (quirks_sheet && d->quirks && !inspect_sheet(d, quirks_sheet, e, &order, sink)) return false;
    for (int i = 0; i < d->sty.sheets.n; i++)
        if (!inspect_sheet(d, d->sty.sheets.v[i], e, &order, sink)) return false;
    return true;
}
bool css_inspect_inline(web_doc *d, node_t *e, const struct css_inspect_sink *sink) {
    (void)d;
    if (!e || e->type != N_ELEM || !sink) return false;
    const struct css_declarations *block = e->style_declarations;
    if (!sink->rule(sink->ctx, "element.style", NULL, false, UINT32_MAX, UINT32_MAX)) return false;
    return !block || inspect_decls(block->values, block->count, sink);
}
