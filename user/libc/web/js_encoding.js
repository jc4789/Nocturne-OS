/* UTF-8 Encoding API. No host filesystem or process facilities are exposed. */
{
    const slots = new WeakMap(), encoders = new WeakSet();
    const apply = Reflect.apply, U8 = Uint8Array, isView = ArrayBuffer.isView;
    const wmGet = WeakMap.prototype.get, wmSet = WeakMap.prototype.set;
    const wsHas = WeakSet.prototype.has, wsAdd = WeakSet.prototype.add;
    const str = String, charCodeAt = String.prototype.charCodeAt, fromCP = String.fromCodePoint;
    const push = Array.prototype.push, join = Array.prototype.join;
    const nativeDecode = host.decodeUTF8;
    const assign = Object.assign, regexTest = RegExp.prototype.test;
    const utf8Label = /^[\t\n\f\r ]*(?:utf-8|utf8|unicode-1-1-utf-8|unicode11utf8|unicode20utf8|x-unicode20utf8)[\t\n\f\r ]*$/i;
    const getter = (proto, name) => Object.getOwnPropertyDescriptor(proto, name).get;
    const abLength = getter(ArrayBuffer.prototype, 'byteLength');
    const resizable = Object.getOwnPropertyDescriptor(ArrayBuffer.prototype, 'resizable');
    const ta = Object.getPrototypeOf(U8.prototype), tag = getter(ta, Symbol.toStringTag);
    const taBuffer = getter(ta, 'buffer'), taOffset = getter(ta, 'byteOffset'), taLength = getter(ta, 'byteLength');
    const dvBuffer = getter(DataView.prototype, 'buffer'), dvOffset = getter(DataView.prototype, 'byteOffset');
    const dvLength = getter(DataView.prototype, 'byteLength');
    const state = value => apply(wmGet, slots, [value]);
    const code = (s, i) => apply(charCodeAt, s, [i]);
    const text = value => { if (typeof value === 'symbol') throw new TypeError('Expected a string'); return str(value); };
    const dictionary = value => {
        if (value == null) return {};
        if (typeof value !== 'object' && typeof value !== 'function') throw new TypeError('Expected a dictionary');
        return value;
    };
    const encoderBrand = value => { if (!apply(wsHas, encoders, [value])) throw new TypeError('Illegal invocation'); };
    const bytes = input => {
        if (input === undefined) return new U8(0);
        let buffer = input, offset = 0, length;
        if (isView(input)) {
            const typed = apply(tag, input, []) !== undefined;
            buffer = apply(typed ? taBuffer : dvBuffer, input, []);
            offset = apply(typed ? taOffset : dvOffset, input, []);
            length = apply(typed ? taLength : dvLength, input, []);
        }
        const size = apply(abLength, buffer, []);
        if (resizable && apply(resizable.get, buffer, [])) throw new TypeError('Resizable buffer is not supported');
        // SharedArrayBuffer is not exposed in Nocturne. This getter rejects it if
        // the bootstrap is used in another runtime; AllowShared remains unsupported.
        return new U8(buffer, offset, length === undefined ? size : length);
    };
    function encodeString(s, destination, capacity) {
        let read = 0, written = 0;
        while (read < s.length) {
            let cp = code(s, read), count = 1;
            if (cp >= 0xd800 && cp <= 0xdbff) {
                const lo = code(s, read + 1);
                if (lo >= 0xdc00 && lo <= 0xdfff) { cp = 0x10000 + ((cp - 0xd800) << 10) + lo - 0xdc00; count = 2; }
                else cp = 0xfffd;
            } else if (cp >= 0xdc00 && cp <= 0xdfff) cp = 0xfffd;
            const n = cp < 0x80 ? 1 : cp < 0x800 ? 2 : cp < 0x10000 ? 3 : 4;
            if (written + n > capacity) break;
            if (destination) {
                if (n === 1) destination[written] = cp;
                else {
                    destination[written] = (n === 2 ? 0xc0 : n === 3 ? 0xe0 : 0xf0) | (cp >> (6 * (n - 1)));
                    for (let j = 1; j < n; j++) destination[written + j] = 0x80 | (cp >> (6 * (n - j - 1))) & 63;
                }
            }
            written += n; read += count;
        }
        return {read, written};
    }
    class TextEncoder {
        constructor() { apply(wsAdd, encoders, [this]); }
        get encoding() { encoderBrand(this); return 'utf-8'; }
        encode(input = '') {
            encoderBrand(this);
            const s = text(input), length = encodeString(s, null, Infinity).written;
            const output = new U8(length);
            encodeString(s, output, length);
            return output;
        }
        encodeInto(input, destination) {
            encoderBrand(this);
            if (arguments.length < 2) throw new TypeError('encodeInto requires source and destination');
            const s = text(input);
            if (apply(tag, destination, []) !== 'Uint8Array') throw new TypeError('Expected Uint8Array');
            const output = bytes(destination);
            return encodeString(s, output, apply(taLength, output, []));
        }
    }
    class TextDecoder {
        constructor(label = 'utf-8', options = {}) {
            label = text(label);
            options = dictionary(options);
            const fatal = !!options.fatal, ignoreBOM = !!options.ignoreBOM;
            if (!apply(regexTest, utf8Label, [label]))
                throw new RangeError('Unsupported text encoding');
            apply(wmSet, slots, [this, {fatal, ignoreBOM, streaming:false, bom:false,
                needed:0, seen:0, cp:0, lower:0x80, upper:0xbf}]);
        }
        get encoding() { if (!state(this)) throw new TypeError('Illegal invocation'); return 'utf-8'; }
        get fatal() { const s=state(this); if(!s) throw new TypeError('Illegal invocation'); return s.fatal; }
        get ignoreBOM() { const s=state(this); if(!s) throw new TypeError('Illegal invocation'); return s.ignoreBOM; }
        decode(input, options = {}) {
            const s = state(this); if (!s) throw new TypeError('Illegal invocation');
            const a = bytes(input), out = [], chunks = [];
            const stream = !!dictionary(options).stream;
            const fresh = !s.streaming;
            if (fresh) assign(s, {bom:false, needed:0, seen:0, cp:0, lower:0x80, upper:0xbf});
            s.streaming = stream;
            if (fresh && !stream)
                return nativeDecode(apply(taBuffer,a,[]),apply(taOffset,a,[]),apply(taLength,a,[]),s.fatal,s.ignoreBOM);
            function emit(cp) {
                if (!s.bom) { s.bom = true; if (!s.ignoreBOM && cp === 0xfeff) return; }
                apply(push, out, [fromCP(cp)]);
                // Streaming retains only a small work array, not one JSValue
                // per scalar across a multi-megabyte XHR/Fetch body.
                if(out.length===512){apply(push,chunks,[apply(join,out,[''])]);out.length=0;}
            }
            function error() {
                if (s.fatal) { s.streaming = false; throw new TypeError('Invalid UTF-8'); }
                emit(0xfffd);
            }
            for (let i = 0; i < a.length; i++) {
                const c = a[i];
                if (!s.needed) {
                    if (c <= 0x7f) emit(c);
                    else if (c >= 0xc2 && c <= 0xdf) { s.needed = 1; s.cp = c & 31; }
                    else if (c >= 0xe0 && c <= 0xef) {
                        s.needed = 2; s.cp = c & 15;
                        if (c === 0xe0) s.lower = 0xa0;
                        if (c === 0xed) s.upper = 0x9f;
                    } else if (c >= 0xf0 && c <= 0xf4) {
                        s.needed = 3; s.cp = c & 7;
                        if (c === 0xf0) s.lower = 0x90;
                        if (c === 0xf4) s.upper = 0x8f;
                    } else error();
                } else if (c < s.lower || c > s.upper) {
                    assign(s, {needed:0, seen:0, cp:0, lower:0x80, upper:0xbf});
                    error(); i--;
                } else {
                    s.lower = 0x80; s.upper = 0xbf; s.cp = (s.cp << 6) | (c & 63); s.seen++;
                    if (s.seen === s.needed) { emit(s.cp); s.needed = s.seen = s.cp = 0; }
                }
            }
            if (!s.streaming && s.needed) { s.needed = s.seen = s.cp = 0; error(); }
            if(out.length)apply(push,chunks,[apply(join,out,[''])]);
            return apply(join, chunks, ['']);
        }
    }
    for (const [Type, name] of [[TextEncoder, 'TextEncoder'], [TextDecoder, 'TextDecoder']]) {
        Object.defineProperty(Type.prototype, Symbol.toStringTag, {value:name, configurable:true});
        for (const key of Object.getOwnPropertyNames(Type.prototype)) {
            if (key === 'constructor') continue;
            const d = Object.getOwnPropertyDescriptor(Type.prototype, key); d.enumerable = true;
            Object.defineProperty(Type.prototype, key, d);
        }
    }
    Object.assign(globalThis, {TextEncoder, TextDecoder});
}
