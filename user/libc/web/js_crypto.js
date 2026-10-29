/* Embedded inside js_bootstrap.js after DOMException and the URL bindings, and inside the
   Worker runtime. The Web Cryptography API (https://w3c.github.io/webcrypto/): Crypto,
   SubtleCrypto and CryptoKey. Every specification step is here: the Web IDL conversions,
   "normalize an algorithm", the registered algorithms' operations, the key formats (raw, spki,
   pkcs8, jwk) and the error types in the order the specification raises them. js_crypto.c
   holds only the primitives (BearSSL, and Ed25519). The steps the specification runs "in
   parallel" run in a task of this realm's crypto task source (host.postTask: js.c's posted
   tasks in a Window, the Worker runtime's queue in a Worker), which settles the promise; RSA
   key generation runs on a native thread meanwhile. Results are created in this realm. */
const cryptoSecureContext = (function installCrypto(host) {
  'use strict';
  const apply = Reflect.apply, define = Object.defineProperty, create = Object.create, objectKeys = Object.keys;
  const ownDescriptor = Object.getOwnPropertyDescriptor, isArray = Array.isArray;
  const U8 = Uint8Array, AB = ArrayBuffer, P = Promise, TypeErr = TypeError;
  const str = String, isView = AB.isView, proto = Object.getPrototypeOf(U8.prototype);
  const getter = (p, name) => Object.getOwnPropertyDescriptor(p, name).get;
  const abLength = getter(AB.prototype, 'byteLength');
  const abResizable = Object.getOwnPropertyDescriptor(AB.prototype, 'resizable');
  const abDetachedDescriptor = Object.getOwnPropertyDescriptor(AB.prototype, 'detached');
  const abDetached = abDetachedDescriptor && abDetachedDescriptor.get;
  const SAB = typeof SharedArrayBuffer === 'function' ? SharedArrayBuffer : null;
  const sabLength = SAB && getter(SAB.prototype, 'byteLength');
  const taBuffer = getter(proto, 'buffer'), taOffset = getter(proto, 'byteOffset');
  const taLength = getter(proto, 'byteLength'), taTag = getter(proto, Symbol.toStringTag);
  const dvBuffer = getter(DataView.prototype, 'buffer');
  const dvOffset = getter(DataView.prototype, 'byteOffset');
  const dvLength = getter(DataView.prototype, 'byteLength');
  const u8Set = U8.prototype.set;
  const iteratorSymbol = Symbol.iterator;
  const charCode = String.prototype.charCodeAt, fromCharCode = String.fromCharCode;
  const jsonStringify = JSON.stringify, jsonParse = JSON.parse, BigIntType = BigInt;
  const keySlots = new WeakMap(), keyGet = WeakMap.prototype.get, keySet = WeakMap.prototype.set;
  const floor = Math.floor, abs = Math.abs, isFinite = Number.isFinite;
  const error = (name, message) => domExceptionBridge.create(name, message);
  const native = {
    random: host.cryptoRandom, digest: host.cryptoDigest, aes: host.cryptoAES, hmac: host.cryptoHMAC,
    pbkdf2: host.cryptoPBKDF2Step, hkdf: host.cryptoHKDF,
    rsaGenerate: host.cryptoRSAGenerateStart, rsaGenerated: host.cryptoRSAGenerateResult, jobWait: host.cryptoJobWait,
    rsaPrivate: host.cryptoRSAPrivate, rsaPublic: host.cryptoRSAPublic,
    ecGenerate: host.cryptoECGenerate, ecPublic: host.cryptoECPublic, ecValid: host.cryptoECValid,
    ecdsa: host.cryptoECDSA, ecdh: host.cryptoECDH, x25519: host.cryptoX25519, ed25519: host.cryptoEd25519
  };
  const integers = ['Int8Array', 'Uint8Array', 'Uint8ClampedArray', 'Int16Array',
    'Uint16Array', 'Int32Array', 'Uint32Array', 'BigInt64Array', 'BigUint64Array'];
  const includes = (list, value) => { for (let i = 0; i < list.length; i++) if (list[i] === value) return true; return false; };
  const append = (list, value) => { define(list, list.length, {value, writable: true, enumerable: true, configurable: true}); };

  /* ---- Bytes ---- */
  const byteLength = view => apply(taLength, view, []);
  function shared(buffer) {
    if (!sabLength) return false;
    try { apply(sabLength, buffer, []); return true; } catch (_) { return false; }
  }
  /* A BufferSource's or ArrayBufferView's bytes. None of this API's buffer types is
     [AllowShared] or [AllowResizable]. */
  function bufferView(value) {
    const typed = isView(value) && apply(taTag, value, []) !== undefined;
    const buffer = !isView(value) ? value : typed ? apply(taBuffer, value, []) : apply(dvBuffer, value, []);
    if (shared(buffer)) throw new TypeErr('A shared buffer is not accepted by this BufferSource');
    const size = apply(abLength, buffer, []); // Also rejects forged buffers.
    if (abResizable && apply(abResizable.get, buffer, []))
      throw new TypeErr('Resizable buffers are not accepted by this Web IDL BufferSource');
    // A detached buffer holds no bytes (Web IDL "get a copy of the bytes held by the buffer source").
    if (abDetached && apply(abDetached, buffer, [])) return new U8(0);
    if (buffer === value) return new U8(buffer, 0, size);
    return typed ? new U8(buffer, apply(taOffset, value, []), apply(taLength, value, [])) :
      new U8(buffer, apply(dvOffset, value, []), apply(dvLength, value, []));
  }
  /* "Get a copy of the bytes held by the buffer source." */
  function snapshot(value) {
    const view = bufferView(value), copy = new U8(byteLength(view));
    apply(u8Set, copy, [view]);
    return copy;
  }
  function copy(bytes) { const out = new U8(byteLength(bytes)); apply(u8Set, out, [bytes]); return out; }
  /* A view of bytes[start, end); callers keep start <= end <= the length. */
  const sub = (bytes, start, end) => new U8(apply(taBuffer, bytes, []), apply(taOffset, bytes, []) + start, end - start);
  /* The whole ArrayBuffer of a fresh copy, for native calls and results. */
  const buffer = bytes => apply(taBuffer, copy(bytes), []);
  const fromNative = value => new U8(value);
  function concat(...parts) {
    let size = 0;
    for (let i = 0; i < parts.length; i++) size += byteLength(parts[i]);
    const out = new U8(size); let at = 0;
    for (let i = 0; i < parts.length; i++) { apply(u8Set, out, [parts[i], at]); at += byteLength(parts[i]); }
    return out;
  }
  function equalBytes(a, b) {
    if (byteLength(a) !== byteLength(b)) return false;
    let diff = 0;
    for (let i = 0; i < byteLength(a); i++) diff |= a[i] ^ b[i];
    return diff === 0;
  }
  /* Unsigned big-endian integers without leading zero octets. */
  function trimmed(bytes) {
    let i = 0; while (i < byteLength(bytes) - 1 && bytes[i] === 0) i++;
    return i ? copy(sub(bytes, i, byteLength(bytes))) : bytes;
  }
  function bitLength(bytes) {
    let i = 0; while (i < byteLength(bytes) && bytes[i] === 0) i++;
    if (i === byteLength(bytes)) return 0;
    let bits = (byteLength(bytes) - i - 1) * 8;
    for (let v = bytes[i]; v; v >>= 1) bits++;
    return bits;
  }
  function padded(bytes, size) {
    if (byteLength(bytes) > size) return null;
    const out = new U8(size); apply(u8Set, out, [bytes, size - byteLength(bytes)]); return out;
  }
  function randomBytes(length) {
    try {
      const bytes = new U8(native.random(length));
      if (byteLength(bytes) !== length) throw new TypeErr('Invalid random result');
      return bytes;
    } catch (_) {
      throw error('OperationError', 'The system random source failed');
    }
  }
  /* The first length bits of secret (ECDH and X25519 "derive bits"). */
  function firstBits(secret, length) {
    const out = copy(sub(secret, 0, (length + 7) >> 3));
    if (length & 7) out[byteLength(out) - 1] &= (0xff << (8 - (length & 7))) & 0xff;
    return out;
  }
  function utf8Encode(text) {
    const out = [];
    for (let i = 0; i < text.length; i++) {
      let c = apply(charCode, text, [i]);
      if (c >= 0xd800 && c <= 0xdbff && i + 1 < text.length) {
        const d = apply(charCode, text, [i + 1]);
        if (d >= 0xdc00 && d <= 0xdfff) { c = 0x10000 + ((c - 0xd800) << 10) + (d - 0xdc00); i++; }
      }
      if (c >= 0xd800 && c <= 0xdfff) c = 0xfffd;
      if (c < 0x80) append(out, c);
      else if (c < 0x800) { append(out, 0xc0 | c >> 6); append(out, 0x80 | c & 63); }
      else if (c < 0x10000) { append(out, 0xe0 | c >> 12); append(out, 0x80 | c >> 6 & 63); append(out, 0x80 | c & 63); }
      else { append(out, 0xf0 | c >> 18); append(out, 0x80 | c >> 12 & 63); append(out, 0x80 | c >> 6 & 63); append(out, 0x80 | c & 63); }
    }
    return new U8(out);
  }
  /* UTF-8 decode (Encoding's, with U+FFFD for each maximal invalid subpart). */
  function utf8Decode(bytes) {
    let out = '';
    for (let i = 0; i < byteLength(bytes);) {
      const b = bytes[i];
      let need = 0, c = 0, lower = 0x80, upper = 0xbf;
      if (b < 0x80) { out += fromCharCode(b); i++; continue; }
      if (b >= 0xc2 && b <= 0xdf) { need = 1; c = b & 31; }
      else if (b >= 0xe0 && b <= 0xef) { need = 2; c = b & 15; if (b === 0xe0) lower = 0xa0; if (b === 0xed) upper = 0x9f; }
      else if (b >= 0xf0 && b <= 0xf4) { need = 3; c = b & 7; if (b === 0xf0) lower = 0x90; if (b === 0xf4) upper = 0x8f; }
      else { out += '�'; i++; continue; }
      let j = i + 1, ok = true;
      for (let k = 0; k < need; k++, j++) {
        const t = bytes[j];
        if (j >= byteLength(bytes) || t < lower || t > upper) { ok = false; break; }
        c = (c << 6) | (t & 63); lower = 0x80; upper = 0xbf;
      }
      if (!ok) { out += '�'; i = j; continue; }
      if (c >= 0x10000) { c -= 0x10000; out += fromCharCode(0xd800 + (c >> 10), 0xdc00 + (c & 1023)); }
      else out += fromCharCode(c);
      i = j;
    }
    return out;
  }
  const b64chars = 'ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_';
  /* base64url without padding (RFC 7515 §2), as JSON Web Key members use. */
  function base64url(bytes) {
    let output = '', bits = 0, acc = 0;
    for (let i = 0; i < byteLength(bytes); i++) {
      acc = ((acc << 8) | bytes[i]) & 0xffffff; bits += 8;
      while (bits >= 6) { bits -= 6; output += b64chars[(acc >>> bits) & 63]; }
    }
    if (bits) output += b64chars[(acc << (6 - bits)) & 63];
    return output;
  }
  function fromBase64url(value) {
    if (typeof value !== 'string' || value.length % 4 === 1) throw error('DataError', 'Invalid base64url in a JSON Web Key member');
    for (let i = 0; i < value.length; i++) {
      const c = apply(charCode, value, [i]);
      if (!(c >= 65 && c <= 90 || c >= 97 && c <= 122 || c >= 48 && c <= 57 || c === 45 || c === 95))
        throw error('DataError', 'Invalid base64url in a JSON Web Key member');
    }
    const bytes = new U8(floor(value.length * 6 / 8)); let bits = 0, acc = 0, at = 0;
    for (let i = 0; i < value.length; i++) {
      const c = apply(charCode, value, [i]);
      const v = c >= 65 && c <= 90 ? c - 65 : c >= 97 && c <= 122 ? c - 71 : c >= 48 && c <= 57 ? c + 4 : c === 45 ? 62 : 63;
      acc = ((acc << 6) | v) & 0xffffff; bits += 6;
      if (bits >= 8) { bits -= 8; bytes[at++] = (acc >>> bits) & 255; }
    }
    return bytes;
  }

  /* ---- DER (X.690 §10), strict: definite minimal lengths, exact contents. ---- */
  const dataError = message => error('DataError', message || 'Invalid key data');
  function derElement(bytes, at, end) {
    if (at + 2 > end) throw dataError('Truncated DER element');
    const tag = bytes[at];
    if ((tag & 0x1f) === 0x1f) throw dataError('Unsupported DER tag');
    let length = bytes[at + 1], start = at + 2;
    if (length & 0x80) {
      const count = length & 0x7f;
      if (!count || count > 4 || start + count > end || !bytes[start]) throw dataError('Invalid DER length');
      length = 0;
      for (let i = 0; i < count; i++) length = length * 256 + bytes[start + i];
      start += count;
      if (length < 128) throw dataError('Non-minimal DER length');
    }
    if (start + length > end) throw dataError('Truncated DER element');
    return {tag, start, end: start + length};
  }
  function derChildren(bytes, node) {
    const out = [];
    for (let at = node.start; at < node.end;) { const child = derElement(bytes, at, node.end); append(out, child); at = child.end; }
    return out;
  }
  function derTop(bytes, tag) {
    const node = derElement(bytes, 0, byteLength(bytes));
    if (node.tag !== tag || node.end !== byteLength(bytes)) throw dataError('Invalid DER structure');
    return node;
  }
  const contents = (bytes, node) => sub(bytes, node.start, node.end);
  function derUnsigned(bytes, node) {
    if (node.tag !== 0x02 || node.start === node.end) throw dataError('Expected a DER INTEGER');
    const v = contents(bytes, node);
    if (v[0] & 0x80) throw dataError('Negative DER INTEGER');
    if (byteLength(v) > 1 && v[0] === 0 && !(v[1] & 0x80)) throw dataError('Non-minimal DER INTEGER');
    return copy(v[0] === 0 && byteLength(v) > 1 ? sub(v, 1, byteLength(v)) : v);
  }
  function derSmall(bytes, node) {
    const v = derUnsigned(bytes, node);
    return byteLength(v) === 1 ? v[0] : -1;
  }
  function derBits(bytes, node) {
    if (node.tag !== 0x03 || node.start === node.end || bytes[node.start] !== 0) throw dataError('Expected a DER BIT STRING');
    return copy(sub(bytes, node.start + 1, node.end));
  }
  function derLength(n) {
    if (n < 128) return new U8([n]);
    const out = []; while (n) { out[out.length] = n & 255; n = floor(n / 256); }
    const r = new U8(out.length + 1); r[0] = 0x80 | out.length;
    for (let i = 0; i < out.length; i++) r[i + 1] = out[out.length - 1 - i];
    return r;
  }
  const der = (tag, ...parts) => { const body = apply(concat, null, parts); return concat(new U8([tag]), derLength(byteLength(body)), body); };
  const derInteger = value => { let v = trimmed(value); if (!byteLength(v)) v = new U8([0]); if (v[0] & 0x80) v = concat(new U8([0]), v); return der(0x02, v); };
  const derNull = new U8([0x05, 0x00]);
  const oid = (...b) => new U8([0x06, b.length, ...b]);
  const OID = {
    rsaEncryption: oid(0x2a, 0x86, 0x48, 0x86, 0xf7, 0x0d, 0x01, 0x01, 0x01),
    ecPublicKey: oid(0x2a, 0x86, 0x48, 0xce, 0x3d, 0x02, 0x01),
    ecDH: oid(0x2b, 0x81, 0x04, 0x01, 0x0c),
    X25519: oid(0x2b, 0x65, 0x6e), Ed25519: oid(0x2b, 0x65, 0x70),
    'P-256': oid(0x2a, 0x86, 0x48, 0xce, 0x3d, 0x03, 0x01, 0x07),
    'P-384': oid(0x2b, 0x81, 0x04, 0x00, 0x22), 'P-521': oid(0x2b, 0x81, 0x04, 0x00, 0x23)
  };
  const isOID = (bytes, node, value) => !!node && node.tag === 0x06 && node.start >= 2 && bytes[node.start - 1] === node.end - node.start &&
    equalBytes(sub(bytes, node.start - 2, node.end), value);
  /* AlgorithmIdentifier: {oid node, params node or null}. */
  function algorithmIdentifier(bytes, node) {
    if (node.tag !== 0x30) throw dataError('Expected an AlgorithmIdentifier');
    const parts = derChildren(bytes, node);
    if (parts.length < 1 || parts.length > 2 || parts[0].tag !== 0x06) throw dataError('Invalid AlgorithmIdentifier');
    return {oid: parts[0], params: parts.length > 1 ? parts[1] : null};
  }
  /* "Parse a subjectPublicKeyInfo" (RFC 5280 §4.1). */
  function parseSPKI(bytes) {
    const parts = derChildren(bytes, derTop(bytes, 0x30));
    if (parts.length !== 2) throw dataError('Invalid SubjectPublicKeyInfo');
    return {algorithm: algorithmIdentifier(bytes, parts[0]), key: derBits(bytes, parts[1]), bytes};
  }
  /* "Parse a privateKeyInfo" (RFC 5208 §5, RFC 5958 §2). */
  function parsePKCS8(bytes) {
    const parts = derChildren(bytes, derTop(bytes, 0x30));
    if (parts.length < 3) throw dataError('Invalid PrivateKeyInfo');
    const version = derSmall(bytes, parts[0]);
    if (version !== 0 && version !== 1) throw dataError('Unsupported PrivateKeyInfo version');
    if (parts[2].tag !== 0x04) throw dataError('Invalid PrivateKeyInfo key');
    let at = 3;
    if (at < parts.length && parts[at].tag === 0xa0) at++;
    if (at < parts.length && parts[at].tag === 0x81 && version === 1) at++;
    if (at !== parts.length) throw dataError('Invalid PrivateKeyInfo');
    return {algorithm: algorithmIdentifier(bytes, parts[1]), key: copy(contents(bytes, parts[2])), bytes};
  }
  const spki = (algorithm, key) => der(0x30, algorithm, der(0x03, new U8([0]), key));
  const pkcs8 = (algorithm, key) => der(0x30, derInteger(new U8([0])), algorithm, der(0x04, key));

  /* ---- Web IDL ---- */
  function domString(value) {
    if (typeof value === 'symbol') throw new TypeErr('A Symbol is not a DOMString');
    return str(value);
  }
  /* [EnforceRange] integer types. */
  function enforceRange(value, max, what) {
    let x = +value;
    if (!isFinite(x)) throw new TypeErr(what + ' is not a finite number');
    x = x < 0 ? -floor(abs(x)) : floor(x);
    if (x < 0 || x > max) throw new TypeErr(what + ' is out of range');
    return x + 0;
  }
  function isObject(value) { return value !== null && (typeof value === 'object' || typeof value === 'function'); }
  function sequence(value, convert, what) {
    if (!isObject(value)) throw new TypeErr(what + ' is not a sequence');
    const method = value[iteratorSymbol];
    if (typeof method !== 'function') throw new TypeErr(what + ' is not iterable');
    const out = [];
    for (const item of {[iteratorSymbol]: () => apply(method, value, [])}) append(out, convert(item));
    return out;
  }
  const keyUsages = ['encrypt', 'decrypt', 'sign', 'verify', 'deriveKey', 'deriveBits', 'wrapKey', 'unwrapKey',
    'encapsulateKey', 'encapsulateBits', 'decapsulateKey', 'decapsulateBits'];
  const keyFormats = ['raw', 'spki', 'pkcs8', 'jwk', 'raw-public', 'raw-private', 'raw-seed', 'raw-secret'];
  function enumeration(values, what) {
    return value => { const s = domString(value); if (!includes(values, s)) throw new TypeErr(`'${s}' is not a valid ${what}`); return s; };
  }
  const keyUsage = enumeration(keyUsages, 'KeyUsage'), keyFormat = enumeration(keyFormats, 'KeyFormat');
  const usageList = value => sequence(value, keyUsage, 'keyUsages');
  function cryptoKey(value, what) {
    if (!apply(keyGet, keySlots, [value])) throw new TypeErr(what + ' is not a CryptoKey');
    return value;
  }
  function bufferSource(value, what) {
    if (!isObject(value)) throw new TypeErr(what + ' is not a BufferSource');
    bufferView(value);
    return value;
  }
  /* JsonWebKey and RsaOtherPrimesInfo: members in lexicographic order. */
  const jwkStrings = ['alg', 'crv', 'd', 'dp', 'dq', 'e'], jwkLater = ['kty', 'n'], jwkLast = ['p', 'q', 'qi', 'use', 'x', 'y'];
  const othMembers = ['d', 'r', 't'];
  const jwkMembers = ['alg', 'crv', 'd', 'dp', 'dq', 'e', 'ext', 'k', 'key_ops', 'kty', 'n', 'p', 'q', 'qi', 'use', 'x', 'y'];
  function dictionaryValue(value, what) {
    if (value === undefined || value === null) return {};
    if (!isObject(value)) throw new TypeErr(what + ' is not a dictionary');
    return value;
  }
  /* parsed: a JSON.parse result ("parse a JWK" converts it in a new global object), so only its
     own data properties count and no author prototype, getter, iterator or toString runs. */
  function jsonWebKey(value, parsed) {
    const source = dictionaryValue(value, 'JsonWebKey'), jwk = create(null);
    const get = parsed ? (o, name) => { const d = ownDescriptor(o, name); return d ? d.value : undefined; } : (o, name) => o[name];
    const string = parsed ? v => { if (isObject(v)) throw new TypeErr('Expected a string'); return str(v); } : domString;
    const list = (v, convert, what) => {
      if (!parsed) return sequence(v, convert, what);
      if (!isArray(v)) throw new TypeErr(what + ' is not a sequence');
      const out = [], count = get(v, 'length');
      for (let i = 0; i < count; i++) append(out, convert(get(v, i)));
      return out;
    };
    const read = name => { const v = get(source, name); if (v !== undefined) jwk[name] = string(v); };
    for (let i = 0; i < jwkStrings.length; i++) read(jwkStrings[i]);
    const ext = get(source, 'ext'); if (ext !== undefined) jwk.ext = !!ext;
    read('k');
    const ops = get(source, 'key_ops'); if (ops !== undefined) jwk.key_ops = list(ops, string, 'key_ops');
    for (let i = 0; i < jwkLater.length; i++) read(jwkLater[i]);
    const oth = get(source, 'oth');
    if (oth !== undefined) jwk.oth = list(oth, item => {
      const info = dictionaryValue(item, 'RsaOtherPrimesInfo'), out = create(null);
      for (let i = 0; i < othMembers.length; i++) { const name = othMembers[i], v = get(info, name); if (v !== undefined) out[name] = string(v); }
      return out;
    }, 'oth');
    for (let i = 0; i < jwkLast.length; i++) read(jwkLast[i]);
    return jwk;
  }
  /* A JsonWebKey dictionary as an ECMAScript object (its members in dictionary order). */
  function jwkObject(jwk) {
    const out = {};
    for (let i = 0; i < jwkMembers.length; i++) {
      const name = jwkMembers[i];
      if (jwk[name] === undefined) continue;
      let value = jwk[name];
      if (name === 'key_ops') { const ops = []; for (let i = 0; i < value.length; i++) append(ops, value[i]); value = ops; }
      define(out, name, {value, writable: true, enumerable: true, configurable: true});
    }
    return out;
  }

  /* ---- Normalize an algorithm (§18.4) ---- */
  /* Dictionary members, inherited ones first, then in lexicographic order. */
  const dictionaries = {
    Algorithm: [],
    AesCtrParams: [['counter', 'buffer', true], ['length', 'octet', true]],
    AesCbcParams: [['iv', 'buffer', true]],
    AesGcmParams: [['additionalData', 'buffer', false], ['iv', 'buffer', true], ['tagLength', 'octet', false]],
    RsaOaepParams: [['label', 'buffer', false]],
    RsaPssParams: [['saltLength', 'ulong', true]],
    EcdsaParams: [['hash', 'hash', true]],
    RsaHashedKeyGenParams: [['modulusLength', 'ulong', true], ['publicExponent', 'bigint', true], ['hash', 'hash', true]],
    RsaHashedImportParams: [['hash', 'hash', true]],
    EcKeyGenParams: [['namedCurve', 'string', true]],
    EcKeyImportParams: [['namedCurve', 'string', true]],
    EcdhKeyDeriveParams: [['public', 'key', true]],
    AesKeyGenParams: [['length', 'ushort', true]],
    AesDerivedKeyParams: [['length', 'ushort', true]],
    HmacKeyGenParams: [['hash', 'hash', true], ['length', 'ulong', false]],
    HmacImportParams: [['hash', 'hash', true], ['length', 'ulong', false]],
    HkdfParams: [['hash', 'hash', true], ['info', 'buffer', true], ['salt', 'buffer', true]],
    Pbkdf2Params: [['hash', 'hash', true], ['iterations', 'ulong', true], ['salt', 'buffer', true]]
  };
  const aesNames = ['AES-CTR', 'AES-CBC', 'AES-GCM', 'AES-KW'], rsaNames = ['RSASSA-PKCS1-v1_5', 'RSA-PSS', 'RSA-OAEP'];
  const table = entries => { const t = create(null); for (const [name, type] of entries) t[name] = type; return t; };
  const cipher = [['RSA-OAEP', 'RsaOaepParams'], ['AES-CTR', 'AesCtrParams'], ['AES-CBC', 'AesCbcParams'], ['AES-GCM', 'AesGcmParams']];
  const signature = [['RSASSA-PKCS1-v1_5', 'Algorithm'], ['RSA-PSS', 'RsaPssParams'], ['ECDSA', 'EcdsaParams'],
    ['HMAC', 'Algorithm'], ['Ed25519', 'Algorithm']];
  const supportedAlgorithms = {
    encrypt: table(cipher), decrypt: table(cipher), sign: table(signature), verify: table(signature),
    digest: table([['SHA-1', 'Algorithm'], ['SHA-256', 'Algorithm'], ['SHA-384', 'Algorithm'], ['SHA-512', 'Algorithm']]),
    generateKey: table([...rsaNames.map(n => [n, 'RsaHashedKeyGenParams']), ['ECDSA', 'EcKeyGenParams'], ['ECDH', 'EcKeyGenParams'],
      ...aesNames.map(n => [n, 'AesKeyGenParams']), ['HMAC', 'HmacKeyGenParams'], ['Ed25519', 'Algorithm'], ['X25519', 'Algorithm']]),
    deriveBits: table([['ECDH', 'EcdhKeyDeriveParams'], ['HKDF', 'HkdfParams'], ['PBKDF2', 'Pbkdf2Params'], ['X25519', 'EcdhKeyDeriveParams']]),
    importKey: table([...rsaNames.map(n => [n, 'RsaHashedImportParams']), ['ECDSA', 'EcKeyImportParams'], ['ECDH', 'EcKeyImportParams'],
      ...aesNames.map(n => [n, 'Algorithm']), ['HMAC', 'HmacImportParams'], ['HKDF', 'Algorithm'], ['PBKDF2', 'Algorithm'],
      ['Ed25519', 'Algorithm'], ['X25519', 'Algorithm']]),
    wrapKey: table([['AES-KW', 'Algorithm']]), unwrapKey: table([['AES-KW', 'Algorithm']]),
    'get key length': table([...aesNames.map(n => [n, 'AesDerivedKeyParams']), ['HMAC', 'HmacImportParams'], ['HKDF', 'Algorithm'], ['PBKDF2', 'Algorithm']])
  };
  function asciiLower(value) {
    let out = '';
    for (let i = 0; i < value.length; i++) {
      const c = apply(charCode, value, [i]);
      out += fromCharCode(c >= 65 && c <= 90 ? c + 32 : c);
    }
    return out;
  }
  function convertMember(value, kind, member) {
    switch (kind) {
      case 'buffer': return bufferSource(value, member);
      case 'octet': return enforceRange(value, 255, member);
      case 'ushort': return enforceRange(value, 65535, member);
      case 'ulong': return enforceRange(value, 4294967295, member);
      case 'string': return domString(value);
      case 'key': return cryptoKey(value, member);
      case 'hash': return isObject(value) ? value : domString(value);
      case 'bigint':
        if (!isView(value) || apply(taTag, value, []) !== 'Uint8Array') throw new TypeErr(member + ' is not a Uint8Array');
        bufferView(value);
        return value;
    }
  }
  function normalizeAlgorithm(alg, op) {
    if (!isObject(alg)) alg = {name: domString(alg)};
    const registered = supportedAlgorithms[op];
    let name = alg.name;
    if (name === undefined) throw new TypeErr('Algorithm.name is required');
    name = domString(name);
    let registeredName;
    const wanted = asciiLower(name);
    for (const key in registered) if (asciiLower(key) === wanted) { registeredName = key; break; }
    if (registeredName === undefined) throw error('NotSupportedError', `Unrecognized algorithm name '${name}' for ${op}`);
    const members = dictionaries[registered[registeredName]], normalized = create(null);
    // Step 6 converts alg to desiredType, which reads name a second time; WPT (digest/,
    // sign_verify/ "transferred ... during call") and the engines read it once, so the
    // conversion here takes only desiredType's own members and keeps initialAlg's name.
    for (let i = 0; i < members.length; i++) {
      const [member, kind, required] = members[i], value = alg[member];
      if (value === undefined) { if (required) throw new TypeErr(`${member} is required by ${registeredName}`); continue; }
      normalized[member] = convertMember(value, kind, member);
    }
    normalized.name = registeredName;
    for (let i = 0; i < members.length; i++) {
      const [member, kind] = members[i];
      if (normalized[member] === undefined) continue;
      if (kind === 'buffer') normalized[member] = snapshot(normalized[member]);
      else if (kind === 'hash') normalized[member] = normalizeAlgorithm(normalized[member], 'digest');
      else if (kind === 'bigint') normalized[member] = snapshot(normalized[member]);
    }
    return normalized;
  }

  /* ---- CryptoKey ---- */
  function keySlot(value) {
    const slot = apply(keyGet, keySlots, [value]);
    if (!slot) throw new TypeErr('Expected a CryptoKey');
    return slot;
  }
  class CryptoKey {
    constructor() { throw new TypeErr('Illegal constructor'); }
    get type() { return keySlot(this).type; }
    get extractable() { return keySlot(this).extractable; }
    get algorithm() {
      const slot = keySlot(this);
      if (!slot.algorithmCache) slot.algorithmCache = algorithmObject(slot.algorithm);
      return slot.algorithmCache;
    }
    get usages() {
      const slot = keySlot(this);
      if (!slot.usagesCache) { const list = []; for (let i = 0; i < slot.usages.length; i++) append(list, slot.usages[i]); slot.usagesCache = list; }
      return slot.usagesCache;
    }
  }
  /* A KeyAlgorithm dictionary (and its derived dictionaries) as an ECMAScript object. */
  function algorithmObject(algorithm) {
    const out = {}, put = (name, value) => define(out, name, {value, writable: true, enumerable: true, configurable: true});
    put('name', algorithm.name);
    if (algorithm.modulusLength !== undefined) { put('modulusLength', algorithm.modulusLength); put('publicExponent', copy(algorithm.publicExponent)); }
    if (algorithm.namedCurve !== undefined) put('namedCurve', algorithm.namedCurve);
    if (algorithm.hash !== undefined) put('hash', algorithmObject(algorithm.hash));
    if (algorithm.length !== undefined) put('length', algorithm.length);
    return out;
  }
  /* "The usage intersection": recognized usages in both, in their listed order, once. */
  function intersection(usages, allowed) {
    const out = [];
    for (let i = 0; i < keyUsages.length; i++) if (includes(usages, keyUsages[i]) && includes(allowed, keyUsages[i])) append(out, keyUsages[i]);
    return out;
  }
  const normalizedUsages = usages => intersection(usages, keyUsages);
  function checkUsages(usages, allowed) {
    for (let i = 0; i < usages.length; i++)
      if (!includes(allowed, usages[i])) throw error('SyntaxError', `Usage '${usages[i]}' is not valid for this key`);
  }
  function makeKey(type, extractable, algorithm, usages, handle) {
    const key = create(CryptoKey.prototype);
    apply(keySet, keySlots, [key, {type, extractable: !!extractable, algorithm, usages: normalizedUsages(usages), handle,
      algorithmCache: null, usagesCache: null}]);
    return key;
  }
  /* JWK import checks shared by every key type (use, key_ops, ext). */
  function jwkChecks(jwk, usages, extractable, use) {
    if (usages.length && jwk.use !== undefined && jwk.use !== use) throw dataError(`JWK use is not '${use}'`);
    if (jwk.key_ops !== undefined) {
      const ops = jwk.key_ops;
      for (let i = 0; i < ops.length; i++) for (let j = i + 1; j < ops.length; j++)
        if (ops[i] === ops[j]) throw dataError('JWK key_ops has a duplicate');
      for (let i = 0; i < usages.length; i++) if (!includes(ops, usages[i])) throw dataError('JWK key_ops does not allow every usage');
    }
    if (jwk.ext === false && extractable) throw dataError('JWK is not extractable');
  }
  function jwkMember(jwk, name) {
    if (jwk[name] === undefined) throw dataError(`JWK member '${name}' is required`);
    return fromBase64url(jwk[name]);
  }
  function exportedJwk(key, members) {
    const slot = keySlot(key), jwk = create(null);
    const names = objectKeys(members);
    for (let i = 0; i < names.length; i++) jwk[names[i]] = members[names[i]];
    jwk.key_ops = slot.usages; jwk.ext = slot.extractable;
    return jwk;
  }
  const isRaw = format => format === 'raw' || format === 'raw-secret';
  const operation = message => error('OperationError', message);
  /* A native primitive's failure is the operation's OperationError. */
  function primitive(fn, args, message) {
    try { return apply(fn, native, args); } catch (_) { throw operation(message); }
  }
  const hashSize = name => name === 'SHA-1' ? 20 : name === 'SHA-256' ? 32 : name === 'SHA-384' ? 48 : 64;
  const blockSize = name => name === 'SHA-384' || name === 'SHA-512' ? 1024 : 512;
  const hashSuffix = name => name === 'SHA-1' ? '1' : name === 'SHA-256' ? '256' : name === 'SHA-384' ? '384' : '512';

  /* ---- Registered algorithms ---- */
  const algorithms = create(null);

  /* SHA (§30) */
  for (const name of ['SHA-1', 'SHA-256', 'SHA-384', 'SHA-512'])
    algorithms[name] = {digest: (normalized, data) => fromNative(primitive(native.digest, [name, buffer(data)], 'Digest failed'))};

  /* AES (§26-29) */
  const aesUsages = {'AES-CTR': ['encrypt', 'decrypt', 'wrapKey', 'unwrapKey'], 'AES-CBC': ['encrypt', 'decrypt', 'wrapKey', 'unwrapKey'],
    'AES-GCM': ['encrypt', 'decrypt', 'wrapKey', 'unwrapKey'], 'AES-KW': ['wrapKey', 'unwrapKey']};
  const aesSuffix = {'AES-CTR': 'CTR', 'AES-CBC': 'CBC', 'AES-GCM': 'GCM', 'AES-KW': 'KW'};
  const aesMode = {'AES-CBC': 1, 'AES-CTR': 2, 'AES-GCM': 3, 'AES-KW': 4};
  function aesCipher(name, decrypt) {
    return (normalized, key, data) => {
      const bytes = keySlot(key).handle.bytes;
      let iv = new U8(0), bits = 128, aad = new U8(0);
      if (name === 'AES-CTR') {
        if (byteLength(normalized.counter) !== 16) throw operation('The AES-CTR counter must be 16 bytes');
        if (normalized.length === 0 || normalized.length > 128) throw operation('The AES-CTR counter length must be between 1 and 128');
        iv = normalized.counter; bits = normalized.length;
      } else if (name === 'AES-CBC') {
        if (byteLength(normalized.iv) !== 16) throw operation('The AES-CBC iv must be 16 bytes');
        iv = normalized.iv;
      } else {
        if (!decrypt && byteLength(data) > 549755813632) throw operation('The AES-GCM plaintext is too long');
        iv = normalized.iv;
        if (normalized.additionalData !== undefined) aad = normalized.additionalData;
        if (normalized.tagLength !== undefined) {
          bits = normalized.tagLength;
          if (!includes([32, 64, 96, 104, 112, 120, 128], bits)) throw operation('Invalid AES-GCM tag length');
        }
        if (decrypt && byteLength(data) < bits / 8) throw operation('The AES-GCM ciphertext is shorter than its tag');
      }
      return fromNative(primitive(native.aes, [aesMode[name], decrypt ? 1 : 0, buffer(bytes), buffer(iv), buffer(data), bits, buffer(aad)],
        `The ${name} operation failed`));
    };
  }
  for (const name of aesNames) {
    const usages = aesUsages[name];
    algorithms[name] = {
      generateKey(normalized, extractable, keyUsages) {
        checkUsages(keyUsages, usages);
        if (!includes([128, 192, 256], normalized.length)) throw operation('AES key length must be 128, 192 or 256');
        return makeKey('secret', extractable, {name, length: normalized.length}, keyUsages, {bytes: randomBytes(normalized.length / 8)});
      },
      importKey(format, keyData, normalized, extractable, keyUsages) {
        checkUsages(keyUsages, usages);
        let data;
        if (isRaw(format)) {
          data = keyData;
          if (!includes([128, 192, 256], byteLength(data) * 8)) throw dataError('AES key length must be 128, 192 or 256');
        } else if (format === 'jwk') {
          const jwk = keyData;
          if (jwk.kty !== 'oct') throw dataError('JWK kty must be oct');
          data = jwkMember(jwk, 'k');
          const bits = byteLength(data) * 8;
          if (!includes([128, 192, 256], bits)) throw dataError('AES key length must be 128, 192 or 256');
          if (jwk.alg !== undefined && jwk.alg !== 'A' + bits + aesSuffix[name]) throw dataError('JWK alg does not match');
          jwkChecks(jwk, keyUsages, extractable, 'enc');
        } else throw error('NotSupportedError', `${name} keys have no ${format} format`);
        return makeKey('secret', extractable, {name, length: byteLength(data) * 8}, keyUsages, {bytes: copy(data)});
      },
      exportKey(format, key) {
        const bytes = keySlot(key).handle.bytes;
        if (isRaw(format)) return copy(bytes);
        if (format === 'jwk') return exportedJwk(key, {kty: 'oct', k: base64url(bytes), alg: 'A' + byteLength(bytes) * 8 + aesSuffix[name]});
        throw error('NotSupportedError', `${name} keys have no ${format} format`);
      },
      getKeyLength(normalized) {
        if (!includes([128, 192, 256], normalized.length)) throw operation('AES key length must be 128, 192 or 256');
        return normalized.length;
      }
    };
    if (name === 'AES-KW') {
      const wrap = unwrap => (normalized, key, data) => {
        if (byteLength(data) % 8) throw operation('AES-KW data must be a multiple of 64 bits');
        return fromNative(primitive(native.aes, [4, unwrap ? 1 : 0, buffer(keySlot(key).handle.bytes), new AB(0), buffer(data), 0, new AB(0)],
          `AES-KW ${unwrap ? 'unwrap' : 'wrap'} failed`));
      };
      algorithms[name].wrapKey = wrap(false); algorithms[name].unwrapKey = wrap(true);
    } else { algorithms[name].encrypt = aesCipher(name, false); algorithms[name].decrypt = aesCipher(name, true); }
  }

  /* HMAC (§31) */
  algorithms.HMAC = {
    sign: (normalized, key, data) => {
      const slot = keySlot(key);
      return fromNative(primitive(native.hmac, [slot.algorithm.hash.name, buffer(slot.handle.bytes), buffer(data)], 'HMAC failed'));
    },
    verify: (normalized, key, signature, data) => {
      const slot = keySlot(key);
      return equalBytes(fromNative(primitive(native.hmac, [slot.algorithm.hash.name, buffer(slot.handle.bytes), buffer(data)], 'HMAC failed')), signature);
    },
    generateKey(normalized, extractable, keyUsages) {
      checkUsages(keyUsages, ['sign', 'verify']);
      let length;
      if (normalized.length === undefined) length = blockSize(normalized.hash.name);
      else if (normalized.length !== 0) length = normalized.length;
      else throw operation('HMAC key length must not be zero');
      const bytes = randomBytes((length + 7) >> 3);
      if (length & 7) bytes[byteLength(bytes) - 1] &= (0xff << (8 - (length & 7))) & 0xff;
      return makeKey('secret', extractable, {name: 'HMAC', hash: {name: normalized.hash.name}, length}, keyUsages, {bytes});
    },
    importKey(format, keyData, normalized, extractable, keyUsages) {
      checkUsages(keyUsages, ['sign', 'verify']);
      const hash = normalized.hash.name;
      let data;
      if (isRaw(format)) data = keyData;
      else if (format === 'jwk') {
        const jwk = keyData;
        if (jwk.kty !== 'oct') throw dataError('JWK kty must be oct');
        data = jwkMember(jwk, 'k');
        if (jwk.alg !== undefined && jwk.alg !== 'HS' + hashSuffix(hash)) throw dataError('JWK alg does not match');
        jwkChecks(jwk, keyUsages, extractable, 'sig');
      } else throw error('NotSupportedError', `HMAC keys have no ${format} format`);
      let length = byteLength(data) * 8;
      if (length === 0) throw dataError('HMAC key is empty');
      if (normalized.length !== undefined) {
        if (normalized.length > length || normalized.length <= length - 8) throw dataError('HMAC length does not match the key data');
        length = normalized.length;
      }
      return makeKey('secret', extractable, {name: 'HMAC', hash: {name: hash}, length}, keyUsages, {bytes: copy(data)});
    },
    exportKey(format, key) {
      const slot = keySlot(key), bytes = slot.handle.bytes;
      if (isRaw(format)) return copy(bytes);
      if (format === 'jwk') return exportedJwk(key, {kty: 'oct', k: base64url(bytes), alg: 'HS' + hashSuffix(slot.algorithm.hash.name)});
      throw error('NotSupportedError', `HMAC keys have no ${format} format`);
    },
    getKeyLength(normalized) {
      if (normalized.length === undefined) return blockSize(normalized.hash.name);
      if (normalized.length !== 0) return normalized.length;
      throw new TypeErr('HMAC key length must not be zero');
    }
  };

  /* HKDF (§32) and PBKDF2 (§33): secret keys that only derive. */
  function kdfImport(name) {
    return (format, keyData, normalized, extractable, keyUsages) => {
      if (!isRaw(format)) throw error('NotSupportedError', `${name} keys have only the raw format`);
      checkUsages(keyUsages, ['deriveKey', 'deriveBits']);
      if (extractable) throw error('SyntaxError', `${name} keys are not extractable`);
      return makeKey('secret', false, {name}, keyUsages, {bytes: copy(keyData)});
    };
  }
  algorithms.HKDF = {
    importKey: kdfImport('HKDF'), getKeyLength: () => null,
    deriveBits(normalized, key, length) {
      if (length === null || length % 8) throw operation('HKDF length must be a multiple of 8');
      return fromNative(primitive(native.hkdf, [normalized.hash.name, buffer(keySlot(key).handle.bytes), buffer(normalized.salt),
        buffer(normalized.info), length / 8], 'HKDF failed'));
    }
  };
  algorithms.PBKDF2 = {
    importKey: kdfImport('PBKDF2'), getKeyLength: () => null,
    deriveBits(normalized, key, length) {
      if (length === null || length % 8) throw operation('PBKDF2 length must be a multiple of 8');
      if (normalized.iterations === 0) throw operation('PBKDF2 iterations must not be zero');
      const hash = normalized.hash.name, size = hashSize(hash), password = buffer(keySlot(key).handle.bytes);
      const out = new U8(length / 8);
      for (let block = 1, at = 0; at < byteLength(out); block++, at += size) {
        const index = new U8([block >>> 24, (block >>> 16) & 255, (block >>> 8) & 255, block & 255]);
        let u = primitive(native.hmac, [hash, password, buffer(concat(normalized.salt, index))], 'PBKDF2 failed'), t = u;
        // RFC 8018 §5.2 F: bounded native rounds, so the realm's interrupt checks run between them.
        for (let left = normalized.iterations - 1; left > 0;) {
          const count = left > 4096 ? 4096 : left;
          const state = primitive(native.pbkdf2, [hash, password, u, t, count], 'PBKDF2 failed');
          u = state[0]; t = state[1]; left -= count;
        }
        const bytes = new U8(t);
        apply(u8Set, out, [sub(bytes, 0, byteLength(out) - at < size ? byteLength(out) - at : size), at]);
      }
      return out;
    }
  };

  /* RSA (§23-25) */
  const rsaSigning = {public: ['verify'], private: ['sign'], all: ['verify', 'sign']};
  const rsaCipher = {public: ['encrypt', 'wrapKey'], private: ['decrypt', 'unwrapKey'], all: ['encrypt', 'wrapKey', 'decrypt', 'unwrapKey']};
  const rsaMembers = ['n', 'e', 'd', 'p', 'q', 'dp', 'dq', 'qi'];
  function rsaJwkAlg(name, hash) {
    const h = hashSuffix(hash);
    return name === 'RSASSA-PKCS1-v1_5' ? 'RS' + h : name === 'RSA-PSS' ? 'PS' + h : hash === 'SHA-1' ? 'RSA-OAEP' : 'RSA-OAEP-' + h;
  }
  function rsaAlgorithm(name, handle, hash) {
    return {name, modulusLength: bitLength(handle.n), publicExponent: copy(handle.e), hash: {name: hash}};
  }
  /* RFC 8017 §3.2: n = p q, d e = 1 modulo the factors' p - 1 and q - 1 (through dp and dq),
     dp and dq the reductions of d, and qi the inverse of q modulo p. */
  function rsaPrivateValid(h) {
    const n = bigFrom(h.n), e = bigFrom(h.e), d = bigFrom(h.d), p = bigFrom(h.p), q = bigFrom(h.q);
    const dp = bigFrom(h.dp), dq = bigFrom(h.dq), qi = bigFrom(h.qi);
    if (p < 3n || q < 3n || e < 3n || p * q !== n || bitLength(h.n) > 4096) return false;
    const p1 = p - 1n, q1 = q - 1n;
    return dp === d % p1 && dq === d % q1 && e * dp % p1 === 1n && e * dq % q1 === 1n && qi < p && qi * q % p === 1n;
  }
  function rsaPublicKey(bytes) {
    const parts = derChildren(bytes, derTop(bytes, 0x30));
    if (parts.length !== 2) throw dataError('Invalid RSAPublicKey');
    const n = derUnsigned(bytes, parts[0]), e = derUnsigned(bytes, parts[1]);
    if (!bitLength(n) || !bitLength(e)) throw dataError('Invalid RSAPublicKey');
    return {n, e};
  }
  function rsaPrivateKey(bytes) {
    const parts = derChildren(bytes, derTop(bytes, 0x30));
    if (parts.length !== 9 || derSmall(bytes, parts[0]) !== 0) throw dataError('Unsupported RSAPrivateKey');
    const h = {};
    for (let i = 0; i < 8; i++) h[rsaMembers[i]] = derUnsigned(bytes, parts[i + 1]);
    return h;
  }
  const rsaIdentifier = () => der(0x30, OID.rsaEncryption, derNull);
  function rsaAlgorithmIdentifier(info) {
    const {oid: node, params} = info.algorithm;
    if (!isOID(info.bytes, node, OID.rsaEncryption)) throw dataError('Not an rsaEncryption key');
    if (params && (params.tag !== 0x05 || params.start !== params.end)) throw dataError('Invalid rsaEncryption parameters');
  }
  for (const name of rsaNames) {
    const usage = name === 'RSA-OAEP' ? rsaCipher : rsaSigning, use = name === 'RSA-OAEP' ? 'enc' : 'sig';
    const entry = algorithms[name] = {
      generateKey(normalized, extractable, keyUsages) {
        checkUsages(keyUsages, usage.all);
        const job = primitive(native.rsaGenerate, [normalized.modulusLength, buffer(normalized.publicExponent)], 'RSA key generation failed');
        return background(job, () => {
          const parts = primitive(native.rsaGenerated, [job], 'RSA key generation failed');
          const handle = {};
          for (let i = 0; i < 8; i++) handle[rsaMembers[i]] = trimmed(new U8(parts[i]));
          const algorithm = rsaAlgorithm(name, handle, normalized.hash.name);
          const privateKey = makeKey('private', extractable, algorithm, intersection(keyUsages, usage.private), handle);
          const publicKey = makeKey('public', true, rsaAlgorithm(name, handle, normalized.hash.name),
            intersection(keyUsages, usage.public), {n: handle.n, e: handle.e});
          return {privateKey, publicKey};
        });
      },
      importKey(format, keyData, normalized, extractable, keyUsages) {
        const hash = normalized.hash.name;
        let handle, type;
        if (format === 'spki') {
          checkUsages(keyUsages, usage.public);
          const info = parseSPKI(keyData);
          rsaAlgorithmIdentifier(info);
          handle = rsaPublicKey(info.key); type = 'public';
        } else if (format === 'pkcs8') {
          checkUsages(keyUsages, usage.private);
          const info = parsePKCS8(keyData);
          rsaAlgorithmIdentifier(info);
          handle = rsaPrivateKey(info.key); type = 'private';
          if (!rsaPrivateValid(handle)) throw dataError('Invalid RSA private key');
        } else if (format === 'jwk') {
          const jwk = keyData;
          checkUsages(keyUsages, jwk.d !== undefined ? usage.private : usage.public);
          if (jwk.kty !== 'RSA') throw dataError('JWK kty must be RSA');
          jwkChecks(jwk, keyUsages, extractable, use);
          if (jwk.alg !== undefined && jwk.alg !== rsaJwkAlg(name, hash)) throw dataError('JWK alg does not match');
          handle = {n: trimmed(jwkMember(jwk, 'n')), e: trimmed(jwkMember(jwk, 'e'))};
          if (!bitLength(handle.n) || !bitLength(handle.e)) throw dataError('Invalid RSA public key');
          type = 'public';
          if (jwk.d !== undefined) {
            if (jwk.oth !== undefined) throw dataError('Multi-prime RSA keys are not supported');
            for (let i = 2; i < 8; i++) handle[rsaMembers[i]] = trimmed(jwkMember(jwk, rsaMembers[i]));
            if (!rsaPrivateValid(handle)) throw dataError('Invalid RSA private key');
            type = 'private';
          }
        } else throw error('NotSupportedError', `${name} keys have no ${format} format`);
        return makeKey(type, extractable, rsaAlgorithm(name, handle, hash), keyUsages, handle);
      },
      exportKey(format, key) {
        const slot = keySlot(key), h = slot.handle;
        if (format === 'spki') {
          if (slot.type !== 'public') throw error('InvalidAccessError', 'Only a public key has the spki format');
          return spki(rsaIdentifier(), der(0x30, derInteger(h.n), derInteger(h.e)));
        }
        if (format === 'pkcs8') {
          if (slot.type !== 'private') throw error('InvalidAccessError', 'Only a private key has the pkcs8 format');
          const parts = [0x30, derInteger(new U8(1))];
          for (let i = 0; i < 8; i++) append(parts, derInteger(h[rsaMembers[i]]));
          return pkcs8(rsaIdentifier(), apply(der, null, parts));
        }
        if (format === 'jwk') {
          const members = {kty: 'RSA', alg: rsaJwkAlg(name, slot.algorithm.hash.name)};
          const count = slot.type === 'private' ? 8 : 2;
          for (let i = 0; i < count; i++) members[rsaMembers[i]] = base64url(h[rsaMembers[i]]);
          return exportedJwk(key, members);
        }
        throw error('NotSupportedError', `${name} keys have no ${format} format`);
      }
    };
    const privateCall = (op, hash, param, data, h) => [op, hash, param, data, buffer(h.n), buffer(h.p), buffer(h.q),
      buffer(h.dp), buffer(h.dq), buffer(h.qi)];
    if (name === 'RSA-OAEP') {
      entry.encrypt = (normalized, key, data) => {
        const slot = keySlot(key);
        if (slot.type !== 'public') throw error('InvalidAccessError', 'RSA-OAEP encrypts with a public key');
        const label = normalized.label !== undefined ? normalized.label : new U8(0);
        return fromNative(primitive(native.rsaPublic, [2, slot.algorithm.hash.name, buffer(label), buffer(data), new AB(0),
          buffer(slot.handle.n), buffer(slot.handle.e)], 'RSA-OAEP encryption failed'));
      };
      entry.decrypt = (normalized, key, data) => {
        const slot = keySlot(key);
        if (slot.type !== 'private') throw error('InvalidAccessError', 'RSA-OAEP decrypts with a private key');
        const label = normalized.label !== undefined ? normalized.label : new U8(0);
        return fromNative(primitive(native.rsaPrivate, privateCall(2, slot.algorithm.hash.name, buffer(label), buffer(data), slot.handle),
          'RSA-OAEP decryption failed'));
      };
    } else {
      const scheme = name === 'RSA-PSS' ? 1 : 0;
      entry.sign = (normalized, key, data) => {
        const slot = keySlot(key);
        if (slot.type !== 'private') throw error('InvalidAccessError', `${name} signs with a private key`);
        return fromNative(primitive(native.rsaPrivate, privateCall(scheme, slot.algorithm.hash.name, scheme ? normalized.saltLength : 0,
          buffer(data), slot.handle), `${name} signing failed`));
      };
      entry.verify = (normalized, key, signature, data) => {
        const slot = keySlot(key);
        if (slot.type !== 'public') throw error('InvalidAccessError', `${name} verifies with a public key`);
        return primitive(native.rsaPublic, [scheme, slot.algorithm.hash.name, scheme ? normalized.saltLength : 0, buffer(data), buffer(signature),
          buffer(slot.handle.n), buffer(slot.handle.e)], `${name} verification failed`) === true;
      };
    }
  }

  /* ECDSA (§19) and ECDH (§20) over P-256, P-384 and P-521. */
  const curves = create(null);
  curves['P-256'] = {id: 23, size: 32, p: 2n ** 256n - 2n ** 224n + 2n ** 192n + 2n ** 96n - 1n,
    b: 0x5ac635d8aa3a93e7b3ebbd55769886bc651d06b0cc53b0f63bce3c3e27d2604bn};
  curves['P-384'] = {id: 24, size: 48, p: 2n ** 384n - 2n ** 128n - 2n ** 96n + 2n ** 32n - 1n,
    b: 0xb3312fa7e23ee7e4988e056be3f82d19181d9c6efe8141120314088f5013875ac656398d8a2ed19d2a85c8edd3ec2aefn};
  curves['P-521'] = {id: 25, size: 66, p: 2n ** 521n - 1n,
    b: 0x0051953eb9618e1c9a1f929a21a0b68540eea2da725b99b315f3b8b489918ef109e156193951ec7e937b1652c0bd3bb1bf073573df883d2c34f1ef451fd46b503f00n};
  function bigFrom(bytes) { let v = 0n; for (let i = 0; i < byteLength(bytes); i++) v = (v << 8n) | BigIntType(bytes[i]); return v; }
  function bigTo(value, size) { const out = new U8(size); for (let i = size - 1; i >= 0; i--) { out[i] = Number(value & 255n); value >>= 8n; } return out; }
  function modPow(base, exponent, modulus) {
    let result = 1n; base %= modulus;
    while (exponent > 0n) { if (exponent & 1n) result = result * base % modulus; base = base * base % modulus; exponent >>= 1n; }
    return result;
  }
  /* SEC 1 §2.3.4: an uncompressed point, or a compressed one (y = sqrt(x^3 - 3x + b), p = 3 mod 4). */
  function curvePoint(curve, bytes) {
    const c = curves[curve], size = c.size;
    let point;
    if (byteLength(bytes) === 1 + 2 * size && bytes[0] === 4) point = copy(bytes);
    else if (byteLength(bytes) === 1 + size && (bytes[0] === 2 || bytes[0] === 3)) {
      const x = bigFrom(sub(bytes, 1, byteLength(bytes)));
      if (x >= c.p) throw dataError('Invalid compressed point');
      const rhs = ((x * x % c.p * x - 3n * x + c.b) % c.p + c.p) % c.p;
      let y = modPow(rhs, (c.p + 1n) / 4n, c.p);
      if (y * y % c.p !== rhs) throw dataError('Invalid compressed point');
      if ((y & 1n) !== BigIntType(bytes[0] & 1)) y = c.p - y;
      point = concat(new U8([4]), sub(bytes, 1, byteLength(bytes)), bigTo(y, size));
    } else throw dataError('Invalid point encoding');
    if (native.ecValid(c.id, buffer(point)) !== true) throw dataError('The point is not on the curve');
    return point;
  }
  function curveOf(info) {
    const params = info.algorithm.params, bytes = info.bytes;
    if (!params || params.tag !== 0x06) throw dataError('EC keys need a named curve');
    for (const name in curves) if (isOID(bytes, params, OID[name])) return name;
    throw dataError('Unsupported named curve');
  }
  function ecPublicFrom(curve, d) {
    const q = native.ecPublic(curves[curve].id, buffer(d));
    if (q === null) throw dataError('Invalid EC private key');
    return new U8(q);
  }
  function ecPrivateKey(curve, bytes, info) {
    const parts = derChildren(bytes, derTop(bytes, 0x30));
    if (parts.length < 2 || parts.length > 4 || derSmall(bytes, parts[0]) !== 1 || parts[1].tag !== 0x04)
      throw dataError('Invalid ECPrivateKey');
    const d = padded(contents(bytes, parts[1]), curves[curve].size);
    if (!d) throw dataError('Invalid EC private key length');
    let at = 2, embedded = null;
    if (at < parts.length && parts[at].tag === 0xa0) {
      const inner = derChildren(bytes, parts[at]);
      if (inner.length !== 1 || !isOID(bytes, inner[0], OID[curve])) throw dataError('ECPrivateKey parameters do not match');
      at++;
    }
    if (at < parts.length && parts[at].tag === 0xa1) {
      const inner = derChildren(bytes, parts[at]);
      if (inner.length !== 1) throw dataError('Invalid ECPrivateKey public key');
      embedded = derBits(bytes, inner[0]);
      at++;
    }
    if (at !== parts.length) throw dataError('Invalid ECPrivateKey');
    const q = ecPublicFrom(curve, d);
    if (embedded && !equalBytes(curvePoint(curve, embedded), q)) throw dataError('ECPrivateKey public key does not match');
    return {curve, d, q};
  }
  const ecIdentifier = curve => der(0x30, OID.ecPublicKey, OID[curve]);
  for (const name of ['ECDSA', 'ECDH']) {
    const signing = name === 'ECDSA';
    const usage = signing ? {public: ['verify'], private: ['sign'], all: ['verify', 'sign']} :
      {public: [], private: ['deriveKey', 'deriveBits'], all: ['deriveKey', 'deriveBits']};
    const supported = curve => curve in curves;
    const entry = algorithms[name] = {
      generateKey(normalized, extractable, keyUsages) {
        checkUsages(keyUsages, usage.all);
        const curve = normalized.namedCurve;
        if (!supported(curve)) throw error('NotSupportedError', `Unsupported named curve '${curve}'`);
        const pair = primitive(native.ecGenerate, [curves[curve].id], 'EC key generation failed');
        const d = new U8(pair[0]), q = new U8(pair[1]);
        const privateKey = makeKey('private', extractable, {name, namedCurve: curve}, intersection(keyUsages, usage.private), {curve, d, q});
        const publicKey = makeKey('public', true, {name, namedCurve: curve}, intersection(keyUsages, usage.public), {curve, q});
        return {privateKey, publicKey};
      },
      importKey(format, keyData, normalized, extractable, keyUsages) {
        const wanted = normalized.namedCurve;
        let handle, type;
        const keyOID = info => {
          const node = info.algorithm.oid;
          if (!isOID(info.bytes, node, OID.ecPublicKey) && (signing || !isOID(info.bytes, node, OID.ecDH))) throw dataError(`Not an ${name} key`);
        };
        if (format === 'spki') {
          checkUsages(keyUsages, usage.public);
          const info = parseSPKI(keyData);
          keyOID(info);
          const curve = curveOf(info);
          handle = {curve, q: curvePoint(curve, info.key)};
          if (curve !== wanted) throw dataError('The key is on another curve');
          type = 'public';
        } else if (format === 'pkcs8') {
          checkUsages(keyUsages, usage.private);
          const info = parsePKCS8(keyData);
          keyOID(info);
          const curve = curveOf(info);
          handle = ecPrivateKey(curve, info.key, info);
          if (curve !== wanted) throw dataError('The key is on another curve');
          type = 'private';
        } else if (format === 'jwk') {
          const jwk = keyData;
          checkUsages(keyUsages, jwk.d !== undefined ? usage.private : usage.public);
          if (jwk.kty !== 'EC') throw dataError('JWK kty must be EC');
          jwkChecks(jwk, keyUsages, extractable, signing ? 'sig' : 'enc');
          const curve = jwk.crv;
          if (curve !== wanted) throw dataError('JWK crv does not match the named curve');
          if (!supported(curve)) throw dataError(`Unsupported named curve '${curve}'`);
          if (signing && jwk.alg !== undefined) {
            const algCurve = jwk.alg === 'ES256' ? 'P-256' : jwk.alg === 'ES384' ? 'P-384' : jwk.alg === 'ES512' ? 'P-521' : null;
            if (algCurve !== curve) throw dataError('JWK alg does not match the named curve');
          }
          const size = curves[curve].size, x = jwkMember(jwk, 'x'), y = jwkMember(jwk, 'y');
          if (byteLength(x) !== size || byteLength(y) !== size) throw dataError('Invalid JWK coordinate length');
          const q = curvePoint(curve, concat(new U8([4]), x, y));
          handle = {curve, q}; type = 'public';
          if (jwk.d !== undefined) {
            const d = jwkMember(jwk, 'd');
            if (byteLength(d) !== size) throw dataError('Invalid JWK private key length');
            if (!equalBytes(ecPublicFrom(curve, d), q)) throw dataError('JWK private and public keys do not match');
            handle.d = d; type = 'private';
          }
        } else if (format === 'raw' || format === 'raw-public') {
          checkUsages(keyUsages, usage.public);
          if (!supported(wanted)) throw dataError(`Unsupported named curve '${wanted}'`);
          handle = {curve: wanted, q: curvePoint(wanted, keyData)}; type = 'public';
        } else throw error('NotSupportedError', `${name} keys have no ${format} format`);
        return makeKey(type, extractable, {name, namedCurve: handle.curve}, keyUsages, handle);
      },
      exportKey(format, key) {
        const slot = keySlot(key), h = slot.handle;
        if (format === 'spki') {
          if (slot.type !== 'public') throw error('InvalidAccessError', 'Only a public key has the spki format');
          return spki(ecIdentifier(h.curve), h.q);
        }
        if (format === 'pkcs8') {
          if (slot.type !== 'private') throw error('InvalidAccessError', 'Only a private key has the pkcs8 format');
          return pkcs8(ecIdentifier(h.curve), der(0x30, derInteger(new U8([1])), der(0x04, h.d), der(0xa1, der(0x03, new U8([0]), h.q))));
        }
        if (format === 'jwk') {
          const size = curves[h.curve].size, members = {kty: 'EC', crv: h.curve,
            x: base64url(sub(h.q, 1, 1 + size)), y: base64url(sub(h.q, 1 + size, 1 + 2 * size))};
          if (slot.type === 'private') members.d = base64url(h.d);
          return exportedJwk(key, members);
        }
        if (format === 'raw' || format === 'raw-public') {
          if (slot.type !== 'public') throw error('InvalidAccessError', 'Only a public key has the raw format');
          return copy(h.q);
        }
        throw error('NotSupportedError', `${name} keys have no ${format} format`);
      }
    };
    if (signing) {
      entry.sign = (normalized, key, data) => {
        const slot = keySlot(key);
        if (slot.type !== 'private') throw error('InvalidAccessError', 'ECDSA signs with a private key');
        return fromNative(primitive(native.ecdsa, [0, curves[slot.handle.curve].id, normalized.hash.name, buffer(slot.handle.d), buffer(data), new AB(0)],
          'ECDSA signing failed'));
      };
      entry.verify = (normalized, key, signature, data) => {
        const slot = keySlot(key);
        if (slot.type !== 'public') throw error('InvalidAccessError', 'ECDSA verifies with a public key');
        if (byteLength(signature) !== 2 * curves[slot.handle.curve].size) return false;
        return primitive(native.ecdsa, [1, curves[slot.handle.curve].id, normalized.hash.name, buffer(slot.handle.q), buffer(data), buffer(signature)],
          'ECDSA verification failed') === true;
      };
    } else {
      entry.deriveBits = (normalized, key, length) => {
        const slot = keySlot(key), other = keySlot(normalized.public);
        if (slot.type !== 'private') throw error('InvalidAccessError', 'ECDH derives with a private key');
        if (other.type !== 'public') throw error('InvalidAccessError', 'The ECDH public member is not a public key');
        if (other.algorithm.name !== slot.algorithm.name) throw error('InvalidAccessError', 'The public key is not an ECDH key');
        if (other.algorithm.namedCurve !== slot.algorithm.namedCurve) throw error('InvalidAccessError', 'The public key is on another curve');
        const secret = native.ecdh(curves[slot.handle.curve].id, buffer(slot.handle.d), buffer(other.handle.q));
        if (secret === null) throw operation('ECDH failed');
        const bytes = new U8(secret);
        if (length === null) return bytes;
        if (length > byteLength(bytes) * 8) throw operation('The ECDH secret is shorter than the length');
        return firstBits(bytes, length);
      };
    }
  }

  /* Ed25519 (§21) and X25519 (§22): RFC 8410 identifiers, RFC 8037 JWKs. */
  const basepoint = new U8(32); basepoint[0] = 9;
  const okpPublic = (name, priv) => name === 'Ed25519' ? new U8(native.ed25519(0, buffer(priv), new AB(0), new AB(0))) :
    new U8(native.x25519(buffer(priv), buffer(basepoint)) || new AB(32));
  for (const name of ['Ed25519', 'X25519']) {
    const signing = name === 'Ed25519';
    const usage = signing ? {public: ['verify'], private: ['sign'], all: ['verify', 'sign']} :
      {public: [], private: ['deriveKey', 'deriveBits'], all: ['deriveKey', 'deriveBits']};
    const keyOID = OID[name];
    const identifier = info => {
      if (!isOID(info.bytes, info.algorithm.oid, keyOID)) throw dataError(`Not an ${name} key`);
      if (info.algorithm.params) throw dataError(`${name} identifiers have no parameters`);
    };
    const entry = algorithms[name] = {
      generateKey(normalized, extractable, keyUsages) {
        checkUsages(keyUsages, usage.all);
        const priv = randomBytes(32), pub = okpPublic(name, priv);
        const privateKey = makeKey('private', extractable, {name}, intersection(keyUsages, usage.private), {priv, pub});
        const publicKey = makeKey('public', true, {name}, intersection(keyUsages, usage.public), {pub});
        return {privateKey, publicKey};
      },
      importKey(format, keyData, normalized, extractable, keyUsages) {
        let handle, type;
        if (format === 'spki') {
          checkUsages(keyUsages, usage.public);
          const info = parseSPKI(keyData);
          identifier(info);
          if (byteLength(info.key) !== 32) throw dataError(`${name} public keys are 32 bytes`);
          handle = {pub: info.key}; type = 'public';
        } else if (format === 'pkcs8') {
          checkUsages(keyUsages, usage.private);
          const info = parsePKCS8(keyData);
          identifier(info);
          const node = derTop(info.key, 0x04);
          const priv = copy(contents(info.key, node));
          if (byteLength(priv) !== 32) throw dataError(`${name} private keys are 32 bytes`);
          handle = {priv, pub: okpPublic(name, priv)}; type = 'private';
        } else if (format === 'jwk') {
          const jwk = keyData;
          checkUsages(keyUsages, jwk.d !== undefined ? usage.private : usage.public);
          if (jwk.kty !== 'OKP') throw dataError('JWK kty must be OKP');
          if (jwk.crv !== name) throw dataError(`JWK crv must be ${name}`);
          if (signing && jwk.alg !== undefined && jwk.alg !== 'Ed25519' && jwk.alg !== 'EdDSA') throw dataError('JWK alg does not match');
          jwkChecks(jwk, keyUsages, extractable, signing ? 'sig' : 'enc');
          const pub = jwkMember(jwk, 'x');
          if (byteLength(pub) !== 32) throw dataError(`${name} public keys are 32 bytes`);
          handle = {pub}; type = 'public';
          if (jwk.d !== undefined) {
            const priv = jwkMember(jwk, 'd');
            if (byteLength(priv) !== 32) throw dataError(`${name} private keys are 32 bytes`);
            if (!equalBytes(okpPublic(name, priv), pub)) throw dataError('JWK private and public keys do not match');
            handle.priv = priv; type = 'private';
          }
        } else if (format === 'raw' || format === 'raw-public') {
          checkUsages(keyUsages, usage.public);
          if (byteLength(keyData) !== 32) throw dataError(`${name} public keys are 32 bytes`);
          handle = {pub: copy(keyData)}; type = 'public';
        } else throw error('NotSupportedError', `${name} keys have no ${format} format`);
        return makeKey(type, extractable, {name}, keyUsages, handle);
      },
      exportKey(format, key) {
        const slot = keySlot(key), h = slot.handle;
        if (format === 'spki') {
          if (slot.type !== 'public') throw error('InvalidAccessError', 'Only a public key has the spki format');
          return spki(der(0x30, keyOID), h.pub);
        }
        if (format === 'pkcs8') {
          if (slot.type !== 'private') throw error('InvalidAccessError', 'Only a private key has the pkcs8 format');
          return pkcs8(der(0x30, keyOID), der(0x04, h.priv));
        }
        if (format === 'jwk') {
          const members = {kty: 'OKP', crv: name, x: base64url(h.pub)};
          if (signing) members.alg = 'Ed25519';
          if (slot.type === 'private') members.d = base64url(h.priv);
          return exportedJwk(key, members);
        }
        if (format === 'raw' || format === 'raw-public') {
          if (slot.type !== 'public') throw error('InvalidAccessError', 'Only a public key has the raw format');
          return copy(h.pub);
        }
        throw error('NotSupportedError', `${name} keys have no ${format} format`);
      }
    };
    if (signing) {
      entry.sign = (normalized, key, data) => {
        const slot = keySlot(key);
        if (slot.type !== 'private') throw error('InvalidAccessError', 'Ed25519 signs with a private key');
        return fromNative(primitive(native.ed25519, [1, buffer(slot.handle.priv), buffer(data), new AB(0)], 'Ed25519 signing failed'));
      };
      entry.verify = (normalized, key, signature, data) => {
        const slot = keySlot(key);
        if (slot.type !== 'public') throw error('InvalidAccessError', 'Ed25519 verifies with a public key');
        return primitive(native.ed25519, [2, buffer(slot.handle.pub), buffer(data), buffer(signature)], 'Ed25519 verification failed') === true;
      };
    } else {
      entry.deriveBits = (normalized, key, length) => {
        const slot = keySlot(key), other = keySlot(normalized.public);
        if (slot.type !== 'private') throw error('InvalidAccessError', 'X25519 derives with a private key');
        if (other.type !== 'public') throw error('InvalidAccessError', 'The X25519 public member is not a public key');
        if (other.algorithm.name !== slot.algorithm.name) throw error('InvalidAccessError', 'The public key is not an X25519 key');
        const secret = native.x25519(buffer(slot.handle.priv), buffer(other.handle.pub));
        if (secret === null) throw operation('The X25519 shared secret is all zero');
        const bytes = new U8(secret);
        if (length === null) return bytes;
        if (length > 256) throw operation('The X25519 secret is shorter than the length');
        return firstBits(bytes, length);
      };
    }
  }
  const supports = (name, op) => !!algorithms[name] && typeof algorithms[name][op] === 'function';

  /* ---- Promises: an operation's binding and synchronous steps reject, never throw. ---- */
  function reject(e) { return new P((_, fail) => fail(e)); }
  /* "Return promise and perform the remaining steps in parallel", whose steps "queue a global
     task on the crypto task source, given realm's global object" to settle the promise: the
     steps run in such a task and settle it there. A realm whose document is no longer fully
     active runs no task of its own (postTask refuses): its promise stays pending. */
  const postTask = host.postTask;
  function queueCryptoTask(task) { try { apply(postTask, host, [task]); } catch (_) {} }
  /* Work too long for the realm's thread (RSA key generation): a native job running in
     parallel; finish() continues the steps in a crypto task once the job is through. */
  const backgroundMark = Symbol('background');
  const background = (job, finish) => ({[backgroundMark]: true, job, finish});
  const isBackground = value => isObject(value) && value[backgroundMark] === true;
  /* next(value) once value, or the background work it stands for, is ready. */
  const after = (value, next) => isBackground(value) ? background(value.job, () => after(value.finish(), next)) : next(value);
  function inParallel(action) {
    let fulfil, fail;
    const promise = new P((resolve, reject) => { fulfil = resolve; fail = reject; });
    const step = run => {
      let value;
      try { value = run(); } catch (e) { fail(e); return; }
      if (!isBackground(value)) { fulfil(value); return; }
      const resume = () => queueCryptoTask(() => step(value.finish));
      let watching;
      try { watching = apply(native.jobWait, native, [value.job, resume]); } catch (e) { fail(e); return; }
      if (!watching) resume();
    };
    queueCryptoTask(() => step(action));
    return promise;
  }
  const result = bytes => apply(taBuffer, copy(bytes), []);
  const algorithmArgument = value => isObject(value) ? value : domString(value);
  function arityCheck(args, count, name) { if (args.length < count) throw new TypeErr(`${name} requires ${count} arguments`); }
  function receiverCheck(receiver) { if (receiver !== subtle) throw new TypeErr('Illegal invocation'); }
  function keyPairObject(pair) {
    const out = {};
    define(out, 'privateKey', {value: pair.privateKey, writable: true, enumerable: true, configurable: true});
    define(out, 'publicKey', {value: pair.publicKey, writable: true, enumerable: true, configurable: true});
    return out;
  }
  /* The key import steps of importKey, unwrapKey and deriveKey (steps 8-11 of importKey). */
  function importSteps(format, keyData, normalized, extractable, usages) {
    const key = algorithms[normalized.name].importKey(format, keyData, normalized, extractable, usages);
    const slot = keySlot(key);
    if ((slot.type === 'secret' || slot.type === 'private') && !usages.length) throw error('SyntaxError', 'A secret or private key needs a usage');
    slot.extractable = !!extractable; slot.usages = normalizedUsages(usages);
    return key;
  }
  function exportSteps(format, key) {
    const slot = keySlot(key);
    if (!supports(slot.algorithm.name, 'exportKey')) throw error('NotSupportedError', `${slot.algorithm.name} keys cannot be exported`);
    if (!slot.extractable) throw error('InvalidAccessError', 'Key is not extractable');
    return algorithms[slot.algorithm.name].exportKey(format, key);
  }
  function checkKey(normalized, key, usage) {
    const slot = keySlot(key);
    if (normalized.name !== slot.algorithm.name) throw error('InvalidAccessError', 'The key is not a key of this algorithm');
    if (!includes(slot.usages, usage)) throw error('InvalidAccessError', `The key does not allow ${usage}`);
  }
  /* "Parse a JWK" (§14.4). */
  function parseJWK(bytes) {
    let value;
    try { value = apply(jsonParse, JSON, [utf8Decode(bytes)]); } catch (_) { throw dataError('The unwrapped key is not JSON'); }
    let jwk;
    try { jwk = jsonWebKey(value, true); } catch (_) { throw dataError('The unwrapped key is not a JSON Web Key'); }
    if (jwk.kty === undefined) throw dataError('The unwrapped JSON Web Key has no kty');
    return jwk;
  }
  /* The keyData argument of importKey: (BufferSource or JsonWebKey). */
  function keyDataUnion(value) {
    if (isObject(value) && !isView(value)) {
      try { apply(abLength, value, []); return {buffer: value}; } catch (_) {}
      if (shared(value)) throw new TypeErr('A shared buffer is not accepted by this BufferSource');
      return {jwk: jsonWebKey(value)};
    }
    if (isView(value)) return {buffer: bufferSource(value, 'keyData')};
    if (value === undefined || value === null) return {jwk: jsonWebKey(value)};
    throw new TypeErr('keyData is neither a BufferSource nor a JsonWebKey');
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
      if (!includes(integers, apply(taTag, array, [])))
        throw error('TypeMismatchError', 'Expected an integer typed array');
      const length = byteLength(bytes);
      if (length > 65536) throw error('QuotaExceededError', 'Random request exceeds 65536 bytes');
      if (length) apply(u8Set, bytes, [randomBytes(length)]);
      return array;
    }
  }
  class SubtleCrypto {
    constructor() { throw new TypeErr('Illegal constructor'); }
    encrypt(algorithm, key, data) { return cipherMethod(this, arguments, 'encrypt'); }
    decrypt(algorithm, key, data) { return cipherMethod(this, arguments, 'decrypt'); }
    sign(algorithm, key, data) { return cipherMethod(this, arguments, 'sign'); }
    verify(algorithm, key, signature, data) {
      try {
        receiverCheck(this); arityCheck(arguments, 4, 'verify');
        const alg = algorithmArgument(algorithm);
        cryptoKey(key, 'key'); bufferSource(signature, 'signature'); bufferSource(data, 'data');
        const normalized = normalizeAlgorithm(alg, 'verify');
        const signatureBytes = snapshot(signature), bytes = snapshot(data);
        return inParallel(() => {
          checkKey(normalized, key, 'verify');
          return algorithms[normalized.name].verify(normalized, key, signatureBytes, bytes);
        });
      } catch (e) { return reject(e); }
    }
    digest(algorithm, data) {
      try {
        receiverCheck(this); arityCheck(arguments, 2, 'digest');
        const alg = algorithmArgument(algorithm);
        bufferSource(data, 'data');
        const normalized = normalizeAlgorithm(alg, 'digest'), bytes = snapshot(data);
        return inParallel(() => result(algorithms[normalized.name].digest(normalized, bytes)));
      } catch (e) { return reject(e); }
    }
    generateKey(algorithm, extractable, keyUsages) {
      try {
        receiverCheck(this); arityCheck(arguments, 3, 'generateKey');
        const alg = algorithmArgument(algorithm), exportable = !!extractable, usages = usageList(keyUsages);
        const normalized = normalizeAlgorithm(alg, 'generateKey');
        return inParallel(() => after(algorithms[normalized.name].generateKey(normalized, exportable, usages), generated => {
          if (generated.privateKey) {
            if (!keySlot(generated.privateKey).usages.length) throw error('SyntaxError', 'A private key needs a usage');
            return keyPairObject(generated);
          }
          const slot = keySlot(generated);
          if ((slot.type === 'secret' || slot.type === 'private') && !slot.usages.length) throw error('SyntaxError', 'A secret key needs a usage');
          return generated;
        }));
      } catch (e) { return reject(e); }
    }
    deriveKey(algorithm, baseKey, derivedKeyType, extractable, keyUsages) {
      try {
        receiverCheck(this); arityCheck(arguments, 5, 'deriveKey');
        const alg = algorithmArgument(algorithm);
        cryptoKey(baseKey, 'baseKey');
        const derived = algorithmArgument(derivedKeyType), exportable = !!extractable, usages = usageList(keyUsages);
        const normalized = normalizeAlgorithm(alg, 'deriveBits');
        const importAlgorithm = normalizeAlgorithm(derived, 'importKey');
        const lengthAlgorithm = normalizeAlgorithm(derived, 'get key length');
        return inParallel(() => {
          checkKey(normalized, baseKey, 'deriveKey');
          const length = algorithms[lengthAlgorithm.name].getKeyLength(lengthAlgorithm);
          const secret = algorithms[normalized.name].deriveBits(normalized, baseKey, length);
          return importSteps('raw', secret, importAlgorithm, exportable, usages);
        });
      } catch (e) { return reject(e); }
    }
    deriveBits(algorithm, baseKey, length = null) {
      try {
        receiverCheck(this); arityCheck(arguments, 2, 'deriveBits');
        const alg = algorithmArgument(algorithm);
        cryptoKey(baseKey, 'baseKey');
        const bits = length === null || length === undefined ? null : enforceRange(length, 4294967295, 'length');
        const normalized = normalizeAlgorithm(alg, 'deriveBits');
        return inParallel(() => {
          checkKey(normalized, baseKey, 'deriveBits');
          return result(algorithms[normalized.name].deriveBits(normalized, baseKey, bits));
        });
      } catch (e) { return reject(e); }
    }
    importKey(format, keyData, algorithm, extractable, keyUsages) {
      try {
        receiverCheck(this); arityCheck(arguments, 5, 'importKey');
        const keyFormatValue = keyFormat(format), data = keyDataUnion(keyData), alg = algorithmArgument(algorithm);
        const exportable = !!extractable, usages = usageList(keyUsages);
        const normalized = normalizeAlgorithm(alg, 'importKey');
        let material;
        if (keyFormatValue === 'jwk') {
          if (!data.jwk) throw new TypeErr('The jwk format takes a JsonWebKey');
          material = data.jwk;
        } else {
          if (data.jwk) throw new TypeErr(`The ${keyFormatValue} format takes a BufferSource`);
          material = snapshot(data.buffer);
        }
        return inParallel(() => importSteps(keyFormatValue, material, normalized, exportable, usages));
      } catch (e) { return reject(e); }
    }
    exportKey(format, key) {
      try {
        receiverCheck(this); arityCheck(arguments, 2, 'exportKey');
        const keyFormatValue = keyFormat(format);
        cryptoKey(key, 'key');
        return inParallel(() => {
          const exported = exportSteps(keyFormatValue, key);
          return keyFormatValue === 'jwk' ? jwkObject(exported) : result(exported);
        });
      } catch (e) { return reject(e); }
    }
    wrapKey(format, key, wrappingKey, wrapAlgorithm) {
      try {
        receiverCheck(this); arityCheck(arguments, 4, 'wrapKey');
        const keyFormatValue = keyFormat(format);
        cryptoKey(key, 'key'); cryptoKey(wrappingKey, 'wrappingKey');
        const alg = algorithmArgument(wrapAlgorithm);
        let normalized;
        try { normalized = normalizeAlgorithm(alg, 'wrapKey'); }
        catch (_) { normalized = normalizeAlgorithm(alg, 'encrypt'); }
        return inParallel(() => {
          checkKey(normalized, wrappingKey, 'wrapKey');
          const exported = exportSteps(keyFormatValue, key);
          const bytes = keyFormatValue === 'jwk' ? utf8Encode(apply(jsonStringify, JSON, [jwkObject(exported)])) : exported;
          const entry = algorithms[normalized.name];
          if (typeof entry.wrapKey === 'function') return result(entry.wrapKey(normalized, wrappingKey, bytes));
          if (typeof entry.encrypt === 'function') return result(entry.encrypt(normalized, wrappingKey, bytes));
          throw error('NotSupportedError', `${normalized.name} cannot wrap keys`);
        });
      } catch (e) { return reject(e); }
    }
    unwrapKey(format, wrappedKey, unwrappingKey, unwrapAlgorithm, unwrappedKeyAlgorithm, extractable, keyUsages) {
      try {
        receiverCheck(this); arityCheck(arguments, 7, 'unwrapKey');
        const keyFormatValue = keyFormat(format);
        bufferSource(wrappedKey, 'wrappedKey'); cryptoKey(unwrappingKey, 'unwrappingKey');
        const alg = algorithmArgument(unwrapAlgorithm), keyAlg = algorithmArgument(unwrappedKeyAlgorithm);
        const exportable = !!extractable, usages = usageList(keyUsages);
        let normalized;
        try { normalized = normalizeAlgorithm(alg, 'unwrapKey'); }
        catch (_) { normalized = normalizeAlgorithm(alg, 'decrypt'); }
        const keyAlgorithm = normalizeAlgorithm(keyAlg, 'importKey');
        const wrapped = snapshot(wrappedKey);
        return inParallel(() => {
          checkKey(normalized, unwrappingKey, 'unwrapKey');
          const entry = algorithms[normalized.name];
          let bytes;
          if (typeof entry.unwrapKey === 'function') bytes = entry.unwrapKey(normalized, unwrappingKey, wrapped);
          else if (typeof entry.decrypt === 'function') bytes = entry.decrypt(normalized, unwrappingKey, wrapped);
          else throw error('NotSupportedError', `${normalized.name} cannot unwrap keys`);
          return importSteps(keyFormatValue, keyFormatValue === 'jwk' ? parseJWK(bytes) : bytes, keyAlgorithm, exportable, usages);
        });
      } catch (e) { return reject(e); }
    }
  }
  /* encrypt, decrypt and sign: (algorithm, key, data). */
  function cipherMethod(receiver, args, op) {
    try {
      receiverCheck(receiver); arityCheck(args, 3, op);
      const algorithm = algorithmArgument(args[0]), key = args[1], data = args[2];
      cryptoKey(key, 'key'); bufferSource(data, 'data');
      const normalized = normalizeAlgorithm(algorithm, op), bytes = snapshot(data);
      return inParallel(() => {
        checkKey(normalized, key, op);
        return result(algorithms[normalized.name][op](normalized, key, bytes));
      });
    } catch (e) { return reject(e); }
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
    define(globalThis, 'SubtleCrypto', { value: SubtleCrypto, writable: true, configurable: true });
    define(CryptoKey.prototype, Symbol.toStringTag, { value: 'CryptoKey', configurable: true });
    for (const name of ['type', 'extractable', 'algorithm', 'usages']) {
      const descriptor = Object.getOwnPropertyDescriptor(CryptoKey.prototype, name);
      descriptor.enumerable = true; define(CryptoKey.prototype, name, descriptor);
    }
    define(globalThis, 'CryptoKey', { value: CryptoKey, writable: true, configurable: true });
    for (const name of ['encrypt', 'decrypt', 'sign', 'verify', 'digest', 'generateKey', 'deriveKey', 'deriveBits',
      'importKey', 'exportKey', 'wrapKey', 'unwrapKey']) {
      const descriptor = Object.getOwnPropertyDescriptor(SubtleCrypto.prototype, name);
      descriptor.enumerable = true; define(SubtleCrypto.prototype, name, descriptor);
    }
    /* CryptoKey is [Serializable] (§13): the internal slots travel, never the cached objects. */
    if (typeof cloneData !== 'undefined') {
      const binary = bytes => { let s = ''; for (let i = 0; i < byteLength(bytes); i++) s += fromCharCode(bytes[i]); return s; };
      const unbinary = text => { const out = new U8(text.length); for (let i = 0; i < text.length; i++) out[i] = apply(charCode, text, [i]); return out; };
      const plain = algorithm => {
        const out = [algorithm.name, algorithm.length === undefined ? -1 : algorithm.length,
          algorithm.namedCurve === undefined ? '' : algorithm.namedCurve, algorithm.hash === undefined ? '' : algorithm.hash.name,
          algorithm.modulusLength === undefined ? -1 : algorithm.modulusLength,
          algorithm.publicExponent === undefined ? '' : binary(algorithm.publicExponent)];
        return out;
      };
      const unplain = r => {
        const algorithm = {name: r[0]};
        if (r[1] !== -1) algorithm.length = r[1];
        if (r[2] !== '') algorithm.namedCurve = r[2];
        if (r[3] !== '') algorithm.hash = {name: r[3]};
        if (r[4] !== -1) { algorithm.modulusLength = r[4]; algorithm.publicExponent = unbinary(r[5]); }
        return algorithm;
      };
      cloneData.registerSerializable('CryptoKey', value => !!apply(keyGet, keySlots, [value]), value => {
        // The handle's members: name, then 's' and a string (a curve) or 'b' and the bytes.
        const slot = keySlot(value), handle = [], usages = [];
        const names = objectKeys(slot.handle);
        for (let n = 0; n < names.length; n++) {
          const name = names[n];
          const v = slot.handle[name], text = typeof v === 'string';
          append(handle, name); append(handle, text ? 's' : 'b'); append(handle, text ? v : binary(v));
        }
        for (let i = 0; i < slot.usages.length; i++) append(usages, slot.usages[i]);
        return [slot.type, slot.extractable, plain(slot.algorithm), usages, handle];
      }, data => {
        const handle = {}, h = data[4];
        for (let i = 0; i < h.length; i += 3) handle[h[i]] = h[i + 1] === 's' ? h[i + 2] : unbinary(h[i + 2]);
        return makeKey(data[0], data[1], unplain(data[2]), data[3], handle);
      });
    }
  }
  for (const name of ['getRandomValues']) {
    const descriptor = Object.getOwnPropertyDescriptor(Crypto.prototype, name);
    descriptor.enumerable = true; define(Crypto.prototype, name, descriptor);
  }
  define(globalThis, 'Crypto', { value: Crypto, writable: true, configurable: true });
  define(globalThis, 'crypto', { get: () => crypto, enumerable: true, configurable: true });
  define(globalThis, 'isSecureContext', { get: () => secure, enumerable: true, configurable: true });
  return secure; // Private bootstrap value, not the page-writable global property.
})(host);
