/* Native viewport-fixed document coordinates must agree across DOM geometry,
   flat shadow trees, observers, clipping, hit testing, raster and top layer. */
#include "nocturne.h"
#include "webi.h"
#include <stdio.h>

static int checks, failures, errors;
static int scroll_x, scroll_y;
static bool initial_ready, moved_ready;
static uint32_t pixels[480 * 240];
static void check(const char *name, bool pass) {
    checks++;
    if (!pass) { failures++; printf("FAIL fixedvisual %s\n", name); }
}
static void console(void *opaque, int level, const char *message) {
    if (!strncmp(message, "OK fixed ", 9)) { checks++; puts(message); }
    else if (!strncmp(message, "FAIL fixed ", 11)) { checks++; failures++; puts(message); }
    else if (!strcmp(message, "FIXED-IO-INITIAL-READY")) initial_ready = true;
    else if (!strcmp(message, "FIXED-IO-MOVED-READY")) moved_ready = true;
    else if (level >= 2) { errors++; printf("ERROR fixedvisual %s\n", message); }
}
static void scroll(void *opaque, int *x, int *y) { *x = scroll_x; *y = scroll_y; }
static node_t *id(web_doc *d, const char *value) {
    for (node_t *n = d->owned_nodes; n; n = n->owned_next) {
        const char *name = node_attr(n, "id");
        if (name && !strcmp(name, value) && doc_node_connected(n)) return n;
    }
    return NULL;
}
static bool at(web_doc *d, node_t *n, int x, int y, int w, int h) {
    int nx = 0, ny = 0, nw = 0, nh = 0;
    return n && web_node_rect(d, n, &nx, &ny, &nw, &nh) && nx == x && ny == y && nw == w && nh == h;
}
static void paint(web_doc *d) {
    canvas_t c; gfx_init(&c, pixels, 480, 240, 480);
    web_paint(d, &c, 0, 0, 480, 240, scroll_x, scroll_y);
}
static bool color(int x, int y, uint32_t rgb) { return (pixels[y * 480 + x] & 0xffffff) == rgb; }
static bool hit(web_doc *d, node_t *n, int x, int y) {
    struct web_hit h = {0};
    return n && web_hit_test(d, x + scroll_x, y + scroll_y, &h) && h.kind == WEB_HIT_LINK && h.node == n;
}
static bool evaluate(web_doc *d, const char *source) { return web_console_eval(d, source, strlen(source)); }
static bool pump(web_doc *d, bool *ready) {
    uint64_t deadline = uptime_ms() + 3000;
    while (!*ready && uptime_ms() < deadline) { web_tick(d, uptime_ms()); msleep(10); }
    return *ready;
}

static const char page[] =
    "<!doctype html><style>html,body{margin:0}body{width:1200px;height:1200px}"
    "#ordinary{position:fixed;left:20px;top:30px;width:100px;height:60px;background:lime}"
    "#ordinary-child{display:block;width:40px;height:20px;background:red}"
    "#outer{position:absolute;left:0;top:600px;width:5px;height:0;overflow:hidden}"
    "#slotted{position:fixed;left:160px;top:20px;width:100px;height:80px;background:blue}"
    "#slotchild{position:absolute;left:60px;top:0;width:30px;height:20px;background:yellow}"
    "#scrollport{position:relative;width:60px;height:40px;overflow:auto}"
    "#inside{position:absolute;left:0;top:60px;width:40px;height:20px;background:red}"
    "#zero{position:fixed;left:410px;top:20px;width:20px;height:0}"
    "#normal{position:absolute;left:450px;top:700px;width:40px;height:40px}"
    "#modal{position:fixed;left:80px;top:120px;width:100px;height:40px;margin:0;padding:0;border:0;background:magenta}"
    "#modal-child{display:block;width:30px;height:20px;background:red}"
    "#fixedframe{position:fixed;left:320px;top:120px;width:60px;height:40px;border:0}"
    "</style><div id=ordinary><a id=ordinary-child href='/ordinary'></a></div>"
    "<div id=outer><div id=slot-host><div id=slotted><a id=slotchild href='/slot'></a>"
    "<div id=scrollport><a id=inside href='/inside'></a></div></div></div><div id=shadow-host></div></div>"
    "<div id=zero></div><div id=normal></div>"
    "<iframe id=fixedframe srcdoc='<body style=\"margin:0;background:orange\"></body>'></iframe>"
    "<dialog id=modal><a id=modal-child href='/modal'></a></dialog>";

static const char init[] =
    "globalThis.fixedAssert=(name,value)=>console.log((value?'OK fixed ':'FAIL fixed ')+name);"
    "document.getElementById('slot-host').attachShadow({mode:'open'}).innerHTML='<slot></slot>';"
    "globalThis.fixedShadow=document.getElementById('shadow-host').attachShadow({mode:'open'});"
    "fixedShadow.innerHTML='<div id=shadow-fixed style=\"position:fixed;left:320px;top:20px;width:80px;height:50px;background:cyan\">"
    "<a id=shadow-child href=/shadow style=\"display:block;width:30px;height:20px\"></a></div>';"
    "globalThis.fixedState={phase:0,entries:{},observer:null};"
    "fixedState.observer=new IntersectionObserver(entries=>{for(const entry of entries)fixedState.entries[entry.target.id]=entry;"
    "if(Object.keys(fixedState.entries).length===5&&fixedState.phase===0)console.log('FIXED-IO-INITIAL-READY');"
    "if(fixedState.phase===1&&fixedState.entries.inside?.isIntersecting)console.log('FIXED-IO-MOVED-READY');});"
    "for(const target of [document.getElementById('ordinary-child'),document.getElementById('slotchild'),"
    "fixedShadow.getElementById('shadow-child'),document.getElementById('zero'),document.getElementById('inside')])fixedState.observer.observe(target);"
    "globalThis.fixedGeometry=stage=>{"
    "const ordinary=document.getElementById('ordinary').getBoundingClientRect(),child=document.getElementById('ordinary-child').getBoundingClientRect(),"
    "slotted=document.getElementById('slotted').getBoundingClientRect(),shadow=fixedShadow.getElementById('shadow-child').getBoundingClientRect(),"
    "normal=document.getElementById('normal').getBoundingClientRect();"
    "fixedAssert(stage+'-ordinary-viewport',ordinary.x===20&&ordinary.y===30);"
    "fixedAssert(stage+'-fixed-child-viewport',child.x===20&&child.y===30);"
    "fixedAssert(stage+'-flat-slot-viewport',slotted.x===160&&slotted.y===20);"
    "fixedAssert(stage+'-shadow-viewport',shadow.x===320&&shadow.y===20);"
    "fixedAssert(stage+'-normal-document-scroll',normal.x===450-scrollX&&normal.y===700-scrollY);};";

int main(void) {
    struct web_host host = {.console=console, .scroll=scroll, .js_task_budget_ms=5000};
    web_doc *d = web_live(page, sizeof page - 1, "https://fixed-visual.test/", "utf-8", &host);
    check("native live document allocated", d != NULL); if (!d) goto end;
    /* web_live is incremental: finish the initial parser task before author
       setup reads elements. A layout call alone does not advance JS parsing. */
    for (int i = 0; i < 8; i++) { web_tick(d, uptime_ms()); msleep(1); }
    web_layout(d, 480, 240);
    check("flat shadow and observer setup", evaluate(d, init));
    web_layout(d, 480, 240);
    check("initial native observer delivery", pump(d, &initial_ready));
    check("initial JS geometry evaluated", evaluate(d, "fixedGeometry('initial');"
        "fixedAssert('IO-ordinary',fixedState.entries['ordinary-child']?.isIntersecting);"
        "fixedAssert('IO-slot-escapes-external-zero-clip',fixedState.entries.slotchild?.isIntersecting);"
        "fixedAssert('IO-shadow-escapes-external-zero-clip',fixedState.entries['shadow-child']?.isIntersecting);"
        "fixedAssert('IO-zero-area-ratio',fixedState.entries.zero?.isIntersecting&&fixedState.entries.zero.intersectionRatio===1);"
        "fixedAssert('IO-internal-scroll-clip',fixedState.entries.inside?.isIntersecting===false);"));
    node_t *ordinary = id(d, "ordinary"), *child = id(d, "ordinary-child"), *slotted = id(d, "slotted"),
           *slotchild = id(d, "slotchild"), *shadow = id(d, "shadow-child"), *inside = id(d, "inside"), *frame = id(d, "fixedframe");
    check("ordinary native document rectangle", at(d, ordinary, 20, 30, 100, 60));
    check("slotted native document rectangle", at(d, slotted, 160, 20, 100, 80));
    check("shadow child native document rectangle", at(d, shadow, 320, 20, 30, 20));
    paint(d);
    check("ordinary fixed raster", color(80, 70, 0x00ff00));
    check("fixed child raster", color(25, 35, 0xff0000));
    check("slotted fixed layer escapes external overflow raster", color(225, 25, 0xffff00));
    check("shadow fixed escapes external overflow raster", color(350, 45, 0x00ffff));
    check("ordinary fixed child hit", hit(d, child, 25, 35));
    check("slotted fixed layer escapes external overflow hit", hit(d, slotchild, 225, 25));
    check("inside fixed scrollport is initially clipped", !hit(d, inside, 165, 85));
    check("fixed frame initial document rectangle", at(d, frame, 320, 120, 60, 40));
    check("fixed frame paints real child document", color(325, 125, 0xffa500));
    scroll_x = 70; scroll_y = 300; web_viewport_position(d, scroll_x, scroll_y);
    check("moved JS geometry evaluated", evaluate(d, "fixedGeometry('moved');fixedState.phase=1;document.getElementById('scrollport').scrollTop=40;"));
    check("internal scroll observer delivers after viewport move", pump(d, &moved_ready));
    check("moved observer geometry evaluated", evaluate(d,
        "fixedAssert('IO-inside-fixed-after-inner-scroll',fixedState.entries.inside?.isIntersecting);"
        "fixedAssert('IO-inside-fixed-viewport-rect',fixedState.entries.inside?.boundingClientRect.x===160&&fixedState.entries.inside.boundingClientRect.y===40);"
        "fixedState.observer.disconnect();"));
    check("ordinary native document rectangle includes viewport once", at(d, ordinary, 90, 330, 100, 60));
    check("fixed child native document rectangle includes viewport once", at(d, child, 90, 330, 40, 20));
    check("slotted native rectangle includes viewport once", at(d, slotted, 230, 320, 100, 80));
    check("shadow native rectangle includes viewport once", at(d, shadow, 390, 320, 30, 20));
    check("inner element scroll remains independent of viewport", at(d, inside, 230, 340, 40, 20));
    check("fixed frame native rectangle includes viewport once", at(d, frame, 390, 420, 60, 40));
    paint(d);
    check("ordinary fixed raster stays on viewport", color(80, 70, 0x00ff00));
    check("slotted fixed raster stays on viewport", color(225, 25, 0xffff00));
    check("scrolled fixed descendant raster", color(165, 45, 0xff0000));
    check("shadow fixed raster stays on viewport", color(350, 45, 0x00ffff));
    check("fixed frame crop is not double shifted", color(325, 125, 0xffa500));
    check("ordinary fixed hit stays on viewport", hit(d, child, 25, 35));
    check("slotted fixed hit stays on viewport", hit(d, slotchild, 225, 25));
    check("scrolled fixed descendant hit", hit(d, inside, 165, 45));
    check("modal opens through actual JS API", evaluate(d, "document.getElementById('modal').showModal();"
        "const modalRect=document.getElementById('modal').getBoundingClientRect();"
        "fixedAssert('modal-fixed-viewport-rect',modalRect.x===80&&modalRect.y===120&&modalRect.width===100&&modalRect.height===40);"));
    node_t *modal = id(d, "modal"), *modal_child = id(d, "modal-child");
    check("modal native rectangle includes viewport once", at(d, modal, 150, 420, 100, 40));
    check("modal child native rectangle includes viewport once", at(d, modal_child, 150, 420, 30, 20));
    paint(d);
    check("modal fixed raster is not double shifted", color(160, 150, 0xff00ff));
    check("modal child hit is not double shifted", hit(d, modal_child, 85, 125));
    check("native JS runtime has no exceptions", errors == 0);
    web_free(d);
end:
    printf("fixedvisual: %d checks, %d failures\n", checks, failures);
    return failures != 0;
}
