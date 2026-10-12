/* Binding/session checks only: media bridge is a recording mock, not playback. */
globalThis.Event=class Event{constructor(type){this.type=type;}};
globalThis.EventTarget=class EventTarget{
    constructor(){this.listeners=new Map();}
    addEventListener(type,f){let a=this.listeners.get(type);if(!a)this.listeners.set(type,a=[]);a.push(f);}
    dispatchEvent(event){for(const f of this.listeners.get(event.type)||[])f.call(this,event);if(this['on'+event.type])this['on'+event.type](event);return true;}
};
globalThis.runPSSHChunkCases=async function(){
    let checks=0;const ok=(v,n)=>{checks++;if(!v)throw new Error(n);};
    const pssh=new Uint8Array(52),view=new DataView(pssh.buffer);view.setUint32(0,52);view.setUint32(4,0x70737368);pssh[8]=1;
    pssh.set([0x10,0x77,0xef,0xec,0xc0,0xb2,0x4d,0x02,0xac,0xe3,0x3c,0x1e,0x52,0xe2,0xfb,0x4b],12);view.setUint32(28,1);for(let i=0;i<16;i++)pssh[32+i]=i;
    const node=new HTMLMediaElement(),sb={},events=[];node.addEventListener('encrypted',e=>events.push(new Uint8Array(e.initData)));
    const init=new Uint8Array(60);new DataView(init.buffer).setUint32(0,60);new DataView(init.buffer).setUint32(4,0x6d6f6f76);init.set(pssh,8);
    for(const byte of init)mediaEme.inspect(new Uint8Array([byte]).buffer,node,sb,1);mediaEme.commit(sb);await Promise.resolve();
    ok(events.length===1&&events[0].length===52&&events[0].every((v,i)=>v===pssh[i]),'PSSH and parent header split at every byte');
    mediaEme.reset(sb);mediaEme.inspect(init.slice(0,35).buffer,node,sb,1);mediaEme.reset(sb);mediaEme.inspect(init.slice(35).buffer,node,sb,1);await Promise.resolve();ok(events.length===1,'Abort cancels partial PSSH');
    mediaEme.reset(sb);mediaEme.inspect(init.buffer,node,sb,1);mediaEme.commit(sb);mediaEme.reset(sb);await Promise.resolve();ok(events.length===1,'Detach cancels queued encrypted event');
    mediaEme.inspect(init.buffer,node,sb,2);mediaEme.commit(sb);await Promise.resolve();ok(events.length===2,'New generation receives its own init');
    const independent={};mediaEme.inspect(init.slice(0,20).buffer,node,independent,2);mediaEme.inspect(init.buffer,node,sb,2);mediaEme.inspect(init.slice(20).buffer,node,independent,2);mediaEme.commit(sb);mediaEme.commit(independent);await Promise.resolve();ok(events.length===4,'SourceBuffer scanners remain independent');
    return checks;
};
globalThis.HTMLMediaElement=class HTMLMediaElement extends EventTarget{};
globalThis.MediaSource={isTypeSupported:s=>s.includes('mp4')&&!s.includes('av01')};
globalThis.navigator={};globalThis.emeNativeKeys=new Uint8Array();
host.avmedia=(op,node,data)=>{if(!(node instanceof HTMLMediaElement))throw new TypeError('Media element required');if(op==='emeKeys'){globalThis.emeNativeKeys=new Uint8Array(data);return true;}if(op==='state')return {msePending:false,error:''};throw new Error(op);};
host.set=f=>{Promise.resolve().then(f);return 1;};
globalThis.installHandlers=(prototype,names)=>{for(const name of names)Object.defineProperty(prototype,'on'+name,{writable:true,value:null});};
globalThis.runEMECases=async function(){
    let checks=0;const ok=(v,n)=>{checks++;if(!v)throw new Error(n);};
    const fails=async(f,name)=>{let e;try{await f();}catch(x){e=x;}ok(e?.name===name,'Expected '+name+', got '+e);};
    const config=[{initDataTypes:['keyids','cenc'],videoCapabilities:[{contentType:'video/mp4; codecs="avc1.42E01E"'}]}];
    const access=await navigator.requestMediaKeySystemAccess('org.w3.clearkey',config);ok(access instanceof MediaKeySystemAccess,'Access brand');
    ok(access.keySystem==='org.w3.clearkey','Key system');ok(access.getConfiguration().persistentState==='not-allowed','No persistent licenses');
    await fails(()=>navigator.requestMediaKeySystemAccess('com.widevine.alpha',config),'NotSupportedError');
    await fails(()=>navigator.requestMediaKeySystemAccess('org.w3.clearkey',[{...config[0],persistentState:'required'}]),'NotSupportedError');
    await fails(()=>navigator.requestMediaKeySystemAccess('org.w3.clearkey',[{videoCapabilities:[{contentType:'video/webm'}]}]),'NotSupportedError');
    await fails(()=>navigator.requestMediaKeySystemAccess('org.w3.clearkey',[{videoCapabilities:[{contentType:'video/mp4',encryptionScheme:'cbcs'}]}]),'NotSupportedError');
    const keys=await access.createMediaKeys(),node=new HTMLMediaElement();await node.setMediaKeys(keys);ok(node.mediaKeys===keys,'Element attachment');
    const second=new HTMLMediaElement();await fails(()=>second.setMediaKeys(keys),'QuotaExceededError');
    let illegal;try{keys.createSession('persistent-license');}catch(e){illegal=e;}ok(illegal?.name==='NotSupportedError','Persistent session rejected');
    const session=keys.createSession(),kid='AAECAwQFBgcICQoLDA0ODw',key='K34VFiiu0qar9xWICc9PPA';
    const request=new TextEncoder().encode(JSON.stringify({kids:[kid]}));let message;
    session.addEventListener('message',e=>message=e);await session.generateRequest('keyids',request);
    await Promise.resolve();ok(message?.messageType==='license-request','License request event');ok(JSON.parse(new TextDecoder().decode(message.message)).kids[0]===kid,'Request carries KID');
    await fails(()=>session.generateRequest('keyids',request),'InvalidStateError');
    await session.update(new TextEncoder().encode(JSON.stringify({keys:[{kty:'oct',kid,k:key}],type:'temporary'})));
    ok(emeNativeKeys.length===32&&emeNativeKeys[0]===0&&emeNativeKeys[16]===43,'Native KID/key snapshot');ok(session.keyStatuses.size===1,'Key status size');
    const id=new Uint8Array(16);for(let i=0;i<16;i++)id[i]=i;
    ok(session.keyStatuses.get(id)==='usable'&&session.keyStatuses.has(id),'Content-key lookup by bytes');
    ok([...session.keyStatuses][0][0].byteLength===16,'Status iteration');
    await fails(()=>session.update(new TextEncoder().encode('{"keys":[{"kty":"oct","kid":"x","k":"x"}]}')),'TypeError');
    const other=keys.createSession();await other.generateRequest('keyids',request);
    await fails(()=>other.update(new TextEncoder().encode(JSON.stringify({keys:[{kty:'oct',kid,k:'AAAAAAAAAAAAAAAAAAAAAA'}]}))),'InvalidStateError');
    await session.close();await session.closed;ok(session.keyStatuses.size===0&&emeNativeKeys.length===0,'Close revokes native key');
    await fails(()=>session.update(request),'InvalidStateError');await node.setMediaKeys(null);ok(node.mediaKeys===null,'Detach keys');
    let encrypted;node.addEventListener('encrypted',e=>encrypted=e);
    const pssh=new Uint8Array(52),view=new DataView(pssh.buffer);view.setUint32(0,52);view.setUint32(4,0x70737368);pssh[8]=1;
    pssh.set([0x10,0x77,0xef,0xec,0xc0,0xb2,0x4d,0x02,0xac,0xe3,0x3c,0x1e,0x52,0xe2,0xfb,0x4b],12);view.setUint32(28,1);pssh.set(id,32);view.setUint32(48,0);
    mediaEme.inspect(pssh.buffer,node);mediaEme.commit(node);await Promise.resolve();ok(encrypted?.initDataType==='cenc'&&encrypted.initData.byteLength===52,'PSSH encrypted event');
    const cenc=keys.createSession();let cencMessage;cenc.addEventListener('message',e=>cencMessage=e);await cenc.generateRequest('cenc',pssh);await Promise.resolve();
    ok(JSON.parse(new TextDecoder().decode(cencMessage.message)).kids[0]===kid,'Common PSSH KID request');
    await fails(()=>keys.createSession().generateRequest('cenc',new Uint8Array(9)),'TypeError');
    return checks;
};
