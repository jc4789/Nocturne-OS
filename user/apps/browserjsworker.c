/* Dedicated QuickJS realm in a native Nocturne child task. The only author
 * capabilities are data-only IPC to the browser; no libc/native modules. */
#include <nocturne.h>
#include <quickjs.h>
#include <js_worker_wire.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include "../libc/web/js_encoding.h"
#include "../libc/web/js_worker_runtime.inc"
struct deferred {struct deferred *next;struct njw_header h;uint8_t *data;};
struct worker {JSRuntime *rt;JSContext *ctx;JSValue hooks;uint32_t generation,request;uint64_t until;bool closed,established;struct deferred *head,*tail;size_t queued;};
static bool exact(int fd,void *p,size_t n,bool writing){size_t at=0;while(at<n){ssize_t got=writing?write(fd,(uint8_t *)p+at,n-at):read(fd,(uint8_t *)p+at,n-at);if(got<0&&errno==EINTR)continue;if(got<=0)return false;at+=(size_t)got;}return true;}
static bool send_value(struct worker *w,unsigned op,unsigned request,unsigned kind,JSValueConst value){size_t n=0;uint8_t *p=JS_WriteObject(w->ctx,&n,value,JS_WRITE_OBJ_REFERENCE);if(!p)return false;if(n>NJW_MAX_BYTES){js_free(w->ctx,p);return false;}struct njw_header h={NJW_MAGIC,op,w->generation,request,kind,(uint32_t)n};bool ok=exact(1,&h,sizeof h,true)&&exact(1,p,n,true);js_free(w->ctx,p);if(!ok)w->closed=true;return ok;}
static struct deferred *receive(struct worker *w){
    struct deferred *d=calloc(1,sizeof *d);if(!d)return NULL;
    if(!exact(0,&d->h,sizeof d->h,false)||d->h.magic!=NJW_MAGIC||!d->h.bytes||d->h.bytes>NJW_MAX_BYTES||d->h.kind||
       (d->h.op!=NJW_START&&d->h.op!=NJW_MESSAGE&&d->h.op!=NJW_LOADED)||(w->established&&d->h.generation!=w->generation)||(!w->established&&d->h.op!=NJW_START)){free(d);return NULL;}
    if(!w->established){w->generation=d->h.generation;w->established=true;}else if(d->h.op==NJW_START){free(d);return NULL;}
    d->data=malloc(d->h.bytes);if(!d->data||!exact(0,d->data,d->h.bytes,false)){free(d->data);free(d);return NULL;}return d;
}
static struct worker *state(JSContext *ctx){return JS_GetContextOpaque(ctx);}
static JSValue native_send(JSContext *ctx,JSValueConst thisv,int argc,JSValueConst *argv){(void)thisv;struct worker *w=state(ctx);uint32_t op=0,request=0;if(argc<3||JS_ToUint32(ctx,&op,argv[0])<0||JS_ToUint32(ctx,&request,argv[1])<0)return JS_EXCEPTION;if(op!=NJW_MESSAGE&&op!=NJW_ERROR&&op!=NJW_CONSOLE)return JS_ThrowTypeError(ctx,"Invalid worker send");if(!send_value(w,op,request,0,argv[2]))return JS_ThrowRangeError(ctx,"Worker send quota/transport failed");return JS_UNDEFINED;}
static JSValue native_class(JSContext *ctx,JSValueConst thisv,int argc,JSValueConst *argv){(void)thisv;return JS_NewUint32(ctx,argc?JS_GetClassID(argv[0]):0);}
static JSValue native_detach(JSContext *ctx,JSValueConst thisv,int argc,JSValueConst *argv){(void)thisv;if(argc)JS_DetachArrayBuffer(ctx,argv[0]);return JS_UNDEFINED;}
static JSValue native_now(JSContext *ctx,JSValueConst thisv,int argc,JSValueConst *argv){(void)thisv;(void)argc;(void)argv;return JS_NewFloat64(ctx,(double)uptime_ms());}
static JSValue native_close(JSContext *ctx,JSValueConst thisv,int argc,JSValueConst *argv){(void)thisv;(void)argc;(void)argv;struct worker *w=state(ctx);send_value(w,NJW_CLOSE,0,0,JS_NULL);w->closed=true;return JS_UNDEFINED;}
static JSValue native_eval(JSContext *ctx,JSValueConst thisv,int argc,JSValueConst *argv){
    (void)thisv;struct worker *w=state(ctx);if(argc<2)return JS_ThrowTypeError(ctx,"Worker source/URL required");size_t n;const char *source=JS_ToCStringLen(ctx,&n,argv[0]),*url=JS_ToCString(ctx,argv[1]);if(!source||!url){JS_FreeCString(ctx,source);JS_FreeCString(ctx,url);return JS_EXCEPTION;}
    w->until=uptime_ms()+30000;JSValue code=JS_Eval(ctx,source,n,url,JS_EVAL_TYPE_GLOBAL|JS_EVAL_FLAG_COMPILE_ONLY);JS_FreeCString(ctx,source);JS_FreeCString(ctx,url);w->until=UINT64_MAX;
    if(JS_IsException(code))return code;return JS_EvalFunction(ctx,code);
}
static JSValue native_request(JSContext *ctx,JSValueConst thisv,int argc,JSValueConst *argv){(void)thisv;struct worker *w=state(ctx);if(!argc)return JS_ThrowTypeError(ctx,"URL required");uint32_t id=++w->request;if(!send_value(w,NJW_LOAD,id,2,argv[0]))return JS_ThrowTypeError(ctx,"Worker fetch IPC failed");return JS_NewUint32(ctx,id);}
static JSValue native_import(JSContext *ctx,JSValueConst thisv,int argc,JSValueConst *argv){
    (void)thisv;struct worker *w=state(ctx);if(!argc)return JS_ThrowTypeError(ctx,"URL required");uint32_t id=++w->request;if(!send_value(w,NJW_LOAD,id,1,argv[0]))return JS_ThrowTypeError(ctx,"Worker import IPC failed");
    uint64_t wait_start=uptime_ms(),deadline=wait_start+30000;
    while(!w->closed){uint64_t now=uptime_ms();if(now>=deadline)return JS_ThrowTypeError(ctx,"Worker import deadline exceeded");struct n_pollfd p={0,N_POLLIN,0};int ready=poll(&p,1,(int)(deadline-now));if(ready<0&&errno==EINTR)continue;if(ready<=0)return JS_ThrowTypeError(ctx,"Worker import timed out");struct deferred *d=receive(w);if(!d){w->closed=true;return JS_ThrowTypeError(ctx,"Worker import pipe closed");}
        if(d->h.op==NJW_LOADED&&d->h.request==id){if(w->until!=UINT64_MAX)w->until+=uptime_ms()-wait_start;JSValue result=JS_ReadObject(ctx,d->data,d->h.bytes,JS_READ_OBJ_REFERENCE);free(d->data);free(d);return result;}
        if(d->h.bytes>NJW_MAX_QUEUE-w->queued){free(d->data);free(d);return JS_ThrowRangeError(ctx,"Worker deferred message quota");}w->queued+=d->h.bytes;if(w->tail)w->tail->next=d;else w->head=d;w->tail=d;
    }return JS_ThrowTypeError(ctx,"Worker closed");
}
static int interrupt(JSRuntime *rt,void *opaque){(void)rt;struct worker *w=opaque;return w->closed||uptime_ms()>=w->until;}
static JSValue hook(struct worker *w,const char *name,int argc,JSValueConst *argv){JSValue fn=JS_GetPropertyStr(w->ctx,w->hooks,name);JSValue r=JS_Call(w->ctx,fn,JS_UNDEFINED,argc,argv);JS_FreeValue(w->ctx,fn);return r;}
static void exception(struct worker *w){JSValue ex=JS_GetException(w->ctx);w->until=UINT64_MAX;JSValue r=hook(w,"report",1,&ex);if(JS_IsException(r)){JSValue ignored=JS_GetException(w->ctx);JS_FreeValue(w->ctx,ignored);}JS_FreeValue(w->ctx,r);JS_FreeValue(w->ctx,ex);}
static void jobs(struct worker *w){for(unsigned i=0;i<256&&!w->closed&&JS_IsJobPending(w->rt);i++){JSContext *ctx=NULL;int n=JS_ExecutePendingJob(w->rt,&ctx);if(n<0){exception(w);break;}if(!n)break;}}
int main(void){
    struct worker w={0};w.rt=JS_NewRuntime();if(!w.rt)return 1;JS_SetMemoryLimit(w.rt,NJW_HEAP_BYTES);JS_SetMaxStackSize(w.rt,512u*1024u);JS_SetCanBlock(w.rt,false);JS_SetInterruptHandler(w.rt,interrupt,&w);w.ctx=JS_NewContext(w.rt);if(!w.ctx){JS_FreeRuntime(w.rt);return 1;}JS_SetContextOpaque(w.ctx,&w);w.until=uptime_ms()+10000;
    JSValue global=JS_GetGlobalObject(w.ctx),host=JS_NewObject(w.ctx);const JSCFunctionListEntry funcs[]={JS_CFUNC_DEF("send",3,native_send),JS_CFUNC_DEF("classID",1,native_class),JS_CFUNC_DEF("detach",1,native_detach),JS_CFUNC_DEF("now",0,native_now),JS_CFUNC_DEF("close",0,native_close),JS_CFUNC_DEF("request",1,native_request),JS_CFUNC_DEF("import",1,native_import),JS_CFUNC_DEF("eval",2,native_eval)};JS_SetPropertyFunctionList(w.ctx,host,funcs,sizeof funcs/sizeof funcs[0]);web_js_encoding_init_isolated(w.ctx,host);JS_SetPropertyStr(w.ctx,global,"__workerHost",host);JS_FreeValue(w.ctx,global);
    w.hooks=JS_Eval(w.ctx,js_worker_runtime,sizeof js_worker_runtime-1,"<worker-bootstrap>",JS_EVAL_TYPE_GLOBAL);if(JS_IsException(w.hooks))goto shutdown;
    while(!w.closed){
        struct deferred *d=NULL;if(w.head){d=w.head;w.head=d->next;if(!w.head)w.tail=NULL;w.queued-=d->h.bytes;}
        else{
            w.until=UINT64_MAX;JSValue deadline=hook(&w,"deadline",0,NULL);int64_t due=-1;JS_ToInt64(w.ctx,&due,deadline);JS_FreeValue(w.ctx,deadline);uint64_t now=uptime_ms();int wait=JS_IsJobPending(w.rt)?0:due<0?-1:due<=(int64_t)now?0:(int)(due-now);struct n_pollfd p={0,N_POLLIN,0};int n=poll(&p,1,wait);if(n<0&&errno==EINTR)continue;if(n<0)break;if(p.revents&(N_POLLIN|N_POLLHUP)){d=receive(&w);if(!d)break;}
        }
        w.until=UINT64_MAX;
        if(d){JSValue value=JS_ReadObject(w.ctx,d->data,d->h.bytes,JS_READ_OBJ_REFERENCE);if(JS_IsException(value))exception(&w);else{JSValue result;if(d->h.op==NJW_LOADED){JSValue args[]={JS_NewUint32(w.ctx,d->h.request),value};result=hook(&w,"loaded",2,args);JS_FreeValue(w.ctx,args[0]);}else result=hook(&w,d->h.op==NJW_START?"start":"receive",1,&value);if(JS_IsException(result)){exception(&w);if(d->h.op==NJW_START){send_value(&w,NJW_CLOSE,0,0,JS_NULL);w.closed=true;}}JS_FreeValue(w.ctx,result);JS_FreeValue(w.ctx,value);}free(d->data);free(d);}
        JSValue now=JS_NewFloat64(w.ctx,(double)uptime_ms());JSValue tick=hook(&w,"tick",1,&now);if(JS_IsException(tick))exception(&w);JS_FreeValue(w.ctx,tick);JS_FreeValue(w.ctx,now);jobs(&w);
    }
shutdown:
    while(w.head){struct deferred *d=w.head;w.head=d->next;free(d->data);free(d);}JS_FreeValue(w.ctx,w.hooks);JS_FreeContext(w.ctx);JS_FreeRuntime(w.rt);return 0;
}
