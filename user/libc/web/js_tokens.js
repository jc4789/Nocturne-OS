/* Native-attribute backed ordered token sets. Include after DOMException.
 * Only platform bindings use for(node, attribute, supportedTokens). A missing
 * vocabulary is distinct from an empty vocabulary (supports() throws/false).
 * https://dom.spec.whatwg.org/#interface-domtokenlist
 * https://webidl.spec.whatwg.org/#es-iterable */
const tokenListBridge = (() => {
    'use strict';
    const slots=new WeakMap(), lists=new WeakMap();
    const StringImpl=String, TypeErrorImpl=TypeError, ExceptionImpl=DOMException;
    const define=Object.defineProperty, descriptor=Object.getOwnPropertyDescriptor;
    const ownKeys=Reflect.ownKeys, reflectGet=Reflect.get, reflectSet=Reflect.set;
    const reflectDefine=Reflect.defineProperty, reflectDelete=Reflect.deleteProperty;
    const index=k=>typeof k==='string' && /^(0|[1-9][0-9]*)$/.test(k) && +k<4294967295;
    function slot(receiver){const s=slots.get(receiver);if(!s)throw new TypeErrorImpl('Illegal DOMTokenList receiver');return s;}
    function string(value){if(typeof value==='symbol')throw new TypeErrorImpl('Cannot convert Symbol to DOMString');return StringImpl(value);}
    function validate(token){
        if(token==='')throw new ExceptionImpl('The token is empty','SyntaxError');
        if(/[\t\n\f\r ]/.test(token))throw new ExceptionImpl('The token contains ASCII whitespace','InvalidCharacterError');
    }
    function text(s){return reflectedAttr(s.node,s.attribute)||'';}
    function tokens(s){
        const value=text(s);
        if(value!==s.text){
            s.text=value;s.tokens=[];
            for(const token of value.match(/[^\t\n\f\r ]+/g)||[])if(!s.tokens.includes(token))s.tokens.push(token);
        }
        return s.tokens;
    }
    function update(s,current){
        if(current.length || reflectedAttr(s.node,s.attribute)!==null)reflectedAttr(s.node,s.attribute,current.join(' '));
    }
    class DOMTokenList {
        constructor(){throw new TypeErrorImpl('Illegal DOMTokenList constructor');}
        get length(){return tokens(slot(this)).length;}
        item(index){const s=slot(this);if(!arguments.length)throw new TypeErrorImpl('Missing token index');index=index>>>0;return tokens(s)[index]??null;}
        contains(token){const s=slot(this);if(!arguments.length)throw new TypeErrorImpl('Missing token');token=string(token);return tokens(s).includes(token);}
        add(...values){
            const s=slot(this), converted=values.map(string);for(const token of converted)validate(token);
            const current=tokens(s).slice();for(const token of converted)if(!current.includes(token))current.push(token);update(s,current);
        }
        remove(...values){
            const s=slot(this), converted=values.map(string);for(const token of converted)validate(token);
            update(s,tokens(s).filter(token=>!converted.includes(token)));
        }
        toggle(token,force){
            const s=slot(this);if(!arguments.length)throw new TypeErrorImpl('Missing token');token=string(token);validate(token);
            const current=tokens(s).slice(),has=current.includes(token);
            if(has){if(force===undefined || !force){update(s,current.filter(value=>value!==token));return false;}return true;}
            if(force===undefined || !!force){current.push(token);update(s,current);return true;}return false;
        }
        replace(token,replacement){
            const s=slot(this);if(arguments.length<2)throw new TypeErrorImpl('Missing token/replacement');
            token=string(token);replacement=string(replacement);
            if(token==='' || replacement==='')throw new ExceptionImpl('The token is empty','SyntaxError');
            validate(token);validate(replacement);
            const current=tokens(s);if(!current.includes(token))return false;
            const result=[];let replaced=false;
            for(const value of current){
                if(value===token || value===replacement){if(!replaced){result.push(replacement);replaced=true;}}
                else result.push(value);
            }
            update(s,result);return true;
        }
        supports(token){
            const s=slot(this);if(!arguments.length)throw new TypeErrorImpl('Missing token');token=string(token);
            if(s.supported===null)throw new TypeErrorImpl('This attribute has no supported-token vocabulary');
            return s.supported.includes(token.replace(/[A-Z]/g,c=>StringImpl.fromCharCode(c.charCodeAt(0)+32)));
        }
        get value(){return text(slot(this));}
        set value(value){const s=slot(this);value=string(value);reflectedAttr(s.node,s.attribute,value);}
        toString(){return text(slot(this));}
    }
    // Web IDL value iterables use the actual generic Array functions. Their
    // indexed reads are live, and forEach fixes length before invoking callbacks.
    for(const key of ['keys','values','entries','forEach'])define(DOMTokenList.prototype,key,
        {value:Array.prototype[key],writable:true,enumerable:true,configurable:true});
    define(DOMTokenList.prototype,Symbol.iterator,{value:Array.prototype.values,writable:true,configurable:true});
    define(DOMTokenList.prototype,Symbol.toStringTag,{value:'DOMTokenList',configurable:true});
    for(const key of ['length','item','contains','add','remove','toggle','replace','supports','value','toString']){
        const d=descriptor(DOMTokenList.prototype,key);d.enumerable=true;define(DOMTokenList.prototype,key,d);
    }
    function make(node,attribute,supported){
        const target=Object.create(DOMTokenList.prototype),s={node,attribute,supported,text:null,tokens:[]};
        const read=()=>tokens(s);
        const proxy=new Proxy(target,{
            get(t,k,r){if(index(k)){const a=read();if(+k<a.length)return a[+k];}return reflectGet(t,k,r);},
            has(t,k){return index(k)?+k<read().length || Reflect.has(t,k):Reflect.has(t,k);},
            ownKeys(t){const keys=read().map((_,i)=>StringImpl(i));return [...keys,...ownKeys(t).filter(k=>!keys.includes(k))];},
            getOwnPropertyDescriptor(t,k){if(index(k)){const a=read();if(+k<a.length)return {value:a[+k],writable:false,enumerable:true,configurable:true};}return descriptor(t,k);},
            set(t,k,v,r){return index(k)?false:reflectSet(t,k,v,r);},
            defineProperty(t,k,d){return index(k)?false:reflectDefine(t,k,d);},
            deleteProperty(t,k){return index(k) && +k<read().length?false:reflectDelete(t,k);},
            preventExtensions(){return false;}
        });
        slots.set(target,s);slots.set(proxy,s);return proxy;
    }
    return {DOMTokenList,for(node,attribute,supported=null){
        if(rawDom('get',node,'nodeType')!==1)throw new TypeErrorImpl('DOMTokenList requires an element');
        let cache=lists.get(node);if(!cache){cache=new Map();lists.set(node,cache);}
        let list=cache.get(attribute);if(!list){list=make(node,attribute,supported);cache.set(attribute,list);}return list;
    }};
})();
