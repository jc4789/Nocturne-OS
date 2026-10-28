/* The Worker realm's Fetch (js_worker_runtime.js: its AbortSignal, Headers, Request, Response,
 * fetch and the "loaded" completion, sliced from the production source) with js_cache_storage.js
 * and js_streams.js in a node vm realm. The native transport is a recorder (host.request,
 * host.cancel) and the storage broker cache_storage_contract.cjs's in-memory one. It verifies the
 * Request constructor's steps, the Response statics, fetch of a Request, the records Cache Storage
 * keeps of this realm's objects, and add, addAll and match through them; not the native transport,
 * not webcache.c, not a guest browser. Run: node tests/worker_cache_storage_contract.cjs */
'use strict';
const fs = require('node:fs'), vm = require('node:vm'), assert = require('node:assert/strict');
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
const bridges = slice('    const workerAbortBridge=(()=>{', '    const {Headers,Request,Response}=workerFetchBridge;\n');
const fetchFunction = slice('    function fetch(input,init=undefined){', '\n    }\n');
const loadedLine = slice('        loaded(id,r,flags=0){', '\n').trim().replace(/^loaded\(/, 'function loaded(').replace(/,$/, '');

const broker = makeBroker();
const context = {console, URL, URLSearchParams, TextEncoder, TextDecoder, EventTarget, Event, setTimeout, Buffer, broker};
vm.createContext(context);
const run = code => vm.runInContext(code, context);
run(fs.readFileSync(web + 'js_dom_exception.js', 'utf8'));
context.host = {promiseThen: (p, f, r) => Promise.prototype.then.call(p, f, r)};
run(streamsSource());
run(`
const apply=Reflect.apply,DOMException=domExceptionBridge.DOMException;
let closed=false,address='https://cache.test/worker/w.js',workerCanCancel=true,formDataBridge;
const fetches=new Map(),listeners=new WeakMap(),reported=[],sent=[],cancelled=[];
const blobBridge={brand:()=>false};
function report(e){reported.push(e);}
function timer(fn,ms,args){setTimeout(()=>fn(...(args||[])),0);return 1;}
function clearTimer(){}
function dispatchNativeWorkerEvent(target,event){return EventTarget.prototype.dispatchEvent.call(target,event);}
Object.assign(host,{now:()=>Date.now(),securityURL:()=>address,referrerPolicy:()=>7,dataURL:()=>{throw new TypeError('no data');},
    request(url,noCors,noReferrer,method,headers,bytes,referrer,policy){sent.push({url,noCors,noReferrer,method,headers,bytes,referrer,policy});return sent.length;},
    cancel(id){cancelled.push(id);}});
`);
run(bridges + fetchFunction + loadedLine);
run(fs.readFileSync(web + 'js_cache_storage.js', 'utf8') +
    `\nconst policy={value:0};installCacheStorage({...workerFetchBridge.cache,secure:true,native:broker.native,embedderPolicy:()=>policy.value,streams:streamBridge});`);
const caches = () => run("Object.getOwnPropertyDescriptor(globalThis,'caches').get.call(globalThis)");
const tick = () => new Promise(ok => setTimeout(ok, 0));
/* The native completion of fetch id: status, headers text, body. */
function respond(id, status, url, headers, body, flags = 0) {
    const bytes = body === null ? new ArrayBuffer(0) : new TextEncoder().encode(body).buffer;
    run('loaded')(id, [status, url, 'HTTP/1.1 ' + status + ' X\r\n' + headers, bytes, ''], flags);
}

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
    check('request guard', R("(r=>!r.headers.has('cookie')&&r.headers.get('x-a')==='1')(new Request('a',{headers:{cookie:'c','x-a':'1'}}))"));
    check('no-cors guard', R("(r=>!r.headers.has('x-a')&&r.headers.get('accept')==='a/b')(new Request('a',{mode:'no-cors',headers:{'x-a':'1',accept:'a/b'}}))"));
    check('referrer', R("new Request('a',{referrer:''}).referrer===''&&new Request('a',{referrer:'/r'}).referrer==='https://cache.test/r'&&new Request('a',{referrer:'https://other.test/'}).referrer==='about:client'"));
    check('init resets referrer', R("new Request(new Request('a',{referrer:'/r'}),{method:'GET'}).referrer==='about:client'&&new Request(new Request('a',{referrer:'/r'})).referrer==='https://cache.test/r'"));
    check('dictionary order', R("(()=>{const seen=[];const init=new Proxy({},{get(t,k){if(typeof k==='string')seen.push(k);return undefined;}});new Request('a',init);return seen.join()==='body,cache,credentials,duplex,headers,integrity,keepalive,method,mode,redirect,referrer,referrerPolicy,signal';})()"));
    check('body text', await R("(async()=>{const r=new Request('a',{method:'POST',body:'hello'});const t=await r.text();return t==='hello'&&r.bodyUsed&&r.headers.get('content-type')==='text/plain;charset=UTF-8';})()"));
    check('clone', await R("(async()=>{const r=new Request('a',{method:'POST',body:'x'}),c=r.clone();return (await r.text())==='x'&&(await c.text())==='x'&&c.url===r.url&&c.signal!==r.signal;})()"));
    check('clone used', await R("(async()=>{const r=new Request('a',{method:'POST',body:'x'});await r.text();try{r.clone();return false;}catch(e){return e instanceof TypeError;}})()"));
    check('body moves to the new request', await R("(async()=>{const a=new Request('a',{method:'POST',body:'x'}),b=new Request(a);return a.bodyUsed&&!b.bodyUsed&&(await b.text())==='x';})()"));
    check('used input', await R("(async()=>{const a=new Request('a',{method:'POST',body:'x'});await a.text();try{new Request(a);return false;}catch(e){return e instanceof TypeError;}})()"));
    check('signal follows', R("(()=>{const c=new AbortController(),r=new Request('a',{signal:c.signal}),s=new Request(r);c.abort('why');return r.signal.aborted&&s.signal.aborted&&s.signal.reason==='why';})()"));
    check('null signal', R("(()=>{const c=new AbortController(),r=new Request(new Request('a',{signal:c.signal}),{signal:null});c.abort();return !r.signal.aborted;})()"));
    check('interface', R("typeof Request.prototype.arrayBuffer==='function'&&Object.getOwnPropertyDescriptor(Request.prototype,'url').enumerable&&Object.prototype.toString.call(new Request('a'))!==''"));
    check('illegal receiver', throwsType("Object.getOwnPropertyDescriptor(Request.prototype,'url').get.call({})"));

    /* Response's statics. */
    check('error', R("(r=>r.type==='error'&&r.status===0&&r.body===null)(Response.error())"));
    check('redirect', R("(r=>r.status===301&&r.headers.get('location')==='https://cache.test/x'&&r.body===null)(Response.redirect('/x',301))"));
    check('redirect status', R("(()=>{try{Response.redirect('/x',200);return false;}catch(e){return e instanceof RangeError;}})()"));
    check('json', await R("(async()=>{const r=Response.json({a:1},{status:201});return r.status===201&&r.headers.get('content-type')==='application/json'&&(await r.text())==='{\"a\":1}';})()"));
    check('json unserializable', throwsType("Response.json(undefined)"));
    check('json null-body status', throwsType("Response.json(1,{status:204})"));

    /* fetch: the Request constructor's steps, then the transport's. */
    const sent = R('sent');
    let p = R("fetch(new Request('f',{method:'POST',body:'up',headers:{'x-a':'1'}}))");
    check('fetch sent', sent.length === 1 && sent[0].url === 'https://cache.test/worker/f' && sent[0].method === 'POST' &&
        sent[0].headers.includes('x-a: 1\r\n') && new TextDecoder().decode(sent[0].bytes) === 'up' && sent[0].referrer === 'https://cache.test/worker/w.js' && sent[0].policy === 7);
    respond(1, 200, 'https://cache.test/worker/f', 'content-type: text/plain', 'down');
    const fr = await p;
    check('fetch resolved', fr.status === 200 && fr.type === 'basic' && (await fr.text()) === 'down');
    check('fetch input used', await R("(async()=>{const r=new Request('g',{method:'POST',body:'b'});fetch(r);return r.bodyUsed;})()"));
    check('fetch aborted', await rejects(R("fetch('h',{signal:AbortSignal.abort()})"), 'AbortError') && sent.length === 2);
    check('fetch unsupported mode', await rejects(R("fetch('h',{credentials:'omit'})"), 'NotSupportedError'));
    check('fetch requires input', await rejects(R("fetch()"), 'TypeError'));
    p = R("fetch('https://other.test/o',{mode:'no-cors'})");
    respond(3, 0, '', '', null, 4 | (5 << 3));
    const opaque = await p;
    check('opaque', opaque.type === 'opaque' && opaque.status === 0 && R("x=>JSON.parse(workerFetchBridge.cache.response(x).extra).c")(opaque) === 5);
    const cancelled = R('cancelled');
    const c = R('new AbortController()');
    p = R('s=>fetch("i",{signal:s})')(c.signal);
    c.abort();
    check('fetch cancelled', await rejects(p, 'AbortError') && cancelled.includes(4));

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
    const before = sent.length;
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
    check('no reports', R('reported.length') === 0);
    await tick();
    console.log(`worker cache storage contract: ${checks} checks passed`);
}
test().catch(e => { console.error(e); process.exit(1); });
