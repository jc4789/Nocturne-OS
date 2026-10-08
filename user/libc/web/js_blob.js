/* 有界で実バイトを所有する File API。OSファイル選択と
 * ReadableStream は未実装。object URL は js_object_url.js が所有する。
 * https://w3c.github.io/FileAPI/ (2026-10-08) */
const blobBridge = (() => {
    'use strict';
    const slots=new WeakMap(), files=new WeakMap(), readers=new WeakMap();
    const apply=Reflect.apply, define=Object.defineProperty, create=Object.create;
    const get=WeakMap.prototype.get, put=WeakMap.prototype.set;
    const U8=Uint8Array, AB=ArrayBuffer, P=Promise, Encoder=TextEncoder, Decoder=TextDecoder,ProgressEventType=ProgressEvent;
    const encode=Encoder.prototype.encode, decode=Decoder.prototype.decode;
    const getter=(p,k)=>Object.getOwnPropertyDescriptor(p,k).get;
    const ta=Object.getPrototypeOf(U8.prototype), length=getter(ta,'byteLength');
    const buffer=getter(ta,'buffer'),offset=getter(ta,'byteOffset'),tag=getter(ta,Symbol.toStringTag);
    const abLength=getter(AB.prototype,'byteLength'),isView=AB.isView;
    const dvBuffer=getter(DataView.prototype,'buffer'),dvOffset=getter(DataView.prototype,'byteOffset');
    const dvLength=getter(DataView.prototype,'byteLength'),set=U8.prototype.set;
    const str=String, wellFormed=String.prototype.toWellFormed, dateNow=Date.now,fromCode=String.fromCharCode,push=Array.prototype.push;
    const LIMIT=16*1024*1024, token={};
    const state=v=>{const s=apply(get,slots,[v]);if(!s)throw new TypeError('Illegal Blob receiver');return s;};
    const dictionary=v=>{if(v==null)return {};if(typeof v!=='object'&&typeof v!=='function')throw new TypeError('Expected dictionary');return v;};
    const string=v=>{if(typeof v==='symbol')throw new TypeError('Cannot convert Symbol to string');return str(v);};
    const mime=v=>{const s=string(v);return /[^\x20-\x7e]/.test(s)?'':s.replace(/[A-Z]/g,c=>c.toLowerCase());};
    function view(value){
        let b=value,o=0,n;
        if(isView(value)){
            const typed=apply(tag,value,[])!==undefined;
            b=apply(typed?buffer:dvBuffer,value,[]);o=apply(typed?offset:dvOffset,value,[]);
            n=apply(typed?length:dvLength,value,[]);
        }
        const size=apply(abLength,b,[]);return new U8(b,o,n===undefined?size:n);
    }
    function copy(bytes){const n=apply(length,bytes,[]),out=new U8(n);apply(set,out,[bytes]);return out;}
    function parts(values,options){
        if(values===null||(typeof values!=='object'&&typeof values!=='function'))throw new TypeError('Expected BlobPart sequence');
        const method=values[Symbol.iterator];if(typeof method!=='function')throw new TypeError('Blob parts must be iterable');
        const chunks=[];let total=0;
        for(const part of {[Symbol.iterator]:()=>apply(method,values,[])}){
            let chunk,existing=apply(get,slots,[part]);
            if(existing)chunk=existing.bytes;
            else if(isView(part))chunk=view(part);
            else {
                let binary=false;
                try{apply(abLength,part,[]);binary=true;}catch(_){}
                if(binary)chunk=view(part);
                else {let text=apply(wellFormed,string(part),[]);if(options.endings==='native')text=text.replace(/\r\n|\r/g,'\n');chunk=apply(encode,new Encoder(),[text]);}
            }
            const n=apply(length,chunk,[]);
            if(n>LIMIT-total)throw new DOMException('Blob exceeds the 16 MiB limit','QuotaExceededError');
            /* 入力iteratorの次の呼出しによる変更/transferより前にsnapshotを取る。 */
            apply(push,chunks,[copy(chunk)]);total+=n;
        }
        const bytes=new U8(total);let at=0;
        for(let i=0;i<chunks.length;i++){const chunk=chunks[i];apply(set,bytes,[chunk,at]);at+=apply(length,chunk,[]);}return bytes;
    }
    function options(value){
        const o=dictionary(value),e=o.endings,endings=e===undefined?'transparent':string(e);
        if(endings!=='transparent'&&endings!=='native')throw new TypeError('Invalid line ending mode');
        const t=o.type;return {endings,type:t===undefined?'':mime(t)};
    }
    function make(bytes,type){if(apply(length,bytes,[])>LIMIT)throw new DOMException('Blob exceeds the 16 MiB limit','QuotaExceededError');const result=create(Blob.prototype);apply(put,slots,[result,{bytes:copy(bytes),type}]);return result;}
    function base64(bytes){
        const alphabet='ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/',n=apply(length,bytes,[]);let out='';
        for(let i=0;i<n;i+=3){const a=bytes[i],b=i+1<n?bytes[i+1]:0,c=i+2<n?bytes[i+2]:0;
            out+=alphabet[a>>2]+alphabet[(a&3)<<4|b>>4]+(i+1<n?alphabet[(b&15)<<2|c>>6]:'=')+(i+2<n?alphabet[c&63]:'=');}return out;
    }
    function integer(v){const n=+v;return Number.isNaN(n)||n===0?0:Number.isFinite(n)?Math.trunc(n):n;}
    function read(blob,kind){
        return new P((resolve,reject)=>{try{
            const bytes=copy(state(blob).bytes);
            resolve(kind==='text'?apply(decode,new Decoder(),[bytes]):kind==='bytes'?bytes:apply(buffer,bytes,[]));
        }catch(e){reject(e);}});
    }
    class Blob {
        constructor(blobParts=[],opts={}){const o=options(opts),bytes=parts(blobParts,o);apply(put,slots,[this,{bytes,type:o.type}]);}
        get size(){return apply(length,state(this).bytes,[]);}
        get type(){return state(this).type;}
        slice(start=0,end=undefined,contentType=''){
            const s=state(this),n=apply(length,s.bytes,[]),a=integer(start),b=end===undefined?n:integer(end),type=mime(contentType);
            const first=a<0?Math.max(n+a,0):Math.min(a,n),last=b<0?Math.max(n+b,0):Math.min(b,n);
            const count=Math.max(last-first,0),bytes=new U8(count);
            for(let i=0;i<count;i++)bytes[i]=s.bytes[first+i];return make(bytes,type);
        }
        arrayBuffer(){return read(this,'arrayBuffer');}
        bytes(){return read(this,'bytes');}
        text(){return read(this,'text');}
        stream(){state(this);throw new DOMException('ReadableStream is not implemented','NotSupportedError');}
    }
    class File extends Blob {
        constructor(fileBits,fileName,opts={}){
            if(arguments.length<2)throw new TypeError('File requires parts and a name');
            const name=apply(wellFormed,string(fileName),[]),o=dictionary(opts),e=o.endings;
            const endings=e===undefined?'transparent':string(e),v=o.lastModified;
            let lastModified=v===undefined?dateNow():integer(v);
            if(!Number.isFinite(lastModified))lastModified=0;
            /* Web IDL signed long long conversion; stay exact for realistic dates. */
            lastModified=Number(BigInt(Math.trunc(lastModified))%18446744073709551616n);
            if(lastModified>=9223372036854775808)lastModified-=18446744073709551616;
            if(lastModified<-9223372036854775808)lastModified+=18446744073709551616;
            const t=o.type,type=t===undefined?'':mime(t);
            super(fileBits,{endings,type});apply(put,files,[this,{name,lastModified}]);
        }
        get name(){const s=apply(get,files,[this]);if(!s)throw new TypeError('Illegal File receiver');return s.name;}
        get lastModified(){const s=apply(get,files,[this]);if(!s)throw new TypeError('Illegal File receiver');return s.lastModified;}
        get webkitRelativePath(){if(!apply(get,files,[this]))throw new TypeError('Illegal File receiver');return '';}
    }
    const readerState=v=>{const s=apply(get,readers,[v]);if(!s)throw new TypeError('Illegal FileReader receiver');return s;};
    function fire(reader,type,total=0){const event=new ProgressEventType(type,{lengthComputable:true,loaded:type==='loadstart'?0:total,total});event.isTrusted=true;dispatch(reader,event);}
    function start(reader,blob,kind,encoding){
        const s=readerState(reader),b=state(blob);
        if(s.ready===1)throw new DOMException('Reader is already loading','InvalidStateError');
        const charset=encoding===undefined?undefined:string(encoding),epoch=++s.epoch;
        s.ready=1;s.result=null;s.error=null;
        try{s.timer=host.timer(0,()=>{
            if(s.epoch!==epoch||s.ready!==1)return;s.timer=0;
            const n=apply(length,b.bytes,[]);fire(reader,'loadstart',n);
            if(s.epoch!==epoch||s.ready!==1)return;
            try{
                const bytes=copy(b.bytes);let result;
                if(kind==='arrayBuffer')result=apply(buffer,bytes,[]);
                else if(kind==='text'){
                    const parameter=/;\s*charset\s*=\s*(?:"([^"]*)"|([^;\s]*))/i.exec(b.type);
                    let decoder;try{decoder=new Decoder(charset??(parameter?(parameter[1]??parameter[2]):'utf-8'));}catch(_){decoder=new Decoder('utf-8');}
                    result=apply(decode,decoder,[bytes]);
                }else {
                    if(kind==='dataURL')result='data:'+(b.type||'application/octet-stream')+';base64,'+base64(bytes);
                    else {result='';for(let i=0;i<n;i++)result+=fromCode(bytes[i]);}
                }
                fire(reader,'progress',n);if(s.epoch!==epoch||s.ready!==1)return;
                s.result=result;s.ready=2;
                fire(reader,'load',n);
            }catch(e){if(s.epoch!==epoch)return;s.ready=2;s.result=null;s.error=e;fire(reader,'error',n);}
            if(s.epoch===epoch&&s.ready!==1)fire(reader,'loadend',n);
        },0,[]);}catch(e){s.ready=2;s.error=e;throw e;}
    }
    class FileReader extends EventTarget {
        constructor(){super();apply(put,readers,[this,{ready:0,result:null,error:null,timer:0,epoch:0}]);}
        get readyState(){return readerState(this).ready;}
        get result(){return readerState(this).result;}
        get error(){return readerState(this).error;}
        readAsArrayBuffer(blob){if(!arguments.length)throw new TypeError('Blob required');start(this,blob,'arrayBuffer');}
        readAsText(blob,encoding){if(!arguments.length)throw new TypeError('Blob required');start(this,blob,'text',encoding);}
        readAsDataURL(blob){if(!arguments.length)throw new TypeError('Blob required');start(this,blob,'dataURL');}
        readAsBinaryString(blob){if(!arguments.length)throw new TypeError('Blob required');start(this,blob,'binary');}
        abort(){const s=readerState(this);s.result=null;if(s.ready!==1)return;
            if(s.timer)host.clear(s.timer);s.timer=0;s.ready=2;const epoch=++s.epoch;fire(this,'abort');if(s.epoch===epoch&&s.ready!==1)fire(this,'loadend');}
    }
    for(const [key,value] of [['EMPTY',0],['LOADING',1],['DONE',2]])for(const target of [FileReader,FileReader.prototype])define(target,key,{value,enumerable:true});
    const readerEvents=new Set(['loadstart','progress','load','error','abort','loadend']);
    blobHandlerTarget=(value,type)=>apply(get,readers,[value])!==undefined&&readerEvents.has(type);
    installHandlers(FileReader.prototype,readerEvents);
    for(const C of [Blob,File,FileReader]){
        define(C.prototype,Symbol.toStringTag,{value:C.name,configurable:true});
        for(const key of Object.getOwnPropertyNames(C.prototype))if(key!=='constructor'){const d=Object.getOwnPropertyDescriptor(C.prototype,key);d.enumerable=true;define(C.prototype,key,d);}
    }
    Object.assign(globalThis,{Blob,File,FileReader});
    return {brand:value=>apply(get,slots,[value])!==undefined,readerBrand:value=>apply(get,readers,[value])!==undefined,
        bytes:value=>apply(buffer,copy(state(value).bytes),[]),type:value=>state(value).type,
        fromBytes:(bytes,type)=>make(view(bytes),mime(type)),
        snapshot(value){const s=state(value),f=apply(get,files,[value]);return [apply(buffer,copy(s.bytes),[]),s.type,f?f.name:null,f?f.lastModified:0];},
        restore(data){return data[2]===null?make(view(data[0]),data[1]):new File([data[0]],data[2],{type:data[1],lastModified:data[3]});}};
})();
