/* Live DOM ranges over the sole native tree. Offsets are UTF-16 code units
 * for CharacterData and native child indices otherwise (DOM Standard, §5).
 * Script-facing native insert/remove/adopt/replace-all/data hooks update the
 * boundaries synchronously, before CE callbacks or observer delivery.
 * Incremental parser/native-only mutations still need native range emission.
 * No selection painting or invented layout rectangles are provided here. */
const rangeBridge = (() => {
    const states=new WeakMap(), live=new Set(), Ref=WeakRef, deref=WeakRef.prototype.deref;
    const abstractToken={}, get=(n,k)=>rawDom('get',n,k);
    const parent=n=>get(n,'parentNode'), children=n=>get(n,'childNodes');
    const character=n=>[3,4,7,8].includes(get(n,'nodeType'));
    let replacing=null;
    function node(n){if(!rawDom('isNode',null,n))throw new TypeError('Expected a native Node');return n;}
    function data(r){const d=states.get(r);if(!d)throw new TypeError('Illegal Range receiver');return d;}
    function changed(d){if(d.selectionChanged)d.selectionChanged();}
    function required(count,min){if(count<min)throw new TypeError('Missing Range arguments');}
    function string(v){if(typeof v==='symbol')throw new TypeError('Cannot convert Symbol to DOMString');return String(v);}
    const offset=v=>(+v)>>>0;
    function root(n){let p;while((p=parent(n)))n=p;return n;}
    function inside(a,n){for(;n;n=parent(n))if(a===n)return true;return false;}
    function length(n){const t=get(n,'nodeType');return t===2 || t===10?0:character(n)?get(n,'nodeValue').length:children(n).length;}
    function validate(n,o){
        if(get(n,'nodeType')===10)throw new DOMException('Doctype cannot be a boundary container','InvalidNodeTypeError');
        if(o>length(n))throw new DOMException('Offset exceeds node length','IndexSizeError');
    }
    function compare(a,b){
        if(a[0]===b[0])return a[1]===b[1]?0:a[1]<b[1]?-1:1;
        const aa=[],bb=[];for(let n=a[0];n;n=parent(n))aa.push(n);for(let n=b[0];n;n=parent(n))bb.push(n);
        let i=aa.length-1,j=bb.length-1;
        if(aa[i]!==bb[j])throw new DOMException('Boundary points have different roots','WrongDocumentError');
        while(i>=0 && j>=0 && aa[i]===bb[j]){i--;j--;}
        if(i<0)return children(a[0]).indexOf(bb[j])<a[1]?1:-1;
        if(j<0)return children(b[0]).indexOf(aa[i])<b[1]?-1:1;
        const siblings=children(aa[i+1]);return siblings.indexOf(aa[i])<siblings.indexOf(bb[j])?-1:1;
    }
    function set(d,which,n,o){
        validate(n,o);const p=[n,o],other=which==='start'?'end':'start';
        if(root(d.start[0])!==root(n) || compare(p,d[other])===(which==='start'?1:-1))d[other]=p.slice();
        d[which]=p;
        changed(d);
    }
    function relative(r,which,n,after){
        const d=data(r);node(n);const p=parent(n);
        if(!p)throw new DOMException('The node has no parent','InvalidNodeTypeError');
        set(d,which,p,children(p).indexOf(n)+(after?1:0));
    }
    function common(d){let n=d.start[0];while(!inside(n,d.end[0]))n=parent(n);return n;}
    function collapsed(d){return d.start[0]===d.end[0] && d.start[1]===d.end[1];}
    function register(r,d){states.set(r,d);live.add(new Ref(r));return r;}
    function make(d){return register(Object.create(Range.prototype),{start:d.start.slice(),end:d.end.slice()});}
    function contained(n,d){return compare([n,0],d.start)>0 && compare([n,length(n)],d.end)<0;}
    function partial(n,d){return inside(n,d.start[0])!==inside(n,d.end[0]);}
    function deleteRange(d){
        if(collapsed(d))return;
        // Snapshot the original endpoints and topmost contained nodes before
        // native mutations move any live boundary. Only the actual DOM tree
        // participates: do not cross shadow/template/Attr ownership edges.
        const start=d.start.slice(),end=d.end.slice();
        if(start[0]===end[0] && character(start[0])){
            replace(start[0],start[1],end[1]-start[1],'');return;
        }
        const original={start,end},remove=[],stack=[common(original)];
        while(stack.length){
            const n=stack.pop();
            if(contained(n,original)){remove.push(n);continue;}
            const kids=children(n);for(let i=kids.length-1;i>=0;i--)stack.push(kids[i]);
        }
        let newNode=start[0],newOffset=start[1];
        if(!inside(start[0],end[0])){
            let reference=start[0],p=parent(reference);
            while(p && !inside(p,end[0])){reference=p;p=parent(reference);}
            newNode=p;newOffset=children(p).indexOf(reference)+1;
        }
        // DOM deleteContents collapses BEFORE deleting. Subsequent native
        // removal/splice hooks also update this caret and all other ranges.
        d.start=[newNode,newOffset];d.end=d.start.slice();changed(d);
        if(character(start[0]))replace(start[0],start[1],length(start[0])-start[1],'');
        for(const n of remove)dom('remove',n);
        if(character(end[0]))replace(end[0],0,end[1],'');
    }
    function clone(d){
        const owner=get(d.start[0],'nodeType')===9?d.start[0]:get(d.start[0],'ownerDocument');
        const fragment=rawDom('create',owner,11,'#document-fragment','');
        if(collapsed(d))return fragment;
        function append(n){rawDom('insert',fragment,n,null);}
        function characterClone(n,start,end){const c=rawDom('clone',n,false);rawDom('set',c,'nodeValue',get(n,'nodeValue').slice(start,end));return c;}
        if(d.start[0]===d.end[0] && character(d.start[0])){
            append(characterClone(d.start[0],d.start[1],d.end[1]));return fragment;
        }
        const kids=children(common(d)),whole=kids.filter(n=>contained(n,d));
        if(whole.some(n=>get(n,'nodeType')===10))throw new DOMException('Cannot clone a doctype into a fragment','HierarchyRequestError');
        const first=inside(d.start[0],d.end[0])?null:kids.find(n=>partial(n,d));
        const last=inside(d.end[0],d.start[0])?null:kids.slice().reverse().find(n=>partial(n,d));
        if(first){
            if(character(first))append(characterClone(first,d.start[1],length(first)));
            else{const c=rawDom('clone',first,false);append(c);rawDom('insert',c,clone({start:d.start.slice(),end:[first,length(first)]}),null);}
        }
        for(const n of whole)append(rawDom('clone',n,true));
        if(last){
            if(character(last))append(characterClone(last,0,d.end[1]));
            else{const c=rawDom('clone',last,false);append(c);rawDom('insert',c,clone({start:[last,0],end:d.end.slice()}),null);}
        }
        return fragment;
    }
    class AbstractRange {constructor(token){if(token!==abstractToken)throw new TypeError('Illegal AbstractRange constructor');}}
    class Range extends AbstractRange {
        constructor(){super(abstractToken);register(this,{start:[document,0],end:[document,0]});}
        get commonAncestorContainer(){return common(data(this));}
        setStart(n,o){const d=data(this);required(arguments.length,2);node(n);o=offset(o);set(d,'start',n,o);}
        setEnd(n,o){const d=data(this);required(arguments.length,2);node(n);o=offset(o);set(d,'end',n,o);}
        setStartBefore(n){required(arguments.length,1);relative(this,'start',n,false);}
        setStartAfter(n){required(arguments.length,1);relative(this,'start',n,true);}
        setEndBefore(n){required(arguments.length,1);relative(this,'end',n,false);}
        setEndAfter(n){required(arguments.length,1);relative(this,'end',n,true);}
        collapse(toStart=false){const d=data(this);if(toStart)d.end=d.start.slice();else d.start=d.end.slice();changed(d);}
        selectNode(n){
            const d=data(this);required(arguments.length,1);node(n);const p=parent(n);
            if(!p)throw new DOMException('The node has no parent','InvalidNodeTypeError');
            const i=children(p).indexOf(n);d.start=[p,i];d.end=[p,i+1];changed(d);
        }
        selectNodeContents(n){const d=data(this);required(arguments.length,1);node(n);validate(n,0);d.start=[n,0];d.end=[n,length(n)];changed(d);}
        compareBoundaryPoints(how,source){
            const d=data(this);required(arguments.length,2);how=offset(how)&65535;const other=data(source);
            if(how>3)throw new DOMException('Unknown comparison mode','NotSupportedError');
            if(root(d.start[0])!==root(other.start[0]))throw new DOMException('Ranges have different roots','WrongDocumentError');
            return compare(how===0 || how===3?d.start:d.end,how===0 || how===1?other.start:other.end);
        }
        cloneRange(){return make(data(this));}
        cloneContents(){const d=data(this);return customElementsBridge.reactions(()=>clone({start:d.start.slice(),end:d.end.slice()}));}
        deleteContents(){const d=data(this);return customElementsBridge.reactions(()=>deleteRange(d));}
        createContextualFragment(markup){
            const d=data(this);required(arguments.length,1);markup=string(markup);
            // DOMString conversion may run author code and change the range;
            // read its native start container only after that conversion.
            const start=d.start[0],type=get(start,'nodeType');
            const owner=type===9?start:get(start,'ownerDocument');
            if(get(owner,'contentType')!=='text/html')
                throw new DOMException('XML contextual fragments are not implemented','NotSupportedError');
            let context=type===1?start:null;
            if(type===3 || type===4 || type===8){const p=parent(start);if(p && get(p,'nodeType')===1)context=p;}
            const html='http://www.w3.org/1999/xhtml';
            if(context && get(context,'namespaceURI')!==html)
                throw new DOMException('Foreign-namespace fragment contexts are not implemented','NotSupportedError');
            // The HTML Standard uses a new body, not the document's existing
            // body and its ancestry, for null context or an HTML html element.
            if(!context || get(context,'localName')==='html')context=rawDom('create',owner,1,'body','');
            return customElementsBridge.reactions(()=>{
                // true requests the native contextual (runScripts) mode. The
                // fragment remains detached and never executes while parsing;
                // eligible scripts are prepared by native code on insertion.
                const fragment=rawDom('parseFragment',context,markup,true);
                if(get(fragment,'scripting'))customElementsBridge.upgradeTree(fragment);
                return fragment;
            });
        }
        detach(){data(this);}
        isPointInRange(n,o){
            const d=data(this);required(arguments.length,2);node(n);o=offset(o);
            if(root(n)!==root(d.start[0]))return false;validate(n,o);
            return compare([n,o],d.start)>=0 && compare([n,o],d.end)<=0;
        }
        comparePoint(n,o){
            const d=data(this);required(arguments.length,2);node(n);o=offset(o);
            if(root(n)!==root(d.start[0]))throw new DOMException('Point has a different root','WrongDocumentError');
            validate(n,o);return compare([n,o],d.start)<0?-1:compare([n,o],d.end)>0?1:0;
        }
        intersectsNode(n){
            const d=data(this);required(arguments.length,1);node(n);if(root(n)!==root(d.start[0]))return false;
            const p=parent(n);if(!p)return true;const i=children(p).indexOf(n);
            return compare([p,i],d.end)<0 && compare([p,i+1],d.start)>0;
        }
        toString(){
            const d=data(this);if(collapsed(d))return '';const stack=[common(d)],parts=[];
            while(stack.length){const n=stack.pop(),type=get(n,'nodeType');
                if(type===3 || type===4){
                    const text=get(n,'nodeValue');
                    if(compare([n,text.length],d.start)>0 && compare([n,0],d.end)<0)
                        parts.push(text.slice(n===d.start[0]?d.start[1]:0,n===d.end[0]?d.end[1]:text.length));
                }else{const kids=children(n);for(let i=kids.length-1;i>=0;i--)stack.push(kids[i]);}
            }
            return parts.join('');
        }
    }
    for(const k of ['startContainer','startOffset','endContainer','endOffset','collapsed'])Object.defineProperty(AbstractRange.prototype,k,{
        enumerable:true,configurable:true,get(){const d=data(this);return k==='collapsed'?collapsed(d):d[k.startsWith('start')?'start':'end'][k.endsWith('Container')?0:1];}
    });
    for(const [k,v] of [['START_TO_START',0],['START_TO_END',1],['END_TO_END',2],['END_TO_START',3]])
        for(const target of [Range,Range.prototype])Object.defineProperty(target,k,{value:v,enumerable:true});
    Object.defineProperty(Range.prototype,'deleteContents',{enumerable:true});
    for(const k of ['extractContents','insertNode','surroundContents','getClientRects','getBoundingClientRect'])
        Object.defineProperty(Range.prototype,k,{enumerable:true,configurable:true,writable:true,value:function(){data(this);
            throw new DOMException('Range '+k+' is not implemented','NotSupportedError');
        }});
    for(const C of [AbstractRange,Range])Object.defineProperty(C.prototype,Symbol.toStringTag,{value:C.name,configurable:true});
    Document.prototype.createRange=function(){documentBridge.brand(this);return make({start:[this,0],end:[this,0]});};
    Object.assign(globalThis,{AbstractRange,Range});

    function capture(){
        const a=[];for(const ref of live){const r=apply(deref,ref,[]);if(!r){live.delete(ref);continue;}
            const d=data(r);a.push({d,start:d.start.slice(),end:d.end.slice()});}
        return a;
    }
    function before(op,n,key,value){
        if(!n || !live.size || !(op==='insert' || op==='remove' || op==='set' && ['innerHTML','textContent','nodeValue','title'].includes(key)))return null;
        if(op==='insert' && (!key || key===value))return null;
        const type=get(n,'nodeType');
        if(op==='set' && (key==='nodeValue' && !character(n) || key==='textContent' && !character(n) && type!==1 && type!==11))return null;
        if(op==='set' && key==='title'){
            if(type!==9)return null;
            const head=get(n,'head');if(!head)return null;
            n=children(head).find(c=>get(c,'nodeType')===1 && get(c,'localName')==='title');if(!n)return null;
            key='textContent';
        }
        const ranges=capture();if(!ranges.length)return null;
        const trees=new Map(),tree=p=>{let a=trees.get(p);if(!a)trees.set(p,a=children(p).slice());return a;};
        function points(fn){for(const r of ranges){fn(r.start);fn(r.end);}}
        function remove(child){
            const p=parent(child);if(!p)return;const siblings=tree(p),i=siblings.indexOf(child);if(i<0)return;
            points(q=>{if(inside(child,q[0])){q[0]=p;q[1]=i;}else if(q[0]===p && q[1]>i)q[1]--;});
            siblings.splice(i,1);
        }
        if(op==='remove')remove(n);
        else if(op==='insert'){
            const nodes=get(key,'nodeType')===11?children(key):[key];if(!nodes.length)return null;
            for(const child of nodes)remove(child);
            const siblings=tree(n),i=value?siblings.indexOf(value):siblings.length;
            if(i<0)return null;
            points(q=>{if(q[0]===n && q[1]>i)q[1]+=nodes.length;});
        }else if(character(n) && key!=='innerHTML'){
            const old=get(n,'nodeValue'),detail=replacing && replacing.node===n?replacing:null;
            const start=detail?detail.offset:0,count=detail?detail.count:old.length;
            // Ordinary data/nodeValue/textContent setters replace the entire
            // string. Partial CharacterData methods supply exact splice data.
            points(q=>{if(q[0]===n && q[1]>start){if(q[1]<=start+count)q[1]=start;
                else q[1]+=detail?detail.inserted-count:string(value).length-count;}});
        }else for(const child of children(n))remove(child);
        return ranges;
    }
    function after(ranges){if(!ranges)return;for(const r of ranges){
        const moved=r.d.start[0]!==r.start[0]||r.d.start[1]!==r.start[1]||r.d.end[0]!==r.end[0]||r.d.end[1]!==r.end[1];
        r.d.start=r.start;r.d.end=r.end;if(moved)changed(r.d);
    }}

    // Keep splice provenance rather than guessing a diff between two strings:
    // repeated characters and same-value replacements still have DOM semantics.
    function replace(node,at,count,insert){
        const old=get(node,'nodeValue');
        if(at>old.length)throw new DOMException('Offset exceeds data length','IndexSizeError');
        count=Math.min(count,old.length-at);
        const previous=replacing;replacing={node,offset:at,count,inserted:insert.length};
        try{dom('set',node,'nodeValue',old.slice(0,at)+insert+old.slice(at+count));}finally{replacing=previous;}
    }
    CharacterData.prototype.appendData=function(value){characterDataBrand(this);required(arguments.length,1);value=string(value);replace(this,length(this),0,value);};
    CharacterData.prototype.insertData=function(at,value){characterDataBrand(this);required(arguments.length,2);at=offset(at);value=string(value);replace(this,at,0,value);};
    CharacterData.prototype.deleteData=function(at,count){characterDataBrand(this);required(arguments.length,2);at=offset(at);count=offset(count);replace(this,at,count,'');};
    CharacterData.prototype.replaceData=function(at,count,value){characterDataBrand(this);required(arguments.length,3);at=offset(at);count=offset(count);value=string(value);replace(this,at,count,value);};
    return {before,after,isRange:r=>states.has(r),boundaries:r=>{const d=data(r);return {start:d.start.slice(),end:d.end.slice()};},comparePoints:compare,
        listen:(r,listener)=>{data(r).selectionChanged=listener;}};
})();
