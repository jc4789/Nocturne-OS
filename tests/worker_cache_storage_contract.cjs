/* The Worker realm's Fetch (js_worker_runtime.js from its AbortSignal to the Fetch globals: the
 * js_fetch.js it includes, workerFetchTransport and the fetchBridge port, sliced from the
 * production source) with js_cache_storage.js and js_streams.js in a node vm realm. The native
 * transport is a recorder (host.request, host.resume, host.cancel) answered through the
 * transport's own loaded and frame hooks, and the storage broker cache_storage_contract.cjs's
 * in-memory one. It verifies the Request constructor's steps, the Response statics, the transport
 * query fetch hands the parent (modes, referrer, body), integrity, the streamed frames and their
 * flow control, opaque responses, close, the records Cache Storage keeps of this realm's objects,
 * and add, addAll and match through them; not browserjsworker.c, js_worker.c or js.c (the wire and
 * the parent's checks), not webcache.c, not a guest browser.
 * Run: node tests/worker_cache_storage_contract.cjs */
'use strict';
const fs = require('node:fs'), vm = require('node:vm'), crypto = require('node:crypto'), assert = require('node:assert/strict');
const {makeBroker, streamsSource, rejects} = require('./cache_storage_contract.cjs');
const web = 'user/libc/web/';
let checks = 0;
const check = (name, yes) => { checks++; assert(yes, name); };

const source = fs.readFileSync(web + 'js_worker_runtime.js', 'utf8');
function slice(start, end) {
    const a = source.indexOf(start), b = source.indexOf(end, a);
    assert(a >= 0 && b > a, start);
    return source.slice(a, b + end.length);
}
const fetchPart = slice('    /* @include js_abort.js */', '    const {Headers,Request,Response,fetch}=fetchBridge;\n');
assert(fetchPart.includes('/* @include js_fetch.js */') && !source.includes('workerFetchBridge') && !source.includes('workerAbortBridge'));

const broker = makeBroker();
const digest = (algorithm, bytes) => new Uint8Array(crypto.createHash(algorithm.replace('-', '').toLowerCase()).update(Buffer.from(bytes)).digest()).buffer;
const context = {console, URL, URLSearchParams, TextEncoder, TextDecoder, EventTarget, Event, setTimeout, Buffer, Blob, WeakRef, FinalizationRegistry, broker, digest};
vm.createContext(context);
const run = code => vm.runInContext(code, context);
run(fs.readFileSync(web + 'js_dom_exception.js', 'utf8'));
context.host = {promiseThen: (p, f, r) => Promise.prototype.then.call(p, f, r)};
run(streamsSource());
run(`
const apply=Reflect.apply,DOMException=domExceptionBridge.DOMException;
let closed=false,address='https://cache.test/worker/w.js',workerCanCancel=true,workerStreaming=false,formDataBridge;
const fetches=new Map(),listeners=new WeakMap(),reported=[],sent=[],cancelled=[],resumed=[];
const blobBridge={brand:()=>false,bytes:()=>new ArrayBuffer(0),type:()=>'',fromBytes:(bytes,type)=>new Blob([bytes],{type})};
function report(e){reported.push(e);}
/* js_performance.js's bridge, included after Fetch: the report timings it is given. */
const timings=[],performanceBridge={monotonicResource(...a){timings.push(a);}};
function timer(fn,ms,args){setTimeout(()=>fn(...(args||[])),0);return 1;}
function clearTimer(){}
function dispatchNativeWorkerEvent(target,event){return EventTarget.prototype.dispatchEvent.call(target,event);}
let abortHandlerTarget=()=>false;
function installHandlers(prototype,types){for(const type of types)Object.defineProperty(prototype,'on'+type,{configurable:true,enumerable:true,writable:true,value:null});}
Object.assign(host,{now:()=>Date.now(),securityURL:()=>address,referrerPolicy:()=>7,cryptoDigest:digest,
    dataURL:url=>[new TextEncoder().encode(decodeURIComponent(url.slice(url.indexOf(',')+1))).buffer,'text/plain'],
    request(q,stream){sent.push({...q,stream,proto:Object.getPrototypeOf(q)});return sent.length;},resume(id){resumed.push(id);},cancel(id){cancelled.push(id);}});
`);
run(fetchPart.replace('/* @include js_abort.js */', fs.readFileSync(web + 'js_abort.js', 'utf8'))
    .replace('/* @include js_fetch.js */', fs.readFileSync(web + 'js_fetch.js', 'utf8')));
run(fs.readFileSync(web + 'js_cache_storage.js', 'utf8') +
    `\nconst policy={value:0};installCacheStorage({...fetchBridge.cache,secure:true,native:broker.native,embedderPolicy:()=>policy.value,streams:streamBridge});`);
const caches = () => run("Object.getOwnPropertyDescriptor(globalThis,'caches').get.call(globalThis)");
const tick = () => new Promise(ok => setTimeout(ok, 0));
const transport = run('workerFetchTransport'), sent = run('sent'), cancelled = run('cancelled'), resumed = run('resumed');
const last = () => sent[sent.length - 1];
const bytesOf = text => new TextEncoder().encode(text).buffer;
/* The parent's NJW_LOADED for fetch id: status, headers text, body; flags are NJW_LOADED_*;
   timing its NJW_LOADED_TIMING field (7), absent when undefined. */
function respond(id, status, url, headers, body, flags = 0, error = '', timing = undefined) {
    const r = [status, url, 'HTTP/1.1 ' + status + ' X\r\n' + headers, body === null ? new ArrayBuffer(0) : bytesOf(body), error];
    if (timing !== undefined) r[7] = timing;
    transport.loaded(id, r, flags);
}
/* The parent's NJW_FRAME events: 1 headers, 2 chunk, 3 end, 4 upload (NJW_FRAME_SHIFT 8). */
const frame = (id, event, value, flags = 0) => transport.frame(id, value, (event << 8) | flags);
/* An END frame: [error, the report timing or null]. */
const end = (id, error = '', timing = null) => frame(id, 3, [error, timing]);
const count = (list, id) => list.filter(x => x === id).length;

async function test() {
    const R = run;
    /* The Request constructor's steps. */
    check('url against the worker', R("new Request('a').url") === 'https://cache.test/worker/a');
    check('defaults', R("(r=>r.method==='GET'&&r.mode==='cors'&&r.credentials==='same-origin'&&r.cache==='default'&&r.redirect==='follow'&&r.referrer==='about:client'&&r.referrerPolicy===''&&r.integrity===''&&r.keepalive===false&&r.destination===''&&r.duplex==='half')(new Request('a'))"));
    check('method normalized', R("new Request('a',{method:'post'}).method==='POST'&&new Request('a',{method:'patch'}).method==='patch'"));
    const throwsType = code => R(`(()=>{try{${code};return false;}catch(e){return e instanceof TypeError;}})()`);
    check('forbidden method', throwsType("new Request('a',{method:'CONNECT'})"));
    check('method token', throwsType("new Request('a',{method:'a b'})"));
    check('url credentials', throwsType("new Request('https://u:p@cache.test/')"));
    check('navigate mode', throwsType("new Request('a',{mode:'navigate'})"));
    check('no-cors method', throwsType("new Request('a',{mode:'no-cors',method:'PUT'})"));
    check('GET body', throwsType("new Request('a',{body:'x'})"));
    check('only-if-cached', throwsType("new Request('a',{cache:'only-if-cached'})"));
    check('bad enum', throwsType("new Request('a',{credentials:'x'})") && throwsType("new Request('a',{duplex:'full'})"));
    check('init not an object', throwsType("new Request('a',5)"));
    check('window must be null', throwsType("new Request('a',{window:{}})") && R("new Request('a',{window:null}).url!==''"));
    check('stream body needs duplex', throwsType("new Request('a',{method:'POST',body:new ReadableStream()})") &&
        R("new Request('a',{method:'POST',body:new ReadableStream(),duplex:'half'}).duplex==='half'"));
    check('stream body and keepalive', throwsType("new Request('a',{method:'POST',body:new ReadableStream(),duplex:'half',keepalive:true})"));
    check('request guard', R("(r=>!r.headers.has('cookie')&&r.headers.get('x-a')==='1')(new Request('a',{headers:{cookie:'c','x-a':'1'}}))"));
    check('no-cors guard', R("(r=>!r.headers.has('x-a')&&r.headers.get('accept')==='a/b')(new Request('a',{mode:'no-cors',headers:{'x-a':'1',accept:'a/b'}}))"));
    check('referrer', R("new Request('a',{referrer:''}).referrer===''&&new Request('a',{referrer:'/r'}).referrer==='https://cache.test/r'&&new Request('a',{referrer:'https://other.test/'}).referrer==='about:client'"));
    check('init resets referrer', R("new Request(new Request('a',{referrer:'/r'}),{method:'GET'}).referrer==='about:client'&&new Request(new Request('a',{referrer:'/r'})).referrer==='https://cache.test/r'"));
    check('dictionary order', R("(()=>{const seen=[];const init=new Proxy({},{get(t,k){if(typeof k==='string')seen.push(k);return undefined;}});new Request('a',init);return seen.join();})()") ===
        'body,cache,credentials,duplex,headers,integrity,keepalive,method,mode,priority,redirect,referrer,referrerPolicy,signal,window');
    check('body text', await R("(async()=>{const r=new Request('a',{method:'POST',body:'hello'});const t=await r.text();return t==='hello'&&r.bodyUsed&&r.headers.get('content-type')==='text/plain;charset=UTF-8';})()"));
    check('clone', await R("(async()=>{const r=new Request('a',{method:'POST',body:'x'}),c=r.clone();return (await r.text())==='x'&&(await c.text())==='x'&&c.url===r.url&&c.signal!==r.signal;})()"));
    check('clone used', await R("(async()=>{const r=new Request('a',{method:'POST',body:'x'});await r.text();try{r.clone();return false;}catch(e){return e instanceof TypeError;}})()"));
    check('body moves to the new request', await R("(async()=>{const a=new Request('a',{method:'POST',body:'x'}),b=new Request(a);return a.bodyUsed&&!b.bodyUsed&&(await b.text())==='x';})()"));
    check('used input', await R("(async()=>{const a=new Request('a',{method:'POST',body:'x'});await a.text();try{new Request(a);return false;}catch(e){return e instanceof TypeError;}})()"));
    check('signal follows', R("(()=>{const c=new AbortController(),r=new Request('a',{signal:c.signal}),s=new Request(r);c.abort('why');return r.signal.aborted&&s.signal.aborted&&s.signal.reason==='why';})()"));
    check('null signal', R("(()=>{const c=new AbortController(),r=new Request(new Request('a',{signal:c.signal}),{signal:null});c.abort();return !r.signal.aborted;})()"));
    check('interface', R("typeof Request.prototype.arrayBuffer==='function'&&Object.getOwnPropertyDescriptor(Request.prototype,'url').enumerable&&Object.prototype.toString.call(new Request('a'))==='[object Request]'"));
    check('illegal receiver', throwsType("Object.getOwnPropertyDescriptor(Request.prototype,'url').get.call({})"));
    check('headers iterator', R("(()=>{const h=new Headers({b:'2',a:'1'}),it=h.entries();return Headers.prototype[Symbol.iterator]===Headers.prototype.entries&&Object.prototype.toString.call(it)==='[object Headers Iterator]'&&Object.getPrototypeOf(Object.getPrototypeOf(it))===Object.getPrototypeOf(Object.getPrototypeOf([][Symbol.iterator]()))&&[...h.keys()].join()==='a,b';})()"));

    /* Response's statics. */
    check('error', R("(r=>r.type==='error'&&r.status===0&&r.body===null)(Response.error())"));
    check('redirect', R("(r=>r.status===301&&r.headers.get('location')==='https://cache.test/x'&&r.body===null)(Response.redirect('/x',301))"));
    check('redirect status', R("(()=>{try{Response.redirect('/x',200);return false;}catch(e){return e instanceof RangeError;}})()"));
    check('json', await R("(async()=>{const r=Response.json({a:1},{status:201});return r.status===201&&r.headers.get('content-type')==='application/json'&&(await r.text())==='{\"a\":1}';})()"));
    check('json unserializable', throwsType("Response.json(undefined)"));
    check('json null-body status', throwsType("Response.json(1,{status:204})"));

    /* fetch: the Request constructor's steps, then the transport query the parent receives. */
    let p = R("fetch(new Request('f',{method:'POST',body:'up',headers:{'x-a':'1'}}))");
    check('fetch sent', sent.length === 1 && sent[0].url === 'https://cache.test/worker/f' && sent[0].method === 'POST' &&
        sent[0].headers.includes('x-a: 1\r\n') && new TextDecoder().decode(sent[0].body) === 'up' &&
        sent[0].referrer === 'https://cache.test/worker/w.js' && sent[0].referrerPolicy === 7 && sent[0].stream === false);
    check('fetch default modes', sent[0].credentials === 1 && sent[0].cache === 0 && !sent[0].sameOrigin && !sent[0].noCors &&
        !sent[0].redirectError && !sent[0].keepalive && !sent[0].forcePreflight && !sent[0].noReferrer && sent[0].destination === '');
    check('query has no prototype', sent[0].proto === null);
    respond(1, 200, 'https://cache.test/worker/f', 'content-type: text/plain', 'down');
    const fr = await p;
    check('fetch resolved', fr.status === 200 && fr.type === 'basic' && fr.url === 'https://cache.test/worker/f' && (await fr.text()) === 'down');
    check('fetch input used', await R("(async()=>{const r=new Request('g',{method:'POST',body:'b'});fetch(r).catch(()=>{});return r.bodyUsed;})()"));
    check('fetch aborted', await rejects(R("fetch('h',{signal:AbortSignal.abort()})"), 'AbortError') && sent.length === 2);
    check('fetch requires input', await rejects(R("fetch()"), 'TypeError'));
    R("fetch('m',{credentials:'omit',mode:'same-origin',redirect:'error',cache:'no-store',keepalive:true,referrerPolicy:'origin',priority:'high'}).catch(()=>{})");
    check('modes to the parent', last().url === 'https://cache.test/worker/m' && last().credentials === 0 && last().sameOrigin === true &&
        last().redirectError === true && last().cache === 1 && last().keepalive === true && last().referrerPolicy === 4);
    R("fetch('n',{credentials:'include',cache:'reload',referrer:''}).catch(()=>{})");
    check('include, no referrer', last().credentials === 2 && last().cache === 2 && last().noReferrer === true && last().referrer === '');
    R("fetch('https://other.test/x',{mode:'no-cors',referrer:'/page#f'}).catch(()=>{})");
    check('no-cors query', last().noCors === true && last().referrer === 'https://cache.test/page');
    check('manual redirect', await rejects(R("fetch('r',{redirect:'manual'})"), 'NotSupportedError'));
    let before = sent.length;
    check('stream upload', await rejects(R("fetch('u',{method:'POST',body:new ReadableStream(),duplex:'half'})"), 'TypeError') && sent.length === before);
    check('data: stays in the realm', (await (await R("fetch('data:,hello%20there')")).text()) === 'hello there' && sent.length === before);
    p = R("fetch('loaded-error')");
    respond(sent.length, 0, '', '', null, 0, 'net::refused');
    check('network error', await rejects(p, 'TypeError'));
    p = R("fetch('cors-redirected')");
    respond(sent.length, 200, 'https://other.test/final', '', 'x', 1 | 2);
    const cr = await p;
    check('cors redirected response', cr.type === 'cors' && cr.redirected === true && cr.url === 'https://other.test/final');

    /* Opaque responses: blank from the parent, never contents in this realm. */
    p = R("fetch('https://other.test/o',{mode:'no-cors'})");
    respond(sent.length, 0, '', '', null, 4 | (5 << 3));
    const opaque = await p;
    check('opaque', opaque.type === 'opaque' && opaque.status === 0 && opaque.url === '' && [...opaque.headers].length === 0 &&
        opaque.body === null && R("x=>JSON.parse(fetchBridge.cache.response(x).extra).c")(opaque) === 5);
    p = R("fetch('https://other.test/c')");
    respond(sent.length, 0, '', '', null, 4);
    check('opaque for a cors request', await rejects(p, 'TypeError'));

    /* Integrity: the bytes are checked before the Response is published. */
    const good = 'sha256-' + crypto.createHash('sha256').update('down').digest('base64');
    p = R(`fetch('sri',{integrity:'${good}'})`);
    respond(sent.length, 200, 'https://cache.test/worker/sri', '', 'down');
    check('integrity match', (await (await p).text()) === 'down');
    p = R(`fetch('sri',{integrity:'${good}'})`);
    respond(sent.length, 200, 'https://cache.test/worker/sri', '', 'other');
    check('integrity mismatch', await rejects(p, 'TypeError'));
    p = R(`fetch('https://other.test/sri',{mode:'no-cors',integrity:'${good}'})`);
    respond(sent.length, 0, '', '', null, 4);
    check('integrity of opaque', await rejects(p, 'TypeError'));

    const c = R('new AbortController()');
    p = R('s=>fetch("i",{signal:s})')(c.signal);
    c.abort();
    check('fetch cancelled', await rejects(p, 'AbortError') && cancelled.includes(sent.length));

    /* Streamed responses: NJW_FRAME, one resume per frame taken. */
    R('workerStreaming=true');
    p = R("fetch('s')");
    let id = sent.length;
    check('stream asked', last().stream === true);
    frame(id, 1, [200, 'https://cache.test/worker/s', 'HTTP/1.1 200 OK\r\ncontent-type: text/plain\r\n']);
    const sr = await p;
    check('headers frame publishes', sr.status === 200 && sr.headers.get('content-type') === 'text/plain' && sr.body !== null && count(resumed, id) === 1);
    transport.resume(id);
    check('resume coalesced', count(resumed, id) === 1);
    frame(id, 2, bytesOf('ab'));
    check('chunk asks for the next', count(resumed, id) === 2);
    frame(id, 2, bytesOf('cd'));
    end(id, '');
    check('stream body', (await sr.text()) === 'abcd' && count(resumed, id) === 3);
    check('ended fetch forgotten', !R('fetches').has(id));

    p = R("fetch('big')");
    id = sent.length;
    frame(id, 1, [200, 'https://cache.test/worker/big', 'HTTP/1.1 200 OK\r\n']);
    const big = await p, reader = big.body.getReader();
    const resumedBefore = count(resumed, id);
    frame(id, 2, new ArrayBuffer((1 << 20) + 1));
    check('full queue pauses', count(resumed, id) === resumedBefore);
    transport.resume(id); transport.resume(id);
    check('paused resume once', count(resumed, id) === resumedBefore + 1);
    const piece = await reader.read();
    check('read the chunk', piece.value.byteLength === (1 << 20) + 1);
    end(id, 'connection reset');
    check('error after headers', await rejects(reader.read(), 'TypeError'));

    p = R("fetch('before')");
    end(sent.length, 'refused');
    check('error before headers', await R("p=>p.then(()=>false,e=>e instanceof TypeError&&e.message==='refused')")(p));

    p = R("fetch('https://other.test/so',{mode:'no-cors'})");
    id = sent.length;
    frame(id, 1, [0, '', ''], 4 | (2 << 3));
    const so = await p;
    end(id, '');
    check('opaque headers frame', so.type === 'opaque' && so.body === null && so.status === 0 && [...so.headers].length === 0);
    p = R("fetch('https://other.test/sc')");
    frame(sent.length, 1, [0, '', ''], 4);
    check('opaque frame for a cors request', await rejects(p, 'TypeError') && !R('fetches').has(sent.length));

    p = R("fetch('up',{method:'POST',body:'x'})");
    id = sent.length;
    frame(id, 4, 1);
    check('upload frame resumes', count(resumed, id) === 1);
    frame(id, 1, [204, 'https://cache.test/worker/up', 'HTTP/1.1 204 No Content\r\n']);
    check('null body status', (await p).body === null);
    end(id, '');

    const sc = R('new AbortController()');
    p = R('s=>fetch("sa",{signal:s})')(sc.signal);
    id = sent.length;
    frame(id, 1, [200, 'https://cache.test/worker/sa', 'HTTP/1.1 200 OK\r\n']);
    const sa = await p;
    sc.abort();
    check('abort a stream', cancelled.includes(id) && await rejects(sa.text(), 'AbortError'));
    frame(id, 2, bytesOf('late'));
    check('late frame ignored', count(resumed, id) === 1);

    p = R("fetch('closing')");
    check('fetches the parent has not ended', R('fetches.size') === 5);
    transport.close();
    check('close rejects', await rejects(p, 'AbortError') && R('fetches.size') === 0);
    R('workerStreaming=false');

    /* Cache Storage over this realm's records. */
    const storage = caches();
    const cache = await storage.open('w');
    await cache.put(R("new Request('k',{headers:{'x-shape':'circle'}})"), R("new Response('stored',{headers:{'content-type':'text/plain',vary:'x-shape'}})"));
    const keys = await cache.keys();
    check('keys are Requests', keys.length === 1 && R("x=>x instanceof Request")(keys[0]) && keys[0].url === 'https://cache.test/worker/k' &&
        keys[0].headers.get('x-shape') === 'circle' && keys[0].mode === 'cors');
    check('keys headers immutable', R("(k=>{try{k.headers.set('x','y');return false;}catch(e){return e instanceof TypeError;}})")(keys[0]));
    const m = await cache.match('k');
    check('match is a Response', R("x=>x instanceof Response")(m) && m.status === 200 && m.headers.get('vary') === 'x-shape' && (await m.text()) === 'stored');
    check('put a used body', await rejects(R("(async c=>{const r=new Response('u');await r.text();return c.put('u',r);})")(cache), 'TypeError'));
    check('put 206', await rejects(cache.put('p', R("new Response('x',{status:206})")), 'TypeError'));
    check('put Response.error', (await cache.put('e', R('Response.error()')), (await cache.match('e')).type === 'error'));
    before = sent.length;
    const added = cache.addAll(['one', R("new Request('two')")]);
    check('addAll fetched', sent.length === before + 2 && sent[before].url === 'https://cache.test/worker/one');
    respond(before + 1, 200, 'https://cache.test/worker/one', 'content-type: text/plain', 'first');
    respond(before + 2, 200, 'https://cache.test/worker/two', 'content-type: text/plain', 'second');
    await added;
    check('addAll stored', (await (await cache.match('two')).text()) === 'second' && (await cache.keys()).length === 4);
    const failing = cache.addAll(['three', 'four']);
    const base = sent.length - 2;
    respond(base + 1, 404, 'https://cache.test/worker/three', '', 'no');
    check('addAll rejects', await rejects(failing, 'TypeError'));
    check('addAll cancelled the other fetch', cancelled.includes(base + 2));
    check('addAll stored nothing', (await cache.keys()).length === 4);
    const opaqueCached = R("fetch('https://other.test/p',{mode:'no-cors'})");
    respond(sent.length, 0, '', '', null, 4 | (2 << 3));
    await cache.put('https://other.test/p', await opaqueCached);
    run('policy').value = 1;
    check('opaque blocked by require-corp', await rejects(cache.match('https://other.test/p'), 'TypeError'));
    run('policy').value = 0;
    check('opaque under unsafe-none', (await cache.match('https://other.test/p')).type === 'opaque');
    /* Fetch's report timing of this Worker's own fetches, from the parent's records (monotonic times
       js_performance.js's monotonicResource converts): after the fetch ended, with the request's URL
       and fetch()'s request kind (js.c P_FETCH). */
    const timings = run('timings');
    timings.length = 0;
    const record = {startTime: 1000, responseEnd: 1010};
    p = R("fetch('timed')");
    respond(sent.length, 200, 'https://cache.test/worker/timed', '', 'ok', 0, '', record);
    await p;
    check('loaded reports timing', timings.length === 1 && timings[0][1] === 1 && timings[0][2] === 'https://cache.test/worker/timed' && timings[0][3] === record);
    p = R("fetch('failed')");
    respond(sent.length, 0, '', '', null, 0, 'refused', record);
    check('a failed fetch reports timing', await rejects(p, 'TypeError') && timings.length === 2 && timings[1][2] === 'https://cache.test/worker/failed');
    R("Object.defineProperty(Array.prototype,7,{configurable:true,get(){return {startTime:1};}})");
    p = R("fetch('untimed')");
    respond(sent.length, 200, 'https://cache.test/worker/untimed', '', 'ok');
    await p;
    R("delete Array.prototype[7]");
    check("the page's Array.prototype is no timing", timings.length === 3 && timings[2][3] === undefined);
    R('workerStreaming=true');
    p = R("fetch('streamed')");
    id = sent.length;
    frame(id, 1, [200, 'https://cache.test/worker/streamed', 'HTTP/1.1 200 OK\r\n']);
    const streamed = await p;
    check('no timing before the end', timings.length === 3);
    end(id, '', record);
    check('the end reports timing', (await streamed.text()) === '' && timings.length === 4 && timings[3][2] === 'https://cache.test/worker/streamed' && timings[3][3] === record);
    R('workerStreaming=false');
    check('no reports', R('reported.length') === 0);
    await tick();
    console.log(`worker cache storage contract: ${checks} checks passed`);
}
test().catch(e => { console.error(e); process.exit(1); });
