/* WHATWG table interfaces over Nocturne's native DOM. Collection filters are
 * shallow: nested tables never contribute rows or cells to their ancestors. */
const htmlTablesBridge = (() => {
    'use strict';
    const define=Object.defineProperty, string=elementURL.string, get=rawDom.get;
    const HTML='http://www.w3.org/1999/xhtml';
    const bodyCollections=new WeakMap(),rowCollections=new WeakMap(),cellCollections=new WeakMap();
    function brand(node,name){
        if(get(node,'htmlInterface')!==name)throw new TypeError('Illegal '+name+' receiver');
    }
    function tag(node,name){return !!node && get(node,'nodeType')===1 && get(node,'namespaceURI')===HTML && get(node,'localName')===name;}
    function section(node){return tag(node,'thead') || tag(node,'tbody') || tag(node,'tfoot');}
    function children(node){return get(node,'childNodes');}
    function matching(node,name){const result=[];for(const n of children(node))if(tag(n,name))result.push(n);return result;}
    function first(node,name){for(const n of children(node))if(tag(n,name))return n;return null;}
    function tableRows(node){
        const head=[],body=[],foot=[];
        for(const n of children(node)){
            if(tag(n,'tr'))body.push(n);
            else if(section(n)){
                const target=tag(n,'thead')?head:tag(n,'tfoot')?foot:body;
                for(const row of children(n))if(tag(row,'tr'))target.push(row);
            }
        }
        return head.concat(body,foot);
    }
    function cells(node){const result=[];for(const n of children(node))if(tag(n,'td') || tag(n,'th'))result.push(n);return result;}
    function sameCollection(cache,node,read){
        let value=cache.get(node);
        if(!value){value=collectionBridge.domHTML(node,read);cache.set(node,value);}
        return value;
    }
    function create(node,name){return rawDom.create(get(node,'ownerDocument'),1,name,'');}
    function insert(parent,node,before=null){validateInsertion(parent,node,before);rawDom.insert(parent,node,before);return node;}
    function removeFirst(node,name){const child=first(node,name);if(child)rawDom.remove(child);}
    function reactions(action){return customElementsReady?customElementsBridge.reactions(action):action();}
    function headBefore(node){
        for(const n of children(node))if(get(n,'nodeType')===1 && !tag(n,'caption') && !tag(n,'colgroup'))return n;
        return null;
    }
    function nullable(value,name){
        if(value===null || value===undefined)return null;
        brand(value,name);return value;
    }
    function indexValue(value,optional){return optional && value===undefined?-1:value>>0;}
    function bounds(index,length,inserting){
        if(index < -1 || index > length || !inserting && index===length)
            throw new DOMException('Table index is out of range','IndexSizeError');
    }
    function deleteIndexed(nodes,index){
        bounds(index,nodes.length,false);
        const node=index===-1?nodes[nodes.length-1]:nodes[index];
        if(node)rawDom.remove(node);
    }
    class HTMLTableElement extends HTMLElement {constructor(){return customElementsBridge.construct(new.target,HTMLTableElement);}}
    class HTMLTableCaptionElement extends HTMLElement {constructor(){return customElementsBridge.construct(new.target,HTMLTableCaptionElement);}}
    class HTMLTableSectionElement extends HTMLElement {constructor(){return customElementsBridge.construct(new.target,HTMLTableSectionElement);}}
    class HTMLTableRowElement extends HTMLElement {constructor(){return customElementsBridge.construct(new.target,HTMLTableRowElement);}}
    class HTMLTableCellElement extends HTMLElement {constructor(){return customElementsBridge.construct(new.target,HTMLTableCellElement);}}
    class HTMLTableColElement extends HTMLElement {constructor(){return customElementsBridge.construct(new.target,HTMLTableColElement);}}
    function property(C,name,getter,setter){
        define(getter,'name',{value:'get '+name,configurable:true});
        if(setter)define(setter,'name',{value:'set '+name,configurable:true});
        define(C.prototype,name,{configurable:true,enumerable:true,get:getter,...(setter?{set:setter}:{})});
    }
    function method(C,name,fn){define(fn,'name',{value:name,configurable:true});define(C.prototype,name,{configurable:true,enumerable:true,writable:true,value:fn});}
    for(const [propertyName,name,type] of [['caption','caption','HTMLTableCaptionElement'],['tHead','thead','HTMLTableSectionElement'],['tFoot','tfoot','HTMLTableSectionElement']]){
        property(HTMLTableElement,propertyName,function(){brand(this,'HTMLTableElement');return first(this,name);},function(value){
            brand(this,'HTMLTableElement');value=nullable(value,type);
            if(value && name!=='caption' && !tag(value,name))throw new DOMException('Incorrect table section','HierarchyRequestError');
            reactions(()=>{
                removeFirst(this,name);
                if(value)insert(this,value,name==='caption'?get(this,'firstChild'):name==='thead'?headBefore(this):null);
            });
        });
        const suffix=propertyName==='caption'?'Caption':propertyName==='tHead'?'THead':'TFoot';
        method(HTMLTableElement,'create'+suffix,function(){
            brand(this,'HTMLTableElement');const old=first(this,name);if(old)return old;
            return insert(this,create(this,name),name==='caption'?get(this,'firstChild'):name==='thead'?headBefore(this):null);
        });
        method(HTMLTableElement,'delete'+suffix,function(){brand(this,'HTMLTableElement');reactions(()=>removeFirst(this,name));});
    }
    property(HTMLTableElement,'tBodies',function(){brand(this,'HTMLTableElement');return sameCollection(bodyCollections,this,()=>matching(this,'tbody'));});
    property(HTMLTableElement,'rows',function(){brand(this,'HTMLTableElement');return sameCollection(rowCollections,this,()=>tableRows(this));});
    method(HTMLTableElement,'createTBody',function(){
        brand(this,'HTMLTableElement');const bodies=matching(this,'tbody'),last=bodies[bodies.length-1];
        return insert(this,create(this,'tbody'),last?get(last,'nextSibling'):null);
    });
    method(HTMLTableElement,'insertRow',function(index=-1){
        brand(this,'HTMLTableElement');index=indexValue(index,true);
        const rows=tableRows(this);bounds(index,rows.length,true);const row=create(this,'tr');
        if(!rows.length){
            const bodies=matching(this,'tbody');let parent=bodies[bodies.length-1];
            if(!parent){parent=create(this,'tbody');insert(parent,row);insert(this,parent);return row;}
            return insert(parent,row);
        }
        const reference=index===-1 || index===rows.length?rows[rows.length-1]:rows[index];
        return insert(get(reference,'parentNode'),row,index===-1 || index===rows.length?null:reference);
    });
    method(HTMLTableElement,'deleteRow',function(index){
        brand(this,'HTMLTableElement');if(!arguments.length)throw new TypeError('deleteRow requires an index');
        index=indexValue(index,false);reactions(()=>deleteIndexed(tableRows(this),index));
    });
    property(HTMLTableSectionElement,'rows',function(){brand(this,'HTMLTableSectionElement');return sameCollection(rowCollections,this,()=>matching(this,'tr'));});
    method(HTMLTableSectionElement,'insertRow',function(index=-1){
        brand(this,'HTMLTableSectionElement');index=indexValue(index,true);const rows=matching(this,'tr');bounds(index,rows.length,true);
        return insert(this,create(this,'tr'),index===-1 || index===rows.length?null:rows[index]);
    });
    method(HTMLTableSectionElement,'deleteRow',function(index){
        brand(this,'HTMLTableSectionElement');if(!arguments.length)throw new TypeError('deleteRow requires an index');
        index=indexValue(index,false);reactions(()=>deleteIndexed(matching(this,'tr'),index));
    });
    property(HTMLTableRowElement,'rowIndex',function(){
        brand(this,'HTMLTableRowElement');let parent=get(this,'parentNode');
        if(section(parent))parent=get(parent,'parentNode');
        return tag(parent,'table')?tableRows(parent).indexOf(this):-1;
    });
    property(HTMLTableRowElement,'sectionRowIndex',function(){
        brand(this,'HTMLTableRowElement');const parent=get(this,'parentNode');
        return tag(parent,'table')?tableRows(parent).indexOf(this):section(parent)?matching(parent,'tr').indexOf(this):-1;
    });
    property(HTMLTableRowElement,'cells',function(){brand(this,'HTMLTableRowElement');return sameCollection(cellCollections,this,()=>cells(this));});
    method(HTMLTableRowElement,'insertCell',function(index=-1){
        brand(this,'HTMLTableRowElement');index=indexValue(index,true);const values=cells(this);bounds(index,values.length,true);
        return insert(this,create(this,'td'),index===-1 || index===values.length?null:values[index]);
    });
    method(HTMLTableRowElement,'deleteCell',function(index){
        brand(this,'HTMLTableRowElement');if(!arguments.length)throw new TypeError('deleteCell requires an index');
        index=indexValue(index,false);reactions(()=>deleteIndexed(cells(this),index));
    });
    property(HTMLTableCellElement,'cellIndex',function(){brand(this,'HTMLTableCellElement');const parent=get(this,'parentNode');return tag(parent,'tr')?cells(parent).indexOf(this):-1;});
    function reflect(C,name,attribute=name.toLowerCase(),nullEmpty=false){
        const type=C.name;
        property(C,name,function(){brand(this,type);return reflectedAttr(this,attribute)||'';},function(value){
            brand(this,type);reflectedAttr(this,attribute,nullEmpty && value===null?'':string(value));
        });
    }
    function span(C,name,min,max){
        const type=C.name,attribute=name.toLowerCase();
        property(C,name,function(){
            brand(this,type);const value=reflectedAttr(this,attribute),match=value===null?null:/^[\t\n\f\r ]*([+-]?[0-9]+)/.exec(value);
            if(!match)return 1;
            const parsed=+match[1];return parsed<0?1:Math.min(max,Math.max(min,parsed));
        },function(value){
            brand(this,type);value=value>>>0;
            // ReflectRange clamps only the getter, not the stored attribute.
            reflectedAttr(this,attribute,string(value<=2147483647?value:1));
        });
    }
    span(HTMLTableColElement,'span',1,1000);
    span(HTMLTableCellElement,'colSpan',1,1000);span(HTMLTableCellElement,'rowSpan',0,65534);
    reflect(HTMLTableCellElement,'headers');reflect(HTMLTableCellElement,'abbr');
    property(HTMLTableCellElement,'scope',function(){
        brand(this,'HTMLTableCellElement');const value=(reflectedAttr(this,'scope')||'').toLowerCase();
        return ['row','col','rowgroup','colgroup'].includes(value)?value:'';
    },function(value){brand(this,'HTMLTableCellElement');reflectedAttr(this,'scope',string(value));});
    for(const name of ['align','border','frame','rules','summary','width'])reflect(HTMLTableElement,name);
    for(const name of ['bgColor','cellPadding','cellSpacing'])reflect(HTMLTableElement,name,name.toLowerCase(),true);
    reflect(HTMLTableCaptionElement,'align');
    for(const C of [HTMLTableColElement,HTMLTableSectionElement,HTMLTableRowElement,HTMLTableCellElement]){
        reflect(C,'align');reflect(C,'ch','char');reflect(C,'chOff','charoff');reflect(C,'vAlign');
    }
    reflect(HTMLTableColElement,'width');
    for(const C of [HTMLTableRowElement,HTMLTableCellElement])reflect(C,'bgColor','bgcolor',true);
    for(const name of ['axis','height','width'])reflect(HTMLTableCellElement,name);
    property(HTMLTableCellElement,'noWrap',function(){brand(this,'HTMLTableCellElement');return reflectedAttr(this,'nowrap')!==null;},function(value){brand(this,'HTMLTableCellElement');reflectedAttr(this,'nowrap',value?'':null);});
    const exports={HTMLTableElement,HTMLTableCaptionElement,HTMLTableSectionElement,HTMLTableRowElement,HTMLTableCellElement,HTMLTableColElement};
    for(const C of Object.values(exports))define(C.prototype,Symbol.toStringTag,{value:C.name,configurable:true});
    for(const [name,C] of Object.entries(exports))define(globalThis,name,{value:C,writable:true,configurable:true});
    return {exports,nodeProtos:Object.values(exports).map(C=>C.prototype)};
})();
