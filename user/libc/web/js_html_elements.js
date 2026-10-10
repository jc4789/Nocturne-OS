/* Metadata interfaces backed by native DOM nodes, not instanceof overrides.
 * Include after js_hyperlink.js and js_tokens.js. Prototype order is shared
 * with NP_META..NP_HEAD in js.c. Native sheet metadata is in js_stylesheets.js.
 * https://html.spec.whatwg.org/multipage/semantics.html */
const htmlElementsBridge = (() => {
    'use strict';
    const define=Object.defineProperty, URLImpl=globalThis.URL, call=Reflect.apply;
    const hrefGet=Object.getOwnPropertyDescriptor(URLImpl.prototype,'href').get;
    const string=elementURL.string, scalar=elementURL.scalar, TypeErrorImpl=TypeError;
    const lower=text=>text.replace(/[A-Z]/g,c=>c.toLowerCase());
    class HTMLMetaElement extends HTMLElement {constructor(){return customElementsBridge.construct(new.target,HTMLMetaElement);}}
    class HTMLLinkElement extends HTMLElement {constructor(){return customElementsBridge.construct(new.target,HTMLLinkElement);}}
    class HTMLStyleElement extends HTMLElement {constructor(){return customElementsBridge.construct(new.target,HTMLStyleElement);}}
    class HTMLBaseElement extends HTMLElement {constructor(){return customElementsBridge.construct(new.target,HTMLBaseElement);}}
    class HTMLTitleElement extends HTMLElement {constructor(){return customElementsBridge.construct(new.target,HTMLTitleElement);}}
    class HTMLHeadElement extends HTMLElement {constructor(){return customElementsBridge.construct(new.target,HTMLHeadElement);}}
    class HTMLHeadingElement extends HTMLElement {constructor(){return customElementsBridge.construct(new.target,HTMLHeadingElement);}}
    class HTMLPictureElement extends HTMLElement {constructor(){return customElementsBridge.construct(new.target,HTMLPictureElement);}}
    class HTMLSourceElement extends HTMLElement {constructor(){return customElementsBridge.construct(new.target,HTMLSourceElement);}}
    class HTMLMenuElement extends HTMLElement {constructor(){return customElementsBridge.construct(new.target,HTMLMenuElement);}}
    class HTMLDivElement extends HTMLElement {constructor(){return customElementsBridge.construct(new.target,HTMLDivElement);}}
    reflect(HTMLDivElement,'div','align');
    function headingBrand(node){
        const tag=rawDom.get(node,'localName');
        if(!/^h[1-6]$/.test(tag))throw new TypeErrorImpl('Illegal heading receiver');
        htmlElementBrand(node,tag);
    }
    define(HTMLHeadingElement.prototype,'align',{configurable:true,enumerable:true,
        get(){headingBrand(this);return reflectedAttr(this,'align')||'';},
        set(value){headingBrand(this);reflectedAttr(this,'align',string(value));}});
    // pictureには固有IDL memberがない。画像選択はnative img resource経路の責務。
    for(const name of ['type','media','sizes'])reflect(HTMLSourceElement,'source',name);
    reflect(HTMLSourceElement,'source','srcset','srcset',scalar);
    define(HTMLSourceElement.prototype,'src',{configurable:true,enumerable:true,
        get(){return elementURL.attribute(this,'source','src',false);},
        set(value){htmlElementBrand(this,'source');reflectedAttr(this,'src',scalar(value));}});
    for(const name of ['width','height'])define(HTMLSourceElement.prototype,name,{configurable:true,enumerable:true,
        get(){htmlElementBrand(this,'source');const match=/^[\t\n\f\r ]*\+?(\d+)/.exec(reflectedAttr(this,name)||'');
            const number=match?Number(match[1]):0;return number<=2147483647?number:0;},
        set(value){htmlElementBrand(this,'source');reflectedAttr(this,name,string(value>>>0));}});
    // 廃止されたmenu.type/contextmenuは公開しない。現menuはcommandのul相当。
    define(HTMLMenuElement.prototype,'compact',{configurable:true,enumerable:true,
        get(){htmlElementBrand(this,'menu');return reflectedAttr(this,'compact')!==null;},
        set(value){htmlElementBrand(this,'menu');reflectedAttr(this,'compact',value?'':null);}});
    function htmlBrand(node){
        rawDom.get(node,'elementBrand');
        if(rawDom.get(node,'namespaceURI')!=='http://www.w3.org/1999/xhtml')
            throw new TypeErrorImpl('HTMLElement receiver required');
    }
    // CSSOM View element scrolling uses the real native scrolling box. There
    // are no author-visible offset fields or CSS transforms standing in for it.
    const finiteScroll=Number.isFinite,postScrollTask=host.postTask;
    const scrollTasks=new WeakMap();let smoothScrollReported=false;
    function scrollNumber(value){
        value=+value; // WebIDL ToNumber: Symbol and BigInt must throw.
        return finiteScroll(value)?value:0;
    }
    function elementScrollPosition(node,axis){
        rawDom.get(node,'elementBrand');
        return rawDom.geometry(node,axis);
    }
    function queueElementScroll(node){
        if(scrollTasks.has(node))return;
        const owner=rawDom.get(node,'ownerDocument');
        const record={owner};scrollTasks.set(node,record);
        try{postScrollTask(()=>{
            if(scrollTasks.get(node)!==record)return;
            scrollTasks.delete(node);
            if(rawDom.get(node,'ownerDocument')!==owner || !rawDom.get(node,'elementScrollActive'))return;
            const event=new Event('scroll');eventState(event).isTrusted=true;dispatch(node,event);
        });}catch(error){scrollTasks.delete(node);throw error;}
    }
    function applyElementScroll(node,x,y){
        // Native rechecks the current owner, active document and associated
        // box after all author conversions. Viewport forwarding queues its
        // existing Window event and intentionally returns false here.
        if(rawDom.elementScroll(node,x,y))queueElementScroll(node);
    }
    function instantScrollBehavior(behavior){
        if(behavior==='smooth' && !smoothScrollReported){
            smoothScrollReported=true;
            host.log(1,'Capability: smooth element scrolling is not implemented; applying an instant scroll');
        }
    }
    function elementScroll(node,args,relative){
        rawDom.get(node,'elementBrand');
        let x,y,behavior='auto';
        if(args.length>=2){x=scrollNumber(args[0]);y=scrollNumber(args[1]);}
        else {
            const options=args[0];
            if(options!=null && typeof options!=='object' && typeof options!=='function')
                throw new TypeErrorImpl('Scroll options must be a dictionary');
            if(options!=null){
                // Inherited dictionary member first, then left/top in IDL order.
                const b=options.behavior;
                if(b!==undefined){behavior=string(b);
                    if(behavior!=='auto' && behavior!=='instant' && behavior!=='smooth')
                        throw new TypeErrorImpl('Invalid scroll behavior');}
                const left=options.left;if(left!==undefined)x=scrollNumber(left);
                const top=options.top;if(top!==undefined)y=scrollNumber(top);
            }
        }
        // Read positions only after dictionary getters/value conversion, which
        // may themselves scroll, detach or adopt the receiver.
        if(relative || x===undefined)x=(relative?(x===undefined?0:x):0)+elementScrollPosition(node,'scrollLeft');
        if(relative || y===undefined)y=(relative?(y===undefined?0:y):0)+elementScrollPosition(node,'scrollTop');
        instantScrollBehavior(behavior);
        applyElementScroll(node,x,y);
    }
    for(const axis of ['scrollLeft','scrollTop'])define(Element.prototype,axis,{configurable:true,enumerable:true,
        get(){return elementScrollPosition(this,axis);},
        set(value){
            rawDom.get(this,'elementBrand');value=scrollNumber(value);
            const other=elementScrollPosition(this,axis==='scrollLeft'?'scrollTop':'scrollLeft');
            applyElementScroll(this,axis==='scrollLeft'?value:other,axis==='scrollTop'?value:other);
        }});
    for(const size of ['scrollWidth','scrollHeight'])define(Element.prototype,size,{configurable:true,enumerable:true,
        get(){return elementScrollPosition(this,size);}});
    define(Element.prototype,'scroll',{configurable:true,enumerable:true,writable:true,
        value:function scroll(options=undefined){elementScroll(this,arguments,false);}});
    define(Element.prototype,'scrollTo',{configurable:true,enumerable:true,writable:true,
        value:function scrollTo(options=undefined){elementScroll(this,arguments,false);}});
    define(Element.prototype,'scrollBy',{configurable:true,enumerable:true,writable:true,
        value:function scrollBy(options=undefined){elementScroll(this,arguments,true);}});
    function scrollLogical(value){
        value=string(value);
        if(value==='start')return 0;if(value==='end')return 1;
        if(value==='center')return 2;if(value==='nearest')return 3;
        throw new TypeErrorImpl('Invalid scroll alignment');
    }
    define(Element.prototype,'scrollIntoView',{configurable:true,enumerable:true,writable:true,
        value:function scrollIntoView(options=undefined){
            rawDom.get(this,'elementBrand');
            let behavior='auto',block=0,inline=3,nearest=false;
            if(options!=null && (typeof options==='object'||typeof options==='function')){
                const b=options.behavior;
                if(b!==undefined){behavior=string(b);
                    if(behavior!=='auto'&&behavior!=='instant'&&behavior!=='smooth')
                        throw new TypeErrorImpl('Invalid scroll behavior');}
                const at=options.block;if(at!==undefined)block=scrollLogical(at);
                const container=options.container;
                if(container!==undefined){const c=string(container);
                    if(c!=='all'&&c!=='nearest')throw new TypeErrorImpl('Invalid scroll container');nearest=c==='nearest';}
                const side=options.inline;if(side!==undefined)inline=scrollLogical(side);
            }else if(options!==undefined && options!==null && !options)block=1;
            instantScrollBehavior(behavior);
            // The native containing-block tree reflects rendered shadow/slot
            // distribution. Never invent a DOM parent or cross-frame traversal.
            const changed=rawDom.elementScrollIntoView(this,block,inline,nearest);
            for(let i=0;i<changed.length;i++)queueElementScroll(changed[i]);
        }});
    define(HTMLElement.prototype,'innerText',{configurable:true,enumerable:true,get(){
        htmlBrand(this);return rawDom.get(this,'innerText');
    },set(value){
        htmlBrand(this);value=string(value);
        const owner=rawDom.get(this,'ownerDocument'),fragment=rawDom.create(owner,11,'#document-fragment','');
        // Build the native fragment before replace-all. CRLF is one break;
        // lone CR/LF becomes a real HTML br, not markup or a text alias.
        const lines=value.split(/\r\n|[\r\n]/);
        for(let i=0;i<lines.length;i++){
            if(lines[i])dom('insert',fragment,rawDom.create(owner,3,'#text',lines[i]),null);
            if(i+1<lines.length)dom('insert',fragment,rawDom.create(owner,1,'br',''),null);
        }
        customElementsBridge.reactions(()=>{
            dom('set',this,'textContent','');dom('insert',this,fragment,null);
        });
    }});
    define(HTMLElement.prototype,'hidden',{configurable:true,enumerable:true,get(){
        htmlBrand(this);const value=reflectedAttr(this,'hidden');
        return value===null?false:lower(value)==='until-found'?'until-found':true;
    },set(value){
        htmlBrand(this);
        // nullable boolean/number/DOMString union。objectとBigIntはDOMStringへ変換。
        if(value!=null && typeof value!=='boolean' && typeof value!=='number' && typeof value!=='string')value=string(value);
        reflectedAttr(this,'hidden',typeof value==='string' && lower(value)==='until-found'?'until-found':value?'':null);
    }});
    function revealHidden(ancestors,owner){
        // Native flat-tree snapshot is captured before any author callback.
        // Use the real attribute mutation bridge, including CE/MO reactions.
        for(const node of ancestors){
            if(rawDom.get(node,'ownerDocument')!==owner || !rawDom.get(node,'isConnected') ||
                lower(rawDom.attr(node,'hidden')||'')!=='until-found')return false;
            const event=new Event('beforematch',{bubbles:true});eventState(event).isTrusted=true;dispatch(node,event);
            if(rawDom.get(node,'ownerDocument')!==owner || !rawDom.get(node,'isConnected') ||
                lower(rawDom.attr(node,'hidden')||'')!=='until-found')return false;
            dom('attr',node,'hidden',null);
        }
        return true;
    }
    function adjacent(node,position){
        rawDom.get(node,'elementBrand');position=lower(string(position));
        let parent,before;
        if(position==='beforebegin' || position==='afterend'){
            parent=rawDom.get(node,'parentNode');
            if(!parent || rawDom.get(parent,'nodeType')===9)
                throw new DOMException('No parent insertion point','NoModificationAllowedError');
            before=position==='beforebegin'?node:rawDom.get(node,'nextSibling');
        }else if(position==='afterbegin' || position==='beforeend'){
            parent=node;before=position==='afterbegin'?rawDom.get(node,'firstChild'):null;
        }else throw new DOMException('Invalid adjacent position','SyntaxError');
        let context=parent;
        if(rawDom.get(context,'nodeType')!==1 || rawDom.get(context,'namespaceURI')==='http://www.w3.org/1999/xhtml' && rawDom.get(context,'localName')==='html')
            context=rawDom.create(rawDom.get(node,'ownerDocument'),1,'body','');
        return {parent,before,context};
    }
    define(Element.prototype,'insertAdjacentHTML',{configurable:true,enumerable:true,writable:true,value:function(position,markup){
        rawDom.get(this,'elementBrand');if(arguments.length<2)throw new TypeErrorImpl('Position and markup required');
        position=string(position);markup=string(markup);const point=adjacent(this,position);
        const fragment=rawDom.parseFragment(point.context,markup);
        dom('insert',point.parent,fragment,point.before);
    }});
    function insertAdjacentNode(node,position,child){
        position=lower(position);let parent,before;
        if(position==='beforebegin'||position==='afterend'){
            parent=rawDom.get(node,'parentNode');if(!parent)return null;
            before=position==='beforebegin'?node:rawDom.get(node,'nextSibling');
        }else if(position==='afterbegin'||position==='beforeend'){
            parent=node;before=position==='afterbegin'?rawDom.get(node,'firstChild'):null;
        }else throw new DOMException('Invalid adjacent position','SyntaxError');
        if(rawDom.get(parent,'nodeType')===9 &&
           (rawDom.get(child,'nodeType')===3 || child!==node))
            throw new DOMException('Document cannot have text or a second root element','HierarchyRequestError');
        for(let ancestor=parent;ancestor;ancestor=rawDom.get(ancestor,'parentNode')||rawDom.get(ancestor,'insertionHost'))
            if(ancestor===child)throw new DOMException('Insertion would create a cycle','HierarchyRequestError');
        dom('insert',parent,child,before);return child;
    }
    define(Element.prototype,'insertAdjacentElement',{configurable:true,enumerable:true,writable:true,value:function(position,element){
        rawDom.get(this,'elementBrand');if(arguments.length<2)throw new TypeErrorImpl('Position and element required');
        position=string(position);rawDom.get(element,'elementBrand');
        return insertAdjacentNode(this,position,element);
    }});
    define(Element.prototype,'insertAdjacentText',{configurable:true,enumerable:true,writable:true,value:function(position,data){
        rawDom.get(this,'elementBrand');if(arguments.length<2)throw new TypeErrorImpl('Position and text required');
        position=string(position);data=string(data);
        const child=rawDom.create(rawDom.get(this,'ownerDocument'),3,'#text',data);
        insertAdjacentNode(this,position,child);
    }});
    const innerDescriptor=Object.getOwnPropertyDescriptor(Element.prototype,'innerHTML');
    define(Element.prototype,'innerHTML',{...innerDescriptor,set(value){
        rawDom.get(this,'elementBrand');dom('set',this,'innerHTML',value===null?'':string(value));
    }});
    const dialogs=new WeakMap();let ToggleEventImpl;
    function dialogState(node){
        htmlElementBrand(node,'dialog');let value=dialogs.get(node);
        if(!value)dialogs.set(node,value={returnValue:'',previous:null,toggle:null});
        return value;
    }
    function dialogToggle(node,oldState,newState){
        const value=dialogState(node);
        if(value.toggle){clearTimeout(value.toggle.task);oldState=value.toggle.oldState;}
        const record={oldState,newState,task:null};value.toggle=record;
        record.task=setTimeout(()=>{
            if(value.toggle!==record)return;value.toggle=null;
            const event=new ToggleEventImpl('toggle',{oldState:record.oldState,newState:record.newState});
            eventState(event).isTrusted=true;dispatch(node,event);
        },0);
    }
    function dialogBefore(node,oldState,newState,cancelable){
        const event=new ToggleEventImpl('beforetoggle',{oldState,newState,cancelable});
        eventState(event).isTrusted=true;return dispatch(node,event);
    }
    function descendant(node,ancestor){
        for(;node;){
            if(node===ancestor)return true;
            const parent=rawDom.get(node,'parentNode');
            node=parent || (rawDom.get(node,'nodeType')===11?rawDom.get(node,'shadowHost'):null);
        }
        return false;
    }
    function dialogFocus(node){
        const target=rawDom.dialogFocus(node);
        if(target)dom('focus',target);
    }
    function modalReady(node){
        const status=rawDom.dialogPrepare(node);
        if(status===1)return false;
        if(status===2)throw new DOMException('Dialog is already open nonmodally','InvalidStateError');
        if(status===3)throw new DOMException('Dialog is not connected to the active document','InvalidStateError');
        if(status===4)throw new RangeError('Native dialog top-layer capacity exceeded');
        return true;
    }
    class HTMLDialogElement extends HTMLElement {
        constructor(){return customElementsBridge.construct(new.target,HTMLDialogElement);}
        get open(){dialogState(this);return reflectedAttr(this,'open')!==null;}
        set open(value){dialogState(this);reflectedAttr(this,'open',value?'':null);}
        get returnValue(){return dialogState(this).returnValue;}
        set returnValue(value){dialogState(this).returnValue=string(value);}
        show(){
            const value=dialogState(this);
            if(rawDom.dialogModal(this))throw new DOMException('Dialog is already modal','InvalidStateError');
            if(reflectedAttr(this,'open')!==null)return;
            if(!dialogBefore(this,'closed','open',true) || reflectedAttr(this,'open')!==null)return;
            dialogToggle(this,'closed','open');reflectedAttr(this,'open','');
            value.previous=rawDom.get(rawDom.get(this,'ownerDocument'),'activeElement');dialogFocus(this);
        }
        showModal(){
            const value=dialogState(this);if(!modalReady(this))return;
            if(!dialogBefore(this,'closed','open',true) || reflectedAttr(this,'open')!==null)return;
            if(!rawDom.get(this,'isConnected') || rawDom.get(this,'ownerDocument')!==document)return;
            if(!modalReady(this))return;
            const previous=rawDom.get(rawDom.get(this,'ownerDocument'),'activeElement');
            // Reflection keeps MutationObserver/CE reactions on the ordinary
            // DOM path. Native state commits before any focusing or painting.
            reflectedAttr(this,'open','');
            if(!rawDom.dialogEnter(this)){
                reflectedAttr(this,'open',null);
                throw new DOMException('Dialog changed during modal opening','InvalidStateError');
            }
            value.previous=previous;dialogToggle(this,'closed','open');dialogFocus(this);
        }
        close(result=undefined){
            const value=dialogState(this),hasResult=arguments.length>0 && result!==undefined;
            if(hasResult)result=string(result);
            if(reflectedAttr(this,'open')===null)return;
            dialogBefore(this,'open','closed',false);
            if(reflectedAttr(this,'open')===null)return;
            const wasModal=rawDom.dialogModal(this);
            const nativePrevious=wasModal?rawDom.dialogLeave(this):null;
            dialogToggle(this,'open','closed');reflectedAttr(this,'open',null);
            if(hasResult)value.returnValue=result;
            const owner=rawDom.get(this,'ownerDocument'),active=rawDom.get(owner,'activeElement'),previous=wasModal?nativePrevious:value.previous;
            value.previous=null;
            if(previous && rawDom.get(previous,'isConnected') && (wasModal || descendant(active,this)))dom('focus',previous);
            else if(wasModal && descendant(active,this))dom('blur',active);
            if(wasModal && reflectedAttr(this,'open')===null){
                const remaining=rawDom.get(owner,'activeElement');
                if(descendant(remaining,this))dom('blur',remaining);
            }
            setTimeout(()=>{const event=new Event('close');eventState(event).isTrusted=true;dispatch(this,event);},0);
        }
        requestClose(result=undefined){
            dialogState(this);const hasResult=arguments.length>0 && result!==undefined;
            if(hasResult)result=string(result);if(reflectedAttr(this,'open')===null)return;
            if(!rawDom.get(this,'isConnected') || rawDom.get(this,'ownerDocument')!==document)return;
            const event=new Event('cancel',{cancelable:true});eventState(event).isTrusted=true;
            if(dispatch(this,event))call(closeDialog,this,hasResult?[result]:[]);
        }
    }
    for(const name of ['open','returnValue','show','showModal','close','requestClose'])
        define(HTMLDialogElement.prototype,name,{...Object.getOwnPropertyDescriptor(HTMLDialogElement.prototype,name),enumerable:true});
    const closeDialog=HTMLDialogElement.prototype.close,requestCloseDialog=HTMLDialogElement.prototype.requestClose;
    function reflect(C,tag,property,attribute=property,convert=string){
        define(C.prototype,property,{configurable:true,enumerable:true,
            get(){htmlElementBrand(this,tag);return convert(reflectedAttr(this,attribute)||'');},
            set(value){htmlElementBrand(this,tag);reflectedAttr(this,attribute,convert(value));}});
    }
    function enumerated(C,tag,property,attribute,known,fallback){
        define(C.prototype,property,{configurable:true,enumerable:true,get(){
            htmlElementBrand(this,tag);const value=reflectedAttr(this,attribute);
            if(value===null)return fallback;
            const keyword=lower(value);return known.includes(keyword)?keyword:fallback;
        },set(value){htmlElementBrand(this,tag);reflectedAttr(this,attribute,string(value));}});
    }
    function tokenAttribute(C,tag,property,attribute=property,supported=null){
        define(C.prototype,property,{configurable:true,enumerable:true,
            get(){htmlElementBrand(this,tag);return tokenListBridge.for(this,attribute,supported);},
            set(value){htmlElementBrand(this,tag);tokenListBridge.for(this,attribute,supported).value=value;}});
    }
    for(const [property,attribute] of [['name','name'],['httpEquiv','http-equiv'],['content','content'],['media','media'],['scheme','scheme']])
        reflect(HTMLMetaElement,'meta',property,attribute);
    for(const name of ['rel','media','integrity','hreflang','type','charset','rev','target'])reflect(HTMLLinkElement,'link',name);
    reflect(HTMLLinkElement,'link','imageSrcset','imagesrcset',scalar);
    reflect(HTMLLinkElement,'link','imageSizes','imagesizes');
    define(HTMLLinkElement.prototype,'href',{configurable:true,enumerable:true,
        get(){return elementURL.attribute(this,'link','href',false);},
        set(value){htmlElementBrand(this,'link');reflectedAttr(this,'href',scalar(value));}});
    // Unknown CORS keywords have the Anonymous state; only a missing attribute
    // returns null. Empty and ASCII-insensitive anonymous return "anonymous".
    define(HTMLLinkElement.prototype,'crossOrigin',{configurable:true,enumerable:true,get(){
        htmlElementBrand(this,'link');const value=reflectedAttr(this,'crossorigin');
        return value===null?null:lower(value)==='use-credentials'?'use-credentials':'anonymous';
    },set(value){htmlElementBrand(this,'link');reflectedAttr(this,'crossorigin',value==null?null:string(value));}});
    enumerated(HTMLLinkElement,'link','as','as',
        ['audio','document','embed','fetch','font','image','json','object','script','style','track','video','worker'],'');
    enumerated(HTMLLinkElement,'link','referrerPolicy','referrerpolicy',
        ['','no-referrer','no-referrer-when-downgrade','same-origin','origin','strict-origin','origin-when-cross-origin','strict-origin-when-cross-origin','unsafe-url'],'');
    enumerated(HTMLLinkElement,'link','fetchPriority','fetchpriority',['high','low','auto'],'auto');
    // supports() advertises native processing, never merely reflected strings:
    // stylesheet is supported; preload/modulepreload/icon/render blocking are not.
    tokenAttribute(HTMLLinkElement,'link','relList','rel',['stylesheet']);
    tokenAttribute(HTMLLinkElement,'link','sizes');
    tokenAttribute(HTMLLinkElement,'link','blocking','blocking',[]);
    define(HTMLLinkElement.prototype,'disabled',{configurable:true,enumerable:true,
        get(){htmlElementBrand(this,'link');return reflectedAttr(this,'disabled')!==null;},
        set(value){htmlElementBrand(this,'link');reflectedAttr(this,'disabled',value?'':null);}});
    for(const [C,tag] of [[HTMLAnchorElement,'a'],[HTMLAreaElement,'area']])tokenAttribute(C,tag,'relList','rel',[]);
    reflect(HTMLStyleElement,'style','media');reflect(HTMLStyleElement,'style','type');
    tokenAttribute(HTMLStyleElement,'style','blocking','blocking',[]);
    // Native stylesheet disabled state is installed by js_stylesheets.js.
    reflect(HTMLBaseElement,'base','target');
    define(HTMLBaseElement.prototype,'href',{configurable:true,enumerable:true,get(){
        htmlElementBrand(this,'base');const value=reflectedAttr(this,'href')||'';
        const fallback=rawDom.get(rawDom.get(this,'ownerDocument'),'URL');
        try{return call(hrefGet,new URLImpl(value,fallback),[]);}
        catch(error){if(error instanceof TypeErrorImpl)return scalar(value);throw error;}
    },set(value){htmlElementBrand(this,'base');reflectedAttr(this,'href',scalar(value));}});
    define(HTMLTitleElement.prototype,'text',{configurable:true,enumerable:true,get(){
        htmlElementBrand(this,'title');let text='';
        for(const child of rawDom.get(this,'childNodes'))if(rawDom.get(child,'nodeType')===3)text+=rawDom.get(child,'nodeValue');
        return text;
    },set(value){htmlElementBrand(this,'title');dom('set',this,'textContent',string(value));}});
    // The modern head interface has no additional IDL members. Its inherited
    // tree APIs operate on the native head, including parser-created nodes.
    const exports={HTMLMetaElement,HTMLLinkElement,HTMLStyleElement,HTMLBaseElement,HTMLTitleElement,HTMLHeadElement};
    const extraExports={HTMLHeadingElement,HTMLPictureElement,HTMLSourceElement,HTMLMenuElement,HTMLDialogElement,HTMLDivElement};
    for(const C of [...Object.values(exports),...Object.values(extraExports)])define(C.prototype,Symbol.toStringTag,{value:C.name,configurable:true});
    Object.assign(globalThis,exports,extraExports);
    return {revealHidden,nodeProtos:Object.values(exports).map(C=>C.prototype),extraNodeProtos:Object.values(extraExports).map(C=>C.prototype),exports:{...exports,...extraExports},
        initializeDialogs(C){if(ToggleEventImpl)throw new TypeErrorImpl('Dialog events already initialized');ToggleEventImpl=C;},
        requestClose(node){dialogState(node);call(requestCloseDialog,node,[]);},
        submit(node,result){
            dialogState(node);
            if(rawDom.get(node,'ownerDocument')!==document || !rawDom.get(node,'isConnected') || reflectedAttr(node,'open')===null)return false;
            // Native optional value has already been copied before author events.
            // A missing value preserves returnValue; an explicit empty one clears it.
            call(closeDialog,node,result===null?[]:[result]);
            return reflectedAttr(node,'open')===null;
        }};
})();
