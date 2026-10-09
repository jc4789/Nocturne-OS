/* HTML NavigatorPlugins / PDF-viewer capability collections.
 * https://html.spec.whatwg.org/multipage/system-state.html#pdf-viewing-support
 * Nocturne has no inline PDF renderer, so the standard requires empty plugin
 * and MIME lists and pdfViewerEnabled=false. Do not invent installed plugins
 * or browser names. If an inline PDF renderer is added, bind its capability
 * here and implement the standard's associated PDF objects together.
 */
(() => {
    const slots = new WeakMap();
    function slot(value, kind) {
        const state = slots.get(value);
        if (!state || state.kind !== kind) throw new TypeError('Illegal invocation');
        return state;
    }
    function string(value) {
        if (typeof value === 'symbol') throw new TypeError('Cannot convert a Symbol to a string');
        return String(value);
    }
    function define(object, name, descriptor) {
        Object.defineProperty(object, name, {configurable: true, enumerable: true, ...descriptor});
    }
    function interfaceObject(name) {
        const Interface = {[name]: function () { throw new TypeError('Illegal constructor'); }}[name];
        Object.defineProperty(Interface.prototype, Symbol.toStringTag, {value: name, configurable: true});
        Object.defineProperty(globalThis, name, {value: Interface, writable: true, configurable: true});
        return Interface;
    }
    const PluginArray = interfaceObject('PluginArray');
    const MimeTypeArray = interfaceObject('MimeTypeArray');
    const Plugin = interfaceObject('Plugin');
    const MimeType = interfaceObject('MimeType');

    function collection(Interface, kind, nameProperty) {
        define(Interface.prototype, 'length', {get: function () { return slot(this, kind).items.length; }});
        define(Interface.prototype, 'item', {value: function item(index) {
            const state = slot(this, kind);
            if (!arguments.length) throw new TypeError('item requires an index');
            // WebIDL unsigned long conversion, including BigInt/Symbol errors.
            index = (+index) >>> 0;
            return state.items[index] || null;
        }, writable: true});
        define(Interface.prototype, 'namedItem', {value: function namedItem(name) {
            const state = slot(this, kind);
            if (!arguments.length) throw new TypeError('namedItem requires a name');
            name = string(name);
            for (const item of state.items) if (item[nameProperty] === name) return item;
            return null;
        }, writable: true});
        Object.defineProperty(Interface.prototype, Symbol.iterator, {
            value: Array.prototype.values, writable: true, configurable: true,
        });
    }
    collection(PluginArray, 'PluginArray', 'name');
    collection(MimeTypeArray, 'MimeTypeArray', 'type');
    collection(Plugin, 'Plugin', 'type');
    define(PluginArray.prototype, 'refresh', {value: function refresh() {
        slot(this, 'PluginArray');
        // HTML explicitly specifies that refresh() does nothing.
    }, writable: true});
    for (const key of ['name', 'description', 'filename']) {
        define(Plugin.prototype, key, {get: function () { return slot(this, 'Plugin')[key]; }});
    }
    for (const key of ['type', 'description', 'suffixes', 'enabledPlugin']) {
        define(MimeType.prototype, key, {get: function () { return slot(this, 'MimeType')[key]; }});
    }
    function empty(Interface, kind) {
        const value = Object.create(Interface.prototype);
        slots.set(value, {kind, items: []});
        return value;
    }
    const plugins = empty(PluginArray, 'PluginArray');
    const mimeTypes = empty(MimeTypeArray, 'MimeTypeArray');
    const owner = navigator;
    function brand(receiver) { if (receiver !== owner) throw new TypeError('Illegal invocation'); }
    define(owner, 'plugins', {get: function () { brand(this); return plugins; }});
    define(owner, 'mimeTypes', {get: function () { brand(this); return mimeTypes; }});
    define(owner, 'pdfViewerEnabled', {get: function () { brand(this); return false; }});
    define(owner, 'javaEnabled', {value: function javaEnabled() { brand(this); return false; }, writable: true});
})();
