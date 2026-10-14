/* 製品のQuickJS/WABTを実Nocturneで強制GCする補助試験。
 * GUI実サイト受入の代わりではない。各反復はCoreの64KiB memoryを1個持つ。
 * 反復数は実行引数で与え、製品側の上限や回収条件には使わない。 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../user/libc/web/js_wasm.h"

static unsigned cancel_after,poll_count;
static int poll_wasm(JSContext *ctx){(void)ctx;return cancel_after&&++poll_count>=cancel_after;}
static JSValue force_gc(JSContext *ctx,JSValueConst self,int argc,JSValueConst *argv){
    (void)self;(void)argc;(void)argv;JS_RunGC(JS_GetRuntime(ctx));return JS_UNDEFINED;
}
static JSValue retire_wasm(JSContext *ctx,JSValueConst self,int argc,JSValueConst *argv){
    (void)self;(void)argc;(void)argv;web_js_wasm_cancel(ctx);return JS_UNDEFINED;
}
static int test_hooks(JSContext *ctx){
    JSValue global=JS_GetGlobalObject(ctx);
    int ok=JS_SetPropertyStr(ctx,global,"forceGC",JS_NewCFunction(ctx,force_gc,"forceGC",0))>=0&&
        JS_SetPropertyStr(ctx,global,"retireWasm",JS_NewCFunction(ctx,retire_wasm,"retireWasm",0))>=0;
    JS_FreeValue(ctx,global);return ok;
}
static int evaluate(JSContext *ctx,const char *source){
    JSValue value=JS_Eval(ctx,source,strlen(source),"wasm-gc-test",JS_EVAL_TYPE_GLOBAL);
    if(JS_IsException(value)){
        JSValue error=JS_GetException(ctx);const char *message=JS_ToCString(ctx,error);
        printf("FAIL wasmgctest: %s\n",message?message:"exception");
        JS_FreeCString(ctx,message);JS_FreeValue(ctx,error);return 0;
    }
    JS_FreeValue(ctx,value);return 1;
}
static int collect_empty(JSContext *ctx,const char *label){
    JS_RunGC(JS_GetRuntime(ctx));web_js_wasm_collect(ctx);
    web_js_wasm_stats stats={0};
    if(web_js_wasm_get_stats(ctx,&stats)<0||stats.live_wrappers){
        printf("FAIL wasmgctest %s: live wrappers %lu\n",label,(unsigned long)stats.live_wrappers);return 0;
    }
    printf("PASS wasmgctest %s\n",label);return 1;
}
static int drain_jobs(JSRuntime *rt){
    while(JS_IsJobPending(rt)){
        JSContext *ctx=NULL;
        if(JS_ExecutePendingJob(rt,&ctx)<0){
            JSValue error=JS_GetException(ctx);const char *message=JS_ToCString(ctx,error);
            printf("FAIL wasmgctest job: %s\n",message?message:"exception");
            JS_FreeCString(ctx,message);JS_FreeValue(ctx,error);return 0;
        }
    }
    return 1;
}
int main(int argc,char **argv){
    if(argc!=2){printf("wasmgctest: 反復数を指定してください\n");return 1;}
    char *end;unsigned long iterations=strtoul(argv[1],&end,10);
    if(!iterations||*end)return 1;
    JSRuntime *rt=JS_NewRuntime();if(!rt)return 1;
    JSContext *ctx=JS_NewContext(rt);if(!ctx){JS_FreeRuntime(rt);return 1;}
    int ok=web_js_wasm_init(ctx,poll_wasm,NULL)>=0;
    if(ok)ok=test_hooks(ctx);
    /* Import/export名やopcodeはWebAssembly Core binary encodingそのもの。 */
    const char *program=
        "(()=>{const b=new Uint8Array([0,97,115,109,1,0,0,0,1,6,1,96,1,127,1,127,2,9,1,3,101,110,118,1,102,0,0,3,2,1,0,7,7,1,3,114,117,110,0,1,10,8,1,6,0,32,0,16,0,11]);"
        "for(let i=0;i<%lu;i++){const mod=new WebAssembly.Module(b);let instance;"
        "instance=new WebAssembly.Instance(mod,{env:{f:x=>{forceGC();return instance?x:0}}});"
        "if(instance.exports.run(42)!==42)throw new Error('callback');"
        "const memory=new WebAssembly.Memory({initial:1}),table=new WebAssembly.Table({element:'externref',initial:1});"
        "const global=new WebAssembly.Global({value:'externref',mutable:true},table);"
        "table.set(0,{instance,memory,table,global});}})();";
    int length=snprintf(NULL,0,program,iterations);
    char *source=length<0?NULL:malloc((size_t)length+1);
    if(!source)ok=0;
    else snprintf(source,(size_t)length+1,program,iterations);
    if(ok)ok=evaluate(ctx,source)&&collect_empty(ctx,"callback-instance-table-global-cycles");
    free(source);
    if(ok)ok=evaluate(ctx,"globalThis.kept=(()=>{const m=new WebAssembly.Memory({initial:1});new Uint8Array(m.buffer)[7]=91;return m.buffer;})();");
    if(ok){
        JS_RunGC(rt);web_js_wasm_collect(ctx);
        ok=evaluate(ctx,"if(new Uint8Array(kept)[7]!==91)throw new Error('buffer owner lifetime');kept=null;")&&
           collect_empty(ctx,"buffer-outlives-memory-wrapper");
    }
    if(ok)ok=evaluate(ctx,
        "(()=>{const b=new Uint8Array([0,97,115,109,1,0,0,0,1,8,2,96,0,0,96,0,1,111,2,18,2,1,101,5,99,108,101,97,114,0,0,1,101,1,103,3,111,1,3,2,1,1,7,7,1,3,114,117,110,0,1,10,14,1,12,1,1,111,35,0,33,0,16,0,32,0,11]);"
        "const g=new WebAssembly.Global({value:'externref',mutable:true},{marker:73});"
        "const i=new WebAssembly.Instance(new WebAssembly.Module(b),{e:{g,clear(){g.value=null;forceGC()}}});"
        "const result=i.exports.run();if(!result||result.marker!==73||g.value!==null)throw new Error('native returned ref lifetime')})();")&&
        collect_empty(ctx,"returned-ref-outlives-cleared-global");
    if(ok)ok=evaluate(ctx,
        "globalThis.keptTable=(()=>{const b=new Uint8Array([0,97,115,109,1,0,0,0,1,6,1,96,1,127,1,127,3,2,1,0,7,7,1,3,114,117,110,0,0,10,9,1,7,0,32,0,65,1,106,11]);"
        "const instance=new WebAssembly.Instance(new WebAssembly.Module(b));"
        "const t=new WebAssembly.Table({element:'anyfunc',initial:1});t.set(0,instance.exports.run);"
        "globalThis.keptExport=instance.exports.run;return t})();");
    if(ok){
        JS_RunGC(rt);web_js_wasm_collect(ctx);
        ok=evaluate(ctx,"if(keptExport(41)!==42||keptTable.get(0)(8)!==9)throw new Error('native code lifetime');keptExport=null;keptTable=null;")&&
           collect_empty(ctx,"exports-outlive-module-and-instance-wrappers");
    }
    if(ok){
        web_js_wasm_stats stats={0};
        ok=web_js_wasm_get_stats(ctx,&stats)==0&&stats.native_modules>=iterations+1&&stats.native_calls>=iterations+2;
        printf("%s wasmgctest wasm2c/TCC: native modules %lu, calls %lu, fallbacks %lu\n",
            ok?"PASS":"FAIL",(unsigned long)stats.native_modules,(unsigned long)stats.native_calls,
            (unsigned long)stats.native_fallbacks);
        if(!ok)printf("wasmgctest fallback: %s\n",web_js_wasm_native_fallback(ctx));
    }
    if(ok)ok=evaluate(ctx,
        "globalThis.loopInstance=new WebAssembly.Instance(new WebAssembly.Module(new Uint8Array([0,97,115,109,1,0,0,0,1,4,1,96,0,0,3,2,1,0,7,7,1,3,114,117,110,0,0,10,9,1,7,0,3,64,12,0,11,11])));");
    if(ok){
        /* Enter the function and its loop, then request cancellation through
         * the embedding callback. This is a test trigger, not a product limit. */
        cancel_after=3;poll_count=0;
        ok=evaluate(ctx,"let cancelled=false;try{loopInstance.exports.run()}catch(e){cancelled=e instanceof WebAssembly.RuntimeError}if(!cancelled)throw new Error('native loop cancellation');loopInstance=null;");
        cancel_after=0;
        if(ok)ok=collect_empty(ctx,"native-loop-cancellation");
    }
    web_js_wasm_stats before_jspi={0};
    if(ok)ok=web_js_wasm_get_stats(ctx,&before_jspi)==0;
    if(ok)ok=evaluate(ctx,
        "globalThis.jspiDone=false;(()=>{let release;const p=new Promise(r=>release=r);globalThis.jspiRelease=release;"
        "const b=new Uint8Array([0,97,115,109,1,0,0,0,1,6,1,96,1,127,1,127,2,9,1,3,101,110,118,1,102,0,0,3,2,1,0,7,7,1,3,114,117,110,0,1,10,8,1,6,0,32,0,16,0,11]);"
        "const i=new WebAssembly.Instance(new WebAssembly.Module(b),{env:{f:new WebAssembly.Suspending(x=>p.then(()=>{forceGC();return x+1}))}});"
        "WebAssembly.promising(i.exports.run)(41).then(v=>{if(v!==42)throw new Error('JSPI resumed value');jspiDone=true})})();");
    if(ok){
        JS_RunGC(rt);web_js_wasm_collect(ctx);
        ok=evaluate(ctx,"jspiRelease();jspiRelease=null;")&&drain_jobs(rt)&&
            evaluate(ctx,"if(!jspiDone)throw new Error('JSPI did not resume');delete globalThis.jspiDone;delete globalThis.jspiRelease;")&&
            collect_empty(ctx,"jspi-suspended-code-and-ref-gc");
    }
    if(ok){
        web_js_wasm_stats after={0};
        ok=web_js_wasm_get_stats(ctx,&after)==0&&after.native_calls>before_jspi.native_calls;
        printf("%s wasmgctest JSPI native calls before %lu after %lu\n",ok?"PASS":"FAIL",
            (unsigned long)before_jspi.native_calls,(unsigned long)after.native_calls);
    }
    web_js_wasm_stats before_fallback={0};
    if(ok)ok=web_js_wasm_get_stats(ctx,&before_fallback)==0;
    if(ok)ok=evaluate(ctx,
        "globalThis.fallbackDone=false;(()=>{"
        "const b=new Uint8Array([0,97,115,109,1,0,0,0,1,8,2,96,0,0,96,0,1,111,2,18,2,1,101,5,112,97,117,115,101,0,0,1,101,1,103,3,111,1,3,3,2,1,0,7,7,1,3,114,117,110,0,1,10,19,2,12,1,1,111,35,0,33,0,16,0,32,0,11,4,0,18,0,11]);"
        "const g=new WebAssembly.Global({value:'externref',mutable:true},{marker:73});"
        "const i=new WebAssembly.Instance(new WebAssembly.Module(b),{e:{g,pause:new WebAssembly.Suspending(()=>{g.value=null;forceGC();return new Promise(r=>globalThis.fallbackResolve=r)})}});"
        "WebAssembly.promising(i.exports.run)().then(v=>{if(!v||v.marker!==73)throw new Error('fallback suspended ref');fallbackDone=true})})();");
    if(ok){
        JS_RunGC(rt);web_js_wasm_collect(ctx);
        ok=evaluate(ctx,"fallbackResolve();fallbackResolve=null;")&&drain_jobs(rt)&&
            evaluate(ctx,"if(!fallbackDone)throw new Error('fallback did not resume');delete globalThis.fallbackDone;delete globalThis.fallbackResolve;")&&
            collect_empty(ctx,"jspi-fallback-thread-ref-gc");
        web_js_wasm_stats after={0};
        if(ok)ok=web_js_wasm_get_stats(ctx,&after)==0&&after.native_fallbacks==before_fallback.native_fallbacks+1&&after.native_calls==before_fallback.native_calls;
        printf("%s wasmgctest JSPI interpreter fallback boundary\n",ok?"PASS":"FAIL");
    }
    if(ok)ok=evaluate(ctx,
        "globalThis.jspiRetiredCallback=0;(()=>{"
        "const b=new Uint8Array([0,97,115,109,1,0,0,0,1,6,1,96,1,127,1,127,2,9,1,3,101,110,118,1,102,0,0,3,2,1,0,7,7,1,3,114,117,110,0,1,10,8,1,6,0,32,0,16,0,11]);"
        "const i=new WebAssembly.Instance(new WebAssembly.Module(b),{env:{f:new WebAssembly.Suspending(()=>new Promise(()=>{}))}});"
        "WebAssembly.promising(i.exports.run)(1).then(()=>jspiRetiredCallback++,()=>jspiRetiredCallback++)})();");
    if(ok){
        web_js_wasm_cancel(ctx);
        ok=drain_jobs(rt)&&evaluate(ctx,"if(jspiRetiredCallback!==0)throw new Error('retired JSPI author callback');delete globalThis.jspiRetiredCallback;")&&
            collect_empty(ctx,"jspi-retirement-unwinds-without-author-callback");
    }
    JS_FreeContext(ctx);
    const char *retire_cases[]={
        "globalThis.afterRetire=0;(()=>{const b=new Uint8Array([0,97,115,109,1,0,0,0,1,6,1,96,1,127,1,127,2,17,2,3,101,110,118,1,102,0,0,3,101,110,118,1,103,0,0,3,2,1,0,7,7,1,3,114,117,110,0,2,10,10,1,8,0,32,0,16,0,16,1,11]);"
        "const i=new WebAssembly.Instance(new WebAssembly.Module(b),{env:{f(x){retireWasm();return x},g(x){afterRetire++;return x}}});WebAssembly.promising(i.exports.run)(1)})();",
        "globalThis.afterRetire=0;(()=>{const b=new Uint8Array([0,97,115,109,1,0,0,0,1,6,1,96,1,127,1,127,2,9,1,3,101,110,118,1,102,0,0,3,2,1,0,7,7,1,3,114,117,110,0,1,10,8,1,6,0,32,0,16,0,11]);"
        "const i=new WebAssembly.Instance(new WebAssembly.Module(b),{env:{f:new WebAssembly.Suspending(()=>{retireWasm();return {get then(){afterRetire++;return ()=>{}}}})}});WebAssembly.promising(i.exports.run)(1)})();",
        "globalThis.afterRetire=0;globalThis.settlementGetter=false;(()=>{const b=new Uint8Array([0,97,115,109,1,0,0,0,1,5,1,96,0,1,111,2,9,1,3,101,110,118,1,102,0,0,3,2,1,0,7,7,1,3,114,117,110,0,1,10,6,1,4,0,16,0,11]);"
        "const i=new WebAssembly.Instance(new WebAssembly.Module(b),{env:{f(){return {get then(){settlementGetter=true;retireWasm();return undefined}}}}});WebAssembly.promising(i.exports.run)()})();"
        "if(!settlementGetter)throw new Error('settlement getter not entered');delete globalThis.settlementGetter;"
    };
    for(size_t i=0;ok&&i<sizeof retire_cases/sizeof *retire_cases;i++){
        ctx=JS_NewContext(rt);
        if(!ctx){ok=0;break;}
        ok=web_js_wasm_init(ctx,poll_wasm,NULL)>=0&&test_hooks(ctx)&&evaluate(ctx,retire_cases[i])&&
            drain_jobs(rt)&&evaluate(ctx,"if(afterRetire!==0)throw new Error('author code after running retirement');delete globalThis.afterRetire;")&&
            collect_empty(ctx,i==0?"running-retirement-before-next-import":i==1?"running-retirement-before-thenable-getter":"settlement-retirement-keeps-context-until-return");
        web_js_wasm_cancel(ctx);JS_FreeContext(ctx);
    }
    JS_FreeRuntime(rt);
    if(ok)printf("WASM_GC_PASS iterations=%lu\n",iterations);
    return !ok;
}
