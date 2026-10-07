/* Browser bindings for Nocturne's sole native DOM, including native shadow trees. */
(function (host) {
    'use strict';
    delete globalThis.__nocturne_host;
    const rawDom = host.dom;
    let customElementsReady = false;
    function dom(...args) {
        const op = args[0];
        const mutation = op === 'shadowAttach' || op === 'slotAssign' || op === 'insert' || op === 'remove' || op === 'adopt' || op === 'clone' || op === 'set' || op === 'attrSetNode' || op === 'attrRemoveNode' ||
            (op === 'attrNS' && args.length > 4) ||
            ((op === 'attr' || op === 'style') && args.length > 3);
        return mutation && customElementsReady ? customElementsBridge.reactions(() => rawDom(...args)) : rawDom(...args);
    }
    // IDL reflection always addresses the null namespace; getAttribute remains
    // a qualified-name lookup and may find an explicitly namespaced attribute.
    function reflectedAttr(node,name,...value) {
        return value.length?dom('attrNS',node,null,name,value[0]):rawDom('attrNS',node,null,name);
    }
    const apply = Reflect.apply;
    const eventSlice = Array.prototype.slice, mouseAssign = Object.assign;
    const listenerMap = new WeakMap(), inlineMap = new WeakMap();
    const handlerMap = new WeakMap();
    const eventPaths = new WeakMap();
    let xhrHandlerTarget = () => false;
    let messageHandlerTarget = () => false;
    let svgHandlerTarget = () => false;
    let abortHandlerTarget = () => false;
    let shadowHandlerTarget = () => false;
    let avmediaHandlerTarget = () => false;
    const globalHandlerTypes = new Set(('abort blur change click dblclick error focus focusin focusout input invalid keydown keypress keyup load mousedown mouseenter mouseleave mousemove mouseout mouseover mouseup reset resize scroll select slotchange submit toggle wheel').split(' '));
    const windowHandlerTypes = new Set(['hashchange','popstate','message','messageerror']);
    const state = new WeakMap();
    /* @include js_collections.js */
    function list(a) { return collectionBridge.list(a); }
    function options(o) { return typeof o === 'boolean' ? {capture:o} : (o || {}); }
    function report(e) { host.log(2, String(e) + (e && e.stack ? '\n' + e.stack : '')); }
    class Event {
        constructor(type, init = {}) {
            this.type = String(type); this.bubbles = !!init.bubbles;
            this.cancelable = !!init.cancelable; this.composed = !!init.composed;
            this.target = null; this.currentTarget = null; this.eventPhase = 0;
            this.defaultPrevented = false; this.isTrusted = false;
            this.timeStamp = host.now(); this._stop = false; this._immediate = false;
            this._passive = false; this._dispatching = false; this._initialized = true;
        }
        initEvent(type,bubbles=false,cancelable=false) {
            if(!arguments.length)throw new TypeError('initEvent requires type');
            if(this._dispatching)return;
            this.type=String(type);this.bubbles=!!bubbles;this.cancelable=!!cancelable;
            this._stop=this._immediate=this.defaultPrevented=false;this.isTrusted=false;this.target=null;this._initialized=true;
        }
        preventDefault() { if (this.cancelable && !this._passive) this.defaultPrevented = true; }
        stopPropagation() { this._stop = true; }
        stopImmediatePropagation() { this._stop = this._immediate = true; }
        composedPath() { return apply(eventSlice,eventPaths.get(this)||[],[]); }
        get cancelBubble() { return this._stop; }
        set cancelBubble(v) { if (v) this.stopPropagation(); }
        get returnValue() { return !this.defaultPrevented; }
        set returnValue(v) { if (!v) this.preventDefault(); }
    }
    Object.assign(Event, {NONE:0, CAPTURING_PHASE:1, AT_TARGET:2, BUBBLING_PHASE:3});
    Object.assign(Event.prototype, {NONE:0, CAPTURING_PHASE:1, AT_TARGET:2, BUBBLING_PHASE:3});
    class CustomEvent extends Event {
        constructor(t, o = {}) { super(t,o); this.detail = o.detail === undefined ? null : o.detail; }
        initCustomEvent(type,bubbles=false,cancelable=false,detail=null) {
            if(!arguments.length)throw new TypeError('initCustomEvent requires type');
            if(this._dispatching)return;
            this.initEvent(type,bubbles,cancelable);this.detail=detail;
        }
    }
    class UIEvent extends Event {
        constructor(t,o={}){super(t,o);this.view=o.view??null;this.detail=(+(o.detail??0))>>0;}
        initUIEvent(type,bubbles=false,cancelable=false,view=null,detail=0){
            if(!arguments.length)throw new TypeError('initUIEvent requires type');
            if(this._dispatching)return;
            Event.prototype.initEvent.call(this,type,bubbles,cancelable);this.view=view;this.detail=(+detail)>>0;
        }
    }
    class MouseEvent extends UIEvent {
        constructor(t, o = {}) { super(t,o); mouseAssign(this, {clientX:0, clientY:0, pageX:0, pageY:0,
            screenX:0,screenY:0,button:0, buttons:0, relatedTarget:null, ctrlKey:false, shiftKey:false, altKey:false, metaKey:false}, o); }
        initMouseEvent(type,bubbles=false,cancelable=false,view=null,detail=0,screenX=0,screenY=0,clientX=0,clientY=0,ctrlKey=false,altKey=false,shiftKey=false,metaKey=false,button=0,relatedTarget=null){
            if(!arguments.length)throw new TypeError('initMouseEvent requires type');
            if(this._dispatching)return;
            if(relatedTarget!==null && !(relatedTarget instanceof EventTarget) && relatedTarget!==globalThis)throw new TypeError('Expected an EventTarget');
            UIEvent.prototype.initUIEvent.call(this,type,bubbles,cancelable,view,detail);
            mouseAssign(this,{screenX:(+screenX)>>0,screenY:(+screenY)>>0,clientX:(+clientX)>>0,clientY:(+clientY)>>0,
                ctrlKey:!!ctrlKey,altKey:!!altKey,shiftKey:!!shiftKey,metaKey:!!metaKey,button:((+button)<<16)>>16,relatedTarget});
            this.pageX=this.clientX+host.scroll(0);this.pageY=this.clientY+host.scroll(1);
        }
        getModifierState(key){return ({Control:this.ctrlKey,Alt:this.altKey,Shift:this.shiftKey,Meta:this.metaKey})[String(key)]||false;}
    }
    class KeyboardEvent extends UIEvent {
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
            if (!(event instanceof Event)) throw new TypeError('Invalid event');
            if (!event._initialized || event._dispatching) throw new DOMException('Uninitialized or dispatching event','InvalidStateError');
            event.isTrusted=false;
            return dispatch(this, event);
        }
    }
    function handlerTarget(target,type) {
        return (globalHandlerTypes.has(type) && (target===globalThis || target instanceof HTMLElement || target instanceof Document || svgHandlerTarget(target))) ||
            (windowHandlerTypes.has(type) && target===globalThis) || xhrHandlerTarget(target,type) || messageHandlerTarget(target,type) || abortHandlerTarget(target,type) || shadowHandlerTarget(target,type) || avmediaHandlerTarget(target,type);
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
            const text=(target instanceof HTMLElement || svgHandlerTarget(target))?reflectedAttr(target,'on'+type):null;
            r={value:null,text,compiled:text===null,entry:null};map.set(type,r);
            if(text!==null)activateHandler(target,type,r);
        }
        return r;
    }
    function handlerValue(target,type) {
        const r=handlerRecord(target,type);if(!r)throw new TypeError('Illegal event handler receiver');
        if(!r.compiled) {
            if(target instanceof Node && !rawDom('get',target,'scripting'))return null;
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
                const text = reflectedAttr(target,'on' + event.type);
                if (text !== null && rawDom('get',target,'scripting')) {
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
        const plan=snapshot && snapshot.entries?snapshot:shadowBridge.path(target,event,snapshot),entries=plan.entries;
        const hasRelated='relatedTarget' in event;
        function visit(entry,capture,phase){
            event.target=entry.target;if(hasRelated)event.relatedTarget=entry.related;
            eventPaths.set(event,entry.visible);event.eventPhase=phase;invoke(entry.node,event,capture);
        }
        for(let i=entries.length-1;i>0 && !event._stop;i--)visit(entries[i],true,entries[i].atTarget?2:1);
        if(entries.length && !event._stop){
            /* Capture and noncapture at AT_TARGET are distinct invocations.
               stopPropagation keeps this capture listener list running, but
               prevents the following noncapture invocation. */
            visit(entries[0],true,2);if(!event._stop)visit(entries[0],false,2);
        }
        for(let i=1;i<entries.length && !event._stop;i++)
            if(event.bubbles || entries[i].atTarget)visit(entries[i],false,entries[i].atTarget?2:3);
        event.target=plan.finalTarget;if(hasRelated)event.relatedTarget=plan.finalRelated;
        eventPaths.delete(event);
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
        get childNodes() { return collectionBridge.children(this,false); }
        get ownerDocument() { return dom('get',this,'ownerDocument'); }
        get baseURI() { return dom('get',this,'baseURI'); }
        get isConnected() { return dom('get',this,'isConnected'); }
        get textContent() { return dom('get',this,'textContent'); }
        set textContent(v) { dom('set',this,'textContent',v == null ? '' : String(v)); }
        appendChild(child) { dom('insert',this,child,null); return child; }
        insertBefore(child,before) { dom('insert',this,child,before); return child; }
        removeChild(child) { if (child.parentNode !== this) throw new Error('NotFoundError'); dom('remove',child); return child; }
        replaceChild(child,old) { if(child!==old){this.insertBefore(child,old);this.removeChild(old);}return old; }
        cloneNode(deep = false) { if(rawDom('get',this,'shadowHost'))throw new DOMException('Shadow roots cannot be cloned directly','NotSupportedError');return dom('clone',this,!!deep); }
        isEqualNode(other = null) { return dom('equal',this,other); }
        isSameNode(other = null) { return dom('same',this,other); }
        contains(other) { for(let n=other;n;n=n.parentNode) if(n===this) return true; return false; }
        hasChildNodes() { return this.firstChild !== null; }
        getRootNode(options={}) { return rawDom('root',this,!!shadowBridge.dictionary(options).composed); }
    }
    class Element extends Node {
        get children() { return collectionBridge.children(this,true); }
        get firstElementChild() { return this.children[0] || null; }
        get lastElementChild() { const a = this.children; return a[a.length-1] || null; }
        get nextElementSibling() { let n = this.nextSibling; while (n && n.nodeType !== 1) n=n.nextSibling; return n; }
        get previousElementSibling() { let n=this.previousSibling; while(n && n.nodeType !== 1) n=n.previousSibling; return n; }
        get childElementCount() { return this.children.length; }
        remove() { if (this.parentNode) this.parentNode.removeChild(this); }
        append(...nodes) { for (const n of nodes) this.appendChild(n instanceof Node ? n : (this.ownerDocument||this).createTextNode(String(n))); }
        prepend(...nodes) { const before=this.firstChild; for (const n of nodes) this.insertBefore(n instanceof Node ? n : (this.ownerDocument||this).createTextNode(String(n)),before); }
        replaceChildren(...nodes) { this.textContent=''; this.append(...nodes); }
        before(...nodes) { if(this.parentNode) for(const n of nodes) this.parentNode.insertBefore(n instanceof Node?n:this.ownerDocument.createTextNode(String(n)),this); }
        after(...nodes) { if(this.parentNode) { const next=this.nextSibling; for(const n of nodes) this.parentNode.insertBefore(n instanceof Node?n:this.ownerDocument.createTextNode(String(n)),next); } }
        get tagName() { return this.nodeType === 1 ? this.nodeName : undefined; }
        get localName() { return dom('get',this,'localName'); }
        get namespaceURI() { return dom('get',this,'namespaceURI'); }
        get id() { return reflectedAttr(this,'id') || ''; }
        set id(v) { reflectedAttr(this,'id',String(v)); }
        get className() { return reflectedAttr(this,'class') || ''; }
        set className(v) { reflectedAttr(this,'class',String(v)); }
        get classList() { return tokenListBridge.for(this,'class'); }
        set classList(value) { tokenListBridge.for(this,'class').value=value; }
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
        getElementsByTagName(name) { const query=String(name)==='*'?'*':CSS.escape(String(name));return collectionBridge.html(()=>dom('query',this,query,false)); }
        getElementsByClassName(names) { const query=String(names).trim().split(/\s+/).filter(Boolean).map(x=>'.'+CSS.escape(x)).join('');return collectionBridge.html(()=>query?dom('query',this,query,false):[]); }
        getBoundingClientRect() { return dom('rect',this); }
        get clientWidth() { return dom('geometry',this,'clientWidth'); }
        get clientHeight() { return dom('geometry',this,'clientHeight'); }
        get clientLeft() { return dom('geometry',this,'clientLeft'); }
        get clientTop() { return dom('geometry',this,'clientTop'); }
        get style() { let s=state.get(this);if(!s)state.set(this,s={});return s.style||(s.style=new StyleDeclaration(this)); }
        get dataset() { const n=this;return new Proxy({}, {get(_,k){return reflectedAttr(n,'data-'+String(k).replace(/[A-Z]/g,c=>'-'+c.toLowerCase()));},set(_,k,v){reflectedAttr(n,'data-'+String(k).replace(/[A-Z]/g,c=>'-'+c.toLowerCase()),v);return true;}}); }
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
        get value() { return dom('get',this,'value'); }
        set value(v) { dom('set',this,'value',String(v)); }
        get checked() { return dom('get',this,'checked'); }
        set checked(v) { dom('set',this,'checked',!!v); }
        get selectedIndex() { return dom('get',this,'selectedIndex'); }
        set selectedIndex(v) { dom('set',this,'selectedIndex',Number(v)); }
        get selected() { return dom('get',this,'selected'); }
        set selected(v) { dom('set',this,'selected',!!v); }
        get disabled() { return reflectedAttr(this,'disabled')!==null; }
        set disabled(v) { reflectedAttr(this,'disabled',v?'':null); }
        focus() { dom('focus',this); }
        blur() { dom('blur',this); }
        click() { host.click(this); }
    }
    // A distinct native interface, not an alias or an instanceof override.
    // Nested browsing contexts/navigation are not implemented by this class.
    class HTMLIFrameElement extends HTMLElement {
        constructor() { throw new TypeError('Illegal HTMLIFrameElement constructor'); }
    }
    class HTMLUnknownElement extends HTMLElement {
        constructor(){throw new TypeError('Illegal HTMLUnknownElement constructor');}
    }
    Object.defineProperty(HTMLUnknownElement.prototype,Symbol.toStringTag,{value:'HTMLUnknownElement',configurable:true});
    class HTMLTemplateElement extends HTMLElement {
        constructor(){throw new TypeError('Illegal HTMLTemplateElement constructor');}
        get content(){return dom('get',this,'templateContent');}
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
            dom('get',this,'imageBrand'); const value=reflectedAttr(this,'src');
            if(value===null)return '';try{return new URL(value,this.baseURI).href;}catch(_){return value;}
        }
        set src(v) { dom('get',this,'imageBrand');reflectedAttr(this,'src',String(v)); }
        get alt() { dom('get',this,'imageBrand');return reflectedAttr(this,'alt')||''; }
        set alt(v) { dom('get',this,'imageBrand');reflectedAttr(this,'alt',String(v)); }
        get srcset() { dom('get',this,'imageBrand');return reflectedAttr(this,'srcset')||''; }
        set srcset(v) { dom('get',this,'imageBrand');reflectedAttr(this,'srcset',String(v)); }
        get sizes() { dom('get',this,'imageBrand');return reflectedAttr(this,'sizes')||''; }
        set sizes(v) { dom('get',this,'imageBrand');reflectedAttr(this,'sizes',String(v)); }
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
        get defaultValue(){return reflectedAttr(this,'value')||'';}
        set defaultValue(v){reflectedAttr(this,'value',String(v));}
        get defaultChecked(){return reflectedAttr(this,'checked')!==null;}
        set defaultChecked(v){reflectedAttr(this,'checked',v?'':null);}
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
        get defaultValue(){return this.textContent||'';}
        set defaultValue(v){this.textContent=v==null?'':String(v);}
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
        get defaultSelected(){return reflectedAttr(this,'selected')!==null;}
        set defaultSelected(v){reflectedAttr(this,'selected',v?'':null);}
    }
    function htmlElementBrand(node,tag){
        if(rawDom('get',node,'nodeType')!==1 || rawDom('get',node,'namespaceURI')!=='http://www.w3.org/1999/xhtml' ||
           rawDom('get',node,'localName')!==tag)throw new TypeError('Illegal '+tag+' receiver');
    }
    let elementURL; // Initialized by js_hyperlink after the private URL implementation.
    const formCollections=new WeakMap(), formResetting=new WeakSet();
    function formControls(form){
        htmlElementBrand(form,'form');
        let collection=formCollections.get(form);
        if(!collection){
            // SameObject/live collection; RadioNodeList and the dedicated collection type remain unsupported.
            collection=collectionBridge.html(()=>{
                let root=form,parent;
                while((parent=rawDom('get',root,'parentNode')))root=parent;
                return rawDom('query',root,'button,fieldset,input,object,output,select,textarea',false).filter(el=>{
                    if(rawDom('get',el,'namespaceURI')!=='http://www.w3.org/1999/xhtml')return false;
                    const tag=rawDom('get',el,'localName');
                    return !(tag==='input' && (reflectedAttr(el,'type')||'').toLowerCase()==='image') &&
                        rawDom('get',el,'form:'+tag)===form;
                });
            });
            formCollections.set(form,collection);
        }
        return collection;
    }
    class HTMLScriptElement extends HTMLElement {
        constructor(){throw new TypeError('Illegal HTMLScriptElement constructor');}
        static supports(type){
            if(!arguments.length)throw new TypeError('Script type required');
            type=elementURL.string(type);return type==='classic'||type==='module'||type==='importmap';
        }
        get async(){htmlElementBrand(this,'script');return !!dom('get',this,'async');}
        set async(v){htmlElementBrand(this,'script');dom('set',this,'async',!!v);}
        get defer(){htmlElementBrand(this,'script');return reflectedAttr(this,'defer')!==null;}
        set defer(v){htmlElementBrand(this,'script');reflectedAttr(this,'defer',v?'':null);}
        get src(){return elementURL.attribute(this,'script','src',false);}
        set src(v){htmlElementBrand(this,'script');reflectedAttr(this,'src',elementURL.scalar(v));}
        get type(){htmlElementBrand(this,'script');return reflectedAttr(this,'type')||'';}
        set type(v){htmlElementBrand(this,'script');reflectedAttr(this,'type',elementURL.string(v));}
        get noModule(){htmlElementBrand(this,'script');return reflectedAttr(this,'nomodule')!==null;}
        set noModule(v){htmlElementBrand(this,'script');reflectedAttr(this,'nomodule',v?'':null);}
        get text(){htmlElementBrand(this,'script');return rawDom('get',this,'textContent');}
        set text(v){htmlElementBrand(this,'script');dom('set',this,'textContent',v===null?'':elementURL.string(v));}
        get crossOrigin(){
            htmlElementBrand(this,'script');const v=reflectedAttr(this,'crossorigin');
            if(v===null)return null;
            return v.toLowerCase()==='use-credentials'?'use-credentials':'anonymous';
        }
        set crossOrigin(v){htmlElementBrand(this,'script');reflectedAttr(this,'crossorigin',v==null?null:elementURL.string(v));}
    }
    class HTMLFormElement extends HTMLElement {
        constructor(){throw new TypeError('Illegal HTMLFormElement constructor');}
        get elements(){return formControls(this);}
        get length(){return formControls(this).length;}
        get action(){return elementURL.attribute(this,'form','action',true);}
        set action(v){htmlElementBrand(this,'form');reflectedAttr(this,'action',elementURL.scalar(v));}
        get method(){
            htmlElementBrand(this,'form');const m=(reflectedAttr(this,'method')||'get').toLowerCase();
            return m==='post'||m==='dialog'?m:'get';
        }
        set method(v){htmlElementBrand(this,'form');reflectedAttr(this,'method',elementURL.string(v));}
        get name(){htmlElementBrand(this,'form');return reflectedAttr(this,'name')||'';}
        set name(v){htmlElementBrand(this,'form');reflectedAttr(this,'name',elementURL.string(v));}
        get target(){htmlElementBrand(this,'form');return reflectedAttr(this,'target')||'';}
        set target(v){htmlElementBrand(this,'form');reflectedAttr(this,'target',elementURL.string(v));}
        get enctype(){
            htmlElementBrand(this,'form');const e=(reflectedAttr(this,'enctype')||'').toLowerCase();
            if(e==='multipart/form-data'||e==='text/plain')return e;
            return 'application/x-www-form-urlencoded';
        }
        set enctype(v){htmlElementBrand(this,'form');reflectedAttr(this,'enctype',elementURL.string(v));}
        get encoding(){htmlElementBrand(this,'form');const e=(reflectedAttr(this,'enctype')||'').toLowerCase();return e==='multipart/form-data'||e==='text/plain'?e:'application/x-www-form-urlencoded';}
        set encoding(v){htmlElementBrand(this,'form');reflectedAttr(this,'enctype',elementURL.string(v));}
        get noValidate(){htmlElementBrand(this,'form');return reflectedAttr(this,'novalidate')!==null;}
        set noValidate(v){htmlElementBrand(this,'form');reflectedAttr(this,'novalidate',v?'':null);}
        submit(){htmlElementBrand(this,'form');dom('submit',this);}
        reset(){
            htmlElementBrand(this,'form');if(formResetting.has(this))return;
            formResetting.add(this);
            try { if(dispatch(this,new Event('reset',{bubbles:true,cancelable:true})))dom('reset',this); }
            finally { formResetting.delete(this); }
        }
        requestSubmit(submitter){
            htmlElementBrand(this,'form');
            if(submitter!=null){
                const tag=rawDom('get',submitter,'localName'),type=(reflectedAttr(submitter,'type')||'').toLowerCase();
                if(rawDom('get',submitter,'namespaceURI')!=='http://www.w3.org/1999/xhtml' ||
                   !(tag==='input' && (type==='submit'||type==='image') || tag==='button' && type!=='reset' && type!=='button'))
                    throw new TypeError('submitter must be a submit button');
                if(rawDom('get',submitter,'form:'+tag)!==this)throw new DOMException('submitter belongs to another form','NotFoundError');
            }
            const e=new Event('submit',{bubbles:true,cancelable:true});
            e.submitter=submitter||null;
            if(dispatch(this,e))dom('submit',submitter||this);
        }
    }
    class HTMLAnchorElement extends HTMLElement {
        constructor(){throw new TypeError('Illegal HTMLAnchorElement constructor');}
    }
    class HTMLAreaElement extends HTMLElement {
        constructor(){throw new TypeError('Illegal HTMLAreaElement constructor');}
    }
    Object.assign(Node, {ELEMENT_NODE:1,TEXT_NODE:3,PROCESSING_INSTRUCTION_NODE:7,COMMENT_NODE:8,DOCUMENT_NODE:9,DOCUMENT_TYPE_NODE:10,DOCUMENT_FRAGMENT_NODE:11});
    Object.assign(Node.prototype, {ELEMENT_NODE:1,TEXT_NODE:3,PROCESSING_INSTRUCTION_NODE:7,COMMENT_NODE:8,DOCUMENT_NODE:9,DOCUMENT_TYPE_NODE:10,DOCUMENT_FRAGMENT_NODE:11});
    class Document extends Node {}
    class HTMLDocument extends Document {}
    function characterDataBrand(node){
        const type=rawDom('get',node,'nodeType');if(type!==3 && type!==7 && type!==8)throw new TypeError('Illegal CharacterData receiver');
    }
    class CharacterData extends Node {
        get data(){characterDataBrand(this);return rawDom('get',this,'nodeValue');}
        set data(v){characterDataBrand(this);dom('set',this,'nodeValue',v===null?'':elementURL.string(v));}
        get length(){characterDataBrand(this);return rawDom('get',this,'nodeValue').length;}
        substringData(offset,count){
            characterDataBrand(this);if(arguments.length<2)throw new TypeError('Two arguments required');
            offset=offset>>>0;count=count>>>0;const data=rawDom('get',this,'nodeValue');
            if(offset>data.length)throw new DOMException('Offset exceeds data length','IndexSizeError');
            return data.slice(offset,offset+count);
        }
        appendData(data){
            characterDataBrand(this);if(!arguments.length)throw new TypeError('Data required');
            data=elementURL.string(data);dom('set',this,'nodeValue',rawDom('get',this,'nodeValue')+data);
        }
        insertData(offset,data){
            characterDataBrand(this);if(arguments.length<2)throw new TypeError('Two arguments required');
            offset=offset>>>0;data=elementURL.string(data);const old=rawDom('get',this,'nodeValue');
            if(offset>old.length)throw new DOMException('Offset exceeds data length','IndexSizeError');
            dom('set',this,'nodeValue',old.slice(0,offset)+data+old.slice(offset));
        }
        deleteData(offset,count){
            characterDataBrand(this);if(arguments.length<2)throw new TypeError('Two arguments required');
            offset=offset>>>0;count=count>>>0;const old=rawDom('get',this,'nodeValue');
            if(offset>old.length)throw new DOMException('Offset exceeds data length','IndexSizeError');
            dom('set',this,'nodeValue',old.slice(0,offset)+old.slice(offset+count));
        }
        replaceData(offset,count,data){
            characterDataBrand(this);if(arguments.length<3)throw new TypeError('Three arguments required');
            offset=offset>>>0;count=count>>>0;data=elementURL.string(data);const old=rawDom('get',this,'nodeValue');
            if(offset>old.length)throw new DOMException('Offset exceeds data length','IndexSizeError');
            dom('set',this,'nodeValue',old.slice(0,offset)+data+old.slice(offset+count));
        }
    }
    class Text extends CharacterData {}
    class Comment extends CharacterData {}
    function processingInstructionCreate(receiver,target,data){
        if(rawDom('get',receiver,'nodeType')!==9)throw new TypeError('Document receiver required');
        target=elementURL.string(target);data=elementURL.string(data);
        // XML 1.0 Name; this is distinct from HTML's PI token target syntax.
        if(!/^[:A-Z_a-z\u00C0-\u00D6\u00D8-\u00F6\u00F8-\u02FF\u0370-\u037D\u037F-\u1FFF\u200C-\u200D\u2070-\u218F\u2C00-\u2FEF\u3001-\uD7FF\uF900-\uFDCF\uFDF0-\uFFFD\u{10000}-\u{EFFFF}][:A-Z_a-z0-9.\-\u00B7\u0300-\u036F\u203F-\u2040\u00C0-\u00D6\u00D8-\u00F6\u00F8-\u02FF\u0370-\u037D\u037F-\u1FFF\u200C-\u200D\u2070-\u218F\u2C00-\u2FEF\u3001-\uD7FF\uF900-\uFDCF\uFDF0-\uFFFD\u{10000}-\u{EFFFF}]*(?![\s\S])/u.test(target) || data.includes('?>'))
            throw new DOMException('Invalid processing instruction target or data','InvalidCharacterError');
        return dom('create',receiver,7,target,data);
    }
    class ProcessingInstruction extends CharacterData {
        constructor(target,data=''){
            if(!arguments.length)throw new TypeError('ProcessingInstruction target required');
            const node=processingInstructionCreate(document,target,data),proto=new.target.prototype;
            if(proto!==null && (typeof proto==='object'||typeof proto==='function'))Object.setPrototypeOf(node,proto);
            return node;
        }
        get target(){return rawDom('get',this,'piTarget');}
    }
    Document.prototype.createProcessingInstruction=function(target,data){
        if(arguments.length<2)throw new TypeError('Target and data required');
        return processingInstructionCreate(this,target,data);
    };
    class DocumentFragment extends Node {}
    class DocumentType extends Node {
        get name(){return dom('get',this,'doctypeName');}
        get publicId(){return dom('get',this,'publicId');}
        get systemId(){return dom('get',this,'systemId');}
    }
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
    Object.setPrototypeOf(document,HTMLDocument.prototype);
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
    document.getElementsByName=name=>{const query='[name="'+CSS.escape(String(name))+'"]';return collectionBridge.live(()=>dom('query',document,query,false));};
    document.createElement=(name,options)=>customElementsBridge.create(name,options);
    document.createElementNS=(ns,name,options)=>{if(ns==='http://www.w3.org/1999/xhtml')return customElementsBridge.create(name,options,true);if(ns!=='http://www.w3.org/2000/svg')throw new DOMException('Unsupported namespace','NotSupportedError');return dom('create',null,1,String(name),'',true);};
    document.createTextNode=text=>dom('create',null,3,'#text',String(text));
    document.createComment=text=>dom('create',null,8,'#comment',String(text));
    document.createDocumentFragment=()=>dom('create',null,11,'#document-fragment','');
    document.write=(...s)=>host.write(s.join(''));
    document.writeln=(...s)=>host.write(s.join('')+'\n');
    document.createEvent=function(name){
        if(!arguments.length)throw new TypeError('createEvent requires interface');
        const type=String(name).toLowerCase();let e;
        if(['event','events','htmlevents','svgevents'].includes(type))e=new Event('');
        else if(type==='customevent')e=new CustomEvent('');
        else if(['mouseevent','mouseevents'].includes(type))e=new MouseEvent('');
        else if(['uievent','uievents'].includes(type))e=new UIEvent('');
        else if(['keyboardevent','keyevents'].includes(type))e=new KeyboardEvent('');
        else throw new DOMException('Unsupported event interface','NotSupportedError');
        e._initialized=false;return e;
    };
    function cssName(k){return String(k).replace(/[A-Z]/g,c=>'-'+c.toLowerCase());}
    class StyleDeclaration {
        constructor(node){this.node=node;return new Proxy(this,{get(t,k){if(k in t||typeof k==='symbol')return Reflect.get(t,k);return t.getPropertyValue(cssName(k));},set(t,k,v){if(k in t)return Reflect.set(t,k,v);t.setProperty(cssName(k),v);return true;}});}
        get cssText(){return reflectedAttr(this.node,'style')||'';}set cssText(v){reflectedAttr(this.node,'style',String(v));}
        getPropertyValue(name){return dom('style',this.node,String(name));}
        getPropertyPriority(name){return /!important\s*$/i.test(this.getPropertyValue(name))?'important':'';}
        setProperty(name,value,priority=''){dom('style',this.node,String(name),String(value),String(priority));}
        removeProperty(name){const old=this.getPropertyValue(name);this.setProperty(name,'');return old;}
        get length(){return this.cssText.split(';').filter(x=>x.includes(':')).length;}
        item(i){const a=this.cssText.split(';').filter(x=>x.includes(':'));return a[i]?a[i].split(':')[0].trim():'';}
    }
    for(const name of ['name','type','src','href','rel','action','method','placeholder','lang','dir','title'])
        Object.defineProperty(HTMLElement.prototype,name,{configurable:true,get(){const s=reflectedAttr(this,name)||'';if(['src','href','action'].includes(name)&&s){try{return new URL(s,this.baseURI).href;}catch(_){return s;}}return s;},set(v){reflectedAttr(this,name,String(v));}});
    const CSS={escape(s){return Array.from(String(s)).map((c,i)=>/[a-zA-Z_\-]/.test(c)||(/[0-9]/.test(c)&&i>0)?c:'\\'+c.codePointAt(0).toString(16)+' ').join('');}};
    const exceptionString=String;
    class DOMException extends Error {constructor(message='',name='Error'){super(message);this.name=exceptionString(name);}}
    /* @include js_tokens.js */
    const DOMTokenList=tokenListBridge.DOMTokenList;
    /* @include js_fetch.js */
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
    function queueMicrotask(fn){return host.microtask(fn);}
    const navigator={userAgent:'Nocturne/1.0 QuickJS',platform:'Nocturne',language:'en-US',languages:['en-US'],onLine:true};
    Object.assign(globalThis,{document,console,navigator,Node,Element,HTMLElement,HTMLUnknownElement,HTMLIFrameElement,HTMLImageElement,Image,
        HTMLInputElement,HTMLButtonElement,HTMLSelectElement,HTMLTextAreaElement,HTMLFieldSetElement,HTMLObjectElement,HTMLOutputElement,HTMLOptionElement,
        HTMLScriptElement,HTMLFormElement,HTMLAnchorElement,HTMLAreaElement,
        Document,HTMLDocument,HTMLTemplateElement,DocumentType,CharacterData,Text,Comment,ProcessingInstruction,DocumentFragment,
        Event,CustomEvent,UIEvent,MouseEvent,KeyboardEvent,EventTarget,DOMTokenList,CSS,Headers,Request,Response,DOMException,AbortController,AbortSignal,
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
    Object.defineProperty(Document.prototype,'cookie',{get(){return this===document?host.cookie():'';},set(v){v=String(v);if(this===document)host.cookie(v);}});
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
    fetchBridge.initialize();
    /* @include js_importmaps.js */
    /* @include js_hyperlink.js */
    /* @include js_html_elements.js */
    /* @include js_xhr.js */
    /* @include js_screen.js */
    /* @include js_intl.js */
    /* @include js_collator.js */
    /* @include js_crypto.js */
    /* @include js_clone.js */
    /* @include js_storage.js */
    /* @include js_messaging.js */
    /* @include js_css_supports.js */
    /* @include js_performance.js */
    /* @include js_history.js */
    /* @include js_custom_elements.js */
    /* @include js_media.js */
    /* @include js_observers.js */
    /* @include js_mutations.js */
    /* @include js_selection.js */
    /* @include js_document.js */
    /* @include js_svg.js */
    /* @include js_attributes.js */
    /* @include js_shadow.js */
    /* @include js_web_legacy.js */
    /* @include js_form_controls.js */
    /* @include js_form_validation.js */
    /* @include js_semantic_elements.js */
    /* @include js_canvas.js */
    /* @include js_avmedia.js */
    customElementsReady = true;
    /* Private native-input state, never reachable from page JS. C supplies the
       hit target's complete ancestry BEFORE any event handler can change it.
       Keep the previous snapshot, including ancestors of now-detached nodes. */
    let hoverPath = [];
    let hoverSnapshot = shadowBridge.capture([]);
    function hover(path, init) {
        if (path.length && path[path.length-1] === document) path[path.length] = globalThis;
        const previous = hoverPath, from = previous[0] || null, to = path[0] || null;
        const previousSnapshot=hoverSnapshot,currentSnapshot=shadowBridge.capture(path);
        const tasks = [];
        function contains(a,n) { for (let i=0;i<a.length;i++) if(a[i]===n) return true; return false; }
        function queue(type, ancestry, index, related, boundary) {
            const e = new MouseEvent(type, mouseAssign({},init,{
                relatedTarget:related,bubbles:!boundary,cancelable:!boundary,composed:!boundary
            }));
            e.isTrusted = true;
            const own=ancestry===previous?previousSnapshot:currentSnapshot,other=ancestry===previous?currentSnapshot:previousSnapshot;
            const part={nodes:apply(eventSlice,ancestry,[index]),info:own.info};
            tasks[tasks.length] = {event:e,target:part.nodes[0],plan:shadowBridge.path(part.nodes[0],e,part,other)};
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
        hoverSnapshot = currentSnapshot;
        let i=0;
        /* Return to native code between events: each native dispatch needs its
           microtask checkpoint, without rebuilding the snapshotted paths. */
        return function nextHoverEvent() {
            if(i===tasks.length) return false;
            const task=tasks[i++]; dispatch(task.target,task.event,task.plan);
            return true;
        };
    }
    return {
        storageOrigin:storageBridge.origin,
        registerImportMap:importMapsBridge.register,
        resolveModule:importMapsBridge.resolve,
        moduleURL:importMapsBridge.url,
        observerFrame(){observerBridge.frame();},
        eventHandlerAttribute:handlerAttribute,
        imageError(){return new DOMException('The image request changed or could not be decoded','EncodingError');},
        hover,
        customElementBefore(...args){
            const reactionArgs=args[0]==='adopt' && args[2] && rawDom('get',args[2],'nodeType')!==2?['remove',args[2]]:args;
            const reactionNode=args[0]==='adopt' && args[2] && rawDom('get',args[2],'nodeType')===2?rawDom('get',args[2],'attrOwner'):reactionArgs[1];
            const ceActive=reactionNode && rawDom('get',reactionNode,'scripting') || args[0]==='adopt' && rawDom('get',args[1],'scripting');
            const ce=ceActive?customElementsBridge.before(...args):null;
            let mutationArgs=reactionArgs;
            if(args[0]==='set' && args[2]==='innerHTML' && args[1] instanceof HTMLTemplateElement)
                mutationArgs=['set',rawDom('get',args[1],'templateContent'),'innerHTML',args[3]];
            const mutation=mutationBridge.before(...mutationArgs);
            return ce||mutation?{ce,mutation,op:args[0]}:null;
        },
        customElementAfter(token,result){mutationBridge.after(token.mutation);if(token.op!=='clone' || !result || rawDom('get',result,'scripting'))customElementsBridge.after(token.ce,result);},
        customElementScan(){customElementsBridge.upgradeTree(document);},
        slotChanges(){mutationBridge.signalSlots();},
        detailsToggle(target,oldOpen,newOpen){semanticElementsBridge.toggle(target,oldOpen,newOpen);},
        historyEvent(oldURL,popstate){historyEvent(oldURL,popstate);},
        mediaChanged(){mediaBridge.changed();},
        avmediaActivate(node){return avmediaBridge.activate(node);},
        inertClick(target){return dispatch(target,new MouseEvent('click',{bubbles:true,cancelable:true,composed:true}));},
        nodeProtos:[Node.prototype,HTMLDocument.prototype,Element.prototype,HTMLElement.prototype,
            Text.prototype,Comment.prototype,DocumentFragment.prototype,HTMLIFrameElement.prototype,HTMLImageElement.prototype,
            HTMLInputElement.prototype,HTMLButtonElement.prototype,HTMLSelectElement.prototype,HTMLTextAreaElement.prototype,
            HTMLFieldSetElement.prototype,HTMLObjectElement.prototype,HTMLOutputElement.prototype,HTMLOptionElement.prototype,HTMLTemplateElement.prototype,DocumentType.prototype,
            HTMLScriptElement.prototype,HTMLFormElement.prototype,HTMLAnchorElement.prototype,HTMLAreaElement.prototype,...svgBridge.nodeProtos,ProcessingInstruction.prototype,attributeBridge.nodeProto,...htmlElementsBridge.nodeProtos,shadowBridge.ShadowRoot.prototype,shadowBridge.HTMLSlotElement.prototype,...semanticElementsBridge.nodeProtos,HTMLUnknownElement.prototype,...canvasBridge.nodeProtos,HTMLMediaElement.prototype,HTMLAudioElement.prototype,HTMLVideoElement.prototype],
        dispatch(target,type,init){const C=type==='submit'?formValidationBridge.SubmitEvent:/^(key)/.test(type)?KeyboardEvent:/^(mouse|click|dblclick)/.test(type)?MouseEvent:Event;const e=new C(type,init);for(const k of Object.keys(init))if(k!=='submitter')e[k]=init[k];e.composed=/^(?:keydown|keyup|keypress|click|dblclick|mousedown|mouseup|mouseout|mousemove|mouseover|wheel|focus|blur|focusin|focusout|input)$/.test(type);e.isTrusted=true;return dispatch(target===null?globalThis:target,e);},
        response(...args){return fetchBridge.response(...args);},
        reject(message,abort){return abort?new DOMException(message,'AbortError'):new TypeError(message);}
    };
})(__nocturne_host);
