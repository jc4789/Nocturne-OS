/* HTTP/1 parser fixtures replace exactly one ns_open transaction. A persistent
   pool must not retain their borrowed, rewritten static wire between cases.
   HTTP/2 is exercised by transport8 over real native streams; entering it in
   one of these HTTP/1-only mocks is a fixture error and aborts immediately. */
#pragma once
#include <stdlib.h>
#include <string.h>
struct http2_connection;
struct http2_stream;
net_stream *ns_open_http_timed(const char *h,uint16_t p,bool tls,int t,char *e,size_t n,
    struct http_transport_timing *m,const char *partition){(void)partition;return ns_open_timed(h,p,tls,t,e,n,m);}
net_stream *ns_open_http_cancelled_timed(const char *h,uint16_t p,bool tls,int t,char *e,size_t n,
    struct http_transport_timing *m,const char *partition,int (*cancelled)(void *),void *ctx){
    (void)cancelled;(void)ctx;return ns_open_http_timed(h,p,tls,t,e,n,m,partition);}
const char *ns_protocol(net_stream *s){(void)s;return "http/1.1";}
bool ns_session_resumed(net_stream *s){(void)s;return false;}
void ns_set_cancel(net_stream *s,int (*f)(void*),void *c){(void)s;(void)f;(void)c;}
void ns_abort(net_stream *s){if(s)ns_close(s);}
void ns_sessions_clear(void){}
uint64_t ns_bytes_sent(net_stream *s){(void)s;return 0;}
long ns_try_read(net_stream *s,void *p,size_t n){(void)s;(void)p;(void)n;abort();}
long ns_try_write(net_stream *s,const void *p,size_t n){(void)s;(void)p;(void)n;abort();}
#ifndef HTTP_MOCK_HAS_TIMEOUT
void ns_set_timeout(net_stream *s,int n){(void)s;(void)n;}
#endif
#ifdef _WIN32
void yield(void){}
int msleep(unsigned n){(void)n;return 0;}
#endif
struct http2_connection *http2_new(net_stream *s){(void)s;abort();}
void http2_free(struct http2_connection *c){if(c)abort();}
bool http2_available(struct http2_connection *c){(void)c;abort();}
bool http2_reusable(struct http2_connection *c){(void)c;abort();}
struct http2_stream *http2_start(struct http2_connection *c,const struct http_req *r,const struct url_owned *u,
    const char *h,const struct http_stream_hooks *k,struct http_transport_result *t,char *e,size_t n){
    (void)c;(void)r;(void)u;(void)h;(void)k;(void)t;(void)e;(void)n;abort();}
long http2_read(struct http2_stream *s,void *p,size_t n){(void)s;(void)p;(void)n;abort();}
int http2_finish(struct http2_stream *s,bool consumed,const struct http_stream_hooks *h){(void)s;(void)consumed;(void)h;abort();}
const char *http2_error(struct http2_stream *s){(void)s;abort();}
void http2_release(struct http2_stream *s){if(s)abort();}
#ifndef HTTP_MOCK_NO_ISOLATION
static inline int isolated_http_request(const struct http_req *q,struct http_resp *r){http_pool_clear();int code=http_request(q,r);http_pool_clear();return code;}
static inline int isolated_http_limited(const struct http_req *q,struct http_resp *r,size_t n){http_pool_clear();int code=http_request_limited(q,r,n);http_pool_clear();return code;}
static inline int isolated_http_stream(const struct http_req *q,struct http_resp *r,size_t n,const struct http_stream_hooks *h){http_pool_clear();int code=http_request_stream(q,r,n,h);http_pool_clear();return code;}
static inline int isolated_http_timed(const struct http_req *q,struct http_resp *r,size_t n,const struct http_stream_hooks *h,struct http_transport_timing *t){http_pool_clear();int code=http_request_stream_timed(q,r,n,h,t);http_pool_clear();return code;}
#define http_request isolated_http_request
#define http_request_limited isolated_http_limited
#define http_request_stream isolated_http_stream
#define http_request_stream_timed isolated_http_timed
#endif
