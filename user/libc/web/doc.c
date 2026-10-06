/* The public API (web.h): documents and their resources (stylesheets, images, data: URLs), the
   style/box/layout pipeline, and forms. */
#include <stdio.h>
#include <ctype.h>
#include "webi.h"

struct pending {
    char *url;
    double order;
    char *media; /* the <link media> the sheet is wrapped in, or NULL */
};

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
    pvec imports = {0};
    sheet_t *sh;
    if (media && *media) {
        sbuf w = {0};
        sb_puts(&w, "@media ");
        sb_puts(&w, media);
        sb_puts(&w, "{\n");
        sb_put(&w, css, n);
        sb_puts(&w, "\n}");
        sh = css_parse_sheet(&d->mem, w.p, w.n, base, order, &imports);
        sb_free(&w);
    } else sh = css_parse_sheet(&d->mem, css, n, base, order, &imports);
    pv_push(&d->sty.sheets, sh);
    for (int i = 0; i < imports.n; i++) {
        struct css_import *im = imports.v[i];
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
}

void doc_add_stylesheet_text(web_doc *d, const char *css, size_t n, const char *base) {
    add_sheet(d, css, n, base, d->next_sheet_order++, NULL);
}

static int add_image(web_doc *d, const char *rel) {
    char abs[2048];
    if (!rel || !*rel) return -1;
    while (is_space((unsigned char)*rel)) rel++;
    if (!url_resolve(d->base, rel, abs, sizeof abs)) return -1;
    for (int i = 0; i < d->images.n; i++) {
        struct web_image *im = d->images.v[i];
        if (!strcmp(im->url, abs)) return i;
    }
    if (d->images.n >= 2000) return -1;
    struct web_image *im = calloc(1, sizeof *im);
    im->url = strdup(abs);
    pv_push(&d->images, im);
    int idx = d->images.n - 1;
    if (!strncasecmp(abs, "data:", 5)) {
        size_t n;
        char *data = data_url(abs, &n);
        web_image_loaded(d, idx, data, n);
        free(data);
    }
    return idx;
}

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

static void init_control(node_t *n) {
    if (n->tag == T_input) {
        const char *v = node_attr(n, "value");
        n->value = strdup(v ? v : "");
        n->checked = node_attr(n, "checked") != NULL;
    } else if (n->tag == T_textarea) {
        sbuf b = {0};
        text_of(n, &b);
        n->value = strdup(b.p ? sb_cstr(&b) : "");
        sb_free(&b);
    } else if (n->tag == T_select) {
        int idx = 0;
        n->selected = 0;
        for (node_t *o = n->first; o; o = o->next) {
            node_t *list = o->type == N_ELEM && o->tag == T_optgroup ? o->first : o;
            for (node_t *q = list; q; q = q->next) {
                if (q->type == N_ELEM && q->tag == T_option) {
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
            if (!d->title && !node_ancestor(c, T_svg)) {
                sbuf b = {0};
                text_of(c, &b);
                /* collapse white space */
                char *t = ar_alloc(&d->mem, b.n + 1);
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
                d->title = t;
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
        case T_img: {
            const char *src = node_attr(c, "src"), *ss = node_attr(c, "srcset");
            const char *lazy = node_attr(c, "data-src");
            if (!lazy) lazy = node_attr(c, "data-lazy-src");
            if (!lazy) lazy = node_attr(c, "data-original");
            char *pick = NULL;
            if (placeholder_src(src) && lazy) src = lazy;
            else if (placeholder_src(src) && ss) src = pick = srcset_pick(ss);
            else if (placeholder_src(src) && node_attr(c, "data-srcset")) src = pick = srcset_pick(node_attr(c, "data-srcset"));
            c->image = add_image(d, src);
            free(pick);
            break;
        }
        case T_input: {
            const char *t = node_attr(c, "type");
            if (t && str_ieq(t, "image")) c->image = add_image(d, node_attr(c, "src"));
            init_control(c);
            break;
        }
        case T_textarea: case T_select: init_control(c); break;
        }
        if (c->tag != T_template) scan(d, c);
    }
}

/* ---------------------------------------------------------------- documents */
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
    if (d->root) free_values(d->root);
    css_styling_free(&d->sty);
    for (int i = 0; i < d->pending_css.n; i++) {
        struct pending *p = d->pending_css.v[i];
        free(p->url);
        free(p->media);
        free(p);
    }
    pv_free(&d->pending_css);
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
    free(d->url);
    free(d);
}

const char *web_title(web_doc *d) { return d->title ? d->title : ""; }
const char *web_url(web_doc *d) { return d->url; }

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
    if (d->need_style || d->styled_w != width || d->styled_h != height || !d->root_box) {
        css_cascade(d, width, height);
        if (d->root) register_bg(d, d->root);
        boxes_build(d, &d->smem);
        d->need_style = false;
    } else if (d->root_box) clear_intrinsic(d->root_box);
    d->width = width;
    d->height = height;
    layout_doc(d, width, height);
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
    (void)d;
    if (!n || !n->box) return false;
    box_t *b = n->box;
    *x = (int)(box_abs_x(b) - b->p[3] - b->b[3]);
    *y = (int)(box_abs_y(b) - b->p[0] - b->b[0]);
    *w = (int)(b->w + b->p[1] + b->p[3] + b->b[1] + b->b[3]);
    *h = (int)(b->h + b->p[0] + b->p[2] + b->b[0] + b->b[2]);
    return true;
}

/* ---------------------------------------------------------------- forms */
void web_focus(web_doc *d, web_node *n) {
    d->focus = n;
    d->caret = n && n->value ? (int)strlen(n->value) : 0;
}

web_node *web_focused(web_doc *d) { return d->focus; }

static bool readonly(node_t *n) { return node_attr(n, "readonly") || node_attr(n, "disabled"); }

int web_key(web_doc *d, const struct gui_event *e) {
    node_t *n = d->focus;
    if (!n || !(n->tag == T_input || n->tag == T_textarea)) return 0;
    if (!n->value) n->value = strdup("");
    char *v = n->value;
    int len = (int)strlen(v);
    if (d->caret > len) d->caret = len;
    int c = d->caret;
    uint32_t k = e->key;
    bool multi = n->tag == T_textarea;
    switch (k) {
    case NKEY_LEFT:
        if (c > 0) {
            c--;
            while (c > 0 && ((unsigned char)v[c] & 0xC0) == 0x80) c--;
        }
        d->caret = c;
        return 1;
    case NKEY_RIGHT:
        if (c < len) {
            c++;
            while (c < len && ((unsigned char)v[c] & 0xC0) == 0x80) c++;
        }
        d->caret = c;
        return 1;
    case NKEY_HOME:
        if (multi)
            while (c > 0 && v[c - 1] != '\n') c--;
        else c = 0;
        d->caret = c;
        return 1;
    case NKEY_END:
        if (multi)
            while (c < len && v[c] != '\n') c++;
        else c = len;
        d->caret = c;
        return 1;
    case NKEY_ENTER:
        if (!multi) return 2;
        k = '\n';
        break;
    case NKEY_BACKSPACE: {
        if (readonly(n) || c == 0) return 0;
        int s = c - 1;
        while (s > 0 && ((unsigned char)v[s] & 0xC0) == 0x80) s--;
        memmove(v + s, v + c, (size_t)(len - c + 1));
        d->caret = s;
        return 1;
    }
    case NKEY_DELETE: {
        if (readonly(n) || c >= len) return 0;
        int e2 = c + 1;
        while (e2 < len && ((unsigned char)v[e2] & 0xC0) == 0x80) e2++;
        memmove(v + c, v + e2, (size_t)(len - e2 + 1));
        return 1;
    }
    }
    if (e->mods & (NMOD_CTRL | NMOD_ALT)) return 0;
    if (k < 32 && k != '\n') return 0;
    if (k >= 0x100 && k < 0x200) return 0; /* other special keys */
    if (k > 0x10FFFF || readonly(n)) return 0;
    const char *ml = node_attr(n, "maxlength");
    if (ml && atoi(ml) > 0 && len >= atoi(ml)) return 0;
    char enc[4];
    int el = utf8_put(enc, k);
    char *nv = malloc((size_t)len + (size_t)el + 1);
    memcpy(nv, v, (size_t)c);
    memcpy(nv + c, enc, (size_t)el);
    memcpy(nv + c + el, v + c, (size_t)(len - c + 1));
    free(v);
    n->value = nv;
    d->caret = c + el;
    return 1;
}

static node_t *form_of(web_doc *d, node_t *n) {
    const char *fid = node_attr(n, "form");
    if (fid && d->root) {
        node_t *f = find_anchor(d->root, fid);
        if (f && f->tag == T_form) return f;
    }
    return node_ancestor(n, T_form);
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

void web_toggle(web_doc *d, web_node *n) {
    if (!n || n->tag != T_input || node_attr(n, "disabled")) return;
    const char *t = node_attr(n, "type");
    if (t && str_ieq(t, "radio")) {
        const char *name = node_attr(n, "name");
        if (name && *name) {
            node_t *scope = form_of(d, n);
            uncheck_radios(scope ? scope : d->root, n, name);
        }
        n->checked = true;
    } else n->checked = !n->checked;
}

static node_t *option_at(node_t *sel, int index) {
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
                node_t *o = option_at(c, c->selected);
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
    if (selected) *selected = sel->selected;
    int k = 0;
    for (node_t *o; k < max && (o = option_at(sel, k)); k++) {
        sbuf t = {0};
        option_text(o, &t);
        labels[k] = ar_strdup(&d->mem, t.p ? sb_cstr(&t) : "");
        sb_free(&t);
    }
    return k;
}

void web_select_set(web_doc *d, web_node *sel, int index) {
    (void)d;
    if (sel && option_at(sel, index)) sel->selected = index;
}
