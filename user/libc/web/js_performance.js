/* The performance timeline (Performance Timeline, HR Time, User Timing,
 * Resource Timing, Navigation Timing, Paint Timing, Server Timing, HTML's
 * VisibilityStateEntry): entries, their buffers and queued observer delivery.
 * Include AFTER js_clone.js; metadata uses its private structured serializer,
 * not a replaceable page function.
 * Times come from native code: the transport's timing record of each request
 * (js.c timing_value) for resource and navigation entries, the document's
 * recorded milestones (host.timing) for its load and for the legacy
 * PerformanceTiming, the paint walk for paint entries.
 * Shared with Worker realms: without host.timing a realm has no navigation
 * timing, no reserved mark names, and its time origin is its creation. */
const performanceBridge = (() => {
    const call=Reflect.apply,define=Object.defineProperty,create=Object.create,freeze=Object.freeze;
    const StringType=String,TypeErr=TypeError,DomError=DOMException;
    const isWindow=typeof host.timing==="function",hostNow=host.now,base=isWindow?0:hostNow();
    const finite=Number.isFinite,floor=Math.floor,random=Math.random,now=isWindow?hostNow:()=>hostNow()-base,navigationTime=host.timing,dateNow=Date.now;
    const serialize=cloneData.serialize,deserialize=cloneData.deserialize;
    const entries=new WeakMap(),observers=new WeakMap(),lists=new WeakMap(),performances=new WeakMap(),timings=new WeakMap(),fullHandlers=new WeakMap();
    const getWeak=WeakMap.prototype.get,setWeak=WeakMap.prototype.set;
    const get=(map,value)=>call(getWeak,map,[value]),put=(map,key,value)=>call(setWeak,map,[key,value]);
    // These interfaces have no [Serializable] annotation. Private slots, not
    // mutable prototypes or instanceof hooks, identify even nested metadata.
    cloneData.registerUncloneable(value=>get(entries,value)!==undefined||get(observers,value)!==undefined||
        get(lists,value)!==undefined||get(performances,value)!==undefined||get(timings,value)!==undefined);
    const push=Array.prototype.push,slice=Array.prototype.slice,sort=Array.prototype.sort,splice=Array.prototype.splice;
    const append=(a,x)=>call(push,a,[x]),copy=a=>call(slice,a,[]);
    const active=new Set(),setAdd=Set.prototype.add,setDelete=Set.prototype.delete,setHas=Set.prototype.has;
    const eachSet=Set.prototype.forEach;
    const activeList=()=>{const a=[];call(eachSet,active,[x=>append(a,x)]);return a;};
    const token={};
    // The registry of entry types this global supports (alphabetical), each with its
    // performance entry buffer: maxBufferSize (Infinity for marks and measures), its
    // entries and the dropped entries count. The resource buffer follows Resource
    // Timing's own size limit and secondary buffer. A Worker has Resource Timing too
    // (PerformanceResourceTiming is [Exposed=(Window,Worker)]): its own requests' entries.
    const registry=isWindow?{mark:Infinity,measure:Infinity,navigation:1,paint:2,resource:Infinity,'visibility-state':50}:{mark:Infinity,measure:Infinity,resource:Infinity};
    const typeNames=Object.keys(registry);call(sort,typeNames,[]);
    const supported=freeze(typeNames);
    const tuples=new Map(),mapGet=Map.prototype.get,mapSet=Map.prototype.set,mapDelete=Map.prototype.delete;
    for(let i=0;i<typeNames.length;i++)call(mapSet,tuples,[typeNames[i],{buffer:[],max:registry[typeNames[i]],dropped:0}]);
    const tupleOf=type=>call(mapGet,tuples,[type]);
    const observable=type=>tupleOf(type)!==undefined;
    const timingNames=('navigationStart unloadEventStart unloadEventEnd redirectStart redirectEnd '+
        'fetchStart domainLookupStart domainLookupEnd connectStart connectEnd secureConnectionStart '+
        'requestStart responseStart responseEnd domLoading domInteractive domContentLoadedEventStart '+
        'domContentLoadedEventEnd domComplete loadEventStart loadEventEnd').split(' ');
    const reserved=new Set(isWindow?timingNames:[]);
    let sequence=0,notificationPending=false;
    // Each global's last performance entry id starts at a random integer in
    // [100, 10000]; each new id adds a small random amount (Performance Timeline).
    let lastEntryId=100+floor(random()*9901),mostRecentNavigation=null;
    const nextEntryId=()=>(lastEntryId+=1+floor(random()*10));
    // Native monotonic epoch is host navigation start, or explicit document init.
    // Pair it once with the real OS wall clock; never rebase it after wall jumps.
    const before=now(),wall=dateNow(),after=now();
    const origin=wall-(before+after)/2;
    const schedule=isWindow?setTimeout:(fn,ms)=>host.timer(0,fn,ms,[]);
    function slot(map,value){const s=get(map,value);if(!s)throw new TypeErr('Illegal invocation');return s;}
    function kind(value,type){const s=slot(entries,value);if(s.entryType!==type)throw new TypeErr('Illegal invocation');return s;}
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
    const chronological=(a,b)=>{const x=get(entries,a),y=get(entries,b);return x.startTime-y.startTime||x.sequence-y.sequence;};
    function selected(source,name,type){
        const result=[];
        for(let i=0;i<source.length;i++){
            const entry=source[i],s=slot(entries,entry);
            if(name!==undefined&&s.name!==name||type!==undefined&&s.entryType!==type)continue;
            append(result,entry);
        }
        return call(sort,result,[chronological]);
    }
    // "Filter buffer map by name and type": one type's buffer, or every buffer.
    function timeline(name,type){
        if(type!==undefined){const t=tupleOf(type);return t?selected(t.buffer,name,type):[];}
        const all=[];
        for(let i=0;i<typeNames.length;i++){const b=tupleOf(typeNames[i]).buffer;for(let j=0;j<b.length;j++)append(all,b[j]);}
        return selected(all,name,undefined);
    }
    // User Timing "convert a name to a timestamp": the legacy PerformanceTiming
    // value of a navigation timing name, relative to navigationStart.
    function timestamp(mark){
        if(typeof mark==='number'){if(mark<0)throw new TypeErr('A timestamp cannot be negative');return mark;}
        if(call(setHas,reserved,[mark])){
            const measured=navigationTime(mark);
            // Null, not a timestamp of 0, means native code never observed it.
            if(measured===null)throw new DomError('Navigation timestamp has not been recorded','InvalidAccessError');
            return measured;
        }
        const marks=tupleOf('mark').buffer;
        for(let i=marks.length-1;i>=0;i--){const s=get(entries,marks[i]);if(s.name===mark)return s.startTime;}
        throw new DomError('The named performance mark does not exist','SyntaxError');
    }
    // [Default] toJSON: the attributes of each interface in the chain that declares it.
    function json(entry,members){
        const result={};
        for(let i=0;i<members.length;i++)result[members[i][0]]=call(members[i][1],entry,[]);
        return result;
    }
    const entryMembers=[];
    class PerformanceEntry {
        constructor(key){if(key!==token)throw new TypeErr('Illegal constructor');}
        get id(){return slot(entries,this).id;}
        get name(){return slot(entries,this).name;}
        get entryType(){return slot(entries,this).entryType;}
        get startTime(){return slot(entries,this).startTime;}
        get duration(){const s=slot(entries,this);return typeof s.duration==='function'?s.duration():s.duration;}
        // An unqueued or document-less entry's null converts to the IDL unsigned long long 0.
        get navigationId(){const id=slot(entries,this).navigationId;return id===null?0:id;}
        toJSON(){slot(entries,this);return json(this,entryMembers);}
    }
    for(const name of ['id','name','entryType','startTime','duration','navigationId'])
        append(entryMembers,[name,Object.getOwnPropertyDescriptor(PerformanceEntry.prototype,name).get]);
    function entrySlot(fields){fields.id=0;fields.navigationId=null;fields.sequence=0;return fields;}
    class PerformanceMark extends PerformanceEntry {
        constructor(markName,options={}){
            if(!arguments.length)throw new TypeErr('A mark name is required');
            const name=string(markName),o=markOptions(options);
            if(call(setHas,reserved,[name]))throw new DomError('Reserved navigation timing name','SyntaxError');
            if(o.startTime!==undefined&&o.startTime<0)throw new TypeErr('A timestamp cannot be negative');
            const startTime=o.startTime===undefined?now():o.startTime;
            super(token);put(entries,this,entrySlot({name,entryType:'mark',startTime,duration:0,detail:metadata(o.detail)}));
        }
        get detail(){return kind(this,'mark').detail;}
    }
    class PerformanceMeasure extends PerformanceEntry {
        constructor(key){super(key);if(key!==token)throw new TypeErr('Illegal constructor');}
        get detail(){return kind(this,'measure').detail;}
    }
    // Server Timing: the metrics of a resource's Server-Timing values.
    class PerformanceServerTiming {
        constructor(){throw new TypeErr('Illegal constructor');}
        get name(){return slot(timings,this).name;}
        get duration(){return slot(timings,this).duration;}
        get description(){return slot(timings,this).description;}
        toJSON(){slot(timings,this);return json(this,serverMembers);}
    }
    const serverMembers=['name','duration','description'].map(name=>[name,Object.getOwnPropertyDescriptor(PerformanceServerTiming.prototype,name).get]);
    const asciiSpace=c=>c===' '||c==='\t'||c==='\n'||c==='\r'||c==='\f';
    const tcharSymbols=create(null);
    for(const c of "!#$%&'*+-.^_`|~")tcharSymbols[c]=true;
    const tchar=c=>c!==undefined&&(c>='0'&&c<='9'||c>='A'&&c<='Z'||c>='a'&&c<='z'||tcharSymbols[c]===true);
    const asciiLower=create(null),upper='ABCDEFGHIJKLMNOPQRSTUVWXYZ',lowerLetters='abcdefghijklmnopqrstuvwxyz';
    for(let i=0;i<26;i++)asciiLower[upper[i]]=lowerLetters[i];
    // Server Timing §2: a field value is its ABNF's #server-timing-metric list,
    // read as the section requires of user agents: characters after a metric name
    // or after a parameter value are ignored up to the next ";" or ",", only a
    // parameter's first instance counts (an incomplete or invalid one included),
    // unknown parameters are ignored, and a parameter name, a token, matches ASCII
    // case-insensitively. A value that is neither a token nor a whole quoted-string
    // is empty. The list ends where a metric name is not a token. (The section's
    // step list never stores a parameter and re-reads a repeated one's value as a
    // name; WPT's server-timing parsing cases follow the ABNF and its rules.)
    // The transport joins repeated Server-Timing fields with ", ".
    function serverMetrics(field){
        const metrics=[];let at=0;
        const ows=()=>{while(field[at]===' '||field[at]==='\t')at++;};
        const token=()=>{let t='';while(tchar(field[at]))t+=field[at++];return t;};
        const lowerToken=()=>{let t='';while(tchar(field[at])){const c=field[at++];t+=asciiLower[c]||c;}return t;};
        const extraneous=()=>{while(at<field.length&&field[at]!==','&&field[at]!==';')at++;};
        // quoted-string: DQUOTE *( qdtext / quoted-pair ) DQUOTE, its value unescaped;
        // one without its closing quote is not a value.
        const quotedString=()=>{
            let value='';at++;
            while(at<field.length){
                const c=field[at++];
                if(c==='"')return value;
                if(c==='\\'){if(at>=field.length)break;value+=field[at++];}
                else value+=c;
            }
            return '';
        };
        for(;;){
            ows();
            const name=token();
            if(!name)break;
            const params=create(null);
            extraneous();
            while(field[at]===';'){
                at++;ows();
                const paramName=lowerToken();
                ows();
                let value='';
                if(field[at]==='='){at++;ows();value=field[at]==='"'?quotedString():token();}
                extraneous();
                if(paramName&&params[paramName]===undefined)params[paramName]=value;
            }
            const dur=params.dur,desc=params.desc;
            const metric=create(PerformanceServerTiming.prototype);
            put(timings,metric,{name,duration:dur===undefined?0:floatValue(dur),description:desc===undefined?'':desc});
            append(metrics,metric);
            ows();
            if(field[at]!==',')break;
            at++;
        }
        return metrics;
    }
    // HTML "rules for parsing floating-point number values"; an error is 0.
    // The digits it takes are rounded once to the nearest double, as the
    // rules' conversion step does, by the language's own decimal conversion.
    function floatValue(input){
        let position=0,sign='',integer='',fraction='',exponent='';
        const digit=c=>c!==undefined&&c>='0'&&c<='9';
        const digits=()=>{const start=position;while(digit(input[position]))position++;return input.slice(start,position);};
        while(position<input.length&&asciiSpace(input[position]))position++;
        if(position>=input.length)return 0;
        if(input[position]==='-'){sign='-';position++;if(position>=input.length)return 0;}
        else if(input[position]==='+'){position++;if(position>=input.length)return 0;}
        if(input[position]==='.'&&digit(input[position+1]))integer='0';
        else if(!digit(input[position]))return 0;
        else integer=digits();
        if(input[position]==='.'&&digit(input[position+1])){position++;fraction=digits();}
        if(input[position]==='e'||input[position]==='E'){
            let at=position+1,exponentSign='';
            if(input[at]==='-'||input[at]==='+')exponentSign=input[at++];
            if(digit(input[at])){position=at;exponent=exponentSign+digits();}
        }
        const value=+(sign+integer+(fraction?'.'+fraction:'')+(exponent?'e'+exponent:''));
        if(!finite(value))return 0;
        return value===0?0:value;
    }
    // Resource Timing. The transport's record (js.c timing_value) carries the
    // timing info as the realm sees it: the opaque one after a failed timing allow
    // check, an empty body info for an opaque response.
    const zeroTime=()=>0,emptyString=()=>'';
    const resourceGetters={
        initiatorType:s=>s.initiatorType,
        deliveryType:s=>s.record.cacheState?'cache':'',
        nextHopProtocol:s=>s.record.nextHopProtocol,
        workerStart:zeroTime, // no service worker ran this fetch
        redirectStart:s=>s.record.redirectStart,
        redirectEnd:s=>s.record.redirectEnd,
        fetchStart:s=>s.record.fetchStart,
        domainLookupStart:s=>s.record.domainLookupStart,
        domainLookupEnd:s=>s.record.domainLookupEnd,
        connectStart:s=>s.record.connectStart,
        connectEnd:s=>s.record.connectEnd,
        secureConnectionStart:s=>s.record.secureConnectionStart,
        requestStart:s=>s.record.requestStart,
        finalResponseHeadersStart:s=>s.record.finalResponseHeadersStart,
        firstInterimResponseStart:s=>s.record.firstInterimResponseStart,
        responseStart:s=>s.record.firstInterimResponseStart||s.record.finalResponseHeadersStart,
        responseEnd:s=>s.record.responseEnd,
        workerRouterEvaluationStart:zeroTime,workerCacheLookupStart:zeroTime,
        workerMatchedRouterSource:emptyString,workerFinalRouterSource:emptyString,
        // A constant 300 for the headers keeps their size (cookies) unexposed; a
        // failed timing allow check exposes none.
        transferSize:s=>!s.record.timingAllowPassed||s.record.cacheState==='local'?0:s.record.cacheState==='validated'?300:s.record.encodedBodySize+300,
        encodedBodySize:s=>s.record.encodedBodySize,
        decodedBodySize:s=>s.record.decodedBodySize,
        responseStatus:s=>s.record.responseStatus,
        renderBlockingStatus:()=>'non-blocking',
        contentType:s=>s.record.contentType,
        contentEncoding:s=>s.record.contentEncoding,
        serverTiming:s=>{
            if(!s.serverTiming){
                s.serverTiming=freeze(s.record.serverTiming?serverMetrics(string(s.record.serverTiming)):[]);
            }
            return s.serverTiming;
        },
    };
    const resourceKinds=new Set(['resource','navigation']);
    const resourceSlot=value=>{const s=slot(entries,value);if(!call(setHas,resourceKinds,[s.entryType]))throw new TypeErr('Illegal invocation');return s;};
    class PerformanceResourceTiming extends PerformanceEntry {
        constructor(key){super(key);if(key!==token)throw new TypeErr('Illegal constructor');}
        toJSON(){resourceSlot(this);return json(this,resourceMembers);}
    }
    const resourceMembers=copy(entryMembers);
    for(const name of Object.keys(resourceGetters)){
        const read=resourceGetters[name];
        define(PerformanceResourceTiming.prototype,name,{configurable:true,get(){return read(resourceSlot(this));}});
        append(resourceMembers,[name,Object.getOwnPropertyDescriptor(PerformanceResourceTiming.prototype,name).get]);
    }
    function resourceEntry(prototype,name,entryType,record,fields){
        const entry=create(prototype);
        const s=entrySlot({name,entryType,startTime:record.startTime,duration:record.responseEnd-record.startTime,record,serverTiming:null});
        for(const key of Object.keys(fields))s[key]=fields[key];
        put(entries,entry,s);return entry;
    }
    // Navigation Timing: the document's own navigation. Its load milestones are
    // the document's (host.timing), read as they are recorded.
    const loadTime=name=>{const measured=navigationTime(name);return measured===null?0:measured;};
    const navigationGetters={
        // No previous document's unload runs before a document replaces it.
        unloadEventStart:zeroTime,unloadEventEnd:zeroTime,
        domInteractive:()=>loadTime('domInteractive'),
        domContentLoadedEventStart:()=>loadTime('domContentLoadedEventStart'),
        domContentLoadedEventEnd:()=>loadTime('domContentLoadedEventEnd'),
        domComplete:()=>loadTime('domComplete'),
        loadEventStart:()=>loadTime('loadEventStart'),
        loadEventEnd:()=>loadTime('loadEventEnd'),
        type:s=>s.record.type,
        redirectCount:s=>s.record.redirectCount,
        // No Critical-CH restart happens: Client Hints never restart a navigation.
        criticalCHRestart:zeroTime,
    };
    const navigationSlot=value=>kind(value,'navigation');
    class PerformanceNavigationTiming extends PerformanceResourceTiming {
        constructor(key){super(key);if(key!==token)throw new TypeErr('Illegal constructor');}
        // A navigation with no exposed redirects has no redirect times.
        get redirectStart(){const s=navigationSlot(this);return s.record.redirectCount?s.record.redirectStart:0;}
        get redirectEnd(){const s=navigationSlot(this);return s.record.redirectCount?s.record.redirectEnd:0;}
        toJSON(){navigationSlot(this);return json(this,navigationMembers);}
    }
    const navigationMembers=copy(resourceMembers);
    for(const name of Object.keys(navigationGetters)){
        const read=navigationGetters[name];
        define(PerformanceNavigationTiming.prototype,name,{configurable:true,get(){return read(navigationSlot(this));}});
        append(navigationMembers,[name,Object.getOwnPropertyDescriptor(PerformanceNavigationTiming.prototype,name).get]);
    }
    for(const name of ['redirectStart','redirectEnd']){
        const getter=Object.getOwnPropertyDescriptor(PerformanceNavigationTiming.prototype,name).get;
        for(let i=0;i<navigationMembers.length;i++)if(navigationMembers[i][0]===name)navigationMembers[i]=[name,getter];
    }
    // Paint Timing: first-paint and first-contentful-paint of the document.
    class PerformancePaintTiming extends PerformanceEntry {
        constructor(key){super(key);if(key!==token)throw new TypeErr('Illegal constructor');}
        get paintTime(){return kind(this,'paint').paintTime;}
        get presentationTime(){kind(this,'paint');return null;} // presentation is not observed
        toJSON(){kind(this,'paint');return json(this,paintMembers);}
    }
    const paintMembers=copy(entryMembers);
    for(const name of ['paintTime','presentationTime'])append(paintMembers,[name,Object.getOwnPropertyDescriptor(PerformancePaintTiming.prototype,name).get]);
    function paint(name,time){
        const n=string(name);
        if(n!=='first-paint'&&n!=='first-contentful-paint')return;
        const entry=create(PerformancePaintTiming.prototype);
        // The default paint timestamp is the paint time while presentation is unknown.
        put(entries,entry,entrySlot({name:n,entryType:'paint',startTime:time,duration:0,paintTime:time}));
        queue(entry);
    }
    // HTML 6.2 VisibilityStateEntry (Window only): one per "update the visibility state" of the
    // document, and one for its initial state at time 0. Its registry entry keeps at most 50 in
    // the performance timeline (maxBufferSize); observers still receive the rest.
    class VisibilityStateEntry extends PerformanceEntry {
        constructor(key){super(key);if(key!==token)throw new TypeErr('Illegal constructor');}
        get name(){return kind(this,'visibility-state').name;}
        get entryType(){return kind(this,'visibility-state').entryType;}
        get startTime(){return kind(this,'visibility-state').startTime;}
        get duration(){kind(this,'visibility-state');return 0;}
    }
    function visibility(state,startTime){
        const entry=create(VisibilityStateEntry.prototype);
        put(entries,entry,entrySlot({name:string(state),entryType:'visibility-state',startTime,duration:0}));
        queue(entry);
    }
    // Performance Timeline "queue a PerformanceEntry": its id and navigation id,
    // every observer whose registration names its type, and its type's buffer
    // unless that is full (the dropped entries count) or the type keeps its own.
    function queue(entry,buffered=true){
        const s=get(entries,entry);s.sequence=++sequence;
        if(!s.id)s.id=nextEntryId();
        if(s.navigationId===null&&isWindow)s.navigationId=mostRecentNavigation;
        const registered=activeList();let interested=false;
        for(let i=0;i<registered.length;i++){
            const o=get(observers,registered[i]);
            if(call(setHas,o.types,[s.entryType])){append(o.buffer,entry);interested=true;}
        }
        if(buffered){
            const t=tupleOf(s.entryType);
            if(t.buffer.length>=t.max)t.dropped++;else append(t.buffer,entry);
        }
        if(interested)queueNotification();
        return entry;
    }
    // Resource Timing's buffer: a size limit, and a secondary buffer for the
    // entries that came while it was full, until the resourcetimingbufferfull
    // task gives the page its chance to make room.
    const initiators=new Map();
    const initiatorKinds=['script','fetch','link','img','other','iframe','css','other','script'];
    let resourceLimit=250,resourceSecondary=[],resourceFullPending=false,performanceObject=null;
    const resourceTuple=tupleOf('resource');
    function addResource(entry){
        if(resourceTuple.buffer.length<resourceLimit&&!resourceFullPending){append(resourceTuple.buffer,entry);return;}
        resourceTuple.dropped++;
        if(!resourceFullPending){resourceFullPending=true;try{schedule(bufferFull,0);}catch(e){resourceFullPending=false;report(e);}}
        append(resourceSecondary,entry);
    }
    function bufferFull(){
        while(resourceSecondary.length){
            const excess=resourceSecondary.length;
            if(resourceTuple.buffer.length>=resourceLimit){
                try{performanceObject.dispatchEvent(new Event('resourcetimingbufferfull'));}catch(e){report(e);}
            }
            while(resourceSecondary.length&&resourceTuple.buffer.length<resourceLimit){
                append(resourceTuple.buffer,call(splice,resourceSecondary,[0,1])[0]);
                resourceTuple.dropped--; // it was kept after all
            }
            if(resourceSecondary.length===excess){resourceSecondary=[];break;}
        }
        resourceFullPending=false;
    }
    // Resource Timing "mark resource timing" for a request native code reported.
    function resource(id,requestKind,url,record){
        const initiatorType=call(mapGet,initiators,[id])||initiatorKinds[requestKind]||'other';call(mapDelete,initiators,[id]);
        const entry=resourceEntry(PerformanceResourceTiming.prototype,string(url),'resource',record,{initiatorType});
        queue(entry,false);addResource(entry);
    }
    // A Worker's own request: its parent's transport reports the record (js.c timing_value,
    // NJW_LOADED_TIMING) in monotonic milliseconds, the clock of host.now(); this realm's time
    // origin converts each time ("convert fetch timestamp": zero stays zero). The record is
    // copied into a private one: a wire record has the page's Object.prototype.
    const recordTimes=['startTime','fetchStart','responseEnd','redirectStart','redirectEnd','domainLookupStart','domainLookupEnd',
        'connectStart','connectEnd','secureConnectionStart','requestStart','firstInterimResponseStart','finalResponseHeadersStart'];
    const recordValues=['encodedBodySize','decodedBodySize','responseStatus','redirectCount','legacyRedirectCount',
        'nextHopProtocol','contentEncoding','contentType','cacheState','serverTiming','timingAllowPassed'];
    const hasOwn=Object.prototype.hasOwnProperty;
    function monotonicResource(id,requestKind,url,wire){
        if(isWindow||wire===null||typeof wire!=='object')return;
        const record=create(null),own=key=>call(hasOwn,wire,[key])?wire[key]:undefined;
        for(let i=0;i<recordTimes.length;i++){const at=+own(recordTimes[i]);record[recordTimes[i]]=finite(at)&&at>0?(at>base?at-base:0):0;}
        for(let i=0;i<recordValues.length;i++)record[recordValues[i]]=own(recordValues[i]);
        resource(id,requestKind,url,record);
    }
    function queueNotification(){
        if(notificationPending)return;
        notificationPending=true;
        try{schedule(notify,0);}catch(e){notificationPending=false;report(e);}
    }
    function clear(type,name){
        const t=tupleOf(type),retained=[];
        for(let i=0;i<t.buffer.length;i++){const e=t.buffer[i],s=get(entries,e);if(name!==undefined&&s.name!==name)append(retained,e);}
        t.buffer=retained;
    }
    class PerformanceTiming {
        constructor(){throw new TypeErr('Illegal constructor');}
        toJSON(){slot(timings,this);return json(this,legacyMembers);}
    }
    const legacyMembers=[];
    for(let i=0;i<timingNames.length;i++){
        const name=timingNames[i];
        define(PerformanceTiming.prototype,name,{enumerable:true,configurable:true,get(){
            slot(timings,this);const measured=navigationTime(name);
            return measured===null?0:floor(origin+measured);
        }});
        append(legacyMembers,[name,Object.getOwnPropertyDescriptor(PerformanceTiming.prototype,name).get]);
    }
    const legacyTiming=create(PerformanceTiming.prototype);put(timings,legacyTiming,true);
    // The document's navigation (js.c native_navigation_timing), null without one.
    const navigationRecord=isWindow&&typeof host.navigationTiming==='function'?host.navigationTiming():null;
    class PerformanceNavigation {
        constructor(){throw new TypeErr('Illegal constructor');}
        get type(){slot(timings,this);return navigationRecord?navigationRecord.type==='reload'?1:navigationRecord.type==='back_forward'?2:0:0;}
        // The legacy count is 0 once any redirect was cross-origin.
        get redirectCount(){slot(timings,this);return navigationRecord?navigationRecord.legacyRedirectCount:0;}
        toJSON(){slot(timings,this);return json(this,legacyNavigationMembers);}
    }
    const legacyNavigationMembers=['type','redirectCount'].map(name=>[name,Object.getOwnPropertyDescriptor(PerformanceNavigation.prototype,name).get]);
    const legacyNavigation=create(PerformanceNavigation.prototype);put(timings,legacyNavigation,true);
    class Performance extends EventTarget {
        constructor(key){super();if(key!==token)throw new TypeErr('Illegal constructor');put(performances,this,true);}
        now(){slot(performances,this);return now();}
        get timeOrigin(){slot(performances,this);return origin;}
        get timing(){slot(performances,this);return legacyTiming;}
        get navigation(){slot(performances,this);return legacyNavigation;}
        toJSON(){slot(performances,this);return isWindow?{timeOrigin:origin,timing:legacyTiming,navigation:legacyNavigation}:{timeOrigin:origin};}
        mark(name,options={}){
            slot(performances,this);if(!arguments.length)throw new TypeErr('A mark name is required');
            return queue(new PerformanceMark(name,options));
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
            put(entries,entry,entrySlot({name,entryType:'measure',startTime,duration:endTime-startTime,detail:isOptions?metadata(o.detail):null}));
            return queue(entry);
        }
        clearMarks(name){slot(performances,this);clear('mark',name===undefined?undefined:string(name));}
        clearMeasures(name){slot(performances,this);clear('measure',name===undefined?undefined:string(name));}
        // Clear only the resource buffer of the actual private timeline. Marks,
        // measures and previously queued observer records survive.
        clearResourceTimings(){slot(performances,this);resourceTuple.buffer=[];}
        setResourceTimingBufferSize(maxSize){
            slot(performances,this);if(!arguments.length)throw new TypeErr('A buffer size is required');
            resourceLimit=(+maxSize)>>>0; // Web IDL unsigned long: ToUint32 of the number
        }
        get onresourcetimingbufferfull(){slot(performances,this);return get(fullHandlers,this)||null;}
        set onresourcetimingbufferfull(value){
            slot(performances,this);const previous=get(fullHandlers,this);
            if(previous)this.removeEventListener('resourcetimingbufferfull',previous);
            const handler=typeof value==='function'||(value&&typeof value==='object')?value:null;
            put(fullHandlers,this,handler);if(handler)this.addEventListener('resourcetimingbufferfull',handler);
        }
        getEntries(){slot(performances,this);return timeline();}
        getEntriesByType(type){slot(performances,this);if(!arguments.length)throw new TypeErr('An entry type is required');return timeline(undefined,string(type));}
        getEntriesByName(name,type){slot(performances,this);if(!arguments.length)throw new TypeErr('An entry name is required');return timeline(string(name),type===undefined?undefined:string(type));}
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
                const source=tupleOf(type).buffer;
                for(let i=0;i<source.length;i++)append(s.buffer,source[i]);
                queueNotification();
            }
        }
        disconnect(){const s=slot(observers,this);call(setDelete,active,[this]);s.types=new Set();s.buffer=[];}
        takeRecords(){const s=slot(observers,this),a=copy(s.buffer);s.buffer=[];return a;}
    }
    // The PerformanceObserver task: each observer's buffer, with the dropped
    // entries of its types on its first callback after observe().
    function notify(){
        notificationPending=false;
        const registered=activeList();
        for(let i=0;i<registered.length;i++){
            const observer=registered[i],s=get(observers,observer);
            if(!s.buffer.length)continue;
            const batch=s.buffer;s.buffer=[];
            const list=create(PerformanceObserverEntryList.prototype);put(lists,list,batch);
            const options={};
            if(s.requiresDropped){
                let dropped=0;call(eachSet,s.types,[type=>{dropped+=tupleOf(type).dropped;}]);
                options.droppedEntriesCount=dropped;s.requiresDropped=false;
            }
            try{call(s.callback,observer,[list,observer,options]);}catch(e){report(e);}
        }
    }
    const interfaces=[Performance,PerformanceTiming,PerformanceNavigation,PerformanceEntry,PerformanceMark,PerformanceMeasure,
        PerformanceResourceTiming,PerformanceNavigationTiming,PerformancePaintTiming,PerformanceServerTiming,
        VisibilityStateEntry,PerformanceObserver,PerformanceObserverEntryList];
    for(const ctor of interfaces){
        define(ctor.prototype,Symbol.toStringTag,{value:ctor.name,configurable:true});
        for(const key of Object.getOwnPropertyNames(ctor.prototype))if(key!=='constructor'){
            const d=Object.getOwnPropertyDescriptor(ctor.prototype,key);d.enumerable=true;define(ctor.prototype,key,d);
        }
    }
    for(const ctor of interfaces)if(ctor!==PerformanceMark&&ctor!==PerformanceObserver)define(ctor,'length',{value:0,configurable:true});
    for(const key of ['clearMarks','clearMeasures'])define(Performance.prototype[key],'length',{value:0,configurable:true});
    define(Performance.prototype.getEntriesByName,'length',{value:1,configurable:true});
    define(PerformanceObserverEntryList.prototype.getEntriesByName,'length',{value:1,configurable:true});
    const supportedDescriptor=Object.getOwnPropertyDescriptor(PerformanceObserver,'supportedEntryTypes');
    supportedDescriptor.enumerable=true;define(PerformanceObserver,'supportedEntryTypes',supportedDescriptor);
    performanceObject=new Performance(token);
    Object.assign(globalThis,{Performance,PerformanceEntry,PerformanceMark,PerformanceMeasure,PerformanceResourceTiming,PerformanceServerTiming,
        PerformanceObserver,PerformanceObserverEntryList,performance:performanceObject});
    // Navigation and paint timing, and the legacy interfaces, are Window-only.
    if(isWindow)Object.assign(globalThis,{PerformanceTiming,PerformanceNavigation,PerformanceNavigationTiming,PerformancePaintTiming});
    else{delete Performance.prototype.timing;delete Performance.prototype.navigation;}
    // Navigation Timing "create the navigation timing entry" (HTML "create and
    // initialize a Document", before the initial visibility state): in the buffer
    // from the document's creation, its id the document's most recent navigation's;
    // observers see it when HTML queues it after the load event.
    let navigationEntry=null;
    if(navigationRecord){
        navigationEntry=resourceEntry(PerformanceNavigationTiming.prototype,string(host.url()),'navigation',navigationRecord,{initiatorType:'navigation'});
        const s=get(entries,navigationEntry);
        s.startTime=0;s.duration=()=>loadTime('loadEventEnd');
        s.id=s.navigationId=mostRecentNavigation=nextEntryId();s.sequence=++sequence;
        append(tupleOf('navigation').buffer,navigationEntry);
    }
    // HTML "set the initial visibility state" queues the first entry with timestamp 0.
    if(isWindow&&typeof host.visible==='function'){globalThis.VisibilityStateEntry=VisibilityStateEntry;visibility(host.visible()?'visible':'hidden',0);}
    function navigationQueued(){
        if(!navigationEntry)return;
        const entry=navigationEntry;navigationEntry=null;
        queue(entry,false);
    }
    // An aborted request never completes; its registration is dropped with the
    // oldest rather than kept for the document's lifetime.
    return {resource,monotonicResource,visibility,paint,navigationQueued,initiator(id,type){
        if(initiators.size>=4096)for(const key of initiators.keys()){call(mapDelete,initiators,[key]);break;}
        call(mapSet,initiators,[id,type]);
    }};
})();
