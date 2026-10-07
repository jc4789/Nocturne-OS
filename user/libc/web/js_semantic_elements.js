/* Semantics interfaces backed by native opaque node brands. The generic
 * sectioning/grouping elements correctly continue to use HTMLElement.
 * Prototype order: TIME, DATA, DETAILS, OL, LI in js.c's nodeProtos. */
const semanticElementsBridge = (() => {
    'use strict';
    const define=Object.defineProperty, string=elementURL.string, NumberImpl=Number;
    class HTMLTimeElement extends HTMLElement {constructor(){throw new TypeError('Illegal HTMLTimeElement constructor');}}
    class HTMLDataElement extends HTMLElement {constructor(){throw new TypeError('Illegal HTMLDataElement constructor');}}
    class HTMLDetailsElement extends HTMLElement {constructor(){throw new TypeError('Illegal HTMLDetailsElement constructor');}}
    class HTMLOListElement extends HTMLElement {constructor(){throw new TypeError('Illegal HTMLOListElement constructor');}}
    class HTMLLIElement extends HTMLElement {constructor(){throw new TypeError('Illegal HTMLLIElement constructor');}}
    function reflect(C,tag,property,attribute=property){
        define(C.prototype,property,{configurable:true,enumerable:true,
            get(){htmlElementBrand(this,tag);return reflectedAttr(this,attribute)||'';},
            set(value){htmlElementBrand(this,tag);reflectedAttr(this,attribute,string(value));}});
    }
    function boolean(C,tag,property,attribute=property){
        define(C.prototype,property,{configurable:true,enumerable:true,
            get(){htmlElementBrand(this,tag);return reflectedAttr(this,attribute)!==null;},
            set(value){htmlElementBrand(this,tag);reflectedAttr(this,attribute,value?'':null);}});
    }
    function integer(C,tag,property,fallback){
        define(C.prototype,property,{configurable:true,enumerable:true,get(){
            htmlElementBrand(this,tag);const value=reflectedAttr(this,property);
            if(value===null)return fallback;
            const m=/^[\t\n\f\r ]*([+-]?\d+)/.exec(value);
            if(!m)return fallback;
            const n=NumberImpl(m[1]);return n>=-2147483648 && n<=2147483647?n|0:fallback;
        },set(value){
            htmlElementBrand(this,tag);reflectedAttr(this,property,string(value>>0));
        }});
    }
    reflect(HTMLTimeElement,'time','dateTime','datetime');
    reflect(HTMLDataElement,'data','value');
    reflect(HTMLDetailsElement,'details','name');boolean(HTMLDetailsElement,'details','open');
    boolean(HTMLOListElement,'ol','reversed');integer(HTMLOListElement,'ol','start',1);reflect(HTMLOListElement,'ol','type');
    integer(HTMLLIElement,'li','value',0);
    const toggleValues=new WeakMap();
    class ToggleEvent extends Event {
        constructor(type,init={}){
            if(!arguments.length)throw new TypeError('ToggleEvent type required');
            if(init===null)init={};
            if(typeof init!=='object' && typeof init!=='function')throw new TypeError('ToggleEventInit dictionary required');
            super(string(type),init);
            const oldValue=init.oldState,newValue=init.newState,sourceValue=init.source;
            const oldState=oldValue===undefined?'':string(oldValue);
            const newState=newValue===undefined?'':string(newValue);
            const source=sourceValue===undefined?null:sourceValue;
            if(source!==null && (!rawDom('isNode',null,source) || rawDom('get',source,'nodeType')!==1))
                throw new TypeError('ToggleEvent source must be an Element');
            toggleValues.set(this,{oldState,newState,source});
        }
        get oldState(){const d=toggleValues.get(this);if(!d)throw new TypeError('ToggleEvent receiver required');return d.oldState;}
        get newState(){const d=toggleValues.get(this);if(!d)throw new TypeError('ToggleEvent receiver required');return d.newState;}
        get source(){const d=toggleValues.get(this);if(!d)throw new TypeError('ToggleEvent receiver required');return d.source;}
    }
    const exports={HTMLTimeElement,HTMLDataElement,HTMLDetailsElement,HTMLOListElement,HTMLLIElement};
    for(const C of [...Object.values(exports),ToggleEvent])define(C.prototype,Symbol.toStringTag,{value:C.name,configurable:true});
    Object.assign(globalThis,exports,{ToggleEvent});
    return {nodeProtos:Object.values(exports).map(C=>C.prototype),toggle(target,oldOpen,newOpen){
        const event=new ToggleEvent('toggle',{oldState:oldOpen?'open':'closed',newState:newOpen?'open':'closed'});
        event.isTrusted=true;dispatch(target,event);
    }};
})();
