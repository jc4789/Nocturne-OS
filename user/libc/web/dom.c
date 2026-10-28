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
/* GC ownership spans shadow and template forests, not just ordinary children.
   Share the native non-recursive walker with wrapper lifetime publication. */
node_t *doc_forest_next(node_t *n,node_t *root) {
    return n && root ? tree_next(n,root,true,true,true) : NULL;
}

/* Only an element insertion/removal changes the element sibling positions.
   The prefix before first remains coherent, including a parser-published
   prefix. Appends therefore do not revisit every previous child. */
static void indices(node_t *p, node_t *first) {
    if (!first) return;
    web_doc *d = p->owner;
    if (d && d->dom_family) d = d->dom_family;
    uint64_t start = d && d->profile_enabled ? uptime_ms() : 0, visited = 0;
    int i = 0;
    for (node_t *c = first->prev; c; c = c->prev) {
        visited++;
        if (c->type == N_ELEM) { i = c->elem_index; break; }
    }
    for (node_t *c = first; c; c = c->next) {
        visited++;
        if (c->type == N_ELEM) c->elem_index = ++i;
    }
    if (d) { d->profile.index_visits += visited; if(d->profile_enabled)d->profile.index_ms += uptime_ms() - start; }
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
    if (n && n->owner) doc_shadow_flush(n->owner);
    if (!slottable(n) || !n->parent || !n->parent->shadow_root) return NULL;
    node_t *root = n->parent->shadow_root;
    if (open_only && root->shadow_closed) return NULL;
    return n->assigned_slot;
}
/* Keep assignment snapshots as intrusive native lists, without arena churn or
   a parallel JS DOM. All comparisons happen before rewriting any next link. */
static bool compare_slot(node_t *slot) {
    node_t *old = slot->slot_assigned_first, *wanted = slot->slot_wanted_first;
    while (old && wanted && old == wanted) { old = old->assigned_next; wanted = wanted->pending_assigned_next; }
    if (!old && !wanted) return false;
    doc_slot_signal(slot);
    return true;
}
static size_t slot_hash(const char *name) {
    size_t hash = 2166136261u;
    for (; *name; name++) hash = (hash ^ (unsigned char)*name) * 16777619u;
    return hash;
}
static void wanted_append(node_t *slot, node_t *n) {
    n->pending_assigned_slot = slot; n->pending_assigned_next = NULL;
    if (slot->slot_wanted_last) slot->slot_wanted_last->pending_assigned_next = n;
    else slot->slot_wanted_first = n;
    slot->slot_wanted_last = n;
}
static bool shadow_wanted(node_t *root, web_doc *family) {
    size_t slots = 0, capacity = 1;
    for (node_t *s = root->first; s; s = tree_next(s, root, false, false, true)) {
        family->profile.shadow_visits++;
        if (!web_native_checkpoint(root->owner)) return false;
        if (is_slot(s)) { slots++; s->slot_wanted_first = s->slot_wanted_last = NULL; }
    }
    while (capacity <= slots && capacity <= SIZE_MAX / 2 / sizeof(node_t *)) capacity *= 2;
    node_t **names = slots && !root->shadow_manual ? calloc(capacity, sizeof *names) : NULL;
    for (node_t *s = root->first; s; s = tree_next(s, root, false, false, true)) if (is_slot(s)) {
        if (root->shadow_manual) {
            for (node_t *n = s->slot_manual_first; n; n = n->manual_next)
                if (n->parent == root->shadow_host) wanted_append(s, n);
        } else if (names) {
            size_t at = slot_hash(slot_name(s, true)) & (capacity - 1);
            while (names[at] && strcmp(slot_name(names[at], true), slot_name(s, true))) at = (at + 1) & (capacity - 1);
            if (!names[at]) names[at] = s; /* first slot in native tree order */
        }
    }
    if (!root->shadow_manual && slots) for (node_t *n = root->shadow_host->first; n; n = n->next) {
        family->profile.shadow_visits++;
        if (!web_native_checkpoint(root->owner)) { free(names); return false; }
        if (!slottable(n)) continue;
        node_t *s;
        if (names) {
            const char *name = slot_name(n, false); size_t at = slot_hash(name) & (capacity - 1);
            while (names[at] && strcmp(slot_name(names[at], true), name)) at = (at + 1) & (capacity - 1);
            s = names[at];
        } else s = first_named_slot(root, slot_name(n, false)); /* allocation failure: local fallback */
        if (s) wanted_append(s, n);
    }
    free(names); return true;
}
static bool compare_shadow_slots(node_t *root) {
    bool changed = false;
    for (node_t *n = root->first; n; n = tree_next(n, root, false, false, true)) {
        if (is_slot(n) && compare_slot(n)) changed = true;
    }
    return changed;
}
/* Slot reassignment can change fallback visibility and inherited parents
 * without touching those elements directly, so a changed assignment must not
 * reuse a stale flat-tree style/box topology. An unchanged assignment leaves
 * the flat tree as the mutation itself left it, and that mutation already
 * invalidated its own boundary: an ordinary insert into a shadow tree is not a
 * whole-document recascade. */
static void shadow_assignment_changed(node_t *root) {
    web_doc *d = root->owner;
    if (!d || !root->shadow_host || !d->live || d->inert || doc_node_root(root->shadow_host, true) != d->root) return;
    d->need_style = d->style_full_dirty = d->need_boxes = true;
    d->layout_valid = false;
    boxes_require_full(d, "slot assignment");
}
void doc_shadow_dirty(node_t *root) {
    if (!root || !root->shadow_host) return;
    web_doc *d = root->owner, *family = d->dom_family ? d->dom_family : d;
    /* The next geometry read or paint flushes assignment before using it. */
    if(d->live && !d->inert && doc_node_root(root->shadow_host,true)==d->root) d->layout_valid=false;
    if(root->shadow_dirty)return;
    root->shadow_dirty = true; root->shadow_dirty_next = NULL;
    if (family->shadow_dirty_last) family->shadow_dirty_last->shadow_dirty_next = root;
    else family->shadow_dirty_first = root;
    family->shadow_dirty_last = root;
}
void doc_shadow_flush(web_doc *d) {
    if (!d) return;
    web_doc *family = d->dom_family ? d->dom_family : d;
    if (family->shadow_flushing || !family->shadow_dirty_first) return;
    family->shadow_flushing = true;
    for (node_t *r = family->shadow_dirty_first; r; r = r->shadow_dirty_next)
        if (!shadow_wanted(r, family)) { family->shadow_flushing = false; return; }
    /* Compare every affected root before touching shared assignment links.
       A slot can move between two roots in the same transaction. */
    for (node_t *r = family->shadow_dirty_first; r; r = r->shadow_dirty_next) {
        family->profile.shadow_reassigns++;
        bool changed = compare_shadow_slots(r);
        for (node_t *s = r->shadow_slots; s; s = s->shadow_slot_next)
            if (!doc_node_root(s, false)->shadow_host) {
                s->slot_wanted_first = s->slot_wanted_last = NULL;
                if (compare_slot(s)) changed = true;
            }
        if (changed) shadow_assignment_changed(r);
    }
    for (node_t *r = family->shadow_dirty_first; r; r = r->shadow_dirty_next) {
        for (node_t *s = r->shadow_slots; s; s = s->shadow_slot_next) {
            for (node_t *c = s->slot_assigned_first, *next; c; c = next) {
                next = c->assigned_next; c->assigned_slot = c->assigned_next = NULL;
            }
            s->slot_assigned_first = s->slot_assigned_last = NULL;
        }
    }
    /* Old root lists are no longer needed after the clear phase. */
    for (node_t *r = family->shadow_dirty_first; r; r = r->shadow_dirty_next) r->shadow_slots = NULL;
    for (node_t *r = family->shadow_dirty_first, *next; r; r = next) {
        next = r->shadow_dirty_next;
        node_t **tail = &r->shadow_slots;
        for (node_t *s = r->first; s; s = tree_next(s, r, false, false, true)) if (is_slot(s)) {
            *tail = s; tail = &s->shadow_slot_next;
            for (node_t *c = s->slot_wanted_first; c; c = c->pending_assigned_next) {
                c->assigned_slot = s;
                if (s->slot_assigned_last) s->slot_assigned_last->assigned_next = c;
                else s->slot_assigned_first = c;
                s->slot_assigned_last = c;
            }
        }
        *tail = NULL; r->shadow_dirty = false; r->shadow_dirty_next = NULL;
    }
    family->shadow_dirty_first = family->shadow_dirty_last = NULL;
    family->shadow_flushing = false;
}
void doc_shadow_reassign(web_doc *d) {
    /* Parser-only complete-tree notification; mutations queue exact roots. */
    if (!d || !(d->dom_family ? d->dom_family : d)->has_shadow) return;
    for (node_t *n = d->root; n; n = tree_next(n, d->root, true, true, true))
        if (n->shadow_host) doc_shadow_dirty(n);
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
    doc_shadow_dirty(root);
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
        if (n->manual_slot) doc_shadow_dirty(doc_node_root(n->manual_slot, false));
        manual_unlink(n);
        n->manual_slot = slot;
        if (slot->slot_manual_last) slot->slot_manual_last->manual_next = n; else slot->slot_manual_first = n;
        slot->slot_manual_last = n;
    }
    doc_mutated(slot->owner, slot);
    doc_shadow_dirty(doc_node_root(slot, false));
    return true;
}
struct slot_walk { node_t *slot, *next; bool assigned; };
static struct slot_walk slot_walk_start(node_t *slot, bool flatten) {
    node_t *first = slot->slot_assigned_first;
    return (struct slot_walk){slot, first ? first : flatten ? slot->first : NULL, first != NULL};
}
bool doc_slot_nodes(node_t *slot, bool flatten, pvec *out) {
    if (slot && slot->owner) doc_shadow_flush(slot->owner);
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
    doc_shadow_flush(n->owner);
    if (n->shadow_root) n = n->shadow_root;
    node_t *root = is_slot(n) ? doc_node_root(n, false) : NULL;
    if (root && root->shadow_host && n->slot_assigned_first) {
        for (node_t *c = n->slot_assigned_first; c; c = c->assigned_next) pv_push(out, c);
    } else for (node_t *c = n->first; c; c = c->next) pv_push(out, c);
}
node_t *doc_flat_parent(node_t *n) {
    if (!n || n->shadow_host) return NULL;
    doc_shadow_flush(n->owner);
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

static void changed_geometry(web_doc *d, node_t *n, bool resources, bool recascade, bool boxes,node_t *child_list,bool inserted,
                             const struct css_attribute_change *attribute) {
    if (!d) return;
    d->dom_revision++;
    /* Window の名前表はdocument-tree構造だけを追う。style/rAFやshadow内部は無効化しない。 */
    if(!n || (child_list && doc_node_root(n,false)==d->root))web_js_window_names_changed(d);
    if (d->js_nodes_dirty) { d->js_nodes_dirty = false; web_js_nodes_changed(d); }
    if (n) {
        n->resource_revision++;
        html_bridge_native_changed(n, HTML_BRIDGE_FIELDS);
    }
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
    d->dirty = true;
    if(recascade){
        if(child_list)css_mark_child_list_dirty(d,n,child_list,inserted);
        else if(attribute)css_mark_attribute_dirty(d,n,attribute);
        else css_mark_dirty(d,n);
    }
    /* A text box regenerates in its parent; an element's changed children
       were recorded where they moved. */
    if(boxes){d->need_boxes=true;d->layout_valid=false;if(n && n->type==N_TEXT)boxes_note_regen(d,n);}
    if (resources) d->resources_dirty = true; /* its rescan restyles everything if the sheets changed */
    if (recascade || boxes) {
        d->layout_valid = false;
        if (n && n->box && (boxes || n->box->kind==B_ATOMIC)) layout_invalidate(n->box);
    }
    boxes_note_foreign(d,n);
    d->find_node = NULL;
    d->find_run = NULL;
    web_dialog_sync(d);
    textarea_changed(n);
    /* Keep SVG raster ownership until the next box publication. doc_svg
       serializes the real DOM/computed paint on every rebuild and compares
       the complete source before reusing pixels. A mutation outside an SVG
       must not discard every icon; removed/hidden entries are swept after
       boxes_build has stopped borrowing the old cache pointers. */
}
static void changed(web_doc *d,node_t *n,bool resources,const struct css_attribute_change *attribute) {
    changed_geometry(d,n,resources,true,false,NULL,false,attribute);
}
void doc_mutated(web_doc *d, node_t *n) {
    if (d && !n) { d->js_nodes_dirty = true; d->need_boxes = true; d->metadata_valid=false; boxes_require_full(d, "parser forest"); }
    changed(d, n, true, NULL);
    /* NULL denotes a parser's complete forest transaction. Ordinary control or
       CharacterData notifications cannot change assignment; structural/attribute
       mutations below identify the exact assignment-relevant boundary instead. */
    if (!n) doc_shadow_reassign(d);
}

/* Structural edits of ordinary text/elements change selectors and geometry,
   not the authored sheet set. Preserve the scan for every node kind consumed
   by doc.c scan(), image candidates, and document identity. In particular a
   text edit inside style/title/control ancestors still needs that scan. */
enum { SUBTREE_RESOURCES = 1, SUBTREE_IMAGES = 2 };
/* Images need only their requests synced (doc_rescan's images_dirty path, the
   same doc_image_sync a full scan does for them). They never add or remove an
   authored sheet, so they must not discard the rule index and recascade the
   whole document on every inserted thumbnail. */
static unsigned resource_subtree(node_t *root) {
    if (!root) return SUBTREE_RESOURCES;
    unsigned found = 0;
    for (node_t *n = root; n;) {
      bool descend = true;
      if (n->type == N_ELEM && !n->foreign) {
        switch (n->tag) {
        case T_html: case T_head: case T_body: case T_frameset:
        case T_base: case T_title: case T_meta: case T_link: case T_style:
        case T_input: case T_textarea: case T_select: case T_option:
            return SUBTREE_RESOURCES;
        case T_img: case T_picture: case T_source:
            found |= SUBTREE_IMAGES; break;
        default: break;
        }
        if (n->tag == T_template) descend = false; /* inert template content */
      }
      n = tree_next(n, root, true, false, descend);
    }
    return found;
}
/* A fragment parser builds a detached forest; inserting it notifies the tree
   it joins at that boundary. Here only the forest changed: its wrappers, and
   its images, which request while detached. */
void doc_fragment_parsed(web_doc *d, node_t *forest) {
    if (!d || !forest) return;
    web_js_nodes_reparented(d, forest);
    changed(d, forest, false, NULL);
    if (resource_subtree(forest) & SUBTREE_IMAGES) d->images_dirty = d->dirty = true;
}
/* The attributes doc.c scan() reads to find an authored sheet, the base URL,
   a meta directive or an image button's image. Any other attribute of these
   elements (class, ARIA, data-*, an inline style, a control's value) changes
   the cascade, not the set of sheets, and must not re-parse all of them. */
static bool resource_attribute(const node_t *n, const char *name) {
    if (n->foreign) return false;
    switch (n->tag) {
    case T_base: return !strcmp(name, "href");
    case T_link: return !strcmp(name, "href") || !strcmp(name, "rel") || !strcmp(name, "type") ||
                        !strcmp(name, "media") || !strcmp(name, "disabled");
    case T_style: return !strcmp(name, "type") || !strcmp(name, "media");
    case T_meta: return !strcmp(name, "name") || !strcmp(name, "content") || !strcmp(name, "http-equiv");
    case T_input: return !strcmp(name, "type") || !strcmp(name, "src");
    default: return false;
    }
}
static bool resource_ancestor(node_t *n) {
    for (; n; n = n->parent) if (!n->foreign && n->type == N_ELEM &&
        (n->tag == T_style || n->tag == T_title || n->tag == T_textarea || n->tag == T_select)) return true;
    return false;
}
static bool metadata_subtree(node_t *root) {
    for(node_t *n=root;n;n=tree_next(n,root,false,false,n->foreign||n->tag!=T_template))
        if(n->type==N_ELEM&&!n->foreign&&n->tag==T_base&&node_attr(n,"href"))return true;
    return false;
}
static void structure_changed_lifetime(web_doc *d, node_t *parent, node_t *subtree, bool lifetime_changed, bool inserted) {
    html_bridge_native_changed(parent, HTML_BRIDGE_CHILDREN);
    cssom_style_text_changed(parent);
    if (lifetime_changed) cssom_style_lifecycle(subtree);
    if (d && lifetime_changed) web_js_nodes_reparented(d,subtree);
    /* documentElement/head/body identities and the first ordinary-tree base
       are metadata dependencies. Class/text/control/ARIA and shadow/template
       mutations are not. Inspect the moved subtree even after detach: the
       old live parent still owns removal invalidation at this boundary. */
    if(d&&d->metadata_valid&&doc_node_root(parent,false)==d->root&&
       !node_ancestor(parent,T_template)&&
       (parent->type==N_DOC || parent==d->html || metadata_subtree(subtree)))
        d->metadata_valid=false;
    if (d && doc_node_root(parent, true) == d->root) d->need_boxes = true;
    bool resources = parent->type==N_DOC || resource_ancestor(parent);
    unsigned kind = resources ? 0 : resource_subtree(subtree);
    resources = resources || (kind & SUBTREE_RESOURCES);
    changed_geometry(d,parent,resources,true,false,subtree,inserted,NULL);
    if (d && !resources && (kind & SUBTREE_IMAGES) && doc_node_root(parent,true)==d->root)
        d->images_dirty = d->dirty = true;
    if (d && doc_node_root(parent,true)==d->root) boxes_note_child_list(d,parent,subtree);
}
static void structure_changed(web_doc *d, node_t *parent, node_t *subtree) {
    structure_changed_lifetime(d, parent, subtree, true, false);
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

int doc_namespace_id(const char *ns) {
    if (!ns || !*ns) return NS_NONE;
    if (!strcmp(ns, WEB_NS_HTML)) return NS_HTML;
    if (!strcmp(ns, WEB_NS_SVG)) return NS_SVG;
    if (!strcmp(ns, WEB_NS_MATHML)) return NS_MATHML;
    return NS_OTHER;
}
const char *doc_element_namespace(const node_t *n) {
    if (!n || n->type != N_ELEM) return NULL;
    switch (n->namespace_id) {
    case NS_HTML: return WEB_NS_HTML;
    case NS_SVG: return WEB_NS_SVG;
    case NS_MATHML: return WEB_NS_MATHML;
    case NS_OTHER: return n->namespace_uri;
    default: return NULL;
    }
}
/* DOM "create an element"'s names, under the caller's arena trap. The tag ID
   follows the local name; an HTML local name is case-sensitive, so one with
   ASCII uppercase is an unknown element, not the lowercase element. */
static void element_names(web_doc *d, node_t *n, int namespace_id, const char *ns,
                          const char *prefix, size_t prefix_len, const char *local, size_t local_len) {
    size_t at = prefix_len ? prefix_len + 1 : 0;
    char *qualified = ar_alloc(&d->mem, at + local_len + 1);
    if (prefix_len) { memcpy(qualified, prefix, prefix_len); qualified[prefix_len] = ':'; }
    memcpy(qualified + at, local, local_len); qualified[at + local_len] = 0;
    n->raw_name = qualified; n->local_name = qualified + at;
    n->prefix = prefix_len ? ar_strndup(&d->mem, prefix, prefix_len) : NULL;
    n->namespace_id = (uint8_t)namespace_id;
    n->namespace_uri = namespace_id == NS_OTHER ? ar_strdup(&d->mem, ns) : NULL;
    n->foreign = namespace_id != NS_HTML;
    bool upper = false;
    for (size_t i = 0; i < local_len; i++) if (local[i] >= 'A' && local[i] <= 'Z') { upper = true; break; }
    n->tag = namespace_id == NS_HTML && upper ? T_UNKNOWN : (uint16_t)tag_lookup(local, local_len);
    if (n->tag) n->name = tag_names[n->tag];
    else {
        char *lower_name = ar_strndup(&d->mem, local, local_len);
        for (size_t i = 0; i < local_len; i++) lower_name[i] = (char)lower((unsigned char)lower_name[i]);
        n->name = lower_name;
    }
}
/* DOM "create an element": a synchronous custom element constructor's result
   takes the requested namespace prefix after the constructor's checks. */
bool doc_element_prefix_set(node_t *n, const char *prefix) {
    if (!n || n->type != N_ELEM || !n->allocation_doc) return false;
    web_doc *d = n->allocation_doc;
    const char *local = n->local_name ? n->local_name : n->raw_name ? n->raw_name : n->name;
    size_t local_len = strlen(local), prefix_len = prefix ? strlen(prefix) : 0;
    if (prefix_len > SIZE_MAX - 2 - local_len) return false;
    jmp_buf trap;
    jmp_buf *old = d->mem.trap;
    d->mem.trap = &trap;
    if (setjmp(trap)) { d->mem.trap = old; return false; }
    size_t at = prefix_len ? prefix_len + 1 : 0;
    char *qualified = ar_alloc(&d->mem, at + local_len + 1);
    if (prefix_len) { memcpy(qualified, prefix, prefix_len); qualified[prefix_len] = ':'; }
    memcpy(qualified + at, local, local_len); qualified[at + local_len] = 0;
    char *kept_prefix = prefix_len ? ar_strndup(&d->mem, prefix, prefix_len) : NULL;
    d->mem.trap = old;
    n->raw_name = qualified; n->local_name = qualified + at; n->prefix = kept_prefix;
    return true;
}

static node_t *node_create(web_doc *d, int type, const char *name, const char *text, size_t len,
                           int namespace_id, const char *ns, const char *prefix) {
    if (!d) return NULL;
    size_t name_bytes=name?strlen(name):0, prefix_bytes=prefix?strlen(prefix):0;
    if(type<N_DOC || type>N_ATTR || len==SIZE_MAX || (len && !text) ||
       (type==N_ELEM && (!name || !*name || prefix_bytes>SIZE_MAX-2-name_bytes)) ||
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
    n->custom_registry_id = -2;
    n->image = -1;
    n->owned_next = d->owned_nodes;
    d->owned_nodes = n;
    if (type == N_ELEM) {
        element_names(d, n, namespace_id, ns, prefix, prefix_bytes, name, name_bytes);
        doc_image_node_owned(n);
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
/* An element is an HTML-namespace element without a prefix. */
node_t *doc_node_create(web_doc *d, int type, const char *name, const char *text, size_t len) {
    return node_create(d, type, name, text, len, NS_HTML, WEB_NS_HTML, NULL);
}
/* DOM "create an element" given a local name, namespace (NULL: none) and
   prefix (NULL: none) that the caller validated and extracted. */
node_t *doc_element_create(web_doc *d, const char *ns, const char *prefix, const char *local) {
    if (prefix && !*prefix) prefix = NULL;
    return node_create(d, N_ELEM, local, NULL, 0, doc_namespace_id(ns), ns, prefix);
}

/* The element where DOM's namespace lookups begin for a node of any interface:
   a document's document element, an Attr's element, another node's parent
   element; doctypes and fragments (shadow roots too) have none. */
static const node_t *namespace_scope(const node_t *n) {
    if (!n) return NULL;
    switch (n->type) {
    case N_ELEM: return n;
    case N_DOC:
        for (const node_t *c = n->first; c; c = c->next) if (c->type == N_ELEM) return c;
        return NULL;
    case N_DOCTYPE: case N_FRAGMENT: return NULL;
    case N_ATTR: return n->attr_owner;
    default: return n->parent && n->parent->type == N_ELEM ? n->parent : NULL;
    }
}
static const node_t *parent_element(const node_t *n) {
    return n->parent && n->parent->type == N_ELEM ? n->parent : NULL;
}
static bool same_string(const char *a, const char *b) { return a == b || (a && b && !strcmp(a, b)); }
const char *doc_locate_namespace(const node_t *n, const char *prefix) {
    if (prefix && !*prefix) prefix = NULL;
    for (const node_t *e = namespace_scope(n); e; e = parent_element(e)) {
        if (prefix && !strcmp(prefix, "xml")) return WEB_NS_XML;
        if (prefix && !strcmp(prefix, "xmlns")) return WEB_NS_XMLNS;
        const char *ns = doc_element_namespace(e);
        if (ns && same_string(e->prefix, prefix)) return ns;
        for (int i = 0; i < e->nattrs; i++) {
            const struct attr *a = &e->attrs[i];
            const char *local = a->local ? a->local : a->raw;
            if (!same_string(a->namespace_uri, WEB_NS_XMLNS)) continue;
            if (prefix ? a->prefix && !strcmp(a->prefix, "xmlns") && !strcmp(local, prefix) :
                         !a->prefix && !strcmp(local, "xmlns"))
                return a->value && *a->value ? a->value : NULL;
        }
    }
    return NULL;
}
/* DOM "list of elements with qualified name" (namespaced false) and "with
   namespace and local name" (namespaced: ns NULL is the null namespace, "*"
   in either is any): root's descendants in tree order, without shadow trees
   or template contents. */
void doc_elements_named(node_t *root, bool namespaced, const char *ns, const char *name, pvec *out) {
    if (!root || !name) return;
    bool any_name = !strcmp(name, "*"), any_ns = namespaced && ns && !strcmp(ns, "*");
    /* An HTML document's HTML elements match the ASCII-lowercased name. */
    char *folded = NULL;
    if (!namespaced && !any_name && root->owner && !root->owner->xml) {
        size_t length = strlen(name);
        folded = malloc(length + 1);
        if (!folded) return;
        for (size_t i = 0; i <= length; i++) folded[i] = (char)lower((unsigned char)name[i]);
    }
    for (node_t *c = root->first; c;) {
        if (c->type == N_ELEM) {
            bool match;
            if (namespaced) match = (any_ns || same_string(doc_element_namespace(c), ns)) &&
                                    (any_name || !strcmp(c->local_name ? c->local_name : c->name, name));
            else match = any_name || !strcmp(c->raw_name ? c->raw_name : c->name,
                                             folded && c->namespace_id == NS_HTML ? folded : name);
            if (match) pv_push(out, c);
        }
        if (c->first) c = c->first;
        else {
            while (c != root && !c->next) c = c->parent;
            if (c == root) break;
            c = c->next;
        }
    }
    free(folded);
}
const char *doc_locate_prefix(const node_t *n, const char *ns) {
    if (!ns || !*ns) return NULL;
    for (const node_t *e = namespace_scope(n); e; e = parent_element(e)) {
        if (e->prefix && same_string(doc_element_namespace(e), ns)) return e->prefix;
        for (int i = 0; i < e->nattrs; i++) {
            const struct attr *a = &e->attrs[i];
            if (a->prefix && !strcmp(a->prefix, "xmlns") && same_string(a->value, ns))
                return a->local ? a->local : a->raw;
        }
    }
    return NULL;
}

node_t *doc_doctype_create(web_doc *d, const char *name, size_t name_len,
    const char *public_id, size_t public_len, const char *system_id, size_t system_len) {
    if (!d || !name || name_len == SIZE_MAX || public_len == SIZE_MAX || system_len == SIZE_MAX ||
        (public_len && !public_id) || (system_len && !system_id)) return NULL;
    /* DOM doctype names are not XML QNames. Empty names are permitted; their
       only forbidden code points are ASCII whitespace, NUL and '>'. */
    for (size_t i = 0; i < name_len; i++)
        if (!name[i] || name[i] == '>' || name[i] == ' ' || name[i] == '\t' ||
            name[i] == '\n' || name[i] == '\r' || name[i] == '\f') return NULL;
    node_t *n = doc_node_create(d, N_DOCTYPE, name, NULL, 0);
    if (!n) return NULL;
    jmp_buf trap; jmp_buf *old = d->mem.trap; d->mem.trap = &trap;
    if (setjmp(trap)) { d->mem.trap = old; return NULL; }
    n->public_id = ar_strndup(&d->mem, public_id ? public_id : "", public_len);
    n->system_id = ar_strndup(&d->mem, system_id ? system_id : "", system_len);
    n->public_id_len = public_len; n->system_id_len = system_len;
    n->doctype_identifiers_sized = true;
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

/* A parser's attribute list for an element it just created, under the
   caller's arena trap: the list becomes the element's, with its id and classes. */
void doc_attrs_initial(web_doc *d, node_t *n, struct attr *attrs, int count) {
    node_t temp = *n;
    temp.attrs = attrs; temp.nattrs = count;
    attribute_cache(d, &temp);
    doc_attrs_publish(n, attrs, count);
    n->id = temp.id; n->classes = temp.classes; n->nclasses = temp.nclasses;
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
            bool fold = doc_html_names(n);
            while (*p && *q && (fold ? lower((unsigned char)*p) : *p) == (unsigned char)*q) { p++; q++; }
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
                         !strcmp(low, "crossorigin"))) ||
         (n->tag == T_source && n->parent && !n->parent->foreign && n->parent->tag == T_picture &&
          (!strcmp(low, "srcset") || !strcmp(low, "sizes") || !strcmp(low, "media") || !strcmp(low, "type"))));
    bool resources = (n->tag == T_img && !html_image) || resource_attribute(n, low);
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
    const char *old_value = found >= 0 ? n->attrs[found].value : NULL;
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
    if(!ns&&!n->foreign&&n->tag==T_meta&&(!strcmp(low,"name")||!strcmp(low,"content")))doc_referrer_meta(d,n);
    if(!ns&&!n->foreign&&n->tag==T_base&&!strcmp(low,"href")&&doc_node_root(n,false)==d->root&&
       !node_ancestor(n,T_template))d->metadata_valid=false;
    if (ordinary && !strcmp(low,"form")) doc_parser_form_set(n,NULL);
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
    struct css_attribute_change change = {low, old_value, value};
    changed(d, n, resources, ordinary ? &change : NULL);
    if(!ns && (!strcmp(low,"id") || !strcmp(low,"name")) && doc_node_root(n,false)==d->root)
        web_js_window_names_changed(d);
    if(ordinary&&n->tag==T_link&&(!strcmp(low,"rel")||!strcmp(low,"href")))web_js_link_changed(d);
    /* These native HTML semantics are consumed while building boxes rather
       than encoded in computed CSS. Equal styles therefore do not imply the
       old marker text, suppressed descendants, or atomic kind is reusable. */
    bool semantic_boxes = ordinary &&
        ((!strcmp(low,"colspan") || !strcmp(low,"rowspan")) ||
         (n->tag == T_ol && (!strcmp(low,"start") || !strcmp(low,"reversed"))) ||
         (n->tag == T_li && !strcmp(low,"value")) ||
         (n->tag == T_details && !strcmp(low,"open")) ||
         (n->tag == T_input && !strcmp(low,"type")) ||
         (n->tag == T_audio && !strcmp(low,"controls")));
    if (semantic_boxes && doc_node_root(n, true) == d->root) {
        d->need_boxes=true; d->layout_valid=false; boxes_note_regen(d,n);
    }
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
        doc_shadow_dirty(n->parent && n->parent->shadow_root ? n->parent->shadow_root : doc_node_root(n, false));
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
    if (doc_html_names(n)) for (char *p = normalized; *p; p++) *p = (char)lower((unsigned char)*p);
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
    html_bridge_native_changed(p, HTML_BRIDGE_CHILDREN);
    html_bridge_native_changed(n->prev, HTML_BRIDGE_LINKS);
    html_bridge_native_changed(n->next, HTML_BRIDGE_LINKS);
    html_bridge_native_changed(n, HTML_BRIDGE_LINKS);
    stylesheet_detach(n);
    if (n->prev) n->prev->next = n->next; else p->first = n->next;
    if (n->next) n->next->prev = n->prev; else p->last = n->prev;
    node_t *next = n->next;
    n->parent = n->prev = n->next = NULL;
    if (n->type == N_ELEM) { n->elem_index = 0; indices(p, next); }
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
    return (p->shadow_root && slottable(c)) ||
        (c->type == N_ELEM && doc_node_root(p, false)->shadow_host);
}
static void assignment_dirty(node_t *p, node_t *c) {
    if (!assignment_structure(p, c)) return;
    if (!p || !c) return;
    if (p->shadow_root && slottable(c)) doc_shadow_dirty(p->shadow_root);
    if (c->type == N_ELEM) doc_shadow_dirty(doc_node_root(p, false));
}

/* Private status: 0 valid, 1 hierarchy, 2 reference-not-found. This performs
   no adoption, detach, Range migration or observer delivery. Validate a whole
   fragment before moving its first child, including Document's order rules. */
static int insertion_validity(node_t *p, node_t *c, node_t *before, node_t *replaced, bool replace_all) {
    if (!p || !c || (p->type != N_DOC && p->type != N_ELEM && p->type != N_FRAGMENT) ||
        c->type == N_DOC || c->type == N_ATTR) return 1;
    for (node_t *a = p; a; a = a->parent ? a->parent : a->shadow_host ? a->shadow_host : a->template_host)
        if (a == c) return 1;
    if (before && before->parent != p) return 2;
    if (before == c) before = c->next;
    if (p->type != N_DOC) return c->type == N_DOCTYPE ? 1 : 0;
    node_t *first = c->type == N_FRAGMENT ? c->first : c;
    unsigned elements = 0, doctypes = 0;
    for (node_t *n = first; n; n = c->type == N_FRAGMENT ? n->next : NULL) {
        if (n->type == N_TEXT || n->type == N_DOC || n->type == N_ATTR || n->type == N_FRAGMENT) return 1;
        if (n->type == N_ELEM && ++elements > 1) return 1;
        if (n->type == N_DOCTYPE && ++doctypes > 1) return 1;
    }
    if (c->type == N_FRAGMENT && doctypes) return 1;
    bool preceding = true;
    for (node_t *n = p->first; n; n = n->next) {
        if (n == before) preceding = false;
        if (replace_all || n == c || n == replaced) continue;
        if (elements && (n->type == N_ELEM || (!preceding && n->type == N_DOCTYPE))) return 1;
        if (doctypes && (n->type == N_DOCTYPE || (preceding && n->type == N_ELEM))) return 1;
    }
    return 0;
}

int doc_node_insert_validity(node_t *p, node_t *c, node_t *before) {
    return insertion_validity(p,c,before,NULL,false);
}

/* DOM "move", the pre-move validity checks in their order: 0, 1 HierarchyRequestError or
   2 NotFoundError. One shadow-including root keeps a move within one connectedness. */
int doc_node_move_validity(node_t *p, node_t *c, node_t *before) {
    if (!p || !c || (p->type != N_DOC && p->type != N_ELEM && p->type != N_FRAGMENT)) return 1;
    if (doc_node_root(p, true) != doc_node_root(c, true)) return 1;
    for (node_t *n = p; n; n = n->parent ? n->parent : n->shadow_host ? n->shadow_host : n->template_host)
        if (n == c) return 1;
    if (before && before->parent != p) return 2;
    if (c->type != N_ELEM && c->type != N_TEXT && c->type != N_COMMENT && c->type != N_PI) return 1;
    if (c->type == N_TEXT && p->type == N_DOC) return 1;
    if (p->type == N_DOC && c->type == N_ELEM) {
        for (node_t *n = p->first; n; n = n->next) if (n->type == N_ELEM) return 1;
        if (before && before->type == N_DOCTYPE) return 1;
        for (node_t *n = before ? before->next : NULL; n; n = n->next) if (n->type == N_DOCTYPE) return 1;
    }
    return 0;
}

int doc_node_replace_validity(node_t *p, node_t *c, node_t *old) {
    /* Do not remove Document's old root merely to make insertion validation
       pass. Its exclusion is part of validation, before any tree mutation. */
    if(!old)return 2;
    return insertion_validity(p,c,old,old,false);
}
int doc_node_replace_all_validity(node_t *p,node_t *c) {
    if(!c)return !p || (p->type!=N_DOC&&p->type!=N_ELEM&&p->type!=N_FRAGMENT);
    return insertion_validity(p,c,NULL,NULL,true);
}

void doc_parser_form_set(node_t *node,node_t *form){
    if(!node || node->parser_form_owner==form)return;
    html_bridge_native_changed(node, HTML_BRIDGE_FIELDS);
    web_doc *d=node->owner;if(!d)return;
    if(node->parser_form_owner){
        node_t **link=&d->parser_form_nodes;
        while(*link && *link!=node)link=&(*link)->parser_form_next;
        if(*link)*link=node->parser_form_next;
    }
    node->parser_form_owner=form;node->parser_form_next=NULL;
    if(form){node->parser_form_next=d->parser_form_nodes;d->parser_form_nodes=node;}
}
static void reset_parser_forms(node_t *root) {
    web_doc *d=root->owner;
    if (!d || !d->parser_form_nodes) return;
    /* An unassociated leaf cannot move a control or its parser-owned form.
       A leaf form is the exception: it can own foster-parented controls. */
    if (!root->first && !root->shadow_root && !root->template_content && !root->parser_form_owner &&
        (root->type != N_ELEM || root->foreign || root->tag != T_form)) return;
    /* Only registered parser associations can need this reset. Do not walk a
       large moved ordinary subtree once per insertion merely because another
       control elsewhere in its document was parser-associated. Templates have
       a separate inert owner, so include that owner's registrations as well.
       Removing through the current list link avoids a second head-to-node
       search for every cleared association. */
    web_doc *owners[2] = {d, d->template_owner ? NULL : d->template_doc};
    for (unsigned owner = 0; owner < 2; owner++) {
        web_doc *document = owners[owner];
        if (!document) continue;
        for (node_t **link = &document->parser_form_nodes; *link;) {
            node_t *n = *link, *f = n->parser_form_owner, *ancestor = n;
            while (ancestor && ancestor != root)
                ancestor = ancestor->parent ? ancestor->parent : ancestor->shadow_host ? ancestor->shadow_host : ancestor->template_host;
            /* A moved form may own foster-parented controls outside its
               subtree. Preserve the ordinary-root separation check. */
            if (ancestor == root || doc_node_root(n,false) != doc_node_root(f,false)) {
                html_bridge_native_changed(n, HTML_BRIDGE_FIELDS);
                *link = n->parser_form_next;
                n->parser_form_owner = n->parser_form_next = NULL;
            } else link = &n->parser_form_next;
        }
    }
}
/* insert c into p before before, first removing it from its parent; preserve: DOM "move", which
   runs neither the removing steps nor the insertion steps (the frame keeps its document, a modal
   dialog stays in the top layer), only the moving steps */
static bool insert_node(web_doc *d, node_t *p, node_t *c, node_t *before, bool preserve) {
    if (!d || !p || !c || p->owner != d ||
        (preserve ? doc_node_move_validity(p, c, before) : doc_node_insert_validity(p, c, before))) return false;
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
        while (c->first) if (!insert_node(d, p, c->first, before, false)) return false;
        return true;
    }
    if (!may_insert(p, c)) return false;
    bool same_lifetime_forest = web_js_nodes_same_forest(d, c, p);
    if (c->owner != d && !doc_node_adopt(d, c)) return false;
    node_t *old_parent = c->parent;
    node_t *old_select = node_ancestor(old_parent, T_select);
    if (is_slot(old_parent) || is_slot(p)) doc_shadow_flush(d);
    assignment_dirty(old_parent, c); assignment_dirty(p, c);
    if (is_slot(old_parent) && !old_parent->slot_assigned_first && doc_node_root(old_parent, false)->shadow_host)
        doc_slot_signal(old_parent);
    if(old_parent&&!preserve)web_dialog_removed(d,c);
    if(old_parent&&!preserve)web_frames_detach_tree(d,c);
    detach(c);
    c->parent = p;
    c->next = before;
    c->prev = before ? before->prev : p->last;
    if (c->prev) c->prev->next = c; else p->first = c;
    if (before) before->prev = c; else p->last = c;
    html_bridge_native_changed(c, HTML_BRIDGE_LINKS);
    html_bridge_native_changed(c->prev, HTML_BRIDGE_LINKS);
    html_bridge_native_changed(c->next, HTML_BRIDGE_LINKS);
    if (c->type == N_ELEM) indices(p, c);
    reset_parser_forms(c);
    if (is_slot(p) && !p->slot_assigned_first && doc_node_root(p, false)->shadow_host)
        doc_slot_signal(p);
    /* A move into a detached forest still removes content from the live tree. */
    if (old_parent && old_parent != p) structure_changed_lifetime(d, old_parent, c, !same_lifetime_forest, false);
    /* A reorder within p also changed the former position: not an insertion. */
    structure_changed_lifetime(d, p, c, !same_lifetime_forest, old_parent != p);
    doc_details_inserted(c);
    node_t *new_select = node_ancestor(p, T_select);
    web_select_inserted(d, c, old_parent);
    if (old_select && old_select != new_select) web_select_sync(old_select->owner, old_select, false);
    if (new_select) web_select_sync(new_select->owner, new_select, false);
    return true;
}
bool doc_node_move(web_doc *d, node_t *p, node_t *c, node_t *before) { return insert_node(d, p, c, before, false); }
bool doc_node_move_preserving(web_doc *d, node_t *p, node_t *c, node_t *before) {
    return c && c->parent && insert_node(d, p, c, before, true);
}

void doc_node_remove(web_doc *d, node_t *n) {
    if (!d || !n || !n->parent) return;
    node_t *p = n->parent;
    node_t *select = node_ancestor(p, T_select);
    if (is_slot(p)) doc_shadow_flush(d);
    assignment_dirty(p, n);
    if (shadow_under(d->focus, n)) { d->focus = NULL; d->caret = 0; }
    if (is_slot(p) && !p->slot_assigned_first && doc_node_root(p, false)->shadow_host)
        doc_slot_signal(p);
    web_frames_detach_tree(d,n);
    detach(n);
    reset_parser_forms(n);
    css_node_style_unpublish(n); /* inherited borrows cannot outlive the former parent */
    structure_changed(d, p, n);
    web_select_inserted(d, n, p);
    if (select) web_select_sync(select->owner, select, false);
}

bool doc_node_replace(web_doc *d, node_t *p, node_t *c, node_t *old) {
    if(!d || !p || p->owner!=d || doc_node_replace_validity(p,c,old))return false;
    node_t *reference=old->next;
    if(reference==c)reference=c->next;
    /* Adoption reserves a needed template owner before detaching its source.
       Complete that potentially failing work BEFORE removing the old child.
       Subsequent moves already have the destination document as their owner. */
    if(!doc_node_adopt(d,c))return false;
    if(old->parent)doc_node_remove(d,old);
    return doc_node_move(d,p,c,reference);
}
bool doc_node_replace_all(web_doc *d,node_t *p,node_t *c) {
    if(!d||!p||p->owner!=d||doc_node_replace_all_validity(p,c))return false;
    /* Reserve/adopt before touching the old children, as with replacement. */
    if(c&&(c->owner!=d||c->parent)&&!doc_node_adopt(d,c))return false;
    while(p->first)doc_node_remove(d,p->first);
    return !c||doc_node_move(d,p,c,NULL);
}

static int32_t clone_document_registry(web_doc *d) {
    return d->custom_registry_id!=-2?d->custom_registry_id:web_js_enabled(d)?0:-1;
}
static void adopt_registry(web_doc *document,node_t *node) {
    if(node->type!=N_ELEM && !node->shadow_host)return;
    int32_t registry=node->custom_registry_id;
    if(registry==-2)registry=clone_document_registry(node->owner);
    if(registry>0)return; /* A scoped registry keeps its identity. */
    if(node->shadow_host && registry==-1 && node->shadow_keep_registry_null)return;
    int32_t destination=clone_document_registry(document);
    if(!node->shadow_host && registry==-1 && node->parent &&
       !(node->parent->type==N_FRAGMENT && !node->parent->shadow_host)){
        node_t *parent=node->parent;
        destination=parent->custom_registry_id==-2?clone_document_registry(parent->owner):parent->custom_registry_id;
    }
    /* Adoption chooses only the effective global registry of the document or
       parent; it must not accidentally opt a null element into a scoped one. */
    node->custom_registry_id=destination==0?0:-1;
    node->custom_registry_realm=destination==0?document->custom_registry_realm:NULL;
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
    if (n->owner != owner && n->adopted_count) {
        free(n->adopted_sheets); n->adopted_sheets = NULL; n->adopted_count = 0;
    }
    if(n->owner!=owner){adopt_registry(owner,n);n->style_scope=NULL;}
    doc_parser_form_set(n,NULL);n->owner = owner;
    html_bridge_native_changed(n, HTML_BRIDGE_FIELDS | HTML_BRIDGE_LINKS);
    for (int i = 0; i < n->nattrs; i++) if (n->attrs[i].node) n->attrs[i].node->owner = owner;
    n->style = n->animation_base_style = n->animation_after_style = NULL; n->box = n->anchor_block = NULL;
    n->image = -1; n->image_request = -1; n->image_initialized = false;
    n->image_generation++;
    /* Adoption is an image-data mutation even without a later insertion or
       getter. Keep allocation ownership untouched; schedule the live scan. */
    if (!owner->inert && n->type == N_ELEM && !n->foreign && n->tag == T_img)
        owner->resources_dirty = owner->dirty = owner->need_style = true;
    if (n->shadow_root)
        (owner->dom_family ? owner->dom_family : owner)->has_shadow = true;
    /* Queue after this shadow root itself changed owner; queuing from its
     * host iteration would put the migrated root back in the source family. */
    if (n->shadow_host) doc_shadow_dirty(n);
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
    web_doc *previous_owner=n->owner;
    if (n->parent) doc_node_remove(n->owner ? n->owner : d, n);
    if(previous_owner && previous_owner!=d && previous_owner->root_box) {
        /* Cross-document adoption must not leave the source box tree borrowing
           element-owned style arenas reclaimed by a destination layout read. */
        boxes_discard(previous_owner);previous_owner->need_boxes=true;previous_owner->layout_valid=false;
    }
    if (n->owner) doc_shadow_flush(n->owner); /* drain the source-family queue before owner migration */
    adopt_subtree(d, n);
    web_js_nodes_reparented(d,n);
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
        /* HTML "appropriate template contents owner document": an HTML
           document's is an HTML document, an XML document's an XML document. */
        if (d->xml) { owner->xml = true; owner->content_type = strdup("application/xml"); }
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

static bool ordinary_text_geometry(node_t *n) {
    /* The conservative boundary keeps control/resource, SVG serialization and
       slot/shadow assignment styling on their established full-cascade path. */
    for(node_t *p=n;p;p=p->parent) {
        if(p->foreign || p->shadow_root || p->shadow_host || p->assigned_slot || p->manual_slot)return false;
        if(p->type==N_ELEM) switch(p->tag) {
        case T_style:case T_title:case T_script:case T_input:case T_textarea:
        case T_select:case T_option:case T_slot:return false;
        default:break;
        }
    }
    return true;
}
bool doc_node_text(web_doc *d, node_t *n, const char *text, size_t len) {
    if (!d || !n || len == SIZE_MAX || (len && !text)) return false;
    if (n->type == N_ATTR) return doc_attr_value(d, n, text);
    if (n->type == N_DOC || n->type == N_DOCTYPE) return true;
    bool resources=resource_ancestor(n),ordinary=!resources && ordinary_text_geometry(n);
    bool keep_style=false,rebuild_boxes=false,was_need_style=d->need_style;
    doc_dom_budget(d);
    if (n->type == N_TEXT || n->type == N_COMMENT || n->type == N_PI) {
        bool same=n->textlen==len && (!len || (n->text && !memcmp(n->text,text,len)));
        keep_style=ordinary && (n->type!=N_TEXT || ((n->textlen!=0)==(len!=0)));
        rebuild_boxes=n->type==N_TEXT && !same;
        /* Same-value CharacterData still goes through mutation/revision hooks.
           Only its identical native string/storage/geometry can be reused. */
        if(!same) {
            jmp_buf trap;
            jmp_buf *old = d->mem.trap;
            d->mem.trap = &trap;
            if (setjmp(trap)) { d->mem.trap = old; return false; }
            char *v = ar_strndup(&d->mem, text, len);
            n->text = v;
            n->textlen = len;
            d->mem.trap = old;
            if (keep_style && rebuild_boxes && boxes_update_text(d,n)) {
                rebuild_boxes=false; d->layout_valid=false;
            }
        }
        if(n->type==N_PI){n->pi_attributes_ready=false;n->pi_attribute_count=0;n->pi_attributes=NULL;}
    } else {
        bool had_text=false,text_children=true;
        for(node_t *child=n->first;child;child=child->next) {
            if(child->type==N_TEXT){if(child->textlen)had_text=true;}
            else if(child->type!=N_COMMENT && child->type!=N_PI){text_children=false;break;}
        }
        keep_style=ordinary && text_children && (had_text==(len!=0));
        node_t *t = len ? doc_node_create(d, N_TEXT, NULL, text, len) : NULL;
        if (len && !t) return false;
        while (n->first) doc_node_remove(d, n->first);
        if (t && !doc_node_move(d, n, t, NULL)) return false;
        /* Keep real replacement identities, Range adjustments and all native
           insertion/removal notifications. No author callback runs inside the
           text-only transaction, so its selector-invariant invalidations can
           be downgraded without clearing an already pending cascade. */
        if(keep_style){d->need_style=was_need_style;rebuild_boxes=true;}
    }
    cssom_style_text_changed(n);
    changed_geometry(d,n,resources,!keep_style,rebuild_boxes,NULL,false,NULL);
    return true;
}

bool doc_node_html(web_doc *d, node_t *n, const char *html, size_t len) {
    if (!d || !n || (n->type != N_ELEM && n->type != N_FRAGMENT) || len == SIZE_MAX || (len && !html)) return false;
    /* The insertion target owns the fragment, but the host/context document
       supplies scripting and compatibility (not the inert template owner). */
    node_t *context = n->shadow_host?n->shadow_host:n->template_host?n->template_host:n;
    if (n->type == N_ELEM && !n->foreign && n->tag == T_template) {
        n = doc_template_content(d, n);
        if (!n) return false;
        d = n->owner;
    }
    doc_dom_budget(d);
    web_doc *context_document=context->owner?context->owner:d;
    enum html_scripting_mode mode=web_js_enabled(context_document)?HTML_SCRIPTING_INERT:HTML_SCRIPTING_DISABLED;
    node_t *fragment = html_fragment_mode(d,n,context,html,len,false,mode);
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
    css_mark_form_state(d, n);
    d->dirty = true;
    if(n->box)layout_invalidate(n->box);
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

static void clone_registry(web_doc *d,node_t *copy,const node_t *source,int32_t fallback,void *fallback_realm) {
    int32_t registry=source->custom_registry_id;
    void *realm=source->custom_registry_realm;
    if(registry==-2 && source->owner){
        registry=clone_document_registry(source->owner);
        realm=source->owner->custom_registry_realm;
    }
    if(registry==-1 || registry==-2){registry=fallback;realm=fallback_realm;}
    if(registry==0){
        /* A global registry is never borrowed from another Document. Scoped
           registries retain their identity; the destination's effective
           global registry is null when its registry is scoped or null. */
        registry=clone_document_registry(d)==0?0:-1;
        realm=registry==0?d->custom_registry_realm:NULL;
    }
    copy->custom_registry_id=registry;
    copy->custom_registry_realm=realm;
}
static node_t *clone_one(web_doc *d, node_t *n, int32_t fallback, void *fallback_realm) {
    if (!d || !n || n->type == N_DOC || n->shadow_host) return NULL;
    if (n->type == N_ATTR) {
        struct attr *a = n->attribute;
        return doc_attr_create(d, a->namespace_uri, a->prefix, a->local ? a->local : a->raw, a->value);
    }
    node_t *c = n->type == N_ELEM ?
        doc_element_create(d, doc_element_namespace(n), n->prefix, n->local_name ? n->local_name : n->raw_name) :
        doc_node_create(d, n->type, n->raw_name ? n->raw_name : n->name, n->text, n->textlen);
    if (!c) return NULL;
    c->cdata = n->cdata;
    clone_registry(d,c,n,fallback,fallback_realm);
    if (n->custom_is) {
        jmp_buf trap; jmp_buf *old = d->mem.trap; d->mem.trap = &trap;
        if (setjmp(trap)) { d->mem.trap = old; return NULL; }
        c->custom_is = ar_strdup(&d->mem, n->custom_is);
        d->mem.trap = old;
    }
    if (n->type == N_DOCTYPE) {
        /* Doctype strings are arena-owned like ordinary attributes. */
        jmp_buf trap; jmp_buf *old = d->mem.trap; d->mem.trap = &trap;
        if (setjmp(trap)) { d->mem.trap = old; return NULL; }
        c->public_id_len = n->doctype_identifiers_sized ? n->public_id_len : n->public_id ? strlen(n->public_id) : 0;
        c->system_id_len = n->doctype_identifiers_sized ? n->system_id_len : n->system_id ? strlen(n->system_id) : 0;
        c->public_id = ar_strndup(&d->mem, n->public_id ? n->public_id : "", c->public_id_len);
        c->system_id = ar_strndup(&d->mem, n->system_id ? n->system_id : "", c->system_id_len);
        c->doctype_identifiers_sized = true;
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
    int32_t fallback_registry;
    void *fallback_realm;
    unsigned stage;
    bool deep;
};
node_t *doc_node_clone_registry(web_doc *d, node_t *n, bool deep, int32_t fallback_registry, void *fallback_realm) {
    if(!d)return NULL;
    if(fallback_registry==-2){fallback_registry=clone_document_registry(d);fallback_realm=d->custom_registry_realm;}
    node_t *root = clone_one(d, n, fallback_registry, fallback_realm);
    if (!root || n->type == N_ATTR) return root;
    size_t capacity = 16, used = 1;
    struct clone_walk *stack = malloc(capacity * sizeof *stack);
    if (!stack) return NULL;
    stack[0] = (struct clone_walk){.source=n, .copy=root, .deep=deep,
        .fallback_registry=fallback_registry, .fallback_realm=fallback_realm};
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
                at->target->shadow_keep_registry_null = shadow->shadow_keep_registry_null;
                clone_registry(copy->owner,at->target,shadow,-1,NULL);
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
        /* HTML template cloning uses its contents owner and no fallback.
           DOM's clonable-shadow steps likewise deliberately omit fallback.
           Snapshot before growth; realloc can retire the current walk slot. */
        int32_t child_registry=at->stage==2?at->fallback_registry:-1;
        void *child_realm=at->stage==2?at->fallback_realm:NULL;
        if (used == capacity) {
            size_t max = SIZE_MAX / sizeof *stack;
            if (capacity == max) { free(stack); return NULL; }
            size_t next = capacity > max / 2 ? max : capacity * 2;
            struct clone_walk *grown = realloc(stack, next * sizeof *stack);
            if (!grown) { free(stack); return NULL; }
            stack = grown; capacity = next;
        }
        node_t *made = clone_one(target->owner, child, child_registry, child_realm);
        if (!made) { free(stack); return NULL; }
        stack[used++] = (struct clone_walk){.source=child, .copy=made, .attach=target, .deep=true,
            .fallback_registry=child_registry, .fallback_realm=child_realm};
    }
    free(stack); return root;
}
node_t *doc_node_clone(web_doc *d, node_t *n, bool deep) {
    return doc_node_clone_registry(d,n,deep,-1,NULL);
}
