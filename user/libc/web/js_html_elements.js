/* HTML element interfaces backed by native DOM nodes, not instanceof overrides.
 * Include after js_hyperlink.js and js_tokens.js. Native prototype registration
 * in js_bootstrap.js must match js.c. Sheet metadata is in js_stylesheets.js;
 * table interfaces are in js_html_tables.js.
 * https://html.spec.whatwg.org/multipage/indices.html#element-interfaces */
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
    class HTMLBodyElement extends HTMLElement {constructor(){return customElementsBridge.construct(new.target,HTMLBodyElement);}}
    class HTMLHtmlElement extends HTMLElement {constructor(){return customElementsBridge.construct(new.target,HTMLHtmlElement);}}
    class HTMLParagraphElement extends HTMLElement {constructor(){return customElementsBridge.construct(new.target,HTMLParagraphElement);}}
    class HTMLSpanElement extends HTMLElement {constructor(){return customElementsBridge.construct(new.target,HTMLSpanElement);}}
    class HTMLBRElement extends HTMLElement {constructor(){return customElementsBridge.construct(new.target,HTMLBRElement);}}
    class HTMLHRElement extends HTMLElement {constructor(){return customElementsBridge.construct(new.target,HTMLHRElement);}}
    class HTMLPreElement extends HTMLElement {constructor(){return customElementsBridge.construct(new.target,HTMLPreElement);}}
    class HTMLQuoteElement extends HTMLElement {constructor(){return customElementsBridge.construct(new.target,HTMLQuoteElement);}}
    class HTMLModElement extends HTMLElement {constructor(){return customElementsBridge.construct(new.target,HTMLModElement);}}
    class HTMLUListElement extends HTMLElement {constructor(){return customElementsBridge.construct(new.target,HTMLUListElement);}}
    class HTMLDListElement extends HTMLElement {constructor(){return customElementsBridge.construct(new.target,HTMLDListElement);}}
    class HTMLDirectoryElement extends HTMLElement {constructor(){return customElementsBridge.construct(new.target,HTMLDirectoryElement);}}
    class HTMLLegendElement extends HTMLElement {constructor(){return customElementsBridge.construct(new.target,HTMLLegendElement);}}
    class HTMLOptGroupElement extends HTMLElement {constructor(){return customElementsBridge.construct(new.target,HTMLOptGroupElement);}}
    class HTMLMapElement extends HTMLElement {constructor(){return customElementsBridge.construct(new.target,HTMLMapElement);}}
    class HTMLEmbedElement extends HTMLElement {
        constructor(){return customElementsBridge.construct(new.target,HTMLEmbedElement);}
        getSVGDocument(){interfaceBrand(this,'HTMLEmbedElement');return rawDom.get(this,'embeddedSVGDocument');}
    }
    class HTMLParamElement extends HTMLElement {constructor(){return customElementsBridge.construct(new.target,HTMLParamElement);}}
    class HTMLFontElement extends HTMLElement {constructor(){return customElementsBridge.construct(new.target,HTMLFontElement);}}
    class HTMLFrameSetElement extends HTMLElement {constructor(){return customElementsBridge.construct(new.target,HTMLFrameSetElement);}}
    class HTMLMarqueeElement extends HTMLElement {
        constructor(){return customElementsBridge.construct(new.target,HTMLMarqueeElement);}
        start(){interfaceBrand(this,'HTMLMarqueeElement');dom('set',this,'marqueeRunning',true);}
        stop(){interfaceBrand(this,'HTMLMarqueeElement');dom('set',this,'marqueeRunning',false);}
    }
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
    /* HTMLElement's own reflected attributes. name, type, src and the like belong to
       the interfaces that define them: here they would be setters on every element,
       a custom element's own fields among them. */
    for(const [property,attribute] of [['title','title'],['lang','lang'],['accessKey','accesskey']])
        define(HTMLElement.prototype,property,{configurable:true,enumerable:true,
            get(){htmlBrand(this);return reflectedAttr(this,attribute)||'';},
            set(value){htmlBrand(this);reflectedAttr(this,attribute,string(value));}});
    // Limited to only known values (ltr, rtl, auto), ASCII case-insensitively.
    define(HTMLElement.prototype,'dir',{configurable:true,enumerable:true,get(){
        htmlBrand(this);const value=lower(reflectedAttr(this,'dir')||'');
        return value==='ltr'||value==='rtl'||value==='auto'?value:'';
    },set(value){htmlBrand(this);reflectedAttr(this,'dir',string(value));}});
    define(HTMLElement.prototype,'inert',{configurable:true,enumerable:true,
        get(){htmlBrand(this);return reflectedAttr(this,'inert')!==null;},
        set(value){htmlBrand(this);reflectedAttr(this,'inert',value?'':null);}});
    // [Reflect, ReflectRange=(0, 8)] unsigned long: a non-negative integer clamped to the
    // range; the setter stores any value up to 2147483647 as it is (else 0).
    define(HTMLElement.prototype,'headingOffset',{configurable:true,enumerable:true,get(){
        htmlBrand(this);
        const value=reflectedAttr(this,'headingoffset'),match=value===null?null:/^[\t\n\f\r ]*([+-]?)(\d+)/.exec(value);
        if(!match || (match[1]==='-' && /[1-9]/.test(match[2])))return 0;
        const parsed=Number(match[2]);
        return parsed>8?8:parsed;
    },set(value){htmlBrand(this);value=value>>>0;reflectedAttr(this,'headingoffset',string(value<=2147483647?value:0));}});
    define(HTMLElement.prototype,'headingReset',{configurable:true,enumerable:true,
        get(){htmlBrand(this);return reflectedAttr(this,'headingreset')!==null;},
        set(value){htmlBrand(this);reflectedAttr(this,'headingreset',value?'':null);}});
    function htmlNamed(node,name){
        return !!node && rawDom.get(node,'nodeType')===1 && rawDom.get(node,'namespaceURI')==='http://www.w3.org/1999/xhtml' &&
            rawDom.get(node,'localName')===name;
    }
    // A summary is its details' summary when it is that details' first summary child.
    function detailsSummary(node){
        const parent=rawDom.get(node,'parentNode');
        if(!htmlNamed(parent,'details'))return false;
        for(let child=rawDom.get(parent,'firstChild');child;child=rawDom.get(child,'nextSibling'))
            if(htmlNamed(child,'summary'))return child===node;
        return false;
    }
    // Reflects tabindex as a long whose default is 0 for the elements that are focusable by
    // default, -1 otherwise.
    const focusableByDefault=new Set(['a','area','button','frame','iframe','input','object','select','textarea']);
    define(HTMLElement.prototype,'tabIndex',{configurable:true,enumerable:true,get(){
        htmlBrand(this);
        const value=reflectedAttr(this,'tabindex'),match=value===null?null:/^[\t\n\f\r ]*([+-]?\d+)/.exec(value);
        const parsed=match?Number(match[1]):NaN;
        if(parsed>=-2147483648 && parsed<=2147483647)return parsed|0;
        const name=rawDom.get(this,'localName');
        return focusableByDefault.has(name) || (name==='summary' && detailsSummary(this))?0:-1;
    },set(value){htmlBrand(this);reflectedAttr(this,'tabindex',string(value>>0));}});
    // The translation mode: yes or the empty string enable it, no disables it, and any other
    // value (or none) takes the parent element's mode; the root's is enabled.
    define(HTMLElement.prototype,'translate',{configurable:true,enumerable:true,get(){
        htmlBrand(this);
        for(let node=this;node && rawDom.get(node,'nodeType')===1;node=rawDom.get(node,'parentNode')){
            const value=rawDom.attrNS(node,null,'translate');
            if(value===null)continue;
            const mode=lower(value);
            if(mode==='yes'||mode==='')return true;
            if(mode==='no')return false;
        }
        return true;
    },set(value){htmlBrand(this);reflectedAttr(this,'translate',value?'yes':'no');}});
    // true and false are the attribute's states; otherwise an img, or an a with an href, is
    // draggable by default.
    define(HTMLElement.prototype,'draggable',{configurable:true,enumerable:true,get(){
        htmlBrand(this);const value=lower(reflectedAttr(this,'draggable')||'');
        if(value==='true')return true;
        if(value==='false')return false;
        const name=rawDom.get(this,'localName');
        return name==='img' || (name==='a' && reflectedAttr(this,'href')!==null);
    },set(value){htmlBrand(this);reflectedAttr(this,'draggable',value?'true':'false');}});
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
    // stylesheet and modulepreload are processed; preload/icon/render blocking are not.
    tokenAttribute(HTMLLinkElement,'link','relList','rel',['stylesheet','modulepreload']);
    tokenAttribute(HTMLLinkElement,'link','sizes');
    tokenAttribute(HTMLLinkElement,'link','blocking','blocking',[]);
    define(HTMLLinkElement.prototype,'disabled',{configurable:true,enumerable:true,
        get(){htmlElementBrand(this,'link');return reflectedAttr(this,'disabled')!==null;},
        set(value){htmlElementBrand(this,'link');reflectedAttr(this,'disabled',value?'':null);}});
    for(const [C,tag] of [[HTMLAnchorElement,'a'],[HTMLAreaElement,'area']])tokenAttribute(C,tag,'relList','rel',[]);
    reflect(HTMLFormElement,'form','rel');tokenAttribute(HTMLFormElement,'form','relList','rel',[]);
    reflect(HTMLStyleElement,'style','media');reflect(HTMLStyleElement,'style','type');
    tokenAttribute(HTMLStyleElement,'style','blocking','blocking',[]);
    // Native stylesheet disabled state is installed by js_stylesheets.js.
    reflect(HTMLBaseElement,'base','target');
    define(HTMLBaseElement.prototype,'href',{configurable:true,enumerable:true,get(){
        htmlElementBrand(this,'base');const value=reflectedAttr(this,'href')||'';
        // The URL parser with the document's character encoding, against its
        // fallback base URL.
        const fallback=rawDom.get(rawDom.get(this,'ownerDocument'),'URL');
        const url=urlBridge.encodingParse(value,fallback,rawDom.get(this,'characterSet'));
        return url?call(hrefGet,url,[]):scalar(value);
    },set(value){htmlElementBrand(this,'base');reflectedAttr(this,'href',scalar(value));}});
    define(HTMLTitleElement.prototype,'text',{configurable:true,enumerable:true,get(){
        htmlElementBrand(this,'title');let text='';
        for(const child of rawDom.get(this,'childNodes'))if(rawDom.get(child,'nodeType')===3)text+=rawDom.get(child,'nodeValue');
        return text;
    },set(value){htmlElementBrand(this,'title');dom('set',this,'textContent',string(value));}});
    // The modern head interface has no additional IDL members. Its inherited
    // tree APIs operate on the native head, including parser-created nodes.
    function interfaceBrand(node,name){
        if(rawDom.get(node,'htmlInterface')!==name)throw new TypeErrorImpl('Illegal '+name+' receiver');
    }
    function reflectInterface(C,property,attribute=property.toLowerCase(),nullEmpty=false){
        const name=C.name;
        define(C.prototype,property,{configurable:true,enumerable:true,
            get(){interfaceBrand(this,name);return reflectedAttr(this,attribute)||'';},
            set(value){interfaceBrand(this,name);reflectedAttr(this,attribute,nullEmpty && value===null?'':string(value));}});
    }
    function reflectBoolean(C,property,attribute=property.toLowerCase()){
        const name=C.name;
        define(C.prototype,property,{configurable:true,enumerable:true,
            get(){interfaceBrand(this,name);return reflectedAttr(this,attribute)!==null;},
            set(value){interfaceBrand(this,name);reflectedAttr(this,attribute,value?'':null);}});
    }
    const number=Number;
    function reflectInteger(C,property,unsigned=false,fallback=0){
        const name=C.name,attribute=property.toLowerCase();
        define(C.prototype,property,{configurable:true,enumerable:true,get(){
            interfaceBrand(this,name);
            const match=/^[\t\n\f\r ]*([+-]?\d+)/.exec(reflectedAttr(this,attribute)||'');
            if(!match)return fallback;
            const value=number(match[1]);
            return value>=(unsigned?0:-2147483648) && value<=2147483647?(value===0?0:value):fallback;
        },set(value){
            interfaceBrand(this,name);value=unsigned?value>>>0:value>>0;
            reflectedAttr(this,attribute,string(unsigned && value>2147483647?fallback:value));
        }});
    }
    function reflectURL(C,property){
        const name=C.name;
        define(C.prototype,property,{configurable:true,enumerable:true,
            get(){interfaceBrand(this,name);return elementURL.attribute(this,rawDom.get(this,'localName'),property,false);},
            set(value){interfaceBrand(this,name);reflectedAttr(this,property,scalar(value));}});
    }
    for(const property of ['text','link','vLink','aLink','bgColor'])reflectInterface(HTMLBodyElement,property,property.toLowerCase(),true);
    reflectInterface(HTMLBodyElement,'background');
    reflectInterface(HTMLHtmlElement,'version');reflectInterface(HTMLParagraphElement,'align');reflectInterface(HTMLBRElement,'clear');
    for(const property of ['align','color','size','width'])reflectInterface(HTMLHRElement,property);
    reflectBoolean(HTMLHRElement,'noShade');reflectInteger(HTMLPreElement,'width');
    reflectURL(HTMLQuoteElement,'cite');reflectURL(HTMLModElement,'cite');reflectInterface(HTMLModElement,'dateTime');
    for(const C of [HTMLUListElement,HTMLDListElement,HTMLDirectoryElement])reflectBoolean(C,'compact');
    reflectInterface(HTMLUListElement,'type');reflectInterface(HTMLLegendElement,'align');
    define(HTMLLegendElement.prototype,'form',{configurable:true,enumerable:true,get(){
        interfaceBrand(this,'HTMLLegendElement');const parent=rawDom.get(this,'parentNode');
        return parent && rawDom.get(parent,'nodeType')===1 && rawDom.get(parent,'namespaceURI')==='http://www.w3.org/1999/xhtml' &&
            rawDom.get(parent,'localName')==='fieldset'?rawDom.get(parent,'form:fieldset'):null;
    }});
    reflectBoolean(HTMLOptGroupElement,'disabled');reflectInterface(HTMLOptGroupElement,'label');reflectInterface(HTMLMapElement,'name');
    // HTML template: the declarative shadow root's content attributes.
    enumerated(HTMLTemplateElement,'template','shadowRootMode','shadowrootmode',['open','closed'],'');
    enumerated(HTMLTemplateElement,'template','shadowRootSlotAssignment','shadowrootslotassignment',['named','manual'],'named');
    for(const property of ['shadowRootDelegatesFocus','shadowRootSerializable','shadowRootClonable'])reflectBoolean(HTMLTemplateElement,property);
    reflectInterface(HTMLTemplateElement,'shadowRootCustomElementRegistry');
    const mapAreas=new WeakMap();
    define(HTMLMapElement.prototype,'areas',{configurable:true,enumerable:true,get(){
        interfaceBrand(this,'HTMLMapElement');let areas=mapAreas.get(this);
        if(!areas){
            const node=this;
            areas=collectionBridge.domHTML(node,()=>rawDom.query(node,'area',false).filter(n=>rawDom.get(n,'namespaceURI')==='http://www.w3.org/1999/xhtml'));
            mapAreas.set(node,areas);
        }
        return areas;
    }});
    reflectURL(HTMLEmbedElement,'src');
    for(const property of ['name','type'])reflectInterface(HTMLObjectElement,property);
    for(const property of ['name','type'])reflectInterface(HTMLAnchorElement,property);
    reflectInterface(HTMLImageElement,'name');
    for(const property of ['type','width','height','align','name'])reflectInterface(HTMLEmbedElement,property);
    for(const property of ['name','value','type','valueType'])reflectInterface(HTMLParamElement,property);
    reflectInterface(HTMLFontElement,'color','color',true);reflectInterface(HTMLFontElement,'face');reflectInterface(HTMLFontElement,'size');
    reflectInterface(HTMLFrameSetElement,'cols');reflectInterface(HTMLFrameSetElement,'rows');
    for(const property of ['behavior','bgColor','direction','height','width'])reflectInterface(HTMLMarqueeElement,property);
    reflectInteger(HTMLMarqueeElement,'hspace',true);reflectInteger(HTMLMarqueeElement,'vspace',true);
    reflectInteger(HTMLMarqueeElement,'scrollAmount',true,6);reflectInteger(HTMLMarqueeElement,'scrollDelay',true,85);
    reflectBoolean(HTMLMarqueeElement,'trueSpeed');
    function marqueeLoops(node){
        const match=/^[\t\n\f\r ]*([+-]?\d+)/.exec(reflectedAttr(node,'loop')||'');
        if(!match)return -1;
        const value=number(match[1]);return value>=1 && value<=2147483647?value:-1;
    }
    define(HTMLMarqueeElement.prototype,'loop',{configurable:true,enumerable:true,
        get(){interfaceBrand(this,'HTMLMarqueeElement');return marqueeLoops(this);},
        set(value){
            interfaceBrand(this,'HTMLMarqueeElement');value=value>>0;
            if((value>0 || value===-1) && value!==marqueeLoops(this))reflectedAttr(this,'loop',string(value));
        }});
    const interfaces={HTMLBodyElement,HTMLHtmlElement,HTMLParagraphElement,HTMLSpanElement,HTMLBRElement,HTMLHRElement,
        HTMLPreElement,HTMLQuoteElement,HTMLModElement,HTMLUListElement,HTMLDListElement,HTMLDirectoryElement,
        HTMLLegendElement,HTMLOptGroupElement,HTMLMapElement,HTMLEmbedElement,HTMLParamElement,HTMLFontElement,
        HTMLFrameSetElement,HTMLMarqueeElement};
    for(const [C,names] of [[HTMLEmbedElement,['getSVGDocument']],[HTMLMarqueeElement,['start','stop']]])
        for(const name of names)define(C.prototype,name,{enumerable:true});
    for(const [name,C] of Object.entries(interfaces)){
        define(C.prototype,Symbol.toStringTag,{value:name,configurable:true});
        for(const property of Object.getOwnPropertyNames(C.prototype)){
            const descriptor=Object.getOwnPropertyDescriptor(C.prototype,property);
            if(descriptor.get)define(descriptor.get,'name',{value:'get '+property,configurable:true});
            if(descriptor.set)define(descriptor.set,'name',{value:'set '+property,configurable:true});
        }
        define(globalThis,name,{value:C,writable:true,configurable:true});
    }
    const exports={HTMLMetaElement,HTMLLinkElement,HTMLStyleElement,HTMLBaseElement,HTMLTitleElement,HTMLHeadElement};
    const extraExports={HTMLHeadingElement,HTMLPictureElement,HTMLSourceElement,HTMLMenuElement,HTMLDialogElement,HTMLDivElement};
    for(const C of [...Object.values(exports),...Object.values(extraExports)])define(C.prototype,Symbol.toStringTag,{value:C.name,configurable:true});
    Object.assign(globalThis,exports,extraExports);
    return {revealHidden,nodeProtos:Object.values(exports).map(C=>C.prototype),extraNodeProtos:Object.values(extraExports).map(C=>C.prototype),exports:{...exports,...extraExports,...interfaces},
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
