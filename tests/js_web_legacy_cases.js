/* HTML forgiving Base64 and native effective-domain boundaries. */
globalThis.runWebLegacyCases = function runWebLegacyCases() {
    let checks = 0;
    function eq(actual, expected, label) {
        checks++; if (actual !== expected) throw Error('web legacy: ' + label + ': ' + actual + ' != ' + expected);
    }
    function raises(fn, name, label) {
        let error; try { fn(); } catch (e) { error = e; }
        eq(error && error.name, name, label);
        if (name !== 'TypeError') eq(error instanceof DOMException, true, label + ' DOMException');
    }
    eq(typeof atob, 'function', 'atob exists'); eq(typeof btoa, 'function', 'btoa exists');
    eq(atob.length, 1, 'atob arity'); eq(btoa.length, 1, 'btoa arity');
    raises(() => new atob(''), 'TypeError', 'atob is not a constructor');
    raises(() => new btoa(''), 'TypeError', 'btoa is not a constructor');
    eq(Object.hasOwn(atob, 'prototype'), false, 'atob has no constructor prototype');
    eq(Object.hasOwn(btoa, 'prototype'), false, 'btoa has no constructor prototype');
    const vectors = [['',''], ['f','Zg=='], ['fo','Zm8='], ['foo','Zm9v'], ['foob','Zm9vYg=='],
        ['fooba','Zm9vYmE='], ['foobar','Zm9vYmFy'], ['\0\xff\x80','AP+A']];
    for (const [binary, encoded] of vectors) {
        eq(btoa(binary), encoded, 'encode padding'); eq(atob(encoded), binary, 'decode binary');
        eq(atob(encoded.replace(/=+$/, '')), binary, 'unpadded decode');
    }
    eq(atob(' \tZ\ng\f=\r= '), 'f', 'ASCII whitespace anywhere');
    eq(atob('\t\n\f\r '), '', 'ASCII whitespace only');
    eq(atob('YR'), 'a', 'nonzero unused four bits'); eq(atob('YWJ'), 'ab', 'nonzero unused two bits');
    for (const input of ['A','AAAAA','=','==','===','====','A===','AA=','AA===','AAAA==','AA=A',
        '=AAA','AA==A','AA==\0','AA-_','AA\v==','AA\u00a0==','AA\u2003==','AA\ufeff==','éé','🙂'])
        raises(() => atob(input), 'InvalidCharacterError', 'reject invalid Base64 ' + JSON.stringify(input));
    for (const input of ['\u0100','Ā','日本語','🙂','\ud800','\udfff','abc\u0100'])
        raises(() => btoa(input), 'InvalidCharacterError', 'reject nonbyte string');
    raises(() => atob(), 'TypeError', 'atob argument required'); raises(() => btoa(), 'TypeError', 'btoa argument required');
    raises(() => atob(Symbol()), 'TypeError', 'atob Symbol'); raises(() => btoa(Symbol()), 'TypeError', 'btoa Symbol');
    const sentinel = Error('conversion sentinel');
    let converted = 0;
    eq(atob({toString(){converted++;return 'Zg==';}}), 'f', 'atob DOMString conversion');
    eq(btoa({toString(){converted++;return '\xff';}}), '/w==', 'btoa DOMString conversion');
    eq(converted, 2, 'one conversion per operation');
    eq(btoa(null), 'bnVsbA==', 'null converts'); eq(btoa(undefined), 'dW5kZWZpbmVk', 'undefined converts');
    eq(atob(null), '\x9e\xe9\x65', 'null decode DOMString');
    raises(() => atob(undefined), 'InvalidCharacterError', 'undefined is not valid Base64');
    let caught; try { atob({toString(){throw sentinel;}}); } catch (e) { caught = e; }
    eq(caught, sentinel, 'conversion exception identity');
    const decode = atob, encode = btoa;
    eq(decode('Zg=='), 'f', 'detached decode'); eq(encode.call(null, 'f'), 'Zg==', 'null receiver global');
    raises(() => decode.call({}, 'Zg=='), 'TypeError', 'invalid Window receiver');
    let binary = ''; for (let i = 0; i < 256; i++) binary += String.fromCharCode(i);
    eq(atob(btoa(binary)), binary, 'all byte values');
    const large = binary.repeat(48); eq(atob(btoa(large)), large, 'multi-chunk byte roundtrip');
    const descriptor = Object.getOwnPropertyDescriptor(Document.prototype, 'domain');
    eq(typeof descriptor.get, 'function', 'domain prototype getter');
    eq(typeof descriptor.set, 'function', 'domain explicit setter');
    eq(descriptor.enumerable, true, 'domain enumerable');
    raises(() => descriptor.get.call({}), 'TypeError', 'domain getter brand');
    raises(() => descriptor.set.call({}, 'x'), 'TypeError', 'domain setter brand');
    const parsed = new DOMParser().parseFromString('<base href="https://unrelated.invalid/"><p>inert</p>', 'text/html');
    const origin = new URL(document.URL).origin;
    const expected = origin === 'null' ? '' : new URL(origin).hostname;
    eq(document.domain, expected, 'native document origin hostname');
    eq(parsed.domain, expected, 'DOMParser inherits creator URL and origin');
    raises(() => { parsed.domain = parsed.domain; }, 'SecurityError', 'inactive parser setter rejected');
    const blank = document.implementation.createHTMLDocument();
    eq(blank.URL, 'about:blank', 'created blank URL'); eq(blank.domain, '', 'created blank opaque domain');
    raises(() => { blank.domain = ''; }, 'SecurityError', 'blank setter rejected');
    raises(() => { document.domain = Symbol(); }, 'TypeError', 'domain DOMString Symbol');
    const before = document.URL;
    if (expected) {
        document.domain = expected; eq(document.domain, expected, 'same-host compatibility no-op');
        document.domain = expected.toUpperCase(); eq(document.domain, expected, 'same-host normalization');
        for (const value of ['', expected + ':80', expected + '/path', 'user@' + expected, ' ' + expected, '\n' + expected,
                             'https://' + expected, 'not-the-host.invalid'])
            raises(() => { document.domain = value; }, 'SecurityError', 'unsafe host write rejected');
        const dot = expected.indexOf('.');
        if (dot >= 0 && !/^[\d.]+$/.test(expected)) {
            const suffix = expected.slice(dot + 1);
            raises(() => { document.domain = suffix; }, 'NotSupportedError', 'PSL/domain relaxation is explicitly unsupported');
        }
    } else raises(() => { document.domain = ''; }, 'SecurityError', 'live opaque domain setter');
    eq(document.URL, before, 'domain never changes native URL');
    eq(new URL(document.URL).origin, origin, 'domain never forges origin');
    // Private intrinsics and native metadata survive public API replacement.
    const OldURL = globalThis.URL, oldJoin = Array.prototype.join, oldChar = String.prototype.charCodeAt;
    const oldDomain = expected;
    try {
        globalThis.URL = function(){throw Error('public URL must not be used');};
        Object.defineProperty(parsed, 'URL', {configurable:true,value:'https://forged.invalid/'});
        eq(descriptor.get.call(parsed), oldDomain, 'native domain survives forged document.URL and URL constructor');
        Array.prototype.join = function(){throw Error('public join must not be used');};
        String.prototype.charCodeAt = function(){throw Error('public charCodeAt must not be used');};
        eq(decode('Zg=='), 'f', 'decode protected intrinsics'); eq(encode('f'), 'Zg==', 'encode protected intrinsics');
    } finally {
        delete parsed.URL; globalThis.URL = OldURL; Array.prototype.join = oldJoin; String.prototype.charCodeAt = oldChar;
    }
    return checks;
};
