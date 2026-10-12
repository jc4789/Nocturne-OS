/* DOM collections backed by the native tree, not Array aliases. */
const collectionBridge = (() => {
    const slots=new WeakMap(), childLists=new WeakMap(), elementLists=new WeakMap(),domReaders=new WeakSet();
    const apply=Reflect.apply, find=Array.prototype.find, map=Array.prototype.map, filter=Array.prototype.filter;
    const MapImpl=Map,SetImpl=Set,define=Object.defineProperty,mapGet=Map.prototype.get,mapHas=Map.prototype.has,mapSet=Map.prototype.set,
        setHas=Set.prototype.has,setAdd=Set.prototype.add,readerHas=WeakSet.prototype.has,readerAdd=WeakSet.prototype.add;
    function pushOwn(array,value){define(array,array.length,{value,writable:true,enumerable:true,configurable:true});}
    const index=k=>typeof k==='string' && /^(0|[1-9][0-9]*)$/.test(k) && Number(k)<4294967295;
    function source(receiver){const s=slots.get(receiver);if(!s)throw new TypeError('Illegal collection receiver');return s;}
    function values(receiver){return source(receiver).read();}
    // Only DOM-tree queries use this cache. Control state (options/form/radio
    // groups) can change without a DOM revision, so generic reads stay live.
    function domRead(node,read){
        let owner=null,revision=null,cached=null;
        const live=()=>{
            const version=rawDom.get(node,'collectionVersion');
            if(cached && owner===version[0] && revision===version[1])return cached;
            const next=read(); // Publish only after a successful native read.
            owner=version[0];revision=version[1];cached=next;return next;
        };
        apply(readerAdd,domReaders,[live]);return live;
    }
    function named(a,name){if(!name)return null;return apply(find,a,[n=>reflectedAttr(n,'id')===name ||
        (rawDom.get(n,'namespaceURI')==='http://www.w3.org/1999/xhtml' && reflectedAttr(n,'name')===name)])||null;}
    class NodeList {
        constructor(){throw new TypeError('Illegal NodeList constructor');}
        get length(){return values(this).length;}
        item(i){if(!arguments.length)throw new TypeError('item requires index');return values(this)[Number(i)>>>0]||null;}
        forEach(callback,thisArg){if(typeof callback!=='function')throw new TypeError('Expected callback');const n=this.length;for(let i=0;i<n;i++){const a=values(this);if(i<a.length)callback.call(thisArg,a[i],i,this);}}
        *keys(){for(let i=0;i<this.length;i++)yield i;}
        *values(){for(let i=0;i<this.length;i++)yield this[i];}
        *entries(){for(let i=0;i<this.length;i++)yield [i,this[i]];}
        [Symbol.iterator](){return this.values();}
    }
    class HTMLCollection {
        constructor(){throw new TypeError('Illegal HTMLCollection constructor');}
        get length(){return values(this).length;}
        item(i){if(!arguments.length)throw new TypeError('item requires index');return values(this)[Number(i)>>>0]||null;}
        namedItem(name){if(!arguments.length)throw new TypeError('namedItem requires name');return source(this).named(String(name));}
        *[Symbol.iterator](){for(let i=0;i<this.length;i++)yield this[i];}
    }
    function make(read,html=false,prototype=null,indexedSetter=null,namedLookup=null){
        const target=Object.create(prototype || (html?HTMLCollection.prototype:NodeList.prototype)),slot={read,html};
        const versioned=apply(readerHas,domReaders,[read]);let nameValues=null,nameIndex=null,nameKeys=null;
        function names(a){
            if(versioned&&a===nameValues&&nameIndex)return nameIndex;
            const next=new MapImpl(),keys=[];
            const add=(key,node)=>{if(!key||apply(mapHas,next,[key]))return;apply(mapSet,next,[key,node]);pushOwn(keys,key);};
            for(let i=0;i<a.length;i++){const node=a[i];add(reflectedAttr(node,'id'),node);
                if(rawDom.get(node,'namespaceURI')==='http://www.w3.org/1999/xhtml')add(reflectedAttr(node,'name'),node);}
            // DOM readers replace their private snapshot when the native owner
            // or revision changes, including ID/name attributes and adoption.
            // Keep only one generation; control-state readers never use this
            // cache because their state may change without a DOM revision.
            nameValues=a;nameIndex=next;nameKeys=keys;return next;
        }
        const defaultLookup=name=>{if(!name)return null;const a=read();return versioned?apply(mapGet,names(a),[name])||null:named(a,name);};
        slot.named=defaultLookup;
        const lookup=name=>namedLookup?namedLookup(name):defaultLookup(name);
        const proxy=new Proxy(target,{
            get(t,k,r){if(index(k))return read()[Number(k)];if(html && typeof k==='string' && !(k in t)){const n=lookup(k);if(n)return n;}return Reflect.get(t,k,r);},
            has(t,k){return (index(k)?Number(k)<read().length:html && typeof k==='string' && !!lookup(k)) || Reflect.has(t,k);},
            ownKeys(t){const a=read(),keys=[],seen=new SetImpl();const add=key=>{if(apply(setHas,seen,[key]))return;apply(setAdd,seen,[key]);pushOwn(keys,key);};
                for(let i=0;i<a.length;i++)add(String(i));
                if(html){names(a);for(let i=0;i<nameKeys.length;i++){const key=nameKeys[i];if(!index(key)&&!(key in t))add(key);}}
                const own=Reflect.ownKeys(t);for(let i=0;i<own.length;i++)add(own[i]);return keys;},
            getOwnPropertyDescriptor(t,k){if(index(k)){const a=read();if(Number(k)<a.length)return {value:a[Number(k)],writable:!!indexedSetter,enumerable:true,configurable:true};}else if(html && typeof k==='string' && !(k in t)){const n=lookup(k);if(n)return {value:n,writable:false,enumerable:false,configurable:true};}return Reflect.getOwnPropertyDescriptor(t,k);},
            set(t,k,v,r){if(index(k)){if(!indexedSetter)return false;indexedSetter(Number(k),v);return true;}if(html && typeof k==='string' && !(k in t) && lookup(k))return false;return Reflect.set(t,k,v,r);},
            defineProperty(t,k,d){if(index(k)){if(!indexedSetter || !('value' in d) || 'get' in d || 'set' in d)return false;indexedSetter(Number(k),d.value);return true;}return Reflect.defineProperty(t,k,d);},
            deleteProperty(t,k){if(index(k) && Number(k)<read().length)return false;return Reflect.deleteProperty(t,k);},
            preventExtensions(){return false;}
        });slots.set(target,slot);slots.set(proxy,slot);return proxy;
    }
    function children(n,elements){rawDom.get(n,'nodeType');const cache=elements?elementLists:childLists;let c=cache.get(n);if(!c){c=make(domRead(n,()=>{const a=rawDom.get(n,'childNodes');return elements?apply(filter,a,[x=>rawDom.get(x,'nodeType')===1]):a;}),elements);cache.set(n,c);}return c;}
    for(const C of [NodeList,HTMLCollection])Object.defineProperty(C.prototype,Symbol.toStringTag,{value:C.name,configurable:true});
    Object.assign(globalThis,{NodeList,HTMLCollection});
    return {list:a=>make(()=>a),live:read=>make(read),html:read=>make(read,true),domLive:(node,read)=>make(domRead(node,read)),domHTML:(node,read)=>make(domRead(node,read),true),options:(read,prototype,setter)=>make(read,true,prototype,setter),
        typed:(read,prototype)=>make(read,false,prototype),form:(read,prototype,lookup)=>make(read,true,prototype,null,lookup),children};
})();
