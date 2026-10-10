/* Fetch API over the native, CORS-checked browser transport.
   https://fetch.spec.whatwg.org/#fetch-api (consulted 2026-10-06).
   Body bytes are real snapshots, not String(BufferSource). Response and Blob
   bodies expose real Streams with native headers/chunk/end flow control.
   Stream uploads and manual opaque redirects fail explicitly. Native no-cors
   fetches perform actual HTTP and expose an empty opaque response after a
   cross-origin hop; the native worker checks safelists and resource policy.
   Buffered keepalive requests use a native, document-independent transport;
   Fetch's 64 KiB inflight body accounting belongs to that native fetch group.
   Request cache modes use the cacheless native host's genuine network path;
   only-if-cached always misses and never sends a network request. No Cache
   API or stored responses are fabricated. HTTP-generated cache headers belong
   after CORS preflight, not in the author's Request.headers.
   https://fetch.spec.whatwg.org/#http-network-or-cache-fetch (2026-10-07).
   Included after DOMException; initialize after js_encoding.js/js_url.js. */
const fetchBridge = (() => {
    const headerSlots=new WeakMap(), requestSlots=new WeakMap(), responseSlots=new WeakMap();
    const bodySlots=new WeakMap(), signalSlots=new WeakMap(), controllerSlots=new WeakMap(),networkSlots=new WeakMap();
    const NativePromise=Promise, then=Promise.prototype.then, apply=Reflect.apply;
    const NativeWeakRef=WeakRef, deref=WeakRef.prototype.deref;
    const registerFinalizer=FinalizationRegistry.prototype.register,unregisterFinalizer=FinalizationRegistry.prototype.unregister;
    const dependentFinalizer=new FinalizationRegistry(cleanups=>{for(const [s,fn]of cleanups)s.algorithms.delete(fn);});
    const networkFinalizer=new FinalizationRegistry(id=>host.cancel(id));
    const define=Object.defineProperty, create=Object.create, keys=Object.keys, setPrototype=Object.setPrototypeOf;
    const weakGet=WeakMap.prototype.get, weakSet=WeakMap.prototype.set, weakHas=WeakMap.prototype.has;
    // These maps contain the exclusive native transport buffer. Page changes
    // to WeakMap.prototype must not expose private records through callbacks.
    for(const map of [headerSlots,requestSlots,responseSlots,bodySlots,signalSlots,controllerSlots,networkSlots]) {
        define(map,'get',{value:key=>apply(weakGet,map,[key])});
        define(map,'set',{value:(key,value)=>apply(weakSet,map,[key,value])});
        define(map,'has',{value:key=>apply(weakHas,map,[key])});
    }
    function storeBody(object,body) {
        // Missing stream/releaseBytes fields must not invoke an author accessor
        // on Object.prototype with the private Body as its receiver.
        setPrototype(body,null);
        if(!('stream' in body))body.stream=null;
        if(!('releaseBytes' in body))body.releaseBytes=false;
        bodySlots.set(object,body);
    }
    const promiseConstructor=Object.freeze({[Symbol.species]:NativePromise});
    const U8=Uint8Array, AB=ArrayBuffer, isView=ArrayBuffer.isView;
    const getter=(proto,key)=>Object.getOwnPropertyDescriptor(proto,key).get;
    const abLength=getter(AB.prototype,'byteLength'), ta=Object.getPrototypeOf(U8.prototype);
    const taBuffer=getter(ta,'buffer'), taOffset=getter(ta,'byteOffset'), taLength=getter(ta,'byteLength');
    const taTag=getter(ta,Symbol.toStringTag), dvBuffer=getter(DataView.prototype,'buffer');
    const dvOffset=getter(DataView.prototype,'byteOffset'), dvLength=getter(DataView.prototype,'byteLength');
    const stringify=JSON.stringify, parse=JSON.parse, wellFormed=String.prototype.toWellFormed;
    const token=/^[!#$%&'*+.^_`|~0-9A-Za-z-]+$/;
    const forbidden=/^(accept-charset|accept-encoding|access-control-request-headers|access-control-request-method|connection|content-length|cookie2?|date|dnt|expect|host|keep-alive|origin|referer|set-cookie|te|trailer|transfer-encoding|upgrade|via)$|^(proxy-|sec-)/i;
    const nullStatuses=new Set([101,103,204,205,304]);
    const cacheModes=['default','no-store','reload','no-cache','force-cache','only-if-cached'];
    let NativeURL, Params, paramsString, Encoder, encode, Decoder, decode;
    let blobAPI,formDataAPI,objectURLAPI,streams;
    const str=v=>{if(typeof v==='symbol')throw new TypeError('Cannot convert Symbol to string');return String(v);};
    const usv=v=>apply(wellFormed,str(v),[]);
    const byteString=v=>{const t=str(v);if(/[^\x00-\xff]/.test(t))throw new TypeError('Expected ByteString');return t;};
    const dictionary=v=>{if(v==null)return {};if(typeof v!=='object'&&typeof v!=='function')throw new TypeError('Expected dictionary');return v;};
    const slot=(map,v,label)=>{const s=map.get(v);if(!s)throw new TypeError('Illegal '+label+' receiver');return s;};
    const notSupported=message=>new DOMException(message,'NotSupportedError');
    const reject=e=>new NativePromise((_,r)=>r(e));
    const normalizeName=v=>{v=byteString(v).toLowerCase();if(!token.test(v))throw new TypeError('Invalid header name');return v;};
    const normalizeValue=v=>{v=byteString(v).replace(/^[\t\r\n ]+|[\t\r\n ]+$/g,'');if(/[\0\r\n]/.test(v))throw new TypeError('Invalid header value');return v;};
    function forbiddenRequest(name,value) {
        return forbidden.test(name) || (['x-http-method','x-http-method-override','x-method-override'].includes(name)&&
            value.split(',').some(v=>/^(CONNECT|TRACE|TRACK)$/i.test(v.trim())));
    }
    function noCorsSafe(name,value) {
        if(value.length>128)return false;
        const unsafe=/[\x00-\x08\x0a-\x1f"():<>?@\[\\\]{}\x7f]/;
        if(name==='accept')return !unsafe.test(value);
        if(name==='accept-language'||name==='content-language')return /^[0-9A-Za-z *,\-.;=]*$/.test(value);
        if(name==='content-type') {
            if(unsafe.test(value))return false;
            const essence=value.split(';')[0].trim().toLowerCase();
            return ['application/x-www-form-urlencoded','multipart/form-data','text/plain'].includes(essence);
        }
        return false;
    }
    function canChange(s,name,value) {
        if(s.guard==='immutable')throw new TypeError('Headers are immutable');
        if(s.guard==='request'||s.guard==='request-no-cors')if(forbiddenRequest(name,value))return false;
        if(s.guard==='response'&&/^set-cookie2?$/.test(name))return false;
        return true;
    }
    const headerValue=(s,name)=>{const a=s.list.filter(e=>e[0]===name);return a.length?a.map(e=>e[1]).join(', '):null;};
    function appendHeader(s,name,value) {
        if(!canChange(s,name,value))return;
        const previous=headerValue(s,name);
        if(s.guard==='request-no-cors'&&!noCorsSafe(name,previous===null?value:previous+', '+value))return;
        s.list.push([name,value]);
        if(s.guard==='request-no-cors')s.list=s.list.filter(e=>e[0]!=='range');
    }
    function fillHeaders(s,init) {
        if(init==null)return;
        if(headerSlots.has(init)) {for(const [n,v]of headerSlots.get(init).list)appendHeader(s,n,v);return;}
        if(typeof init!=='object'&&typeof init!=='function')throw new TypeError('Expected HeadersInit');
        const iterator=init[Symbol.iterator];
        if(iterator!==undefined) {
            if(typeof iterator!=='function')throw new TypeError('HeadersInit is not iterable');
            for(const pair of init) {
                if(pair===null||(typeof pair!=='object'&&typeof pair!=='function'))throw new TypeError('Expected header pair');
                const fields=Array.from(pair);if(fields.length!==2)throw new TypeError('Header pair must contain two items');
                appendHeader(s,normalizeName(fields[0]),normalizeValue(fields[1]));
            }
        } else for(const n of keys(init))appendHeader(s,normalizeName(n),normalizeValue(init[n]));
    }
    function makeHeaders(init,guard='none') {
        const h=create(Headers.prototype),s={guard:guard==='immutable'?'none':guard,list:[]};
        headerSlots.set(h,s);fillHeaders(s,init);s.guard=guard;return h;
    }
    function sortedHeaders(s) {
        const names=[...new Set(s.list.map(e=>e[0]))].sort(),out=[];
        for(const n of names) {
            if(n==='set-cookie'){for(const e of s.list)if(e[0]===n)out.push([n,e[1]]);}
            else out.push([n,headerValue(s,n)]);
        }
        return out;
    }
    class Headers {
        constructor(init){headerSlots.set(this,{guard:'none',list:[]});fillHeaders(headerSlots.get(this),init);}
        append(name,value){const s=slot(headerSlots,this,'Headers');if(arguments.length<2)throw new TypeError('append requires two arguments');appendHeader(s,normalizeName(name),normalizeValue(value));}
        set(name,value) {
            const s=slot(headerSlots,this,'Headers');if(arguments.length<2)throw new TypeError('set requires two arguments');
            name=normalizeName(name);value=normalizeValue(value);if(!canChange(s,name,value))return;
            if(s.guard==='request-no-cors'&&!noCorsSafe(name,value))return;
            const i=s.list.findIndex(e=>e[0]===name);s.list=s.list.filter(e=>e[0]!==name);
            s.list.splice(i<0?s.list.length:i,0,[name,value]);
            if(s.guard==='request-no-cors')s.list=s.list.filter(e=>e[0]!=='range');
        }
        delete(name) {
            const s=slot(headerSlots,this,'Headers');if(!arguments.length)throw new TypeError('delete requires name');name=normalizeName(name);
            if(!canChange(s,name,''))return;
            if(s.guard==='request-no-cors'&&!['accept','accept-language','content-language','content-type','range'].includes(name))return;
            s.list=s.list.filter(e=>e[0]!==name&&(s.guard!=='request-no-cors'||e[0]!=='range'));
        }
        get(name){const s=slot(headerSlots,this,'Headers');if(!arguments.length)throw new TypeError('get requires name');return headerValue(s,normalizeName(name));}
        has(name){const s=slot(headerSlots,this,'Headers');if(!arguments.length)throw new TypeError('has requires name');name=normalizeName(name);return s.list.some(e=>e[0]===name);}
        getSetCookie(){return slot(headerSlots,this,'Headers').list.filter(e=>e[0]==='set-cookie').map(e=>e[1]);}
        entries(){const s=slot(headerSlots,this,'Headers');return (function*(){for(let i=0;;i++){const list=sortedHeaders(s);if(i>=list.length)return;yield list[i];}})();}
        keys(){const s=slot(headerSlots,this,'Headers');return (function*(){for(let i=0;;i++){const list=sortedHeaders(s);if(i>=list.length)return;yield list[i][0];}})();}
        values(){const s=slot(headerSlots,this,'Headers');return (function*(){for(let i=0;;i++){const list=sortedHeaders(s);if(i>=list.length)return;yield list[i][1];}})();}
        forEach(fn,thisArg){const s=slot(headerSlots,this,'Headers');if(typeof fn!=='function')throw new TypeError('Expected callback');for(let i=0;;i++){const list=sortedHeaders(s);if(i>=list.length)return;apply(fn,thisArg,[list[i][1],list[i][0],this]);}}
        [Symbol.iterator](){return apply(Headers.prototype.entries,this,[]);}
    }
    // Copy using integer byte access, never ArrayBuffer/TypedArray @@species or
    // page-defined .buffer/.byteOffset accessors. Detached buffers must fail.
    function copyBytes(input) {
        let buffer=input,offset=0,length;
        if(isView(input)) {
            const typed=apply(taTag,input,[])!==undefined;
            buffer=apply(typed?taBuffer:dvBuffer,input,[]);offset=apply(typed?taOffset:dvOffset,input,[]);
            length=apply(typed?taLength:dvLength,input,[]);
        }
        const size=apply(abLength,buffer,[]),source=new U8(buffer,offset,length===undefined?size:length);
        const n=apply(taLength,source,[]),result=new U8(n);for(let i=0;i<n;i++)result[i]=source[i];
        return apply(taBuffer,result,[]);
    }
    function isBuffer(input){if(isView(input))return true;try{apply(abLength,input,[]);return true;}catch(_){return false;}}
    function extractBody(value) {
        if(value==null)return {bytes:null,type:null};
        if(isBuffer(value))return {bytes:copyBytes(value),type:null};
        if(blobAPI&&blobAPI.brand(value))return {bytes:blobAPI.bytes(value),type:blobAPI.type(value)||null};
        if(formDataAPI&&formDataAPI.brand(value))return formDataAPI.multipart(value);
        if(streams&&streams.brand(value)) {
            if(streams.locked(value)||streams.disturbed(value))throw new TypeError('Body stream is locked or disturbed');
            return {bytes:null,type:null,stream:value};
        }
        if(Params&&value instanceof Params)return {bytes:apply(taBuffer,apply(encode,new Encoder(),[apply(paramsString,value,[])]),[]),type:'application/x-www-form-urlencoded;charset=UTF-8'};
        return {bytes:apply(taBuffer,apply(encode,new Encoder(),[usv(value)]),[]),type:'text/plain;charset=UTF-8'};
    }
    function bodyStream(b) {
        if(b.stream)return b.stream;
        if(b.bytes===null)return null;
        b.stream=streams.fromBytes(b.bytes,()=>{
            if(b.abort){const signal=slot(signalSlots,b.abort,'AbortSignal');if(signal.aborted)throw signal.reason;}
        });
        /* A Response has no upload consumer. Once its stream owns the bytes,
         * the Body slot must not retain another backing-buffer root. */
        if(b.releaseBytes)b.bytes=null;
        return b.stream;
    }
    function unusable(b){return b.used||!!(b.stream&&(streams.locked(b.stream)||streams.disturbed(b.stream)));}
    function consume(object,kind) {
        let b;try{b=slot(bodySlots,object,'Body');}catch(e){return reject(e);}
        if(kind==='blob'&&!blobAPI)return reject(notSupported('Blob is not initialized'));
        if(unusable(b))return reject(new TypeError('Body already consumed or locked'));
        if((b.bytes!==null||b.stream)&&b.abort&&slot(signalSlots,b.abort,'AbortSignal').aborted)return reject(slot(signalSlots,b.abort,'AbortSignal').reason);
        const stream=streams?bodyStream(b):null;
        if(b.bytes!==null||stream)b.used=true;
        return new NativePromise((resolve,reject)=>{
            const finish=bytes=>{try {
                if(kind==='arrayBuffer')resolve(bytes);
                else if(kind==='bytes')resolve(new U8(bytes));
                else if(kind==='blob'){
                    const s=requestSlots.get(object)||responseSlots.get(object);
                    resolve(blobAPI.fromBytes(bytes,headerValue(headerSlots.get(s.headers),'content-type')||''));
                }
                else {const text=apply(decode,new Decoder(),[bytes]);resolve(kind==='json'?parse(text):text);}
            }catch(e){reject(e);}};
            if(stream){
                /* The stream already owns the immutable transport bytes. Do
                 * not retain a second backing-buffer root on a consumed Body. */
                b.bytes=null;
                try{const pending=streams.collect(stream);define(pending,'constructor',{value:promiseConstructor});apply(then,pending,[finish,reject]);}catch(e){reject(e);}
            }
            else finish(b.bytes===null?new AB(0):copyBytes(b.bytes));
        });
    }
    function installBody(proto) {
        define(proto,'bodyUsed',{configurable:true,enumerable:true,get(){const b=slot(bodySlots,this,'Body');return b.used||!!(b.stream&&streams.disturbed(b.stream));}});
        define(proto,'body',{configurable:true,enumerable:true,get(){return bodyStream(slot(bodySlots,this,'Body'));}});
        for(const name of ['text','json','arrayBuffer','bytes','blob'])define(proto,name,{configurable:true,enumerable:true,writable:true,value:function(){return consume(this,name);}});
        define(proto,'formData',{configurable:true,enumerable:true,writable:true,value:function(){try{slot(bodySlots,this,'Body');}catch(e){return reject(e);}return reject(notSupported('formData is not supported'));}});
    }
    const signalToken={};
    function newSignal(){return new AbortSignal(signalToken);}
    function abortSignal(signal,reason) {
        const s=slot(signalSlots,signal,'AbortSignal');if(s.aborted)return;
        s.aborted=true;s.reason=reason===undefined?new DOMException('The operation was aborted','AbortError'):reason;
        s.sources=null;
        const algorithms=[...s.algorithms];s.algorithms.clear();
        for(const fn of algorithms){try{fn(s.reason);}catch(e){report(e);}}
        const e=new Event('abort');eventState(e).isTrusted=true;dispatch(signal,e);
    }
    function dependentSignal(sources) {
        const signal=newSignal(),out=signalSlots.get(signal),cleanups=[],reference=new NativeWeakRef(signal);
        // A live dependent signal keeps its intermediate source signals alive.
        // Otherwise any([...]) temporaries could disappear before fetch aborts.
        out.sources=sources.slice();
        const cleanup=()=>{for(const [s,fn]of cleanups)s.algorithms.delete(fn);cleanups.length=0;};
        out.algorithms.add(cleanup);
        for(const source of sources) {
            const s=slot(signalSlots,source,'AbortSignal');
            if(s.aborted){abortSignal(signal,s.reason);break;}
            const fn=reason=>{const target=apply(deref,reference,[]);if(target)abortSignal(target,reason);else cleanup();};
            s.algorithms.add(fn);cleanups.push([s,fn]);
        }
        if(cleanups.length)dependentFinalizer.register(signal,cleanups);
        return signal;
    }
    class AbortSignal extends EventTarget {
        constructor(t){super();if(t!==signalToken)throw new TypeError('Illegal constructor');signalSlots.set(this,{aborted:false,reason:undefined,algorithms:new Set()});}
        get aborted(){return slot(signalSlots,this,'AbortSignal').aborted;}
        get reason(){return slot(signalSlots,this,'AbortSignal').reason;}
        throwIfAborted(){const s=slot(signalSlots,this,'AbortSignal');if(s.aborted)throw s.reason;}
        static abort(reason){const signal=newSignal();abortSignal(signal,reason);return signal;}
        static timeout(ms) {
            if(!arguments.length)throw new TypeError('timeout requires milliseconds');
            ms=+ms;if(!Number.isFinite(ms))throw new TypeError('Invalid timeout');
            ms=Math.trunc(ms);if(ms<0||ms>=18446744073709551616)throw new TypeError('Invalid timeout');
            // AbortSignal takes unsigned long long, not setTimeout's signed
            // long. Keep its complete duration and split only the native wait.
            const signal=newSignal(),started=host.now();
            const elapsed=()=>{
                const remaining=ms-(host.now()-started);
                if(remaining>0)host.timer(0,elapsed,Math.min(remaining,2147483647),[]);
                else abortSignal(signal,new DOMException('Timed out','TimeoutError'));
            };
            host.timer(0,elapsed,Math.min(ms,2147483647),[]);return signal;
        }
        static any(signals) {
            if(!arguments.length)throw new TypeError('any requires signals');
            if(signals===null||(typeof signals!=='object'&&typeof signals!=='function'))throw new TypeError('Expected signal sequence');
            if(typeof signals[Symbol.iterator]!=='function')throw new TypeError('Expected iterable signal sequence');
            const sources=Array.from(signals);for(const source of sources)slot(signalSlots,source,'AbortSignal');return dependentSignal(sources);
        }
    }
    class AbortController {
        constructor(){controllerSlots.set(this,newSignal());}
        get signal(){return slot(controllerSlots,this,'AbortController');}
        abort(reason){abortSignal(slot(controllerSlots,this,'AbortController'),reason);}
    }
    abortHandlerTarget=(target,type)=>signalSlots.has(target)&&type==='abort';
    installHandlers(AbortSignal.prototype,['abort']);
    function readInit(value) {
        const init=dictionary(value),out={};
        // WebIDL dictionary members are read once, in lexicographic order.
        for(const key of ['body','cache','credentials','duplex','headers','integrity','keepalive','method','mode','redirect','referrer','referrerPolicy','signal']) {
            const v=init[key];if(v!==undefined)out[key]=v;
        }
        return out;
    }
    function enumValue(v,allowed,label){v=str(v);if(!allowed.includes(v))throw new TypeError('Invalid '+label);return v;}
    function makeURL(value){return new NativeURL(usv(value),rawDom.get(document,'baseURI'));}
    function methodValue(value){const method=byteString(value),upper=method.toUpperCase();if(!token.test(method)||['CONNECT','TRACE','TRACK'].includes(upper))throw new TypeError('Invalid or forbidden HTTP method');return ['DELETE','GET','HEAD','OPTIONS','POST','PUT'].includes(upper)?upper:method;}
    function cloneBody(b){if(unusable(b))throw new TypeError('Body already consumed or locked');let stream;
        if(b.stream){const branches=streams.tee(b.stream);b.stream=branches[0];stream=branches[1];}
        return {bytes:b.bytes===null?null:copyBytes(b.bytes),stream,used:false,abort:b.abort};}
    function requestCopy(s,b) {
        const object=create(Request.prototype),copy={...s,headers:makeHeaders(s.headers,headerSlots.get(s.headers).guard),signal:dependentSignal([s.signal])};
        requestSlots.set(object,copy);storeBody(object,b);return object;
    }
    class Request {
        constructor(input,init={}) {
            if(!arguments.length)throw new TypeError('Request requires input');
            const old=requestSlots.get(input),i=readInit(init);
            let s=old?{...old}:{url:'',method:'GET',mode:'cors',credentials:'same-origin',cache:'default',redirect:'follow',
                referrer:'about:client',referrerPolicy:'',integrity:'',keepalive:false};
            if(!old){const url=makeURL(input);if(url.username||url.password)throw new TypeError('Request URL cannot contain credentials');s.url=url.href;}
            if(keys(i).length){s.referrer='about:client';s.referrerPolicy='';}
            if(i.method!==undefined)s.method=methodValue(i.method);
            if(i.mode!==undefined)s.mode=enumValue(i.mode,['cors','same-origin','no-cors','navigate'],'request mode');
            if(s.mode==='navigate')throw new TypeError('Cannot construct navigate request');
            if(i.credentials!==undefined)s.credentials=enumValue(i.credentials,['omit','same-origin','include'],'credentials mode');
            if(i.cache!==undefined)s.cache=enumValue(i.cache,cacheModes,'cache mode');
            if(s.cache==='only-if-cached'&&s.mode!=='same-origin')throw new TypeError('only-if-cached requires same-origin mode');
            if(i.redirect!==undefined)s.redirect=enumValue(i.redirect,['follow','error','manual'],'redirect mode');
            if(i.duplex!==undefined)enumValue(i.duplex,['half'],'duplex mode');
            if(i.integrity!==undefined)s.integrity=str(i.integrity);
            if(i.keepalive!==undefined)s.keepalive=!!i.keepalive;
            if(i.referrerPolicy!==undefined)s.referrerPolicy=enumValue(i.referrerPolicy,['','no-referrer','no-referrer-when-downgrade','same-origin','origin','strict-origin','origin-when-cross-origin','strict-origin-when-cross-origin','unsafe-url'],'referrer policy');
            if(i.referrer!==undefined) {
                const referrer=usv(i.referrer);
                if(referrer==='')s.referrer='';
                else {const u=makeURL(referrer);s.referrer=u.protocol==='about:'&&u.pathname==='client'||u.origin!==new NativeURL(host.url()).origin?'about:client':u.href;}
            }
            if(s.mode==='no-cors'&&!['GET','HEAD','POST'].includes(s.method))throw new TypeError('no-cors requires a safelisted method');
            const signal=i.signal!==undefined?i.signal:old?old.signal:null;
            if(signal!==null)slot(signalSlots,signal,'AbortSignal');
            s.headers=makeHeaders(i.headers!==undefined?i.headers:old?old.headers:undefined,s.mode==='no-cors'?'request-no-cors':'request');
            const previous=old?bodySlots.get(input):null,override=i.body!==undefined&&i.body!==null;
            const previousBody=previous&&(previous.bytes!==null||previous.stream);
            if((override||previousBody)&&['GET','HEAD'].includes(s.method))throw new TypeError('GET and HEAD cannot have a body');
            let body;
            if(override) {
                const extracted=extractBody(i.body);if(extracted.stream)throw notSupported('ReadableStream uploads require incremental native transport');
                body={bytes:extracted.bytes,used:false,abort:null};
                if(extracted.type!==null&&headerValue(headerSlots.get(s.headers),'content-type')===null)appendHeader(headerSlots.get(s.headers),'content-type',extracted.type);
            } else if(previousBody)body=cloneBody(previous);
            else body={bytes:null,used:false,abort:null};
            s.signal=dependentSignal(signal===null?[]:[signal]);
            // Transferring a Request body consumes the input, unlike clone().
            if(!override&&previousBody)previous.used=true;
            requestSlots.set(this,s);storeBody(this,body);
        }
        clone(){const s=slot(requestSlots,this,'Request');return requestCopy(s,cloneBody(slot(bodySlots,this,'Body')));}
        get destination(){slot(requestSlots,this,'Request');return '';}
        get duplex(){slot(requestSlots,this,'Request');return 'half';}
        get isReloadNavigation(){slot(requestSlots,this,'Request');return false;}
        get isHistoryNavigation(){slot(requestSlots,this,'Request');return false;}
    }
    for(const name of ['url','method','headers','credentials','mode','redirect','signal','cache','referrer','referrerPolicy','integrity','keepalive'])
        define(Request.prototype,name,{configurable:true,enumerable:true,get(){return slot(requestSlots,this,'Request')[name];}});
    function responseObject(s,body) {const object=create(Response.prototype);storeBody(object,body);body.releaseBytes=true;responseSlots.set(object,s);return object;}
    function responseInit(value) {
        const init=dictionary(value),headers=init.headers,status=init.status,statusText=init.statusText;
        let n=status===undefined?200:+status;n=Number.isFinite(n)?((Math.trunc(n)%65536)+65536)%65536:0;
        if(n<200||n>599)throw new RangeError('Response status must be between 200 and 599');
        const text=statusText===undefined?'':byteString(statusText);if(/[^\t\x20-\x7e\x80-\xff]/.test(text))throw new TypeError('Invalid statusText');
        return {status:n,statusText:text,headers:makeHeaders(headers,'response'),url:'',redirected:false,type:'default'};
    }
    class Response {
        constructor(body=null,init={}) {
            const s=responseInit(init),b=extractBody(body);if((b.bytes!==null||b.stream)&&nullStatuses.has(s.status))throw new TypeError('This status cannot have a body');
            if(b.type!==null&&headerValue(headerSlots.get(s.headers),'content-type')===null)appendHeader(headerSlots.get(s.headers),'content-type',b.type);
            responseSlots.set(this,s);storeBody(this,{bytes:b.bytes,stream:b.stream,used:false,abort:null,releaseBytes:true});
        }
        get ok(){const n=slot(responseSlots,this,'Response').status;return n>=200&&n<300;}
        clone(){const s=slot(responseSlots,this,'Response'),b=cloneBody(slot(bodySlots,this,'Body'));return responseObject({...s,headers:makeHeaders(s.headers,headerSlots.get(s.headers).guard)},b);}
        static error(){return responseObject({status:0,statusText:'',headers:makeHeaders(undefined,'immutable'),url:'',redirected:false,type:'error'},{bytes:null,used:false,abort:null});}
        static redirect(url,status=302) {
            if(!arguments.length)throw new TypeError('redirect requires URL');const parsed=makeURL(url);
            status=+status;status=Number.isFinite(status)?((Math.trunc(status)%65536)+65536)%65536:0;
            if(![301,302,303,307,308].includes(status))throw new RangeError('Invalid redirect status');
            const headers=makeHeaders({'location':parsed.href});headerSlots.get(headers).guard='immutable';
            return responseObject({status,statusText:'',headers,url:'',redirected:false,type:'default'},{bytes:null,used:false,abort:null});
        }
        static json(data,init={}) {
            if(!arguments.length)throw new TypeError('json requires data');
            const text=stringify(data);if(text===undefined)throw new TypeError('Value cannot be serialized as JSON');
            const s=responseInit(init);if(nullStatuses.has(s.status))throw new TypeError('This status cannot have a body');
            if(headerValue(headerSlots.get(s.headers),'content-type')===null)appendHeader(headerSlots.get(s.headers),'content-type','application/json');
            return responseObject(s,{bytes:extractBody(text).bytes,used:false,abort:null});
        }
    }
    for(const name of ['type','url','redirected','status','statusText','headers'])define(Response.prototype,name,{configurable:true,enumerable:true,get(){return slot(responseSlots,this,'Response')[name];}});
    installBody(Request.prototype);installBody(Response.prototype);
    function fetchCore(input,init,observer,forcePreflight=false) {
        let r,s,b;
        try {
            r=new Request(input,init);s=requestSlots.get(r);b=bodySlots.get(r);
            const signal=signalSlots.get(s.signal);if(signal.aborted)return reject(signal.reason);
            if(s.mode==='no-cors'&&!host.noCorsFetch)throw notSupported('This native context has no no-cors transport');
            if(s.redirect==='manual')throw notSupported('Opaque manual redirect responses are not supported');
            if(s.integrity)throw notSupported('Subresource integrity is not supported by fetch');
            // These overrides really request the native host's existing
            // no-referrer behavior. Do not reject them as missing transport.
            const noReferrer=s.referrer===''||s.referrerPolicy==='no-referrer';
            if(!noReferrer&&(s.referrer!=='about:client'||s.referrerPolicy))throw notSupported('Fetch referrer overrides requiring a Referer header are not supported');
            // This host has no stored HTTP responses. A cache-only miss is a
            // Fetch network error (TypeError), not a synthetic HTTP 504, and
            // cannot reach host.fetch even for a cross-origin target URL.
            if(s.cache==='only-if-cached')throw new TypeError('No cached response is available');
            if(objectURLAPI&&s.url.startsWith('blob:')){
                const local=objectURLAPI.fetch(s.url,s.method,headerValue(headerSlots.get(s.headers),'range')),fields={'content-type':local.type,'content-length':String(local.size)};if(local.range)fields['content-range']=local.range;const headers=makeHeaders(fields,'immutable');
                return new NativePromise(resolve=>resolve(responseObject({status:local.status,statusText:local.status===206?'Partial Content':'OK',url:s.url.split('#')[0],headers,redirected:false,type:'basic'},{bytes:local.bytes,used:false,abort:s.signal})));
            }
            let raw='';for(const [name,value]of sortedHeaders(headerSlots.get(s.headers)))raw+=name+': '+value+'\r\n';
            let pair,channel,channelRef,waiting,transportId=0,done=false,hasBody=false,finishStream=()=>{};
            const liveChannel=()=>channel||(channelRef&&apply(deref,channelRef,[]));
            const resume=()=>{if(!done&&transportId)host.resumeFetch(transportId);};
            if(host.streamingFetch&&streams){
                channel=streams.network(()=>new NativePromise(resolve=>{waiting=resolve;resume();}),reason=>{
                    if(done)return;done=true;finishStream();host.cancel(transportId);
                    if(waiting){const resolve=waiting;waiting=null;resolve();}
                });
            }
            const notify=channel?(event,value)=>{
                if(done)return true;
                const active=liveChannel();
                if(!active){done=true;finishStream();host.cancel(transportId);return false;}
                if(event===1){
                    const state=slot(bodySlots,value,'Body'),meta=slot(responseSlots,value,'Response');
                    hasBody=s.method!=='HEAD'&&meta.type!=='opaque'&&!nullStatuses.has(meta.status);
                    state.bytes=null;state.abort=s.signal;
                    if(hasBody&&!observer){state.stream=active.stream;networkSlots.set(active.stream,active);apply(registerFinalizer,networkFinalizer,[active,transportId,active]);channelRef=new NativeWeakRef(active);channel=null;}
                    if(observer){observer('headers',value,resume);return false;}
                    return !hasBody;
                }
                if(event===2){
                    if(observer){if(!hasBody)return true;observer('chunk',value,resume);return false;}
                    if(hasBody)active.enqueue(value);
                    if(waiting){const resolve=waiting;waiting=null;resolve();}
                    return !hasBody;
                }
                if(event===4){if(observer){observer('upload',value,resume);return false;}return true;}
                if(event===3){
                    done=true;finishStream();
                    if(value!==undefined)active.error(value);else active.close();
                    apply(unregisterFinalizer,networkFinalizer,[active]);channel=null;channelRef=null;
                    if(waiting){const resolve=waiting;waiting=null;resolve();}
                    if(observer)observer('end',value,()=>{});
                }
                return false;
            }:undefined;
            pair=host.fetch(s.url,s.method,raw,b.bytes===null?'':b.bytes,s.mode==='same-origin',['omit','same-origin','include'].indexOf(s.credentials),forcePreflight,s.redirect==='error'?1:0,cacheModes.indexOf(s.cache),s.keepalive,s.mode==='no-cors',noReferrer,notify);
            transportId=pair.id;const transportPromise=pair.promise;pair=null;
            if(b.bytes!==null)b.used=true;
            return new NativePromise((resolve,reject)=>{
                let finished=false,pendingReject=reject;
                const sig=signalSlots.get(s.signal),cleanup=()=>sig.algorithms.delete(abort);
                const abort=reason=>{if(done)return;done=true;finished=true;cleanup();const active=liveChannel();if(active){active.error(reason);apply(unregisterFinalizer,networkFinalizer,[active]);}channel=null;channelRef=null;if(waiting){const resolve=waiting;waiting=null;resolve();}try{host.cancel(transportId);}finally{if(pendingReject)pendingReject(reason);pendingReject=null;if(observer)observer('end',reason,()=>{});}};
                finishStream=cleanup;
                sig.algorithms.add(abort);
                try {
                    define(transportPromise,'constructor',{value:promiseConstructor});
                    apply(then,transportPromise,[response=>{
                        if(finished)return;finished=true;pendingReject=null;if(!notify)cleanup();
                        try {
                            const state=slot(bodySlots,response,'Body');state.abort=s.signal;
                            if(s.method==='HEAD')state.bytes=null;
                            resolve(response);
                        }catch(e){reject(e);}
                    },error=>{if(finished)return;finished=true;done=true;cleanup();reject(error);}]);
                }catch(e){finished=true;done=true;cleanup();const active=liveChannel();if(active)active.error(e);host.cancel(transportId);pendingReject=null;reject(e);}
                if(sig.aborted)abort(sig.reason);
            });
        }catch(e){return reject(e);}
    }
    function fetch(input,init){if(!arguments.length)return reject(new TypeError('fetch requires input'));return fetchCore(input,init);}
    for(const [C,name]of [[Headers,'Headers'],[Request,'Request'],[Response,'Response'],[AbortSignal,'AbortSignal'],[AbortController,'AbortController']]) {
        define(C.prototype,Symbol.toStringTag,{value:name,configurable:true});
        for(const key of Object.getOwnPropertyNames(C.prototype)){if(key==='constructor')continue;const d=Object.getOwnPropertyDescriptor(C.prototype,key);d.enumerable=true;define(C.prototype,key,d);}
    }
    return {
        Headers,Request,Response,AbortSignal,AbortController,fetch,
        initialize(){NativeURL=globalThis.URL;Params=globalThis.URLSearchParams;paramsString=Params.prototype.toString;Encoder=globalThis.TextEncoder;encode=Encoder.prototype.encode;Decoder=globalThis.TextDecoder;decode=Decoder.prototype.decode;},
        initializeBlobs(api){blobAPI=api;},
        initializeFormData(api){formDataAPI=api;},
        initializeStreams(api){streams=api;},
        initializeObjectURLs(api){objectURLAPI=api;},
        xhrBody(value){const binary=isBuffer(value)||!!(blobAPI&&blobAPI.brand(value))||!!(formDataAPI&&formDataAPI.brand(value)),body=extractBody(value);if(body.stream)throw notSupported('XHR stream uploads are unsupported');return {...body,binary};},
        xhrResponse(response) {
            // Only the private XHR completion path calls this. Do not invoke
            // page-mutable Response getters or publish a backing-buffer API.
            const s=slot(responseSlots,response,'Response'),b=slot(bodySlots,response,'Body');
            if(unusable(b)||b.stream)throw new TypeError('XHR body is not an exclusive buffered response');
            const bytes=b.bytes===null?new AB(0):b.bytes;b.bytes=null;b.used=true;
            return {status:s.status,statusText:s.statusText,url:s.url,headers:s.headers,bytes};
        },
        xhrFetch(url,init,observer,forcePreflight){const signal=newSignal();return {promise:fetchCore(url,{...init,signal},observer,forcePreflight),cancel(){abortSignal(signal);}};},
        xhrMetadata(response){const s=slot(responseSlots,response,'Response');return {status:s.status,statusText:s.statusText,url:s.url,headers:s.headers};},
        response(status,url,raw,text,bytes,redirected,statusText,type) {
            // Native completions redact the opaque internals before entering
            // this realm. Do not inspect author-modifiable regex/URL methods.
            if(type==='opaque')return responseObject({status:0,statusText:'',url:'',headers:makeHeaders(undefined,'immutable'),redirected:false,type:'opaque'},{bytes:null,used:false,abort:null});
            const headers=makeHeaders(undefined,'response');
            for(const line of raw.split(/\r?\n/)){const i=line.indexOf(':');if(i>0&&!line.startsWith('HTTP/'))appendHeader(headerSlots.get(headers),normalizeName(line.slice(0,i)),normalizeValue(line.slice(i+1)));}
            headerSlots.get(headers).guard='immutable';
            if(statusText===undefined){const m=/^HTTP\/\S+\s+\d{3}(?:[ \t]+([^\r\n]*))?/.exec(raw);statusText=m?(m[1]||''):'';}
            // Negotiated native metadata is not an HTTP field: webfetch only
            // copies server fields containing ':', so a server cannot forge it.
            const transport=type===undefined?/^HTTP\/Nocturne-Meta cors=([01]) redirected=([01])(?: opaque=([01]))?\r?$/m.exec(raw):null;
            if(transport&&transport[3]==='1')return responseObject({status:0,statusText:'',url:'',headers:makeHeaders(undefined,'immutable'),redirected:false,type:'opaque'},{bytes:null,used:false,abort:null});
            if(transport){type=transport[1]==='1'?'cors':'basic';redirected=transport[2]==='1';}
            if(type===undefined)type=new NativeURL(url).origin===new NativeURL(host.url()).origin?'basic':'cors';
            return responseObject({status,statusText,url:usv(url).split('#')[0],headers,redirected:!!redirected,type},
                // The native hook transfers an exclusive runtime-charged
                // snapshot. Author constructors and clone still copy/tee.
                {bytes:nullStatuses.has(status)?null:bytes,used:false,abort:null});
        }
    };
})();
const {Headers,Request,Response,AbortSignal,AbortController,fetch}=fetchBridge;
