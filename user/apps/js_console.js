/* js_console.js - console + value inspection chunk of js.
 * Function body: (function (host, internal) { ... }). Defines internal.inspect,
 * internal.format and the global console. Node util.inspect-like output. */
'use strict';

const hasOwn = Object.hasOwn, ObjKeys = Object.keys, getProto = Object.getPrototypeOf;
const getDesc = Object.getOwnPropertyDescriptor, getSyms = Object.getOwnPropertySymbols;
const propIsEnum = Object.prototype.propertyIsEnumerable;
const objToString = Object.prototype.toString;
const fnToString = Function.prototype.toString;
const SYM_TAG = Symbol.toStringTag, SYM_ITER = Symbol.iterator;
const INSPECT_HOOKS = [Symbol.for('nodejs.util.inspect.custom'), Symbol.for('nocturne.inspect')];
const kObject = 0, kArray = 1, kArrayExtras = 2;
const NO = {};

const SGR = { special: 36, number: 33, bigint: 33, boolean: 33, undefined: 90, null: 1,
  string: 32, symbol: 32, date: 35, regexp: 31 };
const stripAnsi = (s) => s.replace(/\x1b\[[0-9;]*m/g, '');
const st = (ctx, s, type) => {
  const c = ctx.colors ? SGR[type] : undefined;
  return c === undefined ? s : `\x1b[${c}m${s}\x1b[0m`;
};

// Brand checks that do not throw: they return NO for foreign objects.
const brandFn = (proto, prop) => { const d = getDesc(proto, prop); return d.get || d.value; };
const probe = (fn, v, a) => { try { return fn.call(v, a); } catch { return NO; } };
const mapSize = brandFn(Map.prototype, 'size'), setSize = brandFn(Set.prototype, 'size');
const reSource = brandFn(RegExp.prototype, 'source'), dateTime = Date.prototype.getTime;
const abLength = brandFn(ArrayBuffer.prototype, 'byteLength'), dvLength = brandFn(DataView.prototype, 'byteLength');
const sabLength = typeof SharedArrayBuffer === 'function' ? brandFn(SharedArrayBuffer.prototype, 'byteLength') : null;
const TAProto = getProto(Uint8Array.prototype);
const taName = getDesc(TAProto, SYM_TAG).get, taLength = getDesc(TAProto, 'length').get;
const wmHas = WeakMap.prototype.has, wsHas = WeakSet.prototype.has;
const wrDeref = typeof WeakRef === 'function' ? WeakRef.prototype.deref : null;
const boxedValueOf = { Number: Number.prototype.valueOf, String: String.prototype.valueOf,
  Boolean: Boolean.prototype.valueOf, BigInt: BigInt.prototype.valueOf, Symbol: Symbol.prototype.valueOf };

const errMsg = (e) => { try { return String(e && e.message !== undefined ? e.message : e); } catch { return 'unknown'; } };
const safeGet = (o, k) => { try { return o[k]; } catch { return undefined; } };
const instOf = (v, c) => { try { return v instanceof c; } catch { return false; } };
const isIndexKey = (k) => /^(?:0|[1-9]\d*)$/.test(k) && +k < 4294967295;
const more = (n, what = 'item') => `... ${n} more ${what}${n > 1 ? 's' : ''}`;
const fmtNum = (n) => (Object.is(n, -0) ? '-0' : `${n}`);

function getKeys(v, showHidden) {
  const syms = getSyms(v);
  let keys;
  if (showHidden) {
    keys = Object.getOwnPropertyNames(v);
    for (const s of syms) keys.push(s);
  } else {
    keys = ObjKeys(v);
    for (const s of syms) if (propIsEnum.call(v, s)) keys.push(s);
  }
  return keys;
}
// Non-index keys form a suffix of the own key order (indices first), scan from the end.
function nonIndexKeys(ctx, v) {
  const keys = getKeys(v, ctx.showHidden);
  let i = keys.length;
  while (i > 0 && (typeof keys[i - 1] === 'symbol' || !isIndexKey(keys[i - 1]))) i--;
  return i === 0 ? keys : keys.slice(i);
}

function getCtorName(obj) {
  let p = obj, first = NO;
  while (p !== null && p !== undefined) {
    let d;
    try { d = getDesc(p, 'constructor'); } catch { d = undefined; }
    if (d !== undefined && typeof d.value === 'function' && typeof d.value.name === 'string' &&
        d.value.name !== '' && instOf(obj, d.value)) return d.value.name;
    p = getProto(p);
    if (first === NO) first = p;
  }
  return first === null ? null : 'Object';
}

function getPrefix(ctor, tag, fallback, size = '') {
  if (ctor === null) {
    if (tag !== '' && fallback !== tag) return `[${fallback}${size}: null prototype] [${tag}] `;
    return `[${fallback}${size}: null prototype] `;
  }
  if (tag !== '' && ctor !== tag) return `${ctor}${size} [${tag}] `;
  return `${ctor}${size} `;
}

// ---- primitives ----
const ESC_META = { '\b': '\\b', '\t': '\\t', '\n': '\\n', '\f': '\\f', '\r': '\\r', '\\': '\\\\' };
const escRe = /[\x00-\x1f'\\\x7f-\x9f]|[\ud800-\udbff](?![\udc00-\udfff])|(?<![\ud800-\udbff])[\udc00-\udfff]/g;
function strEscape(s) {
  let q = "'";
  if (s.includes("'")) {
    if (!s.includes('"')) q = '"';
    else if (!s.includes('`') && !s.includes('${')) q = '`';
  }
  if (!/[\x00-\x1f'\\\x7f-\x9f\ud800-\udfff]/.test(s)) return q + s + q;
  return q + s.replace(escRe, (c) => {
    if (c === "'") return q === "'" ? "\\'" : c;
    return ESC_META[c] || `\\${c.length === 1 && c.charCodeAt(0) >= 0xd800 ? 'u' : 'x'}${
      c.charCodeAt(0).toString(16).toUpperCase().padStart(c.charCodeAt(0) >= 0xd800 ? 4 : 2, '0')}`;
  }) + q;
}

function formatPrimitive(ctx, v) {
  switch (typeof v) {
    case 'string': {
      let trailer = '';
      if (v.length > ctx.maxStringLength) {
        trailer = more(v.length - ctx.maxStringLength, 'character');
        v = v.slice(0, ctx.maxStringLength);
      }
      let out;
      if (ctx.compact !== true && v.length > 16 && v.length > ctx.breakLength - ctx.indentationLvl - 4) {
        out = v.split(/(?<=\n)/).map((l) => st(ctx, strEscape(l), 'string'))
          .join(` +\n${' '.repeat(ctx.indentationLvl + 2)}`);
      } else out = st(ctx, strEscape(v), 'string');
      return trailer ? `${out}${trailer}` : out; // Node appends the trailer directly
    }
    case 'number': return st(ctx, fmtNum(v), 'number');
    case 'bigint': return st(ctx, `${v}n`, 'bigint');
    case 'boolean': return st(ctx, `${v}`, 'boolean');
    case 'undefined': return st(ctx, 'undefined', 'undefined');
    case 'symbol': return st(ctx, Symbol.prototype.toString.call(v), 'symbol');
  }
  return String(v);
}

// ---- functions / errors ----
function getFunctionBase(value, ctor, tag, rawTag) {
  let src = '';
  try { src = fnToString.call(value); } catch {}
  const name = typeof safeGet(value, 'name') === 'string' ? value.name : '';
  if (/^class[\s{]/.test(src)) {
    let base = `class ${(hasOwn(value, 'name') && name) || '(anonymous)'}`;
    if (ctor !== 'Function' && ctor !== null) base += ` [${ctor}]`;
    if (tag !== '' && ctor !== tag) base += ` [${tag}]`;
    if (ctor !== null) {
      const sup = safeGet(getProto(value) || {}, 'name');
      if (sup && typeof sup === 'string') base += ` extends ${sup}`;
    } else base += ' extends [null prototype]';
    return `[${base}]`;
  }
  let type = 'Function';
  if (rawTag === 'GeneratorFunction' || rawTag === 'AsyncFunction' || rawTag === 'AsyncGeneratorFunction') type = rawTag;
  let base = `[${type}`;
  if (ctor === null) base += ' (null prototype)';
  base += name === '' ? ' (anonymous)' : `: ${name}`;
  base += ']';
  if (ctor !== type && ctor !== null) base += ` ${ctor}`;
  if (tag !== '' && ctor !== tag) base += ` [${tag}]`;
  return base;
}

function formatError(ctx, err, ctor, tag, keys) {
  let name = safeGet(err, 'name'), message = safeGet(err, 'message'), stack = safeGet(err, 'stack');
  try {
    name = name == null ? 'Error' : String(name);
    message = message == null ? '' : String(message);
  } catch { name = 'Error'; message = ''; }
  stack = typeof stack === 'string' ? stack.replace(/\s+$/, '') : '';
  let text;
  if (stack === '') text = message ? `${name}: ${message}` : name;
  else if (/^\s*at\s|^\s*$/.test(stack.split('\n', 1)[0]) || /^[^\n]*@[^\n]*$/.test(stack.split('\n', 1)[0])) {
    // frames only (QuickJS): synthesize the header
    text = (message ? `${name}: ${message}` : name) + '\n' + stack;
  } else text = stack;
  // drop own props already visible in the text
  if (!ctx.showHidden) {
    for (const k of ['name', 'message', 'stack']) {
      const i = keys.indexOf(k);
      if (i !== -1) { const v = safeGet(err, k); if (typeof v !== 'string' || text.includes(v)) keys.splice(i, 1); }
    }
  }
  try {
    if ('cause' in err && !keys.includes('cause')) keys.push('cause');
    if ('errors' in err && Array.isArray(err.errors) && !keys.includes('errors')) keys.push('errors');
  } catch {}
  // class name differs from err.name: follow Node's improveStack
  if (name.endsWith('Error') && text.startsWith(name) && (text.length === name.length || text[name.length] === ':' || text[name.length] === '\n')) {
    let fallback = 'Error', len = name.length;
    if (ctor === null) {
      const m = /^([A-Z][a-z_ A-Z0-9[\]()-]+)(?::|\n\s+at)/.exec(text) || /^([a-z_A-Z0-9-]*Error)$/.exec(text);
      fallback = (m && m[1]) || '';
      len = fallback.length;
      fallback = fallback || 'Error';
    }
    const prefix = getPrefix(ctor, tag, fallback).slice(0, -1);
    if (name !== prefix) {
      if (prefix.includes(name)) text = len === 0 ? `${prefix}: ${text}` : prefix + text.slice(len);
      else text = `${prefix} [${name}]${text.slice(len)}`;
    }
  }
  if (!text.includes('\n    at')) text = `[${text}]`;
  if (ctx.indentationLvl !== 0) text = text.replace(/\n/g, `\n${' '.repeat(ctx.indentationLvl)}`);
  return text;
}

// ---- element formatters ----
function formatProperty(ctx, value, recurseTimes, key, type) {
  let desc, str, name;
  try { desc = getDesc(value, key); } catch {}
  if (desc === undefined) desc = { value: safeGet(value, key), enumerable: true };
  if (desc.value !== undefined) {
    ctx.indentationLvl += 2;
    str = formatValue(ctx, desc.value, recurseTimes);
    ctx.indentationLvl -= 2;
  } else if (desc.get !== undefined) {
    str = st(ctx, desc.set !== undefined ? '[Getter/Setter]' : '[Getter]', 'special');
  } else if (desc.set !== undefined) str = st(ctx, '[Setter]', 'special');
  else str = st(ctx, 'undefined', 'undefined');
  if (type === kArray) return str;
  if (typeof key === 'symbol') name = `[${st(ctx, Symbol.prototype.toString.call(key), 'symbol')}]`;
  else if (key === '__proto__') name = "['__proto__']";
  else if (desc.enumerable === false) name = `[${key.replace(escRe, (c) => ESC_META[c] || c)}]`;
  else if (/^[a-zA-Z_][a-zA-Z_0-9]*$/.test(key)) name = key;
  else name = st(ctx, strEscape(key), 'string');
  return `${name}: ${str}`;
}

function formatSparse(ctx, value, rt, max, out, i) {
  const keys = ObjKeys(value);
  let index = i;
  for (; i < keys.length && out.length < max; i++) {
    const key = keys[i], n = +key;
    if (n > 4294967294) break;
    if (`${index}` !== key) {
      if (!/^(?:0|[1-9][0-9]*)$/.test(key)) break;
      const empty = n - index;
      out.push(st(ctx, `<${empty} empty item${empty > 1 ? 's' : ''}>`, 'undefined'));
      index = n;
      if (out.length === max) break;
    }
    out.push(formatProperty(ctx, value, rt, key, kArray));
    index++;
  }
  const remaining = value.length - index;
  if (out.length !== max) {
    if (remaining > 0) out.push(st(ctx, `<${remaining} empty item${remaining > 1 ? 's' : ''}>`, 'undefined'));
  } else if (remaining > 0) out.push(more(remaining));
  return out;
}

function formatArray(ctx, value, rt) {
  const len = value.length, max = Math.min(Math.max(0, ctx.maxArrayLength), len), out = [];
  for (let i = 0; i < max; i++) {
    if (!hasOwn(value, i)) return formatSparse(ctx, value, rt, max, out, i);
    out.push(formatProperty(ctx, value, rt, i, kArray));
  }
  if (len > max) out.push(more(len - max));
  return out;
}

function formatTypedArray(ctx, value, rt, size) {
  const max = Math.min(Math.max(0, ctx.maxArrayLength), size), out = [];
  for (let i = 0; i < max; i++) out.push(formatPrimitive(ctx, value[i]));
  if (size > max) out.push(more(size - max));
  if (ctx.showHidden) {
    ctx.indentationLvl += 2;
    for (const k of ['BYTES_PER_ELEMENT', 'length', 'byteLength', 'byteOffset', 'buffer'])
      out.push(`[${k}]: ${formatValue(ctx, value[k], rt, true)}`);
    ctx.indentationLvl -= 2;
  }
  return out;
}

function formatSet(ctx, value, rt, size) {
  const max = Math.min(Math.max(0, ctx.maxArrayLength), size), out = [];
  ctx.indentationLvl += 2;
  let i = 0;
  for (const v of Set.prototype.values.call(value)) {
    if (i++ >= max) break;
    out.push(formatValue(ctx, v, rt));
  }
  if (size > max) out.push(more(size - max));
  ctx.indentationLvl -= 2;
  return out;
}

function formatMap(ctx, value, rt, size) {
  const max = Math.min(Math.max(0, ctx.maxArrayLength), size), out = [];
  ctx.indentationLvl += 2;
  let i = 0;
  for (const [k, v] of Map.prototype.entries.call(value)) {
    if (i++ >= max) break;
    out.push(`${formatValue(ctx, k, rt)} => ${formatValue(ctx, v, rt)}`);
  }
  if (size > max) out.push(more(size - max));
  ctx.indentationLvl -= 2;
  return out;
}

function formatArrayBuffer(ctx, value) {
  let u8;
  try { u8 = new Uint8Array(value); } catch { return [st(ctx, '(detached)', 'special')]; }
  const n = Math.min(50, u8.length), hex = [];
  for (let i = 0; i < n; i++) hex.push((u8[i] < 16 ? '0' : '') + u8[i].toString(16));
  let s = hex.join(' ');
  if (u8.length > n) s += ` ${more(u8.length - n, 'byte')}`;
  return [`${st(ctx, '[Uint8Contents]', 'special')}: <${s}>`];
}

function formatPromise(ctx, value, rt, pst) {
  if (pst[0] === 0) return [st(ctx, '<pending>', 'special')];
  ctx.indentationLvl += 2;
  const s = formatValue(ctx, pst[1], rt);
  ctx.indentationLvl -= 2;
  return [pst[0] === 2 ? `${st(ctx, '<rejected>', 'special')} ${s}` : s];
}

// ---- layout ----
function groupArrayElements(ctx, output, value) {
  let total = 0, maxLen = 0, i = 0, outLen = output.length;
  if (ctx.maxArrayLength < output.length) outLen--; // keep the "... more items" line out
  const sep = 2, dataLen = new Array(outLen);
  for (; i < outLen; i++) {
    const len = ctx.colors ? stripAnsi(output[i]).length : output[i].length;
    dataLen[i] = len;
    total += len + sep;
    if (maxLen < len) maxLen = len;
  }
  const actualMax = maxLen + sep;
  if (actualMax * 3 + ctx.indentationLvl < ctx.breakLength &&
      (total / actualMax > 5 || maxLen <= 6)) {
    const bias = Math.sqrt(actualMax - total / output.length);
    const biasedMax = Math.max(actualMax - 3 - bias, 1);
    const columns = Math.min(
      Math.round(Math.sqrt(2.5 * biasedMax * outLen) / biasedMax),
      Math.floor((ctx.breakLength - ctx.indentationLvl) / actualMax),
      ctx.compact * 4, 15);
    if (columns <= 1) return output;
    const tmp = [], maxLine = [];
    for (let c = 0; c < columns; c++) {
      let w = 0;
      for (let j = c; j < output.length; j += columns) if (dataLen[j] > w) w = dataLen[j];
      maxLine.push(w + sep);
    }
    let padStart = true;
    if (value !== undefined) {
      for (let k = 0; k < output.length; k++) {
        const t = typeof value[k];
        if (t !== 'number' && t !== 'bigint') { padStart = false; break; }
      }
    }
    for (let r = 0; r < outLen; r += columns) {
      const max = Math.min(r + columns, outLen);
      let s = '', j = r;
      for (; j < max - 1; j++) {
        const pad = maxLine[j - r] + output[j].length - dataLen[j];
        s += padStart ? `${output[j]}, `.padStart(pad, ' ') : `${output[j]}, `.padEnd(pad, ' ');
      }
      if (padStart) s += output[j].padStart(maxLine[j - r] + output[j].length - dataLen[j] - sep, ' ');
      else s += output[j];
      tmp.push(s);
    }
    if (ctx.maxArrayLength < output.length) tmp.push(output[outLen]);
    output = tmp;
  }
  return output;
}

function isBelowBreakLength(ctx, output, start, base) {
  let total = output.length + start;
  if (total + output.length > ctx.breakLength) return false;
  for (let i = 0; i < output.length; i++) {
    total += ctx.colors ? stripAnsi(output[i]).length : output[i].length;
    if (total > ctx.breakLength) return false;
  }
  return base === '' || !base.includes('\n');
}

function reduceToSingleString(ctx, output, base, braces, extras, recurseTimes, value) {
  if (typeof ctx.compact === 'number' && ctx.compact >= 1) {
    const entries = output.length;
    if (extras === kArrayExtras && entries > 6) output = groupArrayElements(ctx, output, value);
    if (ctx.currentDepth - recurseTimes < ctx.compact && entries === output.length) {
      const start = output.length + ctx.indentationLvl + braces[0].length + base.length + 10;
      if (isBelowBreakLength(ctx, output, start, base)) {
        const joined = output.join(', ');
        if (!joined.includes('\n')) return `${base ? `${base} ` : ''}${braces[0]} ${joined} ${braces[1]}`;
      }
    }
  }
  const ind = `\n${' '.repeat(ctx.indentationLvl)}`;
  return `${base ? `${base} ` : ''}${braces[0]}${ind}  ${output.join(`,${ind}  `)}${ind}${braces[1]}`;
}

// ---- core ----
function formatValue(ctx, value, recurseTimes, typedArray) {
  if ((typeof value !== 'object' && typeof value !== 'function') || value === null)
    return value === null ? st(ctx, 'null', 'null') : formatPrimitive(ctx, value);
  const seenLen = ctx.seen.length, ind = ctx.indentationLvl;
  try {
    if (ctx.customInspect) {
      for (const sym of INSPECT_HOOKS) {
        const f = value[sym];
        if (typeof f === 'function' && f !== inspect) {
          const c = safeGet(value, 'constructor');
          if (c && c.prototype === value) break;
          const depth = ctx.depth - recurseTimes;
          const opts = userOptions(ctx);
          opts.depth = depth;
          opts.stylize = (s, t) => st(ctx, String(s), t);
          const ret = f.call(value, depth, opts, inspect);
          if (ret !== value) {
            if (typeof ret !== 'string') return formatValue(ctx, ret, recurseTimes);
            return ret.replace(/\n/g, `\n${' '.repeat(ctx.indentationLvl)}`);
          }
          break;
        }
      }
    }
    if (ctx.seen.includes(value)) {
      let index = 1;
      if (ctx.circular === undefined) { ctx.circular = new Map(); ctx.circular.set(value, 1); }
      else {
        index = ctx.circular.get(value);
        if (index === undefined) { index = ctx.circular.size + 1; ctx.circular.set(value, index); }
      }
      return st(ctx, `[Circular *${index}]`, 'special');
    }
    return formatRaw(ctx, value, recurseTimes, typedArray);
  } catch (e) {
    ctx.seen.length = seenLen;
    ctx.indentationLvl = ind;
    return st(ctx, `[Object: <Inspection threw (${errMsg(e)})>]`, 'special');
  }
}

function formatRaw(ctx, value, recurseTimes, typedArray) {
  const ctor = getCtorName(value);
  const rawTag = safeGet(value, SYM_TAG);
  let tag = typeof rawTag === 'string' ? rawTag : '';
  // an own tag is listed as a key already; do not print it twice
  if (tag !== '' && (ctx.showHidden ? hasOwn(value, SYM_TAG) : propIsEnum.call(value, SYM_TAG))) tag = '';
  const bt = typeof rawTag === 'string' ? rawTag : objToString.call(value).slice(8, -1);
  let keys, base = '', braces, extras = kObject, noIter = true, size, r;
  let formatter = () => [];

  if (value[SYM_ITER] || ctor === null) {
    noIter = false;
    if (Array.isArray(value)) {
      const prefix = (ctor !== 'Array' || tag !== '') ? getPrefix(ctor, tag, 'Array', `(${value.length})`) : '';
      keys = nonIndexKeys(ctx, value);
      braces = [`${prefix}[`, ']'];
      if (value.length === 0 && keys.length === 0) return `${braces[0]}]`;
      extras = kArrayExtras;
      formatter = formatArray;
    } else if ((size = probe(setSize, value)) !== NO) {
      const prefix = getPrefix(ctor, tag, 'Set', `(${size})`);
      keys = getKeys(value, ctx.showHidden);
      formatter = (c, v, rt) => formatSet(c, v, rt, size);
      if (size === 0 && keys.length === 0) return `${prefix}{}`;
      braces = [`${prefix}{`, '}'];
    } else if ((size = probe(mapSize, value)) !== NO) {
      const prefix = getPrefix(ctor, tag, 'Map', `(${size})`);
      keys = getKeys(value, ctx.showHidden);
      formatter = (c, v, rt) => formatMap(c, v, rt, size);
      if (size === 0 && keys.length === 0) return `${prefix}{}`;
      braces = [`${prefix}{`, '}'];
    } else if (typeof (r = taName.call(value)) === 'string') {
      size = taLength.call(value);
      keys = nonIndexKeys(ctx, value);
      braces = [`${getPrefix(ctor, tag, r, `(${size})`)}[`, ']'];
      if (size === 0 && keys.length === 0 && !ctx.showHidden) return `${braces[0]}]`;
      formatter = (c, v, rt) => formatTypedArray(c, v, rt, size);
      extras = kArrayExtras;
    } else noIter = true;
  }

  if (noIter) {
    keys = getKeys(value, ctx.showHidden);
    braces = ['{', '}'];
    if (typeof value === 'function') {
      base = st(ctx, getFunctionBase(value, ctor, tag, rawTag), 'special');
      if (keys.length === 0) return base;
    } else if (ctor === 'Object') {
      if (bt === 'Arguments') {
        keys = nonIndexKeys(ctx, value);
        braces = ['[Arguments] [', ']'];
        extras = kArrayExtras;
        formatter = formatArray;
        if (value.length === 0 && keys.length === 0) return '[Arguments] []';
      } else if (tag !== '') braces[0] = `${getPrefix(ctor, tag, 'Object')}{`;
      if (keys.length === 0 && braces[0] !== '[Arguments] [') return `${braces[0]}}`;
    } else if (bt === 'RegExp' && probe(reSource, value) !== NO) {
      base = RegExp.prototype.toString.call(ctor !== null ? value : new RegExp(value.source, value.flags));
      const prefix = getPrefix(ctor, tag, 'RegExp');
      if (prefix !== 'RegExp ') base = `${prefix}${base}`;
      base = st(ctx, base, 'regexp');
      if (keys.length === 0 || recurseTimes > ctx.depth) return base;
    } else if (bt === 'Date' && (r = probe(dateTime, value)) !== NO) {
      base = Number.isNaN(r) ? 'Invalid Date' : Date.prototype.toISOString.call(value);
      const prefix = getPrefix(ctor, tag, 'Date');
      if (prefix !== 'Date ') base = `${prefix}${base}`;
      base = st(ctx, base, 'date');
      if (keys.length === 0) return base;
    } else if (bt === 'Error' || instOf(value, Error)) {
      base = formatError(ctx, value, ctor, tag, keys);
      if (keys.length === 0) return base;
    } else if ((bt === 'ArrayBuffer' && probe(abLength, value) !== NO) ||
               (bt === 'SharedArrayBuffer' && sabLength && probe(sabLength, value) !== NO)) {
      const prefix = getPrefix(ctor, tag, bt);
      if (typedArray === undefined) formatter = formatArrayBuffer;
      else if (keys.length === 0) return `${prefix}{ byteLength: ${st(ctx, fmtNum(value.byteLength), 'number')} }`;
      braces[0] = `${prefix}{`;
      keys.unshift('byteLength');
    } else if (bt === 'DataView' && probe(dvLength, value) !== NO) {
      braces[0] = `${getPrefix(ctor, tag, 'DataView')}{`;
      keys.unshift('byteLength', 'byteOffset', 'buffer');
    } else if (bt === 'Promise' && (r = (() => { try { return host.promiseState(value); } catch { return NO; } })()) !== NO) {
      braces[0] = `${getPrefix(ctor, tag, 'Promise')}{`;
      const pst = r;
      formatter = (c, v, rt) => formatPromise(c, v, rt, pst);
    } else if (bt === 'WeakSet' && probe(wsHas, value, {}) !== NO) {
      braces[0] = `${getPrefix(ctor, tag, 'WeakSet')}{`;
      formatter = (c) => [st(c, '<items unknown>', 'special')];
    } else if (bt === 'WeakMap' && probe(wmHas, value, {}) !== NO) {
      braces[0] = `${getPrefix(ctor, tag, 'WeakMap')}{`;
      formatter = (c) => [st(c, '<items unknown>', 'special')];
    } else if (bt === 'WeakRef' && wrDeref && probe(wrDeref, value) !== NO) {
      braces[0] = `${getPrefix(ctor, tag, 'WeakRef')}{`;
      formatter = (c, v) => [st(c, wrDeref.call(v) === undefined ? '<cleared>' : '<target>', 'special')];
    } else if (boxedValueOf[bt] && (r = probe(boxedValueOf[bt], value)) !== NO) {
      if (bt === 'String' && !ctx.showHidden) keys.splice(0, value.length);
      base = `[${bt}`;
      if (bt !== ctor) base += ctor === null ? ' (null prototype)' : ` (${ctor})`;
      base += `: ${formatPrimitive({ ...ctx, colors: false }, r)}]`;
      if (tag !== '' && tag !== ctor) base += ` [${tag}]`;
      if (keys.length === 0) return st(ctx, base, bt.toLowerCase());
      base = st(ctx, base, bt.toLowerCase());
    } else {
      if (keys.length === 0) return `${getPrefix(ctor, tag, 'Object')}{}`;
      braces[0] = `${getPrefix(ctor, tag, 'Object')}{`;
    }
  }

  if (recurseTimes > ctx.depth) {
    let name = getPrefix(ctor, tag, 'Object').slice(0, -1);
    if (ctor !== null) name = `[${name}]`;
    return st(ctx, name, 'special');
  }
  recurseTimes += 1;
  ctx.seen.push(value);
  ctx.currentDepth = recurseTimes;
  const output = formatter(ctx, value, recurseTimes);
  for (let i = 0; i < keys.length; i++) output.push(formatProperty(ctx, value, recurseTimes, keys[i], extras));
  if (ctx.circular !== undefined) {
    const index = ctx.circular.get(value);
    if (index !== undefined) {
      const ref = st(ctx, `<ref *${index}>`, 'special');
      base = base === '' ? ref : `${ref} ${base}`;
    }
  }
  ctx.seen.pop();
  return reduceToSingleString(ctx, output, base, braces, extras, recurseTimes, value);
}

function userOptions(ctx) {
  return { showHidden: ctx.showHidden, depth: ctx.depth, colors: ctx.colors, customInspect: ctx.customInspect,
    compact: ctx.compact, breakLength: ctx.breakLength, maxArrayLength: ctx.maxArrayLength,
    maxStringLength: ctx.maxStringLength };
}

function inspect(value, opts) {
  const ctx = { seen: [], circular: undefined, indentationLvl: 0, currentDepth: 0, depth: 2, colors: false,
    showHidden: false, compact: 3, breakLength: 80, maxArrayLength: 100, maxStringLength: 10000,
    customInspect: true };
  try {
    if (opts !== null && typeof opts === 'object') {
      const lim = (v, d) => (v === null || v === Infinity ? Infinity : typeof v === 'number' ? v : d);
      if ('depth' in opts) ctx.depth = lim(opts.depth, 2);
      if ('colors' in opts) ctx.colors = !!opts.colors;
      if ('showHidden' in opts) ctx.showHidden = !!opts.showHidden;
      if ('customInspect' in opts) ctx.customInspect = !!opts.customInspect;
      if ('compact' in opts) ctx.compact = typeof opts.compact === 'number' ? opts.compact : opts.compact === false ? 0 : 3;
      if ('breakLength' in opts) ctx.breakLength = lim(opts.breakLength, 80);
      if ('maxArrayLength' in opts) ctx.maxArrayLength = lim(opts.maxArrayLength, 100);
      if ('maxStringLength' in opts) ctx.maxStringLength = lim(opts.maxStringLength, 10000);
    }
    return formatValue(ctx, value, 0);
  } catch (e) {
    return `[Inspection threw (${errMsg(e)})]`;
  }
}

// ---- util.format ----
let builtinNames;
function hasBuiltInToString(v) {
  try {
    const tp = v[Symbol.toPrimitive];
    if (typeof tp === 'function' && tp !== Date.prototype[Symbol.toPrimitive]) return false;
    if (typeof v.toString !== 'function') return true;
    if (hasOwn(v, 'toString')) return false;
    let p = v;
    do { p = getProto(p); } while (p !== null && !hasOwn(p, 'toString'));
    if (p === null) return true;
    const d = getDesc(p, 'constructor');
    if (!(d && typeof d.value === 'function')) return false;
    if (!builtinNames) builtinNames = new Set(Object.getOwnPropertyNames(globalThis).filter((k) => /^[A-Z]/.test(k)));
    return builtinNames.has(d.value.name);
  } catch { return true; }
}

function tryNum(f, x) { try { return f(x); } catch { return NaN; } }

function format(args, colors) {
  const first = args[0];
  const iopts = { colors: !!colors };
  let a = 0, str = '', join = '';
  if (typeof first === 'string') {
    if (args.length === 1) return first;
    let lastPos = 0, tmp;
    for (let i = 0; i < first.length - 1; i++) {
      if (first.charCodeAt(i) !== 37) continue;
      const c = first.charCodeAt(++i);
      if (a + 1 !== args.length) {
        switch (c) {
          case 115: { // s
            const v = args[++a];
            if (typeof v === 'number') tmp = fmtNum(v);
            else if (typeof v === 'bigint') tmp = `${v}n`;
            else if (typeof v !== 'object' || v === null || !hasBuiltInToString(v)) {
              try { tmp = String(v); } catch (e) { tmp = `[${errMsg(e)}]`; }
            } else tmp = inspect(v, { depth: 0, colors: false });
            break;
          }
          case 106: { // j
            try { tmp = JSON.stringify(args[++a]); } catch (e) {
              tmp = /circular/i.test(errMsg(e)) ? '[Circular]' : `[${errMsg(e)}]`;
            }
            break;
          }
          case 100: { // d
            const v = args[++a];
            tmp = typeof v === 'bigint' ? `${v}n` : typeof v === 'symbol' ? 'NaN' : fmtNum(tryNum(Number, v));
            break;
          }
          case 79: tmp = inspect(args[++a], iopts); break; // O
          case 111: tmp = inspect(args[++a], { colors: !!colors, showHidden: true, depth: 4 }); break; // o
          case 105: { // i
            const v = args[++a];
            tmp = typeof v === 'bigint' ? `${v}n` : typeof v === 'symbol' ? 'NaN' : fmtNum(tryNum(parseInt, v));
            break;
          }
          case 102: { // f
            const v = args[++a];
            tmp = typeof v === 'symbol' ? 'NaN' : fmtNum(tryNum(parseFloat, v));
            break;
          }
          case 99: a++; tmp = ''; break; // c
          case 37: str += first.slice(lastPos, i); lastPos = i + 1; continue;
          default: continue;
        }
        if (lastPos !== i - 1) str += first.slice(lastPos, i - 1);
        str += tmp;
        lastPos = i + 1;
      } else if (c === 37) {
        str += first.slice(lastPos, i);
        lastPos = i + 1;
      }
    }
    if (lastPos !== 0) {
      a++;
      join = ' ';
      if (lastPos < first.length) str += first.slice(lastPos);
    }
  }
  for (; a < args.length; a++) {
    const v = args[a];
    str += join + (typeof v !== 'string' ? inspect(v, iopts) : v);
    join = ' ';
  }
  return str;
}

// ---- console ----
function charWidth(cp) {
  if (cp < 0x20 || (cp >= 0x7f && cp < 0xa0) || (cp >= 0x300 && cp <= 0x36f) || (cp >= 0x200b && cp <= 0x200f) ||
      (cp >= 0xfe00 && cp <= 0xfe0f) || cp === 0xfeff) return 0;
  if (cp >= 0x1100 && (cp <= 0x115f || cp === 0x2329 || cp === 0x232a ||
      (cp >= 0x2e80 && cp <= 0xa4cf && cp !== 0x303f) || (cp >= 0xac00 && cp <= 0xd7a3) ||
      (cp >= 0xf900 && cp <= 0xfaff) || (cp >= 0xfe30 && cp <= 0xfe6f) || (cp >= 0xff00 && cp <= 0xff60) ||
      (cp >= 0xffe0 && cp <= 0xffe6) || (cp >= 0x1f300 && cp <= 0x1f64f) || (cp >= 0x1f900 && cp <= 0x1f9ff) ||
      (cp >= 0x20000 && cp <= 0x3fffd))) return 2;
  return 1;
}
function strWidth(s) {
  s = stripAnsi(s);
  if (/^[\x20-\x7e]*$/.test(s)) return s.length;
  let w = 0;
  for (const ch of s) w += charWidth(ch.codePointAt(0));
  return w;
}

function renderTable(head, columns) {
  const widths = head.map(strWidth);
  const rows = [], longest = Math.max(0, ...columns.map((c) => c.length));
  for (let i = 0; i < head.length; i++) {
    for (let j = 0; j < longest; j++) {
      if (rows[j] === undefined) rows[j] = [];
      const v = rows[j][i] = hasOwn(columns[i], j) ? columns[i][j] : '';
      widths[i] = Math.max(widths[i], strWidth(v));
    }
  }
  const div = widths.map((w) => '─'.repeat(w + 2));
  const line = (row) => `│ ${row.map((c, i) => c + ' '.repeat(widths[i] - strWidth(c))).join(' │ ')} │`;
  let out = `┌${div.join('┬')}┐\n${line(head)}\n├${div.join('┼')}┤\n`;
  for (const row of rows) out += `${line(row)}\n`;
  return `${out}└${div.join('┴')}┘`;
}

function formatTime(ms) {
  let h = 0, m = 0, s = 0;
  if (ms >= 1000) {
    if (ms >= 60000) {
      if (ms >= 3600000) { h = Math.floor(ms / 3600000); ms %= 3600000; }
      m = Math.floor(ms / 60000);
      ms %= 60000;
    }
    s = ms / 1000;
  }
  if (h !== 0 || m !== 0) {
    const [sec, frac] = s.toFixed(3).split('.');
    const pad = (x) => String(x).padStart(2, '0');
    return `${h !== 0 ? `${h}:${pad(m)}` : m}:${pad(sec)}.${frac} (${h !== 0 ? 'h:m' : ''}m:ss.mmm)`;
  }
  if (s !== 0) return `${s.toFixed(3)}s`;
  return `${Number(ms.toFixed(3))}ms`;
}

const ttyCache = [undefined, undefined, undefined];
const colorsFor = (fd) => (ttyCache[fd] === undefined ? (ttyCache[fd] = !!host.isTerminal(fd)) : ttyCache[fd]);
let groupIndent = '';
const counts = new Map(), timers = new Map();

function emit(fd, text) {
  if (groupIndent) text = groupIndent + text.replace(/\n/g, `\n${groupIndent}`);
  host.write(fd, `${text}\n`);
}
const logTo = (fd, args) => emit(fd, format(args, colorsFor(fd)));
const warn = (msg) => emit(2, msg);

function tableCell(v) {
  const isObj = v !== null && typeof v === 'object';
  const depth = isObj && !Array.isArray(v) && ObjKeys(v).length > 2 ? -1 : 0;
  return inspect(v, { depth, maxArrayLength: 3, breakLength: Infinity, colors: false });
}

function table(data, props) {
  if (data === null || typeof data !== 'object') return logTo(1, [data]);
  const idx = (n) => Array.from({ length: n }, (_, i) => tableCell(i));
  if (probe(mapSize, data) !== NO) {
    const ks = [], vs = [];
    for (const [k, v] of Map.prototype.entries.call(data)) { ks.push(tableCell(k)); vs.push(tableCell(v)); }
    return emit(1, renderTable(['(iteration index)', 'Key', 'Values'], [idx(ks.length), ks, vs]));
  }
  if (probe(setSize, data) !== NO) {
    const vs = [];
    for (const v of Set.prototype.values.call(data)) vs.push(tableCell(v));
    return emit(1, renderTable(['(iteration index)', 'Values'], [idx(vs.length), vs]));
  }
  const map = Object.create(null), valuesCol = [];
  let hasPrim = false;
  const props2 = Array.isArray(props) ? props : undefined;
  const indexKeys = ObjKeys(data);
  for (let i = 0; i < indexKeys.length; i++) {
    const item = data[indexKeys[i]];
    const prim = item === null || (typeof item !== 'function' && typeof item !== 'object');
    if (props2 === undefined && prim) { hasPrim = true; valuesCol[i] = tableCell(item); continue; }
    for (const key of props2 || ObjKeys(item)) {
      if (map[key] === undefined) map[key] = [];
      map[key][i] = (prim && props2) || !hasOwn(item, key) ? '' : tableCell(item[key]);
    }
  }
  const keys = ObjKeys(map), cols = keys.map((k) => map[k]);
  if (hasPrim) { keys.push('Values'); cols.push(valuesCol); }
  keys.unshift('(index)');
  cols.unshift(indexKeys);
  emit(1, renderTable(keys, cols));
}

const labelOf = (l) => (l === undefined ? 'default' : `${l}`);

const con = {
  log(...a) { logTo(1, a); },
  info(...a) { logTo(1, a); },
  debug(...a) { logTo(1, a); },
  warn(...a) { logTo(2, a); },
  error(...a) { logTo(2, a); },
  dirxml(...a) { logTo(1, a); },
  trace(...a) {
    let lines = String(new Error().stack || '').split('\n').filter((l) => l.trim() !== '');
    while (lines.length && !/^\s*at\s/.test(lines[0]) && !/@/.test(lines[0])) lines.shift();
    lines.shift(); // frame of trace itself
    emit(2, (a.length ? `Trace: ${format(a, colorsFor(2))}` : 'Trace') + (lines.length ? `\n${lines.join('\n')}` : ''));
  },
  assert(cond, ...a) {
    if (cond) return;
    if (a.length === 0) emit(2, 'Assertion failed');
    else if (typeof a[0] === 'string') { a[0] = `Assertion failed: ${a[0]}`; logTo(2, a); }
    else emit(2, `Assertion failed: ${format(a, colorsFor(2))}`);
  },
  dir(obj, options) {
    emit(1, inspect(obj, { colors: colorsFor(1), ...(options !== null && typeof options === 'object' ? options : {}) }));
  },
  table,
  group(...a) { if (a.length) logTo(1, a); groupIndent += '  '; },
  groupCollapsed(...a) { if (a.length) logTo(1, a); groupIndent += '  '; },
  groupEnd() { groupIndent = groupIndent.slice(0, -2); },
  count(label) {
    label = labelOf(label);
    const n = (counts.get(label) || 0) + 1;
    counts.set(label, n);
    logTo(1, ['%s: %d', label, n]);
  },
  countReset(label) {
    label = labelOf(label);
    if (!counts.has(label)) return warn(`Warning: Count for '${label}' does not exist`);
    counts.delete(label);
  },
  time(label) {
    label = labelOf(label);
    if (timers.has(label)) return warn(`Warning: Label '${label}' already exists for console.time()`);
    timers.set(label, host.now());
  },
  timeLog(label, ...data) {
    label = labelOf(label);
    const t = timers.get(label);
    if (t === undefined) return warn(`Warning: No such label '${label}' for console.timeLog()`);
    logTo(1, ['%s: %s', label, formatTime(host.now() - t), ...data]);
  },
  timeEnd(label) {
    label = labelOf(label);
    const t = timers.get(label);
    if (t === undefined) return warn(`Warning: No such label '${label}' for console.timeEnd()`);
    timers.delete(label);
    logTo(1, ['%s: %s', label, formatTime(host.now() - t)]);
  },
  clear() { if (host.isTerminal(1)) host.write(1, '\x1b[2J\x1b[H'); },
};
Object.defineProperty(con, SYM_TAG, { value: 'console', configurable: true });
Object.defineProperty(globalThis, 'console', { value: con, writable: true, configurable: true });

internal.inspect = inspect;
internal.format = format;
