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
            const address=new URL(htmlSafetyBridge.check(url,3,'Worker constructor'),document.baseURI).href;
            let id;try{id=native(0,0,address,String(options.name??''));}catch(e){if(String(e?.message||'').startsWith('SecurityError:'))throw new DOMException(e.message,'SecurityError');throw e;}
            slots.set(this,{id,closed:false});live.set(id,this);
            this.onmessage=null;this.onmessageerror=null;this.onerror=null;
        }
        postMessage(value,options){
            const s=slots.get(this);if(!s)throw new TypeError('Illegal invocation');if(s.closed)return;
            const list=Array.isArray(options)?options:options?.transfer;
            const prepared=messagingBridge.prepareWorker(value,cloneData.transferList(list),s.id);
            try{native(3,s.id,prepared.packet,prepared.plan);}catch(error){messagingBridge.abortWorker(prepared);throw error;}
        }
        terminate(){const s=slots.get(this);if(!s)throw new TypeError('Illegal invocation');if(s.closed)return;s.closed=true;live.delete(s.id);messagingBridge.closeWorker(s.id);native(2,s.id);}
    }
    cloneData.registerUncloneable(value=>slots.has(value));
    Object.defineProperty(Worker.prototype,Symbol.toStringTag,{value:'Worker',configurable:true});
    globalThis.Worker=Worker;
    return {deliver(id,kind,payload,request=0,flags=0){
        const worker=live.get(id);if(!worker)return;
        if(kind===7){host.log(0,String(payload.message||''));return;}
        if(kind===8||kind===9||kind===10){messagingBridge.receiveWorkerPort(id,request,flags,payload,kind!==8);return;}
        let event;
        if(kind===2){
            try{const message=flags===3?messagingBridge.importWorker(payload,id):{data:cloneData.deserialize(payload),ports:[]};if(slots.get(worker).closed)return;event=new MessageEvent('message',{data:message.data,ports:message.ports});}
            catch(_){event=new MessageEvent('messageerror');}
        }else if(kind===4){
            // Only a native transport/process failure is terminal. An ordinary
            // author exception must leave its live Worker/ports usable.
            if(payload.terminal===true){slots.get(worker).closed=true;live.delete(id);messagingBridge.closeWorker(id);}
            event=new ErrorEvent('error',{cancelable:true,message:String(payload.message||'Worker failed'),filename:String(payload.filename||''),lineno:payload.lineno||0});
        }
        else if(kind===6){slots.get(worker).closed=true;live.delete(id);messagingBridge.closeWorker(id);return;}else return;
        event.isTrusted=true;
        if(dispatch(worker,event)&&kind===4)host.log(2,'Worker: '+event.message);
    }};
})();
