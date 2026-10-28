/* The realm half of CSSOM View text geometry (js_range.js): Range getClientRects and
   getBoundingClientRect, Document caretPositionFromPoint (argument and dictionary
   conversion, shadow retargeting) and CaretPosition. A fake rawDom stands in for the native
   tree and the native rangeRects, caretPosition and caretRect operations, recording what the
   bindings ask of them; layout itself is tests/check_text_geometry_native.py's.
   Run from the repository root. */
'use strict';
const fs = require('node:fs'), vm = require('node:vm'), crypto = require('node:crypto');
const source = fs.readFileSync('user/libc/web/js_range.js', 'utf8');
console.log('source_sha256=' + crypto.createHash('sha256').update(source).digest('hex'));

let passed = 0, failed = 0;
function check(condition, name) { if (condition) passed++; else { failed++; console.log('FAIL ' + name); } }
function throws(fn, type, name) { try { fn(); check(false, name + ' (no exception)'); } catch (e) { check(e instanceof type || e.name === type.name, name + ' (' + e + ')'); } }

// A native tree: nodes are plain objects the fake rawDom recognises.
const nodes = new Set();
function make(nodeType, parent, extra) {
    const n = Object.assign({nodeType, parentNode: null, childNodes: [], nodeValue: null, shadowHost: null}, extra);
    nodes.add(n);
    if (parent) { n.parentNode = parent; parent.childNodes.push(n); }
    return n;
}
const document = make(9, null);
const html = make(1, document), container = make(1, html);
const textA = make(3, container, {nodeValue: 'a'});
const host = make(1, container);
const shadow = make(11, null, {shadowHost: host});
const span = make(1, shadow), shadowText = make(3, span, {nodeValue: 'hello'});
const outerHost = make(1, container);
const outer = make(11, null, {shadowHost: outerHost});
const spacer = make(3, outer, {nodeValue: ' '}), innerHost = make(1, outer);
const inner = make(11, null, {shadowHost: innerHost});
const innerText = make(3, inner, {nodeValue: 'some text'});
const input = make(1, container);
const lone = make(1, null); // a node whose root is not a shadow root

const calls = [];
let rectFixture = [], boundsFixture = {x: 1, y: 2, width: 3, height: 4};
let caretFixture = null, caretRectFixture = null;
const rawDom = {
    isNode: (_, v) => nodes.has(v),
    get: (n, key) => n[key],
    rangeRects(sc, so, ec, eo, fragments) { calls.push(['rangeRects', sc, so, ec, eo, fragments]); return fragments ? rectFixture : boundsFixture; },
    caretPosition(doc, x, y) { calls.push(['caretPosition', doc, x, y]); return caretFixture; },
    caretRect(node, offset) { calls.push(['caretRect', node, offset]); return caretRectFixture; },
};
class DOMRect { constructor(x, y, w, h) { this.x = x; this.y = y; this.width = w; this.height = h; } }
const observerBridge = {
    clientRects: values => values.map(v => new DOMRect(v[0], v[1], v[2], v[3])),
    clientBoundingRect: v => new DOMRect(v.x, v.y, v.width, v.height),
};
const documentBridge = {brand(v) { if (!nodes.has(v) || v.nodeType !== 9) throw new TypeError('Document receiver required'); return v; }};
function Document() {} function Node() {} function CharacterData() {} function Text() {}
Object.setPrototypeOf(document, Document.prototype);
class DOMException extends Error { constructor(m, name) { super(m); this.name = name; } }
const context = vm.createContext({rawDom, observerBridge, documentBridge, customElementsBridge: {reactions: f => f()},
    host: {}, document, Document, Node, CharacterData, Text, DOMException, DOMRect, WeakRef, console, apply: Reflect.apply});
vm.runInContext(source, context, {filename: 'js_range.js'});
const {Range, CaretPosition} = context;

// Range rectangles: the boundary points go to the native operation as they are.
const range = new Range();
rectFixture = [[10, 20, 30, 40], [0, 0, 0, 16]];
let list = range.getClientRects();
check(list.length === 2 && list[0].x === 10 && list[1].height === 16, 'getClientRects copies every native rectangle');
let call = calls.pop();
check(call[0] === 'rangeRects' && call[1] === document && call[2] === 0 && call[3] === document && call[4] === 0 && call[5] === true,
    'getClientRects asks for fragments between the boundary points');
range.setStart(shadowText, 1); range.setEnd(shadowText, 4);
calls.length = 0;
range.getClientRects();
call = calls.pop();
check(call[1] === shadowText && call[2] === 1 && call[3] === shadowText && call[4] === 4, 'the live boundary points');
range.setStart(document, 0); range.setEnd(document, 0);
calls.length = 0;
range.getBoundingClientRect();
check(calls.length === 1 && calls[0][0] === 'rangeRects' && calls[0][1] === document && calls[0][5] === false, 'getBoundingClientRect asks for the bounds');
let bounds = range.getBoundingClientRect();
check(bounds instanceof DOMRect && bounds.x === 1 && bounds.height === 4 && bounds !== range.getBoundingClientRect(), 'getBoundingClientRect is a new DOMRect');
throws(() => Range.prototype.getClientRects.call({}), TypeError, 'getClientRects brand');
throws(() => Range.prototype.getBoundingClientRect.call({}), TypeError, 'getBoundingClientRect brand');
const descriptor = Object.getOwnPropertyDescriptor(Range.prototype, 'getClientRects');
check(descriptor.enumerable && descriptor.writable && descriptor.configurable, 'getClientRects is an ordinary operation');

// caretPositionFromPoint: receiver, arguments and options as Web IDL converts them.
const at = (...args) => context.Document.prototype.caretPositionFromPoint.apply(document, args);
throws(() => context.Document.prototype.caretPositionFromPoint.call(textA, 1, 1), TypeError, 'Document receiver');
throws(() => at(), TypeError, 'no coordinates');
throws(() => at(5), TypeError, 'one coordinate');
throws(() => at('foo', 5), TypeError, 'a non-finite x');
throws(() => at(5, Infinity), TypeError, 'a non-finite y');
throws(() => at(5, 5, 'foo'), TypeError, 'options that are a string');
throws(() => at(5, 5, 6), TypeError, 'options that are a number');
throws(() => at(5, 5, {shadowRoots: 5}), TypeError, 'shadowRoots not a sequence');
throws(() => at(5, 5, {shadowRoots: {}}), TypeError, 'shadowRoots not iterable');
throws(() => at(5, 5, {shadowRoots: [host]}), TypeError, 'shadowRoots holding an element');
const order = [];
caretFixture = null;
check(at({valueOf() { order.push('x'); return 1; }}, {valueOf() { order.push('y'); return 2; }},
    {get shadowRoots() { order.push('options'); return []; }}) === null && order.join() === 'x,y,options', 'conversion order, then null');
calls.length = 0;
at(1.5, 2.25, undefined);
check(calls[0][0] === 'caretPosition' && calls[0][2] === 1.5 && calls[0][3] === 2.25, 'fractional coordinates reach the native position');
check(at(1, 1, null) === null && at(1, 1, {}) === null, 'null and empty options');

// Shadow retargeting (CSSOM View §5): out of every shadow tree no named root lies in.
caretFixture = [shadowText, 2];
let p = at(1, 1);
check(p instanceof CaretPosition && p.offsetNode === container && p.offset === 1, 'an unnamed shadow tree gives its host\'s parent');
p = at(1, 1, {shadowRoots: [shadow]});
check(p.offsetNode === shadowText && p.offset === 2, 'a named shadow tree keeps the position');
p = at(1, 1, {shadowRoots: new Set([shadow])});
check(p.offsetNode === shadowText, 'shadowRoots from any iterable');
caretFixture = [innerText, 2];
p = at(1, 1);
check(p.offsetNode === container && p.offset === 2, 'nested trees: the outer host\'s parent');
p = at(1, 1, {shadowRoots: [inner]});
check(p.offsetNode === innerText && p.offset === 2, 'the inner tree named');
p = at(1, 1, {shadowRoots: [outer]});
check(p.offsetNode === outer && p.offset === 1, 'the outer tree named: the inner host\'s index in it');
p = at(1, 1, {shadowRoots: [inner, outer]});
check(p.offsetNode === innerText, 'both trees named');
caretFixture = [textA, 1];
p = at(1, 1, {shadowRoots: [inner]});
check(p.offsetNode === textA && p.offset === 1, 'a position in the document tree stays');
caretFixture = [input, 0];
check(at(1, 1).offsetNode === input, 'a text entry widget is the caret node');

// CaretPosition: not constructible, a snapshot, and its rectangle.
throws(() => new CaretPosition(), TypeError, 'CaretPosition constructor');
throws(() => Object.getOwnPropertyDescriptor(CaretPosition.prototype, 'offset').get.call({}), TypeError, 'CaretPosition brand');
check(Object.prototype.toString.call(p) === '[object CaretPosition]', 'CaretPosition toStringTag');
caretFixture = [textA, 1];
p = at(1, 1);
caretRectFixture = {x: 7, y: 8, width: 0, height: 12};
calls.length = 0;
let rect = p.getClientRect();
check(rect instanceof DOMRect && rect.x === 7 && rect.height === 12 && calls.length === 1, 'a widget caret rectangle');
caretRectFixture = null;
calls.length = 0;
rect = p.getClientRect();
check(calls.length === 2 && calls[1][0] === 'rangeRects' && calls[1][1] === textA && calls[1][2] === 1 &&
    calls[1][3] === textA && calls[1][4] === 1 && calls[1][5] === false && rect.x === 1, 'else the collapsed range\'s bounding box');
check(p.getClientRect() !== p.getClientRect(), 'getClientRect is a new DOMRect each time');
caretFixture = [textA, 1];
const q = at(1, 1);
caretFixture = [textA, 0];
check(q.offset === 1, 'a caret position is not live');

console.log(`range geometry contract: ${passed} passed, ${failed} failed`);
process.exitCode = failed ? 1 : 0;
