/* The public API (web.h): documents and their resources (stylesheets, images, data: URLs), the
   style/box/layout pipeline, and forms. */
#include <stdio.h>
#include <ctype.h>
#include "nocturne.h"
#include "webi.h"

struct pending {
    char *url;
    double order;
    char *media; /* the <link media> the sheet is wrapped in, or NULL */
};

struct cached_css {
    char *url, *base, *body;
    size_t n;
    bool done;
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

static void add_sheet(web_doc *d, const char *css, size_t n, const char *base, double order, const char *media) {
    if (d->css_depth >= 16) return;
    d->css_depth++;
    arena_t *arena = d->live ? &d->cssmem : &d->mem;
    pvec imports = {0};
    sheet_t *sh;
    if (media && *media) {
        sbuf w = {0};
        sb_puts(&w, "@media ");
        sb_puts(&w, media);
        sb_puts(&w, "{\n");
        sb_put(&w, css, n);
        sb_puts(&w, "\n}");
        sh = css_parse_sheet(arena, w.p, w.n, base, order, &imports);
        sb_free(&w);
    } else sh = css_parse_sheet(arena, css, n, base, order, &imports);
    pv_push(&d->sty.sheets, sh);
    for (int i = 0; i < imports.n; i++) {
        struct css_import *im = imports.v[i];
        struct cached_css *cached = d->live ? cached_css(d, im->url) : NULL;
        if (cached && cached->done) {
            if (cached->body) add_sheet(d, cached->body, cached->n, cached->base, im->order, media);
            free(im->url);
            free(im);
            continue;
        }
        if (d->live && css_queued(d, im->url)) {
            free(im->url);
            free(im);
            continue;
        }
        if (d->live && d->css_cache.n >= 128) {
            free(im->url);
            free(im);
            continue;
        }
        if (d->pending_css.n < 64) {
            struct pending *p = calloc(1, sizeof *p);
            p->url = im->url;
            p->order = im->order;
            p->media = media && *media ? strdup(media) : NULL;
            pv_push(&d->pending_css, p);
        } else free(im->url);
        free(im);
    }
    pv_free(&imports);
    d->need_style = true;
    d->css_depth--;
}

void doc_add_stylesheet_text(web_doc *d, const char *css, size_t n, const char *base) {
    add_sheet(d, css, n, base, d->next_sheet_order++, NULL);
}

static int add_image_request(web_doc *d, const char *rel, bool retry_failed, int preferred) {
    char abs[2048];
    if (!rel || !*rel) return -1;
    while (is_space((unsigned char)*rel)) rel++;
    if (!url_resolve(d->base, rel, abs, sizeof abs)) return -1;
    if (preferred >= 0 && preferred < d->images.n && !strcmp(((struct web_image *)d->images.v[preferred])->url, abs)) return preferred;
    int failed_match = -1;
    for (int i = 0; i < d->images.n; i++) {
        struct web_image *im = d->images.v[i];
        if (strcmp(im->url, abs)) continue;
        if (!im->done || !im->failed) return i;
        if (failed_match < 0) failed_match = i;
    }
    if (failed_match >= 0 && !retry_failed) return failed_match;
    if (d->images.n >= 2000) return -1;
    struct web_image *im = calloc(1, sizeof *im);
    if (!im) return -1;
    im->url = strdup(abs);
    if (!im->url) { free(im); return -1; }
    if (d->images.n == d->images.cap) {
        int cap = d->images.cap ? d->images.cap * 2 : 16;
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
static int add_image(web_doc *d, const char *rel) { return add_image_request(d, rel, false, -1); }

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
    /* Retain the existing renderer's source-selection policy; responsive
       density/sizes/picture selection is a separate, still-limited subsystem. */
    if (placeholder_src(src) && lazy) src = lazy;
    else if (placeholder_src(src) && ss) src = pick = srcset_pick(ss);
    else if (placeholder_src(src) && node_attr(n, "data-srcset")) src = pick = srcset_pick(node_attr(n, "data-srcset"));
    bool has_source = src != NULL || ss != NULL;
    int requested = add_image_request(d, src, n->image_invalidated, n->image_initialized && !n->image_invalidated ? n->image_request : -1);
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
    } else if (n->tag == T_select) {
        int idx = 0;
        n->selected = -1;
        for (node_t *o = n->first; o; o = o->next) {
            node_t *list = o->type == N_ELEM && o->tag == T_optgroup ? o->first : o;
            for (node_t *q = list; q; q = q->next) {
                if (q->type == N_ELEM && q->tag == T_option) {
                    if (n->selected < 0) n->selected = idx;
                    if (node_attr(q, "selected")) n->selected = idx;
                    idx++;
                }
                if (list == o) break;
            }
        }
    }
}

static void scan(web_doc *d, node_t *n) {
    for (node_t *c = n->first; c; c = c->next) {
        if (c->type != N_ELEM) continue;
        if (c->foreign) {
            scan(d, c);
            continue;
        }
        switch (c->tag) {
        case T_base: {
            const char *h = node_attr(c, "href");
            if (h && !d->base_seen) { /* only the first <base> counts */
                char abs[1024];
                if (url_resolve(d->url, h, abs, sizeof abs)) snprintf(d->base, sizeof d->base, "%s", abs);
                d->base_seen = true;
            }
            break;
        }
        case T_title:
            if ((!d->live ? !d->title : !d->scan_title_seen) && !node_ancestor(c, T_svg)) {
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
                char tmp[1024];
                snprintf(tmp, sizeof tmp, "%s", u);
                size_t tl = strlen(tmp);
                if (tl && (tmp[0] == '\'' || tmp[0] == '"')) {
                    memmove(tmp, tmp + 1, tl);
                    char *q = strpbrk(tmp, "'\"");
                    if (q) *q = 0;
                }
                char abs[1024];
                if (url_resolve(d->base, *tmp ? tmp : d->url, abs, sizeof abs)) d->refresh_url = ar_strdup(&d->mem, abs);
            }
            break;
        }
        case T_link: {
            const char *rel = node_attr(c, "rel"), *href = node_attr(c, "href");
            if (!rel || !href) break;
            /* rel is a space-separated list; "alternate stylesheet" is not applied */
            bool sheet = false, alt = false;
            const char *p = rel;
            while (*p) {
                while (is_space((unsigned char)*p)) p++;
                const char *e = p;
                while (*e && !is_space((unsigned char)*e)) e++;
                if (e - p == 10 && strn_ieq(p, "stylesheet", 10)) sheet = true;
                if (e - p == 9 && strn_ieq(p, "alternate", 9)) alt = true;
                p = e;
            }
            const char *media = node_attr(c, "media");
            if (!sheet || alt || !media_wanted(media)) break;
            char abs[2048];
            if (!url_resolve(d->base, href, abs, sizeof abs)) break;
            struct cached_css *cached = d->live ? cached_css(d, abs) : NULL;
            if (cached && cached->done) {
                if (cached->body) add_sheet(d, cached->body, cached->n, cached->base, d->next_sheet_order++, media);
                break;
            }
            if (d->live && css_queued(d, abs)) break;
            if (d->live && d->css_cache.n >= 128) break;
            struct pending *pd = calloc(1, sizeof *pd);
            pd->url = strdup(abs);
            pd->order = d->next_sheet_order++;
            pd->media = media && *media ? strdup(media) : NULL;
            if (!strncasecmp(abs, "data:", 5)) {
                size_t dn;
                char *css = data_url(abs, &dn);
                if (css) add_sheet(d, css, dn, d->base, pd->order, pd->media);
                free(css);
                free(pd->url);
                free(pd->media);
                free(pd);
            } else if (d->pending_css.n < 64) pv_push(&d->pending_css, pd);
            else {
                free(pd->url);
                free(pd->media);
                free(pd);
            }
            break;
        }
        case T_style: {
            const char *media = node_attr(c, "media");
            if (!media_wanted(media) || node_ancestor(c, T_template)) break;
            sbuf b = {0};
            text_of(c, &b);
            add_sheet(d, b.p ? b.p : "", b.n, d->base, d->next_sheet_order++, media);
            sb_free(&b);
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
        if (c->tag != T_template) scan(d, c);
    }
}

/* ---------------------------------------------------------------- documents */
size_t doc_dom_remaining(web_doc *d) {
    web_doc *family = d->dom_family ? d->dom_family : d;
    size_t used = family->mem.allocated + family->control_bytes;
    for (web_doc *p = family->dom_docs; p; p = p->dom_next) {
        if (p->mem.allocated > (32u << 20) || p->control_bytes > (32u << 20) ||
            used > (32u << 20) - p->mem.allocated || used + p->mem.allocated > (32u << 20) - p->control_bytes) return 0;
        used += p->mem.allocated + p->control_bytes;
    }
    return used >= (32u << 20) ? 0 : (32u << 20) - used;
}
void doc_dom_budget(web_doc *d) {
    if (!d || (!d->live && !d->dom_family && !d->inert)) return;
    size_t left = doc_dom_remaining(d);
    d->mem.limit = d->mem.allocated + left;
    if (!d->mem.limit) d->mem.limit = 1; /* arena zero means unbounded */
}
web_doc *doc_inert(web_doc *family, const char *html, size_t n, const char *url) {
    if (!family || n > (16u << 20)) return NULL;
    family = family->dom_family ? family->dom_family : family;
    unsigned count = 0;
    for (web_doc *p = family->dom_docs; p; p = p->dom_next) count++;
    if (count >= 64 || doc_dom_remaining(family) < 65536) return NULL;
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
    if (d->root) scan(d, d->root);
    return d;
}

web_doc *web_live(const char *html, size_t len, const char *url, const char *charset,
                  const struct web_host *host) {
    if (len > (16u << 20)) return NULL;
    web_doc *d = calloc(1, sizeof *d);
    if (!d) return NULL;
    d->live = true;
    d->dirty = d->need_style = d->resources_dirty = true;
    d->mem.limit = 32u << 20;
    d->cssmem.limit = 8u << 20;
    d->url = strdup(url && *url ? url : "about:blank");
    if (!d->url) { free(d); return NULL; }
    snprintf(d->base, sizeof d->base, "%s", d->url);
    d->parser = html_begin(d, html ? html : "", html ? len : 0, charset, true);
    if (!d->parser) { web_free(d); return NULL; }
    web_js_start(d, host);
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
    uint64_t start = uptime_ms();
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
    snprintf(d->base, sizeof d->base, "%s", d->url);
    node_t *base = first_base(d->root, &profile->metadata_visits);
    if (base) {
        char url[1024];
        if (url_resolve(d->url, node_attr(base, "href"), url, sizeof url))
            snprintf(d->base, sizeof d->base, "%s", url);
    }
    profile->metadata_ms += uptime_ms() - start;
}

void doc_rescan(web_doc *d) {
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
                char url[1024];
                if (url_resolve(d->url, node_attr(n, "href"), url, sizeof url)) snprintf(d->base, sizeof d->base, "%s", url);
                break;
            }
        return; /* no authored CSS, image/meta fetch or live resource scan */
    }
    if (!d || !d->live || !d->resources_dirty) return;
    uint64_t profile_start = uptime_ms();
    d->profile.rescans++;
    d->resources_dirty = false;
    css_styling_free(&d->sty);
    ar_free(&d->cssmem);
    free_pending(d);
    d->next_sheet_order = 0;
    d->css_depth = 0;
    d->base_seen = false;
    d->scan_title_seen = false;
    d->refresh_url = NULL;
    d->refresh_delay = 0;
    snprintf(d->base, sizeof d->base, "%s", d->url);
    d->html = d->head = d->body = NULL;
    for (node_t *n = d->root ? d->root->first : NULL; n; n = n->next)
        if (n->type == N_ELEM) { d->html = n; break; }
    for (node_t *n = d->html ? d->html->first : NULL; n; n = n->next) {
        if (!d->head && n->tag == T_head) d->head = n;
        if (!d->body && (n->tag == T_body || n->tag == T_frameset)) d->body = n;
    }
    jmp_buf trap;
    jmp_buf *old_mem = d->mem.trap, *old_css = d->cssmem.trap;
    d->mem.trap = d->cssmem.trap = &trap;
    if (!setjmp(trap)) {
        if (d->root) scan(d, d->root);
        for (node_t *n = doc_image_node_next(d, NULL); n; n = doc_image_node_next(d, n)) {
            bool in_template = false;
            for (node_t *p = n; p; p = p->parent ? p->parent : p->template_host) {
                if (p->tag == T_template || p->template_host) { in_template = true; break; }
            }
            if (n->tag == T_img && !in_template) doc_image_sync(d, n);
        }
        if (!d->scan_title_seen) d->title = NULL;
    } else {
        /* Incomplete authored sheet snapshots must never outlive their arena. */
        css_styling_free(&d->sty);
        ar_free(&d->cssmem);
        free_pending(d);
    }
    d->mem.trap = old_mem;
    d->cssmem.trap = old_css;
    d->dirty = d->need_style = true;
    d->profile.rescan_ms += uptime_ms() - profile_start;
}

void doc_css_loaded(web_doc *d, const char *url, const char *final_url, const char *css, size_t n) {
    if (!d || !url) return;
    struct cached_css *c = cached_css(d, url);
    if (!c) {
        if (d->css_cache.n >= 128) return;
        c = calloc(1, sizeof *c);
        if (!c) return;
        c->url = strdup(url);
        c->base = strdup(final_url && *final_url ? final_url : url);
        if (!c->url || !c->base) { free(c->url); free(c->base); free(c); return; }
        pv_push(&d->css_cache, c);
    }
    if (c->done) return;
    c->done = true;
    if (css && n <= (8u << 20) && d->css_bytes <= (8u << 20) - n) {
        c->body = malloc(n + 1);
        if (c->body) {
            memcpy(c->body, css, n);
            c->body[n] = 0;
            c->n = n;
            d->css_bytes += n;
        }
    }
    d->resources_dirty = d->dirty = d->need_style = true;
}

void web_tick(web_doc *d, uint64_t now) {
    if (!d || !d->live) return;
    doc_rescan(d);
    web_js_tick(d, now);
    doc_rescan(d);
}
int64_t web_deadline(web_doc *d) { return d && d->live ? web_js_deadline(d) : -1; }
void web_resource_loaded(web_doc *d, uint64_t id, const struct web_response *r) {
    if (d && d->live) web_js_loaded(d, id, r);
}
bool web_dirty(web_doc *d) {
    if (!d) return false;
    bool dirty = d->dirty;
    d->dirty = false;
    return dirty;
}
bool web_script_running(web_doc *d) { return d && d->live && web_js_running(d); }
bool web_dispatch(web_doc *d, web_node *target, const struct web_event *e) {
    return !d || !d->live || web_js_dispatch(d, target, e);
}

static void free_values(node_t *n) {
    for (node_t *c = n->first; c; c = c->next) {
        if (c->type != N_ELEM) continue;
        free(c->value);
        c->value = NULL;
        free_values(c);
    }
}

void web_free(web_doc *d) {
    if (!d) return;
    web_js_free(d);
    while (d->dom_docs) {
        web_doc *child = d->dom_docs;
        d->dom_docs = child->dom_next; child->dom_next = NULL;
        web_free(child);
    }
    html_finish(d->parser);
    d->parser = NULL;
    if (d->owned_nodes) {
        for (node_t *n = d->owned_nodes; n; n = n->owned_next) free(n->value);
    } else if (d->root) free_values(d->root);
    css_styling_free(&d->sty);
    free_pending(d);
    pv_free(&d->pending_css);
    for (int i = 0; i < d->css_cache.n; i++) {
        struct cached_css *c = d->css_cache.v[i];
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
    ar_free(&d->smem);
    ar_free(&d->lmem);
    ar_free(&d->cssmem);
    free(d->url);
    free(d);
}

const char *web_title(web_doc *d) { return d->title ? d->title : ""; }
const char *web_url(web_doc *d) { return d->url; }
bool web_set_url(web_doc *d, const char *url) {
    if (!d || !url) return false;
    char *copy = strdup(url);
    if (!copy) return false;
    free(d->url); d->url = copy;
    if (!d->base_seen) snprintf(d->base, sizeof d->base, "%s", url);
    return true;
}
const char *web_control_value(web_node *control) { return control ? control->value : NULL; }

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
        add_sheet(d, css, n, p->url, p->order, p->media);
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
        else im->img = image_decode(data, n);
    }
    if (!im->img) im->failed = true;
    d->layout_valid = false; d->dirty = true;
    for (node_t *node = doc_image_node_next(d, NULL); node; node = doc_image_node_next(d, node)) {
        if (node->tag == T_img && node->image_initialized && node->image_request == i) {
            bool in_template = false;
            for (node_t *p = node; p; p = p->parent ? p->parent : p->template_host) {
                if (p->tag == T_template || p->template_host) { in_template = true; break; }
            }
            if (!in_template) node->image = i;
        }
    }
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
    }
}

static void clear_intrinsic(box_t *b) {
    b->intrinsic_done = false;
    for (box_t *c = b->first; c; c = c->next) clear_intrinsic(c);
}

int web_layout(web_doc *d, int width, int height) {
    if (width < 1) width = 1;
    if (height < 1) height = 1;
    if (d->resources_dirty) doc_rescan(d);
    /* Geometry reads may flush layout repeatedly within one script. Reuse the
       result until DOM/style, viewport, or image intrinsic dimensions change. */
    if (d->layout_valid && !d->need_style && d->root_box &&
        d->width == width && d->height == height) return d->doc_h;
    if (d->need_style || d->styled_w != width || d->styled_h != height || !d->root_box) {
        css_cascade(d, width, height);
        if (d->root) register_bg(d, d->root);
        boxes_build(d, &d->smem);
        d->need_style = false;
    } else if (d->root_box) clear_intrinsic(d->root_box);
    d->width = width;
    d->height = height;
    layout_doc(d, width, height);
    d->layout_revision++;
    d->layout_valid = true;
    if (d->find_text[0]) {
        char t[sizeof d->find_text];
        memcpy(t, d->find_text, sizeof t);
        web_find(d, t, 0);
    }
    return d->doc_h;
}

int web_doc_height(web_doc *d) { return d->doc_h; }

const char *doc_link_href(web_doc *d, node_t *a) {
    static char buf[2048];
    const char *h = node_attr(a, "href");
    if (!h) return NULL;
    while (is_space((unsigned char)*h)) h++;
    if (!strncasecmp(h, "javascript:", 11)) return NULL;
    if (!url_resolve(d->base, h, buf, sizeof buf)) return NULL;
    return buf;
}

/* the border-box top of the element's first box, or of the line its first inline box is on */
static bool node_top(node_t *n, float *y) {
    if (n->box && n->box->kind != B_INLINE) {
        box_t *b = n->box;
        *y = box_abs_y(b) - b->p[0] - b->b[0];
        return true;
    }
    if (n->anchor_block) {
        *y = box_abs_y(n->anchor_block) + n->anchor_block->content_dy + n->anchor_dy;
        return true;
    }
    for (node_t *c = n->first; c; c = c->next)
        if (c->type == N_ELEM && node_top(c, y)) return true;
    return false;
}

static node_t *find_anchor(node_t *n, const char *frag) {
    for (node_t *c = n->first; c; c = c->next) {
        if (c->type != N_ELEM) continue;
        if (c->id && !strcmp(c->id, frag)) return c;
        if (c->tag == T_a) {
            const char *nm = node_attr(c, "name");
            if (nm && !strcmp(nm, frag)) return c;
        }
        node_t *r = find_anchor(c, frag);
        if (r) return r;
    }
    return NULL;
}

int web_anchor_y(web_doc *d, const char *fragment) {
    if (!fragment || !*fragment || !d->root) return -1;
    if (!strcmp(fragment, "top")) return 0;
    /* percent-decode */
    char f[256];
    size_t k = 0;
    for (const char *p = fragment; *p && k + 1 < sizeof f; p++) {
        if (*p == '%' && hexv(p[1]) >= 0 && hexv(p[2]) >= 0) {
            f[k++] = (char)(hexv(p[1]) * 16 + hexv(p[2]));
            p += 2;
        } else f[k++] = *p;
    }
    f[k] = 0;
    node_t *n = find_anchor(d->root, f);
    float y;
    if (!n || !node_top(n, &y)) return -1;
    return y < 0 ? 0 : (int)y;
}

bool web_node_rect(web_doc *d, web_node *n, int *x, int *y, int *w, int *h) {
    if (!n || !n->box) return false;
    if (d && d->live) {
        node_t *root = n;
        while (root->parent) root = root->parent;
        if (root != d->root) return false;
    }
    box_t *b = n->box;
    *x = (int)(box_abs_x(b) - b->p[3] - b->b[3]);
    *y = (int)(box_abs_y(b) - b->p[0] - b->b[0]);
    *w = (int)(b->w + b->p[1] + b->p[3] + b->b[1] + b->b[3]);
    *h = (int)(b->h + b->p[0] + b->p[2] + b->b[0] + b->b[2]);
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
    if (d->focus == n) d->caret = (int)doc_utf16_to_byte(n->value,
        n->selection_direction == 2 ? n->selection_start : n->selection_end, false);
}
void doc_control_selection(web_doc *d, node_t *n, uint32_t start, uint32_t end, uint8_t direction) {
    uint32_t length = doc_utf16_length(n->value);
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
bool doc_control_replace(web_doc *d, node_t *n, uint32_t start, uint32_t end, const char *text) {
    const char *value = n->value ? n->value : ""; uint32_t length = doc_utf16_length(value);
    if (start > length) start = length;
    if (end > length) end = length;
    if (start > end) return false;
    size_t bytes = strlen(value), replacement = strlen(text);
    if (replacement > (16u << 20) || bytes > (16u << 20) || bytes + replacement > (16u << 20) + 6u) return false;
    /* Reserve enough for both split-surrogate boundaries. Do not let sbuf's
       abort-on-OOM grow path turn a failed edit into a browser process crash. */
    sbuf b = {0}; b.cap = bytes + replacement + 7; b.p = malloc(b.cap);
    if (!b.p) return false;
    control_slice(&b, value, 0, start); sb_puts(&b, text); control_slice(&b, value, end, length);
    bool ok = doc_node_value(d, n, b.p ? b.p : "", b.n); sb_free(&b); return ok;
}

void web_focus(web_doc *d, web_node *n) {
    if (n && node_attr(n, "disabled")) return;
    if (n) doc_control_init(d, n);
    /* Keep Nocturne's existing first native focus at the end of a pristine
       control. An explicit JS range (including 0,0), value setter, or remembered
       user range takes priority; the unfocused initial DOM cursor is still 0. */
    if (n && (n->tag == T_input || n->tag == T_textarea) && !n->selection_set) {
        n->selection_start = n->selection_end = doc_utf16_length(n->value);
        n->selection_direction = 0; n->selection_set = true;
    }
    d->focus = n;
    d->caret = 0;
    if (n) doc_control_caret(d, n);
    d->dirty = true;
}

web_node *web_focused(web_doc *d) { return d->focus; }

static bool readonly(node_t *n) { return node_attr(n, "readonly") || node_attr(n, "disabled"); }

int web_key(web_doc *d, const struct gui_event *e) {
    node_t *n = d->focus;
    if (!n || !(n->tag == T_input || n->tag == T_textarea) || node_attr(n, "disabled")) return 0;
    doc_control_init(d, n);
    if (!n->value && !doc_node_value(d, n, "", 0)) return 0;
    char *v = n->value;
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
        if (!multi) return 0;
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
        if (readonly(n) || (start == end && !split_cursor && c == 0)) return 0;
        if (split_cursor) start--;
        else if (start == end) { int s = c - 1; while (s > 0 && ((unsigned char)v[s] & 0xc0) == 0x80) s--; start = doc_byte_to_utf16(v, (size_t)s); }
        if (!doc_control_replace(d, n, start, end, "")) return 0;
        doc_control_selection(d, n, start, start, 0);
        return 1;
    }
    case NKEY_DELETE: {
        if (readonly(n) || (start == end && c >= len)) return 0;
        if (split_cursor) end++;
        else if (start == end) { int e2 = c + 1; while (e2 < len && ((unsigned char)v[e2] & 0xc0) == 0x80) e2++; end = doc_byte_to_utf16(v, (size_t)e2); }
        if (!doc_control_replace(d, n, start, end, "")) return 0;
        doc_control_selection(d, n, start, start, 0);
        return 1;
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
    if (k > 0x10FFFF || (k >= 0xd800 && k <= 0xdfff) || readonly(n)) return 0;
    const char *ml = node_attr(n, "maxlength");
    uint32_t units = k > 0xffff ? 2 : 1;
    if (ml && *ml >= '0' && *ml <= '9' && doc_utf16_length(v) - (end - start) + units > (uint32_t)atoi(ml)) return 0;
    char enc[5];
    int el = utf8_put(enc, k);
    enc[el] = 0;
    bool changed = doc_control_replace(d, n, start, end, enc);
    if (!changed) return 0;
    doc_control_selection(d, n, start + units, start + units, 0);
    return 1;
}

static node_t *form_of(web_doc *d, node_t *n) {
    if (!n) return NULL;
    const char *fid = node_attr(n, "form");
    if (fid && d->root) {
        node_t *f = find_anchor(d->root, fid);
        if (f && f->tag == T_form) return f;
    }
    return node_ancestor(n, T_form);
}

web_node *web_form_owner(web_doc *d, web_node *control) { return form_of(d, control); }

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
        if (n->textlen > (16u << 20) - *len) return false;
        if (out && n->textlen) memcpy(out + *len, n->text, n->textlen);
        *len += n->textlen;
    } else for (node_t *c = n->first; c; c = c->next)
        if (!reset_form_text(c, out, len)) return false;
    return true;
}

static bool reset_form_controls(web_doc *d, node_t *root, node_t *form, node_t *n) {
    if (n->type == N_ELEM && !n->foreign && (n->tag == T_input || n->tag == T_textarea || n->tag == T_select)) {
        const char *fid = node_attr(n, "form");
        node_t *owner = fid ? (root == d->root ? reset_form_id(root, fid) : NULL) : node_ancestor(n, T_form);
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
                    option->selected_set = false;
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
    d->dirty = d->need_style = true;
    return ok;
}

static void uncheck_radios(node_t *scope, node_t *keep, const char *name) {
    for (node_t *c = scope->first; c; c = c->next) {
        if (c->type != N_ELEM) continue;
        if (c != keep && c->tag == T_input) {
            const char *t = node_attr(c, "type"), *nm = node_attr(c, "name");
            if (t && str_ieq(t, "radio") && nm && !strcmp(nm, name)) c->checked = false;
        }
        uncheck_radios(c, keep, name);
    }
}

void doc_control_checked(web_doc *d, node_t *n, bool checked) {
    if (!d || !n || n->tag != T_input) return;
    doc_control_init(d, n);
    n->checked_dirty = true;
    const char *t = node_attr(n, "type");
    if (checked && t && str_ieq(t, "radio")) {
        const char *name = node_attr(n, "name");
        if (name && *name) {
            node_t *scope = form_of(d, n);
            uncheck_radios(scope ? scope : d->root, n, name);
        }
    }
    n->checked = checked;
    d->dirty = d->need_style = true;
}

void web_toggle(web_doc *d, web_node *n) {
    if (!n || n->tag != T_input || node_attr(n, "disabled")) return;
    doc_control_init(d, n);
    const char *t = node_attr(n, "type");
    doc_control_checked(d, n, t && str_ieq(t, "radio") ? true : !n->checked);
}

node_t *doc_select_option(node_t *sel, int index) {
    if (!sel || sel->tag != T_select || index < 0) return NULL;
    int idx = 0;
    for (node_t *o = sel->first; o; o = o->next) {
        node_t *list = o->type == N_ELEM && o->tag == T_optgroup ? o->first : o;
        for (node_t *q = list; q; q = q->next) {
            if (q->type == N_ELEM && q->tag == T_option) {
                if (idx == index) return q;
                idx++;
            }
            if (list == o) break;
        }
    }
    return NULL;
}

bool doc_option_selected(node_t *option) {
    if (!option || option->tag != T_option) return false;
    node_t *sel = option->parent;
    if (sel && sel->tag == T_optgroup) sel = sel->parent;
    if (!sel || sel->tag != T_select) return node_attr(option, "selected") != NULL;
    int selected = sel->selected;
    if (!sel->control_ready) {
        selected = 0;
        for (int i = 0; ; i++) {
            node_t *n = doc_select_option(sel, i);
            if (!n) break;
            if (node_attr(n, "selected")) selected = i;
        }
    }
    return doc_select_option(sel, selected) == option;
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

static void add_pair(sbuf *q, const char *name, const char *value) {
    if (q->n) sb_putc(q, '&');
    url_encode_form(q, name);
    sb_putc(q, '=');
    url_encode_form(q, value);
}

static void collect(web_doc *d, node_t *form, node_t *scope, node_t *submitter, sbuf *q) {
    for (node_t *c = scope->first; c; c = c->next) {
        if (c->type != N_ELEM) continue;
        if (c->tag == T_template) continue;
        const char *name = node_attr(c, "name");
        bool mine = form_of(d, c) == form;
        if (mine && name && *name && !node_attr(c, "disabled")) {
            doc_control_init(d, c);
            if (c->tag == T_input) {
                const char *t = node_attr(c, "type");
                if (!t) t = "text";
                if (str_ieq(t, "checkbox") || str_ieq(t, "radio")) {
                    if (c->checked) {
                        const char *v = node_attr(c, "value");
                        add_pair(q, name, v ? v : "on");
                    }
                } else if (str_ieq(t, "submit") || str_ieq(t, "button") || str_ieq(t, "reset")) {
                    if (c == submitter && str_ieq(t, "submit")) add_pair(q, name, web_button_label(c));
                } else if (str_ieq(t, "image")) {
                    if (c == submitter) {
                        char nm[300];
                        snprintf(nm, sizeof nm, "%s.x", name);
                        add_pair(q, nm, "0");
                        snprintf(nm, sizeof nm, "%s.y", name);
                        add_pair(q, nm, "0");
                    }
                } else if (!str_ieq(t, "file")) add_pair(q, name, c->value ? c->value : "");
            } else if (c->tag == T_textarea) {
                add_pair(q, name, c->value ? c->value : "");
            } else if (c->tag == T_select) {
                node_t *o = doc_select_option(c, c->selected);
                if (o) {
                    const char *v = node_attr(o, "value");
                    sbuf t = {0};
                    if (!v) {
                        option_text(o, &t);
                        v = t.p ? sb_cstr(&t) : "";
                    }
                    add_pair(q, name, v);
                    sb_free(&t);
                }
            } else if (c->tag == T_button && c == submitter) {
                const char *v = node_attr(c, "value");
                add_pair(q, name, v ? v : "");
            }
        }
        collect(d, form, c, submitter, q);
    }
}

bool web_submit(web_doc *d, web_node *submitter, char **url, char **body) {
    *url = NULL;
    *body = NULL;
    if (!submitter) return false;
    node_t *form = submitter->tag == T_form ? submitter : form_of(d, submitter);
    if (!form) return false;
    const char *action = node_attr(submitter, "formaction");
    if (!action) action = node_attr(form, "action");
    const char *method = node_attr(submitter, "formmethod");
    if (!method) method = node_attr(form, "method");
    bool post = method && str_ieq(method, "post");
    char abs[2048];
    if (!url_resolve(d->base, action && *action ? action : d->url, abs, sizeof abs)) return false;
    if (!strncasecmp(abs, "javascript:", 11)) return false;
    sbuf q = {0};
    collect(d, form, d->root, submitter, &q);
    const char *qs = q.p ? sb_cstr(&q) : "";
    if (post) {
        *url = strdup(abs);
        *body = strdup(qs);
    } else {
        /* replace the query; drop the fragment */
        char *hash = strchr(abs, '#');
        if (hash) *hash = 0;
        char *qm = strchr(abs, '?');
        if (qm) *qm = 0;
        size_t n = strlen(abs) + strlen(qs) + 2;
        *url = malloc(n);
        snprintf(*url, n, "%s?%s", abs, qs);
    }
    sb_free(&q);
    return true;
}

int web_select_options(web_doc *d, web_node *sel, const char **labels, int max, int *selected) {
    if (!d || !sel || sel->tag != T_select || !labels || max <= 0) return 0;
    doc_control_init(d, sel);
    if (selected) *selected = sel->selected;
    int k = 0;
    for (node_t *o; k < max && (o = doc_select_option(sel, k)); k++) {
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
    if (d && sel && sel->tag == T_select && (index == -1 || doc_select_option(sel, index))) {
        doc_control_init(d, sel);
        sel->selected = index;
        sel->selected_set = true;
        d->dirty = d->need_style = true;
    }
}
