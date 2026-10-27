/* Shared real CSS parser/cascade contracts. QEMU/native results are distinct
   from website interaction and the standalone host extraction probe. */
#include <stdio.h>
#include "nocturne.h"
#include "webi.h"
static int checks, failures;
static void check(const char *name, bool actual, bool expected) {
    checks++;
    if (actual != expected) { failures++; printf("FAIL csssupports %s: got=%d expected=%d\n", name, actual, expected); }
}
struct decl_case { const char *property, *value; bool expected; };
static const struct decl_case declarations[] = {
    {"color","red",true}, {"COLOR","#abcdef",true}, {"display","flex",true},
    {"display","inline grid",true}, {"width","calc(100% - 1px)",true},
    {"padding","1px 2px 3px 4px",true}, {"gap","1px 2px",true},
    {"border","1px solid red",true}, {"background-image","linear-gradient(red, blue)",true},
    {"color","var(--test, red)",true}, {"color","var(--test)",true},
    {"color","var(--test,)",true}, {"color","VAR(--test, red)",true},
    {"color","v\\61r(--test, r\\65 d)",true}, {"color","var(--a, var(--b, red))",true},
    {"color","r\\65 d",true}, {"color","red/**/",true},
    {"color","inherit",true}, {"display","initial",true},
    {"--test","",true}, {"--test","[foo] {bar} var(--x, red)",true},
    {"--日本語","1px",true},
    {"not-a-property","red",false}, {" color","red",false}, {"color ","red",false},
    {"c\\6flor","red",false}, {"color","",false}, {"color","nonsense",false},
    {"color","red blue",false}, {"color","red !important",false}, {"color","red;display:flex",false},
    {"color","rgb(1,2,3,4,5)",false}, {"display","flex nonsense",false},
    {"display","flex grid",false}, {"display","inline inline",false}, {"z-index","1.5",false},
    {"font-family","sans-serif,nonsense()",false}, {"border","solid dashed",false},
    {"flex","auto auto",false}, {"background","nonsense()",false}, {"width","fit-content(nonsense)",false},
    {"width","-1px",false}, {"width","calc(1px",false},
    {"gap","nonsense",false}, {"gap","1px nonsense",false}, {"gap","1px 2px 3px",false},
    {"padding","1px 2px 3px 4px 5px",false}, {"inset","nonsense",false},
    {"overflow","nonsense",false}, {"text-decoration","nonsense",false},
    {"background-image","nonsense()",false}, {"background-image","url(a),url(b)",false},
    {"display","ruby",false}, {"text-wrap-mode","balance",false},
    {"color","var(test, red)",false}, {"color","var(--, red)",false},
    {"color","var(--test extra, red)",false}, {"color","var(--test",false},
    {"color","var(--test, var(no))",false}, {"color","env(foo)",false},
    {"color","var(--a) !garbage",false}, {"--","x",false}, {"--test","!foo",false},
    {"color","#var(--x)",false}, {"color","@var(--x)",false},
    {"background-image","url(var(--x))",false}, {"--test","url(var(--x))",false},
    {"--test","url(foo bar)",false}, {"background-image","url(\"var(--x)\")",true},
    {"background-image","url(foo\\ bar)",true},
    {"--test","x;",false}, {"--test", "[}",false}, {"--test","var(foo)",false},
    {"color","red /*",false}, {"content","\"unterminated",false},
    {"filter","none",true}, {"filter","opacity()",true}, {"filter","opacity(.5)",true},
    {"filter","opacity(25%)",true}, {"filter","opacity(200%)",true}, {"filter","opacity(2)",true},
    {"filter","opacity(.5) opacity(50%)",true}, {"filter","OPACITY( 1e-1 )",true},
    {"filter","opacity(-.1)",false}, {"filter","opacity(-10%)",false},
    {"filter","opacity(1px)",false}, {"filter","opacity(1,2)",false}, {"filter","opacity(1 2)",false},
    {"filter","opacity(1",false}, {"filter","opacity(1))",false}, {"filter","none opacity(1)",false},
    {"filter","opacity(1) garbage",false}, {"filter","blur(0)",false}, {"filter","brightness(1)",false},
    {"filter","opacity(.5) blur(1px)",false}, {"filter","url(#effect)",false},
    {"filter","opacity(1e999999999999999999999999999999)",false},
};
struct cond_case { const char *condition; bool expected; };
static const struct cond_case conditions[] = {
    {"(color:red)",true}, {"((color:red))",true}, {"(c\\6flor:r\\65 d)",true},
    {"(color:var(--test,red))",true}, {"not (color:nonsense)",true},
    {"(color:red) and (display:flex)",true}, {"(color:red) or (display:nonsense)",true},
    {"(color:red)and (display:flex)",true}, {"not/**/(color:nonsense)",true},
    {"(color:red !important)",true}, {"(--test:)",true},
    {"not future(unknown)",true}, {"not (future unknown)",true},
    {"selector(div > .item)",true}, {"selector(:not(.item))",true},
    {"selector(:is(div,span))",true}, {"selector(:nth-child(2n+1))",true},
    {"selector(div::before)",true}, {"selector([data-test=foo i])",true},
    {"(display:nonsense)",false}, {"(unknown:red)",false}, {"",false},
    {"color:red",false}, {"(color:red);",false}, {"(color:red;)",false},
    {"not",false}, {"not not (color:red)",false}, {"not (not (color:red) and)",false},
    {"not (color:red) and (width:1px)",false},
    {"(color:red) and (width:1px) or (height:1px)",false},
    {"(color:red) and",false}, {"(color:red) or malformed",false},
    {"not(color:red)",false}, {"(color:red)and(display:flex)",false},
    {"(color:red]",false}, {"(color:red) /*",false},
    {"(color:red !bad)",false}, {"future(unknown)",false},
    {"selector(div,span)",false}, {"selector(:has(div))",true},
    {"selector(:has(> div + span))",true}, {"selector(:has(+ .a .b))",true},
    {"selector(:has(:has(div)))",false}, {"selector(:has(div::before))",false},
    {"selector(:has(div,))",false}, {"selector(:has(:unknown))",false},
    {"selector(:hover)",true}, {"selector(:scope)",true}, {"selector(:defined)",false},
    {"selector(div::unknown)",false}, {"selector(:is(div,:unknown))",false},
    {"selector(:is(div,))",false}, {"selector(:nth-child(2n of .item))",false},
    {"selector(div >)",false}, {"selector(a|div)",false}, {"selector(**)",false},
    {"selector(123)",false}, {"selector(.123)",false},
};
static void rule_check(const char *css, float width, uint32_t color, const char *name) {
    char html[4096];
    snprintf(html,sizeof html,"<!doctype html><html><head><style>%s</style></head><body><div id=probe></div></body></html>",css);
    web_doc *d = web_parse(html,strlen(html),"http://css.test/index.html","utf-8");
    check(name,d != NULL,true);
    if (!d) return;
    css_cascade(d,800,600);
    pvec nodes = {0};
    bool selected = css_select(d,d->root,"#probe",&nodes);
    node_t *n = selected && nodes.n == 1 ? nodes.v[0] : NULL;
    check(name,n && n->style && len_resolve(&n->style->width,800) == width && n->style->color == color,true);
    pv_free(&nodes);
    web_free(d);
}
static web_doc *length_document(const char *value, const len_t **length) {
    char html[4096];
    snprintf(html,sizeof html,"<!doctype html><style>#probe{position:relative;left:%s}</style><div id=probe></div>",value);
    web_doc *d=web_parse(html,strlen(html),"https://css.test/length","utf-8");
    *length=NULL;
    if(!d)return NULL;
    css_cascade(d,800,600);
    pvec nodes={0};
    if(css_select(d,d->root,"#probe",&nodes)&&nodes.n==1){node_t*n=nodes.v[0];if(n->style)*length=&n->style->inset[3];}
    pv_free(&nodes);return d;
}
static void length_roundtrip(const char *value) {
    const len_t *a=NULL,*b=NULL;
    web_doc *first=length_document(value,&a);
    check("length real computed source",first&&a,true);
    sbuf out={0};bool serialized=a&&css_length_serialize(a,&out,first);
    check("length serialization owned and terminated",serialized&&out.p&&out.n&&out.p[out.n]==0,true);
    web_doc *second=serialized?length_document(out.p,&b):NULL;
    check("serialized calculation parsed by product",second&&b,true);
    const float bases[]={0,20,100,500};
    for(size_t i=0;i<sizeof bases/sizeof *bases;i++)
        check("computed calculation reference preserved",a&&b&&len_resolve(a,bases[i])==len_resolve(b,bases[i]),true);
    sb_free(&out);if(second)web_free(second);if(first)web_free(first);
}
static void test_length_serialization(void) {
    const struct {len_t value;const char *text;} cases[]={
        {{.kind=LK_AUTO},"auto"},{{.kind=LK_NONE},"none"},{{.kind=LK_NORMAL},"normal"},
        {{.kind=LK_NUMBER,.px=2.5f},"2.5"},{{.kind=LK_LEN},"0px"},
        {{.kind=LK_LEN,.px=12.5f},"12.5px"},{{.kind=LK_LEN,.pct=25},"25%"},
        {{.kind=LK_LEN,.px=-3,.pct=20},"calc(20% - 3px)"},
        {{.kind=LK_LEN,.px=3,.pct=-20},"calc(-20% + 3px)"}
    };
    for(size_t i=0;i<sizeof cases/sizeof *cases;i++) {
        sbuf out={0};check("length kind serialized",css_length_serialize(&cases[i].value,&out,NULL),true);
        check("length real computed text",out.p&&out.n==strlen(cases[i].text)&&!strcmp(out.p,cases[i].text),true);sb_free(&out);
    }
    sbuf held={0};len_t value={.kind=LK_AUTO};css_length_serialize(&value,&held,NULL);
    value.kind=255;check("unknown kind never fabricated zero",!css_length_serialize(&value,&held,NULL)&&held.n==4&&!strcmp(held.p,"auto"),true);
    value.kind=LK_EXPR;value.expr=NULL;check("invalid tree rollback",!css_length_serialize(&value,&held,NULL)&&held.n==4&&!strcmp(held.p,"auto"),true);
    value.kind=LK_NUMBER;value.pct=1;check("number percentage not fabricated",!css_length_serialize(&value,&held,NULL)&&held.n==4,true);sb_free(&held);
    sbuf overflow={.p=(char*)"held",.n=SIZE_MAX,.cap=SIZE_MAX};value.kind=LK_AUTO;
    check("serialization addition overflow rejected",!css_length_serialize(&value,&overflow,NULL)&&overflow.n==SIZE_MAX,true);
    const char *expressions[]={"auto","12.5px","25%","calc(20% - 3px)","calc(-20% - 3px)",
        "min(40px,50%)","max(5px,10%)","clamp(5px,10%,15px)",
        "calc(min(20%,30px) + max(2px,10%))","calc(min(20%,30px) * 2)","calc(max(12px,20%) / 2)"};
    for(size_t i=0;i<sizeof expressions/sizeof *expressions;i++)length_roundtrip(expressions[i]);
    char wide[2048];size_t n=0;n+=(size_t)snprintf(wide+n,sizeof wide-n,"min(");
    for(int i=0;i<80;i++)n+=(size_t)snprintf(wide+n,sizeof wide-n,"%s%d%%",i?",":"",i+1);
    snprintf(wide+n,sizeof wide-n,")");length_roundtrip(wide); /* grows beyond the local explicit stack */
    const len_t *cancelled_length=NULL;web_doc *cancelled=length_document(wide,&cancelled_length);
    check("cancellable native calculation available",cancelled&&cancelled_length,true);
    sbuf cancelled_out={0};
    if(cancelled)cancelled->native_cancelled=true;
    check("native cancellation rolls back calculation",cancelled_length&&!css_length_serialize(cancelled_length,&cancelled_out,cancelled)&&
        cancelled_out.n==0&&(!cancelled_out.p||!cancelled_out.p[0]),true);
    sb_free(&cancelled_out);if(cancelled)web_free(cancelled);
}
static web_doc *filter_document(const char *value, node_t **probe) {
    char html[4096];
    snprintf(html,sizeof html,"<!doctype html><body style=margin:0><div id=probe style='filter:%s'><div id=child></div></div>",value);
    web_doc *d=web_parse(html,strlen(html),"https://css.test/filter","utf-8");
    *probe=NULL;if(!d)return NULL;
    css_cascade(d,800,600);pvec nodes={0};
    if(css_select(d,d->root,"#probe",&nodes)&&nodes.n==1)*probe=nodes.v[0];
    pv_free(&nodes);return d;
}
static void test_filter_values(void) {
    const struct {const char *value,*text;float used;size_t count;} cases[]={
        {"none","none",1,0},{"opacity()","opacity(1)",1,1},
        {"opacity(.5)","opacity(0.5)",.5f,1},{"opacity(25%)","opacity(25%)",.25f,1},
        {"opacity(200%) opacity(2)","opacity(200%) opacity(2)",1,2},
        {"opacity(.5) opacity(50%)","opacity(0.5) opacity(50%)",.25f,2},
        {"opacity(0) opacity(4)","opacity(0) opacity(4)",0,2}
    };
    for(size_t i=0;i<sizeof cases/sizeof *cases;i++) {
        node_t *probe=NULL;web_doc *d=filter_document(cases[i].value,&probe);sbuf out={0};
        check("native filter computed source",d&&probe&&probe->style,true);
        const struct css_filter *f=probe&&probe->style?probe->style->filter:NULL;
        check("none versus identity filter",cases[i].count?f&&f->count==cases[i].count:!f,true);
        check("filter used step clamp/product",(f?f->opacity:1)==cases[i].used,true);
        check("filter real computed list serialization",probe&&css_filter_serialize(f,&out,d)&&out.p&&
            !strcmp(out.p,cases[i].text)&&out.p[out.n]==0,true);
        check("filter not inherited into descendants",probe&&probe->first&&probe->first->style&&!probe->first->style->filter,true);
        sb_free(&out);if(d)web_free(d);
    }
    node_t *probe=NULL;web_doc *d=filter_document("opacity(.5);filter:blur(1px)",&probe);sbuf out={0};
    check("invalid effect preserves valid cascade",probe&&probe->style&&probe->style->filter&&
        probe->style->filter->opacity==.5f,true);
    if(probe&&probe->style) {
        style_t same=*probe->style,none=same;none.filter=NULL;
        struct css_filter identity={.count=1,.opacity=1};same.filter=&identity;
        check("paint amount changes do not invalidate geometry",css_style_layout_equal(probe->style,&same),true);
        check("none changes containing-block geometry",!css_style_layout_equal(probe->style,&none),true);
    } else {check("paint amount changes do not invalidate geometry",false,true);check("none changes containing-block geometry",false,true);}
    sb_free(&out);if(d)web_free(d);
    char many[2048];size_t n=0;
    for(int i=0;i<96;i++)n+=(size_t)snprintf(many+n,sizeof many-n,"%sopacity(100%%)",i?" ":"");
    d=filter_document(many,&probe);
    check("dynamic filter list is not artificially truncated",probe&&probe->style&&probe->style->filter&&probe->style->filter->count==96,true);
    check("long filter list computed product",probe&&probe->style&&probe->style->filter&&probe->style->filter->opacity==1,true);
    len_t held={.kind=LK_AUTO};css_length_serialize(&held,&out,NULL);
    if(d)d->native_cancelled=true;
    check("filter cancellation restores caller prefix",probe&&probe->style&&
        !css_filter_serialize(probe->style->filter,&out,d)&&out.n==4&&!strcmp(out.p,"auto"),true);
    sb_free(&out);if(d)web_free(d);
}
static uint32_t filter_pixels[32*32];
static void filter_paint(web_doc *d, canvas_t *canvas) {
    gfx_init(canvas,filter_pixels,32,32,32);web_paint(d,canvas,0,0,32,32,0,0);
}
static bool filter_blue(unsigned channel) {
    uint32_t pixel=filter_pixels[5*32+5]&0xffffff;
    return (pixel&255)==255 && abs((int)((pixel>>8)&255)-(int)channel)<=1 && abs((int)(pixel>>16)-(int)channel)<=1;
}
static void test_filter_borrows(void) {
    node_t *probe=NULL;web_doc *d=filter_document("opacity(.5);display:inline-flex;width:20px;height:20px",&probe);
    check("filter derived borrower fixture",d&&probe&&probe->first,true);
    if(!d||!probe||!probe->first){if(d)web_free(d);return;}
    node_t *child=probe->first;
    doc_node_attr(d,d->body,"style","margin:0;font-size:0;line-height:0");
    doc_node_attr(d,child,"style","display:inline-block;vertical-align:top;width:20px;height:20px;background:blue");
    const struct {const char *style;unsigned channel;bool alpha,wrapped;} changes[]={
        {"display:inline-flex;width:20px;height:20px;filter:opacity(.5)",127,true,true},
        {"display:inline-flex;width:20px;height:20px;filter:opacity(.25)",191,true,true},
        {"display:inline;filter:opacity(.5)",127,true,false},
        {"display:inline;filter:none",0,false,false},
        {"display:inline;filter:opacity(.5)",127,true,false},
        {"display:inline-flex;width:20px;height:20px;filter:opacity(1)",0,false,true}
    };
    for(size_t i=0;i<sizeof changes/sizeof *changes;i++) {
        check("filter style mutation admitted",doc_node_attr(d,probe,"style",changes[i].style),true);
        web_layout(d,800,600);canvas_t canvas;filter_paint(d,&canvas);
        check("filter arena retirement/reflow keeps real group",filter_blue(changes[i].channel),true);
        check("real alpha aggregation refreshed by existing restyle",d->root_box&&d->root_box->paint_alpha==changes[i].alpha,true);
        check("derived inner group is not applied twice",!changes[i].wrapped||(probe->box&&probe->box->first&&
            !probe->box->first->st->filter&&probe->box->first->st->opacity==1),true);
    }
    web_free(d);
    const char *html="<!doctype html><style>@keyframes f{from{opacity:1}to{opacity:0}}</style>"
        "<body style='margin:0'><div id=probe style='width:20px;height:20px;background:blue;filter:opacity(1);animation:f 1s linear paused both'></div>";
    d=web_parse(html,strlen(html),"https://css.test/filter-motion","utf-8");web_layout(d,800,600);
    pvec nodes={0};probe=NULL;
    if(css_select(d,d->root,"#probe",&nodes)&&nodes.n==1)probe=nodes.v[0];pv_free(&nodes);
    box_t *old=d->root_box;
    check("identity animation does not snapshot",probe&&probe->style->opacity==1&&old&&!old->paint_alpha,true);
    if(probe)doc_node_attr(d,probe,"style","width:20px;height:20px;background:blue;filter:opacity(1);animation:f 1s linear -.5s paused both");
    web_layout(d,800,600);canvas_t canvas;filter_paint(d,&canvas);
    check("native opacity sample aggregates without rebuilding boxes",probe&&d->root_box==old&&d->root_box->paint_alpha&&probe->style->opacity==.5f,true);
    check("native animated opacity uses same real group",filter_blue(127),true);web_free(d);
}
struct filter_cancel {bool stop;};
static bool filter_checkpoint(void *opaque) {return !((struct filter_cancel*)opaque)->stop;}
static void test_filter_cancel(void) {
    struct filter_cancel state={0};struct web_host host={.opaque=&state,.script_checkpoint=filter_checkpoint};
    const char *html="<!doctype html><body style='margin:0'><div style='filter:opacity(.5)'>"
        "<a id=child style='display:block;width:20px;height:20px;background:blue'></a></div>";
    web_doc *d=web_live(html,strlen(html),"https://css.test/filter-cancel","utf-8",&host);
    check("filter live cancellation fixture",d!=NULL,true);if(!d)return;
    web_tick(d,uptime_ms());web_layout(d,800,600);canvas_t canvas;filter_paint(d,&canvas);
    uint32_t previous[32*32];memcpy(previous,filter_pixels,sizeof previous);
    pvec nodes={0};node_t *child=NULL;
    if(css_select(d,d->root,"#child",&nodes)&&nodes.n==1)child=nodes.v[0];pv_free(&nodes);
    check("filter cancellation has parsed real child",child!=NULL,true);
    if(!child){web_free(d);return;}
    doc_node_attr(d,child,"style","display:block;width:20px;height:20px;background:red");
    web_layout(d,800,600);state.stop=true;d->native_checkpoint_count=63;
    filter_paint(d,&canvas);
    check("filter blend respects native stop",d->native_cancelled&&d->native_recovery_pending,true);
    check("cancelled group/frame restores last coherent pixels",!memcmp(previous,filter_pixels,sizeof previous),true);
    check("cancelled group restores graphics clip",canvas.cx0==0&&canvas.cy0==0&&canvas.cx1==32&&canvas.cy1==32,true);
    state.stop=false;
    /* Recovery only starts a new native transaction. The actual browser's
       outer layout path rebuilds and commits it before pending/active can be
       cleared and the painter can safely overwrite the last bitmap. */
    web_layout(d,800,600);
    check("cancelled group retires through outer recovery",!d->native_cancelled&&!d->native_recovery_pending&&
          !d->native_recovery_active&&d->layout_valid&&d->root_box,true);
    filter_paint(d,&canvas);
    check("recovery draws current native DOM instead of old bitmap",(filter_pixels[5*32+5]&0xffffff)==0xff7f7f,true);
    web_free(d);
}
static void test_filter_layer_vectors(void) {
    canvas_t canvas;web_doc *d;
    char many[8192];size_t length=(size_t)snprintf(many,sizeof many,"<!doctype html><body style=margin:0><div style='filter:opacity(.5)'>");
    for(int i=0;i<40;i++)length+=(size_t)snprintf(many+length,sizeof many-length,
        "<span style='display:block;filter:opacity(1);width:20px;height:1px;background:blue'></span>");
    snprintf(many+length,sizeof many-length,"</div>");d=web_parse(many,strlen(many),"https://css.test/filter-layers","utf-8");
    web_layout(d,800,600);filter_paint(d,&canvas);size_t count=0;
    if(d->body&&d->body->first)for(node_t *n=d->body->first->first;n;n=n->next)count++;
    check("fallible layer vector grows across real filter contexts",count==40,true);
    check("grown filter layer vector preserves group composition",filter_blue(127),true);
    check("grown filter layer vector cleans up paint context",canvas.cx0==0&&canvas.cy0==0&&canvas.cx1==32&&canvas.cy1==32,true);
    web_free(d);
}
static void test_hover_support(void) {
    const char html[]="<!doctype html><style>html,body{margin:0}#parent{width:80px;height:40px}"
        "#leaf{width:80px;height:20px;color:#ff0000}@supports selector(:hover){#leaf:hover{color:#0000ff}}</style>"
        "<div id=parent><div id=leaf></div></div>";
    web_doc *d=web_live(html,sizeof html-1,"https://css.test/hover","utf-8",NULL);
    if(d)for(unsigned i=0;i<128&&!web_js_complete(d);i++)web_tick(d,uptime_ms());
    check("hover real live document",d&&web_js_complete(d),true);if(!d)return;
    web_layout(d,800,600);
    pvec nodes={0};node_t *leaf=NULL,*parent=NULL;
    if(css_select(d,d->root,"#leaf",&nodes)&&nodes.n==1)leaf=nodes.v[0];pv_free(&nodes);
    if(css_select(d,d->root,"#parent",&nodes)&&nodes.n==1)parent=nodes.v[0];pv_free(&nodes);
    check("hover real selector nodes",leaf&&parent,true);if(!leaf||!parent){web_free(d);return;}
    bool valid=false;
    check("hover initially unmatched",!css_matches(leaf,":hover",&valid)&&valid,true);
    check("hover initial cascade",leaf->style&&leaf->style->color==RGB(255,0,0),true);
    check("hover support while unmatched",css_supports_condition("selector(:hover)",16,false),true);
    node_t *hit=web_node_at(d,5,5);check("hover native pointer hit",hit==leaf,true);
    struct web_event event={.x=5,.y=5};web_hover(d,hit,&event);
    valid=false;check("hover pointer matches leaf",css_matches(leaf,":hover",&valid)&&valid,true);
    valid=false;check("hover flat ancestor matches",css_matches(parent,":hover",&valid)&&valid,true);
    web_layout(d,800,600);
    check("hover supports rule real cascade",leaf->style&&leaf->style->color==RGB(0,0,255),true);
    check("hover support while matched",css_supports_condition("selector(:hover)",16,false),true);
    web_hover(d,NULL,&event);valid=false;
    bool leaf_left=!css_matches(leaf,":hover",&valid)&&valid;valid=false;
    check("hover pointer exit unmatches leaf and ancestor",leaf_left&&!css_matches(parent,":hover",&valid)&&valid,true);
    web_layout(d,800,600);
    check("hover exit restores cascade",leaf->style&&leaf->style->color==RGB(255,0,0),true);
    check("hover support after exit",css_supports_condition("selector(:hover)",16,false),true);
    web_free(d);
}
int main(int argc,char **argv) {
    if(argc>1&&!strcmp(argv[1],"--filter-cancel")) {
        test_filter_cancel();
        printf("csssupportstest-filter-cancel: %d checks, %d failures\n",checks,failures);
        return failures != 0;
    }
    for (size_t i=0;i<sizeof declarations/sizeof *declarations;i++) {
        const struct decl_case *c=&declarations[i];
        check(c->value,css_supports_declaration(c->property,strlen(c->property),c->value,strlen(c->value)),c->expected);
    }
    for (size_t i=0;i<sizeof conditions/sizeof *conditions;i++) {
        const struct cond_case *c=&conditions[i];
        check(c->condition,css_supports_condition(c->condition,strlen(c->condition),false),c->expected);
    }
    check("api implied parentheses",css_supports_condition("color:red",9,true),true);
    check("api malformed negation",css_supports_condition("not (not (color:red) and)",25,true),false);
    const char embedded[]="(color:red)\0junk";
    check("condition embedded nul",css_supports_condition(embedded,sizeof embedded-1,true),false);
    check("value embedded nul",css_supports_declaration("color",5,"red\0junk",8),false);
    char deep[256],long_input[65538];
    memset(deep,'(',40);memcpy(deep+40,"color:red",9);memset(deep+49,')',40);deep[89]=0;
    check("deep valid condition",css_supports_condition(deep,89,true),true);
    memset(long_input,' ',sizeof long_input);memcpy(long_input,"(color:red)",11);
    check("long valid condition",css_supports_condition(long_input,sizeof long_input,true),true);
    rule_check("#probe{width:11px;color:red}@supports (not-a-property:x){#probe{width:99px}}"
               "@supports not (display:nonsense){#probe{width:21px;color:blue}}",21,RGB(0,0,255),"rules true false not");
    rule_check("#probe{width:11px;color:red}@supports (color:var(--test,red)){#probe{width:31px}}"
               "@supports (color:red) and (width:1px) or (height:1px){#probe{width:99px}}",31,RGB(255,0,0),"rules invalid logical mix");
    rule_check("#probe{width:11px;color:red}@supports selector(:has(div)){#probe{width:99px}}"
               "@supports selector(div>.item){#probe{width:41px}}",41,RGB(255,0,0),"rules selector capabilities");
    rule_check("#probe{width:11px;color:red;@supports not (display:nonsense){width:51px;}"
               "@supports (color:nonsense){width:99px;}}",51,RGB(255,0,0),"rules nested body");
    rule_check("#probe{--TEST:r\\65 d;color:VAR(--TEST);w\\69 dth:61px}",61,RGB(255,0,0),"real cascade escape var");
    test_length_serialization();
    test_filter_values();
    test_filter_borrows();
    test_filter_cancel();
    test_filter_layer_vectors();
    test_hover_support();
    printf("csssupportstest: %d checks, %d failures\n",checks,failures);
    return failures != 0;
}
