/* XMLHttpRequest send()'s body extraction as the Window's Fetch owns it (js_fetch.js xhrBody,
 * called from js_xhr.js send()), in the node vm Window realm of tests/fetch_wpt_contract.cjs.
 * send() takes an optional (Document or XMLHttpRequestBodyInit)? body: null is no body, unlike
 * BodyInit's conversion that would make it the string "null"
 * (https://xhr.spec.whatwg.org/#the-send()-method). This is not the guest browser: the guest's
 * slop8/xhr-regression case "native null/empty body presence" sends them over the native path.
 * Run: node tests/xhr_body_contract.cjs */
'use strict';
const vm = require('node:vm'), assert = require('node:assert/strict');
const {realms} = require('./fetch_wpt_contract.cjs');
const c = realms.window();
const body = source => JSON.parse(vm.runInContext(`(()=>{const e=fetchBridge.xhrBody(${source});
    return JSON.stringify({bytes:e.bytes===null?null:e.bytes.byteLength,type:e.type,binary:e.binary});})()`, c));
let failed = 0, passed = 0;
function check(name, fn) {
    try { fn(); passed++; } catch (e) { failed++; console.log(`FAIL ${name}: ${e.message}`); }
}
check('null is no body', () => assert.deepEqual(body('null'), {bytes: null, type: null, binary: false}));
check('the empty string is an empty text body', () =>
    assert.deepEqual(body("''"), {bytes: 0, type: 'text/plain;charset=UTF-8', binary: false}));
check('an empty ArrayBuffer is an empty binary body without a type', () =>
    assert.deepEqual(body('new ArrayBuffer(0)'), {bytes: 0, type: null, binary: true}));
check('a URLSearchParams body is form-urlencoded', () =>
    assert.deepEqual(body("new URLSearchParams('a=b')"),
                     {bytes: 3, type: 'application/x-www-form-urlencoded;charset=UTF-8', binary: false}));
check('another object converts to a string', () =>
    assert.deepEqual(body('({toString(){return "abc";}})'), {bytes: 3, type: 'text/plain;charset=UTF-8', binary: false}));
console.log(`xhr body contract: ${passed} passed, ${failed} failed`);
if (failed) process.exit(1);
