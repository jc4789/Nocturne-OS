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
net_stream *ns_open(const char *h,uint16_t p,bool tls,int t,char *e,size_t n) {
    (void)h;(void)p;(void)tls;(void)t;(void)e;(void)n;stream.at=0;return &stream;
}
long ns_write(net_stream *s,const void *p,size_t n) {
    (void)s;writes++;if(short_write)return n?(long)n-1:0;
    if(n>sizeof sent-sent_n-1)return -1;
    memcpy(sent+sent_n,p,n);sent_n+=n;sent[sent_n]=0;return (long)n;
}
long ns_read(net_stream *s,void *p,size_t n) {
    const char *r="HTTP/1.1 200 OK\r\nContent-Length: 0\r\n\r\n";
    size_t left=strlen(r)-s->at;if(n>left)n=left;
    memcpy(p,r+s->at,n);s->at+=n;return (long)n;
}
void ns_close(net_stream *s){(void)s;}
void ns_set_timeout(net_stream *s,int t){(void)s;(void)t;}
const char *ns_error(net_stream *s){(void)s;return "synthetic short write";}
int http_inflate(char *a,int b,const char *c,int d,int *e){(void)a;(void)b;(void)c;(void)d;(void)e;return -1;}
#define CHECK(c) do{checks++;if(!(c)){failed++;printf("FAIL HTTP batch line %d\n",__LINE__);}}while(0)
int main(void) {
    struct http_req q={.url="https://headers.invalid/path?q=1",.headers="X-Test: one\r\n"};
    struct http_resp r;
    CHECK(http_request(&q,&r)==0);CHECK(writes==1);
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
