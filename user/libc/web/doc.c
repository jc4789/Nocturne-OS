#include "form_face.h"
/* The public API (web.h): documents and their resources (stylesheets, images, data: URLs), the
   style/box/layout pipeline, and forms. */
#include <stdio.h>
#include <ctype.h>
#include <math.h>
#include "nocturne.h"
#include "webi.h"
#include "web_dialog.h"
#include "form_value.h"
#include "form_file.h"
#include "form_validation.h"
#include "elements.h"
#include "js_canvas.h"
#include "image_source.h"
#include "avmedia.h"
#include "js_worker.h"
#include "frame.h"
#include "cssom.h"
#include <limits.h>

/* Document arenas and stylesheet storage grow through the OS allocator. */
static void invalidate_layout_tree(box_t *b);

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
};

struct cached_css {
    char *url, *base, *body;
    size_t n;
    bool done;
    /* A completed fetch is immutable. Keep its parser ownership independent
       of resource-scan snapshots; only scope/order/media instances are rebuilt. */
    arena_t ast_mem;
    sheet_t *ast;
};

/* Repeated shadow <style> blocks need separate scope/order wrappers, not a
   fresh expanded AST per component. This memo is local to one arena snapshot.
   Imported sheets are excluded: their request order is not interchangeable. */
struct css_sheet_reuse {
    struct css_sheet_reuse *next;
    const char *css, *base, *media;
    size_t n;
    sheet_t *sheet;
};

static struct cached_css *cached_css(web_doc *d, const char *url) {
    for (int i = 0; i < d->css_cache.n; i++) {
        struct cached_css *c = d->css_cache.v[i];
        if (!strcmp(c->url, url)) return c;
    }
    return NULL;
}

static bool css_queued(web_doc *d, const char *url) {
    for (int i = 0; i < d->pending_css.n; i++)
        if (!strcmp(((struct pending *)d->pending_css.v[i])->url, url)) return true;
    return false;
}

/* ---------------------------------------------------------------- data: URLs */
static int b64(int c) {
    if (c >= 'A' && c <= 'Z') return c - 'A';
    if (c >= 'a' && c <= 'z') return c - 'a' + 26;
    if (c >= '0' && c <= '9') return c - '0' + 52;
    if (c == '+' || c == '-') return 62;
    if (c == '/' || c == '_') return 63;
    return -1;
}

static int hexv(int c) {
    if (c >= '0' && c <= '9') return c - '0';
    c = lower(c);
    return c >= 'a' && c <= 'f' ? c - 'a' + 10 : -1;
}

/* decode a data: URL into a malloc'd buffer */
static char *data_url(const char *u, size_t *out_n) {
    if (strncasecmp(u, "data:", 5)) return NULL;
    const char *comma = strchr(u, ',');
    if (!comma) return NULL;
    bool base64 = false;
    for (const char *p = u + 5; p + 7 <= comma; p++)
        if (!strncasecmp(p, ";base64", 7)) base64 = true;
    const char *s = comma + 1;
    size_t n = strlen(s);
    char *o = malloc(n + 1);
    if(!o)return NULL;
    size_t k = 0;
    if (base64) {
        uint32_t acc = 0;
        int bits = 0;
        for (size_t i = 0; i < n; i++) {
            int c = (unsigned char)s[i];
            if (c == '%' && i + 2 < n && hexv(s[i + 1]) >= 0 && hexv(s[i + 2]) >= 0) {
                c = hexv(s[i + 1]) * 16 + hexv(s[i + 2]);
                i += 2;
            }
            int v = b64(c);
            if (v < 0) continue;
            acc = acc << 6 | (uint32_t)v;
            bits += 6;
            if (bits >= 8) {
                bits -= 8;
                o[k++] = (char)(acc >> bits);
            }
        }
    } else {
        for (size_t i = 0; i < n; i++) {
            if (s[i] == '%' && i + 2 < n && hexv(s[i + 1]) >= 0 && hexv(s[i + 2]) >= 0) {
                o[k++] = (char)(hexv(s[i + 1]) * 16 + hexv(s[i + 2]));
                i += 2;
            } else o[k++] = s[i];
        }
    }
    o[k] = 0;
    *out_n = k;
    return o;
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
    for(struct css_rule_info *r=css_sheet_rules(ast,NULL);r;r=r->next)if(r->import_url) {
        struct css_import *im=calloc(1,sizeof *im);
        if(!im)return false;
        im->url=strdup(r->import_url);im->order=order;
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
    const char *source = css ? css : "", *source_base = base ? base : "", *source_media = media ? media : "";
    for(int i=0;d->live && i<d->css_cache.n;i++) {
        struct cached_css *c=d->css_cache.v[i];
        if(!c->done || c->body!=css || c->n!=n || strcmp(c->base,source_base))continue;
        sheet_t *ast=cached_sheet_ast(d,c);
        return ast && sheet_instance_imports(d,f,ast,order,media,scope);
    }
    for (struct css_sheet_reuse *c = d->css_reuse; c; c = c->next) {
        if (n != c->n || strcmp(source_base, c->base) || strcmp(source_media, c->media) || memcmp(source, c->css, n)) continue;
        f->sheet = css_sheet_instance(arena, c->sheet, order, scope);
        return true;
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
    if (!f->imports.n && d->css_reuse_count < UINT_MAX && n < SIZE_MAX && d->css_reuse_bytes <= SIZE_MAX - n) {
        struct css_sheet_reuse *c = ar_alloc(arena, sizeof *c);
        c->css = ar_strndup(arena, source, n); c->n = n;
        c->base = ar_strdup(arena, source_base); c->media = ar_strdup(arena, source_media);
        c->sheet = sh; c->next = d->css_reuse; d->css_reuse = c;
        d->css_reuse_count++; d->css_reuse_bytes += n;
    }
    return true;
}

static void add_sheet_ast(web_doc *d, const char *css, size_t n, const char *base, double order, const char *media, node_t *scope, sheet_t *native_ast) {
    struct sheet_walk *volatile active = calloc(1, sizeof(struct sheet_walk));
    if (!active) {
        web_js_console(d, 2, "Stylesheet import traversal allocation failed");
        return;
    }
    /* Inline sheets use their document URL for resolution, but are not an
       imported ancestor of every stylesheet fetched from that document URL. */
    for (int i = 0; d->live && i < d->css_cache.n; i++) {
        struct cached_css *c = d->css_cache.v[i];
        if (c->body == css && c->n == n) { active->url = c->url; active->base = c->base; break; }
    }
    arena_t *arena = d->live ? &d->cssmem : &d->mem;
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
        return;
    }
    if (native_ast) {
        if(!sheet_instance_imports(d,active,native_ast,order,media,scope))failed=true;
    } else if (!sheet_parse(d, active, css, n, base, order, media, scope)) failed = true;
    while (!failed && active) {
        struct sheet_walk *f = active;
        if (f->next == f->imports.n) {
            /* Postorder is CSS import source order. All members of this root
               sheet use its order key; the stable cascade sort then preserves
               imports before the parent, without a shrinking floating-point
               depth interval that can cross an earlier root stylesheet. */
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
        if (cached && cached->done) {
            if (cached->body) {
                struct sheet_walk *child = calloc(1, sizeof *child);
                if (!child) { failed = true; break; }
                child->parent = f; child->url = cached->url; child->base = cached->base;
                f->next++; free(im->url); free(im);
                active = child;
                if (!sheet_parse(d, child, cached->body, cached->n, cached->base, order, media, scope)) failed = true;
            } else { f->next++; free(im->url); free(im); }
            continue;
        }
        if (d->live && css_queued(d, im->url)) {
            f->next++; free(im->url); free(im);
            continue;
        }
        struct pending *p = calloc(1, sizeof *p);
        if (!p) { failed = true; break; }
        p->url = im->url; p->order = im->order; p->scope = scope;
        p->media = media && *media ? strdup(media) : NULL;
        if ((media && *media && !p->media) || !sheet_push(&d->pending_css, p)) {
            free(p->media); free(p); failed = true; break;
        }
        f->next++; free(im); /* URL ownership has moved to pending_css. */
    }
    sheet_walk_free(active);
    arena->trap = outer_trap;
    d->need_style = true;
    if (failed) web_js_console(d, 2, "Stylesheet import traversal allocation or index representation failed");
}
static void add_sheet(web_doc *d,const char *css,size_t n,const char *base,double order,const char *media,node_t *scope) {
    add_sheet_ast(d,css,n,base,order,media,scope,NULL);
}

void doc_add_stylesheet_text(web_doc *d, const char *css, size_t n, const char *base) {
    add_sheet(d, css, n, base, d->next_sheet_order++, NULL, NULL);
}

static char *doc_absolute_url(const char *base,const char *relative) {
    char *url=NULL;
    if(web_resolve_url_owned(base,relative,&url)==1)return url;
    free(url);return NULL;
}
static int add_image_url(web_doc *d,const char *abs,bool retry_failed,int preferred,bool image_upgrade) {
    if (preferred >= 0 && preferred < d->images.n) {
        struct web_image *im=d->images.v[preferred];
        if(im->image_upgrade==image_upgrade && !strcmp(im->url,abs))return preferred;
    }
    int failed_match = -1;
    for (int i = 0; i < d->images.n; i++) {
        struct web_image *im = d->images.v[i];
        if (im->image_upgrade!=image_upgrade || strcmp(im->url, abs)) continue;
        if (!im->done || !im->failed) return i;
        if (failed_match < 0) failed_match = i;
    }
    if (failed_match >= 0 && !retry_failed) return failed_match;
    if (d->images.n == INT_MAX) return -1;
    struct web_image *im = calloc(1, sizeof *im);
    if (!im) return -1;
    im->url = strdup(abs);
    im->image_upgrade = image_upgrade;
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
        char *data = data_url(abs, &n);
        web_image_loaded(d, idx, data, n);
        free(data);
    }
    return idx;
}
static int add_image_request(web_doc *d, const char *rel, bool retry_failed, int preferred,bool image_upgrade) {
    if(!rel || !*rel)return -1;
    while(is_space((unsigned char)*rel))rel++;
    char *abs=doc_absolute_url(d->base,rel);if(!abs)return -1;
    int result=add_image_url(d,abs,retry_failed,preferred,image_upgrade);free(abs);return result;
}
static int add_image(web_doc *d, const char *rel) { return add_image_request(d, rel, false, -1,true); }

/* the srcset candidate for a 1x display: an "1x" one, else the first */
static char *srcset_pick(const char *ss) {
    const char *best = NULL, *best_e = NULL, *first = NULL, *first_e = NULL;
    float best_w = 0;
    const char *p = ss;
    while (*p) {
        while (is_space((unsigned char)*p) || *p == ',') p++;
        if (!*p) break;
        const char *u = p;
        while (*p && !is_space((unsigned char)*p)) p++;
        const char *ue = p;
        if (ue > u && ue[-1] == ',') ue--;
        const char *desc = p;
        while (*p && *p != ',') p++;
        if (!first) first = u, first_e = ue;
        float v = (float)atof(desc);
        while (is_space((unsigned char)*desc)) desc++;
        const char *unit = desc;
        while (*unit && (isdigit((unsigned char)*unit) || *unit == '.')) unit++;
        if (*unit == 'x' && v == 1) {
            best = u, best_e = ue;
            break;
        }
        if (*unit == 'w' && v >= 600 && (!best || v < best_w)) best = u, best_e = ue, best_w = v;
    }
    if (!best) best = first, best_e = first_e;
    if (!best) return NULL;
    return strndup(best, (size_t)(best_e - best));
}

static bool placeholder_src(const char *s) {
    return !s || !*s || (!strncasecmp(s, "data:image/gif", 14) && strlen(s) < 200) || (!strncasecmp(s, "data:image/svg", 14) && strlen(s) < 200 && strstr(s, "%3C/svg") == NULL);
}

/* Allocation lists keep their lifetime owner even after DOM adoption. The
 * family contains at most 64 auxiliary documents (doc_inert's existing cap).
 * Resource users must enumerate logical owners, not only d's own arena.
 * previous must be NULL or a node returned by this iterator. */
node_t *doc_image_node_next(web_doc *d, node_t *previous) {
    if (!d || d->inert) return NULL;
    web_doc *family = d->dom_family ? d->dom_family : d;
    web_doc *allocation = previous ? previous->allocation_doc : family;
    node_t *n = previous ? previous->owned_next : allocation->owned_nodes;
    while (allocation) {
        for (; n; n = n->owned_next)
            if (n->owner == d && n->type == N_ELEM && !n->foreign &&
                (n->tag == T_img || n->tag == T_input)) return n;
        allocation = allocation == family ? family->dom_docs : allocation->dom_next;
        n = allocation ? allocation->owned_nodes : NULL;
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
    const char *lazy = node_attr(n, "data-src");
    if (!lazy) lazy = node_attr(n, "data-lazy-src");
    if (!lazy) lazy = node_attr(n, "data-original");
    char *pick = NULL;
    pick=web_image_source_pick(d,n,placeholder_src(src)?NULL:src);
    if(pick)src=pick;
    else if (placeholder_src(src) && lazy) src = lazy;
    else if (placeholder_src(src) && node_attr(n, "data-srcset")) src = pick = srcset_pick(node_attr(n, "data-srcset"));
    bool has_source = src != NULL || ss != NULL;
    /* Keep responsive/CORS images outside this ordinary-image upgrade path.
       In particular, URL deduplication must not promote a blocked imageset. */
    bool image_upgrade=!ss && !node_attr(n,"data-srcset") && !node_attr(n,"crossorigin") &&
        !(n->parent && !n->parent->foreign && n->parent->tag==T_picture);
    int requested = add_image_request(d, src, n->image_invalidated,
        n->image_initialized && !n->image_invalidated ? n->image_request : -1,image_upgrade);
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
    if (n->tag == T_select) { web_select_sync(d, n, !n->control_ready); return; }
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
static bool link_sheet_applies(node_t *n) {
    if (cssom_owner_disabled(n) || !media_wanted(node_attr(n,"media"))) return false;
    const char *rel=node_attr(n,"rel");
    while (rel && *rel) {
        while(is_space((unsigned char)*rel))rel++;
        const char *end=rel;while(*end&&!is_space((unsigned char)*end))end++;
        if(end-rel==9&&strn_ieq(rel,"alternate",9))return false;
        rel=end;
    }
    return true;
}
/* Borrow only a completed native resource, never a fabricated empty sheet.
   URL/data scratch is caller-owned; cache text/base survives the document. */
int doc_cssom_link_source(web_doc *d,node_t *n,struct cssom_link_source *source) {
    memset(source,0,sizeof *source);
    if(!d||!n||n->owner!=d||!d->live||d->inert||!link_stylesheet(n)||
        doc_node_root(n,true)!=d->root||node_ancestor(n,T_template))return CSSOM_INACTIVE;
    const char *href=node_attr(n,"href");if(!href||!*href)return CSSOM_INACTIVE;
    int resolved=web_resolve_url_owned(d->base,href,&source->url);
    if(resolved!=1)return resolved<0?CSSOM_OOM:CSSOM_INACTIVE;
    if(!strncasecmp(source->url,"data:",5)) {
        source->owned_text=data_url(source->url,&source->length);
        if(!source->owned_text){bool malformed=!strchr(source->url,',');free(source->url);memset(source,0,sizeof *source);return malformed?CSSOM_INACTIVE:CSSOM_OOM;}
        source->text=source->owned_text;source->base=d->base;return CSSOM_OK;
    }
    struct cached_css *cached=cached_css(d,source->url);
    if(!cached||!cached->done||!cached->body){free(source->url);memset(source,0,sizeof *source);return CSSOM_INACTIVE;}
    source->text=cached->body;source->length=cached->n;source->base=cached->base;return CSSOM_OK;
}
bool doc_link_load_current(web_doc *d, node_t *n, uint64_t generation) {
    if (!d || !d->live || d->inert || !n || n->owner != d ||
        doc_node_root(n,true) != d->root || n->stylesheet_owner != d || n->stylesheet_generation != generation ||
        !n->stylesheet_url || !link_stylesheet(n)) return false;
    const char *href = node_attr(n,"href");
    char *url = href && *href ? doc_absolute_url(d->base,href) : NULL;
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
        switch (c->tag) {
        case T_base: {
            if (scope) break;
            const char *h = node_attr(c, "href");
            if (h && !d->base_seen) { /* only the first <base> counts */
                char *abs=doc_absolute_url(web_effective_url(d),h);
                if(abs)snprintf(d->base,sizeof d->base,"%s",abs);
                free(abs);
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
                char *abs=doc_absolute_url(d->base,*tmp?tmp:d->url);
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
            char *abs=doc_absolute_url(d->base,href);if(!abs)break;
            if (d->live && (c->stylesheet_owner != d || !c->stylesheet_url || strcmp(c->stylesheet_url,abs))) {
                c->stylesheet_url = ar_strdup(&d->mem,abs);
                c->stylesheet_owner = d;
                c->stylesheet_generation++; c->stylesheet_notified = false;
            }
            struct cached_css *cached = d->live ? cached_css(d, abs) : NULL;
            if (cached && cached->done) {
                if (cached->body && link_sheet_applies(c)) {
                    if(c->cssom_current) {
                        int error;struct cssom_sheet *sheet=cssom_style_sheet(d,c,&error);
                        if(sheet)add_sheet_ast(d,sheet->source,sheet->length,sheet->base,d->next_sheet_order++,media,scope,sheet->ast);
                        else if(error==CSSOM_OOM)web_js_console(d,2,"Native link style sheet allocation failed");
                    }else add_sheet(d,cached->body,cached->n,cached->base,d->next_sheet_order++,media,scope);
                }
                link_stylesheet_complete(d,c,!cached->body);
                free(abs);
                break;
            }
            if(cssom_owner_disabled(c)){free(abs);break;}
            if (d->live && css_queued(d, abs)) {free(abs);break;}
            struct pending *pd = calloc(1, sizeof *pd);
            if(!pd){free(abs);break;}
            pd->url = abs;
            pd->order = d->next_sheet_order++;
            pd->media = media && *media ? strdup(media) : NULL;
            pd->scope = scope;
            if (!strncasecmp(abs, "data:", 5)) {
                size_t dn;
                char *css = data_url(abs, &dn);
                if (css && link_sheet_applies(c)) {
                    if(c->cssom_current) {
                        int error;struct cssom_sheet *sheet=cssom_style_sheet(d,c,&error);
                        if(sheet)add_sheet_ast(d,sheet->source,sheet->length,sheet->base,pd->order,pd->media,scope,sheet->ast);
                        else if(error==CSSOM_OOM)web_js_console(d,2,"Native link style sheet allocation failed");
                    }else add_sheet(d,css,dn,d->base,pd->order,pd->media,scope);
                }
                link_stylesheet_complete(d,c,!css);
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
            if (c->style_disabled || (type && *type && !str_ieq(type, "text/css")) ||
                !media_wanted(media) || node_ancestor(c, T_template)) break;
            /* Keep the existing per-snapshot native AST reuse for untouched
               sheets. A real CSSOM handle lazily promotes only its owner to
               persistent parser storage, not every repeated component style. */
            if (d->live && c->cssom_current) {
                int error; struct cssom_sheet *sheet=cssom_style_sheet(d,c,&error);
                if (sheet) add_sheet_ast(d,sheet->source,sheet->length,sheet->base,d->next_sheet_order++,media,scope,sheet->ast);
                else if (error==CSSOM_OOM) web_js_console(d,2,"Native style sheet allocation failed");
            } else {
                sbuf b = {0}; text_of(c,&b);
                add_sheet(d,b.p ? b.p : "",b.n,d->base,d->next_sheet_order++,media,scope); sb_free(&b);
            }
            break;
        }
        case T_img: doc_image_sync(d, c); break;
        case T_input: {
            const char *t = node_attr(c, "type");
            if (t && str_ieq(t, "image")) c->image = add_image(d, node_attr(c, "src"));
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
    /* Adopted sheets follow tree-owned sheets in this scope. Their persistent
       native AST is instantiated, never reparsed for each component root. */
    if (n == d->root || n->shadow_host) {
        for (uint32_t i = 0; i < n->adopted_count; i++) {
            struct cssom_sheet *s = n->adopted_sheets[i];
            if (!s->disabled) add_sheet_ast(d, s->source, s->length, s->base,
                d->next_sheet_order++, s->media, scope, s->ast);
        }
    }
}

/* ---------------------------------------------------------------- documents */
size_t doc_dom_remaining(web_doc *d) {
    /* Accounting headroom, not a promise of physically available RAM. */
    if (!d || d->control_bytes > SIZE_MAX - d->mem.allocated) return 0;
    return SIZE_MAX - d->mem.allocated - d->control_bytes;
}
void doc_dom_budget(web_doc *d) {
    if (d) d->mem.limit = 0; /* arena zero: actual allocator/overflow only */
}
web_doc *doc_inert(web_doc *family, const char *html, size_t n, const char *url) {
    if (!family || n == SIZE_MAX || (n && !html)) return NULL;
    family = family->dom_family ? family->dom_family : family;
    web_doc *d = calloc(1, sizeof *d);
    if (!d) return NULL;
    d->inert = true; d->dom_family = family;
    d->url = strdup(url && *url ? url : "about:blank");
    if (!d->url) { free(d); return NULL; }
    snprintf(d->base, sizeof d->base, "%s", d->url);
    family->dom_family = family;
    d->dom_next = family->dom_docs; family->dom_docs = d;
    doc_dom_budget(d);
    web_doc *previous = d->dom_next;
    d->root = html_parse(d, html ? html : "", n, "utf-8");
    if (!d->root) {
        /* No wrappers have escaped: discard every nested template document too. */
        while (family->dom_docs != previous) {
            web_doc *bad = family->dom_docs;
            family->dom_docs = bad->dom_next; bad->dom_next = NULL;
            web_free(bad);
        }
        return NULL;
    }
    d->resources_dirty = true;
    return d;
}
web_doc *web_parse(const char *html, size_t len, const char *url, const char *charset) {
    web_doc *d = calloc(1, sizeof *d);
    if (!d) return NULL;
    d->url = strdup(url && *url ? url : "about:blank");
    snprintf(d->base, sizeof d->base, "%s", d->url);
    d->root = html_parse(d, html ? html : "", html ? len : 0, charset);
    d->need_style = true;
    if (d->root) scan(d, d->root, NULL);
    return d;
}

web_doc *web_live(const char *html, size_t len, const char *url, const char *charset,
                  const struct web_host *host) {
    return web_live_child(html,len,url,charset,host,NULL,NULL,NULL);
}
web_doc *web_live_child(const char *html, size_t len, const char *url, const char *charset,
                       const struct web_host *host, web_doc *parent,node_t *frame,web_doc *inherited_origin) {
    if (len == SIZE_MAX || (len && !html)) return NULL;
    web_doc *d = calloc(1, sizeof *d);
    if (!d) return NULL;
    d->live = true;
    d->frame_parent=parent;d->frame_element=frame;
    if(parent && frame){
        struct web_frame *f=web_frame_find(parent,frame);d->window_token=f?f->window_token:NULL;
        d->sandbox_flags=f?f->pending_sandbox_flags:parent->sandbox_flags;
        if(f && f->sandbox_initial && d->sandbox_flags)d->sandbox_flags|=SB_SCRIPTS;
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
    if (!d->url) { free((void *)d->inherited_url);free(d); return NULL; }
    snprintf(d->base, sizeof d->base, "%s", web_effective_url(d));
    d->parser = html_begin(d, html ? html : "", html ? len : 0, charset, !(d->sandbox_flags&SB_SCRIPTS));
    if (!d->parser) { web_free(d); return NULL; }
    /* The restricted initial blank exposes only a native WindowProxy token.
       Do not create a QuickJS realm whose raw Function/eval could bypass
       disabled task dispatch when borrowed by the embedding document. */
    if(!(d->sandbox_flags&SB_SCRIPTS))web_js_start(d, host);
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

static node_t *first_base(node_t *n, uint64_t *visits) {
    for (node_t *c = n ? n->first : NULL; c; c = c->next) {
        (*visits)++;
        if (c->type != N_ELEM) continue;
        if (!c->foreign && c->tag == T_template) continue;
        if (!c->foreign && c->tag == T_base && node_attr(c, "href")) return c;
        node_t *found = first_base(c, visits);
        if (found) return found;
    }
    return NULL;
}

/* Synchronous DOM/URL reads need the current tree, not a new CSS snapshot.
   Leave resources_dirty set: task completion or a layout read still scans
   resources, applies stylesheet changes, and initializes pending controls. */
void doc_sync_tree(web_doc *d) {
    if (!d) return;
    struct web_profile *profile = &(d->dom_family ? d->dom_family : d)->profile;
    uint64_t start = d->profile_enabled ? uptime_ms() : 0;
    profile->metadata_syncs++;
    d->html = d->head = d->body = NULL;
    for (node_t *n = d->root ? d->root->first : NULL; n; n = n->next) {
        profile->metadata_visits++;
        if (n->type == N_ELEM) { d->html = n; break; }
    }
    for (node_t *n = d->html ? d->html->first : NULL; n; n = n->next) {
        profile->metadata_visits++;
        if (!d->head && !n->foreign && n->tag == T_head) d->head = n;
        if (!d->body && !n->foreign && (n->tag == T_body || n->tag == T_frameset)) d->body = n;
    }
    snprintf(d->base, sizeof d->base, "%s", web_effective_url(d));
    node_t *base = first_base(d->root, &profile->metadata_visits);
    if (base) {
        char *url=doc_absolute_url(web_effective_url(d),node_attr(base,"href"));
        if(url)snprintf(d->base,sizeof d->base,"%s",url);
        free(url);
    }
    if (d->profile_enabled) profile->metadata_ms += uptime_ms() - start;
}

void doc_rescan(web_doc *d) {
    if (d && d->native_cancelled) return;
    if (d && d->inert && d->resources_dirty) {
        d->resources_dirty = false; d->html = d->head = d->body = NULL;
        snprintf(d->base, sizeof d->base, "%s", d->url);
        for (node_t *n = d->root ? d->root->first : NULL; n; n = n->next)
            if (n->type == N_ELEM) { d->html = n; break; }
        for (node_t *n = d->html ? d->html->first : NULL; n; n = n->next) {
            if (!d->head && n->tag == T_head) d->head = n;
            if (!d->body && (n->tag == T_body || n->tag == T_frameset)) d->body = n;
        }
        for (node_t *n = d->head ? d->head->first : NULL; n; n = n->next)
            if (n->tag == T_base && node_attr(n, "href")) {
                char *url=doc_absolute_url(d->url,node_attr(n,"href"));
                if(url)snprintf(d->base,sizeof d->base,"%s",url);
                free(url);
                break;
            }
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
    d->profile.rescans++;
    d->resources_dirty = false;
    d->images_dirty = false;
    css_styling_free(&d->sty);
    ar_free(&d->cssmem);
    d->css_reuse = NULL; d->css_reuse_count = 0; d->css_reuse_bytes = 0;
    free_pending(d);
    d->next_sheet_order = 0;
    d->base_seen = false;
    d->scan_title_seen = false;
    d->refresh_url = NULL;
    d->refresh_delay = 0;
    snprintf(d->base, sizeof d->base, "%s", web_effective_url(d));
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
        d->css_reuse = NULL; d->css_reuse_count = 0; d->css_reuse_bytes = 0;
        free_pending(d);
    }
    d->mem.trap = old_mem;
    d->cssmem.trap = old_css;
    d->dirty = d->need_style = true;
    if (d->profile_enabled) d->profile.rescan_ms += uptime_ms() - profile_start;
}

void doc_css_loaded(web_doc *d, const char *url, const char *final_url, const char *css, size_t n) {
    if (!d || !url) return;
    struct cached_css *c = cached_css(d, url);
    if (!c) {
        c = calloc(1, sizeof *c);
        if (!c) return;
        c->url = strdup(url);
        c->base = strdup(final_url && *final_url ? final_url : url);
        if (!c->url || !c->base) { free(c->url); free(c->base); free(c); return; }
        pv_push(&d->css_cache, c);
    }
    if (c->done) return;
    c->done = true;
    if (css && n < SIZE_MAX && d->css_bytes <= SIZE_MAX - n) {
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
    web_avmedia_background(now);
    web_worker_background();
}
int64_t web_media_background_deadline(uint64_t now) {
    int64_t media = web_avmedia_background_deadline(now), worker = web_worker_background_deadline(now);
    return media < 0 ? worker : worker < 0 ? media : MIN(media,worker);
}
void web_tick(web_doc *d, uint64_t now) {
    web_media_background(now);
    if (!d || !d->live) return;
    doc_rescan(d);
    web_avmedia_tick(d, now);
    css_motion_tick(d,now);
    web_js_tick(d, now);
    doc_rescan(d);
    /* The child can complete while author JS runs. Refill before GUI paint,
       not one complete browser turn later. Both calls retain their native
       2 ms / 8 packet bounds; there are exactly two opportunities per tick. */
    web_avmedia_tick(d, uptime_ms());
    web_js_frames_sync(d);
    web_frames_tick(d,now);
}
int64_t web_deadline(web_doc *d) {
    uint64_t now = uptime_ms();
    int64_t background = web_media_background_deadline(now);
    if (!d || !d->live) return background;
    int64_t js = web_js_deadline(d), media = web_avmedia_deadline(d, now);
    int64_t deadline = js < 0 ? media : media < 0 ? js : MIN(js, media);
    int64_t motion=css_motion_deadline(d,now);
    if(motion>=0 && (deadline<0 || motion<deadline))deadline=motion;
    int64_t frames=web_frames_deadline(d);
    if(frames>=0 && (deadline<0 || frames<deadline))deadline=frames;
    return deadline < 0 ? background : background < 0 ? deadline : MIN(deadline, background);
}
void web_resource_loaded(web_doc *d, uint64_t id, const struct web_response *r) {
    if (d && d->live) {web_js_loaded(d, id, r);web_frames_loaded(d,id,r);}
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
}
bool web_dirty(web_doc *d) {
    if (!d) return false;
    bool dirty = d->dirty;
    d->dirty = false;
    return dirty;
}
bool web_paint_dirty(web_doc *d) {
    if (!d) return false;
    bool dirty = d->paint_dirty;
    d->paint_dirty = false;
    return dirty;
}
bool web_script_running(web_doc *d) { return d && d->live && web_js_running(d); }
bool web_dispatch(web_doc *d, web_node *target, const struct web_event *e) {
    return !d || !d->live || web_js_dispatch(d, target, e);
}

static void free_values(node_t *n) {
    free(n->container_names);n->container_names=NULL;
    css_node_style_free(n);
    cssom_style_free(n);
    web_paint_debug_node_free(n);
    css_animation_free(n);
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
    f->loading=false;f->done=true;f->failed=!f->face;
    /* Both real decode success and failure must recascade: success changes
       metrics/paint; failure makes the following authored src eligible. */
    d->need_style=d->dirty=true;d->layout_valid=false;d->paint_dirty=true;
    invalidate_layout_tree(d->root_box);
}

void web_free(web_doc *d) {
    if (!d) return;
    doc_active_cancel(d);
    /* Adopted DOM nodes may be allocation-owned by a helper document. Retire
       all published box/scope borrows before freeing that family of owners. */
    boxes_discard(d);
    css_styles_release(d);
    css_styling_free(&d->sty);
    web_dialog_free(d);
    web_avmedia_free(d);
    web_frames_free(d);
    web_js_free(d);
    css_motion_doc_free(d);
    while(d->fonts) {
        struct web_font_resource *f=d->fonts;d->fonts=f->next;
        font_close(f->face);free(f->url);free(f);
    }
    web_form_validation_free(d);
    while (d->dom_docs) {
        web_doc *child = d->dom_docs;
        d->dom_docs = child->dom_next; child->dom_next = NULL;
        web_free(child);
    }
    html_finish(d->parser);
    d->parser = NULL;
    if (d->owned_nodes) {
        for (node_t *n = d->owned_nodes; n; n = n->owned_next) {
            free(n->container_names);n->container_names=NULL;
            css_node_style_free(n);
            cssom_style_free(n);
            web_paint_debug_node_free(n);
            css_animation_free(n);
            web_canvas_free(n);
            web_input_files_release(n);
            web_face_release(n);
            free(n->value);
            free(n->input_edit);
        }
    } else if (d->root) free_values(d->root);
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
        if (im->scaled) image_free(im->scaled);
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
    free(d->url);
    free(d->resolved_link);
    free(d->find_text);
    free((void *)d->inherited_url);
    free(d);
}

const char *web_title(web_doc *d) { return d->title ? d->title : ""; }
const char *web_url(web_doc *d) { return d->url; }
#include "frame_address.h"
bool web_set_url(web_doc *d, const char *url) {
    if (!d || !url) return false;
    char *copy = strdup(url);
    if (!copy) return false;
    free(d->url); d->url = copy;
    if (!d->base_seen) snprintf(d->base, sizeof d->base, "%s", url);
    return true;
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
        add_sheet(d, css, n, p->url, p->order, p->media, p->scope);
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
    return !im->done && !im->failed;
}

static void image_decode_service(void *context) { web_native_checkpoint(context); }
void web_image_loaded(web_doc *d, int i, const void *data, size_t n) {
    if (i < 0 || i >= d->images.n) return;
    struct web_image *im = d->images.v[i];
    if (im->done) return;
    im->done = true;
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
        if (node->image != i) continue;
        box_t *box = node->box;
        if (!box || box->node != node || box->kind != B_ATOMIC || box->atomic != AT_IMG ||
            doc_node_root(node, true) != d->root) continue;
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
    d->paint_dirty = true;
    if (intrinsic_changed) { d->layout_valid = false; d->dirty = true; }
}

/* CSS background images are known once styles are: add them to the document's images */
static void bg_images(web_doc *d, style_t *st) {
    if (st && st->bg_image && !st->bg_img) {
        int i = add_image(d, st->bg_image);
        st->bg_img = i >= 0 ? i + 1 : 0;
    }
    if (st && st->mask_image && !st->mask_img) {
        int i = add_image(d, st->mask_image);
        st->mask_img = i >= 0 ? i + 1 : 0;
    }
}

static void register_bg(web_doc *d, node_t *n) {
    for (node_t *c = n->first; c; c = c->next) {
        if (c->type != N_ELEM || !c->style) continue;
        bg_images(d, c->style);
        bg_images(d, c->style->before);
        bg_images(d, c->style->after);
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

int web_layout(web_doc *d, int width, int height) {
    unsigned container_pass=0;
    if (d->native_cancelled) return d->doc_h;
    doc_shadow_flush(d);
    if (width < 1) width = 1;
    if (height < 1) height = 1;
    web_dialog_sync(d);
    if (d->resources_dirty || d->images_dirty) doc_rescan(d);
    if (d->native_cancelled) goto cancelled;
    /* Geometry reads may flush layout repeatedly within one script. Reuse the
       result until DOM/style, viewport, or image intrinsic dimensions change. */
    if (d->layout_valid && !d->need_style && !d->need_boxes && d->root_box &&
        d->width == width && d->height == height) return d->doc_h;
    /* No independent patch may publish a previously committed video span
       while geometry is incomplete. Native PCM/transport supply continues. */
    web_avmedia_layout_begin(d);
    if(d->width!=width||d->height!=height){
        invalidate_layout_tree(d->root_box);
        d->width=width;d->height=height;
        for(node_t *image=doc_image_node_next(d,NULL);image;image=doc_image_node_next(d,image))doc_image_sync(d,image);
    }
reflow:
    if (d->need_style || d->styled_w != width || d->styled_h != height || !d->root_box) {
        css_cascade(d, width, height);
        if (d->native_cancelled) goto cancelled;
        if (d->root) register_bg(d, d->root);
        if (!d->root_box || d->style_boxes_changed || d->need_boxes) {
            boxes_discard(d);
            css_styles_release(d);
            d->bmem.trap=d->smem.trap;
            boxes_build(d, &d->bmem);
            d->bmem.trap=NULL;
        } else {
            /* Topology is unchanged. Update computed-style borrows before
               reclaiming old element arenas; dirty boxes drive local layout. */
            boxes_restyle(d);
            css_styles_release(d);
        }
        if (d->native_cancelled) goto cancelled;
        d->need_style = false;
        d->need_boxes = false;
    } else if(d->need_boxes) {
        /* Computed styles remain owned by smem. Only text boxes/anonymous
           styles borrow bmem, so old box ownership can be reclaimed safely. */
        boxes_discard(d);
        d->bmem.trap=d->smem.trap;
        boxes_build(d,&d->bmem);
        d->bmem.trap=NULL;
        if (d->native_cancelled) goto cancelled;
        d->need_boxes=false;
    }
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
        return d->doc_h;
    }
    d->layout_revision++;
    d->layout_valid = true;
    layout_clamp_trace_report(d);
    if (d->find_text && *d->find_text && !d->find_revealing) web_find(d, d->find_text, 0);
    return d->doc_h;
cancelled:
    /* Never paint/hit-test a half-built tree or label it a valid layout. */
    d->bmem.trap = NULL;
    boxes_discard(d);
    css_node_style_unpublish(d->root);
    css_styles_release(d);
    d->layout_valid = false; d->dirty = false;
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

bool doc_element_scroll(web_doc *d, node_t *n, double x, double y) {
    float width, height;
    if (!doc_element_scroll_metrics(d, n, &width, &height) || !box_element_scrollable(n->box) ||
        n == d->html) return false;
    box_t *b = n->box;
    double max_x = (double)width - b->w - b->p[1] - b->p[3];
    double max_y = (double)height - b->h - b->p[0] - b->p[2];
    x = !isfinite(x) || !isfinite(max_x) || x < 0 || max_x <= 0 ? 0 : x > max_x ? max_x : x;
    y = !isfinite(y) || !isfinite(max_y) || y < 0 || max_y <= 0 ? 0 : y > max_y ? max_y : y;
    if (n->scroll_x == x && n->scroll_y == y) return false;
    n->scroll_x = x;
    n->scroll_y = y;
    d->paint_dirty = true; /* scrolling changes presentation, not CSS/layout */
    return true;
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

static bool scroll_target_rect(node_t *n, double r[4]) {
    box_t *b = n->box;
    if (!b || !b->st || b->st->display == D_NONE) return false;
    if (b->kind != B_INLINE) {
        r[0] = box_visual_x(b) - b->p[3] - b->b[3];
        r[1] = box_visual_y(b) - b->p[0] - b->b[0];
        r[2] = b->w + b->p[1] + b->p[3] + b->b[1] + b->b[3];
        r[3] = b->h + b->p[0] + b->p[2] + b->b[0] + b->b[2];
    } else {
        /* Native inline fragments are deco spans in their formatting block,
           not a fabricated parentNode relationship or the unpositioned inline
           box. Union this element's painted spans in its first native block. */
        box_t *a = n->anchor_block;
        if (!a) return false;
        double x = box_visual_x(a), y = box_visual_y(a) + a->content_dy;
        if (box_element_scrollable(a)) { x -= a->node->scroll_x; y -= a->node->scroll_y; }
        bool found = false;
        double right = 0, bottom = 0;
        for (int i = 0; i < a->ndecos; i++) {
            const struct deco *span = &a->decos[i];
            if (span->node != n) continue;
            double l = x + span->x, t = y + span->y, rr = l + span->w, bb = t + span->h;
            if (!found) { r[0] = l; r[1] = t; right = rr; bottom = bb; found = true; }
            else { if (l < r[0]) r[0] = l; if (t < r[1]) r[1] = t;
                   if (rr > right) right = rr; if (bb > bottom) bottom = bb; }
        }
        if (!found) return false;
        r[2] = right - r[0]; r[3] = bottom - r[1];
    }
    return isfinite(r[0]) && isfinite(r[1]) && isfinite(r[2]) && isfinite(r[3]);
}

int doc_element_scroll_into_view(web_doc *d, node_t *n, int block, int inline_, bool nearest_only,
                                double *x, double *y, pvec *changed) {
    if (!d || !n || !x || !y || !changed || changed->v || changed->n || changed->cap ||
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
    if (count) {
        if ((size_t)count > SIZE_MAX / sizeof *changed->v) return -3;
        changed->v = malloc((size_t)count * sizeof *changed->v);
        if (!changed->v) return -1;
        changed->cap = count;
    }
    for (box_t *a = first; a; a = a->cb) {
        if (!box_element_scrollable(a)) continue;
        if (!scroll_target_rect(n, target)) return 1;
        double left = box_visual_x(a) - a->p[3], top = box_visual_y(a) - a->p[0];
        double dx = scroll_align_delta(target[0], target[0] + target[2], left, a->w + a->p[1] + a->p[3], inline_);
        double dy = scroll_align_delta(target[1], target[1] + target[3], top, a->h + a->p[0] + a->p[2], block);
        if (doc_element_scroll(d, a->node, a->node->scroll_x + dx, a->node->scroll_y + dy))
            changed->v[changed->n++] = a->node;
        if (nearest_only) return 1;
    }
    if (!fixed && scroll_target_rect(n, target)) {
        /* This document's real viewport only. There is no traversal through a
           fabricated frame parent or access to a different-origin document. */
        *x += scroll_align_delta(target[0], target[0] + target[2], *x, d->width, inline_);
        *y += scroll_align_delta(target[1], target[1] + target[3], *y, d->height, block);
    }
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
    if(web_resolve_url_owned(d->base,h,&resolved)!=1)return NULL;
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
    d->hover_target=target;
    if(d->live){
        d->dirty=d->need_style=true;
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
    d->active_target=target;
    if(d->live){
        d->dirty=d->need_style=true;
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
   Count a scalar outside the BMP as two code units; never count bytes as units. */
static uint32_t control_decode(const char *text, size_t *byte) {
    const unsigned char *p = (const unsigned char *)text + *byte;
    uint32_t cp = *p; size_t n = 1;
    if (cp >= 0xc2 && cp <= 0xf4) {
        size_t want = cp < 0xe0 ? 2 : cp < 0xf0 ? 3 : 4;
        uint32_t out = cp & (want == 2 ? 31 : want == 3 ? 15 : 7);
        size_t j = 1;
        while (j < want && p[j] && (p[j] & 0xc0) == 0x80) { out = (out << 6) | (p[j] & 63); j++; }
        if (j == want && out <= 0x10ffff && out >= (want == 2 ? 0x80u : want == 3 ? 0x800u : 0x10000u)) { cp = out; n = want; }
    }
    *byte += n; return cp;
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
    if (changed) { d->dirty = true; web_js_selection_changed(d, n); }
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
                     node_attr(n, "tabindex") || (editable && !str_ieq(editable, "false"));
    return focusable ? n : NULL;
}
node_t *web_focus_candidate(web_doc *d, node_t *n) { return focus_area(d,n); }
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
    d->focus = n;
    d->caret = 0;
    if (n) doc_control_caret(d, n);
    d->dirty = d->need_style = true;
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

static int web_key_local(web_doc *d, const struct gui_event *e) {
    if(!d||!e||!d->live||d->inert)return 0;
    node_t *n = d->focus;
    /* Range's existing arrow-step UI is numeric, never text replacement. */
    bool range_step=n&&n->owner==d&&n->type==N_ELEM&&!n->foreign&&n->tag==T_input&&
        web_input_type(n)==WEB_INPUT_RANGE&&(e->key==NKEY_UP||e->key==NKEY_DOWN)&&
        doc_node_root(n,true)==d->root&&!web_control_disabled(n)&&!web_dialog_inert(d,n);
    if (!control_edit_target(d,n,false)&&!range_step) return 0;
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
    return node_ancestor(n, T_form);
}

int web_key(web_doc *d,const struct gui_event *event) {
    web_doc *root=d;
    while(d && d->live && !d->inert && web_frame_element(d->focus)) {
        struct web_frame *f=web_frame_find(d,d->focus);if(!f || !f->document || f->detached)break;
        d=f->document;
    }
    int result=web_key_local(d,event);
    if(result)for(web_doc *p=d;p && p!=root;p=p->frame_parent)if(p->frame_parent)p->frame_parent->dirty=true;
    return result;
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
    if (scope != keep && web_radio_same_group(d, scope, keep)) { doc_control_init(d, scope); scope->checked = false; }
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
    d->dirty = d->need_style = true;
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
