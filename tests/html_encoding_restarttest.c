/* Real native decoder/token/tree path. Guest regression, not site acceptance. */
#include "webi.h"
#include "html_policy.h"
#include <stdio.h>
#define BASE "https://encoding.test/dir/page"
static unsigned checks,failures;
static void check(bool ok,const char *name){checks++;if(!ok){failures++;printf("FAIL encoding restart %s\n",name);}}
static node_t *by_id(node_t *n,const char *id){
    if(!n)return NULL;if(n->id&&!strcmp(n->id,id))return n;
    for(node_t *c=n->first;c;c=c->next){node_t *found=by_id(c,id);if(found)return found;}return NULL;
}
static bool text_is(node_t *n,const char *expected){sbuf text={0};if(n)node_text_content(n,&text);bool same=text.n==strlen(expected)&&!memcmp(text.p?text.p:"",expected,text.n);sb_free(&text);return same;}
static unsigned policies(web_doc *d){unsigned n=0;for(struct web_html_policy_rule *r=d->html_policy?d->html_policy->first:NULL;r;r=r->next)n++;return n;}
static char *source(const char *meta,const char *body,size_t *length){
    sbuf text={0};sb_puts(&text,"<!doctype html><html><head><!--");for(unsigned i=0;i<1100;i++)sb_putc(&text,'x');
    sb_puts(&text,"-->");sb_puts(&text,meta);sb_puts(&text,"</head><body><p id=target>");sb_puts(&text,body);sb_puts(&text,"</p></body></html>");
    *length=text.n;return sb_cstr(&text);
}
static void parse_case(const char *name,const char *meta,const char *body,const char *transport,const char *encoding,const char *expected,bool string_input){
    web_doc *d=web_parse("",0,BASE,NULL);check(d!=NULL,name);if(!d)return;
    size_t length;char *bytes=source(meta,body,&length);
    struct html_parser *p=string_input?html_begin_string(d,bytes,length,false,false):html_begin(d,bytes,length,transport,false);free(bytes);
    check(p!=NULL,"parser starts");if(!p){web_free(d);return;}node_t *identity=d->root,*ignored=NULL;
    check(html_resume(p,&ignored)==0,"parser completes");check(d->root==identity,"Document identity retained");
    check(d->encoding_certain,"accepted encoding confidence");check(!strcasecmp(d->encoding,encoding),"canonical encoding");
    check(text_is(by_id(d->root,"target"),expected),name);html_finish(p);web_free(d);
}
int main(void){
    parse_case("late windows1251 restores Cyrillic","<meta charset=windows-1251>","\xcf\xf0\xe8\xe2\xe5\xf2",NULL,"windows-1251","\xd0\x9f\xd1\x80\xd0\xb8\xd0\xb2\xd0\xb5\xd1\x82",false);
    parse_case("late pragma charset", "<meta content='text/html; charset=windows-1251' http-equiv=content-type>","\xcf\xf0",NULL,"windows-1251","\xd0\x9f\xd1\x80",false);
    parse_case("transport certain ignores late conflicting meta","<meta charset=windows-1251>","\xc3\xa9","utf-8","UTF-8","\xc3\xa9",false);
    parse_case("same encoding becomes certain without reparse","<meta charset=windows-1252>","\xe9",NULL,"windows-1252","\xc3\xa9",false);
    parse_case("UTF16 declaration normalizes UTF8","<meta charset=utf-16le>","\xc3\xa9",NULL,"UTF-8","\xc3\xa9",false);
    parse_case("x-user-defined declaration normalizes1252","<meta charset=x-user-defined>","\xe9",NULL,"windows-1252","\xc3\xa9",false);
    parse_case("DOMString confidence never reparses","<meta charset=windows-1251>","\xc3\xa9",NULL,"UTF-8","\xc3\xa9",true);
    web_doc *d=web_parse("",0,BASE,NULL);check(d!=NULL,"policy document");
    if(d){
        check(html_policy_init(d,"Content-Security-Policy: trusted-types initial\r\n",NULL),"initial policy");
        size_t length;char *bytes=source("<meta http-equiv=Content-Security-Policy content='trusted-types meta'><meta charset=windows-1251>","\xcf",&length);
        struct html_parser *p=html_begin(d,bytes,length,NULL,false);free(bytes);node_t *ignored=NULL;
        check(p&&html_resume(p,&ignored)==0,"policy reparse");check(policies(d)==2,"CSP meta replay does not duplicate");
        if(p)html_finish(p);web_free(d);
    }
    printf("html_encoding_restarttest: %u checks, %u failed\n",checks,failures);return failures!=0;
}
