/* Native/Lexbor import transactions, not visible-site acceptance. */
#include "html_lexbor.h"
#include "frame.h"
#include <lexbor/dom/interfaces/element.h>
#include <nocturne.h>
#include <stdio.h>

static unsigned checks,failures,errors;
static void check(bool ok,const char *name){checks++;if(!ok){failures++;printf("FAIL parserdedup %s\n",name);}}
static void log_line(void *context,int level,const char *text){(void)context;if(level>=2){errors++;printf("ERROR %s\n",text);}}
static bool checkpoint(void *context){(void)context;return true;}
static bool eval(web_doc *d,const char *source){return web_console_eval(d,source,strlen(source));}
static void settle(web_doc *top,unsigned count){for(unsigned i=0;i<count;i++){web_tick(top,uptime_ms());msleep(1);}}
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
static void adopted_forests(web_doc *d){
    sbuf source={0};sb_puts(&source,"<!doctype html><html><body>");
    for(unsigned i=0;i<70;i++){char part[64];snprintf(part,sizeof part,"<i id=n%u>x</i>",i);sb_puts(&source,part);}
    sb_puts(&source,"<section id=order><b id=o0></b><b id=o1></b><b id=o2></b><b id=o3></b></section><script>void 0</script>");
    struct html_parser *p=html_begin(d,sb_cstr(&source),source.n,"utf-8",true);sb_free(&source);
    check(p!=NULL,"forest parser");if(!p)return;
    node_t *script=NULL;int result=html_resume(p,&script);
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
    html_finish(p);
}

int main(void){
    const char *page="<!doctype html><iframe srcdoc='<p>initial frame</p>'></iframe>";
    struct web_host host={.console=log_line,.script_checkpoint=checkpoint,.js_task_budget_ms=5000};
    web_doc *top=web_live(page,strlen(page),"https://parser.test/","utf-8",&host);
    check(top!=NULL,"live document");if(!top)goto done;
    uint64_t deadline=uptime_ms()+10000;web_doc *d=NULL;
    do{settle(top,1);if(top->frames&&!top->frames->detached)d=top->frames->document;}while(!d&&uptime_ms()<deadline);
    check(d!=NULL,"live child browsing context");if(!d)goto cleanup;
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
cleanup:web_free(top);
done:
    check(errors==0,"no unexpected JS errors");
    printf("parserdedup: %u checks, %u failures\n",checks,failures);return failures?1:0;
}
