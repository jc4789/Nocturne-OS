/* 次23だけ。旧transfer/helper/実サイトを再走しない。 */
const checks=(name,ok)=>__check(name,!!ok),throws=(fn,name)=>{try{fn();return false;}catch(e){return e.name===name;}};
let c,seen=[],transferred,other,replacement,received,b,closedChannel,leaks=0;
globalThis.__suite=[
 {steps:1,run(){
    checks('constructors-installed',typeof MessageChannel==='function'&&typeof MessagePort==='function');
    c=new MessageChannel();checks('real-eventtarget-ports',c.port1 instanceof MessagePort&&c.port2 instanceof EventTarget);
    checks('port-illegal-constructor',throws(()=>new MessagePort(),'TypeError'));
    checks('borrowed-port-brand',throws(()=>MessagePort.prototype.start.call({}),'TypeError'));
    c.port2.addEventListener('message',e=>{seen.push('m'+e.data);Promise.resolve().then(()=>seen.push('u'+e.data));});
    c.port1.postMessage(1);c.port1.postMessage(2);checks('posted-not-synchronous',seen.length===0);
 }},
 {steps:2,run(){checks('listeners-do-not-start-disabled-queue',seen.length===0);c.port2.start();}},
 {steps:0,run(){
    checks('fifo-and-task-microtask-checkpoint',seen.join(',')==='m1,u1,m2,u2');
    checks('required-post-argument',throws(()=>c.port1.postMessage(),'TypeError'));
    checks('duplicate-port-rejected',throws(()=>c.port1.postMessage(null,[c.port2,c.port2]),'DataCloneError'));
    checks('sending-port-rejected',throws(()=>c.port1.postMessage(null,[c.port1]),'DataCloneError'));
    checks('port-not-ordinary-clone',throws(()=>structuredClone(c.port1),'DataCloneError'));
    checks('worker-ipc-external-port-rejected',throws(()=>postMessage({p:c.port1},[c.port1]),'DataCloneError'));
    checks('channel-not-ordinary-clone',throws(()=>structuredClone(c),'DataCloneError'));
    checks('messageevent-brand',throws(()=>Object.getOwnPropertyDescriptor(MessageEvent.prototype,'data').get.call({}),'TypeError'));
    checks('messageevent-symbol-domstring',throws(()=>new MessageEvent(Symbol()),'TypeError'));
 }},
 {steps:1,run(){
    transferred=new MessageChannel();other=new MessageChannel();b=new ArrayBuffer(3);new Uint8Array(b).set([4,5,6]);
    transferred.port1.postMessage('already-queued');
    other.port2.onmessage=e=>{received=e;replacement=e.ports[0];};
    const data={bytes:new Uint8Array(b),port:transferred.port2};data.self=data;
    other.port1.postMessage(data,{transfer:[b,transferred.port2]});
    checks('buffer-detached-at-accepted-send',b.byteLength===0);
    checks('source-port-detached',throws(()=>structuredClone(transferred.port2,{transfer:[transferred.port2]}),'DataCloneError'));
 }},
 {steps:1,run(){
    checks('cloned-cycle-typed-bytes',received.data.self===received.data&&received.data.bytes[2]===6);
    checks('event-transferred-port-identity',received.data.port===replacement&&replacement instanceof MessagePort&&Object.isFrozen(received.ports));
    received=null;replacement.addEventListener('message',e=>received=e.data);
    replacement.start();
 }},
 {steps:1,run(){
    checks('transferred-endpoint-keeps-queued-message',received==='already-queued');
    transferred.port1.postMessage('new-message');
 }},
 {steps:0,run(){
    checks('transferred-entanglement-still-real',received==='new-message');
    const channel=new MessageChannel(),buffer=new ArrayBuffer(7);let deliveries=0;
    channel.port2.onmessage=()=>deliveries++;
    globalThis.__denyCommit=true;
    checks('native-preflight-oom-surfaces',throws(()=>channel.port1.postMessage({buffer},{transfer:[buffer]}),'InternalError'));
    globalThis.__denyCommit=false;
    checks('native-preflight-oom-sender-buffer-preserved',buffer.byteLength===7);
    globalThis.__oomDeliveryCount=()=>deliveries;
 }},
 {steps:1,run(){
    checks('preflight-failure-no-partial-queue-publication',__oomDeliveryCount()===0);
    closedChannel=new MessageChannel();closedChannel.port2.onmessage=()=>checks('closed-port-must-not-dispatch',false);
    closedChannel.port1.postMessage(1);closedChannel.port2.close();
 }},
 {steps:1,run(){
    // 未実行補正：HTML close()はDetached=true、閉鎖Portのtransferは拒否。
    checks('closed-port-transfer-rejected',throws(()=>structuredClone(closedChannel.port2,{transfer:[closedChannel.port2]}),'DataCloneError'));
    let once=0;const o=new MessageChannel();o.port2.addEventListener('message',()=>once++,{once:true});o.port2.start();o.port1.postMessage(1);o.port1.postMessage(2);
    globalThis.__onceCount=()=>once;
    globalThis.__onceChannel=o;
 }},
 {steps:1,run(){}},
 {steps:2,run(){
    checks('port-once-listener-order',__onceCount()===1);
    const x=new MessageChannel();let value=null;x.port2.onmessage=e=>value=e.data;
    const moved=structuredClone(x.port1,{transfer:[x.port1]});moved.postMessage('via-clone');
    globalThis.__cloneDelivered=()=>value;
    const poison=['endpoint','owner','head','tail','scheduled','enabled','cancelled','committed'];
    for(const name of poison)Object.defineProperty(Object.prototype,name,{set(){leaks++;},configurable:true});
    const p=new MessageChannel();p.port2.onmessage=()=>{};p.port1.postMessage(3);p.port2.close();
    for(const name of poison)delete Object.prototype[name];
 }},
 {steps:2,run(){
    checks('structured-clone-port-real-entanglement',__cloneDelivered()==='via-clone');
    checks('private-records-no-prototype-setter-leak',leaks===0);
    const q=new MessageChannel();let order=[];q.port2.onmessage=e=>order.push('port'+e.data);
    q.port1.postMessage(1);q.port1.postMessage(2);setTimeout(()=>order.push('timer'),1);
    globalThis.__fairOrder=()=>order;
    globalThis.__advanceClock=true;
 }},
 {steps:1,run(){checks('timer-port-fair-work',__fairOrder().includes('timer')&&__fairOrder().includes('port1'));}},
 {steps:0,run(){
    const a=new MessageChannel(),buf=new ArrayBuffer(4);a.port2.onmessage=()=>checks('closed-worker-must-not-dispatch',false);a.port1.postMessage(2);
    close();checks('retired-native-commit-rejected',throws(()=>structuredClone(buf,{transfer:[buf]}),'TypeError'));
    checks('retired-commit-keeps-source',buf.byteLength===4);
 }}
];
