/* WPT's tests of Fetch (fetch/api: Headers, Request, Response, bodies, aborting) and of DOM's
 * AbortSignal (dom/abort), the files below run by WPT's own resources/testharness.js, against the
 * production definitions in two node vm realms: a Window's (js_abort.js and js_fetch.js with
 * js_bootstrap.js's ports, as js_embed.py includes them) and a Worker's (the same files with
 * js_worker_runtime.js's ports, as js_worker_embed.py includes them). Both realms' Event and
 * EventTarget are js_worker_runtime.js's (its dispatch and event handler attributes); the
 * Window's EventTarget in js_bootstrap.js is not run here. Blob, File and FileReader are
 * js_blob.js's (its UTF-8 concatenation is node's here), and each realm's blob URL store is
 * js_object_url.js as its bootstrap installs it (a Worker's only in the REVs whose Worker has
 * one). The native transports are recorders that never answer, so a test that needs the network
 * times out in every realm, except in a file marked network:'error', where every request is a
 * network error. A Worker's request for a blob: URL is answered as js.c's worker_blob answers
 * for a creating document whose store does not hold it: a network error. The realms have node's
 * URL (its createObjectURL hidden until a realm installs its own) and TextEncoder. This is not
 * the guest browser and not wptrunner: it shows the two realms give the same results and, with
 * --base=REV, that neither realm passes fewer subtests than REV's did. The checks after the WPT
 * files are the parts of DOM's AbortSignal no WPT file tests in a realm without a garbage
 * collector to call.
 * Run: node tests/fetch_wpt_contract.cjs [--base=REV] [--wpt=DIR] [--verbose] */
'use strict';
const fs = require('node:fs'), vm = require('node:vm'), path = require('node:path'), cp = require('node:child_process');
const crypto = require('node:crypto'), assert = require('node:assert/strict');
/* The garbage collection checks call V8's collector. */
if (typeof global.gc !== 'function') {
    const run = cp.spawnSync(process.execPath, ['--expose-gc', __filename, ...process.argv.slice(2)], {stdio: 'inherit'});
    process.exit(run.status === null ? 1 : run.status);
}
const {streamsSource} = require('./cache_storage_contract.cjs');
const web = 'user/libc/web/';
const option = name => { const a = process.argv.find(x => x.startsWith('--' + name + '=')); return a ? a.slice(name.length + 3) : null; };
const wpt = option('wpt') || process.env.WPT_ROOT || 'D:/Programes/cloned repos/wpt';
const base = option('base'), verbose = process.argv.includes('--verbose');
const files = [
    'fetch/api/headers/headers-basic.any.js', 'fetch/api/headers/headers-casing.any.js', 'fetch/api/headers/headers-combine.any.js',
    'fetch/api/headers/headers-errors.any.js', 'fetch/api/headers/headers-normalize.any.js', 'fetch/api/headers/headers-record.any.js',
    'fetch/api/headers/headers-structure.any.js', 'fetch/api/headers/headers-no-cors.any.js', 'fetch/api/headers/header-setcookie.any.js',
    'fetch/api/headers/headers-forbidden-override.any.js',
    'fetch/api/request/forbidden-method.any.js', 'fetch/api/request/request-constructor-init-body-override.any.js',
    'fetch/api/request/request-consume-empty.any.js', 'fetch/api/request/request-consume.any.js', 'fetch/api/request/request-disturbed.any.js',
    'fetch/api/request/request-error.any.js', 'fetch/api/request/request-headers.any.js', 'fetch/api/request/request-init-002.any.js',
    'fetch/api/request/request-init-contenttype.any.js', 'fetch/api/request/request-init-priority.any.js', 'fetch/api/request/request-init-stream.any.js',
    'fetch/api/request/request-structure.any.js', 'fetch/api/request/request-clone-readable-stream-body.any.js',
    'fetch/api/response/json.any.js', 'fetch/api/response/response-clone.any.js', 'fetch/api/response/response-consume-empty.any.js',
    'fetch/api/response/response-consume-stream.any.js', 'fetch/api/response/response-error.any.js', 'fetch/api/response/response-error-from-stream.any.js',
    'fetch/api/response/response-from-stream.any.js', 'fetch/api/response/response-headers-guard.any.js', 'fetch/api/response/response-init-001.any.js',
    'fetch/api/response/response-init-002.any.js', 'fetch/api/response/response-init-contenttype.any.js', 'fetch/api/response/response-static-error.any.js',
    'fetch/api/response/response-static-json.any.js', 'fetch/api/response/response-static-redirect.any.js', 'fetch/api/response/response-stream-bad-chunk.any.js',
    'fetch/api/response/response-stream-disturbed-1.any.js', 'fetch/api/response/response-stream-disturbed-2.any.js',
    'fetch/api/response/response-stream-disturbed-3.any.js', 'fetch/api/response/response-stream-disturbed-4.any.js',
    'fetch/api/response/response-stream-disturbed-5.any.js', 'fetch/api/response/response-stream-disturbed-6.any.js',
    'fetch/api/response/response-stream-disturbed-by-pipe.any.js',
    'fetch/api/body/formdata.any.js', 'fetch/api/body/cloned-any.js', 'fetch/api/abort/request.any.js',
    'dom/abort/AbortSignal.any.js', 'dom/abort/abort-signal-any.any.js', 'dom/abort/event.any.js', 'dom/abort/timeout.any.js',
    /* Fetch's scheme fetch "blob" and FileAPI's blob URL store. scheme-blob's /images/blue.png
       needs the network: there it is a network error, so the tests after it run. */
    {file: 'fetch/api/basic/scheme-blob.sub.any.js', network: 'error'}, 'fetch/range/blob.any.js',
    'FileAPI/url/url-format.any.js', 'FileAPI/url/url-with-fetch.any.js',
    /* DOM "Aborting ongoing activities", in testharness.js's form: the garbage collection rule
       ("A non-aborted dependent AbortSignal object must not be garbage collected while its source
       signals is non-empty and it has registered event listeners for its abort event or its abort
       algorithms is non-empty"; an event handler is such a listener), Web IDL's sequence from an
       iterable (each value converted as it is taken) and [EnforceRange] unsigned long long. */
    {file: '(contract) dom/abort', text: `
        const collect = async () => { for (let i = 0; i < 3; i++) { await new Promise(r => setTimeout(r, 0)); __gc(); } };
        promise_test(async () => {
            const controller = new AbortController();
            let fired = 0;
            (() => { AbortSignal.any([controller.signal]).addEventListener('abort', () => fired++); })();
            await collect();
            controller.abort();
            assert_equals(fired, 1);
        }, 'A dependent signal with an abort event listener lives while its source does');
        promise_test(async () => {
            const controller = new AbortController();
            let fired = 0;
            (() => { AbortSignal.any([AbortSignal.any([controller.signal])]).onabort = () => fired++; })();
            await collect();
            controller.abort();
            assert_equals(fired, 1);
        }, 'A dependent signal with an onabort handler lives while its source does');
        promise_test(async () => {
            const controller = new AbortController();
            let fired = 0;
            (() => {
                const signal = AbortSignal.any([controller.signal]), handler = () => fired++;
                signal.addEventListener('abort', handler);
                signal.removeEventListener('abort', handler);
            })();
            await collect();
            controller.abort();
            assert_equals(fired, 0);
        }, 'A removed listener no longer holds its dependent signal');
        test(() => {
            let taken = 0;
            function* signals() { taken++; yield new AbortController().signal; taken++; yield {}; taken++; yield new AbortController().signal; }
            assert_throws_js(TypeError, () => AbortSignal.any(signals()));
            assert_equals(taken, 2, 'the value after the first non-AbortSignal is not taken');
        }, 'AbortSignal.any() converts each value as it takes it from the iterator');
        test(() => {
            for (const value of [2 ** 53, -1, NaN, Infinity, -Infinity])
                assert_throws_js(TypeError, () => AbortSignal.timeout(value), String(value));
            assert_false(AbortSignal.timeout(2 ** 53 - 1).aborted);
            assert_false(AbortSignal.timeout(-0.9).aborted);
        }, 'AbortSignal.timeout() takes an [EnforceRange] unsigned long long');
        promise_test(async () => {
            const controller = new AbortController(), order = [];
            const timeout = AbortSignal.timeout(0), any = AbortSignal.any([controller.signal, timeout]);
            any.addEventListener('abort', () => order.push('any ' + any.reason.name));
            timeout.addEventListener('abort', () => order.push('timeout ' + timeout.reason.name));
            await new Promise(r => setTimeout(r, 20));
            controller.abort();
            assert_array_equals(order, ['timeout TimeoutError', 'any TimeoutError']);
        }, 'A timeout signal aborts before its dependents, with a TimeoutError');`},
];
const BASE_URL = 'https://web-platform.test:8443/fetch/api/request/request-structure.any.html';
const read = file => fs.readFileSync(file, 'utf8');
const git = (rev, file) => cp.execFileSync('git', ['show', rev + ':' + file], {encoding: 'utf8', maxBuffer: 1 << 26});
/* A file of the working tree, or of REV. */
const source = (rev, file) => rev ? git(rev, file) : read(file);
/* text from start to end (end included unless open). */
function slice(text, start, end, open = false) {
    const a = text.indexOf(start), b = text.indexOf(end, a);
    assert(a >= 0 && b > a, start);
    return text.slice(a, open ? b : b + end.length);
}
/* A data: URL as the native host reads it: [bytes, type]. */
const dataURL = `url=>{const comma=url.indexOf(',');const meta=url.slice(5,comma),body=url.slice(comma+1);
    const b64=/;base64$/i.test(meta),type=(b64?meta.slice(0,-7):meta)||'text/plain;charset=US-ASCII';
    const octets=[];for(let i=0;i<body.length;i++){const hex=body.slice(i+1,i+3);
        if(body[i]==='%'&&/^[0-9a-fA-F]{2}$/.test(hex)){octets.push(parseInt(hex,16));i+=2;}else octets.push(...Buffer.from(body[i],'utf8'));}
    const bytes=b64?Buffer.from(Buffer.from(octets).toString('latin1'),'base64'):Buffer.from(octets);
    return [new Uint8Array(bytes).buffer,type];}`;
const digest = (algorithm, bytes) => {
    const hash = crypto.createHash(algorithm.replace('-', '').toLowerCase()).update(Buffer.from(bytes)).digest();
    return new Uint8Array(hash).buffer;
};
/* js_worker_runtime.js's Event, EventTarget, dispatch and event handler attributes (REV's with
   --base), the event system of both realms here. */
function events(rev) {
    const runtime = source(rev, web + 'js_worker_runtime.js');
    let handlers = slice(runtime, '    let blobHandlerTarget=', '    /* @include js_navigator.js */', true);
    if (!handlers.includes('abortHandlerTarget'))   // REV's Window AbortSignal assigned the Window's own
        handlers = handlers.replace('indexedDBHandlerTarget=()=>false;', 'indexedDBHandlerTarget=()=>false,abortHandlerTarget=()=>false;')
            .replace('||indexedDBHandlerTarget(value,type);', '||indexedDBHandlerTarget(value,type)||abortHandlerTarget(value,type);');
    return slice(runtime, '    const eventStates=new WeakMap()', '    Object.assign(globalThis,{DOMException,QuotaExceededError,Event', true)
        .replace('/* @include js_error_event.js */', read(web + 'js_error_event.js'))
        .replace('/* @include js_promise_rejection.js */', read(web + 'js_promise_rejection.js'))
        .replace('/* @include js_custom_event.js */', read(web + 'js_custom_event.js')) +
        'const target=new EventTarget();Object.assign(globalThis,{Event,EventTarget,CustomEvent});\n' + handlers;
}
/* node's URL is every realm's: a realm without a blob URL store has no URL.createObjectURL. */
const hideObjectURLs = () => { for (const name of ['createObjectURL', 'revokeObjectURL'])
    Object.defineProperty(URL, name, {value: undefined, writable: true, configurable: true, enumerable: false}); };
/* The realm's shared start: what both a Window's and a Worker's bootstrap have before Fetch:
   js_worker_runtime.js's ProgressEvent and js_blob.js (this tree's, the environment of every REV). */
function context(rev) {
    hideObjectURLs();
    const c = {console, URL, URLSearchParams, TextEncoder, TextDecoder, setTimeout, clearTimeout, crypto: globalThis.crypto,
        location: new URL(BASE_URL), queueMicrotask, Buffer, WeakRef, FinalizationRegistry, digest, __gc: global.gc};
    vm.createContext(c);
    c.self = vm.runInContext('globalThis', c);
    vm.runInContext(read(web + 'js_dom_exception.js'), c);
    vm.runInContext(`const apply=Reflect.apply,DOMException=domExceptionBridge.DOMException;globalThis.DOMException=DOMException;
        const reported=[],calls=[],sent=[],cancelled=[],resumed=[];
        function report(e){reported.push(e);}
        const host={now:()=>Date.now(),promiseThen:(p,f,r)=>Promise.prototype.then.call(p,f,r),dataURL:${dataURL},cryptoDigest:digest,
            safety:(name,fallback)=>fallback,
            /* The native UTF-8 concatenation of Blob parts: strings as scalar values, bytes as they are. */
            concatUTF8(chunks){const parts=[];let n=0;
                for(const c of chunks){const b=typeof c==='string'?new Uint8Array(Buffer.from(c.toWellFormed(),'utf8')):new Uint8Array(c.buffer,c.byteOffset,c.byteLength);parts.push(b);n+=b.length;}
                const out=new Uint8Array(n);let at=0;for(const b of parts){out.set(b,at);at+=b.length;}return out.buffer;}};`, c);
    vm.runInContext(events(rev), c, {filename: (rev ? rev + ':' : '') + 'js_worker_runtime.js (events)'});
    vm.runInContext(streamsSource(), c);
    const runtime = read(web + 'js_worker_runtime.js');
    vm.runInContext(slice(runtime, '    const progressSlots=new WeakMap();', "{value:ProgressEvent,writable:true,configurable:true});") +
        '\n' + read(web + 'js_blob.js'), c, {filename: 'js_blob.js'});
    return c;
}
/* The realm's blob URL store, as its bootstrap installs it after FormData. */
function objectURLs(c, rev, text, start, end) {
    if (!text.includes(start)) return;
    vm.runInContext(slice(text, start, end).replace('/* @include js_object_url.js */', source(rev, web + 'js_object_url.js')),
        c, {filename: (rev ? rev + ':' : '') + 'js_object_url.js'});
}
const includes = (rev, text) => text.replace('/* @include js_abort.js */', source(rev, web + 'js_abort.js'))
    .replace('/* @include js_fetch.js */', source(rev, web + 'js_fetch.js'));
const realms = {
    /* js_bootstrap.js from its js_abort.js include to the Fetch globals. */
    window(rev, networkError) {
        const c = context(rev);
        vm.runInContext(`let abortSignals,abortListen=()=>{};const mapGet=WeakMap.prototype.get;
            const document={},performanceBridge={initiator(){}};
            const rawDom={get:(d,k)=>({baseURI:'${BASE_URL}',fetchOrigin:new URL('${BASE_URL}').origin,fetchClientReferrer:'${BASE_URL}',referrerPolicy:0})[k]};
            Object.assign(host,{url:()=>'${BASE_URL}',timer:(k,fn,ms,args)=>setTimeout(()=>fn(...args),ms),clear:id=>clearTimeout(id),
                noCorsFetch:true,streamingFetch:true,resumeFetch(id){resumed.push(id);},cancel(id){cancelled.push(id);},
                fetch(...args){calls.push(args);
                    return {id:calls.length,promise:${networkError}?Promise.reject(new TypeError('Network error (the harness has no network)')):new Promise(()=>{})};}});
            /* REV's Worker events asked the Worker's own AbortSignal copy for the signal option. */
            var workerAbortBridge={brand:v=>abortSignals.is(v),state:v=>({aborted:abortSignals.aborted(v)}),subscribe:(s,f)=>abortSignals.follow(s,f)};`, c);
        const bootstrap = source(rev, web + 'js_bootstrap.js');
        vm.runInContext(includes(rev, slice(bootstrap, '    /* @include js_abort.js */', '    const {Headers,Request,Response,fetch}=fetchBridge;\n')),
            c, {filename: (rev ? rev + ':' : '') + 'js_bootstrap.js (Fetch)'});
        vm.runInContext(`fetchBridge.initialize();fetchBridge.initializeStreams(streamBridge);fetchBridge.initializeBlobs(blobBridge);
            Object.assign(globalThis,{Headers,Request,Response,fetch,AbortController,AbortSignal});`, c);
        formData(c, rev);
        objectURLs(c, rev, bootstrap, '    /* @include js_object_url.js */', 'fetchBridge.initializeObjectURLs(objectURLBridge);');
        return c;
    },
    /* js_worker_runtime.js from its AbortSignal to the Fetch globals. The parent answers a blob:
       URL's request as worker_blob does for a document whose store does not hold it. */
    worker(rev, networkError) {
        const c = context(rev);
        vm.runInContext(`let closed=false,address='${BASE_URL}',workerCanCancel=true,workerStreaming=true;
            const fetches=new Map();
            function timer(fn,ms,args){return setTimeout(()=>fn(...(args||[])),ms);}
            function clearTimer(id){clearTimeout(id);}
            Object.assign(host,{securityURL:()=>address,referrerPolicy:()=>0,resume(id){resumed.push(id);},cancel(id){cancelled.push(id);},
                request(...args){sent.push(args);const id=sent.length,blob=String(args[0].url).startsWith('blob:');
                    if(blob||${networkError})setTimeout(()=>workerFetchTransport.loaded(id,[0,'','',new ArrayBuffer(0),
                        blob?"The blob: URL has no Blob in the creating document's store":'Network error (the harness has no network)'],0));
                    return id;}});`, c);
        const runtime = source(rev, web + 'js_worker_runtime.js');
        const start = runtime.includes('    /* @include js_abort.js */') ? '    /* @include js_abort.js */' : '    const workerAbortBridge=(()=>{';
        vm.runInContext(includes(rev, slice(runtime, start, '    const {Headers,Request,Response,fetch}=fetchBridge;\n')),
            c, {filename: (rev ? rev + ':' : '') + 'js_worker_runtime.js (Fetch)'});
        vm.runInContext(`Object.assign(globalThis,{Headers,Request,Response,fetch,AbortController,AbortSignal});`, c);
        formData(c, rev);
        objectURLs(c, rev, runtime, '        /* @include js_object_url.js */', 'policy:()=>null}));');
        return c;
    },
};
/* js_formdata.js, as both bootstraps include it after Fetch and hand it to fetchBridge. */
function formData(c, rev) {
    vm.runInContext(source(rev, web + 'js_formdata.js'), c, {filename: 'js_formdata.js'});
    vm.runInContext('fetchBridge.initializeFormData(formDataBridge);', c);
}
const testharness = read(path.join(wpt, 'resources/testharness.js'));
const statusName = ['PASS', 'FAIL', 'TIMEOUT', 'NOTRUN', 'PRECONDITION_FAILED'];
/* One WPT file (or one of this file's own checks) in a new realm: its subtests' names and statuses. */
/* wptserve's substitutions in a .sub. file: the test domains and ports. */
const substitute = text => text.replace(/\{\{domains\[([^\]]*)\]\}\}/g, (m, sub) => (sub ? sub + '.' : '') + 'web-platform.test')
    .replace(/\{\{host\}\}/g, 'web-platform.test')
    .replace(/\{\{ports\[(https?)\]\[([01])\]\}\}/g, (m, scheme, i) => String({http: [8000, 8001], https: [8443, 8444]}[scheme][i]));
function runFile(kind, rev, entry) {
    const file = typeof entry === 'string' ? entry : entry.file;
    return new Promise(resolve => {
        const c = realms[kind](rev, typeof entry === 'object' && entry.network === 'error');
        const results = new Map();
        let timer;
        c.__done = (tests, status) => {
            clearTimeout(timer);
            for (const t of tests) results.set(t.name, statusName[t.status]);
            if (status.status !== 0) results.set('(harness) ' + file, 'ERROR ' + status.message);
            resolve(results);
        };
        vm.runInContext(testharness, c, {filename: 'testharness.js'});
        vm.runInContext('add_completion_callback((tests,status)=>__done(tests,status));', c);
        const own = typeof entry === 'object' && entry.text !== undefined;
        const full = path.join(wpt, own ? 'dom/abort/x' : file);
        const text = own ? entry.text : file.includes('.sub.') ? substitute(read(full)) : read(full);
        try {
            for (const line of text.split('\n')) {
                const m = /^\/\/ META: script=(.*)$/.exec(line.trim());
                if (m) vm.runInContext(read(m[1].startsWith('/') ? path.join(wpt, m[1]) : path.join(path.dirname(full), m[1])), c);
            }
            vm.runInContext(text, c, {filename: file});
        } catch (e) { results.set('(load) ' + file, 'ERROR ' + e.message); }
        vm.runInContext('done()', c);
        /* A test that waits on the network: the harness's own timeout (dom/abort's tests wait
           2 seconds for an event that must not come). */
        timer = setTimeout(() => vm.runInContext('timeout()', c), file.startsWith('dom/') ? 4000 : 1500);
    });
}
async function runRealm(kind, rev) {
    const all = new Map();
    for (const entry of files) for (const [name, status] of await runFile(kind, rev, entry)) all.set((entry.file || entry) + ' | ' + name, status);
    return all;
}
const passed = results => [...results].filter(([, s]) => s === 'PASS').length;
async function main() {
    const window = await runRealm('window'), worker = await runRealm('worker');
    const report = (label, r) => console.log(`${label}: ${passed(r)}/${r.size} subtests pass`);
    report('Window', window);
    report('Worker', worker);
    /* One owner: the same definitions give the same result in both realms. */
    const differ = [...window].filter(([name, s]) => worker.get(name) !== s);
    for (const [name, s] of differ) console.log(`  differs: ${name}: Window ${s}, Worker ${worker.get(name)}`);
    if (verbose) for (const [name, s] of window) if (s !== 'PASS') console.log(`  ${s}: ${name}`);
    let failed = differ.length > 0;
    if (base) {
        for (const [kind, now] of [['window', window], ['worker', worker]]) {
            const before = await runRealm(kind, base);
            report(`${kind} at ${base}`, before);
            const lost = [...before].filter(([name, s]) => s === 'PASS' && now.get(name) !== 'PASS');
            for (const [name] of lost) console.log(`  no longer passes in the ${kind}: ${name} (${now.get(name)})`);
            if (verbose) for (const [name, s] of now) if (s === 'PASS' && before.get(name) !== 'PASS') console.log(`  newly passes in the ${kind}: ${name} (was ${before.get(name)})`);
            if (lost.length) failed = true;
        }
    }
    /* AbortSignal.timeout's long timers would keep node waiting. */
    process.exit(failed ? 1 : 0);
}
/* A file's top-level body reads (request-disturbed's) reject with nobody waiting: not a result. */
process.on('unhandledRejection', () => {});
main().catch(e => { console.error(e); process.exit(1); });
