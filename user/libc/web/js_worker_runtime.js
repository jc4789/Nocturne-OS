/* Only this private host crosses the native pipe. No window, document, native
 * modules, files, sockets or process functions are installed in this realm. */
(function(host){
    const apply=Reflect.apply;
    delete globalThis.__workerHost;
    delete globalThis.SharedArrayBuffer;delete globalThis.Atomics;
    /* @include js_dom_exception.js */
    const DOMException=domExceptionBridge.DOMException;
    const eventStates=new WeakMap(),listeners=new WeakMap(),eventGet=WeakMap.prototype.get,eventSet=WeakMap.prototype.set;
    const eventDefine=Object.defineProperty,eventMapGet=Map.prototype.get,eventMapSet=Map.prototype.set,eventSlice=Array.prototype.slice;
    const EventMap=Map,EventString=String,eventIndexOf=Array.prototype.indexOf,eventSplice=Array.prototype.splice;
    const eventString=value=>{if(typeof value==='symbol')throw new TypeError('Symbol is not a DOMString');return EventString(value);};
    function eventState(event){const s=apply(eventGet,eventStates,[event]);if(!s)throw new TypeError('Illegal Event receiver');return s;}
    class Event{
        constructor(type,init={}){
            if(!arguments.length)throw new TypeError('Event requires type');type=eventString(type);init=init==null?{}:Object(init);
            apply(eventSet,eventStates,[this,{type,bubbles:!!init.bubbles,cancelable:!!init.cancelable,composed:!!init.composed,
                target:null,currentTarget:null,eventPhase:0,defaultPrevented:false,isTrusted:false,timeStamp:host.now(),
                stop:false,immediate:false,passive:false,dispatching:false,initialized:true,path:[]}]);
            eventDefine(this,'isTrusted',{enumerable:true,get(){return eventState(this).isTrusted;}});
        }
        initEvent(type,bubbles=false,cancelable=false){
            const s=eventState(this);if(!arguments.length)throw new TypeError('initEvent requires type');type=eventString(type);if(s.dispatching)return;
            s.type=type;s.bubbles=!!bubbles;s.cancelable=!!cancelable;s.stop=s.immediate=s.defaultPrevented=false;s.isTrusted=false;s.target=null;s.initialized=true;
        }
        preventDefault(){const s=eventState(this);if(s.cancelable&&!s.passive)s.defaultPrevented=true;}
        stopPropagation(){eventState(this).stop=true;}
        stopImmediatePropagation(){const s=eventState(this);s.stop=s.immediate=true;}
        composedPath(){return apply(eventSlice,eventState(this).path,[]);}
        get cancelBubble(){return eventState(this).stop;}set cancelBubble(value){if(value)eventState(this).stop=true;}
        get returnValue(){return !eventState(this).defaultPrevented;}set returnValue(value){const s=eventState(this);if(!value&&s.cancelable&&!s.passive)s.defaultPrevented=true;}
    }
    for(const key of ['type','target','srcElement','currentTarget','eventPhase','bubbles','cancelable','composed','defaultPrevented','timeStamp'])
        eventDefine(Event.prototype,key,{configurable:true,enumerable:true,get(){return eventState(this)[key==='srcElement'?'target':key];}});
    for(const [key,value]of [['NONE',0],['CAPTURING_PHASE',1],['AT_TARGET',2],['BUBBLING_PHASE',3]]){
        eventDefine(Event,key,{value,enumerable:true});eventDefine(Event.prototype,key,{value,enumerable:true});
    }
    /* @include js_error_event.js */
    function workerListeners(value){const m=apply(eventGet,listeners,[value]);if(!m)throw new TypeError('Illegal EventTarget receiver');return m;}
    function removeWorkerListener(list,entry){if(entry.removed)return;entry.removed=true;const index=apply(eventIndexOf,list,[entry]);if(index>=0)apply(eventSplice,list,[index,1]);if(entry.unlisten){entry.unlisten();entry.unlisten=null;}}
    class EventTarget{
        constructor(){apply(eventSet,listeners,[this,new EventMap()]);}
        addEventListener(type,fn,options={}){
            const m=workerListeners(this);type=eventString(type);if(fn==null)return;
            if(typeof fn!=='function'&&typeof fn!=='object')throw new TypeError('Invalid event listener');
            const capture=typeof options==='boolean'?options:!!options?.capture,once=typeof options==='object'&&!!options?.once,
                passive=typeof options==='object'&&!!options?.passive,signal=typeof options==='object'?options?.signal:null;
            if(signal!=null&&!workerAbortBridge.brand(signal))throw new TypeError('Expected AbortSignal');
            if(signal!=null&&workerAbortBridge.state(signal).aborted)return;
            const list=apply(eventMapGet,m,[type])||[];
            if(list.some(x=>!x.removed&&x.fn===fn&&x.capture===capture))return;
            const entry={fn,capture,once,passive,removed:false,unlisten:null};list.push(entry);apply(eventMapSet,m,[type,list]);
            if(signal!=null)entry.unlisten=workerAbortBridge.subscribe(signal,()=>removeWorkerListener(list,entry));
        }
        removeEventListener(type,fn,options={}){
            const m=workerListeners(this);type=eventString(type);const capture=typeof options==='boolean'?options:!!options?.capture,
                list=apply(eventMapGet,m,[type])||[];
            for(const entry of list)if(!entry.removed&&entry.fn===fn&&entry.capture===capture){removeWorkerListener(list,entry);break;}
        }
        dispatchEvent(event){workerListeners(this);const s=eventState(event);if(!s.initialized||s.dispatching)throw new DOMException('Uninitialized or dispatching event','InvalidStateError');s.isTrusted=false;return dispatchWorkerEvent(this,event);}
    }
    function dispatchWorkerEvent(object,event){
        const m=workerListeners(object),s=eventState(event);if(!s.initialized||s.dispatching)throw new DOMException('Uninitialized or dispatching event','InvalidStateError');
        const receiver=object===target?globalThis:object,type=s.type,reached=!s.stop;s.dispatching=true;s.target=receiver;s.path=[receiver];
        try{
            const invoke=capture=>{
                if(s.stop)return;s.currentTarget=receiver;s.eventPhase=2;
                const list=apply(eventMapGet,m,[type])||[],snapshot=apply(eventSlice,list,[]);
                for(const entry of snapshot){
                    if(entry.removed||entry.capture!==capture)continue;if(entry.once)removeWorkerListener(list,entry);s.passive=entry.passive;
                    try{if(typeof entry.fn==='function')apply(entry.fn,receiver,[event]);else{const fn=entry.fn.handleEvent;if(typeof fn==='function')apply(fn,entry.fn,[event]);}}catch(error){report(error);}finally{s.passive=false;}
                    if(s.immediate)break;
                }
            };
            if(reached)invoke(true);if(reached&&!s.immediate){
                /* stopPropagation at this target still permits its remaining
                 * listeners; stopImmediatePropagation suppresses them. */
                const stopped=s.stop;s.stop=false;invoke(false);s.stop=s.stop||stopped;
                if(!s.immediate){try{const fn=object['on'+type];if(typeof fn==='function'&&apply(fn,receiver,[event])===false&&s.cancelable)s.defaultPrevented=true;}catch(error){report(error);}}
            }
            return !s.defaultPrevented;
        }finally{s.currentTarget=null;s.eventPhase=0;s.path=[];s.dispatching=s.passive=s.stop=s.immediate=false;}
    }
    function dispatchNativeWorkerEvent(object,event){eventState(event).isTrusted=true;return dispatchWorkerEvent(object,event);}
    Object.assign(globalThis,{DOMException,Event,ErrorEvent,EventTarget});
    /* @include js_navigator.js */
    const {navigator,Interface:WorkerNavigator}=navigatorBridge.create(true);
    /* @include js_encoding.js */
    /* @include js_url.js */
    /* @include js_clone.js */
    /* @include js_worker_messaging.js */
    /* @include js_sanitizer_constants.js */
    /* @include js_html_safety.js */
    const {MessageEvent,MessagePort,MessageChannel}=workerMessagingBridge;
    Object.assign(globalThis,{MessageEvent,MessagePort,MessageChannel});
    const target=new EventTarget(),timers=new Map(),fetches=new Map();let nextTimer=0,closed=false,address='',workerName='',workerCanCancel=false,reporting=false,lastPortTask=false;
    const errorPayload=e=>({message:String(e?.message||e),filename:address,lineno:0,stack:String(e?.stack||'')});
    function report(e){const info=errorPayload(e);if(reporting){host.send(4,0,info);return;}reporting=true;try{const event=new ErrorEvent('error',{...info,error:e,cancelable:true});if(dispatchNativeWorkerEvent(target,event))host.send(4,0,info);}finally{reporting=false;}}
    function evalSource(text,url){return host.eval(String(text),String(url));}
    function postMessage(value,options){if(closed)return;const list=Array.isArray(options)?options:options?.transfer,prepared=workerMessagingBridge.prepareExternal(value,cloneData.transferList(list));try{host.portSend(2,0,0,prepared.packet,prepared.plan);}catch(error){workerMessagingBridge.abortExternal(prepared);throw error;}}
    function close(){if(closed)return;closed=true;timers.clear();workerMessagingBridge.close();host.close();}
    function timer(fn,ms,args,repeat){if(closed)return 0;if(typeof fn!=='function')fn=new Function(htmlSafetyBridge.timerCode(fn,repeat?'setInterval':'setTimeout'));let id;for(let i=0;i<=timers.size;i++){nextTimer=nextTimer===4294967295?1:nextTimer+1;if(!timers.has(nextTimer)){id=nextTimer;break;}}if(id===undefined)throw new RangeError('Worker timer identifier space exhausted');const delay=Math.max(1,Math.min(2147483647,Number(ms)||0));timers.set(id,{fn,args,repeat,delay,due:host.now()+delay});return id;}
    function importScripts(...urls){for(const u of urls){const url=new URL(htmlSafetyBridge.check(u,3,'WorkerGlobalScope importScripts'),address).href;const r=host.import(url);if(r[4]||r[0]<200||r[0]>=300)throw new DOMException(r[4]||'Worker script load failed','NetworkError');evalSource(new TextDecoder().decode(r[3]),r[1]||url);}}
    const workerAbortBridge=(()=>{
        const apply=Reflect.apply,get=WeakMap.prototype.get,set=WeakMap.prototype.set,has=WeakMap.prototype.has,
            add=Set.prototype.add,remove=Set.prototype.delete,values=Set.prototype.values,clear=Set.prototype.clear,
            NativeSet=Set,Reference=WeakRef,deref=WeakRef.prototype.deref,define=Object.defineProperty,
            dispatch=EventTarget.prototype.dispatchEvent,listen=EventTarget.prototype.addEventListener,unlisten=EventTarget.prototype.removeEventListener,
            mapGet=Map.prototype.get,string=String,trunc=Math.trunc,finite=Number.isFinite,
            slots=new WeakMap(),controllers=new WeakMap(),retained=new NativeSet(),token={};
        const setSize=Object.getOwnPropertyDescriptor(Set.prototype,'size').get,setNext=Object.getPrototypeOf(apply(values,retained,[])).next;
        const size=s=>apply(setSize,s,[]),each=(s,fn)=>{const iterator=apply(values,s,[]);for(;;){const item=apply(setNext,iterator,[]);if(item.done)return;fn(item.value);}};
        const safeReport=error=>{try{report(error);}catch(_){}};
        const state=signal=>{const s=apply(get,slots,[signal]);if(!s)throw new TypeError('Illegal AbortSignal invocation');return s;};
        const text=value=>{if(typeof value==='symbol')throw new TypeError('Symbol is not a DOMString');return string(value);};
        const removeDependencies=list=>{for(let i=0;i<list.length;i++)apply(remove,state(list[i][0]).dependents,[list[i][1]]);};
        const finalizer=new FinalizationRegistry(removeDependencies),register=FinalizationRegistry.prototype.register,unregister=FinalizationRegistry.prototype.unregister;
        function retain(signal){const s=state(signal),m=apply(get,listeners,[signal]),events=m&&apply(mapGet,m,['abort']);if(!s.aborted&&size(s.sources)&&(size(s.algorithms)||s.handler||(events&&events.some(entry=>!entry.removed))))apply(add,retained,[signal]);else apply(remove,retained,[signal]);}
        function clean(signal){const s=state(signal);removeDependencies(s.dependencies);s.dependencies=[];apply(clear,s.sources,[]);apply(unregister,finalizer,[signal]);apply(remove,retained,[signal]);}
        function run(signal){const s=state(signal);each(s.algorithms,algorithm=>{try{algorithm(s.reason);}catch(error){safeReport(error);}});apply(clear,s.algorithms,[]);clean(signal);const event=new Event('abort');try{dispatchNativeWorkerEvent(signal,event);}catch(error){safeReport(error);}}
        function abort(signal,reason){
            const s=state(signal);if(s.aborted)return;s.aborted=true;s.reason=reason===undefined?new DOMException('The operation was aborted','AbortError'):reason;
            const dependents=[];each(s.dependents,reference=>{const dependent=apply(deref,reference,[]);if(!dependent){apply(remove,s.dependents,[reference]);return;}const d=state(dependent);if(!d.aborted){d.aborted=true;d.reason=s.reason;dependents[dependents.length]=dependent;}});
            run(signal);for(let i=0;i<dependents.length;i++)run(dependents[i]);apply(clear,s.dependents,[]);
        }
        function newSignal(){return new AbortSignal(token);}
        class AbortSignal extends EventTarget{
            constructor(secret){super();if(secret!==token)throw new TypeError('Illegal constructor');apply(set,slots,[this,{aborted:false,reason:undefined,handler:null,dependent:false,algorithms:new NativeSet(),sources:new NativeSet(),dependents:new NativeSet(),dependencies:[]}]);}
            get aborted(){return state(this).aborted;}get reason(){return state(this).reason;}throwIfAborted(){const s=state(this);if(s.aborted)throw s.reason;}
            get onabort(){return state(this).handler;}set onabort(value){state(this).handler=typeof value==='function'?value:null;retain(this);}
            addEventListener(type,fn,options){state(this);apply(listen,this,[text(type),fn,options]);retain(this);}
            removeEventListener(type,fn){state(this);apply(unlisten,this,[text(type),fn]);retain(this);}
            static abort(reason){const signal=newSignal(),s=state(signal);s.aborted=true;s.reason=reason===undefined?new DOMException('The operation was aborted','AbortError'):reason;return signal;}
            static timeout(milliseconds){
                if(!arguments.length)throw new TypeError('timeout requires milliseconds');const number=+milliseconds,ms=trunc(number);if(!finite(number)||ms<0||ms>=18446744073709551616)throw new TypeError('Timeout is outside unsigned long long range');
                const signal=newSignal(),started=host.now();function elapsed(){const left=ms-(host.now()-started);if(left>0)timer(elapsed,Math.min(left,2147483647),[],false);else abort(signal,new DOMException('Timed out','TimeoutError'));}timer(elapsed,Math.min(ms,2147483647),[],false);return signal;
            }
            static any(input){
                if(!arguments.length||input==null||(typeof input!=='object'&&typeof input!=='function'))throw new TypeError('any requires a signal sequence');const sources=[...input];for(let i=0;i<sources.length;i++)state(sources[i]);
                const signal=newSignal(),s=state(signal);for(let i=0;i<sources.length;i++){const p=state(sources[i]);if(p.aborted){s.aborted=true;s.reason=p.reason;return signal;}}s.dependent=true;
                for(let i=0;i<sources.length;i++){const source=sources[i],p=state(source);if(p.dependent)each(p.sources,root=>apply(add,s.sources,[root]));else apply(add,s.sources,[source]);}
                const reference=new Reference(signal);each(s.sources,source=>{apply(add,state(source).dependents,[reference]);s.dependencies[s.dependencies.length]=[source,reference];});if(s.dependencies.length)apply(register,finalizer,[signal,s.dependencies,signal]);return signal;
            }
        }
        class AbortController{
            constructor(){apply(set,controllers,[this,newSignal()]);}
            get signal(){const signal=apply(get,controllers,[this]);if(!signal)throw new TypeError('Illegal AbortController invocation');return signal;}
            abort(reason){const signal=apply(get,controllers,[this]);if(!signal)throw new TypeError('Illegal AbortController invocation');abort(signal,reason);}
        }
        return {AbortSignal,AbortController,state,brand:signal=>apply(has,slots,[signal]),subscribe(signal,algorithm){const s=state(signal);if(s.aborted)return ()=>{};apply(add,s.algorithms,[algorithm]);retain(signal);return ()=>{apply(remove,s.algorithms,[algorithm]);retain(signal);};}};
    })();
    const {AbortSignal,AbortController}=workerAbortBridge;
    const workerFetchBridge=(()=>{
        const apply=Reflect.apply,get=WeakMap.prototype.get,set=WeakMap.prototype.set,
            mapGet=Map.prototype.get,mapSet=Map.prototype.set,mapHas=Map.prototype.has,mapDelete=Map.prototype.delete,
            mapEntries=Map.prototype.entries,mapKeys=Map.prototype.keys,mapValues=Map.prototype.values,
            create=Object.create,define=Object.defineProperty,string=String,slice=ArrayBuffer.prototype.slice,
            Buffer=ArrayBuffer,isView=ArrayBuffer.isView,Bytes=Uint8Array,Encoder=TextEncoder,Decoder=TextDecoder,NativeMap=Map,
            entries=Object.entries,isArray=Array.isArray,NativePromise=Promise,rejectPromise=Promise.reject,headerSlots=new WeakMap(),responseSlots=new WeakMap();
        const text=v=>{if(typeof v==='symbol')throw new TypeError('Symbol is not a DOMString');return string(v);};
        const hstate=h=>{const s=apply(get,headerSlots,[h]);if(!s)throw new TypeError('Illegal Headers invocation');return s;};
        const rstate=r=>{const s=apply(get,responseSlots,[r]);if(!s)throw new TypeError('Illegal Response invocation');return s;};
        const name=k=>{k=text(k).toLowerCase();if(!/^[!#$%&'*+.^_`|~0-9a-z-]+$/.test(k))throw new TypeError('Invalid header name');return k;};
        const value=v=>{v=text(v).replace(/^[\t\r\n ]+|[\t\r\n ]+$/g,'');if(/[\0\r\n\u0100-\uffff]/.test(v))throw new TypeError('Invalid header value');return v;};
        function writable(s,k){if(s.guard==='immutable')throw new TypeError('Headers are immutable');return s.guard!=='response'||!/^set-cookie2?$/.test(k);}
        function append(h,k,v){const s=hstate(h);k=name(k);v=value(v);if(!writable(s,k))return;const old=apply(mapGet,s.map,[k]);apply(mapSet,s.map,[k,old===undefined?v:old+', '+v]);}
        function fill(h,input){if(input===undefined)return;const source=apply(get,headerSlots,[input]);if(source){for(const pair of apply(mapEntries,source.map,[]))append(h,pair[0],pair[1]);return;}if(input===null||(typeof input!=='object'&&typeof input!=='function'))throw new TypeError('HeadersInit requires an object');if(isArray(input)||input[Symbol.iterator]){for(const pair of input){const parts=[...pair];if(parts.length!==2)throw new TypeError('Header pair requires two entries');append(h,parts[0],parts[1]);}}else for(const pair of entries(input))append(h,pair[0],pair[1]);}
        class Headers{
            constructor(input){apply(set,headerSlots,[this,{map:new NativeMap(),guard:'none'}]);fill(this,input);}
            append(k,v){append(this,k,v);}get(k){return apply(mapGet,hstate(this).map,[name(k)])??null;}has(k){return apply(mapHas,hstate(this).map,[name(k)]);}
            set(k,v){const s=hstate(this);k=name(k);v=value(v);if(writable(s,k))apply(mapSet,s.map,[k,v]);}
            delete(k){const s=hstate(this);k=name(k);if(writable(s,k))apply(mapDelete,s.map,[k]);}
            entries(){return apply(mapEntries,hstate(this).map,[]);}keys(){return apply(mapKeys,hstate(this).map,[]);}values(){return apply(mapValues,hstate(this).map,[]);}
            forEach(fn,thisArg){if(typeof fn!=='function')throw new TypeError('Callback required');for(const [k,v]of apply(mapEntries,hstate(this).map,[]))apply(fn,thisArg,[v,k,this]);}
            [Symbol.iterator](){return apply(mapEntries,hstate(this).map,[]);}
        }
        function headers(input,guard){const h=create(Headers.prototype);apply(set,headerSlots,[h,{map:new NativeMap(),guard:guard==='immutable'?'response':guard}]);fill(h,input);hstate(h).guard=guard;return h;}
        const copy=b=>b===null?null:apply(slice,b,[0]);
        function response(bytes,info){const r=create(Response.prototype);apply(set,responseSlots,[r,{...info,bytes,used:false}]);if(bytes===null)define(r,'body',{value:null,enumerable:true,configurable:true});return r;}
        function consume(r){const s=rstate(r);if(s.used)throw new TypeError('Body already consumed');if(s.bytes===null)return new Buffer(0);if(s.abort){const signal=workerAbortBridge.state(s.abort);if(signal.aborted)throw signal.reason;}s.used=true;return copy(s.bytes);}
        class Response{
            constructor(body=null,init={}){
                init=init??{};const status=init.status===undefined?200:(+init.status>>>0)&65535,statusText=init.statusText===undefined?'':text(init.statusText);
                if(status<200||status>599)throw new RangeError('Invalid Response status');if(/[^\t\x20-\x7e\x80-\xff]/.test(statusText))throw new TypeError('Invalid status text');
                if(body!==null&&[204,205,304].includes(status))throw new TypeError('Null-body status cannot have a body');
                let bytes=null;if(body!==null){if(body instanceof Buffer)bytes=copy(body);else if(isView(body))bytes=new Bytes(new Bytes(body.buffer,body.byteOffset,body.byteLength)).buffer;else bytes=new Encoder().encode(text(body)).buffer;}
                const h=headers(init.headers,'response');if(body!==null&&!(body instanceof Buffer)&&!isView(body)&&!apply(mapHas,hstate(h).map,['content-type']))append(h,'content-type','text/plain;charset=UTF-8');
                apply(set,responseSlots,[this,{status,statusText,headers:h,url:'',redirected:false,type:'default',bytes,used:false,abort:null}]);if(bytes===null)define(this,'body',{value:null,enumerable:true,configurable:true});
            }
            get status(){return rstate(this).status;}get statusText(){return rstate(this).statusText;}get headers(){return rstate(this).headers;}get url(){return rstate(this).url;}get redirected(){return rstate(this).redirected;}get type(){return rstate(this).type;}
            get ok(){const s=rstate(this);return s.status>=200&&s.status<300;}get bodyUsed(){return rstate(this).used;}
            async arrayBuffer(){return consume(this);}
            async text(){return new Decoder().decode(consume(this));}async json(){return JSON.parse(new Decoder().decode(consume(this)));}
            clone(){const s=rstate(this);if(s.used)throw new TypeError('Body already consumed');return response(copy(s.bytes),{status:s.status,statusText:s.statusText,headers:headers(s.headers,hstate(s.headers).guard),url:s.url,redirected:s.redirected,type:s.type,abort:s.abort});}
        }
        function networkResponse(r,flags,signal){
            if(flags&4)return response(null,{status:0,statusText:'',headers:headers(undefined,'immutable'),url:'',redirected:false,type:'opaque',abort:null});
            /* Native transport has already applied CORS exposure and cookie
               filtering, including redirect taint. Do not filter it twice. */
            const pairs=[];let statusText='';for(const line of text(r[2]).split(/\r?\n/)){if(line.startsWith('HTTP/1.1 ')){const at=line.indexOf(' ',9);if(at>=0)statusText=line.slice(at+1);continue;}const at=line.indexOf(':');if(at>0)pairs.push([line.slice(0,at),line.slice(at+1)]);}
            return response([204,205,304].includes(r[0])?null:copy(r[3]),{status:r[0],statusText,headers:headers(pairs,'immutable'),url:r[1],redirected:!!(flags&2),type:flags&1?'cors':'basic',abort:signal});
        }
        return {Headers,Response,NativePromise,reject:error=>apply(rejectPromise,NativePromise,[error]),text,networkResponse,remember:(id,p)=>apply(mapSet,fetches,[id,p]),take:id=>{const p=apply(mapGet,fetches,[id]);apply(mapDelete,fetches,[id]);return p;}};
    })();
    const {Headers,Response}=workerFetchBridge,workerFetchURL=URL;
    function fetch(input,options={}){
        try{
            if(closed)throw new TypeError('Worker closed');options=options??{};
            if(typeof options!=='object'&&typeof options!=='function')throw new TypeError('RequestInit requires an object');
            const text=workerFetchBridge.text,url=new workerFetchURL(text(input),address),
                body=options.body,cache=options.cache,credentials=options.credentials,headerInit=options.headers,
                integrity=options.integrity,keepalive=options.keepalive,method=options.method,mode=options.mode,
                redirect=options.redirect,referrer=options.referrer,policy=options.referrerPolicy,signal=options.signal,
                requestMode=mode===undefined?'cors':text(mode),ref=referrer===undefined?'about:client':text(referrer),refPolicy=policy===undefined?'':text(policy);
            if(url.username||url.password)throw new TypeError('Fetch URL cannot contain credentials');
            if(!['cors','no-cors','same-origin','navigate'].includes(requestMode))throw new TypeError('Invalid request mode');
            if(!['','no-referrer','no-referrer-when-downgrade','same-origin','origin','strict-origin','origin-when-cross-origin','strict-origin-when-cross-origin','unsafe-url'].includes(refPolicy))throw new TypeError('Invalid referrer policy');
            if(ref!==''&&ref!=='about:client')new workerFetchURL(ref,address);
            const noReferrer=ref===''||refPolicy==='no-referrer';
            if(signal!=null&&!workerAbortBridge.brand(signal))throw new TypeError('Expected AbortSignal');
            if((method!==undefined&&text(method).toUpperCase()!=='GET')||body!=null||headerInit!=null||(credentials!==undefined&&text(credentials)!=='same-origin')||(requestMode!=='cors'&&requestMode!=='no-cors')||(redirect!==undefined&&text(redirect)!=='follow')||(cache!==undefined&&text(cache)!=='default')||(integrity!==undefined&&text(integrity)!=='')||keepalive||(!noReferrer&&(ref!=='about:client'||refPolicy!=='')))throw new DOMException('Worker Fetch currently supports GET, cors/no-cors, same-origin credentials, follow redirects, and no-referrer/default referrer','NotSupportedError');
            const requestURL=url.href;if(closed)throw new TypeError('Worker closed');
            if(signal!=null&&workerAbortBridge.state(signal).aborted)throw workerAbortBridge.state(signal).reason;
            if(signal!=null&&!workerCanCancel)throw new DOMException('This Worker host has no native Fetch cancellation','NotSupportedError');
            return new workerFetchBridge.NativePromise((resolve,reject)=>{const id=host.request(requestURL,requestMode==='no-cors',noReferrer),p={resolve,reject,noCors:requestMode==='no-cors',signal,cleanup:()=>{}};workerFetchBridge.remember(id,p);if(signal!=null)p.cleanup=workerAbortBridge.subscribe(signal,reason=>{if(!workerFetchBridge.take(id))return;p.cleanup();try{host.cancel(id);}finally{reject(reason);}});});
        }catch(error){return workerFetchBridge.reject(error);}
    }
    Object.assign(globalThis,{self:globalThis,navigator,WorkerNavigator,postMessage,close,importScripts,Headers,Response,AbortSignal,AbortController,fetch,
        setTimeout:(fn,ms,...args)=>timer(fn,ms,args,false),setInterval:(fn,ms,...args)=>timer(fn,ms,args,true),
        clearTimeout:id=>timers.delete(Number(id)),clearInterval:id=>timers.delete(Number(id)),
        queueMicrotask:fn=>{if(typeof fn!=='function')throw new TypeError('Callback required');Promise.resolve().then(fn).catch(report);},
        performance:{now:()=>host.now()},console:Object.fromEntries(['log','warn','error','info','debug'].map(k=>[k,(...args)=>host.send(7,0,{message:args.map(String).join(' '),filename:address})])),
        addEventListener:target.addEventListener.bind(target),removeEventListener:target.removeEventListener.bind(target),dispatchEvent:target.dispatchEvent.bind(target)});
    Object.defineProperties(globalThis,{name:{get:()=>workerName},onmessage:{get:()=>target.onmessage,set:fn=>target.onmessage=fn},onmessageerror:{get:()=>target.onmessageerror,set:fn=>target.onmessageerror=fn},onerror:{get:()=>target.onerror,set:fn=>target.onerror=fn},onsecuritypolicyviolation:{get:()=>target.onsecuritypolicyviolation,set:fn=>target.onsecuritypolicyviolation=fn}});
    return {
        safetyCheck:htmlSafetyBridge.check,
        safetyDynamicCode:htmlSafetyBridge.dynamicCode,
        safetyViolation(init){if(!closed)dispatchNativeWorkerEvent(target,htmlSafetyBridge.violationEvent(init));},
        start(r){address=r[1];workerName=String(r[5]||"");workerCanCancel=r[6]===true;if(r[4]||r[0]<200||r[0]>=300)throw new DOMException(r[4]||'Worker source failed','NetworkError');Object.defineProperty(globalThis,'location',{value:Object.freeze({href:address,origin:new URL(address).origin,toString(){return address;}}),configurable:true});evalSource(new TextDecoder().decode(r[3]),address);},
        receive(packet,op=2,id=0,kind=0){try{if(op===8||op===9||op===10){workerMessagingBridge.receivePort(id,kind,packet,op!==8);return;}const payload=kind===3?workerMessagingBridge.importExternal(packet):{data:cloneData.deserialize(packet),ports:[]};if(!closed)dispatchNativeWorkerEvent(target,new MessageEvent('message',{data:payload.data,ports:payload.ports}));}catch(e){if(!closed)dispatchNativeWorkerEvent(target,new MessageEvent('messageerror'));}},
        loaded(id,r,flags=0){const p=workerFetchBridge.take(id);if(!p||closed)return;p.cleanup();if(r[4]||(!(flags&4)&&!r[0])||((flags&4)&&!p.noCors)){p.reject(new TypeError(r[4]||'Worker fetch failed'));return;}try{p.resolve(workerFetchBridge.networkResponse(r,flags,p.signal));}catch(error){p.reject(error);}},
        tick(now){if(closed)return false;const ports=workerMessagingBridge.ready();if(ports&&!lastPortTask){lastPortTask=true;return workerMessagingBridge.runOne();}for(const [id,t]of timers){if(t.due>now)continue;lastPortTask=false;if(t.repeat)t.due=now+t.delay;else timers.delete(id);try{t.fn(...t.args);}catch(e){report(e);}return true;}lastPortTask=ports;return ports?workerMessagingBridge.runOne():false;},
        deadline(){if(workerMessagingBridge.ready())return host.now();let due=-1;for(const t of timers.values())if(due<0||t.due<due)due=t.due;return due;},report
    };
})(__workerHost);
