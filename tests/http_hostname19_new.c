/* New hostname/DNS representation boundaries only. Native stream and user
   memory are fixtures; this does not establish remote/browser acceptance. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <errno.h>
#include "http.h"
static size_t live, malloc_calls, last_size;
static unsigned fail_malloc;
static void *count_malloc(size_t n) {
    malloc_calls++; last_size=n;
    if(fail_malloc && !--fail_malloc)return NULL;
    void*p=malloc(n);if(p)live++;return p;
}
static void *count_calloc(size_t n,size_t s){void*p=calloc(n,s);if(p)live++;return p;}
static void *count_realloc(void *p,size_t n){void*q=realloc(p,n);if(q&&!p)live++;return q;}
static void count_free(void*p){if(p){live--;free(p);}}
#define malloc count_malloc
#define calloc count_calloc
#define realloc count_realloc
#define free count_free
#include "../user/libc/http.c"
#undef malloc
#undef calloc
#undef realloc
#undef free
static const char *user_source;
static size_t user_bytes;
static bool short_copy;
static bool user_ok(const void*p,size_t n){uintptr_t a=(uintptr_t)p,b=(uintptr_t)user_source;return a>=b&&n<=user_bytes&&a-b<=user_bytes-n;}
static int user_str(char*dst,const char*src,size_t n){if(short_copy){dst[0]=0;return 0;}for(size_t i=0;i<n;i++){if(!user_ok(src+i,1))return -EFAULT;dst[i]=src[i];if(!dst[i])return(int)i;}dst[n-1]=0;return -ENAMETOOLONG;}
#define kmalloc count_malloc
#define kfree count_free
#include "../kernel/src/net/dns_name.h"
#undef kmalloc
#undef kfree
struct net_stream {size_t at;};
static struct net_stream stream;
static char sent[2048],opened_host[512];
static size_t sent_length;
static unsigned opens,closes,checks,failed;
static bool open_failure,send_failure;
net_stream *ns_open(const char*h,uint16_t p,bool tls,int t,char*err,size_t n){(void)p;(void)tls;(void)t;opens++;snprintf(opened_host,sizeof opened_host,"%s",h);stream.at=0;if(open_failure){snprintf(err,n,"fixture open failure");return NULL;}return &stream;}
long ns_write(net_stream*s,const void*p,size_t n){(void)s;if(send_failure)return -1;if(n>sizeof sent-sent_length-1)return -1;memcpy(sent+sent_length,p,n);sent_length+=n;sent[sent_length]=0;return(long)n;}
long ns_read(net_stream*s,void*p,size_t n){const char*r="HTTP/1.1 200 OK\r\nContent-Length: 0\r\n\r\n";size_t left=strlen(r)-s->at;if(n>left)n=left;memcpy(p,r+s->at,n);s->at+=n;return(long)n;}
const char *ns_error(net_stream*s){(void)s;return "fixture I/O failure";}
void ns_close(net_stream*s){(void)s;closes++;}
void ns_set_timeout(net_stream*s,int t){(void)s;(void)t;}
int http_inflate(char*a,int b,const char*c,int d,int*e){(void)a;(void)b;(void)c;(void)d;(void)e;return -1;}
#define CHECK(label,condition) do{checks++;if(!(condition)){failed++;printf("FAIL %s\n",label);}}while(0)
static void reset(void){sent_length=opens=closes=0;sent[0]=opened_host[0]=0;open_failure=send_failure=false;fail_malloc=0;}
static void hostname(char *out,size_t length){size_t at=0;while(at<length){size_t n=length-at;if(n>63)n=63;memset(out+at,'a',n);at+=n;if(at<length)out[at++]='.';}out[length]=0;}
int main(void){
    char host[401],url[512],expected[512];hostname(host,253);
    snprintf(url,sizeof url,"https://%s:8443/deep?q=1#fragment",host);
    struct url_owned parsed={0};
    CHECK("253 byte host is parsed with exact owned storage",url_parse_owned(url,&parsed)&&strlen(parsed.host)==253&&!strcmp(parsed.host,host)&&parsed.port==8443);
    CHECK("owned transport path omits fragment without dropping query",parsed.path&&!strcmp(parsed.path,"/deep?q=1"));
    url_owned_free(&parsed);CHECK("owned parse releases host and path",!live&&!parsed.host&&!parsed.path);
    struct http_req request={.url=url};struct http_resp response;
    int r=http_request(&request,&response);
    snprintf(expected,sizeof expected,"\r\nHost: %s:8443\r\n",host);
    CHECK("real HTTP transport hands complete long host to stream",r==0&&opens==1&&!strcmp(opened_host,host));
    CHECK("native Host field has full hostname and nondefault port",strstr(sent,expected)&&!strncmp(sent,"GET /deep?q=1 HTTP/1.1\r\n",24));
    http_resp_free(&response);CHECK("long host native transfer frees ownership",live==0&&closes==1);reset();
    fail_malloc=1;r=http_request(&request,&response);
    CHECK("host allocation failure refuses before connect and frees path",r<0&&opens==0&&sent_length==0&&live==0);http_resp_free(&response);reset();
    fail_malloc=2;r=http_request(&request,&response);
    CHECK("path allocation failure preserves no leaked host or partial send",r<0&&opens==0&&live==0);http_resp_free(&response);reset();
    open_failure=true;r=http_request(&request,&response);http_resp_free(&response);
    CHECK("long host connect failure frees both URL strings and head",r<0&&opens==1&&closes==0&&live==0);reset();
    send_failure=true;r=http_request(&request,&response);http_resp_free(&response);
    CHECK("long host send failure closes and releases owned transport",r<0&&opens==1&&closes==1&&live==0);reset();
    char embedded[]="https://safe.test\0.bad.test/path";
    CHECK("explicit URL span rejects embedded NUL instead of safe prefix",!url_parse_owned_n(embedded,sizeof embedded-1,&parsed)&&!parsed.host&&!parsed.path);
    CHECK("URL span size overflow rejected before span read",!url_parse_owned_n("x",SIZE_MAX,&parsed)&&!live);
    CHECK("unsupported IPv6 authority remains fail closed",!url_parse_owned("https://[::1]/",&parsed)&&!live);
    CHECK("userinfo never becomes a native host",!url_parse_owned("https://safe.test@evil.test/",&parsed)&&!live);
    CHECK("nondecimal and overflow ports rejected without truncation",!url_parse_owned("https://safe.test:999999999999999/",&parsed)&&!url_parse_owned("https://safe.test:80x/",&parsed));
    hostname(host,400);snprintf(url,sizeof url,"https://%s/",host);
    CHECK("URL storage does not impose DNS resolution length quota",url_parse_owned(url,&parsed)&&strlen(parsed.host)==400);url_owned_free(&parsed);
    uint8_t encoded[300];size_t n=0;hostname(host,253);
    CHECK("maximum valid DNS text encodes all 255 wire octets",dns_name_encode(host,253,encoded,sizeof encoded,&n)&&n==255&&encoded[0]==63&&encoded[254]==0);
    host[253]='.';host[254]=0;
    CHECK("254 byte final-dot text is same exact DNS representation",dns_name_encode(host,254,encoded,sizeof encoded,&n)&&n==255);
    CHECK("DNS root terminator capacity participates in validation",!dns_name_encode(host,254,encoded,254,&n));
    hostname(host,254);
    CHECK("DNS total 256 wire octets rejected despite each valid label",!dns_name_encode(host,254,encoded,sizeof encoded,&n));
    memset(host,'a',64);host[64]=0;
    CHECK("DNS 64 byte individual label remains protocol error",!dns_name_encode(host,64,encoded,sizeof encoded,&n));
    CHECK("DNS embedded NUL explicit span and empty middle label rejected",!dns_name_encode("a\0b",3,encoded,sizeof encoded,&n)&&!dns_name_encode("a..b",4,encoded,sizeof encoded,&n));
    hostname(host,253);host[253]='.';host[254]=0;user_source=host;user_bytes=255;char *copied=NULL;
    r=dns_user_name_copy(host,&copied);
    CHECK("DNS syscall accepts full254 text with exact255 allocation",r==0&&copied&&!strcmp(copied,host)&&last_size==255);count_free(copied);CHECK("DNS syscall copied lifetime releases after request",live==0);
    fail_malloc=1;copied=(char*)1;r=dns_user_name_copy(host,&copied);
    CHECK("DNS allocation failure is explicit and owns nothing",r==-ENOMEM&&!copied&&live==0);fail_malloc=0;
    user_bytes=120;copied=NULL;r=dns_user_name_copy(host,&copied);
    CHECK("invalid user span rejected before allocation",r==-EFAULT&&!copied&&live==0);
    user_bytes=255;short_copy=true;r=dns_user_name_copy(host,&copied);short_copy=false;
    CHECK("user string shrink between measurement and copy frees snapshot",r==-EINVAL&&!copied&&live==0);
    memset(host,'a',255);host[255]=0;user_bytes=256;size_t before=malloc_calls;r=dns_user_name_copy(host,&copied);
    CHECK("unrepresentable DNS text does not allocate giant backing",r==-ENAMETOOLONG&&!copied&&live==0&&malloc_calls==before);
    printf("new owned hostname/DNS boundaries: %u checks / %u failed\n",checks,failed);return failed?1:0;
}
