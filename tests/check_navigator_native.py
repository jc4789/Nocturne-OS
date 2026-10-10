"""Focused actual C/QuickJS Navigator contract, with only syscall/host shims.

The production realm initializer, cookie capability callback and HTTP header
builder are executed. The same JS assertion table is used by navigatortest.c.
This host probe is not a substitute for OS Worker or real-site acceptance.
"""
from pathlib import Path
import json
import os
import subprocess

root = Path(__file__).resolve().parent.parent
out = root / 'build/parallel-navigator-contract-20261010'
out.mkdir(parents=True, exist_ok=True)
shim = out / 'include'
shim.mkdir(exist_ok=True)
(shim / 'nocturne.h').write_text('#pragma once\n#undef SYS_OPEN\n#include "common/abi.h"\nint cpu_info(struct n_cpuinfo *);\n', encoding='utf-8', newline='\n')
js = (root / 'user/libc/web/js.c').read_text(encoding='utf-8')
cookie = js[js.index('static JSValue native_cookie_enabled('):js.index('static JSValue native_cookie_impl(')]
worker_native = (root / 'user/apps/browserjsworker.c').read_text(encoding='utf-8')
worker_class = worker_native[worker_native.index('static JSValue native_class('):worker_native.index('static JSValue native_detach(')]
http = (root / 'user/libc/http.c').read_text(encoding='utf-8')
head = http[http.index('struct http_scratch {'):http.index('static int request_inner(')]
module = (root / 'user/libc/web/js_navigator.js').read_text(encoding='utf-8')
cases = (root / 'tests/js_navigator_cases.js').read_text(encoding='utf-8')
source = r'''
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>
#include "user/libc/web/js_navigator_native.h"
#include "user/libc/web/js_worker_runtime.inc"
#include "user/include/http.h"
#include "user/include/web.h"
static struct n_cpuinfo native_info;
static int cpu_result,cpu_calls,checks,failed,capability_calls;
int cpu_info(struct n_cpuinfo *out){*out=native_info;cpu_calls++;return cpu_result;}
struct web_js_state {struct web_host host;};
static struct web_js_state *state(JSContext *ctx){return JS_GetContextOpaque(ctx);}
static bool enabled;
static bool capability(void *opaque){capability_calls++;return enabled;}
static char *cookie_get(void *opaque,const char *url){failed++;return NULL;}
static void cookie_set(void *opaque,const char *url,const char *value){failed++;}
static void check(const char *name,bool yes){checks++;if(!yes){failed++;printf("FAIL %s\n",name);}}
''' + cookie + worker_class + head + r'''
static JSValue js_check(JSContext *ctx,JSValueConst receiver,int argc,JSValueConst *args){
    const char *name=JS_ToCString(ctx,args[0]);check(name?name:"conversion",argc>1&&JS_ToBool(ctx,args[1])>0);JS_FreeCString(ctx,name);return JS_UNDEFINED;
}
static bool evaluate(JSContext *ctx,const char *source){
    JSValue value=JS_Eval(ctx,source,strlen(source),"navigator-contract.js",JS_EVAL_TYPE_GLOBAL);bool ok=!JS_IsException(value);
    if(!ok){JSValue error=JS_GetException(ctx);const char *text=JS_ToCString(ctx,error);printf("EXCEPTION %s\n",text?text:"unknown");JS_FreeCString(ctx,text);JS_FreeValue(ctx,error);}
    JS_FreeValue(ctx,value);return ok;
}
static void realm(bool worker,int provider,bool enabled_value){
    JSRuntime *rt=JS_NewRuntime();JS_SetMaxStackSize(rt,512u*1024u);JSContext *ctx=JS_NewContext(rt);
    struct web_js_state state_value={0};JS_SetContextOpaque(ctx,&state_value);enabled=enabled_value;
    if(provider){state_value.host.cookie_get=cookie_get;state_value.host.cookie_set=cookie_set;}
    if(provider==2)state_value.host.cookie_enabled=capability;
    if(provider==3){state_value.host.cookie_enabled=capability;state_value.host.cookie_get=NULL;}
    if(provider==4){state_value.host.cookie_enabled=capability;state_value.host.cookie_set=NULL;}
    JSValue global=JS_GetGlobalObject(ctx),host=JS_NewObject(ctx);web_js_navigator_init(ctx,host);
    JS_SetPropertyStr(ctx,host,"classID",JS_NewCFunction(ctx,native_class,"classID",1));
    JS_SetPropertyStr(ctx,host,"cookieEnabled",JS_NewCFunction(ctx,native_cookie_enabled,"cookieEnabled",0));
    if(worker)JS_SetPropertyStr(ctx,global,"__workerHost",JS_DupValue(ctx,host));
    JS_SetPropertyStr(ctx,global,"host",host);JS_SetPropertyStr(ctx,global,"check",JS_NewCFunction(ctx,js_check,"check",2));JS_FreeValue(ctx,global);
    int queries=cpu_calls,cookie_queries=capability_calls;char ending[1024];
    snprintf(ending,sizeof ending,"%srunNavigatorChecks(%s,navigator,{userAgent:'%s',cpus:%u,cookies:%s},%s,check);for(let i=0;i<100;i++)navigator.hardwareConcurrency;",
        worker?"":"const {navigator,Interface}=navigatorBridge.create(false);",worker?"WorkerNavigator":"Interface",
        NOCTURNE_USER_AGENT,native_info.scheduler_cpus,provider==2&&enabled_value?"true":"false",worker?"true":"false");
    check("realm-evaluation",evaluate(ctx,worker?js_worker_runtime:navigator_source)&&evaluate(ctx,navigator_cases)&&evaluate(ctx,ending));
    check("count-getters-no-syscall",cpu_calls==queries);
    if(worker)check("worker-does-not-query-cookies",capability_calls==cookie_queries);
    if(!worker&&provider==2){enabled=!enabled_value;check("live-capability-evaluation",evaluate(ctx,enabled?"check('live-cookie-enabled',navigator.cookieEnabled===true)":"check('live-cookie-disabled',navigator.cookieEnabled===false)"));}
    JS_FreeContext(ctx);JS_FreeRuntime(rt);
}
int main(void){
    const struct {unsigned version,detected,online,workers,scheduler;int result;unsigned expected;} rows[]={
        {1,1,1,0,1,0,1},{1,4,4,3,4,0,4},{1,8,6,5,2,0,2},{1,12,6,5,6,0,6},
        {1,8,4,3,0,0,1},{0,4,4,3,4,0,1},{2,4,4,3,4,0,1},{1,4,4,3,4,-1,1},{1,8,2,1,4,0,1}
    };
    for(unsigned i=0;i<sizeof rows/sizeof rows[0];i++){
        native_info=(struct n_cpuinfo){.version=rows[i].version,.detected_cpus=rows[i].detected,.online_cpus=rows[i].online,.worker_cpus=rows[i].workers,.scheduler_cpus=rows[i].scheduler};cpu_result=rows[i].result;
        check("native-scheduler-matrix",web_js_hardware_concurrency()==rows[i].expected);
    }
    native_info=(struct n_cpuinfo){.version=1,.detected_cpus=8,.online_cpus=4,.worker_cpus=3,.scheduler_cpus=4};cpu_result=0;
    realm(false,0,false);realm(false,1,true);realm(false,2,false);realm(false,2,true);realm(false,3,true);realm(false,4,true);realm(true,2,true);
    check("live-capability-called",capability_calls>0);
    struct http_req request={0};struct http_scratch work={.url={.tls=true,.host="identity.test",.port=443,.path="/path?q=1"}};
    check("http-head-build",request_head(&request,&work)==NULL);
    check("http-exact-UA",work.head&&strstr(work.head,"\r\nUser-Agent: " NOCTURNE_USER_AGENT "\r\n")!=NULL);
    check("http-honest-identity",work.head&&!strstr(work.head,"Chrome")&&!strstr(work.head,"QuickJS"));
    check("http-no-referrer",work.head&&!strstr(work.head,"Referer:"));
    check("http-size",work.head&&strlen(work.head)==work.head_length);free(work.head);
    printf("native Navigator contract: %d checks, %d failures; UA=%s scheduler=%u\n",checks,failed,NOCTURNE_USER_AGENT,native_info.scheduler_cpus);
    return failed?1:0;
}
'''
source = source.replace('static bool evaluate(', 'static const char *navigator_source=' + json.dumps(module) + ';\nstatic const char *navigator_cases=' + json.dumps(cases) + ';\nstatic bool evaluate(', 1)
c = out / 'navigator-contract.c'
c.write_text(source, encoding='utf-8', newline='\n')
cc = root / 'tools/msys64/ucrt64/bin/clang.exe'
env = dict(os.environ, PATH=str(cc.parent)+os.pathsep+os.environ.get('PATH',''))
engine = [root / ('third_party/quickjs/'+name+'.c') for name in ('quickjs','dtoa','libregexp','libunicode','cutils')]
exe = out / 'navigator-contract.exe'
log = out / 'navigator-contract.log'
with log.open('w', encoding='utf-8') as stream:
    command = [str(cc), '-std=gnu11', '-O1', '-fwrapv', '-funsigned-char', '-DCONFIG_NOCTURNE',
               '-DCONFIG_VERSION="Nocturne-test"', '-I'+str(shim), '-I.', '-Ithird_party/quickjs',
               '-Ibuild/nocturne-audit/url-host-include', '-Icommon', '-idirafter', 'user/include',
               str(c), *map(str,engine), '-lm', '-o', str(exe)]
    subprocess.run(command, cwd=root, env=env, stdout=stream, stderr=subprocess.STDOUT, check=True)
    result = subprocess.run([str(exe)], cwd=root, env=env, stdout=stream, stderr=subprocess.STDOUT, timeout=30)
print(log.read_text(encoding='utf-8'),end='')
raise SystemExit(result.returncode)
