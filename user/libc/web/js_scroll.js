/* CSSOM View scrolling: "perform a scroll" (§3.1) of the viewport or of an element's scrolling
 * box, instant or smooth, and the methods that start one: Window scroll()/scrollTo()/scrollBy()
 * (§4) and Element scroll()/scrollTo()/scrollBy()/scrollIntoView() and the scrollTop/scrollLeft
 * setters (§6), each returning the Promise of its ScrollResult. The one owner of programmatic
 * scrolling: the positions, limits and the scroll-into-view plan are native (js.c geometry
 * "scrollLeft"/"scrollTop"/"scrollBox"/"scrollMaxLeft"/"scrollMaxTop", host.scroll,
 * rawDom.elementScrollIntoView), the viewport's scroll event is native and an element's is
 * queued here. Include after js_html_elements.js. https://drafts.csswg.org/cssom-view/#scrolling */
const scrollBridge = (() => {
    'use strict';
    const define=Object.defineProperty,PromiseImpl=Promise,TypeErrorImpl=TypeError,finite=Number.isFinite;
    const mathMin=Math.min,mathMax=Math.max,mathSqrt=Math.sqrt;
    const string=elementURL.string,postScrollTask=host.postTask;
    const frame=fn=>host.timer(2,fn,16,[]),cancelFrame=id=>host.clear(id);
    const viewport={}; /* the viewport's scrolling box; an element's is the element */
    const ongoing=new Map(); /* scrolling box → its ongoing smooth scroll */
    const scrollTasks=new WeakMap();
    const resolvedResult=interrupted=>new PromiseImpl(resolve=>resolve({interrupted}));
    const rejected=error=>new PromiseImpl((resolve,reject)=>reject(error));
    /* Web IDL unrestricted double, then "normalize non-finite values" */
    function scrollNumber(value){
        value=+value; // ToNumber: Symbol and BigInt throw
        return finite(value)?value:0;
    }
    function scrollBehavior(value){
        value=string(value);
        if(value!=='auto' && value!=='instant' && value!=='smooth')throw new TypeErrorImpl('Invalid scroll behavior');
        return value;
    }
    /* ScrollToOptions: the inherited behavior first, then left and top (Web IDL member order). */
    function scrollToOptions(options){
        if(options!=null && typeof options!=='object' && typeof options!=='function')
            throw new TypeErrorImpl('Scroll options must be a dictionary');
        const out={behavior:'auto',left:undefined,top:undefined};
        if(options!=null){
            const b=options.behavior;if(b!==undefined)out.behavior=scrollBehavior(b);
            const left=options.left;if(left!==undefined)out.left=scrollNumber(left);
            const top=options.top;if(top!==undefined)out.top=scrollNumber(top);
        }
        return out;
    }
    function queueElementScroll(node){
        if(scrollTasks.has(node))return;
        const owner=rawDom.get(node,'ownerDocument');
        const record={owner};scrollTasks.set(node,record);
        try{postScrollTask(()=>{
            if(scrollTasks.get(node)!==record)return;
            scrollTasks.delete(node);
            if(rawDom.get(node,'ownerDocument')!==owner || !rawDom.get(node,'elementScrollActive'))return;
            const event=new Event('scroll');eventState(event).isTrusted=true;dispatch(node,event);
        });}catch(error){scrollTasks.delete(node);throw error;}
    }
    function position(box){
        return box===viewport?[host.scroll(0),host.scroll(1)]:
            [rawDom.geometry(box,'scrollLeft'),rawDom.geometry(box,'scrollTop')];
    }
    function limits(box){
        return box===viewport?[host.scroll(2),host.scroll(3)]:
            [rawDom.geometry(box,'scrollMaxLeft'),rawDom.geometry(box,'scrollMaxTop')];
    }
    /* "instant scroll": native clamps, commits and repaints; the viewport's scroll event is
       native, an element's is queued when its position changed. */
    function instant(box,x,y){
        if(box===viewport)host.scroll(x,y);
        else if(rawDom.elementScroll(box,x,y))queueElementScroll(box);
    }
    function rootElement(node){
        return rawDom.get(node?rawDom.get(node,'ownerDocument'):document,'documentElement');
    }
    /* "If the user agent honors the scroll-behavior property": behavior smooth, or auto and the
       associated element's computed scroll-behavior is smooth. */
    function smoothScroll(associated,behavior){
        if(behavior==='smooth')return true;
        return behavior==='auto' && !!associated && dom('computed',associated,'scroll-behavior',null)==='smooth';
    }
    /* "Abort any ongoing smooth scroll for box" and resolve its pending Promise with an
       interrupted scroll result. */
    function abort(box){
        const record=ongoing.get(box);
        if(!record)return;
        ongoing.delete(box);cancelFrame(record.frame);record.done(true);
    }
    /* The user-agent-defined smooth scroll: an ease-in-out curve over 150 ms plus 12 ms per
       square root of a pixel of distance, at most 600 ms, one step per animation frame. */
    function smoothDuration(dx,dy){return mathMin(600,150+12*mathSqrt(mathSqrt(dx*dx+dy*dy)));}
    function ease(t){return t<0.5?4*t*t*t:1-(2-2*t)*(2-2*t)*(2-2*t)/2;}
    /* "Perform a scroll" of box to (x, y); done(interrupted) settles the caller's Promise. */
    function perform(box,x,y,associated,behavior,done){
        abort(box);
        if(!smoothScroll(associated,behavior)){instant(box,x,y);done(false);return;}
        const max=limits(box),from=position(box);
        const toX=mathMax(0,mathMin(x,max[0])),toY=mathMax(0,mathMin(y,max[1]));
        if(toX===from[0] && toY===from[1]){done(false);return;}
        const record={done,frame:0,last:from,start:host.now(),duration:smoothDuration(toX-from[0],toY-from[1])};
        ongoing.set(box,record);
        const step=()=>{
            if(ongoing.get(box)!==record)return;
            const now=position(box);
            if(now[0]!==record.last[0] || now[1]!==record.last[1]){
                /* the user or another algorithm moved the box: this scroll is aborted */
                ongoing.delete(box);done(true);return;
            }
            const t=mathMin(1,(host.now()-record.start)/record.duration),e=ease(t);
            instant(box,t>=1?toX:from[0]+(toX-from[0])*e,t>=1?toY:from[1]+(toY-from[1])*e);
            record.last=position(box);
            if(t>=1){ongoing.delete(box);done(false);return;}
            record.frame=frame(step);
        };
        record.frame=frame(step);
    }
    function performPromise(box,x,y,associated,behavior){
        let settle;const promise=new PromiseImpl(resolve=>{settle=resolve;});
        perform(box,x,y,associated,behavior,interrupted=>settle({interrupted}));
        return promise;
    }
    /* Window scroll(): left/top or the current position, then the viewport's scroll with the
       document's root element as the associated element. scrollBy(): relative to scrollX/Y. */
    function windowScroll(args,relative){
        let x,y,behavior='auto';
        if(args.length>=2){x=scrollNumber(args[0]);y=scrollNumber(args[1]);}
        else{const options=scrollToOptions(args[0]);behavior=options.behavior;x=options.left;y=options.top;}
        if(relative){x=(x===undefined?0:x)+host.scroll(0);y=(y===undefined?0:y)+host.scroll(1);}
        else{if(x===undefined)x=host.scroll(0);if(y===undefined)y=host.scroll(1);}
        return performPromise(viewport,x,y,rootElement(null),behavior);
    }
    /* Element scroll(): conversions first (their getters may scroll, detach or adopt the
       element), then the element's current state: the root element (and the quirks-mode body
       that is not potentially scrollable) scrolls the viewport, an element without a scrolling
       box or overflow does nothing. */
    function elementScroll(node,args,relative){
        rawDom.get(node,'elementBrand');
        let x,y,behavior='auto';
        if(args.length>=2){x=scrollNumber(args[0]);y=scrollNumber(args[1]);}
        else{const options=scrollToOptions(args[0]);behavior=options.behavior;x=options.left;y=options.top;}
        if(relative || x===undefined)x=(relative?(x===undefined?0:x):0)+rawDom.geometry(node,'scrollLeft');
        if(relative || y===undefined)y=(relative?(y===undefined?0:y):0)+rawDom.geometry(node,'scrollTop');
        const kind=rawDom.geometry(node,'scrollBox');
        if(kind===2)return performPromise(viewport,x,y,rootElement(node),behavior);
        if(kind!==1)return resolvedResult(false);
        return performPromise(node,x,y,node,behavior);
    }
    function scrollLogical(value){
        value=string(value);
        if(value==='start')return 0;if(value==='end')return 1;
        if(value==='center')return 2;if(value==='nearest')return 3;
        throw new TypeErrorImpl('Invalid scroll alignment');
    }
    /* scrollIntoView(): every scrolling box from the innermost out, each with its own
       associated element (the viewport's is the root element); the Promise settles when all
       their scrolls have, interrupted when any was. */
    function scrollIntoView(node,arg){
        rawDom.get(node,'elementBrand');
        let behavior='auto',block=0,inline=3,nearest=false;
        if(arg!=null && (typeof arg==='object'||typeof arg==='function')){
            const b=arg.behavior;if(b!==undefined)behavior=scrollBehavior(b);
            const at=arg.block;if(at!==undefined)block=scrollLogical(at);
            const container=arg.container;
            if(container!==undefined){const c=string(container);
                if(c!=='all'&&c!=='nearest')throw new TypeErrorImpl('Invalid scroll container');nearest=c==='nearest';}
            const side=arg.inline;if(side!==undefined)inline=scrollLogical(side);
        }else if(arg!==undefined && arg!==null && !arg)block=1;
        // The native containing-block tree reflects rendered shadow/slot
        // distribution. Never invent a DOM parent or cross-frame traversal.
        const plan=rawDom.elementScrollIntoView(node,block,inline,nearest);
        if(plan.length<2)return resolvedResult(false);
        let settle,pending=1,interrupted=false;
        const promise=new PromiseImpl(resolve=>{settle=resolve;});
        const done=value=>{if(value)interrupted=true;if(!--pending)settle({interrupted});};
        for(let i=2;i+2<plan.length;i+=3){pending++;perform(plan[i],plan[i+1],plan[i+2],plan[i],behavior,done);}
        const now=position(viewport);
        if(plan[0]!==now[0] || plan[1]!==now[1] || ongoing.has(viewport)){
            pending++;perform(viewport,plan[0],plan[1],rootElement(node),behavior,done);
        }
        done(false);
        return promise;
    }
    /* Web IDL: an operation returning a Promise rejects it instead of throwing. */
    function promiseOperation(name,steps){
        const method={[name](){try{return steps(this,arguments);}catch(error){return rejected(error);}}}[name];
        return method;
    }
    function install(windowReceiver){
        for(const [name,relative] of [['scroll',false],['scrollTo',false],['scrollBy',true]]){
            define(Element.prototype,name,{configurable:true,enumerable:true,writable:true,
                value:promiseOperation(name,(self,args)=>elementScroll(self,args,relative))});
            define(globalThis,name,{configurable:true,enumerable:true,writable:true,
                value:promiseOperation(name,(self,args)=>{windowReceiver(self);return windowScroll(args,relative);})});
        }
        define(Element.prototype,'scrollIntoView',{configurable:true,enumerable:true,writable:true,
            value:promiseOperation('scrollIntoView',(self,args)=>scrollIntoView(self,args[0]))});
        for(const axis of ['scrollLeft','scrollTop'])define(Element.prototype,axis,{configurable:true,enumerable:true,
            get(){rawDom.get(this,'elementBrand');return rawDom.geometry(this,axis);},
            set(value){
                rawDom.get(this,'elementBrand');value=scrollNumber(value);
                const horizontal=axis==='scrollLeft',kind=rawDom.geometry(this,'scrollBox');
                if(kind===2){
                    /* the Window's scroll(x, y): behavior auto */
                    const now=position(viewport);
                    perform(viewport,horizontal?value:now[0],horizontal?now[1]:value,rootElement(this),'auto',()=>{});
                }else if(kind===1){
                    const now=position(this);
                    perform(this,horizontal?value:now[0],horizontal?now[1]:value,this,'auto',()=>{});
                }
            }});
        for(const size of ['scrollWidth','scrollHeight'])define(Element.prototype,size,{configurable:true,enumerable:true,
            get(){rawDom.get(this,'elementBrand');return rawDom.geometry(this,size);}});
    }
    return {install,queueElementScroll};
})();
