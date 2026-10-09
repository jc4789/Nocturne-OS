/* 実DOMの隣接挿入回帰。実サイト表示の代替ではない。 */
async function runAdjacentCases(){
    let count=0;
    const eq=(a,b,name)=>{count++;check('adjacent-'+name,Object.is(a,b));if(!Object.is(a,b))throw new Error(name);};
    const throws=(fn,name,label)=>{let error;try{fn();}catch(e){error=e;}eq(error&&error.name,name,label);};
    const target=document.createElement('div'),parent=document.createElement('section');
    parent.appendChild(target);document.body.appendChild(parent);
    try{
        for(const [position,expected] of [['beforebegin','<b></b><div></div>'],['afterend','<div></div><b></b>'],
            ['afterbegin','<div><b></b></div>'],['beforeend','<div><b></b></div>']]){
            const inserted=document.createElement('b');
            eq(target.insertAdjacentElement(position.toUpperCase(),inserted),inserted,position+'-return');
            eq(parent.innerHTML,expected,position+'-native-tree');inserted.remove();
            eq(target.insertAdjacentText(position,'<&>'),undefined,position+'-text-return');
            eq(parent.textContent,'<&>',position+'-literal-text');
            const text=position==='beforebegin'?parent.firstChild:position==='afterend'?parent.lastChild:target.firstChild;
            eq(text.nodeType,3,position+'-native-text');text.remove();
        }
        const first=document.createElement('i'),last=document.createElement('u'),moving=document.createElement('b');
        target.append(first,last);parent.appendChild(moving);
        target.insertAdjacentElement('afterbegin',moving);eq(target.firstChild,moving,'afterbegin-before-existing');
        target.insertAdjacentElement('beforeend',moving);eq(target.lastChild,moving,'beforeend-after-existing');
        eq(target.childNodes.length,3,'native-move-not-duplicate');target.replaceChildren();
        const detached=document.createElement('div'),unmoved=document.createElement('span');
        for(const position of ['beforebegin','afterend']){
            eq(detached.insertAdjacentElement(position,unmoved),null,position+'-detached-null');
            eq(detached.insertAdjacentText(position,'text'),undefined,position+'-detached-void');
            eq(unmoved.parentNode,null,position+'-detached-not-adopted');
        }
        for(const method of ['insertAdjacentElement','insertAdjacentText']){
            const value=method==='insertAdjacentElement'?unmoved:'text',fn=Element.prototype[method];
            throws(()=>fn.call({},'beforeend',value),'TypeError',method+'-forged-receiver');
            throws(()=>fn.call(document.createTextNode('x'),'beforeend',value),'TypeError',method+'-text-receiver');
            throws(()=>fn.call(target,'beforeend'),'TypeError',method+'-required-argument');
            throws(()=>fn.call(target,Symbol(),value),'TypeError',method+'-position-symbol');
            throws(()=>fn.call(target,' beforeend',value),'SyntaxError',method+'-no-trim');
            throws(()=>fn.call(detached,'invalid',value),'SyntaxError',method+'-detached-invalid');
        }
        for(const value of [null,undefined,{},document.createTextNode('x'),document.createDocumentFragment()])
            throws(()=>target.insertAdjacentElement('beforeend',value),'TypeError','element-brand-'+String(value));
        throws(()=>target.insertAdjacentText('beforeend',Symbol()),'TypeError','text-symbol');
        let converted=0;
        target.insertAdjacentText({toString(){converted++;return 'beforeend';}},{toString(){converted++;return 'converted';}});
        eq(converted,2,'single-conversion');eq(target.textContent,'converted','converted-text');target.replaceChildren();
        throws(()=>target.insertAdjacentElement('beforeend',parent),'HierarchyRequestError','ancestor-cycle');
        throws(()=>target.insertAdjacentElement('afterbegin',target),'HierarchyRequestError','self-cycle');
        const inert=new DOMParser().parseFromString('<!doctype html><html><body></body></html>','text/html');
        throws(()=>inert.documentElement.insertAdjacentText('beforebegin','x'),'HierarchyRequestError','document-text');
        throws(()=>inert.documentElement.insertAdjacentElement('afterend',inert.createElement('div')),'HierarchyRequestError','document-second-root');
        const foreign=inert.createElement('em');target.insertAdjacentElement('beforeend',foreign);
        eq(foreign.ownerDocument,document,'native-adoption');eq(target.firstChild,foreign,'adopted-native-child');foreign.remove();
        const svg=document.createElementNS('http://www.w3.org/2000/svg','svg'),rect=document.createElementNS(svg.namespaceURI,'rect');
        eq(svg.insertAdjacentElement('beforeend',rect),rect,'svg-element');eq(svg.firstChild,rect,'svg-native-child');
        const host=document.createElement('div'),shadow=host.attachShadow({mode:'closed'}),inside=document.createElement('span');
        shadow.appendChild(inside);
        throws(()=>inside.insertAdjacentElement('beforeend',host),'HierarchyRequestError','shadow-host-cycle');
        const template=document.createElement('template'),templateChild=document.createElement('span');template.content.appendChild(templateChild);
        throws(()=>templateChild.insertAdjacentElement('beforeend',template),'HierarchyRequestError','template-host-cycle');
        let records=[];const observer=new MutationObserver(value=>records.push(...value));observer.observe(target,{childList:true});
        const observed=document.createElement('strong');target.insertAdjacentElement('beforeend',observed);target.insertAdjacentText('afterbegin','text');
        await Promise.resolve();eq(records.length,2,'mutation-records');eq(records[0].addedNodes[0],observed,'mutation-element');
        eq(records[1].addedNodes[0],target.firstChild,'mutation-text');observer.disconnect();target.replaceChildren();
        const frame=document.createElement('iframe');document.body.appendChild(frame);
        try{const child=frame.contentDocument,childTarget=child.createElement('div');child.body.appendChild(childTarget);
            childTarget.insertAdjacentText('beforeend','child');eq(childTarget.firstChild.ownerDocument,child,'frame-text-owner');
            const adopted=document.createElement('i');eq(childTarget.insertAdjacentElement('afterbegin',adopted),adopted,'frame-element-return');
            eq(adopted.ownerDocument,child,'frame-element-adoption');eq(childTarget.firstChild.isSameNode(adopted),true,'frame-native-tree');
            // 既存appendChildにもある別realm wrapper参照同一性の制約は隠さない。
            const baseline=document.createElement('u');childTarget.appendChild(baseline);
            eq(childTarget.lastChild.isSameNode(baseline),true,'frame-append-native-baseline');
            console.log('ADJACENT_BOUNDARY cross-realm wrapper identity: adjacent='+String(childTarget.firstChild===adopted)+' append='+String(childTarget.lastChild===baseline));
        }finally{frame.remove();}
    }finally{parent.remove();}
    return count;
}
