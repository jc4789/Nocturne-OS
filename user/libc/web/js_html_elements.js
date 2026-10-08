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
    class HTMLMetaElement extends HTMLElement {constructor(){throw new TypeErrorImpl('Illegal HTMLMetaElement constructor');}}
    class HTMLLinkElement extends HTMLElement {constructor(){throw new TypeErrorImpl('Illegal HTMLLinkElement constructor');}}
    class HTMLStyleElement extends HTMLElement {constructor(){throw new TypeErrorImpl('Illegal HTMLStyleElement constructor');}}
    class HTMLBaseElement extends HTMLElement {constructor(){throw new TypeErrorImpl('Illegal HTMLBaseElement constructor');}}
    class HTMLTitleElement extends HTMLElement {constructor(){throw new TypeErrorImpl('Illegal HTMLTitleElement constructor');}}
    class HTMLHeadElement extends HTMLElement {constructor(){throw new TypeErrorImpl('Illegal HTMLHeadElement constructor');}}
    class HTMLHeadingElement extends HTMLElement {constructor(){throw new TypeErrorImpl('Illegal HTMLHeadingElement constructor');}}
    class HTMLPictureElement extends HTMLElement {constructor(){throw new TypeErrorImpl('Illegal HTMLPictureElement constructor');}}
    class HTMLSourceElement extends HTMLElement {constructor(){throw new TypeErrorImpl('Illegal HTMLSourceElement constructor');}}
    class HTMLMenuElement extends HTMLElement {constructor(){throw new TypeErrorImpl('Illegal HTMLMenuElement constructor');}}
    function headingBrand(node){
        const tag=rawDom('get',node,'localName');
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
        rawDom('get',node,'elementBrand');
        if(rawDom('get',node,'namespaceURI')!=='http://www.w3.org/1999/xhtml')
            throw new TypeErrorImpl('HTMLElement receiver required');
    }
    define(HTMLElement.prototype,'hidden',{configurable:true,enumerable:true,get(){
        htmlBrand(this);const value=reflectedAttr(this,'hidden');
        return value===null?false:lower(value)==='until-found'?'until-found':true;
    },set(value){
        htmlBrand(this);
        // nullable boolean/number/DOMString union。objectとBigIntはDOMStringへ変換。
        if(value!=null && typeof value!=='boolean' && typeof value!=='number' && typeof value!=='string')value=string(value);
        reflectedAttr(this,'hidden',typeof value==='string' && lower(value)==='until-found'?'until-found':value?'':null);
    }});
    function adjacent(node,position){
        rawDom('get',node,'elementBrand');position=lower(string(position));
        let parent,before;
        if(position==='beforebegin' || position==='afterend'){
            parent=rawDom('get',node,'parentNode');
            if(!parent || rawDom('get',parent,'nodeType')===9)
                throw new DOMException('No parent insertion point','NoModificationAllowedError');
            before=position==='beforebegin'?node:rawDom('get',node,'nextSibling');
        }else if(position==='afterbegin' || position==='beforeend'){
            parent=node;before=position==='afterbegin'?rawDom('get',node,'firstChild'):null;
        }else throw new DOMException('Invalid adjacent position','SyntaxError');
        let context=parent;
        if(rawDom('get',context,'nodeType')!==1 || rawDom('get',context,'namespaceURI')==='http://www.w3.org/1999/xhtml' && rawDom('get',context,'localName')==='html')
            context=rawDom('create',rawDom('get',node,'ownerDocument'),1,'body','');
        return {parent,before,context};
    }
    define(Element.prototype,'insertAdjacentHTML',{configurable:true,enumerable:true,writable:true,value:function(position,markup){
        rawDom('get',this,'elementBrand');if(arguments.length<2)throw new TypeErrorImpl('Position and markup required');
        position=string(position);markup=string(markup);const point=adjacent(this,position);
        const fragment=rawDom('parseFragment',point.context,markup);
        dom('insert',point.parent,fragment,point.before);
    }});
    const innerDescriptor=Object.getOwnPropertyDescriptor(Element.prototype,'innerHTML');
    define(Element.prototype,'innerHTML',{...innerDescriptor,set(value){
        rawDom('get',this,'elementBrand');dom('set',this,'innerHTML',value===null?'':string(value));
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
            event.isTrusted=true;dispatch(node,event);
        },0);
    }
    function dialogBefore(node,oldState,newState,cancelable){
        const event=new ToggleEventImpl('beforetoggle',{oldState,newState,cancelable});
        event.isTrusted=true;return dispatch(node,event);
    }
    function descendant(node,ancestor){
        for(;node;){
            if(node===ancestor)return true;
            const parent=rawDom('get',node,'parentNode');
            node=parent || (rawDom('get',node,'nodeType')===11?rawDom('get',node,'shadowHost'):null);
        }
        return false;
    }
    function dialogFocus(node){
        const target=rawDom('dialogFocus',node);
        if(target)dom('focus',target);
    }
    function modalReady(node){
        const status=rawDom('dialogPrepare',node);
        if(status===1)return false;
        if(status===2)throw new DOMException('Dialog is already open nonmodally','InvalidStateError');
        if(status===3)throw new DOMException('Dialog is not connected to the active document','InvalidStateError');
        if(status===4)throw new RangeError('Native dialog top-layer capacity exceeded');
        return true;
    }
    class HTMLDialogElement extends HTMLElement {
        constructor(){throw new TypeErrorImpl('Illegal HTMLDialogElement constructor');}
        get open(){dialogState(this);return reflectedAttr(this,'open')!==null;}
        set open(value){dialogState(this);reflectedAttr(this,'open',value?'':null);}
        get returnValue(){return dialogState(this).returnValue;}
        set returnValue(value){dialogState(this).returnValue=string(value);}
        show(){
            const value=dialogState(this);
            if(rawDom('dialogModal',this))throw new DOMException('Dialog is already modal','InvalidStateError');
            if(reflectedAttr(this,'open')!==null)return;
            if(!dialogBefore(this,'closed','open',true) || reflectedAttr(this,'open')!==null)return;
            dialogToggle(this,'closed','open');reflectedAttr(this,'open','');
            value.previous=rawDom('get',rawDom('get',this,'ownerDocument'),'activeElement');dialogFocus(this);
        }
        showModal(){
            const value=dialogState(this);if(!modalReady(this))return;
            if(!dialogBefore(this,'closed','open',true) || reflectedAttr(this,'open')!==null)return;
            if(!rawDom('get',this,'isConnected') || rawDom('get',this,'ownerDocument')!==document)return;
            if(!modalReady(this))return;
            const previous=rawDom('get',rawDom('get',this,'ownerDocument'),'activeElement');
            // Reflection keeps MutationObserver/CE reactions on the ordinary
            // DOM path. Native state commits before any focusing or painting.
            reflectedAttr(this,'open','');
            if(!rawDom('dialogEnter',this)){
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
            const wasModal=rawDom('dialogModal',this);
            const nativePrevious=wasModal?rawDom('dialogLeave',this):null;
            dialogToggle(this,'open','closed');reflectedAttr(this,'open',null);
            if(hasResult)value.returnValue=result;
            const owner=rawDom('get',this,'ownerDocument'),active=rawDom('get',owner,'activeElement'),previous=wasModal?nativePrevious:value.previous;
            value.previous=null;
            if(previous && rawDom('get',previous,'isConnected') && (wasModal || descendant(active,this)))dom('focus',previous);
            else if(wasModal && descendant(active,this))dom('blur',active);
            if(wasModal && reflectedAttr(this,'open')===null){
                const remaining=rawDom('get',owner,'activeElement');
                if(descendant(remaining,this))dom('blur',remaining);
            }
            setTimeout(()=>{const event=new Event('close');event.isTrusted=true;dispatch(this,event);},0);
        }
        requestClose(result=undefined){
            dialogState(this);const hasResult=arguments.length>0 && result!==undefined;
            if(hasResult)result=string(result);if(reflectedAttr(this,'open')===null)return;
            if(!rawDom('get',this,'isConnected') || rawDom('get',this,'ownerDocument')!==document)return;
            const event=new Event('cancel',{cancelable:true});event.isTrusted=true;
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
        const fallback=rawDom('get',rawDom('get',this,'ownerDocument'),'URL');
        try{return call(hrefGet,new URLImpl(value,fallback),[]);}
        catch(error){if(error instanceof TypeErrorImpl)return scalar(value);throw error;}
    },set(value){htmlElementBrand(this,'base');reflectedAttr(this,'href',scalar(value));}});
    define(HTMLTitleElement.prototype,'text',{configurable:true,enumerable:true,get(){
        htmlElementBrand(this,'title');let text='';
        for(const child of rawDom('get',this,'childNodes'))if(rawDom('get',child,'nodeType')===3)text+=rawDom('get',child,'nodeValue');
        return text;
    },set(value){htmlElementBrand(this,'title');dom('set',this,'textContent',string(value));}});
    // The modern head interface has no additional IDL members. Its inherited
    // tree APIs operate on the native head, including parser-created nodes.
    const exports={HTMLMetaElement,HTMLLinkElement,HTMLStyleElement,HTMLBaseElement,HTMLTitleElement,HTMLHeadElement};
    const extraExports={HTMLHeadingElement,HTMLPictureElement,HTMLSourceElement,HTMLMenuElement,HTMLDialogElement};
    for(const C of [...Object.values(exports),...Object.values(extraExports)])define(C.prototype,Symbol.toStringTag,{value:C.name,configurable:true});
    Object.assign(globalThis,exports,extraExports);
    return {nodeProtos:Object.values(exports).map(C=>C.prototype),extraNodeProtos:Object.values(extraExports).map(C=>C.prototype),exports:{...exports,...extraExports},
        initializeDialogs(C){if(ToggleEventImpl)throw new TypeErrorImpl('Dialog events already initialized');ToggleEventImpl=C;},
        requestClose(node){dialogState(node);call(requestCloseDialog,node,[]);},
        submit(node,result){
            dialogState(node);
            if(rawDom('get',node,'ownerDocument')!==document || !rawDom('get',node,'isConnected') || reflectedAttr(node,'open')===null)return false;
            // Native optional value has already been copied before author events.
            // A missing value preserves returnValue; an explicit empty one clears it.
            call(closeDialog,node,result===null?[]:[result]);
            return reflectedAttr(node,'open')===null;
        }};
})();
