/* A document-clock Web Animations subset. Samples enter the native cascade;
   author inline styles are never used as animation storage. No scroll timeline,
   additive composition or CSS-transition integration is advertised here. */
{
    const effects=new WeakMap(),animations=new WeakMap(),timelines=new WeakMap();
    const relevant=new Set(),running=new Set();
    let frame=0,nextId=1;
    const animationId=()=>{if(nextId>0xffffffff)throw new RangeError('Native animation identity is not representable');return nextId++;};
    const finite=(v,name)=>{v=Number(v);if(!Number.isFinite(v))throw new TypeError('Invalid '+name);return v;};
    const element=(v)=>{if(v!==null && (!rawDom('isNode',null,v)||rawDom('get',v,'nodeType')!==1))throw new TypeError('Expected Element');return v;};
    const effectState=(v)=>{const s=effects.get(v);if(!s)throw new TypeError('Illegal AnimationEffect receiver');return s;};
    const animationState=(v)=>{const s=animations.get(v);if(!s)throw new TypeError('Illegal Animation receiver');return s;};
    const names=new Set(['offset','easing','composite','computedOffset']);
    function cssValue(value){return String(value);}
    function easing(value) {
        const text=String(value).trim(),aliases={ease:[.25,.1,.25,1],'ease-in':[.42,0,1,1],'ease-out':[0,0,.58,1],'ease-in-out':[.42,0,.58,1]};
        if(text==='linear')return x=>x;
        const step=/^steps\(\s*(\d+)\s*(?:,\s*(start|end|jump-start|jump-end|jump-none|jump-both)\s*)?\)$/.exec(text);
        if(text==='step-start')return x=>1;
        if(text==='step-end')return x=>x===1?1:0;
        if(step){const n=Number(step[1]),where=step[2]||'end';if(!n||(where==='jump-none'&&n===1))throw new TypeError('Invalid easing');return x=>{let k=Math.floor(x*n),d=n;if(where==='start'||where==='jump-start'||where==='jump-both')k++;if(where==='jump-both')d++;if(where==='jump-none')d--;return Math.max(0,Math.min(1,k/d));};}
        const match=/^cubic-bezier\(\s*([-+\d.e]+)\s*,\s*([-+\d.e]+)\s*,\s*([-+\d.e]+)\s*,\s*([-+\d.e]+)\s*\)$/.exec(text);
        const p=aliases[text]||(match?match.slice(1).map(Number):null);
        if(!p||!p.every(Number.isFinite)||p[0]<0||p[0]>1||p[2]<0||p[2]>1)throw new TypeError('Invalid easing');
        const bez=(t,a,b)=>3*(1-t)*(1-t)*t*a+3*(1-t)*t*t*b+t*t*t;
        return x=>{if(x<=0||x>=1)return x;let lo=0,hi=1,t=x;for(let i=0;i<20;i++){const z=bez(t,p[0],p[2]);if(Math.abs(z-x)<1e-6)break;if(z<x)lo=t;else hi=t;t=(lo+hi)/2;}return bez(t,p[1],p[3]);};
    }
    function timing(options,previous) {
        const o=typeof options==='number'?{duration:options}:(options==null?{}:Object(options));
        const t=Object.assign({delay:0,endDelay:0,fill:'auto',iterationStart:0,iterations:1,duration:'auto',direction:'normal',easing:'linear'},previous);
        for(const key of Object.keys(t))if(o[key]!==undefined)t[key]=o[key];
        t.delay=finite(t.delay,'delay');t.endDelay=finite(t.endDelay,'endDelay');
        t.iterationStart=finite(t.iterationStart,'iterationStart');t.iterations=Number(t.iterations);
        if(t.iterationStart<0||Number.isNaN(t.iterations)||t.iterations<0)throw new TypeError('Invalid iterations');
        if(t.duration!=='auto'){t.duration=Number(t.duration);if(Number.isNaN(t.duration)||t.duration<0)throw new TypeError('Invalid duration');}
        t.fill=String(t.fill);t.direction=String(t.direction);t.easing=String(t.easing);
        if(!['auto','none','forwards','backwards','both'].includes(t.fill)||!['normal','reverse','alternate','alternate-reverse'].includes(t.direction))throw new TypeError('Invalid animation timing');
        easing(t.easing);return t;
    }
    const duration=t=>t.duration==='auto'?0:t.duration;
    const activeDuration=t=>duration(t)===0||t.iterations===0?0:duration(t)*t.iterations;
    const endTime=t=>Math.max(0,t.delay+activeDuration(t)+t.endDelay);
    function frames(input) {
        if(input==null)return [];
        let out=[];
        if(Array.isArray(input)||typeof input[Symbol.iterator]==='function'){
            for(const entry of input){if(entry==null||typeof entry!=='object')throw new TypeError('Invalid keyframe');const f={offset:entry.offset==null?null:finite(entry.offset,'offset'),easing:entry.easing===undefined?'linear':String(entry.easing),composite:entry.composite===undefined?'auto':String(entry.composite)};for(const key of Object.keys(entry))if(!names.has(key))f[cssName(key)]=cssValue(entry[key]);out.push(f);}
        }else{
            const object=Object(input),byOffset=new Map();
            for(const key of Object.keys(object)){
                if(names.has(key))continue;
                const values=Array.isArray(object[key])?object[key]:[object[key]];
                for(let i=0;i<values.length;i++){const off=values.length===1?1:i/(values.length-1);let f=byOffset.get(off);if(!f)byOffset.set(off,f={offset:off,easing:'linear',composite:'auto'});f[cssName(key)]=cssValue(values[i]);}
            }
            out=Array.from(byOffset.values()).sort((a,b)=>a.offset-b.offset);
            const offsets=object.offset==null?null:(Array.isArray(object.offset)?object.offset:[object.offset]);
            const easings=object.easing===undefined?null:(Array.isArray(object.easing)?object.easing:[object.easing]);
            const composites=object.composite===undefined?null:(Array.isArray(object.composite)?object.composite:[object.composite]);
            for(let i=0;i<out.length;i++){if(offsets&&i<offsets.length)out[i].offset=offsets[i]==null?null:finite(offsets[i],'offset');if(easings&&easings.length)out[i].easing=String(easings[i%easings.length]);if(composites&&composites.length)out[i].composite=String(composites[i%composites.length]);}
        }
        let last=-1;const properties=new Set();
        for(const f of out){for(const key of Object.keys(f))if(!names.has(key))properties.add(key);if(f.offset!==null){if(f.offset<0||f.offset>1||f.offset<last)throw new TypeError('Keyframe offsets are not ordered');last=f.offset;}easing(f.easing);if(!['auto','replace'].includes(f.composite))throw new DOMException('Additive animation composition is not implemented','NotSupportedError');}
        if(!out.length)return out;
        if(out.length===1&&out[0].offset===null)out[0].offset=1;
        if(out[0].offset===null)out[0].offset=0;
        if(out[out.length-1].offset===null)out[out.length-1].offset=1;
        for(let left=0;left<out.length-1;){let right=left+1;while(out[right].offset===null)right++;for(let i=left+1;i<right;i++)out[i].offset=out[left].offset+(out[right].offset-out[left].offset)*(i-left)/(right-left);left=right;}
        return out;
    }
    function progress(t,local) {
        if(local===null)return null;
        const ad=activeDuration(t),limit=Math.min(t.delay+ad,endTime(t));
        let active;
        if(local<Math.min(t.delay,limit)){if(!['backwards','both'].includes(t.fill))return null;active=0;}
        else if(local>=limit){if(!['forwards','both'].includes(t.fill))return null;active=ad;}
        else active=Math.max(0,local-t.delay);
        let overall=duration(t)===0?(active===0&&local<t.delay?0:t.iterations):active/duration(t);
        overall+=t.iterationStart;
        let iteration=Math.floor(overall),part=overall%1;
        if(active===ad&&t.iterations!==0&&part===0){part=1;iteration=Math.max(0,iteration-1);}
        if(t.direction==='reverse'||t.direction==='alternate'&&iteration%2===1||t.direction==='alternate-reverse'&&iteration%2===0)part=1-part;
        return {value:easing(t.easing)(part),iteration};
    }
    function transformIdentity(text) {
        return text.replace(/(translate(?:X|Y|Z|3d)?|scale(?:X|Y|Z|3d)?|rotate(?:X|Y|Z)?|skew(?:X|Y)?|matrix)\(([^)]*)\)/g,(_,fn,args)=>{
            const numbers=args.trim().split(/\s*,\s*|\s+/).filter(Boolean);
            return fn+'('+numbers.map((s,i)=>fn==='matrix'?(i===0||i===3?'1':'0'):fn.startsWith('scale')?'1':s.replace(/[-+]?(?:\d*\.)?\d+(?:e[-+]?\d+)?/i,'0')).join(', ')+')';
        });
    }
    function color(text){
        if(text==='transparent')return [0,0,0,0];
        const named={black:'#000',white:'#fff',red:'#f00',green:'#008000',blue:'#00f'};
        text=named[text]||text;
        if(/^#[0-9a-f]{3,4}$/i.test(text))text='#'+text.slice(1).split('').map(x=>x+x).join('');
        if(/^#[0-9a-f]{6}(?:[0-9a-f]{2})?$/i.test(text))return [parseInt(text.slice(1,3),16),parseInt(text.slice(3,5),16),parseInt(text.slice(5,7),16),text.length===9?parseInt(text.slice(7,9),16)/255:1];
        const match=/^rgba?\(([^)]+)\)$/i.exec(text);if(!match)return null;
        const parts=match[1].trim().split(/\s*,\s*|\s*\/\s*|\s+/);if(parts.length!==3&&parts.length!==4)return null;
        const out=parts.map((v,i)=>Number(v.replace('%',''))*(v.endsWith('%')?(i===3?.01:2.55):1));
        if(!out.every(Number.isFinite))return null;if(out.length===3)out.push(1);
        for(let i=0;i<3;i++)out[i]=Math.max(0,Math.min(255,out[i]));out[3]=Math.max(0,Math.min(1,out[3]));return out;
    }
    function interpolate(a,b,p,key) {
        if(a===b)return a;
        if(key==='transform'){if(!a||a==='none')a=transformIdentity(b);if(!b||b==='none')b=transformIdentity(a);}
        const numeric=/^([-+]?(?:\d*\.)?\d+(?:e[-+]?\d+)?)([a-z%]*)$/i,na=numeric.exec(a),nb=numeric.exec(b);
        if(na&&nb&&(na[2]===nb[2]||Number(na[1])===0||Number(nb[1])===0))return String(Number(na[1])+(Number(nb[1])-Number(na[1]))*p)+(nb[2]||na[2]);
        if(key==='color'||key.endsWith('-color')){const aa=color(a),bb=color(b);if(aa&&bb){const alpha=aa[3]+(bb[3]-aa[3])*p,rgb=aa.slice(0,3).map((v,i)=>alpha===0?0:Math.max(0,Math.min(255,(v*aa[3]+(bb[i]*bb[3]-v*aa[3])*p)/alpha)));return 'rgba('+rgb.join(', ')+', '+Math.max(0,Math.min(1,alpha))+')';}}
        if(key==='transform'){
            const number=/[-+]?(?:\d*\.)?\d+(?:e[-+]?\d+)?/ig,aa=a.match(number),bb=b.match(number);
            if(aa&&bb&&aa.length===bb.length&&a.replace(number,'#').replace(/\s/g,'')===b.replace(number,'#').replace(/\s/g,'')){let i=0;return b.replace(number,()=>String(Number(aa[i])+(Number(bb[i])-Number(aa[i]))*p+(i++*0)));}
        }
        // Values without an implemented interpolation type use CSS discrete
        // interpolation, not a fabricated numeric conversion (auto, %, etc.).
        return p<.5?a:b;
    }
    function values(effect,local) {
        const e=effectState(effect),p=progress(e.timing,local);if(!e.target||!p||!e.frames.length)return null;
        const tracks=new Map();
        for(const f of e.frames)for(const key of Object.keys(f))if(!names.has(key)){let a=tracks.get(key);if(!a)tracks.set(key,a=[]);a.push({offset:f.offset,value:f[key],easing:f.easing});}
        const pairs=[];
        for(const [key,a] of tracks){
            const base=()=>String(rawDom('animationComputed',e.target,e.pseudo,key)||'');
            if(a[0].offset!==0)a.unshift({offset:0,value:base(),easing:'linear'});
            if(a[a.length-1].offset!==1)a.push({offset:1,value:base(),easing:'linear'});
            let left=0;while(left<a.length-2&&p.value>=a[left+1].offset)left++;
            const x=a[left],y=a[Math.min(left+1,a.length-1)],span=y.offset-x.offset;
            const portion=span===0?1:easing(x.easing)((p.value-x.offset)/span);
            pairs.push(key,interpolate(x.value,y.value,portion,key));
        }
        return pairs;
    }
    function promiseRecord(){let resolve,reject;const promise=new Promise((yes,no)=>{resolve=yes;reject=no;});return {promise,resolve,reject};}
    function localTime(s){return s.state==='idle'?null:s.state==='running'&&s.start!==null?(host.now()-s.start)*s.rate:s.hold;}
    function clearSample(s){if(s.target)rawDom('animationStyle',s.target,s.pseudo,s.id,null);s.target=null;s.pseudo='';s.pairs=null;}
    function sample(a){
        const s=animationState(a),e=s.effect&&effectState(s.effect);
        if(s.target&&(!e||s.target!==e.target||s.pseudo!==e.pseudo))clearSample(s);
        if(!e||!e.target){clearSample(s);return;}
        const pairs=values(s.effect,localTime(s));
        if(pairs){if(!s.pairs||pairs.length!==s.pairs.length||pairs.some((value,i)=>value!==s.pairs[i]))rawDom('animationStyle',e.target,e.pseudo,s.id,pairs);s.target=e.target;s.pseudo=e.pseudo;s.pairs=pairs;}
        else clearSample(s);
    }
    function enqueueEvent(a,type){const s=animationState(a),generation=s.generation,current=localTime(s);setTimeout(()=>{if(s.generation!==generation)return;const event=new AnimationPlaybackEvent(type,{currentTime:current,timelineTime:host.now()});a.dispatchEvent(event);},0);}
    function completed(a){
        const s=animationState(a);s.hold=s.rate<0?0:endTime(effectState(s.effect).timing);s.state='finished';s.start=null;running.delete(a);sample(a);
        if(!s.target)relevant.delete(a);if(s.finished)s.finished.resolve(a);enqueueEvent(a,'finish');
    }
    function failed(a,error,notify){
        const s=animationState(a);s.generation++;
        try{clearSample(s);}catch(cleanupError){s.target=null;s.pseudo='';s.pairs=null;report(cleanupError);}
        s.state='idle';s.start=s.hold=null;running.delete(a);relevant.delete(a);
        if(s.finished)s.finished.reject(error);s.finished=null;
        if(notify)report(error);
    }
    function tick(){
        frame=0;
        try{for(const a of Array.from(running)){
            try{const s=animationState(a),local=localTime(s),end=s.effect?endTime(effectState(s.effect).timing):0;if(s.rate!==0&&(s.rate>0?local>=end:local<=0))completed(a);else sample(a);}
            catch(error){failed(a,error,true);}
        }}finally{schedule();}
    }
    function schedule(){const moving=Array.from(running).some(a=>animationState(a).rate!==0);if(!frame&&moving)frame=requestAnimationFrame(tick);else if(frame&&!moving){cancelAnimationFrame(frame);frame=0;}}
    function remember(a){relevant.add(a);}
    function changed(effect){const e=effectState(effect);if(e.animation){sample(e.animation);schedule();}}
    class AnimationTimeline {constructor(){throw new TypeError('Illegal AnimationTimeline constructor');}get currentTime(){if(!timelines.has(this))throw new TypeError('Illegal AnimationTimeline receiver');return host.now()-timelines.get(this).origin;}}
    class DocumentTimeline extends AnimationTimeline {
        constructor(options={}){const timeline=Object.create(new.target.prototype);timelines.set(timeline,{origin:finite(options.originTime===undefined?0:options.originTime,'originTime')});return timeline;}
    }
    const timeline=new DocumentTimeline();
    class AnimationEffect {
        constructor(){throw new TypeError('Illegal AnimationEffect constructor');}
        getTiming(){return Object.assign({},effectState(this).timing);}
        getComputedTiming(){const e=effectState(this),local=e.animation?localTime(animationState(e.animation)):null,p=progress(e.timing,local);return Object.assign({},e.timing,{fill:e.timing.fill==='auto'?'none':e.timing.fill,duration:duration(e.timing),activeDuration:activeDuration(e.timing),endTime:endTime(e.timing),localTime:local,progress:p?p.value:null,currentIteration:p?p.iteration:null});}
        updateTiming(options={}){const e=effectState(this);e.timing=timing(options,e.timing);changed(this);}
    }
    class KeyframeEffect extends AnimationEffect {
        constructor(target,input,options={}){
            const effect=Object.create(new.target.prototype);let e;
            if(effects.has(target)){const source=effectState(target);e={target:source.target,frames:source.frames.map(f=>Object.assign({},f)),timing:Object.assign({},source.timing),pseudo:source.pseudo,animation:null};}
            else{const o=typeof options==='number'?{duration:options}:options==null?{}:Object(options);if(o.composite!==undefined&&o.composite!=='replace')throw new DOMException('Additive animation composition is not implemented','NotSupportedError');if(o.iterationComposite!==undefined&&o.iterationComposite!=='replace')throw new DOMException('Iteration accumulation is not implemented','NotSupportedError');e={target:element(target),frames:frames(input),timing:timing(o),pseudo:'',animation:null};}
            effects.set(effect,e);if(!effects.has(target)&&options&&typeof options==='object'&&options.pseudoElement!==undefined)effect.pseudoElement=options.pseudoElement;return effect;
        }
        get target(){return effectState(this).target;}set target(v){effectState(this).target=element(v);changed(this);}
        get pseudoElement(){return effectState(this).pseudo||null;}set pseudoElement(v){v=v==null?'':String(v);if(!['','::before','::after'].includes(v))throw new DOMException('Unsupported animation pseudo-element','SyntaxError');effectState(this).pseudo=v;changed(this);}
        get composite(){effectState(this);return 'replace';}set composite(v){effectState(this);if(String(v)!=='replace')throw new DOMException('Additive animation composition is not implemented','NotSupportedError');}
        get iterationComposite(){effectState(this);return 'replace';}set iterationComposite(v){effectState(this);if(String(v)!=='replace')throw new DOMException('Iteration accumulation is not implemented','NotSupportedError');}
        getKeyframes(){return effectState(this).frames.map(f=>Object.assign({},f,{computedOffset:f.offset}));}
        setKeyframes(v){effectState(this).frames=frames(v);changed(this);}
    }
    class AnimationPlaybackEvent extends Event {constructor(type,options={}){super(type,options);this.currentTime=options.currentTime==null?null:finite(options.currentTime,'currentTime');this.timelineTime=options.timelineTime==null?null:finite(options.timelineTime,'timelineTime');}}
    class Animation extends EventTarget {
        constructor(effect=null,clock=timeline){super();if(clock!==timeline)throw new DOMException('Only the document timeline is implemented','NotSupportedError');animations.set(this,{id:animationId(),effect:null,state:'idle',hold:null,start:null,rate:1,target:null,pseudo:'',pairs:null,finished:null,ready:null,persisted:false,generation:0,handlers:new Map()});this.id='';this.effect=effect;}
        get effect(){return animationState(this).effect;}set effect(v){const s=animationState(this);if(v!==null)effectState(v);if(s.effect)effectState(s.effect).animation=null;if(v){const e=effectState(v);if(e.animation&&e.animation!==this)e.animation.effect=null;e.animation=this;}s.effect=v;sample(this);}
        get timeline(){animationState(this);return timeline;}set timeline(v){animationState(this);if(v!==timeline)throw new DOMException('Only the document timeline is implemented','NotSupportedError');}
        get currentTime(){return localTime(animationState(this));}set currentTime(v){const s=animationState(this);if(v===null){if(s.state!=='idle')throw new TypeError('Cannot clear a running currentTime');return;}s.hold=finite(v,'currentTime');if(s.state==='running')s.start=s.rate?host.now()-s.hold/s.rate:null;else if(s.state==='idle')s.state='paused';else if(s.state==='finished'){s.state='paused';s.finished=null;s.generation++;}remember(this);sample(this);}
        get startTime(){return animationState(this).start;}set startTime(v){const s=animationState(this);s.hold=localTime(s);s.start=v==null?null:finite(v,'startTime');if(s.start!==null){remember(this);s.state='running';running.add(this);}else{running.delete(this);if(s.state!=='idle')s.state='paused';}sample(this);schedule();}
        get playbackRate(){return animationState(this).rate;}set playbackRate(v){const s=animationState(this),hold=localTime(s);s.rate=finite(v,'playbackRate');s.hold=hold;if(s.state==='running')s.start=s.rate?host.now()-hold/s.rate:null;sample(this);schedule();}
        get playState(){return animationState(this).state;}get pending(){animationState(this);return false;}get replaceState(){return animationState(this).persisted?'persisted':'active';}
        get ready(){const s=animationState(this);return s.ready||(s.ready=Promise.resolve(this));}
        get finished(){const s=animationState(this);if(!s.finished)s.finished=promiseRecord();if(s.state==='finished')s.finished.resolve(this);return s.finished.promise;}
        play(){const s=animationState(this),end=s.effect?endTime(effectState(s.effect).timing):0;let hold=localTime(s);if(hold===null||s.state==='finished'||s.rate>0&&hold>=end||s.rate<0&&hold<=0){if(s.rate<0&&!Number.isFinite(end))throw new DOMException('Cannot reverse an infinite animation','InvalidStateError');hold=s.rate<0?end:0;}remember(this);if(s.state==='finished')s.finished=null;s.generation++;s.hold=hold;s.start=s.rate?host.now()-hold/s.rate:null;s.state='running';running.add(this);try{sample(this);}catch(error){failed(this,error,false);throw error;}finally{schedule();}}
        pause(){const s=animationState(this);s.hold=localTime(s);if(s.hold===null)s.hold=0;if(s.state==='finished')s.finished=null;s.start=null;s.state='paused';s.generation++;running.delete(this);remember(this);sample(this);schedule();}
        reverse(){this.updatePlaybackRate(this.playbackRate===0?-1:-this.playbackRate);this.play();}
        updatePlaybackRate(value){this.playbackRate=value;}
        finish(){const s=animationState(this),end=s.effect?endTime(effectState(s.effect).timing):0;if(s.rate===0||s.rate>0&&!Number.isFinite(end))throw new DOMException('Cannot finish this animation','InvalidStateError');if(s.state==='finished')return;remember(this);s.generation++;if(s.effect)completed(this);else{s.hold=0;s.state='finished';running.delete(this);if(s.finished)s.finished.resolve(this);enqueueEvent(this,'finish');relevant.delete(this);}schedule();}
        cancel(){const s=animationState(this),was=s.state!=='idle';s.generation++;clearSample(s);s.state='idle';s.start=s.hold=null;running.delete(this);relevant.delete(this);if(s.finished&&was)s.finished.reject(new DOMException('Animation cancelled','AbortError'));s.finished=null;if(was)enqueueEvent(this,'cancel');schedule();}
        persist(){animationState(this).persisted=true;}
        commitStyles(){const s=animationState(this);if(!s.effect||s.state==='idle')throw new DOMException('No animation effect to commit','InvalidStateError');const e=effectState(s.effect);if(e.pseudo)throw new DOMException('Pseudo-element styles cannot be committed','NoModificationAllowedError');const pairs=values(s.effect,localTime(s));if(!e.target||!pairs)throw new DOMException('Animation is not in effect','InvalidStateError');for(let i=0;i<pairs.length;i+=2)e.target.style.setProperty(pairs[i],pairs[i+1]);}
    }
    for(const type of ['finish','cancel','remove'])Object.defineProperty(Animation.prototype,'on'+type,{configurable:true,enumerable:true,get(){return animationState(this).handlers.get(type)||null;},set(value){const s=animationState(this),old=s.handlers.get(type);if(old)this.removeEventListener(type,old);if(typeof value==='function'){s.handlers.set(type,value);this.addEventListener(type,value);}else s.handlers.delete(type);}});
    function getAnimations(root,options={}){const subtree=!!Object(options).subtree;return Array.from(relevant).filter(a=>{const s=animationState(a),e=s.effect&&effectState(s.effect);if(!e||!e.target||s.state==='idle')return false;if(root instanceof Document)return e.target.ownerDocument===root;if(root===e.target)return true;if(!subtree)return false;for(let node=e.target;node;node=node.parentNode||rawDom('get',node,'shadowHost'))if(node===root)return true;return false;});}
    Object.defineProperties(Element.prototype,{animate:{configurable:true,writable:true,value:function(input,options={}){element(this);const effect=new KeyframeEffect(this,input,options),a=new Animation(effect);if(options&&typeof options==='object'&&options.id!==undefined)a.id=String(options.id);a.play();return a;}},getAnimations:{configurable:true,writable:true,value:function(options={}){element(this);return getAnimations(this,options);}}});
    Object.defineProperties(Document.prototype,{timeline:{configurable:true,get(){if(this!==document)throw new DOMException('Inactive document timeline','NotSupportedError');return timeline;}},getAnimations:{configurable:true,writable:true,value:function(){if(this!==document)throw new DOMException('Inactive document animations','NotSupportedError');return getAnimations(this);}}});
    Object.assign(globalThis,{Animation,AnimationEffect,KeyframeEffect,AnimationTimeline,DocumentTimeline,AnimationPlaybackEvent});
}
