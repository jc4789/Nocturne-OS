/* WHATWG Streams algorithms from pinned MIT web-streams-polyfill. No native
 * OS ABI, disk, network, or host capability is exposed through these classes.
 * Fetch's native response frames feed these controllers on demand, preserving
 * locks, backpressure, cancellation and BYOB. */
const streamBridge=(()=>{
    // The spec's stream algorithms react to promises with PerformPromiseThen.
    // The ponyfill calls Promise.prototype.then, which looks up @@species
    // through the promise's prototype chain; a page Promise polyfill that
    // re-parents the native prototype (core-js does) would make that return a
    // foreign promise and break every later reaction. Give the ponyfill
    // native promises and a species-free then.
    const streamPromise=(()=>{
        const NativePromise=Promise,resolve=Promise.resolve,reject=Promise.reject,all=Promise.all,react=host.promiseThen;
        function StreamPromise(executor){return new NativePromise(executor);}
        Object.defineProperties(StreamPromise,{
            prototype:{value:Object.freeze({then(onFulfilled,onRejected){return react(this,onFulfilled,onRejected);}})},
            resolve:{value:value=>Reflect.apply(resolve,NativePromise,[value])},
            reject:{value:reason=>Reflect.apply(reject,NativePromise,[reason])},
            all:{value:values=>Reflect.apply(all,NativePromise,[values])}
        });
        return Object.freeze(StreamPromise);
    })();
    const types=(Promise=>{
        /* @streams-ponyfill */
    })(streamPromise);
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
    const enqueue=types.ReadableByteStreamController.prototype.enqueue;
    const close=types.ReadableByteStreamController.prototype.close,error=types.ReadableByteStreamController.prototype.error;
    const encoder=TextEncoder,decoder=TextDecoder,encode=encoder.prototype.encode,decode=decoder.prototype.decode;
    const LIMIT=0xffffffff;
    function brand(value){try{apply(locked,value,[]);return true;}catch(_){return false;}}
    function disturbed(value){return !!value._disturbed;}
    function fromBytes(bytes,check=()=>{}){
        let source=new U8(bytes),at=0;
        return new ReadableStream({type:'bytes',pull(controller){
            try{check();const count=Math.min(65536,source.length-at);
                if(!count){controller.close();return;}
                const chunk=new U8(count);apply(set,chunk,[new U8(apply(buffer,source,[]),at,count)]);
                at+=count;controller.enqueue(chunk);if(at===source.length){source=null;controller.close();}
            }catch(e){source=null;controller.error(e);}
        },cancel(){source=null;}},{highWaterMark:0});
    }
    // Network bodies buffer ahead of the reader like a receive window (the
    // kernel TCP buffer is also 1 MiB): the transport keeps flowing while
    // desiredSize is positive instead of waiting for each read() round trip.
    const NETWORK_BUFFER=1<<20;
    const desiredSize=getter(types.ReadableByteStreamController.prototype,'desiredSize');
    function network(pull,cancelSource){
        let controller,ended=false;
        const stream=new ReadableStream({type:'bytes',start(c){controller=c;},pull(){return pull();},cancel(reason){ended=true;return cancelSource(reason);}},{highWaterMark:NETWORK_BUFFER});
        return Object.freeze({stream,
            enqueue(bytes){if(!ended)apply(enqueue,controller,[new U8(bytes)]);},
            wantsMore(){if(ended)return false;const size=apply(desiredSize,controller,[]);return size!==null&&size>0;},
            close(){if(!ended){ended=true;apply(close,controller,[]);}},
            error(reason){if(!ended){ended=true;apply(error,controller,[reason]);}}
        });
    }
    async function collect(stream){
        const reader=apply(getReader,stream,[]),chunks=[];let total=0;
        try{for(;;){const item=await apply(read,reader,[]);if(item.done)break;
            if(apply(tag,item.value,[])!=='Uint8Array')throw new TypeError('Body stream must contain Uint8Array chunks');
            const n=apply(length,item.value,[]);
            if(n>LIMIT-total)throw new RangeError('Body length is not representable');
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
    return Object.freeze({brand,disturbed,fromBytes,network,collect,locked:v=>apply(locked,v,[]),tee:v=>apply(tee,v,[])});
})();
