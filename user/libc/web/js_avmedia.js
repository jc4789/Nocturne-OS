/* Real bounded native playback. The existing fetch path owns URL resolution,
 * redirects, credentials and CORS; no page can pass a local path to the decoder.
 * MSE, DRM, live manifests, autoplay and non-1x playback are not advertised. */
const avmediaBridge = (() => {
    const native = host.avmedia, states = new WeakMap(), active = new Set();
    let timer = 0;
    const mediaEvents = ['loadstart','loadedmetadata','loadeddata','canplay','canplaythrough','play','playing','pause','timeupdate','ended','error','emptied','seeking','seeked','volumechange','durationchange','abort'];
    function event(node,type) { const e=new Event(type);e.isTrusted=true;dispatch(node,e); }
    function brand(node) {
        if (!node || (node.localName !== 'audio' && node.localName !== 'video') || node.namespaceURI !== 'http://www.w3.org/1999/xhtml') throw new TypeError('HTMLMediaElement receiver required');
        native('state',node); // native opaque node brand, not a user-spoofed tag string
    }
    function get(node) {
        brand(node);
        let s=states.get(node);
        if(!s){s={generation:0,intent:0,currentSrc:'',controller:null,promise:null,network:0,error:null,volume:1,muted:false,last:null,lastTime:0};states.set(node,s);}
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
    function monitor() {
        if(timer||!active.size)return;
        timer=setTimeout(()=>{
            timer=0;
            for(const node of Array.from(active)){
                const s=states.get(node);if(!s){active.delete(node);continue;}const generation=s.generation;
                let now;
                try{now=native('state',node);}catch(e){active.delete(node);continue;}
                if(s.currentSrc&&source(node)!==s.currentSrc){const resume=!now.paused;const p=load(node),intent=s.intent;if(resume)p.then(()=>{if(s.intent===intent)return node.play();}).catch(()=>{});else p.catch(()=>{});continue;}
                if(now.error&&!s.error){s.error=new MediaError(3,now.error);event(node,'error');}
                if(s.generation!==generation)continue;
                if(s.last&&!s.last.ended&&now.ended){
                    const intent=s.intent;
                    event(node,'timeupdate');
                    if(s.generation!==generation)continue;
                    if(s.intent===intent&&node.loop&&native('seek',node,0)&&native('play',node)){now=native('state',node);}
                    else event(node,'ended');
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
        if(s.controller)s.controller.abort();
        s.controller=null;s.generation=(s.generation+1)>>>0;s.intent++;s.promise=null;s.error=null;s.network=0;s.currentSrc='';s.last=null;
        native('reset',node,s.generation);active.delete(node);
    }
    async function load(node) {
        const s=get(node);reset(node,s);const generation=s.generation;
        let url;try{url=source(node);}catch(e){s.error=new MediaError(4,'Invalid media URL');s.network=3;event(node,'error');throw e;}
        if(!url){s.network=3;event(node,'emptied');throw new DOMException('No supported media source','NotSupportedError');}
        const controller=new AbortController();s.currentSrc=url;s.network=2;s.controller=controller;active.add(node);monitor();notify(node,s,generation,url,'loadstart');
        const promise=(async()=>{
            try{
                const response=await fetch(url,{signal:controller.signal,credentials:node.crossOrigin==='use-credentials'?'include':'same-origin'});
                if(s.generation!==generation)throw abortError();
                if(!response.ok)throw new Error('Media HTTP status '+response.status);
                const declared=Number(response.headers.get('content-length'));
                if(Number.isFinite(declared)&&declared>33554432)throw new RangeError('Media input exceeds 32 MiB');
                const bytes=await response.arrayBuffer();
                if(s.generation!==generation||source(node)!==url)throw abortError();
                if(bytes.byteLength>33554432)throw new RangeError('Media input exceeds 32 MiB');
                if(!native('load',node,bytes,generation))throw new Error(native('state',node).error||'Unsupported media input');
                native('volume',node,s.volume,s.muted);
                s.network=1;s.controller=null;s.last=native('state',node);
                notify(node,s,generation,url,'durationchange');notify(node,s,generation,url,'loadedmetadata');
                if(s.last.readyState>=2){notify(node,s,generation,url,'loadeddata');notify(node,s,generation,url,'canplay');notify(node,s,generation,url,'canplaythrough');}
                return node;
            }catch(e){
                if(s.generation!==generation)throw abortError();
                s.network=3;s.controller=null;s.error=new MediaError(e&&e.name==='AbortError'?1:4,String(e));
                if(s.error.code!==1)event(node,'error');throw e;
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
        get duration(){return snapshot(this).duration;}
        get paused(){return snapshot(this).paused;}
        get ended(){return snapshot(this).ended;}
        get seeking(){brand(this);return false;}
        get currentTime(){return snapshot(this).currentTime;}
        set currentTime(value){const s=get(this),time=Number(value),generation=s.generation,url=s.currentSrc;if(!Number.isFinite(time)||time<0)throw new TypeError('Invalid media time');notify(this,s,generation,url,'seeking');if(!native('seek',this,time))throw new DOMException('Input cannot seek','NotSupportedError');s.last=native('state',this);notify(this,s,generation,url,'timeupdate');notify(this,s,generation,url,'seeked');}
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
            let pending=null,intent=s.intent;
            if(s.currentSrc!==url||!snapshot(this).readyState){
                if(s.promise&&s.currentSrc===url)pending=s.promise;
                else {intent=s.intent+1;pending=load(this);} /* Only load's own reset; a loadstart handler may cancel. */
            }
            if(pending)await pending;
            if(s.intent!==intent)throw new DOMException('Pending play was cancelled','AbortError');
            if(source(this)!==s.currentSrc)throw abortError();
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
    }
    function Audio(src){const node=document.createElement('audio');node.preload='auto';if(arguments.length)node.src=src;return node;}
    Audio.prototype=HTMLAudioElement.prototype;
    Object.assign(globalThis,{HTMLMediaElement,HTMLAudioElement,HTMLVideoElement,MediaError,Audio});
    for(const cls of [HTMLMediaElement,HTMLAudioElement,HTMLVideoElement,MediaError])Object.defineProperty(cls.prototype,Symbol.toStringTag,{value:cls.name,configurable:true});
    return {HTMLMediaElement,HTMLAudioElement,HTMLVideoElement,activate(node){if(!node||!node.controls)return false;if(node.paused)node.play().catch(report);else node.pause();return true;}};
})();
