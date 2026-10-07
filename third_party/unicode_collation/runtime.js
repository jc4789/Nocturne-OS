/* Nocturne's independent ECMA-402 Collator and Unicode comparison engine.
 * Generated tables: ICU 78.1, Unicode 17, CLDR 48 (Unicode-3.0).
 * Rebuild with scripts/build_collator.py; no host/runtime locale dependency.
 * The packed tables remain strings until their first actual comparison.
 */
(function () {
    'use strict';
    const DATA = /* @collation_data */ null;
    const slots = new WeakMap(), loaded = Object.create(null);
    const nativeUnpack = typeof host === 'object' && host ? host.collationUnpack : undefined;
    const Uint32 = Uint32Array, Uint8 = Uint8Array;
    const canonical = Intl.getCanonicalLocales.bind(Intl);
    const nfd = Function.call.bind(String.prototype.normalize);
    let classes;
    function toString(value) {
        if (typeof value === 'symbol') throw new TypeError('Cannot convert a Symbol to a string');
        return String(value);
    }
    function unpack(text) {
        // The browser captures this CPU-only private helper before page code.
        // Native decode avoids millions of interpreted operations in the first
        // compare; it does not alter the page task budget or use host locales.
        if (nativeUnpack) return new Uint32(nativeUnpack(text));
        const alphabet = 'ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/';
        const inverse = new Uint8(128);
        for (let i = 0; i < alphabet.length; i++) inverse[alphabet.charCodeAt(i)] = i;
        const bytes = new Uint8(text.length / 4 * 3 - (text.endsWith('==') ? 2 : text.endsWith('=') ? 1 : 0));
        let offset = 0;
        for (let i = 0; i < text.length; i += 4) {
            const n = (inverse[text.charCodeAt(i)] << 18) | (inverse[text.charCodeAt(i + 1)] << 12) |
                      (inverse[text.charCodeAt(i + 2)] << 6) | inverse[text.charCodeAt(i + 3)];
            bytes[offset++] = n >>> 16;
            if (offset < bytes.length) bytes[offset++] = n >>> 8;
            if (offset < bytes.length) bytes[offset++] = n;
        }
        // Unsigned LEB128 integers, independent of the guest CPU's byte order.
        let count = 0;
        for (let i = 0; i < bytes.length; i++) if (!(bytes[i] & 128)) count++;
        const out = new Uint32(count);
        let index = 0, value = 0, shift = 0;
        for (let i = 0; i < bytes.length; i++) {
            const byte = bytes[i];
            value |= (byte & 127) << shift;
            if (byte & 128) shift += 7;
            else { out[index++] = value >>> 0; value = 0; shift = 0; }
        }
        return out;
    }
    function upperBound(array, key, stride) {
        let low = 0, high = array.length / stride;
        while (low < high) {
            const mid = (low + high) >>> 1;
            if (array[mid * stride] <= key) low = mid + 1; else high = mid;
        }
        return (low - 1) * stride;
    }
    function combiningClass(cp) {
        if (!classes) classes = unpack(DATA.ccc);
        const i = upperBound(classes, cp, 2);
        return i >= 0 && classes[i] === cp ? classes[i + 1] : 0;
    }
    function load(name) {
        if (loaded[name]) return loaded[name];
        const source = DATA.tables[name], trie = {children: new Map()};
        if (typeof source === 'string') return loaded[name] = load(source);
        const contexts = unpack(source.contexts);
        for (let i = 0; i < contexts.length;) {
            let node = trie;
            const length = contexts[i++];
            for (let end = i + length; i < end; i++) {
                const cp = contexts[i];
                if (!node.children.has(cp)) node.children.set(cp, {children: new Map()});
                node = node.children.get(cp);
            }
            node.value = contexts[i++];
        }
        return loaded[name] = {ranges: unpack(source.ranges), pool: unpack(source.pool),
            offsets: unpack(source.offsets), trie, reorder: source.reorder,
            variableTop: source.variableTop, base: name === 'enSort' ? null : load('enSort')};
    }
    function reorder(table, primary) {
        const i = upperBound(table.reorder, primary >>> 16, 2);
        return i < 0 ? primary : (primary + (table.reorder[i + 1] | 0) * 65536) >>> 0;
    }
    function appendPool(table, id, out) {
        for (let i = table.offsets[id]; i < table.offsets[id + 1]; i += 3)
            out.push([table.pool[i], table.pool[i + 1], table.pool[i + 2]]);
    }
    function appendScalar(table, cp, out) {
        const r = table.ranges, i = upperBound(r, cp, 6);
        if (i < 0 || cp > r[i + 1]) {
            if (table.base) return appendScalar(table.base, cp, out);
            throw new Error('Missing pinned Unicode collation data');
        }
        if (r[i + 3] === 0xffffffff) appendPool(table, r[i + 2], out);
        else out.push([(r[i + 2] + (r[i + 3] | 0) * (cp - r[i])) >>> 0, r[i + 4], r[i + 5]]);
    }
    function matchContext(table, input, start) {
        let best;
        function visit(node, pos, skipped, matched) {
            if (node.value !== undefined && (!best || matched > best.matched ||
                    matched === best.matched && pos > best.end))
                best = {id: node.value, end: pos, skipped, matched};
            if (pos >= input.length) return;
            const direct = node.children.get(input[pos]);
            if (direct) visit(direct, pos + 1, skipped, matched + 1);
            // UCA discontiguous contractions: an intervening non-starter may
            // be skipped only when its class is lower than the next match.
            const firstClass = combiningClass(input[pos]);
            if (!firstClass) return;
            for (const [cp, child] of node.children) {
                const targetClass = combiningClass(cp);
                if (!targetClass || firstClass >= targetClass) continue;
                let j = pos;
                while (j < input.length && combiningClass(input[j]) > 0 &&
                       combiningClass(input[j]) < targetClass) j++;
                if (input[j] === cp) visit(child, j + 1, skipped.concat(input.slice(pos, j)), matched + 1);
            }
        }
        const first = table.trie.children.get(input[start]);
        if (first) visit(first, start + 1, [], 1);
        return best;
    }
    function digit(cp) {
        const i = upperBound(DATA.digitStarts, cp, 1);
        return i >= 0 && cp - DATA.digitStarts[i] < 10 ? cp - DATA.digitStarts[i] : -1;
    }
    // Port of ICU78 CollationIterator::appendNumericSegmentCEs. The generated
    // Unicode decimal-digit metadata includes non-ASCII scripts. No host int
    // parser, locale service or floating-point conversion of long runs occurs.
    function numericSegment(digits, start, length, out) {
        const base = 0x0f000000;
        const emit = primary => out.push([primary >>> 0, 0x0500, 0x0500]);
        if (length <= 7) {
            let value = digits[start];
            for (let i = 1; i < length; i++) value = value * 10 + digits[start + i];
            if (value < 74) { emit(base | (2 + value) << 16); return; }
            value -= 74;
            if (value < 40 * 254) {
                emit(base | (76 + Math.floor(value / 254)) << 16 | (2 + value % 254) << 8); return;
            }
            value -= 40 * 254;
            if (value < 16 * 254 * 254) {
                let primary = base | (2 + value % 254);
                value = Math.floor(value / 254);
                primary |= (2 + value % 254) << 8;
                value = Math.floor(value / 254);
                emit(primary | (116 + value % 254) << 16); return;
            }
        }
        let primary = base | (128 + Math.floor((length + 1) / 2)) << 16;
        while (digits[start + length - 1] === 0 && digits[start + length - 2] === 0) length -= 2;
        let pos, pair;
        if (length & 1) { pair = digits[start]; pos = 1; }
        else { pair = digits[start] * 10 + digits[start + 1]; pos = 2; }
        pair = 11 + 2 * pair;
        let shift = 8;
        while (pos < length) {
            if (shift === 0) { emit(primary | pair); primary = base; shift = 16; }
            else { primary |= pair << shift; shift -= 8; }
            pair = 11 + 2 * (digits[start + pos] * 10 + digits[start + pos + 1]);
            pos += 2;
        }
        emit(primary | (pair - 1) << shift);
    }
    function elements(table, text, numeric) {
        const input = Array.from(nfd(text, 'NFD'), ch => ch.codePointAt(0)), out = [];
        for (let i = 0; i < input.length;) {
            if (numeric && digit(input[i]) >= 0) {
                const digits = [];
                while (i < input.length && digit(input[i]) >= 0) digits.push(digit(input[i++]));
                let first = 0;
                do {
                    while (first < digits.length - 1 && digits[first] === 0) first++;
                    const length = Math.min(254, digits.length - first);
                    numericSegment(digits, first, length, out);
                    first += length;
                } while (first < digits.length);
                continue;
            }
            const context = matchContext(table, input, i);
            if (context) {
                appendPool(table, context.id, out);
                for (const cp of context.skipped) appendScalar(table, cp, out);
                i = context.end;
            } else appendScalar(table, input[i++], out);
        }
        return out;
    }
    function key(table, text, state) {
        const primary = [], secondary = [], cases = [], tertiary = [];
        let shifted = false;
        for (const ce of elements(table, text, state.numeric)) {
            const p = ce[0], s = ce[1], t = ce[2];
            if (state.ignorePunctuation) {
                if (p > 0x02000000 && p <= table.variableTop) { shifted = true; continue; }
                if (!p && shifted) continue;
                if (p) shifted = false;
            }
            if (p) primary.push(p > 0xffffffff ? p : reorder(table, p));
            if (s) secondary.push(s);
            if (p && (s || t)) cases.push(state.caseFirst === 'upper' ? 0xc000 - (t & 0xc000) + 1 : (t & 0xc000) + 1);
            let weight = t & (state.caseFirst === 'false' ? 0x3f3f : 0xff3f);
            if (weight && state.caseFirst === 'upper') weight = s ? weight ^ 0xc000 : weight + 0x4000;
            if (weight) tertiary.push(weight);
        }
        if (state.sensitivity === 'base') return [primary];
        if (state.sensitivity === 'accent') return [primary, secondary];
        if (state.sensitivity === 'case') return [primary, cases];
        return [primary, secondary, tertiary];
    }
    function compare(state, x, y) {
        x = toString(x); y = toString(y);
        if (x === y) return 0;
        const table = load(state.language + (state.usage === 'search' ? 'Search' : 'Sort'));
        const a = key(table, x, state), b = key(table, y, state);
        for (let level = 0; level < a.length; level++) {
            const left = a[level], right = b[level], end = Math.min(left.length, right.length);
            for (let i = 0; i < end; i++) if (left[i] !== right[i]) return left[i] < right[i] ? -1 : 1;
            if (left.length !== right.length) return left.length < right.length ? -1 : 1;
        }
        return 0;
    }
    function optionsObject(value) {
        if (value === undefined) return Object.create(null);
        if (value === null) throw new TypeError('Intl.Collator options must not be null');
        return Object(value);
    }
    function option(options, property, values, fallback) {
        const value = options[property];
        if (value === undefined) return fallback;
        const result = toString(value);
        if (!values.includes(result)) throw new RangeError('Invalid Intl.Collator ' + property);
        return result;
    }
    function booleanOption(options, name) {
        const value = options[name];
        return value === undefined ? undefined : !!value;
    }
    function language(locale) {
        const lang = locale.split('-')[0];
        return lang === 'en' || lang === 'ja' ? lang : undefined;
    }
    function extensions(locale) {
        const result = Object.create(null), fields = locale.toLowerCase().split('-');
        const start = fields.indexOf('u');
        const privateUse = fields.indexOf('x');
        if (start < 0 || privateUse >= 0 && privateUse < start) return result;
        for (let i = start + 1; i < fields.length && fields[i].length !== 1;) {
            const name = fields[i++];
            if (name.length !== 2) continue;
            const value = [];
            while (i < fields.length && fields[i].length > 2) value.push(fields[i++]);
            result[name] = value.length ? value.join('-') : 'true';
        }
        return result;
    }
    function requireSlot(object) {
        const state = slots.get(object);
        if (!state) throw new TypeError('Incompatible Intl.Collator receiver');
        return state;
    }
    function Collator() {
        // OrdinaryCreateFromConstructor precedes locale/options evaluation.
        const prototype = new.target && new.target.prototype;
        const object = Object.create(prototype !== null && (typeof prototype === 'object' || typeof prototype === 'function') ? prototype : Collator.prototype);
        const locales = canonical(arguments[0]), opts = optionsObject(arguments[1]);
        const usage = option(opts, 'usage', ['sort', 'search'], 'sort');
        option(opts, 'localeMatcher', ['lookup', 'best fit'], 'best fit');
        let collation = opts.collation;
        if (collation !== undefined) {
            collation = toString(collation);
            if (!/^[a-z0-9]{3,8}(?:-[a-z0-9]{3,8})*$/i.test(collation)) throw new RangeError('Invalid collation');
        }
        const numericOption = booleanOption(opts, 'numeric');
        const caseFirstOption = option(opts, 'caseFirst', ['upper', 'lower', 'false'], undefined);
        const requested = locales.find(locale => language(locale)) || 'en';
        const lang = language(requested), ext = extensions(requested), retained = [];
        let numeric = ext.kn === 'true', caseFirst = ['upper', 'lower', 'false'].includes(ext.kf) ? ext.kf : 'false';
        if (ext.kn === 'true' || ext.kn === 'false') {
            if (numericOption === undefined || numericOption === numeric) retained.push(ext.kn === 'true' ? 'kn' : 'kn-false');
        }
        if (ext.kf === caseFirst && ext.kf !== undefined) {
            if (caseFirstOption === undefined || caseFirstOption === caseFirst) retained.unshift('kf-' + caseFirst);
        }
        if (numericOption !== undefined) numeric = numericOption;
        if (caseFirstOption !== undefined) caseFirst = caseFirstOption;
        const sensitivity = option(opts, 'sensitivity', ['base', 'accent', 'case', 'variant'], 'variant');
        const ignorePunctuation = booleanOption(opts, 'ignorePunctuation') || false;
        slots.set(object, {language: lang, locale: lang + (retained.length ? '-u-' + retained.join('-') : ''),
            usage, sensitivity, ignorePunctuation, collation: 'default', numeric, caseFirst, bound: undefined});
        return object;
    }
    // Concise methods/getters are not constructors, just like the intrinsics.
    const methods = {
        supportedLocalesOf(locales) {
            const list = canonical(locales), opts = optionsObject(arguments[1]);
            option(opts, 'localeMatcher', ['lookup', 'best fit'], 'best fit');
            return list.filter(locale => language(locale));
        },
        resolvedOptions() {
            const s = requireSlot(this);
            return {locale: s.locale, usage: s.usage, sensitivity: s.sensitivity, ignorePunctuation: s.ignorePunctuation,
                collation: s.collation, numeric: s.numeric, caseFirst: s.caseFirst};
        },
        localeCompare(that) {
            if (this === null || this === undefined) throw new TypeError('String.localeCompare receiver');
            const left = toString(this), right = toString(that);
            return new Collator(arguments[1], arguments[2]).compare(left, right);
        },
        get compare() {
            const state = requireSlot(this);
            if (!state.bound) {
                state.bound = (x, y) => compareStrings(state, x, y);
                Object.defineProperty(state.bound, 'name', {value: '', configurable: true});
            }
            return state.bound;
        }
    };
    Object.defineProperty(Collator, 'supportedLocalesOf', {value: methods.supportedLocalesOf, configurable: true, writable: true});
    Object.defineProperties(Collator.prototype, {
        compare: {get: Object.getOwnPropertyDescriptor(methods, 'compare').get, configurable: true},
        resolvedOptions: {value: methods.resolvedOptions, writable: true, configurable: true},
        [Symbol.toStringTag]: {value: 'Intl.Collator', configurable: true}
    });
    Object.defineProperty(Object.getOwnPropertyDescriptor(Collator.prototype, 'compare').get, 'name', {value: 'get compare', configurable: true});
    Object.defineProperty(Collator, 'prototype', {writable: false});
    // Avoid getter-name shadowing of the comparison implementation.
    const compareStrings = compare;
    Object.defineProperty(Intl, 'Collator', {value: Collator, writable: true, configurable: true});
    Object.defineProperty(String.prototype, 'localeCompare', {value: methods.localeCompare, writable: true, configurable: true});
})();
