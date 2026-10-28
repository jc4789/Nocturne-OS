/* Encoding API: TextEncoder (UTF-8) and TextDecoder of every encoding over the
   native Encoding Standard layer (web_encoding.c). No host filesystem or
   process facilities are exposed. */
{
    const slots = new WeakMap(), encoders = new WeakSet();
    const apply = Reflect.apply, U8 = Uint8Array, isView = ArrayBuffer.isView;
    const wmGet = WeakMap.prototype.get, wmSet = WeakMap.prototype.set;
    const wsHas = WeakSet.prototype.has, wsAdd = WeakSet.prototype.add;
    const str = String, lower = String.prototype.toLowerCase;
    const nativeDecode = host.decodeUTF8, nativeEncode = host.encodeUTF8, nativeEncodeInto = host.encodeUTF8Into;
    const getEncoding = host.getEncoding, nativeDecoder = host.textDecoder, nativeTextDecode = host.textDecode;
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
        if (length === undefined) length = size;
        // A detached buffer (and each view of it) holds no bytes.
        if (!length) return new U8(0);
        const view = new U8(buffer, offset, length);
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
            const name = getEncoding(label);
            if (name === null || name === 'replacement') throw new RangeError('Unsupported text encoding');
            // The decoder instance is made at the first decode that needs one.
            apply(wmSet, slots, [this, {name, encoding:apply(lower, name, []), fatal, ignoreBOM,
                streaming:false, decoder:null}]);
        }
        get encoding() { const s=state(this); if(!s) throw new TypeError('Illegal invocation'); return s.encoding; }
        get fatal() { const s=state(this); if(!s) throw new TypeError('Illegal invocation'); return s.fatal; }
        get ignoreBOM() { const s=state(this); if(!s) throw new TypeError('Illegal invocation'); return s.ignoreBOM; }
        decode(input = undefined, options = {}) {
            const s = state(this); if (!s) throw new TypeError('Illegal invocation');
            const a = bytes(input);
            const stream = !!dictionary(options).stream;
            // do not flush: a call after a flushing one starts a new instance,
            // which the native decoder already is after end-of-queue.
            const fresh = !s.streaming;
            s.streaming = stream;
            const buffer = apply(taBuffer,a,[]), offset = apply(taOffset,a,[]), length = apply(taLength,a,[]);
            if (fresh && !stream && s.name === 'UTF-8')
                return nativeDecode(buffer, offset, length, s.fatal, s.ignoreBOM);
            if (!s.decoder) s.decoder = nativeDecoder(s.name, s.fatal, s.ignoreBOM);
            // A fatal error throws and the instance starts afresh: the bytes
            // after the error are not kept for a later call.
            try { return nativeTextDecode(s.decoder, buffer, offset, length, stream); }
            catch (error) { s.streaming = false; throw error; }
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
