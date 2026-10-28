/* Temporary ClearKey sessions own keys; the element owns the native MSE
 * attachment. CENC packet parsing/decryption is performed in the native worker,
 * not by rewriting SourceBuffer bytes or intercepting a site's player. */
const mediaEme = (function installEncryptedMedia(host) {
    'use strict';
    const native=host.avmedia, apply=Reflect.apply, define=Object.defineProperty;
    const U8=Uint8Array, AB=ArrayBuffer, P=Promise, DomErr=DOMException;
    const keySlots=new WeakMap(), sessionSlots=new WeakMap(), accessSlots=new WeakMap(), elements=new WeakMap(), statusSlots=new WeakMap();
    const get=WeakMap.prototype.get, put=WeakMap.prototype.set, del=WeakMap.prototype.delete;
    const dispatch=EventTarget.prototype.dispatchEvent, token={};
    const isView=AB.isView, getter=(p,n)=>Object.getOwnPropertyDescriptor(p,n).get;
    const typedPrototype=Object.getPrototypeOf(U8.prototype),typedTag=getter(typedPrototype,Symbol.toStringTag);
    const typedBuffer=getter(typedPrototype,'buffer'),typedOffset=getter(typedPrototype,'byteOffset'),typedLength=getter(typedPrototype,'byteLength');
    const dataBuffer=getter(DataView.prototype,'buffer'),dataOffset=getter(DataView.prototype,'byteOffset'),dataLength=getter(DataView.prototype,'byteLength');
    const arrayLength=getter(AB.prototype,'byteLength'),resizable=Object.getOwnPropertyDescriptor(AB.prototype,'resizable');
    const error=(name,message)=>domExceptionBridge.create(name,message);
    const schedule=f=>host.set(f,0,false,[]);
    const reject=e=>new P((_,r)=>r(e));
    function slot(map,value,name){const s=apply(get,map,[value]);if(!s)throw new TypeError(name+' receiver required');return s;}
    function bytes(value){
        let buffer=value,offset=0,length;
        if(isView(value)){
            const typed=apply(typedTag,value,[])!==undefined;
            buffer=apply(typed?typedBuffer:dataBuffer,value,[]);offset=apply(typed?typedOffset:dataOffset,value,[]);length=apply(typed?typedLength:dataLength,value,[]);
        }
        const size=apply(arrayLength,buffer,[]);
        if(resizable&&apply(resizable.get,buffer,[]))throw new TypeError('Resizable buffers are not accepted');
        const view=new U8(buffer,offset,length===undefined?size:length);
        return new U8(view);
    }
    const alphabet='ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_';
    function b64(data){let s='',acc=0,bits=0;for(const v of data){acc=(acc<<8)|v;bits+=8;while(bits>=6){bits-=6;s+=alphabet[(acc>>>bits)&63];}}if(bits)s+=alphabet[(acc<<(6-bits))&63];return s;}
    function unb64(s){
        if(typeof s!=='string'||!/^[A-Za-z0-9_-]+$/.test(s)||s.length%4===1)throw new TypeError('Invalid base64url key');
        const out=new U8(Math.floor(s.length*6/8));let acc=0,bits=0,i=0;
        for(const c of s){acc=(acc<<6)|alphabet.indexOf(c);bits+=6;if(bits>=8){bits-=8;out[i++]=(acc>>>bits)&255;}}
        if(bits&&(acc&((1<<bits)-1)))throw new TypeError('Noncanonical base64url key');return out;
    }
    function fire(target,event){if(typeof eventState==='function')eventState(event).isTrusted=true;schedule(()=>apply(dispatch,target,[event]));}
    function equal(a,b){return a.length===b.length&&a.every((v,i)=>v===b[i]);}
    function records(keys){const s=slot(keySlots,keys,'MediaKeys'),map=new Map();
        for(const session of s.sessions){const t=slot(sessionSlots,session,'MediaKeySession');if(!t.closed)for(const [kid,key] of t.keys)map.set(kid,key);}
        const out=new U8(map.size*32);let at=0;for(const [kid,key] of map){out.set(unb64(kid),at);out.set(key,at+16);at+=32;}return out;
    }
    function publish(keys){const s=slot(keySlots,keys,'MediaKeys');if(!s.node)return new P(r=>r());
        const node=s.node,data=records(keys);
        if(!native('emeKeys',node,data.buffer))throw error('InvalidStateError','Cannot attach decryption keys');
        return new P((resolve,reject)=>{const check=()=>{if(s.node!==node){resolve();return;}let state;
            try{state=native('state',node);}catch(e){reject(e);return;}
            if(state.error){reject(error('OperationError',state.error));return;}
            if(state.msePending)host.set(check,10,false,[]);else resolve();};schedule(check);});
    }
    function kidsFromPssh(data){
        const view=new DataView(data.buffer,data.byteOffset,data.byteLength),kids=[];let at=0;
        const common='1077efecc0b24d02ace33c1e52e2fb4b',clear='e2719d58a985b3c9781ab030af78d30e';
        while(at<data.length){if(data.length-at<32)throw new TypeError('Truncated PSSH');let size=view.getUint32(at),header=8;
            if(size===1){if(data.length-at<40||view.getUint32(at+8))throw new TypeError('Oversized PSSH');size=view.getUint32(at+12);header=16;}
            if(!size)size=data.length-at;if(size<header+24||size>data.length-at||view.getUint32(at+4)!==0x70737368)throw new TypeError('Invalid PSSH');
            const body=at+header,version=data[body];if(version>1||data[body+1]||data[body+2]||data[body+3])throw new TypeError('Invalid PSSH version');
            let system='';for(const x of data.slice(body+4,body+20))system+=x.toString(16).padStart(2,'0');let cursor=body+20;
            if(version===1){if(cursor+4>at+size)throw new TypeError('Truncated PSSH KIDs');const n=view.getUint32(cursor);cursor+=4;
                if(n>(at+size-cursor-4)/16)throw new TypeError('Truncated PSSH KIDs');
                if(system===common||system===clear)for(let i=0;i<n;i++)kids.push(b64(data.slice(cursor+i*16,cursor+(i+1)*16)));cursor+=n*16;
            }
            if(cursor+4>at+size||view.getUint32(cursor)!==at+size-cursor-4)throw new TypeError('Invalid PSSH payload');at+=size;
        }
        if(!kids.length)throw error('NotSupportedError','No supported ClearKey PSSH KIDs');return [...new Set(kids)];
    }
    class MediaEncryptedEvent extends Event{
        constructor(type,init={}){super(type,init);define(this,'initDataType',{value:String(init.initDataType||''),enumerable:true});define(this,'initData',{value:init.initData==null?null:bytes(init.initData).buffer,enumerable:true});}
    }
    class MediaKeyMessageEvent extends Event{
        constructor(type,init={}){super(type,init);if(init.message===undefined||init.messageType===undefined)throw new TypeError('Message data required');define(this,'messageType',{value:String(init.messageType),enumerable:true});define(this,'message',{value:bytes(init.message).buffer,enumerable:true});}
    }
    class MediaKeyStatusMap{
        constructor(secret,session){if(secret!==token)throw new TypeError('Illegal constructor');apply(put,statusSlots,[this,session]);}
        get size(){return slot(sessionSlots,slot(statusSlots,this,'MediaKeyStatusMap'),'MediaKeySession').keys.size;}
        get(kid){const id=b64(bytes(kid));return slot(sessionSlots,slot(statusSlots,this,'MediaKeyStatusMap'),'MediaKeySession').keys.has(id)?'usable':undefined;}
        has(kid){return this.get(kid)!==undefined;}
        *entries(){for(const kid of slot(sessionSlots,slot(statusSlots,this,'MediaKeyStatusMap'),'MediaKeySession').keys.keys())yield[unb64(kid).buffer,'usable'];}
        *keys(){for(const [kid] of this.entries())yield kid;}
        *values(){for(const [,value] of this.entries())yield value;}
        forEach(callback,thisArg){for(const [kid,status] of this.entries())apply(callback,thisArg,[status,kid,this]);}
        [Symbol.iterator](){return this.entries();}
    }
    class MediaKeySession extends EventTarget{
        constructor(secret,keys){super();if(secret!==token)throw new TypeError('Illegal constructor');let resolveClosed;
            const closedPromise=new P(r=>resolveClosed=r),s={owner:keys,keys:new Map(),initialized:false,closed:false,id:'',closedPromise,resolveClosed};
            apply(put,sessionSlots,[this,s]);s.status=new MediaKeyStatusMap(token,this);
        }
        get sessionId(){return slot(sessionSlots,this,'MediaKeySession').id;}
        get expiration(){slot(sessionSlots,this,'MediaKeySession');return NaN;}
        get closed(){return slot(sessionSlots,this,'MediaKeySession').closedPromise;}
        get keyStatuses(){return slot(sessionSlots,this,'MediaKeySession').status;}
        generateRequest(type,initData){try{const s=slot(sessionSlots,this,'MediaKeySession'),data=bytes(initData);type=String(type);
            if(s.closed||s.initialized)throw error('InvalidStateError','Session is not callable');if(!data.length)throw new TypeError('Initialization data is empty');
            let kids;if(type==='cenc')kids=kidsFromPssh(data);else if(type==='keyids'){
                const json=JSON.parse(new TextDecoder('utf-8',{fatal:true}).decode(data));if(!json||!Array.isArray(json.kids)||!json.kids.length)throw new TypeError('Key IDs required');
                kids=[...new Set(json.kids.map(kid=>{if(unb64(kid).length!==16)throw new TypeError('CENC KID must be 16 bytes');return kid;}))];
            }else throw error('NotSupportedError','Unsupported initialization data');
            s.initialized=true;s.id=crypto.randomUUID();const message=new TextEncoder().encode(JSON.stringify({kids,type:'temporary'}));
            fire(this,new MediaKeyMessageEvent('message',{messageType:'license-request',message:message.buffer}));return new P(r=>r());
        }catch(e){return reject(e);}}
        update(response){try{const s=slot(sessionSlots,this,'MediaKeySession'),data=bytes(response);
            if(s.closed||!s.initialized)throw error('InvalidStateError','Session is not callable');if(!data.length)throw new TypeError('License is empty');
            const json=JSON.parse(new TextDecoder('utf-8',{fatal:true}).decode(data));if(!json||!Array.isArray(json.keys)||!json.keys.length||(json.type!==undefined&&json.type!=='temporary'))throw new TypeError('Invalid ClearKey license');
            const keys=new Map(s.keys);for(const entry of json.keys){if(!entry||entry.kty!=='oct')throw new TypeError('Expected an octet JWK');const kid=unb64(entry.kid),key=unb64(entry.k);
                if(kid.length!==16||key.length!==16)throw new TypeError('ClearKey CENC keys and IDs must be 16 bytes');keys.set(b64(kid),key);}
            const owner=slot(keySlots,s.owner,'MediaKeys');for(const other of owner.sessions)if(other!==this)for(const [kid,key] of slot(sessionSlots,other,'MediaKeySession').keys)
                if(keys.has(kid)&&!equal(keys.get(kid),key))throw error('InvalidStateError','Conflicting key in another session');
            const old=s.keys;s.keys=keys;let result;try{result=publish(s.owner);}catch(e){s.keys=old;throw e;}
            return result.then(()=>{for(const [kid,key] of old)if(keys.get(kid)!==key)key.fill(0);if(!s.closed)fire(this,new Event('keystatuseschange'));},e=>{s.keys=old;throw e;});
        }catch(e){return reject(e);}}
        close(){try{const s=slot(sessionSlots,this,'MediaKeySession');if(s.closed)return new P(r=>r());s.closed=true;for(const key of s.keys.values())key.fill(0);s.keys.clear();slot(keySlots,s.owner,'MediaKeys').sessions.delete(this);
            return publish(s.owner).then(()=>{s.resolveClosed('closed-by-application');fire(this,new Event('keystatuseschange'));});
        }catch(e){return reject(e);}}
        remove(){return this.close();}
        load(){try{slot(sessionSlots,this,'MediaKeySession');return reject(error('NotSupportedError','Persistent sessions are not supported'));}catch(e){return reject(e);}}
    }
    class MediaKeys{
        constructor(secret){if(secret!==token)throw new TypeError('Illegal constructor');apply(put,keySlots,[this,{node:null,sessions:new Set()}]);}
        createSession(type='temporary'){const s=slot(keySlots,this,'MediaKeys');if(String(type)!=='temporary')throw error('NotSupportedError','Only temporary ClearKey sessions are supported');const session=new MediaKeySession(token,this);s.sessions.add(session);return session;}
        setServerCertificate(certificate){try{slot(keySlots,this,'MediaKeys');if(!bytes(certificate).length)throw new TypeError('Certificate is empty');return new P(r=>r(false));}catch(e){return reject(e);}}
    }
    class MediaKeySystemAccess{
        constructor(secret,configuration){if(secret!==token)throw new TypeError('Illegal constructor');apply(put,accessSlots,[this,configuration]);}
        get keySystem(){slot(accessSlots,this,'MediaKeySystemAccess');return 'org.w3.clearkey';}
        getConfiguration(){return JSON.parse(JSON.stringify(slot(accessSlots,this,'MediaKeySystemAccess')));}
        createMediaKeys(){try{slot(accessSlots,this,'MediaKeySystemAccess');return new P(r=>r(new MediaKeys(token)));}catch(e){return reject(e);}}
    }
    function configuration(value){
        if(!value||value.distinctiveIdentifier==='required'||value.persistentState==='required')return null;
        const types=value.initDataTypes===undefined?['cenc','keyids']:Array.from(value.initDataTypes,String).filter(x=>x==='cenc'||x==='keyids');if(!types.length)return null;
        const sessions=value.sessionTypes===undefined?['temporary']:Array.from(value.sessionTypes,String);if(!sessions.length||sessions.some(x=>x!=='temporary'))return null;
        const result={label:String(value.label||''),initDataTypes:types,distinctiveIdentifier:'not-allowed',persistentState:'not-allowed',sessionTypes:sessions};
        let requested=false;
        for(const kind of ['audioCapabilities','videoCapabilities']){if(value[kind]===undefined)continue;const capabilities=Array.from(value[kind]);if(!capabilities.length)continue;requested=true;
            const supported=capabilities.filter(c=>c&&typeof c.contentType==='string'&&/^\s*(audio|video)\/mp4\s*(;|$)/i.test(c.contentType)&&!c.robustness&&(c.encryptionScheme==null||c.encryptionScheme==='cenc')&&MediaSource.isTypeSupported(c.contentType));
            if(!supported.length)return null;result[kind]=supported.map(c=>({contentType:c.contentType,robustness:'',...(c.encryptionScheme===undefined?{}:{encryptionScheme:c.encryptionScheme})}));
        }
        return requested?result:null;
    }
    function requestMediaKeySystemAccess(system,configs){try{if(this!==navigator)throw new TypeError('Navigator receiver required');if(!cryptoSecureContext)throw error('SecurityError','A secure context is required');
        system=String(system);const list=Array.from(configs);if(!system||!list.length)throw new TypeError('Key system and configurations required');if(system!=='org.w3.clearkey')throw error('NotSupportedError','Unsupported key system');
        for(const c of list){const result=configuration(c);if(result)return new P(r=>r(new MediaKeySystemAccess(token,result)));}
        throw error('NotSupportedError','No supported ClearKey configuration');
    }catch(e){return reject(e);}}
    function setMediaKeys(keys){try{native('state',this);const old=apply(get,elements,[this]);if(keys===old||keys===null&&!old)return new P(r=>r());
        const s=keys===null?null:slot(keySlots,keys,'MediaKeys');if(s&&s.node&&s.node!==this)throw error('QuotaExceededError','MediaKeys is attached to another element');
        const data=s?records(keys):new U8(0);if(!native('emeKeys',this,data.buffer))throw error('InvalidStateError','Cannot attach MediaKeys');
        if(old)slot(keySlots,old,'MediaKeys').node=null;
        if(s){s.node=this;apply(put,elements,[this,keys]);}else apply(del,elements,[this]);return new P(r=>r());
    }catch(e){return reject(e);}}
    if(cryptoSecureContext){
        define(navigator,'requestMediaKeySystemAccess',{value:requestMediaKeySystemAccess,writable:true,enumerable:true,configurable:true});
        define(HTMLMediaElement.prototype,'mediaKeys',{get(){native('state',this);return apply(get,elements,[this])||null;},enumerable:true,configurable:true});
        define(HTMLMediaElement.prototype,'setMediaKeys',{value:setMediaKeys,writable:true,enumerable:true,configurable:true});
        for(const Class of [MediaKeys,MediaKeySession,MediaKeySystemAccess,MediaKeyStatusMap,MediaEncryptedEvent,MediaKeyMessageEvent]){
            define(Class.prototype,Symbol.toStringTag,{value:Class.name,configurable:true});define(globalThis,Class.name,{value:Class,writable:true,configurable:true});
        }
        installHandlers(MediaKeySession.prototype,['message','keystatuseschange']);
        if(typeof emeHandlerTarget!=='undefined')emeHandlerTarget=(value,type)=>
            apply(get,sessionSlots,[value])!==undefined&&(type==='message'||type==='keystatuseschange');
        if(typeof cloneData!=='undefined')cloneData.registerUncloneable(value=>
            apply(get,keySlots,[value])!==undefined||apply(get,sessionSlots,[value])!==undefined||
            apply(get,accessSlots,[value])!==undefined||apply(get,statusSlots,[value])!==undefined);
    }
    /* Each SourceBuffer owns one incremental observer. Native MSE retains and
       validates the authored byte stream; this observer owns only at most a
       16-byte header and PSSH fragments, never mdat or the appended history. */
    const scanners=new WeakMap(),containers=new Set([0x6d6f6f76,0x7472616b,0x6d646961,0x6d696e66,0x7374626c,0x6d6f6f66,0x74726166]);
    function reset(key){const old=apply(get,scanners,[key]);if(old){old.live=false;old.current=null;old.stack=[];old.notifications=[];apply(del,scanners,[key]);}}
    function emit(r,key,box){
        const output=new U8(box.bytes);let at=0;for(const part of box.parts){output.set(part,at);at+=part.length;}box.parts=[];
        const event=new MediaEncryptedEvent('encrypted',{initDataType:'cenc',initData:output.buffer});
        if(typeof eventState==='function')eventState(event).isTrusted=true;
        /* abort/remove/changeType/detach cancels notifications as well as the
           partial metadata. A completed old init must not reach a new source. */
        const barrier=r.stack.length>1?r.stack[1]:box.end;
        r.notifications.push({event,barrier});
    }
    function commit(key){const r=apply(get,scanners,[key]);if(!r||r.failed)return;const keep=[];
        for(const item of r.notifications){if(item.barrier>r.offset){keep.push(item);continue;}schedule(()=>{if(r.live&&apply(get,scanners,[key])===r)apply(dispatch,r.node,[item.event]);});}r.notifications=keep;
    }
    function finish(key){const r=apply(get,scanners,[key]);if(r?.current?.pssh&&r.current.end===Infinity){emit(r,key,r.current);r.current=null;}if(r)for(const item of r.notifications)if(item.barrier===Infinity)item.barrier=r.offset;}
    function inspect(input,node,key=node,generation=0){
        if(!cryptoSecureContext||!node)return;
        let r=apply(get,scanners,[key]);if(!r||r.node!==node||r.generation!==generation){reset(key);r={node,generation,live:true,failed:false,offset:0,stack:[Infinity],header:new U8(16),used:0,need:8,current:null,notifications:[]};apply(put,scanners,[key,r]);}
        if(r.failed)return;
        const data=new U8(input);let at=0;
        while(at<data.length&&!r.failed){
            if(r.current){
                const box=r.current,take=Math.min(data.length-at,box.end-r.offset);
                if(take<0){r.failed=true;break;}
                if(box.pssh&&take){box.parts.push(data.slice(at,at+take));box.bytes+=take;}
                at+=take;r.offset+=take;
                if(r.offset===box.end){if(box.pssh)emit(r,key,box);r.current=null;continue;}
                if(!take){r.failed=true;break;}continue;
            }
            while(r.stack.length>1&&r.offset===r.stack[r.stack.length-1])r.stack.pop();
            const parentEnd=r.stack[r.stack.length-1];
            if(parentEnd-r.offset<r.need-r.used){r.failed=true;break;}
            const take=Math.min(data.length-at,r.need-r.used);r.header.set(data.subarray(at,at+take),r.used);r.used+=take;at+=take;r.offset+=take;
            if(r.used<r.need)continue;
            const view=new DataView(r.header.buffer);let size=view.getUint32(0),type=view.getUint32(4);
            if(size===1&&r.need===8){r.need=16;continue;}
            const header=r.need,start=r.offset-header;
            if(size===1){size=view.getUint32(8)*4294967296+view.getUint32(12);if(!Number.isSafeInteger(size)){r.failed=true;break;}}
            const end=size===0?parentEnd:start+size;
            if((size&&size<header)||end>parentEnd||(end!==Infinity&&!Number.isSafeInteger(end))){r.failed=true;break;}
            r.used=0;r.need=8;
            if(containers.has(type)){r.stack.push(end);continue;}
            r.current={end,pssh:type===0x70737368,bytes:header,parts:type===0x70737368?[r.header.slice(0,header)]:null};
            if(r.offset===end){if(r.current.pssh)emit(r,key,r.current);r.current=null;}
        }
        if(r.failed){r.current=null;r.stack=[];r.notifications=[];}
    }
    return {inspect,reset,finish,commit};
})(host);
