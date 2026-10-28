// The DOM collections over collections_native_cases.c's native tree. The
// first part carries WPT's expectations unchanged
// (WebIDL/ecmascript-binding/legacy-platform-object/*.html,
// dom/collections/HTMLCollection-*.html, dom/nodes/NodeList-static-length-
// getter-tampered-*.html), with getElementsByTagName and friends replaced by
// the bridge's DOM-tree collections over the tree; the second part, what each
// collection reads and when. The last value is the number of failures.
const B = collectionBridge;
let passed = 0, failed = 0;
const tags = new WeakMap();
function fmt(v) {
    try {
        if (typeof v === 'symbol') return v.toString();
        if (typeof v === 'object' && v !== null) return tags.has(v) ? '<' + tags.get(v) + '>' : Object.prototype.toString.call(v);
        return String(v);
    } catch (e) { return '?'; }
}
function assert_equals(a, e, m) { if (!Object.is(a, e)) throw new Error((m || '') + ' expected ' + fmt(e) + ' got ' + fmt(a)); }
function assert_true(a, m) { assert_equals(a, true, m); }
function assert_false(a, m) { assert_equals(a, false, m); }
function assert_array_equals(a, e, m) {
    if (a.length !== e.length) throw new Error((m || '') + ' length ' + a.length + ' vs ' + e.length + ' [' + Array.from(a, fmt) + ']');
    for (let i = 0; i < e.length; i++)
        if (!Object.is(a[i], e[i])) throw new Error((m || '') + ' [' + i + '] expected ' + fmt(e[i]) + ' got ' + fmt(a[i]));
}
function assert_throws_js(C, f, m) {
    try { f(); } catch (e) { if (!(e instanceof C)) throw new Error((m || '') + ' threw ' + e); return; }
    throw new Error((m || '') + ' did not throw');
}
// WebIDL/ecmascript-binding/legacy-platform-object/helper.js
function assert_prop_desc_equals(object, key, expected) {
    const actual = Object.getOwnPropertyDescriptor(object, key);
    if (expected === undefined) { assert_equals(actual, undefined, '(no property descriptor expected)'); return; }
    if (actual === undefined) throw new Error('no descriptor for ' + String(key));
    for (const p in actual) { assert_true(Object.prototype.hasOwnProperty.call(expected, p), "property '" + p + "' is not expected"); assert_equals(actual[p], expected[p], "property '" + p + "'"); }
    for (const p in expected) assert_true(Object.prototype.hasOwnProperty.call(actual, p), "expected property '" + p + "' missing");
}
const cleanups = [];
function test(f, name) {
    try { f.call({ add_cleanup(c) { cleanups.push(c); } }); passed++; }
    catch (e) { failed++; T.print('FAIL ' + name + ': ' + e.message); }
    while (cleanups.length) { try { cleanups.pop()(); } catch (e) {} }
}

const doc = T.doc(), body = el('body');
T.insert(doc, body, -1);
function el(tag, attrs = {}, html = true, owner = doc) {
    const e = T.element(owner, html);
    tags.set(e, tag);
    for (const k in attrs) T.attr(e, k, attrs[k]);
    return e;
}
function append(parent, tag, attrs, html) { const e = el(tag, attrs, html); T.insert(parent, e, -1); return e; }
function descendants(root, pred) {
    const out = [];
    (function walk(n) { for (const k of T.get(n, 'childNodes')) { if (T.get(k, 'nodeType') === 1 && pred(k)) out.push(k); walk(k); } })(root);
    return out;
}
const documentLists = new Map();
// getElementsByTagName: the same object for the document and a name.
function byTag(tag, root = doc) {
    if (root === doc && documentLists.has(tag)) return documentLists.get(tag);
    const c = B.domHTML(root, () => descendants(root, e => tag === '*' || tags.get(e) === tag));
    if (root === doc) documentLists.set(tag, c);
    return c;
}

// ---- legacy-platform-object/GetOwnProperty.html
test(function () {
    const span1 = append(body, 'span', { id: 'foo' }), span2 = append(body, 'span', { id: 'bar' });
    this.add_cleanup(() => { T.remove(span1); T.remove(span2); });
    const elementList = byTag('span');
    assert_prop_desc_equals(elementList, 'foo', { value: span1, writable: false, enumerable: false, configurable: true });
    assert_prop_desc_equals(elementList, '1', { value: span2, writable: false, enumerable: true, configurable: true });
    assert_prop_desc_equals(elementList, 'unknown', undefined);
    assert_array_equals(Object.keys(elementList), ['0', '1']);
    assert_array_equals(Object.values(elementList), [span1, span2]);
}, '[[GetOwnProperty]] with getters and no setters');

// ---- legacy-platform-object/GetOwnProperty.html and DefineOwnProperty.html:
// HTMLSelectElement's indexed setter is HTMLOptionsCollection's here.
function optionsCollection() {
    const options = [];
    const c = B.options(() => options.slice(), Object.create(HTMLCollection.prototype), (index, value) => {
        if (value === null) { if (index < options.length) options.splice(index, 1); return; }
        while (options.length < index) options.push(el('option'));
        options[index] = value;
    });
    return { c, options };
}
test(function () {
    const { c } = optionsCollection();
    assert_prop_desc_equals(c, '0', undefined);
    const option0 = el('option'), option1 = el('option');
    assert_throws_js(TypeError, () => Object.defineProperty(c, '0', { get: () => {} }));
    assert_throws_js(TypeError, () => Object.defineProperty(c, '0', { set: () => {} }));
    assert_prop_desc_equals(c, '0', undefined);
    Object.defineProperty(c, '0', { value: option0, writable: false, enumerable: false, configurable: false });
    assert_prop_desc_equals(c, '0', { value: option0, writable: true, enumerable: true, configurable: true });
    c[1] = option1;
    assert_prop_desc_equals(c, '1', { value: option1, writable: true, enumerable: true, configurable: true });
    assert_throws_js(TypeError, () => Object.defineProperty(c, '0', { get: () => {} }));
    assert_throws_js(TypeError, () => Object.defineProperty(c, '0', { set: () => {} }));
    assert_prop_desc_equals(c, '0', { value: option0, writable: true, enumerable: true, configurable: true });
    assert_array_equals(Object.keys(c), ['0', '1']);
    assert_array_equals(Object.values(c), [option0, option1]);
}, 'Test [[DefineOwnProperty]] with indexed property setter support');
test(function () {
    const { c, options } = optionsCollection();
    const option = el('option');
    c[3] = option;
    assert_equals(options.length, 4, 'the setter fills up to the index');
    assert_equals(c[3], option);
    (function () { 'use strict'; c[0] = null; })();
    assert_equals(options.length, 3, 'null removes');
    assert_equals(c[2], option);
    // [[Set]] with another receiver (the collection as a prototype) does not
    // call the setter: the writable indexed property goes to the receiver.
    const object = Object.create(c);
    object[0] = 'own';
    assert_equals(Object.getOwnPropertyDescriptor(object, '0').value, 'own');
    assert_equals(options.length, 3);
}, 'HTMLOptionsCollection indexed setter');

// ---- legacy-platform-object/DefineOwnProperty.html: no named setter.
test(function () {
    const option0 = el('option', { id: 'foo' });
    const htmlCollection = B.html(() => [option0]);
    assert_equals(htmlCollection.length, 1);
    assert_prop_desc_equals(htmlCollection, 'foo', { value: option0, writable: false, enumerable: false, configurable: true });
    assert_prop_desc_equals(htmlCollection, 'bar', undefined);
    assert_throws_js(TypeError, () => Object.defineProperty(htmlCollection, 'foo', { value: true, writable: true }));
    assert_throws_js(TypeError, () => Object.defineProperty(htmlCollection, 'foo', { get: () => {} }));
    assert_throws_js(TypeError, () => Object.defineProperty(htmlCollection, 'foo', { set: () => {} }));
    const option1 = el('option', { id: 'bar' });
    Object.defineProperty(htmlCollection, 'bar', { value: option1, writable: true });
    assert_equals(htmlCollection['foo'], option0);
    assert_equals(htmlCollection['bar'], option1);
    htmlCollection['foo'] = body;
    htmlCollection['baz'] = body;
    assert_equals(htmlCollection['foo'], option0);
    assert_equals(htmlCollection['baz'], body);
    assert_throws_js(TypeError, () => { 'use strict'; htmlCollection['foo'] = body; });
    assert_prop_desc_equals(htmlCollection, 'foo', { value: option0, writable: false, enumerable: false, configurable: true });
}, 'Test [[DefineOwnProperty]] with no named property setter support');

// ---- legacy-platform-object/OwnPropertyKeys.html (childNodes; the named
// cases on an HTMLCollection rather than NamedNodeMap).
test(function () {
    const element = el('div');
    for (let i = 0; i < 3; i++) T.insert(element, el('div'), -1);
    assert_array_equals(Reflect.ownKeys(B.children(element, false)), ['0', '1', '2']);
}, 'must enumerate property indices in ascending numerical order');
test(function () {
    const root = el('div');
    T.insert(root, el('i', { id: 'id' }), -1); T.insert(root, el('i', { id: 'class' }), -1);
    assert_array_equals(Reflect.ownKeys(byTag('i', root)), ['0', '1', 'id', 'class']);
}, 'must first enumerate property indices in ascending numerical order, then named properties in list order');
test(function () {
    const root = el('div'), c = byTag('i', root);
    c.foo = 'some value'; c.bar = 'and another';
    T.insert(root, el('i', { id: 'id' }), -1); T.insert(root, el('i', { id: 'class' }), -1);
    assert_array_equals(Reflect.ownKeys(c), ['0', '1', 'id', 'class', 'foo', 'bar']);
}, "must enumerate own properties after indexed and named properties even when they're added first");
test(function () {
    const symb1 = Symbol(), symb2 = Symbol();
    const root = el('div'), c = byTag('i', root);
    c.foo = 'some value'; c[symb1] = 'symb1'; c[symb2] = 'symb2'; c.bar = 'and another';
    T.insert(root, el('i', { id: 'id' }), -1); T.insert(root, el('i', { id: 'class' }), -1);
    assert_array_equals(Reflect.ownKeys(c), ['0', '1', 'id', 'class', 'foo', 'bar', symb1, symb2]);
}, 'must enumerate symbols after strings, regardless of which ones got added first');

// ---- legacy-platform-object/Set.html
const symbol = Symbol();
function oneChild() { const element = el('div'); T.insert(element, el('div'), -1); return B.children(element, false); }
test(function () { const c = oneChild(); c['5'] = 'foo'; assert_equals(c['5'], undefined); },
     'must not set the property value when assigning to a numeric property on an object which implements an indexed property getter but not a setter when not in strict mode');
test(function () { 'use strict'; const c = oneChild(); assert_throws_js(TypeError, function () { c['5'] = 'foo'; }); },
     'must throw when assigning to a numeric property on an object which implements a indexed property getter but not a setter in strict mode');
test(function () { const c = oneChild(); c[symbol] = 'foo'; assert_equals(c[symbol], 'foo'); },
     'must allow assigning to a symbol property on an object which implements an indexed property getter but not a setter when not in strict mode');
test(function () { 'use strict'; const c = oneChild(); c[symbol] = 'foo'; assert_equals(c[symbol], 'foo'); },
     'must allow assigning to a symbol property on an object which implements an indexed property getter but not a setter in strict mode');
test(function () { const c = byTag('*', el('div')); c[symbol] = 'foo'; assert_equals(c[symbol], 'foo'); },
     'must allow assigning to a symbol property on an object which implements indexed and named property getters but no setters when not in strict mode');
test(function () { 'use strict'; const c = byTag('*', el('div')); c[symbol] = 'foo'; assert_equals(c[symbol], 'foo'); },
     'must allow assigning to a symbol property on an object which implements indexed and named property getters but no setters in strict mode');
test(function () { const c = byTag('*', el('div')); c.foo = 'foo'; assert_equals(c.foo, 'foo'); },
     'must allow assigning to a named property on an object which implements a named property getter but not a setter');

// ---- dom/collections/HTMLCollection-own-props.html
for (const [tag, strict] of [['a', false], ['b', true]]) test(function () {
    const c = byTag(tag), element = append(body, tag, { id: 'named' });
    this.add_cleanup(() => T.remove(element));
    assert_equals(c.named, element);
    if (strict) assert_throws_js(TypeError, function () { 'use strict'; c.named = 'foo'; });
    else c.named = 'foo';
    assert_equals(c.named, element);
}, 'Setting non-array index while named property exists (' + (strict ? 'strict' : 'loose') + ')');
for (const [tag, strict] of [['i', false], ['p', true]]) test(function () {
    const c = byTag(tag);
    assert_equals(c.named, undefined);
    if (strict) (function () { 'use strict'; c.named = 'foo'; })(); else c.named = 'foo';
    assert_equals(c.named, 'foo');
    const element = append(body, tag, { id: 'named' });
    this.add_cleanup(() => T.remove(element));
    assert_equals(c.named, 'foo');
    assert_equals(c.namedItem('named'), element);
}, "Setting non-array index while named property doesn't exist (" + (strict ? 'strict' : 'loose') + ')');
for (const [tag, strict] of [['q', false], ['s', true]]) test(function () {
    const c = byTag(tag), element = append(body, tag);
    this.add_cleanup(() => T.remove(element));
    assert_equals(c[0], element);
    if (strict) assert_throws_js(TypeError, function () { 'use strict'; c[0] = 'foo'; });
    else c[0] = 'foo';
    assert_equals(c[0], element);
}, 'Setting array index while indexed property exists (' + (strict ? 'strict' : 'loose') + ')');
for (const strict of [false, true]) test(function () {
    const c = byTag('u');
    assert_equals(c[0], undefined);
    if (strict) assert_throws_js(TypeError, function () { 'use strict'; c[0] = 'foo'; });
    else c[0] = 'foo';
    assert_equals(c[0], undefined);
    const element = append(body, 'u');
    this.add_cleanup(() => T.remove(element));
    assert_equals(c[0], element);
}, "Setting array index while indexed property doesn't exist (" + (strict ? 'strict' : 'loose') + ')');

// ---- dom/collections/HTMLCollection-as-prototype.html
test(function () {
    const obj = Object.create(byTag('script'));
    assert_throws_js(TypeError, function () { obj.length; });
}, 'HTMLCollection as a prototype should not allow getting .length on the base object');
test(function () {
    // WPT's getElementsByTagName("p") is fresh in its own document; 'p' here
    // already has an expando from the own-props cases above.
    const element = append(body, 'em', { id: 'named' });
    this.add_cleanup(() => T.remove(element));
    const collection = byTag('em');
    assert_equals(collection.named, element);
    const object = Object.create(collection);
    assert_equals(object.named, element);
    object.named = 'foo';
    assert_equals(object.named, 'foo');
    assert_equals(collection.named, element);
}, 'HTMLCollection as a prototype and setting own properties');

// ---- dom/collections/HTMLCollection-delete.html
{
    const expected = append(body, 'i', { id: 'foo' }), c = byTag('i');
    test(() => { assert_equals(c[0], expected, 'before'); delete c[0]; assert_equals(c[0], expected, 'after'); }, 'Loose id');
    test(() => { assert_equals(c[0], expected, 'before'); assert_throws_js(TypeError, function () { 'use strict'; delete c[0]; }); assert_equals(c[0], expected, 'after'); }, 'Strict id');
    test(() => { assert_equals(c.foo, expected, 'before'); delete c.foo; assert_equals(c.foo, expected, 'after'); }, 'Loose name');
    test(() => { assert_equals(c.foo, expected, 'before'); assert_throws_js(TypeError, function () { 'use strict'; delete c.foo; }); assert_equals(c.foo, expected, 'after'); }, 'Strict name');
    T.remove(expected);
}

// ---- dom/collections/HTMLCollection-empty-name.html
test(function () {
    const root = el('div');
    T.insert(root, el('div', { id: '' }), -1); T.insert(root, el('div', { name: '' }), -1); T.insert(root, el('a', { name: '' }), -1);
    for (const c of [byTag('*', root), byTag('a', root), B.children(root, true)]) {
        assert_false('' in c, 'Empty string should not be in the collection.');
        assert_equals(c[''], undefined, 'Named getter should return undefined for empty string.');
        assert_equals(c.namedItem(''), null, 'namedItem should return null for empty string.');
    }
}, 'Empty string as a name');

// ---- dom/collections/HTMLCollection-supported-property-names.html
test(function () {
    const root = el('div');
    for (const attrs of [{}, { id: '' }, { id: 'some-id' }, { id: 'some-id' }, { name: '' }, { name: 'some-name' }, { name: 'some-name' }, { id: 'another-id', name: 'another-name' }])
        T.insert(root, el('span', attrs), -1);
    assert_array_equals(Object.getOwnPropertyNames(byTag('span', root)),
        ['0', '1', '2', '3', '4', '5', '6', '7', 'some-id', 'some-name', 'another-id', 'another-name']);
}, 'Object.getOwnPropertyNames on HTMLCollection');
test(function () {
    const root = el('div');
    T.insert(root, el('foo', { name: 'some-name' }, false), -1);
    assert_array_equals(Object.getOwnPropertyNames(byTag('foo', root)), ['0']);
}, 'Object.getOwnPropertyNames on HTMLCollection with non-HTML namespace');
test(function () {
    const root = el('div');
    T.insert(root, el('foo'), -1);
    const elements = byTag('foo', root);
    elements.someProperty = 'some value';
    assert_array_equals(Object.getOwnPropertyNames(elements), ['0', 'someProperty']);
}, 'Object.getOwnPropertyNames on HTMLCollection with expando object');
{
    const holder = append(body, 'div');
    for (const id of ['some-id', 'some-id']) T.insert(holder, el('span', { id }), -1);
    test(function () {
        const elements = byTag('span'), old_item = elements['some-id'];
        const old_desc = Object.getOwnPropertyDescriptor(elements, 'some-id');
        assert_equals(old_desc.value, old_item);
        assert_false(old_desc.enumerable); assert_true(old_desc.configurable); assert_false(old_desc.writable);
        elements['some-id'] = 5;
        assert_equals(elements['some-id'], old_item);
        assert_throws_js(TypeError, function () { 'use strict'; elements['some-id'] = 5; });
        assert_throws_js(TypeError, function () { Object.defineProperty(elements, 'some-id', { value: 5 }); });
        delete elements['some-id'];
        assert_equals(elements['some-id'], old_item);
        assert_throws_js(TypeError, function () { 'use strict'; delete elements['some-id']; });
        assert_equals(elements['some-id'], old_item);
    }, 'Trying to set an expando that would shadow an already-existing named property');
    test(function () {
        const elements = byTag('span');
        assert_equals(elements['new-id'], undefined);
        assert_equals(Object.getOwnPropertyDescriptor(elements, 'new-id'), undefined);
        elements['new-id'] = 5;
        assert_equals(elements['new-id'], 5);
        const span = append(body, 'span', { id: 'new-id' });
        this.add_cleanup(() => T.remove(span));
        assert_equals(elements.namedItem('new-id'), span);
        assert_equals(elements['new-id'], 5);
        delete elements['new-id'];
        assert_equals(elements['new-id'], span);
    }, 'Trying to set an expando that shadows a named property that gets added later');
    test(function () {
        const elements = byTag('span');
        assert_equals(elements['new-id2'], undefined);
        Object.defineProperty(elements, 'new-id2', { configurable: false, writable: false, value: 5 });
        assert_equals(elements['new-id2'], 5);
        const span = append(body, 'span', { id: 'new-id2' });
        this.add_cleanup(() => T.remove(span));
        assert_equals(elements.namedItem('new-id2'), span);
        assert_equals(elements['new-id2'], 5);
        delete elements['new-id2'];
        assert_equals(elements['new-id2'], 5);
        assert_throws_js(TypeError, function () { 'use strict'; delete elements['new-id2']; });
        assert_equals(elements['new-id2'], 5);
    }, 'Trying to set a non-configurable expando that shadows a named property that gets added later');
    T.remove(holder);
}

// ---- dom/collections/HTMLCollection-supported-property-indices.html
{
    const root = el('div'), byId = {};
    for (const id of ['-2', '-1', '0', '1', '2147483645', '2147483646', '2147483647', '2147483648', '2147483649',
                      '4294967293', '4294967294', '4294967295', '4294967296', '4294967297']) {
        byId[id] = el('foo', { id }); T.insert(root, byId[id], -1);
    }
    const collection = byTag('foo', root);
    test(function () {
        assert_equals(collection.item(-2), null); assert_equals(collection.item(-1), null);
        assert_equals(collection.namedItem(-2), byId['-2']); assert_equals(collection.namedItem(-1), byId['-1']);
        assert_equals(collection[-2], byId['-2']); assert_equals(collection[-1], byId['-1']);
    }, 'Handling of property names that look like negative integers');
    test(function () {
        assert_equals(collection.item(0), byId['-2']); assert_equals(collection.item(1), byId['-1']);
        assert_equals(collection.namedItem(0), byId['0']); assert_equals(collection.namedItem(1), byId['1']);
        assert_equals(collection[0], byId['-2']); assert_equals(collection[1], byId['-1']);
    }, 'Handling of property names that look like small nonnegative integers');
    test(function () {
        for (const n of [2147483645, 2147483646, 2147483647, 2147483648, 2147483649]) {
            assert_equals(collection.item(n), null);
            assert_equals(collection.namedItem(n), byId[String(n)]);
            assert_equals(collection[n], undefined);
        }
    }, 'Handling of property names that look like integers around 2^31');
    test(function () {
        assert_equals(collection.item(4294967293), null); assert_equals(collection.item(4294967294), null);
        assert_equals(collection.item(4294967295), null);
        assert_equals(collection.item(4294967296), byId['-2']); assert_equals(collection.item(4294967297), byId['-1']);
        for (const n of [4294967293, 4294967294, 4294967295, 4294967296, 4294967297]) assert_equals(collection.namedItem(n), byId[String(n)]);
        assert_equals(collection[4294967293], undefined); assert_equals(collection[4294967294], undefined);
        assert_equals(collection[4294967295], byId['4294967295']);
        assert_equals(collection[4294967296], byId['4294967296']);
        assert_equals(collection[4294967297], byId['4294967297']);
    }, 'Handling of property names that look like integers around 2^32');
    test(function () {
        const elements = collection, old_item = elements[0];
        const old_desc = Object.getOwnPropertyDescriptor(elements, 0);
        assert_equals(old_desc.value, old_item);
        assert_true(old_desc.enumerable); assert_true(old_desc.configurable); assert_false(old_desc.writable);
        elements[0] = 5;
        assert_equals(elements[0], old_item);
        assert_throws_js(TypeError, function () { 'use strict'; elements[0] = 5; });
        assert_throws_js(TypeError, function () { Object.defineProperty(elements, 0, { value: 5 }); });
        delete elements[0];
        assert_equals(elements[0], old_item);
        assert_throws_js(TypeError, function () { 'use strict'; delete elements[0]; });
        assert_equals(elements[0], old_item);
    }, 'Trying to set an expando that would shadow an already-existing indexed property');
    test(function () {
        const elements = collection, idx = elements.length;
        assert_equals(elements[idx], undefined);
        assert_equals(Object.getOwnPropertyDescriptor(elements, idx), undefined);
        elements[idx] = 5;
        assert_equals(elements[idx], undefined);
        assert_throws_js(TypeError, function () { 'use strict'; elements[idx] = 5; });
        assert_throws_js(TypeError, function () { Object.defineProperty(elements, idx, { value: 5 }); });
        delete elements[idx];
        (function () { 'use strict'; delete elements[idx]; })();
    }, 'Trying to set an expando with an indexed property name past the end of the list');
    test(function () {
        const elements = collection, old_item = elements[0];
        Object.prototype[0] = 5;
        this.add_cleanup(function () { delete Object.prototype[0]; });
        assert_equals(elements[0], old_item);
        delete elements[0];
        assert_equals(elements[0], old_item);
        assert_throws_js(TypeError, function () { 'use strict'; delete elements[0]; });
        assert_equals(elements[0], old_item);
    }, 'Trying to delete an indexed property name should never work');
}

// ---- dom/nodes/NodeList-static-length-getter-tampered-*.html, at a hundredth
// of the iterations (the expectations, not the time).
{
    const makeStaticNodeList = length => { const a = []; for (let i = 0; i < length; i++) a.push(el('span')); return B.list(a); };
    const indexOfNodeList = nodeList => {
        const node = nodeList[50]; let index = -1;
        for (let i = 0; i < 1e3 / 2; i++) for (let j = 0; j < nodeList.length; j++) if (nodeList[j] === node) { index = j; break; }
        return index;
    };
    const arrayIndexOfNodeList = nodeList => { const node = nodeList[50]; let index; for (let i = 0; i < 1e3; i++) index = Array.prototype.indexOf.call(nodeList, node); return index; };
    for (const [name, search] of [['', indexOfNodeList], [' (Array.prototype.indexOf)', arrayIndexOfNodeList]]) {
        test(function () {
            const nodeList = makeStaticNodeList(100);
            for (let i = 0; i < 50; i++) {
                if (i === 25) Object.defineProperty(nodeList, 'length', { get() { return 10; } });
                assert_equals(search(nodeList), i >= 25 ? -1 : 50);
            }
        }, 'NodeList (static collection) "length" getter tampered' + name + ', own property');
        test(function () {
            const nodeList = makeStaticNodeList(100);
            for (let i = 0; i < 50; i++) {
                if (i === 25) Object.setPrototypeOf(nodeList, { get length() { return 10; }, __proto__: NodeList.prototype });
                assert_equals(search(nodeList), i >= 25 ? -1 : 50);
            }
        }, 'NodeList (static collection) "length" getter tampered' + name + ', prototype');
        test(function () {
            const nodeList = makeStaticNodeList(100), saved = Object.getOwnPropertyDescriptor(NodeList.prototype, 'length');
            this.add_cleanup(() => Object.defineProperty(NodeList.prototype, 'length', saved));
            for (let i = 0; i < 50; i++) {
                if (i === 25) Object.defineProperty(NodeList.prototype, 'length', { get() { return 10; }, configurable: true });
                assert_equals(search(nodeList), i >= 25 ? -1 : 50);
            }
        }, 'NodeList (static collection) "length" getter tampered' + name + ', interface');
    }
}

// ---- Web IDL's other internal methods and the interfaces' members.
test(function () {
    const c = byTag('*', el('div'));
    assert_throws_js(TypeError, () => Object.preventExtensions(c));
    assert_false(Reflect.preventExtensions(c));
    assert_true(Object.isExtensible(c));
    assert_throws_js(TypeError, () => Object.freeze(c));
    assert_throws_js(TypeError, () => Object.seal(c));
}, '[[PreventExtensions]] returns false');
test(function () {
    const root = el('div'), a = el('i', { id: 'x' }), b = el('i', { name: 'y' });
    T.insert(root, a, -1); T.insert(root, b, -1);
    const c = byTag('i', root);
    assert_true(0 in c); assert_true(1 in c); assert_false(2 in c);
    assert_true('x' in c); assert_true('y' in c); assert_false('z' in c);
    assert_true(Object.prototype.hasOwnProperty.call(c, 'x')); assert_true(Object.prototype.hasOwnProperty.call(c, 0));
    assert_array_equals([...c], [a, b]);
    assert_equals(c.item(1), b); assert_equals(c.item(2), null);
    assert_throws_js(TypeError, () => c.item(1n));
    assert_throws_js(TypeError, () => c.item(Symbol()));
    assert_equals(c.namedItem('y'), b); assert_equals(c.namedItem('z'), null);
    const keys = []; for (const k in c) keys.push(k);
    assert_equals(keys.slice(0, 2).join(), '0,1');
    assert_false(keys.includes('x'), 'named properties are not enumerable');
    // A property on the prototype chain hides a supported name.
    T.insert(root, el('i', { id: 'item' }), -1);
    assert_equals(c.item, HTMLCollection.prototype.item);
    assert_false(Object.getOwnPropertyNames(c).includes('item'));
    assert_true(c.namedItem('item') !== null);
    assert_equals(Object.getOwnPropertyDescriptor(c, 'item'), undefined);
}, 'HTMLCollection members and named property visibility');
test(function () {
    const root = el('div'), list = B.children(root, false), children = B.children(root, true);
    assert_equals(B.children(root, false), list, 'same childNodes object');
    assert_equals(B.children(root, true), children, 'same children object');
    assert_equals(list.length, 0);
    const a = el('i'), text = T.text(doc);
    T.insert(root, a, -1); T.insert(root, text, -1);
    assert_equals(list.length, 2); assert_equals(list[0], a); assert_equals(list[1], text);
    assert_equals(children.length, 1); assert_equals(children[0], a);
    const seen = []; list.forEach((n, i, l) => seen.push([n, i, l]));
    assert_equals(seen.length, 2); assert_equals(seen[0][0], a); assert_equals(seen[1][1], 1); assert_equals(seen[0][2], list);
    assert_array_equals([...list.keys()], [0, 1]); assert_array_equals([...list.values()], [a, text]);
    assert_equals([...list.entries()][1][1], text);
    assert_equals(Object.prototype.toString.call(list), '[object NodeList]');
    assert_equals(Object.prototype.toString.call(children), '[object HTMLCollection]');
    assert_true(list instanceof NodeList); assert_true(children instanceof HTMLCollection);
    assert_throws_js(TypeError, () => new NodeList());
    assert_throws_js(TypeError, () => NodeList.prototype.item.call({}, 0));
    assert_throws_js(TypeError, () => list.item(), 'item requires its argument');
    assert_throws_js(TypeError, () => children.namedItem(), 'namedItem requires its argument');
    assert_throws_js(TypeError, () => children.namedItem(Symbol()), 'DOMString conversion');
    // Each member takes only an object implementing its interface.
    const lengthOf = C => Object.getOwnPropertyDescriptor(C.prototype, 'length').get;
    assert_throws_js(TypeError, () => lengthOf(NodeList).call({}));
    assert_throws_js(TypeError, () => lengthOf(NodeList).call(children), 'NodeList length on an HTMLCollection');
    assert_throws_js(TypeError, () => lengthOf(HTMLCollection).call(list), 'HTMLCollection length on a NodeList');
    assert_throws_js(TypeError, () => HTMLCollection.prototype.namedItem.call(list, 'x'), 'namedItem on a NodeList');
    assert_throws_js(TypeError, () => HTMLCollection.prototype.item.call(list, 0), 'HTMLCollection item on a NodeList');
    assert_throws_js(TypeError, () => NodeList.prototype.item.call(children, 0), 'NodeList item on an HTMLCollection');
    const OtherList = function () {}, other = B.typed(() => [a], OtherList.prototype);
    assert_equals(other[0], a, 'another interface keeps the indexed properties');
    assert_throws_js(TypeError, () => NodeList.prototype.item.call(other, 0), 'NodeList item on another interface');
    assert_throws_js(TypeError, () => lengthOf(NodeList).call(other), 'NodeList length on another interface');
    const RadioLike = Object.create(NodeList.prototype), radio = B.typed(() => [a], RadioLike);
    assert_equals(radio.length, 1, 'an interface inheriting NodeList'); assert_equals(radio.item(0), a);
    const optionsLike = B.options(() => [a], Object.create(HTMLCollection.prototype), () => {});
    assert_equals(optionsLike.length, 1, 'an interface inheriting HTMLCollection'); assert_equals(optionsLike.namedItem('x'), null);
    T.remove(a);
    assert_equals(list.length, 1); assert_equals(list[1], undefined); assert_equals(children.length, 0);
}, 'NodeList members and liveness');
test(function () {
    const root = el('div'), a = el('i', { id: 'café' }), b = el('i', { id: '名前' });
    T.insert(root, a, -1); T.insert(root, b, -1);
    const c = byTag('i', root);
    assert_equals(c['café'], a); assert_equals(c['名前'], b);
    assert_array_equals(Object.getOwnPropertyNames(c), ['0', '1', 'café', '名前']);
    T.attr(a, 'id', 'other');
    assert_equals(c['café'], undefined); assert_equals(c.other, a);
}, 'Supported property names are the ID and name values as property keys');
test(function () {
    const a = el('input', { name: 'x' }), b = el('input', { id: 'y' }), radio = { radio: true };
    const c = B.form(() => [a, b], Object.create(HTMLCollection.prototype), name => name === 'x' ? radio : name === 'y' ? b : null);
    assert_equals(c.x, radio); assert_equals(c.y, b); assert_equals(c.z, undefined);
    assert_prop_desc_equals(c, 'x', { value: radio, writable: false, enumerable: false, configurable: true });
    assert_array_equals(Object.getOwnPropertyNames(c), ['0', '1', 'x', 'y']);
    assert_throws_js(TypeError, () => { 'use strict'; delete c.x; });
}, "An interface's own named getter (HTMLFormControlsCollection)");

// ---- What each collection reads, and when (carried over from the host test
// of the JS-only bridge, tests/js_collections_hosttest.js).
test(function () {
    let reads = 0;
    const A = T.doc(), other = T.doc();
    const root = el('div', {}, true, A), one = el('i', { id: 'one' }, true, A), two = el('i', { id: 'two' }, true, A), text = T.text(A);
    T.insert(root, one, -1); T.insert(root, two, -1); T.insert(root, text, -1);
    const query = () => { reads++; return T.get(root, 'childNodes').filter(n => T.get(n, 'nodeType') === 1); };
    const live = B.domHTML(root, query);
    assert_equals(live.length, 2, 'initial length'); assert_equals(live[0], one, 'initial index'); assert_equals(live.item(1), two, 'item');
    assert_equals(live.namedItem('one'), one, 'named item'); assert_equals(live.one, one, 'named property'); assert_true('1' in live, 'has index');
    assert_equals(Object.getOwnPropertyDescriptor(live, '0').value, one, 'descriptor'); assert_equals(Object.keys(live).join(), '0,1', 'keys');
    assert_equals(Array.from(live).length, 2, 'iterator'); assert_equals(reads, 1, 'all reads share one snapshot');
    for (let i = 0; i < 100; i++) { assert_equals(live.length, 2); assert_equals(live[i % 2], i % 2 ? two : one); }
    assert_equals(reads, 1, 'length/index loop does not repeat query');
    const three = el('i', { id: 'three' }, true, A);
    T.insert(root, three, -1);
    assert_equals(live.length, 3, 'tree mutation live'); assert_equals(live[2], three, 'new index'); assert_equals(reads, 2, 'one new query');
    T.attr(one, 'id', 'renamed');
    assert_equals(live.one, undefined, 'old named property gone'); assert_equals(live.renamed, one, 'new named property live');
    assert_equals(reads, 3, 'attribute revision invalidates');
    T.remove(two);
    assert_equals(live.item(1), three, 'remove updates order');
    T.insert(root, two, 0);
    assert_equals(live[0], two, 'reinsert updates order');
    // Adoption into another Document with exactly the same counter.
    T.setRevision(other, T.revision(A));
    const beforeAdopt = reads;
    T.adopt(root, other);
    for (const k of T.get(root, 'childNodes')) T.remove(k, true);
    T.insert(root, two, -1, true);
    assert_equals(live.length, 1, 'owner identity invalidates'); assert_equals(reads, beforeAdopt + 1, 'adoption read with equal revisions');
    T.insert(root, one, -1, true); T.setRevision(A, T.revision(A) + 1n);
    assert_equals(live.length, 1, 'other document revision is not the owner revision');
    T.setRevision(other, T.revision(other) + 1n);
    assert_equals(live.length, 2, 'actual owner mutation invalidates');
    // The whole unsigned 64-bit counter, beyond Number.MAX_SAFE_INTEGER.
    T.setRevision(other, 9007199254740992n);
    assert_equals(live.length, 2, 'large revision');
    T.insert(root, three, -1, true); T.setRevision(other, 9007199254740993n);
    assert_equals(live.length, 3, 'adjacent uint64 revisions distinct');
    const nodes = B.children(root, false), elements = B.children(root, true);
    T.insert(root, text, -1, true); T.setRevision(other, T.revision(other) + 1n);
    const beforeChildren = T.childReads();
    assert_equals(nodes.length, 4, 'childNodes text included'); assert_equals(nodes[3], text, 'childNodes text index'); assert_equals(nodes.item(0), two, 'childNodes item');
    assert_equals(elements.length, 3, 'children elements only'); assert_equals(elements[2], three, 'children index');
    assert_equals(T.childReads(), beforeChildren + 2, 'one read per cached children type');
    // A replaced Array helper never sees a collection's items.
    const saved = [Array.prototype.find, Array.prototype.map, Array.prototype.filter];
    try {
        Array.prototype.find = function () { throw Error('author find saw backing array'); };
        assert_equals(live.namedItem('two'), two, 'captured find');
        Array.prototype.map = function () { throw Error('author map saw backing array'); };
        assert_equals(Object.keys(live).length, 3, 'captured map');
        Array.prototype.filter = function () { throw Error('author filter saw backing array'); };
        T.setRevision(other, T.revision(other) + 1n);
        assert_equals(elements.length, 3, 'captured child filter');
    } finally { [Array.prototype.find, Array.prototype.map, Array.prototype.filter] = saved; }
    const nodeList = B.domLive(root, () => { reads++; return T.get(root, 'childNodes'); });
    const visited = [];
    nodeList.forEach((n, i) => { visited.push(n); if (i === 0) T.remove(one); });
    assert_equals(visited.length, 3, 'forEach skips vanished last index'); assert_equals(visited[1], three, 'forEach mutation live');
    const iter = nodeList.values();
    assert_equals(iter.next().value, two, 'iterator first');
    T.insert(root, one, 0);
    assert_equals(iter.next().value, two, 'iterator sees shifted native index');
    let attempts = 0;
    const retry = B.domLive(root, () => { if (++attempts === 1) throw Error('read failed'); return [one]; });
    let threw = false;
    try { void retry.length; } catch (e) { threw = e.message === 'read failed'; }
    assert_true(threw, 'read failure propagated'); assert_equals(retry.length, 1, 'read failure not cached');
    assert_equals(retry[0], one, 'retry index'); assert_equals(attempts, 2, 'successful retry reused');
    assert_throws_js(TypeError, () => B.domLive(null, () => { throw Error('reader must not run'); }), 'a DOM-tree collection needs a node');
    assert_throws_js(TypeError, () => B.domHTML({}, () => { throw Error('reader must not run'); }), 'a DOM-tree collection needs a node');
    // Readers of state other than the tree are called for every access.
    let current = [one], genericReads = 0;
    const generic = () => { genericReads++; return current; };
    for (const build of [B.live, B.html, r => B.typed(r, NodeList.prototype),
                         r => B.options(r, HTMLCollection.prototype, () => {}), r => B.form(r, HTMLCollection.prototype, () => null)]) {
        current = [one];
        const list = build(generic), before = genericReads;
        assert_equals(list.length, 1, 'generic length');
        current = [two, three];
        assert_equals(list[0], two, 'generic state change without DOM revision'); assert_equals(list.length, 2, 'generic new length');
        assert_equals(genericReads, before + 3, 'generic read remains uncached');
    }
    const staticList = B.list([one, two]);
    for (const k of T.get(root, 'childNodes')) T.remove(k);
    assert_equals(staticList.length, 2, 'static list unchanged'); assert_equals(staticList[1], two, 'static list identity');
    // A full named enumeration and every descriptor read the IDs and names once.
    const many = el('div', {}, true, other), kids = [];
    for (let i = 0; i < 2048; i++) { const k = el('i', { id: 'id' + i }, true, other); kids.push(k); T.insert(many, k, -1); }
    T.attr(kids[0], 'name', 'shared'); T.attr(kids[1], 'id', 'shared');
    const manyLive = B.domHTML(many, () => { reads++; return T.get(many, 'childNodes'); });
    const beforeNames = T.attrReads(), allKeys = Reflect.ownKeys(manyLive);
    for (const key of allKeys) Object.getOwnPropertyDescriptor(manyLive, key);
    assert_equals(allKeys.length, 2048 + 2048, 'indices and names');
    assert_equals(T.attrReads() - beforeNames, 4096, 'one linear ID/name snapshot for all descriptors');
    assert_equals(manyLive.namedItem('shared'), kids[0], 'tree-order name before later ID');
    assert_equals(manyLive.shared, kids[0], 'property uses same first occurrence');
    T.attr(kids[0], 'name', '');
    assert_equals(manyLive.namedItem('shared'), kids[1], 'attribute mutation invalidates named index');
    // Names of a reader of other state follow that state.
    const genericHTML = B.html(() => [one]);
    T.attr(one, 'id', 'before', true);
    assert_equals(genericHTML.namedItem('before'), one, 'generic first named state');
    T.attr(one, 'id', 'after', true);
    assert_equals(genericHTML.namedItem('before'), null, 'generic names not cached without DOM revision');
    assert_equals(genericHTML.namedItem('after'), one, 'generic state change with same backing array');
}, 'What each collection reads');

T.print('collections native: ' + (passed + failed) + ' cases, ' + failed + ' failed');
failed;
