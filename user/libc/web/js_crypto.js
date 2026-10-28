/* Embedded inside js_bootstrap.js after DOMException and the URL bindings. */
const cryptoSecureContext = (function installCrypto(host) {
  'use strict';
  const apply = Reflect.apply, define = Object.defineProperty;
  const U8 = Uint8Array, AB = ArrayBuffer, P = Promise, TypeErr = TypeError;
  const DomErr = DOMException, str = String, upper = String.prototype.toUpperCase;
  const asciiAlgorithm = /^sha-(?:1|256|384|512)$/i;
  const test = RegExp.prototype.test, includes = Array.prototype.includes;
  const isView = AB.isView, proto = Object.getPrototypeOf(U8.prototype);
  const getter = (p, name) => Object.getOwnPropertyDescriptor(p, name).get;
  const abLength = getter(AB.prototype, 'byteLength');
  const abResizable = Object.getOwnPropertyDescriptor(AB.prototype, 'resizable');
  const taBuffer = getter(proto, 'buffer'), taOffset = getter(proto, 'byteOffset');
  const taLength = getter(proto, 'byteLength'), taTag = getter(proto, Symbol.toStringTag);
  const dvBuffer = getter(DataView.prototype, 'buffer');
  const dvOffset = getter(DataView.prototype, 'byteOffset');
  const dvLength = getter(DataView.prototype, 'byteLength');
  const set = U8.prototype.set, then = P.prototype.then;
  const nativeRandom = host.cryptoRandom, nativeDigest = host.cryptoDigest, nativeAES = host.cryptoAES;
  const keySlots = new WeakMap(), keyGet = WeakMap.prototype.get, keySet = WeakMap.prototype.set;
  const freeze = Object.freeze, create=Object.create, slice=Array.prototype.slice;
  const error = (name, message) => domExceptionBridge.create(name, message);
  const integers = ['Int8Array', 'Uint8Array', 'Uint8ClampedArray', 'Int16Array',
    'Uint16Array', 'Int32Array', 'Uint32Array', 'BigInt64Array', 'BigUint64Array'];

  function bufferView(value) {
    let buffer = value, offset = 0, length;
    if (isView(value)) {
      if (apply(taTag, value, []) !== undefined) {
        buffer = apply(taBuffer, value, []);
        offset = apply(taOffset, value, []);
        length = apply(taLength, value, []);
      } else {
        buffer = apply(dvBuffer, value, []);
        offset = apply(dvOffset, value, []);
        length = apply(dvLength, value, []);
      }
    }
    const size = apply(abLength, buffer, []); // Also rejects shared or forged buffers.
    if (abResizable && apply(abResizable.get, buffer, []))
      throw new TypeErr('Resizable buffers are not accepted by this Web IDL BufferSource');
    return new U8(buffer, offset, length === undefined ? size : length);
  }
  function randomBytes(length) {
    try {
      const bytes = new U8(nativeRandom(length));
      if (apply(taLength, bytes, []) !== length) throw new TypeErr('Invalid random result');
      return bytes;
    } catch (_) {
      throw error('OperationError', 'The system random source failed');
    }
  }
  function algorithmName(value) {
    let name;
    if (value !== null && (typeof value === 'object' || typeof value === 'function')) {
      name = value.name;
      if (name === undefined) throw new TypeErr('Algorithm.name is required');
    } else name = value;
    if (typeof name === 'symbol') throw new TypeErr('Algorithm.name must be a string');
    name = str(name);
    if (!apply(test, asciiAlgorithm, [name]))
      throw error('NotSupportedError', 'Unsupported digest algorithm');
    return apply(upper, name, []);
  }
  function snapshot(value) {
    const view = bufferView(value), copy = new U8(apply(taLength, view, []));
    apply(set, copy, [view]);
    return copy;
  }
  function aesName(value) {
    let name = value && typeof value === 'object' ? value.name : value;
    if (name === undefined || typeof name === 'symbol') throw new TypeErr('Algorithm.name is required');
    name = apply(upper, str(name), []);
    if (name !== 'AES-CBC' && name !== 'AES-CTR' && name !== 'AES-GCM') throw error('NotSupportedError', 'Unsupported AES algorithm');
    return name;
  }
  function usagesList(value) {
    const usages = Array.from(value, str);
    for (const usage of usages)
      if (!apply(includes,['encrypt', 'decrypt', 'wrapKey', 'unwrapKey'],[usage]))
        throw error('SyntaxError', 'Invalid AES key usage');
    return [...new Set(usages)];
  }
  function keySlot(value) {
    const slot = apply(keyGet, keySlots, [value]);
    if (!slot) throw new TypeErr('Expected a CryptoKey');
    return slot;
  }
  class CryptoKey {
    constructor() { throw new TypeErr('Illegal constructor'); }
    get type() { keySlot(this); return 'secret'; }
    get extractable() { return keySlot(this).extractable; }
    get algorithm() { return keySlot(this).algorithm; }
    get usages() { return keySlot(this).usages; }
  }
  function makeKey(bytes, name, extractable, usages) {
    const length=apply(taLength,bytes,[]);
    if (!apply(includes,[16, 24, 32],[length])) throw error('DataError', 'Invalid AES key length');
    if (!usages.length) throw error('SyntaxError', 'A secret key requires at least one usage');
    const key = create(CryptoKey.prototype);
    apply(keySet, keySlots, [key, { bytes, extractable: !!extractable,
      algorithm: freeze({ name, length: length * 8 }), usages: freeze(apply(slice,usages,[])) }]);
    return key;
  }
  const b64chars = 'ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_';
  function encodeKey(bytes) {
    let output = '', bits = 0, acc = 0;
    for (const byte of bytes) {
      acc = (acc << 8) | byte; bits += 8;
      while (bits >= 6) { bits -= 6; output += b64chars[(acc >>> bits) & 63]; }
    }
    if (bits) output += b64chars[(acc << (6 - bits)) & 63];
    return output;
  }
  function decodeKey(value) {
    if (typeof value !== 'string' || !/^[A-Za-z0-9_-]*$/.test(value) || value.length % 4 === 1)
      throw error('DataError', 'Invalid JWK key encoding');
    const bytes = new U8(Math.floor(value.length * 6 / 8)); let bits = 0, acc = 0, at = 0;
    for (const char of value) {
      acc = (acc << 6) | b64chars.indexOf(char); bits += 6;
      if (bits >= 8) { bits -= 8; bytes[at++] = (acc >>> bits) & 255; }
    }
    if (bits && (acc & ((1 << bits) - 1))) throw error('DataError', 'Noncanonical JWK key encoding');
    return bytes;
  }
  function reject(e) { return new P((_, fail) => fail(e)); }
  function deferred(action) { return apply(then, new P(resolve => resolve()), [action]); }
  function cryptOperation(receiver, algorithm, key, data, decrypt) {
    try {
      if (receiver !== subtle) throw new TypeErr('Illegal invocation');
      const bytes = snapshot(data), name = aesName(algorithm), slot = keySlot(key);
      if (slot.algorithm.name !== name || !apply(includes,slot.usages,[decrypt ? 'decrypt' : 'encrypt']))
        throw error('InvalidAccessError', 'Key algorithm or usage does not allow this operation');
      if (!algorithm || typeof algorithm !== 'object') throw new TypeErr('AES parameters are required');
      const iv = snapshot(name === 'AES-CTR' ? algorithm.counter : algorithm.iv);
      if (name!=='AES-GCM'&&iv.length !== 16) throw error('OperationError', 'AES IV or counter must be 16 bytes');
      const bits = name === 'AES-CTR' ? Number(algorithm.length) :
        name==='AES-GCM'&&algorithm.tagLength!==undefined?Number(algorithm.tagLength):128;
      if (name === 'AES-CTR' && (!Number.isInteger(bits) || bits < 1 || bits > 128))
        throw error('OperationError', 'AES counter length must be between 1 and 128');
      if(name==='AES-GCM'&&!apply(includes,[32,64,96,104,112,120,128],[bits]))
        throw error('OperationError','Invalid GCM authentication tag length');
      const aad=name==='AES-GCM'&&algorithm.additionalData!==undefined?snapshot(algorithm.additionalData):new U8(0);
      return deferred(() => {
        try { return nativeAES(name === 'AES-CBC' ? 1 : name==='AES-CTR'?2:3, decrypt ? 1 : 0,
          apply(taBuffer, slot.bytes, []), apply(taBuffer, iv, []), apply(taBuffer, bytes, []), bits,apply(taBuffer,aad,[])); }
        catch (_) { throw error('OperationError', 'AES operation failed'); }
      });
    } catch (e) { return reject(e); }
  }

  // Use only private native authority, never author-writable location or
  // isSecureContext. Blob Workers inherit their native creator-origin snapshot;
  // ordinary Workers keep their native policy URL. Unknown origins fail closed.
  function trustedContext() {
    try {
      const raw = typeof host.securityURL === 'function' ? host.securityURL() : host.url(), url = new globalThis.URL(raw);
      if (url.protocol === 'https:' && raw.startsWith('https://')) return true;
      if (url.protocol === 'file:' && raw.startsWith('file://')) return true;
      if (url.protocol !== 'http:' || !raw.startsWith('http://')) return false;
      // The native transport has not adopted WHATWG hostname/IPv4 parsing yet.
      // Do not upgrade a non-loopback native spelling solely because URL normalizes it.
      const authority = raw.slice(7).split(/[/?]/, 1)[0];
      const nativeHost = /^(\[[^\]]+\]|[^:]+)(?::[0-9]+)?$/.exec(authority);
      if (!nativeHost || nativeHost[1] !== url.hostname) return false;
      if (url.hostname === 'localhost' || url.hostname === '[::1]') return true;
      // Nocturne guarantees exact "localhost" in its resolver, not *.localhost.
      const parts = url.hostname.split('.');
      return parts.length === 4 && parts[0] === '127' && parts.every(p => /^\d{1,3}$/.test(p) && +p <= 255);
    } catch (_) { return false; }
  }
  const secure = trustedContext();
  class Crypto {
    constructor() { throw new TypeErr('Illegal constructor'); }
    getRandomValues(array) {
      if (this !== crypto) throw new TypeErr('Illegal invocation');
      if (!isView(array)) throw new TypeErr('Expected an ArrayBufferView');
      const bytes = bufferView(array);
      if (!apply(includes, integers, [apply(taTag, array, [])]))
        throw error('TypeMismatchError', 'Expected an integer typed array');
      const length = apply(taLength, bytes, []);
      if (length > 65536) throw error('QuotaExceededError', 'Random request exceeds 65536 bytes');
      if (length) apply(set, bytes, [randomBytes(length)]);
      return array;
    }
  }
  class SubtleCrypto {
    constructor() { throw new TypeErr('Illegal constructor'); }
    digest(algorithm, data) {
      // All binding/normalization errors reject the Promise, never throw synchronously.
      let name, snapshot;
      try {
        if (this !== subtle) throw new TypeErr('Illegal invocation');
        if (arguments.length < 2) throw new TypeErr('Digest requires algorithm and data');
        bufferView(data); // Web IDL validation precedes algorithm normalization.
        name = algorithmName(algorithm);
        const view = bufferView(data); // A name getter may detach or modify the input.
        snapshot = new U8(apply(taLength, view, []));
        apply(set, snapshot, [view]);
      } catch (e) { return new P((_, reject) => reject(e)); }
      return apply(then, new P(resolve => resolve()), [() => {
        try { return nativeDigest(name, apply(taBuffer, snapshot, [])); }
        catch (_) { throw error('OperationError', 'The digest operation failed'); }
      }]);
    }
    encrypt(algorithm, key, data) { return cryptOperation(this, algorithm, key, data, false); }
    decrypt(algorithm, key, data) { return cryptOperation(this, algorithm, key, data, true); }
    importKey(format, keyData, algorithm, extractable, keyUsages) {
      try {
        if (this !== subtle) throw new TypeErr('Illegal invocation');
        format = str(format);
        const bytes = format === 'raw' ? snapshot(keyData) : null;
        const name = aesName(algorithm), usages = usagesList(keyUsages);
        if (format === 'raw') return deferred(() => makeKey(bytes, name, extractable, usages));
        if (format !== 'jwk') throw error('NotSupportedError', 'Unsupported AES key format');
        if (!keyData || keyData.kty !== 'oct') throw error('DataError', 'Expected an octet JWK');
        const decoded = decodeKey(keyData.k), alg = keyData.alg;
        const suffix=name==='AES-CBC'?'CBC':name==='AES-CTR'?'CTR':'GCM';
        if (alg !== undefined && alg !== 'A' + apply(taLength,decoded,[]) * 8 + suffix)
          throw error('DataError', 'JWK algorithm does not match');
        if (keyData.use !== undefined && keyData.use !== 'enc' && usages.length)
          throw error('DataError', 'JWK use does not match');
        if (keyData.ext === false && extractable) throw error('DataError', 'JWK is not extractable');
        if (keyData.key_ops !== undefined) {
          const ops = Array.from(keyData.key_ops, str);
          if (new Set(ops).size !== ops.length || ops.some(x=>!apply(includes,['encrypt','decrypt','wrapKey','unwrapKey'],[x])) || usages.some(x => !apply(includes,ops,[x])))
            throw error('DataError', 'JWK key operations do not match');
        }
        return deferred(() => makeKey(decoded, name, extractable, usages));
      } catch (e) { return reject(e); }
    }
    generateKey(algorithm, extractable, keyUsages) {
      try {
        if (this !== subtle) throw new TypeErr('Illegal invocation');
        const name = aesName(algorithm), length = Number(algorithm.length), usages = usagesList(keyUsages);
        if (!apply(includes,[128, 192, 256],[length])) throw error('OperationError', 'Invalid AES key length');
        return deferred(() => makeKey(randomBytes(length / 8), name, extractable, usages));
      } catch (e) { return reject(e); }
    }
    exportKey(format, key) {
      try {
        if (this !== subtle) throw new TypeErr('Illegal invocation');
        const slot = keySlot(key); format = str(format);
        if (!slot.extractable) throw error('InvalidAccessError', 'Key is not extractable');
        if (format !== 'raw' && format !== 'jwk') throw error('NotSupportedError', 'Unsupported AES key format');
        return deferred(() => format === 'raw' ? apply(taBuffer, snapshot(slot.bytes), []) : {
          kty: 'oct', k: encodeKey(slot.bytes), alg: 'A' + apply(taLength,slot.bytes,[]) * 8 +
            (slot.algorithm.name==='AES-CBC'?'CBC':slot.algorithm.name==='AES-CTR'?'CTR':'GCM'), ext: true, key_ops: apply(slice,slot.usages,[])
        });
      } catch (e) { return reject(e); }
    }
  }
  const crypto = Object.create(Crypto.prototype);
  const subtle = Object.create(SubtleCrypto.prototype);
  define(Crypto.prototype, Symbol.toStringTag, { value: 'Crypto', configurable: true });
  define(SubtleCrypto.prototype, Symbol.toStringTag, { value: 'SubtleCrypto', configurable: true });
  if (secure) {
    define(Crypto.prototype, 'subtle', {
      get() { if (this !== crypto) throw new TypeErr('Illegal invocation'); return subtle; },
      enumerable: true, configurable: true
    });
    define(Crypto.prototype, 'randomUUID', {
      value: function randomUUID() {
        if (this !== crypto) throw new TypeErr('Illegal invocation');
        const bytes = randomBytes(16), hex = '0123456789abcdef';
        bytes[6] = (bytes[6] & 15) | 64;
        bytes[8] = (bytes[8] & 63) | 128;
        let result = '';
        for (let i = 0; i < 16; i++) {
          if (i === 4 || i === 6 || i === 8 || i === 10) result += '-';
          result += hex[bytes[i] >> 4] + hex[bytes[i] & 15];
        }
        return result;
      }, writable: true, enumerable: true, configurable: true
    });
    // Unsupported operations reject explicitly. They never manufacture keys/results.
    for (const name of ['sign', 'verify', 'deriveKey', 'deriveBits', 'wrapKey', 'unwrapKey']) {
      define(SubtleCrypto.prototype, name, {
        value: function() {
          const e = this !== subtle ? new TypeErr('Illegal invocation') :
            error('NotSupportedError', 'This cryptographic operation is not implemented');
          return new P((_, reject) => reject(e));
        }, writable: true, enumerable: true, configurable: true
      });
    }
    define(globalThis, 'SubtleCrypto', { value: SubtleCrypto, writable: true, configurable: true });
    define(CryptoKey.prototype, Symbol.toStringTag, { value: 'CryptoKey', configurable: true });
    for (const name of ['type', 'extractable', 'algorithm', 'usages']) {
      const descriptor = Object.getOwnPropertyDescriptor(CryptoKey.prototype, name);
      descriptor.enumerable = true; define(CryptoKey.prototype, name, descriptor);
    }
    define(globalThis, 'CryptoKey', { value: CryptoKey, writable: true, configurable: true });
  }
  for (const name of ['getRandomValues']) {
    const descriptor = Object.getOwnPropertyDescriptor(Crypto.prototype, name);
    descriptor.enumerable = true; define(Crypto.prototype, name, descriptor);
  }
  for (const name of ['digest', 'encrypt', 'decrypt', 'importKey', 'exportKey', 'generateKey']) {
    const descriptor = Object.getOwnPropertyDescriptor(SubtleCrypto.prototype, name);
    descriptor.enumerable = true; define(SubtleCrypto.prototype, name, descriptor);
  }
  define(globalThis, 'Crypto', { value: Crypto, writable: true, configurable: true });
  define(globalThis, 'crypto', { get: () => crypto, enumerable: true, configurable: true });
  define(globalThis, 'isSecureContext', { get: () => secure, enumerable: true, configurable: true });
  return secure; // Private bootstrap value, not the page-writable global property.
})(host);
