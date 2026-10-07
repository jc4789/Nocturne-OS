/* Same-realm channels use Nocturne's native posted-task queue. No timer slots,
 * Promise polling, worker emulation, or public page-controlled clone hooks. */
(() => {
    const apply=Reflect.apply,create=Object.create,define=Object.defineProperty,freeze=Object.freeze;
    const WM=WeakMap,wmGet=WM.prototype.get,wmSet=WM.prototype.set,wmHas=WM.prototype.has;
    const ports=new WM(),channels=new WM(),events=new WM();
    const get=(map,key)=>apply(wmGet,map,[key]),put=(map,key,v)=>apply(wmSet,map,[key,v]);
    const has=(map,key)=>apply(wmHas,map,[key]);
    const TypeErr=TypeError,DomError=DOMException,URLType=URL;
    const originGet=Object.getOwnPropertyDescriptor(URLType.prototype,'origin').get;
    const postTask=host.postTask,cancelPost=host.cancelPost,nativeURL=host.url,string=elementURL.string;
    let originURL,originValue;
    const origin=()=>{
        const url=nativeURL();
        if(url!==originURL){
            const value=apply(originGet,new URLType(url),[]);
            originURL=url;originValue=value;
        }
        return originValue;
    };
    const iteratorSymbol=Symbol.iterator,eventInitMethod=Event.prototype.initEvent;
    const portBrand=p=>has(ports,p);
    const fail=()=>{throw new DomError('Value cannot be transferred','DataCloneError');};
    function check(p){const s=get(ports,p);if(!s)throw new TypeErr('Illegal MessagePort receiver');return s;}
    function eventCheck(e){const s=get(events,e);if(!s)throw new TypeErr('Illegal MessageEvent receiver');return s;}
    function eventInit(init){
        if(init==null)init={};
        if(typeof init!=='object'&&typeof init!=='function')throw new TypeErr('Expected a dictionary');
        const d=init.data,data=d===undefined?null:d;
        const lid=init.lastEventId,id=lid===undefined?'':string(lid);
        const og=init.origin,o=og===undefined?'':string(og);
        const ps=cloneData.transferList(init.ports);
        for(let i=0;i<ps.length;i++)if(!portBrand(ps[i]))throw new TypeErr('Expected MessagePort');
        const source=init.source??null;
        if(source!==null&&source!==globalThis&&!portBrand(source))throw new TypeErr('Invalid message source');
        return {data,origin:o,lastEventId:id,source,ports:freeze(ps)};
    }
    class MessageEvent extends Event {
        constructor(type,init={}){
            if(!arguments.length)throw new TypeErr('MessageEvent requires a type');
            super(string(type),init??{});put(events,this,eventInit(init));
        }
        get data(){return eventCheck(this).data;}
        get origin(){return eventCheck(this).origin;}
        get lastEventId(){return eventCheck(this).lastEventId;}
        get source(){return eventCheck(this).source;}
        get ports(){return eventCheck(this).ports;}
        initMessageEvent(type,bubbles=false,cancelable=false,data=null,origin='',lastEventId='',source=null,ports=[]){
            eventCheck(this);if(!arguments.length)throw new TypeErr('MessageEvent requires a type');
            const t=string(type),init=eventInit({data,origin,lastEventId,source,ports});
            if(this._dispatching)return;
            apply(eventInitMethod,this,[t,bubbles,cancelable]);put(events,this,init);
        }
    }
    function makePort(endpoint){
        const value=create(MessagePort.prototype),s={value,endpoint,detached:false,enabled:false};
        put(ports,value,s);return s;
    }
    function deliver(target,packet,source=null,senderOrigin=''){
        let event;
        try{
            const payload=cloneData.deserialize(packet);
            event=new MessageEvent('message',{data:payload.data,ports:payload.ports,source,origin:senderOrigin});
        }catch(_){event=new MessageEvent('messageerror',{source,origin:senderOrigin});}
        event.isTrusted=true;dispatch(target,event);
    }
    function schedule(endpoint,reserve=false){
        if(endpoint.scheduled||(!endpoint.head&&!reserve)||!endpoint.owner.enabled)return;
        endpoint.scheduled=postTask(()=>{
            endpoint.scheduled=0;
            // A transfer moves the queue and disables the new port until start.
            if(!endpoint.owner.enabled||!endpoint.head)return;
            const packet=endpoint.head;endpoint.head=packet.next;
            if(!endpoint.head)endpoint.tail=null;
            endpoint.count--;
            // A full native queue must not discard the already-popped message.
            // Each native callback ends with its own microtask checkpoint.
            try{deliver(endpoint.owner.value,packet.data);}finally{schedule(endpoint);}
        });
    }
    function enable(s){s.enabled=true;if(s.endpoint)schedule(s.endpoint);}
    function packet(message,transfers){
        const transferred=[];
        for(let i=0;i<transfers.length;i++)if(portBrand(transfers[i]))transferred[transferred.length]=transfers[i];
        return cloneData.serialize({data:message,ports:transferred},transfers);
    }
    function listOrOptions(options){
        if(options==null)return [];
        if(typeof options!=='object'&&typeof options!=='function')throw new TypeErr('Expected transfer sequence or options');
        const iterator=options[iteratorSymbol];
        return cloneData.transferList(iterator!=null?{[iteratorSymbol]:()=>apply(iterator,options,[])}:options.transfer);
    }
    class MessagePort extends EventTarget {
        constructor(){super();throw new TypeErr('Illegal constructor');}
        postMessage(message,options={}){
            const s=check(this);if(!arguments.length)throw new TypeErr('Message required');
            const target=s.endpoint?s.endpoint.peer:null,transfers=listOrOptions(options);
            let doomed=false;
            for(let i=0;i<transfers.length;i++){
                if(transfers[i]===this)fail();
                if(target&&transfers[i]===target.owner.value)doomed=true;
            }
            if(target&&target.count>=4096)throw new DomError('Port message queue quota reached','QuotaExceededError');
            const p={data:null,next:null};
            // Reserve native scheduling capacity before transfer commits. A
            // getter can post reentrantly; never cancel its queued message.
            if(target&&!doomed)schedule(target,true);
            try{p.data=packet(message,transfers);}catch(error){
                if(target&&target.scheduled&&!target.head){cancelPost(target.scheduled);target.scheduled=0;}
                throw error;
            }
            if(!target||doomed)return;
            if(target.tail)target.tail.next=p;else target.head=p;
            target.tail=p;target.count++;schedule(target);
        }
        start(){enable(check(this));}
        close(){
            const s=check(this);s.detached=true;
            if(s.endpoint&&s.endpoint.peer){s.endpoint.peer.peer=null;s.endpoint.peer=null;}
        }
        get onmessage(){check(this);return handlerValue(this,'message');}
        set onmessage(v){const s=check(this);setHandler(this,'message',v,null);enable(s);}
        get onmessageerror(){check(this);return handlerValue(this,'messageerror');}
        set onmessageerror(v){check(this);setHandler(this,'messageerror',v,null);}
    }
    class MessageChannel {
        constructor(){
            const a={peer:null,owner:null,head:null,tail:null,count:0,scheduled:false};
            const b={peer:a,owner:null,head:null,tail:null,count:0,scheduled:false};
            a.peer=b;a.owner=makePort(a);b.owner=makePort(b);
            put(channels,this,[a.owner.value,b.owner.value]);
        }
        get port1(){const c=get(channels,this);if(!c)throw new TypeErr('Illegal MessageChannel receiver');return c[0];}
        get port2(){const c=get(channels,this);if(!c)throw new TypeErr('Illegal MessageChannel receiver');return c[1];}
    }
    cloneData.registerTransfer({
        brand:portBrand,
        validate(p){if(check(p).detached)fail();},
        prepare(p){const s=check(p);return {value:makePort(s.endpoint).value};},
        commit(p,prepared){
            const s=check(p),receiver=check(prepared.value);
            s.detached=true;s.endpoint=null;receiver.endpoint.owner=receiver;
        }
    });
    cloneData.registerUncloneable(v=>has(channels,v)||has(events,v));
    messageHandlerTarget=(target,type)=>portBrand(target)&&(type==='message'||type==='messageerror');
    function postMessage(message,targetOrOptions={},transfer){
        if(this!==undefined&&this!==null&&this!==globalThis)throw new TypeErr('Illegal Window receiver');
        if(!arguments.length)throw new TypeErr('Message required');
        let target='/',list;
        if(arguments.length>=3){target=string(targetOrOptions);list=transfer;}
        else if(targetOrOptions!==null&&(typeof targetOrOptions==='object'||typeof targetOrOptions==='function')){
            const o=targetOrOptions.targetOrigin;target=o===undefined?'/':string(o);list=targetOrOptions.transfer;
        }else if(targetOrOptions!==undefined&&targetOrOptions!==null){target=string(targetOrOptions);list=transfer;}
        const senderOrigin=origin();let expected=target;
        if(target==='/')expected=senderOrigin;
        else if(target!=='*'){
            try{expected=apply(originGet,new URLType(target),[]);}
            catch(_){throw new DomError('Invalid target origin','SyntaxError');}
        }
        const transfers=cloneData.transferList(list);let data;
        const reserved=postTask(()=>{
            // Opaque origins only match the same Window through the '/' default,
            // never an arbitrary URL whose serialized origin is also 'null'.
            if(target!=='*'&&(expected!==origin()||(expected==='null'&&target!=='/')))return;
            deliver(globalThis,data,globalThis,senderOrigin);
        });
        try{data=packet(message,transfers);}catch(error){cancelPost(reserved);throw error;}
    }
    for(const [C,name] of [[MessagePort,'MessagePort'],[MessageChannel,'MessageChannel'],[MessageEvent,'MessageEvent']])
        define(C.prototype,Symbol.toStringTag,{value:name,configurable:true});
    Object.assign(globalThis,{MessageChannel,MessagePort,MessageEvent,postMessage});
})();
