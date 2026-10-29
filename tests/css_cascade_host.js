/* The document a page sees on the host (tests/check_registered_properties.py): nodes, attributes, a
   style element's text and the computed values are tests/css_cascade_host.c's (dom.c, css.c's
   cascade, cssom.c); this file only shapes them as the DOM and CSSOM objects the WPT pages touch.
   Not js.c's bindings: no IDL shapes, no events, no layout (sizes, rectangles), no animation API.
   What the host cannot answer throws HOST_UNAVAILABLE, and the check leaves those subtests out. */
'use strict';
globalThis.self = globalThis.window = globalThis;
globalThis.console = {log: (...a) => print(a.join(' ')), error: (...a) => print(a.join(' ')),
                      warn: (...a) => print(a.join(' ')), info: () => {}, debug: () => {}};
globalThis.HOST_UNAVAILABLE = 'host: not available without layout or animations';
const unavailable = () => { throw new Error(HOST_UNAVAILABLE); };
{
    const timers = new Map();
    let next = 1, now = 0;
    globalThis.setTimeout = (fn, ms = 0, ...args) => {
        const id = next++;
        timers.set(id, {at: now + Math.max(0, Number(ms) || 0), id, fn, args});
        return id;
    };
    globalThis.clearTimeout = id => { timers.delete(id); };
    globalThis.requestAnimationFrame = fn => setTimeout(() => fn(now), 16);
    globalThis.cancelAnimationFrame = clearTimeout;
    globalThis.__runTimer = () => {
        let first = null;
        for (const t of timers.values()) if (!first || t.at < first.at || (t.at === first.at && t.id < first.id)) first = t;
        if (!first) return false;
        timers.delete(first.id);
        now = first.at;
        if (typeof first.fn === 'function') first.fn(...first.args);
        return true;
    };
}
(() => {
    const [documentId, htmlId, headId, bodyId] = host.document();
    const wrappers = new Map(), ids = new WeakMap();
    const id = node => { const i = ids.get(node); if (i === undefined) throw new TypeError('Not a node'); return i; };
    const tree = node => host.tree(id(node));
    function wrap(i) {
        if (i === null || i === undefined) return null;
        let w = wrappers.get(i);
        if (w) return w;
        const t = host.tree(i);
        const C = t[0] === 'element' ? (t[1] === 'style' ? HTMLStyleElement : t[1] === 'template' ? HTMLTemplateElement : HTMLElement) :
            t[0] === 'text' ? Text : t[0] === 'document' ? Document : ShadowRoot;
        w = Object.create(C.prototype);
        ids.set(w, i);
        wrappers.set(i, w);
        return w;
    }
    const nodeOf = v => typeof v === 'object' && v ? v : document.createTextNode(String(v));
    /* simple selectors, a compound of them, and descendant lists of compounds: '#a', 'div.b', '#a > div' */
    function compound(text) {
        const parts = text.match(/[#.]?[\w-]+|\[[^\]]+\]|\*/g) || [];
        return e => parts.every(p => p === '*' ? true : p[0] === '#' ? e.id === p.slice(1) :
            p[0] === '.' ? e.classList.contains(p.slice(1)) :
            p[0] === '[' ? (m => m ? (m[2] === undefined ? e.hasAttribute(m[1]) : e.getAttribute(m[1]) === m[2].replace(/^["']|["']$/g, '')) : false)
                (p.match(/^\[\s*([\w-]+)\s*(?:=\s*([^\]]+?)\s*)?\]$/)) : e.localName === p.toLowerCase());
    }
    function matcher(selector) {
        const lists = String(selector).split(',').map(s => s.trim().split(/\s*>\s*|\s+/).map(compound));
        return e => lists.some(list => {
            if (!list[list.length - 1](e)) return false;
            let n = e;
            for (let k = list.length - 2; k >= 0; k--) {
                n = n.parentElement;
                while (n && !list[k](n)) n = n.parentElement;
                if (!n) return false;
            }
            return true;
        });
    }
    class Node {
        constructor() { throw new TypeError('Illegal constructor'); }
        get parentNode() { return wrap(tree(this)[2]); }
        get parentElement() { const p = this.parentNode; return p instanceof Element ? p : null; }
        get firstChild() { return wrap(tree(this)[3]); }
        get nextSibling() { return wrap(tree(this)[4]); }
        get previousSibling() { return wrap(tree(this)[5]); }
        get lastChild() { let c = this.firstChild; while (c && c.nextSibling) c = c.nextSibling; return c; }
        get childNodes() { const r = []; for (let c = this.firstChild; c; c = c.nextSibling) r.push(c); return r; }
        get children() { return this.childNodes.filter(c => c instanceof Element); }
        get firstElementChild() { return this.children[0] || null; }
        get lastElementChild() { const c = this.children; return c[c.length - 1] || null; }
        get childElementCount() { return this.children.length; }
        get nextElementSibling() { let n = this.nextSibling; while (n && !(n instanceof Element)) n = n.nextSibling; return n; }
        get previousElementSibling() { let n = this.previousSibling; while (n && !(n instanceof Element)) n = n.previousSibling; return n; }
        get ownerDocument() { return document; }
        get isConnected() { let n = this; while (n) { if (n === document) return true; n = n.parentNode || (n instanceof ShadowRoot ? n.host : null); } return false; }
        get textContent() { return this.childNodes.map(c => c instanceof Element || c instanceof Text ? c.textContent : '').join(''); }
        set textContent(v) {
            for (const c of this.childNodes) host.remove(id(c));
            v = v === null ? '' : String(v);
            if (v) host.insert(id(this), host.text(v), null);
        }
        appendChild(c) { host.insert(id(this), id(c), null); return c; }
        append(...cs) { for (const c of cs) this.appendChild(nodeOf(c)); }
        prepend(...cs) { const first = this.firstChild; for (const c of cs) this.insertBefore(nodeOf(c), first); }
        insertBefore(c, ref) { host.insert(id(this), id(c), ref ? id(ref) : null); return c; }
        removeChild(c) { host.remove(id(c)); return c; }
        replaceChildren(...cs) { for (const c of this.childNodes) host.remove(id(c)); this.append(...cs); }
        remove() { host.remove(id(this)); }
        before(...cs) { const p = this.parentNode; for (const c of cs) p.insertBefore(nodeOf(c), this); }
        after(...cs) { const p = this.parentNode, next = this.nextSibling; for (const c of cs) p.insertBefore(nodeOf(c), next); }
        contains(o) { for (let n = o; n; n = n.parentNode) if (n === this) return true; return false; }
        querySelectorAll(selector) {
            const match = matcher(selector), result = [];
            const walk = n => { for (const c of n.children) { if (match(c)) result.push(c); walk(c); } };
            walk(this);
            return result;
        }
        querySelector(selector) { return this.querySelectorAll(selector)[0] || null; }
        getElementById(name) { return this.querySelectorAll('*').find(e => e.id === String(name)) || null; }
        getElementsByTagName(name) { return this.querySelectorAll(String(name)); }
        getElementsByClassName(name) { return this.querySelectorAll('.' + String(name)); }
        addEventListener() {}
        removeEventListener() {}
    }
    class Text extends Node {
        get data() { return tree(this)[8]; }
        get textContent() { return this.data; }
        get nodeType() { return 3; }
    }
    class Document extends Node {
        get documentElement() { return wrap(htmlId); }
        get head() { return wrap(headId); }
        get body() { return wrap(bodyId); }
        get nodeType() { return 9; }
        createElement(name) { return wrap(host.element(null, String(name).toLowerCase())); }
        createElementNS(ns, name) { return wrap(host.element(ns, String(name))); }
        createTextNode(text) { return wrap(host.text(String(text))); }
        get styleSheets() { return unavailable(); }
        get fonts() { return unavailable(); }
        get timeline() { return unavailable(); }
        getAnimations() { return unavailable(); }
        get title() { return globalThis.META_TITLE || ''; }
    }
    class ShadowRoot extends Node {
        get host() { return wrap(tree(this)[7]); }
        get mode() { return 'open'; }
        set innerHTML(v) { unavailable(); }
    }
    const styles = new WeakMap();
    const property = k => k === 'cssFloat' ? 'float' : k.startsWith('--') ? k : k.replace(/[A-Z]/g, c => '-' + c.toLowerCase());
    class CSSStyleDeclaration {
        constructor() { throw new TypeError('Illegal constructor'); }
        get cssText() { return host.inlineText(styles.get(this).getAttribute('style') || ''); }
        set cssText(v) { styles.get(this).setAttribute('style', v === null ? '' : String(v)); }
        getPropertyValue(name) { return host.declGet(this.cssText, String(name)); }
        getPropertyPriority() { return ''; }
        setProperty(name, value, priority = '') {
            const text = host.declSet(this.cssText, String(name), value === null || value === undefined ? '' : String(value), String(priority || ''));
            styles.get(this).setAttribute('style', text);
        }
        removeProperty(name) { const old = this.getPropertyValue(name); this.setProperty(name, ''); return old; }
        get length() { return host.declNames(this.cssText).length; }
        item(i) { return host.declNames(this.cssText)[i] || ''; }
    }
    /* CSSStyleProperties' named accessors (style.width, style.backgroundColor) */
    const named = target => new Proxy(target, {
        get: (t, k) => typeof k === 'string' && !(k in t) && !/^\d+$/.test(k) ? t.getPropertyValue(property(k)) :
            typeof t[k] === 'function' ? t[k].bind(t) : t[k],
        set: (t, k, v) => { if (typeof k === 'string' && !(k in t)) t.setProperty(property(k), v); else t[k] = v; return true; }
    });
    class Element extends Node {
        get localName() { return tree(this)[1]; }
        get tagName() { return this.localName.toUpperCase(); }
        get nodeName() { return this.tagName; }
        get nodeType() { return 1; }
        get id() { return this.getAttribute('id') || ''; }
        set id(v) { this.setAttribute('id', v); }
        get className() { return this.getAttribute('class') || ''; }
        set className(v) { this.setAttribute('class', v); }
        get classList() {
            const e = this, list = () => e.className.split(/\s+/).filter(Boolean);
            return {contains: c => list().includes(c), add: (...c) => e.className = [...new Set([...list(), ...c])].join(' '),
                    remove: (...c) => e.className = list().filter(x => !c.includes(x)).join(' '),
                    toggle: (c, force) => { const has = list().includes(c), want = force === undefined ? !has : !!force;
                                            e.className = want ? [...new Set([...list(), c])].join(' ') : list().filter(x => x !== c).join(' '); return want; }};
        }
        getAttribute(name) { return host.attribute(id(this), String(name).toLowerCase(), undefined); }
        setAttribute(name, value) { host.attribute(id(this), String(name).toLowerCase(), String(value)); }
        removeAttribute(name) { host.attribute(id(this), String(name).toLowerCase(), null); }
        hasAttribute(name) { return this.getAttribute(name) !== null; }
        toggleAttribute(name, force) {
            const has = this.hasAttribute(name), want = force === undefined ? !has : !!force;
            if (want && !has) this.setAttribute(name, ''); else if (!want && has) this.removeAttribute(name);
            return want;
        }
        get style() {
            let s = styles.get(this);
            if (!s) { const decl = Object.create(CSSStyleDeclaration.prototype); styles.set(decl, this); s = named(decl); styles.set(this, s); }
            return s;
        }
        set style(v) { this.style.cssText = v; }
        attachShadow() { return wrap(host.attachShadow(id(this))); }
        get shadowRoot() { return wrap(tree(this)[6]); }
        get innerHTML() { return unavailable(); }
        set innerHTML(v) { if (v === '') this.textContent = ''; else unavailable(); }
        computedStyleMap() { return unavailable(); }
        animate() { return unavailable(); }
        getAnimations() { return unavailable(); }
        getBoundingClientRect() { return unavailable(); }
        get offsetWidth() { return unavailable(); }
        get offsetHeight() { return unavailable(); }
        get clientWidth() { return unavailable(); }
        get offsetTop() { return unavailable(); }
        get offsetLeft() { return unavailable(); }
        get contentWindow() { return unavailable(); }
        get contentDocument() { return unavailable(); }
        get attributeStyleMap() { return unavailable(); }
        get part() { return unavailable(); }
        setHTMLUnsafe() { return unavailable(); }
    }
    class HTMLElement extends Element {}
    class HTMLTemplateElement extends HTMLElement {}
    class CSSRule {
        constructor() { throw new TypeError('Illegal constructor'); }
        get cssText() { return styles.get(this)[1]; }
        get type() { const t = styles.get(this)[0]; return t < 16 ? t : 0; }
    }
    class CSSStyleRule extends CSSRule {}
    class CSSPropertyRule extends CSSRule {
        get name() { return styles.get(this)[2] ?? ''; }
        get syntax() { return styles.get(this)[3] ?? ''; }
        get inherits() { return styles.get(this)[4] === 'true'; }
        get initialValue() { return styles.get(this)[5]; }
    }
    /* a style element's sheet: its rules as cssom.c parses them, read again when the text changes */
    class CSSStyleSheet {
        constructor() { throw new TypeError('Illegal constructor'); }
        get cssRules() {
            return host.rules(id(styles.get(this))).map(r => {
                const rule = Object.create((r[0] === 21 ? CSSPropertyRule : r[0] === 1 ? CSSStyleRule : CSSRule).prototype);
                styles.set(rule, r);
                return rule;
            });
        }
        get rules() { return this.cssRules; }
        insertRule() { return unavailable(); }
        deleteRule() { return unavailable(); }
    }
    class HTMLStyleElement extends HTMLElement {
        get sheet() {
            if (!this.isConnected) return null;
            const s = Object.create(CSSStyleSheet.prototype);
            styles.set(s, this);
            return s;
        }
    }
    const computedStyles = (element, pseudo) => {
        const e = element, p = pseudo === undefined || pseudo === null ? '' : String(pseudo);
        const read = name => host.computed(id(e), name.startsWith('--') ? name : name.toLowerCase(), p);
        const target = {getPropertyValue: name => read(String(name)), getPropertyPriority: () => '',
                        get length() { return host.customNames(id(e)).length; },
                        item: i => host.customNames(id(e))[i] || '',
                        [Symbol.iterator]: function* () { yield* host.customNames(id(e)); },
                        setProperty: () => { throw new DOMException('Computed style is read-only', 'NoModificationAllowedError'); }};
        return new Proxy(target, {
            get: (t, k) => typeof k === 'string' && !(k in t) ?
                (/^\d+$/.test(k) ? t.item(Number(k)) : read(property(k))) : t[k],
        });
    };
    const document = Object.create(Document.prototype);
    ids.set(document, documentId);
    wrappers.set(documentId, document);
    Object.assign(globalThis, {Node, Text, Document, ShadowRoot, Element, HTMLElement, HTMLStyleElement, HTMLTemplateElement,
                               CSSRule, CSSStyleRule, CSSPropertyRule, CSSStyleSheet, CSSStyleDeclaration});
    globalThis.location = {href: 'http://web-platform.test:8000/', pathname: '/'};
    /* the CSS namespace's own members are js_bootstrap.js's; js_css_supports.js adds supports() and registerProperty() */
    globalThis.CSS = {escape: s => String(s).replace(/[^\w-]/g, c => '\\' + c)};
    /* the page's markup, as the check read it: [name, attributes, children] or a text */
    /* after testharness.js: it then takes the shell's environment, not a window's (no load event, no output) */
    globalThis.__buildDocument = (json, title, url) => {
        globalThis.DOMException = domExceptionBridge.DOMException;
        globalThis.document = document;
        globalThis.getComputedStyle = (e, pseudo) => computedStyles(e, pseudo);
        globalThis.META_TITLE = title;
        globalThis.location = {href: url, pathname: url.replace(/^[a-z]+:\/\/[^/]*/, '')};
        const build = (parent, item) => {
            if (typeof item === 'string') { parent.appendChild(document.createTextNode(item)); return; }
            const [name, attributes, children] = item;
            if (name === 'template' && attributes.shadowrootmode) {
                const root = parent.attachShadow({mode: attributes.shadowrootmode});
                for (const c of children) build(root, c);
                return;
            }
            const e = name === 'html' ? document.documentElement : name === 'head' ? document.head : name === 'body' ? document.body :
                document.createElement(name);
            for (const [k, v] of Object.entries(attributes)) e.setAttribute(k, v);
            if (e.parentNode !== parent && name !== 'html' && name !== 'head' && name !== 'body') parent.appendChild(e);
            for (const c of children) build(e, c);
        };
        for (const item of JSON.parse(json)) build(document.documentElement, item);
        /* the window's named properties: an element by its id */
        for (const e of document.querySelectorAll('*')) {
            const name = e.id;
            if (name && !(name in globalThis)) Object.defineProperty(globalThis, name, {configurable: true, get: () => document.getElementById(name)});
        }
    };
})();
