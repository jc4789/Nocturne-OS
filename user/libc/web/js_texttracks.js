/* 実media時刻に結線したWebVTT字幕。cue tree/設定は既存native字幕へ渡す。
 * 外部player/別DOMoverlayなし。STYLE stylesheetとin-band抽出は別責務。
 * https://html.spec.whatwg.org/multipage/media.html#text-track-api
 * https://www.w3.org/TR/webvtt1/ */
const textTrackBridge = (() => {
    'use strict';
    const native=host.avmedia, brandMedia=avmediaBridge.brand, mediaState=avmediaBridge.state;
    const generation=avmediaBridge.generation, trackFetch=avmediaBridge.trackFetch;
    const apply=Reflect.apply, define=Object.defineProperty;
    const mapGet=WeakMap.prototype.get, mapSet=WeakMap.prototype.set;
    const mediaSlots=new WeakMap(), trackSlots=new WeakMap(), cueSlots=new WeakMap(), listSlots=new WeakMap(), elementSlots=new WeakMap(),regionSlots=new WeakMap();
    const token={}, HTML='http://www.w3.org/1999/xhtml', KINDS=['subtitles','captions','descriptions','chapters','metadata'];
    const schedule=fn=>host.timer(0,fn,0,[]), encoder=new TextEncoder(), encode=TextEncoder.prototype.encode;
    const decoder=new TextDecoder('utf-8',{fatal:true}), decode=TextDecoder.prototype.decode;
    const Abort=fetchBridge.AbortController, bodyBytes=fetchBridge.Response.prototype.arrayBuffer;
    const refs=new Set();
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
        return text;}
    const defaults=()=>({region:null,vertical:'',snapToLines:true,line:'auto',lineAlign:'start',position:'auto',positionAlign:'auto',size:100,align:'center'});
    function percent(value){const n=finite(value);if(n<0||n>100)throw new DOMException('Percentage is outside 0..100','IndexSizeError');return n;}
    class VTTRegion {
        constructor(){put(regionSlots,this,{id:'',serial:++sequence,width:100,lines:3,regionAnchorX:0,regionAnchorY:100,viewportAnchorX:0,viewportAnchorY:100,scroll:''});}
    }
    for(const name of ['id','width','lines','regionAnchorX','regionAnchorY','viewportAnchorX','viewportAnchorY','scroll'])define(VTTRegion.prototype,name,{enumerable:true,configurable:true,get(){return lookup(regionSlots,this,'VTTRegion')[name];},set(v){const r=lookup(regionSlots,this,'VTTRegion');if(name==='id')v=string(v);else if(name==='scroll'){v=string(v);if(v!==''&&v!=='up')throw new TypeError('Invalid region scroll');}else if(name==='lines')v=Number(v)>>>0;else v=percent(v);if(v!==r[name]){r[name]=v;for(const ref of refs){const o=ref.deref();if(o)update(o);}}}});
    function parseText(text,start=-Infinity,end=Infinity){const root={name:'',children:[]},stack=[root];let last=start;const pattern=/<([^>]*)>|([^<]+)|</g;let match;
        while((match=pattern.exec(text))){const current=stack[stack.length-1];if(match[2]!==undefined){current.children.push({text:decodeEntities(match[2])});continue;}if(match[0]==='<'){current.children.push({text:'<'});continue;}const tag=match[1];
            if(/^\d/.test(tag)){let time;try{time=timestamp(tag);}catch(_){continue;}if(time>last&&time<end){current.children.push({time,source:tag});last=time;}continue;}
            if(tag.startsWith('/')){const name=tag.slice(1).trim();if(stack.length>1&&name===current.name)stack.pop();else if(name==='ruby'&&current.name==='rt'&&stack[stack.length-2]?.name==='ruby'){stack.pop();stack.pop();}continue;}
            const m=/^([^\.\s]+)((?:\.[^\s.]*)*)(?:\s+([\s\S]*))?$/.exec(tag);if(!m)continue;const name=m[1];if(!['c','b','i','u','ruby','rt','v','lang'].includes(name)||name==='rt'&&current.name!=='ruby')continue;const node={name,classes:m[2]?m[2].slice(1).split('.'):[],annotation:m[3]||'',children:[]};current.children.push(node);stack.push(node);
        }return root;
    }
    function cueHTML(tree){const fragment=document.createDocumentFragment();function walk(node,parent){if('text'in node){parent.appendChild(document.createTextNode(node.text));return;}if('time'in node){parent.appendChild(document.createProcessingInstruction('timestamp',node.source));return;}let target=parent;if(node.name){target=document.createElement(['c','v','lang'].includes(node.name)?'span':node.name);if(node.classes.length)target.className=node.classes.join(' ');if(node.name==='v')target.title=node.annotation;if(node.name==='lang')target.lang=node.annotation;parent.appendChild(target);}for(const child of node.children)walk(child,target);}walk(tree,fragment);return fragment;}
    const colors={white:0xffffffff,lime:0xff00ff00,cyan:0xff00ffff,red:0xffff0000,yellow:0xffffff00,magenta:0xffff00ff,blue:0xff0000ff,black:0xff000000};
    function cueRuns(tree,time){const runs=[];let stamp=-Infinity;function textContent(node){return 'text'in node?node.text:node.children?node.children.map(textContent).join(''):'';}
        function walk(node,flags=0,color=0xffffffff){if('time'in node){stamp=node.time;return;}if('text'in node){if(node.text)runs.push({text:node.text,flags,color,past:stamp<=time,ruby:''});return;}if(node.name==='b')flags|=1;if(node.name==='i')flags|=2;if(node.name==='u')flags|=4;for(const c of node.classes||[])if(Object.hasOwn(colors,c))color=colors[c];if(node.name==='ruby'){const base=node.children.filter(c=>c.name!=='rt'),annotation=node.children.filter(c=>c.name==='rt').map(textContent).join('');const before=runs.length;for(const c of base)walk(c,flags,color);if(annotation&&runs.length>before)runs[before].ruby=annotation;return;}for(const c of node.children||[])walk(c,flags,color);}walk(tree);return runs;
    }
    function fire(target,type,extra){const e=new Event(type);if(extra)for(const k of Object.keys(extra))define(e,k,{value:extra[k],enumerable:true});eventState(e).isTrusted=true;dispatch(target,e);}
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
        set startTime(v){const s=cue(this);s.start=finite(v);s.tree=null;if(s.track){const t=track(s.track);sortCues(t);changed(t);}}
        get endTime(){return cue(this).end;}
        set endTime(v){const s=cue(this);s.end=finite(v);s.tree=null;if(s.track){const t=track(s.track);sortCues(t);changed(t);}}
        get pauseOnExit(){return cue(this).pause;}
        set pauseOnExit(v){cue(this).pause=!!v;}
    }
    class VTTCue extends TextTrackCue {
        constructor(start,end,text){if(arguments.length<3)throw new TypeError('VTTCue requires startTime, endTime and text');super(token);
            const value=plain(text);put(cueSlots,this,{id:'',start:finite(start),end:finite(end),text:value,tree:null,bytes:size(value),pause:false,track:null,order:++sequence,active:false,...defaults()});}
        get text(){return cue(this).text;}
        set text(v){const s=cue(this),value=plain(v),bytes=size(value),t=s.track?track(s.track):null,delta=bytes-s.bytes;
            if(t){t.bytes+=delta;if(t.owner)t.owner.bytes+=delta;}s.text=value;s.tree=null;s.bytes=bytes;if(t)changed(t);}
        get region(){return cue(this).region;}
        set region(v){const s=cue(this);if(v!==null)lookup(regionSlots,v,'VTTRegion');s.region=v;if(s.track)changed(track(s.track));}
        getCueAsHTML(){const s=cue(this);return cueHTML(s.tree||(s.tree=parseText(s.text,s.start,s.end)));}
    }
    const enums={vertical:['','rl','lr'],lineAlign:['start','center','end'],positionAlign:['auto','line-left','center','line-right'],align:['start','center','end','left','right']};
    for(const name of ['vertical','snapToLines','line','lineAlign','position','positionAlign','size','align'])define(VTTCue.prototype,name,{enumerable:true,configurable:true,get(){return cue(this)[name];},set(v){const s=cue(this);if(enums[name]){v=string(v);if(!enums[name].includes(v))throw new TypeError('Invalid '+name);}else if(name==='snapToLines')v=!!v;else if((name==='line'||name==='position')&&v==='auto'){}else v=name==='line'?finite(v):percent(v);if(v!==s[name]){s[name]=v;if(s.track)changed(track(s.track));}}});
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
        const n=(+(m[1]||0)*3600 + +m[2]*60 + +m[3])+ +m[4]/1000;return n;}
    function decodeEntities(text){return text.replace(/&(?:amp|lt|gt|nbsp|lrm|rlm);/g,v=>({'&amp;':'&','&lt;':'<','&gt;':'>','&nbsp;':'\u00a0','&lrm;':'\u200e','&rlm;':'\u200f'}[v]));}
    function settings(text,regions){const result=defaults(),seen=new Set();for(const setting of text.trim().split(/[ \t]+/)){const colon=setting.indexOf(':');if(colon<1)continue;const name=setting.slice(0,colon),value=setting.slice(colon+1);if(seen.has(name))continue;let valid=false;
            if(name==='region'&&regions.has(value)){result.region=regions.get(value);valid=true;}
            else if(name==='vertical'&&['rl','lr'].includes(value)){result.vertical=value;valid=true;}
            else if(name==='align'&&enums.align.includes(value)){result.align=value;valid=true;}
            else if(name==='size'&&/^\d+(?:\.\d+)?%$/.test(value)&&Number(value.slice(0,-1))<=100){result.size=Number(value.slice(0,-1));valid=true;}
            else if(name==='line'||name==='position'){const [number,alignment,...rest]=value.split(',');if(rest.length)continue;const percentValue=/^\d+(?:\.\d+)?%$/.test(number),numeric=percentValue?Number(number.slice(0,-1)):Number(number);const allowed=name==='line'?enums.lineAlign:enums.positionAlign.filter(v=>v!=='auto');if(alignment!==undefined&&!allowed.includes(alignment))continue;
                if(name==='line'&&(number==='auto'||/^-?\d+$/.test(number)||percentValue&&numeric<=100)){result.line=number==='auto'?'auto':numeric;result.snapToLines=!percentValue;if(alignment)result.lineAlign=alignment;valid=true;}
                else if(name==='position'&&percentValue&&numeric<=100){result.position=numeric;if(alignment)result.positionAlign=alignment;valid=true;}}
            if(valid)seen.add(name);
        }return result;}
    function parseRegion(block){const region=new VTTRegion(),seen=new Set();for(const field of block.slice(1).join(' ').split(/[ \t]+/)){const split=field.indexOf(':');if(split<1)continue;const name=field.slice(0,split),v=field.slice(split+1);if(seen.has(name))continue;try{if(name==='id')region.id=v;else if(name==='width'&&/%$/.test(v))region.width=Number(v.slice(0,-1));else if(name==='lines'&&/^\d+$/.test(v))region.lines=Number(v);else if(name==='scroll'&&v==='up')region.scroll=v;else if(name==='regionanchor'||name==='viewportanchor'){const m=/^(\d+(?:\.\d+)?)%,(\d+(?:\.\d+)?)%$/.exec(v);if(!m)continue;region[name==='regionanchor'?'regionAnchorX':'viewportAnchorX']=+m[1];region[name==='regionanchor'?'regionAnchorY':'viewportAnchorY']=+m[2];}else continue;seen.add(name);}catch(_){}}
        return region;}
    function parseVTT(text){if(text.charCodeAt(0)===0xfeff)text=text.slice(1);text=text.replace(/\r\n?/g,'\n');
        if(text.includes('\0'))throw new TypeError('WebVTT contains NUL');const lines=text.split('\n');
        if(!/^WEBVTT(?:[ \t].*)?$/.test(lines[0])||lines[0].includes('-->'))throw new TypeError('Invalid WebVTT signature');
        let i=1;const result=[],regions=new Map();
        while(i<lines.length&&lines[i]!=='')i++; /* header lines after WEBVTT carry no cues */
        if(i===lines.length&&lines.length>1)throw new TypeError('WebVTT header requires a blank line');
        while(i<lines.length){while(i<lines.length&&!lines[i])i++;if(i>=lines.length)break;
            const block=[];while(i<lines.length&&lines[i])block.push(lines[i++]);
            if(/^NOTE(?:[ \t]|$)/.test(block[0]))continue;
            if(/^REGION(?:[ \t]|$)/.test(block[0])){const region=parseRegion(block);if(region.id)regions.set(region.id,region);continue;}
            if(/^STYLE(?:[ \t]|$)/.test(block[0]))continue; /* cue stylesheets are not applied; the cues still are */
            let id='',at=0;if(!block[0].includes('-->')){id=block[0];at++;}
            if(at>=block.length)continue;
            const timing=/^([^ \t]+)[ \t]+-->[ \t]+([^ \t]+)(.*)$/.exec(block[at++]);
            if(!timing)continue;const layout=settings(timing[3],regions);
            let start,end;try{start=timestamp(timing[1]);end=timestamp(timing[2]);}catch(_){continue;}if(end<=start)continue;
            const raw=block.slice(at).join('\n');
            result.push({id,start,end,text:raw,layout});
        }return result;}
    function abortTrack(s){s.epoch++;if(s.controller)s.controller.abort();s.controller=null;s.signature=null;}
    function loadTrack(s,url,signature){if(s.mode==='disabled'||s.signature===signature)return;
        abortTrack(s);clearCues(s);s.signature=signature;s.ready=1;s.error=null;
        const o=s.owner,epoch=s.epoch,gen=generation(o.node),controller=new Abort();s.controller=controller;
        const current=()=>s.owner===o&&s.epoch===epoch&&s.signature===signature&&generation(o.node)===gen&&s.element.parentNode===o.node;
        (async()=>{try{
            const cross=o.node.crossOrigin;
            const response=await trackFetch(url,{signal:controller.signal,mode:cross===null?'same-origin':'cors',
                credentials:cross==='use-credentials'?'include':'same-origin',redirect:'error'});
            if(!current())return;if(!response.ok)throw new Error('Text track HTTP status '+response.status);
            const bytes=await apply(bodyBytes,response,[]);if(!current())return;
            const parsed=parseVTT(apply(decode,decoder,[new Uint8Array(bytes)]));
            /* Raw cue markup stays raw; entity decoding happens once in the
             * cue text tokenizer, never through the document HTML parser. */
            for(const c of parsed){const value=new VTTCue(c.start,c.end,c.text);const v=cue(value);Object.assign(v,c.layout);v.id=c.id;v.track=s.object;s.items.push(value);s.bytes+=v.bytes;o.bytes+=v.bytes;o.cueCount++;}
            sortCues(s);s.ready=2;s.controller=null;update(o);queue(s.element,'load',null,current);
        }catch(e){if(!current())return;s.ready=3;s.controller=null;s.error=e;clearCues(s);update(o);
            host.log(2,'TextTrack: '+String(e));queue(s.element,'error',null,current);}})();
    }
    function removeTrack(o,t){const s=track(t);abortTrack(s);s.owner=null;o.bytes-=s.bytes;o.cueCount-=s.items.length;const i=o.items.indexOf(t);if(i>=0)o.items.splice(i,1);
        for(const c of s.activeItems)cue(c).active=false;s.activeItems=[];indexed(s.active,[]);queue(o.list,'removetrack',{track:t});}
    function sync(o){if(o.syncing)return;o.syncing=true;try{
        const elements=[];for(const node of o.node.children)if(node.namespaceURI===HTML&&node.localName==='track')elements.push(node);
        for(const t of o.items.slice())if(track(t).element&&!elements.includes(track(t).element))removeTrack(o,t);
        for(const element of elements){let t=apply(mapGet,elementSlots,[element]);if(!t){t=new TextTrack(token,null,'subtitles','','',element);put(elementSlots,element,t);}const s=track(t);
            if(s.owner!==o){if(s.owner)removeTrack(s.owner,t);s.owner=o;o.items.push(t);o.bytes+=s.bytes;o.cueCount+=s.items.length;queue(o.list,'addtrack',{track:t});}
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
        const parts=[];let pause=false;
        for(const t of o.items){const s=track(t),before=s.activeItems,after=s.mode==='disabled'||state.readyState<1?[]:s.items.filter(c=>{const v=cue(c);return v.start<=time&&time<v.end;});
            const current=()=>s.owner===o&&generation(o.node)===gen;
            const exits=before.filter(c=>!after.includes(c)),enters=after.filter(c=>!before.includes(c));
            for(const c of exits){const v=cue(c);v.active=false;queue(c,'exit',null,current);if(!seeking&&!state.paused&&oldTime!==null&&time>=v.end&&oldTime<v.end&&v.pause)pause=true;}
            for(const c of enters){cue(c).active=true;queue(c,'enter',null,current);}
            let missed=false;if(!seeking&&!state.paused&&oldTime!==null&&time>oldTime&&s.mode!=='disabled'&&state.readyState>=1)
                for(const c of s.items){const v=cue(c);if(v.start>oldTime&&v.end<=time&&!before.includes(c)&&!after.includes(c)){queue(c,'enter',null,current);queue(c,'exit',null,current);missed=true;if(v.pause)pause=true;}}
            s.activeItems=after;indexed(s.active,after);if(exits.length||enters.length||missed){queue(t,'cuechange',null,current);if(s.element)queue(s.element,'cuechange',null,current);}
            if(s.mode==='showing'&&['subtitles','captions'].includes(s.kind))for(const c of after){const v=cue(c);{const region=v.region?{...lookup(regionSlots,v.region,'VTTRegion')}:null;parts.push({spans:cueRuns(v.tree||(v.tree=parseText(v.text,v.start,v.end)),time),vertical:v.vertical,snapToLines:v.snapToLines,line:v.line,lineAlign:v.lineAlign,position:v.position,positionAlign:v.positionAlign,size:v.size,align:v.align,region});}}
        }
        o.time=time;const caption=JSON.stringify(parts);
        if(caption!==o.caption){native('captionCues',o.node,parts);o.caption=caption;}
        if(pause&&!state.paused)o.node.pause();if(!state.paused&&o.items.some(t=>track(t).mode!=='disabled'))poll();
    }finally{o.busy=false;}}
    function poll(){if(pollTimer)return;pollTimer=host.timer(0,()=>{pollTimer=0;for(const ref of Array.from(refs)){const o=ref.deref();if(!o){refs.delete(ref);continue;}update(o);}},40,[]);}
    function owner(node){brandMedia(node);let o=apply(mapGet,mediaSlots,[node]);if(o)return o;
        for(const ref of Array.from(refs))if(!ref.deref())refs.delete(ref);
        o={node,list:new TextTrackList(token),items:[],bytes:0,cueCount:0,time:null,generation:generation(node),caption:'',busy:false,syncing:false};put(mediaSlots,node,o);refs.add(new WeakRef(o));
        lookup(listSlots,o.list,'TextTrackList').owner=o;
        const listener=e=>{try{sync(o);update(o,e.type==='seeking'||e.type==='seeked'||e.type==='emptied'||e.type==='loadstart');}catch(error){report(error);}};
        for(const type of ['timeupdate','seeking','seeked','play','pause','loadedmetadata','emptied','loadstart'])node.addEventListener(type,listener);
        o.observer=new MutationObserver(()=>listener({type:'mutation'}));o.observer.observe(node,{childList:true,subtree:true,attributes:true,attributeFilter:['src','kind','label','srclang','default','id','crossorigin']});return o;
    }
    function manage(node){const o=owner(node);sync(o);update(o);return o.list;}
    function addTextTrack(node,kind,label='',language=''){const o=owner(node),k=string(kind);if(!KINDS.includes(k))throw new TypeError('Invalid TextTrack kind');
        sync(o);const t=new TextTrack(token,o,k,string(label),string(language),null);
        o.items.push(t);indexed(o.list,o.items);queue(o.list,'addtrack',{track:t});update(o);return t;}
    function reset(node){const o=apply(mapGet,mediaSlots,[node]);if(!o)return;o.time=null;
        for(const t of o.items){const s=track(t);if(s.element){abortTrack(s);clearCues(s);s.ready=0;}for(const c of s.activeItems)cue(c).active=false;s.activeItems=[];indexed(s.active,[]);}
        if(o.caption){native('captionCues',node,[]);o.caption='';}}
    const events=['change','addtrack','removetrack'];
    textTrackHandlerTarget=(v,type)=>apply(mapGet,trackSlots,[v])!==undefined&&type==='cuechange'||apply(mapGet,cueSlots,[v])!==undefined&&['enter','exit'].includes(type)||apply(mapGet,listSlots,[v])!==undefined&&v instanceof TextTrackList&&events.includes(type)||v instanceof HTMLTrackElement&&['load','error','cuechange'].includes(type);
    installHandlers(TextTrackList.prototype,events);installHandlers(TextTrack.prototype,['cuechange']);installHandlers(TextTrackCue.prototype,['enter','exit']);installHandlers(HTMLTrackElement.prototype,['load','error','cuechange']);
    for(const C of [TextTrackList,TextTrackCueList,TextTrack,TextTrackCue,VTTCue,VTTRegion,HTMLTrackElement]){define(C.prototype,Symbol.toStringTag,{value:C.name,configurable:true});for(const k of Object.getOwnPropertyNames(C.prototype))if(k!=='constructor'){const d=Object.getOwnPropertyDescriptor(C.prototype,k);d.enumerable=true;define(C.prototype,k,d);}}
    Object.assign(globalThis,{TextTrackList,TextTrackCueList,TextTrack,TextTrackCue,VTTCue,VTTRegion,HTMLTrackElement});
    cloneData.registerUncloneable(v=>apply(mapGet,trackSlots,[v])!==undefined||apply(mapGet,cueSlots,[v])!==undefined||apply(mapGet,listSlots,[v])!==undefined||apply(mapGet,regionSlots,[v])!==undefined);
    /* Native parser-created media/track elements also work without author
     * touching textTracks. DOM mutations trigger bounded rescan. */
    let discoverPending=false;
    function discover(){if(discoverPending)return;discoverPending=true;schedule(()=>{discoverPending=false;for(const node of document.querySelectorAll('video,audio')){
        /* A managed element follows its own tracks through its observer and
         * media events; only newly inserted media need a first scan. */
        if(apply(mapGet,mediaSlots,[node]))continue;try{manage(node);}catch(e){report(e);}}});}
    const documentObserver=new MutationObserver(discover);documentObserver.observe(document,{childList:true,subtree:true});discover();
    return {manage,addTextTrack,reset,nodeProtos:[HTMLTrackElement.prototype]};
})();
