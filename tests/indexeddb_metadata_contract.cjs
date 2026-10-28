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
/* The module is the product's; another version (an earlier commit's, to compare) can be named. */
context.bridge=vm.runInContext(fs.readFileSync(process.argv[2]||'user/libc/web/js_indexeddb.js','utf8')+'\nindexedDBBridge;',context);
/* The global's own indexedDB accessor answers its realm's global; node's vm calls a global
   accessor with the outer sandbox object as this, so the getter is called explicitly. */
const run=code=>vm.runInContext(code,context),idb=()=>run("Object.getOwnPropertyDescriptor(globalThis,'indexedDB').get.call(globalThis)"),request=r=>new Promise((ok,bad)=>{r.onsuccess=()=>ok(r.result);r.onerror=()=>bad(r.error);});
const failures=[],check=(name,yes)=>{checks++;if(!yes)failures.push(name);};
/* A scenario that throws or never settles counts as one failed check. */
async function scenario(name,fn){let timer;try{await Promise.race([fn(),new Promise((_,bad)=>{timer=setTimeout(()=>bad(new Error('timed out')),3000);})]);}catch(e){checks++;failures.push(name+': '+(e?.message||e));}finally{clearTimeout(timer);}}
const thrown=fn=>{try{fn();return 'none';}catch(e){return e?.name;}},same=(list,want)=>JSON.stringify(Array.from(list))===JSON.stringify(want);
const listed=fn=>{try{return Array.from(fn());}catch(e){return 'threw '+e?.name;}},close=db=>{if(typeof db?.close==='function')db.close();};
/* WPT IndexedDB/resources/support-promises.js: migrateNamedDatabase and createBooksStore. An
   aborted upgrade answers with the open request's error. */
function migrate(name,version,fn){return new Promise((ok,bad)=>{const r=idb().open(name,version);r.onupgradeneeded=()=>{try{fn(r.result,r.transaction);}catch(e){bad(e);}};r.onsuccess=()=>ok(r.result);r.onerror=e=>{e.preventDefault();ok(r.error);};r.onblocked=()=>bad(new Error('blocked'));});}
const BOOKS=[{title:'Quarry Memories',author:'Fred',isbn:123456},{title:'Water Buffaloes',author:'Fred',isbn:234567},{title:'Bedrock Nights',author:'Barney',isbn:345678}];
function books(db){const store=db.createObjectStore('books',{keyPath:'isbn',autoIncrement:true});store.createIndex('by_author','author');store.createIndex('by_title','title',{unique:true});for(const record of BOOKS)store.put(run('('+JSON.stringify(record)+')'));return store;}
function notBooks(db){const store=db.createObjectStore('not_books');store.createIndex('not_by_author','author');store.createIndex('not_by_title','title',{unique:true});return store;}
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
    const late=idb().open('unique-late',1);let adds=0,dbErrors=0,constraint=false;
    late.onupgradeneeded=()=>{const udb=late.result,utx=late.transaction,us=udb.createObjectStore('s');
        for(const k of [1,2])us.add(run('({p:"same"})'),k).onsuccess=()=>adds++;
        us.createIndex('p','p',{unique:true});udb.onerror=()=>dbErrors++;utx.onabort=()=>{constraint=utx.error?.name==='ConstraintError';};};
    const lateError=await new Promise((ok,bad)=>{late.onsuccess=()=>bad(new Error('a unique index over equal values was built'));late.onerror=e=>ok(e);});
    check('requests before createIndex run without the index; its build aborts with ConstraintError',adds===2&&constraint&&dbErrors===0);
    check('an aborted upgrade answers the open with a new AbortError and no transaction',lateError.target.error.name==='AbortError'&&late.transaction===null);
    check('the error event of a failed open bubbles and is cancelable though the request has no transaction to reach (idbfactory-open-error-properties)',lateError.type==='error'&&lateError.bubbles===true&&lateError.cancelable===true&&lateError.target===late);
    await schemaOwners();
}
/* Stores and indexes are their schema objects, not their names: renames, deletions, and the
   revert of an aborted upgrade (WPT IndexedDB/idbindex-rename, idbindex_getKey,
   transaction-abort-*-metadata-revert, idbindex-rename-abort, idbdatabase_deleteObjectStore). */
async function schemaOwners(){
    await scenario('index rename onto a deleted index',async()=>{
        (await migrate('rename-deleted',1,db=>books(db))).close();
        let during=false;
        const db=await migrate('rename-deleted',2,(db,tx)=>{const store=tx.objectStore('books'),index=store.index('by_author');store.deleteIndex('by_title');index.name='by_title';during=same(store.indexNames,['by_title']);});
        check('rename onto a deleted index: indexNames during the upgrade',during);
        const store=db.transaction('books').objectStore('books');
        check('rename onto a deleted index: indexNames after commit',same(store.indexNames,['by_title']));
        const got=await request(store.index('by_title').get('Barney'));
        check('rename onto a deleted index keeps the renamed index contents (idbindex-rename: rename to the name of a deleted index)',got?.isbn===345678&&got?.title==='Bedrock Nights');
        db.close();
    });
    await scenario('index swap via renames',async()=>{
        (await migrate('rename-swap',1,db=>books(db))).close();
        let inside;
        const db=await migrate('rename-swap',2,(db,tx)=>{const store=tx.objectStore('books');store.index('by_author').name='tmp';store.index('by_title').name='by_author';store.index('tmp').name='by_title';inside=request(store.index('by_author').get('Bedrock Nights'));});
        check('swapped index answers by its contents inside the upgrade',(await inside)?.author==='Barney');
        const got=await request(db.transaction('books').objectStore('books').index('by_title').get('Barney'));
        check('swapped index answers by its contents after commit (idbindex-rename: swapping via renames)',got?.isbn===345678&&got?.title==='Bedrock Nights');
        db.close();
    });
    await scenario('deleted index handle',async()=>{
        let deletedGetKey,abortedGetKey,cursorDeleted;
        const db=await migrate('getkey-deleted',1,db=>{const store=db.createObjectStore('store',{keyPath:'key'}),index=store.createIndex('index','indexedProperty');store.add(run('({key:1,indexedProperty:"data"})'));store.deleteIndex('index');deletedGetKey=thrown(()=>index.getKey('data'));});
        check('getKey() throws InvalidStateError when the index is deleted (idbindex_getKey)',deletedGetKey==='InvalidStateError');
        close(db);
        const aborted=await migrate('getkey-aborted',1,(db,tx)=>{const store=db.createObjectStore('store',{keyPath:'key'}),index=store.createIndex('index','indexedProperty');store.add(run('({key:1,indexedProperty:"data"})'));tx.abort();abortedGetKey=thrown(()=>index.getKey('data'));});
        check('getKey() throws InvalidStateError on index deleted by aborted upgrade (idbindex_getKey)',aborted?.name==='AbortError'&&abortedGetKey==='InvalidStateError');
        const db2=await migrate('cursor-deleted',1,(db,tx)=>{const store=db.createObjectStore('test',{keyPath:'pKey'});store.createIndex('idx','iKey');for(const [i,p] of [['A',1],['A',2]])store.add(run(`({iKey:"${i}",pKey:${p}})`));
            const r=store.index('idx').openCursor();r.onsuccess=()=>{if(cursorDeleted!==undefined)return;const cursor=r.result;store.deleteIndex('idx');cursorDeleted=[thrown(()=>cursor.continuePrimaryKey('A',2)),thrown(()=>cursor.continue())];};});
        check('a cursor over a deleted index throws InvalidStateError before its source check (idbcursor-continuePrimaryKey-exception-order)',same(cursorDeleted||[],['InvalidStateError','InvalidStateError']));
        close(db2);
    });
    await scenario('object store revert',async()=>{
        (await migrate('store-revert',1,db=>{books(db);notBooks(db);})).close();
        const seen={};let kept,created,createdIndex,index,tx0,db0;
        const result=await migrate('store-revert',2,(db,tx)=>{db0=db;tx0=tx;kept=tx.objectStore('not_books');index=kept.index('not_by_author');db.deleteObjectStore('not_books');
            seen.deletedGet=thrown(()=>kept.get('query'));seen.deletedIndexGet=thrown(()=>index.get('query'));seen.deletedIndexNames=listed(()=>kept.indexNames);seen.names=[listed(()=>db.objectStoreNames),listed(()=>tx.objectStoreNames)];
            created=db.createObjectStore('created');createdIndex=created.createIndex('c','c');tx.abort();
            seen.createAfterAbort=thrown(()=>db.createObjectStore('late'));seen.version=db.version;
            seen.after=[thrown(()=>kept.get('query')),thrown(()=>index.get('query')),thrown(()=>created.get('query')),thrown(()=>createdIndex.get('query'))];
            seen.afterNames=[listed(()=>kept.indexNames),listed(()=>created.indexNames),listed(()=>db.objectStoreNames),listed(()=>tx.objectStoreNames)];});
        check('the open of an aborted upgrade fails with AbortError',result?.name==='AbortError');
        check('a deleted store and its index throw InvalidStateError; its index set is empty',seen.deletedGet==='InvalidStateError'&&seen.deletedIndexGet==='InvalidStateError'&&same(seen.deletedIndexNames,[]));
        check('deleteObjectStore leaves the connection and the upgrade scope',same(seen.names[0],['books'])&&same(seen.names[1],['books']));
        check('createObjectStore right after abort() throws TransactionInactiveError (idbdatabase-createObjectStore-exception-order)',seen.createAfterAbort==='TransactionInactiveError');
        check('abort() gives the connection its version back',seen.version===1&&db0.version===1);
        check('after abort() a deleted store is back and a created one is deleted (transaction-abort-multiple-metadata-revert)',same(seen.after,['TransactionInactiveError','TransactionInactiveError','InvalidStateError','InvalidStateError']));
        check('after abort() index sets and store sets are the ones before the upgrade (transaction-abort-object-store-metadata-revert)',same(seen.afterNames,[['not_by_author','not_by_title'],[],['books','not_books'],['books','not_books']]));
        check('after the abort event the upgrade is gone from the connection',thrown(()=>db0.createObjectStore('later'))==='InvalidStateError'&&same(tx0.objectStoreNames,['books','not_books']));
    });
    await scenario('index revert',async()=>{
        (await migrate('index-revert',1,db=>{books(db);notBooks(db);})).close();
        const seen={};
        await migrate('index-revert',2,(db,tx)=>{const store=tx.objectStore('not_books'),author=store.index('not_by_author'),title=store.index('not_by_title');author.name='renamed_by_author';store.deleteIndex('not_by_title');
            seen.deleted=thrown(()=>title.get('query'));const isbn=store.createIndex('not_by_isbn','isbn');isbn.name='by_isbn_renamed';isbn.name='by_isbn_renamed_again';tx.abort();
            seen.names=[author.name,title.name,isbn.name];seen.after=[thrown(()=>author.get('query')),thrown(()=>title.get('query')),thrown(()=>isbn.get('query'))];seen.indexNames=Array.from(store.indexNames);});
        check('a deleted index throws InvalidStateError (transaction-abort-index-metadata-revert)',seen.deleted==='InvalidStateError');
        check('after abort() index handles take their index names back; a created one keeps its own (idbindex-rename-abort)',same(seen.names,['not_by_author','not_by_title','by_isbn_renamed_again']));
        check('after abort() a deleted index is back and a created one is deleted',same(seen.after,['TransactionInactiveError','TransactionInactiveError','InvalidStateError']));
        check('after abort() the store handle has its index set back',same(seen.indexNames,['not_by_author','not_by_title']));
        const db=await request(idb().open('index-revert',1));
        check('an aborted upgrade leaves the stored indexes',same(db.transaction('not_books').objectStore('not_books').indexNames,['not_by_author','not_by_title']));
        const got=await request(db.transaction('books').objectStore('books').index('by_author').get('Barney'));
        check('an aborted upgrade leaves the stored index contents',got?.isbn===345678);
        db.close();
    });
    await scenario('requests placed before deleteObjectStore',async()=>{
        const keys=[],seen={};
        const db=await migrate('resurrected',1,db=>{const store=db.createObjectStore('resurrected',{autoIncrement:true,keyPath:'k'});store.add(run('({k:5})')).onsuccess=e=>keys.push(e.target.result);store.add(run('({})')).onsuccess=e=>keys.push(e.target.result);store.createIndex('idx','i');
            db.deleteObjectStore('resurrected');seen.after=[thrown(()=>store.add(run('({k:9})'))),thrown(()=>store.index('idx')),thrown(()=>store.deleteIndex('idx')),thrown(()=>store.createIndex('idx2','a'))];
            const again=db.createObjectStore('resurrected',{autoIncrement:true});again.add('Unicorns').onsuccess=e=>keys.push(e.target.result);seen.again=[Array.from(again.indexNames),again.keyPath,thrown(()=>again.index('idx'))];});
        check('requests placed before deleteObjectStore run against the deleted store, not the new one of its name (idbdatabase_deleteObjectStore)',same(keys,[5,6,1]));
        check('a deleted store handle throws InvalidStateError',same(seen.after,['InvalidStateError','InvalidStateError','InvalidStateError','InvalidStateError']));
        check('a store created again under the name has its own schema',same(seen.again[0],[])&&seen.again[1]===null&&seen.again[2]==='NotFoundError');
        db.close();
    });
    await scenario('requests placed before deleteIndex',async()=>{
        const events=[];
        const db=await migrate('later-deleted-index',1,db=>{const store=db.createObjectStore('store');store.add(run('({animal:"Unicorn"})'),1).onsuccess=()=>events.push('add1');store.createIndex('index','animal',{unique:true});
            const add2=store.add(run('({animal:"Unicorn"})'),2);add2.onsuccess=()=>events.push('add2');add2.onerror=e=>{events.push('add2 '+add2.error.name);e.preventDefault();};
            store.deleteIndex('index');store.add(run('({animal:"Unicorn"})'),3).onsuccess=()=>events.push('add3');});
        check('an index deleted later still holds the requests placed while it lived (idbobjectstore_createIndex: event ordering for a later deleted index)',same(events,['add1','add2 ConstraintError','add3'])&&typeof db?.close==='function');
        close(db);
    });
    await scenario('handle identity and rename errors',async()=>{
        const seen={};
        (await migrate('handles',1,db=>{const store=db.createObjectStore('store',{keyPath:['a','b']}),index=store.createIndex('index',['a','b']);seen.created=store.index('index')===index&&index.objectStore===store;})).close();
        const db=await request(idb().open('handles'));const store=db.transaction('store').objectStore('store'),index=store.index('index');
        check('createIndex and index() give the one index handle; objectStore is its store handle (idbindex-objectStore-SameObject)',seen.created&&store.index('index')===index&&index.objectStore===store);
        check('keyPath is the same object each time (idbobjectstore_keyPath, idbindex_keyPath)',Array.isArray(store.keyPath)&&store.keyPath===store.keyPath&&index.keyPath===index.keyPath);
        check('rename outside an upgrade throws InvalidStateError (idbindex-rename-errors, idbobjectstore-rename-errors)',thrown(()=>{index.name='x';})==='InvalidStateError'&&thrown(()=>{store.name='x';})==='InvalidStateError');
        db.close();
    });
    await scenario('stored schema without ids',async()=>{
        /* The form stored before ids: index entries named by index name, a stale entry of an index
           that no longer exists. */
        const values=new Map([['ref1',{id:1,tag:'a'}],['ref2',{id:2,tag:'b'}]]);
        data.set('legacy',{name:'legacy',lease:0,values,meta:{version:1,stores:[{name:'s',keyPath:'id',autoIncrement:false,generator:1,indexes:[{name:'by_tag',keyPath:'tag',unique:false,multiEntry:false}],
            records:[[1,'ref1',[['by_tag',['a']],['gone',['z']]]],[2,'ref2',[['by_tag',['b']]]]]}]}});
        let db=await request(idb().open('legacy'));
        check('a schema stored without ids reads by its index',(await request(db.transaction('s').objectStore('s').index('by_tag').get('b')))?.id===2);
        const tx=db.transaction('s','readwrite');tx.objectStore('s').put(run('({id:3,tag:"c"})'));await new Promise(ok=>tx.oncomplete=ok);db.close();
        const meta=data.get('legacy').meta,store=meta.stores[0];
        check('the first commit stores ids, and the index entries under the index id',Number.isSafeInteger(meta.nextId)&&Number.isSafeInteger(store.id)&&Number.isSafeInteger(store.indexes[0].id)&&store.records.every(r=>r[2].length===1&&r[2][0][0]===store.indexes[0].id)&&meta.nextId>store.indexes[0].id&&meta.nextId>store.id);
        db=await migrate('legacy',2,(db,tx)=>{tx.objectStore('s').index('by_tag').name='tag';});
        check('a migrated index renames without touching its records',(await request(db.transaction('s').objectStore('s').index('tag').getAllKeys())).join(',')==='1,2,3');
        db.close();
    });
}
test().catch(e=>{checks++;failures.push('contract: '+(e?.stack||e));}).finally(()=>{
    context.bridge.retire();
    for(const f of failures)console.error('FAIL '+f);
    console.log(`IndexedDB metadata contract: ${checks} checks, ${failures.length} failures; ${payloadWrites} changed payload writes`);
    process.exit(failures.length?1:0);
});
