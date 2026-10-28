/* Custom elements on Nocturne's existing native DOM wrappers.
 * Algorithms: https://html.spec.whatwg.org/multipage/custom-elements.html
 * Native shadow trees participate in upgrade/connection/adoption. Form-associated
 * definition metadata connects ElementInternals to actual native ownership,
 * validation, submission and CE form callbacks. Registry and internal is values
 * are native node metadata, so parsing, cloning and adoption use the same tree.
 * Embedded in js_bootstrap.js; rawDom/document/HTMLElement/report are private. */
const customElementsBridge = (() => {
    'use strict';
    const HTML = 'http://www.w3.org/1999/xhtml';
    /* HTML keeps this state per similar-origin window agent, and every realm of this runtime
       (the frames' Windows) is that agent's (host.customElementAgent): a registry made in one
       realm defines, upgrades and creates elements of another realm's documents. The scoped
       registries are numbered for the native nodes; a global registry is named by its Window's
       Document instead. */
    const agent = host.customElementAgent(() => ({
        registryStates: new WeakMap(), registries: new Map(), states: new WeakMap(),
        constructors: new Map(), /* the active custom element constructor map */
        scopes: [], backup: [], backupScheduled: false, /* the custom element reactions stack */
        faceElements: new Set(), parserNames: new Set(), parserBuiltins: new Map(),
        constructed: Symbol('already constructed'),
        nextScoped: 1, definitions: 0, scoped: 0, constructing: 0,
    }));
    const {registryStates, registries, states, scopes, backup, faceElements, parserNames, parserBuiltins, constructed} = agent;
    const nativeInterfaces = new Map(), parserConnections = new Set();
    function invokeConstructor(d, registry) {
        // The active custom element constructor map entry lasts for this construction only.
        const C = d.constructor, had = agent.constructors.has(C), previous = agent.constructors.get(C);
        agent.constructors.set(C, registry);
        agent.constructing++;
        try{
            // The parser candidate set is a union of all registries. Only an
            // actual author constructor fixes this document's byte encoding.
            rawDom.customData(document,'authorConstructor');
            return Reflect.construct(C,[]);
        }finally{
            agent.constructing--;
            if(had)agent.constructors.set(C,previous);else agent.constructors.delete(C);
        }
    }
    const registryKey = {};
    const reserved = new Set(['annotation-xml','color-profile','font-face','font-face-src',
        'font-face-uri','font-face-format','font-face-name','missing-glyph']);
    const StringImpl = String;
    const string = value => { if (typeof value === 'symbol') throw new TypeError('Cannot convert Symbol to DOMString'); return StringImpl(value); };
    const lower = name => name.replace(/[A-Z]/g, c => c.toLowerCase());
    const get = (node, key) => rawDom.get(node, key);
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
    const nodeDocument = node => get(node,'nodeType') === 9 ? node : get(node,'ownerDocument');
    // Traverse the actual native tree once, in tree order. Ordinary HTML/text
    // nodes never need wrappers or per-node JS/native crossings for this query.
    const elements = (root, name=null) => rawDom.customCandidates(root,name);
    function registryState(value) {
        const state=registryStates.get(value);
        if(!state)throw new TypeError('CustomElementRegistry receiver required');
        return state;
    }
    // Web IDL conversion to CustomElementRegistry (or null when nullable); undefined is absent.
    function registryOption(value, nullable=true) {
        if(value===undefined)return undefined;
        if(value===null && nullable)return null;
        if(!registryStates.has(value))throw new TypeError('CustomElementRegistry required');
        return value;
    }
    // The native slot names a scoped registry by number and a global one by its Window's Document.
    function identity(value) {
        if(value===null)return [-1];
        const state=registryState(value);
        return state.scoped?[state.id]:[0,state.document];
    }
    const setRegistry = (node,value) => rawDom.customData(node,'registry',...identity(value));
    // HTML "look up a custom element registry" (any other node: null).
    function registryFor(node) {
        const type=get(node,'nodeType');
        if(type!==1 && type!==9 && !(type===11 && get(node,'shadowHost')))return null;
        const value=rawDom.customData(node,'registry');
        if(typeof value!=='number')return value; // null, or another realm's global registry
        return value===0?registry:value>0?registries.get(value)||null:null;
    }
    const isScoped = value => value!==null && registryState(value).scoped;
    // DOM: a global registry other than document's registry is never chosen for its nodes.
    const globalOther = (value,owner) => value!==null && !isScoped(value) && value!==registryFor(owner);
    // HTML "look up a custom element definition".
    function definitionFor(value, namespace, localName, is) {
        if(value===null || namespace!==HTML)return null;
        const table=registryState(value).definitions;
        const autonomous=table.get(localName);
        if(autonomous && autonomous.localName===localName)return autonomous;
        const builtin=is===null?null:table.get(is);
        return builtin && builtin.localName===localName?builtin:null;
    }
    const lookup = element => definitionFor(registryFor(element),get(element,'namespaceURI'),get(element,'localName'),rawDom.customData(element,'is'));
    function nativeInterface(localName) {
        let proto=nativeInterfaces.get(localName);
        if(!proto){proto=Object.getPrototypeOf(rawDom.create(null,1,localName,''));nativeInterfaces.set(localName,proto);}
        return proto;
    }
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
            if (!agent.backupScheduled) {
                agent.backupScheduled = true;
                queueMicrotask(() => { try { invoke(backup); } finally { agent.backupScheduled = false; } });
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
        const d = lookup(element);
        const s = states.get(element);
        if (d && (!s || s.state === 'undefined' || s.state === 'precustomized')) enqueue(element, {definition:d});
    }
    function upgradeTree(root) {
        if (!agent.definitions) return;
        for (const element of elements(root)) tryUpgrade(element);
    }
    function upgrade(element, d) {
        const s = stateFor(element);
        if (s.state !== 'undefined') return;
        s.definition = d; s.state = 'failed';
        for (const attribute of rawDom.attrList(element)) callback(element, 'attributeChangedCallback',
            [get(attribute,'localName'),null,get(attribute,'attrValue'),get(attribute,'namespaceURI')]);
        if (connected(element)) callback(element, 'connectedCallback');
        d.stack.push(element);
        try {
            if(d.disableShadow && get(element,'shadowRoot'))throw fail('This custom element disables shadow roots');
            s.state = 'precustomized';
            rawDom.face(element,'metadata',d.formAssociated && d.localName===d.name);
            if (invokeConstructor(d, registryFor(element)) !== element) throw new TypeError('Custom element constructor returned a different object');
            s.state = 'custom';
            if(d.formAssociated && d.localName===d.name){faceElements.add(element);refreshFormElement(element);}
        } catch (e) {
            rawDom.face(element,'metadata',false);
            faceElements.delete(element);
            s.state = 'failed'; s.definition = null; s.reactions.length = 0;
            throw e;
        } finally { d.stack.pop(); }
    }
    // HTML [HTMLConstructor]; base is the active function object.
    function construct(newTarget,base=HTMLElement) {
        if (newTarget === base) throw new TypeError('Illegal HTML element constructor');
        const value = agent.constructors.has(newTarget) ? agent.constructors.get(newTarget) : registryFor(document);
        const d = value === null ? undefined : registryState(value).constructors.get(newTarget);
        if (!d) throw new TypeError('Illegal HTML element constructor');
        if(d.localName===d.name?base!==HTMLElement:nativeInterface(d.localName)!==base.prototype)
            throw new TypeError('Custom element definition does not match this HTML interface');
        if (!d.stack.length) {
            const element = rawDom.create(null, 1, d.localName, '');
            setRegistry(element,value);
            if(d.localName!==d.name)rawDom.customData(element,'is',d.name);
            rawDom.face(element,'metadata',d.formAssociated && d.localName===d.name);
            let prototype = newTarget.prototype;
            if (!isObject(prototype)) prototype = base.prototype;
            Object.setPrototypeOf(element, prototype);
            states.set(element, {state:'custom', definition:d, reactions:[]});
            if(d.formAssociated && d.localName===d.name)faceElements.add(element);
            return element;
        }
        let prototype = newTarget.prototype;
        if (!isObject(prototype)) prototype = base.prototype;
        const i = d.stack.length - 1, element = d.stack[i];
        if (element === constructed) throw new TypeError('Custom element is already constructed');
        Object.setPrototypeOf(element, prototype);
        d.stack[i] = constructed;
        return element;
    }
    // Web IDL (DOMString or ElementCreationOptions): null for the string, else the dictionary's
    // members in their order (absent ones undefined).
    function creationOptions(options) {
        if(options!==undefined && options!==null && !isObject(options)){string(options);return null;}
        const value=registryOption(options==null?undefined:options.customElementRegistry);
        const is=options==null?undefined:options.is;
        return {registry:value,is:is===undefined?undefined:string(is)};
    }
    // DOM "flatten element creation options".
    function flatten(options, owner) {
        let value=registryFor(owner),is=null;
        if(options!==null){
            if(options.is!==undefined)is=options.is;
            if(options.registry!==undefined){
                if(is!==null)throw fail('An is value and a customElementRegistry cannot be given together');
                value=options.registry;
            }
            if(globalOther(value,owner))throw fail('A global CustomElementRegistry of another document');
        }
        return {registry:value,is};
    }
    // DOM "create an element" (registry "default" is undefined).
    function create(owner, localName, namespace, prefix=null, is=null, synchronous=false, value=undefined) {
        if(get(owner,'nodeType')!==9)throw new TypeError('Document receiver required');
        if(value===undefined)value=registryFor(owner);
        const make=isValue=>{
            const element=namespace===HTML && prefix===null?rawDom.create(owner,1,localName,''):rawDom.create(owner,1,localName,'',namespace,prefix);
            setRegistry(element,value);
            if(isValue!==null)rawDom.customData(element,'is',isValue);return element;
        };
        const d=definitionFor(value,namespace,localName,is);
        if(!d)return make(is);
        if(d.localName!==d.name){
            const element=make(is);
            if(!synchronous){enqueue(element,{definition:d});return element;}
            return reactions(()=>{
                try{upgrade(element,d);}catch(e){report(e);stateFor(element).state='failed';}
                return element;
            });
        }
        if(!synchronous){const element=make(null);enqueue(element,{definition:d});return element;}
        return reactions(() => {
            try {
                const element = invokeConstructor(d, value);
                // The constructor's result converts to HTMLElement.
                if (!rawDom.isNode(null,element) || get(element,'nodeType') !== 1 || get(element,'namespaceURI') !== HTML)
                    throw new TypeError('Constructor did not create an HTMLElement');
                if (get(element,'attributeNames').length) throw fail('The custom element constructor gave the result attributes');
                if (children(element).length) throw fail('The custom element constructor gave the result children');
                if (get(element,'parentNode')) throw fail('The custom element constructor gave the result a parent');
                if (get(element,'ownerDocument')!==owner) throw fail('The custom element constructor\'s result belongs to another document');
                if (get(element,'localName') !== localName) throw fail('The custom element constructor\'s result has another local name');
                if(prefix!==null)rawDom.set(element,'elementPrefix',prefix);
                rawDom.customData(element,'is',null);
                setRegistry(element,value);
                return element;
            } catch (e) {
                report(e);
                const element = make(null);
                Object.setPrototypeOf(element, HTMLUnknownElement.prototype);
                states.set(element, {state:'failed', definition:null, reactions:[]});
                return element;
            }
        });
    }
    // DOM "insert": connected elements with a registry record their document in a scoped
    // registry's scoped document set, then get connectedCallback or an upgrade attempt.
    function inserted(root) {
        if (!connected(root)) return;
        for (const element of elements(root)) {
            const value = registryFor(element);
            if (value === null) continue;
            if (isScoped(value)) registryState(value).documents.add(nodeDocument(element));
            if (!agent.definitions) continue;
            const s = states.get(element);
            if (s && s.state === 'custom') {parserConnections.delete(element);callback(element, 'connectedCallback');}
            else tryUpgrade(element);
        }
    }
    function removed(root, wasConnected) {
        if (!wasConnected || !agent.definitions) return;
        for (const element of elements(root)) {
            const s = states.get(element);
            if (s && s.state === 'custom') callback(element, 'disconnectedCallback');
        }
    }
    /* HTML "enqueue a custom element callback reaction" for connectedMoveCallback: without one,
       disconnectedCallback then connectedCallback, as one callback */
    function movedCallback(element) {
        const s = states.get(element), d = s && s.state === 'custom' && s.definition;
        if (!d) return;
        if (d.callbacks.connectedMoveCallback) { enqueue(element, {callback:d.callbacks.connectedMoveCallback, args:[]}); return; }
        const disconnectedCallback = d.callbacks.disconnectedCallback, connectedCallback = d.callbacks.connectedCallback;
        if (!disconnectedCallback && !connectedCallback) return;
        enqueue(element, {callback() {
            if (disconnectedCallback) apply(disconnectedCallback, this, []);
            if (connectedCallback) apply(connectedCallback, this, []);
        }, args:[]});
    }
    function attributeChanged(element, name, oldValue, newValue, namespace=null) {
        const s = states.get(element);
        if (s && s.state === 'custom')
            callback(element, 'attributeChangedCallback', [name,oldValue,newValue,namespace]);
    }
    const active = () => agent.definitions !== 0 || agent.scoped !== 0;
    // Snapshot native values only. Page overrides of getters/prototypes cannot
    // make internal tree algorithms operate on a different, invented DOM.
    function before(op, node, key, value, extra) {
        if (!active() || !node) return null;
        if(op==='adopt' && key && get(key,'nodeType')!==2)
            return {op,node:key,was:connected(key),parent:get(key,'parentNode'),elements:elements(key),oldDocument:get(key,'ownerDocument')};
        const change=attributeBridge.mutation(op,node,key,value,extra);
        if(change)return {op:'attribute',node:change.node,forced:change.name,namespace:change.namespace,old:change.oldValue};
        if (op === 'insert') {
            if (!key || key === value) return null;
            const nodes = get(key,'nodeType') === 11 ? children(key) : [key];
            return {op,nodes,was:nodes.map(connected),adopted:nodes.map(node=>({elements:elements(node),oldDocument:get(node,'ownerDocument')}))};
        }
        if(op==='replace'){
            const nodes=get(key,'nodeType')===11?children(key):[key];
            return {op,nodes,was:nodes.map(connected),old:value,oldWas:connected(value),same:key===value,
                adopted:nodes.map(node=>({elements:elements(node),oldDocument:get(node,'ownerDocument')}))};
        }
        if (op === 'move') return key ? {op,node:key} : null;
        if (op === 'remove') return {op,node,was:connected(node),parent:get(node,'parentNode')};
        if(op==='replaceAll')return {op:'children',node,nodes:children(node),was:connected(node)};
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
        function adopted(record){if(!agent.definitions)return;for(const element of record.elements){
            const old=record.oldDocument,now=get(element,'ownerDocument'),s=states.get(element);
            if(old!==now && s && s.state==='custom')callback(element,'adoptedCallback',[old,now]);
        }}
        if (token.op === 'insert') {
            token.nodes.forEach((node,i) => { removed(node,token.was[i]);adopted(token.adopted[i]);inserted(node); });
        } else if(token.op==='replace'){
            token.nodes.forEach((node,i)=>{removed(node,token.was[i]);adopted(token.adopted[i]);});
            if(!token.same)removed(token.old,token.oldWas);
            for(const node of token.nodes)inserted(node);
        } else if(token.op==='adopt'){
            removed(token.node,token.was);adopted(token);
        } else if (token.op === 'move') {
            if (connected(token.node)) for (const element of elements(token.node)) movedCallback(element);
        } else if (token.op === 'remove') {
            if (token.parent && get(token.node,'parentNode') !== token.parent) removed(token.node,token.was);
        } else if (token.op === 'clone' || token.op === 'import') upgradeTree(result);
        else if (token.op === 'children') {
            for (const node of token.nodes) removed(node,token.was);
            for (const node of children(token.node)) { upgradeTree(node); inserted(node); }
        } else if (token.op === 'attribute') {
            const current = rawDom.attrNS(token.node,token.namespace??null,token.forced);
            if (token.old !== null || current !== null) attributeChanged(token.node,token.forced,token.old,current,token.namespace??null);
        }
        // Cloning creates a separate native tree: existing FACE ownership is
        // unchanged. Each clone upgrade refreshes just its new FACE; any author
        // constructor DOM mutation still takes the normal synchronous path.
        if(token.op!=='clone' && token.op!=='import' && (token.op!=='attribute'||(!token.namespace&&['form','id','disabled'].includes(token.forced))))formRefresh();
    }

    function internalsInfo(element){
        const s=states.get(element);
        return s && (s.state==='custom'||s.state==='precustomized') && s.definition.localName===s.definition.name ? s.definition : null;
    }
    function refreshFormElement(element){
        const s=states.get(element);if(!s||s.state!=='custom'||!s.definition.formAssociated)return;
        const form=rawDom.face(element,'form'),disabled=rawDom.face(element,'disabled');
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
            for(const element of rawDom.faceControls(form))
                if(rawDom.face(element,'form')===form)callback(element,'formResetCallback');
        });
    }
    class CustomElementRegistry {
        // new CustomElementRegistry() is scoped; each Window's Document gets its global one.
        constructor(key) {
            const global=key===registryKey,id=global?0:agent.nextScoped++;
            registryStates.set(this,{id,scoped:!global,definitions:new Map(),constructors:new Map(),pending:new Map(),
                documents:new Set(),defining:false,document:global?document:null});
            if(!global){
                registries.set(id,this);
                if(!agent.scoped++ && host.domHooks)host.domHooks(3,true);
            }
        }
        define(name, constructor, options={}) {
            const rs=registryState(this);
            if (arguments.length < 2) throw new TypeError('define requires name and constructor');
            name = string(name);
            return reactions(() => {
                if (!isConstructor(constructor)) throw new TypeError('Expected constructor');
                if (!validName(name)) throw fail('Invalid custom element name','SyntaxError');
                if (rs.definitions.has(name) || rs.constructors.has(constructor)) throw fail('Custom element name or constructor is already defined');
                if(options!=null && !isObject(options))throw new TypeError('ElementDefinitionOptions must be a dictionary');
                const extension=options==null?undefined:options.extends;
                let localName=name;
                if(extension!==undefined){
                    localName=string(extension);
                    if(rs.scoped || !/^[a-z][a-z0-9]*$/.test(localName) || validName(localName) || nativeInterface(localName)===HTMLUnknownElement.prototype)
                        throw fail('Cannot extend this element in this registry');
                }
                if (rs.defining) throw fail('Custom element definition is already running');
                rs.defining = true;
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
                    d = {name,localName,registry:this,constructor,callbacks,observed,stack:[],formAssociated,
                        disableInternals:disabledFeatures.includes('internals'),disableShadow:disabledFeatures.includes('shadow')};
                } finally { rs.defining = false; }
                rs.definitions.set(name,d); rs.constructors.set(constructor,d);agent.definitions++;
                if(localName===name)parserNames.add(name);
                else{let names=parserBuiltins.get(localName);if(!names)parserBuiltins.set(localName,names=new Set());names.add(name);}
                // The realms whose documents this definition's elements can be in.
                const where=rs.scoped?undefined:rs.document;
                if(host.domHooks)host.domHooks(0,true,where);
                if(d.formAssociated && host.domHooks)host.domHooks(2,true,where);
                // "Upgrade particular elements within a document": shadow-including descendants
                // whose registry is this, HTML namespace and local name, and is value for a
                // customized built-in; scoped: each of its scoped document set.
                for(const owner of rs.scoped?[...rs.documents]:[rs.document])
                    for(const element of elements(owner,localName))
                        if(get(element,'localName')===localName && (localName===name || rawDom.customData(element,'is')===name) &&
                           registryFor(element)===this)enqueue(element,{definition:d});
                const waiting = rs.pending.get(name);
                if (waiting) { rs.pending.delete(name); waiting.resolve(constructor); }
            });
        }
        get(name) {
            const rs=registryState(this);
            if (!arguments.length) throw new TypeError('get requires a name');
            const d = rs.definitions.get(string(name)); return d ? d.constructor : undefined;
        }
        getName(constructor) {
            const rs=registryState(this);
            if(typeof constructor !== 'function') throw new TypeError('Expected constructor');
            const d = rs.constructors.get(constructor); return d ? d.name : null;
        }
        whenDefined(name) {
            try {
                const rs=registryState(this);
                if (!arguments.length) throw new TypeError('whenDefined requires a name');
                name = string(name);
                if (!validName(name)) throw fail('Invalid custom element name','SyntaxError');
                const d = rs.definitions.get(name); if (d) return Promise.resolve(d.constructor);
                if (!rs.pending.has(name)) {
                    let resolve; const promise = new Promise(r => { resolve = r; }); rs.pending.set(name,{promise,resolve});
                }
                return rs.pending.get(name).promise;
            } catch (e) { return Promise.reject(e); }
        }
        upgrade(root) {
            registryState(this);
            if (!root) throw new TypeError('upgrade requires a Node');
            get(root,'nodeType');
            return reactions(() => {for(const element of elements(root))if(registryFor(element)===this)tryUpgrade(element);});
        }
        initialize(root) {
            const rs=registryState(this);
            if(!arguments.length)throw new TypeError('initialize requires a Node');
            if(!rawDom.isNode(null,root))throw new TypeError('initialize requires a Node');
            const type=get(root,'nodeType');
            if(!rs.scoped && (type===9 || registryFor(get(root,'ownerDocument'))!==this))
                throw fail('A global registry initializes only nodes of its own Document');
            return reactions(()=>{
                if((type===9 || type===11 && get(root,'shadowHost')) && registryFor(root)===null)setRegistry(root,this);
                // Inclusive descendants in tree order: not shadow trees, not template contents.
                const tree=rawDom.query(root,'*',false);
                for(const element of type===1?[root,...tree]:tree){
                    if(registryFor(element)===null){
                        setRegistry(element,this);
                        if(rs.scoped)rs.documents.add(nodeDocument(element));
                    }
                    if(registryFor(element)===this)tryUpgrade(element);
                }
            });
        }
    }
    const registry = new CustomElementRegistry(registryKey);
    // The Window's Document is created with its global registry; other realms reading it see it.
    host.customElementAgent(null,registry);
    setRegistry(document,registry);
    Object.defineProperty(CustomElementRegistry.prototype, Symbol.toStringTag, {value:'CustomElementRegistry',configurable:true});
    Object.defineProperty(globalThis, 'customElements', {get() { return registry; },enumerable:true,configurable:true});
    Object.defineProperty(globalThis, 'CustomElementRegistry', {value:CustomElementRegistry,writable:true,configurable:true});
    Object.defineProperty(Document.prototype,'customElementRegistry',{configurable:true,enumerable:true,get(){
        if(get(this,'nodeType')!==9)throw new TypeError('Document receiver required');return registryFor(this);
    }});
    Object.defineProperty(Element.prototype,'customElementRegistry',{configurable:true,enumerable:true,get(){
        get(this,'elementBrand');return registryFor(this);
    }});
    function parserCreate(element,is=null,synchronous=true){
        if(is!==null)rawDom.customData(element,'is',string(is));
        const d=lookup(element);if(!d)return element;
        if(!synchronous){enqueue(element,{definition:d});return element;}
        const owner=get(element,'ownerDocument'),value=registryFor(element);
        const queue=[];scopes.push(queue);
        try{
            if(d.localName===d.name){
                // Autonomous synchronous creation is not an upgrade. Construct
                // a separate native element so a saved constructor `this` and
                // shadow tree keep their identity even when creation fails.
                const made=invokeConstructor(d,value),s=states.get(made);
                if(!s || s.definition!==d || get(made,'nodeType')!==1 || get(made,'namespaceURI')!==HTML)
                    throw new TypeError('Constructor did not create an HTMLElement');
                if(get(made,'localName')!==d.localName || get(made,'parentNode') || children(made).length || get(made,'attributeNames').length || get(made,'ownerDocument')!==document)
                    throw fail('Parser custom element constructor changed its parent, children, attributes or document');
                if(owner!==document)rawDom.adopt(owner,made);
                element=made;
            }else upgrade(element,d);
            const s=stateFor(element);s.parserAttributesPending=true;parserConnections.add(element);
            return element;
        }catch(error){
            const s=stateFor(element);s.state='failed';s.definition=null;s.reactions.length=0;
            faceElements.delete(element);rawDom.face(element,'metadata',false);
            report(error);return element;
        }finally{scopes.pop();invoke(queue);}
    }
    function parserFinish(element){
        const s=states.get(element);if(!s || s.state!=='custom' || !s.parserAttributesPending)return element;
        s.parserAttributesPending=false;
        reactions(()=>{for(const a of rawDom.attrList(element))callback(element,'attributeChangedCallback',
            [get(a,'localName'),null,get(a,'attrValue'),get(a,'namespaceURI')]);});
        return element;
    }
    function parserInserted(){
        reactions(()=>{for(const element of parserConnections){
            if(!connected(element))continue;
            parserConnections.delete(element);callback(element,'connectedCallback');refreshFormElement(element);
        }});
    }
    function parserCandidate(name,is=null){return parserNames.has(name) || is!==null && !!parserBuiltins.get(name)?.has(is);}
    function allowShadow(element){const d=lookup(element);return !d || !d.disableShadow;}
    return {construct,create,creationOptions,flatten,reactions,inserted,removed,attributeChanged,upgradeTree,before,after,allowShadow,internalsInfo,
        formRefresh,formReset,registryFor,registryOption,setRegistry,identity,globalOther,parserCreate,parserFinish,parserInserted,parserCandidate,
        active,constructing:()=>agent.constructing>0};
})();
