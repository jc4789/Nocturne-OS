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
/* SK_NS: the namespace a compound's element must be in (a written prefix, or a
   stylesheet's default namespace), first in its compound and before its SK_TAG.
   SK_PE: a pseudo-element where it was written, for serialization; matching
   reads only its selector's pseudo. */
enum { SK_TAG, SK_ID, SK_CLASS, SK_ATTR, SK_PC, SK_SLOTTED, SK_ANCHOR, SK_NS, SK_PE };
_Static_assert(PE_PLACEHOLDER != PE_SLOTTED, "pseudo-element overlaps assigned-element selector marker");
enum { AO_EXISTS, AO_EQ, AO_INCL, AO_DASH, AO_PREFIX, AO_SUFFIX, AO_SUBSTR };
enum { PC_FIRST_CHILD, PC_LAST_CHILD, PC_ONLY_CHILD, PC_NTH_CHILD, PC_NTH_LAST_CHILD, PC_FIRST_OF_TYPE,
       PC_LAST_OF_TYPE, PC_ONLY_OF_TYPE, PC_NTH_OF_TYPE, PC_NTH_LAST_OF_TYPE, PC_NOT, PC_IS, PC_NEVER,
       PC_ALWAYS, PC_LINK, PC_CHECKED, PC_DISABLED, PC_ENABLED, PC_ROOT, PC_EMPTY, PC_REQUIRED, PC_OPTIONAL,
       PC_LANG, PC_PLACEHOLDER_SHOWN, PC_READ_WRITE, PC_READ_ONLY, PC_OPEN, PC_HOST, PC_FOCUS, PC_FOCUS_WITHIN, PC_HOVER, PC_ACTIVE,
       PC_VALID, PC_INVALID, PC_IN_RANGE, PC_OUT_OF_RANGE, PC_INDETERMINATE, PC_MODAL,PC_FULLSCREEN, PC_HAS, PC_SCOPE,
       PC_FOCUS_VISIBLE, PC_HEADING, PC_STATE, PC_DIR, PC_HOST_CONTEXT };
/* A namespace written in a selector: none (an attribute in no namespace; a type
   in the default namespace, or in any when there is none), *|, | or prefix|. */
enum { NSK_DEFAULT, NSK_ANY, NSK_NONE, NSK_URI };
struct sel_qname {
    const char *raw;    /* SK_TAG, SK_ATTR: the local name as written */
    const char *prefix; /* NSK_URI: the prefix as written */
    const char *uri;    /* NSK_URI: the namespace (NULL: no namespace) */
    uint8_t ns;         /* NSK_* */
    bool implicit;      /* SK_NS: the stylesheet's default namespace, written as nothing */
};

struct sellist;
struct simple {
    uint8_t kind, op, pc;
    uint8_t flag;       /* SK_ATTR: the case modifier as written, 0, 'i' or 's' */
    bool icase;         /* SK_ATTR: 'i' */
    bool html_list;     /* SK_ATTR: a name whose values HTML compares ASCII case-insensitively */
    uint16_t tag;       /* SK_TAG: the HTML tag ID; :heading(): the levels 1 to 9 it lists, as bits */
    const char *name;   /* SK_TAG, SK_ATTR: ASCII-lowercased; SK_PC, SK_PE: the serialized name */
    union {
        const char *value; /* [attr], :lang() (each range: 'i' or 's', text, NUL; NUL), :dir(), :state(),
                              ::part() (names, NUL each; NUL), other pseudo-element arguments */
        struct { int a, b; }; /* :nth-*(An+B) */
    };
    struct sellist *args;
    const struct sel_qname *q; /* SK_NS; SK_TAG, SK_ATTR when written in another case or with a namespace */
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
    bool part; /* a ::part(): its subject is an element its host's tree exposes */
};
/* A forgiving list keeps its invalid items' text (trimmed), serialized as written
   before the valid item at `at`; matching never reads them. */
struct sel_raw { int at; const char *text; };
struct sellist {
    int n, nraw;
    const struct sel_raw *raw;
    struct selector *v[];
};
/* @namespace: a stylesheet's declared prefixes (NULL prefix: the default) */
struct css_namespace { const char *prefix, *uri; struct css_namespace *next; };

struct decl {
    const struct propdef *p; /* NULL: a custom property */
    const char *name;        /* custom properties */
    const char *value;
    const char *base;        /* custom properties: the declaring sheet's URL (a registered <url> resolves against it) */
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

/* @position-try (css-anchor-position-1 §6.4): a named position option, its accepted declarations
   without !important; rules of one name cascade as @keyframes do */
struct position_try_rule {
    struct position_try_rule *next;
    const char *name;
    const struct mcond *media;
    const struct css_layer_decl *layer;
    struct decl *decls;
    int count;
};

/* @property (css-properties-values-api-1 §3): a valid rule's registration as declared */
struct property_rule {
    struct property_rule *next;
    const char *name, *syntax, *initial; /* syntax unescaped; initial NULL when absent */
    const char *base;
    bool inherits;
    const struct mcond *media;
};

struct sheet {
    struct rule *first, *last;
    struct font_face *fonts,*fonts_last;
    struct opacity_keyframes *motions, *motions_last;
    struct position_try_rule *position_tries, *position_tries_last;
    struct property_rule *properties, *properties_last;
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
    struct css_namespace *namespaces; /* @namespace, in source order; the parse's own */
    /* The parsed AST every instance shares, the arena that owns it, its
       identity across rescans (never reused, unlike its address), and its
       rule index, built at its first cascade. */
    sheet_t *ast;
    arena_t *arena;
    uint64_t serial;
    struct css_index *index;
    /* A CSSOM parse whose top-level text put rules into the lists above that
       no top-level CSSOM rule's span holds (@document, an @font-face with a
       prelude): only a parse of its whole text can change it. */
    bool cssom_hidden;
};
/* What one top-level rule of a CSSOM parse put into its sheet's lists: the
   first and last of each, adjacent there (NULL: none). */
struct css_rule_span {
    struct rule *rules, *rules_last;
    struct font_face *fonts, *fonts_last;
    struct opacity_keyframes *motions, *motions_last;
    struct css_layer_event *layers, *layers_last;
};

struct pctx {
    arena_t *a;
    sheet_t *sh;
    const char *base;
    pvec *imports;
    int nimports;
    unsigned rule_depth;
    struct css_layer_decl *layer;
    bool import_allowed, namespace_allowed;
    /* the selector being parsed: a forgiving list kept an invalid item; it
       holds syntax this engine accepts but never matches */
    bool forgiven, unsupported;
    /* CSSOM parses record the whole rule tree. cssom_parent is the rule
       being filled; cssom_implicit marks a nested group rule's block, whose
       declarations are nested declarations rather than a style rule's own. */
    bool cssom_tree, cssom_implicit, imports_seen;
    struct css_rule_info *cssom_parent;
    struct cssom_run *cssom_run; /* the declaration run of cssom_parent's block */
    /* cssom.c's nested-declarations markers: sorted offsets into cssom_text */
    const char *cssom_text;
    const uint32_t *cssom_markers;
    uint32_t cssom_nmarkers;
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

/* ---------------------------------------------------------------- selectors
   Selectors 4 over CSS Syntax 3 tokens. A selector's text is tokenized where it
   is parsed: comments drop out, CR and FF are newlines, NUL is U+FFFD. The tree
   keeps what CSSOM's "serialize a selector" reads (names as written, namespace
   prefixes, each pseudo-element where it was written, the text of a forgiving
   list's invalid items) beside what matching reads. */
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

/* ---- CSS Syntax tokens */
enum { ST_EOF, ST_WS, ST_IDENT, ST_FUNCTION, ST_AT, ST_HASH, ST_STRING, ST_BAD_STRING, ST_URL, ST_BAD_URL,
       ST_DELIM, ST_NUMBER, ST_PERCENT, ST_DIMENSION, ST_CDO, ST_CDC, ST_COLON, ST_SEMICOLON, ST_COMMA,
       ST_LBRACKET, ST_RBRACKET, ST_LPAREN, ST_RPAREN, ST_LBRACE, ST_RBRACE };
struct stok {
    uint8_t type;
    bool id;           /* hash: its type flag is "id" */
    bool integer;      /* number, dimension: its type flag is "integer" */
    bool sign;         /* number, dimension: written with + or - */
    uint32_t delim;
    double number;
    const char *s, *e;   /* the token's text */
    const char *vs, *ve; /* ident, function, at-keyword, hash: the name; string: between the quotes;
                            dimension: the unit */
};
static bool st_newline(unsigned char c) { return c == '\n' || c == '\r' || c == '\f'; }
static bool st_digit(unsigned char c) { return c >= '0' && c <= '9'; }
/* NUL is U+FFFD, a non-ASCII code point */
static bool st_ident_start(unsigned char c) { return ((c | 32) >= 'a' && (c | 32) <= 'z') || c == '_' || c >= 0x80 || !c; }
static bool st_ident_char(unsigned char c) { return st_ident_start(c) || st_digit(c) || c == '-'; }
static int st_hex(unsigned char c) {
    return c >= '0' && c <= '9' ? c - '0' : (c | 32) >= 'a' && (c | 32) <= 'f' ? (c | 32) - 'a' + 10 : -1;
}
static bool st_escape(const char *p, const char *e) {
    return p < e && *p == '\\' && !(p + 1 < e && st_newline((unsigned char)p[1]));
}
/* past an escape whose backslash is just before p */
static const char *st_escape_end(const char *p, const char *e) {
    if (p >= e) return p;
    if (st_hex((unsigned char)*p) >= 0) {
        for (int k = 0; k < 6 && p < e && st_hex((unsigned char)*p) >= 0; k++) p++;
        if (p < e && is_space((unsigned char)*p)) p += *p == '\r' && p + 1 < e && p[1] == '\n' ? 2 : 1;
        return p;
    }
    p++;
    while (p < e && ((unsigned char)*p & 0xC0) == 0x80) p++;
    return p;
}
static bool st_starts_ident(const char *p, const char *e) {
    if (p >= e) return false;
    if (*p == '-') return p + 1 < e && (st_ident_start((unsigned char)p[1]) || p[1] == '-' || st_escape(p + 1, e));
    if (*p == '\\') return st_escape(p, e);
    return st_ident_start((unsigned char)*p);
}
static bool st_starts_number(const char *p, const char *e) {
    if (p < e && (*p == '+' || *p == '-')) p++;
    if (p < e && st_digit((unsigned char)*p)) return true;
    return p + 1 < e && *p == '.' && st_digit((unsigned char)p[1]);
}
static const char *st_ident_end(const char *p, const char *e) {
    while (p < e) {
        if (st_ident_char((unsigned char)*p)) p++;
        else if (st_escape(p, e)) p = st_escape_end(p + 1, e);
        else break;
    }
    return p;
}
/* The next code point of a name or a string, escapes resolved (a string's
   escaped newline is its caller's); NUL, an invalid escape and a broken
   UTF-8 sequence are U+FFFD. */
static uint32_t st_cp(const char **ps, const char *e) {
    const unsigned char *p = (const unsigned char *)*ps;
    uint32_t cp;
    if (*p == '\\') {
        p++;
        if ((const char *)p >= e) { *ps = (const char *)p; return 0xFFFD; }
        if (st_hex(*p) >= 0) {
            cp = 0;
            for (int k = 0; k < 6 && (const char *)p < e && st_hex(*p) >= 0; k++) cp = cp * 16 + (uint32_t)st_hex(*p++);
            if ((const char *)p < e && is_space(*p)) p += *p == '\r' && (const char *)p + 1 < e && p[1] == '\n' ? 2 : 1;
            if (!cp || cp > 0x10FFFF || (cp >= 0xD800 && cp <= 0xDFFF)) cp = 0xFFFD;
            *ps = (const char *)p;
            return cp;
        }
    }
    cp = *p++;
    if (cp >= 0x80) {
        unsigned more = cp >= 0xF0 ? 3 : cp >= 0xE0 ? 2 : cp >= 0xC0 ? 1 : 0;
        if (!more) cp = 0xFFFD;
        else {
            cp &= 0x3Fu >> more;
            for (; more && (const char *)p < e && (*p & 0xC0) == 0x80; more--) cp = cp << 6 | (*p++ & 0x3Fu);
            if (more) cp = 0xFFFD;
        }
    } else if (!cp) cp = 0xFFFD;
    *ps = (const char *)p;
    return cp;
}
static size_t st_decode(const char *s, const char *e, bool string, bool lowercase, char *out) {
    size_t n = 0;
    while (s < e) {
        if (string && *s == '\\' && (s + 1 == e || st_newline((unsigned char)s[1]))) {
            s += s + 1 == e ? 1 : 2 + (s[1] == '\r' && s + 2 < e && s[2] == '\n');
            continue;
        }
        uint32_t cp = st_cp(&s, e);
        if (lowercase && cp < 128) cp = (uint32_t)lower((int)cp);
        char u[4];
        int k = utf8_put(u, cp);
        if (out) memcpy(out + n, u, (size_t)k);
        n += (size_t)k;
    }
    return n;
}
/* a name's (or, string, a string's) value in the arena */
static char *st_text(arena_t *a, const char *s, const char *e, bool string, bool lowercase) {
    bool plain = true;
    for (const char *p = s; p < e && plain; p++) plain = *p != '\\' && *p && !(lowercase && *p >= 'A' && *p <= 'Z');
    if (plain) return ar_strndup(a, s, (size_t)(e - s));
    size_t n = st_decode(s, e, string, lowercase, NULL);
    char *out = ar_alloc(a, n + 1);
    st_decode(s, e, string, lowercase, out);
    out[n] = 0;
    return out;
}
/* whether a name is the ASCII lowercase literal, ASCII case-insensitively */
static bool st_is(const char *s, const char *e, const char *literal) {
    while (s < e) {
        if (!*literal) return false;
        uint32_t cp = st_cp(&s, e);
        if (cp >= 128 || lower((int)cp) != (unsigned char)*literal++) return false;
    }
    return !*literal;
}
static void st_numeric(const char **ps, const char *e, struct stok *t) {
    const char *p = *ps;
    double sign = 1, v = 0;
    t->integer = true;
    if (*p == '+' || *p == '-') { t->sign = true; if (*p == '-') sign = -1; p++; }
    while (p < e && st_digit((unsigned char)*p)) v = v * 10 + (*p++ - '0');
    if (p + 1 < e && *p == '.' && st_digit((unsigned char)p[1])) {
        t->integer = false;
        p++;
        for (double f = 0.1; p < e && st_digit((unsigned char)*p); f /= 10) v += (*p++ - '0') * f;
    }
    if (p < e && (*p | 32) == 'e') {
        const char *q = p + 1;
        int es = 1, x = 0;
        if (q < e && (*q == '+' || *q == '-')) { if (*q == '-') es = -1; q++; }
        if (q < e && st_digit((unsigned char)*q)) {
            t->integer = false;
            for (; q < e && st_digit((unsigned char)*q); q++) if (x < 100000) x = x * 10 + (*q - '0');
            v *= pow(10, es * x);
            p = q;
        }
    }
    t->number = sign * v;
    if (st_starts_ident(p, e)) { t->type = ST_DIMENSION; t->vs = p; p = t->ve = st_ident_end(p, e); }
    else if (p < e && *p == '%') { t->type = ST_PERCENT; p++; }
    else t->type = ST_NUMBER;
    *ps = p;
}
static void st_identlike(const char **ps, const char *e, struct stok *t) {
    const char *p = *ps;
    t->vs = p;
    p = t->ve = st_ident_end(p, e);
    t->type = ST_IDENT;
    if (p < e && *p == '(') {
        p++;
        t->type = ST_FUNCTION;
        if (st_is(t->vs, t->ve, "url")) {
            const char *q = p;
            while (q < e && is_space((unsigned char)*q)) q++;
            if (!(q < e && (*q == '"' || *q == '\''))) {
                t->type = ST_URL;
                for (p = q; p < e && *p != ')';) {
                    unsigned char c = (unsigned char)*p;
                    if (is_space(c)) {
                        const char *w = p;
                        while (w < e && is_space((unsigned char)*w)) w++;
                        p = w;
                        if (w < e && *w != ')') t->type = ST_BAD_URL;
                        continue;
                    }
                    if (c == '"' || c == '\'' || c == '(' || c == 0x7F || c == 0x0B || (c < 0x20 && c != '\t' && !st_newline(c)) ||
                        (c == '\\' && !st_escape(p, e))) t->type = ST_BAD_URL;
                    p = c == '\\' && st_escape(p, e) ? st_escape_end(p + 1, e) : p + 1;
                }
                if (p < e) p++;
            }
        }
    }
    *ps = p;
}
/* CSS Syntax "consume a token" from *ps, comments skipped */
static void st_next(const char **ps, const char *e, struct stok *t) {
    const char *p = *ps;
    while (p + 1 < e && p[0] == '/' && p[1] == '*') {
        const char *q = p + 2;
        while (q + 1 < e && !(q[0] == '*' && q[1] == '/')) q++;
        p = q + 1 < e ? q + 2 : e;
    }
    memset(t, 0, sizeof *t);
    t->s = p;
    if (p >= e) { t->type = ST_EOF; t->e = p; *ps = p; return; }
    unsigned char c = (unsigned char)*p;
    switch (c) {
    case ' ': case '\t': case '\n': case '\r': case '\f':
        while (p < e && is_space((unsigned char)*p)) p++;
        t->type = ST_WS;
        break;
    case '"': case '\'':
        t->type = ST_STRING;
        t->vs = ++p;
        while (p < e && (unsigned char)*p != c) {
            if (st_newline((unsigned char)*p)) { t->type = ST_BAD_STRING; break; }
            if (*p == '\\' && p + 1 < e) {
                if (st_newline((unsigned char)p[1])) p += 2 + (p[1] == '\r' && p + 2 < e && p[2] == '\n');
                else p = st_escape_end(p + 1, e);
            } else p++;
        }
        t->ve = p;
        if (t->type == ST_STRING && p < e) p++;
        break;
    case '#':
        if (p + 1 < e && (st_ident_char((unsigned char)p[1]) || st_escape(p + 1, e))) {
            t->type = ST_HASH;
            t->id = st_starts_ident(p + 1, e);
            t->vs = p + 1;
            p = t->ve = st_ident_end(p + 1, e);
        } else { t->type = ST_DELIM; t->delim = c; p++; }
        break;
    case '(': t->type = ST_LPAREN; p++; break;
    case ')': t->type = ST_RPAREN; p++; break;
    case '[': t->type = ST_LBRACKET; p++; break;
    case ']': t->type = ST_RBRACKET; p++; break;
    case '{': t->type = ST_LBRACE; p++; break;
    case '}': t->type = ST_RBRACE; p++; break;
    case ',': t->type = ST_COMMA; p++; break;
    case ':': t->type = ST_COLON; p++; break;
    case ';': t->type = ST_SEMICOLON; p++; break;
    case '+': case '.':
        if (st_starts_number(p, e)) st_numeric(&p, e, t);
        else { t->type = ST_DELIM; t->delim = c; p++; }
        break;
    case '-':
        if (st_starts_number(p, e)) st_numeric(&p, e, t);
        else if (p + 2 < e && p[1] == '-' && p[2] == '>') { t->type = ST_CDC; p += 3; }
        else if (st_starts_ident(p, e)) st_identlike(&p, e, t);
        else { t->type = ST_DELIM; t->delim = c; p++; }
        break;
    case '<':
        if (e - p >= 4 && !memcmp(p, "<!--", 4)) { t->type = ST_CDO; p += 4; }
        else { t->type = ST_DELIM; t->delim = c; p++; }
        break;
    case '@':
        if (st_starts_ident(p + 1, e)) { t->type = ST_AT; t->vs = p + 1; p = t->ve = st_ident_end(p + 1, e); }
        else { t->type = ST_DELIM; t->delim = c; p++; }
        break;
    case '\\':
        if (st_escape(p, e)) st_identlike(&p, e, t);
        else { t->type = ST_DELIM; t->delim = c; p++; }
        break;
    default:
        if (st_digit(c)) st_numeric(&p, e, t);
        else if (st_ident_start(c)) st_identlike(&p, e, t);
        else { t->type = ST_DELIM; t->delim = c; p++; }
    }
    t->e = p;
    *ps = p;
}
/* Where the component values from p stop: at the first `close` (or, comma,
   the first comma) outside every block they open, or at e, which closes what
   is still open. *bad: they hold a bad string or URL, or a closer that closes
   nothing open (the tokens <any-value> excludes). */
static const char *st_scan(struct pctx *pc, const char *p, const char *e, int close, bool comma, bool *bad) {
    char local[32], *stack = local;
    size_t depth = 0, capacity = sizeof local;
    struct stok t;
    for (;;) {
        st_next(&p, e, &t);
        switch (t.type) {
        case ST_EOF: return e;
        case ST_FUNCTION: case ST_LPAREN: case ST_LBRACKET: case ST_LBRACE:
            if (depth == capacity) stack = selector_list_grow(pc, stack, depth, &capacity, 1);
            stack[depth++] = t.type == ST_LBRACKET ? ']' : t.type == ST_LBRACE ? '}' : ')';
            break;
        case ST_RPAREN: case ST_RBRACKET: case ST_RBRACE: {
            char c = t.type == ST_RPAREN ? ')' : t.type == ST_RBRACKET ? ']' : '}';
            if (depth && stack[depth - 1] == c) depth--;
            else if (!depth && c == close) return t.s;
            else if (bad) *bad = true;
            break;
        }
        case ST_COMMA: if (comma && !depth) return t.s; break;
        case ST_BAD_STRING: case ST_BAD_URL: if (bad) *bad = true; break;
        default: break;
        }
    }
}

/* ---- CSSOM serialization */
/* CSSOM "serialize an identifier" and "serialize a string" (util.c) */
static void ser_ident(sbuf *b, const char *s) { css_serialize_ident(b, s); }
static void ser_string(sbuf *b, const char *s) { css_serialize_string(b, s); }
static void ser_int(sbuf *b, long long v) {
    char t[24];
    snprintf(t, sizeof t, "%lld", v);
    sb_puts(b, t);
}
/* CSS Syntax "serialize <an+b>" */
static void ser_anb(sbuf *b, int a, int c) {
    if (!a) { ser_int(b, c); return; }
    if (a == -1) sb_putc(b, '-');
    else if (a != 1) ser_int(b, a);
    sb_putc(b, 'n');
    if (c > 0) sb_putc(b, '+');
    if (c) ser_int(b, c);
}
/* a list of NUL-terminated names ending in an empty one */
static void ser_names(sbuf *b, const char *list, const char *separator) {
    for (const char *p = list; p && *p; p += strlen(p) + 1) {
        if (p != list) sb_puts(b, separator);
        ser_ident(b, p);
    }
}
static void ser_list(sbuf *b, const struct sellist *l);
static void ser_simple(sbuf *b, const struct simple *s) {
    static const char *const matchers[] = {"", "=", "~=", "|=", "^=", "$=", "*="};
    switch (s->kind) {
    case SK_ID: sb_putc(b, '#'); ser_ident(b, s->name); break;
    case SK_CLASS: sb_putc(b, '.'); ser_ident(b, s->name); break;
    case SK_ATTR:
        sb_putc(b, '[');
        if (s->q && s->q->ns == NSK_ANY) sb_puts(b, "*|");
        else if (s->q && s->q->ns == NSK_URI) { ser_ident(b, s->q->prefix); sb_putc(b, '|'); }
        ser_ident(b, s->q ? s->q->raw : s->name);
        if (s->op != AO_EXISTS) { sb_puts(b, matchers[s->op]); ser_string(b, s->value); }
        if (s->flag) { sb_putc(b, ' '); sb_putc(b, (char)s->flag); }
        sb_putc(b, ']');
        break;
    case SK_SLOTTED: sb_puts(b, "::slotted("); ser_list(b, s->args); sb_putc(b, ')'); break;
    case SK_PE:
        sb_puts(b, "::");
        ser_ident(b, s->name); /* a ::-webkit-* name is any identifier */
        if (s->flag) {
            sb_putc(b, '(');
            if (s->op) ser_names(b, s->value, " ");
            else if (s->args) ser_list(b, s->args);
            else sb_puts(b, s->value);
            sb_putc(b, ')');
        }
        break;
    case SK_PC:
        sb_putc(b, ':');
        sb_puts(b, s->name);
        if (!s->flag) break;
        sb_putc(b, '(');
        switch (s->pc) {
        case PC_NTH_CHILD: case PC_NTH_LAST_CHILD: case PC_NTH_OF_TYPE: case PC_NTH_LAST_OF_TYPE:
            ser_anb(b, s->a, s->b);
            if (s->args) { sb_puts(b, " of "); ser_list(b, s->args); }
            break;
        case PC_LANG:
            for (const char *p = s->value; *p; p += strlen(p) + 1) {
                if (p != s->value) sb_puts(b, ", ");
                if (*p == 's') ser_string(b, p + 1); else ser_ident(b, p + 1);
            }
            break;
        case PC_DIR: case PC_STATE: ser_ident(b, s->value); break;
        case PC_HEADING: sb_puts(b, s->value); break;
        default:
            if (s->args) ser_list(b, s->args);
            else ser_names(b, s->value, ", ");
        }
        sb_putc(b, ')');
        break;
    default: break;
    }
}
static void ser_compound(sbuf *b, const struct compound *c) {
    int i = 0;
    bool written = false;
    if (c->n && c->s[0].kind == SK_ANCHOR) return;
    if (i < c->n && c->s[i].kind == SK_NS) {
        const struct sel_qname *q = c->s[i++].q;
        if (!q->implicit) {
            if (q->ns == NSK_ANY) sb_putc(b, '*');
            else if (q->ns == NSK_URI) ser_ident(b, q->prefix);
            sb_putc(b, '|');
            if (!(i < c->n && c->s[i].kind == SK_TAG)) sb_putc(b, '*');
            written = true;
        }
    }
    if (i < c->n && c->s[i].kind == SK_TAG) {
        ser_ident(b, c->s[i].q ? c->s[i].q->raw : c->s[i].name);
        i++;
        written = true;
    }
    if (!written && i == c->n) sb_putc(b, '*'); /* only a universal selector */
    for (; i < c->n; i++) ser_simple(b, &c->s[i]);
}
static void ser_chain(sbuf *b, const struct compound *c) {
    if (c->left) {
        ser_chain(b, c->left);
        bool anchor = c->left->n == 1 && c->left->s[0].kind == SK_ANCHOR;
        if (anchor) { if (c->comb != ' ') { sb_putc(b, c->comb); sb_putc(b, ' '); } }
        else if (c->comb == ' ') sb_putc(b, ' ');
        else { sb_putc(b, ' '); sb_putc(b, c->comb); sb_putc(b, ' '); }
    }
    ser_compound(b, c);
}
/* CSSOM "serialize a group of selectors"; a forgiving list's invalid items as written */
static void ser_list(sbuf *b, const struct sellist *l) {
    int r = 0, k = 0;
    for (int i = 0; l && i <= l->n; i++) {
        for (; r < l->nraw && l->raw[r].at == i; r++) {
            if (k++) sb_puts(b, ", ");
            sb_puts(b, l->raw[r].text);
        }
        if (i < l->n) {
            if (k++) sb_puts(b, ", ");
            ser_chain(b, l->v[i]->right);
        }
    }
}

/* ---- the grammar */
enum {
    SPF_NO_PSEUDO_ELEMENT = 1, /* the arguments of :is() :where() :not() :has() :nth-child(of) */
    SPF_COMPOUND = 2,          /* :host() :host-context() ::slotted(): one compound selector */
    SPF_IN_HAS = 4,            /* inside :has(): no :has() */
    SPF_AFTER_PART = 8,        /* behind ::part(): no pseudo-class of tree structure */
    SPF_AFTER_ELEMENT = 16,    /* behind another pseudo-element: user action pseudo-classes only */
    SPF_AFTER_SLOTTED = 32,    /* behind ::slotted(): no pseudo-class */
    SPF_PSEUDO_CLASSES = 64,   /* a logical pseudo-class behind a pseudo-element: pseudo-classes only */
    SPF_RELATIVE = 128,        /* a relative selector, :has()'s */
};
#define SPF_BEHIND (SPF_AFTER_PART | SPF_AFTER_ELEMENT | SPF_AFTER_SLOTTED)
struct selp { struct pctx *pc; const char *p, *e; };
static void sp_peek(const struct selp *x, struct stok *t) { const char *p = x->p; st_next(&p, x->e, t); }
static void sp_take(struct selp *x, struct stok *t) { st_next(&x->p, x->e, t); }
static void sp_ws(struct selp *x) {
    for (;;) {
        struct stok t;
        const char *p = x->p;
        st_next(&p, x->e, &t);
        if (t.type != ST_WS) return;
        x->p = p;
    }
}
static bool sp_end(struct selp *x) { struct stok t; sp_ws(x); sp_peek(x, &t); return t.type == ST_EOF; }
/* a function's arguments: [x->p, *end), and x past its ')' */
static struct selp sp_arguments(struct selp *x, bool *bad) {
    struct selp a = {x->pc, x->p, NULL};
    a.e = st_scan(x->pc, x->p, x->e, ')', false, bad);
    x->p = a.e < x->e ? a.e + 1 : x->e;
    return a;
}
static int sp_clamp(double v) { return v >= INT_MAX ? INT_MAX : v <= INT_MIN ? INT_MIN : (int)v; }

enum { PCF_ACTION = 1, PCF_TREE = 2, PCF_UNSUPPORTED = 4 };
/* Selectors 4, HTML and Shadow DOM pseudo-classes without arguments. PC_NEVER:
   a state this user agent never enters (it has no history, autofill, WebVTT
   time or view transition), or, PCF_UNSUPPORTED, one it does not track yet. */
static const struct { const char *name; uint8_t pc, flags; } simple_pcs[] = {
    {"active", PC_ACTIVE, PCF_ACTION}, {"active-view-transition", PC_NEVER, 0}, {"any-link", PC_LINK, 0},
    {"autofill", PC_NEVER, 0}, {"buffering", PC_NEVER, PCF_UNSUPPORTED}, {"checked", PC_CHECKED, 0},
    {"current", PC_NEVER, 0}, {"default", PC_NEVER, PCF_UNSUPPORTED}, {"defined", PC_ALWAYS, PCF_UNSUPPORTED},
    {"disabled", PC_DISABLED, 0}, {"empty", PC_EMPTY, PCF_TREE}, {"enabled", PC_ENABLED, 0},
    {"first-child", PC_FIRST_CHILD, PCF_TREE}, {"first-of-type", PC_FIRST_OF_TYPE, PCF_TREE},
    {"focus", PC_FOCUS, PCF_ACTION}, {"focus-visible", PC_FOCUS_VISIBLE, PCF_ACTION},
    {"focus-within", PC_FOCUS_WITHIN, PCF_ACTION}, {"fullscreen", PC_FULLSCREEN, 0}, {"future", PC_NEVER, 0},
    {"heading", PC_HEADING, 0}, {"host", PC_HOST, PCF_TREE}, {"hover", PC_HOVER, PCF_ACTION},
    {"in-range", PC_IN_RANGE, 0}, {"indeterminate", PC_INDETERMINATE, 0}, {"invalid", PC_INVALID, 0},
    {"last-child", PC_LAST_CHILD, PCF_TREE}, {"last-of-type", PC_LAST_OF_TYPE, PCF_TREE}, {"link", PC_LINK, 0},
    {"local-link", PC_NEVER, PCF_UNSUPPORTED}, {"modal", PC_MODAL, 0}, {"muted", PC_NEVER, PCF_UNSUPPORTED},
    {"only-child", PC_ONLY_CHILD, PCF_TREE}, {"only-of-type", PC_ONLY_OF_TYPE, PCF_TREE}, {"open", PC_OPEN, 0},
    {"optional", PC_OPTIONAL, 0}, {"out-of-range", PC_OUT_OF_RANGE, 0}, {"past", PC_NEVER, 0},
    {"paused", PC_NEVER, PCF_UNSUPPORTED}, {"picture-in-picture", PC_NEVER, 0},
    {"placeholder-shown", PC_PLACEHOLDER_SHOWN, 0}, {"playing", PC_NEVER, PCF_UNSUPPORTED},
    {"popover-open", PC_NEVER, 0}, {"read-only", PC_READ_ONLY, 0}, {"read-write", PC_READ_WRITE, 0},
    {"required", PC_REQUIRED, 0}, {"root", PC_ROOT, PCF_TREE}, {"scope", PC_SCOPE, PCF_TREE},
    {"seeking", PC_NEVER, PCF_UNSUPPORTED}, {"stalled", PC_NEVER, PCF_UNSUPPORTED},
    {"target", PC_NEVER, PCF_UNSUPPORTED}, {"target-within", PC_NEVER, PCF_UNSUPPORTED},
    {"user-invalid", PC_NEVER, PCF_UNSUPPORTED}, {"user-valid", PC_NEVER, PCF_UNSUPPORTED}, {"valid", PC_VALID, 0},
    {"visited", PC_NEVER, 0}, {"volume-locked", PC_NEVER, PCF_UNSUPPORTED}, {"-webkit-autofill", PC_NEVER, 0},
};
/* Pseudo-elements without arguments. PEF_TREE: tree-abiding, so ::slotted()
   may precede it. PE_OTHER: parsed, never generated. */
enum { PEF_TREE = 1 };
static const struct { const char *name; uint8_t pe, flags; } pseudo_elements[] = {
    {"after", PE_AFTER, PEF_TREE}, {"backdrop", PE_BACKDROP, 0}, {"before", PE_BEFORE, PEF_TREE},
    {"checkmark", PE_OTHER, PEF_TREE}, {"column", PE_OTHER, 0}, {"cue", PE_OTHER, 0}, {"cue-region", PE_OTHER, 0},
    {"details-content", PE_OTHER, PEF_TREE}, {"file-selector-button", PE_OTHER, PEF_TREE},
    {"first-letter", PE_OTHER, 0}, {"first-line", PE_OTHER, 0}, {"grammar-error", PE_OTHER, 0},
    {"marker", PE_MARKER, PEF_TREE}, {"picker-icon", PE_OTHER, PEF_TREE}, {"placeholder", PE_PLACEHOLDER, PEF_TREE},
    {"scroll-marker", PE_OTHER, PEF_TREE}, {"scroll-marker-group", PE_OTHER, 0}, {"search-text", PE_OTHER, 0},
    {"selection", PE_OTHER, 0}, {"spelling-error", PE_OTHER, 0}, {"target-text", PE_OTHER, 0},
    {"view-transition", PE_OTHER, 0},
};
/* HTML: attribute selectors compare these attributes' values ASCII
   case-insensitively on HTML elements in HTML documents. */
static bool html_case_insensitive_attribute(const char *name) {
    static const char *const names[] = {"accept", "accept-charset", "align", "alink", "axis", "bgcolor", "charset",
        "checked", "clear", "codetype", "color", "compact", "declare", "defer", "dir", "direction", "disabled",
        "enctype", "face", "frame", "hreflang", "http-equiv", "lang", "language", "link", "media", "method",
        "multiple", "nohref", "noresize", "noshade", "nowrap", "readonly", "rel", "rev", "rules", "scope",
        "scrolling", "selected", "shape", "target", "text", "type", "valign", "valuetype", "vlink"};
    for (size_t i = 0; i < sizeof names / sizeof *names; i++) if (!strcmp(names[i], name)) return true;
    return false;
}

static struct sellist *sp_list(struct pctx *pc, const char *s, const char *e, unsigned flags, bool forgiving);
static struct sellist *sp_compound_list(struct pctx *pc, struct selp *args, unsigned flags, bool forgiving);

/* A namespace prefix written in a stylesheet: the namespace its @namespace
   declares (NULL: the empty one, no namespace); false when none declares it.
   A DOM query declares none. */
static bool sp_namespace(struct pctx *pc, const char *prefix, const char **uri) {
    const struct css_namespace *found = NULL;
    for (const struct css_namespace *n = pc->sh ? pc->sh->namespaces : NULL; n; n = n->next)
        if (prefix ? n->prefix && !strcmp(n->prefix, prefix) : !n->prefix) found = n; /* the last declaration wins */
    if (!found) return false;
    *uri = found->uri && *found->uri ? found->uri : NULL;
    return true;
}
static struct sel_qname *sp_qname(struct pctx *pc, uint8_t ns, const char *prefix, const char *raw) {
    struct sel_qname *q = ar_alloc(pc->a, sizeof *q);
    memset(q, 0, sizeof *q);
    q->ns = ns;
    q->prefix = prefix;
    q->raw = raw;
    if (ns == NSK_URI && !sp_namespace(pc, prefix, &q->uri)) return NULL;
    return q;
}
/* <ns-prefix>? at x: NSK_* (*prefix for NSK_URI), x past its '|', or -1: none.
   An ident or '*' is a prefix only when a '|' and a name or '*' follow it. */
static int sp_prefix(struct selp *x, struct pctx *pc, const char **prefix, bool attribute) {
    struct selp y = *x;
    struct stok t, bar, after;
    sp_take(&y, &t);
    if (t.type == ST_DELIM && t.delim == '|') {
        sp_peek(&y, &after);
        if (after.type == ST_IDENT || (!attribute && after.type == ST_DELIM && after.delim == '*')) { *x = y; return NSK_NONE; }
        return -1;
    }
    if (t.type != ST_IDENT && !(t.type == ST_DELIM && t.delim == '*')) return -1;
    sp_take(&y, &bar);
    if (bar.type != ST_DELIM || bar.delim != '|') return -1;
    sp_peek(&y, &after);
    if (!(after.type == ST_IDENT || (!attribute && after.type == ST_DELIM && after.delim == '*'))) return -1;
    *x = y;
    if (t.type == ST_DELIM) return NSK_ANY;
    *prefix = st_text(pc->a, t.vs, t.ve, false, false);
    return NSK_URI;
}
/* [ <wq-name> ] or [ <wq-name> <attr-matcher> <string|ident> <i|s>? ], s the '[' */
static bool sp_attribute(struct selp *x, struct simple *s) {
    struct pctx *pc = x->pc;
    bool bad = false;
    struct selp a = {pc, x->p, st_scan(pc, x->p, x->e, ']', false, &bad)};
    x->p = a.e < x->e ? a.e + 1 : x->e;
    if (bad) return false;
    struct stok t;
    const char *prefix = NULL;
    sp_ws(&a);
    int ns = sp_prefix(&a, pc, &prefix, true);
    sp_take(&a, &t);
    if (t.type != ST_IDENT) return false;
    s->kind = SK_ATTR;
    char *raw = st_text(pc->a, t.vs, t.ve, false, false);
    s->name = st_text(pc->a, t.vs, t.ve, false, true);
    if (ns == NSK_NONE) ns = -1; /* no namespace, as with no prefix */
    if (ns >= 0 || strcmp(raw, s->name)) {
        struct sel_qname *q = sp_qname(pc, ns >= 0 ? (uint8_t)ns : NSK_DEFAULT, prefix, raw);
        if (!q) return false;
        s->q = q;
    }
    s->html_list = html_case_insensitive_attribute(s->name);
    sp_ws(&a);
    sp_take(&a, &t);
    if (t.type == ST_EOF) { s->op = AO_EXISTS; return true; }
    if (t.type != ST_DELIM) return false;
    static const char matchers[] = "~|^$*";
    if (t.delim == '=') s->op = AO_EQ;
    else {
        const char *m = t.delim < 128 ? strchr(matchers, (int)t.delim) : NULL;
        struct stok eq;
        if (!m || !t.delim) return false;
        sp_take(&a, &eq);
        if (eq.type != ST_DELIM || eq.delim != '=') return false;
        s->op = (uint8_t)(AO_INCL + (m - matchers));
    }
    sp_ws(&a);
    sp_take(&a, &t);
    if (t.type == ST_IDENT) s->value = st_text(pc->a, t.vs, t.ve, false, false);
    else if (t.type == ST_STRING) s->value = st_text(pc->a, t.vs, t.ve, true, false);
    else return false;
    sp_ws(&a);
    sp_take(&a, &t);
    if (t.type == ST_IDENT) {
        if (st_is(t.vs, t.ve, "i")) s->flag = 'i', s->icase = true;
        else if (st_is(t.vs, t.ve, "s")) s->flag = 's';
        else return false;
        sp_ws(&a);
        sp_take(&a, &t);
    }
    return t.type == ST_EOF;
}

/* CSS Syntax 3 "parse <an+b>" over x up to an `of` (then *of) or its end. */
static bool sp_anb(struct selp *x, int *pa, int *pb, bool *of) {
    struct pctx *pc = x->pc;
    struct stok t;
    double a = 0, b = 0;
    const char *rest = NULL; /* what follows the n of an ident or a dimension's unit */
    *of = false;
    sp_ws(x);
    sp_take(x, &t);
    if (t.type == ST_NUMBER && t.integer) b = t.number;
    else if (t.type == ST_DIMENSION && t.integer) {
        const char *unit = st_text(pc->a, t.vs, t.ve, false, true);
        if (*unit != 'n') return false;
        a = t.number;
        rest = unit + 1;
    } else if (t.type == ST_IDENT || (t.type == ST_DELIM && t.delim == '+')) {
        bool plus = t.type == ST_DELIM;
        if (plus) { sp_take(x, &t); if (t.type != ST_IDENT) return false; }
        const char *name = st_text(pc->a, t.vs, t.ve, false, true);
        if (!plus && !strcmp(name, "odd")) a = 2, b = 1;
        else if (!plus && !strcmp(name, "even")) a = 2;
        else if (!plus && name[0] == '-' && name[1] == 'n') a = -1, rest = name + 2;
        else if (name[0] == 'n') a = 1, rest = name + 1;
        else return false;
    } else return false;
    if (rest) {
        if (!*rest) {
            struct selp y = *x;
            sp_ws(&y);
            sp_peek(&y, &t);
            if (t.type == ST_NUMBER && t.integer && t.sign) { sp_take(&y, &t); b = t.number; *x = y; }
            else if (t.type == ST_DELIM && (t.delim == '+' || t.delim == '-')) {
                double sign = t.delim == '-' ? -1 : 1;
                sp_take(&y, &t);
                sp_ws(&y);
                sp_take(&y, &t);
                if (t.type != ST_NUMBER || !t.integer || t.sign) return false;
                b = sign * t.number;
                *x = y;
            }
        } else if (rest[0] == '-' && !rest[1]) {
            sp_ws(x);
            sp_take(x, &t);
            if (t.type != ST_NUMBER || !t.integer || t.sign) return false;
            b = -t.number;
        } else if (rest[0] == '-' && st_digit((unsigned char)rest[1])) {
            for (const char *p = rest + 1; *p; p++) {
                if (!st_digit((unsigned char)*p)) return false;
                if (b < 1e10) b = b * 10 + (*p - '0');
            }
            b = -b;
        } else return false;
    }
    sp_ws(x);
    sp_peek(x, &t);
    if (t.type == ST_IDENT && st_is(t.vs, t.ve, "of")) { sp_take(x, &t); *of = true; }
    else if (t.type != ST_EOF) return false;
    *pa = sp_clamp(a);
    *pb = sp_clamp(b);
    return true;
}
/* one <ident> and nothing else: its value, or NULL */
static const char *sp_one_ident(struct selp *x, bool lowercase) {
    struct stok t;
    sp_ws(x);
    sp_take(x, &t);
    if (t.type != ST_IDENT || !sp_end(x)) return NULL;
    return st_text(x->pc->a, t.vs, t.ve, false, lowercase);
}
/* <ident>#: NUL-terminated names ending in an empty one, or NULL */
static char *sp_idents(struct selp *x, bool comma) {
    sbuf b = {0};
    struct stok t;
    char *out = NULL;
    for (bool first = true;; first = false) {
        sp_ws(x);
        sp_take(x, &t);
        if (t.type == ST_EOF && !first && !comma) break;
        if (t.type != ST_IDENT) goto done;
        char *name = st_text(x->pc->a, t.vs, t.ve, false, false);
        sb_put(&b, name, strlen(name) + 1);
        sp_ws(x);
        sp_peek(x, &t);
        if (t.type == ST_EOF) break;
        if (comma) { if (t.type != ST_COMMA) goto done; sp_take(x, &t); }
    }
    sb_putc(&b, 0);
    out = ar_alloc(x->pc->a, b.n);
    memcpy(out, b.p, b.n);
done:
    sb_free(&b);
    return out;
}
/* <ident>: the single-colon names that are pseudo-elements */
static bool sp_legacy_element(const struct stok *t) {
    return t->type == ST_IDENT && (st_is(t->vs, t->ve, "before") || st_is(t->vs, t->ve, "after") ||
                                   st_is(t->vs, t->ve, "first-line") || st_is(t->vs, t->ve, "first-letter"));
}
/* A pseudo-class whose name (or function) is t, x past it. flags: the context's
   restrictions, with the pseudo-element it follows. */
static bool sp_pseudo_class(struct selp *x, struct simple *s, const struct stok *t, uint32_t *spec, unsigned flags) {
    struct pctx *pc = x->pc;
    s->kind = SK_PC;
    if (t->type == ST_IDENT) {
        size_t k = 0;
        while (k < sizeof simple_pcs / sizeof *simple_pcs && !st_is(t->vs, t->ve, simple_pcs[k].name)) k++;
        if (k == sizeof simple_pcs / sizeof *simple_pcs) return false;
        unsigned f = simple_pcs[k].flags;
        if ((flags & (SPF_AFTER_ELEMENT | SPF_AFTER_SLOTTED)) && !(f & PCF_ACTION)) return false;
        if ((flags & SPF_AFTER_SLOTTED) || ((flags & SPF_AFTER_PART) && (f & PCF_TREE))) return false;
        if (f & PCF_UNSUPPORTED) pc->unsupported = true;
        s->pc = simple_pcs[k].pc;
        s->name = simple_pcs[k].name;
        if (s->pc == PC_HEADING) s->tag = 0x3FE; /* every level */
        *spec = spec_add(*spec, SPEC_B);
        return true;
    }
    bool bad = false;
    struct selp args = sp_arguments(x, &bad);
    if (bad) return false;
    s->flag = 1;
    const char *vs = t->vs, *ve = t->ve;
    unsigned behind = flags & (SPF_BEHIND | SPF_PSEUDO_CLASSES);
    if (flags & SPF_AFTER_SLOTTED) return false;
    static const char *const logical[] = {"is", "where", "not", "-webkit-any", "-moz-any"};
    for (size_t k = 0; k < sizeof logical / sizeof *logical; k++) {
        if (!st_is(vs, ve, logical[k])) continue;
        /* Behind a pseudo-element, its arguments are compounds of the pseudo-classes allowed there. */
        unsigned inner = (flags & (SPF_COMPOUND | SPF_IN_HAS | SPF_BEHIND | SPF_PSEUDO_CLASSES)) | SPF_NO_PSEUDO_ELEMENT;
        if (behind) inner |= SPF_COMPOUND | SPF_PSEUDO_CLASSES;
        s->args = sp_list(pc, args.p, args.e, inner, k < 2);
        if (!s->args) return false;
        s->pc = k == 2 ? PC_NOT : PC_IS;
        s->name = logical[k];
        if (k != 1) *spec = spec_add(*spec, sellist_max_spec(s->args));
        return true;
    }
    if (st_is(vs, ve, "has")) {
        if (flags & (SPF_IN_HAS | SPF_BEHIND)) return false;
        s->args = sp_list(pc, args.p, args.e, SPF_IN_HAS | SPF_NO_PSEUDO_ELEMENT | SPF_RELATIVE, false);
        if (!s->args) return false;
        s->pc = PC_HAS;
        s->name = "has";
        *spec = spec_add(*spec, sellist_max_spec(s->args));
        return true;
    }
    static const char *const nth[] = {"nth-child", "nth-last-child", "nth-of-type", "nth-last-of-type"};
    static const uint8_t nthpc[] = {PC_NTH_CHILD, PC_NTH_LAST_CHILD, PC_NTH_OF_TYPE, PC_NTH_LAST_OF_TYPE};
    for (int k = 0; k < 4; k++) {
        if (!st_is(vs, ve, nth[k])) continue;
        if (flags & SPF_BEHIND) return false;
        bool of;
        if (!sp_anb(&args, &s->a, &s->b, &of)) return false;
        if (of) {
            if (k >= 2) return false;
            s->args = sp_list(pc, args.p, args.e, (flags & (SPF_IN_HAS | SPF_COMPOUND)) | SPF_NO_PSEUDO_ELEMENT, false);
            if (!s->args) return false;
        }
        s->pc = nthpc[k];
        s->name = nth[k];
        *spec = spec_add(*spec, spec_add(SPEC_B, sellist_max_spec(s->args)));
        return true;
    }
    *spec = spec_add(*spec, SPEC_B);
    if (st_is(vs, ve, "lang")) {
        if (flags & SPF_AFTER_ELEMENT) return false;
        sbuf b = {0};
        bool ok = true;
        for (;;) {
            struct stok u;
            sp_ws(&args);
            sp_take(&args, &u);
            if (u.type != ST_IDENT && u.type != ST_STRING) { ok = false; break; }
            char *range = st_text(pc->a, u.vs, u.ve, u.type == ST_STRING, false);
            sb_putc(&b, u.type == ST_STRING ? 's' : 'i');
            sb_put(&b, range, strlen(range) + 1);
            sp_ws(&args);
            sp_take(&args, &u);
            if (u.type == ST_EOF) break;
            if (u.type != ST_COMMA) { ok = false; break; }
        }
        if (ok) {
            sb_putc(&b, 0);
            char *list = ar_alloc(pc->a, b.n);
            memcpy(list, b.p, b.n);
            s->value = list;
        }
        sb_free(&b);
        s->pc = PC_LANG;
        s->name = "lang";
        return ok;
    }
    if (st_is(vs, ve, "dir")) {
        if (flags & SPF_AFTER_ELEMENT) return false;
        s->pc = PC_DIR;
        s->name = "dir";
        return (s->value = sp_one_ident(&args, true)) != NULL;
    }
    if (st_is(vs, ve, "state")) {
        if (flags & SPF_AFTER_ELEMENT) return false;
        s->pc = PC_STATE;
        s->name = "state";
        return (s->value = sp_one_ident(&args, false)) != NULL;
    }
    if (st_is(vs, ve, "host") || st_is(vs, ve, "host-context")) {
        if (flags & SPF_BEHIND) return false;
        bool context = !st_is(vs, ve, "host");
        s->args = sp_compound_list(pc, &args, SPF_COMPOUND | SPF_NO_PSEUDO_ELEMENT, false);
        if (!s->args) return false;
        s->pc = context ? PC_HOST_CONTEXT : PC_HOST;
        s->name = context ? "host-context" : "host";
        *spec = spec_add(*spec, sellist_max_spec(s->args));
        return true;
    }
    if (st_is(vs, ve, "heading")) {
        /* <integer>#: the levels it lists; only 1 to 9 can match */
        sbuf b = {0};
        bool ok = true;
        for (;;) {
            struct stok u;
            sp_ws(&args);
            sp_take(&args, &u);
            if (u.type != ST_NUMBER || !u.integer) { ok = false; break; }
            int level = sp_clamp(u.number);
            if (b.n) sb_puts(&b, ", ");
            ser_int(&b, level);
            if (level >= 1 && level <= 9) s->tag |= (uint16_t)(1u << level);
            sp_ws(&args);
            sp_take(&args, &u);
            if (u.type == ST_EOF) break;
            if (u.type != ST_COMMA) { ok = false; break; }
        }
        if (ok) s->value = ar_strndup(pc->a, sb_cstr(&b), b.n);
        sb_free(&b);
        s->pc = PC_HEADING;
        s->name = "heading";
        return ok;
    }
    if (st_is(vs, ve, "active-view-transition-type")) {
        s->pc = PC_NEVER;
        s->name = "active-view-transition-type";
        return (s->value = sp_idents(&args, true)) != NULL;
    }
    return false;
}
/* A pseudo-element whose name (or function) is u, x past it; *behind: what the
   pseudo-classes and pseudo-elements after it may be. */
static bool sp_pseudo_element(struct selp *x, struct simple *s, const struct stok *u, struct selector *sel,
                              unsigned flags, unsigned *behind, uint8_t *last) {
    struct pctx *pc = x->pc;
    if (flags & SPF_NO_PSEUDO_ELEMENT) return false;
    const char *vs = u->vs, *ve = u->ve;
    uint8_t pe = PE_OTHER, pflags = 0;
    bool part = false, slotted = false;
    s->kind = SK_PE;
    if (u->type == ST_IDENT) {
        size_t k = 0;
        while (k < sizeof pseudo_elements / sizeof *pseudo_elements && !st_is(vs, ve, pseudo_elements[k].name)) k++;
        if (k < sizeof pseudo_elements / sizeof *pseudo_elements) {
            s->name = pseudo_elements[k].name;
            pe = pseudo_elements[k].pe;
            pflags = pseudo_elements[k].flags;
        } else {
            /* Selectors 4 compatibility: any ::-webkit-* name is valid and never matches. */
            char *name = st_text(pc->a, vs, ve, false, true);
            if (strncmp(name, "-webkit-", 8) || !name[8]) return false;
            s->name = name;
        }
    } else if (u->type == ST_FUNCTION) {
        bool bad = false;
        struct selp args = sp_arguments(x, &bad);
        if (bad) return false;
        s->flag = 1;
        if (st_is(vs, ve, "slotted")) {
            s->kind = SK_SLOTTED;
            s->args = sp_compound_list(pc, &args, SPF_COMPOUND | SPF_NO_PSEUDO_ELEMENT, false);
            if (!s->args) return false;
            slotted = true;
            pe = PE_SLOTTED;
            sel->spec = spec_add(sel->spec, sellist_max_spec(s->args));
        } else if (st_is(vs, ve, "part")) {
            s->name = "part";
            s->op = 1;
            part = true;
            if (!(s->value = sp_idents(&args, false))) return false;
        } else if (st_is(vs, ve, "highlight") || st_is(vs, ve, "picker")) {
            s->name = st_is(vs, ve, "picker") ? "picker" : "highlight";
            const char *name = sp_one_ident(&args, false);
            if (!name) return false;
            sbuf b = {0};
            ser_ident(&b, name);
            s->value = ar_strndup(pc->a, sb_cstr(&b), b.n);
            sb_free(&b);
        } else if (st_is(vs, ve, "cue")) {
            s->name = "cue";
            s->args = sp_list(pc, args.p, args.e, SPF_NO_PSEUDO_ELEMENT, false);
            if (!s->args) return false;
        } else {
            /* <pt-name-selector>: * or a name, then .classes; ::scroll-button(* | name) */
            static const char *const named[] = {"view-transition-group", "view-transition-image-pair",
                                                "view-transition-old", "view-transition-new", "scroll-button"};
            size_t k = 0;
            while (k < sizeof named / sizeof *named && !st_is(vs, ve, named[k])) k++;
            if (k == sizeof named / sizeof *named) return false;
            s->name = named[k];
            sbuf b = {0};
            struct stok t;
            sp_ws(&args);
            sp_take(&args, &t);
            bool ok = t.type == ST_IDENT || (t.type == ST_DELIM && t.delim == '*');
            if (ok && t.type == ST_IDENT) ser_ident(&b, st_text(pc->a, t.vs, t.ve, false, false));
            else if (ok) sb_putc(&b, '*');
            for (sp_peek(&args, &t); ok && k < 4 && t.type == ST_DELIM && t.delim == '.'; sp_peek(&args, &t)) {
                sp_take(&args, &t);
                sp_take(&args, &t);
                if (t.type != ST_IDENT) { ok = false; break; }
                sb_putc(&b, '.');
                ser_ident(&b, st_text(pc->a, t.vs, t.ve, false, false));
            }
            ok = ok && sp_end(&args);
            s->value = ar_strndup(pc->a, sb_cstr(&b), b.n);
            sb_free(&b);
            if (!ok) return false;
        }
    } else return false;
    /* what may follow which pseudo-element: anything but ::part() and ::slotted()
       follows ::part(); tree-abiding ones follow ::slotted(); ::marker follows
       ::before and ::after */
    if (*behind & SPF_AFTER_PART) { if (part || slotted) return false; }
    else if (*behind & SPF_AFTER_SLOTTED) { if (!(pflags & PEF_TREE)) return false; }
    else if (*behind) { if (!(*last == PE_BEFORE || *last == PE_AFTER) || !s->name || strcmp(s->name, "marker")) return false; }
    s->pc = pe;
    *behind = part ? SPF_AFTER_PART : slotted ? SPF_AFTER_SLOTTED : SPF_AFTER_ELEMENT;
    *last = pe;
    /* The cascade generates one pseudo-element of an element; a chain of them never
       matches. ::part() selects an element in a shadow tree (sel->part): a pseudo-element
       after it is that element's. */
    if (part) sel->part = true;
    else sel->pseudo = sel->pseudo ? PE_OTHER : pe;
    if (sel->pseudo == PE_OTHER) pc->unsupported = true;
    sel->spec = spec_add(sel->spec, SPEC_C);
    return true;
}
/* <type-selector> at a compound's start into *s: 1 parsed, 0 none, -1 invalid */
static int sp_type(struct selp *x, struct simple *tmp, int *n, bool *universal, uint32_t *spec) {
    struct pctx *pc = x->pc;
    const char *prefix = NULL;
    int ns = sp_prefix(x, pc, &prefix, false);
    struct stok t;
    sp_peek(x, &t);
    bool star = t.type == ST_DELIM && t.delim == '*';
    if (!star && t.type != ST_IDENT) return ns < 0 ? 0 : -1;
    sp_take(x, &t);
    const char *uri = NULL;
    bool implicit = ns < 0 && sp_namespace(pc, NULL, &uri);
    /* Without a default namespace, *| is what nothing written means. */
    if (ns == NSK_ANY && !sp_namespace(pc, NULL, &uri)) ns = -1;
    if (ns >= 0 || implicit) {
        struct sel_qname *q = sp_qname(pc, implicit ? NSK_URI : (uint8_t)ns, prefix, NULL);
        if (!q) return -1;
        if (implicit) { q->uri = uri; q->implicit = true; }
        struct simple *s = &tmp[(*n)++];
        memset(s, 0, sizeof *s);
        s->kind = SK_NS;
        s->q = q;
    }
    if (star) { *universal = true; return 1; }
    struct simple *s = &tmp[(*n)++];
    memset(s, 0, sizeof *s);
    s->kind = SK_TAG;
    s->name = st_text(pc->a, t.vs, t.ve, false, true);
    char *raw = st_text(pc->a, t.vs, t.ve, false, false);
    if (strcmp(raw, s->name)) {
        struct sel_qname *q = ar_alloc(pc->a, sizeof *q);
        memset(q, 0, sizeof *q);
        q->raw = raw;
        s->q = q;
    }
    s->tag = (uint16_t)tag_lookup(s->name, strlen(s->name));
    *spec = spec_add(*spec, SPEC_C);
    return 1;
}
/* one <compound-selector> with its pseudo-elements at x, or NULL */
static struct compound *sp_compound(struct selp *x, struct selector *sel, unsigned flags) {
    struct pctx *pc = x->pc;
    struct simple local[4], *tmp = local;
    size_t capacity = sizeof local / sizeof *local;
    int n = 0;
    bool universal = false;
    unsigned behind = 0; /* SPF_AFTER_*: the pseudo-element the rest belongs to */
    uint8_t last = PE_NONE;
    if (!(flags & SPF_PSEUDO_CLASSES)) {
        if (capacity - (size_t)n < 2) tmp = selector_list_grow(pc, tmp, (size_t)n, &capacity, sizeof *tmp);
        if (sp_type(x, tmp, &n, &universal, &sel->spec) < 0) return NULL;
    }
    for (;;) {
        struct stok t;
        sp_peek(x, &t);
        if ((size_t)n == capacity) tmp = selector_list_grow(pc, tmp, (size_t)n, &capacity, sizeof *tmp);
        struct simple *s = &tmp[n];
        memset(s, 0, sizeof *s);
        if (t.type == ST_HASH || t.type == ST_LBRACKET || (t.type == ST_DELIM && t.delim == '.')) {
            if (behind || (flags & SPF_PSEUDO_CLASSES)) return NULL;
            sp_take(x, &t);
            if (t.type == ST_HASH) {
                if (!t.id) return NULL;
                s->kind = SK_ID;
                s->name = st_text(pc->a, t.vs, t.ve, false, false);
                sel->spec = spec_add(sel->spec, SPEC_A);
            } else if (t.type == ST_LBRACKET) {
                if (!sp_attribute(x, s)) return NULL;
                sel->spec = spec_add(sel->spec, SPEC_B);
            } else {
                sp_take(x, &t);
                if (t.type != ST_IDENT) return NULL;
                s->kind = SK_CLASS;
                s->name = st_text(pc->a, t.vs, t.ve, false, false);
                sel->spec = spec_add(sel->spec, SPEC_B);
            }
            n++;
            continue;
        }
        if (t.type != ST_COLON) break;
        sp_take(x, &t);
        struct stok u;
        sp_take(x, &u);
        bool element = u.type == ST_COLON;
        if (element) sp_take(x, &u);
        else element = sp_legacy_element(&u);
        if (element) {
            if (!sp_pseudo_element(x, s, &u, sel, flags, &behind, &last)) return NULL;
        } else {
            if (u.type != ST_IDENT && u.type != ST_FUNCTION) return NULL;
            if (!sp_pseudo_class(x, s, &u, &sel->spec, flags | behind)) return NULL;
        }
        n++;
    }
    if (!n && !universal) return NULL;
    struct compound *c = ar_alloc(pc->a, sizeof *c + (size_t)n * sizeof *tmp);
    c->left = NULL;
    c->comb = 0;
    c->n = n;
    c->universal = universal;
    memcpy(c->s, tmp, (size_t)n * sizeof *tmp);
    return c;
}
/* one <complex-selector> (relative: <relative-selector>) filling x, or NULL */
static struct selector *sp_complex(struct selp *x, unsigned flags) {
    struct pctx *pc = x->pc;
    struct selector *sel = ar_alloc(pc->a, sizeof *sel);
    memset(sel, 0, sizeof *sel);
    struct compound *cur = NULL;
    char comb = ' ';
    struct stok t;
    sp_ws(x);
    if (flags & SPF_RELATIVE) {
        /* an internal, zero-specificity anchor; not :scope, which still means the query's root */
        sp_peek(x, &t);
        if (t.type == ST_DELIM && (t.delim == '>' || t.delim == '+' || t.delim == '~')) {
            comb = (char)t.delim;
            sp_take(x, &t);
            sp_ws(x);
        }
        cur = ar_alloc(pc->a, sizeof *cur + sizeof *cur->s);
        memset(cur, 0, sizeof *cur + sizeof *cur->s);
        cur->n = 1;
        cur->s[0].kind = SK_ANCHOR;
    }
    for (;;) {
        struct compound *c = sp_compound(x, sel, flags & ~SPF_RELATIVE);
        if (!c) return NULL;
        c->left = cur;
        c->comb = cur ? comb : 0;
        cur = c;
        sp_peek(x, &t);
        bool ws = t.type == ST_WS;
        if (ws) { sp_take(x, &t); sp_peek(x, &t); }
        if (t.type == ST_EOF) break;
        if (t.type == ST_DELIM && (t.delim == '>' || t.delim == '+' || t.delim == '~')) {
            comb = (char)t.delim;
            sp_take(x, &t);
            sp_ws(x);
        } else if (ws) comb = ' ';
        else return NULL;
        /* a pseudo-element ends its selector; a compound context has no combinator */
        if (sel->pseudo || sel->part || (flags & SPF_COMPOUND)) return NULL;
    }
    sel->right = cur;
    return sel;
}
/* A selector list over [s, e): non-forgiving, the first invalid item fails it;
   forgiving, an invalid item is kept as its text, unless it holds a token
   <any-value> excludes. */
static struct sellist *sp_list(struct pctx *pc, const char *s, const char *e, unsigned flags, bool forgiving) {
    struct selector *local[8], **v = local;
    struct sel_raw rlocal[4], *raw = rlocal;
    size_t capacity = sizeof local / sizeof *local, rcapacity = sizeof rlocal / sizeof *rlocal;
    int n = 0, nraw = 0;
    for (const char *p = s;;) {
        bool bad = false;
        const char *stop = st_scan(pc, p, e, 0, true, &bad);
        struct selp x = {pc, p, stop};
        struct selector *sel = bad ? NULL : sp_complex(&x, flags);
        if (sel && !sp_end(&x)) sel = NULL;
        if (sel) {
            if ((size_t)n == capacity) v = selector_list_grow(pc, v, (size_t)n, &capacity, sizeof *v);
            v[n++] = sel;
        } else {
            if (!forgiving || bad) return NULL;
            const char *ts = skip_ws(p, stop), *te = stop;
            trim_r(ts, &te);
            if ((size_t)nraw == rcapacity) raw = selector_list_grow(pc, raw, (size_t)nraw, &rcapacity, sizeof *raw);
            raw[nraw].at = n;
            raw[nraw++].text = ar_strndup(pc->a, ts, (size_t)(te - ts));
            pc->forgiven = true;
        }
        if (stop >= e) break;
        p = stop + 1;
    }
    struct sellist *l = ar_alloc(pc->a, sizeof *l + (size_t)n * sizeof *v);
    l->n = n;
    l->nraw = nraw;
    l->raw = NULL;
    if (nraw) {
        struct sel_raw *kept = ar_alloc(pc->a, (size_t)nraw * sizeof *kept);
        memcpy(kept, raw, (size_t)nraw * sizeof *kept);
        l->raw = kept;
    }
    memcpy(l->v, v, (size_t)n * sizeof *v);
    return l;
}
/* a <compound-selector> argument (:host(), :host-context(), ::slotted()) as a one-item list */
static struct sellist *sp_compound_list(struct pctx *pc, struct selp *args, unsigned flags, bool forgiving) {
    struct sellist *l = sp_list(pc, args->p, args->e, flags, forgiving);
    return l && l->n == 1 && !l->nraw ? l : NULL;
}

/* A style rule's <selector-list>, a DOM query's or a selectorText's: the whole
   text, else NULL. pc->sh supplies the declared namespaces (none: a query). */
static struct sellist *selector_list_parse(struct pctx *pc, const char *s, const char *e) {
    pc->forgiven = pc->unsupported = false;
    return sp_list(pc, s, e, 0, false);
}
/* @supports selector(<complex-selector>): it parses, without a forgiven item,
   and this engine matches all of it. */
static bool selector_supported(arena_t *a, const char *s, const char *e) {
    struct pctx pc = {.a = a};
    bool bad = false;
    if (st_scan(&pc, s, e, 0, true, &bad) != e || bad) return false;
    struct selp x = {&pc, s, e};
    struct selector *sel = sp_complex(&x, 0);
    return sel && sp_end(&x) && !pc.forgiven && !pc.unsupported;
}
/* CSS Namespaces: an @namespace prelude [s, e), `<namespace-prefix>? [<string> | <url>]`:
   its prefix (NULL: the default namespace) and URI */
static bool namespace_prelude(arena_t *a, const char *s, const char *e, const char **prefix, const char **uri) {
    struct pctx pc = {.a = a};
    struct selp x = {&pc, s, e};
    struct stok t;
    *prefix = *uri = NULL;
    sp_ws(&x);
    sp_take(&x, &t);
    if (t.type == ST_IDENT) {
        *prefix = st_text(a, t.vs, t.ve, false, false);
        sp_ws(&x);
        sp_take(&x, &t);
    }
    if (t.type == ST_STRING) *uri = st_text(a, t.vs, t.ve, true, false);
    else if (t.type == ST_URL) {
        const char *vs = t.ve + 1, *ve = t.e;
        if (ve > vs && ve[-1] == ')') ve--;
        vs = skip_ws(vs, ve);
        trim_r(vs, &ve);
        *uri = st_text(a, vs, ve, false, false);
    } else if (t.type == ST_FUNCTION && st_is(t.vs, t.ve, "url")) {
        struct stok u;
        sp_ws(&x);
        sp_take(&x, &u);
        if (u.type != ST_STRING) return false;
        *uri = st_text(a, u.vs, u.ve, true, false);
        sp_ws(&x);
        sp_take(&x, &u);
        if (u.type != ST_RPAREN) return false;
    } else return false;
    return sp_end(&x);
}
bool css_namespace_rule(arena_t *a, const char *text, size_t length, const char **prefix, const char **uri) {
    const char *s = text, *e = text + length;
    s = skip_ws(s, e);
    if (e - s < 10 || !strn_ieq(s, "@namespace", 10)) return false;
    s += 10;
    while (e > s && (is_space((unsigned char)e[-1]) || e[-1] == ';')) e--;
    return namespace_prelude(a, s, e, prefix, uri);
}
/* CSSOM "serialize a group of selectors" of a parsed style rule */
void css_rule_selector_serialize(const struct css_rule_info *info, sbuf *out) {
    const struct rule *r = info ? info->native_rule : NULL;
    if (r && r->sel) ser_list(out, r->sel);
}
/* selectorText's setter: the selectors of [selector, +length) parsed in the
   namespaces of sheet (NULL: none) and serialized; false when they do not parse */
bool css_selectors_serialize(const sheet_t *sheet, const char *selector, size_t length, sbuf *out) {
    arena_t a = {0};
    jmp_buf trap;
    a.trap = &trap;
    if (setjmp(trap)) { ar_free(&a); return false; } /* the parse's scratch ran out */
    struct pctx pc = {.a = &a, .sh = (sheet_t *)sheet};
    struct sellist *l = selector_list_parse(&pc, selector, selector + length);
    if (l) ser_list(out, l);
    ar_free(&a);
    return l != NULL;
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
    /* their left compounds match the slot's or the host's ancestors */
    if (sel->pseudo == PE_SLOTTED || sel->part) return;
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
                else if (pass == 1 && x->kind == SK_TAG) /* a name written with uppercase matches others by name */
                    h = x->tag && !x->q ? ancestor_tag_hash(x->tag) : ancestor_feature_hash('<', x->name, strlen(x->name));
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

/* An+B: whether idx is A*n+B for some n >= 0 (idx and B as written, A clamped) */
static bool nth(int a, int b, long long idx) {
    long long d = idx - b;
    if (!a) return !d;
    return d / a >= 0 && d % a == 0;
}

static bool form_control(const node_t *e) {
    return !e->foreign && (e->tag == T_input || e->tag == T_button || e->tag == T_select || e->tag == T_textarea ||
           e->tag == T_option || e->tag == T_optgroup || e->tag == T_fieldset);
}

static bool read_write(const node_t *e) {
    if (e->tag == T_input || e->tag == T_textarea) return web_control_read_write(e);
    const node_t *top = e;
    for (const node_t *n = e; n; n = n->parent) {
        top = n;
        const char *editable = node_attr(n, "contenteditable");
        if (!editable) continue;
        if (!*editable || str_ieq(editable, "true") || str_ieq(editable, "plaintext-only")) return true;
        if (str_ieq(editable, "false")) return false;
    }
    /* In design mode the document element is an editing host (HTML, "editing host"). */
    return e->owner && e->owner->design_mode && top->type == N_DOC && top == e->owner->root;
}

static bool icase_eq(const char *a, const char *b, bool icase) { return icase ? str_ieq(a, b) : !strcmp(a, b); }
static bool icase_eqn(const char *a, const char *b, size_t n, bool icase) { return icase ? strn_ieq(a, b, n) : !strncmp(a, b, n); }

/* an attribute's local name (attributes the parser made have none apart) */
static const char *attr_local(const struct attr *a) { return a->local ? a->local : a->raw ? a->raw : a->name; }
static bool attr_value_match(const struct simple *x, const char *v, bool ic) {
    if (x->op == AO_EXISTS) return true;
    const char *w = x->value;
    size_t vl = strlen(v), wl = strlen(w);
    switch (x->op) {
    case AO_EQ: return icase_eq(v, w, ic);
    case AO_INCL:
        if (!wl) return false;
        for (const char *p = v; *p;) {
            while (is_space((unsigned char)*p)) p++;
            const char *st = p;
            while (*p && !is_space((unsigned char)*p)) p++;
            if ((size_t)(p - st) == wl && icase_eqn(st, w, wl, ic)) return true;
        }
        return false;
    case AO_DASH: return vl >= wl && icase_eqn(v, w, wl, ic) && (v[wl] == 0 || v[wl] == '-');
    case AO_PREFIX: return wl && vl >= wl && icase_eqn(v, w, wl, ic);
    case AO_SUFFIX: return wl && vl >= wl && icase_eqn(v + vl - wl, w, wl, ic);
    case AO_SUBSTR:
        if (!wl) return false;
        for (size_t i = 0; i + wl <= vl; i++) if (icase_eqn(v + i, w, wl, ic)) return true;
        return false;
    }
    return false;
}
/* Selectors 4 attribute selectors. The name is the selector's local name, ASCII
   lowercased on an HTML element in an HTML document; its namespace is the one
   written (none: no namespace). A value compares ASCII case-insensitively with
   the i flag, or, with no flag, for HTML's listed attributes of an HTML element
   in an HTML document. */
static bool match_attr(const struct simple *x, const node_t *e) {
    bool html = doc_html_names(e);
    const char *name = html || !x->q || !x->q->raw ? x->name : x->q->raw;
    uint8_t ns = x->q ? x->q->ns : NSK_DEFAULT;
    for (int i = 0; i < e->nattrs; i++) {
        const struct attr *a = &e->attrs[i];
        if (strcmp(attr_local(a), name)) {
            /* HTML: a non-HTML element's attribute in an HTML document matches a
               lowercased selector name ASCII case-insensitively (viewBox) */
            if (html || !e->owner || e->owner->xml || !str_ieq(attr_local(a), name)) continue;
        }
        if (ns == NSK_DEFAULT ? a->namespace_uri != NULL :
            ns == NSK_URI ? (x->q->uri ? !a->namespace_uri || strcmp(a->namespace_uri, x->q->uri) : a->namespace_uri != NULL) :
            false) continue;
        bool ic = x->flag == 'i' || (!x->flag && x->html_list && html && !a->namespace_uri);
        if (attr_value_match(x, a->value ? a->value : "", ic)) return true;
    }
    return false;
}
/* the value of e's attribute local in namespace ns (NULL: none) */
static const char *attr_value_ns(const node_t *e, const char *ns, const char *local) {
    for (int i = 0; i < e->nattrs; i++) {
        const struct attr *a = &e->attrs[i];
        if (!strcmp(attr_local(a), local) && (ns ? a->namespace_uri && !strcmp(a->namespace_uri, ns) : !a->namespace_uri))
            return a->value ? a->value : "";
    }
    return NULL;
}
/* RFC 4647 extended filtering of a language tag by a range, ASCII case-insensitively */
static bool lang_extended_match(const char *tag, const char *range) {
    const char *t = tag, *r = range;
    size_t tl = strcspn(t, "-"), rl = strcspn(r, "-");
    if (!*range || (!(rl == 1 && *r == '*') && (tl != rl || !strn_ieq(t, r, rl)))) return false;
    if (!tl && !(rl == 1 && *r == '*')) return false;
    t += tl; r += rl;
    while (*r) {
        r++;
        rl = strcspn(r, "-");
        if (rl == 1 && *r == '*') { r += rl; continue; }
        for (;;) {
            if (!*t) return false;
            t++;
            tl = strcspn(t, "-");
            if (tl == rl && strn_ieq(t, r, rl)) { t += tl; break; }
            if (tl == 1) return false; /* a singleton ends the subtags a range may skip */
            t += tl;
        }
        r += rl;
    }
    return true;
}
/* HTML "the language" of an element: xml:lang, then lang on HTML elements and
   other content languages, inherited; NULL: unknown */
static const char *element_language(node_t *e) {
    for (node_t *p = e; p; p = doc_shadow_parent(p)) {
        if (p->type != N_ELEM) continue;
        const char *l = attr_value_ns(p, WEB_NS_XML, "lang");
        if (!l) l = attr_value_ns(p, NULL, "lang");
        if (l) return l;
    }
    return NULL;
}
/* HTML "get an element's computed heading level", 0 for no heading */
static int heading_level(node_t *e) {
    if (e->foreign || e->tag < T_h1 || e->tag > T_h6) return 0;
    int level = e->tag - T_h1 + 1;
    for (node_t *p = e; p && level < 9;) {
        if (p->type == N_ELEM && !p->foreign) {
            const char *offset = attr_value_ns(p, NULL, "headingoffset");
            if (offset) {
                const char *s = offset;
                while (*s == ' ' || *s == '\t' || *s == '\n' || *s == '\f' || *s == '\r') s++;
                if (*s == '+') s++;
                if (st_digit((unsigned char)*s)) {
                    long long v = 0;
                    while (st_digit((unsigned char)*s) && v < 100) v = v * 10 + (*s++ - '0');
                    level = v >= 9 ? 9 : level + (int)v > 9 ? 9 : level + (int)v;
                }
            }
            if (attr_value_ns(p, NULL, "headingreset")) break;
        }
        node_t *parent = p->parent;
        p = parent && parent->type == N_FRAGMENT && parent->shadow_host ? parent->shadow_host : parent;
        if (p && p->type != N_ELEM) break;
    }
    return level > 9 ? 9 : level;
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
/* ---- HTML directionality (:dir()) */
#include "unicode_bidi_data.h"
/* UAX #9 bidirectional type, as far as directionality reads it: 1 L, 2 R or AL, 0 any other */
static int bidi_strong(uint32_t cp) {
    size_t lo = 0, hi = sizeof unicode_bidi_strong / sizeof *unicode_bidi_strong;
    while (lo < hi) {
        size_t mid = lo + (hi - lo) / 2;
        if (cp < unicode_bidi_strong[mid].first) hi = mid;
        else if (cp > unicode_bidi_strong[mid].last) lo = mid + 1;
        else return unicode_bidi_strong[mid].type;
    }
    return 0;
}
/* the first strong type in UTF-8 text, 0 for none */
static int text_strong(const char *s, size_t n) {
    for (const unsigned char *p = (const unsigned char *)s, *e = p + n; p < e;) {
        uint32_t cp = *p++;
        if (cp >= 0x80) {
            unsigned more = cp >= 0xF0 ? 3 : cp >= 0xE0 ? 2 : cp >= 0xC0 ? 1 : 0;
            cp &= 0x3Fu >> more;
            for (; more && p < e && (*p & 0xC0) == 0x80; more--) cp = cp << 6 | (*p++ & 0x3Fu);
            if (more) continue;
        }
        int strong = bidi_strong(cp);
        if (strong) return strong;
    }
    return 0;
}
/* the dir attribute's state: 'l', 'r', 'a', or 0 (Undefined) */
static int dir_state(const node_t *e) {
    const char *v = !e->foreign ? attr_value_ns(e, NULL, "dir") : NULL;
    return !v ? 0 : str_ieq(v, "ltr") ? 'l' : str_ieq(v, "rtl") ? 'r' : str_ieq(v, "auto") ? 'a' : 0;
}
static int element_direction(node_t *e);
static bool shadow_slot(const node_t *e) {
    if (e->foreign || e->tag != T_slot) return false;
    node_t *root = doc_node_root((node_t *)e, false);
    return root && root->shadow_host;
}
/* HTML "contained text auto directionality": 1 ltr, 2 rtl, 0 null */
static int contained_text_direction(node_t *element, bool exclude_root) {
    if (exclude_root && !element->foreign && (element->tag == T_bdi || element->tag == T_script ||
        element->tag == T_style || element->tag == T_textarea || dir_state(element))) return 0;
    for (node_t *n = element->first; n;) {
        bool skip = false;
        if (n->type == N_ELEM) {
            skip = !n->foreign && (n->tag == T_bdi || n->tag == T_script || n->tag == T_style ||
                                   n->tag == T_textarea || dir_state(n));
            if (!skip && shadow_slot(n)) return element_direction(doc_node_root(n, false)->shadow_host);
        } else if (n->type == N_TEXT) {
            int strong = text_strong(n->text, n->textlen);
            if (strong) return strong;
        }
        if (!skip && n->first) { n = n->first; continue; }
        while (n != element && !n->next) n = n->parent;
        if (n == element) break;
        n = n->next;
    }
    return 0;
}
/* HTML "auto directionality" */
static int auto_direction(node_t *e) {
    if (!e->foreign && (e->tag == T_textarea || (e->tag == T_input && web_input_type(e) <= WEB_INPUT_HIDDEN) ||
        (e->tag == T_input && (web_input_type(e) == WEB_INPUT_SUBMIT || web_input_type(e) == WEB_INPUT_RESET ||
                               web_input_type(e) == WEB_INPUT_BUTTON)))) {
        const char *value = web_input_edit_text(e);
        if (text_strong(value, strlen(value)) == 2) return 2;
        return *value ? 1 : 0;
    }
    if (shadow_slot(e) && e->slot_assigned_first) {
        for (node_t *c = e->slot_assigned_first; c; c = c->assigned_next) {
            int direction = c->type == N_TEXT ? text_strong(c->text, c->textlen) :
                            c->type == N_ELEM ? contained_text_direction(c, true) : 0;
            if (direction) return direction;
        }
        return 0;
    }
    return contained_text_direction(e, false);
}
/* HTML "the directionality" of an element: 1 ltr, 2 rtl */
static int element_direction(node_t *e) {
    for (int depth = 0; e && e->type == N_ELEM && depth < 100000; depth++) {
        switch (dir_state(e)) {
        case 'l': return 1;
        case 'r': return 2;
        case 'a': { int direction = auto_direction(e); return direction ? direction : 1; }
        }
        if (!e->foreign && e->tag == T_bdi) { int direction = auto_direction(e); return direction ? direction : 1; }
        if (!e->foreign && e->tag == T_input && web_input_type(e) == WEB_INPUT_TEL) return 1;
        node_t *parent = e->parent;
        e = parent && parent->type == N_FRAGMENT && parent->shadow_host ? parent->shadow_host : parent;
    }
    return 1;
}
/* two elements of one type: one namespace and local name */
static bool same_type(const node_t *a, const node_t *b) {
    if (a->tag && a->tag == b->tag && a->namespace_id == b->namespace_id && a->namespace_id != NS_OTHER) return true;
    return a->namespace_id == b->namespace_id && !strcmp(a->local_name ? a->local_name : a->name, b->local_name ? b->local_name : b->name) &&
           (a->namespace_id != NS_OTHER || !strcmp(doc_element_namespace(a), doc_element_namespace(b)));
}
static bool match_pc(const struct simple *x, node_t *e, const struct selector_match *context) {
    node_t *scope = context->shadow;
    switch (x->pc) {
    case PC_FIRST_CHILD: return !prev_elem(e);
    case PC_LAST_CHILD: return !next_elem(e);
    case PC_ONLY_CHILD: return !prev_elem(e) && !next_elem(e);
    case PC_NTH_CHILD:
        if (!x->args) return nth(x->a, x->b, e->elem_index);
        /* fall through: :nth-child(An+B of S) counts the siblings S matches */
    case PC_NTH_LAST_CHILD: {
        if (x->args && !match_list_bound(x->args, e, context)) return false;
        long long k = 1;
        bool before = x->pc == PC_NTH_CHILD;
        for (node_t *s = before ? prev_elem(e) : next_elem(e); s; s = before ? prev_elem(s) : next_elem(s))
            if (!x->args || match_list_bound(x->args, s, context)) k++;
        return nth(x->a, x->b, k);
    }
    case PC_FIRST_OF_TYPE: case PC_LAST_OF_TYPE: case PC_ONLY_OF_TYPE: case PC_NTH_OF_TYPE:
    case PC_NTH_LAST_OF_TYPE: {
        long long before = 1, after = 1;
        if (x->pc != PC_LAST_OF_TYPE && x->pc != PC_NTH_LAST_OF_TYPE)
            for (node_t *s = prev_elem(e); s; s = prev_elem(s))
                if (same_type(s, e)) before++;
        if (x->pc != PC_FIRST_OF_TYPE && x->pc != PC_NTH_OF_TYPE)
            for (node_t *s = next_elem(e); s; s = next_elem(s))
                if (same_type(s, e)) after++;
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
    case PC_HOST_CONTEXT: {
        /* the host, if it or a shadow-including ancestor matches the compound */
        if (!scope || scope->shadow_host != e) return false;
        struct selector_match host = *context; host.shadow = NULL;
        for (node_t *p = e; p && p->type == N_ELEM;) {
            if (match_list_bound(x->args, p, &host)) return true;
            node_t *parent = p->parent;
            p = parent && parent->type == N_FRAGMENT && parent->shadow_host ? parent->shadow_host : parent;
        }
        return false;
    }
    case PC_DIR: return element_direction(e) == (str_ieq(x->value, "rtl") ? 2 : str_ieq(x->value, "ltr") ? 1 : 0);
    case PC_HEADING: { int level = heading_level(e); return level && (x->tag >> level & 1); }
    case PC_STATE: return web_custom_state_has(e, x->value);
    case PC_FOCUS_VISIBLE: return e->owner && e->owner->focus == e && e->owner->focus_visible;
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
    case PC_LANG: {
        const char *language = element_language(e);
        if (!language || !*language) return false;
        for (const char *range = x->value; *range; range += strlen(range) + 1)
            if (lang_extended_match(language, range + 1)) return true;
        return false;
    }
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

/* The simples [from, to) of c against e: all of them, or one side of a ::part(). */
static bool match_compound_range(const struct compound *c, int from, int to, node_t *e,
                                 const struct selector_match *context) {
    node_t *scope = context->shadow;
    if (scope && e == scope->shadow_host) {
        /* a featureless host: only :host, :host() and :host-context() select it */
        bool host = false;
        if (c->universal && !from) return false;
        for (int i = from; i < to; i++) {
            const struct simple *x = &c->s[i];
            if (x->kind == SK_PE || (x->kind == SK_NS && x->q->implicit)) continue;
            if (x->kind != SK_PC || (x->pc != PC_HOST && x->pc != PC_HOST_CONTEXT)) return false;
            host = true;
        }
        if (!host) return false;
    }
    for (int i = from; i < to; i++) {
        const struct simple *x = &c->s[i];
        switch (x->kind) {
        case SK_ANCHOR:
            if (e != context->anchor) return false;
            break;
        case SK_NS: {
            /* the namespace a prefix or the default names (NULL: no namespace) */
            const struct sel_qname *q = x->q;
            if (q->ns == NSK_ANY || (q->implicit && scope && e == scope->shadow_host)) break;
            const char *ns = doc_element_namespace(e);
            if (q->ns == NSK_NONE || !q->uri ? ns != NULL : !ns || strcmp(ns, q->uri)) return false;
            break;
        }
        case SK_TAG:
            /* HTML: an HTML element in an HTML document compares the lowercased
               name; any other element its local name as written */
            if (doc_html_names(e)) { if (x->tag ? e->tag != x->tag : strcmp(e->local_name ? e->local_name : e->name, x->name)) return false; }
            else if (strcmp(e->local_name ? e->local_name : e->name, x->q && x->q->raw ? x->q->raw : x->name)) return false;
            break;
        case SK_PE: break; /* its selector's pseudo is the generated box */
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
static bool match_compound(const struct compound *c, node_t *e, const struct selector_match *context) {
    return match_compound_range(c, 0, c->n, e, context);
}

/* CSS Shadow Parts. Whether name is a part name of e on host: e's own `part`
   tokens when e is in host's shadow tree, else what the exportparts of the
   inner host between them forwards to that name. */
static bool token_listed(const char *list, const char *token, size_t length);
static bool part_forwarded(node_t *e, node_t *inner, const char *name, size_t length);
static bool part_exposed(node_t *e, node_t *host, const char *name, size_t length) {
    for (node_t *inner = doc_node_root(e, false)->shadow_host; inner;) {
        node_t *outer = doc_node_root(inner, false)->shadow_host;
        if (inner == host) return token_listed(node_attr(e, "part"), name, length);
        if (outer == host) return part_forwarded(e, inner, name, length);
        inner = outer;
    }
    return false;
}
/* "Parse a part mapping list": comma-separated `inner` or `inner : outer`
   items, an item that is neither skipped. Whether one maps to name a part name
   e has on inner. */
static bool part_forwarded(node_t *e, node_t *inner, const char *name, size_t length) {
    for (const char *p = node_attr(inner, "exportparts"); p && *p;) {
        const char *end = strchr(p, ','), *s = p;
        if (!end) end = p + strlen(p);
        p = *end ? end + 1 : end;
        while (s < end && is_space(*s)) s++;
        const char *key = s;
        while (s < end && !is_space(*s) && *s != ':') s++;
        const char *value = key;
        size_t key_length = (size_t)(s - key), value_length = key_length;
        while (s < end && is_space(*s)) s++;
        if (s < end) {
            if (*s++ != ':') continue;
            while (s < end && is_space(*s)) s++;
            value = s;
            while (s < end && !is_space(*s) && *s != ':') s++;
            value_length = (size_t)(s - value);
            while (s < end && is_space(*s)) s++;
            if (s < end) continue;
        }
        if (key_length && value_length == length && !memcmp(value, name, length) &&
            part_exposed(e, inner, key, key_length)) return true;
    }
    return false;
}

/* The compounds left of c from e, which matched c. */
static bool match_left(const struct compound *c, node_t *e, const struct selector_match *context) {
    node_t *scope = context->shadow;
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

/* ::part() at c->s[at], e the element it would select. The simples after it
   select e; the ones before it and the compounds left of c select a host that
   exposes e under every listed name and is in the rule's tree, or is that
   tree's own host (:host::part()). */
static bool match_part(const struct compound *c, int at, node_t *e, const struct selector_match *context) {
    node_t *scope = context->shadow;
    if (!match_compound_range(c, at + 1, c->n, e, context)) return false;
    for (node_t *host = doc_node_root(e, false)->shadow_host; host; host = doc_node_root(host, false)->shadow_host) {
        node_t *root = doc_node_root(host, false);
        if (scope ? root != scope && host != scope->shadow_host : root->type != N_DOC) continue;
        bool exposed = true;
        for (const char *name = c->s[at].value; exposed && *name; name += strlen(name) + 1)
            exposed = part_exposed(e, host, name, strlen(name));
        if (exposed && match_compound_range(c, 0, at, host, context) && match_left(c, host, context)) return true;
    }
    return false;
}

static bool match_from_bound(const struct compound *c, node_t *e, const struct selector_match *context) {
    node_t *scope = context->shadow;
    if (!e || e->type != N_ELEM) return false;
    if (e) {
        if (css_can_service()) { if (!web_native_checkpoint(e->owner)) return false; }
        else if (__atomic_load_n(&e->owner->native_cancelled, __ATOMIC_ACQUIRE)) return false;
    }
    /* ::part() (op 1 of SK_PE) and ::slotted() select through another element;
       a compound has at most one of them */
    for (int i = 0; i < c->n; i++) if (c->s[i].kind == SK_PE && c->s[i].op == 1) return match_part(c, i, e, context);
    else if (c->s[i].kind == SK_SLOTTED) {
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
    return match_compound(c, e, context) && match_left(c, e, context);
}

/* A query selects elements of the tree itself: no pseudo-element, no ::part(). */
static bool match_list_bound(const struct sellist *l, node_t *e, const struct selector_match *context) {
    for (int i = 0; l && i < l->n; i++)
        if (!l->v[i]->pseudo && !l->v[i]->part && match_from_bound(l->v[i]->right, e, context)) return true;
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

/* DOM "parse a selector" (querySelector, matches, closest): a selector list
   in no stylesheet, so a namespace prefix is undeclared there. */
bool css_select_n(web_doc *d, node_t *scope, const char *selector, size_t length, pvec *out) {
    (void)d;
    if (!selector || !scope) return false;
    arena_t *arena = calloc(1, sizeof *arena);
    if (!arena) return false;
    jmp_buf trap;
    arena->trap = &trap;
    if (setjmp(trap)) { ar_free(arena); free(arena); return false; }
    struct pctx p = {.a = arena};
    struct sellist *sel = selector_list_parse(&p, selector, selector + length);
    if (sel) select_walk(scope, sel, out);
    bool valid = sel != NULL;
    ar_free(arena);
    free(arena);
    return valid;
}
bool css_select(web_doc *d, node_t *scope, const char *selector, pvec *out) {
    return selector && css_select_n(d, scope, selector, strlen(selector), out);
}

bool css_matches_n(node_t *node, node_t *scope, const char *selector, size_t length, bool *valid) {
    if (valid) *valid = false;
    if (!node || node->type != N_ELEM || !selector) return false;
    arena_t *arena = calloc(1, sizeof *arena);
    if (!arena) return false;
    jmp_buf trap;
    arena->trap = &trap;
    if (setjmp(trap)) { ar_free(arena); free(arena); return false; }
    struct pctx p = {.a = arena};
    struct sellist *sel = selector_list_parse(&p, selector, selector + length);
    if (valid) *valid = sel != NULL;
    const struct selector_match context = {.reference = scope ? scope : node};
    bool matched = sel && match_list_bound(sel, node, &context);
    ar_free(arena);
    free(arena);
    return matched;
}
bool css_matches(node_t *node, const char *selector, bool *valid) {
    if (valid) *valid = false;
    return selector && css_matches_n(node, NULL, selector, strlen(selector), valid);
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
                if (!strcmp(name, "attr")) {
                    /* css-values-5 §8.7: attr( <declaration-value>, <declaration-value>? ) makes the
                       declaration valid at parse time; its arguments are component values */
                    const char *comma = supports_delim(s + 1, end, ",");
                    if (skip_ws(s + 1, comma) == comma) return false;
                    *has = true;
                    s++;
                    continue;
                }
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

/* FNV-1a, for the rule index's tables and a declaration block's names */
static uint32_t hash_str(const char *s) {
    uint32_t h = 2166136261u;
    for (; *s; s++) h = (h ^ (unsigned char)*s) * 16777619u;
    return h;
}
static char *strip_comments(arena_t *a, const char *css, size_t n, size_t *out_n);
char *css_preprocess(const char *css, size_t n, size_t *out_n) {
    arena_t a = {0};
    jmp_buf trap;
    a.trap = &trap;
    if (setjmp(trap)) { ar_free(&a); return NULL; }
    size_t length;
    char *stripped = strip_comments(&a, css, n, &length), *copy = malloc(length + 1);
    if (copy) { memcpy(copy, stripped, length + 1); *out_n = length; }
    ar_free(&a);
    return copy;
}

/* ---- CSSOM declaration blocks as text. An element's style attribute and a
   rule's declarations (cssom.c) are read, set and serialized through these. */
/* the first wanted at depth 0 in [p, end): quoted strings, comments, escapes and
   ()[]{} do not split a declaration */
static const char *decl_separator(const char *p, const char *end, int wanted) {
    int quote = 0, depth = 0;
    while (p < end) {
        int c = (unsigned char)*p;
        if (c == '\\' && p + 1 < end) { p += 2; continue; }
        if (quote) { if (c == quote) quote = 0; }
        else if (c == '\'' || c == '"') quote = c;
        else if (c == '/' && p + 1 < end && p[1] == '*') { p += 2; while (p + 1 < end && !(p[0] == '*' && p[1] == '/')) p++; if (p + 1 < end) p += 2; continue; }
        else if (c == '(' || c == '[' || c == '{') depth++;
        else if (c == ')' || c == ']' || c == '}') { if (depth) depth--; }
        else if (!depth && c == wanted) return p;
        p++;
    }
    return end;
}
static bool decl_property_equal(const char *begin, const char *end, const char *property) {
    while (begin < end && is_space((unsigned char)*begin)) begin++;
    while (end > begin && is_space((unsigned char)end[-1])) end--;
    size_t n = (size_t)(end - begin);
    return strlen(property) == n && (!strncmp(property, "--", 2) ? !strncmp(begin, property, n) : !strncasecmp(begin, property, n));
}
static const char *decl_ws(const char *p, const char *end) {
    for (;;) {
        while (p < end && is_space((unsigned char)*p)) p++;
        if (end - p < 2 || p[0] != '/' || p[1] != '*') return p;
        p += 2;
        while (end - p >= 2 && !(p[0] == '*' && p[1] == '/')) p++;
        if (end - p < 2) return end;
        p += 2;
    }
}
/* Priority is not part of the CSSOM value. Do not split quoted strings,
   functions or custom-property token streams at a nested exclamation mark. */
static bool decl_priority(const char *begin, const char **end) {
    const char *bang = decl_separator(begin, *end, '!');
    if (bang == *end) return false;
    const char *p = decl_ws(bang + 1, *end);
    if (*end - p < 9 || strncasecmp(p, "important", 9) ||
        decl_ws(p + 9, *end) != *end) return false;
    *end = bang;
    while (*end > begin && is_space((unsigned char)(*end)[-1])) (*end)--;
    return true;
}
void css_decl_value(const char *property, const char *begin, size_t n, sbuf *out) {
    const char *end = begin + n;
    if(!strcasecmp(property,"border-radius")||!strcasecmp(property,"border-top-left-radius")||!strcasecmp(property,"border-top-right-radius")||
       !strcasecmp(property,"border-bottom-right-radius")||!strcasecmp(property,"border-bottom-left-radius")) {
        sbuf text={0};
        if(css_radius_specified(property,begin,n,&text)){sb_put(out,text.p?text.p:"",text.n);sb_free(&text);return;}
        sb_free(&text);
    }
    {
        sbuf text={0}; /* colours and color-scheme; CSS-wide keywords and var() stay authored */
        if(css_specified_longhand(property,begin,n,&text)){sb_put(out,text.p?text.p:"",text.n);sb_free(&text);return;}
        sb_free(&text);
    }
    if (!strcasecmp(property,"box-shadow")) {
        sbuf serialized={0};
        if (css_shadow_specified(begin,n,&serialized)) {
            sb_put(out,serialized.p?serialized.p:"",serialized.n);
            sb_free(&serialized);return;
        }
        sb_free(&serialized); /* CSS-wide keywords / unresolved var() stay authored. */
    }
    /* Normalize supported numeric opacity, not arbitrary/custom values.
       Preserve var()/calc() token streams unchanged. */
    if (!strcasecmp(property, "opacity") && end - begin > 0 && end - begin < 64) {
        const char *p = begin, *number_end = end;
        bool percent = end[-1] == '%';
        if (percent) number_end--;
        if (p < number_end && (*p == '+' || *p == '-')) p++;
        const char *digits = p;
        while (p < number_end && *p >= '0' && *p <= '9') p++;
        bool any = p > digits;
        if (p < number_end && *p == '.') {
            p++; digits = p;
            while (p < number_end && *p >= '0' && *p <= '9') p++;
            any = p > digits;
        }
        if (any && p < number_end && (*p == 'e' || *p == 'E')) {
            p++; if (p < number_end && (*p == '+' || *p == '-')) p++;
            digits = p;
            while (p < number_end && *p >= '0' && *p <= '9') p++;
            any = p > digits;
        }
        if (any && p == number_end) {
            char number[64], result[64];
            memcpy(number, begin, (size_t)(number_end - begin));
            number[number_end - begin] = 0;
            double value = strtod(number, NULL);
            if (isfinite(value) && fabs(value) < 1e12) {
                if (fabs(value) < .0000005) value = 0;
                int len = snprintf(result, sizeof result, "%.6f", value);
                if (len > 0 && (size_t)len < sizeof result) {
                    while (len && result[len-1] == '0') len--;
                    if (len && result[len-1] == '.') len--;
                    if (percent) result[len++] = '%';
                    sb_put(out, result, (size_t)len);
                    return;
                }
            }
        }
    }
    sb_put(out, begin, n);
}
bool css_decl_find(const char *css, size_t n, const char *property, const char **value, size_t *value_n, bool *important) {
    const char *found = NULL, *found_end = NULL, *end = css + n;
    bool found_important = false;
    for (const char *p = css; p < end;) {
        const char *e = decl_separator(p, end, ';'), *colon = decl_separator(p, e, ':');
        if (colon < e && decl_property_equal(p, colon, property)) {
            const char *v = colon + 1, *ve = e;
            while (v < ve && is_space((unsigned char)*v)) v++;
            while (ve > v && is_space((unsigned char)ve[-1])) ve--;
            bool priority = decl_priority(v, &ve);
            if (!found || priority || !found_important) {
                found = v; found_end = ve; found_important = priority;
            }
        }
        p = e < end ? e + 1 : end;
    }
    *value = found; *value_n = found ? (size_t)(found_end - found) : 0; *important = found_important;
    return found != NULL;
}
/* ---- a CSS declaration block's declarations (CSSOM 6.6): longhands and custom
   properties, one per name, in order. A shorthand sets its longhands (cssprop.c's
   css_shorthand_expand); one whose value the block keeps whole (a var() reference,
   pending substitution in CSS Variables 1 §3, or a value with no longhand text) is a
   group its longhands stand in, read as the empty string. Each value is the one it
   was given (comment-free, trimmed: a longhand's the text its shorthand gave it), what
   the cascade parses; CSSOM reads it serialized (css_decl_value, a descriptor block's
   check), which may round (an origin colour's channels, CSS Color 4 §15.2). A block's
   text (css_decl_block) writes each declaration, a group once where its first
   longhand stands, so parsing it again gives the same declarations; its
   serialization (css_decl_serialize) puts longhands back into shorthands (CSSOM
   "serialize a CSS declaration block"). */
struct decl_entry { char *name; sbuf value; bool important, live; uint32_t group; /* group index + 1, or 0 */ };
struct decl_group { char *name; sbuf text; bool important, emitted; };
struct decl_list {
    struct decl_entry *v;
    size_t count, capacity;
    size_t *slots, mask;   /* a name's live entry: its index + 1; DECL_GONE where one was */
    struct decl_group *groups;
    size_t groups_n, groups_capacity;
    bool failed;           /* no memory */
};
#define DECL_GONE SIZE_MAX
static void decl_list_free(struct decl_list *l) {
    for (size_t i = 0; i < l->count; i++) { free(l->v[i].name); sb_free(&l->v[i].value); }
    for (size_t i = 0; i < l->groups_n; i++) { free(l->groups[i].name); sb_free(&l->groups[i].text); }
    free(l->v); free(l->slots); free(l->groups);
    memset(l, 0, sizeof *l);
}
/* name's slot: its live entry's (*entry its index), else the first free one on its probe */
static size_t decl_slot(const struct decl_list *l, const char *name, size_t *entry) {
    size_t at = hash_str(name) & l->mask, free_at = SIZE_MAX;
    *entry = SIZE_MAX;
    for (;;) {
        size_t s = l->slots[at];
        if (!s) return free_at != SIZE_MAX ? free_at : at;
        if (s == DECL_GONE) { if (free_at == SIZE_MAX) free_at = at; }
        else if (!strcmp(l->v[s - 1].name, name)) { *entry = s - 1; return at; }
        at = (at + 1) & l->mask;
    }
}
static size_t decl_find(const struct decl_list *l, const char *name) {
    size_t entry = SIZE_MAX;
    if (l->slots) decl_slot(l, name, &entry);
    return entry;
}
static void decl_remove(struct decl_list *l, size_t i) {
    size_t entry, at = decl_slot(l, l->v[i].name, &entry);
    if (entry == i) l->slots[at] = DECL_GONE;
    l->v[i].live = false;
}
/* append a declaration in place of the live one of its name; name and value move in */
static bool decl_append(struct decl_list *l, char *name, sbuf *value, bool important, uint32_t group) {
    if (l->failed) { free(name); sb_free(value); return false; }
    if (l->count == l->capacity) {
        /* the slots are at most half used (each entry has had at most one), so a probe ends */
        size_t grown = l->capacity ? l->capacity * 2 : 16;
        struct decl_entry *more = grown <= SIZE_MAX / sizeof *more ? realloc(l->v, grown * sizeof *more) : NULL;
        size_t *table = grown <= SIZE_MAX / (2 * sizeof *table) ? calloc(grown * 2, sizeof *table) : NULL;
        if (more) l->v = more;
        if (!more || !table) { free(table); free(name); sb_free(value); l->failed = true; return false; }
        l->capacity = grown;
        free(l->slots); l->slots = table; l->mask = grown * 2 - 1;
        for (size_t i = 0; i < l->count; i++) if (l->v[i].live) {
            size_t at = hash_str(l->v[i].name) & l->mask;
            while (l->slots[at]) at = (at + 1) & l->mask;
            l->slots[at] = i + 1;
        }
    }
    size_t entry, at = decl_slot(l, name, &entry);
    if (entry != SIZE_MAX) l->v[entry].live = false;
    l->slots[at] = l->count + 1;
    l->v[l->count++] = (struct decl_entry){name, *value, important, true, group};
    *value = (sbuf){0};
    return true;
}
/* a declaration as parsed: the block keeps the later of two of one name, unless
   only the earlier is !important */
static bool decl_parsed(struct decl_list *l, char *name, sbuf *value, bool important, uint32_t group) {
    size_t i = decl_find(l, name);
    if (i != SIZE_MAX && l->v[i].important && !important) { free(name); sb_free(value); return true; }
    return decl_append(l, name, value, important, group);
}
/* CSSOM "set a CSS declaration" (its example steps): the declaration of the name
   changes in place, unless a later one in its logical property group maps the
   other way (CSS Logical 1), which it must then follow */
static bool decl_set_one(struct decl_list *l, char *name, sbuf *value, bool important) {
    size_t i = decl_find(l, name);
    if (i != SIZE_MAX && !l->v[i].group) {
        char group[32], other[32];
        bool logical, other_logical, after = false;
        if (css_prop_logical_group(name, group, sizeof group, &logical))
            for (size_t k = i + 1; k < l->count && !after; k++)
                after = l->v[k].live && css_prop_logical_group(l->v[k].name, other, sizeof other, &other_logical) &&
                        !strcmp(group, other) && other_logical != logical;
        if (!after) {
            sb_free(&l->v[i].value);
            l->v[i].value = *value; *value = (sbuf){0};
            l->v[i].important = important;
            free(name);
            return true;
        }
    }
    return decl_append(l, name, value, important, 0);
}
/* a shorthand kept whole: the group, and its longhands standing in it */
static bool decl_whole(struct decl_list *l, const struct propdef *p, const char *name, sbuf *text, bool important,
                       bool parsed) {
    if (l->groups_n == l->groups_capacity) {
        size_t grown = l->groups_capacity ? l->groups_capacity * 2 : 4;
        struct decl_group *more = grown <= SIZE_MAX / sizeof *more ? realloc(l->groups, grown * sizeof *more) : NULL;
        if (!more) { sb_free(text); l->failed = true; return false; }
        l->groups = more; l->groups_capacity = grown;
    }
    char *own = strdup(name);
    if (!own) { sb_free(text); l->failed = true; return false; }
    l->groups[l->groups_n++] = (struct decl_group){own, *text, important, false};
    *text = (sbuf){0};
    uint32_t group = (uint32_t)l->groups_n;
    const char *const *names;
    int count = css_shorthand_longhands(p, &names);
    for (int i = 0; i < count; i++) {
        char *longhand = strdup(names[i]);
        sbuf none = {0};
        if (!longhand) { l->failed = true; return false; }
        if (!(parsed ? decl_parsed(l, longhand, &none, important, group) : decl_append(l, longhand, &none, important, group)))
            return false;
    }
    return true;
}
struct decl_expansion { struct decl_list *l; bool important, parsed; };
static void decl_longhand(void *opaque, const char *name, const char *value, size_t n) {
    struct decl_expansion *x = opaque;
    if (x->l->failed) return;
    char *longhand = strdup(name);
    sbuf text = {0};
    if (!longhand) { x->l->failed = true; return; }
    sb_put(&text, value, n);
    if (x->parsed) decl_parsed(x->l, longhand, &text, x->important, 0);
    else decl_set_one(x->l, longhand, &text, x->important);
}
/* the shorthand a (lowercased) name is, or NULL */
static const struct propdef *decl_shorthand_of(const char *name) {
    const struct propdef *p = strncmp(name, "--", 2) ? css_prop_lookup(name, strlen(name)) : NULL;
    bool longhand = true;
    return p && css_prop_name(css_prop_index(p), &longhand) && !longhand ? p : NULL;
}
static bool value_has_var(const char *v, size_t n) {
    arena_t a = {0};
    jmp_buf trap;
    a.trap = &trap;
    bool has = false;
    if (!setjmp(trap) && !supports_vars(&a, v, v + n, &has)) has = false;
    ar_free(&a);
    return has;
}
/* a shorthand's declaration (valid): its longhands, or the group of a value kept whole */
static bool decl_shorthand(struct decl_list *l, const struct propdef *p, const char *name, const char *v, size_t n,
                           bool important, bool parsed) {
    if (!value_has_var(v, n)) {
        struct decl_expansion x = {l, important, parsed};
        int result = css_shorthand_expand(p, v, n, decl_longhand, &x);
        if (result == 1) return !l->failed;
        if (result == 0) return true;
        if (result == -2) { l->failed = true; return false; }
    }
    sbuf text = {0};
    sb_put(&text, v, n);
    return decl_whole(l, p, name, &text, important, parsed);
}
/* the declarations css parses to, their values as given. check: a descriptor
   block's own (its names are not shorthands), NULL for properties. A block's
   text (trusted) has only valid declarations. */
static void decl_list_parse(struct decl_list *l, const char *css, size_t n, css_decl_check *check, bool trusted) {
    const char *end = css + n;
    for (const char *p = css; p < end && !l->failed;) {
        const char *e = decl_separator(p, end, ';'), *colon = decl_separator(p, e, ':');
        const char *ns = p, *ne = colon, *v = colon + 1, *ve = e;
        p = e < end ? e + 1 : end;
        if (colon >= e) continue;
        while (ns < ne && is_space((unsigned char)*ns)) ns++;
        while (ne > ns && is_space((unsigned char)ne[-1])) ne--;
        while (v < ve && is_space((unsigned char)*v)) v++;
        while (ve > v && is_space((unsigned char)ve[-1])) ve--;
        bool important = decl_priority(v, &ve);
        if (ns == ne || v == ve) continue;
        char *name = strndup(ns, (size_t)(ne - ns));
        if (!name) { l->failed = true; break; }
        bool custom = !strncmp(name, "--", 2);
        if (!custom) for (char *c = name; *c; c++) *c = (char)lower((unsigned char)*c);
        size_t vn = (size_t)(ve - v);
        const struct propdef *shorthand = check || custom ? NULL : decl_shorthand_of(name);
        if (shorthand) {
            if (trusted || css_valid_declaration(name, strlen(name), v, vn)) decl_shorthand(l, shorthand, name, v, vn, important, true);
            free(name);
            continue;
        }
        if (!trusted && !(check ? check(name, v, vn, NULL) : css_valid_declaration(name, strlen(name), v, vn))) {
            free(name);
            continue;
        }
        sbuf value = {0};
        sb_put(&value, v, vn);
        decl_parsed(l, name, &value, important, 0);
    }
}
static void decl_put(sbuf *out, size_t start, const char *name, const sbuf *value, bool important) {
    if (out->n > start) sb_putc(out, ' ');
    sb_puts(out, name); sb_puts(out, ": ");
    sb_put(out, value->p ? value->p : "", value->n);
    if (important) sb_puts(out, " !important");
    sb_putc(out, ';');
}
static void decl_list_names(const struct decl_list *l, sbuf *names) {
    for (size_t i = 0; i < l->count; i++)
        if (l->v[i].live) sb_put(names, l->v[i].name, strlen(l->v[i].name) + 1);
}
/* a group, once: at its first longhand that stands; its text, or serialized */
static void decl_put_group(const struct decl_list *l, uint32_t group, bool serialize, sbuf *out, size_t start) {
    struct decl_group *g = &l->groups[group - 1];
    if (g->emitted) return;
    g->emitted = true;
    if (!serialize) { decl_put(out, start, g->name, &g->text, g->important); return; }
    sbuf text = {0};
    css_decl_value(g->name, g->text.p ? g->text.p : "", g->text.n, &text);
    decl_put(out, start, g->name, &text, g->important);
    sb_free(&text);
}
static void decl_list_block(const struct decl_list *l, sbuf *out) {
    size_t start = out->n;
    for (size_t g = 0; g < l->groups_n; g++) l->groups[g].emitted = false;
    for (size_t i = 0; i < l->count; i++) {
        const struct decl_entry *e = &l->v[i];
        if (!e->live) continue;
        if (e->group) decl_put_group(l, e->group, false, out, start);
        else decl_put(out, start, e->name, &e->value, e->important);
    }
}
/* a declaration's value as CSSOM serializes it: a property's specified value, a
   descriptor's as its block's check writes it */
static void decl_value_serialize(const char *name, const sbuf *value, css_decl_check *check, sbuf *out) {
    const char *v = value->p ? value->p : "";
    if (check) check(name, v, value->n, out);
    else css_decl_value(name, v, value->n, out);
}
/* CSSOM "serialize a CSS declaration block", step 4.3: a declaration between a
   shorthand's longhands, not one of them, in the logical property group of one of
   them but mapped the other way keeps them apart */
static bool decl_intermixed(const struct decl_list *l, const size_t *used, int count, size_t first, size_t last) {
    if (last <= first + 1) return false;
    struct { char group[32]; bool logical; } seen[8];
    int kinds = 0;
    for (int k = 0; k < count && kinds < 8; k++) {
        char group[32];
        bool logical, known = false;
        if (!css_prop_logical_group(l->v[used[k]].name, group, sizeof group, &logical)) continue;
        for (int s = 0; s < kinds; s++) known |= !strcmp(seen[s].group, group) && seen[s].logical == logical;
        if (!known) { memcpy(seen[kinds].group, group, sizeof group); seen[kinds++].logical = logical; }
    }
    for (size_t j = first + 1; kinds && j < last; j++) {
        char group[32];
        bool logical;
        if (!l->v[j].live || !css_prop_logical_group(l->v[j].name, group, sizeof group, &logical)) continue;
        for (int s = 0; s < kinds; s++) if (!strcmp(seen[s].group, group) && seen[s].logical != logical) return true;
    }
    return false;
}
/* CSSOM "serialize a CSS declaration block": each declaration, or the first
   shorthand (in preferred order) all of whose longhands are still to write, of one
   importance, and whose value can be written; a group as its shorthand. A
   shorthand that cannot be written once cannot later (its reasons stay), so each
   is tried once. A descriptor block (check) has no shorthands. */
static bool decl_list_serialize(const struct decl_list *l, css_decl_check *check, sbuf *out) {
    size_t start = out->n, room = 0;
    sbuf *serialized = calloc(l->count ? l->count : 1, sizeof *serialized);
    uint8_t *done = calloc(l->count ? l->count : 1, 1), *tried = calloc((size_t)css_prop_count(), 1);
    const char **values = NULL;
    size_t *lengths = NULL, *used = NULL;
    bool failed = !serialized || !done || !tried;
    for (size_t i = 0; !failed && i < l->count; i++)
        if (l->v[i].live && !l->v[i].group) decl_value_serialize(l->v[i].name, &l->v[i].value, check, &serialized[i]);
    for (size_t g = 0; g < l->groups_n; g++) l->groups[g].emitted = false;
    for (size_t i = 0; !failed && i < l->count; i++) {
        const struct decl_entry *e = &l->v[i];
        if (!e->live || done[i]) continue;
        done[i] = 1;
        if (e->group) { decl_put_group(l, e->group, true, out, start); continue; }
        const struct propdef *const *candidates;
        int candidate_n = !check && strncmp(e->name, "--", 2) ? css_shorthands_of(e->name, &candidates) : 0;
        bool written = false;
        for (int c = 0; c < candidate_n && !written && !failed; c++) {
            const struct propdef *shorthand = candidates[c];
            int index = css_prop_index(shorthand);
            if (tried[index]) continue;
            tried[index] = 1;
            const char *const *names;
            int count = css_shorthand_longhands(shorthand, &names);
            if (count < 1) continue;
            if ((size_t)count > room) {
                const char **v = realloc(values, (size_t)count * sizeof *v);
                if (v) values = v;
                size_t *n = v ? realloc(lengths, (size_t)count * sizeof *n) : NULL;
                if (n) lengths = n;
                size_t *u = n ? realloc(used, (size_t)count * sizeof *u) : NULL;
                if (u) used = u;
                if (!u) { failed = true; break; }
                room = (size_t)count;
            }
            bool ok = true, important = e->important;
            size_t first = i, last = i;
            for (int k = 0; ok && k < count; k++) {
                size_t j = decl_find(l, names[k]);
                ok = j != SIZE_MAX && (j == i || !done[j]) && !l->v[j].group && l->v[j].important == important;
                if (!ok) break;
                used[k] = j;
                values[k] = serialized[j].p ? serialized[j].p : "";
                lengths[k] = serialized[j].n;
                if (j < first) first = j;
                if (j > last) last = j;
            }
            if (!ok || decl_intermixed(l, used, count, first, last)) continue;
            size_t at = out->n;
            if (out->n > start) sb_putc(out, ' ');
            sb_puts(out, css_prop_name(index, NULL)); sb_puts(out, ": ");
            size_t value_at = out->n;
            if (!css_shorthand_serialize(shorthand, values, lengths, out) || out->n == value_at) { out->n = at; continue; }
            if (important) sb_puts(out, " !important");
            sb_putc(out, ';');
            for (int k = 0; k < count; k++) done[used[k]] = 1;
            written = true;
        }
        if (!written && !failed) decl_put(out, start, e->name, &serialized[i], e->important);
    }
    for (size_t i = 0; serialized && i < l->count; i++) sb_free(&serialized[i]);
    free(serialized); free(done); free(tried); free(values); free(lengths); free(used);
    return !failed;
}
static char *decl_name(const char *property) {
    char *name = strdup(property);
    if (name && strncmp(name, "--", 2)) for (char *c = name; *c; c++) *c = (char)lower((unsigned char)*c);
    return name;
}
bool css_decl_block(const char *css, size_t n, css_decl_check *check, sbuf *block) {
    struct decl_list l = {0};
    decl_list_parse(&l, css, n, check, false);
    if (!l.failed) decl_list_block(&l, block);
    bool ok = !l.failed;
    decl_list_free(&l);
    return ok;
}
bool css_decl_serialize(const char *css, size_t n, css_decl_check *check, sbuf *out, sbuf *names) {
    struct decl_list l = {0};
    decl_list_parse(&l, css, n, check, false);
    bool ok = !l.failed && decl_list_serialize(&l, check, out);
    if (ok && names) decl_list_names(&l, names);
    decl_list_free(&l);
    return ok;
}
bool css_decl_block_serialize(const char *block, size_t n, css_decl_check *check, sbuf *out, sbuf *names) {
    struct decl_list l = {0};
    decl_list_parse(&l, block, n, check, true);
    bool ok = !l.failed && decl_list_serialize(&l, check, out);
    if (ok && names) decl_list_names(&l, names);
    decl_list_free(&l);
    return ok;
}
/* CSSOM getPropertyValue() and getPropertyPriority(): a shorthand's value is its
   longhands' (all of them, of one importance) as the shorthand serializes them, or
   the text of the group they all stand in; a longhand in a group reads as "" */
bool css_decl_get(const char *block, size_t n, const char *property, css_decl_check *check, sbuf *value, bool *important) {
    *important = false;
    char *name = decl_name(property);
    if (!name) return false;
    struct decl_list l = {0};
    decl_list_parse(&l, block, n, check, true);
    const struct propdef *shorthand = check ? NULL : decl_shorthand_of(name);
    bool ok = !l.failed;
    if (ok && shorthand) {
        const char *const *names;
        int count = css_shorthand_longhands(shorthand, &names);
        const char **values = count > 0 ? malloc((size_t)count * sizeof *values) : NULL;
        size_t *lengths = count > 0 ? malloc((size_t)count * sizeof *lengths) : NULL;
        sbuf *serialized = count > 0 ? calloc((size_t)count, sizeof *serialized) : NULL;
        ok = values && lengths && serialized;
        bool all = ok, every_important = true, first_important = false, mixed = false, grouped = false;
        uint32_t group = 0;
        for (int k = 0; all && k < count; k++) {
            size_t j = decl_find(&l, names[k]);
            if (j == SIZE_MAX) { all = false; break; }
            const struct decl_entry *e = &l.v[j];
            if (!k) first_important = e->important;
            every_important &= e->important;
            mixed |= e->important != first_important;
            if (e->group) grouped = true;
            if (!k) group = e->group; else if (e->group != group) group = 0;
            if (!e->group) decl_value_serialize(names[k], &e->value, NULL, &serialized[k]);
            values[k] = serialized[k].p ? serialized[k].p : "";
            lengths[k] = serialized[k].n;
        }
        /* longhands of two importances: no value, and not important */
        if (all && !mixed) {
            *important = every_important;
            if (grouped) {
                const struct decl_group *g = group ? &l.groups[group - 1] : NULL;
                if (g && !strcmp(g->name, name)) css_decl_value(g->name, g->text.p ? g->text.p : "", g->text.n, value);
            } else css_shorthand_serialize(shorthand, values, lengths, value);
        }
        for (int k = 0; serialized && k < count; k++) sb_free(&serialized[k]);
        free(serialized); free(values); free(lengths);
    } else if (ok) {
        size_t j = decl_find(&l, name);
        if (j != SIZE_MAX) {
            const struct decl_entry *e = &l.v[j];
            *important = e->important;
            if (!e->group) decl_value_serialize(e->name, &e->value, check, value);
        }
    }
    decl_list_free(&l);
    free(name);
    return ok;
}
/* CSSOM setProperty() (and removeProperty(), an empty value) on a block's text:
   out gets the block's new text, the value as given. 1, 0 for a value that is not
   one of the property (or a priority that is not "important"), -1 a name that
   cannot be a property's, -2 no memory. */
int css_decl_set(const char *css, size_t n, const char *property, const char *value, const char *priority,
                 css_decl_check *check, sbuf *out) {
    if (strchr(property, ':') || strchr(property, ';') || strchr(property, '{') || strchr(property, '}')) return -1;
    const char *value_end = value + strlen(value);
    if (*value && ((priority && *priority && strcasecmp(priority, "important")) ||
                   decl_separator(value, value_end, '!') != value_end ||
                   decl_separator(value, value_end, ';') != value_end)) return 0;
    char *name = decl_name(property);
    if (!name) return -2;
    const struct propdef *shorthand = check ? NULL : decl_shorthand_of(name);
    bool important = priority && *priority;
    /* CSSOM setProperty parses the value for the property before changing the
       block: the value as CSS reads it (no comments), trimmed. Keep
       syntactically valid but not-yet-rendered values separate from @supports
       capability reporting; both use the real property grammar and var rules. */
    sbuf exact = {0};
    char *clean = NULL;
    const char *v = value;
    size_t vn = 0;
    if (*value) {
        clean = css_preprocess(value, (size_t)(value_end - value), &vn);
        if (!clean) { free(name); return -2; }
        v = clean;
        while (vn && is_space((unsigned char)*v)) v++, vn--;
        while (vn && is_space((unsigned char)v[vn - 1])) vn--;
        if (!vn || !(check ? check(name, v, vn, NULL) : css_valid_declaration(name, strlen(name), v, vn))) {
            free(clean); free(name); return 0;
        }
        sb_put(&exact, v, vn);
    }
    struct decl_list l = {0};
    decl_list_parse(&l, css, n, check, true);
    if (!*value) {
        /* removeProperty(): the declaration, or each of a shorthand's longhands */
        const char *const *names;
        int count = shorthand ? css_shorthand_longhands(shorthand, &names) : 0;
        for (int k = 0; k < count; k++) { size_t j = decl_find(&l, names[k]); if (j != SIZE_MAX) decl_remove(&l, j); }
        size_t j = shorthand ? SIZE_MAX : decl_find(&l, name);
        if (j != SIZE_MAX) decl_remove(&l, j);
    } else if (shorthand) decl_shorthand(&l, shorthand, name, v, vn, important, false);
    else {
        char *own = strdup(name);
        if (own) decl_set_one(&l, own, &exact, important);
        else l.failed = true;
    }
    if (!l.failed) decl_list_block(&l, out);
    int status = l.failed ? -2 : 1;
    decl_list_free(&l);
    sb_free(&exact); free(clean); free(name);
    return status;
}
void css_decl_drop_important(const char *css, size_t n, sbuf *out) {
    const char *end = css + n;
    for (const char *p = css; p < end;) {
        const char *e = decl_separator(p, end, ';'), *colon = decl_separator(p, e, ':');
        const char *v = colon < e ? colon + 1 : e, *ve = e;
        while (ve > v && is_space((unsigned char)ve[-1])) ve--;
        if (colon >= e || !decl_priority(v, &ve)) { sb_put(out, p, (size_t)(e - p)); if (e < end) sb_putc(out, ';'); }
        p = e < end ? e + 1 : end;
    }
}
/* whether [s, +n) can stand inside a block: its braces, parentheses and
   brackets balance (as block_end and scan_to count them) and no string is open */
bool css_decl_balanced(const char *s, size_t n) {
    size_t braces = 0, parens = 0;
    int quote = 0;
    for (const char *p = s, *end = s + n; p < end; p++) {
        char c = *p;
        if (c == '\\') { if (p + 1 < end) p++; continue; }
        if (quote) { if (c == quote) quote = 0; else if (c == '\n') return false; continue; }
        if (c == '"' || c == '\'') quote = c;
        else if (c == '{') braces++;
        else if (c == '}') { if (!braces) return false; braces--; }
        else if (c == '(' || c == '[') parens++;
        else if (c == ')' || c == ']') { if (!parens) return false; parens--; }
    }
    return !quote && !braces && !parens;
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
                if (!strcmp(name, "selector")) yes = selector_supported(a, p + 1, end);
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

static bool supports_evaluate(const char *condition, size_t n, bool implied_parens, bool *valid) {
    *valid = false;
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
        result = r.valid && r.value; *valid = r.valid;
        if (!result && implied_parens) {
            char *wrapped = ar_alloc(a, len + 3);
            wrapped[0] = '(';
            memcpy(wrapped + 1, s, len);
            wrapped[len + 1] = ')'; wrapped[len + 2] = 0;
            r = supports_expr(a, wrapped, wrapped + len + 2);
            result = r.valid && r.value; *valid |= r.valid;
        }
    }
    ar_free(a);
    free(a);
    return result;
}
bool css_supports_condition(const char *condition, size_t n, bool implied_parens) {
    bool valid; return supports_evaluate(condition, n, implied_parens, &valid);
}
bool css_supports_grammar(const char *condition, size_t n, bool implied_parens) {
    bool valid; supports_evaluate(condition, n, implied_parens, &valid); return valid;
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
        d->base = pc->base;
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

/* ---- CSSOM rule tree. An ordinary parse records only the top-level
   @import rules the resource scan reads; a CSSOM parse (cssom_tree) records
   every rule the grammar keeps, in the same pass that builds the cascade AST,
   so both always agree on what a sheet contains. */
struct cssom_run { const char *start, *end; bool valid, implicit; };
static struct css_rule_info *cssom_link(struct pctx *pc, uint32_t kind, const char *s, const char *e) {
    struct css_rule_info *parent = pc->cssom_parent;
    uint32_t *count = parent ? &parent->child_count : &pc->sh->cssom_count;
    if (*count == UINT32_MAX || (size_t)(e - s) > UINT32_MAX) {
        if (pc->a->trap) longjmp(*pc->a->trap, 1);
        abort();
    }
    struct css_rule_info *info = ar_alloc(pc->a, sizeof *info);
    /* The comment-free source lives as long as the sheet: no copy of the rule. */
    info->type = kind; info->text = s; info->text_len = (uint32_t)(e - s); info->parent = parent;
    info->closed = e > s && (e[-1] == '}' || e[-1] == ';');
    if (parent) {
        /* A style or page rule owns the declarations before its first child. */
        if (!parent->child_count && (parent->type == CSS_RULE_STYLE || parent->type == CSS_RULE_PAGE) &&
            s >= parent->text + parent->decls_at && s <= parent->text + parent->decls_at + parent->decls_len)
            parent->decls_len = (uint32_t)(s - (parent->text + parent->decls_at));
        if (parent->last) parent->last->next = info; else parent->first = info;
        parent->last = info;
    } else {
        if (pc->sh->cssom_last) pc->sh->cssom_last->next = info; else pc->sh->cssom_first = info;
        pc->sh->cssom_last = info;
    }
    (*count)++; return info;
}
static void cssom_flush_run(struct pctx *pc) {
    struct cssom_run *run = pc->cssom_run;
    if (!run || !run->start) return;
    pc->cssom_run = NULL;
    /* The leading run of a style rule is its own declarations. */
    if ((run->implicit || (pc->cssom_parent && pc->cssom_parent->child_count)) && run->valid) {
        struct css_rule_info *info = cssom_link(pc, CSS_RULE_NESTED_DECLARATIONS, run->start, run->end);
        info->decls_len = info->text_len;
    }
    run->start = run->end = NULL; run->valid = false; pc->cssom_run = run;
}
static struct css_rule_info *cssom_record(struct pctx *pc, uint32_t kind, const char *s, const char *e) {
    if (!pc->sh || (!pc->cssom_tree && (kind != CSS_RULE_IMPORT || pc->rule_depth != 1))) return NULL;
    if (pc->cssom_tree) cssom_flush_run(pc);
    return cssom_link(pc, kind, s, e);
}
static void cssom_ranges(struct css_rule_info *info, const char *ps, const char *pe, const char *bs, const char *be) {
    if (!info) return;
    if (ps && pe >= ps) { info->prelude_at = (uint32_t)(ps - info->text); info->prelude_len = (uint32_t)(pe - ps); }
    if (bs) {
        info->block = true;
        info->block_at = (uint32_t)(bs - info->text); info->block_len = (uint32_t)(be - bs);
        if (info->type == CSS_RULE_STYLE || info->type == CSS_RULE_PAGE || info->type == CSS_RULE_KEYFRAME ||
            info->type == CSS_RULE_FONT_FACE || info->type == CSS_RULE_MARGIN || info->type == CSS_RULE_COUNTER_STYLE ||
            info->type == CSS_RULE_PROPERTY || info->type == CSS_RULE_FONT_PALETTE_VALUES) {
            info->decls_at = info->block_at; info->decls_len = info->block_len;
        }
    }
}
/* A false @supports or an unimplemented @starting-style keeps its rules in
   the CSSOM tree, while this condition keeps them out of every cascade. */
static const struct mcond *cssom_inert(struct pctx *pc, const struct mcond *up) {
    struct mcond *mc = ar_alloc(pc->a, sizeof *mc);
    mc->up = up; mc->q = "not all"; mc->axes = 1;
    return mc;
}
static uint32_t cssom_group_kind(const char *name) {
    return !name ? 0 : !strcmp(name,"media") ? CSS_RULE_MEDIA : !strcmp(name,"supports") ? CSS_RULE_SUPPORTS :
        !strcmp(name,"container") ? CSS_RULE_CONTAINER : !strcmp(name,"layer") ? CSS_RULE_LAYER_BLOCK :
        !strcmp(name,"scope") ? CSS_RULE_SCOPE : !strcmp(name,"starting-style") ? CSS_RULE_STARTING_STYLE : 0;
}

/* Only where cssom.c put one (its offsets, never the author's text) is
   CSS_NESTED_DECLARATIONS_MARKER a nested declarations rule. */
static bool cssom_marker(const struct pctx *pc, const char *s) {
    if (!pc->cssom_nmarkers || !pc->cssom_text || s < pc->cssom_text) return false;
    size_t at = (size_t)(s - pc->cssom_text);
    uint32_t lo = 0, hi = pc->cssom_nmarkers;
    while (lo < hi) {
        uint32_t mid = lo + (hi - lo) / 2;
        if (pc->cssom_markers[mid] < at) lo = mid + 1; else hi = mid;
    }
    return lo < pc->cssom_nmarkers && pc->cssom_markers[lo] == at &&
        !strncmp(s, CSS_NESTED_DECLARATIONS_MARKER, sizeof CSS_NESTED_DECLARATIONS_MARKER - 1);
}

/* the declarations and nested rules of a block */
static void parse_body(struct pctx *pc, const char *s, const char *e, const char *sel, const struct mcond *media,
                       struct decl **out, int *nout) {
    sbuf d = {0}; /* struct decl array */
    struct cssom_run *run = pc->cssom_tree && sel ? pc->cssom_run : NULL;
    while (s < e) {
        while (s < e && (is_space((unsigned char)*s) || *s == ';')) s++;
        if (s >= e) break;
        if (*s == '@') {
            const char *p = scan_to(s, e, "{;}");
            if (p >= e || *p != '{') {
                const char *ns=s+1;const char *name=read_ident(pc->a,&ns,p,true);
                const char *qs=skip_ws(ns,p),*qe=p;trim_r(qs,&qe);
                if(pc->sh && name&&!strcmp(name,"layer") && (p<e&&*p==';') && layer_statement(pc,qs,qe,media) && run)
                    cssom_ranges(cssom_record(pc,CSS_RULE_LAYER_STATEMENT,s,p+1),qs,qe,NULL,NULL);
                s = p < e ? p + 1 : e;
                continue;
            }
            const char *be = block_end(p + 1, e);
            if (sel && cssom_marker(pc, s)) {
                /* cssom.c's text for a CSSNestedDeclarations rule: its block is
                   one run of the parent's declarations, which apply as & does */
                struct css_rule_info *nd = run ? cssom_record(pc, CSS_RULE_NESTED_DECLARATIONS, s, be < e ? be + 1 : e) : NULL;
                if (nd) {
                    nd->marker = nd->block = true;
                    nd->block_at = nd->decls_at = (uint32_t)(p + 1 - s);
                    nd->block_len = nd->decls_len = (uint32_t)(be - (p + 1));
                }
                bool tree = pc->cssom_tree;
                pc->cssom_tree = false;
                static const char nested[] = "&";
                parse_style_rule(pc, nested, nested + 1, p + 1, be, media, sel);
                pc->cssom_tree = tree;
                s = be < e ? be + 1 : e;
                continue;
            }
            if (sel) {
                const char *ns = s + 1;
                const char *name = read_ident(pc->a, &ns, p, true);
                const char *qs = skip_ws(ns, p), *qe = p;
                trim_r(qs, &qe);
                uint32_t kind = cssom_group_kind(name);
                if (kind && (kind != CSS_RULE_STARTING_STYLE || run)) {
                    const struct mcond *m = media;
                    if (kind == CSS_RULE_MEDIA || kind == CSS_RULE_CONTAINER)
                        m=parse_condition(pc,qs,qe,media,kind == CSS_RULE_CONTAINER);
                    bool applies = kind != CSS_RULE_STARTING_STYLE &&
                        (kind != CSS_RULE_SUPPORTS || css_supports_condition(qs, (size_t)(p - qs), false));
                    bool valid = kind != CSS_RULE_SUPPORTS || applies || css_supports_grammar(qs, (size_t)(p - qs), false);
                    struct css_layer_decl *previous=pc->layer;
                    if(kind == CSS_RULE_LAYER_BLOCK) {
                        pc->layer=layer_name_parse(pc,qs,qe,true);
                        if(!pc->layer){pc->layer=previous;s=be<e?be+1:e;continue;}
                        layer_event(pc,pc->layer,media,NULL);
                    }
                    struct css_rule_info *group = run && valid ? cssom_record(pc, kind, s, be < e ? be + 1 : e) : NULL;
                    cssom_ranges(group, qs, qe, p + 1, be);
                    if (applies || group) {
                        struct css_rule_info *outer = pc->cssom_parent;
                        if (group) { pc->cssom_parent = group; pc->cssom_implicit = true; }
                        static const char amp[] = "&";
                        parse_style_rule(pc, amp, amp + 1, p + 1, be, applies ? m : cssom_inert(pc, m), sel);
                        pc->cssom_parent = outer; pc->cssom_implicit = false;
                    }
                    pc->layer=previous;
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
        bool valid = parse_decl(pc, s, p, &x) != NULL;
        if (valid) sb_put(&d, (const char *)&x, sizeof x);
        if (run && pc->cssom_run == run) {
            if (!run->start) run->start = s;
            run->end = p < e ? p + 1 : e; run->valid |= valid;
        }
        s = p < e ? p + 1 : e;
    }
    if (run && pc->cssom_run == run) cssom_flush_run(pc);
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
    /* A nested group rule's block parses as a style block of the implied
       parent rule; that rule is not a CSSOM rule of its own. */
    bool implicit = pc->cssom_implicit; pc->cssom_implicit = false;
    struct sellist *sl = selector_list_parse(pc, sel_text, sel_text + strlen(sel_text));
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
    struct css_rule_info *outer = pc->cssom_parent;
    struct cssom_run *outer_run = pc->cssom_run, run = {.implicit = implicit};
    if (pc->cssom_tree) {
        if (!implicit) {
            struct css_rule_info *info = cssom_record(pc, CSS_RULE_STYLE, ps, *be == '}' ? be + 1 : be);
            info->native_rule = r; info->selector = sel_text;
            cssom_ranges(info, ps, pe, bs, be);
            pc->cssom_parent = info;
        }
        pc->cssom_run = &run;
    }
    parse_body(pc, bs, be, sel_text, media, &r->decls, &r->ndecls);
    pc->cssom_parent = outer; pc->cssom_run = outer_run;
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

/* A keyframe block's selector list: from, to and percentages, comma
   separated. NULL offsets only validates the list. */
static bool keyframe_offsets(struct pctx *pc,const char *s,const char *p,float *offsets,int cap,int *count) {
    int no=0;const char *q=s;
    if(skip_ws(s,p)==p)return false;
    while(q<p) {
        const char *comma=scan_to(q,p,",");const char *a=skip_ws(q,comma),*z=comma;trim_r(a,&z);
        float off=-1;
        if(z-a==4 && strn_ieq(a,"from",4))off=0;
        else if(z-a==2 && strn_ieq(a,"to",2))off=1;
        else if(z>a && z[-1]=='%') {
            char *text=ar_strndup(pc->a,a,(size_t)(z-a-1)),*tail;double v=strtod(text,&tail);
            if(tail!=text && !*tail && isfinite(v) && v>=0 && v<=100)off=(float)(v/100);
        }
        if(off<0 || (offsets && no==cap))return false;
        if(offsets)offsets[no]=off;
        no++;
        q=comma<p?comma+1:p;
        if(comma<p && q==p)return false;
    }
    *count=no;return true;
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
        float offsets[64];int no=0;
        bool valid=keyframe_offsets(pc,s,p,offsets,64,&no);
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

static void parse_position_try(struct pctx *pc, const char *ns, const char *ne, const char *s, const char *e,
                               const struct mcond *media) {
    const char *cursor = ns, *name = read_ident(pc->a, &cursor, ne, false);
    if (!name || skip_ws(cursor, ne) != ne || name[0] != '-' || name[1] != '-' || !name[2]) return;
    struct decl *decls = NULL;
    int count = 0, kept = 0;
    parse_body(pc, s, e, NULL, NULL, &decls, &count);
    /* only the accepted properties; !important makes a declaration invalid, not the rule */
    for (int i = 0; i < count; i++)
        if (decls[i].p && !decls[i].important && css_prop_position_try_accepted(decls[i].p)) decls[kept++] = decls[i];
    struct position_try_rule *rule = ar_alloc(pc->a, sizeof *rule);
    *rule = (struct position_try_rule){.name = name, .media = media, .layer = pc->layer, .decls = decls, .count = kept};
    if (pc->sh->position_tries_last) pc->sh->position_tries_last->next = rule; else pc->sh->position_tries = rule;
    pc->sh->position_tries_last = rule;
}

/* ---- CSSOM-only grammar: the rules the cascade does not implement still
   belong to the CSSOM tree when their own grammar accepts them. */
static bool cssom_keyword(const char *name,const char *const *words) {
    for(;*words;words++)if(!strcasecmp(name,*words))return true;
    return false;
}
static const char *const css_wide[]={"initial","inherit","unset","revert","revert-layer","default",NULL};
/* <custom-ident>, whole range: no leading digit, no reserved keyword */
static bool cssom_custom_ident(struct pctx *pc,const char *s,const char *e,const char *const *reserved) {
    if(s==e || (*s>='0'&&*s<='9') || (*s=='-'&&s+1<e&&s[1]>='0'&&s[1]<='9'))return false;
    const char *cursor=s,*name=read_ident(pc->a,&cursor,e,false);
    return name && cursor==e && !cssom_keyword(name,css_wide) && !(reserved&&cssom_keyword(name,reserved));
}
static bool cssom_dashed_ident(struct pctx *pc,const char *s,const char *e) {
    const char *cursor=s,*name=e-s>2&&s[0]=='-'&&s[1]=='-'?read_ident(pc->a,&cursor,e,false):NULL;
    return name && cursor==e;
}
/* one whole CSS string token */
static bool cssom_string(const char *s,const char *e) {
    if(e-s<2 || (*s!='"'&&*s!='\'') || e[-1]!=*s)return false;
    for(const char *p=s+1;p<e-1;p++) {
        if(*p=='\\'){if(p+1==e-1)return false;p++;continue;}
        if(*p==*s||*p=='\n')return false;
    }
    return true;
}
static bool cssom_keyframes_name(struct pctx *pc,const char *s,const char *e) {
    static const char *const none[]={"none",NULL};
    return cssom_string(s,e) || cssom_custom_ident(pc,s,e,none);
}
static bool cssom_counter_style_name(struct pctx *pc,const char *s,const char *e) {
    static const char *const fixed[]={"none","decimal","disc","square","circle","disclosure-open","disclosure-closed",NULL};
    return cssom_custom_ident(pc,s,e,fixed);
}
/* [ <ident>? <pseudo-page>* ]# ; an empty list is the page rule for every page */
static bool cssom_page_selectors(struct pctx *pc,const char *s,const char *e) {
    static const char *const pseudo[]={"left","right","first","blank",NULL};
    if(skip_ws(s,e)==e)return true;
    while(s<e) {
        const char *comma=scan_to(s,e,","),*a=skip_ws(s,comma),*z=comma;trim_r(a,&z);
        if(a==z)return false;
        if(*a!=':') {
            const char *cursor=a,*name=read_ident(pc->a,&cursor,z,false);
            if(!name || (*a>='0'&&*a<='9') || cssom_keyword(name,css_wide))return false;
            a=cursor;
        }
        while(a<z) {
            if(*a!=':')return false;
            const char *cursor=a+1,*name=read_ident(pc->a,&cursor,z,true);
            if(!name || !cssom_keyword(name,pseudo))return false;
            a=cursor;
        }
        s=comma<e?comma+1:e;
        if(comma<e && skip_ws(s,e)==e)return false;
    }
    return true;
}
static bool cssom_margin_name(const char *name) {
    static const char *const boxes[]={"top-left-corner","top-left","top-center","top-right","top-right-corner",
        "bottom-left-corner","bottom-left","bottom-center","bottom-right","bottom-right-corner",
        "left-top","left-middle","left-bottom","right-top","right-middle","right-bottom",NULL};
    return name && cssom_keyword(name,boxes);
}
/* the value of a descriptor in a declaration list (the last one), false if there is none; an empty
   value is one (initial-value: ; is the empty value of a universal syntax, csswg-drafts#9078) */
static bool cssom_descriptor(const char *s,const char *e,const char *name,const char **vs,const char **ve) {
    bool found=false;size_t n=strlen(name);
    while(s<e) {
        const char *end=scan_to(s,e,";"),*colon=scan_to(s,end,":"),*a=skip_ws(s,colon),*z=colon;trim_r(a,&z);
        if(colon<end && (size_t)(z-a)==n && strn_ieq(a,name,n)) {
            *vs=skip_ws(colon+1,end);*ve=end;trim_r(*vs,ve);found=true;
        }
        s=end<e?end+1:e;
    }
    return found;
}
enum { SYNTAX_NOT_INDEPENDENT = CSS_SYNTAX_DEP_FONT | CSS_SYNTAX_DEP_ROOT_FONT | CSS_SYNTAX_DEP_LINE_HEIGHT |
       CSS_SYNTAX_DEP_COLOR | CSS_SYNTAX_DEP_CONTAINER | CSS_SYNTAX_DEP_TREE | CSS_SYNTAX_DEP_REFERENCE };
/* a registration's initial value computed against a viewport (§3.3, §4.1 steps 5-6): it parses against
   the syntax and is computationally independent (§4.1: only the property's own value and what CSS cannot
   change: no font-relative, root or container length, no currentcolor, no var() or attr()). A universal
   syntax takes any token stream, or none at all (the guaranteed-invalid value). */
static bool registration_initial(const struct css_syntax *syn, const char *text, int vw, int vh, const char *base,
                                 arena_t *a, const char **out) {
    *out = NULL;
    if (!text) return !syn->count;
    const char *v = text, *end = text + strlen(text);
    v = skip_ws(v, end);
    trim_r(v, &end);
    size_t n = (size_t)(end - v);
    if (css_syntax_dependencies(v, n) & SYNTAX_NOT_INDEPENDENT) return false;
    /* a CSS-wide keyword defaults through the cascade (the parent, other origins and layers, or the very
       initial value being defined): not computationally independent */
    for (const char *const *k = css_wide; *k; k++)
        if (strcmp(*k, "default") && strn_ieq(v, *k, n)) return false; /* default is only reserved */
    if (!syn->count) {
        if (!css_valid_declaration("--x", 3, v, n)) return false;
        *out = ar_strndup(a, v, n);
        return true;
    }
    style_t st;
    css_style_init(&st, NULL);
    struct cx cx = {.s = &st, .em = 16, .rem = 16, .vw = (float)vw, .vh = (float)vh, .a = a,
                    .set = ar_alloc(a, (size_t)css_prop_count())};
    sbuf b = {0};
    unsigned deps = 0;
    bool ok = css_syntax_compute(syn, v, n, &cx, base, NULL, &b, &deps) >= 0 && !(deps & CSS_SYNTAX_DEP_COLOR);
    if (ok) *out = ar_strndup(a, b.p ? b.p : "", b.n);
    sb_free(&b);
    return ok;
}
static bool registration_valid(const char *syntax, const char *initial, const char *base) {
    struct css_syntax *syn = css_syntax_parse(syntax, strlen(syntax), NULL);
    if (!syn) return false;
    arena_t *a = calloc(1, sizeof *a);
    volatile bool ok = false;
    if (a) {
        jmp_buf trap;
        a->trap = &trap;
        a->chunk_size = 1024;
        const char *computed;
        if (!setjmp(trap)) ok = registration_initial(syn, initial, 800, 600, base, a, &computed);
        ar_free(a);
        free(a);
    }
    free(syn);
    return ok;
}
/* css-properties-values-api-1 §3: an @property rule's registration. syntax (a string) and inherits are
   required, and so is initial-value unless the syntax is the universal one (§3.1-3.3, as WPT and the
   engines read the descriptors); the initial value parses against the syntax and is computationally
   independent (§3.3). A rule that is not valid is not a rule (§3: the CSSOM drops it too). */
static bool property_descriptors(struct pctx *pc,const char *s,const char *e,struct property_rule *out) {
    const char *vs,*ve;
    if(!cssom_descriptor(s,e,"inherits",&vs,&ve))return false;
    if(ve-vs==4&&strn_ieq(vs,"true",4))out->inherits=true;
    else if(ve-vs==5&&strn_ieq(vs,"false",5))out->inherits=false;
    else return false;
    if(!cssom_descriptor(s,e,"syntax",&vs,&ve) || !cssom_string(vs,ve))return false;
    sbuf text={0};
    css_unescape(&text,vs+1,(size_t)(ve-vs-2));
    out->syntax=ar_strndup(pc->a,text.p?text.p:"",text.n);
    sb_free(&text);
    out->initial=cssom_descriptor(s,e,"initial-value",&vs,&ve)?ar_strndup(pc->a,vs,(size_t)(ve-vs)):NULL;
    out->base=pc->base;
    return registration_valid(out->syntax,out->initial,out->base);
}
static bool cssom_property_rule(struct pctx *pc,const char *qs,const char *qe,const char *s,const char *e) {
    struct property_rule rule={0};
    return cssom_dashed_ident(pc,qs,qe) && property_descriptors(pc,s,e,&rule);
}
/* a valid rule, in the sheet's order, for the document's registrations (build_ctx) */
static void parse_property_rule(struct pctx *pc,const char *qs,const char *qe,const char *s,const char *e,
                                const struct mcond *media) {
    const char *cursor=qs,*name=qe-qs>2&&qs[0]=='-'&&qs[1]=='-'?read_ident(pc->a,&cursor,qe,false):NULL;
    if(!name || skip_ws(cursor,qe)!=qe)return;
    struct property_rule *rule=ar_alloc(pc->a,sizeof *rule);
    if(!property_descriptors(pc,s,e,rule))return;
    rule->name=name;rule->media=media;
    if(pc->sh->properties_last)pc->sh->properties_last->next=rule;else pc->sh->properties=rule;
    pc->sh->properties_last=rule;
}
static void cssom_keyframe_rules(struct pctx *pc,const char *s,const char *e) {
    while((s=skip_ws(s,e))<e) {
        const char *p=scan_to(s,e,"{;}");if(p==e)break;
        if(*p!='{'){s=p+1;continue;}
        const char *end=block_end(p+1,e);int count;
        if(keyframe_offsets(pc,s,p,NULL,0,&count)) {
            const char *ke=p;trim_r(s,&ke);
            cssom_ranges(cssom_record(pc,CSS_RULE_KEYFRAME,s,end<e?end+1:e),s,ke,p+1,end);
        }
        s=end<e?end+1:e;
    }
}
/* @page declarations with their margin rules */
static void cssom_page_rules(struct pctx *pc,const char *s,const char *e) {
    while((s=skip_ws(s,e))<e) {
        const char *p=scan_to(s,e,"{;}");
        if(p<e && *p=='{' && *s=='@') {
            const char *end=block_end(p+1,e),*ns=s+1,*name=read_ident(pc->a,&ns,p,true);
            if(cssom_margin_name(name) && skip_ws(ns,p)==p)
                cssom_ranges(cssom_record(pc,CSS_RULE_MARGIN,s,end<e?end+1:e),NULL,NULL,p+1,end);
            s=end<e?end+1:e;continue;
        }
        if(p<e && *p=='{'){const char *end=block_end(p+1,e);s=end<e?end+1:e;continue;}
        s=p<e?p+1:e;
    }
}

/* The lists' ends before a top-level rule of a CSSOM parse, and what the
   rule added after them: its span. */
struct span_mark {
    struct rule *rules; struct font_face *fonts; struct opacity_keyframes *motions;
    struct css_layer_event *layers; struct css_rule_info *cssom;
};
static void span_mark(const sheet_t *sh, struct span_mark *m) {
    *m = (struct span_mark){sh->last, sh->fonts_last, sh->motions_last, sh->layers_last, sh->cssom_last};
}
static void span_close(struct pctx *pc, struct span_mark *m) {
    sheet_t *sh = pc->sh;
    struct css_rule_info *rule = sh->cssom_last == m->cssom ? NULL : m->cssom ? m->cssom->next : sh->cssom_first;
    bool added = sh->last != m->rules || sh->fonts_last != m->fonts || sh->motions_last != m->motions ||
                 sh->layers_last != m->layers;
    if (rule && rule == sh->cssom_last) {
        struct css_rule_span *span = ar_alloc(pc->a, sizeof *span);
        if (sh->last != m->rules) { span->rules = m->rules ? m->rules->next : sh->first; span->rules_last = sh->last; }
        if (sh->fonts_last != m->fonts) { span->fonts = m->fonts ? m->fonts->next : sh->fonts; span->fonts_last = sh->fonts_last; }
        if (sh->motions_last != m->motions) {
            span->motions = m->motions ? m->motions->next : sh->motions; span->motions_last = sh->motions_last;
        }
        if (sh->layers_last != m->layers) {
            span->layers = m->layers ? m->layers->next : sh->layers_first; span->layers_last = sh->layers_last;
        }
        rule->span = span;
    } else if (rule || added) sh->cssom_hidden = true;
    span_mark(sh, m);
}
static void parse_rules(struct pctx *pc, const char *s, const char *e, const struct mcond *media) {
    pc->rule_depth++;
    bool top = pc->rule_depth == 1;
    struct span_mark mark;
    bool spans = top && pc->cssom_tree;
    if (spans) span_mark(pc->sh, &mark);
    while (s < e) {
        if (spans) span_close(pc, &mark);
        s = skip_ws(s, e);
        if (s >= e) break;
        if (*s == '@') {
            const char *ns = s + 1;
            const char *name = read_ident(pc->a, &ns, e, true);
            const char *p = scan_to(ns, e, "{;");
            const char *qs = skip_ws(ns, p), *qe = p;
            trim_r(qs, &qe);
            if (p >= e || *p == ';') {
                /* Statements: @import, @namespace and @layer. A block at-rule
                   without its block is invalid. */
                const char *end = p < e ? p + 1 : e;
                if (name && !strcmp(name, "layer")) {
                    if (layer_statement(pc, qs, qe, media)) {
                        cssom_ranges(cssom_record(pc, CSS_RULE_LAYER_STATEMENT, s, end), qs, qe, NULL, NULL);
                        /* No @import after a layer statement that follows an @import. */
                        if (top && pc->imports_seen) pc->import_allowed = false;
                    }
                } else if (name && !strcmp(name, "namespace")) {
                    /* CSS Namespaces: before every rule but @charset, @import and @layer statements */
                    const char *prefix, *uri;
                    if (top && pc->namespace_allowed && namespace_prelude(pc->a, qs, qe, &prefix, &uri)) {
                        struct css_namespace *declared = ar_alloc(pc->a, sizeof *declared), **at = &pc->sh->namespaces;
                        declared->prefix = prefix; declared->uri = uri; declared->next = NULL;
                        while (*at) at = &(*at)->next;
                        *at = declared;
                        cssom_ranges(cssom_record(pc, CSS_RULE_NAMESPACE, s, end), qs, qe, NULL, NULL);
                        pc->import_allowed = false;
                    }
                } else if (name && !strcmp(name, "import") && top && pc->import_allowed) {
                    /* @import <url> [layer | layer(<name>)]? [supports(...)]? <media-query-list>? */
                    const char *ue = scan_to(qs, qe, " \t\n");
                    const char *t = qs;
                    size_t tl = (size_t)(ue - qs);
                    sbuf u = {0};
                    bool url = false;
                    if (tl > 4 && strn_ieq(t, "url(", 4) && t[tl-1] == ')') {
                        const char *a = t + 4, *b = t + tl - 1;
                        a = skip_ws(a, b);
                        trim_r(a, &b);
                        if (b > a && (*a == '"' || *a == '\'')) a++, b--;
                        css_unescape(&u, a, (size_t)(b - a)); url = true;
                    } else if (tl >= 2 && (t[0] == '"' || t[0] == '\'') && cssom_string(t, ue)) {
                        css_unescape(&u, t + 1, tl - 2); url = true;
                    }
                    if (url) {
                        const char *rest = skip_ws(ue, qe), *layer_text = NULL, *supports_text = NULL;
                        size_t layer_n = 0, supports_n = 0;
                        bool ok = u.n > 0;
                        struct css_layer_decl *import_layer=NULL;
                        if((size_t)(qe-rest)>=5 && strn_ieq(rest,"layer",5) &&
                           (rest+5==qe || rest[5]=='(' || is_space((unsigned char)rest[5]))) {
                            const char *after=rest+5;
                            if(after<qe&&*after=='(') {
                                const char *close=scan_to(after+1,qe,")");
                                const char *begin=skip_ws(after+1,close),*end_name=close;trim_r(begin,&end_name);
                                /* An invalid layer() is not a layer: it begins the media query list. */
                                import_layer=close<qe?layer_name_parse(pc,begin,end_name,false):NULL;
                                if(import_layer){layer_text=begin;layer_n=(size_t)(end_name-begin);rest=skip_ws(close+1,qe);}
                            } else {
                                import_layer=layer_name_parse(pc,after,after,true);
                                layer_text="";rest=skip_ws(after,qe);
                            }
                        }
                        if((size_t)(qe-rest)>=9 && strn_ieq(rest,"supports(",9)) {
                            const char *close=scan_to(rest+9,qe,")");
                            bool supported=false,grammar=false;
                            if(close<qe) {
                                const char *in=rest+9;size_t n=(size_t)(close-in);
                                supported=css_supports_condition(in,n,false);grammar=supported||css_supports_grammar(in,n,false);
                                if(!supported){sbuf q={0};sb_putc(&q,'(');sb_put(&q,in,n);sb_putc(&q,')');
                                    supported=css_supports_condition(sb_cstr(&q),q.n,false);
                                    grammar=grammar||supported||css_supports_grammar(sb_cstr(&q),q.n,false);sb_free(&q);}
                            }
                            /* Like layer(), a supports() outside the grammar begins the media list. */
                            if(grammar){
                                const char *in=skip_ws(rest+9,close),*finish=close;trim_r(in,&finish);
                                supports_text=in;supports_n=(size_t)(finish-in);
                                ok&=supported;rest=skip_ws(close+1,qe);
                            }
                        }
                        struct css_rule_info *info = cssom_record(pc, CSS_RULE_IMPORT, s, end);
                        cssom_ranges(info, qs, qe, NULL, NULL);
                        struct css_rule_import *record=info?ar_alloc(pc->a,sizeof *record):NULL;
                        if(record){
                            record->media=rest<qe?ar_strndup(pc->a,rest,(size_t)(qe-rest)):NULL;
                            record->href=ar_strndup(pc->a,u.p?u.p:"",u.n);
                            record->layer=layer_text?ar_strndup(pc->a,layer_text,layer_n):NULL;
                            record->supports=supports_text?ar_strndup(pc->a,supports_text,supports_n):NULL;
                            info->import=record;
                        }
                        const struct mcond *import_condition=rest<qe?parse_condition(pc,rest,qe,media,false):media;
                        if(ok)layer_event(pc,import_layer,import_condition,info);
                        pc->imports_seen = true;
                        /* CSS's url(): UTF-8 against the style sheet's base URL. */
                        char *abs = NULL;
                        int resolved = ok ? web_resolve_url_owned(pc->base, sb_cstr(&u), &abs) : 0;
                        sb_free(&u);
                        if (resolved < 0) ar_alloc(pc->a, SIZE_MAX);
                        if (resolved == 1) {
                            if (record) { record->url = ar_strdup(pc->a, abs); pc->sh->import_urls++; }
                            struct css_import *im = pc->imports ? malloc(sizeof *im) : NULL;
                            if (im) {
                                im->url = abs; abs = NULL;
                                im->order = pc->sh->order - 0.1 + 0.001 * pc->nimports++;
                                im->owner=info;
                                pv_push(pc->imports, im);
                            } else if (pc->imports) { free(abs); ar_alloc(pc->a, SIZE_MAX); }
                        }
                        free(abs);
                    }
                    sb_free(&u);
                }
                s = end;
                continue;
            }
            const char *be = block_end(p + 1, e), *end = be < e ? be + 1 : e;
            const char *bs = p + 1;
            struct css_layer_decl *block_layer=NULL;
            if(name&&!strcmp(name,"layer")) {
                block_layer=layer_name_parse(pc,qs,qe,true);
                if(!block_layer){s=end;continue;}
            }
            bool keyframes = name && (!strcmp(name,"keyframes") || !strcmp(name,"-webkit-keyframes"));
            bool document = name && (!strcmp(name, "document") || !strcmp(name, "-moz-document"));
            uint32_t kind = cssom_group_kind(name);
            if (name && !kind) {
                kind = !strcmp(name,"font-face") ? CSS_RULE_FONT_FACE : keyframes ? CSS_RULE_KEYFRAMES :
                    !strcmp(name,"page") ? CSS_RULE_PAGE : !strcmp(name,"counter-style") ? CSS_RULE_COUNTER_STYLE :
                    !strcmp(name,"property") ? CSS_RULE_PROPERTY : !strcmp(name,"font-feature-values") ? CSS_RULE_FONT_FEATURE_VALUES :
                    !strcmp(name,"font-palette-values") ? CSS_RULE_FONT_PALETTE_VALUES : 0;
            }
            bool supported = kind != CSS_RULE_SUPPORTS || css_supports_condition(qs, (size_t)(p - qs), false);
            if (kind && pc->cssom_tree) {
                /* Each at-rule's own prelude grammar decides whether it exists. */
                bool valid =
                    kind == CSS_RULE_SUPPORTS ? supported || css_supports_grammar(qs, (size_t)(p - qs), false) :
                    kind == CSS_RULE_CONTAINER ? qs < qe :
                    kind == CSS_RULE_FONT_FACE || kind == CSS_RULE_STARTING_STYLE ? qs == qe :
                    kind == CSS_RULE_KEYFRAMES ? cssom_keyframes_name(pc, qs, qe) :
                    kind == CSS_RULE_PAGE ? cssom_page_selectors(pc, qs, qe) :
                    kind == CSS_RULE_COUNTER_STYLE ? cssom_counter_style_name(pc, qs, qe) :
                    kind == CSS_RULE_PROPERTY ? cssom_property_rule(pc, qs, qe, bs, be) :
                    kind == CSS_RULE_FONT_PALETTE_VALUES ? cssom_dashed_ident(pc, qs, qe) :
                    kind == CSS_RULE_FONT_FEATURE_VALUES ? qs < qe : true;
                if (!valid) kind = 0;
            } else if (!pc->cssom_tree) kind = 0;
            struct css_rule_info *info = kind ? cssom_record(pc, kind, s, end) : NULL;
            cssom_ranges(info, qs, qe, bs, be);
            if (top && (info || cssom_group_kind(name) || keyframes || document ||
                        (name && (!strcmp(name,"font-face") || !strcmp(name,"page"))))) {
                pc->import_allowed = false; pc->namespace_allowed = false;
            }
            struct css_rule_info *outer = pc->cssom_parent;
            if (info) pc->cssom_parent = info;
            if (name) {
                if (!strcmp(name, "font-face")) parse_font_face(pc,bs,be,media);
                else if (keyframes) {
                    parse_opacity_keyframes(pc,qs,qe,bs,be,media);
                    if (info) cssom_keyframe_rules(pc,bs,be);
                }
                else if (!strcmp(name, "position-try")) parse_position_try(pc, qs, qe, bs, be, media);
                else if (!strcmp(name, "property")) parse_property_rule(pc, qs, qe, bs, be, media);
                else if (!strcmp(name,"media") || !strcmp(name,"container")) {
                    const struct mcond *mc=parse_condition(pc,qs,qe,media,!strcmp(name,"container"));
                    parse_rules(pc,bs,be,mc);
                } else if (!strcmp(name, "supports")) {
                    if (supported) parse_rules(pc, bs, be, media);
                    else if (info) parse_rules(pc, bs, be, cssom_inert(pc, media));
                } else if (!strcmp(name,"layer")) {
                    struct css_layer_decl *previous=pc->layer;pc->layer=block_layer;
                    layer_event(pc,block_layer,media,NULL);parse_rules(pc,bs,be,media);pc->layer=previous;
                } else if (!strcmp(name, "scope")) {
                    parse_rules(pc, bs, be, media);
                } else if (document) {
                    /* Legacy: the cascade applies its rules; it is no CSSOM rule. */
                    bool tree = pc->cssom_tree;
                    pc->cssom_tree = false;
                    parse_rules(pc, bs, be, media);
                    pc->cssom_tree = tree;
                } else if (info && kind == CSS_RULE_STARTING_STYLE) {
                    parse_rules(pc, bs, be, cssom_inert(pc, media));
                } else if (info && kind == CSS_RULE_PAGE) cssom_page_rules(pc, bs, be);
                /* @page, @counter-style ... have no cascade effect yet */
            }
            pc->cssom_parent = outer;
            s = end;
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
        if (native && top) pc->import_allowed = pc->namespace_allowed = false;
        s = be < e ? be + 1 : e;
    }
    if (spans) span_close(pc, &mark);
    pc->rule_depth--;
}

/* comments out, <!-- --> out; CSS Syntax preprocessing turns NUL into U+FFFD */
static char *strip_comments(arena_t *a, const char *css, size_t n, size_t *out_n) {
    size_t nuls = 0;
    for (size_t i = 0; i < n; i++) nuls += !css[i];
    if (nuls > (SIZE_MAX - 1 - n) / 2) ar_alloc(a, SIZE_MAX);
    char *o = ar_alloc(a, n + 2 * nuls + 1);
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
                if (css[i] == '\\' && i + 1 < n && css[i + 1]) o[k++] = css[i++];
                if (!css[i]) { memcpy(o + k, "\xEF\xBF\xBD", 3); k += 3; i++; continue; }
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
        if (c) o[k++] = c;
        else { memcpy(o + k, "\xEF\xBF\xBD", 3); k += 3; }
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
const char *css_sheet_source(const sheet_t *sheet, size_t *length) {
    if (length) *length = sheet ? sheet->cssom_length : 0;
    return sheet ? sheet->cssom_source : NULL;
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

/* an AST's identity across rescans: never reused, unlike its address */
static uint64_t sheet_serials;
/* context: the sheet the text's rules are parsed for (css_parse_cssom_rules) */
static sheet_t *parse_sheet(arena_t *a, const char *css, size_t n, const char *base_url, double order, pvec *imports,
                            bool cssom_tree, const uint32_t *markers, uint32_t nmarkers, sheet_t *context) {
    /* the sheet keeps its URL: custom property declarations resolve registered <url>s against it */
    struct pctx pc = {.a = a, .base = base_url ? ar_strdup(a, base_url) : NULL, .imports = imports, .import_allowed = !context,
                      .namespace_allowed = !context, .cssom_tree = cssom_tree,
                      .cssom_markers = markers, .cssom_nmarkers = nmarkers};
    pc.sh = ar_alloc(a, sizeof(sheet_t));
    pc.sh->order = order;
    pc.sh->ast = pc.sh; pc.sh->arena = a;
    pc.sh->serial = __atomic_add_fetch(&sheet_serials, 1, __ATOMIC_RELAXED);
    if (context) {
        /* its prefixes, read only: no @namespace parses here */
        pc.sh->ast = context; pc.sh->order = context->order;
        pc.sh->namespaces = context->namespaces; pc.sh->layer_count = context->layer_count;
    }
    size_t cn;
    char *c = strip_comments(a, css, n, &cn);
    pc.sh->cssom_source = pc.cssom_text = c; pc.sh->cssom_length = cn;
    parse_rules(&pc, c, c + cn, NULL);
    return pc.sh;
}
sheet_t *css_parse_sheet(arena_t *a, const char *css, size_t n, const char *base_url, double order, pvec *imports) {
    return parse_sheet(a, css, n, base_url, order, imports, false, NULL, 0, NULL);
}

struct sheet_parse_job {
    arena_t *arena;
    const char *text, *base;
    size_t length;
    double order;
    pvec *imports;
    sheet_t *result;
    int failed;
    bool cssom_tree;
    const uint32_t *markers;
    uint32_t nmarkers;
    sheet_t *context;
};
static void sheet_parse_worker(size_t index, void *context) {
    (void)index;
    struct sheet_parse_job *job = context;
    parallel_record_work(PARALLEL_CSS_PARSE, job->length);
    jmp_buf trap; jmp_buf *outer = job->arena->trap;
    job->arena->trap = &trap;
    job->failed = setjmp(trap);
    if (!job->failed) job->result = parse_sheet(job->arena, job->text, job->length, job->base, job->order, job->imports, job->cssom_tree,
                                                    job->markers, job->nmarkers, job->context);
    job->arena->trap = outer;
}
static void sheet_parse_service(void *context) { web_native_checkpoint(context); }
static sheet_t *parse_serviced(web_doc *d, arena_t *a, const char *css, size_t n, const char *base_url, double order,
                               pvec *imports, bool cssom_tree, const uint32_t *markers, uint32_t nmarkers, sheet_t *context) {
    if (!d || !d->live || n < 16384 || parallel_active())
        return parse_sheet(a,css,n,base_url,order,imports,cssom_tree,markers,nmarkers,context);
    struct sheet_parse_job job = {a,css,base_url,n,order,imports,NULL,0,cssom_tree,markers,nmarkers,context};
    parallel_call_stage(PARALLEL_CSS_PARSE,sheet_parse_worker,&job,sheet_parse_service,d);
    if (job.failed) {
        if (a->trap) longjmp(*a->trap,job.failed);
        ar_alloc(a,SIZE_MAX); return NULL;
    }
    return job.result;
}
sheet_t *css_parse_sheet_serviced(web_doc *d, arena_t *a, const char *css, size_t n, const char *base_url, double order, pvec *imports) {
    return parse_serviced(d, a, css, n, base_url, order, imports, false, NULL, 0, NULL);
}
sheet_t *css_parse_cssom_sheet(web_doc *d, arena_t *a, const char *css, size_t n, const char *base_url,
                               const uint32_t *markers, uint32_t nmarkers) {
    return parse_serviced(d, a, css, n, base_url, 0, NULL, true, markers, nmarkers, NULL);
}
sheet_t *css_parse_cssom_rules(web_doc *d, sheet_t *sheet, const char *css, size_t n, const char *base_url,
                               const uint32_t *markers, uint32_t nmarkers) {
    return parse_serviced(d, sheet->arena, css, n, base_url, 0, NULL, true, markers, nmarkers, sheet);
}
bool css_cssom_spliceable(const sheet_t *sheet) { return sheet && !sheet->cssom_hidden; }

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
            const char *p=skip_ws(s,end),*z=end;trim_r(p,&z);bool space=false;size_t query=used;
            for(;p<z;p++){
                if(is_space((unsigned char)*p)){space=true;continue;}
                char c=*p,prev=out&&used?out[used-1]:0;
                bool before=c=='<'||c=='>'||c=='='||c=='/'||(space&&c!=')'&&c!=':'&&prev!='(');
                if(out&&used&&prev!=' '&&before&&!(c=='='&&(prev=='<'||prev=='>')))ok=ok&&mq_append(out,cap,&used," ",1);
                ok=ok&&mq_append(out,cap,&used,&c,1);space=false;
                if(c==':'||c=='/'||c=='='||((c=='<'||c=='>')&&(p+1==z||p[1]!='=')))ok=ok&&mq_append(out,cap,&used," ",1);
            }
            /* CSSOM "serialize a media query": the type "all" with features is implied */
            if(out&&used-query>8&&!memcmp(out+query,"all and ",8)){memmove(out+query,out+query+8,used-query-8+1);used-=8;}
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
    struct ient *parts; /* ::part() rules: they select elements of other trees */
    struct font_face *fonts;
    struct opacity_keyframes *motions;
    struct position_try_rule *position_tries;
    struct property_rule *properties;
    uint32_t rules;
    unsigned form_features;
    bool containers, siblings;
    const struct mcond **media; /* the distinct @media of its rules */
    uint32_t nmedia, media_cap;
    const struct mcond *last_media; /* the last rule's, at media[last_media_id - 1] */
    uint32_t last_media_id;
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
    /* the document's custom property registrations by name (struct registered); NULL: none */
    struct var_slot *registry;
    size_t registry_capacity;
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
    for (int i = 0; s->pseudo != PE_SLOTTED && !s->part && i < c->n; i++) {
        if (c->s[i].kind == SK_ID && !id) id = c->s[i].name;
        else if (c->s[i].kind == SK_CLASS && !cls) cls = c->s[i].name;
        else if (c->s[i].kind == SK_TAG) tag = c->s[i].name;
    }
    struct ient **slot = s->part ? &x->parts
                       : id ? ht_slot(a, &x->ids, id, true)
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
    if (s->pc == PC_FOCUS || s->pc == PC_FOCUS_WITHIN || s->pc == PC_FOCUS_VISIBLE) return CSS_STATE_FOCUS;
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
static void index_features(arena_t *a, struct css_index *x, const struct selector *sel) {
    const struct compound *right = sel->right;
    struct feature_key key;
    /* ::part(): the subject is in a shadow tree below the host the right
       compound names, out of a keyed walk's reach */
    bool keyed = !sel->part && subject_key(right, &key);
    unsigned reach = sel->part ? REACH_SUBTREE : REACH_SELF;
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
/* a rule after every rule the index holds */
static void index_add(web_doc *d, arena_t *a, struct css_index *index, const struct rule *r) {
    for(const struct mcond *m=r->media;m;m=m->up)if(m->container)index->containers=true;
    for(int k=0;k<r->ndecls;k++)if(container_units_used(r->decls[k].value))index->containers=true;
    if (r->ndecls) {
        /* The rules of one @media block share its condition and are adjacent. */
        if (r->media != index->last_media) {
            index->last_media = r->media; index->last_media_id = r->media ? index_media(a, index, r->media) : 0;
        }
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
            index_rule(a, index, r, r->sel->v[k], index->rules, index->last_media_id);
            index_features(a, index, r->sel->v[k]);
            if (d && d->profile_enabled) d->profile.style_index_entries++;
        }
    }
    index->rules++;
}
/* NULL when cancelled: a partial index is never published to the AST. */
static struct css_index *index_sheet(web_doc *d, sheet_t *s) {
    arena_t *a = s->arena;
    struct css_index *index = ar_alloc(a, sizeof *index);
    index->fonts = s->fonts; index->motions = s->motions; index->position_tries = s->position_tries;
    index->properties = s->properties;
    /* Small component sheets no longer allocate a document-sized hash
       table, nor duplicate selector entries for every adoption. */
    ht_init(a, &index->ids, 64);
    ht_init(a, &index->classes, 128);
    ht_init(a, &index->tags, 64);
    ht_init(a, &index->feature_ids, 32);
    ht_init(a, &index->feature_classes, 128);
    ht_init(a, &index->feature_attrs, 32);
    for (struct rule *r = s->first; r; r = r->next) {
        if (!web_native_checkpoint(d)) return NULL;
        index_add(d, a, index, r);
    }
    return index;
}
/* One list of an AST with a top-level rule's part replaced: after the last
   element an earlier top-level rule put there (pred), before what followed
   the old part (succ). */
#define SPLICE_LIST(T, head, tail, first, last, moved)                                                    \
    do {                                                                                                  \
        T *gone = old && old->span ? old->span->first : NULL, *gone_last = gone ? old->span->last : NULL; \
        T *in = put ? put->first : NULL, *in_last = in ? put->last : NULL;                                \
        if (!gone && !in) break;                                                                          \
        T *pred = NULL;                                                                                   \
        if (at == count && !old) pred = sheet->tail;                                                      \
        else for (uint32_t k = at; k-- > 0;)                                                              \
            if (top[k]->span && top[k]->span->last) { pred = top[k]->span->last; break; }                 \
        T *succ = gone ? gone_last->next : pred ? pred->next : sheet->head;                               \
        if (pred) pred->next = in ? in : succ; else sheet->head = in ? in : succ;                         \
        if (in) in_last->next = succ;                                                                     \
        if (!succ) sheet->tail = in ? in_last : pred;                                                     \
        moved = moved || gone || succ;                                                                    \
    } while (0)
bool css_cssom_splice(web_doc *d, sheet_t *sheet, struct css_rule_info *const *top, uint32_t count, uint32_t at,
                      struct css_rule_info *old, sheet_t *fresh) {
    struct css_rule_info *rule = fresh ? fresh->cssom_first : NULL;
    const struct css_rule_span *put = rule ? rule->span : NULL;
    /* moved: style rules other than after all others, which the index orders */
    bool moved = false, others = false;
    SPLICE_LIST(struct rule, first, last, rules, rules_last, moved);
    SPLICE_LIST(struct font_face, fonts, fonts_last, fonts, fonts_last, others);
    SPLICE_LIST(struct opacity_keyframes, motions, motions_last, motions, motions_last, others);
    SPLICE_LIST(struct css_layer_event, layers_first, layers_last, layers, layers_last, others);
    (void)others;
    struct css_rule_info *prev = at ? top[at - 1] : NULL, *next = old ? old->next : at < count ? top[at] : NULL;
    if (rule) rule->next = next;
    if (prev) prev->next = rule ? rule : next; else sheet->cssom_first = rule ? rule : next;
    if (!next) sheet->cssom_last = rule ? rule : prev;
    sheet->cssom_count = sheet->cssom_count - (old != NULL) + (rule != NULL);
    if (fresh && fresh->layer_count > sheet->layer_count) sheet->layer_count = fresh->layer_count;
    sheet->serial = __atomic_add_fetch(&sheet_serials, 1, __ATOMIC_RELAXED);
    struct css_index *index = sheet->index;
    if (!index) return false;
    /* Rules after all others take the next places of its order; any other
       change to the rules leaves an index to build again. */
    if (moved) { sheet->index = NULL; return true; }
    for (const struct rule *r = put ? put->rules : NULL; r; r = r == put->rules_last ? NULL : r->next)
        index_add(d, sheet->arena, index, r);
    index->fonts = sheet->fonts; index->motions = sheet->motions;
    return false;
}
#undef SPLICE_LIST
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

static void registry_build(web_doc *d, struct css_ctx *x, const struct css_index **sheets, int nsheets, int vw, int vh,
                           bool scripting);
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
    const struct css_index **registering = sh->n ? ar_alloc(&x->a, (size_t)sh->n * sizeof *registering) : NULL;
    int nregistering = 0;
    for (int i = -2; i < sh->n; i++) {
        sheet_t *s = i == -2 ? ua_sheet : i == -1 ? quirks_sheet : sh->v[i];
        if (!sheet_media_ok(s,vw,vh,scripting)) continue;
        if (i == -1 && !d->quirks) continue;
        struct css_index *index = sheet_index(d, s);
        if (!index) return x;
        if (i >= 0 && index->properties) registering[nregistering++] = index;
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
    registry_build(d, x, registering, nregistering, vw, vh, scripting);
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
        style_t *styles[]={s,s->before_generated?s->before:NULL,s->after_generated?s->after:NULL,s->backdrop,s->marker,base,
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

/* HTML's rules for parsing integers: false for an error (no digits, or beyond an int32: CSS
   clamps the integers of the hints above, which may step one past it) */
static bool html_integer(const char *s, int64_t *out) {
    if (!s) return false;
    while (*s == ' ' || *s == '\t' || *s == '\n' || *s == '\f' || *s == '\r') s++;
    bool negative = *s == '-';
    if (*s == '-' || *s == '+') s++;
    if (*s < '0' || *s > '9') return false;
    int64_t value = 0;
    while (*s >= '0' && *s <= '9') {
        value = value * 10 + (*s++ - '0');
        if (value > 2147483648ll) return false;
    }
    if (!negative && value > 2147483647ll) return false;
    *out = negative ? -value : value;
    return true;
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
    if (!e->foreign && (t == T_ul || t == T_ol || t == T_li) && (v = node_attr(e, "type")) && *v) {
        /* ol[type=1], li[type=a s] ...: case-sensitive; ul[type=disc i] ...: not */
        const char *ls = NULL;
        bool ordered = t != T_ul, unordered = t != T_ol;
        if (ordered && !strcmp(v, "1")) ls = "decimal";
        else if (ordered && !strcmp(v, "a")) ls = "lower-alpha";
        else if (ordered && !strcmp(v, "A")) ls = "upper-alpha";
        else if (ordered && !strcmp(v, "i")) ls = "lower-roman";
        else if (ordered && !strcmp(v, "I")) ls = "upper-roman";
        else if (unordered && (str_ieq(v, "none") || str_ieq(v, "disc") || str_ieq(v, "circle") || str_ieq(v, "square"))) ls = v;
        if (ls) hint(h, "list-style-type", "%s", ls);
    }
    /* li's value is counter-set: list-item value; ol's start and reversed are its counter-reset */
    int64_t number;
    if (!e->foreign && t == T_li && html_integer(node_attr(e, "value"), &number))
        hint(h, "counter-set", "list-item %lld", (long long)number);
    if (!e->foreign && t == T_ol) {
        bool start = html_integer(node_attr(e, "start"), &number);
        if (node_attr(e, "reversed")) {
            if (start) hint(h, "counter-reset", "reversed(list-item) %lld", (long long)number + 1);
            else hint(h, "counter-reset", "reversed(list-item)");
        } else if (start) hint(h, "counter-reset", "list-item %lld", (long long)number - 1);
    }
    if ((t == T_caption) && (v = node_attr(e, "valign")) && str_ieq(v, "bottom")) {
    }
}
/* The attributes of e whose hints above belong to its descendants: a table's
   cellpadding and border pad and border its cells. */
static bool hint_reaches_descendants(const node_t *e, const char *name) {
    return !e->foreign && e->tag == T_table && (!strcmp(name, "cellpadding") || !strcmp(name, "border"));
}

/* ---------------------------------------------------------------- an element's inline declarations */
const char *css_inline_block(const node_t *e) {
    const char *attribute = node_attr(e, "style");
    if (!e->style_cssom || !attribute || strcmp(e->style_cssom, attribute)) return NULL;
    return e->style_cssom + strlen(e->style_cssom) + 1;
}
const char *css_inline_text(const node_t *e) {
    /* a style attribute CSP blocked gives no declarations (dom.c's csp_style_attribute) */
    if (e->csp_style_attribute == 2) return NULL;
    const char *block = css_inline_block(e);
    return block ? block : node_attr(e, "style");
}
/* the declarations (a block's text) stand for the attribute once it is set to
   attribute, their serialization; an element whose attribute does not change (the
   same serialization) restyles for the new values here */
static bool inline_store(web_doc *d, node_t *e, const sbuf *attribute, const sbuf *block) {
    char *text = malloc(attribute->n + block->n + 2);
    if (!text) return false;
    memcpy(text, attribute->p ? attribute->p : "", attribute->n);
    text[attribute->n] = 0;
    memcpy(text + attribute->n + 1, block->p ? block->p : "", block->n);
    text[attribute->n + 1 + block->n] = 0;
    const char *before = css_inline_text(e);
    bool same = before && strlen(before) == block->n && !memcmp(before, text + attribute->n + 1, block->n);
    free(e->style_cssom);
    e->style_cssom = text;
    if (!same) css_mark_style_own(d, e);
    return true;
}
int css_inline_set(web_doc *d, node_t *e, const char *block, size_t n, const char *property, const char *value,
                   const char *priority, sbuf *attribute) {
    sbuf changed = {0};
    int status = css_decl_set(block, n, property, value, priority, NULL, &changed);
    /* CSSOM "set a CSS declaration": the declaration it already has changes nothing */
    if (status > 0 && changed.n == n && (!n || !memcmp(changed.p, block, n))) status = 0;
    if (status > 0 && !(css_decl_block_serialize(changed.p ? changed.p : "", changed.n, NULL, attribute, NULL) &&
                        inline_store(d, e, attribute, &changed))) status = -2;
    sb_free(&changed);
    return status;
}
bool css_inline_replace(web_doc *d, node_t *e, const char *text, size_t n, sbuf *attribute) {
    sbuf block = {0};
    size_t clean_n;
    char *clean = css_preprocess(text, n, &clean_n);
    bool ok = clean && css_decl_block(clean, clean_n, NULL, &block) &&
              css_decl_block_serialize(block.p ? block.p : "", block.n, NULL, attribute, NULL) &&
              inline_store(d, e, attribute, &block);
    free(clean);
    sb_free(&block);
    return ok;
}

/* ---------------------------------------------------------------- the cascade */
struct dent {
    const struct decl *d;
    uint64_t key;
    uint8_t pseudo;
    int scope_depth;
    bool animation;  /* the animation or transition origin (CSS Cascade 5 §6.1) */
    bool transition; /* the transition origin, above every other */
    bool position_try; /* the position fallback origin, between the author and animation origins
                          (css-anchor-position-1 §6.4) */
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
    dents[ndents].animation = dents[ndents].transition = dents[ndents].position_try = false;
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
        if (!binding->ua && !l->sel->part) { /* match_part() checks the trees of a ::part() */
            if (!scope && root && root->shadow_host) continue;
            if (scope && l->pseudo != PE_SLOTTED && root != scope && e != scope->shadow_host) continue;
        }
        if (!binding->media_ok[l->media]) continue;
        if (ancestor_filter_rejects(&k->ancestors, l->ancestor_hashes)) { tally->filtered++; continue; }
        if (!container_chain_ok(l->r->media,e,l->pseudo==PE_BEFORE || l->pseudo==PE_AFTER || l->pseudo==PE_MARKER)) continue;
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
/* ::part(): an element of a shadow tree with part names, from its own tree
   (:host::part()) out to the document's. */
static void collect_parts(const struct element_keys *k) {
    node_t *root = k->root;
    if (!root || !root->shadow_host || !node_attr(k->e, "part")) return;
    for (; root; root = root->shadow_host ? doc_node_root(root->shadow_host, false) : NULL) {
        if (!root->style_scope) continue;
        node_t *scope = root->shadow_host ? root : NULL;
        struct collect_tally tally = {0};
        for (struct css_binding *binding = root->style_scope->first; binding; binding = binding->next)
            collect(binding->index->parts, k, scope, scope_depth(scope), binding, &tally, COLLECT_UNIVERSAL);
    }
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
    /* The position fallback origin likewise: above normal declarations, below important ones. */
    if (da->position_try != db->position_try) {
        const struct dent *normal = da->position_try ? db : da;
        bool tried_first = !normal->d->important;
        return da->position_try == tried_first ? -1 : 1;
    }
    if (da->position_try) return da->key < db->key ? 1 : da->key > db->key ? -1 : 0;
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

/* ---------------------------------------------------------------- registered custom properties
   css-properties-values-api-1 §2.1: a document's registration of a name is its [[registeredPropertySet]]
   record (CSS.registerProperty, kept for the document's life), else the last valid @property in its
   active sheets in document order, shadow trees' included (§2.8). build_ctx gathers them into the
   context with the viewport their initial values compute against; a cascade reads them by name. */
struct registered {
    struct css_registration r;
    struct custom_prop initial; /* r.initial as the entry var() and the computed style read */
};
struct css_script_registration {
    struct css_script_registration *next;
    char *name, *syntax_text, *initial; /* initial: comments removed, escapes canonical, closed at its end */
    struct css_syntax *syntax;
    bool inherits;
};
static const struct registered *registry_get(const struct css_ctx *x, const char *name, size_t n) {
    return x && x->registry ? var_slot_get(x->registry, x->registry_capacity, name, n) : NULL;
}
const struct css_registration *css_registration(web_doc *d, const char *name, size_t n) {
    const struct registered *r = d && name ? registry_get(d->sty.ctx, name, n) : NULL;
    return r ? &r->r : NULL;
}
static void registry_put(struct css_ctx *x, const char *name, const struct css_syntax *syntax, bool inherits,
                         const char *initial, const char *base, int vw, int vh) {
    if (var_slot_get(x->registry, x->registry_capacity, name, strlen(name))) return; /* an earlier one wins */
    struct registered *r = ar_alloc(&x->a, sizeof *r);
    r->r.name = ar_strdup(&x->a, name);
    r->r.syntax = syntax;
    r->r.inherits = inherits;
    if (!registration_initial(syntax, initial, vw, vh, base, &x->a, &r->r.initial)) return;
    r->initial.name = r->r.name;
    r->initial.value = r->r.initial;
    var_slot_put(x->registry, x->registry_capacity, r->r.name, strlen(r->r.name), r);
}
static void registry_build(web_doc *d, struct css_ctx *x, const struct css_index **sheets, int nsheets, int vw, int vh,
                           bool scripting) {
    size_t count = 0;
    for (struct css_script_registration *s = d->sty.registered; s; s = s->next) count++;
    for (int i = 0; i < nsheets; i++)
        for (const struct property_rule *r = sheets[i]->properties; r; r = r->next) count++;
    if (!count) return;
    x->registry_capacity = var_slot_capacity(count);
    x->registry = ar_alloc(&x->a, x->registry_capacity * sizeof *x->registry);
    /* the first put of a name is kept: the scripts' first, then the rules from the last back */
    for (struct css_script_registration *s = d->sty.registered; s; s = s->next)
        registry_put(x, s->name, s->syntax, s->inherits, s->initial, NULL, vw, vh);
    const struct property_rule **rules = ar_alloc(&x->a, count * sizeof *rules);
    size_t n = 0;
    for (int i = 0; i < nsheets; i++)
        for (const struct property_rule *r = sheets[i]->properties; r; r = r->next)
            if (font_conditions_ok(r->media, vw, vh, scripting)) rules[n++] = r;
    while (n--) {
        const struct property_rule *r = rules[n];
        if (var_slot_get(x->registry, x->registry_capacity, r->name, strlen(r->name))) continue;
        struct css_syntax *syntax = css_syntax_parse(r->syntax, strlen(r->syntax), &x->a);
        if (syntax) registry_put(x, r->name, syntax, r->inherits, r->initial, r->base, vw, vh);
    }
}
/* text as a whole value: its comments removed, its escapes canonical and what it leaves open closed at its
   end (css-syntax-3: an EOF ends a string or a block) */
static char *registration_text(const char *text) {
    size_t n;
    char *stripped = css_preprocess(text, strlen(text), &n);
    if (!stripped) return NULL;
    arena_t a = {.chunk_size = 1024};
    jmp_buf trap;
    a.trap = &trap;
    char *volatile out = NULL;
    if (!setjmp(trap)) {
        size_t cn;
        const char *v = css_value_canonical(&a, stripped, n, &cn);
        sbuf b = {0}, closers = {0};
        char quote = 0;
        for (size_t i = 0; i < cn; i++) {
            char c = v[i];
            if (c == '\\' && i + 1 < cn) { i++; continue; }
            if (quote) { if (c == quote) quote = 0; continue; }
            if (c == '"' || c == '\'') quote = c;
            else if (c == '(' || c == '[' || c == '{') sb_putc(&closers, c == '(' ? ')' : c == '[' ? ']' : '}');
            else if ((c == ')' || c == ']' || c == '}') && closers.n && closers.p[closers.n - 1] == c) closers.n--;
        }
        sb_put(&b, v, cn);
        if (quote) sb_putc(&b, quote);
        while (closers.n) sb_putc(&b, closers.p[--closers.n]);
        out = malloc(b.n + 1);
        if (out) { memcpy(out, b.p ? b.p : "", b.n); out[b.n] = 0; }
        sb_free(&b);
        sb_free(&closers);
    }
    ar_free(&a);
    free(stripped);
    return out;
}
/* §4.1 registerProperty(): the name is a custom property name, not registered by script already; the
   syntax parses; an initial value is given unless the syntax is universal, and it parses against the
   syntax and is computationally independent */
int css_register_property(web_doc *d, const char *name, const char *syntax, bool inherits, const char *initial) {
    if (!d || !name || !syntax) return CSS_REGISTER_SYNTAX;
    if (strlen(name) <= 2 || name[0] != '-' || name[1] != '-') return CSS_REGISTER_SYNTAX;
    for (struct css_script_registration *s = d->sty.registered; s; s = s->next)
        if (!strcmp(s->name, name)) return CSS_REGISTER_DUPLICATE;
    struct css_syntax *parsed = css_syntax_parse(syntax, strlen(syntax), NULL);
    if (!parsed) return CSS_REGISTER_SYNTAX;
    char *text = initial ? registration_text(initial) : NULL;
    if (initial && !text) { free(parsed); return CSS_REGISTER_MEMORY; }
    struct css_script_registration *r = calloc(1, sizeof *r);
    if (r) { r->name = strdup(name); r->syntax_text = strdup(syntax); }
    if (!r || !r->name || !r->syntax_text) {
        if (r) { free(r->name); free(r->syntax_text); free(r); }
        free(parsed); free(text);
        return CSS_REGISTER_MEMORY;
    }
    if (!registration_valid(syntax, text, NULL)) {
        free(r->name); free(r->syntax_text); free(r); free(parsed); free(text);
        return CSS_REGISTER_SYNTAX;
    }
    r->syntax = parsed;
    r->initial = text;
    r->inherits = inherits;
    r->next = d->sty.registered;
    d->sty.registered = r;
    /* every element's values of the name may change (§2.4): the registrations are the context's */
    d->sty.index_dirty = true;
    css_mark_dirty(d, NULL);
    return CSS_REGISTER_OK;
}
/* the document's end: its [[registeredPropertySet]] outlives sheet invalidation (css_styling_free) */
void css_registrations_free(struct styling *st) {
    while (st->registered) {
        struct css_script_registration *r = st->registered;
        st->registered = r->next;
        free(r->name); free(r->syntax_text); free(r->initial); free(r->syntax); free(r);
    }
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
/* the element's own entry of a name: the entries before own_end (one segment indexes them all) */
static const struct custom_prop *own_find(const struct custom_prop *vars, const struct custom_prop *own_end,
                                          const char *name, size_t n) {
    if (!vars || vars == own_end) return NULL;
    if (vars->segment) return var_slot_get(vars->segment->slots, vars->segment->capacity, name, n);
    for (const struct custom_prop *v = vars; v && v != own_end; v = v->next)
        if (strlen(v->name) == n && !memcmp(v->name, name, n)) return v;
    return NULL;
}
/* a name's computed value on an element whose chain is vars (own_end: where its own entries end), its
   registration applied (§2.1, §2.4): a registered property that does not inherit takes the element's
   own value only, and a registered one without a value has its initial value */
static const struct custom_prop *custom_find(const struct css_ctx *x, const struct custom_prop *vars,
                                             const struct custom_prop *own_end, const char *name, size_t n) {
    const struct registered *reg = registry_get(x, name, n);
    const struct custom_prop *found = reg && !reg->r.inherits ? own_find(vars, own_end, name, n) : find_var(vars, name, n);
    return found ? found : reg ? &reg->initial : NULL;
}
const char *css_custom_value(web_doc *d, const style_t *st, const char *name) {
    if (!st || !name) return NULL;
    const struct custom_prop *p = custom_find(d ? d->sty.ctx : NULL, st->vars, st->vars_inherited, name, strlen(name));
    return p ? p->value : NULL;
}
bool css_custom_values(web_doc *d, const style_t *st, bool (*each)(void *arg, const char *name, const char *value),
                       void *arg) {
    const struct css_ctx *x = d ? d->sty.ctx : NULL;
    size_t count = 0;
    for (const struct custom_prop *p = st ? st->vars : NULL; p; p = p->next) count++;
    size_t capacity = var_slot_capacity(count + 1);
    struct var_slot *seen = calloc(capacity, sizeof *seen);
    if (!seen) return false;
    bool own = true, ok = true;
    for (const struct custom_prop *p = st ? st->vars : NULL; p && ok; p = p->next) {
        if (p == st->vars_inherited) own = false;
        size_t n = strlen(p->name);
        if (var_slot_get(seen, capacity, p->name, n)) continue;
        var_slot_put(seen, capacity, p->name, n, (void *)p);
        const struct registered *reg = registry_get(x, p->name, n);
        if (reg && !reg->r.inherits && !own) continue; /* its initial value, below */
        ok = each(arg, p->name, p->value);
    }
    /* the registered properties the element has no entry of take their initial values */
    if (ok && x && x->registry)
        for (size_t i = 0; i < x->registry_capacity && ok; i++) {
            const struct registered *reg = x->registry[i].value;
            if (!reg) continue;
            const struct custom_prop *p = var_slot_get(seen, capacity, reg->r.name, strlen(reg->r.name));
            bool own_entry = p && (!st || own_find(st->vars, st->vars_inherited, reg->r.name, strlen(reg->r.name)));
            if ((p && (reg->r.inherits || own_entry)) || !reg->r.initial) continue;
            ok = each(arg, reg->r.name, reg->r.initial);
        }
    free(seen);
    return ok;
}
/* a computed value is its own computed value (canonical units, resolved colours and URLs), so it matches
   the component its specified value matched: the first one that takes it (§5.3) */
const struct css_syntax_component *css_custom_component(web_doc *d, const char *name, const char *value) {
    const struct registered *reg = d && name && value ? registry_get(d->sty.ctx, name, strlen(name)) : NULL;
    if (!reg || !reg->r.syntax->count) return NULL;
    arena_t *a = calloc(1, sizeof *a);
    if (!a) return NULL;
    volatile int k = -1;
    jmp_buf trap;
    a->trap = &trap;
    a->chunk_size = 1024;
    if (!setjmp(trap)) {
        style_t st;
        css_style_init(&st, NULL);
        struct cx cx = {.s = &st, .em = 16, .rem = 16, .vw = 800, .vh = 600, .a = a,
                        .set = ar_alloc(a, (size_t)css_prop_count())};
        k = css_syntax_compute(reg->r.syntax, value, strlen(value), &cx, NULL, NULL, NULL, NULL);
    }
    ar_free(a);
    free(a);
    return k >= 0 ? &reg->r.syntax->component[k] : NULL;
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
    size_t start;  /* where its text began in the output */
    bool tainted;  /* it is an attr()-tainted value: the whole of what it puts is */
    /* lazy: property's computed value, computed here because a var() reached it before its turn
       (CUSTOM_LAZY; css-values-5 §8.6 replace a var() function), with that var()'s fallback and
       whether the substitution around it was tainted before */
    bool lazy, outer_tainted, has_fallback;
    bool invalid; /* a function of its text was the guaranteed-invalid value: so is the whole text */
    const char *fallback;
    size_t fallback_n;
};

/* What one substitution of arbitrary substitution functions (css-values-5 §8.6: var(), attr(), env())
   reads and finds. mode: DEFER, a custom property that waits for the early properties makes the value
   wait (the cascade computes it after them); EARLY, the value is an early property's, so such a reference
   is a cycle through the font size (css-properties-values-api-1 §2.7.1). */
enum { SUBST_PLAIN, SUBST_DEFER, SUBST_EARLY };
struct subst_taint { size_t (*range)[2]; size_t n, cap; bool all; };
/* a substitution context «attribute, name» (§8.6): marked cyclic when it is guarded again inside itself;
   walk: where the substitution that entered it was, whose «property» contexts are still guarded */
struct subst_walk;
struct attr_context { const char *name; struct attr_context *parent; const struct subst_walk *walk; bool cyclic; };
struct subst_env {
    const struct css_ctx *x;                 /* the registrations; NULL: none */
    const struct custom_prop *vars, *own;    /* the chain var() reads; where the element's own entries end */
    node_t *node;                            /* the element attr() reads (a pseudo-element's originating one) */
    struct cx *cx;                           /* attr()'s type() parses against it (NULL: a probe) */
    struct attr_context *attrs;              /* «attribute, name» substitution contexts being substituted */
    const struct subst_walk *walk;           /* the walk an attr() is replaced from (subst sets it) */
    const struct custom_prop *self;          /* «property, name» of the custom property being computed */
    bool *self_cyclic;                       /* set when a var() inside reaches it again */
    struct resolve *resolve;                 /* computes a CUSTOM_LAZY entry a var() reaches; NULL: none */
    struct subst_taint *taint;               /* NULL: whether only (tainted) */
    uint8_t mode;
    bool waited, tainted;
};
struct resolve;
static void custom_settle(struct resolve *R, struct custom_prop *p, const char *text, size_t n, bool tainted);
static void taint_range(struct subst_env *env, size_t a, size_t b) {
    env->tainted = true;
    struct subst_taint *t = env->taint;
    if (!t || a >= b || t->all) return;
    if (t->n == t->cap) {
        size_t cap = t->cap ? t->cap * 2 : 8;
        void *r = realloc(t->range, cap * sizeof *t->range);
        if (!r) { t->all = true; return; } /* every byte counts as tainted then */
        t->range = r; t->cap = cap;
    }
    t->range[t->n][0] = a;
    t->range[t->n++][1] = b;
}
static bool subst(const char *s, size_t n, struct subst_env *env, sbuf *out);
/* css-syntax-3: s starts with a function token of this name */
static bool function_at(const char *s, size_t i, size_t n, const char *name, size_t k) {
    return n - i > k && strn_ieq(s + i, name, k) && s[i + k] == '(' &&
           (i == 0 || (!ident_char((unsigned char)s[i - 1]) && s[i - 1] != '#' && s[i - 1] != '@' && s[i - 1] != '\\'));
}
/* §2.7.1: an early property's declaration references a value that waits for the font size. That value,
   and the values waiting with it that its var()s reach, are in a cycle with font-size. */
static void custom_cycle(const struct subst_env *env, const struct custom_prop *start) {
    struct custom_prop *p = (struct custom_prop *)start; /* waiting entries are the element's own */
    p->state |= CUSTOM_CYCLE;
    size_t cap = 8, count = 0;
    struct custom_prop **stack = malloc(cap * sizeof *stack);
    if (!stack) return;
    stack[count++] = p;
    while (count) {
        p = stack[--count];
        const char *s = p->raw ? p->raw : "";
        size_t n = strlen(s);
        for (size_t i = 0; i < n; i++) {
            if (s[i] == '"' || s[i] == '\'') {
                char q = s[i++];
                while (i < n && s[i] != q) { if (s[i] == '\\' && i + 1 < n) i++; i++; }
                continue;
            }
            if (!function_at(s, i, n, "var", 3)) continue;
            const char *a = s + i + 4, *e = s + n, *close = scan_to(a, e, ")"), *comma = scan_to(a, close, ",");
            const char *ns = skip_ws(a, comma), *ne = comma;
            trim_r(ns, &ne);
            struct custom_prop *q = (struct custom_prop *)own_find(env->vars, env->own, ns, (size_t)(ne - ns));
            if (!q || !(q->state & CUSTOM_PENDING) || (q->state & CUSTOM_CYCLE)) continue;
            q->state |= CUSTOM_CYCLE;
            if (count == cap) {
                struct custom_prop **grown = realloc(stack, cap * 2 * sizeof *stack);
                if (!grown) continue;
                stack = grown;
                cap *= 2;
            }
            stack[count++] = q;
        }
    }
    free(stack);
}
/* the element's attribute of a name, as an attribute selector matches it (Selectors 4: the name ASCII
   lowercased on an HTML element in an HTML document; no namespace) */
static const char *attr_named(const node_t *e, const char *name) {
    if (!e || e->type != N_ELEM) return NULL;
    bool html = doc_html_names(e);
    for (int i = 0; i < e->nattrs; i++) {
        const struct attr *a = &e->attrs[i];
        if (a->namespace_uri) continue;
        const char *local = attr_local(a);
        /* HTML: a non-HTML element's attribute in an HTML document matches ASCII case-insensitively too */
        if (html || (e->owner && !e->owner->xml) ? str_ieq(local, name) : !strcmp(local, name))
            return a->value ? a->value : "";
    }
    return NULL;
}
/* css-values-5 §8.7 attr( <attr-name> <attr-type>? , <declaration-value>? ): the first argument (its
   arbitrary substitution functions substituted) as a name and a type */
enum { ATTR_STRING, ATTR_NUMBER, ATTR_UNIT, ATTR_TYPE };
struct attr_arguments { const char *name; uint8_t type; bool typed; int unit; const char *unit_name; struct css_syntax *syntax; };
static bool attr_arguments(arena_t *a, const char *s, const char *e, struct attr_arguments *out) {
    *out = (struct attr_arguments){0};
    s = skip_ws(s, e);
    trim_r(s, &e);
    const char *p = s;
    if (p < e && *p == '|') p++; /* [ <ident-token>? '|' ]: the null namespace */
    const char *name = read_ident(a, &p, e, false);
    if (!name || !*name || (p < e && *p == '|')) return false; /* a namespace prefix: no @namespace map here */
    out->name = name;
    p = skip_ws(p, e);
    if (p == e) return true;
    out->typed = true;
    if (e - p >= 5 && strn_ieq(p, "type(", 5)) {
        const char *close = scan_to(p + 5, e, ")");
        if (close >= e || skip_ws(close + 1, e) != e) return false;
        out->syntax = css_syntax_parse(p + 5, (size_t)(close - p - 5), a);
        if (!out->syntax) return false;
        /* <url> is not a <syntax-single-component> here (§8.7.2) */
        for (unsigned k = 0; k < out->syntax->count; k++) if (out->syntax->component[k].type == CSS_SYNTAX_URL) return false;
        out->type = ATTR_TYPE;
        return true;
    }
    if (*p == '%' && skip_ws(p + 1, e) == e) { out->type = ATTR_UNIT; out->unit = css_unit_find("%", 1); out->unit_name = "%"; return true; }
    const char *word = read_ident(a, &p, e, true);
    if (!word || skip_ws(p, e) != e) return false;
    if (!strcmp(word, "raw-string")) { out->type = ATTR_STRING; return true; }
    if (!strcmp(word, "number")) { out->type = ATTR_NUMBER; return true; }
    /* an <attr-unit> is any <custom-ident>; one that is no unit falls back */
    out->type = ATTR_UNIT;
    out->unit = css_unit_find(word, strlen(word));
    out->unit_name = word;
    return true;
}
/* §8.7.1 replace an attr() function: its value into out (attr()-tainted), or false for the fallback */
static bool attr_value(struct subst_env *env, const struct attr_arguments *arg, const char *value, sbuf *out) {
    switch (arg->type) {
    case ATTR_STRING:
        css_serialize_string(out, value);
        return true;
    case ATTR_NUMBER: case ATTR_UNIT: {
        const char *v = value, *e = value + strlen(value), *p, *u;
        while (v < e && is_space((unsigned char)*v)) v++;
        while (e > v && is_space((unsigned char)e[-1])) e--;
        double number;
        size_t un;
        p = v;
        if (!css_numeric_token(&p, e, &number, &u, &un) || p != e || un) return false;
        if (arg->type == ATTR_UNIT) {
            unsigned kind = css_unit_kind(arg->unit);
            if (arg->unit < 0 || kind == CSS_CALC_NUMBER || !kind) return false;
        }
        sb_put(out, v, (size_t)(e - v));
        if (arg->type == ATTR_UNIT) sb_puts(out, arg->unit_name);
        return true;
    }
    case ATTR_TYPE: {
        /* its arbitrary substitution functions, with «attribute, name» the substitution context: one
           that a nested substitution guarded again is cyclic, and gives the guaranteed-invalid value */
        struct attr_context here = {arg->name, env->attrs, env->walk, false};
        struct subst_env inner = *env;
        inner.attrs = &here;
        inner.taint = NULL;
        sbuf text = {0};
        bool ok = subst(value, strlen(value), &inner, &text) && !here.cyclic;
        env->waited |= inner.waited;
        const char *t = text.p ? text.p : "";
        if (ok && !arg->syntax->count) ok = css_valid_declaration("--x", 3, t, text.n);
        else if (ok) {
            arena_t scratch = {.chunk_size = 1024};
            jmp_buf trap;
            scratch.trap = &trap;
            style_t probe_style;
            volatile bool matched = false;
            if (!setjmp(trap)) {
                struct cx probe = {0};
                if (env->cx) probe = *env->cx;
                else {
                    css_style_init(&probe_style, NULL);
                    probe = (struct cx){.s = &probe_style, .em = 16, .rem = 16, .vw = 800, .vh = 600};
                }
                probe.a = &scratch;
                probe.set = NULL;
                matched = css_syntax_compute(arg->syntax, t, text.n, &probe, NULL, NULL, NULL, NULL) >= 0;
            }
            ar_free(&scratch);
            ok = matched;
        }
        if (ok) sb_put(out, t, text.n);
        sb_free(&text);
        return ok;
    }
    }
    return false;
}

/* the lazy frames of a walk from f up to stop (NULL: to its root) are cyclic */
static void subst_mark_cyclic(const struct subst_walk *f, const struct subst_walk *stop) {
    for (; f; f = f->parent) {
        if (f->lazy) ((struct custom_prop *)f->property)->state |= CUSTOM_CYCLE;
        if (f == stop) break;
    }
}
/* §8.7.1 replace an attr() function whose arguments are a..close (comma: the first's end): DONE, its
   value put into out (attr()-tainted); FALLBACK, its fallback is what close has after comma; FAIL, the
   guaranteed-invalid value */
enum { ATTR_DONE, ATTR_FALLBACK, ATTR_FAIL };
static int attr_replace(struct subst_env *env, const char *a, const char *comma, const char *close, sbuf *out) {
    arena_t scratch = {.chunk_size = 512};
    jmp_buf trap;
    scratch.trap = &trap;
    volatile int result = ATTR_FAIL;
    sbuf first = {0}, value = {0};
    if (!setjmp(trap)) {
        struct subst_env inner = *env;
        inner.taint = NULL;
        bool parsed = subst(a, (size_t)(comma - a), &inner, &first);
        env->waited |= inner.waited;
        struct attr_arguments arg;
        if (parsed && attr_arguments(&scratch, first.p ? first.p : "", first.p ? first.p + first.n : "", &arg)) {
            /* the attribute is already being substituted: guarding «attribute, name» again marks every
               context of the cycle cyclic, and this attr() fails (§8.6): the «attribute» contexts since
               that one, and the «property» contexts (lazy frames) of the walks inside it. Only a type()
               substitutes in the value and guards it; a string, a number or a unit is the value as it
               is (§8.7.1) */
            struct attr_context *again = arg.type == ATTR_TYPE ? env->attrs : NULL;
            while (again && strcmp(again->name, arg.name)) again = again->parent;
            if (again) subst_mark_cyclic(env->walk, NULL);
            for (struct attr_context *c = env->attrs; again && c; c = c->parent) {
                c->cyclic = true;
                if (c == again) break;
                subst_mark_cyclic(c->walk, NULL);
            }
            const char *attribute = again ? NULL : attr_named(env->node, arg.name);
            size_t begin = out->n;
            if (attribute && attr_value(env, &arg, attribute, &value)) {
                if (subst_put(out, value.p ? value.p : "", value.n)) { taint_range(env, begin, out->n); result = ATTR_DONE; }
            } else if (comma < close) result = ATTR_FALLBACK;
            /* FAILURE without a fallback: an empty string when no type was given, else invalid */
            else if (!arg.typed && subst_put(out, "\"\"", 2)) result = ATTR_DONE;
        }
    }
    sb_free(&first);
    sb_free(&value);
    ar_free(&scratch);
    return result;
}

/* No C recursion for var(): real allocation/output overflow bounds expansion (attr()'s type() recurses
   once per distinct attribute, which its substitution contexts bound). Cyclic computed custom properties
   are represented by NULL, distinct from an empty but valid value; a defensive active-property check also
   protects raw callers. */
/* a fallback argument, what follows the comma: a <declaration-value>? of the function's argument
   grammar, so without the whitespace that separates it from the comma and the closing parenthesis */
static void fallback(const char *comma, const char *close, const char **text, size_t *length) {
    const char *t = skip_ws(comma + 1, close), *end = close;
    trim_r(t, &end);
    *text = t;
    *length = (size_t)(end - t);
}
/* §8.6: whether «property, q» is guarded: the property being computed, one a lazy frame of this walk
   computes, or one an enclosing attr() substitution is in. Guarding it again marks it and every
   context guarded since then cyclic. */
static bool subst_guarded(struct subst_env *env, const struct subst_walk *f, const struct custom_prop *q) {
    if (q == env->self) {
        subst_mark_cyclic(f, NULL);
        for (struct attr_context *c = env->attrs; c; c = c->parent) { c->cyclic = true; subst_mark_cyclic(c->walk, NULL); }
        if (env->self_cyclic) *env->self_cyclic = true;
        return true;
    }
    for (const struct subst_walk *p = f; p; p = p->parent)
        if (p->lazy && p->property == q) { subst_mark_cyclic(f, p); return true; }
    for (struct attr_context *c = env->attrs; c; c = c->parent)
        for (const struct subst_walk *p = c->walk; p; p = p->parent)
            if (p->property == q) {
                subst_mark_cyclic(f, NULL);
                for (struct attr_context *m = env->attrs; m; m = m->parent) {
                    m->cyclic = true;
                    subst_mark_cyclic(m->walk, m == c ? p : NULL);
                    if (m == c) break;
                }
                return true;
            }
    return false;
}
/* whether the substitution context that f's text is substituted in is cyclic: the nearest lazy frame's
   «property», else the innermost «attribute» context of this walk, else the property being computed */
static bool subst_context_cyclic(const struct subst_env *env, const struct subst_walk *f) {
    for (; f; f = f->parent)
        if (f->lazy) return (f->property->state & CUSTOM_CYCLE) != 0;
    if (env->attrs) return env->attrs->cyclic;
    if (env->self_cyclic && *env->self_cyclic) return true;
    return env->self && (env->self->state & CUSTOM_CYCLE);
}
/* §8.6 substitute arbitrary substitution functions. Every function of a sequence is replaced, also after
   one was the guaranteed-invalid value, which makes the sequence the guaranteed-invalid value (f->invalid,
   carried up to the frame its text is part of): the later ones' contexts are guarded too, and a cycle
   through them marks every context in it. A var() of an entry not computed yet this round (CUSTOM_LAZY)
   computes it where it is (a lazy frame): its value substituted, then settled as its computed value. A
   function whose value is the guaranteed-invalid value substitutes its fallback, unless the context it
   is substituted in is already cyclic: that is the guaranteed-invalid value whatever the fallback is,
   which therefore is not substituted (short-circuiting, csswg-drafts#11500). */
static bool subst(const char *s, size_t n, struct subst_env *env, sbuf *out) {
    size_t original = out->n;
    struct subst_walk *f = calloc(1, sizeof *f);
    if (!f) return false;
    f->s = s; f->n = n; f->start = out->n;
    bool ok = true;
    while (f) {
        s = f->s; n = f->n; size_t i = f->i;
        if (i == n) {
            struct subst_walk *parent = f->parent;
            if (f->lazy) {
                /* the entry's computed value, where its var() was */
                struct custom_prop *p = (struct custom_prop *)f->property;
                bool inner = env->tainted;
                env->tainted = f->outer_tainted;
                if (f->invalid) custom_settle(env->resolve, p, NULL, 0, false);
                else custom_settle(env->resolve, p, out->p ? out->p + f->start : "", out->n - f->start, inner);
                out->n = f->start;
                if (out->p && out->cap > out->n) out->p[out->n] = 0;
                if (p->state & CUSTOM_PENDING) { env->waited = true; ok = false; break; }
                if (!p->value) {
                    /* the guaranteed-invalid value: its var()'s fallback, unless what holds it is cyclic */
                    if (f->has_fallback && !subst_context_cyclic(env, parent)) {
                        f->lazy = false; f->property = NULL; f->tainted = false; f->invalid = false;
                        f->s = f->fallback; f->n = f->fallback_n; f->i = 0; f->start = out->n;
                        continue;
                    }
                    parent->invalid = true;
                } else if (!subst_put(out, p->value, strlen(p->value))) { ok = false; break; }
                else if (p->state & CUSTOM_TAINTED) taint_range(env, f->start, out->n);
            } else if (f->invalid) {
                if (parent) parent->invalid = true;
                else ok = false;
            } else if (f->tainted) taint_range(env, f->start, out->n);
            free(f); f = parent;
            continue;
        }
        if (s[i] == '"' || s[i] == '\'') {
            size_t start = i; char quote = s[i++];
            while (i < n && s[i] != quote) { if (s[i] == '\\' && i + 1 < n) i++; i++; }
            if (i < n) i++;
            if (!subst_put(out, s + start, i - start)) { ok = false; break; }
            f->i = i;
            continue;
        }
        bool is_var = function_at(s, i, n, "var", 3), is_env = !is_var && function_at(s, i, n, "env", 3);
        bool is_attr = !is_var && !is_env && function_at(s, i, n, "attr", 4);
        if (!is_var && !is_env && !is_attr) {
            if (!subst_put(out, s + i, 1)) { ok = false; break; }
            f->i++;
            continue;
        }
        const char *a = s + i + (is_attr ? 5 : 4), *e = s + n;
        const char *close = scan_to(a, e, ")");
        if (close >= e) { f->invalid = true; f->i = n; continue; }
        f->i = (size_t)(close + 1 - s); /* what follows the function is substituted next */
        const char *comma = scan_to(a, close, ",");
        const char *text;
        size_t length;
        const struct custom_prop *property = NULL;
        bool tainted = false;
        if (is_attr) {
            env->walk = f;
            int replaced = attr_replace(env, a, comma, close, out);
            env->walk = NULL;
            if (env->waited) { ok = false; break; }
            if (replaced == ATTR_DONE) continue;
            if (replaced == ATTR_FAIL || subst_context_cyclic(env, f)) { f->invalid = true; continue; }
            fallback(comma, close, &text, &length);
        } else {
            const char *ns = skip_ws(a, comma), *ne = comma;
            trim_r(ns, &ne);
            if (is_var && !supports_custom_name(ns, (size_t)(ne-ns))) { f->invalid = true; continue; }
            property = is_var ? custom_find(env->x, env->vars, env->own, ns, (size_t)(ne - ns)) : NULL;
            const char *val = property ? property->value : NULL;
            /* «property, name» guarded again: the guaranteed-invalid value */
            if (val && subst_guarded(env, f, property)) val = NULL;
            else if (val && env->resolve && (property->state & CUSTOM_LAZY)) {
                struct subst_walk *child = calloc(1, sizeof *child);
                if (!child) { ok = false; break; }
                ((struct custom_prop *)property)->state &= (uint8_t)~CUSTOM_PENDING;
                child->parent = f; child->property = property; child->s = val; child->n = strlen(val);
                child->start = out->n; child->lazy = true; child->outer_tainted = env->tainted;
                if (comma < close) { child->has_fallback = true; fallback(comma, close, &child->fallback, &child->fallback_n); }
                env->tainted = false;
                f = child;
                continue;
            } else if (property && (property->state & CUSTOM_PENDING)) {
                /* it waits for the early properties (§2.7.1) */
                if (env->mode == SUBST_EARLY) custom_cycle(env, property);
                else env->waited = true;
                ok = false;
                break;
            } else if (val) {
                /* a raw value already being substituted (a caller without lazy frames) */
                const struct subst_walk *p = f;
                while (p && p->property != property) p = p->parent;
                if (p) { f->invalid = true; continue; }
            }
            if (val) {
                text = val; length = strlen(val);
                tainted = (property->state & CUSTOM_TAINTED) != 0; /* §8.7.2: tainted as a whole */
            } else if (comma < close) {
                if (subst_context_cyclic(env, f)) { f->invalid = true; continue; }
                fallback(comma, close, &text, &length); property = NULL;
            } else if (is_env) {
                if (!subst_put(out, "0px", 3)) { ok = false; break; }
                continue;
            } else { f->invalid = true; continue; }
        }
        struct subst_walk *child = calloc(1, sizeof *child);
        if (!child) { ok = false; break; }
        child->parent = f; child->property = property; child->s = text; child->n = length;
        child->start = out->n; child->tainted = tainted;
        f = child;
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
    bool seen, on_stack;
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
/* §8.7.2: attr()-tainted bytes of a substituted value used as or in a <url> make its declaration
   invalid at computed-value time: in url() and src(), and the strings image() and image-set() (and
   cross-fade()'s images) take as URLs */
static bool taint_in_url(const struct subst_taint *t, const char *v, size_t n) {
    if (!t->n && !t->all) return false;
    for (size_t i = 0; i < n; i++) {
        bool whole = function_at(v, i, n, "url", 3) || function_at(v, i, n, "src", 3);
        size_t k = whole ? 4 : function_at(v, i, n, "image", 5) ? 6 : function_at(v, i, n, "image-set", 9) ? 10 :
                   function_at(v, i, n, "-webkit-image-set", 17) ? 18 : function_at(v, i, n, "cross-fade", 10) ? 11 : 0;
        if (!k) continue;
        const char *close = scan_to(v + i + k, v + n, ")");
        size_t from = i, to = (size_t)(close - v) + (close < v + n);
        if (t->all) return true;
        for (size_t r = 0; r < t->n; r++) {
            size_t a = t->range[r][0], b = t->range[r][1];
            if (b <= from || a >= to) continue;
            if (whole) return true;
            /* a string inside, or a url() inside, overlapped */
            for (size_t j = i + k; j < to; j++) {
                if (v[j] == '"' || v[j] == '\'') {
                    size_t start = j;
                    char q = v[j++];
                    while (j < to && v[j] != q) { if (v[j] == '\\' && j + 1 < to) j++; j++; }
                    if (!(j < a || start >= b)) return true;
                } else if (function_at(v, j, n, "url", 3) || function_at(v, j, n, "src", 3)) {
                    const char *end = scan_to(v + j + 4, v + to, ")");
                    if (!((size_t)(end - v) < a || j >= b)) return true;
                    j = (size_t)(end - v);
                }
            }
        }
    }
    return false;
}
bool css_substitute(web_doc *d, const style_t *st, node_t *node, const char *v, sbuf *out) {
    size_t n = strlen(v);
    if (!value_references(v)) { sb_put(out, v, n); return true; }
    struct subst_taint taint = {0};
    struct subst_env env = {.x = d ? d->sty.ctx : NULL, .vars = st ? st->vars : NULL, .own = st ? st->vars_inherited : NULL,
                            .node = node, .taint = &taint};
    size_t begin = out->n;
    bool ok = subst(v, n, &env, out) && !taint_in_url(&taint, out->p + begin, out->n - begin);
    free(taint.range);
    if (!ok) { out->n = begin; if (out->p && begin < out->cap) out->p[begin] = 0; }
    return ok;
}
const struct css_syntax *css_custom_syntax(web_doc *d, const char *name) {
    const struct registered *reg = d && name ? registry_get(d->sty.ctx, name, strlen(name)) : NULL;
    return reg && reg->r.syntax->count ? reg->r.syntax : NULL;
}
/* Web Animations 1 §5.3 and css-properties-values-api-1 §2.5: a custom property's keyframe value as the
   computed value it animates by. Its substitution is the declarations' (css-values-5 §8.6, attr() reading
   the site's element); a CSS-wide keyword, written or substituted, defaults as compute() defaults one */
int css_custom_animation_value(const struct css_anim_ctx *c, const char *name, const char *text, sbuf *out) {
    web_doc *d = c->d;
    const struct css_ctx *x = d && d->sty.ctx && d->sty.ctx->registry ? d->sty.ctx : NULL;
    size_t nn = strlen(name);
    const struct registered *reg = registry_get(x, name, nn);
    sbuf value = {0};
    if (!css_substitute(d, c->base, c->node, text, &value)) { sb_free(&value); return -1; }
    const char *v = value.p ? value.p : "", *end = v + value.n;
    v = skip_ws(v, end); trim_r(v, &end);
    size_t n = (size_t)(end - v);
    bool initial = strn_ieq(v, "initial", n), inherit = strn_ieq(v, "inherit", n),
         unset = strn_ieq(v, "unset", n) || strn_ieq(v, "revert", n);
    int result = 1;
    if (initial || inherit || unset) {
        const char *from;
        if (initial || (unset && reg && !reg->r.inherits)) from = reg ? reg->r.initial : NULL;
        else {
            const struct custom_prop *p = c->parent ? custom_find(x, c->parent->vars, c->parent->vars_inherited, name, nn)
                                                    : reg ? &reg->initial : NULL;
            from = p ? p->value : NULL;
        }
        if (from) sb_puts(out, from); else result = 0;
    } else if (reg && reg->r.syntax->count) {
        style_t scratch = *c->base;
        struct cx cx = {.s = &scratch, .parent = c->parent, .node = c->node, .em = c->base->font_size, .rem = c->rem,
                        .vw = c->vw, .vh = c->vh, .a = c->a, .set = ar_alloc(c->a, (size_t)css_prop_count())};
        struct css_color_computed color;
        css_style_color(c->base, CSS_SLOT_COLOR, &color);
        if (css_syntax_compute(reg->r.syntax, v, n, &cx, d ? d->base : NULL, &color.color, out, NULL) < 0) result = -1;
    } else sb_put(out, v, n);
    sb_free(&value);
    return result;
}
static bool value_references(const char *s) {
    if (!s) return false;
    size_t n = strlen(s);
    for (size_t i=0; n-i>=4; i++)
        if (strn_ieq(s+i,"var(",4) || strn_ieq(s+i,"env(",4) || (n-i>=5 && strn_ieq(s+i,"attr(",5))) return true;
    return false;
}

/* what an element's own custom properties are computed against */
struct resolve {
    struct cx *cx;
    struct subst_env env;
    const struct custom_prop *inherited;
    bool late, root;
    sbuf *output;   /* what custom_finish substitutes into */
    sbuf *scratch;  /* what custom_settle computes a registered value into */
    arena_t *arena; /* the computed values' */
};
/* invalid at computed-value time: a registered property's unset value (inherited, else initial) */
static const char *custom_unset(const struct registered *reg, const struct custom_prop *inherited) {
    if (reg->r.inherits) {
        const struct custom_prop *p = find_var(inherited, reg->r.name, strlen(reg->r.name));
        if (p) return p->value;
    }
    return reg->r.initial;
}
/* p's computed value from text, what its value substituted to (NULL, or p marked cyclic: invalid at
   computed-value time, a registered property's unset value). A value that is, after substitution, one
   CSS-wide keyword is as if that keyword were specified: explicit defaulting (css-values-5 §8.6,
   css-cascade-4 §7.3), as compute() does for a written one; revert rolls an author declaration back
   to origins without custom properties. Then a registered property's value is computed against its
   syntax (css-properties-values-api-1 §2.4); in the first round one that needs the element's font
   size or colour scheme waits for the early properties (late: the round after them). */
static void custom_settle(struct resolve *R, struct custom_prop *p, const char *text, size_t n, bool tainted) {
    const struct registered *reg = registry_get(R->env.x, p->name, strlen(p->name));
    bool typed = reg && reg->r.syntax->count;
    p->state &= (uint8_t)~CUSTOM_LAZY;
    if (!text || (p->state & CUSTOM_CYCLE)) { p->value = typed ? custom_unset(reg, R->inherited) : NULL; return; }
    if (text != p->value) {
        const char *k = skip_ws(text, text + n), *end = text + n;
        trim_r(k, &end);
        size_t kn = (size_t)(end - k);
        bool initial = strn_ieq(k, "initial", kn), inherit = strn_ieq(k, "inherit", kn),
             unset = strn_ieq(k, "unset", kn) || strn_ieq(k, "revert", kn);
        if (initial || inherit || unset) {
            p->state &= (uint8_t)~CUSTOM_TAINTED;
            if (initial || (unset && reg && !reg->r.inherits)) { p->value = reg ? reg->r.initial : NULL; return; }
            const style_t *parent = R->cx->parent;
            const struct custom_prop *from = parent ? custom_find(R->env.x, parent->vars, parent->vars_inherited, p->name, strlen(p->name))
                                                    : reg ? &reg->initial : NULL;
            p->value = from ? from->value : NULL;
            return;
        }
        if (tainted) p->state |= CUSTOM_TAINTED;
        p->value = ar_strndup(R->arena, text, n);
    }
    if (!typed) return;
    n = strlen(p->value);
    unsigned deps = css_syntax_dependencies(p->value, n);
    if (!R->late && (deps & (CSS_SYNTAX_DEP_FONT | CSS_SYNTAX_DEP_LINE_HEIGHT | CSS_SYNTAX_DEP_SCHEME |
                             (R->root ? CSS_SYNTAX_DEP_ROOT_FONT : 0)))) {
        p->state |= CUSTOM_PENDING;
        return;
    }
    R->scratch->n = 0;
    deps = 0;
    if (css_syntax_compute(reg->r.syntax, p->value, n, R->cx, p->base, NULL, R->scratch, &deps) < 0) {
        p->value = custom_unset(reg, R->inherited);
        return;
    }
    p->value = ar_strndup(R->arena, R->scratch->p ? R->scratch->p : "", R->scratch->n);
    if (deps & CSS_SYNTAX_DEP_COLOR) p->state |= CUSTOM_COLOR;
}
/* one own entry's computed value in its turn (css-variables-1 §3, css-properties-values-api-1 §2.4):
   the entries it references come first, and one a substitution reaches before its turn (a cycle, or
   through an attribute's value) is computed there (subst) */
static void custom_finish(struct resolve *R, struct custom_prop *p) {
    if (!(p->state & CUSTOM_LAZY)) return; /* not this round's, or computed already */
    p->state &= (uint8_t)~CUSTOM_PENDING;
    if (p->state & CUSTOM_CYCLE) { custom_settle(R, p, NULL, 0, false); return; }
    if (!p->value) { p->state &= (uint8_t)~CUSTOM_LAZY; return; }
    if (!value_references(p->value)) { custom_settle(R, p, p->value, strlen(p->value), false); return; }
    R->output->n = 0;
    R->env.waited = R->env.tainted = false;
    R->env.mode = R->late ? SUBST_PLAIN : SUBST_DEFER;
    bool cyclic = false;
    p->state &= (uint8_t)~CUSTOM_LAZY; /* guarded as the property being computed */
    R->env.self = p;
    R->env.self_cyclic = &cyclic;
    bool ok = subst(p->value, strlen(p->value), &R->env, R->output);
    R->env.self = NULL;
    R->env.self_cyclic = NULL;
    if (R->env.waited && !cyclic) { p->value = p->raw; p->state |= CUSTOM_PENDING; return; }
    if (cyclic) p->state |= CUSTOM_CYCLE;
    custom_settle(R, p, ok ? (R->output->p ? R->output->p : "") : NULL, R->output->n, R->env.tainted);
}

/* CSS Color 4 §15: a registered <color> that names currentcolor computes against the element's colour,
   known once its declarations applied (its descendants inherit the resolved colour) */
static void custom_colors(web_doc *d, arena_t *a, struct cx *cx, struct custom_prop *mine, const struct custom_prop *inherited) {
    struct css_color_computed color;
    css_style_color(cx->s, CSS_SLOT_COLOR, &color);
    sbuf out = {0};
    for (struct custom_prop *p = mine; p && p != inherited; p = p->next) {
        if (!(p->state & CUSTOM_COLOR) || !p->value) continue;
        p->state &= (uint8_t)~CUSTOM_COLOR;
        const struct registered *reg = registry_get(d->sty.ctx, p->name, strlen(p->name));
        if (!reg) continue;
        out.n = 0;
        if (css_syntax_compute(reg->r.syntax, p->value, strlen(p->value), cx, p->base, &color.color, &out, NULL) >= 0)
            p->value = ar_strndup(a, out.p ? out.p : "", out.n);
    }
    sb_free(&out);
}

/* Iterative Tarjan: only declarations on this element form graph vertices.
   Inherited properties are already computed, so cannot form child cycles.
   Components are resolved in dependency order before mutating raw values. The edges include
   references in fallbacks, so a component can be cyclic only through a fallback no substitution
   takes: within one, the substitution finds the cycles it goes through (css-values-5 §8.6, a cycle
   only through what is substituted: csswg-drafts#11500). */
static void resolve_vars(web_doc *d, arena_t *a, struct cx *cx, struct custom_prop *mine, const struct custom_prop *inherited,
                         bool late, bool root) {
    const struct css_ctx *x = d->sty.ctx && d->sty.ctx->registry ? d->sty.ctx : NULL;
    bool references=false;
    for (struct custom_prop *m=mine;m!=inherited;m=m->next) {
        if (!m->raw) continue;
        if (late ? (m->state & CUSTOM_PENDING) != 0 :
            value_references(m->value) || (x && registry_get(x, m->name, strlen(m->name)))) { references=true; break; }
    }
    if (!references) return; /* Retain immutable literal AST values, no scratch/copy. */
    struct var_workspace { arena_t arena; sbuf output, scratch; } *work = calloc(1,sizeof *work);
    if (!work) {
        if (css_worker()) { ar_alloc(a, SIZE_MAX); return; }
        for (struct custom_prop *m=mine;m!=inherited;m=m->next) m->value=NULL;
        web_js_console(d,2,"Custom property graph allocation failed"); return;
    }
    jmp_buf trap; jmp_buf *outer = a->trap;
    work->arena.trap = a->trap = &trap;
    int failed = setjmp(trap);
    if (failed) {
        sb_free(&work->output); sb_free(&work->scratch); ar_free(&work->arena); free(work); a->trap=outer;
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
    struct resolve R = {.cx = cx, .env = {.x = x, .vars = mine, .own = inherited, .node = cx->node, .cx = cx},
                        .inherited = inherited, .late = late, .root = root, .output = &work->output,
                        .scratch = &work->scratch, .arena = a};
    R.env.resolve = &R;
    /* this round's entries are not computed yet: one a substitution reaches before its turn is computed there */
    for (struct custom_prop *m=mine;m!=inherited;m=m->next)
        if (m->raw && (!late || (m->state & CUSTOM_PENDING))) m->state |= CUSTOM_LAZY;
    for (struct var_walk *start=all;start;start=start->next) {
        if (start->seen) continue;
        struct var_walk *f=start;
        while (f) {
            if (!f->seen) {
                if (index==SIZE_MAX) longjmp(trap,1);
                f->seen=f->on_stack=true;f->index=f->low=++index;
                f->component=components;components=f;
            }
            struct var_walk *dep=var_dependency(f,&walks);
            if (dep) {
                if (!dep->seen) { dep->parent=f;f=dep;continue; }
                if (dep->on_stack && dep->index<f->low) f->low=dep->index;
                continue;
            }
            if (f->low==f->index) {
                struct var_walk *v;
                do {
                    v=components;components=v->component;v->on_stack=false;
                    custom_finish(&R, v->property);
                } while (v!=f);
            }
            struct var_walk *parent=f->parent;
            if (parent && f->low<parent->low) parent->low=f->low;
            f=parent;
        }
    }
    sb_free(&work->output); sb_free(&work->scratch); ar_free(&work->arena); free(work); a->trap=outer;
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
    /* revert in the position fallback origin reverts as the author origin does (§6.4) */
    if(from->position_try)return rollback->origin?candidate->position_try||dent_author(candidate):candidate->position_try;
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
    bool substituted = value_references(v);
    if (substituted) {
        c->vbuf.n = 0;
        struct subst_taint taint = {0};
        struct subst_env env = {.x = c->d->sty.ctx && c->d->sty.ctx->registry ? c->d->sty.ctx : NULL,
                                .vars = cx->s->vars, .own = cx->s->vars_inherited, .node = cx->node, .cx = cx,
                                .taint = &taint, .mode = cx->font_pass ? SUBST_EARLY : SUBST_PLAIN};
        bool ok = subst(v, n, &env, &c->vbuf) && !taint_in_url(&taint, c->vbuf.p, c->vbuf.n);
        free(taint.range);
        /* css-values-5 §8.6 / css-variables-1 §3: a declaration whose substitution fails is invalid at
           computed-value time: its property computes as unset, not as the declarations below it */
        if (!ok) { v = "unset"; n = 5; }
        else { v = sb_cstr(&c->vbuf); n = c->vbuf.n; }
    }
    const char *end=v+n;v=skip_ws(v,end);trim_r(v,&end);n=(size_t)(end-v);
    bool revert_layer=strn_ieq(v,"revert-layer",n),revert_origin=strn_ieq(v,"revert",n);
    /* the same for a substituted value that does not match the property's grammar */
    if(!*has_rollback&&!revert_layer&&!revert_origin){
        if(!css_apply(d->p,v,n,cx)&&substituted)css_apply(d->p,"unset",5,cx);
        return;
    }
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
    } else if (!css_apply(d->p, v, n, cx) && substituted) css_apply(d->p, "unset", 5, cx);
    /* Restore only temporarily hidden bits. Real winners remain consumed. */
    if(was_rollback)for(int i=0;i<count;i++)if(hidden[i])cx->set[i]=0;
}

/* css-text-decor-4 §2: an element's text-decoration-line decorates its box, whose lines then draw over
   the text of its in-flow inline content, its in-flow descendants included; not over out-of-flow
   descendants (floats, absolutely positioned boxes) nor the contents of atomic inlines, which start
   afresh, and a display: contents element has no box to decorate. s's list is its parent's so filtered,
   its own entry first; each entry keeps the decorating box's used values (its color, thickness and
   offset against its own font size), so a descendant borrows the list as it borrows inherited values.
   The baseline shift is how far vertical-align raised s's inline box within its line, so that paint
   keeps a decorating box's lines at its own baseline over a raised descendant's text (§2: a decoration's
   position follows the vertical-align of the decorating box, not of its descendants). */
static void style_decorations(style_t *s, const style_t *parent, arena_t *a) {
    bool atomic = s->display == D_INLINE_BLOCK || s->display == D_INLINE_TABLE || s->display == D_INLINE_FLEX ||
                  s->display == D_INLINE_GRID;
    bool out_of_flow = s->float_ != FL_NONE || s->position == POS_ABSOLUTE || s->position == POS_FIXED;
    const struct css_decorating *list = parent && !atomic && !out_of_flow ? parent->decorations : NULL;
    float shift = parent ? parent->baseline_shift : 0;
    if (s->display == D_INLINE) shift += css_style_baseline_shift(s);
    else if (s->display != D_CONTENTS) {
        /* a block container's lines are their own: its ancestors' lines draw at their baseline */
        shift = 0;
        bool shifted = false;
        for (const struct css_decorating *d = list; d && !shifted; d = d->next) shifted = d->shift != 0;
        if (shifted) {
            struct css_decorating *first = NULL, **tail = &first;
            for (const struct css_decorating *d = list; d; d = d->next) {
                struct css_decorating *copy = ar_alloc(a, sizeof *copy);
                *copy = *d; copy->shift = 0; copy->next = NULL; *tail = copy; tail = (struct css_decorating **)&copy->next;
            }
            list = first;
        }
    }
    s->baseline_shift = shift;
    if (s->text_decoration && s->display != D_CONTENTS && s->display != D_NONE) {
        const struct css_decoration *dec = css_style_decoration(s);
        const struct css_text *text = css_style_text(s);
        struct css_decorating *own = ar_alloc(a, sizeof *own);
        wfont f = style_font(s);
        *own = (struct css_decorating){.next = list, .font = f.ttf, .font_size = s->font_size, .shift = shift,
                                       .lines = s->text_decoration, .style = dec->style,
                                       .underline_position = text->underline_position};
        /* a percentage is of 1em (§2.4, §2.7); from-font has no metric from this font: auto */
        own->thickness = dec->thickness.kind == LK_LEN || dec->thickness.kind == LK_EXPR ?
                         len_resolve(&dec->thickness, s->font_size) : NAN;
        own->underline_offset = text->underline_offset.kind == LK_LEN || text->underline_offset.kind == LK_EXPR ?
                                len_resolve(&text->underline_offset, s->font_size) : NAN;
        own->color = dec->color.argb != COLOR_CURRENT ? dec->color.argb :
                     dec->color.exact ? css_color_value_argb(dec->color.exact, s->color) : s->color;
        list = own;
    }
    s->decorations = list;
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
    if (pseudo == PE_MARKER) css_style_marker_defaults(s);
    /* custom properties: the most important declaration of each name */
    const struct custom_prop *inherited = s->vars;
    const struct css_ctx *registry = d->sty.ctx && d->sty.ctx->registry ? d->sty.ctx : NULL;
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
        /* css-cascade-4 §7.3 explicit defaulting: initial is the initial value (a registered one, else the
           guaranteed-invalid value), inherit the parent's computed value, unset either, as the property
           inherits (css-properties-values-api-1 §2.3: registered or not) */
        bool initial = !strcasecmp(x->value, "initial"), inherit = !strcasecmp(x->value, "inherit"),
             unset = !strcasecmp(x->value, "unset");
        if (initial || inherit || unset) {
            const struct registered *reg = registry_get(registry, cp->name, strlen(cp->name));
            if (initial || (unset && reg && !reg->r.inherits)) cp->value = reg ? reg->r.initial : NULL;
            else {
                const struct custom_prop *from = parent ? custom_find(registry, parent->vars, parent->vars_inherited,
                                                                      cp->name, strlen(cp->name))
                                                        : reg ? &reg->initial : NULL;
                cp->value = from ? from->value : NULL;
            }
        } else {
            cp->value = cp->raw = ar_strdup(c->a, x->value);
            cp->base = x->base;
        }
        cp->next = mine;
        mine = cp;
        if (segment) var_slot_put(segment->slots, segment->capacity, cp->name, strlen(cp->name), cp);
    }
    memset(setbits, 0, (size_t)css_prop_count());
    struct css_rollback rollback[css_prop_count()];bool has_rollback=false; /* initialized only for a real rollback */
    struct cx cx = {s, parent, e, parent ? parent->font_size : 16, c->rem, (float)c->vw, (float)c->vh,
                    c->a, setbits, true, false, false, 0, 0, 0, false, false};
    struct css_ctx *index=d->sty.ctx;
    if(index && __atomic_load_n(&index->containers,__ATOMIC_ACQUIRE)) {
        bool inclusive=pseudo==PE_BEFORE || pseudo==PE_AFTER || pseudo==PE_MARKER;
        node_t *inline_container=query_container(e,NULL,1,inclusive),*block_container=query_container(e,NULL,2,inclusive);
        if(inline_container){cx.cq_width=inline_container->container_width;cx.cq_width_set=true;}
        if(block_container){cx.cq_height=block_container->container_height;cx.cq_height_set=true;}
    }
    /* the custom properties first: the declarations below substitute them. A registered value that needs
       the element's font size or colour scheme is computed after the early properties (§2.7.1) */
    bool root_element = e == d->html && !pseudo;
    if (mine) {
        struct custom_prop *last = mine;
        while (last->next) last = last->next;
        last->next = (struct custom_prop *)inherited;
        mine->segment = segment;
        s->vars = mine;
        resolve_vars(d, c->a, &cx, mine, inherited, false, root_element);
    }
    /* css-values-5 §10: a declaration from a tree outside the element's (::part()) does not count its
       siblings; the animation and position fallback origins are the element's own (their depth is 0) */
    int tree_depth = scope_depth(doc_node_root(e, false));
    for (int i = 0; i < ndents; i++)
        if (dents[i].pseudo == pseudo && dents[i].d->p && css_prop_is_early(dents[i].d->p)) {
            cx.tree_mismatch = !dents[i].animation && !dents[i].position_try && dents[i].scope_depth < tree_depth;
            apply_decl(c, &cx, &dents[i],rollback,&has_rollback);
        }
    cx.font_pass = false;
    cx.tree_mismatch = false;
    cx.em = s->font_size;
    if (mine && registry) resolve_vars(d, c->a, &cx, mine, inherited, true, root_element);
    for (int i = 0; i < ndents; i++)
        if (dents[i].pseudo == pseudo && dents[i].d->p) {
            cx.tree_mismatch = !dents[i].animation && !dents[i].position_try && dents[i].scope_depth < tree_depth;
            apply_decl(c, &cx, &dents[i],rollback,&has_rollback);
        }
    cx.tree_mismatch = false;
    if (!pseudo) web_dialog_style(d,e,s);
    if(pseudo==PE_BACKDROP) {
        if(s->position!=POS_FIXED && s->position!=POS_ABSOLUTE)s->position=POS_ABSOLUTE;
        if(s->display==D_CONTENTS)s->display=D_BLOCK; s->float_=FL_NONE;
    }
    css_style_finish(s, parent, e == d->html && !pseudo);
    /* CSS Generated Content 3 §1.1: normal computes to none on ::before and ::after */
    if ((pseudo == PE_BEFORE || pseudo == PE_AFTER) && !s->content) s->content = &css_content_none;
    if (registry && mine) custom_colors(d, c->a, &cx, mine, inherited);
    /* anchor() and anchor-size() resolve only for absolutely positioned boxes (layout.c) */
    css_style_anchor_fallbacks(s, c->a);
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
    style_decorations(s, parent, c->a);
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
            if (x->important) continue; /* CSS Animations 1 §3: !important in a keyframe is ignored */
            if (!x->p) {
                /* css-properties-values-api-1 §2.5: custom properties animate by computed value */
                if (x->name && x->value) k->values[k->count++] = (struct web_keyframe_value){.name = (char *)x->name, .text = (char *)x->value};
                continue;
            }
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
/* a custom property's sampled computed value, a declaration of the same origin; the guaranteed-invalid
   value is the initial keyword's (an unregistered property's initial value, css-variables-1 §2) */
static void motion_emit_custom(void *arg, const char *name, const char *value) {
    struct motion_cascade *m = arg;
    struct cascade *c = m->c;
    struct decl *x = ar_alloc(c->a, sizeof *x);
    *x = (struct decl){.name = ar_strdup(c->a, name), .value = ar_strdup(c->a, value ? value : "initial")};
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
        .pseudo = pseudo, .keyframes = motion_frames, .emit = motion_emit, .emit_custom = motion_emit_custom, .arg = &m};
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
/* css-anchor-position-1 §6.4: the @position-try rule of a name, searched as @keyframes are */
static const struct position_try_rule *position_try_lookup(struct cascade *c, node_t *n, const char *name) {
    for (node_t *root = doc_node_root(n, false); root; root = root->shadow_host ? doc_node_root(root->shadow_host, false) : NULL) {
        const struct position_try_rule *found = NULL;
        struct cascade_layer *found_layer = NULL;
        struct css_scope *scope = root->style_scope;
        for (struct css_binding *b = scope ? scope->first : NULL; b; b = b->next)
            for (const struct position_try_rule *r = b->index->position_tries; r; r = r->next)
                if (!strcmp(r->name, name) && font_conditions_ok(r->media, c->vw, c->vh, web_js_enabled(c->d))) {
                    struct cascade_layer *layer = binding_layer(b, r->layer);
                    if (!found || layer_compare(layer, found_layer) >= 0) { found = r; found_layer = layer; }
                }
        if (found) return found;
    }
    return NULL;
}
/* §6.1, §6.5.2: an absolutely positioned box's @position-try and <position-area> options, each the
   cascade with the option's declarations inserted in the position fallback origin. s's anchor group
   becomes a copy whose options hold them; layout (layout.c) applies the try tactics and chooses. */
static void position_try_styles(struct cascade *c, node_t *e, const style_t *parent, uint8_t pseudo, style_t *s) {
    if (!s || (s->position != POS_ABSOLUTE && s->position != POS_FIXED)) return;
    const struct css_anchor *anchor = css_style_anchor(s);
    const struct css_position_tries *tries = &anchor->tries;
    bool any = false;
    for (uint32_t i = 0; i < tries->count; i++) any |= tries->option[i].rule || tries->option[i].area[0];
    if (!any) return;
    struct css_position_try *options = ar_alloc(c->a, tries->count * sizeof *options);
    memcpy(options, tries->option, tries->count * sizeof *options);
    const struct propdef *area = css_prop_lookup("position-area", 13);
    for (uint32_t i = 0; i < tries->count; i++) {
        struct css_position_try *o = &options[i];
        o->style = NULL;
        if (o->rule) {
            const struct position_try_rule *rule = position_try_lookup(c, e, o->rule);
            if (!rule) continue;
            for (int j = 0; j < rule->count; j++) { add_dent(&rule->decls[j], (uint64_t)j, pseudo, 0); dents[ndents - 1].position_try = true; }
        } else if (o->area[0] && area) {
            sbuf text = {0};
            bool written = css_position_area_text(o->area, &text);
            struct decl *x = written ? ar_alloc(c->a, sizeof *x) : NULL;
            if (x) *x = (struct decl){.p = area, .value = ar_strndup(c->a, text.p ? text.p : "", text.n)};
            sb_free(&text);
            if (!x) continue;
            add_dent(x, 0, pseudo, 0); dents[ndents - 1].position_try = true;
        } else continue;
        qsort(dents, (size_t)ndents, sizeof *dents, cmp_dent);
        o->style = compute(c, e, parent, pseudo);
        /* the option's declarations out again; the others keep their order */
        int kept = 0;
        for (int j = 0; j < ndents; j++) if (!dents[j].position_try) dents[kept++] = dents[j];
        ndents = kept;
    }
    struct css_anchor *copy = ar_alloc(c->a, sizeof *copy);
    *copy = *anchor;
    copy->tries.option = options;
    s->anchor = copy;
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
    free(n->style_cssom); n->style_cssom = NULL;
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
/* the styles of two styles' position options (css_position_try's style) */
static bool position_options_equal(const style_t *a, const style_t *b) {
    const struct css_position_tries *x = &css_style_anchor(a)->tries, *y = &css_style_anchor(b)->tries;
    if (x->count != y->count) return false;
    for (uint32_t i = 0; i < x->count; i++) if (!style_equal(x->option[i].style, y->option[i].style)) return false;
    return true;
}
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
        if (s->kind == SK_ATTR) {
            struct simple present = *s;
            present.op = AO_EXISTS;
            if (!match_attr(&present, node)) return false;
        }
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
        "checked", "contenteditable", "dir", "disabled", "form", "headingoffset", "headingreset", "href", "lang",
        "list", "max", "maxlength", "min", "minlength", "multiple", "name", "open", "pattern", "placeholder",
        "readonly", "required", "selected", "slot", "step", "type", "value", "xml:lang",
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
    /* a selector names a namespaced attribute by its local name */
    const char *colon = strchr(change->name, ':');
    if (colon) reach_named(d, scope, FEATURE_ATTR, colon + 1, strlen(colon + 1), &r);
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
    /* css-values-5 §10: the children whose styles count their siblings (sibling-index(), sibling-count()) */
    if(subtree->type==N_ELEM && __atomic_load_n(&parent->style_counts_children,__ATOMIC_RELAXED))
        for(node_t *child=parent->first;child;child=child->next)
            if(child->type==N_ELEM && child!=subtree)mark_style_subject(d,child,MARK_POSITION);
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
    /* containing-block ownership changes (Filter Effects 1 §5, 2 §2) */
    if (!!a->filter != !!b->filter || !!a->backdrop_filter != !!b->backdrop_filter) return false;
    /* Values change scrollable overflow even though normal-flow boxes stay
       unchanged; identity versus none also changes fixed containing blocks. */
    if(!same_translation(a,b))return false;
    if ((a->display == D_INLINE || b->display == D_INLINE) && (a->opacity < 1) != (b->opacity < 1)) return false;
    /* content, the list and counter values and the ::marker make generated text */
    if(!equal_string(a->font_names,b->font_names) || !css_content_equal(a->content,b->content) ||
        !equal_string(a->container_names,b->container_names) ||
        !css_list_equal(a->list,b->list) || !css_counters_equal(a->counters,b->counters) ||
        !css_style_layout_equal(a->marker,b->marker) ||
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
    x.filter=y.filter=x.backdrop_filter=y.backdrop_filter=NULL; /* effect amounts are paint-only arena borrows */
    x.clip_path=y.clip_path=NULL;memset(&x.clip,0,sizeof x.clip);memset(&y.clip,0,sizeof y.clip); /* clipping paints only */
    /* a float's shape-outside and shape-margin shape the lines beside it */
    if(!css_float_shape_equal(x.float_shape,y.float_shape))return false;
    x.float_shape=y.float_shape=NULL;
    /* anchor positioning: the anchor names others find, and this box's anchors and options */
    if(!css_anchor_equal(x.anchor,y.anchor) || !position_options_equal(a,b))return false;
    x.anchor=y.anchor=NULL;
    /* same_translation() saw whether there is an offset transform; where it puts the box is paint's */
    x.offset=y.offset=NULL;
    /* a gap decoration's segments are layout's (layout.c records them); colour and style paint only */
    if(!css_gap_rules_layout_equal(x.gap_rules,y.gap_rules))return false;
    x.gap_rules=y.gap_rules=NULL;
    /* the background layers, border-image and mask-border paint only (an outset is ink overflow) */
    x.background=y.background=NULL;x.border_image=y.border_image=x.mask_border=y.mask_border=NULL;
    x.border_image_img=y.border_image_img=x.mask_border_img=y.mask_border_img=0;
    x.bg_layers=y.bg_layers=x.bg_layer=y.bg_layer=0;
    x.ui=y.ui=NULL; /* the pointer and the caret's and accents' colours are not layout */
    /* line breaking reads the text group's breaking and hyphenation values; text-shadow and the
       decorations (their lines, colour, style, thickness, offset) paint only */
    if(!css_text_layout_equal(x.text,y.text))return false;
    x.text=y.text=NULL;x.decoration=y.decoration=NULL;x.decorations=y.decorations=NULL;x.text_decoration=y.text_decoration=0;
    /* same_translation() decided every geometric effect of the transform
       properties; their values and arena addresses are paint state. */
    x.transform=y.transform=NULL;memset(&x.translate,0,sizeof x.translate);memset(&y.translate,0,sizeof y.translate);
    x.rotate=y.rotate=x.scale=y.scale=NULL;
    memset(x.transform_origin,0,sizeof x.transform_origin);memset(y.transform_origin,0,sizeof y.transform_origin);
    x.transform_box=y.transform_box=0;
    memset(&x.motion,0,sizeof x.motion);memset(&y.motion,0,sizeof y.motion); /* timing lists are not layout */
    x.animated=y.animated=false;
    x.font_names=y.font_names=NULL;x.font_choices=y.font_choices=NULL; x.content=y.content=NULL;
    x.list=y.list=NULL;x.counters=y.counters=NULL;x.marker=y.marker=NULL;
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
    x.bg_image=y.bg_image=NULL;x.mask=y.mask=NULL; /* mask layers paint over the laid-out boxes */
    x.bg_img=y.bg_img=0;
    x.color=y.color=x.bg_color=y.bg_color=0;
    x.color_inherited=y.color_inherited=x.color_own=y.color_own=NULL;x.color_current=y.color_current=0;
    x.color_scheme=y.color_scheme=0;x.color_scheme_text=y.color_scheme_text=NULL;
    memset(x.border_color,0,sizeof x.border_color);memset(y.border_color,0,sizeof y.border_color);
    x.outline_color=y.outline_color=0;x.outline_width=y.outline_width=x.outline_offset=y.outline_offset=0;
    x.outline_style=y.outline_style=0; /* outlines paint outside layout */
    memset(&x.svg_fill,0,sizeof x.svg_fill);memset(&y.svg_fill,0,sizeof y.svg_fill);
    memset(&x.svg_stroke,0,sizeof x.svg_stroke);memset(&y.svg_stroke,0,sizeof y.svg_stroke);
    x.svg_stop_color=y.svg_stop_color=0;
    x.opacity=y.opacity=x.svg_fill_opacity=y.svg_fill_opacity=x.svg_stroke_opacity=y.svg_stroke_opacity=x.svg_stop_opacity=y.svg_stop_opacity=0;
    memset(x.grad,0,sizeof x.grad);memset(y.grad,0,sizeof y.grad);x.has_grad=y.has_grad=false;
    x.bg_repeat=y.bg_repeat=x.bg_size_kind=y.bg_size_kind=0;
    memset(x.bg_size,0,sizeof x.bg_size);memset(y.bg_size,0,sizeof y.bg_size);
    x.svg_clip_rule=y.svg_clip_rule=0;
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
    if (!x || !y || x->count != y->count || x->opacity != y->opacity || x->effects != y->effects) return false;
    for (size_t i = 0; i < x->count; i++) {
        const struct css_filter_step *s = &x->steps[i], *e = &y->steps[i];
        if (s->kind != e->kind || s->value != e->value || s->x != e->x || s->y != e->y || s->color != e->color ||
            !css_color_value_equal(s->color_value, e->color_value) || !equal_string(s->url, e->url)) return false;
    }
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
        !css_content_equal(a->content, b->content) || !css_list_equal(a->list, b->list) ||
        !css_counters_equal(a->counters, b->counters) || !style_equal(a->marker, b->marker) ||
        !equal_string(a->bg_image, b->bg_image) || !css_mask_equal(css_style_mask(a), css_style_mask(b)) ||
        !css_motion_equal(&a->motion, &b->motion) || !equal_string(a->svg_fill.ref, b->svg_fill.ref) ||
        !equal_string(a->svg_stroke.ref, b->svg_stroke.ref) || !shadows_equal(a->box_shadow, b->box_shadow) ||
        !filters_equal(a->filter, b->filter) || !filters_equal(a->backdrop_filter, b->backdrop_filter) ||
        !css_shape_equal(a->clip_path, b->clip_path) ||
        !css_float_shape_equal(a->float_shape, b->float_shape) || !css_offset_equal(a->offset, b->offset) ||
        !css_gap_rules_equal(a->gap_rules, b->gap_rules) || !css_anchor_equal(a->anchor, b->anchor) || !position_options_equal(a, b) ||
        !css_background_equal(a->background, b->background) || !css_border_image_equal(a->border_image, b->border_image, false) ||
        !css_border_image_equal(a->mask_border, b->mask_border, true) ||
        !css_ui_equal(a->ui, b->ui) ||
        !transforms_equal(a->transform, b->transform) ||
        !transforms_equal(a->rotate, b->rotate) || !transforms_equal(a->scale, b->scale) ||
        !gradients_equal(a->gradient, b->gradient) || !vars_equal(a->vars, b->vars) ||
        !equal_tracks(a->grid_cols, b->grid_cols) || !equal_tracks(a->grid_rows, b->grid_rows) ||
        !equal_tracks(a->grid_auto_cols, b->grid_auto_cols) || !equal_tracks(a->grid_auto_rows, b->grid_auto_rows) ||
        !equal_areas(a->grid_areas, b->grid_areas) || !style_equal(a->before, b->before) ||
        !style_equal(a->after, b->after) || !style_equal(a->backdrop, b->backdrop) ||
        !style_equal(a->placeholder, b->placeholder) || !css_color_slots_equal(a, b) ||
        !equal_string(a->color_scheme_text, b->color_scheme_text) || !css_text_equal(a->text, b->text) ||
        !css_decoration_equal(a->decoration, b->decoration) || !css_decorating_equal(a->decorations, b->decorations))
        return false;
    for (int i = 0; i < 4; i++) if (!equal_string(a->grid_place[i].name, b->grid_place[i].name)) return false;
    style_t x = *a, y = *b;
    len_t *xl[] = {&x.object_pos[0], &x.object_pos[1], &x.line_height, &x.svg_stroke_width, &x.width, &x.height,
                   &x.min_width, &x.max_width, &x.min_height, &x.max_height, &x.text_indent, &x.flex_basis,
                   &x.translate.xy[0], &x.translate.xy[1], &x.transform_origin[0], &x.transform_origin[1],
                   &x.bg_size[0], &x.bg_size[1]};
    len_t *yl[] = {&y.object_pos[0], &y.object_pos[1], &y.line_height, &y.svg_stroke_width, &y.width, &y.height,
                   &y.min_width, &y.max_width, &y.min_height, &y.max_height, &y.text_indent, &y.flex_basis,
                   &y.translate.xy[0], &y.translate.xy[1], &y.transform_origin[0], &y.transform_origin[1],
                   &y.bg_size[0], &y.bg_size[1]};
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
    x.container_names = y.container_names = x.font_names = y.font_names = NULL; x.content = y.content = NULL;
    x.list = y.list = NULL; x.counters = y.counters = NULL; x.marker = y.marker = NULL; /* compared above */
    x.bg_image = y.bg_image = NULL; x.mask = y.mask = NULL;
    memset(&x.motion, 0, sizeof x.motion); memset(&y.motion, 0, sizeof y.motion);
    x.svg_fill.ref = y.svg_fill.ref = x.svg_stroke.ref = y.svg_stroke.ref = NULL;
    x.box_shadow = y.box_shadow = NULL; x.filter = y.filter = x.backdrop_filter = y.backdrop_filter = NULL;
    x.clip_path = y.clip_path = NULL;
    x.float_shape = y.float_shape = NULL; x.offset = y.offset = NULL; x.gap_rules = y.gap_rules = NULL; x.anchor = y.anchor = NULL; x.ui = y.ui = NULL; /* the groups compared above */
    x.background = y.background = NULL; x.border_image = y.border_image = x.mask_border = y.mask_border = NULL;
    x.text = y.text = NULL; x.decoration = y.decoration = NULL; x.decorations = y.decorations = NULL;
    x.transform = y.transform = x.rotate = y.rotate = x.scale = y.scale = NULL;
    x.gradient = y.gradient = NULL; x.vars = y.vars = NULL;
    x.color_inherited = y.color_inherited = x.color_own = y.color_own = NULL; x.color_scheme_text = y.color_scheme_text = NULL;
    x.grid_cols = y.grid_cols = x.grid_rows = y.grid_rows = NULL;
    x.grid_auto_cols = y.grid_auto_cols = x.grid_auto_rows = y.grid_auto_rows = NULL;
    x.grid_areas = y.grid_areas = NULL;
    for (int i = 0; i < 4; i++) x.grid_place[i].name = y.grid_place[i].name = NULL;
    x.before = y.before = x.after = y.after = x.backdrop = y.backdrop = x.placeholder = y.placeholder = NULL;
    x.bg_img = y.bg_img = 0; /* found from bg_image after the cascade */
    x.border_image_img = y.border_image_img = x.mask_border_img = y.mask_border_img = 0; /* and from the sources */
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
        !!old->filter != !!next->filter || !!old->backdrop_filter != !!next->backdrop_filter ||
        ((old->display == D_INLINE || next->display == D_INLINE) && (old->opacity < 1) != (next->opacity < 1)) ||
        old->container_type != next->container_type ||
        old->overflow != next->overflow || old->content_visibility != next->content_visibility ||
        old->white_space != next->white_space || old->text_transform != next->text_transform ||
        old->list_style != next->list_style || old->list_style_inside != next->list_style_inside ||
        !css_counters_equal(old->counters, next->counters)) return false;
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
    collect_parts(&keys);
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
            "display", "visibility", "clip-rule", "mask-type"
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
    /* the declarations the style attribute stands for: none when CSP blocked it, the exact
       ones when a CSSOM change wrote it (css_inline_text) */
    const char *sa = css_inline_text(e);
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
    bool has_before = false, has_after = false, has_placeholder = false, has_marker = false;
    for (int i = 0; i < ndents; i++) {
        if (dents[i].pseudo == PE_BEFORE) has_before = true;
        if (dents[i].pseudo == PE_AFTER) has_after = true;
        if (dents[i].pseudo == PE_PLACEHOLDER) has_placeholder = true;
        if (dents[i].pseudo == PE_MARKER) has_marker = true;
    }
    /* CSS Transitions 1 §3: the after-change style inherits from the parent's */
    node_t *flat_parent = doc_flat_parent(e);
    const style_t *parent_after = flat_parent && flat_parent->style == parent ? flat_parent->animation_after_style : NULL;
    style_t *base = NULL, *after = NULL;
    style_t *s = motion_compute(c, e, parent, parent_after, PE_NONE, e->style_previous, &base, &after);
    position_try_styles(c, e, parent, PE_NONE, s);
    e->style = s;
    e->animation_base_style = base;
    e->animation_after_style = after;
    /* Only controls with matching authored declarations get a pseudo body.
       Its slot retires with the originating node's other computed styles. */
    if (has_placeholder && !e->foreign && (e->tag == T_textarea ||
        (e->tag == T_input && web_input_type(e) <= WEB_INPUT_PASSWORD)))
        s->placeholder = compute(c, e, s, PE_PLACEHOLDER);
    if (web_dialog_is_modal(d,e)) s->backdrop = compute(c,e,NULL,PE_BACKDROP);
    /* CSS Lists 3 §3.1: a ::marker with declarations of its own; one without takes
       css_style_marker() of its item where it is used (box.c, CSSOM) */
    if (has_marker) s->marker = compute(c, e, s, PE_MARKER);
    if (e == d->html) c->rem = s->font_size;
    /* CSSOM exposes resolved pseudo styles even when content:none, display:none
       or the originating replaced element prevents a generated box. Keep the
       body in the existing style lifetime, not in a second queried-style cache. */
    const style_t *previous = e->style_previous;
    style_t *pseudo_base = NULL, *pseudo_after = NULL;
    if (has_before) {
        s->before = motion_compute(c, e, s, after, PE_BEFORE, previous ? previous->before : NULL, &pseudo_base, &pseudo_after);
        position_try_styles(c, e, s, PE_BEFORE, s->before);
        if (base) base->before = pseudo_base ? pseudo_base : s->before;
    }
    if (has_after) {
        s->after = motion_compute(c, e, s, after, PE_AFTER, previous ? previous->after : NULL, &pseudo_base, &pseudo_after);
        position_try_styles(c, e, s, PE_AFTER, s->after);
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
    if (!sheet_media_ok(s, vw, vh, scripting)) { for (struct rule *r = s->first; r; r = r->next) (*order)++; return true; }
    bool scoped = inspect_scope_ok(s, e); /* else only a ::part() can select e */
    const struct selector_match context = {.shadow = s->scope};
    for (struct rule *r = s->first; r; r = r->next) {
        uint32_t at = (*order)++;
        if (!r->ndecls || !r->sel || !media_chain_ok(r->media, vw, vh, scripting)) continue;
        bool matched = false; uint32_t spec = 0;
        for (int k = 0; k < r->sel->n; k++) {
            const struct selector *x = r->sel->v[k];
            if (x->pseudo || (!scoped && !x->part) || !match_from_bound(x->right, e, &context)) continue;
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
