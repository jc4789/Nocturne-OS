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
struct url {
    bool tls;
    char host[128];
    uint16_t port;
    char path[1024]; /* includes the query string; "/" if empty */
};
bool url_parse(const char *s, struct url *u);

/* ---- HTTP ---- */
/* body callback: return a negative value to abort the transfer */
typedef int (*http_body_cb)(void *ctx, const char *data, size_t n);

struct http_req {
    const char *method;  /* "GET" if NULL */
    const char *url;
    const char *headers; /* extra request headers, each ending in "\r\n" (may be NULL) */
    const void *body;
    size_t body_len;
    int timeout_ms;      /* per read; 0 means 60 s */
    http_body_cb on_body; /* stream the body here; NULL collects it into resp.body */
    void *ctx;
};

struct http_resp {
    int status;
    char headers[4096]; /* raw response header block */
    char *body;         /* malloc'd and NUL-terminated when on_body is NULL */
    size_t body_len;
    char error[160];
};

/* returns 0 when a response arrived (check status), -1 on failure with resp->error set */
int http_request(const struct http_req *rq, struct http_resp *resp);
void http_resp_free(struct http_resp *resp);
/* case-insensitive header lookup in resp->headers; returns out or NULL */
char *http_header(const struct http_resp *resp, const char *name, char *out, size_t n);
