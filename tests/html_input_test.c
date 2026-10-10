#include <stdio.h>
#include "webi.h"
static unsigned checks,failures;
static void check(const char *name,bool ok){checks++;if(!ok){failures++;printf("FAIL html_input_test %s\n",name);}}
static node_t *id(node_t *root,const char *name){if(!root)return NULL;if(root->id && !strcmp(root->id,name))return root;for(node_t *n=root->first;n;n=n->next){node_t *found=id(n,name);if(found)return found;}return NULL;}
static bool text(node_t *node,const char *expected){sbuf out={0};if(node)node_text_content(node,&out);bool ok=node && out.n==strlen(expected) && !memcmp(out.p,expected,out.n);sb_free(&out);return ok;}
int main(void){
    web_doc *d=web_parse("<p id=x>\x80",sizeof "<p id=x>\x80"-1,"https://parser.test/"," WINDOWS-1252 ");
    check("label whitespace canonical",d && !strcmp(d->encoding,"windows-1252"));
    check("windows euro",d && text(id(d->root,"x"),"\xe2\x82\xac"));web_free(d);
    const char deceptive[]="<!-- charset=shift_jis --><div title='charset=shift_jis'><p id=x>\xe3\x81\x82";
    d=web_parse(deceptive,sizeof deceptive-1,"https://parser.test/",NULL);
    check("prescan ignores comment and non-meta attribute",d && text(id(d->root,"x"),"\xe3\x81\x82"));web_free(d);
    const char gb[]="<meta charset=gb18030><p id=x>\xc4\xe3\xba\xc3";
    d=web_parse(gb,sizeof gb-1,"https://parser.test/",NULL);
    check("gb18030 decoder",d && text(id(d->root,"x"),"\xe4\xbd\xa0\xe5\xa5\xbd"));web_free(d);
    const char utf16[]={0xff,0xfe,'<',0,'p',0,'>',0,'A',0,0x42,0x30};
    d=web_parse(utf16,sizeof utf16,"https://parser.test/","windows-1252");
    check("UTF16 BOM overrides transport",d && !strcmp(d->encoding,"UTF-16LE"));
    check("UTF16 bytes decode",d && text(d->root,"A\xe3\x81\x82"));web_free(d);
    const char malformed[]="<p id=x>\xc0\xaf\xed\xa0\x80";
    d=web_parse(malformed,sizeof malformed-1,"https://parser.test/","utf-8");
    check("UTF8 malformed replacement",d && text(id(d->root,"x"),"\xef\xbf\xbd\xef\xbf\xbd\xef\xbf\xbd\xef\xbf\xbd\xef\xbf\xbd"));
    web_doc *u=doc_inert(d,"\xef\xbb\xbf<!doctype html><p id=x>A",sizeof "\xef\xbb\xbf<!doctype html><p id=x>A"-1,"about:blank");
    check("DOMString FEFF remains text",u && text(u->root,"\xef\xbb\xbf" "A"));
    check("DOMString BOM is not stripped before doctype",u && u->quirks);
    const char nul[]="<p id=x>A\0B</p><textarea id=t>A\0B</textarea><div id=a title='A\0B'>";
    u=doc_inert(d,nul,sizeof nul-1,"about:blank");
    check("data NUL omitted by tree state",u && text(id(u->root,"x"),"AB"));
    check("RCDATA NUL replacement",u && text(id(u->root,"t"),"A\xef\xbf\xbd" "B"));
    check("attribute NUL replacement",u && !strcmp(node_attr(id(u->root,"a"),"title"),"A\xef\xbf\xbd" "B"));
    web_free(d);
    const char limited[]="<!doctype html PUBLIC '-//W3C//DTD XHTML 1.0 Transitional//EN' 'http://www.w3.org/TR/xhtml1/DTD/xhtml1-transitional.dtd'><p id=x>";
    d=web_parse(limited,sizeof limited-1,"https://parser.test/","utf-8");
    check("limited quirks is distinct",d && d->document_mode==2 && !d->quirks);web_free(d);
    d=web_live("<!doctype html><script></script><p id=future>end",sizeof "<!doctype html><script></script><p id=future>end"-1,"https://parser.test/","utf-8",NULL);
    check("live parser exists",d && d->parser);
    if(d && d->parser){
        node_t *script=NULL;check("script boundary",html_resume(d->parser,&script)==1 && script);
        void *fence=html_write_boundary(d->parser);
        check("insert write",html_write(d->parser,"<p id=written>now</p>",sizeof "<p id=written>now</p>"-1));
        check("bounded pump",html_resume_written(d->parser,&script,fence)==2 && !script);
        check("written visible before network suffix",id(d->root,"written") && !id(d->root,"future"));
        check("network resume completes",html_resume(d->parser,&script)==0 && id(d->root,"future"));
    }
    web_free(d);
    printf("html_input_test: %u checks, %u failures\n",checks,failures);return failures!=0;
}
