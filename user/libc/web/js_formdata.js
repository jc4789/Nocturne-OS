/* Real FormData entry lists and bounded UTF-8 multipart bodies.
 * Include after js_blob.js/js_crypto.js, before any author script runs.
 * https://xhr.spec.whatwg.org/#interface-formdata
 * https://html.spec.whatwg.org/multipage/form-control-infrastructure.html#multipart/form-data-encoding-algorithm
 * Native successful-control collection is not an entry-list bridge yet:
 * FormData(form) fails explicitly, rather than submitting an incomplete list. */
const formDataBridge = (() => {
    'use strict';
    const apply=Reflect.apply, define=Object.defineProperty, create=Object.create;
    const setPrototype=Object.setPrototypeOf, names=Object.getOwnPropertyNames;
    const descriptor=Object.getOwnPropertyDescriptor, get=WeakMap.prototype.get, put=WeakMap.prototype.set;
    const slots=new WeakMap(), iteratorSlots=new WeakMap();
    const TypeErr=TypeError, DomErr=DOMException, StringImpl=String;
    const wellFormed=String.prototype.toWellFormed, slice=String.prototype.slice;
    const code=String.prototype.charCodeAt, Encoder=TextEncoder, encode=TextEncoder.prototype.encode;
    const NativeFile=File, fileName=descriptor(File.prototype,'name').get;
    const fileModified=descriptor(File.prototype,'lastModified').get;
    const blobSize=descriptor(Blob.prototype,'size').get, blobBrand=blobBridge.brand;
    const blobBytes=blobBridge.bytes, blobType=blobBridge.type;
    const cryptoImpl=globalThis.crypto, random=cryptoImpl.getRandomValues;
    const U8=Uint8Array, typed=Object.getPrototypeOf(U8.prototype);
    const buffer=descriptor(typed,'buffer').get, byteLength=descriptor(typed,'byteLength').get;
    const setBytes=U8.prototype.set, LIMIT=16*1024*1024;
    const iteratorProto=create(Object.getPrototypeOf(Object.getPrototypeOf([][Symbol.iterator]())));
    function list(){return setPrototype([],null);}
    function state(value){const s=apply(get,slots,[value]);if(!s)throw new TypeErr('Illegal FormData receiver');return s;}
    function scalar(value){if(typeof value==='symbol')throw new TypeErr('Cannot convert Symbol to string');return apply(wellFormed,StringImpl(value),[]);}
    function required(count,minimum){if(count<minimum)throw new TypeErr('Not enough FormData arguments');}
    function entry(name,value,filename,count){
        name=scalar(name);
        const binary=blobBrand(value);
        // With three arguments, Web IDL selects the Blob overload even when
        // the optional filename is undefined (which means missing).
        if(count>=3&&!binary)throw new TypeErr('A filename requires a Blob');
        if(!binary)value=scalar(value);
        else {
            const supplied=count>=3&&filename!==undefined;
            if(supplied)filename=scalar(filename);
            let isFile=false;
            try{apply(fileName,value,[]);isFile=true;}catch(_){}
            if(!isFile||supplied){
                const original=value,options=create(null);
                options.type=blobType(original);
                if(isFile)options.lastModified=apply(fileModified,original,[]);
                // An author-modified Array iterator must not replace the
                // immutable Blob bytes while creating the File entry.
                const bits=create(null);
                bits[Symbol.iterator]=()=>{let done=false;const iterator=create(null);
                    iterator.next=()=>done?{value:undefined,done:true}:(done=true,{value:original,done:false});return iterator;};
                value=new NativeFile(bits,supplied?filename:'blob',options);
            }
        }
        const result=create(null);result.name=name;result.value=value;return result;
    }
    function remove(entries,name,replacement){
        let at=0,found=false;
        for(let i=0;i<entries.length;i++){
            const item=entries[i];
            if(item.name===name){if(replacement&&!found)entries[at++]=replacement;found=true;}
            else entries[at++]=item;
        }
        entries.length=at;
        if(replacement&&!found)entries[entries.length]=replacement;
    }
    function iterator(value,kind){
        const entries=state(value),object=create(iteratorProto),s=create(null);
        s.entries=entries;s.index=0;s.kind=kind;s.finished=false;apply(put,iteratorSlots,[object,s]);return object;
    }
    define(iteratorProto,'next',{value:function next(){
        const s=apply(get,iteratorSlots,[this]);if(!s)throw new TypeErr('Illegal FormData iterator receiver');
        if(s.finished||s.index>=s.entries.length){s.finished=true;return {value:undefined,done:true};}
        const item=s.entries[s.index++];
        return {value:s.kind===0?[item.name,item.value]:s.kind===1?item.name:item.value,done:false};
    },writable:true,enumerable:true,configurable:true});
    define(iteratorProto,Symbol.toStringTag,{value:'FormData Iterator',configurable:true});
    class FormData {
        constructor(form=undefined,submitter=null){
            if(form!==undefined)htmlElementBrand(form,'form');
            if(submitter!=null){
                rawDom('get',submitter,'elementBrand');
                if(rawDom('get',submitter,'namespaceURI')!=='http://www.w3.org/1999/xhtml')throw new TypeErr('Submitter must be an HTMLElement');
            }
            if(form!==undefined)throw new DomErr('Native FormData(form) entry collection is not implemented','NotSupportedError');
            apply(put,slots,[this,list()]);
        }
        append(name,value,filename=undefined){const entries=state(this);required(arguments.length,2);const item=entry(name,value,filename,arguments.length);entries[entries.length]=item;}
        set(name,value,filename=undefined){const entries=state(this);required(arguments.length,2);const item=entry(name,value,filename,arguments.length);remove(entries,item.name,item);}
        delete(name){const entries=state(this);required(arguments.length,1);remove(entries,scalar(name),null);}
        get(name){const entries=state(this);required(arguments.length,1);name=scalar(name);for(let i=0;i<entries.length;i++)if(entries[i].name===name)return entries[i].value;return null;}
        getAll(name){const entries=state(this);required(arguments.length,1);name=scalar(name);const result=[];for(let i=0;i<entries.length;i++)if(entries[i].name===name)define(result,result.length,{value:entries[i].value,writable:true,enumerable:true,configurable:true});return result;}
        has(name){const entries=state(this);required(arguments.length,1);name=scalar(name);for(let i=0;i<entries.length;i++)if(entries[i].name===name)return true;return false;}
        entries(){return iterator(this,0);}
        keys(){return iterator(this,1);}
        values(){return iterator(this,2);}
        forEach(callback,thisArg){const entries=state(this);if(typeof callback!=='function')throw new TypeErr('FormData callback must be callable');for(let i=0;i<entries.length;i++){const item=entries[i];apply(callback,thisArg,[item.value,item.name,this]);}}
    }
    define(FormData.prototype,Symbol.iterator,{value:FormData.prototype.entries,writable:true,configurable:true});
    define(FormData.prototype,Symbol.toStringTag,{value:'FormData',configurable:true});
    for(const key of names(FormData.prototype))if(key!=='constructor'){const d=descriptor(FormData.prototype,key);d.enumerable=true;define(FormData.prototype,key,d);}
    define(globalThis,'FormData',{value:FormData,writable:true,configurable:true});

    function quota(){throw new DomErr('Multipart body exceeds the 16 MiB limit','QuotaExceededError');}
    // Count the transformed UTF-8 bytes BEFORE creating normalized strings or
    // encoding buffers. Lone surrogates have already become U+FFFD at entry creation.
    function textSize(text,normalize,escape,available){
        if(text.length>available)quota();
        let n=0;
        for(let i=0;i<text.length;i++){
            const c=apply(code,text,[i]);
            if(normalize&&(c===13||c===10)){
                if(c===13&&apply(code,text,[i+1])===10)i++;
                n+=escape?6:2;
            }else if(escape&&(c===13||c===10||c===34))n+=3;
            else if(c<128)n++;
            else if(c<2048)n+=2;
            else if(c>=0xd800&&c<=0xdbff){n+=4;i++;}
            else n+=3;
            if(n>available)quota();
        }
        return n;
    }
    function textBytes(text,normalize,escape){
        // Do not dispatch through page-replaceable RegExp @@replace/exec.
        let transformed='',start=0;
        for(let i=0;i<text.length;i++){
            const c=apply(code,text,[i]);let replacement;
            if(normalize&&(c===13||c===10))replacement=escape?'%0D%0A':'\r\n';
            else if(escape&&(c===13||c===10||c===34))replacement=c===34?'%22':c===13?'%0D':'%0A';
            else continue;
            transformed+=apply(slice,text,[start,i])+replacement;
            if(normalize&&c===13&&apply(code,text,[i+1])===10)i++;
            start=i+1;
        }
        return apply(encode,new Encoder(),[transformed+apply(slice,text,[start])]);
    }
    function multipart(value){
        const entries=state(value),entropy=new U8(16);apply(random,cryptoImpl,[entropy]);
        let boundary='----NocturneFormBoundary';const hex='0123456789abcdef';
        for(let i=0;i<16;i++)boundary+=hex[entropy[i]>>4]+hex[entropy[i]&15];
        const plans=list(),ending='--'+boundary+'--\r\n';let total=ending.length;
        for(let i=0;i<entries.length;i++){
            const item=entries[i],p=create(null);p.entry=item;p.file=typeof item.value!=='string';
            const prefix='--'+boundary+'\r\nContent-Disposition: form-data; name="';
            let headSize=prefix.length+textSize(item.name,true,true,LIMIT-total-prefix.length)+1;
            if(p.file){
                p.filename=apply(fileName,item.value,[]);p.type=blobType(item.value)||'application/octet-stream';
                headSize+='; filename="'.length+textSize(p.filename,false,true,LIMIT-total-headSize-'; filename="'.length)+1;
                headSize+='\r\nContent-Type: '.length+p.type.length;
                p.valueSize=apply(blobSize,item.value,[]);
            }else p.valueSize=textSize(item.value,true,false,LIMIT-total-headSize-4-2);
            p.headSize=headSize+4;
            const size=p.headSize+p.valueSize+2;if(size>LIMIT-total)quota();total+=size;plans[plans.length]=p;
        }
        const bytes=new U8(total);let offset=0;
        function write(chunk){apply(setBytes,bytes,[chunk,offset]);offset+=apply(byteLength,chunk,[]);}
        function plain(text){write(apply(encode,new Encoder(),[text]));}
        for(let i=0;i<plans.length;i++){
            const p=plans[i],item=p.entry;
            plain('--'+boundary+'\r\nContent-Disposition: form-data; name="');
            write(textBytes(item.name,true,true));plain('"');
            if(p.file){plain('; filename="');write(textBytes(p.filename,false,true));plain('"\r\nContent-Type: '+p.type);}
            plain('\r\n\r\n');
            write(p.file?new U8(blobBytes(item.value)):textBytes(item.value,true,false));plain('\r\n');
        }
        plain(ending);
        if(offset!==total)throw new TypeErr('Multipart byte count mismatch');
        return {bytes:apply(buffer,bytes,[]),type:'multipart/form-data; boundary='+boundary};
    }

    function faceValue(value){
        if(value===null)return null;
        const source=apply(get,slots,[value]),single=source===undefined,out=list();
        function add(name,v){
            const p=list();p[0]=name;
            let filename,modified,file=false;
            if(blobBrand(v))try{filename=apply(fileName,v,[]);modified=apply(fileModified,v,[]);file=true;}catch(_){}
            if(file){
                p[1]=blobBytes(v);p[2]=filename;p[3]=blobType(v);p[4]=modified;
            }else{p[1]=scalar(v);p[2]=null;p[3]='';p[4]=0;}
            out[out.length]=p;
        }
        if(single)add('',value);else for(let i=0;i<source.length;i++)add(source[i].name,source[i].value);
        const packet=list();packet[0]=single;packet[1]=out;return packet;
    }
    return {brand:value=>apply(get,slots,[value])!==undefined,multipart,faceValue};
})();
