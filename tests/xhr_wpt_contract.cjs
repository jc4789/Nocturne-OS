/* WPT's XMLHttpRequest tests (the xhr/*.any.js files below and the encoding tests that decode
 * through XHR, run by WPT's own resources/testharness.js) against the production XMLHttpRequest
 * (js_xhr.js) in two node vm realms, each built from its bootstrap's own lines:
 *   a Window's: js_bootstrap.js from its js_abort.js include to the Fetch globals, then its XHR
 *     port (the js_xhr.js include to the js_blob.js one);
 *   a Worker's: js_worker_runtime.js's Event and EventTarget, its ProgressEvent and event handler
 *     lines (with the js_xhr.js include), its AbortSignal and Fetch (js_fetch.js,
 *     workerFetchTransport, the fetchBridge port) and its XHR port.
 * The Window's DOM events are the Worker runtime's Event, EventTarget and handlers (the window's
 * own dispatch needs the native DOM); its document is a stub, open()'s URL is node's URL parser.
 * The native transports are doubles over one in-process server: WPT's static files under the
 * test's directory (an .asis file is its raw response) and the xhr/resources programs content.py,
 * delay.py, status.py, trickle.py, echo-content-type.py and corsenabled.py, written here after
 * their Python (query values percent-decoded to bytes, as wptserve's request.GET). It delivers
 * frames as js.c (Window: host.fetch's notify) and js_worker.c (Worker: NJW_FRAME through
 * workerFetchTransport.frame) do, from the event loop, one per resume. The Worker's requestWait
 * (browserjsworker.c's pause for a synchronous request) answers at once from the same server,
 * null when the response would take longer than the timeout. Text decoding is node's TextDecoder
 * behind the native decoder's contract (decode's BOM sniffing, an unknown label as UTF-8, the
 * replacement decoder), labels from WPT's encoding/resources/encodings.js. No CORS, cookies,
 * redirects or CSP: those are webnet.c's and js.c's, not here. This is not the guest browser and
 * not wptrunner. It shows what the two realms pass, fails when a subtest the Window passes does not
 * pass in the Worker (one owner; the Window's synchronous requests are NotSupportedError, a Window
 * cannot pause, so those differ the other way and are listed), and, with --base=REV, fails when
 * either realm no longer passes a subtest REV's did (REV's sources through the same doubles).
 * Run: node tests/xhr_wpt_contract.cjs [--base=REV] [--wpt=DIR] [--verbose] [--only=FILE] */
'use strict';
const fs = require('node:fs'), vm = require('node:vm'), path = require('node:path'), cp = require('node:child_process');
const assert = require('node:assert/strict');
const {streamsSource} = require('./cache_storage_contract.cjs');
const web = 'user/libc/web/';
const option = name => { const a = process.argv.find(x => x.startsWith('--' + name + '=')); return a ? a.slice(name.length + 3) : null; };
const wpt = option('wpt') || process.env.WPT_ROOT || 'D:/Programes/cloned repos/wpt';
const base = option('base'), only = option('only'), verbose = process.argv.includes('--verbose');
const files = [
    'xhr/abort-after-receive.any.js', 'xhr/abort-after-send.any.js', 'xhr/abort-after-timeout.any.js',
    'xhr/abort-during-open.any.js', 'xhr/abort-during-readystatechange.any.js', 'xhr/abort-during-unsent.any.js',
    'xhr/abort-during-upload.any.js', 'xhr/abort-event-abort.any.js', 'xhr/abort-event-listeners.any.js',
    'xhr/abort-event-loadend.any.js', 'xhr/abort-progress-events.any.js', 'xhr/abort-upload-event-abort.any.js',
    'xhr/abort-upload-event-loadend.any.js', 'xhr/abort-with-error.any.js', 'xhr/content-type-unmodified.any.js',
    'xhr/event-abort.any.js', 'xhr/event-load.any.js', 'xhr/event-loadend.any.js', 'xhr/event-loadstart.any.js',
    'xhr/event-loadstart-upload.any.js', 'xhr/event-progress.any.js', 'xhr/event-readystate-sync-open.any.js',
    'xhr/event-readystatechange-loaded.any.js', 'xhr/event-timeout.any.js', 'xhr/event-timeout-order.any.js',
    'xhr/getresponseheader.any.js', 'xhr/json.any.js', 'xhr/over-1-meg.any.js', 'xhr/overridemimetype-done-state.any.js',
    'xhr/overridemimetype-unsent-state-force-shiftjis.any.js', 'xhr/responsetype.any.js', 'xhr/responseurl-after-abort.any.js',
    'xhr/send-data-arraybuffer.any.js', 'xhr/send-data-arraybufferview.any.js', 'xhr/send-data-es-object.any.js',
    'xhr/send-data-string-invalid-unicode.any.js', 'xhr/send-send.any.js', 'xhr/send-usp.any.js',
    'xhr/sync-no-progress.any.js', 'xhr/sync-no-timeout.any.js', 'xhr/XMLHttpRequest-withCredentials.any.js',
    'encoding/replacement-encodings.any.js', 'encoding/unsupported-encodings.any.js',
].filter(file => !only || file.includes(only));
const ORIGIN = 'https://web-platform.test:8443';
const read = file => fs.readFileSync(file, 'utf8');
const git = (rev, file) => cp.execFileSync('git', ['show', rev + ':' + file], {encoding: 'utf8', maxBuffer: 1 << 26});
const source = (rev, file) => rev ? git(rev, file) : read(file);
const expand = (rev, text) => text.replace(/\/\* @include ([a-z_]+\.js) \*\//g, (_, name) => source(rev, web + name));
/* From the line holding start to the end of the line holding end; null when start is missing. */
function lines(text, start, end, label) {
    const a = text.indexOf(start);
    if (a < 0) return null;
    const from = text.lastIndexOf('\n', a) + 1, b = text.indexOf(end, a);
    assert(b > a, label + ': ' + end);
    const to = text.indexOf('\n', b + end.length - 1);
    return text.slice(from, to < 0 ? text.length : to + 1);
}
const must = (text, label) => { assert(text !== null, label); return text; };

/* ---- the Encoding Standard's labels, as WPT lists them ---- */
const encodingNames = (() => {
    const c = {};vm.createContext(c);vm.runInContext(read(path.join(wpt, 'encoding/resources/encodings.js')) + ';globalThis.table=encodings_table;', c);
    const names = new Map();
    for (const section of c.table) for (const e of section.encodings) for (const label of e.labels) names.set(label, e.name);
    return names;
})();
/* The native decoder's contract (js_encoding.c web_js_text_decoder): label, fatal, ignoreBOM, and
   decode (XHR's text response: a label that is no encoding is UTF-8, replacement decodes, a BOM
   chooses the encoding). */
function textDecoder(label, fatal, ignoreBOM, decode) {
    let name = encodingNames.get(String(label).trim().toLowerCase()) || null;
    if (decode && !name) name = 'UTF-8';
    if (!name || (!decode && name === 'replacement')) throw new RangeError('Unsupported text encoding');
    return {name, fatal: !!fatal, ignoreBOM: !!ignoreBOM || !!decode, sniff: !!decode, pending: Buffer.alloc(0), inner: null, replaced: false};
}
function textDecode(d, bytes, offset, length, stream) {
    let input = bytes ? Buffer.from(bytes, offset, length) : Buffer.alloc(0);
    if (!d.inner) {
        input = Buffer.concat([d.pending, input]);
        if (d.sniff && stream && input.length < 3) { d.pending = input; return ''; }
        let name = d.name;
        if (d.sniff) {
            if (input[0] === 0xef && input[1] === 0xbb && input[2] === 0xbf) { name = 'UTF-8'; input = input.subarray(3); }
            else if (input[0] === 0xfe && input[1] === 0xff) { name = 'UTF-16BE'; input = input.subarray(2); }
            else if (input[0] === 0xff && input[1] === 0xfe) { name = 'UTF-16LE'; input = input.subarray(2); }
        }
        d.inner = name === 'replacement' ? {replacement: true} : new TextDecoder(name, {fatal: d.fatal, ignoreBOM: d.ignoreBOM});
    }
    if (d.inner.replacement) { if (input.length && !d.replaced) { d.replaced = true; return '\ufffd'; } return ''; }
    return d.inner.decode(input, {stream: !!stream});
}

/* ---- the server ---- */
const latin1 = text => Buffer.from(String(text), 'latin1');
const sleepFrames = (bytes, delay) => [{delay, bytes}];
/* A response: status, statusText, headers [[name, value]], headDelay (ms before the headers),
   chunks [{delay, bytes}], tailDelay; or {error} for a network error. */
function respond(status, statusText, headers, body, extra = {}) {
    const bytes = Buffer.isBuffer(body) ? body : latin1(body ?? '');
    return {status, statusText, headers: [...headers, ...(extra.noLength ? [] : [['Content-Length', String(bytes.length)]])],
        headDelay: extra.headDelay || 0, chunks: bytes.length ? sleepFrames(bytes, 0) : [], tailDelay: 0};
}
const contentTypes = {'.xml': 'application/xml', '.txt': 'text/plain', '.json': 'application/json', '.html': 'text/html', '.js': 'text/javascript'};
function serve(req) {
    let u;try{u = new URL(req.url);}catch(_){return {error: 'invalid URL'};}
    if (u.protocol !== 'https:' && u.protocol !== 'http:') return {error: 'unsupported URL scheme ' + u.protocol};
    if (u.origin !== ORIGIN) return {error: 'host not found: ' + u.host};
    /* wptserve's request.GET: percent-decoded bytes, isomorphic (one code unit per byte). */
    const params = new Map();
    for (const pair of u.search.slice(1).split('&')) { if (!pair) continue;
        const eq = pair.indexOf('='), bytes = s => Buffer.from(s.replace(/\+/g, ' ').replace(/%([0-9a-fA-F]{2})/g, (_, h) => String.fromCharCode(parseInt(h, 16))), 'latin1').toString('latin1');
        const key = bytes(eq < 0 ? pair : pair.slice(0, eq)); if (!params.has(key)) params.set(key, bytes(eq < 0 ? '' : pair.slice(eq + 1))); }
    const get = (name, fallback) => params.has(name) ? params.get(name) : fallback;
    const header = name => req.headers.has(name) ? req.headers.get(name) : 'NO';
    const query = u.search ? u.search.slice(1) : 'NO';
    const name = u.pathname.slice(u.pathname.lastIndexOf('/') + 1);
    switch (name) {
    case 'content.py': {
        const type = 'text/plain' + (params.has('response_charset_label') ? ';charset=' + get('response_charset_label') : '');
        const body = params.has('content') ? latin1(get('content')) : (req.body || Buffer.alloc(0));
        return respond(200, 'OK', [['Content-type', type], ['X-Request-Method', req.method], ['X-Request-Query', query],
            ['X-Request-Content-Length', req.body ? String(req.body.length) : 'NO'], ['X-Request-Content-Type', header('content-type')]], body);
    }
    case 'delay.py':
        return respond(200, 'OK', [['Access-Control-Allow-Origin', '*'], ['Access-Control-Allow-Methods', 'YO'], ['Content-type', 'text/plain']],
            'TEST_DELAY', {headDelay: Number(get('ms', 500))});
    case 'status.py':
        return respond(Number(get('code', 200)), get('text', 'OMG'), [['Content-Type', get('type', '')], ['X-Request-Method', req.method]], get('content', ''));
    case 'trickle.py': {
        const chunk = latin1('TEST_TRICKLE\n'), delay = Number(get('ms', 500)), count = Number(get('count', 50));
        const headers = [['Content-type', 'text/plain']];
        if (params.has('specifylength')) headers.unshift(['Content-Length', String(count * chunk.length)]);
        return {status: 200, statusText: 'OK', headers, headDelay: delay, chunks: Array.from({length: count}, (_, i) => ({delay: delay, bytes: chunk})), tailDelay: delay};
    }
    case 'echo-content-type.py':
        return respond(200, 'OK', [['Content-Type', 'text/plain'], ['Connection', 'close']], req.headers.get('content-type') ?? '');
    case 'corsenabled.py':
        return respond(200, 'OK', [['Access-Control-Allow-Origin', '*'], ['Access-Control-Allow-Credentials', 'true'],
            ['Access-Control-Allow-Methods', 'GET, POST, PUT, FOO'], ['Access-Control-Allow-Headers', 'x-test, x-foo'],
            ['Access-Control-Expose-Headers', 'x-request-method, x-request-content-type, x-request-query, x-request-content-length, x-request-data'],
            ['X-Request-Method', req.method], ['X-Request-Query', query], ['X-Request-Content-Length', req.body ? String(req.body.length) : 'NO'],
            ['X-Request-Content-Type', header('content-type')], ['X-Request-Data', req.body ? req.body.toString('latin1') : '']], 'Test',
            {headDelay: params.has('delay') ? Number(get('delay')) * 1000 : 0});
    }
    if (u.pathname === '/') return respond(200, 'OK', [['Content-Type', 'text/html']], '<!doctype html><title>Index</title>');
    const file = path.join(wpt, decodeURIComponent(u.pathname));
    if (!file.startsWith(path.resolve(wpt)) && !path.resolve(file).startsWith(path.resolve(wpt))) return respond(404, 'Not Found', [], '');
    if (!fs.existsSync(file) || !fs.statSync(file).isFile()) return respond(404, 'Not Found', [['Content-Type', 'text/plain']], 'Not Found');
    const bytes = fs.readFileSync(file);
    if (file.endsWith('.asis')) {
        const text = bytes.toString('latin1'), split = /\r?\n\r?\n/.exec(text), head = split ? text.slice(0, split.index) : text;
        const [statusLine, ...fields] = head.split(/\r?\n/), m = /^HTTP\/\S+\s+(\d{3})\s*(.*)$/.exec(statusLine);
        const headers = fields.filter(Boolean).map(line => { const colon = line.indexOf(':'); return [line.slice(0, colon), line.slice(colon + 1)]; });
        const body = split ? Buffer.from(text.slice(split.index + split[0].length), 'latin1') : Buffer.alloc(0);
        return {status: Number(m[1]), statusText: m[2], headers, headDelay: 0, chunks: body.length ? sleepFrames(body, 0) : [], tailDelay: 0};
    }
    return respond(200, 'OK', [['Content-Type', contentTypes[path.extname(file)] || 'application/octet-stream']], bytes);
}
const rawHeaders = res => 'HTTP/1.1 ' + res.status + ' ' + res.statusText + '\r\n' + res.headers.map(([n, v]) => n + ':' + (/^\s/.test(v) ? '' : ' ') + v + '\r\n').join('');
const bodyOf = res => Buffer.concat(res.chunks.map(c => c.bytes));
const duration = res => res.headDelay + res.chunks.reduce((n, c) => n + c.delay, 0) + res.tailDelay;
/* A request's header block, as the native transports carry it: Name: value lines. */
function headerMap(raw) {
    const map = new Map();
    if (typeof raw === 'string') for (const line of raw.split(/\r\n/)) { const colon = line.indexOf(':'); if (colon > 0) map.set(line.slice(0, colon).toLowerCase(), line.slice(colon + 1).trim()); }
    return map;
}
const sleep = ms => new Promise(resolve => setTimeout(resolve, ms));
/* One exchange over the server: deliver(kind, value) returns a promise for the consumer's resume. */
async function exchange(req, deliver, control) {
    const res = serve(req);
    if (req.body && req.body.length) { await deliver('upload', req.body.length); if (control.cancelled) return; }
    if (res.error) { await sleep(0); if (!control.cancelled) deliver('error', res.error); return; }
    await sleep(res.headDelay); if (control.cancelled) return;
    await deliver('headers', res); if (control.cancelled) return;
    for (const chunk of res.chunks) { await sleep(chunk.delay); if (control.cancelled) return; await deliver('chunk', chunk.bytes); if (control.cancelled) return; }
    await sleep(res.tailDelay); if (control.cancelled) return;
    deliver('end');
}
/* One resume per frame: a credit taken now or when it arrives. */
function credits() {
    const c = {count: 0, waiter: null, cancelled: false};
    c.give = () => { if (c.waiter) { const w = c.waiter; c.waiter = null; w(); } else c.count++; };
    c.take = () => c.count ? (c.count--, Promise.resolve()) : new Promise(resolve => { c.waiter = resolve; });
    return c;
}

/* ---- the realms ---- */
function context(kind, file) {
    const c = {console, URL, URLSearchParams, TextEncoder, TextDecoder, setTimeout, clearTimeout, Blob, File, queueMicrotask, Buffer, WeakRef, FinalizationRegistry};
    vm.createContext(c);
    c.self = vm.runInContext('globalThis', c);
    const url = ORIGIN + '/' + file.replace(/\.js$/, kind === 'worker' ? '.worker.js' : '.html');
    c.__url = url;c.__reported = [];c.__logged = [];
    c.__decoder = {textDecoder, textDecode};
    /* The realm's own ArrayBuffers, as the native side makes them. */
    c.__arrayBuffer = vm.runInContext('bytes=>{const out=new ArrayBuffer(bytes.length);new Uint8Array(out).set(bytes);return out;}', c);
    c.GLOBAL = {isWindow: () => kind === 'window', isWorker: () => kind === 'worker', isShadowRealm: () => false};
    const location = new URL(url);
    c.location = {href: url, origin: location.origin, protocol: location.protocol, host: location.host, hostname: location.hostname,
        port: location.port, pathname: location.pathname, search: '', hash: '', toString: () => url};
    vm.runInContext(read(web + 'js_dom_exception.js'), c);
    vm.runInContext(`globalThis.DOMException=domExceptionBridge.DOMException;
        const host={now:()=>Date.now(),promiseThen:(p,f,r)=>Promise.prototype.then.call(p,f,r),textDecoder:__decoder.textDecoder,textDecode:__decoder.textDecode};`, c);
    vm.runInContext(streamsSource(), c);
    return c;
}
const dataURL = url => {
    const comma = url.indexOf(','), meta = url.slice(5, comma), body = url.slice(comma + 1);
    const b64 = /;base64$/i.test(meta), type = (b64 ? meta.slice(0, -7) : meta) || 'text/plain;charset=US-ASCII', octets = [];
    for (let i = 0; i < body.length; i++) { const hex = body.slice(i + 1, i + 3);
        if (body[i] === '%' && /^[0-9a-fA-F]{2}$/.test(hex)) { octets.push(parseInt(hex, 16)); i += 2; } else octets.push(...Buffer.from(body[i], 'utf8')); }
    const bytes = b64 ? Buffer.from(Buffer.from(octets).toString('latin1'), 'base64') : Buffer.from(octets);
    return [arrayBuffer(bytes), type];
};
const eventLines = (rev) => {
    const runtime = source(rev, web + 'js_worker_runtime.js');
    return expand(rev, must(lines(runtime, '    const eventStates=new WeakMap()', '    Object.assign(globalThis,{DOMException,QuotaExceededError,Event,', 'worker events'), 'worker events'));
};
const blobLines = `const blobBridge={brand:()=>false,bytes:()=>new ArrayBuffer(0),type:()=>'',fromBytes:(bytes,type)=>new Blob([bytes],{type})};
    const urlBridge={encodingParse(url,base){try{return new URL(url,base);}catch(_){return null;}}};`;
const realms = {
    /* js_bootstrap.js's Fetch and XHR ports over a Window transport double (host.fetch's notify). */
    window(rev, file) {
        const c = context('window', file);
        const bootstrap = source(rev, web + 'js_bootstrap.js');
        const fetchPort = expand(rev, must(lines(bootstrap, '    /* @include js_abort.js */', '    const {Headers,Request,Response,fetch}=fetchBridge;', 'window fetch'), 'window fetch'));
        /* From the js_xhr.js include to the line before js_blob.js's. */
        const xhrPort = expand(rev, must(lines(bootstrap, '    /* @include js_xhr.js */', '    /* @include js_blob.js */', 'window xhr'), 'window xhr')
            .replace(/[^\n]*\/\* @include js_blob\.js \*\/\n$/, ''));
        const handlers = must(lines(read(web + 'js_worker_runtime.js'), '    function installHandlers(prototype,types){', '}});}', 'worker handlers'), 'worker handlers');
        const transport = windowTransport(c);
        c.__transport = transport;
        vm.runInContext(`(function(){
            const apply=Reflect.apply,mapGet=WeakMap.prototype.get,DOMException=domExceptionBridge.DOMException,QuotaExceededError=domExceptionBridge.QuotaExceededError;
            function report(e){__reported.push(e);}
            let abortHandlerTarget=()=>false,abortSignals,xhrHandlerTarget=()=>false;
            ${eventLines(null)}
            const target=new EventTarget();
            /* The Window's event handler IDL attributes and dispatch, as a Worker has them. */
            const fileHandlers=new WeakMap(),handlerTarget=(v,t)=>xhrHandlerTarget(v,t)||abortHandlerTarget(v,t);
            ${handlers}
            const dispatch=(target,event)=>dispatchWorkerEvent(target,event);
            /* The Window's listener list holds its event handlers too. */
            function listenerList(target){const out=[],m=apply(mapGet,listeners,[target]),h=apply(mapGet,fileHandlers,[target]);
                if(m)for(const list of m.values())for(const x of list)out.push(x);if(h)for(const k in h)if(h[k])out.push({removed:false});return out.length?out:undefined;}
            const listenerMap={get:listenerList};
            const document={getElementsByTagName:()=>[]},Document=class Document{},performanceBridge={initiator(){}};
            const rawDom={get:(d,k)=>({baseURI:__url,fetchOrigin:'${ORIGIN}',fetchClientReferrer:__url,referrerPolicy:0,characterSet:'UTF-8'})[k],
                parseXML:()=>({nodeType:9}),parseDocument:()=>({nodeType:9})};
            ${blobLines}
            Object.assign(host,{url:()=>__url,timer:(k,fn,ms,args)=>setTimeout(()=>fn(...args),ms),clear:id=>clearTimeout(id),microtask:fn=>queueMicrotask(fn),
                log:(level,text)=>__logged.push(text),dataURL:(arrayBuffer=>${dataURL.toString()})(__arrayBuffer),cryptoDigest:()=>new ArrayBuffer(32),noCorsFetch:true,streamingFetch:true},__transport.host);
            ${fetchPort}
            fetchBridge.initialize();fetchBridge.initializeStreams(streamBridge);fetchBridge.initializeBlobs(blobBridge);
            __transport.bridge=fetchBridge;
            ${xhrPort}
            Object.assign(globalThis,{Event,EventTarget,Headers,Request,Response,fetch,AbortController,AbortSignal});
            /* The Window's document global, set once testharness.js has chosen its environment. */
            globalThis.__document=document;
        })();`, c, {filename: (rev ? rev + ':' : '') + 'js_bootstrap.js (Fetch, XHR)'});
        return c;
    },
    /* js_worker_runtime.js's events, XHR and Fetch lines over a Worker transport double. */
    worker(rev, file) {
        const c = context('worker', file);
        const runtime = source(rev, web + 'js_worker_runtime.js');
        const xhrLines = lines(runtime, '    /* XMLHttpRequest and ProgressEvent are the Window\'s', '        return found;\n    }', 'worker xhr');
        /* Its AbortSignal: js_abort.js (as the Window's), or REV's own copy. */
        const abortStart = runtime.includes('    /* @include js_abort.js */') ? '    /* @include js_abort.js */' : '    const workerAbortBridge=(()=>{';
        const fetchPart = expand(rev, must(lines(runtime, abortStart, '    const {Headers,Request,Response,fetch}=fetchBridge;', 'worker fetch'), 'worker fetch'));
        const xhrPort = lines(runtime, '    /* This Worker\'s XMLHttpRequest:', '        Object.defineProperty(globalThis,name,{value:xhr[name],writable:true,configurable:true});', 'worker xhr port');
        const transport = workerTransport(c);
        c.__transport = transport;
        vm.runInContext(`(function(){
            const apply=Reflect.apply,DOMException=domExceptionBridge.DOMException,QuotaExceededError=domExceptionBridge.QuotaExceededError;
            let closed=false,address=__url,workerCanCancel=true,workerStreaming=true;
            const fetches=new Map();
            /* The Worker's performance timeline (js_performance.js) is not run here. */
            const performanceBridge={initiator(){},monotonicResource(){}};
            function report(e){__reported.push(e);}
            function timer(fn,ms,args){return setTimeout(()=>fn(...(args||[])),ms);}
            function clearTimer(id){clearTimeout(id);}
            Object.assign(host,{timer:(_,fn,ms,args)=>timer(fn,ms,args,false),clear:clearTimer,securityURL:()=>address,referrerPolicy:()=>0,
                dataURL:(arrayBuffer=>${dataURL.toString()})(__arrayBuffer),cryptoDigest:()=>new ArrayBuffer(32),send:(op,id,value)=>__logged.push(value.message)},__transport.host);
            ${eventLines(rev)}
            /* The global's own event target (the runtime's). */
            const target=new EventTarget();
            ${xhrLines === null ? '' : expand(rev, xhrLines)}
            ${blobLines}
            ${fetchPart}
            ${xhrPort === null ? '' : xhrPort}
            __transport.port=workerFetchTransport;
            Object.assign(globalThis,{Event,EventTarget,Headers,Request,Response,fetch,AbortController,AbortSignal});
        })();`, c, {filename: (rev ? rev + ':' : '') + 'js_worker_runtime.js (events, Fetch, XHR)'});
        return c;
    },
};
/* js.c's side for a Window: host.fetch(url, method, headers, body, ..., notify, ...) -> {id,
   promise}; notify(1, Response) / (2, ArrayBuffer) / (4, uploaded) / (3, error or undefined), the
   next frame after notify answers true or host.resumeFetch(id). */
function windowTransport(c) {
    const t = {requests: [], controls: new Map(), bridge: null};
    let next = 0;
    t.host = {
        fetch(url, method, headers, body, sameOrigin, credentials, forcePreflight, redirectError, cache, keepalive, noCors, noReferrer, notify) {
            const id = ++next, control = credits();t.controls.set(id, control);
            const req = {method, url, headers: headerMap(headers), body: body ? Buffer.from(body) : null};t.requests.push(req);
            let resolve, reject;const promise = new Promise((a, b) => { resolve = a; reject = b; });
            /* js.c calls notify from its event loop, never inside host.fetch. */
            const frame = (event, value, then) => { setTimeout(() => { if (control.cancelled) return; if (notify(event, value)) control.give(); if (then) then(); }, 0); return control.take(); };
            let buffered = null;
            exchange(req, (kind, value) => {
                if (kind === 'error') { const e = new TypeError(value); if (notify) notify(3, e); reject(e); return Promise.resolve(); }
                if (kind === 'end' && notify) { setTimeout(() => { if (!control.cancelled) notify(3, undefined); }, 0); return Promise.resolve(); }
                if (!notify) {
                    if (kind === 'headers') buffered = value;
                    else if (kind === 'end') resolve(t.bridge.response(buffered.status, url, rawHeaders(buffered), undefined, c.__arrayBuffer(bodyOf(buffered)), false, buffered.statusText, 'basic'));
                    return Promise.resolve();
                }
                if (kind === 'upload') return frame(4, value);
                if (kind === 'headers') {
                    const response = t.bridge.response(value.status, url, rawHeaders(value), undefined, c.__arrayBuffer(Buffer.alloc(0)), false, value.statusText, 'basic');
                    return frame(1, response, () => resolve(response));
                }
                return frame(2, c.__arrayBuffer(value));
            }, control);
            return {id, promise};
        },
        resumeFetch(id) { const control = t.controls.get(id); if (control) control.give(); },
        cancel(id) { const control = t.controls.get(id); if (control) control.cancelled = true; },
    };
    return t;
}
/* js_worker.c's side for a Worker: host.request(query, stream) -> id; NJW_FRAME through
   workerFetchTransport.frame (headers [status, url, raw], a chunk, the end, upload progress),
   the next after each host.resume(id); an error before the headers is NJW_LOADED's. requestWait:
   browserjsworker.c's pause, answered from the server at once. */
function workerTransport(c) {
    const t = {requests: [], controls: new Map(), port: null};
    let next = 0;
    t.host = {
        request(q, stream) {
            const id = ++next, req = {method: q.method, url: q.url, headers: headerMap(q.headers), body: q.body ? Buffer.from(q.body) : null, stream, noCors: q.noCors};
            t.requests.push(req);
            if (!stream) { t.controls.set(id, {sync: req}); return id; }
            const control = credits();t.controls.set(id, control);
            const frame = (kind, value) => { setTimeout(() => { if (!control.cancelled) t.port.frame(id, value, kind); }, 0); return control.take(); };
            exchange(req, (kind, value) => {
                if (kind === 'error') { t.port.loaded(id, [0, '', '', null, value], 0); return Promise.resolve(); }
                if (kind === 'upload') return frame(4 << 8, value);
                if (kind === 'headers') return frame(1 << 8, [value.status, req.url, rawHeaders(value)]);
                if (kind === 'chunk') return frame(2 << 8, c.__arrayBuffer(value));
                frame(3 << 8, '');return Promise.resolve();
            }, control);
            return id;
        },
        resume(id) { const control = t.controls.get(id); if (control && control.give) control.give(); },
        cancel(id) { const control = t.controls.get(id); if (control) control.cancelled = true; },
        requestWait(id, timeout) {
            const control = t.controls.get(id);t.controls.delete(id);
            if (!control || !control.sync) throw new TypeError('Worker request identifier required');
            const res = serve(control.sync);
            if (timeout > 0 && !res.error && duration(res) > timeout) return null;
            if (res.error) return [[0, '', '', null, res.error], 0];
            return [[res.status, control.sync.url, rawHeaders(res), c.__arrayBuffer(bodyOf(res)), ''], 0];
        },
    };
    return t;
}

/* ---- running ---- */
const testharness = read(path.join(wpt, 'resources/testharness.js'));
const statusName = ['PASS', 'FAIL', 'TIMEOUT', 'NOTRUN', 'PRECONDITION_FAILED'];
/* One WPT file in a new realm: its subtests' names and statuses. */
function runFile(kind, rev, file) {
    return new Promise(resolve => {
        const results = new Map(), full = path.join(wpt, file), text = read(full);
        let c;
        try { c = realms[kind](rev, file); }
        catch (e) { results.set('(realm) ' + file, 'ERROR ' + e.message); resolve(results); return; }
        let timer;
        c.__done = (tests, status) => {
            clearTimeout(timer);
            for (const t of tests) results.set(t.name, statusName[t.status] + (t.status && verbose && t.message ? ' ' + t.message : ''));
            if (status.status !== 0 && !tests.length) results.set('(harness) ' + file, 'ERROR ' + status.message);
            resolve(results);
        };
        vm.runInContext(testharness, c, {filename: 'testharness.js'});
        vm.runInContext('add_completion_callback((tests,status)=>__done(tests,status));if(globalThis.__document)globalThis.document=__document;', c);
        try {
            for (const line of text.split('\n')) {
                const m = /^\/\/ META: script=(.*)$/.exec(line.trim());
                if (m) vm.runInContext(read(m[1].startsWith('/') ? path.join(wpt, m[1]) : path.join(path.dirname(full), m[1])), c);
            }
            vm.runInContext(text, c, {filename: file});
        } catch (e) { results.set('(load) ' + file, 'ERROR ' + e.message); }
        vm.runInContext('done()', c);
        timer = setTimeout(() => vm.runInContext('timeout()', c), /META: timeout=long/.test(text) ? 30000 : 10000);
    });
}
async function runRealm(kind, rev) {
    const all = new Map();
    await Promise.all(files.map(async file => { for (const [name, status] of await runFile(kind, rev, file)) all.set(file + ' | ' + name, status); }));
    return new Map([...all].sort((a, b) => a[0] < b[0] ? -1 : a[0] > b[0] ? 1 : 0));
}
const passed = results => [...results].filter(([, s]) => s.startsWith('PASS')).length;
async function main() {
    const window = await runRealm('window'), worker = await runRealm('worker');
    const report = (label, r) => console.log(`${label}: ${passed(r)}/${r.size} subtests pass`);
    report('Window (js_xhr.js)', window);
    report('Worker (js_xhr.js)', worker);
    /* One owner: a subtest the Window passes passes in the Worker too. */
    let failed = false;
    for (const [name, s] of window) {
        const w = worker.get(name) || 'MISSING';
        if (s.startsWith('PASS') === w.startsWith('PASS')) continue;
        console.log(`  differs: ${name}: Window ${s}, Worker ${w}`);
        if (s.startsWith('PASS')) failed = true;
    }
    if (verbose) for (const [kind, r] of [['Window', window], ['Worker', worker]]) for (const [name, s] of r) if (!s.startsWith('PASS')) console.log(`  ${kind} ${s}: ${name}`);
    if (base) {
        for (const [kind, now] of [['window', window], ['worker', worker]]) {
            const before = await runRealm(kind, base);
            report(`${kind} at ${base}`, before);
            const lost = [...before].filter(([name, s]) => s.startsWith('PASS') && !(now.get(name) || '').startsWith('PASS'));
            for (const [name] of lost) console.log(`  no longer passes in the ${kind}: ${name} (${now.get(name)})`);
            if (verbose) for (const [name, s] of now) if (s.startsWith('PASS') && !(before.get(name) || '').startsWith('PASS')) console.log(`  now passes in the ${kind}: ${name}`);
            if (lost.length) failed = true;
        }
    }
    if (failed) process.exit(1);
}
if (require.main === module) {
    process.on('unhandledRejection', () => {});
    main().catch(e => { console.error(e); process.exit(1); });
}
