#include <stdio.h>
#include "webi.h"
#include "sandbox.h"
static unsigned checks,failures;
static void check(const char *name,bool ok){checks++;if(!ok){failures++;printf("FAIL html_input_test %s\n",name);}}
static node_t *id(node_t *root,const char *name){if(!root)return NULL;if(root->id && !strcmp(root->id,name))return root;for(node_t *n=root->first;n;n=n->next){node_t *found=id(n,name);if(found)return found;}return NULL;}
static bool text(node_t *node,const char *expected){sbuf out={0};if(node)node_text_content(node,&out);bool ok=node && out.n==strlen(expected) && !memcmp(out.p,expected,out.n);sb_free(&out);return ok;}
static int pump(struct html_parser *p,node_t **script){int result;do{result=html_resume(p,script);}while(result==HTML_PARSE_YIELD);return result;}
static void document_mode_case(const char *source,unsigned expected,bool srcdoc,bool dom_string){
    web_doc *d=web_parse("",0,"https://parser.test/","utf-8");check("文書モードの準備",d!=NULL);if(!d)return;
    d->html_srcdoc=srcdoc;
    struct html_parser *p=dom_string?html_begin_string(d,source,strlen(source),false,false):html_begin(d,source,strlen(source),"utf-8",false);
    node_t *script=NULL;check("文書モード入力の完了",p && pump(p,&script)==HTML_PARSE_DONE && !script);
    check("文書モードとquirksの一致",p && d->document_mode==expected && d->quirks==(expected==1));
    if(srcdoc && strstr(source,"<!doctype")){
        node_t *doctype=d->root?d->root->first:NULL;
        const char *name=strstr(source,"<!doctype wrong>")?"wrong":"html";
        check("srcdocのDOCTYPEを保持",doctype && doctype->type==N_DOCTYPE && !strcmp(doctype->name,name));
    }
    if(p)html_finish(p);web_free(d);
}
static void cooperative_input(void){
    const char *prefix="<!doctype html><p id=large>";size_t prefix_n=strlen(prefix),body_n=300000;
    char *source=malloc(prefix_n+body_n+5);check("大きい入力の確保",source!=NULL);if(!source)return;
    memcpy(source,prefix,prefix_n);memset(source+prefix_n,'a',body_n);memcpy(source+prefix_n+body_n,"</p>",5);
    memcpy(source+16382,"&amp;",5);source[32767]='\r';source[32768]='\n';memcpy(source+49151,"\xf0\x9f\x98\x80",4);
    web_doc *d=web_parse("",0,"https://parser.test/","utf-8");check("協調解析文書",d!=NULL);if(!d){free(source);return;}
    struct html_parser *p=html_begin_string_mode(d,source,prefix_n+body_n+4,HTML_SCRIPTING_DISABLED,false);
    node_t *script=NULL;int result=p?html_resume(p,&script):HTML_PARSE_FAILED;node_t *identity=d->root;
    check("大入力でscriptではなく協調yield",result==HTML_PARSE_YIELD && !script && html_pending_input(p));
    result=p?pump(p,&script):HTML_PARSE_FAILED;
    check("協調解析再開は同じDocumentで完了",result==HTML_PARSE_DONE && !script && d->root==identity && !d->quirks);
    sbuf out={0};node_t *large=id(d->root,"large");if(large)node_text_content(large,&out);
    size_t amp=16382-prefix_n,cr=32767-prefix_n-4,emoji=49151-prefix_n-5;
    check("chunk境界の文字参照CRLF UTF8を保持",large && out.n==body_n-5 && out.p[amp]=='&' && out.p[cr]=='\n' && !memcmp(out.p+emoji,"\xf0\x9f\x98\x80",4));
    sb_free(&out);if(p)html_finish(p);web_free(d);
    d=web_parse("",0,"https://parser.test/","utf-8");p=d?html_begin_string(d,source,prefix_n+body_n+4,false,false):NULL;
    check("取消前の協調yield",p && html_resume(p,&script)==HTML_PARSE_YIELD);
    if(d)d->native_cancelled=true; /* this test document has no concurrent owner */
    check("取消はEOF完了と区別",p && html_resume(p,&script)==HTML_PARSE_CANCELLED && !script && !html_pending_input(p));
    check("取消後のwriteを拒否",p && !html_write(p,"<p>late",7));
    if(p)html_finish(p);web_free(d);free(source);
}
static void fragment_modes(void){
    web_doc *d=web_parse("<!doctype html><body>",sizeof "<!doctype html><body>"-1,"https://parser.test/","utf-8");
    check("断片解析文書",d && d->body);if(!d)return;
    d->live=true; /* a browsing-context document supplies enabled scripting */
    const char *source="<script>void 0</script><noscript><b id=fallback>fallback</b></noscript>";
    node_t *fragment=html_fragment_mode(d,d->body,d->body,source,strlen(source),false,HTML_SCRIPTING_INERT);
    check("Inertはscript不活性とnoscript rawtext",fragment && fragment->first && fragment->first->script_started && !fragment->first->script_parse_eligible && !id(fragment,"fallback"));
    fragment=html_fragment_mode(d,d->body,d->body,source,strlen(source),false,HTML_SCRIPTING_DISABLED);
    check("Disabledはnoscript fallbackを解析",fragment && id(fragment,"fallback") && !fragment->first->script_parse_eligible);
    fragment=html_fragment_mode(d,d->body,d->body,source,strlen(source),false,HTML_SCRIPTING_FRAGMENT);
    check("Fragmentは閉鎖scriptを挿入後実行候補として保持",fragment && fragment->first && !fragment->first->script_started && fragment->first->script_parse_eligible && fragment->first->script_text_snapshot_valid && !id(fragment,"fallback"));
    node_t *context=doc_node_create(d,N_ELEM,"nullcontext",NULL,0);
    if(context){context->namespace_id=NS_NONE;context->foreign=true;}
    fragment=context?html_fragment_mode(d,context,context,"<nullchild/>",12,false,HTML_SCRIPTING_INERT):NULL;
    check("null名前空間の断片文脈を保全",fragment && fragment->first && fragment->first->namespace_id==NS_NONE && fragment->first->foreign);
    node_t *template=doc_node_create(d,N_ELEM,"template",NULL,0);
    node_t *content=template?doc_template_content(d,template):NULL;
    const char *template_markup="<noscript><b id=hidden>hidden</b></noscript><p id=para><table id=table><tr><td>X</td></tr></table>";
    if(content){content->owner->document_mode=1;content->owner->quirks=true;}
    fragment=content?html_fragment_mode(content->owner,content,template,template_markup,strlen(template_markup),false,HTML_SCRIPTING_INERT):NULL;
    node_t *table=id(fragment,"table");
    check("template targetのownerで断片と子を確保",fragment && content && fragment->owner==content->owner && fragment->first && fragment->first->owner==content->owner);
    check("template contextのscriptingと標準modeを使用",fragment && !id(fragment,"hidden") && table && table->parent==fragment);
    check("native template innerHTMLもcontext基準でnoscript解析",template && doc_node_html(d,template,template_markup,strlen(template_markup)) && !id(content,"hidden") && id(content,"table") && id(content,"table")->parent==content);
    bool live=d->live,inert=d->inert;uint32_t sandbox=d->sandbox_flags;
    d->live=true;d->sandbox_flags=SB_SCRIPTS;
    fragment=html_fragment_ex(d,d->body,source,strlen(source),false,false);
    check("sandbox Disabledの互換Inert入口はnoscript fallback",fragment && id(fragment,"fallback") && fragment->first->script_started && !fragment->first->script_parse_eligible);
    fragment=html_contextual_fragment(d,d->body,source,strlen(source));
    check("sandbox Disabledのcontextual入口はscript不活性",fragment && id(fragment,"fallback") && fragment->first->script_started && !fragment->first->script_parse_eligible);
    d->sandbox_flags=0;d->inert=true;
    fragment=html_fragment_ex(d,d->body,source,strlen(source),false,true);
    check("inert文書の互換contextual入口はDisabled",fragment && id(fragment,"fallback") && fragment->first->script_started && !fragment->first->script_parse_eligible);
    d->inert=false;d->sandbox_flags=SB_SCRIPTS;
    struct html_parser *p=html_open(d);node_t *script=NULL;
    const char *stream="<!doctype html><body><noscript><b id=streamfallback>fallback</b></noscript><script>void 0</script>";
    check("sandbox Disabled streamは外部入力を受理",p && html_write(p,stream,strlen(stream)));
    if(p)html_close(p);
    check("sandbox Disabled streamはscript停止せずfallbackを構築",p && pump(p,&script)==HTML_PARSE_DONE && !script && id(d->root,"streamfallback"));
    if(p)html_finish(p);
    d->live=live;d->inert=inert;d->sandbox_flags=sandbox;
    web_free(d);
}
int main(void){
    document_mode_case("<!doctype html><p>x",0,false,false);
    document_mode_case("<p>x",1,false,false);
    document_mode_case("<!doctype html PUBLIC '-//W3C//DTD XHTML 1.0 Transitional//EN' 'x'><p>x",2,false,false);
    document_mode_case("<!doctype html><p>x",0,false,true);
    document_mode_case("<!doctype html><p>x",0,true,false);
    document_mode_case("<!doctype html><p>x",0,true,true);
    document_mode_case("<p>x",0,true,false);
    document_mode_case("<!doctype wrong><p>x",0,true,false);
    cooperative_input();fragment_modes();
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
