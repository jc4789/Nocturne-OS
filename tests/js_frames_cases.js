/* Supporting web_live regression; public-site rendering remains acceptance. */
async function runFrameCases(){
    let count=0,loadStep=0;const equal=(actual,expected)=>{count++;if(!Object.is(actual,expected))throw new Error('Frame check '+count+': '+String(actual)+' != '+String(expected));};
    const throws=(call,name)=>{count++;try{call();}catch(error){if(error.name===name)return;throw error;}throw new Error('Frame check '+count+': missing '+name);};
    const loaded=(frame,change)=>new Promise((resolve,reject)=>{
        const step=++loadStep;
        let timeout=setTimeout(()=>reject(new Error('Frame load '+step+' did not arrive after '+count+' checks; src='+frame.getAttribute('src')+' srcdoc='+(frame.hasAttribute('srcdoc')?'present':'absent'))),5000);
        frame.onload=()=>{clearTimeout(timeout);frame.onload=null;console.log('OK frame-load-step-'+step);resolve();};change();
    });
    const frame=document.createElement('iframe');
    equal(frame.contentWindow,null);equal(frame.contentDocument,null);
    frame.style.cssText='position:absolute;left:10px;top:40px;width:120px;height:70px;border:0';document.body.appendChild(frame);
    const window=frame.contentWindow,child=frame.contentDocument;
    equal(child!==document,true);equal(child.URL,'about:blank');equal(child.body!==null,true);
    equal(child.createElement('span').ownerDocument,child);equal(window.document,child);equal(window===frame.contentWindow,true);
    equal(child.defaultView,window);const retainedView=child.defaultView;
    count++;try{Object.getOwnPropertyDescriptor(window.Document.prototype,'defaultView').get.call(child.body);throw new Error('defaultView accepted an Element');}catch(error){if(error.name!=='TypeError')throw error;}
    equal(window.parent.document,document);equal(window.top.document,document);equal(window.frameElement,frame);
    equal(window.Array===Array,false);equal(typeof window.Array,'function');
    window.parentProbe=23;equal(window.parentProbe,23);equal(typeof globalThis.parentProbe,'undefined');
    await loaded(frame,()=>{
        equal(child.open(),child);
        child.write('<!doctype html><body style="margin:0;background:#123456" onload="document._loaded=1"><div id="inside">child</div><scr'+'ipt>globalThis.childValue=41;const parserRoot=document.documentElement;document.open();document._parserOpenIgnored=document.documentElement===parserRoot;customElements.define("frame-open-probe",class extends HTMLElement{constructor(){super();try{document.open();}catch(e){document._ceOpen=e.name;}}});document.createElement("frame-open-probe");</scr'+'ipt>');
        equal(child.getElementById('inside')!==null,true);
        equal(window.childValue,41);child.close();
    });
    equal(child._loaded,1);equal(child._parserOpenIgnored,true);equal(child._ceOpen,'InvalidStateError');equal(document.getElementById('inside'),null);
    let cancelledIdle=0;
    const cancelledHandle=window.requestIdleCallback(()=>cancelledIdle++,{timeout:1});equal(typeof cancelledHandle,'number');
    window.cancelIdleCallback(cancelledHandle);
    await new Promise((resolve,reject)=>{
        const timeout=setTimeout(()=>reject(new Error('WindowProxy idle callback missing')),1000);
        child.defaultView.requestIdleCallback(deadline=>{
            try{equal(deadline instanceof window.IdleDeadline,true);equal(typeof deadline.timeRemaining(),'number');equal(typeof deadline.didTimeout,'boolean');clearTimeout(timeout);resolve();}
            catch(error){clearTimeout(timeout);reject(error);}
        },{timeout:1});
    });
    equal(cancelledIdle,0);
    // Instrumentation may replace Window methods and call the saved original.
    // The original delegate must capture the function, not re-read its name.
    const heldTimer=window.setTimeout,heldIdle=window.requestIdleCallback;
    equal(window.setTimeout,heldTimer);equal(window.requestIdleCallback,heldIdle);
    equal(window.cancelIdleCallback,window.cancelIdleCallback);
    let timerWrappers=0,idleWrappers=0,timerRuns=0,idleRuns=0;
    function wrappedTimer(){timerWrappers++;return heldTimer.apply(this,arguments);}
    function wrappedIdle(){idleWrappers++;return heldIdle.apply(this,arguments);}
    try{
        window.setTimeout=wrappedTimer;window.requestIdleCallback=wrappedIdle;
        equal(window.setTimeout,wrappedTimer);equal(window.setTimeout,wrappedTimer);
        equal(window.requestIdleCallback,wrappedIdle);
        await new Promise((resolve,reject)=>{
            const timeout=setTimeout(()=>reject(new Error('Wrapped Window timer missing')),1000);
            window.setTimeout(()=>{timerRuns++;clearTimeout(timeout);resolve();},0);
        });
        await new Promise((resolve,reject)=>{
            const timeout=setTimeout(()=>reject(new Error('Wrapped Window idle callback missing')),1000);
            window.requestIdleCallback(()=>{idleRuns++;clearTimeout(timeout);resolve();},{timeout:1});
        });
        equal(timerWrappers,1);equal(timerRuns,1);equal(idleWrappers,1);equal(idleRuns,1);
        window.setTimeout=17;window.requestIdleCallback=null;
        equal(window.setTimeout,17);equal(window.requestIdleCallback,null);
    }finally{window.setTimeout=heldTimer;window.requestIdleCallback=heldIdle;}
    equal(window.setTimeout,heldTimer);equal(window.requestIdleCallback,heldIdle);
    console.log('OK frame-window-method-overrides');
    const oldNode=child.createElement('span');oldNode.textContent='retained';child.body.appendChild(oldNode);
    const oldHost=child.createElement('div'),oldShadow=oldHost.attachShadow({mode:'closed'}),oldShadowNode=child.createElement('span');
    oldShadow.appendChild(oldShadowNode);child.body.appendChild(oldHost);
    let oldEvents=0;oldNode.addEventListener('probe',()=>oldEvents++);oldShadowNode.addEventListener('probe',()=>oldEvents++);
    oldNode.setAttribute('onclick','document._oldInline=1');oldNode.onclick=()=>oldEvents++;
    let windowLoad=0;window.addEventListener('load',()=>windowLoad++);
    await loaded(frame,()=>{child.open();child.write('<body style="margin:0;background:#123456">written again');child.close();});
    equal(windowLoad,0);equal(frame.contentDocument,child);equal(oldNode.onclick,null);
    oldNode.dispatchEvent(new window.Event('probe'));oldShadowNode.dispatchEvent(new window.Event('probe'));oldNode.dispatchEvent(new window.Event('click'));
    equal(oldEvents,0);equal(child._oldInline,undefined);equal(oldNode.getAttribute('onclick'),'document._oldInline=1');
    const channelName='frame-realms',parentChannel=new BroadcastChannel(channelName),childChannel=new window.BroadcastChannel(channelName);
    let crossMessages=0;parentChannel.onmessage=()=>crossMessages++;
    await new Promise((resolve,reject)=>{
        const timeout=setTimeout(()=>reject(new Error('Cross-realm BroadcastChannel delivery missing')),1000);
        childChannel.onmessage=event=>{
            try{equal(event.data.array instanceof window.Array,true);equal(event.data.map instanceof window.Map,true);equal(event.data.array instanceof Array,false);
                childChannel.postMessage(event.data);}catch(e){clearTimeout(timeout);reject(e);}
        };
        parentChannel.onmessage=event=>{crossMessages++;try{equal(event.data.array instanceof Array,true);equal(event.data.map instanceof Map,true);clearTimeout(timeout);resolve();}catch(e){clearTimeout(timeout);reject(e);}};
        parentChannel.postMessage({array:[1,2],map:new Map([['answer',42]])});
    });
    childChannel.close();parentChannel.onmessage=()=>crossMessages++;
    await loaded(frame,()=>{frame.srcdoc='<!doctype html><body><div id="srcdoc-child">srcdoc</div><scr'+'ipt>globalThis.srcdocValue=42</scr'+'ipt>';});
    equal(frame.contentWindow,window);equal(window.document!==child,true);equal(oldNode.textContent,'retained');
    equal(window.document.defaultView,window);equal(retainedView,window);
    equal(window.document.URL,'about:srcdoc');equal(window.srcdocValue,42);
    equal(window.document.baseURI,document.baseURI);equal(document.getElementById('srcdoc-child'),null);
    throws(()=>heldTimer(()=>{},0),'TypeError');throws(()=>heldIdle(()=>{}, {timeout:1}),'TypeError');
    const sameOriginTimer=window.setTimeout;equal(window.setTimeout,sameOriginTimer);equal(sameOriginTimer===heldTimer,false);
    await loaded(frame,()=>{frame.removeAttribute('srcdoc');frame.src='http://other.test/frame-cross.html';});
    equal(frame.contentDocument,null);equal(frame.contentWindow,window);
    count++;try{void window.document;throw new Error('Cross-origin document leaked');}catch(error){if(error.name!=='SecurityError')throw error;}
    count++;try{window.stolen=1;throw new Error('Cross-origin Window write leaked');}catch(error){if(error.name!=='SecurityError')throw error;}
    count++;try{void window.requestIdleCallback;throw new Error('Cross-origin idle method lookup leaked');}catch(error){if(error.name!=='SecurityError')throw error;}
    count++;try{void retainedView.document;throw new Error('Retained defaultView bypassed WindowProxy guard');}catch(error){if(error.name!=='SecurityError')throw error;}
    throws(()=>sameOriginTimer(()=>{},0),'SecurityError');throws(()=>heldIdle(()=>{}, {timeout:1}),'SecurityError');
    console.log('OK frame-saved-method-navigation-guard');
    equal(window.closed,false);equal(window.parent.document,document);
    await new Promise(resolve=>setTimeout(resolve,20));equal(crossMessages,1);parentChannel.close();
    await loaded(frame,()=>{frame.src='about:blank';});
    equal(window.document.URL,'about:blank');equal(window.document!==child,true);
    const activeTimer=window.setTimeout;
    const before=document.createElement('iframe');document.body.insertBefore(before,frame);void before.contentWindow;
    equal(globalThis.frames[0],before.contentWindow);equal(globalThis.frames[1],frame.contentWindow);equal(globalThis.length,2);
    before.remove();equal(before.contentWindow,null);
    frame.remove();equal(frame.contentWindow,null);equal(window.closed,true);equal(oldNode.textContent,'retained');
    throws(()=>activeTimer(()=>{},0),'TypeError');
    document.body.appendChild(frame);const replacement=frame.contentWindow;
    equal(replacement===window,false);equal(window.closed,true);equal(replacement.closed,false);
    throws(()=>activeTimer(()=>{},0),'TypeError');console.log('OK frame-saved-method-detach-guard');frame.remove();
    // The detached context must not be able to keep timers alive.
    const detached=document.createElement('iframe');document.body.appendChild(detached);
    detached.contentWindow.setTimeout(()=>{throw new Error('Retired frame timer ran');},20);detached.remove();
    await new Promise(resolve=>setTimeout(resolve,35));
    return count;
}
