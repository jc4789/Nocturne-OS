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
net_stream *ns_open_timed(const char *h,uint16_t p,bool tls,int t,char *e,size_t n,struct http_transport_timing *m){memset(m,0,sizeof *m);return ns_open(h,p,tls,t,e,n);}
uint64_t uptime_ms(void){return 0;}
long ns_read(net_stream *s,void *data,size_t n){abort();}
const char *ns_error(net_stream *s){abort();}
void ns_close(net_stream *s){abort();}
#include "http_mock_lifecycle.h"
void *http_br_create(void){abort();}int http_br_decode(void *s,const unsigned char **in,size_t *n,unsigned char **out,size_t *room){abort();}void http_br_free(void *s){(void)s;}
static unsigned checks,failed;
static void check(bool ok,const char *name){checks++;if(!ok){failed++;printf("FAIL %s\n",name);}}
static bool wasm_allowed(const web_doc *d,size_t *violations,size_t *reports){
    bool allowed=true;*violations=*reports=0;
    for(const struct web_html_policy_rule *r=d&&d->html_policy?d->html_policy->first:NULL;r;r=r->next){
        if(html_policy_wasm_allowed(r))continue;
        (*violations)++;if(r->report_only)(*reports)++;else allowed=false;
    }
    return allowed;
}
static void wasm_policy_cases(void){
    /* Expected values derive from CSP3 4.5.1: only script-src/default-src
     * affect byte compilation; report-only still produces a violation. */
    static const struct {const char *headers;bool allowed;size_t violations,reports;} cases[]={
        {"",true,0,0},
        {"Content-Security-Policy: object-src 'none'\r\n",true,0,0},
        {"Content-Security-Policy: script-src 'self'\r\n",false,1,0},
        {"Content-Security-Policy: default-src 'none'\r\n",false,1,0},
        {"Content-Security-Policy: script-src 'self' 'wasm-unsafe-eval'\r\n",true,0,0},
        {"Content-Security-Policy: script-src 'unsafe-eval'\r\n",true,0,0},
        {"Content-Security-Policy: SCRIPT-SRC 'UNSAFE-EVAL'\r\n",true,0,0},
        {"Content-Security-Policy: script-src 'WASM-UNSAFE-EVAL'\r\n",true,0,0},
        {"Content-Security-Policy: default-src 'wasm-unsafe-eval';script-src 'none'\r\n",false,1,0},
        {"Content-Security-Policy: default-src 'none';script-src 'wasm-unsafe-eval'\r\n",true,0,0},
        {"Content-Security-Policy: script-src 'wasm-unsafe-eval';default-src 'none'\r\n",true,0,0},
        {"Content-Security-Policy: default-src 'wasm-unsafe-eval';script-src\r\n",false,1,0},
        {"Content-Security-Policy: script-src;script-src 'wasm-unsafe-eval'\r\n",false,1,0},
        {"Content-Security-Policy: script-src 'wasm-unsafe-eval';script-src 'none'\r\n",true,0,0},
        {"Content-Security-Policy: default-src 'none';default-src 'wasm-unsafe-eval'\r\n",false,1,0},
        {"Content-Security-Policy: script-src-elem 'none';default-src 'wasm-unsafe-eval'\r\n",true,0,0},
        {"Content-Security-Policy: script-src-attr 'wasm-unsafe-eval';default-src 'none'\r\n",false,1,0},
        {"Content-Security-Policy: script-src https://source.test/'wasm-unsafe-eval'\r\n",false,1,0},
        {"Content-Security-Policy: script-src 'wasm-unsafe-eval-suffix'\r\n",false,1,0},
        {"Content-Security-Policy: script-src wasm-unsafe-eval\r\n",false,1,0},
        {"Content-Security-Policy: script-src 'wasm-unsafe-eval'\v\r\n",false,1,0},
        {"Content-Security-Policy-Report-Only: script-src 'self'\r\n",true,1,1},
        {"Content-Security-Policy: script-src 'wasm-unsafe-eval'\r\nContent-Security-Policy-Report-Only: script-src 'none'\r\n",true,1,1},
        {"Content-Security-Policy: script-src 'none'\r\nContent-Security-Policy-Report-Only: script-src 'wasm-unsafe-eval'\r\n",false,1,0},
        {"Content-Security-Policy: script-src 'wasm-unsafe-eval',script-src 'none'\r\n",false,1,0},
        {"Content-Security-Policy: script-src 'none',default-src 'none'\r\n",false,2,0},
        {"Content-Security-Policy: script-src 'wasm-unsafe-eval'\r\nContent-Security-Policy: default-src 'unsafe-eval'\r\n",true,0,0},
        {"Content-Security-Policy: default-src 'none';script-src \xc3\xa9 'wasm-unsafe-eval'\r\n",false,1,0},
        {"Content-Security-Policy: default-src 'none';script-src \xc3\xa9;script-src 'wasm-unsafe-eval'\r\n",true,0,0}
    };
    check(html_policy_wasm_allowed(NULL),"missing Wasm rule imposes no restriction");
    for(size_t i=0;i<sizeof cases/sizeof *cases;i++){
        web_doc d={.url="https://policy.test/document"},inherited={.url="about:blank"},restored={.url="https://policy.test/worker"};
        size_t violations,reports;
        check(html_policy_init(&d,cases[i].headers,NULL),"Wasm policy parse allocation");
        bool allowed=wasm_allowed(&d,&violations,&reports);
        if(allowed!=cases[i].allowed||violations!=cases[i].violations||reports!=cases[i].reports)printf("Wasm policy case %zu: %s\n",i,cases[i].headers);
        check(allowed==cases[i].allowed&&violations==cases[i].violations&&reports==cases[i].reports,"Wasm source selection, report disposition and intersection");
        check(!html_policy_requires_script(&d),"Wasm policy does not become a Trusted Types requirement");
        check(html_policy_init(&inherited,NULL,&d),"Wasm inherited policy allocation");
        check(wasm_allowed(&inherited,&violations,&reports)==cases[i].allowed&&violations==cases[i].violations&&reports==cases[i].reports,"Wasm policy inheritance preserves all rules");
        char *snapshot=html_policy_snapshot(&d);check(snapshot!=NULL,"Wasm policy snapshot allocation");
        if(snapshot){
            check(html_policy_snapshot_init(&restored,snapshot),"Wasm policy snapshot parse");
            check(wasm_allowed(&restored,&violations,&reports)==cases[i].allowed&&violations==cases[i].violations&&reports==cases[i].reports,"Wasm Worker snapshot preserves decisions and disposition");
            free(snapshot);
        }
        html_policy_free(&restored);html_policy_free(&inherited);html_policy_free(&d);
    }
    web_doc meta={.url="https://policy.test/meta"},restored={.url="https://policy.test/worker"};size_t violations,reports;
    check(html_policy_init(&meta,"Content-Security-Policy: script-src 'wasm-unsafe-eval'\r\n",NULL),"Wasm header retained without Trusted Types");
    struct web_html_policy_rule *tail=meta.html_policy?meta.html_policy->last:NULL;
    const char *content="script-src 'none';report-uri /ignored";
    check(html_policy_meta(&meta,content,strlen(content)),"Wasm meta policy parsed");
    check(!wasm_allowed(&meta,&violations,&reports)&&violations==1&&reports==0&&meta.html_policy->last->from_meta&&meta.html_policy->last->report_uri_count==0,"Wasm meta restricts and ignores report-uri");
    char *snapshot=html_policy_snapshot(&meta);
    check(snapshot&&html_policy_snapshot_init(&restored,snapshot)&&!wasm_allowed(&restored,&violations,&reports)&&restored.html_policy->last->from_meta,"Wasm snapshot preserves meta policy provenance");
    free(snapshot);html_policy_free(&restored);html_policy_rewind(&meta,tail);
    check(wasm_allowed(&meta,&violations,&reports)&&violations==0,"parser rewind removes only appended Wasm meta policy");
    html_policy_free(&meta);
}
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
    wasm_policy_cases();
    printf("frame embedding policy: %u checks, %u failures\n",checks,failed);return failed!=0;
}
