/* HTML element IDL, including the legacy interfaces required by HTML.
 * Prototype order must agree with the native node interface table. */
const missingHTMLElementsBridge = (() => {
    'use strict';
    const define=Object.defineProperty, string=elementURL.string, scalar=elementURL.scalar;
    const TypeErrorImpl=TypeError, number=Number;
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
        getSVGDocument(){brand(this,'HTMLEmbedElement');return rawDom.get(this,'embeddedSVGDocument');}
    }
    class HTMLParamElement extends HTMLElement {constructor(){return customElementsBridge.construct(new.target,HTMLParamElement);}}
    class HTMLFontElement extends HTMLElement {constructor(){return customElementsBridge.construct(new.target,HTMLFontElement);}}
    class HTMLFrameSetElement extends HTMLElement {constructor(){return customElementsBridge.construct(new.target,HTMLFrameSetElement);}}
    class HTMLMarqueeElement extends HTMLElement {
        constructor(){return customElementsBridge.construct(new.target,HTMLMarqueeElement);}
        start(){brand(this,'HTMLMarqueeElement');dom('set',this,'marqueeRunning',true);}
        stop(){brand(this,'HTMLMarqueeElement');dom('set',this,'marqueeRunning',false);}
    }
    function brand(node,name){
        if(rawDom.get(node,'htmlInterface')!==name)throw new TypeErrorImpl('Illegal '+name+' receiver');
    }
    function reflect(C,property,attribute=property.toLowerCase(),nullEmpty=false){
        const name=C.name;
        define(C.prototype,property,{configurable:true,enumerable:true,
            get(){brand(this,name);return reflectedAttr(this,attribute)||'';},
            set(value){brand(this,name);reflectedAttr(this,attribute,nullEmpty && value===null?'':string(value));}});
    }
    function boolean(C,property,attribute=property.toLowerCase()){
        const name=C.name;
        define(C.prototype,property,{configurable:true,enumerable:true,
            get(){brand(this,name);return reflectedAttr(this,attribute)!==null;},
            set(value){brand(this,name);reflectedAttr(this,attribute,value?'':null);}});
    }
    function integer(C,property,unsigned=false,fallback=0){
        const name=C.name,attribute=property.toLowerCase();
        define(C.prototype,property,{configurable:true,enumerable:true,get(){
            brand(this,name);
            const match=/^[\t\n\f\r ]*([+-]?\d+)/.exec(reflectedAttr(this,attribute)||'');
            if(!match)return fallback;
            const value=number(match[1]);
            return value>=(unsigned?0:-2147483648) && value<=2147483647?(value===0?0:value):fallback;
        },set(value){
            brand(this,name);value=unsigned?value>>>0:value>>0;
            reflectedAttr(this,attribute,string(unsigned && value>2147483647?fallback:value));
        }});
    }
    function url(C,property){
        const name=C.name;
        define(C.prototype,property,{configurable:true,enumerable:true,
            get(){brand(this,name);return elementURL.attribute(this,rawDom.get(this,'localName'),property,false);},
            set(value){brand(this,name);reflectedAttr(this,property,scalar(value));}});
    }
    for(const property of ['text','link','vLink','aLink','bgColor'])reflect(HTMLBodyElement,property,property.toLowerCase(),true);
    reflect(HTMLBodyElement,'background');
    reflect(HTMLHtmlElement,'version');reflect(HTMLParagraphElement,'align');reflect(HTMLBRElement,'clear');
    for(const property of ['align','color','size','width'])reflect(HTMLHRElement,property);
    boolean(HTMLHRElement,'noShade');integer(HTMLPreElement,'width');
    url(HTMLQuoteElement,'cite');url(HTMLModElement,'cite');reflect(HTMLModElement,'dateTime');
    for(const C of [HTMLUListElement,HTMLDListElement,HTMLDirectoryElement])boolean(C,'compact');
    reflect(HTMLUListElement,'type');reflect(HTMLLegendElement,'align');
    define(HTMLLegendElement.prototype,'form',{configurable:true,enumerable:true,get(){
        brand(this,'HTMLLegendElement');const parent=rawDom.get(this,'parentNode');
        return parent && rawDom.get(parent,'nodeType')===1 && rawDom.get(parent,'namespaceURI')==='http://www.w3.org/1999/xhtml' &&
            rawDom.get(parent,'localName')==='fieldset'?rawDom.get(parent,'form:fieldset'):null;
    }});
    boolean(HTMLOptGroupElement,'disabled');reflect(HTMLOptGroupElement,'label');reflect(HTMLMapElement,'name');
    const mapAreas=new WeakMap();
    define(HTMLMapElement.prototype,'areas',{configurable:true,enumerable:true,get(){
        brand(this,'HTMLMapElement');let areas=mapAreas.get(this);
        if(!areas){
            const node=this;
            areas=collectionBridge.domHTML(node,()=>rawDom.query(node,'area',false).filter(n=>rawDom.get(n,'namespaceURI')==='http://www.w3.org/1999/xhtml'));
            mapAreas.set(node,areas);
        }
        return areas;
    }});
    url(HTMLEmbedElement,'src');
    for(const property of ['type','width','height','align','name'])reflect(HTMLEmbedElement,property);
    for(const property of ['name','value','type','valueType'])reflect(HTMLParamElement,property);
    reflect(HTMLFontElement,'color','color',true);reflect(HTMLFontElement,'face');reflect(HTMLFontElement,'size');
    reflect(HTMLFrameSetElement,'cols');reflect(HTMLFrameSetElement,'rows');
    for(const property of ['behavior','bgColor','direction','height','width'])reflect(HTMLMarqueeElement,property);
    integer(HTMLMarqueeElement,'hspace',true);integer(HTMLMarqueeElement,'vspace',true);
    integer(HTMLMarqueeElement,'scrollAmount',true,6);integer(HTMLMarqueeElement,'scrollDelay',true,85);
    boolean(HTMLMarqueeElement,'trueSpeed');
    function marqueeLoops(node){
        const match=/^[\t\n\f\r ]*([+-]?\d+)/.exec(reflectedAttr(node,'loop')||'');
        if(!match)return -1;
        const value=number(match[1]);return value>=1 && value<=2147483647?value:-1;
    }
    define(HTMLMarqueeElement.prototype,'loop',{configurable:true,enumerable:true,
        get(){brand(this,'HTMLMarqueeElement');return marqueeLoops(this);},
        set(value){
            brand(this,'HTMLMarqueeElement');value=value>>0;
            if((value>0 || value===-1) && value!==marqueeLoops(this))reflectedAttr(this,'loop',string(value));
        }});
    const exports={HTMLBodyElement,HTMLHtmlElement,HTMLParagraphElement,HTMLSpanElement,HTMLBRElement,HTMLHRElement,
        HTMLPreElement,HTMLQuoteElement,HTMLModElement,HTMLUListElement,HTMLDListElement,HTMLDirectoryElement,
        HTMLLegendElement,HTMLOptGroupElement,HTMLMapElement,HTMLEmbedElement,HTMLParamElement,HTMLFontElement,
        HTMLFrameSetElement,HTMLMarqueeElement};
    for(const [C,names] of [[HTMLEmbedElement,['getSVGDocument']],[HTMLMarqueeElement,['start','stop']]])
        for(const name of names)define(C.prototype,name,{enumerable:true});
    for(const [name,C] of Object.entries(exports)){
        define(C.prototype,Symbol.toStringTag,{value:name,configurable:true});
        for(const property of Object.getOwnPropertyNames(C.prototype)){
            const descriptor=Object.getOwnPropertyDescriptor(C.prototype,property);
            if(descriptor.get)define(descriptor.get,'name',{value:'get '+property,configurable:true});
            if(descriptor.set)define(descriptor.set,'name',{value:'set '+property,configurable:true});
        }
        define(globalThis,name,{value:C,writable:true,configurable:true});
    }
    return {exports,nodeProtos:Object.values(exports).map(C=>C.prototype)};
})();
