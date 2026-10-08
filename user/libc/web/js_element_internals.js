/* Private brands and WebIDL conversion; native node owns FACE state/value. */
const elementInternalsBridge=(()=>{
    'use strict';
    const define=Object.defineProperty,create=Object.create,weakGet=WeakMap.prototype.get,weakSet=WeakMap.prototype.set;
    const call=Reflect.apply,owners=new WeakMap(),attached=new WeakMap(),labelLists=new WeakMap();
    const StringImpl=String,TypeErr=TypeError,DomErr=DOMException,replace=String.prototype.replace;
    const flags=['valueMissing','typeMismatch','patternMismatch','tooLong','tooShort','rangeUnderflow','rangeOverflow','stepMismatch','badInput','customError'];
    const token={};
    function text(value){if(typeof value==='symbol')throw new TypeErr('Cannot convert Symbol to DOMString');return StringImpl(value);}
    function owner(value,form=false){
        const n=call(weakGet,owners,[value]);if(!n)throw new TypeErr('Illegal ElementInternals receiver');
        if(form&&!rawDom('face',n,'associated'))throw new DomErr('Not a form-associated custom element','NotSupportedError');
        return n;
    }
    function pack(value){return formDataBridge.faceValue(value);}
    class ElementInternals{
        constructor(key,node){if(key!==token)throw new TypeErr('Illegal ElementInternals constructor');call(weakSet,owners,[this,node]);}
        get shadowRoot(){return rawDom('get',owner(this),'shadowRoot');}
        get form(){return rawDom('face',owner(this,true),'form');}
        get willValidate(){return rawDom('validation',owner(this,true),'will');}
        get validity(){return formValidationBridge.validity(owner(this,true));}
        get validationMessage(){return rawDom('face',owner(this,true),'message');}
        get labels(){const n=owner(this,true);let list=call(weakGet,labelLists,[this]);if(!list){list=collectionBridge.live(()=>rawDom('face',n,'labels'));call(weakSet,labelLists,[this,list]);}return list;}
        setFormValue(value,state){
            const n=owner(this,true);if(!arguments.length)throw new TypeErr('Value required');
            const valuePacket=pack(value);
            if(arguments.length<2||state===undefined)rawDom('face',n,'value',valuePacket);
            else rawDom('face',n,'value',valuePacket,pack(state));
        }
        setValidity(dictionary,message='',anchor=undefined){
            const n=owner(this,true);if(!arguments.length)throw new TypeErr('Flags required');
            if(dictionary!=null&&typeof dictionary!=='object'&&typeof dictionary!=='function')throw new TypeErr('ValidityStateFlags dictionary required');
            // WebIDL dictionary member getters run once, in lexicographic order.
            let bits=0;const order=[8,9,2,6,5,7,3,4,1,0];
            for(let i=0;i<order.length;i++){const index=order[i],v=dictionary==null?undefined:dictionary[flags[index]];if(v!==undefined&&!!v)bits|=1<<index;}
            message=call(replace,text(message),[/\r\n?/g,'\n']);
            if(bits&&!message)throw new TypeErr('Invalid flags require a nonempty message');
            if(anchor!==undefined&&(rawDom('get',anchor,'nodeType')!==1||rawDom('get',anchor,'namespaceURI')!=='http://www.w3.org/1999/xhtml'))throw new TypeErr('Anchor must be an HTMLElement');
            if(rawDom('face',n,'validity',bits,message,anchor)===2)throw new DomErr('Anchor must be a shadow-including descendant','NotFoundError');
        }
        checkValidity(){return rawDom('validation',owner(this,true),'check');}
        reportValidity(){return rawDom('validation',owner(this,true),'report');}
    }
    define(ElementInternals.prototype,Symbol.toStringTag,{value:'ElementInternals',configurable:true});
    define(HTMLElement.prototype,'attachInternals',{configurable:true,writable:true,value:function(){
        const info=customElementsBridge.internalsInfo(this);
        if(!info||info.disableInternals)throw new DomErr('Internals unavailable for this custom element','NotSupportedError');
        if(call(weakGet,attached,[this]))throw new DomErr('Internals already attached','NotSupportedError');
        rawDom('face',this,'attach');
        const object=new ElementInternals(token,this);call(weakSet,attached,[this,object]);return object;
    }});
    define(globalThis,'ElementInternals',{value:ElementInternals,writable:true,configurable:true});
    return {ElementInternals};
})();
