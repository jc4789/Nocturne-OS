/* Constraint validation is computed from native controls. This layer only
 * supplies WebIDL brands, live objects, reflection and submission events. */
const formValidationBridge=(() => {
    const define=Object.defineProperty,create=Object.create;
    const validityOwners=new WeakMap(),validityObjects=new WeakMap(),submitters=new WeakMap(),submitting=new WeakSet();
    const controls=[['input',HTMLInputElement],['button',HTMLButtonElement],['select',HTMLSelectElement],
        ['textarea',HTMLTextAreaElement],['fieldset',HTMLFieldSetElement],['object',HTMLObjectElement],['output',HTMLOutputElement]];
    const flags=['valueMissing','typeMismatch','patternMismatch','tooLong','tooShort','rangeUnderflow','rangeOverflow','stepMismatch','badInput','customError'];
    class ValidityState {constructor(){throw new TypeError('Illegal ValidityState constructor');}}
    const owner=object=>{
        const node=validityOwners.get(object);
        if(!node)throw new TypeError('Illegal ValidityState receiver');
        return node;
    };
    for(let index=0;index<flags.length;index++)define(ValidityState.prototype,flags[index],{
        configurable:true,enumerable:true,get(){return !!(dom('validation',owner(this),'state')&(1<<index));}
    });
    define(ValidityState.prototype,'valid',{configurable:true,enumerable:true,get(){return dom('validation',owner(this),'state')===0;}});
    define(ValidityState.prototype,Symbol.toStringTag,{value:'ValidityState',configurable:true});
    function validity(node){
        let object=validityObjects.get(node);
        if(!object){object=create(ValidityState.prototype);validityObjects.set(node,object);validityOwners.set(object,node);}
        return object;
    }
    function getter(prototype,property,get){define(prototype,property,{get,configurable:true,enumerable:true});}
    function method(prototype,property,value){define(prototype,property,{value,writable:true,configurable:true,enumerable:true});}
    function boolean(prototype,tag,property,attribute){define(prototype,property,{configurable:true,enumerable:true,
        get(){htmlElementBrand(this,tag);return reflectedAttr(this,attribute)!==null;},
        set(value){htmlElementBrand(this,tag);reflectedAttr(this,attribute,value?'':null);}});}
    function string(prototype,tag,property,attribute){define(prototype,property,{configurable:true,enumerable:true,
        get(){htmlElementBrand(this,tag);return reflectedAttr(this,attribute)||'';},
        set(value){htmlElementBrand(this,tag);reflectedAttr(this,attribute,elementURL.string(value));}});}
    const number=Number;
    function integer(value){return (+value)|0;}
    function length(prototype,tag,property,attribute){define(prototype,property,{configurable:true,enumerable:true,
        get(){
            htmlElementBrand(this,tag);const text=reflectedAttr(this,attribute),match=text===null?null:/^[\t\n\f\r ]*\+?([0-9]+)/.exec(text);
            const value=match?number(match[1]):-1;return value>=0&&value<=2147483647?value:-1;
        },set(value){
            htmlElementBrand(this,tag);value=integer(value);
            if(value<0)throw new DOMException('Length must not be negative','IndexSizeError');
            reflectedAttr(this,attribute,''+value);
        }});}
    for(const [tag,C] of controls){
        getter(C.prototype,'willValidate',function(){htmlElementBrand(this,tag);return dom('validation',this,'will');});
        getter(C.prototype,'validity',function(){htmlElementBrand(this,tag);dom('validation',this,'state');return validity(this);});
        getter(C.prototype,'validationMessage',function(){htmlElementBrand(this,tag);return dom('validation',this,'message');});
        method(C.prototype,'setCustomValidity',function(message){
            htmlElementBrand(this,tag);if(!arguments.length)throw new TypeError('Message required');
            dom('validation',this,'custom',elementURL.string(message));
        });
        method(C.prototype,'checkValidity',function(){htmlElementBrand(this,tag);return dom('validation',this,'check');});
        method(C.prototype,'reportValidity',function(){htmlElementBrand(this,tag);return dom('validation',this,'report');});
    }
    for(const [tag,C] of [['input',HTMLInputElement],['select',HTMLSelectElement],['textarea',HTMLTextAreaElement]])boolean(C.prototype,tag,'required','required');
    for(const [tag,C] of [['input',HTMLInputElement],['textarea',HTMLTextAreaElement]]){
        boolean(C.prototype,tag,'readOnly','readonly');length(C.prototype,tag,'minLength','minlength');length(C.prototype,tag,'maxLength','maxlength');
    }
    boolean(HTMLInputElement.prototype,'input','multiple','multiple');boolean(HTMLSelectElement.prototype,'select','multiple','multiple');
    define(HTMLSelectElement.prototype,'size',{configurable:true,enumerable:true,
        get(){htmlElementBrand(this,'select');const text=reflectedAttr(this,'size'),match=text===null?null:/^[\t\n\f\r ]*\+?([0-9]+)/.exec(text);const value=match?number(match[1]):0;return value>0&&value<=4294967295?value:0;},
        set(value){htmlElementBrand(this,'select');reflectedAttr(this,'size',''+((+value)>>>0));}});
    getter(HTMLSelectElement.prototype,'type',function(){htmlElementBrand(this,'select');return reflectedAttr(this,'multiple')!==null?'select-multiple':'select-one';});
    string(HTMLInputElement.prototype,'input','pattern','pattern');
    boolean(HTMLInputElement.prototype,'input','formNoValidate','formnovalidate');boolean(HTMLButtonElement.prototype,'button','formNoValidate','formnovalidate');
    method(HTMLFormElement.prototype,'checkValidity',function(){htmlElementBrand(this,'form');return dom('validation',this,'formCheck');});
    method(HTMLFormElement.prototype,'reportValidity',function(){htmlElementBrand(this,'form');return dom('validation',this,'formReport');});

    class SubmitEvent extends Event {
        constructor(type,init={}){
            if(!arguments.length)throw new TypeError('Event type required');
            init=init==null?{}:init;
            if(typeof init!=='object'&&typeof init!=='function')throw new TypeError('Event dictionary required');
            super(elementURL.string(type),init);
            const value=init.submitter,submitter=value==null?null:value;
            if(submitter!==null&&(rawDom.get(submitter,'nodeType')!==1||rawDom.get(submitter,'namespaceURI')!=='http://www.w3.org/1999/xhtml'))throw new TypeError('submitter must be an HTMLElement');
            submitters.set(this,submitter);
        }
        get submitter(){if(!submitters.has(this))throw new TypeError('Illegal SubmitEvent receiver');return submitters.get(this);}
    }
    define(SubmitEvent.prototype,Symbol.toStringTag,{value:'SubmitEvent',configurable:true});
    method(HTMLFormElement.prototype,'requestSubmit',function(submitter){
        htmlElementBrand(this,'form');
        if(submitter!=null){
            const tag=rawDom.get(submitter,'localName');
            if(!dom('validation',submitter,'submitButton'))throw new TypeError('submitter must be a submit button');
            if(rawDom.get(submitter,'form:'+tag)!==this)throw new DOMException('submitter belongs to another form','NotFoundError');
        }
        if(host.sandboxFlags(this)&32)return;
        if(submitting.has(this))return;
        submitting.add(this);
        try{
            if(!dom('validation',submitter||this,'submission'))return;
            const event=new SubmitEvent('submit',{bubbles:true,cancelable:true,submitter:submitter||null});
            if(dispatch(this,event))dom('submit',submitter||this);
        }finally{submitting.delete(this);}
    });
    Object.assign(globalThis,{ValidityState,SubmitEvent});
    return {ValidityState,SubmitEvent,validity};
})();
