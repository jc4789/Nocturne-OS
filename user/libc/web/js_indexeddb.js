/* IndexedDB owns a separate native transactional store. Values use the same
 * private structured-clone graph as messages/history, including cycles and
 * binary buffers; neither JSON nor localStorage is used for persistence. */
const indexedDBBridge=(()=>{
    'use strict';
    const {Date,Uint8Array,ArrayBuffer,Array,Number,String,Object,Set,Map,WeakMap,DOMException,TypeError,Reflect}=globalThis;
    const native=host.indexedDB,storageKey=host.indexedDBStorageKey,pack=host.pack,unpack=host.unpack;
    if(typeof native!=='function')return null;
    const invoke=Reflect.apply,wmGet=WeakMap.prototype.get,wmSet=WeakMap.prototype.set,wmHas=WeakMap.prototype.has,wmDelete=WeakMap.prototype.delete;
    const setAdd=Set.prototype.add,setClear=Set.prototype.clear,setValues=Set.prototype.values;
    const setNext=Object.getPrototypeOf(new Set().values()).next;
    const mapGet=Map.prototype.get,mapSet=Map.prototype.set,mapValues=Map.prototype.values,mapClear=Map.prototype.clear;
    const slots=new WeakMap(),connectionSlots=new Map();
    const activeTasks=new Set();
    const records={get:v=>invoke(wmGet,slots,[v]),set:(v,r)=>invoke(wmSet,slots,[v,r]),has:v=>invoke(wmHas,slots,[v])};
    const connections={get:v=>invoke(mapGet,connectionSlots,[v]),set:(v,r)=>invoke(mapSet,connectionSlots,[v,r]),values:()=>invoke(mapValues,connectionSlots,[]),clear:()=>invoke(mapClear,connectionSlots,[])};
    const later=setTimeout,token={};let watching=false,retired=false;
    const get=(v,kind)=>{const r=records.get(v);if(!r||r.kind!==kind)throw new TypeError('Illegal '+kind+' receiver');return r;};
    const fail=(name,message=name)=>{throw domExceptionBridge.create(name,message);};
    const strings=v=>{if(typeof v==='symbol')throw new TypeError('Cannot convert Symbol');return String(v);};
    function names(values){const a=values.slice().sort();Object.defineProperties(a,{contains:{value:v=>a.includes(strings(v))},item:{value:i=>a[i>>>0]??null}});return a;}
    function fire(target,type,extra={},cancelable=false){const r=records.get(target),chain=[target];if(type==='error'&&r?.kind==='IDBRequest'&&r.transaction){chain.push(r.transaction,get(r.transaction,'IDBTransaction').db);}else if(type==='abort'&&r?.kind==='IDBTransaction')chain.push(r.db);/* a request's error event and a transaction's abort event bubble ("fire an error event", "abort a transaction", open's failure), whether or not the request still has a transaction to reach */const e=new Event(type,{cancelable,bubbles:type==='error'&&r?.kind==='IDBRequest'||type==='abort'&&r?.kind==='IDBTransaction'});for(const [k,v]of Object.entries(extra))Object.defineProperty(e,k,{value:v,enumerable:true});eventState(e).isTrusted=true;dispatch(target,e,chain.length>1?{entries:chain.map(node=>({node,target,visible:chain,atTarget:false,related:undefined})),finalTarget:target,finalRelated:undefined}:undefined);return e;}
    function key(value,seen=new Set()){
        if(typeof value==='number'){if(Number.isNaN(value))fail('DataError');return value===0?0:value;}
        if(value instanceof Date){const n=Date.prototype.getTime.call(value);if(Number.isNaN(n))fail('DataError');return new Date(n);}
        if(typeof value==='string')return value;
        if(value instanceof ArrayBuffer||ArrayBuffer.isView(value)){
            let input;try{input=value instanceof ArrayBuffer?new Uint8Array(value):new Uint8Array(value.buffer,value.byteOffset,value.byteLength);}catch(_){fail('DataError');}
            return new Uint8Array(input).buffer;
        }
        if(Array.isArray(value)){
            if(seen.has(value))fail('DataError');seen.add(value);const result=[];
            for(let i=0;i<value.length;i++){if(!Object.hasOwn(value,i))fail('DataError');result.push(key(value[i],seen));}seen.delete(value);return result;
        }
        fail('DataError');
    }
    const rank=v=>typeof v==='number'?0:v instanceof Date?1:typeof v==='string'?2:v instanceof ArrayBuffer?3:4;
    function compare(a,b){const x=rank(a),y=rank(b);if(x!==y)return x<y?-1:1;if(x===1){a=Date.prototype.getTime.call(a);b=Date.prototype.getTime.call(b);}if(x===3){a=new Uint8Array(a);b=new Uint8Array(b);}if(x>=3){for(let i=0;i<Math.min(a.length,b.length);i++){const c=x===4?compare(a[i],b[i]):a[i]<b[i]?-1:a[i]>b[i]?1:0;if(c)return c;}return a.length<b.length?-1:a.length>b.length?1:0;}return a<b?-1:a>b?1:0;}
    function path(value,path){if(Array.isArray(path))return path.map(p=>pathValue(value,p));return pathValue(value,path);}
    function pathValue(value,path){if(path==='')return value;for(const p of path.split('.')){if(value==null||!Object.hasOwn(Object(value),p))return undefined;value=value[p];}return value;}
    /* WebIDL: a JavaScript value to (DOMString or sequence<DOMString>). An object with an iterator
       is a sequence, each item a DOMString; anything else (null too) is a DOMString. */
    function keyPathValue(v){
        if(v!==null&&(typeof v==='object'||typeof v==='function')){
            const method=v[Symbol.iterator];
            if(method!==undefined&&method!==null){
                if(typeof method!=='function')throw new TypeError('The key path iterator is not callable');
                const iterator=invoke(method,v,[]);if(iterator===null||typeof iterator!=='object'&&typeof iterator!=='function')throw new TypeError('The key path iterator is not an object');
                const next=iterator.next,items=[];
                for(;;){const step=invoke(next,iterator,[]);if(step===null||typeof step!=='object'&&typeof step!=='function')throw new TypeError('The key path iterator result is not an object');if(step.done)return items;items[items.length]=strings(step.value);}
            }
        }
        return strings(v);
    }
    /* A "valid key path" (2.5): the empty string, identifiers joined by periods, or a non-empty
       list of such strings. */
    const identifiers=/^[\p{ID_Start}_$][\p{ID_Continue}$\u200c\u200d]*(?:\.[\p{ID_Start}_$][\p{ID_Continue}$\u200c\u200d]*)*$/u;
    const validPath=x=>x===''||identifiers.test(x),validKeyPath=p=>Array.isArray(p)?p.length>0&&p.every(validPath):validPath(p);
    /* WebIDL: a dictionary argument is undefined, null (its defaults) or an object. */
    function dictionary(v){if(v==null)return null;if(typeof v!=='object'&&typeof v!=='function')throw new TypeError('The options argument is not an object');return v;}
    function inject(value,path,valueKey){if(!value||typeof value!=='object'||!path||Array.isArray(path))fail('DataError');const parts=path.split('.');for(let i=0;i<parts.length-1;i++){const p=parts[i];if(!Object.hasOwn(value,p))Object.defineProperty(value,p,{value:{},writable:true,enumerable:true,configurable:true});value=value[p];if(!value||typeof value!=='object')fail('DataError');}Object.defineProperty(value,parts[parts.length-1],{value:valueKey,writable:true,enumerable:true,configurable:true});}
    function clone(value){return cloneData.deserialize(cloneData.serialize(value,undefined,true),null,true);}
    async function prefetch(list,tx){
        const r=get(tx,'IDBTransaction');
        if(!r.valueCache)r.valueCache=new Map();
        const references=[...new Set(list.map(record=>record[1]))].filter(ref=>!r.valueCache.has(ref));
        if(!references.length)return;
        const a=await native('values',r.nativeToken,references);status(a[0]);
        if(r.finished)return;
        if(!Array.isArray(a[1])||a[1].length!==references.length)fail('UnknownError','Invalid IndexedDB value batch');
        for(let i=0;i<references.length;i++)r.valueCache.set(references[i],a[1][i]);
    }
    async function value(record,tx){
        const r=get(tx,'IDBTransaction');
        if(r.valueCache?.has(record[1])){
            const bytes=r.valueCache.get(record[1]);r.valueCache.delete(record[1]);
            return cloneData.deserialize(unpack(bytes),null,true);
        }
        const a=await native('value',r.nativeToken,record[1]);status(a[0]);
        return cloneData.deserialize(unpack(a[1]),null,true);
    }
    function status(code){if(code!==0)fail(code===2?'SecurityError':code===4?'QuotaExceededError':'UnknownError','Native IndexedDB operation failed');}
    class IDBKeyRange {
        constructor(t,r){if(t!==token)throw new TypeError('Illegal IDBKeyRange constructor');records.set(this,{kind:'IDBKeyRange',...r});}
        get lower(){return clone(get(this,'IDBKeyRange').lower);}
        get upper(){return clone(get(this,'IDBKeyRange').upper);}
        get lowerOpen(){return get(this,'IDBKeyRange').lowerOpen;}
        get upperOpen(){return get(this,'IDBKeyRange').upperOpen;}
        includes(v){const r=get(this,'IDBKeyRange');return matches(key(v),r);}
        static only(v){const k=key(v);return new IDBKeyRange(token,{lower:k,upper:k,lowerOpen:false,upperOpen:false});}
        static lowerBound(v,open=false){return new IDBKeyRange(token,{lower:key(v),upper:undefined,lowerOpen:!!open,upperOpen:false});}
        static upperBound(v,open=false){return new IDBKeyRange(token,{lower:undefined,upper:key(v),lowerOpen:false,upperOpen:!!open});}
        static bound(a,b,ao=false,bo=false){a=key(a);b=key(b);const c=compare(a,b);if(c>0||!c&&(ao||bo))fail('DataError');return new IDBKeyRange(token,{lower:a,upper:b,lowerOpen:!!ao,upperOpen:!!bo});}
    }
    function matches(k,r){if(!r)return true;return (r.lower===undefined||compare(k,r.lower)>(r.lowerOpen?0:-1))&&(r.upper===undefined||compare(k,r.upper)<(r.upperOpen?0:1));}
    const isRange=v=>records.get(v)?.kind==='IDBKeyRange';
    function range(v,optional=false){if(v==null&&optional)return null;if(isRange(v))return get(v,'IDBKeyRange');const k=key(v);return {lower:k,upper:k,lowerOpen:false,upperOpen:false};}
    /* "is a potentially valid key range": a key range, or a value whose conversion to a key is
       not "invalid type" (it may still be an invalid value, a DataError when converted). */
    const potentialRange=v=>isRange(v)||typeof v==='number'||typeof v==='string'||v instanceof Date||v instanceof ArrayBuffer||ArrayBuffer.isView(v)||Array.isArray(v);
    /* WebIDL [EnforceRange] unsigned long. */
    function enforceCount(v){v=Number(v);if(!Number.isFinite(v))throw new TypeError('Count is not a finite number');v=Math.trunc(v);if(v<0||v>4294967295)throw new TypeError('Count is outside the unsigned long range');return v===0?0:v;}
    const directions=['next','nextunique','prev','prevunique'];
    function direction(v){v=strings(v);if(!directions.includes(v))throw new TypeError('Invalid cursor direction');return v;}
    /* The IDBGetAllOptions dictionary: its members in order (count, direction, query). */
    function getAllOptions(v){
        if(v!==undefined&&v!==null&&typeof v!=='object'&&typeof v!=='function')throw new TypeError('IDBGetAllOptions must be an object');
        if(v==null)return {count:undefined,direction:'next',query:null};
        const count=v.count,c=count===undefined?undefined:enforceCount(count);
        const d=v.direction,dir=d===undefined?'next':direction(d);
        const query=v.query;
        return {count:c,direction:dir,query:query===undefined?null:query};
    }
    class IDBRequest extends EventTarget {
        constructor(t,source=null,transaction=null){if(t!==token)throw new TypeError('Illegal IDBRequest constructor');super();records.set(this,{kind:'IDBRequest',source,transaction,done:false,result:undefined,error:null});}
        get result(){const r=get(this,'IDBRequest');if(!r.done)fail('InvalidStateError');return r.result;}
        get error(){const r=get(this,'IDBRequest');if(!r.done)fail('InvalidStateError');return r.error;}
        get source(){return get(this,'IDBRequest').source;}
        get transaction(){return get(this,'IDBRequest').transaction;}
        get readyState(){return get(this,'IDBRequest').done?'done':'pending';}
    }
    class IDBOpenDBRequest extends IDBRequest {}
    function finish(request,result,error){const r=get(request,'IDBRequest');r.done=true;r.result=result;r.error=error||null;return fire(request,error?'error':'success',{},!!error);}
    class IDBVersionChangeEvent extends Event {constructor(type,init={}){super(type,init);Object.defineProperties(this,{oldVersion:{value:Number(init.oldVersion)||0,enumerable:true},newVersion:{value:init.newVersion==null?null:Number(init.newVersion),enumerable:true}});}}
    function versionEvent(target,type,oldVersion,newVersion){const e=new IDBVersionChangeEvent(type,{oldVersion,newVersion});eventState(e).isTrusted=true;dispatch(target,e);return e;}
    function live(tx,writing=false){const r=get(tx,'IDBTransaction');if(r.finished||r.committing||!r.active)fail('TransactionInactiveError');if(writing&&r.mode==='readonly')fail('ReadOnlyError');return r;}
    function activate(tx){get(tx,'IDBTransaction').active=true;invoke(setAdd,activeTasks,[tx]);}
    function taskEnd(){
        const iterator=invoke(setValues,activeTasks,[]);
        for(;;){const item=invoke(setNext,iterator,[]);if(item.done)break;get(item.value,'IDBTransaction').active=false;}
        invoke(setClear,activeTasks,[]);
    }
    /* The schema names an object store or an index by a fixed id, never by its name, which an
       upgrade can give to another one (a rename, or a delete and a rename onto the name). Records
       keep their index keys under the index's id. A schema stored before ids existed keyed those
       entries by index name; it gets its ids here, in its own order, so every snapshot of the same
       stored schema gets the same ids, and keeps them from the first commit that writes it. */
    function adopt(data){
        if(!data||!Array.isArray(data.stores)||!Number.isSafeInteger(data.version))fail('UnknownError','Invalid IndexedDB snapshot');
        if(Number.isSafeInteger(data.nextId))return data;
        let id=0;
        for(const store of data.stores){
            store.id=++id;
            const ids=new Map();
            for(const index of store.indexes){index.id=++id;ids.set(index.name,index.id);}
            for(const record of store.records)if(record[2])record[2]=record[2].flatMap(entry=>ids.has(entry[0])?[[ids.get(entry[0]),entry[1]]]:[]);
        }
        data.nextId=id+1;
        return data;
    }
    /* The object store set of a database before an upgrade, without records: what "abort an upgrade
       transaction" gives back to the connection and to the upgrade's handles. */
    const outline=data=>({version:data.version,nextId:data.nextId,stores:data.stores.map(({id,name,keyPath,autoIncrement,indexes})=>({id,name,keyPath,autoIncrement,indexes:indexes.map(index=>({...index}))}))});
    /* A transaction's view of its database: its own snapshot once it has begun (an upgrade has one
       from the start), before that its connection's. */
    function view(tx){const r=get(tx,'IDBTransaction');return r.data||r.dbRecord.schema;}
    /* A handle's object store (or index) is a schema object of its transaction's view. The view
       changes when the transaction takes its snapshot and when an aborted upgrade gives back the
       connection's; the handle then finds its object again by id and takes its name ("abort an
       upgrade transaction"), or keeps the one it had when the view has none. An object the view
       does not hold has been deleted: by deleteObjectStore or deleteIndex, or created by an
       upgrade that aborted. Requests placed before a deletion still run against the object. */
    function storeOf(s){const data=view(s.tx);if(s.view!==data){s.view=data;const found=data.stores.find(v=>v.id===s.id);if(found){s.store=found;s.name=found.name;}}return s.store;}
    function indexOf(s){const store=storeOf(records.get(s.handle));if(s.of!==store){s.of=store;const found=store.indexes.find(v=>v.id===s.id);if(found){s.index=found;s.name=found.name;}}return s.index;}
    function deleted(s){if(s.kind==='IDBIndex'){const index=indexOf(s);return deleted(records.get(s.handle))||!s.of.indexes.includes(index);}return !view(s.tx).stores.includes(storeOf(s));}
    function existing(s){if(deleted(s))fail('InvalidStateError');return s;}
    function enqueue(tx,source,operation){const r=live(tx),request=new IDBRequest(token,source,tx);r.queue.push({request,operation});return request;}
    async function closing(db){
        const r=get(db,'IDBDatabase');
        if(!r.closed||r.transactions.size||!r.connection||r.disconnecting)return;
        r.disconnecting=true;
        try{
            const code=await native('disconnect',r.connection);
            if(code===1){later(()=>closing(db),5);return;}
            r.connection=0;
        }catch(_){r.connection=0;}
        finally{r.disconnecting=false;}
    }
    function watch(){
        if(watching||retired)return;
        watching=true;
        later(async function poll(){
            let active=false;
            try{
                if(retired)return;
                for(const list of connections.values())for(const db of list){
                    const r=get(db,'IDBDatabase');
                    if(!r.connection)continue;
                    active=true;
                    const connection=r.connection,a=await native('poll',connection);
                    if(retired)return;
                    if(r.connection===connection&&a[0]===0&&a[2]===true)forceClose(db);
                    else if(r.connection===connection&&a[0]===0&&a[1]!==null){
                        /* A connection whose close is pending gets no versionchange. Either way
                           the opener learns it has been fired, after this task (a close its
                           handler began goes to the broker first), and only then may fire
                           blocked at its request. */
                        if(!r.closed)versionEvent(db,'versionchange',r.version,a[1]===0?null:a[1]);
                        const fired=async()=>{if(retired||r.connection!==connection)return;try{if(await native('fired',connection)===1)later(fired,5);}catch(_){}};
                        later(fired,0);
                    }
                }
            }catch(_){/* Broker failure is also delivered to its pending requests. */}
            finally{watching=false;if(active&&!retired)watch();}
        },100);
    }
    /* "Close a database connection" with forced (its database was cleared): close pending, its
       transactions abort with AbortError, and once they are done close fires at it. */
    function forceClose(db){
        const r=get(db,'IDBDatabase');if(r.forced)return;
        r.forced=r.closed=true;
        for(const tx of [...r.transactions])abort(tx,new DOMException('The database connection was closed','AbortError'));
        closing(db);
        later(()=>{const e=new Event('close');eventState(e).isTrusted=true;dispatch(db,e);},0);
    }
    function discard(nativeToken){
        if(nativeToken)try{Promise.resolve(native('abort',nativeToken)).catch(()=>{});}catch(_){}
    }
    function abort(tx,error){
        const r=get(tx,'IDBTransaction');
        if(r.finished)return;
        r.finished=true;r.active=false;r.error=error||new DOMException('Transaction aborted','AbortError');
        r.valueCache?.clear();
        discard(r.nativeToken);r.nativeToken=0;
        r.dbRecord.transactions.delete(tx);
        /* "Abort an upgrade transaction": the connection's version and object store set, and with
           them the view of the upgrade's handles, go back to the database's before the upgrade. The
           database keeps its upgrade transaction until the abort event's task. */
        if(r.openRequest){r.data=r.before;r.dbRecord.schema=r.before;r.dbRecord.version=r.before.version;r.dbRecord.closed=true;}
        closing(r.db);
        const queue=r.executing?[r.executing,...r.queue]:r.queue;
        r.executing=null;r.queue=[];
        later(()=>{
            for(const q of queue)if(q.request)finish(q.request,undefined,new DOMException('Transaction aborted','AbortError'));
            if(r.openRequest)r.dbRecord.upgrade=null;
            fire(tx,'abort');
            /* "Abort a transaction" resets an upgrade's open request (no transaction, so the open's
               error does not reach the transaction or the connection), and "open a database
               connection" then answers it with a new AbortError, in a task of its own. */
            if(r.openRequest){const request=get(r.openRequest,'IDBRequest');request.transaction=null;request.result=undefined;request.done=false;
                later(()=>finish(r.openRequest,undefined,new DOMException('The upgrade transaction was aborted','AbortError')),0);}
        },0);
    }
    async function complete(tx){
        const r=get(tx,'IDBTransaction');if(r.finished)return;
        r.committing=true;r.active=false;
        try{
            if(r.openRequest&&!r.dbRecord.connection){
                const a=await native('connect',r.nativeToken,r.data.version);
                if(a[0]===1){later(()=>complete(tx),5);return;}
                status(a[0]);r.dbRecord.connection=a[1];watch();
            }
            if(r.finished)return;
            if(r.mode==='readonly')status(await native('abort',r.nativeToken));
            else status(await native('commit',r.nativeToken,pack(r.data),r.data.stores.flatMap(s=>s.records.map(v=>v[1])),r.data.version));
            if(r.finished)return;
            r.nativeToken=0;r.finished=true;r.dbRecord.version=r.data.version;r.dbRecord.schema=r.data;
            r.valueCache?.clear();
            r.dbRecord.transactions.delete(tx);
            closing(r.db);
            later(()=>{if(r.openRequest)r.dbRecord.upgrade=null;fire(tx,'complete');if(r.openRequest){get(r.openRequest,'IDBRequest').transaction=null;finish(r.openRequest,r.db);}},0);
        }catch(e){abort(tx,e);}
    }
    async function step(tx){
        const r=get(tx,'IDBTransaction');if(r.finished||r.stepping)return;
        r.stepping=true;r.active=false;
        try{
            if(!r.nativeToken){
                const a=await native('begin',r.dbRecord.name);
                if(r.finished){if(a[0]===0)discard(a[1]);return;}
                if(a[0]===1){later(()=>step(tx),5);return;}
                status(a[0]);r.nativeToken=a[1];r.data=adopt(a[2]===null?{version:0,stores:[]}:unpack(a[2]));
                if(r.mode==='versionchange'){
                    r.data.version=r.upgradeVersion;activate(tx);
                    const request=get(r.openRequest,'IDBRequest');request.done=true;request.result=r.db;request.transaction=tx;
                    versionEvent(r.openRequest,'upgradeneeded',r.oldVersion,r.upgradeVersion);
                }else{
                    if(r.data.version!==r.dbRecord.version)fail('VersionError','Database schema changed');
                    for(const n of r.scope)if(!r.data.stores.some(s=>s.name===n))fail('NotFoundError');
                }
            }
            if(r.finished)return;
            const q=r.queue.shift();
            if(!q){complete(tx);return;}
            r.executing=q;
            let result,error;
            try{result=await q.operation(r);}catch(e){error=e;}
            if(r.finished)return;
            later(()=>{
                if(r.finished)return;
                r.executing=null;
                if(q.request){
                    activate(tx);
                    const event=finish(q.request,result,error);
                    if(eventState(event).listenerException){abort(tx,new DOMException('Request listener threw','AbortError'));return;}
                    if(error&&!event.defaultPrevented){abort(tx,error);return;}
                }else if(error){abort(tx,error);return;}
                later(()=>{r.active=false;step(tx);},0);
            },0);
        }catch(e){abort(tx,e);}
        finally{r.stepping=false;}
    }
    class IDBTransaction extends EventTarget {
        constructor(t,db,scope,mode,options={}){
            if(t!==token)throw new TypeError('Illegal IDBTransaction constructor');
            super();const dbRecord=get(db,'IDBDatabase');
            records.set(this,{
                kind:'IDBTransaction',db,dbRecord,scope:scope.slice(),mode,
                durability:options.durability||'default',active:true,finished:false,
                committing:false,error:null,queue:[],data:null,nativeToken:0,stores:new Map()
            });
            activate(this);dbRecord.transactions.add(this);
            later(()=>{const r=get(this,'IDBTransaction');r.active=false;step(this);},0);
        }
        get db(){return get(this,'IDBTransaction').db;}
        get mode(){return get(this,'IDBTransaction').mode;}
        get durability(){return get(this,'IDBTransaction').durability;}
        get error(){return get(this,'IDBTransaction').error;}
        /* An upgrade's scope is its connection's object store set; another transaction's is the
           names it was given, which no rename can reach while it lives. */
        get objectStoreNames(){const r=get(this,'IDBTransaction');return names(r.mode==='versionchange'?view(this).stores.map(s=>s.name):r.scope);}
        objectStore(name){const r=get(this,'IDBTransaction');name=strings(name);if(r.finished)fail('InvalidStateError');const store=view(this).stores.find(s=>s.name===name);if(!store||r.mode!=='versionchange'&&!r.scope.includes(name))fail('NotFoundError');
            /* One handle per object store in a transaction, whatever it is renamed to. */
            let handle=r.stores.get(store.id);if(!handle){handle=new IDBObjectStore(token,this,store);r.stores.set(store.id,handle);}return handle;}
        abort(){const r=get(this,'IDBTransaction');if(r.finished||r.committing)fail('InvalidStateError');abort(this);}
        /* "If this's state is not active, then throw an InvalidStateError": finished, committing
           and inactive alike. */
        commit(){const r=get(this,'IDBTransaction');if(r.finished||r.committing||!r.active)fail('InvalidStateError');r.committing=true;r.active=false;}
    }
    function insertion(records,k){let a=0,b=records.length;while(a<b){const m=(a+b)>>>1;if(compare(records[m][0],k)<0)a=m+1;else b=m;}return a;}
    function indexKeys(index,v){let raw=path(v,index.keyPath),out=[];if(index.multiEntry&&Array.isArray(raw)){for(const candidate of raw)try{const k=key(candidate);if(!out.some(x=>!compare(x,k)))out.push(k);}catch(e){if(e.name!=='DataError')throw e;}}else try{out.push(key(raw));}catch(e){if(e.name!=='DataError')throw e;}return out;}
    /* The order a handle reads: a store's own records ([key, value, index keys], kept sorted by
       key), or an index's [key, record] entries sorted by key and then primary key. An index's
       order is built from its store's records when first read, then kept in step with each record
       stored or deleted (reorder); other changes to the records drop the store's orders. Reading a
       record, a count or one step of a cursor is then a search, not a pass over the store. */
    const orderSlots=new WeakMap(),orders={get:v=>invoke(wmGet,orderSlots,[v]),set:(v,r)=>invoke(wmSet,orderSlots,[v,r]),drop:v=>invoke(wmDelete,orderSlots,[v])};
    const entryKeys=(record,id)=>record[2]?.find(v=>v[0]===id)?.[1]||[];
    function indexOrder(store,index){
        let kept=orders.get(store);if(!kept){kept=Object.create(null);orders.set(store,kept);}
        let list=kept[index.id];
        if(!list){list=[];for(const r of store.records)for(const k of entryKeys(r,index.id))list.push([k,r]);list.sort((a,b)=>compare(a[0],b[0])||compare(a[1][0],b[1][0]));kept[index.id]=list;}
        return list;
    }
    const primaryOf=(entry,ofStore)=>ofStore?entry[0]:entry[1][0];
    /* The first position whose entry is at or after (k, p), or after it with "after"; without p,
       keys alone are compared. */
    function seek(list,ofStore,k,p,after){let a=0,b=list.length;while(a<b){const m=(a+b)>>>1,c=compare(list[m][0],k)||(p===undefined?0:compare(primaryOf(list[m],ofStore),p));if(c<0||after&&!c)a=m+1;else b=m;}return a;}
    /* The positions [lo, hi) of the entries whose key is in the range. */
    function span(list,ofStore,query){const lo=query?.lower===undefined?0:seek(list,ofStore,query.lower,undefined,query.lowerOpen),hi=query?.upper===undefined?list.length:seek(list,ofStore,query.upper,undefined,!query.upperOpen);return [lo,Math.max(lo,hi)];}
    function order(source){const s=records.get(source);if(s.kind==='IDBObjectStore')return {list:storeOf(s).records,ofStore:true};const index=indexOf(s);return {list:indexOrder(s.of,index),ofStore:false};}
    /* Keep a store's kept index orders in step with one record replaced: old (or none) by record
       (or none), both with the same primary key. */
    function reorder(st,old,record){
        const kept=orders.get(st);if(!kept)return;
        for(const id in kept){
            const list=kept[id],n=+id;
            if(old)for(const k of entryKeys(old,n)){const i=seek(list,false,k,old[0],false);if(i<list.length&&list[i][1]===old)list.splice(i,1);else{orders.drop(st);return;}}
            if(record)for(const k of entryKeys(record,n))list.splice(seek(list,false,k,record[0],false),0,[k,record]);
        }
    }
    function rows(source,query){const {list,ofStore}=order(source),[lo,hi]=span(list,ofStore,query),result=[];for(let i=lo;i<hi;i++){const e=list[i],record=ofStore?e:e[1];result.push({key:e[0],primaryKey:record[0],record});}return result;}
    function write(source,v,provided,hasKey,addOnly){const s=existing(get(source,'IDBObjectStore'));live(s.tx,true);const detached=clone(v),store=storeOf(s);let candidate=provided;
        if(store.keyPath!==null){if(hasKey)fail('DataError');candidate=path(detached,store.keyPath);}
        let initial;if(candidate===undefined){if(!store.autoIncrement)fail('DataError');}else initial=key(candidate);
        return storeRecord(s,source,detached,initial,addOnly);
    }
    /* "delete records from an object store": the operation of a store's delete and a cursor's. */
    const deleteRecords=(s,query)=>()=>{const st=storeOf(s),[lo,hi]=span(st.records,true,query);if(hi-lo>64)orders.drop(st);else for(let i=lo;i<hi;i++)reorder(st,st.records[i],null);st.records.splice(lo,hi-lo);};
    /* "store a record into an object store", placed as a request whose source is the store handle
       (put, add) or the cursor (update). */
    function storeRecord(s,source,detached,initial,addOnly){
        /* Requests run in the order they were placed. Only a version change transaction changes an
           index set, and it is the only transaction then, so the indexes this request finds when it
           runs are the ones the store's handle has now: an index created after it is built by its
           own later step, one deleted after it is still there. */
        const present=storeOf(s).indexes.slice();
        return enqueue(s.tx,source,async r=>{const st=storeOf(s);let k=initial;if(k===undefined){if(st.generator>9007199254740992)fail('ConstraintError');k=st.generator;if(st.keyPath!==null)inject(detached,st.keyPath,k);}const at=insertion(st.records,k),exists=at<st.records.length&&!compare(st.records[at][0],k);if(addOnly&&exists)fail('ConstraintError');
            /* A unique index refuses a key another record (another primary key) has in it. */
            const indexes=[];for(const index of present){const keys=indexKeys(index,detached);indexes.push([index.id,keys]);if(index.unique){const list=indexOrder(st,index);for(const ik of keys)for(let i=seek(list,false,ik,undefined,false);i<list.length&&!compare(list[i][0],ik);i++)if(compare(list[i][1][0],k))fail('ConstraintError');}}
            const staged=await native('stage',r.nativeToken,pack(cloneData.serialize(detached,undefined,true)));status(staged[0]);if(r.finished)return;const record=[k,staged[1],indexes];reorder(st,exists?st.records[at]:null,record);if(exists)st.records[at]=record;else st.records.splice(at,0,record);if(st.autoIncrement&&typeof k==='number'&&k>=st.generator)st.generator=Math.floor(k)+1;return clone(k);});
    }
    /* The query steps of a store or index handle: "has been deleted" (InvalidStateError) first, then
       "not active" (TransactionInactiveError), then the query's conversion. */
    function query(source,kind,q){const s=existing(records.get(source));live(s.tx);const r=range(q,kind==='count');
        return enqueue(s.tx,source,async()=>{
            const {list,ofStore}=order(source),[lo,hi]=span(list,ofStore,r);
            if(kind==='count')return hi-lo;
            if(lo===hi)return undefined;
            const record=ofStore?list[lo]:list[lo][1];
            return kind==='get'?value(record,s.tx):clone(record[0]);
        });
    }
    /* "Create a request to retrieve multiple items" for getAll (kind 'value'), getAllKeys ('key')
       and getAllRecords ('record'). getAll and getAllKeys take a potentially valid key range and
       a count, or else an IDBGetAllOptions (and ignore count); getAllRecords takes the options,
       converted as its WebIDL argument before the steps. A count of 0 is no limit. */
    function many(source,kind,queryOrOptions,countArgument){
        const s=records.get(source);
        let options=kind==='record'?getAllOptions(queryOrOptions):null;
        const counted=countArgument===undefined?undefined:enforceCount(countArgument);
        existing(s);live(s.tx);
        let r,count,order='next';
        /* An absent query (undefined or null) is the unbounded range with the count argument, as
           before IDBGetAllOptions; only an options dictionary replaces the count. */
        if(!options&&(queryOrOptions==null||potentialRange(queryOrOptions))){r=range(queryOrOptions,true);count=counted;}
        else{if(!options)options=getAllOptions(queryOrOptions);r=range(options.query,true);count=options.count;order=options.direction;}
        return enqueue(s.tx,source,async()=>{
            let list=rows(source,r);
            if(order.endsWith('unique'))list=list.filter((v,i)=>!i||compare(v.key,list[i-1].key));
            if(order.startsWith('prev'))list.reverse();
            const n=count?Math.min(count,list.length):list.length,result=[];
            for(let i=0;i<n;i++){
                if(kind!=='key'&&i%32===0)
                    await prefetch(list.slice(i,Math.min(i+32,n)).map(row=>row.record),s.tx);
                if(get(s.tx,'IDBTransaction').finished)return;
                const row=list[i];
                result.push(kind==='key'?clone(row.primaryKey):kind==='value'?await value(row.record,s.tx):
                    new IDBRecord(token,clone(row.key),clone(row.primaryKey),await value(row.record,s.tx)));
            }
            return result;
        });
    }
    class IDBRecord {
        constructor(t,k,primaryKey,v){if(t!==token)throw new TypeError('Illegal IDBRecord constructor');records.set(this,{kind:'IDBRecord',key:k,primaryKey,value:v});}
        get key(){return clone(get(this,'IDBRecord').key);}
        get primaryKey(){return clone(get(this,'IDBRecord').primaryKey);}
        get value(){return get(this,'IDBRecord').value;}
    }
    class IDBObjectStore {
        constructor(t,tx,store){if(t!==token)throw new TypeError('Illegal IDBObjectStore constructor');records.set(this,{kind:'IDBObjectStore',tx,id:store.id,name:store.name,view:view(tx),store,keyPath:undefined,indexes:new Map()});}
        get name(){const s=get(this,'IDBObjectStore');storeOf(s);return s.name;}
        set name(value){const s=get(this,'IDBObjectStore');value=strings(value);existing(s);const r=get(s.tx,'IDBTransaction');if(r.mode!=='versionchange')fail('InvalidStateError');live(s.tx);const st=storeOf(s);if(st.name===value)return;if(r.data.stores.some(x=>x.name===value))fail('ConstraintError');st.name=value;s.name=value;}
        /* The same converted key path each time this handle is asked. */
        get keyPath(){const s=get(this,'IDBObjectStore');if(s.keyPath===undefined)s.keyPath=clone(storeOf(s).keyPath);return s.keyPath;}
        /* A deleted store's handle has an empty index set (deleteObjectStore). */
        get indexNames(){const s=get(this,'IDBObjectStore');return names(deleted(s)?[]:storeOf(s).indexes.map(i=>i.name));}
        get transaction(){return get(this,'IDBObjectStore').tx;}
        get autoIncrement(){return storeOf(get(this,'IDBObjectStore')).autoIncrement;}
        put(v,k){if(!arguments.length)throw new TypeError('Value required');return write(this,v,k,arguments.length>1,false);}
        add(v,k){if(!arguments.length)throw new TypeError('Value required');return write(this,v,k,arguments.length>1,true);}
        get(q){if(!arguments.length)throw new TypeError('Query required');return query(this,'get',q);}
        getKey(q){if(!arguments.length)throw new TypeError('Query required');return query(this,'getKey',q);}
        getAll(q,c){return many(this,'value',q,c);}
        getAllKeys(q,c){return many(this,'key',q,c);}
        getAllRecords(options){return many(this,'record',options);}
        count(q){return query(this,'count',q);}
        delete(q){if(!arguments.length)throw new TypeError('Query required');const s=existing(get(this,'IDBObjectStore'));live(s.tx,true);return enqueue(s.tx,this,deleteRecords(s,range(q)));}
        clear(){const s=existing(get(this,'IDBObjectStore'));live(s.tx,true);return enqueue(s.tx,this,()=>{const st=storeOf(s);orders.drop(st);st.records=[];});}
        index(name){const s=get(this,'IDBObjectStore');name=strings(name);existing(s);if(get(s.tx,'IDBTransaction').finished)fail('InvalidStateError');const index=storeOf(s).indexes.find(i=>i.name===name);if(!index)fail('NotFoundError');return indexHandle(this,index);}
        /* The arguments are converted first (the key path is not nullable, so null is "null"; the
           options' members in lexicographic order), then: not an upgrade, store deleted
           (InvalidStateError), not active, the name taken (ConstraintError), an invalid key path
           (SyntaxError), a sequence key path with multiEntry (InvalidAccessError). */
        createIndex(name,p,options){const s=get(this,'IDBObjectStore');if(arguments.length<2)throw new TypeError('Name and key path required');name=strings(name);p=keyPathValue(p);options=dictionary(options);const multiEntry=!!options?.multiEntry,unique=!!options?.unique;
            const r=get(s.tx,'IDBTransaction');if(r.mode!=='versionchange')fail('InvalidStateError');existing(s);live(s.tx);const st=storeOf(s);if(st.indexes.some(i=>i.name===name))fail('ConstraintError');if(!validKeyPath(p))fail('SyntaxError');if(Array.isArray(p)&&multiEntry)fail('InvalidAccessError');const index={id:r.data.nextId++,name,keyPath:p,unique,multiEntry};
            st.indexes.push(index);
            /* The build runs in order even when the index is deleted before it runs: requests
               placed between see its entries, and deleteIndex's later step removes them. */
            r.queue.push({request:null,operation:async()=>{
                const seen=[];orders.drop(st);
                for(let i=0;i<st.records.length;i++){
                    if(i%32===0)await prefetch(st.records.slice(i,i+32),s.tx);
                    const record=st.records[i];
                    const keys=indexKeys(index,await value(record,s.tx));
                    if(r.finished)return;
                    /* seen is sorted: a unique index's keys so far. */
                    if(index.unique)for(const k of keys){let a=0,b=seen.length;while(a<b){const m=(a+b)>>>1;if(compare(seen[m],k)<0)a=m+1;else b=m;}if(a<seen.length&&!compare(seen[a],k))fail('ConstraintError');seen.splice(a,0,k);}
                    if(!record[2])record[2]=[];
                    record[2]=record[2].filter(entry=>entry[0]!==index.id);
                    record[2].push([index.id,keys]);
                }
                orders.drop(st);
            }});
            return indexHandle(this,index);}
        deleteIndex(name){const s=get(this,'IDBObjectStore');name=strings(name);const r=get(s.tx,'IDBTransaction');if(r.mode!=='versionchange')fail('InvalidStateError');existing(s);live(s.tx);const st=storeOf(s),i=st.indexes.findIndex(x=>x.name===name);if(i<0)fail('NotFoundError');const index=st.indexes[i];st.indexes.splice(i,1);
            /* The handle loses the index now; its records go when the requests placed before this
               call have run, in order, as createIndex builds one. A rename never touches them. */
            r.queue.push({request:null,operation:async()=>{orders.drop(st);for(const record of st.records)if(record[2])record[2]=record[2].filter(v=>v[0]!==index.id);}});}
        openCursor(q,d='next'){return cursor(this,q,d,true);}
        openKeyCursor(q,d='next'){return cursor(this,q,d,false);}
    }
    class IDBIndex {
        constructor(t,store,index){if(t!==token)throw new TypeError('Illegal IDBIndex constructor');const s=records.get(store);records.set(this,{kind:'IDBIndex',tx:s.tx,handle:store,id:index.id,name:index.name,of:storeOf(s),index,keyPath:undefined});}
        get name(){const s=get(this,'IDBIndex');indexOf(s);return s.name;}
        /* A rename changes the index's name and the handle's, never the records, which hold the
           index's id. */
        set name(value){const s=get(this,'IDBIndex');value=strings(value);const r=get(s.tx,'IDBTransaction');if(r.mode!=='versionchange')fail('InvalidStateError');live(s.tx);existing(s);const index=indexOf(s);if(index.name===value)return;if(s.of.indexes.some(i=>i.name===value))fail('ConstraintError');index.name=value;s.name=value;}
        get objectStore(){return get(this,'IDBIndex').handle;}
        get keyPath(){const s=get(this,'IDBIndex');if(s.keyPath===undefined)s.keyPath=clone(indexOf(s).keyPath);return s.keyPath;}
        get unique(){return indexOf(get(this,'IDBIndex')).unique;}
        get multiEntry(){return indexOf(get(this,'IDBIndex')).multiEntry;}
        get(q){if(!arguments.length)throw new TypeError('Query required');return query(this,'get',q);}
        getKey(q){if(!arguments.length)throw new TypeError('Query required');return query(this,'getKey',q);}
        getAll(q,c){return many(this,'value',q,c);}
        getAllKeys(q,c){return many(this,'key',q,c);}
        getAllRecords(options){return many(this,'record',options);}
        count(q){return query(this,'count',q);}
        openCursor(q,d='next'){return cursor(this,q,d,true);}
        openKeyCursor(q,d='next'){return cursor(this,q,d,false);}
    }
    /* One index handle per index and store handle: createIndex's, then index()'s each time. */
    function indexHandle(store,index){const s=records.get(store);let handle=s.indexes.get(index.id);if(!handle){handle=new IDBIndex(token,store,index);s.indexes.set(index.id,handle);}return handle;}
    function cursor(source,q,d,withValue){d=direction(d);const s=existing(records.get(source));live(s.tx);const query=range(q,true),request=new IDBRequest(token,source,s.tx),object=new(withValue?IDBCursorWithValue:IDBCursor)(token,source,request,d,query);const r=get(object,'IDBCursor');get(s.tx,'IDBTransaction').queue.push({request,operation:()=>move(object,undefined,undefined,1)});return request;}
    async function move(object,to,primary,amount){
        /* "Iterate a cursor", count times: the first entry in range past the position (past its
           key alone when unique) and at or past the key (and primary key) asked for, or the last
           such before them going back; prevunique takes the first entry of the key it lands on. */
        const r=get(object,'IDBCursor'),{list,ofStore}=order(r.source),[lo,hi]=span(list,ofStore,r.query);
        const unique=r.direction.endsWith('unique'),forward=r.direction.startsWith('next');
        let at=-1;
        for(let n=0;n<amount;n++){
            const p=n?{key:list[at][0],primaryKey:primaryOf(list[at],ofStore)}:r.position;let i;
            if(forward){i=lo;if(p)i=Math.max(i,seek(list,ofStore,p.key,unique?undefined:p.primaryKey,true));if(to!==undefined)i=Math.max(i,seek(list,ofStore,to,primary,false));at=i<hi?i:-1;}
            else{i=hi;if(p)i=Math.min(i,seek(list,ofStore,p.key,unique?undefined:p.primaryKey,false));if(to!==undefined)i=Math.min(i,seek(list,ofStore,to,primary,true));i--;if(i>=lo&&unique)i=Math.max(lo,seek(list,ofStore,list[i][0],undefined,false));at=i>=lo?i:-1;}
            if(at<0)break;
        }
        const recordAt=i=>ofStore?list[i]:list[i][1];
        r.position=at<0?null:{key:list[at][0],primaryKey:recordAt(at)[0],record:recordAt(at)};
        if(r.position&&object instanceof IDBCursorWithValue){
            const tx=records.get(r.source).tx;
            if(!get(tx,'IDBTransaction').valueCache?.has(r.position.record[1])){
                const ahead=[];
                if(forward)for(let i=at;i<Math.min(hi,at+32);i++)ahead.push(recordAt(i));
                else for(let i=at;i>=Math.max(lo,at-31);i--)ahead.push(recordAt(i));
                await prefetch(ahead,tx);
            }
            if(get(tx,'IDBTransaction').finished)return null;
            r.value=await value(r.position.record,tx);
        }else r.value=undefined;
        r.pending=false;return r.position?object:null;
    }
    class IDBCursor {
        constructor(t,source,request,direction,query){if(t!==token)throw new TypeError('Illegal IDBCursor constructor');records.set(this,{kind:'IDBCursor',source,request,direction,query,position:null,pending:true});}
        get source(){return get(this,'IDBCursor').source;}
        get direction(){return get(this,'IDBCursor').direction;}
        get request(){return get(this,'IDBCursor').request;}
        get key(){const r=get(this,'IDBCursor');return r.position?clone(r.position.key):undefined;}
        get primaryKey(){const r=get(this,'IDBCursor');return r.position?clone(r.position.primaryKey):undefined;}
        continue(k){iterate(this,k,undefined,1,false);}
        advance(n){if(!arguments.length)throw new TypeError('Count required');n=enforceCount(n);if(n===0)throw new TypeError('Invalid advance count');iterate(this,undefined,undefined,n,false);}
        continuePrimaryKey(k,p){iterate(this,k,p,1,true);}
        /* Not active, read-only, source or effective store deleted, no got value, key only; then
           the clone, and with in-line keys the key the clone yields must be the effective key
           (DataError). The request's source is the cursor. The value is a required argument. */
        update(v){const r=get(this,'IDBCursor'),s=records.get(r.source);if(!arguments.length)throw new TypeError('Value required');live(s.tx,true);existing(s);if(r.pending||!r.position||!(this instanceof IDBCursorWithValue))fail('InvalidStateError');const detached=clone(v),store=records.get(effectiveStore(r)),st=storeOf(store);
            if(st.keyPath!==null&&compare(key(path(detached,st.keyPath)),r.position.primaryKey))fail('DataError');
            return storeRecord(store,this,detached,r.position.primaryKey,false);}
        delete(){const r=get(this,'IDBCursor'),s=records.get(r.source);live(s.tx,true);existing(s);if(r.pending||!r.position||!(this instanceof IDBCursorWithValue))fail('InvalidStateError');return enqueue(s.tx,this,deleteRecords(records.get(effectiveStore(r)),range(r.position.primaryKey)));}
    }
    /* The cursor's effective object store, as its handle: its source index's, or its source. */
    const effectiveStore=r=>{const s=records.get(r.source);return s.kind==='IDBIndex'?s.handle:r.source;};
    /* advance, continue and continuePrimaryKey: "not active", then "source or effective object
       store has been deleted", then continuePrimaryKey's source and direction, then the got value
       flag, then the keys. */
    function iterate(object,k,p,n,primary){
        const r=get(object,'IDBCursor'),s=records.get(r.source),tx=live(s.tx);existing(s);
        if(primary&&(s.kind!=='IDBIndex'||r.direction.endsWith('unique')))fail('InvalidAccessError');
        if(r.pending||!r.position)fail('InvalidStateError');
        if(k!==undefined||primary){k=key(k);if(primary)p=key(p);const c=compare(k,r.position.key)||(p===undefined?0:compare(p,r.position.primaryKey));if(r.direction.startsWith('next')?c<=0:c>=0)fail('DataError');}
        r.pending=true;get(r.request,'IDBRequest').done=false;tx.queue.push({request:r.request,operation:()=>move(object,k,p,n)});
    }
    class IDBCursorWithValue extends IDBCursor {get value(){return get(this,'IDBCursor').value;}}
    class IDBDatabase extends EventTarget {
        /* schema is the connection's object store set: during an upgrade, the upgrade's own
           snapshot, which createObjectStore and deleteObjectStore change. */
        constructor(t,name,data){if(t!==token)throw new TypeError('Illegal IDBDatabase constructor');super();records.set(this,{kind:'IDBDatabase',name,version:data.version,schema:data,closed:false,upgrade:null,connection:0,transactions:new Set()});}
        get name(){return get(this,'IDBDatabase').name;}
        get version(){return get(this,'IDBDatabase').version;}
        get objectStoreNames(){return names(get(this,'IDBDatabase').schema.stores.map(s=>s.name));}
        close(){get(this,'IDBDatabase').closed=true;closing(this);}
        transaction(scope,mode='readonly',options={}){const r=get(this,'IDBDatabase');if(r.closed||r.upgrade)fail('InvalidStateError');scope=typeof scope==='string'?[scope]:Array.from(scope,strings);scope=[...new Set(scope)];if(!scope.length)fail('InvalidAccessError');for(const name of scope)if(!r.schema.stores.some(s=>s.name===name))fail('NotFoundError');mode=strings(mode);if(mode!=='readonly'&&mode!=='readwrite')throw new TypeError('Invalid transaction mode');if(options.durability!==undefined&&!['default','strict','relaxed'].includes(options.durability))throw new TypeError('Invalid durability');return new IDBTransaction(token,this,scope,mode,options);}
        /* The arguments are converted first (the options' members in lexicographic order; a null or
           missing key path is null), then: no upgrade (InvalidStateError), not active, an invalid
           key path (SyntaxError), the name taken (ConstraintError), then a key generator on an
           empty or sequence key path. */
        createObjectStore(name,options){const r=get(this,'IDBDatabase');if(!arguments.length)throw new TypeError('Name required');name=strings(name);options=dictionary(options);const autoIncrement=!!options?.autoIncrement,given=options?.keyPath,p=given==null?null:keyPathValue(given);
            if(!r.upgrade)fail('InvalidStateError');const tx=live(r.upgrade,true);if(p!==null&&!validKeyPath(p))fail('SyntaxError');if(tx.data.stores.some(s=>s.name===name))fail('ConstraintError');if(autoIncrement&&(p===''||Array.isArray(p)))fail('InvalidAccessError');tx.data.stores.push({id:tx.data.nextId++,name,keyPath:p,autoIncrement,generator:1,records:[],indexes:[]});return r.upgrade.objectStore(name);}
        /* The store leaves the connection's object store set; its handle's index set empties with
           it (deleted), and requests placed before still run against it. */
        deleteObjectStore(name){const r=get(this,'IDBDatabase');name=strings(name);if(!r.upgrade)fail('InvalidStateError');const tx=live(r.upgrade,true);const i=tx.data.stores.findIndex(s=>s.name===name);if(i<0)fail('NotFoundError');tx.data.stores.splice(i,1);}
    }
    function open(name,version,deleting=false){name=strings(name);if(version!==undefined){version=Number(version);if(!Number.isSafeInteger(version)||version<=0)throw new TypeError('Invalid database version');}
        /* "obtain a storage key": failure throws now, before a request exists. */
        if(typeof storageKey==='function'&&!storageKey())throw new DOMException('Storage is not available to this origin','SecurityError');
        const request=new IDBOpenDBRequest(token);let blocked=false;
        async function attempt(){let a;try{a=await native('begin',name);if(retired){if(a[0]===0)discard(a[1]);return;}if(a[0]===1){later(attempt,5);return;}status(a[0]);const data=adopt(a[2]===null?{version:0,stores:[]}:unpack(a[2])),wanted=deleting?0:version===undefined?data.version||1:version;
                if(!deleting&&wanted<data.version){await native('abort',a[1]);finish(request,undefined,new DOMException('Requested version is older','VersionError'));return;}
                const list=connections.get(name)||[];
                /* versionchange goes to every other open connection (each realm's poll fires it);
                   blocked waits until all of them were fired, then fires once if any is still
                   open; the open goes on when none is. active: [status, open, not yet fired]. */
                if(deleting||wanted>data.version){const active=await native('connections',a[1],deleting?0:wanted);if(active[0]===1){await native('abort',a[1]);later(attempt,5);return;}status(active[0]);if(active[1]){await native('abort',a[1]);if(!blocked&&!active[2]){blocked=true;versionEvent(request,'blocked',data.version,deleting?null:wanted);}later(attempt,50);return;}}
                if(deleting){status(await native('commit',a[1],pack({version:0,stores:[]}),[],0));const rr=get(request,'IDBRequest');rr.done=true;rr.result=undefined;rr.error=null;versionEvent(request,'success',data.version,null);return;}
                let connection=0;if(wanted===data.version){const c=await native('connect',a[1],wanted);if(c[0]===1){await native('abort',a[1]);later(attempt,5);return;}status(c[0]);connection=c[1];}const db=new IDBDatabase(token,name,data);get(db,'IDBDatabase').connection=connection;list.push(db);connections.set(name,list);watch();
                /* "Upgrade a database": the connection's object store set is the upgrade's snapshot,
                   whose version is the new one; before keeps what an abort gives back. */
                if(wanted>data.version){const tx=new IDBTransaction(token,db,[],'versionchange'),r=get(tx,'IDBTransaction'),d=get(db,'IDBDatabase');r.nativeToken=a[1];r.before=outline(data);r.data=data;r.oldVersion=data.version;r.upgradeVersion=wanted;r.openRequest=request;d.upgrade=tx;d.version=wanted;r.data.version=wanted;activate(tx);const rr=get(request,'IDBRequest');rr.result=db;rr.done=true;rr.transaction=tx;const e=versionEvent(request,'upgradeneeded',r.oldVersion,wanted);if(eventState(e).listenerException)abort(tx,new DOMException('Upgrade listener threw','AbortError'));}
                else {status(await native('abort',a[1]));finish(request,db);}
            }catch(e){if(a?.[1])discard(a[1]);finish(request,undefined,e);}}
        later(attempt,0);return request;
    }
    class IDBFactory {constructor(t){if(t!==token)throw new TypeError('Illegal IDBFactory constructor');}open(name,version){if(!arguments.length)throw new TypeError('Database name required');return open(name,version);}deleteDatabase(name){if(!arguments.length)throw new TypeError('Database name required');return open(name,undefined,true);}cmp(a,b){if(arguments.length<2)throw new TypeError('Two keys required');return compare(key(a),key(b));}databases(){return new Promise((resolve,reject)=>{async function attempt(){try{const a=await native('databases');if(a[0]===1){later(attempt,5);return;}status(a[0]);resolve(a[1]);}catch(e){reject(e);}}later(attempt,0);});}}
    indexedDBHandlerTarget=(v,type)=>{const r=records.get(v);return !!r&&(r.kind==='IDBRequest'&&(['success','error'].includes(type)||v instanceof IDBOpenDBRequest&&['blocked','upgradeneeded'].includes(type))||r.kind==='IDBDatabase'&&['abort','error','versionchange','close'].includes(type)||r.kind==='IDBTransaction'&&['abort','complete','error'].includes(type));};
    for(const [ctor,events]of [[IDBRequest,['success','error']],[IDBOpenDBRequest,['blocked','upgradeneeded']],[IDBDatabase,['abort','error','versionchange','close']],[IDBTransaction,['abort','complete','error']]])installHandlers(ctor.prototype,events);
    /* WebIDL: the members of an interface (and its static operations) are enumerable. */
    for(const ctor of [IDBFactory,IDBRequest,IDBOpenDBRequest,IDBDatabase,IDBTransaction,IDBObjectStore,IDBIndex,IDBCursor,IDBCursorWithValue,IDBKeyRange,IDBVersionChangeEvent,IDBRecord]){
        for(const holder of [ctor.prototype,ctor])for(const name of Object.getOwnPropertyNames(holder)){
            if(name==='constructor'||name==='prototype'||name==='length'||name==='name'||name.startsWith('_'))continue;
            Object.defineProperty(holder,name,{enumerable:true});
        }
        Object.defineProperty(ctor.prototype,Symbol.toStringTag,{value:ctor.name,configurable:true});Object.defineProperty(globalThis,ctor.name,{value:ctor,writable:true,configurable:true});
    }
    /* WindowOrWorkerGlobalScope's indexedDB: a read-only attribute of the global, an accessor
       whose getter (named "get indexedDB") answers the realm's one IDBFactory. */
    const factory=new IDBFactory(token),globalScope=globalThis;
    Object.defineProperty(globalThis,'indexedDB',{configurable:true,enumerable:true,
        get:Object.getOwnPropertyDescriptor({get indexedDB(){if(this!=null&&this!==globalScope)throw new TypeError('Illegal invocation');return factory;}},'indexedDB').get});
    cloneData.registerUncloneable(v=>records.has(v),'IndexedDB object');
    return {taskEnd,retire(){retired=true;for(const list of connections.values())for(const db of list){const r=get(db,'IDBDatabase');r.closed=true;for(const tx of r.transactions)abort(tx);closing(db);}connections.clear();invoke(setClear,activeTasks,[]);}};
})();
