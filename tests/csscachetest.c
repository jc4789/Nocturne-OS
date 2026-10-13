/* Completed-resource AST lifetime, not visible-site acceptance. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <nocturne.h>
#include <parallel.h>
#include "webi.h"
#include "cssom.h"
#include "frame.h"

static int checks,failed;
static void check(bool ok,const char *name){checks++;if(!ok){failed++;printf("FAIL csscache %s\n",name);}}
static bool checkpoint(void *context){(void)context;return true;}
static bool near(float a,float b){return fabsf(a-b)<0.1f;}
static node_t *element(web_doc *d,const char *tag,node_t *parent,const char *classes){
    node_t *n=doc_node_create(d,N_ELEM,tag,NULL,0);
    if(n&&classes)doc_node_attr(d,n,"class",classes);
    if(n&&parent)doc_node_move(d,parent,n,NULL);
    return n;
}
static node_t *link_sheet(web_doc *d,node_t *parent,const char *url){
    node_t *n=element(d,"link",parent,NULL);
    if(n){doc_node_attr(d,n,"rel","stylesheet");doc_node_attr(d,n,"href",url);}
    return n;
}
static char *large_css(const char *rules){
    const size_t length=40000;
    size_t n=strlen(rules);if(n>length-4)return NULL;
    char *css=malloc(length+1);if(!css)return NULL;
    memcpy(css,rules,n);css[n++]='/';css[n++]='*';
    memset(css+n,'x',length-n-2);css[length-2]='*';css[length-1]='/';css[length]=0;
    return css;
}
static uint64_t parsed_bytes(void){struct parallel_stats s={0};parallel_get_stats(PARALLEL_CSS_PARSE,&s);return s.work;}
static void rescan(web_doc *d,int width){doc_mutated(d,NULL);web_layout(d,width,600);}

static node_t *font_example(node_t *root) {
    for(node_t *n=root;n;){
        if(n->type==N_ELEM && node_attr(n,"id") && !strcmp(node_attr(n,"id"),"font-example"))return n;
        if(n->first)n=n->first;
        else {while(n&&n!=root&&!n->next)n=n->parent;if(!n||n==root)break;n=n->next;}
    }
    return NULL;
}
static bool font_queue_request(void *opaque,const struct web_request *request) {
    (void)opaque;return request->kind==WEB_RESOURCE_FETCH;
}
static int font_sources_integration(void) {
    const char *html="<!doctype html><style>"
        "@font-face{font-family:Example;src:url(unhinted.eot),url(fallback.ttf) format(truetype)}"
        "@font-face{font-family:Preserved;src:url(preserved.woff2) format(woff2);src:url(compressed.eot) format(embedded-opentype)}"
        ".example{font-family:Example,sans-serif}.preserved{font-family:Preserved,sans-serif}"
        "</style><div class=example id=font-example>Wide AWi</div><div class=preserved>Preserved</div>";
    struct web_host host={.request=font_queue_request,.script_checkpoint=checkpoint};
    web_doc *d=web_live(html,strlen(html),"https://font.test/","utf-8",&host);
    check(d!=NULL,"font source integration document");if(!d)goto done;
    /* The no-remeasurement contract is for a committed, quiet live snapshot.
       Let the actual parser/resource scan consume its initial work; manually
       clearing dirty flags on a static document would bypass that contract. */
    for(unsigned i=0;i<12;i++)web_tick(d,uptime_ms());
    web_layout(d,800,600);node_t *node=font_example(d->root);
    struct web_font_resource *first=doc_font_resource(d,"https://font.test/unhinted.eot");
    struct web_font_resource *second=doc_font_resource(d,"https://font.test/fallback.ttf");
    struct web_font_resource *preserved=doc_font_resource(d,"https://font.test/preserved.woff2");
    check(node&&node->style&&first&&second&&preserved,"actual computed style and doc-owned candidate resources");
    if(!node||!node->style||!first||!second||!preserved){web_free(d);goto done;}
    check(first->wanted&&!second->wanted,"only first unhinted candidate is initially wanted");
    check(preserved->wanted,"invalid unsupported src preserves earlier supported resource in cascade");
    bool compressed=false;for(struct web_font_resource *f=d->fonts;f;f=f->next)if(!strcmp(f->url,"https://font.test/compressed.eot"))compressed=true;
    check(!compressed,"unsupported hinted EOT is not registered for download");
    box_t *root=d->root_box;uint64_t revision=d->layout_revision;
    bool quiet=d->live&&d->layout_valid&&!d->resources_dirty&&!d->need_style&&!d->need_boxes&&
        !d->style_full_dirty&&!d->style_pending_dirty&&!d->sty.index_dirty&&
        d->sty.index_w==d->styled_w&&d->sty.index_h==d->styled_h;
    doc_font_loaded(d,first,NULL,0);web_layout(d,800,600);
    check(first->done&&first->failed&&second->wanted&&!second->done,"failed unhinted candidate advances next source without losing font user");
    check(quiet&&root==d->root_box&&revision==d->layout_revision,"failed candidate does not remeasure unchanged font metrics");
    FILE *file=fopen("/usr/share/fonts/Inter-Regular.ttf","rb");
    void *bytes=NULL;long length=0;
    if(file){fseek(file,0,SEEK_END);length=ftell(file);fseek(file,0,SEEK_SET);if(length>0)bytes=malloc((size_t)length);}
    bool loaded=bytes&&file&&fread(bytes,1,(size_t)length,file)==(size_t)length;
    check(loaded,"actual bundled fallback font bytes available");
    if(loaded){
        doc_font_loaded(d,second,bytes,(size_t)length);web_layout(d,800,600);
        check(second->done&&!second->failed&&second->face&&node->style->named_font==second->face,
              "decoded fallback publishes actual selected face into computed style");
        check(second->face&&font_has_primary(second->face,'A')&&d->layout_valid,"decoded fallback glyph and committed native layout");
    }
    if(file)fclose(file);free(bytes);web_free(d);
done:
    printf("csscache-font-sources: %d checks, %d failures\n",checks,failed);return failed?1:0;
}

static int font_failure_noop(void) {
    const char *html="<!doctype html><style>@font-face{font-family:Example;src:url(first.ttf),url(second.ttf)}"
        ".example{font-family:Example,sans-serif}</style><div class=example id=font-example>AWi</div>";
    struct web_host host={.request=font_queue_request,.script_checkpoint=checkpoint};
    web_doc *d=web_live(html,strlen(html),"https://font.test/","utf-8",&host);
    if(d){for(unsigned i=0;i<12;i++)web_tick(d,uptime_ms());web_layout(d,800,600);}
    struct web_font_resource *first=d?doc_font_resource(d,"https://font.test/first.ttf"):NULL;
    struct web_font_resource *second=d?doc_font_resource(d,"https://font.test/second.ttf"):NULL;
    bool quiet=d&&d->live&&d->layout_valid&&!d->resources_dirty&&!d->need_style&&!d->need_boxes&&
        !d->style_full_dirty&&!d->style_pending_dirty&&!d->sty.index_dirty&&
        d->sty.index_w==d->styled_w&&d->sty.index_h==d->styled_h;
    box_t *root=d?d->root_box:NULL;uint64_t revision=d?d->layout_revision:0,generation=font_metrics_generation();
    if(d&&first){doc_font_loaded(d,first,NULL,0);web_layout(d,800,600);}
    printf("font failure noop: quiet=%d resources=%d need_style=%d layout_revision=%llu/%llu generation=%llu/%llu\n",
        quiet,d?d->resources_dirty:0,d?d->need_style:0,(unsigned long long)revision,
        (unsigned long long)(d?d->layout_revision:0),(unsigned long long)generation,
        (unsigned long long)font_metrics_generation());
    check(quiet&&first&&first->failed&&second&&second->wanted&&root==d->root_box&&
          revision==d->layout_revision&&generation==font_metrics_generation(),
          "quiet live failed candidate advances selection without remeasuring unchanged metrics");
    web_free(d);printf("csscache-font-noop: %d checks, %d failures\n",checks,failed);return failed?1:0;
}

struct css_supply {uint64_t ids[16];char *urls[16];unsigned count,cancels,loads[3],errors[3];};
static bool css_supply_request(void *opaque,const struct web_request *request) {
    struct css_supply *s=opaque;
    if(request->kind!=WEB_RESOURCE_CSS)return false;
    if(s->count>=16)return false; /* fixture contains seven distinct dispatches */
    s->ids[s->count]=request->id;s->urls[s->count]=strdup(request->url);
    return s->urls[s->count++]!=NULL;
}
static void css_supply_cancel(void *opaque,uint64_t id) {(void)id;((struct css_supply *)opaque)->cancels++;}
static int css_supply_index(struct css_supply *s,const char *url,unsigned after) {
    for(unsigned i=after;i<s->count;i++)if(!strcmp(s->urls[i],url))return (int)i;return -1;
}
static void css_supply_ticks(web_doc *d){for(unsigned i=0;i<12;i++)web_tick(d,uptime_ms());}
static void css_supply_deliver(web_doc *d,struct css_supply *s,int index,const char *final,const char *text) {
    if(index<0 || (unsigned)index>=s->count){check(false,"supply response has dispatched identity");return;}
    struct web_response response={.status=200,.body=(char *)text,.body_len=strlen(text)};
    snprintf(response.headers,sizeof response.headers,"Content-Type: text/css\r\n");
    if(!web_response_set_url(&response,final)){check(false,"response URL allocation");return;}
    web_resource_loaded(d,s->ids[index],&response);free(response.url_full);css_supply_ticks(d);
}
static void css_owner_console(void *opaque,int level,const char *message) {
    struct css_supply *s=opaque;
    const char *loaded="CSS_OWNER_LOADED ",*error="CSS_OWNER_FAILED ";
    if(!strncmp(message,loaded,strlen(loaded))){char id=message[strlen(loaded)];if(id>='a'&&id<='c')s->loads[id-'a']++;}
    else if(!strncmp(message,error,strlen(error))){char id=message[strlen(error)];if(id>='a'&&id<='c')s->errors[id-'a']++;}
    else if(level==2)check(false,message);
}
static int stylesheet_owner_events(void) {
    struct css_supply supply={0};
    struct web_host host={.opaque=&supply,.request=css_supply_request,.cancel=css_supply_cancel,
        .script_checkpoint=checkpoint,.console=css_owner_console};
    const char *html="<!doctype html><head></head><body>CSS owner events</body>";
    web_doc *d=web_live(html,strlen(html),"https://css-owner.test/","utf-8",&host);
    check(d!=NULL,"stylesheet owner live document");if(!d)goto done;
    css_supply_ticks(d);web_layout(d,800,600);
    const char *create="for(const name of ['a','b','c']){const l=document.createElement('link');"
        "l.rel='stylesheet';l.href=name+'.css';l.addEventListener('load',()=>console.log('CSS_OWNER_LOADED '+name));"
        "l.addEventListener('error',()=>console.log('CSS_OWNER_FAILED '+name));document.head.append(l);}";
    check(web_console_eval(d,create,strlen(create)),"author stylesheet load listeners");css_supply_ticks(d);
    int a=css_supply_index(&supply,"https://css-owner.test/a.css",0),
        b=css_supply_index(&supply,"https://css-owner.test/b.css",0),
        c=css_supply_index(&supply,"https://css-owner.test/c.css",0);
    check(a>=0&&b>=0&&c>=0&&supply.count==3,"independent root requests dispatched");
    css_supply_deliver(d,&supply,a,"https://css-owner.test/a.css","@import 'shared.css';body{color:red}");
    int child=css_supply_index(&supply,"https://css-owner.test/shared.css",0);
    check(child>=0&&!supply.loads[0],"root load waits for its actual critical import");
    css_supply_deliver(d,&supply,b,"https://css-owner.test/b.css","@import 'shared.css';body{background:white}");
    check(supply.count==4&&!supply.loads[0]&&!supply.loads[1],"shared pending import holds both roots without duplicate fetch");
    css_supply_deliver(d,&supply,child,"https://css-owner.test/shared.css","body{font-size:16px}");
    check(supply.loads[0]==1&&supply.loads[1]==1&&!supply.loads[2]&&!supply.cancels,
        "own critical imports release load events while unrelated root stays pending");
    if(c>=0){struct web_response response={.status=404};web_resource_loaded(d,supply.ids[c],&response);css_supply_ticks(d);}
    check(supply.loads[0]==1&&supply.loads[1]==1&&!supply.loads[2]&&supply.errors[2]==1,
        "unrelated failure emits its own error without repeating completed load events");
    web_free(d);
done:
    for(unsigned i=0;i<supply.count;i++)free(supply.urls[i]);
    printf("csscache-owner-events: %d checks, %d failures\n",checks,failed);return failed?1:0;
}
static int parallel_css_integration(void) {
    struct css_supply supply={0};
    struct web_host host={.opaque=&supply,.request=css_supply_request,.cancel=css_supply_cancel,.script_checkpoint=checkpoint};
    const char *html="<!doctype html><link rel=stylesheet href=first.css><link rel=stylesheet href=first.css>"
        "<link rel=stylesheet href=second.css><link rel=stylesheet href=parent.css>"
        "<div id=font-example>CSS</div>";
    web_doc *d=web_live(html,strlen(html),"https://supply.test/page/","utf-8",&host);
    check(d!=NULL,"parallel CSS live document");if(!d)goto done;
    css_supply_ticks(d);web_layout(d,800,600);
    check(supply.count==3,"all three CSS dispatched before any response, duplicate link shares request");
    int first=css_supply_index(&supply,"https://supply.test/page/first.css",0);
    int second=css_supply_index(&supply,"https://supply.test/page/second.css",0);
    int parent=css_supply_index(&supply,"https://supply.test/page/parent.css",0);
    check(first>=0&&second>=0&&parent>=0,"distinct authored resource URLs registered");
    doc_mutated(d,NULL);web_layout(d,800,600);css_supply_ticks(d);
    check(supply.count==3,"active claims survive resource rescan without duplicate dispatch");
    css_supply_deliver(d,&supply,parent,"https://assets.supply.test/sub/parent.css","@import 'child.css';#font-example{height:19px}");
    int child=css_supply_index(&supply,"https://assets.supply.test/sub/child.css",0);
    check(child>=0&&supply.count==4,"redirect response base resolves imported child while other CSS remain active");
    css_supply_deliver(d,&supply,second,"https://supply.test/page/second.css","#font-example{width:63px}");
    css_supply_deliver(d,&supply,child,"https://assets.supply.test/sub/child.css","#font-example{height:11px;color:orange}");
    css_supply_deliver(d,&supply,first,"https://supply.test/page/first.css","#font-example{width:31px}");
    web_layout(d,800,600);node_t *node=font_example(d->root);
    check(node&&node->box&&near(node->box->w,63),"out-of-order responses preserve root stylesheet DOM source order");
    check(node&&node->box&&near(node->box->h,19)&&node->style->color==0xffffa500u,
          "import child precedes parent, with inherited root source order");
    css_supply_ticks(d);check(supply.count==4,"completed resources do not refetch on later ticks");
    node_t *old_link=link_sheet(d,d->head,"https://supply.test/page/replaced.css");
    check(old_link!=NULL,"pending stream cancellation link");web_layout(d,800,600);css_supply_ticks(d);
    unsigned boundary=supply.count;
    int old=css_supply_index(&supply,"https://supply.test/page/replaced.css",0);
    const char *replace="document.open();document.write('<!doctype html><link rel=stylesheet href=https://supply.test/page/replaced.css><div id=font-example>New stream</div>');document.close();";
    check(web_console_eval(d,replace,strlen(replace)),"document replacement uses actual author stream reset");
    css_supply_ticks(d);web_layout(d,800,600);css_supply_ticks(d);
    int fresh=css_supply_index(&supply,"https://supply.test/page/replaced.css",boundary);
    check(old>=0&&fresh>=0&&supply.ids[old]!=supply.ids[fresh]&&supply.cancels>0,
          "stream reset cancels old claim and creates distinct new same-URL request");
    if(old>=0)css_supply_deliver(d,&supply,old,"https://supply.test/page/replaced.css","#font-example{width:99px}");
    if(fresh>=0)css_supply_deliver(d,&supply,fresh,"https://supply.test/page/replaced.css","#font-example{width:41px}");
    web_layout(d,800,600);node=font_example(d->root);
    check(node&&node->box&&near(node->box->w,41),"retired completion cannot publish into new same-URL document stream");
    web_free(d);
done:
    for(unsigned i=0;i<supply.count;i++)free(supply.urls[i]);
    printf("csscache-parallel: %d checks, %d failures\n",checks,failed);return failed?1:0;
}
static int layers_import_integration(void) {
    struct css_supply supply={0};struct web_host host={.opaque=&supply,.request=css_supply_request,
        .cancel=css_supply_cancel,.script_checkpoint=checkpoint};
    const char *html="<!doctype html><link rel=stylesheet href=first.css><link rel=stylesheet href=second.css>"
        "<div id=font-example class=heading data-variant=small>Layered font</div>";
    web_doc *d=web_live(html,strlen(html),"https://layer-supply.test/","utf-8",&host);
    if(!d)return 1;css_supply_ticks(d);web_layout(d,1920,1080);
    int first=css_supply_index(&supply,"https://layer-supply.test/first.css",0);
    int second=css_supply_index(&supply,"https://layer-supply.test/second.css",0);
    check(first>=0&&second>=0,"layer roots dispatched before completion");
    css_supply_deliver(d,&supply,second,"https://layer-supply.test/second.css",
        "@layer upper{#font-example{font-size:24px;color:red}} ");
    css_supply_deliver(d,&supply,first,"https://layer-supply.test/first.css",
        "@layer upper,lower;@import url(child.css) layer( lower ) supports(display:block) screen;"
        "@import url(hidden.css) layer(hidden) supports(display:not-a-real-display);"
        "@layer upper{#font-example{font-size:24px;color:red}}");
    int child=css_supply_index(&supply,"https://layer-supply.test/child.css",0);
    check(child>=0,"supported conditional named layer import dispatched");
    check(css_supply_index(&supply,"https://layer-supply.test/hidden.css",0)<0,
        "unsupported import condition does not fetch or establish a layer");
    css_supply_deliver(d,&supply,child,"https://layer-supply.test/child.css",
        ":where(.heading):where([data-variant=small]){font:600 1rem/1.5 sans-serif;color:green}"
        "@layer nested{#font-example{width:17px}}");
    web_layout(d,1920,1080);node_t *node=font_example(d->root);
    check(node&&node->style&&node->style->font_size==16&&node->style->font_weight==600&&node->style->color==0xff008000u,
        "statement-before-import and reversed root response preserve named layer precedence");
    check(node&&node->box&&near(node->box->w,17),"imported named layer owns its nested layer rules");
    web_free(d);for(unsigned i=0;i<supply.count;i++)free(supply.urls[i]);
    printf("csscache-layer-imports: %d checks, %d failures\n",checks,failed);return failed?1:0;
}
static int layers_names_integration(void) {
    const char *html="<!doctype html><style>@layer base,override;"
        "@font-face{font-family:Unlayered;src:url(unlayered.ttf)}"
        "@layer base{@font-face{font-family:Unlayered;src:url(layered.ttf)}}"
        "@layer override{@font-face{font-family:Ordered;src:url(winner.ttf)}"
        "@font-face{font-family:Ranges;src:url(range-winner.ttf);unicode-range:U+0041}}"
        "@layer base{@font-face{font-family:Ordered;src:url(loser.ttf)}"
        "@font-face{font-family:Ranges;src:url(range-loser.ttf);unicode-range:U+0041}}"
        "#font-example{font-family:Unlayered}#ordered{font-family:Ordered}#ranges{font-family:Ranges}"
        "@keyframes unnamed{from{opacity:.7}to{opacity:.7}}"
        "@layer base{@keyframes unnamed{from{opacity:.2}to{opacity:.2}}}"
        "@layer override{@keyframes named{from{opacity:.6}to{opacity:.6}}}"
        "@layer base{@keyframes named{from{opacity:.3}to{opacity:.3}}}"
        "#motion-unlayered{animation:unnamed 1s paused}#motion-layered{animation:named 1s paused}"
        "</style><div id=font-example>A</div><div id=ordered>A</div><div id=ranges>A</div>"
        "<div id=motion-unlayered>A</div><div id=motion-layered>A</div>";
    struct web_host host={.request=font_queue_request,.script_checkpoint=checkpoint};
    web_doc *d=web_live(html,strlen(html),"https://layer-names.test/","utf-8",&host);if(!d)return 1;
    css_supply_ticks(d);web_layout(d,800,600);
    struct web_font_resource *unlayered=doc_font_resource(d,"https://layer-names.test/unlayered.ttf"),
        *layered=doc_font_resource(d,"https://layer-names.test/layered.ttf"),
        *winner=doc_font_resource(d,"https://layer-names.test/winner.ttf"),
        *loser=doc_font_resource(d,"https://layer-names.test/loser.ttf");
    check(unlayered&&layered&&unlayered->wanted&&!layered->wanted,"unlayered font-face outranks later layered definition");
    check(winner&&loser&&winner->wanted&&!loser->wanted,"font-face uses layer order rather than later lower-layer source");
    pvec found={0};css_select(d,d->root,"#ranges",&found);node_t *range=found.n?found.v[0]:NULL;pv_free(&found);
    check(range&&range->style&&range->style->font_choices&&
        !strcmp(range->style->font_choices->resource->url,"https://layer-names.test/range-winner.ttf")&&
        range->style->font_choices->next&&!strcmp(range->style->font_choices->next->resource->url,"https://layer-names.test/range-loser.ttf"),
        "unicode-range composite faces retain author fallback sorted by layer priority");
    css_select(d,d->root,"#motion-unlayered",&found);node_t *motion=found.n?found.v[0]:NULL;pv_free(&found);
    check(motion&&motion->style&&near(motion->style->opacity,.7),"unlayered keyframes outrank later layered name");
    css_select(d,d->root,"#motion-layered",&found);motion=found.n?found.v[0]:NULL;pv_free(&found);
    check(motion&&motion->style&&near(motion->style->opacity,.6),"keyframes use named layer priority");
    web_free(d);printf("csscache-layer-names: %d checks, %d failures\n",checks,failed);return failed?1:0;
}

static int initial_render_integration(void) {
    struct css_supply supply={0};
    struct web_host host={.opaque=&supply,.request=css_supply_request,.cancel=css_supply_cancel,.script_checkpoint=checkpoint};
    const char *html="<!doctype html><html><head><link rel=stylesheet href=head.css>"
        "<link rel=stylesheet media=print href=print.css></head><body>"
        "<link rel=stylesheet href=body.css><div id=font-example>CSS</div></body></html>";
    web_doc *d=web_live(html,strlen(html),"https://render.test/","utf-8",&host);
    check(d!=NULL,"initial render document");if(!d)goto done;
    d->initial_render_deadline=uptime_ms()+30000;
    css_supply_ticks(d);web_layout(d,800,600);
    check(web_render_blocked(d),"pending active head CSS blocks initial paint");
    int head=css_supply_index(&supply,"https://render.test/head.css",0);
    int body=css_supply_index(&supply,"https://render.test/body.css",0);
    check(head>=0&&body>=0,"render blocking does not suppress other resource dispatch");
    css_supply_deliver(d,&supply,head,"https://render.test/head.css","@import 'child.css';body{background:tomato}");
    web_layout(d,800,600);check(web_render_blocked(d),"head import remains an initial blocker");
    int child=css_supply_index(&supply,"https://render.test/child.css",0);
    css_supply_deliver(d,&supply,child,"https://render.test/child.css","#font-example{height:20px}");
    web_layout(d,800,600);
    check(!web_render_blocked(d),"print and body resources do not stall first paint");
    check(supply.count==4&&!supply.cancels,"paint readiness never cancels resources");
    d->initial_render_done=true;
    link_sheet(d,d->head,"https://render.test/later.css");web_layout(d,800,600);css_supply_ticks(d);
    check(!web_render_blocked(d),"later head insertion does not hide committed content");
    d->initial_render_done=false;d->initial_render_deadline=uptime_ms();
    check(!web_render_blocked(d),"paint fallback releases a stalled sheet without discarding it");
    check(supply.count==5&&!supply.cancels,"fallback preserves live request ownership");
    d->paint_dirty=false;d->paint_damage_full=false;
    web_tick(d,uptime_ms());
    check(d->initial_render_deadline==0&&web_paint_dirty(d),"fallback requests one committed repaint");
    d->paint_damage_full=false;web_tick(d,uptime_ms());
    check(!web_paint_dirty(d),"unpainted or hidden frame does not repeatedly dirty its parent");
    web_free(d);
done:
    for(unsigned i=0;i<supply.count;i++)free(supply.urls[i]);
    printf("csscache-render: %d checks, %d failures\n",checks,failed);return failed?1:0;
}
static int color_format_integration(void) {
    const char *html="<!doctype html><div id=color>color</div><svg><rect id=svg-color width=8 height=8 /></svg>";
    struct web_host host={.script_checkpoint=checkpoint};
    web_doc *d=web_live(html,strlen(html),"https://color-format.test/","utf-8",&host);
    check(d!=NULL,"color serialization live document");if(!d)goto done;
    for(unsigned i=0;i<8;i++)web_tick(d,uptime_ms());
    web_layout(d,800,600);
    static const struct {const char *input,*expected;} cases[]={
        {"#0c2238ff","rgb(12, 34, 56)"},
        {"#0c223880","rgba(12, 34, 56, 0.5)"},
        {"#0c22387f","rgba(12, 34, 56, 0.498)"},
        {"#0c2238ed","rgba(12, 34, 56, 0.93)"},
        {"#0c2238ec","rgba(12, 34, 56, 0.925)"},
        {"#0c223801","rgba(12, 34, 56, 0.004)"},
        {"#0c223800","rgba(12, 34, 56, 0)"}
    };
    for(unsigned i=0;i<sizeof cases/sizeof *cases;i++) {
        char script[400];snprintf(script,sizeof script,
            "var p=document.getElementById('color');p.style.color='%s';"
            "if(getComputedStyle(p).color!=='%s')throw Error('color format %u: '+getComputedStyle(p).color);",
            cases[i].input,cases[i].expected,i);
        check(web_console_eval(d,script,strlen(script)),cases[i].expected);
    }
    const char *svg="var p=document.getElementById('svg-color');p.setAttribute('fill','#0c2238ff');"
        "p.setAttribute('stroke','#0c223880');var s=getComputedStyle(p);"
        "if(s.fill!=='rgb(12, 34, 56)'||s.stroke!=='rgba(12, 34, 56, 0.5)')throw Error('SVG color format');";
    check(web_console_eval(d,svg,strlen(svg)),"SVG fill/stroke share opaque and alpha serialization");
    web_free(d);
done:
    printf("csscache-color-format: %d checks, %d failures\n",checks,failed);return failed?1:0;
}
static int frame_viewport_integration(void) {
    const char *html="<!doctype html><style>#wrap{width:300px}iframe{border:0;width:100%;height:100px}</style>"
        "<div id=wrap><iframe id=frame srcdoc='<style>html,body{margin:0}#t{color:red}"
        "@media(min-width:500px){#t{color:green}}</style><div id=t>viewport</div>'></iframe></div>";
    struct web_host host={.script_checkpoint=checkpoint};
    web_doc *d=web_live(html,strlen(html),"https://frame-viewport.test/","utf-8",&host);
    check(d!=NULL,"frame viewport live document");if(!d)goto done;
    for(unsigned i=0;i<24;i++)web_tick(d,uptime_ms());web_layout(d,800,600);
    const char *initial="var f=document.getElementById('frame'),t=f.contentDocument.getElementById('t');"
        "if(!t||f.contentWindow.innerWidth!==300||getComputedStyle(t).color!=='rgb(255, 0, 0)')throw Error('initial child viewport');";
    check(web_console_eval(d,initial,strlen(initial)),"initial child viewport and CSS media");
    const char *grow="document.getElementById('wrap').style.width='500px';"
        "if(f.contentWindow.innerWidth!==500||!f.contentWindow.matchMedia('(min-width:500px)').matches||"
        "getComputedStyle(t).color!=='rgb(0, 128, 0)')throw Error('parent CSS child viewport growth');";
    check(web_console_eval(d,grow,strlen(grow)),"parent CSS width immediately updates child dimensions and media without paint");
    const char *shrink="document.getElementById('wrap').style.width='300px';"
        "if(getComputedStyle(t).color!=='rgb(255, 0, 0)'||f.contentWindow.innerWidth!==300)throw Error('child viewport shrink');";
    check(web_console_eval(d,shrink,strlen(shrink)),"child computed style flushes the dirty embedding parent first");
    pvec found={0};css_select(d,d->root,"#frame",&found);node_t *node=found.n?found.v[0]:NULL;pv_free(&found);
    struct web_frame *frame=node?web_frame_find(d,node):NULL;web_doc *child=frame?frame->document:NULL;
    uint64_t parent_revision=d->layout_revision,child_revision=child?child->layout_revision:0;
    const char *quiet="if(f.contentWindow.innerWidth!==300||getComputedStyle(t).color!=='rgb(255, 0, 0)')throw Error('quiet frame read');";
    check(child&&web_console_eval(d,quiet,strlen(quiet))&&d->layout_revision==parent_revision&&child->layout_revision==child_revision,
        "unchanged child geometry reads reuse committed parent and child layouts");
    web_free(d);
done:
    printf("csscache-frame-viewport: %d checks, %d failures\n",checks,failed);return failed?1:0;
}
static node_t *selected_node(web_doc *d,const char *selector) {
    pvec nodes={0};node_t *result=NULL;
    if(css_select(d,d->root,selector,&nodes)&&nodes.n)result=nodes.v[0];
    pv_free(&nodes);return result;
}
static void pseudo_geometry_diagnostic(web_doc *d,node_t *origin,const char *phase) {
    box_t *root=d->root_box;
    printf("pseudo geometry %s: root=%p revision=%llu valid=%d style=%d boxes=%d changed=%d recovery=%d viewport=%d,%d font=%llu\n",
        phase,(void *)root,(unsigned long long)d->layout_revision,d->layout_valid,d->need_style,d->need_boxes,
        d->style_boxes_changed,d->native_recovery_pending,d->width,d->height,(unsigned long long)font_metrics_generation());
    if(root)printf("pseudo geometry %s root: dirty=%d cache=%d safe=%d post=%d font=%llu size=%.3f,%.3f post-size=%.3f,%.3f content=%.3f/%.3f\n",
        phase,root->layout_dirty,root->layout_cache_valid,root->layout_cache_safe,root->layout_post_valid,
        (unsigned long long)root->cached_font_generation,(double)root->w,(double)root->h,
        (double)root->layout_post_w,(double)root->layout_post_h,(double)root->content_dy,(double)root->layout_post_content_dy);
    if(origin&&origin->box)printf("pseudo geometry %s origin: kind=%u dirty=%d cache=%d post=%d before=%d after=%d\n",
        phase,origin->box->kind,origin->box->layout_dirty,origin->box->layout_cache_valid,origin->box->layout_post_valid,
        origin->style->before_generated,origin->style->after_generated);
}
static int pseudo_bodies_integration(void) {
    const char *html="<!doctype html><style>"
        "@font-face{font-family:UnusedPseudo;src:url(unused-pseudo.ttf)}"
        ".none::before{color:red;font-family:UnusedPseudo;background-image:url(unused-before.png)}"
        ".none::after{color:blue;content:'hidden';display:none;background-image:url(unused-after.png)}"
        ".other::before{color:lime}"
        ".replaced::before{content:'not rendered';color:red;font-family:UnusedPseudo;background-image:url(unused-replaced.png)}"
        ".real::before{content:'generated';color:red}.real::after{content:'';width:2px}"
        "</style><div id=plain class=none>plain</div><input id=replaced class=replaced>"
        "<button id=button class=real>button</button><object id=fallback class=real>fallback</object>";
    struct web_host host={.script_checkpoint=checkpoint};
    web_doc *d=web_live(html,strlen(html),"https://pseudo-body.test/","utf-8",&host);
    check(d!=NULL,"computed pseudo body document");if(!d)goto done;
    for(unsigned i=0;i<8;i++)web_tick(d,uptime_ms());web_layout(d,800,600);
    node_t *plain=selected_node(d,"#plain"),*input=selected_node(d,"#replaced"),
        *button=selected_node(d,"#button"),*object=selected_node(d,"#fallback");
    check(plain&&input&&button&&object,"all pseudo body origins exist");
    if(!plain||!input||!button||!object){web_free(d);goto done;}
    check(plain->style->before&&plain->style->before->color==RGB(255,0,0)&&
        !plain->style->before_generated,"content none preserves computed before without a box");
    check(plain->style->after&&plain->style->after->color==RGB(0,0,255)&&
        !plain->style->after_generated,"display none preserves computed after without a box");
    check(input->style->before&&input->style->before->color==RGB(255,0,0)&&
        !input->style->before_generated,"replaced input preserves authored computed body only");
    check(button->style->before_generated&&button->style->after_generated,"button keeps both generated pseudo children");
    check(object->style->before_generated&&object->style->after_generated,"object fallback keeps generated pseudo children");
    check(d->images.n==0,"nongenerated pseudo backgrounds do not create image resources");
    bool fonts_unused=true;for(struct web_font_resource *font=d->fonts;font;font=font->next)
        if(font->wanted)fonts_unused=false;
    check(fonts_unused,"nongenerated pseudo font is not requested");
    css_refresh_font_selection(d);fonts_unused=true;
    for(struct web_font_resource *font=d->fonts;font;font=font->next)if(font->wanted)fonts_unused=false;
    check(fonts_unused,"font refresh also ignores nongenerated pseudo bodies");
    box_t *root=d->root_box;uint64_t revision=d->layout_revision;
    pseudo_geometry_diagnostic(d,plain,"before-colour");
    doc_node_attr(d,plain,"class","none other");
    pseudo_geometry_diagnostic(d,plain,"mutated-colour");
    web_layout(d,800,600);
    pseudo_geometry_diagnostic(d,plain,"after-colour");
    check(plain->style->before&&plain->style->before->color==RGB(0,255,0),"nongenerated author colour updates through real cascade");
    check(root==d->root_box&&revision==d->layout_revision,"nongenerated pseudo colour change preserves published geometry");
    revision=d->layout_revision;
    doc_node_attr(d,plain,"style","width:123px");web_layout(d,800,600);
    check(plain->box&&near(plain->box->w,123)&&d->layout_revision>revision,
        "real width mutation still remeasures published geometry");
    revision=d->layout_revision;web_layout(d,900,600);
    check(d->width==900&&d->layout_revision>revision,"viewport mutation still remeasures geometry");
    revision=d->layout_revision;doc_node_attr(d,plain,"class","none real");web_layout(d,900,600);
    check(plain->style->before_generated&&d->layout_revision>revision,
        "newly generated pseudo still rebuilds geometry");
    check(d->images.n==1,"newly generated pseudo registers its real background resource");
    bool font_used=false;for(struct web_font_resource *font=d->fonts;font;font=font->next)
        if(font->wanted)font_used=true;
    check(font_used,"newly generated pseudo selects its real font resource");
    web_free(d);
done:
    printf("csscache-pseudo-bodies: %d checks, %d failures\n",checks,failed);return failed?1:0;
}
int main(int argc,char **argv){
    if(argc>1&&!strcmp(argv[1],"--pseudo-bodies"))return pseudo_bodies_integration();
    if(argc>1&&!strcmp(argv[1],"--font-sources"))return font_sources_integration();
    if(argc>1&&!strcmp(argv[1],"--font-failure-noop"))return font_failure_noop();
    if(argc>1&&!strcmp(argv[1],"--parallel-css"))return parallel_css_integration();
    if(argc>1&&!strcmp(argv[1],"--layers-import"))return layers_import_integration();
    if(argc>1&&!strcmp(argv[1],"--layers-names"))return layers_names_integration();
    if(argc>1&&!strcmp(argv[1],"--color-format"))return color_format_integration();
    if(argc>1&&!strcmp(argv[1],"--owner-events"))return stylesheet_owner_events();
    if(argc>1&&!strcmp(argv[1],"--frame-viewport"))return frame_viewport_integration();
    if(argc>1&&!strcmp(argv[1],"--render"))return initial_render_integration();
    const char *html="<!doctype html><html><head></head><body></body></html>";
    struct web_host host={.script_checkpoint=checkpoint};
    web_doc *d=web_live(html,strlen(html),"https://cache.test/page/","utf-8",&host);
    check(d!=NULL,"document");if(!d)goto done;
    d->profile_enabled=true;parallel_profile_enable(true);
    for(int i=0;i<8;i++)web_tick(d,uptime_ms());
    const char *one_url="https://cache.test/assets/one.css",*two_url="https://cache.test/assets/two.css";
    char *one_css=large_css(".value{width:40px;height:10px;color:red}"
        "@media(min-width:900px){.value{color:blue}}");
    char *two_css=large_css(".value{height:20px}");
    check(one_css&&two_css,"large native parser resources");
    if(!one_css||!two_css){free(one_css);free(two_css);web_free(d);goto done;}
    node_t *one=link_sheet(d,d->head,one_url),*value=element(d,"div",d->body,"value");
    check(one&&value,"connected stylesheet and target");
    doc_css_loaded(d,one_url,one_url,one_css,40000);
    uint64_t before=parsed_bytes();web_layout(d,800,600);
    check(parsed_bytes()-before==40000,"completed external body parses exactly once");
    check(value&&value->box&&near(value->box->w,40)&&near(value->box->h,10)&&value->style->color==0xffff0000u,
        "initial cached native AST cascades");
    before=parsed_bytes();rescan(d,800);
    check(parsed_bytes()==before,"second complete resource scan does not reparse cached body");
    node_t *ordinary=element(d,"span",d->body,NULL);
    node_t *text=doc_node_create(d,N_TEXT,NULL,"ordinary body",13);
    if(ordinary&&text)doc_node_move(d,ordinary,text,NULL);
    rescan(d,800);
    check(parsed_bytes()==before&&value->box&&near(value->box->w,40),
        "body mutation plus conservative rescan retains completed AST");
    node_t *two=link_sheet(d,d->head,two_url);doc_css_loaded(d,two_url,two_url,two_css,40000);
    before=parsed_bytes();web_layout(d,800,600);
    check(parsed_bytes()-before==40000,"only newly completed sheet parses");
    check(two&&value->box&&near(value->box->h,20),"new sheet source order is retained");
    before=parsed_bytes();web_layout(d,1000,600);
    check(parsed_bytes()==before&&value->style->color==0xff0000ffu,"viewport media flip reuses external AST");
    doc_node_attr(d,two,"media","(min-width:1100px)");web_layout(d,1000,600);
    check(parsed_bytes()==before&&value->box&&near(value->box->h,10),"link media wrapper is not cached in shared AST");
    node_t *shadow_host=element(d,"div",d->body,NULL);
    node_t *shadow=doc_shadow_attach(d,shadow_host,false,false,false,false,false);
    node_t *shadow_link=link_sheet(d,shadow,one_url),*shadow_value=element(d,"div",shadow,"value");
    web_layout(d,1000,600);
    check(shadow_link&&shadow_value&&parsed_bytes()==before&&shadow_value->box&&near(shadow_value->box->w,40),
        "same completed body has independent shadow scope without reparse");
    int error=0;struct cssom_sheet *editable=cssom_style_sheet(d,one,&error);
    check(editable&&error==CSSOM_OK,"link CSSOM promotion has independent editable AST");
    const char *edit=".value{width:99px}";
    check(editable&&cssom_insert(editable,edit,strlen(edit),2)==CSSOM_OK,"CSSOM mutation");
    web_layout(d,1000,600);
    check(value->box&&near(value->box->w,99)&&shadow_value->box&&near(shadow_value->box->w,40),
        "edited CSSOM AST wins for its owner without altering cached shared source");
    before=parsed_bytes();rescan(d,1000);
    check(parsed_bytes()==before&&value->box&&near(value->box->w,99),"rescan preserves CSSOM edit without reparsing either AST");
    const char *parent_url="https://cache.test/nested/parent.css",*child_url="https://cache.test/nested/child.css";
    char *parent_css=large_css("@import 'child.css';.value{width:80px}"),
         *child_css=large_css("@import 'parent.css';.value{width:70px;color:orange}");
    check(parent_css&&child_css,"import native resources");
    if(parent_css&&child_css){
        node_t *parent=link_sheet(d,d->head,parent_url);
        doc_css_loaded(d,parent_url,parent_url,parent_css,40000);
        doc_css_loaded(d,child_url,child_url,child_css,40000);
        before=parsed_bytes();web_layout(d,1000,600);
        check(parsed_bytes()-before==80000,"parent and imported completed bodies each parse once");
        check(parent&&value->box&&near(value->box->w,80)&&value->style->color==0xffffa500u,
            "cached import metadata restores child-before-parent order");
        check(d->pending_css.n==0,"cached import cycle retains URL ancestor rejection");
        before=parsed_bytes();rescan(d,1000);
        check(parsed_bytes()==before&&value->box&&near(value->box->w,80),"repeated import traversal does not reparse cached ASTs");
        doc_node_attr(d,parent,"media","(min-width:1100px)");web_layout(d,1000,600);
        check(parsed_bytes()==before&&value->box&&near(value->box->w,99)&&value->style->color==0xff0000ffu,
            "inactive root media applies to cached imported sheet too");
        web_layout(d,1200,600);
        check(parsed_bytes()==before&&value->box&&near(value->box->w,80)&&near(value->box->h,20),
            "viewport reactivates import and independent link media without parse");
    }
    char *inline_css=large_css(".inline_value{width:63px;height:11px}");
    node_t *inline_sheet=element(d,"style",d->head,NULL);
    node_t *inline_text=doc_node_create(d,N_TEXT,NULL,inline_css,inline_css?40000:0);
    node_t *inline_value=element(d,"div",d->body,"inline_value");
    if(inline_sheet&&inline_text)doc_node_move(d,inline_sheet,inline_text,NULL);
    before=parsed_bytes();web_layout(d,800,600);
    check(inline_css&&inline_value&&inline_value->box&&near(inline_value->box->w,63)&&parsed_bytes()-before==40000,
        "large inline AST initially parsed once");
    before=parsed_bytes();rescan(d,800);
    check(parsed_bytes()==before,"inline AST survives conservative resource rescan");
    node_t *inline_shadow=element(d,"style",shadow,NULL);
    node_t *shadow_text=doc_node_create(d,N_TEXT,NULL,inline_css,inline_css?40000:0);
    node_t *inline_shadow_value=element(d,"div",shadow,"inline_value");
    if(inline_shadow&&shadow_text)doc_node_move(d,inline_shadow,shadow_text,NULL);
    web_layout(d,800,600);
    check(parsed_bytes()==before&&inline_shadow_value&&inline_shadow_value->box&&near(inline_shadow_value->box->w,63),
        "identical inline source shares AST with independent shadow scope");
    unsigned cached=d->css_reuse_count;
    if(inline_sheet)doc_node_remove(d,inline_sheet);
    if(inline_shadow)doc_node_remove(d,inline_shadow);
    web_layout(d,800,600);
    check(d->css_reuse_count+1==cached&&inline_value->box&&!near(inline_value->box->w,63),
        "last inline owner removal retires shared cache without stale styles");
    free(inline_css);
    free(parent_css);free(child_css);free(one_css);free(two_css);
    web_free(d);
done:
    printf("csscache: %d checks, %d failures\n",checks,failed);
    return failed?1:0;
}
