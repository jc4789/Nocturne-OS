/* HTML5test's public tokenizer probes, split into independent product-path
   checks. The old <?import ...> comment expectation is logged separately:
   current HTML defines a ProcessingInstruction for a valid target instead.
   https://html.spec.whatwg.org/multipage/parsing.html#processing-instruction-open-state
   https://dom.spec.whatwg.org/#interface-processinginstruction */
globalThis.runLexborCases = function () {
    let count = 0, failures = 0, legacyPassed = 0, legacyTotal = 0,
        unsupportedAPIPassed = 0, unsupportedAPITotal = 0;
    const host = document.createElement('div');
    const piInput = '<?import namespace="foo" implementation="#bar">';
    const piData = 'namespace="foo" implementation="#bar"';

    function snapshot() {
        const node = host.firstChild;
        if (!node) return {childCount: host.childNodes.length, first: null};
        const first = {type: node.nodeType, name: node.nodeName, value: node.nodeValue,
            target: node.target === undefined ? null : node.target,
            data: node.data === undefined ? null : node.data};
        if (node.nodeType === 1) first.attributesAPI = node.attributes !== undefined;
        if (typeof node.getAttributeNames === 'function') first.attributeNames = node.getAttributeNames();
        if (node.attributes) first.attributes = Array.from(node.attributes, a => ({name: a.name, nodeName: a.nodeName, value: a.value}));
        if (node.firstChild) first.child = {type: node.firstChild.nodeType, name: node.firstChild.nodeName, value: node.firstChild.nodeValue};
        return {childCount: host.childNodes.length, first: first};
    }

    function probe(name, html, predicate, diagnostic) {
        let passed = false, error = null, actual = null;
        try {
            host.innerHTML = html;
            passed = !!predicate(host.firstChild, host);
        } catch (e) {
            error = String(e);
        }
        try { actual = snapshot(); } catch (e) { actual = {snapshotError: String(e)}; }
        const category = diagnostic === 'unsupported-api' ? 'unsupported-api' : diagnostic ? 'legacy' : 'current';
        console.log('Lexbor ' + category + ' ' + name + ' ' +
            (passed ? 'PASS' : diagnostic ? 'MISMATCH' : 'FAIL') + ' input=' + JSON.stringify(html) +
            ' actual=' + JSON.stringify(actual) + (error ? ' error=' + error : ''));
        if (category === 'unsupported-api') {
            unsupportedAPITotal++;
            if (passed) unsupportedAPIPassed++;
        } else if (diagnostic) {
            legacyTotal++;
            if (passed) legacyPassed++;
        } else {
            count++;
            if (!passed) failures++;
            check('lexbor-' + name, passed);
        }
    }

    function api(name, body) {
        let result = null, passed = false, error = null;
        try { result = body(); passed = !!result.pass; } catch (e) { error = String(e); }
        console.log('Lexbor current ' + name + ' ' + (passed ? 'PASS' : 'FAIL') +
            ' actual=' + JSON.stringify(result && result.actual) + (error ? ' error=' + error : ''));
        count++;
        if (!passed) failures++;
        check('lexbor-' + name, passed);
    }

    function piState(node) {
        return {type: node.nodeType, name: node.nodeName, target: node.target,
            data: node.data, value: node.nodeValue, text: node.textContent, length: node.length};
    }

    function rejects(name, target, data) {
        api(name, function () {
            let actual = {target: target, data: data, exception: null};
            try { document.createProcessingInstruction(target, data); }
            catch (e) { actual.exception = e.name; }
            return {pass: actual.exception === 'InvalidCharacterError', actual: actual};
        });
    }

    probe('tag-less-than', '<div<div>', n => n && n.nodeName === 'DIV<DIV');
    probe('attribute-less-than', "<div foo<bar=''>", n => n &&
        n.getAttributeNames().length === 1 && n.getAttributeNames()[0] === 'foo<bar' && n.getAttribute('foo<bar') === '');
    probe('attribute-backticks', '<div foo=`bar`>', n => n && n.getAttribute('foo') === '`bar`');
    probe('attribute-quote-in-name', "<div \"foo=''>", n => n &&
        n.getAttributeNames().length === 1 && n.getAttributeNames()[0] === '"foo' && n.getAttribute('"foo') === '');
    probe('attribute-newline', "<a href='\nbar'></a>", n => n && n.getAttribute('href') === '\nbar');
    probe('fragment-doctype-ignored', '<!DOCTYPE html>', n => n === null);
    probe('carriage-return-normalized', '\u000d', n => n && n.nodeValue === '\u000a');
    probe('entities-lang-rang', '&lang;&rang;', n => n && n.nodeValue === '\u27e8\u27e9');
    probe('entity-apos', '&apos;', n => n && n.nodeValue === "'");
    probe('entity-imaginary-i', '&ImaginaryI;', n => n && n.nodeValue === '\u2148');
    probe('entity-astral-kopf', '&Kopf;', n => n && n.nodeValue === '\ud835\udd42');
    probe('entity-notinva', '&notinva;', n => n && n.nodeValue === '\u2209');
    probe('comment-double-hyphen', '<!--foo--bar-->', n => n && n.nodeType === 8 && n.nodeValue === 'foo--bar');
    probe('html-cdata-bogus-comment', '<![CDATA[x]]>', n => n && n.nodeType === 8 && n.nodeValue === '[CDATA[x]]');
    probe('textarea-comment-double-end', '<textarea><!--</textarea>--></textarea>', n => n && n.firstChild && n.firstChild.nodeValue === '<!--');
    probe('textarea-comment-single-end', '<textarea><!--</textarea>-->', n => n && n.firstChild && n.firstChild.nodeValue === '<!--');
    probe('style-comment-double-end', '<style><!--</style>--></style>', n => n && n.firstChild && n.firstChild.nodeValue === '<!--');
    probe('style-comment-single-end', '<style><!--</style>-->', n => n && n.firstChild && n.firstChild.nodeValue === '<!--');

    /* These original HTML5test probes now exercise real native Attr wrappers.
       Passing them does not establish real-site compatibility. */
    probe('html5test-attributes-index-less-than', "<div foo<bar=''>", n => n && n.attributes && n.attributes[0] &&
        (n.attributes[0].nodeName === 'foo<bar' || n.attributes[0].name === 'foo<bar'));
    probe('html5test-attributes-index-quote-in-name', "<div \"foo=''>", n => n && n.attributes && n.attributes[0] &&
        (n.attributes[0].nodeName === '"foo' || n.attributes[0].name === '"foo'));

    /* Preserve the old site's expectation as evidence, not as a requirement
       that would regress Lexbor's current-spec tokenizer into a bogus comment. */
    probe('html5test-2016-import-comment', piInput, n => n && n.nodeType === 8 &&
        n.nodeValue === '?import namespace="foo" implementation="#bar"', true);
    probe('pi-node-type', piInput, n => n && n.nodeType === 7);
    probe('pi-node-name', piInput, n => n && n.nodeName === 'import');
    probe('pi-target', piInput, n => n && n.target === 'import');
    probe('pi-node-value', piInput, n => n && n.nodeValue === piData);
    probe('pi-character-data', piInput, n => n && n.data === piData);
    probe('pi-question-mark-close', '<?import marker?>', n => n && n.nodeType === 7 && n.nodeValue === 'marker');
    probe('pi-invalid-target-comment', '<?9import marker>', n => n && n.nodeType === 8 && n.nodeValue === '?9import marker');
    probe('pi-disallowed-xml-comment', '<?xml version="1.0"?>', n => n && n.nodeType === 8 && n.nodeValue === '?xml version="1.0"?');
    probe('pi-eof-ignored', '<?import marker', n => n === null);

    api('pi-create-fields', function () {
        const n = document.createProcessingInstruction('import', 'payload');
        const actual = piState(n); actual.owner = n.ownerDocument === document;
        return {pass: n.nodeType === 7 && n.nodeName === 'import' && n.target === 'import' &&
            n.data === 'payload' && n.nodeValue === 'payload' && n.textContent === 'payload' &&
            n.ownerDocument === document && n.parentNode === null, actual: actual};
    });
    api('pi-character-data-prototype', function () {
        const n = document.createProcessingInstruction('import', 'payload');
        const actual = {pi: n instanceof ProcessingInstruction, characterData: n instanceof CharacterData,
            node: n instanceof Node, comment: n instanceof Comment, element: n instanceof Element};
        return {pass: actual.pi && actual.characterData && actual.node && !actual.comment && !actual.element,
            actual: actual};
    });
    api('pi-data-setter', function () {
        const n = document.createProcessingInstruction('import', 'old'); n.data = 'A\ud83d\ude42';
        return {pass: n.data === 'A\ud83d\ude42' && n.nodeValue === n.data && n.textContent === n.data &&
            n.length === 3 && n.target === 'import', actual: piState(n)};
    });
    api('pi-node-value-setter', function () {
        const n = document.createProcessingInstruction('import', 'old'); n.nodeValue = 'new';
        return {pass: n.data === 'new' && n.nodeValue === 'new' && n.textContent === 'new' &&
            n.target === 'import', actual: piState(n)};
    });
    api('pi-text-content-setter', function () {
        const n = document.createProcessingInstruction('import', 'old'); n.textContent = 'new';
        return {pass: n.data === 'new' && n.nodeValue === 'new' && n.textContent === 'new' &&
            n.target === 'import', actual: piState(n)};
    });
    api('pi-readonly-target', function () {
        const n = document.createProcessingInstruction('import', 'payload');
        try { n.target = 'changed'; } catch (_) {}
        return {pass: n.target === 'import' && n.nodeName === 'import', actual: piState(n)};
    });
    api('pi-clone', function () {
        const n = document.createProcessingInstruction('import', 'payload'), copy = n.cloneNode(true);
        return {pass: copy !== n && copy.nodeType === 7 && copy.target === 'import' &&
            copy.data === 'payload' && copy.ownerDocument === document && copy.parentNode === null,
            actual: piState(copy)};
    });
    api('pi-adopt', function () {
        const owner = document.implementation.createHTMLDocument('pi-owner');
        const n = document.createProcessingInstruction('import', 'payload'); host.appendChild(n);
        const adopted = owner.adoptNode(n); owner.body.appendChild(adopted);
        const actual = piState(adopted);
        actual.identity = adopted === n; actual.owner = n.ownerDocument === owner;
        actual.parent = n.parentNode === owner.body;
        return {pass: actual.identity && actual.owner && actual.parent && n.target === 'import' &&
            n.data === 'payload', actual: actual};
    });
    api('pi-parent-text-content', function () {
        const parent = document.createElement('div');
        const n = document.createProcessingInstruction('import', 'hidden');
        parent.appendChild(document.createTextNode('A')); parent.appendChild(n);
        parent.appendChild(document.createTextNode('B'));
        return {pass: parent.textContent === 'AB' && n.textContent === 'hidden',
            actual: {parentText: parent.textContent, ownText: n.textContent}};
    });
    api('pi-serialization', function () {
        const parent = document.createElement('div');
        parent.appendChild(document.createProcessingInstruction('import', 'payload'));
        return {pass: parent.innerHTML === '<?import payload>', actual: parent.innerHTML};
    });
    api('pi-create-unicode-target', function () {
        const n = document.createProcessingInstruction('\u51e6\u7406', 'payload');
        return {pass: n.target === '\u51e6\u7406' && n.nodeName === '\u51e6\u7406', actual: piState(n)};
    });
    rejects('pi-reject-empty-target', '', 'payload');
    rejects('pi-reject-leading-digit', '9import', 'payload');
    rejects('pi-reject-target-whitespace', 'im port', 'payload');
    rejects('pi-reject-target-trailing-newline', 'import\n', 'payload');
    rejects('pi-reject-data-terminator', 'import', 'a?>b');
    api('pi-create-required-arguments', function () {
        let exception = null;
        try { document.createProcessingInstruction('import'); } catch (e) { exception = e.name; }
        return {pass: exception === 'TypeError', actual: {exception: exception}};
    });

    api('pi-constructor-default-data', function () {
        const n = new ProcessingInstruction('import');
        return {pass: n instanceof ProcessingInstruction && n.target === 'import' && n.data === '' &&
            n.ownerDocument === document && n.parentNode === null, actual: piState(n)};
    });
    api('pi-constructor-data', function () {
        const n = new ProcessingInstruction('import', 'payload');
        return {pass: n.nodeType === 7 && n.target === 'import' && n.data === 'payload' &&
            n.ownerDocument === document, actual: piState(n)};
    });
    for (const test of [
        ['required-target', [], 'TypeError'],
        ['invalid-target', ['9import', 'payload'], 'InvalidCharacterError'],
        ['invalid-data', ['import', 'a?>b'], 'InvalidCharacterError']
    ]) api('pi-constructor-' + test[0], function () {
        let exception = null;
        try { new ProcessingInstruction(...test[1]); } catch (e) { exception = e.name; }
        return {pass: exception === test[2], actual: {arguments: test[1], exception: exception}};
    });
    api('pi-constructor-derived-class', function () {
        class DerivedPI extends ProcessingInstruction {
            constructor() { super('import', 'payload'); this.marker = 7; }
        }
        const n = new DerivedPI();
        return {pass: Object.getPrototypeOf(n) === DerivedPI.prototype && n instanceof DerivedPI &&
            n instanceof ProcessingInstruction && n instanceof CharacterData && n.marker === 7 &&
            n.target === 'import' && n.data === 'payload' && n.ownerDocument === document,
            actual: piState(n)};
    });
    api('pi-substring-data-utf16', function () {
        const n = new ProcessingInstruction('import', 'A\ud83d\ude42B');
        const actual = {pair: n.substringData(1, 2), low: n.substringData(2, 1),
            tail: n.substringData(3, 99), end: n.substringData(4, 1), data: n.data};
        return {pass: actual.pair === '\ud83d\ude42' && actual.low === '\ude42' && actual.tail === 'B' &&
            actual.end === '' && actual.data === 'A\ud83d\ude42B', actual: actual};
    });
    api('pi-append-data-utf16', function () {
        const n = new ProcessingInstruction('import', 'A'); const value = n.appendData('\ud83d\ude42B');
        return {pass: value === undefined && n.data === 'A\ud83d\ude42B' && n.length === 4,
            actual: piState(n)};
    });
    api('pi-insert-data-utf16', function () {
        const n = new ProcessingInstruction('import', 'A\ud83d\ude42B'); const value = n.insertData(3, 'X');
        return {pass: value === undefined && n.data === 'A\ud83d\ude42XB' && n.length === 5,
            actual: piState(n)};
    });
    api('pi-delete-data-utf16', function () {
        const n = new ProcessingInstruction('import', 'A\ud83d\ude42B'); const value = n.deleteData(1, 2);
        return {pass: value === undefined && n.data === 'AB' && n.length === 2, actual: piState(n)};
    });
    api('pi-replace-data-utf16', function () {
        const n = new ProcessingInstruction('import', 'A\ud83d\ude42B'); const value = n.replaceData(1, 2, 'X');
        return {pass: value === undefined && n.data === 'AXB' && n.length === 3, actual: piState(n)};
    });
    for (const test of [
        ['substringData', [5, 1]], ['insertData', [5, 'X']],
        ['deleteData', [5, 1]], ['replaceData', [5, 1, 'X']]
    ]) api('pi-' + test[0] + '-out-of-range', function () {
        const n = new ProcessingInstruction('import', 'A\ud83d\ude42B'); let exception = null;
        try { n[test[0]](...test[1]); } catch (e) { exception = e.name; }
        return {pass: exception === 'IndexSizeError' && n.data === 'A\ud83d\ude42B',
            actual: {arguments: test[1], exception: exception, data: n.data}};
    });
    api('pi-character-data-mutation-observer', function () {
        const n = new ProcessingInstruction('import', 'A'), observer = new MutationObserver(function () {});
        observer.observe(n, {characterData: true, characterDataOldValue: true});
        n.appendData('B'); n.replaceData(0, 1, 'C');
        const records = observer.takeRecords(); observer.disconnect();
        const actual = records.map(r => ({type: r.type, oldValue: r.oldValue, target: r.target === n}));
        return {pass: n.data === 'CB' && records.length === 2 && records.every(r =>
            r.type === 'characterData' && r.target === n) && records[0].oldValue === 'A' &&
            records[1].oldValue === 'AB', actual: actual};
    });

    console.log('Lexbor summary current=' + count + ' failures=' + failures +
        ' legacy=' + legacyPassed + '/' + legacyTotal +
        ' unsupported-api=' + unsupportedAPIPassed + '/' + unsupportedAPITotal);
    return {checks: count, failures: failures, legacyPassed: legacyPassed, legacyTotal: legacyTotal,
        unsupportedAPIPassed: unsupportedAPIPassed, unsupportedAPITotal: unsupportedAPITotal};
};
