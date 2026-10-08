#ifndef WEB_DIALOG_H
#define WEB_DIALOG_H
#include "webi.h"

/* Native top layer; DOM parent/child links and author inert attributes never change. */
#define WEB_DIALOG_LIMIT 16
enum { WEB_DIALOG_READY, WEB_DIALOG_ALREADY_MODAL, WEB_DIALOG_NONMODAL,
       WEB_DIALOG_INACTIVE, WEB_DIALOG_CAPACITY };
int web_dialog_prepare(web_doc *d, node_t *n);
bool web_dialog_enter(web_doc *d, node_t *n);
node_t *web_dialog_leave(web_doc *d, node_t *n);
void web_dialog_sync(web_doc *d);
void web_dialog_removed(web_doc *d, node_t *subtree);
void web_dialog_free(web_doc *d);
bool web_dialog_is_modal(web_doc *d, const node_t *n);
node_t *web_dialog_top(web_doc *d);
int web_dialog_count(web_doc *d);
node_t *web_dialog_at(web_doc *d, int index);
box_t *web_dialog_backdrop(web_doc *d, node_t *n);
void web_dialog_set_backdrop(web_doc *d, node_t *n, box_t *box);
bool web_dialog_layer_box(web_doc *d, box_t *box);
bool web_dialog_inert(web_doc *d, node_t *n);
void web_dialog_style(web_doc *d, node_t *n, style_t *st);
bool web_dialog_rendered(node_t *n);
node_t *web_dialog_focus_target(web_doc *d, node_t *n);
node_t *web_dialog_tab_target(web_doc *d, bool backward);
void web_dialog_scroll_offset(web_doc *d, node_t *n, int *x, int *y);
/* Final submission branch, after the caller's validation/submit event.
   0: ordinary method, 1: dialog target ready, -1: dialog method without a
   closable target. result is an optional native attribute value, not a label. */
int web_dialog_submission(web_doc *d, node_t *submitter, node_t **dialog, const char **result);
/* doc.c's existing native focusability decision, without changing focus. */
node_t *web_focus_candidate(web_doc *d, node_t *n);
#endif
