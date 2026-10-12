/* Production QuickJS + BearSSL + WebCrypto; only random device I/O uses the
 * existing Windows CSPRNG harness. No browser/network acceptance is claimed. */
#include <windows.h>
#include <bcrypt.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include "quickjs.h"
static int aes_test_open(const char *path,int flags,...){return !strcmp(path,"/dev/random")&&flags==O_RDONLY?42:-1;}
static ssize_t aes_test_read(int fd,void *bytes,size_t size){if(fd!=42||size>65536)return -1;return BCryptGenRandom(NULL,bytes,(ULONG)size,BCRYPT_USE_SYSTEM_PREFERRED_RNG)<0?-1:(ssize_t)size;}
static int aes_test_close(int fd){return fd==42?0:-1;}
#define open aes_test_open
#define read aes_test_read
#define close aes_test_close
#include "../user/libc/web/js_crypto.c"
#undef open
#undef read
#undef close
static void exception(JSContext *ctx){JSValue e=JS_GetException(ctx),stack=JS_GetPropertyStr(ctx,e,"stack");const char *s=JS_ToCString(ctx,e),*t=JS_ToCString(ctx,stack);fprintf(stderr,"%s\n%s\n",s?s:"exception",t?t:"");JS_FreeCString(ctx,s);JS_FreeCString(ctx,t);JS_FreeValue(ctx,stack);JS_FreeValue(ctx,e);}
static JSValue evaluate(JSContext *ctx,const char *source,const char *name){JSValue v=JS_Eval(ctx,source,strlen(source),name,JS_EVAL_TYPE_GLOBAL);if(JS_IsException(v)){exception(ctx);exit(2);}return v;}
static void load(JSContext *ctx,const char *path){FILE *f=fopen(path,"rb");if(!f){perror(path);exit(2);}fseek(f,0,SEEK_END);long size=ftell(f);rewind(f);char *source=malloc((size_t)size+1);if(!source||fread(source,1,(size_t)size,f)!=(size_t)size)exit(2);fclose(f);source[size]=0;JS_FreeValue(ctx,evaluate(ctx,source,path));free(source);}
static JSValue url(JSContext *ctx,JSValueConst self,int argc,JSValueConst *argv){(void)self;(void)argc;(void)argv;return JS_NewString(ctx,"https://example.com/");}
static int await_result(JSContext *ctx,JSValue p){JSContext *job;int n;while((n=JS_ExecutePendingJob(JS_GetRuntime(ctx),&job))>0){}if(n<0){exception(job);exit(2);}JSValue result=JS_PromiseResult(ctx,p);if(JS_PromiseState(ctx,p)!=JS_PROMISE_FULFILLED){JS_Throw(ctx,result);exception(ctx);exit(2);}int count=0;if(JS_ToInt32(ctx,&count,result)<0)exit(2);JS_FreeValue(ctx,result);JS_FreeValue(ctx,p);return count;}
int main(int argc,char **argv){
    JSRuntime *rt=JS_NewRuntime();JSContext *ctx=JS_NewContext(rt);
    JSValue global=JS_GetGlobalObject(ctx),host=JS_NewObject(ctx);web_js_crypto_init(ctx,host);
    JS_SetPropertyStr(ctx,host,"url",JS_NewCFunction(ctx,url,"url",0));
    JS_SetPropertyStr(ctx,global,"host",host);JS_FreeValue(ctx,global);
    JS_FreeValue(ctx,evaluate(ctx,"globalThis.DOMException=class DOMException extends Error{constructor(m,n){super(m);this.name=n;}};host.decodeUTF8=(b,o,n)=>{let s='';for(const v of new Uint8Array(b,o,n))s+='%'+v.toString(16).padStart(2,'0');return decodeURIComponent(s);};","setup"));
    load(ctx,"user/libc/web/js_encoding.js");load(ctx,"user/libc/web/js_url.js");load(ctx,"user/libc/web/js_crypto.js");
    bool incremental=argc>1&&!strcmp(argv[1],"--incremental");
    load(ctx,"tests/js_aes_cases.js");printf("Production AES checks: %d\n",await_result(ctx,evaluate(ctx,incremental?"runGCMCases()":"runAESCases()","AES cases")));
    load(ctx,"tests/js_eme_cases.js");load(ctx,"user/libc/web/js_eme.js");
    printf("Production EME lifecycle checks (mock media bridge): %d\n",await_result(ctx,evaluate(ctx,incremental?"runPSSHChunkCases()":"runEMECases()","EME cases")));
    JS_FreeContext(ctx);JS_FreeRuntime(rt);return 0;
}
