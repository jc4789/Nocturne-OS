/* Structured data for session history. Native class IDs reject proxies/host
 * objects without invoking their traps; engine bytecode is never serialized. */
const cloneData = (() => {
    const apply=Reflect.apply,M=Map,S=Set,A=Array,AB=ArrayBuffer,U8=Uint8Array;
    const Obj=Object,DateType=Date,RegExpType=RegExp,DomError=DOMException,TypeErr=TypeError;
    const keys=Object.keys,descriptor=Object.getOwnPropertyDescriptor,define=Object.defineProperty;
    const own=Object.hasOwn,toString=String,iteratorSymbol=Symbol.iterator;
    const mHas=M.prototype.has,mGet=M.prototype.get,mSet=M.prototype.set;
    const setAdd=S.prototype.add,mapEach=M.prototype.forEach,setEach=S.prototype.forEach;
    const byteSet=U8.prototype.set;
    const has=(map,key)=>apply(mHas,map,[key]),get=(map,key)=>apply(mGet,map,[key]);
    const put=(map,key,value)=>apply(mSet,map,[key,value]);
    // Internal slots must not run author Array.prototype index setters while
    // assembling a transaction, especially after its final validation pass.
    const add=(array,value)=>define(array,array.length,{value,writable:true,enumerable:true,configurable:true});
    const field=(record,key,value)=>define(record,key,{value,writable:true,enumerable:true,configurable:true});
    const kinds=new M(),constructors=new M(),classID=host.classID,detach=host.detach,uncloneable=[],uncloneableNames=[],transferTypes=[];
    const workerDiagnostics=typeof host.frame!=='function'&&typeof host.transferCommit==='function';
    const exceptionBridge=domExceptionBridge;
    const blobSnapshotBridge=typeof blobBridge==='undefined'?null:blobBridge;
    const handlerName=handler=>{const d=descriptor(handler,'name');return d&&'value'in d&&typeof d.value==='string'?d.value:'Transferable';};
    const diagnosticKinds=new M();
    if(workerDiagnostics){
        put(diagnosticKinds,classID(Promise.resolve()),'Promise');put(diagnosticKinds,classID(new WeakMap()),'WeakMap');
        put(diagnosticKinds,classID(new WeakSet()),'WeakSet');put(diagnosticKinds,classID(new Proxy({},{})),'Proxy');
    }
    const register=(name,sample,ctor)=>{put(kinds,classID(sample),name);if(ctor)put(constructors,name,ctor);};
    register('Object',{});register('Array',[]);register('Date',new DateType());
    register('RegExp',/a/);register('Map',new M());register('Set',new S());
    register('ArrayBuffer',new AB(0));register('DataView',new DataView(new AB(0)),DataView);
    register('Boolean',new Boolean());register('Number',new Number());register('String',new String());
    register('BigInt',Obj(0n));register('Error',new Error());
    for(const ctor of [Int8Array,U8,Uint8ClampedArray,Int16Array,Uint16Array,Int32Array,Uint32Array,
        Float32Array,Float64Array,BigInt64Array,BigUint64Array])register(ctor.name,new ctor(0),ctor);
    if(typeof Float16Array==='function')register('Float16Array',new Float16Array(0),Float16Array);
    const arrayBufferID=classID(new AB(0)),typed=Object.getPrototypeOf(U8.prototype);
    const getter=(proto,key)=>descriptor(proto,key).get;
    const abLength=getter(AB.prototype,'byteLength'),abResizable=descriptor(AB.prototype,'resizable')?.get;
    const abMaxLength=descriptor(AB.prototype,'maxByteLength')?.get;
    const byteBuffer=getter(typed,'buffer'),byteOffset=getter(typed,'byteOffset'),length=getter(typed,'length');
    const typedValues=typed.values;
    const viewBuffer=getter(DataView.prototype,'buffer'),viewOffset=getter(DataView.prototype,'byteOffset');
    const viewLength=getter(DataView.prototype,'byteLength');
    const date=DateType.prototype.getTime,source=getter(RegExpType.prototype,'source');
    const flagGetters=['hasIndices','global','ignoreCase','multiline','dotAll','unicode','unicodeSets','sticky']
        .map((key,i)=>[descriptor(RegExpType.prototype,key)?.get,'dgimsuvy'[i]]);
    const boxed={Boolean:Boolean.prototype.valueOf,Number:Number.prototype.valueOf,String:String.prototype.valueOf,BigInt:BigInt.prototype.valueOf};
    const errors={Error,EvalError,RangeError,ReferenceError,SyntaxError,TypeError,URIError};
    const fail=(stage='unsupported',value,count=0,external=false,brand)=>{
        // These labels come only from private built-in registration, never an
        // author constructor/name/toString/getter or a property/URL/value.
        let message='Value cannot be cloned';
        if(workerDiagnostics){const id=classID(value),exception=exceptionBridge&&exceptionBridge.brand(value);
            const kind=brand||(exception?'DOMException':get(kinds,id)||get(diagnosticKinds,id)||typeof value);
            message+=' [stage '+stage+', brand '+kind+', nativeclass '+id+', transfers '+count+', external '+(external?'yes':'no')+']';}
        throw new DomError(message,'DataCloneError');
    };
    const resizable=buffer=>abResizable&&apply(abResizable,buffer,[]);
    function copyBuffer(buffer,reject=fail) {
        // No slice/species or constructor hooks from the source buffer.
        let size,input;
        try{size=apply(abLength,buffer,[]);input=new U8(buffer,0,size);}catch(_){return reject('detached-buffer',buffer);}
        const output=resizable(buffer)?new AB(size,{maxByteLength:apply(abMaxLength,buffer,[])}):new AB(size);
        apply(byteSet,new U8(output),[input]);return output;
    }
    const packets=new WeakMap(),packetGet=WeakMap.prototype.get,packetSet=WeakMap.prototype.set;
    function prepare(input,transfers,external=false,workerContext=null) {
        const seen=new M(),records=[],handlers=[],prepared=[];
        const reject=(stage,value,brand)=>fail(stage,value,transfers?transfers.length:0,external,brand);
        // Register placeholders first; capture transferred bytes after user getters.
        if(transfers)for(let i=0;i<transfers.length;i++){
            const item=transfers[i];if(has(seen,item))reject('duplicate-transfer',item);
            let handler=null;
            if(classID(item)!==arrayBufferID){
                for(let j=0;j<transferTypes.length;j++)if(transferTypes[j].brand(item)){handler=transferTypes[j];break;}
                if(!handler)reject('invalid-transfer-kind',item);handler.validate(item);
                // External factories are private, same-runtime endpoint
                // capabilities; they are never part of the serialized graph.
                // Workers use boolean external=true and must keep rejecting
                // ports: only the private Window/endpoint path opts in.
                if(external&&(external==='worker'?!handler.prepareWorker:(external!=='ports'||!handler.prepareExternal)))reject('external-transfer-unsupported',item,handlerName(handler));
            }else {try{new U8(item,0,0);}catch(_){reject('detached-transfer-buffer',item);}}
            add(handlers,handler);put(seen,item,records.length);add(records,[handler?'Transferred':'ArrayBuffer',null]);
        }
        function visit(value) {
            if(typeof value==='symbol'||typeof value==='function')return reject('non-cloneable-primitive',value);
            if(value===null||typeof value!=='object')return [0,value];
            // JS-backed Web IDL objects share QuickJS's ordinary Object class
            // ID. Their private brands reject them here, during this same walk,
            // before ordinary properties/getters are read (no second traversal).
            if(has(seen,value))return [1,get(seen,value)];
            for(let i=0;i<uncloneable.length;i++)if(apply(uncloneable[i],undefined,[value]))return reject('non-serializable-brand',value,uncloneableNames[i]);
            if(exceptionBridge&&exceptionBridge.brand(value)){
                const id=records.length;put(seen,value,id);add(records,['DOMException',exceptionBridge.snapshot(value)]);return [1,id];
            }
            if(blobSnapshotBridge){
                if(blobSnapshotBridge.readerBrand(value))return reject('non-serializable-brand',value,'FileReader');
                if(blobSnapshotBridge.brand(value)){
                    if(external==='worker')return reject('worker-blob-unsupported',value,'Blob');
                    const id=records.length;put(seen,value,id);add(records,['Blob',blobSnapshotBridge.snapshot(value)]);return [1,id];
                }
            }
            const kind=get(kinds,classID(value));if(!kind)return reject('unknown-native-class',value);
            const id=records.length,record=[kind];add(records,record);put(seen,value,id);
            if(kind==='Object'||kind==='Array'){
                field(record,1,kind==='Array'?value.length:0);field(record,2,[]);
                const properties=keys(value);
                for(let i=0;i<properties.length;i++){
                    const key=properties[i];
                    // Enumerable keys were snapshotted; only deletion removes one.
                    if(own(value,key))add(record[2],[key,visit(value[key])]);
                }
            }else if(kind==='Map'||kind==='Set'){
                const entries=[];
                apply(kind==='Map'?mapEach:setEach,value,[(v,k)=>add(entries,kind==='Map'?[k,v]:v)]);
                field(record,1,[]);
                for(let i=0;i<entries.length;i++)add(record[1],kind==='Map'?[visit(entries[i][0]),visit(entries[i][1])]:visit(entries[i]));
            }else if(kind==='ArrayBuffer')field(record,1,copyBuffer(value,reject));
            else if(has(constructors,kind)){
                const dataView=kind==='DataView';let buffer,offset,size;
                try{
                    if(!dataView)apply(typedValues,value,[]);
                    buffer=apply(dataView?viewBuffer:byteBuffer,value,[]);
                    offset=apply(dataView?viewOffset:byteOffset,value,[]);
                    size=apply(dataView?viewLength:length,value,[]);new U8(buffer,0,0);
                }catch(_){return reject('invalid-view-state',value);}
                // Public getters cannot distinguish fixed from length-tracking
                // views. Reject instead of silently changing their semantics.
                if(resizable(buffer))return reject('resizable-view-unsupported',value);
                field(record,1,visit(buffer));field(record,2,offset);field(record,3,size);
            }else if(kind==='Date')field(record,1,apply(date,value,[]));
            else if(kind==='RegExp'){
                field(record,1,apply(source,value,[]));field(record,2,'');
                for(let i=0;i<flagGetters.length;i++)if(flagGetters[i][0]&&apply(flagGetters[i][0],value,[]))record[2]+=flagGetters[i][1];
            }else if(kind==='Error'){
                const name=value.name;field(record,1,typeof name==='string'&&own(errors,name)?name:'Error');
                const message=descriptor(value,'message');
                field(record,2,message&&'value'in message?toString(message.value):undefined);
                const cause=descriptor(value,'cause');field(record,3,!!cause);if(cause)field(record,4,visit(value.cause));
                const stack=descriptor(value,'stack');field(record,5,stack&&typeof stack.value==='string'?stack.value:undefined);
            }else field(record,1,apply(boxed[kind],value,[]));
            return [1,id];
        }
        const root=visit(input);
        // Complete every fallible allocation/validation before detaching any
        // source. A getter may itself have transferred an earlier list member.
        if(transfers){
            for(let i=0;i<transfers.length;i++){
                const handler=handlers[i];
                if(handler){handler.validate(transfers[i]);add(prepared,external==='worker'?handler.prepareWorker(transfers[i],workerContext):external?handler.prepareExternal(transfers[i]):handler.prepare(transfers[i]));}
                else add(prepared,copyBuffer(transfers[i],reject));
            }
            for(let i=0;i<transfers.length;i++){
                const handler=handlers[i];
                const record=records[get(seen,transfers[i])];
                if(external&&handler){record[0]='ExternalTransferred';field(record,1,i);}
                else field(record,1,prepared[i]);
            }
        }
        const tokens=[];
        if(external)for(let i=0;i<prepared.length;i++)add(tokens,handlers[i]?prepared[i]:null);
        const packet={data:[root,records],tokens:external?tokens:null};
        apply(packetSet,packets,[packet,{transfers,handlers,prepared,external,committed:false}]);
        return packet;
    }
    function validate(packet) {
        const p=apply(packetGet,packets,[packet]);
        if(!p||p.committed)throw new TypeErr('Invalid structured clone commit');
        // A complete late pass precedes the first irreversible detach. This
        // also catches reentrant author getters that transferred another item.
        if(p.transfers)for(let i=0;i<p.transfers.length;i++){
            if(p.handlers[i])p.handlers[i].validate(p.transfers[i]);
            else try{new U8(p.transfers[i],0,0);}catch(_){fail('late-detached-buffer',p.transfers[i],p.transfers.length,p.external);}
        }
        return p;
    }
    function commit(packet,receivers,validated=false) {
        const p=validated?apply(packetGet,packets,[packet]):validate(packet);
        if(!p.transfers||!p.transfers.length){p.committed=true;return;}
        if(typeof host.frame==='function'){
            const plan=commitPlan(packet,receivers);define(plan,'generations',{value:[host.frame('transferGeneration')],writable:true,enumerable:true,configurable:true});
            return host.frame('transferCommit',plan);
        }
        if(typeof host.transferCommit==='function')return host.transferCommit(commitPlan(packet,receivers));
        // A worker has no same-runtime DOM endpoint bridge. Its boolean
        // external preparation already rejects every MessagePort transfer.
        p.committed=true;
        if(p.transfers)for(let i=0;i<p.transfers.length;i++){
            const handler=p.handlers[i];
            if(handler)handler.commit(p.transfers[i],p.external?receivers[i]:p.prepared[i]);else detach(p.transfers[i]);
        }
    }
    function commitPlan(packet,receivers){
        const p=apply(packetGet,packets,[packet]);
        if(!p||p.committed)throw new TypeErr('Invalid structured clone commit');
        const plan={buffers:[],writes:[],cancels:[]};
        if(p.transfers)for(let i=0;i<p.transfers.length;i++){
            const handler=p.handlers[i];
            if(handler){
                const port=handler.commitPlan(p.transfers[i],p.external?receivers[i]:p.prepared[i]);
                for(let j=0;j<port.writes.length;j++)add(plan.writes,port.writes[j]);
                if(port.cancel)add(plan.cancels,port.cancel);
            }else add(plan.buffers,p.transfers[i]);
        }
        add(plan.writes,{object:p,key:'committed',value:true});return plan;
    }
    function serialize(input,transfers) {
        const packet=prepare(input,transfers);commit(packet);return packet.data;
    }
    function deserialize(serialized,receivers=null,copyBytes=false) {
        const root=serialized[0],records=serialized[1],values=new M();
        function read(ref){
            if(ref[0]===0)return ref[1];const id=ref[1];if(has(values,id))return get(values,id);
            const r=records[id],kind=r[0];let value;
            if(kind==='Object')value={};else if(kind==='Array')value=new A(r[1]);
            else if(kind==='Map')value=new M();else if(kind==='Set')value=new S();
            else if(kind==='ArrayBuffer')value=copyBytes?copyBuffer(r[1]):r[1];
            else if(kind==='Transferred')value=r[1].value;
            else if(kind==='ExternalTransferred')value=receivers[r[1]].value;
            else if(kind==='Blob'){
                if(!blobSnapshotBridge)return fail('target-brand-unavailable',undefined,0,false,'Blob');
                value=blobSnapshotBridge.restore(r[1]);
            }
            else if(kind==='DOMException')value=new DomError(r[1][1],r[1][0]);
            else if(has(constructors,kind))value=new(get(constructors,kind))(read(r[1]),r[2],r[3]);
            else if(kind==='Date')value=new DateType(r[1]);else if(kind==='RegExp')value=new RegExpType(r[1],r[2]);
            else if(kind==='Error')value=new errors[r[1]](r[2]);else value=Obj(r[1]);
            put(values,id,value);
            if(kind==='Object'||kind==='Array')for(let i=0;i<r[2].length;i++){
                const pair=r[2][i];define(value,pair[0],{value:read(pair[1]),writable:true,enumerable:true,configurable:true});
            }else if(kind==='Map')for(let i=0;i<r[1].length;i++)put(value,read(r[1][i][0]),read(r[1][i][1]));
            else if(kind==='Set')for(let i=0;i<r[1].length;i++)apply(setAdd,value,[read(r[1][i])]);
            else if(kind==='Error'){
                if(r[3])define(value,'cause',{value:read(r[4]),writable:true,configurable:true});
                if(r[5]!==undefined)define(value,'stack',{value:r[5],writable:true,configurable:true});
            }
            return value;
        }
        return read(root);
    }
    function transferList(list){
        const transfers=[];
        if(list!==undefined){
            if(list===null||(typeof list!=='object'&&typeof list!=='function'))throw new TypeErr('Expected an iterable sequence');
            const method=list[iteratorSymbol];if(typeof method!=='function')throw new TypeErr('Expected an iterable sequence');
            // Array.from wrongly accepts non-iterable array-likes for Web IDL sequences.
            for(const item of {[iteratorSymbol]:()=>apply(method,list,[])}){
                if(item===null||(typeof item!=='object'&&typeof item!=='function'))throw new TypeErr('Transfer entries must be objects');
                add(transfers,item);
            }
        }
        return transfers;
    }
    globalThis.structuredClone=function(value,options={}){
        if(!arguments.length)throw new TypeErr('Value required');
        if(options==null)options={};
        if(typeof options!=='object'&&typeof options!=='function')throw new TypeErr('Expected a dictionary');
        const transfers=transferList(options.transfer);
        const packet=prepare(value,transfers),result=deserialize(packet.data);
        // Even same-realm target graph allocation must precede source detach.
        commit(packet);return result;
    };
    return {serialize,prepare,validate,commit,commitPlan,deserialize,transferList,registerTransfer(handler){
        add(transferTypes,handler);add(uncloneable,handler.brand);add(uncloneableNames,handlerName(handler));
    },registerUncloneable(test,name='Web object'){
        if(typeof test!=='function')throw new TypeErr('Expected a private brand predicate');
        add(uncloneable,test);add(uncloneableNames,name);
    }};
})();
