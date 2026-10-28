// Cases WPT's compression/ does not reach, under testharness.js in compression_hosttest.c:
// chunks past js_compression.c's 1 MiB steps both ways, the end of the data at a chunk
// boundary, and the single-stream rules of the standard's formats.
// Needs pako (WPT's compression/third_party/pako) and resources/concatenate-stream.js.
'use strict';

const formats = ['deflate', 'deflate-raw', 'gzip', 'brotli'];

function pattern(length) {
  // Compressible but not trivially: runs of text with a counter in them.
  const bytes = new Uint8Array(length);
  let x = 1;
  for (let i = 0; i < length; i++) {
    if (i % 4096 < 3000) bytes[i] = 97 + (i % 26);
    else { x = (x * 1103515245 + 12345) >>> 0; bytes[i] = x >>> 24; }
  }
  return bytes;
}

async function through(stream, chunks, close = true) {
  const writer = stream.writable.getWriter();
  for (const chunk of chunks) writer.write(chunk).catch(() => {});
  if (close) writer.close().catch(() => {});
  return concatenateStream(stream.readable);
}

for (const format of formats) {
  promise_test(async () => {
    const input = pattern(3 * 1024 * 1024 + 123);
    const compressed = await through(new CompressionStream(format), [input]);
    assert_less_than(compressed.length, input.length, 'compressed output should be smaller');
    const back = await through(new DecompressionStream(format), [compressed]);
    assert_equals(back.length, input.length, 'length');
    assert_equals(back.toString(), input.toString(), 'bytes');
    if (format === 'gzip' || format === 'deflate') {
      const other = pako.inflate(compressed);
      assert_equals(other.length, input.length, 'pako agrees');
    } else if (format === 'deflate-raw') {
      assert_equals(pako.inflateRaw(compressed).length, input.length, 'pako agrees');
    }
  }, `${format}: a chunk of more than three steps goes both ways`);

  promise_test(async () => {
    const input = new Uint8Array(9 * 1024 * 1024);
    const compressed = await through(new CompressionStream(format), [input]);
    assert_less_than(compressed.length, 64 * 1024, 'zeros compress well');
    const ds = new DecompressionStream(format);
    const writer = ds.writable.getWriter();
    writer.write(compressed);
    writer.close();
    const reader = ds.readable.getReader();
    let total = 0, pieces = 0, largest = 0;
    for (;;) {
      const {value, done} = await reader.read();
      if (done) break;
      assert_equals(value.constructor, Uint8Array, 'Uint8Array chunks');
      assert_true(value.byteLength > 0, 'no empty chunk');
      total += value.byteLength; pieces++; largest = Math.max(largest, value.byteLength);
    }
    assert_equals(total, input.length, 'the whole output');
    assert_true(pieces > 9 && largest <= 65536, `pieces ${pieces}, largest ${largest}`);
  }, `${format}: a small chunk that expands past many output steps`);

  promise_test(async t => {
    const compressed = await through(new CompressionStream(format), [new TextEncoder().encode('expected output')]);
    const ds = new DecompressionStream(format);
    const writer = ds.writable.getWriter(), reader = ds.readable.getReader();
    writer.write(compressed);
    const first = await reader.read();
    assert_equals(new TextDecoder().decode(first.value), 'expected output');
    const next = reader.read();  // takes the backpressure off the writes below
    const empty = writer.write(new Uint8Array(0));
    const late = writer.write(new Uint8Array([1]));
    await empty;
    await promise_rejects_js(t, TypeError, late, 'a byte after the end, in its own chunk');
    await promise_rejects_js(t, TypeError, next, 'the readable side errors');
  }, `${format}: the end at a chunk boundary, then an empty chunk, then a byte`);

  promise_test(async () => {
    const parts = [];
    for (let i = 0; i < 300; i++) parts.push(new TextEncoder().encode(`chunk ${i} `.repeat(i % 7 + 1)));
    const compressed = await through(new CompressionStream(format), parts);
    const pieces = [];
    for (let at = 0; at < compressed.length; at += 5) pieces.push(compressed.subarray(at, at + 5));
    const back = new TextDecoder().decode(await through(new DecompressionStream(format), pieces));
    let want = '';
    for (let i = 0; i < 300; i++) want += `chunk ${i} `.repeat(i % 7 + 1);
    assert_equals(back, want);
  }, `${format}: many small chunks in, five-byte pieces back`);
}

promise_test(async t => {
  const one = await through(new CompressionStream('gzip'), [new TextEncoder().encode('one')]);
  const two = await through(new CompressionStream('gzip'), [new TextEncoder().encode('two')]);
  const both = new Uint8Array(one.length + two.length);
  both.set(one); both.set(two, one.length);
  const ds = new DecompressionStream('gzip');
  const writer = ds.writable.getWriter(), reader = ds.readable.getReader();
  writer.write(both).catch(() => {});
  const first = await reader.read();
  assert_equals(new TextDecoder().decode(first.value), 'one', 'the first member');
  await promise_rejects_js(t, TypeError, reader.read(), 'a second member is data after the end');
}, 'gzip: one member only');

promise_test(async t => {
  const raw = await through(new CompressionStream('deflate-raw'), [new TextEncoder().encode('bare data')]);
  const ds = new DecompressionStream('deflate');
  const writer = ds.writable.getWriter(), reader = ds.readable.getReader();
  writer.write(raw).catch(() => {});
  writer.close().catch(() => {});
  await promise_rejects_js(t, TypeError, reader.read(), 'no zlib header');
}, 'deflate: a bare DEFLATE stream is not the zlib format');

test(() => {
  for (const name of ['CompressionStream', 'DecompressionStream']) {
    const d = Object.getOwnPropertyDescriptor(globalThis, name);
    assert_true(d.writable && d.configurable && !d.enumerable, `${name} on the global`);
    const p = Object.getOwnPropertyDescriptor(globalThis[name].prototype, 'readable');
    assert_true(p.enumerable && p.configurable && p.set === undefined && p.get.name === 'get readable', `${name}.prototype.readable`);
  }
  const cs = new CompressionStream('gzip');
  assert_throws_js(TypeError, () => Object.getOwnPropertyDescriptor(DecompressionStream.prototype, 'readable').get.call(cs));
  assert_equals(Object.prototype.toString.call(cs), '[object CompressionStream]');
  assert_true(cs.readable instanceof ReadableStream && cs.writable instanceof WritableStream);
  assert_equals(cs.readable, cs.readable, 'the same stream each time');
  assert_throws_js(TypeError, () => new CompressionStream(Symbol()));
  assert_throws_js(TypeError, () => CompressionStream('gzip'));
}, 'Web IDL shapes');

test(() => {
  const ts = new TextDecoderStream();
  assert_true(ts.readable instanceof ReadableStream && !Object.hasOwn(ts, 'readable'), 'TextDecoderStream shares the mixin');
  assert_true(new TextEncoderStream().writable instanceof WritableStream);
}, 'GenericTransformStream on the encoding streams');

promise_test(async () => {
  // A page's then getter on Object.prototype must not reach the transformer setup.
  let seen = 0;
  Object.defineProperty(Object.prototype, 'start', {get() { seen++; return undefined; }, configurable: true});
  Object.defineProperty(Object.prototype, 'highWaterMark', {get() { seen++; return undefined; }, configurable: true});
  try {
    const cs = new CompressionStream('deflate');
    await through(cs, [new Uint8Array([1, 2, 3])]);
  } finally {
    delete Object.prototype.start;
    delete Object.prototype.highWaterMark;
  }
  assert_equals(seen, 0, 'no transformer or strategy lookup on Object.prototype');
}, 'the transform is set up without page-visible lookups');
