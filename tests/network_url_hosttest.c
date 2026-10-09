/* Production URL resolver/HTTP parser regression. TCP/TLS alone is substituted.
   This is not website rendering or a guest-network acceptance test. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../user/include/http.h"
extern bool web_resolve_url(const char *, const char *, char *, size_t);
struct net_stream { size_t pos; };
static struct net_stream stream;
static unsigned checks, failed, opened;
static char request[HTTP_URL_MAX + 1024];
static size_t used;
static const char response[]="HTTP/1.1 200 OK\r\nContent-Length: 2\r\n\r\nok";
#define CHECK(c) do { checks++; if (!(c)) { failed++; printf("FAIL URL:%d %s\n", __LINE__, #c); } } while (0)
net_stream *ns_open(const char *host, uint16_t port, bool tls, int timeout, char *err, size_t cap) {
    (void)timeout; (void)err; (void)cap;
    CHECK(!strcmp(host,"mail.google.com") && port==443 && tls);
    opened++; stream.pos=used=0; request[0]=0; return &stream;
}
long ns_write(net_stream *s,const void *p,size_t n) {
    (void)s; if(n>=sizeof request-used)return -1;
    memcpy(request+used,p,n); used+=n; request[used]=0; return (long)n;
}
long ns_read(net_stream *s,void *p,size_t n) {
    size_t left=sizeof response-1-s->pos; if(n>left)n=left;
    memcpy(p,response+s->pos,n);s->pos+=n;return (long)n;
}
const char *ns_error(net_stream *s){(void)s;return "test IO";}
void ns_close(net_stream *s){(void)s;}
int http_inflate(char *o,int cap,const char *p,int n,int *consumed) {
    (void)o;(void)cap;(void)p;(void)n;(void)consumed;return -1;
}
int main(void) {
    char *input=malloc(HTTP_URL_MAX+1), *out=malloc(HTTP_URL_MAX), *expected=malloc(HTTP_URL_MAX);
    struct url *parsed=malloc(sizeof *parsed);
    CHECK(input&&out&&expected&&parsed); if(!input||!out||!expected||!parsed)return 1;
    static const size_t sizes[]={2136,4095,8191,HTTP_URL_MAX-1};
    const char *prefix="https://mail.google.com/_/scs/mail-static/_/js/k=";
    size_t pre=strlen(prefix);
    for(unsigned i=0;i<sizeof sizes/sizeof *sizes;i++) {
        size_t len=sizes[i];strcpy(input,prefix);memset(input+pre,'x',len-pre);input[len]=0;
        CHECK(web_resolve_url("https://mail.google.com/mail/u/0/",input,out,HTTP_URL_MAX));
        CHECK(strlen(out)==len&&!strcmp(out,input));
        CHECK(url_parse(input,parsed));CHECK(strlen(parsed->path)==len-strlen("https://mail.google.com"));
        struct http_req q={.url=input};struct http_resp r;
        CHECK(http_request(&q,&r)==0&&r.status==200&&r.body_len==2);
        size_t pathlen=strlen(parsed->path);
        CHECK(used>pathlen+4&&!memcmp(request,"GET ",4)&&!memcmp(request+4,parsed->path,pathlen)&&
              !memcmp(request+4+pathlen," HTTP/1.1\r\n",11));
        http_resp_free(&r);
    }
    strcpy(input,"https://mail.google.com/path?key=");pre=strlen(input);
    memset(input+pre,'q',6000);input[pre+6000]=0;
    CHECK(web_resolve_url("https://mail.google.com/",input,out,HTTP_URL_MAX)&&!strcmp(out,input));
    CHECK(web_resolve_url(input,"?new=preserved",out,HTTP_URL_MAX)&&!strcmp(out,"https://mail.google.com/path?new=preserved"));
    strcpy(input,"https://mail.google.com/a/b?old=1");
    CHECK(web_resolve_url(input,"../c?new=2#done",out,HTTP_URL_MAX)&&!strcmp(out,"https://mail.google.com/c?new=2#done"));
    strcpy(input,"https://mail.google.com/a/../b");
    CHECK(web_resolve_url("https://mail.google.com/",input,input,HTTP_URL_MAX)&&!strcmp(input,"https://mail.google.com/b"));
    strcpy(input,prefix);pre=strlen(prefix);memset(input+pre,'x',HTTP_URL_MAX-pre);input[HTTP_URL_MAX]=0;
    strcpy(out,"unchanged");CHECK(!web_resolve_url("https://mail.google.com/",input,out,HTTP_URL_MAX)&&!out[0]);
    CHECK(!url_parse(input,parsed));unsigned before=opened;
    struct http_req q={.url=input};struct http_resp r;
    CHECK(http_request(&q,&r)<0&&r.error[0]&&opened==before);http_resp_free(&r);
    strcpy(out,"unchanged");CHECK(!web_resolve_url("https://mail.google.com/","/too-long",out,8)&&!out[0]);
    CHECK(web_resolve_url("https://mail.google.com/","data:text/plain,hello",out,HTTP_URL_MAX)&&!strcmp(out,"data:text/plain,hello"));
    strcpy(out,"unchanged");CHECK(!web_resolve_url("https://mail.google.com/","data:text/plain,hello",out,8)&&!out[0]);
    /* Two individually bounded paths must not truncate at the merge seam. */
    strcpy(input,"https://mail.google.com/");pre=strlen(input);memset(input+pre,'a',9000);input[pre+9000]='/';input[pre+9001]=0;
    memset(expected,'b',9000);expected[9000]=0;
    CHECK(!web_resolve_url(input,expected,out,HTTP_URL_MAX)&&!out[0]);
    free(input);free(out);free(expected);free(parsed);
    printf("network_url_hosttest: %u checks, %u failed\n",checks,failed);
    return failed!=0;
}
