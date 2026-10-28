/* Window and Worker share the actual native HTTP identity and scheduler count.
 * Cookies remain a live Window-only host capability, not a write/read probe.
 * The languages are the user's, as the browser's preferences hold them (the
 * same list goes out as Accept-Language). HTML: navigator.languages is the same frozen array
 * until the global's languagechange task begins; the realm's host calls
 * languagesChanged() there and fires the event when it returns true. */
const navigatorBridge=(()=>{
    const invoke=Reflect.apply,has=WeakSet.prototype.has,add=WeakSet.prototype.add;
    const define=Object.defineProperty,getOwnDescriptor=Object.getOwnPropertyDescriptor,freeze=Object.freeze;
    const cookieEnabled=host.cookieEnabled,nativeLanguages=host.languages;
    const automation=host.automation===true;
    const split=String.prototype.split,join=Array.prototype.join;
    function userLanguages(){return freeze(invoke(split,invoke(nativeLanguages,host,[]),[',']));}
    // HTML appVersion in the Gecko navigator compatibility mode: empty unless the
    // User-Agent starts with "Mozilla/5.0 (", else "5.0 (Windows)" or the trail up to
    // its first ";" closed with ")".
    function geckoAppVersion(userAgent){
        if(!userAgent.startsWith('Mozilla/5.0 ('))return '';
        const trail=userAgent.slice('Mozilla/'.length);
        if(trail.startsWith('5.0 (Windows'))return '5.0 (Windows)';
        const semicolon=trail.indexOf(';');
        return (semicolon<0?trail:trail.slice(0,semicolon))+')';
    }
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
        // Web IDL: an attribute's getter is a function named "get <name>" of length 0.
        function attribute(name,read){
            const get=getOwnDescriptor({get [name](){requireReceiver(this);return read();}},name).get;
            define(Interface.prototype,name,{configurable:true,enumerable:true,get});
        }
        // HTML NavigatorID. The navigator compatibility mode is Gecko: the User-Agent names
        // neither Chrome nor WebKit and Nocturne claims no vendor, so productSub is
        // "20100101", vendor "", and Window has taintEnabled() and oscpu. productSub,
        // vendor, vendorSub, taintEnabled and oscpu are [Exposed=Window].
        const userAgent=host.userAgent,platform=host.platform;
        const values={userAgent,platform,appCodeName:'Mozilla',appName:'Netscape',
            appVersion:geckoAppVersion(userAgent),product:'Gecko',
            onLine:true,hardwareConcurrency:host.hardwareConcurrency};
        if(!worker){values.productSub='20100101';values.vendor='';values.vendorSub='';values.oscpu=platform;}
        for(const [name,value] of Object.entries(values))attribute(name,()=>value);
        if(!worker)define(Interface.prototype,'taintEnabled',{configurable:true,enumerable:true,writable:true,
            value:{taintEnabled(){requireReceiver(this);return false;}}.taintEnabled});
        let languages=userLanguages();
        attribute('language',()=>languages[0]);
        attribute('languages',()=>languages);
        function languagesChanged(){
            const next=userLanguages();
            if(invoke(join,next,[','])===invoke(join,languages,[',']))return false;
            languages=next;return true;
        }
        if(!worker)attribute('cookieEnabled',()=>typeof cookieEnabled==='function'&&!!invoke(cookieEnabled,host,[]));
        // WebDriver NavigatorAutomationInformation: Window-only. True only while
        // the browser's input is driven for automation (the WPT runner starts it
        // with --wpt-automation; see native_automation in js.c), never otherwise.
        if(!worker)attribute('webdriver',()=>automation);
        // Pointer Events: Window-only. The input layer turns a touch screen or
        // pen into the one system pointer (kernel usbhid.c), so pages never see
        // simultaneous touch contacts.
        if(!worker)attribute('maxTouchPoints',()=>0);
        // Beacon: a keepalive POST that outlives the document. Its steps are the Window's Fetch
        // (js_fetch.js beacon), not the page's replaceable fetch().
        if(!worker)define(Interface.prototype,'sendBeacon',{configurable:true,enumerable:true,writable:true,value:function sendBeacon(url,data=null){
            requireReceiver(this);
            if(arguments.length<1)throw new TypeError('sendBeacon requires a URL');
            return fetchBridge.beacon(url,data);
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
            attribute('mediaCapabilities',()=>capabilities);
            define(globalThis,'MediaCapabilities',{value:MediaCapabilities,writable:true,configurable:true});
        }
        define(Interface.prototype,Symbol.toStringTag,{value:worker?'WorkerNavigator':'Navigator',configurable:true});
        return {navigator:new Interface(token),Interface,languagesChanged};
    }
    return {create};
})();
