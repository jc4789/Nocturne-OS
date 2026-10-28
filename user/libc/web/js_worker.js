/* Dedicated native child runtime. No timer-driven main-realm emulation. */
const workerBridge=(()=>{
    const native=host.worker, slots=new WeakMap(), live=new Map();
    // encoding-parse relative to the outside settings (this window's document).
    function scriptURL(url){
        const parsed=urlBridge.encodingParse(url,rawDom.get(document,'baseURI'),rawDom.get(document,'characterSet'));
        if(parsed===null)throw new DOMException('Invalid worker script URL','SyntaxError');
        return parsed.href;
    }
    class Worker extends EventTarget {
        constructor(url,options={}){
            super();if(!arguments.length)throw new TypeError('Worker script URL required');if(options==null)options={};if(typeof options!=='object'&&typeof options!=='function')throw new TypeError('Worker options must be a dictionary');
            if(options.type!==undefined&&String(options.type)!=='classic')
                throw new DOMException('Module workers are not supported','NotSupportedError');
            if(options.credentials!==undefined&&String(options.credentials)!=='same-origin')
                throw new DOMException('Worker credentials mode is not supported','NotSupportedError');
            const address=scriptURL(htmlSafetyBridge.check(url,3,'Worker constructor'));
            let id;try{id=native(0,0,address,String(options.name??''));}catch(e){if(String(e?.message||'').startsWith('SecurityError:'))throw new DOMException(e.message,'SecurityError');throw e;}
            slots.set(this,{id,closed:false});live.set(id,new Set([this]));
            this.onmessage=null;this.onmessageerror=null;this.onerror=null;
        }
        postMessage(value,options){
            const s=slots.get(this);if(!s||s.shared)throw new TypeError('Illegal invocation');if(!arguments.length)throw new TypeError('Worker message required');if(s.closed)return;
            const list=Array.isArray(options)?options:options?.transfer;
            const prepared=messagingBridge.prepareWorker(value,cloneData.transferList(list),s.id);
            try{native(3,s.id,prepared.packet,prepared.plan);}catch(error){messagingBridge.abortWorker(prepared);throw error;}
        }
        terminate(){const s=slots.get(this);if(!s||s.shared)throw new TypeError('Illegal invocation');if(s.closed)return;s.closed=true;live.delete(s.id);messagingBridge.closeWorker(s.id);native(2,s.id);}
    }
    class SharedWorker extends EventTarget {
        constructor(url,options={}){
            super();if(!arguments.length)throw new TypeError('SharedWorker script URL required');
            if(typeof options==='string')options={name:options};else if(options==null)options={};
            if(typeof options!=='object'&&typeof options!=='function')throw new TypeError('SharedWorker options must be a name or dictionary');
            if(options.type!==undefined&&String(options.type)!=='classic')throw new DOMException('Module workers are not supported','NotSupportedError');
            if(options.credentials!==undefined&&!['omit','same-origin','include'].includes(String(options.credentials)))throw new TypeError('Invalid worker credentials mode');
            const address=scriptURL(htmlSafetyBridge.check(url,3,'SharedWorker constructor'));
            let id;try{id=native(5,0,address,String(options.name??''));}catch(error){if(String(error?.message||'').startsWith('SecurityError:'))throw new DOMException(error.message,'SecurityError');throw error;}
            const channel=new MessageChannel(),prepared=messagingBridge.prepareWorker(null,[channel.port2],id);
            try{native(10,id,prepared.packet,prepared.plan);}catch(error){messagingBridge.abortWorker(prepared);channel.port1.close();throw error;}
            slots.set(this,{id,closed:false,port:channel.port1,shared:true});
            let owners=live.get(id);if(!owners)live.set(id,owners=new Set());owners.add(this);this.onerror=null;
        }
        get port(){const s=slots.get(this);if(!s?.shared)throw new TypeError('Illegal invocation');return s.port;}
    }
    cloneData.registerUncloneable(value=>slots.has(value));
    Object.defineProperty(Worker.prototype,Symbol.toStringTag,{value:'Worker',configurable:true});
    Object.defineProperty(SharedWorker.prototype,Symbol.toStringTag,{value:'SharedWorker',configurable:true});
    Object.assign(globalThis,{Worker,SharedWorker});
    return {deliver(id,kind,payload,request=0,flags=0){
        const owners=live.get(id);if(!owners)return;
        if(kind===7){host.log(0,String(payload.message||''));return;}
        if(kind===8||kind===9||kind===10){messagingBridge.receiveWorkerPort(id,request,flags,payload,kind!==8);return;}
        if(kind===6){for(const worker of owners)slots.get(worker).closed=true;live.delete(id);messagingBridge.closeWorker(id);return;}
        for(const worker of owners){let event;
        if(kind===2){
            try{const message=flags===3?messagingBridge.importWorker(payload,id):{data:cloneData.deserialize(payload),ports:[]};if(slots.get(worker).closed)return;event=new MessageEvent('message',{data:message.data,ports:message.ports});}
            catch(_){event=new MessageEvent('messageerror');}
        }else if(kind===4){
            // Only a native transport/process failure is terminal. An ordinary
            // author exception must leave its live Worker/ports usable.
            if(payload.terminal===true){slots.get(worker).closed=true;live.delete(id);messagingBridge.closeWorker(id);}
            event=new ErrorEvent('error',{cancelable:true,message:String(payload.message||'Worker failed'),filename:String(payload.filename||''),lineno:payload.lineno||0});
        }
        else return;
        eventState(event).isTrusted=true;
        if(dispatch(worker,event)&&kind===4)host.log(2,'Worker: '+event.message);}
    }};
})();
