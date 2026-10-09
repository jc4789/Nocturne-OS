/* Nocturne's web engine: HTML, CSS, layout, painting and an optional QuickJS host.

   The embedder does the networking. A typical page load:
     web_doc *d = web_parse(html, len, final_url, charset);
     while ((u = web_pending_stylesheet(d))) web_stylesheet_loaded(d, css_or_NULL, len);
     web_layout(d, width, height);              -> paint it
     for each i < web_image_count(d) with web_image_wanted(d, i):
         web_image_loaded(d, i, bytes_or_NULL, n); web_layout(...) again now and then
   Coordinates passed to and returned by these functions are document coordinates: (0, 0) is the
   top-left of the page, before scrolling. */
#pragma once
#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>
#include "gfx.h"
#include "abi.h"
#include "webstorage.h"
#include "http.h"

typedef struct web_doc web_doc;
typedef struct node web_node; /* an element of the document */
struct web_hit;

/* charset: from the Content-Type header, or NULL (a <meta charset> or UTF-8 is used) */
web_doc *web_parse(const char *html, size_t len, const char *url, const char *charset);

/* Live documents. All callbacks run on the browser thread. request() must copy
   its arguments before returning. Completion only queues work, never runs JS.
   sync_load() is exclusively for the synchronous module loader: service native
   UI/networking but NEVER enter JS or replace/free the document in that callback.
   sync_load transfers malloc'd body and optional headers_full to the caller,
   including error responses; release them with web_response_free. Completion
   callbacks borrow these pointers only until they return. Resource ids belong
   to one document; the embedder must also
   check its navigation generation before delivering a completion. */
enum { WEB_RESOURCE_SCRIPT, WEB_RESOURCE_MODULE, WEB_RESOURCE_CSS,
       WEB_RESOURCE_IMAGE, WEB_RESOURCE_FETCH, WEB_RESOURCE_FRAME };
struct web_request {
    uint64_t id;
    int kind;
    const char *url, *method, *headers;
    const char *origin; /* initiating document's effective origin URL; NULL uses embedder document */
    const void *body;
    size_t body_len;
    int credentials; /* 0 omit, 1 same-origin, 2 include */
    bool force_preflight; /* XHR upload listeners require CORS preflight even with safe headers. */
    bool redirect_error, same_origin;
    bool image_upgrade; /* Native ordinary-image destination, never imageset/Fetch/script. */
    bool keepalive; /* Buffered Fetch may outlive this document, not the browser process. */
    uint64_t fetch_group; /* Native per-environment identity; not supplied by page headers. */
    int cache_mode; /* Fetch: 0 default, 1 no-store, 2 reload, 3 no-cache, 4 force-cache, 5 only-if-cached. */
};
enum { WEB_HISTORY_INFO, WEB_HISTORY_PUSH, WEB_HISTORY_REPLACE, WEB_HISTORY_GO, WEB_HISTORY_SCROLL };
struct web_history {
    uint64_t entry;
    int length;
    bool manual_scroll;
    const void *state; /* borrowed only until the host callback returns to JS binding */
    size_t state_len;
};
struct web_response {
    int status;
    char url[HTTP_URL_MAX], headers[4096], error[160];
    char *body;
    size_t body_len;
    char *headers_full; /* Complete block when too large for inline headers; never a truncated tail. */
    char *url_full; /* Complete owned URL for native transports; inline URL remains legacy-compatible. */
};
static inline const char *web_response_url(const struct web_response *r) {
    return r->url_full ? r->url_full : r->url;
}
bool web_response_set_url(struct web_response *r, const char *url);
/* Includes the native HTTP status/private metadata prefix. */
#define WEB_RESPONSE_HEADERS_MAX ((size_t)UINT32_MAX)
static inline const char *web_response_headers(const struct web_response *r) {
    return r->headers_full ? r->headers_full : r->headers;
}
void web_response_free(struct web_response *response);
/* Navigation observations are copied into the document, not retained as a
   pointer to a mutable browser request. All values use uptime_ms(), not wall
   clock time. Unobserved DNS/TLS/first-byte milestones are deliberately absent. */
struct web_navigation_timing {
    bool valid, fetch_valid, response_end_valid;
    uint64_t navigation_start_ms, fetch_start_ms, response_end_ms;
};
/* Optional native-host cancellation deadline. Normal pages are not rejected
   at an arbitrary 5/15/30-second cutoff. Page code cannot reset it. */
#define WEB_JS_TASK_DEFAULT_MS UINT32_MAX
#define WEB_JS_TASK_MIN_MS 1u
#define WEB_JS_TASK_MAX_MS UINT32_MAX
#define WEB_WINDOW_VISIBLE 1u
#define WEB_WINDOW_FOCUSED 2u
/* A bounded, native video-layer update from the last published document scene.
   The host validates identity/viewport before touching its canvas. */
struct web_video_patch {
    canvas_t canvas;
    int scroll_x, scroll_y;
    int x, y, w, h, pitch;
    const uint32_t *pixels;
};
struct web_host {
    void *opaque;
    bool (*request)(void *opaque, const struct web_request *request);
    void (*cancel)(void *opaque, uint64_t id);
    /* Document retirement, unlike explicit AbortSignal: detach callback delivery
       and retain a keepalive transport. A host without this cannot accept it. */
    void (*release_request)(void *opaque, uint64_t id);
    bool (*sync_load)(void *opaque, const char *url, int kind, struct web_response *response);
    /* Origin-aware module transport. Prefer this over the legacy top-document
       callback; origin/credentials are native initiating-realm values. Same
       native-only wait/response ownership contract as sync_load applies. */
    bool (*sync_request)(void *opaque, const struct web_request *request, struct web_response *response);
    void (*navigate)(void *opaque, const char *url, const char *post);
    /* Native form bytes are not a NUL-terminated urlencoded substitute.
       The host must copy transient data before returning to the JS task. */
    void (*navigate_form)(void *opaque, const char *url, const void *body,
                          size_t length, const char *content_type, const char *target);
    void (*console)(void *opaque, int level, const char *message);
    void (*scroll)(void *opaque, int *x, int *y);
    void (*scroll_to)(void *opaque, int x, int y);
    unsigned (*window_state)(void *opaque); /* actual host visibility/page focus; optional */
    /* Native-only getter: a queued navigation/close cancels stale default edits. */
    bool (*navigation_pending)(void *opaque);
    bool (*video_present)(void *opaque, web_doc *document,
                          const struct web_video_patch *patch);
    bool (*history)(void *opaque, int operation, const char *url, const void *state,
                    size_t state_len, int value, struct web_history *out);
    /* The host owns the cookie jar; returned text is malloc'd, never HttpOnly. */
    char *(*cookie_get)(void *opaque, const char *url);
    void (*cookie_set)(void *opaque, const char *url, const char *value);
    void (*navigate_mode)(void *opaque, const char *url, int mode); /* 0 assign, 1 reload, 2 replace */
    /* origin is derived from the native document URL by the private URL parser,
       never a page-selected directory. Output text transfers malloc ownership. */
    int (*storage)(void *opaque, const char *origin, const struct web_storage_request *request,
                   struct web_storage_result *out);
    /* Opt in only for the native browser embedder. Custom/fixture hosts keep
       their request() transport; this flag does not grant origin/CORS access. */
    bool media_range;
    /* Zero-initialized custom hosts keep the explicit document-init epoch. */
    struct web_navigation_timing navigation_timing;
    /* Native embedder-selected deadline; 0 keeps the uint32 representation
       maximum. Page code cannot reset it. */
    uint32_t js_task_budget_ms;
    /* Optional native-only cancellation checkpoint. NEVER dispatch JS/DOM,
       layout, paint, navigation or free a document here. False stops this
       task after a user close/stop request, not after a page-count quota. */
    bool (*script_checkpoint)(void *opaque);
};
bool web_set_url(web_doc *d, const char *url);
/* Call between JS tasks, after the host history position and document URL change. */
void web_history_event(web_doc *d, const char *old_url, bool popstate);
void web_visibility_event(web_doc *d);
web_doc *web_live(const char *html, size_t len, const char *url, const char *charset,
                  const struct web_host *host);
void web_tick(web_doc *d, uint64_t now_ms);
/* Private media/JS-worker children outlive a retired document until nonblocking reap.
 * No document/node callback is retained by this process-lifetime cleanup. */
void web_media_background(uint64_t now_ms);
int64_t web_media_background_deadline(uint64_t now_ms);
/* Native playback transitions are reported only when explicitly requested. */
void web_avmedia_debug(bool enabled);
/* -1: no deadline, otherwise an absolute uptime_ms() deadline. */
int64_t web_deadline(web_doc *d);
void web_resource_loaded(web_doc *d, uint64_t id, const struct web_response *response);
bool web_dirty(web_doc *d); /* consumes the legacy layout/paint notification */
bool web_paint_dirty(web_doc *d); /* consumes a paint-only media notification */
bool web_script_running(web_doc *d);
/* Explicit developer-console input only. It runs in the active page realm,
 * under the ordinary JS watchdog, and grants no native module/file access. */
bool web_console_eval(web_doc *d, const char *source, size_t length);
struct web_event {
    const char *type, *key;
    int x, y, button, key_code;
    bool bubbles, cancelable, ctrl, shift, alt;
    web_node *related_target;
    web_node *submitter; /* SubmitEvent only; NULL for implicit/no-button submit */
    int buttons;
    double delta_x, delta_y, delta_z;
    unsigned delta_mode; /* 0=pixels, 1=lines, 2=pages */
    bool synthetic; /* DOM .click() is not a trusted user click */
};
/* NULL target means window. false means preventDefault() was called. */
bool web_dispatch(web_doc *d, web_node *target, const struct web_event *event);
/* controls付きの実mediaだけを操作。JS clickのpreventDefault後は呼ばない。 */
bool web_media_activate(web_doc *d, web_node *target);
/* Coalesced viewport scrolling targets Document and bubbles to Window. The host
   calls this between JS tasks, not recursively from its scroll_to callback. */
void web_document_scroll(web_doc *d);
/* One native mouse movement, between JS tasks. target is the hit-tested element;
   NULL means outside the document. Snapshots ancestry before any handlers run,
   sends out/leave/over/enter as needed, then mousemove for a non-NULL target.
   event is pointer state: its type may be NULL, since the hook chooses types. */
void web_hover(web_doc *d, web_node *target, const struct web_event *event);
/* Native primary-button state only: no DOM event dispatch or author callbacks.
   Release also applies outside the page and when native capture/focus is lost. */
void web_active_press(web_doc *d, web_node *target);
void web_active_release(web_doc *d);
web_node *web_node_at(web_doc *d, int x, int y);
bool web_node_action(web_doc *d, web_node *target, struct web_hit *hit);
/* Capture before click dispatch, then resolve only that anchor after dispatch.
   Detached HTML anchors can navigate; their owner must still be the active d. */
web_node *web_link_activation_anchor(web_doc *d, web_node *target);
bool web_link_action(web_doc *d, web_node *anchor, struct web_hit *hit);
/* Trusted native link activation: true when a child-frame destination consumed
   the navigation. Script location changes use their own, stricter child path. */
bool web_frame_navigate(web_doc *top, web_node *anchor, const char *url);
/* Actual active child URL for native chrome only. The parent URL is unchanged.
   Selection validates the complete live embedding chain; no author callbacks. */
const char *web_frame_address(web_doc *top);
void web_frame_address_select(web_doc *top, web_node *node);
void web_free(web_doc *d);
const char *web_title(web_doc *d); /* "" if none */
const char *web_url(web_doc *d);
/* <meta http-equiv="refresh" content="N; url=...">: the absolute URL, or NULL */
const char *web_refresh_url(web_doc *d, int *delay_s);

/* resolve a (possibly relative) URL against base into out; false if it cannot be made absolute */
bool web_resolve_url(const char *base, const char *rel, char *out, size_t n);
/* Fresh malloc output: 1 resolved, 0 invalid/representation overflow, -1 OOM.
   No legacy fixed-buffer URL quota; caller frees successful output. */
int web_resolve_url_owned(const char *base, const char *rel, char **out);

/* ---- resources ---- */
const char *web_pending_stylesheet(web_doc *d);                   /* next stylesheet to fetch, or NULL */
void web_stylesheet_loaded(web_doc *d, const char *css, size_t n); /* css NULL: it failed */
int web_image_count(web_doc *d);
const char *web_image_url(web_doc *d, int i);
bool web_image_wanted(web_doc *d, int i); /* not loaded or failed yet */
void web_image_loaded(web_doc *d, int i, const void *data, size_t n); /* data NULL: it failed */

/* ---- layout and painting ---- */
/* lay the page out for a viewport; returns the document height */
int web_layout(web_doc *d, int width, int height);
int web_doc_height(web_doc *d);
/* Laid-out root scrolling area, including propagated horizontal overflow. */
int web_doc_width(web_doc *d);
/* Native viewport position only: no layout, event dispatch or painting. */
void web_viewport_position(web_doc *d, int x, int y);
/* paint the document rectangle at (doc_x, doc_y) into the canvas rectangle (x, y, w, h) */
void web_paint(web_doc *d, canvas_t *c, int x, int y, int w, int h, int doc_x, int doc_y);
/* Debug-only native video geometry/paint transitions; no page state changes. */
void web_paint_debug(bool enabled);

/* ---- interaction ---- */
enum { WEB_HIT_NONE, WEB_HIT_LINK, WEB_HIT_TEXT_INPUT, WEB_HIT_CHECKBOX, WEB_HIT_RADIO, WEB_HIT_SUBMIT,
       WEB_HIT_BUTTON, WEB_HIT_SELECT, WEB_HIT_TEXTAREA, WEB_HIT_DETAILS, WEB_HIT_FILE };
struct web_hit {
    int kind;
    const char *href; /* absolute URL of the link under the point, or NULL */
    web_node *node;   /* the form control or link element */
};
bool web_hit_test(web_doc *d, int x, int y, struct web_hit *hit);
int web_anchor_y(web_doc *d, const char *fragment); /* y of the element with that id/name, or -1 */

/* form controls */
void web_focus(web_doc *d, web_node *n); /* NULL: nothing focused */
web_node *web_focused(web_doc *d);
bool web_node_inert(web_doc *d, web_node *n); /* native author/modal flat-tree inertness */
bool web_modal_active(web_doc *d);
const char *web_control_value(web_node *control);
/* a key for the focused text control: 0 ignored, 1 changed (repaint), 2 Enter: submit its form */
int web_key(web_doc *d, const struct gui_event *e);
/* Native user edits only, not a script clipboard grant. 0 unsupported,
   1 consumed/cancelled, 3 changed. data is a bounded UTF-8 byte string. */
int web_control_edit(web_doc *top, web_node *target, const char *input_type,
                     const char *data, size_t length);
/* Caller owns the selected UTF-8 text. Password/default-copy is denied. */
char *web_control_selected_text(web_doc *top, web_node *target, size_t *length);
struct web_control_activation { web_node *control, *previous; bool checked, indeterminate, radio; };
void web_control_activation_begin(web_doc *d, web_node *control, struct web_control_activation *activation);
bool web_control_activation_end(web_doc *d, struct web_control_activation *activation, bool allowed);
void web_toggle(web_doc *d, web_node *n); /* checkbox or radio click */
web_node *web_disclosure_focus(web_node *details); /* first summary, or the native default legend host */
bool web_reset(web_doc *d, web_node *control); /* reset button default action, with cancellable event */
/* the request a form submission makes: *url is malloc'd; *body is malloc'd for POST, NULL for GET.
   submitter is the clicked button, or any control of the form. */
struct web_form_request { char *url, *body, *content_type, *target; size_t body_len; };
/* Final method=dialog branch: 0 ordinary method, 1 actually closed,
   -1 dialog method without closure (no target, image coordinates, or no JS state).
   Call after validation/submit cancellation; never navigate on a nonzero result. */
int web_js_dialog_submit(web_doc *d, web_node *submitter);
bool web_submit_request(web_doc *d, web_node *submitter, struct web_form_request *request);
void web_submit_request_free(struct web_form_request *request);
bool web_submit(web_doc *d, web_node *submitter, char **url, char **body);
bool web_input_is_file(web_node *input);
bool web_input_choose_files(web_doc *d, web_node *input);
web_node *web_form_owner(web_doc *d, web_node *control);
bool web_control_disabled(const web_node *control);
bool web_form_submission_validate(web_doc *d, web_node *submitter);
bool web_take_validation_report(web_doc *d, web_node **control, const char **message, size_t *length);
int web_select_options(web_doc *d, web_node *sel, const char **labels, int max, int *selected);
void web_select_set(web_doc *d, web_node *sel, int index);
int web_datalist_options(web_doc *d, web_node *input, const char **labels, int capacity);
bool web_datalist_choose(web_doc *d, web_node *input, int index);
/* Native opt-in search completion; never exposed as a script history API. */
int web_autocomplete_options(web_doc *d, web_node *input, const char **labels, int capacity);
bool web_autocomplete_choose(web_doc *d, web_node *input, int index);
void web_autocomplete_record(web_doc *d, web_node *submitter, const char *destination);
void web_autocomplete_settings(void);
web_node *web_label_activation(web_node *target);
/* document rectangle of an element (its first box), for placing popups */
bool web_node_rect(web_doc *d, web_node *n, int *x, int *y, int *w, int *h);

/* find in page: the next match at or after (doc) y = from_y (wrapping around); highlights it and
   returns its y, -1 when not found, or -2 on query allocation failure (the
   previous query/highlight is retained). An empty string clears the highlight. */
int web_find(web_doc *d, const char *text, int from_y);
