/* Real bounded native playback. Buffered fetch remains for unsupported hosts
 * and credentialed/opaque sources. Native anonymous Range derives document
 * origin in C and checks every response/preflight; it never follows redirects.
 * No page can pass a local path or an origin grant to the decoder.
 * MSE uses the private packet-buffer bridge. DRM, native live manifest loading,
 * autoplay and non-1x playback are not advertised. */
const avmediaBridge = (() => {
    const native = host.avmedia, states = new WeakMap(), active = new Set();
    let timer = 0;
    const mediaEvents = ['loadstart','loadedmetadata','loadeddata','canplay','canplaythrough','play','playing','pause','timeupdate','ended','error','emptied','seeking','seeked','volumechange','durationchange','abort','waiting','stalled','progress'];
    function event(node,type) { const e=new Event(type);e.isTrusted=true;dispatch(node,e); }
    function brand(node) {
        if (!node || (node.localName !== 'audio' && node.localName !== 'video') || node.namespaceURI !== 'http://www.w3.org/1999/xhtml') throw new TypeError('HTMLMediaElement receiver required');
        native('state',node); // native opaque node brand, not a user-spoofed tag string
    }
    function get(node) {
        brand(node);
        let s=states.get(node);
        if(!s){s={generation:0,intent:0,seekIntent:0,currentSrc:'',controller:null,promise:null,network:0,error:null,volume:1,muted:false,last:null,lastTime:0};states.set(node,s);}
        return s;
    }
    function snapshot(node) { get(node);return native('state',node); }
    function source(node) {
        let value=node.getAttribute('src');
        if(value===null)for(const child of node.children)if(child.localName==='source'){
            const type=child.getAttribute('type');if(!type||native('type',null,type)){value=child.getAttribute('src');break;}
        }
        if(!value)return '';
        return new URL(value,node.baseURI).href;
    }
    function abortError() { return new DOMException('Media source changed','AbortError'); }
    function current(node,s,generation,url) { return s.generation===generation&&s.currentSrc===url&&source(node)===url; }
    function notify(node,s,generation,url,type) { if(!current(node,s,generation,url))throw abortError();event(node,type);if(!current(node,s,generation,url))throw abortError(); }
    async function resumeLoop(node,s,generation,url,intent) {
        /* The seek is a child command, not an immediate restart. Every wait
         * may admit pause(), source replacement or loop=false. */
        for(;;) {
            if(!current(node,s,generation,url)||s.intent!==intent||!node.loop)throw abortError();
            const now=native('state',node);
            if(now.error)throw new Error(now.error);
            if(!now.seeking)break;
            await new Promise(resolve=>setTimeout(resolve,10));
        }
        await node.play();
    }
    function monitor() {
        if(timer||!active.size)return;
        timer=setTimeout(()=>{
            timer=0;
            for(const node of Array.from(active)){
                const s=states.get(node);if(!s){active.delete(node);continue;}const generation=s.generation;
                let now;
                try{now=native('state',node);}catch(e){active.delete(node);continue;}
                if(s.currentSrc&&source(node)!==s.currentSrc){const resume=!now.paused;const p=load(node),intent=s.intent;if(resume)p.then(()=>{if(s.intent===intent)return node.play();}).catch(()=>{});else p.catch(()=>{});continue;}
                if(now.error&&!now.mseQuotaError&&!s.error){s.error=new MediaError(3,now.error);event(node,'error');}
                if(s.generation!==generation)continue;
                if(s.last&&s.last.waiting!==now.waiting){if(now.waiting)event(node,'waiting');else if(!now.paused)event(node,'playing');}
                if(s.generation!==generation)continue;
                if(s.last&&!s.last.ended&&now.ended){
                    const intent=s.intent;
                    event(node,'timeupdate');
                    if(s.generation!==generation)continue;
                    const url=s.currentSrc;
                    if(s.intent===intent&&node.loop&&current(node,s,generation,url)&&native('seek',node,0)){
                        resumeLoop(node,s,generation,url,intent).catch(e=>{
                            if(current(node,s,generation,url)&&s.intent===intent&&e.name!=='AbortError'&&!s.error){s.error=new MediaError(3,String(e));event(node,'error');}
                        });now=native('state',node);
                    } else event(node,'ended');
                }
                if(s.generation!==generation)continue;
                if(!now.paused&&host.now()-s.lastTime>=250){s.lastTime=host.now();event(node,'timeupdate');}
                if(s.generation!==generation)continue;
                now=native('state',node);
                s.last=now;
                if(now.paused&&s.network!==2)active.delete(node);
            }
            monitor();
        },40);
    }
    function reset(node,s){
        mseBridge.detach(node);
        if(s.controller)s.controller.abort();
        s.controller=null;s.generation=(s.generation+1)>>>0;s.intent++;s.seekIntent++;s.promise=null;s.error=null;s.network=0;s.currentSrc='';s.last=null;
        native('reset',node,s.generation);textTrackBridge.reset(node);active.delete(node);
    }
    async function load(node,forPlay=false) {
        const s=get(node);reset(node,s);const generation=s.generation,loadIntent=s.intent;
        let url;try{url=source(node);}catch(e){s.error=new MediaError(4,'Invalid media URL');s.network=3;event(node,'error');throw e;}
        if(!url){s.network=3;event(node,'emptied');throw new DOMException('No supported media source','NotSupportedError');}
        const controller=new AbortController();s.currentSrc=url;s.network=2;s.controller=controller;active.add(node);monitor();notify(node,s,generation,url,'loadstart');
        const promise=(async()=>{
            try{
                /* Install the load Promise before submitting a native child. */
                await Promise.resolve();
                if(!current(node,s,generation,url)||controller.signal.aborted)throw abortError();
                const mediaSource=objectURLBridge.mediaSource(url),blob=objectURLBridge.blob(url);
                if(mediaSource) {
                    mseBridge.attach(mediaSource,node,generation);
                    for(;;){
                        if(!current(node,s,generation,url)||controller.signal.aborted)throw abortError();
                        const now=native('state',node);if(now.error&&!now.mseQuotaError)throw new Error(now.error);
                        if(now.readyState>=2)break;
                        await new Promise(resolve=>setTimeout(resolve,20));
                    }
                } else if(blob) {
                    if(!native('load',node,blobBridge.bytes(blob),generation))throw new Error(native('state',node).error||'Unsupported Blob media');
                } else if(native('range',node,url)) {
                    if(!native('loadURL',node,url,generation,forPlay&&s.intent===loadIntent))throw new Error(native('state',node).error||'Unsupported Range media input');
                    for(;;) {
                        if(!current(node,s,generation,url)||controller.signal.aborted)throw abortError();
                        const now=native('state',node);
                        if(now.error)throw new Error(now.error);
                        if(now.readyState>=2&&!now.loading)break;
                        /* Metadata progress is not a request timeout. Real
                         * child/network errors and explicit cancellation above
                         * terminate this load; elapsed GUI time must not reset it. */
                        await new Promise(resolve=>setTimeout(resolve,10));
                    }
                } else {
                    const response=await fetch(url,{signal:controller.signal,credentials:node.crossOrigin==='use-credentials'?'include':'same-origin'});
                    if(!current(node,s,generation,url))throw abortError();
                    if(!response.ok)throw new Error('Media HTTP status '+response.status);
                    const bytes=await response.arrayBuffer();
                    if(!current(node,s,generation,url))throw abortError();
                    if(!native('load',node,bytes,generation))throw new Error(native('state',node).error||'Unsupported media input');
                }
                if(!current(node,s,generation,url)||controller.signal.aborted)throw abortError();
                native('volume',node,s.volume,s.muted);
                s.network=1;s.controller=null;s.last=native('state',node);
                notify(node,s,generation,url,'durationchange');notify(node,s,generation,url,'loadedmetadata');
                if(s.last.readyState>=2){notify(node,s,generation,url,'loadeddata');notify(node,s,generation,url,'canplay');}
                if(s.last.readyState>=4)notify(node,s,generation,url,'canplaythrough');
                return node;
            }catch(e){
                if(s.generation!==generation)throw abortError();
                s.network=3;s.controller=null;const previous=s.error;s.error=s.error||new MediaError(e&&e.name==='AbortError'?1:4,String(e));
                if(s.error.code!==1&&!previous)event(node,'error');throw e;
            }finally{if(s.generation===generation)s.promise=null;}
        })();
        s.promise=promise;return promise;
    }
    class MediaError {
        constructor(code,message=''){this.code=code;this.message=message;}
    }
    for(const [name,value] of Object.entries({MEDIA_ERR_ABORTED:1,MEDIA_ERR_NETWORK:2,MEDIA_ERR_DECODE:3,MEDIA_ERR_SRC_NOT_SUPPORTED:4})){
        Object.defineProperty(MediaError,name,{value});Object.defineProperty(MediaError.prototype,name,{value});
    }
    class HTMLMediaElement extends HTMLElement {
        get src(){brand(this);const value=this.getAttribute('src');return value===null?'':new URL(value,this.baseURI).href;}
        set src(value){const s=get(this);this.setAttribute('src',String(value));reset(this,s);if(this.preload!=='none')load(this).catch(()=>{});}
        get currentSrc(){return get(this).currentSrc;}
        get networkState(){return get(this).network;}
        get readyState(){return snapshot(this).readyState;}
        get error(){return get(this).error;}
        get duration(){return mseBridge.attached(this)?mseBridge.duration(this):snapshot(this).duration;}
        get buffered(){get(this);return mseBridge.attached(this)?mseBridge.nodeRanges(this,false):mseBridge.timeRanges();}
        get seekable(){get(this);return mseBridge.attached(this)?mseBridge.nodeRanges(this,true):mseBridge.timeRanges();}
        get audioTracks(){get(this);return mseBridge.nodeTracks(this,'audio');}
        get videoTracks(){get(this);return mseBridge.nodeTracks(this,'video');}
        get textTracks(){get(this);return textTrackBridge.manage(this);}
        addTextTrack(kind,label='',language=''){get(this);if(!arguments.length)throw new TypeError('TextTrack kind required');return textTrackBridge.addTextTrack(this,kind,label,language);}
        get paused(){return snapshot(this).paused;}
        get ended(){return snapshot(this).ended;}
        get seeking(){return snapshot(this).seeking;}
        get currentTime(){return snapshot(this).currentTime;}
        set currentTime(value){
            const s=get(this),time=Number(value),generation=s.generation,url=s.currentSrc;
            if(!Number.isFinite(time)||time<0)throw new TypeError('Invalid media time');
            const seekIntent=++s.seekIntent;notify(this,s,generation,url,'seeking');
            if(!native('seek',this,time))throw new DOMException('Input cannot seek','NotSupportedError');
            const finish=async()=>{
                while(native('state',this).seeking){
                    await new Promise(resolve=>setTimeout(resolve,10));
                    if(!current(this,s,generation,url)||seekIntent!==s.seekIntent)throw abortError();
                }
                if(!current(this,s,generation,url)||seekIntent!==s.seekIntent)throw abortError();
                s.last=native('state',this);if(s.last.error)throw new Error(s.last.error);
                notify(this,s,generation,url,'timeupdate');notify(this,s,generation,url,'seeked');
            };
            finish().catch(e=>{if(current(this,s,generation,url)&&seekIntent===s.seekIntent&&e.name!=='AbortError'&&!s.error){s.error=new MediaError(3,String(e));event(this,'error');}});
        }
        get volume(){return get(this).volume;}
        set volume(value){const s=get(this),v=Number(value);if(!Number.isFinite(v)||v<0||v>1)throw new DOMException('Invalid volume','IndexSizeError');s.volume=v;native('volume',this,v,s.muted);event(this,'volumechange');}
        get muted(){return get(this).muted;}
        set muted(value){const s=get(this);s.muted=!!value;native('volume',this,s.volume,s.muted);event(this,'volumechange');}
        get defaultMuted(){brand(this);return this.hasAttribute('muted');}
        set defaultMuted(v){brand(this);this.toggleAttribute('muted',!!v);}
        get preload(){brand(this);const v=(this.getAttribute('preload')||'metadata').toLowerCase();return v==='none'||v==='auto'?v:'metadata';}
        set preload(v){brand(this);this.setAttribute('preload',String(v));}
        get crossOrigin(){brand(this);const v=this.getAttribute('crossorigin');return v===null?null:v.toLowerCase()==='use-credentials'?'use-credentials':'anonymous';}
        set crossOrigin(v){brand(this);if(v===null)this.removeAttribute('crossorigin');else this.setAttribute('crossorigin',String(v));}
        get playbackRate(){brand(this);return 1;}
        set playbackRate(v){brand(this);if(Number(v)!==1)throw new DOMException('Only 1x playback is supported','NotSupportedError');}
        get defaultPlaybackRate(){return this.playbackRate;}
        set defaultPlaybackRate(v){this.playbackRate=v;}
        canPlayType(type){get(this);if(!arguments.length)throw new TypeError('Media type is required');return native('type',null,String(type));}
        load(){get(this);load(this).catch(()=>{});}
        async play(){
            const s=get(this),url=source(this);
            const initialIntent=s.intent;
            /* A bounded Range child is a process resource, not a permanent
             * privilege of the first video. Explicit playback releases the
             * previous Range owner through its normal pause/intent path. */
            if(native('range',this,url))for(const other of Array.from(active)){
                if(other===this)continue;
                const old=native('state',other);
                if(old.rangeInput&&(!old.paused||old.loading||old.seeking))other.pause();
                if(s.intent!==initialIntent||source(this)!==url)throw abortError();
            }
            let pending=null,intent=s.intent;
            if(s.currentSrc!==url||!snapshot(this).readyState){
                if(s.promise&&s.currentSrc===url)pending=s.promise;
                else {intent=s.intent+1;pending=load(this,true);} /* Only load's own reset; a loadstart handler may cancel. */
            }
            if(pending)native('preparePlay',this);
            if(pending)await pending;
            if(s.intent!==intent||source(this)!==s.currentSrc)throw abortError();
            if(snapshot(this).ended&&!native('seek',this,0))throw new DOMException('Input cannot restart','NotSupportedError');
            if(!native('preparePlay',this))throw new DOMException(native('state',this).error||'Cannot prepare playback','NotSupportedError');
            for(let prepared=snapshot(this);prepared.seeking||prepared.playPreparing;prepared=snapshot(this)){
                await new Promise(resolve=>setTimeout(resolve,10));
                if(s.intent!==intent||source(this)!==s.currentSrc)throw abortError();
                if(snapshot(this).error)throw new Error(snapshot(this).error);
            }
            if(s.intent!==intent)throw new DOMException('Pending play was cancelled','AbortError');
            if(source(this)!==s.currentSrc)throw abortError();
            if(snapshot(this).error)throw new Error(snapshot(this).error);
            const wasPaused=snapshot(this).paused;
            if(!native('play',this))throw new DOMException(native('state',this).error||'No playable media','NotSupportedError');
            s.last=native('state',this);active.add(this);monitor();
            if(wasPaused){event(this,'play');if(s.intent!==intent||snapshot(this).paused)throw new DOMException('Play was cancelled','AbortError');event(this,'playing');}
        }
        pause(){const s=get(this),wasPaused=snapshot(this).paused;s.intent++;native('pause',this);s.last=native('state',this);active.delete(this);if(!wasPaused){event(this,'timeupdate');event(this,'pause');}}
    }
    for(const name of ['controls','autoplay','loop'])Object.defineProperty(HTMLMediaElement.prototype,name,{configurable:true,enumerable:true,get(){brand(this);return this.hasAttribute(name);},set(v){brand(this);this.toggleAttribute(name,!!v);}});
    for(const [name,value] of Object.entries({NETWORK_EMPTY:0,NETWORK_IDLE:1,NETWORK_LOADING:2,NETWORK_NO_SOURCE:3,HAVE_NOTHING:0,HAVE_METADATA:1,HAVE_CURRENT_DATA:2,HAVE_FUTURE_DATA:3,HAVE_ENOUGH_DATA:4})){
        Object.defineProperty(HTMLMediaElement,name,{value});Object.defineProperty(HTMLMediaElement.prototype,name,{value});
    }
    avmediaHandlerTarget=(target,type)=>target instanceof HTMLMediaElement && mediaEvents.includes(type);
    installHandlers(HTMLMediaElement.prototype,mediaEvents);
    class HTMLAudioElement extends HTMLMediaElement {}
    class HTMLVideoElement extends HTMLMediaElement {
        get videoWidth(){return snapshot(this).videoWidth;}
        get videoHeight(){return snapshot(this).videoHeight;}
        get width(){brand(this);return Math.max(0,Number(this.getAttribute('width'))||0);}
        set width(v){brand(this);this.setAttribute('width',String(Number(v)>>>0));}
        get height(){brand(this);return Math.max(0,Number(this.getAttribute('height'))||0);}
        set height(v){brand(this);this.setAttribute('height',String(Number(v)>>>0));}
        get poster(){brand(this);const v=this.getAttribute('poster');return v===null?'':new URL(v,this.baseURI).href;}
        set poster(v){brand(this);this.setAttribute('poster',String(v));}
        getVideoPlaybackQuality(){
            const s=snapshot(this);
            return {creationTime:performance.now(),totalVideoFrames:s.totalVideoFrames||0,droppedVideoFrames:s.droppedVideoFrames||0,corruptedVideoFrames:0};
        }
    }
    function Audio(src){const node=document.createElement('audio');node.preload='auto';if(arguments.length)node.src=src;return node;}
    Audio.prototype=HTMLAudioElement.prototype;
    Object.assign(globalThis,{HTMLMediaElement,HTMLAudioElement,HTMLVideoElement,MediaError,Audio});
    for(const cls of [HTMLMediaElement,HTMLAudioElement,HTMLVideoElement,MediaError])Object.defineProperty(cls.prototype,Symbol.toStringTag,{value:cls.name,configurable:true});
    return {HTMLMediaElement,HTMLAudioElement,HTMLVideoElement,brand,state:snapshot,generation(node){return get(node).generation;},privateFetch:fetch,activate(node){if(!node||!node.controls)return false;if(node.paused)node.play().catch(report);else node.pause();return true;}};
})();
