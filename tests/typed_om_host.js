/* Stand-ins for typed_om_hosttest.c's realm, loaded before the product scripts: what WPT's
   css/css-typed-om files and testharness.js reach that is not under test here (timers, the
   console, a document with elements). The Typed OM values, their parsing, arithmetic and
   serialization, the property grammars, the colours and the declaration blocks are the
   product's. What the host cannot answer throws HOST_UNAVAILABLE, and the check leaves those
   subtests out: computed values (no cascade), a style sheet's rules (no cssom.c), layout. */
'use strict';
globalThis.self = globalThis;
globalThis.console = {log: (...a) => print(a.join(' ')), error: (...a) => print(a.join(' ')),
                      warn: (...a) => print(a.join(' ')), info: () => {}, debug: () => {}};
globalThis.HOST_UNAVAILABLE = 'host: not available without a document';
/* Timers in virtual time: typed_om_hosttest.c runs the next one when no microtask is left. */
{
    const timers = new Map();
    let next = 1, now = 0;
    globalThis.setTimeout = (fn, ms = 0, ...args) => {
        const id = next++;
        timers.set(id, {at: now + Math.max(0, Number(ms) || 0), id, fn, args});
        return id;
    };
    globalThis.clearTimeout = id => { timers.delete(id); };
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
/* The product's Window setup of js_bootstrap.js, with the document's parts stood in: an
   element's inline style is a serialized block that css.c's functions read and write. */
/* idlharness's IDL files, from the WPT checkout */
globalThis.fetch = url => {
    const text = host.readFile(String(url));
    return Promise.resolve({ok: text !== null, status: text === null ? 404 : 200, text: () => Promise.resolve(text || '')});
};
globalThis.__hostRealm = (title, url) => {
    globalThis.DOMException = domExceptionBridge.DOMException;
    const geometry = geometryInterfaces({});
    /* the CSS namespace's own members are js_bootstrap.js's; Typed OM adds the factories */
    globalThis.CSS = {escape: () => { throw new Error(HOST_UNAVAILABLE); }};
    const unavailable = () => { throw new Error(HOST_UNAVAILABLE); };
    const typedOM = typedOMInterfaces({native: host, geometry, DOMException, css: CSS, declarations: {
        read: (block, name) => host.declGet(block.cssText, name),
        write: (block, name, text) => { block.cssText = host.declSet(block.cssText, name, text); },
        names: block => host.declBlock(block.cssText).slice(1),
        computed: unavailable, computedNames: unavailable}});
    host.webidl('Window');
    /* idlharness's exposure: this realm stands for a Window */
    globalThis.Window = function Window() { unavailable(); };
    class HostStyle {
        constructor() { this.text = ''; }
        get cssText() { return this.text; }
        set cssText(v) { this.text = host.declBlock(String(v))[0]; }
        getPropertyValue(name) { return host.declGet(this.text, String(name)); }
        setProperty(name, value) { this.text = host.declSet(this.text, String(name), value == null ? '' : String(value)); }
        removeProperty(name) { const old = this.getPropertyValue(name); this.setProperty(name, ''); return old; }
        get length() { return host.declBlock(this.text).length - 1; }
    }
    /* CSSStyleProperties' named accessors (style.width, style.backgroundImage), as the product's
       bootstrap defines one per property */
    const named = style => new Proxy(style, {
        get: (t, k) => typeof k === 'string' && !(k in t) ? t.getPropertyValue(k === 'cssFloat' ? 'float' : k.replace(/[A-Z]/g, c => '-' + c.toLowerCase())) :
            typeof t[k] === 'function' ? t[k].bind(t) : t[k],
        set: (t, k, v) => { if (typeof k === 'string' && !(k in t)) t.setProperty(k === 'cssFloat' ? 'float' : k.replace(/[A-Z]/g, c => '-' + c.toLowerCase()), v); else t[k] = v; return true; }
    });
    const styles = new WeakMap();
    class HostElement {
        constructor(name) { this.localName = name; styles.set(this, named(new HostStyle())); }
        get style() { return styles.get(this); }
        set style(v) { styles.get(this).cssText = v; }
        get attributeStyleMap() { return typedOM.declaredMap(styles.get(this)); }
        computedStyleMap() { unavailable(); }
        get sheet() { return unavailable(); }
        setAttribute(name, v) { if (name === 'style') styles.get(this).cssText = v; }
        appendChild(child) { return child; }
        append() {}
        remove() {}
    }
    const root = new HostElement('html'), titleElement = new HostElement('title');
    titleElement.firstChild = {data: title};
    globalThis.document = {createElement: name => new HostElement(String(name)), body: root, head: root,
                           documentElement: root, location: {href: url}, getElementById: unavailable, querySelector: unavailable,
                           get styleSheets() { return unavailable(); },
                           getElementsByTagName: name => String(name).toLowerCase() === 'title' && title ? [titleElement] : []};
    globalThis.location = {href: url, pathname: url.replace(/^[a-z]+:\/\/[^/]*/, '')};
    globalThis.Image = unavailable;
};
