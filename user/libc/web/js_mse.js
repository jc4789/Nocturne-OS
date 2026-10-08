/* MSE: 実packet buffer/FFmpegへ接続する文書内実装。DRM・worker handleは公開しない。
 * https://www.w3.org/TR/media-source-2/ (2026-10-08) */
const mseBridge = (() => {
    'use strict';
    const native=host.avmedia, sources=new WeakMap(),buffers=new WeakMap(),lists=new WeakMap(),ranges=new WeakMap(),attachments=new WeakMap();
    const tracksData=new WeakMap(),nodeTrackLists=new WeakMap(),trackSlots=new WeakMap();
    const apply=Reflect.apply,define=Object.defineProperty,create=Object.create;
    const get=WeakMap.prototype.get,put=WeakMap.prototype.set,del=WeakMap.prototype.delete;
    const U8=Uint8Array,AB=ArrayBuffer,isView=AB.isView,ta=Object.getPrototypeOf(U8.prototype);
    const getter=(p,k)=>Object.getOwnPropertyDescriptor(p,k).get;
    const taBuffer=getter(ta,'buffer'),taOffset=getter(ta,'byteOffset'),taLength=getter(ta,'byteLength'),taTag=getter(ta,Symbol.toStringTag);
    const abLength=getter(AB.prototype,'byteLength'),dvBuffer=getter(DataView.prototype,'buffer'),dvOffset=getter(DataView.prototype,'byteOffset'),dvLength=getter(DataView.prototype,'byteLength'),set=U8.prototype.set;
    const EventType=Event,ErrorType=DOMException,token={},schedule=(fn)=>host.timer(0,fn,0,[]),MAX=32*1024*1024;
    const read=(map,value,label)=>{const s=apply(get,map,[value]);if(!s)throw new TypeError('Illegal '+label+' receiver');return s;};
    cloneData.registerUncloneable(value=>apply(get,sources,[value])!==undefined||apply(get,buffers,[value])!==undefined||apply(get,lists,[value])!==undefined||apply(get,ranges,[value])!==undefined||apply(get,tracksData,[value])!==undefined||apply(get,trackSlots,[value])!==undefined);
    const source=v=>read(sources,v,'MediaSource'),buffer=v=>read(buffers,v,'SourceBuffer');
    const str=v=>{if(typeof v==='symbol')throw new TypeError('Cannot convert Symbol to string');return String(v);};
    const error=(name,message)=>new ErrorType(message,name);
    const finite=v=>{const n=+v;if(!Number.isFinite(n))throw new TypeError('Expected finite number');return n;};
    function fire(target,type){const e=new EventType(type);e.isTrusted=true;dispatch(target,e);}
    function queue(target,type){schedule(()=>fire(target,type));}
    function bytes(value){
        let b=value,o=0,n;
        if(isView(value)){const typed=apply(taTag,value,[])!==undefined;b=apply(typed?taBuffer:dvBuffer,value,[]);o=apply(typed?taOffset:dvOffset,value,[]);n=apply(typed?taLength:dvLength,value,[]);}
        const size=apply(abLength,b,[]),input=new U8(b,o,n===undefined?size:n);n=apply(taLength,input,[]);
        if(n>MAX)throw error('QuotaExceededError','Append exceeds 32 MiB');
        const out=new U8(n);apply(set,out,[input]);return apply(taBuffer,out,[]);
    }
    class TimeRanges {
        constructor(key,data){if(key!==token)throw new TypeError('Illegal constructor');apply(put,ranges,[this,data]);}
        get length(){return read(ranges,this,'TimeRanges').length;}
        start(index){if(!arguments.length)throw new TypeError('Index required');const a=read(ranges,this,'TimeRanges'),i=+index>>>0;if(i>=a.length)throw error('IndexSizeError','Range index is out of bounds');return a[i][0];}
        end(index){if(!arguments.length)throw new TypeError('Index required');const a=read(ranges,this,'TimeRanges'),i=+index>>>0;if(i>=a.length)throw error('IndexSizeError','Range index is out of bounds');return a[i][1];}
    }
    function timeRanges(data=[]){const copy=[];for(let i=0;i<data.length;i++)copy.push([data[i][0],data[i][1]]);return new TimeRanges(token,copy);}
    function refreshList(list){
        const s=read(lists,list,'SourceBufferList');
        for(let i=0;i<s.indexed;i++)delete list[i];
        for(let i=0;i<s.items.length;i++)define(list,i,{configurable:true,enumerable:true,get(){return s.items[i];}});
        s.indexed=s.items.length;
    }
    class SourceBufferList extends EventTarget {
        constructor(key){super();if(key!==token)throw new TypeError('Illegal constructor');apply(put,lists,[this,{items:[],indexed:0}]);}
        get length(){return read(lists,this,'SourceBufferList').items.length;}
        item(index){const s=read(lists,this,'SourceBufferList');return s.items[+index>>>0]||null;}
        *[Symbol.iterator](){const s=read(lists,this,'SourceBufferList');for(let i=0;i<s.items.length;i++)yield s.items[i];}
    }
    class MediaTrackList extends EventTarget {
        constructor(key){super();if(key!==token)throw new TypeError('Illegal constructor');apply(put,tracksData,[this,{items:[],indexed:0}]);}
        get length(){return read(tracksData,this,'TrackList').items.length;}
        getTrackById(id){const a=read(tracksData,this,'TrackList').items,v=str(id);for(let i=0;i<a.length;i++)if(a[i].id===v)return a[i];return null;}
        *[Symbol.iterator](){const a=read(tracksData,this,'TrackList').items;for(let i=0;i<a.length;i++)yield a[i];}
    }
    class AudioTrackList extends MediaTrackList {}
    class VideoTrackList extends MediaTrackList {get selectedIndex(){const a=read(tracksData,this,'TrackList').items;for(let i=0;i<a.length;i++)if(a[i].selected)return i;return -1;}}
    function replaceTracks(list,items){const s=read(tracksData,list,'TrackList'),old=s.items;s.items=items;
        for(let i=0;i<s.indexed;i++)delete list[i];for(let i=0;i<items.length;i++)define(list,i,{enumerable:true,configurable:true,get(){return s.items[i];}});s.indexed=items.length;
        for(const t of old)if(!items.includes(t))queueTrack(list,'removetrack',t);
        for(const t of items)if(!old.includes(t))queueTrack(list,'addtrack',t);
    }
    function queueTrack(list,type,track){schedule(()=>{const event=new EventType(type);define(event,'track',{value:track,enumerable:true});event.isTrusted=true;dispatch(list,event);});}
    function refreshTracks(p){const audio=[],video=[];
        for(const sb of p.buffers){const s=buffer(sb),info=native('mseInfo',p.node,s.id);
            for(const kind of ['audio','video']){const list=kind==='audio'?s.audioTracks:s.videoTracks,old=read(tracksData,list,'TrackList').items;
                const items=info[kind]?(old.length?old:[createTrack(kind,s,p)]):[];
                for(const t of items)apply(get,trackSlots,[t]).value=!!info[kind==='audio'?'audioEnabled':'videoSelected'];
                replaceTracks(list,items);for(const t of items)(kind==='audio'?audio:video).push(t);}
            const enabled=[...s.audioTracks,...s.videoTracks].some(t=>apply(get,trackSlots,[t]).value);
            const active=read(lists,p.active,'SourceBufferList').items.includes(sb);
            if(enabled&&!active)add(p.active,sb);else if(!enabled&&active)remove(p.active,sb);}
        const nodeLists=apply(get,nodeTrackLists,[p.node]);if(nodeLists){replaceTracks(nodeLists.audio,audio);replaceTracks(nodeLists.video,video);}
    }
    function createTrack(kind,s,p){const track={},slot={kind,buffer:s,parent:p,value:true};apply(put,trackSlots,[track,slot]);
        for(const [k,v] of [['id',kind+'-'+s.id],['kind','main'],['label',''],['language','']])define(track,k,{value:v,enumerable:true});
        define(track,kind==='audio'?'enabled':'selected',{enumerable:true,get(){return slot.value;},set(v){
            const selected=!!v;if(selected===slot.value)return;
            if(s.removed||!p.node||p.state==='closed')throw error('InvalidStateError','Track is detached');
            if(!native('mseSelect',p.node,kind,selected))throw error('InvalidStateError','Native track selection failed');
            slot.value=selected;refreshTracks(p);
            queue(kind==='audio'?s.audioTracks:s.videoTracks,'change');
            const nodeLists=apply(get,nodeTrackLists,[p.node]);if(nodeLists)queue(nodeLists[kind],'change');
        }});return track;
    }
    function nodeTracks(node,kind){let s=apply(get,nodeTrackLists,[node]);if(!s){s={audio:new AudioTrackList(token),video:new VideoTrackList(token)};apply(put,nodeTrackLists,[node,s]);}const value=apply(get,attachments,[node]);if(value)refreshTracks(source(value));return s[kind];}
    function add(list,item){const s=read(lists,list,'SourceBufferList');s.items.push(item);refreshList(list);queue(list,'addsourcebuffer');}
    function remove(list,item){const s=read(lists,list,'SourceBufferList'),i=s.items.indexOf(item);if(i>=0){s.items.splice(i,1);refreshList(list);queue(list,'removesourcebuffer');}}
    function live(s){const p=source(s.parent);if(s.removed||!p.node||p.state==='closed')throw error('InvalidStateError','SourceBuffer is detached');return p;}
    function mutable(s){const p=live(s);if(s.updating)throw error('InvalidStateError','SourceBuffer is updating');return p;}
    function reopen(p){if(p.state==='ended'){native('mseReopen',p.node);p.state='open';queue(p.object,'sourceopen');}}
    function support(type){return !!native('mseType',null,str(type));}
    function completed(bufferObject,s,epoch,kind){
        if(s.epoch!==epoch||s.removed)return;
        s.updating=false;s.timer=0;
        queue(bufferObject,kind);queue(bufferObject,'updateend');
    }
    function updateDuration(p){
        refreshTracks(p);
        let highest=0,initial=NaN;const a=read(lists,p.buffers,'SourceBufferList').items;
        for(let i=0;i<a.length;i++){const s=buffer(a[i]);if(s.removed)continue;const info=native('mseInfo',p.node,s.id);
            if(info.audio||info.video){if(Number.isNaN(initial))initial=info.initDuration;else initial=Math.max(initial,info.initDuration);}
            const data=native('mseRanges',p.node,s.id);for(let j=0;j<data.length;j++)highest=Math.max(highest,data[j][1]);}
        if(Number.isNaN(p.duration)&&!Number.isNaN(initial)){p.duration=Math.max(initial,highest);native('mseDuration',p.node,p.duration);}
        if(Number.isFinite(p.duration)&&highest>p.duration){p.duration=highest;native('mseDuration',p.node,highest);}
    }
    function operation(object,s,callback){
        const p=mutable(s);reopen(p);s.updating=true;const epoch=++s.epoch,node=p.node,generation=p.generation;
        queue(object,'updatestart');
        s.timer=schedule(()=>{
            if(s.epoch!==epoch||s.removed||source(s.parent).node!==node||source(s.parent).generation!==generation)return;
            try{
                if(!callback(p))throw new TypeError(native('state',p.node).error||'Media segment processing failed');
                const deadline=host.now()+30000;
                const poll=()=>{
                    if(s.epoch!==epoch||s.removed||source(s.parent).node!==node||source(s.parent).generation!==generation)return;
                    try{const state=native('state',p.node);if(state.error)throw state.mseQuotaError?error('QuotaExceededError',state.error):new TypeError(state.error);
                        if(state.msePending){if(host.now()>deadline)throw new Error('MSE worker deadline exceeded');s.timer=host.timer(0,poll,10,[]);return;}
                        if(s.mode==='sequence'){const info=native('mseInfo',p.node,s.id);if(Number.isFinite(info.sequenceOffset))s.offset=info.sequenceOffset;}
                        updateDuration(p);completed(object,s,epoch,'update');
                    }catch(e){fail(e);}
                };
                poll();
            }catch(e){fail(e);}
            function fail(e){if(s.epoch!==epoch||s.removed)return;
                try{native('mseAbort',p.node,s.id);}catch(_){}
                completed(object,s,epoch,'error');p.lastError=e;
                if(e.name!=='QuotaExceededError'&&e.name!=='RangeError'){try{native('mseEnd',p.node,'decode');}catch(_){}p.state='ended';queue(p.object,'sourceended');}
                host.log(2,'MSE: '+String(e));
            }
        });
    }
    class SourceBuffer extends EventTarget {
        constructor(key,parent,id,type){super();if(key!==token)throw new TypeError('Illegal constructor');apply(put,buffers,[this,{parent,id,type,removed:false,updating:false,mode:'segments',offset:0,start:0,end:Infinity,epoch:0,timer:0,audioTracks:new AudioTrackList(token),videoTracks:new VideoTrackList(token),textTracks:new MediaTrackList(token)}]);}
        get updating(){return buffer(this).updating;}
        get buffered(){const s=buffer(this),p=live(s);return timeRanges(native('mseRanges',p.node,s.id));}
        get audioTracks(){const s=buffer(this);refreshTracks(live(s));return s.audioTracks;}
        get videoTracks(){const s=buffer(this);refreshTracks(live(s));return s.videoTracks;}
        get textTracks(){const s=buffer(this);live(s);return s.textTracks;}
        get mode(){return buffer(this).mode;}
        set mode(value){const s=buffer(this),v=str(value),p=mutable(s);if(v!=='segments'&&v!=='sequence')throw new TypeError('Invalid append mode');if(native('mseInfo',p.node,s.id).parsing)throw error('InvalidStateError','Parser is processing a partial segment');reopen(p);s.mode=v;}
        get timestampOffset(){return buffer(this).offset;}
        set timestampOffset(value){const s=buffer(this),n=finite(value),p=mutable(s);if(native('mseInfo',p.node,s.id).parsing)throw error('InvalidStateError','Parser is processing a partial segment');reopen(p);s.offset=n;}
        get appendWindowStart(){return buffer(this).start;}
        set appendWindowStart(value){const s=buffer(this),n=finite(value);mutable(s);if(n<0||n>=s.end)throw new TypeError('Invalid append window start');s.start=n;}
        get appendWindowEnd(){return buffer(this).end;}
        set appendWindowEnd(value){const s=buffer(this),n=+value;mutable(s);if(Number.isNaN(n)||n<=s.start)throw new TypeError('Invalid append window end');s.end=n;}
        appendBuffer(value){
            if(!arguments.length)throw new TypeError('BufferSource required');const s=buffer(this),p=mutable(s),data=bytes(value),n=apply(abLength,data,[]);
            const free=native('mseQuota',p.node,s.id);if(n>free)throw error('QuotaExceededError','Native media buffer quota exceeded');
            if(native('state',p.node).error)throw error('InvalidStateError','Media element has an error');
            operation(this,s,parent=>native('mseAppend',parent.node,s.id,data,s.offset,s.start,s.end,s.mode==='sequence'));
        }
        abort(){const s=buffer(this),p=live(s);if(p.state!=='open')throw error('InvalidStateError','MediaSource is not open');
            if(s.updating){++s.epoch;if(s.timer)host.clear(s.timer);s.timer=0;s.updating=false;queue(this,'abort');queue(this,'updateend');}
            native('mseAbort',p.node,s.id);s.start=0;s.end=Infinity;
        }
        remove(start,end){if(arguments.length<2)throw new TypeError('Range required');const s=buffer(this),p=mutable(s),a=finite(start),b=+end;
            if(a<0||a>p.duration||Number.isNaN(p.duration)||Number.isNaN(b)||b<=a)throw new TypeError('Invalid removal range');
            operation(this,s,parent=>native('mseRemove',parent.node,s.id,a,b));
        }
        changeType(value){if(!arguments.length)throw new TypeError('Type required');const s=buffer(this),p=mutable(s),type=str(value);if(!type)throw new TypeError('Type is empty');if(!support(type))throw error('NotSupportedError','Unsupported MSE type');reopen(p);
            if(!native('mseChangeType',p.node,s.id,type))throw error('NotSupportedError','Media type change failed');s.type=type;
        }
    }
    class MediaSource extends EventTarget {
        constructor(){super();apply(put,sources,[this,{object:this,state:'closed',duration:NaN,node:null,generation:0,buffers:new SourceBufferList(token),active:new SourceBufferList(token),nextId:1,live:null,lastError:null}]);}
        static isTypeSupported(type){if(!arguments.length)throw new TypeError('Type required');return support(type);}
        static get canConstructInDedicatedWorker(){return false;}
        get readyState(){return source(this).state;}
        get sourceBuffers(){return source(this).buffers;}
        get activeSourceBuffers(){return source(this).active;}
        get duration(){return source(this).duration;}
        set duration(value){const p=source(this),n=+value;idle(p);if(Number.isNaN(n)||n<0)throw new TypeError('Invalid media duration');if(!native('mseDuration',p.node,n))throw error('InvalidStateError','Native duration change failed');p.duration=n;}
        addSourceBuffer(value){if(!arguments.length)throw new TypeError('Type required');const p=source(this),type=str(value);if(!type)throw new TypeError('Type is empty');if(!support(type))throw error('NotSupportedError','Unsupported MSE type');if(p.state!=='open')throw error('InvalidStateError','MediaSource is not open');
            if(p.buffers.length>=2)throw error('QuotaExceededError','At most two SourceBuffers');const id=p.nextId++;
            if(!native('mseAdd',p.node,id,type))throw error('QuotaExceededError','Native SourceBuffer could not be created');
            const s=new SourceBuffer(token,this,id,type);add(p.buffers,s);return s;
        }
        removeSourceBuffer(value){const p=source(this),s=apply(get,buffers,[value]);if(!s||s.parent!==this||s.removed)throw error('NotFoundError','SourceBuffer not found');
            if(s.updating){++s.epoch;if(s.timer)host.clear(s.timer);s.timer=0;s.updating=false;queue(value,'abort');queue(value,'updateend');}
            native('mseDrop',p.node,s.id);s.removed=true;remove(p.active,value);remove(p.buffers,value);refreshTracks(p);
        }
        endOfStream(reason=undefined){const p=source(this);idle(p);const type=reason===undefined?'':str(reason);if(type!==''&&type!=='network'&&type!=='decode')throw new TypeError('Invalid end of stream error');
            if(!native('mseEnd',p.node,type))throw error('InvalidStateError','Cannot end native media stream');
            let highest=0;for(const sb of p.buffers){const s=buffer(sb),a=native('mseRanges',p.node,s.id);for(let i=0;i<a.length;i++)highest=Math.max(highest,a[i][1]);}
            if(!type){
                p.duration=highest;
                /* EOS can finish the final open-sized Cluster/fragment in the
                 * child. Never trim/clamp to ranges cached before its ACK. */
                const node=p.node,generation=p.generation,deadline=host.now()+30000;
                const finish=()=>{
                    if(p.node!==node||p.generation!==generation||p.state!=='ended')return;
                    const state=native('state',node);if(state.error)return;
                    if(state.msePending){if(host.now()<deadline)host.timer(0,finish,10,[]);return;}
                    let end=0;for(const sb of p.buffers){const s=buffer(sb),a=native('mseRanges',node,s.id);for(let i=0;i<a.length;i++)end=Math.max(end,a[i][1]);}
                    p.duration=end;native('mseDuration',node,end);
                };
                schedule(finish);
            }p.state='ended';queue(this,'sourceended');
        }
        setLiveSeekableRange(start,end){const p=source(this);if(p.state!=='open')throw error('InvalidStateError','MediaSource is not open');const a=finite(start),b=finite(end);if(a<0||b<a)throw new TypeError('Invalid live seekable range');p.live=[a,b];}
        clearLiveSeekableRange(){const p=source(this);if(p.state!=='open')throw error('InvalidStateError','MediaSource is not open');p.live=null;}
    }
    function idle(p){if(p.state!=='open'||!p.node)throw error('InvalidStateError','MediaSource is not open');for(const sb of p.buffers)if(buffer(sb).updating)throw error('InvalidStateError','SourceBuffer is updating');}
    function attach(value,node,generation){
        const p=source(value);if(p.state!=='closed'||p.node)throw error('NotSupportedError','MediaSource is already attached');
        if(!native('mseCreate',node,generation))throw error('NotSupportedError','Cannot create native media source');
        p.node=node;p.generation=generation;p.duration=NaN;apply(put,attachments,[node,value]);
        const epoch=generation;schedule(()=>{if(p.node===node&&p.generation===epoch){p.state='open';fire(value,'sourceopen');}});
    }
    function detach(node){
        const value=apply(get,attachments,[node]);if(!value)return;const p=source(value);apply(del,attachments,[node]);
        for(const sb of p.buffers){const s=buffer(sb);++s.epoch;if(s.timer)host.clear(s.timer);s.timer=0;s.updating=false;s.removed=true;}
        const all=read(lists,p.buffers,'SourceBufferList'),active=read(lists,p.active,'SourceBufferList');all.items=[];active.items=[];refreshList(p.buffers);refreshList(p.active);
        p.node=null;p.state='closed';p.duration=NaN;p.live=null;p.generation++;queue(value,'sourceclose');
        const nodeLists=apply(get,nodeTrackLists,[node]);if(nodeLists){replaceTracks(nodeLists.audio,[]);replaceTracks(nodeLists.video,[]);}
    }
    function nodeRanges(node,seekable){
        const value=apply(get,attachments,[node]);if(!value)return timeRanges();const p=source(value);refreshTracks(p);let result=null,highest=0;
        if(seekable&&Number.isNaN(p.duration))return timeRanges();
        if(seekable&&Number.isFinite(p.duration))return timeRanges(p.duration>0?[[0,p.duration]]:[]);
        for(const sb of p.active){const s=buffer(sb),a=native('mseRanges',node,s.id);
            if(result===null)result=a;else {const intersection=[];for(const x of result)for(const y of a){const start=Math.max(x[0],y[0]),end=Math.min(x[1],y[1]);if(start<end)intersection.push([start,end]);}result=intersection;}}
        if(result?.length)highest=result[result.length-1][1];
        if(seekable&&p.duration===Infinity){if(p.live)return timeRanges(p.live[1]>p.live[0]||highest>0?[[Math.min(p.live[0],result?.[0]?.[0]??p.live[0]),Math.max(p.live[1],highest)]]:[]);return timeRanges(highest>0?[[0,highest]]:[]);}
        return timeRanges(result||[]);
    }
    const sourceEvents=['sourceopen','sourceended','sourceclose'],bufferEvents=['updatestart','update','updateend','error','abort'],listEvents=['addsourcebuffer','removesourcebuffer'];
    mseHandlerTarget=(v,type)=>apply(get,sources,[v])!==undefined&&sourceEvents.includes(type)||apply(get,buffers,[v])!==undefined&&bufferEvents.includes(type)||apply(get,lists,[v])!==undefined&&listEvents.includes(type)||apply(get,tracksData,[v])!==undefined&&['change','addtrack','removetrack'].includes(type);
    installHandlers(MediaSource.prototype,sourceEvents);installHandlers(SourceBuffer.prototype,bufferEvents);installHandlers(SourceBufferList.prototype,listEvents);
    installHandlers(MediaTrackList.prototype,['change','addtrack','removetrack']);
    for(const C of [MediaSource,SourceBuffer,SourceBufferList,TimeRanges]){define(C.prototype,Symbol.toStringTag,{value:C.name,configurable:true});for(const k of Object.getOwnPropertyNames(C.prototype))if(k!=='constructor'){const d=Object.getOwnPropertyDescriptor(C.prototype,k);d.enumerable=true;define(C.prototype,k,d);}}
    Object.assign(globalThis,{MediaSource,SourceBuffer,SourceBufferList,TimeRanges,AudioTrackList,VideoTrackList});
    objectURLBridge.registerMediaSource(v=>apply(get,sources,[v])!==undefined);
    return {attach,detach,timeRanges,nodeRanges,nodeTracks,brand:v=>apply(get,sources,[v])!==undefined,attached:node=>apply(get,attachments,[node]),duration(node){const v=apply(get,attachments,[node]);return v?source(v).duration:NaN;}};
})();
