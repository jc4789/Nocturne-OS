/* Auxiliary native-engine regressions, not real-site browser acceptance. */
(() => {
    let checks = 0;
    function eq(actual, expected) {
        ++checks;
        if (actual !== expected) throw new Error(`legacy regexp check ${checks}: ${String(actual)} != ${String(expected)}`);
    }
    function throws(fn) { let threw = false; try { fn(); } catch(e) { threw = e instanceof TypeError; } eq(threw, true); }
    eq(RegExp.$1, ''); eq(RegExp.input, '');
    const extReg = /(\.[a-z0-9]+)$/i;
    eq(extReg.test('https://site/a.css'), true); eq(RegExp.$1, '.css');
    let extname = RegExp.$1, id = 'https://site/a.css'.replace(extReg, '');
    eq(id + extname, 'https://site/a.css');
    /(a)(b)?/.exec('L-a-R');
    eq(RegExp.input, 'L-a-R'); eq(RegExp.$_, RegExp.input);
    eq(RegExp.lastMatch, 'a'); eq(RegExp['$&'], 'a');
    eq(RegExp.lastParen, ''); eq(RegExp['$+'], '');
    eq(RegExp.$1, 'a'); eq(RegExp.$2, ''); eq(RegExp.$9, '');
    eq(RegExp.leftContext, 'L-'); eq(RegExp['$`'], 'L-');
    eq(RegExp.rightContext, '-R'); eq(RegExp["$'"], '-R');
    /not-present/.exec('different input'); eq(RegExp.input, 'L-a-R'); eq(RegExp.$1, 'a');
    /(1)(2)(3)(4)(5)(6)(7)(8)(9)(0)/.test('1234567890');
    eq(RegExp.$9, '9'); eq(RegExp.lastParen, '0');
    /x/.test('x'); eq(RegExp.$1, ''); eq(RegExp.lastParen, '');
    /(?<name>🙂)/du.exec('前🙂後');
    eq(RegExp.$1, '🙂'); eq(RegExp.leftContext, '前'); eq(RegExp.rightContext, '後');
    'a1b2'.replace(/([0-9])/g, '#'); eq(RegExp.$1, '2'); eq(RegExp.lastMatch, '2');
    'a1b2'.match(/([0-9])/g); eq(RegExp.$1, '2');
    'ab'.search(/(b)/); eq(RegExp.$1, 'b');
    'ab'.split(/(a)/); eq(RegExp.$1, 'a');
    Array.from('x1x2'.matchAll(/([0-9])/g)); eq(RegExp.$1, '2');
    RegExp.$_ = 123; eq(RegExp.input, '123'); eq(RegExp.$1, '2');
    const getter = Object.getOwnPropertyDescriptor(RegExp, '$1').get;
    throws(() => getter.call({}));
    const d = Object.getOwnPropertyDescriptor(RegExp, '$1');
    eq(d.enumerable, false); eq(d.configurable, true);
    throws(() => { 'use strict'; RegExp.$1 = 'fake'; });
    const custom = /fake/; custom.exec = () => ['fake']; custom.test('input'); eq(RegExp.$1, '2');
    class Sub extends RegExp {}
    new Sub('(sub)').test('sub'); throws(() => RegExp.$1);
    RegExp.input = 'restored'; eq(RegExp.input, 'restored'); throws(() => RegExp.lastMatch);
    /(restored)/.test('restored'); eq(RegExp.$1, 'restored');
    if (typeof __other !== 'undefined') {
        const foreign = new __other.RegExp('(child)');
        foreign.test('child'); eq(__other.RegExp.$1, 'child'); eq(RegExp.$1, 'restored');
        RegExp.prototype.exec.call(foreign, 'child'); eq(RegExp.$1, 'restored'); eq(__other.RegExp.$1, 'child');
        const local = /(local)/;
        __other.RegExp.prototype.exec.call(local, 'local'); eq(RegExp.$1, 'restored'); eq(__other.RegExp.$1, 'child');
    }
    if (typeof __gc !== 'undefined') {
        __gc(); eq(RegExp.$1, 'restored'); eq(__other.RegExp.$1, 'child');
    }
    if (typeof __failAllocation !== 'undefined') {
        const body = 'a'.repeat(8192);
        /(a+)/.test(`prefix${body}suffix`);
        eq(RegExp.$1, body);
        let failed = false;
        __failAllocation(true);
        try { RegExp.$1; } catch(e) { failed = true; }
        finally { __failAllocation(false); }
        eq(failed, true); eq(RegExp.$1, body); eq(RegExp.leftContext, 'prefix');
    }
    return `REGEXP_LEGACY_AUXILIARY_PASS ${checks}`;
})();
