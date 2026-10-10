/* Supplemental contracts on the real Nocturne web_live/native request path.
   A site pass still requires the separate public-network browser GUI tests. */
async function runXHRCases() {
    let n=0;
    const assert=(v,label)=>{n++;if(!v)throw Error('xhr '+label);};
    const throws=(fn,name,label)=>{let e;try{fn();}catch(x){e=x;}assert(e&&e.name===name,label);};
    const settle=x=>new Promise(resolve=>x.addEventListener('loadend',resolve,{once:true}));
    const pause=()=>new Promise(resolve=>setTimeout(resolve,10));
    const x=new XMLHttpRequest();
    assert(x instanceof XMLHttpRequestEventTarget && x instanceof EventTarget,'inheritance');
    assert(x.upload instanceof XMLHttpRequestUpload && x.upload===x.upload,'upload identity');
    assert(x.readyState===0 && x.status===0 && x.responseText==='' && x.responseURL==='' && x.responseXML===null,'initial');
    assert(x.timeout===0 && x.withCredentials===false && x.responseType==='' && x.response==='','initial controls');
    assert(x.getAllResponseHeaders()==='' && x.getResponseHeader('content-type')===null,'initial headers');
    throws(()=>new XMLHttpRequestUpload(),'TypeError','illegal upload constructor');
    throws(()=>new XMLHttpRequestEventTarget(),'TypeError','illegal event target constructor');
    throws(()=>XMLHttpRequest.prototype.open.call({},'GET','api/json'),'TypeError','receiver');
    throws(()=>x.open(),'TypeError','required open arguments');
    throws(()=>x.open('bad method','api/json'),'SyntaxError','method token');
    throws(()=>x.open('TRACE','api/json'),'SecurityError','forbidden method');
    throws(()=>x.open('\u1000','api/json'),'TypeError','bytestring');
    throws(()=>x.open('GET','http://['),'SyntaxError','URL syntax');
    throws(()=>x.open('GET','api/json',false),'NotSupportedError','sync explicitly unsupported');
    throws(()=>x.open('GET','api/json',true,'user','password'),'NotSupportedError','credentials explicitly unsupported');
    throws(()=>x.send(),'InvalidStateError','send before open');
    throws(()=>x.setRequestHeader('x','y'),'InvalidStateError','header before open');
    const pe=new ProgressEvent('progress',{lengthComputable:true,loaded:17,total:42});
    assert(pe instanceof Event && pe.loaded===17 && pe.total===42 && pe.lengthComputable,'progress counts');
    throws(()=>new ProgressEvent(),'TypeError','progress required type');
    assert(new ProgressEvent('x').loaded===0,'default progress');

    const ready=[],events=[],ordering=[];
    x.onreadystatechange=()=>{ready.push(x.readyState);ordering.push('property');};
    x.addEventListener('readystatechange',()=>ordering.push('listener'));
    for(const type of ['loadstart','progress','load','error','loadend'])x.addEventListener(type,e=>{
        assert(e.target===x && e.currentTarget===x && e.isTrusted,'native '+type);
        events.push(type);
        if(type==='progress')assert(e.loaded===22,'download byte count');
    });
    x.open('get','api/json');
    assert(x.readyState===1 && ready.join(',')==='1','open immediate state');
    throws(()=>x.setRequestHeader('bad name','value'),'SyntaxError','header name');
    throws(()=>x.setRequestHeader('x','bad\nvalue'),'SyntaxError','header injection');
    x.setRequestHeader('Host','attacker.invalid');x.setRequestHeader('Cookie','secret');
    x.responseType='unknown';assert(x.responseType==='','unknown response type ignored');
    x.responseType='blob';assert(x.responseType==='blob','binary Blob response type');x.responseType='';
    const first=settle(x);x.send();
    throws(()=>x.send(),'InvalidStateError','duplicate send');
    throws(()=>{x.withCredentials=true;},'InvalidStateError','credentials after send');
    throws(()=>x.setRequestHeader('x','value'),'InvalidStateError','header after send');
    await first;
    assert(ready.join(',')==='1,2,3,4','state order '+ready);
    assert(ordering.every((v,i)=>v===(i%2?'listener':'property')),'event property listener activation order');
    assert(events.join(',')==='loadstart,progress,load,loadend','event order '+events);
    assert(x.status===200 && x.responseText==='{"ok":true,"value":42}' && x.response===x.responseText,'GET text');
    assert(x.responseURL==='http://fixture.test/dir/api/json','final URL');
    assert(x.getResponseHeader('X-Fixture')==='yes' && x.getResponseHeader('Set-Cookie')===null,'response headers');
    assert(x.getAllResponseHeaders()==='content-type: application/json\r\nx-fixture: yes\r\n','sorted headers');
    throws(()=>{x.responseType='json';},'InvalidStateError','type after complete');
    throws(()=>x.overrideMimeType('text/plain'),'InvalidStateError','MIME after complete');

    for(const type of ['json','arraybuffer']) {
        const r=new XMLHttpRequest();r.open('GET','api/json');r.responseType=type;
        assert(r.response===null,'nontext pending');throws(()=>r.responseText,'InvalidStateError','typed responseText');
        const p=settle(r);r.send();await p;
        assert(r.response===r.response,'cached response identity');
        if(type==='json')assert(r.response.ok && r.response.value===42,'JSON');
        else assert(new TextDecoder().decode(r.response)==='{"ok":true,"value":42}','ArrayBuffer');
    }
    const missing=new XMLHttpRequest();missing.open('GET','missing');missing.responseType='json';
    let loaded=false;missing.onload=()=>loaded=true;const mp=settle(missing);missing.send();await mp;
    assert(missing.status===404 && loaded && missing.response===null,'HTTP error is load, invalid JSON is null');
    const post=new XMLHttpRequest(),uploaded=[];post.open('POST','api/post');post.setRequestHeader('x-fixture','yes');
    for(const t of ['loadstart','progress','load','loadend'])post.upload.addEventListener(t,e=>{uploaded.push(t);if(t!=='loadstart')assert(e.loaded===21 && e.total===21 && e.lengthComputable,'upload count');});
    const pp=settle(post);post.send('Nocturne request body');await pp;
    assert(post.responseText==='Nocturne request body' && post.status===200,'POST bytes');
    assert(uploaded.join(',')==='loadstart,progress,load,loadend','upload events');
    const slow=new XMLHttpRequest();slow.open('GET','api/slow');slow.timeout=3;
    const errors=[];for(const t of ['timeout','error','abort','load','loadend'])slow.addEventListener(t,()=>errors.push(t));
    const sp=settle(slow);slow.send();await sp;
    assert(slow.status===0 && slow.readyState===4 && slow.responseText==='' && errors.join(',')==='timeout,loadend','timeout');
    const abort=new XMLHttpRequest();abort.open('GET','api/slow');let aborted=0;abort.onabort=()=>aborted++;
    abort.send();abort.abort();await pause();
    assert(aborted===1 && abort.readyState===0 && abort.status===0,'abort exactly once');
    abort.abort();assert(aborted===1,'abort idempotent');

    // Reusing the same object from a state callback must not deliver the old
    // response, headers, timeout, upload, or completion into the new request.
    const reuse=new XMLHttpRequest();let replaced=false,reuseLoads=0;
    reuse.onreadystatechange=()=>{if(reuse.readyState===2&&!replaced){replaced=true;reuse.open('GET','api/json');reuse.send();}};
    reuse.onload=()=>reuseLoads++;reuse.open('GET','missing');const rp=settle(reuse);reuse.send();await rp;await pause();
    assert(replaced && reuseLoads===1 && reuse.status===200 && JSON.parse(reuse.responseText).value===42,'reentrant open at headers');
    const ls=new XMLHttpRequest();let starts=0,lsLoads=0;
    ls.onloadstart=()=>{if(starts++===0){ls.open('GET','api/json');ls.send();}};
    ls.onload=()=>lsLoads++;ls.open('GET','missing');const lp=settle(ls);ls.send();await lp;
    assert(starts===2 && lsLoads===1 && ls.status===200,'reentrant loadstart');
    const changing=new XMLHttpRequest();changing.open('GET','api/slow');changing.timeout=1000;
    const cp=settle(changing);changing.send();changing.timeout=1;await cp;
    assert(changing.status===0 && changing.readyState===4,'change timeout after send');
    const poisoned=new XMLHttpRequest();poisoned.open('GET','api/json');
    const np=settle(poisoned),oldPromise=globalThis.Promise,oldFetch=globalThis.fetch;
    globalThis.Promise={resolve(){throw Error('public Promise used');}};globalThis.fetch=()=>{throw Error('public fetch used');};
    poisoned.send();globalThis.Promise=oldPromise;globalThis.fetch=oldFetch;await np;
    assert(poisoned.status===200,'native scheduling unaffected by public replacement');
    const species=new XMLHttpRequest();species.open('GET','api/json');species.responseType='arraybuffer';
    const speciesDone=settle(species),promiseDescriptor=Object.getOwnPropertyDescriptor(Promise.prototype,'constructor'),bufferDescriptor=Object.getOwnPropertyDescriptor(ArrayBuffer.prototype,'constructor');
    let leaked=0;
    Object.defineProperty(Promise.prototype,'constructor',{configurable:true,get(){leaked++;throw Error('promise species leak');}});
    Object.defineProperty(ArrayBuffer.prototype,'constructor',{configurable:true,get(){leaked++;throw Error('buffer species leak');}});
    species.send();
    // Restore before awaiting (the page's own await is allowed to consult its
    // Promise prototype). Completion is checked separately for ArrayBuffer.
    Object.defineProperty(Promise.prototype,'constructor',promiseDescriptor);
    await speciesDone;Object.defineProperty(ArrayBuffer.prototype,'constructor',bufferDescriptor);
    assert(species.status===200 && new TextDecoder().decode(species.response)==='{"ok":true,"value":42}' && leaked===0,'host-private promise and buffer species');
    const unicode=new XMLHttpRequest();unicode.open('POST','api/echo');unicode.responseType='arraybuffer';
    const up=settle(unicode);unicode.send('\ud800');await up;
    assert(Array.from(new Uint8Array(unicode.response)).join(',')==='239,191,189','upload USVString replacement bytes');
    for(const [given,want] of [
        ['text/plain; note="x;charset=windows-1252"; charset=iso-8859-1','text/plain; note="x;charset=windows-1252"; charset=UTF-8'],
        ['not-a-mime; charset=windows-1252','not-a-mime; charset=windows-1252'],
        ['text/plain; note="unfinished\\','text/plain; note="unfinished\\']
    ]) {
        const mime=new XMLHttpRequest();mime.open('POST','api/headers');mime.setRequestHeader('content-type',given);
        const done=settle(mime);mime.send('x');await done;
        assert(mime.responseText==='content-type: '+want+'\r\n','MIME charset respects quoted parameters');
    }
    const timers=[];for(let i=0;i<130;i++)timers.push(setTimeout(()=>{},60000));
    assert(timers.length===130&&new Set(timers).size===130,'native timers grow past the former cap');
    const capacity=new XMLHttpRequest();capacity.open('GET','api/json');capacity.timeout=1000;
    const capacityDone=settle(capacity);capacity.send();await capacityDone;
    assert(capacity.status===200&&capacity.readyState===4,'XHR timeout timer coexists with grown table');
    for(const id of timers)clearTimeout(id);
    capacity.open('GET','api/json');capacity.timeout=0;const retryDone=settle(capacity);capacity.send();await retryDone;
    assert(capacity.status===200,'XHR reuse after timer cancellation');
    const backlog=new XMLHttpRequest();backlog.open('GET','api/json');
    let backlogError=false;backlog.onerror=()=>backlogError=true;const backlogDone=settle(backlog);backlog.send();
    timers.length=0;for(let i=0;i<130;i++)timers.push(setTimeout(()=>{},60000));
    await backlogDone;for(const id of timers)clearTimeout(id);
    assert(!backlogError && backlog.status===200 && backlog.readyState===4,'completion task survives grown timer table');
    const invalid=new XMLHttpRequest();invalid.open('GET','file:///home/not-a-network-url');let networkError=false;invalid.onerror=()=>networkError=true;
    const ip=settle(invalid);invalid.send();assert(!networkError,'unsupported network error asynchronous');await ip;
    assert(networkError && invalid.status===0,'transport rejects non HTTP');
    const binary=new XMLHttpRequest();binary.open('POST','api/json');binary.send(new Uint8Array([1,0,2]));binary.abort();assert(binary.readyState===0,'binary upload can be cancelled');
    const screenCopy=screen;
    assert(screenCopy===screen && screen instanceof Screen && screen instanceof EventTarget,'screen identity');
    assert(screen.width>0 && screen.height>0 && screen.availWidth<=screen.width && screen.availHeight<=screen.height,'native screen dimensions');
    assert(screen.colorDepth===24 && screen.pixelDepth===24 && devicePixelRatio===1,'Nocturne pixel format');
    throws(()=>new Screen(),'TypeError','screen not constructible');
    return n;
}
