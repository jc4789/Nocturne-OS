/* webtest: the web engine (libc/web) offline: layout positions, painted pixels, links, forms,
   charsets, URLs, resources and hostile input. Prints FAIL lines and "webtest: N failed". */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "nocturne.h"
#include "web.h"
#include "webi.h"
#include "web_form_validation_cases.h"

#define BASE "http://h.test/dir/page.html"
#define VW 800
#define VH 600

static int failed, total;

static void fail(const char *name, const char *fmt, int a, int b) {
    printf("FAIL %s: ", name);
    printf(fmt, a, b);
    printf("\n");
    failed++;
}

static web_doc *load(const char *html) {
    web_doc *d = web_parse(html, strlen(html), BASE, NULL);
    while (web_pending_stylesheet(d)) web_stylesheet_loaded(d, NULL, 0);
    web_layout(d, VW, VH);
    return d;
}

static web_doc *load_controls(const char *html) {
    web_doc *d=web_live(html,strlen(html),BASE,NULL,NULL);
    if(d){web_tick(d,uptime_ms());web_layout(d,VW,VH);}
    return d;
}

/* the y of #t must be want (to within a pixel) */
static void y_is(const char *name, const char *html, int want) {
    printf("WEB-STAGE %s\n",name);fflush(stdout);
    total++;
    web_doc *d = load(html);
    int y = web_anchor_y(d, "t");
    if (y < want - 1 || y > want + 1) fail(name, "#t at y=%d, expected %d", y, want);
    web_free(d);
}

static node_t *node_id(web_doc *d, const char *value) {
    for (node_t *n = d->owned_nodes; n; n = n->owned_next) {
        const char *name = node_attr(n, "id");
        if (name && !strcmp(name, value) && doc_node_connected(n)) return n;
    }
    return NULL;
}

/* the same with the viewport scrolled to view_y and the scroll container #s, if any, to scroll_y */
static void scrolled_y_is(const char *name, const char *html, int view_y, int scroll_y, int want) {
    printf("WEB-STAGE %s\n",name);fflush(stdout);
    total++;
    web_doc *d = load(html);
    web_viewport_position(d, 0, view_y);
    node_t *s = node_id(d, "s");
    if (s) s->scroll_y = scroll_y;
    int y = web_anchor_y(d, "t");
    if (y < want - 1 || y > want + 1) fail(name, "#t at y=%d, expected %d", y, want);
    web_free(d);
}

/* ---------------------------------------------------------------- layout */
static void test_layout(void) {
    y_is("block", "<body style=margin:0><div style=height:100px></div><div id=t></div>", 100);
    y_is("body-margin", "<div style=height:10px></div><p id=t style=margin:0>x</p>", 18);
    y_is("margin-collapse",
         "<body style=margin:0><div style='margin-bottom:30px;height:10px'></div><div id=t style=margin-top:20px></div>", 40);
    y_is("display-none", "<body style=margin:0><div style='display:none;height:500px'></div><div id=t></div>", 0);
    y_is("float-clear",
         "<body style=margin:0><div style='float:left;width:50px;height:70px'></div><div id=t style=clear:left></div>", 70);
    y_is("clearfix",
         "<style>.cf::after{content:'';display:table;clear:both}</style><body style=margin:0>"
         "<div class=cf><div style='float:left;height:60px;width:10px'></div></div><div id=t></div>", 60);
    y_is("float-beside", /* two floats that fit side by side do not stack */
         "<body style=margin:0><div style=width:300px><div style='float:left;width:150px;height:40px'></div>"
         "<div id=t style='float:left;width:150px;height:40px'></div></div>", 0);
    y_is("flex-column-gap",
         "<body style=margin:0><div style='display:flex;flex-direction:column;gap:5px'><div style=height:30px></div>"
         "<div id=t></div></div>", 35);
    y_is("flex-wrap",
         "<body style=margin:0><div style='display:flex;flex-wrap:wrap;width:100px'><div style='width:60px;height:20px'></div>"
         "<div style='width:60px;height:20px'></div></div><div id=t></div>", 40);
    y_is("flex-align-center",
         "<body style=margin:0><div style='display:flex;align-items:center;height:100px'><div id=t style=height:20px></div></div>", 40);
    y_is("grid-rows",
         "<body style=margin:0><div style='display:grid;grid-template-rows:40px 25px'><div></div><div></div></div><div id=t></div>", 65);
    y_is("grid-auto-placement",
         "<body style=margin:0><div style='display:grid;grid-template-columns:1fr 1fr;width:200px'><div style=height:10px></div>"
         "<div style=height:30px></div><div id=t></div></div>", 30);
    y_is("grid-areas",
         "<body style=margin:0><div style=\"display:grid;grid-template-areas:'a b' 'c c';grid-template-rows:50px 10px\">"
         "<div id=t style=grid-area:c></div><div style=grid-area:a></div></div>", 50);
    y_is("grid-scroller-spans-fr", /* a scroll container spanning a 1fr row has no content minimum */
         "<body style=margin:0><div style='display:grid;height:200px;grid-template-rows:1fr auto;"
         "grid-template-columns:1fr 50px'><div style='grid-row:1/3;grid-column:2;overflow:auto;height:100%'>"
         "<div style=height:1000px></div></div><div style='grid-row:2;grid-column:1;height:20px'></div>"
         "<div id=t style='grid-row:1;grid-column:1;align-self:end;height:10px'></div></div>", 170);
    y_is("grid-min-content-track", /* a min-content track keeps its item's min-content width */
         "<body style=margin:0><div style='display:grid;width:100px;grid-template-columns:min-content minmax(0,auto);"
         "font-size:0;line-height:0'><div><span style='display:inline-block;width:60px;height:10px'></span></div><div>"
         "<span style='display:inline-block;width:25px;height:10px'></span><span style='display:inline-block;width:25px;height:10px'></span>"
         "<span style='display:inline-block;width:25px;height:10px'></span><span style='display:inline-block;width:25px;height:10px'></span>"
         "</div></div><div id=t></div>", 40);
    y_is("grid-min-height-zero", /* min-height: 0 lets an item's row shrink below its content */
         "<body style=margin:0><div style='display:grid;height:100px;grid-template-rows:1fr 1fr'>"
         "<div style=min-height:0><div style=height:400px></div></div><div id=t></div></div>", 50);
    /* position: sticky follows the viewport or its scroll container, within its containing block */
    const char *sticky = "<body style=margin:0><div style=height:100px></div>"
        "<div id=t style='position:sticky;top:10px;height:20px'></div><div style=height:2000px></div>";
    scrolled_y_is("sticky-in-flow", sticky, 0, 0, 100);
    scrolled_y_is("sticky-viewport", sticky, 500, 0, 510);
    scrolled_y_is("sticky-containing-block",
         "<body style=margin:0><div style=height:300px><div id=t style='position:sticky;top:0;height:50px'></div></div>"
         "<div style=height:2000px></div>", 400, 0, 250);
    scrolled_y_is("sticky-bottom",
         "<body style=margin:0><div style=height:2000px></div><div id=t style='position:sticky;bottom:0;height:20px'></div>",
         0, 0, 580);
    scrolled_y_is("sticky-scroller",
         "<body style=margin:0><div style=height:40px></div><div id=s style='overflow:auto;height:200px'>"
         "<div style=height:50px></div><div id=t style='position:sticky;top:5px;height:20px'></div>"
         "<div style=height:1000px></div></div>", 0, 300, 45);
    scrolled_y_is("sticky-table-header",
         "<body style=margin:0><table style=border-spacing:0><thead><tr><th id=t style='position:sticky;top:0;padding:0'>h"
         "</th></tr></thead><tbody><tr><td style='height:1000px;padding:0'>b</td></tr></tbody></table>"
         "<div style=height:2000px></div>", 300, 0, 300);
    y_is("relative-inline-flex", /* the wrapper and its inner flex box are one element, offset once */
         "<body style=margin:0><span style='display:inline-flex;vertical-align:top;position:relative;top:30px'>"
         "<div id=t style=height:10px></div></span>", 30);
    y_is("table-rows",
         "<body style=margin:0><table style=border-spacing:0><tr><td style='height:50px;padding:0'>a<tr><td id=t>b</table>", 50);
    y_is("table-caption-bottom",
         "<body style=margin:0><table style='border-spacing:0;caption-side:bottom'><caption id=t>c</caption>"
         "<tr><td style='height:50px;padding:0'>a</table>", 50);
    y_is("abspos",
         "<body style=margin:0><div style='position:relative;height:10px;top:5px'><div id=t style='position:absolute;top:123px'></div></div>", 128);
    y_is("percent-height",
         "<body style=margin:0><div style=height:200px><div style=height:50%></div><div id=t></div></div>", 100);
    y_is("box-sizing",
         "<body style=margin:0><div style='box-sizing:border-box;height:50px;padding:10px;border:5px solid'></div><div id=t></div>", 50);
    y_is("cascade-specificity", "<style>#a{height:10px} div{height:99px}</style><body style=margin:0><div id=a></div><div id=t></div>", 10);
    y_is("cascade-important", "<style>#a{height:10px} div{height:99px!important}</style><body style=margin:0><div id=a></div><div id=t></div>", 99);
    y_is("css-var", "<style>:root{--h:33px} #a{height:var(--h)}</style><body style=margin:0><div id=a></div><div id=t></div>", 33);
    y_is("css-calc", "<body style=margin:0><div style='height:calc(10px + 2 * 7px)'></div><div id=t></div>", 24);
    y_is("anchor-name", "<body style=margin:0><div style=height:77px></div><a name=t>x</a>", 77);

    /* @media follows the viewport width */
    const char *mq = "<style>#a{height:10px} @media (max-width:500px){#a{height:300px}}</style><body style=margin:0><div id=a></div><div id=t></div>";
    total++;
    web_doc *d = load(mq);
    int y1 = web_anchor_y(d, "t");
    web_layout(d, 400, VH);
    int y2 = web_anchor_y(d, "t");
    if (y1 != 10 || y2 != 300) fail("media-query", "#t at %d / %d, expected 10 / 300", y1, y2);
    web_free(d);

    /* the document height */
    total++;
    d = load("<body style=margin:0><div style=height:1234px></div>");
    if (web_doc_height(d) != 1234) fail("doc-height", "height %d, expected %d", web_doc_height(d), 1234);
    web_free(d);
}

/* ---------------------------------------------------------------- painting */
static uint32_t px[VW * 100];

static uint32_t at(int x, int y) { return px[y * VW + x] & 0xFFFFFF; }

static void color_is(const char *name, int x, int y, uint32_t want, int tol) {
    total++;
    uint32_t c = at(x, y);
    int dr = (int)((c >> 16) & 255) - (int)((want >> 16) & 255), dg = (int)((c >> 8) & 255) - (int)((want >> 8) & 255),
        db = (int)(c & 255) - (int)(want & 255);
    if (abs(dr) > tol || abs(dg) > tol || abs(db) > tol) fail(name, "pixel %06x, expected %06x", (int)c, (int)want);
}

static void paint(web_doc *d, int scroll) {
    canvas_t c;
    gfx_init(&c, px, VW, 100, VW);
    web_paint(d, &c, 0, 0, VW, 100, 0, scroll);
}

static void test_paint(void) {
    web_doc *d = load("<body style='margin:0;background:#00ff00'>"
                      "<div style='width:10px;height:10px;background:red'></div>"
                      "<div style='width:100px;height:10px;margin:0 auto;background:#0000ff'></div>"
                      "<div style='width:20px;height:20px;border:4px solid #ffff00;background:white'></div>"
                      "<div style='width:10px;height:10px;background:rgba(255,0,0,.5)'></div>"
                      "<div style='width:40px;height:10px;background:linear-gradient(to right,#000,#fff)'></div>"
                      "<div style='width:100px;height:10px;background:linear-gradient(90deg,red 0 50%,blue 50%)'></div>"
                      "<div style='width:100px;height:10px;background:linear-gradient(to right,#000,#fff,#000)'></div>"
                      "<div style='width:20px;height:10px;background:#000 radial-gradient(circle closest-side,#fff,transparent)'></div>");
    paint(d, 0);
    color_is("paint-bg", 5, 5, 0xff0000, 0);
    color_is("paint-canvas", 50, 5, 0x00ff00, 0);
    color_is("paint-auto-margin-in", 400, 15, 0x0000ff, 0);
    color_is("paint-auto-margin-out", 345, 15, 0x00ff00, 0);
    color_is("paint-border", 1, 21, 0xffff00, 0);
    color_is("paint-border-inside", 10, 30, 0xffffff, 0);
    color_is("paint-alpha", 5, 52, 0x7f7f00, 3); /* half red over green */
    color_is("gradient-to-right", 20, 62, 0x828282, 4);
    color_is("gradient-hard-stop-1", 45, 72, 0xff0000, 0);
    color_is("gradient-hard-stop-2", 55, 72, 0x0000ff, 0);
    color_is("gradient-three-stops", 50, 82, 0xfafafa, 8);
    color_is("gradient-three-stops-end", 99, 82, 0x000000, 8);
    color_is("gradient-radial-centre", 10, 93, 0xdcdcdc, 8); /* 0.7 px from the centre, fading to transparent over black */
    color_is("gradient-radial-edge", 1, 89, 0x000000, 0);
    paint(d, 10); /* scrolled by 10: the blue bar is at the top */
    color_is("paint-scrolled", 400, 5, 0x0000ff, 0);
    web_free(d);
}

/* Narrow regression coverage for the paint-order defect found on the live
   DeepMind page; these pixel checks are not website acceptance tests. */
static void test_item_z_order(void) {
    const char *layouts[] = {"grid", "flex"};
    for (int i = 0; i < 2; i++) {
        char html[1024];
        snprintf(html, sizeof html,
                 "<body style=margin:0><div style='position:relative;display:%s;width:100px;height:50px'>"
                 "<a href=/foreground style='display:block;width:100px;height:50px;background:red;z-index:1'></a>"
                 "<div style='position:absolute;inset:0;background:blue;z-index:0'></div></div>", layouts[i]);
        web_doc *d = load(html);
        paint(d, 0);
        color_is(i ? "flex-item-z-index" : "grid-item-z-index", 10, 10, 0xff0000, 0);
        struct web_hit h;
        total++;
        if (!web_node_action(d, web_node_at(d, 10, 10), &h) || h.kind != WEB_HIT_LINK ||
            !h.href || strcmp(h.href, "http://h.test/foreground"))
            fail("item-z-index-hit", "foreground link missing (kind %d)", h.kind, 0);
        web_free(d);
    }
    /* A positioned child belongs to the grid item's context, not the page. */
    web_doc *d = load("<body style=margin:0><div style='position:relative;display:grid;width:100px;height:50px'>"
        "<div style='height:50px;background:red;z-index:1'>"
        "<a href=/nested style='position:relative;display:block;height:50px;background:lime'></a></div>"
        "<div style='position:absolute;inset:0;background:blue;z-index:0'></div></div>");
    paint(d, 0);
    color_is("grid-context-auto-child", 10, 10, 0x00ff00, 0);
    struct web_hit hit;
    total++;
    if (!web_node_action(d, web_node_at(d, 10, 10), &hit) || !hit.href || strcmp(hit.href, "http://h.test/nested"))
        fail("grid-context-auto-hit", "nested link missing", 0, 0);
    web_free(d);

    d = load("<body style=margin:0><div style='position:relative;z-index:1;width:100px;height:50px'>"
        "<div style='position:absolute;inset:0;background:red;z-index:99'></div></div>"
        "<div style='position:absolute;top:0;left:0;width:100px;height:50px;background:blue;z-index:2'></div>");
    paint(d, 0);
    color_is("context-descendant-cannot-escape", 10, 10, 0x0000ff, 0);
    web_free(d);

    d = load("<body style=margin:0><div style='position:relative;z-index:1;width:100px;height:50px;background:lime'>"
        "<div style='position:absolute;inset:0;background:red;z-index:-1'></div></div>");
    paint(d, 0);
    color_is("negative-after-context-background", 10, 10, 0xff0000, 0);
    web_free(d);

    d = load("<body style=margin:0><div style='position:relative;width:100px;height:50px'>"
        "<div style='position:absolute;inset:0;background:red;z-index:3'></div></div>"
        "<div style='position:absolute;top:0;left:0;width:100px;height:50px;background:blue;z-index:2'></div>");
    paint(d, 0);
    color_is("auto-does-not-isolate-z-index", 10, 10, 0xff0000, 0);
    web_free(d);

    /* A paint context is not necessarily the child's containing block. */
    const char *clip_cases[] = {
        "<div style='position:relative;z-index:1;overflow:hidden;width:20px;height:20px'>"
        "<a href=/escape style='position:fixed;left:40px;top:0;width:20px;height:20px;background:red'></a></div>",
        "<div style='position:relative;display:grid;width:100px;height:50px'>"
        "<div style='z-index:1;overflow:hidden;width:20px;height:20px'>"
        "<a href=/escape style='position:absolute;left:40px;top:0;width:20px;height:20px;background:red'></a></div></div>",
        "<div style='position:relative;z-index:1;overflow:hidden;top:1000px;width:20px;height:20px'>"
        "<div style='position:relative;z-index:2;overflow:hidden;width:10px;height:10px'>"
        "<a href=/escape style='position:fixed;left:40px;top:0;width:20px;height:20px;background:red'></a></div></div>"
    };
    for (int i = 0; i < 3; i++) {
        char html[1024];
        snprintf(html, sizeof html, "<body style=margin:0>%s", clip_cases[i]);
        d = load(html);
        paint(d, 0);
        color_is("context-overflow-escape", 45, 5, 0xff0000, 0);
        total++;
        if (!web_node_action(d, web_node_at(d, 45, 5), &hit) || !hit.href || strcmp(hit.href, "http://h.test/escape"))
            fail("context-overflow-escape-hit", "case %d lost positioned link", i, 0);
        web_free(d);
    }
    /* Conversely, overflow on the actual containing block must still apply. */
    d = load("<body style=margin:0><div style='position:relative;z-index:1;overflow:hidden;width:20px;height:20px'>"
        "<a href=/clipped style='position:absolute;left:40px;top:0;width:20px;height:20px;background:red'></a></div>");
    paint(d, 0);
    color_is("context-overflow-containing-block", 45, 5, 0xffffff, 0);
    total++;
    if (web_node_action(d, web_node_at(d, 45, 5), &hit))
        fail("context-overflow-containing-block-hit", "clipped link is actionable", 0, 0);
    web_free(d);
}

/* ---------------------------------------------------------------- links and forms */
static void test_abs_flex_position(void) {
    static const struct {
        const char *name, *container, *child;
        int x, y;
        bool shadow;
    } cases[] = {
        {"abs-flex-start", "", "", 100, 0, false},
        {"abs-flex-center", "justify-content:center;align-items:center", "", 140, 30, false},
        {"abs-flex-self-end", "justify-content:flex-end", "align-self:flex-end", 180, 60, false},
        {"abs-flex-row-reverse", "flex-direction:row-reverse;align-items:center", "", 180, 30, false},
        {"abs-flex-reverse-end", "flex-direction:row-reverse;justify-content:flex-end", "", 100, 0, false},
        {"abs-flex-column", "flex-direction:column;justify-content:center;align-items:flex-end", "", 180, 30, false},
        {"abs-flex-column-reverse", "flex-direction:column-reverse", "", 100, 60, false},
        {"abs-flex-wrap-reverse", "flex-wrap:wrap-reverse", "", 100, 60, false},
        {"abs-inline-flex-cb-chain", "display:inline-flex;justify-content:center;align-items:center", "", 140, 30, false},
        {"abs-shadow-contents-flex", "justify-content:center;align-items:center", "", 140, 30, true},
        {"abs-explicit-insets-preserved", "justify-content:center;align-items:center", "left:7px;top:9px", 7, 9, false},
        {"abs-one-static-axis", "justify-content:center;align-items:center", "left:7px", 7, 30, false},
        {"abs-static-margin-box", "justify-content:center;align-items:center", "margin:4px 15px 6px 5px", 135, 29, false},
        {"abs-both-auto-margins", "", "left:10px;right:30px;top:0;margin-left:auto;margin-right:auto", 130, 0, false},
        {"abs-single-left-auto", "", "left:10px;right:30px;top:0;margin-left:auto;margin-right:5px", 245, 0, false},
        {"abs-single-right-auto", "", "left:10px;right:30px;top:0;margin-left:5px;margin-right:auto", 15, 0, false},
        {"abs-vertical-auto-margins", "", "left:0;top:5px;bottom:15px;margin-top:auto;margin-bottom:auto", 0, 35, false},
        {"abs-single-top-auto", "", "left:0;top:5px;bottom:15px;margin-top:auto;margin-bottom:10px", 0, 55, false},
        {"abs-negative-horizontal-auto", "", "left:10px;right:290px;top:0;margin-left:auto;margin-right:auto", 10, 0, false},
        {"abs-negative-vertical-auto", "", "left:0;top:10px;bottom:90px;margin-top:auto;margin-bottom:auto", 0, 0, false}
    };
    for (size_t i=0;i<sizeof cases/sizeof cases[0];i++) {
        char html[1536];
        const char *tag=cases[i].shadow?"x-flex":"div";
        snprintf(html,sizeof html,"<!doctype html><body style=margin:0><div style='position:relative;width:300px;height:100px;font-size:0;line-height:0'>"
            "<%s style='display:flex;width:100px;height:80px;margin-left:100px;%s'>%s"
            "<div style=display:contents><a href=/placed style='position:absolute;width:20px;height:20px;background:blue;%s'></a></div>"
            "%s</%s></div>",tag,cases[i].container,cases[i].shadow?"<template shadowrootmode=open>":"",
            cases[i].child,cases[i].shadow?"</template>":"",tag);
        web_doc *d=load(html);
        web_node *target=web_node_at(d,cases[i].x+1,cases[i].y+1);
        int x=0,y=0,w=0,h=0;
        total++;
        if(!target||!web_node_rect(d,target,&x,&y,&w,&h)||x!=cases[i].x||y!=cases[i].y||w!=20||h!=20)
            fail(cases[i].name,"position %d / %d",x,y);
        paint(d,0);color_is(cases[i].name,cases[i].x+1,cases[i].y+1,0x0000ff,0);
        struct web_hit hit;total++;
        if(!web_node_action(d,target,&hit)||hit.kind!=WEB_HIT_LINK||!hit.href||strcmp(hit.href,"http://h.test/placed"))
            fail(cases[i].name,"positioned link hit missing",0,0);
        web_free(d);
    }
}

static void test_filter_groups(void) {
    /* Overlapping children must be flattened once, not faded separately. The
       same backend includes negative overflow and native inline line groups. */
    static const struct {const char *name,*html;int x,y,x2,y2;uint32_t color,color2;bool link;} cases[]={
        {"filter-group-overlap","<div style='width:40px;height:20px;background:red;filter:opacity(.5)'>"
            "<a href=/effect style='position:absolute;left:10px;top:0;width:20px;height:20px;background:blue'></a></div>",
            15,5,5,5,0x7f7fff,0xff7f7f,true},
        {"filter-negative-overflow","<div style=height:10px></div><div style='margin-left:30px;width:10px;height:10px;background:red;filter:opacity(.5)'>"
            "<a href=/effect style='position:absolute;left:-20px;top:20px;width:10px;height:10px;background:blue'></a></div>",
            11,31,31,11,0x7f7fff,0xff7f7f,true},
        {"filter-nested-plus-opacity","<div style='filter:opacity(.5)'><div style='filter:opacity(50%);opacity:.5'>"
            "<a href=/effect style='display:block;width:20px;height:20px;background:blue'></a></div></div>",
            5,5,30,5,0xdfdfff,0xffffff,true},
        {"filter-multiple-functions","<a href=/effect style='display:block;width:20px;height:20px;background:blue;filter:opacity(.5) opacity(50%)'></a>",
            5,5,30,5,0xbfbfff,0xffffff,true},
        {"filter-above-range-used-clamp","<a href=/effect style='display:block;width:20px;height:20px;background:blue;filter:opacity(200%) opacity(2)'></a>",
            5,5,30,5,0x0000ff,0xffffff,true},
        {"filter-none-with-property-opacity","<a href=/effect style='display:block;width:20px;height:20px;background:blue;filter:none;opacity:.5'></a>",
            5,5,30,5,0x7f7fff,0xffffff,true},
        {"filter-zero-still-hit-tests","<a href=/effect style='display:block;width:20px;height:20px;background:blue;filter:opacity(0)'></a>",
            5,5,30,5,0xffffff,0xffffff,true},
        {"filter-inline-overlapping-subtree","<span style='filter:opacity(.5)'>"
            "<a href=/effect style='display:inline-block;vertical-align:top;width:20px;height:20px;background:blue'></a>"
            "<a href=/effect style='position:absolute;left:0;top:0;width:20px;height:20px;background:blue'></a></span>",
            5,5,30,5,0x7f7fff,0xffffff,true},
        {"filter-inline-nested-groups","<span style='filter:opacity(.5)'><span style='filter:opacity(.5)'>"
            "<a href=/effect style='display:inline-block;vertical-align:top;width:20px;height:20px;background:blue'></a>"
            "</span></span>",5,5,30,5,0xbfbfff,0xffffff,true},
        {"filter-inline-flex-composites-once","<span style='display:inline-flex;vertical-align:top;width:20px;height:20px;filter:opacity(.5)'>"
            "<a href=/effect style='width:20px;height:20px;background:blue'></a></span>",5,5,30,5,0x7f7fff,0xffffff,true},
        {"filter-inline-grid-composites-once","<span style='display:inline-grid;vertical-align:top;width:20px;height:20px;filter:opacity(.5)'>"
            "<a href=/effect style='width:20px;height:20px;background:blue'></a></span>",5,5,30,5,0x7f7fff,0xffffff,true},
        {"filter-inline-table-composites-once","<table style='display:inline-table;vertical-align:top;border-spacing:0;filter:opacity(.5)'>"
            "<tr><td style='padding:0'><a href=/effect style='display:block;width:20px;height:20px;background:blue'></a></td></tr></table>",
            5,5,30,5,0x7f7fff,0xffffff,true},
        {"filter-boxless-contents","<span style='display:contents;filter:opacity(.5)'>"
            "<a href=/effect style='display:block;width:20px;height:20px;background:blue'></a></span>",5,5,30,5,0x0000ff,0xffffff,true},
        {"filter-atomic-svg-root","<svg width=20 height=20 style='filter:opacity(.5);vertical-align:top'><rect width=20 height=20 fill=blue /></svg>",
            5,5,30,5,0x7f7fff,0xffffff,false},
        {"filter-root-canvas-group","<style>html{filter:opacity(.5)}body{background:blue}</style>",
            5,5,100,90,0x7f7fff,0x7f7fff,false},
        {"filter-document-root-not-fixed-cb","<style>html{filter:opacity(1);margin-left:30px;padding-left:10px}</style>"
            "<a href=/effect style='position:fixed;left:5px;top:5px;width:20px;height:20px;background:blue'></a>",
            6,6,30,5,0x0000ff,0xffffff,true},
        {"filter-clipped-descendant","<div style='width:10px;height:10px;overflow:hidden;filter:opacity(.5)'>"
            "<a href=/effect style='position:fixed;left:15px;top:0;width:10px;height:10px;background:blue'></a></div>",
            16,5,5,5,0xffffff,0xffffff,false}
    };
    for(size_t i=0;i<sizeof cases/sizeof *cases;i++) {
        char html[1536];snprintf(html,sizeof html,"<!doctype html><body style='margin:0;font-size:0;line-height:0'>%s",cases[i].html);
        web_doc *d=load(html);paint(d,0);
        color_is(cases[i].name,cases[i].x,cases[i].y,cases[i].color,1);
        color_is(cases[i].name,cases[i].x2,cases[i].y2,cases[i].color2,1);
        struct web_hit hit={0};bool link=web_hit_test(d,cases[i].x,cases[i].y,&hit)&&hit.kind==WEB_HIT_LINK&&
            hit.href&&!strcmp(hit.href,"http://h.test/effect");total++;
        if(link!=cases[i].link)fail(cases[i].name,"link hit %d expected %d",link,cases[i].link);
        web_free(d);
    }
    y_is("filter-inline-preserves-wrapping","<!doctype html><body style='margin:0;font-size:0;line-height:0'><div style=width:30px>"
        "<span style='filter:opacity(.5)'><span style='display:inline-block;width:20px;height:20px;vertical-align:top'></span>"
        "<span style='display:inline-block;width:20px;height:20px;vertical-align:top'></span></span></div><div id=t></div>",40);
    static const struct {const char *name,*filter,*position;bool shadow;int x,y,visual_y;} cb_cases[]={
        {"filter-static-absolute-cb","opacity(1)","absolute",false,40,30,20},
        {"filter-static-fixed-cb","opacity(1)","fixed",false,40,30,20},
        {"filter-multiple-fixed-cb","opacity(50%) opacity(200%)","fixed",false,40,30,20},
        {"filter-none-absolute-viewport","none","absolute",false,5,5,-5},
        {"filter-none-fixed-viewport","none","fixed",false,5,5,5},
        {"filter-shadow-fixed-cb","opacity(1)","fixed",true,40,30,20}
    };
    for(size_t i=0;i<sizeof cb_cases/sizeof *cb_cases;i++) {
        char html[1536];const char *tag=cb_cases[i].shadow?"x-filter":"div";
        snprintf(html,sizeof html,"<!doctype html><body style='margin:0;font-size:0;line-height:0'><div style=height:20px></div>"
            "<%s style='display:block;margin-left:30px;padding:10px;border:5px solid transparent;width:100px;height:60px;filter:%s'>%s"
            "<a href=/effect style='position:%s;left:5px;top:5px;width:10px;height:20px;background:blue'></a>%s</%s>",
            tag,cb_cases[i].filter,cb_cases[i].shadow?"<template shadowrootmode=open>":"",cb_cases[i].position,
            cb_cases[i].shadow?"</template>":"",tag);
        web_doc *d=load(html);web_node *n=web_node_at(d,cb_cases[i].x+1,cb_cases[i].y+6);
        int x=0,y=0,w=0,h=0;total++;
        if(!n||!web_node_rect(d,n,&x,&y,&w,&h)||x!=cb_cases[i].x||y!=cb_cases[i].y||w!=10||h!=20)
            fail(cb_cases[i].name,"CB position %d / %d",x,y);
        uint32_t color=!strcmp(cb_cases[i].filter,"opacity(50%) opacity(200%)")?0x7f7fff:0x0000ff;
        paint(d,0);color_is(cb_cases[i].name,cb_cases[i].x+1,cb_cases[i].y+6,color,1);
        paint(d,10);color_is(cb_cases[i].name,cb_cases[i].x+1,cb_cases[i].visual_y+6,color,1);
        struct web_hit hit={0};total++;
        if(!web_hit_test(d,cb_cases[i].x+1,cb_cases[i].visual_y+16,&hit)||hit.kind!=WEB_HIT_LINK)
            fail(cb_cases[i].name,"scrolling effect link missing",0,0);
        web_free(d);
    }
    const char *wrapped[]={"inline-flex","inline-grid","inline-table"};
    const char *positions[]={"absolute","fixed"};
    for(size_t k=0;k<sizeof wrapped/sizeof *wrapped;k++)for(size_t p=0;p<sizeof positions/sizeof *positions;p++) {
        char html[2048];snprintf(html,sizeof html,"<!doctype html><body style='margin:0;font-size:0;line-height:0'><div style=height:20px></div>"
            "<div style='display:%s;vertical-align:top;margin-left:30px;padding:4px;border:3px solid transparent;"
            "width:80px;height:40px;filter:opacity(1);overflow:hidden'>"
            "<a href=/effect style='position:%s;left:25%%;top:5px;width:50%%;height:10px;background:blue'></a>"
            "<a href=/outside style='position:%s;left:-10px;top:5px;width:10px;height:10px;background:red'></a></div>",
            wrapped[k],positions[p],positions[p]);
        web_doc *d=load(html);web_node *n=web_node_at(d,56,29);int x=0,y=0,w=0,h=0;total++;
        if(!n||!web_node_rect(d,n,&x,&y,&w,&h)||x!=55||y!=28||w!=44||h!=10)
            fail("wrapped-filter-padding-percent-CB","CB position %d / %d",x,y);
        paint(d,0);color_is("wrapped-filter-padding-CB-paint",56,29,0x0000ff,0);
        color_is("wrapped-filter-fixed-overflow-clip",31,29,0xffffff,0);
        struct web_hit hit={0};total++;
        if(web_hit_test(d,31,29,&hit)&&hit.kind==WEB_HIT_LINK)
            fail("wrapped-filter-overflow-clip-hit","outside child unexpectedly hit",0,0);
        web_free(d);
    }
}

static web_node *node_at(web_doc *d, int x, int y, int kind, const char *name) {
    struct web_hit h;
    total++;
    if (!web_hit_test(d, x, y, &h) || h.kind != kind) {
        fail(name, "hit kind %d, expected %d", h.kind, kind);
        return NULL;
    }
    return h.node;
}

static void str_is(const char *name, const char *got, const char *want) {
    total++;
    if (!got || strcmp(got, want)) {
        printf("FAIL %s: got '%s', expected '%s'\n", name, got ? got : "(null)", want);
        failed++;
    }
}

static void test_forms(void) {
    web_doc *d = load_controls("<body style=margin:0><a href='/x?y=1' style=display:block;height:20px>link</a>"
                      "<a href='sub/p#frag' style=display:block;height:20px>rel</a>");
    struct web_hit h;
    web_hit_test(d, 5, 5, &h);
    str_is("link-absolute", h.href, "http://h.test/x?y=1");
    web_hit_test(d, 5, 25, &h);
    str_is("link-relative", h.href, "http://h.test/dir/sub/p#frag");
    web_free(d);

    d = load_controls("<style>input,select{display:block;height:20px;margin:0;box-sizing:border-box}</style>"
             "<body style=margin:0><form action=/s><input name=q value=hi>"
             "<input type=hidden name=h value='a b'><input type=checkbox name=c checked>"
             "<select name=s><option>one<option value=2>two</select>"
             "<input type=submit name=go value=Go></form>");
    web_node *q = node_at(d, 5, 5, WEB_HIT_TEXT_INPUT, "form-text-hit");
    web_node *cb = node_at(d, 5, 25, WEB_HIT_CHECKBOX, "form-checkbox-hit");
    web_node *sel = node_at(d, 5, 45, WEB_HIT_SELECT, "form-select-hit");
    web_node *go = node_at(d, 5, 65, WEB_HIT_SUBMIT, "form-submit-hit");
    if (q && cb && sel && go) {
        char *url, *body;
        web_submit(d, go, &url, &body);
        str_is("form-get", url, "http://h.test/s?q=hi&h=a+b&c=on&s=one&go=Go");
        total++;
        if (body) fail("form-get-body", "a GET has a body", 0, 0);
        free(url);
        free(body);
        web_focus(d, q);
        struct gui_event e = {.type = EV_KEY, .key = 'x', .pressed = 1};
        total++;
        if (web_key(d, &e) != 1) fail("form-typing", "web_key did not take a character", 0, 0);
        e.key = NKEY_BACKSPACE;
        web_key(d, &e);
        web_key(d, &e);
        e.key = '!';
        web_key(d, &e);
        web_toggle(d, cb);
        const char *labels[4];
        int cur = -1;
        total++;
        if (web_select_options(d, sel, labels, 4, &cur) != 2 || cur != 0) fail("form-select-options", "options, selected %d", cur, 0);
        web_select_set(d, sel, 1);
        e.key = NKEY_ENTER;
        total++;
        if (web_key(d, &e) != 2) fail("form-enter", "Enter in a text field does not submit", 0, 0);
        web_submit(d, q, &url, &body);
        str_is("form-edited", url, "http://h.test/s?q=h%21&h=a+b&s=2");
        free(url);
        free(body);
    }
    web_free(d);

    d = load_controls("<body style=margin:0><form method=post action=p><input name=a value='1&2=3 4' style=display:block;height:20px></form>");
    web_node *a = node_at(d, 5, 5, WEB_HIT_TEXT_INPUT, "form-post-hit");
    if (a) {
        char *url, *body;
        web_submit(d, a, &url, &body);
        str_is("form-post-url", url, "http://h.test/dir/p");
        str_is("form-post-body", body, "a=1%262%3D3+4");
        free(url);
        free(body);
    }
    web_free(d);
}

/* ---------------------------------------------------------------- text, URLs, resources */
static void title_is(const char *name, const char *html, const char *charset, const char *want) {
    web_doc *d = web_parse(html, strlen(html), BASE, charset);
    str_is(name, web_title(d), want);
    web_free(d);
}

static void url_is(const char *name, const char *base, const char *rel, const char *want) {
    char out[512];
    str_is(name, web_resolve_url(base, rel, out, sizeof out) ? out : NULL, want);
}

static const unsigned char blue_png[] = {
    0x89, 0x50, 0x4e, 0x47, 0x0d, 0x0a, 0x1a, 0x0a, 0x00, 0x00, 0x00, 0x0d, 0x49, 0x48, 0x44, 0x52, 0x00, 0x00,
    0x00, 0x03, 0x00, 0x00, 0x00, 0x05, 0x08, 0x02, 0x00, 0x00, 0x00, 0x0f, 0x13, 0xc1, 0xf5, 0x00, 0x00, 0x00,
    0x0f, 0x49, 0x44, 0x41, 0x54, 0x78, 0xda, 0x63, 0x60, 0x60, 0xf8, 0x0f, 0x43, 0xc4, 0xb1, 0x00, 0x66, 0xd9,
    0x0e, 0xf2, 0x89, 0x1a, 0x34, 0xe3, 0x00, 0x00, 0x00, 0x00, 0x49, 0x45, 0x4e, 0x44, 0xae, 0x42, 0x60, 0x82};

static void test_replaced_flow(void) {
    /* Real decoded natural dimensions (3x5), never a synthetic image box.
       Contents and anonymous wrappers must not lose percentage references;
       min constraints transfer the natural ratio only into automatic axes. */
    static const struct {
        const char *name, *parent, *before, *image;
        int width, height;
    } cases[] = {
        {"contents-image-auto", "width:60px", "", "width:100%", 60, 100},
        {"contents-image-indefinite-percent", "width:60px", "", "width:100%;height:100%", 60, 100},
        {"contents-image-definite-percent", "width:60px;height:80px", "", "width:100%;height:100%", 60, 80},
        {"anonymous-image-definite-percent", "width:60px;height:80px", "<div style=height:0></div>",
            "width:100%;height:100%", 60, 80},
        {"anonymous-image-indefinite-percent", "width:60px", "<div style=height:0></div>",
            "width:100%;height:100%", 60, 100},
        {"image-min-width-ratio", "width:60px", "", "min-width:30px", 30, 50},
        {"image-min-height-ratio", "width:60px", "", "min-height:50px", 30, 50},
        {"image-definite-axis-preserved", "width:60px", "", "width:12px;height:20px;min-width:30px", 30, 20},
        {"image-conflicting-limits", "width:60px", "", "min-width:30px;max-height:10px", 30, 10}
    };
    for (size_t i = 0; i < sizeof cases / sizeof cases[0]; i++) {
        char html[768];
        snprintf(html, sizeof html, "<!doctype html><body style=margin:0><div style='font-size:0;line-height:0;%s'>%s"
            "<picture style=display:contents><img src=i.png style='display:inline-block;vertical-align:top;%s'></picture>"
            "</div><div id=t></div>", cases[i].parent, cases[i].before, cases[i].image);
        web_doc *d = load(html);
        total++;
        if (web_image_count(d) != 1) {
            fail(cases[i].name, "image count %d, expected %d", web_image_count(d), 1);
            web_free(d); continue;
        }
        web_image_loaded(d, 0, blue_png, sizeof blue_png, true);
        web_layout(d, VW, VH);
        web_node *image = web_node_at(d, 1, 1);
        int x = 0, y = 0, w = 0, h = 0;
        total++;
        if (!image || !web_node_rect(d, image, &x, &y, &w, &h) ||
            x != 0 || y != 0 || w != cases[i].width || h != cases[i].height)
            fail(cases[i].name, "image size %d x %d", w, h);
        total++;
        if (web_anchor_y(d, "t") != cases[i].height)
            fail(cases[i].name, "flow height %d, expected %d", web_anchor_y(d, "t"), cases[i].height);
        paint(d, 0);
        color_is(cases[i].name, 1, 1, 0x0000ff, 0);
        web_free(d);
    }
    /* Explicit zero is intentional; never manufacture a natural minimum. */
    web_doc *d = load("<!doctype html><body style=margin:0><div style='font-size:0;line-height:0'>"
        "<picture style=display:contents><img src=i.png style='display:inline-block;width:60px;height:0'></picture>"
        "</div><div id=t></div>");
    web_image_loaded(d, 0, blue_png, sizeof blue_png, true);
    web_layout(d, VW, VH);
    total++;
    if (web_anchor_y(d, "t") != 0) fail("image-explicit-zero", "flow height %d, expected %d", web_anchor_y(d, "t"), 0);
    web_free(d);
}

static void test_float_percent_heights(void) {
    static const struct {
        const char *name, *parent, *content;
        int image_count, y, height, flow_height, probe_y;
    } cases[] = {
        {"float-block-definite-percent", "height:80px",
            "<div style='float:left;width:60px;height:100%'><picture style=display:contents>"
            "<img src=i.png style='display:inline-block;width:100%;height:100%;vertical-align:top'></picture></div>",
            1, 0, 80, 80, 1},
        {"float-replaced-definite-percent", "height:80px",
            "<img src=i.png style='float:left;width:60px;height:100%'>", 1, 0, 80, 80, 1},
        {"inline-float-definite-percent", "height:80px",
            "<span><img src=i.png style='float:left;width:60px;height:100%'></span>", 1, 0, 80, 80, 1},
        /* CSS 2.1 §9.5: a float that fits beside the line's content goes on
           that line's top; one that does not is deferred below the line. */
        {"deferred-float-definite-percent", "height:80px",
            "<span style='display:inline-block;width:50px;height:5px;vertical-align:top'></span>"
            "<img src=i.png style='float:left;width:60px;height:100%'>", 1, 5, 80, 85, 6},
        {"float-beside-line-definite-percent", "height:80px",
            "<span style='display:inline-block;width:1px;height:5px;vertical-align:top'></span>"
            "<img src=i.png style='float:left;width:60px;height:100%'>", 1, 0, 80, 80, 1},
        {"split-inline-float-percent", "height:80px",
            "<div style='float:left;width:60px;height:100%'>"
            "<span style='height:200px;min-height:300px;max-height:400px'><div style='width:100%;height:100%'>"
            "<picture style=display:contents><img src=i.png style='display:inline-block;width:100%;height:100%;vertical-align:top'>"
            "</picture></div></span></div>", 1, 0, 80, 80, 1},
        {"float-table-definite-percent", "height:80px",
            "<table style='float:left;width:60px;height:100%;border-spacing:0;background:blue'><tr><td style=padding:0>"
            "<div style='width:60px;height:1px'></div></td></tr></table>", 0, 0, 80, 80, 10},
        {"float-replaced-indefinite-percent", "height:auto;overflow:hidden",
            "<img src=i.png style='float:left;width:60px;height:100%'>", 1, 0, 100, 100, 1}
    };
    for (size_t i = 0; i < sizeof cases / sizeof cases[0]; i++) {
        char html[1024];
        snprintf(html, sizeof html, "<!doctype html><body style=margin:0>"
            "<div style='width:100px;font-size:0;line-height:0;%s'>%s</div><div id=t style=clear:both></div>",
            cases[i].parent, cases[i].content);
        web_doc *d = load(html);
        total++;
        if (web_image_count(d) != cases[i].image_count) {
            fail(cases[i].name, "image count %d, expected %d", web_image_count(d), cases[i].image_count);
            web_free(d); continue;
        }
        if (cases[i].image_count) web_image_loaded(d, 0, blue_png, sizeof blue_png, true);
        web_layout(d, VW, VH);
        web_node *target = web_node_at(d, 1, cases[i].probe_y);
        int x = 0, y = 0, w = 0, h = 0;
        total++;
        if (!target || !web_node_rect(d, target, &x, &y, &w, &h) ||
            x != 0 || y != cases[i].y || w != 60 || h != cases[i].height)
            fail(cases[i].name, "target size %d x %d", w, h);
        total++;
        if (web_anchor_y(d, "t") != cases[i].flow_height)
            fail(cases[i].name, "flow height %d, expected %d", web_anchor_y(d, "t"), cases[i].flow_height);
        paint(d, 0);
        color_is(cases[i].name, 1, cases[i].probe_y, 0x0000ff, 0);
        web_free(d);
    }
}

static void test_text(void) {
    title_is("title-entities", "<title>a &amp; b &lt;&#x263A;&hellip;</title>", NULL, "a & b <\xe2\x98\xba\xe2\x80\xa6");
    title_is("title-whitespace", "<title>\n  two   words \n</title>", NULL, "two words");
    title_is("charset-header", "<title>caf\xe9 \x93q\x94</title>", "windows-1252", "caf\xc3\xa9 \xe2\x80\x9cq\xe2\x80\x9d");
    title_is("charset-meta", "<meta charset=iso-8859-1><title>\xe9</title>", NULL, "\xc3\xa9");
    title_is("charset-utf8", "<title>\xe6\x9c\x88</title>", NULL, "\xe6\x9c\x88");

    url_is("url-dotdot", "http://a/b/c?d", "../x#y", "http://a/x#y");
    url_is("url-scheme-relative", "https://a/b", "//c/d", "https://c/d");
    url_is("url-query-only", "http://a/b/c", "?q=1", "http://a/b/c?q=1");
    url_is("url-absolute", "http://a/b", "HTTPS://Example.COM", "https://example.com/");
    url_is("url-port", "http://a:8080/b/", "c", "http://a:8080/b/c");

    web_doc *d = web_parse("<meta http-equiv=refresh content='3; url=/next'>", 48, BASE, NULL);
    int delay = -1;
    str_is("meta-refresh", web_refresh_url(d, &delay), "http://h.test/next");
    total++;
    if (delay != 3) fail("meta-refresh-delay", "delay %d, expected %d", delay, 3);
    web_free(d);

    /* an external stylesheet, then an image */
    const char *html = "<link rel=stylesheet href=../s.css><link rel=stylesheet media=print href=p.css>"
                       "<body style=margin:0><div id=a></div><img src=i.png style=display:block><div id=t></div>";
    d = web_parse(html, strlen(html), BASE, NULL);
    str_is("stylesheet-url", web_pending_stylesheet(d), "http://h.test/s.css");
    web_stylesheet_loaded(d, "#a { height: 70px }", 19);
    total++;
    if (web_pending_stylesheet(d)) fail("stylesheet-print-skipped", "a print stylesheet was requested", 0, 0);
    web_layout(d, VW, VH);
    total++;
    if (web_image_count(d) != 1 || !web_image_wanted(d, 0)) fail("image-wanted", "%d images", web_image_count(d), 0);
    else {
        str_is("image-url", web_image_url(d, 0), "http://h.test/dir/i.png");
        web_image_loaded(d, 0, blue_png, sizeof blue_png, true);
        web_layout(d, VW, VH);
        total++;
        if (web_anchor_y(d, "t") != 75) fail("image-size", "#t at %d, expected %d", web_anchor_y(d, "t"), 75);
        paint(d, 0);
        color_is("image-pixels", 1, 72, 0x0000ff, 0);
    }
    web_free(d);

    d = load("<body style=margin:0><div style=height:300px></div><p style=margin:0>a needle here</p>");
    int y = web_find(d, "NEEDLE", 0);
    total++;
    if (y < 295 || y > 305) fail("find", "found at %d, expected about %d", y, 300);
    total++;
    if (web_find(d, "haystack", 0) >= 0) fail("find-missing", "found something", 0, 0);
    web_free(d);
}

/* ---------------------------------------------------------------- hostile input */
static void survives(const char *name, const char *html) {
    printf("WEB-STAGE %s\n",name);fflush(stdout);
    total++;
    web_doc *d = load(html);
    int h = web_doc_height(d);
    paint(d, 0);
    if (h < 0) fail(name, "height %d", h, 0);
    web_free(d);
}

static char *repeat(const char *a, const char *b, int n) {
    size_t la = strlen(a), lb = strlen(b);
    char *s = malloc((la + lb) * (size_t)n + 1), *p = s;
    for (int i = 0; i < n; i++) memcpy(p, a, la), p += la;
    for (int i = 0; i < n; i++) memcpy(p, b, lb), p += lb;
    *p = 0;
    return s;
}

static void test_hostile(void) {
    survives("tag-soup", "<table><b><tr><i><td>a<p>b</table></b></i><li><dd>c</ul></ol><select><div>d</select><frameset>");
    survives("garbage-css", "<style>}}{{ div{color:red;;;width:calc(1px+);} @media ((( { a{} } @x;</style><div style='width:-5px;"
                            "height:1e30px;margin:nanpx;grid-template-columns:repeat(99999,1fr);flex:-1'>x</div>");
    survives("huge-colspan", "<table><tr><td colspan=100000 rowspan=100000>a<td>b<tr><td>c</table>");
    survives("unterminated", "<div class='a><!-- <script>var x = '</div>");
    char *s = repeat("<div>", "</div>", 5000);
    survives("deep-divs", s);
    free(s);
    s = repeat("<table><tr><td>", "</table>", 300);
    survives("deep-tables", s);
    free(s);
    s = repeat("<span style=display:flex>", "</span>", 2000);
    survives("deep-flex", s);
    free(s);

    /* a long page: layout time is logged */
    s = repeat("<p>Lorem ipsum dolor sit amet, <b>consectetur</b> adipiscing elit, <a href=x>sed do</a> eiusmod.</p>", "", 3000);
    printf("WEB-STAGE 3000-paragraphs\n");fflush(stdout);
    uint64_t t0 = uptime_ms();
    web_doc *d = load(s);
    printf("webtest: 3000 paragraphs laid out in %d ms, height %d\n", (int)(uptime_ms() - t0), web_doc_height(d));
    web_free(d);
    free(s);
}

static int test_flex_zero_basis(void) {
    const char *html="<!doctype html><html><head><style>body{margin:0}"
        ".row{display:flex;flex-wrap:wrap;width:1280px}"
        ".main{display:flex;flex-direction:column;flex:1 1 0;width:100%;min-width:1px;height:40px}"
        ".pane{display:flex;width:328px;height:60px;flex-shrink:0}"
        "</style></head><body><div class=row><div class=main></div><div class=pane id=t></div></div></body></html>";
    web_doc *d=web_live(html,strlen(html),BASE,"utf-8",NULL);
    if(!d){fail("flex-zero-basis","document failed %d %d",0,0);return 1;}
    uint64_t start=uptime_ms();
    while(d->parser && uptime_ms()-start<5000)web_tick(d,uptime_ms());
    total++;
    if(d->parser || !d->body)fail("flex-zero-basis-parser","unfinished %d expected %d",1,0);
    web_layout(d,VW,VH);total++;
    int y=web_anchor_y(d,"t");
    if(y!=0)fail("flex-zero-basis-row","pane y=%d expected %d",y,0);
    web_free(d);
    printf("webtest-flex-zero: %d checks, %d failed\n",total,failed);return failed!=0;
}
static int test_flex_freeze(void) {
    static const struct { const char *row,*a,*b;int aw,bw,bx; } cases[]={
        {"width:1000px","flex:0 1 400px;min-width:400px","flex:0 1 1000px;min-width:0",400,600,400},
        {"width:300px","flex:1 1 0;max-width:50px","flex:1 1 0",50,250,50},
        {"width:300px","flex:.25 1 0","flex:.25 1 0",75,75,75},
        {"width:300px","flex:0 .25 200px","flex:0 .25 200px",175,175,175},
        {"width:300px;gap:10px","flex:0 1 200px;min-width:180px;margin-right:10px","flex:0 1 200px;min-width:0",180,100,200},
        {"width:300px;flex-direction:row-reverse","flex:1 1 0;max-width:50px","flex:1 1 0",50,250,0},
        {"width:300px","flex:0 0 200px","flex:0 1 200px;min-width:0",200,100,200}
    };
    for(size_t i=0;i<sizeof cases/sizeof cases[0];i++) {
        char html[1024];snprintf(html,sizeof html,"<!doctype html><style>body{margin:0}.row{display:flex;%s}"
            "#a{height:20px;%s}#b{height:20px;%s}</style><div class=row><div id=a></div><div id=b></div></div>",
            cases[i].row,cases[i].a,cases[i].b);
        web_doc *d=load(html);
        if(!d){total++;fail("flex-freeze-document","case %d failed %d",(int)i,1);continue;}
        const char *ids[]={"#a","#b"};int rects[2][4]={{0}};
        for(int k=0;k<2;k++) {
            pvec v={0};css_select(d,d->root,ids[k],&v);total++;
            if(!v.n||!web_node_rect(d,v.v[0],&rects[k][0],&rects[k][1],&rects[k][2],&rects[k][3]))
                fail("flex-freeze-geometry","case %d item %d",(int)i,k);
            pv_free(&v);
        }
        total++;if(rects[0][2]!=cases[i].aw)fail("flex-freeze-first-size","width %d expected %d",rects[0][2],cases[i].aw);
        total++;if(rects[1][2]!=cases[i].bw)fail("flex-freeze-second-size","width %d expected %d",rects[1][2],cases[i].bw);
        total++;if(rects[1][0]!=cases[i].bx)fail("flex-freeze-second-position","x %d expected %d",rects[1][0],cases[i].bx);
        web_free(d);
    }
    printf("webtest-flex-freeze: %d checks, %d failed\n",total,failed);return failed!=0;
}
static int test_auto_table_percent(void) {
    static const struct { const char *cells;int widths[3],columns; } cases[]={
        {"<td id=a style=width:100%></td><td id=b style=width:100px></td>",{200,100,0},2},
        {"<td id=a style=width:50%></td><td id=b style=width:50%></td><td id=c style=width:100px></td>",{100,100,100},3},
        {"<td id=a style=width:80%></td><td id=b style=width:80%></td>",{240,60,0},2},
        {"<td id=a style=width:100px></td><td id=b></td>",{100,200,0},2},
        {"<td id=a></td><td id=b></td><td id=c></td>",{100,100,100},3},
        {"<td id=a style='width:100%;max-width:0'></td><td id=b style=width:200px></td>",{100,200,0},2}
    };
    for(size_t i=0;i<sizeof cases/sizeof cases[0];i++) {
        char html[1536];snprintf(html,sizeof html,"<!doctype html><style>body{margin:0}table{width:300px;border-spacing:0}td{padding:0;height:10px}</style><table id=t><tr>%s</tr></table>",cases[i].cells);
        web_doc *d=load(html);total++;if(!d){fail("auto-table-percent-document","case %d failed %d",(int)i,1);continue;}
        const char *ids[]={"#a","#b","#c"};int right=0;
        for(int j=0;j<cases[i].columns;j++) {
            pvec v={0};css_select(d,d->root,ids[j],&v);int x=0,y=0,w=0,h=0;total++;
            if(!v.n||!web_node_rect(d,v.v[0],&x,&y,&w,&h))fail("auto-table-percent-cell","case %d column %d",(int)i,j);
            total++;if(w!=cases[i].widths[j])fail("auto-table-percent-size","width %d expected %d",w,cases[i].widths[j]);
            total++;if(x!=right)fail("auto-table-percent-position","x %d expected %d",x,right);
            right=x+w;pv_free(&v);
        }
        total++;if(right!=300)fail("auto-table-percent-sum","right %d expected %d",right,300);
        web_free(d);
    }
    printf("webtest-auto-table-percent: %d checks, %d failed\n",total,failed);return failed!=0;
}
static int test_fixed_table(void) {
    static const struct { const char *columns,*rows;int widths[3],table; } cases[]={
        {"","<tr><td id=a>first</td><td id=b>row</td><td id=c>three</td></tr>"
             "<tr><td style='width:1000px;white-space:nowrap'>averylongunbrokencontentmustnotsetfixedtracks</td><td></td><td></td></tr>",{100,100,100},300},
        {"<colgroup><col style='width:100px'><col><col></colgroup>",
             "<tr><td id=a></td><td id=b></td><td id=c></td></tr>",{100,100,100},300},
        {"","<tr><td id=a style='width:60px'></td><td id=b></td><td id=c></td></tr>",{60,120,120},300},
        {"<colgroup><col span=2 style='width:60px'><col></colgroup>",
             "<tr><td id=a></td><td id=b></td><td id=c></td></tr>",{60,60,180},300},
        {"<colgroup><col style='width:120px'><col style='width:120px'><col style='width:120px'></colgroup>",
             "<tr><td id=a></td><td id=b></td><td id=c></td></tr>",{120,120,120},360},
        {"","<tr><td id=a colspan=2 style='width:120px'></td><td id=c></td></tr>",{120,0,180},300},
    };
    for(size_t i=0;i<sizeof cases/sizeof *cases;i++) {
        sbuf html={0};sb_puts(&html,"<!doctype html><html><head><style>body{margin:0}table{table-layout:fixed;width:300px;border-spacing:0;border:0}td{padding:0;border:0}</style></head><body><table id=t>");
        sb_puts(&html,cases[i].columns);sb_puts(&html,cases[i].rows);sb_puts(&html,"</table></body></html>");
        web_doc *d=web_live(sb_cstr(&html),html.n,BASE,"utf-8",NULL);sb_free(&html);
        if(!d){total++;fail("fixed-table-create","case %d failed %d",(int)i,1);continue;}
        uint64_t start=uptime_ms();while(d->parser&&uptime_ms()-start<5000)web_tick(d,uptime_ms());web_layout(d,VW,VH);
        const char *ids[]={"#a","#b","#c","#t"};
        for(int j=0;j<4;j++) {
            if(j<3&&!cases[i].widths[j])continue;
            pvec nodes={0};css_select(d,d->root,ids[j],&nodes);
            int x=0,y=0,w=-1,h=0;total++;
            bool ok=nodes.n && web_node_rect(d,nodes.v[0],&x,&y,&w,&h);
            int expected=j==3?cases[i].table:cases[i].widths[j];
            if(!ok || w!=expected)fail("fixed-table-track","width %d expected %d",w,expected);
            pv_free(&nodes);
        }
        web_free(d);
    }
    printf("webtest-fixed-table: %d checks, %d failed\n",total,failed);return failed!=0;
}
static web_doc *overflow_document(const char *text,float width,const char *extra) {
    char html[4096];
    snprintf(html,sizeof html,"<!doctype html><html><head><style>body{margin:0}"
        "#t{font:16px/20px monospace;width:%.4fpx;white-space:nowrap;overflow:hidden;text-overflow:ellipsis;%s}"
        "a{color:red;text-decoration:none}</style></head><body><div id=t>%s</div></body></html>",width,extra,text);
    web_doc *d=web_live(html,strlen(html),BASE,"utf-8",NULL);
    if(d){uint64_t start=uptime_ms();while(d->parser&&uptime_ms()-start<5000)web_tick(d,uptime_ms());web_layout(d,VW,VH);}
    return d;
}
static box_t *overflow_box(web_doc *d) {
    pvec v={0};css_select(d,d->root,"#t",&v);box_t *b=v.n?((node_t*)v.v[0])->box:NULL;pv_free(&v);return b;
}
static void overflow_check(bool condition,const char *name) {
    total++;if(!condition)fail(name,"condition %d expected %d",0,1);
}
static int test_text_overflow(void) {
    web_doc *probe=overflow_document("XXXXXXXXXX",200,"");
    overflow_check(probe!=NULL,"ellipsis-document");if(!probe)return 1;
    box_t *p=overflow_box(probe);
    overflow_check(p&&p->nruns==1,"ellipsis-real-inline-layout");if(!p){web_free(probe);return 1;}
    float prefix=style_text_width(p->st,"XXX",3),marker=style_text_width(p->st,"\xe2\x80\xa6",3);
    float width=prefix+marker+.1f;
    web_free(probe);
    web_doc *d=overflow_document("<a id=l href=x>XXXXXXXXXX</a>",width,"");
    web_doc *reference=overflow_document("<a href=x>XXX</a>\xe2\x80\xa6",width,"text-overflow:clip");
    overflow_check(d&&reference,"ellipsis-reference-documents");if(!d||!reference){web_free(d);web_free(reference);return 1;}
    p=overflow_box(d);float original_width=p->runs[0].w,original_scroll=p->scroll_w;
    paint(d,0);uint32_t *actual=malloc(sizeof px);overflow_check(actual!=NULL,"ellipsis-pixel-storage");
    if(actual){memcpy(actual,px,sizeof px);paint(reference,0);overflow_check(!memcmp(actual,px,sizeof px),"ellipsis-real-glyph-and-owner-color-reference");free(actual);}
    overflow_check(p->runs[0].n==10&&p->runs[0].w==original_width&&p->scroll_w==original_scroll,"ellipsis-retains-logical-text-and-scroll-width");
    overflow_check(p->nlines==1&&p->lines[0].ellipsis_active&&p->lines[0].ellipsis_bytes==3,"ellipsis-complete-prefix-metadata");
    struct web_hit hit;web_hit_test(d,(int)(prefix+marker/2),10,&hit);
    overflow_check(hit.href!=NULL,"ellipsis-hit-target-is-original-elided-link");
    node_t *n=p->node;overflow_check(doc_element_scroll(d,n,10000,0),"ellipsis-native-scroll-to-end");paint(d,0);
    overflow_check(!p->lines[0].ellipsis_active&&p->runs[0].n==10,"ellipsis-disappears-at-content-end-without-changing-text");
    web_free(d);web_free(reference);
    d=overflow_document("XX",200,"");paint(d,0);p=overflow_box(d);
    overflow_check(!p->lines[0].ellipsis_active,"ellipsis-no-marker-for-fitting-text");web_free(d);
    d=overflow_document("XXXXXXXXXX",width,"overflow:visible");paint(d,0);p=overflow_box(d);
    overflow_check(!p->lines[0].ellipsis_active,"ellipsis-does-not-enable-clipping");web_free(d);
    d=overflow_document("<span style='display:inline-block;width:30px;height:10px;background:blue'></span>XXXXXXXXXX",10,"");
    paint(d,0);p=overflow_box(d);
    overflow_check(p->nruns&&p->runs[0].atomic&&!p->runs[0].atomic->ellipsis_hidden,"ellipsis-first-atomic-is-clipped-not-hidden");web_free(d);
    d=overflow_document("XXX<span style='display:inline-block;width:80px;height:10px;background:blue'></span>",width,"");
    paint(d,0);p=overflow_box(d);
    overflow_check(p->nruns>1&&p->runs[1].atomic&&p->runs[1].atomic->ellipsis_hidden,"ellipsis-hides-whole-later-atomic");web_free(d);
    d=overflow_document("X<span>\xcc\x81</span>YYYYYYYYYY",marker+prefix/3+.1f,"");
    paint(d,0);p=overflow_box(d);
    overflow_check(p->nlines&&p->lines[0].ellipsis_active&&p->lines[0].ellipsis_run>=1,"ellipsis-never-separates-combining-mark-across-style-runs");web_free(d);
    printf("webtest-text-overflow: %d checks, %d failed\n",total,failed);return failed!=0;
}
static int test_button_formatting(void) {
    const char *displays[]={"flex","inline-flex","grid","inline-grid"};
    for(int i=0;i<4;i++) {
        char html[2048];snprintf(html,sizeof html,"<!doctype html><html><head><style>body{margin:0}"
            "button{display:%s;align-items:center;gap:4px;font:16px/20px monospace;height:40px;padding:0;border:0;"
            "grid-template-columns:max-content 16px}svg{display:block;width:16px;height:16px}"
            "</style></head><body><button id=t type=button><span id=label>Label</span>"
            "<svg id=icon xmlns='http://www.w3.org/2000/svg' viewBox='0 0 16 16'><path d='M0 0H16V16H0Z'/></svg></button></body></html>",displays[i]);
        web_doc *d=web_live(html,strlen(html),BASE,"utf-8",NULL);
        overflow_check(d!=NULL,"button-formatting-document");if(!d)continue;
        uint64_t start=uptime_ms();while(d->parser&&uptime_ms()-start<5000)web_tick(d,uptime_ms());web_layout(d,VW,VH);
        box_t *b=overflow_box(d);
        overflow_check(b && (i&1?b->kind==B_ATOMIC:b->kind==(i<2?B_FLEX:B_GRID)),"button-preserves-author-display-formatting");
        int rect[2][4]={{0}};const char *ids[]={"#label","#icon"};
        for(int k=0;k<2;k++) {
            pvec v={0};css_select(d,d->root,ids[k],&v);
            overflow_check(v.n&&web_node_rect(d,v.v[0],&rect[k][0],&rect[k][1],&rect[k][2],&rect[k][3]),"button-child-real-geometry");pv_free(&v);
        }
        overflow_check(rect[1][0]>=rect[0][0]+rect[0][2]+3,"button-icon-inline-after-label");
        overflow_check(abs((2*rect[0][1]+rect[0][3])-(2*rect[1][1]+rect[1][3]))<=2,"button-label-and-icon-center-aligned");
        overflow_check(b&&b->w<200,"button-auto-width-is-fit-content-not-viewport-width");
        struct web_hit hit;paint(d,0);web_hit_test(d,rect[1][0]+8,rect[1][1]+8,&hit);
        struct web_hit action;
        overflow_check(b&&web_node_action(d,b->node,&action)&&action.kind==WEB_HIT_BUTTON,"button-native-activation-survives-flex-or-grid");
        overflow_check(hit.node!=NULL,"button-descendant-hit-remains-live");
        web_free(d);
    }
    printf("webtest-button-formatting: %d checks, %d failed\n",total,failed);return failed!=0;
}
int main(int argc,char **argv) {
    if(argc>1 && !strcmp(argv[1],"--auto-table-percent"))return test_auto_table_percent();
    if(argc>1 && !strcmp(argv[1],"--flex-freeze"))return test_flex_freeze();
    if(argc>1 && !strcmp(argv[1],"--text-overflow"))return test_text_overflow();
    if(argc>1 && !strcmp(argv[1],"--button-formatting"))return test_button_formatting();
    if(argc>1 && !strcmp(argv[1],"--flex-zero"))return test_flex_zero_basis();
    if(argc>1 && !strcmp(argv[1],"--fixed-table"))return test_fixed_table();
    printf("WEB-STAGE layout\n");fflush(stdout);
    test_layout();
    printf("WEB-STAGE test_paint\n");fflush(stdout);
    test_paint();
    printf("WEB-STAGE test_item_z_order\n");fflush(stdout);
    test_item_z_order();
    printf("WEB-STAGE test_abs_flex_position\n");fflush(stdout);
    test_abs_flex_position();
    printf("WEB-STAGE test_filter_groups\n");fflush(stdout);
    test_filter_groups();
    printf("WEB-STAGE test_forms\n");fflush(stdout);
    test_forms();
    printf("WEB-STAGE form-validation\n");fflush(stdout);
    run_native_form_validation_cases(&total,&failed);
    printf("WEB-STAGE test_text\n");fflush(stdout);
    test_text();
    printf("WEB-STAGE test_replaced_flow\n");fflush(stdout);
    test_replaced_flow();
    printf("WEB-STAGE test_float_percent_heights\n");fflush(stdout);
    test_float_percent_heights();
    printf("WEB-STAGE test_hostile\n");fflush(stdout);
    test_hostile();
    printf("webtest: %d checks, %d failed\n", total, failed);
    return failed != 0;
}
