"""One-shot new-depth/count supplement; exact product walkers/clone helpers.
External document lifecycle, controls and insertion notifications are shims.
This does not validate browser rendering or real-site acceptance.
"""
from pathlib import Path
import subprocess
import hashlib
import sys

dom = Path('user/libc/web/dom.c').read_text(encoding='utf-8')
html = Path('user/libc/web/html.c').read_text(encoding='utf-8')
util = Path('user/libc/web/util.c').read_text(encoding='utf-8')
header = Path('user/libc/web/webi.h').read_text(encoding='utf-8')
def part(text, start, end):
    at = text.index(start)
    return text[at:text.index(end, at)]

if '--indices' in sys.argv:
    # Exact production numbering/detach/form-reset functions, with only their
    # unrelated lifecycle and parser notification consumers shimmed. This is
    # supporting evidence, not a real-site or OS performance measurement.
    def function(signature):
        at = dom.index(signature)
        return dom[at:dom.index('\n}', at)+2]+'\n'
    prefix = r'''
#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>
#include <stdint.h>
enum {N_DOC,N_ELEM,N_TEXT,N_COMMENT,N_FRAGMENT};
enum {T_div,T_form,T_input,T_template};
enum {HTML_BRIDGE_CHILDREN,HTML_BRIDGE_LINKS,HTML_BRIDGE_FIELDS};
typedef struct web_doc web_doc;
typedef struct node {web_doc *owner;int type,tag,elem_index;bool foreign;
    struct node *parent,*first,*last,*prev,*next,*shadow_root,*shadow_host,*template_content,*template_host;
    struct node *parser_form_owner,*parser_form_next;
} node_t;
struct web_doc {web_doc *dom_family,*template_doc;node_t *parser_form_nodes;bool profile_enabled,template_owner;
    struct {uint64_t index_visits,index_ms;} profile;};
static unsigned notices,checks,failed;
static uint64_t uptime_ms(void){return 0;}
static void html_bridge_native_changed(node_t*n,int flags){(void)n;(void)flags;notices++;}
static void stylesheet_detach(node_t*n){(void)n;}
static void textarea_changed(node_t*n){(void)n;}
static void check(bool value,const char *name){checks++;if(!value){failed++;printf("FAIL %s\n",name);}}
'''
    product = ''.join(function(signature) for signature in (
        'static void indices(', 'node_t *doc_shadow_parent(', 'node_t *doc_node_root(',
        'static void detach(', 'void doc_parser_form_set(', 'static void reset_parser_forms('))
    cases = r'''
static void insert(node_t*p,node_t*n,node_t*before){
    n->parent=p;n->next=before;n->prev=before?before->prev:p->last;
    if(n->prev)n->prev->next=n;else p->first=n;
    if(before)before->prev=n;else p->last=n;
    if(n->type==N_ELEM)indices(p,n);
}
static bool positions(node_t*p){int index=0;for(node_t*n=p->first;n;n=n->next)if(n->type==N_ELEM&&n->elem_index!=++index)return false;return true;}
int main(void){
    web_doc d={.profile_enabled=true};node_t parent={.owner=&d,.type=N_ELEM};
    enum {COUNT=10000};node_t *items=calloc(COUNT,sizeof *items);
    for(int i=0;i<COUNT;i++){items[i].owner=&d;items[i].type=N_ELEM;insert(&parent,&items[i],NULL);}
    check(positions(&parent)&&items[COUNT-1].elem_index==COUNT,"append positions remain exact");
    check(d.profile.index_visits==2*COUNT-1,"10000 appends visit only current element and preceding element");
    node_t text={.owner=&d,.type=N_TEXT},comment={.owner=&d,.type=N_COMMENT};
    uint64_t visits=d.profile.index_visits;
    insert(&parent,&text,&items[COUNT/2]);insert(&parent,&comment,&items[COUNT/2]);
    check(d.profile.index_visits==visits&&positions(&parent),"text and comment insertion do not renumber elements");
    node_t middle={.owner=&d,.type=N_ELEM};
    insert(&parent,&middle,&items[COUNT/2]);
    check(middle.elem_index==COUNT/2+1&&items[COUNT/2].elem_index==COUNT/2+2&&positions(&parent),"middle insert skips mixed non-element siblings");
    check(items[COUNT/2-1].elem_index==COUNT/2,"unaffected prefix retains parser-published positions");
    visits=d.profile.index_visits;detach(&text);detach(&comment);
    check(d.profile.index_visits==visits&&positions(&parent),"text and comment removal do not renumber elements");
    detach(&middle);check(!middle.parent&&middle.elem_index==0&&positions(&parent),"element removal clears detached position and repairs suffix");
    visits=d.profile.index_visits;detach(&items[COUNT-1]);
    check(d.profile.index_visits==visits&&positions(&parent),"last element removal does not visit the prefix");
    detach(&items[COUNT/2]);insert(&parent,&items[COUNT/2],parent.first);
    check(items[COUNT/2].elem_index==1&&positions(&parent),"same-parent reorder keeps all positions coherent");
    node_t fragment={.owner=&d,.type=N_FRAGMENT};
    detach(&items[0]);detach(&items[1]);insert(&fragment,&items[0],NULL);insert(&fragment,&text,NULL);insert(&fragment,&items[1],NULL);
    while(fragment.first){node_t*n=fragment.first;detach(n);insert(&parent,n,NULL);check(positions(&fragment)&&positions(&parent),"mixed fragment child transfer preserves positions at each boundary");}
    /* Parser form associations only: source list and actual reset/unlink code. */
    node_t document={.owner=&d,.type=N_DOC},form={.owner=&d,.type=N_ELEM,.tag=T_form};
    node_t otherform={.owner=&d,.type=N_ELEM,.tag=T_form},outside={.owner=&d,.type=N_ELEM,.tag=T_input};
    node_t retained={.owner=&d,.type=N_ELEM,.tag=T_input},moved={.owner=&d,.type=N_ELEM,.tag=T_div};
    node_t control={.owner=&d,.type=N_ELEM,.tag=T_input};
    insert(&document,&form,NULL);insert(&document,&otherform,NULL);insert(&document,&outside,NULL);insert(&otherform,&retained,NULL);insert(&moved,&control,NULL);
    doc_parser_form_set(&outside,&form);doc_parser_form_set(&retained,&otherform);doc_parser_form_set(&control,&form);
    reset_parser_forms(&moved);
    check(!control.parser_form_owner&&!control.parser_form_next,"moved subtree control association resets");
    check(outside.parser_form_owner==&form&&retained.parser_form_owner==&otherform,"unrelated connected parser controls retain associations");
    detach(&form);reset_parser_forms(&form);
    check(!outside.parser_form_owner&&retained.parser_form_owner==&otherform,"leaf form removal resets outside foster-parented control only");
    node_t leaf={.owner=&d,.type=N_ELEM,.tag=T_div};unsigned oldnotices=notices;
    reset_parser_forms(&leaf);check(notices==oldnotices&&retained.parser_form_owner==&otherform,"ordinary leaf insertion leaves unrelated association alone");
    web_doc inert={.template_owner=true};d.template_doc=&inert;
    node_t templ={.owner=&d,.type=N_ELEM,.tag=T_template},content={.owner=&inert,.type=N_FRAGMENT,.template_host=&templ};
    node_t inertform={.owner=&inert,.type=N_ELEM,.tag=T_form},inertcontrol={.owner=&inert,.type=N_ELEM,.tag=T_input};
    templ.template_content=&content;insert(&content,&inertform,NULL);insert(&inertform,&inertcontrol,NULL);doc_parser_form_set(&inertcontrol,&inertform);
    reset_parser_forms(&templ);check(!inertcontrol.parser_form_owner&&!inert.parser_form_nodes,"template descendants reset through their own inert registration list");
    node_t host={.owner=&d,.type=N_ELEM},shadow={.owner=&d,.type=N_FRAGMENT,.shadow_host=&host},shadowform={.owner=&d,.type=N_ELEM,.tag=T_form},shadowcontrol={.owner=&d,.type=N_ELEM,.tag=T_input};
    host.shadow_root=&shadow;insert(&shadow,&shadowform,NULL);insert(&shadowform,&shadowcontrol,NULL);doc_parser_form_set(&shadowcontrol,&shadowform);
    reset_parser_forms(&host);check(!shadowcontrol.parser_form_owner&&retained.parser_form_owner==&otherform,"shadow forest descendants reset without clearing unrelated control");
    node_t bulk={.owner=&d,.type=N_ELEM},bulkform={.owner=&d,.type=N_ELEM,.tag=T_form};
    node_t *controls=calloc(COUNT,sizeof *controls);insert(&bulk,&bulkform,NULL);
    for(int i=0;i<COUNT;i++){controls[i].owner=&d;controls[i].type=N_ELEM;controls[i].tag=T_input;insert(&bulkform,&controls[i],NULL);doc_parser_form_set(&controls[i],&bulkform);}
    oldnotices=notices;reset_parser_forms(&bulk);
    check(notices-oldnotices==COUNT&&d.parser_form_nodes==&retained,"10000 related registrations unlink once each and preserve unrelated tail");
    check(!controls[0].parser_form_owner&&!controls[COUNT-1].parser_form_next,"first and last reset associations both clear");
    printf("DOM localized indices/forms: %u checks, %u failed; append index visits=%u\n",checks,failed,2*COUNT-1);
    free(controls);free(items);return failed?1:0;
}
'''
    out = Path('build/goal-20261009/dom-indices-host.c')
    out.parent.mkdir(parents=True, exist_ok=True)
    out.write_text(prefix+product+cases,encoding='utf-8',newline='\n')
    exe='build/goal-20261009/dom-indices-host.exe'
    subprocess.run(['tools/msys64/ucrt64/bin/clang.exe','-std=gnu11','-O2',str(out),'-o',exe],check=True)
    subprocess.run([exe],check=True)
    sys.exit(0)

prefix = r'''
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <limits.h>
#include <setjmp.h>
typedef struct web_doc web_doc;
typedef struct arena {struct achunk *head;size_t allocated,limit;jmp_buf *trap;} arena_t;
typedef struct sbuf {char *p;size_t n,cap;} sbuf;
typedef struct pvec {void **v;int n,cap;} pvec;
'''
prefix += part(header, '#define WEB_TAGS(X)', 'int tag_lookup')
prefix += part(header, 'enum { N_DOC', 'const char *node_attr')
prefix += r'''
struct web_doc {arena_t mem;web_doc *dom_family,*template_doc;node_t *root,*owned_nodes;bool template_owner,inert,has_shadow,resources_dirty,dirty,need_style,js_nodes_dirty;};
static unsigned total,failed;
static void check(bool ok,const char *name){total++;if(!ok){failed++;fprintf(stderr,"FAIL %s\n",name);}}
static int lower(unsigned char c){return c>='A'&&c<='Z'?c+32:c;}
static bool is_space(unsigned char c){return c==' '||c=='\t'||c=='\n'||c=='\r'||c=='\f';}
static void doc_dom_budget(web_doc *d){(void)d;}
enum {DOC_CREATE_INVALID,DOC_CREATE_QUOTA,DOC_CREATE_ALLOCATOR,DOC_CREATE_OVERFLOW};
static void doc_create_diagnostic(web_doc*d,unsigned stage,int type,size_t names,size_t bytes){(void)d;(void)stage;(void)type;(void)names;(void)bytes;}
static void doc_mutated(web_doc*d,node_t*n){(void)d;(void)n;}
static void doc_shadow_reassign(web_doc*d){(void)d;}
node_t *doc_node_create(web_doc*,int,const char*,const char*,size_t);
static web_doc *doc_inert(web_doc*,const char*,size_t,const char*);
static bool doc_node_move(web_doc*,node_t*,node_t*,node_t*);
static void doc_node_remove(web_doc*d,node_t*n){(void)d;if(!n->parent)return;node_t*p=n->parent;if(n->prev)n->prev->next=n->next;else p->first=n->next;if(n->next)n->next->prev=n->prev;else p->last=n->prev;n->parent=n->prev=n->next=NULL;}
static bool doc_attr_set_ns(web_doc*d,node_t*n,const char*ns,const char*p,const char*l,const char*v){(void)d;(void)n;(void)ns;(void)p;(void)l;(void)v;return false;}
static bool doc_node_value(web_doc*d,node_t*n,const char*v,size_t len){(void)d;n->value=malloc(len+1);if(!n->value)return false;memcpy(n->value,v,len+1);return true;}
static bool web_input_files_clone(web_doc*d,node_t*c,node_t*n){(void)d;(void)c;(void)n;return true;}
'''
code = part(util, 'struct achunk {', 'bool str_ieq')
code += part(html, 'const char *const tag_names', 'bool node_has_class')
code += html[html.index('void node_text_content'):]
code += part(dom, 'static node_t *tree_next', 'static void indices')
code += part(dom, 'static bool under', 'static bool is_slot')
code += part(dom, 'static bool is_slot', 'void doc_slot_signal')
code += part(dom, 'static const char *slot_name', 'node_t *doc_assigned_slot')
code += part(dom, 'bool doc_shadow_host_valid', 'static void manual_unlink')
code += part(dom, 'struct slot_walk', 'void doc_flat_children')
code += part(dom, 'static bool resource_subtree', 'static bool resource_ancestor')
code += part(dom, 'static bool xml_name_start', 'void doc_create_diagnostic')
code += part(dom, 'node_t *doc_node_create', 'static void attribute_cache')
code += part(dom, 'static void attribute_cache', '/* The element array remains')
code += part(dom, 'void doc_attrs_publish', 'int doc_attr_index')
code += part(dom, 'node_t *doc_attr_create', 'static bool attribute_change_normalized')
code += part(dom, 'static void stylesheet_detach', 'static void detach')
code += part(dom, 'static bool may_insert', 'static bool assignment_structure')
code += part(dom, 'static void adopt_subtree', 'bool doc_node_adopt')
code += part(dom, 'node_t *doc_template_content', 'bool doc_templates_finish')
code += dom[dom.index('static bool clone_attributes'):]
suffix = r'''
static web_doc *doc_inert(web_doc*p,const char*s,size_t n,const char*u){(void)p;(void)s;(void)n;(void)u;web_doc*d=calloc(1,sizeof *d);if(d){d->inert=true;d->root=doc_node_create(d,N_DOC,NULL,NULL,0);}return d;}
static bool doc_node_move(web_doc*d,node_t*p,node_t*c,node_t*b){if(!d||p->owner!=d||!may_insert(p,c)||b)return false;doc_node_remove(d,c);c->parent=p;c->prev=p->last;if(p->last)p->last->next=c;else p->first=c;p->last=c;return true;}
static void link(node_t*p,node_t*c){c->parent=p;c->prev=p->last;if(p->last)p->last->next=c;else p->first=c;p->last=c;}
static void free_doc(web_doc*d){for(node_t*n=d->owned_nodes;n;n=n->owned_next)free(n->value);if(d->template_doc){free_doc(d->template_doc);free(d->template_doc);}ar_free(&d->mem);}
int main(void){
    web_doc source={0},dest={0},inert={.template_owner=true,.inert=true};dest.template_doc=&inert;
    const size_t depth=20000;node_t *forest=calloc(depth,sizeof *forest);
    for(size_t i=0;i<depth;i++){forest[i].owner=&source;forest[i].type=N_ELEM;forest[i].tag=T_div;forest[i].name=forest[i].raw_name="div";if(i)link(&forest[i-1],&forest[i]);}
    size_t count=0;for(node_t*n=forest;n;n=tree_next(n,forest,true,true,true))count++;
    check(count==depth,"20000 native depth walks without truncation");
    node_t sibling={.type=N_ELEM};forest[0].next=&sibling;
    count=0;for(node_t*n=forest;n;n=tree_next(n,forest,true,true,true))count++;
    check(count==depth,"bounded subtree excludes root sibling");forest[0].next=NULL;
    check(may_insert(&forest[depth-1],&(node_t){.type=N_ELEM}),"deep insertion allowed");
    check(!may_insert(&forest[depth-1],forest),"native ancestor cycle still refused");
    node_t text={.type=N_TEXT,.text="deep",.textlen=4};link(&forest[depth-1],&text);
    sbuf content={0};node_text_content(forest,&content);check(content.n==4&&!memcmp(content.p,"deep",4),"deep textContent uses real walker");sb_free(&content);
    forest[depth-1].tag=T_img;check(resource_subtree(forest),"deep resource not missed");forest[depth-1].tag=T_div;
    forest[depth-1].stylesheet_url="deep-sheet";stylesheet_detach(forest);
    check(!forest[depth-1].stylesheet_url&&forest[depth-1].stylesheet_generation==1,"deep stylesheet detached once");
    node_t shadow={.type=N_FRAGMENT,.shadow_host=&forest[depth-1]},inside={.type=N_ELEM,.tag=T_slot,.name="slot"};forest[depth-1].shadow_root=&shadow;link(&shadow,&inside);
    check(first_named_slot(&shadow,"")==&inside,"deep shadow slot native order");
    node_t template_root={.type=N_FRAGMENT,.template_host=&forest[depth-1]},template_child={.type=N_ELEM};forest[depth-1].template_content=&template_root;link(&template_root,&template_child);
    check(has_template(forest),"deep template discovery");
    adopt_subtree(&dest,forest);
    check(forest[depth-1].owner==&dest&&inside.owner==&dest,"deep ordinary and shadow adoption");
    check(template_root.owner==&inert&&template_child.owner==&inert,"template owner stays inert on adoption");
    check(forest[depth-1].allocation_doc==NULL,"allocation ownership never rewritten");
    forest[depth-1].template_content=NULL;forest[depth-1].shadow_root=NULL;
    char name[601];memset(name,'A',600);name[0]='a';name[1]='-';name[600]=0;
    node_t *long_node=doc_node_create(&source,N_ELEM,name,NULL,0);
    check(long_node&&strlen(long_node->name)==600&&long_node->name[599]=='a'&&long_node->raw_name[599]=='A',"600-byte element name is actual arena data");
    node_t *attribute=doc_attr_create(&source,"urn:long","Prefix",name,"value");
    check(attribute&&strlen(attribute->attribute->local)==600&&!strcmp(attribute->attribute->namespace_uri,"urn:long"),"600-byte namespaced attribute preserves case and namespace");
    struct attr *attrs=calloc(1200,sizeof *attrs);for(int i=0;i<1200;i++)attrs[i]=(struct attr){.name="data-long",.raw="data-long",.local="data-long",.value=""};
    node_t original={.type=N_ELEM,.tag=T_div,.attrs=attrs,.nattrs=1200},copy={0};
    check(clone_attributes(&source,&copy,&original)&&copy.nattrs==1200&&copy.attrs!=attrs,"1200 attributes cloned without count cap");
    check(copy.attrs[1199].value!=attrs[1199].value&&!strcmp(copy.attrs[1199].value,""),"last attribute has independent lifetime and boolean value");
    /* Build a fallback-slot graph deeper than the former 400 recursion gate. */
    node_t *slots=calloc(700,sizeof *slots),host={.type=N_ELEM},sr={.type=N_FRAGMENT,.shadow_host=&host},leaf={.type=N_TEXT};host.shadow_root=&sr;
    for(int i=0;i<700;i++){slots[i].type=N_ELEM;slots[i].tag=T_slot;if(i)link(&slots[i-1],&slots[i]);else link(&sr,&slots[i]);}link(&slots[699],&leaf);
    pvec flattened={0};check(doc_slot_nodes(slots,true,&flattened)&&flattened.n==1&&flattened.v[0]==&leaf,"700-level fallback slot flatten completes");pv_free(&flattened);
    slots[699].slot_assigned_first=slots;slots[0].assigned_next=NULL;
    check(doc_slot_nodes(slots,true,&flattened)&&flattened.n==0,"cyclic virtual assignment is bounded without false success data");pv_free(&flattened);
    /* Native clone walker, with only lifecycle/notification hooks shimmed. */
    text.parent=NULL;forest[depth-1].first=forest[depth-1].last=NULL;
    forest[4999].first=forest[4999].last=NULL;
    node_t *cloned=doc_node_clone(&source,forest,true);count=0;
    for(node_t*n=cloned;n;n=tree_next(n,cloned,true,true,true))count++;
    check(cloned&&count==5000,"5000-level clone completes on dynamic correspondence stack");
    node_t *clone_shadow=doc_shadow_attach(&source,long_node,false,false,true,false,false);
    node_t *shadow_text=doc_node_create(&source,N_TEXT,NULL,"shadow",6);link(clone_shadow,shadow_text);
    node_t *shallow=doc_node_clone(&source,long_node,false);
    check(shallow&&shallow->shadow_root&&shallow->shadow_root->first&&shallow->shadow_root->first->textlen==6,"shallow clone still clones clonable shadow descendants");
    printf("dom unbounded new boundaries: %u checks / %u failed\n",total,failed);
    source.template_doc=NULL;dest.template_doc=NULL;free_doc(&source);free_doc(&inert);free(attrs);free(forest);free(slots);return failed?1:0;
}
'''
out = Path('build/goal-20261009/dom-unbounded-host.c')
out.write_text(prefix+code+suffix,encoding='utf-8',newline='\n')
print('generated SHA256',hashlib.sha256(out.read_bytes()).hexdigest(),flush=True)
exe='build/goal-20261009/dom-unbounded-host.exe'
subprocess.run(['tools/msys64/ucrt64/bin/clang.exe','-O2',str(out),'-o',exe],check=True)
subprocess.run([exe],check=True)
