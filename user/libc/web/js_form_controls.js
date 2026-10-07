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
    function type(node) { brand(node); return rawDom('formValue', node, 'type'); }
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
                return rawDom('get', this, 'value');
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
            get() { brand(this); return rawDom('formValue', this, 'number'); },
            set(value) { brand(this); result(rawDom('formValue', this, 'setNumber', numeric(value))); }
        },
        valueAsDate: {configurable:true, enumerable:true,
            get() {
                brand(this); const value = rawDom('formValue', this, 'date');
                if (!finite(value)) return null;
                const date = new NativeDate(value);
                return finite(call(dateTime, date, [])) ? date : null;
            },
            set(value) {
                const kind = type(this);
                if (!['date','month','week','time'].includes(kind)) result(1);
                let time = NaN;
                if (value !== null) time = call(dateTime, value, []); // intrinsic Date brand check
                result(rawDom('formValue', this, 'setDate', time));
            }
        },
        stepUp: {configurable:true, enumerable:true, writable:true,
            value: function stepUp(count) {
                brand(this); count = count === undefined ? 1 : numeric(count) >> 0;
                result(rawDom('formValue', this, 'stepUp', count));
            }
        },
        stepDown: {configurable:true, enumerable:true, writable:true,
            value: function stepDown(count) {
                brand(this); count = count === undefined ? 1 : numeric(count) >> 0;
                result(rawDom('formValue', this, 'stepDown', count));
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
    function nativeOptions(node) { return rawDom('get', node, 'selectOptions'); }
    function collectionOwner(collection) {
        const node = optionOwners.get(collection);
        if (!node) throw new TypeError('Illegal options collection receiver');
        return node;
    }
    function optionElement(element, allowGroup) {
        const tag = rawDom('get', element, 'localName');
        if (rawDom('get', element, 'nodeType') !== 1 || rawDom('get', element, 'namespaceURI') !== 'http://www.w3.org/1999/xhtml' ||
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
                rawDom('get', before, 'nodeType'); anchor = before;
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
                collection = collectionBridge.html(() => nativeOptions(owner).filter(option => rawDom('get', option, 'selected')));
                selectedCollections.set(owner, collection);
            }
            return collection;
        }},
        length: {configurable:true, enumerable:true, get() { selectBrand(this); return nativeOptions(this).length; }, set(value) { lengthSet(this, value); }},
        selectedIndex: {configurable:true, enumerable:true, get() { selectBrand(this); return rawDom('get', this, 'selectedIndex'); },
            set(value) { selectBrand(this); dom('set', this, 'selectedIndex', numeric(value) >> 0); }},
        value: {configurable:true, enumerable:true, get() { selectBrand(this); return rawDom('get', this, 'value'); },
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
        selected: {configurable:true, enumerable:true, get() { optionBrand(this); return rawDom('get', this, 'selected'); },
            set(value) { optionBrand(this); dom('set', this, 'selected', !!value); }},
        value: {configurable:true, enumerable:true, get() { optionBrand(this); return rawDom('get', this, 'value'); },
            set(value) { optionBrand(this); reflect(this, 'value', text(value)); }},
        text: {configurable:true, enumerable:true, get() { optionBrand(this); return rawDom('get', this, 'optionText'); },
            set(value) { optionBrand(this); dom('set', this, 'textContent', text(value)); }},
        label: {configurable:true, enumerable:true, get() { optionBrand(this); const label = reflect(this, 'label'); return label === null ? this.text : label; },
            set(value) { optionBrand(this); reflect(this, 'label', text(value)); }},
        index: {configurable:true, enumerable:true, get() { optionBrand(this); return rawDom('get', this, 'optionIndex'); }}
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
    return {type};
})();
