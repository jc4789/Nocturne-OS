/* A small DOM for tests/check_editing.py: tree, live ranges (DOM's insert,
 * remove, replace data and split steps), one Selection, attributes, inline
 * style, a computed style from the UA rules the editing commands read, the
 * MutationObserver records the undo history reads, and an HTML fragment
 * parser and serializer for setting up and reading the cases.  It stands in
 * for the guest's native DOM so that the product's js_document_commands.js and
 * js_editing.js run on the host; it is not the product and proves nothing
 * about the native DOM (tests/js_editing_cases.js runs in the guest). */
'use strict';
var HTML_NS = 'http://www.w3.org/1999/xhtml';
var liveRanges = new Set(), observers = [];
function indexOf(node) { return node.parentNode ? node.parentNode.childNodes.indexOf(node) : -1; }
function nodeLength(node) { return node.nodeType === 3 || node.nodeType === 8 ? node._data.length : node.childNodes.length; }
function isInclusiveAncestor(a, n) { for (; n; n = n.parentNode) if (n === a) return true; return false; }
function rootOf(n) { while (n.parentNode) n = n.parentNode; return n; }
function domError(name) { var e = new Error(name); e.name = name; return e; }
function queueRecord(type, target, fields) {
    for (var o of observers) {
        if (!o.target || !isInclusiveAncestor(o.target, target)) continue;
        var options = o.options;
        if (target !== o.target && !options.subtree) continue;
        if (!options[type]) continue;
        o.records.push(Object.assign({type: type, target: target, addedNodes: [], removedNodes: [], previousSibling: null,
            nextSibling: null, attributeName: null, attributeNamespace: null, oldValue: null}, fields));
    }
}
function MiniMutationObserver(callback) { this.callback = callback; this.target = null; this.records = []; }
MiniMutationObserver.prototype.observe = function (target, options) { this.target = target; this.options = options; if (observers.indexOf(this) < 0) observers.push(this); };
MiniMutationObserver.prototype.takeRecords = function () { var r = this.records; this.records = []; return r; };
MiniMutationObserver.prototype.disconnect = function () { var i = observers.indexOf(this); if (i >= 0) observers.splice(i, 1); this.records = []; this.target = null; };

// Comparing boundary points (DOM).
function comparePoints(nodeA, offsetA, nodeB, offsetB) {
    if (nodeA === nodeB) return offsetA === offsetB ? 0 : offsetA < offsetB ? -1 : 1;
    var position = nodeA.compareDocumentPosition(nodeB);
    if (position & 4) {  // B follows A
        if (position & 16) {  // B is a descendant of A
            var child = nodeB; while (child.parentNode !== nodeA) child = child.parentNode;
            return indexOf(child) < offsetA ? 1 : -1;
        }
        return -1;
    }
    return -comparePoints(nodeB, offsetB, nodeA, offsetA);
}

function MiniNode(doc, type) { this.ownerDocument = doc; this.nodeType = type; this.parentNode = null; this.childNodes = []; }
MiniNode.ELEMENT_NODE = 1; MiniNode.TEXT_NODE = 3; MiniNode.COMMENT_NODE = 8; MiniNode.DOCUMENT_NODE = 9; MiniNode.DOCUMENT_FRAGMENT_NODE = 11;
MiniNode.DOCUMENT_POSITION_DISCONNECTED = 1; MiniNode.DOCUMENT_POSITION_PRECEDING = 2; MiniNode.DOCUMENT_POSITION_FOLLOWING = 4;
MiniNode.DOCUMENT_POSITION_CONTAINS = 8; MiniNode.DOCUMENT_POSITION_CONTAINED_BY = 16; MiniNode.DOCUMENT_POSITION_IMPLEMENTATION_SPECIFIC = 32;
Object.assign(MiniNode.prototype, {ELEMENT_NODE: 1, TEXT_NODE: 3, COMMENT_NODE: 8, DOCUMENT_NODE: 9, DOCUMENT_FRAGMENT_NODE: 11});
Object.defineProperties(MiniNode.prototype, {
    firstChild: {get: function () { return this.childNodes[0] || null; }},
    lastChild: {get: function () { return this.childNodes[this.childNodes.length - 1] || null; }},
    previousSibling: {get: function () { var i = indexOf(this); return i > 0 ? this.parentNode.childNodes[i - 1] : null; }},
    nextSibling: {get: function () { var i = indexOf(this); return i >= 0 && i + 1 < this.parentNode.childNodes.length ? this.parentNode.childNodes[i + 1] : null; }},
    parentElement: {get: function () { return this.parentNode && this.parentNode.nodeType === 1 ? this.parentNode : null; }},
    isConnected: {get: function () { return rootOf(this).nodeType === 9; }},
    nodeValue: {get: function () { return this.nodeType === 3 || this.nodeType === 8 ? this._data : null; }},
    textContent: {
        get: function () {
            if (this.nodeType === 3 || this.nodeType === 8) return this._data;
            var s = ''; for (var n = this.firstChild; n; n = n.nextSibling) if (n.nodeType !== 8) s += n.textContent; return s;
        },
        set: function (v) {
            while (this.firstChild) this.removeChild(this.firstChild);
            if (v !== '') this.appendChild(this.ownerDocument.createTextNode(String(v)));
        }},
    nodeName: {get: function () { return this.nodeType === 1 ? this.tagName : this.nodeType === 3 ? '#text' : this.nodeType === 8 ? '#comment' : this.nodeType === 9 ? '#document' : '#document-fragment'; }},
});
MiniNode.prototype.hasChildNodes = function () { return this.childNodes.length > 0; };
MiniNode.prototype.contains = function (o) { return isInclusiveAncestor(this, o); };
MiniNode.prototype.compareDocumentPosition = function (other) {
    if (other === this) return 0;
    if (rootOf(other) !== rootOf(this)) return 1 | 32 | 2;
    if (isInclusiveAncestor(other, this)) return 8 | 2;
    if (isInclusiveAncestor(this, other)) return 16 | 4;
    var a = [], b = []; for (var n = this; n; n = n.parentNode) a.unshift(n); for (n = other; n; n = n.parentNode) b.unshift(n);
    var i = 0; while (a[i] === b[i]) i++;
    return indexOf(a[i]) < indexOf(b[i]) ? 4 : 2;
};
// DOM "remove": live ranges first, then the record.
function removeNode(node, suppress) {
    var parent = node.parentNode, index = indexOf(node);
    for (var r of liveRanges) {
        if (isInclusiveAncestor(node, r.startContainer)) { r.startContainer = parent; r.startOffset = index; }
        if (isInclusiveAncestor(node, r.endContainer)) { r.endContainer = parent; r.endOffset = index; }
        if (r.startContainer === parent && r.startOffset > index) r.startOffset--;
        if (r.endContainer === parent && r.endOffset > index) r.endOffset--;
    }
    var previous = node.previousSibling, next = node.nextSibling;
    parent.childNodes.splice(index, 1); node.parentNode = null;
    if (!suppress) queueRecord('childList', parent, {removedNodes: [node], previousSibling: previous, nextSibling: next});
}
MiniNode.prototype.insertBefore = function (node, child) {
    if (child === undefined) child = null;
    if (child !== null && child.parentNode !== this) throw domError('NotFoundError');
    if (isInclusiveAncestor(node, this)) throw domError('HierarchyRequestError');
    var nodes = node.nodeType === 11 ? node.childNodes.slice() : [node];
    if (child === node) child = node.nextSibling;
    if (node.nodeType === 11) { for (var n of nodes) removeNode(n, true); if (nodes.length) queueRecord('childList', node, {removedNodes: nodes}); }
    else if (node.parentNode) removeNode(node);
    if (!nodes.length) return node;
    var index = child ? indexOf(child) : this.childNodes.length;
    for (var r of liveRanges) {
        if (r.startContainer === this && r.startOffset > index) r.startOffset += nodes.length;
        if (r.endContainer === this && r.endOffset > index) r.endOffset += nodes.length;
    }
    var previous = index > 0 ? this.childNodes[index - 1] : null;
    for (var i = 0; i < nodes.length; i++) { this.childNodes.splice(index + i, 0, nodes[i]); nodes[i].parentNode = this; }
    queueRecord('childList', this, {addedNodes: nodes, previousSibling: previous, nextSibling: child});
    return node;
};
MiniNode.prototype.appendChild = function (node) { return this.insertBefore(node, null); };
MiniNode.prototype.removeChild = function (node) { if (node.parentNode !== this) throw domError('NotFoundError'); removeNode(node); return node; };
MiniNode.prototype.replaceChild = function (node, old) { var next = old.nextSibling === node ? node.nextSibling : old.nextSibling; this.removeChild(old); this.insertBefore(node, next); return old; };
MiniNode.prototype.cloneNode = function (deep) {
    var copy;
    if (this.nodeType === 1) { copy = this.ownerDocument.createElement(this.localName); for (var a of this.attributes) copy.setAttribute(a.name, a.value); }
    else if (this.nodeType === 3) copy = this.ownerDocument.createTextNode(this._data);
    else if (this.nodeType === 8) copy = this.ownerDocument.createComment(this._data);
    else copy = this.ownerDocument.createDocumentFragment();
    if (deep) for (var n of this.childNodes) copy.appendChild(n.cloneNode(true));
    return copy;
};

function MiniText(doc, data) { MiniNode.call(this, doc, 3); this._data = data; }
MiniText.prototype = Object.create(MiniNode.prototype);
function MiniComment(doc, data) { MiniNode.call(this, doc, 8); this._data = data; }
MiniComment.prototype = Object.create(MiniNode.prototype);
// DOM "replace data".
function replaceData(node, offset, count, data) {
    var length = node._data.length; if (offset > length) throw domError('IndexSizeError');
    if (offset + count > length) count = length - offset;
    var old = node._data;
    node._data = old.slice(0, offset) + data + old.slice(offset + count);
    queueRecord('characterData', node, {oldValue: old});
    for (var r of liveRanges) {
        if (r.startContainer === node && r.startOffset > offset && r.startOffset <= offset + count) r.startOffset = offset;
        if (r.endContainer === node && r.endOffset > offset && r.endOffset <= offset + count) r.endOffset = offset;
        if (r.startContainer === node && r.startOffset > offset + count) r.startOffset += data.length - count;
        if (r.endContainer === node && r.endOffset > offset + count) r.endOffset += data.length - count;
    }
}
[MiniText, MiniComment].forEach(function (C) {
    Object.defineProperties(C.prototype, {
        data: {get: function () { return this._data; }, set: function (v) { replaceData(this, 0, this._data.length, String(v)); }},
        length: {get: function () { return this._data.length; }},
    });
    C.prototype.insertData = function (offset, data) { replaceData(this, offset, 0, String(data)); };
    C.prototype.deleteData = function (offset, count) { replaceData(this, offset, count, ''); };
    C.prototype.appendData = function (data) { replaceData(this, this._data.length, 0, String(data)); };
    C.prototype.replaceData = function (offset, count, data) { replaceData(this, offset, count, String(data)); };
    C.prototype.substringData = function (offset, count) { return this._data.substr(offset, count); };
});
// DOM "split a Text node".
MiniText.prototype.splitText = function (offset) {
    var length = this._data.length; if (offset > length) throw domError('IndexSizeError');
    var node = this.ownerDocument.createTextNode(this._data.slice(offset)), parent = this.parentNode;
    if (parent) {
        parent.insertBefore(node, this.nextSibling);
        var index = indexOf(this);
        for (var r of liveRanges) {
            if (r.startContainer === this && r.startOffset > offset) { r.startContainer = node; r.startOffset -= offset; }
            if (r.endContainer === this && r.endOffset > offset) { r.endContainer = node; r.endOffset -= offset; }
            if (r.startContainer === parent && r.startOffset === index + 1) r.startOffset++;
            if (r.endContainer === parent && r.endOffset === index + 1) r.endOffset++;
        }
    }
    replaceData(this, offset, length - offset, '');
    return node;
};

// Inline style: the style attribute's declarations, in order.
var styleNames = {backgroundColor: 'background-color', fontWeight: 'font-weight', fontFamily: 'font-family', fontSize: 'font-size',
    color: 'color', fontStyle: 'font-style', textDecoration: 'text-decoration', textDecorationLine: 'text-decoration-line',
    verticalAlign: 'vertical-align', textAlign: 'text-align', whiteSpace: 'white-space', display: 'display', margin: 'margin',
    padding: 'padding', border: 'border', marginLeft: 'margin-left', marginRight: 'margin-right', direction: 'direction'};
function parseStyle(text) {
    var list = [];
    (text || '').split(';').forEach(function (part) {
        var colon = part.indexOf(':'); if (colon < 0) return;
        var name = part.slice(0, colon).trim().toLowerCase(), value = part.slice(colon + 1).trim();
        if (name && value) { list = list.filter(function (d) { return d[0] !== name; }); list.push([name, value]); }
    });
    return list;
}
function MiniStyle(element) {
    this._element = element;
    return new Proxy(this, {get: function (t, k) { return typeof k === 'string' && /^[0-9]+$/.test(k) ? (t._list()[+k] || [undefined])[0] : t[k]; }});
}
MiniStyle.prototype._list = function () { return parseStyle(this._element.getAttribute('style')); };
MiniStyle.prototype._write = function (list) { this._element.setAttribute('style', list.map(function (d) { return d[0] + ': ' + d[1] + ';'; }).join(' ')); };
MiniStyle.prototype.getPropertyValue = function (name) { var d = this._list().find(function (d) { return d[0] === name; }); return d ? d[1] : ''; };
MiniStyle.prototype.setProperty = function (name, value) {
    value = String(value).trim(); var list = this._list().filter(function (d) { return d[0] !== name; });
    if (value !== '') list.push([name, value]);
    this._write(list);
};
MiniStyle.prototype.removeProperty = function (name) { var old = this.getPropertyValue(name); if (old !== '') this.setProperty(name, ''); return old; };
Object.defineProperties(MiniStyle.prototype, {
    length: {get: function () { return this._list().length; }},
    cssText: {get: function () { return this._element.getAttribute('style') || ''; }, set: function (v) { this._element.setAttribute('style', v); }},
});
Object.keys(styleNames).forEach(function (camel) {
    Object.defineProperty(MiniStyle.prototype, camel, {
        get: function () { return this.getPropertyValue(styleNames[camel]); },
        set: function (v) { this.setProperty(styleNames[camel], v); }});
});

function MiniElement(doc, localName) {
    MiniNode.call(this, doc, 1); this.localName = localName; this.namespaceURI = HTML_NS; this.attributes = [];
    this.style = new MiniStyle(this);
}
MiniElement.prototype = Object.create(MiniNode.prototype);
Object.defineProperty(MiniElement.prototype, 'tagName', {get: function () { return this.localName.toUpperCase(); }});
MiniElement.prototype.getAttributeNS = function (ns, name) { var a = this.attributes.find(function (a) { return a.namespaceURI === ns && a.localName === name; }); return a ? a.value : null; };
MiniElement.prototype.getAttribute = function (name) { return this.getAttributeNS(null, String(name).toLowerCase()); };
MiniElement.prototype.hasAttribute = function (name) { return this.getAttribute(name) !== null; };
MiniElement.prototype.setAttributeNS = function (ns, name, value) {
    value = String(value); var a = this.attributes.find(function (a) { return a.namespaceURI === ns && a.localName === name; });
    queueRecord('attributes', this, {attributeName: name, attributeNamespace: ns, oldValue: a ? a.value : null});
    if (a) a.value = value; else this.attributes.push({namespaceURI: ns, localName: name, name: name, prefix: null, value: value});
};
MiniElement.prototype.setAttribute = function (name, value) { this.setAttributeNS(null, String(name).toLowerCase(), value); };
MiniElement.prototype.removeAttributeNS = function (ns, name) {
    var i = this.attributes.findIndex(function (a) { return a.namespaceURI === ns && a.localName === name; }); if (i < 0) return;
    queueRecord('attributes', this, {attributeName: name, attributeNamespace: ns, oldValue: this.attributes[i].value});
    this.attributes.splice(i, 1);
};
MiniElement.prototype.removeAttribute = function (name) { this.removeAttributeNS(null, String(name).toLowerCase()); };
['color', 'face', 'size', 'href', 'id'].forEach(function (name) {
    Object.defineProperty(MiniElement.prototype, name, {get: function () { return this.getAttribute(name) || ''; },
        set: function (v) { this.setAttribute(name, v); }});
});

var voidElements = ['area', 'base', 'br', 'col', 'embed', 'hr', 'img', 'input', 'link', 'meta', 'source', 'track', 'wbr'];
function serialize(node) {
    var s = '';
    for (var n of node.childNodes) {
        if (n.nodeType === 3) s += n._data.replace(/&/g, '&amp;').replace(/ /g, '&nbsp;').replace(/</g, '&lt;').replace(/>/g, '&gt;');
        else if (n.nodeType === 8) s += '<!--' + n._data + '-->';
        else if (n.nodeType === 1) {
            s += '<' + n.localName + n.attributes.map(function (a) { return ' ' + a.name + '="' + a.value.replace(/&/g, '&amp;').replace(/"/g, '&quot;') + '"'; }).join('') + '>';
            if (voidElements.indexOf(n.localName) < 0) s += serialize(n) + '</' + n.localName + '>';
        }
    }
    return s;
}
function decode(text) { return text.replace(/&nbsp;/g, ' ').replace(/&lt;/g, '<').replace(/&gt;/g, '>').replace(/&quot;/g, '"').replace(/&amp;/g, '&'); }
function parseFragment(doc, html) {
    var fragment = doc.createDocumentFragment(), stack = [fragment], re = /<!--([\s\S]*?)-->|<\/([a-zA-Z0-9]+)\s*>|<([a-zA-Z0-9]+)((?:\s+[a-zA-Z-]+(?:\s*=\s*(?:"[^"]*"|'[^']*'|[^\s>]+))?)*)\s*\/?>|([^<]+)/g, m;
    while ((m = re.exec(html))) {
        var top = stack[stack.length - 1];
        if (m[1] !== undefined) top.appendChild(doc.createComment(m[1]));
        else if (m[2]) { var name = m[2].toLowerCase(); for (var i = stack.length - 1; i > 0; i--) if (stack[i].localName === name) { stack.length = i; break; } }
        else if (m[3]) {
            var e = doc.createElement(m[3].toLowerCase()), are = /([a-zA-Z-]+)(?:\s*=\s*(?:"([^"]*)"|'([^']*)'|([^\s>]+)))?/g, a;
            while ((a = are.exec(m[4]))) e.setAttribute(a[1], decode(a[2] !== undefined ? a[2] : a[3] !== undefined ? a[3] : a[4] !== undefined ? a[4] : ''));
            top.appendChild(e); if (voidElements.indexOf(e.localName) < 0) stack.push(e);
        } else top.appendChild(doc.createTextNode(decode(m[5])));
    }
    return fragment;
}
Object.defineProperty(MiniElement.prototype, 'innerHTML', {
    get: function () { return serialize(this); },
    set: function (html) { while (this.firstChild) this.removeChild(this.firstChild); this.appendChild(parseFragment(this.ownerDocument, String(html))); }});

// Computed style: the UA rules the editing commands read, inherited properties,
// presentational hints of font, and the inline style.
var blockNames = ['address', 'article', 'aside', 'blockquote', 'body', 'center', 'dd', 'details', 'dialog', 'dir', 'div', 'dl', 'dt',
    'fieldset', 'figcaption', 'figure', 'footer', 'form', 'h1', 'h2', 'h3', 'h4', 'h5', 'h6', 'header', 'hgroup', 'hr', 'html',
    'legend', 'listing', 'main', 'menu', 'nav', 'ol', 'p', 'plaintext', 'pre', 'section', 'summary', 'ul', 'xmp'];
var hiddenNames = ['head', 'script', 'style', 'template', 'title', 'meta', 'link'];
var namedColors = {black: [0, 0, 0], white: [255, 255, 255], red: [255, 0, 0], lime: [0, 255, 0], green: [0, 128, 0], blue: [0, 0, 255],
    yellow: [255, 255, 0], aqua: [0, 255, 255], orange: [255, 165, 0], purple: [128, 0, 128], gray: [128, 128, 128], grey: [128, 128, 128],
    navy: [0, 0, 128], silver: [192, 192, 192], tan: [210, 180, 140], brown: [165, 42, 42]};
// A CSS color as getComputedStyle() serializes it; null if it is not one.
function colorText(value) {
    value = String(value).trim().toLowerCase(); var m, rgb, alpha = 1;
    if (value === 'transparent') return 'rgba(0, 0, 0, 0)';
    if (namedColors[value]) rgb = namedColors[value];
    else if ((m = /^#([0-9a-f]{3,4}|[0-9a-f]{6}|[0-9a-f]{8})$/.exec(value))) {
        var h = m[1]; if (h.length <= 4) h = h.split('').map(function (c) { return c + c; }).join('');
        rgb = [0, 2, 4].map(function (i) { return parseInt(h.substr(i, 2), 16); }); if (h.length === 8) alpha = parseInt(h.substr(6, 2), 16) / 255;
    } else if ((m = /^rgba?\(\s*([0-9]+)\s*,\s*([0-9]+)\s*,\s*([0-9]+)\s*(?:,\s*([0-9.]+)\s*)?\)$/.exec(value))) {
        rgb = [+m[1], +m[2], +m[3]].map(function (c) { return Math.min(255, c); }); if (m[4] !== undefined) alpha = Math.min(1, +m[4]);
    } else return null;
    return alpha === 1 ? 'rgb(' + rgb.join(', ') + ')' : 'rgba(' + rgb.join(', ') + ', ' + (Math.round(alpha * 1000) / 1000) + ')';
}
var fontKeywords = [9, 10, 13, 16, 18, 24, 32, 48], keywordNames = ['xx-small', 'x-small', 'small', 'medium', 'large', 'x-large', 'xx-large', 'xxx-large'];
function computedStyle(element) {
    function own(name) { return element.style.getPropertyValue(name); }
    function inherited(name, fallback) { var p = element.parentNode; return p && p.nodeType === 1 ? computedStyle(p)[name] : fallback; }
    var tag = element.localName, s = {};
    s.display = own('display') || (hiddenNames.indexOf(tag) >= 0 ? 'none' : tag === 'li' ? 'list-item' : blockNames.indexOf(tag) >= 0 ? 'block'
        : tag === 'table' ? 'table' : tag === 'tr' ? 'table-row' : tag === 'td' || tag === 'th' ? 'table-cell' : 'inline');
    s.whiteSpace = own('white-space') || (['pre', 'listing', 'xmp', 'plaintext'].indexOf(tag) >= 0 ? 'pre' : tag === 'nobr' ? 'nowrap' : inherited('whiteSpace', 'normal'));
    var weight = own('font-weight') || (['b', 'strong', 'th', 'h1', 'h2', 'h3', 'h4', 'h5', 'h6'].indexOf(tag) >= 0 ? 'bold' : '');
    s.fontWeight = weight === 'bold' ? '700' : weight === 'normal' ? '400' : weight || inherited('fontWeight', '400');
    s.fontStyle = own('font-style') || (['i', 'em', 'cite', 'var', 'dfn', 'address'].indexOf(tag) >= 0 ? 'italic' : inherited('fontStyle', 'normal'));
    s.textDecoration = own('text-decoration') || own('text-decoration-line') || (['u', 'ins'].indexOf(tag) >= 0 ? 'underline'
        : ['s', 'strike', 'del'].indexOf(tag) >= 0 ? 'line-through' : 'none');
    s.verticalAlign = own('vertical-align') || (tag === 'sub' ? 'sub' : tag === 'sup' ? 'super' : 'baseline');
    s.textAlign = own('text-align') || (tag === 'center' ? 'center' : tag === 'th' ? 'center' : inherited('textAlign', 'start'));
    s.backgroundColor = colorText(own('background-color')) || 'rgba(0, 0, 0, 0)';
    s.color = colorText(own('color')) || (tag === 'font' && colorText(element.getAttribute('color') || '')) || inherited('color', 'rgb(0, 0, 0)');
    s.fontFamily = own('font-family') || (tag === 'font' && element.getAttribute('face')) || (['tt', 'code', 'pre', 'kbd', 'samp'].indexOf(tag) >= 0 ? 'monospace' : inherited('fontFamily', 'serif'));
    var size = own('font-size'), px = null;
    if (keywordNames.indexOf(size) >= 0) px = fontKeywords[keywordNames.indexOf(size)];
    else if (/^[0-9.]+px$/.test(size)) px = parseFloat(size);
    else if (tag === 'font' && element.hasAttribute('size')) px = fontKeywords[Math.min(7, Math.max(1, parseInt(element.getAttribute('size'))))];
    s.fontSize = px !== null ? px + 'px' : inherited('fontSize', '16px');
    return s;
}

function MiniRange(doc) { this.startContainer = this.endContainer = doc; this.startOffset = this.endOffset = 0; liveRanges.add(this); }
Object.defineProperties(MiniRange.prototype, {
    collapsed: {get: function () { return this.startContainer === this.endContainer && this.startOffset === this.endOffset; }},
    commonAncestorContainer: {get: function () { var n = this.startContainer; while (!isInclusiveAncestor(n, this.endContainer)) n = n.parentNode; return n; }},
});
MiniRange.prototype.setStart = function (node, offset) {
    if (offset > nodeLength(node)) throw domError('IndexSizeError');
    this.startContainer = node; this.startOffset = offset;
    if (rootOf(node) !== rootOf(this.endContainer) || comparePoints(node, offset, this.endContainer, this.endOffset) > 0) { this.endContainer = node; this.endOffset = offset; }
};
MiniRange.prototype.setEnd = function (node, offset) {
    if (offset > nodeLength(node)) throw domError('IndexSizeError');
    this.endContainer = node; this.endOffset = offset;
    if (rootOf(node) !== rootOf(this.startContainer) || comparePoints(node, offset, this.startContainer, this.startOffset) < 0) { this.startContainer = node; this.startOffset = offset; }
};
MiniRange.prototype.setStartBefore = function (n) { this.setStart(n.parentNode, indexOf(n)); };
MiniRange.prototype.setStartAfter = function (n) { this.setStart(n.parentNode, indexOf(n) + 1); };
MiniRange.prototype.setEndBefore = function (n) { this.setEnd(n.parentNode, indexOf(n)); };
MiniRange.prototype.setEndAfter = function (n) { this.setEnd(n.parentNode, indexOf(n) + 1); };
MiniRange.prototype.collapse = function (toStart) { if (toStart) { this.endContainer = this.startContainer; this.endOffset = this.startOffset; } else { this.startContainer = this.endContainer; this.startOffset = this.endOffset; } };
MiniRange.prototype.selectNode = function (n) { var p = n.parentNode, i = indexOf(n); this.startContainer = this.endContainer = p; this.startOffset = i; this.endOffset = i + 1; };
MiniRange.prototype.selectNodeContents = function (n) { this.startContainer = this.endContainer = n; this.startOffset = 0; this.endOffset = nodeLength(n); };
MiniRange.prototype.cloneRange = function () { var r = new MiniRange(this.startContainer.ownerDocument || this.startContainer); r.startContainer = this.startContainer; r.startOffset = this.startOffset; r.endContainer = this.endContainer; r.endOffset = this.endOffset; return r; };
MiniRange.prototype.createContextualFragment = function (html) { return parseFragment(this.startContainer.ownerDocument || this.startContainer, html); };
MiniRange.prototype.toString = function () {
    var s = '', n;
    if (this.startContainer === this.endContainer && this.startContainer.nodeType === 3) return this.startContainer._data.slice(this.startOffset, this.endOffset);
    for (n = this.startContainer; n; n = nextInTree(n)) {
        if (n.nodeType === 3) {
            var a = n === this.startContainer ? this.startOffset : 0, b = n === this.endContainer ? this.endOffset : n._data.length;
            if (comparePoints(n, 0, this.endContainer, this.endOffset) >= 0) break;
            if (comparePoints(n, n._data.length, this.startContainer, this.startOffset) > 0) s += n._data.slice(a, b);
        }
    }
    return s;
};
function nextInTree(n) { if (n.firstChild) return n.firstChild; while (n && !n.nextSibling) n = n.parentNode; return n ? n.nextSibling : null; }
// DOM "insert" into a range.
MiniRange.prototype.insertNode = function (node) {
    var start = this.startContainer, offset = this.startOffset, reference, parent;
    if (start.nodeType === 3) reference = start; else reference = start.childNodes[offset] || null;
    parent = reference === null ? start : reference.parentNode;
    if (start.nodeType === 3) { reference = start.splitText(offset); parent = reference.parentNode; }
    if (node === reference) reference = reference.nextSibling;
    if (node.parentNode) node.parentNode.removeChild(node);
    var newOffset = reference === null ? nodeLength(parent) : indexOf(reference);
    newOffset += node.nodeType === 11 ? node.childNodes.length : 1;
    var collapsed = this.collapsed;
    parent.insertBefore(node, reference);
    if (collapsed) { this.endContainer = parent; this.endOffset = newOffset; }
};
// DOM "extract".
MiniRange.prototype.extractContents = function () {
    var doc = this.startContainer.ownerDocument || this.startContainer, fragment = doc.createDocumentFragment();
    if (this.collapsed) return fragment;
    var sc = this.startContainer, so = this.startOffset, ec = this.endContainer, eo = this.endOffset;
    if (sc === ec && (sc.nodeType === 3 || sc.nodeType === 8)) {
        var clone = sc.cloneNode(); clone._data = sc._data.slice(so, eo); fragment.appendChild(clone); sc.deleteData(so, eo - so); return fragment;
    }
    var common = this.commonAncestorContainer, firstPartial = null, lastPartial = null;
    if (!isInclusiveAncestor(sc, ec)) { firstPartial = sc; while (firstPartial.parentNode !== common) firstPartial = firstPartial.parentNode; }
    if (!isInclusiveAncestor(ec, sc)) { lastPartial = ec; while (lastPartial.parentNode !== common) lastPartial = lastPartial.parentNode; }
    var contained = common.childNodes.filter(function (c) {
        return comparePoints(c.parentNode, indexOf(c), sc, so) >= 0 && comparePoints(c.parentNode, indexOf(c) + 1, ec, eo) <= 0
            && c !== firstPartial && c !== lastPartial; });
    var newNode, newOffset;
    if (isInclusiveAncestor(sc, ec)) { newNode = sc; newOffset = so; }
    else { var ref = sc; while (ref.parentNode && !isInclusiveAncestor(ref.parentNode, ec)) ref = ref.parentNode; newNode = ref.parentNode; newOffset = indexOf(ref) + 1; }
    if (firstPartial && (firstPartial.nodeType === 3 || firstPartial.nodeType === 8)) {
        var c1 = sc.cloneNode(); c1._data = sc._data.slice(so); fragment.appendChild(c1); sc.deleteData(so, sc._data.length - so);
    } else if (firstPartial) {
        var c2 = firstPartial.cloneNode(); fragment.appendChild(c2);
        var sub = new MiniRange(doc); sub.setStart(sc, so); sub.setEnd(firstPartial, nodeLength(firstPartial));
        c2.appendChild(sub.extractContents()); liveRanges.delete(sub);
    }
    contained.forEach(function (c) { fragment.appendChild(c); });
    if (lastPartial && (lastPartial.nodeType === 3 || lastPartial.nodeType === 8)) {
        var c3 = ec.cloneNode(); c3._data = ec._data.slice(0, eo); fragment.appendChild(c3); ec.deleteData(0, eo);
    } else if (lastPartial) {
        var c4 = lastPartial.cloneNode(); fragment.appendChild(c4);
        var sub2 = new MiniRange(doc); sub2.setStart(lastPartial, 0); sub2.setEnd(ec, eo);
        c4.appendChild(sub2.extractContents()); liveRanges.delete(sub2);
    }
    this.startContainer = this.endContainer = newNode; this.startOffset = this.endOffset = newOffset;
    return fragment;
};

function MiniSelection(doc) { this._doc = doc; this._range = null; this._backward = false; }
Object.defineProperties(MiniSelection.prototype, {
    rangeCount: {get: function () { return this._range && this._range.startContainer.isConnected && this._range.endContainer.isConnected ? 1 : 0; }},
    anchorNode: {get: function () { return this._range ? (this._backward ? this._range.endContainer : this._range.startContainer) : null; }},
    anchorOffset: {get: function () { return this._range ? (this._backward ? this._range.endOffset : this._range.startOffset) : 0; }},
    focusNode: {get: function () { return this._range ? (this._backward ? this._range.startContainer : this._range.endContainer) : null; }},
    focusOffset: {get: function () { return this._range ? (this._backward ? this._range.startOffset : this._range.endOffset) : 0; }},
    isCollapsed: {get: function () { return !this._range || this._range.collapsed; }},
});
MiniSelection.prototype.getRangeAt = function (i) { if (i !== 0 || !this.rangeCount) throw domError('IndexSizeError'); return this._range; };
MiniSelection.prototype.addRange = function (r) { if (!this._range) { this._range = r; this._backward = false; } };
MiniSelection.prototype.removeAllRanges = function () { this._range = null; };
MiniSelection.prototype.collapse = function (node, offset) {
    if (node === null) { this._range = null; return; }
    var r = new MiniRange(this._doc); r.setStart(node, offset || 0); this._range = r; this._backward = false;
};
MiniSelection.prototype.setBaseAndExtent = function (a, ao, f, fo) {
    var r = new MiniRange(this._doc), backward = comparePoints(a, ao, f, fo) > 0;
    if (backward) { r.setStart(f, fo); r.setEnd(a, ao); } else { r.setStart(a, ao); r.setEnd(f, fo); }
    this._range = r; this._backward = backward;
};
MiniSelection.prototype.extend = function (node, offset) { if (!this._range) throw domError('InvalidStateError'); this.setBaseAndExtent(this.anchorNode, this.anchorOffset, node, offset || 0); };
MiniSelection.prototype.collapseToStart = function () { this.collapse(this._range.startContainer, this._range.startOffset); };
MiniSelection.prototype.collapseToEnd = function () { this.collapse(this._range.endContainer, this._range.endOffset); };
MiniSelection.prototype.selectAllChildren = function (n) { this.setBaseAndExtent(n, 0, n, n.childNodes.length); };
MiniSelection.prototype.toString = function () { return this._range ? this._range.toString() : ''; };

function MiniDocument() {
    MiniNode.call(this, null, 9); this.ownerDocument = null;
    var html = this.createElement('html'), head = this.createElement('head'), body = this.createElement('body');
    html.appendChild(head); html.appendChild(body); this.appendChild(html);
    this._selection = new MiniSelection(this);
}
MiniDocument.prototype = Object.create(MiniNode.prototype);
MiniDocument.prototype.createElement = function (name) { return new MiniElement(this, String(name).toLowerCase()); };
MiniDocument.prototype.createTextNode = function (data) { return new MiniText(this, String(data)); };
MiniDocument.prototype.createComment = function (data) { return new MiniComment(this, String(data)); };
MiniDocument.prototype.createDocumentFragment = function () { var f = new MiniNode(this, 11); return f; };
MiniDocument.prototype.createRange = function () { return new MiniRange(this); };
MiniDocument.prototype.getSelection = function () { return this._selection; };
Object.defineProperties(MiniDocument.prototype, {
    documentElement: {get: function () { return this.childNodes.find(function (n) { return n.nodeType === 1; }) || null; }},
    body: {get: function () { var h = this.documentElement; return h && h.childNodes.find(function (n) { return n.localName === 'body'; }) || null; }},
});
// Events: listeners on the target and, for a bubbling event, its ancestors.
MiniNode.prototype.addEventListener = function (type, listener) { ((this._listeners || (this._listeners = {}))[type] || (this._listeners[type] = [])).push(listener); };
MiniNode.prototype.removeEventListener = function (type, listener) { var l = this._listeners && this._listeners[type]; if (l && l.indexOf(listener) >= 0) l.splice(l.indexOf(listener), 1); };
function dispatchEvent(target, event) {
    event.target = target;
    for (var n = target; n; n = event.bubbles ? n.parentNode : null) {
        var listeners = n._listeners && n._listeners[event.type];
        if (listeners) listeners.slice().forEach(function (l) { l.call(n, event); });
    }
    return !event.defaultPrevented;
}
globalThis.miniDom = {MiniNode: MiniNode, MiniDocument: MiniDocument, MiniMutationObserver: MiniMutationObserver, MiniElement: MiniElement,
    MiniSelection: MiniSelection, dispatchEvent: dispatchEvent, parseFragment: parseFragment,
    computedStyle: computedStyle, colorText: colorText, fontKeywords: fontKeywords, serialize: serialize, liveRanges: liveRanges};
