/* Contract regressions executed by Nocturne's native web_live/QuickJS host.
 * These do not substitute for the subsequent multi-site browser run. */
async function runShadowCases(){
    let checks=0;
    const ok=(value,name)=>{checks++;check('shadow-'+name,!!value);if(!value)throw new Error(name);};
    const eq=(a,b,name)=>ok(Object.is(a,b),name);
    const throws=(fn,name,type=DOMException)=>{let error;try{fn();}catch(e){error=e;}ok(error instanceof type && (type!==DOMException || error.name===name),'throws-'+name);};
    const tick=()=>new Promise(resolve=>setTimeout(resolve,0));
    const make=(name,parent)=>{const n=document.createElement(name);if(parent)parent.appendChild(n);return n;};
    const host=make('div',document.body),light=make('span',host);light.id='light';light.textContent='light text';
    const root=host.attachShadow({mode:'open'});
    eq(root,host.shadowRoot,'same-root');ok(root instanceof ShadowRoot,'shadow-brand');ok(root instanceof DocumentFragment,'fragment-brand');
    ok(root instanceof Node,'node-brand');eq(Object.getPrototypeOf(root),ShadowRoot.prototype,'prototype');
    eq(Object.prototype.toString.call(root),'[object ShadowRoot]','tag');throws(()=>new ShadowRoot(),'constructor',TypeError);
    throws(()=>Object.getOwnPropertyDescriptor(ShadowRoot.prototype,'host').get.call(document.createDocumentFragment()),'brand',TypeError);
    eq(root.host,host,'host');eq(root.mode,'open','mode');eq(root.delegatesFocus,false,'focus-default');eq(root.clonable,false,'clone-default');
    eq(root.serializable,false,'serialize-default');eq(root.slotAssignment,'named','assignment-default');
    eq(root.nodeType,11,'node-type');eq(root.nodeName,'#document-fragment','node-name');eq(root.parentNode,null,'root-parent');
    eq(root.ownerDocument,document,'root-owner');eq(root.isConnected,true,'root-connected');eq(root.getRootNode(),root,'root-self');
    eq(root.getRootNode({composed:true}),document,'composed-root');eq(host.childNodes.length,1,'light-children');
    eq(host.contains(root),false,'contains-boundary');eq(root.contains(light),false,'light-not-shadow');eq(host.textContent,'light text','light-text');
    root.innerHTML='<div id="inside"><slot></slot><slot name="x"></slot></div>';
    const inside=root.getElementById('inside'),normal=root.querySelector('slot'),named=root.querySelector('[name=x]');
    ok(inside && normal && named,'root-query');eq(document.getElementById('inside'),null,'id-isolation');
    eq(host.querySelector('#inside'),null,'query-isolation');eq(inside.getRootNode(),root,'inner-root');eq(inside.getRootNode({composed:true}),document,'inner-composed-root');
    eq(inside.parentNode,root,'ordinary-parent');eq(inside.parentElement,null,'root-not-element');eq(inside.isConnected,true,'inner-connected');
    eq(root.textContent,'','shadow-text-excludes-slotted-light');ok(root.innerHTML.includes('<slot'),'serialize-root');ok(!host.outerHTML.includes('slot'),'serialize-host-excludes-root');
    ok(normal instanceof HTMLSlotElement,'slot-brand');eq(Object.prototype.toString.call(normal),'[object HTMLSlotElement]','slot-tag');
    throws(()=>new HTMLSlotElement(),'slot-constructor',TypeError);throws(()=>normal.assign(document),'slot-type',TypeError);
    eq(normal.name,'','slot-name-default');eq(light.assignedSlot,normal,'assigned-slot');eq(normal.assignedNodes()[0],light,'assigned-nodes');
    eq(normal.assignedElements()[0],light,'assigned-elements');const saved=normal.assignedNodes();saved.length=0;eq(normal.assignedNodes().length,1,'assigned-snapshot');
    const text=document.createTextNode('text'),comment=document.createComment('comment');host.append(text,comment);
    eq(text.assignedSlot,normal,'text-assigned');eq(normal.assignedNodes().length,2,'comment-not-assigned');eq(normal.assignedElements().length,1,'elements-filter');
    light.slot='x';eq(light.getAttribute('slot'),'x','slot-reflection');eq(light.assignedSlot,named,'named-assignment');eq(normal.assignedNodes()[0],text,'default-after-name');
    named.name='changed';eq(light.assignedSlot,null,'name-reassignment');eq(named.getAttribute('name'),'changed','name-reflection');named.name='x';
    const duplicate=make('slot',inside);duplicate.name='x';eq(duplicate.assignedNodes().length,0,'first-slot-wins');
    named.remove();eq(light.assignedSlot,duplicate,'removed-first-slot');inside.appendChild(named);
    duplicate.remove();eq(light.assignedSlot,named,'restored-slot');
    const fallback=make('b',named);fallback.textContent='fallback';light.slot='missing';
    eq(named.assignedNodes().length,0,'fallback-not-raw-assigned');eq(named.assignedNodes({flatten:true})[0],fallback,'fallback-flatten');
    const innerSlot=make('slot',named),fallbackText=document.createTextNode('nested');innerSlot.appendChild(fallbackText);fallback.remove();
    eq(named.assignedNodes({flatten:true})[0],fallbackText,'nested-fallback-flatten');light.slot='x';
    eq(named.assignedNodes({flatten:true})[0],light,'assigned-replaces-fallback');
    host.remove();eq(root.isConnected,false,'detached-root');eq(inside.isConnected,false,'detached-inner');
    eq(inside.getRootNode({composed:true}),host,'detached-composed');document.body.appendChild(host);
    throws(()=>host.attachShadow({mode:'open'}),'NotSupportedError');throws(()=>make('input').attachShadow({mode:'open'}),'NotSupportedError');
    throws(()=>make('div').attachShadow({mode:'invalid'}),'mode',TypeError);throws(()=>make('div').attachShadow({}),'missing-mode',TypeError);
    throws(()=>make('div').attachShadow({mode:'open',slotAssignment:'invalid'}),'assignment',TypeError);
    throws(()=>document.createElementNS('http://www.w3.org/2000/svg','g').attachShadow({mode:'open'}),'NotSupportedError');
    const closedHost=make('section',document.body),closed=closedHost.attachShadow({mode:'closed'}),closedSlot=make('slot',closed),closedLight=make('em',closedHost);
    eq(closedHost.shadowRoot,null,'closed-not-exposed');eq(closed.mode,'closed','closed-mode');eq(closedLight.assignedSlot,null,'closed-slot-not-exposed');
    eq(closedSlot.assignedNodes()[0],closedLight,'closed-assignment-real');eq(closed.getRootNode({composed:true}),document,'closed-connected-root');
    const manualHost=make('div',document.body),manual=manualHost.attachShadow({mode:'open',slotAssignment:'manual'}),manualSlot=make('slot',manual);
    const one=make('b',manualHost),two=make('i',manualHost);eq(manualSlot.assignedNodes().length,0,'manual-empty');
    manualSlot.assign(two,one,two);eq(manualSlot.assignedNodes().length,2,'manual-dedup');eq(manualSlot.assignedNodes()[0],two,'manual-order');
    eq(one.assignedSlot,manualSlot,'manual-assigned-slot');manualSlot.assign();eq(one.assignedSlot,null,'manual-clear');
    const pendingSlot=make('slot');pendingSlot.assign(one);manual.appendChild(pendingSlot);eq(pendingSlot.assignedNodes()[0],one,'manual-assignment-before-insertion');
    const autoSlot=make('slot',inside);autoSlot.assign(two);manual.appendChild(autoSlot);eq(two.assignedSlot,autoSlot,'auto-assignment-preserved-until-manual');
    throws(()=>root.cloneNode(true),'NotSupportedError');throws(()=>document.importNode(root,true),'NotSupportedError');throws(()=>document.adoptNode(root),'HierarchyRequestError');
    eq(host.cloneNode(true).shadowRoot,null,'nonclonable');
    const cloneHost=make('div'),cloneRoot=cloneHost.attachShadow({mode:'open',clonable:true,serializable:true});cloneRoot.innerHTML='<b>clone</b>';
    const clone=cloneHost.cloneNode(false);ok(clone.shadowRoot && clone.shadowRoot!==cloneRoot,'clonable-root');eq(clone.shadowRoot.innerHTML,'<b>clone</b>','shallow-host-deep-root');
    eq(clone.shadowRoot.host,clone,'clone-host');eq(clone.shadowRoot.clonable,true,'clone-flag');eq(clone.shadowRoot.serializable,true,'serializable-flag');
    const drain=make('div');eq(drain.appendChild(clone.shadowRoot),clone.shadowRoot,'fragment-insertion-return');eq(drain.textContent,'clone','fragment-drain');eq(clone.shadowRoot.childNodes.length,0,'root-still-attached-empty');
    let cycle=false;try{root.appendChild(host);}catch(_){cycle=true;}ok(cycle,'host-cycle-rejected');eq(root.host,host,'cycle-no-damage');
    // Event paths and retargeting, including composed non-bubbling events.
    const target=make('button',inside);let records=[];
    function listen(node,label){for(const capture of [true,false])node.addEventListener('shadow-test',e=>records.push({label,capture,target:e.target,phase:e.eventPhase,path:e.composedPath()}),capture);}
    listen(target,'target');listen(root,'root');listen(host,'host');listen(document,'document');
    const composed=new Event('shadow-test',{bubbles:true,composed:true});target.dispatchEvent(composed);
    eq(records.length,8,'composed-callback-count');eq(records[0].label,'document','capture-order');eq(records[0].target,host,'document-target-retarget');
    eq(records[1].phase,2,'host-capture-at-target');eq(records[2].target,target,'inner-target');eq(records[6].phase,2,'host-bubble-at-target');
    ok(records[0].path.includes(target) && records[0].path.includes(root),'open-composed-path');eq(composed.target,host,'post-dispatch-retarget');
    eq(composed.composedPath().length,0,'post-dispatch-path-cleared');eq(composed.currentTarget,null,'post-current-null');
    records=[];const local=new Event('shadow-test',{bubbles:true});target.dispatchEvent(local);
    eq(records.length,4,'noncomposed-stops-at-root');ok(records.every(r=>r.label!=='host' && r.label!=='document'),'noncomposed-isolation');eq(local.target,null,'noncomposed-post-target-cleared');
    records=[];target.dispatchEvent(new Event('shadow-test',{composed:true}));
    eq(records.filter(r=>!r.capture).map(r=>r.label).join(','),'target,host','nonbubbling-host-at-target');
    // A capture stop keeps listeners in that invocation running, not the
    // following noncapture invocation. Hosts use the same AT_TARGET boundary.
    function propagationCase(stopNode,dispatchTarget,parent,mode,label,expected){
        const type='shadow-stop-'+label,sequence=[],listeners=[];
        function on(node,capture,fn){node.addEventListener(type,fn,capture);listeners.push([node,capture,fn]);}
        on(stopNode,true,e=>{
            sequence.push('capture-first:'+e.eventPhase);
            if(mode==='propagation')e.stopPropagation();
            else if(mode==='immediate')e.stopImmediatePropagation();
        });
        on(stopNode,true,e=>sequence.push('capture-second:'+e.eventPhase));
        if(dispatchTarget!==stopNode)on(dispatchTarget,true,e=>sequence.push('inner:'+e.eventPhase));
        on(stopNode,false,e=>sequence.push('noncapture:'+e.eventPhase));
        on(parent,false,e=>sequence.push('parent:'+e.eventPhase));
        try{dispatchTarget.dispatchEvent(new Event(type,{bubbles:true,composed:true}));}
        finally{for(const [node,capture,fn] of listeners)node.removeEventListener(type,fn,capture);}
        eq(sequence.join(','),expected,label);
    }
    const ordinaryParent=make('div',document.body),ordinaryTarget=make('button',ordinaryParent);
    propagationCase(ordinaryTarget,ordinaryTarget,ordinaryParent,'propagation','ordinary-capture-stop',
        'capture-first:2,capture-second:2');
    propagationCase(ordinaryTarget,ordinaryTarget,ordinaryParent,'immediate','ordinary-capture-immediate',
        'capture-first:2');
    propagationCase(ordinaryTarget,ordinaryTarget,ordinaryParent,'none','ordinary-no-stop',
        'capture-first:2,capture-second:2,noncapture:2,parent:3');
    ordinaryParent.remove();
    propagationCase(target,target,root,'propagation','shadow-target-capture-stop',
        'capture-first:2,capture-second:2');
    propagationCase(target,target,root,'immediate','shadow-target-capture-immediate',
        'capture-first:2');
    propagationCase(host,target,document,'propagation','host-capture-stop',
        'capture-first:2,capture-second:2');
    propagationCase(host,target,document,'immediate','host-capture-immediate',
        'capture-first:2');
    propagationCase(host,target,document,'none','host-no-stop',
        'capture-first:2,capture-second:2,inner:2,noncapture:2,parent:3');
    let lightPath;document.addEventListener('slotted-event',e=>lightPath=e.composedPath(),{once:true});light.dispatchEvent(new Event('slotted-event',{bubbles:true}));
    ok(lightPath.includes(named) && lightPath.includes(root) && lightPath.includes(host),'noncomposed-light-crosses-slot');eq(lightPath[0],light,'light-target-unretargeted');
    const secret=make('button',closed);let publicPath,privatePath,publicTarget;
    closed.addEventListener('secret-event',e=>privatePath=e.composedPath(),{once:true});
    document.addEventListener('secret-event',e=>{publicPath=e.composedPath();publicTarget=e.target;},{once:true});
    secret.dispatchEvent(new Event('secret-event',{bubbles:true,composed:true}));
    eq(publicTarget,closedHost,'closed-retarget');ok(!publicPath.includes(secret) && !publicPath.includes(closed),'closed-path-hidden');
    ok(privatePath.includes(secret) && privatePath.includes(closed),'closed-listener-sees-internals');
    let closedLightPath;document.addEventListener('closed-light',e=>closedLightPath=e.composedPath(),{once:true});
    closedLight.dispatchEvent(new Event('closed-light',{bubbles:true,composed:true}));
    ok(closedLightPath.includes(closedLight) && !closedLightPath.includes(closedSlot) && !closedLightPath.includes(closed),'closed-slot-hides-only-internals');
    const nestedHost=make('div',closed),nested=nestedHost.attachShadow({mode:'open'}),nestedTarget=make('b',nested);let nestedPublic,nestedPrivate;
    nested.addEventListener('nested-test',e=>nestedPrivate=e.composedPath(),{once:true});
    document.addEventListener('nested-test',e=>nestedPublic=e.composedPath(),{once:true});nestedTarget.dispatchEvent(new Event('nested-test',{bubbles:true,composed:true}));
    ok(!nestedPublic.includes(nestedHost) && !nestedPublic.includes(nestedTarget),'nested-closed-outer-hides-open-inner');ok(nestedPrivate.includes(closed),'nested-listener-sees-closed-outer');
    let relatedOuter=0,relatedInner=0;const peer=make('button',inside);
    host.addEventListener('mouseout',()=>relatedOuter++);root.addEventListener('mouseout',e=>{if(e.target===target && e.relatedTarget===peer)relatedInner++;});
    target.dispatchEvent(new MouseEvent('mouseout',{bubbles:true,composed:true,relatedTarget:peer}));
    eq(relatedOuter,0,'related-retarget-trims-host');eq(relatedInner,1,'related-inside-dispatched');
    let sameRelated=0;root.addEventListener('mouseover',()=>sameRelated++,{once:true});
    target.dispatchEvent(new MouseEvent('mouseover',{bubbles:true,composed:true,relatedTarget:target}));eq(sameRelated,1,'self-related-preserves-inner-ancestors');
    let frozen=false;root.addEventListener('frozen',()=>target.remove(),{capture:true,once:true});
    document.addEventListener('frozen',e=>{frozen=e.target===host && e.composedPath().includes(root);},{once:true});
    target.dispatchEvent(new Event('frozen',{bubbles:true,composed:true}));ok(frozen,'path-frozen-through-removal');inside.appendChild(target);
    Object.defineProperty(target,'parentNode',{configurable:true,value:null});let spoof=false;
    document.addEventListener('spoof',e=>{spoof=e.target===host;},{once:true});target.dispatchEvent(new Event('spoof',{bubbles:true,composed:true}));
    ok(spoof,'native-ancestry-not-page-getter');delete target.parentNode;
    // Slot signals use the same microtask as MutationObserver, not polling.
    await tick();let slotEvents=0,rootEvents=0,documentEvents=0,order=[];
    normal.addEventListener('slotchange',e=>{slotEvents++;order.push('slot');ok(e.isTrusted && e.bubbles && !e.composed,'slot-event-flags');});
    root.onslotchange=()=>rootEvents++;
    const outside=()=>documentEvents++;document.addEventListener('slotchange',outside);
    const observer=new MutationObserver(()=>order.push('observer'));observer.observe(host,{childList:true});
    const extra=make('a',host);extra.remove();host.appendChild(extra);eq(slotEvents,0,'slotchange-asynchronous');await tick();
    eq(slotEvents,1,'slotchange-coalesced');eq(rootEvents,1,'root-slot-handler');eq(documentEvents,0,'slotchange-boundary');eq(order.join(','),'observer,slot','observer-before-slot');
    observer.disconnect();document.removeEventListener('slotchange',outside);root.onslotchange=null;
    let mutations=0,innerMutations=0;const outerMO=new MutationObserver(r=>mutations+=r.length),innerMO=new MutationObserver(r=>innerMutations+=r.length);
    outerMO.observe(host,{subtree:true,attributes:true,childList:true});innerMO.observe(root,{subtree:true,attributes:true,childList:true});
    inside.setAttribute('data-shadow','yes');make('i',inside);await tick();eq(mutations,0,'observer-does-not-pierce-shadow');eq(innerMutations,2,'observer-root-subtree');outerMO.disconnect();innerMO.disconnect();
    // Real shadow scripts, custom elements, ownership, focus, not empty shims.
    const script=make('script');script.textContent='globalThis.shadowScriptExecuted=42';root.appendChild(script);
    eq(globalThis.shadowScriptExecuted,42,'script-in-connected-shadow');delete globalThis.shadowScriptExecuted;
    const lifecycle=[];
    class ShadowChild extends HTMLElement {connectedCallback(){lifecycle.push('connected');}disconnectedCallback(){lifecycle.push('disconnected');}adoptedCallback(a,b){lifecycle.push(a===document && b!==document?'adopted':'back');}}
    customElements.define('nocturne-shadow-child',ShadowChild);const custom=make('nocturne-shadow-child',closed);
    eq(lifecycle.join(','),'connected','shadow-custom-connected');closedHost.remove();eq(lifecycle.at(-1),'disconnected','shadow-custom-disconnected');
    document.body.appendChild(closedHost);eq(lifecycle.at(-1),'connected','shadow-custom-reconnected');
    const inactive=document.implementation.createHTMLDocument('other');inactive.adoptNode(closedHost);
    eq(lifecycle.slice(-2).join(','),'disconnected,adopted','shadow-custom-adopted');eq(custom.ownerDocument,inactive,'shadow-owner-adopted');eq(closed.ownerDocument,inactive,'root-owner-adopted');
    document.body.appendChild(closedHost);eq(custom.ownerDocument,document,'adopt-back');
    class NoShadow extends HTMLElement {static disabledFeatures=['shadow'];}
    customElements.define('nocturne-no-shadow',NoShadow);throws(()=>make('nocturne-no-shadow').attachShadow({mode:'open'}),'NotSupportedError');
    const focusHost=make('div',document.body),focusRoot=focusHost.attachShadow({mode:'open',delegatesFocus:true}),input=make('input',focusRoot);
    focusHost.focus();eq(document.activeElement,focusHost,'document-focus-retarget');eq(focusRoot.activeElement,input,'shadow-active-element');eq(root.activeElement,null,'other-root-no-focus');
    let focusEvents=0,externalFocusOut=0;
    input.addEventListener('focus',()=>focusEvents++);input.addEventListener('blur',()=>focusEvents++);
    focusHost.focus();eq(focusEvents,0,'delegated-refocus-no-repeat');
    focusHost.addEventListener('focusout',()=>externalFocusOut++);const secondInput=make('input',focusRoot);secondInput.focus();
    eq(externalFocusOut,0,'focus-related-target-trims-host');eq(focusRoot.activeElement,secondInput,'second-shadow-focus');
    input.blur();eq(focusRoot.activeElement,secondInput,'unfocused-blur-does-not-steal-focus');
    secondInput.blur();eq(focusRoot.activeElement,null,'blur-clears-root-focus');
    const ordinaryInput=make('input',root);ordinaryInput.focus();host.blur();eq(root.activeElement,ordinaryInput,'nondelegating-host-blur-preserves-inner');ordinaryInput.blur();
    for(const n of [host,closedHost,manualHost,focusHost])n.remove();
    return checks;
}
