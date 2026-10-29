/* HTML "Server-sent events" (https://html.spec.whatwg.org/multipage/server-sent-events.html,
   consulted 2026-10-28): EventSource, one definition for a Window (js_bootstrap.js) and a Worker
   realm (js_worker_runtime.js), as the interface is [Exposed=(Window,Worker)].
   eventSourceInterfaces(realm) builds it over what differs between the realms:
     realm.fetch          the realm's Fetch (js_fetch.js's fetchInterfaces bridge). Every connection
                          is its eventSource(url, credentials, lastEventId, process): the request
                          ("create a potential-CORS request", Accept, cache mode "no-store",
                          initiator type "other", Last-Event-ID), and CORS and its preflight, the
                          credentials mode and cookies, Mixed Content, CSP's connect-src and
                          redirects are Fetch's and its native transport's, never relaxed here.
     realm.parseURL(url)  encoding-parse url relative to the relevant settings object (a Window's
                          document base URL and encoding, a Worker's API base URL and UTF-8): a
                          URL, or null for failure.
     realm.MessageEvent   the realm's MessageEvent.
     realm.fire(t, e)     dispatches e at t as a trusted event.
     realm.handlers(prototype, types)  installs the event handler IDL attributes.
     realm.report(error)  reports an exception.
     realm.now(), realm.timer(fn, ms) -> id, realm.clear(id)
                          its monotonic clock and its timers. A task of the remote event task
                          source is a timer of 0 ms: the realm runs them in the order they are
                          queued.
   The event stream is decoded by UTF-8 decode through the realm's native decoder (host.textDecoder
   and host.textDecode, which both realms have by these names, as js_xhr.js uses them).
   eventSourceInterfaces returns the interface and handlerTarget, the realm's
   eventSourceHandlerTarget.
   Choices the standard leaves to the user agent:
     - The reconnection time starts at 3000 ms; no backoff is added to it.
     - "Reestablish the connection, unless the user agent knows that to be futile": a network
       error before this source's first announced connection (a CORS or policy failure, a URL no
       fetch can answer, a host that does not answer) fails the connection; one after it (a
       dropped connection, a server that restarts) reestablishes it. An aborted network error
       always fails it.
     - A new event stream's last event ID buffer starts as the source's last event ID string, so
       an event after a reconnection that has no id field keeps the ID the server last set (as
       WPT's eventsource/format-field-id expects).
   Garbage collection: a source whose readyState is not CLOSED is kept (retained below), which
   includes every strong reference the standard requires; a CLOSED source has no fetch left to
   abort when it is collected. */
function eventSourceInterfaces(realm) {
    const {fetch,parseURL,MessageEvent,fire,handlers,report,now,timer,clear}=realm;
    const apply=Reflect.apply,define=Object.defineProperty,setPrototype=Object.setPrototypeOf;
    const NativeTypeError=TypeError,NativeDOMException=DOMException,NativeURL=URL,NativeEvent=Event,NativeString=String;
    const weakGet=WeakMap.prototype.get,weakSet=WeakMap.prototype.set,weakHas=WeakMap.prototype.has;
    const NativeSet=Set,setAdd=Set.prototype.add,setDelete=Set.prototype.delete;
    const urlHref=Object.getOwnPropertyDescriptor(NativeURL.prototype,'href').get,urlOrigin=Object.getOwnPropertyDescriptor(NativeURL.prototype,'origin').get;
    const exceptionName=Object.getOwnPropertyDescriptor(NativeDOMException.prototype,'name').get;
    const charCode=String.prototype.charCodeAt,strSlice=String.prototype.slice,strIndex=String.prototype.indexOf,wellFormed=String.prototype.toWellFormed;
    const regexExec=RegExp.prototype.exec,join=Array.prototype.join;
    const abLength=Object.getOwnPropertyDescriptor(ArrayBuffer.prototype,'byteLength').get;
    const nativeHost=host,textDecoder=host.textDecoder,textDecode=host.textDecode;
    const CONNECTING=0,OPEN=1,CLOSED=2,LONGEST_WAIT=2147483647,digits=/^[0-9]+$/;
    const slots=new WeakMap(),retained=new NativeSet();
    const code=(text,i)=>apply(charCode,text,[i]);
    const slice=(text,a,b)=>apply(strSlice,text,[a,b]);
    const indexOf=(text,search,from)=>apply(strIndex,text,[search,from]);
    const newList=()=>setPrototype([],null);
    const add=(list,value)=>{list[list.length]=value;};
    const state=value=>{const s=(typeof value==='object'&&value!==null)?apply(weakGet,slots,[value]):undefined;if(!s)throw new NativeTypeError('Illegal EventSource receiver');return s;};
    /* Web IDL USVString. */
    function usv(value){if(typeof value==='symbol')throw new NativeTypeError('Cannot convert a Symbol to a string');return apply(wellFormed,NativeString(value),[]);}
    /* Web IDL EventSourceInit: its boolean withCredentials member, false by default. */
    function withCredentials(value){
        if(value===undefined||value===null)return false;
        if(typeof value!=='object'&&typeof value!=='function')throw new NativeTypeError('EventSourceInit is not a dictionary');
        const member=value.withCredentials;return member===undefined?false:!!member;
    }
    /* An aborted network error: the fetch's controller was aborted (AbortError). */
    function aborted(reason){try{return apply(exceptionName,reason,[])==='AbortError';}catch(_){return false;}}
    function task(fn){timer(()=>{try{fn();}catch(error){report(error);}},0);}
    function retain(target,s){apply(s.ready===CLOSED?setDelete:setAdd,retained,[target]);}
    /* Stops this source's fetch and reconnection timer: what they deliver later is ignored. */
    function stop(s){
        const connection=s.connection;s.connection=null;s.attempt++;
        if(s.timer){clear(s.timer);s.timer=0;}
        if(connection)connection.cancel();
    }
    /* "Announce the connection". */
    function announce(target,s){
        task(()=>{if(s.ready===CLOSED)return;s.ready=OPEN;fire(target,new NativeEvent('open'));});
    }
    /* "Fail the connection": no attempt to reconnect follows. */
    function failConnection(target,s){
        task(()=>{if(s.ready===CLOSED)return;s.ready=CLOSED;retain(target,s);fire(target,new NativeEvent('error'));});
    }
    /* "Reestablish the connection": a task sets CONNECTING and fires error; once it has run, the
       reconnection time passes (the realm's timers wait at most LONGEST_WAIT at a time), then a
       task fetches the request again, with the last event ID. */
    function reestablish(target,s){
        task(()=>{
            if(s.ready===CLOSED)return;
            s.ready=CONNECTING;fire(target,new NativeEvent('error'));
            if(s.ready!==CONNECTING)return;
            const delay=s.reconnection,due=now()+delay,attempt=s.attempt;
            const wait=ms=>{s.timer=timer(elapsed,ms<LONGEST_WAIT?ms:LONGEST_WAIT);};
            const elapsed=()=>{
                if(s.attempt!==attempt)return;
                s.timer=0;const left=due-now();
                if(left>0){wait(left);return;}
                if(s.ready===CONNECTING)connect(s);
            };
            wait(delay);
        });
    }
    /* One event stream (a response's body): its decoder, its unfinished line and its buffers. */
    function eventStream(s,url){
        let origin;try{origin=apply(urlOrigin,new NativeURL(url),[]);}catch(_){origin='null';}
        return setPrototype({decoder:apply(textDecoder,nativeHost,['UTF-8',false,false]),pieces:newList(),cr:false,
            data:newList(),type:'',id:s.lastEventId,origin},null);
    }
    /* "Dispatch the event": the source's last event ID string from the buffer, then, when the data
       buffer is not empty, a MessageEvent (its origin the final URL's), dispatched in a task
       unless the source is CLOSED by then. */
    function dispatchEvent(target,s,stream){
        s.lastEventId=stream.id;
        if(!stream.data.length){stream.type='';return;}
        const data=apply(join,stream.data,['\n']),type=stream.type===''?'message':stream.type;
        stream.data=newList();stream.type='';
        const event=new MessageEvent(type,setPrototype({data,origin:stream.origin,lastEventId:s.lastEventId},null));
        task(()=>{if(s.ready!==CLOSED)fire(target,event);});
    }
    /* One line of the stream, as "Interpreting an event stream" processes it. Field names are
       compared literally. */
    function processLine(target,s,stream,line){
        if(line===''){dispatchEvent(target,s,stream);return;}
        const colon=indexOf(line,':',0);
        if(colon===0)return;
        let field=line,value='';
        if(colon>0){
            field=slice(line,0,colon);value=slice(line,colon+1,line.length);
            if(value.length&&code(value,0)===32)value=slice(value,1,value.length);
        }
        if(field==='event')stream.type=value;
        else if(field==='data')add(stream.data,value);
        else if(field==='id'){if(indexOf(value,'\0',0)<0)stream.id=value;}
        else if(field==='retry'){if(apply(regexExec,digits,[value])!==null)s.reconnection=+value;}
    }
    /* A chunk of the body, UTF-8 decoded (one leading BOM stripped) and read line by line: a line
       ends at CRLF, at an LF, or at a CR not followed by an LF (a CR that ends a chunk waits for
       the next chunk's first character). */
    function interpret(target,s,stream,bytes){
        const text=apply(textDecode,nativeHost,[stream.decoder,bytes,0,apply(abLength,bytes,[]),true]),n=text.length;
        let start=0;
        if(stream.cr&&n){stream.cr=false;if(code(text,0)===10)start=1;}
        let lf=-2,cr=-2;
        for(;;){
            if(lf!==-1&&lf<start)lf=indexOf(text,'\n',start);
            if(cr!==-1&&cr<start)cr=indexOf(text,'\r',start);
            const end=lf<0?cr:cr<0?lf:lf<cr?lf:cr;
            if(end<0)break;
            let line=slice(text,start,end);
            if(stream.pieces.length){add(stream.pieces,line);line=apply(join,stream.pieces,['']);stream.pieces=newList();}
            processLine(target,s,stream,line);
            start=end+1;
            if(code(text,end)===13){if(start<n){if(code(text,start)===10)start++;}else stream.cr=true;}
        }
        if(start<n)add(stream.pieces,slice(text,start,n));
    }
    /* Fetch the source's request and process the response as the constructor's steps say. */
    function connect(s){
        const attempt=++s.attempt,target=s.target;let stream=null,ended=false;
        const connection=fetch.eventSource(s.url,s.credentials,s.lastEventId,(kind,value,resume)=>{
            if(kind!=='response'&&kind!=='chunk')ended=true;
            if(s.attempt!==attempt||s.ready===CLOSED)return;
            try{
                if(kind==='response'){
                    if(value.status!==200||value.essence!=='text/event-stream'){stop(s);failConnection(target,s);return;}
                    s.announced=true;stream=eventStream(s,value.url);announce(target,s);resume();return;
                }
                if(kind==='chunk'){if(stream!==null)interpret(target,s,stream,value);resume();return;}
                s.connection=null;
                /* processResponseEndOfBody: the body ended, the stream's unfinished event is
                   discarded. */
                if(kind==='end'&&value===undefined){reestablish(target,s);return;}
                if(aborted(value)||(kind==='error'&&!s.announced)){failConnection(target,s);return;}
                reestablish(target,s);
            }catch(error){stop(s);failConnection(target,s);report(error);}
        });
        if(s.attempt===attempt&&!ended)s.connection=connection;
    }
    class EventSource extends EventTarget {
        constructor(url,eventSourceInitDict=undefined){
            if(!arguments.length)throw new NativeTypeError('EventSource requires a URL');
            url=usv(url);const credentials=withCredentials(eventSourceInitDict);
            super();
            const record=parseURL(url);
            if(record===null)throw new NativeDOMException('The EventSource URL cannot be parsed','SyntaxError');
            const s=setPrototype({url:apply(urlHref,record,[]),credentials,ready:CONNECTING,reconnection:3000,lastEventId:'',
                announced:false,attempt:0,connection:null,timer:0,target:this},null);
            apply(weakSet,slots,[this,s]);retain(this,s);
            connect(s);
        }
        get url(){return state(this).url;}
        get withCredentials(){return state(this).credentials;}
        get readyState(){return state(this).ready;}
        /* close(): aborts the fetch, and the reconnection's wait; queued events are not dispatched. */
        close(){const s=state(this);stop(s);s.ready=CLOSED;retain(this,s);}
    }
    handlers(EventSource.prototype,['open','message','error']);
    define(EventSource.prototype,Symbol.toStringTag,{value:'EventSource',configurable:true});
    for(const key of Object.getOwnPropertyNames(EventSource.prototype)){
        if(key==='constructor')continue;
        const d=Object.getOwnPropertyDescriptor(EventSource.prototype,key);d.enumerable=true;define(EventSource.prototype,key,d);
    }
    for(const [key,value] of [['CONNECTING',CONNECTING],['OPEN',OPEN],['CLOSED',CLOSED]])
        for(const object of [EventSource,EventSource.prototype])define(object,key,{value,enumerable:true});
    return {EventSource,
        handlerTarget:(target,type)=>(type==='open'||type==='message'||type==='error')&&
            (typeof target==='object'&&target!==null)&&apply(weakHas,slots,[target])};
}
