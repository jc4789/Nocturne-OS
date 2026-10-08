/* WHATWG Streams algorithms from pinned MIT web-streams-polyfill. No native
 * OS ABI, disk, network, or host capability is exposed through these classes.
 * Fetch remains a buffered transport; its real response bytes are consumed via
 * this implementation with locks, backpressure, cancellation and BYOB. */
const streamBridge=(()=>{
    const types=(()=>{
        /* @streams-ponyfill */
    })();
    const {ReadableStream,WritableStream,TransformStream}=types;
    const U8=Uint8Array,AB=ArrayBuffer,P=Promise,apply=Reflect.apply;
    const getter=(p,k)=>Object.getOwnPropertyDescriptor(p,k).get;
    const ta=Object.getPrototypeOf(U8.prototype),tag=getter(ta,Symbol.toStringTag);
    const length=getter(ta,'byteLength'),buffer=getter(ta,'buffer');
    const set=U8.prototype.set,locked=getter(ReadableStream.prototype,'locked');
    const getReader=ReadableStream.prototype.getReader,tee=ReadableStream.prototype.tee;
    const read=types.ReadableStreamDefaultReader.prototype.read;
    const release=types.ReadableStreamDefaultReader.prototype.releaseLock;
    const cancel=types.ReadableStreamDefaultReader.prototype.cancel;
    const encoder=TextEncoder,decoder=TextDecoder,encode=encoder.prototype.encode,decode=decoder.prototype.decode;
    const LIMIT=16*1024*1024;
    function brand(value){try{apply(locked,value,[]);return true;}catch(_){return false;}}
    function disturbed(value){return !!value._disturbed;}
    function fromBytes(bytes,check=()=>{}){
        let source=new U8(bytes),at=0;
        return new ReadableStream({type:'bytes',pull(controller){
            try{check();const count=Math.min(65536,source.length-at);
                if(!count){controller.close();return;}
                const chunk=new U8(count);for(let i=0;i<count;i++)chunk[i]=source[at+i];
                at+=count;controller.enqueue(chunk);if(at===source.length){source=null;controller.close();}
            }catch(e){source=null;controller.error(e);}
        },cancel(){source=null;}},{highWaterMark:0});
    }
    async function collect(stream){
        const reader=apply(getReader,stream,[]),chunks=[];let total=0;
        try{for(;;){const item=await apply(read,reader,[]);if(item.done)break;
            if(apply(tag,item.value,[])!=='Uint8Array')throw new TypeError('Body stream must contain Uint8Array chunks');
            const n=apply(length,item.value,[]);
            if(n>LIMIT-total)throw new DOMException('Body exceeds the 16 MiB limit','QuotaExceededError');
            const copy=new U8(n);apply(set,copy,[item.value]);chunks.push(copy);total+=n;
        }}catch(e){try{await apply(cancel,reader,[e]);}catch(_){}throw e;}
        finally{apply(release,reader,[]);}
        const bytes=new U8(total);let at=0;for(const chunk of chunks){apply(set,bytes,[chunk,at]);at+=apply(length,chunk,[]);}return apply(buffer,bytes,[]);
    }
    class TextEncoderStream {
        constructor(){let pending='';const enc=new encoder();
            const transform=new TransformStream({transform(chunk,c){
                let text=pending+String(chunk);pending='';
                if(text.length&&/[\uD800-\uDBFF]/.test(text[text.length-1])){pending=text[text.length-1];text=text.slice(0,-1);}
                if(text)c.enqueue(apply(encode,enc,[text]));
            },flush(c){if(pending)c.enqueue(apply(encode,enc,[pending]));}});
            Object.defineProperties(this,{readable:{value:transform.readable,enumerable:true},writable:{value:transform.writable,enumerable:true}});
        }
        get encoding(){return 'utf-8';}
    }
    class TextDecoderStream {
        constructor(label='utf-8',options={}){const dec=new decoder(label,options);
            const transform=new TransformStream({transform(chunk,c){const text=apply(decode,dec,[chunk,{stream:true}]);if(text)c.enqueue(text);},flush(c){const text=apply(decode,dec,[]);if(text)c.enqueue(text);}});
            for(const key of ['encoding','fatal','ignoreBOM'])Object.defineProperty(this,key,{value:dec[key],enumerable:true});
            Object.defineProperties(this,{readable:{value:transform.readable,enumerable:true},writable:{value:transform.writable,enumerable:true}});
        }
    }
    Object.assign(globalThis,types,{TextEncoderStream,TextDecoderStream});
    return Object.freeze({brand,disturbed,fromBytes,collect,locked:v=>apply(locked,v,[]),tee:v=>apply(tee,v,[])});
})();
