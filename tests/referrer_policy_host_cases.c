static unsigned checks,failed;
static void check(bool condition,const char *label){checks++;if(!condition){failed++;printf("FAIL %s\n",label);}}
static void clear(struct job *j){free(j->referrer);free(j->referrer_origin);free(j->target_origin);free(j->outgoing.p);*j=(struct job){0};}
static struct job job(const char *source,const char *target,int policy){
    struct job j={.referrer=strdup(source),.referrer_policy=policy};
    char *canonical=NULL;check(http_url(target,&canonical,&j.target_origin)==WEB_URL_TUPLE,"target native canonicalization");free(canonical);return j;
}
static void output(int policy,const char *target,const char *value,const char *label){
    struct job j=job("https://user:pass@Referrer.Test:443/where#private",target,policy);
    check(prepare_referrer(&j)&&append_referrer(&j,target),label);
    if(value){char expected[128];snprintf(expected,sizeof expected,"Referer: %s\r\n",value);check(j.outgoing.p&&!strcmp(j.outgoing.p,expected),"policy emits precise header");}
    else check(j.outgoing.length==0,"policy omits header");
    clear(&j);check(live_allocations==0,"scoped URL and header storage released");
}
int main(void){
    check(sizeof(struct webnet_wire_request)==64,"persisted request wire remains 64 bytes");
    check(forbidden_header("Referer")&&forbidden_header("Origin")&&forbidden_header("Cookie"),"author header protections remain");
    check(webnet_referrer_policy_header("Referrer-Policy: origin, unknown, strict-origin\r\nreferrer-policy: no-referrer\r\n",7)==1,"last recognized response token wins across fields");
    check(webnet_referrer_policy_header("Referrer-Policy: invalid,\t\r\n",3)==3,"invalid response token retains policy");
    web_doc top={.url="https://referrer.test/page"},srcdoc={.url="about:srcdoc",.inherited_url=top.url,.frame_parent=&top};
    web_doc nested={.url="about:srcdoc",.inherited_url=top.url,.frame_parent=&srcdoc};
    check(!strcmp(fetch_client_referrer_source(&nested),top.url),"srcdoc source walks native container documents");
    srcdoc.sandbox_flags=SB_ORIGIN;check(fetch_client_referrer_source(&nested)[0]==0,"opaque srcdoc ancestor cannot expose URL");srcdoc.sandbox_flags=0;
    nested.sandbox_flags=SB_ORIGIN;check(fetch_client_referrer_source(&nested)[0]==0,"opaque client suppresses about-client referrer");
    web_doc opaque={.url="data:text/html,document"};check(fetch_client_referrer_source(&opaque)[0]==0,"non-tuple Window origin has no client source");
    top.sandbox_flags=SB_ORIGIN;check(fetch_client_referrer_source(&top)[0]==0,"HTTPS URL is not enough to make sandbox origin nonopaque");top.sandbox_flags=0;
    const char *full="https://referrer.test/where",*origin="https://referrer.test/";
    for(int p=0;p<=8;p++){
        output(p,"https://referrer.test/next",p==1?NULL:(p==4||p==5?origin:full),"same-origin policy");
        output(p,"https://target.test/next",p==1||p==3?NULL:(p==2||p==8?full:origin),"cross-origin policy");
        output(p,"http://target.test/next",p==0||p==1||p==2||p==3||p==5||p==7?NULL:(p==8?full:origin),"downgrade policy");
    }
    struct job j=job("http://referrer.test/path#private","http://target.test/",2);
    check(prepare_referrer(&j)&&append_referrer(&j,"http://target.test/")&&strstr(j.outgoing.p,"http://referrer.test/path\r\n"),"untrusted HTTP to HTTP is not downgrade");clear(&j);
    j=job("https://referrer.test/path","http://localhost:8080/",5);
    check(prepare_referrer(&j)&&append_referrer(&j,"http://localhost:8080/")&&j.outgoing.length>0,"loopback potentially trustworthy target");clear(&j);
    j=job("https://referrer.test/path","http://127.evil.test/",5);
    check(prepare_referrer(&j)&&append_referrer(&j,"http://127.evil.test/")&&j.outgoing.length==0,"DNS name with 127 prefix still suppresses downgrade referrer");clear(&j);
    check(web_url_potentially_trustworthy("http://127.0.0.1/")&&web_url_potentially_trustworthy("http://127.255.255.255:80/"),"all valid IPv4 127/8 literals are trustworthy");
    /* Secure Contexts reads the parsed URL: the IPv4 parser reads 127.1 as 127.0.0.1. */
    check(web_url_potentially_trustworthy("http://127.1/")&&web_url_potentially_trustworthy("http://0x7f.1/"),"every IPv4 spelling of 127/8 is that address");
    check(!web_url_potentially_trustworthy("http://127.256.0.1/")&&!web_url_potentially_trustworthy("http://127.1.2.3.evil/")&&!web_url_potentially_trustworthy("http://a.localhost/"),"not URLs and unguaranteed DNS names not upgraded");
    check(web_url_potentially_trustworthy("http://[::1]:8080/")&&web_url_potentially_trustworthy("http://[0:0::1]/")&&!web_url_potentially_trustworthy("http://[::2]/"),"IPv6 loopback in any spelling, nothing else");
    /* The request layer takes every URL the URL parser gives; the network has no IPv6 (tls.c says so). */
    struct http_target ipv6;check(http_target_of("http://user:pass@[0:0::1]:8080/a?b#c",&ipv6)==WEB_URL_TUPLE&&!strcmp(ipv6.host,"[::1]")&&
        !strcmp(ipv6.authority,"[::1]:8080")&&!strcmp(ipv6.path,"/a?b")&&ipv6.port==8080&&!ipv6.tls,"IPv6 target read from the record, without credentials or fragment");
    http_target_free(&ipv6);
    j=job("https://referrer.test/path","https://target.test/",7);
    check(prepare_referrer(&j)&&append_referrer(&j,"https://target.test/")&&!strcmp(j.referrer,origin),"cross-origin hop reduces stored referrer");
    free(j.target_origin);j.target_origin=strdup("https://referrer.test");j.outgoing.length=0;j.referrer_policy=8;
    check(append_referrer(&j,"https://referrer.test/")&&!strcmp(j.referrer,origin),"later unsafe response policy cannot resurrect path");clear(&j);
    j=job("https://referrer.test/path","https://target.test/",3);
    check(prepare_referrer(&j)&&append_referrer(&j,"https://target.test/")&&j.referrer[0]==0,"cross-origin same-origin policy removes stored referrer");
    j.referrer_policy=8;j.outgoing.length=0;check(append_referrer(&j,"https://target.test/")&&j.outgoing.length==0,"redirect cannot resurrect omitted referrer");clear(&j);
    j=job("https://referrer.test/path","https://referrer.test/",8);j.wire.user_navigation=WEBNET_WIRE_NO_REFERRER;
    check(prepare_referrer(&j)&&append_referrer(&j,"https://referrer.test/")&&j.outgoing.length==0,"explicit no-referrer private flag remains authoritative");clear(&j);
    j=job("data:text/plain,none","https://target.test/",8);check(prepare_referrer(&j)&&j.referrer==NULL,"non-HTTP source has no network referrer");clear(&j);
    /* The URL parser removes every tab and newline, so the header gets the URL without them. */
    j=job("https://referrer.test/\r\nforged","https://target.test/",8);check(prepare_referrer(&j)&&append_referrer(&j,"https://target.test/")&&
        !strcmp(j.outgoing.p,"Referer: https://referrer.test/forged\r\n"),"private header injection cannot reach the header");clear(&j);
    j=job("https://referrer.test/ forged","https://target.test/",8);check(prepare_referrer(&j)&&append_referrer(&j,"https://target.test/")&&
        !strcmp(j.outgoing.p,"Referer: https://referrer.test/%20forged\r\n"),"a space in the path is percent-encoded");clear(&j);
    j=job("http://exa mple.test/","https://target.test/",8);check(!prepare_referrer(&j)&&j.error[0],"a referrer that is not a URL fails closed");clear(&j);
    char *long_url=malloc(5000);size_t prefix=strlen("https://referrer.test/");memcpy(long_url,"https://referrer.test/",prefix);memset(long_url+prefix,'x',4999-prefix);long_url[4999]=0;
    j=job(long_url,"https://referrer.test/",8);free(long_url);check(prepare_referrer(&j)&&!strcmp(j.referrer,origin),"over-4096 referrer is reduced to origin per specification");clear(&j);
    int needed;
    j=job("https://user:pass@referrer.test/path#private","https://target.test/",7);allocations=0;check(prepare_referrer(&j)&&append_referrer(&j,"https://target.test/"),"allocator baseline");needed=allocations;clear(&j);
    for(int i=0;i<needed;i++){
        j=job("https://user:pass@referrer.test/path#private","https://target.test/",7);allocations=0;fail_allocation=i;
        bool ok=prepare_referrer(&j)&&append_referrer(&j,"https://target.test/");fail_allocation=-1;
        check(!ok&&j.error[0],"allocation failure is a genuine native error");clear(&j);check(live_allocations==0,"allocation failure preserves ownership for cleanup");
    }
    printf("Native referrer policy supporting checks %u, failed %u\n",checks,failed);return failed?1:0;
}
