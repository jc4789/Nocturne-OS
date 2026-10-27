/* The Web Audio API (https://webaudio.github.io/web-audio-api/) over the browser's own
 * renderer (webaudio.h, through host.webaudio in js_webaudio.c): a program of its own,
 * compiled apart from the bindings like js_locale.js. A realm runs it the first time the
 * page touches one of its interfaces; js_bootstrap.js hands it the pieces of the event
 * system it needs and gets back the interface objects. */
(function (bridge) {
    'use strict';
    const {host, Event, EventTarget, DOMException, eventState, dispatch, handlerValue, setHandler} = bridge;
    const native = host.webaudio;
    const NativePromise = bridge.Promise, F32 = Float32Array, F64 = Float64Array;
    const defineProperty = Object.defineProperty;
    const typedPrototype = Object.getPrototypeOf(Uint8Array.prototype);
    const typedGetter = k => Object.getOwnPropertyDescriptor(typedPrototype, k).get;
    const typedTag = typedGetter(Symbol.toStringTag), typedBuffer = typedGetter('buffer'), typedLength = typedGetter('length');
    const typedSet = typedPrototype.set, typedSubarray = typedPrototype.subarray;
    const abByteLength = Object.getOwnPropertyDescriptor(ArrayBuffer.prototype, 'byteLength').get;

    /* ---- WebIDL conversions ---- */
    const tag = v => ArrayBuffer.isView(v) ? Reflect.apply(typedTag, v, []) : undefined;
    function finite(v, what) {
        const x = +v;
        if (!Number.isFinite(x)) throw new TypeError(what + ' is not a finite number');
        return x;
    }
    function float(v, what) {
        const x = Math.fround(finite(v, what));
        if (!Number.isFinite(x)) throw new TypeError(what + ' is outside the range of a float');
        return x;
    }
    const unsigned = v => (+v) >>> 0;
    function enumeration(v, values, what) {
        const s = String(v);
        if (!values.includes(s)) throw new TypeError("'" + s + "' is not a valid value for " + what);
        return s;
    }
    function dictionary(v, what) {
        if (v === undefined || v === null) return {};
        if (typeof v !== 'object' && typeof v !== 'function') throw new TypeError(what + ' is not a dictionary');
        return v;
    }
    function floatSequence(v, what) {
        if (v === null || (typeof v !== 'object' && typeof v !== 'function')) throw new TypeError(what + ' is not a sequence');
        if (typeof v[Symbol.iterator] !== 'function') throw new TypeError(what + ' is not iterable');
        const list = [];
        for (const x of v) list.push(float(x, what + ' element'));
        return F32.from(list);
    }
    function doubleSequence(v, what) {
        if (v === null || (typeof v !== 'object' && typeof v !== 'function')) throw new TypeError(what + ' is not a sequence');
        if (typeof v[Symbol.iterator] !== 'function') throw new TypeError(what + ' is not iterable');
        const list = [];
        for (const x of v) list.push(finite(x, what + ' element'));
        return F64.from(list);
    }
    /* A Float32Array argument without [AllowShared]: a view on a SharedArrayBuffer is refused. */
    function float32Array(v, what) {
        if (tag(v) !== 'Float32Array') throw new TypeError(what + ' is not a Float32Array');
        try { Reflect.apply(abByteLength, Reflect.apply(typedBuffer, v, []), []); }
        catch (_) { throw new TypeError(what + ' is a view on a SharedArrayBuffer'); }
        return v;
    }
    const fail = (name, message) => name === 'RangeError' ? new RangeError(message) : name === 'TypeError' ? new TypeError(message) :
        new DOMException(message, name);
    /* A native refusal (an exception's name, or undefined) as the exception it is. */
    function check(refusal, message) { if (refusal !== undefined) throw fail(refusal, message); }
    function nonNegative(t, what) {
        if (t < 0) throw new RangeError(what + ' must not be negative');
        return t;
    }

    /* ---- private state ---- */
    const contexts = new WeakMap(), nodes = new WeakMap(), params = new WeakMap(), buffers = new WeakMap();
    const waves = new WeakMap(), listeners = new WeakMap(), completions = new WeakMap();
    /* Contexts the page may no longer hold but that still have work: a running or rendering
       context, or one with sources scheduled (they fire ended). */
    const busy = new Set();
    const contextOf = (x, what) => { const s = contexts.get(x); if (!s) throw new TypeError('Illegal ' + what + ' receiver'); return s; };
    const nodeOf = x => { const s = nodes.get(x); if (!s) throw new TypeError('Illegal AudioNode receiver'); return s; };
    const paramOf = x => { const s = params.get(x); if (!s) throw new TypeError('Illegal AudioParam receiver'); return s; };
    const bufferOf = x => { const s = buffers.get(x); if (!s) throw new TypeError('Illegal AudioBuffer receiver'); return s; };
    let constructing = null; /* the native node a factory method made, for its interface's constructor */

    function fire(target, event) { eventState(event).isTrusted = true; dispatch(target, event); }
    /* The event handler attributes, by the internal slot that brands their receivers. */
    const handlerTypes = [];
    bridge.handlerTarget((target, type) => handlerTypes.some(([types, brand]) => types.includes(type) && brand(target)));
    function handlers(C, types, brand) {
        handlerTypes.push([types, brand]);
        for (const type of types) defineProperty(C.prototype, 'on' + type, {
            configurable: true, enumerable: true,
            get: Object.getOwnPropertyDescriptor({get ['on' + type]() { return handlerValue(this, type); }}, 'on' + type).get,
            set: Object.getOwnPropertyDescriptor({set ['on' + type](v) { setHandler(this, type, v, null); }}, 'on' + type).set,
        });
    }

    /* ---- AudioBuffer ---- */
    function adoptBuffer(token) {
        const [channels, length, rate] = native.bufferInfo(token);
        const b = Object.create(AudioBuffer.prototype);
        buffers.set(b, {token, channels, length, rate, arrays: []});
        return b;
    }
    function channelArray(s, channel) {
        let a = s.arrays[channel];
        if (!a) a = s.arrays[channel] = new F32(native.channel(s.token, channel));
        return a;
    }
    /* Acquire the content (1.4.3): the renderer keeps the data; arrays the page was handed
       are detached, and the next getChannelData returns a copy. */
    function acquire(s) {
        native.acquire(s.token, s.arrays.filter(a => a).map(a => a.buffer));
        s.arrays = [];
    }
    class AudioBuffer {
        constructor(options) {
            if (!arguments.length) throw new TypeError('AudioBuffer requires options');
            const o = dictionary(options, 'AudioBufferOptions');
            if (o.length === undefined) throw new TypeError("AudioBufferOptions requires 'length'");
            const length = unsigned(o.length);
            const channels = o.numberOfChannels === undefined ? 1 : unsigned(o.numberOfChannels);
            if (o.sampleRate === undefined) throw new TypeError("AudioBufferOptions requires 'sampleRate'");
            const rate = float(o.sampleRate, 'sampleRate');
            const token = native.buffer(channels, length, rate);
            if (typeof token === 'string') throw fail(token, 'The AudioBuffer options are out of range');
            buffers.set(this, {token, channels, length, rate, arrays: []});
        }
        get sampleRate() { return bufferOf(this).rate; }
        get length() { return bufferOf(this).length; }
        get duration() { const s = bufferOf(this); return s.length / s.rate; }
        get numberOfChannels() { return bufferOf(this).channels; }
        getChannelData(channel) {
            const s = bufferOf(this);
            if (!arguments.length) throw new TypeError('getChannelData requires a channel');
            channel = unsigned(channel);
            if (channel >= s.channels) throw new DOMException('Channel ' + channel + ' is out of range', 'IndexSizeError');
            return channelArray(s, channel);
        }
        copyFromChannel(destination, channelNumber, bufferOffset = 0) {
            const s = bufferOf(this);
            if (arguments.length < 2) throw new TypeError('copyFromChannel requires a destination and a channel');
            float32Array(destination, 'destination');
            channelNumber = unsigned(channelNumber); bufferOffset = unsigned(bufferOffset);
            if (channelNumber >= s.channels) throw new DOMException('Channel ' + channelNumber + ' is out of range', 'IndexSizeError');
            if (bufferOffset >= s.length) return;
            const from = channelArray(s, channelNumber);
            const count = Math.min(Reflect.apply(typedLength, destination, []), s.length - bufferOffset);
            Reflect.apply(typedSet, destination, [Reflect.apply(typedSubarray, from, [bufferOffset, bufferOffset + count])]);
        }
        copyToChannel(source, channelNumber, bufferOffset = 0) {
            const s = bufferOf(this);
            if (arguments.length < 2) throw new TypeError('copyToChannel requires a source and a channel');
            float32Array(source, 'source');
            channelNumber = unsigned(channelNumber); bufferOffset = unsigned(bufferOffset);
            if (channelNumber >= s.channels) throw new DOMException('Channel ' + channelNumber + ' is out of range', 'IndexSizeError');
            if (bufferOffset >= s.length) return;
            const to = channelArray(s, channelNumber);
            const count = Math.min(Reflect.apply(typedLength, source, []), s.length - bufferOffset);
            Reflect.apply(typedSet, to, [Reflect.apply(typedSubarray, source, [0, count]), bufferOffset]);
        }
    }

    /* ---- PeriodicWave ---- */
    function makeWave(real, imag, normalize) {
        if (real && imag && real.length !== imag.length) throw new DOMException('real and imag differ in length', 'IndexSizeError');
        const n = real ? real.length : imag ? imag.length : 2;
        if (n < 2) throw new DOMException('A PeriodicWave needs at least two coefficients', 'IndexSizeError');
        if (!real && !imag) { real = new F32(2); imag = F32.of(0, 1); }
        const token = native.wave(real || null, imag || null, n, normalize);
        if (typeof token === 'string') throw fail(token, 'The PeriodicWave could not be made');
        const w = Object.create(PeriodicWave.prototype);
        waves.set(w, token);
        return w;
    }
    class PeriodicWave {
        constructor(context, options = {}) {
            if (!arguments.length) throw new TypeError('PeriodicWave requires a context');
            contextOf(context, 'BaseAudioContext');
            const o = dictionary(options, 'PeriodicWaveOptions');
            const normalize = !o.disableNormalization;
            const real = o.real === undefined ? null : floatSequence(o.real, 'real');
            const imag = o.imag === undefined ? null : floatSequence(o.imag, 'imag');
            const w = makeWave(real, imag, normalize);
            waves.set(this, waves.get(w));
        }
    }

    /* ---- AudioParam ---- */
    class AudioParam {
        constructor() { throw new TypeError('Illegal constructor'); }
        get value() { const p = paramOf(this); return native.value(p.node.token, p.index); }
        set value(v) {
            const p = paramOf(this);
            check(native.setValue(p.node.token, p.index, float(v, 'value')), 'The value cannot be set now');
        }
        get automationRate() { return paramOf(this).rate; }
        set automationRate(v) {
            const p = paramOf(this), rate = String(v);
            if (rate !== 'a-rate' && rate !== 'k-rate') return;
            check(native.rate(p.node.token, p.index, rate === 'a-rate'), 'The automation rate of ' + p.name + ' is fixed');
            p.rate = rate;
        }
        get defaultValue() { return paramOf(this).defaultValue; }
        get minValue() { return paramOf(this).minValue; }
        get maxValue() { return paramOf(this).maxValue; }
        setValueAtTime(value, startTime) {
            const p = paramOf(this);
            if (arguments.length < 2) throw new TypeError('setValueAtTime requires a value and a time');
            schedule(p, 'setValue', nonNegative(finite(startTime, 'startTime'), 'startTime'), float(value, 'value'), 0, null);
            return this;
        }
        linearRampToValueAtTime(value, endTime) {
            const p = paramOf(this);
            if (arguments.length < 2) throw new TypeError('linearRampToValueAtTime requires a value and a time');
            value = float(value, 'value');
            schedule(p, 'linearRamp', nonNegative(finite(endTime, 'endTime'), 'endTime'), value, 0, null);
            return this;
        }
        exponentialRampToValueAtTime(value, endTime) {
            const p = paramOf(this);
            if (arguments.length < 2) throw new TypeError('exponentialRampToValueAtTime requires a value and a time');
            value = float(value, 'value');
            endTime = finite(endTime, 'endTime');
            if (value === 0) throw new RangeError('The target of an exponential ramp must not be zero');
            schedule(p, 'exponentialRamp', nonNegative(endTime, 'endTime'), value, 0, null);
            return this;
        }
        setTargetAtTime(target, startTime, timeConstant) {
            const p = paramOf(this);
            if (arguments.length < 3) throw new TypeError('setTargetAtTime requires a target, a time and a time constant');
            target = float(target, 'target');
            startTime = nonNegative(finite(startTime, 'startTime'), 'startTime');
            timeConstant = float(timeConstant, 'timeConstant');
            if (timeConstant < 0) throw new RangeError('The time constant must not be negative');
            schedule(p, 'setTarget', startTime, target, timeConstant, null);
            return this;
        }
        setValueCurveAtTime(values, startTime, duration) {
            const p = paramOf(this);
            if (arguments.length < 3) throw new TypeError('setValueCurveAtTime requires values, a time and a duration');
            const curve = floatSequence(values, 'values');
            startTime = nonNegative(finite(startTime, 'startTime'), 'startTime');
            duration = finite(duration, 'duration');
            if (!(duration > 0)) throw new RangeError('The duration must be positive');
            if (curve.length < 2) throw new DOMException('A value curve needs at least two values', 'InvalidStateError');
            schedule(p, 'setValueCurve', startTime, 0, duration, curve);
            return this;
        }
        cancelScheduledValues(cancelTime) {
            const p = paramOf(this);
            if (!arguments.length) throw new TypeError('cancelScheduledValues requires a time');
            schedule(p, 'cancel', nonNegative(finite(cancelTime, 'cancelTime'), 'cancelTime'), 0, 0, null);
            return this;
        }
        cancelAndHoldAtTime(cancelTime) {
            const p = paramOf(this);
            if (!arguments.length) throw new TypeError('cancelAndHoldAtTime requires a time');
            schedule(p, 'cancelAndHold', nonNegative(finite(cancelTime, 'cancelTime'), 'cancelTime'), 0, 0, null);
            return this;
        }
    }
    function schedule(p, kind, time, value, extra, curve) {
        const now = currentTime(p.node.context);
        if (time < now && kind !== 'cancel' && kind !== 'cancelAndHold') time = now;
        check(native.automate(p.node.token, p.index, kind, time, value, extra, curve),
              kind === 'setValueCurve' ? 'The value curve overlaps another automation event' : 'The automation event overlaps a value curve');
    }
    function makeParams(node, s, kind, maxDelay) {
        const list = native.params(kind, contexts.get(s.context).rate, maxDelay);
        s.params = [];
        list.forEach(([name, defaultValue, minValue, maxValue, aRate], index) => {
            const p = Object.create(AudioParam.prototype);
            params.set(p, {node: s, index, name, defaultValue, minValue, maxValue, rate: aRate ? 'a-rate' : 'k-rate'});
            s.params[index] = p;
            s.paramsByName[name] = p;
        });
    }
    function initialValue(s, name, value) {
        if (value === undefined) return;
        const p = params.get(s.paramsByName[name]);
        check(native.initial(s.token, p.index, float(value, name)), 'The value of ' + name + ' cannot be set');
    }

    /* ---- AudioNode ---- */
    const NODE_DEFAULTS = {
        destination: [2, 'explicit', 'speakers'], gain: [2, 'max', 'speakers'], oscillator: [2, 'max', 'speakers'],
        bufferSource: [2, 'max', 'speakers'], constantSource: [2, 'max', 'speakers'], delay: [2, 'max', 'speakers'],
        biquad: [2, 'max', 'speakers'], iir: [2, 'max', 'speakers'], compressor: [2, 'clamped-max', 'speakers'],
        stereoPanner: [2, 'clamped-max', 'speakers'], panner: [2, 'clamped-max', 'speakers'],
        convolver: [2, 'clamped-max', 'speakers'], waveShaper: [2, 'max', 'speakers'], analyser: [2, 'max', 'speakers'],
        splitter: [6, 'explicit', 'discrete'], merger: [1, 'explicit', 'speakers'],
    };
    const NODE_SHAPES = {destination: [1, 0], oscillator: [0, 1], bufferSource: [0, 1], constantSource: [0, 1]};
    /* The native node behind a new interface object: options are the AudioNodeOptions and
       the node's own (counts, maxDelayTime, IIR coefficients) already converted. */
    function makeNode(node, context, kind, o, extra = {}) {
        const cs = contextOf(context, 'BaseAudioContext');
        let count = 0, mode, interpretation;
        if (o.channelCount !== undefined) {
            count = unsigned(o.channelCount);
            if (count === 0) throw new DOMException('channelCount must not be 0', 'NotSupportedError');
        }
        if (o.channelCountMode !== undefined) mode = enumeration(o.channelCountMode, MODES, 'ChannelCountMode');
        if (o.channelInterpretation !== undefined) interpretation = enumeration(o.channelInterpretation, INTERPRETATIONS, 'ChannelInterpretation');
        const token = native.node(cs.token, kind, count, mode, interpretation, extra.inputs || 0, extra.outputs || 0,
                                  extra.maxDelay || 0, extra.feedforward || null, extra.feedback || null);
        if (typeof token === 'string') throw fail(token, 'The ' + kind + ' node options are not supported');
        const defaults = NODE_DEFAULTS[kind] || [2, 'max', 'speakers'];
        const shape = NODE_SHAPES[kind] || [1, 1];
        const s = {
            token, id: native.nodeId(token), context, kind,
            inputs: kind === 'merger' ? extra.inputs || 6 : shape[0],
            outputs: kind === 'splitter' ? extra.outputs || 6 : shape[1],
            channelCount: count || (kind === 'splitter' ? extra.outputs || 6 : kind === 'destination' ? cs.channels : defaults[0]),
            channelCountMode: mode || defaults[1], channelInterpretation: interpretation || defaults[2],
            params: [], paramsByName: {}, started: false,
        };
        nodes.set(node, s);
        makeParams(node, s, kind, extra.maxDelay || 1);
        return s;
    }
    function connectable(target) { return nodes.get(target) || null; }
    class AudioNode extends EventTarget {
        constructor() {
            if (!constructing) throw new TypeError('Illegal constructor');
            super();
        }
        connect(destination, output = 0, input = 0) {
            const s = nodeOf(this);
            if (!arguments.length) throw new TypeError('connect requires a destination');
            const param = params.get(destination);
            if (param) {
                output = unsigned(output);
                if (param.node.context !== s.context) throw new DOMException('The AudioParam belongs to another context', 'InvalidAccessError');
                if (output >= s.outputs) throw new DOMException('Output ' + output + ' is out of range', 'IndexSizeError');
                check(native.connectParam(s.token, output, param.node.token, param.index), 'The connection could not be made');
                return undefined;
            }
            const d = connectable(destination);
            if (!d) throw new TypeError('The destination is not an AudioNode or an AudioParam');
            output = unsigned(output); input = unsigned(input);
            if (d.context !== s.context) throw new DOMException('The destination belongs to another context', 'InvalidAccessError');
            if (output >= s.outputs) throw new DOMException('Output ' + output + ' is out of range', 'IndexSizeError');
            if (input >= d.inputs) throw new DOMException('Input ' + input + ' is out of range', 'IndexSizeError');
            check(native.connect(s.token, output, d.token, input), 'The connection could not be made');
            return destination;
        }
        disconnect(destinationOrOutput, output, input) {
            const s = nodeOf(this);
            const range = (i, n, what) => { if (i >= n) throw new DOMException(what + ' ' + i + ' is out of range', 'IndexSizeError'); return i; };
            const missing = 'The node is not connected to that destination';
            if (!arguments.length) { check(native.disconnect(s.token, -1, null, -1, -1)); return; }
            const param = params.get(destinationOrOutput);
            if (param) {
                const out = arguments.length > 1 ? range(unsigned(output), s.outputs, 'Output') : -1;
                check(native.disconnect(s.token, out, param.node.token, -1, param.index), missing);
                return;
            }
            const d = connectable(destinationOrOutput);
            if (d) {
                const out = arguments.length > 1 ? range(unsigned(output), s.outputs, 'Output') : -1;
                const into = arguments.length > 2 ? range(unsigned(input), d.inputs, 'Input') : -1;
                check(native.disconnect(s.token, out, d.token, into, -1), missing);
                return;
            }
            if (destinationOrOutput !== null && typeof destinationOrOutput === 'object')
                throw new TypeError('The destination is not an AudioNode or an AudioParam');
            check(native.disconnect(s.token, range(unsigned(destinationOrOutput), s.outputs, 'Output'), null, -1, -1));
        }
        get context() { return nodeOf(this).context; }
        get numberOfInputs() { return nodeOf(this).inputs; }
        get numberOfOutputs() { return nodeOf(this).outputs; }
        get channelCount() { return nodeOf(this).channelCount; }
        set channelCount(v) {
            const s = nodeOf(this), count = unsigned(v);
            check(native.channels(s.token, count, s.channelCountMode, s.channelInterpretation), 'channelCount ' + count + ' is not allowed here');
            s.channelCount = count;
        }
        get channelCountMode() { return nodeOf(this).channelCountMode; }
        set channelCountMode(v) {
            const s = nodeOf(this), mode = String(v);
            if (!MODES.includes(mode)) return;
            check(native.channels(s.token, s.channelCount, mode, s.channelInterpretation), "channelCountMode '" + mode + "' is not allowed here");
            s.channelCountMode = mode;
        }
        get channelInterpretation() { return nodeOf(this).channelInterpretation; }
        set channelInterpretation(v) {
            const s = nodeOf(this), interpretation = String(v);
            if (!INTERPRETATIONS.includes(interpretation)) return;
            check(native.channels(s.token, s.channelCount, s.channelCountMode, interpretation), "channelInterpretation '" + interpretation + "' is not allowed here");
            s.channelInterpretation = interpretation;
        }
    }
    const MODES = ['max', 'clamped-max', 'explicit'], INTERPRETATIONS = ['speakers', 'discrete'];
    /* Interface objects whose node a factory method or constructor makes. */
    function build(C, context, args, make) {
        const previous = constructing;
        constructing = C;
        try { return make(); } finally { constructing = previous; }
    }
    function param(node, name) { return nodeOf(node).paramsByName[name]; }
    function audioOptions(options, what) { return dictionary(options, what); }

    class AudioScheduledSourceNode extends AudioNode {
        constructor() { if (!constructing) throw new TypeError('Illegal constructor'); super(); }
        start(when = 0) {
            const s = nodeOf(this);
            when = nonNegative(finite(when, 'when'), 'when');
            startSource(this, s, when, 0, -1);
        }
        stop(when = 0) {
            const s = nodeOf(this);
            when = nonNegative(finite(when, 'when'), 'when');
            if (!s.started) throw new DOMException('stop() before start()', 'InvalidStateError');
            check(native.stop(s.token, when), 'The source cannot be stopped');
        }
    }
    const SOURCE_KINDS = ['oscillator', 'bufferSource', 'constantSource'];
    handlers(AudioScheduledSourceNode, ['ended'], target => SOURCE_KINDS.includes(nodes.get(target)?.kind));
    function startSource(node, s, when, offset, duration) {
        if (s.started) throw new DOMException('start() was already called', 'InvalidStateError');
        if (s.buffer) acquire(buffers.get(s.buffer));
        check(native.start(s.token, when, offset, duration), 'The source cannot be started');
        s.started = true;
        const cs = contexts.get(s.context);
        cs.sources.set(s.id, node);
        busy.add(s.context);
    }

    class GainNode extends AudioNode {
        constructor(context, options = {}) {
            if (!arguments.length) throw new TypeError('GainNode requires a context');
            const o = audioOptions(options, 'GainOptions');
            build(GainNode, context, arguments, () => super());
            const s = makeNode(this, context, 'gain', o);
            initialValue(s, 'gain', o.gain);
        }
        get gain() { return param(this, 'gain'); }
    }

    class OscillatorNode extends AudioScheduledSourceNode {
        constructor(context, options = {}) {
            if (!arguments.length) throw new TypeError('OscillatorNode requires a context');
            const o = audioOptions(options, 'OscillatorOptions');
            const type = o.type === undefined ? 'sine' : enumeration(o.type, OSCILLATOR_TYPES, 'OscillatorType');
            const wave = o.periodicWave === undefined ? undefined : waves.get(o.periodicWave);
            if (o.periodicWave !== undefined && !wave) throw new TypeError('periodicWave is not a PeriodicWave');
            if (type === 'custom' && !wave) throw new DOMException("type 'custom' requires a periodicWave", 'InvalidStateError');
            build(OscillatorNode, context, arguments, () => super());
            const s = makeNode(this, context, 'oscillator', o);
            initialValue(s, 'frequency', o.frequency);
            initialValue(s, 'detune', o.detune);
            s.type = 'sine';
            if (wave) { check(native.setWave(s.token, wave)); s.type = 'custom'; }
            else if (type !== 'sine') { check(native.set(s.token, 'type', OSCILLATOR_TYPES.indexOf(type))); s.type = type; }
        }
        get type() { return nodeOf(this).type; }
        set type(v) {
            const s = nodeOf(this), type = String(v);
            if (!OSCILLATOR_TYPES.includes(type)) return;
            if (type === 'custom') throw new DOMException("type 'custom' is set by setPeriodicWave()", 'InvalidStateError');
            check(native.set(s.token, 'type', OSCILLATOR_TYPES.indexOf(type)));
            s.type = type;
        }
        get frequency() { return param(this, 'frequency'); }
        get detune() { return param(this, 'detune'); }
        setPeriodicWave(periodicWave) {
            const s = nodeOf(this), wave = waves.get(periodicWave);
            if (!wave) throw new TypeError('setPeriodicWave requires a PeriodicWave');
            check(native.setWave(s.token, wave));
            s.type = 'custom';
        }
    }
    const OSCILLATOR_TYPES = ['sine', 'square', 'sawtooth', 'triangle', 'custom'];

    class AudioBufferSourceNode extends AudioScheduledSourceNode {
        constructor(context, options = {}) {
            if (!arguments.length) throw new TypeError('AudioBufferSourceNode requires a context');
            const o = audioOptions(options, 'AudioBufferSourceOptions');
            build(AudioBufferSourceNode, context, arguments, () => super());
            const s = makeNode(this, context, 'bufferSource', {});
            s.buffer = null; s.loop = false; s.loopStart = 0; s.loopEnd = 0;
            if (o.buffer !== undefined && o.buffer !== null) this.buffer = o.buffer;
            initialValue(s, 'detune', o.detune);
            initialValue(s, 'playbackRate', o.playbackRate);
            if (o.loop !== undefined) this.loop = o.loop;
            if (o.loopStart !== undefined) this.loopStart = finite(o.loopStart, 'loopStart');
            if (o.loopEnd !== undefined) this.loopEnd = finite(o.loopEnd, 'loopEnd');
        }
        get buffer() { return nodeOf(this).buffer; }
        set buffer(v) {
            const s = nodeOf(this);
            if (v !== null && !buffers.has(v)) throw new TypeError('buffer is not an AudioBuffer');
            if (v !== null && s.bufferSet) throw new DOMException('The buffer can be set only once', 'InvalidStateError');
            if (v !== null) s.bufferSet = true;
            const b = v === null ? null : buffers.get(v);
            if (b && s.started) acquire(b);
            check(native.setBuffer(s.token, b ? b.token : null));
            s.buffer = v;
        }
        get playbackRate() { return param(this, 'playbackRate'); }
        get detune() { return param(this, 'detune'); }
        get loop() { return nodeOf(this).loop; }
        set loop(v) { const s = nodeOf(this); s.loop = !!v; native.set(s.token, 'loop', s.loop ? 1 : 0); }
        get loopStart() { return nodeOf(this).loopStart; }
        set loopStart(v) { const s = nodeOf(this); s.loopStart = finite(v, 'loopStart'); native.set(s.token, 'loopStart', s.loopStart); }
        get loopEnd() { return nodeOf(this).loopEnd; }
        set loopEnd(v) { const s = nodeOf(this); s.loopEnd = finite(v, 'loopEnd'); native.set(s.token, 'loopEnd', s.loopEnd); }
        start(when = 0, offset = undefined, duration = undefined) {
            const s = nodeOf(this);
            when = nonNegative(finite(when, 'when'), 'when');
            offset = offset === undefined ? 0 : nonNegative(finite(offset, 'offset'), 'offset');
            duration = duration === undefined ? -1 : nonNegative(finite(duration, 'duration'), 'duration');
            startSource(this, s, when, offset, duration);
        }
    }

    class ConstantSourceNode extends AudioScheduledSourceNode {
        constructor(context, options = {}) {
            if (!arguments.length) throw new TypeError('ConstantSourceNode requires a context');
            const o = audioOptions(options, 'ConstantSourceOptions');
            build(ConstantSourceNode, context, arguments, () => super());
            const s = makeNode(this, context, 'constantSource', {});
            initialValue(s, 'offset', o.offset);
        }
        get offset() { return param(this, 'offset'); }
    }

    class DelayNode extends AudioNode {
        constructor(context, options = {}) {
            if (!arguments.length) throw new TypeError('DelayNode requires a context');
            const o = audioOptions(options, 'DelayOptions');
            const maxDelay = o.maxDelayTime === undefined ? 1 : finite(o.maxDelayTime, 'maxDelayTime');
            if (!(maxDelay > 0 && maxDelay < 180)) throw new DOMException('maxDelayTime must be in (0, 180)', 'NotSupportedError');
            build(DelayNode, context, arguments, () => super());
            const s = makeNode(this, context, 'delay', o, {maxDelay});
            initialValue(s, 'delayTime', o.delayTime);
        }
        get delayTime() { return param(this, 'delayTime'); }
    }

    class BiquadFilterNode extends AudioNode {
        constructor(context, options = {}) {
            if (!arguments.length) throw new TypeError('BiquadFilterNode requires a context');
            const o = audioOptions(options, 'BiquadFilterOptions');
            const type = o.type === undefined ? 'lowpass' : enumeration(o.type, BIQUAD_TYPES, 'BiquadFilterType');
            build(BiquadFilterNode, context, arguments, () => super());
            const s = makeNode(this, context, 'biquad', o);
            for (const name of ['Q', 'detune', 'frequency', 'gain']) initialValue(s, name, o[name]);
            s.type = 'lowpass';
            if (type !== 'lowpass') this.type = type;
        }
        get type() { return nodeOf(this).type; }
        set type(v) {
            const s = nodeOf(this), type = String(v);
            if (!BIQUAD_TYPES.includes(type)) return;
            check(native.set(s.token, 'type', BIQUAD_TYPES.indexOf(type)));
            s.type = type;
        }
        get frequency() { return param(this, 'frequency'); }
        get detune() { return param(this, 'detune'); }
        get Q() { return param(this, 'Q'); }
        get gain() { return param(this, 'gain'); }
        getFrequencyResponse(frequencyHz, magResponse, phaseResponse) {
            frequencyResponse(nodeOf(this), arguments.length, frequencyHz, magResponse, phaseResponse);
        }
    }
    const BIQUAD_TYPES = ['lowpass', 'highpass', 'bandpass', 'lowshelf', 'highshelf', 'peaking', 'notch', 'allpass'];
    function frequencyResponse(s, count, hz, magnitude, phase) {
        if (count < 3) throw new TypeError('getFrequencyResponse requires three arrays');
        float32Array(hz, 'frequencyHz'); float32Array(magnitude, 'magResponse'); float32Array(phase, 'phaseResponse');
        if (magnitude.length < hz.length || phase.length < hz.length)
            throw new DOMException('The response arrays are shorter than frequencyHz', 'InvalidAccessError');
        check(native.response(s.token, hz, magnitude, phase));
    }

    class IIRFilterNode extends AudioNode {
        constructor(context, options) {
            if (arguments.length < 2) throw new TypeError('IIRFilterNode requires a context and options');
            const o = audioOptions(options, 'IIRFilterOptions');
            if (o.feedforward === undefined) throw new TypeError("IIRFilterOptions requires 'feedforward'");
            if (o.feedback === undefined) throw new TypeError("IIRFilterOptions requires 'feedback'");
            const feedforward = doubleSequence(o.feedforward, 'feedforward'), feedback = doubleSequence(o.feedback, 'feedback');
            if (!feedforward.length || feedforward.length > 20) throw new DOMException('feedforward must have 1 to 20 coefficients', 'NotSupportedError');
            if (!feedback.length || feedback.length > 20) throw new DOMException('feedback must have 1 to 20 coefficients', 'NotSupportedError');
            if (feedforward.every(x => x === 0)) throw new DOMException('feedforward must not be all zero', 'InvalidStateError');
            if (feedback[0] === 0) throw new DOMException('feedback[0] must not be zero', 'InvalidStateError');
            build(IIRFilterNode, context, arguments, () => super());
            makeNode(this, context, 'iir', o, {feedforward, feedback});
        }
        getFrequencyResponse(frequencyHz, magResponse, phaseResponse) {
            frequencyResponse(nodeOf(this), arguments.length, frequencyHz, magResponse, phaseResponse);
        }
    }

    class DynamicsCompressorNode extends AudioNode {
        constructor(context, options = {}) {
            if (!arguments.length) throw new TypeError('DynamicsCompressorNode requires a context');
            const o = audioOptions(options, 'DynamicsCompressorOptions');
            build(DynamicsCompressorNode, context, arguments, () => super());
            const s = makeNode(this, context, 'compressor', o);
            for (const name of ['attack', 'knee', 'ratio', 'release', 'threshold']) initialValue(s, name, o[name]);
        }
        get threshold() { return param(this, 'threshold'); }
        get knee() { return param(this, 'knee'); }
        get ratio() { return param(this, 'ratio'); }
        get reduction() { return native.reduction(nodeOf(this).token); }
        get attack() { return param(this, 'attack'); }
        get release() { return param(this, 'release'); }
    }

    class StereoPannerNode extends AudioNode {
        constructor(context, options = {}) {
            if (!arguments.length) throw new TypeError('StereoPannerNode requires a context');
            const o = audioOptions(options, 'StereoPannerOptions');
            build(StereoPannerNode, context, arguments, () => super());
            const s = makeNode(this, context, 'stereoPanner', o);
            initialValue(s, 'pan', o.pan);
        }
        get pan() { return param(this, 'pan'); }
    }

    const PANNING_MODELS = ['equalpower', 'HRTF'], DISTANCE_MODELS = ['linear', 'inverse', 'exponential'];
    class PannerNode extends AudioNode {
        constructor(context, options = {}) {
            if (!arguments.length) throw new TypeError('PannerNode requires a context');
            const o = audioOptions(options, 'PannerOptions');
            const panningModel = o.panningModel === undefined ? 'equalpower' : enumeration(o.panningModel, PANNING_MODELS, 'PanningModelType');
            const distanceModel = o.distanceModel === undefined ? 'inverse' : enumeration(o.distanceModel, DISTANCE_MODELS, 'DistanceModelType');
            build(PannerNode, context, arguments, () => super());
            const s = makeNode(this, context, 'panner', o);
            Object.assign(s, {panningModel: 'equalpower', distanceModel: 'inverse', refDistance: 1, maxDistance: 10000,
                              rolloffFactor: 1, coneInnerAngle: 360, coneOuterAngle: 360, coneOuterGain: 0});
            for (const name of ['positionX', 'positionY', 'positionZ', 'orientationX', 'orientationY', 'orientationZ'])
                initialValue(s, name, o[name]);
            this.panningModel = panningModel;
            this.distanceModel = distanceModel;
            for (const name of ['refDistance', 'maxDistance', 'rolloffFactor', 'coneInnerAngle', 'coneOuterAngle', 'coneOuterGain'])
                if (o[name] !== undefined) this[name] = o[name];
        }
        get panningModel() { return nodeOf(this).panningModel; }
        set panningModel(v) {
            const s = nodeOf(this), model = String(v);
            if (!PANNING_MODELS.includes(model)) return;
            native.set(s.token, 'panningModel', PANNING_MODELS.indexOf(model));
            s.panningModel = model;
        }
        get distanceModel() { return nodeOf(this).distanceModel; }
        set distanceModel(v) {
            const s = nodeOf(this), model = String(v);
            if (!DISTANCE_MODELS.includes(model)) return;
            native.set(s.token, 'distanceModel', DISTANCE_MODELS.indexOf(model));
            s.distanceModel = model;
        }
        get positionX() { return param(this, 'positionX'); }
        get positionY() { return param(this, 'positionY'); }
        get positionZ() { return param(this, 'positionZ'); }
        get orientationX() { return param(this, 'orientationX'); }
        get orientationY() { return param(this, 'orientationY'); }
        get orientationZ() { return param(this, 'orientationZ'); }
        get refDistance() { return nodeOf(this).refDistance; }
        set refDistance(v) { pannerNumber(this, 'refDistance', v, x => x < 0, 'RangeError', 'refDistance must not be negative'); }
        get maxDistance() { return nodeOf(this).maxDistance; }
        set maxDistance(v) { pannerNumber(this, 'maxDistance', v, x => x <= 0, 'RangeError', 'maxDistance must be positive'); }
        get rolloffFactor() { return nodeOf(this).rolloffFactor; }
        set rolloffFactor(v) { pannerNumber(this, 'rolloffFactor', v, x => x < 0, 'RangeError', 'rolloffFactor must not be negative'); }
        get coneInnerAngle() { return nodeOf(this).coneInnerAngle; }
        set coneInnerAngle(v) { pannerNumber(this, 'coneInnerAngle', v, () => false); }
        get coneOuterAngle() { return nodeOf(this).coneOuterAngle; }
        set coneOuterAngle(v) { pannerNumber(this, 'coneOuterAngle', v, () => false); }
        get coneOuterGain() { return nodeOf(this).coneOuterGain; }
        set coneOuterGain(v) { pannerNumber(this, 'coneOuterGain', v, x => x < 0 || x > 1, 'InvalidStateError', 'coneOuterGain must be in [0, 1]'); }
        setPosition(x, y, z) {
            const s = nodeOf(this);
            if (arguments.length < 3) throw new TypeError('setPosition requires x, y and z');
            setValues(s, ['positionX', 'positionY', 'positionZ'], [x, y, z]);
        }
        setOrientation(x, y, z) {
            const s = nodeOf(this);
            if (arguments.length < 3) throw new TypeError('setOrientation requires x, y and z');
            setValues(s, ['orientationX', 'orientationY', 'orientationZ'], [x, y, z]);
        }
    }
    function pannerNumber(node, name, v, wrong, error, message) {
        const s = nodeOf(node), x = finite(v, name);
        if (wrong(x)) throw fail(error, message);
        native.set(s.token, name, x);
        s[name] = x;
    }
    function setValues(s, names, values) {
        const floats = values.map((v, i) => float(v, names[i]));
        names.forEach((name, i) => check(native.setValue(s.token, params.get(s.paramsByName[name]).index, floats[i])));
    }

    class ConvolverNode extends AudioNode {
        constructor(context, options = {}) {
            if (!arguments.length) throw new TypeError('ConvolverNode requires a context');
            const o = audioOptions(options, 'ConvolverOptions');
            build(ConvolverNode, context, arguments, () => super());
            const s = makeNode(this, context, 'convolver', o);
            s.buffer = null;
            s.normalize = true;
            this.normalize = !o.disableNormalization;
            if (o.buffer !== undefined && o.buffer !== null) this.buffer = o.buffer;
        }
        get buffer() { return nodeOf(this).buffer; }
        set buffer(v) {
            const s = nodeOf(this);
            if (v !== null && !buffers.has(v)) throw new TypeError('buffer is not an AudioBuffer');
            const b = v === null ? null : buffers.get(v);
            if (b) {
                if (b.channels !== 1 && b.channels !== 2 && b.channels !== 4)
                    throw new DOMException('A convolver takes a buffer of 1, 2 or 4 channels', 'NotSupportedError');
                if (b.rate !== contexts.get(s.context).rate)
                    throw new DOMException("The buffer's sample rate differs from the context's", 'NotSupportedError');
                acquire(b);
            }
            check(native.setBuffer(s.token, b ? b.token : null), 'The impulse response could not be prepared');
            s.buffer = v;
        }
        get normalize() { return nodeOf(this).normalize; }
        set normalize(v) { const s = nodeOf(this); s.normalize = !!v; native.set(s.token, 'normalize', s.normalize ? 1 : 0); }
    }

    const OVERSAMPLE = ['none', '2x', '4x'];
    class WaveShaperNode extends AudioNode {
        constructor(context, options = {}) {
            if (!arguments.length) throw new TypeError('WaveShaperNode requires a context');
            const o = audioOptions(options, 'WaveShaperOptions');
            const oversample = o.oversample === undefined ? 'none' : enumeration(o.oversample, OVERSAMPLE, 'OverSampleType');
            const curve = o.curve === undefined ? undefined : floatSequence(o.curve, 'curve');
            build(WaveShaperNode, context, arguments, () => super());
            const s = makeNode(this, context, 'waveShaper', o);
            s.curve = null;
            s.oversample = 'none';
            if (curve !== undefined) this.curve = curve;
            this.oversample = oversample;
        }
        get curve() { return nodeOf(this).curve; }
        set curve(v) {
            const s = nodeOf(this);
            if (v !== null) {
                float32Array(v, 'curve');
                if (s.curve !== null) throw new DOMException('The curve can be set only once', 'InvalidStateError');
                if (v.length < 2) throw new DOMException('A curve needs at least two values', 'InvalidStateError');
            }
            check(native.setCurve(s.token, v));
            s.curve = v;
        }
        get oversample() { return nodeOf(this).oversample; }
        set oversample(v) {
            const s = nodeOf(this), value = String(v);
            if (!OVERSAMPLE.includes(value)) return;
            native.set(s.token, 'oversample', OVERSAMPLE.indexOf(value));
            s.oversample = value;
        }
    }

    class AnalyserNode extends AudioNode {
        constructor(context, options = {}) {
            if (!arguments.length) throw new TypeError('AnalyserNode requires a context');
            const o = audioOptions(options, 'AnalyserOptions');
            build(AnalyserNode, context, arguments, () => super());
            const s = makeNode(this, context, 'analyser', o);
            Object.assign(s, {fftSize: 2048, minDecibels: -100, maxDecibels: -30, smoothingTimeConstant: 0.8});
            if (o.fftSize !== undefined) this.fftSize = o.fftSize;
            const min = o.minDecibels === undefined ? -100 : finite(o.minDecibels, 'minDecibels');
            const max = o.maxDecibels === undefined ? -30 : finite(o.maxDecibels, 'maxDecibels');
            if (min >= max) throw new DOMException('minDecibels must be less than maxDecibels', 'IndexSizeError');
            s.minDecibels = min; s.maxDecibels = max;
            native.set(s.token, 'minDecibels', min); native.set(s.token, 'maxDecibels', max);
            if (o.smoothingTimeConstant !== undefined) this.smoothingTimeConstant = o.smoothingTimeConstant;
        }
        getFloatFrequencyData(array) { analyse(this, 0, array, 'Float32Array'); }
        getByteFrequencyData(array) { analyse(this, 1, array, 'Uint8Array'); }
        getFloatTimeDomainData(array) { analyse(this, 2, array, 'Float32Array'); }
        getByteTimeDomainData(array) { analyse(this, 3, array, 'Uint8Array'); }
        get fftSize() { return nodeOf(this).fftSize; }
        set fftSize(v) {
            const s = nodeOf(this), size = unsigned(v);
            if (size < 32 || size > 32768 || (size & (size - 1))) throw new DOMException('fftSize must be a power of two in [32, 32768]', 'IndexSizeError');
            check(native.set(s.token, 'fftSize', size));
            s.fftSize = size;
        }
        get frequencyBinCount() { return nodeOf(this).fftSize / 2; }
        get minDecibels() { return nodeOf(this).minDecibels; }
        set minDecibels(v) {
            const s = nodeOf(this), x = finite(v, 'minDecibels');
            if (x >= s.maxDecibels) throw new DOMException('minDecibels must be less than maxDecibels', 'IndexSizeError');
            native.set(s.token, 'minDecibels', x); s.minDecibels = x;
        }
        get maxDecibels() { return nodeOf(this).maxDecibels; }
        set maxDecibels(v) {
            const s = nodeOf(this), x = finite(v, 'maxDecibels');
            if (x <= s.minDecibels) throw new DOMException('maxDecibels must be greater than minDecibels', 'IndexSizeError');
            native.set(s.token, 'maxDecibels', x); s.maxDecibels = x;
        }
        get smoothingTimeConstant() { return nodeOf(this).smoothingTimeConstant; }
        set smoothingTimeConstant(v) {
            const s = nodeOf(this), x = finite(v, 'smoothingTimeConstant');
            if (x < 0 || x > 1) throw new DOMException('smoothingTimeConstant must be in [0, 1]', 'IndexSizeError');
            native.set(s.token, 'smoothingTimeConstant', x); s.smoothingTimeConstant = x;
        }
    }
    function analyse(node, kind, array, type) {
        const s = nodeOf(node);
        if (tag(array) !== type) throw new TypeError('The array is not a ' + type);
        check(native.analyse(s.token, kind, array));
    }

    class ChannelSplitterNode extends AudioNode {
        constructor(context, options = {}) {
            if (!arguments.length) throw new TypeError('ChannelSplitterNode requires a context');
            const o = audioOptions(options, 'ChannelSplitterOptions');
            const outputs = o.numberOfOutputs === undefined ? 6 : unsigned(o.numberOfOutputs);
            if (outputs < 1 || outputs > 32) throw new DOMException('numberOfOutputs must be in [1, 32]', 'IndexSizeError');
            build(ChannelSplitterNode, context, arguments, () => super());
            makeNode(this, context, 'splitter', o, {outputs});
        }
    }
    class ChannelMergerNode extends AudioNode {
        constructor(context, options = {}) {
            if (!arguments.length) throw new TypeError('ChannelMergerNode requires a context');
            const o = audioOptions(options, 'ChannelMergerOptions');
            const inputs = o.numberOfInputs === undefined ? 6 : unsigned(o.numberOfInputs);
            if (inputs < 1 || inputs > 32) throw new DOMException('numberOfInputs must be in [1, 32]', 'IndexSizeError');
            build(ChannelMergerNode, context, arguments, () => super());
            makeNode(this, context, 'merger', o, {inputs});
        }
    }

    class AudioDestinationNode extends AudioNode {
        constructor() { if (constructing !== AudioDestinationNode) throw new TypeError('Illegal constructor'); super(); }
        get maxChannelCount() { const s = nodeOf(this); return contexts.get(s.context).maxChannels; }
    }
    class AudioListener {
        constructor() { if (constructing !== AudioListener) throw new TypeError('Illegal constructor'); }
        get positionX() { return listenerParam(this, 'positionX'); }
        get positionY() { return listenerParam(this, 'positionY'); }
        get positionZ() { return listenerParam(this, 'positionZ'); }
        get forwardX() { return listenerParam(this, 'forwardX'); }
        get forwardY() { return listenerParam(this, 'forwardY'); }
        get forwardZ() { return listenerParam(this, 'forwardZ'); }
        get upX() { return listenerParam(this, 'upX'); }
        get upY() { return listenerParam(this, 'upY'); }
        get upZ() { return listenerParam(this, 'upZ'); }
        setPosition(x, y, z) {
            const s = listenerOf(this);
            if (arguments.length < 3) throw new TypeError('setPosition requires x, y and z');
            setValues(s, ['positionX', 'positionY', 'positionZ'], [x, y, z]);
        }
        setOrientation(x, y, z, xUp, yUp, zUp) {
            const s = listenerOf(this);
            if (arguments.length < 6) throw new TypeError('setOrientation requires six values');
            setValues(s, ['forwardX', 'forwardY', 'forwardZ', 'upX', 'upY', 'upZ'], [x, y, z, xUp, yUp, zUp]);
        }
    }
    const listenerOf = x => { const s = listeners.get(x); if (!s) throw new TypeError('Illegal AudioListener receiver'); return s; };
    const listenerParam = (x, name) => listenerOf(x).paramsByName[name];

    /* ---- contexts ---- */
    function currentTime(context) {
        const cs = contexts.get(context);
        native.status(cs.token, cs.status);
        return cs.status[1] / cs.rate;
    }
    /* Make the native context and the nodes every context has. */
    function makeContext(context, rate, channels, length) {
        const cs = {
            rate, channels, length, offline: length > 0, maxChannels: length > 0 ? channels : 2,
            control: 'suspended', state: 'suspended', status: new F64(6),
            pendingResume: [], pendingSuspend: [], pendingClose: [], suspends: new Map(),
            sources: new Map(), rendering: null, started: false, token: null, tasks: [],
        };
        const token = native.context(rate, channels, length, (kind, id, value, buffer) => event(context, cs, kind, id, value, buffer));
        if (typeof token === 'string')
            throw fail(token, token === 'InvalidStateError' ? 'The document is not fully active' : 'The context options are not supported');
        cs.token = token;
        contexts.set(context, cs);
        cs.destination = build(AudioDestinationNode, context, [], () => Reflect.construct(AudioDestinationNode, []));
        makeNode(cs.destination, context, 'destination', {});
        cs.listener = build(AudioListener, context, [], () => Reflect.construct(AudioListener, []));
        const listener = makeNode({}, context, 'listener', {});
        listeners.set(cs.listener, listener);
        return cs;
    }
    function settle(list, resolve, value) {
        for (const p of list.splice(0)) resolve ? p.resolve(value) : p.reject(value);
    }
    /* A task in the context's queue, after what its renderer already posted. */
    function queueTask(cs, step) {
        cs.tasks.push(step);
        native.post(cs.token);
    }
    function stateChange(context, cs, state) {
        if (cs.state === state) return;
        cs.state = state;
        queueTask(cs, () => fire(context, new Event('statechange')));
    }
    /* A task the renderer posted (js_webaudio.c delivers them one at a time). */
    function event(context, cs, kind, id, value, buffer) {
        switch (kind) {
        case 'task': {
            const step = cs.tasks.shift();
            if (step) step();
            return;
        }
        case 'ended': {
            const node = cs.sources.get(id);
            if (!node) return;
            cs.sources.delete(id);
            settleBusy(context, cs);
            fire(node, new Event('ended'));
            return;
        }
        case 'state':
            if (value === 'running') {
                settle(cs.pendingResume, true);
                stateChange(context, cs, 'running');
            } else if (value === 'suspended') {
                settle(cs.pendingSuspend, true);
                stateChange(context, cs, 'suspended');
            } else {
                settle(cs.pendingClose, true);
                settle(cs.pendingResume, false, new DOMException('The context was closed', 'InvalidStateError'));
                cs.sources.clear();
                stateChange(context, cs, 'closed');
            }
            settleBusy(context, cs);
            return;
        case 'suspended': {
            const p = cs.suspends.get(value);
            cs.suspends.delete(value);
            cs.control = 'suspended';
            stateChange(context, cs, 'suspended');
            settleBusy(context, cs);
            if (p) p.resolve();
            return;
        }
        case 'complete': {
            const rendering = cs.rendering;
            cs.rendering = null;
            cs.control = 'closed';
            cs.sources.clear();
            settleBusy(context, cs);
            if (!buffer) { if (rendering) rendering.reject(new DOMException('Rendering failed', 'InvalidStateError')); return; }
            const rendered = adoptBuffer(buffer);
            stateChange(context, cs, 'closed');
            if (rendering) rendering.resolve(rendered);
            queueTask(cs, () => fire(context, new OfflineAudioCompletionEvent('complete', {renderedBuffer: rendered})));
            return;
        }
        }
    }
    /* Keep a context reachable while it has work the page may not hold it for. */
    function settleBusy(context, cs) {
        if (cs.control === 'running' || cs.sources.size || cs.pendingResume.length || cs.pendingSuspend.length || cs.pendingClose.length)
            busy.add(context);
        else busy.delete(context);
    }
    function promise() {
        let resolve, reject;
        const p = new NativePromise((y, n) => { resolve = y; reject = n; });
        return {promise: p, resolve, reject};
    }
    const rejected = error => NativePromise.reject(error);

    class BaseAudioContext extends EventTarget {
        constructor() { if (!constructing) throw new TypeError('Illegal constructor'); super(); }
        get destination() { return contextOf(this, 'BaseAudioContext').destination; }
        get sampleRate() { return contextOf(this, 'BaseAudioContext').rate; }
        get currentTime() { contextOf(this, 'BaseAudioContext'); return currentTime(this); }
        get listener() { return contextOf(this, 'BaseAudioContext').listener; }
        get state() { return contextOf(this, 'BaseAudioContext').state; }
        createAnalyser() { return new AnalyserNode(this); }
        createBiquadFilter() { return new BiquadFilterNode(this); }
        createBuffer(numberOfChannels, length, sampleRate) {
            contextOf(this, 'BaseAudioContext');
            if (arguments.length < 3) throw new TypeError('createBuffer requires channels, length and sample rate');
            return new AudioBuffer({numberOfChannels: unsigned(numberOfChannels), length: unsigned(length), sampleRate: float(sampleRate, 'sampleRate')});
        }
        createBufferSource() { return new AudioBufferSourceNode(this); }
        createChannelMerger(numberOfInputs = 6) { return new ChannelMergerNode(this, {numberOfInputs: unsigned(numberOfInputs)}); }
        createChannelSplitter(numberOfOutputs = 6) { return new ChannelSplitterNode(this, {numberOfOutputs: unsigned(numberOfOutputs)}); }
        createConstantSource() { return new ConstantSourceNode(this); }
        createConvolver() { return new ConvolverNode(this); }
        createDelay(maxDelayTime = 1) { return new DelayNode(this, {maxDelayTime: finite(maxDelayTime, 'maxDelayTime')}); }
        createDynamicsCompressor() { return new DynamicsCompressorNode(this); }
        createGain() { return new GainNode(this); }
        createIIRFilter(feedforward, feedback) {
            if (arguments.length < 2) throw new TypeError('createIIRFilter requires feedforward and feedback');
            return new IIRFilterNode(this, {feedforward: doubleSequence(feedforward, 'feedforward'), feedback: doubleSequence(feedback, 'feedback')});
        }
        createOscillator() { return new OscillatorNode(this); }
        createPanner() { return new PannerNode(this); }
        createPeriodicWave(real, imag, constraints = {}) {
            contextOf(this, 'BaseAudioContext');
            if (arguments.length < 2) throw new TypeError('createPeriodicWave requires real and imag');
            const r = floatSequence(real, 'real'), i = floatSequence(imag, 'imag');
            return makeWave(r, i, !dictionary(constraints, 'PeriodicWaveConstraints').disableNormalization);
        }
        createStereoPanner() { return new StereoPannerNode(this); }
        createWaveShaper() { return new WaveShaperNode(this); }
        decodeAudioData(audioData, successCallback = null, errorCallback = null) {
            let cs;
            try {
                cs = contextOf(this, 'BaseAudioContext');
                try { Reflect.apply(abByteLength, audioData, []); } catch { throw new TypeError('decodeAudioData requires an ArrayBuffer'); }
                if (successCallback !== null && typeof successCallback !== 'function') throw new TypeError('successCallback is not a function');
                if (errorCallback !== null && typeof errorCallback !== 'function') throw new TypeError('errorCallback is not a function');
            } catch (e) { return rejected(e); }
            native.status(cs.token, cs.status);
            if (!cs.status[5]) return rejected(new DOMException('The document is not fully active', 'InvalidStateError'));
            /* A refusal rejects at once; the error callback still comes in the task the
               native side queues for it. */
            const p = promise();
            let refusal = null;
            const refused = native.decode(cs.rate, audioData, (error, token) => {
                if (error) {
                    const e = refusal || fail(error, 'The audio data could not be decoded');
                    if (!refusal) p.reject(e);
                    if (errorCallback) errorCallback(e);
                    return;
                }
                const b = adoptBuffer(token);
                p.resolve(b);
                if (successCallback) successCallback(b);
            });
            if (refused) {
                refusal = fail(refused, refused === 'DataCloneError' ? 'The ArrayBuffer is detached' : 'The ArrayBuffer cannot be detached');
                p.reject(refusal);
            }
            return p.promise;
        }
    }
    handlers(BaseAudioContext, ['statechange'], target => contexts.has(target));

    const LATENCY = ['balanced', 'interactive', 'playback'];
    class AudioContext extends BaseAudioContext {
        constructor(contextOptions = {}) {
            const o = dictionary(contextOptions, 'AudioContextOptions');
            if (o.latencyHint !== undefined && typeof o.latencyHint !== 'number' && !(o.latencyHint instanceof Number))
                enumeration(o.latencyHint, LATENCY, 'AudioContextLatencyCategory');
            else if (o.latencyHint !== undefined) finite(o.latencyHint, 'latencyHint');
            const rate = o.sampleRate === undefined ? native.outputRate : float(o.sampleRate, 'sampleRate');
            if (!native.active()) throw new DOMException('The document is not fully active', 'InvalidStateError');
            build(AudioContext, null, [], () => super());
            const cs = makeContext(this, rate, 2, 0);
            /* allowed to start: rendering begins, and statechange follows in a task */
            cs.control = 'running';
            check(native.run(cs.token, true), 'The context could not start');
            settleBusy(this, cs);
        }
        get baseLatency() { contextOf(this, 'AudioContext'); return native.outputTarget / native.outputRate; }
        get outputLatency() {
            const cs = contextOf(this, 'AudioContext');
            native.status(cs.token, cs.status);
            return cs.status[2] / native.outputRate;
        }
        getOutputTimestamp() {
            const cs = contextOf(this, 'AudioContext');
            native.status(cs.token, cs.status);
            if (cs.status[0] !== 1 && !cs.status[3]) return {contextTime: 0, performanceTime: 0};
            return {contextTime: cs.status[3] / native.outputRate, performanceTime: Math.max(0, host.now() - cs.status[4])};
        }
        resume() {
            let cs;
            try { cs = contextOf(this, 'AudioContext'); } catch (e) { return rejected(e); }
            native.status(cs.token, cs.status);
            if (!cs.status[5]) return rejected(new DOMException('The document is not fully active', 'InvalidStateError'));
            if (cs.control === 'closed') return rejected(new DOMException('The context is closed', 'InvalidStateError'));
            const p = promise();
            cs.pendingResume.push(p);
            cs.control = 'running';
            check(native.run(cs.token, true));
            settleBusy(this, cs);
            return p.promise;
        }
        suspend() {
            let cs;
            try { cs = contextOf(this, 'AudioContext'); } catch (e) { return rejected(e); }
            native.status(cs.token, cs.status);
            if (!cs.status[5]) return rejected(new DOMException('The document is not fully active', 'InvalidStateError'));
            if (cs.control === 'closed') return rejected(new DOMException('The context is closed', 'InvalidStateError'));
            const p = promise();
            cs.pendingSuspend.push(p);
            cs.control = 'suspended';
            check(native.run(cs.token, false));
            settleBusy(this, cs);
            return p.promise;
        }
        close() {
            let cs;
            try { cs = contextOf(this, 'AudioContext'); } catch (e) { return rejected(e); }
            native.status(cs.token, cs.status);
            if (!cs.status[5]) return rejected(new DOMException('The document is not fully active', 'InvalidStateError'));
            if (cs.control === 'closed') return rejected(new DOMException('The context is already closed', 'InvalidStateError'));
            const p = promise();
            cs.pendingClose.push(p);
            cs.control = 'closed';
            check(native.close(cs.token));
            settleBusy(this, cs);
            return p.promise;
        }
    }

    class OfflineAudioContext extends BaseAudioContext {
        constructor(contextOptions, length, sampleRate) {
            if (!arguments.length) throw new TypeError('OfflineAudioContext requires options');
            let channels, frames, rate;
            if (arguments.length >= 3) {
                channels = unsigned(contextOptions); frames = unsigned(length); rate = float(sampleRate, 'sampleRate');
            } else {
                const o = dictionary(contextOptions, 'OfflineAudioContextOptions');
                channels = o.numberOfChannels === undefined ? 1 : unsigned(o.numberOfChannels);
                if (o.length === undefined || o.length === null) throw new TypeError("OfflineAudioContextOptions requires 'length'");
                frames = unsigned(o.length);
                if (o.sampleRate === undefined) throw new TypeError("OfflineAudioContextOptions requires 'sampleRate'");
                rate = float(o.sampleRate, 'sampleRate');
            }
            if (!native.active()) throw new DOMException('The document is not fully active', 'InvalidStateError');
            if (!channels || channels > 32) throw new DOMException('numberOfChannels must be in [1, 32]', 'NotSupportedError');
            if (!frames) throw new DOMException('length must be at least 1', 'NotSupportedError');
            build(OfflineAudioContext, null, [], () => super());
            makeContext(this, rate, channels, frames);
        }
        get length() { return contextOf(this, 'OfflineAudioContext').length; }
        startRendering() {
            let cs;
            try { cs = contextOf(this, 'OfflineAudioContext'); } catch (e) { return rejected(e); }
            native.status(cs.token, cs.status);
            if (!cs.status[5]) return rejected(new DOMException('The document is not fully active', 'InvalidStateError'));
            if (cs.started) return rejected(new DOMException('Rendering was already started', 'InvalidStateError'));
            cs.started = true;
            cs.rendering = promise();
            cs.control = 'running';
            const refused = native.run(cs.token, true);
            if (refused) { cs.rendering = null; return rejected(fail(refused, 'Rendering could not start')); }
            settleBusy(this, cs);
            return cs.rendering.promise;
        }
        resume() {
            let cs;
            try { cs = contextOf(this, 'OfflineAudioContext'); } catch (e) { return rejected(e); }
            native.status(cs.token, cs.status);
            if (!cs.status[5]) return rejected(new DOMException('The document is not fully active', 'InvalidStateError'));
            if (cs.control === 'closed') return rejected(new DOMException('The context is closed', 'InvalidStateError'));
            if (!cs.started) return rejected(new DOMException('Rendering has not started', 'InvalidStateError'));
            const p = promise();
            cs.pendingResume.push(p);
            cs.control = 'running';
            check(native.run(cs.token, true));
            settleBusy(this, cs);
            return p.promise;
        }
        suspend(suspendTime) {
            let cs, frame;
            try {
                cs = contextOf(this, 'OfflineAudioContext');
                if (!arguments.length) throw new TypeError('suspend requires a time');
                const t = finite(suspendTime, 'suspendTime');
                if (t < 0) throw new DOMException('suspendTime must not be negative', 'InvalidStateError');
                frame = Math.floor(t * cs.rate / 128) * 128;
            } catch (e) { return rejected(e); }
            native.status(cs.token, cs.status);
            if (frame < cs.status[1] || frame >= cs.length)
                return rejected(new DOMException('suspendTime is before the current time or past the end', 'InvalidStateError'));
            if (cs.suspends.has(frame)) return rejected(new DOMException('A suspension is already scheduled at that time', 'InvalidStateError'));
            const refused = native.suspendAt(cs.token, frame);
            if (refused) return rejected(fail(refused, 'The context cannot suspend at that time'));
            const p = promise();
            cs.suspends.set(frame, p);
            return p.promise;
        }
    }
    handlers(OfflineAudioContext, ['complete'], target => contexts.has(target) && contexts.get(target).offline);

    class OfflineAudioCompletionEvent extends Event {
        constructor(type, eventInitDict) {
            if (arguments.length < 2) throw new TypeError('OfflineAudioCompletionEvent requires a type and an init');
            const o = dictionary(eventInitDict, 'OfflineAudioCompletionEventInit');
            if (!buffers.has(o.renderedBuffer)) throw new TypeError("OfflineAudioCompletionEventInit requires 'renderedBuffer'");
            super(type, o);
            completions.set(this, o.renderedBuffer);
        }
        get renderedBuffer() {
            if (!completions.has(this)) throw new TypeError('Illegal OfflineAudioCompletionEvent receiver');
            return completions.get(this);
        }
    }

    const interfaces = {
        AudioContext, BaseAudioContext, OfflineAudioContext, OfflineAudioCompletionEvent, AudioBuffer, AudioNode,
        AudioParam, AudioScheduledSourceNode, AnalyserNode, AudioBufferSourceNode, AudioDestinationNode, AudioListener,
        BiquadFilterNode, ChannelMergerNode, ChannelSplitterNode, ConstantSourceNode, ConvolverNode, DelayNode,
        DynamicsCompressorNode, GainNode, IIRFilterNode, OscillatorNode, PannerNode, PeriodicWave, StereoPannerNode,
        WaveShaperNode,
    };
    for (const [name, C] of Object.entries(interfaces))
        defineProperty(C.prototype, Symbol.toStringTag, {value: name, configurable: true});
    return interfaces;
})
