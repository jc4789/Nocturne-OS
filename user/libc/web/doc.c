#include "form_face.h"
/* The public API (web.h): documents and their resources (stylesheets, images, data: URLs), the
   style/box/layout pipeline, and forms. */
#include <stdio.h>
#include <ctype.h>
#include <math.h>
#include "nocturne.h"
#include "webi.h"
#include "js_atomics.h"
#include <webprefs.h>
#include "web_presentation.h"
#include <webidb_async.h>
#include <webnet.h>
#include <parallel.h>
#include "web_dialog.h"
#include "form_value.h"
#include "form_file.h"
#include "form_validation.h"
#include "elements.h"
#include "js_canvas.h"
#include "image_source.h"
#include "avmedia.h"
#include "js_worker.h"
#include "inspect_private.h"
#include "frame.h"
#include "html_policy.h"
#include "html_encoding.h"
#include "html_lexbor.h"
#include "xml.h"
#include "cssom.h"
#include "web_url.h"
#include <limits.h>

/* Document arenas and stylesheet storage grow through the OS allocator. */
static void invalidate_layout_tree(box_t *b);
static void element_scroll_input_unqueue(node_t *n);
static bool scroll_input_coherent(web_doc *d);

bool doc_viewport_overflow_box(const web_doc *d,const box_t *b) {
    if(!d || !b || !b->node || !d->html || !d->html->style || d->html->style->display==D_NONE)return false;
    node_t *source=d->html;
    /* d->html denotes the document element, and d->body may be a frameset.
       Only an HTML html element propagates from its first displayed HTML body
       child; a foreign lookalike or frameset must not lose its own clipping. */
    if(d->html->type==N_ELEM && !d->html->foreign && d->html->tag==T_html &&
       d->html->style->overflow==OV_VISIBLE)
        for(node_t *child=d->html->first;child;child=child->next)
            if(child->type==N_ELEM && !child->foreign && child->tag==T_body &&
               child->style && child->style->display!=D_NONE){source=child;break;}
    return b->node==source && b==source->box;
}

struct pending {
    char *url;
    double order;
    char *media; /* the <link media> the sheet is wrapped in, or NULL */
    node_t *scope; /* stylesheet owner shadow root, NULL for document */
    node_t *owner; /* the link element that asked, NULL for an @import */
    /* its root sheet's element contributes a script-blocking style sheet
       (doc_style_blocks_scripts) */
    bool blocks_scripts;
};

/* Immutable inline ASTs survive resource snapshots. Each cascade instance
 * still owns its scope/order/media and import traversal; unreferenced entries
 * are reclaimed after the new snapshot is published, not retained forever. */
struct css_sheet_reuse {
    struct css_sheet_reuse *next, *hash_next;
    const char *css, *base, *media;
    size_t n;
    sheet_t *sheet;
    arena_t arena;
    uint64_t hash, key_hash;
    bool used;
};

static uint64_t sheet_hash(const char *text,size_t length) {
    uint64_t hash=UINT64_C(14695981039346656037);
    for(size_t i=0;i<length;i++)hash=(hash^(uint8_t)text[i])*UINT64_C(1099511628211);
    return hash;
}
static void sheet_reuse_reindex(web_doc *d) {
    if(!d->css_reuse_capacity)return;
    memset(d->css_reuse_buckets,0,d->css_reuse_capacity*sizeof *d->css_reuse_buckets);
    for(struct css_sheet_reuse *c=d->css_reuse;c;c=c->next) {
        if(!c->sheet)continue;
        size_t slot=(size_t)c->key_hash&(d->css_reuse_capacity-1);
        c->hash_next=d->css_reuse_buckets[slot];d->css_reuse_buckets[slot]=c;
    }
}
static void sheet_reuse_grow(web_doc *d) {
    size_t capacity=d->css_reuse_capacity;
    if(capacity && (size_t)d->css_reuse_count < capacity-capacity/4)return;
    size_t next=capacity?capacity*2:16;
    if(next<=capacity || next>SIZE_MAX/sizeof *d->css_reuse_buckets)return;
    struct css_sheet_reuse **buckets=calloc(next,sizeof *buckets);
    if(!buckets)return; /* preserve the existing index, or the linked fallback */
    free(d->css_reuse_buckets);d->css_reuse_buckets=buckets;d->css_reuse_capacity=next;
    sheet_reuse_reindex(d);
}
static void sheet_reuse_sweep(web_doc *d,bool all) {
    struct css_sheet_reuse **link=&d->css_reuse;
    while(*link) {
        struct css_sheet_reuse *c=*link;
        if(!all && c->used){link=&c->next;continue;}
        *link=c->next;d->css_reuse_count--;d->css_reuse_bytes-=c->n;
        ar_free(&c->arena);free(c);
    }
    if(!d->css_reuse_count){free(d->css_reuse_buckets);d->css_reuse_buckets=NULL;d->css_reuse_capacity=0;}
    else sheet_reuse_reindex(d); /* no index pointer may borrow a freed arena */
}

static struct cached_css *cached_css(web_doc *d, const char *url) {
    for (int i = 0; i < d->css_cache.n; i++) {
        struct cached_css *c = d->css_cache.v[i];
        if (!strcmp(c->url, url)) return c;
    }
    return NULL;
}

static struct pending *css_queued(web_doc *d, const char *url) {
    for (int i = 0; i < d->pending_css.n; i++)
        if (!strcmp(((struct pending *)d->pending_css.v[i])->url, url)) return d->pending_css.v[i];
    return NULL;
}

/* ---------------------------------------------------------------- data: URLs */
/* decode a data: URL into a malloc'd buffer */
static bool data_url_checkpoint(void *opaque){return web_native_checkpoint(opaque);}
static char *data_url(web_doc *d,const char *u,size_t *out_n,int *status) {
    *out_n=0;struct web_data_url data;int result=web_data_url_process(u,&data,data_url_checkpoint,d);
    if(status)*status=result;if(result!=1)return NULL;
    char *body=(char *)data.body;*out_n=data.length;data.body=NULL;web_data_url_free(&data);return body;
}

/* ---------------------------------------------------------------- collecting resources */
static void text_of(node_t *n, sbuf *b) {
    for (node_t *c = n->first; c; c = c->next)
        if (c->type == N_TEXT) sb_put(b, c->text, c->textlen);
}

static bool media_wanted(const char *m) {
    if (!m || !*m) return true;
    /* print-only sheets are not fetched; anything else is wrapped in @media and evaluated */
    char low[256];
    snprintf(low, sizeof low, "%s", m);
    for (char *p = low; *p; p++) *p = (char)lower((unsigned char)*p);
    /* wanted if any query of the list could match a screen: "not print" can, "print" cannot */
    for (char *q = low; q;) {
        char *comma = strchr(q, ',');
        if (comma) *comma = 0;
        while (is_space((unsigned char)*q)) q++;
        if (!strncmp(q, "not ", 4) || !strstr(q, "print")) return true;
        q = comma ? comma + 1 : NULL;
    }
    return false;
}

struct sheet_walk {
    struct sheet_walk *parent;
    const char *url, *base; /* cached request/final URL, not an inline sheet's base */
    sheet_t *sheet;
    struct cssom_sheet *cssom; /* the CSSOM sheet whose AST it is, if any */
    pvec imports;
    int next;
    sbuf wrapped;
};

static bool sheet_url_equal(const char *a, const char *b) {
    if (!a || !b) return false;
    size_t an = strcspn(a, "#"), bn = strcspn(b, "#");
    return an == bn && !memcmp(a, b, an);
}

static bool sheet_ancestor_url(const struct sheet_walk *f, const char *url) {
    for (; f; f = f->parent)
        if (sheet_url_equal(url, f->url) || sheet_url_equal(url, f->base)) return true;
    return false;
}

/* The vectors expose int indices. Their representable size and real allocation
   failure, rather than a stylesheet-count quota, bound publication. */
static bool sheet_push(pvec *p, void *value) {
    if (p->n == INT_MAX) return false;
    if (p->n == p->cap) {
        int cap = p->cap ? (p->cap > INT_MAX / 2 ? INT_MAX : p->cap * 2) : 16;
        if ((size_t)cap > SIZE_MAX / sizeof *p->v) return false;
        void **v = realloc(p->v, sizeof *p->v * (size_t)cap);
        if (!v) return false;
        p->v = v; p->cap = cap;
    }
    p->v[p->n++] = value;
    return true;
}

static void sheet_walk_free(struct sheet_walk *f) {
    for (; f;) {
        struct sheet_walk *parent = f->parent;
        for (int i = f->next; i < f->imports.n; i++) {
            struct css_import *im = f->imports.v[i];
            free(im->url); free(im);
        }
        pv_free(&f->imports); sb_free(&f->wrapped); free(f);
        f = parent;
    }
}

static bool sheet_instance_imports(web_doc *d, struct sheet_walk *f, sheet_t *ast,
                                   double order, const char *media, node_t *scope) {
    arena_t *arena=d->live?&d->cssmem:&d->mem;
    f->sheet=css_sheet_instance(arena,ast,order,scope);
    if(media && *media)css_sheet_media(f->sheet,ar_strdup(arena,media));
    if(!css_sheet_import_urls(ast))return true; /* no @import: rescans re-instance every sheet, so skip its rule walk */
    for(struct css_rule_info *r=css_sheet_rules(ast,NULL);r;r=r->next)if(r->import&&r->import->url) {
        struct css_import *im=calloc(1,sizeof *im);
        if(!im)return false;
        im->url=strdup(r->import->url);im->order=order;im->owner=r;
        if(!im->url || !sheet_push(&f->imports,im)){free(im->url);free(im);return false;}
    }
    return true;
}

static sheet_t *cached_sheet_ast(web_doc *d, struct cached_css *c) {
    if(c->ast)return c->ast;
    jmp_buf trap;c->ast_mem.trap=&trap;
    if(setjmp(trap)) {
        c->ast_mem.trap=NULL;ar_free(&c->ast_mem);
        web_js_console(d,2,"Completed external stylesheet AST allocation failed");
        if(d->cssmem.trap)longjmp(*d->cssmem.trap,1);
        return NULL;
    }
    sheet_t *ast=css_parse_sheet_serviced(d,&c->ast_mem,c->body,c->n,c->base,0,NULL);
    c->ast_mem.trap=NULL;
    /* The service wrapper has joined its parser worker before returning. A
       cancelled parse may own complete data but must not publish that AST. */
    if(__atomic_load_n(&d->native_cancelled,__ATOMIC_ACQUIRE) || !ast) {
        ar_free(&c->ast_mem);return NULL;
    }
    c->ast=ast;return ast;
}

static bool sheet_parse(web_doc *d, struct sheet_walk *f, const char *css, size_t n,
                        const char *base, double order, const char *media, node_t *scope) {
    arena_t *arena = d->live ? &d->cssmem : &d->mem;
    const char *source = css ? css : "", *source_base = base ? base : "";
    for(int i=0;d->live && i<d->css_cache.n;i++) {
        struct cached_css *c=d->css_cache.v[i];
        if(!c->done || c->body!=css || c->n!=n || strcmp(c->base,source_base))continue;
        sheet_t *ast=cached_sheet_ast(d,c);
        return ast && sheet_instance_imports(d,f,ast,order,media,scope);
    }
    uint64_t hash=sheet_hash(source,n);
    uint64_t base_hash=sheet_hash(source_base,strlen(source_base));
    uint64_t key_hash=hash^(base_hash<<1|base_hash>>63);
    if(d->live)sheet_reuse_grow(d);
    bool indexed=d->css_reuse_capacity!=0;
    struct css_sheet_reuse *candidate=indexed?d->css_reuse_buckets[(size_t)key_hash&(d->css_reuse_capacity-1)]:d->css_reuse;
    for (struct css_sheet_reuse *c = candidate; c; c = indexed?c->hash_next:c->next) {
        if (!c->sheet || hash!=c->hash || n != c->n || strcmp(source_base, c->base) || memcmp(source, c->css, n)) continue;
        c->used=true;
        return sheet_instance_imports(d,f,c->sheet,order,media,scope);
    }
    if(d->live && d->css_reuse_count<UINT_MAX && n<SIZE_MAX && d->css_reuse_bytes<=SIZE_MAX-n) {
        struct css_sheet_reuse *c=calloc(1,sizeof *c);
        if(!c)return false;
        c->n=n;c->hash=hash;c->key_hash=key_hash;c->next=d->css_reuse;d->css_reuse=c;
        d->css_reuse_count++;d->css_reuse_bytes+=n;
        /* The document's existing scan trap owns failure recovery. Link first
         * so a cancelled/failed allocation is reclaimed along with its arena. */
        c->arena.trap=arena->trap;
        c->css=ar_strndup(&c->arena,source,n);c->base=ar_strdup(&c->arena,source_base);
        c->sheet=css_parse_sheet_serviced(d,&c->arena,c->css,n,c->base,0,NULL);
        c->arena.trap=NULL;
        if(!c->sheet || d->native_cancelled)return false;
        if(d->css_reuse_capacity){size_t slot=(size_t)key_hash&(d->css_reuse_capacity-1);c->hash_next=d->css_reuse_buckets[slot];d->css_reuse_buckets[slot]=c;}
        c->used=true;
        return sheet_instance_imports(d,f,c->sheet,order,media,scope);
    }
    sheet_t *sh;
    if (media && *media) {
        sbuf *w = &f->wrapped;
        sb_puts(w, "@media "); sb_puts(w, media); sb_puts(w, "{\n");
        sb_put(w, source, n); sb_puts(w, "\n}");
        sh = css_parse_sheet_serviced(d,arena, w->p, w->n, source_base, order, &f->imports);
        sb_free(w);
    } else sh = css_parse_sheet_serviced(d,arena, source, n, source_base, order, &f->imports);
    css_sheet_scope(sh, scope);
    f->sheet = sh;
    return true;
}

/* Return whether this root has no outstanding critical import fetches. The
   ordinary parser/OOM diagnostics remain separate from resource load state;
   unrelated root sheets must not become this owner's completion barrier.
   blocks: the root sheet's element contributes a script-blocking style sheet,
   which its imports still loading keep blocking. */
static bool add_sheet_ast(web_doc *d, const char *css, size_t n, const char *base, double order, const char *media, node_t *scope, struct cssom_sheet *cssom, bool blocks) {
    struct sheet_walk *volatile active = calloc(1, sizeof(struct sheet_walk));
    if (!active) {
        web_js_console(d, 2, "Stylesheet import traversal allocation failed");
        return true;
    }
    /* Inline sheets use their document URL for resolution, but are not an
       imported ancestor of every stylesheet fetched from that document URL. */
    for (int i = 0; d->live && i < d->css_cache.n; i++) {
        struct cached_css *c = d->css_cache.v[i];
        if (c->body == css && c->n == n) { active->url = c->url; active->base = c->base; break; }
    }
    arena_t *arena = d->live ? &d->cssmem : &d->mem;
    volatile bool pending_import=false;
    jmp_buf trap;
    jmp_buf *outer_trap = arena->trap;
    arena->trap = &trap;
    int arena_failed = setjmp(trap);
    bool failed = false;
    if (arena_failed) {
        sheet_walk_free(active);
        arena->trap = outer_trap;
        if (outer_trap) longjmp(*outer_trap, arena_failed);
        web_js_console(d, 2, "Stylesheet arena allocation failed during import traversal");
        d->need_style = true;
        return !pending_import;
    }
    if (cssom) {
        active->cssom=cssom;
        if(!sheet_instance_imports(d,active,cssom->p.ast,order,media,scope))failed=true;
    } else if (!sheet_parse(d, active, css, n, base, order, media, scope)) failed = true;
    while (!failed && active) {
        struct sheet_walk *f = active;
        if (f->next == f->imports.n) {
            /* Postorder is CSS import source order. All members of this root
               sheet use its order key; the stable cascade sort then preserves
               imports before the parent, without a shrinking floating-point
               depth interval that can cross an earlier root stylesheet. */
            if (f->url) css_sheet_href(f->sheet, ar_strdup(arena, f->url)); /* DevTools rule origin */
            if (!sheet_push(&d->sty.sheets, f->sheet)) { failed = true; break; }
            d->sty.index_dirty = true;
            active = f->parent; f->parent = NULL; sheet_walk_free(f);
            continue;
        }
        /* Keep the parent slot until child creation succeeds: an arena failure
           then cleans the current import as well as the remaining siblings. */
        struct css_import *im = f->imports.v[f->next];
        struct cached_css *cached = d->live ? cached_css(d, im->url) : NULL;
        bool cycle = sheet_ancestor_url(f, im->url) ||
                     (cached && sheet_ancestor_url(f, cached->base));
        if (cycle) { f->next++; free(im->url); free(im); continue; }
        /* An @import rule CSSOM gave a sheet of its own (CSSImportRule.styleSheet)
           cascades that sheet's AST, which its changes reach; a disabled one none. */
        struct cssom_sheet *own = f->cssom && im->owner ? cssom_import_child(f->cssom, im->owner->id) : NULL;
        if (own) {
            const struct css_rule_info *import_owner=im->owner;
            f->next++; free(im->url); free(im);
            if (own->disabled) continue;
            struct sheet_walk *child = calloc(1, sizeof *child);
            if (!child) { failed = true; break; }
            child->parent = f; child->url = own->href; child->base = own->base; child->cssom = own;
            active = child;
            if (!sheet_instance_imports(d, child, own->p.ast, order, media, scope)) failed = true;
            if (!failed) css_sheet_import_context(child->sheet, f->sheet, import_owner);
            continue;
        }
        if (cached && cached->done) {
            if (cached->body) {
                struct sheet_walk *child = calloc(1, sizeof *child);
                if (!child) { failed = true; break; }
                child->parent = f; child->url = cached->url; child->base = cached->base;
                const struct css_rule_info *import_owner=im->owner;
                f->next++; free(im->url); free(im);
                active = child;
                if (!sheet_parse(d, child, cached->body, cached->n, cached->base, order, media, scope)) failed = true;
                if(!failed)css_sheet_import_context(child->sheet,f->sheet,import_owner);
            } else { f->next++; free(im->url); free(im); }
            continue;
        }
        struct pending *queued = d->live ? css_queued(d, im->url) : NULL;
        if (queued) {
            queued->blocks_scripts |= blocks;
            pending_import=true;
            f->next++; free(im->url); free(im);
            continue;
        }
        struct pending *p = calloc(1, sizeof *p);
        if (!p) { failed = true; break; }
        p->url = im->url; p->order = im->order; p->scope = scope; p->blocks_scripts = blocks;
        p->media = media && *media ? strdup(media) : NULL;
        if ((media && *media && !p->media) || !sheet_push(&d->pending_css, p)) {
            free(p->media); free(p); failed = true; break;
        }
        pending_import=true;
        f->next++; free(im); /* URL ownership has moved to pending_css. */
    }
    sheet_walk_free(active);
    arena->trap = outer_trap;
    d->need_style = true;
    if (failed) web_js_console(d, 2, "Stylesheet import traversal allocation or index representation failed");
    return !pending_import;
}
static bool add_sheet(web_doc *d,const char *css,size_t n,const char *base,double order,const char *media,node_t *scope,bool blocks) {
    return add_sheet_ast(d,css,n,base,order,media,scope,NULL,blocks);
}

void doc_add_stylesheet_text(web_doc *d, const char *css, size_t n, const char *base) {
    add_sheet(d, css, n, base, d->next_sheet_order++, NULL, NULL, false);
}

/* The document's character encoding, the URL parser's for a query (failure,
   an empty name, is UTF-8). */
static web_encoding doc_url_encoding(const web_doc *d) {
    return web_encoding_get(d->encoding, strlen(d->encoding));
}
int doc_encoding_parse_url(web_doc *d, const char *input, char **out) {
    return web_url_resolve(d->base, input, doc_url_encoding(d), out);
}
bool doc_set_base(web_doc *d, const char *url) {
    if (d->base && !strcmp(d->base, url)) return true;
    char *copy = strdup(url);
    if (!copy) return false;
    free(d->base); d->base = copy;
    return true;
}
/* HTML's encoding-parse-and-serialize a URL relative to d: NULL for failure
   (or out of memory). */
static char *doc_absolute_url(web_doc *d,const char *relative) {
    char *url=NULL;
    return doc_encoding_parse_url(d,relative,&url)==1?url:NULL;
}
/* HTML's "set the frozen base URL": href against the fallback base URL with
   the document's character encoding; failure, a data: or a javascript: URL
   leaves the fallback. */
/* CSP: an element's check, once for each new text (webi.h's csp_checked). */
static bool element_csp_checked(node_t *n,const char *text,size_t length,bool *allowed) {
    uint64_t hash=sheet_hash(text,length);
    if(n->csp_checked && n->csp_checked_hash==hash && n->csp_checked_length==length){*allowed=n->csp_checked==1;return true;}
    n->csp_checked_hash=hash;n->csp_checked_length=length;n->csp_checked=0;return false;
}
static bool element_csp_answer(node_t *n,bool allowed){n->csp_checked=allowed?1:2;return allowed;}
static bool document_has_csp(const web_doc *d){return d->html_policy && d->html_policy->first;}
/* HTML "set the frozen base URL": a URL that cannot be parsed, or that CSP's
   "Is base allowed for Document?" blocks, leaves the fallback base URL. */
static void doc_frozen_base(web_doc *d,node_t *base) {
    const char *fallback=web_effective_url(d),*href=node_attr(base,"href");
    char *url=NULL;
    if(web_url_resolve(fallback,href?href:"",doc_url_encoding(d),&url)!=1 ||
       !strncmp(url,"data:",5) || !strncmp(url,"javascript:",11)){free(url);url=NULL;}
    if(url && document_has_csp(d)){
        bool allowed;
        if(!element_csp_checked(base,url,strlen(url),&allowed))
            allowed=element_csp_answer(base,web_js_csp_base_allowed(d,base,url));
        if(!allowed){free(url);url=NULL;}
    }
    if(!doc_set_base(d,url?url:fallback))web_js_console(d,2,"Document base URL allocation failed");
    free(url);
}
bool doc_style_csp_allowed(web_doc *d,node_t *n,const char *text,size_t length) {
    if(!d || !n || !document_has_csp(d))return true;
    sbuf b={0};
    if(!text){
        text_of(n,&b); /* the child text content */
        if(!sb_cstr(&b))return false; /* no memory to read it: blocked */
        text=b.p;length=b.n;
    }
    bool allowed;
    if(!element_csp_checked(n,text,length,&allowed))
        allowed=element_csp_answer(n,web_js_csp_style_allowed(d,n,false,text,length));
    sb_free(&b);return allowed;
}
static int add_image_url(web_doc *d,const char *abs,bool retry_failed,int preferred,bool image_upgrade,int cors,int referrer_policy) {
    if (preferred >= 0 && preferred < d->images.n) {
        struct web_image *im=d->images.v[preferred];
        if(im->image_upgrade==image_upgrade && im->cors==cors && im->referrer_policy==referrer_policy && !strcmp(im->url,abs))return preferred;
    }
    int failed_match = -1;
    for (int i = 0; i < d->images.n; i++) {
        struct web_image *im = d->images.v[i];
        if (im->image_upgrade!=image_upgrade || im->cors!=cors || im->referrer_policy!=referrer_policy || strcmp(im->url, abs)) continue;
        if (!im->done || !im->failed) return i;
        if (failed_match < 0) failed_match = i;
    }
    if (failed_match >= 0 && !retry_failed) return failed_match;
    if (d->images.n == INT_MAX) return -1;
    struct web_image *im = calloc(1, sizeof *im);
    if (!im) return -1;
    im->url = strdup(abs);
    im->image_upgrade = image_upgrade;
    im->cors = (uint8_t)cors;
    im->referrer_policy = (uint8_t)referrer_policy;
    if (!im->url) { free(im); return -1; }
    if (d->images.n == d->images.cap) {
        int cap = d->images.cap ? d->images.cap > INT_MAX/2 ? INT_MAX : d->images.cap * 2 : 16;
        if((size_t)cap > SIZE_MAX/sizeof(void *)){free(im->url);free(im);return -1;}
        void **items = realloc(d->images.v, sizeof(void *) * (size_t)cap);
        if (!items) { free(im->url); free(im); return -1; }
        d->images.v = items; d->images.cap = cap;
    }
    d->images.v[d->images.n++] = im;
    int idx = d->images.n - 1;
    if (!strncasecmp(abs, "data:", 5)) {
        size_t n;
        char *data = data_url(d,abs,&n,NULL);
        web_image_loaded(d, idx, data, n, true); /* a data: response is basic */
        free(data);
    }
    return idx;
}
/* An image's URL is HTML's (encoding-parse a URL relative to d), or with
   css CSS's url() (UTF-8 against d's base URL). */
static int add_image_request(web_doc *d, const char *rel, bool css, bool retry_failed, int preferred,bool image_upgrade,int cors,int referrer_policy) {
    if(!rel || !*rel)return -1;
    while(is_space((unsigned char)*rel))rel++;
    char *abs=NULL;
    if((css?web_resolve_url_owned(d->base,rel,&abs):doc_encoding_parse_url(d,rel,&abs))!=1)return -1;
    int result=add_image_url(d,abs,retry_failed,preferred,image_upgrade,cors,referrer_policy);free(abs);return result;
}
static int add_image(web_doc *d, const char *rel, bool css) { return add_image_request(d, rel, css, false, -1,true,WEB_IMAGE_NO_CORS,WEBNET_REFERRER_DEFAULT); }
/* a CSS <image>'s url() in the document's images: cors WEB_IMAGE_NO_CORS (background-image,
   list-style-image), or a CORS request's credentials mode (mask images, css-masking-1 §7.1: cors
   mode, same-origin credentials; a response that fails the check is not drawn) */
int doc_css_image(web_doc *d, const struct css_image *image, int cors) {
    if (!d || !image || image->kind != CSS_IMAGE_URL) return -1;
    struct css_image *cached = (struct css_image *)image; /* the value's cache of its document image */
    if (!cached->img) {
        int i = add_image_request(d, image->url, true, false, -1, cors == WEB_IMAGE_NO_CORS, cors, WEBNET_REFERRER_DEFAULT);
        cached->img = i >= 0 ? i + 1 : -1;
    }
    return cached->img > 0 ? cached->img - 1 : -1;
}
int doc_element_referrer_policy(const node_t *n) {
    const char *value=n&&n->type==N_ELEM?node_attr((node_t *)n,"referrerpolicy"):NULL;
    char token[40];size_t length=value?strlen(value):0;
    if(!value||length>=sizeof token)return WEBNET_REFERRER_DEFAULT;
    /* An enumerated attribute: ASCII case-insensitive. */
    for(size_t i=0;i<length;i++)token[i]=(char)lower((unsigned char)value[i]);
    return webnet_referrer_policy_token(token,length);
}
/* The crossorigin attribute's state: missing is no CORS, an invalid value anonymous. */
static int image_cors(node_t *n) {
    const char *v = node_attr(n, "crossorigin");
    return !v ? WEB_IMAGE_NO_CORS : str_ieq(v, "use-credentials") ? WEB_IMAGE_USE_CREDENTIALS : WEB_IMAGE_ANONYMOUS;
}

/* Element creation links each <img>/<input> into its allocation's image list,
 * a subset of owned_nodes: the iterator below runs on every script tick and
 * must not visit every node of a large document to find a few images. */
void doc_image_node_owned(node_t *n) {
    if (n->type != N_ELEM || (n->tag != T_img && n->tag != T_input)) return;
    web_doc *allocation = n->allocation_doc;
    n->owned_image_next = allocation->owned_images;
    allocation->owned_images = n;
}

/* Allocation lists keep their lifetime owner even after DOM adoption. The
 * family contains at most 64 auxiliary documents (doc_inert's existing cap).
 * Resource users must enumerate logical owners, not only d's own arena.
 * previous must be NULL or a node returned by this iterator. */
node_t *doc_image_node_next(web_doc *d, node_t *previous) {
    if (!d || d->inert) return NULL;
    web_doc *family = d->dom_family ? d->dom_family : d;
    web_doc *allocation = previous ? previous->allocation_doc : family;
    node_t *n = previous ? previous->owned_image_next : allocation->owned_images;
    while (allocation) {
        for (; n; n = n->owned_image_next)
            if (n->owner == d && !n->foreign) return n;
        allocation = allocation == family ? family->dom_docs : allocation->dom_next;
        n = allocation ? allocation->owned_images : NULL;
    }
    return NULL;
}

/* Both connected and script-created detached HTML images have image requests.
 * Preserve an available current image while its replacement is pending. */
void doc_image_sync(web_doc *d, node_t *n) {
    if (!d || d->inert) return;
    if (!d || !n || n->type != N_ELEM || n->foreign || n->tag != T_img || n->owner != d) return;
    for (node_t *p = n; p; p = p->parent ? p->parent : p->template_host) {
        if (p->tag == T_template || p->template_host) return;
    }
    const char *src = node_attr(n, "src"), *ss = node_attr(n, "srcset");
    char *pick = NULL;
    /* HTML's source selection uses src/srcset/picture, not a guessed meaning
       for author data-* attributes or a guessed "placeholder" data URL. */
    pick=web_image_source_pick(d,n,src);
    if(pick)src=pick;
    bool has_source = src != NULL || ss != NULL;
    /* Keep responsive/CORS images outside this ordinary-image upgrade path.
       In particular, URL deduplication must not promote a blocked imageset. */
    int cors = image_cors(n);
    bool image_upgrade=!ss && cors==WEB_IMAGE_NO_CORS &&
        !(n->parent && !n->parent->foreign && n->parent->tag==T_picture);
    int requested = add_image_request(d, src, false, n->image_invalidated,
        n->image_initialized && !n->image_invalidated ? n->image_request : -1,image_upgrade,cors,doc_element_referrer_policy(n));
    free(pick);
    if (!n->image_initialized || n->image_invalidated || requested != n->image_request || has_source != n->image_has_source) {
        n->image_initialized = true; n->image_invalidated = false;
        n->image_request = requested; n->image_has_source = has_source;
        n->image_generation++;
        struct web_image *old = n->image >= 0 && n->image < d->images.n ? d->images.v[n->image] : NULL;
        struct web_image *next = requested >= 0 ? d->images.v[requested] : NULL;
        if (!next || next->done || !old || !old->img) n->image = requested;
    }
    if (n->image_request >= 0 && ((struct web_image *)d->images.v[n->image_request])->done)
        n->image = n->image_request;
}

void doc_control_init(web_doc *d, node_t *n) {
    if (n->tag == T_select) {
        int selected = n->selected;
        web_select_sync(d, n, !n->control_ready);
        if (n->selected != selected) css_mark_form_state(d, n); /* its options' :checked */
        return;
    }
    if (n->control_ready) return;
    n->control_ready = true;
    if (n->tag == T_input) {
        const char *v = node_attr(n, "value");
        doc_node_value(d, n, v ? v : "", v ? strlen(v) : 0);
        n->value_dirty = false;
        n->checked = node_attr(n, "checked") != NULL;
    } else if (n->tag == T_textarea) {
        sbuf b = {0};
        node_text_content(n, &b);
        doc_node_value(d, n, b.p ? b.p : "", b.n);
        n->value_dirty = false;
        sb_free(&b);
    }
}

static bool link_stylesheet(node_t *n) {
    if (!n || n->type != N_ELEM || n->foreign || n->tag != T_link) return false;
    const char *rel = node_attr(n,"rel"), *type = node_attr(n,"type");
    if (!rel || (type && *type && !str_ieq(type,"text/css"))) return false;
    bool sheet = false;
    while (*rel) {
        while (is_space((unsigned char)*rel)) rel++;
        const char *end = rel; while (*end && !is_space((unsigned char)*end)) end++;
        if (end-rel == 10 && strn_ieq(rel,"stylesheet",10)) sheet = true;
        rel = end;
    }
    return sheet;
}
/* its style sheet is enabled: not disabled, not an alternative style sheet */
static bool link_sheet_enabled(node_t *n) {
    if (cssom_owner_disabled(n)) return false;
    const char *rel=node_attr(n,"rel");
    while (rel && *rel) {
        while(is_space((unsigned char)*rel))rel++;
        const char *end=rel;while(*end&&!is_space((unsigned char)*end))end++;
        if(end-rel==9&&strn_ieq(rel,"alternate",9))return false;
        rel=end;
    }
    return true;
}
static bool link_sheet_applies(node_t *n) {
    return link_sheet_enabled(n) && media_wanted(node_attr(n,"media"));
}
/* the media attribute of a pending sheet's root element matches the environment */
static bool pending_media_matches(web_doc *d, const struct pending *p) {
    return !p->media || !*p->media || css_media_evaluate(p->media,
        MAX(1,d->width),MAX(1,d->height),!(d->sandbox_flags&SB_SCRIPTS),NULL,0);
}
/* HTML's initial render-blocking mechanism. A short paint fallback cannot
   abort a fetch, suppress an error, or limit the size/lifetime of the page.
   Inactive/alternate/disabled sheets, body images and author Fetch promises
   are not blockers. After first paint, resource updates remain progressive. */
bool web_render_blocked(web_doc *d) {
    if (!d || !d->live || d->inert || d->initial_render_done ||
        uptime_ms() >= d->initial_render_deadline) return false;
    if (!d->body && d->parser) return true;
    for (int i=0; i<d->pending_css.n; i++) {
        const struct pending *p=d->pending_css.v[i];
        if (p->scope || p->order >= d->render_head_order) continue;
        if (!pending_media_matches(d,p)) continue;
        const struct cached_css *c=cached_css(d,p->url);
        if (!c || !c->done) return true;
    }
    return false;
}
/* HTML "contributes a script-blocking style sheet" asks what the element was
   when the parser created it: a style element, or a link that contributed to
   the styling processing model with its style sheet enabled (no disabled
   attribute, not an alternative style sheet). */
void doc_parser_element_created(node_t *n) {
    if (!n || n->type != N_ELEM || n->foreign) return;
    n->parser_sheet = n->tag == T_style || (link_stylesheet(n) && link_sheet_enabled(n));
}
/* A document's script-blocking style sheet set is its parser-created sheets
   (doc_parser_element_created) still in its tree, outside shadow trees, whose
   media matches the environment, while the sheet or one of its imports is
   loading. A rescan rebuilds pending_css; until then a loaded entry keeps
   blocking, so the imports it brings are queued before a script runs. */
static bool script_blocking_set_empty(web_doc *d) {
    for (int i=0; i<d->pending_css.n; i++) {
        const struct pending *p=d->pending_css.v[i];
        if (p->blocks_scripts && pending_media_matches(d,p)) return false;
    }
    return true;
}
bool doc_style_blocks_scripts(web_doc *d) {
    if (!d) return false;
    if (!script_blocking_set_empty(d)) return true;
    /* and its container document's set (HTML "has a style sheet that is blocking scripts" 3-4) */
    return d->frame_parent && !script_blocking_set_empty(d->frame_parent);
}
/* Borrow only a completed native resource, never a fabricated empty sheet.
   URL/data scratch is caller-owned; cache text/base survives the document.
   source->url is the sheet's absolute URL, which the source then owns. */
static int cssom_url_source(web_doc *d,struct cssom_link_source *source) {
    if(!strncasecmp(source->url,"data:",5)) {
        int result;source->owned_text=data_url(d,source->url,&source->length,&result);
        if(!source->owned_text){free(source->url);memset(source,0,sizeof *source);return result==-1?CSSOM_OOM:CSSOM_INACTIVE;}
        source->text=source->owned_text;source->base=d->base;return CSSOM_OK;
    }
    struct cached_css *cached=cached_css(d,source->url);
    if(!cached||!cached->done||!cached->body){free(source->url);memset(source,0,sizeof *source);return CSSOM_INACTIVE;}
    source->text=cached->body;source->length=cached->n;source->base=cached->base;return CSSOM_OK;
}
int doc_cssom_link_source(web_doc *d,node_t *n,struct cssom_link_source *source) {
    memset(source,0,sizeof *source);
    if(!d||!n||n->owner!=d||!d->live||d->inert||!link_stylesheet(n)||
        doc_node_root(n,true)!=d->root||node_ancestor(n,T_template))return CSSOM_INACTIVE;
    const char *href=node_attr(n,"href");if(!href||!*href)return CSSOM_INACTIVE;
    int resolved=doc_encoding_parse_url(d,href,&source->url);
    if(resolved!=1)return resolved<0?CSSOM_OOM:CSSOM_INACTIVE;
    return cssom_url_source(d,source);
}
int doc_cssom_import_source(web_doc *d,const char *url,struct cssom_link_source *source) {
    memset(source,0,sizeof *source);
    if(!d||!url||!d->live||d->inert)return CSSOM_INACTIVE;
    if(!(source->url=strdup(url)))return CSSOM_OOM;
    return cssom_url_source(d,source);
}
bool doc_link_load_current(web_doc *d, node_t *n, uint64_t generation) {
    if (!d || !d->live || d->inert || !n || n->owner != d ||
        doc_node_root(n,true) != d->root || n->stylesheet_owner != d || n->stylesheet_generation != generation ||
        !n->stylesheet_url || !link_stylesheet(n)) return false;
    const char *href = node_attr(n,"href");
    char *url = href && *href ? doc_absolute_url(d,href) : NULL;
    bool current = url && !strcmp(url,n->stylesheet_url);
    free(url); return current;
}
static void link_stylesheet_complete(web_doc *d, node_t *n, bool failed) {
    if (d->live && !n->stylesheet_notified && web_js_stylesheet_event(d,n,failed))
        n->stylesheet_notified = true;
}
static void scan(web_doc *d, node_t *n, node_t *scope) {
    for (node_t *c = n->first; c; c = c->next) {
        if (!web_native_checkpoint(d)) return;
        if (c->type != N_ELEM) continue;
        if (c->foreign) {
            scan(d, c, scope);
            if (c->shadow_root) scan(d, c->shadow_root, c->shadow_root);
            continue;
        }
        if(c->tag==T_marquee)web_marquee_register(d,c);
        switch (c->tag) {
        case T_base: {
            if (scope) break;
            const char *h = node_attr(c, "href");
            if (h && !d->base_seen) { /* only the first <base> counts */
                doc_frozen_base(d,c);
                d->base_seen = true;
            }
            break;
        }
        case T_title:
            if (!scope && (!d->live ? !d->title : !d->scan_title_seen) && !node_ancestor(c, T_svg)) {
                d->scan_title_seen = true;
                sbuf b = {0};
                text_of(c, &b);
                /* collapse white space */
                char *t = sb_cstr(&b);
                size_t k = 0;
                bool sp = true;
                for (size_t i = 0; i < b.n; i++) {
                    if (is_space((unsigned char)b.p[i])) {
                        if (!sp) t[k++] = ' ';
                        sp = true;
                    } else t[k++] = b.p[i], sp = false;
                }
                while (k && t[k - 1] == ' ') k--;
                t[k] = 0;
                if (!d->title || strcmp(d->title, t)) d->title = ar_strdup(&d->mem, t);
                sb_free(&b);
            }
            break;
        case T_meta: {
            if (scope) break;
            if(!c->referrer_meta_processed)doc_referrer_meta(d,c);
            const char *he = node_attr(c, "http-equiv"), *ct = node_attr(c, "content");
            if (he && ct && str_ieq(he, "refresh") && !d->refresh_url) {
                d->refresh_delay = atoi(ct);
                const char *u = ct;
                while (*u && *u != ';' && *u != ',') u++;
                while (*u == ';' || *u == ',' || is_space((unsigned char)*u)) u++;
                if (!strncasecmp(u, "url", 3)) {
                    u += 3;
                    while (is_space((unsigned char)*u)) u++;
                    if (*u == '=') u++;
                    while (is_space((unsigned char)*u)) u++;
                }
                if(strlen(u)>=HTTP_URL_MAX)break;
                char *tmp=strdup(u);if(!tmp)break;
                size_t tl = strlen(tmp);
                if (tl && (tmp[0] == '\'' || tmp[0] == '"')) {
                    memmove(tmp, tmp + 1, tl);
                    char *q = strpbrk(tmp, "'\"");
                    if (q) *q = 0;
                }
                char *abs=doc_absolute_url(d,*tmp?tmp:d->url);
                if(abs)d->refresh_url=ar_strdup(&d->mem,abs);
                free(abs);free(tmp);
            }
            break;
        }
        case T_link: {
            const char *href = node_attr(c, "href");
            const char *media = node_attr(c, "media");
            if (!href || !*href || !link_stylesheet(c)) {
                if (c->stylesheet_url) c->stylesheet_generation++;
                c->stylesheet_url = NULL; c->stylesheet_notified = false; break;
            }
            /* A static parse has no CSSOM observer or future media changes.
               Preserve its screen-only pending-resource API, while live
               documents may fetch inactive sheets for CSSOM/load events. */
            if (!d->live && !link_sheet_applies(c)) break;
            /* HTML "contributes a script-blocking style sheet"; its media is
               matched when scripts ask (doc_style_blocks_scripts) */
            bool blocks = c->parser_sheet && c->allocation_doc == d && !scope;
            char *abs=doc_absolute_url(d,href);if(!abs)break;
            if (d->live && (c->stylesheet_owner != d || !c->stylesheet_url || strcmp(c->stylesheet_url,abs))) {
                c->stylesheet_url = ar_strdup(&d->mem,abs);
                c->stylesheet_owner = d;
                c->stylesheet_generation++; c->stylesheet_notified = false;
            }
            struct cached_css *cached = d->live ? cached_css(d, abs) : NULL;
            if (cached && cached->done) {
                bool complete=true;
                if (cached->body && link_sheet_applies(c)) {
                    if(c->cssom_current) {
                        int error;struct cssom_sheet *sheet=cssom_style_sheet(d,c,&error);
                        if(sheet)complete=add_sheet_ast(d,sheet->p.source,sheet->p.length,sheet->base,d->next_sheet_order++,sheet->media,scope,sheet,blocks);
                        else if(error==CSSOM_OOM)web_js_console(d,2,"Native link style sheet allocation failed");
                    }else complete=add_sheet(d,cached->body,cached->n,cached->base,d->next_sheet_order++,media,scope,blocks);
                }
                if(complete)link_stylesheet_complete(d,c,!cached->body);
                free(abs);
                break;
            }
            if(cssom_owner_disabled(c)){free(abs);break;}
            struct pending *queued = d->live ? css_queued(d, abs) : NULL;
            if (queued) {queued->blocks_scripts |= blocks;free(abs);break;}
            struct pending *pd = calloc(1, sizeof *pd);
            if(!pd){free(abs);break;}
            pd->url = abs;
            pd->order = d->next_sheet_order++;
            pd->media = media && *media ? strdup(media) : NULL;
            pd->scope = scope;
            pd->owner = c;
            pd->blocks_scripts = blocks;
            if (!strncasecmp(abs, "data:", 5)) {
                size_t dn;
                char *css = data_url(d,abs,&dn,NULL);
                bool complete=true;
                if (css && link_sheet_applies(c)) {
                    if(c->cssom_current) {
                        int error;struct cssom_sheet *sheet=cssom_style_sheet(d,c,&error);
                        if(sheet)complete=add_sheet_ast(d,sheet->p.source,sheet->p.length,sheet->base,pd->order,sheet->media,scope,sheet,blocks);
                        else if(error==CSSOM_OOM)web_js_console(d,2,"Native link style sheet allocation failed");
                    }else complete=add_sheet(d,css,dn,d->base,pd->order,pd->media,scope,blocks);
                }
                if(complete)link_stylesheet_complete(d,c,!css);
                free(css);
                free(pd->url);
                free(pd->media);
                free(pd);
            } else pv_push(&d->pending_css, pd);
            break;
        }
        case T_style: {
            const char *type = node_attr(c, "type");
            const char *media = node_attr(c, "media");
            /* A sheet CSSOM stands for applies by its MediaList, which starts
               as the media attribute and scripts may change. */
            bool cssom = d->live && c->cssom_current;
            if (c->style_disabled || (type && *type && !str_ieq(type, "text/css")) ||
                (!cssom && !media_wanted(media)) || node_ancestor(c, T_template)) break;
            /* its imports still loading block scripts as a link's sheet does */
            bool blocks = c->parser_sheet && c->allocation_doc == d && !scope;
            /* Keep the existing per-snapshot native AST reuse for untouched
               sheets. A real CSSOM handle lazily promotes only its owner to
               persistent parser storage, not every repeated component style. */
            if (cssom) {
                int error; struct cssom_sheet *sheet=cssom_style_sheet(d,c,&error);
                if (sheet) add_sheet_ast(d,sheet->p.source,sheet->p.length,sheet->base,d->next_sheet_order++,sheet->media,scope,sheet,blocks);
                else if (error==CSSOM_OOM) web_js_console(d,2,"Native style sheet allocation failed");
            } else {
                sbuf b = {0}; text_of(c,&b);
                /* HTML "update a style block": CSP's inline check for the
                   child text (cssom_style_sheet makes it for a CSSOM sheet). */
                if (doc_style_csp_allowed(d,c,b.p ? b.p : "",b.n))
                    add_sheet(d,b.p ? b.p : "",b.n,d->base,d->next_sheet_order++,media,scope,blocks);
                sb_free(&b);
            }
            break;
        }
        case T_img: doc_image_sync(d, c); break;
        case T_input: {
            const char *t = node_attr(c, "type");
            if (t && str_ieq(t, "image")) c->image = add_image(d, node_attr(c, "src"), false);
            doc_control_init(d, c);
            break;
        }
        case T_textarea: doc_control_init(d, c); break;
        case T_select:
            if (!c->selected_set) c->control_ready = false;
            doc_control_init(d, c);
            break;
        }
        if (c->tag != T_template) {
            scan(d, c, scope);
            if (c->shadow_root) scan(d, c->shadow_root, c->shadow_root);
        }
    }
    if (n == d->head && !scope) d->render_head_order=d->next_sheet_order;
    /* Adopted sheets follow tree-owned sheets in this scope. Their persistent
       native AST is instantiated, never reparsed for each component root. */
    if (n == d->root || n->shadow_host) {
        for (uint32_t i = 0; i < n->adopted_count; i++) {
            struct cssom_sheet *s = n->adopted_sheets[i];
            if (!s->disabled) add_sheet_ast(d, s->p.source, s->p.length, s->base,
                d->next_sheet_order++, s->media, scope, s, false);
        }
    }
}

/* ---------------------------------------------------------------- documents */
void doc_referrer_meta(web_doc *d,node_t *n) {
    if(!d||!n||n->type!=N_ELEM||n->foreign||n->tag!=T_meta||doc_node_root(n,false)!=d->root||node_ancestor(n,T_template))return;
    const char *name=node_attr(n,"name"),*content=node_attr(n,"content");
    if(!name||!content||!str_ieq(name,"referrer"))return;
    n->referrer_meta_processed=true;
    size_t length=strlen(content);char token[40];
    while(length&&is_space((unsigned char)*content))content++,length--;
    while(length&&is_space((unsigned char)content[length-1]))length--;
    if(length>=sizeof token)return;
    for(size_t i=0;i<length;i++)token[i]=(char)lower((unsigned char)content[i]);token[length]=0;
    int policy=webnet_referrer_policy_token(token,length);
    /* HTML's legacy-token mode applies to meta, never HTTP policy fields. */
    if(!policy){if(!strcmp(token,"never"))policy=WEBNET_REFERRER_NONE;
        else if(!strcmp(token,"default"))policy=WEBNET_REFERRER_DOWNGRADE;
        else if(!strcmp(token,"always"))policy=WEBNET_REFERRER_UNSAFE;
        else if(!strcmp(token,"origin-when-crossorigin"))policy=WEBNET_REFERRER_ORIGIN_CROSS;}
    if(policy)d->referrer_policy=policy;
}
size_t doc_dom_remaining(web_doc *d) {
    /* Accounting headroom, not a promise of physically available RAM. */
    if (!d || d->control_bytes > SIZE_MAX - d->mem.allocated) return 0;
    return SIZE_MAX - d->mem.allocated - d->control_bytes;
}
void doc_dom_budget(web_doc *d) {
    if (d) d->mem.limit = 0; /* arena zero: actual allocator/overflow only */
}
web_doc *doc_inert(web_doc *family, const char *html, size_t n, const char *url) {
    return doc_inert_ex(family, html, n, url, false);
}
web_doc *doc_inert_new(web_doc *family, const char *url) {
    if (!family) return NULL;
    int referrer_policy=family->referrer_policy;
    family = family->dom_family ? family->dom_family : family;
    web_doc *d = calloc(1, sizeof *d);
    if (!d) return NULL;
    d->inert = true; d->custom_registry_id = -1; d->dom_family = family;
    d->referrer_policy=referrer_policy;
    d->url = strdup(url && *url ? url : "about:blank");
    d->base = d->url ? strdup(d->url) : NULL;
    if (!d->base) { free(d->url); free(d); return NULL; }
    family->dom_family = family;
    d->dom_next = family->dom_docs; family->dom_docs = d;
    doc_dom_budget(d);
    return d;
}
void doc_inert_discard(web_doc *d) {
    web_doc *family = d->dom_family, *previous = d->dom_next;
    /* No wrappers have escaped: discard every nested template document too. */
    while (family->dom_docs != previous) {
        web_doc *bad = family->dom_docs;
        family->dom_docs = bad->dom_next; bad->dom_next = NULL;
        web_free(bad);
    }
}
web_doc *doc_inert_ex(web_doc *family, const char *html, size_t n, const char *url, bool allow_shadow) {
    if (!family || n == SIZE_MAX || (n && !html)) return NULL;
    web_doc *d = doc_inert_new(family, url);
    if (!d) return NULL;
    d->root = html_parse_string(d, html ? html : "", n, allow_shadow);
    if (!d->root) { doc_inert_discard(d); return NULL; }
    d->resources_dirty = true;
    return d;
}
/* XMLHttpRequest "set a document response", HTML: the received bytes in the
   final encoding label, else the prescan's, else UTF-8, parsed with scripting
   and declarative shadow roots disabled. */
web_doc *doc_inert_bytes(web_doc *family, const char *bytes, size_t n, const char *label,
                         const char *url, const char *content_type) {
    if (!family || (n && !bytes)) return NULL;
    char encoding[64];
    if (!label || !html_encoding_label(label, strlen(label), encoding, sizeof encoding))
        label = html_prescan_label(bytes, n, encoding, sizeof encoding) ? encoding : "UTF-8";
    else label = encoding;
    web_doc *d = doc_inert_new(family, url);
    if (!d) return NULL;
    d->content_type = strdup(content_type && *content_type ? content_type : "text/html");
    struct html_parser *p = d->content_type ? html_begin(d, bytes ? bytes : "", n, label, false) : NULL;
    if (!p) { doc_inert_discard(d); return NULL; }
    html_declarative_shadow_set(p, false);
    node_t *ignored; int result;
    do result = html_resume(p, &ignored); while (result == HTML_PARSE_YIELD);
    html_finish(p);
    if (result < 0 || !d->root) { doc_inert_discard(d); return NULL; }
    d->resources_dirty = true;
    return d;
}
web_doc *web_parse(const char *html, size_t len, const char *url, const char *charset) {
    web_doc *d = calloc(1, sizeof *d);
    if (!d) return NULL;
    d->url = strdup(url && *url ? url : "about:blank");
    d->base = d->url ? strdup(d->url) : NULL;
    if (!d->base) { free(d->url); free(d); return NULL; }
    d->root = html_parse(d, html ? html : "", html ? len : 0, charset);
    d->need_style = true;
    if (d->root) scan(d, d->root, NULL);
    return d;
}

web_doc *web_live(const char *html, size_t len, const char *url, const char *charset,
                  const struct web_host *host) {
    return web_live_child(html,len,url,charset,host,NULL,NULL,NULL);
}
web_doc *web_live_response(const char *html,size_t len,const char *url,const char *charset,
                         const char *headers,const char *type,const struct web_host *host) {
    return web_live_child_response(html,len,url,charset,headers,type,host,NULL,NULL,NULL);
}
web_doc *web_live_child(const char *html, size_t len, const char *url, const char *charset,
                       const struct web_host *host, web_doc *parent,node_t *frame,web_doc *inherited_origin) {
    return web_live_child_response(html,len,url,charset,NULL,NULL,host,parent,frame,inherited_origin);
}
static bool mime_token_char(char c) {
    return c > ' ' && c < 0x7f && !strchr("\"(),/:;<=>?@[\\]{}", c);
}
/* MIME Sniffing "parse a MIME type" as far as the essence, then HTML "load a
   document"'s types: an HTML MIME type, an XML MIME type, and the text
   types (a JavaScript MIME type, a JSON MIME type, text/css, text/plain,
   text/vtt). No JSON or XML type is explicitly supported: none has dedicated
   processing rules or an external application here. The other text types
   are text documents too: this user agent hands no type to external software,
   and its processing rules for them are a text document's. */
enum web_document_kind web_document_kind(const char *type) {
    if (!type) return WEB_DOCUMENT_OTHER;
    const char *p = type;
    while (*p && strchr(" \t\r\n", *p)) p++;
    const char *slash = p;
    while (mime_token_char(*slash)) slash++;
    if (slash == p || *slash != '/') return WEB_DOCUMENT_OTHER;
    const char *subtype = slash + 1, *end = subtype;
    while (mime_token_char(*end)) end++;
    const char *after = end;
    while (*after && strchr(" \t\r\n", *after)) after++;
    if (end == subtype || (*after && *after != ';')) return WEB_DOCUMENT_OTHER;
    size_t type_length = (size_t)(slash - p), subtype_length = (size_t)(end - subtype);
    bool text = strn_ieq(p, "text", type_length);
    if (text && strn_ieq(subtype, "html", subtype_length)) return WEB_DOCUMENT_HTML;
    if ((text || strn_ieq(p, "application", type_length)) && strn_ieq(subtype, "xml", subtype_length)) return WEB_DOCUMENT_XML;
    if (subtype_length >= 4 && strn_ieq(end - 4, "+xml", 4)) return WEB_DOCUMENT_XML;
    size_t essence_length = (size_t)(end - p);
    if (text || web_javascript_mime_essence(p, essence_length) || web_json_mime_essence(p, essence_length))
        return WEB_DOCUMENT_TEXT;
    return WEB_DOCUMENT_OTHER;
}
/* The essence of a valid type, lowercase (malloc'd). */
static char *mime_essence(const char *type) {
    const char *p = type;
    while (*p && strchr(" \t\r\n", *p)) p++;
    size_t n = 0;
    while (mime_token_char(p[n]) || p[n] == '/') n++;
    char *essence = strndup(p, n);
    for (size_t i = 0; essence && i < n; i++) essence[i] = (char)lower((unsigned char)essence[i]);
    return essence;
}
const void *doc_parser_current(const web_doc *d) {
    if (!d) return NULL;
    return d->parser ? (const void *)d->parser : (const void *)d->xml_parser;
}
int doc_parser_resume(web_doc *d, node_t **script) {
    if (d && d->parser) return html_resume(d->parser, script);
    if (d && d->xml_parser) return xml_parser_resume(d->xml_parser, script);
    if (script) *script = NULL;
    return HTML_PARSE_FAILED;
}
bool doc_parser_changed(const web_doc *d) {
    return d && (d->parser ? html_import_changed(d->parser) : xml_parser_changed(d->xml_parser));
}
void doc_parser_stop(web_doc *d) {
    if (!d) return;
    html_finish(d->parser); d->parser = NULL;
    xml_parser_finish(d->xml_parser); d->xml_parser = NULL;
}
web_doc *web_live_child_response(const char *html,size_t len,const char *url,const char *charset,
                       const char *headers,const char *type,const struct web_host *host,web_doc *parent,node_t *frame,
                       web_doc *inherited_origin) {
    if (len == SIZE_MAX || (len && !html)) return NULL;
    web_doc *d = calloc(1, sizeof *d);
    if (!d) return NULL;
    d->live = true;
    static uint64_t input_serial;
    d->input_identity=__atomic_add_fetch(&input_serial,1,__ATOMIC_RELAXED);
    d->initial_render_deadline=uptime_ms()+3000;
    d->frame_parent=parent;d->frame_element=frame;
    /* HTML "activate history entry" (and "initialize the navigable" for a new navigable's
       first document) sets the initial visibility state to the traversable's system visibility
       state: the host's for a top-level document, the one its top document holds for a frame. */
    if(host)d->host=*host;
    if(host && host->window_state){d->window_state=host->window_state;d->window_opaque=host->opaque;}
    if(parent){web_doc *top=parent;while(top->frame_parent)top=top->frame_parent;d->visible=top->system_visible;}
    else d->visible=d->system_visible=!d->window_state || (d->window_state(d->window_opaque)&WEB_WINDOW_VISIBLE);
    /* A frame whose own policy disables scripting but keeps its origin
       (sandbox="allow-same-origin" without allow-scripts) still has a Window
       realm the same-origin embedder may use; its own scripts, handlers and
       script-initiated tasks stay refused by the SB_SCRIPTS checks. */
    bool realm_only=false;
    if(parent && frame){
        struct web_frame *f=web_frame_find(parent,frame);d->window_token=f?f->window_token:NULL;
        d->sandbox_flags=f?f->pending_sandbox_flags:parent->sandbox_flags;
        if(f && f->sandbox_initial && d->sandbox_flags)d->sandbox_flags|=SB_SCRIPTS;
        realm_only=f && (f->pending_sandbox_flags&SB_SCRIPTS) && !(d->sandbox_flags&(SB_ORIGIN|SB_UNSUPPORTED));
    }
    if(inherited_origin && parent){
        d->origin_owner=inherited_origin->origin_owner?inherited_origin->origin_owner:inherited_origin;
        d->inherited_url=strdup(web_effective_url(inherited_origin));
        if(!d->inherited_url){free(d);return NULL;}
    }
    d->dirty = d->need_style = d->resources_dirty = true;
    d->mem.limit = 0;
    d->cssmem.limit = 0;
    d->url = strdup(url && *url ? url : "about:blank");
    d->base = d->url ? strdup(web_effective_url(d)) : NULL;
    if (!d->base) { free(d->url);free((void *)d->inherited_url);free(d); return NULL; }
    if(!html_policy_init(d,headers,inherited_origin)){web_free(d);return NULL;}
    html_policy_isolation(d,headers,inherited_origin);
    if(!web_presentation_policy_init(d,headers,inherited_origin)){web_free(d);return NULL;}
    d->referrer_policy=webnet_referrer_policy_header(headers,inherited_origin?inherited_origin->referrer_policy:WEBNET_REFERRER_DEFAULT);
    /* The transport's metadata line (never a server field: it has no
       field-name) carries the referrer the navigation request sent. */
    const char *meta=headers?strstr(headers,"\r\nHTTP/Nocturne-Meta cors="):NULL;
    const char *line_end=meta?strstr(meta+2,"\r\n"):NULL;
    const char *sent=meta&&line_end?strstr(meta+2," referrer="):NULL;
    if(sent&&sent<line_end){
        sent+=10;
        d->referrer=strndup(sent,(size_t)(line_end-sent));
        if(!d->referrer){web_free(d);return NULL;}
    }
    char transport_label[64]={0};
    if(headers){
        struct http_resp response_headers={.headers_full=(char *)headers};char type[512]={0};
        http_header(&response_headers,"Content-Type",type,sizeof type);
        html_transport_label(type,transport_label,sizeof transport_label);
        if(transport_label[0])charset=transport_label;
    }
    d->html_srcdoc=parent && url && !strcmp(url,"about:srcdoc");
    enum web_document_kind kind = d->html_srcdoc ? WEB_DOCUMENT_HTML : web_document_kind(type);
    if (kind == WEB_DOCUMENT_XML) {
        /* HTML "load an XML document": a Document of type "xml" whose content
           type is the response's, in no-quirks mode, and its XML parser. */
        d->content_type = mime_essence(type);
        d->xml_parser = d->content_type ? xml_parser_begin(d, html ? html : "", html ? len : 0, charset) : NULL;
        if (!d->xml_parser) { web_free(d); return NULL; }
    } else if (kind == WEB_DOCUMENT_TEXT) {
        /* HTML "load a text document": a Document of type "html" whose content type is the
           response's, its parser cannot change the mode flag set and its mode no-quirks, and
           an HTML parser that reads every byte as text (html_begin_text). The bytes become
           characters by the type's own rules (html_decode_text): a JSON MIME type's differ. */
        d->text_document = true;
        d->content_type = mime_essence(type);
        d->parser = d->content_type ? html_begin_text(d, html ? html : "", html ? len : 0, charset,
            web_json_mime_essence(d->content_type, strlen(d->content_type)), !(d->sandbox_flags&SB_SCRIPTS)) : NULL;
        if (!d->parser) { web_free(d); return NULL; }
    } else {
        d->parser = d->html_srcdoc?html_begin_string(d,html?html:"",html?len:0,!(d->sandbox_flags&SB_SCRIPTS),true):
            html_begin(d, html ? html : "", html ? len : 0, charset, !(d->sandbox_flags&SB_SCRIPTS));
        if (!d->parser) { web_free(d); return NULL; }
    }
    /* The restricted initial blank of a scripted sandbox exposes only a native
       WindowProxy token. Do not create a QuickJS realm whose raw Function/eval
       could bypass disabled task dispatch when borrowed by the embedding
       document. A realm-only frame's natives refuse every task authority. */
    if(!(d->sandbox_flags&SB_SCRIPTS) || realm_only)web_js_start(d, host);
    return d;
}

static void free_pending(web_doc *d) {
    for (int i = 0; i < d->pending_css.n; i++) {
        struct pending *p = d->pending_css.v[i];
        free(p->url);
        free(p->media);
        free(p);
    }
    d->pending_css.n = 0;
}

static node_t *first_base(web_doc *d, node_t *root, uint64_t *visits) {
    for (node_t *c = root ? root->first : NULL; c;) {
        (*visits)++;
        if (!web_native_checkpoint(d)) return NULL;
        if (c->type == N_ELEM && !c->foreign && c->tag == T_base && node_attr(c, "href")) return c;
        /* Document metadata uses the ordinary tree, not shadow/template trees.
           Avoid a native recursion frame for each deeply nested element. */
        if (c->type == N_ELEM && (c->foreign || c->tag != T_template) && c->first) c = c->first;
        else {
            while (c && c != root && !c->next) c = c->parent;
            if (!c || c == root) break;
            c = c->next;
        }
    }
    return NULL;
}

/* Synchronous DOM/URL reads need the current tree, not a new CSS snapshot.
   Leave resources_dirty set: task completion or a layout read still scans
   resources, applies stylesheet changes, and initializes pending controls. */
void doc_sync_tree(web_doc *d) {
    if (!d) return;
    /* resources_dirty intentionally survives metadata reads. Its independent
       snapshot is invalidated by metadata-relevant native DOM hooks, not by
       every class/text/control mutation in the document. */
    if (d->metadata_valid) return;
    struct web_profile *profile = &(d->dom_family ? d->dom_family : d)->profile;
    uint64_t start = d->profile_enabled ? uptime_ms() : 0;
    profile->metadata_syncs++;
    node_t *html = NULL, *head = NULL, *body = NULL;
    for (node_t *n = d->root ? d->root->first : NULL; n; n = n->next) {
        profile->metadata_visits++;
        if (n->type == N_ELEM) { html = n; break; }
    }
    /* HTML "the html element" is the document element only when it is an HTML
       html element; head and body are its children (an XML document's root may be anything). */
    for (node_t *n = html && !html->foreign && html->tag == T_html ? html->first : NULL; n; n = n->next) {
        profile->metadata_visits++;
        if (!head && !n->foreign && n->tag == T_head) head = n;
        if (!body && !n->foreign && (n->tag == T_body || n->tag == T_frameset)) body = n;
    }
    node_t *base = first_base(d, d->root, &profile->metadata_visits);
    if (d->native_cancelled) return; /* never publish a cancelled partial tree */
    d->html=html;d->head=head;d->body=body;
    if (base) doc_frozen_base(d,base);
    else if (!doc_set_base(d,web_effective_url(d))) web_js_console(d,2,"Document base URL allocation failed");
    d->metadata_revision=d->dom_revision;d->metadata_valid=true;
    if (d->profile_enabled) profile->metadata_ms += uptime_ms() - start;
}

void doc_rescan(web_doc *d) {
    if (d && d->native_cancelled) return;
    if (d && d->inert && d->resources_dirty) {
        /* The same html/head/body and frozen base URL steps as a live
           document's metadata (an XML document's root may be any element). */
        d->resources_dirty = false; d->metadata_valid = false;
        doc_sync_tree(d);
        return; /* no authored CSS, image/meta fetch or live resource scan */
    }
    if (!d || !d->live || (!d->resources_dirty && !d->images_dirty)) return;
    if (!d->resources_dirty) {
        d->images_dirty = false;
        for (node_t *n = doc_image_node_next(d, NULL); n; n = doc_image_node_next(d, n))
            doc_image_sync(d, n);
        return;
    }
    uint64_t profile_start = d->profile_enabled ? uptime_ms() : 0;
    d->metadata_valid = false;
    d->profile.rescans++;
    d->resources_dirty = false;
    d->images_dirty = false;
    d->marquees.n=0;
    uint64_t base_hash=sheet_hash(d->base,strlen(d->base));
    css_sheets_rescan(&d->sty);
    ar_free(&d->cssmem);
    for(struct css_sheet_reuse *c=d->css_reuse;c;c=c->next)c->used=false;
    free_pending(d);
    d->next_sheet_order = 0;
    d->render_head_order = 0;
    d->base_seen = false;
    d->scan_title_seen = false;
    d->refresh_url = NULL;
    d->refresh_delay = 0;
    if (!doc_set_base(d,web_effective_url(d))) web_js_console(d,2,"Document base URL allocation failed");
    d->html = d->head = d->body = NULL;
    for (node_t *n = d->root ? d->root->first : NULL; n; n = n->next)
        if (n->type == N_ELEM) { d->html = n; break; }
    for (node_t *n = d->html ? d->html->first : NULL; n; n = n->next) {
        if (!d->head && n->tag == T_head) d->head = n;
        if (!d->body && (n->tag == T_body || n->tag == T_frameset)) d->body = n;
    }
    jmp_buf mem_trap, css_trap;
    jmp_buf *old_mem = d->mem.trap, *old_css = d->cssmem.trap;
    d->mem.trap = &mem_trap; d->cssmem.trap = &css_trap;
    int failed_arena;
    if (setjmp(mem_trap)) failed_arena = 1;
    else if (setjmp(css_trap)) failed_arena = 2;
    else {
        if (d->root) scan(d, d->root, NULL);
        for (node_t *n = doc_image_node_next(d, NULL); n; n = doc_image_node_next(d, n)) {
            bool in_template = false;
            for (node_t *p = n; p; p = p->parent ? p->parent : p->template_host) {
                if (p->tag == T_template || p->template_host) { in_template = true; break; }
            }
            if (n->tag == T_img && !in_template) doc_image_sync(d, n);
        }
        if (!d->scan_title_seen) d->title = NULL;
        failed_arena = 0;
    }
    if (failed_arena) {
        /* Distinguish the arenas: title/control/resource allocations can fail
           in the same transaction as authored CSS. Capture actual accounting
           before discarding the incomplete snapshot, even if JS is stopped. */
        char message[320];
        snprintf(message, sizeof message,
            "%s arena allocation failed during stylesheet/resource scan; CSS arena allocated=%zu limit=%zu; "
            "DOM arena allocated=%zu limit=%zu; authored sheets discarded=%d; CSS source bytes=%zu",
            failed_arena == 2 ? "CSS" : "DOM", d->cssmem.allocated, d->cssmem.limit,
            d->mem.allocated, d->mem.limit, d->sty.sheets.n, d->css_bytes);
        web_js_console(d, 2, message);
        /* Incomplete authored sheet snapshots must never outlive their arena. */
        css_styling_free(&d->sty);
        ar_free(&d->cssmem);
        sheet_reuse_sweep(d,true);
        free_pending(d);
    }
    /* Most rescans are for other resources and rebuild the same sheet list,
       whose cascade stands. A new base URL resolves inline styles anew. */
    bool sheets_same=!failed_arena && css_sheets_rescanned(d);
    if(!failed_arena)sheet_reuse_sweep(d,false);
    d->mem.trap = old_mem;
    d->cssmem.trap = old_css;
    d->dirty = true;
    if(!sheets_same)d->need_style = true;
    if(base_hash!=sheet_hash(d->base,strlen(d->base)))d->need_style=d->style_full_dirty=true;
    if(!failed_arena && !d->native_cancelled){d->metadata_revision=d->dom_revision;d->metadata_valid=true;}
    if (d->profile_enabled) d->profile.rescan_ms += uptime_ms() - profile_start;
}

static struct cached_css *css_resource(web_doc *d,const char *url) {
    struct cached_css *c = cached_css(d, url);
    if(c)return c;
    c=calloc(1,sizeof *c);
    if(!c)return NULL;
    c->url=strdup(url);c->base=strdup(url);
    if(!c->url || !c->base || !sheet_push(&d->css_cache,c)) {
        free(c->url);free(c->base);free(c);return NULL;
    }
    return c;
}
struct cached_css *doc_css_request_next(web_doc *d,size_t *cursor,node_t **element) {
    if(element)*element=NULL;
    if(!d || !cursor || !d->live)return NULL;
    while(*cursor<(size_t)d->pending_css.n) {
        struct pending *pending=d->pending_css.v[(*cursor)++];
        struct cached_css *c=css_resource(d,pending->url);
        if(!c){
            d->dirty=true;
            web_js_console(d,2,"Stylesheet request identity allocation failed in the OS allocator");
            return NULL;
        }
        if(!c->done&&!c->loading_id){if(element)*element=pending->owner;return c;}
    }
    return NULL;
}
void doc_css_loaded(web_doc *d, const char *url, const char *final_url, const char *css, size_t n) {
    if (!d || !url) return;
    struct cached_css *c=css_resource(d,url);
    if(!c)return;
    if (c->done) return;
    /* Claims are allocated at dispatch, before redirects are known. Relative
       imports use the final response URL, while request/dedup identity keeps
       the originally requested URL. Never publish bytes with a stale base. */
    const char *base=final_url&&*final_url?final_url:url;
    char *redirect_base=strcmp(c->base,base)?strdup(base):NULL;
    bool base_ok=!strcmp(c->base,base)||redirect_base;
    if(redirect_base){free(c->base);c->base=redirect_base;}
    if(!base_ok)web_js_console(d,2,"Stylesheet redirect base allocation failed in the OS allocator");
    c->done = true;
    if (base_ok && css && n < SIZE_MAX && d->css_bytes <= SIZE_MAX - n) {
        c->body = malloc(n + 1);
        if (c->body) {
            memcpy(c->body, css, n);
            c->body[n] = 0;
            c->n = n;
            d->css_bytes += n;
        } else web_js_console(d, 2, "Stylesheet storage allocation failed in the OS allocator");
    }
    d->resources_dirty = d->dirty = d->need_style = true;
}

void web_media_background(uint64_t now) {
    webidb_async_background();
    web_avmedia_background(now);
    web_worker_background();
}
int64_t web_media_background_deadline(uint64_t now) {
    int64_t media = web_avmedia_background_deadline(now), worker = web_worker_background_deadline(now);
    return media < 0 ? worker : worker < 0 ? media : MIN(media,worker);
}
/* --debug-js: which step of the turns since the last web_tick_report() left the document to lay
   out again (main thread only). A step is named when the flag was clear before it and set after. */
enum { TICK_RENDER, TICK_RESCAN, TICK_VISIBILITY, TICK_MEDIA, TICK_MOTION, TICK_MARQUEE, TICK_SCRIPT, TICK_FRAMES, TICK_STEPS };
/* Which source set each web_deadline(): what wakes an otherwise idle window. */
enum { DUE_NONE, DUE_BACKGROUND, DUE_RENDER, DUE_VISIBILITY, DUE_SCRIPT, DUE_MEDIA, DUE_MOTION, DUE_MARQUEE, DUE_FRAMES, DUE_SOURCES };
static struct { uint64_t turns, dirty[TICK_STEPS], style[TICK_STEPS], due[DUE_SOURCES]; } tick_profile;
static void tick_step(const web_doc *d, int step, bool dirty, bool style) {
    if (!d->profile_enabled) return;
    if (!dirty && d->dirty) tick_profile.dirty[step]++;
    if (!style && d->need_style) tick_profile.style[step]++;
}
int web_tick_report(char *out, size_t size) {
    if (!tick_profile.turns) return 0;
    static const char *const names[TICK_STEPS] = {"first render", "rescan", "visibility", "media", "CSS motion", "marquee", "script", "frames"};
    int n = snprintf(out, size, "Document tick profile: turns %lu; dirtied (layout and paint)/restyle by",
                     (unsigned long)tick_profile.turns);
    for (int i = 0; i < TICK_STEPS && n >= 0 && (size_t)n < size; i++)
        n += snprintf(out + n, size - (size_t)n, "%s %s %lu/%lu", i ? "," : "", names[i],
                      (unsigned long)tick_profile.dirty[i], (unsigned long)tick_profile.style[i]);
    static const char *const due_names[DUE_SOURCES] = {"none", "background", "first render", "visibility", "script", "media", "CSS motion", "marquee", "frames"};
    for (int i = 0; i < DUE_SOURCES && n >= 0 && (size_t)n < size; i++)
        n += snprintf(out + n, size - (size_t)n, "%s %s %lu", i ? "," : "; deadlines from", due_names[i],
                      (unsigned long)tick_profile.due[i]);
    memset(&tick_profile, 0, sizeof tick_profile);
    return n;
}
/* --debug-js: what the document and its frames hold now, by owner. A cached sheet or a
   runtime that frames share is counted once, by the document that owns it. */
struct memory_tally { size_t dom, styles, boxes, layout, sheets, css_source, css_ast, images, canvas, js, largest_js; int docs, image_count; web_doc *largest; };
static void memory_tally_doc(web_doc *d, struct memory_tally *t) {
    t->docs++;
    t->dom += d->mem.allocated; t->styles += d->smem.allocated; t->boxes += d->bmem.allocated;
    t->layout += d->lmem.allocated; t->sheets += d->cssmem.allocated;
    for (int i = 0; i < d->css_cache.n; i++) {
        struct cached_css *c = d->css_cache.v[i];
        t->css_source += c->n; t->css_ast += c->ast_mem.allocated;
    }
    for (int i = 0; i < d->images.n; i++) {
        const struct web_image *im = d->images.v[i];
        const image_t *shown[3] = {im->img, im->scaled[0], im->scaled[1]};
        for (int k = 0; k < 3; k++) if (shown[k]) t->images += (size_t)shown[k]->w * (size_t)shown[k]->h * 4;
        t->images += im->svg_n; t->image_count++;
    }
    t->canvas += d->canvas_bytes;
    size_t js = web_js_heap_bytes(d);
    t->js += js;
    if (js > t->largest_js) { t->largest_js = js; t->largest = d; }
}
int web_memory_report(web_doc *d, char *out, size_t size) {
    if (!d || !d->live) return 0;
    struct memory_tally t = {0};
    memory_tally_doc(d, &t);
    for (struct web_frame *f = d->frames; f; f = web_frame_walk_next(d, f, true))
        if (f->document && !f->detached) memory_tally_doc(f->document, &t);
    int n = snprintf(out, size, "Document memory (KiB, %d documents): DOM and sheets %zu, computed styles %zu, boxes %zu, layout %zu, "
                     "authored sheets %zu, fetched CSS %zu source/%zu parsed, images %zu (%d), canvases %zu, JavaScript %zu",
                     t.docs, t.dom / 1024, t.styles / 1024, t.boxes / 1024, t.layout / 1024, t.sheets / 1024,
                     t.css_source / 1024, t.css_ast / 1024, t.images / 1024, t.image_count, t.canvas / 1024, t.js / 1024);
    /* The largest runtime's own census, on the next line. It walks the whole heap, as long
       as a busy frame on a large page, so the profile it is part of would mostly measure it:
       only when that heap grew or shrank by an eighth, or every 30 s. */
    static size_t census_bytes; static uint64_t census_at;
    uint64_t now = uptime_ms();
    bool census = t.largest_js > census_bytes + census_bytes / 8 || t.largest_js + t.largest_js / 8 < census_bytes ||
                  now - census_at >= 30000;
    if (t.largest && census && n >= 0 && (size_t)n + 1 < size) {
        census_bytes = t.largest_js; census_at = now;
        out[n++] = '\n';
        int census = web_js_heap_census(t.largest, out + n, size - (size_t)n);
        if (census > 0) n += census; else out[--n] = 0;
    }
    return n;
}
/* HTML 6.2: queue one "update the visibility state" task on d, to newState. The queue holds
   only alternating states (the system state changes before each), so a count and the newest
   state are all of it; at the count's limit the oldest is dropped, which keeps them alternating. */
static void visibility_queue(web_doc *d, bool visible) {
    if (d->visibility_updates < UINT_MAX) d->visibility_updates++;
    d->visibility_update_last = visible;
}
void web_visibility_event(web_doc *d) {
    while (d && d->frame_parent) d = d->frame_parent;
    if (!d || !d->live) return;
    bool visible = !d->window_state || (d->window_state(d->window_opaque) & WEB_WINDOW_VISIBLE);
    if (visible == d->system_visible) return;
    /* The system visibility state of the traversable changed: a task for each inclusive
       descendant navigable's active document, on the user interaction task source. */
    d->system_visible = visible;
    visibility_queue(d, visible);
    for (struct web_frame *f = d->frames; f; f = web_frame_walk_next(d, f, true))
        if (f->document && !f->detached && f->document->live) visibility_queue(f->document, visible);
}
/* One queued task, oldest first: "update the visibility state of document to newState". The
   rest of the steps (the VisibilityStateEntry, visibilitychange) belong to the realm. A hidden
   document's work stops by itself: each clock (CSS motion, marquee, rendering, timers, idle
   periods) reads d->visible when it next asks to run. */
static void visibility_step(web_doc *d) {
    if (!d->visibility_updates || web_js_running(d)) return;
    bool visible = d->visibility_update_last ^ ((d->visibility_updates - 1) & 1u);
    d->visibility_updates--;
    if (visible == d->visible) return;
    d->visible = visible;
    web_js_visibility_changed(d);
}
#define TICK_STEP(d, step, call) do { bool dirty_ = (d)->dirty, style_ = (d)->need_style; call; tick_step(d, step, dirty_, style_); } while (0)
void web_tick(web_doc *d, uint64_t now) {
    web_media_background(now);
    if (!d || !d->live) return;
    if (d->profile_enabled) tick_profile.turns++;
    if(!d->initial_render_done && d->initial_render_deadline && now >= d->initial_render_deadline) {
        /* A hidden frame may never paint. Its fallback is a one-shot wake,
           not a perpetual full-page damage source on every subsequent tick. */
        d->initial_render_deadline=0;
        TICK_STEP(d, TICK_RENDER, doc_paint_full(d));
    }
    /* A native-only cancellation preserves author queues, not partial boxes.
       Rebuild outside every author/media/animation task before any callback
       can observe geometry. Active parent/author/parser transactions refuse
       recovery in web_js_native_recover; their queues stay parked here. */
    if(d->native_recovery_pending){
        web_layout(d,MAX(1,d->width),MAX(1,d->height));
        if(d->native_recovery_pending || d->native_cancelled || !d->layout_valid)return;
    }
    TICK_STEP(d, TICK_RESCAN, doc_rescan(d));
    if(d->native_cancelled)return;
    TICK_STEP(d, TICK_VISIBILITY, visibility_step(d));
    if(d->native_cancelled)return;
    TICK_STEP(d, TICK_MEDIA, web_avmedia_tick(d, now));
    if(d->native_cancelled)return;
    TICK_STEP(d, TICK_MOTION, web_animations_tick(d,now));
    if(d->native_cancelled)return;
    TICK_STEP(d, TICK_MARQUEE, web_marquee_tick(d,now));
    if(d->native_cancelled)return;
    TICK_STEP(d, TICK_SCRIPT, web_js_tick(d, now));
    if(d->native_cancelled)return;
    TICK_STEP(d, TICK_RESCAN, doc_rescan(d));
    if(d->native_cancelled)return;
    /* The child can complete while author JS runs. Refill before GUI paint,
       not one complete browser turn later. Both calls retain their native
       2 ms / 8 packet bounds; there are exactly two opportunities per tick. */
    TICK_STEP(d, TICK_MEDIA, web_avmedia_tick(d, uptime_ms()));
    if(d->native_cancelled)return;
    TICK_STEP(d, TICK_FRAMES, web_js_frames_sync(d));
    if(d->native_cancelled)return;
    TICK_STEP(d, TICK_FRAMES, web_frames_tick(d,now));
}
/* The window process's one queue. The first call also watches the browser's preferences in it
   (webprefs), once for the process, before any realm reads the languages; web_wait_events hands
   the watch's reports to webprefs, which reads the preferences again. */
n_handle web_wait_queue(void) {
    static bool preferences_watched;
    n_handle queue = web_atomics_queue();
    if (queue >= 0 && !preferences_watched) {
        preferences_watched = true;
        (void)webprefs_watch(queue); /* without it, the first use reads the preferences once */
    }
    return queue;
}
void web_wait_events(const struct n_event *events, size_t count) {
    web_atomics_events(events, count);
    webprefs_events(events, count);
}
int web_wait_report(char *out, size_t size) { return web_atomics_report(out, size, uptime_ms()); }
int64_t web_deadline(web_doc *d) {
    uint64_t now = uptime_ms();
    int64_t background = web_media_background_deadline(now);
    if (!d || !d->live) return background;
    const int64_t sources[DUE_SOURCES] = {-1, background,
        !d->initial_render_done && now < d->initial_render_deadline ? (int64_t)d->initial_render_deadline : -1,
        d->visibility_updates ? (int64_t)now : -1,
        web_js_deadline(d), web_avmedia_deadline(d, now), web_animations_deadline(d), web_marquee_deadline(d),
        web_frames_deadline(d)};
    int64_t deadline = -1; int from = DUE_NONE;
    for (int i = DUE_BACKGROUND; i < DUE_SOURCES; i++)
        if (sources[i] >= 0 && (deadline < 0 || sources[i] < deadline)) { deadline = sources[i]; from = i; }
    if (d->profile_enabled) tick_profile.due[from]++;
    return deadline;
}
void web_resource_loaded(web_doc *d, uint64_t id, const struct web_response *r) {
    if (d && d->live) {web_js_loaded(d, id, r);web_frames_loaded(d,id,r);}
}
void web_socket_event(web_doc *d, uint64_t id, const struct web_socket_event *e) {
    if (d && d->live && e) {web_js_socket_event(d, id, e);web_frames_socket_event(d,id,e);}
}
bool web_response_set_url(struct web_response *r, const char *url) {
    if (!r || !url) return false;
    size_t length = strlen(url);
    if (length == SIZE_MAX) return false;
    char *owned = NULL;
    if (length >= sizeof r->url) {
        owned = malloc(length + 1);
        if (!owned) return false;
        memcpy(owned, url, length + 1);
    } else memmove(r->url, url, length + 1);
    free(r->url_full); r->url_full = owned;
    if (owned) r->url[0] = 0;
    return true;
}
void web_response_free(struct web_response *r) {
    if (!r) return;
    free(r->body); free(r->headers_full); free(r->url_full);
    r->body = r->headers_full = r->url_full = NULL; r->body_len = 0;
    http_fetch_timing_free(&r->timing);
}
bool web_dirty(web_doc *d) {
    if (!d) return false;
    bool dirty = d->dirty || d->restyle_dirty;
    d->dirty = d->restyle_dirty = false;
    return dirty;
}
bool web_restyle_only(const web_doc *d) { return d && d->restyle_dirty && !d->dirty; }
bool web_paint_dirty(web_doc *d) {
    if (!d) return false;
    bool dirty = d->paint_dirty;
    d->paint_dirty = false;
    return dirty;
}
bool web_script_running(web_doc *d) { return d && d->live && web_js_running(d); }
static void focus_keyboard_input(web_doc *d, const struct web_event *e);
bool web_dispatch(web_doc *d, web_node *target, const struct web_event *e) {
    if(!d||!d->live)return true;
    if(e&&e->type&&!e->synthetic) {
        /* the latest user input, for :focus-visible */
        web_doc *input=target&&target->owner?target->owner:d;
        if(!strcmp(e->type,"keydown"))focus_keyboard_input(input,e);
        else if(!strcmp(e->type,"mousedown")||!strcmp(e->type,"pointerdown"))input->focus_modality=WEB_MODALITY_POINTER;
    }
    /* HTML image button: "if the user activated the control while explicitly selecting a
       coordinate", the point in the displayed image, captured before click listeners can
       replace layout or reparent it. The click's activation behavior (js_activation_native.h)
       makes it the selected coordinate, only when the click is not canceled; keyboard and
       script activation keep the initially-zero one. Padding/border clicks may yield negative
       CSS coordinates, not 0-clamped bitmap pixels. No extra layout flush or mutable author
       getter is used. */
    web_doc *owner=target?target->owner:NULL;bool image_point=false;int selected_x=0,selected_y=0;
    if(e&&e->type&&!strcmp(e->type,"click")&&!e->synthetic&&!e->keyboard_activation&&
       target&&target->type==N_ELEM&&!target->foreign&&target->tag==T_input&&
       web_input_type(target)==WEB_INPUT_IMAGE&&owner&&owner->live&&target->box&&
       target->image>=0&&target->image<owner->images.n){
        struct web_image *image=owner->images.v[target->image];int x,y,w,h;
        if(image&&!image->failed&&(image->img||image->svg)&&web_node_rect(d,target,&x,&y,&w,&h)){
            box_t *box=target->box;
            double px=(double)e->x+d->view_x-x-box->p[3]-box->b[3];
            double py=(double)e->y+d->view_y-y-box->p[0]-box->b[0];
            double left=ceil(-(double)box->p[3]-box->b[3]),right=floor((double)box->w+box->p[1]+box->b[1]);
            double top=ceil(-(double)box->p[0]-box->b[0]),bottom=floor((double)box->h+box->p[2]+box->b[2]);
            if(isfinite(px)&&isfinite(py)&&left<=right&&top<=bottom){
                px=MAX(left,MIN(floor(px),right));py=MAX(top,MIN(floor(py),bottom));
                if(px>=INT_MIN&&px<=INT_MAX&&py>=INT_MIN&&py<=INT_MAX){selected_x=(int)px;selected_y=(int)py;image_point=true;}
            }
        }
    }
    bool trace_wheel=d->profile_enabled && e && e->type && !strcmp(e->type,"wheel");
    unsigned target_tag=trace_wheel&&target?target->tag:0;
    uint64_t window_blocking=trace_wheel?(target&&target->owner?target->owner:d)->wheel_window_blocking:0;
    node_t *saved_image=NULL;int saved_x=0,saved_y=0;
    if(image_point){
        saved_image=owner->image_activation;saved_x=owner->image_activation_x;saved_y=owner->image_activation_y;
        owner->image_activation=target;owner->image_activation_x=selected_x;owner->image_activation_y=selected_y;
    }
    bool allowed=web_js_dispatch(d,target,e);
    if(image_point){owner->image_activation=saved_image;owner->image_activation_x=saved_x;owner->image_activation_y=saved_y;}
    if(trace_wheel) {
        char message[256];snprintf(message,sizeof message,
            "Native wheel default: allowed %d, cancelable %d, delta %.0f, tag %u, window-blocking %llu, coherent %d/ready %d, viewport %d/%d, view %d, style %d/boxes %d",
            allowed,e->cancelable,(double)e->delta_y,target_tag,(unsigned long long)window_blocking,
            scroll_input_coherent(d),web_native_input_ready(d,false),d->doc_h,d->height,d->view_y,d->need_style,d->need_boxes);
        web_js_console(d,1,message);
    }
    return allowed;
}

static void free_values(node_t *n) {
    element_scroll_input_unqueue(n);
    free(n->container_names);n->container_names=NULL;
    css_node_style_free(n);
    cssom_style_free(n);
    web_paint_debug_node_free(n);
    web_animations_node_free(n);
    web_canvas_free(n);
    web_input_files_release(n);
    web_face_release(n);
    if (n->shadow_root) free_values(n->shadow_root);
    for (node_t *c = n->first; c; c = c->next) {
        if (c->type != N_ELEM) continue;
        free(c->value);
        c->value = NULL;
        free(c->input_edit);
        c->input_edit = NULL;
        free_values(c);
    }
}

struct web_font_resource *doc_font_resource(web_doc *d,const char *url) {
    if(!d || !url || !*url)return NULL;
    for(struct web_font_resource *f=d->fonts;f;f=f->next)if(!strcmp(f->url,url))return f;
    struct web_font_resource *f=calloc(1,sizeof *f);if(!f)return NULL;
    f->url=strdup(url);if(!f->url){free(f);return NULL;}
    f->next=d->fonts;d->fonts=f;return f;
}
void doc_font_loaded(web_doc *d,struct web_font_resource *f,const void *bytes,size_t length) {
    if(!d || !f || f->done)return;
    f->face=bytes && length?font_open_memory(bytes,length):NULL;
    f->loading=f->demanded=false;f->done=true;f->failed=!f->face;
    d->dirty=true;
    __atomic_add_fetch(&d->font_generation,1,__ATOMIC_RELAXED); /* FontFace statuses */
    /* A decoded face changes native glyph metrics and fallback coverage.
       Mark committed formatting stale before selecting it at the next layout
       boundary; otherwise a geometry getter can reuse the old font's cache.
       Failed sources merely advance selection and preserve valid metrics. */
    if(f->face){
        d->layout_valid=false;
        layout_invalidate_fonts(d->root_box);
    }
    d->font_selection_dirty=true;
}
bool doc_fonts_unrequested(web_doc *d) {
    for(struct web_font_resource *f=d?d->fonts:NULL;f;f=f->next)
        if((f->wanted || f->demanded) && !f->loading && !f->done)return true;
    return false;
}
/* CSS Font Loading's "pending stylesheet requests": every requested sheet,
   whatever its media or scope, until it arrives or fails. */
bool doc_css_loading(web_doc *d) {
    if(!d || !d->live || d->inert)return false;
    for(int i=0;i<d->pending_css.n;i++){
        const struct cached_css *c=cached_css(d,((const struct pending *)d->pending_css.v[i])->url);
        if(!c || !c->done)return true;
    }
    return false;
}

/* A document family (a document and its dom_docs) shares nodes: a node is allocation-owned by one
   member and may be DOM-owned by another after adoption. web_free releases the whole family in
   three steps, so that no step reads a member that an earlier step freed: each member's own state
   (scripts, styles, boxes, animations, parsers), then every member's allocation-owned nodes, then
   every member's storage. */
static void family_release(web_doc *member);
static void doc_release(web_doc *d) {
    while (d->scroll_input_first) {
        node_t *n = d->scroll_input_first;
        d->scroll_input_first = n->scroll_input_next;
        n->scroll_input_owner = NULL; n->scroll_input_next = NULL;
    }
    d->scroll_input_last = NULL;
    web_presentation_free(d);
    doc_active_cancel(d);
    /* Adopted DOM nodes may be allocation-owned by a helper document. Retire
       all published box/scope borrows before freeing that family of owners. */
    boxes_discard(d);
    web_paint_free(d);
    layout_cache_free(d);
    css_styles_release(d);
    css_styling_free(&d->sty);
    css_registrations_free(&d->sty);
    web_dialog_free(d);
    web_avmedia_free(d);
    /* The frames' documents are released here, before this realm, and keep their nodes and
       allocations until web_free retires and frees this document's: a node can be adopted
       between a document and its frames' documents in either direction. */
    d->frame_docs = web_frames_detach(d);
    for (web_doc *c = d->frame_docs; c; c = c->frame_free_next) { doc_release(c); family_release(c->dom_docs); }
    web_js_free(d);
    doc_named_free(d);
    html_policy_free(d);
    web_animations_doc_free(d);
    pv_free(&d->marquees);
    /* FontFace records outlive neither their document font sources nor the
       resources they name: release them before either goes. */
    css_font_set_free(d);
    while(d->fonts) {
        struct web_font_resource *f=d->fonts;d->fonts=f->next;
        font_close(f->face);free(f->url);free(f);
    }
    while(d->font_data) {
        struct web_font_resource *f=d->font_data;d->font_data=f->next;
        font_close(f->face);free(f);
    }
    web_form_validation_free(d);
    /* Parser subscriptions can borrow allocation-owned nodes in helper docs. */
    doc_parser_stop(d);
}
static void doc_nodes_retire(web_doc *d) {
    if (d->owned_nodes) {
        for (node_t *n = d->owned_nodes; n; n = n->owned_next) {
            /* Allocation ownership differs from DOM ownership after adoption.
               A sibling document may still queue this allocation-owned node. */
            element_scroll_input_unqueue(n);
            html_bridge_native_retire(n);
            free(n->container_names);n->container_names=NULL;
            css_node_style_free(n);
            cssom_style_free(n);
            web_paint_debug_node_free(n);
            web_animations_node_free(n);
            web_canvas_free(n);
            web_input_files_release(n);
            web_face_release(n);
            free(n->value);
            free(n->input_edit);
        }
    } else if (d->root) free_values(d->root);
}
static void doc_storage_free(web_doc *d) {
    css_style_doc_free(d);
    free_pending(d);
    pv_free(&d->pending_css);
    for (int i = 0; i < d->css_cache.n; i++) {
        struct cached_css *c = d->css_cache.v[i];
        ar_free(&c->ast_mem);
        free(c->url);
        free(c->base);
        free(c->body);
        free(c);
    }
    pv_free(&d->css_cache);
    for (int i = 0; i < d->images.n; i++) {
        struct web_image *im = d->images.v[i];
        if (im->img) image_free(im->img);
        for (size_t k = 0; k < sizeof im->scaled / sizeof *im->scaled; k++) image_free(im->scaled[k]);
        free(im->svg);
        free(im->url);
        free(im);
    }
    pv_free(&d->images);
    for (int i = 0; i < d->svgs.n; i++) {
        struct svg_cache *s = d->svgs.v[i];
        if (s->img) image_free(s->img);
        free(s->src);
        free(s);
    }
    pv_free(&d->svgs);
    pv_free(&d->abs_boxes);
    ar_free(&d->mem);
    ar_free(&d->bmem);
    ar_free(&d->smem);
    ar_free(&d->lmem);
    ar_free(&d->cssmem);
    sheet_reuse_sweep(d,true);
    free(d->url);
    free(d->base);
    free(d->resolved_link);
    free(d->find_text);
    free((void *)d->inherited_url);
    free(d->referrer);
    free(d->content_type);
    free(d);
}
static void family_release(web_doc *member) {
    for (; member; member = member->dom_next) { doc_release(member); family_release(member->dom_docs); }
}
static void frames_retire(web_doc *c);
static void family_retire(web_doc *member) {
    for (; member; member = member->dom_next) { family_retire(member->dom_docs); doc_nodes_retire(member); frames_retire(member->frame_docs); }
}
/* A frames' documents list is flat (web_frames_detach): only a family member's own frames add
   another level. */
static void frames_retire(web_doc *c) {
    for (; c; c = c->frame_free_next) { family_retire(c->dom_docs); doc_nodes_retire(c); frames_retire(c->frame_docs); }
}
static void family_free(web_doc *d);
static void frames_free(web_doc *c) {
    while (c) {
        web_doc *next = c->frame_free_next; c->frame_free_next = NULL;
        frames_free(c->frame_docs); c->frame_docs = NULL;
        family_free(c);
        doc_storage_free(c);
        c = next;
    }
}
static void family_free(web_doc *d) {
    while (d->dom_docs) {
        web_doc *child = d->dom_docs;
        d->dom_docs = child->dom_next; child->dom_next = NULL;
        family_free(child);
        frames_free(child->frame_docs); child->frame_docs = NULL;
        doc_storage_free(child);
    }
}
/* Three stages over the document, its family (dom_docs) and its frames' documents (theirs too):
   every state, then every node, then every allocation. A node's allocation owner and DOM owner
   can be any two of them after adoption, so no stage may free what another's nodes still name. */
void web_free(web_doc *d) {
    if (!d) return;
    doc_release(d);
    family_release(d->dom_docs);
    family_retire(d->dom_docs);
    doc_nodes_retire(d);
    frames_retire(d->frame_docs);
    frames_free(d->frame_docs); d->frame_docs = NULL;
    family_free(d);
    doc_storage_free(d);
}

const char *web_title(web_doc *d) { return d->title ? d->title : ""; }
const char *web_url(web_doc *d) { return d->url; }
#include "frame_address.h"
bool web_set_url(web_doc *d, const char *url) {
    if (!d || !url) return false;
    char *copy = strdup(url);
    if (!copy) return false;
    free(d->url); d->url = copy;
    d->metadata_valid=false;
    return d->base_seen || doc_set_base(d, url);
}
const char *web_control_value(web_node *control) { return control ? web_input_edit_text(control) : NULL; }

const char *web_refresh_url(web_doc *d, int *delay_s) {
    if (delay_s) *delay_s = d->refresh_delay;
    return d->refresh_url;
}

const char *web_pending_stylesheet(web_doc *d) {
    return d->pending_css.n ? ((struct pending *)d->pending_css.v[0])->url : NULL;
}

void web_stylesheet_loaded(web_doc *d, const char *css, size_t n) {
    if (!d->pending_css.n) return;
    struct pending *p = d->pending_css.v[0];
    memmove(d->pending_css.v, d->pending_css.v + 1, sizeof(void *) * (size_t)(d->pending_css.n - 1));
    d->pending_css.n--;
    if (css) {
        /* skip a UTF-8 byte order mark */
        if (n >= 3 && !memcmp(css, "\xEF\xBB\xBF", 3)) css += 3, n -= 3;
        add_sheet(d, css, n, p->url, p->order, p->media, p->scope, false);
    }
    free(p->url);
    free(p->media);
    free(p);
}

int web_image_count(web_doc *d) { return d->images.n; }

const char *web_image_url(web_doc *d, int i) {
    return i >= 0 && i < d->images.n ? ((struct web_image *)d->images.v[i])->url : NULL;
}

bool web_image_wanted(web_doc *d, int i) {
    if (i < 0 || i >= d->images.n) return false;
    struct web_image *im = d->images.v[i];
    return !im->done && !im->failed && !im->loading_id;
}

static void image_decode_service(void *context) { web_native_checkpoint(context); }
void web_image_loaded(web_doc *d, int i, const void *data, size_t n, bool origin_clean) {
    if (i < 0 || i >= d->images.n) return;
    struct web_image *im = d->images.v[i];
    if (im->done) return;
    im->done = true;
    im->origin_clean = origin_clean;
    if (data && n) {
        if (image_is_svg(data, n)) {
            im->img = image_decode_svg(data, n, 0, 0);
            if (im->img && (im->svg = malloc(n))) {
                memcpy(im->svg, data, n);
                im->svg_n = n;
            }
        }
        else im->img = image_decode_serviced(data,n,image_decode_service,d);
        if (d->native_cancelled) { image_free(im->img); im->img=NULL; im->failed=true; return; }
    }
    if (!im->img) im->failed = true;
    bool intrinsic_changed = false;
    for (node_t *node = doc_image_node_next(d, NULL); node; node = doc_image_node_next(d, node)) {
        if (node->tag == T_img && node->image_initialized && node->image_request == i) {
            bool in_template = false;
            for (node_t *p = node; p; p = p->parent ? p->parent : p->template_host) {
                if (p->tag == T_template || p->template_host) { in_template = true; break; }
            }
            if (!in_template) node->image = i;
        }
        if (node->image != i || doc_node_root(node, true) != d->root) continue;
        if (d->profile_enabled && d->replaced_trace_reported < 2 && im->img && node->tag == T_img) d->replaced_trace_pending = true;
        box_t *box = node->box;
        if (!box || box->node != node || box->kind != B_ATOMIC || box->atomic != AT_IMG) continue;
        const style_t *style = box->st;
        /* Image availability/natural size never changes the author's CSS.
           Both definite absolute axes make replaced sizing independent of
           natural dimensions, including intrinsic width contributions. Keep
           percentage/auto/ratio-dependent cases conservative; failure can
           also introduce the alt-text intrinsic size. Background/mask-only,
           hidden and detached image completions need pixels/events, not a
           whole-document geometry pass. */
        if (!style || style->width.kind != LK_LEN || style->width.pct != 0 ||
            style->height.kind != LK_LEN || style->height.pct != 0)
            { intrinsic_changed = true; layout_invalidate(box); }
    }
    doc_paint_full(d);
    if (intrinsic_changed) { d->layout_valid = false; d->dirty = true; }
}

/* CSS background images are known once styles are: add them to the document's images */
static void bg_images(web_doc *d, style_t *st) {
    if (st && st->bg_image && !st->bg_img) {
        int i = add_image(d, st->bg_image, true);
        st->bg_img = i >= 0 ? i + 1 : 0;
    }
    /* the mask layers' images (css-masking-1 §7.1): CORS requests; a url() naming an element of the
       document itself is no fetch */
    const struct css_mask *mask = st ? css_style_mask(st) : NULL;
    for (uint32_t layer = 0; mask && layer < mask->image.n; layer++) {
        const struct css_image *image = mask->image.v[layer];
        if (image && image->kind == CSS_IMAGE_URL && !image->img && !doc_url_local(d, image->url))
            doc_css_image(d, image, WEB_IMAGE_ANONYMOUS);
    }
    /* border-image-source and mask-border-source (CORS, as mask images: css-masking-1 §8.1): the same
       images, fetched and decoded once */
    if (st && st->border_image && st->border_image->source.url && !st->border_image_img) {
        int i = add_image(d, st->border_image->source.url, true);
        st->border_image_img = i >= 0 ? i + 1 : 0;
    }
    if (st && st->mask_border && st->mask_border->source.url && !st->mask_border_img) {
        int i = add_image_request(d, st->mask_border->source.url, true, false, -1, false, WEB_IMAGE_ANONYMOUS,
                                  WEBNET_REFERRER_DEFAULT);
        st->mask_border_img = i >= 0 ? i + 1 : 0;
    }
}

/* css-images-4 §2.3.1: the option of an image-set() a 1 dppx screen shows: of those whose type() it
   decodes, the lowest resolution not under 1x, else the highest */
static const struct css_cursor_choice *cursor_choice(const struct css_cursor_image *im) {
    const struct css_cursor_choice *best = NULL;
    float best_dppx = 0;
    for (uint32_t i = 0; i < im->count; i++) {
        const struct css_cursor_choice *c = &im->choices[i];
        if (c->type && !image_type_supported(c->type)) continue;
        float r = c->dppx > 0 ? c->dppx : 1;
        if (!best || (r >= 1 ? best_dppx < 1 || r < best_dppx : best_dppx < 1 && r > best_dppx)) { best = c; best_dppx = r; }
    }
    return best;
}
int doc_cursor_image(web_doc *d, const struct css_cursor_image *image, float *dppx) {
    const struct css_cursor_choice *c = cursor_choice(image);
    if (!c) return -1;
    *dppx = c->dppx > 0 ? c->dppx : 1;
    return add_image(d, c->url, true);
}
/* the images an element's cursor names are fetched as its style is known (the ones it inherits are
   its parent's) */
static void cursor_images(web_doc *d, const style_t *st, const style_t *parent) {
    const struct css_cursor *cursor = &css_style_ui(st)->cursor;
    if (!cursor->count || (parent && css_style_ui(parent)->cursor.images == cursor->images)) return;
    float dppx;
    for (uint32_t i = 0; i < cursor->count; i++) (void)doc_cursor_image(d, &cursor->images[i], &dppx);
}

static void register_bg(web_doc *d, node_t *n) {
    for (node_t *c = n->first; c; c = c->next) {
        if (c->type != N_ELEM || !c->style) continue;
        bg_images(d, c->style);
        cursor_images(d, c->style, n->type == N_ELEM ? n->style : NULL);
        if (c->style->before_generated) bg_images(d, c->style->before);
        if (c->style->after_generated) bg_images(d, c->style->after);
        register_bg(d, c);
        if (c->shadow_root) register_bg(d, c->shadow_root);
    }
}

static void invalidate_layout_tree(box_t *b) {
    if (!b) return;
    b->intrinsic_done = false;
    b->layout_dirty = true; b->layout_cache_valid = false;
    for (box_t *c = b->first; c; c = c->next) invalidate_layout_tree(c);
}

void doc_viewport_update(web_doc *d,int width,int height) {
    width=MAX(1,width);height=MAX(1,height);
    if(!d || (d->width==width&&d->height==height))return;
    /* The viewport exists even before the next raster is painted. Preserve
       the ordinary resize invalidation and source-selection path for both
       top-level windows and embedded documents, without invoking author JS. */
    invalidate_layout_tree(d->root_box);
    d->width=width;d->height=height;d->layout_valid=false;
    d->need_style=d->style_full_dirty=d->dirty=true;
    for(node_t *image=doc_image_node_next(d,NULL);image;image=doc_image_node_next(d,image))doc_image_sync(d,image);
}

static bool committed_geometry_current(const web_doc *d,int width,int height,uint64_t font_generation) {
    const box_t *root=d?d->root_box:NULL;
    return root && root->layout_post_valid && !root->layout_dirty &&
        root->cached_font_generation==font_generation &&
        root->layout_post_w==root->w && root->layout_post_h==root->h &&
        root->layout_post_content_dy==root->content_dy &&
        d->width==width && d->height==height && !d->native_recovery_pending;
}
int web_layout(web_doc *d, int width, int height) {
    unsigned container_pass=0;
    if (d->native_cancelled) {
        if(!web_js_native_recover(d)){
            if(d->native_recovery_pending)d->dirty=true;
            return d->doc_h;
        }
        d->layout_valid=false;d->need_style=d->need_boxes=d->style_full_dirty=true;
        d->resources_dirty=true;
    }
    d->native_layout_depth++;
    doc_shadow_flush(d);
    if (width < 1) width = 1;
    if (height < 1) height = 1;
    web_dialog_sync(d);
    if (d->resources_dirty || d->images_dirty) doc_rescan(d);
    if (d->native_cancelled) goto cancelled;
    if(d->font_selection_dirty && !d->need_style) {
        d->font_selection_dirty=false;
        css_refresh_font_selection(d);
        if(d->native_cancelled)goto cancelled;
    }
    /* Geometry reads may flush layout repeatedly within one script. Reuse the
       result until DOM/style, viewport, or image intrinsic dimensions change. */
    if (d->layout_valid && !d->need_style && !d->need_boxes && d->root_box &&
        d->width == width && d->height == height) { d->native_layout_depth--; return d->doc_h; }
    /* Layout changes the next paint, not the raster already on screen.
       Native video snapshots own committed pixels/geometry independently;
       actual paint, canvas/scroll changes and media retirement invalidate
       them without borrowing these mutable layout boxes. */
    web_avmedia_layout_begin(d);
    doc_viewport_update(d,width,height);
    /* debug_js only: why a slow layout ran (cascade scope, box path, passes). */
    uint64_t slow_start=d->profile_enabled?uptime_ms():0,slow_cascade=0,slow_boxes=0;
    unsigned slow_full=0,slow_cascades=0,slow_rebuilds=0,slow_locals=0,slow_restyles=0;
    struct web_profile slow_profile=d->profile;
reflow:
    if (d->need_style || d->styled_w != width || d->styled_h != height || !d->root_box) {
        uint64_t phase=d->profile_enabled?uptime_ms():0;
        slow_full+=d->style_full_dirty;slow_cascades++;
        css_cascade(d, width, height);
        if(d->profile_enabled){uint64_t now=uptime_ms();slow_cascade+=now-phase;phase=now;}
        if (d->native_cancelled) goto cancelled;
        if (d->root) register_bg(d, d->root);
        if (!d->root_box || d->style_boxes_changed || d->need_boxes) {
            doc_restyle_damage_lost(d);
            bool local=false;
            if(d->root_box){
                boxes_restyle(d); /* refresh existing arena borrows before branch publication */
                local=boxes_rebuild_local(d);
            }
            if(local)slow_locals++;else slow_rebuilds++;
            if(d->profile_enabled && !local){
                char line[200];
                snprintf(line,sizeof line,"Box rebuild flags: root %d, style %d, existing %d, dom %d, because %s",
                    !!d->root_box,d->style_boxes_changed,d->style_existing_boxes_changed,d->need_boxes,
                    d->box_full_reason?d->box_full_reason:"no box tree");
                web_js_console(d,1,line);
                css_box_change_report(d);
            }
            if(!local){
                boxes_discard(d);
                css_styles_release(d);
                d->bmem.trap=d->smem.trap;
                boxes_build(d, &d->bmem);
                d->bmem.trap=NULL;
            }else css_styles_release(d);
        } else {
            /* Topology is unchanged. Update computed-style borrows before
               reclaiming old element arenas; dirty boxes drive local layout. */
            doc_restyle_damage_begin(d);
            boxes_restyle(d);
            css_styles_release(d);
            slow_restyles++;
        }
        if(d->profile_enabled)slow_boxes+=uptime_ms()-phase;
        if (d->native_cancelled) goto cancelled;
        d->need_style = false;
        d->need_boxes = false;
    } else if(d->need_boxes) {
        /* Computed styles remain owned by smem. Only text boxes/anonymous
           styles borrow bmem, so old box ownership can be reclaimed safely. */
        doc_restyle_damage_lost(d);
        if(d->root_box && boxes_rebuild_local(d))slow_locals++;
        else{
            boxes_discard(d);
            d->bmem.trap=d->smem.trap;
            boxes_build(d,&d->bmem);
            d->bmem.trap=NULL;
            slow_rebuilds++;
        }
        if (d->native_cancelled) goto cancelled;
        d->need_boxes=false;
    }
    /* A partial cascade clears resource wanted bits too. Re-select unchanged
       font users after publication so their next failed-src candidate survives. */
    if(d->font_selection_dirty) {
        d->font_selection_dirty=false;
        css_refresh_font_selection(d);
        if(d->native_cancelled)goto cancelled;
        if(d->need_style)goto reflow;
    }
    /* A class/effect change first invalidates the document conservatively.
       After cascade publication, unchanged box geometry is still committed:
       paint-only or nongenerated pseudo styles must not age the layout epoch
       or rerun layout/overflow merely because their computed bodies changed.
       Real box/font/intrinsic/viewport changes dirty the root via the existing
       invalidation path; a newly built root has no completed post-layout. */
    /* layout_cache_valid is permission to reuse an inner formatting pass under
       its input key, not validity of already-published post-layout geometry.
       In particular, first-use font initialization can disable that inner
       cache even though the completed post pass and final font epoch remain
       the document's committed geometry. */
    if (committed_geometry_current(d,width,height,font_metrics_generation())) {
        doc_restyle_damage_commit(d);
        goto geometry_committed;
    }
    doc_restyle_damage_lost(d);
    d->width = width;
    d->height = height;
    layout_doc(d, width, height);
    if (d->native_cancelled) goto cancelled;
    /* Publish immutable container inputs only once all layout workers have
       joined. A changed cross-subtree query input conservatively re-cascades.
       A pathological cyclic/nested sheet cannot monopolize one geometry read. */
    bool container_changed=css_containers_update(d);
    if (d->native_cancelled) goto cancelled;
    if(container_changed) {
        d->need_style=true;d->style_full_dirty=true;
        if(++container_pass<8)goto reflow;
        d->layout_revision++;
        d->layout_valid=false;d->dirty=true;
        d->native_layout_depth--;
        return d->doc_h;
    }
    d->layout_revision++;
geometry_committed:
    if(d->profile_enabled && uptime_ms()-slow_start>=250){
        char line[400];uint64_t total=uptime_ms()-slow_start;
        const struct web_profile *p=&d->profile;
        snprintf(line,sizeof line,"Slow layout: %lu ms (cascade %lu ms in %u passes, full %u: %lu elements, %lu rule candidates, %lu filtered; boxes %lu ms: restyle %u, local %u, rebuild %u; geometry %lu ms; container passes %u)",
            (unsigned long)total,(unsigned long)slow_cascade,slow_cascades,slow_full,
            (unsigned long)(p->style_visits-slow_profile.style_visits),
            (unsigned long)(p->style_rule_checks-slow_profile.style_rule_checks),
            (unsigned long)(p->style_rule_filtered-slow_profile.style_rule_filtered),(unsigned long)slow_boxes,
            slow_restyles,slow_locals,slow_rebuilds,(unsigned long)(total-slow_cascade-slow_boxes),container_pass);
        web_js_console(d,1,line);
    }
    boxes_changes_clear(d);
    d->layout_valid = true;
    web_frames_commit_viewports(d);
    if(d->native_recovery_pending){
        d->native_recovery_pending=d->native_recovery_active=false;
        web_js_console(d,1,"Native rendering recovered from coherent DOM.");
    }
    layout_clamp_trace_report(d);
    if (d->find_text && *d->find_text && !d->find_revealing) web_find(d, d->find_text, 0);
    d->native_layout_depth--;
    return d->doc_h;
cancelled:
    /* Never paint/hit-test a half-built tree or label it a valid layout. */
    d->bmem.trap = NULL;
    doc_restyle_damage_lost(d);
    boxes_discard(d);
    css_node_style_unpublish(d->root);
    css_styles_release(d);
    d->layout_valid = false; d->dirty = d->native_recovery_pending;
    d->native_recovery_active=false;
    d->native_layout_depth--;
    return d->doc_h;
}

int web_doc_height(web_doc *d) { return d->doc_h; }
int web_doc_width(web_doc *d) {
    if (!d) return 0;
    double width = MAX(d->width,d->doc_w);
    if (d->root_box && isfinite(d->root_box->scroll_w)) width = MAX(width,d->root_box->scroll_w);
    /* The public viewport API is signed int; internal finite CSS extents are
       not a page-size quota. Compare as double before the narrowing cast. */
    return !isfinite(width) || width <= 0 ? 0 : width >= (double)INT_MAX ? INT_MAX : (int)ceil(width);
}
void web_viewport_position(web_doc *d, int x, int y) {
    if (d) { d->view_x = MAX(x,0); d->view_y = MAX(y,0); }
}

bool doc_element_scroll_metrics(web_doc *d, node_t *n, float *width, float *height) {
    if (width) *width = 0;
    if (height) *height = 0;
    if (!d || !n || n->type != N_ELEM || n->owner != d || !d->live || d->inert ||
        !doc_node_connected(n) || doc_node_root(n, true) != d->root) return false;
    web_layout(d, d->width > 0 ? d->width : 800, d->height > 0 ? d->height : 600);
    box_t *b = n->box;
    if (n->owner != d || !doc_node_connected(n) || !b || b->anon || b->kind == B_INLINE ||
        !b->st || b->st->display == D_NONE) return false;
    if (width) *width = b->scroll_w;
    if (height) *height = b->scroll_h;
    return true;
}

static double element_scroll_limit(box_t *b, int axis) {
    double limit = axis ? (double)b->scroll_h - b->h - b->p[0] - b->p[2] :
                          (double)b->scroll_w - b->w - b->p[1] - b->p[3];
    return isfinite(limit) && limit > 0 ? limit : 0;
}

void doc_paint_full(web_doc *d) {
    for (; d; d = d->frame_parent) {
        d->paint_dirty = true;
        /* A prior video/control rectangle cannot describe shifted content or
           a changed child composite. Never downgrade this geometry damage. */
        d->paint_damage_full = true;
    }
}

/* Shared CSSOM/native commit: never flushes layout or invokes authors. */
static bool element_scroll_commit(web_doc *d, node_t *n, box_t *b, double x, double y) {
    double max_x = element_scroll_limit(b, 0), max_y = element_scroll_limit(b, 1);
    x = !isfinite(x) || !isfinite(max_x) || x < 0 || max_x <= 0 ? 0 : x > max_x ? max_x : x;
    y = !isfinite(y) || !isfinite(max_y) || y < 0 || max_y <= 0 ? 0 : y > max_y ? max_y : y;
    if (n->scroll_x == x && n->scroll_y == y) return false;
    n->scroll_x = x;
    n->scroll_y = y;
    d->visual_revision++;
    doc_paint_full(d); /* scrolling changes presentation, not CSS/layout */
    return true;
}

bool doc_element_scroll(web_doc *d, node_t *n, double x, double y) {
    if (!doc_element_scroll_metrics(d, n, NULL, NULL) || !box_element_scrollable(n->box) ||
        n == d->html) return false;
    return element_scroll_commit(d, n, n->box, x, y);
}

double doc_element_scroll_max(web_doc *d, node_t *n, int axis) {
    if (!doc_element_scroll_metrics(d, n, NULL, NULL) || !box_element_scrollable(n->box) ||
        n == d->html) return 0;
    return element_scroll_limit(n->box, axis ? 1 : 0);
}

static void element_scroll_input_unqueue(node_t *n) {
    web_doc *old = n->scroll_input_owner;
    if (old) {
        node_t *previous = NULL, *at = old->scroll_input_first;
        while (at && at != n) { previous = at; at = at->scroll_input_next; }
        if (at) {
            if (previous) previous->scroll_input_next = n->scroll_input_next;
            else old->scroll_input_first = n->scroll_input_next;
            if (old->scroll_input_last == n) old->scroll_input_last = previous;
        }
    }
    n->scroll_input_owner = NULL; n->scroll_input_next = NULL;
}

static void element_scroll_input_queue(web_doc *d, node_t *n) {
    if (!d->js || n->scroll_input_owner == d) return;
    /* Adoption can move an allocation-owned node while an old owner still has
       a pending notification. Unlink that queue, not the whole DOM forest. */
    element_scroll_input_unqueue(n);
    n->scroll_input_owner = d; n->scroll_input_next = NULL;
    if (d->scroll_input_last) d->scroll_input_last->scroll_input_next = n;
    else d->scroll_input_first = n;
    d->scroll_input_last = n;
}

/* Integer GUI deltas may meet a fractional CSSOM offset. Round the residual
   towards zero, keeping its sign and never forwarding a consumed fraction as
   an additional pixel to the root. This loses at most a subpixel at a boundary. */
static int element_scroll_remaining(int requested, double consumed) {
    double rest = (double)requested - consumed;
    if (requested > 0) return rest <= 0 ? 0 : rest >= requested ? requested : (int)rest;
    return rest >= 0 ? 0 : rest <= requested ? requested : (int)rest;
}

static bool scroll_input_coherent(web_doc *d) {
    if (!d) return false;
    web_doc *family = d->dom_family ? d->dom_family : d;
    return d->live && !d->inert && d->layout_valid &&
        d->root_box && !d->root_box->layout_dirty && d->root_box->layout_post_valid &&
        !d->need_style && !d->need_boxes && !d->style_pending_dirty &&
        !d->font_selection_dirty && !d->shadow_slots_pending && !d->shadow_dirty_first &&
        !family->shadow_slots_pending && !family->shadow_dirty_first &&
        !d->native_cancelled && !d->native_recovery_pending && !d->native_recovery_active &&
        !d->native_layout_depth;
}

bool web_native_input_ready(web_doc *d,bool prepared_wait) {
    if (!scroll_input_coherent(d) || d->native_checkpoint_depth || parallel_active() ||
        (!prepared_wait && (d->native_input_depth || (d->parser && d->parser->callback_depth)))) return false;
    /* Painting can enter child surfaces too. Do not read a child's half-built
       boxes just because the containing document is currently coherent. */
    for (struct web_frame *f=d->frames; f; f=f->next)
        if (!f->detached && f->document && !web_native_input_ready(f->document,prepared_wait)) return false;
    return true;
}

static web_doc *input_document(web_doc *d,uint64_t identity) {
    if (!d || !d->live) return NULL;
    if (d->input_identity==identity) return d;
    for (struct web_frame *f=d->frames; f; f=f->next) {
        web_doc *found=!f->detached?input_document(f->document,identity):NULL;
        if (found) return found;
    }
    return NULL;
}

bool web_input_target_valid(web_doc *d,const struct web_input_target *target) {
    if (!target) return false;
    web_doc *owner=input_document(d,target->document);
    /* Find the live owner by identity BEFORE dereferencing a target whose
       child document may already have navigated and freed its native arena. */
    return owner && target->stream==web_js_input_stream(owner) &&
        (!target->node || (target->node->owner==owner &&
                          doc_node_root(target->node,true)==owner->root));
}

bool web_input_capture(web_doc *d,web_node *target,struct web_input_target *out) {
    if (!out || !d || !d->live) return false;
    web_doc *owner=target?target->owner:d;
    if (!owner || input_document(d,owner->input_identity)!=owner ||
        (target && doc_node_root(target,true)!=owner->root)) return false;
    *out=(struct web_input_target){target,owner->input_identity,web_js_input_stream(owner)};
    return true;
}

bool web_native_wheel_target(web_doc *d,web_node *target,struct web_input_target *out,bool prepared_wait) {
    if (!web_native_input_ready(d,prepared_wait) || !web_input_capture(d,target,out)) return false;
    web_doc *owner=target?target->owner:d;
    if (owner->wheel_window_blocking) {
        if(d->profile_enabled)web_js_console(d,1,"Native wheel fast: window cancellation listener requires dispatch");
        return false;
    }
    /* Composed wheel ancestors follow the already-published slot assignment.
       No doc_flat_parent/shadow flush, attribute handler compilation or JS. */
    for (node_t *at=target?target:owner->root; at;
         at=at->assigned_slot?at->assigned_slot:at->parent?at->parent:at->shadow_host)
        if (at->wheel_blocking || node_attr(at,"onwheel")) {
            if(d->profile_enabled) {
                char message[160];snprintf(message,sizeof message,"Native wheel fast: ancestor tag %u cancellation listeners %llu/handler %d require dispatch",
                    (unsigned)at->tag,(unsigned long long)at->wheel_blocking,node_attr(at,"onwheel")!=NULL);
                web_js_console(d,1,message);
            }
            return false;
        }
    return true;
}

static struct web_frame *scroll_input_frame(web_doc *child) {
    struct web_frame *f = child->frame_container;
    web_doc *parent = child->frame_parent;
    node_t *element = child->frame_element;
    if (!f || !parent || !element || f->document != child || f->owner != parent ||
        f->element != element || f->detached || element->owner != parent ||
        doc_node_root(element, true) != parent->root || !element->box ||
        !element->box->st || element->box->st->display == D_NONE ||
        f->viewport_w <= 0 || f->viewport_h <= 0 ||
        child->width != f->viewport_w || child->height != f->viewport_h ||
        !isfinite(element->box->w) || !isfinite(element->box->h) ||
        round((double)element->box->w) != f->viewport_w ||
        round((double)element->box->h) != f->viewport_h) return NULL;
    return f;
}

static box_t *scroll_input_box(node_t *n, web_doc *d) {
    box_t *b = NULL;
    /* Assignment snapshots are coherent above. doc_flat_parent would flush
       Shadow DOM here, potentially running reactions during an active task. */
    for (node_t *at = n; at && !b;) {
        b = at->box ? at->box : at->anchor_block;
        if (at->assigned_slot) at = at->assigned_slot;
        else at = at->parent ? at->parent : at->shadow_host;
    }
    if (!b) return NULL;
    box_t *root = b;
    while (root->parent) root = root->parent;
    return root == d->root_box ? b : NULL;
}
static bool scroll_input_result(web_doc *d,bool handled,int x,int y,int requested_x,int requested_y) {
    if(d->profile_enabled) {
        char message[192];snprintf(message,sizeof message,
            "Native scroll chain: handled %d, request %d,%d residual %d,%d, extent %d/%d, view %d",
            handled,requested_x,requested_y,x,y,d->doc_h,d->height,d->view_y);
        web_js_console(d,1,message);
    }
    return handled;
}

bool web_scroll_input(web_doc *d, web_node *target, int *dx, int *dy, int edge) {
    node_t *n = target;
    if (!d || !n || !n->owner || !dx || !dy || edge < -1 || edge > 1 ||
        doc_node_root(n, true) != n->owner->root) return false;
    /* Validate the complete attached browsing-context chain before committing
       any offset. Native input crosses origins, never JS wrappers/authority. */
    web_doc *owner = n->owner;
    for (web_doc *at = owner;; at = at->frame_parent) {
        if (!scroll_input_coherent(at)) {
            if(d->profile_enabled) {
                char message[192];snprintf(message,sizeof message,"Native scroll chain: incoherent owner, valid %d/style %d/boxes %d, checkpoint %u/layout %u, viewport %d/%d",
                    at->layout_valid,at->need_style,at->need_boxes,(unsigned)at->native_checkpoint_depth,(unsigned)at->native_layout_depth,at->doc_h,at->height);
                web_js_console(d,1,message);
            }
            return false;
        }
        if (at == d) break;
        if (!scroll_input_frame(at)) return false;
    }
    box_t *b = scroll_input_box(n, owner);
    if (!b) return false;
    int requested_x=*dx,requested_y=*dy;
    bool handled = false;
    for (;;) {
      for (; b; b = b->parent) {
        node_t *element = b->node;
        if (!box_element_scrollable(b) || !element || element->owner != owner ||
            element == owner->html || doc_viewport_overflow_box(owner, b) ||
            (b->st->overflow != OV_AUTO && b->st->overflow != OV_SCROLL)) continue;
        double x = element->scroll_x, y = element->scroll_y;
        if (!isfinite(x) || !isfinite(y)) continue;
        if (edge) {
            double max_y = element_scroll_limit(b, 1);
            if (max_y <= 0) continue;
            if (element_scroll_commit(owner, element, b, x, edge < 0 ? 0 : max_y)) {
                element_scroll_input_queue(owner, element);
            }
            *dy = 0; return scroll_input_result(d,true,*dx,*dy,requested_x,requested_y); /* also consume Home/End already at its edge */
        }
        if (element_scroll_commit(owner, element, b, x + *dx, y + *dy)) {
            *dx = element_scroll_remaining(*dx, element->scroll_x - x);
            *dy = element_scroll_remaining(*dy, element->scroll_y - y);
            element_scroll_input_queue(owner, element); handled = true;
        }
        if (!*dx && !*dy) return scroll_input_result(d,handled,*dx,*dy,requested_x,requested_y);
      }
      if (owner == d) return scroll_input_result(d,handled,*dx,*dy,requested_x,requested_y);
      struct web_frame *f = scroll_input_frame(owner); /* no callbacks above */
      int mx = MAX(0, web_doc_width(owner) - f->viewport_w);
      int my = MAX(0, owner->doc_h - f->viewport_h);
      int x = f->scroll_x, y = f->scroll_y;
      int64_t wanted_x = (int64_t)x + *dx;
      int64_t wanted_y = edge ? (edge < 0 ? 0 : my) : (int64_t)y + *dy;
      bool edge_handled = edge && my > 0;
      if (!edge || edge_handled) {
        f->scroll_x = wanted_x < 0 ? 0 : wanted_x > mx ? mx : (int)wanted_x;
        f->scroll_y = wanted_y < 0 ? 0 : wanted_y > my ? my : (int)wanted_y;
        if (f->scroll_x != x || f->scroll_y != y) {
            owner->view_x = f->scroll_x; owner->view_y = f->scroll_y;
            owner->visual_revision++; doc_paint_full(owner);
            if (owner->js) owner->scroll_viewport_pending = true;
            /* Preserve normal frame-tick propagation, including intermediate
               composites if another native wait occurs before that tick. */
            handled = true;
        }
        if (edge_handled) { *dy = 0; return scroll_input_result(d,true,*dx,*dy,requested_x,requested_y); }
        *dx = element_scroll_remaining(*dx, (double)f->scroll_x - x);
        *dy = element_scroll_remaining(*dy, (double)f->scroll_y - y);
        if (!*dx && !*dy) return scroll_input_result(d,handled,*dx,*dy,requested_x,requested_y);
      }
      b = f->element->box->parent;
      owner = f->owner;
    }
}

/* Physical horizontal-LTR alignment. nearest deliberately leaves an oversized
   target spanning both viewport edges alone, rather than oscillating edges. */
static double scroll_align_delta(double first, double last, double start, double size, int align) {
    double end = start + size, length = last - first;
    if (align == DOC_SCROLL_START) return first - start;
    if (align == DOC_SCROLL_END) return last - end;
    if (align == DOC_SCROLL_CENTER) return (first + last - start - end) / 2;
    bool before = first < start, after = last > end;
    if (before && after) return 0;
    if (before) return length <= size ? first - start : last - end;
    if (after) return length <= size ? last - end : first - start;
    return 0;
}

struct scroll_target_union { double left, top, right, bottom; bool found; };
static bool scroll_target_span(void *opaque, const box_t *space, double x, double y, double w, double h) {
    (void)space;
    struct scroll_target_union *u = opaque;
    if (!u->found) { u->left = x; u->top = y; u->right = x + w; u->bottom = y + h; u->found = true; return true; }
    if (x < u->left) u->left = x;
    if (y < u->top) u->top = y;
    if (x + w > u->right) u->right = x + w;
    if (y + h > u->bottom) u->bottom = y + h;
    return true;
}
static bool scroll_target_rect(node_t *n, double r[4]) {
    box_t *b = n->box;
    if (!b || !b->st || b->st->display == D_NONE) return false;
    if (b->kind != B_INLINE) {
        r[0] = box_visual_x(b) - b->p[3] - b->b[3];
        r[1] = box_visual_y(b) - b->p[0] - b->b[0];
        r[2] = b->w + b->p[1] + b->p[3] + b->b[1] + b->b[3];
        r[3] = b->h + b->p[0] + b->p[2] + b->b[0] + b->b[2];
    } else {
        /* Native inline fragments are deco spans in their formatting block (layout owns
           which), not a fabricated parentNode relationship or the unpositioned inline box. */
        struct scroll_target_union u = {0};
        layout_inline_rects(n, true, scroll_target_span, &u);
        if (!u.found) return false;
        r[0] = u.left; r[1] = u.top; r[2] = u.right - u.left; r[3] = u.bottom - u.top;
    }
    return isfinite(r[0]) && isfinite(r[1]) && isfinite(r[2]) && isfinite(r[3]);
}

int doc_element_scroll_into_view(web_doc *d, node_t *n, int block, int inline_, bool nearest_only,
                                double *x, double *y, struct doc_scroll_target **targets, int *count_out) {
    if (count_out) *count_out = 0;
    if (!d || !n || !x || !y || !targets || *targets || !count_out ||
        block < DOC_SCROLL_START || block > DOC_SCROLL_NEAREST ||
        inline_ < DOC_SCROLL_START || inline_ > DOC_SCROLL_NEAREST || n->type != N_ELEM ||
        n->owner != d || !d->live || d->inert || !doc_node_connected(n) ||
        doc_node_root(n, true) != d->root) return 0;
    web_layout(d, d->width > 0 ? d->width : 800, d->height > 0 ? d->height : 600);
    double target[4];
    if (!n->box || !n->box->st || n->box->st->display == D_NONE) return 0;
    box_t *first = n->box->kind == B_INLINE ? n->anchor_block : n->box->cb;
    int count = 0;
    box_t *slow = first, *fast = first;
    bool fixed = n->box->st->position == POS_FIXED;
    for (box_t *a = first; a; a = a->cb) {
        /* Reject an actual invalid containing-block cycle, not an arbitrary
           number of legal ancestors. Detect it before mutating any offset. */
        slow = slow ? slow->cb : NULL;
        fast = fast && fast->cb ? fast->cb->cb : NULL;
        if (slow && slow == fast) return -2;
        if (a->st && a->st->position == POS_FIXED) fixed = true;
        if (box_element_scrollable(a) && (!nearest_only || !count)) {
            if (count == INT_MAX) return -3; /* pvec's actual index type */
            count++;
        }
    }
    if (!scroll_target_rect(n, target)) return 0;
    struct doc_scroll_target *out = NULL;
    double *from = NULL;
    if (count) {
        if ((size_t)count > SIZE_MAX / sizeof *out / 2) return -3;
        out = malloc((size_t)count * sizeof *out);
        from = malloc((size_t)count * 2 * sizeof *from);
        if (!out || !from) { free(out); free(from); return -1; }
    }
    int used = 0;
    bool viewport = !fixed;
    for (box_t *a = first; a; a = a->cb) {
        if (!box_element_scrollable(a)) continue;
        if (!scroll_target_rect(n, target)) { viewport = false; break; }
        double left = box_visual_x(a) - a->p[3], top = box_visual_y(a) - a->p[0];
        double dx = scroll_align_delta(target[0], target[0] + target[2], left, a->w + a->p[1] + a->p[3], inline_);
        double dy = scroll_align_delta(target[1], target[1] + target[3], top, a->h + a->p[0] + a->p[2], block);
        node_t *e = a->node;
        double old_x = e->scroll_x, old_y = e->scroll_y;
        /* Each outer box is aligned as if this one already had its position. */
        if (doc_element_scroll(d, e, e->scroll_x + dx, e->scroll_y + dy)) {
            out[used] = (struct doc_scroll_target){e, e->scroll_x, e->scroll_y};
            from[2 * used] = old_x; from[2 * used + 1] = old_y; used++;
        }
        if (nearest_only) { viewport = false; break; }
    }
    if (viewport && scroll_target_rect(n, target)) {
        /* This document's real viewport only. There is no traversal through a
           fabricated frame parent or access to a different-origin document. */
        *x += scroll_align_delta(target[0], target[0] + target[2], *x, d->width, inline_);
        *y += scroll_align_delta(target[1], target[1] + target[3], *y, d->height, block);
    }
    /* Put every box back: performing the scrolls (and their events) is the caller's. */
    for (int i = used; i-- > 0;) { out[i].node->scroll_x = from[2 * i]; out[i].node->scroll_y = from[2 * i + 1]; }
    if (used) { d->visual_revision++; doc_paint_full(d); }
    free(from);
    if (!used) { free(out); out = NULL; }
    *targets = out; *count_out = used;
    return 1;
}

const char *doc_link_href(web_doc *d, node_t *a) {
    if (!a || !a->owner) return NULL;
    /* Adoption changes the URL base even if the caller retained its old doc. */
    d = a->owner;
    if (d->resources_dirty) doc_sync_tree(d);
    const char *h = node_attr(a, "href");
    if (!h) return NULL;
    while (is_space((unsigned char)*h)) h++;
    if (!strncasecmp(h, "javascript:", 11)) return NULL;
    char *resolved=NULL;
    if(doc_encoding_parse_url(d,h,&resolved)!=1)return NULL;
    free(d->resolved_link);d->resolved_link=resolved;
    return d->resolved_link;
}

/* the border-box top of the element's first box, or of the line its first inline box is on */
static bool node_top(node_t *n, float *y) {
    if (n->box && n->box->kind != B_INLINE) {
        box_t *b = n->box;
        *y = box_visual_y(b) - b->p[0] - b->b[0];
        return true;
    }
    if (n->anchor_block) {
        *y = box_visual_y(n->anchor_block) + n->anchor_block->content_dy + n->anchor_dy;
        if (box_element_scrollable(n->anchor_block)) *y -= (float)n->anchor_block->node->scroll_y;
        return true;
    }
    for (node_t *c = n->first; c; c = c->next)
        if (c->type == N_ELEM && node_top(c, y)) return true;
    return false;
}

static node_t *find_anchor(node_t *root, const char *frag, bool named) {
    /* Real document tree order, without native-stack depth or a fixed quota.
       All matching IDs precede the legacy named-anchor fallback. */
    for (node_t *c = root ? root->first : NULL; c;) {
        if (c->type == N_ELEM && !named && c->id && !strcmp(c->id, frag)) return c;
        if (named && c->type == N_ELEM && c->tag == T_a && !c->foreign) {
            const char *nm = node_attr(c, "name");
            if (nm && !strcmp(nm, frag)) return c;
        }
        if (c->first) { c = c->first; continue; }
        while (c != root && !c->next) c = c->parent;
        if (c == root) break;
        c = c->next;
    }
    return NULL;
}

/* Fragment percent-decoding also uses a hex digit conversion. Keep this
 * document-navigation helper independent of Fetch's private data decoder. */
static int hexv(int c) {
    if(c>='0'&&c<='9')return c-'0';
    c=lower(c);return c>='a'&&c<='f'?c-'a'+10:-1;
}

int web_anchor_y(web_doc *d, const char *fragment) {
    if (!d || !fragment || !d->root) return -1;
    if (!*fragment) return 0;
    node_t *n = find_anchor(d->root, fragment, false);
    if (!n) n = find_anchor(d->root, fragment, true);
    float y;
    if (n) {
        if (!node_top(n, &y)) return -1;
        return !isfinite(y) || y < 0 ? 0 : (double)y >= (double)INT_MAX ? INT_MAX : (int)y;
    }
    /* Percent decoding cannot increase the byte length. Retain the entire
       fragment instead of silently matching an unrelated 255-byte prefix. */
    size_t length = strlen(fragment);
    if (length == SIZE_MAX) return -1;
    char *f = malloc(length + 1);
    if (!f) return -1;
    size_t k = 0;
    for (const char *p = fragment; *p; p++) {
        if (*p == '%' && hexv(p[1]) >= 0 && hexv(p[2]) >= 0) {
            f[k++] = (char)(hexv(p[1]) * 16 + hexv(p[2]));
            p += 2;
        } else f[k++] = *p;
    }
    f[k] = 0;
    /* The byte-oriented native attribute layer cannot represent an embedded
       NUL. Do not turn it into a successful lookup of an unrelated prefix. */
    if (memchr(f, 0, k)) { free(f); return -1; }
    n = find_anchor(d->root, f, false);
    if (!n) n = find_anchor(d->root, f, true);
    bool top = str_ieq(f, "top");
    free(f);
    if (!n && top) return 0;
    if (!n || !node_top(n, &y)) return -1;
    return !isfinite(y) || y < 0 ? 0 : (double)y >= (double)INT_MAX ? INT_MAX : (int)y;
}

static void hover_target_set(web_doc *d,node_t *target) {
    if(d->hover_target==target)return;
    node_t *old=d->hover_target;
    d->hover_target=target;
    if(d->live){
        css_mark_state_moved(d,CSS_STATE_HOVER,old,target);d->dirty=true;
        for(web_doc *p=d->frame_parent;p && p->live;p=p->frame_parent)p->dirty=true;
    }
}
void doc_hover_update(web_doc *d,node_t *target) {
    if(!d)return;
    web_doc *root=d;while(root->frame_parent)root=root->frame_parent;
    web_doc *leaf=target?target->owner:NULL;
    if(leaf && (!leaf->live || doc_node_root(target,true)!=leaf->root || web_dialog_inert(leaf,target)))leaf=NULL;
    if(leaf){
        web_doc *p=leaf;
        while(p && p!=root){
            struct web_frame *f=web_frame_find(p->frame_parent,p->frame_element);
            if(!p->frame_parent || !p->frame_parent->live || !f || f->detached || f->document!=p ||
               doc_node_root(p->frame_element,true)!=p->frame_parent->root){p=NULL;break;}
            p=p->frame_parent;
        }
        if(p!=root)leaf=NULL;
    }
    if(!leaf)target=NULL;
    if(root->hover_leaf==leaf && (!leaf || leaf->hover_target==target))return;
    /* Find the context-chain intersection without allocating or scanning the
       whole frame forest. Parent frame hover survives movement within a child. */
    web_doc *old=root->hover_leaf,*a=old,*b=leaf;size_t da=0,db=0;
    for(web_doc *p=a;p;p=p->frame_parent)da++;
    for(web_doc *p=b;p;p=p->frame_parent)db++;
    while(da>db){a=a->frame_parent;da--;}
    while(db>da){b=b->frame_parent;db--;}
    while(a!=b){a=a->frame_parent;b=b->frame_parent;}
    for(web_doc *p=old;p && p!=a;p=p->frame_parent)hover_target_set(p,NULL);
    for(web_doc *p=leaf;p;p=p->frame_parent){
        hover_target_set(p,target);target=p->frame_element;
    }
    root->hover_leaf=leaf;
}

static void active_target_set(web_doc *d,node_t *target) {
    if(d->active_target==target)return;
    node_t *old=d->active_target;
    d->active_target=target;
    if(d->live){
        css_mark_state_moved(d,CSS_STATE_ACTIVE,old,target);d->dirty=true;
        for(web_doc *p=d->frame_parent;p && p->live;p=p->frame_parent)p->dirty=true;
    }
}
static bool active_target_valid(web_doc *root,web_doc *leaf,node_t *target) {
    if(!root || !root->live || !leaf || !leaf->live || !target || target->owner!=leaf ||
       doc_node_root(target,true)!=leaf->root || web_dialog_inert(leaf,target))return false;
    for(web_doc *p=leaf;p!=root;p=p->frame_parent){
        if(!p || !p->frame_parent || !p->frame_parent->live)return false;
        struct web_frame *f=web_frame_find(p->frame_parent,p->frame_element);
        if(!f || f->detached || f->document!=p ||
           doc_node_root(p->frame_element,true)!=p->frame_parent->root ||
           web_dialog_inert(p->frame_parent,p->frame_element))return false;
    }
    return true;
}
web_node *web_pointer_click_target(web_doc *d,node_t *down,node_t *up) {
    if(!d || !down || !up || down->owner!=up->owner)return NULL;
    web_doc *root=d;while(root->frame_parent)root=root->frame_parent;
    web_doc *leaf=down->owner;
    if(!leaf || leaf->inert || !active_target_valid(root,leaf,down) ||
       !active_target_valid(root,leaf,up))return NULL;
    /* DOM inclusive ancestry, not CSS boxes, slots, or a synthetic ancestor
       across separate shadow roots/documents. Native insertion cycle checks
       already guarantee finite parent chains. No allocation or depth quota. */
    node_t *a=down,*b=up,*ar=NULL,*br=NULL;
    size_t da=0,db=0;
    for(node_t *p=a;p;p=p->parent){ar=p;da++;}
    for(node_t *p=b;p;p=p->parent){br=p;db++;}
    if(ar!=br)return NULL;
    while(da>db){a=a->parent;da--;}
    while(db>da){b=b->parent;db--;}
    while(a!=b){a=a->parent;b=b->parent;}
    return a;
}
void web_active_release(web_doc *d) {
    if(!d)return;
    web_doc *root=d;while(root->frame_parent)root=root->frame_parent;
    web_doc *old=root->active_leaf;root->active_leaf=NULL;
    for(web_doc *p=old;p;p=p->frame_parent)active_target_set(p,NULL);
}
void web_active_press(web_doc *d,node_t *target) {
    if(!d)return;
    web_doc *root=d;while(root->frame_parent)root=root->frame_parent;
    web_doc *leaf=target?target->owner:NULL;
    web_active_release(root);
    if(!active_target_valid(root,leaf,target))return;
    root->active_leaf=leaf;
    for(web_doc *p=leaf;p;p=p->frame_parent){
        active_target_set(p,target);target=p->frame_element;
    }
}
void doc_active_cancel(web_doc *d) {
    if(!d)return;
    web_doc *root=d;while(root->frame_parent)root=root->frame_parent;
    for(web_doc *p=root->active_leaf;p;p=p->frame_parent)
        if(p==d){web_active_release(root);return;}
}
node_t *doc_active_target(web_doc *d) {
    if(!d || !d->live)return NULL;
    web_doc *root=d;while(root->frame_parent)root=root->frame_parent;
    web_doc *leaf=root->active_leaf;
    if(!active_target_valid(root,leaf,leaf?leaf->active_target:NULL))return NULL;
    for(web_doc *p=leaf;p;p=p->frame_parent)if(p==d)return d->active_target;
    return NULL;
}

bool web_node_rect(web_doc *d, web_node *n, int *x, int *y, int *w, int *h) {
    if (!n || !n->box) return false;
    web_doc *owner=n->owner;
    if (d && d->live) {
        node_t *root = doc_node_root(n, true);
        web_doc *ancestor=owner;while(ancestor && ancestor!=d)ancestor=ancestor->frame_parent;
        if (!owner || !owner->live || root!=owner->root || !ancestor) return false;
    }
    double rect[4];
    if (!scroll_target_rect(n, rect)) return false;
    for (unsigned i=0;i<4;i++) if(rect[i]<INT32_MIN || rect[i]>INT32_MAX)return false;
    *x = (int)rect[0];
    *y = (int)rect[1];
    for(web_doc *child=owner;child && child!=d;child=child->frame_parent) {
        struct web_frame *f=web_frame_find(child->frame_parent,child->frame_element);
        if(!f || f->document!=child || f->detached || !child->frame_element->box)return false;
        *x+=(int)box_visual_x(child->frame_element->box)-f->scroll_x;
        *y+=(int)box_visual_y(child->frame_element->box)-f->scroll_y;
    }
    *w = (int)rect[2];
    *h = (int)rect[3];
    return true;
}

/* ---------------------------------------------------------------- forms */
bool doc_control_selection_supported(const node_t *n) {
    if (!n || n->type != N_ELEM || n->foreign) return false;
    if (n->tag == T_textarea) return true;
    if (n->tag != T_input) return false;
    const char *type = node_attr(n, "type");
    if (!type || !*type || str_ieq(type, "text") || str_ieq(type, "search") ||
        str_ieq(type, "url") || str_ieq(type, "tel") || str_ieq(type, "password")) return true;
    /* Unknown type keywords use the Text missing/invalid-value default. */
    static const char *other[] = {"hidden", "email", "date", "month", "week", "time", "datetime-local",
        "number", "range", "color", "checkbox", "radio", "file", "submit", "image", "reset", "button"};
    for (size_t i = 0; i < sizeof other / sizeof *other; i++) if (str_ieq(type, other[i])) return false;
    return true;
}

/* Control strings use UTF-8, with WTF-8 for isolated JS UTF-16 surrogates.
   Count a scalar outside the BMP as two code units; never count bytes as units.
   CharacterData is the same encoding, counted: its data may hold U+0000. */
static uint32_t utf16_decode(const char *text, size_t length, size_t *byte) {
    const unsigned char *p = (const unsigned char *)text + *byte;
    size_t room = length - *byte;
    uint32_t cp = *p; size_t n = 1;
    if (cp >= 0xc2 && cp <= 0xf4) {
        size_t want = cp < 0xe0 ? 2 : cp < 0xf0 ? 3 : 4;
        uint32_t out = cp & (want == 2 ? 31 : want == 3 ? 15 : 7);
        size_t j = 1;
        while (j < want && j < room && (p[j] & 0xc0) == 0x80) { out = (out << 6) | (p[j] & 63); j++; }
        if (j == want && out <= 0x10ffff && out >= (want == 2 ? 0x80u : want == 3 ? 0x800u : 0x10000u)) { cp = out; n = want; }
    }
    *byte += n; return cp;
}
/* A NUL ends a control string: it is not a continuation byte either. */
static uint32_t control_decode(const char *text, size_t *byte) { return utf16_decode(text, SIZE_MAX, byte); }
uint32_t doc_text_utf16(const char *text, size_t length, size_t byte) {
    uint32_t units = 0; size_t at = 0;
    if (byte > length) byte = length;
    while (text && at < byte) units += utf16_decode(text, length, &at) > 0xffff ? 2 : 1;
    return units;
}
size_t doc_text_byte(const char *text, size_t length, uint32_t offset, bool round_up) {
    size_t byte = 0; uint32_t at = 0;
    while (text && byte < length && at < offset) {
        size_t before = byte; uint32_t count = utf16_decode(text, length, &byte) > 0xffff ? 2 : 1;
        if (offset - at < count) return round_up ? byte : before;
        at += count;
    }
    return byte;
}
uint32_t doc_utf16_length(const char *text) {
    uint32_t length = 0; size_t byte = 0;
    if (text) while (text[byte]) length += control_decode(text, &byte) > 0xffff ? 2 : 1;
    return length;
}
uint32_t doc_byte_to_utf16(const char *text, size_t byte) {
    uint32_t length = 0; size_t at = 0;
    if (text) while (text[at] && at < byte) length += control_decode(text, &at) > 0xffff ? 2 : 1;
    return length;
}
size_t doc_utf16_to_byte(const char *text, uint32_t offset, bool round_up) {
    size_t byte = 0; uint32_t at = 0;
    if (text) while (text[byte] && at < offset) {
        size_t before = byte; uint32_t count = control_decode(text, &byte) > 0xffff ? 2 : 1;
        if (offset - at < count) return round_up ? byte : before;
        at += count;
    }
    return byte;
}
void doc_control_caret(web_doc *d, node_t *n) {
    if (d->focus == n) d->caret = (int)doc_utf16_to_byte(web_input_edit_text(n),
        n->selection_direction == 2 ? n->selection_start : n->selection_end, false);
}
void doc_control_selection(web_doc *d, node_t *n, uint32_t start, uint32_t end, uint8_t direction) {
    uint32_t length = doc_utf16_length(web_input_edit_text(n));
    if (start > length) start = length;
    if (end > length) end = length;
    if (end <= start) start = end;
    if (direction > 2) direction = 0;
    bool changed = n->selection_start != start || n->selection_end != end || n->selection_direction != direction;
    n->selection_start = start; n->selection_end = end; n->selection_direction = direction;
    n->selection_set = true;
    doc_control_caret(d, n);
    if (changed) { doc_paint_damage(d,n); web_js_selection_changed(d, n); }
}
static void control_surrogate(sbuf *b, uint32_t cp) {
    char enc[] = {(char)(0xe0 | (cp >> 12)), (char)(0x80 | ((cp >> 6) & 63)), (char)(0x80 | (cp & 63))};
    sb_put(b, enc, 3);
}
static void control_slice(sbuf *out, const char *text, uint32_t start, uint32_t end) {
    size_t byte = 0; uint32_t at = 0;
    while (text[byte] && at < end) {
        size_t before = byte; uint32_t cp = control_decode(text, &byte), count = cp > 0xffff ? 2 : 1;
        if (start <= at && at + count <= end) sb_put(out, text + before, byte - before);
        else if (count == 2) {
            if (start <= at && at < end) control_surrogate(out, 0xd800 + ((cp - 0x10000) >> 10));
            if (start <= at + 1 && at + 1 < end) control_surrogate(out, 0xdc00 + ((cp - 0x10000) & 1023));
        }
        at += count;
    }
}
static bool control_replace(web_doc *d, node_t *n, uint32_t start, uint32_t end, const char *text, bool user) {
    const char *value = user ? web_input_edit_text(n) : n->value;
    if (!value) value = "";
    uint32_t length = doc_utf16_length(value);
    if (start > length) start = length;
    if (end > length) end = length;
    if (start > end) return false;
    size_t bytes = strlen(value), replacement = strlen(text);
    if (bytes > SIZE_MAX - 7 || replacement > SIZE_MAX - 7 - bytes) return false;
    /* Reserve enough for both split-surrogate boundaries. Do not let sbuf's
       abort-on-OOM grow path turn a failed edit into a browser process crash. */
    sbuf b = {0}; b.cap = bytes + replacement + 7; b.p = malloc(b.cap);
    if (!b.p) return false;
    control_slice(&b, value, 0, start); sb_puts(&b, text); control_slice(&b, value, end, length);
    bool ok = user ? web_input_user_value(d, n, b.p ? b.p : "", b.n) : doc_node_value(d, n, b.p ? b.p : "", b.n);
    sb_free(&b); return ok;
}
bool doc_control_replace(web_doc *d, node_t *n, uint32_t start, uint32_t end, const char *text) {
    return control_replace(d, n, start, end, text, false);
}

static bool focus_under(node_t *n, node_t *ancestor) {
    for (; n; n = doc_shadow_parent(n)) if (n == ancestor) return true;
    return false;
}
static node_t *focus_delegate(web_doc *d, node_t *parent);
static node_t *focus_area(web_doc *d, node_t *n) {
    if (!n || n->type != N_ELEM || !n->style || n->style->display == D_NONE ||
        (!n->box && !n->anchor_block) ||
        web_dialog_inert(d, n)) return NULL;
    if (n->shadow_root && n->shadow_root->shadow_delegates_focus) {
        if (d->focus && focus_under(d->focus, n)) return d->focus;
        return focus_delegate(d, n->shadow_root);
    }
    if (n->style->visibility || web_control_disabled(n)) return NULL;
    const char *type = node_attr(n, "type");
    bool control = n->tag == T_button || n->tag == T_select || n->tag == T_textarea ||
                   (n->tag == T_input && (!type || !str_ieq(type, "hidden")));
    const char *editable = node_attr(n, "contenteditable");
    bool focusable = (n->tag == T_dialog && !n->foreign && node_attr(n,"open")) || control || ((n->tag == T_a || n->tag == T_area) && node_attr(n, "href")) ||
                     ((n->tag == T_audio || n->tag == T_video) && node_attr(n, "controls")) ||
                     (n->tag == T_summary && doc_details_summary(n->parent)==n) ||
                     (n->tag == T_details && !doc_details_summary(n)) ||
                     node_attr(n, "tabindex") || (editable && !str_ieq(editable, "false")) ||
                     /* Editing hosts are focusable areas; in design mode the document element is one. */
                     (d->design_mode && n->parent == d->root && !n->foreign);
    return focusable ? n : NULL;
}
node_t *web_focus_candidate(web_doc *d, node_t *n) { return focus_area(d,n); }
bool web_pointer_focus_target(web_doc *d, node_t *target, node_t **area) {
    if (!area) return false;
    *area = NULL;
    if (!d || !d->live || d->inert) return false;
    if (!target) return true;
    web_doc *leaf = target->owner;
    if (!leaf || leaf->inert || !active_target_valid(d, leaf, target)) return false;
    web_layout(leaf, leaf->width > 0 ? leaf->width : 800, leaf->height > 0 ? leaf->height : 600);
    /* An inline/SVG child of a control is the event target, not necessarily
       the focusable area. Follow the rendered (slot/shadow) ancestry only;
       do not search the document or cross into a sibling browsing context. */
    for (node_t *n = target; n; n = doc_flat_parent(n)) {
        if (web_control_disabled(n)) return false;
        node_t *candidate = focus_area(leaf, n);
        if (candidate) { *area = candidate; return true; }
    }
    return true;
}
static node_t *focus_descendants(web_doc *d, node_t *parent, bool autofocus) {
    for (node_t *n = parent->first; n; n = n->next) {
        if (n->type != N_ELEM || !n->style || n->style->display == D_NONE || node_attr(n, "inert")) continue;
        node_t *target = (!autofocus || node_attr(n, "autofocus")) ? focus_area(d, n) : NULL;
        if (!target) target = focus_descendants(d, n, autofocus);
        if (target) return target;
    }
    return NULL;
}
node_t *web_autofocus_candidate(web_doc *d) {
    if (!d || d->inert || d->focus || (d->url && strchr(d->url, '#'))) return NULL;
    web_layout(d, d->width > 0 ? d->width : 800, d->height > 0 ? d->height : 600);
    return focus_descendants(d, d->root, true);
}
static node_t *focus_delegate(web_doc *d, node_t *parent) {
    /* HTML's focus delegate uses DOM descendants, not assigned/flattened
       descendants. Shadow boundaries are entered only by focus_area above
       for a host that itself delegates focus. */
    node_t *target = focus_descendants(d, parent, true);
    return target ? target : focus_descendants(d, parent, false);
}

/* Selectors 4 :focus-visible heuristics: an element that takes keyboard input
   (a text-entry control or an editing host) always shows focus; another one
   does unless a pointer press was the latest user input. A focus() call's
   focusVisible decides it instead. */
static bool focus_shows_indicator(web_doc *d, node_t *n) {
    if (d->focus_visible_request) return d->focus_visible_request == 2;
    if (n->type == N_ELEM && !n->foreign) {
        if (n->tag == T_textarea) return true;
        if (n->tag == T_input) {
            enum web_input_kind type = web_input_type(n);
            if (type <= WEB_INPUT_PASSWORD || (type >= WEB_INPUT_DATE && type <= WEB_INPUT_NUMBER)) return true;
        }
        const char *editable = node_attr(n, "contenteditable");
        if (editable && !str_ieq(editable, "false")) return true;
    }
    return d->focus_modality != WEB_MODALITY_POINTER;
}
void web_focus(web_doc *d, web_node *n) {
    if (!d) return;
    if(n && n->owner && n->owner!=d && n->owner->frame_parent){
        web_doc *child=n->owner,*ancestor=child;
        while(ancestor && ancestor!=d)ancestor=ancestor->frame_parent;
        if(ancestor==d){
            web_focus(child,n);
            for(web_doc *parent=child->frame_parent;parent;child=parent,parent=parent->frame_parent){
                parent->focus=child->frame_element;parent->dirty=true;if(parent==d)break;
            }
        }
        return;
    }
    if (n) {
        if (!doc_node_connected(n) || n->owner!=d) return;
        web_layout(d, d->width > 0 ? d->width : 800, d->height > 0 ? d->height : 600);
        n=focus_area(d,n);
        if(!n)return;
    }
    if (n && web_control_disabled(n)) return;
    if (n) doc_control_init(d, n);
    /* Keep Nocturne's existing first native focus at the end of a pristine
       control. An explicit JS range (including 0,0), value setter, or remembered
       user range takes priority; the unfocused initial DOM cursor is still 0. */
    if (n && (n->tag == T_input || n->tag == T_textarea) && !n->selection_set) {
        n->selection_start = n->selection_end = doc_utf16_length(web_input_edit_text(n));
        n->selection_direction = 0; n->selection_set = true;
    }
    node_t *old = d->focus;
    bool was_visible = d->focus_visible;
    d->focus = n;
    d->caret = 0;
    d->focus_visible = n && focus_shows_indicator(d, n);
    if (n) doc_control_caret(d, n);
    css_mark_state_moved(d, CSS_STATE_FOCUS, old, n);
    if (n && old == n && was_visible != d->focus_visible) css_mark_dirty(d, n);
    d->dirty = true;
}
/* Selectors 4 :focus-visible after a key press: the focused element shows its
   indicator from then on, though the key moved no focus. */
static void focus_keyboard_input(web_doc *d, const struct web_event *e) {
    static const char *const modifiers[] = {"Alt", "AltGraph", "CapsLock", "Control", "Fn", "Meta", "NumLock", "Shift"};
    if (e->ctrl || e->alt) return; /* a shortcut, not keyboard navigation or typing */
    for (size_t i = 0; e->key && i < sizeof modifiers / sizeof *modifiers; i++) if (!strcmp(e->key, modifiers[i])) return;
    d->focus_modality = WEB_MODALITY_KEYBOARD;
    if (d->focus && !d->focus_visible) { d->focus_visible = true; css_mark_dirty(d, d->focus); }
}

web_node *web_focused(web_doc *d) {
    while(d && web_frame_element(d->focus)){
        struct web_frame *f=web_frame_find(d,d->focus);if(!f || !f->document || f->detached)break;
        d=f->document;
    }
    return d && !web_dialog_inert(d,d->focus) ? d->focus : NULL;
}
web_node *web_disclosure_focus(web_node *details) {
    if (!details || details->foreign || details->tag!=T_details) return NULL;
    node_t *summary=doc_details_summary(details);
    return summary?summary:details;
}

static bool readonly(node_t *n) {
    if (web_control_disabled(n)) return true;
    if (!node_attr(n,"readonly")) return false;
    if (n->tag==T_textarea) return true;
    enum web_input_kind type=web_input_type(n);
    return type<=WEB_INPUT_PASSWORD || (type>=WEB_INPUT_DATE && type<=WEB_INPUT_NUMBER);
}

#include "control_edit.h"

/* js_document_commands.js holds the definitions; this is the native reader's copy of the same walk. */
bool doc_node_editable(web_doc *d, node_t *n) {
    if (!d || !d->live || d->inert) return false;
    if (d->design_mode) return true;
    for (; n && n->owner == d; n = n->parent) {
        if (n->type != N_ELEM) continue;
        const char *editable = node_attr(n, "contenteditable");
        if (!editable) continue;
        if (!*editable || str_ieq(editable, "true") || str_ieq(editable, "plaintext-only")) return true;
        if (str_ieq(editable, "false")) return false;
    }
    return false;
}
/* The keys go to an editing host: the document's focus is editable. */
static bool editing_focus(web_doc *d) { return d && doc_node_editable(d, d->focus); }
/* An editing key, as the editing layer's action: an Input Events inputType, or a
   caret move (extend with Shift). The editing layer fires beforeinput and input
   and takes the action through the command execCommand runs. */
static int editing_key(web_doc *d, const struct gui_event *e) {
    uint32_t k = e->key;
    bool shift = (e->mods & NMOD_SHIFT) != 0, ctrl = (e->mods & NMOD_CTRL) != 0;
    const char *action = NULL;
    char enc[5]; size_t length = 0; bool text = false;
    if (e->mods & NMOD_ALT) return 0;
    if (ctrl) {
        uint32_t c = k >= 'A' && k <= 'Z' ? k + ('a' - 'A') : k;
        if (c == 'a') action = "selectAll";
        else if (c == 'z') action = shift ? "historyRedo" : "historyUndo";
        else if (c == 'y') action = "historyRedo";
        else if (c == 'b') action = "formatBold";
        else if (c == 'i') action = "formatItalic";
        else if (c == 'u') action = "formatUnderline";
        else return 0;
    } else switch (k) {
    case NKEY_BACKSPACE: action = "deleteContentBackward"; break;
    case NKEY_DELETE: action = "deleteContentForward"; break;
    case NKEY_ENTER: action = shift ? "insertLineBreak" : "insertParagraph"; break;
    case NKEY_LEFT: action = "caretBackward"; break;
    case NKEY_RIGHT: action = "caretForward"; break;
    case NKEY_HOME: action = "caretLineStart"; break;
    case NKEY_END: action = "caretLineEnd"; break;
    default:
        if (k < 32 || (k >= 0x100 && k < 0x200) || k > 0x10FFFF || (k >= 0xd800 && k <= 0xdfff)) return 0;
        length = (size_t)utf8_put(enc, k); enc[length] = 0; text = true;
        action = "insertText";
    }
    int handled = web_js_editing_action(d, action, shift, text ? enc : NULL, length);
    /* The keys that edit stay with the editing host even when it took nothing:
       Backspace must not go back in history from an editor. */
    if (!handled && (text || k == NKEY_BACKSPACE || k == NKEY_DELETE || k == NKEY_ENTER)) return 1;
    return handled;
}
static int web_key_local(web_doc *d, const struct gui_event *e) {
    if(!d||!e||!d->live||d->inert)return 0;
    node_t *n = d->focus;
    /* Range's existing arrow-step UI is numeric, never text replacement. */
    bool range_step=n&&n->owner==d&&n->type==N_ELEM&&!n->foreign&&n->tag==T_input&&
        web_input_type(n)==WEB_INPUT_RANGE&&(e->key==NKEY_UP||e->key==NKEY_DOWN)&&
        doc_node_root(n,true)==d->root&&!web_control_disabled(n)&&!web_dialog_inert(d,n);
    if (!control_edit_target(d,n,false)&&!range_step) return editing_focus(d) ? editing_key(d,e) : 0;
    doc_control_init(d, n);
    if (!n->value && !doc_node_value(d, n, "", 0)) return 0;
    const char *v = web_input_edit_text(n);
    int len = (int)strlen(v);
    doc_control_caret(d, n);
    int c = d->caret;
    uint32_t start = n->selection_start, end = n->selection_end;
    bool split_cursor = start == end && doc_utf16_to_byte(v, start, false) != doc_utf16_to_byte(v, start, true);
    bool shift = (e->mods & NMOD_SHIFT) != 0;
    uint32_t k = e->key;
    bool multi = n->tag == T_textarea;
    if ((e->mods & NMOD_CTRL) && (k == 'a' || k == 'A')) {
        doc_control_selection(d, n, 0, doc_utf16_length(v), 1); return 1;
    }
    switch (k) {
    case NKEY_LEFT:
        if (split_cursor) break; /* already rounded to this scalar's start */
        if (!shift && start != end) { c = (int)doc_utf16_to_byte(v, start, false); break; }
        if (c > 0) {
            c--;
            while (c > 0 && ((unsigned char)v[c] & 0xC0) == 0x80) c--;
        }
        break;
    case NKEY_RIGHT:
        if (split_cursor) { c = (int)doc_utf16_to_byte(v, end, true); break; }
        if (!shift && start != end) { c = (int)doc_utf16_to_byte(v, end, true); break; }
        if (c < len) {
            c++;
            while (c < len && ((unsigned char)v[c] & 0xC0) == 0x80) c++;
        }
        break;
    case NKEY_HOME:
        if (multi && !(e->mods & NMOD_CTRL))
            while (c > 0 && v[c - 1] != '\n') c--;
        else c = 0;
        break;
    case NKEY_END:
        if (multi && !(e->mods & NMOD_CTRL))
            while (c < len && v[c] != '\n') c++;
        else c = len;
        break;
    case NKEY_UP: case NKEY_DOWN: {
        if (!multi) {
            if (readonly(n) || !web_input_numeric(web_input_type(n)) || (e->mods & (NMOD_CTRL|NMOD_ALT))) return 0;
            char *old=strdup(web_input_edit_text(n));if(!old)return 1;
            if (web_input_step(d,n,1,k==NKEY_DOWN)!=WEB_INPUT_OK){free(old);return 1;}
            bool changed=strcmp(old,web_input_edit_text(n));free(old);
            n->control_user_edited=true;
            uint32_t last=doc_utf16_length(web_input_edit_text(n));doc_control_selection(d,n,last,last,0);
            /* A numeric spin is not a text-edit InputEvent. Preserve its
             * previous trusted generic input notification exactly once. */
            if(changed){struct web_event event={.type="input",.bubbles=true};web_js_dispatch(d,n,&event);}
            return 1;
        }
        int line = c; while (line > 0 && v[line - 1] != '\n') line--;
        uint32_t column = doc_byte_to_utf16(v + line, (size_t)(c - line));
        int target;
        if (k == NKEY_UP) {
            if (!line) { c = 0; break; }
            target = line - 1; while (target > 0 && v[target - 1] != '\n') target--;
        } else {
            target = c; while (target < len && v[target] != '\n') target++;
            if (target == len) { c = len; break; } target++;
        }
        int limit = target; while (limit < len && v[limit] != '\n') limit++;
        c = target + (int)doc_utf16_to_byte(v + target, column, false);
        if (c > limit) c = limit;
        break;
    }
    case NKEY_ENTER:
        if (!multi) return 2;
        k = '\n';
        break;
    case NKEY_BACKSPACE: {
        if (readonly(n) || (start == end && !split_cursor && c == 0)) return 1;
        if (split_cursor) start--;
        else if (start == end) { int s = c - 1; while (s > 0 && ((unsigned char)v[s] & 0xc0) == 0x80) s--; start = doc_byte_to_utf16(v, (size_t)s); }
        return control_user_replace(d,n,start,end,"",0,"deleteContentBackward",true)?1:0;
    }
    case NKEY_DELETE: {
        if (readonly(n) || (start == end && c >= len)) return 1;
        if (split_cursor) end++;
        else if (start == end) { int e2 = c + 1; while (e2 < len && ((unsigned char)v[e2] & 0xc0) == 0x80) e2++; end = doc_byte_to_utf16(v, (size_t)e2); }
        return control_user_replace(d,n,start,end,"",0,"deleteContentForward",true)?1:0;
    }
    }
    if (k == NKEY_LEFT || k == NKEY_RIGHT || k == NKEY_HOME || k == NKEY_END || k == NKEY_UP || k == NKEY_DOWN) {
        uint32_t to = doc_byte_to_utf16(v, (size_t)c);
        if (shift) {
            uint32_t anchor = n->selection_direction == 2 ? end : start;
            doc_control_selection(d, n, to < anchor ? to : anchor, to < anchor ? anchor : to, to < anchor ? 2 : 1);
        } else doc_control_selection(d, n, to, to, 0);
        return 1;
    }
    if (e->mods & (NMOD_CTRL | NMOD_ALT)) return 0;
    if (k < 32 && k != '\n') return 0;
    if (k >= 0x100 && k < 0x200) return 0; /* other special keys */
    if (k > 0x10FFFF || (k >= 0xd800 && k <= 0xdfff)) return 0;
    if(readonly(n))return 1;
    char enc[5];
    int el = utf8_put(enc, k);
    enc[el] = 0;
    return control_user_replace(d,n,start,end,enc,(size_t)el,k=='\n'?"insertLineBreak":"insertText",k=='\n')?1:0;
}

static node_t *form_id(node_t *n, const char *id) {
    if (n->type == N_ELEM && n->id && !strcmp(n->id,id)) return n;
    for (node_t *c=n->first;c;c=c->next) { node_t *found=form_id(c,id); if(found)return found; }
    return NULL;
}
static node_t *form_of(web_doc *d, node_t *n) {
    (void)d;
    if (!n) return NULL;
    if (n->tag == T_form && !n->foreign) return n;
    const char *fid = node_attr(n, "form");
    if (fid) {
        node_t *f = *fid && doc_node_connected(n) ? form_id(doc_node_root(n,false),fid) : NULL;
        return f && f->type==N_ELEM && !f->foreign && f->tag==T_form ? f : NULL;
    }
    if (n->parser_form_owner && n->parser_form_owner->owner == n->owner &&
        doc_node_root(n, false) == doc_node_root(n->parser_form_owner, false)) return n->parser_form_owner;
    return node_ancestor(n, T_form);
}

/* The document the keys go to: the focused frame's, through nested frames. */
static web_doc *key_document(web_doc *d) {
    while(d && d->live && !d->inert && web_frame_element(d->focus)) {
        struct web_frame *f=web_frame_find(d,d->focus);if(!f || !f->document || f->detached)break;
        d=f->document;
    }
    return d;
}
static void key_document_changed(web_doc *root,web_doc *d) {
    for(web_doc *p=d;p && p!=root;p=p->frame_parent)if(p->frame_parent)p->frame_parent->dirty=true;
}
int web_key(web_doc *d,const struct gui_event *event) {
    web_doc *root=d;
    d=key_document(d);
    int result=web_key_local(d,event);
    if(result)key_document_changed(root,d);
    return result;
}
bool web_editing_target(web_doc *top) {
    web_doc *d=key_document(top);
    return d && !control_edit_target(d,d->focus,false) && editing_focus(d);
}
int web_editing_edit(web_doc *top,const char *input_type,const char *data,size_t length) {
    web_doc *d=key_document(top);
    if(!input_type||!d||control_edit_target(d,d->focus,false)||!editing_focus(d))return 0;
    if(data&&length&&!control_edit_utf8(data,length))return 0;
    int result=web_js_editing_action(d,input_type,false,data,data?length:0);
    if(result)key_document_changed(top,d);
    return result;
}
char *web_selection_text(web_doc *top,size_t *length) {
    if(length)*length=0;
    web_doc *d=key_document(top);
    return d?web_js_selection_text(d,length):NULL;
}
web_node *web_form_owner(web_doc *d, web_node *control) { return form_of(d, control); }
bool web_take_validation_report(web_doc *d, web_node **control, const char **message, size_t *length) {
    if (!d || !d->validation_report_pending) return false;
    d->validation_report_pending=false;
    node_t *n=d->validation_target;
    if (!n || n->owner!=d || !doc_node_connected(n) || !web_control_will_validate(n)) return false;
    const char *current=web_control_validation_message(d,n);
    size_t size=web_control_validation_message_length(d,n);
    if (!size) return false;
    if (control) *control=web_face_validation_anchor(n);
    if (message) *message=current;
    if (length) *length=size;
    return true;
}

/* Reset current control state without writing default attributes or making it dirty. */
static node_t *reset_form_id(node_t *n, const char *id) {
    if (n->type == N_ELEM && n->id && !strcmp(n->id, id)) return n;
    for (node_t *c = n->first; c; c = c->next) {
        node_t *found = reset_form_id(c, id); if (found) return found;
    }
    return NULL;
}

static bool reset_form_text(node_t *n, char *out, size_t *len) {
    if (n->type == N_TEXT) {
        if (n->textlen > SIZE_MAX - *len) return false;
        if (out && n->textlen) memcpy(out + *len, n->text, n->textlen);
        *len += n->textlen;
    } else for (node_t *c = n->first; c; c = c->next)
        if (!reset_form_text(c, out, len)) return false;
    return true;
}

static bool reset_form_controls(web_doc *d, node_t *root, node_t *form, node_t *n) {
    if (n->type == N_ELEM && !n->foreign && (n->tag == T_input || n->tag == T_textarea || n->tag == T_select)) {
        const char *fid = node_attr(n, "form");
        node_t *owner = fid ? (doc_node_connected(n) ? reset_form_id(root, fid) : NULL) : node_ancestor(n, T_form);
        if (owner == form) {
            if (n->tag == T_input) {
                const char *v = node_attr(n, "value");
                if (!doc_node_value(d, n, v ? v : "", v ? strlen(v) : 0)) return false;
                n->value_dirty = n->checked_dirty = false;
                n->checked = node_attr(n, "checked") != NULL;
                n->control_ready = true;
                const char *type = node_attr(n, "type");
                if (n->checked && type && str_ieq(type, "radio")) {
                    doc_control_checked(d, n, true); n->checked_dirty = false;
                }
            } else if (n->tag == T_textarea) {
                size_t len = 0, used = 0;
                if (!reset_form_text(n, NULL, &len)) return false;
                char *text = malloc(len + 1); if (!text) return false;
                bool ok = reset_form_text(n, text, &used) && doc_node_value(d, n, text, used);
                free(text); if (!ok) return false;
                n->value_dirty = false; n->control_ready = true;
            } else {
                n->selected_set = false; n->control_ready = false;
                for (int i = 0; ; i++) {
                    node_t *option = doc_select_option(n, i); if (!option) break;
                    option->selected_set = option->checked_dirty = false;
                }
                doc_control_init(d, n);
            }
        }
    }
    for (node_t *c = n->first; c; c = c->next)
        if (!reset_form_controls(d, root, form, c)) return false;
    return true;
}

bool doc_form_reset(web_doc *d, node_t *form) {
    if (!d || !form || form->type != N_ELEM || form->foreign || form->tag != T_form) return false;
    node_t *root = form; while (root->parent) root = root->parent;
    bool ok = reset_form_controls(d, root, form, root);
    if (ok) web_js_face_reset(d, form);
    d->dirty = d->need_style = true;
    return ok;
}

static void uncheck_radios(web_doc *d, node_t *scope, node_t *keep) {
    if (scope != keep && web_radio_same_group(d, scope, keep)) {
        doc_control_init(d, scope);
        if (scope->checked) css_mark_form_state(d, scope);
        scope->checked = false;
    }
    for (node_t *c = scope->first; c; c = c->next) uncheck_radios(d, c, keep);
}

void doc_control_checked(web_doc *d, node_t *n, bool checked) {
    if (!d || !n || n->tag != T_input) return;
    doc_control_init(d, n);
    n->checked_dirty = true;
    const char *t = node_attr(n, "type");
    if (checked && t && str_ieq(t, "radio")) {
        const char *name = node_attr(n, "name");
        if (name && *name) {
            uncheck_radios(d, doc_node_root(n, false), n);
        }
    }
    n->checked = checked;
    css_mark_form_state(d, n);
    d->dirty = true;
}

void web_toggle(web_doc *d, web_node *n) {
    if(n && n->owner && n->owner->frame_parent)d=n->owner;
    if (n && n->tag==T_details && !n->foreign) { doc_details_toggle(d,n); return; }
    if (!n || n->tag != T_input || web_control_disabled(n)) return;
    doc_control_init(d, n);
    const char *t = node_attr(n, "type");
    if (web_input_type(n) == WEB_INPUT_CHECKBOX) n->indeterminate = false;
    doc_control_checked(d, n, t && str_ieq(t, "radio") ? true : !n->checked);
}
bool web_reset(web_doc *d, web_node *n) {
    if(n && n->owner && n->owner->frame_parent)d=n->owner;
    if(!d || !n || n->foreign || (n->tag!=T_input && n->tag!=T_button) || web_control_disabled(n)) return false;
    const char *type=node_attr(n,"type");
    if(!type || !str_ieq(type,"reset"))return false;
    node_t *form=web_form_owner(d,n);
    if(!form)return false;
    struct web_event e={.type="reset",.bubbles=true,.cancelable=true};
    if(web_dispatch(d,form,&e))doc_form_reset(d,form);
    return true;
}

node_t *doc_select_option(node_t *sel, int index) {
    if (index < 0) return NULL;
    for (node_t *o = web_select_next_option(sel, NULL); o; o = web_select_next_option(sel, o)) if (!index--) return o;
    return NULL;
}

bool doc_option_selected(node_t *option) {
    if (!option || option->tag != T_option) return false;
    node_t *sel = web_option_select(option);
    if (sel) doc_control_init(sel->owner, sel);
    else if (!option->checked_dirty) { option->checked = node_attr(option, "selected") != NULL; option->checked_dirty = true; }
    return option->checked;
}

static void option_text(node_t *o, sbuf *b) {
    const char *lab = node_attr(o, "label");
    if (lab) {
        sb_puts(b, lab);
        return;
    }
    sbuf t = {0};
    node_text_content(o, &t);
    bool sp = true;
    for (size_t i = 0; i < t.n; i++) {
        if (is_space((unsigned char)t.p[i])) {
            if (!sp) sb_putc(b, ' ');
            sp = true;
        } else sb_putc(b, t.p[i]), sp = false;
    }
    if (b->n && b->p[b->n - 1] == ' ') b->n--;
    sb_free(&t);
}

bool web_submit(web_doc *d, web_node *submitter, char **url, char **body) {
    *url = NULL; *body = NULL;
    struct web_form_request request;
    if (!web_submit_request(d, submitter, &request)) return false;
    *url = request.url; *body = request.body;
    request.url = request.body = NULL; web_submit_request_free(&request); return true;
}

int web_select_options(web_doc *d, web_node *sel, const char **labels, int max, int *selected) {
    if (!d || !sel || sel->tag != T_select || max < 0 || (!labels && max)) return 0;
    if (sel->owner != d) { if (!frame_address_live(d,sel->owner)) return 0; d=sel->owner; }
    doc_control_init(d, sel);
    if (selected) *selected = sel->selected;
    if (!labels) {
        int count=0;
        for (node_t *o=web_select_next_option(sel,NULL); o; o=web_select_next_option(sel,o)) {
            if (count == INT_MAX) return -1;
            count++;
        }
        return count;
    }
    int k = 0;
    for (node_t *o=web_select_next_option(sel,NULL); k < max && o; o=web_select_next_option(sel,o), k++) {
        if (!o->option_label || o->option_label_revision != d->dom_revision) {
            /* The heap buffer remains well-defined after an arena limit jump. */
            sbuf *t = calloc(1, sizeof *t);
            if (!t) return k;
            option_text(o, t);
            jmp_buf trap;
            jmp_buf *old = d->mem.trap;
            d->mem.trap = &trap;
            if (setjmp(trap)) {
                d->mem.trap = old;
                sb_free(t);
                free(t);
                return k;
            }
            o->option_label = ar_strdup(&d->mem, t->p ? sb_cstr(t) : "");
            o->option_label_revision = d->dom_revision;
            d->mem.trap = old;
            sb_free(t);
            free(t);
        }
        labels[k] = o->option_label;
    }
    return k;
}

void web_select_set(web_doc *d, web_node *sel, int index) {
    if(sel && sel->owner && sel->owner->frame_parent)d=sel->owner;
    if (d && sel && sel->tag == T_select && (index == -1 || doc_select_option(sel, index))) {
        node_t *option = doc_select_option(sel, index);
        if (option && web_option_disabled(option)) return;
        if (option && node_attr(sel, "multiple")) web_option_set_selected(d, option, !doc_option_selected(option), true);
        else web_select_set_index(d, sel, index);
    }
}
