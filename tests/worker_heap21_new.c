/* 次21だけの支持試験。OS Worker起動／実サイトの受入ではない。 */
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <quickjs.h>
#include "../user/include/js_worker_wire.h"
#include "../user/libc/web/js_worker.h"

union allocation { size_t bytes; long double alignment; };
struct backend { size_t live; unsigned failed; int deny; };
static unsigned checks, failures;
static void check(const char *name, int ok) {
    ++checks; if (!ok) { ++failures; printf("FAIL %s\n", name); }
}
static void *backend_malloc(JSMallocState *s,size_t n) {
    struct backend *b=s->opaque;
    if(b->deny){b->failed++;return NULL;}
    if(!n||n>SIZE_MAX-sizeof(union allocation)||
       n+sizeof(union allocation)>SIZE_MAX-s->malloc_size||
       n+sizeof(union allocation)>s->malloc_limit-s->malloc_size)return NULL;
    union allocation *p=malloc(sizeof *p+n);if(!p)return NULL;
    p->bytes=n;b->live++;s->malloc_count++;s->malloc_size+=sizeof *p+n;return p+1;
}
static void backend_free(JSMallocState *s,void *ptr) {
    if(!ptr)return;union allocation *p=(union allocation *)ptr-1;
    ((struct backend *)s->opaque)->live--;s->malloc_count--;
    s->malloc_size-=sizeof *p+p->bytes;free(p);
}
static void *backend_realloc(JSMallocState *s,void *ptr,size_t n) {
    if(!ptr)return backend_malloc(s,n);if(!n){backend_free(s,ptr);return NULL;}
    struct backend *b=s->opaque;if(b->deny){b->failed++;return NULL;}
    union allocation *p=(union allocation *)ptr-1;size_t old=p->bytes;
    if(n>SIZE_MAX-sizeof *p||n>SIZE_MAX-(s->malloc_size-old)||
       n>s->malloc_limit-(s->malloc_size-old))return NULL;
    union allocation *q=realloc(p,sizeof *q+n);if(!q)return NULL;
    q->bytes=n;s->malloc_size=s->malloc_size-old+n;return q+1;
}
static size_t backend_usable(const void *p) {
    return p?((const union allocation *)p-1)->bytes:0;
}
static const JSMallocFunctions allocation_functions={backend_malloc,backend_free,backend_realloc,backend_usable};

static int fail_console_allocation,callback_throws;
static size_t console_live;
static unsigned callbacks;
static const char *wanted_text;
static size_t wanted_length;
static void *console_malloc(size_t n){if(fail_console_allocation){fail_console_allocation=0;return NULL;}void *p=malloc(n);if(p)console_live++;return p;}
static void console_free(void *p){if(p){console_live--;free(p);}}
#define malloc console_malloc
#define free console_free
#include "../build/goal-20261009/worker-heap21-new-once/console-tested.inc"
#undef malloc
#undef free
static JSValue receive_console(JSContext *ctx,JSValueConst self,int argc,JSValueConst *argv){
    (void)self;callbacks++;check("new-long-console-callback-arguments",argc==3);
    JSValue message=JS_GetPropertyStr(ctx,argv[2],"message");size_t n=0;const char *s=JS_ToCStringLen(ctx,&n,message);
    const char prefix[]="[Worker 4294967295 PID 2147483647] ";size_t pn=sizeof prefix-1;
    check("complete-long-utf8-nul-and-suffix-after-prefix",s&&n==pn+wanted_length&&!memcmp(s,prefix,pn)&&!memcmp(s+pn,wanted_text,wanted_length));
    JS_FreeCString(ctx,s);JS_FreeValue(ctx,message);
    return callback_throws?JS_ThrowTypeError(ctx,"new-long-console-callback-failure"):JS_UNDEFINED;
}
static void enqueue_console(web_workers *w,struct child *c){
    JSValue v=JS_NewObject(w->ctx);JS_SetPropertyStr(w->ctx,v,"message",JS_NewStringLen(w->ctx,wanted_text,wanted_length));
    size_t n;uint8_t *serialized=JS_WriteObject(w->ctx,&n,v,JS_WRITE_OBJ_REFERENCE);JS_FreeValue(w->ctx,v);
    if(!serialized){check("fixture-serialization",0);return;}
    struct event *e=console_malloc(sizeof *e);memset(e,0,sizeof *e);e->data=console_malloc(n);memcpy(e->data,serialized,n);js_free(w->ctx,serialized);
    e->id=c->id;e->h=(struct njw_header){NJW_MAGIC,NJW_CONSOLE,1,0,0,(uint32_t)n};w->events=w->last=e;w->queued=sizeof e->h+n;
}

int main(void) {
    check("heap-is-local-size-t-not-wire-ceiling", NJW_HEAP_BYTES==SIZE_MAX);
    check("wire-representation-unchanged", sizeof(struct njw_header)==24&&NJW_MAX_BYTES==UINT32_MAX&&NJW_MAX_WORKERS==UINT32_MAX);
    struct backend b={0};JSRuntime *rt=JS_NewRuntime2(&allocation_functions,&b);
    check("runtime",rt!=NULL);if(!rt)goto done;
    JS_SetMemoryLimit(rt,NJW_HEAP_BYTES);JS_SetMaxStackSize(rt,512u*1024u);
    JSMemoryUsage usage;JS_ComputeMemoryUsage(rt,&usage);
    check("actual-runtime-no-configured-ceiling", (size_t)usage.malloc_limit==SIZE_MAX);
    JSContext *ctx=JS_NewContext(rt);check("context",ctx!=NULL);
    if(!ctx){JS_FreeRuntime(rt);goto done;}
    char text[4096];memset(text,'x',sizeof text);
    b.deny=1;JSValue value=JS_NewStringLen(ctx,text,sizeof text);b.deny=0;
    check("actual-backend-failure-not-success",b.failed>0&&JS_IsException(value));
    JS_FreeValue(ctx,value);value=JS_GetException(ctx);
    check("oom-exception-present",!JS_IsNull(value)&&!JS_IsUndefined(value)&&!JS_IsException(value));
    JS_FreeValue(ctx,value);
    value=JS_NewStringLen(ctx,text,sizeof text);
    check("runtime-usable-after-allocation-restored",JS_IsString(value));JS_FreeValue(ctx,value);
    size_t n=65541;char *long_text=malloc(n);memset(long_text,'y',n);
    memcpy(long_text+17000,"\xE2\x82\xAC",3);long_text[18000]=0;memcpy(long_text+n-3,"\xE6\x97\xA5",3);
    wanted_text=long_text;wanted_length=n;
    struct child c={.id=UINT32_MAX,.pid=INT32_MAX};
    web_workers w={.ctx=ctx,.generation=1,.children=&c,.callback=JS_NewCFunction(ctx,receive_console,"receive-console",3)};
    enqueue_console(&w,&c);check("new-long-console-dispatch",web_worker_run_one(&w)&&callbacks==1&&!JS_HasException(ctx)&&!console_live&&!w.queued);
    callback_throws=1;enqueue_console(&w,&c);check("new-long-console-callback-exception-preserved",web_worker_run_one(&w)&&callbacks==2&&JS_HasException(ctx)&&!console_live&&!w.queued);
    value=JS_GetException(ctx);JS_FreeValue(ctx,value);callback_throws=0;
    enqueue_console(&w,&c);fail_console_allocation=1;
    check("new-long-console-scratch-oom-not-fake-callback",web_worker_run_one(&w)&&callbacks==2&&JS_HasException(ctx)&&!console_live&&!w.queued);
    value=JS_GetException(ctx);JS_FreeValue(ctx,value);JS_FreeValue(ctx,w.callback);free(long_text);
    JS_FreeContext(ctx);JS_FreeRuntime(rt);check("all-owned-backend-blocks-released",b.live==0);
done:
    printf("worker-heap21-new: %u checks, %u failed\n",checks,failures);
    return failures?1:0;
}
