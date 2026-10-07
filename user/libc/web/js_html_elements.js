/* Metadata interfaces backed by native DOM nodes, not instanceof overrides.
 * Include after js_hyperlink.js and js_tokens.js. Prototype order is shared
 * with NP_META..NP_HEAD in js.c. CSSStyleSheet/CSSOM are not implemented here.
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
    // disabled needs native stylesheet state, not a fabricated CSSStyleSheet or
    // the generic HTMLElement disabled-attribute getter. No sheet object is added.
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
    for(const C of Object.values(exports))define(C.prototype,Symbol.toStringTag,{value:C.name,configurable:true});
    Object.assign(globalThis,exports);
    return {nodeProtos:Object.values(exports).map(C=>C.prototype),exports};
})();
