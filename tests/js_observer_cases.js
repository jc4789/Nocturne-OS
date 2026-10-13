globalThis.runObserverCases=async function(){
    let count=0;const ok=(v,n)=>{count++;check('observer-'+n,v);if(!v)throw Error(n);};
    const wait=()=>new Promise(r=>setTimeout(r,40));
    const until=async fn=>{for(let i=0;i<40 && !fn();i++)await wait();ok(fn(),'delivery');};
    const root=document.createElement('div');root.style.cssText='position:absolute;left:0;top:0;width:100px;height:100px;overflow:hidden';
    const box=document.createElement('div');box.style.cssText='position:absolute;left:10px;top:10px;width:20px;height:30px;padding:3px;border:2px solid';
    root.appendChild(box);document.body.appendChild(root);
    let resizes=[],ios=[],receiver;
    const ro=new ResizeObserver(function(es,o){resizes.push(...es);receiver=this;ok(o===ro,'resize-argument');});
    ro.observe(box);ok(resizes.length===0,'resize-not-synchronous');
    await until(()=>resizes.length>0);
    let e=resizes.at(-1);
    ok(receiver===ro && e instanceof ResizeObserverEntry && e.target===box,'resize-identity');
    ok(e.contentRect instanceof DOMRectReadOnly && e.contentRect.width===20 && e.contentRect.height===30,'content-box');
    ok(e.contentRect.x===3 && e.contentRect.y===3,'content-origin');
    ok(e.borderBoxSize[0].inlineSize===30 && e.borderBoxSize[0].blockSize===40,'border-box');
    ok(e.contentBoxSize[0] instanceof ResizeObserverSize && e.contentBoxSize[0].inlineSize===20,'size-interface');
    ok(e.devicePixelContentBoxSize[0].inlineSize===20,'device-pixels');
    const old=resizes.length;await wait();ok(resizes.length===old,'unchanged-no-notification');
    box.style.width='40px';await until(()=>resizes.length>old);ok(resizes.at(-1).contentRect.width===40,'size-change');
    box.style.display='none';await until(()=>resizes.at(-1).contentRect.width===0);ok(resizes.at(-1).contentRect.height===0,'hidden-zero');
    box.style.display='block';await until(()=>resizes.at(-1).contentRect.width===40);
    ro.disconnect();const stopped=resizes.length;box.style.width='20px';await wait();ok(resizes.length===stopped,'resize-disconnect');
    const io=new IntersectionObserver(function(es,o){ios.push(...es);receiver=this;ok(o===io,'intersection-argument');},{root,threshold:[0,.5,1]});
    io.observe(box);ok(ios.length===0,'intersection-not-synchronous');await until(()=>ios.length>0);
    e=ios.at(-1);ok(e instanceof IntersectionObserverEntry && e.target===box && receiver===io,'intersection-identity');
    ok(e.isIntersecting && e.intersectionRatio===1 && e.boundingClientRect.width===30,'fully-inside');
    ok(e.rootBounds.width===100 && e.rootBounds.height===100,'root-bounds');
    box.style.left='85px';await until(()=>ios.at(-1).intersectionRatio===.5);
    ok(ios.at(-1).isIntersecting && ios.at(-1).intersectionRect.width===15,'partly-clipped');
    box.style.left='101px';await until(()=>!ios.at(-1).isIntersecting);ok(ios.at(-1).intersectionRatio===0,'outside-not-visible');
    box.style.left='100px';await until(()=>ios.at(-1).isIntersecting);ok(ios.at(-1).intersectionRatio===0,'edge-adjacent');
    root.removeChild(box);await until(()=>!ios.at(-1).isIntersecting);ok(ios.at(-1).boundingClientRect.width===0,'detached');
    io.disconnect();ok(io.takeRecords().length===0,'take-records');
    ok(io.root===root && io.rootMargin==='0px 0px 0px 0px' && io.thresholds.join(',')==='0,0.5,1','options');
    const margin=new IntersectionObserver(()=>{},{rootMargin:'10% 2px'});
    ok(margin.rootMargin==='10% 2px 10% 2px','margin-expansion');margin.disconnect();
    let thrown=false;try{new IntersectionObserver(()=>{},{threshold:2});}catch(e){thrown=e instanceof RangeError;}ok(thrown,'threshold-range');
    thrown=false;try{new IntersectionObserver(()=>{},{rootMargin:'1em'});}catch(e){thrown=e.name==='SyntaxError';}ok(thrown,'margin-syntax');
    thrown=false;try{ro.observe(document);}catch(e){thrown=e instanceof TypeError;}ok(thrown,'target-brand');
    thrown=false;try{ro.observe(box,{box:'wrong'});}catch(e){thrown=e instanceof TypeError;}ok(thrown,'box-enum');
    root.appendChild(box);box.style.cssText='width:20px;height:30px';
    const frameOrder=[],frameTimes=[];let nestedFrame=false,frameResize=false,cancelFrame;
    const frameRO=new ResizeObserver(entries=>{
        frameOrder.push('resize');frameResize=true;
        ok(entries[0].contentRect.width===77,'render-layout-after-animation');
    });
    frameRO.observe(box);
    requestAnimationFrame(t=>{
        frameOrder.push('first');frameTimes.push(t);box.style.width='77px';
        Promise.resolve().then(()=>frameOrder.push('microtask'));
        cancelAnimationFrame(cancelFrame);
        requestAnimationFrame(next=>{nestedFrame=true;frameTimes.push(next);});
    });
    requestAnimationFrame(t=>{
        frameOrder.push('second');frameTimes.push(t);
        ok(!nestedFrame,'new-animation-deferred');
    });
    cancelFrame=requestAnimationFrame(()=>frameOrder.push('cancelled'));
    await until(()=>nestedFrame&&frameResize);
    ok(frameOrder.slice(0,4).join(',')==='first,microtask,second,resize','animation-microtask-resize-order');
    ok(frameTimes[0]===frameTimes[1]&&frameTimes[2]>frameTimes[1],'one-timestamp-per-rendering-opportunity');
    ok(!frameOrder.includes('cancelled'),'animation-cancellation-before-dispatch');
    frameRO.disconnect();root.remove();return count;
};

/* Focused native regression for lazy-loading targets using inline fragments
   and slotted containing blocks. It is independently selectable in jstest. */
async function runIntersectionInlineCases(){
    let count=0;
    const ok=(v,n)=>{count++;check('intersection-inline-'+n,!!v);if(!v)throw new Error(n);};
    const until=async fn=>{const end=performance.now()+3000;while(!fn()){if(performance.now()>end)throw new Error('observer timeout');await new Promise(r=>setTimeout(r,1));}};
    const sameRect=(actual,expected)=>['x','y','width','height'].every(k=>Math.abs(actual[k]-expected[k])<.0001);
    const port=document.createElement('div');
    port.style.cssText='position:absolute;left:200px;top:120px;width:120px;height:100px;overflow:hidden';
    const inline=document.createElement('span');inline.textContent='inline fragment';inline.style.fontSize='16px';
    port.appendChild(inline);document.body.appendChild(port);
    const seen=[];const io=new IntersectionObserver(entries=>seen.push(...entries),{root:port});
    io.observe(inline);ok(seen.length===0,'asynchronous');await until(()=>seen.length);
    let rect=inline.getBoundingClientRect(),entry=seen.at(-1);
    ok(rect.x>=200&&rect.y>=120&&rect.width>0&&rect.height>0,'real-fragment-position');
    ok(sameRect(entry.boundingClientRect,rect),'same-cssom-view-rectangle');
    ok(entry.isIntersecting&&entry.intersectionRatio>0,'inline-inside-explicit-root');
    inline.style.display='none';await until(()=>!seen.at(-1).isIntersecting);
    ok(seen.at(-1).boundingClientRect.width===0&&seen.at(-1).boundingClientRect.height===0,'hidden-not-visible');
    io.disconnect();port.remove();

    const host=document.createElement('div');
    host.style.cssText='position:absolute;left:350px;top:120px;width:120px;height:100px';
    const light=document.createElement('span');light.textContent='slotted fragment';host.appendChild(light);
    const shadow=host.attachShadow({mode:'open'});
    shadow.innerHTML='<div id="port" style="width:120px;height:100px;overflow:hidden"><slot></slot></div>';
    document.body.appendChild(host);const root=shadow.getElementById('port');
    const slotted=[];const sio=new IntersectionObserver(entries=>slotted.push(...entries),{root});
    sio.observe(light);await until(()=>slotted.length);entry=slotted.at(-1);
    ok(light.parentNode===host&&light.assignedSlot!==null,'logical-and-assigned-parents');
    ok(entry.isIntersecting,'actual-shadow-containing-block');
    ok(sameRect(entry.boundingClientRect,light.getBoundingClientRect()),'slotted-cssom-view-rectangle');
    const fixed=document.createElement('span');fixed.textContent='fixed';
    fixed.style.cssText='display:block;position:fixed;left:350px;top:120px;width:10px;height:10px';host.appendChild(fixed);
    sio.observe(fixed);await until(()=>slotted.some(e=>e.target===fixed));
    ok(!slotted.find(e=>e.target===fixed).isIntersecting,'fixed-not-in-explicit-containing-chain');
    const zero=document.createElement('span');zero.style.cssText='display:block;width:0;height:0';host.appendChild(zero);
    sio.observe(zero);await until(()=>slotted.some(e=>e.target===zero));entry=slotted.find(e=>e.target===zero);
    ok(entry.isIntersecting&&entry.intersectionRatio===1,'zero-area-inside');
    sio.disconnect();host.remove();return count;
}

/* Prototype feature detection must reach the same branded records as native
   notifications, not a presence-only shim or mutable own-property snapshot. */
async function runIntersectionEntryCases(){
    let count=0;
    const ok=(v,n)=>{count++;check('intersection-entry-'+n,!!v);if(!v)throw new Error(n);};
    const names=['time','rootBounds','boundingClientRect','intersectionRect','isIntersecting','intersectionRatio','target'];
    const descriptors=names.map(n=>Object.getOwnPropertyDescriptor(IntersectionObserverEntry.prototype,n));
    ok(descriptors.every(d=>d&&typeof d.get==='function'&&!d.set&&d.enumerable&&d.configurable)&&
        !('isVisible' in IntersectionObserverEntry.prototype),'prototype-readonly-accessors-no-unimplemented-v2');
    const getRatio=Object.getOwnPropertyDescriptor(IntersectionObserverEntry.prototype,'intersectionRatio').get;
    let rejected=false;try{getRatio.call({});}catch(e){rejected=e instanceof TypeError;}
    ok(rejected,'getter-brand');
    const target=document.createElement('div');
    target.style.cssText='position:absolute;left:240px;top:100px;width:20px;height:10px';
    document.body.appendChild(target);
    const source=new DOMRect(1.25,2.5,20,10);
    const init={time:12.5,rootBounds:null,boundingClientRect:source,intersectionRect:source,
        isIntersecting:true,intersectionRatio:.75,target};
    const copy=new IntersectionObserverEntry(init);
    ok(copy instanceof IntersectionObserverEntry&&copy.target===target&&copy.time===12.5&&
        copy.rootBounds===null&&copy.isIntersecting&&copy.intersectionRatio===.75,'constructor-values');
    source.x=500;
    ok(copy.boundingClientRect.x===1.25&&copy.intersectionRect.x===1.25&&
        copy.boundingClientRect instanceof DOMRectReadOnly,'constructor-rectangle-copy');
    rejected=false;try{new IntersectionObserverEntry({...init,time:Infinity});}catch(e){rejected=e instanceof TypeError;}
    ok(rejected,'finite-double-conversion');
    rejected=false;try{new IntersectionObserverEntry({...init,target:undefined});}catch(e){rejected=e instanceof TypeError;}
    ok(rejected,'required-member');
    rejected=false;try{new IntersectionObserverEntry({...init,target:{}});}catch(e){rejected=e instanceof TypeError;}
    ok(rejected,'element-conversion');
    const entries=[];const observer=new IntersectionObserver(values=>entries.push(...values));
    try{
        observer.observe(target);
        const end=performance.now()+3000;
        while(!entries.length){if(performance.now()>end)throw new Error('observer timeout');await new Promise(r=>setTimeout(r,1));}
        const entry=entries[0];
        ok(entry instanceof IntersectionObserverEntry&&getRatio.call(entry)===1&&entry.target===target,'native-record-getter');
        ok(!Object.hasOwn(entry,'intersectionRatio')&&!Object.hasOwn(entry,'target')&&
            Object.prototype.toString.call(entry)==='[object IntersectionObserverEntry]','native-prototype-shape');
        target.style.width='10px';target.getBoundingClientRect();
        ok(entry.boundingClientRect.width===20&&entry.intersectionRect.width===20,'native-immutable-snapshot');
    }finally{observer.disconnect();target.remove();}
    return count;
}
