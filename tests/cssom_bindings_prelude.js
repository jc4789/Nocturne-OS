/* What js_bootstrap.js puts around js_stylesheets.js, as tests/cssom_bindings_cases.c
   needs it: the classes the bindings extend, rawDom.cssom (the product's js_cssom.h),
   the element brand, and a rule's CSSStyleDeclaration (the methods the cases use, over
   the same backend js_bootstrap.js's CSSStyleDeclaration reads and writes). This file
   opens the closure js_stylesheets.js runs in; the harness closes it. */
class Document {}
class ShadowRoot {}
class HTMLStyleElement {}
class HTMLLinkElement {}
Object.assign(globalThis, { Document, ShadowRoot, HTMLStyleElement, HTMLLinkElement });
globalThis.DOMException = class DOMException extends Error {
    constructor(message, name) { super(message); this.name = name; }
};
globalThis.document = fixture('document');
(function () {
'use strict';
const rawDom = { cssom: (...args) => fixtureRaw('cssom', ...args), styleDisabled: () => false };
function htmlElementBrand(node, tag) { if (fixture('tag', node) !== tag) throw new TypeError('Wrong HTML element receiver'); }
function reflectedAttr(node, name) { return fixture('attr', node, name); }
function cssRuleStyle(kind, backend) {
    return {
        get cssText() { return backend.block()[0]; },
        getPropertyValue(name) { return backend.read(name, false); },
        getPropertyPriority(name) { return backend.read(name, true); },
        setProperty(name, value, priority = '') { backend.write(name, String(value), String(priority)); },
    };
}
