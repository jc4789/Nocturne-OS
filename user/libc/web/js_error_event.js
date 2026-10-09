// Shared ErrorEvent construction; private data cannot be forged by prototype.
const ErrorEvent = (() => {
    const data = new WeakMap(), get = WeakMap.prototype.get, set = WeakMap.prototype.set;
    const call = Reflect.apply, text = String, code = String.prototype.charCodeAt, TypeErr = TypeError;
    function string(value) {
        if (typeof value === 'symbol') throw new TypeErr('Cannot convert Symbol to DOMString');
        return text(value);
    }
    function scalarString(value) {
        const input = string(value); let output = '';
        for (let i = 0; i < input.length; i++) {
            const c = call(code, input, [i]);
            if (c >= 0xd800 && c <= 0xdbff) {
                const next = i + 1 < input.length ? call(code, input, [i + 1]) : 0;
                if (next >= 0xdc00 && next <= 0xdfff) { output += input[i] + input[++i]; continue; }
                output += '\ufffd';
            } else output += c >= 0xdc00 && c <= 0xdfff ? '\ufffd' : input[i];
        }
        return output;
    }
    function record(value) {
        const result = call(get, data, [value]);
        if (!result) throw new TypeErr('Illegal ErrorEvent receiver');
        return result;
    }
    class ErrorEvent extends Event {
        constructor(type, init = {}) {
            if (!arguments.length) throw new TypeErr('ErrorEvent requires a type');
            const t = string(type);
            if (init == null) init = {};
            else if (typeof init !== 'object' && typeof init !== 'function') throw new TypeErr('Expected event dictionary');
            const base = {bubbles:!!init.bubbles, cancelable:!!init.cancelable, composed:!!init.composed};
            const values = {}, colno = init.colno;
            values.colno = colno === undefined ? 0 : (+colno) >>> 0;
            values.error = init.error;
            const filename = init.filename;
            values.filename = filename === undefined ? '' : scalarString(filename);
            const lineno = init.lineno;
            values.lineno = lineno === undefined ? 0 : (+lineno) >>> 0;
            const message = init.message;
            values.message = message === undefined ? '' : string(message);
            super(t, base); call(set, data, [this, values]);
        }
        get message() { return record(this).message; }
        get filename() { return record(this).filename; }
        get lineno() { return record(this).lineno; }
        get colno() { return record(this).colno; }
        get error() { return record(this).error; }
    }
    Object.defineProperty(ErrorEvent.prototype, Symbol.toStringTag, {configurable:true, value:'ErrorEvent'});
    for (const key of ['message','filename','lineno','colno','error'])
        Object.defineProperty(ErrorEvent.prototype, key, {...Object.getOwnPropertyDescriptor(ErrorEvent.prototype, key), enumerable:true});
    return ErrorEvent;
})();
