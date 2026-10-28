/* CSS Font Loading's realm half (js_font_loading.js) against a host.fonts that
   stands in for css.c and request_fonts: statuses, [[LoadingFonts]], ready,
   the environment, events and setlike behaviour. The native grammar, font
   matching and loads are not exercised here. Run from the repository root. */
'use strict';
const fs = require('node:fs'), crypto = require('node:crypto');
const source = fs.readFileSync('user/libc/web/js_font_loading.js', 'utf8');
console.log('source_sha256=' + crypto.createHash('sha256').update(source).digest('hex'));
const tasks = [];
const log = [];
let pending = false, stuck = false;
const records = new Map(); let serial = 100;
const docA = {nodeType: 9, name: 'main'};
const rules = new Map([[docA, []]]);
const authors = new Map([[docA, []]]);
const native = (op, ...a) => {
    switch (op) {
    case 'create': { const r = {id: ++serial, family: null, started: false, state: 1, sources: '', data: false}; records.set(r.id, r); return r; }
    case 'id': return a[0].id;
    case 'activate': return;
    case 'set': { const [rec, which, value] = a; if (value === 'invalid') return null;
        if (which === 0) { const v = /^[A-Za-z][A-Za-z0-9]*( [A-Za-z][A-Za-z0-9]*)*$/.test(value) && value !== 'sans-serif' ? value : JSON.stringify(value); if (rec) rec.family = value; return v; }
        return value; }
    case 'source': { if (a[1] === 'bad') return false; a[0].sources = a[1]; return true; }
    case 'data': { a[0].data = a[1].byteLength >= 12; return a[0].data; }
    case 'load': { a[0].started = true; return a[0].sources.includes('local(') && !a[0].sources.includes('url(') ? 3 : a[0].state; }
    case 'status': return a[0].started ? a[0].state : 0;
    case 'add': authors.get(a[0]).push(a[1]); return;
    case 'delete': { const l = authors.get(a[0]); l.splice(l.indexOf(a[1]), 1); return; }
    case 'clear': authors.set(a[0], []); return;
    case 'rules': return rules.get(a[0]).flatMap(r => [r.id, 0, r.status]);
    case 'rule': { const r = rules.get(a[0]).find(r => r.id === a[1]); return r ? r.desc.slice() : null; }
    case 'ruleSet': { const r = rules.get(a[0]).find(r => r.id === a[1]); if (!r || a[4] === 'invalid') return null; r.desc[a[3]] = a[4]; return a[4]; }
    case 'ruleLoad': { const r = rules.get(a[0]).find(r => r.id === a[1]); if (!r) return null; if (r.status === 0) r.status = 1; return r.status; }
    case 'match': { const [doc, font] = a; if (/var\(|inherit|initial/.test(font)) return null; const out = [];
        for (const r of rules.get(doc)) if (font.includes(r.desc[0])) out.push(r.id, 0, 0);
        for (const rec of authors.get(doc)) if (rec.family && font.includes(rec.family)) out.push(rec.id, 0, rec.id);
        return out; }
    case 'pending': return pending;
    case 'stuck': stuck = !!a[0]; return;
    }
    throw new Error('op ' + op);
};
const host = {fonts: native, postTask(fn) { tasks.push(fn); return tasks.length; }};
const eventStates = new WeakMap();
class Event { constructor(type, init) { this.type = type; eventStates.set(this, {isTrusted: false}); } }
const listeners = new WeakMap();
class EventTarget {
    addEventListener(type, fn) { let m = listeners.get(this); if (!m) listeners.set(this, m = []); m.push({type, fn}); }
}
function eventState(e) { return eventStates.get(e); }
function dispatch(target, event) { log.push(event.type + ':' + event.fontfaces.length); for (const l of listeners.get(target) || []) if (l.type === event.type) l.fn.call(target, event); }
const handlers = new WeakMap();
function handlerValue(t, type) { return (handlers.get(t) || {})[type] ?? null; }
function setHandler(t, type, v) { let h = handlers.get(t); if (!h) handlers.set(t, h = {}); h[type] = v; }
class DOMException extends Error { constructor(m, name) { super(m); this.name = name; } }
function Document() {}
Object.setPrototypeOf(docA, Document.prototype);
const document = docA;
const rawDom = {isNode: (_, v) => v === docA, get: (v, k) => v[k]};
function report(e) { console.log('report', e); }
const run = new Function('host', 'Event', 'EventTarget', 'eventState', 'dispatch', 'handlerValue', 'setHandler', 'DOMException', 'Document', 'document', 'rawDom', 'report', 'globalThis',
    /* js_bootstrap.js declares the handler brand the module assigns. */
    'let fontHandlerTarget = () => false;' + source + '\nreturn {bridge: fontLoadingBridge, target: () => fontHandlerTarget};');
const g = {};
const exported = run(host, Event, EventTarget, eventState, dispatch, handlerValue, setHandler, DOMException, Document, document, rawDom, report, g);
const {FontFace, FontFaceSet, FontFaceSetLoadEvent} = g;
const bridge = exported.bridge;
async function drain() { for (let i = 0; i < 50; i++) { await null; while (tasks.length) { tasks.shift()(); await null; } } }
let failures = 0;
function check(ok, name) { if (!ok) failures++; console.log((ok ? 'ok   ' : 'FAIL ') + name); }
(async () => {
    // A rule in the document, loading through rendering.
    rules.get(docA).push({id: 1, status: 1, desc: ['WebFont', 'normal', 'normal', 'normal', 'U+0-10FFFF', 'normal', 'normal', 'auto', 'normal', 'normal', 'normal', '100%']});
    pending = true;
    const fonts = document.fonts;
    check(fonts === document.fonts, 'same set');
    check(fonts.size === 1 && fonts.status === 'loading', 'created loading with the rule');
    const ready1 = fonts.ready;
    let readyDone = false; ready1.then(() => { readyDone = true; });
    const [rule] = [...fonts];
    check(rule instanceof FontFace && rule.family === 'WebFont' && rule.status === 'loading', 'CSS-connected face');
    let threw = null; try { fonts.add(rule); } catch (e) { threw = e; }
    check(threw === null, 'add of a present CSS face returns');
    check(fonts.delete(rule) === false && fonts.size === 1, 'delete CSS-connected is false');
    fonts.clear(); check(fonts.size === 1, 'clear keeps CSS-connected');
    // clear() emptied LoadingFonts: switch to loaded, but the environment is pending.
    check(stuck === true && fonts.status === 'loading', 'stuck on the environment');
    rules.get(docA)[0].status = 2;
    bridge.changed(); await drain();
    check(rule.status === 'loaded', 'rule loaded');
    check(!readyDone, 'still pending on the environment');
    pending = false; bridge.changed(); await drain();
    check(readyDone && fonts.status === 'loaded' && stuck === false, 'ready after the environment');
    // fontface-fonts-loading
    const face = new FontFace('AhemTest2', 'url(/fonts/Ahem.ttf)');
    check(face.status === 'unloaded' && face.family === 'AhemTest2' && face.weight === 'normal', 'constructed');
    fonts.add(face);
    const p = face.load();
    const ready2 = fonts.ready;
    check(ready1 !== ready2 && fonts.status === 'loading', 'new ready promise');
    let loadingFired = 0; fonts.addEventListener('loading', () => loadingFired++);
    let doneFaces = null; fonts.addEventListener('loadingdone', e => { doneFaces = e.fontfaces; });
    await drain();
    check(loadingFired === 1, 'loading event');
    [...records.values()].pop().state = 2; /* face is the newest record */
    bridge.changed(); await drain();
    check(face.status === 'loaded', 'face loaded');
    check(doneFaces && doneFaces.length === 1 && doneFaces[0] === face && Object.isFrozen(doneFaces), 'loadingdone with the face');
    check((await p) === face && (await fonts.ready) === fonts, 'promises');
    // fontfaceset-has / delete
    const f2 = new FontFace('WebFont2', "url('x.woff2') format('woff2')");
    check(!fonts.has(f2), 'not has'); fonts.add(f2); check(fonts.has(f2) && fonts.size === 3, 'has');
    rules.get(docA).length = 0; check(!fonts.has(rule) && fonts.size === 2, 'rule removed from the set');
    check(fonts.delete(f2) && !fonts.has(f2), 'delete author');
    let threw2; try { fonts.add(1); } catch (e) { threw2 = e; } check(threw2 instanceof TypeError, 'add non-FontFace');
    // order: CSS-connected first
    rules.get(docA).push({id: 7, status: 0, desc: ['Later', 'normal', 'normal', 'normal', 'U+0-10FFFF', 'normal', 'normal', 'auto', 'normal', 'normal', 'normal', '100%']});
    const order = [...fonts.values()];
    check(order.length === 2 && order[0].family === 'Later' && order[1] === face, 'CSS-connected faces first');
    const seen = []; fonts.forEach(function (v, k, s) { seen.push(v === k && s === fonts); });
    check(seen.length === 2 && seen.every(Boolean), 'forEach value, value, set');
    check(fonts.keys === fonts.values && fonts[Symbol.iterator] === fonts.values, 'setlike aliases');
    // load() and check()
    let result = null; fonts.load('12px Later').then(r => { result = r; });
    await drain();
    check(result === null && order[0].status === 'loading' && rules.get(docA)[0].status === 1, 'load starts the rule');
    check(fonts.check('12px Later') === false, 'check while loading');
    rules.get(docA)[0].status = 2; bridge.changed(); await drain();
    check(result && result.length === 1 && result[0] === order[0] && order[0].status === 'loaded', 'load resolves with the face');
    let rejected = null; await fonts.load('var(--x) serif').catch(e => { rejected = e; });
    check(rejected && rejected.name === 'SyntaxError', 'var() is a SyntaxError');
    let threw3; try { fonts.check('inherit'); } catch (e) { threw3 = e; } check(threw3 && threw3.name === 'SyntaxError', 'check SyntaxError');
    check(fonts.check('12px AhemTest2') === true, 'check loaded face');
    check(fonts.check('12px Nothing') === true, 'check with no faces');
    // descriptors
    const w = new FontFace('w', 'url(a)', {stretch: 'condensed', width: 'expanded'});
    check(w.width === 'expanded' && w.stretch === 'expanded', 'width wins');
    w.stretch = '125%'; check(w.width === '125%', 'aliases');
    let threw4; try { w.width = 'invalid'; } catch (e) { threw4 = e; } check(threw4 && threw4.name === 'SyntaxError' && w.width === '125%', 'invalid setter');
    const bad = new FontFace('b', 'url(a)', {weight: 'invalid'});
    check(bad.status === 'error' && bad.weight === '' && bad.style === 'normal', 'invalid descriptor');
    let badReason; await bad.loaded.catch(e => { badReason = e; }); check(badReason && badReason.name === 'SyntaxError', 'SyntaxError rejection');
    const local = new FontFace('l', 'local(x)'); fonts.add(local);
    let localReason; local.load().catch(e => { localReason = e; }); await drain();
    check(local.status === 'error' && localReason && localReason.name === 'NetworkError', 'local() fails with NetworkError');
    // ArrayBuffer source
    const data = new FontFace('d', new Uint8Array(16)); fonts.add(data);
    check(data.status === 'unloaded', 'data starts unloaded'); await drain();
    check(data.status === 'loaded', 'data decoded');
    const bogus = new FontFace('e', new ArrayBuffer(8)); fonts.add(bogus);
    let bogusReason; bogus.loaded.catch(e => { bogusReason = e; }); await drain();
    check(bogus.status === 'error' && bogusReason && bogusReason.name === 'SyntaxError', 'bogus data');
    // events
    const ev = new FontFaceSetLoadEvent('type'); check(Array.isArray(ev.fontfaces) && ev.fontfaces.length === 0 && Object.isFrozen(ev.fontfaces), 'event default');
    const ff = [face]; const ev2 = new FontFaceSetLoadEvent('type', {fontfaces: ff});
    check(ev2.fontfaces !== ff && ev2.fontfaces[0] === face && ev2.fontfaces === ev2.fontfaces, 'event copies');
    let threw5; try { new FontFaceSet([]); } catch (e) { threw5 = e; } check(threw5 instanceof TypeError, 'no constructor');
    check(exported.target()(fonts, 'loadingdone') && !exported.target()(fonts, 'click'), 'handler target');
    console.log('log', log.join(' '));
    console.log(failures + ' failures');
    process.exitCode = failures ? 1 : 0;
})().catch(e => { console.log('THREW', e); process.exitCode = 1; });
