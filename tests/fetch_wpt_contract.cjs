/* WPT's fetch/api tests of Headers, Request and Response (the files below, run by WPT's own
 * resources/testharness.js) against the production Fetch definitions in two node vm realms: a
 * Window's (js_abort.js and js_fetch.js with js_bootstrap.js's port, as js_embed.py includes
 * them) and a Worker's (js_worker_runtime.js's AbortSignal, js_fetch.js and its port). The
 * native transports are recorders that never answer, so a test that needs the network times out
 * in every realm; the realms have node's URL, TextEncoder and EventTarget, no Blob or FormData.
 * This is not the guest browser and not wptrunner: it shows the two realms give the same results
 * and, with --base=REV, that neither realm passes fewer subtests than REV's Window and Worker
 * Fetch (REV's js_fetch.js and REV's Worker copy of it) did.
 * Run: node tests/fetch_wpt_contract.cjs [--base=REV] [--wpt=DIR] [--verbose] */
'use strict';
const fs = require('node:fs'), vm = require('node:vm'), path = require('node:path'), cp = require('node:child_process');
const crypto = require('node:crypto'), assert = require('node:assert/strict');
const {streamsSource} = require('./cache_storage_contract.cjs');
const web = 'user/libc/web/';
const option = name => { const a = process.argv.find(x => x.startsWith('--' + name + '=')); return a ? a.slice(name.length + 3) : null; };
const wpt = option('wpt') || process.env.WPT_ROOT || 'D:/Programes/cloned repos/wpt';
const base = option('base'), verbose = process.argv.includes('--verbose');
const files = [
    'headers/headers-basic.any.js', 'headers/headers-casing.any.js', 'headers/headers-combine.any.js',
    'headers/headers-errors.any.js', 'headers/headers-normalize.any.js', 'headers/headers-record.any.js',
    'headers/headers-structure.any.js', 'headers/headers-no-cors.any.js', 'headers/header-setcookie.any.js',
    'headers/headers-forbidden-override.any.js',
    'request/forbidden-method.any.js', 'request/request-constructor-init-body-override.any.js',
    'request/request-consume-empty.any.js', 'request/request-consume.any.js', 'request/request-disturbed.any.js',
    'request/request-error.any.js', 'request/request-headers.any.js', 'request/request-init-002.any.js',
    'request/request-init-contenttype.any.js', 'request/request-init-priority.any.js', 'request/request-init-stream.any.js',
    'request/request-structure.any.js', 'request/request-clone-readable-stream-body.any.js',
    'response/json.any.js', 'response/response-clone.any.js', 'response/response-consume-empty.any.js',
    'response/response-consume-stream.any.js', 'response/response-error.any.js', 'response/response-error-from-stream.any.js',
    'response/response-from-stream.any.js', 'response/response-headers-guard.any.js', 'response/response-init-001.any.js',
    'response/response-init-002.any.js', 'response/response-init-contenttype.any.js', 'response/response-static-error.any.js',
    'response/response-static-json.any.js', 'response/response-static-redirect.any.js', 'response/response-stream-bad-chunk.any.js',
    'response/response-stream-disturbed-1.any.js', 'response/response-stream-disturbed-2.any.js',
    'response/response-stream-disturbed-3.any.js', 'response/response-stream-disturbed-4.any.js',
    'response/response-stream-disturbed-5.any.js', 'response/response-stream-disturbed-6.any.js',
    'response/response-stream-disturbed-by-pipe.any.js',
];
const BASE_URL = 'https://web-platform.test:8443/fetch/api/request/request-structure.any.html';
const read = file => fs.readFileSync(file, 'utf8');
const git = (rev, file) => cp.execFileSync('git', ['show', rev + ':' + file], {encoding: 'utf8', maxBuffer: 1 << 26});
function slice(source, start, end) {
    const a = source.indexOf(start), b = source.indexOf(end, a);
    assert(a >= 0 && b > a, start);
    return source.slice(a, b + end.length);
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
/* The realm's shared start: what both a Window's and a Worker's bootstrap have before Fetch. */
function context() {
    const c = {console, URL, URLSearchParams, TextEncoder, TextDecoder, EventTarget, Event, setTimeout, clearTimeout, Blob, File, crypto: globalThis.crypto,
        queueMicrotask, Buffer, WeakRef, FinalizationRegistry, digest};
    vm.createContext(c);
    c.self = vm.runInContext('globalThis', c);
    vm.runInContext(read(web + 'js_dom_exception.js'), c);
    vm.runInContext(`const DOMException=domExceptionBridge.DOMException;globalThis.DOMException=DOMException;
        const reported=[],calls=[],sent=[],cancelled=[],resumed=[];
        const host={now:()=>Date.now(),promiseThen:(p,f,r)=>Promise.prototype.then.call(p,f,r),dataURL:${dataURL},cryptoDigest:digest};
        /* Body's blob(): node's Blob of the bytes; no Blob is a BodyInit here. */
        const blobBridge={brand:()=>false,bytes:()=>new ArrayBuffer(0),type:()=>'',fromBytes:(bytes,type)=>new Blob([bytes],{type})};`, c);
    vm.runInContext(streamsSource(), c);
    return c;
}
const realms = {
    /* js_bootstrap.js's port and includes, from its js_abort.js include to the Fetch globals. */
    window(rev) {
        const c = context();
        vm.runInContext(`let abortHandlerTarget=()=>false,abortSignals;const mapGet=WeakMap.prototype.get,apply=Reflect.apply;
            function report(e){reported.push(e);}function eventState(){return {};}function installHandlers(){}
            function dispatch(target,event){return EventTarget.prototype.dispatchEvent.call(target,event);}
            const document={},performanceBridge={initiator(){}};
            const rawDom={get:(d,k)=>({baseURI:'${BASE_URL}',fetchOrigin:new URL('${BASE_URL}').origin,fetchClientReferrer:'${BASE_URL}',referrerPolicy:0})[k]};
            Object.assign(host,{url:()=>'${BASE_URL}',timer:(k,fn,ms,args)=>setTimeout(()=>fn(...args),ms),clear:id=>clearTimeout(id),
                noCorsFetch:true,streamingFetch:true,resumeFetch(id){resumed.push(id);},cancel(id){cancelled.push(id);},
                fetch(...args){calls.push(args);return {id:calls.length,promise:new Promise(()=>{})};}});`, c);
        if (rev) {
            vm.runInContext(git(rev, web + 'js_fetch.js'), c, {filename: rev + ':js_fetch.js'});
        } else {
            const bootstrap = read(web + 'js_bootstrap.js');
            const port = slice(bootstrap, '    /* @include js_abort.js */', '    const {Headers,Request,Response,fetch}=fetchBridge;\n')
                .replace('/* @include js_abort.js */', read(web + 'js_abort.js')).replace('/* @include js_fetch.js */', read(web + 'js_fetch.js'));
            vm.runInContext(port, c, {filename: 'js_bootstrap.js (Fetch)'});
        }
        vm.runInContext(`fetchBridge.initialize();fetchBridge.initializeStreams(streamBridge);fetchBridge.initializeBlobs(blobBridge);
            Object.assign(globalThis,{Headers,Request,Response,fetch,AbortController,AbortSignal});`, c);
        formData(c, rev);
        return c;
    },
    /* js_worker_runtime.js from its AbortSignal to the Fetch globals. */
    worker(rev) {
        const c = context();
        vm.runInContext(`const apply=Reflect.apply;let closed=false,address='${BASE_URL}',workerCanCancel=true,workerStreaming=true;
            const fetches=new Map(),listeners=new WeakMap();
            function report(e){reported.push(e);}
            function timer(fn,ms,args){return setTimeout(()=>fn(...(args||[])),ms);}
            function clearTimer(id){clearTimeout(id);}
            function dispatchNativeWorkerEvent(target,event){return EventTarget.prototype.dispatchEvent.call(target,event);}
            Object.assign(host,{securityURL:()=>address,referrerPolicy:()=>0,resume(id){resumed.push(id);},cancel(id){cancelled.push(id);},
                request(...args){sent.push(args);return sent.length;}});`, c);
        if (rev) {
            const source = git(rev, web + 'js_worker_runtime.js');
            vm.runInContext(slice(source, '    const workerAbortBridge=(()=>{', '    const {Headers,Request,Response}=workerFetchBridge;\n') +
                slice(source, '    function fetch(input,init=undefined){', '\n    }\n'), c, {filename: rev + ':js_worker_runtime.js'});
        } else {
            const source = read(web + 'js_worker_runtime.js');
            vm.runInContext(slice(source, '    const workerAbortBridge=(()=>{', '    const {Headers,Request,Response,fetch}=fetchBridge;\n')
                .replace('/* @include js_fetch.js */', read(web + 'js_fetch.js')), c, {filename: 'js_worker_runtime.js (Fetch)'});
        }
        vm.runInContext(`Object.assign(globalThis,{Headers,Request,Response,fetch,AbortController,AbortSignal});`, c);
        formData(c, rev);
        return c;
    },
};
/* js_formdata.js, as both bootstraps include it after Fetch and hand it to fetchBridge. */
function formData(c, rev) {
    vm.runInContext(read(web + 'js_formdata.js'), c, {filename: 'js_formdata.js'});
    if (!rev || vm.runInContext("typeof fetchBridge!=='undefined'", c)) vm.runInContext('fetchBridge.initializeFormData(formDataBridge);', c);
}
const testharness = read(path.join(wpt, 'resources/testharness.js'));
const statusName = ['PASS', 'FAIL', 'TIMEOUT', 'NOTRUN', 'PRECONDITION_FAILED'];
/* One WPT file in a new realm: its subtests' names and statuses. */
function runFile(kind, rev, file) {
    return new Promise(resolve => {
        const c = realms[kind](rev);
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
        const full = path.join(wpt, 'fetch/api', file), source = read(full);
        try {
            for (const line of source.split('\n')) {
                const m = /^\/\/ META: script=(.*)$/.exec(line.trim());
                if (m) vm.runInContext(read(m[1].startsWith('/') ? path.join(wpt, m[1]) : path.join(path.dirname(full), m[1])), c);
            }
            vm.runInContext(source, c, {filename: file});
        } catch (e) { results.set('(load) ' + file, 'ERROR ' + e.message); }
        vm.runInContext('done()', c);
        /* A test that waits on the network: the harness's own timeout. */
        timer = setTimeout(() => vm.runInContext('timeout()', c), 1500);
    });
}
async function runRealm(kind, rev) {
    const all = new Map();
    for (const file of files) for (const [name, status] of await runFile(kind, rev, file)) all.set(file + ' | ' + name, status);
    return all;
}
const passed = results => [...results].filter(([, s]) => s === 'PASS').length;
async function main() {
    const window = await runRealm('window'), worker = await runRealm('worker');
    const report = (label, r) => console.log(`${label}: ${passed(r)}/${r.size} subtests pass`);
    report('Window (js_fetch.js)', window);
    report('Worker (js_fetch.js)', worker);
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
            if (lost.length) failed = true;
        }
    }
    if (failed) process.exit(1);
}
/* A file's top-level body reads (request-disturbed's) reject with nobody waiting: not a result. */
process.on('unhandledRejection', () => {});
main().catch(e => { console.error(e); process.exit(1); });
