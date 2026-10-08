/* HTML Base64 utility methods and read-only effective-domain compatibility.
   Domain relaxation is not backed by Nocturne's origin model or a PSL: it is
   explicitly rejected, never emulated by changing a JS-only origin value. */
(function () {
    'use strict';
    const global = globalThis, StringImpl = String, TypeErrorImpl = TypeError, Exception = DOMException;
    const call = Reflect.apply, define = Object.defineProperty;
    const charCode = String.prototype.charCodeAt, fromCode = String.fromCharCode;
    const endsWith = String.prototype.endsWith, join = Array.prototype.join;
    const URLImpl = URL;
    const originGet = Object.getOwnPropertyDescriptor(URLImpl.prototype, 'origin').get;
    const hostnameGet = Object.getOwnPropertyDescriptor(URLImpl.prototype, 'hostname').get;
    const documentBrand = documentBridge.brand;
    const alphabet = 'ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/';
    // The sole native browsing-context global is an actual Window/EventTarget,
    // not a duck-typed stand-in. Preserve its own native properties and event
    // wrappers, while making the inherited interface available to libraries
    // which distinguish Window from Document targets (for example loaders).
    // No WindowProxy, child browsing context, or cross-origin access is added.
    class Window extends EventTarget {
        constructor() { throw new TypeErrorImpl('Illegal Window constructor'); }
    }
    define(Window.prototype, Symbol.toStringTag, { value: 'Window', configurable: true });
    Object.setPrototypeOf(global, Window.prototype);
    define(global, 'Window', { value: Window, configurable: true, writable: true });
    // Do not leave Window's own captured-method wrappers bypassing later
    // EventTarget prototype instrumentation. Preserve bare global calls and
    // captured original functions, but let ordinary member lookups inherit.
    for (const name of ['addEventListener', 'removeEventListener', 'dispatchEvent']) {
        const descriptor = Object.getOwnPropertyDescriptor(EventTarget.prototype, name);
        const method = descriptor.value;
        const wrapped = { [name](...args) {
            return call(method, this == null ? global : this, args);
        }}[name];
        define(wrapped, 'length', { value: method.length, configurable: true });
        define(EventTarget.prototype, name, { ...descriptor, value: wrapped });
        delete global[name];
    }
    function string(value) {
        if (typeof value === 'symbol') throw new TypeErrorImpl('Cannot convert Symbol to DOMString');
        return StringImpl(value);
    }
    function receiver(value) {
        if (value != null && value !== global) throw new TypeErrorImpl('Window receiver required');
    }
    function invalid() { throw new Exception('The string is not valid binary/Base64 data', 'InvalidCharacterError'); }
    function digit(c) {
        return c >= 65 && c <= 90 ? c - 65 : c >= 97 && c <= 122 ? c - 71 :
               c >= 48 && c <= 57 ? c + 4 : c === 43 ? 62 : c === 47 ? 63 : -1;
    }
    const btoa = { btoa(data) {
        receiver(this);
        if (!arguments.length) throw new TypeErrorImpl('btoa requires one argument');
        data = string(data);
        const parts = []; let chunk = '';
        for (let i = 0; i < data.length; i += 3) {
            const a = call(charCode, data, [i]), b = i + 1 < data.length ? call(charCode, data, [i + 1]) : 0;
            const c = i + 2 < data.length ? call(charCode, data, [i + 2]) : 0;
            if (a > 255 || b > 255 || c > 255) invalid();
            chunk += alphabet[a >> 2] + alphabet[((a & 3) << 4) | (b >> 4)] +
                     (i + 1 < data.length ? alphabet[((b & 15) << 2) | (c >> 6)] : '=') +
                     (i + 2 < data.length ? alphabet[c & 63] : '=');
            if (chunk.length >= 4096) { parts[parts.length] = chunk; chunk = ''; }
        }
        if (chunk) parts[parts.length] = chunk;
        return call(join, parts, ['']);
    }}.btoa;
    const atob = { atob(data) {
        receiver(this);
        if (!arguments.length) throw new TypeErrorImpl('atob requires one argument');
        data = string(data);
        // Infra forgiving-base64: remove only ASCII whitespace, not JS \s.
        const cleanParts = []; let cleanChunk = '';
        for (let i = 0; i < data.length; i++) {
            const c = call(charCode, data, [i]);
            if (c === 9 || c === 10 || c === 12 || c === 13 || c === 32) continue;
            cleanChunk += data[i];
            if (cleanChunk.length >= 4096) { cleanParts[cleanParts.length] = cleanChunk; cleanChunk = ''; }
        }
        if (cleanChunk) cleanParts[cleanParts.length] = cleanChunk;
        data = call(join, cleanParts, ['']);
        let length = data.length;
        if (!(length % 4) && data[length - 1] === '=') {
            length--; if (data[length - 1] === '=') length--;
        }
        if (length % 4 === 1) invalid();
        const parts = []; let chunk = '', buffer = 0, bits = 0;
        for (let i = 0; i < length; i++) {
            const value = digit(call(charCode, data, [i]));
            if (value < 0) invalid();
            buffer = (buffer << 6) | value; bits += 6;
            if (bits >= 8) {
                bits -= 8; chunk += call(fromCode, StringImpl, [(buffer >> bits) & 255]);
                buffer &= (1 << bits) - 1;
                if (chunk.length >= 4096) { parts[parts.length] = chunk; chunk = ''; }
            }
        }
        // Unused tail bits are discarded, not required to be zero.
        if (chunk) parts[parts.length] = chunk;
        return call(join, parts, ['']);
    }}.atob;
    for (const [name, value] of [['atob', atob], ['btoa', btoa]])
        define(global, name, { value, configurable: true, enumerable: true, writable: true });

    function domain(receiver) {
        documentBrand(receiver);
        // Never trust an overridden document.URL, location, or global URL binding.
        const nativeURL = rawDom('get', receiver, 'URL');
        try {
            const url = new URLImpl(nativeURL), origin = call(originGet, url, []);
            if (origin === 'null') return '';
            return call(hostnameGet, new URLImpl(origin), []);
        } catch (e) {
            if (e instanceof TypeErrorImpl) return '';
            throw e;
        }
    }
    function host(value) {
        if (!value) return null;
        // URL parsing is not host parsing: do not let path/userinfo/port or
        // whitespace removal make an invalid host into a same-host write.
        for (let i = 0; i < value.length; i++) {
            const c = call(charCode, value, [i]);
            if (c <= 32 || c === 127 || c === 35 || c === 47 || c === 63 || c === 64 || c === 92) return null;
        }
        if (value[0] !== '[') {
            for (let i = 0; i < value.length; i++)
                if (value[i] === ':' || value[i] === '[' || value[i] === ']') return null;
        } else if (value[value.length - 1] !== ']') return null;
        try { return call(hostnameGet, new URLImpl('http://' + value + '/'), []); }
        catch (e) { if (e instanceof TypeErrorImpl) return null; throw e; }
    }
    define(Document.prototype, 'domain', { configurable: true, enumerable: true,
        get() { return domain(this); },
        set(value) {
            documentBrand(this); value = string(value);
            if (this !== document) throw new Exception('The document has no browsing context', 'SecurityError');
            const current = domain(this);
            if (!current) throw new Exception('The document has an opaque origin', 'SecurityError');
            const requested = host(value);
            if (!requested) throw new Exception('The value is not a valid host', 'SecurityError');
            // Nocturne has no origin-domain relaxation. An equal-host write is
            // a safe compatibility no-op; it never clears ports or affects SOP.
            if (requested === current) return;
            if (call(endsWith, current, ['.' + requested]))
                throw new Exception('Domain relaxation and public suffix validation are not implemented', 'NotSupportedError');
            throw new Exception('The value is not the document host', 'SecurityError');
        }
    });
})();
