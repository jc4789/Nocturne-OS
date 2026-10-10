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
    const registryStates = new WeakMap(), registries = new Map(), activeDefinitions = new WeakMap();
    const states = new WeakMap(), scopes = [], backup = [], faceElements=new Set(), parserConnections=new Set();
    const nativeInterfaces = new Map(),parserNames=new Set(),parserBuiltins=new Map();
    let constructionDepth=0, nextRegistry=1, definitionCount=0;
    function invokeConstructor(d) {
        const previous=activeDefinitions.get(d.constructor);
        activeDefinitions.set(d.constructor,d);
        constructionDepth++;
        try{
            // The parser candidate set is a union of all registries. Only an
            // actual author constructor fixes this document's byte encoding.
            rawDom.customData(document,'authorConstructor');
            return Reflect.construct(d.constructor,[]);
        }finally{
            constructionDepth--;
            if(previous)activeDefinitions.set(d.constructor,previous);else activeDefinitions.delete(d.constructor);
        }
    }
    const constructed = Symbol('already constructed'), registryKey = {};
    const reserved = new Set(['annotation-xml','color-profile','font-face','font-face-src',
        'font-face-uri','font-face-format','font-face-name','missing-glyph']);
    let backupScheduled = false;
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
    // Traverse the actual native tree once, in tree order. Ordinary HTML/text
    // nodes never need wrappers or per-node JS/native crossings for this query.
    const elements = (root, name=null) => rawDom.customCandidates(root,name);
    function registryState(value) {
        const state=registryStates.get(value);
        if(!state)throw new TypeError('CustomElementRegistry receiver required');
        return state;
    }
    function registryOption(value, fallback) {
        if(value===undefined)return fallback;
        if(value===null)return null;
        registryState(value);return value;
    }
    function setRegistry(node,value) {
        rawDom.customData(node,'registry',value===null?-1:registryState(value).id);
        if(value!==null){
            const state=registryState(value);
            if(state.scoped)state.documents.add(get(node,'nodeType')===9?node:get(node,'ownerDocument'));
        }
    }
    function registryFor(node) {
        const type=get(node,'nodeType');
        if(type!==1 && type!==9 && !(type===11 && get(node,'shadowHost')))return null;
        const id=rawDom.customData(node,'registry');
        if(id!==-2)return id===-1?null:registries.get(id)||null;
        // Older/native callers may leave the slot unassigned. Resolve once;
        // subsequent moves never replace an element's established registry.
        const value=type===9?(get(node,'scripting')?registry:null):registryFor(get(node,'ownerDocument'));
        setRegistry(node,value);return value;
    }
    function lookup(element) {
        if(get(element,'namespaceURI')!==HTML)return null;
        const value=registryFor(element);if(value===null)return null;
        const table=registryState(value).definitions,local=get(element,'localName'),is=rawDom.customData(element,'is');
        const autonomous=table.get(local);
        if(autonomous && autonomous.localName===local && autonomous.name===local)return autonomous;
        const builtin=is===null?null:table.get(is);
        return builtin && builtin.localName===local && builtin.name!==local?builtin:null;
    }
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
        const d = lookup(element);
        const s = states.get(element);
        if (d && (!s || s.state === 'undefined' || s.state === 'precustomized')) enqueue(element, {definition:d});
    }
    function upgradeTree(root) {
        if (!definitionCount) return;
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
            if (invokeConstructor(d) !== element) throw new TypeError('Custom element constructor returned a different object');
            s.state = 'custom';
            if(d.formAssociated && d.localName===d.name){faceElements.add(element);refreshFormElement(element);}
        } catch (e) {
            rawDom.face(element,'metadata',false);
            faceElements.delete(element);
            s.state = 'failed'; s.definition = null; s.reactions.length = 0;
            throw e;
        } finally { d.stack.pop(); }
    }
    function construct(newTarget,base=HTMLElement) {
        const d = activeDefinitions.get(newTarget) || registryState(registry).constructors.get(newTarget);
        if (newTarget === base || !d) throw new TypeError('Illegal HTML element constructor');
        if(d.localName===d.name?base!==HTMLElement:nativeInterface(d.localName)!==base.prototype)
            throw new TypeError('Custom element definition does not match this HTML interface');
        let prototype = newTarget.prototype;
        if (!isObject(prototype)) prototype = base.prototype;
        if (d.stack.length) {
            const i = d.stack.length - 1, element = d.stack[i];
            if (element === constructed) throw new TypeError('Custom element is already constructed');
            Object.setPrototypeOf(element, prototype);
            d.stack[i] = constructed;
            return element;
        }
        const element = rawDom.create(null, 1, d.localName, '');
        setRegistry(element,d.registry);
        if(d.localName!==d.name)rawDom.customData(element,'is',d.name);
        rawDom.face(element,'metadata',d.formAssociated && d.localName===d.name);
        Object.setPrototypeOf(element, prototype);
        states.set(element, {state:'custom', definition:d, reactions:[]});
        if(d.formAssociated && d.localName===d.name)faceElements.add(element);
        return element;
    }
    function create(name, options, preserveCase=false,owner=document) {
        name = string(name); if (!preserveCase) name = lower(name);
        if(get(owner,'nodeType')!==9)throw new TypeError('Document receiver required');
        let value=registryFor(owner),is=null;
        if(isObject(options)){
            value=registryOption(options.customElementRegistry,value);
            const optionIs=options.is;if(optionIs!==undefined)is=string(optionIs);
        }
        const make=()=>{const element=rawDom.create(owner,1,name,'');setRegistry(element,value);
            if(is!==null)rawDom.customData(element,'is',is);return element;};
        const table=value===null?null:registryState(value).definitions;
        const candidate=table && (table.get(name)||is!==null && table.get(is));
        const d=candidate && candidate.localName===name && (candidate.name===name || candidate.name===is)?candidate:null;
        if(!d)return make();
        return reactions(() => {
            if(d.localName!==d.name){const element=make();try{upgrade(element,d);}catch(e){report(e);}return element;}
            try {
                const element = invokeConstructor(d), s = states.get(element);
                if (!s || s.definition !== d || get(element,'nodeType') !== 1 || get(element,'namespaceURI') !== HTML)
                    throw new TypeError('Constructor did not create an HTMLElement');
                if (get(element,'localName') !== name || get(element,'parentNode') || children(element).length || get(element,'attributeNames').length || get(element,'ownerDocument')!==document)
                    throw fail('Custom element constructor changed its name, parent, children or attributes');
                if(get(element,'ownerDocument')!==owner)rawDom.adopt(owner,element);
                return element;
            } catch (e) {
                report(e);
                const element = make();
                states.set(element, {state:'failed', definition:null, reactions:[]});
                return element;
            }
        });
    }
    function inserted(root) {
        if (!definitionCount || !connected(root)) return;
        for (const element of elements(root)) {
            const s = states.get(element);
            if (s && s.state === 'custom') {parserConnections.delete(element);callback(element, 'connectedCallback');}
            else tryUpgrade(element);
        }
    }
    function removed(root, wasConnected) {
        if (!wasConnected || !definitionCount) return;
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
        if (!definitionCount || !node) return null;
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
        function adopted(record){for(const element of record.elements){
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
        constructor(key) {
            const id=key===registryKey?0:nextRegistry++;
            registryStates.set(this,{id,scoped:key!==registryKey,definitions:new Map(),constructors:new Map(),pending:new Map(),documents:new Set(),defining:false});
            registries.set(id,this);
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
                rs.definitions.set(name,d); rs.constructors.set(constructor,d);definitionCount++;
                if(localName===name)parserNames.add(name);
                else{let names=parserBuiltins.get(localName);if(!names)parserBuiltins.set(localName,names=new Set());names.add(name);}
                if(host.domHooks)host.domHooks(0,true);
                if(d.formAssociated && host.domHooks)host.domHooks(2,true);
                for(const owner of rs.scoped?rs.documents:[document])
                    for(const element of elements(owner,localName))if(registryFor(element)===this)tryUpgrade(element);
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
            const type=get(root,'nodeType'),owner=type===9?root:get(root,'ownerDocument');
            if(!rs.scoped && (type===9 || registryFor(owner)!==this))throw fail('Global registry cannot initialize this root');
            return reactions(()=>{
                if((type===9 || type===11 && get(root,'shadowHost')) && registryFor(root)===null)setRegistry(root,this);
                function visit(node){
                    if(get(node,'nodeType')===1){
                        if(registryFor(node)===null)setRegistry(node,rs.scoped?thisRegistry:registry);
                        if(registryFor(node)===thisRegistry)tryUpgrade(node);
                    }
                    // initialize is inclusive tree order, not shadow-including.
                    for(const child of children(node))visit(child);
                }
                const thisRegistry=this;visit(root);
            });
        }
    }
    const registry = new CustomElementRegistry(registryKey);
    Object.defineProperty(CustomElementRegistry.prototype, Symbol.toStringTag, {value:'CustomElementRegistry',configurable:true});
    Object.defineProperty(globalThis, 'customElements', {get() { return registry; },enumerable:true,configurable:true});
    Object.defineProperty(globalThis, 'CustomElementRegistry', {value:CustomElementRegistry,writable:true,configurable:true});
    Object.defineProperty(Document.prototype,'customElementRegistry',{configurable:true,enumerable:true,get(){
        if(get(this,'nodeType')!==9)throw new TypeError('Document receiver required');return registryFor(this);
    }});
    function parserCreate(element,is=null,synchronous=true){
        if(is!==null)rawDom.customData(element,'is',string(is));
        const d=lookup(element);if(!d)return element;
        if(!synchronous){enqueue(element,{definition:d});return element;}
        const owner=get(element,'ownerDocument');
        const queue=[];scopes.push(queue);
        try{
            if(d.localName===d.name){
                // Autonomous synchronous creation is not an upgrade. Construct
                // a separate native element so a saved constructor `this` and
                // shadow tree keep their identity even when creation fails.
                const made=invokeConstructor(d),s=states.get(made);
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
    return {construct,create,reactions,inserted,removed,attributeChanged,upgradeTree,before,after,allowShadow,internalsInfo,
        formRefresh,formReset,registryFor,registryOption,setRegistry,parserCreate,parserFinish,parserInserted,parserCandidate,
        active:()=>definitionCount!==0,constructing:()=>constructionDepth>0};
})();
