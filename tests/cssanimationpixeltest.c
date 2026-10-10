/* One old pixel failure and the three directly related blend invariants. */
#include <stdio.h>
#include <string.h>
#include <nocturne.h>
#include "webi.h"

uint32_t doc_canvas_bg(web_doc *d,int *from);

static int checks, failures;
static uint32_t pixels[320*200];
static void check(const char *name,bool pass) {
    checks++;if(!pass){failures++;printf("FAIL cssanimationpixel %s\n",name);}
}
static void pixel(void) {
    const char *html="<!doctype html><html><head><style>html,body{margin:0}"
        "@keyframes fade{to{opacity:.8}}</style></head><body>"
        "<div id=target style='opacity:.2;animation:fade 1s linear -.5s both paused'></div></body></html>";
    web_doc *d=web_live(html,strlen(html),"https://css-animation.test/","utf-8",NULL);
    if(!d){check("native opacity affects raster pixels",false);return;}
    for(int i=0;i<8;i++)web_tick(d,uptime_ms());
    web_layout(d,320,200);
    node_t *n=d->body?d->body->first:NULL;
    doc_node_attr(d,n,"style","opacity:.2;animation:fade 1s linear -.5s both paused;width:20px;height:20px;background:red");
    web_layout(d,320,200);
    int x=-1,y=-1,w=-1,h=-1;bool rect=n && web_node_rect(d,n,&x,&y,&w,&h);
    canvas_t canvas;gfx_init(&canvas,pixels,320,200,320);
    web_paint(d,&canvas,0,0,320,200,0,0);
    uint32_t p=pixels[5*320+5];
    int bgfrom=-1;uint32_t bg=doc_canvas_bg(d,&bgfrom);
    printf("cssanimationpixel input rect=%d:%d,%d,%d,%d point=5,5 style_opacity=%.9g box_opacity=%.9g style_bg=%08x box_bg=%08x canvas_bg=%08x canvas_from=%d actual=%08x\n",
        rect,x,y,w,h,n&&n->style?(double)n->style->opacity:-1.,n&&n->box&&n->box->st?(double)n->box->st->opacity:-1.,
        n&&n->style?n->style->bg_color:0,n&&n->box&&n->box->st?n->box->st->bg_color:0,bg,bgfrom,p);
    check("native opacity affects raster pixels",rect && x==0 && y==0 && w==20 && h==20 &&
        (p&0xff0000)==0xff0000 && ((p>>8)&255)>=126 && ((p>>8)&255)<=128 && (p&255)>=126 && (p&255)<=128);
    web_free(d);
}
int main(void) {
    uint32_t zero=gfx_mix(0xffffffff,0xffff0000,0),
        full=gfx_mix(0xffffffff,0xffff0000,255),
        same=gfx_mix(0xffff0000,0xffff0000,128);
    printf("cssanimationpixel blend weight0=%08x weight255=%08x same128=%08x\n",zero,full,same);
    check("blend zero weight preserves destination",zero==0xffffffff);
    check("blend full weight preserves source",full==0xffff0000);
    check("blend equal channels are invariant",same==0xffff0000);
    pixel();
    printf("cssanimationpixel: %d checks, %d failures\n",checks,failures);
    return failures?1:0;
}
