/* Dedicated native child runtime. No timer-driven main-realm emulation. */
const workerBridge=(()=>{
    const native=host.worker, slots=new WeakMap(), live=new Map();
    class Worker extends EventTarget {
        constructor(url,options={}){
            super(); if(options==null)options={};
            if(options.type!==undefined&&String(options.type)!=='classic')
                throw new DOMException('Module workers are not supported','NotSupportedError');
            if(options.credentials!==undefined&&String(options.credentials)!=='same-origin')
                throw new DOMException('Worker credentials mode is not supported','NotSupportedError');
            const address=new URL(String(url),document.baseURI).href;
            let id;try{id=native(0,0,address,String(options.name??''));}catch(e){if(String(e?.message||'').startsWith('SecurityError:'))throw new DOMException(e.message,'SecurityError');throw e;}
            slots.set(this,{id,closed:false});live.set(id,this);
            this.onmessage=null;this.onmessageerror=null;this.onerror=null;
        }
        postMessage(value,options){
            const s=slots.get(this);if(!s)throw new TypeError('Illegal invocation');if(s.closed)return;
            const list=Array.isArray(options)?options:options?.transfer;
            const packet=cloneData.prepare(value,cloneData.transferList(list),true);
            if(packet.data[1].some(record=>record[0]==='Blob'))throw new DOMException('Blob cloning across Worker realms is not supported','DataCloneError');
            native(1,s.id,packet.data);cloneData.commit(packet);
        }
        terminate(){const s=slots.get(this);if(!s)throw new TypeError('Illegal invocation');if(s.closed)return;s.closed=true;live.delete(s.id);native(2,s.id);}
    }
    cloneData.registerUncloneable(value=>slots.has(value));
    Object.defineProperty(Worker.prototype,Symbol.toStringTag,{value:'Worker',configurable:true});
    globalThis.Worker=Worker;
    return {deliver(id,kind,payload){
        const worker=live.get(id);if(!worker)return;
        if(kind===7){host.log(0,String(payload.message||''));return;}
        let event;
        if(kind===2){
            try{event=new MessageEvent('message',{data:cloneData.deserialize(payload)});}
            catch(_){event=new MessageEvent('messageerror');}
        }else if(kind===4)event=new ErrorEvent('error',{cancelable:true,message:String(payload.message||'Worker failed'),filename:String(payload.filename||''),lineno:payload.lineno||0});
        else if(kind===6){slots.get(worker).closed=true;live.delete(id);return;}else return;
        event.isTrusted=true;
        if(dispatch(worker,event)&&kind===4)host.log(2,'Worker: '+event.message);
    }};
})();
