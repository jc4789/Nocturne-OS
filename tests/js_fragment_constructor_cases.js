/* Native constructor / owner realm regression. Not real-site acceptance. */
async function runFragmentConstructorCases(){
    let count=0;
    const eq=(a,b,name)=>{count++;check('fragment-constructor-'+name,Object.is(a,b));if(!Object.is(a,b))throw new Error(name);};
    const throws=(fn,name)=>{let error;try{fn();}catch(e){error=e;}eq(error instanceof TypeError,true,name);};
    const fresh=()=>{
        const f=new DocumentFragment();
        eq(f.nodeType,11,'native-type');eq(f.nodeName,'#document-fragment','native-name');
        eq(f.ownerDocument,document,'owner-document');eq(f.parentNode,null,'detached-parent');
        eq(f.isConnected,false,'detached');eq(f.childNodes.length,0,'empty');
        eq(f instanceof Node,true,'node-interface');eq(f instanceof DocumentFragment,true,'fragment-interface');
        eq(f instanceof Element,false,'not-element');eq(Object.getPrototypeOf(f),DocumentFragment.prototype,'prototype');
        return f;
    };
    const first=fresh(),second=fresh();eq(first===second,false,'distinct-native-fragments');
    // Optional/excess arguments must be ignored without conversion.
    const ignored=new DocumentFragment({toString(){throw new Error('must not convert');}});
    eq(ignored.nodeType,11,'arguments-ignored');
    throws(()=>DocumentFragment(),'requires-new');throws(()=>new Node(),'node-stays-illegal');
    throws(()=>Node.prototype.appendChild.call(Object.create(DocumentFragment.prototype),document.createElement('i')),'forged-native-brand');
    class DerivedFragment extends DocumentFragment {constructor(){super();this.derived=true;}}
    const derived=new DerivedFragment();eq(derived.derived,true,'derived-constructor');
    eq(Object.getPrototypeOf(derived),DerivedFragment.prototype,'derived-prototype');eq(derived.ownerDocument,document,'derived-owner');
    const literal=document.createTextNode('native'),element=document.createElement('b');element.textContent=' tree';
    derived.append(literal,element);eq(derived.childNodes.length,2,'native-append');eq(derived.querySelector('b'),element,'native-query');
    const live=derived.childNodes;eq(live[0],literal,'live-first');literal.remove();eq(live.length,1,'live-after-remove');derived.prepend(literal);
    let records=[];const observer=new MutationObserver(r=>records.push(...r));observer.observe(derived,{childList:true});
    const extra=document.createElement('em');derived.appendChild(extra);await Promise.resolve();
    eq(records.length,1,'mutation-record');eq(records[0].addedNodes[0],extra,'mutation-native-node');observer.disconnect();extra.remove();
    const target=document.createElement('div');document.body.appendChild(target);
    try{
        eq(target.appendChild(derived),derived,'append-native-return');eq(derived.childNodes.length,0,'move-drains-fragment');
        eq(target.textContent,'native tree','move-native-text');eq(target.lastChild,element,'move-native-identity');
        const clone=first.cloneNode();eq(clone instanceof DocumentFragment,true,'clone-interface');eq(clone===first,false,'clone-native-distinct');
        // An iframe's constructor is associated with its own document, including
        // invocation through a parent-realm function reference.
        const frame=document.createElement('iframe');document.body.appendChild(frame);
        try{
            const win=frame.contentWindow,child=frame.contentDocument,childFragment=new win.DocumentFragment();
            eq(childFragment.ownerDocument.isSameNode(child),true,'frame-owner');eq(childFragment.nodeType,11,'frame-native-type');
            eq(Object.getPrototypeOf(childFragment),win.DocumentFragment.prototype,'frame-prototype');
            const foreign=child.createElement('strong');childFragment.appendChild(foreign);
            eq(childFragment.firstChild.isSameNode(foreign),true,'frame-native-append');
            target.appendChild(childFragment);eq(childFragment.childNodes.length,0,'frame-native-drain');
            eq(foreign.ownerDocument.isSameNode(document),true,'frame-native-adoption');eq(target.lastChild.isSameNode(foreign),true,'frame-native-move');
        }finally{frame.remove();}
    }finally{target.remove();}
    return count;
}
