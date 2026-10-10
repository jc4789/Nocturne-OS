/* Actual web_live bindings, not a JS DOM mock. Supporting regression only. */
async function runNativeMutationCases(){
    let count=0;
    const t=(name,ok)=>{count++;console.log((ok?'OK':'FAIL')+' paralleljs '+name);};
    const p=document.createElement('div');document.body.append(p);
    const records=[],mo=new MutationObserver(r=>records.push(...r));
    mo.observe(p,{childList:true,attributes:true,characterData:true,subtree:true,attributeOldValue:true,characterDataOldValue:true});
    const child=document.createElement('span');p.append(child);
    t('callback-deferred',records.length===0);
    let q=mo.takeRecords();t('take-synchronous',q.length===1&&q[0].target===p&&q[0].addedNodes[0]===child);
    child.setAttribute('data-x','a');child.setAttribute('data-x','a');child.removeAttribute('data-x');
    q=mo.takeRecords();t('same-value-attribute-record',q.length===3);
    t('old-attribute-values',q[0].oldValue===null&&q[1].oldValue==='a'&&q[2].oldValue==='a');
    child.setAttributeNS('urn:test','x:data','old');child.setAttributeNS('urn:test','y:data','new');
    q=mo.takeRecords();t('namespaced-attribute',q.length===2&&q[1].attributeName==='data'&&q[1].attributeNamespace==='urn:test'&&q[1].oldValue==='old');
    const attr=child.getAttributeNodeNS('urn:test','data');attr.value='three';
    q=mo.takeRecords();t('attr-node-setter',q.length===1&&q[0].oldValue==='new');
    const text=document.createTextNode('old\0text');child.append(text);mo.takeRecords();
    text.data='new';q=mo.takeRecords();t('character-old-length',q.length===1&&q[0].oldValue==='old\0text');
    const fragment=document.createDocumentFragment(),a=document.createElement('i'),b=document.createElement('b');fragment.append(a,b);
    child.append(fragment);q=mo.takeRecords();t('fragment-record',q.length===1&&q[0].addedNodes.length===2&&q[0].addedNodes[0]===a&&q[0].addedNodes[1]===b);
    const stable=q[0].addedNodes;child.removeChild(a);mo.takeRecords();t('record-list-static',stable.length===2&&stable[0]===a);
    p.removeChild(child);child.setAttribute('data-detached','yes');q=mo.takeRecords();
    t('detached-transient',q.length===2&&q[0].removedNodes[0]===child&&q[1].target===child);
    await Promise.resolve();child.setAttribute('data-detached','after');t('transient-expires',mo.takeRecords().length===0);
    p.append(child);mo.takeRecords();
    const filtered=new MutationObserver(()=>{});filtered.observe(child,{attributes:true,attributeFilter:['data-keep']});
    child.setAttribute('data-drop','x');child.setAttribute('data-keep','x');
    q=filtered.takeRecords();t('native-attribute-filter',q.length===1&&q[0].attributeName==='data-keep');filtered.disconnect();mo.takeRecords();
    const second=new MutationObserver(()=>{});second.observe(child,{attributes:true});
    child.setAttribute('data-both','1');q=second.takeRecords();const first=mo.takeRecords();
    t('multiple-observers',q.length===1&&first.length===1&&q[0]!==first[0]&&q[0].oldValue===null);second.disconnect();
    child.style.color='red';child.style.color='red';q=mo.takeRecords();t('style-noop-suppressed',q.length===1&&q[0].attributeName==='style');
    const r=document.createRange();r.setStart(child,1);r.setEnd(child,2);child.insertBefore(document.createElement('u'),child.firstChild);
    t('live-range-synchronous',r.startOffset===2&&r.endOffset===3);mo.takeRecords();
    /* takeRecords/disconnect do not cancel the agent's already queued notify
     * microtask. The mutations above queued it before this Promise reaction. */
    const pendingOrder=[];mo.disconnect();const pending=new MutationObserver(()=>pendingOrder.push('observer'));pending.observe(p,{childList:true});
    Promise.resolve().then(()=>pendingOrder.push('prior'));p.append(document.createElement('div'));queueMicrotask(()=>pendingOrder.push('later'));
    await Promise.resolve();console.log('ORDER paralleljs pending '+pendingOrder.join(','));
    t('native-pending-job-survives-disconnect',pendingOrder.join(',')==='observer,prior,later');pending.disconnect();
    /* The checkpoint above exhausted that pending job. Now the new mutation
     * must enqueue notification after prior, and before the later microtask. */
    const order=[],ordered=new MutationObserver(()=>order.push('observer'));ordered.observe(p,{childList:true});
    Promise.resolve().then(()=>order.push('prior'));p.append(document.createElement('div'));queueMicrotask(()=>order.push('later'));
    await Promise.resolve();console.log('ORDER paralleljs clean '+order.join(','));
    t('native-microtask-order',order.join(',')==='prior,observer,later');ordered.disconnect();
    const ceOrder=[];customElements.define('parallel-order',class extends HTMLElement{
        connectedCallback(){ceOrder.push('connected');}disconnectedCallback(){ceOrder.push('disconnected');}
        static get observedAttributes(){return ['data-a'];}attributeChangedCallback(){ceOrder.push('attribute');}
    });
    const ce=document.createElement('parallel-order');p.append(ce);t('ce-connected-synchronous',ceOrder.join(',')==='connected');
    ce.setAttribute('data-a','one');t('ce-attribute-synchronous',ceOrder.join(',')==='connected,attribute');
    ce.remove();t('ce-disconnected-synchronous',ceOrder.join(',')==='connected,attribute,disconnected');
    const reentrant=new MutationObserver(()=>{});reentrant.observe(text,{characterData:true,characterDataOldValue:true});
    text.data={toString(){text.data='inner';return 'outer';}};q=reentrant.takeRecords();
    t('coercion-mutation-order',q.length===2&&q[0].oldValue==='new'&&q[1].oldValue==='inner');reentrant.disconnect();
    const discarded=new MutationObserver(()=>t('disconnect-does-not-call',false));discarded.observe(p,{childList:true});p.append(document.createElement('s'));discarded.disconnect();
    await Promise.resolve();t('disconnect-discards',discarded.takeRecords().length===0);
    const onlyAttrs=new MutationObserver(()=>{}),outer=document.createElement('div'),inner=document.createElement('div');
    outer.append(inner);onlyAttrs.observe(outer,{attributes:true,subtree:true});inner.remove();
    await Promise.resolve();inner.setAttribute('after-expiry','x');t('attribute-only-transient-expires',onlyAttrs.takeRecords().length===0);onlyAttrs.disconnect();
    console.log('DONE paralleljs '+count);return count;
}
