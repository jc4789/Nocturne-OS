/* First failed assertions only: live parser/resource publication. */
/* One native CSS opacity animation, not transform/events or site acceptance. */
#include <stdio.h>
#include <string.h>
#include <math.h>
#include <nocturne.h>
#include "webi.h"
#include "cssom.h"

static int checks, failures, js_failures, js_checks;
static void check(const char *name, bool pass) {
    checks++;if(!pass){failures++;printf("FAIL cssanimationregression %s\n",name);}
}
static bool near(float a,float b){return fabsf(a-b)<.006f;}
static web_doc *load(const char *css) {
    const char *markup="<!doctype html><html><head></head><body></body></html>";
    web_doc *d=web_live(markup,strlen(markup),"https://css-animation.test/","utf-8",NULL);
    if(!d)return NULL;
    for(int i=0;i<8;i++)web_tick(d,uptime_ms());
    node_t *s=doc_node_create(d,N_ELEM,"style",NULL,0);
    if(!s || !doc_node_text(d,s,css,strlen(css))){web_free(d);return NULL;}
    doc_node_move(d,d->head,s,NULL);return d;
}
static node_t *element(web_doc *d,node_t *parent,const char *style) {
    node_t *n=doc_node_create(d,N_ELEM,"div",NULL,0);
    if(n){doc_node_attr(d,n,"class","sample");doc_node_attr(d,n,"style",style);doc_node_move(d,parent,n,NULL);}
    return n;
}
static bool opacity(node_t *n,float value){return n && n->style && near(n->style->opacity,value);}
static void layout(web_doc *d){web_layout(d,320,200);}
static void restyle(web_doc *d,node_t *n){css_mark_dirty(d,n);d->need_style=d->dirty=true;layout(d);}
static struct cssom_sheet *sheet(web_doc *d,const char *text) {
    int error=0;struct cssom_sheet *s=cssom_construct(d,&error);
    if(!s || cssom_replace_sync(s,text,strlen(text))!=CSSOM_OK)return NULL;return s;
}
static void sampling(void) {
    web_doc *d=load("html,body{margin:0}@keyframes fade{to{opacity:.8}}"
        "@keyframes full{from{opacity:0}to{opacity:1}}"
        "@keyframes duplicate{0%{opacity:0}50%{opacity:.2}50%{opacity:.6}100%{opacity:1}}"
        "@keyframes timing{0%{opacity:0}0%{animation-timing-function:step-start}to{opacity:1}}"
        "@keyframes invalid{0%,bad{opacity:0}to{opacity:.8!important}}"
        "@keyframes vars{to{opacity:var(--end,.55)}}");
    ;if(!d)return;
    node_t *a=element(d,d->body,"opacity:.2;animation:fade 1s linear -.5s both paused"),
        *b=element(d,d->body,"opacity:.2;animation:fade 1s linear 1s backwards paused"),
        *c=element(d,d->body,"opacity:.2;animation:fade 1s linear 1s forwards paused"),
        *z=element(d,d->body,"opacity:.2;animation:fade 0s linear forwards"),
        *r=element(d,d->body,"animation:full 1s linear -.25s reverse both paused"),
        *alt=element(d,d->body,"animation:full 1s linear -1.25s 3 alternate both paused"),
        *altrev=element(d,d->body,"animation:full 1s linear -1.25s 3 alternate-reverse both paused"),
        *fraction=element(d,d->body,"animation:full 1s linear -9s 2.5 forwards paused"),
        *dup=element(d,d->body,"animation:duplicate 1s linear -.5s both paused"),
        *timing=element(d,d->body,"animation:timing 1s linear both paused"),
        *end=element(d,d->body,"animation:full 1s step-end -.5s both paused"),
        *invalid=element(d,d->body,"opacity:.2;animation:invalid 1s linear -.5s both paused"),
        *vars=element(d,d->body,"opacity:0;--end:.55;--duration:3.4s;animation:vars var(--duration) ease-out both;animation-play-state:paused;animation-delay:-3.4s"),
        *ease=element(d,d->body,"animation:full 1s cubic-bezier(.22,1,.36,1) -.5s both paused"),
        *important=element(d,d->body,"opacity:.3!important;animation:full 0s forwards"),
        *longhand=element(d,d->body,"animation:full 1s linear -.5s both paused;animation-duration:2s");
    node_t *delay_step=element(d,d->body,"animation:full 1s step-start 1s backwards paused"),
        *delay_reverse=element(d,d->body,"animation:full 1s step-start 1s reverse backwards paused");
    (void)b;(void)c;(void)timing;(void)invalid;(void)important;(void)delay_reverse;
    layout(d);
    check("omitted from uses underlying opacity",opacity(a,.5));
    ;
    ;
    check("zero duration forwards samples endpoint",opacity(z,.8));
    check("reverse",opacity(r,.75));
    check("alternate second iteration",opacity(alt,.75));
    check("alternate reverse second iteration",opacity(altrev,.25));
    check("fractional final iteration",opacity(fraction,.5));
    check("duplicate offset last wins",opacity(dup,.6));
    ;
    check("step end retains lower endpoint",opacity(end,0));
    ;
    check("authored var duration and endpoint",opacity(vars,.55));
    check("cubic bezier native easing",ease && ease->style && ease->style->opacity>.9f && ease->style->opacity<1);
    ;
    check("later longhand overrides shorthand",opacity(longhand,.25));
    check("delay backwards fill does not apply step-start",opacity(delay_step,0));
    ;
    ;
    ;
    (void)(css_animation_set(a,PE_NONE,1,"opacity:.65;")==1);
    restyle(d,a);;
    css_animation_set(a,PE_NONE,1,NULL);restyle(d,a);check("WAAPI removal restores CSS sample",opacity(a,.5));
    doc_node_attr(d,a,"style","opacity:.2;animation:fade 1s linear -.5s both paused;width:20px;height:20px;background:red");
    layout(d);static uint32_t pixels[320*200];canvas_t canvas;gfx_init(&canvas,pixels,320,200,320);
    web_paint(d,&canvas,0,0,320,200,0,0);
    uint32_t p=pixels[5*320+5]&0xffffff;
    check("native opacity affects raster pixels",(p>>16)==255 && ((p>>8)&255)>=120 && ((p>>8)&255)<=135 && (p&255)>=120 && (p&255)<=135);
    web_free(d);
}
static void lifecycle(void) {
    web_doc *d=load("@keyframes fade{from{opacity:0}to{opacity:1}}");
    ;if(!d)return;
    node_t *a=element(d,d->body,"animation:fade 1s linear -.25s both paused");layout(d);
    struct css_motion_state *identity=a->css_motion;
    check("paused initial negative delay",opacity(a,.25));
    msleep(20);restyle(d,a);check("paused wall time does not advance",opacity(a,.25));
    d->style_full_dirty=d->need_style=d->need_boxes=true;layout(d);
    check("full style rebuild retains playback",a->css_motion==identity && opacity(a,.25));
    doc_node_attr(d,a,"style","animation:fade 1s linear -.25s both");layout(d);
    check("resume schedules native frame",css_motion_deadline(d,uptime_ms())>=0);
    msleep(35);css_motion_tick(d,uptime_ms());layout(d);
    check("resume advances actual native time",a->style->opacity>.27f && a->style->opacity<.6f);
    doc_node_attr(d,a,"style","animation:fade 1s linear -.25s both paused");layout(d);
    msleep(20);restyle(d,a);;
    ;
    doc_node_attr(d,a,"style","opacity:.4;animation:none");layout(d);
    ;
    doc_node_attr(d,a,"style","animation:fade 1s linear both paused");layout(d);
    check("restart begins zero",opacity(a,0));
    doc_node_attr(d,a,"style","display:none;animation:fade 1s linear both");layout(d);
    ;
    doc_node_attr(d,a,"style","animation:fade 1s linear both paused");layout(d);
    check("display restoration restarts",opacity(a,0));
    doc_node_remove(d,a);;
    doc_node_move(d,d->body,a,NULL);layout(d);check("reconnection starts fresh",opacity(a,0));
    doc_node_attr(d,a,"style","animation:fade 40ms linear forwards");layout(d);
    msleep(65);css_motion_tick(d,uptime_ms());layout(d);
    ;
    ;
    doc_node_attr(d,a,"style","opacity:.4;animation:fade 0s linear");layout(d);
    ;
    d->native_cancelled=true;;
    d->native_cancelled=false;web_free(d);
}
static void cssom_scope(void) {
    web_doc *d=load("");;if(!d)return;
    const char *source="@keyframes same{to{opacity:.25}}.sample{opacity:.1;animation:same 0s linear forwards}";
    struct cssom_sheet *s=sheet(d,source);;if(!s){web_free(d);return;}
    (void)(cssom_adopt(d->root,&s,1)==CSSOM_OK);
    node_t *a=element(d,d->body,""),*host=element(d,d->body,"animation:none"),*host2=element(d,d->body,"animation:none");
    node_t *root=doc_shadow_attach(d,host,false,false,false,false,false),
        *root2=doc_shadow_attach(d,host2,false,false,false,false,false);
    struct cssom_sheet *shadow=sheet(d,"@keyframes same{to{opacity:.75}}.sample{opacity:.1;animation:same 0s forwards}");
    (void)(root && shadow && cssom_adopt(root,&shadow,1)==CSSOM_OK);
    node_t *b=element(d,root,""),*c=element(d,root2,"opacity:.1;animation:same 0s forwards");layout(d);
    node_t *nested_host=element(d,root,"animation:none"),
        *nested_root=doc_shadow_attach(d,nested_host,false,false,false,false,false),
        *nested=element(d,nested_root,"opacity:.1;animation:same 0s forwards");layout(d);
    check("document keyframes sample",opacity(a,.25));
    check("same name shadow scoped independently",opacity(b,.75));
    check("unresolved shadow name falls back to document ancestor",opacity(c,.25));
    check("nearest ancestor shadow name wins over document",opacity(nested,.75));
    const char *replacement="@keyframes same{to{opacity:.45}}.sample{opacity:.1;animation:same 0s linear forwards}";
    (void)(cssom_replace_sync(s,replacement,strlen(replacement))==CSSOM_OK);layout(d);
    check("replacement AST sampled without dangling old pointer",opacity(a,.45));
    ;
    check("replacement does not affect foreign scope",opacity(b,.75));
    const char *ordinary=".ordinary{opacity:.4}";
    (void)(cssom_insert(s,ordinary,strlen(ordinary),0)==CSSOM_OK);layout(d);
    check("insert rebuilt keyframes still sample",opacity(a,.45));
    (void)(cssom_delete(s,1)==CSSOM_OK);layout(d);
    check("deleted keyframe stops effect and identity",opacity(a,.1) && a->css_motion==NULL);
    const char *media="@media print{@keyframes same{to{opacity:.9}}}@media screen{@keyframes same{to{opacity:.35}}}"
        ".sample{opacity:.1;animation:same 0s forwards}";
    (void)(cssom_replace_sync(s,media,strlen(media))==CSSOM_OK);layout(d);
    check("media condition selects native keyframes",opacity(a,.35));
    struct cssom_sheet *later=sheet(d,"@keyframes same{to{opacity:.65}}");struct cssom_sheet *both[]={s,later};
    (void)(later && cssom_adopt(d->root,both,2)==CSSOM_OK);layout(d);
    check("later bound sheet wins named keyframes",opacity(a,.65));
    (void)(later && cssom_adopt(root2,&later,1)==CSSOM_OK);layout(d);
    check("shared immutable AST resolves per root",opacity(c,.65));
    web_free(d);
}

static bool checkpoint(void *opaque){(void)opaque;return true;}
static void console(void *opaque,int level,const char *message) {
    (void)opaque;(void)level;
    if(!strcmp(message,"CSSREG PASS unsupported-transform"))js_checks++;
    else if(!strcmp(message,"CSSREG FAIL unsupported-transform")){js_checks++;js_failures++;printf("%s\n",message);}
}
static void unsupported_transform(void) {
    const char *html="<!doctype html><html><body><div id=t></div></body></html>";
    struct web_host host={.console=console,.script_checkpoint=checkpoint};
    web_doc *d=web_live(html,strlen(html),"https://css-animation.test/","utf-8",&host);
    if(!d){js_checks++;js_failures++;return;}
    for(int i=0;i<8;i++)web_tick(d,uptime_ms());layout(d);
    const char *script="console.log('CSSREG '+(getComputedStyle(document.getElementById('t')).transform===''&&!CSS.supports('transform','translateX(1px)')?'PASS ':'FAIL ')+'unsupported-transform')";
    if(!web_console_eval(d,script,strlen(script)) || js_checks!=1){js_checks=1;js_failures++;}
    web_free(d);
}
int main(void) {
    sampling();lifecycle();cssom_scope();unsupported_transform();
    printf("cssanimationregression: %d checks, %d failures (JS %d/%d)\n",checks+js_checks,failures+js_failures,js_checks,js_failures);
    return failures || js_failures?1:0;
}
