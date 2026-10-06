/* Native layout observers. Geometry is read from Nocturne's actual box tree;
   there is no always-visible result and no synthetic periodic resize event. */
const observerBridge=(()=>{
    const roSlots=new WeakMap(),ioSlots=new WeakMap(),resize=new Set(),intersections=new Set();
    const rectSlots=new WeakMap();
    class DOMRectReadOnly {
        constructor(x=0,y=0,width=0,height=0){rectSlots.set(this,[Number(x),Number(y),Number(width),Number(height)]);}
        static fromRect(r={}){return new DOMRectReadOnly(r.x,r.y,r.width,r.height);}
        get x(){return rectSlots.get(this)[0];}get y(){return rectSlots.get(this)[1];}
        get width(){return rectSlots.get(this)[2];}get height(){return rectSlots.get(this)[3];}
        get top(){return Math.min(this.y,this.y+this.height);}get bottom(){return Math.max(this.y,this.y+this.height);}
        get left(){return Math.min(this.x,this.x+this.width);}get right(){return Math.max(this.x,this.x+this.width);}
        toJSON(){return {x:this.x,y:this.y,width:this.width,height:this.height,top:this.top,right:this.right,bottom:this.bottom,left:this.left};}
    }
    const rect=a=>new DOMRectReadOnly(...a);
    function readonly(object,values){for(const key of Object.keys(values))Object.defineProperty(object,key,{enumerable:true,value:values[key]});return object;}
    function slot(map,value){const s=map.get(value);if(!s)throw new TypeError('Illegal observer receiver');return s;}
    function element(target){if(!(target instanceof Element))throw new TypeError('Expected an Element');}
    function wake(){host.observers(resize.size>0 || intersections.size>0);}
    class ResizeObserverEntry {constructor(){throw new TypeError('Illegal constructor');}}
    class ResizeObserverSize {constructor(){throw new TypeError('Illegal constructor');}}
    function sizes(a){const s=Object.create(ResizeObserverSize.prototype);readonly(s,{inlineSize:a[2],blockSize:a[3]});return Object.freeze([s]);}
    class ResizeObserver {
        constructor(callback){if(typeof callback!=='function')throw new TypeError('Callback is required');roSlots.set(this,{callback,targets:new Map()});}
        observe(target,options={}){
            const s=slot(roSlots,this);element(target);const box=String(options?.box??'content-box');
            if(!['content-box','border-box','device-pixel-content-box'].includes(box))throw new TypeError('Invalid observed box');
            const old=s.targets.get(target);if(old && old.box===box)return;
            s.targets.set(target,{box,w:0,h:0});resize.add(this);wake();
        }
        unobserve(target){const s=slot(roSlots,this);element(target);s.targets.delete(target);if(!s.targets.size)resize.delete(this);wake();}
        disconnect(){slot(roSlots,this).targets.clear();resize.delete(this);wake();}
    }
    function depth(target){let d=0;for(let n=target;n;n=n.parentNode)d++;return d;}
    function resizeFrame(){
        let limit=0,skipped=false;
        for(;;){
            let shallow=Infinity;const batches=[];skipped=false;
            for(const observer of Array.from(resize)){
                const s=roSlots.get(observer),entries=[];
                for(const [target,old] of s.targets){
                    const g=dom('observerGeometry',target,null),device=[0,0,Math.round(g.content[2]),Math.round(g.content[3])];
                    const box=old.box==='border-box'?g.border:old.box==='device-pixel-content-box'?device:g.content;
                    if(box[2]===old.w && box[3]===old.h)continue;
                    const d=depth(target);if(d<=limit){skipped=true;continue;}
                    old.w=box[2];old.h=box[3];shallow=Math.min(shallow,d);
                    const e=Object.create(ResizeObserverEntry.prototype);
                    entries.push(readonly(e,{target,contentRect:rect(g.content),borderBoxSize:sizes(g.border),
                        contentBoxSize:sizes(g.content),devicePixelContentBoxSize:sizes(device)}));
                }
                if(entries.length)batches.push({observer,s,entries});
            }
            if(!batches.length)break;
            for(const b of batches)try{b.s.callback.call(b.observer,b.entries,b.observer);}catch(e){report(e);}
            limit=shallow;
        }
        if(skipped)report(new Error('ResizeObserver loop completed with undelivered notifications.'));
    }
    function margins(value){
        const words=String(value).trim().split(/\s+/);
        if(words.length<1 || words.length>4)throw new DOMException('Invalid rootMargin','SyntaxError');
        const a=words.map(x=>{const m=/^([+-]?(?:\d+(?:\.\d*)?|\.\d+))(px|%)$/.exec(x);
            if(!m || !Number.isFinite(Number(m[1])))throw new DOMException('Invalid rootMargin','SyntaxError');
            return {value:Number(m[1]),unit:m[2]};});
        return [a[0],a[1]||a[0],a[2]||a[0],a[3]||a[1]||a[0]];
    }
    class IntersectionObserverEntry {constructor(){throw new TypeError('Illegal constructor');}}
    class IntersectionObserver {
        constructor(callback,options={}){
            if(typeof callback!=='function')throw new TypeError('Callback is required');options=options??{};
            const root=options.root??null;if(root!==null && !(root instanceof Element) && !(root instanceof Document))throw new TypeError('Invalid root');
            const margin=margins(options.rootMargin??'0px');
            let thresholds=options.threshold??0;
            thresholds=typeof thresholds==='object' && thresholds!==null?Array.from(thresholds,Number):[Number(thresholds)];
            if(thresholds.some(x=>!Number.isFinite(x)||x<0||x>1))throw new RangeError('Threshold must be between zero and one');
            thresholds.sort((a,b)=>a-b);if(!thresholds.length)thresholds.push(0);
            ioSlots.set(this,{callback,root,margin,thresholds:Object.freeze(thresholds),targets:new Map(),pending:[],queued:false});
        }
        get root(){return slot(ioSlots,this).root;}
        get rootMargin(){return slot(ioSlots,this).margin.map(m=>m.value+m.unit).join(' ');}
        get thresholds(){return slot(ioSlots,this).thresholds;}
        observe(target){const s=slot(ioSlots,this);element(target);if(s.targets.has(target))return;
            s.targets.set(target,{index:-1,inside:false});intersections.add(this);wake();}
        unobserve(target){const s=slot(ioSlots,this);element(target);s.targets.delete(target);if(!s.targets.size)intersections.delete(this);wake();}
        disconnect(){slot(ioSlots,this).targets.clear();intersections.delete(this);wake();}
        takeRecords(){const s=slot(ioSlots,this),records=s.pending;s.pending=[];return records;}
    }
    function intersectionFrame(){
        for(const observer of Array.from(intersections)){
            const s=ioSlots.get(observer);
            for(const [target,old] of s.targets){
                const g=dom('observerGeometry',target,s.root),r=g.root;
                const m=s.margin.map(x=>x.unit==='%'?x.value*r[2]/100:x.value);
                const bounds=[r[0]-m[3],r[1]-m[0],Math.max(0,r[2]+m[1]+m[3]),Math.max(0,r[3]+m[0]+m[2])];
                let left=g.rect[0],top=g.rect[1],right=left+g.rect[2],bottom=top+g.rect[3],inside=g.intersectsRoot;
                for(const c of g.clips.concat([bounds])){
                    left=Math.max(left,c[0]);top=Math.max(top,c[1]);right=Math.min(right,c[0]+c[2]);bottom=Math.min(bottom,c[1]+c[3]);
                    if(right<left || bottom<top)inside=false;
                }
                const ir=inside?[left,top,Math.max(0,right-left),Math.max(0,bottom-top)]:[0,0,0,0];
                const area=g.rect[2]*g.rect[3],ratio=area?ir[2]*ir[3]/area:inside?1:0;
                let index=0;while(index<s.thresholds.length && s.thresholds[index]<=ratio)index++;
                if(index!==old.index || inside!==old.inside){
                    old.index=index;old.inside=inside;
                    const entry=Object.create(IntersectionObserverEntry.prototype);
                    s.pending.push(readonly(entry,{target,time:host.now(),rootBounds:rect(bounds),boundingClientRect:rect(g.rect),
                        intersectionRect:rect(ir),isIntersecting:inside,intersectionRatio:ratio}));
                }
            }
            if(s.pending.length && !s.queued){s.queued=true;setTimeout(()=>{
                s.queued=false;const records=observer.takeRecords();
                if(records.length)try{s.callback.call(observer,records,observer);}catch(e){report(e);}
            },0);}
        }
    }
    Object.assign(globalThis,{DOMRectReadOnly,ResizeObserver,ResizeObserverEntry,ResizeObserverSize,IntersectionObserver,IntersectionObserverEntry});
    return {frame(){resizeFrame();intersectionFrame();}};
})();
