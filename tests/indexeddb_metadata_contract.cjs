/* Production JS module, with a private native-contract adapter. This verifies
 * IDB transaction/key/cursor semantics, not native disk atomicity or sites. */
const fs=require('node:fs'),vm=require('node:vm'),assert=require('node:assert/strict'),v8=require('node:v8'),crypto=require('node:crypto');
const data=new Map(),clients=new Map();let serial=0,checks=0,payloadWrites=0;
function hostIDB(op,a,b,c){
    if(op==='begin'){let d=data.get(a);if(!d){d={name:a,meta:null,values:new Map(),lease:0};data.set(a,d);}if(d.lease)return[1,0,null];const id=++serial;d.lease=id;return[0,id,d.meta===null?null:v8.deserialize(v8.serialize(d.meta))];}
    if(op==='poll'){const r=clients.get(a);if(!r)return[3,null];const wanted=r.pending;r.pending=null;return[0,wanted];}
    if(op==='disconnect'){clients.delete(a);return 0;}
    let d=[...data.values()].find(d=>d.lease===a);if(!d)return op==='stage'||op==='value'||op==='connect'||op==='connections'?[3,null]:3;
    if(op==='abort'){d.lease=0;return 0;}
    if(op==='stage'){const bytes=v8.serialize(b),ref=crypto.createHash('sha256').update(bytes).digest('hex');if(!d.values.has(ref)){d.values.set(ref,v8.deserialize(bytes));payloadWrites++;}return[0,ref];}
    if(op==='value')return d.values.has(b)?[0,v8.deserialize(v8.serialize(d.values.get(b)))]:[3,null];
    if(op==='commit'){assert(Array.isArray(c));d.meta=v8.deserialize(v8.serialize(b));d.lease=0;return 0;}
    if(op==='connect'){const id=++serial;clients.set(id,{name:d.name,version:b,pending:null,notified:undefined});return[0,id];}
    if(op==='connections'){let n=0;for(const r of clients.values())if(r.name===d.name){n++;if(r.notified!==b){r.pending=b;r.notified=b;}}return[0,n];}
    throw new Error(op);
}
const handlers=new WeakMap(),context={console,setTimeout,Event,EventTarget,DOMException,structuredClone,indexedDBHandlerTarget:()=>false,host:{indexedDB:hostIDB,pack:x=>x,unpack:x=>x},
    cloneData:{serialize:x=>structuredClone(x),deserialize:x=>structuredClone(x),registerUncloneable(){}},eventState:()=>({}),
    dispatch:(t,e)=>t.dispatchEvent(e),installHandlers:(p,events)=>{for(const type of events)Object.defineProperty(p,'on'+type,{configurable:true,get(){return handlers.get(this)?.[type]||null;},set(fn){let h=handlers.get(this);if(!h){h={};handlers.set(this,h);}if(h[type])this.removeEventListener(type,h[type]);h[type]=typeof fn==='function'?fn:null;if(h[type])this.addEventListener(type,h[type]);}});}};
vm.createContext(context);context.bridge=vm.runInContext(fs.readFileSync('user/libc/web/js_indexeddb.js','utf8')+'\nindexedDBBridge;',context);
const run=code=>vm.runInContext(code,context),request=r=>new Promise((ok,bad)=>{r.onsuccess=()=>ok(r.result);r.onerror=()=>bad(r.error);});
const check=(name,yes)=>{checks++;assert(yes,name);};
async function test(){
    const r=context.indexedDB.open('segments',1);r.onupgradeneeded=()=>{check('oldVersion zero',r.result.version===1);const s=r.result.createObjectStore('segments',{keyPath:'id',autoIncrement:true});s.createIndex('label','label',{unique:true});s.createIndex('__proto__','tags',{multiEntry:true});};
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
    check('key type order',context.indexedDB.cmp(1,run('new Date(0)'))<0&&context.indexedDB.cmp(run('new Date(0)'),'a')<0&&context.indexedDB.cmp('a',run('new Uint8Array([0]).buffer'))<0);
    check('range open bound',!context.IDBKeyRange.bound(1,2,true,false).includes(1));
    let versionchanges=0,blocked=0;db.onversionchange=e=>{versionchanges++;check('version notification',e.oldVersion===1&&e.newVersion===2);db.close();};const upgrade=context.indexedDB.open('segments',2);upgrade.onblocked=()=>blocked++;upgrade.onupgradeneeded=()=>upgrade.result.createObjectStore('manifest');const db2=await request(upgrade);check('blocked and resumed',blocked===1&&versionchanges===1&&db2.version===2);db2.close();
    const db3=await request(context.indexedDB.open('segments'));check('reopen schema',db3.version===2&&db3.objectStoreNames.contains('manifest'));db3.close();await request(context.indexedDB.deleteDatabase('segments'));
    check('deletion commit empties schema',data.get('segments').meta.version===0);
    context.bridge.retire();console.log(`IndexedDB metadata contract: ${checks} checks, 0 failures; ${payloadWrites} changed payload writes`);
}
test().catch(e=>{console.error(e);context.bridge.retire();process.exitCode=1;});
