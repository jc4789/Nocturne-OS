/* Mutable nodes belong to the document, not to the layout or to a JS wrapper.
   Detached nodes stay valid until web_free; all mutation allocations are bounded. */
#include <stdio.h>
#include "webi.h"

#define DOM_MAX_DEPTH 400

static void indices(node_t *p) {
    int i = 0;
    for (node_t *c = p->first; c; c = c->next)
        if (c->type == N_ELEM) c->elem_index = ++i;
}

static bool under(node_t *n, node_t *ancestor) {
    for (; n; n = n->parent) if (n == ancestor) return true;
    return false;
}

static void changed(web_doc *d, node_t *n, bool resources) {
    if (!d) return;
    d->dirty = d->need_style = true;
    if (resources) d->resources_dirty = true;
    d->dom_revision++;
    d->find_node = NULL;
    d->find_run = NULL;
    if (n) n->resource_revision++;
    /* SVG cache source and the box tree are snapshots, not a second DOM. */
    for (int i = 0; i < d->svgs.n; i++) {
        struct svg_cache *s = d->svgs.v[i];
        if (s->img) image_free(s->img);
        free(s->src);
        free(s);
    }
    d->svgs.n = 0;
}
void doc_mutated(web_doc *d, node_t *n) { changed(d, n, true); }

node_t *doc_node_create(web_doc *d, int type, const char *name, const char *text, size_t len) {
    if (!d || type < N_DOC || type > N_FRAGMENT || len > (16u << 20)) return NULL;
    if (type == N_ELEM && (!name || !*name || strlen(name) >= 64)) return NULL;
    jmp_buf trap;
    jmp_buf *old = d->mem.trap;
    d->mem.trap = &trap;
    if (setjmp(trap)) { d->mem.trap = old; return NULL; }
    node_t *n = ar_alloc(&d->mem, sizeof *n);
    n->type = (uint8_t)type;
    n->image = -1;
    n->owned_next = d->owned_nodes;
    d->owned_nodes = n;
    if (type == N_ELEM) {
        char lower_name[64];
        size_t l = strlen(name);
        for (size_t i = 0; i < l; i++) lower_name[i] = (char)lower((unsigned char)name[i]);
        lower_name[l] = 0;
        n->tag = (uint16_t)tag_lookup(lower_name, l);
        n->name = n->tag ? tag_names[n->tag] : ar_strndup(&d->mem, lower_name, l);
        n->raw_name = ar_strdup(&d->mem, name);
    } else if (type == N_TEXT || type == N_COMMENT) {
        n->text = ar_strndup(&d->mem, text ? text : "", text ? len : 0);
        n->textlen = text ? len : 0;
    }
    d->mem.trap = old;
    return n;
}

static void attribute_cache(web_doc *d, node_t *n) {
    n->id = NULL;
    n->classes = NULL;
    n->nclasses = 0;
    const char *id = node_attr(n, "id"), *classes = node_attr(n, "class");
    if (id && *id) n->id = id;
    if (!classes) return;
    int count = 0;
    for (const char *p = classes; *p;) {
        while (is_space((unsigned char)*p)) p++;
        if (!*p) break;
        count++;
        while (*p && !is_space((unsigned char)*p)) p++;
    }
    if (!count) return;
    n->classes = ar_alloc(&d->mem, (size_t)count * sizeof *n->classes);
    for (const char *p = classes; *p;) {
        while (is_space((unsigned char)*p)) p++;
        if (!*p) break;
        const char *start = p;
        while (*p && !is_space((unsigned char)*p)) p++;
        n->classes[n->nclasses++] = ar_strndup(&d->mem, start, (size_t)(p - start));
    }
}

bool doc_node_attr(web_doc *d, node_t *n, const char *name, const char *value) {
    if (!d || !n || n->type != N_ELEM || !name || !*name || strlen(name) >= 128 ||
        (value && strlen(value) > (16u << 20))) return false;
    char low[128];
    size_t l = strlen(name);
    for (size_t i = 0; i < l; i++) low[i] = (char)lower((unsigned char)name[i]);
    low[l] = 0;
    int found = -1;
    for (int i = 0; i < n->nattrs; i++) if (!strcmp(n->attrs[i].name, low)) { found = i; break; }
    if (found < 0 && !value) return true;
    bool resources = n->tag == T_base || n->tag == T_link || n->tag == T_style ||
                     n->tag == T_img || n->tag == T_input || n->tag == T_meta;
    /* Keep JS attribute/CE reactions in native_dom, but an identical ordinary
       attribute has no new style to cascade and needs no arena allocation. */
    if (!resources && found >= 0 && value && !strcmp(n->attrs[found].value, value)) return true;
    int count = n->nattrs + (found < 0 ? 1 : value ? 0 : -1);
    if (count > 1024) return false;
    jmp_buf trap;
    jmp_buf *old = d->mem.trap;
    d->mem.trap = &trap;
    if (setjmp(trap)) { d->mem.trap = old; return false; }
    struct attr *attrs = ar_alloc(&d->mem, sizeof *attrs * (size_t)count);
    int at = 0;
    for (int i = 0; i < n->nattrs; i++) {
        if (i == found) {
            if (!value) continue;
            attrs[at] = n->attrs[i];
            attrs[at++].value = ar_strdup(&d->mem, value);
        } else attrs[at++] = n->attrs[i];
    }
    if (found < 0) attrs[at++] = (struct attr){ar_strdup(&d->mem, low),
                                                              ar_strdup(&d->mem, name),
                                                              ar_strdup(&d->mem, value)};
    node_t temp = *n;
    temp.attrs = attrs;
    temp.nattrs = count;
    attribute_cache(d, &temp);
    /* Publish only after every allocation succeeds. */
    n->attrs = temp.attrs;
    n->nattrs = temp.nattrs;
    n->id = temp.id;
    n->classes = temp.classes;
    n->nclasses = temp.nclasses;
    d->mem.trap = old;
    if (n->control_ready && n->tag == T_input) {
        if (!strcmp(low, "value") && !n->value_dirty) {
            const char *v = value ? value : "";
            if (!doc_node_value(d, n, v, strlen(v))) { doc_mutated(d, n); return false; }
            n->value_dirty = false;
        } else if (!strcmp(low, "checked") && !n->checked_dirty) {
            doc_control_checked(d, n, value != NULL);
            n->checked_dirty = false;
        }
    }
    /* An ordinary class/style/ARIA attribute affects the cascade, not the set
       of authored sheets. Re-parsing every external CSS sheet here made geometry
       reads after classList updates needlessly rebuild all sheet snapshots. */
    changed(d, n, resources);
    return true;
}

static void detach(node_t *n) {
    node_t *p = n->parent;
    if (!p) return;
    if (n->prev) n->prev->next = n->next; else p->first = n->next;
    if (n->next) n->next->prev = n->prev; else p->last = n->prev;
    n->parent = n->prev = n->next = NULL;
    indices(p);
}

static int tree_depth(node_t *n, int depth) {
    if (depth > DOM_MAX_DEPTH) return depth;
    int max = depth;
    for (node_t *c = n->first; c; c = c->next) {
        int h = tree_depth(c, depth + 1);
        if (h > max) max = h;
        if (max > DOM_MAX_DEPTH) break;
    }
    return max;
}

static bool may_insert(node_t *p, node_t *c) {
    if (!p || !c || p == c || under(p, c) || c->type == N_DOC ||
        (p->type != N_DOC && p->type != N_ELEM && p->type != N_FRAGMENT)) return false;
    int depth = 0;
    for (node_t *n = p; n; n = n->parent) depth++;
    if (tree_depth(c, depth) > DOM_MAX_DEPTH) return false;
    if (p->type == N_DOC) {
        if (c->type == N_TEXT) return false;
        if (c->type == N_ELEM) for (node_t *n = p->first; n; n = n->next)
            if (n != c && n->type == N_ELEM) return false;
    }
    return true;
}

bool doc_node_move(web_doc *d, node_t *p, node_t *c, node_t *before) {
    if (!d || !p || !c || (before && before->parent != p)) return false;
    if (before == c) return true;
    if (c->type == N_FRAGMENT) {
        int elements = 0;
        for (node_t *n = c->first; n; n = n->next) {
            if (!may_insert(p, n)) return false;
            if (n->type == N_ELEM) elements++;
        }
        if (p->type == N_DOC && elements > 1) return false;
        while (c->first) if (!doc_node_move(d, p, c->first, before)) return false;
        return true;
    }
    if (!may_insert(p, c)) return false;
    detach(c);
    c->parent = p;
    c->next = before;
    c->prev = before ? before->prev : p->last;
    if (c->prev) c->prev->next = c; else p->first = c;
    if (before) before->prev = c; else p->last = c;
    indices(p);
    doc_mutated(d, p);
    return true;
}

void doc_node_remove(web_doc *d, node_t *n) {
    if (!d || !n || !n->parent) return;
    node_t *p = n->parent;
    if (under(d->focus, n)) { d->focus = NULL; d->caret = 0; }
    detach(n);
    doc_mutated(d, p);
}

bool doc_node_text(web_doc *d, node_t *n, const char *text, size_t len) {
    if (!d || !n || len > (16u << 20)) return false;
    if (n->type == N_DOC) return true;
    if (n->type == N_TEXT || n->type == N_COMMENT) {
        jmp_buf trap;
        jmp_buf *old = d->mem.trap;
        d->mem.trap = &trap;
        if (setjmp(trap)) { d->mem.trap = old; return false; }
        char *v = ar_strndup(&d->mem, text, len);
        n->text = v;
        n->textlen = len;
        d->mem.trap = old;
    } else {
        node_t *t = len ? doc_node_create(d, N_TEXT, NULL, text, len) : NULL;
        if (len && !t) return false;
        while (n->first) doc_node_remove(d, n->first);
        if (t && !doc_node_move(d, n, t, NULL)) return false;
    }
    doc_mutated(d, n);
    return true;
}

bool doc_node_html(web_doc *d, node_t *n, const char *html, size_t len) {
    if (!d || !n || (n->type != N_ELEM && n->type != N_FRAGMENT) || len > (16u << 20)) return false;
    node_t *fragment = html_fragment(d, n, html, len);
    if (!fragment) return false;
    for (node_t *c = fragment->first; c; c = c->next) if (!may_insert(n, c)) return false;
    while (n->first) doc_node_remove(d, n->first);
    return doc_node_move(d, n, fragment, NULL);
}

bool doc_node_value(web_doc *d, node_t *n, const char *text, size_t len) {
    if (!d || !n || len > (16u << 20) || (len && !text)) return false;
    size_t next = d->control_bytes - n->value_capacity;
    if (d->live && (len + 1 > (32u << 20) || next > (32u << 20) - len - 1 ||
                    d->mem.allocated > (32u << 20) - next - len - 1)) return false;
    char *value = malloc(len + 1);
    if (!value) return false;
    if (len) memcpy(value, text, len);
    value[len] = 0;
    free(n->value);
    n->value = value;
    n->value_capacity = len + 1;
    n->value_dirty = true;
    d->control_bytes = next + len + 1;
    if (d->live) d->mem.limit = (32u << 20) - d->control_bytes;
    d->dirty = d->need_style = true;
    return true;
}

node_t *doc_node_clone(web_doc *d, node_t *n, bool deep) {
    if (!d || !n || n->type == N_DOC) return NULL;
    node_t *c = doc_node_create(d, n->type, n->raw_name ? n->raw_name : n->name, n->text, n->textlen);
    if (!c) return NULL;
    c->foreign = n->foreign;
    for (int i = 0; i < n->nattrs; i++)
        if (!doc_node_attr(d, c, n->attrs[i].raw, n->attrs[i].value)) return NULL;
    c->checked = n->checked;
    c->selected = n->selected;
    c->selected_set = n->selected_set;
    c->control_ready = n->control_ready;
    if (n->value && !doc_node_value(d, c, n->value, strlen(n->value))) return NULL;
    c->value_dirty = n->value_dirty;
    c->checked_dirty = n->checked_dirty;
    c->script_started = n->tag == T_script;
    if (deep) for (node_t *ch = n->first; ch; ch = ch->next) {
        node_t *copy = doc_node_clone(d, ch, true);
        if (!copy || !doc_node_move(d, c, copy, NULL)) return NULL;
    }
    return c;
}
