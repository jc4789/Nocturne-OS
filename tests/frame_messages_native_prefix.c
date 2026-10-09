/* Supporting two-context integration of actual messaging/clone/frame bindings.
 * DOM/token/host surroundings are fixtures; this is not an OS/site test. */
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <limits.h>
#include "quickjs.h"
struct web_js_state;
typedef struct web_doc web_doc;
typedef struct node {web_doc *owner,*document;JSValue wrapper;} node_t;
struct web_doc {struct web_js_state *js;bool live;node_t *root,*window_token;web_doc *parent;const char *origin,*url;};
struct js_posted_task {struct js_posted_task *next;JSValue fn;uint32_t id;};
struct js_frame_proxy {struct js_frame_proxy *next;node_t *token;JSValue proxy;};
struct web_js_state {JSContext *ctx;web_doc *doc;bool disabled;struct web_js_state *runtime_owner,*runtime_active;struct js_frame_proxy *frame_proxies;JSValue hooks;struct js_posted_task *posted,*last_posted;unsigned posted_count;uint32_t next_posted;};
static JSClassID node_class;
static struct web_js_state *state(JSContext *ctx){return JS_GetContextOpaque(ctx);}
static bool task_context_active(const struct web_js_state *s){return s&&s->ctx&&s->doc&&s->doc->js==s&&s->doc->live&&!s->disabled;}
static JSValue wrap(struct web_js_state *s,node_t *n){return n?JS_DupValue(s->ctx,n->wrapper):JS_NULL;}
static node_t *unwrap(JSContext *ctx,JSValueConst value){return JS_GetOpaque2(ctx,value,node_class);}
static node_t *frame_window_token(web_doc *d){return d->window_token;}
static web_doc *frame_window_document(node_t *n){return n?n->document:NULL;}
static const char *web_effective_url(web_doc *d){return d->origin;}
static bool make_origin(const char *url,char *out,size_t n){snprintf(out,n,"%s",url);return strcmp(url,"null")!=0;}
static bool web_frame_same_origin(web_doc *a,web_doc *b){return a==b||(!strcmp(a->origin,b->origin)&&strcmp(a->origin,"null"));}
static JSValue custom_element_hook(struct web_js_state *s,const char *key,int argc,JSValueConst *argv){JSValue fn=JS_GetPropertyStr(s->ctx,s->hooks,key);JSValue result=JS_Call(s->ctx,fn,s->hooks,argc,argv);JS_FreeValue(s->ctx,fn);return result;}
/* NATIVE_MESSAGE_FUNCTIONS */
static JSValue host_frame(JSContext *ctx,JSValueConst receiver,int argc,JSValueConst *argv){
    struct web_js_state *s=state(ctx),*caller=s->runtime_owner->runtime_active?s->runtime_owner->runtime_active:s;
    const char *op=argc?JS_ToCString(ctx,argv[0]):NULL;if(!op)return JS_EXCEPTION;JSValue result=JS_UNDEFINED;
    if(!strcmp(op,"messageBrand")){bool brand=false;if(argc>1)frame_message_receiver(s,argv[1],&brand);result=JS_NewBool(ctx,brand);}
    else if(!strcmp(op,"message"))result=frame_message_send(ctx,s,caller,argc,argv);
    else{
        node_t *token=argc>1&&!JS_IsNull(argv[1])?unwrap(ctx,argv[1]):NULL;
        web_doc *target=token?frame_window_document(token):s->doc;
        if(!strcmp(op,"self"))result=wrap(s,s->doc->window_token);
        else if(!strcmp(op,"parent")||!strcmp(op,"top"))result=wrap(s,target&&target->parent?target->parent->window_token:target->window_token);
        else if(!strcmp(op,"proxyGet")||!strcmp(op,"proxySet")){
            struct js_frame_proxy *p=s->runtime_owner->frame_proxies;while(p&&p->token!=token)p=p->next;
            if(p)result=JS_DupValue(ctx,p->proxy);
            else if(!strcmp(op,"proxyGet"))result=JS_NULL;
            else{p=js_mallocz(ctx,sizeof *p);p->token=token;p->proxy=JS_DupValue(ctx,argv[2]);p->next=s->runtime_owner->frame_proxies;s->runtime_owner->frame_proxies=p;result=JS_DupValue(ctx,p->proxy);}
        }
        else if(!strcmp(op,"closed"))result=JS_NewBool(ctx,!target||!target->live);
        else if(!strcmp(op,"length"))result=JS_NewInt32(ctx,0);
        else if(!strcmp(op,"frameElement"))result=JS_NULL;
        else if(!strcmp(op,"messageMethod")){
            /* NATIVE_MESSAGE_METHOD */
        }
        else if(!strcmp(op,"get")){
            if(!target||!web_frame_same_origin(caller->doc,target))result=JS_ThrowTypeError(ctx,"SecurityError: cross-origin Window access is forbidden");
            else{const char *key=JS_ToCString(ctx,argv[2]);JSValue global=JS_GetGlobalObject(target->js->ctx);result=JS_GetPropertyStr(ctx,global,key);JS_FreeValue(ctx,global);JS_FreeCString(ctx,key);}
        }
        else result=JS_ThrowTypeError(ctx,"Unsupported fixture frame operation");
    }
out:JS_FreeCString(ctx,op);return result;
}
static JSValue host_origin(JSContext *ctx,JSValueConst receiver,int argc,JSValueConst *argv){return JS_NewString(ctx,state(ctx)->doc->origin);}
static JSValue host_url(JSContext *ctx,JSValueConst receiver,int argc,JSValueConst *argv){return JS_NewString(ctx,state(ctx)->doc->url);}
static JSValue host_class(JSContext *ctx,JSValueConst receiver,int argc,JSValueConst *argv){return JS_NewUint32(ctx,argc?JS_GetClassID(argv[0]):0);}
static JSValue host_detach(JSContext *ctx,JSValueConst receiver,int argc,JSValueConst *argv){if(argc)JS_DetachArrayBuffer(ctx,argv[0]);return JS_UNDEFINED;}
static JSValue host_post(JSContext *ctx,JSValueConst receiver,int argc,JSValueConst *argv){
    struct web_js_state *s=state(ctx);if(!task_context_active(s)||!argc||!JS_IsFunction(ctx,argv[0]))return JS_ThrowTypeError(ctx,"Inactive callback");
    if(s->posted_count>=4096)return JS_ThrowRangeError(ctx,"Queue full");struct js_posted_task *p=js_malloc(ctx,sizeof *p);if(!p)return JS_EXCEPTION;
    p->fn=JS_DupValue(ctx,argv[0]);p->next=NULL;p->id=++s->next_posted;if(s->last_posted)s->last_posted->next=p;else s->posted=p;s->last_posted=p;s->posted_count++;return JS_NewUint32(ctx,p->id);
}
static JSValue host_cancel(JSContext *ctx,JSValueConst receiver,int argc,JSValueConst *argv){return JS_UNDEFINED;}
static void web_js_prepare_bytes(JSContext *ctx,size_t n){(void)ctx;(void)n;}
/* NATIVE_UTF8_FUNCTIONS */
static void install(struct web_js_state *s){
    JS_SetContextOpaque(s->ctx,s);JSValue host=JS_NewObject(s->ctx),global=JS_GetGlobalObject(s->ctx);
    JS_SetPropertyStr(s->ctx,host,"frame",JS_NewCFunction(s->ctx,host_frame,"frame",2));
    JS_SetPropertyStr(s->ctx,host,"origin",JS_NewCFunction(s->ctx,host_origin,"origin",0));
    JS_SetPropertyStr(s->ctx,host,"url",JS_NewCFunction(s->ctx,host_url,"url",0));
    JS_SetPropertyStr(s->ctx,host,"classID",JS_NewCFunction(s->ctx,host_class,"classID",1));
    JS_SetPropertyStr(s->ctx,host,"detach",JS_NewCFunction(s->ctx,host_detach,"detach",1));
    JS_SetPropertyStr(s->ctx,host,"postTask",JS_NewCFunction(s->ctx,host_post,"postTask",1));
    JS_SetPropertyStr(s->ctx,host,"cancelPost",JS_NewCFunction(s->ctx,host_cancel,"cancelPost",1));
    JS_SetPropertyStr(s->ctx,host,"decodeUTF8",JS_NewCFunction(s->ctx,isolated_decode_utf8,"decodeUTF8",5));
    JS_SetPropertyStr(s->ctx,global,"fixtureHost",host);JS_FreeValue(s->ctx,global);
}
