/* Native CSS translation geometry, raster, hit and overflow; not site acceptance. */
#include <stdio.h>
#include <string.h>
#include <math.h>
#include <nocturne.h>
#include "webi.h"
static int checks,failures;
static uint32_t pixels[400*240];
static void check(bool ok,const char *name){checks++;if(!ok){failures++;printf("FAIL translate %s\n",name);}}
static bool near(float a,float b){return fabsf(a-b)<.1f;}
static node_t *find(node_t *n,const char *id){if(!n)return NULL;const char *own=node_attr(n,"id");if(own&&!strcmp(own,id))return n;for(node_t *c=n->first;c;c=c->next){node_t *v=find(c,id);if(v)return v;}return NULL;}
static void support(const char *property,const char *value,bool expected){check(css_supports_declaration(property,strlen(property),value,strlen(value))==expected,value);}
static void style(web_doc *d,node_t *n,const char *value){doc_node_attr(d,n,"style",value);web_layout(d,400,240);}
int main(void){
    support("transform","translateX(-100%)",true);
    support("transform","translate(10px, 20%) translateY(calc(10px + 5%))",true);
    support("transform","translate3d(-100%, 0, 0px)",true);
    support("transform","translate3d(0, 0, 1px)",false);
    support("transform","translate3d(0, 0, 0%)",false);
    support("transform","translateX(3)",false);
    support("transform","translateX(calc(3))",false);
    support("transform","translateX(2px) rotate(2deg)",false);
    support("transform","translateX(1px, 2px)",false);
    support("translate","calc(10% + 2px) -20px",true);
    support("translate","1px 2px 3px",false);
    const char *html="<!doctype html><html><head><style>html,body{margin:0}</style></head><body>"
        "<div id=panel style='width:100px;height:40px;padding:10px;border:5px solid black;background:red;transform:translateX(-100%);translate:20px 5px'>"
        "<a id=link href='/target' style='display:block;width:20px;height:20px'></a>"
        "<div id=fixed style='position:fixed;left:80px;top:25px;width:5px;height:5px'></div></div>"
        "<div id=flow style='width:10px;height:10px;background:blue'></div>"
        "<div id=scroller style='width:100px;height:30px;overflow:auto'><div id=shift style='width:20px;height:10px;transform:translateX(140px)'></div></div>"
        "<div id=wrapper style='display:inline-flex;width:100px;height:30px;transform:translateX(30px)'><div id=child style='width:10px;height:10px'></div></div>"
        "<span id=inline style='transform:translateX(100px)'><span id=inlinefixed style='position:fixed;left:0;top:0'></span></span>"
        "</body></html>";
    web_doc *d=web_live(html,strlen(html),"https://translation.test/","utf-8",NULL);
    check(d!=NULL,"native document");if(!d)goto done;
    for(int i=0;i<8;i++)web_tick(d,uptime_ms());web_layout(d,400,240);
    node_t *panel=find(d->root,"panel"),*link=find(d->root,"link"),*fixed=find(d->root,"fixed"),*flow=find(d->root,"flow"),
        *scroller=find(d->root,"scroller"),*shift=find(d->root,"shift"),*wrapper=find(d->root,"wrapper"),*child=find(d->root,"child"),*inlinefixed=find(d->root,"inlinefixed");
    check(panel&&link&&fixed&&flow&&scroller&&shift&&wrapper&&child&&inlinefixed,"native nodes");
    if(!panel||!panel->box||!link||!fixed||!fixed->box||!flow||!flow->box||!scroller||!scroller->box||!shift||!wrapper||!wrapper->box||!child||!child->box||!inlinefixed||!inlinefixed->box){web_free(d);goto done;}
    int x,y,w,h;web_node_rect(d,panel,&x,&y,&w,&h);
    check(x==-110&&y==5&&w==130&&h==70,"own border-box percentage plus individual translate");
    check(near(box_abs_x(panel->box)-panel->box->p[3]-panel->box->b[3],0)&&near(box_abs_y(flow->box),70),"normal flow unchanged");
    check(fixed->box->cb==panel->box,"transformed fixed containing block");
    check(inlinefixed->box->cb!=inlinefixed->box->parent,"non-replaced inline cannot establish transform CB");
    check(near(scroller->box->scroll_w,160),"original and positive transformed overflow union");
    check(near(box_visual_x(child->box)-box_visual_x(wrapper->box),0),"wrapped inline-flex applies translation once");
    style(d,panel,"width:100px;height:40px;padding:10px;border:5px solid black;background:red;transform:translateX(0);translate:20px 5px");
    web_node_rect(d,panel,&x,&y,&w,&h);check(x==20&&y==5&&fixed->box->cb==panel->box,"identity remains a context");
    canvas_t canvas;gfx_init(&canvas,pixels,400,240,400);web_paint(d,&canvas,0,0,400,240,0,0);
    check((pixels[12*400+27]&0xffffff)==0xff0000,"translated native raster");
    struct web_hit hit={0};check(web_hit_test(d,36,21,&hit)&&hit.kind==WEB_HIT_LINK&&hit.node==link,"real hit matches translated link");
    style(d,panel,"width:100px;height:40px;padding:10px;border:5px solid black;transform:translateX(calc(50% + 10px)) translateY(7px);translate:2px 3px");
    web_node_rect(d,panel,&x,&y,&w,&h);check(x==77&&y==10,"calc and transform list resolve against own border box");
    style(d,panel,"width:100px;height:40px;padding:10px;border:5px solid black;transform:translateX(25px);transform:rotate(20deg);translate:none");
    web_node_rect(d,panel,&x,&y,&w,&h);check(x==25,"unsupported whole declaration preserves prior cascade value");
    style(d,shift,"width:20px;height:10px;transform:translateX(-140px)");
    check(near(scroller->box->scroll_w,100),"negative transform never shrinks original overflow");
    style(d,panel,"width:100px;height:40px;padding:10px;border:5px solid black;transform:none;translate:none");
    check(fixed->box->cb!=panel->box,"none releases fixed containing block");
    style(d,panel,"width:100px;height:40px;padding:10px;border:5px solid black;transform:none;translate:0px");
    check(fixed->box->cb==panel->box,"individual identity differs from none");
    web_free(d);
done:printf("translatetest: %d checks, %d failures\n",checks,failures);return failures?1:0;
}
