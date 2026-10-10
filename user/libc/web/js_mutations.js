/* Mutation records from native DOM operation boundaries, delivered at the
 * QuickJS microtask checkpoint. No polling, invented tree, or empty observer.
 * JS DOM mutations (including fragment parsing through innerHTML) are covered.
 * Incremental HTML parser/document.write mutations still need native emission;
 * they do not pass through the script-facing DOM operation boundary. */
const mutationBridge = (() => {
    const native=typeof host.mutationObserve==='function';
    const registrations=new WeakMap(), observers=new WeakMap(), records=new WeakMap();
    const pending=new Set(),signalSlots=new Set();let scheduled=false,active=0;
    const get=(n,k)=>rawDom.get(n,k), parent=n=>get(n,'parentNode'), children=n=>get(n,'childNodes');
    const attr=(n,k)=>reflectedAttr(n,k);
    function data(o){const d=observers.get(o);if(!d)throw new TypeError('Illegal MutationObserver receiver');return d;}
    function drain(){
        if(!native)return;
        for(const row of host.mutationTake()){
            const d=observers.get(row.observer);if(!d || !d.active)continue;
            const fields=row.fields,record=Object.create(MutationRecord.prototype);
            records.set(record,{...fields,addedNodes:list(fields.addedNodes),removedNodes:list(fields.removedNodes)});
            d.queue.push(record);pending.add(row.observer);
        }
    }
    function ancestorRegistrations(n){const a=[];for(;n;n=parent(n)){const r=registrations.get(n);if(r)a.push(...r);}return a;}
    function notify(){
        drain();
        scheduled=false;const deliver=Array.from(pending),slots=Array.from(signalSlots);pending.clear();signalSlots.clear();
        for(const o of deliver){const d=data(o),q=d.queue;d.queue=[];
            for(const r of Array.from(d.regs))if(r.source){registrations.get(r.node).delete(r);d.regs.delete(r);}
            if(q.length)try{apply(d.callback,o,[q,o]);}catch(e){report(e);}
        }
        for(const slot of slots){const event=new Event('slotchange',{bubbles:true});event.isTrusted=true;dispatch(slot,event);}
    }
    function enqueue(type,target,fields={}){
        const interested=new Map();
        for(let n=target;n;n=parent(n))for(const r of registrations.get(n)||[]){const opt=r.options;
            if(n!==target && !opt.subtree || !opt[type])continue;
            if(type==='attributes' && opt.attributeFilter && (fields.attributeNamespace!==null || !opt.attributeFilter.includes(fields.attributeName)))continue;
            const old=(type==='attributes' && opt.attributeOldValue)||(type==='characterData' && opt.characterDataOldValue);
            interested.set(r.observer,interested.get(r.observer)||old);
        }
        for(const [o,old] of interested){
            const record=Object.create(MutationRecord.prototype);
            records.set(record,{type,target,attributeName:null,attributeNamespace:null,oldValue:null,
                previousSibling:null,nextSibling:null,...fields,
                addedNodes:list(fields.addedNodes||[]),removedNodes:list(fields.removedNodes||[]),oldValue:old?fields.oldValue??null:null});
            data(o).queue.push(record);pending.add(o);
        }
        if(interested.size && !scheduled){scheduled=true;queueMicrotask(notify);}
    }
    function transient(node,regs){for(const r of regs){if(!r.options.subtree)continue;
        const source=r.source||r,d=data(r.observer);let set=registrations.get(node);if(!set)registrations.set(node,set=new Set());
        if(Array.from(set).some(x=>x.source===source))continue;
        const t={node,observer:r.observer,options:r.options,source};set.add(t);d.regs.add(t);
    }}
    function removal(n){const p=parent(n);return p?{node:n,parent:p,previousSibling:get(n,'previousSibling'),nextSibling:get(n,'nextSibling'),regs:ancestorRegistrations(p)}:null;}
    function removed(r){if(!r)return;transient(r.node,r.regs);enqueue('childList',r.parent,{removedNodes:[r.node],previousSibling:r.previousSibling,nextSibling:r.nextSibling});}
    function before(op,node,key,value){
        if(!active || !node)return null;
        const attribute=attributeBridge.mutation(op,node,key,value);
        if(attribute)return {op:'attributes',...attribute};
        if(op==='attrSetNode' || op==='attrRemoveNode' || op==='attrNS' || op==='set' && get(node,'nodeType')===2)return null;
        if(op==='insert'){
            if(!key || key===value)return null;
            const fragment=get(key,'nodeType')===11,nodes=fragment?children(key):[key];
            return {op,node,nodes,fragment:fragment?key:null,removals:nodes.map(removal)};
        }
        if(op==='replace'){
            const fragment=get(key,'nodeType')===11,nodes=fragment?children(key):[key];
            let nextSibling=get(value,'nextSibling');if(nextSibling===key)nextSibling=get(key,'nextSibling');
            return {op,node,nodes,fragment:fragment?key:null,removals:nodes.map(removal),old:removal(value),
                same:key===value,previousSibling:get(value,'previousSibling'),nextSibling};
        }
        if(op==='remove')return {op,removal:removal(node)};
        if(op==='set' && ['innerHTML','textContent','nodeValue'].includes(key)){
            const type=get(node,'nodeType');
            if((key==='nodeValue' || key==='textContent') && (type===3 || type===7 || type===8))return {op:'characterData',node,oldValue:get(node,'nodeValue')};
            if(key!=='nodeValue' && (type===1 || type===11))return {op:'children',node,old:children(node),regs:ancestorRegistrations(node)};
        }
        let name;
        if(op==='attr')name=get(node,'namespaceURI')==='http://www.w3.org/1999/xhtml'?String(key).toLowerCase():String(key);
        else if(op==='style')name='style';
        else if(op==='set' && ['async','selected','imageWidth','imageHeight'].includes(key))name=key==='imageWidth'?'width':key==='imageHeight'?'height':key;
        else if(op==='set' && key==='value' && get(node,'localName')==='option')name='value';
        if(name!==undefined)return {op:'attributes',node,name,namespace:null,oldValue:attr(node,name),style:op==='style'};
        return null;
    }
    function after(t){
        if(!t)return;
        if(t.op==='attributes'){
            const now=rawDom.attrNS(t.node,t.namespace,t.name);
            if((now!==null || t.oldValue!==null) && (!t.style || now!==t.oldValue))enqueue('attributes',t.node,{attributeName:t.name,attributeNamespace:t.namespace,oldValue:t.oldValue});
        }else if(t.op==='characterData')enqueue('characterData',t.node,{oldValue:t.oldValue});
        else if(t.op==='children'){
            const added=children(t.node);for(const n of t.old)transient(n,t.regs);
            if(added.length || t.old.length)enqueue('childList',t.node,{addedNodes:added,removedNodes:t.old});
        }else if(t.op==='replace'){
            // Adoption removes the incoming node from its old parent normally;
            // only the replaced child's removal is observer-suppressed. One
            // final record describes the actual replacement, even for an
            // empty fragment or replacing a node with itself.
            if(t.fragment){
                for(const r of t.removals)if(r)transient(r.node,r.regs);
                if(t.nodes.length)enqueue('childList',t.fragment,{removedNodes:t.nodes});
            }else for(const r of t.removals)removed(r);
            if(t.old&&!t.same)transient(t.old.node,t.old.regs);
            enqueue('childList',t.node,{addedNodes:t.nodes,removedNodes:t.old&&!t.same?[t.old.node]:[],
                previousSibling:t.previousSibling,nextSibling:t.nextSibling});
        }else if(t.op==='remove'){if(t.removal && parent(t.removal.node)!==t.removal.parent)removed(t.removal);}
        else if(t.op==='insert' && t.nodes.length){
            if(t.fragment){for(const r of t.removals)if(r)transient(r.node,r.regs);enqueue('childList',t.fragment,{removedNodes:t.nodes});}
            else for(const r of t.removals)removed(r);
            enqueue('childList',t.node,{addedNodes:t.nodes,previousSibling:get(t.nodes[0],'previousSibling'),nextSibling:get(t.nodes.at(-1),'nextSibling')});
        }
    }
    class MutationObserver {
        constructor(callback){if(typeof callback!=='function')throw new TypeError('Expected callback');observers.set(this,{callback,queue:[],regs:new Set(),active:false});}
        observe(target,options={}){
            const d=data(this);if(!target || typeof target!=='object')throw new TypeError('Expected Node');get(target,'nodeType');
            if(options===null)options={};if(typeof options!=='object' && typeof options!=='function')throw new TypeError('Expected options dictionary');
            const o={subtree:!!options.subtree,childList:!!options.childList};
            const a=options.attributes,c=options.characterData,ao=options.attributeOldValue,co=options.characterDataOldValue,af=options.attributeFilter;
            o.attributes=a===undefined?(ao!==undefined || af!==undefined):!!a;o.characterData=c===undefined?co!==undefined:!!c;
            o.attributeOldValue=!!ao;o.characterDataOldValue=!!co;
            if(af!==undefined){if(af===null || typeof af[Symbol.iterator]!=='function')throw new TypeError('Expected attributeFilter sequence');o.attributeFilter=Array.from(af,String);}
            if(!o.childList && !o.attributes && !o.characterData || (!o.attributes && (o.attributeOldValue || af!==undefined)) || (!o.characterData && o.characterDataOldValue))throw new TypeError('Invalid mutation observation options');
            let set=registrations.get(target);if(!set)registrations.set(target,set=new Set());
            let r=Array.from(set).find(x=>x.observer===this && !x.source);
            if(r){for(const t of Array.from(d.regs))if(t.source===r){registrations.get(t.node).delete(t);d.regs.delete(t);}r.options=o;}
            else{r={node:target,observer:this,options:o};set.add(r);d.regs.add(r);}
            if(!d.active){d.active=true;active++;}
            if(native)host.mutationObserve(this,target,
                (o.childList?1:0)|(o.attributes?2:0)|(o.characterData?4:0)|(o.subtree?8:0)|
                (o.attributeOldValue?16:0)|(o.characterDataOldValue?32:0)|(o.attributeFilter?64:0),o.attributeFilter||[]);
        }
        disconnect(){const d=data(this);drain();if(native)host.mutationObserve(this,null);for(const r of d.regs)registrations.get(r.node).delete(r);d.regs.clear();d.queue=[];if(d.active){active--;d.active=false;}}
        takeRecords(){const d=data(this);drain();const q=d.queue;d.queue=[];return q;}
    }
    class MutationRecord {constructor(){throw new TypeError('Illegal MutationRecord constructor');}}
    for(const k of ['type','target','addedNodes','removedNodes','previousSibling','nextSibling','attributeName','attributeNamespace','oldValue'])Object.defineProperty(MutationRecord.prototype,k,{enumerable:true,configurable:true,get(){const r=records.get(this);if(!r)throw new TypeError('Illegal MutationRecord receiver');return r[k];}});
    for(const C of [MutationObserver,MutationRecord])Object.defineProperty(C.prototype,Symbol.toStringTag,{value:C.name,configurable:true});
    function collectSlots(){
        for(const slot of rawDom.slotChanges(null))signalSlots.add(slot);
        if(signalSlots.size && !scheduled){scheduled=true;queueMicrotask(notify);}
    }
    Object.assign(globalThis,{MutationObserver,MutationRecord});return {native,flush:notify,before,after,signalSlots:collectSlots};
})();
