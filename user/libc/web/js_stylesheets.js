/* CSSOM handles address native style/link parser storage and the same cascade
 * AST. Constructed sheets share that AST across native document/shadow scopes. */
(() => {
    'use strict';
    const define = Object.defineProperty, apply = Reflect.apply;
    const StringType = String, Exception = DOMException, TypeErr = TypeError, MapType = Map, ProxyType = Proxy;
    const ReflectGet = Reflect.get, ReflectSet = Reflect.set, OwnKeys = Reflect.ownKeys, Descriptor = Reflect.getOwnPropertyDescriptor;
    const DefineProperty = Reflect.defineProperty, DeleteProperty = Reflect.deleteProperty;
    const push = Array.prototype.push, concat = Array.prototype.concat;
    const slice = Array.prototype.slice, splice = Array.prototype.splice;
    const constructorDocument = document, PromiseType = Promise;
    const mapGet = Map.prototype.get, mapSet = Map.prototype.set;
    const indexPattern = /^(0|[1-9][0-9]*)$/, regexTest = RegExp.prototype.test;
    const owners = new WeakMap(), sheets = new WeakMap(), ruleOwners = new WeakMap(), listOwners = new WeakMap(),
        sheetListOwners = new WeakMap(), documentLists = new WeakMap(), constructedSheets = new WeakMap(),
        adoptedLists = new WeakMap(), token = {};
    const get = WeakMap.prototype.get, set = WeakMap.prototype.set;
    function owner(sheet) {
        const node = apply(get, owners, [sheet]);
        if (!node) throw new TypeErr('StyleSheet receiver required');
        return node;
    }
    function string(value) { if (typeof value === 'symbol') throw new TypeErr('Cannot convert Symbol to DOMString'); return StringType(value); }
    function index(value) { return (+value) >>> 0; }
    function checked(result) {
        if (typeof result === 'number' && result < 0) {
            const names = ['', '', 'IndexSizeError', 'SyntaxError', 'NotSupportedError', 'InvalidStateError', 'HierarchyRequestError', 'SecurityError', 'NotAllowedError'];
            throw new Exception('Native style sheet operation failed', names[-result] || 'InvalidStateError');
        }
        return result;
    }
    function operation(sheet, op, idx = 0, text) {
        const state = owner(sheet);
        return checked(rawDom.cssom(state.node, op, state.id, idx, text));
    }
    function ruleInfo(rule) {
        const state = apply(get, ruleOwners, [rule]);
        if (!state) throw new TypeErr('CSSRule receiver required');
        const info = operation(state.sheet, 'ruleId', state.id);
        return { state, info };
    }
    class CSSRule {
        constructor(key, sheet, id) {
            if (key !== token) throw new TypeErr('Illegal CSSRule constructor');
            apply(set, ruleOwners, [this, { sheet, id }]);
        }
        get type() { const r = ruleInfo(this); return r.info ? r.info.type : 0; }
        get cssText() { const r = ruleInfo(this); return r.info ? r.info.text : ''; }
        // CSSOM explicitly specifies no rule mutation for this base setter.
        set cssText(value) { ruleInfo(this); string(value); }
        get parentStyleSheet() { const r = ruleInfo(this); return r.info && r.info.attached ? r.state.sheet : null; }
        get parentRule() { ruleInfo(this); return null; }
    }
    class CSSStyleRule extends CSSRule {
        get selectorText() { const r = ruleInfo(this); return r.info ? r.info.selector : ''; }
    }
    function ruleAt(sheet, idx) {
        const info = operation(sheet, 'rule', idx); if (!info) return null;
        const state = owner(sheet);
        let rule = apply(mapGet, state.rules, [info.id]);
        if (!rule) { rule = info.type === 1 ? new CSSStyleRule(token, sheet, info.id) : new CSSRule(token, sheet, info.id); apply(mapSet, state.rules, [info.id, rule]); }
        return rule;
    }
    function listSheet(list) {
        const sheet = apply(get, listOwners, [list]);
        if (!sheet) throw new TypeErr('CSSRuleList receiver required'); return sheet;
    }
    function rulesFor(sheet) {
        const state=owner(sheet);operation(sheet,'length');
        if(!state.list)state.list=new CSSRuleList(token,sheet);return state.list;
    }
    function numeric(key) {
        if (typeof key !== 'string' || !apply(regexTest,indexPattern,[key])) return null;
        const n = +key; return n < 0xffffffff && StringType(n) === key ? n : null;
    }
    class CSSRuleList {
        constructor(key, sheet) {
            if (key !== token) throw new TypeErr('Illegal CSSRuleList constructor');
            const target = this;
            const proxy = new ProxyType(target, {
                get(t, key, receiver) { const i = numeric(key); return i === null ? ReflectGet(t, key, receiver) : ruleAt(sheet, i) ?? undefined; },
                has(t, key) { const i = numeric(key); return i === null ? key in t : i < operation(sheet, 'length'); },
                set(t, key, value, receiver) { if (numeric(key) !== null) return false; return ReflectSet(t, key, value, receiver); },
                defineProperty(t,key,descriptor) { return numeric(key) === null && DefineProperty(t,key,descriptor); },
                deleteProperty(t,key) { return numeric(key) === null && DeleteProperty(t,key); },
                preventExtensions() { return false; },
                ownKeys(t) { const keys = []; const count = operation(sheet, 'length'); for (let i=0;i<count;i++) apply(push,keys,[StringType(i)]); return apply(concat,keys,[OwnKeys(t)]); },
                getOwnPropertyDescriptor(t,key) { const i=numeric(key); if(i===null)return Descriptor(t,key); const rule=ruleAt(sheet,i); return rule ? {value:rule,writable:false,enumerable:true,configurable:true} : undefined; }
            });
            apply(set,listOwners,[target,sheet]);apply(set,listOwners,[proxy,sheet]);return proxy;
        }
        get length() { return operation(listSheet(this), 'length'); }
        item(value) { const sheet=listSheet(this); if(!arguments.length)throw new TypeErr('CSSRuleList.item requires index'); return ruleAt(sheet,index(value)); }
        *[Symbol.iterator]() { const sheet=listSheet(this); for(let i=0;i<operation(sheet,'length');i++)yield ruleAt(sheet,i); }
    }
    class StyleSheet {
        constructor(key, node, id) {
            if (key !== token) throw new TypeErr('Illegal StyleSheet constructor');
            apply(set, owners, [this, { node, id, rules: new MapType(), list: null }]);
        }
        get type() { owner(this); return 'text/css'; }
        get href() { return operation(this,'href'); }
        get ownerNode() {
            const state = owner(this);
            return operation(this,'current') ? state.node : null;
        }
        get parentStyleSheet() { owner(this); return null; }
        get title() { return owner(this).constructed ? null : reflectedAttr(owner(this).node, 'title') || ''; }
        get disabled() { return operation(this,'disabled'); }
        set disabled(value) { operation(this,'disable',value ? 1 : 0); }
    }
    class CSSStyleSheet extends StyleSheet {
        constructor(key, node, id) {
            const constructed = key !== token;
            const options = constructed ? key ?? {} : null;
            if (constructed) { node = constructorDocument; id = checked(rawDom.cssom(node, 'construct')); }
            super(token, node, id);
            if (constructed) {
                const state = owner(this); state.constructed = true;
                rememberConstructed(node, id, this);
                if (options.media !== undefined) operation(this, 'media', 0, string(options.media));
                if (options.disabled) this.disabled = true;
            }
        }
        get ownerRule() { owner(this); return null; }
        get cssRules() { return rulesFor(this); }
        get rules() { return rulesFor(this); }
        insertRule(rule, at = 0) {
            owner(this);if(!arguments.length)throw new TypeErr('insertRule requires rule');
            const text=string(rule), i=index(at);return operation(this,'insert',i,text);
        }
        deleteRule(at) { owner(this);if(!arguments.length)throw new TypeErr('deleteRule requires index');operation(this,'delete',index(at)); }
        replaceSync(text) {
            owner(this); if (!arguments.length) throw new TypeErr('replaceSync requires text');
            operation(this, 'replace', 0, string(text));
        }
        replace(text) {
            owner(this); if (!arguments.length) return PromiseType.reject(new TypeErr('replace requires text'));
            try { this.replaceSync(text); return PromiseType.resolve(this); }
            catch (error) { return PromiseType.reject(error); }
        }
    }
    function rememberConstructed(node, id, sheet) {
        let cache = apply(get, constructedSheets, [node]);
        if (!cache) { cache = new MapType(); apply(set, constructedSheets, [node, cache]); }
        apply(mapSet, cache, [id, sheet]);
    }
    function constructedFor(node, id) {
        const cache = apply(get, constructedSheets, [node]);
        let sheet = cache && apply(mapGet, cache, [id]);
        if (!sheet) { sheet = new CSSStyleSheet(token, node, id); owner(sheet).constructed = true; rememberConstructed(node, id, sheet); }
        return sheet;
    }
    function sheetFor(node) {
        const id=checked(rawDom.cssom(node,'sheet'));if(!id)return null;
        let sheet = apply(get, sheets, [node]);
        if (!sheet || owner(sheet).id!==id) { sheet = new CSSStyleSheet(token, node, id); apply(set, sheets, [node, sheet]); }
        return sheet;
    }
    function listRoot(list) {
        const root=apply(get,sheetListOwners,[list]);
        if(!root)throw new TypeErr('StyleSheetList receiver required');return root;
    }
    function sheetAt(root,i) {
        const node=checked(rawDom.cssom(root,'listItem',0,i));return node ? sheetFor(node) : null;
    }
    function sheetCount(root) { return checked(rawDom.cssom(root,'listLength')); }
    class StyleSheetList {
        constructor(key,root) {
            if(key!==token)throw new TypeErr('Illegal StyleSheetList constructor');
            const target=this,proxy=new ProxyType(target,{
                get(t,key,receiver){const i=numeric(key);return i===null?ReflectGet(t,key,receiver):sheetAt(root,i)??undefined;},
                has(t,key){const i=numeric(key);return i===null?key in t:i<sheetCount(root);},
                set(t,key,value,receiver){if(numeric(key)!==null)return false;return ReflectSet(t,key,value,receiver);},
                defineProperty(t,key,descriptor){return numeric(key)===null&&DefineProperty(t,key,descriptor);},
                deleteProperty(t,key){return numeric(key)===null&&DeleteProperty(t,key);},
                preventExtensions(){return false;},
                ownKeys(t){const keys=[],count=sheetCount(root);for(let i=0;i<count;i++)apply(push,keys,[StringType(i)]);return apply(concat,keys,[OwnKeys(t)]);},
                getOwnPropertyDescriptor(t,key){const i=numeric(key);if(i===null)return Descriptor(t,key);const sheet=sheetAt(root,i);return sheet?{value:sheet,writable:false,enumerable:true,configurable:true}:undefined;}
            });
            apply(set,sheetListOwners,[target,root]);apply(set,sheetListOwners,[proxy,root]);return proxy;
        }
        get length(){return sheetCount(listRoot(this));}
        item(value){const root=listRoot(this);if(!arguments.length)throw new TypeErr('StyleSheetList.item requires index');return sheetAt(root,index(value));}
        *[Symbol.iterator](){const root=listRoot(this);for(let i=0;i<sheetCount(root);i++)yield sheetAt(root,i);}
    }
    for(const [C,tag] of [[HTMLStyleElement,'style'],[HTMLLinkElement,'link']]) {
        define(C.prototype,'sheet',{configurable:true,enumerable:true,get(){htmlElementBrand(this,tag);return sheetFor(this);}});
    }
    function documentSheets() {
        checked(rawDom.cssom(this,'listLength')); // native Document/ShadowRoot brand
        let list=apply(get,documentLists,[this]);
        if(!list){list=new StyleSheetList(token,this);apply(set,documentLists,[this,list]);}return list;
    }
    define(Document.prototype,'styleSheets',{configurable:true,enumerable:true,get:documentSheets});
    define(ShadowRoot.prototype,'styleSheets',{configurable:true,enumerable:true,get:documentSheets});
    function adopt(root, values) {
        const pairs = [];
        for (const sheet of values) {
            const state = owner(sheet);
            if (!state.constructed) throw new Exception('Only constructed style sheets can be adopted', 'NotAllowedError');
            apply(push, pairs, [[state.node, state.id]]);
        }
        checked(rawDom.cssom(root, 'adopt', pairs));
    }
    function adoptedSheets() {
        checked(rawDom.cssom(this, 'adopted')); // native brand and current document
        let list = apply(get, adoptedLists, [this]);
        if (list) return list;
        const root = this, target = [];
        function sync() {
            const pairs = checked(rawDom.cssom(root, 'adopted'));
            target.length = 0;
            for (const pair of pairs) apply(push, target, [constructedFor(pair[0], pair[1])]);
        }
        function commit(values) { adopt(root, values); sync(); }
        list = new ProxyType(target, {
            get(t, key, receiver) { sync(); return ReflectGet(t, key, receiver); },
            set(t, key, value) {
                sync(); const i = numeric(key), values = apply(slice, t, []);
                if (key === 'length') {
                    const length = +value;
                    if (length !== (length >>> 0) || length > values.length) throw new RangeError('Invalid adoptedStyleSheets length');
                    values.length = length;
                } else if (i !== null) {
                    if (i > values.length) throw new RangeError('Sparse adoptedStyleSheets is not supported');
                    values[i] = value;
                } else return ReflectSet(t, key, value);
                commit(values); return true;
            },
            deleteProperty(t, key) {
                sync(); const i = numeric(key); if (i === null) return DeleteProperty(t, key);
                const values = apply(slice, t, []); apply(splice, values, [i, 1]); commit(values); return true;
            },
            defineProperty(t, key, descriptor) {
                if (key === 'length' || numeric(key) !== null) return false;
                return DefineProperty(t, key, descriptor);
            },
            has(t, key) { sync(); return key in t; },
            ownKeys(t) { sync(); return OwnKeys(t); },
            getOwnPropertyDescriptor(t, key) { sync(); return Descriptor(t, key); },
            preventExtensions() { return false; }
        });
        apply(set, adoptedLists, [root, list]); return list;
    }
    for (const C of [Document, ShadowRoot]) define(C.prototype, 'adoptedStyleSheets', {
        configurable: true, enumerable: true, get: adoptedSheets,
        set(values) { checked(rawDom.cssom(this, 'adopted')); adopt(this, values); }
    });
    define(HTMLStyleElement.prototype, 'disabled', { configurable: true, enumerable: true,
        get() { htmlElementBrand(this, 'style'); return rawDom.styleDisabled(this); },
        set(value) { htmlElementBrand(this, 'style'); rawDom.styleDisabled(this, !!value); }
    });
    for (const C of [StyleSheet, CSSStyleSheet, CSSRule, CSSStyleRule, CSSRuleList, StyleSheetList]) {
        define(C.prototype, Symbol.toStringTag, { value: C.name, configurable: true });
        for (const name of Object.getOwnPropertyNames(C.prototype)) {
            if (name !== 'constructor') define(C.prototype, name, { ...Object.getOwnPropertyDescriptor(C.prototype, name), enumerable: true });
        }
    }
    for(const [name,value] of Object.entries({STYLE_RULE:1,IMPORT_RULE:3,MEDIA_RULE:4,FONT_FACE_RULE:5,PAGE_RULE:6,KEYFRAMES_RULE:7,NAMESPACE_RULE:10,SUPPORTS_RULE:12})) {
        define(CSSRule,name,{value,enumerable:true});define(CSSRule.prototype,name,{value,enumerable:true});
    }
    Object.assign(globalThis, { StyleSheet, CSSStyleSheet, CSSRule, CSSStyleRule, CSSRuleList, StyleSheetList });
})();
