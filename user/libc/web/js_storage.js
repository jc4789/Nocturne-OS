/* Storage over the browser-owned native backend. No JS shadow map or page path.
 * Named properties are live; public constructor/prototype replacement cannot
 * change native origin selection or the private brand. */
const storageBridge=(()=>{
    'use strict';
    const URLImpl=URL, StringImpl=String, TypeErrorImpl=TypeError, DOMExceptionImpl=DOMException;
    const call=Reflect.apply, getOwn=Reflect.getOwnPropertyDescriptor, reflectGet=Reflect.get;
    const reflectHas=Reflect.has, reflectSet=Reflect.set, reflectDefine=Reflect.defineProperty, reflectDelete=Reflect.deleteProperty;
    const reflectOwn=Reflect.ownKeys, define=Object.defineProperty, create=Object.create;
    const URLOrigin=Object.getOwnPropertyDescriptor(URLImpl.prototype,'origin').get;
    const URLProtocol=Object.getOwnPropertyDescriptor(URLImpl.prototype,'protocol').get;
    const records=new WeakMap(), mapGet=WeakMap.prototype.get, mapSet=WeakMap.prototype.set;
    const native=host.storage;
    function origin(url){
        try{
            const u=new URLImpl(url), protocol=call(URLProtocol,u,[]);
            return protocol==='http:' || protocol==='https:'?call(URLOrigin,u,[]):null;
        }catch(e){if(e instanceof TypeErrorImpl)return null;throw e;}
    }
    function brand(value){
        const r=call(mapGet,records,[value]);
        if(!r)throw new TypeErrorImpl('Illegal Storage receiver');
        return r;
    }
    function string(value){
        if(typeof value==='symbol')throw new TypeErrorImpl('Cannot convert Symbol to DOMString');
        return StringImpl(value);
    }
    function access(r,op,key,value,index){
        const pair=call(native,host,[r.kind,op,key,value,index]);
        if(pair[0]!==0){
            const name=pair[0]===1?'SecurityError':pair[0]===2 || op===3?'QuotaExceededError':'UnknownError';
            throw new DOMExceptionImpl(pair[0]===1?'Storage is not available for this origin':
                pair[0]===2?'Storage quota exceeded':'Native storage read/write failed',name);
        }
        return pair[1];
    }
    class Storage {
        constructor(){throw new TypeErrorImpl('Illegal Storage constructor');}
        get length(){return access(brand(this),0);}
        key(index){const r=brand(this);if(!arguments.length)throw new TypeErrorImpl('Missing index');return access(r,1,undefined,undefined,index>>>0);}
        getItem(key){const r=brand(this);if(!arguments.length)throw new TypeErrorImpl('Missing key');return access(r,2,string(key));}
        setItem(key,value){const r=brand(this);if(arguments.length<2)throw new TypeErrorImpl('Missing key/value');access(r,3,string(key),string(value));}
        removeItem(key){const r=brand(this);if(!arguments.length)throw new TypeErrorImpl('Missing key');access(r,4,string(key));}
        clear(){access(brand(this),5);}
    }
    const prototype=Storage.prototype;
    define(prototype,Symbol.toStringTag,{configurable:true,value:'Storage'});
    for(const name of ['length','key','getItem','setItem','removeItem','clear']){
        const d=getOwn(prototype,name);d.enumerable=true;define(prototype,name,d);
    }
    function make(kind){
        const target=create(prototype), r={kind};
        function visible(p){return typeof p==='string' && !reflectHas(target,p);}
        const object=new Proxy(target,{
            get(t,p,receiver){if(visible(p)){const value=access(r,2,p);return value===null?undefined:value;}return reflectGet(t,p,receiver);},
            has(t,p){return reflectHas(t,p) || (typeof p==='string' && access(r,2,p)!==null);},
            set(t,p,v,receiver){
                if(typeof p==='string' && receiver===object){access(r,3,p,string(v));return true;}
                return reflectSet(t,p,v,receiver);
            },
            deleteProperty(t,p){if(visible(p) && access(r,2,p)!==null){access(r,4,p);return true;}return reflectDelete(t,p);},
            defineProperty(t,p,d){
                if(typeof p==='string'){
                    /* Proxy invariants cannot represent a virtual nonconfigurable
                     * named property. Reject it without mutating the store. */
                    if('get' in d || 'set' in d || !('value' in d || 'writable' in d) || d.configurable===false)return false;
                    access(r,3,p,string('value' in d?d.value:undefined));return true;
                }
                return reflectDefine(t,p,d);
            },
            getOwnPropertyDescriptor(t,p){
                const own=getOwn(t,p);if(own)return own;
                if(visible(p)){const value=access(r,2,p);if(value!==null)return {value,writable:true,enumerable:true,configurable:true};}
            },
            ownKeys(t){
                const keys=[], length=access(r,0);
                for(let i=0;i<length;i++){const key=access(r,1,undefined,undefined,i);if(visible(key))keys[keys.length]=key;}
                for(const key of reflectOwn(t))keys[keys.length]=key;
                return keys;
            },
            preventExtensions(){return false;}
        });
        call(mapSet,records,[object,r]);return object;
    }
    const local=make(0),session=make(1),windowObject=globalThis;
    function getter(object){return function(){
        if(this!==windowObject)throw new TypeErrorImpl('Illegal Window receiver');
        access(brand(object),6);return object;
    };}
    define(globalThis,'localStorage',{configurable:true,enumerable:true,get:getter(local)});
    define(globalThis,'sessionStorage',{configurable:true,enumerable:true,get:getter(session)});
    define(globalThis,'Storage',{configurable:true,writable:true,value:Storage});
    const EventImpl=Event, initEvent=Event.prototype.initEvent, wellFormed=String.prototype.toWellFormed;
    const eventRecords=new WeakMap();
    function eventRecord(event){const r=call(mapGet,eventRecords,[event]);if(!r)throw new TypeErrorImpl('Illegal StorageEvent receiver');return r;}
    function nullable(value){return value==null?null:string(value);}
    function fields(init){
        const area=init.storageArea==null?null:init.storageArea;if(area!==null)brand(area);
        return {key:nullable(init.key),oldValue:nullable(init.oldValue),newValue:nullable(init.newValue),
            url:init.url===undefined?'':call(wellFormed,string(init.url),[]),storageArea:area};
    }
    class StorageEvent extends EventImpl {
        constructor(type,init={}){
            if(!arguments.length)throw new TypeErrorImpl('StorageEvent requires type');
            init=init==null?{}:Object(init);super(type,init);call(mapSet,eventRecords,[this,fields(init)]);
        }
        initStorageEvent(type,bubbles=false,cancelable=false,key=null,oldValue=null,newValue=null,url='',storageArea=null){
            eventRecord(this);
            if(!arguments.length)throw new TypeErrorImpl('initStorageEvent requires type');
            const data=fields({key,oldValue,newValue,url,storageArea});type=string(type);
            if(eventState(this).dispatching)return;
            call(initEvent,this,[type,bubbles,cancelable]);call(mapSet,eventRecords,[this,data]);
        }
    }
    for(const key of ['key','oldValue','newValue','url','storageArea'])define(StorageEvent.prototype,key,{
        configurable:true,enumerable:true,get(){return eventRecord(this)[key];}
    });
    define(StorageEvent.prototype,Symbol.toStringTag,{configurable:true,value:'StorageEvent'});
    define(globalThis,'StorageEvent',{configurable:true,writable:true,value:StorageEvent});
    function deliver(kind,key,oldValue,newValue,url){
        const event=new StorageEvent('storage',{key,oldValue,newValue,url,storageArea:kind===0?local:session});
        eventState(event).isTrusted=true;dispatch(windowObject,event);
    }
    return {origin,deliver};
})();
