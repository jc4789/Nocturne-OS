"""Actual native DOM_position arm and unwrap, with QuickJS exception delivery.

Only the valid-node comparison result and DOM storage are shims. This focused
host test does not claim full browser or real-site compatibility.
"""
from pathlib import Path
import os
import subprocess

root = Path(__file__).resolve().parent.parent
out = root / 'build/parallel-js-contract-20261010'
out.mkdir(parents=True, exist_ok=True)
js = (root / 'user/libc/web/js.c').read_text(encoding='utf-8')
unwrap = js[js.index('static node_t *unwrap('):js.index('static node_t *node_opaque(')]
arm = js[js.index('    case DOM_position: {'):js.index('    case DOM_equal:', js.index('    case DOM_position: {'))]
source = r'''
#include <stdio.h>
#include <string.h>
#include "quickjs.h"
typedef struct node {int unused;} node_t;
struct js_node_ref {node_t *node;};
static JSClassID node_class;
static unsigned compare_node_position(node_t *a,node_t *b){return a==b?0:4;}
'''+unwrap+r'''
enum {DOM_position};
static JSValue position(JSContext *ctx,JSValueConst receiver,int supplied,JSValueConst *incoming){
    /* Public JS wrapper always forwards its formal other, including undefined
       for an omitted argument, to the magic binding. */
    JSValueConst argv[]={JS_UNDEFINED,supplied?incoming[0]:JS_UNDEFINED,supplied>1?incoming[1]:JS_UNDEFINED};
    int argc=3;node_t *n=unwrap(ctx,argv[1]);
    if(!n)return JS_ThrowTypeError(ctx,"Invalid DOM receiver");
    JSValue result=JS_UNDEFINED;switch(DOM_position){
'''+arm+r'''
    }return result;
}
int main(void){
    JSRuntime *rt=JS_NewRuntime();JSContext *ctx=JS_NewContext(rt);
    JS_NewClassID(&node_class);JSClassDef definition={.class_name="DOMNode"};
    JS_NewClass(rt,node_class,&definition);
    node_t root={0},other={0};struct js_node_ref refs[]={{&root},{&other}};
    JSValue global=JS_GetGlobalObject(ctx),a=JS_NewObjectClass(ctx,node_class),b=JS_NewObjectClass(ctx,node_class);
    JS_SetOpaque(a,&refs[0]);JS_SetOpaque(b,&refs[1]);
    JS_SetPropertyStr(ctx,global,"root",a);JS_SetPropertyStr(ctx,global,"other",b);
    JS_SetPropertyStr(ctx,global,"nativePosition",JS_NewCFunction(ctx,position,"nativePosition",2));JS_FreeValue(ctx,global);
    const char *cases="let count=0;function check(v){count++;if(!v)throw Error('position check '+count);}"
        "for(const value of [null,undefined,{},42,'node']){let error;try{nativePosition(root,value);}catch(e){error=e;}check(error instanceof TypeError);}"
        "let error;try{nativePosition(root);}catch(e){error=e;}check(error instanceof TypeError);"
        "for(const value of [null,undefined,{}]){let error;try{nativePosition(value,root);}catch(e){error=e;}check(error instanceof TypeError);}"
        "check(nativePosition(root,root)===0);check(nativePosition(root,other)===4);"
        "let recovered=false;try{nativePosition(root,undefined);}catch(e){recovered=e instanceof TypeError;}check(recovered);count;";
    JSValue result=JS_Eval(ctx,cases,strlen(cases),"position-exception.js",JS_EVAL_TYPE_GLOBAL);
    int failed=JS_IsException(result),count=0;
    if(failed){JSValue error=JS_GetException(ctx);const char *text=JS_ToCString(ctx,error);fprintf(stderr,"FAIL %s\n",text?text:"unknown");JS_FreeCString(ctx,text);JS_FreeValue(ctx,error);}
    else JS_ToInt32(ctx,&count,result);
    printf("native position exception: %d checks, %d failures\n",count,failed);
    JS_FreeValue(ctx,result);JS_FreeContext(ctx);JS_FreeRuntime(rt);return failed||count!=12;
}
'''
c = out / 'dom-position-exception.c'
c.write_text(source, encoding='utf-8', newline='\n')
cc = root / 'tools/msys64/ucrt64/bin/clang.exe'
env = dict(os.environ, PATH=str(cc.parent)+os.pathsep+os.environ.get('PATH', ''))
engine = [root / ('third_party/quickjs/'+name+'.c') for name in ('quickjs','dtoa','libregexp','libunicode','cutils')]
exe = out / 'dom-position-exception.exe'
log = out / 'position-exception.log'
with log.open('w', encoding='utf-8') as stream:
    command = [str(cc), '-std=gnu11', '-O1', '-fwrapv', '-funsigned-char', '-DCONFIG_NOCTURNE',
               '-DCONFIG_VERSION="Nocturne-test"', '-Ithird_party/quickjs', '-Ibuild/nocturne-audit/url-host-include',
               str(c), *map(str, engine), '-lm', '-o', str(exe)]
    subprocess.run(command, cwd=root, env=env, stdout=stream, stderr=subprocess.STDOUT, check=True)
    result = subprocess.run([str(exe)], cwd=root, env=env, stdout=stream, stderr=subprocess.STDOUT, timeout=20)
print(log.read_text(encoding='utf-8'), end='')
raise SystemExit(result.returncode)
