/* Native/Lexbor import transactions, not visible-site acceptance. */
#include "html_lexbor.h"
#include "frame.h"
#include <lexbor/dom/interfaces/element.h>
#include <lexbor/dom/interfaces/character_data.h>
#include <nocturne.h>
#include <stdio.h>

static unsigned checks,failures,errors;
static void check(bool ok,const char *name){checks++;if(!ok){failures++;printf("FAIL parserdedup %s\n",name);}}
static void log_line(void *context,int level,const char *text){(void)context;if(level>=2){errors++;printf("ERROR %s\n",text);}}
static bool checkpoint(void *context){(void)context;return true;}
static bool eval(web_doc *d,const char *source){return web_console_eval(d,source,strlen(source));}
static void settle(web_doc *top,unsigned count){for(unsigned i=0;i<count;i++){web_tick(top,uptime_ms());n_sleep_ms(1);}}
static node_t *find_id(node_t *n,const char *id){
    if(!n)return NULL;if(n->id&&!strcmp(n->id,id))return n;
    for(node_t *c=n->first;c;c=c->next){node_t *found=find_id(c,id);if(found)return found;}
    return NULL;
}
static lxb_dom_node_t *find_lex(struct html_parser *p,lxb_dom_node_t *lex,node_t *native){
    if(html_bridge_native(p,lex)==native)return lex;
    for(lxb_dom_node_t *c=lex->first_child;c;c=c->next){lxb_dom_node_t *found=find_lex(p,c,native);if(found)return found;}
    return NULL;
}
struct recovery_control {web_doc *document;bool armed,inside_author;unsigned stopped,recovered;};
static bool recovery_checkpoint(void *context){
    struct recovery_control *c=context;
    if(c->armed && c->document && c->document->native_layout_depth){
        c->armed=false;c->inside_author=web_js_running(c->document);return false;
    }
    return true;
}
static void recovery_log(void *context,int level,const char *line){
    struct recovery_control *c=context;
    if(strstr(line,"Native processing cancelled:"))c->stopped++;
    if(strstr(line,"Native rendering recovered from coherent DOM"))c->recovered++;
    if(level>=1)printf("RECOVERY %s\n",line);
}
static void cancelled_view_recovery(void){
    const char *page="<!doctype html><style>body{margin:0;background:#112233}a{display:block;width:120px;height:40px}#tail{margin-top:1200px}</style><body><a id=go href='https://parser.test/go'>go</a><a id=tail href='https://parser.test/tail'>tail</a><script id=pause>0</script><p id=never>unconsumed</p>";
    struct recovery_control control={0};
    struct web_host host={.opaque=&control,.console=recovery_log,.script_checkpoint=recovery_checkpoint,.js_task_budget_ms=5000};
    web_doc *d=web_live(page,strlen(page),"https://parser.test/","utf-8",&host);control.document=d;
    check(d!=NULL,"cancellable live document");if(!d)return;
    node_t *script=NULL;
    check(d->parser&&html_resume(d->parser,&script)==HTML_PARSE_SCRIPT&&script,"unconsumed HTML remains behind real script boundary");
    check(eval(d,"document.addEventListener('DOMContentLoaded',()=>document.body.setAttribute('data-ready','fired'));window.addEventListener('load',()=>document.body.setAttribute('data-load','fired'));"),"cancel lifecycle observers installed");
    web_layout(d,128,64);node_t *go=find_id(d->root,"go"),*tail=find_id(d->root,"tail");
    uint32_t pixels[128*64],saved[128*64];canvas_t canvas;gfx_init(&canvas,pixels,128,64,128);
    web_paint(d,&canvas,0,0,128,64,0,0);memcpy(saved,pixels,sizeof pixels);
    check(d->layout_valid&&d->root_box&&pixels[127+63*128]==0xff112233u,"coherent coloured page exists before cancellation");
    d->resources_dirty=d->need_style=d->need_boxes=true;d->native_checkpoint_count=63;control.armed=true;
    (void)eval(d,"document.body.getBoundingClientRect();");
    check(control.inside_author&&!control.armed&&d->native_cancelled&&d->native_recovery_pending&&!d->layout_valid&&!d->root_box,"native checkpoint cancels actual author geometry transaction");
    web_paint(d,&canvas,0,0,128,64,0,500);
    check(!memcmp(saved,pixels,sizeof pixels)&&d->view_y==0,"cancelled painting preserves pixels without faking scroll");
    struct web_hit hit;
    check(!web_hit_test(d,3,3,&hit)&&!web_node_at(d,3,3),"cancelled view cannot hit stale geometry");
    control.armed=true;d->native_checkpoint_count=63;
    web_layout(d,128,64);
    check(!control.armed&&d->native_cancelled&&d->native_recovery_pending&&!d->native_layout_depth&&!d->parser,
        "recovery itself remains cancellable and safely unwinds for retry");
    web_layout(d,128,64);
    check(!d->native_cancelled&&!d->native_recovery_pending&&!d->native_recovery_active&&d->layout_valid&&d->root_box,"outer native transaction rebuilds coherent layout after unwind");
    check(!d->parser&&!find_id(d->root,"never"),"aborted HTML suffix is retired not resumed");
    check(go&&tail&&find_id(d->root,"go")==go&&find_id(d->root,"tail")==tail,"recovery preserves coherent native DOM identity");
    check(!eval(d,"window.mustNotRestart=1"),"author runtime stays stopped after view recovery");
    int x=0,y=0,w=0,h=0;
    bool rect=go&&web_node_rect(d,go,&x,&y,&w,&h);
    check(rect&&web_hit_test(d,x+3,y+3,&hit)&&hit.kind==WEB_HIT_LINK,"recovered visible link has valid native hit test");
    rect=tail&&web_node_rect(d,tail,&x,&y,&w,&h);
    if(rect)web_paint(d,&canvas,0,0,128,64,0,y);
    check(rect&&y>900&&d->view_y==y&&web_hit_test(d,x+3,y+3,&hit)&&hit.kind==WEB_HIT_LINK,"recovered viewport really scrolls to rebuilt lower content");
    settle(d,20);
    check(!d->parser&&!node_attr(d->body,"data-ready")&&!node_attr(d->body,"data-load"),"cancelled parser never fakes ready or load callbacks");
    check(control.stopped==1&&control.recovered==1,"stop and recovery console diagnostics are emitted once");
    web_free(d);
}
static void cancelled_render_resume(void){
    const char *page="<!doctype html><body><a id=go href='/go'>go</a></body>";
    struct recovery_control control={0};
    struct web_host host={.opaque=&control,.console=recovery_log,.script_checkpoint=recovery_checkpoint};
    web_doc *d=web_live(page,strlen(page),"https://parser.test/","utf-8",&host);
    check(d!=NULL,"render resume document");if(!d)return;control.document=d;
    settle(d,8);
    check(!d->parser&&eval(d,"if(document.readyState!=='complete')throw new Error('parser incomplete')"),"real parser and lifecycle completed before native interruption");
    check(eval(d,"window.nativeResumeCount=0;setTimeout(()=>{window.nativeResumeCount++;const r=document.body.getBoundingClientRect();if(r.width<=0)throw new Error('unrecovered geometry');document.body.setAttribute('data-resumed','yes')},40)"),"queued author timer exists before interrupted rendering");
    node_t *go=find_id(d->root,"go");
    d->need_style=d->need_boxes=d->resources_dirty=true;
    d->native_checkpoint_count=63;control.armed=true;
    web_layout(d,320,200);
    check(!control.inside_author&&d->native_cancelled&&!d->layout_valid&&!d->root_box,"autonomous native transaction cancelled without author stack");
    web_tick(d,uptime_ms());
    check(!d->native_cancelled&&!d->native_recovery_pending&&d->layout_valid&&find_id(d->root,"go")==go,"normal tick rebuilds coherent rendering before author callbacks");
    settle(d,65);
    check(node_attr(d->body,"data-resumed")&&!strcmp(node_attr(d->body,"data-resumed"),"yes"),"queued author timer survives native-only cancellation");
    check(eval(d,"if(nativeResumeCount!==1)throw new Error('timer count');document.body.setAttribute('data-after','alive')"),"new author task remains executable after rendering recovery");
    check(node_attr(d->body,"data-after")&&!strcmp(node_attr(d->body,"data-after"),"alive"),"new task changes the actual native DOM");
    check(control.stopped==1&&control.recovered==1,"native-only interruption and recovery reported once");
    web_free(d);
}
static void isolated_owner_reparent(struct html_parser *p,web_doc *d,web_doc *shadow_owner,web_doc *template_owner){
    node_t *shadow_host=doc_node_create(d,N_ELEM,"div",NULL,0);
    node_t *template=doc_node_create(d,N_ELEM,"template",NULL,0);
    node_t *shadow=shadow_host?doc_shadow_attach(d,shadow_host,false,false,false,false,false):NULL;
    node_t *content=template?doc_template_content(d,template):NULL;
    bool ready=shadow&&content&&doc_node_move(d,d->body,shadow_host,NULL)&&doc_node_move(d,d->body,template,NULL);
    check(ready,"real shadow and template isolated forests are created");if(!ready)return;
    web_doc *shadow_allocation=shadow->allocation_doc,*content_allocation=content->allocation_doc;
    ready=doc_node_move(shadow_owner,shadow_owner->body,shadow_host,NULL)&&
        doc_node_move(template_owner,template_owner->body,template,NULL)&&html_bridge_export(p);
    lxb_dom_node_t *lexbody=html_bridge_lex(p,d->body),*lexhost=html_bridge_lex(p,shadow_host);
    lxb_dom_node_t *lextemplate=html_bridge_lex(p,template),*lexshadow=html_bridge_lex(p,shadow),*lexcontent=html_bridge_lex(p,content);
    check(ready&&lexbody&&lexhost&&lextemplate&&lexshadow&&lexcontent,
        "adopted isolated forests have real stable Lexbor bindings");
    if(!ready||!lexbody||!lexhost||!lextemplate||!lexshadow||!lexcontent)return;
    web_doc *old_shadow_owner=shadow->owner,*old_content_owner=content->owner;
    lxb_dom_node_t *shadow_text=lxb_dom_interface_node(lxb_dom_document_create_text_node(&p->document->dom_document,(const lxb_char_t *)"shadow-new",10));
    lxb_dom_node_t *template_text=lxb_dom_interface_node(lxb_dom_document_create_text_node(&p->document->dom_document,(const lxb_char_t *)"template-new",12));
    check(shadow_text&&template_text,"unbound new children use real Lexbor interfaces");if(!shadow_text||!template_text)return;
    lxb_dom_node_remove_wo_events(lexhost);lxb_dom_node_insert_child_wo_events(lexbody,lexhost);
    lxb_dom_node_remove_wo_events(lextemplate);lxb_dom_node_insert_child_wo_events(lexbody,lextemplate);
    lxb_dom_node_insert_child_wo_events(lexshadow,shadow_text);
    lxb_dom_node_insert_child_wo_events(lexcontent,template_text);
    bool imported=html_bridge_import(p);
    check(imported&&html_import_changed(p),"host reparent and unbound isolated children commit together");
    node_t *new_shadow=imported?html_bridge_native(p,shadow_text):NULL,*new_template=imported?html_bridge_native(p,template_text):NULL;
    check(new_shadow&&shadow_host->parent==d->body&&shadow_host->owner==d&&shadow->owner==d&&
        new_shadow->parent==shadow&&new_shadow->owner==d&&new_shadow->allocation_doc==old_shadow_owner&&
        shadow->allocation_doc==shadow_allocation,"shadow child follows final host owner without migrating allocation storage");
    check(new_template&&template->parent==d->body&&template->owner==d&&content->owner==d->template_doc&&
        new_template->parent==content&&new_template->owner==content->owner&&new_template->allocation_doc==old_content_owner&&
        content->allocation_doc==content_allocation,"template child follows final inert owner without migrating allocation storage");
    uint64_t revision=d->dom_revision;
    check(html_bridge_import(p)&&!html_import_changed(p)&&d->dom_revision==revision,"isolated owner resolution is stable after one commit");
    bool staged=lxb_dom_element_set_attribute(lxb_dom_interface_element(lexhost),(const lxb_char_t *)"data-uncommitted",16,(const lxb_char_t *)"pending",7)!=NULL;
    lxb_dom_node_t *following=lexhost->next;
    lxb_dom_node_remove_wo_events(lexhost);lxb_dom_node_insert_child_wo_events(lexshadow,lexhost);
    imported=html_bridge_import(p);
    check(staged&&!imported&&shadow_host->parent==d->body&&shadow_host->owner==d&&shadow->owner==d&&
        !node_attr(shadow_host,"data-uncommitted"),"host dependency cycle fails before native links or prepared fields are published");
    lxb_dom_node_remove_wo_events(lexhost);
    if(following)lxb_dom_node_insert_before_wo_events(following,lexhost);else lxb_dom_node_insert_child_wo_events(lexbody,lexhost);
    (void)lxb_dom_element_remove_attribute(lxb_dom_interface_element(lexhost),(const lxb_char_t *)"data-uncommitted",16);
    check(html_bridge_import(p)&&!html_import_changed(p)&&d->dom_revision==revision,"restoring private dependencies leaves the original coherent DOM unchanged");
}

static void adopted_forests(web_doc *d){
    sbuf source={0};sb_puts(&source,"<!doctype html><html><body>");
    for(unsigned i=0;i<70;i++){char part[64];snprintf(part,sizeof part,"<i id=n%u>x</i>",i);sb_puts(&source,part);}
    sb_puts(&source,"<section id=order><b id=o0></b><b id=o1></b><b id=o2></b><b id=o3></b></section><script>void 0</script>");
    struct html_parser *p=html_begin(d,sb_cstr(&source),source.n,"utf-8",true);sb_free(&source);
    check(p!=NULL,"forest parser");if(!p)return;
    node_t *script=NULL;int result;
    do{result=html_resume(p,&script);}while(result==HTML_PARSE_YIELD);
    check(result==1&&script&&html_import_changed(p),"real inline script yield publishes input");
    web_doc *children[70]={0};node_t *nodes[70]={0};lxb_dom_node_t *lexnodes[70]={0};uint64_t revisions[70]={0};
    bool prepared=true;
    const char *empty="<!doctype html><html><body></body></html>";
    for(unsigned i=0;i<70;i++){
        char id[20];snprintf(id,sizeof id,"n%u",i);nodes[i]=find_id(d->root,id);
        lexnodes[i]=nodes[i]?find_lex(p,p->root,nodes[i]):NULL;
        children[i]=doc_inert(d,empty,strlen(empty),"about:blank");
        if(!nodes[i]||!lexnodes[i]||!children[i]||!children[i]->body||
           !doc_node_move(children[i],children[i]->body,nodes[i],NULL)){prepared=false;break;}
    }
    check(prepared,"70 allocation-stable adopted owners");
    if(!prepared){html_finish(p);return;}
    check(html_bridge_export(p),"export all adopted native forests");
    for(unsigned i=0;i<70;i++){children[i]->dirty=children[i]->resources_dirty=false;revisions[i]=children[i]->dom_revision;}
    uint64_t main_revision=d->dom_revision;
    bool imported=html_bridge_import(p),stable=imported&&!html_import_changed(p);
    for(unsigned i=0;i<70;i++)if(children[i]->dom_revision!=revisions[i])stable=false;
    check(stable&&d->dom_revision==main_revision,"native export followed by equal import is not double-notified");
    bool lex_changed=true;
    for(unsigned i=0;i<70;i++)if(!lxb_dom_element_set_attribute(lxb_dom_interface_element(lexnodes[i]),
        (const lxb_char_t *)"data-parser",11,(const lxb_char_t *)"changed",7))lex_changed=false;
    check(lex_changed,"real lexical attribute change across 70 owners");
    imported=html_bridge_import(p);bool notified=imported&&html_import_changed(p),owned=true;
    for(unsigned i=0;i<70;i++){
        const char *value=node_attr(nodes[i],"data-parser");
        if(children[i]->dom_revision<=revisions[i]||!children[i]->resources_dirty||!value||strcmp(value,"changed"))notified=false;
        if(nodes[i]->owner!=children[i]||nodes[i]->parent!=children[i]->body)owned=false;
        revisions[i]=children[i]->dom_revision;
    }
    check(notified,"all 70 changed documents notified without 66-document loss");
    check(owned,"adopted forest logical owners survive parser import");
    imported=html_bridge_import(p);stable=imported&&!html_import_changed(p);
    for(unsigned i=0;i<70;i++)if(children[i]->dom_revision!=revisions[i])stable=false;
    check(stable,"second equal multi-owner transaction has no new revision");
    lxb_dom_node_t *destination=lexnodes[1]->parent;
    lxb_dom_node_remove_wo_events(lexnodes[0]);
    lxb_dom_node_insert_child_wo_events(destination,lexnodes[0]);
    imported=html_bridge_import(p);
    check(imported&&html_import_changed(p)&&children[0]->dom_revision>revisions[0]&&
        children[1]->dom_revision>revisions[1],"real lexical adoption notifies both old and new owners");
    check(nodes[0]->owner==children[1]&&nodes[0]->parent==children[1]->body&&
        nodes[0]->first&&nodes[0]->first->owner==children[1],"real lexical adoption updates whole subtree ownership");
    node_t *order=find_id(d->root,"order"),*one=find_id(d->root,"o1"),*two=find_id(d->root,"o2");
    lxb_dom_node_t *lexone=one?find_lex(p,p->root,one):NULL,*lextwo=two?find_lex(p,p->root,two):NULL;
    uint64_t order_revision=order?order->resource_revision:0;
    node_t *first=order?order->first:NULL,*last=order?order->last:NULL;
    if(lexone&&lextwo){lxb_dom_node_remove_wo_events(lextwo);lxb_dom_node_insert_before_wo_events(lexone,lextwo);}
    imported=html_bridge_import(p);
    check(imported&&html_import_changed(p)&&order&&order->first==first&&order->last==last&&
        order->resource_revision>order_revision&&two&&two->next==one,
        "middle sibling reorder changes parent revision without changing endpoints");
    check(two&&one&&two->elem_index==2&&one->elem_index==3,"reordered native nth-child indexes are coherent");
    main_revision=d->dom_revision;uint64_t destination_revision=children[1]->dom_revision;
    check(html_bridge_import(p)&&!html_import_changed(p)&&d->dom_revision==main_revision&&
        children[1]->dom_revision==destination_revision,"adoption and reorder second import is a no-op");
    isolated_owner_reparent(p,d,children[4],children[5]);
    /* Both are real product parsers, paused at actual script tokens. Exporting
       an adopted node's destination binds its enclosing forest in parser one.
       A later parser-two commit must broadcast to those shared identities. */
    const char *second_source="<!doctype html><body><div id=shared><span id=mutable>ABCDE<script id=stop>0</script>FG</span><i id=late></i></div><body data-second=parser>";
    web_doc *second=web_live(second_source,strlen(second_source),"https://parser.test/shared","utf-8",NULL);
    node_t *second_script=NULL;int second_result=HTML_PARSE_FAILED;
    if(second&&second->parser)do{second_result=html_resume(second->parser,&second_script);}while(second_result==HTML_PARSE_YIELD);
    check(second&&second->parser&&second_result==HTML_PARSE_SCRIPT&&second_script,"second product parser pauses at a real script boundary");
    if(second&&second->parser&&second_result==HTML_PARSE_SCRIPT&&second_script){
        node_t *shared=find_id(second->root,"shared"),*mutable=find_id(second->root,"mutable");
        node_t *text=mutable?mutable->first:NULL;
        bool bound=shared&&mutable&&text&&doc_node_move(second,shared,nodes[2],NULL)&&html_bridge_export(p);
        lxb_dom_node_t *lexshared=bound?lexnodes[2]->parent:NULL;
        check(bound&&lexshared&&html_bridge_native(p,lexshared)==shared,"destination forest has two live parser subscriptions");
        if(bound&&lexshared){
            bool edit=doc_node_text(second,text,"A-middle-E",10);
            doc_node_remove(second,second_script);
            check(edit&&html_bridge_export(p),"author middle edit and removal are exported before parser commit");
            do{second_result=html_resume(second->parser,&second_script);}while(second_result==HTML_PARSE_YIELD);
            node_t *late=find_id(second->root,"late");
            check(second_result==HTML_PARSE_DONE&&late&&mutable->first==text&&text->textlen==12&&
                !memcmp(text->text,"A-middle-EFG",12),"second parser commits real text merging and a new child");
            check(late&&!find_lex(p,lexshared,late),"new parser child was not already exported by author edits");
            node_t *ignored=NULL;int first_result;
            do{first_result=html_resume(p,&ignored);}while(first_result==HTML_PARSE_YIELD);
            check(first_result==HTML_PARSE_DONE&&!ignored,"first product parser resumes after shared commit");
            lxb_dom_node_t *lexlate=late?find_lex(p,lexshared,late):NULL;
            lxb_dom_node_t *lextext=find_lex(p,lexshared,text);
            check(lexlate&&lexlate->parent==lexshared,"other parser sees the committed child list without losing identity");
            check(lextext&&lxb_dom_interface_character_data(lextext)->data.length==12&&
                !memcmp(lxb_dom_interface_character_data(lextext)->data.data,"A-middle-EFG",12),"other parser sees only the final coherent character data");
            lxb_dom_node_t *lexbody=lexshared->parent;size_t value_length=0;
            const lxb_char_t *value=lexbody?lxb_dom_element_get_attribute(lxb_dom_interface_element(lexbody),
                (const lxb_char_t *)"data-second",11,&value_length):NULL;
            check(value&&value_length==6&&!memcmp(value,"parser",6),"other parser sees an existing element attribute merged by real tokens");
            uint64_t second_revision=second->dom_revision;
            check(html_bridge_import(p)&&!html_import_changed(p)&&second->dom_revision==second_revision,
                "shared coherent commit does not feed back into duplicate notifications");
            /* The original allocation owner outlives this independent document. */
            (void)doc_node_move(d,d->body,nodes[2],NULL);
        }
    }
    if(second)web_free(second);
    html_finish(p);
}

int main(int argc,char **argv){
    if(argc>1&&!strcmp(argv[1],"--render-resume")){
        cancelled_render_resume();
        printf("parser-render-resume: %u checks, %u failures\n",checks,failures);return failures?1:0;
    }
    cancelled_view_recovery();
    /* Missing DOCTYPE changes a normal document's mode, never a srcdoc
       document's. Exercise both real child contexts instead of allowing a
       lost srcdoc flag to make the normal-document assertion pass. */
    const char *page="<!doctype html><iframe id=regular src=about:blank></iframe><iframe id=srcdoc srcdoc='<p>initial frame</p>'></iframe>";
    struct web_host host={.console=log_line,.script_checkpoint=checkpoint,.js_task_budget_ms=5000};
    web_doc *top=web_live(page,strlen(page),"https://parser.test/","utf-8",&host);
    check(top!=NULL,"live document");if(!top)goto done;
    uint64_t deadline=uptime_ms()+10000;web_doc *d=NULL,*srcdoc=NULL;
    do{
        settle(top,1);
        struct web_frame *normal_frame=web_frame_find(top,find_id(top->root,"regular"));
        struct web_frame *srcdoc_frame=web_frame_find(top,find_id(top->root,"srcdoc"));
        if(normal_frame&&!normal_frame->detached)d=normal_frame->document;
        if(srcdoc_frame&&!srcdoc_frame->detached&&srcdoc_frame->document&&srcdoc_frame->document->html_srcdoc)srcdoc=srcdoc_frame->document;
    }while((!d||!srcdoc)&&uptime_ms()<deadline);
    check(d!=NULL,"live child browsing context");if(!d)goto cleanup;
    check(!d->html_srcdoc,"normal about:blank context is not srcdoc");
    check(srcdoc!=NULL,"separate live srcdoc browsing context");
    settle(top,20);web_layout(d,800,600);
    check(d->width==800&&d->height==600,"initialized child viewport");d->profile_enabled=true;
    check(eval(d,"document.open();document.write('<!doctype html><style>#target{color:rgb(1,2,3)}</style><body><p id=target>A</p><template id=holder><b id=tchild>T</b></template><div id=tail>B');var saved=document.getElementById('target');"),
        "document.open real input");
    settle(top,8);web_layout(d,800,600);
    node_t *target=find_id(d->root,"target"),*holder=find_id(d->root,"holder");
    node_t *template_child=holder&&holder->template_content?holder->template_content->first:NULL;
    check(d->parser&&target&&target->style&&target->style->color==0xff010203u&&!d->quirks,"initial real CSS and nonquirks tree");
    if(!d->parser||!target)goto cleanup;
    uint64_t revision=d->dom_revision,body_revision=d->body->resource_revision,
        rescans=d->profile.rescans,style=d->profile.style_visits;
    check(html_bridge_import(d->parser)&&!html_import_changed(d->parser),"direct no-op import reports no actual transaction");
    check(d->dom_revision==revision&&d->body->resource_revision==body_revision,"no-op import preserves DOM and resource revisions");
    for(unsigned i=0;i<100;i++){settle(top,1);web_layout(d,800,600);}
    check(d->dom_revision==revision&&d->profile.rescans==rescans&&d->profile.style_visits==style,
        "100 idle stream ticks preserve CSS and style work");
    check(!html_import_changed(d->parser),"latest empty resume resets previous change result");
    check(eval(d,"saved.textContent='native';document.body.setAttribute('data-native','kept');"),"native edits during parser suspension");
    check(html_bridge_export(d->parser),"native edits export to tree builder");
    revision=d->dom_revision;
    check(html_bridge_import(d->parser)&&!html_import_changed(d->parser)&&d->dom_revision==revision,
        "native field and link exports are not duplicate mutations");
    check(eval(d,"document.write('C</div><body data-late=parser><style>#target{color:rgb(4,5,6)}</style><p id=new>new</p><script>window.parsedScript=1;</script>');"),
        "new actual tokens and inline script");
    settle(top,30);web_layout(d,800,600);
    check(d->dom_revision>revision&&target==find_id(d->root,"target")&&target->first&&
        target->first->textlen==6&&!memcmp(target->first->text,"native",6),"actual input keeps native identity and text export");
    check(node_attr(d->body,"data-native")&&!strcmp(node_attr(d->body,"data-native"),"kept")&&
        node_attr(d->body,"data-late")&&!strcmp(node_attr(d->body,"data-late"),"parser"),"native and parser attributes both survive");
    node_t *tail=find_id(d->root,"tail");
    check(tail&&tail->first&&tail->first->textlen==2&&!memcmp(tail->first->text,"BC",2)&&find_id(d->root,"new"),
        "real parser merged text and new tree are visible");
    check(target->style&&target->style->color==0xff040506u&&d->profile.rescans>rescans,"actual style input still triggers resource scan");
    check(holder&&holder->template_content&&holder->template_content->first==template_child&&
        template_child&&template_child->owner==holder->template_content->owner,"template forest identity and owner remain coherent");
    check(eval(d,"if(window.parsedScript!==1)throw Error('inline script not executed');document.close();"),"inline script executed before close");
    settle(top,20);check(!d->parser,"actual EOF completes stream");
    check(eval(d,"document.open();document.write('<p id=quirks>Q</p>');document.close();"),"new quirks stream input");
    settle(top,20);check(d->quirks&&find_id(d->root,"quirks")&&!d->parser,"actual compatibility-mode change is published");
    adopted_forests(d);
    if(srcdoc){
        check(eval(srcdoc,"document.open();document.write('<p id=srcdoc-stable>S</p>');document.close();"),"srcdoc missing DOCTYPE stream input");
        settle(top,20);
        check(srcdoc->html_srcdoc&&srcdoc->document_mode==0&&!srcdoc->quirks&&!srcdoc->parser&&find_id(srcdoc->root,"srcdoc-stable"),
            "srcdoc stays no-quirks after open write close without DOCTYPE");
        check(eval(srcdoc,"document.open();document.write('<!doctype wrong><p id=srcdoc-doctype>D</p>');document.close();"),"srcdoc legacy DOCTYPE stream input");
        settle(top,20);node_t *doctype=srcdoc->root?srcdoc->root->first:NULL;
        check(srcdoc->html_srcdoc&&srcdoc->document_mode==0&&!srcdoc->quirks&&!srcdoc->parser&&find_id(srcdoc->root,"srcdoc-doctype")&&
            doctype&&doctype->type==N_DOCTYPE&&!strcmp(doctype->name,"wrong"),
            "srcdoc retains DOCTYPE and no-quirks after stream replacement");
    }
cleanup:web_free(top);
done:
    check(errors==0,"no unexpected JS errors");
    printf("parserdedup: %u checks, %u failures\n",checks,failures);return failures?1:0;
}
