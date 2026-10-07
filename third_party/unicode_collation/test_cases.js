/* ECMA-402 surface checks plus fixed upstream-ICU comparison vectors.
 * Executed by the combined browser API batch, not by the data generator. */
async function runCollatorCases() {
    let checks = 0;
    const eq = (actual, expected) => { checks++; if (!Object.is(actual, expected)) throw Error('Collator ' + String(actual) + ' != ' + String(expected)); };
    const throws = (fn, name) => { checks++; try { fn(); } catch (error) { if (error.name === name) return; throw error; } throw Error('Collator expected ' + name); };
    const C = Intl.Collator, collator = new C('en'), bound = collator.compare;
    eq(typeof C, 'function'); eq(C.length, 0); eq(bound.length, 2); eq(bound.name, '');
    eq(collator.compare, bound); eq(bound.call(null, 'a', 'b'), -1);
    eq(Object.prototype.toString.call(collator), '[object Intl.Collator]');
    eq(Object.getPrototypeOf(C('en')), C.prototype);
    class Derived extends C {} const derived = new Derived('ja');
    eq(derived instanceof Derived, true); eq(derived.compare('あ', 'か'), -1);
    const getter = Object.getOwnPropertyDescriptor(C.prototype, 'compare').get;
    eq(getter.name, 'get compare'); eq(Object.getOwnPropertyDescriptor(C, 'prototype').writable, false);
    throws(() => getter.call({}), 'TypeError');
    throws(() => C.prototype.resolvedOptions.call(C.prototype), 'TypeError');
    throws(() => C.prototype.resolvedOptions.call(new Proxy(collator, {})), 'TypeError');
    throws(() => new bound('a', 'b'), 'TypeError');
    throws(() => new C.supportedLocalesOf('en'), 'TypeError');
    throws(() => new String.prototype.localeCompare('a'), 'TypeError');
    eq(Object.hasOwn(C.prototype.resolvedOptions, 'prototype'), false);
    throws(() => bound(Symbol(), 'a'), 'TypeError');
    throws(() => bound('a', Symbol()), 'TypeError');
    throws(() => new C('en_US'), 'RangeError');
    throws(() => new C('en', null), 'TypeError');
    throws(() => new C('en', {usage:'invalid'}), 'RangeError');
    throws(() => new C('en', {sensitivity:'invalid'}), 'RangeError');
    throws(() => new C('en', {caseFirst:'invalid'}), 'RangeError');
    throws(() => new C('en', {localeMatcher:'invalid'}), 'RangeError');
    throws(() => new C('en', {collation:'bad_collation'}), 'RangeError');
    throws(() => new C('en', {usage:Symbol()}), 'TypeError');
    eq(C.supportedLocalesOf(['en-US', 'ja-JP', 'fr', 'zz']).join(','), 'en-US,ja-JP');
    eq(C.supportedLocalesOf('fr').length, 0);
    throws(() => C.supportedLocalesOf('en', {localeMatcher:'invalid'}), 'RangeError');
    eq(new C('zz').resolvedOptions().locale, 'en');
    eq(new C('en-x-u-kn').resolvedOptions().numeric, false);
    eq(collator.resolvedOptions().usage, 'sort');
    eq(collator.resolvedOptions().sensitivity, 'variant');
    eq(collator.resolvedOptions().ignorePunctuation, false);
    eq(collator.resolvedOptions().numeric, false); eq(collator.resolvedOptions().caseFirst, 'false');
    const extension = new C('ja-u-co-phonebk-kf-upper-kn').resolvedOptions();
    eq(extension.locale, 'ja-u-kf-upper-kn'); eq(extension.collation, 'default');
    eq(extension.caseFirst, 'upper'); eq(extension.numeric, true);
    const override = new C('en-u-kf-upper-kn', {caseFirst:'lower',numeric:false}).resolvedOptions();
    eq(override.locale, 'en'); eq(override.caseFirst, 'lower'); eq(override.numeric, false);
    eq(new C('ja', {usage:'search',collation:'unihan'}).resolvedOptions().collation, 'default');
    eq(new C('en', {numeric:0,ignorePunctuation:'false'}).resolvedOptions().ignorePunctuation, true);
    const reads = [];
    const options = new Proxy({}, {get(_object, property) { reads.push(property); return undefined; }});
    new C('en', options);
    eq(reads.join(','), 'usage,localeMatcher,collation,numeric,caseFirst,sensitivity,ignorePunctuation');
    const values = [];
    eq(bound({toString(){values.push('left');return 'a';}}, {toString(){values.push('right');return 'b';}}), -1);
    eq(values.join(','), 'left,right');
    eq('CAFÉ'.localeCompare('cafe', 'en', {usage:'search',sensitivity:'base'}), 0);
    eq('file2'.localeCompare('file10', 'en', {numeric:true}), -1);
    throws(() => String.prototype.localeCompare.call(null, 'a'), 'TypeError');
    throws(() => String.prototype.localeCompare.call(undefined, 'a'), 'TypeError');
    const sign = value => value < 0 ? -1 : value > 0 ? 1 : 0;
    const groups = /* @collation_oracle */ null;
    for (const group of groups) {
        const instance = new C(group.locale, group.options);
        for (const vector of group.vectors) {
            const actual = sign(instance.compare(vector[0], vector[1]));
            checks++;
            if (actual !== vector[2]) throw Error('Collator oracle ' + group.locale + ' ' + JSON.stringify(group.options) + ' ' + JSON.stringify(vector) + ' got ' + actual);
            if (checks % 32 === 0) await new Promise(resolve => setTimeout(resolve, 0));
        }
    }
    return checks;
}
