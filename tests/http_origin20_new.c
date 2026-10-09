/* Only new origin/authority/CORS flows. Real source, native allocator/user
   surroundings and transport response fixtures; not real-site acceptance. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <ctype.h>
#include <time.h>
#include "http.h"
#include "webnet_wire.h"
#define MIN(a,b) ((a) < (b) ? (a) : (b))
static size_t live;
static unsigned fail_malloc;
static void *count_malloc(size_t n){if(fail_malloc&&! --fail_malloc)return NULL;void*p=malloc(n);if(p)live++;return p;}
static void *count_calloc(size_t n,size_t s){void*p=calloc(n,s);if(p)live++;return p;}
static void *count_realloc(void*p,size_t n){void*q=realloc(p,n);if(q&&!p)live++;return q;}
static void count_free(void*p){if(p){live--;free(p);}}
static uint64_t uptime_ms(void){return 100;}
#define malloc count_malloc
#define calloc count_calloc
#define realloc count_realloc
#define free count_free
#define http_request unused_real_http_request
#define http_request_limited unused_real_http_request_limited
#include "../user/libc/http.c"
#undef http_request
#undef http_request_limited
#include "../user/libc/web/util.c"
#include "../build/goal-20261009/http-origin20-webfetch-tested.inc"
#undef malloc
#undef calloc
#undef realloc
#undef free
struct net_stream {int unused;};
net_stream *ns_open(const char*a,uint16_t b,bool c,int d,char*e,size_t f){(void)a;(void)b;(void)c;(void)d;(void)e;(void)f;return NULL;}
long ns_write(net_stream*a,const void*b,size_t c){(void)a;(void)b;(void)c;return -1;}
long ns_read(net_stream*a,void*b,size_t c){(void)a;(void)b;(void)c;return -1;}
const char *ns_error(net_stream*a){(void)a;return "unused stream";}
void ns_close(net_stream*a){(void)a;}
void ns_set_timeout(net_stream*a,int b){(void)a;(void)b;}
int http_inflate(char*a,int b,const char*c,int d,int*e){(void)a;(void)b;(void)c;(void)d;(void)e;return -1;}
long webcookie_get(webcookie_jar*a,const struct webcookie_context*b,char*c,size_t d,int64_t e){(void)a;(void)b;(void)c;(void)d;(void)e;return 0;}
int webcookie_set(webcookie_jar*a,const struct webcookie_context*b,const char*c,size_t d,int64_t e){(void)a;(void)b;(void)c;(void)d;(void)e;return 0;}
bool webcookie_same_site(const webcookie_jar*a,const char*b,const char*c){(void)a;return !strcmp(b,c);}
static char host_a[254],host_b[254],host_c[254],url_a[512],url_b[512],url_c[512],origin_a[512];
static unsigned requests,checks,failed;
static bool redirects;
static char observed_headers[3][2048];
static int fixture_request(const struct http_req*q,struct http_resp*r){
    unsigned call=requests++;memset(r,0,sizeof *r);if(call>=3)return -1;
    snprintf(observed_headers[call],sizeof observed_headers[call],"%s",q->headers?q->headers:"");
    char headers[1600];
    if(redirects&&call==0){r->status=301;snprintf(headers,sizeof headers,"Location: %s\r\n",url_b);}
    else if(redirects&&call==1){r->status=302;snprintf(headers,sizeof headers,"Location: %s\r\nAccess-Control-Allow-Origin: %s\r\n",url_c,origin_a);}
    else {r->status=200;snprintf(headers,sizeof headers,"Access-Control-Allow-Origin: %s\r\n",redirects?"null":origin_a);}
    size_t n=strlen(headers);r->headers_full=count_malloc(n+1);if(!r->headers_full)return -1;
    memcpy(r->headers_full,headers,n+1);r->headers_len=n;return 0;
}
int http_request(const struct http_req*q,struct http_resp*r){return fixture_request(q,r);}
int http_request_limited(const struct http_req*q,struct http_resp*r,size_t limit){(void)limit;return fixture_request(q,r);}
#define CHECK(label,condition) do{checks++;if(!(condition)){failed++;printf("FAIL %s\n",label);}}while(0)
static void hostname(char*out){memset(out,'A',253);out[63]=out[127]=out[191]='.';out[253]=0;}
static struct job *make_job(const char*url,const char*document,const char*headers){struct job*j=calloc(1,sizeof *j);j->url=(char*)url;j->document=(char*)document;j->method="GET";j->headers=strdup(headers);j->wire.kind=WEBNET_FETCH;j->wire.credentials=WEBNET_CREDENTIALS_OMIT;j->wire.deadline=UINT64_MAX;return j;}
static void close_job(struct job*j){count_free(j->origin);count_free(j->target_origin);count_free(j->body);count_free(j->non_simple);count_free(j->outgoing.p);count_free(j->response_headers);count_free(j->cookie_events);free(j->headers);free(j);requests=0;redirects=false;memset(observed_headers,0,sizeof observed_headers);fail_malloc=0;}
int main(void){
    hostname(host_a);memcpy(host_b,host_a,sizeof host_a);memcpy(host_c,host_a,sizeof host_a);host_b[252]='B';host_c[252]='C';
    snprintf(url_a,sizeof url_a,"HTTPS://%s:443/start",host_a);snprintf(url_b,sizeof url_b,"https://%s/second",host_b);snprintf(url_c,sizeof url_c,"https://%s/third",host_c);
    char *canonical=NULL,*origin=NULL;enum http_url_result result=http_canonical_owned_n(url_a,strlen(url_a),&canonical,&origin);
    CHECK("new exact origin exceeds old160 and256 slots",result==HTTP_URL_TUPLE&&strlen(origin)==261&&strstr(origin,":443")==NULL);
    if(origin)snprintf(origin_a,sizeof origin_a,"%s",origin);
    CHECK("canonical ASCII host case and default port are unified",canonical&&!strncmp(canonical,"https://aaa",11)&&!strstr(canonical,":443")&&strstr(canonical,"/start"));count_free(canonical);count_free(origin);
    CHECK("shared origin ignores nonASCII path rather than adding rejection",http_origin_owned("https://under_score.test/\xE6\x97\xA5",&origin)==HTTP_URL_TUPLE&&!strcmp(origin,"https://under_score.test"));count_free(origin);
    for(unsigned fault=1;fault<=4;fault++){fail_malloc=fault;canonical=origin=(char*)1;result=http_canonical_owned_n(url_a,strlen(url_a),&canonical,&origin);CHECK("each shared preparation allocation OOM keeps outputsNULL",result==HTTP_URL_OOM&&!canonical&&!origin&&live==0);}fail_malloc=0;
    canonical=origin=(char*)1;result=http_canonical_owned_n("about:blank",11,&canonical,&origin);CHECK("opaque result is distinct no empty origin allocation",result==HTTP_URL_OPAQUE&&!canonical&&!origin&&!live);
    char nul[]="https://safe.test\0.other.test/";CHECK("shared explicit origin rejects NUL beyond apparent prefix",http_origin_owned_n(nul,sizeof nul-1,&origin)==HTTP_URL_INVALID&&!origin);
    CHECK("malformed HTTP does not silently become opaque",http_origin_owned("https:broken",&origin)==HTTP_URL_INVALID&&!origin);
    char out[1024],expected[1024];snprintf(expected,sizeof expected,"%s/next#keep",origin_a);
    CHECK("resolver long authority preserves relative path and fragment",web_resolve_url(url_a,"/next#keep",out,sizeof out)&&!strcmp(out,expected));
    snprintf(expected,sizeof expected,"https://%s:8443/next",host_b);for(char*p=expected;p<strstr(expected,":8443");p++)if(*p>='A'&&*p<='Z')*p+=(char)32;
    char relative[512];snprintf(relative,sizeof relative,"//%s:8443/next",host_b);
    CHECK("resolver network-path authority exceeds old256 without prefix",web_resolve_url(url_a,relative,out,sizeof out)&&!strcmp(out,expected));
    fail_malloc=1;strcpy(out,"unchanged");CHECK("resolver authority OOM clears output and releases heap",!web_resolve_url(url_a,"/next",out,sizeof out)&&!out[0]&&live==0);fail_malloc=0;
    strcpy(out,"unchanged");CHECK("resolver undersized destination cannot return truncated origin",!web_resolve_url(url_a,"/next",out,100)&&!out[0]&&live==0);
    struct job*j=make_job(url_a,url_a,"");CHECK("actual webfetch same-origin longhost completes",run_http(j)&&requests==1&&!strcmp(j->origin,j->target_origin)&&!j->cors_tainted);close_job(j);CHECK("webfetch long origin success releases all new ownership",live==0);
    j=make_job(url_b,url_a,"");j->wire.user_navigation|=WEBNET_WIRE_SAME_ORIGIN;CHECK("long hostname suffix difference still rejects sameorigin mode",!run_http(j)&&requests==0&&strstr(j->error,"Cross-origin"));close_job(j);CHECK("sameorigin rejection releases long origin allocations",live==0);
    j=make_job(url_b,url_a,"");CHECK("cross-origin exact long AllowOrigin succeeds without159 quota",run_http(j)&&requests==1&&j->cors_tainted&&strstr(observed_headers[0],origin_a));close_job(j);CHECK("long CORS success releases all allocations",live==0);
    j=make_job(url_a,url_a,"Authorization: fixture-only\r\n");redirects=true;CHECK("real webfetch threehop origin taint flow completes",run_http(j)&&requests==3&&j->cors_tainted&&j->origin_tainted);
    CHECK("redirect drops authorization and serializes later Origin null",strstr(observed_headers[0],"Authorization: fixture-only")&&!strstr(observed_headers[1],"Authorization:")&&!strstr(observed_headers[2],"Authorization:")&&strstr(observed_headers[1],origin_a)&&strstr(observed_headers[2],"Origin: null\r\n"));close_job(j);CHECK("redirect previous-next origin ownership releases once",live==0);
    j=make_job(url_a,url_a,"");fail_malloc=5;CHECK("initiator origin OOM is fatal not opaque equality",!run_http(j)&&requests==0&&strstr(j->error,"memory")&&!j->origin);close_job(j);CHECK("initiator failure keeps target owned until final cleanup",live==0);
    j=make_job(url_b,"about:blank","");j->wire.user_navigation|=WEBNET_WIRE_SAME_ORIGIN;CHECK("opaque document cannot equal any tuple origin",!run_http(j)&&requests==0&&strstr(j->error,"Cross-origin"));close_job(j);
    struct http_resp response={0};char fields[1600];j=make_job(url_a,url_a,"");http_origin_owned(url_a,&j->origin);j->wire.credentials=WEBNET_CREDENTIALS_INCLUDE;
    snprintf(fields,sizeof fields,"Access-Control-Allow-Origin: %s\r\nAccess-Control-Allow-Credentials: true\r\n",origin_a);response.headers_full=fields;
    CHECK("credentialed long exact CORS permission remains required",cors_allowed(j,&response));
    snprintf(fields,sizeof fields,"Access-Control-Allow-Origin: *\r\nAccess-Control-Allow-Credentials: true\r\n");CHECK("credentialed wildcard remains forbidden",!cors_allowed(j,&response));
    snprintf(fields,sizeof fields,"Access-Control-Allow-Origin: %s\r\nAccess-Control-Allow-Origin: %s\r\nAccess-Control-Allow-Credentials: true\r\n",origin_a,origin_a);CHECK("duplicate long AllowOrigin remains forbidden",!cors_allowed(j,&response));close_job(j);
    CHECK("all new origin lifetime conditions finish with no tracked ownership",live==0);
    printf("new owned-origin/resolver/webfetch boundaries: %u checks / %u failed\n",checks,failed);return failed?1:0;
}
