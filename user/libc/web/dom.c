/* Mutable nodes belong to the document, not to the layout or to a JS wrapper.
   Detached nodes stay valid until web_free; all mutation allocations are bounded. */
#include <stdio.h>
#include "nocturne.h"
#include "webi.h"
#include "web_dialog.h"
#include "form_value.h"
#include "form_file.h"
#include "elements.h"
#include "js_canvas.h"

#define DOM_MAX_DEPTH 400

static void indices(node_t *p) {
    web_doc *d = p->owner;
    if (d && d->dom_family) d = d->dom_family;
    uint64_t start = uptime_ms(), visited = 0;
    int i = 0;
    for (node_t *c = p->first; c; c = c->next) {
        visited++;
        if (c->type == N_ELEM) c->elem_index = ++i;
    }
    if (d) { d->profile.index_visits += visited; d->profile.index_ms += uptime_ms() - start; }
}

static bool under(node_t *n, node_t *ancestor) {
    for (; n; n = n->parent) if (n == ancestor) return true;
    return false;
}

node_t *doc_shadow_parent(node_t *n) {
    return n ? (n->parent ? n->parent : n->shadow_host) : NULL;
}
node_t *doc_node_root(node_t *n, bool composed) {
    if (!n) return NULL;
    node_t *p;
    while ((p = composed ? doc_shadow_parent(n) : n->parent)) n = p;
    return n;
}
bool doc_node_connected(node_t *n) {
    node_t *root = doc_node_root(n, true);
    return root && root->type == N_DOC;
}
static bool shadow_under(node_t *n, node_t *ancestor) {
    for (; n; n = doc_shadow_parent(n)) if (n == ancestor) return true;
    return false;
}
static bool is_slot(const node_t *n) {
    return n && n->type == N_ELEM && !n->foreign && n->tag == T_slot;
}
static bool slottable(const node_t *n) { return n && (n->type == N_ELEM || n->type == N_TEXT); }
void doc_slot_signal(node_t *slot) {
    if (!is_slot(slot) || slot->slot_change_pending || !slot->owner) return;
    web_doc *family = slot->owner->dom_family ? slot->owner->dom_family : slot->owner;
    slot->slot_change_pending = true; slot->slot_change_next = NULL;
    if (family->shadow_slots_last) family->shadow_slots_last->slot_change_next = slot;
    else family->shadow_slots_first = slot;
    family->shadow_slots_last = slot; family->shadow_slots_pending = true;
}
static const char *slot_name(node_t *n, bool slot) {
    const char *name = n->type == N_ELEM ? node_attr(n, slot ? "name" : "slot") : NULL;
    return name ? name : "";
}
static node_t *first_named_slot(node_t *root, const char *name) {
    for (node_t *n = root->first; n; n = n->next) {
        if (is_slot(n) && !strcmp(slot_name(n, true), name)) return n;
        node_t *found = first_named_slot(n, name);
        if (found) return found;
    }
    return NULL;
}
node_t *doc_assigned_slot(node_t *n, bool open_only) {
    if (!slottable(n) || !n->parent || !n->parent->shadow_root) return NULL;
    node_t *root = n->parent->shadow_root;
    if (open_only && root->shadow_closed) return NULL;
    if (root->shadow_manual) {
        node_t *slot = n->manual_slot;
        return slot && doc_node_root(slot, false) == root ? slot : NULL;
    }
    return first_named_slot(root, slot_name(n, false));
}
/* Keep assignment snapshots as intrusive native lists, without arena churn or
   a parallel JS DOM. All comparisons happen before rewriting any next link. */
static node_t *allocation_next(web_doc *family, web_doc **allocation, node_t *n) {
    if (n && n->owned_next) return n->owned_next;
    do {
        *allocation = *allocation == family ? family->dom_docs : (*allocation)->dom_next;
    } while (*allocation && !(*allocation)->owned_nodes);
    return *allocation ? (*allocation)->owned_nodes : NULL;
}
static node_t *slot_candidate(node_t *slot, node_t *after) {
    node_t *root = doc_node_root(slot, false);
    if (!root || !root->shadow_host) return NULL;
    if (root->shadow_manual) {
        for (node_t *n = after ? after->manual_next : slot->slot_manual_first; n; n = n->manual_next)
            if (n->parent == root->shadow_host) return n;
    } else {
        for (node_t *n = after ? after->next : root->shadow_host->first; n; n = n->next)
            if (doc_assigned_slot(n, false) == slot) return n;
    }
    return NULL;
}
static void compare_slot(node_t *slot) {
    node_t *old = slot->slot_assigned_first, *wanted = slot_candidate(slot, NULL);
    while (old && wanted && old == wanted) { old = old->assigned_next; wanted = slot_candidate(slot, wanted); }
    if (old || wanted) doc_slot_signal(slot);
}
static void compare_shadow_slots(node_t *root) {
    for (node_t *n = root->first; n; n = n->next) {
        if (is_slot(n)) compare_slot(n);
        compare_shadow_slots(n);
    }
}
void doc_shadow_reassign(web_doc *d) {
    if (!d) return;
    web_doc *family = d->dom_family ? d->dom_family : d;
    if (!family->has_shadow) return;
    family->profile.shadow_reassigns++;
    web_doc *allocation = family;
    node_t *n = family->owned_nodes;
    if (!n) n = allocation_next(family, &allocation, NULL);
    for (; n; n = allocation_next(family, &allocation, n)) {
        if (n->shadow_host) compare_shadow_slots(n); /* standard tree-order signals, not allocation order */
        else if (is_slot(n) && !doc_node_root(n, false)->shadow_host) compare_slot(n);
    }
    allocation = family; n = family->owned_nodes;
    if (!n) n = allocation_next(family, &allocation, NULL);
    for (; n; n = allocation_next(family, &allocation, n)) {
        n->assigned_slot = n->assigned_next = NULL;
        n->slot_assigned_first = n->slot_assigned_last = NULL;
    }
    allocation = family; n = family->owned_nodes;
    if (!n) n = allocation_next(family, &allocation, NULL);
    for (; n; n = allocation_next(family, &allocation, n)) if (is_slot(n)) {
        for (node_t *c = slot_candidate(n, NULL); c; c = slot_candidate(n, c)) {
            c->assigned_slot = n;
            if (n->slot_assigned_last) n->slot_assigned_last->assigned_next = c;
            else n->slot_assigned_first = c;
            n->slot_assigned_last = c;
        }
    }
}
bool doc_shadow_host_valid(const node_t *n) {
    if (!n || n->type != N_ELEM || n->foreign || n->namespace_id != NS_HTML) return false;
    /* HTML's valid shadow-host names plus autonomous custom element names. */
    switch (n->tag) {
    case T_article: case T_aside: case T_blockquote: case T_body: case T_div:
    case T_footer: case T_h1: case T_h2: case T_h3: case T_h4: case T_h5: case T_h6:
    case T_header: case T_main: case T_nav: case T_p: case T_section: case T_span: return true;
    default: break;
    }
    const char *name = n->name;
    if (!name || *name < 'a' || *name > 'z' || !strchr(name, '-')) return false;
    static const char *const excluded[] = {"annotation-xml", "color-profile", "font-face", "font-face-src",
        "font-face-uri", "font-face-format", "font-face-name", "missing-glyph"};
    for (unsigned i = 0; i < sizeof excluded / sizeof *excluded; i++) if (!strcmp(name, excluded[i])) return false;
    /* Current HTML uses DOM's ASCII-letter local-name branch, not PCENChar. */
    for (const unsigned char *p = (const unsigned char *)name; *p; p++)
        if (is_space(*p) || *p == '/' || *p == '>' || (*p >= 'A' && *p <= 'Z')) return false;
    return true;
}
node_t *doc_shadow_attach(web_doc *d, node_t *host, bool closed, bool delegates_focus,
                         bool clonable, bool serializable, bool manual) {
    if (!d || !doc_shadow_host_valid(host) || host->shadow_root || host->owner != d) return NULL;
    int depth = 0;
    for (node_t *n = host; n; n = doc_shadow_parent(n)) if (++depth >= DOM_MAX_DEPTH) return NULL;
    node_t *root = doc_node_create(d, N_FRAGMENT, NULL, NULL, 0);
    if (!root) return NULL;
    root->shadow_host = host; host->shadow_root = root;
    root->shadow_closed = closed; root->shadow_delegates_focus = delegates_focus;
    root->shadow_clonable = clonable; root->shadow_serializable = serializable; root->shadow_manual = manual;
    (d->dom_family ? d->dom_family : d)->has_shadow = true;
    doc_mutated(d, host);
    doc_shadow_reassign(d);
    return root;
}
static void manual_unlink(node_t *n) {
    node_t *slot = n->manual_slot;
    if (!slot) return;
    node_t *prev = NULL;
    for (node_t *c = slot->slot_manual_first; c; c = c->manual_next) {
        if (c == n) {
            if (prev) prev->manual_next = c->manual_next; else slot->slot_manual_first = c->manual_next;
            if (slot->slot_manual_last == c) slot->slot_manual_last = prev;
            break;
        }
        prev = c;
    }
    n->manual_slot = n->manual_next = NULL;
}
bool doc_slot_assign(node_t *slot, node_t **nodes, int count) {
    if (!is_slot(slot) || count < 0 || (count && !nodes)) return false;
    for (int i = 0; i < count; i++) if (!slottable(nodes[i])) return false;
    while (slot->slot_manual_first) manual_unlink(slot->slot_manual_first);
    for (int i = 0; i < count; i++) {
        node_t *n = nodes[i];
        if (n->manual_slot == slot) continue; /* ordered-set duplicate */
        manual_unlink(n);
        n->manual_slot = slot;
        if (slot->slot_manual_last) slot->slot_manual_last->manual_next = n; else slot->slot_manual_first = n;
        slot->slot_manual_last = n;
    }
    doc_mutated(slot->owner, slot);
    doc_shadow_reassign(slot->owner);
    return true;
}
static void slot_nodes(node_t *slot, bool flatten, pvec *out, int depth) {
    if (!is_slot(slot) || depth > DOM_MAX_DEPTH) return;
    node_t *root = doc_node_root(slot, false);
    if (!root || !root->shadow_host) return;
    node_t *first = slot->slot_assigned_first;
    bool assigned = first != NULL;
    for (node_t *n = first ? first : flatten ? slot->first : NULL; n; n = assigned ? n->assigned_next : n->next) {
        if (!slottable(n)) continue;
        node_t *nr = is_slot(n) ? doc_node_root(n, false) : NULL;
        if (flatten && nr && nr->shadow_host) slot_nodes(n, true, out, depth + 1);
        else pv_push(out, n);
    }
}
void doc_slot_nodes(node_t *slot, bool flatten, pvec *out) { if (out) slot_nodes(slot, flatten, out, 0); }
void doc_flat_children(node_t *n, pvec *out) {
    if (!n || !out) return;
    if (n->shadow_root) n = n->shadow_root;
    node_t *root = is_slot(n) ? doc_node_root(n, false) : NULL;
    if (root && root->shadow_host && n->slot_assigned_first) {
        for (node_t *c = n->slot_assigned_first; c; c = c->assigned_next) pv_push(out, c);
    } else for (node_t *c = n->first; c; c = c->next) pv_push(out, c);
}
node_t *doc_flat_parent(node_t *n) {
    if (!n || n->shadow_host) return NULL;
    node_t *p = n->parent;
    if (!p) return NULL;
    if (p->shadow_root) return doc_assigned_slot(n, false);
    if (p->shadow_host) return p->shadow_host;
    if (is_slot(p) && p->slot_assigned_first && doc_node_root(p, false)->shadow_host) return NULL;
    return p;
}

static void textarea_changed(node_t *n) {
    for (; n; n = n->parent)
        if (n->type == N_ELEM && !n->foreign && n->tag == T_textarea && !n->value_dirty) n->control_ready = false;
}

static void changed(web_doc *d, node_t *n, bool resources) {
    if (!d) return;
    d->dirty = d->need_style = true;
    if (resources) d->resources_dirty = true;
    d->dom_revision++;
    d->find_node = NULL;
    d->find_run = NULL;
    if (n) n->resource_revision++;
    web_dialog_sync(d);
    textarea_changed(n);
    /* SVG cache source and the box tree are snapshots, not a second DOM. */
    for (int i = 0; i < d->svgs.n; i++) {
        struct svg_cache *s = d->svgs.v[i];
        if (s->img) image_free(s->img);
        free(s->src);
        free(s);
    }
    d->svgs.n = 0;
}
void doc_mutated(web_doc *d, node_t *n) {
    changed(d, n, true);
    /* NULL denotes a parser's complete forest transaction. Ordinary control or
       CharacterData notifications cannot change assignment; structural/attribute
       mutations below identify the exact assignment-relevant boundary instead. */
    if (!n) doc_shadow_reassign(d);
}

/* XML 1.0 Name, required by the DOM PI creation APIs. HTML tokenization has
   its own narrower target rules; never normalize a DOM-created PI target. */
static bool xml_name_start(uint32_t c) {
    return c == ':' || c == '_' || (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
        (c >= 0xc0 && c <= 0xd6) || (c >= 0xd8 && c <= 0xf6) || (c >= 0xf8 && c <= 0x2ff) ||
        (c >= 0x370 && c <= 0x37d) || (c >= 0x37f && c <= 0x1fff) || (c >= 0x200c && c <= 0x200d) ||
        (c >= 0x2070 && c <= 0x218f) || (c >= 0x2c00 && c <= 0x2fef) || (c >= 0x3001 && c <= 0xd7ff) ||
        (c >= 0xf900 && c <= 0xfdcf) || (c >= 0xfdf0 && c <= 0xfffd) || (c >= 0x10000 && c <= 0xeffff);
}
bool doc_pi_target_valid(const char *target, size_t len) {
    if (!target || !len) return false;
    size_t i = 0;
    bool first = true;
    while (i < len) {
        uint32_t c = (unsigned char)target[i++], min = 0;
        unsigned extra = 0;
        if (c >= 0xc2 && c <= 0xdf) { c &= 0x1f; extra = 1; min = 0x80; }
        else if (c >= 0xe0 && c <= 0xef) { c &= 0x0f; extra = 2; min = 0x800; }
        else if (c >= 0xf0 && c <= 0xf4) { c &= 7; extra = 3; min = 0x10000; }
        else if (c >= 0x80) return false;
        if (extra > len - i) return false;
        while (extra--) {
            unsigned char tail = (unsigned char)target[i++];
            if ((tail & 0xc0) != 0x80) return false;
            c = (c << 6) | (tail & 0x3f);
        }
        if (c < min || c > 0x10ffff || (c >= 0xd800 && c <= 0xdfff)) return false;
        bool valid = xml_name_start(c);
        if (!first) valid = valid || c == '-' || c == '.' || (c >= '0' && c <= '9') || c == 0xb7 ||
            (c >= 0x300 && c <= 0x36f) || (c >= 0x203f && c <= 0x2040);
        if (!valid) return false;
        first = false;
    }
    return true;
}

node_t *doc_node_create(web_doc *d, int type, const char *name, const char *text, size_t len) {
    if (!d || type < N_DOC || type > N_ATTR || len > (16u << 20)) return NULL;
    if (type == N_ELEM && (!name || !*name || strlen(name) >= 64)) return NULL;
    if (type == N_PI && (!name || !doc_pi_target_valid(name, strlen(name)))) return NULL;
    doc_dom_budget(d);
    jmp_buf trap;
    jmp_buf *old = d->mem.trap;
    d->mem.trap = &trap;
    if (setjmp(trap)) { d->mem.trap = old; return NULL; }
    node_t *n = ar_alloc(&d->mem, sizeof *n);
    n->type = (uint8_t)type;
    n->owner = n->allocation_doc = d;
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
    } else if (type == N_DOCTYPE) {
        n->name = ar_strdup(&d->mem, name ? name : "html");
    } else if (type == N_TEXT || type == N_COMMENT || type == N_PI) {
        if (type == N_PI) n->name = ar_strdup(&d->mem, name);
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
    const char *id = NULL, *classes = NULL;
    for (int i = 0; i < n->nattrs; i++) {
        if (n->attrs[i].namespace_uri) continue;
        if (!strcmp(n->attrs[i].raw, "id")) id = n->attrs[i].value;
        if (!strcmp(n->attrs[i].raw, "class")) classes = n->attrs[i].value;
    }
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

/* The element array remains the renderer/parser's canonical data. An Attr
   wrapper points into that array; old arena slots survive detachment. */
void doc_attrs_publish(node_t *n, struct attr *attrs, int count) {
    for (int i = 0; i < n->nattrs; i++) if (n->attrs[i].node) {
        node_t *identity = n->attrs[i].node;
        bool retained = false;
        for (int j = 0; j < count; j++) if (attrs[j].node == identity) { retained = true; break; }
        if (!retained) identity->attr_owner = NULL;
    }
    n->attrs = attrs; n->nattrs = count;
    for (int i = 0; i < count; i++) if (attrs[i].node) {
        attrs[i].node->attribute = &attrs[i];
        attrs[i].node->attr_owner = n;
        attrs[i].node->owner = n->owner;
    }
}
int doc_attr_index(node_t *n, const char *ns, const char *name, bool namespaced) {
    if (!n || n->type != N_ELEM || !name) return -1;
    if (ns && !*ns) ns = NULL;
    for (int i = 0; i < n->nattrs; i++) {
        struct attr *a = &n->attrs[i];
        if (namespaced) {
            if ((ns ? a->namespace_uri && !strcmp(ns, a->namespace_uri) : !a->namespace_uri) &&
                !strcmp(name, a->local ? a->local : a->raw)) return i;
        } else {
            const char *p = name, *q = a->raw;
            while (*p && *q && (n->foreign ? *p : lower((unsigned char)*p)) == (unsigned char)*q) { p++; q++; }
            if (!*p && !*q) return i;
        }
    }
    return -1;
}
node_t *doc_attr_node(web_doc *d, node_t *n, int i) {
    if (!n || n->type != N_ELEM || i < 0 || i >= n->nattrs) return NULL;
    struct attr *a = &n->attrs[i];
    if (a->node) return a->node;
    node_t *made = doc_node_create(d, N_ATTR, NULL, NULL, 0);
    if (made) { a->node = made; made->attribute = a; made->attr_owner = n; }
    return made;
}
node_t *doc_attr_create(web_doc *d, const char *ns, const char *prefix, const char *local, const char *value) {
    if (!d || !local || !*local || strlen(local) >= 128 || (value && strlen(value) > (16u << 20))) return NULL;
    node_t *made = doc_node_create(d, N_ATTR, NULL, NULL, 0);
    if (!made) return NULL;
    jmp_buf trap; jmp_buf *old = d->mem.trap; d->mem.trap = &trap;
    if (setjmp(trap)) { d->mem.trap = old; return NULL; }
    struct attr *a = ar_alloc(&d->mem, sizeof *a);
    size_t pl = prefix ? strlen(prefix) : 0, ll = strlen(local);
    char *raw = ar_alloc(&d->mem, pl + ll + (pl ? 2 : 1));
    if (pl) { memcpy(raw, prefix, pl); raw[pl] = ':'; }
    memcpy(raw + pl + (pl ? 1 : 0), local, ll + 1);
    a->raw = raw;
    char *low = ar_strdup(&d->mem, raw);
    for (char *p = low; *p; p++) *p = (char)lower((unsigned char)*p);
    a->name = low; a->value = ar_strdup(&d->mem, value ? value : "");
    a->local = ar_strdup(&d->mem, local);
    a->namespace_uri = ns && *ns ? ar_strdup(&d->mem, ns) : NULL;
    a->prefix = prefix && *prefix ? ar_strdup(&d->mem, prefix) : NULL;
    a->node = made; made->attribute = a;
    d->mem.trap = old; return made;
}

static bool attribute_change(web_doc *d, node_t *n, int found, const char *name, const char *value,
                             const char *ns, const char *prefix, const char *local, node_t *identity) {
    if (!d || !n || n->type != N_ELEM || !name || !*name || strlen(name) >= 128 ||
        (value && strlen(value) > (16u << 20))) return false;
    char low[128];
    size_t l = strlen(name);
    for (size_t i = 0; i < l; i++) low[i] = (char)lower((unsigned char)name[i]);
    low[l] = 0;
    bool previously_selectable = doc_control_selection_supported(n);
    bool input_type_change = !ns && !n->foreign && n->tag == T_input &&
                             !strcmp(name, low) && !strcmp(low, "type");
    enum web_input_kind previous_type = web_input_type(n);
    if (input_type_change) doc_control_init(d, n);
    if (!ns && !n->foreign && n->tag == T_select && !strcmp(name, low) &&
        (!strcmp(low, "multiple") || !strcmp(low, "size"))) doc_control_init(d, n);
    if (found < 0 && !value) return true;
    bool resources = n->tag == T_base || n->tag == T_link || n->tag == T_style ||
                     n->tag == T_img || n->tag == T_input || n->tag == T_meta;
    /* Keep JS attribute/CE reactions in native_dom, but an identical ordinary
       attribute has no new style to cascade and needs no arena allocation. */
    if (!resources && !identity && found >= 0 && value && !strcmp(n->attrs[found].value, value)) {
        /* Canvasの寸法設定は同値でもbitmap/contextをresetする。arena再割当は不要。 */
        if (!ns && !n->foreign && n->tag == T_canvas && !strcmp(name, low) &&
            (!strcmp(low, "width") || !strcmp(low, "height"))) {
            web_canvas_attr_changed(n, local);
            d->dirty = true;
        }
        return true;
    }
    int count = n->nattrs + (found < 0 ? 1 : value ? 0 : -1);
    if (count > 1024) return false;
    doc_dom_budget(d);
    jmp_buf trap;
    jmp_buf *old = d->mem.trap;
    d->mem.trap = &trap;
    if (setjmp(trap)) { d->mem.trap = old; return false; }
    struct attr *attrs = ar_alloc(&d->mem, sizeof *attrs * (size_t)count);
    int at = 0;
    for (int i = 0; i < n->nattrs; i++) {
        if (i == found) {
            if (!value) continue;
            attrs[at] = identity ? *identity->attribute : n->attrs[i];
            attrs[at++].value = ar_strdup(&d->mem, value);
        } else attrs[at++] = n->attrs[i];
    }
    if (found < 0) {
        if (identity) attrs[at++] = *identity->attribute;
        else attrs[at++] = (struct attr){.name=ar_strdup(&d->mem, low),
            .raw=ar_strdup(&d->mem, name), .value=ar_strdup(&d->mem, value),
            .namespace_uri=ns && *ns ? ar_strdup(&d->mem, ns) : NULL,
            .prefix=prefix && *prefix ? ar_strdup(&d->mem, prefix) : NULL,
            .local=ar_strdup(&d->mem, local ? local : name)};
    }
    node_t temp = *n;
    temp.attrs = attrs;
    temp.nattrs = count;
    attribute_cache(d, &temp);
    /* Publish only after every allocation succeeds. */
    doc_attrs_publish(n, temp.attrs, temp.nattrs);
    n->id = temp.id;
    n->classes = temp.classes;
    n->nclasses = temp.nclasses;
    d->mem.trap = old;
    bool ordinary = !ns && !n->foreign && !strcmp(name, low);
    if (input_type_change && previous_type != web_input_type(n)) {
        if (previous_type == WEB_INPUT_FILE) web_input_files_clear(d, n);
        enum web_input_mode before = web_input_value_mode(previous_type);
        enum web_input_mode after = web_input_value_mode(web_input_type(n));
        bool dirty = n->value_dirty;
        if (before == WEB_INPUT_VALUE && (after == WEB_INPUT_DEFAULT || after == WEB_INPUT_DEFAULT_ON) && n->value && *n->value) {
            if (!doc_node_attr(d, n, "value", n->value)) { doc_mutated(d, n); return false; }
        } else if (before != WEB_INPUT_VALUE && after == WEB_INPUT_VALUE) {
            const char *v = node_attr(n, "value");
            if (!doc_node_value(d, n, v ? v : "", v ? strlen(v) : 0)) { doc_mutated(d, n); return false; }
            dirty = false;
        } else if (before != WEB_INPUT_FILENAME && after == WEB_INPUT_FILENAME) {
            if (!doc_node_value(d, n, "", 0)) { doc_mutated(d, n); return false; }
        }
        if (!doc_node_value(d, n, n->value ? n->value : "", n->value ? strlen(n->value) : 0)) {
            doc_mutated(d, n); return false;
        }
        n->value_dirty = dirty;
    }
    if (n->tag == T_input && ordinary && !strcmp(low, "type") && !previously_selectable && doc_control_selection_supported(n)) {
        n->selection_start = n->selection_end = 0; n->selection_direction = 0;
        n->selection_set = true;
        doc_control_caret(d, n);
    }
    if (ordinary && n->control_ready && n->tag == T_input) {
        if (!strcmp(low, "value") && !n->value_dirty) {
            const char *v = value ? value : "";
            if (!doc_node_value(d, n, v, strlen(v))) { doc_mutated(d, n); return false; }
            n->value_dirty = false;
        } else if (!strcmp(low, "checked") && !n->checked_dirty) {
            doc_control_checked(d, n, value != NULL);
            n->checked_dirty = false;
        } else if ((!strcmp(low, "multiple") && web_input_type(n) == WEB_INPUT_EMAIL) ||
                   (web_input_type(n) == WEB_INPUT_RANGE && (!strcmp(low, "min") || !strcmp(low, "max") || !strcmp(low, "step")))) {
            bool dirty = n->value_dirty;
            if (!doc_node_value(d, n, n->value ? n->value : "", n->value ? strlen(n->value) : 0)) {
                doc_mutated(d, n); return false;
            }
            n->value_dirty = dirty;
        }
    }
    /* An ordinary class/style/ARIA attribute affects the cascade, not the set
       of authored sheets. Re-parsing every external CSS sheet here made geometry
       reads after classList updates needlessly rebuild all sheet snapshots. */
    changed(d, n, resources);
    if (!ns && ((!strcmp(name, "slot") && n->parent && n->parent->shadow_root && !n->parent->shadow_root->shadow_manual) ||
        (!strcmp(name, "name") && is_slot(n) && doc_node_root(n, false)->shadow_host && !doc_node_root(n, false)->shadow_manual)))
        doc_shadow_reassign(d);
    if (!ns) doc_details_attribute_changed(d, n, local, found >= 0);
    if (ordinary) web_select_attribute_changed(d, n, local, value != NULL);
    if (ordinary) web_canvas_attr_changed(n, local);
    return true;
}

bool doc_node_attr(web_doc *d, node_t *n, const char *name, const char *value) {
    int found = doc_attr_index(n, NULL, name, false);
    if (found < 0 && !value) return true;
    const struct attr *a = found >= 0 ? &n->attrs[found] : NULL;
    char normalized[128];
    if (!name || strlen(name) >= sizeof normalized) return false;
    snprintf(normalized, sizeof normalized, "%s", name);
    if (n && !n->foreign) for (char *p = normalized; *p; p++) *p = (char)lower((unsigned char)*p);
    return attribute_change(d, n, found, a ? a->raw : normalized, value,
        a ? a->namespace_uri : NULL, a ? a->prefix : NULL, a ? a->local : normalized, NULL);
}
bool doc_attr_set_ns(web_doc *d, node_t *n, const char *ns, const char *prefix, const char *local, const char *value) {
    int found = doc_attr_index(n, ns, local, true);
    if (found >= 0) {
        struct attr *a = &n->attrs[found];
        return attribute_change(d, n, found, a->raw, value, a->namespace_uri, a->prefix, a->local, NULL);
    }
    char name[128];
    int len = prefix ? snprintf(name, sizeof name, "%s:%s", prefix, local) : snprintf(name, sizeof name, "%s", local);
    return len > 0 && (size_t)len < sizeof name && attribute_change(d, n, -1, name, value, ns, prefix, local, NULL);
}
bool doc_attr_set_node(web_doc *d, node_t *n, node_t *attribute) {
    if (!n || n->type != N_ELEM || !attribute || attribute->type != N_ATTR ||
        (attribute->attr_owner && attribute->attr_owner != n)) return false;
    struct attr *a = attribute->attribute;
    int found = doc_attr_index(n, a->namespace_uri, a->local ? a->local : a->raw, true);
    if (found >= 0 && n->attrs[found].node == attribute) return true;
    return attribute_change(d, n, found, a->raw, a->value, a->namespace_uri, a->prefix, a->local, attribute);
}
bool doc_attr_remove(web_doc *d, node_t *n, int i) {
    if (!n || i < 0 || i >= n->nattrs) return true;
    struct attr *a = &n->attrs[i];
    return attribute_change(d, n, i, a->raw, NULL, a->namespace_uri, a->prefix, a->local, NULL);
}
bool doc_attr_value(web_doc *d, node_t *n, const char *value) {
    if (!n || n->type != N_ATTR || !n->attribute || !value || strlen(value) > (16u << 20)) return false;
    if (n->attr_owner) {
        struct attr *a = n->attribute;
        return doc_attr_set_ns(d, n->attr_owner, a->namespace_uri, a->prefix, a->local ? a->local : a->raw, value);
    }
    doc_dom_budget(d);
    jmp_buf trap; jmp_buf *old = d->mem.trap; d->mem.trap = &trap;
    if (setjmp(trap)) { d->mem.trap = old; return false; }
    n->attribute->value = ar_strdup(&d->mem, value);
    d->mem.trap = old; return true;
}

static void detach(node_t *n) {
    node_t *p = n->parent;
    if (!p) return;
    if (n->prev) n->prev->next = n->next; else p->first = n->next;
    if (n->next) n->next->prev = n->prev; else p->last = n->prev;
    n->parent = n->prev = n->next = NULL;
    indices(p);
    textarea_changed(p);
}

static int tree_depth(node_t *n, int depth) {
    if (depth > DOM_MAX_DEPTH) return depth;
    int max = depth;
    for (node_t *c = n->first; c; c = c->next) {
        int h = tree_depth(c, depth + 1);
        if (h > max) max = h;
        if (max > DOM_MAX_DEPTH) break;
    }
    if (n->shadow_root && max <= DOM_MAX_DEPTH) {
        int h = tree_depth(n->shadow_root, depth + 1);
        if (h > max) max = h;
    }
    return max;
}

static bool may_insert(node_t *p, node_t *c) {
    if (!p || !c || p == c || under(p, c) || c->type == N_DOC || c->type == N_ATTR ||
        (p->type != N_DOC && p->type != N_ELEM && p->type != N_FRAGMENT)) return false;
    for (node_t *n = p; n; n = n->parent ? n->parent : n->shadow_host ? n->shadow_host : n->template_host) if (n == c) return false;
    int depth = 0;
    for (node_t *n = p; n; n = doc_shadow_parent(n)) depth++;
    if (tree_depth(c, depth) > DOM_MAX_DEPTH) return false;
    if (p->type == N_DOC) {
        if (c->type == N_TEXT) return false;
        if (c->type == N_ELEM) for (node_t *n = p->first; n; n = n->next)
            if (n != c && n->type == N_ELEM) return false;
        if (c->type == N_DOCTYPE) for (node_t *n = p->first; n; n = n->next)
            if (n != c && n->type == N_DOCTYPE) return false;
    } else if (c->type == N_DOCTYPE) return false;
    return true;
}

static bool assignment_structure(node_t *p, node_t *c) {
    if (!p || !c) return false;
    if (p->shadow_root && slottable(c)) return true;
    /* Element insertions/removals can add, remove or reorder descendant slots.
       Text/comment/PI changes within a shadow tree cannot change slot matching. */
    return c->type == N_ELEM && doc_node_root(p, false)->shadow_host;
}

bool doc_node_move(web_doc *d, node_t *p, node_t *c, node_t *before) {
    if (!d || !p || !c || (before && before->parent != p)) return false;
    (d->dom_family ? d->dom_family : d)->profile.inserts++;
    if (before == c) return true;
    if (c->type == N_FRAGMENT) {
        for (node_t *n = p; n; n = n->parent ? n->parent : n->shadow_host ? n->shadow_host : n->template_host)
            if (n == c) return false;
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
    if (c->owner != d && !doc_node_adopt(d, c)) return false;
    node_t *old_parent = c->parent;
    node_t *old_select = node_ancestor(old_parent, T_select);
    bool assignment_changed = assignment_structure(old_parent, c) || assignment_structure(p, c);
    if (is_slot(old_parent) && !old_parent->slot_assigned_first && doc_node_root(old_parent, false)->shadow_host)
        doc_slot_signal(old_parent);
    if(old_parent)web_dialog_removed(d,c);
    detach(c);
    c->parent = p;
    c->next = before;
    c->prev = before ? before->prev : p->last;
    if (c->prev) c->prev->next = c; else p->first = c;
    if (before) before->prev = c; else p->last = c;
    indices(p);
    if (is_slot(p) && !p->slot_assigned_first && doc_node_root(p, false)->shadow_host)
        doc_slot_signal(p);
    doc_mutated(d, p);
    if (assignment_changed) doc_shadow_reassign(d);
    doc_details_inserted(c);
    node_t *new_select = node_ancestor(p, T_select);
    web_select_inserted(d, c, old_parent);
    if (old_select && old_select != new_select) web_select_sync(old_select->owner, old_select, false);
    if (new_select) web_select_sync(new_select->owner, new_select, false);
    return true;
}

void doc_node_remove(web_doc *d, node_t *n) {
    if (!d || !n || !n->parent) return;
    node_t *p = n->parent;
    node_t *select = node_ancestor(p, T_select);
    bool assignment_changed = assignment_structure(p, n);
    if (shadow_under(d->focus, n)) { d->focus = NULL; d->caret = 0; }
    if (is_slot(p) && !p->slot_assigned_first && doc_node_root(p, false)->shadow_host)
        doc_slot_signal(p);
    detach(n);
    doc_mutated(d, p);
    if (assignment_changed) doc_shadow_reassign(d);
    web_select_inserted(d, n, p);
    if (select) web_select_sync(select->owner, select, false);
}

static void adopt_subtree(web_doc *d, node_t *n) {
    n->owner = d;
    for (int i = 0; i < n->nattrs; i++) if (n->attrs[i].node) n->attrs[i].node->owner = d;
    n->style = NULL; n->box = n->anchor_block = NULL;
    n->image = -1; n->image_request = -1; n->image_initialized = false;
    n->image_generation++;
    /* Adoption is an image-data mutation even without a later insertion or
       getter. Keep allocation ownership untouched; schedule the live scan. */
    if (!d->inert && n->type == N_ELEM && !n->foreign && n->tag == T_img)
        d->resources_dirty = d->dirty = d->need_style = true;
    for (node_t *c = n->first; c; c = c->next) adopt_subtree(d, c);
    if (n->shadow_root) {
        (d->dom_family ? d->dom_family : d)->has_shadow = true;
        adopt_subtree(d, n->shadow_root);
    }
    if (n->template_content) {
        web_doc *owner = d->template_owner ? d : d->template_doc;
        if (!owner) {
            owner = doc_inert(d, "", 0, "about:blank");
            if (owner) {
                while (owner->root->first) doc_node_remove(owner, owner->root->first);
                owner->template_owner = true; d->template_doc = owner;
            }
        }
        if (owner) adopt_subtree(owner, n->template_content);
    }
}
static bool has_template(node_t *n) {
    if (n->template_content) return true;
    if (n->shadow_root && has_template(n->shadow_root)) return true;
    for (node_t *c = n->first; c; c = c->next) if (has_template(c)) return true;
    return false;
}
bool doc_node_adopt(web_doc *d, node_t *n) {
    if (!d || !n || n->type == N_DOC || n->shadow_host) return false;
    if (n->type == N_ATTR && n->attr_owner) {
        node_t *el = n->attr_owner;
        int i = (int)(n->attribute - el->attrs);
        if (!doc_attr_remove(el->owner, el, i)) return false;
    }
    /* Reserve the destination's template owner before detaching anything. */
    if (has_template(n) && !d->template_owner && !d->template_doc) {
        web_doc *owner = doc_inert(d, "", 0, "about:blank");
        if (!owner) return false;
        while (owner->root->first) doc_node_remove(owner, owner->root->first);
        owner->template_owner = true; d->template_doc = owner;
    }
    if (n->parent) doc_node_remove(n->owner ? n->owner : d, n);
    adopt_subtree(d, n);
    doc_shadow_reassign(d);
    return true;
}

node_t *doc_template_content(web_doc *d, node_t *n) {
    if (!d || !n || n->type != N_ELEM || n->foreign || n->tag != T_template) return NULL;
    if (n->template_content) return n->template_content;
    web_doc *owner = d->template_owner ? d : d->template_doc;
    if (!owner) {
        owner = doc_inert(d, "", 0, "about:blank");
        if (!owner) return NULL;
        while (owner->root->first) doc_node_remove(owner, owner->root->first);
        owner->template_owner = true; d->template_doc = owner;
    }
    n->template_content = doc_node_create(owner, N_FRAGMENT, NULL, "", 0);
    if (n->template_content) n->template_content->template_host = n;
    return n->template_content;
}

bool doc_templates_finish(web_doc *d, node_t *root) {
    for (node_t *n = root; n; n = n->next) {
        if (n->type == N_ELEM && !n->foreign && n->tag == T_template) {
            node_t *content = doc_template_content(n->owner ? n->owner : d, n);
            if (!content) return false;
            while (n->first) if (!doc_node_move(content->owner, content, n->first, NULL)) return false;
            if (!doc_templates_finish(content->owner, content->first)) return false;
        } else if (n->first && !doc_templates_finish(d, n->first)) return false;
    }
    return true;
}

bool doc_node_text(web_doc *d, node_t *n, const char *text, size_t len) {
    if (!d || !n || len > (16u << 20)) return false;
    if (n->type == N_ATTR) return doc_attr_value(d, n, text);
    if (n->type == N_DOC || n->type == N_DOCTYPE) return true;
    doc_dom_budget(d);
    if (n->type == N_TEXT || n->type == N_COMMENT || n->type == N_PI) {
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
    node_t *context = n->shadow_host ? n->shadow_host : n;
    if (n->type == N_ELEM && !n->foreign && n->tag == T_template) {
        n = doc_template_content(d, n);
        if (!n) return false;
        d = n->owner;
    }
    doc_dom_budget(d);
    node_t *fragment = html_fragment(d, context, html, len);
    if (!fragment) return false;
    for (node_t *c = fragment->first; c; c = c->next) if (!may_insert(n, c)) return false;
    while (n->first) doc_node_remove(d, n->first);
    return doc_node_move(d, n, fragment, NULL);
}

bool doc_node_value(web_doc *d, node_t *n, const char *text, size_t len) {
    if (!d || !n || len > (16u << 20) || (len && !text)) return false;
    web_doc *allocation = n->allocation_doc ? n->allocation_doc : d;
    bool input = n->type == N_ELEM && !n->foreign && n->tag == T_input;
    size_t capacity = input && len < 127 ? 128 : len + 1;
    size_t replaced = n->value_capacity + n->input_edit_capacity;
    if (d->dom_family && capacity > doc_dom_remaining(d) + replaced) return false;
    size_t next = allocation->control_bytes - replaced;
    if (!d->dom_family && d->live && (capacity > (32u << 20) || next > (32u << 20) - capacity ||
                    d->mem.allocated > (32u << 20) - next - capacity)) return false;
    char *value = malloc(capacity);
    if (!value) return false;
    if (input && web_input_type(n) == WEB_INPUT_FILE) { size_t files_bytes = n->files ? n->files->allocation : 0; web_input_files_clear(d, n); next -= files_bytes; }
    size_t used = 0;
    if (input) used = web_input_sanitize(n, text ? text : "", len, value);
    else for (size_t i = 0; i < len; i++) {
        if (n->tag == T_textarea && text[i] == '\r') {
            value[used++] = '\n'; if (i + 1 < len && text[i + 1] == '\n') i++;
        } else value[used++] = text[i];
    }
    value[used] = 0;
    free(n->value);
    web_input_clear_edit(d, n);
    n->value = value;
    n->value_capacity = capacity;
    n->value_dirty = true;
    uint32_t units = doc_utf16_length(value);
    if (n->selection_start > units) n->selection_start = units;
    if (n->selection_end > units) n->selection_end = units;
    doc_control_caret(d, n);
    allocation->control_bytes = next + capacity;
    if (d->dom_family) doc_dom_budget(d);
    else if (d->live) d->mem.limit = (32u << 20) - d->control_bytes;
    d->dirty = d->need_style = true;
    return true;
}

node_t *doc_node_clone(web_doc *d, node_t *n, bool deep) {
    if (!d || !n || n->type == N_DOC || n->shadow_host) return NULL;
    if (n->type == N_ATTR) {
        struct attr *a = n->attribute;
        return doc_attr_create(d, a->namespace_uri, a->prefix, a->local ? a->local : a->raw, a->value);
    }
    node_t *c = doc_node_create(d, n->type, n->raw_name ? n->raw_name : n->name, n->text, n->textlen);
    if (!c) return NULL;
    c->foreign = n->foreign;
    c->namespace_id = n->namespace_id;
    if (n->type == N_DOCTYPE) {
        /* Doctype strings are arena-owned like ordinary attributes. */
        jmp_buf trap; jmp_buf *old = d->mem.trap; d->mem.trap = &trap;
        if (setjmp(trap)) { d->mem.trap = old; return NULL; }
        c->public_id = ar_strdup(&d->mem, n->public_id ? n->public_id : "");
        c->system_id = ar_strdup(&d->mem, n->system_id ? n->system_id : "");
        d->mem.trap = old;
    }
    for (int i = 0; i < n->nattrs; i++) {
        struct attr *a = &n->attrs[i];
        if (!doc_attr_set_ns(d, c, a->namespace_uri, a->prefix, a->local ? a->local : a->raw, a->value)) return NULL;
    }
    c->checked = n->checked;
    c->indeterminate = n->indeterminate;
    c->selected = n->selected;
    c->selected_set = n->selected_set;
    c->control_ready = n->control_ready;
    if (n->value && !doc_node_value(d, c, n->value, strlen(n->value))) return NULL;
    if (!web_input_files_clone(d, c, n)) return NULL;
    c->value_dirty = n->value_dirty;
    c->checked_dirty = n->checked_dirty;
    c->script_started = n->tag == T_script;
    if (n->tag == T_template && !n->foreign) {
        node_t *content = doc_template_content(d, c);
        if (!content) return NULL;
        if (deep) for (node_t *ch = n->template_content ? n->template_content->first : NULL; ch; ch = ch->next) {
            node_t *copy = doc_node_clone(content->owner, ch, true);
            if (!copy || !doc_node_move(content->owner, content, copy, NULL)) return NULL;
        }
    }
    if (deep) for (node_t *ch = n->first; ch; ch = ch->next) {
        node_t *copy = doc_node_clone(d, ch, true);
        if (!copy || !doc_node_move(d, c, copy, NULL)) return NULL;
    }
    if (n->shadow_root && n->shadow_root->shadow_clonable) {
        node_t *source = n->shadow_root;
        node_t *root = doc_shadow_attach(d, c, source->shadow_closed, source->shadow_delegates_focus,
            true, source->shadow_serializable, source->shadow_manual);
        if (!root) return NULL;
        root->shadow_declarative = source->shadow_declarative;
        for (node_t *ch = source->first; ch; ch = ch->next) {
            node_t *copy = doc_node_clone(d, ch, true);
            if (!copy || !doc_node_move(d, root, copy, NULL)) return NULL;
        }
    }
    return c;
}
