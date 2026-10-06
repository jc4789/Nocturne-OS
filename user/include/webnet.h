/* Nocturne browser requests. All work happens in isolated /bin/webfetch children. */
#pragma once
#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

#define WEBNET_BODY_LIMIT (16u * 1024u * 1024u)
#define WEBNET_TIMEOUT_MS 30000u
#define WEBNET_URL_MAX 2048
#define WEBNET_HEADERS_MAX 8192

typedef struct webnet webnet;
enum webnet_kind { WEBNET_NAVIGATION, WEBNET_CLASSIC, WEBNET_MODULE, WEBNET_RESOURCE, WEBNET_FETCH };
enum webnet_credentials { WEBNET_CREDENTIALS_OMIT, WEBNET_CREDENTIALS_SAME_ORIGIN, WEBNET_CREDENTIALS_INCLUDE };
struct webnet_request {
    enum webnet_kind kind;
    uint64_t generation;
    const char *url;
    const char *origin; /* full initiating document URL, not a page-controlled Origin header */
    const char *method; /* GET (default) or POST */
    const char *headers; /* CRLF-delimited fields; prohibited fields are rejected */
    const void *body;
    size_t body_len;
    bool user_navigation; /* only this permits a file: top-level request */
    enum webnet_credentials credentials;
    bool force_preflight; /* Fetch use-CORS-preflight flag, not an author request header. */
};
struct webnet_response {
    int status;
    const char *final_url;
    const char *headers;
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
