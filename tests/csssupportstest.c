/* Shared real CSS parser/cascade contracts. QEMU/native results are distinct
   from website interaction and the standalone host extraction probe. */
#include <stdio.h>
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
    {"selector(div,span)",false}, {"selector(:has(div))",false},
    {"selector(:hover)",false}, {"selector(:scope)",false}, {"selector(:defined)",false},
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
int main(void) {
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
    printf("csssupportstest: %d checks, %d failures\n",checks,failures);
    return failures != 0;
}
