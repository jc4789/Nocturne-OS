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
  const nativeRandom = host.cryptoRandom, nativeDigest = host.cryptoDigest;
  const error = (name, message) => new DomErr(message, name);
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

  // There are no child browsing contexts yet. Use the trusted initial document URL,
  // never the page-writable location/isSecureContext properties. Unknown schemes fail closed.
  function trustedContext() {
    try {
      const raw = host.url(), url = new globalThis.URL(raw);
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
    for (const name of ['encrypt', 'decrypt', 'sign', 'verify', 'generateKey',
      'deriveKey', 'deriveBits', 'importKey', 'exportKey', 'wrapKey', 'unwrapKey']) {
      define(SubtleCrypto.prototype, name, {
        value: function() {
          const e = this !== subtle ? new TypeErr('Illegal invocation') :
            error('NotSupportedError', 'This cryptographic operation is not implemented');
          return new P((_, reject) => reject(e));
        }, writable: true, enumerable: true, configurable: true
      });
    }
    define(globalThis, 'SubtleCrypto', { value: SubtleCrypto, writable: true, configurable: true });
  }
  for (const name of ['getRandomValues']) {
    const descriptor = Object.getOwnPropertyDescriptor(Crypto.prototype, name);
    descriptor.enumerable = true; define(Crypto.prototype, name, descriptor);
  }
  const digestDescriptor = Object.getOwnPropertyDescriptor(SubtleCrypto.prototype, 'digest');
  digestDescriptor.enumerable = true; define(SubtleCrypto.prototype, 'digest', digestDescriptor);
  define(globalThis, 'Crypto', { value: Crypto, writable: true, configurable: true });
  define(globalThis, 'crypto', { get: () => crypto, enumerable: true, configurable: true });
  define(globalThis, 'isSecureContext', { get: () => secure, enumerable: true, configurable: true });
  return secure; // Private bootstrap value, not the page-writable global property.
})(host);
