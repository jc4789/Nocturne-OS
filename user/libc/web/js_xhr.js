/* Buffered asynchronous XHR over Nocturne's existing, CORS-checked transport.
   No page-replaceable fetch/Promise methods are used to schedule host work.
   Streaming, synchronous requests and XML documents are
   not implemented; unsupported operations must not pretend to succeed. */
{
    const states = new WeakMap(), uploadStates = new WeakMap(), progressStates = new WeakMap();
    const NativeURL = globalThis.URL, Decoder = globalThis.TextDecoder;
    const nativeThen = Promise.prototype.then, nativeApply = Reflect.apply;
    const defineProperty=Object.defineProperty, promiseConstructor=Object.freeze({[Symbol.species]:Promise});
    const parseJSON = JSON.parse, wellFormed=String.prototype.toWellFormed;
    const headerEntries = Headers.prototype.entries, headerGet = Headers.prototype.get;
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

    class ProgressEvent extends Event {
        constructor(type,init={}) {
            if(!arguments.length)throw new TypeError('ProgressEvent requires a type');
            init=init??{};super(type,init);
            const count=v=>{v=+v;if(!Number.isFinite(v))return 0;v=Math.trunc(v)%18446744073709551616;return v<0?v+18446744073709551616:v;};
            progressStates.set(this,{lengthComputable:!!init.lengthComputable,loaded:count(init.loaded??0),total:count(init.total??0)});
        }
        get lengthComputable(){const p=progressStates.get(this);if(!p)throw new TypeError('Illegal ProgressEvent receiver');return p.lengthComputable;}
        get loaded(){const p=progressStates.get(this);if(!p)throw new TypeError('Illegal ProgressEvent receiver');return p.loaded;}
        get total(){const p=progressStates.get(this);if(!p)throw new TypeError('Illegal ProgressEvent receiver');return p.total;}
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
        e.isTrusted=true;dispatch(x,e);
    }
    function stopTimer(s){if(s.timer)host.clear(s.timer);s.timer=0;}
    function clearResponse(s){s.status=0;s.statusText='';s.responseURL='';s.responseHeaders=null;s.text='';s.bytes=null;s.object=null;s.objectSet=false;}
    function cancel(s){if(s.id)host.cancel(s.id);s.id=0;stopTimer(s);}
    function error(x,s,epoch,type) {
        if(!current(s,epoch))return;
        cancel(s);s.sent=false;clearResponse(s);s.ready=4;
        fire(x,'readystatechange');if(s.epoch!==epoch)return;
        if(!s.uploadDone){s.uploadDone=true;if(s.uploadListener){fire(s.upload,type);if(s.epoch!==epoch)return;fire(s.upload,'loadend');if(s.epoch!==epoch)return;}}
        fire(x,type);if(s.epoch!==epoch)return;fire(x,'loadend');
    }
    function armTimer(x,s,epoch) {
        stopTimer(s);
        if(s.timeout && current(s,epoch))s.timer=host.timer(0,()=>error(x,s,epoch,'timeout'),Math.max(0,s.started+s.timeout-host.now()),[]);
    }
    function emergencyError(x,s,epoch) {
        cancel(s);
        try{host.microtask(()=>error(x,s,epoch,'error'));}
        catch(_){
            // A simultaneous job-allocation failure cannot deliver events.
            // Still release the request and leave a terminal, reusable object.
            if(current(s,epoch)){s.sent=false;clearResponse(s);s.ready=4;s.uploadDone=true;s.epoch++;}
        }
    }
    function queue(x,s,epoch,fn) {
        if(!current(s,epoch))return;
        try{host.timer(0,fn,0,[]);}
        catch(_){
            // Resource exhaustion must not leave an in-flight request hung.
            // An emergency native job reports failure, never a fake success.
            emergencyError(x,s,epoch);
        }
    }
    function textResponse(s) {
        const charset=mime=>{const m=/;\s*charset\s*=\s*(?:"([^"]*)"|([^;\s]*))/i.exec(mime);return m?(m[1]??m[2]):null;};
        const encoding=charset(s.mime)??charset(s.responseHeaders?nativeApply(headerGet,s.responseHeaders,['content-type'])||'':'')??'utf-8';
        let decoder;
        try{decoder=new Decoder(s.type==='json'?'utf-8':encoding);}catch(_){decoder=new Decoder('utf-8');}
        return decoder.decode(s.bytes);
    }
    function completed(x,s,epoch,response) {
        if(!current(s,epoch))return;
        s.id=0;
        if(!s.uploadDone) {
            s.uploadDone=true;
            if(s.uploadListener)for(const type of ['progress','load','loadend']){fire(s.upload,type,s.uploadSize,s.uploadSize,true);if(!current(s,epoch))return;}
        }
        s.status=response.status;s.statusText=response.statusText;s.responseURL=response.url.split('#')[0];s.responseHeaders=response.headers;
        s.ready=2;fire(x,'readystatechange');if(!current(s,epoch))return;
        // This Response is private to this request, never passed to page code.
        // Retain its native copy instead of invoking ArrayBuffer@@species.
        s.bytes=response._bytes;
        s.text=s.type==='arraybuffer'||s.type==='blob'?'':textResponse(s);
        if(s.bytes.byteLength){s.ready=3;fire(x,'readystatechange');if(!current(s,epoch))return;}
        const loaded=s.bytes.byteLength,header=nativeApply(headerGet,s.responseHeaders,['content-length']);
        const total=header!==null && /^\d+$/.test(header)?Number(header):0;
        const computable=header!==null && total===loaded;
        fire(x,'progress',loaded,computable?total:0,computable);if(!current(s,epoch))return;
        stopTimer(s);s.sent=false;s.ready=4;
        fire(x,'readystatechange');if(s.epoch!==epoch)return;
        fire(x,'load',loaded,computable?total:0,computable);if(s.epoch!==epoch)return;
        fire(x,'loadend',loaded,computable?total:0,computable);
    }
    class XMLHttpRequest extends XMLHttpRequestEventTarget {
        constructor() {
            super();const upload=new XMLHttpRequestUpload(uploadToken);
            const s={ready:0,sent:false,epoch:0,id:0,timer:0,started:0,timeout:0,credentials:false,type:'',mime:'',
                method:'',url:'',headers:new Map(),upload,uploadDone:true,uploadSize:0,uploadListener:false};
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
            let parsed;try{parsed=new NativeURL(url,document.baseURI);}catch(_){fail('SyntaxError','Invalid request URL');}
            if(!async)fail('NotSupportedError','Synchronous XMLHttpRequest is not supported');
            if(username!==null || password!==null || parsed.username || parsed.password)fail('NotSupportedError','URL credentials are not supported');
            cancel(s);s.epoch++;s.sent=false;s.method=method;s.url=parsed.href;s.headers=new Map();clearResponse(s);
            s.uploadDone=true;s.uploadSize=0;
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
            if(body!==null && body instanceof Document)fail('NotSupportedError','Document uploads are not supported');
            const extracted=fetchBridge.xhrBody(body),contentType=extracted.type;
            body=extracted.bytes;
            // Body conversion may have called open()/send()/abort() itself.
            if(s.ready!==1 || s.sent)fail('InvalidStateError','Request state changed during body conversion');
            if(s.method==='GET' || s.method==='HEAD')body=null;
            if(body!==null && contentType!==null && !s.headers.has('content-type'))s.headers.set('content-type',contentType);
            else if(body!==null && !extracted.binary && s.headers.has('content-type'))s.headers.set('content-type',utf8ContentType(s.headers.get('content-type')));
            let raw='';for(const [key,value]of s.headers)raw+=key+': '+value+'\r\n';
            const uploadSize=body===null?0:body.byteLength;
            const epoch=++s.epoch;s.sent=true;s.started=host.now();s.uploadDone=body===null;s.uploadSize=uploadSize;
            s.uploadListener=!!(listenerMap.get(s.upload)||[]).some(entry=>!entry.removed);
            fire(this,'loadstart');if(!current(s,epoch))return;
            if(!s.uploadDone && s.uploadListener){fire(s.upload,'loadstart');if(!current(s,epoch))return;}
            try{armTimer(this,s,epoch);}catch(e){cancel(s);s.sent=false;s.epoch++;throw e;}
            let pair;
            try{pair=s.url.startsWith('blob:')?{id:0,promise:fetchBridge.fetch(s.url,{method:s.method,headers:Object.fromEntries(s.headers),body})}:host.fetch(s.url,s.method,raw,body??'',false,s.credentials?2:1,s.uploadListener);}
            catch(_){queue(this,s,epoch,()=>error(this,s,epoch,'error'));return;}
            s.id=pair.id;
            // Native promises settle at the host checkpoint. XHR notifications
            // are tasks; never run them recursively in send() or a page promise.
            // Even intrinsic then reads constructor@@species. This promise is
            // host-private, so shield it from a mutated Promise.prototype.
            try{
                defineProperty(pair.promise,'constructor',{value:promiseConstructor});
                nativeApply(nativeThen,pair.promise,[response=>{
                    queue(this,s,epoch,()=>{try{completed(this,s,epoch,response);}catch(e){error(this,s,epoch,'error');report(e);}});
                },()=>queue(this,s,epoch,()=>error(this,s,epoch,'error'))]);
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
        set timeout(v){const s=state(this);s.timeout=uint(v);if(s.sent){const epoch=s.epoch;try{armTimer(this,s,epoch);}catch(e){emergencyError(this,s,epoch);throw e;}}}
        get withCredentials(){return state(this).credentials;}
        set withCredentials(v){const s=state(this);if(s.ready>1 || s.sent)fail('InvalidStateError','Request already sent');s.credentials=!!v;}
        get responseType(){return state(this).type;}
        set responseType(v){
            const s=state(this);v=string(v);
            if(!['','text','json','arraybuffer','blob','document'].includes(v))return;
            if(s.ready===3 || s.ready===4)fail('InvalidStateError','Response already loading');
            if(v==='document')fail('NotSupportedError','Document responses are not supported');
            s.type=v;
        }
        get responseText(){const s=state(this);if(s.type!=='' && s.type!=='text')fail('InvalidStateError','Response is not text');return s.ready<3?'':s.text;}
        get responseXML(){const s=state(this);if(s.type!=='' && s.type!=='document')fail('InvalidStateError','Response is not a document');return null;}
        get response(){
            const s=state(this);if(s.type==='' || s.type==='text')return s.ready<3?'':s.text;
            if(s.ready!==4 || !s.bytes)return null;
            if(!s.objectSet){
                if(s.type==='arraybuffer')s.object=s.bytes;
                else if(s.type==='blob')s.object=blobBridge.fromBytes(s.bytes,s.mime||nativeApply(headerGet,s.responseHeaders,['content-type'])||'');
                else if(s.type==='json'){try{s.object=parseJSON(s.text);}catch(_){s.object=null;}}
                s.objectSet=true;
            }
            return s.object;
        }
        overrideMimeType(mime){const s=state(this);if(!arguments.length)throw new TypeError('overrideMimeType requires a MIME type');mime=string(mime);if(s.ready===3 || s.ready===4)fail('InvalidStateError','Response already loading');s.mime=mime;}
        getResponseHeader(name){const s=state(this);if(!arguments.length)throw new TypeError('getResponseHeader requires name');name=bytestring(name);if(!token.test(name) || /^set-cookie2?$/i.test(name) || !s.responseHeaders)return null;return nativeApply(headerGet,s.responseHeaders,[name]);}
        getAllResponseHeaders(){
            const s=state(this);if(!s.responseHeaders)return '';
            const headers=[];for(const [key,value]of nativeApply(headerEntries,s.responseHeaders,[]))if(!/^set-cookie2?$/i.test(key))headers.push([key.toLowerCase(),value]);
            headers.sort((a,b)=>a[0]<b[0]?-1:a[0]>b[0]?1:0);return headers.map(([k,v])=>k+': '+v+'\r\n').join('');
        }
    }
    xhrHandlerTarget=(target,type)=>(states.has(target)&&(eventTypes.has(type)||type==='readystatechange')) || (uploadStates.has(target)&&eventTypes.has(type));
    installHandlers(XMLHttpRequestEventTarget.prototype,eventTypes);
    installHandlers(XMLHttpRequest.prototype,['readystatechange']);
    for(const [key,value]of Object.entries({UNSENT:0,OPENED:1,HEADERS_RECEIVED:2,LOADING:3,DONE:4}))
        for(const obj of [XMLHttpRequest,XMLHttpRequest.prototype])Object.defineProperty(obj,key,{value,enumerable:true});
    Object.assign(globalThis,{XMLHttpRequest,XMLHttpRequestUpload,XMLHttpRequestEventTarget,ProgressEvent});
}
