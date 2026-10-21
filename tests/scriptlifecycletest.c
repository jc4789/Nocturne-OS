/* Product web_live parsing, script execution and lifecycle boundaries. */
#ifdef SCRIPT_INSERTION_HOST
/* Focused extracted-product check. The tree, preparation/execution callback,
   clock and allocator below are fixtures, not a browser or real-site test. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>
typedef int JSValue;
typedef void JSContext;
#define JS_EXCEPTION (-1)
enum {N_DOC,N_ELEM,N_FRAGMENT,N_TEXT,T_div=1,T_script,T_meta,T_template,T_head,T_iframe};
typedef struct web_doc web_doc;
typedef struct node {
    web_doc *owner;
    struct node *parent,*shadow_host,*first,*next,*shadow_root;
    unsigned type,tag;
    bool foreign,script_started,html_policy_processed;
    char identity;
    unsigned action;
} node_t;
struct js_script {node_t *node;bool ready,executed,module,external;};
struct web_js_state {
    web_doc *doc;JSContext *ctx;
    struct {bool debug_js;} host;
    uint64_t stream_generation;
    bool disabled;
};
struct web_doc {
    node_t *root,*head;bool resources_dirty;
    struct {uint64_t script_scans,script_visits,script_ms;} profile;
};
/* PRODUCT_INSERTED_ROOTS_TYPE */
static unsigned checks,failures,prepared,frames,policies,exceptions;
static unsigned allocations,frees,cancel_after,checkpoint_count;
static bool fail_allocate;
static char order[128];static size_t order_count;
static struct js_script records[64];
static node_t *nested_node,*detach_node,*late_node;
static JSValue JS_ThrowInternalError(JSContext *ctx,const char *text){exceptions++;return JS_EXCEPTION;}
static JSValue JS_ThrowOutOfMemory(JSContext *ctx){exceptions++;return JS_EXCEPTION;}
static void *js_malloc(JSContext *ctx,size_t size){
    if(fail_allocate){fail_allocate=false;JS_ThrowOutOfMemory(ctx);return NULL;}
    void *p=malloc(size);if(p)allocations++;return p;
}
static void js_free(JSContext *ctx,void *p){if(p){frees++;free(p);}}
static bool web_native_checkpoint(web_doc *doc){return !cancel_after||++checkpoint_count<cancel_after;}
static bool web_frame_element(node_t *node){return node->type==N_ELEM&&node->tag==T_iframe;}
static bool connected(struct web_js_state *s,node_t *node){
    while(node&&node!=s->doc->root)node=node->parent?node->parent:node->shadow_host;
    return node==s->doc->root;
}
static bool str_ieq(const char *a,const char *b){return !strcmp(a,b);}
static const char *node_attr(node_t *node,const char *name){
    if(node->tag!=T_meta)return NULL;
    return !strcmp(name,"http-equiv")?"content-security-policy":"default-src 'self'";
}
static node_t *node_ancestor(node_t *node,unsigned tag){while(node){if(node->tag==tag)return node;node=node->parent;}return NULL;}
static void doc_sync_tree(web_doc *doc){doc->resources_dirty=false;}
static bool html_policy_meta(web_doc *doc,const char *text,size_t length){policies++;return true;}
static uint64_t uptime_ms(void){return 0;}
static void exception(struct web_js_state *s){exceptions++;}
static void frame_sync(struct web_js_state *s,node_t *node){frames++;}
static void dynamic_inserted(struct web_js_state *s,node_t *parent,
    struct js_inserted_roots *inserted,uint64_t generation);
static bool inserted_roots_capture(struct web_js_state *s,node_t *node,struct js_inserted_roots *roots);
static void inserted_roots_release(struct web_js_state *s,struct js_inserted_roots *roots);
static void attach(node_t *parent,node_t *node){
    node->parent=parent;node->next=NULL;
    node_t **last=&parent->first;while(*last)last=&(*last)->next;*last=node;
}
static void detach(node_t *node){
    node_t **link=&node->parent->first;while(*link&&*link!=node)link=&(*link)->next;
    if(*link)*link=node->next;node->parent=NULL;node->next=NULL;
}
static struct js_script *queue_script(struct web_js_state *s,node_t *node,bool dynamic){
    node->script_started=true;prepared++;
    if(node->action==3){s->stream_generation++;return NULL;}
    struct js_script *script=&records[prepared-1];
    *script=(struct js_script){.node=node,.ready=true,.module=node->action==4,.external=node->action==5};
    return script;
}
static void run_script(struct web_js_state *s,struct js_script *script){
    script->executed=true;order[order_count++]=script->node->identity;order[order_count]=0;
    if(script->node->action==1){
        struct js_inserted_roots roots={0};uint64_t generation=s->stream_generation;
        if(inserted_roots_capture(s,nested_node,&roots)){
            attach(script->node->parent,nested_node);
            dynamic_inserted(s,script->node->parent,&roots,generation);
        }
        inserted_roots_release(s,&roots);
    }else if(script->node->action==2)detach(detach_node);
    else if(script->node->action==6)s->stream_generation++;
    else if(script->node->action==7){detach(late_node);attach(script->node->parent,late_node);}
}
/* PRODUCT_DYNAMIC_INSERTION */
static void check(const char *name,bool value){checks++;if(!value){printf("FAIL %s\n",name);failures++;}}
static void clear(struct web_js_state *s){
    prepared=frames=policies=exceptions=0;order_count=0;order[0]=0;
    s->stream_generation=1;s->disabled=false;memset(&s->doc->profile,0,sizeof s->doc->profile);
    s->doc->root->first->first=NULL;s->doc->root->first->next->first=NULL;
}
static void insert(struct web_js_state *s,node_t *parent,node_t *node){
    struct js_inserted_roots roots={0};uint64_t generation=s->stream_generation;
    if(inserted_roots_capture(s,node,&roots)){
        if(node->type==N_FRAGMENT){while(node->first){node_t *child=node->first;node->first=child->next;attach(parent,child);}}
        else attach(parent,node);
        dynamic_inserted(s,parent,&roots,generation);
    }
    inserted_roots_release(s,&roots);
}
int main(void){
    web_doc doc={0};node_t root={.owner=&doc,.type=N_DOC},head={.owner=&doc,.type=N_ELEM,.tag=T_head};
    node_t body={.owner=&doc,.type=N_ELEM,.tag=T_div};doc.root=&root;doc.head=&head;
    attach(&root,&head);attach(&root,&body);
    struct web_js_state s={.doc=&doc,.host.debug_js=true,.stream_generation=1};
    node_t ordinary[1500];memset(ordinary,0,sizeof ordinary);
    for(unsigned i=0;i<1500;i++){ordinary[i].owner=&doc;ordinary[i].type=N_ELEM;ordinary[i].tag=T_div;insert(&s,&body,&ordinary[i]);}
    check("ordinary appends visit only inserted leaves",doc.profile.script_visits==1500&&prepared==0);
    check("ordinary appends need no allocation",allocations==0&&frees==0);
    clear(&s);
    node_t a={.owner=&doc,.type=N_ELEM,.tag=T_script,.identity='A',.action=1};
    node_t b={.owner=&doc,.type=N_ELEM,.tag=T_script,.identity='B'};
    node_t c={.owner=&doc,.type=N_ELEM,.tag=T_script,.identity='C'};nested_node=&c;
    node_t fragment={.owner=&doc,.type=N_FRAGMENT};attach(&fragment,&a);attach(&fragment,&b);insert(&s,&body,&fragment);
    check("fragment reentrant append order",!strcmp(order,"ACB")&&prepared==3);
    check("fragment emptied without losing following candidate",!fragment.first&&b.script_started);
    clear(&s);
    node_t subtree={.owner=&doc,.type=N_ELEM,.tag=T_div};
    a=(node_t){.owner=&doc,.type=N_ELEM,.tag=T_script,.identity='A',.action=2};
    b=(node_t){.owner=&doc,.type=N_ELEM,.tag=T_script,.identity='B'};detach_node=&b;
    attach(&subtree,&a);attach(&subtree,&b);insert(&s,&body,&subtree);
    check("earlier inline script detaches later candidate before preparation",!strcmp(order,"A")&&prepared==1&&!b.script_started);
    clear(&s);
    a=(node_t){.owner=&doc,.type=N_ELEM,.tag=T_script,.identity='A',.action=3};
    b=(node_t){.owner=&doc,.type=N_ELEM,.tag=T_script,.identity='B'};
    fragment=(node_t){.owner=&doc,.type=N_FRAGMENT};attach(&fragment,&a);attach(&fragment,&b);insert(&s,&body,&fragment);
    check("preparation generation change stops old sibling",prepared==1&&!b.script_started&&!order_count);
    clear(&s);
    a=(node_t){.owner=&doc,.type=N_ELEM,.tag=T_script,.identity='A',.action=6};
    b=(node_t){.owner=&doc,.type=N_ELEM,.tag=T_script,.identity='B'};
    fragment=(node_t){.owner=&doc,.type=N_FRAGMENT};attach(&fragment,&a);attach(&fragment,&b);insert(&s,&body,&fragment);
    check("execution generation change stops old sibling",!strcmp(order,"A")&&prepared==1&&!b.script_started);
    clear(&s);
    a=(node_t){.owner=&doc,.type=N_ELEM,.tag=T_script,.identity='A',.action=7};
    b=(node_t){.owner=&doc,.type=N_ELEM,.tag=T_script,.identity='B'};
    c=(node_t){.owner=&doc,.type=N_ELEM,.tag=T_script,.identity='C'};late_node=&b;
    fragment=(node_t){.owner=&doc,.type=N_FRAGMENT};attach(&fragment,&a);attach(&fragment,&b);attach(&fragment,&c);insert(&s,&body,&fragment);
    check("static candidate order survives sibling reordering",!strcmp(order,"ABC")&&prepared==3);
    clear(&s);
    node_t host={.owner=&doc,.type=N_ELEM,.tag=T_div},shadow={.owner=&doc,.type=N_FRAGMENT,.shadow_host=&host};host.shadow_root=&shadow;
    a=(node_t){.owner=&doc,.type=N_ELEM,.tag=T_script,.identity='S'};
    b=(node_t){.owner=&doc,.type=N_ELEM,.tag=T_script,.identity='L'};attach(&shadow,&a);attach(&host,&b);insert(&s,&body,&host);
    check("shadow including preorder",!strcmp(order,"SL")&&prepared==2);
    clear(&s);
    node_t template={.owner=&doc,.type=N_ELEM,.tag=T_template};
    a=(node_t){.owner=&doc,.type=N_ELEM,.tag=T_script,.identity='X'};attach(&template,&a);insert(&s,&body,&template);
    check("template content remains inert",prepared==0&&!a.script_started);
    clear(&s);
    node_t meta={.owner=&doc,.type=N_ELEM,.tag=T_meta},frame={.owner=&doc,.type=N_ELEM,.tag=T_iframe};
    insert(&s,&head,&meta);insert(&s,&body,&frame);
    check("meta CSP and frame connection retained",policies==1&&meta.html_policy_processed&&frames==1);
    clear(&s);
    a=(node_t){.owner=&doc,.type=N_ELEM,.tag=T_script,.identity='T'};attach(&body,&a);
    node_t text={.owner=&doc,.type=N_TEXT};insert(&s,&a,&text);
    check("connected script child text preparation",!strcmp(order,"T")&&prepared==1);
    clear(&s);
    a=(node_t){.owner=&doc,.type=N_ELEM,.tag=T_script,.identity='T'};attach(&body,&a);
    fragment=(node_t){.owner=&doc,.type=N_FRAGMENT};insert(&s,&a,&fragment);
    check("empty fragment insertion has no children changed preparation",prepared==0&&!a.script_started);
    clear(&s);
    a=(node_t){.owner=&doc,.type=N_ELEM,.tag=T_script,.identity='M',.action=4};
    b=(node_t){.owner=&doc,.type=N_ELEM,.tag=T_script,.identity='E',.action=5};
    fragment=(node_t){.owner=&doc,.type=N_FRAGMENT};attach(&fragment,&a);attach(&fragment,&b);insert(&s,&body,&fragment);
    check("module and external scripts stay asynchronous",prepared==2&&!order_count);
    dynamic_scripts(&s,&body);check("already started scripts not prepared twice",prepared==2&&!order_count);
    clear(&s);
    node_t many[33];memset(many,0,sizeof many);fragment=(node_t){.owner=&doc,.type=N_FRAGMENT};
    for(unsigned i=0;i<33;i++){many[i].owner=&doc;many[i].type=N_ELEM;many[i].tag=T_iframe;attach(&fragment,&many[i]);}
    insert(&s,&body,&fragment);check("candidate list grows beyond inline storage",frames==33&&allocations==frees);
    clear(&s);fragment=(node_t){.owner=&doc,.type=N_FRAGMENT};
    for(unsigned i=0;i<9;i++){many[i]=(node_t){.owner=&doc,.type=N_ELEM,.tag=T_script};attach(&fragment,&many[i]);}
    fail_allocate=true;insert(&s,&body,&fragment);
    check("capture OOM leaves original fragment and does not prepare",fragment.first==&many[0]&&prepared==0&&exceptions==1&&allocations==frees);
    clear(&s);cancel_after=1;checkpoint_count=0;
    insert(&s,&body,&fragment);cancel_after=0;
    check("capture cancellation leaves original fragment",fragment.first==&many[0]&&prepared==0&&exceptions==1&&allocations==frees);
    clear(&s);node_t *deep=calloc(12000,sizeof *deep);
    for(unsigned i=0;i<12000;i++){deep[i].owner=&doc;deep[i].type=N_ELEM;deep[i].tag=i==11999?T_iframe:T_div;if(i)attach(&deep[i-1],&deep[i]);}
    insert(&s,&body,deep);check("deep candidate traversal uses no recursive native stack",frames==1&&doc.profile.script_visits==12000);free(deep);
    printf("script-insertion-host: %u checks, %u failed\n",checks,failures);return failures?1:0;
}
#else
#include <nocturne.h>
#include <web.h>
#include <stdio.h>
#include <string.h>

static int checks, failures, errors;
static bool done;
static uint64_t blocking_request, deferred_request, following_request, module_request;

static void console(void *unused, int level, const char *text) {
    if (!strncmp(text,"OK ",3)) { checks++; printf("%s\n",text); }
    else if (!strncmp(text,"FAIL ",5)) { checks++; failures++; printf("%s\n",text); }
    else if (!strcmp(text,"SCRIPT-LIFECYCLE-DONE")) done=true;
    else if (level>=2) { errors++; printf("ERROR %s\n",text); }
}
static bool request(void *unused, const struct web_request *r) {
    if (r->kind!=WEB_RESOURCE_SCRIPT && r->kind!=WEB_RESOURCE_MODULE) return false;
    if (!strcmp(r->url,"https://lifecycle.test/blocking.js")) blocking_request=r->id;
    else if (!strcmp(r->url,"https://lifecycle.test/deferred.js")) deferred_request=r->id;
    else if (!strcmp(r->url,"https://lifecycle.test/following.js")) following_request=r->id;
    else if (!strcmp(r->url,"https://lifecycle.test/load-await.js")) module_request=r->id;
    else return false;
    return true;
}
static const char blocking[]=
    "document.write('<meta id=blocking-written>');"
    "check('blocking-write-preserved',!!document.getElementById('blocking-written'));"
    "Promise.resolve().then(()=>{document.write('<meta id=blocking-microtask>');"
    "check('blocking-checkpoint-write-preserved',!!document.getElementById('blocking-microtask'));});";
static const char deferred[]=
    "{const original=document.getElementById('original'),script=document.currentScript;"
    "document.write('<p id=bad-defer>replacement</p>');"
    "check('defer-write-preserves-document',document.getElementById('original')===original&&!document.getElementById('bad-defer'));"
    "const inner=document.createElement('script');"
    "inner.textContent=\"document.write('<p id=bad-nested>replacement</p>');check('nested-write-guard',!!document.getElementById('original')&&!document.getElementById('bad-nested'));\";"
    "document.head.appendChild(inner);"
    "check('nested-restores-currentScript',document.currentScript===script);"
    "Promise.resolve().then(()=>{document.write('<p id=bad-microtask>replacement</p>');"
    "check('defer-checkpoint-write-guard',document.getElementById('original')===original&&!document.getElementById('bad-microtask'));"
    "check('defer-checkpoint-currentScript',document.currentScript===script);});}"
    "{window.__insertionOrder='';const make=text=>{const s=document.createElement('script');s.textContent=text;return s;};"
    "const fragment=document.createDocumentFragment();"
    "fragment.appendChild(make(\"__insertionOrder+='A';const nested=document.createElement('script');nested.textContent=\\\"__insertionOrder+='C';\\\";document.body.appendChild(nested);\"));"
    "fragment.appendChild(make(\"__insertionOrder+='B';\"));document.body.appendChild(fragment);"
    "check('fragment-reentrant-inline-order',__insertionOrder==='ACB'&&!fragment.firstChild);"
    "window.__insertionOrder='';const subtree=document.createElement('div');"
    "const first=make(\"__insertionOrder+='P';document.getElementById('later-inline').remove();\"),later=make(\"__insertionOrder+='Q';\");later.id='later-inline';"
    "subtree.appendChild(first);subtree.appendChild(later);document.body.appendChild(subtree);"
    "check('detached-post-connection-script-not-prepared',__insertionOrder==='P'&&!later.isConnected);"
    "document.body.appendChild(later);check('later-reinserted-inline-script-runs-once',__insertionOrder==='PQ');"
    "later.remove();document.body.appendChild(later);check('started-inline-script-not-reexecuted',__insertionOrder==='PQ');"
    "const empty=document.createElement('script');document.body.appendChild(empty);empty.appendChild(document.createTextNode(\"__insertionOrder+='T';\"));"
    "check('connected-script-child-text-synchronous',__insertionOrder==='PQT');"
    "const template=document.createElement('template');template.content.appendChild(make(\"__insertionOrder+='S';\"));document.body.appendChild(template);"
    "check('template-content-script-is-inert',__insertionOrder==='PQT');"
    "document.body.appendChild(template.content);check('template-content-connection-prepares-script',__insertionOrder==='PQTS');delete window.__insertionOrder;}";
static const char following[]=
    "check('following-defer-runs-during-await',!moduleDone&&document.readyState==='interactive');following=true;";
static const char load_module[]=
    "check('external-module-starts-before-load',!windowLoaded);"
    "await new Promise(resolve=>window.addEventListener('load',resolve,{once:true}));"
    "check('await-window-load-resumes',windowLoaded&&domReady&&moduleDone&&moduleElementLoaded);"
    "console.log('SCRIPT-LIFECYCLE-DONE');";
static const char page[]=
    "<!doctype html><head><script>"
    "function check(name,pass){console.log((pass?'OK ':'FAIL ')+name);}"
    "let moduleDone=false,following=false,domReady=false,windowLoaded=false,moduleElementLoaded=false;"
    "document.addEventListener('DOMContentLoaded',()=>{check('dom-ready-before-await-completion',following&&!moduleDone);domReady=true;});"
    "window.addEventListener('load',()=>{check('window-load-does-not-await-module-evaluation',domReady&&moduleDone&&moduleElementLoaded);windowLoaded=true;});"
    "</script><script src='/blocking.js'></script>"
    "<script defer src='/deferred.js'></script>"
    "<script type='module'>"
    "document.write('<p id=bad-module>replacement</p>');"
    "check('module-initial-write-guard',!!document.getElementById('original')&&!document.getElementById('bad-module'));"
    "await new Promise(resolve=>document.addEventListener('DOMContentLoaded',resolve,{once:true}));"
    "check('await-dom-ready-resumes',domReady&&following);moduleDone=true;"
    "</script><script defer src='/following.js'></script>"
    "<script type='module' src='/load-await.js' onload=\"check('module-element-load-before-await-completion',!windowLoaded);moduleElementLoaded=true;\"></script>"
    "</head><body><p id='original'>original document</p></body>";
static void deliver(web_doc *doc, uint64_t *pending, const char *source, const char *url) {
    if (!*pending) return;
    struct web_response response={.status=200,.body=(char *)source,.body_len=strlen(source)};
    strcpy(response.headers,"Content-Type: text/javascript\r\n");
    strcpy(response.url,url);
    uint64_t id=*pending; *pending=0;
    web_resource_loaded(doc,id,&response);
}
int main(void) {
    struct web_host host={.request=request,.console=console,.js_task_budget_ms=5000};
    web_doc *doc=web_live(page,sizeof page-1,"https://lifecycle.test/","utf-8",&host);
    if (!doc) return 1;
    uint64_t deadline=uptime_ms()+10000;
    while (!done && uptime_ms()<deadline) {
        deliver(doc,&blocking_request,blocking,"https://lifecycle.test/blocking.js");
        deliver(doc,&deferred_request,deferred,"https://lifecycle.test/deferred.js");
        deliver(doc,&following_request,following,"https://lifecycle.test/following.js");
        deliver(doc,&module_request,load_module,"https://lifecycle.test/load-await.js");
        web_tick(doc,uptime_ms()); n_sleep_ms(1);
    }
    if (!done || checks!=22 || errors) {
        printf("FAIL script lifecycle completion done=%d checks=%d errors=%d\n",done,checks,errors);
        failures++;
    }
    web_free(doc);
    printf("scriptlifecycletest: %d checks, %d failed\n",checks,failures);
    return failures?1:0;
}
#endif
