/* 新21の16KiB超 owned spanだけ。実HTTP source＋匿名stream fixture。 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include "../user/include/http.h"
static size_t live;
static unsigned fail_malloc;
static void *count_malloc(size_t n){if(fail_malloc&&!--fail_malloc)return NULL;void *p=malloc(n);if(p)live++;return p;}
static void *count_calloc(size_t n,size_t s){void *p=calloc(n,s);if(p)live++;return p;}
static void *count_realloc(void *p,size_t n){void *q=realloc(p,n);if(q&&!p)live++;return q;}
static void count_free(void *p){if(p){live--;free(p);}}
#define malloc count_malloc
#define calloc count_calloc
#define realloc count_realloc
#define free count_free
#include "../user/libc/http.c"
#undef malloc
#undef calloc
#undef realloc
#undef free
struct net_stream {size_t received;};
static struct net_stream stream;
static const char response[]="HTTP/1.1 200 OK\r\nContent-Length: 0\r\n\r\n";
static char *expected_path;
static unsigned requested,exact_path,checks,failed;
net_stream *ns_open(const char *h,uint16_t p,bool tls,int t,char *e,size_t n){(void)t;(void)e;(void)n;if(strcmp(h,"long.test")||p!=443||!tls)return NULL;stream.received=0;requested++;return &stream;}
long ns_write(net_stream *s,const void *b,size_t n){(void)s;const char *p=b;size_t pn=strlen(expected_path);if(n>=pn+15&&!memcmp(p,"GET ",4)&&!memcmp(p+4,expected_path,pn)&&!memcmp(p+4+pn," HTTP/1.1\r\n",11))exact_path++;return (long)n;}
long ns_read(net_stream *s,void *b,size_t n){size_t left=sizeof response-1-s->received;if(n>left)n=left;memcpy(b,response+s->received,n);s->received+=n;return (long)n;}
void ns_set_timeout(net_stream *s,int t){(void)s;(void)t;}
const char *ns_error(net_stream *s){(void)s;return "";}
void ns_close(net_stream *s){(void)s;}
int http_inflate(char *o,int c,const char *i,int n,int *used){(void)o;(void)c;(void)i;(void)n;(void)used;return -1;}
#define CHECK(name,ok) do{checks++;if(!(ok)){failed++;printf("FAIL %s\n",name);}}while(0)
int main(void){
    const char prefix[]="HTTPS://LONG.test:443/?p=";
    size_t pn=sizeof prefix-1,tail=65537,n=pn+tail;
    char *raw=malloc(n+1);if(!raw)return 2;memcpy(raw,prefix,pn);memset(raw+pn,'x',tail);raw[n]=0;
    struct url_owned u;
    CHECK("new-public-enum-accepts-exact-64KiB-plus-span",url_parse_owned_result_n(raw,n,&u)==HTTP_URL_TUPLE&&strlen(u.path)==tail+4&&!strcmp(u.host,"long.test"));
    if(!u.path||!u.host){free(raw);return 3;}expected_path=u.path;
    char *canonical=NULL,*origin=NULL;
    CHECK("new-long-canonical-full-owned-path",http_canonical_owned_n(raw,n,&canonical,&origin)==HTTP_URL_TUPLE&&canonical&&strlen(canonical)==strlen("https://long.test")+strlen(u.path)&&origin&&!strcmp(origin,"https://long.test")&&!strcmp(canonical+strlen(origin),u.path));
    count_free(canonical);count_free(origin);
    struct url *legacy=malloc(sizeof *legacy);CHECK("legacy-fixed-ABI-still-rejects-new-long-target",legacy&&!url_parse(raw,legacy));free(legacy);
    struct http_req request={.url=raw};struct http_resp result={0};
    CHECK("real-http-owned-transport-writes-complete-long-request-target",http_request(&request,&result)==0&&result.status==200&&requested==1&&exact_path==1);http_resp_free(&result);
    url_owned_free(&u);expected_path=NULL;CHECK("long-canonical-and-transport-release-all-new-ownership",live==0);
    fail_malloc=1;CHECK("new-public-long-span-oom-host",url_parse_owned_result_n(raw,n,&u)==HTTP_URL_OOM&&!u.host&&!u.path&&!live);
    fail_malloc=2;CHECK("new-public-long-span-oom-path-rolls-back-host",url_parse_owned_result_n(raw,n,&u)==HTTP_URL_OOM&&!u.host&&!u.path&&!live);fail_malloc=0;
    raw[18000]=0;CHECK("embedded-nul-beyond-old-cap-not-prefix-success",url_parse_owned_result_n(raw,n,&u)==HTTP_URL_INVALID&&!u.host&&!u.path&&!live);raw[18000]='x';
    CHECK("oversize-representation-rejected-before-span-read",url_parse_owned_result_n((const char *)1,(size_t)UINT32_MAX+1,&u)==HTTP_URL_INVALID&&!u.host&&!u.path);
    CHECK("canonical-overflow-rejected-before-span-read",http_canonical_owned_n((const char *)1,SIZE_MAX,&canonical,&origin)==HTTP_URL_INVALID&&!canonical&&!origin&&!live);
    free(raw);printf("http-owned-span21-new: %u checks, %u failed\n",checks,failed);return failed?1:0;
}
