/* Beacon's sendBeacon steps (https://w3c.github.io/beacon/#sec-processing-model) as the Window's
 * Fetch owns them (js_fetch.js beacon, reached from js_navigator.js's sendBeacon), in the node vm
 * Window realm of tests/fetch_wpt_contract.cjs: js_bootstrap.js's Fetch port, whose host.fetch is
 * a recorder. Here host.keepaliveRoom() answers from the keepalive bodies recorded so far, as the
 * native fetch group would while they are in flight. The cases are WPT beacon/beacon-basic's
 * (the 64 KiB bound, its accumulation, the stream TypeError) and the body's mode and
 * Content-Type. This is not the guest browser: the native accounting is webnet.c's.
 * Run: node tests/beacon_contract.cjs */
'use strict';
const fs = require('node:fs'), vm = require('node:vm'), assert = require('node:assert/strict');
const {realms} = require('./fetch_wpt_contract.cjs');
const c = realms.window();
vm.runInContext(`host.keepaliveRoom=()=>{let used=0;for(const a of calls)if(a[9]&&a[3]!=null)used+=typeof a[3]==='string'?new TextEncoder().encode(a[3]).byteLength:a[3].byteLength;return Math.max(0,65536-used);};`, c);
const run = source => vm.runInContext(source, c);
let failed = 0, passed = 0;
function check(name, fn) {
    try { fn(); passed++; } catch (e) { failed++; console.log(`FAIL ${name}: ${e.message}`); }
}
const last = () => run('calls[calls.length-1]');
const count = () => run('calls.length');
const header = (raw, name) => { const m = new RegExp('^' + name + ': (.*)$', 'mi').exec(raw); return m ? m[1].replace(/\r$/, '') : null; };

check('no payload: true, a credentialed keepalive no-cors POST without Content-Type', () => {
    assert.equal(run(`fetchBridge.beacon('/beacon/resources/beacon.py?cmd=store&id=1')`), true);
    const a = last();
    assert.equal(a[0], 'https://web-platform.test:8443/beacon/resources/beacon.py?cmd=store&id=1');
    assert.equal(a[1], 'POST');
    assert.equal(a[3], null);
    assert.equal(a[5], 2, 'credentials include');
    assert.equal(a[9], true, 'keepalive');
    assert.equal(a[10], true, 'no-cors');
    assert.equal(header(a[2], 'content-type'), null);
});
check('null payload: as no payload', () => {
    const before = count();
    assert.equal(run(`fetchBridge.beacon('/b', null)`), true);
    assert.equal(count(), before + 1);
    assert.equal(last()[3], null);
});
check('string payload: text/plain;charset=UTF-8, no-cors', () => {
    assert.equal(run(`fetchBridge.beacon('/b', 'hello')`), true);
    const a = last();
    assert.equal(header(a[2], 'content-type'), 'text/plain;charset=UTF-8');
    assert.equal(a[10], true);
});
check('URLSearchParams payload: form-urlencoded, no-cors', () => {
    assert.equal(run(`fetchBridge.beacon('/b', new URLSearchParams('a=b'))`), true);
    assert.equal(header(last()[2], 'content-type'), 'application/x-www-form-urlencoded;charset=UTF-8');
    assert.equal(last()[10], true);
});
check('ArrayBuffer payload: no Content-Type, no-cors', () => {
    assert.equal(run(`fetchBridge.beacon('/b', new ArrayBuffer(10))`), true);
    assert.equal(header(last()[2], 'content-type'), null);
    assert.equal(last()[10], true);
});
check('a non-HTTP(S) URL is a TypeError', () => {
    assert.throws(() => run(`fetchBridge.beacon('ftp://example.test/')`), e => e.name === 'TypeError');
    assert.throws(() => run(`fetchBridge.beacon('data:text/plain,x')`), e => e.name === 'TypeError');
});
check('an unparsable URL is a TypeError', () => {
    assert.throws(() => run(`fetchBridge.beacon('http://[')`), e => e.name === 'TypeError');
});
check('a stream payload is a TypeError (keepalive)', () => {
    assert.throws(() => run(`fetchBridge.beacon('/b', new ReadableStream())`), e => e.name === 'TypeError');
});
/* Each case below starts from a fetch group with nothing in flight. */
const reset = () => run('calls.length=0');
check('too large payload: 65537 bytes is false and sends nothing', () => {
    reset();
    assert.equal(run(`fetchBridge.beacon('/b', 'x'.repeat(65537))`), false);
    assert.equal(run(`fetchBridge.beacon('/b', new ArrayBuffer(65537))`), false);
    assert.equal(count(), 0);
});
check('payload size restriction is accumulated', () => {
    reset();
    assert.equal(run(`fetchBridge.beacon('/b', 'x'.repeat(65536))`), true);
    assert.equal(run(`fetchBridge.beacon('/b', '')`), true);
    assert.equal(run(`fetchBridge.beacon('/b', 'x')`), false);
    assert.equal(count(), 2);
});
check('ArrayBuffer accumulation', () => {
    reset();
    assert.equal(run(`fetchBridge.beacon('/b', new ArrayBuffer(65536))`), true);
    assert.equal(run(`fetchBridge.beacon('/b', new ArrayBuffer(1))`), false);
});
check('sendBeacon is the Window Fetch owner\'s steps, not the page\'s fetch()', () => {
    const navigator = fs.readFileSync('user/libc/web/js_navigator.js', 'utf8');
    const at = navigator.indexOf("'sendBeacon'");
    assert.ok(at >= 0);
    const body = navigator.slice(at, navigator.indexOf('}});', at));
    assert.match(body, /fetchBridge\.beacon\(url,data\)/);
    assert.doesNotMatch(body, /\bfetch\(/);
});
console.log(`beacon contract: ${passed} passed, ${failed} failed`);
if (failed) process.exit(1);
