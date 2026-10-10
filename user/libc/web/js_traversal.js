/* DOM traversal over native node relationships, not a copied JS tree.
 * Include after js_mutations.js and js_document.js. Traversal is ordinary DOM
 * tree order: shadow roots and template contents must be explicit roots.
 * Algorithms: https://dom.spec.whatwg.org/#traversal */
(() => {
    const walkers=new WeakMap(), iteratorData=new WeakMap(), iterators=new Set();
    const WeakRefImpl=WeakRef, deref=WeakRef.prototype.deref;
    const ACCEPT=1, REJECT=2, SKIP=3, SHOW_ALL=0xffffffff;
    const get=(node,key)=>rawDom('get',node,key);
    const parent=node=>get(node,'parentNode');
    const first=node=>get(node,'firstChild'), last=node=>get(node,'lastChild');
    const next=node=>get(node,'nextSibling'), previous=node=>get(node,'previousSibling');
    function node(value){
        if(!rawDom('isNode',null,value))throw new TypeError('Expected a native Node');
        return value;
    }
    function data(map,value,name){
        const d=map.get(value);
        if(!d)throw new TypeError('Illegal '+name+' receiver');
        return d;
    }
    function invalidState(){throw new DOMException('A traversal filter is already active','InvalidStateError');}
    function filter(d,node){
        if(d.active)invalidState();
        const bit=get(node,'nodeType')-1;
        if(bit<0 || bit>31 || !(d.whatToShow & (1<<bit)))return SKIP;
        if(d.filter===null)return ACCEPT;
        d.active=true;
        try{
            const callable=typeof d.filter==='function', callback=callable?d.filter:d.filter.acceptNode;
            if(typeof callback!=='function')throw new TypeError('NodeFilter.acceptNode must be callable');
            /* Web IDL unsigned short conversion occurs while the active flag
             * is set, including conversion of a callback's object result. */
            return (apply(callback,callable?undefined:d.filter,[node])>>>0)&0xffff;
        }finally{d.active=false;}
    }
    function state(root,whatToShow,callback){
        root=node(root);whatToShow=whatToShow>>>0;
        if(callback!==null && typeof callback!=='object' && typeof callback!=='function')
            throw new TypeError('Expected a NodeFilter callback');
        return {root,whatToShow,filter:callback,active:false};
    }
    function chosen(d,node){d.current=node;return node;}
    function child(d,forward){
        const edge=forward?first:last, sibling=forward?next:previous;
        let node=edge(d.current);
        while(node){
            const result=filter(d,node);
            if(result===ACCEPT)return chosen(d,node);
            if(result===SKIP){const descendant=edge(node);if(descendant){node=descendant;continue;}}
            while(node){
                const adjacent=sibling(node);
                if(adjacent){node=adjacent;break;}
                const p=parent(node);
                if(!p || p===d.root || p===d.current)return null;
                node=p;
            }
        }
        return null;
    }
    function sibling(d,forward){
        const adjacent=forward?next:previous, edge=forward?first:last;
        let node=d.current;
        if(node===d.root)return null;
        while(node){
            let candidate=adjacent(node);
            while(candidate){
                node=candidate;
                const result=filter(d,node);
                if(result===ACCEPT)return chosen(d,node);
                candidate=edge(node);
                if(result===REJECT || !candidate)candidate=adjacent(node);
            }
            node=parent(node);
            if(!node || node===d.root)return null;
            if(filter(d,node)===ACCEPT)return null;
        }
        return null;
    }
    class TreeWalker {
        constructor(){throw new TypeError('Illegal TreeWalker constructor');}
        get root(){return data(walkers,this,'TreeWalker').root;}
        get whatToShow(){return data(walkers,this,'TreeWalker').whatToShow;}
        get filter(){return data(walkers,this,'TreeWalker').filter;}
        get currentNode(){return data(walkers,this,'TreeWalker').current;}
        set currentNode(value){const d=data(walkers,this,'TreeWalker');d.current=node(value);}
        parentNode(){
            const d=data(walkers,this,'TreeWalker');let node=d.current;
            while(node && node!==d.root){node=parent(node);if(node && filter(d,node)===ACCEPT)return chosen(d,node);}
            return null;
        }
        firstChild(){return child(data(walkers,this,'TreeWalker'),true);}
        lastChild(){return child(data(walkers,this,'TreeWalker'),false);}
        nextSibling(){return sibling(data(walkers,this,'TreeWalker'),true);}
        previousSibling(){return sibling(data(walkers,this,'TreeWalker'),false);}
        nextNode(){
            const d=data(walkers,this,'TreeWalker');let node=d.current,result=ACCEPT;
            while(node){
                let descendant;
                while(result!==REJECT && (descendant=first(node))){
                    node=descendant;result=filter(d,node);
                    if(result===ACCEPT)return chosen(d,node);
                }
                let adjacent=null,temporary=node;
                while(temporary){
                    if(temporary===d.root)return null;
                    adjacent=next(temporary);if(adjacent)break;
                    temporary=parent(temporary);
                }
                if(!adjacent)return null;
                node=adjacent;result=filter(d,node);
                if(result===ACCEPT)return chosen(d,node);
            }
            return null;
        }
        previousNode(){
            const d=data(walkers,this,'TreeWalker');let node=d.current;
            while(node!==d.root){
                let candidate=previous(node);
                while(candidate){
                    node=candidate;let result=filter(d,node),descendant;
                    while(result!==REJECT && (descendant=last(node))){node=descendant;result=filter(d,node);}
                    if(result===ACCEPT)return chosen(d,node);
                    candidate=previous(node);
                }
                if(node===d.root || !parent(node))return null;
                node=parent(node);
                if(filter(d,node)===ACCEPT)return chosen(d,node);
            }
            return null;
        }
    }
    function following(node,root,skipChildren=false,removed=null){
        if(!skipChildren){const c=first(node);if(c)return c;}
        while(node && node!==root){
            let sibling=next(node);
            while(sibling && removed && removed.has(sibling))sibling=next(sibling);
            if(sibling)return sibling;
            node=parent(node);
        }
        return null;
    }
    function preceding(node,root,removed=null){
        if(node===root)return null;
        let sibling=previous(node);
        while(sibling && removed && removed.has(sibling))sibling=previous(sibling);
        if(!sibling)return parent(node);
        let descendant;
        while((descendant=last(sibling))){
            while(descendant && removed && removed.has(descendant))descendant=previous(descendant);
            if(!descendant)break;
            sibling=descendant;
        }
        return sibling;
    }
    function traverse(d,forward){
        /* Do not overwrite an in-flight candidate on callback reentrancy. */
        if(d.active)invalidState();
        d.candidate={...d.reference};
        try{
            while(true){
                let node=d.candidate.node;
                if(forward?!d.candidate.before:d.candidate.before){
                    node=forward?following(node,d.root):preceding(node,d.root);
                    if(!node)return null;
                }
                d.candidate={node,before:!forward};
                /* REJECT does not prune NodeIterator descendants. A filter may
                 * remove node; mutation hooks also adjust this candidate. */
                if(filter(d,node)===ACCEPT){d.reference=d.candidate;return node;}
            }
        }finally{d.candidate=null;}
    }
    class NodeIterator {
        constructor(){throw new TypeError('Illegal NodeIterator constructor');}
        get root(){return data(iteratorData,this,'NodeIterator').root;}
        get whatToShow(){return data(iteratorData,this,'NodeIterator').whatToShow;}
        get filter(){return data(iteratorData,this,'NodeIterator').filter;}
        get referenceNode(){return data(iteratorData,this,'NodeIterator').reference.node;}
        get pointerBeforeReferenceNode(){return data(iteratorData,this,'NodeIterator').reference.before;}
        nextNode(){return traverse(data(iteratorData,this,'NodeIterator'),true);}
        previousNode(){return traverse(data(iteratorData,this,'NodeIterator'),false);}
        detach(){data(iteratorData,this,'NodeIterator');}
    }
    function ancestor(ancestor,node){for(;node;node=parent(node))if(node===ancestor)return true;return false;}
    function adjusted(pointer,d,removed,alreadyRemoved){
        if(!pointer || !ancestor(removed,pointer.node) || ancestor(removed,d.root))return pointer;
        if(pointer.before){
            const node=following(removed,d.root,true,alreadyRemoved);
            if(node)return {node,before:true};
        }
        return {node:preceding(removed,d.root,alreadyRemoved),before:false};
    }
    function liveIterators(){
        const live=[];
        for(const weak of iterators){
            const iterator=apply(deref,weak,[]);
            if(iterator)live.push(iteratorData.get(iterator));else iterators.delete(weak);
        }
        return live;
    }
    function removals(op,target,key,value){
        if(!target)return [];
        if(op==='remove')return parent(target)?[target]:[];
        if(op==='replace' && key && value){
            const nodes=get(key,'nodeType')===11?Array.from(get(key,'childNodes')):parent(key)?[key]:[];
            if(value!==key && parent(value))nodes.push(value);
            return nodes;
        }
        if(op==='insert' && key && key!==value){
            if(!rawDom('isNode',null,key))return [];
            return get(key,'nodeType')===11?Array.from(get(key,'childNodes')):parent(key)?[key]:[];
        }
        if(op==='set' && (key==='innerHTML' || key==='textContent')){
            const type=get(target,'nodeType');
            if(type===1 || type===11)return Array.from(get(target,'childNodes'));
        }
        if(op==='set' && key==='title' && get(target,'nodeType')===9){
            const head=get(target,'head');
            for(let c=head?first(head):null;c;c=next(c))
                if(get(c,'localName')==='title')return Array.from(get(c,'childNodes'));
        }
        return [];
    }
    /* The native boundary validates after "before" and invokes "after" only
     * on success, before CE reactions or dynamic scripts. Prepare pre-remove
     * pointers in the old tree, commit only successful mutations. Iterator
     * objects are weakly tracked: discarded iterators do not pin DOM trees.
     * Native parser/document.write removals do not enter this script boundary. */
    const beforeMutation=mutationBridge.before, afterMutation=mutationBridge.after;
    mutationBridge.before=function(...args){
        const base=apply(beforeMutation,mutationBridge,args);
        if(!iterators.size)return base;
        const removed=removals(...args);
        if(!removed.length)return base;
        const plans=[];
        for(const d of liveIterators()){
            const oldReference=d.reference, oldCandidate=d.candidate, alreadyRemoved=new Set();
            let reference=oldReference,candidate=oldCandidate;
            for(const r of removed){
                reference=adjusted(reference,d,r,alreadyRemoved);
                candidate=adjusted(candidate,d,r,alreadyRemoved);
                alreadyRemoved.add(r);
            }
            if(reference!==oldReference || candidate!==oldCandidate)
                plans.push({d,oldReference,oldCandidate,reference,candidate});
        }
        return plans.length?{traversal:true,base,plans}:base;
    };
    mutationBridge.after=function(token){
        if(token && token.traversal){
            /* A nested mutation/conversion may have changed a pointer since
             * preparation; never overwrite that newer traversal state. */
            for(const p of token.plans){
                if(p.d.reference===p.oldReference)p.d.reference=p.reference;
                if(p.d.candidate===p.oldCandidate)p.d.candidate=p.candidate;
            }
            return apply(afterMutation,mutationBridge,[token.base]);
        }
        return apply(afterMutation,mutationBridge,[token]);
    };
    Object.defineProperties(Document.prototype,{
        createTreeWalker:{enumerable:true,configurable:true,writable:true,value:function createTreeWalker(root,whatToShow=SHOW_ALL,callback=null){
            documentBridge.brand(this);
            if(!arguments.length)throw new TypeError('createTreeWalker requires a root');
            const d=state(root,whatToShow,callback),walker=Object.create(TreeWalker.prototype);
            d.current=d.root;walkers.set(walker,d);return walker;
        }},
        createNodeIterator:{enumerable:true,configurable:true,writable:true,value:function createNodeIterator(root,whatToShow=SHOW_ALL,callback=null){
            documentBridge.brand(this);
            if(!arguments.length)throw new TypeError('createNodeIterator requires a root');
            const d=state(root,whatToShow,callback),iterator=Object.create(NodeIterator.prototype);
            d.reference={node:d.root,before:true};d.candidate=null;iteratorData.set(iterator,d);
            if((iterators.size&63)===0)liveIterators();
            iterators.add(new WeakRefImpl(iterator));return iterator;
        }}
    });
    const NodeFilter=()=>{throw new TypeError('Illegal NodeFilter constructor');};
    const constants={FILTER_ACCEPT:ACCEPT,FILTER_REJECT:REJECT,FILTER_SKIP:SKIP,SHOW_ALL,
        SHOW_ELEMENT:1,SHOW_ATTRIBUTE:2,SHOW_TEXT:4,SHOW_CDATA_SECTION:8,
        SHOW_ENTITY_REFERENCE:0x10,SHOW_ENTITY:0x20,SHOW_PROCESSING_INSTRUCTION:0x40,
        SHOW_COMMENT:0x80,SHOW_DOCUMENT:0x100,SHOW_DOCUMENT_TYPE:0x200,
        SHOW_DOCUMENT_FRAGMENT:0x400,SHOW_NOTATION:0x800};
    for(const [key,value] of Object.entries(constants))Object.defineProperty(NodeFilter,key,{value,enumerable:true});
    for(const C of [TreeWalker,NodeIterator]){
        for(const key of Object.getOwnPropertyNames(C.prototype))if(key!=='constructor'){
            const descriptor=Object.getOwnPropertyDescriptor(C.prototype,key);
            Object.defineProperty(C.prototype,key,{...descriptor,enumerable:true});
        }
        Object.defineProperty(C.prototype,Symbol.toStringTag,{value:C.name,configurable:true});
    }
    Object.assign(globalThis,{NodeFilter,TreeWalker,NodeIterator});
})();
