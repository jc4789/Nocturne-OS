/* Actual HTTP request builder over a synthetic stream, not website evidence. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../user/include/http.h"
#include "../user/libc/http.c"
struct net_stream { size_t at; };
static struct net_stream stream;
static unsigned writes,checks,failed;
static bool short_write;
static char sent[8192];
static size_t sent_n;
static uint64_t clock_ms;
uint64_t uptime_ms(void){return clock_ms;}
net_stream *ns_open(const char *h,uint16_t p,bool tls,int t,char *e,size_t n) {
    (void)h;(void)p;(void)tls;(void)t;(void)e;(void)n;stream.at=0;return &stream;
}
net_stream *ns_open_timed(const char *h,uint16_t p,bool tls,int t,char *e,size_t n,
                          struct http_transport_timing *timing){
    *timing=(struct http_transport_timing){.resolve_ms=3,.connect_ms=4,.tls_ms=tls?5:0};
    clock_ms+=timing->resolve_ms+timing->connect_ms+timing->tls_ms;
    return ns_open(h,p,tls,t,e,n);}
long ns_write(net_stream *s,const void *p,size_t n) {
    (void)s;writes++;clock_ms+=7;if(short_write)return n?(long)n-1:0;
    if(n>sizeof sent-sent_n-1)return -1;
    memcpy(sent+sent_n,p,n);sent_n+=n;sent[sent_n]=0;return (long)n;
}
long ns_read(net_stream *s,void *p,size_t n) {
    clock_ms+=11;
    const char *r="HTTP/1.1 200 OK\r\nContent-Length: 0\r\n\r\n";
    size_t left=strlen(r)-s->at;if(n>left)n=left;
    memcpy(p,r+s->at,n);s->at+=n;return (long)n;
}
void ns_close(net_stream *s){(void)s;clock_ms+=13;}
void ns_set_timeout(net_stream *s,int t){(void)s;(void)t;}
const char *ns_error(net_stream *s){(void)s;return "synthetic short write";}
void *brotli_decoder_begin(void){return NULL;}int brotli_decode(void *s,const unsigned char **in,size_t *n,unsigned char **out,size_t *room){(void)s;(void)in;(void)n;(void)out;(void)room;return -1;}void brotli_decoder_end(void *s){(void)s;}
struct inflate_stream *inflate_begin(enum deflate_wrap w,unsigned o,inflate_put p,void *c){(void)w;(void)o;(void)p;(void)c;return NULL;}enum inflate_result inflate_feed(struct inflate_stream *s,const void *in,size_t n,bool last,size_t *used){(void)s;(void)in;(void)n;(void)last;*used=0;return INFLATE_INVALID;}void inflate_end(struct inflate_stream *s){(void)s;}
#define HTTP_MOCK_HAS_TIMEOUT
#include "http_mock_lifecycle.h"
#define CHECK(c) do{checks++;if(!(c)){failed++;printf("FAIL HTTP batch line %d\n",__LINE__);}}while(0)
int main(void) {
    struct http_req q={.url="https://headers.invalid/path?q=1",.headers="X-Test: one\r\n"};
    struct http_resp r;
    CHECK(http_request(&q,&r)==0);CHECK(writes==1);
    http_resp_free(&r);
    struct http_transport_timing timing={0};
    CHECK(http_request_stream_timed(&q,&r,0,NULL,&timing)==0);
    CHECK(timing.phase==HTTP_PHASE_DONE&&timing.resolve_ms==3&&timing.connect_ms==4&&timing.tls_ms==5);
    /* A reusable socket has no close_notify wait in the response body phase. */
    CHECK(timing.first_header_ms==18&&timing.header_ms==18&&timing.body_ms==0);
    http_resp_free(&r);
    CHECK(strstr(sent,"GET /path?q=1 HTTP/1.1\r\nHost: headers.invalid\r\n")!=NULL);
    CHECK(strstr(sent,"X-Test: one\r\n\r\n")!=NULL);http_resp_free(&r);
    writes=0;sent_n=0;q.method="POST";q.body="ab";q.body_len=2;
    CHECK(http_request(&q,&r)==0);CHECK(writes==2);
    CHECK(strstr(sent,"Content-Length: 2\r\nX-Test: one\r\n\r\nab")!=NULL);http_resp_free(&r);
    short_write=true;writes=0;
    CHECK(http_request(&q,&r)<0);CHECK(writes==1 && r.error[0]);http_resp_free(&r);
    printf("HTTP header batching: %u checks, %u failed\n",checks,failed);
    return failed?1:0;
}
