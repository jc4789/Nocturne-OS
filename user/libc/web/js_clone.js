/* Structured data for session history. Native class IDs reject proxies/host
 * objects without invoking their traps; engine bytecode is never serialized. */
const cloneData = (() => {
    const apply=Reflect.apply,M=Map,S=Set,A=Array,AB=ArrayBuffer,U8=Uint8Array;
    const Obj=Object,DateType=Date,RegExpType=RegExp,DomError=DOMException,TypeErr=TypeError;
    const keys=Object.keys,descriptor=Object.getOwnPropertyDescriptor,define=Object.defineProperty;
    const own=Object.hasOwn,toString=String,iteratorSymbol=Symbol.iterator;
    const mHas=M.prototype.has,mGet=M.prototype.get,mSet=M.prototype.set;
    const setAdd=S.prototype.add,mapEach=M.prototype.forEach,setEach=S.prototype.forEach;
    const push=A.prototype.push,byteSet=U8.prototype.set;
    const has=(map,key)=>apply(mHas,map,[key]),get=(map,key)=>apply(mGet,map,[key]);
    const put=(map,key,value)=>apply(mSet,map,[key,value]),add=(array,value)=>apply(push,array,[value]);
    const kinds=new M(),constructors=new M(),classID=host.classID,detach=host.detach;
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
    const fail=()=>{throw new DomError('Value cannot be cloned','DataCloneError');};
    const resizable=buffer=>abResizable&&apply(abResizable,buffer,[]);
    function copyBuffer(buffer) {
        // No slice/species or constructor hooks from the source buffer.
        let size,input;
        try{size=apply(abLength,buffer,[]);input=new U8(buffer,0,size);}catch(_){return fail();}
        const output=resizable(buffer)?new AB(size,{maxByteLength:apply(abMaxLength,buffer,[])}):new AB(size);
        apply(byteSet,new U8(output),[input]);return output;
    }
    function serialize(input,transfers) {
        const seen=new M(),records=[];
        // Register placeholders first; capture transferred bytes after user getters.
        if(transfers)for(let i=0;i<transfers.length;i++){
            put(seen,transfers[i],records.length);add(records,['ArrayBuffer',null]);
        }
        function visit(value) {
            if(typeof value==='symbol'||typeof value==='function')return fail();
            if(value===null||typeof value!=='object')return [0,value];
            if(has(seen,value))return [1,get(seen,value)];
            const kind=get(kinds,classID(value));if(!kind)return fail();
            const id=records.length,record=[kind];add(records,record);put(seen,value,id);
            if(kind==='Object'||kind==='Array'){
                record[1]=kind==='Array'?value.length:0;record[2]=[];
                const properties=keys(value);
                for(let i=0;i<properties.length;i++){
                    const key=properties[i];
                    // Enumerable keys were snapshotted; only deletion removes one.
                    if(own(value,key))add(record[2],[key,visit(value[key])]);
                }
            }else if(kind==='Map'||kind==='Set'){
                const entries=[];
                apply(kind==='Map'?mapEach:setEach,value,[(v,k)=>add(entries,kind==='Map'?[k,v]:v)]);
                record[1]=[];
                for(let i=0;i<entries.length;i++)add(record[1],kind==='Map'?[visit(entries[i][0]),visit(entries[i][1])]:visit(entries[i]));
            }else if(kind==='ArrayBuffer')record[1]=copyBuffer(value);
            else if(has(constructors,kind)){
                const dataView=kind==='DataView';let buffer,offset,size;
                try{
                    if(!dataView)apply(typedValues,value,[]);
                    buffer=apply(dataView?viewBuffer:byteBuffer,value,[]);
                    offset=apply(dataView?viewOffset:byteOffset,value,[]);
                    size=apply(dataView?viewLength:length,value,[]);new U8(buffer,0,0);
                }catch(_){return fail();}
                // Public getters cannot distinguish fixed from length-tracking
                // views. Reject instead of silently changing their semantics.
                if(resizable(buffer))return fail();
                record[1]=visit(buffer);record[2]=offset;record[3]=size;
            }else if(kind==='Date')record[1]=apply(date,value,[]);
            else if(kind==='RegExp'){
                record[1]=apply(source,value,[]);record[2]='';
                for(let i=0;i<flagGetters.length;i++)if(flagGetters[i][0]&&apply(flagGetters[i][0],value,[]))record[2]+=flagGetters[i][1];
            }else if(kind==='Error'){
                const name=value.name;record[1]=typeof name==='string'&&own(errors,name)?name:'Error';
                const message=descriptor(value,'message');
                record[2]=message&&'value'in message?toString(message.value):undefined;
                const cause=descriptor(value,'cause');record[3]=!!cause;if(cause)record[4]=visit(value.cause);
                const stack=descriptor(value,'stack');record[5]=stack&&typeof stack.value==='string'?stack.value:undefined;
            }else record[1]=apply(boxed[kind],value,[]);
            return [1,id];
        }
        const root=visit(input);
        if(transfers)for(let i=0;i<transfers.length;i++){
            records[get(seen,transfers[i])][1]=copyBuffer(transfers[i]);detach(transfers[i]);
        }
        return [root,records];
    }
    function deserialize(serialized) {
        const root=serialized[0],records=serialized[1],values=new M();
        function read(ref){
            if(ref[0]===0)return ref[1];const id=ref[1];if(has(values,id))return get(values,id);
            const r=records[id],kind=r[0];let value;
            if(kind==='Object')value={};else if(kind==='Array')value=new A(r[1]);
            else if(kind==='Map')value=new M();else if(kind==='Set')value=new S();
            else if(kind==='ArrayBuffer')value=r[1];
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
    globalThis.structuredClone=function(value,options={}){
        if(!arguments.length)throw new TypeErr('Value required');
        if(options==null)options={};
        if(typeof options!=='object'&&typeof options!=='function')throw new TypeErr('Expected a dictionary');
        const list=options.transfer,transfers=[],unique=new M();
        if(list!==undefined){
            if(list===null||(typeof list!=='object'&&typeof list!=='function'))throw new TypeErr('Expected an iterable sequence');
            const method=list[iteratorSymbol];if(typeof method!=='function')throw new TypeErr('Expected an iterable sequence');
            // Array.from wrongly accepts non-iterable array-likes for Web IDL sequences.
            for(const item of {[iteratorSymbol]:()=>apply(method,list,[])}){
                if(item===null||(typeof item!=='object'&&typeof item!=='function'))throw new TypeErr('Transfer entries must be objects');
                add(transfers,item);
            }
        }
        for(let i=0;i<transfers.length;i++){
            const item=transfers[i];if(classID(item)!==arrayBufferID||has(unique,item))fail();put(unique,item,true);
        }
        return deserialize(serialize(value,transfers));
    };
    return {serialize,deserialize};
})();
