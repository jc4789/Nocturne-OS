#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <limits.h>
#include "web.h"
#include "webnet.h"
#include "js_worker_wire.h"
typedef int JSContext;
typedef int JSValue;
typedef struct web_workers web_workers;
typedef bool (*web_worker_loader)(void *,uint32_t,uint32_t,const char *,int);
struct event;
struct web_doc {const char *url,*inherited_url;};
enum {P_WORKER};
struct js_pending {char *url;uint64_t id;uint32_t worker,worker_request;int worker_kind;bool done;struct web_response response;};
struct web_js_state {web_doc *doc;JSContext *ctx;bool disabled,running;struct web_host host;};
static struct js_pending *pending;
static bool fail_pending,blob_available=true;
static struct js_pending *pending_new(struct web_js_state *s,int kind,const char *url){(void)s;(void)kind;if(fail_pending)return NULL;pending=calloc(1,sizeof *pending);pending->url=strdup(url);return pending;}
static void pending_error(struct js_pending *p,const char *error){p->done=true;snprintf(p->response.error,sizeof p->response.error,"%s",error);}
static bool JS_HasException(JSContext *ctx){(void)ctx;return false;}
static JSValue JS_GetException(JSContext *ctx){(void)ctx;return 0;}
static void JS_FreeValue(JSContext *ctx,JSValue value){(void)ctx;(void)value;}
static char *script_blob_source(struct web_js_state *s,const char *url,bool module,size_t *length,char mime[128]){(void)s;(void)url;(void)module;if(!blob_available)return NULL;*length=1;snprintf(mime,128,"text/javascript");return strdup("0");}
static uint64_t clock_now=1000;
uint64_t uptime_ms(void){return clock_now;}
struct job {struct {uint64_t deadline;} wire;char error[160];};
static bool fail(struct job *j,const char *message){snprintf(j->error,sizeof j->error,"%s",message);return false;}
/* PRODUCT_TRANSFER */
static webnet *network;
static web_doc *doc;
static bool quit;
static uint64_t generation=17;
static struct transfer *transfers;
static struct webnet_request captured;
static char captured_origin[512];
static unsigned submissions;
static void resource_completed(webnet *net,uint64_t id,uint64_t gen,const struct webnet_response *response,void *opaque){(void)net;(void)id;(void)gen;(void)response;(void)opaque;}
uint64_t webnet_submit(webnet *net,const struct webnet_request *request,webnet_callback cb,void *opaque){(void)net;(void)cb;(void)opaque;submissions++;captured=*request;snprintf(captured_origin,sizeof captured_origin,"%s",request->origin);captured.origin=captured_origin;return submissions;}
const char *web_url(web_doc *document){return document->url;}
/* PRODUCT_FUNCTIONS */
/* PRODUCT_QUEUE_TYPES */
/* PRODUCT_QUEUE */
static unsigned checks,failed;
#define CHECK(name,test) do {checks++;if(!(test)){failed++;printf("FAIL %s\n",name);}} while(0)
static void cleanup(void){if(pending){free(pending->url);free(pending->response.body);free(pending);pending=NULL;}while(transfers){struct transfer *t=transfers;transfers=t->next;free(t);}captured=(struct webnet_request){0};captured_origin[0]=0;submissions=0;}
int main(void){
    web_doc top={.url="https://top.test/page"},child={.url="https://child.test/frame"};doc=&top;
    struct web_js_state state={.doc=&child,.host={.request=host_request}};
    CHECK("different top origin does not replace child worker creator",worker_load(&state,1,0,"https://child.test/worker.js",0)&&submissions==1&&!strcmp(captured.origin,child.url)&&captured.same_origin&&captured.credentials==1);
    char a[256],b[256];CHECK("transport receives same creator origin as validated worker",make_origin(captured.origin,a,sizeof a)&&make_origin(captured.url,b,sizeof b)&&!strcmp(a,b));cleanup();
    child.url="about:blank";child.inherited_url="https://child.test/frame";
    CHECK("inherited about blank worker initiator is effective creator",worker_load(&state,2,0,"https://child.test/worker.js",0)&&submissions==1&&!strcmp(captured.origin,child.inherited_url));cleanup();
    child.url="about:srcdoc";
    CHECK("srcdoc retains inherited creator rather than about URL",worker_load(&state,3,0,"https://child.test/worker.js",0)&&submissions==1&&!strcmp(captured.origin,child.inherited_url));cleanup();
    CHECK("actual foreign startup still fails before host submission",worker_load(&state,4,0,"https://foreign.test/worker.js",0)&&submissions==0&&pending&&pending->done&&pending->response.error[0]);cleanup();
    CHECK("worker import keeps child credentials and initiating origin",worker_load(&state,3,7,"https://static.test/library.js",1)&&submissions==1&&!strcmp(captured.origin,child.inherited_url)&&!captured.same_origin&&captured.kind==WEBNET_CLASSIC&&captured.credentials==1);cleanup();
    CHECK("worker fetch CORS transport receives child origin",worker_load(&state,3,8,"https://api.test/data",2)&&submissions==1&&!strcmp(captured.origin,child.inherited_url)&&!captured.same_origin&&captured.kind==WEBNET_FETCH&&captured.credentials==1);cleanup();
    state.running=true;
    CHECK("creator blob startup retains bytes without network origin fallback",worker_load(&state,5,0,"blob:https://child.test/id",0)&&submissions==0&&pending->done&&pending->response.body_len==1&&pending->response.body);cleanup();
    blob_available=false;
    CHECK("unavailable blob still fails closed without network",worker_load(&state,6,0,"blob:https://foreign.test/unavailable",0)&&submissions==0&&pending->done&&pending->response.error[0]);cleanup();
    state.disabled=true;
    CHECK("retired creator never dispatches worker request",!worker_load(&state,7,0,"https://child.test/worker.js",0)&&submissions==0&&pending==NULL);state.disabled=false;
    fail_pending=true;
    CHECK("pending allocation failure cannot substitute top initiator",!worker_load(&state,8,0,"https://child.test/worker.js",0)&&submissions==0&&pending==NULL);fail_pending=false;
    struct job job={.wire={.deadline=clock_now+(uint64_t)WEBNET_TIMEOUT_MS}};
    CHECK("browser duration uses full uint32 representation",WEBNET_TIMEOUT_MS==UINT32_MAX);
    CHECK("long duration does not narrow to negative signed wait",remaining(&job)==INT_MAX);
    clock_now+=31000;CHECK("resource remains eligible after old 30 seconds",remaining(&job)==INT_MAX&&!job.error[0]);
    job.wire.deadline=clock_now+(uint64_t)INT_MAX+1;CHECK("signed wait saturates at INTMAX plus one",remaining(&job)==INT_MAX);
    job.wire.deadline=clock_now+37;CHECK("last finite remainder is precise",remaining(&job)==37);
    clock_now=job.wire.deadline;CHECK("required representation deadline expires rather than wraps",remaining(&job)==0&&job.error[0]);
    web_workers workers={.generation=17};struct child receiver={0};
    unsigned char *body=calloc(1,9u*1024u*1024u);
    CHECK("payload exceeds removed 8MiB quota with lazy actual allocation",body&&queue(&workers,&receiver,NJW_MESSAGE,1,0,body,9u*1024u*1024u,false)&&receiver.tx->h.bytes==9u*1024u*1024u);
    free(body);if(receiver.tx){free(receiver.tx->data);free(receiver.tx);receiver.tx=receiver.tail=NULL;}workers.queued=0;
    workers.queued=17u*1024u*1024u;
    CHECK("queue beyond old16MiB accepts another actual packet",queue(&workers,&receiver,NJW_MESSAGE,2,0,"x",1,false)&&workers.queued==17u*1024u*1024u+sizeof(struct njw_header)+1);
    if(receiver.tx){free(receiver.tx->data);free(receiver.tx);receiver.tx=receiver.tail=NULL;}workers.queued=SIZE_MAX-8;
    CHECK("size accounting overflow fails before allocation",!queue(&workers,&receiver,NJW_MESSAGE,3,0,"x",1,false)&&receiver.tx==NULL);
    CHECK("wire overrepresentation fails before reading source",!queue(&workers,&receiver,NJW_MESSAGE,4,0,NULL,(size_t)UINT32_MAX+1,false));
    CHECK("worker task and heap use requested uint32 bound",NJW_TASK_MS==UINT32_MAX&&NJW_HEAP_BYTES==UINT32_MAX&&NJW_MAX_WORKERS==UINT32_MAX);
    printf("new worker-origin/deadline/queue boundaries: %u checks / %u failed\n",checks,failed);return failed?1:0;
}
