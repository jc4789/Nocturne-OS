/* The editing commands: the execCommand draft's own implementation
 * (https://w3c.github.io/editing/docs/execCommand/; WPT's implementation.js,
 * 3-clause BSD, kept in third_party/editing with what changed listed in its
 * README.md), inside one function compiled apart from the bindings.
 * js_document_commands.js calls it once per realm, with its bridge, the first
 * time a page asks about or runs a command or the user edits an editing host,
 * and keeps the API it returns.
 *
 * After the draft's code come the steps around its actions: the input event at
 * the affected editing host (and, for the user's own edits, the cancelable
 * beforeinput first), the undo history that the undo and redo commands take,
 * and the user's caret moves. */
(function (bridge) {
'use strict';
// The names the draft's code reads, bound to this realm's own objects rather
// than to the globals a page can replace.
const document = bridge.document;
const Node = bridge.Node;
function getSelection() {
    return bridge.selection();
}
function getComputedStyle(element) {
    return bridge.computedStyle(element);
}
// The browser's CSS color parser: null when value is not a <color> or is
// currentcolor, else the color as getComputedStyle() serializes it.
function cssColor(value) {
    return bridge.color(String(value));
}
// The CSS engine's pixel sizes of the absolute-size keywords, xx-small to
// xxx-large.  HTML maps a font element's size 1 to 7 onto x-small to
// xxx-large.
const fontKeywordPixels = bridge.fontKeywords;
function legacyFontPixels(size) {
    return fontKeywordPixels[Math.min(7, Math.max(1, size))];
}
const absoluteSizeKeywords = ["xx-small", "x-small", "small", "medium", "large",
    "x-large", "xx-large", "xxx-large"];
const absoluteUnitPixels = {px: 1, in: 96, cm: 96 / 2.54, mm: 96 / 25.4,
    pt: 96 / 72, pc: 16};
// The computed font-size, in pixels, of an absolute-size keyword or an
// absolute length (CSS Values: the fixed ratios of the absolute units).
function fontSizePixels(size) {
    var keyword = absoluteSizeKeywords.indexOf(size);
    if (keyword != -1) {
        return fontKeywordPixels[keyword];
    }
    var match = /^([0-9]+(?:\.[0-9]+)?)(cm|mm|in|pt|pc|px)$/.exec(size);
    return match ? Number(match[1]) * absoluteUnitPixels[match[2]] : NaN;
}
// HTML's "editing host" and "editable" (js_document_commands.js), as the
// commands read them: less the option data of the controls that hold options.
// An option or optgroup element, wherever it is, and everything in a select or
// a datalist, are the control's data, not content the user edits: the user
// puts no caret in them, and a command that edited them would lose the user's
// choices (WPT editing/other/editing-around-select-element.tentative.html: no
// command changes them).  A select in an editable parent is still editable, so
// a command can remove it as a whole.
function isOptionData(node) {
    for (var n = node; n; n = n.parentNode) {
        if (n.nodeType == Node.ELEMENT_NODE && n.namespaceURI == "http://www.w3.org/1999/xhtml"
        && (n.localName == "option" || n.localName == "optgroup"
        || (n != node && (n.localName == "select" || n.localName == "datalist")))) {
            return true;
        }
    }
    return false;
}
// A select or datalist: a boundary point in it is among its option data.
function holdsOptionData(node) {
    return node && node.nodeType == Node.ELEMENT_NODE
        && node.namespaceURI == "http://www.w3.org/1999/xhtml"
        && (node.localName == "select" || node.localName == "datalist");
}
function editingHost(node) {
    return bridge.editingHost(node) && !holdsOptionData(node) && !isOptionData(node);
}
function editable(node) {
    return bridge.editable(node) && !isOptionData(node);
}

/* @editing-implementation */

///////////////////////////////////////////////////////////////////////////////
/////////////////// Nocturne: the steps around the actions ////////////////////
///////////////////////////////////////////////////////////////////////////////

// The draft's Miscellaneous commands: they fire no input event of the generic
// kind ("they should all have their own events").
var miscellaneousCommands = ["copy", "cut", "defaultparagraphseparator",
    "paste", "redo", "selectall", "stylewithcss", "undo", "usecss"];

// "To map an edit command to input type value" (the draft's table), and, for
// the commands the table leaves out, the Input Events inputType of the same
// intent (italic, underline, subscript, hiliteColor, removeFormat).  The
// others (unlink, fontSize, formatBlock, insertHTML, insertImage) map to "".
var inputTypes = Object.assign(Object.create(null), {
    backcolor: "formatBackColor", bold: "formatBold", createlink: "insertLink",
    fontname: "formatFontName", forecolor: "formatFontColor",
    strikethrough: "formatStrikeThrough", superscript: "formatSuperscript",
    "delete": "deleteContentBackward", forwarddelete: "deleteContentForward",
    indent: "formatIndent", inserthorizontalrule: "insertHorizontalRule",
    insertlinebreak: "insertLineBreak", insertorderedlist: "insertOrderedList",
    insertparagraph: "insertParagraph", inserttext: "insertText",
    insertunorderedlist: "insertUnorderedList",
    justifycenter: "formatJustifyCenter", justifyfull: "formatJustifyFull",
    justifyleft: "formatJustifyLeft", justifyright: "formatJustifyRight",
    outdent: "formatOutdent", cut: "deleteByCut", paste: "insertFromPaste",
    redo: "historyRedo", undo: "historyUndo",
    italic: "formatItalic", underline: "formatUnderline",
    subscript: "formatSubscript", hilitecolor: "formatBackColor",
    removeformat: "formatRemove",
});

// The input event's data, as Input Events defines it for the inputType: the
// inserted text, the link's URL, the font family, the serialized color.
function inputData(command, value) {
    switch (command) {
    case "inserttext":
    case "createlink":
    case "fontname":
        return value;
    case "forecolor":
    case "backcolor":
    case "hilitecolor":
        var color = getColorCommandValue(value);
        return color === null ? null : normalizeColor(color);
    }
    return null;
}

// The draft: "the editing host that is an inclusive ancestor of the active
// range's start node and end node, and is not the ancestor of any editing
// host that is an inclusive ancestor of the active range's start node and end
// node".
function getAffectedEditingHost() {
    var selection = getSelection();
    if (!selection || !selection.rangeCount) {
        return null;
    }
    for (var node = selection.getRangeAt(0).commonAncestorContainer; node; node = node.parentNode) {
        if (isEditingHost(node)) {
            return node;
        }
    }
    return null;
}

// HTML: an editing host in the Plaintext-Only state: "Only the element's raw
// text content is editable; rich formatting is disabled."  The editing host
// whose state decides is the outermost one of the affected editing host's run
// of editable ancestors: an editing host whose parent is editable is edited as
// part of its parent's (WPT editing/plaintext-only/
// nested-with-contenteditable-true.html, plaintext-only-in-designMode.html).
function getGoverningEditingHost() {
    var host = getAffectedEditingHost();
    while (host && host.parentNode
    && (isEditable(host.parentNode) || isEditingHost(host.parentNode))) {
        host = getEditingHostOf(host.parentNode);
    }
    return host;
}

function isPlaintextOnly() {
    var host = getGoverningEditingHost();
    return host !== null && bridge.editableState(host) == "plaintext-only";
}

// The commands a Plaintext-Only editing host takes besides the miscellaneous
// ones: those that edit its text and line breaks.  The others make or change
// markup (the inline formatting commands; formatBlock, indent, outdent, the
// list and justify commands, insertHorizontalRule, insertImage) and are not
// enabled there (WPT editing/plaintext-only/styling-commands.html).
var plainTextCommands = Object.assign(Object.create(null), {
    "delete": true, forwarddelete: true, inserthtml: true,
    insertlinebreak: true, insertparagraph: true, inserttext: true,
});

// The selection's range, not the draft's cached active range.
function selectionRange() {
    var selection = getSelection();
    return selection && selection.rangeCount ? selection.getRangeAt(0) : null;
}

// Whether white-space keeps the line feeds of node's text (CSS Text:
// white-space-collapse preserve, preserve-breaks or break-spaces).
function keepsLineFeeds(node) {
    var element = node.nodeType == Node.ELEMENT_NODE ? node : node.parentNode;
    if (!element || element.nodeType != Node.ELEMENT_NODE) {
        return false;
    }
    var style = getComputedStyle(element);
    var collapse = style.whiteSpaceCollapse;
    if (typeof collapse == "string" && collapse != "") {
        return ["preserve", "preserve-breaks", "break-spaces"].indexOf(collapse) != -1;
    }
    return ["pre", "pre-wrap", "pre-line", "break-spaces"].indexOf(style.whiteSpace) != -1;
}

// Whether something after (node, offset) in its block makes a line after a
// line break there: text, a br or an img before the block ends.
function hasLineContentAfter(node, offset) {
    if (node.nodeType == Node.TEXT_NODE && offset < node.length) {
        return true;
    }
    var block = node.nodeType == Node.TEXT_NODE ? node.parentNode : node;
    while (!isBlockNode(block) && !isEditingHost(block) && block.parentNode) {
        block = block.parentNode;
    }
    var next = node.nodeType != Node.TEXT_NODE && offset < node.childNodes.length
        ? node.childNodes[offset] : nextNodeDescendants(node);
    for (; next && isDescendant(next, block); next = nextNode(next)) {
        if (isBlockNode(next)) {
            return false;
        }
        if ((next.nodeType == Node.TEXT_NODE && next.length && isVisible(next))
        || isHtmlElement(next, ["br", "img"])) {
            return true;
        }
    }
    return false;
}

// A line break in a Plaintext-Only editing host: where white-space keeps line
// feeds, a preformatted line feed, which the text content has; elsewhere, and
// for the padding an empty last line needs, a br (WPT
// editing/plaintext-only/insertLineBreak.html).  The draft's insertLineBreak
// otherwise.  pad false: text follows at once, so no padding.
function insertPlainLineBreak(pad) {
    deleteSelection({stripWrappers: false});
    var range = getActiveRange();
    var node = range.startContainer, offset = range.startOffset;
    if ((!isEditable(node) && !isEditingHost(node)) || !keepsLineFeeds(node)
    || (node.nodeType == Node.ELEMENT_NODE && !isAllowedChild("br", node))) {
        return commands.insertlinebreak.action("");
    }
    var text = node;
    if (node.nodeType == Node.TEXT_NODE) {
        node.insertData(offset, "\n");
        offset++;
    } else {
        text = document.createTextNode("\n");
        range.insertNode(text);
        offset = 1;
    }
    getSelection().collapse(text, offset);
    range.setStart(text, offset);
    range.setEnd(text, offset);
    if (pad !== false && !hasLineContentAfter(text, offset)) {
        text.parentNode.insertBefore(document.createElement("br"), text.nextSibling);
        getSelection().collapse(text, offset);
        range.setStart(text, offset);
        range.setEnd(text, offset);
    }
    return true;
}

// The line break right after (node, offset): a br, or a line feed that
// white-space keeps (as its Text node and its index there), or null.
function getLineBreakAfter(node, offset) {
    if (node.nodeType == Node.TEXT_NODE) {
        if (offset < node.length) {
            return node.data[offset] == "\n" && keepsLineFeeds(node)
                ? {node: node, index: offset} : null;
        }
        offset = getNodeIndex(node) + 1;
        node = node.parentNode;
    }
    var next = node ? node.childNodes[offset] : null;
    if (next && isHtmlElement(next, "br")) {
        return {node: next, index: -1};
    }
    if (next && next.nodeType == Node.TEXT_NODE && next.data[0] == "\n" && keepsLineFeeds(next)) {
        return {node: next, index: 0};
    }
    return null;
}

// Whether removing that line break would not change the layout: the draft's
// extraneous line break, or a kept line feed that ends its block's last line
// (like a br, it puts no line after it when nothing follows before the block
// ends) after something on that line.
function isExtraneousBreak(lineBreak) {
    if (lineBreak.index < 0) {
        return isExtraneousLineBreak(lineBreak.node);
    }
    var text = lineBreak.node, block = getLineBlockOf(text);
    if (lineBreak.index != text.length - 1 || hasLineContentBeside(text, block, true)) {
        return false;
    }
    return lineBreak.index > 0
        ? text.data[lineBreak.index - 1] != "\n"
        : hasLineContentBeside(text, block, false);
}

// Text in a Plaintext-Only editing host: the insertText action for each line,
// and a line break for each line feed, padded only when the last line is
// empty.  The line break after the text, when the text made it extraneous,
// is removed: the padding of the empty line the text now fills (a br or a
// kept line feed; WPT editing/plaintext-only/insertText.html), or the padding
// the draft's insertLineBreak put after a br that ended its block.  A line
// break that was extraneous before stays.
function insertPlainText(text) {
    if (!getActiveRange().collapsed) {
        deleteSelection({stripWrappers: false});
    }
    var range = getActiveRange();
    var before = getLineBreakAfter(range.startContainer, range.startOffset);
    var wasExtraneous = before != null && isExtraneousBreak(before);
    var lines = text.split("\n");
    for (var i = 0; i < lines.length; i++) {
        if (i) {
            insertPlainLineBreak(i == lines.length - 1 && lines[i] == "");
        }
        if (lines[i] != "" || lines.length == 1) {
            commands.inserttext.action(lines[i]);
        }
    }
    if (lines[lines.length - 1] == "") {
        return true;
    }
    range = getActiveRange();
    var after = getLineBreakAfter(range.endContainer, range.endOffset);
    if (!after || (wasExtraneous && after.node == before.node) || !isExtraneousBreak(after)) {
        return true;
    }
    if (after.index < 0) {
        after.node.parentNode.removeChild(after.node);
    } else {
        after.node.deleteData(after.index, 1);
    }
    return true;
}

// The text of markup, for a Plaintext-Only editing host: HTML's rendered text
// collection steps (the innerText getter's) over the markup, parsed in an
// inert document so that nothing of it loads or runs.  Nothing of it is
// rendered, so an element's display and white-space are its style
// attribute's, else the UA style sheet's (HTML's rendering section).  noscript
// is hidden: the document the text goes into has scripting (WPT
// editing/plaintext-only/insertHTML.html).
var markupHidden = ["area", "base", "basefont", "datalist", "head", "link",
    "meta", "noembed", "noframes", "param", "rp", "script", "style",
    "template", "title"];
var markupBlocks = ["address", "article", "aside", "blockquote", "body",
    "center", "dd", "details", "dialog", "dir", "div", "dl", "dt", "fieldset",
    "figcaption", "figure", "footer", "form", "h1", "h2", "h3", "h4", "h5",
    "h6", "header", "hgroup", "hr", "html", "legend", "listing", "main", "menu",
    "nav", "ol", "optgroup", "option", "p", "plaintext", "pre", "search",
    "section", "summary", "ul", "xmp"];

function getMarkupDisplay(element) {
    var name = element.localName;
    var html = element.namespaceURI == "http://www.w3.org/1999/xhtml";
    // The UA style sheet's !important rules first: no style attribute wins.
    if (html && (name == "noscript"
    || (name == "input" && /^hidden$/i.test(element.getAttribute("type") || "")))) {
        return "none";
    }
    var own = element.style ? element.style.getPropertyValue("display").trim().toLowerCase() : "";
    if (own) {
        return own;
    }
    if (!html) {
        return "inline";
    }
    if ((element.hasAttribute("hidden") && name != "embed")
    || markupHidden.indexOf(name) != -1
    || (name == "dialog" && !element.hasAttribute("open"))) {
        return "none";
    }
    return {li: "list-item", table: "table", caption: "table-caption",
        tr: "table-row", td: "table-cell", th: "table-cell"}[name]
        || (markupBlocks.indexOf(name) != -1 ? "block" : "inline");
}

function isBlockLevelDisplay(display) {
    return /^(block|list-item|table|flex|grid|flow-root)$/.test(display)
        || /^block\b/.test(display) || /\blist-item$/.test(display);
}

function getMarkupWhiteSpace(element, inherited) {
    var own = element.style ? element.style.getPropertyValue("white-space").trim().toLowerCase() : "";
    if (own) {
        return own;
    }
    if (isHtmlElement(element, ["listing", "plaintext", "pre", "xmp"])) {
        return "pre";
    }
    return isHtmlElement(element, "textarea") ? "pre-wrap" : inherited;
}

// "If node's CSS box is not the last 'table-cell' box of its enclosing
// 'table-row' box" / "not the last 'table-row' box of the nearest ancestor
// 'table' box".
function hasLaterMarkupBox(element, display, container) {
    for (var n = nextNodeDescendants(element); n && (!container || isDescendant(n, container)); n = nextNode(n)) {
        if (n.nodeType == Node.ELEMENT_NODE && getMarkupDisplay(n) == display) {
            return true;
        }
    }
    return false;
}

function collectMarkupText(node, whiteSpace, items) {
    if (node.nodeType == Node.TEXT_NODE) {
        items.push({text: node.data, whiteSpace: whiteSpace});
        return;
    }
    if (node.nodeType != Node.ELEMENT_NODE) {
        if (node.nodeType == Node.DOCUMENT_FRAGMENT_NODE) {
            for (var child = node.firstChild; child; child = child.nextSibling) {
                collectMarkupText(child, whiteSpace, items);
            }
        }
        return;
    }
    var display = getMarkupDisplay(node);
    if (display == "none") {
        return;
    }
    if (isHtmlElement(node, "br")) {
        items.push({text: "\n", lineEnd: true});
        return;
    }
    whiteSpace = getMarkupWhiteSpace(node, whiteSpace);
    var start = items.length;
    for (var child = node.firstChild; child; child = child.nextSibling) {
        collectMarkupText(child, whiteSpace, items);
    }
    if (display == "table-cell") {
        var row = node.parentNode;
        while (row && !(row.nodeType == Node.ELEMENT_NODE && getMarkupDisplay(row) == "table-row")) {
            row = row.parentNode;
        }
        if (hasLaterMarkupBox(node, "table-cell", row)) {
            items.push({text: "\t"});
        }
    }
    if (display == "table-row") {
        var table = node.parentNode;
        while (table && !(table.nodeType == Node.ELEMENT_NODE && getMarkupDisplay(table) == "table")) {
            table = table.parentNode;
        }
        if (hasLaterMarkupBox(node, "table-row", table)) {
            items.push({text: "\n", lineEnd: true});
        }
    }
    var count = isHtmlElement(node, "p") ? 2
        : isBlockLevelDisplay(display) || display == "table-caption" ? 1 : 0;
    if (count) {
        items.splice(start, 0, count);
        items.push(count);
    }
}

function getMarkupText(value) {
    var inert = document.implementation.createHTMLDocument("");
    var range = inert.createRange();
    range.selectNodeContents(inert.body);
    var items = [];
    collectMarkupText(range.createContextualFragment(value), "normal", items);

    // CSS white space processing: collapsible white space collapsed to one
    // space, and removed after another or at the start or end of a line.
    var results = [], lineStart = true, spaceAt = -1;
    function endLine() {
        if (spaceAt != -1) {
            results[spaceAt] = results[spaceAt].slice(0, -1);
        }
        spaceAt = -1;
        lineStart = true;
    }
    items.forEach(function(item) {
        if (typeof item == "number") {
            endLine();
            results.push(item);
            return;
        }
        if (item.lineEnd) {
            endLine();
            results.push("\n");
            return;
        }
        var text = item.text, mode = item.whiteSpace || "pre";
        if (mode == "pre" || mode == "pre-wrap" || mode == "break-spaces") {
            if (text != "") {
                results.push(text);
                spaceAt = -1;
                lineStart = text[text.length - 1] == "\n";
            }
            return;
        }
        var segments = mode == "pre-line"
            ? text.replace(/[\t\f\r ]*\n[\t\f\r ]*/g, "\n").split("\n") : [text];
        segments.forEach(function(segment, index) {
            if (index) {
                endLine();
                results.push("\n");
            }
            segment = segment.replace(/[\t\n\f\r ]+/g, " ");
            if (lineStart || spaceAt != -1) {
                segment = segment.replace(/^ /, "");
            }
            if (segment != "") {
                results.push(segment);
                lineStart = false;
                spaceAt = segment[segment.length - 1] == " " ? results.length - 1 : -1;
            }
        });
    });
    endLine();

    // The innerText getter: "Remove any items from results that are the empty
    // string.  Remove any runs of consecutive required line break count items
    // at the start or end of results.  Replace each remaining run of
    // consecutive required line break count items with a string consisting of
    // as many U+000A LF code points as the maximum of the values in the
    // required line break count items."
    results = results.filter(function(item) { return item !== ""; });
    while (typeof results[0] == "number") {
        results.shift();
    }
    while (typeof results[results.length - 1] == "number") {
        results.pop();
    }
    var text = "", count = 0;
    results.forEach(function(item) {
        if (typeof item == "number") {
            count = Math.max(count, item);
            return;
        }
        text += "\n".repeat(count) + item;
        count = 0;
    });
    return text;
}

// The action for command, as the active range's editing host takes it: the
// draft's, or in a Plaintext-Only editing host, text and line breaks in place
// of the markup the draft's insertHTML, insertLineBreak and insertText insert.
function act(command, showUi, value) {
    if ((command == "inserthtml" || command == "insertlinebreak" || command == "inserttext")
    && isPlaintextOnly()) {
        return editCommandMethod(command, undefined, function() {
            return command == "inserthtml" ? insertPlainText(getMarkupText(value))
                : command == "insertlinebreak" ? insertPlainLineBreak()
                : insertPlainText(value);
        });
    }
    return myExecCommand(command, showUi, value);
}

// The undo history: transactions of mutation records, each with the
// selection before and after it.  Undoing a transaction applies the inverse
// of its records, last first; the records that makes are the transaction redo
// applies the inverse of, and so on.  A node the page has moved or removed
// since is left where the page put it.
var undoStack = [], redoStack = [];
var recordingDepth = 0;

function startRecording() {
    var observer = new bridge.MutationObserver(function() {});
    observer.observe(document, {subtree: true, childList: true,
        attributes: true, attributeOldValue: true,
        characterData: true, characterDataOldValue: true});
    return observer;
}

function stopRecording(observer) {
    var records = observer.takeRecords();
    observer.disconnect();
    return records.map(function(record) {
        return {
            type: record.type,
            target: record.target,
            added: Array.from(record.addedNodes),
            removed: Array.from(record.removedNodes),
            previous: record.previousSibling,
            next: record.nextSibling,
            name: record.attributeName,
            namespace: record.attributeNamespace,
            oldValue: record.oldValue,
        };
    });
}

function revertRecords(records) {
    for (var i = records.length - 1; i >= 0; i--) {
        var record = records[i];
        try {
            if (record.type == "characterData") {
                record.target.data = record.oldValue === null ? "" : record.oldValue;
            } else if (record.type == "attributes") {
                if (record.oldValue === null) {
                    record.target.removeAttributeNS(record.namespace, record.name);
                } else {
                    record.target.setAttributeNS(record.namespace, record.name, record.oldValue);
                }
            } else {
                var parent = record.target;
                record.added.forEach(function(node) {
                    if (node.parentNode == parent) {
                        parent.removeChild(node);
                    }
                });
                // Where the removed nodes were: before the next sibling, or at
                // the end when there was none; after the previous sibling when
                // the next one has left; at the start when there was none.
                var reference;
                if (record.next === null || record.next.parentNode == parent) {
                    reference = record.next;
                } else if (record.previous !== null && record.previous.parentNode == parent) {
                    reference = record.previous.nextSibling;
                } else {
                    reference = record.previous === null ? parent.firstChild : null;
                }
                record.removed.forEach(function(node) {
                    if (!node.parentNode) {
                        parent.insertBefore(node, reference);
                    }
                });
            }
        } catch (error) {
            // A record the page's own changes since have made inapplicable.
        }
    }
}

function getSelectionPoints() {
    var selection = getSelection();
    if (!selection || !selection.rangeCount) {
        return null;
    }
    return [selection.anchorNode, selection.anchorOffset,
        selection.focusNode, selection.focusOffset];
}

function setSelectionPoints(points) {
    var selection = getSelection();
    if (!points || !selection) {
        return;
    }
    try {
        selection.setBaseAndExtent(points[0], Math.min(points[1], getNodeLength(points[0])),
            points[2], Math.min(points[3], getNodeLength(points[2])));
    } catch (error) {
        // A boundary node no longer in the document: the selection stays.
    }
}

function sameSelectionPoints(a, b) {
    return a && b && a[0] == b[0] && a[1] == b[1] && a[2] == b[2] && a[3] == b[3];
}

// A transaction for the history.  Consecutive typing is one transaction when
// each insertion starts where the last one left the selection.
function remember(records, before, editingHost, kind) {
    var after = getSelectionPoints();
    var last = undoStack[undoStack.length - 1];
    if (kind && last && last.kind == kind && last.editingHost == editingHost
    && sameSelectionPoints(last.after, before)) {
        last.records = last.records.concat(records);
        last.after = after;
    } else {
        undoStack.push({records: records, before: before, after: after,
            editingHost: editingHost, kind: kind});
    }
    redoStack.length = 0;
}

// Run an action while the history records what it changes.  A nested run
// (an action that runs another command) belongs to the outer transaction.
function recordAction(action, editingHost, kind) {
    var before = getSelectionPoints();
    var observer = recordingDepth ? null : startRecording();
    var result = false;
    recordingDepth++;
    try {
        result = action();
    } catch (error) {
        bridge.report(error);
        result = false;
    } finally {
        recordingDepth--;
    }
    if (!observer) {
        return {result: result, changed: result};
    }
    var records = stopRecording(observer);
    if (records.length) {
        remember(records, before, editingHost, kind);
    }
    return {result: result, changed: records.length != 0};
}

function isConnected(node) {
    return node && node.ownerDocument == document
        && (node.compareDocumentPosition(document) & Node.DOCUMENT_POSITION_CONTAINS) != 0;
}

// The undo and redo commands: "historyUndo" and "historyRedo" input events at
// the editing host the transaction was made in.
function travel(command) {
    var from = command == "undo" ? undoStack : redoStack;
    var to = command == "undo" ? redoStack : undoStack;
    if (!from.length) {
        return false;
    }
    var item = from.pop();
    var observer = startRecording();
    recordingDepth++;
    try {
        revertRecords(item.records);
    } catch (error) {
        bridge.report(error);
    } finally {
        recordingDepth--;
    }
    var records = stopRecording(observer);
    to.push({records: records, before: item.after, after: item.before,
        editingHost: item.editingHost, kind: null});
    setSelectionPoints(item.before);
    if (records.length && isConnected(item.editingHost)) {
        bridge.event(item.editingHost, "input", null, inputTypes[command]);
    }
    return true;
}

function guarded(query, fallback) {
    try {
        return query();
    } catch (error) {
        // With no active range, a command's state, indeterminacy and value
        // are the defaults (the draft's algorithms read the active range).
        globalRange = null;
        if (getActiveRange() !== null) {
            bridge.report(error);
        }
        return fallback;
    }
}

function supported(command) {
    return command in commands || command == "undo" || command == "redo";
}

function enabled(command) {
    if (command == "undo") {
        return undoStack.length != 0;
    }
    if (command == "redo") {
        return redoStack.length != 0;
    }
    return guarded(function() {
        if (command in commands && miscellaneousCommands.indexOf(command) == -1) {
            // A boundary point in a select or datalist is among its option
            // data, which no command edits (see isOptionData).
            var range = selectionRange();
            if (range && (holdsOptionData(range.startContainer) || holdsOptionData(range.endContainer))) {
                return false;
            }
            if (!plainTextCommands[command] && isPlaintextOnly()) {
                return false;
            }
        }
        return myQueryCommandEnabled(command);
    }, false);
}

// execCommand(command, showUI, value), after the bindings' steps.
//
// Input Events: "A user agent MUST NOT dispatch [beforeinput] due to events
// that are not caused by attempted user input", and a script's command is not
// one; the draft's step 4 fires it (see third_party/editing/README.md).  The
// input event follows the draft's step 7.
function exec(command, showUi, value) {
    if (command == "undo" || command == "redo") {
        return travel(command);
    }
    if (!(command in commands) || !enabled(command)) {
        return false;
    }
    globalRange = null;
    var miscellaneous = miscellaneousCommands.indexOf(command) != -1;
    var host = miscellaneous ? null : getAffectedEditingHost();
    var run = recordAction(function() {
        return act(command, showUi, value);
    }, host, null);
    if (run.result && run.changed && host) {
        bridge.event(host, "input", inputData(command, value), inputTypes[command] || "");
    }
    return run.result;
}

// The user's edits, by their Input Events inputType, and the command each one
// takes.
var userCommands = Object.assign(Object.create(null), {
    insertText: "inserttext", insertFromPaste: "inserttext",
    deleteContentBackward: "delete", deleteContentForward: "forwarddelete",
    deleteByCut: "delete", insertParagraph: "insertparagraph",
    insertLineBreak: "insertlinebreak", formatBold: "bold",
    formatItalic: "italic", formatUnderline: "underline",
});

// Input Events: beforeinput (cancelable) at the editing host, then, unless it
// was canceled, the edit and input.  True when the edit was the editing
// host's to take, done or not.
function userEdit(inputType, data) {
    globalRange = null;
    var host = getAffectedEditingHost();
    if (!host) {
        return false;
    }
    if (inputType == "historyUndo" || inputType == "historyRedo") {
        var command = inputType == "historyUndo" ? "undo" : "redo";
        if ((command == "undo" ? undoStack : redoStack).length
        && bridge.event(host, "beforeinput", null, inputType)) {
            travel(command);
        }
        return true;
    }
    var command = userCommands[inputType];
    if (!command) {
        return false;
    }
    if (isPlaintextOnly()) {
        // Enter in a Plaintext-Only editing host is a line break (WPT
        // editing/plaintext-only/insertLineBreak.html), and it takes no
        // formatting.
        if (inputType == "insertParagraph") {
            inputType = "insertLineBreak";
            command = "insertlinebreak";
        }
        if (!plainTextCommands[command]) {
            return true;
        }
    }
    var value = data === null ? "" : data.replace(/\r\n?/g, "\n");
    if (inputType == "deleteByCut" && getActiveRange().collapsed) {
        return true;
    }
    var eventData = inputType == "insertText" ? value : null;
    if (!bridge.event(host, "beforeinput", eventData, inputType)) {
        return true;
    }
    // "If command is not enabled, return false": the beforeinput handler may
    // have moved the selection.
    globalRange = null;
    if (!enabled(command)) {
        return true;
    }
    host = getAffectedEditingHost();
    var run = recordAction(function() {
        return act(command, false, value);
    }, host, inputType == "insertText" ? "typing" : null);
    if (run.changed && host && isConnected(host)) {
        bridge.event(host, "input", eventData, inputType);
    }
    return true;
}

// The caret moves of the editing keys: one character back or forward (a code
// point of a Text node, or a br or img), or to the start or end of the line
// in its block, which ends at a br.  Without layout, the line is the block's,
// and Up and Down do not move the caret.
function isCaretStop(node) {
    return (node.nodeType == Node.TEXT_NODE && node.length && !isCollapsedWhitespaceNode(node))
        || (isHtmlElement(node, "br") && !isExtraneousLineBreak(node))
        || isHtmlElement(node, "img");
}

function codeUnitsBefore(text, offset) {
    return offset > 1 && /[\udc00-\udfff]/.test(text[offset - 1])
        && /[\ud800-\udbff]/.test(text[offset - 2]) ? 2 : 1;
}

function codeUnitsAfter(text, offset) {
    return offset + 1 < text.length && /[\ud800-\udbff]/.test(text[offset])
        && /[\udc00-\udfff]/.test(text[offset + 1]) ? 2 : 1;
}

function stepBackward(node, offset, host) {
    if (node.nodeType == Node.TEXT_NODE && offset > 0) {
        return [node, offset - codeUnitsBefore(node.data, offset)];
    }
    // The last node that ends before the point.
    var current;
    if (node.nodeType != Node.TEXT_NODE && offset > 0) {
        current = node.childNodes[offset - 1];
        while (current.lastChild) {
            current = current.lastChild;
        }
    } else {
        current = previousNode(node);
    }
    for (; current && isDescendant(current, host); current = previousNode(current)) {
        if (!isCaretStop(current)) {
            continue;
        }
        if (current.nodeType == Node.TEXT_NODE) {
            return [current, current.length - codeUnitsBefore(current.data, current.length)];
        }
        return [current.parentNode, getNodeIndex(current)];
    }
    return null;
}

function stepForward(node, offset, host) {
    if (node.nodeType == Node.TEXT_NODE && offset < node.length) {
        return [node, offset + codeUnitsAfter(node.data, offset)];
    }
    // The first node that starts after the point.
    var current = node.nodeType != Node.TEXT_NODE && offset < node.childNodes.length
        ? node.childNodes[offset]
        : nextNodeDescendants(node);
    for (; current && isDescendant(current, host); current = nextNode(current)) {
        if (!isCaretStop(current)) {
            continue;
        }
        if (current.nodeType == Node.TEXT_NODE) {
            return [current, codeUnitsAfter(current.data, 0)];
        }
        return [current.parentNode, getNodeIndex(current) + 1];
    }
    return null;
}

function lineEdge(node, offset, host, backward) {
    var block = node.nodeType == Node.TEXT_NODE ? node.parentNode : node;
    while (block != host && !isBlockNode(block) && isDescendant(block, host)) {
        block = block.parentNode;
    }
    var edge = backward ? [block, 0] : [block, getNodeLength(block)];
    for (var current = block.firstChild; current && isDescendant(current, block); current = nextNode(current)) {
        if (!isHtmlElement(current, "br")) {
            continue;
        }
        var parent = current.parentNode, index = getNodeIndex(current);
        if (backward && getPosition(parent, index + 1, node, offset) != "after") {
            edge = [parent, index + 1];
        } else if (!backward && getPosition(parent, index, node, offset) != "before") {
            edge = [parent, index];
            break;
        }
    }
    return edge;
}

function moveCaret(action, extend) {
    var selection = getSelection();
    if (!selection || !selection.rangeCount) {
        return false;
    }
    var host = getEditingHostOf(selection.focusNode);
    if (!host) {
        return false;
    }
    var backward = action == "caretBackward" || action == "caretLineStart";
    if (!extend && !selection.isCollapsed
    && (action == "caretBackward" || action == "caretForward")) {
        if (backward) {
            selection.collapseToStart();
        } else {
            selection.collapseToEnd();
        }
        return true;
    }
    var node = selection.focusNode, offset = selection.focusOffset;
    var point = action == "caretBackward" ? stepBackward(node, offset, host)
        : action == "caretForward" ? stepForward(node, offset, host)
        : lineEdge(node, offset, host, backward);
    if (point) {
        if (extend) {
            selection.extend(point[0], point[1]);
        } else {
            selection.collapse(point[0], point[1]);
        }
    }
    return true;
}

// The select-all key: the editing host's contents, or in design mode the
// selectAll command's.
function selectAllUser() {
    globalRange = null;
    var host = getAffectedEditingHost();
    if (!host) {
        return false;
    }
    if (host.parentNode && host.parentNode.nodeType == Node.DOCUMENT_NODE) {
        return exec("selectall", false, ""), true;
    }
    getSelection().setBaseAndExtent(host, 0, host, getNodeLength(host));
    return true;
}

function user(action, extend, data) {
    try {
        if (action == "selectAll") {
            return selectAllUser();
        }
        if (/^caret/.test(action)) {
            return moveCaret(action, extend);
        }
        return userEdit(action, data);
    } catch (error) {
        bridge.report(error);
        return true;
    }
}

return {
    supported: supported,
    enabled: enabled,
    indeterm: function(command) {
        return guarded(function() { return myQueryCommandIndeterm(command); }, false);
    },
    state: function(command) {
        return guarded(function() { return myQueryCommandState(command); }, false);
    },
    value: function(command) {
        return guarded(function() { return myQueryCommandValue(command); }, "");
    },
    exec: exec,
    user: user,
};
})
