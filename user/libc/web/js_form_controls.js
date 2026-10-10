/* Input conversion is native, shared with typing/reset/submission. These IDL
   accessors deliberately do not promise a date/color/file picker UI. */
const formControlBridge = (() => {
    const string = String, NativeDate = Date;
    const dateTime = Date.prototype.getTime, finite = Number.isFinite;
    const reflect = reflectedAttr, call = apply;
    function text(value) {
        if (typeof value === 'symbol') throw new TypeError('Cannot convert a Symbol to a string');
        return string(value);
    }
    function numeric(value) {
        /* Number(BigInt) is allowed by the constructor, not Web IDL ToNumber. */
        if (typeof value === 'bigint' || typeof value === 'symbol') throw new TypeError('Expected a number');
        return +value;
    }
    function brand(node) { htmlElementBrand(node, 'input'); }
    function result(status) {
        if (status === 1) throw new DOMException('This operation does not apply to this input state', 'InvalidStateError');
        if (status === 2) throw new TypeError('An infinite input value is not allowed');
        if (status === 3) throw new RangeError('Input value storage exceeded its budget');
    }
    function type(node) { brand(node); return rawDom.formValue(node, 'type'); }
    function valueMode(kind) {
        return kind === 'checkbox' || kind === 'radio' ? 2 : kind === 'file' ? 3 :
            ['hidden','submit','image','reset','button'].includes(kind) ? 1 : 0;
    }
    Object.defineProperties(HTMLInputElement.prototype, {
        type: {configurable:true, enumerable:true,
            get() { return type(this); },
            set(value) { brand(this); reflect(this, 'type', text(value)); }
        },
        value: {configurable:true, enumerable:true,
            get() {
                const mode = valueMode(type(this));
                if (mode === 1 || mode === 2) {
                    const value = reflect(this, 'value');
                    return value === null ? mode === 2 ? 'on' : '' : value;
                }
                return rawDom.get(this, mode === 3 ? 'fileValue' : 'value');
            },
            set(value) {
                const mode = valueMode(type(this));
                value = value === null ? '' : text(value); // LegacyNullToEmptyString
                if (mode === 1 || mode === 2) reflect(this, 'value', value);
                else if (mode === 3 && value !== '') throw new DOMException('File inputs cannot be assigned a filename', 'InvalidStateError');
                else dom('set', this, 'value', value);
            }
        },
        valueAsNumber: {configurable:true, enumerable:true,
            get() { brand(this); return rawDom.formValue(this, 'number'); },
            set(value) { brand(this); result(rawDom.formValue(this, 'setNumber', numeric(value))); }
        },
        valueAsDate: {configurable:true, enumerable:true,
            get() {
                brand(this); const value = rawDom.formValue(this, 'date');
                if (!finite(value)) return null;
                const date = new NativeDate(value);
                return finite(call(dateTime, date, [])) ? date : null;
            },
            set(value) {
                const kind = type(this);
                if (!['date','month','week','time'].includes(kind)) result(1);
                let time = NaN;
                if (value !== null) time = call(dateTime, value, []); // intrinsic Date brand check
                result(rawDom.formValue(this, 'setDate', time));
            }
        },
        stepUp: {configurable:true, enumerable:true, writable:true,
            value: function stepUp(count) {
                brand(this); count = count === undefined ? 1 : numeric(count) >> 0;
                result(rawDom.formValue(this, 'stepUp', count));
            }
        },
        stepDown: {configurable:true, enumerable:true, writable:true,
            value: function stepDown(count) {
                brand(this); count = count === undefined ? 1 : numeric(count) >> 0;
                result(rawDom.formValue(this, 'stepDown', count));
            }
        }
    });
    for (const name of ['min','max','step']) Object.defineProperty(HTMLInputElement.prototype, name, {
        configurable:true, enumerable:true,
        get() { brand(this); return reflect(this, name) || ''; },
        set(value) { brand(this); reflect(this, name, text(value)); }
    });

    const optionCollections = new WeakMap(), selectedCollections = new WeakMap(), optionOwners = new WeakMap();
    function selectBrand(node) { htmlElementBrand(node, 'select'); }
    function optionBrand(node) { htmlElementBrand(node, 'option'); }
    function nativeOptions(node) { return rawDom.get(node, 'selectOptions'); }
    function collectionOwner(collection) {
        const node = optionOwners.get(collection);
        if (!node) throw new TypeError('Illegal options collection receiver');
        return node;
    }
    function optionElement(element, allowGroup) {
        const tag = rawDom.get(element, 'localName');
        if (rawDom.get(element, 'nodeType') !== 1 || rawDom.get(element, 'namespaceURI') !== 'http://www.w3.org/1999/xhtml' ||
            (tag !== 'option' && !(allowGroup && tag === 'optgroup'))) throw new TypeError('Expected an HTML option element');
    }
    function lengthSet(node, length) {
        selectBrand(node); length = numeric(length) >>> 0;
        const values = nativeOptions(node);
        // The HTML algorithm ignores expansions beyond 100000 options.
        if (length > values.length && length > 100000) return;
        for (let i = values.length - 1; i >= length; i--) values[i].remove();
        for (let i = values.length; i < length; i++) node.appendChild(node.ownerDocument.createElement('option'));
    }
    function removeOption(node, index) {
        selectBrand(node); index = numeric(index) >> 0;
        const option = nativeOptions(node)[index];
        if (option) option.remove();
    }
    function addOption(node, element, before) {
        selectBrand(node); optionElement(element, true);
        if (element.contains(node)) throw new DOMException('The inserted element is an ancestor of this select', 'HierarchyRequestError');
        let anchor = null;
        if (before !== null && before !== undefined) {
            if (typeof before === 'object' && before instanceof HTMLElement) {
                rawDom.get(before, 'nodeType'); anchor = before;
                if (anchor === node || !node.contains(anchor)) throw new DOMException('The reference is not a descendant of this select', 'NotFoundError');
            } else anchor = nativeOptions(node)[numeric(before) >> 0] || null;
        }
        if (element === anchor) return;
        if (anchor) anchor.parentNode.insertBefore(element, anchor);
        else node.appendChild(element);
    }
    function indexedOptionSet(node, index, option) {
        if (option === null) { const old = nativeOptions(node)[index]; if (old) old.remove(); return; }
        optionElement(option, false);
        const values = nativeOptions(node), old = values[index];
        if (old === option) return;
        if (!old) {
            if (index >= 100000) throw new RangeError('Options allocation exceeded its budget');
            lengthSet(node, index); node.appendChild(option);
        } else old.parentNode.replaceChild(option, old);
    }
    const htmlLength = Object.getOwnPropertyDescriptor(HTMLCollection.prototype, 'length').get;
    class HTMLOptionsCollection extends HTMLCollection {
        constructor() { super(); }
        get length() { collectionOwner(this); return call(htmlLength, this, []); }
        set length(value) { lengthSet(collectionOwner(this), value); }
        get selectedIndex() { return collectionOwner(this).selectedIndex; }
        set selectedIndex(value) { collectionOwner(this).selectedIndex = value; }
        add(element, before) {
            const owner = collectionOwner(this);
            if (!arguments.length) throw new TypeError('add requires an element');
            addOption(owner, element, before);
        }
        remove(index) {
            const owner = collectionOwner(this);
            if (!arguments.length) throw new TypeError('remove requires an index');
            removeOption(owner, index);
        }
    }
    Object.defineProperty(HTMLOptionsCollection.prototype, Symbol.toStringTag, {value:'HTMLOptionsCollection', configurable:true});
    globalThis.HTMLOptionsCollection = HTMLOptionsCollection;
    Object.defineProperties(HTMLSelectElement.prototype, {
        options: {configurable:true, enumerable:true, get() {
            selectBrand(this); let collection = optionCollections.get(this);
            if (!collection) {
                const owner = this;
                collection = collectionBridge.options(() => nativeOptions(owner), HTMLOptionsCollection.prototype,
                    (index, option) => indexedOptionSet(owner, index, option));
                optionOwners.set(collection, owner); optionCollections.set(owner, collection);
            }
            return collection;
        }},
        selectedOptions: {configurable:true, enumerable:true, get() {
            selectBrand(this); let collection = selectedCollections.get(this);
            if (!collection) {
                const owner = this;
                collection = collectionBridge.html(() => nativeOptions(owner).filter(option => rawDom.get(option, 'selected')));
                selectedCollections.set(owner, collection);
            }
            return collection;
        }},
        length: {configurable:true, enumerable:true, get() { selectBrand(this); return nativeOptions(this).length; }, set(value) { lengthSet(this, value); }},
        selectedIndex: {configurable:true, enumerable:true, get() { selectBrand(this); return rawDom.get(this, 'selectedIndex'); },
            set(value) { selectBrand(this); dom('set', this, 'selectedIndex', numeric(value) >> 0); }},
        value: {configurable:true, enumerable:true, get() { selectBrand(this); return rawDom.get(this, 'value'); },
            set(value) { selectBrand(this); dom('set', this, 'value', text(value)); }},
        item: {configurable:true, enumerable:true, writable:true, value:function item(index) {
            selectBrand(this); if (!arguments.length) throw new TypeError('item requires an index'); return this.options.item(index);
        }},
        namedItem: {configurable:true, enumerable:true, writable:true, value:function namedItem(name) {
            selectBrand(this); if (!arguments.length) throw new TypeError('namedItem requires a name'); return this.options.namedItem(name);
        }},
        add: {configurable:true, enumerable:true, writable:true, value:function add(element, before) {
            selectBrand(this); if (!arguments.length) throw new TypeError('add requires an element'); addOption(this, element, before);
        }},
        remove: {configurable:true, enumerable:true, writable:true, value:function remove(index) {
            selectBrand(this); if (arguments.length) removeOption(this, index);
            else if (this.parentNode) this.parentNode.removeChild(this);
        }}
    });
    Object.defineProperties(HTMLOptionElement.prototype, {
        selected: {configurable:true, enumerable:true, get() { optionBrand(this); return rawDom.get(this, 'selected'); },
            set(value) { optionBrand(this); dom('set', this, 'selected', !!value); }},
        value: {configurable:true, enumerable:true, get() { optionBrand(this); return rawDom.get(this, 'value'); },
            set(value) { optionBrand(this); reflect(this, 'value', text(value)); }},
        text: {configurable:true, enumerable:true, get() { optionBrand(this); return rawDom.get(this, 'optionText'); },
            set(value) { optionBrand(this); dom('set', this, 'textContent', text(value)); }},
        label: {configurable:true, enumerable:true, get() { optionBrand(this); const label = reflect(this, 'label'); return label === null ? this.text : label; },
            set(value) { optionBrand(this); reflect(this, 'label', text(value)); }},
        index: {configurable:true, enumerable:true, get() { optionBrand(this); return rawDom.get(this, 'optionIndex'); }}
    });
    function Option(optionText = '', optionValue, defaultSelected = false, selected = false) {
        const option = document.createElement('option');
        option.text = text(optionText);
        if (optionValue !== undefined) option.value = text(optionValue);
        option.defaultSelected = !!defaultSelected;
        dom('set', option, 'optionFactorySelected', !!selected);
        return option;
    }
    Object.defineProperty(Option, 'prototype', {value:HTMLOptionElement.prototype, writable:false});
    globalThis.Option = Option;
    /* These collections read native association on every access. No cached
       form owner, labels, selected value, or gauge state belongs to script. */
    const define = Object.defineProperty, fieldsetCollections = new WeakMap(),
        labelCollections = new WeakMap(), datalistCollections = new WeakMap();
    class HTMLLabelElement extends HTMLElement { constructor(){return customElementsBridge.construct(new.target,HTMLLabelElement);} }
    class HTMLDataListElement extends HTMLElement { constructor(){return customElementsBridge.construct(new.target,HTMLDataListElement);} }
    class HTMLProgressElement extends HTMLElement { constructor(){return customElementsBridge.construct(new.target,HTMLProgressElement);} }
    class HTMLMeterElement extends HTMLElement { constructor(){return customElementsBridge.construct(new.target,HTMLMeterElement);} }
    function readonly(C, tag, name, get) {
        define(C.prototype, name, {configurable:true, enumerable:true, get() { htmlElementBrand(this, tag); return get(this); }});
    }
    function reflectedString(C, tag, name, attribute = name) {
        define(C.prototype, name, {configurable:true, enumerable:true,
            get() { htmlElementBrand(this, tag); return reflect(this, attribute) || ''; },
            set(value) { htmlElementBrand(this, tag); reflect(this, attribute, text(value)); }});
    }
    function enumeration(C, tag, name, attribute, values, fallback, invalid = fallback) {
        define(C.prototype, name, {configurable:true, enumerable:true,
            get() { htmlElementBrand(this, tag); const attributeValue = reflect(this, attribute); if (attributeValue === null) return fallback; const value = attributeValue.toLowerCase(); return values.includes(value) ? value : invalid; },
            set(value) { htmlElementBrand(this, tag); reflect(this, attribute, text(value)); }});
    }
    function root(node) { let parent; while ((parent = rawDom.get(node, 'parentNode'))) node = parent; return node; }
    function descendants(node, selector) { return rawDom.query(node, selector, false).filter(n => rawDom.get(n, 'namespaceURI') === 'http://www.w3.org/1999/xhtml'); }
    reflectedString(HTMLLabelElement, 'label', 'htmlFor', 'for');
    readonly(HTMLLabelElement, 'label', 'control', node => rawDom.get(node, 'labelControl'));
    readonly(HTMLLabelElement, 'label', 'form', node => rawDom.get(node, 'form:label'));
    for (const [C, tag] of [[HTMLInputElement,'input'],[HTMLButtonElement,'button'],[HTMLSelectElement,'select'],
        [HTMLTextAreaElement,'textarea'],[HTMLOutputElement,'output'],[HTMLProgressElement,'progress'],[HTMLMeterElement,'meter']]) {
        readonly(C, tag, 'labels', node => {
            if (tag === 'input' && type(node) === 'hidden') return null;
            let collection = labelCollections.get(node);
            if (!collection) {
                collection = collectionBridge.live(() => {
                    const tree = root(node), labels = descendants(tree, 'label');
                    if (rawDom.get(tree, 'nodeType') === 1 && rawDom.get(tree, 'namespaceURI') === 'http://www.w3.org/1999/xhtml' && rawDom.get(tree, 'localName') === 'label') labels.unshift(tree);
                    return labels.filter(label => rawDom.get(label, 'labelControl') === node);
                });
                labelCollections.set(node, collection);
            }
            return collection;
        });
    }
    readonly(HTMLFieldSetElement, 'fieldset', 'elements', node => {
        let collection = fieldsetCollections.get(node);
        if (!collection) {
            collection = collectionBridge.html(() => descendants(node, 'button,fieldset,input,object,output,select,textarea'));
            fieldsetCollections.set(node, collection);
        }
        return collection;
    });
    readonly(HTMLDataListElement, 'datalist', 'options', node => {
        let collection = datalistCollections.get(node);
        if (!collection) { collection = collectionBridge.html(() => descendants(node, 'option')); datalistCollections.set(node, collection); }
        return collection;
    });
    readonly(HTMLInputElement, 'input', 'list', node => rawDom.get(node, 'inputList'));
    define(HTMLInputElement.prototype, 'indeterminate', {configurable:true, enumerable:true,
        get() { brand(this); return rawDom.get(this, 'indeterminate'); },
        set(value) { brand(this); dom('set', this, 'indeterminate', !!value); }});
    for (const dimension of ['width','height']) define(HTMLInputElement.prototype, dimension, {configurable:true, enumerable:true,
        get() { brand(this); return rawDom.get(this, dimension === 'width' ? 'inputWidth' : 'inputHeight'); },
        set(value) { brand(this); reflect(this, dimension, '' + (numeric(value) >>> 0)); }});
    enumeration(HTMLTextAreaElement, 'textarea', 'wrap', 'wrap', ['soft','hard'], 'soft');
    // WHATWG autofill processing: IDL exposes valid, ASCII-normalized tokens,
    // not arbitrary attribute text. Native search history separately enforces
    // its opt-in/privacy policy; reflecting a credential token never saves it.
    const autofillNormal = ('name honorific-prefix given-name additional-name family-name honorific-suffix nickname ' +
        'organization-title username new-password current-password one-time-code organization street-address ' +
        'address-line1 address-line2 address-line3 address-level4 address-level3 address-level2 address-level1 ' +
        'country country-name postal-code cc-name cc-given-name cc-additional-name cc-family-name cc-number ' +
        'cc-exp cc-exp-month cc-exp-year cc-csc cc-type transaction-currency transaction-amount language ' +
        'bday bday-day bday-month bday-year sex url photo').split(' ');
    const autofillContact = 'tel tel-country-code tel-national tel-area-code tel-local tel-local-prefix tel-local-suffix tel-extension email impp'.split(' ');
    function autocompleteValue(node, tag) {
        const attribute = reflect(node, 'autocomplete');
        if (attribute === null) return '';
        const tokens = attribute.split(/[\t\n\f\r ]+/).filter(token => token !== '').map(token =>
            token.replace(/[A-Z]/g, letter => string.fromCharCode(letter.charCodeAt(0) + 32)));
        let index = tokens.length - 1;
        if (index < 0) return '';
        const field = tokens[index];
        if (field === 'on' || field === 'off') return index === 0 && !(tag === 'input' && type(node) === 'hidden') ? field : '';
        let category;
        if (field === 'webauthn') {
            if (index === 0) return field;
            --index;
        }
        if (autofillNormal.includes(tokens[index])) category = 'normal';
        else if (autofillContact.includes(tokens[index])) category = 'contact';
        else return '';
        --index;
        if (category === 'contact' && index >= 0 && ['home','work','mobile','fax','pager'].includes(tokens[index])) --index;
        if (index >= 0 && ['shipping','billing'].includes(tokens[index])) --index;
        if (index >= 0 && !(index === 0 && tokens[index].startsWith('section-'))) return '';
        return tokens.join(' ');
    }
    for (const [C, tag] of [[HTMLInputElement,'input'],[HTMLSelectElement,'select'],[HTMLTextAreaElement,'textarea']])
        define(C.prototype, 'autocomplete', {configurable:true, enumerable:true,
            get() { htmlElementBrand(this, tag); return autocompleteValue(this, tag); },
            set(value) { htmlElementBrand(this, tag); reflect(this, 'autocomplete', text(value)); }});
    enumeration(HTMLFormElement, 'form', 'autocomplete', 'autocomplete', ['on','off'], 'on');
    for (const [C, tag] of [[HTMLInputElement,'input'],[HTMLTextAreaElement,'textarea']]) reflectedString(C, tag, 'dirName', 'dirname');
    // Native focus candidates consume this attribute at the load boundary.
    define(HTMLElement.prototype, 'autofocus', {configurable:true, enumerable:true,
        get() { rawDom.get(this, 'elementBrand'); return reflect(this, 'autofocus') !== null; },
        set(value) { rawDom.get(this, 'elementBrand'); reflect(this, 'autofocus', value ? '' : null); }});
    for (const [C, tag] of [[HTMLInputElement,'input'],[HTMLButtonElement,'button']]) {
        define(C.prototype, 'formAction', {configurable:true, enumerable:true,
            get() { return elementURL.attribute(this, tag, 'formaction', true); },
            set(value) { htmlElementBrand(this, tag); reflect(this, 'formaction', elementURL.scalar(value)); }});
        enumeration(C, tag, 'formMethod', 'formmethod', ['get','post','dialog'], '', 'get');
        enumeration(C, tag, 'formEnctype', 'formenctype', ['application/x-www-form-urlencoded','multipart/form-data','text/plain'], '', 'application/x-www-form-urlencoded');
        reflectedString(C, tag, 'formTarget', 'formtarget');
    }
    for (const [C, tag, names] of [[HTMLProgressElement,'progress',['value','max']],
        [HTMLMeterElement,'meter',['value','min','max','low','high','optimum']]]) {
        for (const name of names) define(C.prototype, name, {configurable:true, enumerable:true,
            get() { htmlElementBrand(this, tag); return rawDom.get(this, 'gauge:' + name); },
            set(value) { htmlElementBrand(this, tag); value = numeric(value); if (!finite(value)) throw new TypeError('Gauge values must be finite'); reflect(this, name, '' + value); }});
    }
    readonly(HTMLProgressElement, 'progress', 'position', node => rawDom.get(node, 'gauge:position'));
    const extraInterfaces = [HTMLLabelElement,HTMLDataListElement,HTMLProgressElement,HTMLMeterElement];
    for (const C of extraInterfaces) { define(C.prototype, Symbol.toStringTag, {value:C.name, configurable:true}); globalThis[C.name] = C; }

    const fileLists = new WeakMap(), fileListSlots = new WeakMap();
    class FileList {
        constructor() { throw new TypeError('Illegal FileList constructor'); }
        get length() { const files = fileListSlots.get(this); if (!files) throw new TypeError('Illegal FileList receiver'); return files.length; }
        item(index) { const files = fileListSlots.get(this); if (!files) throw new TypeError('Illegal FileList receiver'); if (!arguments.length) throw new TypeError('Index required'); return files[numeric(index) >>> 0] || null; }
        *[Symbol.iterator]() { const files = fileListSlots.get(this); if (!files) throw new TypeError('Illegal FileList receiver'); yield* files; }
    }
    function fileList(files) {
        const object = Object.create(FileList.prototype);
        for (let index = 0; index < files.length; index++) define(object, '' + index, {value:files[index], enumerable:true});
        fileListSlots.set(object, files); return object;
    }
    define(FileList.prototype, Symbol.toStringTag, {value:'FileList', configurable:true});
    globalThis.FileList = FileList;
    define(HTMLInputElement.prototype, 'files', {configurable:true, enumerable:true,
        get() {
            brand(this); if (type(this) !== 'file') return null;
            const revision = rawDom.get(this, 'filesRevision');
            let stored = fileLists.get(this);
            if (!stored || stored.revision !== revision) {
                const files = rawDom.get(this, 'filesSnapshot').map(snapshot => blobBridge.nativeFile(snapshot));
                stored = {revision, list:fileList(files)}; fileLists.set(this, stored);
            }
            return stored.list;
        },
        set(value) {
            brand(this); if (value === null) return;
            const files = fileListSlots.get(value); if (!files) throw new TypeError('FileList required');
            if (type(this) !== 'file') return;
            dom('filesSet', this, files.map(file => blobBridge.snapshot(file)));
        }});
    reflectedString(HTMLInputElement, 'input', 'accept');
    define(HTMLInputElement.prototype, 'webkitdirectory', {configurable:true, enumerable:true,
        get() { brand(this); return reflect(this, 'webkitdirectory') !== null; },
        set(value) { brand(this); reflect(this, 'webkitdirectory', value ? '' : null); }});

    return {type,nodeProtos:extraInterfaces.map(C=>C.prototype)};
})();
