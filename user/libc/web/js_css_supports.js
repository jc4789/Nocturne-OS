/* CSS namespace: queries use the renderer's native parser, not a JS feature
   table, public stylesheet mutation, fetch, Promise, or page-replaceable hooks. */
(() => {
    const evaluate = host.cssSupports, string = String, define = Object.defineProperty, Exception = TypeError;
    function text(value) {
        if (typeof value === 'symbol') throw new Exception('Cannot convert a Symbol to a string');
        return string(value);
    }
    function supports(property) {
        if (!arguments.length) throw new Exception('CSS condition is required');
        const first = text(property);
        if (arguments.length === 1) return !!evaluate(first);
        return !!evaluate(first, text(arguments[1]));
    }
    const css = CSS; /* bootstrap's private namespace retains CSS.escape */
    define(css, 'supports', {value:supports, writable:true, enumerable:true, configurable:true});
    define(css, Symbol.toStringTag, {value:'CSS', configurable:true});
    define(globalThis, 'CSS', {value:css, writable:true, configurable:true});
})();
