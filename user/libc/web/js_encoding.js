/* UTF-8 Encoding API. No host filesystem or process facilities are exposed. */
{
    const slots = new WeakMap(), encoders = new WeakSet();
    const apply = Reflect.apply, U8 = Uint8Array, isView = ArrayBuffer.isView;
    const wmGet = WeakMap.prototype.get, wmSet = WeakMap.prototype.set;
    const wsHas = WeakSet.prototype.has, wsAdd = WeakSet.prototype.add;
    const str = String, fromCP = String.fromCodePoint;
    const push = Array.prototype.push, join = Array.prototype.join;
    const nativeDecode = host.decodeUTF8, nativeEncode = host.encodeUTF8, nativeEncodeInto = host.encodeUTF8Into;
    const assign = Object.assign, regexTest = RegExp.prototype.test;
    const utf8Label = /^[\t\n\f\r ]*(?:utf-8|utf8|unicode-1-1-utf-8|unicode11utf8|unicode20utf8|x-unicode20utf8)[\t\n\f\r ]*$/i;
    const getter = (proto, name) => Object.getOwnPropertyDescriptor(proto, name).get;
    const abLength = getter(ArrayBuffer.prototype, 'byteLength');
    const resizable = Object.getOwnPropertyDescriptor(ArrayBuffer.prototype, 'resizable');
    const SAB = typeof SharedArrayBuffer === 'function' ? SharedArrayBuffer : null;
    const sabLength = SAB ? getter(SAB.prototype, 'byteLength') : null;
    const sabGrowable = SAB ? Object.getOwnPropertyDescriptor(SAB.prototype, 'growable') : null;
    const ta = Object.getPrototypeOf(U8.prototype), tag = getter(ta, Symbol.toStringTag);
    const taBuffer = getter(ta, 'buffer'), taOffset = getter(ta, 'byteOffset'), taLength = getter(ta, 'byteLength');
    const dvBuffer = getter(DataView.prototype, 'buffer'), dvOffset = getter(DataView.prototype, 'byteOffset');
    const dvLength = getter(DataView.prototype, 'byteLength');
    const state = value => apply(wmGet, slots, [value]);
    const text = value => { if (typeof value === 'symbol') throw new TypeError('Expected a string'); return str(value); };
    const dictionary = value => {
        if (value == null) return {};
        if (typeof value !== 'object' && typeof value !== 'function') throw new TypeError('Expected a dictionary');
        return value;
    };
    const encoderBrand = value => { if (!apply(wsHas, encoders, [value])) throw new TypeError('Illegal invocation'); };
    // AllowShared sources are copied before decoding: another agent may write
    // them concurrently, and the decoder reads its input more than once.
    const bytes = (input, writable = false) => {
        if (input === undefined) return new U8(0);
        let buffer = input, offset = 0, length;
        if (isView(input)) {
            const typed = apply(tag, input, []) !== undefined;
            buffer = apply(typed ? taBuffer : dvBuffer, input, []);
            offset = apply(typed ? taOffset : dvOffset, input, []);
            length = apply(typed ? taLength : dvLength, input, []);
        }
        let size, shared = false;
        try { size = apply(abLength, buffer, []); }
        catch (error) { if (!sabLength) throw error; size = apply(sabLength, buffer, []); shared = true; }
        const growable = shared ? sabGrowable : resizable;
        if (growable && apply(growable.get, buffer, [])) throw new TypeError('Resizable buffer is not supported');
        const view = new U8(buffer, offset, length === undefined ? size : length);
        return shared && !writable ? new U8(view) : view;
    };
    class TextEncoder {
        constructor() { apply(wsAdd, encoders, [this]); }
        get encoding() { encoderBrand(this); return 'utf-8'; }
        encode(input = '') {
            encoderBrand(this);
            return new U8(nativeEncode(text(input)));
        }
        encodeInto(input, destination) {
            encoderBrand(this);
            if (arguments.length < 2) throw new TypeError('encodeInto requires source and destination');
            const s = text(input);
            if (apply(tag, destination, []) !== 'Uint8Array') throw new TypeError('Expected Uint8Array');
            const output = bytes(destination, true);
            return nativeEncodeInto(s, apply(taBuffer, output, []), apply(taOffset, output, []), apply(taLength, output, []));
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
