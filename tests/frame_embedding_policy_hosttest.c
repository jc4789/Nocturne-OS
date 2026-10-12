/* Real html_policy.c and HTTP URL parser; no author JS or site exceptions.
 * The effective-URL accessor is the same inherited-origin boundary as frame.c. */
#include <stdio.h>
#include "html_policy.h"
#include "frame.h"
const char *web_effective_url(web_doc *d) { return d && d->inherited_url ? d->inherited_url : d ? d->url : "about:blank"; }
/* Networking is never part of this focused header check. The product HTTP
 * translation unit is linked whole to use its actual owned URL parser. */
net_stream *ns_open(const char *host,uint16_t port,bool tls,int timeout,char *error,size_t cap){abort();}
long ns_write(net_stream *s,const void *data,size_t n){abort();}
long ns_read(net_stream *s,void *data,size_t n){abort();}
const char *ns_error(net_stream *s){abort();}
void ns_close(net_stream *s){abort();}
int http_inflate(char *out,int cap,const char *in,int len,int *consumed){abort();}
static unsigned checks,failed;
static void check(bool ok,const char *name){checks++;if(!ok){failed++;printf("FAIL %s\n",name);}}
int main(int argc,char **argv){
    web_doc parent={.url="https://embed.test/page"},top={.url="https://other.test/root"};uint32_t flags=0;
    const char *url="https://embed.test/frame";
#define ALLOW(h) html_policy_embedding_allowed(&parent,h,url,&flags)
    check(ALLOW("Content-Security-Policy: frame-ancestors 'self' https://other.test\r\nX-Frame-Options: SAMEORIGIN\r\n"),"self is allowed instead of all CSP being blocked");
    check(!ALLOW("Content-Security-Policy: frame-ancestors https://deny.test\r\n"),"unlisted ancestor blocked");
    check(!ALLOW("Content-Security-Policy: frame-ancestors 'none'\r\n"),"none blocked");
    check(!ALLOW("Content-Security-Policy: frame-ancestors\r\n"),"empty source list blocked");
    parent.frame_parent=&top;
    check(!ALLOW("Content-Security-Policy: frame-ancestors 'self'\r\n"),"all ancestors checked");
    check(ALLOW("Content-Security-Policy: frame-ancestors 'self' https://other.test\r\nX-Frame-Options: DENY\r\n"),"enforced ancestors overrides XFO");
    check(!ALLOW("X-Frame-Options: SAMEORIGIN\r\n"),"XFO checks all ancestors");parent.frame_parent=NULL;
    check(ALLOW("X-Frame-Options: sameorigin, SAMEORIGIN\r\n"),"identical XFO values allowed");
    check(!ALLOW("X-Frame-Options: SAMEORIGIN,\r\n"),"mixed empty XFO value blocked");
    check(ALLOW("X-Frame-Options: SAMEORIGINISH\r\n"),"XFO token matching not prefix matching");
    check(ALLOW("X-Frame-Options: invalid, invalid\r\n"),"unknown XFO list ignored");
    check(!ALLOW("X-Frame-Options: DENY\r\nContent-Security-Policy-Report-Only: frame-ancestors *\r\n"),"report-only does not override enforcement");
    check(ALLOW("Content-Security-Policy: frame-ancestors 'self'; frame-ancestors 'none'\r\n"),"first duplicate directive wins");
    check(!ALLOW("Content-Security-Policy: frame-ancestors 'self', frame-ancestors 'none'\r\n"),"policy list intersected");
    check(ALLOW("Content-Security-Policy: report-uri /frame-ancestors; default-src 'none'\r\n"),"no substring false positive or default-src fallback");
    parent.url="https://sub.embed.test/path";
    check(ALLOW("Content-Security-Policy: frame-ancestors https://*.embed.test\r\n"),"wildcard subdomain matched");
    parent.url="https://embed.test/path";
    check(!ALLOW("Content-Security-Policy: frame-ancestors https://*.embed.test\r\n"),"wildcard cannot match bare domain");
    parent.url="https://embed.test:8443/path";
    check(ALLOW("Content-Security-Policy: frame-ancestors https://embed.test:*\r\n"),"wildcard port matched");
    check(!ALLOW("Content-Security-Policy: frame-ancestors https://embed.test\r\n"),"default port not arbitrary port");
    parent.url="https://embed.test/path";url="http://embed.test/frame";
    check(ALLOW("Content-Security-Policy: frame-ancestors 'self'\r\n"),"self permits secure default-port upgrade");
    parent.url="http://embed.test/path";url="https://embed.test/frame";
    check(!ALLOW("Content-Security-Policy: frame-ancestors 'self'\r\n"),"self does not permit insecure downgrade");
    parent.url="https://embed.test/path";
    check(ALLOW("Content-Security-Policy: frame-ancestors https://embed.test/\r\n"),"origin path slash matched");
    check(!ALLOW("Content-Security-Policy: frame-ancestors https://embed.test/path\r\n"),"ancestor origin is not document path");
    check(ALLOW("Content-Security-Policy: sandbox allow-scripts allow-same-origin allow-forms\r\n"),"CSP sandbox parsed rather than unconditional reject");
    check(!(flags&(SB_SCRIPTS|SB_ORIGIN|SB_FORMS)) && (flags&SB_TOP),"sandbox grants only declared abilities");
    check(ALLOW("Content-Security-Policy: sandbox allow-scripts allow-same-origin allow-forms, sandbox allow-scripts allow-same-origin\r\n") && (flags&SB_FORMS),"sandbox restrictions union across policies");
    parent.sandbox_flags=SB_ORIGIN;
    check(!ALLOW("Content-Security-Policy: frame-ancestors *\r\n"),"opaque ancestor does not become HTTP origin");parent.sandbox_flags=0;
    if(argc==4){
        FILE *f=fopen(argv[1],"rb");check(f!=NULL,"real response headers present");
        if(f){fseek(f,0,SEEK_END);long n=ftell(f);rewind(f);char *headers=n>=0?malloc((size_t)n+1):NULL;
            if(headers && fread(headers,1,(size_t)n,f)==(size_t)n){headers[n]=0;parent.url=argv[2];url=argv[3];check(ALLOW(headers),"real response embedding policy");}
            else check(false,"real response headers readable");free(headers);fclose(f);}
    }
    printf("frame embedding policy: %u checks, %u failures\n",checks,failed);return failed!=0;
}
