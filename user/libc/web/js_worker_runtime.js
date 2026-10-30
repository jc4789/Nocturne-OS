/* Only this private host crosses the native pipe. No window, document, native
 * modules, files, sockets or process functions are installed in this realm. */
(function(host){
    const apply=Reflect.apply;
    delete globalThis.__workerHost;
    // HR Time: this realm's time origin is its creation. host.now() is the monotonic clock in ms,
    // coarsened to 100 us; a difference of two is rounded back to that grain (floating error).
    // performance (js_performance.js) reads the same origin, so Event.timeStamp and
    // performance.now() are one timeline.
    const timeOrigin=host.now(),relativeNow=()=>Math.round((host.now()-timeOrigin)*10)/10;host.timeOrigin=timeOrigin;
    // SharedArrayBuffer and Atomics stay for the platform bootstrap; the
    // native policy start removes the constructor outside isolation.
    /* @include js_dom_exception.js */
    const DOMException=domExceptionBridge.DOMException,QuotaExceededError=domExceptionBridge.QuotaExceededError;
    /* @include js_atob.js */
    const eventStates=new WeakMap(),listeners=new WeakMap(),eventGet=WeakMap.prototype.get,eventSet=WeakMap.prototype.set;
    const eventDefine=Object.defineProperty,eventCreate=Object.create,eventMapGet=Map.prototype.get,eventMapSet=Map.prototype.set,eventMapForEach=Map.prototype.forEach,eventSlice=Array.prototype.slice;
    const EventMap=Map,EventString=String,eventIndexOf=Array.prototype.indexOf,eventSplice=Array.prototype.splice;
    const eventString=value=>{if(typeof value==='symbol')throw new TypeError('Symbol is not a DOMString');return EventString(value);};
    function eventState(event){const s=apply(eventGet,eventStates,[event]);if(!s)throw new TypeError('Illegal Event receiver');return s;}
    class Event{
        constructor(type,init={}){
            if(!arguments.length)throw new TypeError('Event requires type');type=eventString(type);init=init==null?{}:Object(init);
            apply(eventSet,eventStates,[this,{type,bubbles:!!init.bubbles,cancelable:!!init.cancelable,composed:!!init.composed,
                target:null,currentTarget:null,eventPhase:0,defaultPrevented:false,isTrusted:false,timeStamp:relativeNow(),
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
    /* @include js_promise_rejection.js */
    /* @include js_custom_event.js */
    const CustomEvent=customEventInterface(Event,eventState);
    function workerListeners(value){const m=apply(eventGet,listeners,[value]);if(!m)throw new TypeError('Illegal EventTarget receiver');return m;}
    function removeWorkerListener(target,list,entry){if(entry.removed)return;entry.removed=true;const index=apply(eventIndexOf,list,[entry]);if(index>=0)apply(eventSplice,list,[index,1]);if(entry.unlisten){entry.unlisten();entry.unlisten=null;}if(entry.type==='abort')abortBridge.listen(target,-1);}
    class EventTarget{
        constructor(){apply(eventSet,listeners,[this,new EventMap()]);}
        addEventListener(type,fn,options={}){
            const m=workerListeners(this);type=eventString(type);if(fn==null)return;
            if(typeof fn!=='function'&&typeof fn!=='object')throw new TypeError('Invalid event listener');
            const capture=typeof options==='boolean'?options:!!options?.capture,once=typeof options==='object'&&!!options?.once,
                passive=typeof options==='object'&&!!options?.passive,signal=typeof options==='object'?options?.signal:null;
            if(signal!=null&&!abortBridge.signals.is(signal))throw new TypeError('Expected AbortSignal');
            if(signal!=null&&abortBridge.signals.aborted(signal))return;
            const list=apply(eventMapGet,m,[type])||[];
            if(list.some(x=>!x.removed&&x.fn===fn&&x.capture===capture))return;
            const entry={type,fn,capture,once,passive,removed:false,unlisten:null};list.push(entry);apply(eventMapSet,m,[type,list]);
            if(type==='abort')abortBridge.listen(this,1);
            if(signal!=null)entry.unlisten=abortBridge.signals.follow(signal,()=>removeWorkerListener(this,list,entry));
        }
        removeEventListener(type,fn,options={}){
            const m=workerListeners(this);type=eventString(type);const capture=typeof options==='boolean'?options:!!options?.capture,
                list=apply(eventMapGet,m,[type])||[];
            for(const entry of list)if(!entry.removed&&entry.fn===fn&&entry.capture===capture){removeWorkerListener(this,list,entry);break;}
        }
        dispatchEvent(event){workerListeners(this);const s=eventState(event);if(!s.initialized||s.dispatching)throw new DOMException('Uninitialized or dispatching event','InvalidStateError');s.isTrusted=false;return dispatchWorkerEvent(this,event);}
    }
    /* DOM "dispatch" (https://dom.spec.whatwg.org/#concept-event-dispatch). The path is a
       snapshot's entries in js_bootstrap.js's form, entries[0] the target itself, then the
       parents a platform object names (IndexedDB: request, transaction, database); without a
       snapshot the path is the target alone. Capture runs from the outermost parent inward, then
       the target's capture and other listeners as two invocations, then bubbling; each
       invocation first checks stopPropagation. A listener that throws is reported and sets the
       event's listenerException (IndexedDB aborts the request's transaction for it). */
    function dispatchWorkerEvent(object,event,snapshot){
        workerListeners(object);const s=eventState(event);if(!s.initialized||s.dispatching)throw new DOMException('Uninitialized or dispatching event','InvalidStateError');
        const scope=node=>node===target?globalThis:node,receiver=scope(object),type=s.type;
        const planned=!!(snapshot&&snapshot.entries),entries=planned?snapshot.entries:[{node:object,target:receiver,visible:[receiver],atTarget:true}];
        s.dispatching=true;s.target=receiver;
        function visit(entry,capture,phase){
            const node=entry.node,current=scope(node),list=apply(eventMapGet,workerListeners(node),[type])||[],listed=apply(eventSlice,list,[]);
            s.target=scope(entry.target);s.path=entry.visible;s.eventPhase=phase;s.currentTarget=current;
            for(const item of listed){
                if(item.removed||item.capture!==capture)continue;if(item.once)removeWorkerListener(node,list,item);s.passive=item.passive;
                try{
                    if(typeof item.fn==='function')apply(item.fn,current,[event]);
                    else{const fn=item.fn.handleEvent;if(typeof fn!=='function')throw new TypeError('EventListener handleEvent is not callable');apply(fn,item.fn,[event]);}
                }catch(error){s.listenerException=true;report(error);}finally{s.passive=false;}
                if(s.immediate)return;
            }
            /* The handler, as HTML's event handler processing algorithm runs it: this is the
               currentTarget; special error event handling (an ErrorEvent named error at this
               global) passes the ErrorEvent's five values and cancels on true, otherwise false
               cancels. It still runs after the listeners, not where it was first set. */
            if(!capture){try{
                const fn=node['on'+type];
                if(typeof fn==='function'){
                    const values=type==='error'&&current===globalThis?errorEventValues(event):null;
                    const result=values!==null?apply(fn,current,[values.message,values.filename,values.lineno,values.colno,values.error]):apply(fn,current,[event]);
                    if((values!==null?result===true:result===false)&&s.cancelable&&!s.passive)s.defaultPrevented=true;
                }
            }catch(error){s.listenerException=true;report(error);}}
        }
        try{
            for(let i=entries.length-1;i>0&&!s.stop;i--)visit(entries[i],true,entries[i].atTarget?2:1);
            if(entries.length&&!s.stop){visit(entries[0],true,2);if(!s.stop)visit(entries[0],false,2);}
            for(let i=1;i<entries.length&&!s.stop;i++)if(s.bubbles||entries[i].atTarget)visit(entries[i],false,entries[i].atTarget?2:3);
            s.target=planned?scope(snapshot.finalTarget):receiver;
            return !s.defaultPrevented;
        }finally{s.currentTarget=null;s.eventPhase=0;s.path=[];s.dispatching=s.passive=s.stop=s.immediate=false;}
    }
    function dispatchNativeWorkerEvent(object,event,snapshot){eventState(event).isTrusted=true;return dispatchWorkerEvent(object,event,snapshot);}
    Object.assign(globalThis,{DOMException,QuotaExceededError,Event,ErrorEvent,PromiseRejectionEvent,CustomEvent,EventTarget});
    /* XMLHttpRequest and ProgressEvent are the Window's (js_xhr.js); ProgressEvent is made here,
       before FileReader (js_blob.js) takes it, the rest once this realm's Fetch exists. */
    /* @include js_xhr.js */
    const ProgressEvent=progressEventInterface(Event);
    Object.defineProperty(globalThis,'ProgressEvent',{value:ProgressEvent,writable:true,configurable:true});
    let blobHandlerTarget=()=>false,indexedDBHandlerTarget=()=>false,abortHandlerTarget=()=>false,xhrHandlerTarget=()=>false,eventSourceHandlerTarget=()=>false;
    const fileHandlers=new WeakMap();
    const dispatch=dispatchNativeWorkerEvent;
    const handlerTarget=(value,type)=>blobHandlerTarget(value,type)||indexedDBHandlerTarget(value,type)||abortHandlerTarget(value,type)||xhrHandlerTarget(value,type)||eventSourceHandlerTarget(value,type);
    function installHandlers(prototype,types){for(const type of types)eventDefine(prototype,'on'+type,{enumerable:true,configurable:true,
        get(){if(!handlerTarget(this,type))throw new TypeError('Illegal event handler receiver');return apply(eventGet,fileHandlers,[this])?.[type]||null;},
        set(value){if(!handlerTarget(this,type))throw new TypeError('Illegal event handler receiver');let map=apply(eventGet,fileHandlers,[this]);if(!map){map=eventCreate(null);apply(eventSet,fileHandlers,[this,map]);}
            const had=typeof map[type]==='function';map[type]=typeof value==='function'?value:null;
            /* An abort handler is an abort listener for AbortSignal's garbage collection rule. */
            if(type==='abort'&&had!==(map[type]!==null))abortBridge.listen(this,had?-1:1);}});}
    /* Whether one or more event listeners are registered on target, an event handler among them
       (XMLHttpRequest's upload listener flag). */
    function workerListened(target){
        const handlers=apply(eventGet,fileHandlers,[target]);
        if(handlers)for(const type in handlers)if(handlers[type])return true;
        const m=apply(eventGet,listeners,[target]);let found=false;
        if(m)apply(eventMapForEach,m,[list=>{for(let i=0;i<list.length;i++)if(!list[i].removed)found=true;}]);
        return found;
    }
    /* @include js_navigator.js */
    const {navigator,Interface:WorkerNavigator,languagesChanged}=navigatorBridge.create(true);
    /* @include js_encoding.js */
    /* @include js_url.js */
    /* @include js_streams.js */
    host.timer=(_,fn,ms,args)=>timer(fn,ms,args,false);host.clear=clearTimer;
    /* @include js_blob.js */
    Object.defineProperty(globalThis,'FileReaderSync',{value:blobBridge.FileReaderSync,writable:true,configurable:true});
    /* @include js_clone.js */
    /* @include js_geometry.js */
    const geometryBridge=geometryInterfaces({});
    /* @include js_typed_om.js */
    typedOMInterfaces({native:host,geometry:geometryBridge,DOMException});
    /* @include js_offscreen_canvas.js */
    /* @include js_worker_messaging.js */
    /* @include js_sanitizer_constants.js */
    /* @include js_html_safety.js */
    const {MessageEvent,MessagePort,MessageChannel}=workerMessagingBridge;
    Object.assign(globalThis,{MessageEvent,MessagePort,MessageChannel});
    const target=new EventTarget(),timers=new Map(),timerHeap=[],fetches=new Map();let nextTimer=0,timerOrder=0,timerNesting=0,closed=false,address='',workerName='',workerCanCancel=false,workerStreaming=false,reporting=false,lastPortTask=false,shared=false;
    /* The crypto task source of this event loop (js_crypto.js queues its "queue a global
       task" steps on host.postTask), taking turns with the other tasks in tick. */
    const cryptoTasks=[];let cryptoTaskHead=0,lastCryptoTask=false;
    host.postTask=task=>{if(!closed)cryptoTasks.push(task);};
    const cryptoTaskReady=()=>cryptoTaskHead<cryptoTasks.length;
    function runCryptoTask(){lastCryptoTask=true;const task=cryptoTasks[cryptoTaskHead];cryptoTasks[cryptoTaskHead++]=undefined;if(cryptoTaskHead===cryptoTasks.length){cryptoTasks.length=0;cryptoTaskHead=0;}try{task();}catch(e){report(e);}return true;}
    let formDataBridge;
    function initializeWorkerFormData(){
        // START's native policy URL is now established. No author script runs
        // until the same crypto and form algorithms as Window are installed.
        /* @include js_crypto.js */
        /* @include js_opfs.js */
        installOriginPrivateFileSystem(host,cryptoSecureContext,WorkerNavigator,!shared);
        /* @include js_cache_storage.js */
        installCacheStorage({...fetchBridge.cache,secure:cryptoSecureContext,native:host.cacheStorage,
            embedderPolicy:host.cacheEmbedderPolicy,streams:streamBridge});
        /* @include js_formdata.js */
        fetchBridge.initializeFormData(formDataBridge);
        cloneData.registerUncloneable(value=>formDataBridge.brand(value));
        /* @include js_object_url.js */
        /* This Worker's environment for its blob URL store: its origin (START's, "null" for a
           data: Worker's opaque one); a Worker creates no blob: document or Worker to inherit a
           policy container. */
        const objectURLOrigin=(()=>{try{return new URL(host.securityURL()).origin;}catch(_){return 'null';}})();
        fetchBridge.initializeObjectURLs(objectURLInterfaces({origin:()=>objectURLOrigin,policy:()=>null}));
        return formDataBridge;
    }
    const errorPayload=e=>({message:String(e?.message||e),filename:address,lineno:0,stack:String(e?.stack||'')});
    function report(e){const info=errorPayload(e);if(reporting){host.send(4,0,info);return;}reporting=true;try{const event=new ErrorEvent('error',{...info,error:e,cancelable:true});if(dispatchNativeWorkerEvent(target,event))host.send(4,0,info);}finally{reporting=false;}}
    function evalSource(text,url){return host.eval(String(text),String(url));}
    function postMessage(value,options){if(closed)return;const list=Array.isArray(options)?options:options?.transfer,prepared=workerMessagingBridge.prepareExternal(value,cloneData.transferList(list));try{host.portSend(2,0,0,prepared.packet,prepared.plan);}catch(error){workerMessagingBridge.abortExternal(prepared);throw error;}}
    function close(){if(closed)return;closed=true;workerFetchTransport.close();timers.clear();timerHeap.length=0;workerMessagingBridge.close();host.close();}
    const timerBefore=(a,b)=>a.due<b.due||(a.due===b.due&&a.order<b.order);
    function timerSwap(a,b){const t=timerHeap[a];timerHeap[a]=timerHeap[b];timerHeap[b]=t;timerHeap[a].index=a;timerHeap[b].index=b;}
    function timerFix(index){
        while(index>0){const parent=(index-1)>>1;if(!timerBefore(timerHeap[index],timerHeap[parent]))break;timerSwap(index,parent);index=parent;}
        for(;;){const left=index*2+1;if(left>=timerHeap.length)break;const right=left+1,child=right<timerHeap.length&&timerBefore(timerHeap[right],timerHeap[left])?right:left;if(!timerBefore(timerHeap[child],timerHeap[index]))break;timerSwap(index,child);index=child;}
    }
    function clearTimer(id){const t=timers.get(Number(id));if(!t)return;timers.delete(t.id);const index=t.index,last=timerHeap.pop();if(index<timerHeap.length){timerHeap[index]=last;last.index=index;timerFix(index);}}
    // HTML timer initialization steps. setTimeout and setInterval (html) take the timeout as a Web
    // IDL long and inherit the nesting level of the timer task whose callback runs (timerNesting;
    // 0 in other tasks and in microtasks); above 5 a timeout below 4 ms becomes 4. The worker's own
    // tasks (html false) neither nest nor clamp, and never run before their time. Deadlines are
    // whole milliseconds of host.now()'s clock, as tick's now.
    function timer(fn,ms,args,repeat,html){if(closed)return 0;if(typeof fn!=='function'){fn=htmlSafetyBridge.timerCode(fn,repeat?'setInterval':'setTimeout');if(fn===null)return 0;}let id;for(let i=0;i<=timers.size;i++){nextTimer=nextTimer===4294967295?1:nextTimer+1;if(!timers.has(nextTimer)){id=nextTimer;break;}}if(id===undefined)throw new RangeError('Worker timer identifier space exhausted');if(timerOrder===9007199254740991){timerOrder=0;for(const t of timers.values())t.order=++timerOrder;for(let i=Math.floor(timerHeap.length/2)-1;i>=0;i--)timerFix(i);}const delay=Math.max(0,html?ms|0:Math.min(2147483647,Math.ceil(Number(ms)||0))),level=html?timerNesting:0,t={id,order:++timerOrder,fn,args,repeat,html:!!html,delay,nesting:html?(level>5?level:level+1):0,due:Math.floor(host.now())+(level>5&&delay<4?4:delay),index:timerHeap.length};timers.set(id,t);timerHeap.push(t);timerFix(t.index);return id;}
    function importScripts(...urls){for(const u of urls){const url=new URL(htmlSafetyBridge.check(u,3,'WorkerGlobalScope importScripts'),address).href;const r=host.import(url);performanceBridge.monotonicResource(0,IMPORT_REQUEST,url,wireField(r,LOADED_TIMING));if(r[4]||r[0]<200||r[0]>=300)throw new DOMException(r[4]||'Worker script load failed','NetworkError');evalSource(new TextDecoder().decode(r[3]),r[1]||url);}}
    /* @include js_abort.js */
    /* This Worker's events for AbortSignal: a trusted abort event through this realm's dispatch,
       its event handler attributes and timers. */
    const abortBridge=abortInterfaces({fire:signal=>dispatchNativeWorkerEvent(signal,new Event('abort')),
        handlers:installHandlers,report,now:()=>host.now(),timer:(fn,ms)=>timer(fn,ms,[],false)});
    abortHandlerTarget=abortBridge.handlerTarget;
    const {AbortSignal,AbortController}=abortBridge;
    /* @include js_fetch.js */
    /* This Worker's transport for Fetch: its parent window's native fetch, through
       browserjsworker.c (NJW_LOAD with the Request's modes, NJW_FRAME, NJW_RESUME, NJW_CANCEL) to
       js.c's worker_request, which applies the creating document's CORS, cookies, Mixed Content
       and CSP. An opaque response arrives blank from the parent (js_worker.c); a fetch the
       parent has not ended is in fetches, by its request id. */
    /* The parent's report timing of this Worker's own request (NJW_LOADED_TIMING, field 7 of an
       NJW_LOADED record, [1] of an END frame's), with js.c's request kind: 4 (P_WORKER) is
       importScripts' initiator type "other", 1 (P_FETCH) is fetch()'s "fetch". */
    const LOADED_TIMING=7,IMPORT_REQUEST=4,FETCH_REQUEST=1,ownProperty=Object.prototype.hasOwnProperty;
    /* An optional field of a wire record: its own, never one the page put on Array.prototype. */
    const wireField=(record,index)=>record!==null&&typeof record==='object'&&apply(ownProperty,record,[index])?record[index]:undefined;
    const workerFetchTransport=(()=>{
        const apply=Reflect.apply,mapGet=Map.prototype.get,mapSet=Map.prototype.set,mapDelete=Map.prototype.delete,mapForEach=Map.prototype.forEach,mapClear=Map.prototype.clear,
            setPrototype=Object.setPrototypeOf,NativePromise=Promise,NativeTypeError=TypeError,request=host.request,resume=host.resume,cancel=host.cancel,
            strIndex=String.prototype.indexOf,strSlice=String.prototype.slice,requestWait=host.requestWait;
        const record=object=>setPrototype(object,null);
        const take=id=>{const p=apply(mapGet,fetches,[id]);apply(mapDelete,fetches,[id]);return p;};
        /* A filtered response the request's modes do not allow: an opaque one for a request not in
           mode "no-cors", an opaque-redirect for one whose redirect mode is not "manual". */
        const contradicts=(flags,noCors,manual)=>((flags&4)&&!noCors)||((flags&64)&&!manual);
        /* An NJW_LOADED that is a network error: the parent's reason, no status (a filtered
           response has none), or a filtered response the request's modes contradict. */
        const failed=(r,flags,noCors,manual)=>r[4]||(!(flags&(4|64))&&!r[0])||contradicts(flags,noCors,manual);
        /* A fetch's end: its stream's (notify), then its promise's when no response was published. */
        function end(p,error){if(p.notify)p.notify(3,error);if(!p.published)p.reject(error===undefined?new NativeTypeError('Worker fetch failed'):error);}
        /* The response the parent delivered: NJW_LOADED's [status, url, headers, bytes] or a
           headers frame's [status, url, headers], its NJW_LOADED_* flags (4 opaque with its
           resource bits, 64 an opaque-redirect: its URL only). */
        function response(r,flags,bytes){
            if(flags&4)return fetchBridge.response(0,'','',undefined,null,false,'','opaque',(flags>>3)&7);
            if(flags&64)return fetchBridge.response(0,r[1],'',undefined,null,false,'','opaqueredirect');
            return fetchBridge.response(r[0],r[1],r[2],undefined,bytes,!!(flags&2),undefined,flags&1?'cors':'basic');
        }
        /* One frame taken: the parent sends the next on this resume, once per frame. The fetch
           is paused while its consumer handles the frame (frame below), so a resume the
           consumer makes then counts, as js.c's resumeFetch counts it for a Window (a Fetch
           observer may resume from its callback); more resumes it at once. */
        function next(id,p,more){if(more&&p.paused&&!p.done){p.paused=false;apply(resume,host,[id]);}}
        /* Fetch's report timing, once the fetch has ended (as a Window's js.c reports it): the
           entry in this Worker's performance timeline, by its request id (realm.initiator's). */
        function timing(p,record){try{performanceBridge.monotonicResource(p.id,FETCH_REQUEST,p.url,record);}catch(error){report(error);}}
        /* The parent's answer to obtainBlob: the Blob's bytes and its type (the Content-Type field
           worker_blob writes). */
        function blobAnswer(r){
            const headers=typeof r[2]==='string'?r[2]:'',field=apply(strIndex,headers,['Content-Type: ']),end=field<0?-1:apply(strIndex,headers,['\r\n',field]);
            if(r[4]||r[0]!==200||end<0)throw new NativeTypeError(r[4]||'The blob: URL has no Blob in this Worker\'s partition');
            return [r[3],apply(strSlice,headers,[field+14,end])];
        }
        return {
            noCors:()=>true,streaming:()=>workerStreaming,
            fetch(q,notify){
                if(closed)throw new NativeTypeError('Worker closed');
                const stream=!!notify&&workerStreaming,id=apply(request,host,[q,stream]);
                let resolve,reject;const promise=new NativePromise((a,b)=>{resolve=a;reject=b;});
                apply(mapSet,fetches,[id,record({id,resolve,reject,notify:stream?notify:undefined,noCors:q.noCors,manual:q.redirectManual,url:q.url,published:false,paused:false,done:false})]);
                return record({id,promise});
            },
            /* FileAPI "obtain a blob object" for a blob: URL this Worker's store does not hold: the
               parent's js.c resolves it in its document's store when this Worker is in that
               document's storage partition (worker_blob). The NJW_LOADED body and its Content-Type
               field are the Blob's bytes and type; Fetch (js_fetch.js) runs the rest of scheme
               fetch here, as a Window does. With a timeout (a synchronous XMLHttpRequest's), the
               event loop pauses for the answer as fetchSync's does: [bytes, type], null when the
               timeout passed first. */
            obtainBlob(url,timeout){
                if(closed)throw new NativeTypeError('Worker closed');
                const id=apply(request,host,[record({url,method:'GET',headers:'',body:null,sameOrigin:false,credentials:0,forcePreflight:false,
                    redirectError:false,cache:0,keepalive:false,noCors:false,noReferrer:true,referrer:'',referrerPolicy:0,destination:''}),false]);
                if(timeout!==undefined){const answer=apply(requestWait,host,[id,timeout]);return answer===null?null:blobAnswer(answer[0]);}
                let resolve,reject;const promise=new NativePromise((a,b)=>{resolve=a;reject=b;});
                apply(mapSet,fetches,[id,record({resolve,reject,notify:undefined,noCors:false,url,blob:true,published:false,paused:false,done:false})]);
                return record({id,promise});
            },
            resume(id){const p=apply(mapGet,fetches,[id]);if(!p||!p.paused||closed)return;p.paused=false;apply(resume,host,[id]);},
            cancel(id){const p=take(id);if(!p)return;p.done=true;if(workerCanCancel&&!closed)apply(cancel,host,[id]);},
            /* NJW_LOADED: a buffered response, or the error that ends any fetch. */
            loaded(id,r,flags){
                const p=take(id);if(!p||closed)return;p.done=true;
                if(p.blob){let blob;try{blob=blobAnswer(r);}catch(error){p.reject(error);return;}p.resolve(blob);return;}
                timing(p,wireField(r,LOADED_TIMING));
                if(failed(r,flags,p.noCors,p.manual)||p.notify){end(p,new NativeTypeError(r[4]||'Worker fetch failed'));return;}
                try{p.published=true;p.resolve(response(r,flags,r[3]));}catch(error){p.reject(error);}
            },
            /* NJW_FRAME: one frame of a streamed fetch (the parent's process_fetch_frame order:
               the stream first, then the promise). */
            frame(id,value,kind){
                const p=apply(mapGet,fetches,[id]);if(!p||closed||!p.notify)return;
                const event=(kind>>8)&7,flags=kind&127;
                if(event===3){
                    const error=wireField(value,0);take(id);p.done=true;end(p,error?new NativeTypeError(error):undefined);
                    timing(p,wireField(value,1));return;
                }
                if(event===1){
                    if(contradicts(flags,p.noCors,p.manual)){take(id);p.done=true;end(p,new NativeTypeError('Native Fetch response contradicts its filtering policy'));return;}
                    const published=response(value,flags,new ArrayBuffer(0));p.paused=true;
                    const more=p.notify(1,published);
                    p.published=true;p.resolve(published);next(id,p,more);return;
                }
                p.paused=true;next(id,p,p.notify(event,value));
            },
            /* Closing this Worker ends every fetch the parent has not. */
            close(){
                const list=[];apply(mapForEach,fetches,[p=>{list[list.length]=p;}]);apply(mapClear,fetches,[]);
                for(let i=0;i<list.length;i++){list[i].done=true;try{end(list[i],new DOMException('Worker closed','AbortError'));}catch(error){report(error);}}
            },
            /* A synchronous XMLHttpRequest's fetch, unstreamed: browserjsworker.c's requestWait
               pauses this worker's event loop (other records wait in its task queue) until the
               parent's NJW_LOADED, or until timeout ms (0: none) pass, when it tells the parent to
               cancel and gives null. */
            fetchSync(q,timeout,initiator){
                if(closed)throw new NativeTypeError('Worker closed');
                const id=apply(request,host,[q,false]);
                if(initiator)initiator(id);
                const answer=apply(requestWait,host,[id,timeout]);
                if(answer===null)return null;
                const r=answer[0],flags=answer[1];
                timing(record({id,url:q.url}),wireField(r,LOADED_TIMING));
                if(failed(r,flags,q.noCors,q.redirectManual))throw new NativeTypeError(r[4]||'Worker fetch failed');
                return response(r,flags,r[3]);
            }
        };
    })();
    /* This Worker's settings object for Fetch: its URL (the API base URL and the "client"
       referrer source), origin and policy container. */
    const fetchBridge=(()=>{
        const apply=Reflect.apply,NativeURL=URL,urlOrigin=Object.getOwnPropertyDescriptor(URL.prototype,'origin').get,
            dataURL=host.dataURL,digest=host.cryptoDigest,securityURL=host.securityURL,referrerPolicy=host.referrerPolicy;
        const origin=()=>{try{return apply(urlOrigin,new NativeURL(apply(securityURL,host,[])),[]);}catch(_){return 'null';}};
        return fetchInterfaces({signals:abortBridge.signals,transport:workerFetchTransport,
            baseURL:()=>address,origin,clientReferrer:()=>address,referrerPolicy:()=>apply(referrerPolicy,host,[]),
            url:()=>apply(securityURL,host,[])||address,dataURL:url=>apply(dataURL,host,[url]),
            task:fn=>timer(fn,0,[],false),clearTask:clearTimer,digest:(algorithm,bytes)=>apply(digest,host,[algorithm,bytes]),
            initiator:(id,type)=>performanceBridge.initiator(id,type)});
    })();
    fetchBridge.initialize();fetchBridge.initializeStreams(streamBridge);fetchBridge.initializeBlobs(blobBridge);
    const {Headers,Request,Response,fetch}=fetchBridge;
    /* This Worker's XMLHttpRequest: its Fetch, open()'s URL against its API base URL in UTF-8, no
       documents; its event loop pauses for a synchronous request (workerFetchTransport.fetchSync). */
    const xhr=xhrInterfaces({fetch:fetchBridge,parseURL:url=>urlBridge.encodingParse(url,address,'UTF-8'),listened:workerListened,
        log:text=>host.send(7,0,{message:text,filename:address}),document:null,synchronous:true});
    xhrHandlerTarget=xhr.handlerTarget;
    for(const name of ['XMLHttpRequest','XMLHttpRequestUpload','XMLHttpRequestEventTarget'])
        Object.defineProperty(globalThis,name,{value:xhr[name],writable:true,configurable:true});
    /* @include js_event_source.js */
    /* This Worker's EventSource: its Fetch (the parent window's transport), the constructor's URL
       against its API base URL in UTF-8, its MessageEvent, trusted events through this realm's
       dispatch and its timers (none run once it is closed). */
    const eventSourceBridge=eventSourceInterfaces({fetch:fetchBridge,parseURL:url=>urlBridge.encodingParse(url,address,'UTF-8'),
        MessageEvent,fire:dispatchNativeWorkerEvent,handlers:installHandlers,report,now:()=>host.now(),
        timer:(fn,ms)=>timer(fn,ms,[],false),clear:clearTimer});
    eventSourceHandlerTarget=eventSourceBridge.handlerTarget;
    Object.defineProperty(globalThis,'EventSource',{value:eventSourceBridge.EventSource,writable:true,configurable:true});
    /* @include js_performance.js */
    /* @include js_console.js */
    Object.assign(globalThis,{self:globalThis,navigator,WorkerNavigator,postMessage,close,importScripts,Headers,Request,Response,AbortSignal,AbortController,fetch,
        setTimeout:(fn,ms,...args)=>timer(fn,ms,args,false,true),setInterval:(fn,ms,...args)=>timer(fn,ms,args,true,true),
        clearTimeout:clearTimer,clearInterval:clearTimer,
        queueMicrotask:fn=>{if(typeof fn!=='function')throw new TypeError('Callback required');Promise.resolve().then(fn).catch(report);},
        reportError(e){if(!arguments.length)throw new TypeError("Failed to execute 'reportError': 1 argument required");report(e);},
        console:consoleNamespace((level,values)=>host.send(7,0,{message:values.map(String).join(' '),filename:address}),()=>host.now()),
        addEventListener:target.addEventListener.bind(target),removeEventListener:target.removeEventListener.bind(target),dispatchEvent:target.dispatchEvent.bind(target)});
    // No DOM image sources here; context loss events are timer tasks.
    offscreenCanvasBridge.bindWebGL(undefined,fn=>timer(fn,0,[],false));
    /* @include js_wasm.js */
    /* IndexedDB is the Window's (js_indexeddb.js over js_indexeddb.c): the same storage owner and
       storage key, its connections meeting the Window's in the profile's connection table. Its
       tasks are this worker's timers and browserjsworker.c's completion tasks. */
    /* @include js_indexeddb.js */
    // The global object is a WorkerGlobalScope: its interface objects are not
    // constructible, and the chain runs through EventTarget.prototype. The
    // concrete scope (dedicated or shared) is fixed when the host starts it.
    function scopeInterface(name,parent){
        const ctor=({[name]:function(){throw new TypeError('Illegal constructor');}})[name];
        Object.setPrototypeOf(ctor,parent);
        Object.defineProperty(ctor,'prototype',{value:Object.create(parent.prototype,{
            constructor:{value:ctor,writable:true,configurable:true},[Symbol.toStringTag]:{value:name,configurable:true}}),writable:false});
        return ctor;
    }
    const WorkerGlobalScope=scopeInterface('WorkerGlobalScope',EventTarget);
    const DedicatedWorkerGlobalScope=scopeInterface('DedicatedWorkerGlobalScope',WorkerGlobalScope);
    const SharedWorkerGlobalScope=scopeInterface('SharedWorkerGlobalScope',WorkerGlobalScope);
    Object.defineProperty(globalThis,'WorkerGlobalScope',{value:WorkerGlobalScope,writable:true,configurable:true});
    function installScope(isShared){
        const scope=isShared?SharedWorkerGlobalScope:DedicatedWorkerGlobalScope;
        Object.defineProperty(globalThis,scope.name,{value:scope,writable:true,configurable:true});
        Object.setPrototypeOf(globalThis,scope.prototype);
    }
    Object.defineProperties(globalThis,{name:{get:()=>workerName},onconnect:{get:()=>target.onconnect,set:fn=>target.onconnect=fn},onmessage:{get:()=>target.onmessage,set:fn=>target.onmessage=fn},onmessageerror:{get:()=>target.onmessageerror,set:fn=>target.onmessageerror=fn},onerror:{get:()=>target.onerror,set:fn=>target.onerror=fn},onlanguagechange:{get:()=>target.onlanguagechange,set:fn=>target.onlanguagechange=fn},onunhandledrejection:{get:()=>target.onunhandledrejection,set:fn=>target.onunhandledrejection=fn},onrejectionhandled:{get:()=>target.onrejectionhandled,set:fn=>target.onrejectionhandled=fn},onsecuritypolicyviolation:{get:()=>target.onsecuritypolicyviolation,set:fn=>target.onsecuritypolicyviolation=fn}});
    let rejectionBridge=null;
    const rejections=()=>rejectionBridge??=promiseRejectionBridge(event=>dispatchNativeWorkerEvent(target,event),host.promiseIsHandled,fn=>timer(fn,0,[],false));
    /* The console report is the default action of unhandledrejection. */
    function describeRejection(reason){
        let text;try{text=String(reason);}catch(_){text='JavaScript exception';}
        try{const stack=reason instanceof Error?reason.stack:undefined;if(typeof stack==='string'&&stack)text+='\n'+stack;}catch(_){}
        return text;
    }
    return {
        promiseRejection(promise,reason){if(!closed&&rejections().unhandled(promise,reason)&&!closed)host.send(7,0,{message:'Uncaught (in promise) '+describeRejection(reason),filename:address});},
        promiseRejectionHandled(promise,reason){if(!closed)rejections().handled(promise,reason);},
        safetyCheck:htmlSafetyBridge.check,
        safetyDynamicCode:htmlSafetyBridge.dynamicCode,
        safetyViolation(init){if(!closed)dispatchNativeWorkerEvent(target,htmlSafetyBridge.violationEvent(init));},
        languageChange(){if(languagesChanged()&&!closed)dispatchNativeWorkerEvent(target,new Event('languagechange'));},
        start(r){address=r[1];workerName=String(r[5]||"");workerCanCancel=r[6]===true;shared=r[9]===true;workerStreaming=r[13]===true;installScope(shared);if(shared)delete globalThis.postMessage;if(r[4]||r[0]<200||r[0]>=300)throw new DOMException(r[4]||'Worker source failed','NetworkError');const locationURL=new URL(address);Object.defineProperty(globalThis,'location',{value:Object.freeze({href:address,origin:locationURL.origin,protocol:locationURL.protocol,host:locationURL.host,hostname:locationURL.hostname,port:locationURL.port,pathname:locationURL.pathname,search:locationURL.search,hash:locationURL.hash,toString(){return address;}}),configurable:true});if(!formDataBridge)formDataBridge=initializeWorkerFormData();host.webidl(shared?'SharedWorkerGlobalScope':'DedicatedWorkerGlobalScope');evalSource(new TextDecoder().decode(r[3]),address);},
        receive(packet,op=2,id=0,kind=0){try{if(op===8||op===9||op===10){workerMessagingBridge.receivePort(id,kind,packet,op!==8);return;}const payload=kind===3?workerMessagingBridge.importExternal(packet):{data:cloneData.deserialize(packet),ports:[]};if(!closed){if(op===13){if(!shared||payload.ports.length!==1)throw new TypeError('Invalid shared worker connection');dispatchNativeWorkerEvent(target,new MessageEvent('connect',{data:'',source:payload.ports[0],ports:payload.ports}));}else dispatchNativeWorkerEvent(target,new MessageEvent('message',{data:payload.data,ports:payload.ports}));}}catch(e){if(!closed)dispatchNativeWorkerEvent(target,new MessageEvent('messageerror'));}},
        loaded(id,r,flags=0){workerFetchTransport.loaded(id,r,flags);},
        frame(id,value,kind){workerFetchTransport.frame(id,value,kind);},
        tick(now){if(closed)return false;if(cryptoTaskReady()&&!lastCryptoTask)return runCryptoTask();lastCryptoTask=false;const ports=workerMessagingBridge.ready();if(ports&&!lastPortTask){lastPortTask=true;return workerMessagingBridge.runOne();}const t=timerHeap[0];if(t&&t.due<=now){lastPortTask=false;const level=t.nesting,outer=timerNesting;if(t.repeat){t.due=now+(level>5&&t.delay<4?4:t.delay);t.nesting=level>5?level:level+1;if(timerOrder<9007199254740991)t.order=++timerOrder;timerFix(0);}else clearTimer(t.id);timerNesting=t.html?level:0;try{t.fn(...t.args);}catch(e){report(e);}finally{timerNesting=outer;}return true;}lastPortTask=ports;return ports?workerMessagingBridge.runOne():cryptoTaskReady()?runCryptoTask():false;},
        deadline(){if(workerMessagingBridge.ready()||cryptoTaskReady())return host.now();return timerHeap.length?timerHeap[0].due:-1;},report,
        storageOrigin:urlBridge.storageOrigin,
        indexedDBTaskEnd:indexedDBBridge.taskEnd
    };
})(__workerHost);
