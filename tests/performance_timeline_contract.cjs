/* The production js_performance.js against a stub of its native host (js.c's host.now,
 * host.timing, host.navigationTiming and the resourceTiming / paintTiming /
 * navigationTimingQueued hooks). It checks the timeline's own steps, not native timing:
 * - every Server-Timing case of WPT server-timing/resources/parsing (0..84): the header
 *   lines of each .sub.headers joined as one field value, against its expected metrics;
 * - Resource Timing's buffer limit, secondary buffer, resourcetimingbufferfull and the
 *   observers' droppedEntriesCount (as performance-timeline/droppedentriescount.any.js and
 *   resource-timing/buffer-full-*.html sequence them);
 * - the Web IDL shape of each entry interface from third_party/webref/idl: its attributes
 *   and the [Default] toJSON members, in order;
 * - supportedEntryTypes of a Window and a Worker realm, the navigation entry and its queueing,
 *   paint entries, the legacy PerformanceTiming and User Timing's navigation names.
 * Usage: node tests/performance_timeline_contract.cjs [wpt directory]. Not the guest. */
'use strict';
const fs = require('node:fs'), vm = require('node:vm'), assert = require('node:assert/strict'), path = require('node:path');
const root = path.join(__dirname, '..');
const wpt = process.argv[2] || 'D:/Programes/cloned repos/wpt';
const source = fs.readFileSync(path.join(root, 'user/libc/web/js_performance.js'), 'utf8');
let checks = 0;
const check = (name, yes) => { checks++; assert(yes, name); };
const same = (name, a, b) => { checks++; assert(Object.is(a, b), `${name}: ${String(a)} !== ${String(b)}`); };
const tick = () => new Promise(resolve => setTimeout(resolve, 5));

/* One realm: window has host.timing (the document's milestones, relative ms or null). */
function realm({ window = true, navigation = null, milestones = {} } = {}) {
    let clock = 50;
    const host = { now: () => clock, url: () => 'https://a.test/page', visible: () => true,
        timer: (_, fn, ms) => setTimeout(fn, ms) };
    if (window) {
        host.timing = name => name === 'navigationStart' ? 0 : name in milestones ? milestones[name] : null;
        host.navigationTiming = () => navigation;
    }
    const reported = [];
    const context = { console, setTimeout, Event, EventTarget, DOMException, structuredClone, host,
        report: e => reported.push(e),
        cloneData: { serialize: x => structuredClone(x), deserialize: x => structuredClone(x), registerUncloneable() {} } };
    vm.createContext(context);
    context.bridge = vm.runInContext(source + '\nperformanceBridge;', context);
    context.advance = ms => { clock += ms; };
    context.reported = reported;
    return context;
}
/* A transport record as js.c timing_value gives it (relative times). */
function record(fields = {}) {
    return Object.assign({ startTime: 10, fetchStart: 10, responseEnd: 30, redirectStart: 0, redirectEnd: 0,
        domainLookupStart: 11, domainLookupEnd: 12, connectStart: 12, connectEnd: 14, secureConnectionStart: 13,
        requestStart: 15, firstInterimResponseStart: 0, finalResponseHeadersStart: 20, encodedBodySize: 100,
        decodedBodySize: 200, responseStatus: 200, nextHopProtocol: 'h2', contentEncoding: 'gzip',
        contentType: 'text/javascript', cacheState: '', serverTiming: '', timingAllowPassed: true }, fields);
}

/* ---- Web IDL: attributes per interface, partial interfaces and mixins folded in. */
function idlAttributes(files) {
    const own = new Map(), parent = new Map(), mixins = new Map(), includes = [];
    for (const file of files) {
        const text = fs.readFileSync(path.join(root, 'third_party/webref/idl', file), 'utf8').replace(/\/\/.*$/mg, '');
        for (const m of text.matchAll(/(partial\s+)?interface\s+(mixin\s+)?(\w+)\s*(?::\s*(\w+))?\s*\{([^}]*)\}/g)) {
            const [, , mixin, name, base, body] = m;
            const target = mixin ? mixins : own;
            if (!target.has(name)) target.set(name, { attrs: [], toJSON: false });
            if (base) parent.set(name, base);
            for (const a of body.matchAll(/(?:^|;)\s*(?:\[[^\]]*\]\s*)?(?:readonly\s+)?attribute\s+([\w<>?,\s]+?)\s+(\w+)\s*(?=;)/g))
                target.get(name).attrs.push({ type: a[1].replace(/\s+/g, ' '), name: a[2] });
            if (/\[Default\]\s*object\s+toJSON/.test(body)) target.get(name).toJSON = true;
        }
        for (const m of text.matchAll(/(\w+)\s+includes\s+(\w+)\s*;/g)) includes.push([m[1], m[2]]);
    }
    for (const [name, mixin] of includes) if (own.has(name) && mixins.has(mixin)) own.get(name).attrs.push(...mixins.get(mixin).attrs);
    return { own, parent };
}
function idlChecks() {
    const { own, parent } = idlAttributes(['performance-timeline.idl', 'user-timing.idl', 'resource-timing.idl',
        'server-timing.idl', 'navigation-timing.idl', 'paint-timing.idl', 'hr-time.idl']);
    const r = realm({ navigation: Object.assign(record(), { type: 'navigate', redirectCount: 0, legacyRedirectCount: 0 }),
        milestones: { loadEventEnd: 90 } });
    const g = r;
    r.bridge.resource(1, 0, 'https://a.test/script.js', record({ serverTiming: 'a;dur=1;desc=x' }));
    r.bridge.paint('first-paint', 12);
    const performance = vm.runInContext('performance', r);
    const instances = {
        PerformanceMark: performance.mark('m'), PerformanceMeasure: performance.measure('n'),
        PerformanceResourceTiming: performance.getEntriesByType('resource')[0],
        PerformanceNavigationTiming: performance.getEntriesByType('navigation')[0],
        PerformancePaintTiming: performance.getEntriesByType('paint')[0],
        PerformanceServerTiming: performance.getEntriesByType('resource')[0].serverTiming[0],
        PerformanceTiming: performance.timing, PerformanceNavigation: performance.navigation, Performance: performance,
    };
    // Members this engine does not have yet are listed, so a new one is not missed silently.
    const absent = new Set(['PerformanceNavigationTiming.notRestoredReasons', 'PerformanceNavigationTiming.confidence']);
    for (const [name, value] of Object.entries(instances)) {
        const ctor = g[name];
        check(`${name} is exposed`, typeof ctor === 'function' && value instanceof ctor);
        // The interface's own and inherited attributes exist as accessors on the prototype chain.
        const chain = [];
        for (let i = name; i; i = parent.get(i)) chain.unshift(i);
        for (const iface of chain) for (const attr of own.get(iface)?.attrs || []) {
            const key = `${iface}.${attr.name}`;
            if (absent.has(key)) { same(`${key} not yet present`, attr.name in value, false); continue; }
            check(`${key} is an attribute`, attr.name in value);
            const proto = g[iface].prototype, d = Object.getOwnPropertyDescriptor(proto, attr.name) ||
                Object.getOwnPropertyDescriptor(Object.getPrototypeOf(proto), attr.name);
            if (iface === 'Performance' && (attr.name === 'onresourcetimingbufferfull')) continue;
            check(`${key} is a getter`, d && typeof d.get === 'function');
        }
        if (name === 'Performance') continue;
        // [Default] toJSON: the attributes of each interface in the chain that declares it, base first.
        const members = [];
        for (const iface of chain) if (own.get(iface)?.toJSON)
            for (const attr of own.get(iface).attrs) if (!absent.has(`${iface}.${attr.name}`) && attr.name !== 'onresourcetimingbufferfull') members.push(attr.name);
        const json = value.toJSON();
        same(`${name} toJSON members`, Object.keys(json).join(','), members.join(','));
        for (const key of members) {
            const expected = value[key];
            if (key === 'serverTiming') same(`${name}.toJSON().serverTiming`, json[key], expected);
            else same(`${name}.toJSON().${key}`, json[key], expected);
        }
    }
    // PerformanceServerTiming in a resource's JSON is its own [Default] JSON.
    same('serverTiming in JSON', JSON.stringify(instances.PerformanceResourceTiming.toJSON().serverTiming),
        '[{"name":"a","duration":1,"description":"x"}]');
    // PerformanceMark has no toJSON of its own: detail is not a member.
    same('mark JSON without detail', Object.prototype.hasOwnProperty.call(performance.mark('d', { detail: 1 }).toJSON(), 'detail'), false);
    same('Performance toJSON', Object.keys(performance.toJSON()).join(','), 'timeOrigin,timing,navigation');
}

/* ---- Server-Timing: WPT's parsing cases. */
function serverTimingChecks() {
    const dir = path.join(wpt, 'server-timing/resources/parsing');
    let cases = 0;
    for (let i = 0; i <= 84; i++) {
        const script = fs.readFileSync(path.join(dir, `${i}.js`), 'utf8');
        const expected = JSON.parse(script.slice(script.indexOf('[')).replace(/\)\s*;?\s*$/, ''));
        const headers = fs.readFileSync(path.join(dir, `${i}.js.sub.headers`), 'latin1');
        assert(!headers.includes('{{'), `${i}: a template substitution`);
        // wptserve: one header per line, name and value stripped; repeated fields join with ", ".
        const values = headers.split(/\r?\n/).filter(Boolean).map(line => {
            const at = line.indexOf(':');
            return [line.slice(0, at).trim(), line.slice(at + 1).trim()];
        }).filter(([name]) => name.toLowerCase() === 'server-timing').map(([, value]) => value);
        const r = realm(), url = `https://a.test/parsing/${i}.js`;
        r.bridge.resource(i, 0, url, record({ serverTiming: values.join(', ') }));
        const entry = vm.runInContext('performance', r).getEntriesByName(url)[0];
        const metrics = entry.serverTiming;
        same(`${i}.js count`, metrics.length, expected.length);
        same(`${i}.js frozen`, Object.isFrozen(metrics), true);
        same(`${i}.js SameObject`, entry.serverTiming, metrics);
        expected.forEach((want, k) => {
            same(`${i}.js [${k}] name`, metrics[k].name, want.name);
            same(`${i}.js [${k}] duration`, metrics[k].duration, want.dur || 0);
            same(`${i}.js [${k}] description`, metrics[k].description, want.desc || '');
        });
        cases++;
    }
    return cases;
}

/* ---- Resource Timing's buffer and the dropped entries count. */
async function bufferChecks() {
    const r = realm(), performance = vm.runInContext('performance', r);
    let n = 0;
    const add = () => r.bridge.resource(++n, 0, `https://a.test/r${n}`, record());
    // droppedentriescount.any.js: a buffer of 0 drops each new entry.
    performance.setResourceTimingBufferSize(0);
    let resources = 0;
    new r.PerformanceObserver(() => resources++).observe({ type: 'resource' });
    add();
    same('buffer 0 keeps none', performance.getEntriesByType('resource').length, 0);
    await tick();
    same('observer still receives it', resources, 1);
    let options;
    new r.PerformanceObserver((list, observer, o) => { options = o; observer.disconnect(); }).observe({ type: 'mark' });
    performance.mark('test'); await tick();
    same('no dropped marks', options.droppedEntriesCount, 0);
    new r.PerformanceObserver((list, observer, o) => { options = o; observer.disconnect(); }).observe({ entryTypes: ['mark', 'resource'] });
    performance.mark('meow'); await tick();
    same('dropped across types', options.droppedEntriesCount, 1);
    new r.PerformanceObserver((list, observer, o) => { options = o; observer.disconnect(); }).observe({ type: 'resource', buffered: true });
    await tick();
    same('dropped counted for a later observer', options.droppedEntriesCount, 1);
    const seen = [];
    const first = new r.PerformanceObserver((list, observer, o) => { seen.push(o.droppedEntriesCount); if (seen.length === 1) add(); else observer.disconnect(); });
    first.observe({ type: 'resource' }); add(); await tick(); await tick();
    same('first callback has the count', seen[0], 2);
    same('later callbacks do not', seen[1], undefined);
    // buffer-full-*.html: the event when the buffer is full, and room made in its handler.
    const b = realm(), bp = vm.runInContext('performance', b);
    let m = 0;
    const more = () => b.bridge.resource(++m, 0, `https://a.test/b${m}`, record());
    bp.setResourceTimingBufferSize(2);
    let fired = 0;
    bp.onresourcetimingbufferfull = function (event) { fired++; same('event at performance', this, bp); same('event type', event.type, 'resourcetimingbufferfull'); bp.setResourceTimingBufferSize(4); };
    more(); more(); more();
    same('primary holds its limit', bp.getEntriesByType('resource').length, 2);
    await tick();
    same('bufferfull fired once', fired, 1);
    same('secondary moved after the handler made room', bp.getEntriesByType('resource').map(e => e.name).join(','), 'https://a.test/b1,https://a.test/b2,https://a.test/b3');
    let dropped;
    new b.PerformanceObserver((l, o, opt) => { dropped = opt.droppedEntriesCount; o.disconnect(); }).observe({ type: 'resource', buffered: true });
    await tick();
    same('a moved entry is not dropped', dropped, 0);
    // A handler that clears the buffer: the secondary entries move in one at a time,
    // the event again for each that still finds the buffer full.
    bp.setResourceTimingBufferSize(1); bp.clearResourceTimings(); more();
    bp.onresourcetimingbufferfull = () => { fired++; bp.clearResourceTimings(); };
    more(); more(); await tick();
    same('cleared in the handler', fired, 3);
    same('kept after clearing', bp.getEntriesByType('resource').map(e => e.name).join(','), 'https://a.test/b6');
    // No handler: the entries that do not fit are dropped and the event fires again later.
    bp.onresourcetimingbufferfull = null; let plain = 0; bp.addEventListener('resourcetimingbufferfull', () => plain++);
    more(); await tick(); more(); await tick();
    same('fires each time the buffer is full', plain, 2);
    same('nothing more kept', bp.getEntriesByType('resource').length, 1);
    // Web IDL unsigned long.
    bp.setResourceTimingBufferSize(-1); for (let i = 0; i < 3; i++) more();
    same('-1 is 4294967295', bp.getEntriesByType('resource').length, 4);
    let typeError = false; try { bp.setResourceTimingBufferSize(); } catch (e) { typeError = e instanceof vm.runInContext('TypeError', b); }
    check('setResourceTimingBufferSize requires its argument', typeError);
    same('no errors reported', r.reported.length + b.reported.length, 0);
}

/* ---- Resource entries: the record's values, TAO, and the getters' rules. */
function resourceChecks() {
    const r = realm(), performance = vm.runInContext('performance', r);
    r.bridge.initiator(7, 'fetch');
    r.bridge.resource(7, 0, 'https://a.test/x', record({ firstInterimResponseStart: 18 }));
    const e = performance.getEntriesByName('https://a.test/x')[0];
    same('initiator registered', e.initiatorType, 'fetch');
    same('startTime', e.startTime, 10); same('duration', e.duration, 20);
    same('responseStart is the first interim', e.responseStart, 18);
    same('transferSize', e.transferSize, 400);
    same('deliveryType', e.deliveryType, '');
    r.bridge.resource(8, 1, 'https://a.test/y', record({ cacheState: 'local' }));
    const local = performance.getEntriesByName('https://a.test/y')[0];
    same('local cache transfer', local.transferSize, 0); same('local cache delivery', local.deliveryType, 'cache');
    same('fallback initiator', local.initiatorType, 'fetch');
    r.bridge.resource(9, 0, 'https://b.test/z', record({ cacheState: 'validated' }));
    same('validated transfer', performance.getEntriesByName('https://b.test/z')[0].transferSize, 300);
    // A failed timing allow check (js.c gives the opaque record): no sizes, no details.
    r.bridge.resource(10, 0, 'https://c.test/w', record({ timingAllowPassed: false, encodedBodySize: 0, decodedBodySize: 0,
        domainLookupStart: 0, domainLookupEnd: 0, connectStart: 0, connectEnd: 0, secureConnectionStart: 0, requestStart: 0,
        finalResponseHeadersStart: 0, responseStatus: 0, contentType: '', contentEncoding: '', nextHopProtocol: '' }));
    const opaque = performance.getEntriesByName('https://c.test/w')[0];
    same('TAO failure transfer', opaque.transferSize, 0); same('TAO failure responseStart', opaque.responseStart, 0);
    same('no service worker', opaque.workerStart, 0);
}

/* ---- Navigation and paint entries, PerformanceTiming, User Timing names. */
async function navigationChecks() {
    const nav = Object.assign(record({ redirectStart: 2, redirectEnd: 5 }), { type: 'reload', redirectCount: 0, legacyRedirectCount: 0 });
    const r = realm({ navigation: nav, milestones: { responseEnd: 30, domInteractive: 40, loadEventEnd: 90 } });
    const performance = vm.runInContext('performance', r);
    const [entry] = performance.getEntriesByType('navigation');
    check('navigation entry in the buffer from the start', entry instanceof r.PerformanceNavigationTiming);
    same('navigation name', entry.name, 'https://a.test/page');
    same('navigation startTime', entry.startTime, 0); same('navigation duration', entry.duration, 90);
    same('navigation type', entry.type, 'reload'); same('initiatorType', entry.initiatorType, 'navigation');
    same('no exposed redirects: no redirect times', entry.redirectStart, 0);
    same('navigationId is its id', entry.navigationId, entry.id);
    const [visible] = performance.getEntriesByType('visibility-state');
    same('the initial visibility entry follows the navigation', visible.navigationId, entry.id);
    check('ids increase', visible.id > entry.id && entry.id >= 100);
    same('getEntries order', performance.getEntries()[0], entry);
    same('PerformanceNavigation.type', performance.navigation.type, 1);
    same('legacy navigationStart', performance.timing.navigationStart, Math.floor(performance.timeOrigin));
    same('legacy unrecorded is 0', performance.timing.unloadEventStart, 0);
    same('legacy responseEnd', performance.timing.responseEnd, Math.floor(performance.timeOrigin + 30));
    // User Timing "convert a name to a timestamp".
    same('measure from responseEnd', performance.measure('a', 'responseEnd', 'domInteractive').duration, 10);
    let error; try { performance.measure('b', 'unloadEventStart'); } catch (e) { error = e; }
    same('a 0 attribute is InvalidAccessError', error && error.name, 'InvalidAccessError');
    let navigated = [];
    new r.PerformanceObserver(list => navigated.push(...list.getEntries())).observe({ type: 'navigation' });
    await tick();
    same('not queued before the load event', navigated.length, 0);
    r.bridge.navigationQueued(); await tick();
    same('queued after the load event', navigated[0], entry);
    same('queued once', performance.getEntriesByType('navigation').length, 1);
    r.bridge.navigationQueued(); await tick();
    same('a second queueing is ignored', navigated.length, 1);
    // A navigation with exposed redirects keeps its redirect times.
    const redirected = realm({ navigation: Object.assign(record({ redirectStart: 2, redirectEnd: 5 }), { type: 'navigate', redirectCount: 1, legacyRedirectCount: 1 }) });
    const re = vm.runInContext('performance', redirected).getEntriesByType('navigation')[0];
    same('redirectStart', re.redirectStart, 2); same('redirectEnd', re.redirectEnd, 5);
    same('legacy redirectCount', vm.runInContext('performance', redirected).navigation.redirectCount, 1);
    // Paint Timing.
    const painted = [];
    new r.PerformanceObserver(list => painted.push(...list.getEntries())).observe({ type: 'paint', buffered: true });
    r.bridge.paint('first-paint', 21.5); r.bridge.paint('first-contentful-paint', 21.5); r.bridge.paint('other', 1);
    await tick();
    same('paint entries', painted.map(e => e.name).join(','), 'first-paint,first-contentful-paint');
    same('paintTime', painted[1].paintTime, 21.5); same('presentationTime', painted[1].presentationTime, null);
    same('paint startTime', painted[0].startTime, 21.5); same('paint duration', painted[0].duration, 0);
    same('paint buffer', performance.getEntriesByType('paint').length, 2);
    // Supported types.
    same('window types', r.PerformanceObserver.supportedEntryTypes.join(','), 'mark,measure,navigation,paint,resource,visibility-state');
    check('frozen', Object.isFrozen(r.PerformanceObserver.supportedEntryTypes));
    const w = realm({ window: false });
    same('worker types', w.PerformanceObserver.supportedEntryTypes.join(','), 'mark,measure,resource');
    same('worker has no PerformanceTiming', typeof w.PerformanceTiming, 'undefined');
    same('worker timing', 'timing' in vm.runInContext('performance', w), false);
    same('worker mark navigationId', vm.runInContext("performance.mark('w').navigationId", w), 0);
    /* A Worker's own requests: the parent's records in monotonic times (host.now's clock, 50 when
       this realm began), converted with the Worker's time origin; zero stays zero. */
    const observed = [];
    new w.PerformanceObserver(list => observed.push(...list.getEntries())).observe({ type: 'resource' });
    vm.runInContext("Object.defineProperty(Object.prototype,'serverTiming',{configurable:true,get(){return 'evil;dur=1';}})", w);
    w.bridge.monotonicResource(0, 1, 'https://a.test/w', record({ startTime: 60, fetchStart: 61, responseEnd: 90, redirectStart: 0,
        requestStart: 70, secureConnectionStart: 0, serverTiming: undefined }));
    vm.runInContext("delete Object.prototype.serverTiming", w);
    const [own] = vm.runInContext("performance.getEntriesByType('resource')", w);
    same('worker resource entry', own && own.name, 'https://a.test/w');
    same('worker initiatorType', own.initiatorType, 'fetch');
    same('worker startTime', own.startTime, 10); same('worker fetchStart', own.fetchStart, 11);
    same('worker duration', own.duration, 30); same('worker requestStart', own.requestStart, 20);
    same('worker zero stays zero', own.redirectStart + own.secureConnectionStart, 0);
    same("worker record ignores Object.prototype", own.serverTiming.length, 0);
    same('worker importScripts initiator', (w.bridge.monotonicResource(0, 4, 'https://a.test/i', record({ startTime: 55 })),
        vm.runInContext("performance.getEntriesByName('https://a.test/i')[0].initiatorType", w)), 'other');
    w.bridge.monotonicResource(0, 1, 'https://a.test/x', null);
    same('worker without a record', vm.runInContext("performance.getEntriesByType('resource').length", w), 2);
    await tick();
    same('worker observer', observed.length, 2);
    same('worker clearResourceTimings', vm.runInContext('performance.clearResourceTimings(),performance.getEntriesByType("resource").length', w), 0);
    const windowRealm = realm();
    windowRealm.bridge.monotonicResource(0, 1, 'https://a.test/w', record());
    same('a window has no monotonic records', vm.runInContext("performance.getEntriesByType('resource').length", windowRealm), 0);
    const noNav = realm();
    same('no navigation record, no entry', vm.runInContext("performance.getEntriesByType('navigation').length", noNav), 0);
    same('constructed mark ids', vm.runInContext("JSON.stringify(new PerformanceMark('j',{startTime:1}))", noNav),
        '{"id":0,"name":"j","entryType":"mark","startTime":1,"duration":0,"navigationId":0}');
}

(async () => {
    idlChecks();
    const parsing = serverTimingChecks();
    await bufferChecks();
    resourceChecks();
    await navigationChecks();
    console.log(`performance timeline: ${checks} checks passed (${parsing} Server-Timing parsing cases)`);
})().catch(e => { console.error(e); process.exit(1); });
