/* New outgoing header scope only. These anonymous synthetic fields are not site acceptance. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>
#include <ctype.h>
#include <time.h>
#include "http.h"
#include "webnet_wire.h"
static size_t live, fail_size, cookie_size;
static bool cookie_short;
static unsigned cookie_calls, requests;
static char *observed_headers;
static const char *response_fields;
static void *tracked_malloc(size_t n) { if(fail_size==n)return NULL; void*p=malloc(n);if(p)live++;return p; }
static void *tracked_realloc(void *p,size_t n) { if(fail_size==n)return NULL;void*q=realloc(p,n);if(q&&!p)live++;return q; }
static void tracked_free(void*p) {if(p)live--;free(p);}
long webcookie_get(webcookie_jar*j,const struct webcookie_context*c,char*out,size_t cap,int64_t now) {
    (void)j;(void)c;(void)now;cookie_calls++;
    if(!out)return (long)cookie_size;
    if(cap<=cookie_size)return -1;
    memset(out,'k',cookie_size);out[cookie_size]=0;
    return (long)cookie_size-(cookie_short?1:0);
}
const char *http_response_headers(const struct http_resp*r) {return r->headers_full?r->headers_full:r->headers;}
void http_resp_free(struct http_resp*r) {(void)r;}
int http_request(const struct http_req*q,struct http_resp*r) {
    requests++;free(observed_headers);observed_headers=strdup(q->headers);
    memset(r,0,sizeof *r);r->status=204;r->headers_full=(char*)response_fields;return 0;
}
struct job;
static int remaining(struct job*j) {(void)j;return 1000;}
static int discard_cb(void*j,const char*p,size_t n) {(void)j;(void)p;(void)n;return 0;}
#define malloc tracked_malloc
#define realloc tracked_realloc
#define free tracked_free
#include "../build/goal-20261009/http-request-headers-tested.inc"
#undef malloc
#undef realloc
#undef free
static unsigned checks, failed;
#define CHECK(c) do{checks++;if(!(c)){failed++;printf("FAIL request header:%d %s\n",__LINE__,#c);}}while(0)
static void close_job(struct job*j) {tracked_free(j->non_simple);tracked_free(j->outgoing.p);j->non_simple=NULL;j->outgoing=(struct header_buffer){0};}
static char *large_header(size_t name,size_t value) {
    char*p=malloc(name+value+5);memset(p,'x',name);p[name]=':';p[name+1]=' ';
    memset(p+name+2,'v',value);memcpy(p+name+2+value,"\r\n",3);return p;
}
int main(void) {
    struct job j={.wire={.kind=WEBNET_FETCH,.credentials=WEBNET_CREDENTIALS_OMIT},.document="https://request.test/"};
    strcpy(j.origin,"https://request.test");
    char *raw=large_header(300,12000);j.headers=raw;
    CHECK(validate_headers(&j)&&strlen(j.non_simple)==300);
    CHECK(prepare_outgoing(&j,"https://other.test/","POST",true)&&j.outgoing.length>12300&&strstr(j.outgoing.p,"Origin: https://request.test\r\n"));
    CHECK(!memcmp(j.outgoing.p,raw,strlen(raw))&&j.outgoing.p[j.outgoing.length]==0);
    close_job(&j);CHECK(live==0);
    /* A security byte past the old 8 KiB boundary must be checked, not truncated. */
    raw[11000]='\1';CHECK(!validate_headers(&j)&&strstr(j.error,"value"));CHECK(live==0);raw[11000]='v';j.error[0]=0;
    fail_size=301;CHECK(!validate_headers(&j)&&strstr(j.error,"memory"));fail_size=0;CHECK(live==0);free(raw);
    raw=large_header(9000,1);j.headers=raw;j.error[0]=0;
    CHECK(validate_headers(&j)&&strlen(j.non_simple)==9000);
    fail_size=16384;CHECK(!prepare_outgoing(&j,"https://other.test/","POST",true)&&strstr(j.error,"memory"));fail_size=0;
    close_job(&j);CHECK(live==0);free(raw);
    /* Name-list growth independent of individual name/value size. */
    raw=malloc(80000);size_t at=0;
    for(unsigned i=0;i<2500;i++)at+=(size_t)sprintf(raw+at,"X-%u: v\r\n",i);
    j.headers=raw;j.error[0]=0;
    CHECK(validate_headers(&j)&&strlen(j.non_simple)>16000&&strstr(j.non_simple,"x-2499"));
    size_t fields_len=strlen(j.non_simple)+200;
    char*fields=malloc(fields_len);
    snprintf(fields,fields_len,"Access-Control-Allow-Origin: https://request.test\r\nAccess-Control-Allow-Methods: PATCH\r\nAccess-Control-Allow-Headers: %s\r\n",j.non_simple);
    response_fields=fields;
    CHECK(preflight(&j,"https://other.test/","PATCH",j.non_simple)&&requests==1);
    CHECK(strlen(observed_headers)>16000&&strstr(observed_headers,"x-2499\r\n")&&strstr(observed_headers,"Access-Control-Request-Method: PATCH\r\n"));
    j.wire.user_navigation=WEBNET_CACHE_RELOAD<<WEBNET_WIRE_CACHE_SHIFT;
    CHECK(prepare_outgoing(&j,"https://other.test/","PATCH",true)&&strstr(j.outgoing.p,"Pragma: no-cache\r\n")&&strstr(j.outgoing.p,"Cache-Control: no-cache\r\n"));
    /* Cookie storage policy is unchanged; outgoing capacity is no longer 8 KiB. */
    j.wire.credentials=WEBNET_CREDENTIALS_INCLUDE;cookie_size=24000;cookie_calls=0;
    CHECK(prepare_outgoing(&j,"https://other.test/","PATCH",true)&&j.outgoing.length>50000&&cookie_calls==2);
    CHECK(strstr(j.outgoing.p,"Cookie: ")&&j.outgoing.p[j.outgoing.length-2]=='\r');
    cookie_short=true;CHECK(!prepare_outgoing(&j,"https://other.test/","PATCH",true)&&strstr(j.error,"complete"));cookie_short=false;
    j.wire.credentials=WEBNET_CREDENTIALS_SAME_ORIGIN;j.cors_tainted=true;cookie_calls=0;
    CHECK(prepare_outgoing(&j,"https://request.test/","PATCH",false)&&!j.hop_cookies&&!cookie_calls&&!strstr(j.outgoing.p,"Cookie:"));
    j.origin_tainted=true;
    CHECK(prepare_outgoing(&j,"https://other.test/","PATCH",true)&&strstr(j.outgoing.p,"Origin: null\r\n"));
    close_job(&j);free(raw);free(fields);CHECK(live==0);
    /* A long forbidden/duplicate field remains a security error, not a limit error. */
    raw=large_header(4,12000);memcpy(raw,"Host",4);j.headers=raw;j.error[0]=0;
    CHECK(!validate_headers(&j)&&strstr(j.error,"Prohibited"));free(raw);CHECK(live==0);
    raw=malloc(20000);at=(size_t)sprintf(raw,"X-Large: ");memset(raw+at,'v',12000);at+=12000;sprintf(raw+at,"\r\nx-large: other\r\n");j.headers=raw;j.error[0]=0;
    CHECK(!validate_headers(&j)&&strstr(j.error,"Duplicate"));free(raw);CHECK(live==0);
    /* Required 128-byte safelist rule survives unlimited request capacity. */
    raw=large_header(6,129);memcpy(raw,"Accept",6);j.headers=raw;j.error[0]=0;
    CHECK(validate_headers(&j)&&!strcmp(j.non_simple,"accept"));close_job(&j);free(raw);
    /* Wildcards do not expand credentialed requests or Authorization. */
    j.wire.credentials=WEBNET_CREDENTIALS_OMIT;j.origin_tainted=false;
    response_fields="Access-Control-Allow-Origin: https://request.test\r\nAccess-Control-Allow-Headers: *\r\n";
    CHECK(!preflight(&j,"https://other.test/","GET","authorization"));
    response_fields="Access-Control-Allow-Origin: https://request.test\r\nAccess-Control-Allow-Credentials: true\r\nAccess-Control-Allow-Headers: *\r\n";
    j.wire.credentials=WEBNET_CREDENTIALS_INCLUDE;
    CHECK(!preflight(&j,"https://other.test/","GET","x-large"));
    /* Duplicate singleton is not silently replaced by a later wide list. */
    response_fields="Access-Control-Allow-Origin: https://request.test\r\nAccess-Control-Allow-Headers: x-large\r\nAccess-Control-Allow-Headers: *\r\n";
    j.wire.credentials=WEBNET_CREDENTIALS_OMIT;
    CHECK(!preflight(&j,"https://other.test/","GET","x-large"));
    /* Checked growth preserves the published buffer on real allocation failure. */
    struct header_buffer b={0};CHECK(header_text(&j,&b,"kept"));char*old=b.p;size_t cap=b.capacity;
    fail_size=32768;CHECK(!header_reserve(&j,&b,20000)&&b.p==old&&b.capacity==cap&&!strcmp(b.p,"kept"));fail_size=0;tracked_free(b.p);
    b=(struct header_buffer){.length=SIZE_MAX-2};CHECK(!header_reserve(&j,&b,2));
    CHECK(webnet_wire_request_header_limit(0)==8191&&webnet_wire_request_header_limit(WEBNET_WIRE_DYNAMIC_REQUEST_HEADERS)==UINT32_MAX);
    struct webnet_wire_request w={.url_len=UINT32_MAX,.origin_len=UINT32_MAX,.method_len=UINT32_MAX,.headers_len=UINT32_MAX,.body_len=UINT32_MAX,.cookie_len=UINT32_MAX};size_t total=0;
    CHECK(webnet_wire_request_payload_size(&w,&total)&&total==(uint64_t)UINT32_MAX*6);
    CHECK(sizeof w==64&&offsetof(struct webnet_wire_request,headers_len)==52);
    free(observed_headers);CHECK(live==0);
    printf("request dynamic new boundaries: %u checks / %u failed\n",checks,failed);return failed!=0;
}
