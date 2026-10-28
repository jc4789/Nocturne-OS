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
// HTML's "editing host" and "editable" (js_document_commands.js).
const editingHost = bridge.editingHost;
const editable = bridge.editable;

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
    return guarded(function() { return myQueryCommandEnabled(command); }, false);
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
        return myExecCommand(command, showUi, value);
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
    // A plaintext-only editing host takes no formatting.
    if (/^format/.test(inputType) && bridge.editableState(host) == "plaintext-only") {
        return true;
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
        return myExecCommand(command, false, value);
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
