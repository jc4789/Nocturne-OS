/* New dynamic-header boundaries only. Synthetic stream, never site acceptance. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>
#include "http.h"
#include "webnet_wire.h"
static size_t live_allocations,fail_size;
static void *test_malloc(size_t n){if(fail_size&&n==fail_size)return NULL;void*p=malloc(n);if(p)live_allocations++;return p;}
static void *test_calloc(size_t n,size_t k){if(k&&n>SIZE_MAX/k)return NULL;void*p=test_malloc(n*k);if(p)memset(p,0,n*k);return p;}
static void *test_realloc(void*p,size_t n){if(fail_size&&n==fail_size)return NULL;void*q=realloc(p,n);if(q&&!p)live_allocations++;return q;}
static void test_free(void*p){if(p)live_allocations--;free(p);}
struct net_stream {const char*p;size_t n,at,split;};
static struct net_stream wire;
net_stream *ns_open(const char*h,uint16_t port,bool tls,int timeout,char*error,size_t cap){(void)h;(void)port;(void)tls;(void)timeout;(void)error;(void)cap;wire.at=0;return &wire;}
long ns_write(net_stream*s,const void*p,size_t n){(void)s;(void)p;return (long)n;}
long ns_read(net_stream*s,void*p,size_t n){size_t left=s->n-s->at;if(n>left)n=left;if(n>s->split)n=s->split;memcpy(p,s->p+s->at,n);s->at+=n;return (long)n;}
const char *ns_error(net_stream*s){(void)s;return "";}
void ns_close(net_stream*s){(void)s;}
int http_inflate(char*out,int cap,const char*in,int len,int*used){(void)out;(void)cap;(void)in;(void)len;(void)used;return -1;}
#define malloc test_malloc
#define calloc test_calloc
#define realloc test_realloc
#define free test_free
#include "../user/libc/http.c"
struct job {struct webnet_wire_request wire;bool cors_tainted,redirected;char *response_headers;size_t response_headers_len;char error[160];};
static bool fail(struct job*j,const char*s){snprintf(j->error,sizeof j->error,"%s",s);return false;}
#include "../build/goal-20261009/http-response-filter-tested.inc"
#undef malloc
#undef calloc
#undef realloc
#undef free
static unsigned checks,failed,fields;
static size_t max_line;
#define CHECK(c) do{checks++;if(!(c)){failed++;printf("FAIL dynamic HTTP:%d %s\n",__LINE__,#c);}}while(0)
static int observed(void*c,const char*p,size_t n){(void)c;(void)p;fields++;if(n>max_line)max_line=n;return 0;}
static char *new_wire(size_t size){char*p=malloc(size+512);CHECK(p!=NULL);return p;}
static struct http_resp receive(const char*p,size_t n,size_t split,bool ok,size_t failure){
    wire=(struct net_stream){p,n,0,split};struct http_req q={.url="https://dynamic-header.test/",.on_header=observed};struct http_resp r;
    fields=0;max_line=0;fail_size=failure;int result=http_request(&q,&r);fail_size=0;
    CHECK((result==0)==ok);if(!ok)CHECK(r.error[0]&&!http_response_headers(&r)[0]);return r;
}
int main(void){
    size_t length=180000;char *p=new_wire(length);size_t at=(size_t)sprintf(p,"HTTP/1.1 200 OK\r\nX-Large: ");
    memset(p+at,'q',length);at+=length;at+=(size_t)sprintf(p+at,"\r\nContent-Length: 2\r\n\r\nok");
    struct http_resp r=receive(p,at,257,true,0);
    CHECK(r.headers_len>180000&&r.headers_full&&!r.headers[0]&&r.body_len==2&&!strcmp(r.body,"ok"));
    CHECK(fields==2&&max_line==length+9);
    struct job j={.wire={.kind=WEBNET_FETCH,.user_navigation=WEBNET_WIRE_DYNAMIC_HEADERS|WEBNET_WIRE_STATUS_LINE}};
    CHECK(response_headers(&j,&r,false)&&j.response_headers_len>length&&strstr(j.response_headers,"X-Large: ")!=NULL);
    test_free(j.response_headers);j.response_headers=NULL;j.wire.user_navigation=WEBNET_WIRE_LARGE_HEADERS;
    CHECK(!response_headers(&j,&r,false)&&!j.response_headers);
    http_resp_free(&r);CHECK(live_allocations==0);
    r=receive(p,at,511,false,32768);http_resp_free(&r);CHECK(live_allocations==0);
    /* Late forbidden byte cannot disappear into a truncated security prefix. */
    size_t value_start=strlen("HTTP/1.1 200 OK\r\nX-Large: ");p[value_start+170000]=0;
    r=receive(p,at,4096,false,0);http_resp_free(&r);CHECK(live_allocations==0);free(p);
    length=4096;p=new_wire(length);at=(size_t)sprintf(p,"HTTP/1.1 200 ");memset(p+at,'R',length);at+=length;at+=(size_t)sprintf(p+at,"\r\nContent-Length: 0\r\n\r\n");
    r=receive(p,at,31,true,0);CHECK(r.status_text_full&&!r.status_text[0]&&strlen(http_response_status_text(&r))==length);
    j=(struct job){.wire={.kind=WEBNET_FETCH,.user_navigation=WEBNET_WIRE_DYNAMIC_HEADERS|WEBNET_WIRE_STATUS_LINE}};
    CHECK(response_headers(&j,&r,false)&&j.response_headers_len>length&&j.response_headers[13]=='R');test_free(j.response_headers);
    http_resp_free(&r);CHECK(live_allocations==0);
    r=receive(p,at,31,false,length+1);http_resp_free(&r);CHECK(live_allocations==0);free(p);
    /* Many short fields grow the complete block, independently of line size. */
    p=new_wire(150000);at=(size_t)sprintf(p,"HTTP/1.1 200 OK\r\n");
    for(int i=0;i<100;i++){at+=(size_t)sprintf(p+at,"X-%d: ",i);memset(p+at,'a',1100);at+=1100;memcpy(p+at,"\r\n",2);at+=2;}
    at+=(size_t)sprintf(p+at,"Content-Length: 2\r\n\r\nok");r=receive(p,at,149,true,0);CHECK(r.headers_len>65536&&fields==101&&r.body_len==2);http_resp_free(&r);CHECK(live_allocations==0);
    r=receive(p,at,4096,false,131072);http_resp_free(&r);CHECK(live_allocations==0);free(p);
    /* Interim fields, chunk extensions and trailers use the same dynamic reader. */
    p=new_wire(220000);at=(size_t)sprintf(p,"HTTP/1.1 100 Continue\r\nX-Interim: ");memset(p+at,'i',70000);at+=70000;
    at+=(size_t)sprintf(p+at,"\r\n\r\nHTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n1;");memset(p+at,'e',70000);at+=70000;
    at+=(size_t)sprintf(p+at,"\r\nx\r\n0\r\nX-Trailer: ");memset(p+at,'t',70000);at+=70000;at+=(size_t)sprintf(p+at,"\r\n\r\n");
    r=receive(p,at,37,true,0);CHECK(r.status==200&&r.body_len==1&&!strcmp(r.body,"x"));http_resp_free(&r);CHECK(live_allocations==0);free(p);
    p=new_wire(90000);at=(size_t)sprintf(p,"HTTP/1.1 200 OK\r\nX-Incomplete: ");memset(p+at,'i',80000);at+=80000;r=receive(p,at,313,false,0);http_resp_free(&r);CHECK(live_allocations==0);free(p);
    const char exposed[]="X-Large: visible\r\nAccess-Control-Expose-Headers: X-Large\r\nSet-Cookie: synthetic\r\n";
    r=(struct http_resp){.headers_full=(char*)exposed};j=(struct job){.wire={.kind=WEBNET_FETCH,.user_navigation=WEBNET_WIRE_DYNAMIC_HEADERS,.credentials=WEBNET_CREDENTIALS_INCLUDE}};
    CHECK(response_headers(&j,&r,true)&&strstr(j.response_headers,"X-Large:")&&!strstr(j.response_headers,"Set-Cookie:"));test_free(j.response_headers);CHECK(live_allocations==0);
    CHECK(webnet_wire_header_limit(WEBNET_WIRE_DYNAMIC_HEADERS)==UINT32_MAX);
    struct webnet_wire_response payload={.url_len=UINT32_MAX,.headers_len=UINT32_MAX,.body_len=UINT32_MAX,.error_len=UINT32_MAX,.cookie_len=UINT32_MAX};size_t total=0;
    CHECK(webnet_wire_payload_size(&payload,&total)&&total==(uint64_t)UINT32_MAX*5);
    CHECK(sizeof(struct webnet_wire_request)==64&&sizeof(struct webnet_wire_response)==48);
    printf("http dynamic new boundaries: %u checks / %u failed\n",checks,failed);return failed!=0;
}
