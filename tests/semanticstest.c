/* Native HTML semantic layout, paint, list order and disclosure interaction.
 * Run inside Nocturne; host C syntax or a JS mock is not rendering evidence. */
#include <stdio.h>
#include <math.h>
#include "nocturne.h"
#include "webi.h"
#include "elements.h"

static int checks, failures;
static uint32_t pixels[400 * 240];
static void check(const char *name, bool pass) {
    checks++;
    if (!pass) { failures++; printf("FAIL semantics %s\n", name); }
}
static web_doc *load(const char *html) {
    web_doc *d = web_parse(html, strlen(html), "http://semantics.test/", "utf-8");
    check("document allocation", d != NULL);
    if (d) web_layout(d, 400, 240);
    return d;
}
static node_t *id(web_doc *d, const char *name) {
    char selector[128];snprintf(selector,sizeof selector,"#%s",name);
    pvec found = {0};
    bool selected = css_select(d,d->root,selector,&found);
    node_t *n = selected && found.n ? found.v[0] : NULL;
    pv_free(&found);
    check(name, n != NULL);
    return n;
}
static bool marker(node_t *n, const char *text) {
    return n && n->box && n->box->marker && !strcmp(n->box->marker, text);
}
static void paint(web_doc *d) {
    canvas_t canvas;
    gfx_init(&canvas, pixels, 400, 240, 400);
    web_paint(d, &canvas, 0, 0, 400, 240, 0, 0);
}
static void drain(web_doc *d) { while (doc_details_take_toggle(d, NULL, NULL)) {} }

static void semantic_layout(void) {
    web_doc *d = load("<!doctype html><body style='margin:0'>"
        "<section id=section><nav id=nav><article id=article><aside id=aside><header id=header>"
        "<footer id=footer><main id=main><figure id=figure><figcaption id=caption>text</figcaption>"
        "</figure></main></footer></header></aside></article></nav></section>");
    if (!d) return;
    const char *names[] = {"section", "nav", "article", "aside", "header", "footer", "main", "figure", "caption"};
    for (unsigned i = 0; i < sizeof names / sizeof names[0]; i++) {
        node_t *n = id(d, names[i]);
        check("semantic block native box", n && n->style->display == D_BLOCK && n->box && n->box->kind == B_BLOCK);
    }
    web_free(d);
    d = load("<body style='margin:0'><mark id=mark style='display:block;width:40px;height:20px'></mark>");
    if (!d) return;
    node_t *mark = id(d, "mark");
    check("mark real default color", mark && mark->style->bg_color == RGB(255,255,0) && mark->style->color == RGB(0,0,0));
    paint(d);
    check("mark painted highlight", (pixels[5 * 400 + 5] & 0xffffff) == 0xffff00);
    web_free(d);
}

static void reversed_lists(void) {
    web_doc *d = load("<body style='margin:0'><ol id=list reversed><li id=one>one</li>"
        "<div><li id=two>two</li></div><li id=three value=8>three</li><li id=four>four</li>"
        "<li id=hidden style='display:none'>hidden</li><li id=invalid value=bad>invalid</li>"
        "<li><ol><li id=nested>nested</li></ol></li></ol>");
    if (!d) return;
    node_t *list = id(d,"list"), *one = id(d,"one"), *two = id(d,"two"), *three = id(d,"three"),
           *four = id(d,"four"), *invalid = id(d,"invalid"), *nested = id(d,"nested");
    check("reversed count owned visible items", marker(one,"6. "));
    check("wrapped item shares owning list", marker(two,"5. "));
    check("li value resets reversed order", marker(three,"8. ") && marker(four,"7. "));
    check("invalid value does not become zero", marker(invalid,"6. "));
    check("nested list independent ordinal", marker(nested,"1. "));
    check("start native mutation", doc_node_attr(d,list,"start","-2"));web_layout(d,400,240);
    check("negative reversed start", marker(one,"-2. ") && marker(two,"-3. "));
    check("invalid start native mutation", doc_node_attr(d,list,"start","notanumber"));web_layout(d,400,240);
    check("invalid start restores reversed count", marker(one,"6. "));
    check("overflow start native mutation", doc_node_attr(d,list,"start","999999999999999999999999999"));web_layout(d,400,240);
    check("overflow start bounded", marker(one,"6. "));
    check("remove reversed native mutation", doc_node_attr(d,list,"reversed",NULL));web_layout(d,400,240);
    check("forward default marker", marker(one,"1. ") && marker(two,"2. ") && marker(four,"9. "));
    web_free(d);
}

static void disclosure_layout_and_action(void) {
    web_doc *d = load("<style>summary{height:20px;line-height:20px;list-style:none;background:lime}"
        "p{height:20px;margin:0;background:red}details{line-height:20px}</style><body style='margin:0'>"
        "<details id=details>hidden-prefix<summary id=summary><span id=summary-child>legend</span>"
        "<button id=button type=button>button</button></summary>hidden-body<p id=body>body</p><input id=hidden-input>"
        "<summary id=second>second</summary></details>");
    if (!d) return;
    node_t *details = id(d,"details"), *summary = id(d,"summary"), *child = id(d,"summary-child"),
           *body = id(d,"body"), *second = id(d,"second"), *button = id(d,"button"), *hidden_input = id(d,"hidden-input");
    check("first summary direct native child", doc_details_summary(details) == summary);
    check("closed details only summary box", details->box && summary->box && !body->box && !second->box);
    float closed = details->box->h;
    check("summary first visual child", summary->box && box_abs_y(summary->box) == box_abs_y(details->box));
    check("closed hidden text not findable", web_find(d,"hidden-prefix",0) < 0 && web_find(d,"hidden-body",0) < 0);
    paint(d);check("closed summary painted", (pixels[5 * 400 + 200] & 0xffffff) == 0x00ff00);
    struct web_hit hit;
    check("summary native hit", web_hit_test(d,200,5,&hit) && hit.kind == WEB_HIT_DETAILS && hit.node == details);
    check("summary target not replaced by details", web_node_at(d,200,5) == summary);
    check("summary native action", web_node_action(d,summary,&hit) && hit.kind == WEB_HIT_DETAILS && hit.node == details);
    check("summary child native action", web_node_action(d,child,&hit) && hit.kind == WEB_HIT_DETAILS && hit.node == details);
    check("disclosure focus helper chooses summary", web_disclosure_focus(details) == summary);
    web_focus(d,web_disclosure_focus(details));check("summary receives actual native focus", web_focused(d) == summary);
    web_focus(d,hidden_input);check("closed details control cannot steal native focus", web_focused(d) == summary);
    web_focus(d,NULL);web_focus(d,hidden_input);check("closed details control cannot gain native focus", !web_focused(d));
    check("interactive child owns action", web_node_action(d,button,&hit) && hit.kind == WEB_HIT_BUTTON && hit.node == button);
    check("second summary not actionable", !web_node_action(d,second,&hit));
    check("native disclosure open", doc_details_toggle(d,details));web_layout(d,400,240);
    check("opened details real geometry", details->box && details->box->h > closed && body->box && second->box);
    check("opened hidden text findable", web_find(d,"hidden-prefix",0) >= 0 && web_find(d,"hidden-body",0) >= 0);
    check("native disclosure close", doc_details_toggle(d,details));web_layout(d,400,240);
    check("close rebuild removes descendants", details->box && details->box->h == closed && !body->box && !second->box);
    check("author display cannot reveal closed body", doc_node_attr(d,body,"style","display:block;height:200px"));web_layout(d,400,240);
    check("closed content native suppression", !body->box && details->box->h == closed);
    web_free(d);
    d = load("<body style='margin:0'><details id=default><p style='height:60px;margin:0'>hidden default body</p></details>");
    if (!d) return;
    details = id(d,"default");
    check("default legend rendered", details->box && details->box->h > 0 && !doc_details_summary(details));
    check("default legend not synthetic DOM", details->first && details->first->tag == T_p && details->first == details->last);
    closed = details->box->h;
    check("default legend native hit", web_hit_test(d,150,5,&hit) && hit.kind == WEB_HIT_DETAILS && hit.node == details);
    check("default legend keyboard native action", web_node_action(d,details,&hit) && hit.kind == WEB_HIT_DETAILS && hit.node == details);
    check("default legend focus helper chooses native host", web_disclosure_focus(details) == details);
    web_focus(d,web_disclosure_focus(details));check("default legend receives actual native focus", web_focused(d) == details);
    check("default open", doc_details_toggle(d,details));web_layout(d,400,240);
    check("default legend retains native body", details->box->h > closed && details->first == details->last);
    check("default body whitespace not toggle hit", !web_hit_test(d,150,(int)closed+40,&hit) || hit.kind != WEB_HIT_DETAILS);
    check("default body child not toggle action", !web_node_action(d,details->first,&hit));
    web_free(d);
}

static void disclosure_state(void) {
    web_doc *d = load("<body><details id=a name=g></details><details id=b name=g></details>");
    if (!d) return;
    node_t *a = id(d,"a"), *b = id(d,"b");
    drain(d);
    check("open first named detail", doc_node_attr(d,a,"open",""));
    check("open second named detail", doc_node_attr(d,b,"open",""));
    check("group attributes native exclusivity", !node_attr(a,"open") && node_attr(b,"open"));
    bool old_open = true, new_open = true;
    check("second member task", doc_details_take_toggle(d,&old_open,&new_open) == b && !old_open && new_open);
    check("coalesced task requeued after other member", doc_details_take_toggle(d,&old_open,&new_open) == a && !old_open && !new_open);
    check("native queue fully drained", !doc_details_take_toggle(d,NULL,NULL));
    check("empty name set", doc_node_attr(d,a,"name",""));
    check("empty name opens independently", doc_node_attr(d,a,"open","") && node_attr(b,"open"));
    check("name conflict closes changed element", doc_node_attr(d,a,"name","g") && !node_attr(a,"open") && node_attr(b,"open"));
    node_t *host = doc_node_create(d,N_ELEM,"div",NULL,0), *shadow_details = doc_node_create(d,N_ELEM,"details",NULL,0);
    check("shadow fixture allocation", host && shadow_details);
    node_t *shadow = host ? doc_shadow_attach(d,host,false,false,false,false,false) : NULL;
    check("shadow insertion", shadow && doc_node_move(d,d->body,host,NULL) && doc_node_move(d,shadow,shadow_details,NULL));
    check("shadow group independent", shadow_details && doc_node_attr(d,shadow_details,"name","g") &&
        doc_node_attr(d,shadow_details,"open","") && node_attr(b,"open"));
    drain(d);web_free(d);
}

static void disclosure_parser_snapshots(void) {
    web_doc *d = load("<!doctype html><body></body>");
    if (!d) return;
    const char *source = "<!doctype html><body><details id=a name=group open></details>"
        "<details id=b name=group open></details><script></script>"
        "<details id=c name=group open></details><script></script><script></script></body>";
    struct html_parser *parser = html_begin(d,source,strlen(source),"utf-8",true);
    check("incremental parser allocation",parser != NULL);
    if (!parser) { web_free(d); return; }
    node_t *script = NULL;
    check("first parser script boundary",html_resume(parser,&script) == 1 && script);
    node_t *a = id(d,"a"), *b = id(d,"b");
    check("parsed group preserves first open",a && b && node_attr(a,"open") && !node_attr(b,"open"));
    bool old_open = true,new_open = false;
    check("initial open queues one toggle",doc_details_take_toggle(d,&old_open,&new_open) == a && !old_open && new_open);
    check("initial conflict queues coalesced closed state",doc_details_take_toggle(d,&old_open,&new_open) == b && !old_open && !new_open);
    check("initial tasks not duplicated",!doc_details_take_toggle(d,NULL,NULL));
    check("second parser script boundary",html_resume(parser,&script) == 1 && script);
    node_t *c = id(d,"c");
    check("parser resume exports enforced attributes",node_attr(a,"open") && !node_attr(b,"open") && c && !node_attr(c,"open"));
    check("new group conflict only new toggle",doc_details_take_toggle(d,&old_open,&new_open) == c && !old_open && !new_open);
    check("snapshot import no duplicate toggle",!doc_details_take_toggle(d,NULL,NULL));
    check("native script-boundary attribute mutation",doc_node_attr(d,b,"open","") && !node_attr(a,"open"));
    check("third parser script boundary",html_resume(parser,&script) == 1 && script);
    check("script group state survives import",!node_attr(a,"open") && node_attr(b,"open") && !node_attr(c,"open"));
    check("script open task preserved",doc_details_take_toggle(d,&old_open,&new_open) == b && !old_open && new_open);
    check("script group close task preserved",doc_details_take_toggle(d,&old_open,&new_open) == a && old_open && !new_open);
    check("resume state tasks not duplicated",!doc_details_take_toggle(d,NULL,NULL));
    check("incremental parser completion",html_resume(parser,&script) == 0);
    html_finish(parser);web_free(d);
}

static void soft_break_layout(void) {
    web_doc *d = load("<body style='margin:0'><div id=wrap style='width:90px;font-family:monospace;"
        "font-size:16px;line-height:20px'><span id=left>AAAAAA</span><wbr><span id=right>BBBBBB</span></div>");
    if (!d) return;
    node_t *wrap = id(d,"wrap"), *left = id(d,"left"), *right = id(d,"right");
    check("wbr soft wraps native lines", left->anchor_block && right->anchor_block && right->anchor_dy > left->anchor_dy);
    check("wbr widen native mutation", doc_node_attr(d,wrap,"style","width:400px;font-family:monospace;font-size:16px;line-height:20px"));
    web_layout(d,400,240);check("wbr not forced", right->anchor_dy == left->anchor_dy);
    check("wbr nowrap native mutation", doc_node_attr(d,wrap,"style","width:90px;white-space:nowrap;font-family:monospace;font-size:16px;line-height:20px"));
    web_layout(d,400,240);check("wbr nowrap suppresses soft wrap", right->anchor_dy == left->anchor_dy);
    web_free(d);
}

static bool pixel_is(int x,int y,uint32_t color) {
    return x>=0 && y>=0 && x<400 && y<240 && (pixels[y*400+x]&0xffffff)==color;
}
static void marquee_motion_and_hit(void) {
    const char *html="<!doctype html><body style='margin:0;background:white'>"
        "<marquee id=moving behavior=alternate direction=left scrollamount=20 scrolldelay=10 truespeed "
        "style='display:block;width:200px;height:24px;background:blue'>"
        "<a id=target href='/destination' style='display:block;width:40px;height:20px;background:red'></a>"
        "</marquee>";
    struct web_host host={0};
    web_doc *d=web_live(html,strlen(html),"http://semantics.test/","utf-8",&host);
    check("marquee live document",d!=NULL);if(!d)return;
    for(int i=0;i<8;i++)web_tick(d,uptime_ms());
    web_layout(d,400,240);
    node_t *moving=id(d,"moving"),*target=id(d,"target");
    check("marquee native child geometry",moving&&moving->box&&target&&target->box);
    if(!moving||!moving->box||!target||!target->box){web_free(d);return;}
    box_t *root=d->root_box,*child_box=target->box;
    float layout_x=box_abs_x(child_box),layout_y=box_abs_y(child_box);
    uint64_t now=uptime_ms();
    web_marquee_tick(d,now);
    float before=box_visual_x(child_box),y=box_visual_y(child_box);
    check("marquee registered for native tick",d->marquees.n==1&&moving->marquee_initialized);
    check("marquee initial visible child",before>=0&&before+child_box->w<=200&&child_box->h==20);
    paint(d);
    check("marquee initial painted child",pixel_is((int)before+5,(int)y+5,0xff0000));
    struct web_hit hit;
    check("marquee initial child native hit",web_hit_test(d,(int)before+5,(int)y+5,&hit)&&hit.kind==WEB_HIT_LINK&&hit.node==target);
    d->paint_dirty=false;
    web_marquee_tick(d,now+10);
    float moved=box_visual_x(child_box);
    check("marquee real motion changes visual geometry",fabsf(moved-(before-20))<0.01f&&fabsf(box_visual_y(child_box)-y)<0.01f);
    check("marquee motion preserves layout coordinates",box_abs_x(child_box)==layout_x&&box_abs_y(child_box)==layout_y);
    check("marquee motion preserves layout tree",d->layout_valid&&d->root_box==root&&target->box==child_box);
    check("marquee motion requests repaint",d->paint_dirty);
    paint(d);
    check("marquee translated child painted",pixel_is((int)moved+5,(int)y+5,0xff0000));
    check("marquee old painted position cleared",pixel_is((int)before+35,(int)y+5,0x0000ff));
    check("marquee translated target and link action",web_node_at(d,(int)moved+5,(int)y+5)==target&&
        web_hit_test(d,(int)moved+5,(int)y+5,&hit)&&hit.kind==WEB_HIT_LINK&&hit.node==target&&
        hit.href&&!strcmp(hit.href,"http://semantics.test/destination"));
    check("marquee previous position not stale target",web_node_at(d,(int)before+35,(int)y+5)!=target);
    check("marquee next native deadline",web_marquee_deadline(d,now+10)==10);
    web_marquee_set(moving,false);d->paint_dirty=false;
    web_marquee_tick(d,now+1000);
    check("marquee stop freezes native visual geometry",fabsf(box_visual_x(child_box)-moved)<0.01f&&!d->paint_dirty);
    check("marquee stop removes ticking deadline",web_marquee_deadline(d,now+1000)==-1);
    paint(d);
    check("marquee stopped child stays painted and hittable",pixel_is((int)moved+5,(int)y+5,0xff0000)&&
        web_node_at(d,(int)moved+5,(int)y+5)==target);
    web_marquee_set(moving,true);
    uint64_t resumed=moving->marquee_last;
    web_marquee_tick(d,resumed+9);
    check("marquee resume does not accumulate stopped time",fabsf(box_visual_x(child_box)-moved)<0.01f);
    web_marquee_tick(d,resumed+10);
    float restarted=box_visual_x(child_box);
    check("marquee restart advances one interval",fabsf(restarted-(moved-20))<0.01f);
    paint(d);
    check("marquee restarted paint and hit agree",pixel_is((int)restarted+5,(int)y+5,0xff0000)&&
        web_hit_test(d,(int)restarted+5,(int)y+5,&hit)&&hit.node==target);
    check("marquee restart retains layout boxes",d->layout_valid&&d->root_box==root&&target->box==child_box&&
        box_abs_x(child_box)==layout_x&&box_abs_y(child_box)==layout_y);
    web_free(d);
}

int main(void) {
    semantic_layout();reversed_lists();disclosure_layout_and_action();disclosure_state();disclosure_parser_snapshots();soft_break_layout();
    marquee_motion_and_hit();
    printf("semanticstest: %d checks, %d failures\n",checks,failures);
    return failures != 0;
}
