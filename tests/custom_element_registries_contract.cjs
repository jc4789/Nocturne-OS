/* Scoped custom element registries (HTML "custom elements", DOM "create an element", "flatten
   element creation options", "insert") in the realm half that owns them: js_custom_elements.js,
   loaded unchanged into two node vm realms that stand for a Window and its same-origin iframe's
   Window of one runtime. A fake rawDom stands in for the native tree and its registry slots the
   way dom.c and js.c keep them: -1 null, a positive number the agent's scoped registry, 0 a
   global registry recorded with its Window's Document; a never-assigned element slot is its node
   document's; reading another realm's global identity gives that realm's registry object
   (host.customElementAgent's second argument). The DOM operations call the bridge's before/after
   the way js.c's customElementBefore/After do. The cases follow WPT custom-elements/registries
   (Element-customElementRegistry, Document-createElement, CustomElementRegistry-initialize,
   scoped-registry-define-upgrade-criteria, scoped-registry-append, constructor-reentry-createElement,
   constructor-direct-call-fallback-registry). Not the guest browser, the native DOM or a WPT run.
   Run from the repository root: node tests/custom_element_registries_contract.cjs */
'use strict';
const fs = require('node:fs'), vm = require('node:vm'), crypto = require('node:crypto');
const source = fs.readFileSync('user/libc/web/js_custom_elements.js', 'utf8');
console.log('source_sha256=' + crypto.createHash('sha256').update(source).digest('hex'));
const HTML = 'http://www.w3.org/1999/xhtml';

let passed = 0, failed = 0;
function check(condition, name) { if (condition) passed++; else { failed++; console.log('FAIL ' + name); } }
function throws(fn, name, expected) {
    try { fn(); check(false, name + ' (no exception)'); }
    catch (e) { check(e && e.name === expected, name + ' (' + (e && e.name) + ': ' + (e && e.message) + ')'); }
}

// The native tree: a record per node; each node has one wrapper, from its document's realm.
const record = new WeakMap(), realms = [];
let agentState = null;
function wrapper(realm, fields) {
    const proto = fields.type === 9 ? realm.context.Document.prototype :
        fields.type === 1 ? (fields.namespace === HTML ? realm.context.HTMLElement.prototype : realm.context.Element.prototype) :
        realm.context.Node.prototype;
    const node = Object.create(proto);
    record.set(node, Object.assign({parent: null, children: [], attrs: new Map(), registry: -2, registryRealm: null,
        is: null, shadowRoot: null, shadowHost: null, owner: null, realm}, fields));
    return node;
}
const R = node => { const r = record.get(node); if (!r) throw new TypeError('Node required'); return r; };
function rootOf(node) { let n = node; for (;;) { const r = R(n); if (r.parent) n = r.parent; else if (r.shadowHost) n = r.shadowHost; else return n; } }
const connected = node => R(rootOf(node)).type === 9;
function shadowIncluding(node, visit) {
    visit(node);
    const r = R(node);
    if (r.shadowRoot) shadowIncluding(r.shadowRoot, visit);
    for (const child of r.children) shadowIncluding(child, visit);
}
// dom.c doc_custom_registry and js.c DOM_customData "registry".
function registrySlot(node) {
    const r = R(node);
    if (r.type === 9) return {id: r.registry === -2 ? (r.window ? 0 : -1) : r.registry, realm: r.registryRealm || node};
    if (r.type !== 1 && !r.shadowHost) return {id: -1, realm: null};
    if (r.registry === -2) { const d = registrySlot(r.owner); r.registry = d.id; r.registryRealm = d.id === 0 ? d.realm : null; }
    if (r.registry === 0 && !r.registryRealm) r.registryRealm = r.owner;
    return {id: r.registry, realm: r.registryRealm};
}
function makeRawDom(realm) {
    const raw = {
        isNode: (_, n) => record.has(n),
        get(node, key) {
            const r = R(node);
            switch (key) {
            case 'nodeType': return r.type;
            case 'shadowHost': return r.shadowHost;
            case 'ownerDocument': return r.type === 9 ? null : r.owner;
            case 'localName': return r.localName ?? null;
            case 'namespaceURI': return r.namespace ?? null;
            case 'isConnected': return connected(node);
            case 'childNodes': return r.children.slice();
            case 'parentNode': return r.parent;
            case 'attributeNames': return [...r.attrs.keys()];
            case 'shadowRoot': return r.shadowRoot;
            case 'elementBrand': if (r.type !== 1) throw new TypeError('Element receiver required'); return true;
            default: throw new Error('fake rawDom.get: ' + key);
            }
        },
        customData(node, key, ...value) {
            const r = R(node);
            if (key === 'authorConstructor') return undefined;
            if (key === 'is') { if (value.length) r.is = value[0]; return r.is; }
            if (key !== 'registry') throw new Error('fake customData: ' + key);
            if (value.length) {
                r.registry = value[0];
                r.registryRealm = value[0] === 0 ? (value[1] || realm.document) : null;
                return undefined;
            }
            const slot = registrySlot(node);
            if (slot.id !== 0 || slot.realm === realm.document) return slot.id;
            const owner = realms.find(other => other.document === slot.realm);
            return owner && owner.registry ? owner.registry : null;
        },
        customCandidates(root, name) {
            const found = [];
            shadowIncluding(root, n => {
                const r = R(n);
                if (r.type === 1 && r.namespace === HTML && (r.is !== null || r.localName.includes('-')) &&
                    (name === null || r.localName === name || r.is === name)) found.push(n);
            });
            return found;
        },
        query(root, selector) {
            const found = [];
            const walk = n => { for (const child of R(n).children) { if (R(child).type === 1) found.push(child); walk(child); } };
            walk(root);
            return found;
        },
        create(owner, type, localName, text, namespace = HTML, prefix = null) {
            const doc = owner || realm.document;
            return wrapper(R(doc).realm, {type, localName, namespace, prefix, owner: doc});
        },
        attrList: () => [],
        attrNS: (node, ns, name) => R(node).attrs.get(name) ?? null,
        face: () => null,
        set(node, key, value) { if (key === 'elementPrefix') R(node).prefix = value; },
        adopt(owner, node) { R(node).owner = owner; },
    };
    return raw;
}
function insert(realm, parent, child) {
    const bridge = realm.context.customElementsBridge;
    return bridge.reactions(() => {
        const token = bridge.before('insert', parent, child, null);
        const c = R(child), p = R(parent);
        if (c.parent) R(c.parent).children.splice(R(c.parent).children.indexOf(child), 1);
        const owner = p.type === 9 ? parent : p.owner;
        if (c.owner !== owner) shadowIncluding(child, n => { R(n).owner = owner; });
        c.parent = parent; p.children.push(child);
        bridge.after(token, undefined);
        return child;
    });
}
function attachShadow(host, registry) {
    const realm = R(host).realm;
    const shadow = wrapper(realm, {type: 11, shadowHost: host, owner: R(host).owner});
    R(host).shadowRoot = shadow;
    const owner = R(host).owner;
    const value = registry === undefined ? realm.context.customElementsBridge.registryFor(owner) : registry;
    realm.context.customElementsBridge.setRegistry(shadow, value);
    return shadow;
}

function makeRealm(name) {
    const realm = {name, reported: []};
    const context = vm.createContext({});
    realm.context = context;
    context.host = {
        customElementAgent(init, registry) {
            if (registry) realm.registry = registry;
            if (!agentState) agentState = init();
            return agentState;
        },
        domHooks() {},
    };
    vm.runInContext(`
        class DOMException extends Error { constructor(message, name) { super(message); this.name = name; } }
        class Node {}
        class Element extends Node {}
        class HTMLElement extends Element { constructor() { return customElementsBridge.construct(new.target); } }
        class HTMLUnknownElement extends HTMLElement {}
        class Document extends Node {}
        const apply = Reflect.apply;
        const attributeBridge = {mutation: () => null};
        const reflectedAttr = (node, key) => rawDom.attrNS(node, null, key);
        const report = e => reported.push(e);
        globalThis.Node = Node; globalThis.Element = Element; globalThis.HTMLElement = HTMLElement;
        globalThis.HTMLUnknownElement = HTMLUnknownElement; globalThis.Document = Document; globalThis.DOMException = DOMException;
    `, context);
    context.reported = realm.reported;
    context.queueMicrotask = fn => Promise.resolve().then(fn);
    context.rawDom = makeRawDom(realm);
    realm.document = wrapper(realm, {type: 9, window: true});
    const html = wrapper(realm, {type: 1, localName: 'html', namespace: HTML, owner: realm.document});
    R(html).parent = realm.document; R(realm.document).children.push(html);
    realm.body = html;
    context.document = realm.document;
    realms.push(realm);
    vm.runInContext(source + '\nglobalThis.customElementsBridge = customElementsBridge;', context, {filename: 'js_custom_elements.js'});
    return realm;
}
const top = makeRealm('window'), frame = makeRealm('iframe');
const T = top.context, F = frame.context;
const run = (realm, code) => vm.runInContext(code, realm.context);
// Document createElement, as js_document.js composes the bridge's steps.
function createElement(realm, doc, name, options) {
    const bridge = realm.context.customElementsBridge;
    const converted = bridge.creationOptions(options);
    const {registry, is} = bridge.flatten(converted, doc);
    return bridge.create(doc, name, HTML, null, is, true, registry);
}
const inertDocument = realm => wrapper(realm, {type: 9, window: false, registry: -1, html: true});

// Window and Document registries.
check(T.customElements instanceof T.CustomElementRegistry, 'customElements is a CustomElementRegistry');
check(run(top, 'document.customElementRegistry') === T.customElements, 'the Window Document has the global registry');
const plain = createElement(top, top.document, 'a-b');
check(run(top, 'Object.getOwnPropertyDescriptor(Element.prototype,"customElementRegistry")').get.call(plain) === T.customElements,
    'Element.customElementRegistry of a created element');
check(T.customElementsBridge.registryFor(createElement(top, inertDocument(top), 'a-b')) === null,
    'an element of a document without a browsing context has a null registry');

// flatten element creation options.
const scoped = run(top, 'new CustomElementRegistry()');
check(T.customElementsBridge.registryFor(createElement(top, top.document, 'a-b', {customElementRegistry: scoped})) === scoped,
    'customElementRegistry option sets a scoped registry');
check(T.customElementsBridge.registryFor(createElement(top, top.document, 'a-b', {customElementRegistry: null})) === null,
    'a null customElementRegistry option gives a null registry');
throws(() => createElement(top, top.document, 'a-b', {is: 'x-y', customElementRegistry: scoped}), 'is with customElementRegistry', 'NotSupportedError');
throws(() => createElement(top, top.document, 'a-b', {customElementRegistry: F.customElements}), 'a global registry of another document', 'NotSupportedError');
throws(() => createElement(top, top.document, 'a-b', {customElementRegistry: {}}), 'a non-registry customElementRegistry', 'TypeError');
check(T.customElementsBridge.registryFor(createElement(top, top.document, 'a-b', 'x-y')) === T.customElements, 'a string option');
throws(() => createElement(top, top.document, 'a-b', Symbol()), 'a Symbol option', 'TypeError');

// Cross-realm identities: one agent for both Windows.
const frameElement = createElement(frame, frame.document, 'a-b');
check(T.customElementsBridge.registryFor(frameElement) === F.customElements, 'another realm reads the iframe global registry object');
check(F.customElementsBridge.registryFor(frameElement) === F.customElements, 'the iframe reads its own global registry');
const frameScoped = createElement(frame, frame.document, 'c-d', {customElementRegistry: scoped});
check(F.customElementsBridge.registryFor(frameScoped) === scoped, 'a scoped registry of the window used in the iframe document');
check(T.customElementsBridge.registryFor(frameScoped) === scoped, 'the same scoped registry read back in the window');

// Scoped definitions upgrade connected elements of the scoped document set only.
{
    const registry = run(top, 'new CustomElementRegistry()');
    const candidate = createElement(top, top.document, 'test-element-1', {customElementRegistry: registry});
    insert(top, top.body, candidate);
    const globalOne = createElement(top, top.document, 'test-element-1');
    insert(top, top.body, globalOne);
    run(top, 'globalThis.TE1 = class extends HTMLElement {}');
    registry.define('test-element-1', T.TE1);
    check(candidate instanceof T.TE1, 'define on a scoped registry upgrades its connected element');
    check(!(globalOne instanceof T.TE1), 'define on a scoped registry leaves the document tree scope');
    const disconnected = createElement(top, top.document, 'test-element-2', {customElementRegistry: registry});
    run(top, 'globalThis.TE2 = class extends HTMLElement {}');
    registry.define('test-element-2', T.TE2);
    check(!(disconnected instanceof T.TE2), 'define does not upgrade a disconnected element');
    insert(top, top.body, disconnected);
    check(disconnected instanceof T.TE2, 'insertion upgrades with the scoped definition');
    // A shadow root of the scoped registry in the iframe document joins the scoped document set.
    const frameHost = createElement(frame, frame.document, 'div');
    const frameShadow = attachShadow(frameHost, registry);
    check(F.customElementsBridge.registryFor(frameShadow) === registry, 'attachShadow with a scoped registry');
    const node = createElement(frame, frame.document, 'test-element-3', {customElementRegistry: registry});
    insert(frame, frameShadow, node);
    insert(frame, frame.body, frameHost);
    run(top, 'globalThis.TE3 = class extends HTMLElement {}');
    registry.define('test-element-3', T.TE3);
    check(node instanceof T.TE3, 'a scoped definition upgrades in every document of its scoped document set');
    throws(() => run(top, 'new CustomElementRegistry()').define('x-button', class extends T.HTMLElement {}, {extends: 'button'}),
        'a scoped registry has no customized built-ins', 'NotSupportedError');
}

// Inserting into another realm's document: the global registry stays the element's own.
{
    run(frame, 'globalThis.AB = class extends HTMLElement {}');
    F.customElements.define('frame-ab', F.AB);
    const fromTop = createElement(top, top.document, 'frame-ab');
    check(T.customElementsBridge.registryFor(fromTop) === T.customElements, 'created with the window registry');
    const madeInFrame = createElement(frame, frame.document, 'frame-ab');
    check(madeInFrame instanceof F.AB, 'the iframe definition creates its element synchronously');
    check(F.customElementsBridge.registryFor(madeInFrame) === F.customElements, 'the created element has the iframe registry');
}

// initialize.
{
    const registry = run(top, 'new CustomElementRegistry()');
    const doc = inertDocument(top);
    const container = createElement(top, doc, 'div');
    const first = createElement(top, doc, 'init-ab'), second = createElement(top, doc, 'init-ab');
    insert(top, container, first); insert(top, container, second);
    const seen = [];
    T.seen = seen;
    run(top, 'globalThis.InitAB = class extends HTMLElement { constructor() { super(); seen.push(customElementsBridge.registryFor(this)); } }');
    registry.define('init-ab', T.InitAB);
    check(T.customElementsBridge.registryFor(first) === null, 'null before initialize');
    registry.initialize(container);
    check(T.customElementsBridge.registryFor(container) === registry && T.customElementsBridge.registryFor(second) === registry, 'initialize sets null registries');
    check(first instanceof T.InitAB && second instanceof T.InitAB, 'initialize upgrades in tree order');
    check(seen.length === 2 && seen[0] === registry && seen[1] === registry, 'the constructor sees the initialized registry');
    throws(() => T.customElements.initialize(top.document), 'a global registry cannot initialize a Document', 'NotSupportedError');
    throws(() => T.customElements.initialize(createElement(top, doc, 'div')), 'a global registry initializes only its own Document', 'NotSupportedError');
    const otherDoc = inertDocument(top);
    run(top, 'new CustomElementRegistry()').initialize(otherDoc);
    check(T.customElementsBridge.registryFor(otherDoc) !== null, 'initialize sets a null Document registry');
}

// The active custom element constructor map: re-entry with another registry.
{
    const registry = run(top, 'new CustomElementRegistry()');
    T.state = {nested: null, again: true, createElement, top};
    run(top, `globalThis.Reentry = class extends HTMLElement { constructor() { super();
        if (state.again) { state.again = false; state.nested = state.createElement(state.top, document, 'global-reentry'); } } }`);
    T.customElements.define('global-reentry', T.Reentry);
    registry.define('scoped-reentry', T.Reentry);
    const element = createElement(top, top.document, 'scoped-reentry', {customElementRegistry: registry});
    check(element instanceof T.Reentry && R(element).localName === 'scoped-reentry', 'the outer construction keeps its registry');
    check(T.state.nested instanceof T.Reentry && R(T.state.nested).localName === 'global-reentry', 'the nested construction uses the global registry');
    check(T.customElementsBridge.registryFor(element) === registry, 'the scoped element has the scoped registry');
    // Before super(): the nested construction must restore the outer map entry.
    T.state.again = true;
    run(top, `globalThis.ReentryBefore = class extends HTMLElement { constructor() {
        if (state.again) { state.again = false; state.nested = state.createElement(state.top, document, 'global-reentry-before'); }
        super(); } }`);
    T.customElements.define('global-reentry-before', T.ReentryBefore);
    registry.define('scoped-reentry-before', T.ReentryBefore);
    const outer = createElement(top, top.document, 'scoped-reentry-before', {customElementRegistry: registry});
    check(outer instanceof T.ReentryBefore && R(outer).localName === 'scoped-reentry-before', 'the map entry is restored after a nested construction');
    check(R(T.state.nested).localName === 'global-reentry-before' && outer !== T.state.nested, 'the nested element before super()');
    // Direct construction of a global-only element during a scoped upgrade.
    run(top, 'globalThis.GlobalOnly = class extends HTMLElement {}');
    T.customElements.define('global-only-element', T.GlobalOnly);
    run(top, `globalThis.ScopedElement = class extends HTMLElement { constructor() { super(); state.direct = new GlobalOnly(); } }`);
    registry.define('scoped-element-1', T.ScopedElement);
    const scopedOne = createElement(top, top.document, 'scoped-element-1', {customElementRegistry: registry});
    check(scopedOne instanceof T.ScopedElement && T.state.direct instanceof T.GlobalOnly, 'new GlobalOnly() during a scoped construction');
    check(T.customElementsBridge.registryFor(T.state.direct) === T.customElements, 'a directly constructed element has the global registry');
}

// create an element: a constructor's result of another document is reported and fails.
{
    const registry = run(top, 'new CustomElementRegistry()');
    run(top, 'globalThis.Other = class extends HTMLElement {}');
    registry.define('other-doc', T.Other);
    const doc = inertDocument(top);
    const before = top.reported.length;
    const element = createElement(top, doc, 'other-doc', {customElementRegistry: registry});
    check(top.reported.length === before + 1 && top.reported[before].name === 'NotSupportedError', 'the other document result is reported');
    check(element instanceof T.HTMLUnknownElement && T.customElementsBridge.registryFor(element) === registry, 'the failed element is HTMLUnknownElement with the registry');
}

console.log(`custom element registries: ${passed} passed, ${failed} failed`);
process.exitCode = failed ? 1 : 0;
