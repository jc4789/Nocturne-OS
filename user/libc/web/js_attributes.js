/* Attr identities and every NamedNodeMap read use the renderer/parser's
   native attribute records. No JS attribute store or parallel DOM exists.
   DOM Standard: https://dom.spec.whatwg.org/#interface-attr */
const attributeBridge = (() => {
    const maps=new WeakMap(), elements=new WeakMap();
    const HTML='http://www.w3.org/1999/xhtml', XML='http://www.w3.org/XML/1998/namespace', XMLNS='http://www.w3.org/2000/xmlns/';
    const string=elementURL.string, get=(n,k)=>rawDom('get',n,k);
    const lower=s=>s.replace(/[A-Z]/g,c=>c.toLowerCase());
    function element(n){get(n,'elementBrand');return n;}
    function attribute(n){get(n,'attrBrand');return n;}
    function required(count,needed){if(count<needed)throw new TypeError('Missing attribute arguments');}
    function localValid(name){if(!name || /[\0\t\n\f\r /=>]/.test(name))throw new DOMException('Invalid attribute local name','InvalidCharacterError');}
    function limit(name){let bytes=0;for(const point of name){const c=point.codePointAt(0);bytes+=c<128?1:c<2048?2:c<65536?3:4;}
        if(bytes>=128)throw new DOMException('Attribute names of 128 bytes or more are not implemented','NotSupportedError');}
    function valueString(value){value=string(value);if(value.includes('\0'))throw new DOMException('NUL in native attribute values is not implemented','NotSupportedError');return value;}
    function namespace(value){if(value==null)return null;value=string(value);if(value.includes('\0'))throw new DOMException('NUL in native namespace URIs is not implemented','NotSupportedError');return value||null;}
    function extract(ns,name){
        ns=namespace(ns);name=string(name);let prefix=null,local=name;
        const colon=name.indexOf(':');
        if(colon>=0){prefix=name.slice(0,colon);local=name.slice(colon+1);
            if(!prefix || /[\0\t\n\f\r />]/.test(prefix))throw new DOMException('Invalid namespace prefix','InvalidCharacterError');}
        localValid(local);
        if(prefix!==null && ns===null || prefix==='xml' && ns!==XML ||
            (name==='xmlns' || prefix==='xmlns') && ns!==XMLNS || ns===XMLNS && name!=='xmlns' && prefix!=='xmlns')
            throw new DOMException('Namespace and prefix disagree','NamespaceError');
        limit(name);return {ns,prefix,local};
    }
    class Attr extends Node {
        constructor(){throw new TypeError('Illegal Attr constructor');}
        get namespaceURI(){attribute(this);return get(this,'namespaceURI');}
        get prefix(){attribute(this);return get(this,'prefix');}
        get localName(){attribute(this);return get(this,'localName');}
        get name(){attribute(this);return get(this,'attrName');}
        get value(){attribute(this);return get(this,'attrValue');}
        set value(value){attribute(this);dom('set',this,'attrValue',valueString(value));}
        get nodeValue(){attribute(this);return get(this,'attrValue');}
        set nodeValue(value){attribute(this);dom('set',this,'nodeValue',valueString(value==null?'':value));}
        get textContent(){attribute(this);return get(this,'attrValue');}
        set textContent(value){attribute(this);dom('set',this,'textContent',valueString(value==null?'':value));}
        get ownerElement(){attribute(this);return get(this,'attrOwner');}
        get specified(){attribute(this);return true;}
    }
    function owner(map){const el=elements.get(map);if(!el)throw new TypeError('Illegal NamedNodeMap receiver');return el;}
    function values(map){return rawDom('attrList',owner(map));}
    function setNode(el,a){
        element(el);attribute(a);
        const used=get(a,'attrOwner');
        if(used!==null && used!==el)throw new DOMException('Attribute is owned by another element','InUseAttributeError');
        return dom('attrSetNode',el,a);
    }
    function removeNode(el,a){
        element(el);attribute(a);
        if(get(a,'attrOwner')!==el)throw new DOMException('Attribute does not belong to this element','NotFoundError');
        return dom('attrRemoveNode',el,a);
    }
    function byName(el,name){return rawDom('attrNode',el,name);}
    function byNS(el,ns,local){return rawDom('attrNodeNS',el,ns,local);}
    class NamedNodeMap {
        constructor(){throw new TypeError('Illegal NamedNodeMap constructor');}
        get length(){return get(owner(this),'attributeNames').length;}
        item(index){required(arguments.length,1);return values(this)[index>>>0]||null;}
        getNamedItem(name){const el=owner(this);required(arguments.length,1);return byName(el,string(name));}
        getNamedItemNS(ns,local){const el=owner(this);required(arguments.length,2);return byNS(el,namespace(ns),string(local));}
        setNamedItem(a){required(arguments.length,1);return setNode(owner(this),a);}
        setNamedItemNS(a){required(arguments.length,1);return setNode(owner(this),a);}
        removeNamedItem(name){const el=owner(this);required(arguments.length,1);const a=byName(el,string(name));
            if(!a)throw new DOMException('Attribute not found','NotFoundError');return removeNode(el,a);}
        removeNamedItemNS(ns,local){const el=owner(this);required(arguments.length,2);const a=byNS(el,namespace(ns),string(local));
            if(!a)throw new DOMException('Attribute not found','NotFoundError');return removeNode(el,a);}
        *[Symbol.iterator](){owner(this);for(let i=0;i<this.length;i++)yield values(this)[i];}
    }
    const index=k=>typeof k==='string' && /^(0|[1-9][0-9]*)$/.test(k) && Number(k)<4294967295;
    function names(el){const a=get(el,'attributeNames');return [...new Set(a)].filter(k=>get(el,'namespaceURI')!==HTML || k===lower(k));}
    function map(el){
        element(el);let result=maps.get(el);if(result)return result;
        const target=Object.create(NamedNodeMap.prototype);
        const visible=(t,k)=>typeof k==='string' && !(k in t) && names(el).includes(k);
        const read=k=>index(k)?rawDom('attrList',el)[Number(k)]:byName(el,k);
        result=new Proxy(target,{
            get(t,k,r){if(index(k))return read(k);if(visible(t,k))return read(k);return Reflect.get(t,k,r);},
            has(t,k){return (index(k)?Number(k)<get(el,'attributeNames').length:visible(t,k))||Reflect.has(t,k);},
            ownKeys(t){const keys=get(el,'attributeNames').map((_,i)=>String(i));
                for(const k of names(el))if(!index(k) && visible(t,k))keys.push(k);
                return [...keys,...Reflect.ownKeys(t).filter(k=>!keys.includes(k))];},
            getOwnPropertyDescriptor(t,k){if(index(k) && Number(k)<get(el,'attributeNames').length || visible(t,k))
                return {value:read(k),writable:false,enumerable:index(k),configurable:true};return Reflect.getOwnPropertyDescriptor(t,k);},
            set(t,k,v,r){if(index(k)||visible(t,k))return false;return Reflect.set(t,k,v,r);},
            defineProperty(t,k,d){if(index(k)||visible(t,k))return false;return Reflect.defineProperty(t,k,d);},
            deleteProperty(t,k){if(index(k) && Number(k)<get(el,'attributeNames').length || visible(t,k))return false;return Reflect.deleteProperty(t,k);},
            preventExtensions(){return false;}
        });elements.set(target,el);elements.set(result,el);maps.set(el,result);return result;
    }
    Object.defineProperty(Element.prototype,'attributes',{enumerable:true,configurable:true,get(){return map(this);}});
    Element.prototype.hasAttributes=function(){element(this);return get(this,'attributeNames').length!==0;};
    Element.prototype.getAttributeNames=function(){element(this);return get(this,'attributeNames');};
    Element.prototype.getAttribute=function(name){element(this);required(arguments.length,1);return rawDom('attr',this,string(name));};
    Element.prototype.hasAttribute=function(name){element(this);required(arguments.length,1);return rawDom('attr',this,string(name))!==null;};
    Element.prototype.setAttribute=function(name,value){element(this);required(arguments.length,2);name=string(name);value=valueString(value);localValid(name);limit(name);dom('attr',this,name,value);};
    Element.prototype.removeAttribute=function(name){element(this);required(arguments.length,1);dom('attr',this,string(name),null);};
    Element.prototype.toggleAttribute=function(name,force){element(this);required(arguments.length,1);name=string(name);localValid(name);limit(name);
        const present=rawDom('attr',this,name)!==null, wanted=force===undefined?!present:!!force;
        if(wanted && !present)dom('attr',this,name,'');else if(!wanted && present)dom('attr',this,name,null);return wanted;};
    Element.prototype.getAttributeNode=function(name){element(this);required(arguments.length,1);return byName(this,string(name));};
    Element.prototype.getAttributeNodeNS=function(ns,local){element(this);required(arguments.length,2);return byNS(this,namespace(ns),string(local));};
    Element.prototype.setAttributeNode=function(a){required(arguments.length,1);return setNode(this,a);};
    Element.prototype.setAttributeNodeNS=Element.prototype.setAttributeNode;
    Element.prototype.removeAttributeNode=function(a){required(arguments.length,1);return removeNode(this,a);};
    Element.prototype.getAttributeNS=function(ns,local){element(this);required(arguments.length,2);return rawDom('attrNS',this,namespace(ns),string(local));};
    Element.prototype.hasAttributeNS=function(ns,local){element(this);required(arguments.length,2);return rawDom('attrNS',this,namespace(ns),string(local))!==null;};
    Element.prototype.setAttributeNS=function(ns,name,value){element(this);required(arguments.length,3);ns=namespace(ns);name=string(name);value=valueString(value);const x=extract(ns,name);dom('attrNS',this,x.ns,x.local,value,x.prefix);};
    Element.prototype.removeAttributeNS=function(ns,local){element(this);required(arguments.length,2);dom('attrNS',this,namespace(ns),string(local),null);};
    Document.prototype.createAttribute=function(name){documentBridge.brand(this);required(arguments.length,1);name=string(name);localValid(name);limit(name);
        return rawDom('attrCreate',this,null,null,lower(name));};
    Document.prototype.createAttributeNS=function(ns,name){documentBridge.brand(this);required(arguments.length,2);const x=extract(ns,name);return rawDom('attrCreate',this,x.ns,x.prefix,x.local);};
    for(const [C,name] of [[Attr,'Attr'],[NamedNodeMap,'NamedNodeMap']])Object.defineProperty(C.prototype,Symbol.toStringTag,{value:name,configurable:true});
    Object.defineProperty(Node.prototype,'ATTRIBUTE_NODE',{value:2,enumerable:true});
    Object.defineProperty(Node,'ATTRIBUTE_NODE',{value:2,enumerable:true});
    Object.assign(globalThis,{Attr,NamedNodeMap});

    /* Normalization for native CE/MO boundaries. Metadata comes from the native
       record, not overridable public getters or instanceof. */
    function mutation(op,node,key,value,extra){
        let el=node,a=null,name,ns=null;
        if(op==='attr'){a=byName(el,string(key));name=get(el,'namespaceURI')===HTML?lower(string(key)):string(key);}
        else if(op==='attrNS'){ns=key;name=string(value);a=byNS(el,ns,name);}
        else if(op==='attrSetNode' || op==='attrRemoveNode'){a=key;
            if(op==='attrSetNode' && get(a,'attrOwner')===el)return null;
            name=get(a,'localName');ns=get(a,'namespaceURI');
            if(op==='attrSetNode')a=byNS(el,ns,name);
        }else if(op==='set' && get(node,'nodeType')===2 && ['attrValue','nodeValue','textContent'].includes(key)){
            a=node;el=get(a,'attrOwner');if(!el)return null;
        }else if(op==='adopt' && key && get(key,'nodeType')===2){a=key;el=get(a,'attrOwner');if(!el)return null;}
        else return null;
        if(a){name=get(a,'localName');ns=get(a,'namespaceURI');}
        return {node:el,name,namespace:ns,oldValue:a?get(a,'attrValue'):null};
    }
    return {nodeProto:Attr.prototype,mutation};
})();
