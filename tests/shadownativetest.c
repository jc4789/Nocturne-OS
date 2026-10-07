/* Native tree/ownership/slot contracts, independent of JS wrapper identity.
   Run inside Nocturne; these tests do not assert real-site compatibility. */
#include <stdio.h>
#include "webi.h"

static int checks, failures;
static void check(const char *name, bool ok) {
    checks++;
    if (!ok) { failures++; printf("FAIL shadow native %s\n", name); }
}
static node_t *element(web_doc *d, const char *name) {
    node_t *n = doc_node_create(d, N_ELEM, name, NULL, 0);
    check("element allocation", n != NULL);
    return n;
}
static void clear_changes(web_doc *d) {
    web_doc *family = d->dom_family ? d->dom_family : d;
    for (node_t *n = family->shadow_slots_first, *next; n; n = next) {
        next = n->slot_change_next; n->slot_change_pending = false; n->slot_change_next = NULL;
    }
    family->shadow_slots_first = family->shadow_slots_last = NULL;
    family->shadow_slots_pending = false;
}
static bool list(node_t *n, bool slots, bool flatten, node_t *a, node_t *b) {
    pvec out = {0};
    if (slots) doc_slot_nodes(n, flatten, &out); else doc_flat_children(n, &out);
    bool ok = out.n == (a ? b ? 2 : 1 : 0) && (!a || out.v[0] == a) && (!b || out.v[1] == b);
    pv_free(&out); return ok;
}
int main(void) {
    const char html[] = "<!doctype html><html><head></head><body></body></html>";
    web_doc *d = web_parse(html, sizeof html - 1, "http://shadow.test/", "utf-8");
    if (!d) { puts("FAIL shadow native document allocation"); return 1; }
    node_t *host = element(d, "div"), *light = element(d, "span"), *other = element(d, "b");
    node_t *text = doc_node_create(d, N_TEXT, NULL, "light text", 10);
    node_t *comment = doc_node_create(d, N_COMMENT, NULL, "not slottable", 13);
    check("host insertion", doc_node_move(d, d->body, host, NULL));
    check("light insertion", doc_node_move(d, host, light, NULL));
    check("text insertion", text && doc_node_move(d, host, text, NULL));
    check("comment insertion", comment && doc_node_move(d, host, comment, NULL));
    check("named light", doc_node_attr(d, light, "slot", "named"));
    check("unassigned light insertion", doc_node_attr(d, other, "slot", "missing") && doc_node_move(d, host, other, NULL));
    node_t *root = doc_shadow_attach(d, host, false, true, true, true, false);
    check("root metadata", root && root->type == N_FRAGMENT && !root->parent && root->shadow_host == host &&
        host->shadow_root == root && !root->shadow_closed && root->shadow_delegates_focus && root->shadow_clonable && root->shadow_serializable);
    if (!root) { web_free(d); return 1; }
    check("double attach rejected", doc_shadow_attach(d, host, false, false, false, false, false) == NULL);
    node_t *slot = element(d, "slot"), *duplicate = element(d, "slot"), *fallback = element(d, "i");
    node_t *def = element(d, "slot"), *nested = element(d, "slot"), *nested_fallback = element(d, "strong");
    check("slot names", doc_node_attr(d, slot, "name", "named") && doc_node_attr(d, duplicate, "name", "named"));
    check("shadow insertion", doc_node_move(d, root, slot, NULL) && doc_node_move(d, root, duplicate, NULL) && doc_node_move(d, root, def, NULL));
    check("fallback insertion", doc_node_move(d, duplicate, fallback, NULL));
    check("native boundary", host->first == light && light->parent == host && root->first == slot && slot->parent == root);
    check("root identities", doc_node_root(slot, false) == root && doc_node_root(slot, true) == d->root && doc_node_root(light, false) == d->root);
    check("shadow connected", doc_node_connected(slot) && doc_node_connected(root));
    check("first named slot", doc_assigned_slot(light, true) == slot && list(slot, true, false, light, NULL));
    check("default text only", doc_assigned_slot(text, true) == def && !doc_assigned_slot(comment, false) && list(def, true, false, text, NULL));
    check("duplicate slot fallback", list(duplicate, true, false, NULL, NULL) && list(duplicate, true, true, fallback, NULL));
    check("flat assignment", doc_flat_parent(light) == slot && doc_flat_parent(slot) == host && list(slot, false, false, light, NULL));
    check("hidden unassigned", !doc_flat_parent(other) && !doc_flat_parent(comment));
    check("shadow cycle rejected", !doc_node_move(d, slot, host, NULL) && !doc_node_move(d, slot, root, NULL));
    check("direct shadow clone adopt rejected", !doc_node_clone(d, root, true) && !doc_node_adopt(d, root));
    clear_changes(d);
    check("reassignment rename", doc_node_attr(d, light, "slot", "") && doc_assigned_slot(light, false) == def &&
        list(def, true, false, light, text) && slot->slot_change_pending && def->slot_change_pending);
    check("slotchange FIFO tree order", d->shadow_slots_first == slot && slot->slot_change_next == def && d->shadow_slots_last == def);
    clear_changes(d);
    uint64_t reassigns = d->profile.shadow_reassigns;
    check("unrelated attribute no slotchange or assignment walk", doc_node_attr(d, light, "title", "unchanged assignment") &&
        !def->slot_change_pending && d->profile.shadow_reassigns == reassigns);
    check("character data no assignment walk", doc_node_text(d, text, "changed light text", 18) &&
        d->profile.shadow_reassigns == reassigns && list(def, true, false, light, text));
    /* A slot in fallback still participates in shadow-tree slot discovery.
       This nested default slot precedes def and therefore takes its assignment. */
    check("earlier nested default slot wins in shadow tree order", doc_node_attr(d, duplicate, "name", "fallback-only") &&
        doc_node_move(d, duplicate, nested, NULL) && doc_node_move(d, nested, nested_fallback, NULL) &&
        doc_assigned_slot(light, false) == nested && list(nested, true, false, light, text) && list(def, true, false, NULL, NULL));
    check("fallback nested", doc_node_attr(d, nested, "name", "nested-fallback-only") &&
        doc_assigned_slot(light, false) == def && list(duplicate, true, true, fallback, nested_fallback));
    check("fallback change signal", duplicate->slot_change_pending);
    node_t *copy = doc_node_clone(d, host, false);
    check("shallow host clone includes deep clonable root", copy && !copy->first && copy->shadow_root && copy->shadow_root != root &&
        copy->shadow_root->first && copy->shadow_root->first != slot && copy->shadow_root->shadow_delegates_focus);
    node_t *closed_host = element(d, "span"), *closed_light = element(d, "b");
    node_t *closed = doc_shadow_attach(d, closed_host, true, false, false, false, false), *closed_slot = element(d, "slot");
    check("closed native assignment", closed && doc_node_move(d, closed, closed_slot, NULL) && doc_node_move(d, closed_host, closed_light, NULL) &&
        !doc_assigned_slot(closed_light, true) && doc_assigned_slot(closed_light, false) == closed_slot);
    node_t *manual_host = element(d, "section"), *a = element(d, "a"), *b = element(d, "b");
    node_t *manual = doc_shadow_attach(d, manual_host, false, false, false, false, true), *m1 = element(d, "slot"), *m2 = element(d, "slot");
    check("manual insertion", manual && doc_node_move(d, manual, m1, NULL) && doc_node_move(d, manual, m2, NULL) &&
        doc_node_move(d, manual_host, a, NULL) && doc_node_move(d, manual_host, b, NULL));
    node_t *nodes[] = {b, a, b};
    check("manual ordered-set", doc_slot_assign(m1, nodes, 3) && list(m1, true, false, b, a) && doc_assigned_slot(a, true) == m1);
    clear_changes(d);
    check("manual identical assignments no signal", doc_slot_assign(m1, nodes, 3) && !d->shadow_slots_pending);
    check("manual reassignment", doc_slot_assign(m2, nodes + 1, 1) && list(m1, true, false, b, NULL) && list(m2, true, false, a, NULL));
    doc_node_remove(d, a);
    check("manual detached omitted", !doc_assigned_slot(a, false) && list(m2, true, false, NULL, NULL) && a->manual_slot == m2);
    check("manual reinsert retained", doc_node_move(d, manual_host, a, NULL) && list(m2, true, false, a, NULL));
    check("manual invalid atomic", !doc_slot_assign(m2, &comment, 1) && list(m2, true, false, a, NULL));
    check("manual clear", doc_slot_assign(m2, NULL, 0) && !a->manual_slot && !doc_assigned_slot(a, false));
    check("auto manual list retained without assignment", doc_slot_assign(slot, nodes, 1) && b->manual_slot == slot &&
        !slot->slot_assigned_first && doc_assigned_slot(light, false) == def);
    node_t *outside = element(d, "slot");
    check("detached slot manual list retained", doc_slot_assign(outside, &a, 1) && a->manual_slot == outside && !doc_assigned_slot(a, false));
    check("slot moved into manual root uses saved assignment", doc_node_move(d, manual, outside, NULL) &&
        doc_assigned_slot(a, false) == outside && list(outside, true, false, a, NULL));
    node_t *drain = element(d, "div");
    check("shadow fragment insertion moves only children", doc_node_move(d, drain, closed, NULL) && !closed->first &&
        !closed->parent && closed_host->shadow_root == closed && closed->shadow_host == closed_host && closed_slot->parent == drain);
    d->focus = nested_fallback;
    doc_node_remove(d, host);
    check("host removal disconnects shadow and focus", !doc_node_connected(nested_fallback) && !d->focus && root->shadow_host == host);
    web_doc *inert = doc_inert(d, "", 0, "about:blank");
    check("inert destination", inert != NULL);
    if (inert) {
        check("host adopt includes shadow ownership", doc_node_adopt(inert, host) && host->owner == inert && root->owner == inert &&
            nested_fallback->owner == inert && host->allocation_doc == d && root->allocation_doc == d);
        check("adopted host reinsertion", doc_node_move(inert, inert->body, host, NULL) && doc_node_connected(nested_fallback));
    }
    check("invalid built-in host", !doc_shadow_attach(d, element(d, "button"), false, false, false, false, false));
    check("custom host name", doc_shadow_host_valid(element(d, "x-test")) && doc_shadow_host_valid(element(d, "x-@test")) &&
        !doc_shadow_host_valid(element(d, "font-face")));
    web_free(d);
    printf("shadownativetest: %d checks, %d failures\n", checks, failures);
    return failures != 0;
}
