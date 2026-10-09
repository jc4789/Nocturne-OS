"""New delivery boundary only: actual C helpers and actual four JS modules."""
from pathlib import Path
import os
import subprocess
import sys

root = Path(__file__).resolve().parent.parent
out = root / "build/goal-20261009"
source = (root / "user/libc/web/frame_native.h").read_text(encoding="utf-8")
start = source.index("static web_doc *frame_message_receiver(")
helpers = source[start:source.index("static JSValue native_frame(",start)]
native_commit = source[source.index("struct frame_transfer_write"):start]
if "--commit-fault-only" in sys.argv:
    native_commit = '''static int commit_fail_index=-1,commit_allocation_attempt;
static bool commit_deny_allocation,commit_retire_destination;
static void *commit_materialize_alloc(JSContext *ctx,size_t n,bool zero){
    if(commit_allocation_attempt++==commit_fail_index){JS_ThrowOutOfMemory(ctx);return NULL;}
    return zero?js_mallocz(ctx,n):js_malloc(ctx,n);
}
''' + native_commit.replace('js_malloc(ctx,nb*sizeof *buffers)','commit_materialize_alloc(ctx,nb*sizeof *buffers,false)').replace('js_mallocz(ctx,nw*sizeof *writes)','commit_materialize_alloc(ctx,nw*sizeof *writes,true)').replace('js_malloc(ctx,nc*sizeof *cancels)','commit_materialize_alloc(ctx,nc*sizeof *cancels,false)').replace('    /* No allocation, JS_Call, property creation', '    if(commit_deny_allocation)JS_SetMemoryLimit(JS_GetRuntime(ctx),1);\n    /* No allocation, JS_Call, property creation')
helpers = native_commit + helpers
helpers = '''static bool fail_transfer_task_allocation;
static void *transfer_task_malloc(JSContext *ctx,size_t n){
    if(fail_transfer_task_allocation){fail_transfer_task_allocation=false;JS_ThrowOutOfMemory(ctx);return NULL;}
    return js_malloc(ctx,n);
}
''' + helpers.replace('struct js_posted_task *p=js_malloc(dest,sizeof *p);','struct js_posted_task *p=transfer_task_malloc(dest,sizeof *p);')
start = source.index('}else if(!strcmp(op,"messageMethod")) {')
method = source[start+len('}else if(!strcmp(op,"messageMethod")) {'):source.index("}else {",start)]
prefix = (root / "tests/frame_messages_native_prefix.c").read_text(encoding="utf-8")
prefix = prefix.replace('if(!strcmp(op,"messageBrand")){', 'if(!strcmp(op,"transferGeneration"))result=wrap(s,s->doc->root);\n    else if(!strcmp(op,"transferCommit"))result=frame_transfer_commit(ctx,s,argv[1]);\n    else if(!strcmp(op,"messageBrand")){')
if "--commit-fault-only" in sys.argv:
    prefix = prefix.replace('else if(!strcmp(op,"transferCommit"))result=frame_transfer_commit(ctx,s,argv[1]);','else if(!strcmp(op,"transferCommit")){if(commit_retire_destination){commit_retire_destination=false;s->doc->live=false;}result=frame_transfer_commit(ctx,s,argv[1]);}')
case_name = "frame_commit_fault_cases.c" if "--commit-fault-only" in sys.argv else "frame_transfers_poison_cases.c" if "--poison-only" in sys.argv else "frame_transfers_native_cases.c"
cases = (root / "tests" / case_name).read_text(encoding="utf-8")
support = (root / "tests/frame_messages_native_cases.c").read_text(encoding="utf-8")
cases = support[:support.index("int main(void)")] + cases
prefix = prefix.replace('static JSValue host_cancel(JSContext *ctx,JSValueConst receiver,int argc,JSValueConst *argv){return JS_UNDEFINED;}', '''static JSValue host_cancel(JSContext *ctx,JSValueConst receiver,int argc,JSValueConst *argv){
    struct web_js_state *s=state(ctx);uint32_t id;if(!argc||JS_ToUint32(ctx,&id,argv[0]))return JS_EXCEPTION;
    struct js_posted_task **link=&s->posted,*previous=NULL;
    while(*link){struct js_posted_task *p=*link;if(p->id==id){*link=p->next;if(s->last_posted==p)s->last_posted=previous;s->posted_count--;JS_FreeValue(ctx,p->fn);js_free(ctx,p);break;}previous=p;link=&p->next;}
    return JS_UNDEFINED;
}''')
label = "frame-commit-fault" if "--commit-fault-only" in sys.argv else "frame-transfers-poison" if "--poison-only" in sys.argv else "frame-transfers-native"
c = out / (label+"-generated.c")
encoding=(root / "user/libc/web/js_encoding.c").read_text(encoding="utf-8")
encoding=encoding[encoding.index("static size_t decode_utf8("):encoding.index("void web_js_encoding_init(")]
util=(root / "user/libc/web/util.c").read_text(encoding="utf-8")
at=util.index("int utf8_put(")
encoding=util[at:util.index("\n}",at)+2]+"\n"+encoding
c.write_text(prefix.replace("/* NATIVE_MESSAGE_FUNCTIONS */",helpers).replace("/* NATIVE_MESSAGE_METHOD */",method).replace("/* NATIVE_UTF8_FUNCTIONS */",encoding)+cases,encoding="utf-8")
js = """
globalThis.DOMException=class DOMException extends Error{constructor(m,n){super(m);this.name=n}};
(function(host){'use strict';
const elementURL={string:String},handlers=new WeakMap();let messageHandlerTarget=()=>false;
class Event{constructor(type){this.type=String(type)}initEvent(type){this.type=String(type)}}
class EventTarget{};const handlerValue=(v,t)=>handlers.get(v)?.[t]??null;
const setHandler=(v,t,f)=>{let h=handlers.get(v);if(!h)handlers.set(v,h={});h[t]=f};
function dispatch(target,event){if(target===globalThis)received.push(event);const handler=target===globalThis?target.onmessage:handlerValue(target,event.type);if(handler)handler.call(target,event)}
globalThis.received=[];class Document{};class HTMLIFrameElement{};class HTMLFrameElement{};
const document=globalThis.document={};const rawDom=()=>9,reflectedAttr=()=>null,customElementsBridge={constructing:()=>false};
"""
if "--poison-only" in sys.argv:
    # The original fixture's push runs a deliberately poisoned public index
    # setter. Keep this *unexecuted* repair separate from the recorded failure.
    js = js.replace("received.push(event)","Object.defineProperty(received,received.length,{value:event,writable:true,enumerable:true,configurable:true})")
for name in ("js_encoding.js","js_url.js","js_clone.js","js_messaging.js","js_frames.js"):
    js += (root / "user/libc/web" / name).read_text(encoding="utf-8")+"\n"
js += "return {frameWindowProxy:frameBridge.windowProxy,windowMessageMethod:messagingBridge.postMessage,windowMessageLocal:messagingBridge.local,windowMessagePrepare:messagingBridge.prepare,windowMessageImport:messagingBridge.importPacket,windowMessageCommit:messagingBridge.commitImported,windowMessageReceive:messagingBridge.receive};})(fixtureHost);"
(out / "frame-transfer-bindings.js").write_text(js,encoding="utf-8")
cc=root / "tools/msys64/ucrt64/bin/clang.exe"
env=dict(os.environ,PATH=str(cc.parent)+os.pathsep+os.environ.get("PATH",""))
exe=out / (label+".exe")
engine=[root / ("third_party/quickjs/"+n+".c") for n in ("quickjs","dtoa","libregexp","libunicode","cutils")]
subprocess.run([str(cc),"-std=gnu11","-O1","-fwrapv","-funsigned-char","-DCONFIG_NOCTURNE",'-DCONFIG_VERSION="Nocturne-test"',"-Ithird_party/quickjs","-Ibuild/nocturne-audit/url-host-include",str(c),*map(str,engine),"-lm","-o",str(exe)],cwd=root,env=env,check=True)
subprocess.run([str(exe)],cwd=root,env=env,check=True,timeout=30)
