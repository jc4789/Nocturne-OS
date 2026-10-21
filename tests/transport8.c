/* Real native HTTP/1.1 reuse and HTTP/2 multiplexing regressions. The host
   adapter changes only ns_* and native task I/O; guest uses production TLS/TCP. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#ifdef _WIN32
#include "../user/include/http.h"
extern void n_sleep_ms(uint64_t);
extern int thread_create(void (*)(void *),void *);
extern int thread_join(int);
extern uint64_t uptime_ms(void);
#else
#include "http.h"
#include "nocturne.h"
#endif
static unsigned checks,failed;
#define CHECK(x) do{checks++;if(!(x)){failed++;printf("FAIL transport8:%d %s\n",__LINE__,#x);}}while(0)
static char host[128]="10.0.2.2";
static unsigned http_port,h2_port;
static void address(char *out,size_t n,unsigned port,const char *path){snprintf(out,n,"http://%s:%u%s",host,port,path);}
static int request(const char *kind,const char *method,const char *partition,struct http_resp *r,struct http_transport_result *result) {
    char url[256],path[128];snprintf(path,sizeof path,"/api/net8-http?kind=%s",kind);address(url,sizeof url,http_port,path);
    struct http_req rq={.url=url,.method=method,.timeout_ms=10000};
    int code=http_request_stream_ex(&rq,r,1048576,NULL,NULL,partition,result);
    if(code<0)printf("transport8 HTTP1 %s %s error=%d phase=%d sent=%zu recv=%zu %s\n",method,kind,result->error,result->phase,result->request_bytes_sent,result->response_bytes_received,r->error);
    return code;
}
static unsigned connection(const struct http_resp *r) {char value[64];return http_header(r,"X-Connection-ID",value,sizeof value)?(unsigned)strtoul(value,NULL,10):0;}
static void h1_tests(void) {
    struct http_resp r;struct http_transport_result result;unsigned id,next;
    http_pool_clear();CHECK(request("plain","GET","one",&r,&result)==0&&r.status==200);id=connection(&r);CHECK(id>0&&!result.reused);http_resp_free(&r);
    CHECK(request("plain","GET","one",&r,&result)==0&&r.status==200);CHECK(result.reused&&connection(&r)==id);http_resp_free(&r);
    CHECK(request("interim","GET","one",&r,&result)==0&&r.status==200&&r.body_len>0);CHECK(connection(&r)==id);http_resp_free(&r);
    CHECK(request("head","HEAD","one",&r,&result)==0&&r.body_len==0);CHECK(connection(&r)==id);http_resp_free(&r);
    CHECK(request("plain","GET","one",&r,&result)==0&&connection(&r)==id);http_resp_free(&r);
    CHECK(request("chunked","GET","one",&r,&result)==0&&r.body_len>0);CHECK(connection(&r)==id);http_resp_free(&r);
    CHECK(request("plain","GET","one",&r,&result)==0&&connection(&r)==id);http_resp_free(&r);
    CHECK(request("plain","GET","two",&r,&result)==0);next=connection(&r);CHECK(next&&next!=id&&!result.reused);http_resp_free(&r);
    CHECK(request("close","GET","one",&r,&result)==0);http_resp_free(&r);
    CHECK(request("plain","GET","one",&r,&result)==0);next=connection(&r);CHECK(next&&next!=id&&!result.reused);http_resp_free(&r);id=next;
    CHECK(request("eof","GET","one",&r,&result)==0&&r.body_len>0);http_resp_free(&r);
    CHECK(request("plain","GET","one",&r,&result)==0&&!result.reused&&connection(&r)!=id);http_resp_free(&r);
    CHECK(request("stale","GET","one",&r,&result)==0);id=connection(&r);http_resp_free(&r);
    n_sleep_ms(10);CHECK(request("plain","GET","one",&r,&result)==0&&connection(&r)!=id);http_resp_free(&r);
    CHECK(request("stale","GET","post",&r,&result)==0);http_resp_free(&r);n_sleep_ms(10);
    CHECK(request("plain","POST","post",&r,&result)<0);CHECK(result.response_bytes_received==0&&result.error!=HTTP_ERR_NONE);http_resp_free(&r);
    http_pool_clear();
}
struct parallel {
    char url[256];struct http_resp response;struct http_transport_result result;int code;http_body_cb on_body;
};
static void parallel_request(void *arg) {
    struct parallel *p=arg;struct http_req rq={.url=p->url,.timeout_ms=10000,.on_body=p->on_body};
    p->code=http_request_h2_prior_knowledge_ex(&rq,&p->response,1048576,NULL,"h2-fixture",&p->result);
    if(p->code<0)printf("transport8 H2 error=%d %s\n",p->result.error,p->response.error);
}
static int h2_request(const char *path,const char *method,const void *body,size_t length,
    const struct http_stream_hooks *hooks,void *ctx,http_body_cb cb,struct http_resp *r,struct http_transport_result *result) {
    char url[256];address(url,sizeof url,h2_port,path);
    struct http_req rq={.url=url,.method=method,.body=body,.body_len=length,.timeout_ms=10000,.on_body=cb,.ctx=ctx};
    int code=http_request_h2_prior_knowledge_ex(&rq,r,1048576,hooks,"h2-fixture",result);
    if(code<0)printf("transport8 H2 %s error=%d phase=%d %s\n",path,result->error,result->phase,r->error);
    return code;
}
static int abort_body(void *ctx,const char *p,size_t n){(void)ctx;(void)p;(void)n;return -1;}
static int cancelled(void *ctx){(void)ctx;return 1;}
static int upload(void *ctx,size_t sent,size_t total){size_t *observed=ctx;if(sent>*observed)*observed=sent;return sent<=total?0:-1;}
static void late_cancel_test(void) {
    http_pool_clear();struct parallel p[2]={0};p[0].on_body=abort_body;
    address(p[0].url,sizeof p[0].url,h2_port,"/late-cancel-a");
    address(p[1].url,sizeof p[1].url,h2_port,"/late-cancel-b");
    int a=thread_create(parallel_request,p),b=thread_create(parallel_request,p+1);CHECK(a>=0&&b>=0);
    if(a>=0)thread_join(a);if(b>=0)thread_join(b);
    CHECK(p[0].code<0&&p[0].result.error==HTTP_ERR_CANCELLED);
    CHECK(p[1].code==0&&p[1].response.status==200&&p[1].response.body_len>0);
    CHECK(p[0].result.connection_id&&p[0].result.connection_id==p[1].result.connection_id);
    const char *headers=p[1].response.headers_full?p[1].response.headers_full:p[1].response.headers;
    CHECK(strstr(headers,"x-late-trailer: retained-after-cancel\r\n")!=NULL);
    printf("transport8 H2 late-cancel: connection=%llu/%llu stream=%u/%u cancelled=%d peer=%d status=%d\n",
        (unsigned long long)p[0].result.connection_id,(unsigned long long)p[1].result.connection_id,
        p[0].result.stream_id,p[1].result.stream_id,p[0].result.error,p[1].result.error,p[1].response.status);
    http_resp_free(&p[0].response);http_resp_free(&p[1].response);http_pool_clear();
}
static void header_budget_test(void) {
    for(unsigned encoded=0;encoded<2;encoded++) {
        http_pool_clear();struct parallel p[2]={0};
        address(p[0].url,sizeof p[0].url,h2_port,encoded?"/header-budget-a?huffman":"/header-budget-a?plain");
        address(p[1].url,sizeof p[1].url,h2_port,"/header-budget-b");
        int a=thread_create(parallel_request,p),b=thread_create(parallel_request,p+1);CHECK(a>=0&&b>=0);
        if(a>=0)thread_join(a);if(b>=0)thread_join(b);
        CHECK(p[0].code<0&&p[0].result.error==HTTP_ERR_IO);
        CHECK(p[1].code==0&&p[1].response.status==200&&p[1].response.body_len>0);
        CHECK(p[0].result.connection_id&&p[0].result.connection_id==p[1].result.connection_id);
        const char *headers=p[1].response.headers_full?p[1].response.headers_full:p[1].response.headers;
        CHECK(strstr(headers,"x-budget-synced: retained-after-budget\r\n")!=NULL);
        printf("transport8 H2 header-budget: huffman=%u connection=%llu/%llu refused=%d peer=%d status=%d\n",
            encoded,(unsigned long long)p[0].result.connection_id,(unsigned long long)p[1].result.connection_id,
            p[0].result.error,p[1].result.error,p[1].response.status);
        http_resp_free(&p[0].response);http_resp_free(&p[1].response);http_pool_clear();
    }
    struct http_resp r;struct http_transport_result result;
    CHECK(h2_request("/header-expanded","GET",NULL,0,NULL,NULL,NULL,&r,&result)==0&&r.status==200);
    const char *headers=r.headers_full?r.headers_full:r.headers,*value=strstr(headers,"x-expanded: ");
    bool correct=value&&strlen(value)>=12u+700u*1024+2;if(value){value+=12;for(size_t i=0;correct&&i<700u*1024;i++)if(value[i]!='~')correct=false;
        if(correct)correct=value[700u*1024]=='\r'&&value[700u*1024+1]=='\n';}
    CHECK(correct&&r.headers_len>700u*1024);http_resp_free(&r);http_pool_clear();
}
struct stalled_upload {
    unsigned char *body;
    uint64_t started,elapsed;
    size_t queued;
    volatile uint32_t running;
    int code;
    struct http_resp response;
    struct http_transport_result result;
};
/* Aligned x86 acquire/release, also understood by the guest's TCC. */
static unsigned load_running(const volatile uint32_t *p) {
    unsigned value;__asm__ volatile("movl %1, %0":"=r"(value):"m"(*p):"memory");return value;
}
static void publish_running(volatile uint32_t *p) {
    unsigned value=1;__asm__ volatile("movl %1, %0":"=m"(*p):"r"(value):"memory");
}
static int cancel_stalled(void *ctx){struct stalled_upload *s=ctx;return uptime_ms()-s->started>=300;}
static int stalled_progress(void *ctx,size_t sent,size_t total){struct stalled_upload *s=ctx;(void)total;s->queued=sent;return 0;}
static void stalled_request(void *ctx) {
    struct stalled_upload *s=ctx;char url[256];address(url,sizeof url,h2_port,"/stall-upload");
    struct http_req rq={.url=url,.method="POST",.body=s->body,.body_len=8u*1024*1024,.timeout_ms=10000,.ctx=s};
    struct http_stream_hooks hooks={.upload=stalled_progress,.cancelled=cancel_stalled};
    s->started=uptime_ms();publish_running(&s->running);
    s->code=http_request_h2_prior_knowledge_ex(&rq,&s->response,1048576,&hooks,"h2-stall",&s->result);
    s->elapsed=uptime_ms()-s->started;
}
static void backpressure_test(void) {
    http_pool_clear();struct stalled_upload s={0};s.body=malloc(8u*1024*1024);CHECK(s.body!=NULL);if(!s.body)return;
    memset(s.body,'b',8u*1024*1024);int tid=thread_create(stalled_request,&s);CHECK(tid>=0);if(tid<0){free(s.body);return;}
    while(!load_running(&s.running))n_sleep_ms(1);
    n_sleep_ms(50);char url[256];address(url,sizeof url,h2_port,"/trailers?while-upload-blocked");
    struct http_req rq={.url=url,.timeout_ms=200};struct http_resp r;struct http_transport_result result;
    uint64_t started=uptime_ms();int code=http_request_h2_prior_knowledge_ex(&rq,&r,1048576,NULL,"h2-stall",&result);
    uint64_t elapsed=uptime_ms()-started;thread_join(tid);
    printf("transport8 backpressure: other=%llu ms upload=%llu ms queued=%zu accepted=%zu other-error=%d upload-error=%d\n",
        (unsigned long long)elapsed,(unsigned long long)s.elapsed,s.queued,s.result.request_bytes_sent,result.error,s.result.error);
    CHECK(code<0&&result.error==HTTP_ERR_TIMEOUT&&elapsed>=200&&elapsed<=350);
    CHECK(s.code<0&&s.result.error==HTTP_ERR_CANCELLED&&s.elapsed>=300&&s.elapsed<=450);
    /* Exceed the default HTTP/2 window: this verifies the peer's expanded
       window reached the client before its intentionally blocked TCP receive. */
    CHECK(s.queued>65535&&s.result.request_bytes_sent>65535);
    http_resp_free(&r);http_resp_free(&s.response);free(s.body);http_pool_clear();
}
static void h2_tests(void) {
    struct parallel p[2]={0};
    address(p[0].url,sizeof p[0].url,h2_port,"/multiplex?a");address(p[1].url,sizeof p[1].url,h2_port,"/multiplex?b");
    int a=thread_create(parallel_request,p),b=thread_create(parallel_request,p+1);CHECK(a>=0&&b>=0);
    if(a>=0)thread_join(a);if(b>=0)thread_join(b);
    CHECK(p[0].code==0&&p[1].code==0&&p[0].response.status==200&&p[1].response.status==200);
    unsigned id=connection(&p[0].response);CHECK(id>0&&connection(&p[1].response)==id);
    CHECK(p[0].response.body&&p[1].response.body&&strcmp(p[0].response.body,p[1].response.body));
    http_resp_free(&p[0].response);http_resp_free(&p[1].response);
    struct http_resp r;struct http_transport_result result;
    CHECK(h2_request("/large","GET",NULL,0,NULL,NULL,NULL,&r,&result)==0&&r.body_len==192u*1024);
    bool correct=r.body_len==192u*1024;
    for(size_t i=0;correct&&i<r.body_len;i++)if((unsigned char)r.body[i]!=i%251)correct=false;
    CHECK(correct&&connection(&r)==id&&result.reused);http_resp_free(&r);
    CHECK(h2_request("/trailers","GET",NULL,0,NULL,NULL,NULL,&r,&result)==0&&r.body_len>0);CHECK(connection(&r)==id);http_resp_free(&r);
    unsigned char *body=malloc(150000);CHECK(body!=NULL);size_t uploaded=0;
    if(body){memset(body,'u',150000);struct http_stream_hooks hooks={.upload=upload};
        CHECK(h2_request("/upload","POST",body,150000,&hooks,&uploaded,NULL,&r,&result)==0);
        CHECK(r.body&&strstr(r.body,"150000")&&uploaded==150000);http_resp_free(&r);free(body);}
    CHECK(h2_request("/reset","GET",NULL,0,NULL,NULL,NULL,&r,&result)<0&&result.error==HTTP_ERR_RESET);http_resp_free(&r);
    CHECK(h2_request("/large","GET",NULL,0,NULL,NULL,abort_body,&r,&result)<0&&result.error==HTTP_ERR_CANCELLED);http_resp_free(&r);
    CHECK(h2_request("/trailers","GET",NULL,0,NULL,NULL,NULL,&r,&result)==0&&connection(&r)==id);http_resp_free(&r);
    CHECK(h2_request("/goaway","GET",NULL,0,NULL,NULL,NULL,&r,&result)==0);http_resp_free(&r);
    CHECK(h2_request("/trailers","GET",NULL,0,NULL,NULL,NULL,&r,&result)==0&&connection(&r)!=id);http_resp_free(&r);
    CHECK(h2_request("/late-goaway","GET",NULL,0,NULL,NULL,NULL,&r,&result)==0);id=connection(&r);http_resp_free(&r);
    n_sleep_ms(100);
    CHECK(h2_request("/trailers","GET",NULL,0,NULL,NULL,NULL,&r,&result)==0&&connection(&r)!=id);http_resp_free(&r);
    struct http_stream_hooks hooks={.cancelled=cancelled};
    CHECK(h2_request("/large","GET",NULL,0,&hooks,NULL,NULL,&r,&result)<0&&result.error==HTTP_ERR_CANCELLED&&result.request_bytes_sent==0);http_resp_free(&r);
    http_pool_clear();
}
static unsigned port_file(const char *path){FILE *f=fopen(path,"r");char b[64];unsigned n=0;if(f){if(fgets(b,sizeof b,f))n=(unsigned)strtoul(b,NULL,10);fclose(f);}return n;}
static int cancel_silent_handshake(void *ctx){return uptime_ms()-*(uint64_t*)ctx>=250;}
static int tls_cancel_test(const char *peer,unsigned port) {
    char error[160];http_pool_clear();uint64_t started=uptime_ms();
    net_stream *s=ns_open_http_cancelled_timed(peer,(uint16_t)port,true,30000,error,sizeof error,
        NULL,"silent-handshake",cancel_silent_handshake,&started);
    int cause=errno;uint64_t elapsed=uptime_ms()-started;
    printf("transport8 TLS-cancel: elapsed=%llu ms errno=%d %s\n",(unsigned long long)elapsed,cause,s?"unexpected stream":error);
    CHECK(s==NULL&&cause==ECANCELED);CHECK(elapsed>=250&&elapsed<=400);
    if(s)ns_abort(s);printf("transport8 TLS-cancel: %u checks, %u failed\n",checks,failed);return failed!=0;
}
struct tls_get {
    char url[512];
    struct http_resp response;
    struct http_transport_result result;
    struct tls_get *other;
    volatile uint32_t request_accepted,headers_seen,done;
    bool primary,upload_started,transport_seen,accepted_before_first_headers;
    size_t accepted_bytes;
    int code;
};
static int tls_get_uploaded(void *ctx,size_t sent,size_t total) {
    struct tls_get *g=ctx;(void)sent;(void)total;g->upload_started=true;return 0;
}
static size_t tls_accepted_bytes(const size_t *p) {
    /* H2 drain writers hold the connection lock and store an aligned size_t.
       An aligned x86 movq observes that word atomically; the compiler barrier
       orders this observation before publishing request_accepted with movl. */
    size_t value;__asm__ volatile("movq %1, %0":"=r"(value):"m"(*p):"memory");return value;
}
static int tls_get_cancelled(void *ctx) {
    struct tls_get *g=ctx;
    /* Exactly two external logical GETs. If the native stale retry resets
       transport state after this GET was handed off, cancel before another
       connection/request is opened; no extra retry reaches the provider. */
    size_t accepted=tls_accepted_bytes(&g->result.request_bytes_sent);
    if(accepted&&!g->transport_seen) {
        g->transport_seen=true;g->accepted_bytes=accepted;
        if(!g->primary)g->accepted_before_first_headers=
            load_running(&g->other->request_accepted)&&!load_running(&g->other->headers_seen);
        publish_running(&g->request_accepted);
    }
    return g->upload_started&&g->transport_seen&&accepted==0;
}
static int tls_get_headers(void *ctx,const struct http_resp *response) {
    struct tls_get *g=ctx;(void)response;publish_running(&g->headers_seen);
    return 0;
}
static void tls_get_request(void *ctx) {
    struct tls_get *g=ctx;struct http_req rq={.url=g->url,.timeout_ms=30000,.ctx=g};
    struct http_stream_hooks hooks={.headers=tls_get_headers,.upload=tls_get_uploaded,.cancelled=tls_get_cancelled};
    g->code=http_request_stream_ex(&rq,&g->response,65536,&hooks,NULL,"tls-h2-two-get",&g->result);
    if(g->code<0)printf("transport8 TLS-H2 error=%d %s\n",g->result.error,g->response.error);
    publish_running(&g->done);
}
static void tls_h2_get_tests(const char *peer) {
    http_pool_clear();struct tls_get g[2]={0};g[0].other=&g[1];g[1].other=&g[0];g[0].primary=true;
    snprintf(g[0].url,sizeof g[0].url,"https://%s/httpbin/delay/1?transport8=one",peer);
    snprintf(g[1].url,sizeof g[1].url,"https://%s/httpbin/delay/1?transport8=two",peer);
    int first=thread_create(tls_get_request,&g[0]);CHECK(first>=0);if(first<0)return;
    uint64_t started=uptime_ms();bool initial_wait_timed_out=false;
    while(!load_running(&g[0].request_accepted)&&!load_running(&g[0].done)) {
        if(uptime_ms()-started>=35000){initial_wait_timed_out=true;break;}
        n_sleep_ms(1);
    }
    CHECK(load_running(&g[0].request_accepted)&&!load_running(&g[0].headers_seen));
    int second=thread_create(tls_get_request,&g[1]);CHECK(second>=0);
    if(second<0)publish_running(&g[1].done);
    if(second>=0)thread_join(second);thread_join(first);
    printf("transport8 TLS-H2: connection=%llu/%llu stream=%u/%u h2=%u/%u reused=%u/%u status=%d/%d\n",
        (unsigned long long)g[0].result.connection_id,(unsigned long long)g[1].result.connection_id,
        g[0].result.stream_id,g[1].result.stream_id,g[0].result.http2,g[1].result.http2,
        g[0].result.reused,g[1].result.reused,g[0].response.status,g[1].response.status);
    printf("transport8 TLS-H2 in-flight: accepted=%zu/%zu second-before-first-headers=%u\n",
        g[0].accepted_bytes,g[1].accepted_bytes,g[1].accepted_before_first_headers);
    CHECK(g[0].code==0&&g[1].code==0&&g[0].response.status==200&&g[1].response.status==200);
    CHECK(g[0].response.body_len>0&&g[1].response.body_len>0);
    CHECK(g[0].result.http2&&g[1].result.http2);
    CHECK(g[0].result.connection_id&&g[0].result.connection_id==g[1].result.connection_id);
    CHECK(g[0].result.stream_id&&g[1].result.stream_id&&g[0].result.stream_id!=g[1].result.stream_id);
    CHECK(g[0].accepted_bytes&&g[1].accepted_bytes&&g[1].accepted_before_first_headers);
    CHECK(g[1].result.reused&&!initial_wait_timed_out);
    http_resp_free(&g[0].response);http_resp_free(&g[1].response);http_pool_clear();
}
static int tls_tests(const char *host) {
    char error[160];http_pool_clear();
    net_stream *s=ns_open_http_timed(host,443,true,30000,error,sizeof error,NULL,"tls-regression");
    if(!s){printf("FAIL transport8 TLS first: %s\n",error);return 1;}
    CHECK(!strcmp(ns_protocol(s),"h2"));CHECK(!ns_session_resumed(s));ns_close(s);
    s=ns_open_http_timed(host,443,true,30000,error,sizeof error,NULL,"tls-regression");
    if(!s){printf("FAIL transport8 TLS second: %s\n",error);return 1;}
    CHECK(!strcmp(ns_protocol(s),"h2"));CHECK(ns_session_resumed(s));ns_close(s);
    s=ns_open_http_timed(host,443,true,30000,error,sizeof error,NULL,"other-partition");
    if(!s){printf("FAIL transport8 TLS partition: %s\n",error);return 1;}
    CHECK(!ns_session_resumed(s));ns_close(s);
    tls_h2_get_tests(host);
    printf("transport8 TLS: %u checks, %u failed\n",checks,failed);return failed!=0;
}
int main(int argc,char **argv) {
    if(argc>1&&!strcmp(argv[1],"--header-budget")) {
        if(argc>2)snprintf(host,sizeof host,"%s",argv[2]);
        h2_port=argc>3?(unsigned)strtoul(argv[3],NULL,10):port_file("/data/tests/h2port");
        if(!h2_port){printf("FAIL transport8 header-budget port unavailable\n");return 1;}
        header_budget_test();printf("transport8 header-budget: %u checks, %u failed\n",checks,failed);return failed!=0;
    }
    if(argc>1&&!strcmp(argv[1],"--late-cancel")) {
        if(argc>2)snprintf(host,sizeof host,"%s",argv[2]);
        h2_port=argc>3?(unsigned)strtoul(argv[3],NULL,10):port_file("/data/tests/h2port");
        if(!h2_port){printf("FAIL transport8 late-cancel port unavailable\n");return 1;}
        late_cancel_test();printf("transport8 late-cancel: %u checks, %u failed\n",checks,failed);return failed!=0;
    }
#ifdef _WIN32
    if(argc>2&&!strcmp(argv[1],"--tls-h2-model")) {
        printf("transport8 HTTP/2 API model: no TLS authentication or record assertions\n");
        tls_h2_get_tests(argv[2]);printf("transport8 TLS-H2-model: %u checks, %u failed\n",checks,failed);return failed!=0;
    }
#endif
    if(argc>1&&!strcmp(argv[1],"--tls"))return tls_tests(argc>2?argv[2]:"nghttp2.org");
    if(argc>1&&!strcmp(argv[1],"--tls-cancel")) {
        unsigned port=argc>3?(unsigned)strtoul(argv[3],NULL,10):port_file("/data/tests/tlssilentport");
        if(!port){printf("FAIL transport8 TLS-cancel port unavailable\n");return 1;}
        return tls_cancel_test(argc>2?argv[2]:"10.0.2.2",port);
    }
    if(argc>3){snprintf(host,sizeof host,"%s",argv[1]);http_port=(unsigned)strtoul(argv[2],NULL,10);h2_port=(unsigned)strtoul(argv[3],NULL,10);}
    else {http_port=port_file("/data/tests/webports");h2_port=port_file("/data/tests/h2port");}
    if(!http_port||!h2_port){printf("FAIL transport8 fixture ports unavailable\n");return 1;}
    h1_tests();h2_tests();late_cancel_test();header_budget_test();backpressure_test();printf("transport8: %u checks, %u failed\n",checks,failed);return failed!=0;
}
