/* Auxiliary UTF-8 API regression, not real-site acceptance. */
globalThis.runEncodingCases = function runEncodingCases() {
    let checks = 0;
    const assert = (ok, label) => { if (!ok) throw new Error(label); checks++; };
    const throws = (fn, name, label) => {
        let error; try { fn(); } catch (e) { error = e; }
        assert(error && error.name === name, label);
    };
    const encoder = new TextEncoder(), decoder = new TextDecoder();
    assert(encoder.encoding === 'utf-8' && decoder.encoding === 'utf-8', 'encoding');
    assert(Object.prototype.toString.call(encoder) === '[object TextEncoder]', 'encoder tag');
    assert(Object.prototype.toString.call(decoder) === '[object TextDecoder]', 'decoder tag');
    assert(decoder.fatal === false && decoder.ignoreBOM === false, 'decoder defaults');
    for (const label of ['utf-8','Utf8','unicode-1-1-utf-8','unicode11utf8','unicode20utf8','x-unicode20utf8',' \tUTF-8\r\n\f '])
        assert(new TextDecoder(label).encoding === 'utf-8', 'UTF8 label');
    for (const label of ['\u00a0utf-8','utf-8\u00a0','\vutf-8','utf-8\v','','wat'])
        throws(() => new TextDecoder(label), 'RangeError', 'invalid label');
    throws(() => new TextDecoder(Symbol()), 'TypeError', 'label Symbol');
    assert(new TextDecoder('utf-8', null).fatal === false, 'null dictionary');
    assert(decoder.decode(Uint8Array.of(97), null) === 'a', 'null decode dictionary');
    for (const value of [true, 1, 'x', Symbol()]) {
        throws(() => new TextDecoder('utf8', value), 'TypeError', 'constructor dictionary');
        throws(() => decoder.decode(undefined, value), 'TypeError', 'decode dictionary');
    }
    const get = (proto, key) => Object.getOwnPropertyDescriptor(proto, key).get;
    throws(() => get(TextEncoder.prototype, 'encoding').call({}), 'TypeError', 'encoding brand');
    throws(() => encoder.encode.call({}, 'a'), 'TypeError', 'encode brand');
    throws(() => encoder.encodeInto.call({}, 'a', new Uint8Array(1)), 'TypeError', 'encodeInto brand');
    for (const key of ['encoding','fatal','ignoreBOM'])
        throws(() => get(TextDecoder.prototype, key).call({}), 'TypeError', 'decoder getter brand');
    throws(() => decoder.decode.call({}, new Uint8Array(1)), 'TypeError', 'decode brand');
    throws(() => encoder.encode(Symbol()), 'TypeError', 'encode Symbol');
    throws(() => encoder.encodeInto(Symbol(), new Uint8Array(10)), 'TypeError', 'encodeInto Symbol');
    const vectors = [
        ['', [], ''], ['a', [97], 'a'], ['\u00e9', [195,169], '\u00e9'],
        ['\u4e2d', [228,184,173], '\u4e2d'], ['\ud83d\ude00', [240,159,152,128], '\ud83d\ude00'],
        ['\ud800', [239,191,189], '\ufffd'], ['\udfff', [239,191,189], '\ufffd'],
        ['\ud800a', [239,191,189,97], '\ufffda']
    ];
    for (const [source, output, decoded] of vectors) {
        const encoded = encoder.encode(source);
        assert(Array.from(encoded).join() === output.join(), 'UTF8 encoding');
        assert(decoder.decode(encoded) === decoded, 'UTF8 roundtrip');
        for (let capacity = 0; capacity <= output.length + 1; capacity++) {
            const target = new Uint8Array(capacity), result = encoder.encodeInto(source, target);
            assert(result.written <= capacity && result.read <= source.length, 'encodeInto bounds');
            assert(Array.from(target.subarray(0,result.written)).join() === Array.from(encoder.encode(source.slice(0,result.read))).join(), 'encodeInto progress');
            if (capacity >= output.length) assert(result.read === source.length && result.written === output.length, 'encodeInto complete');
        }
    }
    const outside = new Uint8Array([42,42,42,42]), target = outside.subarray(1,3);
    const r = encoder.encodeInto('\u00e9', target);
    assert(r.read === 1 && r.written === 2 && outside.join() === '42,195,169,42', 'destination offset');
    const source = new Uint8Array([97]);
    Object.defineProperties(source, {buffer:{value:Uint8Array.of(98).buffer}, byteOffset:{value:100}, byteLength:{value:0}});
    assert(decoder.decode(source) === 'a', 'decode intrinsic view');
    const dest = new Uint8Array(1);
    Object.defineProperty(dest,'length',{value:0}); dest.set = () => { throw Error('must not call'); };
    encoder.encode = () => Uint8Array.of(98);
    assert(encoder.encodeInto('a',dest).written === 1 && dest[0] === 97, 'encodeInto internal algorithm');
    delete encoder.encode;
    for (const invalid of [new Uint8ClampedArray(3), new Int8Array(3), new DataView(new ArrayBuffer(3)), {}, null])
        throws(() => encoder.encodeInto('a',invalid), 'TypeError', 'destination type');
    for (const invalid of [null, 1, 'a', [], {}]) throws(() => decoder.decode(invalid), 'TypeError', 'decoder input type');
    for (const [input, expected] of [
        [[0xc0,0xaf], '\ufffd\ufffd'], [[0xe0,0x80,0x80], '\ufffd\ufffd\ufffd'],
        [[0xed,0xa0,0x80], '\ufffd\ufffd\ufffd'], [[0xf4,0x90,0x80,0x80], '\ufffd\ufffd\ufffd\ufffd'],
        [[0xe2,0x82,97], '\ufffda'], [[0xf0,0x9f], '\ufffd']
    ]) {
        assert(decoder.decode(new Uint8Array(input)) === expected, 'invalid UTF8 recovery');
        throws(() => new TextDecoder('utf8',{fatal:true}).decode(new Uint8Array(input)), 'TypeError', 'fatal UTF8');
    }
    for (let split = 0; split <= 4; split++) {
        const d = new TextDecoder(), input = Uint8Array.of(240,159,152,128);
        assert(d.decode(input.subarray(0,split),{stream:true}) + d.decode(input.subarray(split)) === '\ud83d\ude00', 'streaming split');
    }
    const bom = Uint8Array.of(239,187,191,97);
    assert(decoder.decode(bom) === 'a', 'BOM skip');
    assert(new TextDecoder('utf8',{ignoreBOM:true}).decode(bom) === '\ufeffa', 'BOM preserve');
    const stream = new TextDecoder();
    assert(stream.decode(bom.subarray(0,1),{stream:true}) === '' && stream.decode(bom.subarray(1)) === 'a', 'BOM split');
    if (typeof ArrayBuffer.prototype.transfer === 'function') {
        const b = new ArrayBuffer(2), v = new Uint8Array(b); b.transfer();
        throws(() => decoder.decode(v), 'TypeError', 'detached decode');
        throws(() => encoder.encodeInto('a',v), 'TypeError', 'detached encodeInto');
    }
    // SharedArrayBuffer is intentionally excluded: Nocturne does not expose it.
    return checks;
};
