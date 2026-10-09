/* Synthetic transport limits, not live website or guest execution acceptance. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "http.h"
#include "webnet_wire.h"
struct run { unsigned count, byte; };
#include "http_body_limit_fixture.h"
struct net_stream { const unsigned char *p; size_t n, at; };
static struct net_stream wire;
static unsigned checks, failed, opens, callbacks;
static size_t delivered;
#define CHECK(c) do { checks++; if (!(c)) { failed++; printf("FAIL HTTP body:%d %s\n",__LINE__,#c); } } while (0)
net_stream *ns_open(const char *host,uint16_t port,bool tls,int timeout,char *err,size_t cap) {
    (void)host;(void)port;(void)tls;(void)timeout;(void)err;(void)cap;wire.at=0;opens++;return &wire;
}
long ns_write(net_stream *s,const void *p,size_t n){(void)s;(void)p;return (long)n;}
long ns_read(net_stream *s,void *p,size_t n){size_t left=s->n-s->at;if(n>left)n=left;memcpy(p,s->p+s->at,n);s->at+=n;return (long)n;}
void ns_close(net_stream *s){(void)s;}
const char *ns_error(net_stream *s){(void)s;return "";}
static int count_body(void *ctx,const char *p,size_t n) {
    (void)ctx;callbacks++;delivered+=n;
    CHECK(!n || (p[0]=='x'&&p[n-1]=='x'));return 0;
}
static void encoded(const struct run *runs,size_t count,size_t limit,bool legacy,bool ok,size_t expected) {
    size_t n=0;for(size_t i=0;i<count;i++)n+=runs[i].count;
    unsigned char *p=malloc(n+256);CHECK(p!=NULL);if(!p)return;
    int h=sprintf((char*)p,"HTTP/1.1 200 OK\r\nContent-Encoding: gzip\r\nContent-Length: %lu\r\n\r\n",(unsigned long)n);
    size_t at=(size_t)h;for(size_t i=0;i<count;i++){memset(p+at,runs[i].byte,runs[i].count);at+=runs[i].count;}
    wire=(struct net_stream){p,at,0};struct http_req q={.url="https://body-limit.test/",.on_body=count_body};struct http_resp r;
    callbacks=0;delivered=0;
    int result=legacy?http_request(&q,&r):http_request_limited(&q,&r,limit);
    CHECK((result==0)==ok);
    if(ok){CHECK(r.body_len==expected&&delivered==expected&&callbacks==1);}
    else {CHECK(r.error[0]&&callbacks==0&&delivered==0);}
    http_resp_free(&r);free(p);
}
static void plain(size_t limit,size_t size,bool cb,bool ok) {
    unsigned char *p=malloc(size+128);CHECK(p!=NULL);if(!p)return;
    int h=sprintf((char*)p,"HTTP/1.1 200 OK\r\nContent-Length: %lu\r\n\r\n",(unsigned long)size);
    memset(p+h,'x',size);wire=(struct net_stream){p,(size_t)h+size,0};
    struct http_req q={.url="https://body-limit.test/",.on_body=cb?count_body:NULL};struct http_resp r;
    callbacks=0;delivered=0;int result=http_request_limited(&q,&r,limit);
    CHECK((result==0)==ok);if(ok)CHECK(r.body_len==size&&(!cb||delivered==size));else CHECK(r.error[0]);
    http_resp_free(&r);free(p);
}
#define GZIP(a,l,old,ok,n) encoded(a,sizeof a/sizeof *a,l,old,ok,n)
int main(void) {
    CHECK(sizeof(struct webnet_wire_request)==64&&sizeof(struct webnet_wire_response)==48);
    for(unsigned kind=WEBNET_NAVIGATION;kind<=WEBNET_FETCH;kind++) {
        bool script=kind==WEBNET_CLASSIC||kind==WEBNET_MODULE;
        CHECK(webnet_response_limit((enum webnet_kind)kind)==(script?WEBNET_SCRIPT_BODY_LIMIT:WEBNET_BODY_LIMIT));
        CHECK(webnet_wire_response_limit(kind,0)==WEBNET_BODY_LIMIT);
        CHECK(webnet_wire_script_flag_valid(kind,WEBNET_WIRE_LARGE_SCRIPT)==script);
        CHECK(webnet_wire_response_limit(kind,WEBNET_WIRE_LARGE_SCRIPT)==(script?WEBNET_SCRIPT_BODY_LIMIT:WEBNET_BODY_LIMIT));
    }
    GZIP(over16,WEBNET_SCRIPT_BODY_LIMIT,true,false,0);
    GZIP(over16,WEBNET_BODY_LIMIT,false,false,0);
    GZIP(over16,WEBNET_SCRIPT_BODY_LIMIT,false,true,WEBNET_BODY_LIMIT+1);
    GZIP(exact32,WEBNET_SCRIPT_BODY_LIMIT,false,true,WEBNET_SCRIPT_BODY_LIMIT);
    GZIP(over32,WEBNET_SCRIPT_BODY_LIMIT,false,false,0);
    /* A small explicit bound also exercises non-power-of-two growth/clamping. */
    plain(31,31,false,true);plain(31,32,false,false);plain(31,31,true,true);plain(31,32,true,false);
    unsigned before=opens;struct http_req q={.url="https://body-limit.test/"};struct http_resp r;
    CHECK(http_request_limited(&q,&r,0)<0&&r.error[0]&&opens==before);http_resp_free(&r);
    CHECK(http_request_limited(&q,&r,HTTP_RESPONSE_BODY_MAX+1)<0&&r.error[0]&&opens==before);http_resp_free(&r);
    printf("http_body_limit_hosttest: %u checks, %u failed\n",checks,failed);return failed!=0;
}
