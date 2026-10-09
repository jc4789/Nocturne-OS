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
#define HTTP_URL_MAX 16384u /* shared HTTP/browser URL bound, including NUL */
#define HTTP_URL_PATH_MAX HTTP_URL_MAX
struct url {
    bool tls;
    char host[128];
    uint16_t port;
    char path[HTTP_URL_PATH_MAX]; /* includes the query string; "/" if empty */
};
bool url_parse(const char *s, struct url *u);

/* ---- HTTP ---- */
#define HTTP_RESPONSE_HEADERS_LIMIT (64u * 1024u)
#define HTTP_RESPONSE_HEADER_LINE_MAX 16384u /* storage includes the terminating NUL */
/* body callback: return a negative value to abort the transfer */
typedef int (*http_body_cb)(void *ctx, const char *data, size_t n);
/* Each complete final-response header line, after its storage bound is checked. Negative aborts.
   The line has no CRLF and is not truncated. Never log secret field values. */
typedef int (*http_header_cb)(void *ctx, const char *line, size_t n);

struct http_req {
    const char *method;  /* "GET" if NULL */
    const char *url;
    const char *headers; /* extra request headers, each ending in "\r\n" (may be NULL) */
    const void *body;
    size_t body_len;
    int timeout_ms;      /* per read; 0 means 60 s */
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
};

/* returns 0 when a response arrived (check status), -1 on failure with resp->error set */
int http_request(const struct http_req *rq, struct http_resp *resp);
/* Explicit finite response bound for trusted resource loaders. Applies to both
   encoded wire bytes and decoded bytes, including callback delivery. The legacy
   entry keeps its existing defaults; http_req and persisted caller ABI are unchanged. */
#define HTTP_RESPONSE_BODY_MAX (32u * 1024u * 1024u)
int http_request_limited(const struct http_req *rq, struct http_resp *resp, size_t body_limit);
/* Both successful and failed responses may be freed. Do not free a borrowed
   headers_full used solely to adapt an existing header block for lookup. */
void http_resp_free(struct http_resp *resp);
/* Complete NUL-terminated block; valid until http_resp_free, never use the
   legacy inline field for security decisions. Empty for a failed request. */
const char *http_response_headers(const struct http_resp *resp);
/* Case-insensitive lookup in the complete block; returns out or NULL. */
char *http_header(const struct http_resp *resp, const char *name, char *out, size_t n);
