/* Production JS module, with a private native-contract adapter. This verifies
 * IDB transaction/key/cursor semantics, not native disk atomicity or sites. */
const fs=require('node:fs'),vm=require('node:vm'),assert=require('node:assert/strict'),v8=require('node:v8'),crypto=require('node:crypto');
const data=new Map(),clients=new Map();let serial=0,checks=0,payloadWrites=0;
function hostIDB(op,a,b,c){
    if(op==='begin'){let d=data.get(a);if(!d){d={name:a,meta:null,values:new Map(),lease:0};data.set(a,d);}if(d.lease)return[1,0,null];const id=++serial;d.lease=id;return[0,id,d.meta===null?null:v8.deserialize(v8.serialize(d.meta))];}
    if(op==='poll'){const r=clients.get(a);if(!r)return[3,null];if(r.forced){r.forced=false;return[0,null,true];}const wanted=r.pending;r.pending=null;return[0,wanted];}
    if(op==='disconnect'){clients.delete(a);return 0;}
    if(op==='fired'){const r=clients.get(a);if(!r)return 3;r.fired=true;return 0;}
    let d=[...data.values()].find(d=>d.lease===a);if(!d)return op==='stage'||op==='value'||op==='values'||op==='connect'||op==='connections'?[3,null]:3;
    if(op==='abort'){d.lease=0;return 0;}
    if(op==='stage'){const bytes=v8.serialize(b),ref=crypto.createHash('sha256').update(bytes).digest('hex');if(!d.values.has(ref)){d.values.set(ref,v8.deserialize(bytes));payloadWrites++;}return[0,ref];}
    if(op==='value')return d.values.has(b)?[0,v8.deserialize(v8.serialize(d.values.get(b)))]:[3,null];
    if(op==='values')return b.every(ref=>d.values.has(ref))?[0,b.map(ref=>v8.deserialize(v8.serialize(d.values.get(ref))))]:[3,[]];
    if(op==='commit'){assert(Array.isArray(c));d.meta=v8.deserialize(v8.serialize(b));d.lease=0;return 0;}
    if(op==='connect'){const id=++serial;clients.set(id,{name:d.name,version:b,pending:null,notified:undefined});return[0,id];}
    if(op==='connections'){let n=0,unfired=0;for(const r of clients.values())if(r.name===d.name){n++;if(r.notified!==b){r.pending=b;r.notified=b;r.fired=false;}if(!r.fired)unfired++;}return[0,n,unfired];}
    throw new Error(op);
}
const handlers=new WeakMap(),context={console,setTimeout,Event,EventTarget,DOMException,structuredClone,indexedDBHandlerTarget:()=>false,host:{indexedDB:hostIDB,pack:x=>x,unpack:x=>x},
    cloneData:{serialize:x=>structuredClone(x),deserialize:x=>structuredClone(x),registerUncloneable(){}},eventState:()=>({}),
    dispatch:(t,e)=>t.dispatchEvent(e),installHandlers:(p,events)=>{for(const type of events)Object.defineProperty(p,'on'+type,{configurable:true,get(){return handlers.get(this)?.[type]||null;},set(fn){let h=handlers.get(this);if(!h){h={};handlers.set(this,h);}if(h[type])this.removeEventListener(type,h[type]);h[type]=typeof fn==='function'?fn:null;if(h[type])this.addEventListener(type,h[type]);}});}};
vm.createContext(context);
// The realm's DOMException owner, as the bootstrap includes it before js_indexeddb.js.
vm.runInContext(fs.readFileSync('user/libc/web/js_dom_exception.js','utf8'),context);
context.bridge=vm.runInContext(fs.readFileSync('user/libc/web/js_indexeddb.js','utf8')+'\nindexedDBBridge;',context);
/* The global's own indexedDB accessor answers its realm's global; node's vm calls a global
   accessor with the outer sandbox object as this, so the getter is called explicitly. */
const run=code=>vm.runInContext(code,context),idb=()=>run("Object.getOwnPropertyDescriptor(globalThis,'indexedDB').get.call(globalThis)"),request=r=>new Promise((ok,bad)=>{r.onsuccess=()=>ok(r.result);r.onerror=()=>bad(r.error);});
const check=(name,yes)=>{checks++;assert(yes,name);};
async function test(){
    const r=idb().open('segments',1);r.onupgradeneeded=()=>{check('oldVersion zero',r.result.version===1);const s=r.result.createObjectStore('segments',{keyPath:'id',autoIncrement:true});s.createIndex('label','label',{unique:true});s.createIndex('__proto__','tags',{multiEntry:true});};
    const db=await request(r);context.db=db;check('schema persisted',db.objectStoreNames.contains('segments'));
    let tx=db.transaction('segments','readwrite'),s=tx.objectStore('segments');
    const a=s.add(run('({label:"first",tags:["x","x","y"],bytes:new Uint8Array([1,2,3])})'));
    const b=s.add(run('({label:"second",tags:["z"],bytes:new Uint8Array([4,5])})'));
    check('first auto key',await request(a)===1);check('second auto key',await request(b)===2);await new Promise((ok,bad)=>{tx.oncomplete=ok;tx.onabort=()=>bad(tx.error);});
    check('only changed payloads staged',payloadWrites===2);
    tx=db.transaction('segments');s=tx.objectStore('segments');const read=await request(s.get(1));check('binary round trip',read.bytes[2]===3&&read.id===1);
    check('index key query',await request(db.transaction('segments').objectStore('segments').index('label').getKey('second'))===2);
    check('multiEntry distinct keys',await request(db.transaction('segments').objectStore('segments').index('__proto__').count('x'))===1);
    check('null optional query is unbounded',await request(db.transaction('segments').objectStore('segments').count(null))===2);
    let invalidQuery=false;try{db.transaction('segments').objectStore('segments').get(undefined);}catch(e){invalidQuery=e.name==='DataError';}check('undefined required key rejected',invalidQuery);
    tx=db.transaction('segments','readwrite');s=tx.objectStore('segments');const bad=s.add(run('({label:"first",tags:[]})'));bad.onerror=e=>e.preventDefault();await new Promise(ok=>bad.addEventListener('error',ok));check('constraint real error',bad.error.name==='ConstraintError');await new Promise(ok=>tx.oncomplete=ok);
    tx=db.transaction('segments','readwrite');s=tx.objectStore('segments');const aborted=s.put(run('({id:1,label:"changed",tags:[]})'));await request(aborted);tx.abort();await new Promise(ok=>tx.onabort=ok);check('abort retains committed value',(await request(db.transaction('segments').objectStore('segments').get(1))).label==='first');
    const keys=[];await new Promise((ok,bad)=>{const cursor=db.transaction('segments').objectStore('segments').openCursor();cursor.onerror=()=>bad(cursor.error);cursor.onsuccess=()=>{const c=cursor.result;if(!c){ok();return;}keys.push(c.key);c.continue();};});check('cursor order and reused request',keys.join(',')==='1,2');
    check('key type order',idb().cmp(1,run('new Date(0)'))<0&&idb().cmp(run('new Date(0)'),'a')<0&&idb().cmp('a',run('new Uint8Array([0]).buffer'))<0);
    check('range open bound',!context.IDBKeyRange.bound(1,2,true,false).includes(1));
    const store=()=>db.transaction('segments').objectStore('segments');
    const records=await request(store().getAllRecords({direction:'prev',count:1}));
    check('getAllRecords prev count',records.length===1&&Object.prototype.toString.call(records[0])==='[object IDBRecord]'&&records[0].key===2&&records[0].primaryKey===2&&records[0].value.label==='second');
    check('getAll options ignore the count argument',(await request(store().getAll({query:context.IDBKeyRange.lowerBound(1)},1))).length===2);
    check('getAll count zero is unlimited',(await request(store().getAll(null,0))).length===2);
    check('getAllKeys key and count',(await request(store().getAllKeys(2,5))).join(',')==='2');
    check('index getAllRecords nextunique',(await request(store().index('__proto__').getAllRecords({direction:'nextunique'}))).map(r=>r.key+':'+r.primaryKey).join(',')==='x:1,y:1,z:2');
    let enforced=0;for(const count of [NaN,Infinity,-1])try{store().getAll(null,count);}catch(e){if(e.name==='TypeError')enforced++;}check('getAll count EnforceRange',enforced===3);
    let direction=false;try{store().getAllRecords({direction:'sideways'});}catch(e){direction=e.name==='TypeError';}check('getAllRecords direction enum',direction);
    check('indexedDB is an accessor',run("(()=>{const d=Object.getOwnPropertyDescriptor(globalThis,'indexedDB');return typeof d.get==='function'&&d.set===undefined&&d.enumerable&&d.configurable&&d.get.name==='get indexedDB'&&d.get.call(undefined)===d.get.call(globalThis)&&d.get.call(globalThis) instanceof IDBFactory;})()"));
    let versionchanges=0,blocked=0;db.onversionchange=e=>{versionchanges++;check('version notification',e.oldVersion===1&&e.newVersion===2);db.close();};const upgrade=idb().open('segments',2);upgrade.onblocked=()=>blocked++;upgrade.onupgradeneeded=()=>upgrade.result.createObjectStore('manifest');const db2=await request(upgrade);check('versionchange that closes does not block',blocked===0&&versionchanges===1&&db2.version===2);db2.close();
    const db3=await request(idb().open('segments'));check('reopen schema',db3.version===2&&db3.objectStoreNames.contains('manifest'));
    const order=[];db3.onversionchange=e=>order.push('versionchange:'+e.oldVersion+':'+e.newVersion);const up3=idb().open('segments',3);up3.onblocked=e=>{order.push('blocked:'+e.oldVersion+':'+e.newVersion);db3.close();};const db4=await request(up3);
    check('blocked once, after versionchange was fired',order.join(',')==='versionchange:2:3,blocked:2:3'&&db4.version===3);db4.close();
    const deleted=await new Promise((ok,bad)=>{const r=idb().deleteDatabase('segments');r.onsuccess=e=>ok({e,result:r.result});r.onerror=()=>bad(r.error);});
    const fo=idb().open('forced',1);fo.onupgradeneeded=()=>fo.result.createObjectStore('s');const db5=await request(fo);
    const closed=new Promise(ok=>db5.onclose=e=>ok(e));for(const r of clients.values())if(r.name==='forced')r.forced=true;
    const closeEvent=await closed;let pendingClose=false;try{db5.transaction('s');}catch(e){pendingClose=e.name==='InvalidStateError';}
    check('forced close fires close and closes',closeEvent.type==='close'&&pendingClose);
    check('deleteDatabase success is a version change event',deleted.e instanceof context.IDBVersionChangeEvent&&deleted.e.oldVersion===3&&deleted.e.newVersion===null&&deleted.result===undefined);
    check('deletion commit empties schema',data.get('segments').meta.version===0);
    context.bridge.retire();console.log(`IndexedDB metadata contract: ${checks} checks, 0 failures; ${payloadWrites} changed payload writes`);
}
test().catch(e=>{console.error(e);context.bridge.retire();process.exitCode=1;});
