/* DevTools inspection: DOM tree walk, box model and the Styles/Computed
   panes. Read-only views over the native DOM, cascade AST and layout; the
   cascade's own matcher supplies the rules (css_inspect_rules). */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "webi.h"
#include "web_inspect.h"
#include "inspect_private.h"

/* ---------------------------------------------------------------- tree */
static bool skipped(const node_t *n) {
    if (n->type == N_ATTR) return true;
    if (n->type != N_TEXT) return false;
    for (size_t i = 0; i < n->textlen; i++) {
        char c = n->text[i];
        if (c != ' ' && c != '\t' && c != '\n' && c != '\r' && c != '\f') return false;
    }
    return true;
}
static node_t *visible_from(node_t *n) { while (n && skipped(n)) n = n->next; return n; }
static bool shadow_root(const node_t *n) { return n && n->type == N_FRAGMENT && n->shadow_host; }

web_node *web_inspect_root(web_doc *d) { return d ? d->root : NULL; }
web_node *web_inspect_first(web_node *n) {
    if (!n) return NULL;
    if (n->type == N_ELEM && n->shadow_root) return n->shadow_root;
    return visible_from(n->first);
}
web_node *web_inspect_next(web_node *n) {
    if (!n) return NULL;
    if (shadow_root(n)) return visible_from(n->shadow_host->first);
    return visible_from(n->next);
}
web_node *web_inspect_parent(web_node *n) {
    if (!n) return NULL;
    return shadow_root(n) ? n->shadow_host : n->parent;
}
bool web_inspect_contains(web_node *ancestor, web_node *n) {
    for (; n; n = web_inspect_parent(n)) if (n == ancestor) return true;
    return false;
}
int web_inspect_kind(web_node *n) {
    if (!n) return WEB_INSPECT_OTHER;
    switch (n->type) {
    case N_DOC: return WEB_INSPECT_DOCUMENT;
    case N_ELEM: return WEB_INSPECT_ELEMENT;
    case N_TEXT: return WEB_INSPECT_TEXT;
    case N_COMMENT: return WEB_INSPECT_COMMENT;
    case N_DOCTYPE: return WEB_INSPECT_DOCTYPE;
    case N_FRAGMENT: return n->shadow_host ? WEB_INSPECT_SHADOW_ROOT : WEB_INSPECT_OTHER;
    default: return WEB_INSPECT_OTHER;
    }
}
const char *web_inspect_name(web_node *n) {
    if (!n) return "";
    if (n->type == N_ELEM || n->type == N_DOCTYPE) return n->raw_name ? n->raw_name : n->name ? n->name : "";
    return "";
}
int web_inspect_attr_count(web_node *n) { return n && n->type == N_ELEM ? n->nattrs : 0; }
bool web_inspect_attr(web_node *n, int i, const char **name, const char **value) {
    if (!n || n->type != N_ELEM || i < 0 || i >= n->nattrs) return false;
    const struct attr *a = &n->attrs[i];
    *name = a->raw ? a->raw : a->name ? a->name : "";
    *value = a->value ? a->value : "";
    return true;
}
const char *web_inspect_text(web_node *n, size_t *length) {
    if (length) *length = 0;
    if (!n) return "";
    if (shadow_root(n)) {
        const char *mode = n->shadow_closed ? "closed" : "open";
        if (length) *length = strlen(mode);
        return mode;
    }
    if (n->type != N_TEXT && n->type != N_COMMENT) return "";
    if (length) *length = n->textlen;
    return n->text ? n->text : "";
}

/* ---------------------------------------------------------------- box model */
bool web_inspect_box(web_doc *d, web_node *n, struct web_inspect_box *out) {
    memset(out, 0, sizeof *out);
    if (!n || n->type != N_ELEM || !n->box) return false;
    if (!web_node_rect(d, n, &out->x, &out->y, &out->w, &out->h)) return false;
    const box_t *b = n->box;
    for (int i = 0; i < 4; i++) { out->margin[i] = b->m[i]; out->border[i] = b->b[i]; out->padding[i] = b->p[i]; }
    return true;
}

/* ---------------------------------------------------------------- styles */
struct pending_rule {
    struct web_inspect_rule rule;
    int capacity;
    uint32_t specificity, order;
    bool inline_style;
};
struct collector {
    struct pending_rule *v;
    int n, capacity;
    web_node *from;
    bool failed;
};
static char *dup_text(const char *s) { return strdup(s ? s : ""); }
static bool collect_rule(void *ctx, const char *selector, const char *href, bool ua, uint32_t spec, uint32_t order) {
    struct collector *c = ctx;
    if (c->n == c->capacity) {
        int capacity = c->capacity ? c->capacity * 2 : 16;
        void *grown = realloc(c->v, (size_t)capacity * sizeof *c->v);
        if (!grown) { c->failed = true; return false; }
        c->v = grown; c->capacity = capacity;
    }
    struct pending_rule *p = &c->v[c->n];
    memset(p, 0, sizeof *p);
    p->rule.selector = dup_text(selector);
    p->rule.source = dup_text(ua ? "user agent stylesheet" : href ? href : spec == UINT32_MAX ? "" : "<style>");
    p->rule.inherited_from = c->from;
    p->rule.ua = ua;
    p->specificity = spec; p->order = order;
    p->inline_style = spec == UINT32_MAX && order == UINT32_MAX;
    c->n++;
    if (!p->rule.selector || !p->rule.source) { c->failed = true; return false; }
    return true;
}
static bool collect_decl(void *ctx, const char *name, const char *value, bool important, bool inherited) {
    struct collector *c = ctx;
    if (!c->n) return true;
    struct pending_rule *p = &c->v[c->n - 1];
    if (c->from && !inherited) return true; /* inherited sections list inherited properties only */
    if (p->rule.count == p->capacity) {
        int capacity = p->capacity ? p->capacity * 2 : 8;
        void *grown = realloc(p->rule.decls, (size_t)capacity * sizeof *p->rule.decls);
        if (!grown) { c->failed = true; return false; }
        p->rule.decls = grown; p->capacity = capacity;
    }
    struct web_inspect_decl *x = &p->rule.decls[p->rule.count++];
    x->name = dup_text(name); x->value = dup_text(value); x->important = important; x->overridden = false;
    if (!x->name || !x->value) { c->failed = true; return false; }
    return true;
}
static void rule_free(struct web_inspect_rule *r) {
    for (int i = 0; i < r->count; i++) { free(r->decls[i].name); free(r->decls[i].value); }
    free(r->decls); free(r->selector); free(r->source);
}
/* Display order: element.style, then author before user agent, higher
   specificity first, later rules first. */
static int precedence(const void *a, const void *b) {
    const struct pending_rule *x = a, *y = b;
    if (x->inline_style != y->inline_style) return x->inline_style ? -1 : 1;
    if (x->rule.ua != y->rule.ua) return x->rule.ua ? 1 : -1;
    if (x->specificity != y->specificity) return x->specificity > y->specificity ? -1 : 1;
    return x->order > y->order ? -1 : x->order < y->order ? 1 : 0;
}

struct name_set { char **v; int n, capacity; };
static bool set_has(const struct name_set *s, const char *name) {
    for (int i = 0; i < s->n; i++) if (!strcmp(s->v[i], name)) return true;
    return false;
}
static bool set_add(struct name_set *s, char *name) {
    if (set_has(s, name)) return true;
    if (s->n == s->capacity) {
        int capacity = s->capacity ? s->capacity * 2 : 32;
        void *grown = realloc(s->v, (size_t)capacity * sizeof *s->v);
        if (!grown) return false;
        s->v = grown; s->capacity = capacity;
    }
    s->v[s->n++] = name;
    return true;
}

bool web_inspect_styles(web_doc *d, web_node *n, struct web_inspect_styles *out) {
    out->rules = NULL; out->count = 0;
    if (!d || !n || n->type != N_ELEM) return false;
    web_doc *owner = n->owner ? n->owner : d;
    struct collector c = {0};
    const struct css_inspect_sink sink = {&c, collect_rule, collect_decl};
    struct name_set seen = {0}, important = {0};
    bool ok = true;
    for (node_t *e = n; e && ok; e = e->parent ? e->parent : e->type == N_FRAGMENT ? e->shadow_host : NULL) {
        if (e->type == N_FRAGMENT) continue;
        if (e->type != N_ELEM) break;
        c.from = e == n ? NULL : e;
        int first = c.n;
        ok = css_inspect_inline(owner, e, &sink) && css_inspect_rules(owner, e, &sink) && !c.failed;
        if (!ok) break;
        qsort(c.v + first, (size_t)(c.n - first), sizeof *c.v, precedence);
        /* Important declarations win over every normal one of this level. */
        for (int i = first; i < c.n && ok; i++)
            for (int k = 0; k < c.v[i].rule.count; k++)
                if (c.v[i].rule.decls[k].important) {
                    if (set_has(&important, c.v[i].rule.decls[k].name)) c.v[i].rule.decls[k].overridden = true;
                    else if (!set_add(&important, c.v[i].rule.decls[k].name)) ok = false;
                }
        for (int i = first; i < c.n && ok; i++)
            for (int k = 0; k < c.v[i].rule.count; k++) {
                struct web_inspect_decl *x = &c.v[i].rule.decls[k];
                if (!x->important) {
                    if (set_has(&seen, x->name) || set_has(&important, x->name)) x->overridden = true;
                    else if (!set_add(&seen, x->name)) ok = false;
                } else if (!set_add(&seen, x->name)) ok = false;
            }
        /* An inherited section with no inherited property says nothing. */
        if (e != n) {
            int kept = first;
            for (int i = first; i < c.n; i++) {
                if (c.v[i].rule.count) c.v[kept++] = c.v[i];
                else rule_free(&c.v[i].rule);
            }
            c.n = kept;
        }
    }
    free(seen.v); free(important.v);
    if (!ok) {
        for (int i = 0; i < c.n; i++) rule_free(&c.v[i].rule);
        free(c.v);
        return false;
    }
    struct web_inspect_rule *rules = c.n ? malloc((size_t)c.n * sizeof *rules) : NULL;
    if (c.n && !rules) { for (int i = 0; i < c.n; i++) rule_free(&c.v[i].rule); free(c.v); return false; }
    for (int i = 0; i < c.n; i++) rules[i] = c.v[i].rule;
    free(c.v);
    out->rules = rules; out->count = c.n;
    return true;
}
void web_inspect_styles_free(struct web_inspect_styles *styles) {
    if (!styles) return;
    for (int i = 0; i < styles->count; i++) rule_free(&styles->rules[i]);
    free(styles->rules);
    styles->rules = NULL; styles->count = 0;
}
char *web_inspect_computed(web_doc *d, web_node *n) {
    if (!n || n->type != N_ELEM) return NULL;
    return web_js_inspect_computed(n->owner ? n->owner : d, n);
}
