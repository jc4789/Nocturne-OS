/* Auxiliary API regression tests, not real-site acceptance. No shell/filesystem APIs. */
globalThis.runCryptoCases = async function runCryptoCases() {
  let checks = 0;
  const assert = (ok, label) => { if (!ok) throw new Error(label); checks++; };
  const throws = (fn, name, label) => {
    let caught; try { fn(); } catch (e) { caught = e; }
    assert(caught && caught.name === name, label);
  };
  const rejects = async (fn, name, label) => {
    let promise;
    try { promise = fn(); } catch (_) { throw new Error(label + ': synchronous throw'); }
    assert(promise instanceof Promise, label + ': Promise');
    let caught; try { await promise; } catch (e) { caught = e; }
    assert(caught && caught.name === name, label);
  };
  const hex = value => Array.from(new Uint8Array(value), b => b.toString(16).padStart(2, '0')).join('');
  assert(crypto === crypto, 'same crypto identity');
  assert(crypto instanceof Crypto, 'Crypto prototype');
  assert(Object.prototype.toString.call(crypto) === '[object Crypto]', 'Crypto tag');
  throws(() => new Crypto(), 'TypeError', 'illegal constructor');
  throws(() => crypto.getRandomValues.call({}, new Uint8Array(1)), 'TypeError', 'random brand');
  for (const value of [undefined, null, {}, [], 1, 'a', new ArrayBuffer(1)])
    throws(() => crypto.getRandomValues(value), 'TypeError', 'random non-view');
  for (const Type of [Int8Array, Uint8Array, Uint8ClampedArray, Int16Array,
    Uint16Array, Int32Array, Uint32Array, BigInt64Array, BigUint64Array]) {
    const array = new Type(32);
    assert(crypto.getRandomValues(array) === array, Type.name + ' identity');
    const empty = new Type(0);
    assert(crypto.getRandomValues(empty) === empty, Type.name + ' empty');
  }
  for (const value of [new Float32Array(0), new Float64Array(2), new DataView(new ArrayBuffer(4))])
    throws(() => crypto.getRandomValues(value), 'TypeMismatchError', 'random non-integer view');
  if (typeof Float16Array === 'function')
    throws(() => crypto.getRandomValues(new Float16Array(2)), 'TypeMismatchError', 'float16');
  const large = new Uint8Array(65537); large.fill(91);
  throws(() => crypto.getRandomValues(large), 'QuotaExceededError', 'random quota');
  assert(large.every(x => x === 91), 'quota does not write');
  const boundary = new Uint32Array(16384);
  assert(crypto.getRandomValues(boundary) === boundary, '65536 bytes accepted');
  throws(() => crypto.getRandomValues(new Uint32Array(16385)), 'QuotaExceededError', 'quota in bytes');
  const backing = new Uint8Array(80); backing.fill(77);
  const offset = new Uint32Array(backing.buffer, 8, 16);
  crypto.getRandomValues(offset);
  assert(backing.subarray(0, 8).every(x => x === 77) && backing.subarray(72).every(x => x === 77), 'random offset bounds');
  const spoof = new Uint8Array(8);
  Object.defineProperties(spoof, { byteLength: { value: 999999 }, byteOffset: { value: 99999 },
    buffer: { value: null }, [Symbol.toStringTag]: { value: 'Float64Array' } });
  assert(crypto.getRandomValues(spoof) === spoof, 'intrinsic typed-array access');
  if (typeof SharedArrayBuffer === 'function') {
    throws(() => crypto.getRandomValues(new Uint8Array(new SharedArrayBuffer(8))), 'TypeError', 'random shared rejected');
  }
  if (!isSecureContext) {
    assert(crypto.subtle === undefined && crypto.randomUUID === undefined, 'insecure methods hidden');
    assert(typeof SubtleCrypto === 'undefined', 'insecure interface hidden');
    return checks;
  }
  assert(crypto.subtle === crypto.subtle && crypto.subtle instanceof SubtleCrypto, 'subtle identity');
  throws(() => new SubtleCrypto(), 'TypeError', 'subtle constructor');
  throws(() => crypto.randomUUID.call({}), 'TypeError', 'UUID brand');
  const uuid = crypto.randomUUID();
  assert(/^[0-9a-f]{8}-[0-9a-f]{4}-4[0-9a-f]{3}-[89ab][0-9a-f]{3}-[0-9a-f]{12}$/.test(uuid), 'UUID version/variant/case');
  const vectors = [
    ['SHA-1', 'a9993e364706816aba3e25717850c26c9cd0d89d', 'da39a3ee5e6b4b0d3255bfef95601890afd80709'],
    ['SHA-256', 'ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad', 'e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855'],
    ['SHA-384', 'cb00753f45a35e8bb5a03d699ac65007272c32ab0eded1631a8b605a43ff5bed8086072ba1e7cc2358baeca134c825a7', '38b060a751ac96384cd9327eb1b1e36a21fdb71114be07434c0cc7bf63f6e1da274edebfe76f65fbd51ad2f14898b95b'],
    ['SHA-512', 'ddaf35a193617abacc417349ae20413112e6fa4e89a97ea20a9eeee64b55d39a2192992a274fc1a836ba3c23a3feebbd454d4423643ce80e2a9ac94fa54ca49f', 'cf83e1357eefb8bdf1542850d66d8007d620e4050b5715dc83f4a921d36ce9ce47d0d13c5d85f2b0ff8318d2877eec2f63b931bd47417a81a538327af927da3e']
  ];
  for (const [name, abc, empty] of vectors) {
    const data = new Uint8Array([33, 97, 98, 99, 44]);
    const promise = crypto.subtle.digest({ name: name.toLowerCase() }, new DataView(data.buffer, 1, 3));
    assert(promise instanceof Promise, name + ' Promise');
    data.fill(0); // The snapshot must have been taken before the caller can mutate it.
    const result = await promise;
    assert(result instanceof ArrayBuffer && hex(result) === abc, name + ' offset/snapshot/vector');
    assert(hex(await crypto.subtle.digest(name, new ArrayBuffer(0))) === empty, name + ' empty vector');
    assert(hex(await crypto.subtle.digest(name, new Uint8Array([97,98,99]))) === abc, name + ' typed array');
    assert(hex(await crypto.subtle.digest(name, new Uint8Array([97,98,99]).buffer)) === abc, name + ' buffer');
  }
  const multi = new Uint8Array(1000); multi.fill(97);
  for (const [name, expected] of [
    ['SHA-1', '291e9a6c66994949b57ba5e650361e98fc36b1ba'],
    ['SHA-256', '41edece42d63e8d9bf515a9ba6932e1c20cbc9f5a5d134645adb5db1b9737ea3'],
    ['SHA-384', 'f54480689c6b0b11d0303285d9a81b21a93bca6ba5a1b4472765dca4da45ee328082d469c650cd3b61b16d3266ab8ced'],
    ['SHA-512', '67ba5535a46e3f86dbfbed8cbbaf0125c76ed549ff8b0b9e03e0c88cf90fa634fa7b12b47d77b694de488ace8d9a65967dc96df599727d3292a8d9d447709c97']
  ]) assert(hex(await crypto.subtle.digest(name, multi)) === expected, name + ' multiblock');
  for (const name of ['MD5', 'SHA256', ' sha-256 ', '\u017fHA-256', '', null, 42])
    await rejects(() => crypto.subtle.digest(name, new Uint8Array()), 'NotSupportedError', 'unsupported algorithm');
  for (const algorithm of [{}, { name: undefined }, Symbol('SHA-256'), { name: Symbol() }])
    await rejects(() => crypto.subtle.digest(algorithm, new Uint8Array()), 'TypeError', 'algorithm type');
  for (const data of [undefined, null, 'abc', [97, 98, 99], { byteLength: 3 }])
    await rejects(() => crypto.subtle.digest('SHA-256', data), 'TypeError', 'data type');
  await rejects(() => crypto.subtle.digest('SHA-256'), 'TypeError', 'digest arity');
  await rejects(() => crypto.subtle.digest.call({}, 'SHA-256', new Uint8Array()), 'TypeError', 'digest brand');
  const thrown = new Error('getter');
  let caught;
  try { await crypto.subtle.digest({ get name() { throw thrown; } }, new Uint8Array()); }
  catch (e) { caught = e; }
  assert(caught === thrown, 'algorithm getter exception identity');
  if (typeof SharedArrayBuffer === 'function') {
    await rejects(() => crypto.subtle.digest('SHA-256', new SharedArrayBuffer(8)), 'TypeError', 'digest shared');
    await rejects(() => crypto.subtle.digest('SHA-256', new DataView(new SharedArrayBuffer(8))), 'TypeError', 'digest shared view');
  }
  if (typeof ArrayBuffer.prototype.transfer === 'function') {
    const buffer = new ArrayBuffer(8), view = new Uint8Array(buffer); buffer.transfer();
    throws(() => crypto.getRandomValues(view), 'TypeError', 'detached random');
    await rejects(() => crypto.subtle.digest('SHA-256', buffer), 'TypeError', 'detached digest');
    await rejects(() => crypto.subtle.digest('SHA-256', view), 'TypeError', 'detached digest view');
  }
  if (Object.getOwnPropertyDescriptor(ArrayBuffer.prototype, 'resizable')) {
    const buffer = new ArrayBuffer(8, { maxByteLength: 16 });
    throws(() => crypto.getRandomValues(new Uint8Array(buffer)), 'TypeError', 'resizable random');
    await rejects(() => crypto.subtle.digest('SHA-256', buffer), 'TypeError', 'resizable digest');
  }
  for (const name of ['encrypt','decrypt','sign','verify','generateKey','deriveKey',
    'deriveBits','importKey','exportKey','wrapKey','unwrapKey'])
    await rejects(() => crypto.subtle[name](), 'NotSupportedError', name + ' explicit unsupported');
  return checks;
};
