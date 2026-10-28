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
    /* css-properties-values-api-1 §4.1: the PropertyDefinition dictionary's members in Web IDL order
       (inherits, initialValue, name, syntax); the registration is the document's [[registeredPropertySet]]
       record, kept by the renderer. inherits is required as the WPT and the shipped engines require it. */
    const register = host.cssRegisterProperty;
    function registerProperty(definition) {
        if (!arguments.length || definition === undefined || definition === null || typeof definition !== 'object' && typeof definition !== 'function')
            throw new Exception('registerProperty requires a PropertyDefinition dictionary');
        const inherits = definition.inherits;
        if (inherits === undefined) throw new Exception('PropertyDefinition requires inherits');
        const flag = !!inherits, initial = definition.initialValue, value = initial === undefined ? undefined : text(initial);
        const name = definition.name;
        if (name === undefined) throw new Exception('PropertyDefinition requires name');
        const property = text(name), syntax = definition.syntax, grammar = syntax === undefined ? '*' : text(syntax);
        const status = register(property, grammar, flag, value);
        if (status === 1) throw new DOMException(`Cannot register ${property}`, 'SyntaxError');
        if (status === 2) throw new DOMException(`${property} is already registered`, 'InvalidModificationError');
    }
    const css = CSS; /* bootstrap's private namespace retains CSS.escape */
    define(css, 'supports', {value:supports, writable:true, enumerable:true, configurable:true});
    define(css, 'registerProperty', {value:registerProperty, writable:true, enumerable:true, configurable:true});
    define(css, Symbol.toStringTag, {value:'CSS', configurable:true});
    define(globalThis, 'CSS', {value:css, writable:true, configurable:true});
})();
