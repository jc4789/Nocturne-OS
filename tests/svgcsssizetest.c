#include <stdio.h>
#include <string.h>
#include <math.h>
#include <web.h>
#include <gfx.h>
#include "webi.h"

/* New CSS replaced-size boundary. Does not execute the successful SVG DOM or
 * geometry group, nor the old SVG rendering/sizing suite. */
static int checks, failures;
static uint32_t pixels[320 * 240];
static void check(const char *name, bool pass) {
    checks++;
    if (!pass) { failures++; printf("FAIL svgcsssize %s\n", name); }
}
static node_t *id(web_doc *d, const char *value) {
    for (node_t *n = d->owned_nodes; n; n = n->owned_next) {
        const char *name = node_attr(n, "id");
        if (name && !strcmp(name, value) && doc_node_connected(n)) return n;
    }
    return NULL;
}
static void size(web_doc *d, const char *name, float width, float height) {
    node_t *n = id(d, name); box_t *b = n ? n->box : NULL;
    bool pass = b && b->kind == B_ATOMIC && b->atomic == AT_SVG &&
        fabsf(b->w - width) < .05f && fabsf(b->h - height) < .05f;
    check(name, pass);
    if (!pass && b) printf("svgcsssize actual %s %.2fx%.2f expected %.2fx%.2f\n",
        name, b->w, b->h, width, height);
}
static void paint(web_doc *d) {
    canvas_t c; gfx_init(&c, pixels, 320, 240, 320);
    web_paint(d, &c, 0, 0, 320, 240, 0, 0);
}
int main(void) {
    static const char html[] =
        "<style>html,body{margin:0;background:white}.def{width:200px;height:100px}"
        ".indef{width:200px}svg{display:block}</style>"
        "<div class=def><svg id=cw viewBox='0 0 20 10' style='width:40px;height:auto'>"
        "<rect width=20 height=10 fill=red></rect></svg></div>"
        "<div class=def><svg id=ch viewBox='0 0 20 10' style='height:20px;width:auto'></svg></div>"
        "<div class=def><svg id=wp viewBox='0 0 20 10' style='width:25%;height:auto'></svg></div>"
        "<div class=def><svg id=hp viewBox='0 0 20 10' style='height:50%;width:auto'></svg></div>"
        "<div class=def><svg id=aw width=40 viewBox='0 0 20 10'></svg></div>"
        "<div class=def><svg id=ah height=20 viewBox='0 0 20 10'></svg></div>"
        "<div class=def><svg id=bothattr width=60 height=40 viewBox='0 0 20 10'></svg></div>"
        "<div class=def><svg id=bothcss viewBox='0 0 20 10' style='width:40px;height:30px'></svg></div>"
        "<div class=def><svg id=bothauto viewBox='0 0 10 10'></svg></div>"
        "<div class=def><svg id=invalid viewBox='0 0 bad 10' style='width:40px;height:auto'></svg></div>"
        "<div class=def><svg id=absent style='width:auto;height:20px'></svg></div>"
        "<div class=def><svg id=zero viewBox='0 0 20 10' style='width:0;height:auto'></svg></div>"
        "<div class=indef><svg id=indef viewBox='0 0 20 10' style='width:60px;height:auto'></svg></div>"
        "<div class=def><svg id=override width=80 height=60 viewBox='0 0 20 10' style='width:20px;height:auto'></svg></div>"
        "<div class=def><svg id=maxw viewBox='0 0 20 10' style='width:80px;height:auto;max-width:40px'></svg></div>"
        "<div class=def><svg id=maxh viewBox='0 0 20 10' style='height:40px;width:auto;max-height:20px'></svg></div>"
        "<div class=def><svg id=minw viewBox='0 0 20 10' style='min-width:240px'></svg></div>"
        "<div class=def><svg id=minh viewBox='0 0 20 10' style='min-height:150px'></svg></div>"
        "<div class=def><svg id=maxauto viewBox='0 0 20 10' style='max-height:40px'></svg></div>"
        "<div class=def><svg id=contentedge viewBox='0 0 20 10' style='width:40px;height:auto;padding:5px;border:2px solid'></svg></div>"
        "<div class=def><svg id=borderedge viewBox='0 0 20 10' style='box-sizing:border-box;width:54px;height:auto;padding:5px;border:2px solid'></svg></div>"
        "<div class=def><svg id=conflict viewBox='0 0 20 10' style='max-width:80px;min-height:80px'></svg></div>"
        "<div class=indef><svg id=indefpct viewBox='0 0 20 10' style='height:50%'></svg></div>"
        "<div class=indef><svg id=noratio></svg></div>"
        "<div class=def><span id=shrink style=display:inline-block><svg id=intrinsic viewBox='0 0 20 10' style=height:20px></svg></span></div>";
    web_doc *d = web_parse(html, strlen(html), "https://svg-css-size.test/", "utf-8");
    check("new CSS SVG document allocated", d != NULL);
    if (!d) goto done;
    web_layout(d, 320, 240);
    size(d,"cw",40,20); size(d,"ch",40,20);
    size(d,"wp",50,25); size(d,"hp",100,50);
    size(d,"aw",40,20); size(d,"ah",40,20);
    size(d,"bothattr",60,40); size(d,"bothcss",40,30);
    size(d,"bothauto",200,200); size(d,"invalid",40,100);
    size(d,"absent",200,20); size(d,"zero",0,0);
    size(d,"indef",60,30); size(d,"override",20,10);
    size(d,"maxw",40,20); size(d,"maxh",40,20);
    size(d,"minw",240,120); size(d,"minh",300,150);
    size(d,"maxauto",80,40); size(d,"contentedge",40,20);
    size(d,"borderedge",40,20); size(d,"conflict",80,80);
    size(d,"indefpct",200,100); size(d,"noratio",200,150);
    size(d,"intrinsic",40,20);
    node_t *cw = id(d,"cw"), *edge = id(d,"borderedge"), *shrink = id(d,"shrink");
    check("CSS-only SVG has no invented width or height attribute", cw &&
        !node_attr(cw,"width") && !node_attr(cw,"height") && cw->namespace_id == NS_SVG);
    check("CSS auto height remains authored auto", cw && cw->style && len_auto(&cw->style->height));
    check("border-box ratio uses content viewport without dropping edges", edge && edge->box &&
        fabsf(edge->box->w+edge->box->p[1]+edge->box->p[3]+edge->box->b[1]+edge->box->b[3]-54)<.05f);
    check("intrinsic contribution agrees with CSS height and viewBox ratio", shrink &&
        shrink->box && fabsf(shrink->box->w-40)<.05f);
    paint(d);
    check("CSS-only ratio raster reaches viewport bottom-right", (pixels[19*320+39]&0xffffff)==0xff0000);
    check("CSS-only ratio raster stays inside viewport width", (pixels[19*320+40]&0xffffff)==0xffffff);
    check("CSS-only ratio raster does not fill unrelated parent height", (pixels[20*320+1]&0xffffff)==0xffffff);
    doc_node_attr(d,cw,"viewBox","0 0 10 10"); web_layout(d,320,240);
    size(d,"cw",40,40);
    doc_node_attr(d,cw,"style","width:auto;height:15px"); web_layout(d,320,240);
    size(d,"cw",15,15);
    doc_node_attr(d,cw,"viewBox","0 0 20 10"); web_layout(d,320,240);
    size(d,"cw",30,15);
    doc_node_attr(d,cw,"style","width:0;height:auto"); web_layout(d,320,240);
    size(d,"cw",0,0);
    web_free(d);
done:
    printf("svgcsssize: %d checks, %d failures\n", checks, failures);
    return failures != 0;
}
