/* Deterministic production queue boundaries. Pending native output is forced;
   no socket-buffer size or host timing determines whether the queue fills. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../user/include/http.h"
struct net_stream {size_t allowance,accepted;};
static uint64_t clock_ms;
static void (*sleep_hook)(void);
uint64_t uptime_ms(void){return clock_ms;}
void yield(void){}
int msleep(unsigned n){clock_ms+=n;if(sleep_hook)sleep_hook();return 0;}
long ns_try_write(net_stream *s,const void *p,size_t n){(void)p;if(!n)return 0;
    if(!s->allowance)return -2;if(n>s->allowance)n=s->allowance;s->allowance-=n;s->accepted+=n;return (long)n;}
long ns_try_read(net_stream *s,void *p,size_t n){(void)s;(void)p;(void)n;return -2;}
const char *ns_error(net_stream *s){(void)s;return "forced pending output";}
#include "../user/libc/http2.c"
static unsigned checks,failed;
#define CHECK(x) do{checks++;if(!(x)){failed++;printf("FAIL H2 queue:%d %s\n",__LINE__,#x);}}while(0)
static size_t remaining(const struct http2_connection *c){size_t n=0;for(const struct h2_output *o=c->output;o;o=o->next)n+=o->len-o->at;return n;}
static struct http2_connection *capacity_connection;
static void release_capacity(void){if(clock_ms>=3)capacity_connection->active=H2_STREAMS-1;}
static int cancel_capacity(void *ctx){return clock_ms>=*(uint64_t*)ctx;}
static void capacity_tests(void) {
    net_stream wire={.allowance=1024};struct http2_connection *c=http2_new(&wire);CHECK(c!=NULL);if(!c)return;
    struct http_req request={.url="http://test/capacity",.timeout_ms=5};
    struct url_owned url={.host="test",.path="/capacity",.port=80};char error[128];struct http_transport_result result={0};
    c->active=H2_STREAMS;c->settings=true;clock_ms=0;capacity_connection=c;sleep_hook=release_capacity;
    struct http2_stream *s=http2_start(c,&request,&url,"GET /capacity HTTP/1.1\r\nHost: test\r\n\r\n",NULL,&result,error,sizeof error);
    CHECK(s!=NULL&&clock_ms==3&&c->active==H2_STREAMS);if(s)http2_release(s);
    CHECK(!c->start_first&&!c->start_last&&!c->header_waiters);sleep_hook=NULL;
    c->active=H2_STREAMS;clock_ms=0;errno=0;
    s=http2_start(c,&request,&url,"GET /capacity HTTP/1.1\r\nHost: test\r\n\r\n",NULL,&result,error,sizeof error);
    CHECK(s==NULL&&errno==ETIMEDOUT&&clock_ms==5);
    CHECK(!c->dead&&!c->start_first&&!c->start_last&&!c->header_waiters);
    uint64_t cancel_at=2;request.ctx=&cancel_at;struct http_stream_hooks hooks={.cancelled=cancel_capacity};clock_ms=0;errno=0;
    s=http2_start(c,&request,&url,"GET /capacity HTTP/1.1\r\nHost: test\r\n\r\n",&hooks,&result,error,sizeof error);
    CHECK(s==NULL&&errno==ECANCELED&&clock_ms==2);
    CHECK(!c->dead&&!c->start_first&&!c->start_last&&!c->header_waiters);
    printf("H2 capacity: released=3ms timeout=5ms cancel=2ms active=%u\n",c->active);http2_free(c);
}
int main(void) {
    net_stream wire={0};struct http2_connection *c=http2_new(&wire);CHECK(c!=NULL);if(!c)return 1;
    unsigned char *body=calloc(1,8u*1024*1024);CHECK(body!=NULL);if(!body){http2_free(c);return 1;}
    struct http_req rq={.body=body,.body_len=8u*1024*1024};
    struct http2_stream a={.connection=c,.request=&rq,.id=1,.send_window=16u*1024*1024,.recv_window=65535,.headers_started=true};
    c->streams=&a;c->active=1;c->next_id=3;c->send_window=16u*1024*1024;c->settings=true;
    for(unsigned i=0;i<400;i++)pump(c);
    printf("H2 queue boundary: bytes=%zu queued-body=%zu dead=%u accepted=%zu\n",c->output_bytes,a.sent,c->dead,wire.accepted);
    CHECK(!c->dead);
    CHECK(c->output_bytes==4194304u-65536u);
    CHECK(remaining(c)==c->output_bytes&&wire.accepted==0);
    if(c->dead){free(body);http2_free(c);printf("http2-queue: %u checks, %u failed\n",checks,failed);return 1;}
    /* Control frames retain capacity even while application output is full. */
    size_t before=c->output_bytes;update(c,0,16384);reset(c,1,8);
    CHECK(!c->dead&&c->output_bytes==before+26&&c->output_bytes<=4194304);
    CHECK(remaining(c)==c->output_bytes);
    /* A second stream receives the next released application-output slot. */
    struct http2_stream b={.connection=c,.request=&rq,.id=3,.send_window=16u*1024*1024,.recv_window=65535,.headers_started=true,.next=&a};
    c->streams=&b;c->active=2;c->next_id=5;
    size_t a_before=a.sent;
    wire.allowance=35;pump(c); /* 26 control bytes + one 9-byte frame header */
    CHECK(!c->dead&&c->output_bytes<=4194304u-65536u+26);
    wire.allowance=35;pump(c);
    CHECK(!c->dead&&b.sent>0&&a.sent>=a_before);
    CHECK(remaining(c)==c->output_bytes);
    size_t b_before=b.sent;a_before=a.sent;c->header_waiters=1;wire.allowance=64;pump(c);c->header_waiters=0;
    CHECK(a.sent==a_before&&b.sent==b_before); /* headers get the newly freed slot */
    /* Removing queued DATA restores only its reserved, wholly unsent credit. */
    size_t restore=0;for(struct h2_output *o=c->output;o;o=o->next)if(o->stream==1&&o->type==0&&!o->at)restore+=o->len-9;
    int64_t connection_before=c->send_window,stream_before=a.send_window;
    a.accepted=1;cancel_output(c,&a,true);
    CHECK(c->send_window==connection_before+(int64_t)restore&&a.send_window==stream_before+(int64_t)restore);
    CHECK(!c->dead&&remaining(c)==c->output_bytes&&c->output_bytes<65536);
    for(struct h2_output *o=c->output;o;o=o->next)CHECK(!(o->stream==1&&o->type==0&&!o->at));
    size_t controls=c->output_bytes;b.recv_window-=16384;
    for(unsigned i=0;i<16383;i++)stream_credit(&b,1);
    CHECK(c->output_bytes==controls&&b.pending_credit==16383);
    stream_credit(&b,1);CHECK(c->output_bytes==controls+13&&b.pending_credit==0&&b.recv_window==65535);
    controls=c->output_bytes;c->recv_window-=16384;connection_credit(c,16383);
    CHECK(c->output_bytes==controls);connection_credit(c,1);
    CHECK(c->output_bytes==controls+13&&c->pending_credit==0&&c->recv_window==65535);
    free(body);http2_free(c);
    capacity_tests();printf("http2-queue: %u checks, %u failed\n",checks,failed);return failed!=0;
}
