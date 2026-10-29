/* Stand-ins for webcrypto_hosttest.c's realm, loaded before the product scripts: what
   WPT's WebCryptoAPI/ files, testharness.js and js_crypto.js reach that is not under test
   here (timers, the console, the realm's URL and the URL parser, UTF-8 for js_encoding.js).
   Crypto, SubtleCrypto, CryptoKey and DOMException are the product's. */
'use strict';
globalThis.self = globalThis;
globalThis.console = {log: (...a) => print(a.join(' ')), error: (...a) => print(a.join(' ')),
                      warn: (...a) => print(a.join(' ')), info: () => {}, debug: () => {}};
/* The realm's URL, as host.url gives it: check_webcrypto.py sets __realmURL to WPT's https
   origin for .https. files and to its http origin otherwise. */
host.url = () => globalThis.__realmURL;
globalThis.location = {get href() { return globalThis.__realmURL; }, search: '', hash: ''};
/* Base64 for the tests' JWK helpers (Latin-1 strings, as HTML's btoa and atob). */
{
    const chars = 'ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/';
    globalThis.btoa = text => {
        let out = '';
        for (let i = 0; i < text.length; i += 3) {
            const a = text.charCodeAt(i), b = text.charCodeAt(i + 1), c = text.charCodeAt(i + 2);
            const n = a << 16 | (b || 0) << 8 | (c || 0);
            out += chars[n >> 18 & 63] + chars[n >> 12 & 63] + (i + 1 < text.length ? chars[n >> 6 & 63] : '=') +
                (i + 2 < text.length ? chars[n & 63] : '=');
        }
        return out;
    };
    globalThis.atob = text => {
        text = String(text).replace(/[\t\n\f\r ]/g, '').replace(/=+$/, '');
        let out = '', bits = 0, acc = 0;
        for (const ch of text) {
            const v = chars.indexOf(ch);
            if (v < 0) throw new DOMException('Invalid base64', 'InvalidCharacterError');
            acc = acc << 6 | v; bits += 6;
            if (bits >= 8) { bits -= 8; out += String.fromCharCode(acc >> bits & 255); }
        }
        return out;
    };
}
/* structuredClone for the tests' own vectors (plain objects, arrays and buffers) and for the
   [Serializable] interfaces the product registers with cloneData (CryptoKey): their serialize
   steps, a copy of the record, and their deserialize steps, as js_clone.js runs them. */
const serializables = [];
globalThis.cloneData = {registerSerializable(name, brand, serialize, deserialize) {
    serializables.push({name, brand, serialize, deserialize});
}};
globalThis.structuredClone = function clone(value) {
    if (value === null || typeof value !== 'object') return value;
    for (const s of serializables) if (s.brand(value)) return s.deserialize(clone(s.serialize(value)));
    if (value instanceof ArrayBuffer) return value.slice(0);
    if (ArrayBuffer.isView(value)) return new value.constructor(clone(value.buffer), value.byteOffset,
        value instanceof DataView ? value.byteLength : value.length);
    if (Array.isArray(value)) return value.map(clone);
    const out = {};
    for (const key of Object.keys(value)) out[key] = clone(value[key]);
    return out;
};
if (typeof WebAssembly === 'undefined')
    globalThis.WebAssembly = {Memory: class Memory {
        constructor(d) { const n = 65536 * d.initial; this.buffer = d.shared ? new SharedArrayBuffer(n) : new ArrayBuffer(n); }
    }};
/* Only what js_crypto.js's secure-context check reads: the scheme and the host. */
globalThis.URL = class URL {
    constructor(text) {
        const m = /^([a-z][a-z0-9+.-]*:)\/\/(\[[^\]]+\]|[^:/?#]*)(?::\d+)?/.exec(String(text));
        if (!m) throw new TypeError('Invalid URL');
        this.protocol = m[1];
        this.hostname = m[2];
    }
};
/* js_encoding.js's native half, enough for the ASCII and UTF-8 text the tests use */
host.encodeUTF8 = text => {
    const bytes = [];
    for (const ch of text) {
        let c = ch.codePointAt(0);
        if (c >= 0xd800 && c <= 0xdfff) c = 0xfffd;
        if (c < 0x80) bytes.push(c);
        else if (c < 0x800) bytes.push(0xc0 | c >> 6, 0x80 | c & 63);
        else if (c < 0x10000) bytes.push(0xe0 | c >> 12, 0x80 | c >> 6 & 63, 0x80 | c & 63);
        else bytes.push(0xf0 | c >> 18, 0x80 | c >> 12 & 63, 0x80 | c >> 6 & 63, 0x80 | c & 63);
    }
    return new Uint8Array(bytes).buffer;
};
host.decodeUTF8 = (buffer, offset, length) => {
    let s = '';
    for (const v of new Uint8Array(buffer, offset, length)) s += '%' + v.toString(16).padStart(2, '0');
    return decodeURIComponent(s);
};
host.encodeUTF8Into = () => { throw new Error('encodeInto is not part of this check'); };
/* Tasks: webcrypto_hosttest.c runs the next one when no microtask is left. host.postTask's
   (js_crypto.js's crypto task source) in order first, then timers in virtual time. */
{
    const posted = [];
    host.postTask = fn => { posted.push(fn); return posted.length; };
    const timers = new Map();
    let next = 1, now = 0;
    globalThis.setTimeout = (fn, ms = 0, ...args) => {
        const id = next++;
        timers.set(id, {at: now + Math.max(0, Number(ms) || 0), id, fn, args});
        return id;
    };
    globalThis.clearTimeout = id => { timers.delete(id); };
    globalThis.__runTask = () => {
        if (posted.length) { posted.shift()(); return true; }
        let first = null;
        for (const t of timers.values()) if (!first || t.at < first.at || (t.at === first.at && t.id < first.id)) first = t;
        if (!first) return false;
        timers.delete(first.id);
        now = first.at;
        if (typeof first.fn === 'function') first.fn(...first.args);
        return true;
    };
}
