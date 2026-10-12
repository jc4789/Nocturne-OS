/* Real FormData entry lists and bounded UTF-8 multipart bodies.
 * Include after js_blob.js/js_crypto.js, before any author script runs.
 * https://xhr.spec.whatwg.org/#interface-formdata
 * https://html.spec.whatwg.org/multipage/form-control-infrastructure.html#multipart/form-data-encoding-algorithm
 * Window constructors collect typed entries through the native form owner;
 * body parsing/serialization remains shared with Worker realms. */
const formDataBridge = (() => {
    'use strict';
    const apply=Reflect.apply, define=Object.defineProperty, create=Object.create;
    const setPrototype=Object.setPrototypeOf, names=Object.getOwnPropertyNames;
    const descriptor=Object.getOwnPropertyDescriptor, get=WeakMap.prototype.get, put=WeakMap.prototype.set;
    const slots=new WeakMap(), iteratorSlots=new WeakMap();
    const eventSlots=new WeakMap(),windowForms=typeof htmlElementBrand==='function'&&typeof rawDom==='object'&&typeof dom==='function';
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
    // QuickJS's actual ArrayBuffer constructor stores an INT32_MAX byte span.
    // This is representability, not a multipart/page quota; allocation may fail.
    const setBytes=U8.prototype.set, LIMIT=0x7fffffff;
    const Decoder=TextDecoder,decode=TextDecoder.prototype.decode;
    const U32=Uint32Array;
    const trim=String.prototype.trim,lower=String.prototype.toLowerCase;
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
            if(form!==undefined){if(!windowForms)throw new TypeErr('HTMLFormElement is not available in this realm');htmlElementBrand(form,'form');}
            if(submitter!=null){
                if(!windowForms)throw new TypeErr('HTMLElement is not available in this realm');
                rawDom.get(submitter,'elementBrand');
                if(rawDom.get(submitter,'namespaceURI')!=='http://www.w3.org/1999/xhtml')throw new TypeErr('Submitter must be an HTMLElement');
            }
            if(form===undefined){apply(put,slots,[this,list()]);return;}
            if(submitter!==null){
                if(!dom('validation',submitter,'submitButton'))throw new TypeErr('Submitter must be a submit button');
                const tag=rawDom.get(submitter,'localName');if(rawDom.get(submitter,'form:'+tag)!==form)throw new DomErr('Submitter belongs to another form','NotFoundError');
            }
            const packet=dom('formValue',form,'entriesBegin',submitter);
            if(packet===null)throw new DomErr('Form entry list construction is already active','InvalidStateError');
            try{
                const collected=list();for(let i=0;i<packet.length;i++){const row=packet[i];let value;
                    if(row[2]===null)value=scalar(row[1]);
                    else if(row[6])value=formControlBridge.file(row[6],row[7]);
                    else {const bits=create(null),options=create(null);options.type=row[3];if(!row[5])options.lastModified=row[4];bits[Symbol.iterator]=()=>{let done=false;const iterator=create(null);iterator.next=()=>done?{done:true}:(done=true,{done:false,value:row[1]});return iterator;};value=new NativeFile(bits,row[2],options);}
                    const item=create(null);item.name=scalar(row[0]);item.value=value;collected[collected.length]=item;
                }
                // The event owns the construction list. The returned object
                // gets its shallow clone only after handlers finish, as HTML
                // requires; retaining event.formData cannot mutate it later.
                const eventData=new FormData();apply(put,slots,[eventData,collected]);
                const event=new NativeFormDataEvent('formdata',{bubbles:true,formData:eventData});eventState(event).isTrusted=true;dispatch(form,event);
                const result=list();for(let i=0;i<collected.length;i++){const item=create(null);item.name=collected[i].name;item.value=collected[i].value;result[result.length]=item;}apply(put,slots,[this,result]);
            }finally{dom('formValue',form,'entriesEnd');}
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
    let NativeFormDataEvent=null;
    if(windowForms){
        const EventImpl=Event;
        NativeFormDataEvent=class FormDataEvent extends EventImpl {
            constructor(type,init){if(arguments.length<2)throw new TypeErr('FormDataEvent requires two arguments');if(typeof type==='symbol')throw new TypeErr('Cannot convert Symbol to DOMString');const eventType=StringImpl(type);if(init==null||typeof init!=='object'&&typeof init!=='function')throw new TypeErr('FormDataEvent requires a dictionary');const settings=create(null);settings.bubbles=!!init.bubbles;settings.cancelable=!!init.cancelable;settings.composed=!!init.composed;const value=init.formData;state(value);super(eventType,settings);apply(put,eventSlots,[this,value]);}
            get formData(){const value=apply(get,eventSlots,[this]);if(value===undefined)throw new TypeErr('Illegal FormDataEvent receiver');return value;}
        };
        define(NativeFormDataEvent.prototype,Symbol.toStringTag,{value:'FormDataEvent',configurable:true});
        const getter=descriptor(NativeFormDataEvent.prototype,'formData');getter.enumerable=true;define(NativeFormDataEvent.prototype,'formData',getter);
        define(globalThis,'FormDataEvent',{value:NativeFormDataEvent,writable:true,configurable:true});
    }

    function quota(){throw new RangeError('Multipart byte length is not representable by ArrayBuffer');}
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

    /* Body.formData consumes the real byte sequence. Binary parts never pass
     * through a JS string, and delimiter search is linear even for hostile
     * repeated prefixes. There is no alternate submission or DOM overlay. */
    function parameters(text){
        const result=create(null);let at=0;
        while(at<text.length&&text[at]!==';')at++;
        result.essence=apply(lower,apply(trim,apply(slice,text,[0,at]),[]),[]);
        result.values=create(null);
        while(at<text.length){
            at++;while(text[at]===' '||text[at]==='\t')at++;
            const start=at;while(at<text.length&&text[at]!=='='&&text[at]!==';')at++;
            const name=apply(lower,apply(trim,apply(slice,text,[start,at]),[]),[]);
            if(text[at]!=='=')continue;at++;
            while(text[at]===' '||text[at]==='\t')at++;
            let value='';
            if(text[at]==='"'){
                at++;let closed=false;
                while(at<text.length){const ch=text[at++];if(ch==='"'){closed=true;break;}
                    if(ch==='\\'&&at<text.length)value+=text[at++];else value+=ch;}
                if(!closed)throw new TypeErr('Unterminated MIME parameter');
                while(at<text.length&&text[at]!==';')at++;
            }else {const begin=at;while(at<text.length&&text[at]!==';')at++;value=apply(trim,apply(slice,text,[begin,at]),[]);}
            if(name&&!(name in result.values))result.values[name]=value;
        }
        return result;
    }
    function finder(pattern){
        const count=apply(byteLength,pattern,[]),failure=new U32(count);
        for(let i=1,j=0;i<count;i++){while(j&&pattern[i]!==pattern[j])j=failure[j-1];if(pattern[i]===pattern[j])j++;failure[i]=j;}
        return (bytes,start,end=apply(byteLength,bytes,[]))=>{
            for(let i=start,j=0;i<end;i++){while(j&&bytes[i]!==pattern[j])j=failure[j-1];if(bytes[i]===pattern[j])j++;if(j===count)return i+1-j;}
            return -1;
        };
    }
    function bodyMime(text){
        /* Headers may combine several Content-Type values. Split only outside
         * quoted parameters, then retain the last syntactically valid MIME. */
        let start=0,quoted=false,escaped=false,result=null;
        const valid=/^[!#$%&'*+.^_`|~0-9a-z-]+\/[!#$%&'*+.^_`|~0-9a-z-]+$/;
        for(let i=0;i<=text.length;i++){
            const ch=text[i];
            if(i<text.length&&quoted){if(escaped)escaped=false;else if(ch==='\\')escaped=true;else if(ch==='"')quoted=false;continue;}
            if(ch==='"'){quoted=true;continue;}
            if(i===text.length||ch===','){
                try{const candidate=parameters(apply(slice,text,[start,i]));if(valid.test(candidate.essence)&&candidate.essence!=='*/*')result=candidate;}catch(_){}
                start=i+1;
            }
        }
        if(!result)throw new TypeErr('Invalid form MIME type');return result;
    }
    function utf8(bytes,start,end){return apply(decode,new Decoder('utf-8',{ignoreBOM:true}),[new U8(apply(buffer,bytes,[]),start,end-start)]);}
    function urlValue(bytes,start,end){
        const output=new U8(end-start);let used=0;
        const hex=c=>c>=48&&c<=57?c-48:c>=65&&c<=70?c-55:c>=97&&c<=102?c-87:-1;
        for(let i=start;i<end;i++){let c=bytes[i];if(c===43)c=32;
            else if(c===37&&i+2<end){const hi=hex(bytes[i+1]),lo=hex(bytes[i+2]);if(hi>=0&&lo>=0){c=hi*16+lo;i+=2;}}
            output[used++]=c;
        }
        return utf8(output,0,used);
    }
    function parseBody(bufferValue,contentType){
        const bytes=new U8(bufferValue),length=apply(byteLength,bytes,[]),mime=bodyMime(contentType),result=new FormData(),entries=state(result);
        function add(name,value){const item=create(null);item.name=name;item.value=value;entries[entries.length]=item;}
        if(mime.essence==='application/x-www-form-urlencoded'){
            for(let start=0;start<length;){let end=start,equal=-1;
                while(end<length&&bytes[end]!==38){if(bytes[end]===61&&equal<0)equal=end;end++;}
                if(end>start)add(urlValue(bytes,start,equal<0?end:equal),urlValue(bytes,equal<0?end:equal+1,end));
                start=end+1;
            }
            return result;
        }
        if(mime.essence!=='multipart/form-data')throw new TypeErr('Body is not form data');
        const boundary=mime.values.boundary;
        if(!boundary||/[\r\n\0]/.test(boundary))throw new TypeErr('Missing or invalid multipart boundary');
        const marker=apply(encode,new Encoder(),['--'+boundary]),delimiter=apply(encode,new Encoder(),['\r\n--'+boundary]);
        const markerSize=apply(byteLength,marker,[]),delimiterSize=apply(byteLength,delimiter,[]);
        const findBoundary=finder(delimiter),findHeaders=finder(new U8([13,10,13,10]));
        function closing(at){if(bytes[at]!==45||bytes[at+1]!==45)return false;let end=at+2;while(bytes[end]===32||bytes[end]===9)end++;
            return end===length||(bytes[end]===13&&bytes[end+1]===10);}
        let at=bytes[0]===13&&bytes[1]===10?2:0;
        if(markerSize>length-at)throw new TypeErr('Missing multipart delimiter');
        let opening=true;for(let i=0;i<markerSize;i++)if(bytes[at+i]!==marker[i]){opening=false;break;}
        if(!opening){const preamble=findBoundary(bytes,0);if(preamble<0)throw new TypeErr('Invalid multipart opening');at=preamble+2;}
        at+=markerSize;
        for(;;){
            if(closing(at))return result;
            if(bytes[at]!==13||bytes[at+1]!==10)throw new TypeErr('Invalid multipart delimiter');
            at+=2;const headerEnd=findHeaders(bytes,at);
            if(headerEnd<0)throw new TypeErr('Incomplete multipart headers');
            const headers=create(null);let line=at;
            while(line<headerEnd){let end=line;while(end<headerEnd&&!(bytes[end]===13&&bytes[end+1]===10))end++;
                let colon=line;while(colon<end&&bytes[colon]!==58)colon++;
                if(colon===end)throw new TypeErr('Invalid multipart header');
                const name=apply(lower,utf8(bytes,line,colon),[]),value=apply(trim,utf8(bytes,colon+1,end),[]);
                if(!/^[!#$%&'*+.^_`|~0-9a-z-]+$/.test(name)||name in headers)throw new TypeErr('Invalid duplicate multipart header');
                headers[name]=value;line=end+2;
            }
            const disposition=parameters(headers['content-disposition']||'');
            if(disposition.essence!=='form-data'||!('name' in disposition.values))throw new TypeErr('Missing form-data name');
            const begin=headerEnd+4;let end=findBoundary(bytes,begin);
            while(end>=0){const after=end+delimiterSize;if((bytes[after]===13&&bytes[after+1]===10)||closing(after))break;end=findBoundary(bytes,end+delimiterSize);}
            if(end<0)throw new TypeErr('Incomplete multipart body');
            let value;
            if('filename' in disposition.values){
                const part=new U8(apply(buffer,bytes,[]),begin,end-begin),bits=create(null),options=create(null);
                bits[Symbol.iterator]=()=>{let done=false;const iterator=create(null);iterator.next=()=>done?{done:true}:(done=true,{done:false,value:part});return iterator;};
                options.type=headers['content-type']||'text/plain';
                value=new NativeFile(bits,disposition.values.filename,options);
            }else value=utf8(bytes,begin,end);
            add(disposition.values.name,value);at=end+delimiterSize;
        }
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
    return {brand:value=>apply(get,slots,[value])!==undefined,eventBrand:value=>apply(get,eventSlots,[value])!==undefined,multipart,parse:parseBody,faceValue};
})();
