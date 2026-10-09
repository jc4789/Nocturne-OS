/* Nocturne browser requests. All work happens in isolated /bin/webfetch children. */
#pragma once
#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>
#include "http.h"

#define WEBNET_BODY_LIMIT (16u * 1024u * 1024u)
#define WEBNET_SCRIPT_BODY_LIMIT (32u * 1024u * 1024u)
#define WEBNET_TIMEOUT_MS 30000u
#define WEBNET_URL_MAX HTTP_URL_MAX
#define WEBNET_REQUEST_HEADERS_MAX 8192
#define WEBNET_HEADERS_MAX WEBNET_REQUEST_HEADERS_MAX /* legacy request-sized alias */
/* Response storage includes NUL plus room for negotiated status/metadata.
   Independent of the unchanged 8 KiB author request-header bound. */
#define WEBNET_RESPONSE_HEADERS_MAX (64u * 1024u + 512u + 1u)
#define WEBNET_METHOD_MAX 64

/* HTTP token validation belongs at the native boundary as well as in Web IDL.
   No method spelling can inject bytes into the request line. */
static inline bool webnet_method_valid(const char *s) {
    if (!s || !*s) return false;
    unsigned n = 0;
    for (; s[n]; n++) {
        unsigned char c = (unsigned char)s[n];
        if (n + 1 >= WEBNET_METHOD_MAX) return false;
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
enum webnet_kind { WEBNET_NAVIGATION, WEBNET_CLASSIC, WEBNET_MODULE, WEBNET_RESOURCE, WEBNET_FETCH };
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
    enum webnet_cache cache_mode; /* Cacheless host: network misses, or cache-only failure; never stored responses. */
};
struct webnet_response {
    int status;
    const char *final_url;
    const char *headers; /* Complete filtered block; never a capacity-truncated prefix. */
    const void *body;
    size_t body_len;
    const char *error; /* empty for a received HTTP response, including HTTP errors */
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
/* Document teardown: kills and reaps matching children before returning (no callbacks). */
void webnet_cancel_generation(webnet *, uint64_t generation);
