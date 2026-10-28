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
    /* Streams' GenericTransformStream mixin for the interface Type: readable and
       writable are attributes on its prototype that read the object's transform,
       the TransformStream its constructor set up; the getters check that the
       object is a Type. Returns how a constructor attaches its transform. */
    const define=Object.defineProperty,wmGet=WeakMap.prototype.get,wmSet=WeakMap.prototype.set;
    const transformReadable=getter(TransformStream.prototype,'readable'),transformWritable=getter(TransformStream.prototype,'writable');
    function genericTransformStream(Type){
        const transforms=new WeakMap();
        for(const key of ['readable','writable']){
            const get=Object.getOwnPropertyDescriptor({get [key](){
                const transform=apply(wmGet,transforms,[this]);
                if(!transform)throw new TypeError('Illegal invocation');
                return transform[key];
            }},key).get;
            define(Type.prototype,key,{get,set:undefined,enumerable:true,configurable:true});
        }
        return (object,transform)=>apply(wmSet,transforms,[object,{__proto__:null,
            readable:apply(transformReadable,transform,[]),writable:apply(transformWritable,transform,[])}]);
    }
    // TextEncoderCommon and TextDecoderCommon are prototype attributes over each
    // stream's slot (its interface, and a TextDecoderStream's decoder).
    const textStreams=new WeakMap();
    const textStream=(value,Type)=>{const s=apply(wmGet,textStreams,[value]);if(!s||s.type!==Type)throw new TypeError('Illegal invocation');return s;};
    const encodingOf=getter(decoder.prototype,'encoding'),fatalOf=getter(decoder.prototype,'fatal'),ignoreBOMOf=getter(decoder.prototype,'ignoreBOM');
    const codeUnit=String.prototype.charCodeAt,sliceText=String.prototype.slice;
    class TextEncoderStream {
        constructor(){let pending='';const enc=new encoder();
            // A leading surrogate at a chunk's end waits for the next chunk.
            const transform=new TransformStream({transform(chunk,c){
                let text=pending+`${chunk}`;pending='';
                const last=text.length?apply(codeUnit,text,[text.length-1]):0;
                if(last>=0xd800&&last<=0xdbff){pending=apply(sliceText,text,[-1]);text=apply(sliceText,text,[0,-1]);}
                if(text)c.enqueue(apply(encode,enc,[text]));
            },flush(c){if(pending)c.enqueue(apply(encode,enc,[pending]));}});
            apply(wmSet,textStreams,[this,{__proto__:null,type:TextEncoderStream}]);
            textEncoderTransform(this,transform);
        }
        get encoding(){textStream(this,TextEncoderStream);return 'utf-8';}
    }
    const textEncoderTransform=genericTransformStream(TextEncoderStream);
    class TextDecoderStream {
        constructor(label='utf-8',options={}){const dec=new decoder(label,options);
            // A chunk is an AllowSharedBufferSource; undefined is not one.
            const transform=new TransformStream({transform(chunk,c){if(chunk===undefined)throw new TypeError('Expected a BufferSource');const text=apply(decode,dec,[chunk,{stream:true}]);if(text)c.enqueue(text);},flush(c){const text=apply(decode,dec,[]);if(text)c.enqueue(text);}});
            apply(wmSet,textStreams,[this,{__proto__:null,type:TextDecoderStream,decoder:dec}]);
            textDecoderTransform(this,transform);
        }
        get encoding(){return apply(encodingOf,textStream(this,TextDecoderStream).decoder,[]);}
        get fatal(){return apply(fatalOf,textStream(this,TextDecoderStream).decoder,[]);}
        get ignoreBOM(){return apply(ignoreBOMOf,textStream(this,TextDecoderStream).decoder,[]);}
    }
    const textDecoderTransform=genericTransformStream(TextDecoderStream);
    for(const Type of [TextEncoderStream,TextDecoderStream]){
        define(Type.prototype,Symbol.toStringTag,{value:Type.name,configurable:true});
        for(const key of ['encoding','fatal','ignoreBOM'])if(Object.getOwnPropertyDescriptor(Type.prototype,key))define(Type.prototype,key,{enumerable:true});
    }
    /* The Compression Standard's CompressionStream and DecompressionStream. The
       compression context is native (js_compression.h); the transform is set up
       as Streams' "set up" does it, with no transformer or strategy object a page
       could reach through Object.prototype. */
    const compressionNew=host.compressionNew,compressionFeed=host.compressionFeed,compressionEnd=host.compressionEnd;
    const COMPRESSION_FORMATS=['brotli','deflate','deflate-raw','gzip'],indexOf=Array.prototype.indexOf;
    const transformEnqueue=types.TransformStreamDefaultController.prototype.enqueue;
    const isView=AB.isView,taOffset=getter(ta,'byteOffset');
    const dvBuffer=getter(DataView.prototype,'buffer'),dvOffset=getter(DataView.prototype,'byteOffset'),dvLength=getter(DataView.prototype,'byteLength');
    const abLength=getter(AB.prototype,'byteLength'),abResizable=Object.getOwnPropertyDescriptor(AB.prototype,'resizable');
    // Web IDL's enumeration conversion: ToString (which may throw), then a value of the enum.
    function compressionFormat(value){
        const name=`${value}`,index=apply(indexOf,COMPRESSION_FORMATS,[name]);
        if(index<0)throw new TypeError(`'${name}' is not a supported compression format`);
        return index;
    }
    // Web IDL's BufferSource: an ArrayBuffer or a view, not shared and not resizable.
    function bufferSource(value){
        let source=value,offset=0,size,total;
        if(isView(value)){
            const typed=apply(tag,value,[])!==undefined;
            source=apply(typed?buffer:dvBuffer,value,[]);
            offset=apply(typed?taOffset:dvOffset,value,[]);
            size=apply(typed?length:dvLength,value,[]);
        }
        try{total=apply(abLength,source,[]);}catch(_){throw new TypeError('The chunk is not an ArrayBuffer or an ArrayBuffer view');}
        if(abResizable&&apply(abResizable.get,source,[]))throw new TypeError('The chunk is on a resizable ArrayBuffer');
        return [source,offset,size===undefined?total:size];
    }
    function compressionTransform(object,format,decompress,attach){
        const context=compressionNew(format,decompress);
        /* Runs the context over buffer[offset, offset + length) in its bounded
           steps, then enqueues the output. A fault throws before anything of the
           chunk is enqueued; data after the end of the compressed data throws
           after the output (the standard's "decompress and enqueue a chunk"). */
        function run(controller,source,offset,length,finish){
            const output=[];let pieces=0,status,trailing=false;
            for(;;){
                const result=compressionFeed(context,source,offset,length,finish);
                status=result[0];
                if(status<0)throw new TypeError(decompress?'The compressed data is not valid':'The data could not be compressed');
                for(let i=2;i<result.length;i++)define(output,pieces++,{value:result[i],writable:true,enumerable:true,configurable:true});
                if(status!==2){trailing=status===1&&result[1]<length;break;}
                offset+=result[1];length-=result[1];
            }
            for(let i=0;i<pieces;i++)apply(transformEnqueue,controller,[new U8(output[i])]);
            if(trailing)throw new TypeError('Data follows the end of the compressed data');
            return status;
        }
        const transform=new TransformStream({__proto__:null,
            transform(chunk,controller){
                const bytes=bufferSource(chunk);
                if(bytes[2])run(controller,bytes[0],bytes[1],bytes[2],false);
            },
            flush(controller){
                if(run(controller,null,0,0,true)!==1)throw new TypeError('The compressed data ended early');
            },
            cancel(){compressionEnd(context);}
        },{__proto__:null},{__proto__:null});
        attach(object,transform);
    }
    class CompressionStream {
        constructor(format){
            if(!arguments.length)throw new TypeError('CompressionStream requires a format');
            compressionTransform(this,compressionFormat(format),false,compressionStreamTransform);
        }
    }
    const compressionStreamTransform=genericTransformStream(CompressionStream);
    class DecompressionStream {
        constructor(format){
            if(!arguments.length)throw new TypeError('DecompressionStream requires a format');
            compressionTransform(this,compressionFormat(format),true,decompressionStreamTransform);
        }
    }
    const decompressionStreamTransform=genericTransformStream(DecompressionStream);
    for(const Type of [CompressionStream,DecompressionStream]){
        define(Type.prototype,Symbol.toStringTag,{value:Type.name,writable:false,enumerable:false,configurable:true});
        define(globalThis,Type.name,{value:Type,writable:true,enumerable:false,configurable:true}); // [Exposed=*]
    }
    Object.assign(globalThis,types,{TextEncoderStream,TextDecoderStream});
    return Object.freeze({brand,disturbed,fromBytes,network,collect,locked:v=>apply(locked,v,[]),tee:v=>apply(tee,v,[])});
})();
