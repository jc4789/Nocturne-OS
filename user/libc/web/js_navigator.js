/* Window and Worker share the actual native HTTP identity and scheduler count.
 * Cookies remain a live Window-only host capability, not a write/read probe.
 * The languages are the user's, as the browser's preferences hold them (the
 * same list goes out as Accept-Language). HTML: navigator.languages is the same frozen array
 * until the global's languagechange task begins; the realm's host calls
 * languagesChanged() there and fires the event when it returns true. */
const navigatorBridge=(()=>{
    const invoke=Reflect.apply,has=WeakSet.prototype.has,add=WeakSet.prototype.add;
    const define=Object.defineProperty,freeze=Object.freeze,cookieEnabled=host.cookieEnabled,nativeLanguages=host.languages;
    const automation=host.automation===true;
    const split=String.prototype.split,join=Array.prototype.join;
    function userLanguages(){return freeze(invoke(split,invoke(nativeLanguages,host,[]),[',']));}
    function create(worker){
        const token={},brands=new WeakSet();
        function construct(receiver,value){
            if(value!==token)throw new TypeError('Illegal navigator constructor');
            invoke(add,brands,[receiver]);
        }
        class Navigator {constructor(value){construct(this,value);}}
        class WorkerNavigator {constructor(value){construct(this,value);}}
        const Interface=worker?WorkerNavigator:Navigator;
        function requireReceiver(receiver){
            if(!invoke(has,brands,[receiver]))throw new TypeError('Illegal navigator receiver');
        }
        const values={userAgent:host.userAgent,platform:host.platform,
            appCodeName:'Mozilla',appName:'Netscape',appVersion:host.userAgent,
            vendor:'',vendorSub:'',product:'Gecko',productSub:'20030107',
            onLine:true,hardwareConcurrency:host.hardwareConcurrency};
        for(const [name,value] of Object.entries(values))
            define(Interface.prototype,name,{configurable:true,enumerable:true,get(){requireReceiver(this);return value;}});
        let languages=userLanguages();
        define(Interface.prototype,'language',{configurable:true,enumerable:true,get(){requireReceiver(this);return languages[0];}});
        define(Interface.prototype,'languages',{configurable:true,enumerable:true,get(){requireReceiver(this);return languages;}});
        function languagesChanged(){
            const next=userLanguages();
            if(invoke(join,next,[','])===invoke(join,languages,[',']))return false;
            languages=next;return true;
        }
        if(!worker)define(Interface.prototype,'cookieEnabled',{configurable:true,enumerable:true,get(){
            requireReceiver(this);return typeof cookieEnabled==='function'&&!!invoke(cookieEnabled,host,[]);
        }});
        // WebDriver NavigatorAutomationInformation: Window-only. True only while
        // the browser's input is driven for automation (the WPT runner starts it
        // with --wpt-automation; see native_automation in js.c), never otherwise.
        if(!worker)define(Interface.prototype,'webdriver',{configurable:true,enumerable:true,get(){
            requireReceiver(this);return automation;
        }});
        // Pointer Events: Window-only. The input layer turns a touch screen or
        // pen into the one system pointer (kernel usbhid.c), so pages never see
        // simultaneous touch contacts.
        if(!worker)define(Interface.prototype,'maxTouchPoints',{configurable:true,enumerable:true,get(){
            requireReceiver(this);return 0;
        }});
        // Beacon: a POST that outlives the document, carried by the native
        // keepalive fetch transport. Only a CORS-safelisted body type may go
        // without CORS; anything else needs the server's consent.
        if(!worker)define(Interface.prototype,'sendBeacon',{configurable:true,enumerable:true,writable:true,value:function sendBeacon(url,data=null){
            requireReceiver(this);
            if(arguments.length<1)throw new TypeError('sendBeacon requires a URL');
            const target=new URL(String(url),document.baseURI);
            if(target.protocol!=='http:'&&target.protocol!=='https:')throw new TypeError('sendBeacon requires an HTTP(S) URL');
            if(data!==null&&typeof ReadableStream==='function'&&data instanceof ReadableStream)throw new TypeError('sendBeacon cannot send a stream');
            let safelisted=true;
            if(data!==null&&typeof Blob==='function'&&data instanceof Blob&&data.type){
                const essence=data.type.split(';')[0].trim().toLowerCase();
                safelisted=['application/x-www-form-urlencoded','multipart/form-data','text/plain'].includes(essence);
            }
            fetch(target.href,{method:'POST',body:data,keepalive:true,credentials:'include',mode:safelisted?'no-cors':'cors'}).catch(()=>{});
            return true;
        }});
        if(!worker&&typeof host.avmedia==='function'){
            const type=host.avmedia;
            class MediaCapabilities {
                constructor(){throw new TypeError('Illegal MediaCapabilities constructor');}
                async decodingInfo(configuration){
                    if(this!==capabilities)throw new TypeError('Illegal MediaCapabilities receiver');
                    if(!configuration||typeof configuration!=='object')throw new TypeError('Media configuration required');
                    // Web IDL dictionaries ignore unknown fields, including
                    // otherwise uncloneable callbacks. Snapshot only members
                    // of MediaDecodingConfiguration instead of serializing it.
                    const c={type:String(configuration.type)};
                    for(const kind of ['audio','video'])if(configuration[kind]!==undefined){const v=configuration[kind];if(!v||typeof v!=='object')throw new TypeError('Invalid '+kind+' configuration');const member={contentType:String(v.contentType)};
                        if(kind==='video'&&v.bitrate===undefined)throw new TypeError('Video bitrate required');
                        if(v.bitrate!==undefined)member.bitrate=Number(v.bitrate);
                        for(const name of kind==='video'?['width','height','framerate']:['samplerate'])if(v[name]!==undefined)member[name]=Number(v[name]);
                        for(const name of kind==='video'?['transferFunction','colorGamut','hdrMetadataType','scalabilityMode']:['channels'])if(v[name]!==undefined)member[name]=String(v[name]);
                        if(kind==='video')for(const name of ['hasAlphaChannel','spatialScalability'])if(v[name]!==undefined)member[name]=Boolean(v[name]);c[kind]=member;}
                    if(configuration.keySystemConfiguration!==undefined)c.keySystemConfiguration=configuration.keySystemConfiguration;
                    if(c.type!=='file'&&c.type!=='media-source')throw new TypeError('Invalid media decoding type');
                    if(!c.audio&&!c.video)throw new TypeError('Audio or video configuration required');
                    let supported=true;
                    for(const kind of ['audio','video'])if(c[kind]){
                        const v=c[kind];
                        if(typeof v.contentType!=='string'||!v.contentType.startsWith(kind+'/'))throw new TypeError('Invalid content type');
                        if(v.bitrate!==undefined&&(!Number.isFinite(v.bitrate)||v.bitrate<0))throw new TypeError('Invalid bitrate');
                        if(kind==='video'&&(!Number.isFinite(v.width)||v.width<=0||!Number.isFinite(v.height)||v.height<=0||!Number.isFinite(v.framerate)||v.framerate<=0))throw new TypeError('Invalid video dimensions or frame rate');
                        if(kind==='audio'&&v.samplerate!==undefined&&(!Number.isFinite(v.samplerate)||v.samplerate<=0))throw new TypeError('Invalid sample rate');
                        supported=supported&&!!invoke(type,host,['type',null,v.contentType]);
                        // The native pipeline currently outputs SDR. Do not
                        // advertise HDR merely because its container parses.
                        if(v.transferFunction&&v.transferFunction!=='srgb'||v.colorGamut&&v.colorGamut!=='srgb'||v.hdrMetadataType)supported=false;
                        if(kind==='video'&&v.hasAlphaChannel)supported=false; // native surfaces are opaque
                        if(kind==='video'&&(v.scalabilityMode!==undefined||v.spatialScalability!==undefined))throw new TypeError('Scalability members require WebRTC');
                    }
                    let keySystemAccess=null;
                    if(c.keySystemConfiguration){
                        if(typeof navigator.requestMediaKeySystemAccess!=='function')supported=false;
                        else try{keySystemAccess=await navigator.requestMediaKeySystemAccess(c.keySystemConfiguration.keySystem,[c.keySystemConfiguration]);}catch(_){supported=false;}
                    }
                    // Query observed native decoder/converter costs for the
                    // actual codec and dimensions. Unknown configurations stay
                    // conservative; software decode is not hardware efficiency.
                    const query=['decodingPerformance',null,c.video?c.video.contentType:'',c.video?c.video.width:0,
                        c.video?c.video.height:0,c.video?c.video.framerate:0,c.video?c.video.bitrate:0];
                    // An audio request needs its actual codec/profile/channel/
                    // source-rate PCM observation, never a fixed allowance.
                    if(c.audio)query.push(c.audio.contentType,c.audio.channels===undefined?0:Number(c.audio.channels),c.audio.samplerate||0);
                    const smooth=supported&&!!invoke(type,host,query);
                    return {supported,smooth,powerEfficient:false,keySystemAccess,configuration:c};
                }
            }
            const capabilities=Object.create(MediaCapabilities.prototype);
            define(Interface.prototype,'mediaCapabilities',{configurable:true,enumerable:true,get(){requireReceiver(this);return capabilities;}});
            define(globalThis,'MediaCapabilities',{value:MediaCapabilities,writable:true,configurable:true});
        }
        define(Interface.prototype,Symbol.toStringTag,{value:worker?'WorkerNavigator':'Navigator',configurable:true});
        return {navigator:new Interface(token),Interface,languagesChanged};
    }
    return {create};
})();
