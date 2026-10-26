/* HTML atob/btoa, shared by Window and Worker globals
   (WindowOrWorkerGlobalScope). Embedded after DOMException in both realms,
   where the private host's native base64 (js_encoding.c) is in scope.
   The embedders' include pattern takes only [a-z_] names, hence no "64". */
(function () {
    'use strict';
    const global = globalThis, StringImpl = String, TypeErrorImpl = TypeError, Exception = DOMException;
    const define = Object.defineProperty, encode = host.btoa, decode = host.atob;
    function string(value) {
        if (typeof value === 'symbol') throw new TypeErrorImpl('Cannot convert Symbol to DOMString');
        return StringImpl(value);
    }
    function receiver(value) {
        if (value != null && value !== global) throw new TypeErrorImpl('Illegal invocation');
    }
    function invalid() { throw new Exception('The string is not valid binary/Base64 data', 'InvalidCharacterError'); }
    const btoa = { btoa(data) {
        receiver(this);
        if (!arguments.length) throw new TypeErrorImpl('btoa requires one argument');
        const result = encode(string(data));
        return result === null ? invalid() : result;
    }}.btoa;
    const atob = { atob(data) {
        receiver(this);
        if (!arguments.length) throw new TypeErrorImpl('atob requires one argument');
        // Infra forgiving-base64; unused tail bits are discarded.
        const result = decode(string(data));
        return result === null ? invalid() : result;
    }}.atob;
    for (const [name, value] of [['atob', atob], ['btoa', btoa]])
        define(global, name, { value, configurable: true, enumerable: true, writable: true });
})();
