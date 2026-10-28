/* IndexedDB owns a separate native transactional store. Values use the same
 * private structured-clone graph as messages/history, including cycles and
 * binary buffers; neither JSON nor localStorage is used for persistence. */
const indexedDBBridge=(()=>{
    'use strict';
    const {Date,Uint8Array,ArrayBuffer,Array,Number,String,Object,Set,Map,WeakMap,DOMException,TypeError,Reflect}=globalThis;
    const native=host.indexedDB,storageKey=host.indexedDBStorageKey,pack=host.pack,unpack=host.unpack;
    if(typeof native!=='function')return null;
    const invoke=Reflect.apply,wmGet=WeakMap.prototype.get,wmSet=WeakMap.prototype.set,wmHas=WeakMap.prototype.has;
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
    function keyPath(value){if(value===null)return null;const p=Array.isArray(value)?value.map(strings):strings(value);const valid=x=>x===''||/^[\p{ID_Start}_$][\p{ID_Continue}$\u200c\u200d]*(?:\.[\p{ID_Start}_$][\p{ID_Continue}$\u200c\u200d]*)*$/u.test(x);if(Array.isArray(p)?p.some(x=>!valid(x)):!valid(p))fail('SyntaxError');return p;}
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
    function schema(s){const tx=get(s.tx,'IDBTransaction');const store=(tx.data||tx.dbRecord.schema)?.stores.find(v=>v.name===s.name);if(!store)fail('InvalidStateError');return store;}
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
        if(r.openRequest){r.dbRecord.upgrade=null;r.dbRecord.closed=true;}
        closing(r.db);
        const queue=r.executing?[r.executing,...r.queue]:r.queue;
        r.executing=null;r.queue=[];
        later(()=>{
            for(const q of queue)if(q.request)finish(q.request,undefined,new DOMException('Transaction aborted','AbortError'));
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
            r.dbRecord.stores=r.data.stores.map(v=>v.name);r.dbRecord.transactions.delete(tx);r.dbRecord.upgrade=null;
            closing(r.db);
            later(()=>{fire(tx,'complete');if(r.openRequest){get(r.openRequest,'IDBRequest').transaction=null;finish(r.openRequest,r.db);}},0);
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
                status(a[0]);r.nativeToken=a[1];r.data=a[2]===null?{version:0,stores:[]}:unpack(a[2]);
                if(!r.data||!Array.isArray(r.data.stores)||!Number.isSafeInteger(r.data.version))fail('UnknownError','Invalid IndexedDB snapshot');
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
        get objectStoreNames(){return names(get(this,'IDBTransaction').scope);}
        objectStore(name){const r=get(this,'IDBTransaction');if(r.finished)fail('InvalidStateError');name=strings(name);if(!r.scope.includes(name))fail('NotFoundError');if(!r.stores.has(name))r.stores.set(name,new IDBObjectStore(token,this,name));return r.stores.get(name);}
        abort(){const r=get(this,'IDBTransaction');if(r.finished||r.committing)fail('InvalidStateError');abort(this);}
        commit(){const r=live(this);r.committing=true;r.active=false;}
    }
    function insertion(records,k){let a=0,b=records.length;while(a<b){const m=(a+b)>>>1;if(compare(records[m][0],k)<0)a=m+1;else b=m;}return a;}
    function indexKeys(index,v){let raw=path(v,index.keyPath),out=[];if(index.multiEntry&&Array.isArray(raw)){for(const candidate of raw)try{const k=key(candidate);if(!out.some(x=>!compare(x,k)))out.push(k);}catch(e){if(e.name!=='DataError')throw e;}}else try{out.push(key(raw));}catch(e){if(e.name!=='DataError')throw e;}return out;}
    function rows(source,query){const s=records.get(source),store=schema(s),result=[];if(s.kind==='IDBObjectStore'){let begin=0,end=store.records.length;if(query?.lower!==undefined){begin=insertion(store.records,query.lower);if(query.lowerOpen&&begin<end&&!compare(store.records[begin][0],query.lower))begin++;}if(query?.upper!==undefined){end=insertion(store.records,query.upper);if(!query.upperOpen&&end<store.records.length&&!compare(store.records[end][0],query.upper))end++;}for(let i=begin;i<end;i++){const r=store.records[i];result.push({key:r[0],primaryKey:r[0],record:r});}}
        else {const index=store.indexes.find(i=>i.name===s.index);if(!index)fail('InvalidStateError');for(const r of store.records)for(const k of r[2]?.find(v=>v[0]===s.index)?.[1]||[])if(matches(k,query))result.push({key:k,primaryKey:r[0],record:r});result.sort((a,b)=>compare(a.key,b.key)||compare(a.primaryKey,b.primaryKey));}return result;}
    function write(source,v,provided,hasKey,addOnly){const s=get(source,'IDBObjectStore');live(s.tx,true);const detached=clone(v),store=schema(s);let candidate=provided;
        if(store.keyPath!==null){if(hasKey)fail('DataError');candidate=path(detached,store.keyPath);}
        let initial;if(candidate===undefined){if(!store.autoIncrement)fail('DataError');}else initial=key(candidate);
        /* Requests run in the order they were placed. Only a version change transaction changes an
           index set, and it is the only transaction then, so the indexes this request finds when it
           runs are the ones the store's handle has now: an index created after it is built by its
           own later step, one deleted after it is still there. */
        const present=store.indexes.slice();
        return enqueue(s.tx,source,async r=>{const st=schema(s);let k=initial;if(k===undefined){if(st.generator>9007199254740992)fail('ConstraintError');k=st.generator;if(st.keyPath!==null)inject(detached,st.keyPath,k);}const at=insertion(st.records,k),exists=at<st.records.length&&!compare(st.records[at][0],k);if(addOnly&&exists)fail('ConstraintError');
            const indexes=[];for(const index of present){const keys=indexKeys(index,detached);indexes.push([index.name,keys]);if(index.unique)for(const old of st.records){if(!compare(old[0],k))continue;for(const oldKey of old[2]?.find(v=>v[0]===index.name)?.[1]||[])if(keys.some(ik=>!compare(ik,oldKey)))fail('ConstraintError');}}
            const staged=await native('stage',r.nativeToken,pack(cloneData.serialize(detached,undefined,true)));status(staged[0]);if(r.finished)return;const record=[k,staged[1],indexes];if(exists)st.records[at]=record;else st.records.splice(at,0,record);if(st.autoIncrement&&typeof k==='number'&&k>=st.generator)st.generator=Math.floor(k)+1;return clone(k);});
    }
    function query(source,kind,q){const s=records.get(source);live(s.tx);const r=range(q,kind==='count');
        return enqueue(s.tx,source,async()=>{
            const list=rows(source,r);
            if(kind==='count')return list.length;
            if(kind==='get')return list.length?value(list[0].record,s.tx):undefined;
            return list.length?clone(list[0].primaryKey):undefined;
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
        live(s.tx);
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
        constructor(t,tx,name){if(t!==token)throw new TypeError('Illegal IDBObjectStore constructor');records.set(this,{kind:'IDBObjectStore',tx,name});}
        get name(){return get(this,'IDBObjectStore').name;}
        set name(value){const s=get(this,'IDBObjectStore'),r=live(s.tx,true);if(r.mode!=='versionchange')fail('InvalidStateError');const st=schema(s);value=strings(value);if(value===s.name)return;if(r.data.stores.some(x=>x.name===value))fail('ConstraintError');st.name=value;r.scope[r.scope.indexOf(s.name)]=value;r.stores.delete(s.name);s.name=value;r.stores.set(value,this);}
        get keyPath(){return clone(schema(get(this,'IDBObjectStore')).keyPath);}
        get indexNames(){return names(schema(get(this,'IDBObjectStore')).indexes.map(i=>i.name));}
        get transaction(){return get(this,'IDBObjectStore').tx;}
        get autoIncrement(){return schema(get(this,'IDBObjectStore')).autoIncrement;}
        put(v,k){if(!arguments.length)throw new TypeError('Value required');return write(this,v,k,arguments.length>1,false);}
        add(v,k){if(!arguments.length)throw new TypeError('Value required');return write(this,v,k,arguments.length>1,true);}
        get(q){if(!arguments.length)throw new TypeError('Query required');return query(this,'get',q);}
        getKey(q){if(!arguments.length)throw new TypeError('Query required');return query(this,'getKey',q);}
        getAll(q,c){return many(this,'value',q,c);}
        getAllKeys(q,c){return many(this,'key',q,c);}
        getAllRecords(options){return many(this,'record',options);}
        count(q){return query(this,'count',q);}
        delete(q){const s=get(this,'IDBObjectStore');live(s.tx,true);if(!arguments.length)throw new TypeError('Query required');const r=range(q);return enqueue(s.tx,this,()=>{const st=schema(s);st.records=st.records.filter(record=>!matches(record[0],r));});}
        clear(){const s=get(this,'IDBObjectStore');live(s.tx,true);return enqueue(s.tx,this,()=>{schema(s).records=[];});}
        index(name){const s=get(this,'IDBObjectStore');name=strings(name);if(!schema(s).indexes.some(i=>i.name===name))fail('NotFoundError');return new IDBIndex(token,s.tx,s.name,name);}
        createIndex(name,p,options={}){const s=get(this,'IDBObjectStore'),r=live(s.tx,true);if(r.mode!=='versionchange')fail('InvalidStateError');const st=schema(s);name=strings(name);if(st.indexes.some(i=>i.name===name))fail('ConstraintError');p=keyPath(p);if(p===null)throw new TypeError('Key path required');if(Array.isArray(p)&&options.multiEntry)fail('InvalidAccessError');const index={name,keyPath:p,unique:!!options.unique,multiEntry:!!options.multiEntry};
            st.indexes.push(index);
            r.queue.push({request:null,operation:async()=>{
                if(!st.indexes.includes(index))return;
                const seen=[];
                for(let i=0;i<st.records.length;i++){
                    if(i%32===0)await prefetch(st.records.slice(i,i+32),s.tx);
                    const record=st.records[i];
                    const keys=indexKeys(index,await value(record,s.tx));
                    if(r.finished)return;
                    if(index.unique)for(const k of keys){if(seen.some(x=>!compare(x,k)))fail('ConstraintError');seen.push(k);}
                    if(!record[2])record[2]=[];
                    record[2]=record[2].filter(entry=>entry[0]!==index.name);
                    record[2].push([index.name,keys]);
                }
            }});
            return new IDBIndex(token,s.tx,s.name,name);}
        deleteIndex(name){const s=get(this,'IDBObjectStore'),r=live(s.tx,true);if(r.mode!=='versionchange')fail('InvalidStateError');const st=schema(s);name=strings(name);const i=st.indexes.findIndex(x=>x.name===name);if(i<0)fail('NotFoundError');const index=st.indexes[i];st.indexes.splice(i,1);
            /* The handle loses the index now; its records go when the requests placed before this
               call have run, in order, as createIndex builds one. */
            r.queue.push({request:null,operation:async()=>{for(const record of st.records)if(record[2])record[2]=record[2].filter(v=>v[0]!==index.name);}});}
        openCursor(q,d='next'){return cursor(this,q,d,true);}
        openKeyCursor(q,d='next'){return cursor(this,q,d,false);}
    }
    class IDBIndex {
        constructor(t,tx,name,index){if(t!==token)throw new TypeError('Illegal IDBIndex constructor');records.set(this,{kind:'IDBIndex',tx,name,index});}
        get name(){return get(this,'IDBIndex').index;}
        set name(value){const s=get(this,'IDBIndex'),r=live(s.tx,true);if(r.mode!=='versionchange')fail('InvalidStateError');const st=schema(s),index=st.indexes.find(i=>i.name===s.index);if(!index)fail('InvalidStateError');value=strings(value);if(value===s.index)return;if(st.indexes.some(i=>i.name===value))fail('ConstraintError');for(const record of st.records){const entry=record[2]?.find(v=>v[0]===s.index);if(entry)entry[0]=value;}index.name=value;s.index=value;}
        get objectStore(){const s=get(this,'IDBIndex');return s.tx.objectStore(s.name);}
        get keyPath(){const s=get(this,'IDBIndex'),i=schema(s).indexes.find(i=>i.name===s.index);if(!i)fail('InvalidStateError');return clone(i.keyPath);}
        get unique(){const s=get(this,'IDBIndex'),i=schema(s).indexes.find(i=>i.name===s.index);if(!i)fail('InvalidStateError');return i.unique;}
        get multiEntry(){const s=get(this,'IDBIndex'),i=schema(s).indexes.find(i=>i.name===s.index);if(!i)fail('InvalidStateError');return i.multiEntry;}
        get(q){if(!arguments.length)throw new TypeError('Query required');return query(this,'get',q);}
        getKey(q){if(!arguments.length)throw new TypeError('Query required');return query(this,'getKey',q);}
        getAll(q,c){return many(this,'value',q,c);}
        getAllKeys(q,c){return many(this,'key',q,c);}
        getAllRecords(options){return many(this,'record',options);}
        count(q){return query(this,'count',q);}
        openCursor(q,d='next'){return cursor(this,q,d,true);}
        openKeyCursor(q,d='next'){return cursor(this,q,d,false);}
    }
    function cursor(source,q,d,withValue){const s=records.get(source);live(s.tx);d=strings(d);if(!['next','nextunique','prev','prevunique'].includes(d))throw new TypeError('Invalid cursor direction');const query=range(q,true),request=new IDBRequest(token,source,s.tx),object=new(withValue?IDBCursorWithValue:IDBCursor)(token,source,request,d,query);const r=get(object,'IDBCursor');get(s.tx,'IDBTransaction').queue.push({request,operation:()=>move(object,undefined,undefined,1)});return request;}
    async function move(object,to,primary,amount){
        const r=get(object,'IDBCursor');let list=rows(r.source,r.query);
        const unique=r.direction.endsWith('unique');
        if(unique)list=list.filter((v,i)=>!i||compare(v.key,list[i-1].key));
        if(r.direction.startsWith('prev'))list.reverse();
        const forward=r.direction.startsWith('next');
        if(r.position)list=list.filter(v=>{
            const c=compare(v.key,r.position.key)||(unique?0:compare(v.primaryKey,r.position.primaryKey));
            return forward?c>0:c<0;
        });
        if(to!==undefined)list=list.filter(v=>{
            const c=compare(v.key,to)||(primary===undefined?0:compare(v.primaryKey,primary));
            return forward?c>=0:c<=0;
        });
        r.position=list[amount-1]||null;
        if(r.position&&object instanceof IDBCursorWithValue){
            const tx=records.get(r.source).tx;
            if(!get(tx,'IDBTransaction').valueCache?.has(r.position.record[1]))
                await prefetch(list.slice(amount-1,amount+31).map(row=>row.record),tx);
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
        continue(k){this._continue(k,undefined,1);}
        advance(n){if(!arguments.length)throw new TypeError('Count required');n=enforceCount(n);if(n===0)throw new TypeError('Invalid advance count');this._continue(undefined,undefined,n);}
        continuePrimaryKey(k,p){const r=get(this,'IDBCursor');if(records.get(r.source).kind!=='IDBIndex'||r.direction.endsWith('unique'))fail('InvalidAccessError');this._continue(key(k),key(p),1);}
        _continue(k,p,n){const r=get(this,'IDBCursor'),s=records.get(r.source),tx=live(s.tx);if(r.pending||!r.position)fail('InvalidStateError');if(k!==undefined){k=key(k);const c=compare(k,r.position.key)||(p===undefined?0:compare(p,r.position.primaryKey));if(r.direction.startsWith('next')?c<=0:c>=0)fail('DataError');}r.pending=true;get(r.request,'IDBRequest').done=false;tx.queue.push({request:r.request,operation:()=>move(this,k,p,n)});}
        update(v){const r=get(this,'IDBCursor'),s=records.get(r.source);live(s.tx,true);if(r.pending||!r.position||!(this instanceof IDBCursorWithValue))fail('InvalidStateError');const store=s.tx.objectStore(s.name),st=schema(records.get(store));if(st.keyPath!==null){const candidate=key(path(v,st.keyPath));if(compare(candidate,r.position.primaryKey))fail('DataError');return store.put(v);}return store.put(v,r.position.primaryKey);}
        delete(){const r=get(this,'IDBCursor'),s=records.get(r.source);live(s.tx,true);if(r.pending||!r.position)fail('InvalidStateError');return s.tx.objectStore(s.name).delete(r.position.primaryKey);}
    }
    class IDBCursorWithValue extends IDBCursor {get value(){return get(this,'IDBCursor').value;}}
    class IDBDatabase extends EventTarget {
        constructor(t,name,data){if(t!==token)throw new TypeError('Illegal IDBDatabase constructor');super();records.set(this,{kind:'IDBDatabase',name,version:data.version,schema:data,stores:data.stores.map(s=>s.name),closed:false,upgrade:null,connection:0,transactions:new Set()});}
        get name(){return get(this,'IDBDatabase').name;}
        get version(){const r=get(this,'IDBDatabase');return r.upgrade?get(r.upgrade,'IDBTransaction').upgradeVersion:r.version;}
        get objectStoreNames(){const r=get(this,'IDBDatabase');return names(r.upgrade?get(r.upgrade,'IDBTransaction').scope:r.stores);}
        close(){get(this,'IDBDatabase').closed=true;closing(this);}
        transaction(scope,mode='readonly',options={}){const r=get(this,'IDBDatabase');if(r.closed||r.upgrade)fail('InvalidStateError');scope=typeof scope==='string'?[scope]:Array.from(scope,strings);scope=[...new Set(scope)];if(!scope.length)fail('InvalidAccessError');for(const name of scope)if(!r.stores.includes(name))fail('NotFoundError');mode=strings(mode);if(mode!=='readonly'&&mode!=='readwrite')throw new TypeError('Invalid transaction mode');if(options.durability!==undefined&&!['default','strict','relaxed'].includes(options.durability))throw new TypeError('Invalid durability');return new IDBTransaction(token,this,scope,mode,options);}
        createObjectStore(name,options={}){const r=get(this,'IDBDatabase');if(!r.upgrade)fail('InvalidStateError');const tx=live(r.upgrade,true);name=strings(name);if(tx.data.stores.some(s=>s.name===name))fail('ConstraintError');const p=keyPath(options.keyPath===undefined?null:options.keyPath);if(options.autoIncrement&&(p===''||Array.isArray(p)))fail('InvalidAccessError');tx.data.stores.push({name,keyPath:p,autoIncrement:!!options.autoIncrement,generator:1,records:[],indexes:[]});tx.scope.push(name);return r.upgrade.objectStore(name);}
        deleteObjectStore(name){const r=get(this,'IDBDatabase');if(!r.upgrade)fail('InvalidStateError');const tx=live(r.upgrade,true);name=strings(name);const i=tx.data.stores.findIndex(s=>s.name===name);if(i<0)fail('NotFoundError');tx.data.stores.splice(i,1);tx.scope.splice(tx.scope.indexOf(name),1);}
    }
    function open(name,version,deleting=false){name=strings(name);if(version!==undefined){version=Number(version);if(!Number.isSafeInteger(version)||version<=0)throw new TypeError('Invalid database version');}
        /* "obtain a storage key": failure throws now, before a request exists. */
        if(typeof storageKey==='function'&&!storageKey())throw new DOMException('Storage is not available to this origin','SecurityError');
        const request=new IDBOpenDBRequest(token);let blocked=false;
        async function attempt(){let a;try{a=await native('begin',name);if(retired){if(a[0]===0)discard(a[1]);return;}if(a[0]===1){later(attempt,5);return;}status(a[0]);const data=a[2]===null?{version:0,stores:[]}:unpack(a[2]),wanted=deleting?0:version===undefined?data.version||1:version;
                if(!deleting&&wanted<data.version){await native('abort',a[1]);finish(request,undefined,new DOMException('Requested version is older','VersionError'));return;}
                const list=connections.get(name)||[];
                /* versionchange goes to every other open connection (each realm's poll fires it);
                   blocked waits until all of them were fired, then fires once if any is still
                   open; the open goes on when none is. active: [status, open, not yet fired]. */
                if(deleting||wanted>data.version){const active=await native('connections',a[1],deleting?0:wanted);if(active[0]===1){await native('abort',a[1]);later(attempt,5);return;}status(active[0]);if(active[1]){await native('abort',a[1]);if(!blocked&&!active[2]){blocked=true;versionEvent(request,'blocked',data.version,deleting?null:wanted);}later(attempt,50);return;}}
                if(deleting){status(await native('commit',a[1],pack({version:0,stores:[]}),[],0));const rr=get(request,'IDBRequest');rr.done=true;rr.result=undefined;rr.error=null;versionEvent(request,'success',data.version,null);return;}
                let connection=0;if(wanted===data.version){const c=await native('connect',a[1],wanted);if(c[0]===1){await native('abort',a[1]);later(attempt,5);return;}status(c[0]);connection=c[1];}const db=new IDBDatabase(token,name,data);get(db,'IDBDatabase').connection=connection;list.push(db);connections.set(name,list);watch();
                if(wanted>data.version){const tx=new IDBTransaction(token,db,data.stores.map(s=>s.name),'versionchange'),r=get(tx,'IDBTransaction');r.nativeToken=a[1];r.data=data;r.oldVersion=data.version;r.upgradeVersion=wanted;r.openRequest=request;get(db,'IDBDatabase').upgrade=tx;r.data.version=wanted;activate(tx);const rr=get(request,'IDBRequest');rr.result=db;rr.done=true;rr.transaction=tx;const e=versionEvent(request,'upgradeneeded',r.oldVersion,wanted);if(eventState(e).listenerException)abort(tx,new DOMException('Upgrade listener threw','AbortError'));}
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
