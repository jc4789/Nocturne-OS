/* Native selector/cascade/mutation checks, not a substitute for real-site UI. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <nocturne.h>
#include "webi.h"
#include "cssom.h"

static int checks, failed;
static void check(bool value, const char *name) { checks++; if (!value) { failed++; printf("FAIL cssrelational %s\n", name); } }
static bool checkpoint(void *context) { (void)context; return true; }
static bool initialize_counted(web_doc *doc,bool count) {
    /* web_live queues the incremental parser. Exercise the same tick path as
       the browser before querying the DOM; layout is not a parser driver. */
    uint64_t start = uptime_ms();
    while (doc->parser && !doc->native_cancelled && uptime_ms() - start < 5000) web_tick(doc, uptime_ms());
    bool ready = !doc->parser && !doc->native_cancelled && doc->html && doc->body;
    if(count)check(ready, "normal product ticks finish initial parsing"); return ready;
}
static bool initialize(web_doc *doc) { return initialize_counted(doc,true); }
static node_t *id(web_doc *doc, const char *name) {
    pvec values = {0}; sbuf selector = {0}; sb_putc(&selector, '#'); sb_puts(&selector, name);
    css_select(doc, doc->root, sb_cstr(&selector), &values);
    node_t *node = values.n ? values.v[0] : NULL; pv_free(&values); sb_free(&selector); return node;
}
static void selected(web_doc *doc, node_t *scope, const char *selector, const char *expected) {
    pvec values = {0}; sbuf actual = {0}; bool valid = css_select(doc, scope, selector, &values);
    for (int i = 0; i < values.n; i++) { if (i) sb_putc(&actual, ','); sb_puts(&actual, ((node_t *)values.v[i])->id); }
    check(valid && !strcmp(sb_cstr(&actual), expected), selector); pv_free(&values); sb_free(&actual);
}
static void matches(node_t *node, const char *selector, bool expected, bool expected_valid) {
    bool valid = false; bool actual = css_matches(node, selector, &valid);
    check(valid == expected_valid && actual == expected, selector);
}
#include "cssmediacalc_cases.h"
static int media_cascade(void) {
    media_query_cases();
    const char *html = "<!doctype html><html><head><style>:root{font-size:30px}"
        ".content{flex-wrap:wrap;flex:100%;max-width:100%;display:flex}"
        "@media (width<=calc(48rem - .02px)){.content{flex-direction:column}}"
        "</style></head><body><div id='target' class='content'><div>a</div><div>b</div></div></body></html>";
    struct web_host callbacks = {.script_checkpoint = checkpoint};
    web_doc *doc = web_live(html, strlen(html), "https://media.test/", "utf-8", &callbacks);
    check(doc != NULL, "media cascade document"); if (!doc) return 1;
    if (!initialize(doc)) { web_free(doc); return 1; }
    web_layout(doc, 1908, 600); node_t *target = id(doc, "target");
    check(target && target->style && target->style->display == D_FLEX && target->style->flex_direction == FD_ROW,
          "actual original condition keeps 1908 flex row");
    web_layout(doc, 767, 600);
    check(target && target->style && target->style->display == D_FLEX && target->style->flex_direction == FD_COLUMN,
          "actual original condition switches 767 flex column");
    web_layout(doc, 1908, 600);
    check(target && target->style && target->style->display == D_FLEX && target->style->flex_direction == FD_ROW,
          "resize to 1908 restores row from cached stylesheet");
    web_free(doc); printf("cssrelational-media-calc: %d checks, %d failed\n", checks, failed); return failed != 0;
}
static node_t *element(web_doc *doc, node_t *parent, const char *tag, const char *classes) {
    node_t *node = doc_node_create(doc, N_ELEM, tag, NULL, 0);
    if (node && classes) doc_node_attr(doc, node, "class", classes);
    if (node && parent) doc_node_move(doc, parent, node, NULL); return node;
}
static int boundaries(void) {
    const char *html = "<!doctype html><html><head></head><body><section id='other'>other</section></body></html>";
    struct web_host callbacks = {.script_checkpoint = checkpoint};
    web_doc *doc = web_live(html, strlen(html), "https://selector.test/", "utf-8", &callbacks);
    check(doc != NULL, "boundary document"); if (!doc) return 1;
    if (!initialize(doc)) { web_free(doc); return 1; }
    node_t *host = element(doc, doc->body, "div", "host"), *light = element(doc, host, "span", "light");
    node_t *shadow = doc_shadow_attach(doc, host, false, false, false, false, false);
    check(host && light && shadow, "boundary shadow nodes");
    if (!host || !light || !shadow) { web_free(doc); return 1; }
    element(doc, shadow, "div", "shadow-only"); element(doc, shadow, "slot", NULL);
    int error = 0; struct cssom_sheet *sheet = cssom_construct(doc, &error);
    const char *css = ":host{color:red}:host(:has(>.light)){color:blue}";
    check(sheet && cssom_replace_sync(sheet, css, strlen(css)) == CSSOM_OK && cssom_adopt(shadow, &sheet, 1) == CSSOM_OK,
        "boundary constructed stylesheet uses actual host cascade");
    web_layout(doc, 1000, 600); node_t *other = id(doc, "other");
    check(other && other->style, "boundary existing unrelated target has computed style");
    if (!other || !other->style) { web_free(doc); return 1; }
    style_t *untouched = other->style;
    matches(host, ":has(.shadow-only)", false, true);
    matches(host, ":has(>.light)", true, true);
    matches(light, ":has(+.shadow-only)", false, true);
    check(host->style && host->style->color == 0xff0000ffu, "host argument sees light DOM witness");
    doc_node_attr(doc, light, "class", "gone"); web_layout(doc, 1000, 600);
    check(host->style && host->style->color == 0xffff0000u, "light witness mutation invalidates host's own shadow sheet");
    check(other->style == untouched, "host relational change preserves unrelated subtree");
    node_t *detached = element(doc, NULL, "div", NULL), *child = element(doc, detached, "span", "child");
    node_t *next = element(doc, detached, "span", "next"), *later = element(doc, detached, "span", "later");
    check(detached && child && next && later, "boundary detached DOM");
    matches(detached, ":has(>.child)", true, true); matches(child, ":has(+.next)", true, true);
    matches(child, ":has(+.later)", false, true); matches(child, ":has(~.later)", true, true);
    matches(detached, ":has(~*)", false, true); matches(detached, ":has(:scope)", false, true);
    web_free(doc); printf("cssrelational-boundaries: %d checks, %d failed\n", checks, failed); return failed != 0;
}
static int layer_cascade(void) {
    const char *html="<!doctype html><style>html{font-size:16px}"
        "@layer foundation,components,tail;"
        "@layer foundation{h2{font-size:24px}#normal{color:red}#important{color:green!important}}"
        "@layer components{:where(.heading):where([data-variant=small]){font:var(--title,600 1rem/1.5 sans-serif)}"
        ".case{color:green}#important{color:red!important}#revert{color:red;color:revert-layer}"
        "#custom{--selected:red;--selected:revert-layer}#all{color:red;font-size:30px;all:revert-layer}}"
        "@layer foundation{#revert,#all{color:green;font-size:16px}#custom{--selected:green}}"
        "#custom{color:var(--selected)}@layer tail{#important{color:red!important}}"
        "@layer nested{#nested{color:green}@layer child{#nested{color:red}#nested-important{color:green!important}}"
        "#nested-important{color:red!important}}"
        "@layer { @layer child{#anonymous{color:red}}}@layer { @layer child{#anonymous{color:green}}}"
        "@layer components{#unlayered{color:red}#inline{color:red!important}}"
        "#unlayered{color:green}#inline{color:red!important}"
        "@layer foundation{#important-revert{color:green}}"
        "@layer components{#important-revert{color:red!important;color:revert-layer!important}}"
        "@layer tail{#important-revert{color:red!important}}"
        "@media (max-width:1px){@layer conditional-second;}@layer conditional-first{#conditional{color:red}}"
        "@layer conditional-second{#conditional{color:green}}"
        "</style><style>@layer foundation{#normal{color:red}}</style>"
        "<h2 class=heading data-variant=small id=heading>Heading</h2>"
        "<div class=case id=normal></div><div id=important></div><div id=revert></div>"
        "<div id=custom></div><div id=all></div><div id=nested></div><div id=nested-important></div>"
        "<div id=anonymous></div><div id=unlayered></div><div id=inline style='color:green!important'></div>"
        "<div id=important-revert></div><div id=conditional></div>";
    struct web_host host={.script_checkpoint=checkpoint};web_doc *doc=web_live(html,strlen(html),"https://layers.test/","utf-8",&host);
    if(!doc)return 1;if(!initialize_counted(doc,false)){web_free(doc);return 1;}web_layout(doc,1920,1080);
    node_t *heading=id(doc,"heading");
    check(heading&&heading->style->font_size==16&&heading->style->font_weight==600,
          "later layer font shorthand beats earlier type selector through where and var fallback");
    static const char *const ids[]={"normal","important","revert","custom","all","nested","nested-important","anonymous","unlayered","inline","important-revert","conditional"};
    for(size_t i=0;i<sizeof ids/sizeof *ids;i++){node_t *n=id(doc,ids[i]);check(n&&n->style->color==0xff008000u,ids[i]);}
    node_t *all=id(doc,"all");check(all&&all->style->font_size==16,"all revert-layer rolls back font longhand in font pass");
    web_free(doc);printf("cssrelational-layers: %d checks, %d failed\n",checks,failed);return failed!=0;
}
static int layer_dynamic(void) {
    const char *html="<!doctype html><style id=sheet>@layer a{#target{color:red}}@layer b{#target{color:green}}</style>"
        "<style id=imports>@layer before,after;@import url(data:text/css,);</style>"
        "<div id=target></div><div id=host1></div><div id=host2></div>";
    struct web_host host={.script_checkpoint=checkpoint};web_doc *d=web_live(html,strlen(html),"https://layer-dynamic.test/","utf-8",&host);
    if(!d)return 1;if(!initialize_counted(d,false)){web_free(d);return 1;}web_layout(d,800,600);
    int error=0;struct cssom_sheet *sheet=cssom_style_sheet(d,id(d,"sheet"),&error);
    if(!sheet||error){web_free(d);return 1;}
    const char *statement="@layer b,a";
    check(cssom_insert(sheet,statement,strlen(statement),0)==CSSOM_OK,"layer statement without final semicolon inserts through real CSSOM AST");
    web_layout(d,800,600);node_t *target=id(d,"target");
    check(target&&target->style->color==0xffff0000u,"inserted first statement changes layer encounter priority");
    check(cssom_delete(sheet,0)==CSSOM_OK,"layer statement deletion reparses existing AST");
    web_layout(d,800,600);check(target&&target->style->color==0xff008000u,"deleting prelude restores original source encounter priority");
    const char *block="@layer a{#target{color:blue}}";
    check(cssom_insert(sheet,block,strlen(block),2)==CSSOM_OK,"layer block inserts through existing native CSSOM");
    check(cssom_insert(sheet,"@layer 123{}",12,0)==CSSOM_SYNTAX,"invalid layer ident is rejected without publication");
    sheet=cssom_style_sheet(d,id(d,"imports"),&error);if(!sheet||error){web_free(d);return 1;}
    check(cssom_insert(sheet,"@layer more",11,0)==CSSOM_OK,"empty layer statement may precede imports");
    check(cssom_insert(sheet,block,strlen(block),0)==CSSOM_HIERARCHY,"layer block cannot be inserted before imports");
    node_t *one=doc_shadow_attach(d,id(d,"host1"),false,false,false,false,false),
        *two=doc_shadow_attach(d,id(d,"host2"),false,false,false,false,false);
    if(!one||!two){web_free(d);return 1;}node_t *p1=element(d,one,"p",NULL),*p2=element(d,two,"p",NULL);
    struct cssom_sheet *shared=cssom_construct(d,&error),*order1=cssom_construct(d,&error),*order2=cssom_construct(d,&error);
    if(!shared||!order1||!order2||!p1||!p2){web_free(d);return 1;}
    const char *rules="@layer alpha{p{color:red}}@layer beta{p{color:green}}";
    if(cssom_replace_sync(shared,rules,strlen(rules)) || cssom_replace_sync(order1,"@layer alpha,beta;",18) ||
       cssom_replace_sync(order2,"@layer beta,alpha;",18)){web_free(d);return 1;}
    struct cssom_sheet *a[]={order1,shared},*b[]={order2,shared};
    if(cssom_adopt(one,a,2)||cssom_adopt(two,b,2)){web_free(d);return 1;}web_layout(d,800,600);
    check(p1->style&&p2->style&&p1->style->color==0xff008000u&&p2->style->color==0xffff0000u,
        "same adopted AST resolves layer order independently in two shadow scopes");
    const char *changed="@layer alpha{p{color:blue}}@layer beta{p{color:green}}";
    check(cssom_replace_sync(shared,changed,strlen(changed))==CSSOM_OK,"shared constructed layer AST replacement succeeds");
    web_layout(d,800,600);
    check(p1->style&&p2->style&&p1->style->color==0xff008000u&&p2->style->color==0xff0000ffu,
        "shared replacement invalidates both scopes without leaking one layer order into the other");
    web_free(d);printf("cssrelational-layer-dynamic: %d checks, %d failed\n",checks,failed);return failed!=0;
}
static int child_list_dirty(bool locality_only) {
    sbuf html={0};sb_puts(&html,"<!doctype html><style>"
        ".left+.right{color:blue}#box>i:first-child{color:red}#box>i+i{color:blue}"
        ".empty:empty+.after{height:31px}.empty:not(:empty)+.after{height:37px}"
        ".witness:has(>.flag)+.effect{width:43px}"
        "</style><main><div id=box><i id=first>A</i></div><section id=unrelated>");
    for(unsigned i=0;i<200;i++)sb_puts(&html,"<p>Unrelated style forest</p>");
    sb_puts(&html,"</section><div class=empty id=empty></div><div class=after id=after></div>"
        "<div class=witness id=witness></div><div class=effect id=effect></div></main>");
    struct web_host host={.script_checkpoint=checkpoint};
    web_doc *doc=web_live(html.p,html.n,"https://child-list.test/","utf-8",&host);sb_free(&html);
    if(!locality_only)check(doc!=NULL,"child-list live document");if(!doc)return 1;
    if(!initialize_counted(doc,!locality_only)){web_free(doc);return 1;}
    web_layout(doc,1000,600);doc->profile_enabled=true;
    node_t *box=id(doc,"box"),*unrelated=id(doc,"unrelated"),*first=id(doc,"first");
    node_t *empty=id(doc,"empty"),*after=id(doc,"after"),*witness=id(doc,"witness"),*effect=id(doc,"effect");
    if(!locality_only)check(box&&unrelated&&first&&empty&&after&&witness&&effect,"child-list native targets");
    if(!box||!unrelated||!first||!empty||!after||!witness||!effect){web_free(doc);return 1;}
    style_t *held=unrelated->style,*held_child=unrelated->first->style;
    uint64_t before=doc->profile.style_visits;
    node_t *second=element(doc,box,"i",NULL);web_layout(doc,1000,600);
    if(!locality_only)check(second&&first->style->color==0xffff0000u&&second->style->color==0xff0000ffu,
          "child-list positional and immediate sibling styles recompute");
    printf("child-list locality: visits=%lu unrelated=%d child=%d second=%d\n",
        (unsigned long)(doc->profile.style_visits-before),unrelated->style==held,
        unrelated->first->style==held_child,second!=NULL);
    check(unrelated->style==held&&unrelated->first->style==held_child&&doc->profile.style_visits-before<20,
          "global sibling selector does not restyle unrelated grandparent forest");
    if(locality_only){web_free(doc);printf("cssrelational-child-locality: %d checks, %d failed\n",checks,failed);return failed!=0;}
    check(after->style->height.kind==LK_LEN&&after->style->height.px==31,"empty outer sibling starts matched");
    node_t *child=element(doc,empty,"span",NULL);web_layout(doc,1000,600);
    check(child&&after->style->height.px==37,"parent empty state updates its outer sibling after insertion");
    if(child)doc_node_remove(doc,child);web_layout(doc,1000,600);
    check(after->style->height.px==31,"removed empty witness updates old outer sibling match");
    node_t *flag=element(doc,witness,"span","flag");web_layout(doc,1000,600);
    check(flag&&effect->style->width.kind==LK_LEN&&effect->style->width.px==43,"has parent witness still invalidates outer sibling");
    if(flag)doc_node_remove(doc,flag);web_layout(doc,1000,600);
    check(effect->style->width.kind!=LK_LEN||effect->style->width.px!=43,"removed has witness still clears outer sibling style");
    web_free(doc);printf("cssrelational-child-list: %d checks, %d failed\n",checks,failed);return failed!=0;
}

int main(int argc, char **argv) {
    if(argc>1&&!strcmp(argv[1],"--child-list"))return child_list_dirty(false);
    if(argc>1&&!strcmp(argv[1],"--child-list-locality"))return child_list_dirty(true);
    if(argc>1&&!strcmp(argv[1],"--layers"))return layer_cascade();
    if(argc>1&&!strcmp(argv[1],"--layers-dynamic"))return layer_dynamic();
    if (argc > 1 && !strcmp(argv[1], "--media-calc")) return media_cascade();
    if (argc > 1 && !strcmp(argv[1], "--boundaries")) return boundaries();
    const char *html = "<!doctype html><html><head><style>"
        ".subject{color:red}.subject:has(>.witness){color:blue}"
        ".sibling:has(+.following .flag){background-color:lime}"
        "@supports selector(:has(>.witness)){.conditional{height:23px}}"
        "</style></head><body><main id='main'>"
        "<div id='a' class='subject x conditional'><span id='w' class='witness a'></span>"
        "<span id='b' class='b'></span><div id='nest'><span id='deep' class='b'></span></div></div>"
        "<div id='s' class='sibling x'></div><div id='f' class='following a'>"
        "<div id='g'><span id='flag' class='flag b'></span></div></div>"
        "<div id='tail' class='b'></div></main><section id='unrelated'>unrelated</section></body></html>";
    struct web_host host = {.script_checkpoint = checkpoint};
    web_doc *doc = web_live(html, strlen(html), "https://selector.test/", "utf-8", &host);
    check(doc != NULL, "document"); if (!doc) return 1;
    if (!initialize(doc)) { web_free(doc); return 1; }
    web_layout(doc, 1000, 600); node_t *main = id(doc, "main"), *a = id(doc, "a"), *w = id(doc, "w");
    node_t *s = id(doc, "s"), *flag = id(doc, "flag"), *unrelated = id(doc, "unrelated");
    check(main && a && w && s && flag && unrelated, "native targets");
    if (!main || !a || !w || !s || !flag || !unrelated) { web_free(doc); return 1; }
    check(a->style && s->style && unrelated->style, "native targets have committed computed styles");
    if (!a->style || !s->style || !unrelated->style) { web_free(doc); return 1; }
    selected(doc, main, ".x:has(.a)", "a");
    selected(doc, main, ".x:has(>.a)", "a");
    selected(doc, main, ".x:has(.a+.b)", "a");
    selected(doc, main, ".x:has(.a~.b)", "a");
    selected(doc, main, ".x:has(+.a)", "s");
    selected(doc, main, ".x:has(+.a .b)", "s");
    selected(doc, main, ".x:has(+.a>.b)", "");
    selected(doc, main, ".x:has(+.a+.b)", "s");
    selected(doc, main, ".x:has(~.a)", "a,s");
    selected(doc, main, ".x:has(~.a .b)", "a,s");
    selected(doc, main, ".x:has(~.a+.b)", "a,s");
    selected(doc, main, ".x:has(.b):not(:has(>.flag))", "a");
    selected(doc, main, ":scope>.x", "a,s");
    selected(doc, main, ":has(:scope)", "");
    selected(doc, main, ".x:has(:is(:scope .b))", "a");
    matches(a, ":scope", true, true);
    matches(a, ":has(:scope)", false, true);
    matches(a, ":has(>.witness)", true, true);
    matches(a, ":has(>.witness, >.missing)", true, true);
    matches(a, ":has(.a:has(.b))", false, false);
    matches(a, ":has(.a::before)", false, false);
    matches(a, ":has(.a,)", false, false);
    matches(a, ":has(:is(:has(*)))", false, true);
    matches(a, ":has(:is(:has(*), span))", true, true);
    check(a->style->color == 0xff0000ffu && a->box && a->box->h == 23, "has selector and supports actual cascade");
    check(s->style->bg_color == 0xff00ff00u, "sibling descendant cascade");
    style_t *untouched = unrelated->style;
    doc_node_attr(doc, w, "class", "a"); web_layout(doc, 1000, 600);
    check(a->style->color == 0xffff0000u, "attribute removal invalidates old ancestor match");
    check(unrelated->style == untouched, "unrelated subtree styles retained on descendant attribute mutation");
    doc_node_attr(doc, flag, "class", "b"); web_layout(doc, 1000, 600);
    check(s->style->bg_color == 0, "following descendant removal invalidates preceding sibling match");
    check(unrelated->style == untouched, "unrelated subtree retained on following witness mutation");
    doc_node_attr(doc, w, "class", "witness a"); web_layout(doc, 1000, 600);
    check(a->style->color == 0xff0000ffu, "witness restoration recascades");
    doc_node_move(doc, main, w, NULL); web_layout(doc, 1000, 600);
    check(a->style->color == 0xffff0000u, "witness reparent invalidates old parent");
    matches(w, ":has(+*)", false, true);
    /* Dynamic lists/compounds do not silently discard valid tail selectors. */
    sbuf large = {0}; for (int i = 0; i < 90; i++) { if (i) sb_putc(&large, ','); sb_puts(&large, ".absent"); }
    sb_puts(&large, ",#a"); selected(doc, main, sb_cstr(&large), "a"); sb_free(&large);
    for (int i = 0; i < 80; i++) sb_puts(&large, ".x"); sb_puts(&large, ":has(.b)");
    matches(a, sb_cstr(&large), true, true); sb_free(&large);
    web_free(doc); printf("cssrelational: %d checks, %d failed\n", checks, failed); return failed != 0;
}
