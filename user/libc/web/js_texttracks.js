/* 実media時刻に結線した有限plain WebVTT字幕。外部player/偽empty listなし。
 * regions/STYLE/位置指定/roll-up/内部timestamp/暗号/in-band字幕は未対応。
 * https://html.spec.whatwg.org/multipage/media.html#text-track-api
 * https://www.w3.org/TR/webvtt1/ */
const textTrackBridge = (() => {
    'use strict';
    const native=host.avmedia, brandMedia=avmediaBridge.brand, mediaState=avmediaBridge.state;
    const generation=avmediaBridge.generation, privateFetch=avmediaBridge.privateFetch;
    const apply=Reflect.apply, define=Object.defineProperty;
    const mapGet=WeakMap.prototype.get, mapSet=WeakMap.prototype.set;
    const mediaSlots=new WeakMap(), trackSlots=new WeakMap(), cueSlots=new WeakMap(), listSlots=new WeakMap(), elementSlots=new WeakMap();
    const token={}, HTML='http://www.w3.org/1999/xhtml', KINDS=['subtitles','captions','descriptions','chapters','metadata'];
    const schedule=fn=>host.timer(0,fn,0,[]), encoder=new TextEncoder(), encode=TextEncoder.prototype.encode;
    const decoder=new TextDecoder('utf-8',{fatal:true}), decode=TextDecoder.prototype.decode;
    const Abort=fetchBridge.AbortController, bodyBytes=fetchBridge.Response.prototype.arrayBuffer;
    const refs=new Set(), MAX_MEDIA=16, MAX_TRACKS=16, MAX_CUES=4096, MAX_TEXT=2048, MAX_SOURCE=524288, MAX_TOTAL=1048576;
    let pollTimer=0, sequence=0;
    const lookup=(map,value,name)=>{const s=apply(mapGet,map,[value]);if(!s)throw new TypeError('Illegal '+name+' receiver');return s;};
    const cue=v=>lookup(cueSlots,v,'TextTrackCue'), track=v=>lookup(trackSlots,v,'TextTrack');
    const put=(map,value,data)=>apply(mapSet,map,[value,data]);
    const string=v=>{if(typeof v==='symbol')throw new TypeError('Cannot convert Symbol to string');return String(v);};
    const finite=v=>{const n=+v;if(!Number.isFinite(n))throw new TypeError('Cue time must be finite');return n;};
    const quota=message=>new DOMException(message,'QuotaExceededError');
    const unsupported=message=>new DOMException(message,'NotSupportedError');
    const size=text=>apply(encode,encoder,[text]).byteLength;
    function plain(value){const text=string(value);if(text.includes('\0'))throw new TypeError('Cue contains NUL');
        if(/<[^>]*>/.test(text))throw unsupported('WebVTT cue markup/internal timestamps are unsupported');
        if(size(text)>MAX_TEXT)throw quota('Cue text exceeds 2 KiB');return text;}
    function fire(target,type,extra){const e=new Event(type);if(extra)for(const k of Object.keys(extra))define(e,k,{value:extra[k],enumerable:true});e.isTrusted=true;dispatch(target,e);}
    function queue(target,type,extra,guard){schedule(()=>{if(!guard||guard())fire(target,type,extra);});}
    function indexed(object,items){const s=lookup(listSlots,object,'Text track list');s.items=items;
        for(let i=0;i<s.indexed;i++)delete object[i];
        for(let i=0;i<items.length;i++)define(object,i,{configurable:true,enumerable:true,get(){return s.items[i];}});s.indexed=items.length;}
    class TextTrackList extends EventTarget {
        constructor(key){super();if(key!==token)throw new TypeError('Illegal constructor');const s={items:[],indexed:0,owner:null};put(listSlots,this,s);
            const proxy=new Proxy(this,{get(target,key,receiver){if(typeof key==='string'&&/^(?:0|[1-9]\d*)$/.test(key)){if(s.owner)sync(s.owner);return s.items[+key];}return Reflect.get(target,key,receiver);},ownKeys(target){if(s.owner)sync(s.owner);return Reflect.ownKeys(target);}});put(listSlots,proxy,s);return proxy;}
        get length(){const s=lookup(listSlots,this,'TextTrackList');if(s.owner)sync(s.owner);return s.items.length;}
        getTrackById(id){const v=string(id),s=lookup(listSlots,this,'TextTrackList');if(s.owner)sync(s.owner);return s.items.find(t=>track(t).id===v)||null;}
        *[Symbol.iterator](){const s=lookup(listSlots,this,'TextTrackList');if(s.owner)sync(s.owner);for(let i=0;i<s.items.length;i++)yield s.items[i];}
    }
    class TextTrackCueList {
        constructor(key){if(key!==token)throw new TypeError('Illegal constructor');put(listSlots,this,{items:[],indexed:0});}
        get length(){return lookup(listSlots,this,'TextTrackCueList').items.length;}
        getCueById(id){const v=string(id);return lookup(listSlots,this,'TextTrackCueList').items.find(c=>cue(c).id===v)||null;}
        *[Symbol.iterator](){const s=lookup(listSlots,this,'TextTrackCueList');for(let i=0;i<s.items.length;i++)yield s.items[i];}
    }
    function sortCues(s){s.items.sort((a,b)=>cue(a).start-cue(b).start||cue(b).end-cue(a).end||cue(a).order-cue(b).order);indexed(s.cues,s.items);}
    function changed(s){if(s.owner)update(s.owner);}
    class TextTrackCue extends EventTarget {
        constructor(key){super();if(key!==token)throw new TypeError('Illegal constructor');}
        get track(){return cue(this).track;}
        get id(){return cue(this).id;}
        set id(v){cue(this).id=string(v);}
        get startTime(){return cue(this).start;}
        set startTime(v){const s=cue(this);s.start=finite(v);if(s.track){const t=track(s.track);sortCues(t);changed(t);}}
        get endTime(){return cue(this).end;}
        set endTime(v){const s=cue(this);s.end=finite(v);if(s.track){const t=track(s.track);sortCues(t);changed(t);}}
        get pauseOnExit(){return cue(this).pause;}
        set pauseOnExit(v){cue(this).pause=!!v;}
    }
    class VTTCue extends TextTrackCue {
        constructor(start,end,text){if(arguments.length<3)throw new TypeError('VTTCue requires startTime, endTime and text');super(token);
            const value=plain(text);put(cueSlots,this,{id:'',start:finite(start),end:finite(end),text:value,bytes:size(value),pause:false,track:null,order:++sequence,active:false});}
        get text(){return cue(this).text;}
        set text(v){const s=cue(this),value=plain(v),bytes=size(value),t=s.track?track(s.track):null,delta=bytes-s.bytes;
            if(t&&(t.bytes+delta>MAX_TOTAL||t.owner&&t.owner.bytes+delta>MAX_TOTAL))throw quota('Text track text quota');
            if(t){t.bytes+=delta;if(t.owner)t.owner.bytes+=delta;}s.text=value;s.bytes=bytes;if(t)changed(t);}
        get region(){cue(this);return null;}
        set region(v){cue(this);if(v!==null)throw unsupported('WebVTT regions are unsupported');}
        getCueAsHTML(){const s=cue(this),f=document.createDocumentFragment();f.appendChild(document.createTextNode(s.text));return f;}
    }
    /* Nondefault layout must not be accepted and silently discarded. */
    for(const [name,value]of [['vertical',''],['snapToLines',true],['line','auto'],['lineAlign','start'],['position','auto'],['positionAlign','auto'],['size',100],['align','center']])
        define(VTTCue.prototype,name,{enumerable:true,configurable:true,get(){cue(this);return value;},set(v){cue(this);if(v!==value)throw unsupported('Nondefault WebVTT '+name+' is unsupported');}});
    class TextTrack extends EventTarget {
        constructor(key,owner,kind,label,language,element){super();if(key!==token)throw new TypeError('Illegal constructor');
            put(trackSlots,this,{object:this,owner,kind,label,language,id:'',element,mode:element?'disabled':'hidden',explicitMode:false,
                items:[],cues:new TextTrackCueList(token),active:new TextTrackCueList(token),activeItems:[],bytes:0,ready:0,epoch:0,signature:null,controller:null,error:null});}
        get kind(){return track(this).kind;}
        get label(){return track(this).label;}
        get language(){return track(this).language;}
        get id(){return track(this).id;}
        get inBandMetadataTrackDispatchType(){track(this);return '';}
        get mode(){return track(this).mode;}
        set mode(value){const s=track(this),v=string(value);if(!['disabled','hidden','showing'].includes(v))throw new TypeError('Invalid TextTrack mode');
            s.explicitMode=true;if(v===s.mode)return;s.mode=v;if(s.owner){queue(s.owner.list,'change');sync(s.owner);update(s.owner);}}
        get cues(){const s=track(this);return s.mode==='disabled'?null:s.cues;}
        get activeCues(){const s=track(this);if(s.owner)update(s.owner);return s.mode==='disabled'?null:s.active;}
        addCue(value){const s=track(this),c=cue(value);if(s.items.includes(value))return;
            if(s.items.length>=MAX_CUES||s.bytes+c.bytes>MAX_TOTAL||s.owner&&(s.owner.bytes+c.bytes>MAX_TOTAL||s.owner.cueCount>=MAX_CUES))throw quota('Text track cue quota');
            if(c.track)c.track.removeCue(value);c.track=this;s.items.push(value);s.bytes+=c.bytes;if(s.owner){s.owner.bytes+=c.bytes;s.owner.cueCount++;}sortCues(s);changed(s);}
        removeCue(value){const s=track(this),c=cue(value),i=s.items.indexOf(value);if(i<0)throw new DOMException('Cue is not in this track','NotFoundError');
            s.items.splice(i,1);s.bytes-=c.bytes;if(s.owner){s.owner.bytes-=c.bytes;s.owner.cueCount--;}c.track=null;sortCues(s);changed(s);}
    }
    function clearCues(s){for(const c of s.items){cue(c).track=null;cue(c).active=false;}
        if(s.owner){s.owner.bytes-=s.bytes;s.owner.cueCount-=s.items.length;}s.bytes=0;s.items=[];s.activeItems=[];indexed(s.cues,[]);indexed(s.active,[]);}
    function elementBrand(node){rawDom.get(node,'elementBrand');if(node.localName!=='track'||node.namespaceURI!==HTML)throw new TypeError('HTMLTrackElement receiver required');}
    function elementTrack(node){elementBrand(node);let t=apply(mapGet,elementSlots,[node]);
        if(!t){t=new TextTrack(token,null,'subtitles','','',node);put(elementSlots,node,t);}
        const p=node.parentNode;if(p&&p.namespaceURI===HTML&&(p.localName==='video'||p.localName==='audio')){const o=owner(p);sync(o);}return t;}
    class HTMLTrackElement extends HTMLElement {
        constructor(){return customElementsBridge.construct(new.target,HTMLTrackElement);}
        get kind(){elementBrand(this);const v=(this.getAttribute('kind')||'subtitles').toLowerCase();return KINDS.includes(v)?v:'metadata';}
        set kind(v){elementBrand(this);this.setAttribute('kind',string(v));}
        get src(){elementBrand(this);const v=this.getAttribute('src');return v?new URL(v,this.baseURI).href:'';}
        set src(v){elementBrand(this);this.setAttribute('src',string(v));}
        get srclang(){elementBrand(this);return this.getAttribute('srclang')||'';}
        set srclang(v){elementBrand(this);this.setAttribute('srclang',string(v));}
        get label(){elementBrand(this);return this.getAttribute('label')||'';}
        set label(v){elementBrand(this);this.setAttribute('label',string(v));}
        get default(){elementBrand(this);return this.hasAttribute('default');}
        set default(v){elementBrand(this);this.toggleAttribute('default',!!v);}
        get readyState(){return track(elementTrack(this)).ready;}
        get track(){return elementTrack(this);}
    }
    for(const [k,v]of [['NONE',0],['LOADING',1],['LOADED',2],['ERROR',3]]){define(HTMLTrackElement,k,{value:v,enumerable:true});define(HTMLTrackElement.prototype,k,{value:v,enumerable:true});}
    function timestamp(value){const m=/^(?:(\d{2,}):)?(\d{2}):(\d{2})\.(\d{3})$/.exec(value);
        if(!m||+m[2]>59||+m[3]>59)throw new TypeError('Invalid WebVTT timestamp');
        const n=(+(m[1]||0)*3600 + +m[2]*60 + +m[3])+ +m[4]/1000;if(n>1e9)throw quota('WebVTT time limit');return n;}
    function decodeEntities(text){return text.replace(/&(?:amp|lt|gt|nbsp|lrm|rlm);/g,v=>({'&amp;':'&','&lt;':'<','&gt;':'>','&nbsp;':'\u00a0','&lrm;':'\u200e','&rlm;':'\u200f'}[v]));}
    function parseVTT(text){if(text.charCodeAt(0)===0xfeff)text=text.slice(1);text=text.replace(/\r\n?/g,'\n');
        if(text.includes('\0'))throw new TypeError('WebVTT contains NUL');const lines=text.split('\n');
        if(!/^WEBVTT(?:[ \t].*)?$/.test(lines[0])||lines[0].includes('-->'))throw new TypeError('Invalid WebVTT signature');
        let i=1,total=0,last=-Infinity;const result=[];
        while(i<lines.length&&lines[i]!==''){if(lines[i].trim())throw unsupported('WebVTT header extensions are unsupported');i++;}
        if(i===lines.length&&lines.length>1)throw new TypeError('WebVTT header requires a blank line');
        while(i<lines.length){while(i<lines.length&&!lines[i])i++;if(i>=lines.length)break;
            const block=[];while(i<lines.length&&lines[i])block.push(lines[i++]);
            if(/^NOTE(?:[ \t]|$)/.test(block[0]))continue;
            if(/^(?:STYLE|REGION)(?:[ \t]|$)/.test(block[0]))throw unsupported('WebVTT STYLE/REGION blocks are unsupported');
            let id='',at=0;if(!block[0].includes('-->')){id=block[0];at++;}
            if(at>=block.length)throw new TypeError('WebVTT cue has no timing line');
            const timing=/^([^ \t]+)[ \t]+-->[ \t]+([^ \t]+)(.*)$/.exec(block[at++]);
            if(!timing)throw new TypeError('Invalid WebVTT cue timing');if(timing[3].trim())throw unsupported('WebVTT cue settings are unsupported');
            const start=timestamp(timing[1]),end=timestamp(timing[2]);if(end<=start||start<last)throw new TypeError('Invalid WebVTT cue order/duration');last=start;
            const raw=block.slice(at).join('\n');if(/<[^>]*>/.test(raw))throw unsupported('WebVTT cue markup/internal timestamps are unsupported');
            const value=decodeEntities(raw);if(size(value)>MAX_TEXT)throw quota('WebVTT cue text exceeds 2 KiB');total+=size(value);
            if(total>MAX_TOTAL||result.length>=MAX_CUES)throw quota('WebVTT cue quota');result.push({id,start,end,text:value});
        }return result;}
    function abortTrack(s){s.epoch++;if(s.controller)s.controller.abort();s.controller=null;s.signature=null;}
    function loadTrack(s,url,signature){if(s.mode==='disabled'||s.signature===signature)return;
        abortTrack(s);clearCues(s);s.signature=signature;s.ready=1;s.error=null;
        const o=s.owner,epoch=s.epoch,gen=generation(o.node),controller=new Abort();s.controller=controller;
        const current=()=>s.owner===o&&s.epoch===epoch&&s.signature===signature&&generation(o.node)===gen&&s.element.parentNode===o.node;
        (async()=>{try{
            const cross=o.node.crossOrigin;
            const response=await privateFetch(url,{signal:controller.signal,mode:cross===null?'same-origin':'cors',
                credentials:cross==='use-credentials'?'include':'same-origin',redirect:'error'});
            if(!current())return;if(!response.ok)throw new Error('Text track HTTP status '+response.status);
            const length=Number(response.headers.get('content-length'));if(Number.isFinite(length)&&length>MAX_SOURCE)throw quota('WebVTT source exceeds 512 KiB');
            const bytes=await apply(bodyBytes,response,[]);if(!current())return;if(bytes.byteLength>MAX_SOURCE)throw quota('WebVTT source exceeds 512 KiB');
            const parsed=parseVTT(apply(decode,decoder,[new Uint8Array(bytes)]));let total=0;for(const c of parsed)total+=size(c.text);
            if(o.bytes+total>MAX_TOTAL||o.cueCount+parsed.length>MAX_CUES)throw quota('Media text track aggregate quota');
            /* All parse/quota checks precede publication. Encoded angle brackets
             * are literal text, not markup; bypass constructor's raw markup gate. */
            for(const c of parsed){const value=new VTTCue(c.start,c.end,'');const v=cue(value);v.text=c.text;v.bytes=size(c.text);v.id=c.id;v.track=s.object;s.items.push(value);s.bytes+=v.bytes;o.bytes+=v.bytes;o.cueCount++;}
            sortCues(s);s.ready=2;s.controller=null;update(o);queue(s.element,'load',null,current);
        }catch(e){if(!current())return;s.ready=3;s.controller=null;s.error=e;clearCues(s);update(o);
            host.log(2,'TextTrack: '+String(e));queue(s.element,'error',null,current);}})();
    }
    function removeTrack(o,t){const s=track(t);abortTrack(s);s.owner=null;o.bytes-=s.bytes;o.cueCount-=s.items.length;const i=o.items.indexOf(t);if(i>=0)o.items.splice(i,1);
        for(const c of s.activeItems)cue(c).active=false;s.activeItems=[];indexed(s.active,[]);queue(o.list,'removetrack',{track:t});}
    function sync(o){if(o.syncing)return;o.syncing=true;try{
        const elements=[];for(const node of o.node.children)if(node.namespaceURI===HTML&&node.localName==='track')elements.push(node);
        if(elements.length+o.items.filter(t=>!track(t).element).length>MAX_TRACKS)throw quota('At most 16 text tracks per media');
        for(const t of o.items.slice())if(track(t).element&&!elements.includes(track(t).element))removeTrack(o,t);
        for(const element of elements){let t=apply(mapGet,elementSlots,[element]);if(!t){t=new TextTrack(token,null,'subtitles','','',element);put(elementSlots,element,t);}const s=track(t);
            if(s.owner!==o){if(o.bytes+s.bytes>MAX_TOTAL||o.cueCount+s.items.length>MAX_CUES)throw quota('Media text track aggregate quota');if(s.owner)removeTrack(s.owner,t);s.owner=o;o.items.push(t);o.bytes+=s.bytes;o.cueCount+=s.items.length;queue(o.list,'addtrack',{track:t});}
            const kind=(element.getAttribute('kind')||'subtitles').toLowerCase();s.kind=KINDS.includes(kind)?kind:'metadata';s.label=element.getAttribute('label')||'';s.language=element.getAttribute('srclang')||'';s.id=element.id;
            if(!s.explicitMode&&element.hasAttribute('default')&&!o.items.some(x=>x!==t&&track(x).mode==='showing'&&['captions','subtitles'].includes(track(x).kind)))s.mode='showing';
            const raw=element.getAttribute('src');let url='',urlError=null;try{if(raw)url=new URL(raw,element.baseURI).href;}catch(e){urlError=e;}
            const signature=generation(o.node)+'|'+o.node.crossOrigin+'|'+(urlError?'invalid:'+raw:url);
            if(s.signature!==null&&s.signature!==signature){abortTrack(s);clearCues(s);s.ready=0;}
            if(urlError){if(s.signature!==signature){s.signature=signature;s.ready=3;s.error=urlError;queue(element,'error');}}
            else if(url)loadTrack(s,url,signature);
        }
        o.items.sort((a,b)=>{const x=track(a).element,y=track(b).element;return x&&y?elements.indexOf(x)-elements.indexOf(y):x?-1:y?1:0;});indexed(o.list,o.items);
    }finally{o.syncing=false;}}
    function update(o,seeking=false){if(o.busy)return;o.busy=true;try{
        const state=mediaState(o.node),gen=generation(o.node),time=state.currentTime,oldTime=o.time;
        if(gen!==o.generation){o.generation=gen;o.time=null;seeking=true;}seeking=seeking||state.seeking;
        const parts=[];let pause=false,renderedBytes=0,displayOverflow=false;
        for(const t of o.items){const s=track(t),before=s.activeItems,after=s.mode==='disabled'||state.readyState<1?[]:s.items.filter(c=>{const v=cue(c);return v.start<=time&&time<v.end;});
            const current=()=>s.owner===o&&generation(o.node)===gen;
            const exits=before.filter(c=>!after.includes(c)),enters=after.filter(c=>!before.includes(c));
            for(const c of exits){const v=cue(c);v.active=false;queue(c,'exit',null,current);if(!seeking&&!state.paused&&oldTime!==null&&time>=v.end&&oldTime<v.end&&v.pause)pause=true;}
            for(const c of enters){cue(c).active=true;queue(c,'enter',null,current);}
            let missed=false;if(!seeking&&!state.paused&&oldTime!==null&&time>oldTime&&s.mode!=='disabled'&&state.readyState>=1)
                for(const c of s.items){const v=cue(c);if(v.start>oldTime&&v.end<=time&&!before.includes(c)&&!after.includes(c)){queue(c,'enter',null,current);queue(c,'exit',null,current);missed=true;if(v.pause)pause=true;}}
            s.activeItems=after;indexed(s.active,after);if(exits.length||enters.length||missed){queue(t,'cuechange',null,current);if(s.element)queue(s.element,'cuechange',null,current);}
            if(s.mode==='showing'&&['subtitles','captions'].includes(s.kind))for(const c of after){const v=cue(c);renderedBytes+=v.bytes+(parts.length?1:0);if(renderedBytes>MAX_TEXT)displayOverflow=true;else parts.push(v.text);}
        }
        o.time=time;let caption=displayOverflow?'':parts.join('\n');if(displayOverflow){if(!o.displayError){o.displayError=true;host.log(2,'TextTrack: active caption text exceeds 2 KiB');}}else o.displayError=false;
        if(caption!==o.caption){native('captionText',o.node,caption);o.caption=caption;}
        if(pause&&!state.paused)o.node.pause();if(!state.paused&&o.items.some(t=>track(t).mode!=='disabled'))poll();
    }finally{o.busy=false;}}
    function poll(){if(pollTimer)return;pollTimer=host.timer(0,()=>{pollTimer=0;for(const ref of Array.from(refs)){const o=ref.deref();if(!o){refs.delete(ref);continue;}update(o);}},40,[]);}
    function owner(node){brandMedia(node);let o=apply(mapGet,mediaSlots,[node]);if(o)return o;
        for(const ref of Array.from(refs))if(!ref.deref())refs.delete(ref);if(refs.size>=MAX_MEDIA)throw quota('Text track media limit');
        o={node,list:new TextTrackList(token),items:[],bytes:0,cueCount:0,time:null,generation:generation(node),caption:'',displayError:false,busy:false,syncing:false};put(mediaSlots,node,o);refs.add(new WeakRef(o));
        lookup(listSlots,o.list,'TextTrackList').owner=o;
        const listener=e=>{try{sync(o);update(o,e.type==='seeking'||e.type==='seeked'||e.type==='emptied'||e.type==='loadstart');}catch(error){report(error);}};
        for(const type of ['timeupdate','seeking','seeked','play','pause','loadedmetadata','emptied','loadstart'])node.addEventListener(type,listener);
        o.observer=new MutationObserver(()=>listener({type:'mutation'}));o.observer.observe(node,{childList:true,subtree:true,attributes:true,attributeFilter:['src','kind','label','srclang','default','id','crossorigin']});return o;
    }
    function manage(node){const o=owner(node);sync(o);update(o);return o.list;}
    function addTextTrack(node,kind,label='',language=''){const o=owner(node),k=string(kind);if(!KINDS.includes(k))throw new TypeError('Invalid TextTrack kind');
        sync(o);if(o.items.length>=MAX_TRACKS)throw quota('At most 16 text tracks per media');const t=new TextTrack(token,o,k,string(label),string(language),null);
        o.items.push(t);indexed(o.list,o.items);queue(o.list,'addtrack',{track:t});update(o);return t;}
    function reset(node){const o=apply(mapGet,mediaSlots,[node]);if(!o)return;o.time=null;
        for(const t of o.items){const s=track(t);if(s.element){abortTrack(s);clearCues(s);s.ready=0;}for(const c of s.activeItems)cue(c).active=false;s.activeItems=[];indexed(s.active,[]);}
        if(o.caption){native('captionText',node,'');o.caption='';}}
    const events=['change','addtrack','removetrack'];
    textTrackHandlerTarget=(v,type)=>apply(mapGet,trackSlots,[v])!==undefined&&type==='cuechange'||apply(mapGet,cueSlots,[v])!==undefined&&['enter','exit'].includes(type)||apply(mapGet,listSlots,[v])!==undefined&&v instanceof TextTrackList&&events.includes(type)||v instanceof HTMLTrackElement&&['load','error','cuechange'].includes(type);
    installHandlers(TextTrackList.prototype,events);installHandlers(TextTrack.prototype,['cuechange']);installHandlers(TextTrackCue.prototype,['enter','exit']);installHandlers(HTMLTrackElement.prototype,['load','error','cuechange']);
    for(const C of [TextTrackList,TextTrackCueList,TextTrack,TextTrackCue,VTTCue,HTMLTrackElement]){define(C.prototype,Symbol.toStringTag,{value:C.name,configurable:true});for(const k of Object.getOwnPropertyNames(C.prototype))if(k!=='constructor'){const d=Object.getOwnPropertyDescriptor(C.prototype,k);d.enumerable=true;define(C.prototype,k,d);}}
    Object.assign(globalThis,{TextTrackList,TextTrackCueList,TextTrack,TextTrackCue,VTTCue,HTMLTrackElement});
    cloneData.registerUncloneable(v=>apply(mapGet,trackSlots,[v])!==undefined||apply(mapGet,cueSlots,[v])!==undefined||apply(mapGet,listSlots,[v])!==undefined);
    /* Native parser-created media/track elements also work without author
     * touching textTracks. DOM mutations trigger bounded rescan. */
    let discoverPending=false;
    function discover(){if(discoverPending)return;discoverPending=true;schedule(()=>{discoverPending=false;try{for(const node of document.querySelectorAll('video,audio'))manage(node);}catch(e){report(e);}});}
    const documentObserver=new MutationObserver(discover);documentObserver.observe(document,{childList:true,subtree:true});discover();
    return {manage,addTextTrack,reset,nodeProtos:[HTMLTrackElement.prototype]};
})();
