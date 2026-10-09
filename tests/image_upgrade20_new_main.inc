static unsigned image_scenario;
static bool tls_failure;
static char observed_urls[3][1024];
static int fixture_request(const struct http_req*q,struct http_resp*r){
    unsigned call=requests++;memset(r,0,sizeof *r);if(call>=3)return -1;
    snprintf(observed_urls[call],sizeof observed_urls[call],"%s",q->url);
    snprintf(observed_headers[call],sizeof observed_headers[call],"%s",q->headers?q->headers:"");
    if(tls_failure){snprintf(r->error,sizeof r->error,"fixture TLS failure");return -1;}
    const char *fields="";
    if(image_scenario&&call==0){r->status=302;fields=image_scenario==1?
        "Location: http://redirect-image.test:8080/image.png\r\n":
        "Location: http://127.0.0.1/image.png\r\n";}
    else r->status=200;
    size_t n=strlen(fields);r->headers_full=count_malloc(n+1);if(!r->headers_full)return -1;
    memcpy(r->headers_full,fields,n+1);r->headers_len=n;return 0;
}
long webcookie_export(webcookie_jar*a,void*b,size_t c,int64_t d){(void)a;(void)b;(void)c;(void)d;return 0;}
static struct job *make_image(const char*url,bool upgrade){struct job*j=make_job(url,"https://page.test/","");j->wire.kind=WEBNET_RESOURCE;if(upgrade)j->wire.user_navigation|=WEBNET_WIRE_IMAGE_UPGRADE;image_scenario=0;tls_failure=false;return j;}
static void close_image(struct job*j){close_job(j);memset(observed_urls,0,sizeof observed_urls);image_scenario=0;tls_failure=false;}
static void callback(webnet*n,uint64_t id,uint64_t gen,const struct webnet_response*r,void*o){(void)n;(void)id;(void)gen;(void)r;(void)o;}
int main(void){
    CHECK("image wire opt-in is reserved8192 and preserves64byte framing",WEBNET_WIRE_IMAGE_UPGRADE==8192u&&sizeof(struct webnet_wire_request)==64);
    CHECK("only RESOURCE may carry image upgrade flag",webnet_wire_image_flag_valid(WEBNET_RESOURCE,8192)&&!webnet_wire_image_flag_valid(WEBNET_FETCH,8192)&&!webnet_wire_image_flag_valid(WEBNET_CLASSIC,8192)&&!webnet_wire_image_flag_valid(WEBNET_MODULE,8192)&&!webnet_wire_image_flag_valid(WEBNET_NAVIGATION,8192));
    struct webnet net={0};struct webnet_request native={.kind=WEBNET_RESOURCE,.url="http://image.test/x",.origin="https://page.test/",.image_upgrade=true,.credentials=WEBNET_CREDENTIALS_OMIT};
    uint64_t id=webnet_submit(&net,&native,callback,NULL);struct webnet_wire_request wire={0};if(net.queue)memcpy(&wire,net.queue->tx,sizeof wire);
    CHECK("native submit retains image metadata on real serialized request",id&&net.queue&&net.queue->image_upgrade&&(wire.user_navigation&8192)&&wire.kind==WEBNET_RESOURCE);
    if(net.queue)request_free(net.queue);net=(struct webnet){0};
    native.kind=WEBNET_FETCH;CHECK("native submit forbids forged nonimage upgrade class",!webnet_submit(&net,&native,callback,NULL)&&!net.queue&&live==0);
    struct job*j=make_image("http://image.test/p.png?fixture=1",true);
    CHECK("ordinary image really dispatches HTTPS and not original HTTP",run_http(j)&&requests==1&&!strcmp(observed_urls[0],"https://image.test/p.png?fixture=1")&&!strcmp(j->final_url,observed_urls[0]));close_image(j);CHECK("upgrade request releases exact URL and origin ownership",live==0);
    j=make_image("http://image.test:80/p.png",true);CHECK("explicit default80 is443 after scheme upgrade",run_http(j)&&!strcmp(observed_urls[0],"https://image.test/p.png"));close_image(j);
    j=make_image("http://image.test:8080/p.png",true);CHECK("explicit nondefault image port remains8080",run_http(j)&&!strcmp(observed_urls[0],"https://image.test:8080/p.png"));close_image(j);
    j=make_image("http://image.test:443/p.png",true);CHECK("explicit port443 survives as HTTPS default rather than80",run_http(j)&&!strcmp(observed_urls[0],"https://image.test/p.png"));close_image(j);
    j=make_image("http://image.test/p.png",false);CHECK("imageset or crossorigin unclassified RESOURCE remains blocked",!run_http(j)&&requests==0&&strstr(j->error,"insecure"));close_image(j);
    const enum webnet_kind kinds[]={WEBNET_FETCH,WEBNET_CLASSIC,WEBNET_MODULE,WEBNET_NAVIGATION};
    for(unsigned i=0;i<sizeof kinds/sizeof *kinds;i++){j=make_image("http://image.test/p.png",true);j->wire.kind=kinds[i];CHECK("forged nonRESOURCE class never upgrades in worker helper",!run_http(j)&&requests==0);close_image(j);}
    const char *ips[]={"127.0.0.1","127.1","2130706433","0x7f000001","017700000001","0177.0.0.1","0x7f.0.0.1","127.0.0.1.","999999999999999999999999","images.123","images.0x1","0x"};
    char raw[512];
    for(unsigned i=0;i<sizeof ips/sizeof *ips;i++){snprintf(raw,sizeof raw,"http://%s/p.png",ips[i]);j=make_image(raw,true);CHECK("IP and potential legacyIP remain mixed-content failclosed",!run_http(j)&&requests==0&&strstr(j->error,"insecure"));close_image(j);}
    j=make_image("http://[::1]/p.png",true);CHECK("unsupportedIPv6 cannot bypass upgrade exclusion",!run_http(j)&&requests==0);close_image(j);
    j=make_image("http://image.test/p.png",true);j->document="http://page.test/";CHECK("nonHTTPS document does not upgrade ordinary HTTP image",run_http(j)&&requests==1&&!strcmp(observed_urls[0],"http://image.test/p.png"));close_image(j);
    j=make_image("https://image.test/p.png",true);CHECK("alreadyHTTPS image stays unchanged",run_http(j)&&requests==1&&!strcmp(observed_urls[0],"https://image.test/p.png"));close_image(j);
    j=make_image("http://image.test/p.png",true);tls_failure=true;CHECK("TLS error issues exactlyoneHTTPS request and noHTTPfallback",!run_http(j)&&requests==1&&!strcmp(observed_urls[0],"https://image.test/p.png")&&strstr(j->error,"TLS"));close_image(j);
    j=make_image("http://image.test/p.png",true);image_scenario=1;CHECK("HTTP redirect target upgrades before real next-hop dispatch",run_http(j)&&requests==2&&!strcmp(observed_urls[0],"https://image.test/p.png")&&!strcmp(observed_urls[1],"https://redirect-image.test:8080/image.png"));close_image(j);
    j=make_image("http://image.test/p.png",true);image_scenario=2;CHECK("redirect toIP isblocked before insecure target dispatch",!run_http(j)&&requests==1&&strstr(j->error,"insecure"));close_image(j);
    for(unsigned fault=5;fault<=9;fault++){
        j=make_image("http://image.test/p.png",true);http_origin_owned(j->document,&j->origin);j->target_origin=count_malloc(17);strcpy(j->target_origin,"https://old.test");char*out=j->current;strcpy(out,"unchanged");fail_malloc=fault;
        enum http_url_result result=image_http_url(j,j->url,out,sizeof j->current,&j->target_origin);
        CHECK("each upgrade-allocation fault rollsback publicURL-origin slots",result==HTTP_URL_OOM&&!strcmp(out,"unchanged")&&!strcmp(j->target_origin,"https://old.test")&&requests==0);close_image(j);
    }
    j=make_image("http://image.test/x",true);http_origin_owned(j->document,&j->origin);strcpy(j->current,"unchanged");CHECK("upgrade one-byte expansion never prefixfits destination",image_http_url(j,j->url,j->current,strlen(j->url)+1,&j->target_origin)==HTTP_URL_INVALID&&!strcmp(j->current,"unchanged")&&!j->target_origin);close_image(j);
    struct request incoming={.image_upgrade=true,.cookie_site="https://page.test/"};incoming.head.url_len=sizeof("http://image.test/x")-1;incoming.head.status=200;incoming.rx=count_malloc(incoming.head.url_len);memcpy(incoming.rx,"http://image.test/x",incoming.head.url_len);
    CHECK("successful native upgrade response cannot publish HTTPfinalURL",!unpack(&incoming));count_free(incoming.rx);
    incoming=(struct request){.image_upgrade=true,.cookie_site="https://page.test/"};incoming.head.url_len=sizeof("https://image.test/x")-1;incoming.head.status=200;incoming.rx=count_malloc(incoming.head.url_len);memcpy(incoming.rx,"https://image.test/x",incoming.head.url_len);
    CHECK("verified HTTPSfinal metadata is accepted and terminates exactly",unpack(&incoming)&&!strcmp((char*)incoming.rx,"https://image.test/x"));count_free(incoming.rx);
    incoming=(struct request){.image_upgrade=true,.cookie_site="http://page.test/"};incoming.head.url_len=sizeof("http://image.test/x")-1;incoming.head.status=200;incoming.rx=count_malloc(incoming.head.url_len);memcpy(incoming.rx,"http://image.test/x",incoming.head.url_len);
    CHECK("native image metadata does not imposeHTTPS on plainHTTP client",unpack(&incoming)&&!strcmp((char*)incoming.rx,"http://image.test/x"));count_free(incoming.rx);
    CHECK("all new image lifetime boundaries release tracked ownership",live==0);
    printf("new ordinary-image HTTPS upgrade boundaries: %u checks / %u failed\n",checks,failed);return failed?1:0;
}
