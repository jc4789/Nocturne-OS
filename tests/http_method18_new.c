/* First-only new long-method path. Stream/allocation fault surroundings are
   fixtures, not external HTTP or real browser acceptance. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include "http.h"
#include "webnet.h"
#include "webnet_wire.h"
static size_t live,fail_size;
static const char *overflow_string;
static void *tracked_malloc(size_t n){if(fail_size&&n==fail_size){fail_size=0;return NULL;}void*p=malloc(n);if(p)live++;return p;}
static void *tracked_calloc(size_t a,size_t b){void*p=calloc(a,b);if(p)live++;return p;}
static void *tracked_realloc(void *p,size_t n){void*q=realloc(p,n);if(q&&!p)live++;return q;}
static void tracked_free(void *p){if(p){live--;free(p);}}
static size_t fixture_strlen(const char *p){return p==overflow_string?SIZE_MAX:strlen(p);}
#define malloc tracked_malloc
#define calloc tracked_calloc
#define realloc tracked_realloc
#define free tracked_free
#define strlen fixture_strlen
#include "../user/libc/http.c"
#undef malloc
#undef calloc
#undef realloc
#undef free
#undef strlen
struct net_stream {size_t read_at;};
static struct net_stream stream;
static char *sent;
static size_t sent_length,sent_capacity;
static unsigned opens,closes,checks,failed;
static bool send_failure;
net_stream *ns_open(const char*host,uint16_t port,bool tls,int timeout,char*error,size_t n){(void)host;(void)port;(void)tls;(void)timeout;(void)error;(void)n;opens++;stream.read_at=0;return &stream;}
long ns_write(net_stream*s,const void*data,size_t n){(void)s;if(send_failure)return -1;if(sent_length+n+1>sent_capacity){sent_capacity=sent_length+n+1;sent=realloc(sent,sent_capacity);}memcpy(sent+sent_length,data,n);sent_length+=n;sent[sent_length]=0;return (long)n;}
long ns_read(net_stream*s,void*data,size_t n){const char *response="HTTP/1.1 200 OK\r\nContent-Length: 0\r\n\r\n";size_t left=strlen(response)-s->read_at;if(n>left)n=left;memcpy(data,response+s->read_at,n);s->read_at+=n;return (long)n;}
const char *ns_error(net_stream*s){(void)s;return "fixture stream error";}
void ns_close(net_stream*s){(void)s;closes++;}
void ns_set_timeout(net_stream*s,int timeout){(void)s;(void)timeout;}
int http_inflate(char*out,int cap,const char*in,int n,int*consumed){(void)out;(void)cap;(void)in;(void)n;(void)consumed;return -1;}
#define CHECK(name,test) do{checks++;if(!(test)){failed++;printf("FAIL %s\n",name);}}while(0)
static void reset(void){free(sent);sent=NULL;sent_capacity=sent_length=0;opens=closes=0;send_failure=false;}
int main(void){
    char *method=malloc(70001);memset(method,'X',70000);method[70000]=0;method[65]=0;
    struct http_req request={.method=method,.url="https://fixture.test/path",.headers="X-Fixture: long-method\r\n"};
    struct http_resp response;
    CHECK("method above former63 bytes validates complete token",webnet_method_valid(method)&&WEBNET_METHOD_MAX==UINT32_MAX);
    int result=http_request(&request,&response);
    CHECK("65 byte method really reaches native stream unchanged",result==0&&sent_length>65&&!memcmp(sent,method,65)&&sent[65]==' ');
    http_resp_free(&response);CHECK("65 byte path releases all HTTP allocations",live==0&&opens==1&&closes==1);reset();
    method[65]='X';
    CHECK("70000 byte method remains valid native token",webnet_method_valid(method));
    result=http_request(&request,&response);
    CHECK("request line beyond oldURL16K buffer is streamed fully",result==0&&sent_length>70000&&!memcmp(sent,method,70000)&&!memcmp(sent+70000," /path HTTP/1.1\r\n",17));
    CHECK("headers and content framing follow full method without truncation",strstr(sent,"\r\nContent-Length: 0\r\nX-Fixture: long-method\r\n\r\n")!=NULL);
    http_resp_free(&response);CHECK("long actual line allocations are fully freed",live==0&&opens==1&&closes==1);reset();
    struct http_scratch work={0};url_parse(request.url,&work.url);const char*error=request_head(&request,&work);size_t head_size=work.head_length+1;CHECK("builder records exact dynamic size",error==NULL&&head_size>70000);tracked_free(work.head);
    fail_size=head_size;result=http_request(&request,&response);
    CHECK("long line OOM refuses before socket with no partial request",result<0&&strstr(response.error,"out of memory")&&opens==0&&sent_length==0&&live==0);http_resp_free(&response);reset();
    method[69999]='\n';CHECK("long token with injection suffix is not prefix accepted",!webnet_method_valid(method));result=http_request(&request,&response);
    CHECK("general HTTP line grammar also rejects long invalid suffix before connect",result<0&&strstr(response.error,"invalid HTTP method")&&opens==0&&live==0);http_resp_free(&response);method[69999]='X';reset();
    send_failure=true;result=http_request(&request,&response);http_resp_free(&response);
    CHECK("long line send failure frees owned line and closes stream",result<0&&live==0&&opens==1&&closes==1);reset();
    work=(struct http_scratch){0};url_parse(request.url,&work.url);overflow_string=method;error=request_head(&request,&work);overflow_string=NULL;
    CHECK("allocation total plus NUL overflow fails without giant allocation",error&&strstr(error,"overflow")&&work.head==NULL&&live==0);
    struct webnet_wire_request wire={.method_len=70000};size_t payload=0;
    CHECK("existing uint32 variable wire represents complete long method",webnet_wire_request_payload_size(&wire,&payload)&&payload==70000);
    free(method);printf("new HTTP long method boundaries: %u checks / %u failed\n",checks,failed);return failed?1:0;
}
