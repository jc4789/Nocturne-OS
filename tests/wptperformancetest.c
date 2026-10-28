/* 限定した WPT 原本だけを native web_live/webnet で実行する。
 * DOM/CSS/assert/harness を変更しない。結果観測だけを初回 tick 前に接続する。 */
#include "nocturne.h"
#include "web.h"
#include "webnet.h"
#include <bearssl_hash.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <limits.h>

#define VIEW_W 1024
#define VIEW_H 768
#define CASE_MS 30000u
/* sha256 NULL: a test named on the command line (--run), served unchanged from the checkout;
   its hash is printed for the record and any number of cases may report. */
struct original { const char *path, *sha256; size_t bytes; unsigned tests; };
static const struct original originals[] = {
    {"/css/cssom-view/elementFromPoint-negative-z-index-inline-overflow-clip.html",
     "41fb3ead95fe420015748927ada82c3ab17a797642ed4004d106abd2d1645cb1",1554,1},
    {"/css/cssom-view/elementFromPoint-dynamic-anon-box.html",
     "e3ad147b69f8d5423f67fdb434e93d4e5412ef4a0b027f7d7c6ef3c28ef5376a",1310,1},
    {"/css/cssom-view/scrollWidthHeight-negative-margin-001.html",
     "3f915124cacc59c49d67d09cb63dcaf4e92ad464aac8e56528fd7912c85f32bb",1344,4},
    {"/resources/testharness.js",
     "7a108d7cfa98ac413ddb38f82fd509fb2037dfcc25ccf466567520cbb6bdf1fa",198420,0},
    {"/resources/testharnessreport.js",
     "3777176fc736b0a78cf7617b9613b220f635daae7536ec7ebb99c61dbb525aae",1231,0},
    {"/css/selectors/has-matches-to-uninserted-elements.html",
     "1198912c925ce51de32d4770919160294bd86d246cbdd92475354ccd0c8ad442",1520,12},
    {"/css/css-conditional/js/CSS-supports-L3.html",
     "f3b5839f9adca36bd068bbf9f5fb949e911a2dcb2c87e17d7ec1b6599e82387f",3088,17},
    {"/css/css-conditional/js/CSS-supports-L4.html",
     "6cc549b9894a5a6847791c1dca7fb9f436a7c763878e205d4363e73641440c92",2977,18},
    {"/preload/modulepreload-sri.html",
     "4458a0c2e93a44996a272de5f18a12abfcb6d0ae65129cce54068ae34552ea74",1584,1},
    {"/intersection-observer/inline-client-rect.html",
     "747f1ecba0b57919bca5f6a0a4ca11021c73ad75cf8a915bef59377cb4635361",2732,3},
    {"/intersection-observer/slotted-target.html",
     "417830718d7ec888d56b8c4d9894494a9b2d87768f6835bc3f76b95aa2abfdc2",1091,1},
    {"/intersection-observer/resources/intersection-observer-test-utils.js",
     "8454449710342c998b8cc02e426a9fd8f02d1ced15cac233364932a278c73120",8182,0},
    {"/css/css-flexbox/parsing/flex-invalid.html",
     "a8435252ba1160cc68745196a545ce664a69bb8d5ab11bd1a109d8502829543e",894,6},
    {"/css/support/parsing-testcommon.js",
     "ac482e1e1908e9b98f8f1c4b2c457ec77ae5ad2baa074205c4b0f7812f4024b5",7476,0},
    {"/css/css-backgrounds/parsing/border-radius-valid.html",
     "c9741a4430568903ce0db1bce7c65ea456adbb16c8e01f4551cda66f38729533",1926,23},
    {"/css/css-backgrounds/parsing/border-radius-invalid.html",
     "64a471cba88003a253feeadafc2c0ce9fb17c20adf3e697c4eb8663fda22f0c9",1179,11},
    {"/css/css-backgrounds/parsing/border-radius-computed.html",
     "e749eb2b261af965dcaaf06d408fa6a583f3ab9c8c14b2b685e660309dc2bbc4",1633,14},
    {"/css/support/computed-testcommon.js",
     "20bd767e8a8bd9e4857a29aecfd52cad305ed4af1cf73a5064916a8a1d38ab01",7812,0},
    {"/css/css-cascade/layer-basic.html",
     "03af0e7afcc5b1b7a39657f61e49fd9f898b3e60bfd8760bec22a7bf50cb1b75",12555,34},
    {"/css/css-cascade/layer-important.html",
     "10bf97f54563b7db36c600c3ad6e18243dba0a9204b60b4a10fda5e9de2b0096",3332,9},
    {"/css/css-cascade/layer-vs-inline-style.html",
     "3d6d5789b9d017c7c4b6a5625eed7393c7393713ff24435437a192b60c478984",1750,4},
    {"/css/css-cascade/layer-media-query.html",
     "97e59964ac39b3c8474bb4d91d2ddad5ecd20ff99b4ecf6e25cef694c41b55a2",4490,8},
    {"/css/css-flexbox/text-overflow-on-flexbox-001.html",
     "a77f83de64246658a59bcce5286c7c00d606abe65be0d71dd1f847fe1f3b7ffb",750,0},
    {"/css/css-flexbox/reference/text-overflow-on-flexbox-001-ref.html",
     "65fc001d21d2598fd7ee324632d03a0848f98cd6dacd7f2c662833480d389b91",411,0}
};
struct run;
struct pending {
    struct pending *next;
    struct run *run;
    uint64_t native_id, transport_id;
    const struct original *original;
};
struct run {
    webnet *net;
    web_doc *doc;
    struct pending *pending;
    struct web_response navigation;
    const struct original *original;
    char *harness_url, *report_url;
    bool navigation_done, observer_ready, complete;
    unsigned infrastructure, result_count, failed;
    int harness_status, scroll_x, scroll_y;
    uint64_t deadline;
    uint32_t *pixels;
};

/* 標準 add_completion_callback の登録だけ。fixture の関数は差し替えない。
 * 原本 harness は登録済み callback を後から再通知しないので、external
 * script の capture load（次の inline test より前）で接続する。 */
static const char observer[] =
    "(function(){var armed=false;function attach(){"
    "if(armed||typeof add_completion_callback!=='function')return;armed=true;"
    "add_completion_callback(function(tests,status){var bad=0;"
    "for(var i=0;i<tests.length;i++){var t=tests[i];if(t.status!==0)bad++;"
    "console.log('NOCTURNE_WPT_RESULT '+JSON.stringify({name:t.name,status:t.status,"
    "message:t.message,stack:t.stack}));}"
    "console.log('NOCTURNE_WPT_HARNESS '+JSON.stringify({status:status.status,"
    "message:status.message,stack:status.stack}));"
    "console.log('NOCTURNE_WPT_COMPLETE '+status.status+' '+tests.length+' '+bad);});"
    "console.log('NOCTURNE_WPT_OBSERVER_READY');}"
    "window.addEventListener('load',attach,true);attach();})();";
/* Original reftest has no script or external dependencies. This observes only
   native load readiness; it never alters the original DOM, CSS or paint. */
static const char reference_observer[] =
    "window.addEventListener('load',function(){console.log('NOCTURNE_WPT_REF_LOADED');},{once:true});";

static bool unchanged(const struct original *o,const struct webnet_response *r) {
    if(!o)return true;
    if(r->status!=200 || (r->error&&*r->error) || (o->sha256&&r->body_len!=o->bytes) || !r->body)return false;
    br_sha256_context context; unsigned char digest[32];char hex[65];
    static const char digits[]="0123456789abcdef";
    br_sha256_init(&context);br_sha256_update(&context,r->body,r->body_len);br_sha256_out(&context,digest);
    for(unsigned i=0;i<32;i++){hex[2*i]=digits[digest[i]>>4];hex[2*i+1]=digits[digest[i]&15];}hex[64]=0;
    printf("WPT 原本 %s %zu bytes SHA256 %s\n",o->path,r->body_len,hex);
    return !o->sha256||!strcmp(hex,o->sha256);
}
static bool copy_response(struct web_response *out,const struct webnet_response *in) {
    memset(out,0,sizeof *out);out->status=in->status;out->event=in->event;out->uploaded=in->uploaded;
    snprintf(out->error,sizeof out->error,"%s",in->error?in->error:"");
    if(in->body_len==SIZE_MAX || (in->body_len&&!in->body) ||
       !web_response_set_url(out,in->final_url?in->final_url:""))return false;
    out->headers_full=strdup(in->headers?in->headers:"");out->body=malloc(in->body_len+1);
    if(!out->headers_full||!out->body)return false;
    out->body_len=in->body_len;if(in->body_len)memcpy(out->body,in->body,in->body_len);
    out->body[in->body_len]=0;return true;
}
static void unlink_request(struct run *r,struct pending *p) {
    struct pending **at=&r->pending;while(*at&&*at!=p)at=&(*at)->next;
    if(*at)*at=p->next;
}
static void resource_complete(webnet *net,uint64_t id,uint64_t generation,
                              const struct webnet_response *response,void *opaque) {
    (void)net;(void)generation;struct pending *p=opaque;struct run *r=p->run;
    if(p->transport_id!=id || !r->doc)return;
    bool final=response->event==WEBNET_COMPLETE||response->event==WEBNET_END;
    struct web_response out;bool copied=copy_response(&out,response);
    bool intact=!final || unchanged(p->original,response);
    if(!copied||!intact){r->infrastructure++;snprintf(out.error,sizeof out.error,
        "%s",copied?"WPT original checksum mismatch":"WPT response allocation failed");
        /* 不一致の fixture/harness は実行せず、明示した失敗のまま返す。 */
        free(out.body);out.body=NULL;out.body_len=0;
    }
    if(response->error&&*response->error){r->infrastructure++;printf("WPT 通信失敗 %s\n",response->error);}
    if(final)unlink_request(r,p);
    web_resource_loaded(r->doc,p->native_id,&out);web_response_free(&out);
    if(final)free(p);
}
static enum webnet_kind kind(int k) {
    return k==WEB_RESOURCE_SCRIPT?WEBNET_CLASSIC:k==WEB_RESOURCE_MODULE?WEBNET_MODULE:
        k==WEB_RESOURCE_FETCH?WEBNET_FETCH:k==WEB_RESOURCE_REPORT?WEBNET_REPORT:WEBNET_RESOURCE;
}
static bool request(void *opaque,const struct web_request *in) {
    struct run *r=opaque;struct pending *p=calloc(1,sizeof *p);if(!p){r->infrastructure++;return false;}
    p->run=r;p->native_id=in->id;
    if(!strcmp(in->url,r->harness_url))p->original=&originals[3];
    else if(!strcmp(in->url,r->report_url))p->original=&originals[4];
    else for(unsigned i=9;i<sizeof originals/sizeof *originals;i++) {
        if(originals[i].tests)continue;
        char *url=NULL;
        if(web_resolve_url_owned(web_response_url(&r->navigation),originals[i].path,&url)==1 &&
           !strcmp(in->url,url))p->original=&originals[i];
        free(url);if(p->original)break;
    }
    struct webnet_request out={.kind=kind(in->kind),.generation=1,.url=in->url,.origin=in->origin,
        .method=in->method,.headers=in->headers,.body=in->body,.body_len=in->body_len,
        .credentials=(enum webnet_credentials)in->credentials,.force_preflight=in->force_preflight,
        .redirect_error=in->redirect_error,.same_origin=in->same_origin,.no_cors=in->no_cors,
        .no_referrer=in->no_referrer,.keepalive=in->keepalive,.image_upgrade=in->image_upgrade,
        .fetch_group=in->fetch_group,.cache_mode=(enum webnet_cache)in->cache_mode,
        .stream_response=in->stream_response};
    p->transport_id=webnet_submit(r->net,&out,resource_complete,p);
    if(!p->transport_id){free(p);r->infrastructure++;return false;}
    p->next=r->pending;r->pending=p;return true;
}
static void cancel(void *opaque,uint64_t id) {
    struct run *r=opaque;for(struct pending *p=r->pending;p;p=p->next)if(p->native_id==id){
        unlink_request(r,p);webnet_cancel(r->net,p->transport_id);free(p);return;
    }
}
static void resume(void *opaque,uint64_t id) {
    struct run *r=opaque;for(struct pending *p=r->pending;p;p=p->next)if(p->native_id==id){
        webnet_resume(r->net,p->transport_id);return;
    }
}
/* The standalone runner supplies the same native-only module transport seam
   as the browser, instead of refusing every synchronous dependency import. */
static bool wait_request(void *opaque,uint64_t id,const bool *complete) {
    struct run *r=opaque;
    while(!*complete && uptime_ms()<r->deadline) {
        bool found=false;
        for(struct pending *p=r->pending;p;p=p->next)if(p->native_id==id){found=true;break;}
        if(!found)return false;
        webnet_pump(r->net,uptime_ms());if(!*complete)n_sleep_ms(1);
    }
    return *complete;
}
struct sync_module {bool done,valid;struct web_response response;};
static void sync_complete(webnet *net,uint64_t id,uint64_t generation,
                          const struct webnet_response *response,void *opaque) {
    (void)net;(void)id;(void)generation;
    struct sync_module *m=opaque;m->valid=copy_response(&m->response,response);m->done=true;
}
static bool sync_request(void *opaque,const struct web_request *in,struct web_response *out) {
    struct run *r=opaque;struct sync_module m={0};memset(out,0,sizeof *out);
    struct webnet_request request={.kind=WEBNET_MODULE,.generation=1,.url=in->url,.origin=in->origin,
        .method="GET",.credentials=(enum webnet_credentials)in->credentials,
        .same_origin=in->same_origin,.redirect_error=in->redirect_error,.cache_mode=(enum webnet_cache)in->cache_mode};
    uint64_t id=webnet_submit(r->net,&request,sync_complete,&m);
    if(!id)return false;
    while(!m.done && uptime_ms()<r->deadline){webnet_pump(r->net,uptime_ms());if(!m.done)n_sleep_ms(1);}
    if(!m.done||!m.valid){webnet_cancel(r->net,id);web_response_free(&m.response);return false;}
    *out=m.response;return !out->error[0]&&out->status>=200&&out->status<300;
}
static void navigation_complete(webnet *net,uint64_t id,uint64_t generation,
                                const struct webnet_response *response,void *opaque) {
    (void)net;(void)id;(void)generation;struct run *r=opaque;
    if(response->event!=WEBNET_COMPLETE)return;
    if(!unchanged(r->original,response)||!copy_response(&r->navigation,response))r->infrastructure++;
    r->navigation_done=true;
}
static void console(void *opaque,int level,const char *message) {
    struct run *r=opaque;if(!message)return;
    printf("%s\n",message);fflush(stdout);
    if(!strcmp(message,"NOCTURNE_WPT_OBSERVER_READY"))r->observer_ready=true;
    else if(!strcmp(message,"NOCTURNE_WPT_REF_LOADED")){r->observer_ready=true;r->complete=true;}
    else if(!strncmp(message,"NOCTURNE_WPT_COMPLETE ",22)){
        int status=-1;unsigned count=0,bad=0;char tail;
        if(r->complete||sscanf(message+22,"%d %u %u %c",&status,&count,&bad,&tail)!=3||bad>count){
            r->infrastructure++;return;
        }
        r->complete=true;r->harness_status=status;r->result_count=count;r->failed=bad;
    }else if(level==2){
        /* harness の原結果は別に保存し、例外を PASS へ隠さない。取得できなかった資源の報告
           （Resource #）は例外ではない：頁がわざと失敗させる取得（SRI・MIME）もあり、
           その成否は harness の結果が持つ。 */
        if(strncmp(message,"Diagnostic #",12)&&strncmp(message,"Source ",7)&&strncmp(message,"Resource #",10))r->infrastructure++;
    }
}
static void scroll_position(void *opaque,int *x,int *y) {
    struct run *r=opaque;*x=r->scroll_x;*y=r->scroll_y;
}
static void scroll_to(void *opaque,int x,int y) {
    struct run *r=opaque;int mx=r->doc?web_doc_width(r->doc)-VIEW_W:0;
    int my=r->doc?web_doc_height(r->doc)-VIEW_H:0;if(mx<0)mx=0;if(my<0)my=0;
    r->scroll_x=x<0?0:x>mx?mx:x;r->scroll_y=y<0?0:y>my?my:y;
    if(r->doc)web_viewport_position(r->doc,r->scroll_x,r->scroll_y);
}
static unsigned window_state(void *opaque) {(void)opaque;return WEB_WINDOW_VISIBLE|WEB_WINDOW_FOCUSED;}
static void close_run(struct run *r) {
    if(r->doc){web_free(r->doc);r->doc=NULL;}
    if(r->net){webnet_free(r->net);r->net=NULL;}
    while(r->pending){struct pending *next=r->pending->next;free(r->pending);r->pending=next;}
    web_response_free(&r->navigation);free(r->harness_url);free(r->report_url);free(r->pixels);
}
static bool run_case(const char *base,const struct original *original,unsigned *results,unsigned *failed) {
    struct run r={.original=original,.harness_status=-1};char *url=NULL;
    uint64_t begin=uptime_ms();bool ok=false;
    if(web_resolve_url_owned(base,original->path,&url)!=1||
       web_resolve_url_owned(base,originals[3].path,&r.harness_url)!=1||
       web_resolve_url_owned(base,originals[4].path,&r.report_url)!=1)goto end;
    printf("WPT 開始 %s viewport %dx%d\n",url,VIEW_W,VIEW_H);fflush(stdout);
    r.net=webnet_create();if(!r.net)goto end;
    struct webnet_request nav={.kind=WEBNET_NAVIGATION,.generation=1,.url=url,.origin=url,
        .method="GET",.user_navigation=true,.credentials=WEBNET_CREDENTIALS_SAME_ORIGIN};
    if(!webnet_submit(r.net,&nav,navigation_complete,&r))goto end;
    while(!r.navigation_done&&uptime_ms()-begin<CASE_MS){webnet_pump(r.net,uptime_ms());n_sleep_ms(1);}
    if(!r.navigation_done||r.infrastructure)goto end;
    r.deadline=begin+CASE_MS;
    struct web_host host={.opaque=&r,.request=request,.cancel=cancel,.release_request=cancel,
        .sync_request=sync_request,.wait_request=wait_request,
        .resume_request=resume,.console=console,.scroll=scroll_position,.scroll_to=scroll_to,
        .window_state=window_state,.js_task_budget_ms=CASE_MS};
    r.doc=web_live_response(r.navigation.body,r.navigation.body_len,
        web_response_url(&r.navigation),"utf-8",web_response_headers(&r.navigation),NULL,&host);
    if(!r.doc)goto end;
    r.pixels=calloc((size_t)VIEW_W*VIEW_H,sizeof *r.pixels);if(!r.pixels)goto end;
    /* realm 初期化済み・HTML parser の初回 tick より前。 */
    if(!web_console_eval(r.doc,observer,sizeof observer-1))goto end;
    web_layout(r.doc,VIEW_W,VIEW_H);
    while(!r.complete&&uptime_ms()-begin<CASE_MS){
        uint64_t now=uptime_ms();webnet_pump(r.net,now);web_tick(r.doc,now);
        bool dirty=web_dirty(r.doc),paint=web_paint_dirty(r.doc);
        if(dirty)web_layout(r.doc,VIEW_W,VIEW_H);
        if(dirty||paint){canvas_t canvas;gfx_init(&canvas,r.pixels,VIEW_W,VIEW_H,VIEW_W);
            web_paint(r.doc,&canvas,0,0,VIEW_W,VIEW_H,r.scroll_x,r.scroll_y);}
        n_sleep_ms(1);
    }
    *results+=r.result_count;*failed+=r.failed;
    ok=r.complete&&r.observer_ready&&r.harness_status==0&&
        (original->sha256?r.result_count==original->tests:r.result_count>0)&&!r.failed&&!r.infrastructure;
end:
    printf("WPT 終了 %s 完了 %d harness %d ケース %u 失敗 %u 実行基盤失敗 %u elapsed %lu ms %s\n",
        original->path,r.complete,r.harness_status,r.result_count,r.failed,r.infrastructure,
        (unsigned long)(uptime_ms()-begin),ok?"PASS":"FAIL");fflush(stdout);
    close_run(&r);free(url);return ok;
}
static bool render_reference(const char *base,const struct original *original,uint32_t **pixels) {
    struct run r={.original=original,.harness_status=-1};char *url=NULL;
    uint64_t begin=uptime_ms();bool ok=false;unsigned settled=0;
    if(web_resolve_url_owned(base,original->path,&url)!=1||
       web_resolve_url_owned(base,originals[3].path,&r.harness_url)!=1||
       web_resolve_url_owned(base,originals[4].path,&r.report_url)!=1)goto end;
    printf("WPT 画像開始 %s viewport %dx%d\n",url,VIEW_W,VIEW_H);fflush(stdout);
    r.net=webnet_create();if(!r.net)goto end;
    struct webnet_request nav={.kind=WEBNET_NAVIGATION,.generation=1,.url=url,.origin=url,
        .method="GET",.user_navigation=true,.credentials=WEBNET_CREDENTIALS_SAME_ORIGIN};
    if(!webnet_submit(r.net,&nav,navigation_complete,&r))goto end;
    while(!r.navigation_done&&uptime_ms()-begin<CASE_MS){webnet_pump(r.net,uptime_ms());n_sleep_ms(1);}
    if(!r.navigation_done||r.infrastructure)goto end;
    r.deadline=begin+CASE_MS;
    struct web_host host={.opaque=&r,.request=request,.cancel=cancel,.release_request=cancel,
        .sync_request=sync_request,.wait_request=wait_request,.resume_request=resume,
        .console=console,.scroll=scroll_position,.scroll_to=scroll_to,
        .window_state=window_state,.js_task_budget_ms=CASE_MS};
    r.doc=web_live_response(r.navigation.body,r.navigation.body_len,
        web_response_url(&r.navigation),"utf-8",web_response_headers(&r.navigation),NULL,&host);
    if(!r.doc)goto end;
    r.pixels=calloc((size_t)VIEW_W*VIEW_H,sizeof *r.pixels);if(!r.pixels)goto end;
    if(!web_console_eval(r.doc,reference_observer,sizeof reference_observer-1))goto end;
    while(settled<2&&!r.infrastructure&&uptime_ms()-begin<CASE_MS){
        uint64_t now=uptime_ms();webnet_pump(r.net,now);web_tick(r.doc,now);
        if(web_dirty(r.doc))web_layout(r.doc,VIEW_W,VIEW_H);
        if(r.complete&&!r.pending&&!web_render_blocked(r.doc)){
            canvas_t canvas;gfx_init(&canvas,r.pixels,VIEW_W,VIEW_H,VIEW_W);
            web_paint(r.doc,&canvas,0,0,VIEW_W,VIEW_H,0,0);
            if(!web_dirty(r.doc)&&!web_paint_dirty(r.doc))settled++;else settled=0;
        }else settled=0;
        if(settled<2)n_sleep_ms(1);
    }
    ok=r.complete&&r.observer_ready&&settled==2&&!r.infrastructure;
    if(ok){*pixels=r.pixels;r.pixels=NULL;}
end:
    printf("WPT 画像終了 %s load %d settled %u 実行基盤失敗 %u elapsed %lu ms %s\n",
        original->path,r.complete,settled,r.infrastructure,
        (unsigned long)(uptime_ms()-begin),ok?"PASS":"FAIL");fflush(stdout);
    close_run(&r);free(url);return ok;
}
static int reference_case(const char *base) {
    uint32_t *test=NULL,*reference=NULL;unsigned infrastructure=0,mismatches=0;
    /* Both unmodified originals use the same native font state and viewport.
       No fuzzy threshold, cropping or expected-pixel exclusion is permitted. */
    if(!render_reference(base,&originals[22],&test))infrastructure++;
    if(!render_reference(base,&originals[23],&reference))infrastructure++;
    if(!infrastructure)for(size_t i=0;i<(size_t)VIEW_W*VIEW_H;i++){
        if(test[i]==reference[i])continue;
        if(!mismatches)printf("WPT 最初の画素差 %zu,%zu actual %08x reference %08x\n",i%VIEW_W,i/VIEW_W,test[i],reference[i]);
        mismatches++;
    }
    printf("限定WPT画像 1 組 %u 不一致画素 %u 実行基盤失敗\n",mismatches,infrastructure);
    free(test);free(reference);return infrastructure||mismatches?1:0;
}
int main(int argc,char **argv) {
    unsigned results=0,failed=0,page_failures=0;
    /* --run <origin> <test path>...: any testharness page of the checkout, unchanged. */
    if(argc>1&&!strcmp(argv[1],"--run")) {
        if(argc<4){printf("使用方法: wptperformancetest --run <WPT checkout の HTTP origin> <試験の道>...\n");return 2;}
        for(int i=3;i<argc;i++) {
            struct original listed={argv[i],NULL,0,0};
            if(!run_case(argv[2],&listed,&results,&failed))page_failures++;
        }
        printf("WPT %d ページ %u ケース %u 失敗 %u ページ失敗\n",argc-3,results,failed,page_failures);
        return page_failures?1:0;
    }
    bool compat=argc>1&&!strcmp(argv[1],"--compat");
    bool module=argc>1&&!strcmp(argv[1],"--modulepreload");
    bool geometry=argc>1&&!strcmp(argv[1],"--observer-geometry");
    bool inline_rect=argc>1&&!strcmp(argv[1],"--observer-inline");
    bool flex=argc>1&&!strcmp(argv[1],"--flex-invalid");
    bool radius=argc>1&&!strcmp(argv[1],"--radius");
    bool radius_computed=argc>1&&!strcmp(argv[1],"--radius-computed");
    bool layers=argc>1&&!strcmp(argv[1],"--layers");
    bool layers_failed=argc>1&&!strcmp(argv[1],"--layers-failed");
    bool ellipsis=argc>1&&!strcmp(argv[1],"--text-overflow-ref");
    bool mode=compat||module||geometry||inline_rect||flex||radius||radius_computed||layers||layers_failed||ellipsis;
    if(argc>(mode?3:2)){printf("使用方法: wptperformancetest [--compat|--modulepreload|--observer-geometry|--observer-inline|--flex-invalid|--radius|--radius-computed|--layers|--layers-failed|--text-overflow-ref] [WPT checkout の HTTP origin]\n");return 2;}
    const char *base=argc>(mode?2:1)?argv[mode?2:1]:"http://10.0.2.2:8766/";
    if(ellipsis)return reference_case(base);
    unsigned start=layers_failed?20:layers?18:radius_computed?16:radius?14:flex?12:geometry||inline_rect?9:module?8:compat?5:0;
    unsigned count=layers_failed?2:layers?4:radius_computed?1:radius?3:flex||inline_rect?1:geometry?2:module?1:compat?4:3;
    for(unsigned i=start;i<start+count;i++)if(!run_case(base,&originals[i],&results,&failed))page_failures++;
    printf("限定WPT %u ページ %u 原ケース %u 失敗 %u ページ失敗\n",count,results,failed,page_failures);
    return page_failures?1:0;
}
