/* Window and Worker share the actual native HTTP identity and scheduler count.
 * Cookies remain a live Window-only host capability, not a write/read probe. */
const navigatorBridge=(()=>{
    const invoke=Reflect.apply,has=WeakSet.prototype.has,add=WeakSet.prototype.add;
    const define=Object.defineProperty,freeze=Object.freeze,cookieEnabled=host.cookieEnabled;
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
        const values={userAgent:host.userAgent,platform:host.platform,language:'en-US',
            appCodeName:'Mozilla',appName:'Netscape',appVersion:host.userAgent,
            vendor:'',vendorSub:'',product:'Gecko',productSub:'20030107',
            languages:freeze(['en-US']),onLine:true,hardwareConcurrency:host.hardwareConcurrency};
        for(const [name,value] of Object.entries(values))
            define(Interface.prototype,name,{configurable:true,enumerable:true,get(){requireReceiver(this);return value;}});
        if(!worker)define(Interface.prototype,'cookieEnabled',{configurable:true,enumerable:true,get(){
            requireReceiver(this);return typeof cookieEnabled==='function'&&!!invoke(cookieEnabled,host,[]);
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
                    for(const kind of ['audio','video'])if(configuration[kind]!==undefined){const v=configuration[kind];if(!v||typeof v!=='object')throw new TypeError('Invalid '+kind+' configuration');const member={contentType:String(v.contentType),bitrate:Number(v.bitrate)};
                        for(const name of kind==='video'?['width','height','framerate']:['samplerate'])if(v[name]!==undefined)member[name]=Number(v[name]);
                        for(const name of ['channels','transferFunction','colorGamut','hdrMetadataType'])if(v[name]!==undefined)member[name]=String(v[name]);c[kind]=member;}
                    if(configuration.keySystemConfiguration!==undefined)c.keySystemConfiguration=configuration.keySystemConfiguration;
                    if(c.type!=='file'&&c.type!=='media-source')throw new TypeError('Invalid media decoding type');
                    if(!c.audio&&!c.video)throw new TypeError('Audio or video configuration required');
                    let supported=true;
                    for(const kind of ['audio','video'])if(c[kind]){
                        const v=c[kind];
                        if(typeof v.contentType!=='string'||!v.contentType.startsWith(kind+'/'))throw new TypeError('Invalid content type');
                        if(!Number.isFinite(v.bitrate)||v.bitrate<=0)throw new TypeError('Invalid bitrate');
                        if(kind==='video'&&(!Number.isFinite(v.width)||v.width<=0||!Number.isFinite(v.height)||v.height<=0||!Number.isFinite(v.framerate)||v.framerate<=0))throw new TypeError('Invalid video dimensions or frame rate');
                        if(kind==='audio'&&v.samplerate!==undefined&&(!Number.isFinite(v.samplerate)||v.samplerate<=0))throw new TypeError('Invalid sample rate');
                        supported=supported&&!!invoke(type,host,['type',null,v.contentType]);
                        // The native pipeline currently outputs SDR. Do not
                        // advertise HDR merely because its container parses.
                        if(v.transferFunction&&v.transferFunction!=='srgb'||v.colorGamut&&v.colorGamut!=='srgb'||v.hdrMetadataType)supported=false;
                    }
                    let keySystemAccess=null;
                    if(c.keySystemConfiguration){
                        if(typeof navigator.requestMediaKeySystemAccess!=='function')supported=false;
                        else try{keySystemAccess=await navigator.requestMediaKeySystemAccess(c.keySystemConfiguration.keySystem,[c.keySystemConfiguration]);}catch(_){supported=false;}
                    }
                    // No measured real-time/hardware capability model exists:
                    // report the actual decoder support, not fabricated speed.
                    return {supported,smooth:false,powerEfficient:false,keySystemAccess,configuration:c};
                }
            }
            const capabilities=Object.create(MediaCapabilities.prototype);
            define(Interface.prototype,'mediaCapabilities',{configurable:true,enumerable:true,get(){requireReceiver(this);return capabilities;}});
            define(globalThis,'MediaCapabilities',{value:MediaCapabilities,writable:true,configurable:true});
        }
        define(Interface.prototype,Symbol.toStringTag,{value:worker?'WorkerNavigator':'Navigator',configurable:true});
        return {navigator:new Interface(token),Interface};
    }
    return {create};
})();
