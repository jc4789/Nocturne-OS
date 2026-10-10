/* Nocturne browser requests. All work happens in isolated /bin/webfetch children. */
#pragma once
#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include "http.h"

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
                   WEBNET_REPORT }; /* Append-only native CSP transport. */
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
enum webnet_credentials { WEBNET_CREDENTIALS_OMIT, WEBNET_CREDENTIALS_SAME_ORIGIN, WEBNET_CREDENTIALS_INCLUDE };
enum webnet_cache { WEBNET_CACHE_DEFAULT, WEBNET_CACHE_NO_STORE, WEBNET_CACHE_RELOAD,
                    WEBNET_CACHE_NO_CACHE, WEBNET_CACHE_FORCE_CACHE, WEBNET_CACHE_ONLY_IF_CACHED };
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
    bool same_origin; /* Enforced on every redirect hop, not just the initial URL. */
    bool no_cors, no_referrer; /* Safelisted Fetch; opaque response after cross-origin hop. */
    bool keepalive; /* Fetch survives generation retirement; explicit cancel still aborts. */
    bool image_upgrade; /* Trusted ordinary non-CORS, non-imageset image only; RESOURCE. */
    uint64_t fetch_group; /* Distinct environment settings object, including child documents. */
    enum webnet_cache cache_mode; /* Cacheless host: network misses, or cache-only failure; never stored responses. */
    bool stream_response; /* Fetch lifecycle frames; other destinations remain buffered. */
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
};
typedef void (*webnet_callback)(webnet *, uint64_t id, uint64_t generation,
                               const struct webnet_response *, void *opaque);

webnet *webnet_create(void);
void webnet_free(webnet *net);
/* Browser-owned session jar. Native host entry points for document.cookie.
   get returns bytes excluding NUL, out=NULL queries length; -1 on failure.
   set: 1 accepted/deleted, 0 ignored by cookie policy, -1 resource failure. */
long webnet_cookie_get(webnet *, const char *url, char *out, size_t cap);
int webnet_cookie_set(webnet *, const char *url, const char *value);
/* Copies all request data; 0 means invalid request, queue full, or allocation failure. */
uint64_t webnet_submit(webnet *, const struct webnet_request *, webnet_callback, void *opaque);
/* Dispatches completions only here. Response pointers remain valid until callback returns.
   Callbacks may submit/cancel requests, but must not free net or recursively pump it. */
void webnet_pump(webnet *, uint64_t now_ms);
int webnet_timeout(const webnet *, uint64_t now_ms); /* -1 idle, otherwise <=10 ms */
bool webnet_busy(const webnet *);
void webnet_cancel(webnet *, uint64_t id); /* cancellation does not invoke the callback */
/* One-frame demand gate. No reads while paused: the child's pipe applies
   backpressure all the way to its network body callback. */
void webnet_resume(webnet *, uint64_t id);
/* Document teardown: kills/reaps non-keepalive children; keepalive callbacks and
   copied request policy remain owned by net until completion or explicit cancel. */
void webnet_cancel_generation(webnet *, uint64_t generation);
