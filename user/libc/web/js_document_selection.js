/* Document selection uses actual live Range boundaries over the sole native
 * DOM. It is separate from input/textarea selection. Native page-drag paint
 * and Range layout remain unsupported, not simulated successes. */
(() => {
    const token={},states=new WeakMap(),apply=Reflect.apply;
    const NativeRange=Range,SelectionEvent=Event,enqueue=setTimeout;
    const rangeMethods={};
    for(const key of ['setStart','setEnd','collapse','cloneRange','selectNodeContents','toString','deleteContents'])rangeMethods[key]=NativeRange.prototype[key];
    const call=(r,key,...args)=>apply(rangeMethods[key],r,args);
    const data=s=>{const d=states.get(s);if(!d)throw new TypeError('Illegal Selection receiver');return d;};
    const get=(n,key)=>rawDom.get(n,key);
    function root(n){let p;while((p=get(n,'parentNode')))n=p;return n;}
    function inDocument(n){let r=root(n);while(r!==document){const host=get(r,'shadowHost');if(!host)return false;r=root(host);}return true;}
    function node(n){if(!rawDom.isNode(null,n))throw new TypeError('Expected a native Node');return n;}
    const offset=n=>(+n)>>>0;
    function point(n,o){node(n);const r=new NativeRange();call(r,'setStart',n,o);call(r,'collapse',true);return r;}
    function notice(d){if(d.scheduled)return;d.scheduled=true;enqueue(()=>{d.scheduled=false;dispatch(document,new SelectionEvent('selectionchange'));},0);}
    function change(d,r,direction=d.direction){
        if(d.range)rangeBridge.listen(d.range,null);
        d.range=r;d.direction=r?direction:'none';
        if(r)rangeBridge.listen(r,()=>notice(d));
        notice(d);
    }
    function endpoints(d){if(!d.range)return null;const p=rangeBridge.boundaries(d.range);
        return {anchor:d.direction==='forward'?p.start:p.end,focus:d.direction==='forward'?p.end:p.start};}
    function documentPoint(p){return p&&root(p[0])===document;}
    function visible(d){const p=endpoints(d);return p&&documentPoint(p.anchor)&&documentPoint(p.focus);}
    function collapsed(d){if(!d.range)return true;const p=rangeBridge.boundaries(d.range);return p.start[0]===p.end[0]&&p.start[1]===p.end[1];}
    function collapse(d,n,o){
        if(n===null){if(d.range)change(d,null);return;}
        node(n);o=offset(o);const r=point(n,o);if(!inDocument(n))return;change(d,r);
    }
    function setBase(d,anchor,a,focus,f){
        node(anchor);node(focus);a=offset(a);f=offset(f);point(anchor,a);point(focus,f);
        if(!inDocument(anchor)||!inDocument(focus))return;
        const order=root(anchor)===root(focus)?rangeBridge.comparePoints([anchor,a],[focus,f]):null,r=new NativeRange();
        const before=order!==null&&order<0;
        call(r,'setStart',before?anchor:focus,before?a:f);
        call(r,'setEnd',before?focus:anchor,before?f:a);
        change(d,r,order!==null&&order>0?'backward':'forward');
    }
    class Selection {
        constructor(key){if(key!==token)throw new TypeError('Illegal Selection constructor');states.set(this,{range:null,direction:'none',scheduled:false});}
        get anchorNode(){const p=endpoints(data(this));return p&&documentPoint(p.anchor)?p.anchor[0]:null;}
        get anchorOffset(){const p=endpoints(data(this));return p&&documentPoint(p.anchor)?p.anchor[1]:0;}
        get focusNode(){const p=endpoints(data(this));return p&&documentPoint(p.focus)?p.focus[0]:null;}
        get focusOffset(){const p=endpoints(data(this));return p&&documentPoint(p.focus)?p.focus[1]:0;}
        get isCollapsed(){return collapsed(data(this));}
        get rangeCount(){return visible(data(this))?1:0;}
        get type(){const d=data(this);return visible(d)?(collapsed(d)?'Caret':'Range'):'None';}
        get direction(){return data(this).direction;}
        getRangeAt(index){const d=data(this);if(!arguments.length)throw new TypeError('Range index required');index=offset(index);
            if(index!==0||!visible(d))throw new DOMException('Selection range index is out of bounds','IndexSizeError');return d.range;}
        addRange(range){const d=data(this);if(!arguments.length||!rangeBridge.isRange(range))throw new TypeError('Expected a Range');
            const p=rangeBridge.boundaries(range);if(root(p.start[0])!==document||root(p.end[0])!==document||d.range)return;change(d,range);}
        removeRange(range){const d=data(this);if(!arguments.length||!rangeBridge.isRange(range))throw new TypeError('Expected a Range');
            if(d.range!==range)throw new DOMException('Range is not selected','NotFoundError');change(d,null);}
        removeAllRanges(){const d=data(this);if(d.range)change(d,null);}
        empty(){const d=data(this);if(d.range)change(d,null);}
        deleteFromDocument(){const d=data(this);if(visible(d))call(d.range,'deleteContents');}
        collapse(n,o=0){const d=data(this);if(!arguments.length)throw new TypeError('Boundary node required');collapse(d,n,o);}
        setPosition(n,o=0){const d=data(this);if(!arguments.length)throw new TypeError('Boundary node required');collapse(d,n,o);}
        collapseToStart(){const d=data(this);if(!d.range)throw new DOMException('Selection is empty','InvalidStateError');
            const r=call(d.range,'cloneRange');call(r,'collapse',true);change(d,r);}
        collapseToEnd(){const d=data(this);if(!d.range)throw new DOMException('Selection is empty','InvalidStateError');
            const r=call(d.range,'cloneRange');call(r,'collapse',false);change(d,r);}
        setBaseAndExtent(anchor,a,focus,f){const d=data(this);if(arguments.length<4)throw new TypeError('Two boundary points required');
            setBase(d,anchor,a,focus,f);}
        extend(n,o=0){const d=data(this);if(!arguments.length)throw new TypeError('Boundary node required');node(n);o=offset(o);
            if(!inDocument(n))return;if(!d.range)throw new DOMException('Selection is empty','InvalidStateError');
            const p=endpoints(d);if(root(n)!==root(p.anchor[0])){change(d,point(n,o),'forward');return;}
            setBase(d,p.anchor[0],p.anchor[1],n,o);}
        selectAllChildren(n){const d=data(this);if(!arguments.length)throw new TypeError('Node required');node(n);
            if(get(n,'nodeType')===10)throw new DOMException('Doctype cannot be selected','InvalidNodeTypeError');
            if(root(n)!==document)return;const r=new NativeRange();call(r,'setStart',n,0);call(r,'setEnd',n,get(n,'childNodes').length);change(d,r,'forward');}
        containsNode(n,partial=false){const d=data(this);if(!arguments.length)throw new TypeError('Node required');node(n);if(!d.range||root(n)!==document||!visible(d))return false;
            const p=rangeBridge.boundaries(d.range),type=get(n,'nodeType'),len=[3,4,7,8].includes(type)?get(n,'nodeValue').length:get(n,'childNodes').length;
            const begin=rangeBridge.comparePoints(p.start,[n,0]),end=rangeBridge.comparePoints(p.end,[n,len]);
            return partial?rangeBridge.comparePoints(p.end,[n,0])>=0&&rangeBridge.comparePoints(p.start,[n,len])<=0:begin<=0&&end>=0;}
        toString(){const d=data(this);return d.range?call(d.range,'toString'):'';}
    }
    const selection=new Selection(token);
    Object.defineProperty(Selection.prototype,Symbol.toStringTag,{value:'Selection',configurable:true});
    for(const name of Object.getOwnPropertyNames(Selection.prototype))if(name!=='constructor')
        Object.defineProperty(Selection.prototype,name,{...Object.getOwnPropertyDescriptor(Selection.prototype,name),enumerable:true});
    Object.defineProperty(Document.prototype,'getSelection',{configurable:true,writable:true,enumerable:true,value:function(){
        if(!rawDom.isNode(null,this)||get(this,'nodeType')!==9)throw new TypeError('Document receiver required');return this===document?selection:null;
    }});
    Object.defineProperty(globalThis,'getSelection',{configurable:true,writable:true,enumerable:true,value:function(){return selection;}});
    globalThis.Selection=Selection;
})();
