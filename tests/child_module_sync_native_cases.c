#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>
#include <limits.h>
#include "quickjs.h"
#define strcasecmp _stricmp
#define strncasecmp _strnicmp
#define strlcpy(d,s,n) snprintf(d,n,"%s",s)
#define HTTP_URL_MAX 16384u
#define WEB_RESPONSE_HEADERS_MAX ((size_t)UINT32_MAX)
#define JS_SOURCE_LIMIT ((size_t)UINT32_MAX)
#define WEBNET_TIMEOUT_MS 30000u
#define MIN(a,b) ((a)<(b)?(a):(b))
enum {WEB_RESOURCE_SCRIPT,WEB_RESOURCE_MODULE,WEB_RESOURCE_CSS,WEB_RESOURCE_IMAGE,WEB_RESOURCE_FETCH,WEB_RESOURCE_FRAME};
enum webnet_kind {WEBNET_NAVIGATION,WEBNET_CLASSIC,WEBNET_MODULE,WEBNET_RESOURCE,WEBNET_FETCH};
enum webnet_credentials {WEBNET_CREDENTIALS_OMIT,WEBNET_CREDENTIALS_SAME_ORIGIN,WEBNET_CREDENTIALS_INCLUDE};
enum webnet_cache {WEBNET_CACHE_DEFAULT};
enum {P_SCRIPT};
/* PRODUCT_TYPES */
typedef struct web_doc web_doc;
typedef struct node {web_doc *owner;bool connected;} node_t;
struct web_frame {web_doc *owner,*document;node_t *element;bool detached;};
struct web_host {void *opaque;bool (*sync_load)(void*,const char*,int,struct web_response*);bool (*sync_request)(void*,const struct web_request*,struct web_response*);};
struct web_doc {struct web_js_state *js;node_t *root,*frame_element;web_doc *frame_parent;struct web_frame *frame_container;bool live;const char *url,*effective,*base;};
struct web_js_state {JSContext *ctx;JSRuntime *rt;web_doc *doc;struct web_host host;struct js_module *modules;struct web_js_state *runtime_previous;JSValue hooks;const char *module_entry_url;bool disabled;int running,compiling;uint64_t task_deadline,task_native_wait_ms,compile_wait_ms;unsigned resource_failures;};
struct js_pending {struct web_response response;int kind;char *url;uint64_t deadline;};
typedef void webnet;
struct webnet_response {int status;const char *final_url,*headers,*error;const void *body;size_t body_len;};
struct gui_event {int unused;};
struct http_resp {const char *headers;};
struct job {struct {int credentials;} wire;bool origin_tainted;const char *origin;char error[160];};
static const char *http_response_headers(const struct http_resp *r){return r->headers;}
static bool fail(struct job *j,const char *s){snprintf(j->error,sizeof j->error,"%s",s);return false;}
/* PRODUCT_CORS */
static web_doc *doc,*replacement,*retire_doc;
static struct web_frame *replace_frame;
static uint64_t generation=9,clock_ms;
static bool native_wait,pending_navigation,quit,sync_stopped,need_paint,console_dirty,console_open;
static webnet *network;
static void *w;
static int mode,submits,cancels,legacy_calls,checks,errors,snapshots;
static uint64_t queued_generation;
static char queued_url[HTTP_URL_MAX],queued_origin[HTTP_URL_MAX];
static int queued_kind,queued_credentials;
static void (*completion)(webnet*,uint64_t,uint64_t,const struct webnet_response*,void*);
static void *completion_data;
static void check(bool value,const char *name){checks++;if(!value){errors++;printf("FAIL %s\n",name);}}
static const char *web_url(web_doc *d){return d->url;}
static const char *web_effective_url(web_doc *d){return d->effective;}
static bool doc_node_connected(node_t *n){return n&&n->connected;}
static uint64_t uptime_ms(void){return clock_ms;}
static size_t webnet_response_limit(enum webnet_kind k){return UINT32_MAX;}
static const char *web_response_headers(const struct web_response *r){return r->headers_full?r->headers_full:r->headers;}
static void web_response_free(struct web_response *r){free(r->body);free(r->headers_full);r->body=r->headers_full=NULL;}
static void prepare_native_snapshot(bool force){snapshots++;check(native_wait,"native-wait-before-snapshot");}
static void redraw(void){}
static void handle_native_wait(const struct gui_event *e){}
static int webnet_timeout(webnet *net,uint64_t now){return 0;}
static int win_event(void *window,struct gui_event *e,int timeout){return 0;}
static void webnet_cancel(webnet *net,uint64_t id){cancels++;completion=NULL;}
static uint64_t webnet_submit(webnet *net,const struct webnet_request *r,void (*cb)(webnet*,uint64_t,uint64_t,const struct webnet_response*,void*),void *data){
    submits++;check(r->kind==WEBNET_MODULE,"actual-module-network-kind");check(r->credentials==WEBNET_CREDENTIALS_SAME_ORIGIN,"module-default-credentials");
    if(mode==11)return 0;
    snprintf(queued_url,sizeof queued_url,"%s",r->url);snprintf(queued_origin,sizeof queued_origin,"%s",r->origin);
    queued_generation=r->generation;queued_kind=r->kind;queued_credentials=r->credentials;completion=cb;completion_data=data;return 1;
}
static void webnet_pump(webnet *net,uint64_t now){
    clock_ms+=175;
    const char *source=strstr(queued_url,"dep.mjs")?"export const v=42;":"import{v}from'./dep.mjs';globalThis.answer=v;globalThis.realmOK=globalThis.tag==='child';globalThis.meta=import.meta.url;export const result=v;";
    const char *final=strstr(queued_url,"start.mjs")?"https://cdn.test/final/main.mjs":queued_url;
    const char *headers="Content-Type: text/javascript\r\nAccess-Control-Allow-Origin: https://child.test\r\n";
    if(mode==1)headers="Content-Type: text/javascript\r\nAccess-Control-Allow-Origin: https://top.test\r\n";
    if(mode==2)headers="Content-Type: text/plain\r\nAccess-Control-Allow-Origin: https://child.test\r\n";
    if(mode==3)final="file:///data/private.mjs";
    if(mode==4)doc=replacement;
    if(mode==5)generation++;
    if(mode==6)pending_navigation=true;
    if(mode==7||mode==9)retire_doc->live=false;
    if(mode==8)replace_frame->document=replacement;
    if(mode==10)retire_doc->root=NULL;
    char serialized[160];snprintf(serialized,sizeof serialized,"%.*s",(int)(strchr(queued_origin+8,'/')-(queued_origin)),queued_origin);
    struct job j={.wire.credentials=queued_credentials,.origin=serialized};struct http_resp hr={headers};
    bool allowed=cors_allowed(&j,&hr);
    struct webnet_response r={.status=200,.final_url=final,.headers=headers,.body=source,.body_len=strlen(source),.error=allowed?"":j.error};
    completion(net,1,queued_generation,&r,completion_data);completion=NULL;
}
/* PRODUCT_HOST */
static struct web_js_state *state(JSContext *ctx){return JS_GetContextOpaque(ctx);}
static bool task_context_active(const struct web_js_state *s){return s&&s->ctx&&s->doc&&s->doc->js==s&&s->doc->live&&!s->disabled;}
static int interrupt(JSRuntime *rt,void *opaque){return !task_context_active(opaque);}
static bool permitted_url(struct web_js_state *s,const char *url,bool fetch){return !strncmp(url,"https://",8);}
static bool permitted_script_url(struct web_js_state *s,const char *url){return permitted_url(s,url,false);}
static char *script_data_source(struct web_js_state *s,const char *url,bool module,size_t *len,char mime[128]){return NULL;}
static char *script_blob_source(struct web_js_state *s,const char *url,bool module,size_t *len,char mime[128]){return NULL;}
static void diagnostic_resource(struct web_js_state *s,struct js_pending *p,const char *message){}
static void diagnostic_url(const char *url,char *out,size_t n){snprintf(out,n,"%s",url);}
static void log_text(struct web_js_state *s,int level,const char *message){}
static void web_js_prepare_bytes(JSContext *ctx,size_t len){}
static bool strn_ieq(const char *a,const char *b,size_t n){return strlen(b)==n&&!strncasecmp(a,b,n);}
static JSValue compile_source(struct web_js_state *s,const char *source,size_t len,const char *url,int kind){return JS_Eval(s->ctx,source,len,url,kind|JS_EVAL_FLAG_COMPILE_ONLY);}
/* PRODUCT_LOADER */
static JSValue resolve_hook(JSContext *ctx,JSValueConst receiver,int argc,JSValueConst *argv){
    const char *name=JS_ToCString(ctx,argv[0]),*base=JS_ToCString(ctx,argv[1]);JSValue result=JS_EXCEPTION;
    if(name&&base){char out[HTTP_URL_MAX];if(!strncmp(name,"./",2)){const char *slash=strrchr(base,'/');snprintf(out,sizeof out,"%.*s%s",(int)(slash+1-base),base,name+2);}else snprintf(out,sizeof out,"%s",name);result=JS_NewString(ctx,out);}
    JS_FreeCString(ctx,name);JS_FreeCString(ctx,base);return result;
}
static bool legacy(void *opaque,const char *url,int kind,struct web_response *r){legacy_calls++;return true;}
static void setup_state(struct web_js_state *s,JSRuntime *rt,web_doc *d,node_t *root){
    memset(s,0,sizeof *s);s->ctx=JS_NewContext(rt);s->rt=rt;s->doc=d;s->running=1;s->task_deadline=5000;d->js=s;d->root=root;root->owner=d;d->live=true;
    JS_SetContextOpaque(s->ctx,s);s->host=(struct web_host){.sync_load=legacy,.sync_request=host_sync_request};
    s->hooks=JS_NewObject(s->ctx);JS_SetPropertyStr(s->ctx,s->hooks,"resolveModule",JS_NewCFunction(s->ctx,resolve_hook,"resolve",2));JS_SetPropertyStr(s->ctx,s->hooks,"moduleURL",JS_NewCFunction(s->ctx,resolve_hook,"url",2));
}
static void teardown(struct web_js_state *s){
    for(struct js_module *m=s->modules;m;){struct js_module *next=m->next;JS_FreeValue(s->ctx,m->compiled);JS_FreeValue(s->ctx,m->error);js_free(s->ctx,m->url);js_free(s->ctx,m->base);js_free(s->ctx,m->source);js_free(s->ctx,m);m=next;}
    JS_FreeValue(s->ctx,s->hooks);JS_FreeContext(s->ctx);
}
static void one_case(int which){
    mode=which;pending_navigation=quit=sync_stopped=native_wait=false;submits=legacy_calls=0;generation=9;
    JSRuntime *rt=JS_NewRuntime();JS_SetModuleLoaderFunc(rt,normalize_module,load_module,NULL);
    web_doc top={.url="https://top.test/page",.effective="https://top.test/page",.base="https://top.test/"};
    web_doc child={.url="about:blank",.effective="https://child.test/page/child.html",.base="https://child.test/assets/",.frame_parent=&top};
    web_doc other={0};node_t tr={0},cr={0},element={.owner=&top,.connected=true};struct web_frame frame={.owner=&top,.document=&child,.element=&element};
    child.frame_container=&frame;child.frame_element=&element;
    struct web_js_state parent,s;setup_state(&parent,rt,&top,&tr);setup_state(&s,rt,&child,&cr);s.runtime_previous=&parent;
    doc=&top;replacement=&other;replace_frame=&frame;retire_doc=which==9?&top:&child;
    JSValue g=JS_GetGlobalObject(s.ctx);JS_SetPropertyStr(s.ctx,g,"tag",JS_NewString(s.ctx,"child"));JS_FreeValue(s.ctx,g);
    if(which==12)s.host.sync_request=NULL;
    JSModuleDef *m=load_module(s.ctx,"https://cdn.test/start.mjs",NULL);
    if(which==0){
        check(m!=NULL,"child-independent-module-graph-loaded");check(submits==2&&!legacy_calls,"child-dependency-origin-aware-only");
        check(!strcmp(queued_origin,child.effective),"inherited-child-origin-not-top-or-base");check(!strcmp(queued_url,"https://cdn.test/final/dep.mjs"),"redirected-module-relative-base");
        check(s.task_native_wait_ms==350&&parent.task_native_wait_ms==350&&s.task_deadline==5350&&parent.task_deadline==5350,"nested-native-wait-excluded-once");
        if(m){JSValue result=JS_EvalFunction(s.ctx,JS_DupValue(s.ctx,s.modules->next->compiled));check(!JS_IsException(result),"actual-child-module-evaluation");JS_FreeValue(s.ctx,result);JSContext *ctx;while(JS_IsJobPending(rt))check(JS_ExecutePendingJob(rt,&ctx)>=0,"module-job");}
        g=JS_GetGlobalObject(s.ctx);JSValue answer=JS_GetPropertyStr(s.ctx,g,"answer"),realm=JS_GetPropertyStr(s.ctx,g,"realmOK"),meta=JS_GetPropertyStr(s.ctx,g,"meta");int n=0;JS_ToInt32(s.ctx,&n,answer);const char *text=JS_ToCString(s.ctx,meta);
        check(n==42&&JS_ToBool(s.ctx,realm)==1,"child-realm-evaluated-not-top");check(text&&!strcmp(text,"https://cdn.test/final/main.mjs"),"import-meta-redirect-base");JS_FreeCString(s.ctx,text);JS_FreeValue(s.ctx,answer);JS_FreeValue(s.ctx,realm);JS_FreeValue(s.ctx,meta);JS_FreeValue(s.ctx,g);
    }else{
        check(m==NULL,"invalid-or-retired-module-rejected");check(s.modules==NULL,"rejected-response-not-cached");check(!legacy_calls,"child-never-uses-top-legacy-callback");
        check(JS_HasException(s.ctx),"loader-exposes-failure");JSValue e=JS_GetException(s.ctx);JS_FreeValue(s.ctx,e);
        if(which==12)check(submits==0,"origin-unaware-child-fails-before-network");
    }
    check(!native_wait,"native-wait-unwound");teardown(&s);teardown(&parent);JS_FreeRuntime(rt);
}
int main(void){
    setbuf(stdout,NULL);
    for(int i=0;i<=12;i++)one_case(i);
    printf("child-module-sync-new-boundaries: %d checks, %d failures\n",checks,errors);return errors!=0;
}
