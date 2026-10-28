/* The editing commands against the real native DOM, Range, Selection,
 * MutationObserver and computed style (tests/jstest.c).  The expected markup is
 * what the execCommand draft's steps make of the markup each case starts from;
 * the events are those of Input Events.  Supplementary API regressions: WPT's
 * editing/, input-events/ and html/editing/ are the conformance runs. */
globalThis.runEditingCases = function () {
    let count = 0;
    function eq(actual, expected, label) {
        count++;
        if (actual !== expected) throw new Error('editing: ' + label + ' ' + JSON.stringify(actual) + ' != ' + JSON.stringify(expected));
    }
    function raises(fn, name, label) { let error = null; try { fn(); } catch (e) { error = e; } eq(error && error.name, name, label); }
    const selection = getSelection(), events = [];
    const editor = document.createElement('div');
    editor.id = 'editor';
    document.body.appendChild(editor);
    for (const type of ['beforeinput', 'input'])
        editor.addEventListener(type, e => events.push(e.type + ':' + e.inputType + ':' + (e.data === null ? 'null' : e.data) + (e.isTrusted ? '' : ':untrusted')));
    // The editor's markup, with [ and ] in text for the selection's start and end.
    function setup(html, state = 'true') {
        editor.setAttribute('contenteditable', state);
        editor.innerHTML = html.replace(/[\[\]]/g, '');
        const points = {};
        const walker = document.createTreeWalker(editor, NodeFilter.SHOW_TEXT);
        let offset = 0, text = html.replace(/<[^>]*>/g, '');
        for (let node = walker.nextNode(); node; node = walker.nextNode()) {
            for (let i = 0; i <= node.data.length; i++) {
                while (text[offset + i] === '[' || text[offset + i] === ']') {
                    points[text[offset + i]] = [node, i];
                    text = text.slice(0, offset + i) + text.slice(offset + i + 1);
                }
            }
            offset += node.data.length;
        }
        if (points['[']) selection.setBaseAndExtent(points['['][0], points['['][1], (points[']'] || points['['])[0], (points[']'] || points['['])[1]);
        events.length = 0;
    }
    function html() { return editor.innerHTML.replace(/;"/g, '"'); }

    eq(document.queryCommandSupported('bold'), true, 'bold supported');
    eq(document.queryCommandSupported('BOLD'), true, 'ASCII case-insensitive');
    eq(document.queryCommandSupported('copy'), false, 'copy unsupported without clipboard events');
    eq(document.queryCommandSupported('toString'), false, 'no inherited command');
    raises(() => document.execCommand(), 'TypeError', 'command required');
    editor.removeAttribute('contenteditable');
    editor.textContent = 'plain';
    selection.collapse(editor.firstChild, 1);
    eq(document.queryCommandEnabled('bold'), false, 'not enabled outside an editing host');
    eq(document.execCommand('bold'), false, 'execCommand outside an editing host');
    eq(document.queryCommandEnabled('selectAll'), true, 'selectAll always enabled');

    setup('a[b]c');
    eq(document.queryCommandEnabled('bold'), true, 'enabled in an editing host');
    eq(document.execCommand('bold'), true, 'bold');
    eq(html(), 'a<b>b</b>c', 'bold markup');
    eq(document.queryCommandState('bold'), true, 'bold state');
    eq(events.join(' '), 'input:formatBold:null', 'no beforeinput for a script, a trusted input');
    setup('a[b]c');
    document.execCommand('styleWithCSS', false, 'true');
    document.execCommand('bold');
    eq(html(), 'a<span style="font-weight: bold">b</span>c', 'bold with CSS');
    document.execCommand('styleWithCSS', false, 'false');
    setup('a[b]c');
    document.execCommand('foreColor', false, '#FF0000');
    eq(html(), 'a<font color="#ff0000">b</font>c', 'foreColor');
    eq(events.join(' '), 'input:formatFontColor:rgb(255, 0, 0)', 'foreColor data');
    eq(document.queryCommandValue('foreColor'), 'rgb(255, 0, 0)', 'foreColor value');
    setup('a[b]c');
    document.execCommand('fontSize', false, '5');
    eq(html(), 'a<font size="5">b</font>c', 'fontSize');
    eq(document.queryCommandValue('fontSize'), '5', 'fontSize value');
    setup('a[b]c');
    document.execCommand('createLink', false, 'https://example.test/');
    eq(html(), 'a<a href="https://example.test/">b</a>c', 'createLink');
    eq(events.join(' '), 'input:insertLink:https://example.test/', 'createLink data');
    setup('a[]c');
    document.execCommand('insertText', false, 'b');
    eq(html(), 'abc', 'insertText');
    setup('ab[]c');
    document.execCommand('delete');
    eq(html(), 'ac', 'delete');
    setup('a[]bc');
    document.execCommand('forwardDelete');
    eq(html(), 'ac', 'forwardDelete');
    setup('<p>ab[]cd</p>');
    document.execCommand('insertParagraph');
    eq(html(), '<p>ab</p><p>cd</p>', 'insertParagraph');
    setup('abc[]');
    document.execCommand('insertLineBreak');
    eq(html(), 'abc<br><br>', 'insertLineBreak');
    eq(document.execCommand('undo'), true, 'undo');
    eq(html(), 'abc', 'undo markup');
    eq(document.execCommand('redo'), true, 'redo');
    eq(html(), 'abc<br><br>', 'redo markup');
    setup('[abc]');
    document.execCommand('insertUnorderedList');
    eq(html(), '<ul><li>abc</li></ul>', 'insertUnorderedList');
    setup('[abc]');
    document.execCommand('justifyRight');
    eq(html(), '<div style="text-align: right">abc</div>', 'justifyRight');

    // HTML: contentEditable, isContentEditable, :read-write, designMode.
    editor.removeAttribute('contenteditable');
    eq(editor.contentEditable, 'inherit', 'contentEditable default');
    editor.contentEditable = 'TRUE';
    eq(editor.getAttribute('contenteditable'), 'true', 'contentEditable setter');
    eq(editor.isContentEditable, true, 'isContentEditable');
    eq(editor.matches(':read-write'), true, ':read-write');
    raises(() => { editor.contentEditable = 'bogus'; }, 'SyntaxError', 'invalid contentEditable');
    editor.contentEditable = 'inherit';
    eq(editor.hasAttribute('contenteditable'), false, 'inherit removes the attribute');
    eq(editor.matches(':read-only'), true, ':read-only');
    eq(document.designMode, 'off', 'designMode off');
    document.designMode = 'on';
    eq(document.designMode, 'on', 'designMode on');
    eq(editor.isContentEditable, true, 'design mode makes the body editable');
    eq(document.documentElement.matches(':read-write'), true, 'design mode :read-write');
    document.designMode = 'off';
    eq(editor.isContentEditable, false, 'design mode off');

    // A focused text control takes the plain-text commands.
    const input = document.createElement('input');
    document.body.appendChild(input);
    input.value = 'abc';
    input.focus();
    input.setSelectionRange(1, 2);
    const inputEvents = [];
    for (const type of ['beforeinput', 'input']) input.addEventListener(type, e => inputEvents.push(e.type + ':' + e.inputType));
    eq(document.execCommand('insertText', false, 'X'), true, 'text control insertText');
    eq(input.value, 'aXc', 'text control value');
    eq(inputEvents.join(' '), 'input:insertText', 'text control: no beforeinput for a script');
    eq(document.execCommand('delete'), true, 'text control delete');
    eq(input.value, 'ac', 'text control delete value');
    eq(document.queryCommandEnabled('bold'), false, 'rich text commands are not enabled in a text control');
    input.remove();
    editor.remove();
    return count;
};
