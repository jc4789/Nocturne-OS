/* The names of js_bootstrap.js's scope that js_input_event.js and
 * js_document_commands.js read, over tests/editing_host_dom.js, so that those
 * two product files and the editing program run as they are on the host
 * (tests/check_editing.py).  The text-control path is native and has its own
 * check (tests/check_document_commands_native.py). */
'use strict';
const apply = Reflect.apply;
const Node = miniDom.MiniNode, Document = miniDom.MiniDocument, Selection = miniDom.MiniSelection;
const MutationObserver = miniDom.MiniMutationObserver, HTMLElement = miniDom.MiniElement;
globalThis.Node = Node; globalThis.Document = Document; globalThis.Selection = Selection;
globalThis.MutationObserver = MutationObserver; globalThis.HTMLElement = HTMLElement;
const document = new Document();
class DOMException extends Error { constructor(message, name) { super(message); this.name = name; } }
class Event {
    constructor(type, init = {}) {
        this.type = String(type); this.bubbles = !!init.bubbles; this.cancelable = !!init.cancelable;
        this.composed = !!init.composed; this.defaultPrevented = false;
    }
    preventDefault() { if (this.cancelable) this.defaultPrevented = true; }
    get isTrusted() { return !!(eventStates.get(this) || {}).isTrusted; }
}
class UIEvent extends Event { constructor(type, init = {}) { super(type, init); this.view = init.view || null; } }
const eventStates = new WeakMap();
function eventState(event) { let s = eventStates.get(event); if (!s) eventStates.set(event, s = {}); return s; }
const dispatched = [];
function dispatch(target, event) { dispatched.push(event); return miniDom.dispatchEvent(target, event); }
const documentBridge = {brand(value) { if (!(value instanceof Document)) throw new TypeError('Document receiver required'); return value; }};
let focused = null, nativeDesignMode = false;
const logs = [];
function dom(op, ...args) {
    if (op === 'focus') { focused = args[0]; return; }
    if (op === 'attrNS') {
        const [node, namespace, name, value] = args;
        if (value === null) node.removeAttributeNS(namespace, name); else node.setAttributeNS(namespace, name, value);
        return;
    }
    throw new Error('dom ' + op);
}
const rawDom = {
    get(node, key) { const value = node[key]; return value === undefined ? null : value; },
    attrNS(node, namespace, name) { return node.nodeType === 1 ? node.getAttributeNS(namespace, name) : null; },
    isNode(owner, node) { return node instanceof Node; },
    documentCommand(doc, mode, text, value) {
        if (mode === 'textControl') return false;
        if (mode === 'color') return miniDom.colorText(text);
        if (mode === 'fontKeywords') return miniDom.fontKeywords.slice();
        if (mode === 'designMode') { nativeDesignMode = !!value; return false; }
        throw new Error('documentCommand ' + mode);
    },
};
const host = {
    editing() { return editingProgram; },
    log(level, ...values) { logs.push([level, values.map(v => v && v.stack ? v + '\n' + v.stack : String(v)).join(' ')]); },
};
const styleToken = {};
class CSSStyleProperties { constructor(node) { return miniDom.computedStyle(node); } }
