/* Web Animations 1 (with the members level 2 gives a document timeline), CSS Animations 2 and
   CSS Transitions 2's interfaces: a Web IDL facade over animation.c through host.animation
   (js_animation_native.h). The time model, the effect stack, the CSS-started animations and the
   event queue are native, so getAnimations() returns the CSSAnimation and CSSTransition objects
   the cascade started; this file keeps object identity, argument conversion, the promises and
   dispatch. HTML "update animations and send events" reaches it through update() and events(). */
const animationBridge=(()=>{
    const native=host.animation,define=Object.defineProperty;
    const wmGet=WeakMap.prototype.get,wmSet=WeakMap.prototype.set,wmHas=WeakMap.prototype.has;
    const mGet=Map.prototype.get,mSet=Map.prototype.set,mDelete=Map.prototype.delete,thenFn=Promise.prototype.then;
    const PromiseImpl=Promise,WeakRefImpl=WeakRef,iteratorSymbol=Symbol.iterator,construct=Reflect.construct;
    /* the op table of js_animation_native.h (WAO_*) */
    const OP={TIMELINE:0,EFFECT_NEW:1,EFFECT_TIMING:2,EFFECT_TIMING_GET:3,EFFECT_COMPUTED:4,KEYFRAMES_SET:5,KEYFRAMES_GET:6,
        TARGET_SET:7,TARGET_GET:8,COMPOSITE_SET:9,COMPOSITE_GET:10,EFFECT_WRAP:11,EFFECT_RELEASE:12,NEW:13,STATE:14,PLAY:15,
        PAUSE:16,FINISH:17,CANCEL:18,REVERSE:19,PERSIST:20,UPDATE_RATE:21,SET_RATE:22,SET_CURRENT:23,SET_START:24,SET_EFFECT:25,
        SET_TIMELINE:26,NOTIFY:27,RELEASE:28,WRAP:29,LIST:30,EVENTS:31,CSS:32,COMMIT:33,EASING:34,FLUSH:35,SYNC:36,PROPERTY:37};
    /* STATE: play state, pending, current, start, rate, replace, ready gen/state/previous,
       finished gen/state/previous, finish task, effect, kind, timeline, alive */
    const S_PLAY=0,S_PENDING=1,S_CURRENT=2,S_START=3,S_RATE=4,S_REPLACE=5,S_READY=6,S_FINISHED=9,S_TASK=12,S_EFFECT=13,S_KIND=14,S_TIMELINE=15,S_ALIVE=16;
    const PENDING=0,RESOLVED=1,REJECTED=2;
    const playStates=['idle','running','paused','finished'],replaceStates=['active','removed','persisted'];
    const fills=['none','forwards','backwards','both','auto'],directions=['normal','reverse','alternate','alternate-reverse'];
    const composites=['replace','add','accumulate','auto'];
    const animations=new WeakMap(),effects=new WeakMap(),timelines=new WeakMap(),defaultTimelines=new WeakMap();
    const playbackEvents=new WeakMap(),cssEvents=new WeakMap();
    /* id -> WeakRef of the object; held: the objects whose animation can still run or fire */
    const animationRefs=new Map(),effectRefs=new Map(),held=new Map();
    const registry=new FinalizationRegistry(token=>{
        const refs=token.animation?animationRefs:effectRefs,ref=apply(mGet,refs,[token.id]);
        if(ref&&ref.deref()===undefined)apply(mDelete,refs,[token.id]);
        try{native(token.animation?OP.RELEASE:OP.EFFECT_RELEASE,token.id);}catch(e){}
    });
    const noop=()=>{};
    /* ---------------------------------------------------------------- Web IDL conversions */
    function restricted(v,name){v=+v;if(!Number.isFinite(v))throw new TypeError(`The ${name} must be a finite number`);return v;}
    function text(v){return `${v}`;}
    function enumIndex(v,values,name){v=text(v);for(let i=0;i<values.length;i++)if(values[i]===v)return i;throw new TypeError(`'${v}' is not a valid ${name}`);}
    function isObject(v){return v!==null&&(typeof v==='object'||typeof v==='function');}
    function dictionary(v,name){if(v!==undefined&&v!==null&&!isObject(v))throw new TypeError(`The ${name} must be a dictionary`);return v==null?{}:v;}
    function iteratorOf(v){if(!isObject(v))return undefined;const m=v[iteratorSymbol];if(m===undefined||m===null)return undefined;if(typeof m!=='function')throw new TypeError('The @@iterator method is not callable');return m;}
    function sequence(v,method,convert){const out=[],it=apply(method,v,[]);if(!isObject(it))throw new TypeError('The iterator is not an object');
        for(;;){const step=it.next();if(!isObject(step))throw new TypeError('The iterator result is not an object');if(step.done)return out;out.push(convert(step.value));}}
    function isElement(v){return isObject(v)&&rawDom.isNode(null,v)&&rawDom.get(v,'nodeType')===1;}
    function elementThis(v){if(!isElement(v))throw new TypeError('Illegal invocation');return v;}
    function animationState(a){const s=isObject(a)?apply(wmGet,animations,[a]):undefined;if(!s)throw new TypeError('Illegal Animation receiver');return s;}
    function effectState(e){const s=isObject(e)?apply(wmGet,effects,[e]):undefined;if(!s)throw new TypeError('Illegal AnimationEffect receiver');return s;}
    function timelineState(t){const s=isObject(t)?apply(wmGet,timelines,[t]):undefined;if(!s)throw new TypeError('Illegal AnimationTimeline receiver');return s;}
    /* <pseudo-element-selector>, the legacy single-colon four made two-colon; the code is the native target */
    const legacyPseudo={':before':'::before',':after':'::after',':first-letter':'::first-letter',':first-line':'::first-line'};
    const knownPseudo=/^::(?:before|after|marker|first-line|first-letter|placeholder|selection|backdrop|file-selector-button|target-text|spelling-error|grammar-error|details-content|cue|view-transition|(?:highlight|part|cue|slotted|view-transition-(?:group|image-pair|old|new))\([^()]*\))$/i;
    function pseudoSelector(v){
        if(v===null||v===undefined)return null;
        v=text(v);const legacy=legacyPseudo[v.toLowerCase()];
        if(legacy)v=legacy;else if(!knownPseudo.test(v))throw new DOMException(`'${v}' is not a valid pseudo-element selector`,'SyntaxError');
        const lower=v.toLowerCase();
        return {text:v,code:lower==='::before'?1:lower==='::after'?2:3};
    }
    function pseudoCode(t){if(t===null)return 0;const lower=t.toLowerCase();return lower==='::before'?1:lower==='::after'?2:3;}
    const pseudoNames=[null,'::before','::after',null];
    /* ---------------------------------------------------------------- timing (§6.5 "updating the timing properties") */
    const T_DELAY=1,T_END_DELAY=2,T_FILL=4,T_ITERATION_START=8,T_ITERATIONS=16,T_DURATION=32,T_DIRECTION=64,T_EASING=128;
    function readTiming(o,partial){
        // EffectTiming's members in Web IDL order (lexicographic)
        const t={mask:0,delay:0,endDelay:0,fill:4,iterationStart:0,iterations:1,duration:null,direction:0,easing:'linear'};
        let v=o.delay;if(v!==undefined){t.delay=restricted(v,'delay');t.mask|=T_DELAY;}
        v=o.direction;if(v!==undefined){t.direction=enumIndex(v,directions,'playback direction');t.mask|=T_DIRECTION;}
        v=o.duration;if(v!==undefined){if(typeof v==='number')t.duration=v;else{v=text(v);if(v!=='auto')throw new TypeError(`'${v}' is not a valid duration`);t.duration=null;}t.mask|=T_DURATION;}
        v=o.easing;if(v!==undefined){t.easing=text(v);t.mask|=T_EASING;}
        v=o.endDelay;if(v!==undefined){t.endDelay=restricted(v,'endDelay');t.mask|=T_END_DELAY;}
        v=o.fill;if(v!==undefined){t.fill=enumIndex(v,fills,'fill mode');t.mask|=T_FILL;}
        v=o.iterationStart;if(v!==undefined){t.iterationStart=restricted(v,'iterationStart');t.mask|=T_ITERATION_START;}
        v=o.iterations;if(v!==undefined){t.iterations=+v;t.mask|=T_ITERATIONS;}
        if(!partial)t.mask=255;
        return t;
    }
    function checkTiming(t){
        if((t.mask&T_ITERATION_START)&&t.iterationStart<0)throw new TypeError('The iterationStart must not be negative');
        if((t.mask&T_ITERATIONS)&&!(t.iterations>=0))throw new TypeError('The iterations must be a non-negative number');
        if((t.mask&T_DURATION)&&t.duration!==null&&!(t.duration>=0))throw new TypeError('The duration must be a non-negative number');
        if((t.mask&T_EASING)&&native(OP.EASING,t.easing)===null)throw new TypeError(`'${t.easing}' is not a valid easing function`);
    }
    function applyTiming(id,t){native(OP.EFFECT_TIMING,id,t.mask,t.delay,t.endDelay,t.fill,t.iterationStart,t.iterations,t.duration,t.direction,t.easing);}
    /* KeyframeEffectOptions (and KeyframeAnimationOptions for animate()), or a duration */
    function effectOptions(options,animation){
        const out={timing:null,composite:0,iterationComposite:0,pseudo:null,id:'',timeline:undefined};
        if(options!==undefined&&options!==null&&!isObject(options)){
            out.timing=readTiming({},false);out.timing.duration=+options;return out;
        }
        const o=options==null?{}:options;
        out.timing=readTiming(o,false);
        let v=o.composite;if(v!==undefined)out.composite=enumIndex(v,['replace','add','accumulate'],'composite operation');
        v=o.iterationComposite;if(v!==undefined)out.iterationComposite=enumIndex(v,['replace','accumulate'],'iteration composite operation')*2;
        v=o.pseudoElement;if(v!==undefined&&v!==null)out.pseudo=text(v);
        if(animation){
            v=o.id;if(v!==undefined)out.id=text(v);
            v=o.timeline;if(v!==undefined){if(v!==null)timelineState(v);out.timeline=v;}
        }
        return out;
    }
    /* ---------------------------------------------------------------- keyframes ("processing a keyframes argument") */
    const propertyNames=new Map();
    /* an animatable property's CSS name for an IDL attribute name ("IDL attribute name to animation property name"), else null */
    function animationProperty(key){
        if(typeof key!=='string'||key==='composite'||key==='easing'||key==='offset'||key==='float')return null;
        const css=key.startsWith('--')?key:key==='cssFloat'?'float':key==='cssOffset'?'offset':/^[A-Za-z]+$/.test(key)?key.replace(/[A-Z]/g,c=>'-'+c.toLowerCase()):null;
        if(css===null)return null;
        let name=apply(mGet,propertyNames,[css]);
        if(name===undefined){name=native(OP.PROPERTY,css);apply(mSet,propertyNames,[css,name]);}
        return name;
    }
    function idlName(css){return css.startsWith('--')?css:css==='float'?'cssFloat':css==='offset'?'cssOffset':css.replace(/-([a-z])/g,(m,c)=>c.toUpperCase());}
    function keyframeLike(input,lists){
        if(input!==undefined&&input!==null&&!isObject(input))throw new TypeError('A keyframe must be an object');
        const o=input==null?{}:input,frame={composite:3,easing:'linear',offset:null,props:[]};
        // BaseKeyframe / BasePropertyIndexedKeyframe, members in Web IDL order
        let v=o.composite,m;
        if(lists){frame.composite=v===undefined?[]:(m=iteratorOf(v))!==undefined?sequence(v,m,x=>enumIndex(x,composites,'composite operation')):[enumIndex(v,composites,'composite operation')];}
        else if(v!==undefined)frame.composite=enumIndex(v,composites,'composite operation');
        v=o.easing;
        if(lists)frame.easing=v===undefined?[]:(m=iteratorOf(v))!==undefined?sequence(v,m,text):[text(v)];
        else if(v!==undefined)frame.easing=text(v);
        v=o.offset;
        const offset=x=>x===undefined||x===null?null:restricted(x,'offset');
        if(lists)frame.offset=v===undefined?[]:v===null?[null]:(m=iteratorOf(v))!==undefined?sequence(v,m,offset):[offset(v)];
        else frame.offset=offset(v);
        const found=[];
        for(const key of Object.keys(o)){const css=animationProperty(key);if(css!==null)found.push([key,css]);}
        found.sort((a,b)=>a[0]<b[0]?-1:a[0]>b[0]?1:0);
        for(const [key,css] of found){
            v=o[key];
            if(lists)frame.props.push([css,(m=iteratorOf(v))!==undefined?sequence(v,m,text):[text(v)]]);
            else frame.props.push([css,text(v)]);
        }
        return frame;
    }
    function checkOffsets(frames){
        let last=-Infinity;
        for(const f of frames){
            if(f.offset===null)continue;
            if(f.offset<last)throw new TypeError('Keyframe offsets must be loosely sorted');
            if(f.offset<0||f.offset>1)throw new TypeError('Keyframe offsets must lie in [0, 1]');
            last=f.offset;
        }
    }
    function checkEasing(e){if(native(OP.EASING,e)===null)throw new TypeError(`'${e}' is not a valid easing function`);}
    /* a native row: offset, easing, composite, then property and value pairs */
    function row(f){const out=[f.offset,f.easing,f.composite];for(const [css,value] of f.props)out.push(css,value);return out;}
    function keyframesArgument(v){if(v!==undefined&&v!==null&&!isObject(v))throw new TypeError('The keyframes must be an object or null');}
    function processKeyframes(object){
        if(object===undefined||object===null)return [];
        const method=iteratorOf(object);
        if(method!==undefined){
            const frames=sequence(object,method,item=>{
                if(item!==undefined&&item!==null&&!isObject(item))throw new TypeError('A keyframe must be an object, null or undefined');
                return keyframeLike(item,false);
            });
            checkOffsets(frames);
            for(const f of frames)checkEasing(f.easing);
            return frames.map(row);
        }
        const input=keyframeLike(object,true),list=[];
        for(const [css,values] of input.props){
            const n=values.length;
            for(let i=0;i<n;i++)list.push({computed:n===1?1:i/(n-1),order:list.length,props:[[css,values[i]]]});
        }
        list.sort((a,b)=>a.computed-b.computed||a.order-b.order);
        const frames=[];
        for(const k of list){
            const last=frames[frames.length-1];
            if(last&&last.computed===k.computed)last.props.push(k.props[0]);
            else frames.push({computed:k.computed,offset:null,easing:'linear',composite:3,props:[k.props[0]]});
        }
        for(let i=0;i<input.offset.length&&i<frames.length;i++)frames[i].offset=input.offset[i];
        const easings=input.easing.length?input.easing:['linear'];
        for(let i=0;i<frames.length;i++)frames[i].easing=easings[i%easings.length];
        if(input.composite.length)for(let i=0;i<frames.length;i++)frames[i].composite=input.composite[i%input.composite.length];
        checkOffsets(frames);
        for(const e of easings)checkEasing(e);
        return frames.map(row);
    }
    /* ---------------------------------------------------------------- object identity */
    function effectFor(id){
        const ref=apply(mGet,effectRefs,[id]),old=ref&&ref.deref();
        if(old)return old;
        const e=Object.create(KeyframeEffect.prototype);
        native(OP.EFFECT_WRAP,id);
        apply(wmSet,effects,[e,{id,pseudo:pseudoNames[native(OP.TARGET_GET,id)[1]]}]);
        apply(mSet,effectRefs,[id,new WeakRefImpl(e)]);registry.register(e,{animation:false,id});
        return e;
    }
    function makeEffect(proto,target,pseudo,timing,composite,iterationComposite,rows){
        const id=native(OP.EFFECT_NEW,target,pseudo?pseudo.code:0),e=Object.create(proto);
        apply(wmSet,effects,[e,{id,pseudo:pseudo?pseudo.text:null}]);
        apply(mSet,effectRefs,[id,new WeakRefImpl(e)]);registry.register(e,{animation:false,id});
        applyTiming(id,timing);
        native(OP.COMPOSITE_SET,id,composite,iterationComposite);
        native(OP.KEYFRAMES_SET,id,rows);
        return e;
    }
    function animationFor(id){
        const ref=apply(mGet,animationRefs,[id]),old=ref&&ref.deref();
        if(old)return old;
        const st=native(OP.STATE,id,false),kind=st[S_KIND];
        const a=construct(EventTarget,[],kind===1?CSSAnimation:kind===2?CSSTransition:Animation);
        native(OP.WRAP,id);
        attach(a,id,st[S_TIMELINE]?timelineFor(document):null,null);
        return a;
    }
    function attach(a,id,timeline,effect){
        const st=native(OP.STATE,id,false);
        const s={id,label:'',kind:st[S_KIND],timeline,effect,readyGen:st[S_READY],ready:promiseRecord(),
            finishedGen:st[S_FINISHED],finished:promiseRecord(),notifying:false};
        apply(wmSet,animations,[a,s]);
        apply(mSet,animationRefs,[id,new WeakRefImpl(a)]);registry.register(a,{animation:true,id});
        settle(s.ready,st[S_READY+1],a);settle(s.finished,st[S_FINISHED+1],a);
        if(st[S_TASK])notifyLater(a,s);
        hold(a,s,st[S_ALIVE]);
        return s;
    }
    function hold(a,s,alive){if(alive)apply(mSet,held,[s.id,a]);else apply(mDelete,held,[s.id]);}
    /* ---------------------------------------------------------------- promises (§4.4.6–4.4.7) */
    function promiseRecord(){let resolve,reject;const promise=new PromiseImpl((yes,no)=>{resolve=yes;reject=no;});return {promise,resolve,reject,state:PENDING};}
    function settle(record,state,a){
        if(record.state!==PENDING||state===PENDING)return;
        record.state=state;
        if(state===RESOLVED){record.resolve(a);return;}
        // a rejected ready or finished promise has [[PromiseIsHandled]] set
        apply(thenFn,record.promise,[undefined,noop]);
        record.reject(new DOMException('The animation was aborted','AbortError'));
    }
    function promises(a,s,st,at){
        // at: the ready generation's index in st (then state, previous; finished likewise; the finish task)
        const readyGen=st[at],finishedGen=st[at+3];
        if(readyGen!==s.readyGen){settle(s.ready,st[at+2],a);s.ready=promiseRecord();s.readyGen=readyGen;}
        settle(s.ready,st[at+1],a);
        if(finishedGen!==s.finishedGen){settle(s.finished,st[at+5],a);s.finished=promiseRecord();s.finishedGen=finishedGen;}
        settle(s.finished,st[at+4],a);
        if(st[at+6])notifyLater(a,s);
    }
    /* §4.4.16: the finish notification steps run in a microtask */
    function notifyLater(a,s){
        if(s.notifying)return;
        s.notifying=true;
        host.microtask(()=>{s.notifying=false;try{native(OP.NOTIFY,s.id);}catch(e){return;}sync(a,s);});
    }
    function sync(a,s){const st=native(OP.STATE,s.id,false);promises(a,s,st,S_READY);hold(a,s,st[S_ALIVE]);return st;}
    /* ---------------------------------------------------------------- timelines */
    class AnimationTimeline {
        constructor(){throw new TypeError('Illegal constructor');}
        get currentTime(){const t=timelineState(this);if(t.inactive)return null;const now=native(OP.TIMELINE);return now===null?null:now-t.origin;}
        get duration(){timelineState(this);return null;}
    }
    class DocumentTimeline extends AnimationTimeline {
        constructor(){
            const o=dictionary(arguments[0],'DocumentTimelineOptions'),v=o.originTime;
            const timeline=Object.create(new.target.prototype);
            apply(wmSet,timelines,[timeline,{origin:v===undefined?0:restricted(v,'originTime'),inactive:false}]);
            return timeline;
        }
    }
    /* a document's default timeline; one without a browsing context of this realm is inactive */
    function timelineFor(doc){
        let t=apply(wmGet,defaultTimelines,[doc]);
        if(!t){t=Object.create(DocumentTimeline.prototype);apply(wmSet,timelines,[t,{origin:0,inactive:doc!==document}]);apply(wmSet,defaultTimelines,[doc,t]);}
        return t;
    }
    function timelineArgs(t){const s=t===null?null:timelineState(t);return [!!s&&!s.inactive,s?s.origin:0];}
    /* ---------------------------------------------------------------- effects */
    class AnimationEffect {
        constructor(){throw new TypeError('Illegal constructor');}
        getTiming(){
            const t=native(OP.EFFECT_TIMING_GET,effectState(this).id);
            return {delay:t[0],direction:directions[t[6]],duration:t[5],easing:t[7],endDelay:t[1],fill:fills[t[2]],iterationStart:t[3],iterations:t[4]};
        }
        getComputedTiming(){
            const id=effectState(this).id,t=native(OP.EFFECT_TIMING_GET,id),c=native(OP.EFFECT_COMPUTED,id);
            return {delay:t[0],direction:directions[t[6]],duration:t[5]==='auto'?0:t[5],easing:t[7],endDelay:t[1],
                fill:t[2]===4?'none':fills[t[2]],iterationStart:t[3],iterations:t[4],
                activeDuration:c[1],currentIteration:c[4],endTime:c[0],localTime:c[2],progress:c[3]};
        }
        updateTiming(){
            const s=effectState(this),t=readTiming(dictionary(arguments[0],'OptionalEffectTiming'),true);
            checkTiming(t);applyTiming(s.id,t);
        }
    }
    class KeyframeEffect extends AnimationEffect {
        constructor(target){
            if(!arguments.length)throw new TypeError('KeyframeEffect requires a target or a source effect');
            if(arguments.length===1){
                // the copy constructor
                const src=effectState(target),[node,code]=native(OP.TARGET_GET,src.id),t=native(OP.EFFECT_TIMING_GET,src.id);
                const [composite,iterationComposite]=native(OP.COMPOSITE_GET,src.id),rows=[];
                for(const f of native(OP.KEYFRAMES_GET,src.id)){const r=[f[0],f[2],f[3]];for(let i=4;i<f.length;i++)r.push(f[i]);rows.push(r);}
                const timing={mask:255,delay:t[0],endDelay:t[1],fill:t[2],iterationStart:t[3],iterations:t[4],duration:t[5]==='auto'?null:t[5],direction:t[6],easing:t[7]};
                return makeEffect(new.target.prototype,node,src.pseudo===null?null:{text:src.pseudo,code},timing,composite,iterationComposite,rows);
            }
            const keyframes=arguments[1];
            if(target===undefined)target=null;
            if(target!==null&&!isElement(target))throw new TypeError('The target must be an Element or null');
            keyframesArgument(keyframes);
            const o=effectOptions(arguments[2],false),pseudo=pseudoSelector(o.pseudo);
            checkTiming(o.timing);
            const rows=processKeyframes(keyframes);
            return makeEffect(new.target.prototype,target,pseudo,o.timing,o.composite,o.iterationComposite,rows);
        }
        get target(){return native(OP.TARGET_GET,effectState(this).id)[0];}
        set target(v){
            const s=effectState(this);if(v===undefined)v=null;
            if(v!==null&&!isElement(v))throw new TypeError('The target must be an Element or null');
            native(OP.TARGET_SET,s.id,v,pseudoCode(s.pseudo));
        }
        get pseudoElement(){return effectState(this).pseudo;}
        set pseudoElement(v){
            const s=effectState(this),p=pseudoSelector(v);
            s.pseudo=p?p.text:null;
            native(OP.TARGET_SET,s.id,native(OP.TARGET_GET,s.id)[0],p?p.code:0);
        }
        get composite(){return composites[native(OP.COMPOSITE_GET,effectState(this).id)[0]];}
        set composite(v){
            const s=effectState(this),i=['replace','add','accumulate'].indexOf(text(v));
            if(i>=0)native(OP.COMPOSITE_SET,s.id,i,-1); // an attribute ignores a value outside its enumeration
        }
        get iterationComposite(){return composites[native(OP.COMPOSITE_GET,effectState(this).id)[1]];}
        set iterationComposite(v){
            const s=effectState(this),i=['replace','accumulate'].indexOf(text(v));
            if(i>=0)native(OP.COMPOSITE_SET,s.id,-1,i*2);
        }
        getKeyframes(){
            const out=[];
            for(const f of native(OP.KEYFRAMES_GET,effectState(this).id)){
                const k={offset:f[0],computedOffset:f[1],easing:f[2],composite:composites[f[3]]};
                for(let i=4;i+1<f.length;i+=2)k[idlName(f[i])]=f[i+1];
                out.push(k);
            }
            return out;
        }
        setKeyframes(keyframes){
            const s=effectState(this);
            if(!arguments.length)throw new TypeError('setKeyframes requires keyframes');
            keyframesArgument(keyframes);
            native(OP.KEYFRAMES_SET,s.id,processKeyframes(keyframes));
        }
    }
    /* ---------------------------------------------------------------- animations */
    class Animation extends EventTarget {
        constructor(effect=null,timeline){
            super();
            if(effect!==null&&effect!==undefined)effectState(effect);else effect=null;
            const line=arguments.length<2||timeline===undefined?timelineFor(document):timeline,[active,origin]=timelineArgs(line);
            const id=native(OP.NEW,effect?effectState(effect).id:0,active,origin);
            attach(this,id,line,effect);
        }
        get id(){return animationState(this).label;}
        set id(v){animationState(this).label=text(v);}
        get effect(){
            const s=animationState(this),id=native(OP.STATE,s.id,false)[S_EFFECT];
            if(!id){s.effect=null;return null;}
            if(!s.effect||effectState(s.effect).id!==id)s.effect=effectFor(id);
            return s.effect;
        }
        set effect(v){
            const s=animationState(this);
            if(v===undefined)v=null;
            if(v!==null)effectState(v);
            native(OP.SET_EFFECT,s.id,v?effectState(v).id:0);s.effect=v;sync(this,s);
        }
        get timeline(){return animationState(this).timeline;}
        set timeline(v){
            const s=animationState(this);if(v===undefined)v=null;
            const [active,origin]=timelineArgs(v);
            native(OP.SET_TIMELINE,s.id,active,origin);s.timeline=v;sync(this,s);
        }
        get startTime(){return native(OP.STATE,animationState(this).id,false)[S_START];}
        set startTime(v){
            const s=animationState(this);
            native(OP.SET_START,s.id,v===undefined||v===null?null:restricted(v,'startTime'));sync(this,s);
        }
        get currentTime(){return native(OP.STATE,animationState(this).id,false)[S_CURRENT];}
        set currentTime(v){
            const s=animationState(this),status=native(OP.SET_CURRENT,s.id,v===undefined||v===null?null:restricted(v,'currentTime'));
            sync(this,s);
            if(status<0)throw new TypeError('The current time of a playing animation cannot be cleared');
        }
        get playbackRate(){return native(OP.STATE,animationState(this).id,false)[S_RATE];}
        set playbackRate(v){const s=animationState(this);native(OP.SET_RATE,s.id,restricted(v,'playbackRate'));sync(this,s);}
        // CSS Animations 2 §4.1, CSS Transitions 2 §3.1: play state and pending flush style first
        get playState(){const s=animationState(this);return playStates[native(OP.STATE,s.id,s.kind!==0)[S_PLAY]];}
        get replaceState(){return replaceStates[native(OP.STATE,animationState(this).id,false)[S_REPLACE]];}
        get pending(){const s=animationState(this);return native(OP.STATE,s.id,s.kind!==0)[S_PENDING];}
        get ready(){const s=animationState(this);sync(this,s);return s.ready.promise;}
        get finished(){const s=animationState(this);sync(this,s);return s.finished.promise;}
        /* Web Animations 2: the effect's progress, the current time over the effect end */
        get overallProgress(){
            const s=animationState(this),st=native(OP.STATE,s.id,false),current=st[S_CURRENT];
            if(!st[S_EFFECT]||current===null)return null;
            const end=native(OP.EFFECT_COMPUTED,st[S_EFFECT])[0];
            if(end===0)return current<0?0:1;
            if(!Number.isFinite(end))return 0;
            return Math.min(Math.max(current/end,0),1);
        }
        cancel(){const s=animationState(this);native(OP.CANCEL,s.id);sync(this,s);}
        finish(){
            const s=animationState(this),status=native(OP.FINISH,s.id);sync(this,s);
            if(status<0)throw new DOMException('An animation with a zero playback rate or an infinite end cannot finish','InvalidStateError');
        }
        play(){
            const s=animationState(this),status=native(OP.PLAY,s.id);sync(this,s);
            if(status<0)throw new DOMException('A reversed animation with an infinite end cannot play','InvalidStateError');
        }
        pause(){
            const s=animationState(this),status=native(OP.PAUSE,s.id);sync(this,s);
            if(status<0)throw new DOMException('An animation with an infinite end cannot pause before it starts','InvalidStateError');
        }
        updatePlaybackRate(rate){
            const s=animationState(this);
            if(!arguments.length)throw new TypeError('updatePlaybackRate requires a rate');
            native(OP.UPDATE_RATE,s.id,restricted(rate,'playback rate'));sync(this,s);
        }
        reverse(){
            const s=animationState(this),status=native(OP.REVERSE,s.id);sync(this,s);
            if(status<0)throw new DOMException('An animation with an infinite end cannot reverse','InvalidStateError');
        }
        persist(){const s=animationState(this);native(OP.PERSIST,s.id);sync(this,s);}
        /* commitStyles(): each longhand the effect targets, at its current value, into the style attribute */
        commitStyles(){
            const s=animationState(this),effect=this.effect;
            if(effect===null)return;
            const e=effectState(effect),target=native(OP.TARGET_GET,e.id)[0];
            if(target===null)return;
            const style=e.pseudo===null?target.style:undefined;
            if(!isObject(style))throw new DOMException('The animation target has no style attribute','NoModificationAllowedError');
            const pairs=native(OP.COMMIT,s.id);
            if(pairs===null)throw new DOMException('The animation target is not being rendered','InvalidStateError');
            for(let i=0;i+1<pairs.length;i+=2)style.setProperty(pairs[i],pairs[i+1]);
        }
    }
    class CSSAnimation extends Animation {
        constructor(){throw new TypeError('Illegal constructor');}
        get animationName(){return native(OP.CSS,animationState(this).id)[1]??'';}
    }
    class CSSTransition extends Animation {
        constructor(){throw new TypeError('Illegal constructor');}
        get transitionProperty(){return native(OP.CSS,animationState(this).id)[1]??'';}
    }
    /* ---------------------------------------------------------------- events */
    function eventData(map,e,name){const s=isObject(e)?apply(wmGet,map,[e]):undefined;if(!s)throw new TypeError(`Illegal ${name} receiver`);return s;}
    class AnimationPlaybackEvent extends Event {
        constructor(type){
            if(!arguments.length)throw new TypeError('AnimationPlaybackEvent requires a type');
            const init=dictionary(arguments[1],'AnimationPlaybackEventInit');
            super(type,init);
            let v=init.currentTime;const currentTime=v===undefined||v===null?null:restricted(v,'currentTime');
            v=init.timelineTime;const timelineTime=v===undefined||v===null?null:restricted(v,'timelineTime');
            apply(wmSet,playbackEvents,[this,{currentTime,timelineTime}]);
        }
        get currentTime(){return eventData(playbackEvents,this,'AnimationPlaybackEvent').currentTime;}
        get timelineTime(){return eventData(playbackEvents,this,'AnimationPlaybackEvent').timelineTime;}
    }
    /* CSS Animations 1 §4.1 AnimationEvent, CSS Transitions 1 §6.1 TransitionEvent */
    class AnimationEvent extends Event {
        constructor(type){
            if(!arguments.length)throw new TypeError('AnimationEvent requires a type');
            const init=dictionary(arguments[1],'AnimationEventInit');
            super(type,init);
            let v=init.animationName;const name=v===undefined?'':text(v);
            v=init.elapsedTime;const elapsed=v===undefined?0:restricted(v,'elapsedTime');
            v=init.pseudoElement;const pseudo=v===undefined?'':text(v);
            apply(wmSet,cssEvents,[this,{name,elapsed,pseudo}]);
        }
        get animationName(){return eventData(cssEvents,this,'AnimationEvent').name;}
        get elapsedTime(){return eventData(cssEvents,this,'AnimationEvent').elapsed;}
        get pseudoElement(){return eventData(cssEvents,this,'AnimationEvent').pseudo;}
    }
    class TransitionEvent extends Event {
        constructor(type){
            if(!arguments.length)throw new TypeError('TransitionEvent requires a type');
            const init=dictionary(arguments[1],'TransitionEventInit');
            super(type,init);
            let v=init.elapsedTime;const elapsed=v===undefined?0:restricted(v,'elapsedTime');
            v=init.propertyName;const name=v===undefined?'':text(v);
            v=init.pseudoElement;const pseudo=v===undefined?'':text(v);
            apply(wmSet,cssEvents,[this,{name,elapsed,pseudo}]);
        }
        get propertyName(){return eventData(cssEvents,this,'TransitionEvent').name;}
        get elapsedTime(){return eventData(cssEvents,this,'TransitionEvent').elapsed;}
        get pseudoElement(){return eventData(cssEvents,this,'TransitionEvent').pseudo;}
    }
    function trusted(e){eventState(e).isTrusted=true;return e;}
    /* HTML "update animations and send events" step 2: the promises the timelines' update settled
       (their reactions run in the microtask checkpoint that follows) */
    function update(){
        for(const r of native(OP.SYNC)){
            const ref=apply(mGet,animationRefs,[r[0]]),a=ref&&ref.deref();
            if(!a)continue;
            const s=apply(wmGet,animations,[a]);
            promises(a,s,r,1);hold(a,s,r[8]);
        }
    }
    /* steps 4–6: the pending animation event queue, sorted natively, one event per native turn
       (each with its own microtask checkpoint) */
    function events(){
        const rows=native(OP.EVENTS),plans=[];
        for(const r of rows){
            const [id,node,pseudo,type,current,timeline,elapsed,name]=r;
            if(node===null){
                // a CSS event whose element was freed has no target left
                if(type!=='finish'&&type!=='cancel'&&type!=='remove')continue;
                const ref=apply(mGet,animationRefs,[id]),a=ref&&ref.deref();
                if(a)plans.push([a,trusted(new AnimationPlaybackEvent(type,{currentTime:current,timelineTime:timeline}))]);
                continue;
            }
            const init={bubbles:true,elapsedTime:elapsed/1000,pseudoElement:pseudoNames[pseudo]??''};
            if(type.startsWith('animation'))init.animationName=name??'';else init.propertyName=name??'';
            plans.push([node,trusted(type.startsWith('animation')?new AnimationEvent(type,init):new TransitionEvent(type,init))]);
        }
        if(!plans.length)return null;
        let at=0;
        return function nextAnimationEvent(){
            if(at>=plans.length)return false;
            const [target,event]=plans[at++];
            dispatch(target,event);
            return at<plans.length;
        };
    }
    /* ---------------------------------------------------------------- Animatable, DocumentOrShadowRoot */
    const methods={animate(keyframes){
        const target=elementThis(this);
        if(!arguments.length)throw new TypeError('animate requires keyframes');
        keyframesArgument(keyframes);
        const o=effectOptions(arguments[1],true),pseudo=pseudoSelector(o.pseudo);
        checkTiming(o.timing);
        const rows=processKeyframes(keyframes);
        const effect=makeEffect(KeyframeEffect.prototype,target,pseudo,o.timing,o.composite,o.iterationComposite,rows);
        const timeline=o.timeline===undefined?timelineFor(rawDom.get(target,'ownerDocument')):o.timeline;
        const a=new Animation(effect,timeline),s=animationState(a);
        s.label=o.id;
        // the play procedure with auto-rewind; a new animation cannot fail it
        native(OP.PLAY,s.id);sync(a,s);
        return a;
    },
    getAnimations(){
        const target=elementThis(this),o=dictionary(arguments[0],'GetAnimationsOptions');
        let v=o.pseudoElement;const pseudo=v===undefined||v===null?null:pseudoSelector(v);
        const subtree=!!o.subtree;
        const ids=native(OP.LIST,target,subtree,pseudo?pseudo.code:subtree?-1:0),out=[];
        for(const id of ids){
            const a=animationFor(id);
            if(pseudo&&pseudo.code===3){const e=a.effect;if(!e||effectState(e).pseudo===null||effectState(e).pseudo.toLowerCase()!==pseudo.text.toLowerCase())continue;}
            out.push(a);
        }
        return out;
    }};
    function rootAnimations(root,subtree){const out=[];for(const id of native(OP.LIST,root,subtree,-1))out.push(animationFor(id));return out;}
    for(const key of ['animate','getAnimations'])define(Element.prototype,key,{configurable:true,enumerable:true,writable:true,value:methods[key]});
    define(Document.prototype,'timeline',{configurable:true,enumerable:true,get(){
        if(!rawDom.isNode(null,this)||rawDom.get(this,'nodeType')!==9)throw new TypeError('Illegal invocation');
        return timelineFor(this);
    }});
    define(Document.prototype,'getAnimations',{configurable:true,enumerable:true,writable:true,value:{getAnimations(){
        if(!rawDom.isNode(null,this)||rawDom.get(this,'nodeType')!==9)throw new TypeError('Illegal invocation');
        return this===document?rootAnimations(null,false):[];
    }}.getAnimations});
    if(typeof ShadowRoot==='function')define(ShadowRoot.prototype,'getAnimations',{configurable:true,enumerable:true,writable:true,value:{getAnimations(){
        if(!(this instanceof ShadowRoot))throw new TypeError('Illegal invocation');
        return rootAnimations(this,true);
    }}.getAnimations});
    installHandlers(Animation.prototype,['finish','cancel','remove']);
    animationHandlerTarget=(target,type)=>(type==='finish'||type==='cancel'||type==='remove')&&isObject(target)&&apply(wmHas,animations,[target]);
    const interfaces=[AnimationTimeline,DocumentTimeline,AnimationEffect,KeyframeEffect,Animation,CSSAnimation,CSSTransition,
        AnimationPlaybackEvent,AnimationEvent,TransitionEvent];
    for(const C of interfaces){
        define(C.prototype,Symbol.toStringTag,{value:C.name,configurable:true});
        for(const key of Object.getOwnPropertyNames(C.prototype)){
            if(key==='constructor')continue;
            const d=Object.getOwnPropertyDescriptor(C.prototype,key);d.enumerable=true;define(C.prototype,key,d);
        }
        define(globalThis,C.name,{value:C,writable:true,configurable:true,enumerable:false});
    }
    return {update,events};
})();
