/* Native HTTP/2 transport. Owns framing, HPACK and per-stream flow control;
   http.c owns HTTP semantics, decoding, callbacks and the origin pool. */
#pragma once
#include "../include/http.h"
struct http2_connection;
struct http2_stream;
struct http2_connection *http2_new(net_stream *);
void http2_free(struct http2_connection *);
bool http2_available(struct http2_connection *);
bool http2_reusable(struct http2_connection *);
struct http2_stream *http2_start(struct http2_connection *,const struct http_req *,
    const struct http_target *,const char *head,const struct http_stream_hooks *,struct http_transport_result *,char *,size_t);
long http2_read(struct http2_stream *,void *,size_t);
int http2_finish(struct http2_stream *,bool,const struct http_stream_hooks *);
const char *http2_error(struct http2_stream *);
void http2_release(struct http2_stream *);
