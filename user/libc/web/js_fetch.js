/* Fetch API (https://fetch.spec.whatwg.org/#fetch-api, consulted 2026-10-28): Headers, Request,
   Response, the Body mixin and fetch(), one definition for a Window and a Worker realm.
   fetchInterfaces(realm) builds them over what differs between the two:
     realm.signals    the realm's AbortSignal (js_abort.js's abortInterfaces, in both realms):
                      is, aborted, reason, follow, any (a dependent signal) and controller.
     realm.transport  the native transport that carries a request: noCors(), streaming(),
                      fetch(query, notify) -> {id, promise}, resume(id), cancel(id). A Window's is
                      js.c's (host.fetch); a Worker's is its parent window's, through
                      browserjsworker.c, js_worker.c and js.c (NJW_LOAD, NJW_FRAME, NJW_RESUME).
     realm.baseURL(), origin(), clientReferrer(), referrerPolicy(), url()
                      the realm's settings object: API base URL, origin, the "client" referrer
                      source and its referrer policy (enum webnet_referrer_policy).
     realm.dataURL(url) -> [bytes, type], task(fn) -> id, clearTask(id), digest(algorithm, bytes),
     realm.initiator(id) (optional: XHR's Resource Timing initiator).
   CORS and its preflight, cookies and the credentials mode, Mixed Content, the redirect modes,
   the HTTP cache modes, opaque filtering and the Cross-Origin-Resource-Policy check are the
   native transport's (webnet.c, webfetch.c): this layer sends a request's modes with it and
   never relaxes them. Opaque responses enter a realm already redacted.
   Body bytes are real snapshots, not String(BufferSource). Response and Blob bodies expose real
   Streams with native headers/chunk/end flow control. A Request keeps a ReadableStream body;
   the native transport sends whole bodies only, so fetching one is Fetch's network error for an
   HTTP/1.x connection. Manual redirects fail explicitly (NotSupportedError). Native no-cors
   fetches perform actual HTTP and expose an empty opaque response after a cross-origin hop.
   Buffered keepalive requests use a native, document-independent transport; Fetch's 64 KiB
   inflight body accounting belongs to that native fetch group. Request cache modes use the
   native partitioned HTTP response cache; only-if-cached asks that cache for a stored response
   without network I/O. HTTP-generated cache headers belong after CORS preflight, not in the
   author's Request.headers (https://fetch.spec.whatwg.org/#http-network-or-cache-fetch).
   Page-modifiable built-ins (Array, String, RegExp and URL prototypes, Object.prototype) are
   not consulted on private records: methods are captured here and records have no prototype.
   Included after DOMException; initialize() after js_encoding.js and js_url.js. */
function fetchInterfaces(realm) {
    const signals=realm.signals, transport=realm.transport;
    const headerSlots=new WeakMap(), requestSlots=new WeakMap(), responseSlots=new WeakMap();
    const bodySlots=new WeakMap(), networkSlots=new WeakMap();
    const NativePromise=Promise, then=Promise.prototype.then, apply=Reflect.apply;
    const NativeWeakRef=WeakRef, deref=WeakRef.prototype.deref;
    const registerFinalizer=FinalizationRegistry.prototype.register,unregisterFinalizer=FinalizationRegistry.prototype.unregister;
    const networkFinalizer=new FinalizationRegistry(id=>transport.cancel(id));
    const define=Object.defineProperty, create=Object.create, setPrototype=Object.setPrototypeOf;
    const ownKeys=Reflect.ownKeys, getOwnProperty=Reflect.getOwnPropertyDescriptor, freeze=Object.freeze;
    const weakGet=WeakMap.prototype.get, weakSet=WeakMap.prototype.set, weakHas=WeakMap.prototype.has;
    const NativeString=String, NativeTypeError=TypeError, iteratorKey=Symbol.iterator;
    // These maps contain the exclusive native transport buffer. Page changes
    // to WeakMap.prototype must not expose private records through callbacks.
    for(const map of [headerSlots,requestSlots,responseSlots,bodySlots,networkSlots]) {
        define(map,'get',{value:key=>apply(weakGet,map,[key])});
        define(map,'set',{value:(key,value)=>apply(weakSet,map,[key,value])});
        define(map,'has',{value:key=>apply(weakHas,map,[key])});
    }
    /* Private records and lists: no prototype, so a missing field or index never reaches a
       getter or setter the page put on Object.prototype or Array.prototype. */
    const record=object=>setPrototype(object,null);
    const newList=()=>setPrototype([],null);
    const add=(list,value)=>{list[list.length]=value;};
    const pair=(name,value)=>{const p=newList();p[0]=name;p[1]=value;return p;};
    const charCode=String.prototype.charCodeAt, strSlice=String.prototype.slice, strIndex=String.prototype.indexOf;
    const toLower=String.prototype.toLowerCase, toUpper=String.prototype.toUpperCase, wellFormed=String.prototype.toWellFormed;
    const startsWith=String.prototype.startsWith, regexExec=RegExp.prototype.exec;
    const matches=(pattern,text)=>apply(regexExec,pattern,[text])!==null;
    const code=(text,i)=>apply(charCode,text,[i]);
    const slice=(text,a,b)=>apply(strSlice,text,[a,b]);
    const indexOf=(text,search,from=0)=>apply(strIndex,text,[search,from]);
    const lower=text=>apply(toLower,text,[]);
    const beginsWith=(text,prefix)=>apply(startsWith,text,[prefix]);
    function storeBody(object,body) {
        // Missing stream/releaseBytes fields must not invoke an author accessor
        // on Object.prototype with the private Body as its receiver.
        setPrototype(body,null);
        if(!('stream' in body))body.stream=null;
        if(!('releaseBytes' in body))body.releaseBytes=false;
        bodySlots.set(object,body);
    }
    const promiseConstructor=freeze({[Symbol.species]:NativePromise});
    const U8=Uint8Array, AB=ArrayBuffer, isView=ArrayBuffer.isView, taSet=Uint8Array.prototype.set;
    const getter=(proto,key)=>Object.getOwnPropertyDescriptor(proto,key).get;
    const abLength=getter(AB.prototype,'byteLength'), abSlice=AB.prototype.slice, ta=Object.getPrototypeOf(U8.prototype);
    const taBuffer=getter(ta,'buffer'), taOffset=getter(ta,'byteOffset'), taLength=getter(ta,'byteLength');
    const taTag=getter(ta,Symbol.toStringTag), dvBuffer=getter(DataView.prototype,'buffer');
    const dvOffset=getter(DataView.prototype,'byteOffset'), dvLength=getter(DataView.prototype,'byteLength');
    const stringify=JSON.stringify, parse=JSON.parse;
    const token=/^[!#$%&'*+.^_`|~0-9A-Za-z-]+$/, notByte=/[^\x00-\xff]/;
    const forbidden=/^(accept-charset|accept-encoding|access-control-request-headers|access-control-request-method|connection|content-length|cookie2?|date|dnt|expect|host|keep-alive|origin|referer|set-cookie|te|trailer|transfer-encoding|upgrade|via)$|^(proxy-|sec-)/;
    const corsUnsafe=/[\x00-\x08\x0a-\x1f"():<>?@\[\\\]{}\x7f]/, languageValue=/^[0-9A-Za-z *,\-.;=]*$/, reasonPhrase=/[^\t\x20-\x7e\x80-\xff]/;
    const nullStatuses=[101,103,204,205,304], nullStatus=status=>{for(let i=0;i<nullStatuses.length;i++)if(nullStatuses[i]===status)return true;return false;};
    const cacheModes=['default','no-store','reload','no-cache','force-cache','only-if-cached'];
    const referrerPolicies=['','no-referrer','no-referrer-when-downgrade','same-origin','origin','strict-origin','origin-when-cross-origin','strict-origin-when-cross-origin','unsafe-url'];
    const requestModes=['navigate','same-origin','no-cors','cors'], credentialModes=['omit','same-origin','include'];
    const redirectModes=['follow','error','manual'], duplexModes=['half'], priorities=['high','low','auto'];
    const position=(list,value)=>{for(let i=0;i<list.length;i++)if(list[i]===value)return i;return -1;};
    const sriAlphabet='ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/';
    /* W3C SRI 3.3.2--3.3.4 and Fetch main-fetch integrity handling.
       A malformed supported stronger hash must not downgrade to a matching
       weaker one. Unknown algorithms/options are ignored as specified. */
    function integrityMetadata(text) {
        let strength=0,algorithm='',values=newList();
        const space=c=>c===9||c===10||c===12||c===13||c===32;
        for(let i=0;i<text.length;) {
            if(space(code(text,i))){i++;continue;}
            const start=i;while(i<text.length&&!space(code(text,i)))i++;
            const item=slice(text,start,i),option=indexOf(item,'?');
            const expression=option<0?item:slice(item,0,option);
            const dash=indexOf(expression,'-'),name=dash<0?expression:slice(expression,0,dash);
            const rank=name==='sha256'?1:name==='sha384'?2:name==='sha512'?3:0;
            if(!rank||rank<strength)continue;
            if(rank>strength){strength=rank;algorithm=rank===1?'SHA-256':rank===2?'SHA-384':'SHA-512';values=newList();}
            const next=dash<0?-1:indexOf(expression,'-',dash+1);
            add(values,dash<0?'':slice(expression,dash+1,next<0?expression.length:next));
        }
        return record({strength,algorithm,values});
    }
    function verifyIntegrity(response,metadata,signal) {
        if(!metadata)return response;
        const meta=slot(responseSlots,response,'Response'),body=slot(bodySlots,response,'Body');
        /* Even unrecognized nonempty metadata cannot turn an opaque response
           into integrity-eligible bytes or reveal its internal body. */
        if(meta.type==='opaque'||meta.type==='opaqueredirect'||meta.type==='error')
            throw new TypeError('Response is not eligible for integrity verification');
        if(signals.aborted(signal))throw signals.reason(signal);
        if(!metadata.strength)return response;
        if(body.stream)throw new TypeError('Integrity response was published before buffering completed');
        const bytes=body.bytes===null?new AB(0):body.bytes;
        const matched=integrityBytes(bytes,metadata);
        if(signals.aborted(signal))throw signals.reason(signal);
        if(matched)return response;
        /* The response has never escaped to author code. Release its exclusive
           bytes on failure; an integrity error is a Fetch network TypeError. */
        body.bytes=null;body.used=true;
        throw new TypeError('Subresource integrity mismatch');
    }
    function integrityBytes(bytes,metadata) {
        if(!metadata.strength)return true;
        const digest=realm.digest(metadata.algorithm,bytes),size=apply(abLength,digest,[]);
        if(size!==(metadata.strength===1?32:metadata.strength===2?48:64))throw new TypeError('Invalid integrity digest');
        const hash=new U8(digest);let encoded='';
        for(let i=0;i<size;i+=3) {
            const a=hash[i],b=i+1<size?hash[i+1]:0,c=i+2<size?hash[i+2]:0;
            encoded+=sriAlphabet[a>>>2]+sriAlphabet[((a&3)<<4)|(b>>>4)]+
                (i+1<size?sriAlphabet[((b&15)<<2)|(c>>>6)]:'=')+(i+2<size?sriAlphabet[c&63]:'=');
        }
        for(let i=0;i<metadata.values.length;i++)if(encoded===metadata.values[i])return true;
        return false;
    }
    let NativeURL, paramsString, Encoder, encode, Decoder, decode;
    let urlHref, urlOrigin, urlProtocol, urlPathname, urlUsername, urlPassword, setUsername, setPassword, setHash;
    let blobAPI,formDataAPI,objectURLAPI,streams;
    const str=v=>{if(typeof v==='symbol')throw new TypeError('Cannot convert Symbol to string');return NativeString(v);};
    const usv=v=>apply(wellFormed,str(v),[]);
    const byteString=v=>{const t=str(v);if(matches(notByte,t))throw new TypeError('Expected ByteString');return t;};
    const slot=(map,v,label)=>{const s=map.get(v);if(!s)throw new TypeError('Illegal '+label+' receiver');return s;};
    const notSupported=message=>new DOMException(message,'NotSupportedError');
    const reject=e=>new NativePromise((_,r)=>r(e));
    const httpSpace=c=>c===9||c===10||c===13||c===32, tabSpace=c=>c===9||c===32;
    function trim(text,space) {
        let start=0,end=text.length;
        while(start<end&&space(code(text,start)))start++;
        while(end>start&&space(code(text,end-1)))end--;
        return start||end<text.length?slice(text,start,end):text;
    }
    const normalizeName=v=>{v=lower(byteString(v));if(!matches(token,v))throw new TypeError('Invalid header name');return v;};
    function normalizeValue(v) {
        v=trim(byteString(v),httpSpace);
        for(let i=0;i<v.length;i++){const c=code(v,i);if(c===0||c===10||c===13)throw new TypeError('Invalid header value');}
        return v;
    }
    /* The URL parser's record, read through the URL prototype's own accessors as this file
       captured them. */
    function parseURL(input,base) {
        try{return new NativeURL(input,base);}catch(_){throw new TypeError('Invalid URL');}
    }
    const href=url=>apply(urlHref,url,[]), originOf=url=>apply(urlOrigin,url,[]);
    function forbiddenRequest(name,value) {
        if(matches(forbidden,name))return true;
        if(name!=='x-http-method'&&name!=='x-http-method-override'&&name!=='x-method-override')return false;
        // "get, decode, and split" the value: each comma-separated part, HTTP tab or space stripped.
        for(let start=0;;) {
            const comma=indexOf(value,',',start),part=trim(slice(value,start,comma<0?value.length:comma),tabSpace);
            const method=apply(toUpper,part,[]);
            if(method==='CONNECT'||method==='TRACE'||method==='TRACK')return true;
            if(comma<0)return false;
            start=comma+1;
        }
    }
    /* A no-CORS-safelisted request-header: its name is one of four and (name, value) is a
       CORS-safelisted request-header. */
    function noCorsSafe(name,value) {
        if(value.length>128)return false;
        if(name==='accept')return !matches(corsUnsafe,value);
        if(name==='accept-language'||name==='content-language')return matches(languageValue,value);
        if(name==='content-type') {
            if(matches(corsUnsafe,value))return false;
            const semicolon=indexOf(value,';'),essence=lower(trim(semicolon<0?value:slice(value,0,semicolon),httpSpace));
            return essence==='application/x-www-form-urlencoded'||essence==='multipart/form-data'||essence==='text/plain';
        }
        return false;
    }
    function canChange(s,name,value) {
        if(s.guard==='immutable')throw new TypeError('Headers are immutable');
        if(s.guard==='request'||s.guard==='request-no-cors')if(forbiddenRequest(name,value))return false;
        if(s.guard==='response'&&(name==='set-cookie'||name==='set-cookie2'))return false;
        return true;
    }
    function headerValue(s,name) {
        let value=null;const list=s.list;
        for(let i=0;i<list.length;i++)if(list[i][0]===name)value=value===null?list[i][1]:value+', '+list[i][1];
        return value;
    }
    function hasHeader(s,name){const list=s.list;for(let i=0;i<list.length;i++)if(list[i][0]===name)return true;return false;}
    function withoutName(list,name){const out=newList();for(let i=0;i<list.length;i++)if(list[i][0]!==name)add(out,list[i]);return out;}
    function appendHeader(s,name,value) {
        if(!canChange(s,name,value))return;
        if(s.guard==='request-no-cors') {
            const previous=headerValue(s,name);
            if(!noCorsSafe(name,previous===null?value:previous+', '+value))return;
        }
        add(s.list,pair(name,value));
        if(s.guard==='request-no-cors')s.list=withoutName(s.list,'range');
    }
    /* Web IDL "create a sequence from an iterable", each value converted as it arrives. */
    function sequence(object,method,convert) {
        const iterator=apply(method,object,[]);
        if(iterator===null||(typeof iterator!=='object'&&typeof iterator!=='function'))throw new TypeError('Iterator is not an object');
        const next=iterator.next,out=newList();
        for(;;) {
            const result=apply(next,iterator,[]);
            if(result===null||(typeof result!=='object'&&typeof result!=='function'))throw new TypeError('Iterator result is not an object');
            if(result.done)return out;
            add(out,convert(result.value));
        }
    }
    function headerPair(value) {
        if(value===null||(typeof value!=='object'&&typeof value!=='function'))throw new TypeError('Expected header pair');
        const method=value[iteratorKey];
        if(typeof method!=='function')throw new TypeError('Header pair is not iterable');
        return sequence(value,method,byteString);
    }
    /* Web IDL: HeadersInit, (sequence<sequence<ByteString>> or record<ByteString, ByteString>),
       converted to a list of pairs. A sequence's items are checked by fill, a record's are
       pairs already. */
    function headersInit(value) {
        if(value===null||(typeof value!=='object'&&typeof value!=='function'))throw new TypeError('Expected HeadersInit');
        const method=value[iteratorKey];
        if(method!==undefined&&method!==null) {
            if(typeof method!=='function')throw new TypeError('HeadersInit is not iterable');
            return sequence(value,method,headerPair);
        }
        const keys=ownKeys(value),out=newList();
        for(let i=0;i<keys.length;i++) {
            const key=keys[i],desc=getOwnProperty(value,key);
            if(desc===undefined||!desc.enumerable)continue;
            const name=byteString(key);
            add(out,pair(name,byteString(value[key])));
        }
        return out;
    }
    /* Fetch "fill" a Headers from a converted HeadersInit. */
    function fillHeaders(s,list) {
        for(let i=0;i<list.length;i++) {
            const item=list[i];
            if(item.length!==2)throw new TypeError('Header pair must contain two items');
            appendHeader(s,normalizeName(item[0]),normalizeValue(item[1]));
        }
    }
    function copyList(list){const out=newList();for(let i=0;i<list.length;i++)add(out,pair(list[i][0],list[i][1]));return out;}
    /* A Headers object of this realm: init is a converted HeadersInit or undefined. */
    function makeHeaders(init,guard='none') {
        const h=create(Headers.prototype),s=record({guard:guard==='immutable'?'none':guard,list:newList()});
        headerSlots.set(h,s);if(init!==undefined)fillHeaders(s,init);s.guard=guard;return h;
    }
    /* A Headers object whose header list is a copy of source's, with guard. */
    function copyHeaders(source,guard) {
        const h=create(Headers.prototype);
        headerSlots.set(h,record({guard,list:copyList(headerSlots.get(source).list)}));return h;
    }
    function sortedHeaders(s) {
        const names=newList(),list=s.list,out=[];
        for(let i=0;i<list.length;i++) {
            const name=list[i][0];let at=names.length;
            if(position(names,name)>=0)continue;
            while(at>0&&names[at-1]>name){names[at]=names[at-1];at--;}
            names[at]=name;
        }
        for(let n=0;n<names.length;n++) {
            const name=names[n];
            if(name==='set-cookie'){for(let i=0;i<list.length;i++)if(list[i][0]===name)define(out,out.length,{value:[name,list[i][1]],writable:true,enumerable:true,configurable:true});}
            else define(out,out.length,{value:[name,headerValue(s,name)],writable:true,enumerable:true,configurable:true});
        }
        return out;
    }
    class Headers {
        constructor(init=undefined){
            const converted=init===undefined?undefined:headersInit(init);
            const s=record({guard:'none',list:newList()});headerSlots.set(this,s);if(converted!==undefined)fillHeaders(s,converted);
        }
        append(name,value){const s=slot(headerSlots,this,'Headers');if(arguments.length<2)throw new TypeError('append requires two arguments');name=byteString(name);value=byteString(value);appendHeader(s,normalizeName(name),normalizeValue(value));}
        set(name,value) {
            const s=slot(headerSlots,this,'Headers');if(arguments.length<2)throw new TypeError('set requires two arguments');
            name=byteString(name);value=byteString(value);
            name=normalizeName(name);value=normalizeValue(value);if(!canChange(s,name,value))return;
            if(s.guard==='request-no-cors'&&!noCorsSafe(name,value))return;
            const list=s.list,out=newList();let placed=false;
            for(let i=0;i<list.length;i++){if(list[i][0]!==name){add(out,list[i]);continue;}if(!placed){add(out,pair(name,value));placed=true;}}
            if(!placed)add(out,pair(name,value));
            s.list=out;
            if(s.guard==='request-no-cors')s.list=withoutName(s.list,'range');
        }
        delete(name) {
            const s=slot(headerSlots,this,'Headers');if(!arguments.length)throw new TypeError('delete requires name');name=normalizeName(name);
            if(!canChange(s,name,''))return;
            if(s.guard==='request-no-cors'&&name!=='accept'&&name!=='accept-language'&&name!=='content-language'&&name!=='content-type'&&name!=='range')return;
            if(!hasHeader(s,name))return;
            s.list=withoutName(s.list,name);
            if(s.guard==='request-no-cors')s.list=withoutName(s.list,'range');
        }
        get(name){const s=slot(headerSlots,this,'Headers');if(!arguments.length)throw new TypeError('get requires name');return headerValue(s,normalizeName(name));}
        has(name){const s=slot(headerSlots,this,'Headers');if(!arguments.length)throw new TypeError('has requires name');return hasHeader(s,normalizeName(name));}
        getSetCookie(){const list=slot(headerSlots,this,'Headers').list,out=[];for(let i=0;i<list.length;i++)if(list[i][0]==='set-cookie')define(out,out.length,{value:list[i][1],writable:true,enumerable:true,configurable:true});return out;}
        entries(){return headersIterator(slot(headerSlots,this,'Headers'),2);}
        keys(){return headersIterator(slot(headerSlots,this,'Headers'),0);}
        values(){return headersIterator(slot(headerSlots,this,'Headers'),1);}
        forEach(fn,thisArg){const s=slot(headerSlots,this,'Headers');if(typeof fn!=='function')throw new TypeError('Expected callback');for(let i=0;;i++){const list=sortedHeaders(s);if(i>=list.length)return;apply(fn,thisArg,[list[i][1],list[i][0],this]);}}
    }
    /* Web IDL's default iterator objects of a pair iterator: Headers's value pairs are sort and
       combine of its header list, taken again at each next(). kind: 0 key, 1 value, 2 both. */
    const iteratorSlots=new WeakMap();
    define(iteratorSlots,'get',{value:key=>apply(weakGet,iteratorSlots,[key])});
    define(iteratorSlots,'set',{value:(key,value)=>apply(weakSet,iteratorSlots,[key,value])});
    const HeadersIteratorPrototype=create(Object.getPrototypeOf(Object.getPrototypeOf([][iteratorKey]())));
    define(HeadersIteratorPrototype,'next',{writable:true,enumerable:true,configurable:true,value:function next(){
        const it=slot(iteratorSlots,this,'Headers Iterator'),list=sortedHeaders(it.headers);
        if(it.index>=list.length)return {value:undefined,done:true};
        const entry=list[it.index++];
        return {value:it.kind===0?entry[0]:it.kind===1?entry[1]:entry,done:false};
    }});
    define(HeadersIteratorPrototype,Symbol.toStringTag,{value:'Headers Iterator',configurable:true});
    function headersIterator(headers,kind){const it=create(HeadersIteratorPrototype);iteratorSlots.set(it,record({headers,kind,index:0}));return it;}
    // @@iterator is the same function object as entries (Web IDL "iterable declarations").
    define(Headers.prototype,iteratorKey,{value:Headers.prototype.entries,writable:true,configurable:true});
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
        const n=apply(taLength,source,[]),result=new U8(n);apply(taSet,result,[source]);
        return apply(taBuffer,result,[]);
    }
    function isBuffer(input){if(isView(input))return true;try{apply(abLength,input,[]);return true;}catch(_){return false;}}
    function isParams(input){if(input===null||typeof input!=='object')return false;try{apply(paramsString,input,[]);return true;}catch(_){return false;}}
    /* Web IDL: BodyInit, (ReadableStream or Blob or BufferSource or FormData or URLSearchParams
       or USVString). Platform objects stay what they are; anything else becomes a string. */
    function bodyInit(value) {
        if(value!==null&&(typeof value==='object'||typeof value==='function')) {
            if(isBuffer(value)||(blobAPI&&blobAPI.brand(value))||(formDataAPI&&formDataAPI.brand(value))||
               (streams&&streams.brand(value))||isParams(value))return value;
        }
        return usv(value);
    }
    /* Fetch "extract a body" of a converted BodyInit. */
    function extractBody(value,keepalive=false) {
        if(value==null)return record({bytes:null,type:null});
        if(typeof value==='string')return record({bytes:apply(taBuffer,apply(encode,new Encoder(),[value]),[]),type:'text/plain;charset=UTF-8'});
        if(isBuffer(value))return record({bytes:copyBytes(value),type:null});
        if(blobAPI&&blobAPI.brand(value))return record({bytes:blobAPI.bytes(value),type:blobAPI.type(value)||null});
        if(formDataAPI&&formDataAPI.brand(value)){const multipart=formDataAPI.multipart(value);return record({bytes:multipart.bytes,type:multipart.type});}
        if(streams&&streams.brand(value)) {
            if(keepalive)throw new TypeError('A keepalive request cannot have a stream body');
            if(streams.locked(value)||streams.disturbed(value))throw new TypeError('Body stream is locked or disturbed');
            return record({bytes:null,type:null,stream:value});
        }
        if(isParams(value))return record({bytes:apply(taBuffer,apply(encode,new Encoder(),[apply(paramsString,value,[])]),[]),type:'application/x-www-form-urlencoded;charset=UTF-8'});
        return record({bytes:apply(taBuffer,apply(encode,new Encoder(),[usv(value)]),[]),type:'text/plain;charset=UTF-8'});
    }
    /* A body whose source is null: an author's ReadableStream, not bytes this realm holds. */
    const sourceless=b=>b!==null&&b.bytes===null&&!!b.stream;
    function bodyStream(b) {
        if(b.stream)return b.stream;
        if(b.bytes===null)return null;
        b.stream=streams.fromBytes(b.bytes,()=>{
            if(b.abort&&signals.aborted(b.abort))throw signals.reason(b.abort);
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
        if((b.bytes!==null||b.stream)&&b.abort&&signals.aborted(b.abort))return reject(signals.reason(b.abort));
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
                else if(kind==='formData'){
                    const s=requestSlots.get(object)||responseSlots.get(object);
                    if(!formDataAPI)throw notSupported('FormData is not initialized');
                    resolve(formDataAPI.parse(bytes,headerValue(headerSlots.get(s.headers),'content-type')||''));
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
        for(const name of ['text','json','arrayBuffer','bytes','blob','formData'])define(proto,name,{configurable:true,enumerable:true,writable:true,value:function(){return consume(this,name);}});
    }
    function dictionary(v){if(v===undefined||v===null)return null;if(typeof v!=='object'&&typeof v!=='function')throw new TypeError('Expected dictionary');return v;}
    function enumValue(v,allowed,label){v=str(v);if(position(allowed,v)<0)throw new TypeError('Invalid '+label);return v;}
    /* Web IDL: a RequestInit, its members read once each, in lexicographic order, each
       converted as it is read. empty: whether no member exists. */
    function requestInit(value) {
        const init=dictionary(value),out=record({empty:true});
        if(init===null)return out;
        let v;
        const has=(key,converted)=>{out[key]=converted;out.empty=false;};
        if((v=init.body)!==undefined)has('body',v===null?null:bodyInit(v));
        if((v=init.cache)!==undefined)has('cache',enumValue(v,cacheModes,'cache mode'));
        if((v=init.credentials)!==undefined)has('credentials',enumValue(v,credentialModes,'credentials mode'));
        if((v=init.duplex)!==undefined)has('duplex',enumValue(v,duplexModes,'duplex mode'));
        if((v=init.headers)!==undefined)has('headers',headersInit(v));
        if((v=init.integrity)!==undefined)has('integrity',str(v));
        if((v=init.keepalive)!==undefined)has('keepalive',!!v);
        if((v=init.method)!==undefined)has('method',byteString(v));
        if((v=init.mode)!==undefined)has('mode',enumValue(v,requestModes,'request mode'));
        if((v=init.priority)!==undefined)has('priority',enumValue(v,priorities,'request priority'));
        if((v=init.redirect)!==undefined)has('redirect',enumValue(v,redirectModes,'redirect mode'));
        if((v=init.referrer)!==undefined)has('referrer',usv(v));
        if((v=init.referrerPolicy)!==undefined)has('referrerPolicy',enumValue(v,referrerPolicies,'referrer policy'));
        if((v=init.signal)!==undefined){if(v!==null&&!signals.is(v))throw new TypeError('Expected AbortSignal');has('signal',v);}
        if((v=init.window)!==undefined)has('window',v);
        return out;
    }
    function makeURL(value){return parseURL(usv(value),realm.baseURL());}
    function methodValue(method){const upper=apply(toUpper,method,[]);if(!matches(token,method)||upper==='CONNECT'||upper==='TRACE'||upper==='TRACK')throw new TypeError('Invalid or forbidden HTTP method');return upper==='DELETE'||upper==='GET'||upper==='HEAD'||upper==='OPTIONS'||upper==='POST'||upper==='PUT'?upper:method;}
    function cloneBody(b){if(unusable(b))throw new TypeError('Body already consumed or locked');let stream;
        if(b.stream){const branches=streams.tee(b.stream);b.stream=branches[0];stream=branches[1];}
        return record({bytes:b.bytes===null?null:copyBytes(b.bytes),stream,used:false,abort:b.abort});}
    function requestObject(s,b){const object=create(Request.prototype);requestSlots.set(object,s);storeBody(object,b);return object;}
    function requestCopy(s,b){return requestObject(record({...s,headers:copyHeaders(s.headers,headerSlots.get(s.headers).guard),signal:signals.any([s.signal])}),b);}
    /* blobEntry is the URL record's blob URL entry: the URL parser resolves a blob: URL against the
       realm's store when the Request parses it (FileAPI "resolve a blob URL"), so a URL revoked
       later still fetches the Blob, in the Request and its clones. */
    const newRequestRecord=url=>record({url,method:'GET',mode:'cors',credentials:'same-origin',cache:'default',redirect:'follow',
        referrer:'about:client',referrerPolicy:'',integrity:'',keepalive:false,destination:'',priority:'auto',preflight:false,
        blobEntry:objectURLAPI&&beginsWith(url,'blob:')?objectURLAPI.resolve(url):null});
    class Request {
        constructor(input,init=undefined) {
            if(!arguments.length)throw new TypeError('Request requires input');
            // Web IDL: RequestInfo (Request or USVString), then the RequestInit dictionary.
            const old=requestSlots.get(input),source=old?null:usv(input),i=requestInit(init);
            let s,fallbackMode=null,signal=null;
            if(!old) {
                const url=makeURL(source);
                if(apply(urlUsername,url,[])||apply(urlPassword,url,[]))throw new TypeError('Request URL cannot contain credentials');
                s=newRequestRecord(href(url));fallbackMode='cors';
            } else {s=record({...old});signal=old.signal;}
            if(i.window!==undefined&&i.window!==null)throw new TypeError('RequestInit window must be null');
            if(!i.empty){if(s.mode==='navigate')s.mode='same-origin';s.referrer='about:client';s.referrerPolicy='';}
            if(i.referrer!==undefined) {
                if(i.referrer==='')s.referrer='';
                else {
                    const u=makeURL(i.referrer);
                    s.referrer=apply(urlProtocol,u,[])==='about:'&&apply(urlPathname,u,[])==='client'||originOf(u)!==realm.origin()?'about:client':href(u);
                }
            }
            if(i.referrerPolicy!==undefined)s.referrerPolicy=i.referrerPolicy;
            const mode=i.mode!==undefined?i.mode:fallbackMode;
            if(mode==='navigate')throw new TypeError('Cannot construct navigate request');
            if(mode!==null)s.mode=mode;
            if(i.credentials!==undefined)s.credentials=i.credentials;
            if(i.cache!==undefined)s.cache=i.cache;
            if(s.cache==='only-if-cached'&&s.mode!=='same-origin')throw new TypeError('only-if-cached requires same-origin mode');
            if(i.redirect!==undefined)s.redirect=i.redirect;
            if(i.integrity!==undefined)s.integrity=i.integrity;
            if(i.keepalive!==undefined)s.keepalive=i.keepalive;
            if(i.method!==undefined)s.method=methodValue(i.method);
            if(i.signal!==undefined)signal=i.signal;
            if(i.priority!==undefined)s.priority=i.priority;
            s.signal=signals.any(signal===null?[]:[signal]);
            // this's headers: request's header list, guard "request", then the mode's guard.
            const headers=create(Headers.prototype),hs=record({guard:'request',list:old?copyList(headerSlots.get(old.headers).list):newList()});
            headerSlots.set(headers,hs);s.headers=headers;
            if(s.mode==='no-cors') {
                if(s.method!=='GET'&&s.method!=='HEAD'&&s.method!=='POST')throw new TypeError('no-cors requires a safelisted method');
                hs.guard='request-no-cors';
            }
            if(!i.empty) {
                const previous=hs.list;hs.list=newList();
                if(i.headers!==undefined)fillHeaders(hs,i.headers);
                else for(let n=0;n<previous.length;n++)appendHeader(hs,previous[n][0],previous[n][1]);
            }
            const inputBody=old?bodySlots.get(input):null,hasInputBody=!!inputBody&&(inputBody.bytes!==null||!!inputBody.stream);
            const override=i.body!==undefined&&i.body!==null;
            if((override||hasInputBody)&&(s.method==='GET'||s.method==='HEAD'))throw new TypeError('GET and HEAD cannot have a body');
            let initBody=null;
            if(override) {
                const extracted=extractBody(i.body,s.keepalive);
                initBody=record({bytes:extracted.bytes,stream:extracted.stream||null,used:false,abort:null});
                if(extracted.type!==null&&!hasHeader(hs,'content-type'))appendHeader(hs,'content-type',extracted.type);
            }
            const inputOrInitBody=initBody!==null?initBody:hasInputBody?inputBody:null;
            s.preflight=false;
            if(sourceless(inputOrInitBody)) {
                if(initBody!==null&&i.duplex===undefined)throw new TypeError('A request with a stream body requires duplex "half"');
                if(s.mode!=='same-origin'&&s.mode!=='cors')throw new TypeError('A request with a stream body requires cors or same-origin mode');
                s.preflight=true;
            }
            let finalBody=inputOrInitBody;
            if(initBody===null&&hasInputBody) {
                if(unusable(inputBody))throw new TypeError('Body already consumed or locked');
                /* Streams "creating a proxy" for inputBody: a new stream that pulls from the input's,
                   which is locked and disturbed from now on; the input's source (its bytes) stays
                   the new body's. Transferring a Request body consumes the input, unlike clone(). */
                finalBody=record({bytes:inputBody.bytes===null?null:copyBytes(inputBody.bytes),
                    stream:inputBody.stream?streams.proxy(inputBody.stream):null,used:false,abort:inputBody.abort});
                inputBody.used=true;
            }
            requestSlots.set(this,s);storeBody(this,finalBody||{bytes:null,used:false,abort:null});
        }
        clone(){const s=slot(requestSlots,this,'Request');return requestCopy(s,cloneBody(slot(bodySlots,this,'Body')));}
        get destination(){return slot(requestSlots,this,'Request').destination;}
        get duplex(){slot(requestSlots,this,'Request');return 'half';}
        get isReloadNavigation(){slot(requestSlots,this,'Request');return false;}
        get isHistoryNavigation(){slot(requestSlots,this,'Request');return false;}
    }
    for(const name of ['url','method','headers','credentials','mode','redirect','signal','cache','referrer','referrerPolicy','integrity','keepalive'])
        define(Request.prototype,name,{configurable:true,enumerable:true,get(){return slot(requestSlots,this,'Request')[name];}});
    function responseObject(s,body) {const object=create(Response.prototype);storeBody(object,body);body.releaseBytes=true;responseSlots.set(object,record(s));return object;}
    /* Web IDL: a ResponseInit, members headers, status and statusText in that order. */
    function responseInit(value) {
        const init=dictionary(value),out=record({headers:undefined,status:200,statusText:''});
        if(init===null)return out;
        let v;
        if((v=init.headers)!==undefined)out.headers=headersInit(v);
        if((v=init.status)!==undefined){let n=+v;n=Number.isFinite(n)?((Math.trunc(n)%65536)+65536)%65536:0;out.status=n;}
        if((v=init.statusText)!==undefined)out.statusText=byteString(v);
        return out;
    }
    /* Fetch "initialize a response": the status, status text, headers and body of a new one. */
    function initializeResponse(i,body) {
        if(i.status<200||i.status>599)throw new RangeError('Response status must be between 200 and 599');
        if(matches(reasonPhrase,i.statusText))throw new TypeError('Invalid statusText');
        const s=record({status:i.status,statusText:i.statusText,headers:makeHeaders(i.headers,'response'),url:'',redirected:false,type:'default'});
        if(body!==null) {
            if(nullStatus(s.status))throw new TypeError('This status cannot have a body');
            if(body.type!==null&&!hasHeader(headerSlots.get(s.headers),'content-type'))appendHeader(headerSlots.get(s.headers),'content-type',body.type);
        }
        return s;
    }
    class Response {
        constructor(body=null,init=undefined) {
            const converted=body===null||body===undefined?null:bodyInit(body),i=responseInit(init);
            const b=converted===null?null:extractBody(converted),s=initializeResponse(i,b);
            responseSlots.set(this,s);storeBody(this,{bytes:b?b.bytes:null,stream:b&&b.stream||null,used:false,abort:null,releaseBytes:true});
        }
        get ok(){const n=slot(responseSlots,this,'Response').status;return n>=200&&n<300;}
        clone(){const s=slot(responseSlots,this,'Response'),b=cloneBody(slot(bodySlots,this,'Body'));return responseObject({...s,headers:copyHeaders(s.headers,headerSlots.get(s.headers).guard)},b);}
        static error(){return responseObject({status:0,statusText:'',headers:makeHeaders(undefined,'immutable'),url:'',redirected:false,type:'error'},{bytes:null,used:false,abort:null});}
        static redirect(url,status=302) {
            if(!arguments.length)throw new TypeError('redirect requires URL');url=usv(url);
            status=+status;status=Number.isFinite(status)?((Math.trunc(status)%65536)+65536)%65536:0;
            const parsed=makeURL(url);
            if(status!==301&&status!==302&&status!==303&&status!==307&&status!==308)throw new RangeError('Invalid redirect status');
            const headers=makeHeaders(undefined,'immutable');add(headerSlots.get(headers).list,pair('location',href(parsed)));
            return responseObject({status,statusText:'',headers,url:'',redirected:false,type:'default'},{bytes:null,used:false,abort:null});
        }
        static json(data,init=undefined) {
            if(!arguments.length)throw new TypeError('json requires data');
            const i=responseInit(init),text=stringify(data);if(text===undefined)throw new TypeError('Value cannot be serialized as JSON');
            const b=record({bytes:apply(taBuffer,apply(encode,new Encoder(),[text]),[]),type:'application/json'});
            return responseObject(initializeResponse(i,b),{bytes:b.bytes,used:false,abort:null});
        }
    }
    for(const name of ['type','url','redirected','status','statusText','headers'])define(Response.prototype,name,{configurable:true,enumerable:true,get(){return slot(responseSlots,this,'Response')[name];}});
    installBody(Request.prototype);installBody(Response.prototype);
    const urlWithoutFragment=url=>{const hash=indexOf(url,'#');return hash<0?url:slice(url,0,hash);};
    function dataFetch(s,body,integrity,observer) {
        if(body.bytes!==null)body.used=true;
        return new NativePromise((resolve,reject)=>{
            let done=false,task=0,unfollow=()=>{};
            const fail=reason=>{
                if(done)return;done=true;unfollow();
                if(task){realm.clearTask(task);task=0;}
                reject(reason);if(observer)observer('end',reason,()=>{});
            };
            const schedule=fn=>{task=realm.task(()=>{task=0;if(done)return;try{fn();}catch(e){fail(e);}});};
            // The transport's observer owns flow control. Resume at most once,
            // and on a new task, just as native headers/chunks/end are queued.
            const resume=fn=>{let used=false;return ()=>{if(used||done)return;used=true;try{schedule(fn);}catch(e){fail(e);}};};
            unfollow=signals.follow(s.signal,fail);
            try {
                schedule(()=>{
                    const local=realm.dataURL(s.url),bytes=s.method==='HEAD'?null:local[0];
                    const headers=makeHeaders(undefined,'immutable');add(headerSlots.get(headers).list,pair('content-type',local[1]));
                    const response=responseObject({status:200,statusText:'OK',url:urlWithoutFragment(s.url),
                        headers,redirected:false,type:'basic'},{bytes,used:false,abort:s.signal});
                    verifyIntegrity(response,integrity,s.signal);
                    if(!observer){done=true;unfollow();resolve(response);return;}
                    const end=()=>{if(done)return;done=true;unfollow();observer('end',undefined,()=>{});};
                    const chunk=()=>{if(bytes!==null&&apply(abLength,bytes,[])!==0)observer('chunk',bytes,resume(end));else end();};
                    const publish=()=>{observer('headers',response,resume(chunk));resolve(response);};
                    if(body.bytes!==null)observer('upload',apply(abLength,body.bytes,[]),resume(publish));else publish();
                });
            }catch(e){fail(e);}
            if(signals.aborted(s.signal))fail(signals.reason(s.signal));
        });
    }
    /* Fetch's "parse a single range header value" with allowWhitespace: [start, end], each null
       when absent, or null for failure. */
    function singleRange(data) {
        if(!beginsWith(data,'bytes'))return null;
        let at=5;
        const space=()=>{while(at<data.length&&tabSpace(code(data,at)))at++;};
        const digits=()=>{const start=at;while(at<data.length&&code(data,at)>=48&&code(data,at)<=57)at++;return at===start?null:+slice(data,start,at);};
        space();if(code(data,at)!==61)return null;at++;space();
        const start=digits();space();
        if(code(data,at)!==45)return null;at++;space();
        const end=digits();
        if(at<data.length||start===null&&end===null||start!==null&&end!==null&&start>end)return null;
        const out=newList();out[0]=start;out[1]=end;return out;
    }
    /* FileAPI's "slice blob" of bytes from start to end (relative to the end when negative). */
    function sliceBytes(bytes,start,end) {
        const size=apply(abLength,bytes,[]);
        const from=start<0?(size+start>0?size+start:0):(start<size?start:size),to=end<0?(size+end>0?size+end:0):(end<size?end:size);
        return apply(abSlice,bytes,[from,to>from?to:from]);
    }
    /* Fetch's scheme fetch for "blob": only GET; the URL record's blob URL entry and FileAPI's
       "obtain a blob object"; the whole Blob, or a Range's slice (status 206) with its
       Content-Range. A Worker's URL its own store does not hold is obtained from its parent
       document's store by the transport (transport.obtainBlob: js.c checks the Worker's storage
       partition); a Window's is a network error. */
    function blobFetch(s,integrity) {
        return new NativePromise((resolve,reject)=>{
            if(s.method!=='GET')throw new TypeError('A blob: URL is fetched with GET only');
            if(!blobAPI)throw notSupported('Blob is not initialized');
            const range=headerValue(headerSlots.get(s.headers),'range');
            const respond=(bytes,type)=>{
                const fullLength=apply(abLength,bytes,[]),headers=makeHeaders(undefined,'immutable'),list=headerSlots.get(headers).list;
                let status=200,body=bytes;
                if(range===null){add(list,pair('content-length',NativeString(fullLength)));add(list,pair('content-type',type));}
                else{
                    const value=singleRange(range);if(value===null)throw new TypeError('Invalid Range for a blob: URL');
                    let start=value[0],end=value[1];
                    if(start===null){start=fullLength-end;end=start+end-1;}
                    else{if(start>=fullLength)throw new TypeError('Range start is past the Blob');if(end===null||end>=fullLength)end=fullLength-1;}
                    body=sliceBytes(bytes,start,end+1);status=206;
                    add(list,pair('content-length',NativeString(apply(abLength,body,[]))));add(list,pair('content-type',type));
                    add(list,pair('content-range','bytes '+NativeString(start)+'-'+NativeString(end)+'/'+NativeString(fullLength)));
                }
                const response=responseObject({status,statusText:status===206?'Partial Content':'OK',url:urlWithoutFragment(s.url),headers,redirected:false,type:'basic'},
                    {bytes:body,used:false,abort:s.signal});
                resolve(verifyIntegrity(response,integrity,s.signal));
            };
            const blob=s.blobEntry?objectURLAPI.obtain(s.blobEntry):null;
            if(blob!==null){respond(blobAPI.bytes(blob),blobAPI.type(blob));return;}
            if(s.blobEntry||!transport.obtainBlob)throw new TypeError('The blob: URL has no Blob in this realm\'s store');
            const started=transport.obtainBlob(s.url),id=started.id;
            let finished=false;
            const unfollow=signals.follow(s.signal,reason=>{if(finished)return;finished=true;transport.cancel(id);reject(reason);});
            define(started.promise,'constructor',{value:promiseConstructor});
            apply(then,started.promise,[obtained=>{if(finished)return;finished=true;unfollow();try{respond(obtained[0],obtained[1]);}catch(e){reject(e);}},
                error=>{if(finished)return;finished=true;unfollow();reject(error);}]);
        });
    }
    /* prepared: input is a Request this bridge made for the fetch (Cache Storage's addAll), fetched
       as it is, without the Request constructor's steps for init. */
    function fetchCore(input,init,observer,forcePreflight=false,destination='',prepared=false) {
        let r,s,b,integrity;
        try {
            r=prepared?input:new Request(input,init);s=requestSlots.get(r);b=bodySlots.get(r);
            if(destination)s.destination=destination;
            if(signals.aborted(s.signal))return reject(signals.reason(s.signal));
            integrity=s.integrity?integrityMetadata(s.integrity):null;
            // Fetch main fetch selects data before same-origin/no-cors/CORS
            // checks. Local data has a basic response and no HTTP transport,
            // redirect, cache miss, preflight, cookie, or request-header wire.
            if(beginsWith(s.url,'data:'))return dataFetch(s,b,integrity,observer);
            // Referrer policy/source are private native metadata, never part
            // of author headers or CORS preflight header classification.
            const noReferrer=s.referrer===''||s.referrerPolicy==='no-referrer';
            let referrer=noReferrer?'':s.referrer==='about:client'?realm.clientReferrer():s.referrer;
            if(referrer){const source=parseURL(referrer);apply(setUsername,source,['']);apply(setPassword,source,['']);apply(setHash,source,['']);referrer=href(source);}
            const referrerPolicy=s.referrerPolicy?position(referrerPolicies,s.referrerPolicy):realm.referrerPolicy();
            if(beginsWith(s.url,'blob:'))return blobFetch(s,integrity);
            if(s.mode==='no-cors'&&!transport.noCors())throw notSupported('This native context has no no-cors transport');
            if(s.redirect==='manual')throw notSupported('Opaque manual redirect responses are not supported');
            // HTTP-network fetch: a body without a source (an author's stream) cannot be sent
            // over an HTTP/1.x connection, and the native transport sends whole bodies only.
            if(sourceless(b))throw new TypeError('The native transport cannot send a streaming request body');
            let raw='';{const list=sortedHeaders(headerSlots.get(s.headers));for(let n=0;n<list.length;n++)raw+=list[n][0]+': '+list[n][1]+'\r\n';}
            let started,channel,channelRef,waiting,transportId=0,done=false,hasBody=false,finishStream=()=>{};
            const liveChannel=()=>channel||(channelRef&&apply(deref,channelRef,[]));
            const resume=()=>{if(!done&&transportId)transport.resume(transportId);};
            /* Integrity responses must be fully verified before either their
               headers/Response or body stream is exposed. Requests without
               metadata retain the existing native streaming flow control. */
            if(transport.streaming()&&streams&&!integrity){
                channel=streams.network(()=>new NativePromise(resolve=>{waiting=resolve;resume();}),reason=>{
                    if(done)return;done=true;finishStream();transport.cancel(transportId);
                    if(waiting){const resolve=waiting;waiting=null;resolve();}
                });
            }
            const notify=channel?(event,value)=>{
                if(done)return true;
                const active=liveChannel();
                if(!active){done=true;finishStream();transport.cancel(transportId);return false;}
                if(event===1){
                    const state=slot(bodySlots,value,'Body'),meta=slot(responseSlots,value,'Response');
                    hasBody=s.method!=='HEAD'&&meta.type!=='opaque'&&!nullStatus(meta.status);
                    state.bytes=null;state.abort=s.signal;
                    if(hasBody&&!observer){state.stream=active.stream;networkSlots.set(active.stream,active);apply(registerFinalizer,networkFinalizer,[active,transportId,active]);channelRef=new NativeWeakRef(active);channel=null;}
                    if(observer){observer('headers',value,resume);return false;}
                    // The body stream's queue has room up to its high-water
                    // mark: keep the transport flowing until it is full.
                    return !hasBody||active.wantsMore();
                }
                if(event===2){
                    if(observer){if(!hasBody)return true;observer('chunk',value,resume);return false;}
                    if(hasBody)active.enqueue(value);
                    if(waiting){const resolve=waiting;waiting=null;resolve();}
                    return !hasBody||active.wantsMore();
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
            started=transport.fetch(record({url:s.url,method:s.method,headers:raw,body:b.bytes,sameOrigin:s.mode==='same-origin',
                credentials:position(credentialModes,s.credentials),forcePreflight:forcePreflight||s.preflight,redirectError:s.redirect==='error',
                cache:position(cacheModes,s.cache),keepalive:s.keepalive,noCors:s.mode==='no-cors',noReferrer,referrer,referrerPolicy,
                destination:s.destination}),notify);
            transportId=started.id;const transportPromise=started.promise;started=null;
            if(observer&&realm.initiator)realm.initiator(transportId);
            if(b.bytes!==null)b.used=true;
            return new NativePromise((resolve,reject)=>{
                let finished=false,pendingReject=reject,unfollow=()=>{};
                const cleanup=()=>{const stop=unfollow;unfollow=()=>{};stop();};
                const abort=reason=>{if(done)return;done=true;finished=true;cleanup();const active=liveChannel();if(active){active.error(reason);apply(unregisterFinalizer,networkFinalizer,[active]);}channel=null;channelRef=null;if(waiting){const resolve=waiting;waiting=null;resolve();}try{transport.cancel(transportId);}finally{if(pendingReject)pendingReject(reason);pendingReject=null;if(observer)observer('end',reason,()=>{});}};
                finishStream=cleanup;
                unfollow=signals.follow(s.signal,abort);
                try {
                    define(transportPromise,'constructor',{value:promiseConstructor});
                    apply(then,transportPromise,[response=>{
                        if(finished)return;finished=true;pendingReject=null;if(!notify)cleanup();
                        try {
                            const state=slot(bodySlots,response,'Body');state.abort=s.signal;
                            if(s.method==='HEAD')state.bytes=null;
                            resolve(verifyIntegrity(response,integrity,s.signal));
                        }catch(e){reject(e);}
                    },error=>{if(finished)return;finished=true;done=true;cleanup();reject(error);}]);
                }catch(e){finished=true;done=true;cleanup();const active=liveChannel();if(active)active.error(e);transport.cancel(transportId);pendingReject=null;reject(e);}
                if(signals.aborted(s.signal))abort(signals.reason(s.signal));
            });
        }catch(e){return reject(e);}
    }
    function fetch(input,init=undefined){if(!arguments.length)return reject(new TypeError('fetch requires input'));return fetchCore(input,init);}
    for(const [C,name]of [[Headers,'Headers'],[Request,'Request'],[Response,'Response']]) {
        define(C.prototype,Symbol.toStringTag,{value:name,configurable:true});
        for(const key of Object.getOwnPropertyNames(C.prototype)){if(key==='constructor')continue;const d=Object.getOwnPropertyDescriptor(C.prototype,key);d.enumerable=true;define(C.prototype,key,d);}
    }
    /* A native header block (CRLF lines: a status line, Name: value fields, the transport's
       HTTP/Nocturne- lines), read line by line. */
    function eachLine(raw,fn){for(let start=0;start<raw.length;){const end=indexOf(raw,'\n',start),stop=end<0?raw.length:end;fn(slice(raw,start,stop>start&&code(raw,stop-1)===13?stop-1:stop));start=stop+1;}}
    const statusLine=/^HTTP\/\S+\s+\d{3}(?:[ \t]+([^\r\n]*))?/;
    const transportMeta=/^HTTP\/Nocturne-Meta cors=([01]) redirected=([01])(?: opaque=([01]))?(?: resource=[0-7])?\r?$/m;
    const opaqueResponse=resource=>responseObject({status:0,statusText:'',url:'',headers:makeHeaders(undefined,'immutable'),redirected:false,type:'opaque',resource},{bytes:null,used:false,abort:null});
    return {
        Headers,Request,Response,fetch,
        /* The engine's own fetch for an element: a track, a media fallback. Its init is the
           engine's, copied without a prototype. */
        fetchFor(destination,input,init){return fetchCore(input,init===undefined?undefined:record({...init}),undefined,false,destination);},
        moduleIntegrity(bytes,metadata){return integrityBytes(bytes,integrityMetadata(metadata));},
        initialize(){
            NativeURL=globalThis.URL;paramsString=globalThis.URLSearchParams.prototype.toString;Encoder=globalThis.TextEncoder;encode=Encoder.prototype.encode;Decoder=globalThis.TextDecoder;decode=Decoder.prototype.decode;
            const url=NativeURL.prototype,accessor=key=>Object.getOwnPropertyDescriptor(url,key);
            urlHref=accessor('href').get;urlOrigin=accessor('origin').get;urlProtocol=accessor('protocol').get;urlPathname=accessor('pathname').get;
            urlUsername=accessor('username').get;urlPassword=accessor('password').get;
            setUsername=accessor('username').set;setPassword=accessor('password').set;setHash=accessor('hash').set;
        },
        initializeBlobs(api){blobAPI=api;},
        initializeFormData(api){formDataAPI=api;},
        initializeStreams(api){streams=api;},
        initializeObjectURLs(api){objectURLAPI=api;},
        xhrBody(value){const binary=isBuffer(value)||!!(blobAPI&&blobAPI.brand(value))||!!(formDataAPI&&formDataAPI.brand(value)),body=extractBody(bodyInit(value));if(body.stream)throw notSupported('XHR stream uploads are unsupported');return {...body,binary};},
        xhrResponse(response) {
            // Only the private XHR completion path calls this. Do not invoke
            // page-mutable Response getters or publish a backing-buffer API.
            const s=slot(responseSlots,response,'Response'),b=slot(bodySlots,response,'Body');
            if(unusable(b)||b.stream)throw new TypeError('XHR body is not an exclusive buffered response');
            const bytes=b.bytes===null?new AB(0):b.bytes;b.bytes=null;b.used=true;
            return {status:s.status,statusText:s.statusText,url:s.url,headers:s.headers,bytes};
        },
        /* XHR's own request: init and its header record are the engine's, read without a prototype. */
        xhrFetch(url,init,observer,forcePreflight){
            const c=signals.controller(),q=record({...init,signal:c.signal});
            if(q.headers!==null&&typeof q.headers==='object')q.headers=record({...q.headers});
            return {promise:fetchCore(url,q,observer,forcePreflight),cancel(){c.abort();}};
        },
        xhrMetadata(response){const s=slot(responseSlots,response,'Response');return {status:s.status,statusText:s.statusText,url:s.url,headers:s.headers};},
        /* Cache Storage's view of this realm's Fetch objects (js_cache_storage.js): a record of a
           Request's or a Response's own request or response (never its page-mutable getters), the
           objects a match makes of stored ones (immutable headers, a new signal), and the fetch of
           add and addAll. A header list is [name, value, ...]; extra is what the broker keeps for
           this realm to read back. Every realm writes and reads the same records. */
        cache:{
            isRequest:v=>requestSlots.has(v),isResponse:v=>responseSlots.has(v),
            newRequest:input=>new Request(input),
            request(r){
                const s=slot(requestSlots,r,'Request'),list=headerSlots.get(s.headers).list,headers=[];
                for(let i=0;i<list.length;i++){define(headers,headers.length,{value:list[i][0],writable:true,enumerable:true,configurable:true});define(headers,headers.length,{value:list[i][1],writable:true,enumerable:true,configurable:true});}
                return {url:s.url,method:s.method,headers,extra:stringify({__proto__:null,m:s.mode,c:s.credentials,k:s.cache,r:s.redirect,
                    f:s.referrer,p:s.referrerPolicy,i:s.integrity,a:s.keepalive,d:s.destination})};
            },
            makeRequest(url,method,headers,x){
                const h=makeHeaders(undefined,'immutable'),list=headerSlots.get(h).list;
                for(let i=0;i+1<headers.length;i+=2)add(list,pair(headers[i],headers[i+1]));
                const s=newRequestRecord(url);
                s.method=method;s.mode=x.m;s.credentials=x.c;s.cache=x.k;s.redirect=x.r;s.referrer=x.f;s.referrerPolicy=x.p;
                s.integrity=x.i;s.keepalive=x.a;s.destination=x.d;s.headers=h;s.signal=signals.controller().signal;
                return requestObject(s,{bytes:null,used:false,abort:null});
            },
            response(r){
                const s=slot(responseSlots,r,'Response'),b=slot(bodySlots,r,'Body'),list=headerSlots.get(s.headers).list,headers=[];
                for(let i=0;i<list.length;i++){define(headers,headers.length,{value:list[i][0],writable:true,enumerable:true,configurable:true});define(headers,headers.length,{value:list[i][1],writable:true,enumerable:true,configurable:true});}
                return {type:s.type,status:s.status,headers,unusable:unusable(b),
                    extra:stringify({__proto__:null,t:s.type,s:s.status,x:s.statusText,u:s.url,r:s.redirected,c:s.resource})};
            },
            /* The body's stream (null for a null body), read by Cache Storage from here on. */
            takeBody(r){return bodyStream(slot(bodySlots,r,'Body'));},
            makeResponse(x,headers,stream){
                const h=makeHeaders(undefined,'immutable'),list=headerSlots.get(h).list;
                for(let i=0;i+1<headers.length;i+=2)add(list,pair(headers[i],headers[i+1]));
                const meta={status:x.s,statusText:x.x,headers:h,url:x.u,redirected:x.r,type:x.t};
                if(x.t==='opaque')meta.resource=x.c;
                return responseObject(meta,{bytes:null,stream,used:false,abort:null});
            },
            /* A fetch of r as it is, which signal can abort too. */
            fetch(r,signal){
                try{
                    const s=slot(requestSlots,r,'Request');
                    const object=requestObject(record({...s,headers:copyHeaders(s.headers,headerSlots.get(s.headers).guard),signal:signals.any([s.signal,signal])}),cloneBody(slot(bodySlots,r,'Body')));
                    return fetchCore(object,undefined,undefined,false,'',true);
                }catch(e){return reject(e);}
            },
            controller(){return signals.controller();}
        },
        /* A response the native transport delivered to this realm. Native completions redact
           opaque internals before entering it; resource is the bits Cache Storage's resource
           policy check needs of the hidden response (js.c's fetch_resource_bits), private. The
           header block is read without author-modifiable regex, string or URL methods. */
        response(status,url,raw,text,bytes,redirected,statusText,type,resource) {
            if(type==='opaque')return opaqueResponse(typeof resource==='number'?resource:2);
            const headers=makeHeaders(undefined,'response'),hs=headerSlots.get(headers);raw=NativeString(raw);
            eachLine(raw,line=>{const colon=indexOf(line,':');if(colon>0&&!beginsWith(line,'HTTP/'))appendHeader(hs,normalizeName(slice(line,0,colon)),normalizeValue(slice(line,colon+1,line.length)));});
            hs.guard='immutable';
            if(statusText===undefined){const m=apply(regexExec,statusLine,[raw]);statusText=m?(m[1]||''):'';}
            // Negotiated native metadata is not an HTTP field: webfetch only
            // copies server fields containing ':', so a server cannot forge it.
            const meta=type===undefined?apply(regexExec,transportMeta,[raw]):null;
            if(meta&&meta[3]==='1')return opaqueResponse(2);
            if(meta){type=meta[1]==='1'?'cors':'basic';redirected=meta[2]==='1';}
            if(type===undefined)type=originOf(parseURL(url))===originOf(parseURL(realm.url()))?'basic':'cors';
            return responseObject({status,statusText,url:urlWithoutFragment(usv(url)),headers,redirected:!!redirected,type},
                // The native hook transfers an exclusive runtime-charged
                // snapshot. Author constructors and clone still copy/tee.
                {bytes:nullStatus(status)?null:bytes,used:false,abort:null});
        }
    };
}
