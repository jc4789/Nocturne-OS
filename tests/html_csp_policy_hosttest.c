/* Content Security Policy Level 3 on the host: the product html_policy.c with
 * the product WHATWG URL parser (web_url.c over Lexbor) and BearSSL's SHA-2.
 * Expected values come from the CSP3 algorithms (§2.2.1, §4, §6.7, §6.8) and
 * the WPT content-security-policy expectations named beside each case. This
 * is not guest delivery, real-site acceptance or a WPT run.
 * Built and run by tests/check_html_csp_policy.py. */
#include <stdio.h>
#include "html_policy.h"
#include "frame.h"
#include <webnet.h>
const char *web_effective_url(web_doc *d) { return d && d->inherited_url ? d->inherited_url : d ? d->url : "about:blank"; }
bool web_frame_same_origin(web_doc *a,web_doc *b){(void)a;(void)b;return true;}
/* The URL parser's lending pool locks; one thread runs these checks. */
int *__errno_location(void){static int e;return &e;}
int mtx_lock(mtx_t *m){(void)m;return 0;}
int mtx_unlock(mtx_t *m){(void)m;return 0;}

static unsigned checks,failed;
static void check(bool ok,const char *name){checks++;if(!ok){failed++;printf("FAIL %s\n",name);}}

/* The violations a check reported, as "directive resource sample disposition". */
struct seen { unsigned count; char last[512]; };
static void reporter(void *opaque,const struct html_csp_violation *v){
    struct seen *s=opaque;s->count++;
    snprintf(s->last,sizeof s->last,"%s %s %.*s%s%s",v->directive,v->resource,(int)v->sample_length,v->sample?v->sample:"",
             v->sample_length?" ":"",v->policy && v->policy->report_only?"report":"enforce");
}
static web_doc *document(const char *url,const char *headers){
    web_doc *d=calloc(1,sizeof *d);d->url=strdup(url);
    if(!html_policy_init(d,headers,NULL)){printf("FAIL allocation\n");exit(1);}
    return d;
}
static void release(web_doc *d){html_policy_free(d);free(d->url);free((void *)d->inherited_url);free(d);}
static const char *page="http://web-platform.test:8000/content-security-policy/page.html";
static char header[2048];
static const char *csp(const char *policy){snprintf(header,sizeof header,"Content-Security-Policy: %s\r\n",policy);return header;}
/* One request check against one enforced policy on the page above. */
static bool fetch_allowed(const char *policy,int destination,const char *url,const char *nonce,bool parser,unsigned redirects,struct seen *seen){
    web_doc *d=document(page,csp(policy));
    struct html_csp_request r={.url=url,.destination=destination,.nonce=nonce,.parser_inserted=parser,.redirect_count=redirects};
    struct seen local={0};
    bool allowed=html_policy_request_allowed(d,&r,reporter,seen?seen:&local);
    release(d);return allowed;
}
#define SCRIPT WEBNET_DEST_SCRIPT
static void parsing(void){
    web_doc *d=document(page,"Content-Security-Policy: SCRIPT-SRC 'self'; script-src 'none'; ; img-src\r\n");
    struct web_html_policy_rule *r=d->html_policy?d->html_policy->first:NULL;
    check(r && r->directive_count==2,"§2.2.1 empty token skipped, repeated name ignored");
    check(r && !strcmp(r->directives[0].name,"script-src") && r->directives[0].count==1 && !strcmp(r->directives[0].value[0],"'self'"),
          "§2.2.1 name lowercased, first value kept");
    check(r && !strcmp(r->directives[1].name,"img-src") && r->directives[1].count==0,"§2.2.1 a directive may have an empty value");
    check(d->html_policy && d->html_policy->self_url && !strcmp(d->html_policy->self_url,page),"§2.2.2 self-origin is the response URL");
    release(d);
    d=document(page,"Content-Security-Policy: ;  ;\r\nContent-Security-Policy: script-src 'self', img-src 'none'\r\n");
    unsigned n=0;for(r=d->html_policy?d->html_policy->first:NULL;r;r=r->next)n++;
    check(n==2,"§2.2.2 empty policies are dropped and a header list is split on commas");
    release(d);
    d=document(page,"Content-Security-Policy: script-src \xc3\xa9 'none'; script-src 'self'\r\n");
    r=d->html_policy?d->html_policy->first:NULL;
    check(r && r->directive_count==1 && !strcmp(r->directives[0].value[0],"'self'"),"§2.2.1 a non-ASCII token is skipped before duplicate handling");
    release(d);
    d=document(page,NULL);
    const char meta[]="report-uri /r; sandbox; frame-ancestors 'none'; script-src 'self', img-src 'none'";
    check(html_policy_meta(d,meta,strlen(meta)),"meta parse");
    r=d->html_policy?d->html_policy->first:NULL;
    check(r && r->next==NULL && r->directive_count==1 && r->from_meta && !r->report_uri_count,
          "HTML http-equiv: report-uri, sandbox and frame-ancestors removed; one policy, commas are not list separators");
    check(r && r->directives[0].count==3 && !strcmp(r->directives[0].value[0],"'self',"),"meta value keeps the comma inside a token");
    check(d->html_policy && d->html_policy->self_url && !strcmp(d->html_policy->self_url,page),"meta policy self-origin is the document's");
    release(d);
}
static void url_matching(void){
    /* §6.7.2.8 'self', with its secure upgrades on the same port. WPT gen/top.meta/script-src-self. */
    check(fetch_allowed("script-src 'self'",SCRIPT,"http://web-platform.test:8000/a.js",NULL,true,0,NULL),"'self' same origin");
    check(!fetch_allowed("script-src 'self'",SCRIPT,"http://www1.web-platform.test:8000/a.js",NULL,true,0,NULL),"'self' other host");
    check(fetch_allowed("script-src 'self'",SCRIPT,"https://web-platform.test:8000/a.js",NULL,true,0,NULL),"'self' upgrades http to https on the same port");
    check(!fetch_allowed("script-src 'self'",SCRIPT,"https://web-platform.test:8443/a.js",NULL,true,0,NULL),"'self' upgrade needs the same or default ports");
    check(!fetch_allowed("script-src 'self'",SCRIPT,"blob:http://web-platform.test:8000/1234",NULL,false,0,NULL),
          "'self' never matches blob: (WPT blob/blob-urls-do-not-match-self)");
    check(!fetch_allowed("script-src 'self'",SCRIPT,"data:text/javascript,1",NULL,false,0,NULL),"'self' does not match data:");
    web_doc *d=document("http://a.test/x",csp("connect-src 'self'"));
    struct html_csp_request r={.url="ws://a.test/socket",.destination=WEBNET_DEST_EMPTY};
    check(html_policy_request_allowed(d,&r,NULL,NULL),"'self' on http matches ws on the default ports");
    r.url="wss://a.test/socket";check(html_policy_request_allowed(d,&r,NULL,NULL),"'self' matches wss");
    r.url="https://a.test/";check(html_policy_request_allowed(d,&r,NULL,NULL),"'self' matches https on default ports");
    r.url="http://a.test:81/";check(!html_policy_request_allowed(d,&r,NULL,NULL),"'self' rejects another port");
    release(d);
    d=document("https://a.test/x",csp("connect-src 'self'"));
    r.url="http://a.test/";check(!html_policy_request_allowed(d,&r,NULL,NULL),"'self' on https does not downgrade");
    r.url="ws://a.test/";check(!html_policy_request_allowed(d,&r,NULL,NULL),"'self' on https does not match ws");
    release(d);
    /* host-source: scheme-part from the expression or the self-origin (§6.7.2.9). */
    check(fetch_allowed("img-src example.com",WEBNET_DEST_IMAGE,"http://example.com/i.png",NULL,false,0,NULL),"host without scheme, same scheme");
    check(fetch_allowed("img-src example.com",WEBNET_DEST_IMAGE,"https://example.com/i.png",NULL,false,0,NULL),"host without scheme upgrades");
    check(!fetch_allowed("img-src example.com",WEBNET_DEST_IMAGE,"ftp://example.com/i.png",NULL,false,0,NULL),"host without scheme, other scheme");
    check(!fetch_allowed("img-src https://example.com",WEBNET_DEST_IMAGE,"http://example.com/i.png",NULL,false,0,NULL),"https host does not downgrade");
    check(fetch_allowed("img-src http://example.com",WEBNET_DEST_IMAGE,"https://example.com/i.png",NULL,false,0,NULL),"http host upgrades");
    check(fetch_allowed("connect-src ws://example.com",WEBNET_DEST_EMPTY,"wss://example.com/",NULL,false,0,NULL),"ws upgrades to wss");
    check(fetch_allowed("img-src EXAMPLE.com",WEBNET_DEST_IMAGE,"http://example.com/",NULL,false,0,NULL),"host-part is case-insensitive");
    check(fetch_allowed("img-src *.example.com",WEBNET_DEST_IMAGE,"http://a.b.example.com/",NULL,false,0,NULL),"*. matches any subdomain");
    check(!fetch_allowed("img-src *.example.com",WEBNET_DEST_IMAGE,"http://example.com/",NULL,false,0,NULL),"*. does not match the domain itself");
    check(!fetch_allowed("img-src 127.0.0.1",WEBNET_DEST_IMAGE,"http://127.0.0.1/",NULL,false,0,NULL),"§6.7.2.10 an IP host is not a domain");
    check(fetch_allowed("img-src *",WEBNET_DEST_IMAGE,"http://127.0.0.1/",NULL,false,0,NULL),"§6.7.2.8 step 1: the expression * matches any HTTP(S) URL");
    check(!fetch_allowed("img-src http://*",WEBNET_DEST_IMAGE,"http://127.0.0.1/",NULL,false,0,NULL),"§6.7.2.10 the host-part * needs a domain");
    /* '*' (§6.7.2.8 first step) */
    check(fetch_allowed("img-src *",WEBNET_DEST_IMAGE,"https://other.test/",NULL,false,0,NULL),"* matches HTTP(S)");
    check(!fetch_allowed("img-src *",WEBNET_DEST_IMAGE,"data:image/png,1",NULL,false,0,NULL),"* does not match data: (WPT script-src-wildcards-disallowed)");
    check(fetch_allowed("img-src * data:",WEBNET_DEST_IMAGE,"data:image/png,1",NULL,false,0,NULL),"scheme-source data:");
    check(fetch_allowed("img-src http:",WEBNET_DEST_IMAGE,"https://x.test/",NULL,false,0,NULL),"scheme-source http: upgrades");
    check(!fetch_allowed("img-src https:",WEBNET_DEST_IMAGE,"http://x.test/",NULL,false,0,NULL),"scheme-source https: does not downgrade");
    /* ports §6.7.2.11 */
    check(fetch_allowed("img-src example.com:*",WEBNET_DEST_IMAGE,"http://example.com:9/",NULL,false,0,NULL),"port *");
    check(!fetch_allowed("img-src example.com",WEBNET_DEST_IMAGE,"http://example.com:81/",NULL,false,0,NULL),"no port-part means the default port");
    check(fetch_allowed("img-src example.com:80",WEBNET_DEST_IMAGE,"http://example.com/",NULL,false,0,NULL),"explicit default port");
    check(fetch_allowed("img-src example.com:0080",WEBNET_DEST_IMAGE,"http://example.com/",NULL,false,0,NULL),
          "port-part is a decimal number (WPT base-uri-allow-leading-zero-port)");
    check(fetch_allowed("img-src http://example.com:8000",WEBNET_DEST_IMAGE,"http://example.com:8000/",NULL,false,0,NULL),"explicit port");
    /* paths §6.7.2.12 and §7.6 */
    check(fetch_allowed("img-src example.com/a/",WEBNET_DEST_IMAGE,"http://example.com/a/b/c",NULL,false,0,NULL),"directory path");
    check(!fetch_allowed("img-src example.com/a/",WEBNET_DEST_IMAGE,"http://example.com/a",NULL,false,0,NULL),"directory path needs the slash");
    check(fetch_allowed("img-src example.com/a",WEBNET_DEST_IMAGE,"http://example.com/a",NULL,false,0,NULL),"exact path");
    check(!fetch_allowed("img-src example.com/a",WEBNET_DEST_IMAGE,"http://example.com/a/",NULL,false,0,NULL),"exact path, extra slash");
    check(!fetch_allowed("img-src example.com/a",WEBNET_DEST_IMAGE,"http://example.com/ab",NULL,false,0,NULL),"exact path, other piece");
    check(fetch_allowed("img-src example.com/",WEBNET_DEST_IMAGE,"http://example.com/any/thing",NULL,false,0,NULL),"path / matches everything");
    check(fetch_allowed("img-src example.com/f%6F%6f",WEBNET_DEST_IMAGE,"http://example.com/foo",NULL,false,0,NULL),"pieces are percent-decoded");
    check(!fetch_allowed("img-src example.org/path",WEBNET_DEST_IMAGE,"http://example.org/not-path",NULL,false,0,NULL),"§7.6 direct load checks the path");
    check(fetch_allowed("img-src example.org/path",WEBNET_DEST_IMAGE,"http://example.org/not-path",NULL,false,1,NULL),"§7.6 after a redirect the path is not checked");
    check(fetch_allowed("img-src example.com/a?b",WEBNET_DEST_IMAGE,"http://example.com/a?b",NULL,false,0,NULL)==false,
          "a '?' is not a path-part character, so the expression is no host-source");
    /* 'none' and empty lists §6.7.2.7 */
    check(!fetch_allowed("img-src 'none'",WEBNET_DEST_IMAGE,"http://web-platform.test:8000/i",NULL,false,0,NULL),"'none'");
    check(fetch_allowed("img-src 'none' example.com",WEBNET_DEST_IMAGE,"http://example.com/",NULL,false,0,NULL),"'none' with another source");
    check(!fetch_allowed("img-src",WEBNET_DEST_IMAGE,"http://web-platform.test:8000/i",NULL,false,0,NULL),"an empty source list matches nothing");
}
static void fetch_directives(void){
    /* §6.8.3/§6.8.4 fallback */
    check(fetch_allowed("default-src 'none'; script-src 'self'",SCRIPT,"http://web-platform.test:8000/a.js",NULL,true,0,NULL),"script-src overrides default-src");
    check(!fetch_allowed("default-src 'none'; script-src 'self'",WEBNET_DEST_IMAGE,"http://web-platform.test:8000/i",NULL,false,0,NULL),"default-src for images");
    check(!fetch_allowed("default-src 'self'; script-src-elem https://example.com",SCRIPT,"http://web-platform.test:8000/a.js",NULL,true,0,NULL),
          "script-src-elem has no inheritance from default-src");
    check(fetch_allowed("script-src 'none'; child-src 'self'",WEBNET_DEST_WORKER,"http://web-platform.test:8000/w.js",NULL,false,0,NULL),
          "worker-src falls back to child-src before script-src");
    check(!fetch_allowed("script-src 'none'",WEBNET_DEST_WORKER,"http://web-platform.test:8000/w.js",NULL,false,0,NULL),"worker-src falls back to script-src");
    check(fetch_allowed("script-src-elem 'none'",WEBNET_DEST_WORKER,"http://web-platform.test:8000/w.js",NULL,false,0,NULL),
          "script-src-elem is no worker-src fallback");
    check(!fetch_allowed("child-src 'none'",WEBNET_DEST_IFRAME,"http://web-platform.test:8000/f.html",NULL,false,0,NULL),"frame-src falls back to child-src");
    check(fetch_allowed("frame-src 'self'; child-src 'none'",WEBNET_DEST_IFRAME,"http://web-platform.test:8000/f.html",NULL,false,0,NULL),"frame-src first");
    check(!fetch_allowed("default-src 'none'",WEBNET_DEST_EMPTY,"http://web-platform.test:8000/x",NULL,false,0,NULL),"fetch() is connect-src");
    check(!fetch_allowed("media-src 'none'",WEBNET_DEST_TRACK,"http://web-platform.test:8000/t.vtt",NULL,false,0,NULL),"track is media-src");
    check(!fetch_allowed("object-src 'none'",WEBNET_DEST_EMBED,"http://web-platform.test:8000/x",NULL,false,0,NULL),"embed is object-src");
    check(!fetch_allowed("font-src 'none'",WEBNET_DEST_FONT,"http://web-platform.test:8000/f.woff",NULL,false,0,NULL),"font-src");
    check(!fetch_allowed("manifest-src 'none'",WEBNET_DEST_MANIFEST,"http://web-platform.test:8000/m.json",NULL,false,0,NULL),"manifest-src");
    check(fetch_allowed("default-src 'none'",WEBNET_DEST_REPORT,"http://web-platform.test:8000/r",NULL,false,0,NULL),"a report request has no directive");
    check(fetch_allowed("img-src 'none'",SCRIPT,"http://other.test/a.js",NULL,true,0,NULL),"directives for other destinations do not apply");
    /* style-src nonce on the request (§6.1.13.1) */
    check(fetch_allowed("style-src 'nonce-abc'",WEBNET_DEST_STYLE,"http://other.test/s.css","abc",true,0,NULL),"style nonce");
    check(!fetch_allowed("style-src 'nonce-abc'",WEBNET_DEST_STYLE,"http://other.test/s.css","abd",true,0,NULL),"style wrong nonce");
    /* script directives pre-request check §6.7.1.1 */
    check(fetch_allowed("script-src 'nonce-abc'",SCRIPT,"http://other.test/a.js","abc",true,0,NULL),"script nonce");
    check(fetch_allowed("script-src 'NONCE-abc'",SCRIPT,"http://other.test/a.js","abc",true,0,NULL),"the nonce-source prefix is case-insensitive");
    check(!fetch_allowed("script-src 'nonce-abc'",SCRIPT,"http://other.test/a.js","ABC",true,0,NULL),"a nonce is a strict string match");
    check(!fetch_allowed("script-src 'nonce-abc'X",SCRIPT,"http://other.test/a.js","abc",true,0,NULL),
          "'nonce-abc'X is no nonce-source (WPT nonce-with-trailing-characters-blocked)");
    check(!fetch_allowed("script-src 'nonce-abc'",SCRIPT,"http://other.test/a.js",NULL,true,0,NULL),"no nonce");
    const char *sd="script-src 'nonce-abc' 'strict-dynamic' http://ok.test";
    check(!fetch_allowed(sd,SCRIPT,"http://ok.test/a.js",NULL,true,0,NULL),"'strict-dynamic' blocks parser-inserted scripts the host list allows");
    check(fetch_allowed(sd,SCRIPT,"http://elsewhere.test/a.js",NULL,false,0,NULL),"'strict-dynamic' allows non-parser-inserted scripts");
    check(fetch_allowed(sd,SCRIPT,"http://elsewhere.test/a.js","abc",true,0,NULL),"'strict-dynamic' with the nonce");
    check(fetch_allowed("script-src 'strict-dynamic'",WEBNET_DEST_WORKER,"http://elsewhere.test/w.js",NULL,false,0,NULL),
          "'strict-dynamic' in script-src allows a worker");
    check(fetch_allowed(sd,WEBNET_DEST_IMAGE,"http://elsewhere.test/i.png",NULL,false,0,NULL),"'strict-dynamic' does not touch img (no default-src)");
    web_doc *d=document(page,csp("script-src 'sha256-5jFwrAK0UV47oFbVg/iCCBbxD8X1w+QvoOUepu4C2YA='"));
    struct html_csp_request r={.url="http://other.test/a.js",.destination=SCRIPT,.parser_inserted=true,
        .integrity="sha256-5jFwrAK0UV47oFbVg/iCCBbxD8X1w+QvoOUepu4C2YA="};
    check(html_policy_request_allowed(d,&r,NULL,NULL),"§6.7.2.4 integrity metadata matching a hash-source");
    r.integrity="sha256-5jFwrAK0UV47oFbVg/iCCBbxD8X1w+QvoOUepu4C2YA= sha384-abc";
    check(!html_policy_request_allowed(d,&r,NULL,NULL),"§6.7.2.4 every integrity source must be listed");
    r.integrity="md5-5jFwrAK0UV47oFbVg/iCCBbxD8X1w+QvoOUepu4C2YA=";
    check(!html_policy_request_allowed(d,&r,NULL,NULL),"§6.7.2.4 no known algorithm is no metadata");
    release(d);
    /* violations: the effective directive, the request URL, dispositions */
    struct seen s={0};
    check(!fetch_allowed("img-src 'none'",WEBNET_DEST_IMAGE,"http://x.test/i.png",NULL,false,0,&s) && s.count==1 &&
          !strcmp(s.last,"img-src http://x.test/i.png enforce"),"violation for request: effective directive and URL");
    s=(struct seen){0};
    check(!fetch_allowed("default-src 'none'",SCRIPT,"http://x.test/a.js",NULL,true,0,&s) && !strcmp(s.last,"script-src-elem http://x.test/a.js enforce"),
          "§2.4.2 the effective directive, not default-src");
    d=document(page,"Content-Security-Policy-Report-Only: img-src 'none'\r\nContent-Security-Policy: img-src *\r\n");
    s=(struct seen){0};r=(struct html_csp_request){.url="http://x.test/i.png",.original_url="http://first.test/i.png",.destination=WEBNET_DEST_IMAGE};
    check(html_policy_request_allowed(d,&r,reporter,&s) && s.count==1 && !strcmp(s.last,"img-src http://first.test/i.png report"),
          "§4.1.1 report-only reports with the request's original URL and does not block");
    release(d);
    d=document(page,"Content-Security-Policy: img-src *, img-src 'self'\r\n");
    r=(struct html_csp_request){.url="http://x.test/i.png",.destination=WEBNET_DEST_IMAGE};
    check(!html_policy_request_allowed(d,&r,NULL,NULL),"§8.1 every enforced policy must allow");
    release(d);
    /* §6.7.2.2 resource hints */
    d=document(page,csp("default-src 'none'; img-src http://x.test"));
    r=(struct html_csp_request){.url="http://x.test/i.png",.destination=WEBNET_DEST_EMPTY,.prefetch=true};
    check(html_policy_request_allowed(d,&r,NULL,NULL),"a prefetch is allowed by any fetch directive");
    r.url="http://y.test/";check(!html_policy_request_allowed(d,&r,NULL,NULL),"a prefetch matching nothing is blocked");
    release(d);
}
static bool inline_allowed(const char *policy,enum html_csp_inline type,const char *nonce,bool nonceable,bool parser,const char *source,struct seen *seen){
    web_doc *d=document(page,csp(policy));
    struct html_csp_element e={.nonce=nonce,.nonceable=nonceable,.parser_inserted=parser};
    struct seen local={0};
    bool ok=html_policy_inline_allowed(d,type,type==HTML_CSP_NAVIGATION?NULL:&e,source,strlen(source),reporter,seen?seen:&local);
    release(d);return ok;
}
static void inline_checks(void){
    const char *code="alert(1);";
    check(inline_allowed("script-src 'unsafe-inline'",HTML_CSP_SCRIPT,NULL,false,true,code,NULL),"'unsafe-inline'");
    check(inline_allowed("img-src 'none'",HTML_CSP_SCRIPT,NULL,false,true,code,NULL),"no script directive");
    check(!inline_allowed("script-src 'self'",HTML_CSP_SCRIPT,NULL,false,true,code,NULL),"'self' does not allow inline (WPT script-src-1_1)");
    check(!inline_allowed("script-src 'unsafe-inline' 'nonce-abc'",HTML_CSP_SCRIPT,NULL,false,true,code,NULL),
          "a nonce disables 'unsafe-inline' (WPT scriptnonce-ignore-unsafeinline)");
    check(inline_allowed("script-src 'unsafe-inline' 'nonce-abc'",HTML_CSP_SCRIPT,"abc",true,true,code,NULL),"nonce on a nonceable script");
    check(!inline_allowed("script-src 'nonce-abc'",HTML_CSP_SCRIPT,"abc",false,true,code,NULL),"nonce on a non-nonceable script");
    check(!inline_allowed("script-src 'nonce-abc'",HTML_CSP_SCRIPT_ATTRIBUTE,"abc",true,true,code,NULL),"nonces do not apply to attributes");
    check(inline_allowed("script-src 'sha256-5jFwrAK0UV47oFbVg/iCCBbxD8X1w+QvoOUepu4C2YA='",HTML_CSP_SCRIPT,NULL,false,true,code,NULL),"sha256");
    check(inline_allowed("script-src 'sha384-dnux3uAPxaf+IhCrFG1D/XVNzP1XLDNcn3Pe3jyxouEAoot5kfwC5u8rMwNhE5oi'",HTML_CSP_SCRIPT,NULL,false,true,code,NULL),"sha384");
    check(inline_allowed("script-src 'sha512-yth/AKDfYyamGdVY92SJHjP5YqBda8LtutursuX70OzxIztHmFivMqd2l3Hm/STWljOjS5/MGmJn2+NkmIDPnw=='",HTML_CSP_SCRIPT,NULL,false,true,code,NULL),"sha512");
    check(inline_allowed("script-src 'SHA256-5jFwrAK0UV47oFbVg/iCCBbxD8X1w+QvoOUepu4C2YA='",HTML_CSP_SCRIPT,NULL,false,true,code,NULL),
          "hash algorithm is case-insensitive (WPT scripthash-case-insensitive)");
    check(inline_allowed("script-src 'sha256-5jFwrAK0UV47oFbVg_iCCBbxD8X1w-QvoOUepu4C2YA='",HTML_CSP_SCRIPT,NULL,false,true,code,NULL),
          "base64url hashes are base64 (WPT scripthash-base64url-converts-to-base64)");
    check(!inline_allowed("script-src 'sha256-5jFwrAK0UV47oFbVg/iCCBbxD8X1w+QvoOUepu4C2YA'",HTML_CSP_SCRIPT,NULL,false,true,code,NULL),
          "the padding is part of the compared value");
    check(!inline_allowed("script-src 'sha256-5jFwrAK0UV47oFbVg/iCCBbxD8X1w+QvoOUepu4C2YA='",HTML_CSP_SCRIPT,NULL,false,true,"alert(1); ",NULL),
          "the hash covers the exact text");
    check(inline_allowed("script-src 'sha256-g9VEzMIjwFfSv4DT8qMpgsMsPA244mdIINpQZHg/sJc='",HTML_CSP_SCRIPT,NULL,false,true,"\xed\xa0\x80",NULL),
          "an unpaired surrogate is hashed as U+FFFD (WPT utf-8-lone-surrogate)");
    check(!inline_allowed("script-src 'sha256-RFWPLDbv2BY+rCkDzsE+0fr8ylGr2R2faWMhq4lfEQc='",HTML_CSP_SCRIPT_ATTRIBUTE,NULL,false,true,"doSomething();",NULL),
          "a hash needs 'unsafe-hashes' for an event handler (WPT script_event_handlers_denied_missing_unsafe_hashes)");
    check(inline_allowed("script-src 'unsafe-hashes' 'sha256-RFWPLDbv2BY+rCkDzsE+0fr8ylGr2R2faWMhq4lfEQc='",HTML_CSP_SCRIPT_ATTRIBUTE,NULL,false,true,"doSomething();",NULL),
          "'unsafe-hashes' allows a hashed event handler");
    check(inline_allowed("script-src 'nonce-abc' 'strict-dynamic'",HTML_CSP_SCRIPT,"wrong",true,false,code,NULL),
          "'strict-dynamic' allows a non-parser-inserted inline script (WPT script-src-strict_dynamic_non_parser_inserted)");
    check(!inline_allowed("script-src 'nonce-abc' 'strict-dynamic'",HTML_CSP_SCRIPT,NULL,false,true,code,NULL),"'strict-dynamic' and a parser-inserted inline script");
    check(!inline_allowed("script-src 'unsafe-inline' 'strict-dynamic'",HTML_CSP_SCRIPT,NULL,false,true,code,NULL),"'strict-dynamic' disables 'unsafe-inline' for script");
    check(inline_allowed("style-src 'unsafe-inline' 'strict-dynamic'",HTML_CSP_STYLE,NULL,false,true,"p{}",NULL),"'strict-dynamic' only applies to scripts");
    check(!inline_allowed("style-src-elem 'none'; style-src 'unsafe-inline'",HTML_CSP_STYLE,NULL,false,true,"p{}",NULL),"style-src-elem for style elements");
    check(inline_allowed("style-src-elem 'none'; style-src 'unsafe-inline'",HTML_CSP_STYLE_ATTRIBUTE,NULL,false,true,"color:red",NULL),
          "style-src for style attributes (WPT style-src-elem-blocked-attr-allowed)");
    check(!inline_allowed("script-src-attr 'none'; script-src 'unsafe-inline'",HTML_CSP_SCRIPT_ATTRIBUTE,NULL,false,true,code,NULL),"script-src-attr");
    check(inline_allowed("script-src-attr 'none'; script-src 'unsafe-inline'",HTML_CSP_SCRIPT,NULL,false,true,code,NULL),"script-src-attr does not touch elements");
    check(!inline_allowed("default-src 'self'",HTML_CSP_STYLE_ATTRIBUTE,NULL,false,true,"color:red",NULL),"default-src for style attributes");
    check(!inline_allowed("script-src 'unsafe-inline' 'strict-dynamic'",HTML_CSP_NAVIGATION,NULL,false,false,"javascript:void(0)",NULL),
          "'strict-dynamic' and javascript: navigation");
    check(inline_allowed("script-src 'unsafe-inline'",HTML_CSP_NAVIGATION,NULL,false,false,"javascript:void(0)",NULL),"javascript: with 'unsafe-inline'");
    struct seen s={0};
    char long_source[]="0123456789012345678901234567890123456789ABCDEF";
    check(!inline_allowed("script-src 'report-sample' 'self'",HTML_CSP_SCRIPT,NULL,false,true,long_source,&s) &&
          !strcmp(s.last,"script-src-elem inline 0123456789012345678901234567890123456789 enforce"),"§4.2.3 'report-sample' takes 40 characters");
    s=(struct seen){0};
    check(!inline_allowed("script-src 'self'",HTML_CSP_SCRIPT_ATTRIBUTE,NULL,false,true,code,&s) && !strcmp(s.last,"script-src-attr inline enforce"),
          "§4.2.3 no sample without 'report-sample'; effective directive for attributes");
    s=(struct seen){0};
    check(!inline_allowed("script-src 'report-sample'",HTML_CSP_SCRIPT,NULL,false,true,"\xf0\x9f\x98\x80" "0123456789012345678901234567890123456789",&s) &&
          !strcmp(s.last,"script-src-elem inline \xf0\x9f\x98\x80" "01234567890123456789012345678901234567 enforce"),"the sample counts UTF-16 code units");
    /* §6.7.3.1 nonceable */
    node_t n={0};n.type=N_ELEM;n.tag=T_script;
    struct attr attrs[2]={{.name="nonce",.raw="nonce",.value="abc"},{.name="src",.raw="src",.value="x.js"}};n.attrs=attrs;n.nattrs=2;
    check(html_policy_nonceable(&n),"a script with a nonce is nonceable");
    attrs[1].value="<script";check(!html_policy_nonceable(&n),"\"<script\" in an attribute value (WPT nonce-hiding / dangling markup)");
    attrs[1].value="x";attrs[1].name="<style";check(!html_policy_nonceable(&n),"\"<style\" in an attribute name");
    n.tag=T_style;check(html_policy_nonceable(&n),"only script elements are checked for markup");
    n.nattrs=0;check(!html_policy_nonceable(&n),"no nonce attribute");
}
static void other_checks(void){
    struct seen s={0};
    web_doc *d=document(page,csp("script-src 'self' 'report-sample'"));
    check(!html_policy_eval_allowed(d,"1+1",3,reporter,&s) && !strcmp(s.last,"script-src eval 1+1 enforce"),"§4.4.1 eval without 'unsafe-eval'");
    release(d);
    d=document(page,csp("default-src 'unsafe-eval'; script-src 'self'"));
    check(!html_policy_eval_allowed(d,"1",1,NULL,NULL),"§4.4.1 script-src before default-src");release(d);
    d=document(page,csp("default-src 'self' 'unsafe-eval'"));check(html_policy_eval_allowed(d,"1",1,NULL,NULL),"default-src 'unsafe-eval'");release(d);
    d=document(page,csp("script-src-elem 'none'"));check(html_policy_eval_allowed(d,"1",1,NULL,NULL),"script-src-elem is not used for eval");release(d);
    d=document(page,"Content-Security-Policy: require-trusted-types-for 'script'; script-src 'trusted-types-eval'\r\n");
    check(html_policy_eval_allowed(d,"1",1,NULL,NULL),"'trusted-types-eval' with Trusted Types enforced");release(d);
    d=document(page,csp("script-src 'trusted-types-eval'"));
    check(!html_policy_eval_allowed(d,"1",1,NULL,NULL),"'trusted-types-eval' without Trusted Types");release(d);
    d=document(page,"Content-Security-Policy-Report-Only: script-src 'self'\r\n");
    s=(struct seen){0};check(html_policy_eval_allowed(d,"1",1,reporter,&s) && s.count==1,"report-only eval reports (WPT eval-allowed-in-report-only-mode)");release(d);
    d=document(page,csp("base-uri 'self'"));
    check(html_policy_base_allowed(d,"http://web-platform.test:8000/base/",NULL,NULL),"§6.3.1.1 base-uri 'self'");
    s=(struct seen){0};check(!html_policy_base_allowed(d,"http://www1.web-platform.test:8000/base/",reporter,&s) && !strcmp(s.last,"base-uri inline enforce"),
          "§6.3.1.1 base-uri violation");release(d);
    d=document(page,csp("form-action 'self'"));
    struct html_csp_request r={.url="http://www1.web-platform.test:8000/submit"};
    check(html_policy_navigation_allowed(d,&r,false,NULL,NULL),"form-action ignores other navigations");
    s=(struct seen){0};check(!html_policy_navigation_allowed(d,&r,true,reporter,&s) && !strcmp(s.last,"form-action http://www1.web-platform.test:8000/submit enforce"),
          "§6.4.1.1 form-action");
    r.url="http://web-platform.test:8000/submit";check(html_policy_navigation_allowed(d,&r,true,NULL,NULL),"form-action 'self'");
    release(d);
    d=document(page,csp("script-src 'self'"));
    r.url="javascript:alert(1)";s=(struct seen){0};
    check(!html_policy_navigation_allowed(d,&r,false,reporter,&s) && !strcmp(s.last,"script-src-elem inline enforce"),"§4.2.4 javascript: navigation inline check");
    release(d);
    d=document("http://w.test/worker.js",csp("sandbox allow-scripts"));check(!html_policy_worker_allowed(d),"§6.3.2.1 sandbox without allow-same-origin blocks a worker");release(d);
    d=document("http://w.test/worker.js",csp("sandbox allow-scripts allow-same-origin"));check(html_policy_worker_allowed(d),"sandbox that allows both");release(d);
    d=document("http://w.test/worker.js","Content-Security-Policy-Report-Only: sandbox\r\n");check(html_policy_worker_allowed(d),"report-only sandbox is ignored");release(d);
    d=document(page,csp("upgrade-insecure-requests"));check(html_policy_upgrade_insecure(d),"upgrade-insecure-requests");release(d);
    d=document(page,"Content-Security-Policy-Report-Only: upgrade-insecure-requests\r\n");check(!html_policy_upgrade_insecure(d),"report-only UIR does not upgrade");release(d);
    char *u=html_policy_report_url(NULL,"http://user:pass@a.test:81/x?y#z");
    check(u && !strcmp(u,"http://a.test:81/x?y"),"§5.4 strip fragment and credentials");free(u);
    u=html_policy_report_url(NULL,"data:text/plain,secret");check(u && !strcmp(u,"data"),"§5.4 non-HTTP URL is its scheme");free(u);
    /* policy container inheritance keeps the creator's self-origin */
    web_doc *parent=document(page,csp("img-src 'self'"));
    web_doc *child=calloc(1,sizeof *child);child->url=strdup("about:srcdoc");child->inherited_url=strdup(page);
    check(html_policy_init(child,NULL,parent) && child->html_policy && !strcmp(child->html_policy->self_url,page),"inherited self-origin");
    r=(struct html_csp_request){.url="http://web-platform.test:8000/i.png",.destination=WEBNET_DEST_IMAGE};
    check(html_policy_request_allowed(child,&r,NULL,NULL),"srcdoc 'self' is the creator's origin");
    char *snapshot=html_policy_snapshot(child);
    web_doc *worker=calloc(1,sizeof *worker);worker->url=strdup("blob:http://web-platform.test:8000/1");
    check(snapshot && html_policy_snapshot_init(worker,snapshot) && worker->html_policy && !strcmp(worker->html_policy->self_url,page),
          "a snapshot keeps the self-origin");
    check(html_policy_request_allowed(worker,&r,NULL,NULL),"the snapshot's 'self'");
    free(snapshot);release(worker);release(child);release(parent);
}
int main(void){
    parsing();url_matching();fetch_directives();inline_checks();other_checks();
    printf("%u checks, %u failures\n",checks,failed);
    return failed!=0;
}
