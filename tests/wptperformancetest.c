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
     "3777176fc736b0a78cf7617b9613b220f635daae7536ec7ebb99c61dbb525aae",1231,0}
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

static bool unchanged(const struct original *o,const struct webnet_response *r) {
    if(!o)return true;
    if(r->status!=200 || (r->error&&*r->error) || r->body_len!=o->bytes || !r->body)return false;
    br_sha256_context context; unsigned char digest[32];char hex[65];
    static const char digits[]="0123456789abcdef";
    br_sha256_init(&context);br_sha256_update(&context,r->body,r->body_len);br_sha256_out(&context,digest);
    for(unsigned i=0;i<32;i++){hex[2*i]=digits[digest[i]>>4];hex[2*i+1]=digits[digest[i]&15];}hex[64]=0;
    printf("WPT 原本 %s %zu bytes SHA256 %s\n",o->path,r->body_len,hex);
    return !strcmp(hex,o->sha256);
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
    else if(!strncmp(message,"NOCTURNE_WPT_COMPLETE ",22)){
        int status=-1;unsigned count=0,bad=0;char tail;
        if(r->complete||sscanf(message+22,"%d %u %u %c",&status,&count,&bad,&tail)!=3||bad>count){
            r->infrastructure++;return;
        }
        r->complete=true;r->harness_status=status;r->result_count=count;r->failed=bad;
    }else if(level==2){
        /* harness の原結果は別に保存し、例外を PASS へ隠さない。 */
        if(strncmp(message,"Diagnostic #",12)&&strncmp(message,"Source ",7))r->infrastructure++;
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
    while(!r.navigation_done&&uptime_ms()-begin<CASE_MS){webnet_pump(r.net,uptime_ms());msleep(1);}
    if(!r.navigation_done||r.infrastructure)goto end;
    struct web_host host={.opaque=&r,.request=request,.cancel=cancel,.release_request=cancel,
        .resume_request=resume,.console=console,.scroll=scroll_position,.scroll_to=scroll_to,
        .window_state=window_state,.js_task_budget_ms=CASE_MS};
    r.doc=web_live_response(r.navigation.body,r.navigation.body_len,
        web_response_url(&r.navigation),"utf-8",web_response_headers(&r.navigation),&host);
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
        msleep(1);
    }
    *results+=r.result_count;*failed+=r.failed;
    ok=r.complete&&r.observer_ready&&r.harness_status==0&&r.result_count==original->tests&&
        !r.failed&&!r.infrastructure;
end:
    printf("WPT 終了 %s 完了 %d harness %d ケース %u 失敗 %u 実行基盤失敗 %u elapsed %lu ms %s\n",
        original->path,r.complete,r.harness_status,r.result_count,r.failed,r.infrastructure,
        (unsigned long)(uptime_ms()-begin),ok?"PASS":"FAIL");fflush(stdout);
    close_run(&r);free(url);return ok;
}
int main(int argc,char **argv) {
    if(argc>2){printf("使用方法: wptperformancetest [WPT checkout の HTTP origin]\n");return 2;}
    const char *base=argc==2?argv[1]:"http://10.0.2.2:8766/";
    unsigned results=0,failed=0,page_failures=0;
    for(unsigned i=0;i<3;i++)if(!run_case(base,&originals[i],&results,&failed))page_failures++;
    printf("限定WPT 3 ページ %u 原ケース %u 失敗 %u ページ失敗\n",results,failed,page_failures);
    return page_failures?1:0;
}
