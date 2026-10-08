/* Real media reader/parser/allocator; only TCP/TLS is simulated. Not a guest
 * decode/network playback test. Focused anonymous browser policy boundaries. */
#include <stdio.h>
#include <stdint.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include "../user/include/http.h"
#include "../user/include/media_alloc_private.h"
#include "../user/libc/media_http_private.h"
enum scenario { EXACT, WILDCARD, WRONG_ORIGIN, DUP_ORIGIN, PREFLIGHT_DENIED,
    REFILL_DENIED, REDIRECT, PREFLIGHT_REDIRECT, NO_IF_RANGE, GZIP };
static enum scenario scenario;
static unsigned checks,failed,opened,closed,get_requests,options,inflated;
struct net_stream { char request[4096]; size_t used,pos,length; bool built; };
static struct net_stream stream;
static unsigned char response[NMEDIA_HTTP_CACHE_BYTES+4096];
#define CHECK(x) do {checks++;if(!(x)){failed++;printf("FAIL cors:%d %s\n",__LINE__,#x);}}while(0)
static void build(net_stream *s) {
    bool preflight=!strncmp(s->request,"OPTIONS ",8);
    CHECK(strstr(s->request,"Origin: http://media.test\r\n")||strstr(s->request,"Origin: http://cdn.test\r\n"));
    CHECK(!strstr(s->request,"Cookie:")&&!strstr(s->request,"Authorization:"));
    const char *allow=scenario==WILDCARD?"*":
        (scenario==WRONG_ORIGIN||(scenario==PREFLIGHT_DENIED&&preflight))?"http://wrong.test":"http://media.test";
    size_t at=0;
    if(preflight) {
        options++;
        CHECK(strstr(s->request,"Access-Control-Request-Method: GET\r\n")!=NULL);
        CHECK(strstr(s->request,"Access-Control-Request-Headers: range")!=NULL);
        const char *names=scenario==NO_IF_RANGE?"range":"range, if-range";
        at=(size_t)snprintf((char *)response,sizeof response,
            "HTTP/1.1 %d Test\r\nAccess-Control-Allow-Origin: %s\r\n"
            "Access-Control-Allow-Methods: GET\r\nAccess-Control-Allow-Headers: %s\r\n%s\r\n",
            scenario==PREFLIGHT_REDIRECT?302:204,allow,names,
            scenario==PREFLIGHT_REDIRECT?"Location: http://cdn.test/elsewhere\r\n":"");
    } else {
        get_requests++;long long first=0,last=0;
        const char *range=strstr(s->request,"Range: bytes=");
        CHECK(range&&sscanf(range,"Range: bytes=%lld-%lld",&first,&last)==2);
        long long total=(long long)NMEDIA_HTTP_CACHE_BYTES*2+17;
        if(last>=total)last=total-1;
        size_t bytes=(size_t)(last-first+1);
        if(scenario==REFILL_DENIED&&first>0)allow="http://wrong.test";
        if(scenario==REDIRECT) {
            at=(size_t)snprintf((char *)response,sizeof response,
                "HTTP/1.1 302 Test\r\nLocation: http://cdn.test/elsewhere\r\n"
                "Access-Control-Allow-Origin: %s\r\nContent-Length: 0\r\n\r\n",allow);
        } else {
            at=(size_t)snprintf((char *)response,sizeof response,
                "HTTP/1.1 206 Test\r\nContent-Range: bytes %lld-%lld/%lld\r\n"
                "Content-Length: %zu\r\nETag: \"clip-v1\"\r\nAccess-Control-Allow-Origin: %s\r\n%s%s\r\n",
                first,last,total,bytes,allow,
                scenario==DUP_ORIGIN?"Access-Control-Allow-Origin: *\r\n":"",
                scenario==GZIP?"Content-Encoding: gzip\r\n":"");
            CHECK(at+bytes<=sizeof response);
            for(size_t i=0;i<bytes;i++)response[at+i]=(unsigned char)(first+(long long)i);
            at+=bytes;
        }
    }
    s->length=at;s->built=true;
}
net_stream *ns_open(const char *host,uint16_t port,bool tls,int timeout,char *err,size_t n) {
    (void)err;(void)n;CHECK(!strcmp(host,"cdn.test")&&port==80&&!tls&&timeout==5000);
    opened++;memset(&stream,0,sizeof stream);return &stream;
}
long ns_write(net_stream *s,const void *bytes,size_t n) {
    if(n>=sizeof s->request-s->used)return -1;
    memcpy(s->request+s->used,bytes,n);s->used+=n;s->request[s->used]=0;return (long)n;
}
long ns_read(net_stream *s,void *out,size_t n) {
    if(!s->built)build(s);size_t take=s->length-s->pos;if(take>n)take=n;
    memcpy(out,response+s->pos,take);s->pos+=take;return (long)take;
}
const char *ns_error(net_stream *s){(void)s;return "";}
void ns_close(net_stream *s){(void)s;closed++;}
int http_inflate(char *out,int cap,const char *in,int len,int *used) {
    (void)out;(void)cap;(void)in;(void)len;(void)used;inflated++;return -1;
}
static void clean(nmedia_http *r) {
    nmedia_http_close(r);struct nmedia_alloc_stats a;nmedia_alloc_snapshot(&a);
    CHECK(a.current==0&&a.blocks==0);CHECK(opened==closed);
}
static nmedia_http *start(enum scenario s) {
    scenario=s;get_requests=options=0;char error[160];
    return nmedia_http_open_cors("http://cdn.test/video","http://media.test/page#fragment",error,sizeof error);
}
int main(void) {
    unsigned char bytes[16];
    for(int i=0;i<2;i++) {
        nmedia_http *r=start(i?WILDCARD:EXACT);CHECK(r&&get_requests==1&&options==1);
        if(r) {
            CHECK(nmedia_http_read(r,bytes,16)==16&&bytes[15]==15);
            CHECK(nmedia_http_seek(r,NMEDIA_HTTP_CACHE_BYTES+7,SEEK_SET)>=0);
            CHECK(nmedia_http_read(r,bytes,16)==16&&bytes[0]==7&&get_requests==2&&options==2);
            CHECK(strstr(stream.request,"If-Range: \"clip-v1\"\r\n")!=NULL);
        }clean(r);
    }
    enum scenario bad[]={WRONG_ORIGIN,DUP_ORIGIN,PREFLIGHT_DENIED,REDIRECT,PREFLIGHT_REDIRECT,GZIP};
    for(size_t i=0;i<sizeof bad/sizeof bad[0];i++) {
        nmedia_http *r=start(bad[i]);CHECK(!r);
        if(bad[i]==PREFLIGHT_DENIED||bad[i]==PREFLIGHT_REDIRECT)CHECK(get_requests==0);
        if(bad[i]==REDIRECT)CHECK(get_requests==1); /* target is never issued */
        clean(r);
    }
    for(int i=0;i<2;i++) {
        nmedia_http *r=start(i?NO_IF_RANGE:REFILL_DENIED);CHECK(r!=NULL);
        if(r){CHECK(nmedia_http_seek(r,NMEDIA_HTTP_CACHE_BYTES,SEEK_SET)>=0);CHECK(nmedia_http_read(r,bytes,16)<0);}
        if(i)CHECK(get_requests==1); /* missing If-Range permission blocks GET */
        clean(r);
    }
    scenario=EXACT;get_requests=options=0;char error[160];
    nmedia_http *r=nmedia_http_open_cors("http://cdn.test/video","HTTP://CDN.TEST:80/page#x",error,sizeof error);
    CHECK(r&&get_requests==1&&options==0);clean(r); /* canonical same-origin, no OPTIONS */
    unsigned before=opened;
    const char *invalid[]={"https://media.test/page","file:///home/page","about:blank","http://user@media.test/","http://media.test/a\r\nx"};
    for(size_t i=0;i<sizeof invalid/sizeof invalid[0];i++) {
        r=nmedia_http_open_cors("http://cdn.test/video",invalid[i],error,sizeof error);CHECK(!r);clean(r);
    }
    CHECK(opened==before&&inflated==0);
    /* Current HTTP host capacity is 128 bytes: exact maximum is accepted,
     * one beyond is rejected before IO. Not a reachable raw[160] overflow. */
    char longest[160];memcpy(longest,"https://",8);memset(longest+8,'a',127);
    memcpy(longest+135,":65535/",8);
    CHECK(nmedia_http_browser_url(longest,"https://media.test/page"));
    memset(longest+8,'a',128);memcpy(longest+136,":65535/",8);
    CHECK(!nmedia_http_browser_url(longest,"https://media.test/page"));
    CHECK(opened==before);
    printf("media_http_cors_host: %u checks, %u failed; simulated TCP/TLS, real parser\n",checks,failed);
    return failed?1:0;
}
