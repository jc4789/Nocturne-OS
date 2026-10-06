/* Native SVG geometry/cascade/raster regressions, not live-site acceptance. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "web.h"

static int total, failed;
static uint32_t pixels[320 * 240];
static void check(int ok, const char *name) {
    total++;
    if (!ok) { failed++; printf("FAIL SVG %s\n", name); }
}
static web_doc *load(const char *markup) {
    char html[4096];
    snprintf(html, sizeof html, "<body style='margin:0;font-size:0;line-height:0'>%s", markup);
    web_doc *d = web_parse(html, strlen(html), "https://svg.test/", NULL);
    while (web_pending_stylesheet(d)) web_stylesheet_loaded(d, NULL, 0);
    web_layout(d, 320, 240);
    return d;
}
static void size(const char *name, const char *markup, int px, int py, int wantw, int wanth) {
    web_doc *d = load(markup);
    web_node *n = web_node_at(d, px, py);
    int x=0,y=0,w=0,h=0;
    int got = n && web_node_rect(d, n, &x, &y, &w, &h);
    check(got && w == wantw && h == wanth, name);
    if (!got || w != wantw || h != wanth) printf("  rect=%d,%d %dx%d expected %dx%d\n",x,y,w,h,wantw,wanth);
    web_free(d);
}
static void paint(web_doc *d) {
    canvas_t canvas;
    gfx_init(&canvas, pixels, 320, 240, 320);
    web_paint(d, &canvas, 0, 0, 320, 240, 0, 0);
}
static int rgb(int x, int y) { return pixels[y * 320 + x] & 0xFFFFFF; }
int main(void) {
    size("explicit dimensions", "<svg width=120 height=60><rect width=120 height='60'/></svg>", 1,1,120,60);
    size("author cascade", "<style>svg{width:40px;height:20px}</style><svg width=120 height=60></svg>",1,1,40,20);
    size("important cascade", "<style>svg{width:50px!important;height:25px!important}</style><svg style='width:40px;height:20px'></svg>",1,1,50,25);
    size("units preserved", "<svg width=2em height=1em style=font-size:10px></svg>",1,1,20,10);
    size("inline definite containing block", "<a style='display:inline-block;box-sizing:border-box;width:40px;height:40px;padding:8px'><svg viewbox='0 0 24 24'></svg></a>",9,9,24,24);
    size("viewBox only indefinite height", "<div style=width:100px><svg viewBox='0 0 20 10'></svg></div>",1,1,100,50);
    size("comma viewBox", "<div style=width:100px><svg viewbox='0,0,20,10'></svg></div>",1,1,100,50);
    size("viewBox exponent", "<div style=width:100px><svg viewbox='0 0 2e1 1e1'></svg></div>",1,1,100,50);
    size("one explicit dimension", "<svg width=60 viewbox='0 0 20 10'></svg>",1,1,60,30);
    size("percent dimensions", "<div style='width:200px;height:100px'><svg width=50% height=50% viewbox='0 0 20 10'></svg></div>",1,1,100,50);
    size("max width retains ratio", "<div style=width:200px><svg style=max-width:100px viewbox='0 0 20 10'></svg></div>",1,1,100,50);
    size("invalid length not numeric prefix", "<div style='width:80px;height:40px'><svg width=24garbage height=wat></svg></div>",1,1,80,40);
    size("negative size becomes auto", "<div style='width:80px;height:40px'><svg width=-2 height=-3></svg></div>",1,1,80,40);
    size("nonSVG iframe preserved", "<iframe></iframe>",1,1,304,154);
    size("nonSVG explicit image preserved", "<img width=60 height=30>",1,1,60,30);
    /* Quote the last attribute: HTML fill=red/> means the color is red/. */
    const char *zero[] = {
        "<svg width=0 height=20><rect width=20 height=20 fill='red'/></svg>",
        "<svg width=20 height=0><rect width=20 height=20 fill='red'/></svg>",
        "<svg width=20 height=20 viewbox='0 0 0 20'><rect width=20 height=20 fill='red'/></svg>",
        "<svg width=20 height=20 viewbox='0 0 20 0'><rect width=20 height=20 fill='red'/></svg>"
    };
    for (int i=0;i<4;i++) {
        web_doc *d=load(zero[i]);paint(d);int red=0;
        for(int p=0;p<320*240;p++)if((pixels[p]&0xFFFFFF)==0xFF0000)red++;
        check(!red,"zero viewport no paint");web_free(d);
    }
    web_doc *d=load("<svg width=24 height=24 viewbox='0 0 24 24'><rect x=4 y=4 width=16 height=16 fill='red'/></svg>");
    paint(d);
    check(rgb(2,2)!=0xFF0000,"lowercase viewBox preserves padding");
    check(rgb(6,6)==0xFF0000,"lowercase viewBox inner paint");
    web_free(d);
    const char *invalid[]={"0 0 -20 20","0 0 20 -20","0 0 NaN 20","0 0 20 20 trailing","0,,0,20,20"};
    for(int i=0;i<5;i++){
        char text[256];snprintf(text,sizeof text,"<svg width=24 height=24 viewbox='%s'><rect x=4 y=4 width=16 height=16 fill='red'/></svg>",invalid[i]);
        d=load(text);paint(d);check(rgb(2,2)!=0xFF0000&&rgb(6,6)==0xFF0000,"invalid viewBox ignored");web_free(d);
    }
    printf("svgtest: %d checks, %d failed\n",total,failed);
    return failed!=0;
}
