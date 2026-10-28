/* Nocturne's web engine: HTML, CSS, layout, painting and an optional QuickJS host.

   The embedder does the networking. A typical page load:
     web_doc *d = web_parse(html, len, final_url, charset);
     while ((u = web_pending_stylesheet(d))) web_stylesheet_loaded(d, css_or_NULL, len);
     web_layout(d, width, height);              -> paint it
     for each i < web_image_count(d) with web_image_wanted(d, i):
         web_image_loaded(d, i, bytes_or_NULL, n, origin_clean); web_layout(...) again now and then
   Coordinates passed to and returned by these functions are document coordinates: (0, 0) is the
   top-left of the page, before scrolling. */
#pragma once
#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>
#include "gfx.h"
#include "abi.h"
#include "nocturne.h"
#include "webstorage.h"
#include "http.h"

typedef struct web_doc web_doc;
typedef struct node web_node; /* an element of the document */
/* The active native embedding chain, never author-selected Window ancestry. */
web_doc *web_presentation_root(web_doc *);
bool web_presentation_exit(web_doc *,int kind);
web_node *web_presentation_element(web_doc *,int kind);
bool web_avmedia_pip_paint(web_doc *,web_node *,canvas_t *);
uint64_t web_avmedia_pip_revision(web_doc *,web_node *);
void web_presentation_resize(web_doc *,int width,int height);
/* Direct scanout (win_direct) of a fullscreen WebGL canvas. Its frames go to the display by themselves
   while it is d's fullscreen element (through the frame chain) with no dialog over it, its box paints
   nothing of its own (background, border, outline, shadow, radius, opacity, filter, mask), its picture
   fitted by object-fit lies within its box with black around it, and its context draws on the GPU with
   color that needs no compositing over black. NULL when d is not like that now; the handle holds the
   context and the display's images for a screen of screen_width x screen_height. */
typedef struct web_direct web_direct;
web_direct *web_direct_open(web_doc *d, int screen_width, int screen_height);
/* The canvas's newest frame to the display (screen_width x screen_height: the fullscreen window's size)
   while it shows the window directly (shown: EV_DIRECT key 1 last): 1 shown; 0 nothing new; -1 not
   shown (paint it through the window; always when !shown); -2 d no longer shows this context directly.
   The window's frames (web_rendering_now) pace the animation frames in both cases. */
int web_direct_present(web_direct *v, web_doc *d, int screen_width, int screen_height, bool shown);
/* Gives the display's images back. Only once the display no longer shows them (EV_DIRECT key 0). */
void web_direct_close(web_direct *v);
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
       WEB_RESOURCE_IMAGE, WEB_RESOURCE_FETCH, WEB_RESOURCE_FRAME,
       WEB_RESOURCE_REPORT }; /* Native CSP POST only; never author Fetch. */
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
    bool no_cors, no_referrer; /* Private Fetch policy, never author headers. */
    const char *referrer; /* Private source URL; transport strips credentials/fragment. */
    int referrer_policy;
    bool image_upgrade; /* Native ordinary-image destination, never imageset/Fetch/script. */
    bool keepalive; /* Buffered Fetch may outlive this document, not the browser process. */
    uint64_t fetch_group; /* Native per-environment identity; not supplied by page headers. */
    int cache_mode; /* Fetch: 0 default, 1 no-store, 2 reload, 3 no-cache, 4 force-cache, 5 only-if-cached. */
    bool stream_response;
    bool require_corp; /* initiator COEP require-corp */
    int destination; /* Fetch request destination, enum webnet_destination (0 is "") */
};
/* One navigation of the top-level document (HTML "navigate"), with its source
   snapshot: the document that asked for it and whether that document's window
   had transient activation. Strings are borrowed until the callback returns. */
enum { WEB_NAVIGATE_PUSH, WEB_NAVIGATE_RELOAD, WEB_NAVIGATE_REPLACE };
struct web_navigation {
    const char *url;
    int history;             /* WEB_NAVIGATE_* */
    const void *body;        /* the POST body, or NULL for GET */
    size_t body_length;
    const char *content_type; /* with a body */
    const char *target;      /* NULL, "" or "_self"/"_top"/"_parent" for this window; "_blank" for a new one */
    const char *initiator;   /* the source document's URL: the request's origin */
    const char *referrer;    /* referrer source URL; NULL or "" sends no Referer (noreferrer) */
    int referrer_policy;     /* enum webnet_referrer_policy */
    bool user_activation;    /* the source window had transient activation */
};
/* HTML's source snapshot for a navigation the document itself asks for (a
   hyperlink, a form, location, a meta refresh): the initiator and referrer
   of its fetch client, the referrer policy, and its window's transient
   activation. A hyperlink or form element (or NULL) contributes its
   referrerpolicy attribute and rel=noreferrer. Fills only those fields; the
   strings stay valid while the document lives. */
void web_navigation_source(web_doc *, web_node *element, struct web_navigation *);
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
    int event; /* webnet_event; zero retains the complete-response contract. */
    size_t uploaded;
    /* Fetch's timing record of the request (http.h), its Server-Timing values
       owned (web_response_free); valid only on a complete or ended response. */
    struct http_fetch_timing timing;
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
/* How a document was navigated to, for its PerformanceNavigationTiming and
   performance.navigation: navigation start (the time origin), the kind of
   navigation, and the navigation response's timing record. The document
   copies it as it is created; fetch.server_timing and headers are borrowed
   until then. All values use uptime_ms(), not wall clock time. */
enum web_navigation_type { WEB_NAVIGATION_NAVIGATE, WEB_NAVIGATION_RELOAD, WEB_NAVIGATION_BACK_FORWARD };
struct web_navigation_timing {
    bool valid;
    uint8_t type; /* enum web_navigation_type */
    uint64_t navigation_start_ms;
    struct http_fetch_timing fetch;
    /* The navigation response's status and header block (its Content-Type). */
    int status;
    const char *headers;
};
/* Optional native-host cancellation deadline. Normal pages are not rejected
   at an arbitrary 5/15/30-second cutoff. Page code cannot reset it. */
#define WEB_JS_TASK_DEFAULT_MS UINT32_MAX
#define WEB_JS_TASK_MIN_MS 1u
#define WEB_JS_TASK_MAX_MS UINT32_MAX
#define WEB_WINDOW_VISIBLE 1u
#define WEB_WINDOW_FOCUSED 2u
/* A bounded, native video-layer update from the last published document scene.
   The host validates identity/viewport before touching its canvas, then has
   draw put the patch's w x h pixels at dst (rows pitch pixels apart), straight
   into the surface it shows. */
struct web_video_patch {
    canvas_t canvas;
    int scroll_x, scroll_y;
    int x, y, w, h;
    void (*draw)(void *context, uint32_t *dst, int pitch);
    void *context;
};
/* What a console line is: the page's log, warn and error, or the engine's own
   measurement (profiles, network and media traces). A host shows traces apart
   from the page's console, so that they never bury the page's messages. */
enum { WEB_CONSOLE_LOG, WEB_CONSOLE_WARN, WEB_CONSOLE_ERROR, WEB_CONSOLE_TRACE };
/* One input step of a test automation run (web_host.automation): at is ms after the run
   starts; x, y a point in the top-level viewport (CSS px). DOWN and UP press and release
   WebDriver button value (0 left, 1 middle, 2 right, 3 back, 4 forward) at x, y; KEY_DOWN and
   KEY_UP the key whose WebDriver code point is value (a character, or U+E000..U+F8FF for the
   named keys); WHEEL scrolls dx, dy CSS px at x, y. */
enum web_automation_kind { WEB_AUTOMATION_MOVE, WEB_AUTOMATION_DOWN, WEB_AUTOMATION_UP,
    WEB_AUTOMATION_KEY_DOWN, WEB_AUTOMATION_KEY_UP, WEB_AUTOMATION_WHEEL };
struct web_automation_step {
    enum web_automation_kind kind;
    uint32_t at, value;
    double x, y, dx, dy;
};
struct web_host {
    void *opaque;
    bool (*request)(void *opaque, const struct web_request *request);
    void (*cancel)(void *opaque, uint64_t id);
    /* Document retirement, unlike explicit AbortSignal: detach callback delivery
       and retain a keepalive transport. A host without this cannot accept it. */
    void (*release_request)(void *opaque, uint64_t id);
    void (*resume_request)(void *opaque, uint64_t id);
    bool (*sync_load)(void *opaque, const char *url, int kind, struct web_response *response);
    /* Origin-aware module transport. Prefer this over the legacy top-document
       callback; origin/credentials are native initiating-realm values. Same
       native-only wait/response ownership contract as sync_load applies. */
    bool (*sync_request)(void *opaque, const struct web_request *request, struct web_response *response);
    /* Join an already submitted module request without another HTTP fetch.
       completed is borrowed from a pinned native pending record. This callback
       may pump native network/UI only: no author JS, document replacement or
       pending-record destruction. Return false on navigation/cancellation. */
    bool (*wait_request)(void *opaque, uint64_t id, const bool *completed);
    /* Form bytes are not NUL-terminated. The host copies what it keeps
       before returning to the JS task. */
    void (*navigate)(void *opaque, const struct web_navigation *navigation);
    /* level is a WEB_CONSOLE_* value. */
    void (*console)(void *opaque, int level, const char *message);
    void (*scroll)(void *opaque, int *x, int *y);
    void (*scroll_to)(void *opaque, int x, int y);
    /* WEB_WINDOW_VISIBLE: the system visibility state of the top-level traversable this host
       shows (HTML 6.2): whether its page is on screen at all, so false for a minimized or
       hidden window and for a tab that is not the front one. WEB_WINDOW_FOCUSED: system focus.
       Optional; without it the documents count as visible and without focus. */
    unsigned (*window_state)(void *opaque);
    /* Browser window size including its own toolbar; optional. */
    void (*window_size)(void *opaque, int *width, int *height);
    /* Native-only getter: a queued navigation/close cancels stale default edits. */
    bool (*navigation_pending)(void *opaque);
    bool (*video_present)(void *opaque, web_doc *document,
                          const struct web_video_patch *patch);
    /* Native window transition; no DOM/JS callback, no borrowed media pixels.
       0 exit fullscreen, 1 enter fullscreen, 2 enter PiP, 3 exit PiP,
       4 retire fullscreen: release document pointers even if restore fails. */
    bool (*presentation)(void *opaque,web_doc *document,web_node *element,int mode);
    bool (*history)(void *opaque, int operation, const char *url, const void *state,
                    size_t state_len, int value, struct web_history *out);
    /* The host owns the cookie jar; returned text is malloc'd, never HttpOnly. */
    char *(*cookie_get)(void *opaque, const char *url);
    void (*cookie_set)(void *opaque, const char *url, const char *value);
    /* Whether the document at url (its effective URL, a frame's own) may use cookies: there is a
       jar and the user did not block the site. NULL means unavailable/unknown. */
    bool (*cookie_enabled)(void *opaque, const char *url);
    /* origin is derived from the native document URL by the private URL parser,
       never a page-selected directory. Output text transfers malloc ownership. */
    int (*storage)(void *opaque, const char *origin, const struct web_storage_request *request,
                   struct web_storage_result *out);
    bool storage_events; /* backend supports document subscriptions and ACK */
    /* Opt in only for the native browser embedder. Custom/fixture hosts keep
       their request() transport; this flag does not grant origin/CORS access. */
    bool media_range;
    /* Zero-initialized custom hosts keep the explicit document-init epoch. */
    struct web_navigation_timing navigation_timing;
    /* Native embedder-selected deadline; 0 keeps the uint32 representation
       maximum. Page code cannot reset it. */
    uint32_t js_task_budget_ms;
    /* The embedder's display gives the rendering opportunities (web_rendering_now). Without it
       animation frames and rendering observers run on the engine's own clock, 16 ms apart. */
    bool display_frames;
    /* Optional native-only cancellation checkpoint. NEVER dispatch JS/DOM,
       layout, navigation or free a document here. Document paint/scroll is
       allowed ONLY after web_native_input_ready confirms a committed snapshot;
       a C style/layout/parser checkpoint never grants that permission.
       Chrome-only console painting is safe. False stops native style/layout as well as JS
       task after a user close/stop request, not after a page-count quota. */
    bool (*script_checkpoint)(void *opaque);
    bool debug_js; /* native profiling is off unless the host explicitly opts in */
    /* A network panel in the console: each request's method, kind, body size
       and URL, its status and type, and its completed size and time. URLs keep
       their query, so only a host that keeps its log local opts in. */
    bool net_log;
    /* With net_log, a request whose URL contains this text also logs its whole
       request body and the first 2 KiB of its response body, in hex. NULL or
       empty logs no bodies. */
    const char *net_body;
    /* WebSocket transport (RFC 6455); all optional, NULL means unavailable.
       url is ws:/wss:, protocols a comma-separated offer or NULL, origin the
       serialized initiating-realm origin. Events arrive via web_socket_event
       for the returned nonzero id until socket_release. */
    uint64_t (*socket_open)(void *opaque, const char *url, const char *protocols, const char *origin);
    bool (*socket_send)(void *opaque, uint64_t id, bool binary, const void *data, size_t length);
    bool (*socket_close)(void *opaque, uint64_t id, unsigned code, const char *reason, size_t length);
    size_t (*socket_buffered)(void *opaque, uint64_t id);
    void (*socket_release)(void *opaque, uint64_t id);
    /* A test tool, not a product feature: WPT's testdriver drives the host's own input with
       it. A host sets it only when its user started it for automation; then, and only then,
       a top-level document's realm has the function that calls it (see web_automation_step).
       The host takes the steps (copied before return) for document, puts them into its real
       input at their times and calls web_automation_done with id once it has handled the last
       one and duration has passed. Returns NULL when it took them, else why not (static). */
    const char *(*automation)(void *opaque, web_doc *document, uint64_t id,
                              const struct web_automation_step *steps, size_t count, uint32_t duration);
};
enum web_socket_event_type { WEB_SOCKET_OPENED, WEB_SOCKET_MESSAGE_TEXT, WEB_SOCKET_MESSAGE_BINARY,
    WEB_SOCKET_CLOSED, WEB_SOCKET_FAILED };
struct web_socket_event {
    enum web_socket_event_type type;
    unsigned code;          /* close code, 1005 when none was received */
    const void *data;       /* message payload, close reason or error text; borrowed */
    size_t length;
    const char *protocol;   /* OPENED: negotiated subprotocol, borrowed, may be NULL */
};
/* Host to document: deliver one event of a socket opened by this document or
   one of its frames. Data is copied before return. */
void web_socket_event(web_doc *d, uint64_t id, const struct web_socket_event *event);
/* Host to document: the automation run id of d ended, NULL when every step was handled, else
   why it failed (copied). Its promise settles in a task of d's (web_tick). Runs no script. */
void web_automation_done(web_doc *d, uint64_t id, const char *error);
bool web_set_url(web_doc *d, const char *url);
/* Call between JS tasks, after the host history position and document URL change. */
void web_history_event(web_doc *d, const char *old_url, bool popstate);
/* The host's window_state for d's top-level document may have changed its WEB_WINDOW_VISIBLE
   bit: the tab became the front one or stopped being it, or the window was shown, minimized or
   hidden. If the system visibility state did change, d's top document and each document in its
   frame tree get a queued task (run by web_tick) that sets its visibilityState and fires
   visibilitychange; web_deadline is then due at once. A call that finds no change does nothing,
   so the host may call it for every tab whose state might have moved. It runs no script. */
void web_visibility_event(web_doc *d);
/* Optional, once at startup: compiles the platform's script bindings on another
   CPU while the host opens its window and fetches the first page, so that the
   first document with script does not compile them on its own thread. */
void web_js_prepare(void);
web_doc *web_live(const char *html, size_t len, const char *url, const char *charset,
                  const struct web_host *host);
/* HTML "load a document": the document a response of a Content-Type value
   (NULL or empty: none) makes. An HTML MIME type an HTML document, an XML
   MIME type (text/xml, application/xml, a subtype ending in "+xml") an XML
   document; any other the host displays its own way. */
enum web_document_kind { WEB_DOCUMENT_OTHER, WEB_DOCUMENT_HTML, WEB_DOCUMENT_XML };
enum web_document_kind web_document_kind(const char *type);
/* A browsing context's document from a response: html is its body, headers
   its header block, type the Content-Type of html (NULL: text/html, as for
   markup the host writes itself). An XML type loads an XML document. */
web_doc *web_live_response(const char *html,size_t len,const char *url,const char *charset,
                         const char *headers,const char *type,const struct web_host *host);
void web_tick(web_doc *d, uint64_t now_ms);
/* Private media/JS-worker children outlive a retired document until nonblocking reap.
 * No document/node callback is retained by this process-lifetime cleanup. */
void web_media_background(uint64_t now_ms);
int64_t web_media_background_deadline(uint64_t now_ms);
/* Native playback transitions are reported only when explicitly requested. */
void web_avmedia_debug(bool enabled);
/* -1: no deadline, otherwise an absolute uptime_ms() deadline. */
int64_t web_deadline(web_doc *d);
/* With host.display_frames, the rendering opportunities (HTML "update the rendering": animation
   frames, then the rendering observers) of d and its frames come from the embedder's display:
   while web_rendering_wanted, the embedder asks its window for the display's next frame
   (ui_window_frame) and calls web_rendering_now when it comes (UI_EVENT_FRAME), with the display's
   frame period: idle callbacks end before the frame after it. */
bool web_rendering_wanted(web_doc *d);
void web_rendering_now(web_doc *d, double period_ms);
/* The event queue the browser's main loop waits on. Atomics.waitAsync waiters
 * and the watch on the browser's preferences (webprefs.h: the languages, for
 * languagechange and Accept-Language, and the search history) report into it;
 * the loop adds its window and transports with its own tokens and hands every
 * report it receives to web_wait_events. */
n_handle web_wait_queue(void);
void web_wait_events(const struct n_event *events, size_t count);
/* Debug summary of the realm waits behind web_wait_queue; 0 when none. */
int web_wait_report(char *out, size_t size);
/* Debug summary of web_paint time since the previous report; 0 when none. */
int web_paint_report(char *out, size_t size);
/* Debug summary of style cascades since the previous report; 0 when none. */
int web_style_report(char *out, size_t size);
/* Debug summary of box publications (local and whole) since the previous report; 0 when none. */
int web_box_report(char *out, size_t size);
/* Debug summary of which step of the document's turns asked for layout since the previous report. */
int web_tick_report(char *out, size_t size);
/* Debug summary of which source set each document's script deadline since the previous report. */
int web_js_deadline_report(char *out, size_t size);
/* Debug summary of what the document and its frames hold now, by owner. */
int web_memory_report(web_doc *d, char *out, size_t size);
void web_resource_loaded(web_doc *d, uint64_t id, const struct web_response *response);
bool web_dirty(web_doc *d); /* consumes the legacy layout/paint notification */
/* Whether the pending notification is only computed values changing (CSS animation samples):
   after web_layout, web_take_paint_damage then bounds what they repaint. Does not consume it. */
bool web_restyle_only(const web_doc *d);
bool web_paint_dirty(web_doc *d); /* consumes a paint-only media notification */
bool web_render_blocked(web_doc *d); /* initial head CSS only, not all resource loading */
bool web_script_running(web_doc *d);
/* Native scroll never invokes a listener or flushes DOM/layout. A deferred
   notification keeps the original target and document epoch, not a raw node
   borrowed from a possibly retired frame. */
struct web_input_target { web_node *node; uint64_t document, stream; };
/* prepared_wait is host permission only: author/native DOM is suspended at a
   synchronous resource wait AFTER explicit native snapshot preparation. It
   never permits painting from a native C checkpoint or an active layout. */
bool web_native_input_ready(web_doc *d,bool prepared_wait);
bool web_input_capture(web_doc *d,web_node *target,struct web_input_target *out);
bool web_native_wheel_target(web_doc *d,web_node *target,struct web_input_target *out,bool prepared_wait);
bool web_input_target_valid(web_doc *d,const struct web_input_target *target);
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
    bool keyboard_activation; /* trusted keyboard activation has no pointer device */
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
/* Uncaptured native primary mouse only. Resolve after mouseup handlers, using
   the current DOM; detached nodes and inactive embedding chains cannot click. */
web_node *web_pointer_click_target(web_doc *d, web_node *down, web_node *up);
/* Real native mouse capture, including active child documents. The returned
   target is borrowed, live and connected; page properties cannot spoof it. */
web_node *web_pointer_capture_target(web_doc *d, const struct web_event *event);
web_node *web_pointer_capture_click_target(web_doc *d, web_node *released_target);
void web_pointer_cancel(web_doc *d);
bool web_node_action(web_doc *d, web_node *target, struct web_hit *hit);
/* Capture before click dispatch, then resolve only that anchor after dispatch.
   Detached HTML anchors can navigate; their owner must still be the active d. */
web_node *web_link_activation_anchor(web_doc *d, web_node *target);
bool web_link_action(web_doc *d, web_node *anchor, struct web_hit *hit);
/* HTML "follow the hyperlink" for anchor in top's tree, from a user's click or the page's
   click(): its target (or the base element's) chooses the navigable. True when that was not top:
   a child frame, another top-level traversable (_blank, or a name no frame here has: the host's
   navigate with that target) or nothing; false leaves top's own navigation to the caller.
   activated: the source has the user's activation (a click the user made is one).
   Script location changes use their own, stricter child path. */
bool web_frame_navigate(web_doc *top, web_node *anchor, const char *url, bool activated);
/* Actual active child URL for native chrome only. The parent URL is unchanged.
   Selection validates the complete live embedding chain; no author callbacks. */
const char *web_frame_address(web_doc *top);
void web_frame_address_select(web_doc *top, web_node *node);
void web_free(web_doc *d);
const char *web_title(web_doc *d); /* "" if none */
const char *web_url(web_doc *d);
/* <meta http-equiv="refresh" content="N; url=...">: the absolute URL, or NULL */
const char *web_refresh_url(web_doc *d, int *delay_s);

/* web_resolve_url_owned, the URL parser for a URL against a base, is weburl.h's. */

/* ---- resources ---- */
const char *web_pending_stylesheet(web_doc *d);                   /* next stylesheet to fetch, or NULL */
void web_stylesheet_loaded(web_doc *d, const char *css, size_t n); /* css NULL: it failed */
int web_image_count(web_doc *d);
const char *web_image_url(web_doc *d, int i);
bool web_image_wanted(web_doc *d, int i); /* not loaded or failed yet */
/* data NULL: it failed. origin_clean: the response was CORS-same-origin (Fetch's response
   tainting was not opaque), so a canvas or WebGL may read its pixels. */
void web_image_loaded(web_doc *d, int i, const void *data, size_t n, bool origin_clean);

/* ---- layout and painting ---- */
/* lay the page out for a viewport; returns the document height */
int web_layout(web_doc *d, int width, int height);
int web_doc_height(web_doc *d);
/* Laid-out root scrolling area, including propagated horizontal overflow. */
int web_doc_width(web_doc *d);
/* Native viewport position only: no layout, event dispatch or painting. */
void web_viewport_position(web_doc *d, int x, int y);
/* Consume native input in committed nested scrollports, without layout or JS.
 * Remaining integer deltas belong to the caller's root viewport. edge is
 * -1 (Home), 0 (delta), +1 (End); a vertical inner edge consumes the command. */
bool web_scroll_input(web_doc *d, web_node *target, int *dx, int *dy, int edge);
/* paint the document rectangle at (doc_x, doc_y) into the canvas rectangle (x, y, w, h) */
void web_paint(web_doc *d, canvas_t *c, int x, int y, int w, int h, int doc_x, int doc_y);
/* Consume known paint-only damage in viewport coordinates. False means full
 * repaint; true with empty dimensions means damage is outside the viewport. */
bool web_take_paint_damage(web_doc *d,int *x,int *y,int *w,int *h);
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
/* After an uncanceled native primary mousedown, resolve the click-focusable
   area without changing the event target. false preserves focus (inactive,
   detached, disabled or inert target); true with NULL selects the viewport. */
bool web_pointer_focus_target(web_doc *d, web_node *target, web_node **area);
bool web_node_inert(web_doc *d, web_node *n); /* native author/modal flat-tree inertness */
bool web_modal_active(web_doc *d);
const char *web_control_value(web_node *control);
/* a key for the focused text control: 0 ignored, 1 consumed (may request repaint), 2 Enter: submit its form */
int web_key(web_doc *d, const struct gui_event *e);
/* Native user edits only, not a script clipboard grant. 0 unsupported,
   1 consumed/cancelled, 3 changed. data is a bounded UTF-8 byte string. */
int web_control_edit(web_doc *top, web_node *target, const char *input_type,
                     const char *data, size_t length);
/* Caller owns the selected UTF-8 text. Password/default-copy is denied. */
char *web_control_selected_text(web_doc *top, web_node *target, size_t *length);
/* Whether the keys go to an editing host: the focused document (through its
   frames) is in design mode, or its focus is in an element with contenteditable.
   web_key then takes the typing, deleting, Enter and caret keys. */
bool web_editing_target(web_doc *top);
/* The user's clipboard edit of an editing host: "insertFromPaste" with the text,
   or "deleteByCut". 1 when an editing host took it. */
int web_editing_edit(web_doc *top, const char *input_type, const char *data, size_t length);
/* Caller owns the UTF-8 text of the focused document's selection; NULL when empty. */
char *web_selection_text(web_doc *top, size_t *length);
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
/* Native form activation: validates the actual active owner/family and its immutable sandbox. */
bool web_sandbox_form_submission_allowed(web_doc *top, web_node *submitter);
bool web_submit_request(web_doc *d, web_node *submitter, struct web_form_request *request);
/* HTML "submit" after its submit event: a dialog method closes the dialog,
   any other method navigates with the form's source snapshot. */
void web_form_navigate(web_doc *d, web_node *submitter);
void web_submit_request_free(struct web_form_request *request);
bool web_submit(web_doc *d, web_node *submitter, char **url, char **body);
bool web_input_is_file(web_node *input);
bool web_input_choose_files(web_doc *d, web_node *input);
web_node *web_form_owner(web_doc *d, web_node *control);
web_node *web_form_implicit_submission(web_doc *d, web_node *field);
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
/* Turns the search history off (webprefs.h), then removes every stored search; false when either
   failed. Whether it is on is the browser's preference, changed in its Settings window. */
bool web_autocomplete_clear(void);
web_node *web_label_activation(web_node *target);
/* document rectangle of an element (its first box), for placing popups */
bool web_node_rect(web_doc *d, web_node *n, int *x, int *y, int *w, int *h);

/* find in page: the next match at or after (doc) y = from_y (wrapping around); highlights it and
   returns its y, -1 when not found, or -2 on query allocation failure (the
   previous query/highlight is retained). An empty string clears the highlight. */
int web_find(web_doc *d, const char *text, int from_y);
