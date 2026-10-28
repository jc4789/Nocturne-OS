/* Browser bindings for Nocturne's sole native DOM, including native shadow trees. */
(function (host) {
    'use strict';
    delete globalThis.__nocturne_host;
    const domOperations=host.domOperations || Object.fromEntries(
        ['adopt', 'attr', 'attrCreate', 'attrList', 'attrNS', 'attrNode', 'attrNodeNS', 'attrRemoveNode', 'attrSetNode', 'blur', 'caretPosition', 'caretRect', 'clone', 'computed', 'create', 'cssom', 'customCandidates', 'customData', 'dialogEnter', 'dialogFocus', 'dialogLeave', 'dialogModal', 'dialogPrepare', 'doctypeCreate', 'documentCommand', 'elementFromPoint', 'elementScroll', 'elementScrollIntoView', 'equal', 'face', 'faceControls', 'filesSet', 'focus', 'formControls', 'formValue', 'geometry', 'get', 'id', 'imageDecode', 'import', 'insert', 'insertionStatus', 'isNode', 'matches', 'move', 'moveStatus', 'observerGeometry', 'parseDocument', 'parseFragment', 'position', 'query', 'rangeRects', 'rect', 'remove', 'replace', 'replaceAll', 'replaceAllStatus', 'replacementStatus', 'reset', 'root', 'same', 'selection', 'set', 'shadowAttach', 'slotAssign', 'slotChanges', 'slotNodes', 'style', 'styleDisabled', 'stylePriority', 'submit', 'validation', 'viewport', 'xpath'].map(op=>[op,(...args)=>host.dom(op,...args)]));
    const rawDom=Object.assign((op,...args)=>domOperations[op](...args),domOperations);
    // Reading never runs custom element reactions: getters go to the native operation directly.
    const domGet=domOperations.get;
    function validateInsertion(parent,child,before) {
        if(!rawDom.isNode(null,parent) || !rawDom.isNode(null,child) ||
            before!=null && !rawDom.isNode(null,before))throw new TypeError('Node arguments are required');
        const status=rawDom.insertionStatus(parent,child,before==null?null:before);
        if(status)throw new DOMException(status===2?'The reference node is not a child':'The node cannot be inserted here',
            status===2?'NotFoundError':'HierarchyRequestError');
    }
    let customElementsReady = false;
    function dom(op, ...args) {
        const mutation = op === 'shadowAttach' || op === 'slotAssign' || op === 'insert' || op === 'replace' || op === 'replaceAll' || op === 'remove' || op === 'adopt' || op === 'clone' || op === 'import' || op === 'set' || op === 'attrSetNode' || op === 'attrRemoveNode' ||
            (op === 'attrNS' && args.length > 3) ||
            ((op === 'attr' || op === 'style') && args.length > 2);
        return mutation && customElementsReady && customElementsBridge.active() ? customElementsBridge.reactions(() => domOperations[op](...args)) : domOperations[op](...args);
    }
    // IDL reflection always addresses the null namespace; getAttribute remains
    // a qualified-name lookup and may find an explicitly namespaced attribute.
    function reflectedAttr(node,name,...value) {
        return value.length?dom('attrNS',node,null,name,value[0]):rawDom.attrNS(node,null,name);
    }
    const apply = Reflect.apply;
    const eventSlice = Array.prototype.slice, mouseAssign = Object.assign;
    // Dispatch reads the listener lists through these, never through what a page can replace.
    const mapGet=WeakMap.prototype.get,mapSet=WeakMap.prototype.set,mapDelete=WeakMap.prototype.delete,EventTypeError=TypeError;
    const listenerMap = new WeakMap(), inlineMap = new WeakMap();
    const inputWindow=globalThis;
    const handlerMap = new WeakMap();
    let xhrHandlerTarget = () => false;
    let messageHandlerTarget = () => false;
    let svgHandlerTarget = () => false;
    let abortHandlerTarget = () => false;
    let shadowHandlerTarget = () => false;
    let avmediaHandlerTarget = () => false;
    let blobHandlerTarget = () => false;
    let socketHandlerTarget = () => false;
    let mseHandlerTarget = () => false;
    let emeHandlerTarget = () => false;
    let indexedDBHandlerTarget = () => false;
    let presentationHandlerTarget = () => false;
    let textTrackHandlerTarget = () => false;
    let audioHandlerTarget = () => false;
    let viewportHandlerTarget = () => false;
    let fontHandlerTarget = () => false;
    let animationHandlerTarget = () => false;
    const globalHandlerTypes = new Set(('abort animationcancel animationend animationiteration animationstart beforeinput beforematch beforetoggle blur cancel change click close dblclick error focus focusin focusout formdata input invalid keydown keypress keyup load mousedown mouseenter mouseleave mousemove mouseout mouseover mouseup reset resize scroll select slotchange submit toggle transitioncancel transitionend transitionrun transitionstart wheel').split(' '));
    const windowHandlerTypes = new Set(('afterprint beforeprint beforeunload hashchange languagechange message messageerror offline online pageswap pagehide pagereveal pageshow popstate rejectionhandled storage unhandledrejection unload').split(' '));
    const bodyWindowTypes=new Set([...windowHandlerTypes,'blur','error','focus','load','resize','scroll']);
    // HTML: event handler IDL attributes of Document objects only, with no content attribute
    // on any element (onreadystatechange belongs here too once readystatechange fires).
    const documentHandlerTypes=new Set(['visibilitychange']);
    const bodyWindowBrands=new WeakMap();
    const parsedBodyHandlers=new WeakSet();
    const state = new WeakMap();
    /* @include js_collections.js */
    function list(a) { return collectionBridge.list(a); }
    // HTML "report an exception" for what a callback threw (js.c report_exception): an ErrorEvent at
    // this global, then the console unless a listener canceled it. The native side reads only an own
    // stack data property, never a stack getter. errorReporting is the global's errors reporting
    // mode: an error listener's own exception goes to the console alone.
    function report(e) { host.reportException(e); }
    // This Window's current event (DOM "Legacy extensions to the Window interface"): window.event.
    let currentEvent;
    /* The steps of DOM "inner invoke" (https://dom.spec.whatwg.org/#concept-event-listener-inner-invoke)
       that belong to the realm of the listener's callback, run in that realm. This function is
       the realm's port (js.c native_event_realm_init): a dispatch in any realm of the runtime
       calls the port of the realm its listener's callback belongs to, which it learned when the
       listener was added. Steps 2.9 and 2.10: save this Window's current event, and set it to
       the event unless the invocation target is in a shadow tree (setCurrent). Step 2.13, Web
       IDL "call a user object's operation" with "handleEvent": a function is called with the
       event's currentTarget as this; any other object's handleEvent is got now, at every
       dispatch, and called with the object as this. So the Get, the TypeError for a handleEvent
       that is not callable and the call all happen in this realm. Step 2.13.1 reports what the
       callback threw for this realm's global, while the current event is still set; step 2.15
       restores it. The in passive listener flag (steps 2.11 and 2.14) is the dispatching
       realm's, set around this call; step 2.12 (record timing info, Event Timing) is not
       implemented. Returns whether the callback threw (step 2.13.2, the
       legacyOutputDidListenersThrowFlag IndexedDB reads). */
    function invokeListener(callback,thisArg,event,setCurrent) {
        const saved=currentEvent;
        if(setCurrent)currentEvent=event;
        try {
            if(typeof callback==='function')apply(callback,thisArg,[event]);
            else {
                const operation=callback.handleEvent;
                if(typeof operation!=='function')throw new EventTypeError('The event listener has no callable handleEvent');
                apply(operation,callback,[event]);
            }
            return false;
        } catch(e) { report(e); return true; }
        finally { currentEvent=saved; }
    }
    let errorReporting=false;
    function reportException(error,message,filename,lineno,colno,muted) {
        if(errorReporting)return true;
        errorReporting=true;
        try {
            const event=new ErrorEvent('error',muted?{message:'Script error.',error:null,cancelable:true}:
                {message,filename,lineno,colno,error,cancelable:true});
            eventState(event).isTrusted=true;
            return dispatch(globalThis,event);
        } finally { errorReporting=false; }
    }
    /* Event state: one table for every realm of this runtime (js.c native_event_realm_init), so that
       an Event made in one frame dispatches, and is read, in another (Web IDL brands an Event by its
       interface, not its realm). */
    const eventStates=host.eventRealmInit(invokeListener,new WeakMap()),eventGet=WeakMap.prototype.get,eventSet=WeakMap.prototype.set,eventDefine=Object.defineProperty,
        ownProperty=Object.prototype.hasOwnProperty;
    const eventFields=new Set(['type','target','srcElement','currentTarget','eventPhase','bubbles','cancelable','composed','defaultPrevented','isTrusted','timeStamp']);
    function eventState(event){const state=apply(eventGet,eventStates,[event]);if(!state)throw new TypeError('Illegal Event receiver');return state;}
    class Event {
        constructor(type, init = {}) {
            if(!arguments.length)throw new TypeError('Event requires type');
            type=String(type);init=init==null?{}:Object(init);
            apply(eventSet,eventStates,[this,{type,bubbles:!!init.bubbles,cancelable:!!init.cancelable,composed:!!init.composed,
                target:null,currentTarget:null,eventPhase:0,defaultPrevented:false,isTrusted:false,timeStamp:host.now(),
                stop:false,immediate:false,passive:false,dispatching:false,initialized:true,listenerException:false,path:null}]);
            eventDefine(this,'isTrusted',{enumerable:true,get(){return eventState(this).isTrusted;}});
        }
        initEvent(type,bubbles=false,cancelable=false) {
            const s=eventState(this);
            if(!arguments.length)throw new TypeError('initEvent requires type');
            type=String(type);if(s.dispatching)return;
            s.type=type;s.bubbles=!!bubbles;s.cancelable=!!cancelable;
            s.stop=s.immediate=s.defaultPrevented=false;s.isTrusted=false;s.target=null;s.initialized=true;
        }
        preventDefault() { const s=eventState(this);if(s.cancelable && !s.passive)s.defaultPrevented=true; }
        stopPropagation() { eventState(this).stop=true; }
        stopImmediatePropagation() { const s=eventState(this);s.stop=s.immediate=true; }
        composedPath() { const path=eventState(this).path;return path?copyList(path):[]; }
        get cancelBubble() { return eventState(this).stop; }
        set cancelBubble(v) { if (v) this.stopPropagation(); }
        get returnValue() { return !eventState(this).defaultPrevented; }
        set returnValue(v) { if (!v) this.preventDefault(); }
    }
    for(const key of eventFields)if(key!=='isTrusted')Object.defineProperty(Event.prototype,key,{
        configurable:true,enumerable:true,get(){return eventState(this)[key==='srcElement'?'target':key];}
    });
    Object.assign(Event, {NONE:0, CAPTURING_PHASE:1, AT_TARGET:2, BUBBLING_PHASE:3});
    Object.assign(Event.prototype, {NONE:0, CAPTURING_PHASE:1, AT_TARGET:2, BUBBLING_PHASE:3});
    /* @include js_error_event.js */
    /* @include js_promise_rejection.js */
    const rejectionBridge=promiseRejectionBridge(event=>dispatch(globalThis,event),host.promiseIsHandled,fn=>setTimeout(fn,0));
    /* @include js_custom_event.js */
    const CustomEvent=customEventInterface(Event,eventState);
    class UIEvent extends Event {
        constructor(t,o={}){super(t,o);this.view=o.view??null;this.detail=(+(o.detail??0))>>0;}
        initUIEvent(type,bubbles=false,cancelable=false,view=null,detail=0){
            if(!arguments.length)throw new TypeError('initUIEvent requires type');
            if(eventState(this).dispatching)return;
            Event.prototype.initEvent.call(this,type,bubbles,cancelable);this.view=view;this.detail=(+detail)>>0;
        }
    }
    /* @include js_input_event.js */
    // A FocusEvent's relatedTarget lives in its Event state, where dispatch in any realm retargets it.
    const focusEvents=new WeakSet();
    class FocusEvent extends UIEvent {
        constructor(type,init={}) {
            if(!arguments.length)throw new TypeError('FocusEvent requires type');
            init=init==null?{}:Object(init);super(type,init);
            const target=init.relatedTarget??null;
            if(target!==null && !(target instanceof EventTarget) && target!==globalThis)
                throw new TypeError('Expected an EventTarget');
            eventDefine(eventState(this),'relatedTarget',{value:target,writable:true,enumerable:true,configurable:true});
            focusEvents.add(this);
        }
        get relatedTarget(){if(!focusEvents.has(this))throw new TypeError('Illegal FocusEvent receiver');return eventState(this).relatedTarget;}
    }
    Object.defineProperty(FocusEvent.prototype,Symbol.toStringTag,{configurable:true,value:'FocusEvent'});
    Object.defineProperty(FocusEvent.prototype,'relatedTarget',Object.assign({},Object.getOwnPropertyDescriptor(FocusEvent.prototype,'relatedTarget'),{enumerable:true}));
    class MouseEvent extends UIEvent {
        constructor(t, o = {}) { super(t,o);const defaults={clientX:0, clientY:0, pageX:0, pageY:0,
            screenX:0,screenY:0,button:0, buttons:0, relatedTarget:null, ctrlKey:false, shiftKey:false, altKey:false, metaKey:false};
            o=o==null?{}:Object(o);for(const key of Object.keys(defaults))this[key]=o[key]===undefined?defaults[key]:o[key]; }
        initMouseEvent(type,bubbles=false,cancelable=false,view=null,detail=0,screenX=0,screenY=0,clientX=0,clientY=0,ctrlKey=false,altKey=false,shiftKey=false,metaKey=false,button=0,relatedTarget=null){
            if(!arguments.length)throw new TypeError('initMouseEvent requires type');
            if(eventState(this).dispatching)return;
            if(relatedTarget!==null && !(relatedTarget instanceof EventTarget) && relatedTarget!==globalThis)throw new TypeError('Expected an EventTarget');
            UIEvent.prototype.initUIEvent.call(this,type,bubbles,cancelable,view,detail);
            mouseAssign(this,{screenX:(+screenX)>>0,screenY:(+screenY)>>0,clientX:(+clientX)>>0,clientY:(+clientY)>>0,
                ctrlKey:!!ctrlKey,altKey:!!altKey,shiftKey:!!shiftKey,metaKey:!!metaKey,button:((+button)<<16)>>16,relatedTarget});
            this.pageX=this.clientX+host.scroll(0);this.pageY=this.clientY+host.scroll(1);
        }
        getModifierState(key){return ({Control:this.ctrlKey,Alt:this.altKey,Shift:this.shiftKey,Meta:this.metaKey})[String(key)]||false;}
    }
    class KeyboardEvent extends UIEvent {
        constructor(t, o = {}) { super(t,o);const defaults={key:'', code:'', keyCode:0, which:0,
            ctrlKey:false, shiftKey:false, altKey:false, metaKey:false, repeat:false};
            o=o==null?{}:Object(o);for(const key of Object.keys(defaults))this[key]=o[key]===undefined?defaults[key]:o[key]; }
    }
    /* The native side counts the listeners it acts on: non-passive wheel
     * listeners hold scrolling for the page, and only a Window with a storage
     * listener polls the shared storage for other windows' changes. */
    function nativeListenerChange(target,entry,delta) {
        if(entry.type==='storage'){if(target===inputWindow)host.storageListener(delta);return;}
        if(entry.type!=='wheel' || entry.passive)return;
        if(target===inputWindow)host.inputListener(null,delta);
        else if(rawDom.isNode(null,target))host.inputListener(target,delta);
    }
    function defaultPassive(target,type) {
        if(type!=='wheel' && type!=='mousewheel' && type!=='touchstart' && type!=='touchmove')return false;
        if(target===inputWindow)return host.inputListener(null,0);
        return rawDom.isNode(null,target)?host.inputListener(target,0):false;
    }
    /* The event listener lists are read and changed with loops over own elements and the
       captured WeakMap methods: a page that replaces Array.prototype methods, Array's species or
       WeakMap.prototype must not change which listeners an event reaches. */
    function copyList(a) {
        const out=[];
        for(let i=0;i<a.length;i++)out[i]=a[i];
        return out;
    }
    function listenerList(target) { return apply(mapGet,listenerMap,[target]); }
    function appendListener(target,entry) {
        let a=listenerList(target);
        if(!a)apply(mapSet,listenerMap,[target,a=[]]);
        a[a.length]=entry;
    }
    // DOM "remove an event listener".
    function removeListener(target, entry) {
        if (entry.removed) return;
        entry.removed = true;
        nativeListenerChange(target,entry,-1);
        const a=listenerList(target);
        if(a) {
            let i=0;while(i<a.length && a[i]!==entry)i++;
            if(i<a.length) {
                for(;i+1<a.length;i++)a[i]=a[i+1];
                a.length--;
                if(!a.length)apply(mapDelete,listenerMap,[target]);
            }
        }
        if(entry.unfollow)entry.unfollow();
        else if(entry.signal)entry.signal.removeEventListener('abort',entry.abort);
        entry.signal=entry.abort=entry.unfollow=null;
    }
    /* AbortSignal, as js_fetch.js defines it: is(value), aborted(signal), and follow(signal, steps),
       which adds abort steps (DOM "add an algorithm") and returns their removal. */
    let abortSignals={is:()=>false,aborted:()=>false,follow:()=>null};
    /* Another realm's AbortSignal: this realm cannot read its slots, so the listener follows its
       abort event instead of its abort algorithms. */
    function foreignSignal(value) {
        if(value===null || typeof value!=='object')return false;
        const port=host.eventRealm(value);
        return port!==null && port!==invokeListener && typeof value.addEventListener==='function';
    }
    /* DOM "flatten more" (addEventListener, more) and "flatten" (removeEventListener): the
       (AddEventListenerOptions or boolean) union. A non-object is the boolean capture; an object
       is the dictionary, whose members are got once each in Web IDL order: capture, then once,
       passive and signal. */
    function flattenOptions(o,more) {
        let capture=false,once=false,passive=null,signal=null;
        if(o!==undefined && o!==null) {
            if(typeof o!=='object' && typeof o!=='function')capture=!!o;
            else {
                capture=!!o.capture;
                if(more) {
                    once=!!o.once;
                    const p=o.passive;if(p!==undefined)passive=!!p;
                    const v=o.signal;
                    if(v!==undefined) {
                        if(!abortSignals.is(v) && !foreignSignal(v))throw new EventTypeError('The signal option must be an AbortSignal');
                        signal=v;
                    }
                }
            }
        }
        return {capture,once,passive,signal};
    }
    // Web IDL's EventListener? argument: null, undefined or an object, else a TypeError.
    function listenerArgument(callback) {
        if(callback===undefined || callback===null)return null;
        if(typeof callback!=='object' && typeof callback!=='function')throw new EventTypeError('The event listener must be an object');
        return callback;
    }
    /* DOM "add an event listener". The entry keeps the port of its callback's realm
       (host.eventRealm), looked up here once: a callback's realm never changes, so a dispatch
       pays nothing to find the realm whose current event and error reporting the listener
       uses. */
    function addListener(target,type,callback,o) {
        handlerRecord(target,type); // parsed inline handlers precede later listeners
        const signal=o.signal;
        if(signal!==null && (abortSignals.is(signal)?abortSignals.aborted(signal):!!signal.aborted))return;
        if(callback===null)return;
        const a=listenerList(target);
        if(a)for(let i=0;i<a.length;i++) {
            const x=a[i];
            if(x.type===type && x.callback===callback && x.capture===o.capture)return;
        }
        const x={type,callback,capture:o.capture,once:o.once,
            passive:o.passive===null?defaultPassive(target,type):o.passive,removed:false,
            signal:null,abort:null,unfollow:null,port:host.eventRealm(callback),handler:null};
        nativeListenerChange(target,x,1);
        appendListener(target,x);
        if(signal!==null) {
            x.signal=signal;x.abort=()=>removeListener(target,x);
            if(abortSignals.is(signal))x.unfollow=abortSignals.follow(signal,x.abort);
            else signal.addEventListener('abort',x.abort,{once:true});
        }
    }
    function resetDocumentEvents(root) {
        // document.open removes listeners and handlers, not the old attributes.
        // Keep null handler records so retained old nodes cannot recompile them.
        const pending=[root,globalThis];
        while(pending.length) {
            const target=pending.pop(),listeners=listenerList(target);
            if(bodyWindowHandler(target,'load'))parsedBodyHandlers.add(target);
            if(listeners){const copy=copyList(listeners);for(let i=0;i<copy.length;i++)removeListener(target,copy[i]);}
            const handlers=new Map();
            for(const type of globalHandlerTypes)if(handlerTarget(target,type))
                handlers.set(type,{value:null,text:null,compiled:true,entry:null});
            for(const type of documentHandlerTypes)if(handlerTarget(target,type))
                handlers.set(type,{value:null,text:null,compiled:true,entry:null});
            if(target===globalThis)for(const type of windowHandlerTypes)
                handlers.set(type,{value:null,text:null,compiled:true,entry:null});
            handlerMap.set(target,handlers);inlineMap.delete(target);
            if(target!==globalThis) {
                const shadow=rawDom.get(target,'shadowRoot');if(shadow)pending.push(shadow);
                for(const child of rawDom.get(target,'childNodes'))pending.push(child);
            }
        }
        hoverPath=[];hoverSnapshot=shadowBridge.capture([]);
        pointerCaptureBridge.reset();
    }
    class EventTarget {
        // The arguments are converted in order (type, callback, options) before any step runs.
        addEventListener(type, callback, init) {
            type=`${type}`;callback=listenerArgument(callback);
            addListener(this,type,callback,flattenOptions(init,true));
        }
        removeEventListener(type, callback, init) {
            type=`${type}`;callback=listenerArgument(callback);
            const capture=flattenOptions(init,false).capture,a=listenerList(this);
            if(a)for(let i=0;i<a.length;i++) {
                const x=a[i];
                if(x.type===type && x.callback===callback && x.capture===capture){removeListener(this,x);break;}
            }
        }
        dispatchEvent(event) {
            const s=eventState(event);
            if (!s.initialized || s.dispatching) throw new DOMException('Uninitialized or dispatching event','InvalidStateError');
            s.isTrusted=false;
            return dispatch(this, event);
        }
    }
    function handlerTarget(target,type) {
        return (globalHandlerTypes.has(type) && (target===globalThis || target instanceof HTMLElement || target instanceof Document || svgHandlerTarget(target))) ||
            (windowHandlerTypes.has(type) && target===globalThis) || (documentHandlerTypes.has(type) && target instanceof Document) || bodyWindowHandler(target,type) || xhrHandlerTarget(target,type) || messageHandlerTarget(target,type) || abortHandlerTarget(target,type) || shadowHandlerTarget(target,type) || avmediaHandlerTarget(target,type) || blobHandlerTarget(target,type) || mseHandlerTarget(target,type) || emeHandlerTarget(target,type) || indexedDBHandlerTarget(target,type) || presentationHandlerTarget(target,type) || textTrackHandlerTarget(target,type) || socketHandlerTarget(target,type) || audioHandlerTarget(target,type) || viewportHandlerTarget(target,type) || fontHandlerTarget(target,type) || animationHandlerTarget(target,type);
    }
    function bodyWindowHandler(target,type){
        if(!bodyWindowTypes.has(type) || target===globalThis)return false;
        if(target===null || (typeof target!=='object' && typeof target!=='function'))return false;
        if(bodyWindowBrands.has(target))return bodyWindowBrands.get(target);
        let body=false;
        if(rawDom.isNode(null,target) && rawDom.get(target,'nodeType')===1){
            const name=rawDom.get(target,'htmlInterface');body=name==='HTMLBodyElement'||name==='HTMLFrameSetElement';
        }
        // Native interface identity cannot change; the owner Window is resolved afresh.
        bodyWindowBrands.set(target,body);return body;
    }
    function handlerOwner(target,type){return bodyWindowHandler(target,type)?rawDom.get(target,'eventHandlerWindow'):target;}
    /* HTML "activate an event handler": one listener per handler, added once, whose callback
       runs the event handler processing algorithm. Its passive is null, so add an event
       listener gives it the default passive value. The HTML callback's realm is arbitrary;
       here it is the realm of the handler's current value (handlerPort), so inner invoke's
       check of the listener's global is also the processing algorithm's step 4, and window.event
       and the report of what the handler threw belong to the handler's realm. */
    function activateHandler(target,type,r) {
        if(r.entry)return;
        r.entry={type,capture:false,once:false,passive:defaultPassive(target,type),removed:false,
            signal:null,abort:null,unfollow:null,port:null,handler:r,
            callback(event){processHandler(target,type,event);}};
        nativeListenerChange(target,r.entry,1);
        appendListener(target,r.entry);
    }
    /* The port of a handler's value: set with the value when a script assigns an object, and
       this realm's own for a handler compiled from its content attribute here (r.port undefined). */
    function handlerPort(r) { return r.port===undefined?invokeListener:r.port; }
    /* HTML "the event handler processing algorithm", steps 2, 3 and 5 to 7, for target's handler
       named on+type; inner invoke did step 4 (handlerPort). The value is called with the event's
       currentTarget as this. Special error event handling (an ErrorEvent named error at a
       global) passes the ErrorEvent's five values, read from its private data, and cancels when
       the handler returns true; otherwise returning false cancels. A Window's (and so a body's)
       onbeforeunload is an OnBeforeUnloadEventHandler, whose result is converted to DOMString?
       while this event is still the current event. Canceling is DOM "set the canceled flag":
       it does nothing for a non-cancelable event or inside a passive listener. What the handler
       throws propagates to invokeListener, which reports it. */
    function processHandler(target,type,event) {
        const fn=handlerValue(target,type);
        if(typeof fn!=='function')return; // null, or an object that is not callable: nothing to invoke
        const s=eventState(event),current=s.currentTarget;
        const values=type==='error' && current===globalThis?errorEventValues(event):null;
        let result=values!==null?apply(fn,current,[values.message,values.filename,values.lineno,values.colno,values.error]):
            apply(fn,current,[event]);
        if(type==='beforeunload' && current===globalThis) {
            if(result!==undefined && result!==null)result=`${result}`;
            return; // no BeforeUnloadEvent yet, and a DOMString? is never false
        }
        if(values!==null?result===true:result===false) {
            if(s.cancelable && !s.passive)s.defaultPrevented=true;
        }
    }
    function handlerRecord(target,type) {
        if(!handlerTarget(target,type))return null;
        const original=target;target=handlerOwner(target,type);
        if(!target)return null;
        // Foreign owner globals use their own realm's listener/handler maps.
        if(target!==original && target!==globalThis)return null;
        let map=handlerMap.get(target);if(!map)handlerMap.set(target,map=new Map());
        let r=map.get(type);
        if(!r) {
            const source=target===globalThis && bodyWindowTypes.has(type)?rawDom.get(document,'body'):target;
            const text=source && (source instanceof HTMLElement || svgHandlerTarget(source))?reflectedAttr(source,'on'+type):null;
            r={value:null,text,compiled:text===null,entry:null};map.set(type,r);
            if(text!==null)activateHandler(target,type,r);
        }
        return r;
    }
    function handlerValue(target,type) {
        if(!handlerTarget(target,type))throw new TypeError('Illegal event handler receiver');
        const owner=handlerOwner(target,type);
        if(!owner)return null;
        if(owner!==target && owner!==globalThis)return host.windowHandler('get',target,type);
        target=owner;
        const r=handlerRecord(target,type);if(!r)throw new TypeError('Illegal event handler receiver');
        if(!r.compiled) {
            if(target instanceof Node && !rawDom.get(target,'scripting'))return null;
            r.compiled=true;
            try{r.value=host.inline(r.text,target===globalThis && type==='error');}catch(e){r.value=null;report(e);}
        }
        return r.value;
    }
    function setHandler(target,type,value,text) {
        if(!handlerTarget(target,type))throw new TypeError('Illegal event handler receiver');
        const owner=handlerOwner(target,type);
        if(!owner)return;
        if(owner!==target && owner!==globalThis)return host.windowHandler('set',target,type,value,text);
        target=owner;
        const r=handlerRecord(target,type);if(!r)throw new TypeError('Illegal event handler receiver');
        // [LegacyTreatNonObjectAsNull]: any object is kept, callable or not; anything else is null.
        const object=value!==null && (typeof value==='object' || typeof value==='function');
        r.value=object?value:null;r.text=text;r.compiled=text===null;
        r.port=object?host.eventRealm(value):undefined;
        if(r.value!==null || text!==null)activateHandler(target,type,r);
        else if(r.entry){removeListener(target,r.entry);r.entry=null;}
    }
    function parserBodyHandlers(body) {
        // Publish parser attributes before the next author script can replace
        // the corresponding Window property. document.open's old bodies remain
        // tombstoned, even if an author retains and later reinserts one.
        if(parsedBodyHandlers.has(body) || !bodyWindowHandler(body,'load') ||
           rawDom.get(body,'eventHandlerWindow')!==globalThis)return;
        parsedBodyHandlers.add(body);
        let map=handlerMap.get(globalThis);
        if(!map)handlerMap.set(globalThis,map=new Map());
        for(const type of bodyWindowTypes) {
            const text=reflectedAttr(body,'on'+type);
            if(text===null)continue;
            let r=map.get(type);
            if(!r)map.set(type,r={value:null,text:null,compiled:true,entry:null});
            r.value=null;r.text=text;r.compiled=false;r.port=undefined;
            activateHandler(globalThis,type,r);
        }
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
    /* DOM "inner invoke" (https://dom.spec.whatwg.org/#concept-event-listener-inner-invoke), in
       the realm that dispatches. Steps 2.1 to 2.8 are here; the realm part of steps 2.9 to 2.15
       runs in the port of the listener's realm (invokeListener), with the in passive listener
       flag set around it. The realm of a listener was found when it was added, so a listener
       costs one host.eventActive (pointer reads for the target's and the listener's Documents,
       asked anew for each listener because an earlier one can adopt the target or remove a
       frame) and one call through its port. capture is the phase: true "capturing", false
       "bubbling". inShadow is the path item's invocation-target-in-shadow-tree. Returns found. */
    function innerInvoke(target,event,s,listeners,capture,inShadow) {
        let found=false;
        for(let i=0;i<listeners.length;i++) {
            const x=listeners[i];
            if(x.removed || x.type!==s.type)continue;
            found=true;
            if(x.capture!==capture)continue;
            if(x.once)removeListener(target,x);
            const port=x.handler?handlerPort(x.handler):x.port;
            if(port===null || !host.eventActive(target,port===invokeListener?undefined:port))continue;
            if(x.passive)s.passive=true;
            const threw=port(x.callback,target,event,!inShadow);
            s.passive=false;
            if(threw)s.listenerException=true;
            if(s.immediate)break;
        }
        return found;
    }
    // DOM "invoke" step 10: the legacy types a trusted event also reaches when no listener has its type.
    const legacyEventTypes={__proto__:null,animationend:'webkitAnimationEnd',animationiteration:'webkitAnimationIteration',
        animationstart:'webkitAnimationStart',transitionend:'webkitTransitionEnd'};
    /* DOM "invoke" (https://dom.spec.whatwg.org/#concept-event-listener-invoke) steps 7 to 10;
       dispatch's visit did steps 1 to 6. The listeners are a clone of the list, so a listener
       added during this invocation does not run in it, while a removed one is skipped by its
       removed flag. */
    function invoke(target, event, capture, inShadow) {
        const s=eventState(event);s.currentTarget=target;
        handlerRecord(target,s.type);
        const a=listenerList(target);
        if(a) { // a target without listeners finds none, under its type or a legacy one
            const listeners=copyList(a);
            const found=innerInvoke(target,event,s,listeners,capture,inShadow);
            if(!found && s.isTrusted) {
                const legacy=legacyEventTypes[s.type];
                if(legacy!==undefined) {
                    const original=s.type;s.type=legacy;
                    try { innerInvoke(target,event,s,listeners,capture,inShadow); }
                    finally { s.type=original; }
                }
            }
        }
        /* Not in DOM or HTML: an on<type> property or attribute for a type this realm has no
           event handler for yet (its GlobalEventHandlers list is not complete) is still run as
           one, at the end of the bubbling invocation, through the same steps as a handler. */
        if (!capture && !s.immediate && !handlerTarget(target,s.type)) {
            let fn = target['on' + s.type],port;
            if (fn === undefined && target instanceof Node && target.nodeType === 1) {
                const text = reflectedAttr(target,'on' + s.type);
                if (text !== null && rawDom.get(target,'scripting')) {
                    let cache = inlineMap.get(target);
                    if (!cache) inlineMap.set(target, cache = new Map());
                    let x = cache.get(s.type);
                    if (!x || x.text !== text) {
                        try { x = {text, fn:host.inline(text)}; cache.set(s.type,x); }
                        catch (e) { s.listenerException=true;report(e); x = {text,fn:null}; cache.set(s.type,x); }
                    }
                    fn = x.fn;port=invokeListener;
                }
            }
            if (typeof fn === 'function') {
                if(port===undefined)port=host.eventRealm(fn);
                if(port!==null && host.eventActive(target,port===invokeListener?undefined:port)) {
                    const handler=function(){const result=apply(fn,target,[event]);if(result===false && s.cancelable && !s.passive)s.defaultPrevented=true;};
                    if(port(handler,target,event,!inShadow))s.listenerException=true;
                }
            }
        }
    }
    function dispatch(target, event, snapshot) {
        const s=eventState(event);s.dispatching=true;s.target=target;
        try {
        const plan=snapshot && snapshot.entries?snapshot:shadowBridge.path(target,{type:s.type,composed:s.composed,relatedTarget:event.relatedTarget},snapshot),entries=plan.entries;
        const hasRelated='relatedTarget' in event,relatedState=apply(ownProperty,s,['relatedTarget']);
        // Retarget a FocusEvent's relatedTarget in its Event state, not through its read-only IDL getter.
        // Mouse events still use their existing internal writable field.
        function related(value){if(relatedState)s.relatedTarget=value;else event.relatedTarget=value;}
        /* DOM "invoke" steps 1 to 6 for one path item: its shadow-adjusted target and
           relatedTarget, the composed path it sees, and the phase. A path item is in a shadow
           tree when its invocation target's root, snapshotted with the path, is a shadow root
           (entry.scopes, js_shadow.js); a listener there does not set window.event. */
        function visit(entry,capture,phase){
            s.target=entry.target;if(hasRelated)related(entry.related);
            s.path=entry.visible;s.eventPhase=phase;
            const scopes=entry.scopes;
            invoke(entry.node,event,capture,!!scopes && scopes.length>0);
        }
        for(let i=entries.length-1;i>0 && !s.stop;i--)visit(entries[i],true,entries[i].atTarget?2:1);
        if(entries.length && !s.stop){
            /* Capture and noncapture at AT_TARGET are distinct invocations.
               stopPropagation keeps this capture listener list running, but
               prevents the following noncapture invocation. */
            visit(entries[0],true,2);if(!s.stop)visit(entries[0],false,2);
        }
        for(let i=1;i<entries.length && !s.stop;i++)
            if(s.bubbles || entries[i].atTarget)visit(entries[i],false,entries[i].atTarget?2:3);
        s.target=plan.finalTarget;if(hasRelated)related(plan.finalRelated);
        return !s.defaultPrevented;
        } finally {
            s.path=null;s.currentTarget=null;s.eventPhase=0;s.dispatching=s.passive=s.stop=s.immediate=false;
        }
    }
    class Node extends EventTarget {
        constructor() { super(); throw new TypeError('Use document.createElement/createTextNode'); }
        get nodeType() { return domGet(this,'nodeType'); }
        get nodeName() { return domGet(this,'nodeName'); }
        get nodeValue() { return domGet(this,'nodeValue'); }
        set nodeValue(v) { dom('set',this,'nodeValue',v == null ? '' : String(v)); }
        get parentNode() { return domGet(this,'parentNode'); }
        get parentElement() { const p = this.parentNode; return p && p.nodeType === 1 ? p : null; }
        get firstChild() { return domGet(this,'firstChild'); }
        get lastChild() { return domGet(this,'lastChild'); }
        get nextSibling() { return domGet(this,'nextSibling'); }
        get previousSibling() { return domGet(this,'previousSibling'); }
        get childNodes() { return collectionBridge.children(this,false); }
        get ownerDocument() { return domGet(this,'ownerDocument'); }
        get baseURI() { return domGet(this,'baseURI'); }
        get isConnected() { return domGet(this,'isConnected'); }
        get textContent() { return domGet(this,'textContent'); }
        set textContent(v) { dom('set',this,'textContent',v == null ? '' : String(v)); }
        appendChild(child) { validateInsertion(this,child,null);dom('insert',this,child,null); return child; }
        insertBefore(child,before) { if(arguments.length<2)throw new TypeError('Two insertion arguments are required');validateInsertion(this,child,before);dom('insert',this,child,before==null?null:before); return child; }
        removeChild(child) { if(!rawDom.isNode(null,this)||!rawDom.isNode(null,child))throw new TypeError('Node arguments are required');if (rawDom.get(child,'parentNode') !== this) throw new DOMException('The node is not a child','NotFoundError'); dom('remove',child); return child; }
        replaceChild(child,old) {
            if(arguments.length<2 || !rawDom.isNode(null,this) || !rawDom.isNode(null,child) || !rawDom.isNode(null,old))
                throw new TypeError('Two Node arguments are required');
            const status=rawDom.replacementStatus(this,child,old);
            if(status)throw new DOMException(status===2?'The old node is not a child':'The replacement cannot be inserted here',
                status===2?'NotFoundError':'HierarchyRequestError');
            dom('replace',this,child,old);return old;
        }
        cloneNode(deep = false) { if(rawDom.get(this,'shadowHost'))throw new DOMException('Shadow roots cannot be cloned directly','NotSupportedError');return dom('clone',this,!!deep); }
        isEqualNode(other = null) { return dom('equal',this,other); }
        isSameNode(other = null) { return dom('same',this,other); }
        compareDocumentPosition(other) { return rawDom.position(this,other); }
        contains(other) { for(let n=other;n;n=n.parentNode) if(n===this) return true; return false; }
        hasChildNodes() { return this.firstChild !== null; }
        getRootNode(options={}) { return rawDom.root(this,!!shadowBridge.dictionary(options).composed); }
    }
    /* The (Node or DOMString)... arguments of append, prepend, before and after, converted as Web
       IDL does before the steps run (a string conversion may run author code). */
    function nodeValues(nodes){const values=[];for(let i=0;i<nodes.length;i++)values[i]=rawDom.isNode(null,nodes[i])?nodes[i]:elementURL.string(nodes[i]);return values;}
    /* DOM "convert nodes into a node": strings become Text nodes of document, and more than one
       node goes into a new DocumentFragment, so the caller pre-inserts once and the inserted
       scripts' post-connection steps run after every node is in place. */
    function convertNodes(document,values){
        const make=value=>typeof value==='string'?rawDom.create(document,3,'#text',value):value;
        if(values.length===1)return make(values[0]);
        const fragment=rawDom.create(document,11,'#document-fragment','');
        for(let i=0;i<values.length;i++){const child=make(values[i]);validateInsertion(fragment,child,null);dom('insert',fragment,child,null);}
        return fragment;
    }
    /* ParentNode prepend and append: pre-insert the converted node before this's first child, or
       at the end. One [CEReactions] scope for the whole call. */
    function parentInsert(receiver,nodes,first){
        const type=rawDom.get(receiver,'nodeType');
        if(type!==1&&type!==9&&type!==11)throw new TypeError('ParentNode receiver required');
        const values=nodeValues(nodes);
        const insert=()=>{const node=convertNodes(type===9?receiver:rawDom.get(receiver,'ownerDocument'),values);
            const child=first?rawDom.get(receiver,'firstChild'):null;validateInsertion(receiver,node,child);dom('insert',receiver,node,child);};
        if(customElementsReady)customElementsBridge.reactions(insert);else insert();
    }
    /* ChildNode before and after: the viable sibling is the first one on that side that is not
       among the nodes, then the converted node is pre-inserted into the parent next to it. */
    function siblingInsert(receiver,nodes,after){
        const kind=rawDom.get(receiver,'nodeType');
        if(kind!==1 && kind!==3 && kind!==7 && kind!==8 && kind!==10)throw new TypeError('ChildNode receiver required');
        const values=nodeValues(nodes);
        const parent=rawDom.get(receiver,'parentNode');
        if(!parent)return;
        const among=node=>{for(let i=0;i<values.length;i++)if(values[i]===node)return true;return false;};
        let viable=rawDom.get(receiver,after?'nextSibling':'previousSibling');
        while(viable&&among(viable))viable=rawDom.get(viable,after?'nextSibling':'previousSibling');
        const insert=()=>{const node=convertNodes(rawDom.get(receiver,'ownerDocument'),values);
            const child=after?viable:viable===null?rawDom.get(parent,'firstChild'):rawDom.get(viable,'nextSibling');
            validateInsertion(parent,node,child);dom('insert',parent,node,child);};
        if(customElementsReady)customElementsBridge.reactions(insert);else insert();
    }
    function replaceChildNode(receiver,nodes) {
        const kind=rawDom.get(receiver,'nodeType');
        if(kind!==1 && kind!==3 && kind!==7 && kind!==8 && kind!==10)throw new TypeError('ChildNode receiver required');
        // Convert the union before reading the parent: author string conversion
        // can move the receiver. Native branding does not depend on prototypes.
        const values=[];
        for(let i=0;i<nodes.length;i++)values[i]=rawDom.isNode(null,nodes[i])?nodes[i]:elementURL.string(nodes[i]);
        const parent=rawDom.get(receiver,'parentNode');
        if(!parent)return;
        let next=rawDom.get(receiver,'nextSibling');
        while(next){let used=false;for(let i=0;i<values.length;i++)if(values[i]===next){used=true;break;}
            if(!used)break;next=rawDom.get(next,'nextSibling');}
        const replace=()=>{
            const owner=rawDom.get(receiver,'ownerDocument');
            const make=value=>typeof value==='string'?rawDom.create(owner,3,'#text',value):value;
            let replacement;
            if(values.length===1)replacement=make(values[0]);
            else {
                replacement=rawDom.create(owner,11,'#document-fragment','');
                for(let i=0;i<values.length;i++)rawDom.insert(replacement,make(values[i]),null);
            }
            if(rawDom.get(receiver,'parentNode')===parent){
                const status=rawDom.replacementStatus(parent,replacement,receiver);
                if(status)throw new DOMException('The replacement cannot be inserted here',status===2?'NotFoundError':'HierarchyRequestError');
                rawDom.replace(parent,replacement,receiver);
            }else {validateInsertion(parent,replacement,next);rawDom.insert(parent,replacement,next);}
        };
        // rawDom still invokes native CE/MO hooks. One outer reaction scope
        // prevents author CE callbacks observing a half-finished replacement.
        if(customElementsReady)customElementsBridge.reactions(replace);else replace();
    }
    class Element extends Node {
        get children() { return collectionBridge.children(this,true); }
        get firstElementChild() { return this.children[0] || null; }
        get lastElementChild() { const a = this.children; return a[a.length-1] || null; }
        get nextElementSibling() { let n = this.nextSibling; while (n && n.nodeType !== 1) n=n.nextSibling; return n; }
        get previousElementSibling() { let n=this.previousSibling; while(n && n.nodeType !== 1) n=n.previousSibling; return n; }
        get childElementCount() { return this.children.length; }
        remove() { if (this.parentNode) this.parentNode.removeChild(this); }
        append(...nodes) { parentInsert(this,nodes,false); }
        prepend(...nodes) { parentInsert(this,nodes,true); }
        replaceChildren(...nodes) {
            const type=rawDom.get(this,'nodeType');
            if(![1,9,11].includes(type))throw new TypeError('ParentNode receiver required');
            const values=nodes.map(n=>rawDom.isNode(null,n)?n:elementURL.string(n));
            return customElementsBridge.reactions(()=>{
                const owner=type===9?this:rawDom.get(this,'ownerDocument');
                const make=value=>typeof value==='string'?rawDom.create(owner,3,'#text',value):value;
                let replacement=null;
                if(values.length===1)replacement=make(values[0]);
                else if(values.length){replacement=rawDom.create(owner,11,'#document-fragment','');
                    for(const value of values){const child=make(value);validateInsertion(replacement,child,null);dom('insert',replacement,child,null);}}
                if(rawDom.replaceAllStatus(this,replacement))throw new DOMException('The replacement cannot be inserted here','HierarchyRequestError');
                if(replacement){
                    if(rawDom.get(replacement,'parentNode'))dom('remove',replacement);
                    if(rawDom.get(replacement,'ownerDocument')!==owner)dom('adopt',owner,replacement);
                }
                dom('replaceAll',this,replacement);
            });
        }
        /* DOM moveBefore: "move" node into this before child, keeping its state (a frame's
           document, focus, animations); no removing or insertion steps, only the moving steps */
        moveBefore(node,child) {
            const type=rawDom.get(this,'nodeType');
            if(type!==1&&type!==9&&type!==11)throw new TypeError('ParentNode receiver required');
            if(arguments.length<2)throw new TypeError("Failed to execute 'moveBefore': 2 arguments required");
            if(!rawDom.isNode(null,node))throw new TypeError("Failed to execute 'moveBefore': parameter 1 is not of type 'Node'");
            if(child!=null&&!rawDom.isNode(null,child))throw new TypeError("Failed to execute 'moveBefore': parameter 2 is not of type 'Node'");
            let reference=child??null;
            if(reference===node)reference=rawDom.get(node,'nextSibling');
            const status=rawDom.moveStatus(this,node,reference);
            if(status)throw new DOMException(status===2?'The reference node is not a child of this node':'The node cannot be moved here',
                status===2?'NotFoundError':'HierarchyRequestError');
            dom('move',this,node,reference);
        }
        before(...nodes) { siblingInsert(this,nodes,false); }
        after(...nodes) { siblingInsert(this,nodes,true); }
        replaceWith(...nodes) { replaceChildNode(this,nodes); }
        get tagName() { return this.nodeType === 1 ? this.nodeName : undefined; }
        get localName() { return domGet(this,'localName'); }
        get namespaceURI() { return domGet(this,'namespaceURI'); }
        get prefix() { return domGet(this,'prefix'); }
        get id() { return reflectedAttr(this,'id') || ''; }
        set id(v) { reflectedAttr(this,'id',String(v)); }
        get className() { return reflectedAttr(this,'class') || ''; }
        set className(v) { reflectedAttr(this,'class',String(v)); }
        get classList() { return tokenListBridge.for(this,'class'); }
        set classList(value) { tokenListBridge.for(this,'class').value=value; }
        /* CSS Shadow Parts: the element's part names, for ::part() */
        get part() { return tokenListBridge.for(this,'part'); }
        set part(value) { tokenListBridge.for(this,'part').value=value; }
        getAttribute(name) { return dom('attr',this,String(name)); }
        setAttribute(name,value) { dom('attr',this,String(name),String(value)); }
        removeAttribute(name) { dom('attr',this,String(name),null); }
        hasAttribute(name) { return this.getAttribute(name) !== null; }
        toggleAttribute(name,force) { let on=this.hasAttribute(name); on=force===undefined?!on:!!force; if(on)this.setAttribute(name,'');else this.removeAttribute(name); return on; }
        getAttributeNames() { return domGet(this,'attributeNames'); }
        get innerHTML() { return domGet(this,'innerHTML'); }
        set innerHTML(v) { dom('set',this,'innerHTML',String(v)); }
        get outerHTML() { return domGet(this,'outerHTML'); }
        set outerHTML(value) {
            if(rawDom.get(this,'nodeType')!==1)throw new TypeError('Element receiver required');
            const markup=value===null?'':elementURL.string(value);
            // Conversion can move the receiver; sample its native parent only
            // afterwards. Fragment parsing is inert, unlike contextual Range.
            const parent=rawDom.get(this,'parentNode');
            if(!parent)return;
            const type=rawDom.get(parent,'nodeType');
            if(type===9)throw new DOMException('Document elements cannot be replaced with outerHTML','NoModificationAllowedError');
            const owner=rawDom.get(this,'ownerDocument');
            if(rawDom.get(owner,'contentType')!=='text/html')throw new DOMException('XML fragments are not implemented','NotSupportedError');
            customElementsBridge.reactions(()=>{
                const context=type===11?rawDom.create(owner,1,'body',''):parent;
                const fragment=rawDom.parseFragment(context,markup,false);
                dom('insert',parent,fragment,this);
                dom('remove',this);
            });
        }
        querySelector(selector) { return selectorQuery(this,selector,true); }
        querySelectorAll(selector) { return list(selectorQuery(this,selector,false)); }
        matches(selector) { if(!arguments.length)throw new TypeError("Failed to execute 'matches': 1 argument required");return selectorMatches(this,String(selector),this); }
        /* DOM: a legacy alias of matches (an operation of its own name, the same steps). */
        webkitMatchesSelector(selector) { if(!arguments.length)throw new TypeError("Failed to execute 'webkitMatchesSelector': 1 argument required");return selectorMatches(this,String(selector),this); }
        closest(selector) {
            // DOM: parse once, then the inclusive ancestor elements, with this as :scope
            if(!arguments.length)throw new TypeError("Failed to execute 'closest': 1 argument required");
            selector=String(selector);
            for(let n=this;n;){
                if(selectorMatches(n,selector,this))return n;
                const parent=domGet(n,'parentNode');
                n=parent&&domGet(parent,'nodeType')===1?parent:null;
            }
            return null;
        }
        getElementsByTagName(name) { name=String(name);return collectionBridge.domHTML(this,()=>rawDom.tagQuery(this,name)); }
        getElementsByTagNameNS(namespace,localName) {
            if(arguments.length<2)throw new TypeError('Namespace and local name are required');
            namespace=namespace==null?null:String(namespace);localName=String(localName);
            return collectionBridge.domHTML(this,()=>rawDom.tagQuery(this,namespace===''?null:namespace,localName,true));
        }
        getElementsByClassName(names) { const query=String(names).trim().split(/\s+/).filter(Boolean).map(x=>'.'+CSS.escape(x)).join('');return collectionBridge.domHTML(this,()=>query?dom('query',this,query,false):[]); }
        getBoundingClientRect() { rawDom.get(this,'elementBrand');return observerBridge.clientBoundingRect(dom('rect',this)); }
        getClientRects() { return observerBridge.clientRects(dom('rect',this,true)); }
        get clientWidth() { return dom('geometry',this,'clientWidth'); }
        get clientHeight() { return dom('geometry',this,'clientHeight'); }
        get clientLeft() { return dom('geometry',this,'clientLeft'); }
        get clientTop() { return dom('geometry',this,'clientTop'); }
        get style() { return inlineStyle(this); }
        // CSSOM [PutForwards=cssText]: assignment updates the native inline
        // declaration without replacing its SameObject style wrapper.
        set style(value) { inlineStyle(this).cssText=value; }
        get dataset() { const n=this;return new Proxy({}, {get(_,k){return reflectedAttr(n,'data-'+String(k).replace(/[A-Z]/g,c=>'-'+c.toLowerCase()));},set(_,k,v){reflectedAttr(n,'data-'+String(k).replace(/[A-Z]/g,c=>'-'+c.toLowerCase()),v);return true;}}); }
    }
    // DOM "scope-match a selectors string" and "match a selector": an invalid
    // selector is a "SyntaxError" DOMException.
    function invalidSelector(selector) { return domExceptionBridge.create('SyntaxError', "'"+selector+"' is not a valid selector"); }
    function selectorQuery(node, selector, first) {
        selector=String(selector);
        const result=dom('query',node,selector,first);
        if(result===false)throw invalidSelector(selector);
        return result;
    }
    function selectorMatches(node, selector, scope) {
        const result=dom('matches',node,selector,scope);
        if(result===null)throw invalidSelector(selector);
        return result;
    }
    // HTML FocusOptions (WebIDL dictionary, members in lexicographic order):
    // focusVisible, or undefined when absent; preventScroll is read and kept
    // for the conversion's observable getters.
    function focusOptionsVisible(options) {
        if (options === undefined || options === null) return undefined;
        if (typeof options !== 'object' && typeof options !== 'function') throw new TypeError('FocusOptions must be an object');
        const visible = options.focusVisible;
        void options.preventScroll;
        return visible === undefined ? undefined : !!visible;
    }
    class HTMLElement extends Element {
        constructor() { return customElementsBridge.construct(new.target); }
        get offsetParent() { return dom('geometry',this,'offsetParent'); }
        get offsetLeft() { return dom('geometry',this,'offsetLeft'); }
        get offsetTop() { return dom('geometry',this,'offsetTop'); }
        get offsetWidth() { return dom('geometry',this,'offsetWidth'); }
        get offsetHeight() { return dom('geometry',this,'offsetHeight'); }
        get value() { return domGet(this,'value'); }
        set value(v) { dom('set',this,'value',String(v)); }
        get checked() { return domGet(this,'checked'); }
        set checked(v) { dom('set',this,'checked',!!v); }
        get selectedIndex() { return domGet(this,'selectedIndex'); }
        set selectedIndex(v) { dom('set',this,'selectedIndex',Number(v)); }
        get selected() { return domGet(this,'selected'); }
        set selected(v) { dom('set',this,'selected',!!v); }
        get disabled() { return reflectedAttr(this,'disabled')!==null; }
        set disabled(v) { reflectedAttr(this,'disabled',v?'':null); }
        focus(options = undefined) { dom('focus',this,focusOptionsVisible(options)); }
        blur() { dom('blur',this); }
        click() { host.click(this); }
    }
    // A distinct native interface, not an alias or an instanceof override.
    // Nested browsing contexts/navigation are not implemented by this class.
    class HTMLIFrameElement extends HTMLElement {
        constructor(){return customElementsBridge.construct(new.target,HTMLIFrameElement);}
    }
    class HTMLFrameElement extends HTMLElement {
        constructor(){return customElementsBridge.construct(new.target,HTMLFrameElement);}
    }
    class HTMLUnknownElement extends HTMLElement {
        constructor(){throw new TypeError('Illegal HTMLUnknownElement constructor');}
    }
    Object.defineProperty(HTMLUnknownElement.prototype,Symbol.toStringTag,{value:'HTMLUnknownElement',configurable:true});
    class HTMLTemplateElement extends HTMLElement {
        constructor(){return customElementsBridge.construct(new.target,HTMLTemplateElement);}
        get content(){return domGet(this,'templateContent');}
    }
    class HTMLImageElement extends HTMLElement {
        constructor(){return customElementsBridge.construct(new.target,HTMLImageElement);}
        get width() { return domGet(this,'imageWidth'); }
        set width(v) { dom('set',this,'imageWidth',v); }
        get height() { return domGet(this,'imageHeight'); }
        set height(v) { dom('set',this,'imageHeight',v); }
        get naturalWidth() { return domGet(this,'imageNaturalWidth'); }
        get naturalHeight() { return domGet(this,'imageNaturalHeight'); }
        get complete() { return domGet(this,'imageComplete'); }
        get currentSrc() { return domGet(this,'imageCurrentSrc'); }
        get src() {
            domGet(this,'imageBrand'); const value=reflectedAttr(this,'src');
            if(value===null)return '';return elementURL.resolve(this,value);
        }
        set src(v) { domGet(this,'imageBrand');reflectedAttr(this,'src',String(v)); }
        get alt() { domGet(this,'imageBrand');return reflectedAttr(this,'alt')||''; }
        set alt(v) { domGet(this,'imageBrand');reflectedAttr(this,'alt',String(v)); }
        get srcset() { domGet(this,'imageBrand');return reflectedAttr(this,'srcset')||''; }
        set srcset(v) { domGet(this,'imageBrand');reflectedAttr(this,'srcset',String(v)); }
        get sizes() { domGet(this,'imageBrand');return reflectedAttr(this,'sizes')||''; }
        set sizes(v) { domGet(this,'imageBrand');reflectedAttr(this,'sizes',String(v)); }
        // A CORS settings attribute: the native request is a CORS one when it is present.
        get crossOrigin() {
            domGet(this,'imageBrand');const v=reflectedAttr(this,'crossorigin');
            return v===null?null:v.toLowerCase()==='use-credentials'?'use-credentials':'anonymous';
        }
        set crossOrigin(v) { domGet(this,'imageBrand');reflectedAttr(this,'crossorigin',v==null?null:String(v)); }
        decode() { return Promise.resolve().then(()=>dom('imageDecode',this)); }
    }
    function Image() {
        const image=new.target && new.target!==Image?customElementsBridge.construct(new.target,HTMLImageElement):dom('create',null,1,'img','');
        if(arguments.length>0)dom('set',image,'imageWidth',arguments[0]);
        if(arguments.length>1)dom('set',image,'imageHeight',arguments[1]);
        return image;
    }
    Object.defineProperty(Image,'prototype',{value:HTMLImageElement.prototype,writable:false});
    class HTMLInputElement extends HTMLElement {
        constructor(){return customElementsBridge.construct(new.target,HTMLInputElement);}
        get form(){return domGet(this,'form:input');}
        get defaultValue(){return reflectedAttr(this,'value')||'';}
        set defaultValue(v){reflectedAttr(this,'value',String(v));}
        get defaultChecked(){return reflectedAttr(this,'checked')!==null;}
        set defaultChecked(v){reflectedAttr(this,'checked',v?'':null);}
    }
    class HTMLButtonElement extends HTMLElement {
        constructor(){return customElementsBridge.construct(new.target,HTMLButtonElement);}
        get form(){return domGet(this,'form:button');}
    }
    class HTMLSelectElement extends HTMLElement {
        constructor(){return customElementsBridge.construct(new.target,HTMLSelectElement);}
        get form(){return domGet(this,'form:select');}
    }
    class HTMLTextAreaElement extends HTMLElement {
        constructor(){return customElementsBridge.construct(new.target,HTMLTextAreaElement);}
        get form(){return domGet(this,'form:textarea');}
        get defaultValue(){return this.textContent||'';}
        set defaultValue(v){this.textContent=v==null?'':String(v);}
    }
    class HTMLFieldSetElement extends HTMLElement {
        constructor(){return customElementsBridge.construct(new.target,HTMLFieldSetElement);}
        get form(){return domGet(this,'form:fieldset');}
    }
    class HTMLObjectElement extends HTMLElement {
        constructor(){return customElementsBridge.construct(new.target,HTMLObjectElement);}
        get form(){return domGet(this,'form:object');}
    }
    class HTMLOutputElement extends HTMLElement {
        constructor(){return customElementsBridge.construct(new.target,HTMLOutputElement);}
        get form(){return domGet(this,'form:output');}
    }
    class HTMLOptionElement extends HTMLElement {
        constructor(){return customElementsBridge.construct(new.target,HTMLOptionElement);}
        get form(){return domGet(this,'form:option');}
        get defaultSelected(){return reflectedAttr(this,'selected')!==null;}
        set defaultSelected(v){reflectedAttr(this,'selected',v?'':null);}
    }
    function htmlElementBrand(node,tag){
        // Native unwrap and HTML namespace validation remain authoritative.
        // Do not repeat three pipe/native crossings for every reflected IDL
        // access, or cache a brand across an allocation owner's retirement.
        if(rawDom.get(node,'htmlElementBrand')!==tag)throw new TypeError('Illegal '+tag+' receiver');
    }
    let elementURL; // Initialized by js_hyperlink after the private URL implementation.
    const formCollections=new WeakMap(), formResetting=new WeakSet();
    function formControls(form){
        htmlElementBrand(form,'form');
        let collection=formCollections.get(form);
        if(!collection){
            collection=formNameBridge.collection(()=>{
                let root=form,parent;
                while((parent=rawDom.get(root,'parentNode')))root=parent;
                return rawDom.formControls(root,form);
            });
            formCollections.set(form,collection);
        }
        return collection;
    }
    class HTMLScriptElement extends HTMLElement {
        constructor(){return customElementsBridge.construct(new.target,HTMLScriptElement);}
        static supports(type){
            if(!arguments.length)throw new TypeError('Script type required');
            type=elementURL.string(type);return type==='classic'||type==='module'||type==='importmap';
        }
        get async(){htmlElementBrand(this,'script');return !!domGet(this,'async');}
        set async(v){htmlElementBrand(this,'script');dom('set',this,'async',!!v);}
        get defer(){htmlElementBrand(this,'script');return reflectedAttr(this,'defer')!==null;}
        set defer(v){htmlElementBrand(this,'script');reflectedAttr(this,'defer',v?'':null);}
        get src(){return elementURL.attribute(this,'script','src',false);}
        set src(v){htmlElementBrand(this,'script');reflectedAttr(this,'src',elementURL.scalar(v));}
        get type(){htmlElementBrand(this,'script');return reflectedAttr(this,'type')||'';}
        set type(v){htmlElementBrand(this,'script');reflectedAttr(this,'type',elementURL.string(v));}
        get noModule(){htmlElementBrand(this,'script');return reflectedAttr(this,'nomodule')!==null;}
        set noModule(v){htmlElementBrand(this,'script');reflectedAttr(this,'nomodule',v?'':null);}
        get text(){htmlElementBrand(this,'script');return rawDom.get(this,'textContent');}
        set text(v){htmlElementBrand(this,'script');dom('set',this,'textContent',v===null?'':elementURL.string(v));}
        get crossOrigin(){
            htmlElementBrand(this,'script');const v=reflectedAttr(this,'crossorigin');
            if(v===null)return null;
            return v.toLowerCase()==='use-credentials'?'use-credentials':'anonymous';
        }
        set crossOrigin(v){htmlElementBrand(this,'script');reflectedAttr(this,'crossorigin',v==null?null:elementURL.string(v));}
    }
    class HTMLFormElement extends HTMLElement {
        constructor(){return customElementsBridge.construct(new.target,HTMLFormElement);}
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
                const tag=rawDom.get(submitter,'localName'),type=(reflectedAttr(submitter,'type')||'').toLowerCase();
                if(rawDom.get(submitter,'namespaceURI')!=='http://www.w3.org/1999/xhtml' ||
                   !(tag==='input' && (type==='submit'||type==='image') || tag==='button' && type!=='reset' && type!=='button'))
                    throw new TypeError('submitter must be a submit button');
                if(rawDom.get(submitter,'form:'+tag)!==this)throw new DOMException('submitter belongs to another form','NotFoundError');
            }
            const e=new Event('submit',{bubbles:true,cancelable:true});
            e.submitter=submitter||null;
            if(dispatch(this,e))dom('submit',submitter||this);
        }
    }
    /* @include js_form_named.js */
    class HTMLAnchorElement extends HTMLElement {
        constructor(){return customElementsBridge.construct(new.target,HTMLAnchorElement);}
    }
    class HTMLAreaElement extends HTMLElement {
        constructor(){return customElementsBridge.construct(new.target,HTMLAreaElement);}
    }
    Object.assign(Node, {ELEMENT_NODE:1,TEXT_NODE:3,CDATA_SECTION_NODE:4,PROCESSING_INSTRUCTION_NODE:7,COMMENT_NODE:8,DOCUMENT_NODE:9,DOCUMENT_TYPE_NODE:10,DOCUMENT_FRAGMENT_NODE:11});
    Object.assign(Node.prototype, {ELEMENT_NODE:1,TEXT_NODE:3,CDATA_SECTION_NODE:4,PROCESSING_INSTRUCTION_NODE:7,COMMENT_NODE:8,DOCUMENT_NODE:9,DOCUMENT_TYPE_NODE:10,DOCUMENT_FRAGMENT_NODE:11});
    for(const [name,value] of [['DISCONNECTED',1],['PRECEDING',2],['FOLLOWING',4],['CONTAINS',8],['CONTAINED_BY',16],['IMPLEMENTATION_SPECIFIC',32]])
        for(const target of [Node,Node.prototype])Object.defineProperty(target,'DOCUMENT_POSITION_'+name,{value,enumerable:true});
    // DOM "new Document()": an XML document of this realm whose content type
    // is application/xml; it implements Document, not XMLDocument.
    class Document extends Node {
        constructor(){
            const made=rawDom.documentCreate(null,'application/xml',false),proto=new.target.prototype;
            if(proto!==Document.prototype)Object.setPrototypeOf(made,proto);
            return made;
        }
    }
    class HTMLDocument extends Document { constructor(){throw new TypeError('Illegal HTMLDocument constructor');} }
    function characterDataBrand(node){
        const type=rawDom.get(node,'nodeType');if(type!==3 && type!==4 && type!==7 && type!==8)throw new TypeError('Illegal CharacterData receiver');
    }
    class CharacterData extends Node {
        get data(){characterDataBrand(this);return rawDom.get(this,'nodeValue');}
        set data(v){characterDataBrand(this);dom('set',this,'nodeValue',v===null?'':elementURL.string(v));}
        get length(){characterDataBrand(this);return rawDom.get(this,'nodeValue').length;}
        substringData(offset,count){
            characterDataBrand(this);if(arguments.length<2)throw new TypeError('Two arguments required');
            offset=offset>>>0;count=count>>>0;const data=rawDom.get(this,'nodeValue');
            if(offset>data.length)throw new DOMException('Offset exceeds data length','IndexSizeError');
            return data.slice(offset,offset+count);
        }
        appendData(data){
            characterDataBrand(this);if(!arguments.length)throw new TypeError('Data required');
            data=elementURL.string(data);dom('set',this,'nodeValue',rawDom.get(this,'nodeValue')+data);
        }
        insertData(offset,data){
            characterDataBrand(this);if(arguments.length<2)throw new TypeError('Two arguments required');
            offset=offset>>>0;data=elementURL.string(data);const old=rawDom.get(this,'nodeValue');
            if(offset>old.length)throw new DOMException('Offset exceeds data length','IndexSizeError');
            dom('set',this,'nodeValue',old.slice(0,offset)+data+old.slice(offset));
        }
        deleteData(offset,count){
            characterDataBrand(this);if(arguments.length<2)throw new TypeError('Two arguments required');
            offset=offset>>>0;count=count>>>0;const old=rawDom.get(this,'nodeValue');
            if(offset>old.length)throw new DOMException('Offset exceeds data length','IndexSizeError');
            dom('set',this,'nodeValue',old.slice(0,offset)+old.slice(offset+count));
        }
        replaceData(offset,count,data){
            characterDataBrand(this);if(arguments.length<3)throw new TypeError('Three arguments required');
            offset=offset>>>0;count=count>>>0;data=elementURL.string(data);const old=rawDom.get(this,'nodeValue');
            if(offset>old.length)throw new DOMException('Offset exceeds data length','IndexSizeError');
            dom('set',this,'nodeValue',old.slice(0,offset)+data+old.slice(offset+count));
        }
    }
    class Text extends CharacterData {
        constructor(data=''){
            const node=dom('create',document,3,'#text',elementURL.string(data)),proto=new.target.prototype;
            if(proto!==null && (typeof proto==='object'||typeof proto==='function'))Object.setPrototypeOf(node,proto);
            return node;
        }
        get wholeText(){
            const text=n=>{const t=rawDom.get(n,'nodeType');return t===3||t===4;};
            if(!text(this))throw new TypeError('Illegal Text receiver');
            let first=this,p;
            while((p=rawDom.get(first,'previousSibling')) && text(p))first=p;
            const parts=[];
            for(let n=first;n && text(n);n=rawDom.get(n,'nextSibling'))parts.push(rawDom.get(n,'nodeValue'));
            return parts.join('');
        }
    }
    // CDATASection is an exposed Text-derived interface even in an HTML realm:
    // a native text node of an XML document's CDATA section (createCDATASection,
    // the XML parser), never an alias of Text.
    class CDATASection extends Text {
        constructor(){throw new TypeError('Illegal CDATASection constructor');}
    }
    Object.defineProperty(CDATASection.prototype,Symbol.toStringTag,{value:'CDATASection',configurable:true});
    class Comment extends CharacterData {
        constructor(data=''){
            const node=dom('create',document,8,'#comment',elementURL.string(data)),proto=new.target.prototype;
            if(proto!==null && (typeof proto==='object'||typeof proto==='function'))Object.setPrototypeOf(node,proto);
            return node;
        }
    }
    function processingInstructionCreate(receiver,target,data){
        if(rawDom.get(receiver,'nodeType')!==9)throw new TypeError('Document receiver required');
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
        get target(){return rawDom.get(this,'piTarget');}
    }
    Document.prototype.createProcessingInstruction=function(target,data){
        if(arguments.length<2)throw new TypeError('Target and data required');
        return processingInstructionCreate(this,target,data);
    };
    class DocumentFragment extends Node {
        constructor(){
            // Return a genuinely branded native fragment, as createDocumentFragment
            // does. Calling Node's illegal public constructor would reject Lit's
            // slot capture before its DOM move can run.
            const node=dom('create',document,11,'#document-fragment',''),proto=new.target.prototype;
            if(proto!==null && (typeof proto==='object'||typeof proto==='function'))Object.setPrototypeOf(node,proto);
            return node;
        }
    }
    class DocumentType extends Node {
        get name(){return domGet(this,'doctypeName');}
        get publicId(){return domGet(this,'publicId');}
        get systemId(){return domGet(this,'systemId');}
    }
    function copyElementMembers(proto, names) {
        for (const name of names) Object.defineProperty(proto, name, Object.getOwnPropertyDescriptor(Element.prototype, name));
    }
    // ParentNode is shared, but attributes and matching belong only to elements.
    const parentMembers = ['children','firstElementChild','lastElementChild','childElementCount',
        'append','prepend','replaceChildren','moveBefore','querySelector','querySelectorAll'];
    copyElementMembers(Document.prototype, parentMembers.concat(['getElementsByTagName','getElementsByTagNameNS','getElementsByClassName']));
    copyElementMembers(DocumentFragment.prototype, parentMembers);
    copyElementMembers(CharacterData.prototype, ['nextElementSibling','previousElementSibling','remove','before','after','replaceWith']);
    copyElementMembers(DocumentType.prototype, ['replaceWith']);
    for(const proto of [Element.prototype,CharacterData.prototype,DocumentType.prototype]){
        const unscopables=Object.create(null);unscopables.replaceWith=true;
        Object.defineProperty(proto,Symbol.unscopables,{configurable:true,value:unscopables});
    }
    const document=host.document;
    /* HTML "create and initialize a Document": a document of type "xml" is a
       Document, not an HTMLDocument. */
    Object.setPrototypeOf(document,domGet(document,'htmlDocument')?HTMLDocument.prototype:Document.prototype);
    /* Configurable: these move to Document.prototype at the end of bootstrap. */
    Object.defineProperties(document, {
        documentElement:{configurable:true,get(){return domGet(document,'documentElement');}},
        head:{configurable:true,get(){return domGet(document,'head');}}, body:{configurable:true,get(){return domGet(document,'body');}},
        title:{configurable:true,get(){return domGet(document,'title');},set(v){dom('set',document,'title',String(v));}},
        URL:{configurable:true,get(){return host.url();}}, documentURI:{configurable:true,get(){return host.url();}},
        baseURI:{configurable:true,get(){return domGet(document,'baseURI');}},
        readyState:{configurable:true,get(){return host.ready();}}, activeElement:{configurable:true,get(){return domGet(document,'activeElement');}},
        defaultView:{configurable:true,value:globalThis}, currentScript:{configurable:true,get(){return host.current();}}
    });
    document.getElementById=id=>dom('id',document,String(id));
    document.getElementsByName=name=>{const query='[name="'+CSS.escape(String(name))+'"]';return collectionBridge.domLive(document,()=>dom('query',document,query,false));};
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
        else if(type==='focusevent')e=new FocusEvent('');
        else if(['keyboardevent','keyevents'].includes(type))e=new KeyboardEvent('');
        else throw new DOMException('Unsupported event interface','NotSupportedError');
        eventState(e).initialized=false;return e;
    };
    function cssName(k){return k==='cssFloat'?'float':String(k).replace(/[A-Z]/g,c=>'-'+c.toLowerCase());}
    const styleToken={},styleRecords=new WeakMap(),cssPropertyMetadata=host.cssProperties();
    /* CSSOM lists computed longhands in code-unit order with prefixed names
       last; the native table is already in code-unit order. Built on first
       enumeration, not in every realm's bootstrap. */
    let computedLonghands=null;
    function longhandNames(){
        if(computedLonghands)return computedLonghands;
        const names=[],prefixed=[];
        for(const [name,longhand] of cssPropertyMetadata)if(longhand)(name[0]==='-'?prefixed:names).push(name);
        return computedLonghands=names.concat(prefixed);
    }
    function styleRecord(value){const record=styleRecords.get(value);if(!record)throw new TypeError('Invalid CSSStyleDeclaration receiver');return record;}
    function styleProperty(name){name=String(name);return name.startsWith('--')?name:name.toLowerCase();}
    function styleNames(value){
        const record=styleRecord(value);
        if(!record.computed)return (reflectedAttr(record.node,'style')||'').split(';').filter(x=>x.includes(':')).map(x=>x.split(':')[0].trim());
        const custom=host.cssProperties(record.node,record.pseudo);if(custom===null)return [];
        const names=[],seen=new Set();for(const [name,valid] of custom){if(seen.has(name))continue;seen.add(name);if(valid)names.push(name);}
        return longhandNames().concat(names);
    }
    function styleRead(value,name){const record=styleRecord(value);return record.computed?
        dom('computed',record.node,styleProperty(name),record.pseudo):dom('style',record.node,styleProperty(name));}
    function styleWrite(value,name,text,priority=''){
        /* CSSOM: value and priority are [LegacyNullToEmptyString]. */
        name=styleProperty(name);text=text==null?'':String(text);priority=priority===null?'':String(priority);
        const record=styleRecord(value);if(record.computed)throw new DOMException('Computed style is read-only','NoModificationAllowedError');
        dom('style',record.node,name,text,priority);
    }
    function styleIndex(key){return typeof key==='string'&&/^(0|[1-9][0-9]*)$/.test(key)?Number(key):-1;}
    function inlineStyle(node){
        if(!rawDom.isNode(null,node)||rawDom.get(node,'nodeType')!==1)throw new TypeError('Expected an Element');
        let s=state.get(node);if(!s)state.set(node,s={});return s.style||(s.style=new CSSStyleProperties(node,false,styleToken));
    }
    class CSSStyleDeclaration {
        constructor(node,computed,token,pseudo=null){
            if(token!==styleToken)throw new TypeError('Illegal constructor');
            if(!rawDom.isNode(null,node)||rawDom.get(node,'nodeType')!==1)throw new TypeError('Expected an Element');
            const record={node,computed:!!computed,pseudo:computed&&pseudo!=null?String(pseudo):''};styleRecords.set(this,record);
            const proxy=new Proxy(this,{
                get(t,k,r){const i=styleIndex(k);return i>=0?styleNames(t)[i]:Reflect.get(t,k,r);},
                set(t,k,v,r){if(styleIndex(k)>=0)return false;return Reflect.set(t,k,v,r);},
                has(t,k){const i=styleIndex(k);return i>=0?i<styleNames(t).length:Reflect.has(t,k);},
                ownKeys(t){return Array.from(new Set(styleNames(t).map((_,i)=>String(i)).concat(Reflect.ownKeys(t))));},
                getOwnPropertyDescriptor(t,k){const i=styleIndex(k);if(i>=0){const name=styleNames(t)[i];return name===undefined?undefined:{value:name,writable:false,enumerable:true,configurable:true};}return Reflect.getOwnPropertyDescriptor(t,k);}
            });styleRecords.set(proxy,record);return proxy;
        }
        get cssText(){const r=styleRecord(this);return r.computed?'':reflectedAttr(r.node,'style')||'';}
        set cssText(v){v=v==null?'':String(v);const r=styleRecord(this);if(r.computed)throw new DOMException('Computed style is read-only','NoModificationAllowedError');reflectedAttr(r.node,'style',v);}
        get parentRule(){styleRecord(this);return null;}
        getPropertyValue(name){return styleRead(this,name);}
        getPropertyPriority(name){name=styleProperty(name);const r=styleRecord(this);return r.computed?'':dom('stylePriority',r.node,name);}
        setProperty(name,value,priority=''){styleWrite(this,name,value,priority);}
        removeProperty(name){name=styleProperty(name);const old=styleRead(this,name);styleWrite(this,name,'');return old;}
        get length(){return styleNames(this).length;}
        item(i){return styleNames(this)[Number(i)>>>0]||'';}
        *[Symbol.iterator](){for(let i=0;i<styleNames(this).length;i++)yield styleNames(this)[i];}
    }
    class CSSStyleProperties extends CSSStyleDeclaration {}
    /* Expose only the native engine's real properties, not a permissive `has`
       trap. Each named accessor reads/writes the same declaration as methods. */
    for(const [name] of cssPropertyMetadata){
        const camel=name==='float'?'cssFloat':name.replace(/-([a-z])/g,(_,c)=>c.toUpperCase());
        const names=new Set([name,camel]);if(name.startsWith('-webkit-'))names.add(camel[0].toLowerCase()+camel.slice(1));
        for(const key of names)Object.defineProperty(CSSStyleProperties.prototype,key,{configurable:true,enumerable:true,get(){return styleRead(this,name);},set(value){styleWrite(this,name,value);}});
    }
    Object.defineProperty(CSSStyleDeclaration.prototype,Symbol.toStringTag,{value:'CSSStyleDeclaration',configurable:true});
    Object.defineProperty(CSSStyleProperties.prototype,Symbol.toStringTag,{value:'CSSStyleProperties',configurable:true});
    const CSS={escape(s){return Array.from(String(s)).map((c,i)=>/[a-zA-Z_\-]/.test(c)||(/[0-9]/.test(c)&&i>0)?c:'\\'+c.codePointAt(0).toString(16)+' ').join('');}};
    /* @include js_dom_exception.js */
    const DOMException=domExceptionBridge.DOMException,QuotaExceededError=domExceptionBridge.QuotaExceededError;
    /* @include js_atob.js */
    /* @include js_pointer_events.js */
    /* @include js_tokens.js */
    const DOMTokenList=tokenListBridge.DOMTokenList;
    /* @include js_abort.js */
    /* @include js_fetch.js */
    /* This Window's settings and transport for Fetch: js.c's native fetch (CORS, cookies, Mixed
       Content, CSP and the HTTP cache are its), the document's base URL, origin, referrer source
       and policy container. */
    const fetchBridge=fetchInterfaces({signals:abortBridge.signals,
        transport:{noCors:()=>!!host.noCorsFetch,streaming:()=>!!host.streamingFetch,
            fetch:(q,notify)=>host.fetch(q.url,q.method,q.headers,q.body,q.sameOrigin,q.credentials,q.forcePreflight,q.redirectError?1:0,q.cache,
                q.keepalive,q.noCors,q.noReferrer,notify,q.referrer,q.referrerPolicy,q.destination),
            resume:id=>host.resumeFetch(id),cancel:id=>host.cancel(id)},
        baseURL:()=>rawDom.get(document,'baseURI'),origin:()=>rawDom.get(document,'fetchOrigin'),
        clientReferrer:()=>rawDom.get(document,'fetchClientReferrer'),referrerPolicy:()=>rawDom.get(document,'referrerPolicy'),
        url:()=>host.url(),dataURL:url=>host.dataURL(url),task:fn=>host.timer(0,fn,0,[]),clearTask:id=>host.clear(id),
        digest:(algorithm,bytes)=>host.cryptoDigest(algorithm,bytes),initiator:id=>performanceBridge.initiator(id,'xmlhttprequest')});
    const {Headers,Request,Response,fetch}=fetchBridge;
    const location={assign(url){host.navigate(String(url),0);},replace(url){host.navigate(String(url),2);},reload(){host.navigate(host.url(),1);},toString(){return this.href;}};
    Object.defineProperties(location,{href:{get(){return host.url();},set(v){host.navigate(htmlSafetyBridge.navigationInput(v));}}});
    document.location=location;
    /* @include js_console.js */
    const console=consoleNamespace((level,values)=>host.log(level,...values),()=>host.now());
    const timer=(kind,fn,ms,args)=>{const callback=typeof fn==='function'?fn:new Function(String(fn));return host.timer(kind,kind===2?callback:function(){return apply(callback,this,args);},Number(ms)||0,[]);};
    function setTimeout(fn,ms,...args){return timer(0,fn,ms,args);}function setInterval(fn,ms,...args){return timer(1,fn,ms,args);}
    function clearTimeout(id){host.clear(Number(id));}function clearInterval(id){host.clear(Number(id));}
    function requestAnimationFrame(fn){if(typeof fn!=='function')throw new TypeError('Expected callback');return timer(2,fn,16,[]);}
    function cancelAnimationFrame(id){host.clear(Number(id));}
    function queueMicrotask(fn){return host.microtask(fn);}
    function reportError(e){if(!arguments.length)throw new TypeError("Failed to execute 'reportError': 1 argument required");host.reportException(e);}
    /* @include js_navigator.js */
    const {navigator,Interface:Navigator,languagesChanged}=navigatorBridge.create(false);
    Object.assign(globalThis,{document,console,navigator,Navigator,Node,Element,HTMLElement,HTMLUnknownElement,HTMLIFrameElement,HTMLFrameElement,HTMLImageElement,Image,
        HTMLInputElement,HTMLButtonElement,HTMLSelectElement,HTMLTextAreaElement,HTMLFieldSetElement,HTMLObjectElement,HTMLOutputElement,HTMLOptionElement,
        HTMLScriptElement,HTMLFormElement,HTMLAnchorElement,HTMLAreaElement,
        Document,HTMLDocument,HTMLTemplateElement,DocumentType,CharacterData,Text,CDATASection,Comment,ProcessingInstruction,DocumentFragment,
        Event,ErrorEvent,PromiseRejectionEvent,CustomEvent,UIEvent,InputEvent,FocusEvent,MouseEvent,WheelEvent,PointerEvent,KeyboardEvent,EventTarget,DOMTokenList,CSS,Headers,Request,Response,DOMException,QuotaExceededError,AbortController,AbortSignal,
        fetch,setTimeout,setInterval,clearTimeout,clearInterval,requestAnimationFrame,cancelAnimationFrame,queueMicrotask,reportError,
        CSSStyleDeclaration,CSSStyleProperties,
        performance:{now:()=>host.now()},getComputedStyle:(n,pseudo=null)=>new CSSStyleProperties(n,true,styleToken,pseudo)});
    Object.defineProperty(globalThis,'location',{configurable:true,get(){return location;},set(v){host.navigate(String(v));}});
    globalThis.window=globalThis;globalThis.self=globalThis;globalThis.top=globalThis;globalThis.parent=globalThis;
    for(const n of ['addEventListener','removeEventListener','dispatchEvent']) {
        const method = EventTarget.prototype[n];
        globalThis[n] = function(...args) { return apply(method,this == null ? globalThis : this,args); };
    }
    installHandlers(HTMLElement.prototype,globalHandlerTypes);
    installHandlers(Document.prototype,globalHandlerTypes);
    installHandlers(Document.prototype,documentHandlerTypes);
    installHandlers(globalThis,globalHandlerTypes);
    installHandlers(globalThis,windowHandlerTypes);
    Object.defineProperties(globalThis,{innerWidth:{get(){return dom('viewport',null,0);}},innerHeight:{get(){return dom('viewport',null,1);}}});
    // CSSOM View: the window manager does not expose the window position to
    // applications, so it is reported as the screen origin. [Replaceable]:
    // assignment shadows the attribute with an ordinary data property.
    {const replace=(name,value)=>Object.defineProperty(globalThis,name,{configurable:true,enumerable:true,writable:true,value});
     Object.defineProperties(globalThis,Object.getOwnPropertyDescriptors({
        get outerWidth(){return host.windowSize(0);},set outerWidth(v){replace('outerWidth',v);},
        get outerHeight(){return host.windowSize(1);},set outerHeight(v){replace('outerHeight',v);},
        get screenX(){return 0;},set screenX(v){replace('screenX',v);},get screenLeft(){return 0;},set screenLeft(v){replace('screenLeft',v);},
        get screenY(){return 0;},set screenY(v){replace('screenY',v);},get screenTop(){return 0;},set screenTop(v){replace('screenTop',v);}}));
     /* DOM "Legacy extensions to the Window interface": [Replaceable] readonly attribute
        (Event or undefined) event, this Window's current event (invokeListener sets it). An
        undefined this is this global; another receiver is not this Window. The setter is Web
        IDL's [Replaceable] one: CreateDataPropertyOrThrow on the global. js_webidl.c gives the
        accessors their IDL names and lengths. */
     const windowReceiver=self=>{if(self!==undefined && self!==null && self!==globalThis)throw new EventTypeError('Illegal Window receiver');};
     Object.defineProperties(globalThis,Object.getOwnPropertyDescriptors({
        get event(){windowReceiver(this);return currentEvent;},
        set event(v){windowReceiver(this);replace('event',v);}}));}
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
    Object.defineProperty(globalThis,'webkitURL',{value:URL,writable:true,configurable:true}); // [LegacyWindowAlias]
    fetchBridge.initialize();
    /* @include js_streams.js */
    fetchBridge.initializeStreams(streamBridge);
    /* @include js_importmaps.js */
    /* @include js_hyperlink.js */
    /* @include js_html_elements.js */
    for(const C of [HTMLBodyElement,HTMLFrameSetElement])installHandlers(C.prototype,windowHandlerTypes);
    /* @include js_html_tables.js */
    /* @include js_xhr.js */
    /* @include js_blob.js */
    /* @include js_wasm.js */
    fetchBridge.initializeBlobs(blobBridge);
    /* @include js_screen.js */
    /* ECMA-402 and its CLDR and ICU tables (js_locale.js) take megabytes of heap
       in every realm. A realm builds them when it first reads Intl or calls a
       method they replace; until then those are an accessor and stubs that
       build them. A value a page stored over a stub stays the page's. */
    {
        const targets=[[Date.prototype,'toLocaleString'],[Date.prototype,'toLocaleDateString'],
            [Date.prototype,'toLocaleTimeString'],[Number.prototype,'toLocaleString'],
            [BigInt.prototype,'toLocaleString'],[String.prototype,'localeCompare']];
        const builtins=targets.map(([o,k])=>o[k]), built=builtins.slice(), stubs=[];
        let building=false;
        function build(){
            if(building)return;building=true;
            const kept=targets.map(([o,k],i)=>{const d=Reflect.getOwnPropertyDescriptor(o,k);return d&&d.value===stubs[i]?null:d;});
            const intl=Reflect.getOwnPropertyDescriptor(globalThis,'Intl');
            if(intl&&intl.get===intlGet)Reflect.deleteProperty(globalThis,'Intl');
            try{host.locale()(host);}
            finally{
                targets.forEach(([o,k],i)=>{
                    const d=Reflect.getOwnPropertyDescriptor(o,k);
                    if(d&&typeof d.value==='function'&&d.value!==stubs[i])built[i]=d.value;
                    if(kept[i])Reflect.defineProperty(o,k,kept[i]);
                    else if(d&&d.value===stubs[i])Reflect.defineProperty(o,k,{value:built[i],writable:true,enumerable:false,configurable:true});
                });
            }
        }
        targets.forEach(([o,k],i)=>{
            const stub={[k](...args){build();return apply(built[i],this,args);}}[k];
            Object.defineProperty(stub,'length',{value:builtins[i]?builtins[i].length:0,configurable:true});
            stubs[i]=stub;
            Object.defineProperty(o,k,{value:stub,writable:true,enumerable:false,configurable:true});
        });
        function intlGet(){build();return globalThis.Intl;}
        function intlSet(value){build();Reflect.defineProperty(globalThis,'Intl',{value,writable:true,enumerable:false,configurable:true});}
        Object.defineProperty(globalThis,'Intl',{get:intlGet,set:intlSet,enumerable:false,configurable:true});
    }
    /* @include js_crypto.js */
    /* @include js_formdata.js */
    fetchBridge.initializeFormData(formDataBridge);
    /* @include js_credentials.js */
    /* @include js_navigator_plugins.js */
    /* @include js_object_url.js */
    fetchBridge.initializeObjectURLs(objectURLBridge);
    /* @include js_clone.js */
    cloneData.registerUncloneable(formDataBridge.brand,'FormData');
    cloneData.registerUncloneable(formDataBridge.eventBrand,'FormDataEvent');
    /* @include js_worker.js */
    /* @include js_storage.js */
    /* @include js_indexeddb.js */
    /* @include js_opfs.js */
    installOriginPrivateFileSystem(host,cryptoSecureContext,Navigator,false);
    /* @include js_cache_storage.js */
    installCacheStorage({...fetchBridge.cache,secure:cryptoSecureContext,native:host.cacheStorage,
        embedderPolicy:host.cacheEmbedderPolicy,streams:streamBridge});
    /* @include js_messaging.js */
    /* @include js_websocket.js */
    /* @include js_css_supports.js */
    /* @include js_performance.js */
    /* @include js_idle.js */
    /* @include js_history.js */
    /* @include js_custom_elements.js */
    /* @include js_media.js */
    /* @include js_geometry.js */
    const geometryBridge=geometryInterfaces({parse:host.transformMatrix,list:collectionBridge.typed});
    /* @include js_observers.js */
    /* @include js_mutations.js */
    /* @include js_selection.js */
    /* @include js_document.js */
    /* @include js_frames.js */
    /* @include js_range.js */
    /* @include js_document_selection.js */
    /* @include js_document_commands.js */
    /* @include js_traversal.js */
    /* @include js_xpath.js */
    /* @include js_svg.js */
    /* @include js_attributes.js */
    /* @include js_shadow.js */
    /* @include js_stylesheets.js */
    /* @include js_font_loading.js */
    /* @include js_animations.js */
    /* @include js_web_legacy.js */
    /* @include js_form_controls.js */
    /* @include js_form_validation.js */
    /* @include js_element_internals.js */
    /* @include js_semantic_elements.js */
    htmlElementsBridge.initializeDialogs(ToggleEvent);
    /* @include js_offscreen_canvas.js */
    /* @include js_canvas_dash.js */
    /* @include js_canvas.js */
    /* @include js_mse.js */
    /* @include js_avmedia.js */
    /* @include js_eme.js */
    /* @include js_presentation.js */
    /* @include js_texttracks.js */
    /* @include js_sanitizer_constants.js */
    /* @include js_html_safety.js */
    /* @include js_pi.js */
    /* Interface members belong to the interface prototype; only [LegacyUnforgeable]
       members (a document's location) are own properties. Bootstrap defined some
       members on the singleton instance, so move each to the prototype: the
       singleton keeps its member, other instances keep the inherited
       one (or get a TypeError where there was none). */
    function ownMembersToPrototype(instance,proto,unforgeable){
        for(const key of Object.getOwnPropertyNames(instance)){
            const own=Object.getOwnPropertyDescriptor(instance,key);
            if(unforgeable.includes(key)||!own.configurable)continue;
            const inherited=Object.getOwnPropertyDescriptor(proto,key)||{};
            const merged={configurable:true,enumerable:true};
            if('value' in own){
                const f=own.value,g=inherited.value;
                if(typeof f!=='function')continue;
                merged.writable=true;
                merged.value={[key](...args){
                    if(this===instance)return apply(f,this,args);
                    if(typeof g!=='function')throw new TypeError('Illegal invocation');
                    return apply(g,this,args);
                }}[key];
                Object.defineProperty(merged.value,'length',{value:f.length,configurable:true});
            }else{
                const get=own.get,set=own.set,inheritedGet=inherited.get,inheritedSet=inherited.set;
                if(get||inheritedGet)merged.get=Object.getOwnPropertyDescriptor({get [key](){
                    if(this===instance&&get)return apply(get,this,[]);
                    if(!inheritedGet)throw new TypeError('Illegal invocation');
                    return apply(inheritedGet,this,[]);
                }},key).get;
                if(set||inheritedSet)merged.set=Object.getOwnPropertyDescriptor({set [key](value){
                    if(this===instance&&set){apply(set,this,[value]);return;}
                    if(!inheritedSet){if(this===instance)return;throw new TypeError('Illegal invocation');}
                    apply(inheritedSet,this,[value]);
                }},key).set;
            }
            delete instance[key];
            Object.defineProperty(proto,key,merged);
        }
    }
    ownMembersToPrototype(document,Document.prototype,['location']);
    ownMembersToPrototype(navigator,Navigator.prototype,[]);
    /* The ECMAScript shape Web IDL gives every interface, namespace and member these
       bindings define (js_webidl.c, from the specifications' IDL), before any author
       script observes them. */
    host.webidl('Window');
    frameBridge.finalizeMethods();
    customElementsReady = true;
    /* Private native-input state, never reachable from page JS. C supplies the
       hit target's complete ancestry BEFORE any event handler can change it.
       Keep the previous snapshot, including ancestors of now-detached nodes. */
    let hoverPath = [];
    let hoverSnapshot = shadowBridge.capture([]);
    function hover(path, init) {
        const capture=pointerCaptureBridge.prepare(mouseAssign({},init,{button:-1}));
        if(capture){
            path=[];
            for(let n=capture;n;n=rawDom.get(n,'assignedSlot')||rawDom.get(n,'parentNode')||rawDom.get(n,'shadowHost'))path.push(n);
        }
        if (path.length && path[path.length-1] === document) path[path.length] = globalThis;
        const previous = hoverPath, from = previous[0] || null, to = path[0] || null;
        const previousSnapshot=hoverSnapshot,currentSnapshot=shadowBridge.capture(path);
        const tasks = [];
        function contains(a,n) { for (let i=0;i<a.length;i++) if(a[i]===n) return true; return false; }
        function queue(type, ancestry, index, related, boundary) {
            const values=mouseAssign({},init,{
                view:globalThis,relatedTarget:related,bubbles:!boundary,cancelable:!boundary,composed:!boundary
            });
            const own=ancestry===previous?previousSnapshot:currentSnapshot,other=ancestry===previous?currentSnapshot:previousSnapshot;
            const part={nodes:apply(eventSlice,ancestry,[index]),info:own.info};
            const pointer=new PointerEvent(type.replace('mouse','pointer'),mousePointerInit(mouseAssign({},values,{button:-1})));
            eventState(pointer).isTrusted=true;
            tasks[tasks.length]={event:pointer,target:part.nodes[0],plan:shadowBridge.path(part.nodes[0],pointer,part,other)};
            if(type!=='mousemove'||!suppressCompatibilityMouse){
                const e=new MouseEvent(type,values);eventState(e).isTrusted=true;
                tasks[tasks.length]={event:e,target:part.nodes[0],plan:shadowBridge.path(part.nodes[0],e,part,other)};
            }
        }
        if (from !== to && from) queue('mouseout',previous,0,to,false);
        for (let i=0;i<previous.length;i++) {
            const node=previous[i];
            if(node!==globalThis && rawDom.get(node,'nodeType')===1 && !contains(path,node))
                queue('mouseleave',previous,i,to,true);
        }
        if (from !== to && to) queue('mouseover',path,0,from,false);
        for (let i=path.length-1;i>=0;i--) {
            const node=path[i];
            if(node!==globalThis && rawDom.get(node,'nodeType')===1 && !contains(previous,node))
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
        presentationSync:presentationBridge.sync,
        animationUpdate:animationBridge.update,
        animationEvents:animationBridge.events,
        resourceTiming:performanceBridge.resource,
        navigationTimingQueued:performanceBridge.navigationQueued,
        paintTiming:performanceBridge.paint,
        workerNotify:workerBridge.deliver,
        storageOrigin:storageBridge.origin,
        storageEvent:storageBridge.deliver,
        languageChange(){if(!languagesChanged())return;const event=new Event('languagechange');eventState(event).isTrusted=true;dispatch(globalThis,event);},
        // HTML "update the visibility state" steps 3 and 7 (js.c web_js_visibility_changed).
        // CSSOM View's resize steps (js.c run_resize_steps).
        resizeSteps(){viewportBridge.resize();},
        visibilityChange(visibility,time){
            performanceBridge.visibility(visibility,time);
            const event=new Event('visibilitychange',{bubbles:true});eventState(event).isTrusted=true;dispatch(document,event);
        },
        indexedDBTaskEnd:indexedDBBridge.taskEnd,
        promiseRejection:rejectionBridge.unhandled,
        reportException,
        promiseRejectionHandled:rejectionBridge.handled,
        formNamedProperty:formNameBridge.property,
        formNamedKeys:formNameBridge.keys,
        registerImportMap:importMapsBridge.register,
        resolveModule:importMapsBridge.resolve,
        moduleURL:importMapsBridge.url,
        moduleIntegrity:fetchBridge.moduleIntegrity,
        blobScript(url){
            const blob=objectURLBridge.blob(url);
            if(!blob)throw new TypeError('Blob script URL is revoked or unavailable in this document');
            return [blobBridge.bytes(blob),blobBridge.type(blob)];
        },
        blobPolicy:objectURLBridge.policy,
        observerFrame(){observerBridge.frame();},
        pointerCaptureTarget:pointerCaptureBridge.target,
        pointerClickTarget:pointerCaptureBridge.clickTarget,
        pointerCancel:pointerCaptureBridge.cancel,
        eventHandlerAttribute:handlerAttribute,
        bodyHandlerGet:handlerValue,bodyHandlerSet:setHandler,
        documentOpenReset:resetDocumentEvents,
        parserBodyHandlers,
        documentCommandEvent:documentCommandBridge.event,
        editingAction:(...args)=>documentCommandBridge.userAction(...args),
        editingFocus:(...args)=>documentCommandBridge.focused(...args),
        selectionText:()=>documentCommandBridge.selectionText(),
        frameMethodOriginal:frameBridge.methodOriginal,
        frameWindowProxy:frameBridge.windowProxy,
        windowMessageMethod:messagingBridge.postMessage,
        windowMessageLocal:messagingBridge.local,
        windowMessagePrepare:messagingBridge.prepare,
        windowMessageImport:messagingBridge.importPacket,
        windowMessageCommit:messagingBridge.commitImported,
        windowMessageReceive:messagingBridge.receive,
        imageError(){return new DOMException('The image request changed or could not be decoded','EncodingError');},
        invalidStateError(message){return new DOMException(message,'InvalidStateError');},
        safetyCheck:htmlSafetyBridge.check,
        safetyOptions:htmlSafetyBridge.options,
        safetyDynamicCode:htmlSafetyBridge.dynamicCode,
        safetyMutation:htmlSafetyBridge.mutation,
        safetyInput:htmlSafetyBridge.compliantInput,
        safetyViolation(init){return dispatch(document,htmlSafetyBridge.violationEvent(init));},
        hover,
        customElementBefore(...args){
            const reactionArgs=args[0]==='adopt' && args[2] && rawDom.get(args[2],'nodeType')!==2?['remove',args[2]]:args;
            const reactionNode=customElementsBridge.active()?(args[0]==='adopt' && args[2] && rawDom.get(args[2],'nodeType')===2?rawDom.get(args[2],'attrOwner'):reactionArgs[1]):null;
            const ceActive=customElementsBridge.active()&&(['clone','import'].includes(args[0]) || reactionNode && rawDom.get(reactionNode,'scripting') || args[0]==='adopt' && rawDom.get(args[1],'scripting'));
            const ce=ceActive?customElementsBridge.before(...args):null;
            let mutationArgs=reactionArgs;
            /* a move's live range and NodeIterator updates are those of a removal and insertion */
            if(args[0]==='move')mutationArgs=['insert',...args.slice(1)];
            if(args[0]==='set' && args[2]==='innerHTML' && args[1] instanceof HTMLTemplateElement)
                mutationArgs=['set',rawDom.get(args[1],'templateContent'),'innerHTML',args[3]];
            const mutation=mutationBridge.native?null:mutationBridge.before(...mutationArgs);
            const range=rangeBridge.active()?rangeBridge.before(...mutationArgs):null;
            const traversal=traversalBridge.before(...mutationArgs);
            return ce||mutation||range||traversal?{ce,mutation,range,traversal,op:args[0]}:null;
        },
        customElementAfter(token,result){rangeBridge.after(token.range);traversalBridge.after(token.traversal);if(!mutationBridge.native)mutationBridge.after(token.mutation);customElementsBridge.after(token.ce,result);},
        customElementCloneDocuments:customElementsBridge.cloneDocuments,
        mutationFlush(){mutationBridge.flush();},
        customElementScan(){customElementsBridge.upgradeTree(document);customElementsBridge.formRefresh();},
        customElementParserCandidate:customElementsBridge.parserCandidate,
        customElementParserCreate:customElementsBridge.parserCreate,
        customElementParserFinish:customElementsBridge.parserFinish,
        customElementParserInserted:customElementsBridge.parserInserted,
        customElementAllowShadow:customElementsBridge.allowShadow,
        customFormReset(form){customElementsBridge.formReset(form);},
        slotChanges(){mutationBridge.signalSlots();},
        dialogRequestClose(node){htmlElementsBridge.requestClose(node);},
        revealHidden:htmlElementsBridge.revealHidden,
        queueNativeElementScroll:htmlElementsBridge.queueElementScroll,
        dialogSubmit(node,result){return htmlElementsBridge.submit(node,result);},
        detailsToggle(target,oldOpen,newOpen){semanticElementsBridge.toggle(target,oldOpen,newOpen);},
        historyEvent(oldURL,popstate){historyEvent(oldURL,popstate);},
        historyOperation:historyBridge.operation,
        historySecurityError:historyBridge.securityError,
        mediaChanged(){mediaBridge.changed();},
        // CSS Font Loading's font loading task (js.c web_js_tick).
        fontsChanged(){fontLoadingBridge.changed();},
        avmediaActivate(node){return avmediaBridge.activate(node);},
        inertClick(target){return dispatch(target,new MouseEvent('click',{bubbles:true,cancelable:true,composed:true}));},
        nodeProtos:[Node.prototype,HTMLDocument.prototype,Element.prototype,HTMLElement.prototype,
            Text.prototype,Comment.prototype,DocumentFragment.prototype,HTMLIFrameElement.prototype,HTMLImageElement.prototype,
            HTMLInputElement.prototype,HTMLButtonElement.prototype,HTMLSelectElement.prototype,HTMLTextAreaElement.prototype,
            HTMLFieldSetElement.prototype,HTMLObjectElement.prototype,HTMLOutputElement.prototype,HTMLOptionElement.prototype,HTMLTemplateElement.prototype,DocumentType.prototype,
            HTMLScriptElement.prototype,HTMLFormElement.prototype,HTMLAnchorElement.prototype,HTMLAreaElement.prototype,...svgBridge.nodeProtos,ProcessingInstruction.prototype,attributeBridge.nodeProto,...htmlElementsBridge.nodeProtos,shadowBridge.ShadowRoot.prototype,shadowBridge.HTMLSlotElement.prototype,...semanticElementsBridge.nodeProtos,HTMLUnknownElement.prototype,...canvasBridge.nodeProtos,HTMLMediaElement.prototype,HTMLAudioElement.prototype,HTMLVideoElement.prototype,...formControlBridge.nodeProtos,...htmlElementsBridge.extraNodeProtos,...textTrackBridge.nodeProtos,HTMLFrameElement.prototype,...svgBridge.geometryNodeProtos,
            HTMLBodyElement.prototype,HTMLHtmlElement.prototype,HTMLParagraphElement.prototype,HTMLSpanElement.prototype,
            HTMLBRElement.prototype,HTMLHRElement.prototype,HTMLPreElement.prototype,HTMLQuoteElement.prototype,
            HTMLModElement.prototype,HTMLUListElement.prototype,HTMLDListElement.prototype,HTMLDirectoryElement.prototype,
            HTMLLegendElement.prototype,HTMLOptGroupElement.prototype,HTMLMapElement.prototype,HTMLEmbedElement.prototype,
            HTMLParamElement.prototype,HTMLFontElement.prototype,HTMLFrameSetElement.prototype,HTMLMarqueeElement.prototype,
            ...htmlTablesBridge.nodeProtos,svgBridge.groupProto,
            documentBridge.xmlDocumentPrototype,Document.prototype,CDATASection.prototype],
        dispatch:nativeDispatch,
        // js_webaudio.js, run apart when the page first touches a Web Audio interface.
        webaudioBridge:{host,Event,EventTarget,DOMException,eventState,dispatch,handlerValue,setHandler,Promise,
            handlerTarget(brand){audioHandlerTarget=brand;}},
        response(...args){return fetchBridge.response(...args);},
        reject(message,abort){return abort?new DOMException(message,'AbortError'):new TypeError(message);}
    };
})(__nocturne_host);
