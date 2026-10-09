/* Only this private host crosses the native pipe. No window, document, native
 * modules, files, sockets or process functions are installed in this realm. */
(function(host){
    delete globalThis.__workerHost;
    delete globalThis.SharedArrayBuffer;delete globalThis.Atomics;
    class DOMException extends Error{constructor(message='',name='Error'){super(String(message));this.name=String(name);}}
    class Event{constructor(type,init={}){this.type=String(type);this.cancelable=!!init.cancelable;this.defaultPrevented=false;this.target=null;this.currentTarget=null;}preventDefault(){if(this.cancelable)this.defaultPrevented=true;}}
    class MessageEvent extends Event{constructor(type,init={}){super(type,init);this.data=init.data;this.origin=init.origin||'';this.lastEventId='';this.source=null;this.ports=[];}}
    class ErrorEvent extends Event{constructor(type,init={}){super(type,init);Object.assign(this,{message:init.message||'',filename:init.filename||'',lineno:init.lineno||0,colno:init.colno||0,error:init.error});}}
    const listeners=new WeakMap();
    class EventTarget{constructor(){listeners.set(this,new Map());}addEventListener(type,fn,options={}){if(fn==null)return;const m=listeners.get(this);if(!m)throw new TypeError('Illegal invocation');type=String(type);const list=m.get(type)||[];if(!list.some(x=>x.fn===fn))list.push({fn,once:!!options?.once});m.set(type,list);}removeEventListener(type,fn){const m=listeners.get(this);if(!m)throw new TypeError('Illegal invocation');m.set(String(type),(m.get(String(type))||[]).filter(x=>x.fn!==fn));}dispatchEvent(event){const m=listeners.get(this);if(!m||!(event instanceof Event))throw new TypeError('Invalid EventTarget/Event');const receiver=this===target?globalThis:this;event.target=event.currentTarget=receiver;for(const x of [...(m.get(event.type)||[])]){if(x.once)this.removeEventListener(event.type,x.fn);try{if(typeof x.fn==='function')x.fn.call(receiver,event);else x.fn.handleEvent(event);}catch(e){report(e);}}const fn=this['on'+event.type];if(typeof fn==='function'){try{fn.call(receiver,event);}catch(e){report(e);}}return !event.defaultPrevented;}}
    Object.assign(globalThis,{DOMException,Event,MessageEvent,ErrorEvent,EventTarget});
    /* @include js_encoding.js */
    /* @include js_url.js */
    /* @include js_clone.js */
    const target=new EventTarget(),timers=new Map(),fetches=new Map();let nextTimer=0,closed=false,address='',workerName='',reporting=false;
    const errorPayload=e=>({message:String(e?.message||e),filename:address,lineno:0,stack:String(e?.stack||'')});
    function report(e){const info=errorPayload(e);if(reporting){host.send(4,0,info);return;}reporting=true;try{const event=new ErrorEvent('error',{...info,error:e,cancelable:true});if(target.dispatchEvent(event))host.send(4,0,info);}finally{reporting=false;}}
    function evalSource(text,url){return host.eval(String(text),String(url));}
    function postMessage(value,options){if(closed)return;const list=Array.isArray(options)?options:options?.transfer;const packet=cloneData.prepare(value,cloneData.transferList(list),true);host.send(2,0,packet.data);cloneData.commit(packet);}
    function close(){if(closed)return;closed=true;timers.clear();host.close();}
    function timer(fn,ms,args,repeat){if(closed)return 0;if(typeof fn!=='function')fn=new Function(String(fn));const id=++nextTimer,delay=Math.max(1,Math.min(2147483647,Number(ms)||0));if(timers.size>=1024)throw new RangeError('Worker timer quota');timers.set(id,{fn,args,repeat,delay,due:host.now()+delay});return id;}
    function importScripts(...urls){for(const u of urls){const url=new URL(String(u),address).href;const r=host.import(url);if(r[4]||r[0]<200||r[0]>=300)throw new DOMException(r[4]||'Worker script load failed','NetworkError');evalSource(new TextDecoder().decode(r[3]),r[1]||url);}}
    class Headers{constructor(input=[]){this._map=new Map();if(input instanceof Headers)input=[...input];else if(!Array.isArray(input)&&!(Symbol.iterator in Object(input)))input=Object.entries(input);for(const pair of input)this.append(pair[0],pair[1]);}append(k,v){k=String(k).trim().toLowerCase();v=String(v).trim();if(!/^[!#$%&'*+.^_`|~0-9a-z-]+$/.test(k)||/[\r\n]/.test(v))throw new TypeError('Invalid header');this._map.set(k,this._map.has(k)?this._map.get(k)+', '+v:v);}get(k){return this._map.get(String(k).toLowerCase())??null;}has(k){return this._map.has(String(k).toLowerCase());}set(k,v){this.delete(k);this.append(k,v);}delete(k){this._map.delete(String(k).toLowerCase());}entries(){return this._map.entries();}keys(){return this._map.keys();}values(){return this._map.values();}forEach(fn,thisArg){for(const [k,v]of this)fn.call(thisArg,v,k,this);}[Symbol.iterator](){return this.entries();}}
    class Response{constructor(body=null,init={}){this.status=init.status??200;this.statusText=init.statusText||'';this.headers=new Headers(init.headers);this.url=init.url||'';this.redirected=!!init.redirected;this.type='basic';this.bodyUsed=false;this._bytes=body instanceof ArrayBuffer?body.slice(0):body==null?new ArrayBuffer(0):new TextEncoder().encode(String(body)).buffer;}get ok(){return this.status>=200&&this.status<300;}async arrayBuffer(){if(this.bodyUsed)throw new TypeError('Body already consumed');this.bodyUsed=true;return this._bytes.slice(0);}async text(){return new TextDecoder().decode(await this.arrayBuffer());}async json(){return JSON.parse(await this.text());}clone(){if(this.bodyUsed)throw new TypeError('Body already consumed');return new Response(this._bytes,{status:this.status,statusText:this.statusText,headers:this.headers,url:this.url,redirected:this.redirected});}}
    function responseHeaders(raw,url){
        const entries=[];for(const line of String(raw).split(/\r?\n/)){const at=line.indexOf(':');if(at<=0)continue;const name=line.slice(0,at).trim().toLowerCase();if(/^set-cookie2?$|^x-nocturne-/.test(name))continue;entries.push([name,line.slice(at+1).trim()]);}
        const cors=new URL(url,address).origin!==new URL(address).origin;
        const exposed=new Set(['cache-control','content-language','content-length','content-type','expires','last-modified','pragma']);
        if(cors)for(const [name,value]of entries)if(name==='access-control-expose-headers')for(const field of value.split(','))exposed.add(field.trim().toLowerCase());
        const headers=new Headers();for(const [name,value]of entries)if(!cors||exposed.has(name)||exposed.has('*')){try{headers.append(name,value);}catch(_){}}
        return headers;
    }
    function fetch(input,options={}){
        if(closed)return Promise.reject(new TypeError('Worker closed'));options=options||{};
        if((options.method&&String(options.method).toUpperCase()!=='GET')||options.body!=null||options.headers!=null||options.signal!=null||(options.credentials&&options.credentials!=='same-origin')||(options.mode&&options.mode!=='cors')||(options.redirect&&options.redirect!=='follow')||(options.cache&&options.cache!=='default'))return Promise.reject(new DOMException('Only GET/CORS/same-origin-credentials fetch is supported in workers','NotSupportedError'));
        let url;try{url=new URL(String(input),address).href;}catch(e){return Promise.reject(e);}
        return new Promise((resolve,reject)=>{const id=host.request(url);fetches.set(id,{url,resolve,reject});});
    }
    Object.assign(globalThis,{self:globalThis,postMessage,close,importScripts,Headers,Response,fetch,
        setTimeout:(fn,ms,...args)=>timer(fn,ms,args,false),setInterval:(fn,ms,...args)=>timer(fn,ms,args,true),
        clearTimeout:id=>timers.delete(Number(id)),clearInterval:id=>timers.delete(Number(id)),
        queueMicrotask:fn=>{if(typeof fn!=='function')throw new TypeError('Callback required');Promise.resolve().then(fn).catch(report);},
        performance:{now:()=>host.now()},console:Object.fromEntries(['log','warn','error','info','debug'].map(k=>[k,(...args)=>host.send(7,0,{message:args.map(String).join(' '),filename:address})])),
        addEventListener:target.addEventListener.bind(target),removeEventListener:target.removeEventListener.bind(target),dispatchEvent:target.dispatchEvent.bind(target)});
    Object.defineProperties(globalThis,{name:{get:()=>workerName},onmessage:{get:()=>target.onmessage,set:fn=>target.onmessage=fn},onmessageerror:{get:()=>target.onmessageerror,set:fn=>target.onmessageerror=fn},onerror:{get:()=>target.onerror,set:fn=>target.onerror=fn}});
    return {
        start(r){address=r[1];workerName=String(r[5]||"");if(r[4]||r[0]<200||r[0]>=300)throw new DOMException(r[4]||'Worker source failed','NetworkError');Object.defineProperty(globalThis,'location',{value:Object.freeze({href:address,origin:new URL(address).origin,toString(){return address;}}),configurable:true});evalSource(new TextDecoder().decode(r[3]),address);},
        receive(packet){try{target.dispatchEvent(new MessageEvent('message',{data:cloneData.deserialize(packet)}));}catch(e){target.dispatchEvent(new MessageEvent('messageerror'));}},
        loaded(id,r){const p=fetches.get(id);if(!p)return;fetches.delete(id);if(r[4]||!r[0]){p.reject(new TypeError(r[4]||'Worker fetch failed'));return;}const headers=responseHeaders(r[2],r[1]);p.resolve(new Response(r[3],{status:r[0],headers,url:r[1],redirected:p.url!==r[1]}));},
        tick(now){for(const [id,t]of timers){if(t.due>now)continue;if(t.repeat)t.due=now+t.delay;else timers.delete(id);try{t.fn(...t.args);}catch(e){report(e);}return true;}return false;},
        deadline(){let due=-1;for(const t of timers.values())if(due<0||t.due<due)due=t.due;return due;},report
    };
})(__workerHost);
