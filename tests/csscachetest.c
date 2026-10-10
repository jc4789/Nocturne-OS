/* Completed-resource AST lifetime, not visible-site acceptance. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <nocturne.h>
#include <parallel.h>
#include "webi.h"
#include "cssom.h"

static int checks,failed;
static void check(bool ok,const char *name){checks++;if(!ok){failed++;printf("FAIL csscache %s\n",name);}}
static bool checkpoint(void *context){(void)context;return true;}
static bool near(float a,float b){return fabsf(a-b)<0.1f;}
static node_t *element(web_doc *d,const char *tag,node_t *parent,const char *classes){
    node_t *n=doc_node_create(d,N_ELEM,tag,NULL,0);
    if(n&&classes)doc_node_attr(d,n,"class",classes);
    if(n&&parent)doc_node_move(d,parent,n,NULL);
    return n;
}
static node_t *link_sheet(web_doc *d,node_t *parent,const char *url){
    node_t *n=element(d,"link",parent,NULL);
    if(n){doc_node_attr(d,n,"rel","stylesheet");doc_node_attr(d,n,"href",url);}
    return n;
}
static char *large_css(const char *rules){
    const size_t length=40000;
    size_t n=strlen(rules);if(n>length-4)return NULL;
    char *css=malloc(length+1);if(!css)return NULL;
    memcpy(css,rules,n);css[n++]='/';css[n++]='*';
    memset(css+n,'x',length-n-2);css[length-2]='*';css[length-1]='/';css[length]=0;
    return css;
}
static uint64_t parsed_bytes(void){struct parallel_stats s={0};parallel_get_stats(PARALLEL_CSS_PARSE,&s);return s.work;}
static void rescan(web_doc *d,int width){doc_mutated(d,NULL);web_layout(d,width,600);}

int main(void){
    const char *html="<!doctype html><html><head></head><body></body></html>";
    struct web_host host={.script_checkpoint=checkpoint};
    web_doc *d=web_live(html,strlen(html),"https://cache.test/page/","utf-8",&host);
    check(d!=NULL,"document");if(!d)goto done;
    d->profile_enabled=true;parallel_profile_enable(true);
    for(int i=0;i<8;i++)web_tick(d,uptime_ms());
    const char *one_url="https://cache.test/assets/one.css",*two_url="https://cache.test/assets/two.css";
    char *one_css=large_css(".value{width:40px;height:10px;color:red}"
        "@media(min-width:900px){.value{color:blue}}");
    char *two_css=large_css(".value{height:20px}");
    check(one_css&&two_css,"large native parser resources");
    if(!one_css||!two_css){free(one_css);free(two_css);web_free(d);goto done;}
    node_t *one=link_sheet(d,d->head,one_url),*value=element(d,"div",d->body,"value");
    check(one&&value,"connected stylesheet and target");
    doc_css_loaded(d,one_url,one_url,one_css,40000);
    uint64_t before=parsed_bytes();web_layout(d,800,600);
    check(parsed_bytes()-before==40000,"completed external body parses exactly once");
    check(value&&value->box&&near(value->box->w,40)&&near(value->box->h,10)&&value->style->color==0xffff0000u,
        "initial cached native AST cascades");
    before=parsed_bytes();rescan(d,800);
    check(parsed_bytes()==before,"second complete resource scan does not reparse cached body");
    node_t *ordinary=element(d,"span",d->body,NULL);
    node_t *text=doc_node_create(d,N_TEXT,NULL,"ordinary body",13);
    if(ordinary&&text)doc_node_move(d,ordinary,text,NULL);
    rescan(d,800);
    check(parsed_bytes()==before&&value->box&&near(value->box->w,40),
        "body mutation plus conservative rescan retains completed AST");
    node_t *two=link_sheet(d,d->head,two_url);doc_css_loaded(d,two_url,two_url,two_css,40000);
    before=parsed_bytes();web_layout(d,800,600);
    check(parsed_bytes()-before==40000,"only newly completed sheet parses");
    check(two&&value->box&&near(value->box->h,20),"new sheet source order is retained");
    before=parsed_bytes();web_layout(d,1000,600);
    check(parsed_bytes()==before&&value->style->color==0xff0000ffu,"viewport media flip reuses external AST");
    doc_node_attr(d,two,"media","(min-width:1100px)");web_layout(d,1000,600);
    check(parsed_bytes()==before&&value->box&&near(value->box->h,10),"link media wrapper is not cached in shared AST");
    node_t *shadow_host=element(d,"div",d->body,NULL);
    node_t *shadow=doc_shadow_attach(d,shadow_host,false,false,false,false,false);
    node_t *shadow_link=link_sheet(d,shadow,one_url),*shadow_value=element(d,"div",shadow,"value");
    web_layout(d,1000,600);
    check(shadow_link&&shadow_value&&parsed_bytes()==before&&shadow_value->box&&near(shadow_value->box->w,40),
        "same completed body has independent shadow scope without reparse");
    int error=0;struct cssom_sheet *editable=cssom_style_sheet(d,one,&error);
    check(editable&&error==CSSOM_OK,"link CSSOM promotion has independent editable AST");
    const char *edit=".value{width:99px}";
    check(editable&&cssom_insert(editable,edit,strlen(edit),2)==CSSOM_OK,"CSSOM mutation");
    web_layout(d,1000,600);
    check(value->box&&near(value->box->w,99)&&shadow_value->box&&near(shadow_value->box->w,40),
        "edited CSSOM AST wins for its owner without altering cached shared source");
    before=parsed_bytes();rescan(d,1000);
    check(parsed_bytes()==before&&value->box&&near(value->box->w,99),"rescan preserves CSSOM edit without reparsing either AST");
    const char *parent_url="https://cache.test/nested/parent.css",*child_url="https://cache.test/nested/child.css";
    char *parent_css=large_css("@import 'child.css';.value{width:80px}"),
         *child_css=large_css("@import 'parent.css';.value{width:70px;color:orange}");
    check(parent_css&&child_css,"import native resources");
    if(parent_css&&child_css){
        node_t *parent=link_sheet(d,d->head,parent_url);
        doc_css_loaded(d,parent_url,parent_url,parent_css,40000);
        doc_css_loaded(d,child_url,child_url,child_css,40000);
        before=parsed_bytes();web_layout(d,1000,600);
        check(parsed_bytes()-before==80000,"parent and imported completed bodies each parse once");
        check(parent&&value->box&&near(value->box->w,80)&&value->style->color==0xffffa500u,
            "cached import metadata restores child-before-parent order");
        check(d->pending_css.n==0,"cached import cycle retains URL ancestor rejection");
        before=parsed_bytes();rescan(d,1000);
        check(parsed_bytes()==before&&value->box&&near(value->box->w,80),"repeated import traversal does not reparse cached ASTs");
        doc_node_attr(d,parent,"media","(min-width:1100px)");web_layout(d,1000,600);
        check(parsed_bytes()==before&&value->box&&near(value->box->w,99)&&value->style->color==0xff0000ffu,
            "inactive root media applies to cached imported sheet too");
        web_layout(d,1200,600);
        check(parsed_bytes()==before&&value->box&&near(value->box->w,80)&&near(value->box->h,20),
            "viewport reactivates import and independent link media without parse");
    }
    free(parent_css);free(child_css);free(one_css);free(two_css);
    web_free(d);
done:
    printf("csscache: %d checks, %d failures\n",checks,failed);
    return failed?1:0;
}
