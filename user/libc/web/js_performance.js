/* User Timing: real document-relative timestamps, stored entries, and queued
 * observer delivery. Include AFTER js_clone.js; metadata uses its private
 * structured serializer, not a replaceable page function. Legacy navigation
 * timing exposes recorded host/parser/event milestones. Unobserved transport
 * milestones stay 0; navigation entries/paint/event are not advertised.
 * Resource Timing entries come from native request completion (Window only).
 * Shared with Worker realms: without host.timing a realm has no navigation
 * timing, no reserved mark names, and its time origin is its creation. */
const performanceBridge = (() => {
    const call=Reflect.apply,define=Object.defineProperty,create=Object.create;
    const StringType=String,TypeErr=TypeError,DomError=DOMException;
    const isWindow=typeof host.timing==="function",hostNow=host.now,base=isWindow?0:hostNow();
    const finite=Number.isFinite,floor=Math.floor,now=isWindow?hostNow:()=>hostNow()-base,navigationTime=host.timing,dateNow=Date.now;
    const serialize=cloneData.serialize,deserialize=cloneData.deserialize;
    const entries=new WeakMap(),observers=new WeakMap(),lists=new WeakMap(),performances=new WeakMap(),timings=new WeakMap(),fullHandlers=new WeakMap();
    const getWeak=WeakMap.prototype.get,setWeak=WeakMap.prototype.set;
    const get=(map,value)=>call(getWeak,map,[value]),put=(map,key,value)=>call(setWeak,map,[key,value]);
    // These interfaces have no [Serializable] annotation. Private slots, not
    // mutable prototypes or instanceof hooks, identify even nested metadata.
    cloneData.registerUncloneable(value=>get(entries,value)!==undefined||get(observers,value)!==undefined||
        get(lists,value)!==undefined||get(performances,value)!==undefined||get(timings,value)!==undefined);
    const push=Array.prototype.push,slice=Array.prototype.slice,sort=Array.prototype.sort;
    const append=(a,x)=>call(push,a,[x]),copy=a=>call(slice,a,[]);
    const active=new Set(),setAdd=Set.prototype.add,setDelete=Set.prototype.delete;
    const eachSet=Set.prototype.forEach;
    const activeList=()=>{const a=[];call(eachSet,active,[x=>append(a,x)]);return a;};
    const token={},supported=Object.freeze(isWindow?['mark','measure','resource','visibility-state']:['mark','measure']);
    const observable=type=>type==='mark'||type==='measure'||isWindow&&(type==='resource'||type==='visibility-state');
    const timingNames=('navigationStart unloadEventStart unloadEventEnd redirectStart redirectEnd '+
        'fetchStart domainLookupStart domainLookupEnd connectStart connectEnd secureConnectionStart '+
        'requestStart responseStart responseEnd domLoading domInteractive domContentLoadedEventStart '+
        'domContentLoadedEventEnd domComplete loadEventStart loadEventEnd').split(' ');
    const reserved=new Set(isWindow?timingNames:[]);
    const setHas=Set.prototype.has;
    let buffer=[],sequence=0,notificationPending=false;
    // Native monotonic epoch is host navigation start, or explicit document init.
    // Pair it once with the real OS wall clock; never rebase it after wall jumps.
    const before=now(),wall=dateNow(),after=now();
    const origin=wall-(before+after)/2;
    const schedule=isWindow?setTimeout:(fn,ms)=>host.timer(0,fn,ms,[]);
    function slot(map,value){const s=get(map,value);if(!s)throw new TypeErr('Illegal invocation');return s;}
    function string(value){if(typeof value==='symbol')throw new TypeErr('Cannot convert a Symbol to a string');return StringType(value);}
    function number(value){const n=+value;if(!finite(n))throw new TypeErr('Expected a finite timestamp');return n;}
    function dictionary(value){
        if(value==null)return {};
        if(typeof value!=='object'&&typeof value!=='function')throw new TypeErr('Expected a dictionary');
        return value;
    }
    function union(value){return typeof value==='number'?number(value):string(value);}
    function metadata(value){return value===undefined||value===null?null:deserialize(serialize(value));}
    function markOptions(value){
        const o=dictionary(value),detail=o.detail,startTime=o.startTime;
        return {detail,startTime:startTime===undefined?undefined:number(startTime)};
    }
    function measureOptions(value){
        // Web IDL dictionary members are read once, in lexicographic order.
        const o=dictionary(value),detail=o.detail;
        const d=o.duration,duration=d===undefined?undefined:number(d);
        const e=o.end,end=e===undefined?undefined:union(e);
        const s=o.start,start=s===undefined?undefined:union(s);
        return {detail,duration,end,start};
    }
    function selected(source,name,type){
        const result=[];
        for(let i=0;i<source.length;i++){
            const entry=source[i],s=slot(entries,entry);
            if(name!==undefined&&s.name!==name||type!==undefined&&s.entryType!==type)continue;
            append(result,entry);
        }
        return call(sort,result,[(a,b)=>{const x=get(entries,a),y=get(entries,b);return x.startTime-y.startTime||x.sequence-y.sequence;}]);
    }
    function timestamp(mark){
        if(typeof mark==='number'){if(mark<0)throw new TypeErr('A timestamp cannot be negative');return mark;}
        if(call(setHas,reserved,[mark])){
            const measured=navigationTime(mark);
            // Null, not a timestamp of 0, means native code never observed it.
            if(measured===null)throw new DomError('Navigation timestamp has not been recorded','InvalidAccessError');
            return measured;
        }
        for(let i=buffer.length-1;i>=0;i--){const s=get(entries,buffer[i]);if(s.entryType==='mark'&&s.name===mark)return s.startTime;}
        throw new DomError('The named performance mark does not exist','SyntaxError');
    }
    class PerformanceEntry {
        constructor(key){if(key!==token)throw new TypeErr('Illegal constructor');}
        get name(){return slot(entries,this).name;}
        get entryType(){return slot(entries,this).entryType;}
        get startTime(){return slot(entries,this).startTime;}
        get duration(){return slot(entries,this).duration;}
        toJSON(){const s=slot(entries,this);return {name:s.name,entryType:s.entryType,startTime:s.startTime,duration:s.duration,detail:s.detail};}
    }
    class PerformanceMark extends PerformanceEntry {
        constructor(markName,options={}){
            if(!arguments.length)throw new TypeErr('A mark name is required');
            const name=string(markName),o=markOptions(options);
            if(call(setHas,reserved,[name]))throw new DomError('Reserved navigation timing name','SyntaxError');
            if(o.startTime!==undefined&&o.startTime<0)throw new TypeErr('A timestamp cannot be negative');
            const startTime=o.startTime===undefined?now():o.startTime;
            super(token);put(entries,this,{name,entryType:'mark',startTime,duration:0,detail:metadata(o.detail),sequence:0});
        }
        get detail(){const s=slot(entries,this);if(s.entryType!=='mark')throw new TypeErr('Illegal invocation');return s.detail;}
    }
    class PerformanceMeasure extends PerformanceEntry {
        constructor(key){super(key);if(key!==token)throw new TypeErr('Illegal constructor');}
        get detail(){const s=slot(entries,this);if(s.entryType!=='measure')throw new TypeErr('Illegal invocation');return s.detail;}
    }
    // Resource Timing. Native code reports each completed HTTP(S) request with
    // times relative to the time origin; without a passing timing allow check
    // the detailed times and sizes are 0, as for a cross-origin resource.
    const resourceNames=['initiatorType','deliveryType','nextHopProtocol','workerStart','redirectStart','redirectEnd',
        'fetchStart','domainLookupStart','domainLookupEnd','connectStart','connectEnd','secureConnectionStart',
        'requestStart','firstInterimResponseStart','responseStart','responseEnd','transferSize','encodedBodySize',
        'decodedBodySize','responseStatus','renderBlockingStatus','contentType'];
    class PerformanceResourceTiming extends PerformanceEntry {
        constructor(key){super(key);if(key!==token)throw new TypeErr('Illegal constructor');}
        get serverTiming(){const s=slot(entries,this);if(s.entryType!=='resource')throw new TypeErr('Illegal invocation');return [];}
        toJSON(){
            const s=slot(entries,this);if(s.entryType!=='resource')throw new TypeErr('Illegal invocation');
            const result={name:s.name,entryType:s.entryType,startTime:s.startTime,duration:s.duration};
            for(let i=0;i<resourceNames.length;i++)result[resourceNames[i]]=s[resourceNames[i]];
            result.serverTiming=[];return result;
        }
    }
    for(let i=0;i<resourceNames.length;i++){
        const name=resourceNames[i];
        define(PerformanceResourceTiming.prototype,name,{configurable:true,get(){
            const s=slot(entries,this);if(s.entryType!=='resource')throw new TypeErr('Illegal invocation');return s[name];
        }});
    }
    // HTML 6.2 VisibilityStateEntry (Window only): one per "update the visibility state" of the
    // document, and one for its initial state at time 0. Its registry entry keeps at most 50 in
    // the performance timeline (maxBufferSize); observers still receive the rest.
    class VisibilityStateEntry extends PerformanceEntry {
        constructor(key){super(key);if(key!==token)throw new TypeErr('Illegal constructor');}
        get name(){const s=slot(entries,this);if(s.entryType!=='visibility-state')throw new TypeErr('Illegal invocation');return s.name;}
        get entryType(){const s=slot(entries,this);if(s.entryType!=='visibility-state')throw new TypeErr('Illegal invocation');return s.entryType;}
        get startTime(){const s=slot(entries,this);if(s.entryType!=='visibility-state')throw new TypeErr('Illegal invocation');return s.startTime;}
        get duration(){const s=slot(entries,this);if(s.entryType!=='visibility-state')throw new TypeErr('Illegal invocation');return 0;}
    }
    const visibilityLimit=50;let visibilityCount=0;
    function visibility(state,startTime){
        const entry=create(VisibilityStateEntry.prototype);
        put(entries,entry,{name:string(state),entryType:'visibility-state',startTime,duration:0,sequence:0});
        const fits=visibilityCount<visibilityLimit;if(fits)visibilityCount++;
        record(entry,fits);
    }
    // fetch() and XMLHttpRequest share the native fetch transport.
    const initiators=new Map(),mapGet=Map.prototype.get,mapSet=Map.prototype.set,mapDelete=Map.prototype.delete;
    const initiatorKinds=['script','fetch','link','img','other','iframe','css','other','script'];
    let resourceLimit=250,resourceCount=0,resourceSecondary=[],resourceFullPending=false,performanceObject=null;
    function bufferFull(){
        resourceFullPending=false;
        try{performanceObject.dispatchEvent(new Event('resourcetimingbufferfull'));}catch(e){report(e);}
        const waiting=resourceSecondary;resourceSecondary=[];
        for(let i=0;i<waiting.length&&resourceCount<resourceLimit;i++){append(buffer,waiting[i]);resourceCount++;}
    }
    function resource(id,kind,url,start,responseStart,end,bodyBytes,headerBytes,status,contentType,allowed){
        const initiatorType=call(mapGet,initiators,[id])||initiatorKinds[kind]||'other';call(mapDelete,initiators,[id]);
        const detailed=t=>allowed?t:0;
        const entry=create(PerformanceResourceTiming.prototype);
        put(entries,entry,{name:url,entryType:'resource',startTime:start,duration:end-start,sequence:0,
            initiatorType,deliveryType:'',nextHopProtocol:allowed?'http/1.1':'',workerStart:0,redirectStart:0,redirectEnd:0,
            fetchStart:start,domainLookupStart:detailed(start),domainLookupEnd:detailed(start),connectStart:detailed(start),
            connectEnd:detailed(start),secureConnectionStart:allowed&&url.startsWith('https:')?start:0,requestStart:detailed(start),
            firstInterimResponseStart:0,responseStart:detailed(responseStart),responseEnd:end,
            transferSize:detailed(bodyBytes+headerBytes),encodedBodySize:detailed(bodyBytes),decodedBodySize:detailed(bodyBytes),
            responseStatus:status,renderBlockingStatus:'non-blocking',contentType});
        const fits=resourceCount<resourceLimit&&!resourceSecondary.length;
        record(entry,fits);
        if(fits){resourceCount++;return;}
        append(resourceSecondary,entry);
        if(!resourceFullPending){resourceFullPending=true;try{schedule(bufferFull,0);}catch(e){resourceFullPending=false;report(e);}}
    }
    function queueNotification(){
        if(notificationPending)return;
        notificationPending=true;
        try{schedule(notify,0);}catch(e){notificationPending=false;report(e);}
    }
    function record(entry,buffered=true){
        const s=get(entries,entry);s.sequence=++sequence;if(buffered)append(buffer,entry);
        const registered=activeList();let interested=false;
        for(let i=0;i<registered.length;i++){
            const o=get(observers,registered[i]);
            if(call(setHas,o.types,[s.entryType])){append(o.buffer,entry);interested=true;}
        }
        if(interested)queueNotification();return entry;
    }
    function clear(type,name){
        const retained=[];
        for(let i=0;i<buffer.length;i++){const e=buffer[i],s=get(entries,e);if(s.entryType!==type||name!==undefined&&s.name!==name)append(retained,e);}
        buffer=retained;
    }
    class PerformanceTiming {
        constructor(){throw new TypeErr('Illegal constructor');}
        toJSON(){
            slot(timings,this);const result={};
            for(let i=0;i<timingNames.length;i++){
                const name=timingNames[i],measured=navigationTime(name);
                result[name]=measured===null?0:floor(origin+measured);
            }
            return result;
        }
    }
    for(let i=0;i<timingNames.length;i++){
        const name=timingNames[i];
        define(PerformanceTiming.prototype,name,{enumerable:true,configurable:true,get(){
            slot(timings,this);const measured=navigationTime(name);
            return measured===null?0:floor(origin+measured);
        }});
    }
    const legacyTiming=create(PerformanceTiming.prototype);put(timings,legacyTiming,true);
    class Performance extends EventTarget {
        constructor(key){super();if(key!==token)throw new TypeErr('Illegal constructor');put(performances,this,true);}
        now(){slot(performances,this);return now();}
        get timeOrigin(){slot(performances,this);return origin;}
        get timing(){slot(performances,this);return legacyTiming;}
        toJSON(){slot(performances,this);return {timeOrigin:origin};}
        mark(name,options={}){
            slot(performances,this);if(!arguments.length)throw new TypeErr('A mark name is required');
            return record(new PerformanceMark(name,options));
        }
        measure(name,startOrOptions={},endMark){
            slot(performances,this);if(!arguments.length)throw new TypeErr('A measure name is required');
            name=string(name);
            const isOptions=startOrOptions===null||typeof startOrOptions==='object'||typeof startOrOptions==='function';
            const o=isOptions?measureOptions(startOrOptions):string(startOrOptions);
            const end=endMark===undefined?undefined:string(endMark);
            if(isOptions&&(o.start!==undefined||o.end!==undefined||o.duration!==undefined||o.detail!==undefined)){
                if(end!==undefined||o.start===undefined&&o.end===undefined||o.start!==undefined&&o.end!==undefined&&o.duration!==undefined)
                    throw new TypeErr('Invalid performance measure options');
            }
            let endTime,startTime;
            if(end!==undefined)endTime=timestamp(end);
            else if(isOptions&&o.end!==undefined)endTime=timestamp(o.end);
            else if(isOptions&&o.start!==undefined&&o.duration!==undefined)endTime=timestamp(o.start)+timestamp(o.duration);
            else endTime=now();
            if(isOptions&&o.start!==undefined)startTime=timestamp(o.start);
            else if(isOptions&&o.duration!==undefined&&o.end!==undefined)startTime=timestamp(o.end)-timestamp(o.duration);
            else startTime=isOptions?0:timestamp(o);
            const entry=create(PerformanceMeasure.prototype);
            put(entries,entry,{name,entryType:'measure',startTime,duration:endTime-startTime,detail:isOptions?metadata(o.detail):null,sequence:0});
            return record(entry);
        }
        clearMarks(name){slot(performances,this);clear('mark',name===undefined?undefined:string(name));}
        clearMeasures(name){slot(performances,this);clear('measure',name===undefined?undefined:string(name));}
        // Clear only the resource partition of the actual private timeline.
        // Marks, measures and previously queued observer records survive.
        clearResourceTimings(){slot(performances,this);clear('resource');resourceCount=0;}
        setResourceTimingBufferSize(maxSize){
            slot(performances,this);if(!arguments.length)throw new TypeErr('A buffer size is required');
            const n=+maxSize;resourceLimit=finite(n)?floor(n)>>>0:0;
        }
        get onresourcetimingbufferfull(){slot(performances,this);return get(fullHandlers,this)||null;}
        set onresourcetimingbufferfull(value){
            slot(performances,this);const previous=get(fullHandlers,this);
            if(previous)this.removeEventListener('resourcetimingbufferfull',previous);
            const handler=typeof value==='function'||(value&&typeof value==='object')?value:null;
            put(fullHandlers,this,handler);if(handler)this.addEventListener('resourcetimingbufferfull',handler);
        }
        getEntries(){slot(performances,this);return selected(buffer);}
        getEntriesByType(type){slot(performances,this);if(!arguments.length)throw new TypeErr('An entry type is required');return selected(buffer,undefined,string(type));}
        getEntriesByName(name,type){slot(performances,this);if(!arguments.length)throw new TypeErr('An entry name is required');return selected(buffer,string(name),type===undefined?undefined:string(type));}
    }
    class PerformanceObserverEntryList {
        constructor(){throw new TypeErr('Illegal constructor');}
        getEntries(){return selected(slot(lists,this));}
        getEntriesByType(type){const a=slot(lists,this);if(!arguments.length)throw new TypeErr('An entry type is required');return selected(a,undefined,string(type));}
        getEntriesByName(name,type){const a=slot(lists,this);if(!arguments.length)throw new TypeErr('An entry name is required');return selected(a,string(name),type===undefined?undefined:string(type));}
    }
    class PerformanceObserver {
        constructor(callback){
            if(typeof callback!=='function')throw new TypeErr('An observer callback is required');
            put(observers,this,{callback,mode:undefined,types:new Set(),buffer:[],requiresDropped:false});
        }
        static get supportedEntryTypes(){return supported;}
        observe(options={}){
            const s=slot(observers,this),o=dictionary(options);
            const b=o.buffered,buffered=b===undefined?undefined:!!b;
            const e=o.entryTypes;let types;
            if(e!==undefined){
                if(e===null||(typeof e!=='object'&&typeof e!=='function'))throw new TypeErr('Expected an entry type sequence');
                const method=e[Symbol.iterator];if(typeof method!=='function')throw new TypeErr('Expected an iterable entry type sequence');
                types=[];for(const type of {[Symbol.iterator]:()=>call(method,e,[])})append(types,string(type));
            }
            const t=o.type,type=t===undefined?undefined:string(t);
            if(types===undefined&&type===undefined)throw new TypeErr('An entry type is required');
            if(types!==undefined&&(type!==undefined||buffered!==undefined))throw new TypeErr('entryTypes cannot be combined with type or buffered');
            const mode=types===undefined?'single':'multiple';
            if(s.mode!==undefined&&s.mode!==mode)throw new DomError('Cannot change the observer registration mode','InvalidModificationError');
            s.mode=mode;s.requiresDropped=true;
            if(mode==='multiple'){
                const accepted=new Set();
                for(let i=0;i<types.length;i++)if(observable(types[i]))call(setAdd,accepted,[types[i]]);
                if(!accepted.size)return;s.types=accepted;
            }else{
                if(!observable(type))return;
                call(setAdd,s.types,[type]);
            }
            call(setAdd,active,[this]);
            if(buffered){
                for(let i=0;i<buffer.length;i++)if(get(entries,buffer[i]).entryType===type)append(s.buffer,buffer[i]);
                queueNotification();
            }
        }
        disconnect(){const s=slot(observers,this);call(setDelete,active,[this]);s.types=new Set();s.buffer=[];}
        takeRecords(){const s=slot(observers,this),a=copy(s.buffer);s.buffer=[];return a;}
    }
    function notify(){
        notificationPending=false;
        const registered=activeList();
        for(let i=0;i<registered.length;i++){
            const observer=registered[i],s=get(observers,observer);
            if(!s.buffer.length)continue;
            const batch=s.buffer;s.buffer=[];
            const list=create(PerformanceObserverEntryList.prototype);put(lists,list,batch);
            const options=s.requiresDropped?{droppedEntriesCount:0}:{};s.requiresDropped=false;
            try{call(s.callback,observer,[list,observer,options]);}catch(e){report(e);}
        }
    }
    for(const ctor of [Performance,PerformanceTiming,PerformanceEntry,PerformanceMark,PerformanceMeasure,PerformanceResourceTiming,VisibilityStateEntry,PerformanceObserver,PerformanceObserverEntryList]){
        define(ctor.prototype,Symbol.toStringTag,{value:ctor.name,configurable:true});
        for(const key of Object.getOwnPropertyNames(ctor.prototype))if(key!=='constructor'){
            const d=Object.getOwnPropertyDescriptor(ctor.prototype,key);d.enumerable=true;define(ctor.prototype,key,d);
        }
    }
    for(const ctor of [Performance,PerformanceEntry,PerformanceMeasure,PerformanceResourceTiming,VisibilityStateEntry])define(ctor,'length',{value:0,configurable:true});
    for(const key of ['clearMarks','clearMeasures'])define(Performance.prototype[key],'length',{value:0,configurable:true});
    define(Performance.prototype.getEntriesByName,'length',{value:1,configurable:true});
    define(PerformanceObserverEntryList.prototype.getEntriesByName,'length',{value:1,configurable:true});
    const supportedDescriptor=Object.getOwnPropertyDescriptor(PerformanceObserver,'supportedEntryTypes');
    supportedDescriptor.enumerable=true;define(PerformanceObserver,'supportedEntryTypes',supportedDescriptor);
    // Navigation timing (Performance.timing, PerformanceTiming) is Window-only.
    if(isWindow)globalThis.PerformanceTiming=PerformanceTiming;else delete Performance.prototype.timing;
    performanceObject=new Performance(token);
    Object.assign(globalThis,{Performance,PerformanceEntry,PerformanceMark,PerformanceMeasure,PerformanceResourceTiming,PerformanceObserver,PerformanceObserverEntryList,performance:performanceObject});
    // HTML "set the initial visibility state" queues the first entry with timestamp 0.
    if(isWindow&&typeof host.visible==='function'){globalThis.VisibilityStateEntry=VisibilityStateEntry;visibility(host.visible()?'visible':'hidden',0);}
    // An aborted request never completes; its registration is dropped with the
    // oldest rather than kept for the document's lifetime.
    return {resource,visibility,initiator(id,type){
        if(initiators.size>=4096)for(const key of initiators.keys()){call(mapDelete,initiators,[key]);break;}
        call(mapSet,initiators,[id,type]);
    }};
})();
