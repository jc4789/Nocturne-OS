/*
 * js_web.js - web-platform globals for the js terminal runtime.
 *
 * This file is a FUNCTION BODY: js.c wraps it as `(function (host, internal) { ... })`
 * and calls it once after the core chunk.  It installs Event, EventTarget, CustomEvent,
 * AbortController, AbortSignal, Headers, Request, Response, fetch, structuredClone,
 * atob, btoa and reportError, and defines `internal.fetchDone`.
 *
 * Notes / deliberate limits:
 *  - There are no streams in this runtime.  Bodies are held as Uint8Array and
 *    `Request/Response.body` is always null; blob() and formData() reject.
 *  - URL / URLSearchParams are lazy globals; they are touched only when fetch,
 *    Request or Response.redirect really need to parse a URL.
 */
'use strict';

// ---------------------------------------------------------------- helpers

const platform = new WeakSet();           // instances of the classes below (not structured-cloneable)
const def = (name, value) =>
  Object.defineProperty(globalThis, name, { value, writable: true, configurable: true, enumerable: false });
const tagOf = (C, name) =>
  Object.defineProperty(C.prototype, Symbol.toStringTag, { value: name, configurable: true });
const isObj = v => v !== null && (typeof v === 'object' || typeof v === 'function');
const argc = (n, need, what) => {
  if (n < need) throw new TypeError(`${what}: ${need} argument${need > 1 ? 's' : ''} required, but only ${n} present.`);
};
const now = () => (typeof performance === 'object' && performance && typeof performance.now === 'function')
  ? performance.now() : Date.now();
let encoder = null;
const utf8 = s => (encoder ??= new TextEncoder()).encode(s);
const objToString = Object.prototype.toString;
const WS_TRIM = /^[\t\n\r ]+|[\t\n\r ]+$/g;
const TOKEN = /^[!#$%&'*+\-.^_`|~0-9A-Za-z]+$/;

// ---------------------------------------------------------------- Event / EventTarget

const es = new WeakMap();                 // Event -> state
const lis = new WeakMap();                // EventTarget -> Map(type -> listener[])
const evs = e => {
  const s = es.get(e);
  if (!s) throw new TypeError('Illegal invocation');
  return s;
};

class Event {
  constructor(type, init = undefined) {
    argc(arguments.length, 1, "Failed to construct 'Event'");
    if (init !== undefined && init !== null && !isObj(init))
      throw new TypeError("Failed to construct 'Event': The provided value is not of type 'EventInit'.");
    const o = init ?? {};
    es.set(this, {
      type: String(type), bubbles: !!o.bubbles, cancelable: !!o.cancelable, composed: !!o.composed,
      target: null, current: null, phase: 0, canceled: false, stopProp: false, stopImm: false,
      dispatching: false, passive: false, time: now(),
    });
    platform.add(this);
  }
  get type() { return evs(this).type; }
  get target() { return evs(this).target; }
  get srcElement() { return evs(this).target; }
  get currentTarget() { return evs(this).current; }
  get eventPhase() { return evs(this).phase; }
  get bubbles() { return evs(this).bubbles; }
  get cancelable() { return evs(this).cancelable; }
  get composed() { return evs(this).composed; }
  get defaultPrevented() { return evs(this).canceled; }
  get timeStamp() { return evs(this).time; }
  get isTrusted() { evs(this); return false; }
  get cancelBubble() { return evs(this).stopProp; }
  set cancelBubble(v) { const s = evs(this); if (v) s.stopProp = true; }
  get returnValue() { return !evs(this).canceled; }
  set returnValue(v) { if (!v) this.preventDefault(); }
  composedPath() { const s = evs(this); return s.dispatching && s.current ? [s.current] : []; }
  preventDefault() { const s = evs(this); if (s.cancelable && !s.passive) s.canceled = true; }
  stopPropagation() { evs(this).stopProp = true; }
  stopImmediatePropagation() { const s = evs(this); s.stopProp = true; s.stopImm = true; }
}
for (const [k, v] of [['NONE', 0], ['CAPTURING_PHASE', 1], ['AT_TARGET', 2], ['BUBBLING_PHASE', 3]]) {
  Object.defineProperty(Event, k, { value: v, enumerable: true });
  Object.defineProperty(Event.prototype, k, { value: v, enumerable: true });
}
tagOf(Event, 'Event');

class CustomEvent extends Event {
  #detail;
  constructor(type, init = undefined) {
    super(type, init);
    this.#detail = (isObj(init) && init.detail !== undefined) ? init.detail : null;
  }
  get detail() { return this.#detail; }
}
tagOf(CustomEvent, 'CustomEvent');

function addListener(t, type, cb, opts) {
  let capture = false, once = false, passive = false, signal;
  if (isObj(opts)) {
    capture = !!opts.capture; once = !!opts.once; passive = !!opts.passive; signal = opts.signal;
    if (signal !== undefined && !sigState.has(signal))
      throw new TypeError("Failed to execute 'addEventListener' on 'EventTarget': member signal is not of type AbortSignal.");
  } else capture = !!opts;
  if (signal !== undefined && isAborted(signal)) return;
  if (cb === null || cb === undefined) return;
  if (!isObj(cb)) throw new TypeError("Failed to execute 'addEventListener' on 'EventTarget': The callback provided as parameter 2 is not an object.");
  let m = lis.get(t);
  if (!m) lis.set(t, m = new Map());
  let list = m.get(type);
  if (!list) m.set(type, list = []);
  for (const l of list) if (l.cb === cb && l.capture === capture) return;
  const l = { cb, capture, once, passive, signal, removed: false, abortFn: null };
  list.push(l);
  if (signal !== undefined) {
    l.abortFn = () => dropListener(t, type, l);
    addListener(signal, 'abort', l.abortFn, { once: true });
  }
  if (type === 'abort' && sigState.has(t)) arm(t, sigState.get(t));
}

function dropListener(t, type, l) {
  if (l.removed) return;
  l.removed = true;
  const list = lis.get(t).get(type);
  const i = list.indexOf(l);
  if (i >= 0) list.splice(i, 1);
  if (l.abortFn) removeListener(l.signal, 'abort', l.abortFn, false);
  if (type === 'abort' && sigState.has(t)) disarm(t, sigState.get(t));
}

function removeListener(t, type, cb, capture) {
  const list = lis.get(t)?.get(type);
  if (!list) return;
  for (const l of list) if (l.cb === cb && l.capture === capture) { dropListener(t, type, l); return; }
}

function invoke(target, event, s, bubblingPass) {
  if (s.stopProp) return;
  s.current = target;
  const list = lis.get(target)?.get(s.type);
  if (!list || list.length === 0) return;
  for (const l of list.slice()) {
    if (l.removed || l.capture === bubblingPass) continue;   // capture listeners first, then the rest
    if (l.once) dropListener(target, s.type, l);
    s.passive = l.passive;
    try {
      const cb = l.cb;
      if (typeof cb === 'function') cb.call(target, event);
      else {
        const h = cb.handleEvent;
        if (typeof h !== 'function') throw new TypeError('The event listener object has no callable handleEvent');
        h.call(cb, event);
      }
    } catch (e) {
      internal.reportError(e);
    }
    s.passive = false;
    if (s.stopImm) break;
  }
}

function dispatchImpl(target, event) {
  const s = es.get(event);
  if (s.dispatching) throw new DOMException('The event is already being dispatched.', 'InvalidStateError');
  s.dispatching = true; s.target = target; s.phase = 2;
  try {
    invoke(target, event, s, false);
    invoke(target, event, s, true);
  } finally {
    s.dispatching = false; s.phase = 0; s.current = null; s.stopProp = false; s.stopImm = false; s.passive = false;
  }
  return !s.canceled;
}

class EventTarget {
  constructor() { lis.set(this, new Map()); platform.add(this); }
  addEventListener(type, callback, options = undefined) {
    argc(arguments.length, 2, "Failed to execute 'addEventListener' on 'EventTarget'");
    if (!lis.has(this)) throw new TypeError('Illegal invocation');
    addListener(this, String(type), callback, options);
  }
  removeEventListener(type, callback, options = undefined) {
    argc(arguments.length, 2, "Failed to execute 'removeEventListener' on 'EventTarget'");
    if (!lis.has(this)) throw new TypeError('Illegal invocation');
    removeListener(this, String(type), callback, isObj(options) ? !!options.capture : !!options);
  }
  dispatchEvent(event) {
    argc(arguments.length, 1, "Failed to execute 'dispatchEvent' on 'EventTarget'");
    if (!lis.has(this)) throw new TypeError('Illegal invocation');
    if (!es.has(event)) throw new TypeError("Failed to execute 'dispatchEvent' on 'EventTarget': parameter 1 is not of type 'Event'.");
    return dispatchImpl(this, event);
  }
}
tagOf(EventTarget, 'EventTarget');

// Event handler IDL attributes (e.g. onabort): the listener is registered the first time a handler is set.
const handlers = new WeakMap();
function handlerAttr(C, name) {
  const type = name.slice(2);
  Object.defineProperty(C.prototype, name, {
    enumerable: true, configurable: true,
    get() {
      if (!lis.has(this)) throw new TypeError('Illegal invocation');
      return handlers.get(this)?.get(name)?.fn ?? null;
    },
    set(v) {
      if (!lis.has(this)) throw new TypeError('Illegal invocation');
      let m = handlers.get(this);
      if (!m) handlers.set(this, m = new Map());
      let h = m.get(name);
      if (typeof v !== 'function') v = null;
      if (!h) {
        if (v === null) return;
        h = { fn: null };
        m.set(name, h);
        addListener(this, type, function (e) {
          const f = h.fn;
          if (typeof f === 'function' && Reflect.apply(f, this, [e]) === false) e.preventDefault();
        }, false);
      }
      h.fn = v;
    },
  });
}

// ---------------------------------------------------------------- AbortSignal / AbortController

const sigState = new WeakMap();   // signal -> { aborted, reason, deps, sources, deadline, timer }
let creating = false;
const abortReason = () => new DOMException('This operation was aborted', 'AbortError');
const timeoutReason = () => new DOMException('The operation timed out.', 'TimeoutError');
const sst = s => {
  const st = sigState.get(s);
  if (!st) throw new TypeError('Illegal invocation');
  return st;
};

function isAborted(sig) {
  const s = sigState.get(sig);
  if (s.deadline !== undefined && !s.aborted && Date.now() >= s.deadline) doAbort(sig, timeoutReason());
  return s.aborted;
}

// Timeout signals only keep a host timer while somebody could observe them (abort listeners
// or dependent signals), so a completed `fetch(url, {signal: AbortSignal.timeout(n)})` does
// not keep the process alive.
function arm(sig, s) {
  if (s.deadline === undefined || s.timer !== null || s.aborted) return;
  const t = setTimeout(() => { s.timer = null; doAbort(sig, timeoutReason()); }, Math.max(0, s.deadline - Date.now()));
  if (t && typeof t.unref === 'function') t.unref();
  s.timer = t;
}
function disarm(sig, s) {
  if (s.timer === null || s.deps.length > 0) return;
  if (lis.get(sig)?.get('abort')?.length > 0) return;
  clearTimeout(s.timer);
  s.timer = null;
}

function newSignal() {
  creating = true;
  try { return new AbortSignal(); } finally { creating = false; }
}

function fireAbort(sig) { dispatchImpl(sig, new Event('abort')); }

function doAbort(sig, reason) {
  const s = sigState.get(sig);
  if (s.aborted) return;
  s.aborted = true; s.reason = reason;
  if (s.timer !== null) { clearTimeout(s.timer); s.timer = null; }
  const dependents = [];
  for (const d of s.deps) {
    const ds = sigState.get(d);
    if (!ds.aborted) { ds.aborted = true; ds.reason = reason; dependents.push(d); }
  }
  s.deps = [];
  fireAbort(sig);
  for (const d of dependents) fireAbort(d);
}

// Make `dep` abort when any of `srcs` aborts (sources are flattened to non-dependent signals).
function linkSignals(dep, srcs) {
  const ds = sigState.get(dep);
  ds.sources = [];
  for (const src of srcs) {
    for (const leaf of (sigState.get(src).sources ?? [src])) {
      if (ds.sources.includes(leaf)) continue;
      const ls = sigState.get(leaf);
      ls.deps.push(dep);
      ds.sources.push(leaf);
      arm(leaf, ls);
    }
  }
}
function unlinkSignal(dep) {
  const ds = sigState.get(dep);
  if (!ds.sources) return;
  for (const leaf of ds.sources) {
    const ls = sigState.get(leaf);
    const i = ls.deps.indexOf(dep);
    if (i >= 0) ls.deps.splice(i, 1);
    disarm(leaf, ls);
  }
  ds.sources = [];
}
function followSignal(dep, src) {
  if (isAborted(src)) doAbort(dep, sigState.get(src).reason);
  else linkSignals(dep, [src]);
}

class AbortSignal extends EventTarget {
  constructor() {
    if (!creating) throw new TypeError('Illegal constructor');
    creating = false;
    super();
    sigState.set(this, { aborted: false, reason: undefined, deps: [], sources: null, deadline: undefined, timer: null });
  }
  get aborted() { sst(this); return isAborted(this); }
  get reason() { sst(this); isAborted(this); return sigState.get(this).reason; }
  throwIfAborted() { if (this.aborted) throw sigState.get(this).reason; }
  static abort(reason = undefined) {
    const sig = newSignal();
    doAbort(sig, reason === undefined ? abortReason() : reason);
    return sig;
  }
  static timeout(ms) {
    argc(arguments.length, 1, "Failed to execute 'timeout' on 'AbortSignal'");
    ms = Number(ms);
    if (!(ms >= 0) || !isFinite(ms)) throw new TypeError("Failed to execute 'timeout' on 'AbortSignal': value is out of range.");
    const sig = newSignal();
    sigState.get(sig).deadline = Date.now() + Math.trunc(ms);
    return sig;
  }
  static any(signals) {
    argc(arguments.length, 1, "Failed to execute 'any' on 'AbortSignal'");
    const list = [...signals];
    for (const x of list) if (!sigState.has(x)) throw new TypeError("Failed to execute 'any' on 'AbortSignal': Failed to convert value to 'AbortSignal'.");
    const sig = newSignal();
    for (const x of list) if (isAborted(x)) { doAbort(sig, sigState.get(x).reason); return sig; }
    linkSignals(sig, list);
    return sig;
  }
}
handlerAttr(AbortSignal, 'onabort');
tagOf(AbortSignal, 'AbortSignal');

class AbortController {
  #signal;
  constructor() { this.#signal = newSignal(); platform.add(this); }
  get signal() { return this.#signal; }
  abort(reason = undefined) { doAbort(this.#signal, reason === undefined ? abortReason() : reason); }
}
tagOf(AbortController, 'AbortController');

// ---------------------------------------------------------------- Headers

const hst = new WeakMap();        // Headers -> { map: Map(lowercase name -> string[]), guard }
const hs = h => {
  const s = hst.get(h);
  if (!s) throw new TypeError('Illegal invocation');
  return s;
};
const mutable = s => {
  if (s.guard === 'immutable') throw new TypeError('Headers are immutable');
  return s;
};
const hname = n => {
  n = String(n);
  if (!TOKEN.test(n)) throw new TypeError(`Invalid header name: ${JSON.stringify(n)}`);
  return n.toLowerCase();
};
const hvalue = v => {
  v = String(v).replace(WS_TRIM, '');
  if (/[\0\r\n]/.test(v)) throw new TypeError('Invalid header value: contains NUL, CR or LF');
  if (/[^\0-\xff]/.test(v)) throw new TypeError('Invalid header value: not a ByteString');
  return v;
};
function hpush(s, name, value) {
  const a = s.map.get(name);
  if (a) a.push(value); else s.map.set(name, [value]);
}
function sortedEntries(s) {
  const out = [];
  for (const k of [...s.map.keys()].sort()) {
    const v = s.map.get(k);
    if (k === 'set-cookie') for (const c of v) out.push([k, c]);
    else out.push([k, v.join(', ')]);
  }
  return out;
}
function fillHeaders(h, init) {
  if (init === undefined || init === null) return;
  if (!isObj(init)) throw new TypeError("Failed to construct 'Headers': The provided value is not of type 'HeadersInit'.");
  const s = hst.get(h);
  const other = hst.get(init);
  if (other) {
    for (const [k, vs] of other.map) for (const v of vs) hpush(s, k, v);
    return;
  }
  const it = init[Symbol.iterator];
  if (it !== undefined && it !== null) {
    if (typeof it !== 'function') throw new TypeError('Header init is not iterable');
    for (const pair of init) {
      if (!isObj(pair)) throw new TypeError('Each header pair must be an iterable of [name, value]');
      const arr = [...pair];
      if (arr.length !== 2) throw new TypeError('Each header pair must contain exactly two items');
      hpush(s, hname(arr[0]), hvalue(arr[1]));
    }
  } else {
    for (const k of Reflect.ownKeys(init)) {
      if (typeof k !== 'string') continue;
      const d = Reflect.getOwnPropertyDescriptor(init, k);
      if (d && d.enumerable) hpush(s, hname(k), hvalue(init[k]));
    }
  }
}

class Headers {
  constructor(init = undefined) {
    hst.set(this, { map: new Map(), guard: 'none' });
    platform.add(this);
    fillHeaders(this, init);
  }
  append(name, value) {
    argc(arguments.length, 2, "Failed to execute 'append' on 'Headers'");
    const s = mutable(hs(this));
    hpush(s, hname(name), hvalue(value));
  }
  delete(name) {
    argc(arguments.length, 1, "Failed to execute 'delete' on 'Headers'");
    const s = mutable(hs(this));
    s.map.delete(hname(name));
  }
  get(name) {
    argc(arguments.length, 1, "Failed to execute 'get' on 'Headers'");
    const a = hs(this).map.get(hname(name));
    return a ? a.join(', ') : null;
  }
  getSetCookie() { return (hs(this).map.get('set-cookie') ?? []).slice(); }
  has(name) {
    argc(arguments.length, 1, "Failed to execute 'has' on 'Headers'");
    return hs(this).map.has(hname(name));
  }
  set(name, value) {
    argc(arguments.length, 2, "Failed to execute 'set' on 'Headers'");
    const s = mutable(hs(this));
    const n = hname(name);
    s.map.set(n, [hvalue(value)]);
  }
  forEach(callback, thisArg = undefined) {
    argc(arguments.length, 1, "Failed to execute 'forEach' on 'Headers'");
    if (typeof callback !== 'function') throw new TypeError("Failed to execute 'forEach' on 'Headers': parameter 1 is not of type 'Function'.");
    for (const [k, v] of sortedEntries(hs(this))) callback.call(thisArg, v, k, this);
  }
  entries() { return sortedEntries(hs(this))[Symbol.iterator](); }
  keys() { return sortedEntries(hs(this)).map(e => e[0])[Symbol.iterator](); }
  values() { return sortedEntries(hs(this)).map(e => e[1])[Symbol.iterator](); }
}
Object.defineProperty(Headers.prototype, Symbol.iterator,
  { value: Headers.prototype.entries, writable: true, configurable: true });
tagOf(Headers, 'Headers');

function newHeaders(guard) {
  const h = new Headers();
  hst.get(h).guard = guard;
  return h;
}
function parseHeaderBlock(text) {
  const pairs = [];
  for (const line of String(text ?? '').split(/\r?\n/)) {
    if (line === '' || /^HTTP\/\d/.test(line)) continue;
    if (/^[ \t]/.test(line)) {                       // obsolete line folding
      if (pairs.length) pairs[pairs.length - 1][1] += ' ' + line.trim();
      continue;
    }
    const i = line.indexOf(':');
    if (i > 0) pairs.push([line.slice(0, i).trim(), line.slice(i + 1)]);
  }
  const h = new Headers();
  const s = hst.get(h);
  for (const [n, v] of pairs) {
    try { hpush(s, hname(n), hvalue(v)); } catch { /* skip malformed header lines */ }
  }
  s.guard = 'immutable';
  return h;
}

// ---------------------------------------------------------------- Body mixin

const bodyOf = new WeakMap();     // Request/Response -> { bytes: Uint8Array|null, used }
const abLen = Object.getOwnPropertyDescriptor(ArrayBuffer.prototype, 'byteLength').get;
const isAB = v => { try { abLen.call(v); return true; } catch { return false; } };
const bst = o => {
  const s = bodyOf.get(o);
  if (!s) throw new TypeError('Illegal invocation');
  return s;
};

function extractBody(v) {
  if (v === null || v === undefined) return { bytes: null, type: null };
  if (typeof v === 'string') return { bytes: utf8(v), type: 'text/plain;charset=UTF-8' };
  if (isObj(v)) {
    if (isAB(v)) return { bytes: new Uint8Array(v).slice(), type: null };
    if (ArrayBuffer.isView(v)) return { bytes: new Uint8Array(v.buffer, v.byteOffset, v.byteLength).slice(), type: null };
    if (objToString.call(v) === '[object URLSearchParams]')
      return { bytes: utf8(String(v)), type: 'application/x-www-form-urlencoded;charset=UTF-8' };
  }
  return { bytes: utf8(String(v)), type: 'text/plain;charset=UTF-8' };
}
function takeBody(o) {
  const s = bst(o);
  if (s.used) throw new TypeError('Body is unusable: Body has already been read');
  if (s.bytes === null) return new Uint8Array(0);
  s.used = true;
  return s.bytes;
}
const toAB = u => (u.byteOffset === 0 && u.byteLength === u.buffer.byteLength)
  ? u.buffer : u.buffer.slice(u.byteOffset, u.byteOffset + u.byteLength);
function decodeUtf8(u) {
  if (u.length >= 3 && u[0] === 0xEF && u[1] === 0xBB && u[2] === 0xBF) u = u.subarray(3);
  return new TextDecoder('utf-8').decode(u);
}

const bodyMixin = {
  // Streams are not supported in js: `body` is always null.
  get body() { bst(this); return null; },
  get bodyUsed() { return bst(this).used; },
  async arrayBuffer() { return toAB(takeBody(this)); },
  async bytes() { return takeBody(this); },
  async text() { return decodeUtf8(takeBody(this)); },
  async json() { return JSON.parse(decodeUtf8(takeBody(this))); },
  async blob() { bst(this); throw new TypeError('blob() is not supported in js'); },
  async formData() { bst(this); throw new TypeError('formData() is not supported in js'); },
};
const installBody = C => Object.defineProperties(C.prototype, Object.getOwnPropertyDescriptors(bodyMixin));

// ---------------------------------------------------------------- Response

const rs = new WeakMap();         // Response -> { status, statusText, headers, type, url, redirected }
const NULL_BODY = [101, 103, 204, 205, 304];
const rst = r => {
  const s = rs.get(r);
  if (!s) throw new TypeError('Illegal invocation');
  return s;
};
function initResponse(r, st, bytes) {
  rs.set(r, st);
  bodyOf.set(r, { bytes, used: false });
  platform.add(r);
}
// Build a Response from native data without WebIDL validation (any status, e.g. 0 for errors).
function makeResponse(st, bytes) {
  const r = Object.create(Response.prototype);
  initResponse(r, st, bytes);
  return r;
}

class Response {
  constructor(body = null, init = undefined) {
    if (init !== undefined && init !== null && !isObj(init))
      throw new TypeError("Failed to construct 'Response': The provided value is not of type 'ResponseInit'.");
    const o = init ?? {};
    const status = o.status !== undefined ? Number(o.status) : 200;
    if (!Number.isInteger(status) || status < 200 || status > 599)
      throw new RangeError('init["status"] must be in the range of 200 to 599, inclusive.');
    const statusText = o.statusText !== undefined ? String(o.statusText) : '';
    if (!/^[\t\x20-\x7e\x80-\xff]*$/.test(statusText)) throw new TypeError('Invalid statusText');
    const headers = new Headers(o.headers);
    const x = extractBody(body);
    if (x.bytes !== null) {
      if (NULL_BODY.includes(status)) throw new TypeError(`Response with null body status cannot have body`);
      if (x.type !== null && !headers.has('content-type')) headers.append('content-type', x.type);
    }
    initResponse(this, { status, statusText, headers, type: 'default', url: '', redirected: false }, x.bytes);
  }
  get type() { return rst(this).type; }
  get url() { return rst(this).url; }
  get redirected() { return rst(this).redirected; }
  get status() { return rst(this).status; }
  get ok() { const c = rst(this).status; return c >= 200 && c <= 299; }
  get statusText() { return rst(this).statusText; }
  get headers() { return rst(this).headers; }
  clone() {
    const s = rst(this), b = bst(this);
    if (b.used) throw new TypeError('Response.clone: Body has already been consumed.');
    const h = new Headers(s.headers);
    hst.get(h).guard = hst.get(s.headers).guard;
    return makeResponse({ ...s, headers: h }, b.bytes === null ? null : b.bytes.slice());
  }
  static json(data, init = undefined) {
    argc(arguments.length, 1, "Failed to execute 'json' on 'Response'");
    const text = JSON.stringify(data);
    if (text === undefined) throw new TypeError('Value is not JSON serializable');
    const r = new Response(utf8(text), init);
    const h = rst(r).headers;
    if (!h.has('content-type')) h.set('content-type', 'application/json');
    return r;
  }
  static error() {
    return makeResponse({ status: 0, statusText: '', headers: newHeaders('immutable'), type: 'error', url: '', redirected: false }, null);
  }
  static redirect(url, status = 302) {
    argc(arguments.length, 1, "Failed to execute 'redirect' on 'Response'");
    let u;
    try { u = new URL(String(url)); } catch (e) { throw new TypeError(`Failed to parse URL from ${url}`, { cause: e }); }
    status = Number(status);
    if (![301, 302, 303, 307, 308].includes(status)) throw new RangeError(`Invalid status code ${status}`);
    const headers = newHeaders('none');
    headers.append('location', u.href);
    hst.get(headers).guard = 'immutable';
    return makeResponse({ status, statusText: '', headers, type: 'default', url: '', redirected: false }, null);
  }
}
installBody(Response);
tagOf(Response, 'Response');

// ---------------------------------------------------------------- Request

const rq = new WeakMap();         // Request -> { url, method, headers, signal, redirect, ex }
const METHODS = ['DELETE', 'GET', 'HEAD', 'OPTIONS', 'POST', 'PUT'];
const REDIRECT_MODES = ['follow', 'error', 'manual'];
const EXTRA_DEFAULTS = {
  mode: 'cors', credentials: 'same-origin', cache: 'default', referrer: 'about:client',
  referrerPolicy: '', integrity: '', keepalive: false, priority: 'auto',
};
const rqs = r => {
  const s = rq.get(r);
  if (!s) throw new TypeError('Illegal invocation');
  return s;
};
function parseURL(s) {
  let u;
  try { u = new URL(s); } catch (e) { throw new TypeError(`Failed to parse URL from ${s}`, { cause: e }); }
  if (u.username || u.password) throw new TypeError(`Request cannot be constructed from a URL that includes credentials: ${s}`);
  return u.href;
}

class Request {
  constructor(input, init = undefined) {
    argc(arguments.length, 1, "Failed to construct 'Request'");
    if (init !== undefined && init !== null && !isObj(init))
      throw new TypeError("Failed to construct 'Request': The provided value is not of type 'RequestInit'.");
    const o = init ?? {};
    const inq = isObj(input) ? rq.get(input) : undefined;
    let url, method = 'GET', redirect = 'follow', srcSignal = null, headersInit, ex = { ...EXTRA_DEFAULTS };
    let inBody = null;
    if (inq) {
      ({ url, method, redirect } = inq);
      srcSignal = inq.signal; headersInit = inq.headers; ex = { ...inq.ex };
      inBody = bodyOf.get(input);
    } else url = parseURL(String(input));

    if (o.method !== undefined) {
      const m = String(o.method);
      if (!TOKEN.test(m)) throw new TypeError(`'${m}' is not a valid HTTP method.`);
      const up = m.toUpperCase();
      if (up === 'CONNECT' || up === 'TRACE' || up === 'TRACK') throw new TypeError(`'${m}' HTTP method is unsupported.`);
      method = METHODS.includes(up) ? up : m;
    }
    if (o.redirect !== undefined) {
      redirect = String(o.redirect);
      if (!REDIRECT_MODES.includes(redirect)) throw new TypeError(`Failed to construct 'Request': The provided value '${redirect}' is not a valid enum value of type RequestRedirect.`);
    }
    if (o.signal !== undefined) {
      if (o.signal === null) srcSignal = null;
      else if (!sigState.has(o.signal)) throw new TypeError("Failed to construct 'Request': member signal is not of type AbortSignal.");
      else srcSignal = o.signal;
    }
    for (const k in EXTRA_DEFAULTS) if (o[k] !== undefined) ex[k] = k === 'keepalive' ? !!o[k] : String(o[k]);

    const noBody = method === 'GET' || method === 'HEAD';
    let bytes = null, ctype = null;
    if (o.body !== undefined && o.body !== null) {
      if (noBody) throw new TypeError('Request with GET/HEAD method cannot have body.');
      ({ bytes, type: ctype } = extractBody(o.body));
    } else if (inBody && inBody.bytes !== null) {
      if (noBody) throw new TypeError('Request with GET/HEAD method cannot have body.');
      if (inBody.used) throw new TypeError('Cannot construct a Request with a Request object that has already been used.');
      bytes = inBody.bytes; inBody.used = true;      // the body moves to the new request
    }
    const headers = new Headers(o.headers !== undefined ? o.headers : headersInit);
    if (ctype !== null && !headers.has('content-type')) headers.append('content-type', ctype);

    const signal = newSignal();
    if (srcSignal) followSignal(signal, srcSignal);
    rq.set(this, { url, method, headers, signal, redirect, ex });
    bodyOf.set(this, { bytes, used: false });
    platform.add(this);
  }
  get url() { return rqs(this).url; }
  get method() { return rqs(this).method; }
  get headers() { return rqs(this).headers; }
  get signal() { return rqs(this).signal; }
  get redirect() { return rqs(this).redirect; }
  get mode() { return rqs(this).ex.mode; }
  get credentials() { return rqs(this).ex.credentials; }
  get cache() { return rqs(this).ex.cache; }
  get referrer() { return rqs(this).ex.referrer; }
  get referrerPolicy() { return rqs(this).ex.referrerPolicy; }
  get integrity() { return rqs(this).ex.integrity; }
  get keepalive() { return rqs(this).ex.keepalive; }
  get priority() { return rqs(this).ex.priority; }
  get destination() { rqs(this); return ''; }
  clone() {
    const s = rqs(this), b = bst(this);
    if (b.used) throw new TypeError('Request.clone: Body has already been consumed.');
    const r = Object.create(Request.prototype);
    const signal = newSignal();
    followSignal(signal, s.signal);
    rq.set(r, { ...s, headers: new Headers(s.headers), signal, ex: { ...s.ex } });
    bodyOf.set(r, { bytes: b.bytes === null ? null : b.bytes.slice(), used: false });
    platform.add(r);
    return r;
  }
}
installBody(Request);
tagOf(Request, 'Request');

// ---------------------------------------------------------------- base64 / data: URLs

const B64 = 'ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/';
let b64rev = null;
// Forgiving-base64 decode (WHATWG Infra); returns Uint8Array or null on failure.
function b64decode(str) {
  let s = str.replace(/[\t\n\f\r ]+/g, '');
  if (s.length % 4 === 0) s = s.replace(/={1,2}$/, '');
  if (s.length % 4 === 1 || /[^A-Za-z0-9+/]/.test(s)) return null;
  if (!b64rev) {
    b64rev = new Uint8Array(128);
    for (let i = 0; i < 64; i++) b64rev[B64.charCodeAt(i)] = i;
  }
  const out = new Uint8Array((s.length * 3) >> 2);
  let acc = 0, bits = 0, o = 0;
  for (let i = 0; i < s.length; i++) {
    acc = (acc << 6) | b64rev[s.charCodeAt(i)];
    bits += 6;
    if (bits >= 8) { bits -= 8; out[o++] = (acc >> bits) & 255; }
    acc &= (1 << bits) - 1;
  }
  return out;
}
function latin1(u) {
  let r = '';
  for (let i = 0; i < u.length; i += 8192) r += String.fromCharCode.apply(null, u.subarray(i, i + 8192));
  return r;
}
const hexVal = c => (c >= 48 && c <= 57) ? c - 48 : ((c |= 32) >= 97 && c <= 102) ? c - 87 : -1;
function percentDecode(str) {
  const b = utf8(str);
  if (!b.includes(37)) return b;
  const out = new Uint8Array(b.length);
  let o = 0;
  for (let i = 0; i < b.length; i++) {
    const c = b[i];
    if (c === 37 && i + 2 < b.length && hexVal(b[i + 1]) >= 0 && hexVal(b[i + 2]) >= 0) {
      out[o++] = hexVal(b[i + 1]) * 16 + hexVal(b[i + 2]);
      i += 2;
    } else out[o++] = c;
  }
  return out.subarray(0, o);
}
// Returns { bytes, type } or null for a malformed data: URL.
function parseDataURL(url) {
  const hash = url.indexOf('#');
  if (hash >= 0) url = url.slice(0, hash);
  const comma = url.indexOf(',');
  if (comma < 0) return null;
  let mime = url.slice(5, comma).replace(WS_TRIM, '');
  let bytes = percentDecode(url.slice(comma + 1));
  const m = /;[ ]*base64$/i.exec(mime);
  if (m) {
    mime = mime.slice(0, m.index);
    bytes = b64decode(latin1(bytes));
    if (!bytes) return null;
  }
  if (mime.startsWith(';')) mime = 'text/plain' + mime;
  if (!/^[^\s\/;,]+\/[^\s\/;,]+(;.*)?$/.test(mime)) mime = 'text/plain;charset=US-ASCII';
  return { bytes, type: mime };
}

function btoa(data) {
  argc(arguments.length, 1, "Failed to execute 'btoa'");
  const s = String(data);
  if (/[^\0-\xff]/.test(s)) throw new DOMException('Invalid character', 'InvalidCharacterError');
  let out = '';
  const n = s.length;
  for (let i = 0; i < n; i += 3) {
    const a = s.charCodeAt(i), b = i + 1 < n ? s.charCodeAt(i + 1) : 0, c = i + 2 < n ? s.charCodeAt(i + 2) : 0;
    out += B64[a >> 2] + B64[((a & 3) << 4) | (b >> 4)]
      + (i + 1 < n ? B64[((b & 15) << 2) | (c >> 6)] : '=')
      + (i + 2 < n ? B64[c & 63] : '=');
  }
  return out;
}
function atob(data) {
  argc(arguments.length, 1, "Failed to execute 'atob'");
  const bytes = b64decode(String(data));
  if (bytes === null) throw new DOMException('The string to be decoded is not correctly encoded.', 'InvalidCharacterError');
  return latin1(bytes);
}

// ---------------------------------------------------------------- fetch

const pending = new Map();        // request id -> result callback
let nextId = 1;
internal.fetchDone = (id, result) => {
  const cb = pending.get(id);
  if (!cb) return;
  pending.delete(id);
  cb(result);
};

const FORBIDDEN_REQUEST_HEADERS = new Set([
  'host', 'content-length', 'connection', 'keep-alive', 'accept-encoding', 'transfer-encoding',
  'te', 'trailer', 'upgrade', 'expect', 'proxy-connection',
]);
const BODY_HEADERS = ['content-type', 'content-length', 'content-encoding', 'content-language', 'content-location'];

function requestHeaderText(headers) {
  let text = '';
  for (const [k, v] of sortedEntries(hst.get(headers))) if (!FORBIDDEN_REQUEST_HEADERS.has(k)) text += `${k}: ${v}\r\n`;
  return text;
}
const netError = (msg, cause) => new TypeError(msg, { cause: cause instanceof Error ? cause : new Error(String(cause)) });

function fetch(input, init = undefined) {
  return new Promise((resolve, reject) => {
    let req;
    try { req = new Request(input, init); } catch (e) { reject(e); return; }
    const q = rq.get(req);
    const sig = q.signal;
    const ss = sigState.get(sig);
    if (isAborted(sig)) { unlinkSignal(sig); reject(ss.reason); return; }

    const b = bodyOf.get(req);
    let payload = b.bytes;
    if (payload !== null) b.used = true;
    let url = q.url, method = q.method, headers = new Headers(q.headers);
    let curId = 0, finished = false, hops = 0, redirected = false;

    const finish = () => {
      finished = true;
      removeListener(sig, 'abort', onAbort, false);
      unlinkSignal(sig);
    };
    const onAbort = () => {
      if (finished) return;
      if (curId) { pending.delete(curId); host.fetchCancel(curId); curId = 0; }
      finish();
      reject(ss.reason);
    };
    const fail = err => { if (finished) return; finish(); reject(err); };
    const succeed = (bytes, status, statusText, hdrs) => {
      if (finished) return;
      finish();
      resolve(makeResponse({
        status, statusText, headers: hdrs, type: 'basic', redirected,
        url: url.indexOf('#') < 0 ? url : url.slice(0, url.indexOf('#')),
      }, bytes));
    };
    addListener(sig, 'abort', onAbort, { once: true });

    const send = () => {
      const scheme = url.slice(0, url.indexOf(':')).toLowerCase();
      if (scheme === 'data') {
        const d = parseDataURL(url);
        if (!d) { fail(netError('fetch failed', 'invalid data: URL')); return; }
        const h = newHeaders('none');
        h.append('content-type', d.type);
        hst.get(h).guard = 'immutable';
        succeed(method === 'HEAD' ? null : d.bytes, 200, 'OK', h);
        return;
      }
      if (scheme !== 'http' && scheme !== 'https') {
        fail(new TypeError('fetch failed: unsupported scheme', { cause: new Error(`unsupported scheme: ${scheme}`) }));
        return;
      }
      const id = nextId++;
      curId = id;
      pending.set(id, onResult);
      if (!headers.has('accept')) headers.set('accept', '*/*');
      const i = url.indexOf('#');
      try {
        host.fetch(id, method, i < 0 ? url : url.slice(0, i), requestHeaderText(headers),
          payload === null ? undefined : toAB(payload.slice()));
      } catch (e) {
        pending.delete(id);
        curId = 0;
        fail(e instanceof TypeError ? new TypeError('fetch failed', { cause: e }) : e);
      }
    };

    function onResult(res) {
      curId = 0;
      if (finished) return;
      if (!res || res.error !== undefined) { fail(netError('fetch failed', res ? res.error : 'no result')); return; }
      const hdrs = parseHeaderBlock(res.headers);
      const status = res.status | 0;
      const loc = REDIRECT_STATUS.has(status) ? hdrs.get('location') : null;
      if (loc !== null) {
        if (q.redirect === 'error') { fail(netError('fetch failed', 'unexpected redirect')); return; }
        if (q.redirect === 'follow') {
          if (++hops > 20) { fail(netError('fetch failed', 'redirect count exceeded')); return; }
          let next;
          try { next = new URL(loc, url); } catch (e) { fail(netError('fetch failed', e)); return; }
          const cur = new URL(url);
          if (cur.origin !== next.origin) {
            headers.delete('authorization'); headers.delete('cookie'); headers.delete('proxy-authorization');
          }
          if (status === 303 ? (method !== 'GET' && method !== 'HEAD') : ((status === 301 || status === 302) && method === 'POST')) {
            method = 'GET'; payload = null;
            for (const n of BODY_HEADERS) headers.delete(n);
          }
          if (next.hash === '' && cur.hash !== '') next.hash = cur.hash;
          url = next.href;
          redirected = true;
          send();
          return;
        }
      }
      const noBody = method === 'HEAD' || NULL_BODY.includes(status);
      succeed(noBody ? null : (res.body ? new Uint8Array(res.body) : new Uint8Array(0)),
        status, res.statusText ?? '', hdrs);
    }
    send();
  });
}
const REDIRECT_STATUS = new Set([301, 302, 303, 307, 308]);

// ---------------------------------------------------------------- structuredClone

const dce = msg => new DOMException(msg, 'DataCloneError');
const TA = Object.getPrototypeOf(Int8Array);
const taTag = Object.getOwnPropertyDescriptor(TA.prototype, Symbol.toStringTag).get;
const taCtors = {
  Int8Array, Uint8Array, Uint8ClampedArray, Int16Array, Uint16Array, Int32Array, Uint32Array,
  Float32Array, Float64Array, BigInt64Array, BigUint64Array,
};
if (typeof Float16Array === 'function') taCtors.Float16Array = Float16Array;
const errCtors = { Error, EvalError, RangeError, ReferenceError, SyntaxError, TypeError, URIError };
const reSource = Object.getOwnPropertyDescriptor(RegExp.prototype, 'source').get;
const reFlags = Object.getOwnPropertyDescriptor(RegExp.prototype, 'flags').get;
const dvLen = Object.getOwnPropertyDescriptor(DataView.prototype, 'byteLength').get;
const put = (o, k, v) => {
  if (k === '__proto__') Object.defineProperty(o, k, { value: v, writable: true, enumerable: true, configurable: true });
  else o[k] = v;
};
const hidden = v => ({ value: v, writable: true, configurable: true, enumerable: false });

function copyAB(ab) {
  if (ab.detached) throw dce('An ArrayBuffer is detached and could not be cloned.');
  if (ab.resizable) {
    const r = new ArrayBuffer(ab.byteLength, { maxByteLength: ab.maxByteLength });
    new Uint8Array(r).set(new Uint8Array(ab));
    return r;
  }
  return ab.slice(0);
}
function cloneError(v, mem) {
  const n = v.name;
  const C = Object.prototype.hasOwnProperty.call(errCtors, n) ? errCtors[n] : Error;
  const md = Object.getOwnPropertyDescriptor(v, 'message');
  const e = md && 'value' in md ? new C(String(md.value)) : new C();
  mem.set(v, e);
  const st = v.stack;
  if (typeof st === 'string') Object.defineProperty(e, 'stack', hidden(st));
  const cd = Object.getOwnPropertyDescriptor(v, 'cause');
  if (cd && 'value' in cd) Object.defineProperty(e, 'cause', hidden(cloneValue(cd.value, mem)));
  return e;
}
const remember = (mem, v, c) => (mem.set(v, c), c);

function cloneValue(v, mem) {
  switch (typeof v) {
    case 'undefined': case 'boolean': case 'number': case 'string': case 'bigint': return v;
    case 'symbol': throw dce(`${String(v)} could not be cloned.`);
    case 'function': throw dce(`${Function.prototype.toString.call(v)} could not be cloned.`);
  }
  if (v === null) return null;
  const seen = mem.get(v);
  if (seen !== undefined) return seen;
  if (Array.isArray(v)) {
    const out = remember(mem, v, new Array(v.length));
    for (const k of Object.keys(v)) put(out, k, cloneValue(v[k], mem));
    return out;
  }
  const ta = taTag.call(v);
  if (ta !== undefined) {
    const buf = cloneValue(v.buffer, mem);
    return remember(mem, v, new taCtors[ta](buf, v.byteOffset, v.length));
  }
  const tag = objToString.call(v).slice(8, -1);
  try {
    switch (tag) {
      case 'Boolean': return remember(mem, v, new Boolean(Boolean.prototype.valueOf.call(v)));
      case 'Number': return remember(mem, v, new Number(Number.prototype.valueOf.call(v)));
      case 'String': return remember(mem, v, new String(String.prototype.valueOf.call(v)));
      case 'BigInt': return remember(mem, v, Object(BigInt.prototype.valueOf.call(v)));
      case 'Date': return remember(mem, v, new Date(Date.prototype.getTime.call(v)));
      case 'RegExp': return remember(mem, v, new RegExp(reSource.call(v), reFlags.call(v)));
      case 'Symbol': Symbol.prototype.valueOf.call(v); break;
      case 'ArrayBuffer': if (isAB(v)) return remember(mem, v, copyAB(v)); break;
      case 'DataView': dvLen.call(v); break;
      case 'Map': Map.prototype.has.call(v, 0); break;
      case 'Set': Set.prototype.has.call(v, 0); break;
    }
  } catch (e) {
    if (e instanceof DOMException) throw e;
    // brand check failed: fall through and treat it as a plain object
    return clonePlain(v, mem);
  }
  switch (tag) {
    case 'Symbol': throw dce('Symbol object could not be cloned.');
    case 'DataView': {
      const buf = cloneValue(v.buffer, mem);
      return remember(mem, v, new DataView(buf, v.byteOffset, v.byteLength));
    }
    case 'Map': {
      const out = remember(mem, v, new Map());
      for (const [k, x] of [...Map.prototype.entries.call(v)]) out.set(cloneValue(k, mem), cloneValue(x, mem));
      return out;
    }
    case 'Set': {
      const out = remember(mem, v, new Set());
      for (const x of [...Set.prototype.values.call(v)]) out.add(cloneValue(x, mem));
      return out;
    }
    case 'Error':
      if (typeof Error.isError === 'function' ? Error.isError(v) : v instanceof Error) return cloneError(v, mem);
      break;
    case 'DOMException':
      if (v instanceof DOMException) return remember(mem, v, new DOMException(v.message, v.name));
      break;
    case 'Promise': case 'WeakMap': case 'WeakSet': case 'WeakRef': case 'FinalizationRegistry':
    case 'SharedArrayBuffer':
      throw dce(`#<${tag}> could not be cloned.`);
  }
  return clonePlain(v, mem);
}
function clonePlain(v, mem) {
  if (platform.has(v)) {
    let n = 'Object';
    try { n = v.constructor.name || n; } catch { /* keep default */ }
    throw dce(`#<${n}> could not be cloned.`);
  }
  const out = remember(mem, v, {});
  for (const k of Object.keys(v)) put(out, k, cloneValue(v[k], mem));
  return out;
}

function structuredClone(value, options = undefined) {
  argc(arguments.length, 1, "Failed to execute 'structuredClone'");
  let transfer = [];
  if (options !== undefined && options !== null) {
    if (!isObj(options)) throw new TypeError("Failed to execute 'structuredClone': parameter 2 is not of type 'StructuredSerializeOptions'.");
    if (options.transfer !== undefined) transfer = [...options.transfer];
  }
  const mem = new Map();
  for (const ab of transfer) {
    if (!isObj(ab) || !isAB(ab)) throw dce('Value not transferable');
    if (mem.has(ab)) throw dce('Transfer list contains duplicate entries');
    mem.set(ab, copyAB(ab));
  }
  const out = cloneValue(value, mem);
  // Detach the originals (the clone already owns a copy of the data).
  for (const ab of transfer) {
    if (typeof ab.transfer === 'function') { try { ab.transfer(0); } catch { /* not detachable */ } }
  }
  return out;
}

function reportError(error) {
  argc(arguments.length, 1, "Failed to execute 'reportError'");
  internal.reportError(error);
}

// ---------------------------------------------------------------- install

def('Event', Event);
def('EventTarget', EventTarget);
def('CustomEvent', CustomEvent);
def('AbortController', AbortController);
def('AbortSignal', AbortSignal);
def('Headers', Headers);
def('Request', Request);
def('Response', Response);
def('fetch', fetch);
def('structuredClone', structuredClone);
def('atob', atob);
def('btoa', btoa);
def('reportError', reportError);
