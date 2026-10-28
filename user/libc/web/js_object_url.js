/* FileAPI's blob URL store (https://w3c.github.io/FileAPI/#url-model, consulted 2026-10-28): this
   realm's blob URL entries of real Blob and MediaSource objects, never native file authority. One
   definition for a Window (js_bootstrap.js) and a Worker (js_worker_runtime.js); the realm passes
     origin()   the ASCII serialization of its environment's origin ("null" when opaque),
     policy()   the policy container's headers a blob: document or Worker inherits from the entry's
                environment (null for none).
   URL.createObjectURL and URL.revokeObjectURL are installed on the realm's URL. resolve(url) is the
   URL parser's "resolve a blob URL" (the entry, null for none), obtain(entry) "obtain a blob
   object" for one of this realm's entries; Fetch (js_fetch.js) runs scheme fetch over them. Every
   entry here was made by this realm's environment, so the same-partition check of "obtain a blob
   object" holds for it; a Worker's request for a URL this store does not hold is its parent
   document's to resolve (js.c, worker_blob). */
function objectURLInterfaces(realm) {
    const URLType=URL, apply=Reflect.apply, define=Object.defineProperty, setPrototype=Object.setPrototypeOf;
    const urls=new Map(), get=Map.prototype.get, put=Map.prototype.set, remove=Map.prototype.delete;
    const randomUUID=crypto.randomUUID, cryptoObject=crypto, indexOf=String.prototype.indexOf, strSlice=String.prototype.slice;
    const urlGetter=name=>Object.getOwnPropertyDescriptor(URLType.prototype,name).get, href=urlGetter('href'), protocol=urlGetter('protocol');
    let mediaBrand=()=>false;
    const string=v=>{if(typeof v==='symbol')throw new TypeError('Cannot convert Symbol to string');return String(v);};
    /* "Resolve a blob URL": the store's key is the URL serialized without its fragment. */
    function key(value){const url=string(value),hash=apply(indexOf,url,['#']);return hash<0?url:apply(strSlice,url,[0,hash]);}
    function lookup(value){return apply(get,urls,[key(value)]);}
    /* "Add an entry to the blob URL store": a new blob URL ("generate a new blob URL": blob:, the
       origin, /, a random UUID) for object and this realm's environment. */
    function createObjectURL(value){
        const blob=blobBridge.brand(value),media=mediaBrand(value);
        if(!blob&&!media)throw new TypeError('Blob or MediaSource required');
        let id;do id='blob:'+realm.origin()+'/'+apply(randomUUID,cryptoObject,[]);while(lookup(id));
        apply(put,urls,[id,setPrototype({value,blob,policyHeaders:realm.policy()},null)]);return id;
    }
    /* revokeObjectURL: the parsed URL's entry, when it is a blob: URL, "removed from the blob URL
       store" by the URL's whole serialization, so a URL with a fragment removes nothing. Every
       entry here is this realm's environment's, so the same-partition check holds. An entry a URL
       record already resolved keeps its object. */
    function revokeObjectURL(value){
        const text=string(value);let record;
        try{record=new URLType(text);}catch(_){return;}
        if(apply(protocol,record,[])!=='blob:')return;
        apply(remove,urls,[apply(href,record,[])]);
    }
    define(URLType,'createObjectURL',{value:createObjectURL,writable:true,configurable:true,enumerable:true});
    define(URLType,'revokeObjectURL',{value:revokeObjectURL,writable:true,configurable:true,enumerable:true});
    return {
        registerMediaSource(fn){mediaBrand=fn;},
        mediaSource(value){const entry=lookup(value);return entry&&!entry.blob?entry.value:null;},
        blob(value){const entry=lookup(value);return entry&&entry.blob?entry.value:null;},
        policy(value){const entry=lookup(value);return entry&&entry.blob?entry.policyHeaders:null;},
        resolve(value){return lookup(value)||null;},
        /* "Obtain a blob object": the entry's Blob, null for a MediaSource's entry. */
        obtain(entry){return entry&&entry.blob?entry.value:null;}
    };
}
