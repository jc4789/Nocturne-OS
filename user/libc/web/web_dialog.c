#include "web_dialog.h"
#include "form_value.h"
#include "form_validation.h"

struct dialog_entry { node_t *node, *previous; box_t *backdrop; };
struct web_dialog_state { int count; struct dialog_entry stack[WEB_DIALOG_LIMIT]; };

static bool valid(web_doc *d, const node_t *n) {
    return d && n && n->owner == d && n->type == N_ELEM && !n->foreign &&
           n->tag == T_dialog && node_attr(n, "open") && doc_node_connected((node_t *)n);
}
static int index_of(web_doc *d, const node_t *n) {
    struct web_dialog_state *s = d ? d->dialogs : NULL;
    for (int i = 0; s && i < s->count; i++) if (s->stack[i].node == n) return i;
    return -1;
}
static bool under(node_t *n, node_t *ancestor, bool flat) {
    for (; n; n = flat ? doc_flat_parent(n) : doc_shadow_parent(n)) if (n == ancestor) return true;
    return false;
}
bool web_dialog_is_modal(web_doc *d, const node_t *n) { return valid(d, n) && index_of(d, n) >= 0; }
node_t *web_dialog_top(web_doc *d) {
    struct web_dialog_state *s = d ? d->dialogs : NULL;
    for (int i = s ? s->count : 0; i > 0; i--)
        if (valid(d, s->stack[i - 1].node)) return s->stack[i - 1].node;
    return NULL;
}
int web_dialog_count(web_doc *d) { return d && d->dialogs ? d->dialogs->count : 0; }
node_t *web_dialog_at(web_doc *d, int index) {
    if (index < 0 || index >= web_dialog_count(d)) return NULL;
    node_t *n = d->dialogs->stack[index].node;
    return valid(d, n) ? n : NULL;
}
box_t *web_dialog_backdrop(web_doc *d, node_t *n) {
    int i = index_of(d, n); return i >= 0 ? d->dialogs->stack[i].backdrop : NULL;
}
void web_dialog_set_backdrop(web_doc *d, node_t *n, box_t *box) {
    int i = index_of(d, n); if (i >= 0) d->dialogs->stack[i].backdrop = box;
}
bool web_dialog_layer_box(web_doc *d, box_t *box) {
    return box && box->node && web_dialog_is_modal(d, box->node) &&
           (box->node->box == box || web_dialog_backdrop(d, box->node) == box);
}
node_t *web_dialog_leave(web_doc *d, node_t *n) {
    int i = index_of(d, n);
    if (i < 0) return NULL;
    struct web_dialog_state *s = d->dialogs;
    node_t *previous = s->stack[i].previous;
    /* Closing an older dialog cannot leave a later dialog's restoration anchor
       in that closed subtree. This also handles nested modal dialogs. */
    for (int j = i + 1; j < s->count; j++)
        if (under(s->stack[j].previous, n, false)) s->stack[j].previous = previous;
    memmove(s->stack + i, s->stack + i + 1, (size_t)(s->count - i - 1) * sizeof *s->stack);
    s->count--;
    d->dirty = d->need_style = true; d->layout_valid = false;
    return previous;
}
void web_dialog_sync(web_doc *d) {
    struct web_dialog_state *s = d ? d->dialogs : NULL;
    for (int i = 0; s && i < s->count;) {
        node_t *n = s->stack[i].node;
        if (valid(d, n)) { i++; continue; }
        if (under(d->focus, n, false)) { d->focus = NULL; d->caret = 0; }
        web_dialog_leave(d, n);
    }
}
void web_dialog_removed(web_doc *d, node_t *subtree) {
    struct web_dialog_state *s = d ? d->dialogs : NULL;
    for (int i = 0; s && i < s->count;) {
        node_t *n = s->stack[i].node;
        if (!under(n, subtree, false)) { i++; continue; }
        if (under(d->focus, n, false)) { d->focus = NULL; d->caret = 0; }
        web_dialog_leave(d, n);
    }
}
int web_dialog_prepare(web_doc *d, node_t *n) {
    if (!d || d->inert || !n || n->owner != d || !doc_node_connected(n)) return WEB_DIALOG_INACTIVE;
    web_dialog_sync(d);
    if (node_attr(n, "open")) return web_dialog_is_modal(d, n) ? WEB_DIALOG_ALREADY_MODAL : WEB_DIALOG_NONMODAL;
    if (!d->dialogs) d->dialogs = calloc(1, sizeof *d->dialogs);
    return !d->dialogs || d->dialogs->count == WEB_DIALOG_LIMIT ? WEB_DIALOG_CAPACITY : WEB_DIALOG_READY;
}
bool web_dialog_enter(web_doc *d, node_t *n) {
    if (!valid(d, n) || d->inert || !d->dialogs || d->dialogs->count == WEB_DIALOG_LIMIT || index_of(d, n) >= 0) return false;
    d->dialogs->stack[d->dialogs->count++] = (struct dialog_entry){.node=n, .previous=d->focus};
    d->dirty = d->need_style = true; d->layout_valid = false;
    return true;
}
void web_dialog_free(web_doc *d) { if (d) { free(d->dialogs); d->dialogs = NULL; } }
bool web_dialog_inert(web_doc *d, node_t *n) {
    node_t *top = web_dialog_top(d);
    if (top && !under(n, top, true)) return true;
    /* showModal escapes inherited inertness; an explicit inert attribute on
       the modal itself or its descendants still applies. */
    for (; n; n = doc_flat_parent(n)) {
        if (n->type == N_ELEM && node_attr(n, "inert")) return true;
        if (n == top) break;
    }
    return false;
}
bool web_node_inert(web_doc *d, web_node *n) { return web_dialog_inert(d,n); }
bool web_modal_active(web_doc *d) { return web_dialog_top(d) != NULL; }
void web_dialog_style(web_doc *d, node_t *n, style_t *st) {
    if (!web_dialog_is_modal(d, n)) return;
    if (st->display == D_CONTENTS) st->display = D_BLOCK;
    if (st->position != POS_FIXED && st->position != POS_ABSOLUTE) st->position = POS_ABSOLUTE;
    st->float_ = FL_NONE;
}
bool web_dialog_rendered(node_t *n) {
    for (node_t *a = n; a; a = doc_shadow_parent(a))
        if (a->type == N_ELEM && (!a->style || a->style->display == D_NONE)) return false;
    return n && n->style;
}

/* No synthetic DOM or unbounded candidate allocation. Traverse the renderer's
   flat tree, retaining at most first/last/neighbour candidates. */
struct focus_scan {
    web_doc *d; node_t *current, *first, *last, *next, *previous;
    uint64_t current_key, first_key, last_key, next_key, previous_key;
    int visits; bool seen, autofocus, tab;
};
static void scan_focus(struct focus_scan *s, node_t *n, int depth) {
    if (!n || depth > 400 || ++s->visits > 65536) return;
    if (n->type == N_ELEM) {
        if (!n->style || n->style->display == D_NONE || web_dialog_inert(s->d, n)) return;
        node_t *candidate = (!s->autofocus || node_attr(n, "autofocus")) ? web_focus_candidate(s->d, n) : NULL;
        const char *ti = candidate ? node_attr(candidate, "tabindex") : NULL;
        if (s->tab && ((ti && strtol(ti, NULL, 10) < 0) || (candidate && candidate->tag == T_dialog && !ti))) candidate = NULL;
        if (candidate) {
            if (s->tab) {
                long tab = ti ? strtol(ti, NULL, 10) : 0;
                if (tab > 2147483647L) tab = 2147483647L;
                uint64_t key = tab > 0 ? (uint64_t)tab << 32 | (uint32_t)s->visits :
                                         1ull << 63 | (uint32_t)s->visits;
                if (candidate == s->current && !s->seen) { s->current_key=key; s->seen=true; }
                if (!s->first || key<s->first_key) { s->first=candidate; s->first_key=key; }
                if (!s->last || key>s->last_key) { s->last=candidate; s->last_key=key; }
                if (candidate != s->current && s->seen && key<s->current_key && (!s->previous || key>s->previous_key)) { s->previous=candidate; s->previous_key=key; }
                if (candidate != s->current && s->seen && key>s->current_key && (!s->next || key<s->next_key)) { s->next=candidate; s->next_key=key; }
            } else {
                if (!s->first) s->first=candidate;
                s->last=candidate;
            }
        }
    }
    pvec children = {0}; doc_flat_children(n, &children);
    for (int i = 0; i < children.n; i++) scan_focus(s, children.v[i], depth + 1);
    pv_free(&children);
}
node_t *web_dialog_focus_target(web_doc *d, node_t *n) {
    if (!d || !n || !doc_node_connected(n)) return NULL;
    web_layout(d, d->width > 0 ? d->width : 800, d->height > 0 ? d->height : 600);
    if (node_attr(n, "autofocus") && web_focus_candidate(d, n)) return n;
    struct focus_scan s = {.d=d, .autofocus=true}; scan_focus(&s, n, 0);
    if (s.first) return s.first;
    s = (struct focus_scan){.d=d};
    pvec children = {0}; doc_flat_children(n, &children);
    for (int i = 0; i < children.n; i++) scan_focus(&s, children.v[i], 0);
    pv_free(&children);
    return s.first ? s.first : web_focus_candidate(d, n);
}
node_t *web_dialog_tab_target(web_doc *d, bool backward) {
    node_t *top = web_dialog_top(d); if (!top) return NULL;
    web_layout(d, d->width > 0 ? d->width : 800, d->height > 0 ? d->height : 600);
    struct focus_scan s = {.d=d, .current=d->focus, .tab=true}; scan_focus(&s, top, 0);
    /* Positive tabindex values precede zero/default values, ordered by numeric
       value and then flat-tree order. A second bounded pass finds both neighbours. */
    if (s.seen) {
        s = (struct focus_scan){.d=d, .current=d->focus, .tab=true, .seen=true, .current_key=s.current_key};
        scan_focus(&s,top,0);
    }
    return backward ? (s.previous ? s.previous : s.last ? s.last : top) :
                     (s.next ? s.next : s.first ? s.first : top);
}
void web_dialog_scroll_offset(web_doc *d, node_t *n, int *x, int *y) {
    for (; n; n = doc_flat_parent(n)) if (web_dialog_is_modal(d, n)) {
        if (n->style && n->style->position == POS_FIXED) { *x += d->view_x; *y += d->view_y; }
        return;
    }
}

int web_dialog_submission(web_doc *d, node_t *submitter, node_t **dialog, const char **result) {
    *dialog = NULL; *result = NULL;
    if (!d || !submitter || submitter->owner != d || submitter->type != N_ELEM || submitter->foreign) return 0;
    node_t *form = submitter->tag == T_form ? submitter : web_form_owner(d, submitter);
    if (!form || form->owner != d || form->type != N_ELEM || form->foreign || form->tag != T_form) return 0;
    bool button = web_control_submit_button(submitter);
    const char *method = button ? node_attr(submitter, "formmethod") : NULL;
    if (!method) method = node_attr(form, "method");
    /* Presence wins even for an empty/invalid override (its state is GET). */
    if (!method || !str_ieq(method, "dialog")) return 0;
    if (d->inert || !doc_node_connected(form)) return -1;
    /* Selected image coordinates are not recorded by the existing submit path.
       Never manufacture a result or silently close it as an ordinary button. */
    if (button && submitter->tag == T_input && web_input_type(submitter) == WEB_INPUT_IMAGE) return -1;
    /* The form's DOM ancestor, not the submitter's or the flat/shadow tree.
       External form-associated submit buttons consequently close the same owner. */
    for (node_t *n = form->parent; n; n = n->parent) {
        if (n->type != N_ELEM || n->foreign || n->tag != T_dialog) continue;
        if (!valid(d, n)) return -1;
        *dialog = n;
        if (button) *result = node_attr(submitter, "value");
        return 1;
    }
    return -1;
}
