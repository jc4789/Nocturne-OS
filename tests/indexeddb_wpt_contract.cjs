/* WPT's IndexedDB tests (IndexedDB/*.any.js, run by WPT's own resources/testharness.js and the
 * files' META scripts) against the production IndexedDB (js_indexeddb.js) in a node vm realm built
 * from js_worker_runtime.js's own lines: its DOMException (js_dom_exception.js), its Event,
 * EventTarget, ErrorEvent and DOM dispatch (the listenerException IndexedDB reads), its event
 * handler IDL attributes, its structured clone (js_clone.js), then js_indexeddb.js.
 *
 * The realm's event loop is this file's, in virtual time: each task (a timer, one broker result)
 * runs alone, then the microtasks it queued, then the end of the task as js.c's end_task and
 * browserjsworker.c's jobs() run it (indexedDBTaskEnd: the transactions it made active become
 * inactive). A file that only keeps itself busy (retries, polls) reaches the harness timeout in
 * virtual time, so a fixed defect and a slow path are told apart: a timeout is reported with how
 * the virtual time was spent (the broker's BUSY answers, the tasks run).
 *
 * The native side (host.indexedDB, js_indexeddb.c's operations) is a double of webidb_async.c and
 * webidb.c, one command at a time per realm's broker, each result as its own task
 * (js.c's indexed_db_run_one, browserjsworker.c's completion task):
 *   begin(name)                  webidb_begin: the database's lease, exclusive across brokers
 *                                (n_document_claim) and within one (its active list): BUSY when
 *                                held; [status, token, the committed manifest or null].
 *   stage(token, bytes)          webidb_stage: content-addressed payload, staged on the token.
 *   value(token, ref)            webidb_value: only a committed (current) or staged reference
 *                                (SECURITY otherwise).
 *   values(token, refs)          browserstorageworker.c's VALUES: value for each, in order.
 *   commit(token, bytes, refs, version)  webidb_commit: every reference current, old or staged
 *                                (SECURITY otherwise), the manifest written, payloads of neither
 *                                generation collected, then the lease released (webidb_abort).
 *   save(token, bytes, refs, version)    webidb_save: the same commit, the lease kept (the token
 *                                stays the database's, at the generation just written).
 *   abort(token)                 webidb_abort: the lease released, staged payloads dropped.
 *   databases()                  webidb_databases: the committed databases of version > 0; BUSY
 *                                while another broker holds a lease (catalog_scan claims each).
 *   connections(token, version)  webidb_connections: the client rows, marked and counted; the
 *                                ghosts of this version counted, and cleared once unfired is 0;
 *                                the ghosts of another version cleared.
 *   connect(token, version)      webidb_connect: a client row in state NONE (never a ghost's).
 *   poll(connection)             webidb_poll: FORCED (once, desired none) or MARKED (DELIVERED).
 *   fired(connection)            webidb_fired: DELIVERED becomes FIRED.
 *   disconnect, forget           webidb_disconnect: the row cleared; a FIRED one left a GHOST.
 * pack and unpack (QuickJS's JS_WriteObject/JS_ReadObject, js.c native_pack) are a deep copy that
 * keeps shared references; host.classID (JS_GetClassID) is node's util.types brands. The realm's
 * console, Blob, MessageChannel and the Window's DOM are not run: files that need them fail here
 * and are reported, not counted against IndexedDB.
 * Run: node tests/indexeddb_wpt_contract.cjs [--module=FILE] [--wpt=DIR] [--only=NAME] [--verbose]
 *   --module: another js_indexeddb.js (an earlier commit's, to compare); default the product's. */
'use strict';
const fs = require('node:fs'), vm = require('node:vm'), path = require('node:path'), util = require('node:util');
const crypto = require('node:crypto'), v8 = require('node:v8'), assert = require('node:assert/strict');
const web = 'user/libc/web/';
const option = name => { const a = process.argv.find(x => x.startsWith('--' + name + '=')); return a ? a.slice(name.length + 3) : null; };
const wpt = option('wpt') || process.env.WPT_ROOT || 'D:/Programes/cloned repos/wpt';
const only = option('only'), verbose = process.argv.includes('--verbose');
const moduleFile = option('module') || web + 'js_indexeddb.js';
const read = file => fs.readFileSync(file, 'utf8');
const expand = text => text.replace(/\/\* @include ([a-z_]+\.js) \*\//g, (_, name) => read(web + name));
/* From the line holding start to the end of the line holding end. */
function lines(text, start, end, label) {
    const a = text.indexOf(start);
    assert(a >= 0, label + ': ' + start);
    const from = text.lastIndexOf('\n', a) + 1, b = text.indexOf(end, a);
    assert(b > a, label + ': ' + end);
    const to = text.indexOf('\n', b + end.length - 1);
    return text.slice(from, to < 0 ? text.length : to + 1);
}

/* ---- the event loop: virtual time, one task at a time ---- */
const loop = {now: 0, seq: 0, queue: [], cancelled: new Set(), tasks: 0};
function schedule(delay, fn, label) {
    const task = {due: loop.now + Math.max(0, +delay || 0), seq: ++loop.seq, fn, label};
    let a = 0, b = loop.queue.length;
    while (a < b) { const m = (a + b) >> 1, q = loop.queue[m]; if (q.due < task.due || q.due === task.due && q.seq < task.seq) a = m + 1; else b = m; }
    loop.queue.splice(a, 0, task);
    return task.seq;
}
const drained = () => new Promise(resolve => setImmediate(resolve));

/* ---- the storage double (webidb.c), one disk for the run ---- */
const OK = 0, BUSY = 1, SECURITY = 2, IO = 3;
const NONE = 0, MARKED = 1, DELIVERED = 2, FIRED = 3, FORCED = 4, GHOST = 5;
function disk() { return {databases: new Map(), leases: new Map(), clientSerial: 0}; }
const digest = value => crypto.createHash('sha256').update(v8.serialize(value)).digest('hex');
function broker(store, origin, stats) {
    const active = new Map(), clients = new Map();let serial = 0;
    const base = name => { const key = origin + '\u0000' + name; let d = store.databases.get(key);
        if (!d) { d = {key, origin, name, meta: null, version: 0, values: new Map(), current: new Set(), old: new Set(), rows: []}; store.databases.set(key, d); } return d; };
    const tx = token => active.get(token >>> 0);
    function release(t) {
        active.delete(t.token);
        if (store.leases.get(t.db.key) === t) store.leases.delete(t.db.key);
        for (const ref of t.staged) if (!t.db.current.has(ref) && !t.db.old.has(ref)) t.db.values.delete(ref);
    }
    function write(t, bytes, refs, version) {
        const live = new Set(refs);
        for (const ref of live) if (!t.db.current.has(ref) && !t.db.old.has(ref) && !t.staged.has(ref)) return SECURITY;
        t.db.meta = bytes; t.db.version = version;
        for (const ref of t.db.old) if (!live.has(ref) && !t.db.current.has(ref)) t.db.values.delete(ref);
        t.db.old = t.db.current; t.db.current = live;
        for (const ref of t.staged) if (!live.has(ref) && !t.db.old.has(ref)) t.db.values.delete(ref);
        t.staged = new Set();
        return OK;
    }
    const ops = {
        begin(name) {
            const db = base(String(name));
            if (store.leases.has(db.key)) { stats.busy++; return [BUSY, 0, null]; }
            const t = {token: ++serial, db, staged: new Set()};
            active.set(t.token, t); store.leases.set(db.key, t);
            return [OK, t.token, db.meta];
        },
        stage(token, bytes) {
            const t = tx(token); if (!t) return [IO, ''];
            const ref = digest(bytes.__packed); if (!t.db.values.has(ref)) t.db.values.set(ref, bytes);
            t.staged.add(ref); return [OK, ref];
        },
        value(token, ref) {
            const t = tx(token); if (!t) return [IO, null];
            if (!t.db.current.has(ref) && !t.staged.has(ref)) return [SECURITY, null];
            return t.db.values.has(ref) ? [OK, t.db.values.get(ref)] : [IO, null];
        },
        values(token, refs) {
            const out = [];
            for (const ref of refs) { const r = ops.value(token, ref); if (r[0]) return [r[0], []]; out.push(r[1]); }
            return [OK, out];
        },
        commit(token, bytes, refs, version) {
            const t = tx(token); if (!t) return IO;
            const status = write(t, bytes, Array.from(refs), version); if (status) return status;
            release(t); return OK;
        },
        save(token, bytes, refs, version) {
            const t = tx(token); if (!t) return IO;
            return write(t, bytes, Array.from(refs), version);
        },
        abort(token) { const t = tx(token); if (!t) return IO; release(t); return OK; },
        databases() {
            for (const [key, lease] of store.leases) if (!active.has(lease.token) || active.get(lease.token) !== lease) { stats.busy++; return [BUSY, []]; }
            const list = [];
            for (const d of store.databases.values()) if (d.origin === origin && d.meta !== null && d.version) list.push({name: d.name, version: d.version});
            return [OK, list];
        },
        connections(token, version) {
            const t = tx(token); if (!t) return [IO, 0, 0];
            let count = 0, unfired = 0, ghosts = 0;
            for (const row of t.db.rows) {
                if (!row.identity) { if (row.state === GHOST) { if (row.desired === version) ghosts++; else for (const k of Object.keys(row)) delete row[k]; } continue; }
                count++;
                if (row.state === FORCED) continue;
                if (row.state === NONE || row.desired !== version) { row.desired = version; row.state = MARKED; }
                if (row.state !== FIRED) unfired++;
            }
            count += ghosts;
            if (!unfired) for (const row of t.db.rows) if (!row.identity && row.state === GHOST && row.desired === version) for (const k of Object.keys(row)) delete row[k];
            return [OK, count, unfired];
        },
        connect(token, version) {
            const t = tx(token); if (!t) return [IO, 0];
            let row = t.db.rows.find(r => !r.identity && r.state !== GHOST);
            if (!row) { row = {}; t.db.rows.push(row); }
            Object.assign(row, {identity: ++store.clientSerial, version, desired: 0, state: NONE});
            const handle = ++serial; clients.set(handle, {row, identity: row.identity});
            return [OK, handle];
        },
        poll(connection) {
            const c = clients.get(connection >>> 0); if (!c || c.row.identity !== c.identity) return [IO, null, false];
            if (c.row.state === FORCED) { c.row.state = FIRED; c.row.desired = Infinity; return [OK, null, true]; }
            if (c.row.state === MARKED) { c.row.state = DELIVERED; return [OK, c.row.desired, false]; }
            return [OK, null, false];
        },
        fired(connection) {
            const c = clients.get(connection >>> 0); if (!c || c.row.identity !== c.identity) return IO;
            if (c.row.state === DELIVERED) c.row.state = FIRED; return OK;
        },
        disconnect(connection) {
            const c = clients.get(connection >>> 0); if (!c) return IO;
            if (c.row.identity === c.identity) {
                const ghost = c.row.state === FIRED, desired = c.row.desired;
                for (const k of Object.keys(c.row)) delete c.row[k];
                if (ghost) Object.assign(c.row, {desired, state: GHOST});
            }
            clients.delete(connection >>> 0); return OK;
        },
        /* The broker process ends (the realm retired): its leases and connections go with it. */
        end() { for (const t of [...active.values()]) release(t); for (const h of [...clients.keys()]) ops.disconnect(h); },
    };
    ops.forget = ops.disconnect;
    return ops;
}

/* ---- the realm ---- */
const runtime = read(web + 'js_worker_runtime.js');
const eventLines = expand(lines(runtime, '    const eventStates=new WeakMap()', '    Object.assign(globalThis,{DOMException,QuotaExceededError,Event,', 'worker events'));
const handlerLines = lines(runtime, '    let blobHandlerTarget=()=>false,indexedDBHandlerTarget', "if(type==='abort'&&had!==(map[type]!==null))abortBridge.listen(this,had?-1:1);}});}", 'worker handlers');
/* JS_GetClassID's distinctions, as js_clone.js reads them. */
const typedNames = ['Int8Array', 'Uint8Array', 'Uint8ClampedArray', 'Int16Array', 'Uint16Array', 'Int32Array', 'Uint32Array',
    'Float32Array', 'Float64Array', 'BigInt64Array', 'BigUint64Array', 'Float16Array'];
function classID(value) {
    if (value === null || typeof value !== 'object' && typeof value !== 'function') return 0;
    const t = util.types;
    if (t.isProxy(value)) return 90;
    if (typeof value === 'function') return 91;
    if (Array.isArray(value)) return 2;
    const kinds = [t.isDate, t.isRegExp, t.isMap, t.isSet, t.isArrayBuffer, t.isSharedArrayBuffer, t.isDataView, t.isBooleanObject,
        t.isNumberObject, t.isStringObject, t.isBigIntObject, t.isNativeError, t.isPromise, t.isWeakMap, t.isWeakSet, t.isSymbolObject,
        t.isArgumentsObject, t.isGeneratorObject, t.isMapIterator, t.isSetIterator, t.isModuleNamespaceObject];
    for (let i = 0; i < kinds.length; i++) if (kinds[i](value)) return 10 + i;
    if (t.isTypedArray(value)) { const name = Object.prototype.toString.call(value).slice(8, -1); const i = typedNames.indexOf(t['is' + name] && t['is' + name](value) ? name : ''); return i >= 0 ? 40 + i : 89; }
    return 1;
}
const realmSource = `(function(){
    'use strict';
    const apply=Reflect.apply,host=__host;
    ${read(web + 'js_dom_exception.js')}
    const DOMException=domExceptionBridge.DOMException,QuotaExceededError=domExceptionBridge.QuotaExceededError;
    /* AbortSignal is not run here: an abort listener's count is its garbage collection rule only. */
    const abortBridge={listen(){},signals:{is:()=>false,aborted:()=>false,follow:()=>()=>{}}};
    ${eventLines}
    const target=new EventTarget();
    let reporting=false;
    /* The Worker's report(): an ErrorEvent at the global, cancelable; not canceled, it is uncaught. */
    function report(e){if(reporting){__uncaught(e);return;}reporting=true;try{const event=new ErrorEvent('error',{message:String(e&&e.message||e),error:e,cancelable:true});eventState(event).isTrusted=true;if(dispatchWorkerEvent(target,event))__uncaught(e);}finally{reporting=false;}}
    ${handlerLines}
    globalThis.setTimeout=(fn,ms,...args)=>__timer(fn,ms,args);globalThis.clearTimeout=id=>__clear(id);
    globalThis.setInterval=(fn,ms,...args)=>__timer(fn,ms,args,true);globalThis.clearInterval=id=>__clear(id);
    ${expand(read(web + 'js_clone.js'))}
    /* The Worker's geometry interfaces, OffscreenCanvas and ImageData (no WebGL host here): the
       [Serializable] platform objects structured-clone.any.js stores. */
    ${read(web + 'js_geometry.js')}
    geometryInterfaces({});
    ${read(web + 'js_offscreen_canvas.js')}
    ${read(moduleFile)}
    /* node's vm calls a global accessor with the outer sandbox object as this: the realm's one
       IDBFactory, which the product's getter answers, becomes a value here. */
    {const d=Object.getOwnPropertyDescriptor(globalThis,'indexedDB');Object.defineProperty(globalThis,'indexedDB',{value:d.get.call(globalThis),configurable:true,enumerable:true});}
    Object.assign(globalThis,{self:globalThis,addEventListener:target.addEventListener.bind(target),removeEventListener:target.removeEventListener.bind(target),dispatchEvent:target.dispatchEvent.bind(target)});
    globalThis.__reportError=report;
    globalThis.__rejected=(reason)=>{const e=new PromiseRejectionEvent('unhandledrejection',{promise:Promise.resolve(),reason,cancelable:true});eventState(e).isTrusted=true;if(dispatchWorkerEvent(target,e))__uncaught(reason);};
    return indexedDBBridge;
})()`;
/* QuickJS's JS_WriteObject/JS_ReadObject with JS_WRITE_OBJ_REFERENCE: the graph's own data,
   shared references and cycles kept, made in the realm. */
const copySource = `(function copy(v,seen=new Map()){
    if(v===null||typeof v!=='object')return v;if(seen.has(v))return seen.get(v);let out;
    if(v instanceof ArrayBuffer){out=v.slice(0);seen.set(v,out);return out;}
    if(ArrayBuffer.isView(v)){const buffer=copy(v.buffer,seen);out=v instanceof DataView?new DataView(buffer,v.byteOffset,v.byteLength):new v.constructor(buffer,v.byteOffset,v.length);seen.set(v,out);return out;}
    if(v instanceof Date){out=new Date(v.getTime());seen.set(v,out);return out;}
    if(v instanceof RegExp){out=new RegExp(v.source,v.flags);seen.set(v,out);return out;}
    if(v instanceof Map){out=new Map();seen.set(v,out);for(const [k,x] of v)out.set(copy(k,seen),copy(x,seen));return out;}
    if(v instanceof Set){out=new Set();seen.set(v,out);for(const x of v)out.add(copy(x,seen));return out;}
    out=Array.isArray(v)?new Array(v.length):{};seen.set(v,out);
    for(const k of Object.keys(v))Object.defineProperty(out,k,{value:copy(v[k],seen),writable:true,enumerable:true,configurable:true});
    return out;})`;
const ORIGIN = 'http://web-platform.test:8000';
function makeRealm(file, store, text) {
    const stats = {busy: 0, ops: {}, uncaught: []};
    const c = {console: {log() {}, error() {}, warn() {}, info() {}, debug() {}}, TextEncoder, TextDecoder};
    vm.createContext(c);
    const url = ORIGIN + '/IndexedDB/' + file.replace(/\.js$/, '.worker.html'), location = new URL(url);
    c.location = {href: url, origin: location.origin, protocol: location.protocol, host: location.host, hostname: location.hostname,
        port: location.port, pathname: location.pathname, search: '', hash: '', toString: () => url};
    const title = /^\/\/ META: title=(.*)$/m.exec(text); if (title) c.META_TITLE = title[1].trim();
    const timers = new Map();let destroyed = false, taskEnd = null;
    const run = (fn, args) => { try { fn(...args); } catch (e) { c.__reportError(e); } };
    c.__timer = (fn, ms, args, repeat) => {
        if (destroyed || typeof fn !== 'function') return 0;
        const id = loop.seq + 1;
        const fire = () => { if (destroyed || !timers.has(id)) return; if (repeat) timers.set(id, schedule(ms, fire, 'timer')); else timers.delete(id); run(fn, args); };
        timers.set(id, schedule(ms, fire, 'timer'));
        return id;
    };
    c.__clear = id => { timers.delete(id); };
    c.__uncaught = e => stats.uncaught.push(e);
    c.__moduleFile = moduleFile;
    const native = broker(store, ORIGIN, stats), fifo = [];let busy = false;
    const copy = vm.runInContext(copySource, c), RealmPromise = vm.runInContext('Promise', c), RealmTypeError = vm.runInContext('TypeError', c);
    function next() {
        if (!fifo.length) { busy = false; return; }
        busy = true;
        schedule(1, () => {
            const q = fifo.shift();
            if (destroyed) return;
            const result = native[q.op](...q.args);
            stats.ops[q.op] = (stats.ops[q.op] || 0) + 1;
            q.resolve(result);
            next();
        }, 'idb ' + fifo[0].op);
    }
    c.__host = {
        now: () => loop.now,
        indexedDB(op, ...args) {
            if (!(op in native) || op === 'end') throw new RealmTypeError('Invalid IndexedDB operation');
            let resolve; const promise = new RealmPromise(ok => { resolve = ok; });
            fifo.push({op, args, resolve}); if (!busy) next();
            return promise;
        },
        indexedDBStorageKey: () => true,
        pack: value => Object.freeze({__packed: copy(value)}),
        unpack: bytes => copy(bytes.__packed),
        classID, bufferTransferable: () => true,
        detach: buffer => { structuredClone(buffer, {transfer: [buffer]}); },
    };
    const bridge = vm.runInContext(realmSource, c, {filename: 'js_worker_runtime.js (events, handlers, js_clone.js) + ' + moduleFile});
    taskEnd = () => { if (!destroyed) bridge.taskEnd(); };
    return {c, stats, taskEnd, destroy() { if (destroyed) return; try { bridge.retire(); } catch (_) {} destroyed = true; native.end(); }};
}

/* ---- running one file ---- */
const testharness = read(path.join(wpt, 'resources/testharness.js'));
const statusName = ['PASS', 'FAIL', 'TIMEOUT', 'NOTRUN', 'PRECONDITION_FAILED'];
let currentRealm = null;
process.on('unhandledRejection', reason => { if (currentRealm) try { currentRealm.c.__rejected(reason); } catch (_) {} });
async function runFile(file, store) {
    const full = path.join(wpt, 'IndexedDB', file), text = read(full), results = new Map();
    const realm = makeRealm(file, store, text), c = realm.c;currentRealm = realm;
    const started = loop.now, startedTasks = loop.tasks, wall = Date.now();let finished = false, harness = null;
    c.__done = (tests, status) => {
        finished = true;harness = status;
        for (const t of tests) results.set(t.name, statusName[t.status] + (t.status && verbose && t.message ? ' ' + t.message.split('\n')[0] : ''));
    };
    const note = (key, message) => results.set(key, message);
    try {
        vm.runInContext(testharness, c, {filename: 'testharness.js'});
        vm.runInContext('add_completion_callback((tests,status)=>__done(tests,status));', c);
        for (const line of text.split('\n')) {
            const m = /^\/\/ META: script=(.*)$/.exec(line.trim()); if (!m) continue;
            const script = m[1].startsWith('/') ? path.join(wpt, m[1]) : path.join(path.dirname(full), m[1]);
            vm.runInContext(read(script), c, {filename: m[1]});
        }
        vm.runInContext(text, c, {filename: file});
    } catch (e) { note('(load) ' + file, 'ERROR ' + (e && e.message)); }
    try { vm.runInContext('done()', c); } catch (e) { note('(done) ' + file, 'ERROR ' + (e && e.message)); }
    const limit = /META: timeout=long/.test(text) ? 60000 : 10000;
    let timedOut = false;
    schedule(limit, () => { if (!finished) { timedOut = true; vm.runInContext('timeout()', c); } }, 'harness timeout');
    /* The file's script is the first task: its microtasks, then its end. */
    await drained();realm.taskEnd();
    let idle = false, wallOut = false;
    while (!finished) {
        const task = loop.queue.shift();
        if (!task) { idle = true; vm.runInContext('timeout()', c); break; }
        /* A file that spins this file's loop past 30 s of real time is stopped as timed out. */
        if (Date.now() - wall > 30000 && !wallOut) { wallOut = true; loop.queue.unshift(task); vm.runInContext('timeout()', c); continue; }
        if (wallOut && Date.now() - wall > 35000) break;
        /* Each task costs 0.1 ms of virtual time: a loop of zero-delay tasks reaches the timeout. */
        loop.now = Math.max(loop.now + 0.1, task.due); loop.tasks++;
        try { task.fn(); } catch (e) { note('(task) ' + (e && e.message), 'ERROR'); }
        await drained();
        realm.taskEnd();
    }
    await drained();
    realm.destroy();currentRealm = null;
    /* What remains of this file's tasks goes with its realm. */
    loop.queue.length = 0;
    if (process.env.IDB_DEBUG) console.log('harness', harness && harness.status, harness && harness.message);
    if (harness && harness.status !== 0 && harness.status !== 2) note('(harness) ' + file, 'ERROR ' + harness.message);
    for (const e of realm.stats.uncaught.slice(0, 3)) note('(uncaught) ' + String(e && e.stack || e).split('\n')[0], 'ERROR');
    return {results, virtual: loop.now - started, wall: Date.now() - wall, timedOut: timedOut || wallOut, wallOut, idle, stats: realm.stats, tasks: loop.tasks - startedTasks};
}
const targets = process.argv.slice(2).filter(a => !a.startsWith('--'));
async function main() {
    const dir = path.join(wpt, 'IndexedDB');
    let files = targets.length ? targets : fs.readdirSync(dir).filter(f => f.endsWith('.any.js')).sort();
    if (only) files = files.filter(f => f.includes(only));
    let pass = 0, total = 0;const lines = [];
    for (const file of files) {
        const store = disk(), r = await runFile(file, store);
        const subs = [...r.results].filter(([k]) => !k.startsWith('('));
        const p = subs.filter(([, s]) => s.startsWith('PASS')).length;
        pass += p; total += subs.length;
        const flag = r.idle ? ' IDLE' : r.wallOut ? ' TIMEOUT(real 30 s)' : r.timedOut ? ' TIMEOUT' : '';
        lines.push(`${file}: ${p}/${subs.length}${flag} (virtual ${Math.round(r.virtual)} ms, tasks ${r.tasks}, busy ${r.stats.busy}, broker ${Object.values(r.stats.ops).reduce((a, b) => a + b, 0)})`);
        if (process.env.IDB_PROGRESS) console.error(lines[lines.length - 1], (r.wall / 1000).toFixed(1) + ' s');
        if (verbose || targets.length || only) for (const [name, s] of r.results) if (!s.startsWith('PASS')) lines.push(`    ${s}: ${name}`);
        if ((r.timedOut || r.idle) && (verbose || targets.length || only)) lines.push('    ops ' + JSON.stringify(r.stats.ops));
    }
    for (const l of lines) console.log(l);
    console.log(`IndexedDB WPT (${moduleFile}): ${pass}/${total} subtests pass in ${files.length} files`);
}
main().catch(e => { console.error(e); process.exit(1); });
