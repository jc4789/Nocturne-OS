/* The production cache, response callbacks and replay decision are compiled
   directly. Synthetic HTTP fields do not stand in for QEMU browser acceptance. */
#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>
#define ssize_t nocturne_test_ssize_t
#define main static webfetch_service_entry
#define time cache_test_time
#define read cache_test_read
#define close cache_test_close
#include "../user/apps/webfetch.c"
#undef main
#undef time
#undef read
#undef close
static uint64_t clock_ms=1000;
static unsigned checks,failed;
static bool peer_hup,cancel_during_preflight;
static const char *expected_partition;
static unsigned preflight_calls;
static unsigned setup_calls,read_calls,close_calls;
static int setup_fds[4];
static bool fail_setup;
static bool order_transport,order_deny,order_cancel;
static unsigned order_options,order_gets;
#define CHECK(c) do{checks++;if(!(c)){failed++;printf("FAIL cache:%d %s\n",__LINE__,#c);}}while(0)
uint64_t uptime_ms(void){return clock_ms;}
time_t cache_test_time(time_t *out){time_t now=784111777;if(out)*out=now;return now;}
n_result wait_on_address(volatile uint32_t *p,uint32_t expected,unsigned timeout){(void)p;(void)expected;(void)timeout;return 0;}
n_result wake_address(volatile uint32_t *p,unsigned count){(void)p;(void)count;return 0;}
long cache_test_read(int fd,void *data,size_t bytes){(void)fd;(void)data;(void)bytes;read_calls++;return 0;}
int cache_test_close(int fd){(void)fd;CHECK(setup_calls==2&&setup_fds[1]==-1);close_calls++;return 0;}
n_result net_set_setup_cancel_channel(n_handle fd){
    CHECK(setup_calls<4);setup_fds[setup_calls++]=fd;
    return fd>=0&&fail_setup?-N_ERR_INVALID:0;
}
int poll(struct n_pollfd *p,int count,int timeout){(void)timeout;for(int i=0;i<count;i++)p[i].revents=peer_hup?N_POLLHUP:0;return peer_hup?count:0;}
const char *http_response_headers(const struct http_resp *r){return r->headers_full?r->headers_full:r->headers;}
const char *http_response_status_text(const struct http_resp *r){return r->status_text_full?r->status_text_full:r->status_text;}
/* Fixed-fixture URL transport double. URL grammar/canonicalization has its own
   native HTTP parser tests; this suite verifies partition ownership and keys. */
enum http_url_result http_origin_owned(const char *raw,char **origin){
    *origin=NULL;size_t prefix=!strncmp(raw,"https://",8)?8:!strncmp(raw,"http://",7)?7:0;
    if(!prefix)return HTTP_URL_OPAQUE;
    size_t length=prefix+strcspn(raw+prefix,"/?#");if(length==prefix)return HTTP_URL_INVALID;
    *origin=malloc(length+1);if(!*origin)return HTTP_URL_OOM;
    memcpy(*origin,raw,length);(*origin)[length]=0;return HTTP_URL_TUPLE;
}
enum http_url_result http_canonical_owned_n(const char *raw,size_t length,char **canonical,char **origin){
    if(canonical)*canonical=NULL;*origin=NULL;
    if(!raw||strlen(raw)!=length)return HTTP_URL_INVALID;
    enum http_url_result result=http_origin_owned(raw,origin);
    if(result==HTTP_URL_TUPLE&&canonical){*canonical=strdup(raw);if(!*canonical){free(*origin);*origin=NULL;return HTTP_URL_OOM;}}
    return result;
}
int web_resolve_url_owned(const char *base,const char *relative,char **out){
    (void)base;*out=strstr(relative,"://")?strdup(relative):NULL;return *out?1:0;
}
long webcookie_get(webcookie_jar *jar,const struct webcookie_context *ctx,char *out,size_t cap,int64_t now){
    (void)jar;(void)ctx;(void)cap;(void)now;if(out)*out=0;return 0;
}
void http_resp_free(struct http_resp *r){free(r->headers_full);free(r->status_text_full);free(r->body);memset(r,0,sizeof *r);}
int http_request_stream_ex(const struct http_req *q,struct http_resp *r,size_t limit,
                          const struct http_stream_hooks *hooks,struct http_transport_timing *timing,
                          const char *partition,struct http_transport_result *result){
    (void)timing;memset(r,0,sizeof *r);
    if(order_transport){
        struct job *j=q->ctx;if(result)memset(result,0,sizeof *result);
        CHECK(partition&&*partition&&hooks&&hooks->cancelled==request_cancelled);
        if(!strcmp(q->method,"OPTIONS")){
            order_options++;CHECK(q->on_body==discard_cb&&!q->body&&!strstr(q->headers,"If-None-Match:"));
            if(order_cancel){peer_hup=true;CHECK(hooks->cancelled(j));strcpy(r->error,"Request cancelled during preflight");return -1;}
            r->status=204;snprintf(r->headers,sizeof r->headers,
                "Access-Control-Allow-Origin: %s\r\nAccess-Control-Allow-Methods: GET\r\nAccess-Control-Allow-Headers: %s\r\n",
                request_origin(j),order_deny?"x-other":"x-test");return 0;
        }
        order_gets++;CHECK(!strcmp(q->method,"GET"));
        r->status=strstr(q->url,"/redirect")?302:field_exists(q->headers,"If-None-Match")?304:200;
        snprintf(r->headers,sizeof r->headers,
            "Cache-Control: max-age=600\r\nETag: \"order-body\"\r\nAccess-Control-Allow-Origin: %s\r\n%s",
            request_origin(j),r->status==302?"Location: https://resource.test/cache\r\n":"");
        strcpy(r->status_text,"OK");int start=hooks->headers?hooks->headers(j,r):0;
        if(start<0)return -1;
        if(!start&&r->status==200)return q->on_body(j,"cached-body",11);
        return 0;
    }
    preflight_calls++;
    CHECK(!strcmp(q->method,"OPTIONS")&&!q->body&&!q->body_len&&q->on_body==discard_cb);
    CHECK(partition==expected_partition&&partition&&*partition&&limit==HTTP_RESPONSE_BODY_MAX);
    CHECK(hooks&&hooks->cancelled==request_cancelled&&!hooks->headers&&!hooks->upload);
    CHECK(!strstr(q->headers,"Cookie:")&&!strstr(q->headers,"Authorization:"));
    CHECK(!hooks->cancelled(q->ctx));
    if(cancel_during_preflight){
        peer_hup=true;CHECK(hooks->cancelled(q->ctx));CHECK(q->on_body(q->ctx,"discard",7)<0);
        strcpy(r->error,"Request cancelled during preflight");return -1;
    }
    CHECK(q->on_body(q->ctx,"discard",7)==0);
    r->status=204;strcpy(r->headers,"Access-Control-Allow-Origin: https://document.test\r\nAccess-Control-Allow-Methods: PATCH\r\nAccess-Control-Allow-Headers: x-test\r\n");return 0;
}
bool webcookie_same_site(const webcookie_jar *jar,const char *a,const char *b){(void)jar;(void)a;(void)b;return true;}
int webcookie_set(webcookie_jar *jar,const struct webcookie_context *c,const char *value,size_t n,int64_t now){(void)jar;(void)c;(void)value;(void)n;(void)now;return 1;}
static void clear_cache(void){while(cache_entries){struct cache_entry *e=cache_entries;cache_entries=e->next;cache_free(e);}cache_bytes=0;}
static struct job test_job(void){
    return (struct job){.wire={.kind=WEBNET_FETCH,.credentials=WEBNET_CREDENTIALS_OMIT,.deadline=999999},
        .origin="https://document.test",.target_origin="https://document.test",.final_url="https://document.test/resource",
        .document="https://document.test/page",.headers="",.hop_method="GET",.output=1};
}
static void free_response(struct http_resp *r){free(r->headers_full);free(r->status_text_full);memset(r,0,sizeof *r);}
static void close_job(struct job *j){free(j->partition);free(j->outgoing.p);free(j->cache_body.p);free(j->body);free(j->response_headers);free(j->top_level_url);free(j->top_origin);cache_free(j->cached);}
static char *credential_partition(const char *headers){
    struct job j=test_job();CHECK(header_text(&j,&j.outgoing,headers));CHECK(cache_partition(&j));
    char *partition=j.partition;j.partition=NULL;close_job(&j);return partition;
}
static void response(struct http_resp *r,const char *headers){memset(r,0,sizeof *r);r->status=200;snprintf(r->headers,sizeof r->headers,"%s",headers);strcpy(r->status_text,"OK");}
static void store(struct job *j,const char *headers,const char *body){
    struct http_resp r;response(&r,headers);CHECK(response_start(j,&r)==0);
    CHECK(body_cb(j,body,strlen(body))==0);cache_store(j,&r);
}
static struct job http_order_job(const char *headers){
    return (struct job){.wire={.kind=WEBNET_FETCH,.credentials=WEBNET_CREDENTIALS_OMIT,.deadline=999999},
        .url=strdup("https://resource.test/cache"),.document=strdup("https://document.test/page"),
        .method=strdup("GET"),.headers=strdup(headers),.request_body=strdup(""),
        .top_level_url=strdup("https://top.test/page"),.output=1};
}
static void close_http_job(struct job *j){
    free(j->url);free(j->document);free(j->method);free(j->headers);free(j->request_body);
    free(j->origin);free(j->target_origin);free(j->final_url);free(j->non_simple);close_job(j);
}
static void cache_preflight_order_cases(void){
    order_transport=true;struct job seed=http_order_job("");
    CHECK(run_http(&seed)&&seed.status==200&&seed.body_len==11&&order_gets==1&&!order_options);
    close_http_job(&seed);
    struct job allow=http_order_job("X-Test: allowed\r\n");
    CHECK(run_http(&allow)&&allow.status==200&&allow.body_len==11&&!strcmp(allow.body,"cached-body"));
    CHECK(order_options==1&&order_gets==1);close_http_job(&allow);
    unsigned before_options=order_options,before_gets=order_gets;
    order_deny=true;struct job deny=http_order_job("X-Test: denied\r\n");
    CHECK(!run_http(&deny)&&!deny.status&&!deny.body_len&&!deny.response_headers&&!deny.stream_headers_sent);
    CHECK(order_options==before_options+1&&order_gets==before_gets);close_http_job(&deny);order_deny=false;
    before_options=order_options;struct job forced=http_order_job("");forced.wire.user_navigation=WEBNET_WIRE_FORCE_PREFLIGHT;
    CHECK(run_http(&forced)&&forced.status==200&&order_options==before_options+1&&order_gets==before_gets);close_http_job(&forced);
    before_options=order_options;order_cancel=true;struct job cancelled=http_order_job("X-Test: cancel\r\n");
    cancelled.wire.user_navigation=WEBNET_WIRE_SERVICE;
    CHECK(!run_http(&cancelled)&&!cancelled.status&&!cancelled.body_len&&!cancelled.stream_headers_sent);
    CHECK(order_options==before_options+1&&order_gets==before_gets);peer_hup=false;order_cancel=false;close_http_job(&cancelled);
    before_options=order_options;struct job validate=http_order_job("X-Test: validate\r\n");
    validate.wire.user_navigation=WEBNET_CACHE_NO_CACHE<<WEBNET_WIRE_CACHE_SHIFT;
    CHECK(run_http(&validate)&&validate.status==200&&validate.body_len==11&&!strcmp(validate.body,"cached-body"));
    CHECK(order_options==before_options+1&&order_gets==before_gets+1);close_http_job(&validate);
    before_options=order_options;before_gets=order_gets;struct job redirect=http_order_job("X-Test: redirect\r\n");
    free(redirect.url);redirect.url=strdup("https://resource.test/redirect");
    CHECK(run_http(&redirect)&&redirect.status==200&&redirect.redirected&&redirect.body_len==11);
    CHECK(order_options==before_options+2&&order_gets==before_gets+1);close_http_job(&redirect);
    before_options=order_options;before_gets=order_gets;struct job cache_only=http_order_job("X-Test: only-cached\r\n");
    cache_only.wire.user_navigation=WEBNET_WIRE_SAME_ORIGIN|(WEBNET_CACHE_ONLY_IF_CACHED<<WEBNET_WIRE_CACHE_SHIFT);
    CHECK(!run_http(&cache_only)&&!cache_only.status&&!cache_only.body_len&&order_options==before_options&&order_gets==before_gets);
    close_http_job(&cache_only);
    struct job own_seed=http_order_job("");free(own_seed.document);own_seed.document=strdup("https://resource.test/page");
    CHECK(run_http(&own_seed)&&own_seed.status==200);close_http_job(&own_seed);
    before_options=order_options;before_gets=order_gets;struct job own_cached=http_order_job("X-Test: own\r\n");
    free(own_cached.document);own_cached.document=strdup("https://resource.test/page");
    own_cached.wire.user_navigation=WEBNET_WIRE_SAME_ORIGIN|(WEBNET_CACHE_ONLY_IF_CACHED<<WEBNET_WIRE_CACHE_SHIFT);
    CHECK(run_http(&own_cached)&&own_cached.status==200&&own_cached.body_len==11&&order_options==before_options&&order_gets==before_gets);
    close_http_job(&own_cached);clear_cache();order_transport=false;
}
static void top_partition_cases(void){
    struct job a=test_job(),b=test_job(),again_a=test_job(),again_b=test_job();
    struct job *jobs[]={&a,&b,&again_a,&again_b};
    for(unsigned i=0;i<4;i++){
        jobs[i]->origin="https://shared-child.test";jobs[i]->document="https://shared-child.test/frame";
        jobs[i]->top_level_url=strdup(i==0||i==2?"https://top-a.test/page":"https://top-b.test/page");
        CHECK(header_text(jobs[i],&jobs[i]->outgoing,"Cookie: session=same\r\nAuthorization: same\r\n"));
        CHECK(cache_partition(jobs[i]));
    }
    CHECK(strcmp(a.partition,b.partition)&&!strcmp(a.partition,again_a.partition)&&!strcmp(b.partition,again_b.partition));
    CHECK(cookie_site_url(&a)==a.top_level_url&&cookie_site_url(&b)==b.top_level_url);
    store(&a,"Cache-Control: max-age=600\r\n","top-a-body");
    CHECK(!cache_find(&b));
    store(&b,"Cache-Control: max-age=600\r\n","top-b-body");
    struct cache_entry *hit_a=cache_find(&again_a),*hit_b=cache_find(&again_b);
    CHECK(hit_a&&hit_b&&cache_usable(&again_a,hit_a)&&cache_usable(&again_b,hit_b));
    CHECK(hit_a&&!strcmp(hit_a->body,"top-a-body")&&hit_b&&!strcmp(hit_b->body,"top-b-body"));
    cache_free(hit_a);cache_free(hit_b);
    for(unsigned i=0;i<4;i++)close_job(jobs[i]);clear_cache();
    for(unsigned true_top=0;true_top<2;true_top++){
        struct job frame_a=test_job(),frame_b=test_job();
        frame_a.wire.kind=frame_b.wire.kind=WEBNET_NAVIGATION;
        frame_a.wire.user_navigation=frame_b.wire.user_navigation=true_top?WEBNET_WIRE_USER_NAVIGATION:0;
        frame_a.top_level_url=strdup("https://top-a.test/");frame_b.top_level_url=strdup("https://top-b.test/");
        CHECK(header_text(&frame_a,&frame_a.outgoing,"")&&header_text(&frame_b,&frame_b.outgoing,""));
        CHECK(cache_partition(&frame_a)&&cache_partition(&frame_b));
        CHECK(top_level_navigation(&frame_a)==(true_top!=0)&&
              ((!strcmp(frame_a.partition,frame_b.partition))==(true_top!=0)));
        store(&frame_a,"Cache-Control: max-age=600\r\n","frame-a-body");
        struct cache_entry *from_b=cache_find(&frame_b);
        CHECK(true_top?(from_b&&!strcmp(from_b->body,"frame-a-body")):from_b==NULL);cache_free(from_b);
        if(!true_top)store(&frame_b,"Cache-Control: max-age=600\r\n","frame-b-body");
        struct cache_entry *from_a=cache_find(&frame_a);CHECK(from_a&&!strcmp(from_a->body,"frame-a-body"));cache_free(from_a);
        close_job(&frame_a);close_job(&frame_b);clear_cache();
    }
    struct job nav=test_job();nav.wire.kind=WEBNET_NAVIGATION;nav.wire.user_navigation=WEBNET_WIRE_USER_NAVIGATION;
    nav.top_level_url=strdup("https://source-top.test/");
    nav.target_origin="https://hop-one.test";CHECK(header_text(&nav,&nav.outgoing,"")&&cache_partition(&nav));
    char *first_hop=strdup(nav.partition);nav.target_origin="https://hop-two.test";CHECK(cache_partition(&nav)&&strcmp(first_hop,nav.partition));
    nav.target_origin="https://hop-one.test";CHECK(cache_partition(&nav)&&!strcmp(first_hop,nav.partition));free(first_hop);close_job(&nav);
    struct job opaque=test_job();opaque.top_level_url=strdup("");opaque.wire.id=1;opaque.wire.generation=7;
    CHECK(header_text(&opaque,&opaque.outgoing,"")&&cache_partition(&opaque)&&opaque.top_opaque);
    char *opaque_key=strdup(opaque.partition);opaque.wire.id=2;CHECK(cache_partition(&opaque)&&strcmp(opaque_key,opaque.partition));free(opaque_key);
    store(&opaque,"Cache-Control: max-age=600\r\n","opaque-body");CHECK(!cache_entries&&!opaque.cache_capture);close_job(&opaque);
    struct job opaque_frame=test_job();opaque_frame.wire.kind=WEBNET_NAVIGATION;opaque_frame.top_level_url=strdup("");
    CHECK(header_text(&opaque_frame,&opaque_frame.outgoing,"")&&cache_partition(&opaque_frame));
    CHECK(!top_level_navigation(&opaque_frame)&&!cache_context_allowed(&opaque_frame));
    store(&opaque_frame,"Cache-Control: max-age=600\r\n","opaque-frame");CHECK(!cache_entries&&!opaque_frame.cache_capture);
    close_job(&opaque_frame);
    struct job legacy=test_job(),explicit_top=test_job();explicit_top.top_level_url=strdup(legacy.document);
    CHECK(header_text(&legacy,&legacy.outgoing,"")&&header_text(&explicit_top,&explicit_top.outgoing,""));
    CHECK(cache_partition(&legacy)&&cache_partition(&explicit_top)&&!strcmp(legacy.partition,explicit_top.partition));
    CHECK(cookie_site_url(&legacy)==legacy.document);close_job(&legacy);close_job(&explicit_top);
    CHECK(sizeof(struct webnet_wire_request)==64&&sizeof(struct webnet_wire_top_level)==8&&
          (WEBNET_WIRE_REQUEST_FLAGS&WEBNET_WIRE_TOP_LEVEL));
}
int main(void){
    cache_preflight_order_cases();
    top_partition_cases();
    static const char *const credential_fields[]={
        "",
        "Authorization: first\r\nAuthorization: second\r\n",
        "Authorization: second\r\nAuthorization: first\r\n",
        "Authorization: first, second\r\n",
        "Authorization: first\r\nAuthorization: different\r\n",
        "Cookie: first\r\nCookie: second\r\n",
        "Cookie: second\r\nCookie: first\r\n",
        "Cookie: first; second\r\n",
        "Cookie:\r\n",
        "Authorization:\r\n"
    };
    char *keys[sizeof credential_fields/sizeof credential_fields[0]];
    for(unsigned i=0;i<sizeof keys/sizeof keys[0];i++)keys[i]=credential_partition(credential_fields[i]);
    for(unsigned i=0;i<sizeof keys/sizeof keys[0];i++)for(unsigned k=i+1;k<sizeof keys/sizeof keys[0];k++)
        CHECK(keys[i]&&keys[k]&&strcmp(keys[i],keys[k]));
    char *same=credential_partition("authorization: first\r\nAUTHORIZATION: second\r\n");
    CHECK(same&&!strcmp(same,keys[1]));free(same);
    for(unsigned i=0;i<sizeof keys/sizeof keys[0];i++)free(keys[i]);
    struct cache_entry repeated_vary={.headers="Vary: X-Variant\r\n",.request_headers="X-Variant: first\r\nX-Variant: second\r\n"};
    CHECK(cache_vary(&repeated_vary,"x-variant: first\r\nX-VARIANT: second\r\n"));
    CHECK(!cache_vary(&repeated_vary,""));
    CHECK(!cache_vary(&repeated_vary,"X-Variant: first\r\n"));
    CHECK(!cache_vary(&repeated_vary,"X-Variant: first, second\r\n"));
    CHECK(!cache_vary(&repeated_vary,"X-Variant: first\r\nX-Variant: different\r\n"));
    CHECK(!cache_vary(&repeated_vary,"X-Variant: second\r\nX-Variant: first\r\n"));
    repeated_vary.request_headers="";CHECK(!cache_vary(&repeated_vary,"X-Variant:\r\n"));
    CHECK(!cache_vary(&repeated_vary,"X-Variant: first\r\nX-Variant: second\r\n"));
    struct job j=test_job();CHECK(header_text(&j,&j.outgoing,"Accept-Language: ja\r\n"));CHECK(cache_partition(&j));
    store(&j,"Cache-Control: max-age=60\r\nETag: \"v1\"\r\nVary: Accept-Language\r\nSet-Cookie: secret=value\r\n","original");
    CHECK(cache_entries&&cache_bytes<FETCH_CACHE_BYTES&&!strstr(cache_entries->headers,"secret"));
    struct cache_entry *e=cache_find(&j);CHECK(e&&cache_usable(&j,e));
    struct http_resp hit;free(j.body);j.body=NULL;j.body_len=j.body_cap=0;
    CHECK(cache_response(&j,e,&hit)==0&&hit.status==200&&j.body_len==8&&!strcmp(j.body,"original"));
    CHECK(strstr(http_response_headers(&hit),"Age: 0\r\n"));free_response(&hit);
    clock_ms+=61000;CHECK(!cache_usable(&j,e));
    j.wire.user_navigation=WEBNET_CACHE_FORCE_CACHE<<WEBNET_WIRE_CACHE_SHIFT;CHECK(cache_usable(&j,e));
    j.wire.user_navigation=WEBNET_CACHE_ONLY_IF_CACHED<<WEBNET_WIRE_CACHE_SHIFT;CHECK(cache_usable(&j,e));
    j.wire.user_navigation=WEBNET_CACHE_NO_CACHE<<WEBNET_WIRE_CACHE_SHIFT;CHECK(!cache_usable(&j,e));
    j.wire.user_navigation=0;
    free(j.outgoing.p);j.outgoing=(struct header_buffer){0};CHECK(header_text(&j,&j.outgoing,"Accept-Language: en\r\n"));
    CHECK(!cache_find(&j));
    free(j.outgoing.p);j.outgoing=(struct header_buffer){0};CHECK(header_text(&j,&j.outgoing,"Accept-Language: ja\r\n"));
    char *partition=strdup(j.partition);CHECK(header_text(&j,&j.outgoing,"Authorization: token-one\r\nCookie: sid=one\r\n"));
    CHECK(cache_partition(&j)&&strcmp(partition,j.partition)&&!cache_find(&j));free(partition);
    j.origin="https://other-document.test";CHECK(cache_partition(&j)&&!cache_find(&j));
    j.origin="https://document.test";free(j.outgoing.p);j.outgoing=(struct header_buffer){0};CHECK(header_text(&j,&j.outgoing,"Accept-Language: ja\r\n"));CHECK(cache_partition(&j));
    j.cached=e;
    struct http_resp not_modified={.status=304};strcpy(not_modified.headers,"Cache-Control: max-age=120\r\nETag: \"v2\"\r\n");
    CHECK(response_start(&j,&not_modified)==1&&!j.stream_headers_sent);
    CHECK(!cache_validator_matches(e,&not_modified));
    strcpy(not_modified.headers,"Cache-Control: max-age=120\r\nETag: W/\"v1\"\r\n");CHECK(cache_validator_matches(e,&not_modified));
    char *merged=cache_clean_headers(e->headers,not_modified.headers,false,0);CHECK(merged&&strstr(merged,"max-age=120")&&!strstr(merged,"max-age=60"));
    free(e->headers);e->headers=merged;cache_freshness(e);CHECK(cache_usable(&j,e)&&!strcmp(e->body,"original"));
    uint64_t numeric;CHECK(cache_directive("Cache-Control: public\r\nCache-Control: max-age=\"90\", no-cache\r\n","max-age",&numeric)&&numeric==90);
    CHECK(cache_directive("Cache-Control: max-age=600,max-age=999\r\n","max-age",&numeric)&&numeric==0);
    CHECK(cache_directive("Cache-Control: max-age=0,max-stale\r\n","max-stale",&numeric)&&numeric==UINT64_MAX);
    struct cache_entry dated={.headers="Date: Sun, 06 Nov 1994 08:49:37 GMT\r\nExpires: Sun, 06 Nov 1994 08:51:37 GMT\r\nAge: 121\r\n"};
    cache_freshness(&dated);CHECK(dated.lifetime_seconds==120&&dated.age_seconds==121);
    CHECK(!cache_usable(&j,&dated));dated.must_revalidate=true;
    struct cache_entry duplicate_age={.headers="Cache-Control: max-age=600\r\nAge: 0\r\nAge: 999\r\n"};
    cache_freshness(&duplicate_age);CHECK(duplicate_age.age_seconds==UINT64_MAX&&!cache_usable(&j,&duplicate_age));
    j.headers="Pragma: no-cache\r\n";CHECK(!cache_usable(&j,e));j.headers="";
    j.wire.user_navigation=WEBNET_CACHE_FORCE_CACHE<<WEBNET_WIRE_CACHE_SHIFT;CHECK(!cache_usable(&j,&dated));
    j.wire.user_navigation=0;j.headers="Cache-Control: no-store\r\n";struct http_resp r;response(&r,"Cache-Control: max-age=600\r\n");
    CHECK(response_start(&j,&r)==0&&!j.cache_capture);j.headers="";
    response(&r,"Cache-Control: no-store\r\n");CHECK(response_start(&j,&r)==0&&!j.cache_capture);
    response(&r,"Cache-Control: max-age=600\r\nVary: *\r\n");CHECK(response_start(&j,&r)==0);
    size_t saved=cache_bytes;cache_store(&j,&r);CHECK(cache_bytes==saved);
    /* A retained snapshot survives cache invalidation and can finish a stream. */
    cache_invalidate(j.final_url);CHECK(!cache_entries&&!cache_bytes&&!strcmp(e->body,"original"));
    j.cache_capture=true;j.wire.deadline=clock_ms;cache_store(&j,&r);CHECK(!cache_entries);j.error[0]=0;j.wire.deadline=999999;
    struct http_req q={.method="POST",.body="mutation",.body_len=8};
    struct http_transport_result tr={.error=HTTP_ERR_CONNECT,.phase=HTTP_PHASE_CONNECT};
    struct job retry=test_job();struct http_resp empty={0};CHECK(transport_retry(&retry,&q,&empty,&tr,0));
    tr.request_bytes_sent=1;CHECK(!transport_retry(&retry,&q,&empty,&tr,0));
    q=(struct http_req){.method="GET"};tr.error=HTTP_ERR_RESET;tr.phase=HTTP_PHASE_HEADER;CHECK(transport_retry(&retry,&q,&empty,&tr,0));
    tr.response_bytes_received=1;CHECK(!transport_retry(&retry,&q,&empty,&tr,0));tr.response_bytes_received=0;
    tr.error=HTTP_ERR_TLS_VERIFY;CHECK(!transport_retry(&retry,&q,&empty,&tr,0));tr.error=HTTP_ERR_DNS_NOT_FOUND;CHECK(!transport_retry(&retry,&q,&empty,&tr,0));
    tr.error=HTTP_ERR_RESET;retry.stream_headers_sent=true;CHECK(!transport_retry(&retry,&q,&empty,&tr,0));
    struct job options=test_job();options.partition=strdup("private-preflight-partition");expected_partition=options.partition;
    CHECK(preflight(&options,"https://other.test/resource","PATCH","x-test")&&options.body_len==0&&!options.error[0]);
    options.wire.user_navigation=WEBNET_WIRE_SERVICE;cancel_during_preflight=true;
    CHECK(!preflight(&options,"https://other.test/resource","PATCH","x-test")&&options.body_len==0&&strstr(options.error,"cancelled"));
    CHECK(preflight_calls==2);peer_hup=false;cancel_during_preflight=false;close_job(&options);
    struct service_slot slot={.fd=17};
    service_request(&slot);
    CHECK(slot.done&&setup_calls==2&&setup_fds[0]==17&&setup_fds[1]==-1&&read_calls==1&&close_calls==1);
    slot=(struct service_slot){.fd=19};setup_calls=0;read_calls=0;close_calls=0;fail_setup=true;
    service_request(&slot);
    CHECK(slot.done&&setup_calls==2&&setup_fds[0]==19&&setup_fds[1]==-1&&!read_calls&&close_calls==1);
    close_job(&j);clear_cache();printf("webfetch-cache: %u checks, %u failures\n",checks,failed);return failed!=0;
}
