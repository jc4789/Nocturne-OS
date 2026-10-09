/* Mutable nodes belong to the document, not to the layout or to a JS wrapper.
   Detached nodes stay valid until web_free; allocation/representation failures
   remain explicit, without author depth/name/attribute-count feature caps. */
#include <stdio.h>
#include <limits.h>
#include "nocturne.h"
#include "webi.h"
#include "frame.h"
#include "web_dialog.h"
#include "form_value.h"
#include "form_file.h"
#include "elements.h"
#include "js_canvas.h"
#include "cssom.h"
/* Internal lifetime query: implemented beside the wrapper cache. */
bool web_js_nodes_same_forest(web_doc *document, node_t *node, node_t *parent);

/* Native DOM trees are already cycle-checked on insertion. Walk their parent
   links rather than consuming the C stack or imposing an author depth limit.
   Shadow/template edges are optional: neither is an ordinary child edge. */
static node_t *tree_next(node_t *n, node_t *root, bool shadow, bool templates, bool descend) {
    if (descend) {
        if (n->first) return n->first;
        if (shadow && n->shadow_root) return n->shadow_root;
        if (templates && n->template_content) return n->template_content;
    }
    while (n != root) {
        node_t *p;
        if (n->parent) {
            if (n->next) return n->next;
            p = n->parent;
            if (shadow && p->shadow_root) return p->shadow_root;
            if (templates && p->template_content) return p->template_content;
        } else if (n->shadow_host) {
            p = n->shadow_host;
            if (templates && p->template_content) return p->template_content;
        } else if (n->template_host) p = n->template_host;
        else return NULL;
        n = p;
    }
    return NULL;
}

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
    for (node_t *n = root->first; n; n = tree_next(n, root, false, false, true)) {
        if (is_slot(n) && !strcmp(slot_name(n, true), name)) return n;
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
    for (node_t *n = root->first; n; n = tree_next(n, root, false, false, true)) {
        if (is_slot(n)) compare_slot(n);
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
struct slot_walk { node_t *slot, *next; bool assigned; };
static struct slot_walk slot_walk_start(node_t *slot, bool flatten) {
    node_t *first = slot->slot_assigned_first;
    return (struct slot_walk){slot, first ? first : flatten ? slot->first : NULL, first != NULL};
}
bool doc_slot_nodes(node_t *slot, bool flatten, pvec *out) {
    if (!out || !is_slot(slot)) return true;
    node_t *root = doc_node_root(slot, false);
    if (!root || !root->shadow_host) return true;
    size_t capacity = 16, used = 1;
    struct slot_walk *stack = malloc(capacity * sizeof *stack);
    if (!stack) return false;
    stack[0] = slot_walk_start(slot, flatten);
    while (used) {
        struct slot_walk *at = &stack[used - 1];
        node_t *n = at->next;
        if (!n) { used--; continue; }
        at->next = at->assigned ? n->assigned_next : n->next;
        if (!slottable(n)) continue;
        node_t *nr = is_slot(n) ? doc_node_root(n, false) : NULL;
        if (flatten && nr && nr->shadow_host) {
            /* Manual slot assignments can form a virtual cycle even though
               the native DOM is acyclic. Ignore only this cyclic edge, not a
               legitimate repeated node in another expansion branch. */
            bool cycle = false;
            for (size_t i = 0; i < used; i++) if (stack[i].slot == n) { cycle = true; break; }
            if (cycle) continue;
            if (used == capacity) {
                size_t max = SIZE_MAX / sizeof *stack;
                if (capacity == max) { free(stack); return false; }
                size_t next = capacity > max / 2 ? max : capacity * 2;
                struct slot_walk *grown = realloc(stack, next * sizeof *stack);
                if (!grown) { free(stack); return false; }
                stack = grown; capacity = next;
            }
            stack[used++] = slot_walk_start(n, true);
        } else {
            if (out->n == out->cap) {
                size_t max = SIZE_MAX / sizeof *out->v;
                if (max > INT_MAX) max = INT_MAX;
                if (out->cap < 0 || (size_t)out->cap >= max) { free(stack); return false; }
                size_t next = out->cap ? (size_t)out->cap * 2 : 16;
                if (next > max) next = max;
                void **grown = realloc(out->v, next * sizeof *out->v);
                if (!grown) { free(stack); return false; }
                out->v = grown; out->cap = (int)next;
            }
            out->v[out->n++] = n;
        }
    }
    free(stack); return true;
}
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
    d->dom_revision++;
    if (d->js_nodes_dirty) { d->js_nodes_dirty = false; web_js_nodes_changed(d); }
    if (n) n->resource_revision++;
    /* Detached forests are often prepared between geometry reads (jQuery,
       consent banners, ads). They cannot change the live cascade.
       Preserve DOM/reaction revisions; insertion/removal notifies the live
       parent and invalidates the snapshot at the actual connection boundary. */
    if (n && doc_node_root(n, true) != d->root) {
        /* HTML images fetch even while disconnected. Keep that resource path
           observable; ordinary detached forests need no live sheet rescan. */
        if (resources && !n->foreign && n->tag == T_img) d->resources_dirty = d->dirty = true;
        textarea_changed(n); return;
    }
    d->dirty = d->need_style = true;
    if (resources) d->resources_dirty = true;
    d->find_node = NULL;
    d->find_run = NULL;
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
    if (d && !n) d->js_nodes_dirty = true;
    changed(d, n, true);
    /* NULL denotes a parser's complete forest transaction. Ordinary control or
       CharacterData notifications cannot change assignment; structural/attribute
       mutations below identify the exact assignment-relevant boundary instead. */
    if (!n) doc_shadow_reassign(d);
}

/* Structural edits of ordinary text/elements change selectors and geometry,
   not the authored sheet set. Preserve the scan for every node kind consumed
   by doc.c scan(), image candidates, and document identity. In particular a
   text edit inside style/title/control ancestors still needs that scan. */
static bool resource_subtree(node_t *root) {
    if (!root) return true;
    for (node_t *n = root; n;) {
      bool descend = true;
      if (n->type == N_ELEM && !n->foreign) {
        switch (n->tag) {
        case T_html: case T_head: case T_body: case T_frameset:
        case T_base: case T_title: case T_meta: case T_link: case T_style:
        case T_img: case T_input: case T_textarea: case T_select: case T_option:
        case T_picture: case T_source:
            return true;
        default: break;
        }
        if (n->tag == T_template) descend = false; /* inert template content */
      }
      n = tree_next(n, root, true, false, descend);
    }
    return false;
}
static bool resource_ancestor(node_t *n) {
    for (; n; n = n->parent) if (!n->foreign && n->type == N_ELEM &&
        (n->tag == T_style || n->tag == T_title || n->tag == T_textarea || n->tag == T_select)) return true;
    return false;
}
static void structure_changed_lifetime(web_doc *d, node_t *parent, node_t *subtree, bool lifetime_changed) {
    cssom_style_text_changed(parent);
    if (lifetime_changed) cssom_style_lifecycle(subtree);
    if (d && lifetime_changed) d->js_nodes_dirty = true;
    changed(d, parent, resource_ancestor(parent) || resource_subtree(subtree));
}
static void structure_changed(web_doc *d, node_t *parent, node_t *subtree) {
    structure_changed_lifetime(d, parent, subtree, true);
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

void doc_create_diagnostic(web_doc *d,unsigned stage,int type,size_t name_bytes,size_t data_bytes) {
    static const char *const reasons[]={"validation", "DOM family quota", "Nocturne allocator",
        "arena size overflow", "template contents", "native wrapper"};
    if(!d || stage>=sizeof reasons/sizeof *reasons || (d->dom_create_reported&(1u<<stage)))return;
    d->dom_create_reported|=(uint8_t)(1u<<stage);
    // Controlled stage labels and byte counts only: no author key/name/value/URL.
    char message[320];
    snprintf(message,sizeof message,"DOM creation failure: %s; type=%d node_bytes=%zu name_bytes=%zu data_bytes=%zu; arena allocated=%zu limit=%zu; family remaining=%zu bytes",
        reasons[stage],type,sizeof(node_t),name_bytes,data_bytes,d->mem.allocated,d->mem.limit,doc_dom_remaining(d));
    web_doc *report=d->js?d:d->dom_family?d->dom_family:d;
    while(report && !report->js && report->frame_parent)report=report->frame_parent;
    web_js_console(report,2,message);
}

node_t *doc_node_create(web_doc *d, int type, const char *name, const char *text, size_t len) {
    if (!d) return NULL;
    size_t name_bytes=name?strlen(name):0;
    if(type<N_DOC || type>N_ATTR || len==SIZE_MAX || (len && !text) ||
       (type==N_ELEM && (!name || !*name)) ||
       (type==N_PI && (!name || !doc_pi_target_valid(name,name_bytes)))) {
        doc_create_diagnostic(d,DOC_CREATE_INVALID,type,name_bytes,len);return NULL;
    }
    doc_dom_budget(d);
    jmp_buf trap;
    jmp_buf *old = d->mem.trap;
    d->mem.trap = &trap;
    int failure=setjmp(trap);
    if(failure) {
        d->mem.trap=old;
        doc_create_diagnostic(d,failure==2?DOC_CREATE_QUOTA:failure==3?DOC_CREATE_ALLOCATOR:DOC_CREATE_OVERFLOW,type,name_bytes,len);
        return NULL;
    }
    node_t *n = ar_alloc(&d->mem, sizeof *n);
    n->type = (uint8_t)type;
    n->owner = n->allocation_doc = d;
    n->image = -1;
    n->owned_next = d->owned_nodes;
    d->owned_nodes = n;
    if (type == N_ELEM) {
        size_t l = strlen(name);
        n->tag = (uint16_t)tag_lookup(name, l); /* lookup normalizes known short names itself */
        if (n->tag) n->name = tag_names[n->tag];
        else {
            char *lower_name = ar_strndup(&d->mem, name, l);
            for (size_t i = 0; i < l; i++) lower_name[i] = (char)lower((unsigned char)lower_name[i]);
            n->name = lower_name;
        }
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
        if (count == INT_MAX || (size_t)count == SIZE_MAX / sizeof *n->classes) {
            if (d->mem.trap) longjmp(*d->mem.trap, 1);
            abort();
        }
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
        if (!retained) {
            identity->attr_owner = NULL;
            if (n->owner) n->owner->js_nodes_dirty = true;
        }
    }
    n->attrs = attrs; n->nattrs = count;
    for (int i = 0; i < count; i++) if (attrs[i].node) {
        if (attrs[i].node->attr_owner != n && n->owner) n->owner->js_nodes_dirty = true;
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
    if (!d || !local || !*local) return NULL;
    size_t pl = prefix ? strlen(prefix) : 0, ll = strlen(local);
    size_t extra = pl ? 2 : 1;
    if (pl > SIZE_MAX - extra || ll > SIZE_MAX - extra - pl) return NULL;
    node_t *made = doc_node_create(d, N_ATTR, NULL, NULL, 0);
    if (!made) return NULL;
    jmp_buf trap; jmp_buf *old = d->mem.trap; d->mem.trap = &trap;
    if (setjmp(trap)) { d->mem.trap = old; return NULL; }
    struct attr *a = ar_alloc(&d->mem, sizeof *a);
    char *raw = ar_alloc(&d->mem, pl + ll + extra);
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

static bool attribute_change_normalized(web_doc *d, node_t *n, int found, const char *name, const char *value,
                             const char *ns, const char *prefix, const char *local, node_t *identity, const char *low) {
    bool previously_selectable = doc_control_selection_supported(n);
    bool input_type_change = !ns && !n->foreign && n->tag == T_input &&
                             !strcmp(name, low) && !strcmp(low, "type");
    enum web_input_kind previous_type = web_input_type(n);
    if (input_type_change) doc_control_init(d, n);
    if (!ns && !n->foreign && n->tag == T_select && !strcmp(name, low) &&
        (!strcmp(low, "multiple") || !strcmp(low, "size"))) doc_control_init(d, n);
    if (found < 0 && !value) return true;
    bool html_image = !n->foreign && n->tag == T_img;
    bool image_candidate = !ns && !n->foreign && !strcmp(name, low) &&
        ((html_image && (!strcmp(low, "src") || !strcmp(low, "srcset") || !strcmp(low, "sizes") ||
          !strcmp(low, "data-src") || !strcmp(low, "data-lazy-src") || !strcmp(low, "data-original") ||
          !strcmp(low, "data-srcset"))) ||
         (n->tag == T_source && n->parent && !n->parent->foreign && n->parent->tag == T_picture &&
          (!strcmp(low, "srcset") || !strcmp(low, "sizes") || !strcmp(low, "media") || !strcmp(low, "type"))));
    bool resources = n->tag == T_base || n->tag == T_link || n->tag == T_style ||
                     (n->tag == T_img && !html_image) || n->tag == T_input || n->tag == T_meta;
    /* Keep JS attribute/CE reactions in native_dom, but an identical ordinary
       attribute has no new style to cascade and needs no arena allocation. */
    if (!resources && !image_candidate && !identity && found >= 0 && value && !strcmp(n->attrs[found].value, value)) {
        /* Canvasの寸法設定は同値でもbitmap/contextをresetする。arena再割当は不要。 */
        if (!ns && !n->foreign && n->tag == T_canvas && !strcmp(name, low) &&
            (!strcmp(low, "width") || !strcmp(low, "height"))) {
            web_canvas_attr_changed(n, local);
            d->dirty = true;
        }
        return true;
    }
    if (n->nattrs < 0 || (found < 0 && n->nattrs == INT_MAX)) return false;
    int count = n->nattrs + (found < 0 ? 1 : value ? 0 : -1);
    if (count < 0 || (size_t)count > SIZE_MAX / sizeof(struct attr)) return false;
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
    if (ordinary && n->tag==T_style && !strcmp(low,"type")) cssom_style_lifecycle(n);
    if (ordinary && n->tag == T_link && (!strcmp(low,"href") || !strcmp(low,"rel") ||
        !strcmp(low,"media") || !strcmp(low,"type") || !strcmp(low,"disabled"))) {
        n->stylesheet_generation++; n->stylesheet_url = NULL; n->stylesheet_notified = false;
        if(!strcmp(low,"disabled")){n->style_disabled_set=false;n->style_disabled=false;}
        if(!strcmp(low,"href")||!strcmp(low,"rel")||!strcmp(low,"type"))cssom_style_lifecycle(n);
    }
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
    /* Candidate changes retain image requests (including detached images) but
       cannot add/remove authored stylesheets. Attribute selectors and attr()
       still invalidate the live cascade through changed() above. */
    if (image_candidate) {
        if (html_image) n->image_invalidated = true;
        else for (node_t *child = n->parent->first; child; child = child->next)
            if (!child->foreign && child->tag == T_img) child->image_invalidated = true;
        d->images_dirty = d->dirty = true;
    }
    if (!ns && ((!strcmp(name, "slot") && n->parent && n->parent->shadow_root && !n->parent->shadow_root->shadow_manual) ||
        (!strcmp(name, "name") && is_slot(n) && doc_node_root(n, false)->shadow_host && !doc_node_root(n, false)->shadow_manual)))
        doc_shadow_reassign(d);
    if (!ns) doc_details_attribute_changed(d, n, local, found >= 0);
    if (ordinary) web_select_attribute_changed(d, n, local, value != NULL);
    if (ordinary) web_canvas_attr_changed(n, local);
    return true;
}

static bool attribute_change(web_doc *d, node_t *n, int found, const char *name, const char *value,
                             const char *ns, const char *prefix, const char *local, node_t *identity) {
    if (!d || !n || n->type != N_ELEM || !name || !*name) return false;
    size_t length = strlen(name);
    if (length == SIZE_MAX) return false;
    char *low = malloc(length + 1);
    if (!low) return false;
    for (size_t i = 0; i < length; i++) low[i] = (char)lower((unsigned char)name[i]);
    low[length] = 0;
    bool result = attribute_change_normalized(d, n, found, name, value, ns, prefix, local, identity, low);
    free(low); return result;
}

bool doc_node_attr(web_doc *d, node_t *n, const char *name, const char *value) {
    int found = doc_attr_index(n, NULL, name, false);
    if (found < 0 && !value) return true;
    const struct attr *a = found >= 0 ? &n->attrs[found] : NULL;
    if (!name) return false;
    size_t length = strlen(name);
    if (length == SIZE_MAX) return false;
    char *normalized = malloc(length + 1);
    if (!normalized) return false;
    memcpy(normalized, name, length + 1);
    if (n && !n->foreign) for (char *p = normalized; *p; p++) *p = (char)lower((unsigned char)*p);
    bool result = attribute_change(d, n, found, a ? a->raw : normalized, value,
        a ? a->namespace_uri : NULL, a ? a->prefix : NULL, a ? a->local : normalized, NULL);
    free(normalized); return result;
}
bool doc_attr_set_ns(web_doc *d, node_t *n, const char *ns, const char *prefix, const char *local, const char *value) {
    int found = doc_attr_index(n, ns, local, true);
    if (found >= 0) {
        struct attr *a = &n->attrs[found];
        return attribute_change(d, n, found, a->raw, value, a->namespace_uri, a->prefix, a->local, NULL);
    }
    if (!local || !*local) return false;
    size_t pl = prefix ? strlen(prefix) : 0, ll = strlen(local), extra = prefix ? 2 : 1;
    if (pl > SIZE_MAX - extra || ll > SIZE_MAX - extra - pl) return false;
    char *name = malloc(pl + ll + extra);
    if (!name) return false;
    if (prefix) { memcpy(name, prefix, pl); name[pl] = ':'; }
    memcpy(name + pl + (prefix ? 1 : 0), local, ll + 1);
    bool result = attribute_change(d, n, -1, name, value, ns, prefix, local, NULL);
    free(name); return result;
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
    if (!n || n->type != N_ATTR || !n->attribute || !value) return false;
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

static void stylesheet_detach(node_t *root) {
    for (node_t *n = root; n;) {
        if (n->stylesheet_url) {
            n->stylesheet_generation++; n->stylesheet_url = NULL;
            n->stylesheet_owner = NULL; n->stylesheet_notified = false;
        }
        n = tree_next(n, root, true, false, n->foreign || n->tag != T_template);
    }
}
static void detach(node_t *n) {
    node_t *p = n->parent;
    if (!p) return;
    stylesheet_detach(n);
    if (n->prev) n->prev->next = n->next; else p->first = n->next;
    if (n->next) n->next->prev = n->prev; else p->last = n->prev;
    n->parent = n->prev = n->next = NULL;
    indices(p);
    textarea_changed(p);
}

static bool may_insert(node_t *p, node_t *c) {
    if (!p || !c || p == c || under(p, c) || c->type == N_DOC || c->type == N_ATTR ||
        (p->type != N_DOC && p->type != N_ELEM && p->type != N_FRAGMENT)) return false;
    for (node_t *n = p; n; n = n->parent ? n->parent : n->shadow_host ? n->shadow_host : n->template_host) if (n == c) return false;
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
    if (!d || !p || !c || p->owner != d || (before && before->parent != p)) return false;
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
    bool same_lifetime_forest = web_js_nodes_same_forest(d, c, p);
    if (c->owner != d && !doc_node_adopt(d, c)) return false;
    node_t *old_parent = c->parent;
    node_t *old_select = node_ancestor(old_parent, T_select);
    bool assignment_changed = assignment_structure(old_parent, c) || assignment_structure(p, c);
    if (is_slot(old_parent) && !old_parent->slot_assigned_first && doc_node_root(old_parent, false)->shadow_host)
        doc_slot_signal(old_parent);
    if(old_parent)web_dialog_removed(d,c);
    if(old_parent)web_frames_detach_tree(d,c);
    detach(c);
    c->parent = p;
    c->next = before;
    c->prev = before ? before->prev : p->last;
    if (c->prev) c->prev->next = c; else p->first = c;
    if (before) before->prev = c; else p->last = c;
    indices(p);
    if (is_slot(p) && !p->slot_assigned_first && doc_node_root(p, false)->shadow_host)
        doc_slot_signal(p);
    /* A move into a detached forest still removes content from the live tree. */
    if (old_parent && old_parent != p) structure_changed_lifetime(d, old_parent, c, !same_lifetime_forest);
    structure_changed_lifetime(d, p, c, !same_lifetime_forest);
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
    web_frames_detach_tree(d,n);
    detach(n);
    structure_changed(d, p, n);
    if (assignment_changed) doc_shadow_reassign(d);
    web_select_inserted(d, n, p);
    if (select) web_select_sync(select->owner, select, false);
}

static void adopt_subtree(web_doc *d, node_t *root) {
  for (node_t *n = root; n; n = tree_next(n, root, true, true, true)) {
    web_doc *owner = d;
    if (n != root) {
        if (n->parent) owner = n->parent->owner;
        else if (n->shadow_host) owner = n->shadow_host->owner;
        else if (n->template_host) {
            owner = n->template_host->owner;
            owner = owner->template_owner ? owner : owner->template_doc;
        }
    }
    n->owner = owner;
    for (int i = 0; i < n->nattrs; i++) if (n->attrs[i].node) n->attrs[i].node->owner = owner;
    n->style = n->animation_base_style = NULL; n->box = n->anchor_block = NULL;
    n->image = -1; n->image_request = -1; n->image_initialized = false;
    n->image_generation++;
    /* Adoption is an image-data mutation even without a later insertion or
       getter. Keep allocation ownership untouched; schedule the live scan. */
    if (!owner->inert && n->type == N_ELEM && !n->foreign && n->tag == T_img)
        owner->resources_dirty = owner->dirty = owner->need_style = true;
    if (n->shadow_root) {
        (owner->dom_family ? owner->dom_family : owner)->has_shadow = true;
    }
  }
}
static bool has_template(node_t *root) {
    for (node_t *n = root; n; n = tree_next(n, root, true, false, true))
        if (n->template_content) return true;
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
    web_js_nodes_changed(d);
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
  for (node_t *top = root; top; top = top->next) {
    for (node_t *n = top; n; n = tree_next(n, top, false, true, true)) {
        if (n->type == N_ELEM && !n->foreign && n->tag == T_template) {
            node_t *content = doc_template_content(n->owner ? n->owner : d, n);
            if (!content) return false;
            while (n->first) if (!doc_node_move(content->owner, content, n->first, NULL)) return false;
        }
    }
  }
    return true;
}

bool doc_node_text(web_doc *d, node_t *n, const char *text, size_t len) {
    if (!d || !n || len == SIZE_MAX || (len && !text)) return false;
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
    cssom_style_text_changed(n);
    changed(d, n, resource_ancestor(n));
    return true;
}

bool doc_node_html(web_doc *d, node_t *n, const char *html, size_t len) {
    if (!d || !n || (n->type != N_ELEM && n->type != N_FRAGMENT) || len == SIZE_MAX || (len && !html)) return false;
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
    if (!d || !n || len == SIZE_MAX || (len && !text)) return false;
    web_doc *allocation = n->allocation_doc ? n->allocation_doc : d;
    bool input = n->type == N_ELEM && !n->foreign && n->tag == T_input;
    size_t capacity = input && len < 127 ? 128 : len + 1;
    if (n->input_edit_capacity > SIZE_MAX - n->value_capacity) return false;
    size_t replaced = n->value_capacity + n->input_edit_capacity;
    if (allocation->control_bytes < replaced) return false;
    size_t next = allocation->control_bytes - replaced;
    if (capacity > SIZE_MAX - next) return false;
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
    doc_dom_budget(d);
    d->dirty = d->need_style = true;
    return true;
}

/* A clone has no escaped Attr wrappers or native control state yet. Build
   ordinary attributes once rather than retaining 1+2+...+N arena arrays and
   re-tokenizing class after every attribute. All strings belong to the clone's
   allocation document; never retain source-owner pointers or Attr identities. */
static bool clone_attributes(web_doc *d, node_t *copy, const node_t *source) {
    if (!source->nattrs) return true;
    if (source->nattrs < 0 || (size_t)source->nattrs > SIZE_MAX / sizeof(struct attr)) return false;
    if (!source->foreign) switch (source->tag) {
    case T_input: case T_select: case T_option: case T_details:
    case T_img: case T_source:
        /* Keep native value/selection/toggle and detached-image side effects
           on their existing, independently tested setter path. */
        for (int i = 0; i < source->nattrs; i++) {
            const struct attr *a = &source->attrs[i];
            if (!doc_attr_set_ns(d, copy, a->namespace_uri, a->prefix,
                    a->local ? a->local : a->raw, a->value)) return false;
        }
        return true;
    default: break;
    }
    doc_dom_budget(d);
    jmp_buf trap;
    jmp_buf *old = d->mem.trap;
    d->mem.trap = &trap;
    if (setjmp(trap)) { d->mem.trap = old; return false; }
    struct attr *attrs = ar_alloc(&d->mem, (size_t)source->nattrs * sizeof *attrs);
    for (int i = 0; i < source->nattrs; i++) {
        const struct attr *a = &source->attrs[i];
        attrs[i] = (struct attr){
            .name = ar_strdup(&d->mem, a->name),
            .raw = ar_strdup(&d->mem, a->raw),
            .value = ar_strdup(&d->mem, a->value),
            .namespace_uri = a->namespace_uri ? ar_strdup(&d->mem, a->namespace_uri) : NULL,
            .prefix = a->prefix ? ar_strdup(&d->mem, a->prefix) : NULL,
            .local = a->local ? ar_strdup(&d->mem, a->local) : NULL,
            .node = NULL
        };
    }
    node_t ready = *copy;
    ready.attrs = attrs;
    ready.nattrs = source->nattrs;
    attribute_cache(d, &ready);
    doc_attrs_publish(copy, ready.attrs, ready.nattrs);
    copy->id = ready.id;
    copy->classes = ready.classes;
    copy->nclasses = ready.nclasses;
    d->mem.trap = old;
    return true;
}

static node_t *clone_one(web_doc *d, node_t *n) {
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
    if (!clone_attributes(d, c, n)) return NULL;
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
    }
    return c;
}

/* Keep the recursive algorithm's observable order: template descendants,
   ordinary descendants, then clonable shadow descendants; insert a completed
   child only after cloning it. The correspondence stack grows on demand and
   never turns DOM depth into C stack depth. Partial clones remain unescaped. */
struct clone_walk {
    node_t *source, *copy, *attach, *target, *next;
    unsigned stage;
    bool deep;
};
node_t *doc_node_clone(web_doc *d, node_t *n, bool deep) {
    node_t *root = clone_one(d, n);
    if (!root || n->type == N_ATTR) return root;
    size_t capacity = 16, used = 1;
    struct clone_walk *stack = malloc(capacity * sizeof *stack);
    if (!stack) return NULL;
    stack[0] = (struct clone_walk){.source=n, .copy=root, .deep=deep};
    while (used) {
        struct clone_walk *at = &stack[used - 1];
        node_t *source = at->source, *copy = at->copy;
        if (!at->stage) {
            at->target = copy->template_content;
            at->next = at->deep && at->target && source->template_content ? source->template_content->first : NULL;
            at->stage = 1;
        }
        if (!at->next && at->stage == 1) {
            at->target = copy;
            at->next = at->deep ? source->first : NULL;
            at->stage = 2;
        }
        if (!at->next && at->stage == 2) {
            node_t *shadow = source->shadow_root;
            at->stage = 3;
            if (shadow && shadow->shadow_clonable) {
                at->target = doc_shadow_attach(copy->owner, copy, shadow->shadow_closed,
                    shadow->shadow_delegates_focus, true, shadow->shadow_serializable, shadow->shadow_manual);
                if (!at->target) { free(stack); return NULL; }
                at->target->shadow_declarative = shadow->shadow_declarative;
                at->next = shadow->first;
            }
        }
        if (!at->next) {
            node_t *attach = at->attach;
            if (attach && !doc_node_move(attach->owner, attach, copy, NULL)) { free(stack); return NULL; }
            used--; continue;
        }
        node_t *child = at->next, *target = at->target;
        at->next = child->next;
        if (used == capacity) {
            size_t max = SIZE_MAX / sizeof *stack;
            if (capacity == max) { free(stack); return NULL; }
            size_t next = capacity > max / 2 ? max : capacity * 2;
            struct clone_walk *grown = realloc(stack, next * sizeof *stack);
            if (!grown) { free(stack); return NULL; }
            stack = grown; capacity = next;
        }
        node_t *made = clone_one(target->owner, child);
        if (!made) { free(stack); return NULL; }
        stack[used++] = (struct clone_walk){.source=child, .copy=made, .attach=target, .deep=true};
    }
    free(stack); return root;
}
