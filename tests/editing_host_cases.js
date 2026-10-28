/* Cases for tests/check_editing.py: the product's js_document_commands.js and
 * js_editing.js over the host DOM of tests/editing_host_dom.js.  The expected
 * markup is what the execCommand draft's steps make of the markup each case
 * starts from (the draft wraps a line in a div when it splits a paragraph, and
 * wraps the whole of an element whose contents are all selected); the events
 * are those of Input Events. */
'use strict';
let checks = 0, failures = 0;
function check(label, actual, expected) {
    checks++;
    // A declaration block's last semicolon is the serializer's, not the command's.
    if (typeof actual === 'string') actual = actual.replace(/;"/g, '"');
    if (typeof expected === 'string') expected = expected.replace(/;"/g, '"');
    if (actual !== expected) { failures++; print('FAIL ' + label + '\n  got      ' + JSON.stringify(actual) + '\n  expected ' + JSON.stringify(expected)); }
}
function raises(label, fn, name) { let error = null; try { fn(); } catch (e) { error = e; } check(label, error && error.name, name); }
const body = document.body, selection = document.getSelection();
// The editing host's markup, with [ and ] in text for the selection's start and end.
function setup(html, attributes = ' contenteditable') {
    body.innerHTML = '<div id="editor"' + attributes + '>' + html + '</div>';
    const editor = body.firstChild, points = {};
    (function walk(node) {
        for (const child of node.childNodes.slice()) {
            if (child.nodeType === 3) {
                for (const mark of ['[', ']']) {
                    const at = child.data.indexOf(mark);
                    if (at >= 0) { child.deleteData(at, 1); points[mark] = [child, at]; }
                }
            } else walk(child);
        }
    })(editor);
    if (points['[']) selection.setBaseAndExtent(points['['][0], points['['][1], (points[']'] || points['['])[0], (points[']'] || points['['])[1]);
    dispatched.length = 0;
    return editor;
}
function events() { return dispatched.map(e => e.type + ':' + e.inputType + ':' + (e.data === null ? 'null' : e.data) + (e.isTrusted ? '' : ':untrusted')).join(' '); }

// Support and enablement.
check('supported bold', document.queryCommandSupported('bold'), true);
check('supported BoLd (ASCII case-insensitive)', document.queryCommandSupported('BoLd'), true);
check('supported undo', document.queryCommandSupported('undo'), true);
check('copy is unsupported (no clipboard events)', document.queryCommandSupported('copy'), false);
check('unknown command', document.queryCommandSupported('toString'), false);
raises('no argument', () => document.queryCommandSupported(), 'TypeError');
body.innerHTML = '<p>outside</p>';
selection.collapse(body.firstChild.firstChild, 1);
check('not enabled outside an editing host', document.queryCommandEnabled('bold'), false);
check('selectAll always enabled', document.queryCommandEnabled('selectAll'), true);
check('execCommand outside an editing host', document.execCommand('bold'), false);
check('undo not enabled with no history', document.queryCommandEnabled('undo'), false);

// Inline formatting.
let editor = setup('a[b]c');
check('enabled in an editing host', document.queryCommandEnabled('bold'), true);
check('bold returns true', document.execCommand('bold'), true);
check('bold markup', editor.innerHTML, 'a<b>b</b>c');
check('bold state', document.queryCommandState('bold'), true);
check('bold events: input only (no beforeinput for a script)', events(), 'input:formatBold:null');
dispatched.length = 0;
check('italic wraps the whole selected b', document.execCommand('italic') && editor.innerHTML, 'a<i><b>b</b></i>c');
check('italic event', events(), 'input:formatItalic:null');
check('bold again removes it', document.execCommand('bold') && editor.innerHTML, 'a<i>b</i>c');
editor = setup('a[b]c');
document.execCommand('styleWithCSS', false, 'true');
check('styleWithCSS state', document.queryCommandState('styleWithCSS'), true);
check('bold with CSS', document.execCommand('bold') && editor.innerHTML, 'a<span style="font-weight: bold;">b</span>c');
document.execCommand('styleWithCSS', false, 'false');
editor = setup('a[b]c');
check('foreColor', document.execCommand('foreColor', false, '#FF0000') && editor.innerHTML, 'a<font color="#ff0000">b</font>c');
check('foreColor event data is the serialized color', events(), 'input:formatFontColor:rgb(255, 0, 0)');
check('foreColor value', document.queryCommandValue('foreColor'), 'rgb(255, 0, 0)');
editor = setup('a[b]c');
check('invalid color', document.execCommand('foreColor', false, 'not-a-color'), false);
check('invalid color fires nothing', events(), '');
editor = setup('a[b]c');
check('fontSize', document.execCommand('fontSize', false, '5') && editor.innerHTML, 'a<font size="5">b</font>c');
check('fontSize value', document.queryCommandValue('fontSize'), '5');
editor = setup('a[b]c');
check('createLink', document.execCommand('createLink', false, 'https://example.test/') && editor.innerHTML, 'a<a href="https://example.test/">b</a>c');
check('createLink event', events(), 'input:insertLink:https://example.test/');
dispatched.length = 0;
check('unlink', document.execCommand('unlink') && editor.innerHTML, 'abc');
check('unlink event has no inputType', events(), 'input::null');

// Text.
editor = setup('a[]c');
check('insertText', document.execCommand('insertText', false, 'b') && editor.innerHTML, 'abc');
check('insertText event', events(), 'input:insertText:b');
editor = setup('[]');
let nested = [];
const recurse = () => nested.push(document.execCommand('insertText', false, ', ' + nested.length));
editor.addEventListener('input', recurse);
check('a command inside its own input event does nothing', document.execCommand('insertText', false, '0') && editor.innerHTML, '0');
check('the nested command returns false', nested.join(), 'false');
editor.removeEventListener('input', recurse);
editor = setup('ab[]c');
check('delete', document.execCommand('delete') && editor.innerHTML, 'ac');
check('delete event', events(), 'input:deleteContentBackward:null');
editor = setup('a😀[]b');
check('delete takes a surrogate pair', document.execCommand('delete') && editor.innerHTML, 'ab');
editor = setup('a[]bc');
check('forwardDelete', document.execCommand('forwardDelete') && editor.innerHTML, 'ac');
editor = setup('<p>ab[]cd</p>');
check('insertParagraph', document.execCommand('insertParagraph') && editor.innerHTML, '<p>ab</p><p>cd</p>');
editor = setup('abc[]');
check('insertLineBreak at the end', document.execCommand('insertLineBreak') && editor.innerHTML, 'abc<br><br>');
editor = setup('[abc]');
check('insertOrderedList', document.execCommand('insertOrderedList') && editor.innerHTML, '<ol><li>abc</li></ol>');
editor = setup('[abc]');
check('justifyCenter', document.execCommand('justifyCenter') && editor.innerHTML, '<div style="text-align: center;">abc</div>');
editor = setup('a[]b');
check('insertHTML', document.execCommand('insertHTML', false, '<i>x</i>') && editor.innerHTML, 'a<i>x</i>b');
editor = setup('a[]b');
check('insertHorizontalRule', document.execCommand('insertHorizontalRule') && editor.innerHTML, 'a<hr>b');

// Undo and redo.
editor = setup('abc[]');
document.execCommand('insertLineBreak');
dispatched.length = 0;
check('undo enabled', document.queryCommandEnabled('undo'), true);
check('undo', document.execCommand('undo') && editor.innerHTML, 'abc');
check('undo event', events(), 'input:historyUndo:null');
check('undo restores the selection', selection.focusNode === editor.firstChild && selection.focusOffset === 3, true);
check('redo enabled', document.queryCommandEnabled('redo'), true);
check('redo', document.execCommand('redo') && editor.innerHTML, 'abc<br><br>');
check('undo after redo', document.execCommand('undo') && editor.innerHTML, 'abc');
editor.appendChild(document.createElement('i')).textContent = ' appended';
check('redo after the page appended a child', document.execCommand('redo') && editor.innerHTML, 'abc<i> appended</i><br><br>');
editor = setup('a[b]c');
document.execCommand('bold');
document.execCommand('italic');
check('two transactions undone one at a time', document.execCommand('undo') && editor.innerHTML, 'a<b>b</b>c');
check('then the first', document.execCommand('undo') && editor.innerHTML, 'a[b]c'.replace(/[\[\]]/g, ''));
check('a new edit clears redo', (document.execCommand('insertText', false, 'x'), document.queryCommandEnabled('redo')), false);

// The user's edits: beforeinput first, cancelable.
editor = setup('a[]c');
let cancel = true;
editor.addEventListener('beforeinput', e => { if (cancel) e.preventDefault(); });
check('user typing taken', documentCommandBridge.userAction('insertText', false, 'b'), true);
check('canceled beforeinput leaves the DOM', editor.innerHTML, 'ac');
check('canceled typing fires beforeinput only', events(), 'beforeinput:insertText:b');
cancel = false; dispatched.length = 0;
documentCommandBridge.userAction('insertText', false, 'b');
check('user typing', editor.innerHTML, 'abc');
check('user typing events', events(), 'beforeinput:insertText:b input:insertText:b');
documentCommandBridge.userAction('insertText', false, 'x');
documentCommandBridge.userAction('insertText', false, 'y');
check('typed', editor.innerHTML, 'abxyc');
document.execCommand('undo');
check('typing is one transaction', editor.innerHTML, 'ac');
editor = setup('ab[]c');
dispatched.length = 0;
documentCommandBridge.userAction('deleteContentBackward', false, null);
check('Backspace', editor.innerHTML, 'ac');
check('Backspace events', events(), 'beforeinput:deleteContentBackward:null input:deleteContentBackward:null');
editor = setup('ab[]');
documentCommandBridge.userAction('insertParagraph', false, null);
check('Enter', editor.innerHTML, '<div>ab</div><div><br></div>');
editor = setup('a[b]c');
documentCommandBridge.userAction('formatBold', false, null);
check('Ctrl+B', editor.innerHTML, 'a<b>b</b>c');
documentCommandBridge.userAction('historyUndo', false, null);
check('Ctrl+Z', editor.innerHTML, 'abc');
editor = setup('a[]c');
documentCommandBridge.userAction('insertFromPaste', false, 'x\r\ny');
check('paste of two lines', editor.innerHTML, '<div>ax</div><div>yc</div>');
editor = setup('a[b]c');
documentCommandBridge.userAction('deleteByCut', false, null);
check('cut', editor.innerHTML, 'ac');
editor = setup('a[]b', ' contenteditable="plaintext-only"');
documentCommandBridge.userAction('formatBold', false, null);
check('plaintext-only takes no formatting', editor.innerHTML, 'ab');
check('plaintext-only: bold disabled', document.queryCommandEnabled('bold') + ' ' + document.execCommand('bold') + ' ' + editor.innerHTML, 'false false ab');
check('plaintext-only: insertText enabled', document.queryCommandEnabled('insertText'), true);
check('plaintext-only: no input for a refused command', events(), '');
editor = setup('<span contenteditable="true">a[]b</span>', ' contenteditable="plaintext-only"');
check('an editing host in a plaintext-only one is edited as its part', document.queryCommandEnabled('bold'), false);
editor = setup('A[]B', ' contenteditable="plaintext-only"');
document.execCommand('insertLineBreak');
check('plaintext-only, white-space normal: a br', editor.innerHTML, 'A<br>B');
editor = setup('<p style="white-space:normal">A[]B</p>', ' contenteditable="plaintext-only" style="white-space:pre"');
document.execCommand('insertLineBreak');
check('plaintext-only, a white-space normal paragraph: a br', editor.innerHTML, '<p style="white-space:normal">A<br>B</p>');
editor = setup('A[]', ' contenteditable="plaintext-only" style="white-space:pre"');
document.execCommand('insertLineBreak');
check('plaintext-only pre: a line feed, padded', editor.innerHTML, 'A\n<br>');
document.execCommand('insertText', false, 'b');
check('text filling the padded line drops the padding', editor.innerHTML, 'A\nb');
editor = setup('[]\n', ' contenteditable="plaintext-only" style="white-space:pre"');
document.execCommand('insertText', false, 'a');
check('text filling a padding line feed drops it', editor.innerHTML, 'a');
editor = setup('AB[]\n', ' contenteditable="plaintext-only" style="white-space:pre"');
document.execCommand('insertText', false, 'c');
check('a line feed extraneous before stays', editor.innerHTML, 'ABc\n');
editor = setup('[]', ' contenteditable="plaintext-only" style="white-space:pre"');
document.execCommand('insertText', false, 'x\ny');
check('plaintext-only insertText: line feeds', editor.innerHTML, 'x\ny');
editor = setup('a[]<select><option>x</option></select>b');
selection.collapse(editor.childNodes[1].firstChild.firstChild, 0);
check('nothing edits option data', document.queryCommandEnabled('insertText') + ' ' + document.execCommand('insertText', false, 'y') + ' ' + editor.innerHTML,
    'false false a<select><option>x</option></select>b');
selection.setBaseAndExtent(editor, 1, editor, 2);
document.execCommand('delete');
check('deleting a selected select removes it', editor.innerHTML, 'ab');
check('no editing host: not taken', (body.innerHTML = '<p>x</p>', selection.collapse(body.firstChild.firstChild, 0), documentCommandBridge.userAction('insertText', false, 'y')), false);

// Caret moves.
editor = setup('ab[]c');
documentCommandBridge.userAction('caretBackward', false, null);
check('Left', selection.focusNode === editor.firstChild && selection.focusOffset === 1 && selection.isCollapsed, true);
documentCommandBridge.userAction('caretForward', true, null);
check('Shift+Right', selection.anchorOffset === 1 && selection.focusOffset === 2, true);
documentCommandBridge.userAction('caretLineStart', false, null);
check('Home', selection.focusNode === editor && selection.focusOffset === 0, true);
documentCommandBridge.userAction('caretLineEnd', false, null);
check('End', selection.focusNode === editor && selection.focusOffset === 1, true);
editor = setup('ab<br>c[]d');
documentCommandBridge.userAction('caretLineStart', false, null);
check('Home after a br', selection.focusNode === editor && selection.focusOffset === 2, true);
editor = setup('a<br>[]b');
documentCommandBridge.userAction('caretBackward', false, null);
check('Left over a br', selection.focusNode === editor && selection.focusOffset === 1, true);
editor = setup('ab[]c');
documentCommandBridge.userAction('selectAll', false, null);
check('Ctrl+A selects the editing host', selection.anchorNode === editor && selection.focusOffset === editor.childNodes.length, true);
check('selection text', documentCommandBridge.selectionText(), 'abc');

// HTML: contentEditable, isContentEditable, designMode, focus.
body.innerHTML = '<div id="x"><span>a</span></div>';
const div = body.firstChild, span = div.firstChild;
check('contentEditable default', div.contentEditable, 'inherit');
div.contentEditable = 'TRUE';
check('contentEditable set', div.getAttribute('contenteditable') + ' ' + div.contentEditable, 'true true');
check('isContentEditable editor', div.isContentEditable, true);
check('isContentEditable child', span.isContentEditable, true);
span.contentEditable = 'false';
check('false state', span.isContentEditable, false);
raises('invalid contentEditable', () => { div.contentEditable = 'bogus'; }, 'SyntaxError');
div.contentEditable = 'plaintext-only';
check('plaintext-only', div.contentEditable + ' ' + div.isContentEditable, 'plaintext-only true');
div.contentEditable = 'inherit';
check('inherit removes', div.getAttribute('contenteditable') + ' ' + div.isContentEditable, 'null false');
body.innerHTML = '<div id="h" contenteditable>abc</div>';
selection.collapse(document.body, 0);
documentCommandBridge.focused(body.firstChild);
check('focus puts the caret in the editing host', selection.focusNode === body.firstChild && selection.focusOffset === 0, true);
check('designMode off', document.designMode, 'off');
body.innerHTML = '<p>abc</p>';
selection.collapse(body.firstChild.firstChild, 1);
document.designMode = 'ON';
check('designMode on', document.designMode + ' ' + nativeDesignMode, 'on true');
check('designMode resets the selection to the start of the Document', selection.focusNode === document && selection.focusOffset === 0, true);
check('designMode focuses the document element', focused === document.documentElement, true);
check('body is editable', body.isContentEditable, true);
selection.setBaseAndExtent(body.firstChild.firstChild, 0, body.firstChild.firstChild, 3);
check('bold in design mode', document.execCommand('bold') && body.innerHTML, '<p><b>abc</b></p>');
document.designMode = 'off';
check('designMode back off', document.designMode + ' ' + nativeDesignMode + ' ' + body.isContentEditable, 'off false false');

// execCommand does not throw.
body.innerHTML = '';
selection.removeAllRanges();
check('state with no selection', document.queryCommandState('bold'), false);
check('value with no selection', document.queryCommandValue('fontName'), '');
for (const name of ['bold', 'delete', 'insertText', 'insertParagraph', 'fontSize', 'formatBlock', 'indent', 'selectAll'])
    try { document.execCommand(name, false, 'x'); checks++; } catch (e) { failures++; print('FAIL ' + name + ' threw ' + e); }

for (const [level, text] of logs) print('LOG ' + level + ' ' + text);
check('no editing errors logged', logs.length, 0);
print('editing host checks: ' + checks + ', failed: ' + failures);
