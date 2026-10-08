/* Native <style> sheet information and disabling. Constructed sheets and
 * CSS rule mutation are deliberately not advertised by this partial CSSOM. */
(() => {
    'use strict';
    const define = Object.defineProperty, apply = Reflect.apply;
    const owners = new WeakMap(), sheets = new WeakMap(), token = {};
    const get = WeakMap.prototype.get, set = WeakMap.prototype.set;
    function owner(sheet) {
        const node = apply(get, owners, [sheet]);
        if (!node) throw new TypeError('StyleSheet receiver required');
        return node;
    }
    class StyleSheet {
        constructor(key, node) {
            if (key !== token) throw new TypeError('Illegal StyleSheet constructor');
            apply(set, owners, [this, node]);
        }
        get type() { owner(this); return 'text/css'; }
        get href() { owner(this); return null; }
        get ownerNode() {
            const node = owner(this);
            return rawDom('get', node, 'styleSheetAvailable') ? node : null;
        }
        get parentStyleSheet() { owner(this); return null; }
        get title() { return reflectedAttr(owner(this), 'title') || ''; }
        get disabled() { return rawDom('styleDisabled', owner(this)); }
        set disabled(value) { rawDom('styleDisabled', owner(this), !!value); }
    }
    class CSSStyleSheet extends StyleSheet {
        constructor(key, node) {
            if (key !== token) throw new DOMException('Constructed style sheets are not implemented', 'NotSupportedError');
            super(key, node);
        }
        get ownerRule() { owner(this); return null; }
    }
    // These handles refer to native owners; they do not contain a second CSS
    // tree or a fictitious rule list. cssRules/replace/adoptedStyleSheets remain
    // absent until their actual native CSSOM operations are implemented.
    define(HTMLStyleElement.prototype, 'sheet', { configurable: true, enumerable: true, get() {
        htmlElementBrand(this, 'style');
        if (!rawDom('get', this, 'styleSheetAvailable')) return null;
        let sheet = apply(get, sheets, [this]);
        if (!sheet) { sheet = new CSSStyleSheet(token, this); apply(set, sheets, [this, sheet]); }
        return sheet;
    }});
    define(HTMLStyleElement.prototype, 'disabled', { configurable: true, enumerable: true,
        get() { htmlElementBrand(this, 'style'); return rawDom('styleDisabled', this); },
        set(value) { htmlElementBrand(this, 'style'); rawDom('styleDisabled', this, !!value); }
    });
    for (const C of [StyleSheet, CSSStyleSheet]) {
        define(C.prototype, Symbol.toStringTag, { value: C.name, configurable: true });
        for (const name of Object.getOwnPropertyNames(C.prototype)) {
            if (name !== 'constructor') define(C.prototype, name, { ...Object.getOwnPropertyDescriptor(C.prototype, name), enumerable: true });
        }
    }
    Object.assign(globalThis, { StyleSheet, CSSStyleSheet });
})();
