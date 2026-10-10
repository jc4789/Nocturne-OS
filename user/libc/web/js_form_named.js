/* HTMLFormElement's native-wrapper named/indexed properties, including the
 * live multiple-match RadioNodeList. No Proxy is placed around DOM nodes. */
const formNameBridge=(()=>{
    const HTML='http://www.w3.org/1999/xhtml', past=new WeakMap(), groups=new WeakMap();
    const filter=Array.prototype.filter, mapGet=Map.prototype.get, mapSet=Map.prototype.set, mapDelete=Map.prototype.delete;
    const weakGet=WeakMap.prototype.get, weakSet=WeakMap.prototype.set, string=String;
    function read(form,images=false){
        let root=form,parent;while((parent=rawDom.get(root,'parentNode')))root=parent;
        return rawDom.formControls(root,form,images);
    }
    function matches(read,name){return apply(filter,read(),[n=>reflectedAttr(n,'id')===name || reflectedAttr(n,'name')===name]);}
    function radio(n){return rawDom.get(n,'localName')==='input' && rawDom.get(n,'namespaceURI')===HTML && (reflectedAttr(n,'type')||'').toLowerCase()==='radio';}
    class RadioNodeList extends NodeList {
        constructor(){throw new TypeError('Illegal RadioNodeList constructor');}
        get value(){const source=apply(weakGet,groups,[this]);if(!source)throw new TypeError('Illegal RadioNodeList receiver');
            for(const n of source())if(radio(n) && rawDom.get(n,'checked'))return reflectedAttr(n,'value')??'on';return '';
        }
        set value(value){const source=apply(weakGet,groups,[this]);if(!source)throw new TypeError('Illegal RadioNodeList receiver');
            if(typeof value==='symbol')throw new TypeError('Cannot convert Symbol to DOMString');value=string(value);
            for(const n of source())if(radio(n) && (reflectedAttr(n,'value')??'on')===value){dom('set',n,'checked',true);return;}
        }
    }
    function group(source){const list=collectionBridge.typed(source,RadioNodeList.prototype);apply(weakSet,groups,[list,source]);return list;}
    function named(source,name){
        if(!name)return null;const a=matches(source,name);
        return a.length>1?group(()=>matches(source,name)):a[0]||null;
    }
    const sources=new WeakMap();
    class HTMLFormControlsCollection extends HTMLCollection {
        constructor(){throw new TypeError('Illegal HTMLFormControlsCollection constructor');}
        namedItem(name){const source=apply(weakGet,sources,[this]);if(!source)throw new TypeError('Illegal HTMLFormControlsCollection receiver');
            if(!arguments.length)throw new TypeError('Missing name');if(typeof name==='symbol')throw new TypeError('Cannot convert Symbol to DOMString');return named(source,string(name));
        }
    }
    function collection(source){const list=collectionBridge.form(source,HTMLFormControlsCollection.prototype,name=>named(source,name));apply(weakSet,sources,[list,source]);return list;}
    function pastNames(form){let names=apply(weakGet,past,[form]);if(!names){names=new Map();apply(weakSet,past,[form,names]);}
        for(const [name,node] of names)if(rawDom.get(node,'form')!==form)apply(mapDelete,names,[name]);return names;
    }
    function property(form,key){
        const nodes=read(form);
        if(/^(0|[1-9][0-9]*)$/.test(key) && Number(key)<4294967295)return nodes[Number(key)];
        if(!key)return undefined;
        let source=()=>read(form),a=matches(source,key);
        if(!a.length){source=()=>read(form,true);a=matches(source,key);}
        const names=pastNames(form);
        if(a.length>1)return group(()=>matches(source,key));
        if(a.length){apply(mapSet,names,[key,a[0]]);return a[0];}
        return apply(mapGet,names,[key]);
    }
    function keys(form){
        const controls=read(form),images=read(form,true),names=pastNames(form),result=[];
        for(let i=0;i<controls.length;i++)result.push(string(i));
        for(const node of [...controls,...images]){
            for(const key of [reflectedAttr(node,'id'),reflectedAttr(node,'name')])if(key && !result.includes(key))result.push(key);
            for(const [key,old] of names)if(old===node && !result.includes(key))result.push(key);
        }
        return result;
    }
    for(const C of [RadioNodeList,HTMLFormControlsCollection])Object.defineProperty(C.prototype,Symbol.toStringTag,{value:C.name,configurable:true});
    Object.assign(globalThis,{RadioNodeList,HTMLFormControlsCollection});
    return {collection,property,keys};
})();
