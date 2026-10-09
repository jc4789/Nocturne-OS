/* 次23の実QuickJS/bootstrap/nativecommit支持試験。OS/サイト受入でない。 */
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <quickjs.h>
/* 未実行補正：backendのarena再利用に依存せずprivate plan scratch
 * allocation地点だけを故障注入する。原result.logの再実行はしない。 */
static int plan_deny;static unsigned plan_failed;
static void *plan_malloc(JSContext*c,size_t n){if(plan_deny){plan_failed++;JS_ThrowOutOfMemory(c);return NULL;}return js_malloc(c,n);}
static void *plan_mallocz(JSContext*c,size_t n){if(plan_deny){plan_failed++;JS_ThrowOutOfMemory(c);return NULL;}return js_mallocz(c,n);}
#define js_malloc plan_malloc
#define js_mallocz plan_mallocz
#include "../user/libc/web/js_worker_transfer.h"
#undef js_malloc
#undef js_mallocz
union allocation {size_t bytes;long double alignment;};
struct backend {int deny;size_t live;unsigned failed;};
static unsigned checks,failures;static int closed;static double now=100;
static void *amalloc(JSMallocState *s,size_t n){struct backend*b=s->opaque;if(b->deny){b->failed++;return NULL;}if(!n||n>SIZE_MAX-sizeof(union allocation))return NULL;union allocation*p=malloc(sizeof*p+n);if(!p)return NULL;p->bytes=n;b->live++;s->malloc_count++;s->malloc_size+=sizeof*p+n;return p+1;}
static void afree(JSMallocState*s,void*v){if(!v)return;union allocation*p=(union allocation*)v-1;((struct backend*)s->opaque)->live--;s->malloc_count--;s->malloc_size-=sizeof*p+p->bytes;free(p);}
static void *arealloc(JSMallocState*s,void*v,size_t n){if(!v)return amalloc(s,n);if(!n){afree(s,v);return NULL;}struct backend*b=s->opaque;if(b->deny){b->failed++;return NULL;}union allocation*p=(union allocation*)v-1;size_t old=p->bytes;if(n>SIZE_MAX-sizeof*p)return NULL;union allocation*q=realloc(p,sizeof*q+n);if(!q)return NULL;q->bytes=n;s->malloc_size=s->malloc_size-old+n;return q+1;}
static size_t ausable(const void*v){return v?((const union allocation*)v-1)->bytes:0;}
static const JSMallocFunctions allocators={amalloc,afree,arealloc,ausable};
static void check(const char *name,int ok){checks++;printf("%s %s\n",ok?"PASS":"FAIL",name);if(!ok)failures++;}
static JSValue native_check(JSContext*c,JSValueConst t,int n,JSValueConst*a){(void)t;const char*s=n?JS_ToCString(c,a[0]):NULL;check(s?s:"fixture-name",n>1&&JS_ToBool(c,a[1])>0);JS_FreeCString(c,s);return JS_UNDEFINED;}
static JSValue native_class(JSContext*c,JSValueConst t,int n,JSValueConst*a){(void)t;return JS_NewUint32(c,n?JS_GetClassID(a[0]):0);}
static JSValue native_commit(JSContext*c,JSValueConst t,int n,JSValueConst*a){(void)t;if(!n)return JS_ThrowTypeError(c,"plan required");JSValue g=JS_GetGlobalObject(c),v=JS_GetPropertyStr(c,g,"__denyCommit");int deny=JS_ToBool(c,v);JS_FreeValue(c,v);JS_FreeValue(c,g);plan_deny=deny;JSValue r=worker_transfer_commit(c,a[0],!closed);plan_deny=0;return r;}
static JSValue native_now(JSContext*c,JSValueConst t,int n,JSValueConst*a){(void)t;(void)n;(void)a;return JS_NewFloat64(c,now);}
static JSValue native_close(JSContext*c,JSValueConst t,int n,JSValueConst*a){(void)c;(void)t;(void)n;(void)a;closed=1;return JS_UNDEFINED;}
static JSValue native_send(JSContext*c,JSValueConst t,int n,JSValueConst*a){(void)t;
#ifdef WORKER_CLONE25
 if(n<3)return JS_ThrowTypeError(c,"fixture packet required");size_t bytes;uint8_t*p=JS_WriteObject(c,&bytes,a[2],JS_WRITE_OBJ_REFERENCE);if(!p)return JS_EXCEPTION;
 JSValue v=JS_ReadObject(c,p,bytes,JS_READ_OBJ_REFERENCE);js_free(c,p);if(JS_IsException(v))return v;JSValue g=JS_GetGlobalObject(c);JS_SetPropertyStr(c,g,"__packet",v);JS_FreeValue(c,g);return JS_UNDEFINED;
#else
 (void)n;(void)a;return JS_ThrowTypeError(c,"fixture external IPC must not be reached");
#endif
}
static JSValue native_detach(JSContext*c,JSValueConst t,int n,JSValueConst*a){(void)t;if(n)JS_DetachArrayBuffer(c,a[0]);return JS_UNDEFINED;}
static JSValue native_decode(JSContext*c,JSValueConst t,int n,JSValueConst*a){(void)t;size_t size=0;uint32_t off=0,len=0;if(n<3)return JS_ThrowTypeError(c,"decode");uint8_t*p=JS_GetArrayBuffer(c,&size,a[0]);if(JS_ToUint32(c,&off,a[1])<0||JS_ToUint32(c,&len,a[2])<0||off>size||len>size-off)return JS_EXCEPTION;return JS_NewStringLen(c,(char*)p+off,len);}
static int exception(JSContext*c,const char*where){if(!JS_HasException(c))return 0;JSValue e=JS_GetException(c);const char*s=JS_ToCString(c,e);printf("FIXTURE_EXCEPTION %s %s\n",where,s?s:"unknown");JS_FreeCString(c,s);JS_FreeValue(c,e);failures++;return 1;}
static JSValue evaluate(JSContext*c,const char*path){FILE*f=fopen(path,"rb");if(!f)return JS_EXCEPTION;fseek(f,0,SEEK_END);long n=ftell(f);rewind(f);char*p=malloc(n+1);fread(p,1,n,f);fclose(f);p[n]=0;JSValue v=JS_Eval(c,p,n,path,JS_EVAL_TYPE_GLOBAL);free(p);return v;}
static int jobs(JSRuntime*r,JSContext*c){while(JS_IsJobPending(r)){JSContext*job;int n=JS_ExecutePendingJob(r,&job);if(n<0){exception(job,"job");return 0;}if(!n)break;}return !exception(c,"checkpoint");}
#ifndef WORKER_BOOTSTRAP
#define WORKER_BOOTSTRAP "build/goal-20261009/worker-messaging23-new-once/bootstrap.js"
#define WORKER_CASES "tests/worker_messaging23_new.js"
#endif
int main(void){struct backend b={0};JSRuntime*r=JS_NewRuntime2(&allocators,&b);JSContext*c=JS_NewContext(r);JS_SetContextOpaque(c,&b);JS_SetMemoryLimit(r,SIZE_MAX);JS_SetMaxStackSize(r,512*1024);JSValue g=JS_GetGlobalObject(c),h=JS_NewObject(c);
 const JSCFunctionListEntry funcs[]={JS_CFUNC_DEF("classID",1,native_class),JS_CFUNC_DEF("transferCommit",1,native_commit),JS_CFUNC_DEF("now",0,native_now),JS_CFUNC_DEF("close",0,native_close),JS_CFUNC_DEF("send",3,native_send),JS_CFUNC_DEF("detach",1,native_detach),JS_CFUNC_DEF("decodeUTF8",5,native_decode)};
 JS_SetPropertyFunctionList(c,h,funcs,sizeof funcs/sizeof*funcs);JS_SetPropertyStr(c,g,"__workerHost",h);JS_SetPropertyStr(c,g,"__check",JS_NewCFunction(c,native_check,"check",2));
 JSValue hooks=evaluate(c,WORKER_BOOTSTRAP);if(JS_IsException(hooks)){exception(c,"bootstrap");goto out;}
 JSValue v=evaluate(c,WORKER_CASES);JS_FreeValue(c,v);if(exception(c,"suite-compile"))goto out;
 JSValue suite=JS_GetPropertyStr(c,g,"__suite"),length=JS_GetPropertyStr(c,suite,"length");uint32_t count=0;JS_ToUint32(c,&count,length);JS_FreeValue(c,length);
 for(uint32_t i=0;i<count;i++){JSValue entry=JS_GetPropertyUint32(c,suite,i),fn=JS_GetPropertyStr(c,entry,"run"),steps=JS_GetPropertyStr(c,entry,"steps");uint32_t n=0;JS_ToUint32(c,&n,steps);JS_FreeValue(c,steps);v=JS_Call(c,fn,JS_UNDEFINED,0,NULL);JS_FreeValue(c,v);JS_FreeValue(c,fn);JS_FreeValue(c,entry);if(exception(c,"phase")||!jobs(r,c))break;
  JSValue advance=JS_GetPropertyStr(c,g,"__advanceClock");if(JS_ToBool(c,advance)>0)now=101;JS_FreeValue(c,advance);
  for(uint32_t j=0;j<n;j++){fn=JS_GetPropertyStr(c,hooks,"tick");JSValue a=JS_NewFloat64(c,now);v=JS_Call(c,fn,JS_UNDEFINED,1,&a);JS_FreeValue(c,a);JS_FreeValue(c,v);JS_FreeValue(c,fn);if(exception(c,"port-task")||!jobs(r,c))break;}
 }
 JS_FreeValue(c,suite);
#ifndef WORKER_CLONE25
 check("private-plan-preflight-fault-injected",plan_failed>0);
#endif
out:JS_FreeValue(c,hooks);JS_FreeValue(c,g);JS_FreeContext(c);JS_FreeRuntime(r);check("runtime-teardown-releases-all",b.live==0);printf("RESULT %u assertions %u failures\n",checks,failures);return failures?1:0;}
