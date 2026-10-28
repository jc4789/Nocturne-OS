"""Host self-check of the WPT testdriver tool (tests/wpt/testdriver-vendor.js), not of the browser.

Runs the vendor in node against a stand-in window whose __nocturne_automation records what it is
given, and checks the steps it builds for WebDriver's Element Click, Element Send Keys and Perform
Actions, that it stays off without the browser's automation, and how it places a same-origin
frame's points. It says nothing about what the browser does with the steps: that is for WPT's own
infrastructure/testdriver/ tests run in the guest.

usage: python -X utf8 tests/check_testdriver_vendor.py
"""
import json
import subprocess
import sys
from pathlib import Path

root = Path(__file__).resolve().parent.parent
vendor = (root / "tests/wpt/testdriver-vendor.js").read_text(encoding="utf-8")
if any(ord(c) > 127 for c in vendor):
    sys.exit("FAIL testdriver-vendor.js must be ASCII (wptserve opens it without an encoding)")

harness = r"""
const vm = require('vm');
const source = process.argv[1]; // node -e: the first argument after the script
let failed = 0, checks = 0;
function check(name, pass, detail) {
  checks++;
  if (!pass) { failed++; console.log('FAIL ' + name + (detail === undefined ? '' : ' ' + JSON.stringify(detail))); }
}
function element(doc, props) {
  const e = Object.assign({localName: 'div', type: '', ownerDocument: doc, isConnected: true, value: '',
    clientLeft: 0, clientTop: 0, parentNode: null, nodeType: 1,
    getClientRects() { return [{left: 0, top: 0, right: 10, bottom: 10}]; },
    getBoundingClientRect() { return this.getClientRects()[0]; },
    focus() { doc.activeElement = this; },
    setSelectionRange(a, b) { this.selection = [a, b]; }}, props);
  return e;
}
function realm(options) {
  const calls = [];
  const internal = {in_automation: false};
  for (const name of ['click', 'send_keys', 'action_sequence', 'set_permission']) internal[name] = function original() { return 'original ' + name; };
  const doc = {activeElement: null, querySelectorAll() { return []; }};
  const win = {test_driver_internal: internal, innerWidth: 800, innerHeight: 600, navigator: {webdriver: !!options.webdriver},
    document: doc, Promise, Map, Set, Array, Object, Math, JSON, String, Error, isFinite, parseFloat,
    addEventListener() {}, postMessage() {},
    getComputedStyle() { return {paddingLeft: '3px', paddingTop: '4px'}; }};
  win.window = win; win.top = options.top || win; win.frameElement = options.frameElement || null;
  doc.defaultView = win; doc.body = element(doc, {localName: 'body'}); doc.documentElement = element(doc, {localName: 'html'});
  doc.activeElement = doc.body;
  if (options.port) win.__nocturne_automation = function(steps, duration) { calls.push({steps: JSON.parse(JSON.stringify(steps)), duration}); return Promise.resolve(); };
  vm.createContext(win);
  vm.runInContext(source, win);
  return {win, doc, internal, calls};
}
const plain = s => s.map(x => x.type + (x.key !== undefined ? ':' + x.key.toString(16) : '') +
  (x.x !== undefined ? '@' + x.x + ',' + x.y : '') + (x.button !== undefined ? '#' + x.button : '') + (x.at ? '+' + x.at : ''));
(async () => {
  // Off without the browser's automation: testdriver.js's own functions stay.
  const off = realm({port: false});
  check('no port leaves in_automation false', off.internal.in_automation === false);
  check('no port leaves click as it was', off.internal.click() === 'original click');
  const on = realm({port: true});
  check('port sets in_automation', on.internal.in_automation === true);
  check('other functions keep the default refusal', on.internal.set_permission() === 'original set_permission');

  // Element Click: one run of move, press and release at the floored point.
  const button = element(on.doc, {localName: 'button'});
  await on.internal.click(button, {x: 10.7, y: 20.2});
  check('click steps', JSON.stringify(plain(on.calls[0].steps)) === JSON.stringify(['move@10,20', 'down@10,20#0', 'up@10,20#0']), plain(on.calls[0].steps));
  check('click has no duration', on.calls[0].duration === 0);
  let refused = null;
  await on.internal.click(element(on.doc, {localName: 'input', type: 'file'}), {x: 1, y: 1}).catch(e => refused = e);
  check('a file input is not clicked', refused && /invalid argument/.test(refused.message) && on.calls.length === 1);

  // Element Send Keys: focus, caret at the end, implicit Shift, modifiers held until U+E000.
  const input = element(on.doc, {localName: 'input', type: 'text', value: 'xy'});
  on.calls.length = 0;
  await on.internal.send_keys(input, 'aB!');
  check('send_keys focuses the element', on.doc.activeElement === input);
  check('send_keys puts the caret at the end', input.selection && input.selection[0] === 2 && input.selection[1] === 2);
  check('send_keys steps', JSON.stringify(plain(on.calls[0].steps)) === JSON.stringify(
    ['keyDown:61', 'keyUp:61', 'keyDown:e008', 'keyDown:42', 'keyUp:42', 'keyDown:21', 'keyUp:21', 'keyUp:e008']), plain(on.calls[0].steps));
  on.calls.length = 0;
  await on.internal.send_keys(input, 'ab');
  check('send_keys modifiers', JSON.stringify(plain(on.calls[0].steps)) === JSON.stringify(
    ['keyDown:e009', 'keyDown:61', 'keyUp:61', 'keyUp:e009', 'keyDown:62', 'keyUp:62']), plain(on.calls[0].steps));
  check('send_keys keeps the caret of an active element', input.selection[0] === 2);
  const range = element(on.doc, {localName: 'input', type: 'range', value: '0', validity: {badInput: false}});
  on.calls.length = 0;
  await on.internal.send_keys(range, '7');
  check('a range input gets its value set, no keys', range.value === '7' && on.calls.length === 0);
  const inert = element(on.doc, {localName: 'span', focus() {}});
  refused = null;
  await on.internal.send_keys(inert, 'a').catch(e => refused = e);
  check('a span that cannot have the keyboard is refused', refused && /element not interactable/.test(refused.message));

  // Perform Actions: ticks, durations, interpolated moves, release before the next call.
  on.calls.length = 0;
  await on.internal.action_sequence([
    {type: 'pointer', id: 'p', parameters: {pointerType: 'mouse'}, actions: [
      {type: 'pointerMove', x: 100, y: 50, origin: 'viewport', duration: 40}, {type: 'pointerDown', button: 0}]},
    {type: 'key', id: 'k', actions: [{type: 'keyDown', value: ''}, {type: 'pause'}]},
    {type: 'none', id: 'n', actions: [{type: 'pause', duration: 16}, {type: 'pause', duration: 16}]}]);
  check('two ticks, two runs', on.calls.length === 2);
  check('tick 1 steps', JSON.stringify(plain(on.calls[0].steps)) === JSON.stringify(
    ['keyDown:e008', 'move@40,20+16', 'move@80,40+32', 'move@100,50+40']), plain(on.calls[0].steps));
  check('tick 1 lasts its longest move', on.calls[0].duration === 40);
  check('tick 2 presses where the pointer is', JSON.stringify(plain(on.calls[1].steps)) === JSON.stringify(['down@100,50#0']));
  check('tick 2 lasts its pause', on.calls[1].duration === 16);
  on.calls.length = 0;
  await on.internal.action_sequence([{type: 'pointer', id: 'p', actions: [{type: 'pointerMove', x: 5, y: 6, origin: 'pointer'}]}]);
  check('the next call first releases what the last one held', JSON.stringify(plain(on.calls[0].steps)) === JSON.stringify(['up@100,50#0', 'keyUp:e008']), plain(on.calls[0].steps));
  check('a fresh source starts at 0,0', JSON.stringify(plain(on.calls[1].steps)) === JSON.stringify(['move@5,6']), plain(on.calls[1].steps));
  refused = null;
  await on.internal.action_sequence([{type: 'pointer', id: 'p', actions: [{type: 'pointerMove', x: 900, y: 6}]}]).catch(e => refused = e);
  check('a move outside the viewport is refused', refused && /move target out of bounds/.test(refused.message));
  const target = element(on.doc, {getClientRects() { return [{left: 100, top: 200, right: 151, bottom: 221}]; }});
  on.calls.length = 0;
  await on.internal.action_sequence([{type: 'pointer', id: 'p', actions: [{type: 'pointerMove', x: 2, y: -3, origin: target}]}]);
  check('an element origin is its in-view centre', JSON.stringify(plain(on.calls[on.calls.length - 1].steps)) === JSON.stringify(['move@127,207']), plain(on.calls[on.calls.length - 1].steps));
  const other = realm({port: true});
  refused = null;
  await on.internal.action_sequence([{type: 'pointer', id: 'p', actions: [
    {type: 'pointerMove', x: 0, y: 0, origin: target}, {type: 'pointerMove', x: 0, y: 0, origin: element(other.doc, {})}]}]).catch(e => refused = e);
  check('actions in two windows are refused', refused && /single context/.test(refused.message));
  refused = null;
  await on.internal.action_sequence([{type: 'pointer', id: 't', parameters: {pointerType: 'touch'}, actions: [{type: 'pointerDown', button: 0}]}]).catch(e => refused = e);
  check('a touch pointer is refused, not faked', refused && /unsupported operation/.test(refused.message));
  on.calls.length = 0;
  await on.internal.action_sequence([{type: 'wheel', id: 'w', actions: [{type: 'scroll', x: 10, y: 10, deltaX: 0, deltaY: 120, duration: 30}]}]);
  check('a scroll is one wheel step at its duration', JSON.stringify(plain(on.calls[on.calls.length - 1].steps)) === JSON.stringify(['wheel@10,10+30']));

  // A same-origin frame: its points move by the frame's content box, through the top's port.
  const frameElement = element(on.doc, {localName: 'iframe', clientLeft: 2, clientTop: 1,
    getClientRects() { return [{left: 30, top: 40, right: 330, bottom: 240}]; }});
  const child = realm({port: false, webdriver: true, top: on.win, frameElement});
  check('a frame under automation turns on', child.internal.in_automation === true);
  on.calls.length = 0;
  await child.internal.click(element(child.doc, {localName: 'button'}), {x: 5, y: 6});
  check('a frame click goes to the top port, moved by the content box',
    JSON.stringify(plain(on.calls[0].steps)) === JSON.stringify(['move@40,51', 'down@40,51#0', 'up@40,51#0']), plain(on.calls[0].steps));
  const unautomated = realm({port: false, webdriver: false, top: on.win, frameElement});
  check('a frame without automation stays off', unautomated.internal.in_automation === false);
  console.log('testdriver-vendor: ' + checks + ' checks, ' + failed + ' failed');
  process.exit(failed ? 1 : 0);
})().catch(e => { console.log('FAIL harness ' + (e && e.stack || e)); process.exit(1); });
"""
result = subprocess.run(["node", "-e", harness, vendor], capture_output=True, text=True, encoding="utf-8")
sys.stdout.write(result.stdout)
sys.stderr.write(result.stderr)
sys.exit(result.returncode)
