"""新26初回のみ。旧suiteは実行しない。実OS pipe/spawn受入ではない。"""
from pathlib import Path
import re,subprocess,hashlib,datetime,os
root=Path(__file__).resolve().parent.parent
out=root/'build/goal-20261009/worker-broker26-new-once';out.mkdir(parents=True,exist_ok=True)
base=root/'user/libc/web'
source=(base/'js_worker_runtime.js').read_text(encoding='utf-8')
source=re.sub(r'/\* @include ([a-z_]+\.js) \*/',lambda m:(base/m[1]).read_text(encoding='utf-8'),source)
(out/'child-bootstrap.js').write_text(source,encoding='utf-8')
prelude='''(function(host){delete globalThis.__parentHost;
class DOMException extends Error{constructor(m='',n='Error'){super(m);this.name=n;}}
class Event{constructor(t,i={}){this.type=t;this.cancelable=!!i.cancelable;}initEvent(t){this.type=t;}}
class EventTarget{addEventListener(t,f){(this._listeners??={})[t]=f;}dispatchEvent(e){this._listeners?.[e.type]?.call(this,e);this['on'+e.type]?.call(this,e);return true;}}
Object.assign(globalThis,{DOMException,Event,EventTarget});
class URL{get origin(){return 'https://fixture.test';}}
const elementURL={string:String},dispatch=(t,e)=>EventTarget.prototype.dispatchEvent.call(t,e);
const handlers=new WeakMap();function handlerValue(t,k){return handlers.get(t)?.[k]??null;}
function setHandler(t,k,v){let h=handlers.get(t);if(!h){h={};handlers.set(t,h);}h[k]=typeof v==='function'?v:null;Object.defineProperty(t,'on'+k,{value:h[k],writable:true,configurable:true});}
let messageHandlerTarget;const posted=new Map();let next=0;
host.postTask=f=>{posted.set(++next,f);return next;};host.cancelPost=id=>posted.delete(id);host.origin=()=> 'https://fixture.test';
'''
# Avoid defining an own onmessage over the actual public prototype setter.
prelude=prelude.replace("Object.defineProperty(t,'on'+k,{value:h[k],writable:true,configurable:true});",'')
prelude=prelude.replace("this['on'+e.type]?.call(this,e);","const h=handlerValue(this,e.type);h?.call(this,e);")
parent=prelude+(base/'js_clone.js').read_text(encoding='utf-8')+(base/'js_messaging.js').read_text(encoding='utf-8')+'''
return {receive(data,op,id,creator){if(op===2){globalThis.received=messagingBridge.importWorker(data,1);}
else messagingBridge.receiveWorkerPort(1,id,creator,data,op===9);},tick(){const e=posted.entries().next();if(!e.done){posted.delete(e.value[0]);e.value[1]();}},send(data,list){const p=messagingBridge.prepareWorker(data,list,1);try{host.worker(3,1,p.packet,p.plan);}catch(e){messagingBridge.abortWorker(p);throw e;}},close(){messagingBridge.closeWorker(1);}};
})(__parentHost);'''
parent=parent.replace('return {receive(data,op,id,creator)', 'return globalThis.parentBridge={receive(data,op,id,creator)')
(out/'parent-bootstrap.js').write_text(parent,encoding='utf-8')
native=(base/'js_worker.c').read_text(encoding='utf-8');child=(root/'user/apps/browserjsworker.c').read_text(encoding='utf-8')
parts=[native[native.index('struct packet {'):native.index('static struct child *retired;')],
native[native.index('static struct child *find('):native.index('static void release_packet(')],
native[native.index('web_worker_publication *web_worker_prepare('):native.index('static void event(')],
child[child.index('struct deferred {'):child.index('static void output_flush(')],
child[child.index('static struct worker *state('):child.index('static JSValue native_transfer_commit(')]]
(out/'native-extract.h').write_text('\n'.join(parts),encoding='utf-8')
phases=[
'''globalThis.left=new MessageChannel();globalThis.windowMessages=[];left.port1.onmessage=e=>windowMessages.push(e.data);
left.port1.postMessage('before-start');parentBridge.send({port:left.port2},[left.port2]);
__check('window-source-wrapper-detached-after-native-publish',(()=>{try{structuredClone(left.port2,{transfer:[left.port2]});return false;}catch(e){return e.name==='DataCloneError';}})());''',
'''globalThis.childMessages=[];globalThis.fromWindow=null;onmessage=e=>{fromWindow=e.data.port;__check('worker-import-real-port-brand',fromWindow instanceof MessagePort&&e.ports[0]===fromWindow);fromWindow.onmessage=x=>{childMessages.push(x.data);Promise.resolve().then(()=>childMessages.push('microtask'));};};
__check('worker-not-started-yet',fromWindow===null);''',
'''left.port1.postMessage('after-start');''',
'''__check('transferred-queue-and-next-message-fifo',childMessages.join('|')==='before-start|microtask|after-start|microtask');
fromWindow.postMessage({reply:7});globalThis.right=new MessageChannel();right.port1.onmessage=e=>{globalThis.reverse=e.data;};right.port1.postMessage('reverse-queued');postMessage({port:right.port2},[right.port2]);
__check('worker-source-detached-after-native-publish',(()=>{try{structuredClone(right.port2,{transfer:[right.port2]});return false;}catch(e){return e.name==='DataCloneError';}})());''',
'''__check('worker-to-window-entangled-reply',windowMessages.length===1&&windowMessages[0].reply===7);
__check('window-import-real-worker-port-brand',received.port instanceof MessagePort);globalThis.reverseMessages=[];received.port.onmessage=e=>reverseMessages.push(e.data);received.port.postMessage('reverse-reply');
const a=new ArrayBuffer(4);let rejected=false;try{parentBridge.send({p:received.port,a},[a,received.port]);}catch(e){rejected=e.name==='DataCloneError';}__check('broker-retransfer-rejected-before-ab-detach',rejected&&a.byteLength===4);
const d=new MessageChannel(),b=new ArrayBuffer(3);__fault('broker-preflight-native-reject');let oom=false;try{parentBridge.send({p:d.port2,b},[b,d.port2]);}catch(e){oom=true;}__fault('broker-preflight-native-allow');__check('native-publication-failure-preserves-both-sources',oom&&b.byteLength===3);d.port1.postMessage(99);globalThis.recovered=false;d.port2.onmessage=e=>recovered=e.data===99;''',
'''__check('reverse-worker-peer-receives-window-data',reverse==='reverse-reply');fromWindow.close();''',
'''__check('reverse-moved-queue-start-preserved',reverseMessages.join('|')==='reverse-queued');__check('preflight-failure-entanglement-still-live',recovered);const buffer=new ArrayBuffer(2);left.port1.postMessage(buffer,[buffer]);__check('closed-broker-peer-discards-with-real-buffer-detach',buffer.byteLength===0);parentBridge.close();''',
'''void 0;'''
]
# Receiver must exist before the first queued IPC is delivered.
phases[0]="globalThis.__initial=1;"+phases[0]
source=source.replace("return {\n        start(r)","globalThis.onmessage=e=>{globalThis.fromWindow=e.data.port;globalThis.childMessages=[];__check('worker-import-real-port-brand',fromWindow instanceof MessagePort&&e.ports[0]===fromWindow);};\n    return {\n        start(r)")
(out/'child-bootstrap.js').write_text(source,encoding='utf-8')
phases[1]="__check('worker-port-disabled-keeps-transferred-queue',childMessages.length===0);fromWindow.onmessage=x=>{childMessages.push(x.data);Promise.resolve().then(()=>childMessages.push('microtask'));};"
for i,s in enumerate(phases):(out/f'phase{i}.js').write_text(s,encoding='utf-8')
cc=root/'tools/msys64/ucrt64/bin/clang.exe';env=dict(os.environ,PATH=str(cc.parent)+os.pathsep+os.environ.get('PATH',''))
engine=[str(root/f'third_party/quickjs/{name}.c') for name in ['quickjs','dtoa','libregexp','libunicode','cutils']]
binary=out/'worker-broker26-new.exe'
cmd=[str(cc),'-std=gnu11','-O1','-fwrapv','-funsigned-char','-DCONFIG_NOCTURNE','-DCONFIG_VERSION="Nocturne-test"','-Ithird_party/quickjs','-Ibuild/nocturne-audit/url-host-include',str(root/'tests/worker_broker26_new.c'),*engine,'-lm','-o',str(binary)]
with (out/'result.log').open('x',encoding='utf-8') as log:
    log.write('UTC '+datetime.datetime.now(datetime.timezone.utc).isoformat()+'\n')
    for f in ['js_worker.c','js_worker.h','js_worker_transfer.h','js_clone.js','js_messaging.js','js_worker_messaging.js','js_worker_runtime.js','js_worker.js']:
        log.write(f+' SHA256 '+hashlib.sha256((base/f).read_bytes()).hexdigest()+'\n')
    log.write('COMPILE '+repr(cmd)+'\n');log.flush();r=subprocess.run(cmd,cwd=root,env=env,stdout=log,stderr=subprocess.STDOUT);log.write('COMPILE_EXIT '+str(r.returncode)+'\n');log.flush()
    if r.returncode:raise SystemExit(r.returncode)
    r=subprocess.run([str(binary)],cwd=root,env=env,stdout=log,stderr=subprocess.STDOUT,timeout=60);log.write('RUNTIME_EXIT '+str(r.returncode)+'\n');raise SystemExit(r.returncode)
