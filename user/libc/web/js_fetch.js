/* Buffered Fetch API over the native, CORS-checked browser transport.
   https://fetch.spec.whatwg.org/#fetch-api (consulted 2026-10-06).
   Body bytes are real snapshots, not String(BufferSource). ReadableStream,
   FormData, opaque responses and background keepalive are unsupported
   and explicitly fail; no successful-looking stream or multipart stubs.
   Request cache modes use the cacheless native host's genuine network path;
   only-if-cached always misses and never sends a network request. No Cache
   API or stored responses are fabricated. HTTP-generated cache headers belong
   after CORS preflight, not in the author's Request.headers.
   https://fetch.spec.whatwg.org/#http-network-or-cache-fetch (2026-10-07).
   Included after DOMException; initialize after js_encoding.js/js_url.js. */
const fetchBridge = (() => {
    const headerSlots=new WeakMap(), requestSlots=new WeakMap(), responseSlots=new WeakMap();
    const bodySlots=new WeakMap(), signalSlots=new WeakMap(), controllerSlots=new WeakMap();
    const NativePromise=Promise, then=Promise.prototype.then, apply=Reflect.apply;
    const NativeWeakRef=WeakRef, deref=WeakRef.prototype.deref;
    const dependentFinalizer=new FinalizationRegistry(cleanups=>{for(const [s,fn]of cleanups)s.algorithms.delete(fn);});
    const define=Object.defineProperty, create=Object.create, keys=Object.keys;
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
    let blobAPI,objectURLAPI;
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
        for(const name of ['FormData','ReadableStream']) {
            const C=globalThis[name];
            if(typeof C==='function'&&value instanceof C)throw notSupported(name+' bodies are not supported');
        }
        if(Params&&value instanceof Params)return {bytes:apply(taBuffer,apply(encode,new Encoder(),[apply(paramsString,value,[])]),[]),type:'application/x-www-form-urlencoded;charset=UTF-8'};
        return {bytes:apply(taBuffer,apply(encode,new Encoder(),[usv(value)]),[]),type:'text/plain;charset=UTF-8'};
    }
    function consume(object,kind) {
        let b;try{b=slot(bodySlots,object,'Body');}catch(e){return reject(e);}
        if(kind==='blob'&&!blobAPI)return reject(notSupported('Blob is not initialized'));
        if(b.used)return reject(new TypeError('Body already consumed'));
        if(b.bytes!==null&&b.abort&&slot(signalSlots,b.abort,'AbortSignal').aborted)return reject(slot(signalSlots,b.abort,'AbortSignal').reason);
        if(b.bytes!==null)b.used=true;
        return new NativePromise((resolve,reject)=>{
            try {
                const bytes=b.bytes===null?new AB(0):copyBytes(b.bytes);
                if(kind==='arrayBuffer')resolve(bytes);
                else if(kind==='bytes')resolve(new U8(bytes));
                else if(kind==='blob'){
                    const s=requestSlots.get(object)||responseSlots.get(object);
                    resolve(blobAPI.fromBytes(bytes,headerValue(headerSlots.get(s.headers),'content-type')||''));
                }
                else {const text=apply(decode,new Decoder(),[bytes]);resolve(kind==='json'?parse(text):text);}
            }catch(e){reject(e);}
        });
    }
    function installBody(proto) {
        define(proto,'bodyUsed',{configurable:true,enumerable:true,get(){return slot(bodySlots,this,'Body').used;}});
        define(proto,'body',{configurable:true,enumerable:true,get(){if(slot(bodySlots,this,'Body').bytes===null)return null;throw notSupported('ReadableStream bodies are not supported; use text(), json(), arrayBuffer() or bytes()');}});
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
        const e=new Event('abort');e.isTrusted=true;dispatch(signal,e);
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
            ms=Math.trunc(ms);if(ms<0||ms>Number.MAX_SAFE_INTEGER)throw new TypeError('Invalid timeout');
            if(ms>2147483647)throw new RangeError('Timeout exceeds the browser timer limit');
            const signal=newSignal();host.timer(0,()=>abortSignal(signal,new DOMException('Timed out','TimeoutError')),ms,[]);return signal;
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
    function makeURL(value){return new NativeURL(usv(value),rawDom('get',document,'baseURI'));}
    function methodValue(value){const method=byteString(value),upper=method.toUpperCase();if(!token.test(method)||['CONNECT','TRACE','TRACK'].includes(upper))throw new TypeError('Invalid or forbidden HTTP method');return ['DELETE','GET','HEAD','OPTIONS','POST','PUT'].includes(upper)?upper:method;}
    function cloneBody(b){if(b.used)throw new TypeError('Body already consumed');return {bytes:b.bytes===null?null:copyBytes(b.bytes),used:false,abort:b.abort};}
    function requestCopy(s,b) {
        const object=create(Request.prototype),copy={...s,headers:makeHeaders(s.headers,headerSlots.get(s.headers).guard),signal:dependentSignal([s.signal])};
        requestSlots.set(object,copy);bodySlots.set(object,b);return object;
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
            if((override||previous&&previous.bytes!==null)&&['GET','HEAD'].includes(s.method))throw new TypeError('GET and HEAD cannot have a body');
            let body;
            if(override) {
                const extracted=extractBody(i.body);body={bytes:extracted.bytes,used:false,abort:null};
                if(extracted.type!==null&&headerValue(headerSlots.get(s.headers),'content-type')===null)appendHeader(headerSlots.get(s.headers),'content-type',extracted.type);
            } else if(previous&&previous.bytes!==null)body=cloneBody(previous);
            else body={bytes:null,used:false,abort:null};
            s.signal=dependentSignal(signal===null?[]:[signal]);
            // Transferring a Request body consumes the input, unlike clone().
            if(!override&&previous&&previous.bytes!==null)previous.used=true;
            requestSlots.set(this,s);bodySlots.set(this,body);
        }
        clone(){const s=slot(requestSlots,this,'Request');return requestCopy(s,cloneBody(slot(bodySlots,this,'Body')));}
        get destination(){slot(requestSlots,this,'Request');return '';}
        get duplex(){slot(requestSlots,this,'Request');return 'half';}
        get isReloadNavigation(){slot(requestSlots,this,'Request');return false;}
        get isHistoryNavigation(){slot(requestSlots,this,'Request');return false;}
    }
    for(const name of ['url','method','headers','credentials','mode','redirect','signal','cache','referrer','referrerPolicy','integrity','keepalive'])
        define(Request.prototype,name,{configurable:true,enumerable:true,get(){return slot(requestSlots,this,'Request')[name];}});
    function responseObject(s,body) {const object=create(Response.prototype);responseSlots.set(object,s);bodySlots.set(object,body);return object;}
    function responseInit(value) {
        const init=dictionary(value),headers=init.headers,status=init.status,statusText=init.statusText;
        let n=status===undefined?200:+status;n=Number.isFinite(n)?((Math.trunc(n)%65536)+65536)%65536:0;
        if(n<200||n>599)throw new RangeError('Response status must be between 200 and 599');
        const text=statusText===undefined?'':byteString(statusText);if(/[^\t\x20-\x7e\x80-\xff]/.test(text))throw new TypeError('Invalid statusText');
        return {status:n,statusText:text,headers:makeHeaders(headers,'response'),url:'',redirected:false,type:'default'};
    }
    class Response {
        constructor(body=null,init={}) {
            const s=responseInit(init),b=extractBody(body);if(b.bytes!==null&&nullStatuses.has(s.status))throw new TypeError('This status cannot have a body');
            if(b.type!==null&&headerValue(headerSlots.get(s.headers),'content-type')===null)appendHeader(headerSlots.get(s.headers),'content-type',b.type);
            responseSlots.set(this,s);bodySlots.set(this,{bytes:b.bytes,used:false,abort:null});
        }
        get ok(){const n=slot(responseSlots,this,'Response').status;return n>=200&&n<300;}
        clone(){const s=slot(responseSlots,this,'Response'),b=cloneBody(slot(bodySlots,this,'Body'));return responseObject({...s,headers:makeHeaders(s.headers,headerSlots.get(s.headers).guard)},b);}
        // Existing XHR consumes this host-private compatibility copy. It must
        // not expose the body's backing bytes to page mutation or @@species.
        get _bytes(){const b=slot(bodySlots,this,'Body');return b.bytes===null?new AB(0):copyBytes(b.bytes);}
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
    function fetch(input,init) {
        let r,s,b;
        try {
            if(!arguments.length)throw new TypeError('fetch requires input');r=new Request(input,init);s=requestSlots.get(r);b=bodySlots.get(r);
            const signal=signalSlots.get(s.signal);if(signal.aborted)return reject(signal.reason);
            if(s.mode==='no-cors')throw notSupported('Opaque no-cors responses are not supported');
            if(s.redirect==='manual')throw notSupported('Opaque manual redirect responses are not supported');
            if(s.integrity)throw notSupported('Subresource integrity is not supported by fetch');
            if(s.keepalive)throw notSupported('Background keepalive requests are not supported');
            if(s.referrer!=='about:client'||s.referrerPolicy)throw notSupported('Fetch referrer overrides are not supported');
            // This host has no stored HTTP responses. A cache-only miss is a
            // Fetch network error (TypeError), not a synthetic HTTP 504, and
            // cannot reach host.fetch even for a cross-origin target URL.
            if(s.cache==='only-if-cached')throw new TypeError('No cached response is available');
            if(objectURLAPI&&s.url.startsWith('blob:')){
                const local=objectURLAPI.fetch(s.url,s.method,headerValue(headerSlots.get(s.headers),'range')),fields={'content-type':local.type,'content-length':String(local.size)};if(local.range)fields['content-range']=local.range;const headers=makeHeaders(fields,'immutable');
                return new NativePromise(resolve=>resolve(responseObject({status:local.status,statusText:local.status===206?'Partial Content':'OK',url:s.url.split('#')[0],headers,redirected:false,type:'basic'},{bytes:local.bytes,used:false,abort:s.signal})));
            }
            let raw='';for(const [name,value]of sortedHeaders(headerSlots.get(s.headers)))raw+=name+': '+value+'\r\n';
            const pair=host.fetch(s.url,s.method,raw,b.bytes===null?'':b.bytes,s.mode==='same-origin',['omit','same-origin','include'].indexOf(s.credentials),false,s.redirect==='error'?1:0,cacheModes.indexOf(s.cache));
            if(b.bytes!==null)b.used=true;
            return new NativePromise((resolve,reject)=>{
                let finished=false;
                const sig=signalSlots.get(s.signal),cleanup=()=>sig.algorithms.delete(abort);
                const abort=reason=>{if(finished)return;finished=true;cleanup();try{host.cancel(pair.id);}finally{reject(reason);}};
                sig.algorithms.add(abort);
                try {
                    define(pair.promise,'constructor',{value:promiseConstructor});
                    apply(then,pair.promise,[response=>{
                        if(finished)return;finished=true;cleanup();
                        try {
                            const state=slot(bodySlots,response,'Body');state.abort=s.signal;
                            if(s.method==='HEAD')state.bytes=null;
                            resolve(response);
                        }catch(e){reject(e);}
                    },error=>{if(finished)return;finished=true;cleanup();reject(error);}]);
                }catch(e){finished=true;cleanup();host.cancel(pair.id);reject(e);}
                if(sig.aborted)abort(sig.reason);
            });
        }catch(e){return reject(e);}
    }
    for(const [C,name]of [[Headers,'Headers'],[Request,'Request'],[Response,'Response'],[AbortSignal,'AbortSignal'],[AbortController,'AbortController']]) {
        define(C.prototype,Symbol.toStringTag,{value:name,configurable:true});
        for(const key of Object.getOwnPropertyNames(C.prototype)){if(key==='constructor'||key==='_bytes')continue;const d=Object.getOwnPropertyDescriptor(C.prototype,key);d.enumerable=true;define(C.prototype,key,d);}
    }
    return {
        Headers,Request,Response,AbortSignal,AbortController,fetch,
        initialize(){NativeURL=globalThis.URL;Params=globalThis.URLSearchParams;paramsString=Params.prototype.toString;Encoder=globalThis.TextEncoder;encode=Encoder.prototype.encode;Decoder=globalThis.TextDecoder;decode=Decoder.prototype.decode;},
        initializeBlobs(api){blobAPI=api;},
        initializeObjectURLs(api){objectURLAPI=api;},
        xhrBody(value){const binary=isBuffer(value)||!!(blobAPI&&blobAPI.brand(value));return {...extractBody(value),binary};},
        response(status,url,raw,text,bytes,redirected,statusText,type) {
            const headers=makeHeaders(undefined,'response');
            for(const line of raw.split(/\r?\n/)){const i=line.indexOf(':');if(i>0&&!line.startsWith('HTTP/'))appendHeader(headerSlots.get(headers),normalizeName(line.slice(0,i)),normalizeValue(line.slice(i+1)));}
            headerSlots.get(headers).guard='immutable';
            if(statusText===undefined){const m=/^HTTP\/\S+\s+\d{3}(?:[ \t]+([^\r\n]*))?/.exec(raw);statusText=m?(m[1]||''):'';}
            // Negotiated native metadata is not an HTTP field: webfetch only
            // copies server fields containing ':', so a server cannot forge it.
            const transport=/^HTTP\/Nocturne-Meta cors=([01]) redirected=([01])\r?$/m.exec(raw);
            if(transport){type=transport[1]==='1'?'cors':'basic';redirected=transport[2]==='1';}
            if(type===undefined)type=new NativeURL(url).origin===new NativeURL(host.url()).origin?'basic':'cors';
            return responseObject({status,statusText,url:usv(url).split('#')[0],headers,redirected:!!redirected,type},
                {bytes:nullStatuses.has(status)?null:copyBytes(bytes),used:false,abort:null});
        }
    };
})();
const {Headers,Request,Response,AbortSignal,AbortController,fetch}=fetchBridge;
