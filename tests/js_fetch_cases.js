/* One combined Fetch/Headers/Request/Response/Abort regression pass. Transport
   cases use web_live and its native host fixture, including raw byte echo.
   HTTP worker/header/preflight checks live in webnettest; this fixture does
   not itself perform HTTP. Site/GUI acceptance remains a separate step. */
async function runFetchCases() {
    let count=0;
    const assert=(v,label)=>{count++;if(!v)throw Error('fetch '+label);};
    const throws=(fn,name,label)=>{let e;try{fn();}catch(x){e=x;}assert(e&&e.name===name,label);};
    const rejects=async(p,name,label)=>{let e;try{await p;}catch(x){e=x;}assert(e&&e.name===name,label);};
    const numbers=bytes=>Array.from(new Uint8Array(bytes)).join(',');
    const h=new Headers([['Z-Test','one'],['a-test','two'],['z-test','three']]);
    assert(h.get('Z-TEST')==='one, three'&&h.get('absent')===null,'header combination');
    assert(Array.from(h.keys()).join(',')==='a-test,z-test','header sorted iteration');
    assert(Array.from(h.values()).join('|')==='two|one, three','header values');
    const copied=new Headers(h);h.set('z-test','changed');assert(copied.get('z-test')==='one, three','header independent copy');
    copied.delete('A-Test');assert(!copied.has('a-test'),'header delete');
    copied.append('space',' \t a \t ');assert(copied.get('space')==='a','HTTP whitespace');
    const cookies=new Headers([['set-cookie','a=1'],['set-cookie','b=2']]);
    assert(cookies.getSetCookie().join('|')==='a=1|b=2','separate set-cookie');
    assert(Array.from(cookies).length===2,'set-cookie iteration not combined');
    cookies.set('set-cookie','c=3');assert(cookies.getSetCookie().join('|')==='c=3','set-cookie replacement');
    throws(()=>new Headers([['x','y','z']]),'TypeError','pair arity');
    throws(()=>new Headers(['xy']),'TypeError','pair must be sequence object');
    throws(()=>new Headers(42),'TypeError','HeadersInit object');
    throws(()=>new Headers().has('bad name'),'TypeError','empty has validates name');
    throws(()=>h.append('x','bad\nvalue'),'TypeError','header injection');
    throws(()=>h.set('x','\u0100'),'TypeError','header ByteString');
    throws(()=>h.get(Symbol()),'TypeError','name Symbol');
    throws(()=>h.get(),'TypeError','required name');
    throws(()=>Headers.prototype.append.call({},'x','y'),'TypeError','header receiver');
    const iterator=new Headers({b:'2',c:'3'}),seen=[];
    iterator.forEach((value,key)=>{seen.push(key);if(key==='b')iterator.set('d','4');});
    assert(seen.join(',')==='b,c,d','live iteration');

    const basic=new Request('api/json#fragment');
    assert(basic.url==='http://fixture.test/dir/api/json#fragment'&&basic.method==='GET','request URL and default method');
    assert(basic.mode==='cors'&&basic.credentials==='same-origin'&&basic.redirect==='follow','request transport defaults');
    assert(basic.cache==='default'&&basic.referrer==='about:client'&&basic.referrerPolicy===''&&basic.integrity===''&&!basic.keepalive,'request metadata defaults');
    assert(basic.destination===''&&basic.duplex==='half'&&!basic.isReloadNavigation&&!basic.isHistoryNavigation,'request metadata');
    assert(basic.headers===basic.headers&&basic.signal===basic.signal&&basic.signal instanceof AbortSignal,'request stable objects');
    assert(basic.body===null&&!basic.bodyUsed,'null request body');
    assert(await basic.text()===''&&await basic.text()===''&&!basic.bodyUsed,'null body repeatable');
    assert(new Request(new URL('http://fixture.test/dir/api/json')).url==='http://fixture.test/dir/api/json','URL input');
    assert(new Request('api/json',{method:'pOsT'}).method==='POST','standard method normalization');
    assert(new Request('api/json',{method:'patch'}).method==='patch','extension method case preserved');
    throws(()=>new Request(),'TypeError','required input');
    throws(()=>new Request('http://['),'TypeError','invalid URL');
    throws(()=>new Request('https://user:password@fixture.test/'),'TypeError','URL credentials');
    for(const method of ['bad method','TRACE','connect','TRACK','\u0100'])throws(()=>new Request('api/json',{method}),'TypeError','invalid method '+method);
    for(const key of ['credentials','mode','redirect','cache','referrerPolicy'])throws(()=>new Request('api/json',{[key]:'invalid'}),'TypeError','invalid '+key);
    throws(()=>new Request('api/json',{mode:'navigate'}),'TypeError','navigate rejected');
    throws(()=>new Request('api/json',{cache:'only-if-cached'}),'TypeError','cache mode requires same origin');
    for(const cache of ['default','no-store','reload','no-cache','force-cache','only-if-cached']) {
        const cached=new Request('api/json',{cache,mode:'same-origin'}),copy=cached.clone();
        assert(cached.cache===cache&&copy.cache===cache&&copy.mode==='same-origin','cache constructor and clone '+cache);
        assert(new Request(cached).cache===cache,'cache inherited '+cache);
        assert(new Request(cached,{cache:'reload'}).cache==='reload','cache override '+cache);
    }
    const cacheOnly=new Request('api/json',{cache:'only-if-cached',mode:'same-origin'});
    throws(()=>new Request(cacheOnly,{mode:'cors'}),'TypeError','inherited cache mode still requires same-origin');
    assert(new Request(cacheOnly,{mode:'cors',cache:'default'}).mode==='cors','cache override permits cors');
    throws(()=>new Request('api/json',{signal:{aborted:false}}),'TypeError','signal brand');
    throws(()=>new Request('api/json',{body:''}),'TypeError','even empty GET body forbidden');
    throws(()=>new Request('api/json',{method:'HEAD',body:new ArrayBuffer(0)}),'TypeError','even empty HEAD body forbidden');
    const request=new Request('api/echo',{method:'POST',headers:{'x-test':'yes','Cookie':'secret','Host':'attacker.invalid','X-HTTP-Method':'TRACE'},body:'sample'});
    assert(request.headers.get('x-test')==='yes'&&request.headers.get('cookie')===null&&request.headers.get('host')===null&&request.headers.get('x-http-method')===null,'request header guard');
    request.headers.set('sec-test','no');assert(!request.headers.has('sec-test'),'request mutation guard');
    assert(request.headers.get('content-type')==='text/plain;charset=UTF-8','string default MIME');
    const clone=request.clone();assert(!request.bodyUsed&&!clone.bodyUsed&&clone.headers!==request.headers&&clone.signal!==request.signal,'request clone identity');
    clone.headers.set('x-test','clone');assert(request.headers.get('x-test')==='yes','request clone headers independent');
    assert(await clone.text()==='sample'&&clone.bodyUsed&&!request.bodyUsed,'request clone body independent');
    const transferred=new Request(request);assert(request.bodyUsed&&!transferred.bodyUsed&&await transferred.text()==='sample','request body transfer');
    throws(()=>new Request(request),'TypeError','used request transfer rejected');
    throws(()=>request.clone(),'TypeError','used request clone rejected');
    const replacement=new Request(request,{body:'new'});assert(await replacement.text()==='new','body override can replace consumed input');
    const nullOverride=new Request('api/echo',{method:'POST',body:'inherit'}),nullCopy=new Request(nullOverride,{body:null});
    assert(nullOverride.bodyUsed&&await nullCopy.text()==='inherit','null init body inherits source');
    const noCors=new Request('api/json',{mode:'no-cors',headers:{'content-type':'application/json','x-unsafe':'no','accept':'text/plain'}});
    assert(noCors.headers.get('accept')==='text/plain'&&noCors.headers.get('x-unsafe')===null&&noCors.headers.get('content-type')===null,'no-cors header guard');
    noCors.headers.set('accept','x'.repeat(129));assert(noCors.headers.get('accept')==='text/plain','no-cors value limit');
    throws(()=>new Request('api/json',{mode:'no-cors',method:'PUT'}),'TypeError','no-cors method guard');

    const input=new Uint8Array([99,0,128,255,100]),view=new DataView(input.buffer,1,3),response=new Response(view,{status:201,statusText:'Created',headers:{'x-test':'yes','set-cookie':'secret'}});
    input[1]=7;
    assert(response.status===201&&response.statusText==='Created'&&response.ok&&response.type==='default'&&response.url===''&&!response.redirected,'response metadata');
    assert(response.headers.get('content-type')===null&&response.headers.get('set-cookie')===null,'buffer MIME and response guard');
    assert(!response.bodyUsed,'body initially unused');
    const responseCopy=response.clone(),reading=response.arrayBuffer();assert(response.bodyUsed,'body consumed immediately');
    await rejects(response.text(),'TypeError','concurrent consumption rejected');
    throws(()=>response.clone(),'TypeError','consumed response clone rejected');
    assert(numbers(await reading)==='0,128,255','BufferSource offset and snapshot');
    assert(Array.from(await responseCopy.bytes()).join(',')==='0,128,255','bytes clone');
    const empty=new Response();assert(empty.body===null&&await empty.text()===''&&await empty.arrayBuffer() instanceof ArrayBuffer&&!empty.bodyUsed,'null response repeated');
    const invalidJSON=new Response('invalid');await rejects(invalidJSON.json(),'SyntaxError','invalid JSON');assert(invalidJSON.bodyUsed,'invalid JSON consumes body');
    const utf8=new Response(new Uint8Array([239,187,191,65,255]));assert(await utf8.text()==='A\ufffd','UTF-8 BOM and replacement');
    const unicode=new Response('\ud800');assert(numbers(await unicode.arrayBuffer())==='239,191,189','string USV replacement');
    const synthetic=new Response('x',{url:'http://attacker.invalid',bytes:new ArrayBuffer(1)});assert(synthetic.url===''&&await synthetic.text()==='x','nonstandard init fields cannot override native state');
    for(const status of [0,199,600])throws(()=>new Response(null,{status}),'RangeError','status range '+status);
    for(const status of [204,205,304])throws(()=>new Response('',{status}),'TypeError','null-body status '+status);
    throws(()=>new Response(null,{statusText:'bad\r\nphrase'}),'TypeError','statusText injection');
    throws(()=>new Response(null,{statusText:'\u0100'}),'TypeError','statusText ByteString');
    const json=Response.json({ok:true},{status:202});assert(json.status===202&&json.headers.get('content-type')==='application/json'&&(await json.json()).ok,'Response.json');
    const contentType=Response.json(null,{headers:{'content-type':'custom/type'}});assert(contentType.headers.get('content-type')==='custom/type'&&await contentType.text()==='null','JSON explicit MIME');
    throws(()=>Response.json(undefined),'TypeError','unserializable undefined');
    const circular={};circular.self=circular;throws(()=>Response.json(circular),'TypeError','circular JSON');
    const error=Response.error();assert(error.type==='error'&&error.status===0&&!error.ok&&error.body===null,'Response.error');
    throws(()=>error.headers.set('x','y'),'TypeError','error immutable headers');
    assert(error.clone().type==='error','error clone');
    const redirect=Response.redirect('other',307);assert(redirect.status===307&&redirect.headers.get('location')==='http://fixture.test/dir/other'&&redirect.body===null,'Response.redirect');
    throws(()=>redirect.headers.delete('location'),'TypeError','redirect immutable headers');
    const redirectClone=redirect.clone();assert(redirectClone.headers.get('location')===redirect.headers.get('location'),'immutable headers cloned');
    throws(()=>redirectClone.headers.append('x','y'),'TypeError','clone retains immutable guard');
    throws(()=>Response.redirect('other',200),'RangeError','redirect status');
    throws(()=>Response.redirect('http://['),'TypeError','redirect URL');
    throws(()=>Response.prototype.clone.call({}),'TypeError','response receiver');
    await rejects(Response.prototype.text.call({}),'TypeError','Body promise receiver');
    const streamed=new Response('bytes'),bodyStream=streamed.body;
    assert(bodyStream instanceof ReadableStream&&streamed.body===bodyStream,'real same-object response stream');
    const bodyReader=bodyStream.getReader(),firstChunk=await bodyReader.read();
    assert(!firstChunk.done&&firstChunk.value instanceof Uint8Array&&new TextDecoder().decode(firstChunk.value)==='bytes','response stream contains real bytes');
    assert((await bodyReader.read()).done&&streamed.bodyUsed,'stream EOF and disturbance');bodyReader.releaseLock();
    await rejects(streamed.text(),'TypeError','stream reader consumes the body');
    const unsupported=new Response('keep');assert((await new Response('keep').blob()).size===4,'Blob contains real body bytes');await rejects(unsupported.formData(),'NotSupportedError','FormData explicitly unsupported');
    assert(!unsupported.bodyUsed&&await unsupported.text()==='keep','unsupported reader does not falsely consume');

    const controller=new AbortController(),signal=controller.signal;
    assert(signal===controller.signal&&!signal.aborted&&signal.reason===undefined,'signal defaults');
    throws(()=>new AbortSignal(),'TypeError','signal illegal constructor');
    throws(()=>AbortSignal.prototype.throwIfAborted.call({}),'TypeError','signal receiver');
    const reason={why:'custom'},order=[];signal.onabort=()=>order.push('property');signal.addEventListener('abort',e=>{order.push('listener');assert(e.isTrusted,'trusted abort');});
    const following=new Request('api/json',{signal}),followingClone=following.clone();
    assert(following.signal!==signal&&followingClone.signal!==following.signal,'distinct dependent signals');
    controller.abort(reason);controller.abort('ignored');
    assert(signal.aborted&&signal.reason===reason&&following.signal.reason===reason&&followingClone.signal.reason===reason,'signal propagation and idempotence');
    assert(order.join(',')==='property,listener','onabort activation order');
    let thrown;try{signal.throwIfAborted();}catch(e){thrown=e;}assert(thrown===reason,'throw exact reason');
    assert(AbortSignal.abort().reason.name==='AbortError'&&AbortSignal.abort(null).reason===null,'abort default and null reason');
    const a=new AbortController(),b=new AbortController(),any=AbortSignal.any([a.signal,b.signal]);b.abort(reason);a.abort('later');assert(any.aborted&&any.reason===reason,'any first reason');
    assert(AbortSignal.any([signal]).reason===reason&&!AbortSignal.any([]).aborted,'any preaborted and empty');
    throws(()=>AbortSignal.any([{}]),'TypeError','any brand');throws(()=>AbortSignal.any({length:0}),'TypeError','any iterable');
    throws(()=>AbortSignal.timeout(-1),'TypeError','negative timeout');throws(()=>AbortSignal.timeout(Infinity),'TypeError','infinite timeout');
    const longTimeout=AbortSignal.timeout(2147483648);
    assert(longTimeout instanceof AbortSignal&&!longTimeout.aborted&&longTimeout.reason===undefined,'long timeout survives native timer chunk boundary');
    throws(()=>AbortSignal.timeout(18446744073709551616),'TypeError','unsigned 64-bit timeout boundary');
    const timed=AbortSignal.timeout(1);await new Promise(resolve=>timed.addEventListener('abort',resolve,{once:true}));assert(timed.reason.name==='TimeoutError','native timeout');

    await rejects(fetch(),'TypeError','fetch required input');
    let sync=false,bad;try{bad=fetch('api/json',{headers:{'bad name':'x'}});}catch(_){sync=true;}
    assert(!sync&&bad instanceof Promise,'validation failure is promise');await rejects(bad,'TypeError','validation rejection');
    await rejects(fetch('file:///data/secret'),'TypeError','network local scheme rejected');
    for(const init of [{redirect:'manual'},{integrity:'sha256-test'},{referrerPolicy:'origin'}])
        await rejects(fetch('api/json',init),'NotSupportedError','unsupported transport options explicit');
    // This focused host intentionally has no release_request callback. Product
    // browser keepalive support must not be confused with this host boundary.
    await rejects(fetch('api/json',{keepalive:true}),'TypeError','host without keepalive retirement rejects explicitly');
    const noCorsResponse=await fetch('api/fetch-policy',{mode:'no-cors'}),noCorsPolicy=await noCorsResponse.json();
    assert(noCorsResponse.type==='basic'&&noCorsResponse.status===200&&noCorsPolicy.no_cors&&!noCorsPolicy.no_referrer,'same-origin no-cors reaches native policy');
    for(const init of [{referrer:''},{referrerPolicy:'no-referrer'}]) {
        const policy=await (await fetch('api/fetch-policy',init)).json();
        assert(policy.no_referrer&&!policy.no_cors,'no-referrer override reaches native policy');
    }
    let aborted;try{await fetch('api/json',{signal});}catch(e){aborted=e;}assert(aborted===reason,'preaborted exact reason');
    let cacheAborted;try{await fetch('api/json',{cache:'only-if-cached',mode:'same-origin',signal});}catch(e){cacheAborted=e;}
    assert(cacheAborted===reason,'preaborted cache-only fetch retains exact reason');
    await rejects(fetch(cacheOnly),'TypeError','cache-only miss is a network error');
    await rejects(fetch('http://other.fixture.test/api/json',{cache:'only-if-cached',mode:'same-origin'}),'TypeError','cache-only cross-origin target cannot cause network');
    await rejects(fetch(cacheOnly,{mode:'cors'}),'TypeError','fetch init validates inherited cache-only mode');
    const beforeCacheOnly=await (await fetch('api/cache-policy')).json();
    await rejects(fetch(cacheOnly),'TypeError','cache-only observed miss');
    await rejects(fetch('http://other.fixture.test/api/json',{cache:'only-if-cached',mode:'same-origin'}),'TypeError','cache-only observed cross-origin miss');
    const afterCacheOnly=await (await fetch('api/cache-policy')).json();
    assert(afterCacheOnly.fetch_requests===beforeCacheOnly.fetch_requests+1,'cache-only misses never call native host');
    const networkCacheModes=['default','no-store','reload','no-cache','force-cache'];
    for(const cache of networkCacheModes) {
        const cached=new Request('api/json',{cache,headers:{'cache-control':'custom-cache','pragma':'custom-pragma'}});
        const result=await fetch(cached);
        assert(result.status===200&&(await result.json()).value===42,'cacheless host fetch '+cache);
        assert(cached.cache===cache&&cached.headers.get('cache-control')==='custom-cache'&&cached.headers.get('pragma')==='custom-pragma','author cache headers unchanged '+cache);
        const policy=await (await fetch('api/cache-policy',{cache})).json();
        assert(policy.cache_mode===networkCacheModes.indexOf(cache),'native cache policy '+cache);
        assert(!policy.has_cache_control&&!policy.has_pragma,'HTTP cache fields are not author headers '+cache);
    }
    const policyHeaders=new Request('api/cache-policy',{cache:'no-store',headers:{'cache-control':'custom','pragma':'custom'}});
    const headerPolicy=await (await fetch(policyHeaders)).json();
    assert(headerPolicy.has_cache_control&&headerPolicy.has_pragma&&policyHeaders.headers.get('cache-control')==='custom','author cache fields remain author fields');
    await rejects(fetch(cacheOnly),'TypeError','successful fetches do not fabricate cache entries');
    const network=await fetch(new Request('api/json',{credentials:'omit'}));
    assert(network.status===200&&network.statusText==='OK'&&network.type==='basic'&&network.url==='http://fixture.test/dir/api/json','native metadata');
    assert(network.headers.get('set-cookie')===null&&network.headers.get('x-fixture')==='yes','native headers');
    throws(()=>network.headers.set('x','y'),'TypeError','network headers immutable');
    const nativeClone=network.clone();assert((await network.json()).value===42&&numbers(await nativeClone.arrayBuffer()).startsWith('123,'),'native clone JSON and bytes');
    const largeHeaders=await fetch('api/long-headers');
    assert(largeHeaders.headers.get('x-long')==='L'.repeat(6000),'complete large header survives native completion lifetime');
    assert(largeHeaders.headers.get('x-tail')==='complete'&&largeHeaders.headers.get('content-type')==='text/plain','late native headers not truncated');
    const largeClone=largeHeaders.clone();assert(largeClone.headers.get('x-long').length===6000&&await largeClone.text()==='complete headers','large native headers clone');
    await rejects(fetch('api/header-allocation-failure'),'TypeError','native header allocation failure explicitly rejected');
    assert((await import('./long-headers.mjs')).completeHeaders===42,'synchronous module MIME beyond inline header boundary');
    const form=new Request('api/echo',{method:'POST',body:new URLSearchParams([['message','a b'],['unicode','語']])});
    assert(form.headers.get('content-type')==='application/x-www-form-urlencoded;charset=UTF-8','URLSearchParams MIME');
    const echoed=await fetch(form);assert(form.bodyUsed&&await echoed.text()==='message=a+b&unicode=%E8%AA%9E','URLSearchParams native bytes');
    const upload=new Uint8Array([99,0,128,255,100]),binary=new Request('api/echo',{method:'POST',body:new DataView(upload.buffer,1,3)});upload[1]=7;
    const echoedBinary=await fetch(binary);assert(numbers(await echoedBinary.arrayBuffer())==='0,128,255','binary native upload not stringified');
    const original=new Request('api/echo',{method:'POST',headers:{'x-test':'old'},body:'original'}),overridden=await fetch(original,{headers:{'x-test':'new'},body:'override'});
    assert(!original.bodyUsed&&await overridden.text()==='override','fetch Request init body override');
    for(const method of ['PUT','PATCH','DELETE','OPTIONS']){const r=await fetch('api/echo',{method,body:'method '+method});assert(await r.text()==='method '+method,'native method '+method);}
    const head=await fetch('api/json',{method:'HEAD'});assert(head.body===null&&await head.text()===''&&!head.bodyUsed,'HEAD native null body');
    const missing=await fetch('missing');assert(missing.status===404&&!missing.ok,'HTTP errors fulfill');
    const pendingAbort=new AbortController(),pending=fetch('api/slow',{cache:'no-store',signal:AbortSignal.any([pendingAbort.signal])});pendingAbort.abort(reason);
    let pendingReason;try{await pending;}catch(e){pendingReason=e;}assert(pendingReason===reason,'pending native cancellation exact reason');
    const afterHeaders=new AbortController(),late=await fetch('api/json',{cache:'no-cache',signal:afterHeaders.signal}),lateClone=late.clone();afterHeaders.abort(reason);
    for(const r of [late,lateClone]){let e;try{await r.text();}catch(x){e=x;}assert(e===reason,'abort after response before consume');}
    const noRedirect=await fetch('api/json',{redirect:'error'});assert(noRedirect.status===200,'redirect error permits nonredirect');
    const speciesDescriptor=Object.getOwnPropertyDescriptor(ArrayBuffer.prototype,'constructor');let leaked=0;
    Object.defineProperty(ArrayBuffer.prototype,'constructor',{configurable:true,get(){leaked++;throw Error('buffer species leak');}});
    let speciesBytes;
    try {speciesBytes=await new Response(new Uint8Array([1,0,2])).clone().arrayBuffer();}
    finally {Object.defineProperty(ArrayBuffer.prototype,'constructor',speciesDescriptor);}
    assert(numbers(speciesBytes)==='1,0,2'&&leaked===0,'byte copy does not invoke species');
    return count;
}
