/* CSSOM: style sheets and their rules. The rule tree is native (cssom.c owns
 * it, css.c's parse builds it with the cascade AST); these objects are thin
 * handles: a sheet is (owner node, sheet id), a rule (sheet, rule id), and
 * every value they show is read from js_cssom.h. Constructed sheets share the
 * AST across native document/shadow scopes. */
(() => {
    'use strict';
    const define = Object.defineProperty, apply = Reflect.apply;
    const StringType = String, Exception = DOMException, TypeErr = TypeError, MapType = Map, ProxyType = Proxy;
    const ReflectGet = Reflect.get, ReflectSet = Reflect.set, OwnKeys = Reflect.ownKeys, Descriptor = Reflect.getOwnPropertyDescriptor;
    const DefineProperty = Reflect.defineProperty, DeleteProperty = Reflect.deleteProperty;
    const push = Array.prototype.push, concat = Array.prototype.concat, join = Array.prototype.join, freeze = Object.freeze;
    const slice = Array.prototype.slice, splice = Array.prototype.splice;
    const constructorDocument = document, PromiseType = Promise;
    const mapGet = Map.prototype.get, mapSet = Map.prototype.set;
    const indexPattern = /^(0|[1-9][0-9]*)$/, regexTest = RegExp.prototype.test;
    const owners = new WeakMap(), sheets = new WeakMap(), ruleOwners = new WeakMap(), listOwners = new WeakMap(),
        sheetListOwners = new WeakMap(), documentLists = new WeakMap(), constructedSheets = new WeakMap(),
        adoptedLists = new WeakMap(), mediaOwners = new WeakMap(), mapOwners = new WeakMap(), token = {};
    const get = WeakMap.prototype.get, set = WeakMap.prototype.set;
    function owner(sheet) {
        const node = apply(get, owners, [sheet]);
        if (!node) throw new TypeErr('StyleSheet receiver required');
        return node;
    }
    function string(value) { if (typeof value === 'symbol') throw new TypeErr('Cannot convert Symbol to DOMString'); return StringType(value); }
    function index(value) { return (+value) >>> 0; }
    const errorNames = ['', '', 'IndexSizeError', 'SyntaxError', 'NotSupportedError', 'InvalidStateError', 'HierarchyRequestError',
        'SecurityError', 'NotAllowedError', 'NotFoundError', 'InvalidStateError'];
    function checked(result) {
        if (typeof result === 'number' && result < 0)
            throw new Exception('Native style sheet operation failed', errorNames[-result] || 'InvalidStateError');
        return result;
    }
    function operation(sheet, op, ...args) {
        const state = owner(sheet);
        return checked(rawDom.cssom(state.node, op, state.id, ...args));
    }
    function ruleState(rule) {
        const state = apply(get, ruleOwners, [rule]);
        if (!state) throw new TypeErr('CSSRule receiver required');
        return state;
    }
    function ruleOp(rule, op, ...args) { const state = ruleState(rule); return operation(state.sheet, op, state.id, ...args); }
    function attr(rule, name) { return ruleOp(rule, 'attr', name); }
    function info(rule) { return ruleOp(rule, 'rule'); }
    /* CSSRule.parentStyleSheet: null once the rule is removed */
    function ruleSheet(rule) { const r = info(rule); return r && r[1] ? ruleState(rule).sheet : null; }
    /* the CSSRule object a native rule id stands for, one per id */
    function ruleFor(sheet, id, type) {
        const state = owner(sheet);
        let rule = apply(mapGet, state.rules, [id]);
        if (!rule) {
            const Kind = ruleKinds[type] || CSSRule;
            rule = new Kind(token, sheet, id);
            apply(mapSet, state.rules, [id, rule]);
        }
        return rule;
    }
    function childAt(sheet, container, i) {
        const pair = operation(sheet, 'child', container, i);
        return pair ? ruleFor(sheet, pair[0], pair[1]) : null;
    }
    function childCount(sheet, container) { return operation(sheet, 'length', container); }
    function numeric(key) {
        if (typeof key !== 'string' || !apply(regexTest,indexPattern,[key])) return null;
        const n = +key; return n < 0xffffffff && StringType(n) === key ? n : null;
    }
    /* an object with indexed properties read through at(i) and count() */
    function indexed(target, at, count) {
        return new ProxyType(target, {
            get(t, key, receiver) { const i = numeric(key); return i === null ? ReflectGet(t, key, receiver) : at(i) ?? undefined; },
            has(t, key) { const i = numeric(key); return i === null ? key in t : i < count(); },
            set(t, key, value, receiver) { if (numeric(key) !== null) return false; return ReflectSet(t, key, value, receiver); },
            defineProperty(t,key,descriptor) { return numeric(key) === null && DefineProperty(t,key,descriptor); },
            deleteProperty(t,key) { return numeric(key) === null && DeleteProperty(t,key); },
            preventExtensions() { return false; },
            ownKeys(t) { const keys = []; const n = count(); for (let i=0;i<n;i++) apply(push,keys,[StringType(i)]); return apply(concat,keys,[OwnKeys(t)]); },
            getOwnPropertyDescriptor(t,key) { const i=numeric(key); if(i===null)return Descriptor(t,key); const item=at(i); return item != null ? {value:item,writable:false,enumerable:true,configurable:true} : undefined; }
        });
    }
    function listState(list) {
        const state = apply(get, listOwners, [list]);
        if (!state) throw new TypeErr('CSSRuleList receiver required'); return state;
    }
    class CSSRuleList {
        constructor(key, sheet, container) {
            if (key !== token) throw new TypeErr('Illegal CSSRuleList constructor');
            const target = this, state = { sheet, container };
            const proxy = indexed(target, i => childAt(sheet, container, i), () => childCount(sheet, container));
            apply(set,listOwners,[target,state]);apply(set,listOwners,[proxy,state]);return proxy;
        }
        get length() { const s = listState(this); return childCount(s.sheet, s.container); }
        item(value) { const s=listState(this); if(!arguments.length)throw new TypeErr('CSSRuleList.item requires index'); return childAt(s.sheet,s.container,index(value)); }
        *[Symbol.iterator]() { const s=listState(this); for(let i=0;i<childCount(s.sheet,s.container);i++)yield childAt(s.sheet,s.container,i); }
    }
    /* the [SameObject] list of a sheet's (container 0) or a rule's child rules */
    function rulesOf(sheet, container, holder) {
        childCount(sheet, container); // SecurityError before any list
        if (!holder.list) holder.list = new CSSRuleList(token, sheet, container);
        return holder.list;
    }
    /* MediaList (CSSOM 4.3) of a sheet (id 0), an @media rule or an @import rule */
    function mediaState(list) {
        const state = apply(get, mediaOwners, [list]);
        if (!state) throw new TypeErr('MediaList receiver required'); return state;
    }
    function mediaText(state) { return operation(state.sheet, 'media', state.id, 0); }
    function mediaItems(text) {
        const items = [];
        let depth = 0, start = 0, quote = '';
        for (let i = 0; i < text.length; i++) {
            const c = text[i];
            if (quote) { if (c === '\\') i++; else if (c === quote) quote = ''; continue; }
            if (c === '"' || c === "'") quote = c;
            else if (c === '(') depth++;
            else if (c === ')' && depth) depth--;
            else if (c === ',' && !depth) { apply(push, items, [text.slice(start, i).trim()]); start = i + 1; }
        }
        if (text.trim()) apply(push, items, [text.slice(start).trim()]);
        return items;
    }
    class MediaList {
        constructor(key, sheet, id) {
            if (key !== token) throw new TypeErr('Illegal MediaList constructor');
            const state = { sheet, id }, target = this;
            const proxy = indexed(target, i => mediaItems(mediaText(state))[i] ?? null, () => mediaItems(mediaText(state)).length);
            apply(set, mediaOwners, [target, state]); apply(set, mediaOwners, [proxy, state]); return proxy;
        }
        get mediaText() { return mediaText(mediaState(this)); }
        set mediaText(value) { const s = mediaState(this); operation(s.sheet, 'media', s.id, 1, value === null ? '' : string(value)); }
        get length() { return mediaItems(mediaText(mediaState(this))).length; }
        item(value) {
            const s = mediaState(this); if (!arguments.length) throw new TypeErr('MediaList.item requires index');
            return mediaItems(mediaText(s))[index(value)] ?? null;
        }
        appendMedium(medium) {
            const s = mediaState(this); if (!arguments.length) throw new TypeErr('appendMedium requires medium');
            operation(s.sheet, 'media', s.id, 2, string(medium));
        }
        deleteMedium(medium) {
            const s = mediaState(this); if (!arguments.length) throw new TypeErr('deleteMedium requires medium');
            operation(s.sheet, 'media', s.id, 3, string(medium));
        }
        toString() { return this.mediaText; }
        *[Symbol.iterator]() { const items = mediaItems(mediaText(mediaState(this))); for (const item of items) yield item; }
    }
    function mediaOf(sheet, id, holder) {
        if (!holder.media) holder.media = new MediaList(token, sheet, id);
        return holder.media;
    }
    /* a rule's [SameObject] declaration block, a CSSStyleDeclaration (js_bootstrap.js) */
    function styleOf(rule, kind) {
        const state = ruleState(rule);
        if (!state.style) state.style = cssRuleStyle(kind, {
            rule,
            block() { return ruleOp(rule, 'decls'); },
            read(name, priority) { return ruleOp(rule, 'declGet', name, priority ? 1 : 0); },
            write(name, value, priority) { ruleOp(rule, 'declSet', name, value, priority); },
            setText(text) { ruleOp(rule, 'declText', text); }
        });
        return state.style;
    }
    class CSSRule {
        constructor(key, sheet, id) {
            if (key !== token) throw new TypeErr('Illegal CSSRule constructor');
            apply(set, ruleOwners, [this, { sheet, id, list: null, media: null, style: null }]);
        }
        get type() { const r = info(this); return r && r[0] < 16 ? r[0] : 0; }
        get cssText() { return ruleOp(this, 'cssText'); }
        // CSSOM explicitly specifies no rule mutation for this base setter.
        set cssText(value) { ruleState(this); string(value); }
        get parentStyleSheet() { return ruleSheet(this); }
        get parentRule() { const r = info(this); return r && r[2] ? ruleFor(ruleState(this).sheet, r[2], r[3]) : null; }
    }
    class CSSGroupingRule extends CSSRule {
        get cssRules() { const s = ruleState(this); return rulesOf(s.sheet, s.id, s); }
        insertRule(rule, at = 0) {
            ruleState(this); if (!arguments.length) throw new TypeErr('insertRule requires rule');
            const text = string(rule), i = index(at); return ruleOp(this, 'insert', text, i);
        }
        deleteRule(at) { ruleState(this); if (!arguments.length) throw new TypeErr('deleteRule requires index'); ruleOp(this, 'delete', index(at)); }
    }
    class CSSConditionRule extends CSSGroupingRule {
        get conditionText() { return attr(this, 'conditionText') ?? ''; }
    }
    class CSSMediaRule extends CSSConditionRule {
        get media() { const s = ruleState(this); return mediaOf(s.sheet, s.id, s); }
        set media(value) { this.media.mediaText = value; }
        get matches() { return attr(this, 'matches') === 'true'; }
    }
    class CSSSupportsRule extends CSSConditionRule {
        get matches() { return attr(this, 'matches') === 'true'; }
    }
    class CSSContainerRule extends CSSConditionRule {
        get containerName() { return attr(this, 'containerName') ?? ''; }
        get containerQuery() { return attr(this, 'containerQuery') ?? ''; }
    }
    class CSSStyleRule extends CSSGroupingRule {
        get selectorText() { return attr(this, 'selectorText') ?? ''; }
        // CSSOM: parse the value as a group of selectors; on failure nothing changes.
        set selectorText(value) { ruleOp(this, 'setAttr', 'selectorText', string(value)); }
        get style() { return styleOf(this, 'style'); }
        set style(value) { this.style.cssText = value; }
    }
    class CSSNestedDeclarations extends CSSRule {
        get style() { return styleOf(this, 'style'); }
        set style(value) { this.style.cssText = value; }
    }
    class CSSImportRule extends CSSRule {
        get href() { return attr(this, 'href') ?? ''; }
        get media() { const s = ruleState(this); return mediaOf(s.sheet, s.id, s); }
        set media(value) { this.media.mediaText = value; }
        // [SameObject]: the sheet of the rule's resource once it has loaded, with this rule its ownerRule
        get styleSheet() {
            const s = ruleState(this);
            if (s.styleSheet) return s.styleSheet;
            const id = ruleOp(this, 'importSheet');
            if (!id) return null;
            const sheet = new CSSStyleSheet(token, owner(s.sheet).node, id);
            owner(sheet).ownerRule = this;
            return s.styleSheet = sheet;
        }
        get layerName() { return attr(this, 'layerName'); }
        get supportsText() { return attr(this, 'supportsText'); }
    }
    class CSSNamespaceRule extends CSSRule {
        get namespaceURI() { return attr(this, 'namespaceURI') ?? ''; }
        get prefix() { return attr(this, 'prefix') ?? ''; }
    }
    class CSSFontFaceRule extends CSSRule {
        get style() { return styleOf(this, 'font-face'); }
        set style(value) { this.style.cssText = value; }
    }
    class CSSPageRule extends CSSGroupingRule {
        get selectorText() { return attr(this, 'selectorText') ?? ''; }
        set selectorText(value) { ruleOp(this, 'setAttr', 'selectorText', string(value)); }
        get style() { return styleOf(this, 'page'); }
        set style(value) { this.style.cssText = value; }
    }
    class CSSMarginRule extends CSSRule {
        get name() { return attr(this, 'name') ?? ''; }
        get style() { return styleOf(this, 'style'); }
        set style(value) { this.style.cssText = value; }
    }
    class CSSKeyframeRule extends CSSRule {
        get keyText() { return attr(this, 'keyText') ?? ''; }
        set keyText(value) { ruleOp(this, 'setAttr', 'keyText', string(value)); }
        get style() { return styleOf(this, 'style'); }
        set style(value) { this.style.cssText = value; }
    }
    class CSSKeyframesRule extends CSSRule {
        constructor(key, sheet, id) {
            super(key, sheet, id);
            const target = this, proxy = indexed(target, i => childAt(sheet, id, i), () => childCount(sheet, id));
            apply(set, ruleOwners, [proxy, apply(get, ruleOwners, [target])]);
            return proxy;
        }
        get name() { return attr(this, 'name') ?? ''; }
        set name(value) { ruleOp(this, 'setAttr', 'name', string(value)); }
        get cssRules() { const s = ruleState(this); return rulesOf(s.sheet, s.id, s); }
        get length() { const s = ruleState(this); return childCount(s.sheet, s.id); }
        // CSS Animations: a rule that does not parse as one keyframe is ignored.
        appendRule(rule) {
            const s = ruleState(this); if (!arguments.length) throw new TypeErr('appendRule requires rule');
            const text = string(rule), result = rawDom.cssom(owner(s.sheet).node, 'insert', owner(s.sheet).id, s.id, text, childCount(s.sheet, s.id));
            if (typeof result === 'number' && result < 0 && errorNames[-result] === 'SecurityError') checked(result);
        }
        deleteRule(select) {
            ruleState(this); if (!arguments.length) throw new TypeErr('deleteRule requires select');
            const i = ruleOp(this, 'keyframe', string(select)); if (i !== null) ruleOp(this, 'delete', i);
        }
        findRule(select) {
            const s = ruleState(this); if (!arguments.length) throw new TypeErr('findRule requires select');
            const i = ruleOp(this, 'keyframe', string(select)); return i !== null ? childAt(s.sheet, s.id, i) : null;
        }
    }
    class CSSCounterStyleRule extends CSSRule {
        get name() { return attr(this, 'name') ?? ''; }
        set name(value) { ruleOp(this, 'setAttr', 'name', string(value)); }
    }
    for (const name of ['system', 'symbols', 'additiveSymbols', 'negative', 'prefix', 'suffix', 'range', 'pad', 'speakAs', 'fallback'])
        define(CSSCounterStyleRule.prototype, name, { configurable: true, enumerable: true,
            get() { return attr(this, name) ?? ''; }, set(value) { ruleOp(this, 'setAttr', name, string(value)); } });
    class CSSLayerBlockRule extends CSSGroupingRule {
        get name() { return attr(this, 'name') ?? ''; }
    }
    class CSSLayerStatementRule extends CSSRule {
        // FrozenArray: the same array for as long as the names are the same
        get nameList() {
            const s = ruleState(this), names = attr(this, 'nameList') ?? [], key = apply(join, names, ['\u0000']);
            if (!s.nameList || s.nameKey !== key) { s.nameList = freeze(names); s.nameKey = key; }
            return s.nameList;
        }
    }
    class CSSScopeRule extends CSSGroupingRule {
        get start() { return attr(this, 'start'); }
        get end() { return attr(this, 'end'); }
    }
    class CSSStartingStyleRule extends CSSGroupingRule {}
    class CSSPropertyRule extends CSSRule {
        get name() { return attr(this, 'name') ?? ''; }
        get syntax() { return attr(this, 'syntax') ?? ''; }
        get inherits() { return attr(this, 'inherits') === 'true'; }
        get initialValue() { return attr(this, 'initialValue'); }
    }
    class CSSFontPaletteValuesRule extends CSSRule {
        get name() { return attr(this, 'name') ?? ''; }
        get fontFamily() { return attr(this, 'fontFamily') ?? ''; }
        get basePalette() { return attr(this, 'basePalette') ?? ''; }
        get overrideColors() { return attr(this, 'overrideColors') ?? ''; }
    }
    /* CSS Fonts 4: a @font-feature-values block as a map of name to values */
    function mapState(map) {
        const state = apply(get, mapOwners, [map]);
        if (!state) throw new TypeErr('CSSFontFeatureValuesMap receiver required'); return state;
    }
    function mapEntries(map) {
        const state = mapState(map), flat = ruleOp(state.rule, 'features', state.block), entries = [];
        for (let i = 0; i + 1 < flat.length; i += 2) apply(push, entries, [[flat[i], flat[i + 1]]]);
        return entries;
    }
    const featureCounts = { annotation: [1, 1], ornaments: [1, 1], stylistic: [1, 1], swash: [1, 1],
        'character-variant': [1, 2], styleset: [1, Infinity], 'historical-forms': [1, 1] };
    class CSSFontFeatureValuesMap {
        constructor(key, rule, block) {
            if (key !== token) throw new TypeErr('Illegal CSSFontFeatureValuesMap constructor');
            apply(set, mapOwners, [this, { rule, block }]);
        }
        get size() { return mapEntries(this).length; }
        get(name) { name = string(name); for (const [key, values] of mapEntries(this)) if (key === name) return values; return undefined; }
        has(name) { name = string(name); for (const [key] of mapEntries(this)) if (key === name) return true; return false; }
        set(name, values) {
            const state = mapState(this);
            name = string(name);
            const list = typeof values === 'object' && values !== null ? Array.from(values, v => (+v) >>> 0) : [(+values) >>> 0];
            const [low, high] = featureCounts[state.block];
            if (list.length < low || list.length > high) throw new Exception('Wrong number of feature values', 'InvalidAccessError');
            ruleOp(state.rule, 'setFeature', state.block, name, list);
            return this;
        }
        delete(name) {
            const state = mapState(this), had = this.has(name);
            ruleOp(state.rule, 'setFeature', state.block, string(name), null);
            return had;
        }
        clear() { const state = mapState(this); ruleOp(state.rule, 'setFeature', state.block, null, null); }
        forEach(callback, thisArg = undefined) {
            if (typeof callback !== 'function') throw new TypeErr('forEach requires a function');
            for (const [key, values] of mapEntries(this)) apply(callback, thisArg, [values, key, this]);
        }
        *entries() { for (const entry of mapEntries(this)) yield entry; }
        *keys() { for (const [key] of mapEntries(this)) yield key; }
        *values() { for (const [, values] of mapEntries(this)) yield values; }
        [Symbol.iterator]() { return this.entries(); }
    }
    class CSSFontFeatureValuesRule extends CSSRule {
        get fontFamily() { return attr(this, 'fontFamily') ?? ''; }
        set fontFamily(value) { ruleOp(this, 'setAttr', 'fontFamily', string(value)); }
    }
    for (const [name, block] of [['annotation', 'annotation'], ['ornaments', 'ornaments'], ['stylistic', 'stylistic'], ['swash', 'swash'],
        ['characterVariant', 'character-variant'], ['styleset', 'styleset'], ['historicalForms', 'historical-forms']])
        define(CSSFontFeatureValuesRule.prototype, name, { configurable: true, enumerable: true, get() {
            const state = ruleState(this);
            if (!state.maps) state.maps = {};
            return state.maps[block] || (state.maps[block] = new CSSFontFeatureValuesMap(token, this, block));
        } });
    const ruleKinds = { 1: CSSStyleRule, 3: CSSImportRule, 4: CSSMediaRule, 5: CSSFontFaceRule, 6: CSSPageRule, 7: CSSKeyframesRule,
        8: CSSKeyframeRule, 9: CSSMarginRule, 10: CSSNamespaceRule, 11: CSSCounterStyleRule, 12: CSSSupportsRule,
        14: CSSFontFeatureValuesRule, 16: CSSLayerBlockRule, 17: CSSLayerStatementRule, 18: CSSContainerRule, 19: CSSScopeRule,
        20: CSSStartingStyleRule, 21: CSSPropertyRule, 22: CSSNestedDeclarations, 23: CSSFontPaletteValuesRule };
    class StyleSheet {
        constructor(key, node, id) {
            if (key !== token) throw new TypeErr('Illegal StyleSheet constructor');
            apply(set, owners, [this, { node, id, rules: new MapType(), list: null, media: null }]);
        }
        get type() { owner(this); return 'text/css'; }
        get href() { return operation(this,'href'); }
        get ownerNode() {
            const state = owner(this);
            return !state.ownerRule && operation(this,'current') ? state.node : null;
        }
        // an @import rule's sheet: its rule's sheet, while the rule stands in it
        get parentStyleSheet() { const state = owner(this); return state.ownerRule ? ruleSheet(state.ownerRule) : null; }
        get title() {
            const state = owner(this); if (state.constructed || state.ownerRule) return null;
            const title = reflectedAttr(state.node, 'title'); return title ? title : null;
        }
        // an @import rule's sheet has the rule's MediaList (CSSImportRule.media is styleSheet.media)
        get media() {
            const state = owner(this);
            if (state.ownerRule) { const s = ruleState(state.ownerRule); return mediaOf(s.sheet, s.id, s); }
            return mediaOf(this, 0, state);
        }
        set media(value) { this.media.mediaText = value; }
        get disabled() { return operation(this,'disabled'); }
        set disabled(value) { operation(this,'disable',value ? 1 : 0); }
    }
    /* the CSSStyleSheetInit dictionary (WebIDL: its members in order, each converted) */
    function sheetInit(options) {
        const init = { baseURL: null, disabled: false, media: '' };
        if (options === undefined || options === null) return init;
        if (typeof options !== 'object' && typeof options !== 'function') throw new TypeErr('CSSStyleSheetInit must be an object');
        const base = options.baseURL;
        if (base !== undefined && base !== null) init.baseURL = string(base);
        const disabled = options.disabled;
        if (disabled !== undefined) init.disabled = !!disabled;
        const media = options.media;
        if (media !== undefined) init.media = apply(get, mediaOwners, [media]) ? media : string(media);
        return init;
    }
    class CSSStyleSheet extends StyleSheet {
        constructor(key, node, id) {
            const constructed = key !== token;
            let init = null;
            if (constructed) {
                // CSSOM "create a constructed CSSStyleSheet"; a baseURL that is not a URL throws NotAllowedError
                init = sheetInit(key);
                node = constructorDocument; id = checked(rawDom.cssom(node, 'construct', 0, init.baseURL));
            }
            super(token, node, id);
            if (constructed) {
                const state = owner(this); state.constructed = true;
                rememberConstructed(node, id, this);
                const media = typeof init.media === 'string' ? init.media : mediaText(mediaState(init.media));
                if (media) operation(this, 'media', 0, 1, media);
                if (init.disabled) operation(this, 'disable', 1);
            }
        }
        get ownerRule() { return owner(this).ownerRule ?? null; }
        get cssRules() { return rulesOf(this, 0, owner(this)); }
        get rules() { return rulesOf(this, 0, owner(this)); }
        insertRule(rule, at = 0) {
            owner(this);if(!arguments.length)throw new TypeErr('insertRule requires rule');
            const text=string(rule), i=index(at);return operation(this,'insert',0,text,i);
        }
        deleteRule(at) { owner(this);if(!arguments.length)throw new TypeErr('deleteRule requires index');operation(this,'delete',0,index(at)); }
        // CSSOM legacy methods
        addRule(selector = 'undefined', style = 'undefined', at = undefined) {
            owner(this);
            const block = string(selector) + ' { ' + (string(style) ? string(style) + ' ' : '') + '}';
            this.insertRule(block, at === undefined ? childCount(this, 0) : index(at));
            return -1;
        }
        removeRule(at = 0) { this.deleteRule(index(at)); }
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
            const target=this,proxy=indexed(target,i=>sheetAt(root,i),()=>sheetCount(root));
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
    const interfaces = { StyleSheet, CSSStyleSheet, CSSRule, CSSGroupingRule, CSSConditionRule, CSSMediaRule, CSSSupportsRule,
        CSSContainerRule, CSSStyleRule, CSSNestedDeclarations, CSSImportRule, CSSNamespaceRule, CSSFontFaceRule, CSSPageRule,
        CSSMarginRule, CSSKeyframesRule, CSSKeyframeRule, CSSCounterStyleRule, CSSLayerBlockRule, CSSLayerStatementRule,
        CSSScopeRule, CSSStartingStyleRule, CSSPropertyRule, CSSFontPaletteValuesRule, CSSFontFeatureValuesRule,
        CSSFontFeatureValuesMap, CSSRuleList, StyleSheetList, MediaList };
    for (const name of Object.keys(interfaces)) {
        const C = interfaces[name];
        define(C.prototype, Symbol.toStringTag, { value: name, configurable: true });
        for (const key of Object.getOwnPropertyNames(C.prototype)) {
            if (key !== 'constructor') define(C.prototype, key, { ...Object.getOwnPropertyDescriptor(C.prototype, key), enumerable: true });
        }
    }
    for(const [name,value] of Object.entries({STYLE_RULE:1,CHARSET_RULE:2,IMPORT_RULE:3,MEDIA_RULE:4,FONT_FACE_RULE:5,PAGE_RULE:6,
        KEYFRAMES_RULE:7,KEYFRAME_RULE:8,MARGIN_RULE:9,NAMESPACE_RULE:10,COUNTER_STYLE_RULE:11,SUPPORTS_RULE:12,FONT_FEATURE_VALUES_RULE:14})) {
        define(CSSRule,name,{value,enumerable:true});define(CSSRule.prototype,name,{value,enumerable:true});
    }
    Object.assign(globalThis, interfaces);
})();
