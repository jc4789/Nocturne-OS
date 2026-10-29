/* Byte streams over TCP or TLS (tls.c) and a small HTTP/1.1 client (http.c). */
#pragma once
#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <threads.h>
#include "weburl.h"

/* ---- streams: plain TCP or TLS 1.3 and 1.2 (tls.c on BearSSL's primitives, verified against the built-in Mozilla roots) ---- */
typedef struct net_stream net_stream;
/* Opt-in native diagnostics; no request/hook/response layout changes. Times
   include native scheduling, not just server latency. Never expose as fields. */
enum http_transport_phase { HTTP_PHASE_NONE, HTTP_PHASE_RESOLVE, HTTP_PHASE_CONNECT,
    HTTP_PHASE_TLS, HTTP_PHASE_HEADER, HTTP_PHASE_BODY, HTTP_PHASE_DONE };
struct http_transport_timing {
    uint64_t resolve_ms, connect_ms, tls_ms, first_header_ms, header_ms, body_ms;
    uint64_t tls_io_ms, tls_other_ms; /* callback wall versus all other wall, not crypto CPU */
    enum http_transport_phase phase;
};
/* connect to host:port; on failure returns NULL and writes a message into err */
net_stream *ns_open(const char *host, uint16_t port, bool tls, int timeout_ms, char *err, size_t errlen);
net_stream *ns_open_timed(const char *host, uint16_t port, bool tls, int timeout_ms,
                          char *err, size_t errlen, struct http_transport_timing *timing);
/* Read-only connection diagnostics; no resolver or connect policy override.
   Version 1 records the real resolver count, attempted TCP candidates and the
   successful peer. Separate from the existing timing structure's native ABI. */
struct ns_connection_info { uint32_t version, candidates, attempts, peer_ip; };
int ns_get_connection_info(const net_stream *,struct ns_connection_info *);
/* Fetch's "connection timing info" of one connection, which the connection
   records as it opens: when the domain lookup started and ended, when the
   connection started, when its secure handshake started (0 on a cleartext
   connection) and when it was established, handshake included. Absolute
   uptime_ms values. protocol is the ALPN negotiated protocol ID; an HTTP/1
   connection that negotiated none reports "http/1.1". */
#define HTTP_PROTOCOL_MAX 16u
#define HTTP_CONTENT_ENCODING_MAX 16u
struct http_connection_timing {
    uint64_t domain_lookup_start, domain_lookup_end, connect_start, connect_end, secure_connection_start;
    char protocol[HTTP_PROTOCOL_MAX];
};
int ns_get_connection_timing(const net_stream *, struct http_connection_timing *);
long ns_write(net_stream *s, const void *buf, size_t n); /* all or -1 */
long ns_read(net_stream *s, void *buf, size_t n);        /* >0 bytes, 0 = end of stream, -1 = error */
void ns_set_timeout(net_stream *s, int timeout_ms);      /* per read; -1 waits forever */
const char *ns_error(net_stream *s);                     /* last error, "" if none */
void ns_close(net_stream *s);
/* HTTP ALPN is opt-in: non-HTTP users of ns_open keep http/1.1. Session
   resumption and connection reuse never cross this exact partition key. */
net_stream *ns_open_http_timed(const char *, uint16_t, bool, int, char *, size_t,
                              struct http_transport_timing *, const char *partition);
/* The cancellation hook is borrowed only while opening/handshaking. It is
   always cleared before publishing a connection to the shared HTTP/2 pool. */
net_stream *ns_open_http_cancelled_timed(const char *, uint16_t, bool, int, char *, size_t,
    struct http_transport_timing *, const char *partition, int (*cancelled)(void *), void *);
const char *ns_protocol(net_stream *s);
bool ns_session_resumed(net_stream *s); /* authenticated handshake kept ID AND master secret */
void ns_set_cancel(net_stream *, int (*cancelled)(void *), void *);
/* Nonblocking application read for the HTTP/2 connection pump. -2 is pending;
   unlike a failed TLS read, pending does not poison the TLS engine. */
long ns_try_read(net_stream *, void *, size_t);
/* ns_try_read waiting up to timeout_ms for transport bytes; -2 still pending.
   With a lock (held by the caller) another thread may ns_try_write the same
   stream under that lock while this one sleeps in the transport. */
long ns_read_wait(net_stream *, void *, size_t, int timeout_ms, mtx_t *lock);
/* From another thread: reset the connection, waking a blocked read. The
   stream then only fails and must still be released with ns_abort. */
void ns_reset(net_stream *);
/* Native nonblocking send: accepted prefix, -2 pending, -1 error. n==0
   drains pending TLS records without consuming application bytes. */
long ns_try_write(net_stream *, const void *, size_t);
void ns_abort(net_stream *);
uint64_t ns_bytes_sent(net_stream *); /* accepted TCP bytes, including TLS records */
void ns_sessions_clear(void);

/* ---- URLs ---- */
/* The inline URL of struct web_response (web.h) and the network wire's older bound, NUL
   included. A request's own URL has no fixed bound. */
#define HTTP_URL_MAX 16384u

/* What a request to an http or https URL is made with, read from the URL Standard's record
   of it (weburl.h), never from a second reading of its text. The URL's credentials are never
   part of a request: Fetch sends them only from an authentication entry, after a 401, which
   this layer does not make. */
struct http_target {
    bool tls;          /* https */
    uint16_t port;     /* the URL's port, or the scheme's default */
    char *host;        /* the host, serialized: an IPv6 address in brackets */
    char *authority;   /* the host and the URL's port, if any: the Host field and :authority */
    char *path;        /* path and query (origin-form); never empty, never the fragment */
};
/* url (a URL's serialization, or any absolute URL the URL parser reads) as a request's
   target. WEB_URL_TUPLE: t's strings are malloc'd (http_target_free them); WEB_URL_OPAQUE: a
   URL of another scheme; WEB_URL_INVALID: not a URL; WEB_URL_OOM. t is zero unless TUPLE. */
enum web_url_result http_target_of(const char *url, struct http_target *t);
void http_target_free(struct http_target *t);

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
    int (*cancelled)(void *ctx);
};
int http_request_stream(const struct http_req *, struct http_resp *, size_t,
                        const struct http_stream_hooks *);
int http_request_stream_timed(const struct http_req *, struct http_resp *, size_t,
                              const struct http_stream_hooks *, struct http_transport_timing *);
enum http_error_kind { HTTP_ERR_NONE, HTTP_ERR_DNS_TEMP, HTTP_ERR_DNS_NOT_FOUND,
    HTTP_ERR_CONNECT, HTTP_ERR_TIMEOUT, HTTP_ERR_RESET, HTTP_ERR_PROTOCOL,
    HTTP_ERR_TLS_VERIFY, HTTP_ERR_CANCELLED, HTTP_ERR_NO_MEMORY, HTTP_ERR_IO,
    HTTP_ERR_UNPROCESSED };
struct http_transport_result {
    enum http_error_kind error;
    enum http_transport_phase phase;
    size_t request_bytes_sent, response_bytes_received; /* sent is conservative TCP wire bytes */
    bool reused;
    bool request_unprocessed; /* peer explicitly refused it / GOAWAY above last-stream */
    /* Native observations only; never used to choose transport behavior.
       Connection identity is process-local and survives pool clear/retire.
       HTTP/1 uses stream zero; h2 reports its actual wire stream identifier. */
    uint64_t connection_id;
    uint32_t stream_id;
    bool http2;
    /* This attempt's part of Fetch's timing record (absolute uptime_ms, 0 for
       not reached): the timing info of the connection it used, as that
       connection recorded it when it opened (an earlier request's for a reused
       one; the caller clamps it), the protocol of this exchange, when the
       request started to go out (final network-request start), when the first
       interim (1xx) and the final response began to arrive, and the body's
       encoded size: the bytes transmitted, before content codings are undone. */
    struct http_connection_timing connection;
    uint64_t request_start, first_interim_response_start, final_response_start;
    uint64_t encoded_body_size;
    /* Fetch's filtered content coding of the response body: "" (none, or no
       body byte arrived), the lowercased coding this client undoes,
       "multiple" or "@unknown" (HTTP-network fetch, response body info). */
    char content_encoding[HTTP_CONTENT_ENCODING_MAX];
};
/* Fetch's timing record of one fetch, from its start to the end of its body:
   "fetch timing info" (start, redirect, post-redirect start, final connection
   timing info, request and response starts, end), the final response's cache
   state, "timing allow passed" flag, redirect taint and body info, and its
   Server-Timing field values. Absolute uptime_ms values, 0 for not reached.
   The transport (webfetch) records it as the request goes, the browser's
   network layer (webnet.c) adds the start time it was asked at and clamps a
   reused connection's times, and the document builds Navigation and Resource
   Timing entries from it. The record is never the opaque form: the
   performance timeline makes that itself when timing_allow_passed is false. */
enum http_cache_state { HTTP_CACHE_STATE_NONE, HTTP_CACHE_STATE_LOCAL, HTTP_CACHE_STATE_VALIDATED };
struct http_fetch_timing {
    bool valid;
    /* A network hop ran, so "final connection timing info" is non-null. */
    bool connection_valid;
    /* Fetch: every hop passed the TAO check (timing allow passed flag). */
    bool timing_allow_passed;
    /* Fetch: the response's redirect taint is "same-origin". */
    bool redirect_same_origin;
    /* HTML (create and initialize a Document): a navigation's redirect count
       is exposed (redirect taint same-origin, or the navigation TAO check over
       every redirect hop passed for a request that had no client or sent a
       referrer). Meaningful for navigations only. */
    bool navigation_redirects_exposed;
    uint8_t cache_state; /* enum http_cache_state */
    uint32_t redirect_count;
    uint64_t start_time, redirect_start, redirect_end, post_redirect_start;
    struct http_connection_timing connection;
    uint64_t final_network_request_start, first_interim_network_response_start,
             final_network_response_start, end_time;
    uint64_t encoded_body_size, decoded_body_size;
    char content_encoding[HTTP_CONTENT_ENCODING_MAX];
    /* Owned by whoever holds the record (NULL: none): the final response's
       Server-Timing field values combined (", "), before the timing allow
       check; the timeline parses them (Server Timing §2). */
    char *server_timing;
};
/* Copies a record into *out, the Server-Timing values included; false when
   that copy cannot be allocated (out is then the record without them). */
static inline bool http_fetch_timing_copy(struct http_fetch_timing *out, const struct http_fetch_timing *in) {
    *out = *in; out->server_timing = NULL;
    if (!in->server_timing) return true;
    size_t n = strlen(in->server_timing);
    if (!(out->server_timing = malloc(n + 1))) return false;
    memcpy(out->server_timing, in->server_timing, n + 1);
    return true;
}
static inline void http_fetch_timing_free(struct http_fetch_timing *timing) {
    free(timing->server_timing); timing->server_timing = NULL;
}
int http_request_stream_ex(const struct http_req *, struct http_resp *, size_t,
                           const struct http_stream_hooks *, struct http_transport_timing *,
                           const char *partition, struct http_transport_result *);
/* Native explicit cleartext HTTP/2 prior knowledge (RFC 9113 3.3). HTTPS
   always uses certificate validation and ALPN through the normal entry. */
int http_request_h2_prior_knowledge_ex(const struct http_req *, struct http_resp *, size_t,
    const struct http_stream_hooks *, const char *partition, struct http_transport_result *);
/* Drop idle connections/session keys at an explicit security-context teardown. */
void http_pool_clear(void);
/* Both successful and failed responses may be freed. Do not free a borrowed
   headers_full used solely to adapt an existing header block for lookup. */
void http_resp_free(struct http_resp *resp);
/* Complete NUL-terminated block; valid until http_resp_free, never use the
   legacy inline field for security decisions. Empty for a failed request. */
const char *http_response_headers(const struct http_resp *resp);
const char *http_response_status_text(const struct http_resp *resp);
/* Case-insensitive lookup in the complete block; returns out or NULL. */
char *http_header(const struct http_resp *resp, const char *name, char *out, size_t n);
