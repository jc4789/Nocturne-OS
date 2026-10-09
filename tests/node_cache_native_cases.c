#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>
#include "quickjs.h"
enum { N_DOC=1, N_ELEM=2, N_ATTR=9, NP_NODE=0, NP_COUNT=1 };
typedef struct web_doc web_doc;
typedef struct node {
    web_doc *owner;
    struct node *parent,*shadow_host,*template_host,*attr_owner;
    int type;
} node_t;
/* PRODUCT_CACHE_TYPES */
struct web_js_state {
    web_doc *doc; JSRuntime *rt; JSContext *ctx; JSValue node_protos[NP_COUNT];
    struct js_node_ref *nodes, *node_buckets[JS_NODE_BUCKETS];
    bool node_cache_syncing,node_cache_failed;
    struct web_js_state *node_cache_next, **node_cache_prev;
};
struct web_doc { node_t *root; struct web_js_state *js; };
static JSClassID node_class;
static JSValue node_prototype(struct web_js_state *s,node_t *n){return JS_UNDEFINED;}
static bool web_frame_same_origin(web_doc *a,web_doc *b){return true;}
static void log_text(struct web_js_state *s,int level,const char *message){printf("diagnostic: %s\n",message);}
/* PRODUCT_BRAND_HELPERS */
/* PRODUCT_CACHE_LOOKUP */
static bool fail_cache_alloc;
static void *cache_alloc(JSContext *ctx,size_t size){if(fail_cache_alloc){fail_cache_alloc=false;JS_ThrowOutOfMemory(ctx);return NULL;}return js_mallocz(ctx,size);}
#define js_mallocz cache_alloc
#include "../../user/libc/web/js_nodes.h"
#undef js_mallocz
static int checked,failed;
#define CHECK(name,expr) do {checked++;if(!(expr)){printf("FAIL %s\n",name);failed++;}}while(0)
static bool identity(JSValueConst a,JSValueConst b){return JS_VALUE_GET_TAG(a)==JS_VALUE_GET_TAG(b)&&JS_VALUE_GET_PTR(a)==JS_VALUE_GET_PTR(b);}
static int eval(JSContext *ctx,const char *src){JSValue value=JS_Eval(ctx,src,strlen(src),"node-cache-case",JS_EVAL_TYPE_GLOBAL);int okay=!JS_IsException(value);if(!okay){JSValue e=JS_GetException(ctx);const char *msg=JS_ToCString(ctx,e);printf("JS error: %s\n",msg);JS_FreeCString(ctx,msg);JS_FreeValue(ctx,e);}JS_FreeValue(ctx,value);return okay;}
static void state_init(struct web_js_state *s,JSRuntime *rt,JSContext *ctx,web_doc *d){memset(s,0,sizeof *s);s->rt=rt;s->ctx=ctx;s->doc=d;s->node_protos[0]=JS_UNDEFINED;d->js=s;}
static void global(JSContext *ctx,const char *key,JSValue value){JSValue g=JS_GetGlobalObject(ctx);JS_SetPropertyStr(ctx,g,key,value);JS_FreeValue(ctx,g);}
int main(void){
    JSRuntime *rt=JS_NewRuntime();JSContext *ctx=JS_NewContext(rt);JS_NewClassID(&node_class);
    JSClassDef def={.class_name="NocturneDOMNode",.finalizer=node_finalizer,.gc_mark=node_gc_mark};
    CHECK("class",JS_NewClass(rt,node_class,&def)==0);
    struct web_js_state s;web_doc doc={0};node_t root={.owner=&doc,.type=N_DOC};doc.root=&root;state_init(&s,rt,ctx,&doc);
    JSValue document=wrap(&s,&root);global(ctx,"document",JS_DupValue(ctx,document));
    node_t live={.owner=&doc,.parent=&root,.type=N_ELEM};JSValue object=wrap(&s,&live),second=wrap(&s,&live);
    CHECK("live identity",identity(object,second));CHECK("native opaque",unwrap(ctx,object)==&live);
    JS_FreeValue(ctx,second);global(ctx,"target",object);
    CHECK("author listener setup",eval(ctx,"globalThis.listeners=new WeakMap();target.expando={value:42};listeners.set(target,()=>target.expando.value);target=null;"));
    JS_RunGC(rt);object=wrap(&s,&live);
    JSValue expando=JS_GetPropertyStr(ctx,object,"expando"),value=JS_GetPropertyStr(ctx,expando,"value");int32_t number=0;JS_ToInt32(ctx,&number,value);
    CHECK("connected expando retained",number==42);JS_FreeValue(ctx,value);JS_FreeValue(ctx,expando);global(ctx,"target",JS_DupValue(ctx,object));
    CHECK("connected listener retained",eval(ctx,"if(typeof listeners.get(target)!=='function')throw new Error('listener lost');"));
    JSValue bad=JS_NewObject(ctx);CHECK("fake brand refused",!unwrap(ctx,bad));JS_FreeValue(ctx,JS_GetException(ctx));JS_FreeValue(ctx,bad);
    live.parent=NULL;web_js_nodes_changed(&doc);global(ctx,"target",JS_NULL);JS_FreeValue(ctx,object);JS_RunGC(rt);JS_RunGC(rt);
    CHECK("detached collected",node_ref(&s,&live)==NULL);
    object=wrap(&s,&live);CHECK("recreated native identity",unwrap(ctx,object)==&live);JS_FreeValue(ctx,object);
    CHECK("unheld detached immediate finalize",node_ref(&s,&live)==NULL);
    object=wrap(&s,&live);JS_SetPropertyStr(ctx,object,"self",JS_DupValue(ctx,object));JS_FreeValue(ctx,object);JS_RunGC(rt);JS_RunGC(rt);
    CHECK("detached expando self cycle collected",!node_ref(&s,&live));
    node_t tree={.owner=&doc,.type=N_ELEM},child={.owner=&doc,.parent=&tree,.type=N_ELEM};
    JSValue ancestor=wrap(&s,&tree);object=wrap(&s,&child);JS_SetPropertyStr(ctx,object,"saved",JS_NewInt32(ctx,73));JS_FreeValue(ctx,object);JS_RunGC(rt);
    object=wrap(&s,&child);value=JS_GetPropertyStr(ctx,object,"saved");JS_ToInt32(ctx,&number,value);JS_FreeValue(ctx,value);
    CHECK("retained ancestor child state",number==73);JS_FreeValue(ctx,object);JS_FreeValue(ctx,ancestor);JS_RunGC(rt);JS_RunGC(rt);
    CHECK("unreachable forest cycle collected",!node_ref(&s,&tree)&&!node_ref(&s,&child));
    node_t shadow={.owner=&doc,.shadow_host=&live,.type=N_ELEM},attr={.owner=&doc,.attr_owner=&live,.type=N_ATTR},content={.owner=&doc,.template_host=&live,.type=N_ELEM};
    object=wrap(&s,&shadow);ancestor=wrap(&s,&live);second=wrap(&s,&attr);JSValue template=wrap(&s,&content);
    CHECK("shadow native root",node_ref(&s,&shadow)->tree_root==node_ref(&s,&live));
    CHECK("attribute native root",node_ref(&s,&attr)->tree_root==node_ref(&s,&live));
    CHECK("template native root",node_ref(&s,&content)->tree_root==node_ref(&s,&live));
    JS_FreeValue(ctx,object);JS_FreeValue(ctx,second);JS_FreeValue(ctx,template);JS_FreeValue(ctx,ancestor);JS_RunGC(rt);JS_RunGC(rt);
    CHECK("extended forest collected",!node_ref(&s,&shadow)&&!node_ref(&s,&attr)&&!node_ref(&s,&content)&&!node_ref(&s,&live));
    /* Force root creation failure after native mutation. Existing listener state
       must remain owned by the old document graph until a later update succeeds. */
    live.parent=&root;object=wrap(&s,&live);JS_SetPropertyStr(ctx,object,"kept",JS_NewInt32(ctx,99));JS_FreeValue(ctx,object);
    node_t newroot={.owner=&doc,.type=N_ELEM};live.parent=&newroot;fail_cache_alloc=true;web_js_nodes_changed(&doc);
    CHECK("failed regroup retained old edges",node_ref(&s,&live)&&node_ref(&s,&live)->tree_root==node_ref(&s,&root));
    web_js_nodes_changed(&doc);object=wrap(&s,&live);value=JS_GetPropertyStr(ctx,object,"kept");JS_ToInt32(ctx,&number,value);JS_FreeValue(ctx,value);
    CHECK("regroup retry preserves state",number==99&&node_ref(&s,&live)->tree_root==node_ref(&s,&newroot));JS_FreeValue(ctx,object);
    JS_RunGC(rt);JS_RunGC(rt);CHECK("retry forest collected",!node_ref(&s,&live)&&!node_ref(&s,&newroot));
    JSContext *other_ctx=JS_NewContext(rt);struct web_js_state other;web_doc other_doc={0};node_t other_root={.owner=&other_doc,.type=N_DOC};other_doc.root=&other_root;state_init(&other,rt,other_ctx,&other_doc);
    JSValue other_document=wrap(&other,&other_root);global(other_ctx,"document",JS_DupValue(other_ctx,other_document));
    live.parent=&root;object=wrap(&s,&live);live.owner=&other_doc;live.parent=&other_root;web_js_nodes_changed(&other_doc);
    CHECK("adopted old wrapper native owner",unwrap(ctx,object)->owner==&other_doc);
    CHECK("adopted old cache regrouped",node_ref(&s,&live)->tree_root->node==&other_root&&node_opaque(node_ref(&s,&live)->owner_object)==&other_root);
    second=wrap(&s,&live);JSValue canonical=wrap(&other,&live);
    CHECK("same origin owner realm canonical",identity(second,canonical));CHECK("cross realm opaque brand",unwrap(other_ctx,canonical)==&live);
    JS_FreeValue(ctx,second);JS_FreeValue(other_ctx,canonical);JS_FreeValue(ctx,object);
    node_cache_free(&other);other_doc.js=NULL;JS_FreeValue(other_ctx,other_document);global(other_ctx,"document",JS_NULL);JS_FreeContext(other_ctx);
    live.owner=&doc;live.parent=&root;web_js_nodes_changed(&doc);
    live.parent=&root;object=wrap(&s,&live);struct js_node_ref *escaped=JS_GetOpaque(object,node_class);node_cache_free(&s);
    CHECK("teardown cache unpublished",!s.nodes&&!node_ref(&s,&live));CHECK("escaped state severed",escaped->state==NULL);
    CHECK("escaped native brand survives",unwrap(ctx,object)==&live);JS_RunGC(rt);CHECK("escaped object survives GC",unwrap(ctx,object)==&live);
    JS_FreeValue(ctx,object);JS_FreeValue(ctx,document);global(ctx,"document",JS_NULL);global(ctx,"listeners",JS_NULL);JS_RunGC(rt);JS_RunGC(rt);
    JSValue weak_target=JS_NewObject(ctx);JSWeakRef *weak=JS_NewWeakRef(ctx,weak_target);JSValue strong=JS_GetWeakRefValue(ctx,weak);
    CHECK("weak deref returns owned ref",identity(strong,weak_target));JS_FreeValue(ctx,weak_target);CHECK("owned deref survives",JS_IsLiveObject(rt,strong));JS_FreeValue(ctx,strong);
    value=JS_GetWeakRefValue(ctx,weak);CHECK("dead weak deref undefined",JS_IsUndefined(value));JS_FreeValue(ctx,value);JS_FreeWeakRef(rt,weak);
    JS_FreeContext(ctx);JS_FreeRuntime(rt);printf("node-cache-native: %d checks, %d failures\n",checked,failed);return failed?1:0;
}
