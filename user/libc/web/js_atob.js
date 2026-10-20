/* HTML forgiving-base64 atob/btoa, shared by Window and Worker globals
   (WindowOrWorkerGlobalScope). Embedded after DOMException in both realms.
   The embedders' include pattern takes only [a-z_] names, hence no "64". */
(function () {
    'use strict';
    const global = globalThis, StringImpl = String, TypeErrorImpl = TypeError, Exception = DOMException;
    const call = Reflect.apply, define = Object.defineProperty;
    const charCode = String.prototype.charCodeAt, fromCode = String.fromCharCode;
    const join = Array.prototype.join;
    const alphabet = 'ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/';
    function string(value) {
        if (typeof value === 'symbol') throw new TypeErrorImpl('Cannot convert Symbol to DOMString');
        return StringImpl(value);
    }
    function receiver(value) {
        if (value != null && value !== global) throw new TypeErrorImpl('Illegal invocation');
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
})();
