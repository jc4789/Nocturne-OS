/* 次22の新長IPC/redirectだけ。実HTTP/COOKIE/URLsource＋匿名transport fixture。 */
#include <stdio.h>
#include <stdint.h>
#include <limits.h>
#include <time.h>
#include <errno.h>
#include <ctype.h>
#include "http.h"
#include "web.h"
#include "webnet_wire.h"
#define MIN(a,b) ((a)<(b)?(a):(b))
#undef malloc
#undef calloc
#undef realloc
#undef free
#undef strdup
static size_t live,fail_size;
static unsigned fail_next,fail_hits;
int errno; /* Nocturne's errno object; the host CRT uses a different accessor. */
void *count_malloc(size_t n){if((fail_next&&!--fail_next)||(fail_size&&n==fail_size)){fail_size=0;fail_hits++;return NULL;}void *p=malloc(n);if(p)live++;return p;}
void *count_calloc(size_t n,size_t s){if(n&&s>SIZE_MAX/n)return NULL;void *p=count_malloc(n*s);if(p)memset(p,0,n*s);return p;}
void *count_realloc(void *p,size_t n){if(!p)return count_malloc(n);if((fail_next&&!--fail_next)||(fail_size&&n==fail_size)){fail_size=0;fail_hits++;return NULL;}return realloc(p,n);}
void count_free(void *p){if(p){live--;free(p);}}
char *count_strdup(const char *s){size_t n=strlen(s);if(n==SIZE_MAX)return NULL;char *p=count_malloc(n+1);if(p)memcpy(p,s,n+1);return p;}
uint64_t uptime_ms(void){return 100;}
int close(int fd){(void)fd;return 0;}
int kill(int pid){(void)pid;return 0;}
int waitpid(int pid,int *status,int flags){(void)status;(void)flags;return pid;}
static const unsigned char *incoming;static size_t incoming_bytes;
static ssize_t fixture_read(int fd,void *out,size_t n){(void)fd;if(n>incoming_bytes)n=incoming_bytes;memcpy(out,incoming,n);incoming+=n;incoming_bytes-=n;return (ssize_t)n;}
static ssize_t fixture_write(int fd,const void *p,size_t n){(void)fd;(void)p;return (ssize_t)n;}
#define read fixture_read
#define write fixture_write
#define malloc count_malloc
#define calloc count_calloc
#define realloc count_realloc
#define free count_free
#define strdup count_strdup
#include "../build/goal-20261009/webnet-url22-new-once/webfetch-tested.inc"
#include "../build/goal-20261009/webnet-url22-new-once/webnet-tested.inc"
#undef malloc
#undef calloc
#undef realloc
#undef free
#undef strdup
#undef read
#undef write
struct net_stream {int unused;};
net_stream *ns_open(const char *h,uint16_t p,bool tls,int t,char *e,size_t n){(void)h;(void)p;(void)tls;(void)t;(void)e;(void)n;return NULL;}
long ns_read(net_stream *s,void *p,size_t n){(void)s;(void)p;(void)n;return -1;}
long ns_write(net_stream *s,const void *p,size_t n){(void)s;(void)p;(void)n;return -1;}
const char *ns_error(net_stream *s){(void)s;return "unused protocol stream";}
void ns_close(net_stream *s){(void)s;}
void ns_set_timeout(net_stream *s,int t){(void)s;(void)t;}
int http_inflate(char *o,int c,const char *i,int n,int *used){(void)o;(void)c;(void)i;(void)n;(void)used;return -1;}
static char *initial,*creator,*location,*expected;
static unsigned requests,callbacks,checks,failed;
static int mode;
#define CHECK(name,ok) do{checks++;if(!(ok)){failed++;printf("FAIL %s\n",name);}}while(0)
int http_request_limited(const struct http_req *q,struct http_resp *r,size_t limit){
    (void)limit;memset(r,0,sizeof *r);requests++;
    if(requests==1){
        CHECK("new-long-initial-target-not-truncated",!strcmp(q->url,initial));
        r->status=302;size_t n=strlen(location)+12;r->headers_full=count_malloc(n+1);if(!r->headers_full)return -1;
        int wrote=snprintf(r->headers_full,n+1,"Location: %s\r\n",location);r->headers_len=(size_t)wrote;
        if(mode==2)fail_size=strlen(location)+1;
    }else{
        CHECK("new-long-redirect-target-not-truncated",!strcmp(q->url,expected));
        r->status=200;r->headers_full=count_strdup("Content-Type: text/plain\r\n");r->headers_len=strlen(r->headers_full);
        const char *cookie="Set-Cookie: fixture=second; Path=/; Secure; SameSite=Lax";
        if(q->on_header&&q->on_header(q->ctx,cookie,strlen(cookie))<0)return -1;
        if(q->on_body&&q->on_body(q->ctx,"ok",2)<0)return -1;
    }
    return 0;
}
int http_request(const struct http_req *q,struct http_resp *r){return http_request_limited(q,r,UINT32_MAX);}
static void callback(webnet *n,uint64_t id,uint64_t generation,const struct webnet_response *r,void *opaque){
    (void)n;(void)opaque;callbacks++;CHECK("new-long-final-callback-full-length-and-identity",id&&generation==7&&r->status==200&&!strcmp(r->final_url,expected)&&r->body_len==2&&!memcmp(r->body,"ok",2)&&!*r->error);
}
static struct job *worker_job(const struct request *r){
    struct job *j=calloc(1,sizeof *j);memcpy(&j->wire,r->tx,sizeof j->wire);
    incoming=r->tx+sizeof j->wire;incoming_bytes=r->tx_len-sizeof j->wire;
    j->url=read_string(j->wire.url_len,false);j->document=read_string(j->wire.origin_len,false);j->method=read_string(j->wire.method_len,false);j->headers=read_string(j->wire.headers_len,false);j->request_body=read_string(j->wire.body_len,true);
    char *snapshot=read_string(j->wire.cookie_len,true);j->cookies=webcookie_create();
    if(j->wire.cookie_len&&!webcookie_import(j->cookies,snapshot,j->wire.cookie_len,time(NULL)))CHECK("new-long-worker-cookie-snapshot-fixture",0);
    count_free(snapshot);return j;
}
static void free_job(struct job *j){count_free(j->url);count_free(j->document);count_free(j->method);count_free(j->headers);count_free(j->request_body);count_free(j->origin);count_free(j->target_origin);count_free(j->final_url);count_free(j->non_simple);count_free(j->outgoing.p);count_free(j->response_headers);count_free(j->body);count_free(j->cookie_events);webcookie_free(j->cookies);free(j);requests=0;mode=0;fail_size=0;}
static void pack_response(struct request *r,const struct job *j){
    r->head=(struct webnet_wire_response){WEBNET_MAGIC,(uint32_t)j->status,r->id,r->generation,(uint32_t)strlen(j->final_url),(uint32_t)j->response_headers_len,(uint32_t)j->body_len,(uint32_t)strlen(j->error),(uint32_t)j->cookie_len};
    size_t n;webnet_wire_payload_size(&r->head,&n);r->rx=count_malloc(n+1);r->rx_len=n;size_t at=0;
    const void *p[5]={j->final_url,j->response_headers,j->body,j->error,j->cookie_events};size_t sizes[5]={r->head.url_len,r->head.headers_len,r->head.body_len,r->head.error_len,r->head.cookie_len};
    for(int i=0;i<5;i++){if(sizes[i])memcpy(r->rx+at,p[i],sizes[i]);at+=sizes[i];}
}
static char *long_url(const char *prefix,size_t extra){size_t n=strlen(prefix);char *p=malloc(n+extra+1);memcpy(p,prefix,n);memset(p+n,'x',extra);p[n+extra]=0;return p;}
int main(void){
    initial=long_url("https://one.test/dir/start?p=",20001);creator=long_url("https://one.test/page?p=",25003);location=long_url("../target?p=",300011);expected=long_url("https://one.test/target?p=",300011);
    webnet n={0};n.cookies=webcookie_create();for(int i=0;i<WORKERS;i++)n.slots[i].in=n.slots[i].out=-1;
    struct webcookie_context seed={"https://one.test/","https://one.test/","GET",false,true,false};const char *cookie="fixture=first; Path=/; Secure; SameSite=Lax";webcookie_set(n.cookies,&seed,cookie,strlen(cookie),time(NULL));
    struct webnet_request q={.kind=WEBNET_FETCH,.generation=7,.url=initial,.origin=creator,.credentials=WEBNET_CREDENTIALS_SAME_ORIGIN,.same_origin=true};
    uint64_t id=webnet_submit(&n,&q,callback,NULL);CHECK("new-long-native-submit-copies-url-and-origin",id&&n.queue&&n.count==1);if(!id)return 2;
    struct request *r=n.queue;struct webnet_wire_request h;memcpy(&h,r->tx,sizeof h);
    CHECK("new-long-opt-in-wire-lengths-and-unchanged-layout",h.url_len==strlen(initial)&&h.origin_len==strlen(creator)&&(h.user_navigation&WEBNET_WIRE_DYNAMIC_URL)&&sizeof h==64&&sizeof r->head==48);
    struct job *j=worker_job(r);CHECK("new-long-worker-wire-fields-complete",j->url&&j->document&&!strcmp(j->url,initial)&&!strcmp(j->document,creator)&&!incoming_bytes);
    CHECK("new-long-redirect-real-worker-flow",run_http(j)&&requests==2&&strlen(j->final_url)>300000&&!strcmp(j->final_url,expected)&&!j->cors_tainted);
    CHECK("new-long-cookie-event-no256KiB-drop",j->cookie_len>WEBNET_COOKIE_EVENTS_MAX&&j->cookie_len<=UINT32_MAX);
    pack_response(r,j);CHECK("new-long-native-unpack-callback-storage",unpack(r));
    n.queue=NULL;n.slots[0].rq=r;complete(&n,&n.slots[0]);CHECK("new-long-completion-count-and-byte-accounting",callbacks==1&&!n.count&&!n.queued_bytes);
    struct webcookie_context c={expected,creator,"GET",false,true,false};char cookie_out[64];long got=webcookie_get(n.cookies,&c,cookie_out,sizeof cookie_out,time(NULL));CHECK("new-long-response-cookie-applied-not-lost",got>0&&strstr(cookie_out,"fixture=second"));free_job(j);
    char *deep=long_url("https://one.test/",50003);c.url=deep;CHECK("new-long-path-cookie-root-match-not2048-reject",webcookie_get(n.cookies,&c,NULL,0,time(NULL))>0);free(deep);
    struct job legacy={.wire={.kind=WEBNET_FETCH}};char *legacy_url=NULL,*legacy_origin=NULL;
    CHECK("new-worker-oldclient-long-final-fails-closed",image_http_url(&legacy,expected,&legacy_url,&legacy_origin)==HTTP_URL_INVALID&&!legacy_url&&!legacy_origin&&webnet_wire_url_limit(0)==WEBNET_URL_MAX-1u);
    char *resolved=(char *)1;fail_next=1;CHECK("new-long-owned-resolver-oom-distinct-null",web_resolve_url_owned(initial,location,&resolved)==-1&&!resolved&&fail_hits);fail_next=0;
    id=webnet_submit(&n,&q,callback,NULL);r=n.queue;j=worker_job(r);mode=2;CHECK("new-long-redirect-allocation-failure-not-dispatch",!run_http(j)&&requests==1&&strstr(j->error,"Out of memory preparing redirect"));free_job(j);webnet_cancel_generation(&n,7);CHECK("new-long-generation-cancel-no-stale-callback",callbacks==1&&!n.queue&&!n.count&&!n.queued_bytes);
    free(location);location=long_url("https://other.test/next?p=",20007);id=webnet_submit(&n,&q,callback,NULL);r=n.queue;j=worker_job(r);CHECK("new-long-cross-origin-redirect-still-denied-before-target",!run_http(j)&&requests==1&&strstr(j->error,"Cross-origin"));free_job(j);webnet_cancel_generation(&n,7);
    size_t snapshot=live;fail_next=1;CHECK("new-long-native-submit-real-oom-preserves-queue",!webnet_submit(&n,&q,callback,NULL)&&!n.queue&&!n.count&&live==snapshot);fail_next=0;
    webcookie_free(n.cookies);free(initial);free(creator);free(location);free(expected);CHECK("new-owners-completely-released",!live);
    printf("webnet-url22-new: %u checks, %u failed\n",checks,failed);return failed?1:0;
}
