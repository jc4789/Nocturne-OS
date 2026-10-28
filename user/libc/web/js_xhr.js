/* XMLHttpRequest Standard (https://xhr.spec.whatwg.org/, consulted 2026-10-28): ProgressEvent,
   XMLHttpRequestEventTarget, XMLHttpRequestUpload and XMLHttpRequest, one definition for a Window
   and a Worker realm (XMLHttpRequest is [Exposed=(Window,DedicatedWorker,SharedWorker)],
   ProgressEvent [Exposed=(Window,Worker)]).
   progressEventInterface(Event) makes ProgressEvent; a realm makes it before js_blob.js's
   FileReader takes it. xhrInterfaces(realm) builds the rest over what differs between the realms:
     realm.fetch         the realm's Fetch (fetchInterfaces's bridge). Every request is its xhrFetch
                         or, synchronous, its xhrFetchSync: CORS, credentials, Mixed Content and CSP
                         are its native transport's. xhrBody, xhrResponse and xhrMetadata read
                         bodies and responses without page-mutable getters; mimeType and parseMIME
                         are Fetch's "extract a MIME type" and MIME Sniffing's "parse a MIME type".
     realm.parseURL(url) encoding-parse url relative to the relevant settings object: a Window's
                         document base URL and the document's encoding, a Worker's API base URL and
                         UTF-8. A URL, or null for failure.
     realm.listened(t)   whether one or more event listeners (event handlers among them) are
                         registered on t: send()'s upload listener flag.
     realm.log(text)     a console line: the transport's reason for a failed request.
     realm.document      a Window's documents, null in a Worker: is(value), body(document) ->
                         [Content-Type, serialization], and response(bytes, essence, html,
                         encoding, url) -> a Document or null ("set a document response"). In a
                         Worker no body is a Document, responseType "document" is ignored and there
                         is no responseXML ([Exposed=Window]).
     realm.synchronous   whether the realm's event loop can pause for a synchronous request: a
                         Worker's can (browserjsworker.c waits for the parent's answer), a Window's
                         when its host can serve the network alone (js.c native_fetch_wait over
                         web_host.wait_request); otherwise open() with async false is a
                         NotSupportedError.
   The bootstraps have the rest by the same names, as js_blob.js uses them: Event, EventTarget,
   DOMException, Headers, ProgressEvent, eventState, dispatch, installHandlers, report, blobBridge
   and host.now, timer, clear, textDecoder and textDecode. xhrInterfaces returns the interfaces and
   handlerTarget, the realm's xhrHandlerTarget. An asynchronous request's processResponse, body
   chunks and end of body are tasks; no page-replaceable fetch or Promise method schedules them.
   Document responses use the native HTML and XML parsers. */
function progressEventInterface(Event) {
    const slots=new WeakMap(),apply=Reflect.apply,get=WeakMap.prototype.get,set=WeakMap.prototype.set;
    const trunc=Math.trunc,finite=Number.isFinite;
    /* Web IDL unsigned long long: the integer part of ToNumber, modulo 2^64; NaN, 0 and the
       infinities are +0. */
    const count=v=>{v=+v;if(!finite(v)||v===0)return 0;v=trunc(v)%18446744073709551616;return v<0?v+18446744073709551616:v;};
    const state=e=>{const p=apply(get,slots,[e]);if(!p)throw new TypeError('Illegal ProgressEvent receiver');return p;};
    class ProgressEvent extends Event {
        constructor(type,init={}) {
            if(!arguments.length)throw new TypeError('ProgressEvent requires a type');
            init=init??{};super(type,init);
            apply(set,slots,[this,{lengthComputable:!!init.lengthComputable,loaded:count(init.loaded??0),total:count(init.total??0)}]);
        }
        get lengthComputable(){return state(this).lengthComputable;}
        get loaded(){return state(this).loaded;}
        get total(){return state(this).total;}
    }
    return ProgressEvent;
}
function xhrInterfaces(realm) {
    const fetch=realm.fetch,documents=realm.document||null,windowRealm=documents!==null;
    const states = new WeakMap(), uploadStates = new WeakMap();
    const textDecoder = host.textDecoder, textDecode = host.textDecode;
    const nativeThen = Promise.prototype.then, nativeApply = Reflect.apply;
    const defineProperty=Object.defineProperty, promiseConstructor=Object.freeze({[Symbol.species]:Promise});
    const create=Object.create, assign=Object.assign, NativeNumber=Number;
    const bufferLength=Object.getOwnPropertyDescriptor(ArrayBuffer.prototype,'byteLength').get;
    const U8=Uint8Array,typedPrototype=Object.getPrototypeOf(U8.prototype);
    const typedBuffer=Object.getOwnPropertyDescriptor(typedPrototype,'buffer').get;
    const typedLength=Object.getOwnPropertyDescriptor(typedPrototype,'byteLength').get;
    const typedSet=U8.prototype.set;
    const split=String.prototype.split;
    const weakGet=WeakMap.prototype.get, weakSet=WeakMap.prototype.set, weakHas=WeakMap.prototype.has;
    for(const map of [states,uploadStates]) {
        defineProperty(map,'get',{value:key=>nativeApply(weakGet,map,[key])});
        defineProperty(map,'set',{value:(key,value)=>nativeApply(weakSet,map,[key,value])});
        defineProperty(map,'has',{value:key=>nativeApply(weakHas,map,[key])});
    }
    const parseJSON = JSON.parse;
    const headerEntries = Headers.prototype.entries, headerGet = Headers.prototype.get;
    // A job for the emergency path, through this file's own resolved promise.
    const resolved=Promise.resolve();defineProperty(resolved,'constructor',{value:promiseConstructor});
    const microtask=fn=>nativeApply(nativeThen,resolved,[fn]);
    const eventTypes = new Set(['loadstart','progress','abort','error','load','timeout','loadend']);
    const string = v => { if(typeof v==='symbol')throw new TypeError('Cannot convert Symbol to string');return String(v); };
    const bytestring = v => { const t=string(v);if(/[^\x00-\xff]/.test(t))throw new TypeError('Expected ByteString');return t; };
    const uint = v => { const n=+v;return Number.isFinite(n)?Math.trunc(n)>>>0:0; };
    const fail = (name,message) => { throw new DOMException(message,name); };
    const state = x => { const s=states.get(x);if(!s)throw new TypeError('Illegal XMLHttpRequest receiver');return s; };
    const current = (s,epoch) => s.epoch===epoch && s.sent;
    const forbidden = /^(accept-charset|accept-encoding|access-control-request-headers|access-control-request-method|connection|content-length|cookie2?|date|dnt|expect|host|keep-alive|origin|permissions-policy|referer|te|trailer|transfer-encoding|upgrade|via)$|^(proxy-|sec-)/i;
    const token = /^[!#$%&'*+.^_`|~0-9A-Za-z-]+$/;
    function utf8ContentType(value) {
        const semi=value.indexOf(';'),essence=(semi<0?value:value.slice(0,semi)).trim(),parts=essence.split('/');
        if(parts.length!==2 || !token.test(parts[0]) || !token.test(parts[1]) || semi<0)return value;
        // Semicolons inside quoted parameter values are data, not delimiters.
        let out=value.slice(0,semi),start=semi+1,quoted=false,escaped=false;
        for(let i=start;i<=value.length;i++) {
            const c=value[i];
            if(i<value.length){
                if(escaped){escaped=false;continue;}
                if(quoted && c==='\\'){escaped=true;continue;}
                if(c==='"'){quoted=!quoted;continue;}
                if(quoted || c!==';')continue;
            }
            let field=value.slice(start,i);
            if(/^\s*charset\s*=\s*(?:"(?:[^"\\]|\\.)*"|[^\s;"]+)\s*$/i.test(field))field=field.replace(/=.*/s,'=UTF-8');
            out+=';'+field;start=i+1;
        }
        return out;
    }
    /* Fetch "extract a length" of a header list's Content-Length: its one decimal value, or 0 when
       it has none, differing values or one that is not digits (send()'s "not an integer"). */
    function extractLength(headers) {
        const header=nativeApply(headerGet,headers,['content-length']);
        if(header===null)return 0;
        const values=nativeApply(split,header,[',']);let candidate=null;
        for(let i=0;i<values.length;i++){
            let value=values[i],a=0,b=value.length;
            while(a<b&&(value[a]===' '||value[a]==='\t'))a++;
            while(b>a&&(value[b-1]===' '||value[b-1]==='\t'))b--;
            value=value.slice(a,b);
            if(candidate===null)candidate=value;else if(value!==candidate)return 0;
        }
        return candidate!==null&&/^\d+$/.test(candidate)?NativeNumber(candidate):0;
    }
    // XHR "get a response MIME type" and "get a final MIME type".
    // The MIME types are Fetch's and MIME Sniffing's (js_fetch.js's mimeType and parseMIME).
    const responseMime=s=>fetch.mimeType(s.responseHeaders)??{essence:'text/xml',parameters:new Map()};
    const finalMime=s=>s.override??responseMime(s);
    const xmlMime=m=>m.essence==='text/xml' || m.essence==='application/xml' || m.essence.endsWith('+xml');
    // XHR "get a final encoding": a label; the native parsers resolve it and
    // treat an unknown one as none.
    function finalEncoding(s) {
        let label=responseMime(s).parameters.get('charset')??null;
        if(s.override && s.override.parameters.has('charset'))label=s.override.parameters.get('charset');
        return label;
    }
    // Whether the received bytes must be kept for a document response (a Window's only).
    function keepsBytes(s) {
        if(!documents)return false;
        if(s.type==='document')return true;
        return s.type==='' && xmlMime(finalMime(s));
    }
    // XHR "set a document response".
    function documentResponse(s) {
        const bytes=s.bytes;s.bytes=null;
        if(!bytes || !documents)return null;
        const mime=finalMime(s),html=mime.essence==='text/html';
        if(!html && !xmlMime(mime))return null;
        if(s.type==='' && html)return null;
        // The XML parser runs with XML scripting support disabled; failure is null.
        return documents.response(bytes,mime.essence,html,finalEncoding(s),s.finalURL);
    }

    class XMLHttpRequestEventTarget extends EventTarget {
        constructor(){super();if(new.target===XMLHttpRequestEventTarget)throw new TypeError('Illegal constructor');}
    }
    const uploadToken={};
    class XMLHttpRequestUpload extends XMLHttpRequestEventTarget {
        constructor(t){super();if(t!==uploadToken)throw new TypeError('Illegal constructor');}
    }
    function fire(x,type,loaded=0,total=0,computable=false) {
        const e=type==='readystatechange'?new Event(type):new ProgressEvent(type,{loaded,total,lengthComputable:computable});
        eventState(e).isTrusted=true;dispatch(x,e);
    }
    function stopTimer(s){if(s.timer)host.clear(s.timer);s.timer=0;}
    function clearResponse(s){s.status=0;s.statusText='';s.responseURL='';s.responseHeaders=null;s.text='';s.bytes=null;s.object=null;s.objectSet=false;s.chunks=[];s.loaded=0;s.total=0;s.decoder=null;s.progressAt=0;s.uploadProgressAt=0;s.finalURL='';s.keep=false;}
    function cancel(s){if(s.transport){const transport=s.transport;s.transport=null;transport.cancel();}stopTimer(s);}
    function error(x,s,epoch,type) {
        if(!current(s,epoch))return;
        cancel(s);s.sent=false;clearResponse(s);s.ready=4;
        fire(x,'readystatechange');if(s.epoch!==epoch)return;
        if(!s.uploadDone){s.uploadDone=true;if(s.uploadListener){fire(s.upload,type);if(s.epoch!==epoch)return;fire(s.upload,'loadend');if(s.epoch!==epoch)return;}}
        fire(x,type);if(s.epoch!==epoch)return;fire(x,'loadend');
    }
    // Pages see only a bare network error; the console names the transport
    // reason, as other browsers report failed requests there.
    function logFailure(s,reason) {
        let text;try{text=reason instanceof Error?reason.name+': '+reason.message:string(reason);}catch(_){text='unknown reason';}
        // Like resource diagnostics, never copy query/fragment tokens.
        const cut=s.url.search(/[?#]/),shown=cut<0?s.url:s.url.slice(0,cut)+' [URL suffix omitted]';
        try{realm.log('XMLHttpRequest '+s.method+' '+shown+' failed: '+text);}catch(_){}
    }
    function networkFailure(x,s,epoch,reason) {
        if(!current(s,epoch))return;
        logFailure(s,reason);
        error(x,s,epoch,'error');
    }
    function armTimer(x,s,epoch) {
        stopTimer(s);
        if(s.timeout && current(s,epoch))s.timer=host.timer(0,()=>error(x,s,epoch,'timeout'),Math.max(0,s.started+s.timeout-host.now()),[]);
    }
    function emergencyError(x,s,epoch) {
        cancel(s);
        try{microtask(()=>error(x,s,epoch,'error'));}
        catch(_){
            // A simultaneous job-allocation failure cannot deliver events.
            // Still release the request and leave a terminal, reusable object.
            if(current(s,epoch)){s.sent=false;clearResponse(s);s.ready=4;s.uploadDone=true;s.epoch++;}
        }
    }
    function queue(x,s,epoch,fn) {
        if(!current(s,epoch))return;
        // abort()/open() can replace the response after this task is queued.
        // Revalidate at delivery before an old frame touches the new request.
        try{host.timer(0,()=>{if(current(s,epoch))fn();},0,[]);}
        catch(_){
            // Resource exhaustion must not leave an in-flight request hung.
            // An emergency native job reports failure, never a fake success.
            emergencyError(x,s,epoch);
        }
    }
    // The text response's decoder: decode, a BOM over the charset's encoding
    // (UTF-8 when the label names none, replacement too); JSON's UTF-8 decode.
    function responseDecoder(s,label) {
        return s.type==='json'?textDecoder('UTF-8',false,false):textDecoder(label,false,true,true);
    }
    const decodeBytes=(decoder,bytes,stream)=>textDecode(decoder,bytes,0,bytes?nativeApply(bufferLength,bytes,[]):0,stream);
    const charset=mime=>{const m=/;\s*charset\s*=\s*(?:"([^"]*)"|([^;\s]*))/i.exec(mime);return m?(m[1]??m[2]):null;};
    const textLabel=s=>charset(s.mime)??charset(s.responseHeaders?nativeApply(headerGet,s.responseHeaders,['content-type'])||'':'')??'UTF-8';
    function textResponse(s) {
        return decodeBytes(responseDecoder(s,textLabel(s)),s.bytes,false);
    }
    /* A response the realm's Fetch brought whole (no observer frames): the end of body at once. */
    function completed(x,s,epoch,response) {
        if(!current(s,epoch))return;
        s.transport=null;
        if(!s.uploadDone) {
            s.uploadDone=true;
            if(s.uploadListener)for(const type of ['progress','load','loadend']){fire(s.upload,type,s.uploadSize,s.uploadSize,s.uploadSize>0);if(!current(s,epoch))return;}
        }
        const result=fetch.xhrResponse(response);
        s.status=result.status;s.statusText=result.statusText;s.responseURL=nativeApply(split,result.url,['#'])[0];s.responseHeaders=result.headers;
        s.finalURL=result.url;
        s.ready=2;fire(x,'readystatechange');if(!current(s,epoch))return;
        // Transfer the private Response's sole buffer, without an extra
        // fragment-sized copy or page-defined getters/@@species.
        s.bytes=result.bytes;
        const text=s.type==='arraybuffer'||s.type==='blob'||s.type==='document'?'':textResponse(s);
        if(!current(s,epoch))return;
        s.text=text;
        const loaded=nativeApply(bufferLength,s.bytes,[]);
        if(loaded){
            s.ready=3;
            // Text/JSON have their decoded representation now. Keep the wire
            // buffer only for binary response types; do not double-retain a
            // large response for the lifetime of Google's cached XHR objects.
            if((s.type===''&&!keepsBytes(s))||s.type==='text'||s.type==='json')s.bytes=null;
            fire(x,'readystatechange');if(!current(s,epoch))return;
        }
        const total=extractLength(s.responseHeaders);
        fire(x,'progress',loaded,total,total>0);if(!current(s,epoch))return;
        stopTimer(s);s.sent=false;s.ready=4;
        fire(x,'readystatechange');if(s.epoch!==epoch)return;
        fire(x,'load',loaded,total,total>0);if(s.epoch!==epoch)return;
        fire(x,'loadend',loaded,total,total>0);
    }
    function receivedHeaders(x,s,epoch,response) {
        const result=fetch.xhrMetadata(response);
        s.status=result.status;s.statusText=result.statusText;s.responseURL=nativeApply(split,result.url,['#'])[0];s.responseHeaders=result.headers;
        s.finalURL=result.url;s.keep=keepsBytes(s);
        s.total=extractLength(s.responseHeaders);
        s.ready=2;fire(x,'readystatechange');
    }
    function prepareDecoder(s) {
        if(!s.decoder&&s.type!=='arraybuffer'&&s.type!=='blob'&&s.type!=='document')s.decoder=responseDecoder(s,textLabel(s));
    }
    function receivedChunk(x,s,epoch,bytes) {
        prepareDecoder(s);
        const count=nativeApply(bufferLength,bytes,[]);s.loaded+=count;
        if(s.decoder)s.text+=decodeBytes(s.decoder,bytes,true);
        if(!s.decoder||s.keep)s.chunks.push(bytes);
        s.ready=3;
        const now=host.now();
        if(now-s.progressAt<50)return;
        s.progressAt=now;fire(x,'readystatechange');if(!current(s,epoch))return;
        fire(x,'progress',s.loaded,s.total,s.total>0);
    }
    function receivedEnd(x,s,epoch,reason) {
        if(reason!==undefined){networkFailure(x,s,epoch,reason);return;}
        prepareDecoder(s);
        if(s.decoder)s.text+=decodeBytes(s.decoder,null,false);
        if(!s.decoder||s.keep) {
            const bytes=new U8(s.loaded);let at=0;
            for(const chunk of s.chunks){const input=new U8(chunk);nativeApply(typedSet,bytes,[input,at]);at+=nativeApply(typedLength,input,[]);}
            s.bytes=nativeApply(typedBuffer,bytes,[]);
        }
        s.chunks=[];s.decoder=null;s.transport=null;
        fire(x,'progress',s.loaded,s.total,s.total>0);if(!current(s,epoch))return;
        stopTimer(s);s.sent=false;s.ready=4;
        fire(x,'readystatechange');if(s.epoch!==epoch)return;
        fire(x,'load',s.loaded,s.total,s.total>0);if(s.epoch!==epoch)return;
        fire(x,'loadend',s.loaded,s.total,s.total>0);
    }
    function uploaded(s,epoch,count) {
        if(s.uploadDone)return;
        const done=count===s.uploadSize,now=host.now();
        if(done)s.uploadDone=true;
        if(s.uploadListener&&(done||now-s.uploadProgressAt>=50)){
            s.uploadProgressAt=now;fire(s.upload,'progress',count,s.uploadSize,s.uploadSize>0);
            if(s.epoch!==epoch)return;
            if(done){fire(s.upload,'load',count,s.uploadSize,s.uploadSize>0);if(s.epoch!==epoch)return;fire(s.upload,'loadend',count,s.uploadSize,s.uploadSize>0);}
        }
    }
    /* send()'s synchronous steps: no loadstart, progress or upload events. The realm's Fetch pauses
       its event loop until the response's body is whole or timeout milliseconds pass; then "handle
       response end-of-body". A network error or the timeout runs the request error steps, which
       throw for a synchronous request. */
    function synchronous(x,s,epoch,init) {
        let response=null,failure=null;
        try{response=fetch.xhrFetchSync(s.url,init,s.uploadListener,s.timeout);}catch(e){failure=e;}
        if(response===null){
            if(failure!==null)logFailure(s,failure);
            s.sent=false;clearResponse(s);s.ready=4;s.uploadDone=true;
            if(failure===null)fail('TimeoutError','The synchronous request timed out');
            fail('NetworkError','A network error occurred');
        }
        const result=fetch.xhrResponse(response);
        s.status=result.status;s.statusText=result.statusText;s.responseURL=nativeApply(split,result.url,['#'])[0];s.responseHeaders=result.headers;
        s.finalURL=result.url;s.bytes=result.bytes;
        const loaded=nativeApply(bufferLength,s.bytes,[]),total=extractLength(s.responseHeaders);
        if(s.type===''||s.type==='text'||s.type==='json'){s.text=textResponse(s);if(!keepsBytes(s))s.bytes=null;}
        s.uploadDone=true;s.sent=false;s.ready=4;
        fire(x,'readystatechange');if(s.epoch!==epoch)return;
        fire(x,'load',loaded,total,total>0);if(s.epoch!==epoch)return;
        fire(x,'loadend',loaded,total,total>0);
    }
    class XMLHttpRequest extends XMLHttpRequestEventTarget {
        constructor() {
            super();const upload=new XMLHttpRequestUpload(uploadToken);
            // The private state later owns the transport ArrayBuffer; inherited
            // setters must not get this record while clearResponse initializes it.
            const s=assign(create(null),{ready:0,sent:false,sync:false,epoch:0,timer:0,started:0,timeout:0,credentials:false,type:'',mime:'',override:null,
                method:'',url:'',headers:new Map(),transport:null,upload,uploadDone:true,uploadSize:0,uploadListener:false});
            clearResponse(s);states.set(this,s);uploadStates.set(upload,s);
        }
        open(method,url,async,username=null,password=null) {
            const s=state(this);
            if(arguments.length<2)throw new TypeError('open requires method and URL');
            method=bytestring(method);url=string(url);async=arguments.length<3?true:!!async;
            if(username!==null)username=string(username);if(password!==null)password=string(password);
            if(!token.test(method))fail('SyntaxError','Invalid HTTP method');
            const upper=method.toUpperCase();
            if(['CONNECT','TRACE','TRACK'].includes(upper))fail('SecurityError','Forbidden HTTP method');
            if(['DELETE','GET','HEAD','OPTIONS','POST','PUT'].includes(upper))method=upper;
            // encoding-parse relative to the relevant settings object.
            const parsed=realm.parseURL(url);
            if(parsed===null)fail('SyntaxError','Invalid request URL');
            if(!async && windowRealm && (s.timeout!==0 || s.type!==''))fail('InvalidAccessError','A synchronous request of a Window has no timeout or response type');
            if(!async && !realm.synchronous)fail('NotSupportedError','This realm cannot pause for a synchronous XMLHttpRequest');
            if(username!==null || password!==null || parsed.username || parsed.password)fail('NotSupportedError','URL credentials are not supported');
            cancel(s);s.epoch++;s.sent=false;s.sync=!async;s.method=method;s.url=parsed.href;s.headers=new Map();clearResponse(s);
            s.uploadDone=true;s.uploadSize=0;s.uploadListener=false;
            const change=s.ready!==1;s.ready=1;if(change)fire(this,'readystatechange');
        }
        setRequestHeader(name,value) {
            const s=state(this);if(arguments.length<2)throw new TypeError('setRequestHeader requires name and value');
            name=bytestring(name);value=bytestring(value).replace(/^[\t\r\n ]+|[\t\r\n ]+$/g,'');
            if(s.ready!==1 || s.sent)fail('InvalidStateError','The request is not open');
            if(!token.test(name) || /[\0\r\n]/.test(value))fail('SyntaxError','Invalid HTTP header');
            name=name.toLowerCase();if(forbidden.test(name))return;
            if(['x-http-method','x-http-method-override','x-method-override'].includes(name) && /(?:^|,)\s*(?:CONNECT|TRACE|TRACK)\s*(?:,|$)/i.test(value))return;
            s.headers.set(name,s.headers.has(name)?s.headers.get(name)+', '+value:value);
        }
        send(body=null) {
            const s=state(this);if(s.ready!==1 || s.sent)fail('InvalidStateError','The request is not open');
            if(s.method==='GET' || s.method==='HEAD')body=null;
            // A Document body (a Window's) is its serialization, UTF-8 encoded.
            let documentType=null;
            if(body!==null && documents && documents.is(body)){const serialized=documents.body(body);documentType=serialized[0];body=serialized[1];}
            const extracted=fetch.xhrBody(body),contentType=documentType??extracted.type;
            body=extracted.bytes;
            // Body conversion may have called open()/send()/abort() itself.
            if(s.ready!==1 || s.sent)fail('InvalidStateError','Request state changed during body conversion');
            if(s.method==='GET' || s.method==='HEAD')body=null;
            if(body!==null && contentType!==null && !s.headers.has('content-type'))s.headers.set('content-type',contentType);
            else if(body!==null && !extracted.binary && s.headers.has('content-type'))s.headers.set('content-type',utf8ContentType(s.headers.get('content-type')));
            const uploadSize=body===null?0:nativeApply(bufferLength,body,[]);
            s.uploadListener=!!realm.listened(s.upload);
            const init=assign(create(null),{method:s.method,headers:Object.fromEntries(s.headers),body,credentials:s.credentials?'include':'same-origin'});
            const epoch=++s.epoch;s.sent=true;s.started=host.now();s.uploadDone=body===null;s.uploadSize=uploadSize;
            if(s.sync)return synchronous(this,s,epoch,init);
            fire(this,'loadstart');if(!current(s,epoch))return;
            if(!s.uploadDone && s.uploadListener){fire(s.upload,'loadstart',0,s.uploadSize,s.uploadSize>0);if(!current(s,epoch))return;}
            try{armTimer(this,s,epoch);}catch(e){cancel(s);s.sent=false;s.epoch++;throw e;}
            let started;
            try{
                started=fetch.xhrFetch(s.url,init,(event,value,resume)=>{
                    queue(this,s,epoch,()=>{
                        try{
                            if(event==='headers')receivedHeaders(this,s,epoch,value);
                            else if(event==='chunk')receivedChunk(this,s,epoch,value);
                            else if(event==='upload')uploaded(s,epoch,value);
                            else if(event==='end')receivedEnd(this,s,epoch,value);
                        }catch(e){error(this,s,epoch,'error');report(e);}
                        finally{if(current(s,epoch))resume();}
                    });
                },s.uploadListener);
            }catch(e){queue(this,s,epoch,()=>networkFailure(this,s,epoch,e));return;}
            s.transport=started;
            // Native promises settle at the host checkpoint. XHR notifications
            // are tasks; never run them recursively in send() or a page promise.
            // Even intrinsic then reads constructor@@species. This promise is
            // host-private, so shield it from a mutated Promise.prototype.
            try{
                defineProperty(started.promise,'constructor',{value:promiseConstructor});
                nativeApply(nativeThen,started.promise,[response=>{
                    // A request whose frames reach the observer ends there.
                    if(!started.observed)queue(this,s,epoch,()=>{try{completed(this,s,epoch,response);}catch(e){error(this,s,epoch,'error');report(e);}});
                },reason=>queue(this,s,epoch,()=>networkFailure(this,s,epoch,reason))]);
            }catch(e){cancel(s);s.sent=false;s.epoch++;throw e;}
        }
        abort() {
            const s=state(this),epoch=s.epoch;
            if(s.sent){error(this,s,epoch,'abort');if(s.epoch!==epoch)return;}
            else cancel(s);
            s.epoch++;
            if(s.ready===4){s.ready=0;clearResponse(s);}
        }
        get readyState(){return state(this).ready;}
        get status(){return state(this).status;}
        get statusText(){return state(this).statusText;}
        get responseURL(){return state(this).responseURL;}
        get upload(){return state(this).upload;}
        get timeout(){return state(this).timeout;}
        set timeout(v){
            const s=state(this);v=uint(v);
            if(windowRealm && s.sync)fail('InvalidAccessError','A synchronous request of a Window has no timeout');
            s.timeout=v;
            if(s.sent && !s.sync){const epoch=s.epoch;try{armTimer(this,s,epoch);}catch(e){emergencyError(this,s,epoch);throw e;}}
        }
        get withCredentials(){return state(this).credentials;}
        set withCredentials(v){const s=state(this);if(s.ready>1 || s.sent)fail('InvalidStateError','Request already sent');s.credentials=!!v;}
        get responseType(){return state(this).type;}
        set responseType(v){
            const s=state(this);v=string(v);
            if(!['','text','json','arraybuffer','blob','document'].includes(v))return;
            // A document response is a Window's.
            if(!windowRealm && v==='document')return;
            if(s.ready===3 || s.ready===4)fail('InvalidStateError','Response already loading');
            if(windowRealm && s.sync)fail('InvalidAccessError','A synchronous request of a Window has no response type');
            s.type=v;
        }
        get responseText(){const s=state(this);if(s.type!=='' && s.type!=='text')fail('InvalidStateError','Response is not text');return s.ready<3?'':s.text;}
        get responseXML(){
            const s=state(this);if(s.type!=='' && s.type!=='document')fail('InvalidStateError','Response is not a document');
            if(s.ready!==4)return null;
            if(!s.objectSet){s.object=documentResponse(s);s.objectSet=true;}
            return s.object;
        }
        get response(){
            const s=state(this);if(s.type==='' || s.type==='text')return s.ready<3?'':s.text;
            if(s.ready!==4)return null;
            if((s.type==='arraybuffer'||s.type==='blob')&&!s.bytes)return null;
            if(!s.objectSet){
                if(s.type==='arraybuffer')s.object=s.bytes;
                else if(s.type==='blob')s.object=blobBridge.fromBytes(s.bytes,s.mime||nativeApply(headerGet,s.responseHeaders,['content-type'])||'');
                else if(s.type==='json'){try{s.object=parseJSON(s.text);}catch(_){s.object=null;}s.text='';}
                else if(s.type==='document')s.object=documentResponse(s);
                s.objectSet=true;
            }
            return s.object;
        }
        overrideMimeType(mime){const s=state(this);if(!arguments.length)throw new TypeError('overrideMimeType requires a MIME type');mime=string(mime);if(s.ready===3 || s.ready===4)fail('InvalidStateError','Response already loading');s.mime=mime;
            s.override=fetch.parseMIME(mime)??{essence:'application/octet-stream',parameters:new Map()};}
        getResponseHeader(name){const s=state(this);if(!arguments.length)throw new TypeError('getResponseHeader requires name');name=bytestring(name);if(!token.test(name) || /^set-cookie2?$/i.test(name) || !s.responseHeaders)return null;return nativeApply(headerGet,s.responseHeaders,[name]);}
        getAllResponseHeaders(){
            const s=state(this);if(!s.responseHeaders)return '';
            const headers=[];for(const [key,value]of nativeApply(headerEntries,s.responseHeaders,[]))if(!/^set-cookie2?$/i.test(key))headers.push([key.toLowerCase(),value]);
            headers.sort((a,b)=>a[0]<b[0]?-1:a[0]>b[0]?1:0);return headers.map(([k,v])=>k+': '+v+'\r\n').join('');
        }
    }
    // responseXML is [Exposed=Window].
    if(!documents)delete XMLHttpRequest.prototype.responseXML;
    installHandlers(XMLHttpRequestEventTarget.prototype,eventTypes);
    installHandlers(XMLHttpRequest.prototype,['readystatechange']);
    for(const [key,value]of Object.entries({UNSENT:0,OPENED:1,HEADERS_RECEIVED:2,LOADING:3,DONE:4}))
        for(const obj of [XMLHttpRequest,XMLHttpRequest.prototype])Object.defineProperty(obj,key,{value,enumerable:true});
    return {XMLHttpRequest,XMLHttpRequestUpload,XMLHttpRequestEventTarget,
        handlerTarget:(target,type)=>(states.has(target)&&(eventTypes.has(type)||type==='readystatechange')) || (uploadStates.has(target)&&eventTypes.has(type))};
}
