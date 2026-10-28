/* The production js_cache_storage.js and js_streams.js (with the pinned Streams ponyfill, as
 * js_embed.py inlines it) in a node vm realm, over a small in-memory broker and a small Fetch
 * record adapter. It verifies the binding's IDL conversions, its order of checks, how bodies are
 * staged and read in pieces, addAll's abort, the status to exception mapping and the resource
 * policy check of opaque responses; not webcache.c (check_cache_storage_native.py), not
 * js_fetch.js's or the Worker's Fetch records, not a guest browser.
 * Run: node tests/cache_storage_contract.cjs (from the repository root). */
'use strict';
const fs = require('node:fs'), vm = require('node:vm'), assert = require('node:assert/strict');
const web = 'user/libc/web/';
let checks = 0;
const check = (name, yes) => { checks++; assert(yes, name); };

function streamsSource() {
    const manifest = JSON.parse(fs.readFileSync('third_party/web_streams/upstream.json', 'utf8'));
    const ponyfill = fs.readFileSync('third_party/web_streams/ponyfill.mjs', 'utf8');
    assert(manifest.files['ponyfill.mjs']);
    const module = ponyfill.replace(/export\{([A-Za-z,]+)\};\s*$/, 'return {$1};');
    assert(module !== ponyfill);
    return fs.readFileSync(web + 'js_streams.js', 'utf8').replace('/* @streams-ponyfill */', module);
}

/* The broker: names, lists, staged and opened bodies, and what the binding asked of it. */
function makeBroker() {
    const b = {names: [], lists: new Map(), stages: new Map(), bodies: new Map(), serial: 0, log: [], writes: [], reads: [],
        discarded: [], closed: [], fail: new Map()};
    const key = (url, flags) => { let u = url.split('#')[0]; if (flags & 1) u = u.split('?')[0]; return u; };
    const done = v => Promise.resolve(v);
    const open = item => {
        if (!item.body) return 0;
        const t = ++b.serial;
        b.bodies.set(t, {bytes: item.body, at: 0});
        return t;
    };
    b.native = function (op, ...args) {
        b.log.push(op);
        if (b.fail.has(op)) { const status = b.fail.get(op); b.fail.delete(op); return done([status]); }
        switch (op) {
        case 'open': {
            let e = b.names.find(n => n.name === args[0]);
            if (!e) { e = {name: args[0], id: (++b.serial).toString(16).padStart(32, '0')}; b.names.push(e); b.lists.set(e.id, []); }
            return done([0, e.id]);
        }
        case 'has': return done([0, b.names.some(n => n.name === args[0])]);
        case 'delete': {
            const i = b.names.findIndex(n => n.name === args[0]);
            if (i >= 0) b.names.splice(i, 1);
            return done([0, i >= 0]);
        }
        case 'names': return done([0, b.names.map(n => n.name)]);
        case 'stage': { const t = ++b.serial; b.stages.set(t, []); return done([0, t]); }
        case 'write': {
            assert(args[1] instanceof ArrayBuffer || Object.prototype.toString.call(args[1]) === '[object ArrayBuffer]');
            b.writes.push(args[1].byteLength);
            b.stages.get(args[0]).push(Buffer.from(args[1]));
            return done([0]);
        }
        case 'discard': b.discarded.push(args[0]); b.stages.delete(args[0]); return done([0]);
        case 'close': b.closed.push(args[0]); b.bodies.delete(args[0]); return done([0]);
        case 'read': {
            const body = b.bodies.get(args[0]);
            if (!body) return done([6]);
            const n = Math.min(args[1], body.bytes.length - body.at);
            b.reads.push(n);
            if (!n) { b.bodies.delete(args[0]); return done([0, null]); }
            const out = Uint8Array.prototype.slice.call(body.bytes, body.at, body.at + n).buffer;
            body.at += n;
            return done([0, out]);
        }
        case 'batch': {
            const [id, ops] = args, list = b.lists.get(id);
            const added = [];
            for (const o of ops) {
                const k = key(o[2], o[1]);
                if (added.includes(k)) { for (const p of ops) if (p[8]) b.stages.delete(p[8]); return done([7]); }
                if (o[0] === 1) added.push(k);
            }
            let deleted = 0;
            for (const o of ops) {
                const k = key(o[2], o[1]);
                for (let i = list.length - 1; i >= 0; i--) if (key(list[i].url, o[1]) === k) { list.splice(i, 1); if (o[0] === 2) deleted++; }
                if (o[0] !== 1) continue;
                const body = o[8] ? Buffer.concat(b.stages.get(o[8])) : null;
                b.stages.delete(o[8]);
                list.push({url: o[2], method: o[3], headers: o[4], extra: o[5], responseHeaders: o[6], responseExtra: o[7], body});
            }
            return done([0, deleted]);
        }
        case 'match': case 'keys': {
            const [id, flags] = args, query = op === 'match' ? args[3] : args[2], name = op === 'match' ? args[2] : undefined;
            const lists = id !== null ? [b.lists.get(id) || []] :
                b.names.filter(n => name === undefined || n.name === name).map(n => b.lists.get(n.id));
            const out = [];
            for (const l of lists) {
                for (const item of l) {
                    if (query && key(item.url, flags) !== key(query[0], flags)) continue;
                    out.push(op === 'keys' ? [item.url, item.method, item.headers, item.extra] :
                        [item.responseHeaders, item.responseExtra, open(item), item.body ? item.body.length : 0]);
                    if (flags & 64) break;
                }
                if (out.length && (flags & 64)) break;
            }
            return done([0, out]);
        }
        }
        throw new TypeError('unknown operation ' + op);
    };
    return b;
}

/* The realm's Fetch records, as js_fetch.js's cache bridge gives them. */
const fetchSource = `
const fakeFetch = (() => {
    const requests = new WeakMap(), responses = new WeakMap(), fetched = [];
    let respond = null;
    const flat = h => Object.entries(h || {}).flat();
    class Request {
        constructor(input, init = {}) {
            const old = requests.get(input);
            const s = old ? {...old, headers: old.headers.slice()} : {url: new URL(String(input), 'https://cache.test/dir/').href, method: 'GET', headers: [], mode: 'cors'};
            if (init.method !== undefined) s.method = init.method;
            if (init.headers !== undefined) s.headers = flat(init.headers);
            if (init.mode !== undefined) s.mode = init.mode;
            requests.set(this, s);
        }
        get url() { return requests.get(this).url; }
        get method() { return requests.get(this).method; }
        get headers() { return requests.get(this).headers; }
        get mode() { return requests.get(this).mode; }
    }
    class Response {
        constructor(text, init = {}) {
            const bytes = text === null ? null : typeof text === 'string' ? new TextEncoder().encode(text).buffer : text;
            responses.set(this, {status: init.status ?? 200, type: init.type ?? 'basic', headers: flat(init.headers), url: init.url ?? '',
                resource: init.resource, stream: bytes === null ? null : streamBridge.fromBytes(bytes)});
        }
        get status() { return responses.get(this).status; }
        get type() { return responses.get(this).type; }
        get url() { return responses.get(this).url; }
        get headers() { return responses.get(this).headers; }
        get body() { return responses.get(this).stream; }
        get bodyUsed() { const s = responses.get(this); return !!s.stream && streamBridge.disturbed(s.stream); }
        async text() { const s = responses.get(this); return s.stream ? new TextDecoder().decode(await streamBridge.collect(s.stream)) : ''; }
    }
    const realm = {
        isRequest: v => requests.has(v), isResponse: v => responses.has(v),
        newRequest: input => new Request(input),
        request(r) { const s = requests.get(r); return {url: s.url, method: s.method, headers: s.headers.slice(), extra: JSON.stringify({m: s.mode})}; },
        makeRequest(url, method, headers, x) { const r = Object.create(Request.prototype); requests.set(r, {url, method, headers, mode: x.m}); return r; },
        response(r) {
            const s = responses.get(r);
            return {type: s.type, status: s.status, headers: s.headers.slice(), unusable: !!s.stream && (streamBridge.locked(s.stream) || streamBridge.disturbed(s.stream)),
                extra: JSON.stringify({t: s.type, s: s.status, u: s.url, c: s.resource})};
        },
        takeBody: r => responses.get(r).stream,
        makeResponse(x, headers, stream) { const r = Object.create(Response.prototype); responses.set(r, {status: x.s, type: x.t, headers, url: x.u, resource: x.c, stream}); return r; },
        fetch(r, signal) { fetched.push(requests.get(r).url); return respond(requests.get(r), signal); },
        controller() { const c = new AbortController(); return {signal: c.signal, abort(reason) { c.abort(reason); }}; }
    };
    return {Request, Response, realm, fetched, respond(f) { respond = f; }};
})();`;

function makeRealm(secure, policy) {
    const broker = makeBroker();
    const context = {console, URL, TextEncoder, TextDecoder, AbortController, Buffer,
        host: {promiseThen: (p, f, r) => Promise.prototype.then.call(p, f, r)}};
    vm.createContext(context);
    vm.runInContext(fs.readFileSync(web + 'js_dom_exception.js', 'utf8'), context);
    vm.runInContext(streamsSource(), context);
    vm.runInContext(fetchSource, context);
    context.policy = policy;
    context.broker = broker;
    vm.runInContext(fs.readFileSync(web + 'js_cache_storage.js', 'utf8') +
        `\ninstallCacheStorage({...fakeFetch.realm, secure: ${secure}, native: broker.native, embedderPolicy: () => policy.value, streams: streamBridge});`, context);
    const run = code => vm.runInContext(code, context);
    return {context, broker, run, caches: () => run("Object.getOwnPropertyDescriptor(globalThis,'caches').get.call(globalThis)")};
}

async function rejects(promise, name) {
    try { await promise; } catch (e) { return e && e.name === name; }
    return false;
}

async function test() {
    /* [SecureContext]: nothing is defined elsewhere. */
    const insecure = makeRealm(false, {value: 0});
    check('insecure has no caches', insecure.run("!('caches' in globalThis) && typeof CacheStorage === 'undefined' && typeof Cache === 'undefined'"));

    const policy = {value: 0};
    const {broker, run, caches} = makeRealm(true, policy);
    const R = run;
    check('caches is the same object', caches() === caches() && R("Object.prototype.toString.call(Object.getOwnPropertyDescriptor(globalThis,'caches').get.call(globalThis))") === '[object CacheStorage]');
    check('constructors are illegal', R("(()=>{try{new Cache();return false;}catch(e){return e instanceof TypeError;}})()") &&
        R("(()=>{try{new CacheStorage();return false;}catch(e){return e instanceof TypeError;}})()"));
    check('caches accessor shape', R("(()=>{const d=Object.getOwnPropertyDescriptor(globalThis,'caches');return d.enumerable&&d.configurable&&d.set===undefined&&d.get.name==='get caches';})()"));
    check('wrong receiver rejects', await rejects(R("CacheStorage.prototype.keys.call({})"), 'TypeError'));

    /* CacheStorage: open, has, keys, delete; a name is a DOMString. */
    const storage = caches();
    const v1 = await storage.open('v1');
    check('open gives a Cache', R("x=>x instanceof Cache")(v1) && broker.names.length === 1);
    check('open the same name', (await storage.open('v1')) !== v1 && broker.names.length === 1);
    await storage.open(123);
    check('keys in order', JSON.stringify(await storage.keys()) === '["v1","123"]');
    check('has', (await storage.has('123')) === true && (await storage.has('none')) === false);
    check('delete', (await storage.delete('123')) === true && (await storage.delete('123')) === false);
    check('open requires a name', await rejects(storage.open(), 'TypeError'));

    /* put: the checks before any staging, then the body in pieces of at least 256 KiB. */
    const {Request, Response} = R('fakeFetch');
    const before = broker.log.length;
    check('put 206', await rejects(v1.put('a', new Response('x', {status: 206})), 'TypeError'));
    check('put vary star', await rejects(v1.put('a', new Response('x', {headers: {Vary: 'Accept, *'}})), 'TypeError'));
    check('put POST', await rejects(v1.put(new Request('a', {method: 'POST'}), new Response('x')), 'TypeError'));
    check('put non-http', await rejects(v1.put('data:text/plain,x', new Response('x')), 'TypeError'));
    check('put no response', await rejects(v1.put('a', {}), 'TypeError'));
    const used = new Response('used');
    await used.text();
    check('put used body', await rejects(v1.put('a', used), 'TypeError'));
    check('refused puts staged nothing', broker.log.length === before);
    const big = new Uint8Array(600000);
    for (let i = 0; i < big.length; i++) big[i] = i & 255;
    const bigResponse = new Response(big.buffer, {headers: {'Content-Type': 'application/octet-stream'}});
    await v1.put('big', bigResponse);
    check('put streamed in pieces', JSON.stringify(broker.writes) === '[262144,262144,75712]');
    check('put uses the body', bigResponse.bodyUsed === true);
    check('put stored', broker.lists.get(broker.names[0].id).length === 1 && broker.lists.get(broker.names[0].id)[0].url === 'https://cache.test/dir/big');
    await v1.put(new Request('empty'), new Response(null));
    check('null body stages nothing', broker.log.filter(o => o === 'stage').length === 1);

    /* match: the stored body, read from the broker as it is read. */
    const readAll = R("async r=>{const reader=r.body.getReader();const parts=[];for(;;){const x=await reader.read();if(x.done)break;parts.push(x.value);}let n=0;for(const p of parts)n+=p.length;const out=new Uint8Array(n);let at=0;for(const p of parts){out.set(p,at);at+=p.length;}return out.buffer;}");
    const got = await v1.match('big');
    check('match gives a Response', R("x=>x instanceof fakeFetch.Response")(got) && got.status === 200);
    const first = new Uint8Array(await readAll(got));
    check('match body read in pieces', first.length === 600000 && JSON.stringify(broker.reads) === '[262144,262144,75712,0]');
    const bytes = new Uint8Array(await readAll(await v1.match(new Request('big#fragment'))));
    check('match body bytes', bytes.length === 600000 && bytes[599999] === (599999 & 255) && bytes[1000] === (1000 & 255));
    check('match none', (await v1.match('missing')) === undefined);
    const logLength = broker.log.length;
    check('match POST without ignoreMethod', (await v1.match(new Request('big', {method: 'POST'}))) === undefined && broker.log.length === logLength);
    check('match POST with ignoreMethod', (await v1.match(new Request('big', {method: 'POST'}), {ignoreMethod: true})) !== undefined);
    const cancelled = await v1.match('big');
    const token = [...broker.bodies.keys()].pop();
    await cancelled.body.cancel();
    check('cancel closes the body', broker.closed.includes(token) && !broker.bodies.has(token));
    check('match requires a request', await rejects(v1.match(), 'TypeError'));
    check('options must be a dictionary', await rejects(v1.match('big', 5), 'TypeError'));

    /* matchAll and keys: frozen sequences; keys give Requests. */
    const all = await v1.matchAll();
    check('matchAll', Object.isFrozen(all) && all.length === 2);
    for (const r of all) if (r.body) await r.body.cancel();
    const keys = await v1.keys();
    check('keys', Object.isFrozen(keys) && keys.length === 2 && R("x=>x instanceof fakeFetch.Request")(keys[0]) && keys[0].url === 'https://cache.test/dir/big');
    check('keys with a query', (await v1.keys('empty')).length === 1 && (await v1.keys(new Request('big', {method: 'HEAD'}))).length === 0);

    /* delete. */
    check('delete entry', (await v1.delete('empty')) === true && (await v1.delete('empty')) === false);
    check('delete HEAD without ignoreMethod', (await v1.delete(new Request('big', {method: 'HEAD'}))) === false);

    /* addAll: every fetch, then one batch; one failure aborts the others and discards their bodies. */
    const {fetched, respond} = R('({fetched:fakeFetch.fetched,respond:fakeFetch.respond})');
    const signals = [];
    respond((r, signal) => { signals.push(signal); return Promise.resolve(new Response('body of ' + r.url)); });
    await v1.addAll(['one', new Request('two')]);
    check('addAll fetched both', fetched.join() === 'https://cache.test/dir/one,https://cache.test/dir/two');
    check('addAll stored both', (await v1.keys()).length === 3 && (await (await v1.match('two')).text()) === 'body of https://cache.test/dir/two');
    signals.length = 0;
    const discardedBefore = broker.discarded.length;
    respond((r, signal) => {
        signals.push(signal);
        if (r.url.endsWith('/bad')) return new Promise(ok => setTimeout(() => ok(new Response('no', {status: 404})), 5));
        return Promise.resolve(new Response('fine'));
    });
    check('addAll rejects a non-ok response', await rejects(v1.addAll(['fine', 'bad']), 'TypeError'));
    check('addAll aborted the others', signals.length === 2 && signals.every(s => s.aborted));
    check('addAll discarded the staged bodies', broker.discarded.length === discardedBefore + 1);
    check('addAll stored nothing', (await v1.keys()).length === 3);
    respond(() => Promise.resolve(new Response('v', {headers: {vary: '*'}})));
    check('addAll rejects vary star', await rejects(v1.addAll(['star']), 'TypeError'));
    respond(() => Promise.reject(new TypeError('network')));
    check('addAll passes on a network error', await rejects(v1.add('net'), 'TypeError'));
    check('addAll requires HTTP GET requests', await rejects(v1.addAll([new Request('p', {method: 'POST'})]), 'TypeError'));
    check('addAll requires a sequence', await rejects(v1.addAll(5), 'TypeError'));
    respond(() => Promise.resolve(new Response('dup')));
    broker.fail.set('batch', 7);
    check('duplicate requests are InvalidStateError', await rejects(v1.addAll(['d#1', 'd#2']), 'InvalidStateError'));

    /* The broker's statuses. */
    broker.fail.set('batch', 5);
    const quota = await v1.put('q', new Response('q')).catch(e => e);
    check('quota is QuotaExceededError', quota.name === 'QuotaExceededError' && R("x=>x instanceof domExceptionBridge.DOMException && x instanceof domExceptionBridge.QuotaExceededError")(quota));
    broker.fail.set('open', 2);
    check('security', await rejects(storage.open('x'), 'SecurityError'));
    broker.fail.set('names', 3);
    check('io is UnknownError', await rejects(storage.keys(), 'UnknownError'));

    /* An opaque response: Fetch's resource policy check with the realm's embedder policy now. */
    await v1.put('opaque', new Response(null, {type: 'opaque', resource: 2}));
    await v1.put('corp', new Response(null, {type: 'opaque', resource: 3}));
    await v1.put('credentialed', new Response(null, {type: 'opaque', resource: 6}));
    check('opaque under unsafe-none', (await v1.match('opaque')).type === 'opaque');
    policy.value = 1;
    check('opaque blocked under require-corp', await rejects(v1.match('opaque'), 'TypeError'));
    check('opaque with CORP under require-corp', (await v1.match('corp')).type === 'opaque');
    policy.value = 2;
    check('credentialless lets an uncredentialed one through', (await v1.match('opaque')).type === 'opaque');
    check('credentialless blocks a credentialed one', await rejects(v1.match('credentialed'), 'TypeError'));
    policy.value = 0;

    /* CacheStorage match: across the caches, or the named one; a request that is no URL. */
    const v2 = await storage.open('v2');
    await v2.put('only-in-v2', new Response('two'));
    check('storage match across caches', (await (await storage.match('only-in-v2')).text()) === 'two');
    check('storage match named', (await storage.match('only-in-v2', {cacheName: 'v1'})) === undefined &&
        (await storage.match('only-in-v2', {cacheName: 'v2'})) !== undefined);
    check('storage match bad URL with a cache', await rejects(storage.match('http://['), 'TypeError'));
    check('storage match bad URL with no such cache', (await storage.match('http://[', {cacheName: 'none'})) === undefined);
    check('storage match requires a request', await rejects(storage.match(), 'TypeError'));

    console.log(`cache storage contract: ${checks} checks passed`);
}
module.exports = {makeBroker, streamsSource, rejects};
if (require.main === module) test().catch(e => { console.error(e); process.exit(1); });
