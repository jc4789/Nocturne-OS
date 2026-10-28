/* Nocturne browser requests. A browser-owned isolated webfetch service
   shares its connection pool/cache across request-specific native channels. */
#pragma once
#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include "http.h"
#include "nocturne.h"

/* Body lengths are uint32_t on the native worker wire; storage grows lazily. */
#define WEBNET_BODY_LIMIT ((size_t)UINT32_MAX)
#define WEBNET_SCRIPT_BODY_LIMIT WEBNET_BODY_LIMIT
/* No short browser resource quota. The user-requested 32-bit duration is
   accumulated in a uint64_t wire deadline; signed OS waits clamp per call. */
#define WEBNET_TIMEOUT_MS UINT32_MAX
#define WEBNET_URL_MAX HTTP_URL_MAX /* legacy fixed caller/wire bound; owned transport opts out */
#define WEBNET_REQUEST_HEADERS_MAX 8192
#define WEBNET_HEADERS_MAX WEBNET_REQUEST_HEADERS_MAX /* legacy request-sized alias */
/* Response storage includes NUL plus room for negotiated status/metadata.
   Independent of the unchanged 8 KiB author request-header bound. */
#define WEBNET_RESPONSE_HEADERS_MAX (64u * 1024u + 512u + 1u)
#define WEBNET_METHOD_MAX ((size_t)UINT32_MAX) /* length bytes on the native wire, excluding NUL */

/* HTTP token validation belongs at the native boundary as well as in Web IDL.
   No method spelling can inject bytes into the request line. */
static inline bool webnet_method_valid(const char *s) {
    if (!s || !*s) return false;
    size_t n = 0;
    for (; s[n]; n++) {
        unsigned char c = (unsigned char)s[n];
        if (n == WEBNET_METHOD_MAX) return false;
        if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9')) continue;
        switch (c) {
        case '!': case '#': case '$': case '%': case '&': case '\'': case '*': case '+':
        case '-': case '.': case '^': case '_': case '`': case '|': case '~': continue;
        default: return false;
        }
    }
    const char *forbidden[] = {"CONNECT", "TRACE", "TRACK"};
    for (unsigned i = 0; i < 3; i++) {
        unsigned j = 0;
        while (s[j] && forbidden[i][j] && ((unsigned char)s[j] & ~32u) == (unsigned char)forbidden[i][j]) j++;
        if (!s[j] && !forbidden[i][j]) return false;
    }
    return true;
}

typedef struct webnet webnet;
enum webnet_kind { WEBNET_NAVIGATION, WEBNET_CLASSIC, WEBNET_MODULE, WEBNET_RESOURCE, WEBNET_FETCH,
                   WEBNET_REPORT, /* Append-only native CSP transport. */
                   WEBNET_WEBSOCKET }; /* Only through webnet_socket_open. */
/* A separate destination must not become a generic CORS bypass. Both the
   submitting host and isolated worker enforce this exact generated field. */
#define WEBNET_CSP_REPORT_HEADERS "Content-Type: application/csp-report\r\n"
static inline bool webnet_report_fields_valid(const char *method,const char *headers) {
    return method && headers && !strcmp(method,"POST") && !strcmp(headers,WEBNET_CSP_REPORT_HEADERS);
}
/* Response-only policy. Author request/POST bounds remain WEBNET_BODY_LIMIT. */
static inline size_t webnet_response_limit(enum webnet_kind kind) {
    return kind == WEBNET_CLASSIC || kind == WEBNET_MODULE ? WEBNET_SCRIPT_BODY_LIMIT : WEBNET_BODY_LIMIT;
}
/* Fetch's request destination, in the standard's order; EMPTY is the empty
   string (fetch(), XHR, EventSource, beacons). Trusted requester metadata,
   never an author header: it selects the default Accept value and is sent as
   Sec-Fetch-Dest (Fetch Metadata). */
enum webnet_destination { WEBNET_DEST_EMPTY, WEBNET_DEST_AUDIO, WEBNET_DEST_AUDIOWORKLET, WEBNET_DEST_DOCUMENT,
    WEBNET_DEST_EMBED, WEBNET_DEST_FONT, WEBNET_DEST_FRAME, WEBNET_DEST_IFRAME, WEBNET_DEST_IMAGE,
    WEBNET_DEST_JSON, WEBNET_DEST_MANIFEST, WEBNET_DEST_OBJECT, WEBNET_DEST_PAINTWORKLET, WEBNET_DEST_REPORT,
    WEBNET_DEST_SCRIPT, WEBNET_DEST_SERVICEWORKER, WEBNET_DEST_SHAREDWORKER, WEBNET_DEST_STYLE,
    WEBNET_DEST_TEXT, WEBNET_DEST_TRACK, WEBNET_DEST_VIDEO, WEBNET_DEST_WEBIDENTITY, WEBNET_DEST_WORKER, WEBNET_DEST_XSLT,
    WEBNET_DEST_COUNT };
static inline const char *webnet_destination_name(enum webnet_destination d) {
    static const char *const names[WEBNET_DEST_COUNT] = {"", "audio", "audioworklet", "document", "embed", "font",
        "frame", "iframe", "image", "json", "manifest", "object", "paintworklet", "report", "script",
        "serviceworker", "sharedworker", "style", "text", "track", "video", "webidentity", "worker", "xslt"};
    return (unsigned)d < WEBNET_DEST_COUNT ? names[d] : "";
}
enum webnet_credentials { WEBNET_CREDENTIALS_OMIT, WEBNET_CREDENTIALS_SAME_ORIGIN, WEBNET_CREDENTIALS_INCLUDE };
enum webnet_cache { WEBNET_CACHE_DEFAULT, WEBNET_CACHE_NO_STORE, WEBNET_CACHE_RELOAD,
                    WEBNET_CACHE_NO_CACHE, WEBNET_CACHE_FORCE_CACHE, WEBNET_CACHE_ONLY_IF_CACHED };
/* Private Fetch policy metadata, never an author Referer header. Zero resolves
   to the environment's default, strict-origin-when-cross-origin. */
enum webnet_referrer_policy { WEBNET_REFERRER_DEFAULT, WEBNET_REFERRER_NONE,
    WEBNET_REFERRER_DOWNGRADE, WEBNET_REFERRER_SAME_ORIGIN, WEBNET_REFERRER_ORIGIN,
    WEBNET_REFERRER_STRICT_ORIGIN, WEBNET_REFERRER_ORIGIN_CROSS,
    WEBNET_REFERRER_STRICT_ORIGIN_CROSS, WEBNET_REFERRER_UNSAFE };
static inline int webnet_referrer_policy_token(const char *value,size_t length) {
    static const char *const names[]={"","no-referrer","no-referrer-when-downgrade","same-origin","origin",
        "strict-origin","origin-when-cross-origin","strict-origin-when-cross-origin","unsafe-url"};
    while(length&&(value[0]==' '||value[0]=='\t'))value++,length--;
    while(length&&(value[length-1]==' '||value[length-1]=='\t'))length--;
    for(int i=1;i<=WEBNET_REFERRER_UNSAFE;i++)if(strlen(names[i])==length&&!memcmp(names[i],value,length))return i;
    return WEBNET_REFERRER_DEFAULT;
}
static inline int webnet_referrer_policy_header(const char *headers,int previous) {
    for(const char *line=headers?headers:"";*line;){
        const char *end=strchr(line,'\n');if(!end)end=line+strlen(line);
        const char *colon=memchr(line,':',(size_t)(end-line));
        if(colon&&colon-line==15){
            static const char field[]="referrer-policy";bool matches=true;
            for(int i=0;i<15;i++){unsigned char c=(unsigned char)line[i];if(c>='A'&&c<='Z')c+=32;matches&=c==(unsigned char)field[i];}
            if(matches){const char *p=colon+1,*last=end;if(last>p&&last[-1]=='\r')last--;
                while(p<last){const char *comma=memchr(p,',',(size_t)(last-p));const char *finish=comma?comma:last;
                    int policy=webnet_referrer_policy_token(p,(size_t)(finish-p));if(policy)previous=policy;p=comma?comma+1:last;}
            }
        }
        line=*end?end+1:end;
    }
    return previous;
}
struct webnet_request {
    enum webnet_kind kind;
    uint64_t generation;
    const char *url;
    const char *origin; /* full initiating document URL, not a page-controlled Origin header */
    const char *method; /* GET by default; validated HTTP token, no CONNECT/TRACE/TRACK. */
    const char *headers; /* CRLF-delimited fields; prohibited fields are rejected */
    const void *body;
    size_t body_len;
    bool user_navigation; /* only this permits a file: top-level request */
    enum webnet_credentials credentials;
    bool force_preflight; /* Fetch use-CORS-preflight flag, not an author request header. */
    bool redirect_error; /* Refuse a redirect before issuing its target request. */
    /* Fetch's redirect mode "manual" (WEBNET_FETCH only): the first redirect ends the fetch as an
       opaque-redirect filtered response (WEBNET_WIRE_REDIRECT_MANUAL). */
    bool redirect_manual;
    bool same_origin; /* Enforced on every redirect hop, not just the initial URL. */
    bool no_cors, no_referrer; /* Safelisted Fetch; opaque response after cross-origin hop. */
    const char *referrer; /* NULL legacy/no source; empty means explicitly no-referrer. */
    int referrer_policy; /* enum webnet_referrer_policy, copied into private transport. */
    bool keepalive; /* Fetch survives generation retirement; explicit cancel still aborts. */
    bool image_upgrade; /* Trusted ordinary non-CORS, non-imageset image only; RESOURCE. */
    uint64_t fetch_group; /* Distinct environment settings object, including child documents. */
    enum webnet_cache cache_mode;
    bool stream_response; /* Fetch lifecycle frames; other destinations remain buffered. */
    /* Trusted enclosing top document URL, separate from the initiating frame.
       NULL retains legacy initiator-derived partition/site policy; "" denotes
       an opaque top context. Never supplied through an author HTTP header. */
    const char *top_level_url;
    /* Initiator policy container COEP require-corp: a cross-origin no-cors
       response without Cross-Origin-Resource-Policy is same-origin only. */
    bool require_corp;
    enum webnet_destination destination;
    /* Navigation requests only. user_activation: HTML's request user-activation
       (a click or key press, or the browser's own UI), sent as Sec-Fetch-User.
       browser_ui: the user asked the browser itself (address bar, bookmark,
       home, history list), so there is no initiating site: Sec-Fetch-Site none. */
    bool user_activation, browser_ui;
    /* The browser's developer tools record this request: its response says
       which request fields the final hop actually sent (sent_headers). */
    bool report_sent;
};
enum webnet_event { WEBNET_COMPLETE, WEBNET_HEADERS, WEBNET_CHUNK, WEBNET_END, WEBNET_UPLOAD };
struct webnet_response {
    int status;
    const char *final_url;
    const char *headers; /* Complete filtered block; never a capacity-truncated prefix. */
    const void *body;
    size_t body_len;
    const char *error; /* empty for a received HTTP response, including HTTP errors */
    enum webnet_event event;
    size_t uploaded;
    /* With report_sent: the request fields the final hop sent, as sent
       ("Name: value" CRLF lines, Host first); otherwise empty. */
    const char *sent_headers;
    /* WEBNET_COMPLETE and WEBNET_END: Fetch's timing record of the request
       (http.h), network errors included; NULL with every other event. */
    const struct http_fetch_timing *timing;
};
typedef void (*webnet_callback)(webnet *, uint64_t id, uint64_t generation,
                               const struct webnet_response *, void *opaque);

webnet *webnet_create(void);
/* Trusted host diagnostics only; no author fields or response bytes are logged. */
void webnet_debug(webnet *, bool enabled);
void webnet_free(webnet *net);
/* Browser-owned session jar. Native host entry points for document.cookie.
   get returns bytes excluding NUL, out=NULL queries length; -1 on failure.
   set: 1 accepted/deleted, 0 ignored by cookie policy, -1 resource failure. */
long webnet_cookie_get(webnet *, const char *url, char *out, size_t cap);
int webnet_cookie_set(webnet *, const char *url, const char *value);
/* Browser host supplies its trusted top document URL. NULL retains the legacy
   same-site caller policy; an explicit empty URL denotes an opaque top site. */
long webnet_cookie_get_site(webnet *,const char *url,const char *top_level_url,char *out,size_t cap);
int webnet_cookie_set_site(webnet *,const char *url,const char *top_level_url,const char *value);
/* The jar is the window process's; its persistent cookies are kept in the document "cookies" of
   the browser's profile folder, which every window shares (webnet.c, cookie_sync): read at
   webnet_create and whenever another window saves it, saved a second after a change and at
   webnet_free. Session cookies and those of hosts the user's choices do not keep (webprefs.h)
   stay the process's own. The user's exceptions apply to every cookie set and sent. */
struct webcookie_site;
/* What the jar holds of each site (registrable domain), sorted: a malloc'd array (free it) in
   *out and its length, or -1 (no memory). Includes what other windows saved. */
long webnet_cookie_sites(webnet *, struct webcookie_site **out);
/* Removes the cookies of site and its subdomains (NULL or "": all), session ones too, and saves
   at once; other windows remove theirs when they read the store. The count removed, or the
   store's failure (negative n_result; the clear is saved at a later try). */
long webnet_cookie_clear(webnet *, const char *site);
/* For the test automation alone (WebDriver's Cookies, web_host.automation_cookies): the
   associated cookies of a document at url, HttpOnly ones too (webcookie_associated, name's alone
   unless NULL), with what other windows saved: a malloc'd block (free it) in *out and the count,
   or -1. delete_associated removes them (WebDriver's "delete cookies") and saves at once: the
   count removed, or -1. */
struct webcookie_record;
long webnet_cookie_associated(webnet *, const char *url, const char *name, struct webcookie_record **out);
long webnet_cookie_delete_associated(webnet *, const char *url, const char *name);
/* The site (registrable domain, by the Public Suffix List the jar uses) of a host, in out: the
   key the settings page groups a site's cookies and storage by. false when it does not fit. */
bool webnet_site(const webnet *, const char *host, char *out, size_t cap);
/* Copies all request data; 0 means invalid request, queue full, or allocation failure. */
uint64_t webnet_submit(webnet *, const struct webnet_request *, webnet_callback, void *opaque);
/* Why the last webnet_submit returned 0: static text for the page's console. */
const char *webnet_refusal(const webnet *);
/* Fetch's keepalive bound: a fetch group's inflight keepalive request bodies stay within it. */
#define WEBNET_KEEPALIVE_BYTES ((size_t)65536)
size_t webnet_keepalive_inflight(const webnet *, uint64_t fetch_group);
/* Dispatches completions only here. Response pointers remain valid until callback returns.
   Callbacks may submit/cancel requests, but must not free net or recursively pump it. */
void webnet_pump(webnet *, uint64_t now_ms);
/* Bounded native transport progress with all author callbacks deferred until
   webnet_pump. Safe at an author's native checkpoint; never recursively pumps. */
void webnet_service(webnet *,uint64_t now_ms);
int webnet_timeout(const webnet *, uint64_t now_ms); /* -1 idle, otherwise next deadline */
/* Wait on the caller's event queue, where webnet keeps watches of its
   unpaused transports (tokens with top byte 0x57; the caller's own watches,
   such as its window, use others). The wait ends no later than the pump's
   next deadline. Every report (at most max) is returned to the caller, which
   hands on those that are not webnet's; their count, or a negative n_result.
   No callbacks, no DOM access. Caller consumes queued GUI events beforehand. */
int webnet_wait(webnet *,n_handle queue,int timeout_ms,struct n_event *events,size_t max);
bool webnet_busy(const webnet *);
/* Diagnostics: live request transports, excluding queued/retained responses.
   The persistent worker itself is owned until webnet_free. */
unsigned webnet_inflight(const webnet *);
void webnet_cancel(webnet *, uint64_t id); /* cancellation does not invoke the callback */
/* Demand gate: no response reads, credit replenishment or callbacks while
   paused. Byte credit bounds unread pipe storage. Worker retirement does not
   release retained response bytes or its request before resume/cancellation. */
void webnet_resume(webnet *, uint64_t id);
/* Document teardown: closes non-keepalive request channels; keepalive callbacks and
   copied request policy remain owned by net until completion or explicit cancel. */
void webnet_cancel_generation(webnet *, uint64_t generation);

/* WebSocket channels (RFC 6455). The isolated service performs the opening
   handshake, framing and control frames; whole messages cross the channel.
   Events are delivered only from webnet_pump; FAILED and CLOSED are final. */
#define WEBNET_SOCKET_TEXT 1u
#define WEBNET_SOCKET_BINARY 2u
enum webnet_socket_event { WEBNET_SOCKET_OPENED, WEBNET_SOCKET_RECEIVED, WEBNET_SOCKET_CLOSED, WEBNET_SOCKET_FAILED };
struct webnet_socket_message {
    enum webnet_socket_event event;
    unsigned code;      /* RECEIVED: TEXT or BINARY; CLOSED: the close code (1005 none) */
    const void *data;   /* message, close reason or error text */
    size_t length;
    const char *protocol; /* OPENED: the selected subprotocol, "" for none */
};
typedef void (*webnet_socket_callback)(webnet *, uint64_t id, const struct webnet_socket_message *, void *opaque);
/* url is ws: or wss:, origin the initiating document, headers "" or one
   "Sec-WebSocket-Protocol: ...\r\n" field; credentials apply to cookies. */
uint64_t webnet_socket_open(webnet *, const struct webnet_request *, webnet_socket_callback, void *opaque);
bool webnet_socket_send(webnet *, uint64_t id, unsigned type, const void *data, size_t length);
/* Starts the closing handshake; code 0 sends a close frame without a code. */
bool webnet_socket_close(webnet *, uint64_t id, unsigned code, const void *reason, size_t length);
/* Message bytes queued but not yet handed to the service. */
size_t webnet_socket_buffered(const webnet *, uint64_t id);
/* Drops the channel without further callbacks (document teardown, GC). */
void webnet_socket_release(webnet *, uint64_t id);
