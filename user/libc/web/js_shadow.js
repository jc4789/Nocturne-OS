/* Native Shadow DOM bindings. Node identity, ownership, slot assignment and
 * rendering all remain in Nocturne's DOM; there is no parallel JS tree.
 * https://dom.spec.whatwg.org/#shadow-trees
 * https://html.spec.whatwg.org/multipage/scripting.html#the-slot-element */
const shadowBridge = (() => {
    const get=(n,key)=>rawDom.get(n,key), root=n=>rawDom.root(n,false);
    const isNode=n=>rawDom.isNode(null,n);
    const string=value=>{if(typeof value==='symbol')throw new TypeError('Cannot convert Symbol to DOMString');return String(value);};
    function dictionary(value){
        if(value==null)return {};
        if(typeof value!=='object' && typeof value!=='function')throw new TypeError('Expected dictionary');
        return value;
    }
    function shadow(n){if(!get(n,'shadowHost'))throw new TypeError('ShadowRoot receiver required');return n;}
    function slot(n){get(n,'slotBrand');return n;}
    class ShadowRoot extends DocumentFragment {
        constructor(){throw new TypeError('Illegal ShadowRoot constructor');}
        get host(){return get(shadow(this),'shadowHost');}
        get mode(){return get(this,'shadowMode');}
        get delegatesFocus(){return get(this,'shadowDelegatesFocus');}
        get clonable(){return get(this,'shadowClonable');}
        get serializable(){return get(this,'shadowSerializable');}
        get slotAssignment(){return get(this,'shadowSlotAssignment');}
        get innerHTML(){return get(shadow(this),'innerHTML');}
        set innerHTML(value){shadow(this);dom('set',this,'innerHTML',value===null?'':string(value));}
        get activeElement(){return get(shadow(this),'activeElement');}
        elementFromPoint(x,y){
            shadow(this);if(arguments.length<2)throw new TypeError('Two coordinates required');
            return rawDom.elementFromPoint(this,x,y);
        }
    }
    class HTMLSlotElement extends HTMLElement {
        constructor(){throw new TypeError('Illegal HTMLSlotElement constructor');}
        get name(){slot(this);return reflectedAttr(this,'name')||'';}
        set name(value){slot(this);reflectedAttr(this,'name',string(value));}
        assignedNodes(options={}){slot(this);return rawDom.slotNodes(this,!!dictionary(options).flatten);}
        assignedElements(options={}){slot(this);return rawDom.slotNodes(this,!!dictionary(options).flatten).filter(n=>get(n,'nodeType')===1);}
        assign(...nodes){slot(this);dom('slotAssign',this,...nodes);}
    }
    Object.defineProperties(Element.prototype,{
        shadowRoot:{configurable:true,enumerable:true,get(){
            get(this,'elementBrand');const value=get(this,'shadowRoot');
            return value && get(value,'shadowMode')==='open'?value:null;
        }},
        slot:{configurable:true,enumerable:true,get(){get(this,'elementBrand');return reflectedAttr(this,'slot')||'';},
            set(value){get(this,'elementBrand');reflectedAttr(this,'slot',string(value));}},
        attachShadow:{configurable:true,enumerable:true,writable:true,value:function(init){
            get(this,'elementBrand');if(!arguments.length)throw new TypeError('ShadowRootInit required');
            const o=dictionary(init),clonable=!!o.clonable,registry=o.customElementRegistry,delegates=!!o.delegatesFocus;
            const modeValue=o.mode;if(modeValue===undefined)throw new TypeError('Shadow root mode required');
            const mode=string(modeValue),serializable=!!o.serializable,assignmentValue=o.slotAssignment;
            const assignment=assignmentValue===undefined?'named':string(assignmentValue);
            if(!['open','closed'].includes(mode) || !['named','manual'].includes(assignment))throw new TypeError('Invalid shadow root mode or slot assignment');
            if(registry!==undefined && registry!==customElements)throw new DOMException('Scoped registries are not implemented','NotSupportedError');
            if(!get(this,'shadowHostValid') || get(this,'shadowRoot') || !customElementsBridge.allowShadow(this))
                throw new DOMException('This element cannot attach a shadow root','NotSupportedError');
            return dom('shadowAttach',this,mode==='closed',delegates,clonable,serializable,assignment==='manual');
        }}
    });
    for(const proto of [Element.prototype,Text.prototype])Object.defineProperty(proto,'assignedSlot',{
        configurable:true,enumerable:true,get(){
            const type=get(this,'nodeType');
            if(type!==(proto===Element.prototype?1:3))throw new TypeError('Illegal slottable receiver');
            return get(this,'assignedSlotOpen');
        }
    });
    Object.defineProperty(DocumentFragment.prototype,'getElementById',{configurable:true,enumerable:true,writable:true,value:function(id){
        if(get(this,'nodeType')!==11)throw new TypeError('DocumentFragment receiver required');
        if(!arguments.length)throw new TypeError('Identifier required');return rawDom.id(this,string(id));
    }});
    for(const C of [ShadowRoot,HTMLSlotElement])Object.defineProperty(C.prototype,Symbol.toStringTag,{value:C.name,configurable:true});
    Object.assign(globalThis,{ShadowRoot,HTMLSlotElement});
    shadowHandlerTarget=(target,type)=>type==='slotchange' && isNode(target) && !!get(target,'shadowHost');
    installHandlers(ShadowRoot.prototype,['slotchange']);

    /* Snapshot roots and closed boundaries BEFORE invoking any listener. Even
       reparenting and page-overridden parentNode/getRootNode cannot change the
       current dispatch's target, relatedTarget or composed path. */
    function scopes(node){
        const result=[];
        if(!isNode(node))return result;
        for(let r=root(node),h=get(r,'shadowHost');h;r=root(h),h=get(r,'shadowHost'))
            result.push({root:r,host:h,closed:get(r,'shadowMode')==='closed'});
        return result;
    }
    function capture(nodes){
        const info=new Map();
        for(const node of nodes)info.set(node,scopes(node));
        return {nodes:apply(eventSlice,nodes,[]),info};
    }
    function retarget(node,against,info){
        const inner=info.get(node)||scopes(node),outer=info.get(against)||scopes(against);
        for(const boundary of inner){
            if(outer.some(other=>other.root===boundary.root))break;
            node=boundary.host;
        }
        return node;
    }
    function path(target,event,snapshot,relatedSnapshot){
        let saved=snapshot;
        if(!saved){
            const nodes=[target];
            if(isNode(target)){
                const targetRoot=root(target);
                for(let n=target;;){
                    let parent=get(n,'assignedSlot')||get(n,'parentNode');
                    if(!parent){
                        const h=get(n,'shadowHost');
                        if(h && (event.composed || n!==targetRoot))parent=h;
                        else if(n===document && event.type!=='load')parent=globalThis;
                    }
                    if(!parent)break;nodes.push(parent);if(parent===globalThis)break;n=parent;
                }
            }
            saved=capture(nodes);
        }else if(Array.isArray(saved))saved=capture(saved);
        const info=new Map(saved.info);
        if(relatedSnapshot)for(const [node,value] of relatedSnapshot.info)if(!info.has(node))info.set(node,value);
        const originalRelated=event.relatedTarget??null;
        if(originalRelated!==null && !info.has(originalRelated))info.set(originalRelated,scopes(originalRelated));
        const targetScopes=info.get(target)||[],originRoot=targetScopes.length?targetScopes[0].root:null;
        const entries=[];
        for(const node of saved.nodes){
            const adjusted=retarget(target,node,info),related=originalRelated===null?null:retarget(originalRelated,node,info);
            const sameOriginScope=originRoot===null || (info.get(node)||[]).some(scope=>scope.root===originRoot);
            if(related!==null && adjusted===related && !(target===originalRelated && sameOriginScope))break;
            entries.push({node,target:adjusted,related,atTarget:node===adjusted,scopes:info.get(node)||[]});
            if(!event.composed && node===originRoot)break;
        }
        for(const entry of entries)entry.visible=entries.filter(other=>
            other.scopes.every(boundary=>!boundary.closed || entry.scopes.some(scope=>scope.root===boundary.root))
        ).map(other=>other.node);
        const last=entries.at(-1);
        const finalTarget=last?last.target:target,finalRelated=last?last.related:originalRelated;
        const clear=(info.get(finalTarget)||scopes(finalTarget)).length || (finalRelated!==null && (info.get(finalRelated)||scopes(finalRelated)).length);
        return {entries,finalTarget:clear?null:finalTarget,finalRelated:clear?null:finalRelated};
    }
    return {ShadowRoot,HTMLSlotElement,root,isNode,dictionary,capture,path};
})();
