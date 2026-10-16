/* WebSocket over the native webfetch transport (RFC 6455). The service does
   the handshake, framing, masking and control frames; this realm sees whole
   messages as ordinary tasks. https://websockets.spec.whatwg.org/ */
{
    const states = new WeakMap();
    const NativeURL = globalThis.URL, Encoder = globalThis.TextEncoder, MessageEventType = globalThis.MessageEvent;
    const nativeApply = Reflect.apply, defineProperty = Object.defineProperty;
    const weakGet = WeakMap.prototype.get, weakSet = WeakMap.prototype.set, weakHas = WeakMap.prototype.has;
    const encode = Encoder.prototype.encode, wellFormed = String.prototype.toWellFormed;
    const nativeSetTimeout = globalThis.setTimeout;
    const isView = ArrayBuffer.isView, U8 = Uint8Array, typedPrototype = Object.getPrototypeOf(U8.prototype);
    const typedBuffer = Object.getOwnPropertyDescriptor(typedPrototype, 'buffer').get;
    const typedOffset = Object.getOwnPropertyDescriptor(typedPrototype, 'byteOffset').get;
    const typedLength = Object.getOwnPropertyDescriptor(typedPrototype, 'byteLength').get;
    const typedTag = Object.getOwnPropertyDescriptor(typedPrototype, Symbol.toStringTag).get;
    const dvBuffer = Object.getOwnPropertyDescriptor(DataView.prototype, 'buffer').get;
    const dvOffset = Object.getOwnPropertyDescriptor(DataView.prototype, 'byteOffset').get;
    const dvLength = Object.getOwnPropertyDescriptor(DataView.prototype, 'byteLength').get;
    const abLength = Object.getOwnPropertyDescriptor(ArrayBuffer.prototype, 'byteLength').get;
    const abSlice = ArrayBuffer.prototype.slice;
    const eventTypes = new Set(['open', 'message', 'error', 'close']);
    const CONNECTING = 0, OPEN = 1, CLOSING = 2, CLOSED = 3;
    const token = /^[!#$%&'*+\-.^_`|~0-9A-Za-z]+$/;
    const string = v => { if (typeof v === 'symbol') throw new TypeError('Cannot convert Symbol to string'); return String(v); };
    const usv = v => nativeApply(wellFormed, string(v), []);
    const utf8Length = text => nativeApply(typedLength, nativeApply(encode, new Encoder(), [text]), []);
    const fail = (name, message) => { throw new DOMException(message, name); };
    const state = x => { const s = nativeApply(weakGet, states, [x]); if (!s) throw new TypeError('Illegal WebSocket receiver'); return s; };
    const closeStates = new WeakMap();
    const closeState = x => { const s = nativeApply(weakGet, closeStates, [x]); if (!s) throw new TypeError('Illegal CloseEvent receiver'); return s; };
    class CloseEvent extends Event {
        constructor(type, init = {}) {
            if (!arguments.length) throw new TypeError('CloseEvent requires a type');
            if (init !== null && init !== undefined && typeof init !== 'object' && typeof init !== 'function') throw new TypeError('Expected dictionary');
            super(string(type), init ?? {});
            const o = init ?? {}, code = o.code === undefined ? 0 : Number(o.code);
            nativeApply(weakSet, closeStates, [this, { wasClean: !!o.wasClean, code: Number.isFinite(code) ? Math.trunc(code) & 0xffff : 0, reason: o.reason === undefined ? '' : usv(o.reason) }]);
        }
        get wasClean() { return closeState(this).wasClean; }
        get code() { return closeState(this).code; }
        get reason() { return closeState(this).reason; }
    }
    function fire(target, event) { eventState(event).isTrusted = true; dispatch(target, event); }
    function finish(target, s, clean, code, reason, error) {
        if (s.ready === CLOSED) return;
        s.ready = CLOSED; s.id = 0;
        if (error) fire(target, new Event('error'));
        fire(target, new CloseEvent('close', { wasClean: clean, code, reason }));
    }
    function receive(target, s, type, code, data, protocol) {
        switch (type) {
        case 0:
            if (s.ready !== CONNECTING) return;
            s.ready = OPEN; s.protocol = protocol;
            fire(target, new Event('open'));
            return;
        case 1: case 2: {
            if (s.ready !== OPEN) return;
            let value = data;
            if (type === 2 && s.binaryType === 'blob') value = blobBridge.fromBytes(data, '');
            fire(target, new MessageEventType('message', { data: value, origin: s.origin }));
            return;
        }
        case 3:
            finish(target, s, true, code, data, false);
            return;
        default:
            if (data) host.log(1, 'WebSocket connection to \'' + s.url + '\' failed: ' + data);
            finish(target, s, false, 1006, '', true);
        }
    }
    function bytes(data) {
        if (isView(data)) {
            const typed = nativeApply(typedTag, data, []) !== undefined;
            const buffer = nativeApply(typed ? typedBuffer : dvBuffer, data, []);
            const offset = nativeApply(typed ? typedOffset : dvOffset, data, []);
            const length = nativeApply(typed ? typedLength : dvLength, data, []);
            return nativeApply(abSlice, buffer, [offset, offset + length]);
        }
        try { nativeApply(abLength, data, []); return nativeApply(abSlice, data, [0]); } catch (_) {}
        if (blobBridge.brand(data)) return blobBridge.bytes(data);
        return null;
    }
    function secureLocal(hostname) {
        return hostname === 'localhost' || hostname.endsWith('.localhost') || hostname === '[::1]' || /^127\./.test(hostname);
    }
    class WebSocket extends EventTarget {
        constructor(url, protocols = []) {
            if (!arguments.length) throw new TypeError('WebSocket requires a URL');
            super();
            let parsed;
            try { parsed = new NativeURL(usv(url), rawDom.get(document, 'baseURI')); } catch (_) { fail('SyntaxError', 'Invalid WebSocket URL'); }
            if (parsed.protocol === 'http:') parsed.protocol = 'ws:';
            else if (parsed.protocol === 'https:') parsed.protocol = 'wss:';
            if (parsed.protocol !== 'ws:' && parsed.protocol !== 'wss:') fail('SyntaxError', 'WebSocket URL scheme must be ws or wss');
            if (parsed.href.includes('#')) fail('SyntaxError', 'WebSocket URL must not have a fragment');
            const list = typeof protocols === 'string' ? [protocols] : protocols === undefined ? [] : Array.from(protocols, string);
            const seen = new Set();
            for (const p of list) {
                if (!token.test(p)) fail('SyntaxError', 'Invalid WebSocket subprotocol');
                const key = p.toLowerCase();
                if (seen.has(key)) fail('SyntaxError', 'Duplicate WebSocket subprotocol');
                seen.add(key);
            }
            const page = host.url();
            if (parsed.protocol === 'ws:' && /^https:/i.test(page) && !secureLocal(parsed.hostname))
                fail('SecurityError', 'An insecure WebSocket connection may not be initiated from a page loaded over HTTPS');
            const s = { url: parsed.href, origin: parsed.protocol + '//' + parsed.host, ready: CONNECTING, protocol: '', binaryType: 'blob', id: 0, after: 0 };
            nativeApply(weakSet, states, [this, s]);
            const target = this;
            s.id = host.socketOpen(s.url, list.length ? list.join(', ') : null, (type, code, data, protocol) => receive(target, s, type, code, data, protocol));
        }
        get url() { return state(this).url; }
        get readyState() { return state(this).ready; }
        get bufferedAmount() { const s = state(this); return (s.id ? host.socketBuffered(s.id) : 0) + s.after; }
        get extensions() { state(this); return ''; }
        get protocol() { return state(this).protocol; }
        get binaryType() { return state(this).binaryType; }
        set binaryType(v) { const s = state(this); v = string(v); if (v === 'blob' || v === 'arraybuffer') s.binaryType = v; }
        get onopen() { return handlerValue(this, 'open'); }
        set onopen(v) { setHandler(this, 'open', v, null); }
        get onmessage() { return handlerValue(this, 'message'); }
        set onmessage(v) { setHandler(this, 'message', v, null); }
        get onerror() { return handlerValue(this, 'error'); }
        set onerror(v) { setHandler(this, 'error', v, null); }
        get onclose() { return handlerValue(this, 'close'); }
        set onclose(v) { setHandler(this, 'close', v, null); }
        send(data) {
            const s = state(this);
            if (!arguments.length) throw new TypeError('WebSocket.send requires data');
            if (s.ready === CONNECTING) fail('InvalidStateError', 'WebSocket is still connecting');
            let payload = typeof data === 'string' ? null : bytes(data), length;
            if (payload === null) { payload = usv(data); length = utf8Length(payload); }
            else length = nativeApply(abLength, payload, []);
            /* After closing starts, the spec only counts the bytes. */
            if (s.ready !== OPEN || !s.id || !host.socketSend(s.id, payload)) s.after += length;
        }
        close(code = undefined, reason = undefined) {
            const s = state(this);
            if (code !== undefined) {
                code = Number(code);
                code = Number.isFinite(code) ? Math.min(Math.max(Math.round(code), 0), 0xffff) : 0;
                if (code !== 1000 && (code < 3000 || code > 4999)) fail('InvalidAccessError', 'Invalid WebSocket close code');
            }
            reason = reason === undefined ? '' : usv(reason);
            if (utf8Length(reason) > 123) fail('SyntaxError', 'WebSocket close reason is too long');
            if (s.ready === CLOSING || s.ready === CLOSED) return;
            if (s.ready === CONNECTING) {
                /* Fail the connection; error and close are a later task. */
                const id = s.id; s.id = 0; s.ready = CLOSING;
                if (id) host.socketRelease(id);
                const target = this;
                nativeSetTimeout(() => finish(target, s, false, 1006, '', true), 0);
                return;
            }
            s.ready = CLOSING;
            if (!s.id || !host.socketClose(s.id, code === undefined ? 0 : code, reason)) {
                const id = s.id; s.id = 0;
                if (id) host.socketRelease(id);
                const target = this;
                nativeSetTimeout(() => finish(target, s, false, 1006, '', true), 0);
            }
        }
    }
    socketHandlerTarget = (target, type) => nativeApply(weakHas, states, [target]) && eventTypes.has(type);
    for (const [key, value] of Object.entries({ CONNECTING, OPEN, CLOSING, CLOSED }))
        for (const obj of [WebSocket, WebSocket.prototype]) defineProperty(obj, key, { value, enumerable: true });
    for (const [C, name] of [[WebSocket, 'WebSocket'], [CloseEvent, 'CloseEvent']])
        defineProperty(C.prototype, Symbol.toStringTag, { value: name, configurable: true });
    Object.assign(globalThis, { WebSocket, CloseEvent });
}
