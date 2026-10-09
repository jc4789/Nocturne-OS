/* Autonomous custom elements on Nocturne's existing native DOM wrappers.
 * Algorithms: https://html.spec.whatwg.org/multipage/custom-elements.html
 * Native shadow trees participate in upgrade/connection/adoption. Form-associated
 * definition metadata connects ElementInternals to actual native ownership,
 * validation, submission and CE form callbacks. Scoped registries
 * and customized built-ins remain explicitly unsupported.
 * Embedded in js_bootstrap.js; rawDom/document/HTMLElement/report are private. */
const customElementsBridge = (() => {
    'use strict';
    const HTML = 'http://www.w3.org/1999/xhtml';
    const definitions = new Map(), constructors = new Map(), pending = new Map();
    const states = new WeakMap(), scopes = [], backup = [], faceElements=new Set();
    const constructed = Symbol('already constructed'), registryKey = {};
    const reserved = new Set(['annotation-xml','color-profile','font-face','font-face-src',
        'font-face-uri','font-face-format','font-face-name','missing-glyph']);
    let defining = false, backupScheduled = false;
    const StringImpl = String;
    const string = value => { if (typeof value === 'symbol') throw new TypeError('Cannot convert Symbol to DOMString'); return StringImpl(value); };
    const lower = name => name.replace(/[A-Z]/g, c => c.toLowerCase());
    const get = (node, key) => rawDom('get', node, key);
    const children = node => get(node, 'childNodes');
    const attr = (node, key) => reflectedAttr(node, key);
    const fail = (message, name='NotSupportedError') => new DOMException(message, name);
    // Current HTML uses DOM's valid element local name, not the retired PCENChar
    // grammar: https://html.spec.whatwg.org/multipage/custom-elements.html#valid-custom-element-name
    // https://dom.spec.whatwg.org/#valid-element-local-name (ASCII-letter branch).
    const validName = name => /^[a-z]/.test(name) && !/[\0\t\n\f\r />A-Z]/u.test(name) && name.includes('-') && !reserved.has(name);
    const isObject = value => (typeof value === 'object' && value !== null) || typeof value === 'function';
    function isConstructor(value) {
        if (typeof value !== 'function') return false;
        // Probe [[Construct]] without reading value.prototype or running value.
        try { Reflect.construct(new Proxy(value, {construct() { return {}; }}), []); return true; }
        catch (_) { return false; }
    }
    function sequence(value) {
        if (!isObject(value) || typeof value[Symbol.iterator] !== 'function') throw new TypeError('Expected iterable sequence');
        return Array.from(value, string);
    }
    const connected = node => get(node, 'isConnected');
    // Traverse the actual native tree once, in tree order. Ordinary HTML/text
    // nodes never need wrappers or per-node JS/native crossings for this query.
    const elements = (root, name=null) => rawDom('customCandidates',root,name);
    function stateFor(element) {
        let s = states.get(element);
        if (!s) { s = {state:'undefined', definition:null, reactions:[]}; states.set(element, s); }
        return s;
    }
    function invoke(queue) {
        for (let i = 0; i < queue.length; i++) {
            const element = queue[i], s = states.get(element);
            if (!s) continue;
            while (s.reactions.length) {
                const reaction = s.reactions.shift();
                try {
                    if (reaction.definition) upgrade(element, reaction.definition);
                    else apply(reaction.callback, element, reaction.args);
                } catch (e) { report(e); }
            }
        }
        queue.length = 0;
    }
    function enqueue(element, reaction) {
        stateFor(element).reactions.push(reaction);
        if (scopes.length) scopes[scopes.length - 1].push(element);
        else {
            backup.push(element);
            if (!backupScheduled) {
                backupScheduled = true;
                queueMicrotask(() => { try { invoke(backup); } finally { backupScheduled = false; } });
            }
        }
    }
    function reactions(fn) {
        const queue = []; scopes.push(queue);
        try { return fn(); }
        finally { scopes.pop(); invoke(queue); }
    }
    function callback(element, name, args=[]) {
        const s = states.get(element), d = s && s.definition;
        if (!d || !d.callbacks[name]) return;
        if (name === 'attributeChangedCallback' && !d.observed.includes(args[0])) return;
        enqueue(element, {callback:d.callbacks[name], args});
    }
    function tryUpgrade(element) {
        if (get(element, 'namespaceURI') !== HTML) return;
        const d = definitions.get(get(element, 'localName'));
        const s = states.get(element);
        if (d && (!s || s.state === 'undefined' || s.state === 'precustomized')) enqueue(element, {definition:d});
    }
    function upgradeTree(root) {
        if (!definitions.size) return;
        for (const element of elements(root)) tryUpgrade(element);
    }
    function upgrade(element, d) {
        const s = stateFor(element);
        if (s.state !== 'undefined') return;
        s.definition = d; s.state = 'failed';
        for (const attribute of rawDom('attrList',element)) callback(element, 'attributeChangedCallback',
            [get(attribute,'localName'),null,get(attribute,'attrValue'),get(attribute,'namespaceURI')]);
        if (connected(element)) callback(element, 'connectedCallback');
        d.stack.push(element);
        try {
            if(d.disableShadow && get(element,'shadowRoot'))throw fail('This custom element disables shadow roots');
            s.state = 'precustomized';
            rawDom('face',element,'metadata',d.formAssociated);
            if (Reflect.construct(d.constructor, []) !== element) throw new TypeError('Custom element constructor returned a different object');
            s.state = 'custom';
            if(d.formAssociated){faceElements.add(element);refreshFormElement(element);}
        } catch (e) {
            rawDom('face',element,'metadata',false);
            faceElements.delete(element);
            s.state = 'failed'; s.definition = null; s.reactions.length = 0;
            throw e;
        } finally { d.stack.pop(); }
    }
    function construct(newTarget) {
        const d = constructors.get(newTarget);
        if (newTarget === HTMLElement || !d) throw new TypeError('Illegal HTMLElement constructor');
        let prototype = newTarget.prototype;
        if (!isObject(prototype)) prototype = HTMLElement.prototype;
        if (d.stack.length) {
            const i = d.stack.length - 1, element = d.stack[i];
            if (element === constructed) throw new TypeError('Custom element is already constructed');
            Object.setPrototypeOf(element, prototype);
            d.stack[i] = constructed;
            return element;
        }
        const element = rawDom('create', null, 1, d.name, '');
        rawDom('face',element,'metadata',d.formAssociated);
        Object.setPrototypeOf(element, prototype);
        states.set(element, {state:'custom', definition:d, reactions:[]});
        if(d.formAssociated)faceElements.add(element);
        return element;
    }
    function create(name, options, preserveCase=false) {
        name = string(name); if (!preserveCase) name = lower(name);
        if (options != null && typeof options === 'object') {
            if (options.customElementRegistry !== undefined || options.is !== undefined)
                throw fail('Scoped registries and customized built-in elements are not implemented');
        }
        const d = definitions.get(name);
        if (!d) return rawDom('create', null, 1, name, '');
        return reactions(() => {
            try {
                const element = Reflect.construct(d.constructor, []), s = states.get(element);
                if (!s || s.definition !== d || get(element,'nodeType') !== 1 || get(element,'namespaceURI') !== HTML)
                    throw new TypeError('Constructor did not create an HTMLElement');
                if (get(element,'localName') !== name || get(element,'parentNode') || children(element).length || get(element,'attributeNames').length)
                    throw fail('Custom element constructor changed its name, parent, children or attributes');
                return element;
            } catch (e) {
                report(e);
                const element = rawDom('create', null, 1, name, '');
                states.set(element, {state:'failed', definition:null, reactions:[]});
                return element;
            }
        });
    }
    function inserted(root) {
        if (!definitions.size || !connected(root)) return;
        for (const element of elements(root)) {
            const s = states.get(element);
            if (s && s.state === 'custom') callback(element, 'connectedCallback');
            else tryUpgrade(element);
        }
    }
    function removed(root, wasConnected) {
        if (!wasConnected || !definitions.size) return;
        for (const element of elements(root)) {
            const s = states.get(element);
            if (s && s.state === 'custom') callback(element, 'disconnectedCallback');
        }
    }
    function attributeChanged(element, name, oldValue, newValue, namespace=null) {
        const s = states.get(element);
        if (s && s.state === 'custom')
            callback(element, 'attributeChangedCallback', [name,oldValue,newValue,namespace]);
    }
    // Snapshot native values only. Page overrides of getters/prototypes cannot
    // make internal tree algorithms operate on a different, invented DOM.
    function before(op, node, key, value, extra) {
        if (!definitions.size || !node) return null;
        if(op==='adopt' && key && get(key,'nodeType')!==2)
            return {op,node:key,was:connected(key),parent:get(key,'parentNode'),elements:elements(key),oldDocument:get(key,'ownerDocument')};
        const change=attributeBridge.mutation(op,node,key,value,extra);
        if(change)return {op:'attribute',node:change.node,forced:change.name,namespace:change.namespace,old:change.oldValue};
        if (op === 'insert') {
            if (!key || key === value) return null;
            const nodes = get(key,'nodeType') === 11 ? children(key) : [key];
            return {op,nodes,was:nodes.map(connected),adopted:nodes.map(node=>({elements:elements(node),oldDocument:get(node,'ownerDocument')}))};
        }
        if (op === 'remove') return {op,node,was:connected(node),parent:get(node,'parentNode')};
        if (op === 'clone' || op === 'import') return {op};
        if (op === 'set' && (key === 'innerHTML' || key === 'textContent') && [1,11].includes(get(node,'nodeType')))
            return {op:'children',node,nodes:children(node),was:connected(node)};
        const state = states.get(node);
        if (!state || state.state !== 'custom') return null;
        let forced;
        if (op === 'attr') forced = lower(string(key));
        else if (op === 'style') forced = 'style';
        else if (op === 'set' && ['async','selected'].includes(key)) forced = key;
        if (forced === undefined) return null;
        return {op:'attribute',node,forced,old:attr(node,forced)};
    }
    function after(token, result) {
        if (!token) return;
        function adopted(record){for(const element of record.elements){
            const old=record.oldDocument,now=get(element,'ownerDocument'),s=states.get(element);
            if(old!==now && s && s.state==='custom')callback(element,'adoptedCallback',[old,now]);
        }}
        if (token.op === 'insert') {
            token.nodes.forEach((node,i) => { removed(node,token.was[i]);adopted(token.adopted[i]);inserted(node); });
        } else if(token.op==='adopt'){
            removed(token.node,token.was);adopted(token);
        } else if (token.op === 'remove') {
            if (token.parent && get(token.node,'parentNode') !== token.parent) removed(token.node,token.was);
        } else if (token.op === 'clone' || token.op === 'import') upgradeTree(result);
        else if (token.op === 'children') {
            for (const node of token.nodes) removed(node,token.was);
            for (const node of children(token.node)) { upgradeTree(node); inserted(node); }
        } else if (token.op === 'attribute') {
            const current = rawDom('attrNS',token.node,token.namespace??null,token.forced);
            if (token.old !== null || current !== null) attributeChanged(token.node,token.forced,token.old,current,token.namespace??null);
        }
        // Cloning creates a separate native tree: existing FACE ownership is
        // unchanged. Each clone upgrade refreshes just its new FACE; any author
        // constructor DOM mutation still takes the normal synchronous path.
        if(token.op!=='clone' && token.op!=='import' && (token.op!=='attribute'||(!token.namespace&&['form','id','disabled'].includes(token.forced))))formRefresh();
    }

    function internalsInfo(element){
        const s=states.get(element);
        return s && (s.state==='custom'||s.state==='precustomized') ? s.definition : null;
    }
    function refreshFormElement(element){
        const s=states.get(element);if(!s||s.state!=='custom'||!s.definition.formAssociated)return;
        const form=rawDom('face',element,'form'),disabled=rawDom('face',element,'disabled');
        const oldForm=s.formOwner??null,oldDisabled=s.formDisabled??false;
        s.formOwner=form;s.formDisabled=disabled;
        if(form!==oldForm)callback(element,'formAssociatedCallback',[form]);
        if(disabled!==oldDisabled)callback(element,'formDisabledCallback',[disabled]);
    }
    function formRefresh(){
        for(const element of faceElements)refreshFormElement(element);
    }
    function formReset(form){
        reactions(()=>{
            // The private native collector snapshots successful FACE ownership.
            for(const element of rawDom('faceControls',form))
                if(rawDom('face',element,'form')===form)callback(element,'formResetCallback');
        });
    }
    class CustomElementRegistry {
        constructor(key) { if (key !== registryKey) throw new TypeError('Scoped custom element registries are not implemented'); }
        define(name, constructor, options={}) {
            if (this !== registry) throw new TypeError('Illegal invocation');
            if (arguments.length < 2) throw new TypeError('define requires name and constructor');
            name = string(name);
            return reactions(() => {
                if (!isConstructor(constructor)) throw new TypeError('Expected constructor');
                if (!validName(name)) throw fail('Invalid custom element name','SyntaxError');
                if (definitions.has(name) || constructors.has(constructor)) throw fail('Custom element name or constructor is already defined');
                if (options != null && options.extends !== undefined) throw fail('Customized built-in elements are not implemented');
                if (defining) throw fail('Custom element definition is already running');
                defining = true;
                let d;
                try {
                    const prototype = constructor.prototype;
                    if (!isObject(prototype)) throw new TypeError('Constructor prototype must be an object');
                    const callbacks = Object.create(null);
                    for (const key of ['connectedCallback','disconnectedCallback','connectedMoveCallback','adoptedCallback','attributeChangedCallback']) {
                        const value = prototype[key];
                        if (value !== undefined && typeof value !== 'function') throw new TypeError(key+' must be callable');
                        callbacks[key] = value || null;
                    }
                    let observed = [];
                    if (callbacks.attributeChangedCallback) {
                        const value = constructor.observedAttributes;
                        if (value !== undefined) observed = sequence(value);
                    }
                    const disabled = constructor.disabledFeatures;
                    const disabledFeatures=disabled===undefined?[]:sequence(disabled);
                    const formAssociated = !!constructor.formAssociated;
                    if (formAssociated) for (const key of ['formAssociatedCallback','formResetCallback','formDisabledCallback','formStateRestoreCallback']) {
                        const value = prototype[key];
                        if (value !== undefined && typeof value !== 'function') throw new TypeError(key+' must be callable');
                        callbacks[key] = value || null;
                    }
                    // Definition is not use of ElementInternals. Record the
                    // declared metadata instead of rejecting the whole bundle
                    // before its own feature-detected polyfill can be evaluated.
                    // Native form linkage, values and lifecycle delivery remain
                    // unavailable; no attachInternals capability is fabricated.
                    d = {name,constructor,callbacks,observed,stack:[],formAssociated,
                        disableInternals:disabledFeatures.includes('internals'),disableShadow:disabledFeatures.includes('shadow')};
                } finally { defining = false; }
                definitions.set(name,d); constructors.set(constructor,d);
                for (const element of elements(document,name)) tryUpgrade(element);
                const waiting = pending.get(name);
                if (waiting) { pending.delete(name); waiting.resolve(constructor); }
            });
        }
        get(name) {
            if (this !== registry) throw new TypeError('Illegal invocation');
            if (!arguments.length) throw new TypeError('get requires a name');
            const d = definitions.get(string(name)); return d ? d.constructor : undefined;
        }
        getName(constructor) {
            if (this !== registry || typeof constructor !== 'function') throw new TypeError('Expected constructor');
            const d = constructors.get(constructor); return d ? d.name : null;
        }
        whenDefined(name) {
            try {
                if (this !== registry || !arguments.length) throw new TypeError('whenDefined requires a name');
                name = string(name);
                if (!validName(name)) throw fail('Invalid custom element name','SyntaxError');
                const d = definitions.get(name); if (d) return Promise.resolve(d.constructor);
                if (!pending.has(name)) {
                    let resolve; const promise = new Promise(r => { resolve = r; }); pending.set(name,{promise,resolve});
                }
                return pending.get(name).promise;
            } catch (e) { return Promise.reject(e); }
        }
        upgrade(root) {
            if (this !== registry || !root) throw new TypeError('upgrade requires a Node');
            get(root,'nodeType');
            return reactions(() => upgradeTree(root));
        }
    }
    const registry = new CustomElementRegistry(registryKey);
    Object.defineProperty(CustomElementRegistry.prototype, Symbol.toStringTag, {value:'CustomElementRegistry',configurable:true});
    Object.defineProperty(globalThis, 'customElements', {get() { return registry; },enumerable:true,configurable:true});
    Object.defineProperty(globalThis, 'CustomElementRegistry', {value:CustomElementRegistry,writable:true,configurable:true});
    function allowShadow(element){const d=definitions.get(get(element,'localName'));return !d || !d.disableShadow;}
    return {construct,create,reactions,inserted,removed,attributeChanged,upgradeTree,before,after,allowShadow,internalsInfo,formRefresh,formReset};
})();
