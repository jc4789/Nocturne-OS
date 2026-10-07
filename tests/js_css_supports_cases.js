/* Public CSS namespace contracts. Native declaration/condition/rule tests are
   csssupportstest.c. These assertions do not imply public-site acceptance. */
(() => {
    let checks = 0, failed = 0;
    function check(name, yes) { checks++; if (!yes) { failed++; console.log('FAIL css-supports-' + name); } }
    function throws(name, fn, type) { let e; try { fn(); } catch (x) { e=x; } check(name, e instanceof type); }
    check('namespace', Object.prototype.toString.call(CSS) === '[object CSS]');
    check('arity', CSS.supports.length === 1);
    check('escape-preserved', typeof CSS.escape === 'function' && CSS.escape('a b') === 'a\\20 b');
    check('ddg-var', CSS.supports('color', 'var(--test, red)'));
    check('actual-color', CSS.supports('color', 'red'));
    check('actual-flex', CSS.supports('display', 'flex'));
    check('bad-color', !CSS.supports('color', 'nonsense'));
    check('unknown-property', !CSS.supports('not-a-property', 'red'));
    check('no-property-trim', !CSS.supports(' color', 'red'));
    check('no-property-escape', !CSS.supports('c\\6flor', 'red'));
    check('condition-escape', CSS.supports('(c\\6flor: r\\65 d)'));
    check('implied-parens', CSS.supports('color:red'));
    check('not', CSS.supports('not (color:nonsense)'));
    check('and', CSS.supports('(color:red) and (display:flex)'));
    check('or', CSS.supports('(color:red) or (display:nonsense)'));
    check('mixed-invalid', !CSS.supports('(color:red) and (width:1px) or (height:1px)'));
    check('malformed-not', !CSS.supports('not (not (color:red) and)'));
    check('important-two', !CSS.supports('color', 'red !important'));
    check('important-condition', CSS.supports('(color:red !important)'));
    check('custom', CSS.supports('--test', 'anything [balanced]'));
    check('custom-empty', CSS.supports('--test', ''));
    check('custom-invalid', !CSS.supports('--test', ']'));
    check('hash-not-var', !CSS.supports('color', '#var(--x)'));
    check('bad-url-var', !CSS.supports('background-image', 'url(var(--x))'));
    check('custom-bad-url', !CSS.supports('--test', 'url(var(--x))'));
    check('selector', CSS.supports('selector(div > .item)'));
    check('selector-not-implemented', !CSS.supports('selector(:has(div))'));
    check('selector-not-a-list', !CSS.supports('selector(div,span)'));
    check('comments', CSS.supports('not/**/(color:nonsense)'));
    check('unclosed-comment', !CSS.supports('(color:red) /*'));
    check('nul-tail', !CSS.supports('(color:red)\0junk'));
    check('nul-value', !CSS.supports('color', 'red\0junk'));
    check('extra-ignored', CSS.supports('color', 'red', 'ignored'));
    check('receiver-independent', CSS.supports.call(null, 'color', 'red'));
    throws('required', () => CSS.supports(), TypeError);
    throws('symbol-condition', () => CSS.supports(Symbol()), TypeError);
    throws('symbol-value', () => CSS.supports('color', Symbol()), TypeError);
    const sentinel = {};
    let caught;
    try { CSS.supports({toString(){throw sentinel;}}, 'red'); } catch(e) { caught=e; }
    check('conversion-not-swallowed', caught === sentinel);
    let order = '';
    CSS.supports({toString(){order+='p';return 'color';}}, {toString(){order+='v';return 'red';}});
    check('conversion-order', order === 'pv');
    const oldString = globalThis.String, oldDefine = Object.defineProperty;
    try {
        globalThis.String = () => { throw sentinel; };
        Object.defineProperty = () => { throw sentinel; };
        check('captured-intrinsics', CSS.supports({toString(){return 'color';}}, 'red'));
    } finally { globalThis.String=oldString; Object.defineProperty=oldDefine; }
    const oldException=globalThis.TypeError;
    try {
        globalThis.TypeError = function(){throw sentinel;};
        let e;try {CSS.supports(Symbol());}catch(x){e=x;}
        check('captured-typeerror', e instanceof oldException);
    } finally {globalThis.TypeError=oldException;}
    console.log('css-supports-js: ' + checks + ' checks, ' + failed + ' failures');
    if (failed) throw new Error('CSS.supports contract failures: ' + failed);
})();
