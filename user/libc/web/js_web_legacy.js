/* Window event-method wrappers and read-only effective-domain compatibility
   (atob/btoa are shared with Workers in js_atob.js).
   Domain relaxation is not backed by Nocturne's origin model or a PSL: it is
   explicitly rejected, never emulated by changing a JS-only origin value. */
(function () {
    'use strict';
    const global = globalThis, StringImpl = String, TypeErrorImpl = TypeError, Exception = DOMException;
    const call = Reflect.apply, define = Object.defineProperty;
    const charCode = String.prototype.charCodeAt;
    const endsWith = String.prototype.endsWith;
    const URLImpl = URL;
    const originGet = Object.getOwnPropertyDescriptor(URLImpl.prototype, 'origin').get;
    const hostnameGet = Object.getOwnPropertyDescriptor(URLImpl.prototype, 'hostname').get;
    const documentBrand = documentBridge.brand;
    // frameBridge owns the actual Window interface and its native-backed
    // named-properties prototype. Replacing that prototype here would sever
    // every inherited id/name lookup after the complete bootstrap has loaded.
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
    function domain(receiver) {
        documentBrand(receiver);
        // Never trust an overridden document.URL, location, or global URL binding.
        const nativeURL = rawDom.get(receiver, 'URL');
        try {
            const url = new URLImpl(nativeURL), origin = call(originGet, url, []);
            if (origin === 'null') return '';
            return call(hostnameGet, new URLImpl(origin), []);
        } catch (e) {
            if (e instanceof TypeErrorImpl) return '';
            throw e;
        }
    }
    function parseDomainHost(value) {
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
            if (host.sandboxFlags(this) & 256) throw new Exception('Sandbox forbids document.domain writes', 'SecurityError');
            if (this !== document) throw new Exception('The document has no browsing context', 'SecurityError');
            const current = domain(this);
            if (!current) throw new Exception('The document has an opaque origin', 'SecurityError');
            const requested = parseDomainHost(value);
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
