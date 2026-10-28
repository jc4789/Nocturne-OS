/* DOM collections backed by the native tree, not Array aliases. The objects
   are js.c's native collection class (js_collections_native.h), whose indexed
   and named properties are Web IDL's legacy platform objects. This file owns
   the interfaces' members and what each collection reads. */
const collectionBridge = (() => {
    const native=host.collection, make=native.make, nativeItem=native.item, nativeNamed=native.named;
    const childLists=new WeakMap(), elementLists=new WeakMap();
    const apply=Reflect.apply, filter=Array.prototype.filter, define=Object.defineProperty, isPrototypeOf=Object.prototype.isPrototypeOf;
    // The interface a collection implements, for its members' receiver checks
    // (js_collections_native.h's collection_family).
    const OTHER=0, NODE_LIST=1, HTML_COLLECTION=2;
    class NodeList {
        constructor(){throw new TypeError('Illegal NodeList constructor');}
        item(i){return nativeItem(this,NODE_LIST,arguments.length,i);}
        forEach(callback,thisArg){if(typeof callback!=='function')throw new TypeError('Expected callback');const n=this.length;for(let i=0;i<n;i++){const node=nativeItem(this,NODE_LIST,1,i);if(node!==null)callback.call(thisArg,node,i,this);}}
        *keys(){for(let i=0;i<this.length;i++)yield i;}
        *values(){for(let i=0;i<this.length;i++)yield this[i];}
        *entries(){for(let i=0;i<this.length;i++)yield [i,this[i]];}
        [Symbol.iterator](){return this.values();}
    }
    class HTMLCollection {
        constructor(){throw new TypeError('Illegal HTMLCollection constructor');}
        item(i){return nativeItem(this,HTML_COLLECTION,arguments.length,i);}
        namedItem(name){return nativeNamed(this,arguments.length,name);}
        *[Symbol.iterator](){for(let i=0;i<this.length;i++)yield this[i];}
    }
    // Each interface's length getter is its own native function.
    define(NodeList.prototype,'length',{get:native.lengthGetter(NODE_LIST),configurable:true});
    define(HTMLCollection.prototype,'length',{get:native.lengthGetter(HTML_COLLECTION),configurable:true});
    for(const C of [NodeList,HTMLCollection])define(C.prototype,Symbol.toStringTag,{value:C.name,configurable:true});
    // source: a static list's array, or a reader of the current items. A root
    // node makes the reader a DOM-tree one, read again only after the root's
    // owner Document or its DOM revision changed; control state (options, form,
    // radio groups) can change without a DOM revision, so those read live.
    // html: an HTMLCollection (or an interface inheriting it), with named
    // properties; otherwise a NodeList unless the prototype is of another
    // interface (DOMRectList).
    function collection(source,html=false,prototype=null,indexedSetter=null,namedGetter=null,root=undefined){
        const proto=prototype || (html?HTMLCollection.prototype:NodeList.prototype);
        const family=html?HTML_COLLECTION:proto===NodeList.prototype || apply(isPrototypeOf,NodeList.prototype,[proto])?NODE_LIST:OTHER;
        return make(proto,family,source,root,indexedSetter,namedGetter);
    }
    function children(n,elements){rawDom.get(n,'nodeType');const cache=elements?elementLists:childLists;let c=cache.get(n);if(!c){c=collection(()=>{const a=rawDom.get(n,'childNodes');return elements?apply(filter,a,[x=>rawDom.get(x,'nodeType')===1]):a;},elements,null,null,null,n);cache.set(n,c);}return c;}
    Object.assign(globalThis,{NodeList,HTMLCollection});
    return {list:a=>collection(a),live:read=>collection(read),html:read=>collection(read,true),domLive:(node,read)=>collection(read,false,null,null,null,node),domHTML:(node,read)=>collection(read,true,null,null,null,node),options:(read,prototype,setter)=>collection(read,true,prototype,setter),
        typed:(read,prototype)=>collection(read,false,prototype),form:(read,prototype,lookup)=>collection(read,true,prototype,null,lookup),children};
})();
