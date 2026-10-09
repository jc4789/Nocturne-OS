"""New delivery boundary only: actual C helpers and actual four JS modules."""
from pathlib import Path
import os
import subprocess

root = Path(__file__).resolve().parent.parent
out = root / "build/goal-20261009"
source = (root / "user/libc/web/frame_native.h").read_text(encoding="utf-8")
start = source.index("static web_doc *frame_message_receiver(")
helpers = source[start:source.index("static JSValue native_frame(",start)]
start = source.index('}else if(!strcmp(op,"messageMethod")) {')
method = source[start+len('}else if(!strcmp(op,"messageMethod")) {'):source.index("}else {",start)]
prefix = (root / "tests/frame_messages_native_prefix.c").read_text(encoding="utf-8")
cases = (root / "tests/frame_messages_native_cases.c").read_text(encoding="utf-8")
c = out / "frame-messages-native-generated.c"
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
for name in ("js_encoding.js","js_url.js","js_clone.js","js_messaging.js","js_frames.js"):
    js += (root / "user/libc/web" / name).read_text(encoding="utf-8")+"\n"
js += "return {frameWindowProxy:frameBridge.windowProxy,windowMessageMethod:messagingBridge.postMessage,windowMessageLocal:messagingBridge.local,windowMessagePrepare:messagingBridge.prepare,windowMessageReceive:messagingBridge.receive};})(fixtureHost);"
(out / "frame-message-bindings.js").write_text(js,encoding="utf-8")
cc=root / "tools/msys64/ucrt64/bin/clang.exe"
env=dict(os.environ,PATH=str(cc.parent)+os.pathsep+os.environ.get("PATH",""))
exe=out / "frame-messages-native.exe"
engine=[root / ("third_party/quickjs/"+n+".c") for n in ("quickjs","dtoa","libregexp","libunicode","cutils")]
subprocess.run([str(cc),"-std=gnu11","-O1","-fwrapv","-funsigned-char","-DCONFIG_NOCTURNE",'-DCONFIG_VERSION="Nocturne-test"',"-Ithird_party/quickjs","-Ibuild/nocturne-audit/url-host-include",str(c),*map(str,engine),"-lm","-o",str(exe)],cwd=root,env=env,check=True)
subprocess.run([str(exe)],cwd=root,env=env,check=True,timeout=30)
