/* HTML details/summary native behavior and element-task state.
 * https://html.spec.whatwg.org/multipage/interactive-elements.html
 * Dialog's top layer, inertness and modal focus are deliberately not claimed. */
#include "elements.h"

static bool html_tag(const node_t *n, int tag) {
    return n && n->type == N_ELEM && !n->foreign && n->tag == tag;
}

node_t *doc_details_summary(const node_t *details) {
    if (!html_tag(details, T_details)) return NULL;
    for (node_t *n = details->first; n; n = n->next)
        if (html_tag(n, T_summary)) return n;
    return NULL;
}

static bool interactive_descendant(node_t *n) {
    if (!n || n->type != N_ELEM || n->foreign) return false;
    switch (n->tag) {
    case T_a: return node_attr(n, "href") != NULL;
    case T_input: {
        const char *type = node_attr(n, "type");
        return !type || !str_ieq(type, "hidden");
    }
    case T_button: case T_select: case T_textarea: case T_label: case T_details: return true;
    case T_audio: case T_video: return node_attr(n, "controls") != NULL;
    case T_img: case T_object: return node_attr(n, "usemap") != NULL;
    default: return false;
    }
}

node_t *doc_details_activation(node_t *target) {
    for (node_t *n = target; n; n = doc_flat_parent(n)) {
        if (html_tag(n, T_summary)) {
            node_t *parent = n->parent;
            return doc_details_summary(parent) == n ? parent : NULL;
        }
        if (interactive_descendant(n)) return NULL;
    }
    return NULL;
}

bool doc_details_toggle(web_doc *d, node_t *details) {
    if (!html_tag(details, T_details)) return false;
    web_doc *owner = details->owner ? details->owner : d;
    return owner && doc_node_attr(owner, details, "open", node_attr(details, "open") ? NULL : "");
}

static void queue_toggle(node_t *details, bool old_open, bool new_open) {
    web_doc *owner = details->owner;
    if (!owner) return;
    web_doc *family = owner->dom_family ? owner->dom_family : owner;
    if (details->details_toggle_pending) {
        /* Coalescing removes the old task and queues its replacement at the
           end, rather than keeping its old position among other elements. */
        node_t *previous = NULL, *queued = family->details_toggle_first;
        while (queued && queued != details) { previous = queued; queued = queued->details_toggle_next; }
        if (queued) {
            if (previous) previous->details_toggle_next = details->details_toggle_next;
            else family->details_toggle_first = details->details_toggle_next;
            if (family->details_toggle_last == details) family->details_toggle_last = previous;
        }
    } else {
        details->details_toggle_pending = true;
        details->details_toggle_old_open = old_open;
    }
    details->details_toggle_next = NULL;
    if (family->details_toggle_last) family->details_toggle_last->details_toggle_next = details;
    else family->details_toggle_first = details;
    family->details_toggle_last = details;
    /* Preserve the first old state, even open->closed->open within one task. */
    details->details_toggle_new_open = new_open;
}

static node_t *details_next(node_t *n, node_t *root, bool shadow) {
    if (n->first) return n->first;
    if (shadow && n->shadow_root) return n->shadow_root;
    while (n != root) {
        if (n->parent) {
            if (n->next) return n->next;
            n = n->parent;
            if (shadow && n->shadow_root) return n->shadow_root;
        } else if (shadow && n->shadow_host) n = n->shadow_host;
        else return NULL;
    }
    return NULL;
}
static node_t *open_peer(node_t *root, node_t *details, const char *name) {
    /* Name groups use the DOM tree, not the flat or shadow-including tree. */
    for (node_t *n = root; n; n = details_next(n, root, false))
        if (n != details && html_tag(n, T_details) && node_attr(n, "open")) {
            const char *other_name = node_attr(n, "name");
            if (other_name && !strcmp(name, other_name)) return n;
        }
    return NULL;
}

static void enforce_group(node_t *details, bool close_self) {
    const char *name = node_attr(details, "name");
    if (!name || !*name || !node_attr(details, "open")) return;
    node_t *root = doc_node_root(details, false), *peer;
    if (!root) return;
    while ((peer = open_peer(root, details, name))) {
        node_t *close = close_self ? details : peer;
        if (!doc_node_attr(close->owner, close, "open", NULL)) return;
        if (close_self) return;
    }
}

void doc_details_attribute_changed(web_doc *d, node_t *details, const char *name, bool old_present) {
    (void)d;
    if (!html_tag(details, T_details) || !name) return;
    if (!strcmp(name, "name")) enforce_group(details, true);
    else if (!strcmp(name, "open")) {
        bool open = node_attr(details, "open") != NULL;
        if (open != old_present) queue_toggle(details, old_present, open);
        if (open && !old_present) enforce_group(details, false);
    }
}

void doc_details_inserted(node_t *subtree) {
    for (node_t *n = subtree; n; n = details_next(n, subtree, true))
        if (html_tag(n, T_details)) enforce_group(n, true);
}

void doc_details_parser_attribute_changed(node_t *details, bool old_open) {
    if (!html_tag(details, T_details)) return;
    bool open = node_attr(details, "open") != NULL;
    if (open != old_open) queue_toggle(details, old_open, open);
}

void doc_details_parser_finish(node_t *root) {
    /* A newly parsed group preserves its first open member in tree order.
       Closing peers uses ordinary native mutations, so queued toggle tasks
       and exported attributes also survive later script/parser boundaries. */
    for (node_t *n = root; n; n = details_next(n, root, true))
        if (html_tag(n, T_details) && node_attr(n, "open")) enforce_group(n, false);
}

node_t *doc_details_take_toggle(web_doc *d, bool *old_open, bool *new_open) {
    if (!d) return NULL;
    web_doc *family = d->dom_family ? d->dom_family : d;
    node_t *details = family->details_toggle_first;
    if (!details) return NULL;
    family->details_toggle_first = details->details_toggle_next;
    if (!family->details_toggle_first) family->details_toggle_last = NULL;
    if (old_open) *old_open = details->details_toggle_old_open;
    if (new_open) *new_open = details->details_toggle_new_open;
    details->details_toggle_pending = false;
    details->details_toggle_next = NULL;
    return details;
}
