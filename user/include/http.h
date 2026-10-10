/* Byte streams over TCP or TLS (tls.c) and a small HTTP/1.1 client (http.c). */
#pragma once
#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

/* ---- streams: plain TCP or TLS 1.2 (BearSSL, verified against the built-in Mozilla roots) ---- */
typedef struct net_stream net_stream;
/* connect to host:port; on failure returns NULL and writes a message into err */
net_stream *ns_open(const char *host, uint16_t port, bool tls, int timeout_ms, char *err, size_t errlen);
long ns_write(net_stream *s, const void *buf, size_t n); /* all or -1 */
long ns_read(net_stream *s, void *buf, size_t n);        /* >0 bytes, 0 = end of stream, -1 = error */
void ns_set_timeout(net_stream *s, int timeout_ms);      /* per read; -1 waits forever */
const char *ns_error(net_stream *s);                     /* last error, "" if none */
void ns_close(net_stream *s);

/* ---- URLs ---- */
#define HTTP_URL_MAX 16384u /* legacy fixed struct/url_parse bound, including NUL */
#define HTTP_URL_PATH_MAX HTTP_URL_MAX
struct url {
    bool tls;
    char host[128];
    uint16_t port;
    char path[HTTP_URL_PATH_MAX]; /* includes the query string; "/" if empty */
};
bool url_parse(const char *s, struct url *u);

/* Exact-length storage for the real transport. The legacy struct/url_parse
   remain ABI-compatible for callers not yet migrated. A successful parse owns
   both strings; release with url_owned_free. Pass a fresh output, not a live
   owned result. The span entry rejects embedded NUL rather than accepting a
   different prefix. IPv6 transport is not implemented and fails closed. */
struct url_owned {
    bool tls;
    char *host;
    uint16_t port;
    char *path;
};
bool url_parse_owned_n(const char *s, size_t length, struct url_owned *u);
bool url_parse_owned(const char *s, struct url_owned *u);
void url_owned_free(struct url_owned *u);

enum http_url_result {
    HTTP_URL_OOM = -2, HTTP_URL_INVALID = -1, HTTP_URL_OPAQUE = 0, HTTP_URL_TUPLE = 1
};
/* Same grammar/ownership as url_parse_owned_n, but distinguish malformed spans
   from actual allocation failure. The owned entries accept uint32_t byte spans
   (plus a representable terminator), without the legacy fixed-buffer quota.
   Fresh u is zero on failure; on TUPLE release it with url_owned_free. */
enum http_url_result url_parse_owned_result_n(const char *s, size_t length, struct url_owned *u);
/* Fresh output slots are NULL on every non-TUPLE result. On TUPLE each output
   owns an exact-length malloc string. canonical may be NULL to request origin
   only; origin is required. Unsupported non-HTTP documents are OPAQUE, never
   comparable empty origins. HTTP parse/allocation failures are distinct.
   Fragment is excluded, host ASCII case/default port canonicalize; userinfo,
   embedded NUL and unsupported IPv6 fail closed. Caller frees both strings. */
enum http_url_result http_canonical_owned_n(const char *raw, size_t length, char **canonical, char **origin);
enum http_url_result http_origin_owned_n(const char *raw, size_t length, char **origin);
enum http_url_result http_origin_owned(const char *raw, char **origin);

/* ---- HTTP ---- */
/* body callback: return a negative value to abort the transfer */
typedef int (*http_body_cb)(void *ctx, const char *data, size_t n);
/* Each complete final-response header line, after allocation succeeds. Negative aborts.
   The line has no CRLF and is not truncated. Never log secret field values. */
typedef int (*http_header_cb)(void *ctx, const char *line, size_t n);

struct http_req {
    const char *method;  /* "GET" if NULL */
    const char *url;
    const char *headers; /* extra request headers, each ending in "\r\n" (may be NULL) */
    const void *body;
    size_t body_len;
    int timeout_ms;      /* per read; 0 uses the signed API's representation maximum */
    http_body_cb on_body; /* stream the body here; NULL collects it into resp.body */
    void *ctx;
    http_header_cb on_header;
};

struct http_resp {
    int status;
    char headers[4096]; /* Complete small block; empty when headers_full is used. */
    char *body;         /* malloc'd and NUL-terminated when on_body is NULL */
    size_t body_len;
    char error[160];
    char status_text[256]; /* Actual HTTP reason phrase, not an invented status lookup. */
    char *headers_full; /* Owned complete large block, not a truncated snapshot. */
    size_t headers_len; /* Raw field bytes including CRLF, excluding status/empty line/NUL. */
    char *status_text_full; /* Owned complete long phrase; inline phrase is empty when used. */
};

/* returns 0 when a response arrived (check status), -1 on failure with resp->error set */
int http_request(const struct http_req *rq, struct http_resp *resp);
/* Explicit finite response bound for trusted resource loaders. Applies to both
   encoded wire bytes and decoded bytes, including callback delivery. The legacy
   entry keeps its existing defaults; http_req and persisted caller ABI are unchanged. */
#define HTTP_RESPONSE_BODY_MAX ((size_t)UINT32_MAX)
int http_request_limited(const struct http_req *rq, struct http_resp *resp, size_t body_limit);
/* Opt-in lifecycle hooks without changing the persisted http_req layout.
   Headers are complete/validated before body callbacks. A positive headers
   return stops after metadata (redirects); negative aborts the request. */
struct http_stream_hooks {
    int (*headers)(void *ctx, const struct http_resp *response);
    int (*upload)(void *ctx, size_t transmitted, size_t total);
};
int http_request_stream(const struct http_req *, struct http_resp *, size_t,
                        const struct http_stream_hooks *);
/* Both successful and failed responses may be freed. Do not free a borrowed
   headers_full used solely to adapt an existing header block for lookup. */
void http_resp_free(struct http_resp *resp);
/* Complete NUL-terminated block; valid until http_resp_free, never use the
   legacy inline field for security decisions. Empty for a failed request. */
const char *http_response_headers(const struct http_resp *resp);
const char *http_response_status_text(const struct http_resp *resp);
/* Case-insensitive lookup in the complete block; returns out or NULL. */
char *http_header(const struct http_resp *resp, const char *name, char *out, size_t n);
