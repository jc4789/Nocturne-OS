/* Browser bindings for Nocturne's native DOM. This never constructs a shadow DOM. */
(function (host) {
    'use strict';
    delete globalThis.__nocturne_host;
    const rawDom = host.dom;
    let customElementsReady = false;
    function dom(...args) {
        const op = args[0];
        const mutation = op === 'insert' || op === 'remove' || op === 'clone' || op === 'set' ||
            ((op === 'attr' || op === 'style') && args.length > 3);
        return mutation && customElementsReady ? customElementsBridge.reactions(() => rawDom(...args)) : rawDom(...args);
    }
    const apply = Reflect.apply;
    const eventSlice = Array.prototype.slice, mouseAssign = Object.assign;
    const listenerMap = new WeakMap(), inlineMap = new WeakMap();
    const handlerMap = new WeakMap();
    const globalHandlerTypes = new Set(('abort blur change click dblclick error focus focusin focusout input keydown keypress keyup load mousedown mouseenter mouseleave mousemove mouseout mouseover mouseup reset resize scroll submit wheel').split(' '));
    const windowHandlerTypes = new Set(['hashchange','popstate']);
    const state = new WeakMap();
    function list(a) {
        Object.defineProperty(a, 'item', {value(i) { return this[i] || null; }});
        return a;
    }
    function options(o) { return typeof o === 'boolean' ? {capture:o} : (o || {}); }
    function report(e) { host.log(2, String(e) + (e && e.stack ? '\n' + e.stack : '')); }
    class Event {
        constructor(type, init = {}) {
            this.type = String(type); this.bubbles = !!init.bubbles;
            this.cancelable = !!init.cancelable; this.composed = !!init.composed;
            this.target = null; this.currentTarget = null; this.eventPhase = 0;
            this.defaultPrevented = false; this.isTrusted = false;
            this.timeStamp = host.now(); this._stop = false; this._immediate = false;
            this._passive = false; this._dispatching = false; this._path = [];
        }
        preventDefault() { if (this.cancelable && !this._passive) this.defaultPrevented = true; }
        stopPropagation() { this._stop = true; }
        stopImmediatePropagation() { this._stop = this._immediate = true; }
        composedPath() { return this._path.slice(); }
        get cancelBubble() { return this._stop; }
        set cancelBubble(v) { if (v) this.stopPropagation(); }
        get returnValue() { return !this.defaultPrevented; }
        set returnValue(v) { if (!v) this.preventDefault(); }
    }
    Object.assign(Event, {NONE:0, CAPTURING_PHASE:1, AT_TARGET:2, BUBBLING_PHASE:3});
    Object.assign(Event.prototype, {NONE:0, CAPTURING_PHASE:1, AT_TARGET:2, BUBBLING_PHASE:3});
    class CustomEvent extends Event {
        constructor(t, o = {}) { super(t,o); this.detail = o.detail === undefined ? null : o.detail; }
    }
    class MouseEvent extends Event {
        constructor(t, o = {}) { super(t,o); mouseAssign(this, {clientX:0, clientY:0, pageX:0, pageY:0,
            button:0, buttons:0, relatedTarget:null, ctrlKey:false, shiftKey:false, altKey:false, metaKey:false}, o); }
    }
    class KeyboardEvent extends Event {
        constructor(t, o = {}) { super(t,o); Object.assign(this, {key:'', code:'', keyCode:0, which:0,
            ctrlKey:false, shiftKey:false, altKey:false, metaKey:false, repeat:false}, o); }
    }
    function removeListener(target, entry) {
        if (entry.removed) return;
        entry.removed = true;
        const a = listenerMap.get(target), i = a ? a.indexOf(entry) : -1;
        if (i >= 0) {
            a.splice(i, 1);
            if (!a.length) listenerMap.delete(target);
        }
        if (entry.signal) entry.signal.removeEventListener('abort', entry.abort);
        entry.signal = entry.abort = null;
    }
    class EventTarget {
        addEventListener(type, callback, init) {
            if (callback == null) return;
            if (typeof callback !== 'function' && typeof callback.handleEvent !== 'function') return;
            type = String(type); const o = options(init);
            handlerRecord(this,type); // parsed inline handlers precede later listeners
            let a = listenerMap.get(this);
            if (!a) listenerMap.set(this, a = []);
            if (a.some(x => x.type === type && x.callback === callback && x.capture === !!o.capture)) return;
            if (o.signal && o.signal.aborted) return;
            const x = {type, callback, capture:!!o.capture, once:!!o.once, passive:!!o.passive, removed:false, signal:null, abort:null};
            a.push(x);
            if (o.signal) {
                x.signal = o.signal; x.abort = () => removeListener(this, x);
                x.signal.addEventListener('abort', x.abort, {once:true});
            }
        }
        removeEventListener(type, callback, init) {
            const a = listenerMap.get(this), capture = !!options(init).capture;
            type = String(type);
            if (a) for (const x of a) if (x.type === type && x.callback === callback && x.capture === capture) {
                removeListener(this, x); break;
            }
        }
        dispatchEvent(event) {
            if (!(event instanceof Event) || !event.type || event._dispatching) throw new TypeError('Invalid event');
            return dispatch(this, event);
        }
    }
    function handlerTarget(target,type) {
        return (globalHandlerTypes.has(type) && (target===globalThis || target instanceof HTMLElement || target instanceof Document)) ||
            (windowHandlerTypes.has(type) && target===globalThis);
    }
    function activateHandler(target,type,r) {
        if(r.entry)return;
        let a=listenerMap.get(target);if(!a)listenerMap.set(target,a=[]);
        r.entry={type,capture:false,once:false,passive:false,removed:false,signal:null,abort:null,
            callback(event){const fn=handlerValue(target,type);if(typeof fn==='function' && fn.call(target,event)===false)event.preventDefault();}};
        a.push(r.entry);
    }
    function handlerRecord(target,type) {
        if(!handlerTarget(target,type))return null;
        let map=handlerMap.get(target);if(!map)handlerMap.set(target,map=new Map());
        let r=map.get(type);
        if(!r) {
            const text=target instanceof HTMLElement?target.getAttribute('on'+type):null;
            r={value:null,text,compiled:text===null,entry:null};map.set(type,r);
            if(text!==null)activateHandler(target,type,r);
        }
        return r;
    }
    function handlerValue(target,type) {
        const r=handlerRecord(target,type);if(!r)throw new TypeError('Illegal event handler receiver');
        if(!r.compiled) {
            r.compiled=true;
            try{r.value=host.inline(r.text);}catch(e){r.value=null;report(e);}
        }
        return r.value;
    }
    function setHandler(target,type,value,text) {
        const r=handlerRecord(target,type);if(!r)throw new TypeError('Illegal event handler receiver');
        r.value=typeof value==='function'?value:null;r.text=text;r.compiled=text===null;
        if(r.value!==null || text!==null)activateHandler(target,type,r);
        else if(r.entry){removeListener(target,r.entry);r.entry=null;}
    }
    function handlerAttribute(target,name,value) {
        const type=String(name).toLowerCase().slice(2);
        if(handlerTarget(target,type))setHandler(target,type,null,value);
    }
    function installHandlers(target,types) {
        for(const type of types)Object.defineProperty(target,'on'+type,{
            configurable:true,enumerable:true,
            get(){return handlerValue(this,type);},set(v){setHandler(this,type,v,null);}
        });
    }
    function invoke(target, event, capture) {
        event.currentTarget = target;
        handlerRecord(target,event.type);
        const a = listenerMap.get(target);
        if (a) for (const x of a.slice()) {
            if (x.removed || x.capture !== capture || x.type !== event.type) continue;
            if (x.once) removeListener(target, x);
            event._passive = x.passive;
            try {
                if (typeof x.callback === 'function') x.callback.call(target,event);
                else x.callback.handleEvent(event);
            } catch (e) { report(e); }
            event._passive = false;
            if (event._immediate) break;
        }
        if (!capture && !event._immediate && !handlerTarget(target,event.type)) {
            let fn = target['on' + event.type];
            if (fn === undefined && target instanceof Node && target.nodeType === 1) {
                const text = target.getAttribute('on' + event.type);
                if (text !== null) {
                    let cache = inlineMap.get(target);
                    if (!cache) inlineMap.set(target, cache = new Map());
                    let x = cache.get(event.type);
                    if (!x || x.text !== text) {
                        try { x = {text, fn:host.inline(text)}; cache.set(event.type,x); }
                        catch (e) { report(e); x = {text,fn:null}; cache.set(event.type,x); }
                    }
                    fn = x.fn;
                }
            }
            if (typeof fn === 'function') {
                try { if (fn.call(target,event) === false) event.preventDefault(); }
                catch (e) { report(e); }
            }
        }
    }
    function dispatch(target, event, snapshot) {
        event._dispatching = true; event.target = target; event._stop = event._immediate = false;
        const path = snapshot || [target];
        if (!snapshot && target instanceof Node) {
            for (let n = target.parentNode; n; n = n.parentNode) path.push(n);
            if (path[path.length-1] === document) path.push(globalThis);
        }
        event._path = apply(eventSlice,path,[]);
        for (let i = path.length-1; i > 0 && !event._stop; --i) {
            event.eventPhase = 1; invoke(path[i],event,true);
        }
        if (!event._stop) {
            event.eventPhase = 2; invoke(target,event,true);
            if (!event._immediate) invoke(target,event,false);
        }
        if (event.bubbles) for (let i = 1; i < path.length && !event._stop; ++i) {
            event.eventPhase = 3; invoke(path[i],event,false);
        }
        event.currentTarget = null; event.eventPhase = 0; event._dispatching = false; event._passive = false;
        return !event.defaultPrevented;
    }
    class Node extends EventTarget {
        constructor() { super(); throw new TypeError('Use document.createElement/createTextNode'); }
        get nodeType() { return dom('get',this,'nodeType'); }
        get nodeName() { return dom('get',this,'nodeName'); }
        get nodeValue() { return dom('get',this,'nodeValue'); }
        set nodeValue(v) { dom('set',this,'nodeValue',v == null ? '' : String(v)); }
        get parentNode() { return dom('get',this,'parentNode'); }
        get parentElement() { const p = this.parentNode; return p && p.nodeType === 1 ? p : null; }
        get firstChild() { return dom('get',this,'firstChild'); }
        get lastChild() { return dom('get',this,'lastChild'); }
        get nextSibling() { return dom('get',this,'nextSibling'); }
        get previousSibling() { return dom('get',this,'previousSibling'); }
        get childNodes() { return list(dom('get',this,'childNodes')); }
        get ownerDocument() { return this === document ? null : document; }
        get baseURI() { return dom('get',this,'baseURI'); }
        get isConnected() { let n=this; while(n.parentNode) n=n.parentNode; return n===document; }
        get textContent() { return dom('get',this,'textContent'); }
        set textContent(v) { dom('set',this,'textContent',v == null ? '' : String(v)); }
        appendChild(child) { dom('insert',this,child,null); return child; }
        insertBefore(child,before) { dom('insert',this,child,before); return child; }
        removeChild(child) { if (child.parentNode !== this) throw new Error('NotFoundError'); dom('remove',child); return child; }
        replaceChild(child,old) { if(child!==old){this.insertBefore(child,old);this.removeChild(old);}return old; }
        cloneNode(deep = false) { return dom('clone',this,!!deep); }
        isEqualNode(other = null) { return dom('equal',this,other); }
        isSameNode(other = null) { return dom('same',this,other); }
        contains(other) { for(let n=other;n;n=n.parentNode) if(n===this) return true; return false; }
        hasChildNodes() { return this.firstChild !== null; }
        getRootNode() { let n=this; while(n.parentNode) n=n.parentNode; return n; }
    }
    class Element extends Node {
        get children() { return list(this.childNodes.filter(n => n.nodeType === 1)); }
        get firstElementChild() { return this.children[0] || null; }
        get lastElementChild() { const a = this.children; return a[a.length-1] || null; }
        get nextElementSibling() { let n = this.nextSibling; while (n && n.nodeType !== 1) n=n.nextSibling; return n; }
        get previousElementSibling() { let n=this.previousSibling; while(n && n.nodeType !== 1) n=n.previousSibling; return n; }
        get childElementCount() { return this.children.length; }
        remove() { if (this.parentNode) this.parentNode.removeChild(this); }
        append(...nodes) { for (const n of nodes) this.appendChild(n instanceof Node ? n : document.createTextNode(String(n))); }
        prepend(...nodes) { const before=this.firstChild; for (const n of nodes) this.insertBefore(n instanceof Node ? n : document.createTextNode(String(n)),before); }
        replaceChildren(...nodes) { this.textContent=''; this.append(...nodes); }
        before(...nodes) { if(this.parentNode) for(const n of nodes) this.parentNode.insertBefore(n instanceof Node?n:document.createTextNode(String(n)),this); }
        after(...nodes) { if(this.parentNode) { const next=this.nextSibling; for(const n of nodes) this.parentNode.insertBefore(n instanceof Node?n:document.createTextNode(String(n)),next); } }
        get tagName() { return this.nodeType === 1 ? this.nodeName : undefined; }
        get localName() { return dom('get',this,'localName'); }
        get namespaceURI() { return dom('get',this,'namespaceURI'); }
        get id() { return this.getAttribute('id') || ''; }
        set id(v) { this.setAttribute('id',v); }
        get className() { return this.getAttribute('class') || ''; }
        set className(v) { this.setAttribute('class',v); }
        get classList() { return new DOMTokenList(this); }
        getAttribute(name) { return dom('attr',this,String(name)); }
        setAttribute(name,value) { dom('attr',this,String(name),String(value)); }
        removeAttribute(name) { dom('attr',this,String(name),null); }
        hasAttribute(name) { return this.getAttribute(name) !== null; }
        toggleAttribute(name,force) { let on=this.hasAttribute(name); on=force===undefined?!on:!!force; if(on)this.setAttribute(name,'');else this.removeAttribute(name); return on; }
        getAttributeNames() { return dom('get',this,'attributeNames'); }
        get innerHTML() { return dom('get',this,'innerHTML'); }
        set innerHTML(v) { dom('set',this,'innerHTML',String(v)); }
        get outerHTML() { return dom('get',this,'outerHTML'); }
        querySelector(selector) { return dom('query',this,String(selector),true); }
        querySelectorAll(selector) { return list(dom('query',this,String(selector),false)); }
        matches(selector) { return dom('matches',this,String(selector)); }
        closest(selector) { for(let n=this;n&&n.nodeType===1;n=n.parentElement) if(n.matches(selector))return n; return null; }
        getElementsByTagName(name) { return this.querySelectorAll(name==='*'?'*':String(name)); }
        getElementsByClassName(names) { return this.querySelectorAll(String(names).trim().split(/\s+/).map(x=>'.'+CSS.escape(x)).join('')); }
        getBoundingClientRect() { return dom('rect',this); }
        get clientWidth() { return dom('geometry',this,'clientWidth'); }
        get clientHeight() { return dom('geometry',this,'clientHeight'); }
        get clientLeft() { return dom('geometry',this,'clientLeft'); }
        get clientTop() { return dom('geometry',this,'clientTop'); }
        get style() { let s=state.get(this);if(!s)state.set(this,s={});return s.style||(s.style=new StyleDeclaration(this)); }
        get dataset() { const n=this;return new Proxy({}, {get(_,k){return n.getAttribute('data-'+String(k).replace(/[A-Z]/g,c=>'-'+c.toLowerCase()));},set(_,k,v){n.setAttribute('data-'+String(k).replace(/[A-Z]/g,c=>'-'+c.toLowerCase()),v);return true;}}); }
    }
    class HTMLElement extends Element {
        constructor() { return customElementsBridge.construct(new.target); }
        get offsetParent() { return dom('geometry',this,'offsetParent'); }
        get offsetLeft() { return dom('geometry',this,'offsetLeft'); }
        get offsetTop() { return dom('geometry',this,'offsetTop'); }
        get offsetWidth() { return dom('geometry',this,'offsetWidth'); }
        get offsetHeight() { return dom('geometry',this,'offsetHeight'); }
        get text() { return this.textContent; }
        set text(v) { this.textContent=v; }
        get async() { return dom('get',this,'async'); }
        set async(v) { dom('set',this,'async',!!v); }
        get defer() { return this.hasAttribute('defer'); }
        set defer(v) { this.toggleAttribute('defer',!!v); }
        get value() { return dom('get',this,'value'); }
        set value(v) { dom('set',this,'value',String(v)); }
        get checked() { return dom('get',this,'checked'); }
        set checked(v) { dom('set',this,'checked',!!v); }
        get selectedIndex() { return dom('get',this,'selectedIndex'); }
        set selectedIndex(v) { dom('set',this,'selectedIndex',Number(v)); }
        get selected() { return dom('get',this,'selected'); }
        set selected(v) { dom('set',this,'selected',!!v); }
        get disabled() { return this.hasAttribute('disabled'); }
        set disabled(v) { this.toggleAttribute('disabled',!!v); }
        focus() { dom('focus',this); }
        blur() { dom('focus',null); }
        click() { host.click(this); }
        submit() { dom('submit',this); }
        requestSubmit(submitter) { const e=new Event('submit',{bubbles:true,cancelable:true});e.submitter=submitter||null;if(this.dispatchEvent(e))dom('submit',submitter||this); }
        get elements() { return list(Array.from(this.querySelectorAll('input,select,textarea,button'))); }
    }
    // A distinct native interface, not an alias or an instanceof override.
    // Nested browsing contexts/navigation are not implemented by this class.
    class HTMLIFrameElement extends HTMLElement {
        constructor() { throw new TypeError('Illegal HTMLIFrameElement constructor'); }
    }
    class HTMLImageElement extends HTMLElement {
        constructor() { throw new TypeError('Illegal HTMLImageElement constructor'); }
        get width() { return dom('get',this,'imageWidth'); }
        set width(v) { dom('set',this,'imageWidth',v); }
        get height() { return dom('get',this,'imageHeight'); }
        set height(v) { dom('set',this,'imageHeight',v); }
        get naturalWidth() { return dom('get',this,'imageNaturalWidth'); }
        get naturalHeight() { return dom('get',this,'imageNaturalHeight'); }
        get complete() { return dom('get',this,'imageComplete'); }
        get currentSrc() { return dom('get',this,'imageCurrentSrc'); }
        get src() {
            dom('get',this,'imageBrand'); const value=dom('attr',this,'src');
            if(value===null)return '';try{return new URL(value,document.baseURI).href;}catch(_){return value;}
        }
        set src(v) { dom('get',this,'imageBrand');dom('attr',this,'src',String(v)); }
        get alt() { dom('get',this,'imageBrand');return dom('attr',this,'alt')||''; }
        set alt(v) { dom('get',this,'imageBrand');dom('attr',this,'alt',String(v)); }
        get srcset() { dom('get',this,'imageBrand');return dom('attr',this,'srcset')||''; }
        set srcset(v) { dom('get',this,'imageBrand');dom('attr',this,'srcset',String(v)); }
        get sizes() { dom('get',this,'imageBrand');return dom('attr',this,'sizes')||''; }
        set sizes(v) { dom('get',this,'imageBrand');dom('attr',this,'sizes',String(v)); }
        decode() { return Promise.resolve().then(()=>dom('imageDecode',this)); }
    }
    function Image() {
        const image=dom('create',null,1,'img','');
        if(arguments.length>0)dom('set',image,'imageWidth',arguments[0]);
        if(arguments.length>1)dom('set',image,'imageHeight',arguments[1]);
        return image;
    }
    Object.defineProperty(Image,'prototype',{value:HTMLImageElement.prototype,writable:false});
    class HTMLInputElement extends HTMLElement {
        constructor(){throw new TypeError('Illegal HTMLInputElement constructor');}
        get form(){return dom('get',this,'form:input');}
    }
    class HTMLButtonElement extends HTMLElement {
        constructor(){throw new TypeError('Illegal HTMLButtonElement constructor');}
        get form(){return dom('get',this,'form:button');}
    }
    class HTMLSelectElement extends HTMLElement {
        constructor(){throw new TypeError('Illegal HTMLSelectElement constructor');}
        get form(){return dom('get',this,'form:select');}
    }
    class HTMLTextAreaElement extends HTMLElement {
        constructor(){throw new TypeError('Illegal HTMLTextAreaElement constructor');}
        get form(){return dom('get',this,'form:textarea');}
    }
    class HTMLFieldSetElement extends HTMLElement {
        constructor(){throw new TypeError('Illegal HTMLFieldSetElement constructor');}
        get form(){return dom('get',this,'form:fieldset');}
    }
    class HTMLObjectElement extends HTMLElement {
        constructor(){throw new TypeError('Illegal HTMLObjectElement constructor');}
        get form(){return dom('get',this,'form:object');}
    }
    class HTMLOutputElement extends HTMLElement {
        constructor(){throw new TypeError('Illegal HTMLOutputElement constructor');}
        get form(){return dom('get',this,'form:output');}
    }
    class HTMLOptionElement extends HTMLElement {
        constructor(){throw new TypeError('Illegal HTMLOptionElement constructor');}
        get form(){return dom('get',this,'form:option');}
    }
    Object.assign(Node, {ELEMENT_NODE:1,TEXT_NODE:3,COMMENT_NODE:8,DOCUMENT_NODE:9,DOCUMENT_FRAGMENT_NODE:11});
    Object.assign(Node.prototype, {ELEMENT_NODE:1,TEXT_NODE:3,COMMENT_NODE:8,DOCUMENT_NODE:9,DOCUMENT_FRAGMENT_NODE:11});
    class Document extends Node {}
    class CharacterData extends Node {}
    class Text extends CharacterData {}
    class Comment extends CharacterData {}
    class DocumentFragment extends Node {}
    function copyElementMembers(proto, names) {
        for (const name of names) Object.defineProperty(proto, name, Object.getOwnPropertyDescriptor(Element.prototype, name));
    }
    // ParentNode is shared, but attributes and matching belong only to elements.
    const parentMembers = ['children','firstElementChild','lastElementChild','childElementCount',
        'append','prepend','replaceChildren','querySelector','querySelectorAll'];
    copyElementMembers(Document.prototype, parentMembers.concat(['getElementsByTagName','getElementsByClassName']));
    copyElementMembers(DocumentFragment.prototype, parentMembers);
    copyElementMembers(CharacterData.prototype, ['nextElementSibling','previousElementSibling','remove','before','after']);
    const document=host.document;
    Object.setPrototypeOf(document,Document.prototype);
    Object.defineProperties(document, {
        documentElement:{get(){return dom('get',document,'documentElement');}},
        head:{get(){return dom('get',document,'head');}}, body:{get(){return dom('get',document,'body');}},
        title:{get(){return dom('get',document,'title');},set(v){dom('set',document,'title',String(v));}},
        URL:{get(){return host.url();}}, documentURI:{get(){return host.url();}},
        baseURI:{get(){return dom('get',document,'baseURI');}},
        readyState:{get(){return host.ready();}}, activeElement:{get(){return dom('get',document,'activeElement');}},
        defaultView:{value:globalThis}, currentScript:{get(){return host.current();}}
    });
    document.getElementById=id=>dom('id',document,String(id));
    document.getElementsByName=name=>document.querySelectorAll('[name="'+CSS.escape(String(name))+'"]');
    document.createElement=(name,options)=>customElementsBridge.create(name,options);
    document.createElementNS=(ns,name,options)=>{if(ns==='http://www.w3.org/1999/xhtml')return customElementsBridge.create(name,options,true);if(ns!=='http://www.w3.org/2000/svg')throw new DOMException('Unsupported namespace','NotSupportedError');return dom('create',null,1,String(name),'',true);};
    document.createTextNode=text=>dom('create',null,3,'#text',String(text));
    document.createComment=text=>dom('create',null,8,'#comment',String(text));
    document.createDocumentFragment=()=>dom('create',null,11,'#document-fragment','');
    document.importNode=(node,deep)=>node.cloneNode(!!deep);
    document.write=(...s)=>host.write(s.join(''));
    document.writeln=(...s)=>host.write(s.join('')+'\n');
    document.createEvent=()=>new Event('');
    class DOMTokenList {
        constructor(node){this.node=node;}
        _tokens(){return (this.node.className.match(/\S+/g)||[]).filter((x,i,a)=>a.indexOf(x)===i);}
        _check(t){t=String(t);if(!t||/\s/.test(t))throw new Error('InvalidCharacterError');return t;}
        get length(){return this._tokens().length;}
        item(i){return this._tokens()[i]||null;}
        contains(t){return this._tokens().includes(this._check(t));}
        add(...ts){const a=this._tokens();for(let t of ts){t=this._check(t);if(!a.includes(t))a.push(t);}this.node.className=a.join(' ');}
        remove(...ts){ts=ts.map(t=>this._check(t));this.node.className=this._tokens().filter(t=>!ts.includes(t)).join(' ');}
        toggle(t,force){t=this._check(t);const on=force===undefined?!this.contains(t):!!force;if(on)this.add(t);else this.remove(t);return on;}
        replace(a,b){a=this._check(a);b=this._check(b);if(!this.contains(a))return false;this.remove(a);this.add(b);return true;}
        get value(){return this.node.className;}set value(v){this.node.className=v;}
        [Symbol.iterator](){return this._tokens()[Symbol.iterator]();}toString(){return this.value;}
    }
    function cssName(k){return String(k).replace(/[A-Z]/g,c=>'-'+c.toLowerCase());}
    class StyleDeclaration {
        constructor(node){this.node=node;return new Proxy(this,{get(t,k){if(k in t||typeof k==='symbol')return Reflect.get(t,k);return t.getPropertyValue(cssName(k));},set(t,k,v){if(k in t)return Reflect.set(t,k,v);t.setProperty(cssName(k),v);return true;}});}
        get cssText(){return this.node.getAttribute('style')||'';}set cssText(v){this.node.setAttribute('style',String(v));}
        getPropertyValue(name){return dom('style',this.node,String(name));}
        getPropertyPriority(name){return /!important\s*$/i.test(this.getPropertyValue(name))?'important':'';}
        setProperty(name,value,priority=''){dom('style',this.node,String(name),String(value),String(priority));}
        removeProperty(name){const old=this.getPropertyValue(name);this.setProperty(name,'');return old;}
        get length(){return this.cssText.split(';').filter(x=>x.includes(':')).length;}
        item(i){const a=this.cssText.split(';').filter(x=>x.includes(':'));return a[i]?a[i].split(':')[0].trim():'';}
    }
    for(const name of ['name','type','src','href','rel','action','method','placeholder','lang','dir','title'])
        Object.defineProperty(HTMLElement.prototype,name,{configurable:true,get(){const s=this.getAttribute(name)||'';return ['src','href','action'].includes(name)&&s?host.resolve(s):s;},set(v){this.setAttribute(name,v);}});
    const CSS={escape(s){return Array.from(String(s)).map((c,i)=>/[a-zA-Z_\-]/.test(c)||(/[0-9]/.test(c)&&i>0)?c:'\\'+c.codePointAt(0).toString(16)+' ').join('');}};
    class Headers {
        constructor(init){this._h=new Map();if(init instanceof Headers)for(const [k,v]of init)this.append(k,v);else if(Array.isArray(init))for(const [k,v]of init)this.append(k,v);else if(init)for(const k of Object.keys(init))this.append(k,init[k]);}
        _name(k){k=String(k).toLowerCase();if(!/^[!#$%&'*+.^_`|~0-9a-z-]+$/.test(k))throw new TypeError('Invalid header name');return k;}
        _value(v){v=String(v).trim();if(/[\r\n\0]/.test(v))throw new TypeError('Invalid header value');return v;}
        append(k,v){k=this._name(k);v=this._value(v);this._h.set(k,this._h.has(k)?this._h.get(k)+', '+v:v);}
        set(k,v){this._h.set(this._name(k),this._value(v));}
        get(k){return this._h.get(this._name(k))??null;}
        has(k){return this._h.has(this._name(k));}delete(k){this._h.delete(this._name(k));}
        entries(){return this._h.entries();}keys(){return this._h.keys();}values(){return this._h.values();}
        forEach(fn,thisArg){for(const [k,v]of this)fn.call(thisArg,v,k,this);}[Symbol.iterator](){return this.entries();}
    }
    class Response {
        constructor(body='',init={}){this.status=init.status??200;this.statusText=init.statusText||'';this.url=init.url||'';this.redirected=!!init.redirected;this.ok=this.status>=200&&this.status<300;this.headers=new Headers(init.headers);this.bodyUsed=false;this._text=String(body);this._bytes=init.bytes||host.encode(this._text);this.type='basic';}
        _use(){if(this.bodyUsed)throw new TypeError('Body already consumed');this.bodyUsed=true;}
        text(){return Promise.resolve().then(()=>{this._use();return this._text;});}
        json(){return this.text().then(t=>JSON.parse(t));}
        arrayBuffer(){return Promise.resolve().then(()=>{this._use();return this._bytes.slice(0);});}
        clone(){if(this.bodyUsed)throw new TypeError('Body already consumed');return new Response(this._text,{status:this.status,statusText:this.statusText,url:this.url,headers:this.headers,bytes:this._bytes.slice(0),redirected:this.redirected});}
    }
    class DOMException extends Error {constructor(message='',name='Error'){super(message);this.name=String(name);}}
    class AbortSignal extends EventTarget {
        constructor(){super();this.aborted=false;this.reason=undefined;}
        throwIfAborted(){if(this.aborted)throw this.reason;}
        static abort(reason){const c=new AbortController();c.abort(reason);return c.signal;}
        static timeout(ms){const c=new AbortController();setTimeout(()=>c.abort(new DOMException('Timed out','TimeoutError')),ms);return c.signal;}
    }
    class AbortController {
        constructor(){this.signal=new AbortSignal();}
        abort(reason=new DOMException('The operation was aborted','AbortError')){if(this.signal.aborted)return;this.signal.aborted=true;this.signal.reason=reason;this.signal.dispatchEvent(new Event('abort'));}
    }
    function fetch(input,init={}) {
        const url=typeof input==='string'?input:String(input.url??input),method=String(init.method||'GET').toUpperCase();
        const headers=new Headers(init.headers),signal=init.signal;
        if(signal&&signal.aborted)return Promise.reject(signal.reason);
        const credentials=init.credentials===undefined?'same-origin':String(init.credentials);
        if(!['omit','same-origin','include'].includes(credentials))return Promise.reject(new TypeError('Invalid credentials mode'));
        if(init.mode && !['cors','same-origin'].includes(init.mode))return Promise.reject(new TypeError('Unsupported fetch mode'));
        if(!['GET','POST'].includes(method))return Promise.reject(new TypeError('Only GET and POST are supported'));
        let body=init.body==null?'':String(init.body);
        if(method==='GET'&&body)return Promise.reject(new TypeError('GET cannot have a body'));
        if(method==='POST'&&init.body!=null&&!headers.has('content-type'))headers.set('content-type','text/plain;charset=UTF-8');
        let raw='';for(const [k,v]of headers)raw+=k+': '+v+'\r\n';
        let pair;try{pair=host.fetch(url,method,raw,body,init.mode==='same-origin',['omit','same-origin','include'].indexOf(credentials));}catch(e){return Promise.reject(e);}
        let abort;
        if(signal){abort=()=>host.cancel(pair.id);signal.addEventListener('abort',abort,{once:true});}
        return pair.promise.finally(()=>{if(signal)signal.removeEventListener('abort',abort);});
    }
    const location={assign(url){host.navigate(String(url),0);},replace(url){host.navigate(String(url),2);},reload(){host.navigate(host.url(),1);},toString(){return this.href;}};
    Object.defineProperties(location,{href:{get(){return host.url();},set(v){host.navigate(String(v));}}});
    document.location=location;
    const console={};for(const [i,name]of ['log','warn','error','info','debug'].entries())console[name]=(...args)=>host.log(i===2?2:i===1?1:0,...args);
    console.assert=(yes,...args)=>{if(!yes)console.error('Assertion failed:',...args);};
    const timer=(kind,fn,ms,args)=>{const callback=typeof fn==='function'?fn:new Function(String(fn));return host.timer(kind,kind===2?callback:function(){return apply(callback,this,args);},Number(ms)||0,[]);};
    function setTimeout(fn,ms,...args){return timer(0,fn,ms,args);}function setInterval(fn,ms,...args){return timer(1,fn,ms,args);}
    function clearTimeout(id){host.clear(Number(id));}function clearInterval(id){host.clear(Number(id));}
    function requestAnimationFrame(fn){if(typeof fn!=='function')throw new TypeError('Expected callback');return timer(2,fn,16,[]);}
    function cancelAnimationFrame(id){host.clear(Number(id));}
    function queueMicrotask(fn){if(typeof fn!=='function')throw new TypeError('Expected callback');Promise.resolve().then(fn).catch(report);}
    const navigator={userAgent:'Nocturne/1.0 QuickJS',platform:'Nocturne',language:'en-US',languages:['en-US'],onLine:true};
    Object.assign(globalThis,{document,console,navigator,Node,Element,HTMLElement,HTMLIFrameElement,HTMLImageElement,Image,
        HTMLInputElement,HTMLButtonElement,HTMLSelectElement,HTMLTextAreaElement,HTMLFieldSetElement,HTMLObjectElement,HTMLOutputElement,HTMLOptionElement,
        Document,CharacterData,Text,Comment,DocumentFragment,
        Event,CustomEvent,MouseEvent,KeyboardEvent,EventTarget,DOMTokenList,CSS,Headers,Response,DOMException,AbortController,AbortSignal,
        fetch,setTimeout,setInterval,clearTimeout,clearInterval,requestAnimationFrame,cancelAnimationFrame,queueMicrotask,
        performance:{now:()=>host.now()},getComputedStyle:n=>new Proxy({getPropertyValue:k=>dom('computed',n,String(k))},{get(t,k){return k in t?t[k]:t.getPropertyValue(cssName(k));}})});
    Object.defineProperty(globalThis,'location',{configurable:true,get(){return location;},set(v){host.navigate(String(v));}});
    globalThis.window=globalThis;globalThis.self=globalThis;globalThis.top=globalThis;globalThis.parent=globalThis;
    for(const n of ['addEventListener','removeEventListener','dispatchEvent']) {
        const method = EventTarget.prototype[n];
        globalThis[n] = function(...args) { return apply(method,this == null ? globalThis : this,args); };
    }
    installHandlers(HTMLElement.prototype,globalHandlerTypes);
    installHandlers(Document.prototype,globalHandlerTypes);
    installHandlers(globalThis,globalHandlerTypes);
    installHandlers(globalThis,windowHandlerTypes);
    Object.defineProperties(globalThis,{innerWidth:{get(){return dom('viewport',null,0);}},innerHeight:{get(){return dom('viewport',null,1);}}});
    Object.defineProperty(Document.prototype,'cookie',{get(){return host.cookie();},set(v){host.cookie(String(v));}});
    Object.defineProperties(globalThis,{
        scrollX:{get(){return host.scroll(0);}},pageXOffset:{get(){return host.scroll(0);}},
        scrollY:{get(){return host.scroll(1);}},pageYOffset:{get(){return host.scroll(1);}}
    });
    globalThis.scrollTo=globalThis.scroll=(x,y)=>{
        if(x && typeof x==='object') host.scroll(x.left===undefined?host.scroll(0):Number(x.left),x.top===undefined?host.scroll(1):Number(x.top));
        else host.scroll(Number(x)||0,Number(y)||0);
    };
    globalThis.scrollBy=(x,y)=>{
        if(x && typeof x==='object') host.scroll(host.scroll(0)+(Number(x.left)||0),host.scroll(1)+(Number(x.top)||0));
        else host.scroll(host.scroll(0)+(Number(x)||0),host.scroll(1)+(Number(y)||0));
    };
    let historyEvent;
    /* @include js_encoding.js */
    /* @include js_url.js */
    /* @include js_intl.js */
    /* @include js_crypto.js */
    /* @include js_clone.js */
    /* @include js_history.js */
    /* @include js_custom_elements.js */
    /* @include js_media.js */
    /* @include js_observers.js */
    customElementsReady = true;
    /* Private native-input state, never reachable from page JS. C supplies the
       hit target's complete ancestry BEFORE any event handler can change it.
       Keep the previous snapshot, including ancestors of now-detached nodes. */
    let hoverPath = [];
    function hover(path, init) {
        if (path.length && path[path.length-1] === document) path[path.length] = globalThis;
        const previous = hoverPath, from = previous[0] || null, to = path[0] || null;
        const tasks = [];
        function contains(a,n) { for (let i=0;i<a.length;i++) if(a[i]===n) return true; return false; }
        function queue(type, ancestry, index, related, boundary) {
            const e = new MouseEvent(type, mouseAssign({},init,{
                relatedTarget:related,bubbles:!boundary,cancelable:!boundary,composed:!boundary
            }));
            e.isTrusted = true;
            tasks[tasks.length] = {event:e,path:apply(eventSlice,ancestry,[index])};
        }
        if (from !== to && from) queue('mouseout',previous,0,to,false);
        for (let i=0;i<previous.length;i++) {
            const node=previous[i];
            if(node!==globalThis && rawDom('get',node,'nodeType')===1 && !contains(path,node))
                queue('mouseleave',previous,i,to,true);
        }
        if (from !== to && to) queue('mouseover',path,0,from,false);
        for (let i=path.length-1;i>=0;i--) {
            const node=path[i];
            if(node!==globalThis && rawDom('get',node,'nodeType')===1 && !contains(previous,node))
                queue('mouseenter',path,i,from,true);
        }
        if (to) queue('mousemove',path,0,null,false);
        hoverPath = path;
        let i=0;
        /* Return to native code between events: each native dispatch needs its
           microtask checkpoint, without rebuilding the snapshotted paths. */
        return function nextHoverEvent() {
            if(i===tasks.length) return false;
            const task=tasks[i++]; dispatch(task.path[0],task.event,task.path);
            return true;
        };
    }
    return {
        observerFrame(){observerBridge.frame();},
        eventHandlerAttribute:handlerAttribute,
        imageError(){return new DOMException('The image request changed or could not be decoded','EncodingError');},
        hover,
        customElementBefore(...args){return customElementsBridge.before(...args);},
        customElementAfter(token,result){customElementsBridge.after(token,result);},
        customElementScan(){customElementsBridge.upgradeTree(document);},
        historyEvent(oldURL,popstate){historyEvent(oldURL,popstate);},
        mediaChanged(){mediaBridge.changed();},
        nodeProtos:[Node.prototype,Document.prototype,Element.prototype,HTMLElement.prototype,
            Text.prototype,Comment.prototype,DocumentFragment.prototype,HTMLIFrameElement.prototype,HTMLImageElement.prototype,
            HTMLInputElement.prototype,HTMLButtonElement.prototype,HTMLSelectElement.prototype,HTMLTextAreaElement.prototype,
            HTMLFieldSetElement.prototype,HTMLObjectElement.prototype,HTMLOutputElement.prototype,HTMLOptionElement.prototype],
        dispatch(target,type,init){const C=/^(key)/.test(type)?KeyboardEvent:/^(mouse|click|dblclick)/.test(type)?MouseEvent:Event;const e=new C(type,init);Object.assign(e,init);e.isTrusted=true;return dispatch(target===null?globalThis:target,e);},
        response(status,url,raw,text,bytes,redirected){const headers=new Headers();for(const line of raw.split(/\r?\n/)){const i=line.indexOf(':');if(i>0){const k=line.slice(0,i);if(!/^set-cookie2?$/i.test(k))headers.append(k,line.slice(i+1));}}return new Response(text,{status,url,headers,bytes,redirected});},
        reject(message,abort){return abort?new DOMException(message,'AbortError'):new TypeError(message);}
    };
})(__nocturne_host);
