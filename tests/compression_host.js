/* Stand-ins for compression_hosttest.c's realm, loaded before the product scripts:
   what WPT's compression/ files and testharness.js reach that is not under test here
   (timers, fetch of WPT's own files, Blob, Response, postMessage, crypto's random
   values, UTF-8 for js_encoding.js). The streams, the compression contexts and the
   codecs are the product's. */
'use strict';
globalThis.self = globalThis;
globalThis.console = {log: (...a) => print(a.join(' ')), error: (...a) => print(a.join(' ')),
                      warn: (...a) => print(a.join(' ')), info: () => {}, debug: () => {}};
host.promiseThen = (promise, fulfilled, rejected) => Promise.prototype.then.call(promise, fulfilled, rejected);
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
/* Timers in virtual time: compression_hosttest.c runs the next one when no microtask is left. */
{
    const timers = new Map();
    let next = 1, now = 0;
    globalThis.setTimeout = (fn, ms = 0, ...args) => {
        const id = next++;
        timers.set(id, {at: now + Math.max(0, Number(ms) || 0), id, fn, args});
        return id;
    };
    globalThis.clearTimeout = id => { timers.delete(id); };
    globalThis.__runTimer = () => {
        let first = null;
        for (const t of timers.values()) if (!first || t.at < first.at || (t.at === first.at && t.id < first.id)) first = t;
        if (!first) return false;
        timers.delete(first.id);
        now = first.at;
        if (typeof first.fn === 'function') first.fn(...first.args);
        return true;
    };
}
globalThis.crypto = {getRandomValues(view) {
    let x = 0x9e3779b9 ^ view.byteLength;
    const bytes = new Uint8Array(view.buffer, view.byteOffset, view.byteLength);
    for (let i = 0; i < bytes.length; i++) { x ^= x << 13; x ^= x >>> 17; x ^= x << 5; bytes[i] = x & 255; }
    return view;
}};
/* A window's postMessage with a transfer list: here the buffers are detached. */
globalThis.postMessage = (message, origin, transfer = []) => { for (const b of transfer) b.transfer(); };
if (typeof WebAssembly === 'undefined')
    globalThis.WebAssembly = {Memory: class Memory {
        constructor(d) { const n = 65536 * d.initial; this.buffer = d.shared ? new SharedArrayBuffer(n) : new ArrayBuffer(n); }
    }};
{
    const bytesOf = part => part instanceof Uint8Array ? part : ArrayBuffer.isView(part) ?
        new Uint8Array(part.buffer, part.byteOffset, part.byteLength) : part instanceof ArrayBuffer ?
        new Uint8Array(part) : part instanceof Blob ? part._bytes : new TextEncoder().encode(String(part));
    const join = list => {
        let n = 0;
        for (const b of list) n += b.length;
        const out = new Uint8Array(n);
        let at = 0;
        for (const b of list) { out.set(b, at); at += b.length; }
        return out;
    };
    class Blob {
        constructor(parts = []) { this._bytes = join(Array.from(parts, bytesOf)); }
        get size() { return this._bytes.length; }
        async bytes() { return this._bytes.slice(); }
        async arrayBuffer() { return this._bytes.slice().buffer; }
        async text() { return new TextDecoder().decode(this._bytes); }
        stream() { const b = this._bytes.slice(); return new ReadableStream({start(c) { if (b.length) c.enqueue(b); c.close(); }}); }
    }
    globalThis.Blob = Blob;
    class Response {
        constructor(body, status = 200) { this._body = body; this.status = status; this.ok = status >= 200 && status < 300; }
        async bytes() {
            if (this._body instanceof Blob) return this._body.bytes();
            const reader = this._body.getReader(), list = [];
            for (;;) { const {value, done} = await reader.read(); if (done) break; list.push(bytesOf(value)); }
            return join(list);
        }
        async arrayBuffer() { return (await this.bytes()).buffer; }
        async text() { return new TextDecoder().decode(await this.bytes()); }
    }
    globalThis.Response = Response;
    globalThis.fetch = async url => {
        const bytes = host.readFile(String(url).replace(/^https?:\/\/[^/]+/, ''));
        return bytes ? new Response(new Blob([bytes])) : new Response(new Blob([]), 404);
    };
}
