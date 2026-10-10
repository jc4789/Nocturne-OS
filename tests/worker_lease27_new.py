"""新27捕捉target/close競合だけ。旧suite/実サイトは再走しない。"""
from pathlib import Path
import datetime, hashlib, json, os, subprocess
root=Path(__file__).resolve().parents[1];out=root/'build/goal-20261009/worker-lease27-new-once'
out.mkdir(parents=True,exist_ok=True)
result=out/'result.json'
if (out/'result.log').exists() or result.exists():raise SystemExit('同suite再走禁止')
base=root/'user/libc/web';native=(base/'js_worker.c').read_text(encoding='utf-8')
parts=[native[native.index('struct packet {'):native.index('static struct child *retired;')],
 native[native.index('static struct child *find('):native.index('static void release_packet(')],
 native[native.index('static void broker_retire('):native.index('static void event(')]]
(out/'native-extract.h').write_text('\n'.join(parts),encoding='utf-8')
prelude='''(function(host){delete globalThis.__host;
class DOMException extends Error{constructor(m='',n='Error'){super(m);this.name=n;}}
class Event{constructor(t,i={}){this.type=t;this.cancelable=!!i.cancelable;}initEvent(t){this.type=t;}}
const handlers=new WeakMap();function handlerValue(t,k){return handlers.get(t)?.[k]??null;}
function setHandler(t,k,v){let h=handlers.get(t);if(!h){h={};handlers.set(t,h);}h[k]=typeof v==='function'?v:null;}
class EventTarget{addEventListener(t,f){(this._listeners??={})[t]=f;}dispatchEvent(e){this._listeners?.[e.type]?.call(this,e);const h=handlerValue(this,e.type);h?.call(this,e);return true;}}
Object.assign(globalThis,{DOMException,Event,EventTarget});class URL{get origin(){return 'https://fixture.test';}}
const elementURL={string:String},dispatch=(t,e)=>EventTarget.prototype.dispatchEvent.call(t,e);let messageHandlerTarget;
const posted=new Map();let next=0;host.postTask=f=>{posted.set(++next,f);return next;};host.cancelPost=id=>posted.delete(id);host.origin=()=> 'https://fixture.test';
'''
tail='''
globalThis.makeImported=id=>messagingBridge.importWorker([[[1,0],[['Object',0,[['data',[1,1]],['ports',[1,2]]]],['ExternalTransferred',0],['Array',1,[['0',[1,1]]]]]],[[id,1,[]]]],1).data;
globalThis.enqueueImported=(id,value)=>messagingBridge.receiveWorkerPort(1,id,1,cloneData.prepare({data:value,ports:[]},[]).data);
globalThis.pumpTask=()=>{const entry=posted.entries().next();if(!entry.done){posted.delete(entry.value[0]);entry.value[1]();}};
})(__host);'''
(out/'public.js').write_text(prelude+(base/'js_clone.js').read_text(encoding='utf-8')+(base/'js_messaging.js').read_text(encoding='utf-8')+tail,encoding='utf-8')
cc=root/'tools/msys64/ucrt64/bin/clang.exe';env=dict(os.environ,PATH=str(cc.parent)+os.pathsep+os.environ.get('PATH',''))
engine=[str(root/f'third_party/quickjs/{name}.c') for name in ['quickjs','dtoa','libregexp','libunicode','cutils']]
binary=out/'worker-lease27.exe';cmd=[str(cc),'-std=gnu11','-O1','-fwrapv','-funsigned-char','-DCONFIG_NOCTURNE','-DCONFIG_VERSION="Nocturne-test"','-Ithird_party/quickjs','-Iuser/libc/web','-Ibuild/nocturne-audit/url-host-include',str(root/'tests/worker_lease27_new.c'),*engine,'-lm','-o',str(binary)]
with (out/'result.log').open('x',encoding='utf-8') as log:
    log.write('UTC '+datetime.datetime.now(datetime.timezone.utc).isoformat()+'\n')
    for file in ['js_worker.c','js_worker.h','js_worker_port_lifetime.h','js_messaging.js','js_clone.js']:
        log.write(file+' SHA256 '+hashlib.sha256((base/file).read_bytes()).hexdigest()+'\n')
    syntax=subprocess.run(['C:/nvm4w/nodejs/node.exe','--check',str(out/'public.js')],cwd=root,env=env,stdout=log,stderr=subprocess.STDOUT);log.write('FIXTURE_JS_SYNTAX_EXIT '+str(syntax.returncode)+'\n');log.flush()
    if syntax.returncode:
        result.write_text(json.dumps(dict(fixture_js_syntax_exit=syntax.returncode,compile_exit=None,runtime_exit=None),indent=2),encoding='utf-8');raise SystemExit(syntax.returncode)
    log.write('COMPILE '+repr(cmd)+'\n');log.flush();built=subprocess.run(cmd,cwd=root,env=env,stdout=log,stderr=subprocess.STDOUT);log.write('COMPILE_EXIT '+str(built.returncode)+'\n');log.flush()
    record=dict(compile_exit=built.returncode,runtime_exit=None)
    if not built.returncode:
        ran=subprocess.run([str(binary)],cwd=root,env=env,stdout=log,stderr=subprocess.STDOUT,timeout=60);record['runtime_exit']=ran.returncode;log.write('RUNTIME_EXIT '+str(ran.returncode)+'\n')
    result.write_text(json.dumps(record,indent=2),encoding='utf-8')
    raise SystemExit(record['runtime_exit'] if record['runtime_exit'] is not None else built.returncode)
