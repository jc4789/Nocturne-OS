#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <limits.h>
#include "quickjs.h"
#include "cssom.h"
struct web_js_state { JSContext *ctx; web_doc *doc; bool disabled; };
static int checks,failures,fail_calloc,sync_calls;
static void check(bool ok,const char*name){checks++;printf("%s %s\n",ok?"OK":"FAIL",name);if(!ok)failures++;fflush(NULL);}
bool web_js_enabled(web_doc*d){return d&&d->live;}
const char *web_effective_url(web_doc*d){return d->inherited_url?d->inherited_url:d->url;}
void doc_sync_tree(web_doc*d){sync_calls++;d->resources_dirty=false;}
void css_styling_free(struct styling*st){pv_free(&st->sheets);st->ctx=NULL;}
/* REAL_PARSER */
/* REAL_DOCUMENT_HELPERS */
static void *fault_calloc(size_t n,size_t s){if(fail_calloc&&! --fail_calloc)return NULL;return calloc(n,s);}
#define calloc fault_calloc
/* REAL_CSSOM */
#undef calloc
enum{NODE_COUNT=18}; /* fixture nodes, not a product quota */
static node_t nodes[NODE_COUNT];static struct attr attributes[NODE_COUNT][4];
static web_doc docs[2];
struct fixture_state { struct web_js_state s; JSValue wrappers[NODE_COUNT]; };
static struct fixture_state contexts[2];static JSClassID node_class;
static int node_index(node_t*n){for(int i=0;i<NODE_COUNT;i++)if(n==&nodes[i])return i;return -1;}
static JSValue wrap(struct web_js_state*s,node_t*n){
    if(!n)return JS_NULL;
    int i=node_index(n);if(i<0)return JS_ThrowInternalError(s->ctx,"Unknown fixture node");
    struct fixture_state*f=JS_GetContextOpaque(s->ctx);
    if(!JS_IsUndefined(f->wrappers[i]))return JS_DupValue(s->ctx,f->wrappers[i]);
    const char*name=n->type==N_DOC?"Document":n->shadow_host?"ShadowRoot":n->tag==T_link?"HTMLLinkElement":"HTMLStyleElement";
    JSValue global=JS_GetGlobalObject(s->ctx),ctor=JS_GetPropertyStr(s->ctx,global,name),proto=JS_GetPropertyStr(s->ctx,ctor,"prototype");
    JSValue result=JS_NewObjectProtoClass(s->ctx,proto,node_class);JS_SetOpaque(result,n);
    JS_FreeValue(s->ctx,proto);JS_FreeValue(s->ctx,ctor);JS_FreeValue(s->ctx,global);
    f->wrappers[i]=JS_DupValue(s->ctx,result);return result;
}
static JSValue oom(JSContext*ctx){return JS_ThrowOutOfMemory(ctx);}
/* REAL_CSSOM_BRIDGE */
static JSValue raw(JSContext*ctx,JSValueConst this_val,int argc,JSValueConst*argv){
    struct fixture_state*f=JS_GetContextOpaque(ctx);(void)this_val;
    if(argc<2)return JS_ThrowTypeError(ctx,"Native receiver required");
    const char*op=JS_ToCString(ctx,argv[0]);if(!op)return JS_EXCEPTION;
    node_t*n=JS_GetOpaque2(ctx,argv[1],node_class);JSValue result=JS_EXCEPTION;
    if(n&&!strcmp(op,"cssom"))result=cssom_dom(&f->s,n,argc,argv);
    else if(n&&!strcmp(op,"styleDisabled")){
        if(argc>2){n->style_disabled=JS_ToBool(ctx,argv[2]);n->style_disabled_set=true;}
        result=JS_NewBool(ctx,cssom_owner_disabled(n));
    }
    JS_FreeCString(ctx,op);return result;
}
static void attr(int id,const char*name,const char*value){
    node_t*n=&nodes[id];int i=0;while(i<n->nattrs&&strcmp(attributes[id][i].raw,name))i++;
    if(i==n->nattrs)n->nattrs++;
    attributes[id][i]=(struct attr){.raw=name,.local=name,.value=value};n->attrs=attributes[id];
}
static JSValue fixture(JSContext*ctx,JSValueConst this_val,int argc,JSValueConst*argv){
    struct fixture_state*f=JS_GetContextOpaque(ctx);(void)this_val;
    const char*op=argc?JS_ToCString(ctx,argv[0]):NULL;if(!op)return JS_EXCEPTION;
    JSValue result=JS_UNDEFINED;
    if(!strcmp(op,"document"))result=wrap(&f->s,f->s.doc->root);
    else if(!strcmp(op,"parentDoc"))result=wrap(&f->s,&nodes[0]);
    else if(!strcmp(op,"style"))result=wrap(&f->s,&nodes[1]);
    else if(!strcmp(op,"link"))result=wrap(&f->s,&nodes[3]);
    else if(!strcmp(op,"cross"))result=wrap(&f->s,&nodes[5]);
    else if(!strcmp(op,"shadow"))result=wrap(&f->s,&nodes[7]);
    else if(!strcmp(op,"data"))result=wrap(&f->s,&nodes[16]);
    else if(!strcmp(op,"applies"))result=JS_NewBool(ctx,link_sheet_applies(&nodes[3]));
    else if(!strcmp(op,"tag")||!strcmp(op,"attr")) {
        node_t*n=argc>1?JS_GetOpaque2(ctx,argv[1],node_class):NULL;
        if(!n)result=JS_EXCEPTION;
        else if(!strcmp(op,"tag"))result=JS_NewString(ctx,n->type==N_ELEM&&n->tag==T_style?"style":n->type==N_ELEM&&n->tag==T_link?"link":"other");
        else {const char*key=argc>2?JS_ToCString(ctx,argv[2]):NULL;const char*v=key?node_attr(n,key):NULL;result=v?JS_NewString(ctx,v):JS_NULL;JS_FreeCString(ctx,key);}
    } else if(!strcmp(op,"detach")) {
        nodes[0].first=&nodes[3];nodes[3].prev=NULL;nodes[1].parent=nodes[1].next=nodes[1].prev=NULL;cssom_style_lifecycle(&nodes[1]);docs[0].dom_revision++;
    } else if(!strcmp(op,"reinsert")) {
        nodes[1].parent=&nodes[0];nodes[1].next=nodes[0].first;nodes[0].first->prev=&nodes[1];nodes[0].first=&nodes[1];docs[0].dom_revision++;
    } else if(!strcmp(op,"href")) {
        attr(3,"href","/b.css");cssom_style_lifecycle(&nodes[3]);docs[0].dom_revision++;
    } else if(!strcmp(op,"text")) {
        nodes[2].text=".changed {height:8px}";nodes[2].textlen=strlen(nodes[2].text);cssom_style_text_changed(&nodes[2]);docs[0].dom_revision++;
    } else if(!strcmp(op,"pendingLoad")) {
        struct cached_css*c=docs[0].css_cache.v[1];c->done=true;c->body=".late {color:green}";c->n=strlen(c->body);docs[0].resources_dirty=true;
    } else if(!strcmp(op,"fault"))fail_calloc=1;
    else if(!strcmp(op,"retire"))docs[0].live=false;
    else if(!strcmp(op,"restore"))docs[0].live=true;
    JS_FreeCString(ctx,op);return result;
}
static JSValue jscheck(JSContext*ctx,JSValueConst this_val,int argc,JSValueConst*argv){
    (void)this_val;const char*name=argc>1?JS_ToCString(ctx,argv[1]):NULL;
    check(argc&&JS_ToBool(ctx,argv[0])>0,name?name:"unnamed");JS_FreeCString(ctx,name);return JS_UNDEFINED;
}
