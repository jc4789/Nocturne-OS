globalThis.runDOMCompareCases = function() {
    let count=0;
    const assert=(v,n)=>{count++;check('node-compare-'+n,v);if(!v)throw new Error(n);};
    const a=document.createElement('div'),b=document.createElement('DIV');
    a.setAttribute('a','1');a.setAttribute('b','2');b.setAttribute('b','2');b.setAttribute('a','1');
    assert(a.isEqualNode(b)&&b.isEqualNode(a),'attribute-order');
    assert(!a.isSameNode(b)&&a.isSameNode(a),'identity');
    assert(!a.isEqualNode()&&!a.isEqualNode(null)&&!a.isSameNode(),'null');
    a.appendChild(document.createTextNode('ab'));b.appendChild(document.createTextNode('ab'));
    assert(a.isEqualNode(b),'text-child');
    b.firstChild.nodeValue='ac';assert(!a.isEqualNode(b),'text-different');
    b.textContent='ab';b.appendChild(document.createComment('x'));assert(!a.isEqualNode(b),'child-count');
    a.appendChild(document.createComment('x'));assert(a.isEqualNode(b),'comment-child');
    assert(a.isEqualNode(a.cloneNode(true))&&!a.isEqualNode(a.cloneNode(false)),'clone');
    b.setAttribute('a','3');assert(!a.isEqualNode(b),'attribute-value');
    b.setAttribute('a','1');b.removeAttribute('b');assert(!a.isEqualNode(b),'attribute-count');
    const f=document.createDocumentFragment();f.appendChild(a.cloneNode(true));
    assert(f.isEqualNode(f.cloneNode(true)),'fragment');
    const svg=document.createElementNS('http://www.w3.org/2000/svg','div');assert(!a.isEqualNode(svg),'namespace');
    let threw=false;try{a.isEqualNode({});}catch(e){threw=e instanceof TypeError;}assert(threw,'other-brand');
    threw=false;try{Node.prototype.isEqualNode.call({},a);}catch(e){threw=e instanceof TypeError;}assert(threw,'receiver-brand');
    return count;
};

/* Runs through web_live/native node_t; not a JavaScript shadow DOM. */
globalThis.runDOMPositionCases = function() {
    let count=0;
    const assert=(value,name)=>{count++;check('node-position-'+name,value);if(!value)throw new Error(name);};
    const equal=(actual,expected,name)=>assert(actual===expected,name+' '+actual+'!='+expected);
    const throws=(call,name)=>{let error;try{call();}catch(e){error=e;}assert(error&&error.name==='TypeError',name);};
    const compare=Node.prototype.compareDocumentPosition;
    equal(compare.length,1,'method-length');
    for(const [name,value] of [['DISCONNECTED',1],['PRECEDING',2],['FOLLOWING',4],['CONTAINS',8],['CONTAINED_BY',16],['IMPLEMENTATION_SPECIFIC',32]]){
        const key='DOCUMENT_POSITION_'+name;
        for(const [target,label] of [[Node,'constructor'],[Node.prototype,'prototype']]){
            const descriptor=Object.getOwnPropertyDescriptor(target,key);
            equal(target[key],value,label+'-'+name);
            assert(descriptor&&!descriptor.writable&&!descriptor.configurable&&descriptor.enumerable,label+'-'+name+'-descriptor');
        }
    }
    const root=document.createElement('div'),first=document.createElement('span'),last=document.createElement('span');
    const text=document.createTextNode('text'),comment=document.createComment('comment');
    root.appendChild(first);root.appendChild(last);first.appendChild(text);last.appendChild(comment);document.body.appendChild(root);
    equal(root.compareDocumentPosition(root),0,'same');
    equal(root.compareDocumentPosition(text),20,'descendant');equal(text.compareDocumentPosition(root),10,'ancestor');
    equal(first.compareDocumentPosition(last),4,'next-sibling');equal(last.compareDocumentPosition(first),2,'previous-sibling');
    equal(text.compareDocumentPosition(comment),4,'cousin');equal(comment.compareDocumentPosition(text),2,'reverse-cousin');
    equal(document.compareDocumentPosition(root),20,'document-descendant');equal(root.compareDocumentPosition(document),10,'document-ancestor');
    equal(document.doctype.compareDocumentPosition(document.documentElement),4,'doctype-before-html');
    root.insertBefore(last,first);equal(first.compareDocumentPosition(last),2,'live-move');root.appendChild(last);
    const fragment=document.createDocumentFragment(),fragmentChild=document.createElement('i');fragment.appendChild(fragmentChild);
    equal(fragment.compareDocumentPosition(fragmentChild),20,'fragment-descendant');equal(fragmentChild.compareDocumentPosition(fragment),10,'fragment-ancestor');
    function disconnected(a,b,name){
        const ab=a.compareDocumentPosition(b),ba=b.compareDocumentPosition(a);
        assert((ab&33)===33&&((ab&6)===2||(ab&6)===4),name+'-flags');
        equal(ab^ba,6,name+'-antisymmetric');
        equal(a.compareDocumentPosition(b),ab,name+'-stable');
        return ab;
    }
    disconnected(fragment,root,'different-roots');
    const x=document.createElement('i'),y=document.createElement('i');
    const detachedOrder=disconnected(x,y,'detached');
    const secondDocument=document.implementation.createHTMLDocument('other');
    disconnected(document,secondDocument,'different-documents');
    secondDocument.adoptNode(x);equal(x.compareDocumentPosition(y),detachedOrder,'adopt-stable-disconnected');
    root.setAttribute('alpha','1');root.setAttribute('beta','2');
    const alpha=root.getAttributeNode('alpha'),beta=root.getAttributeNode('beta');
    equal(alpha.compareDocumentPosition(alpha),0,'same-attr');
    equal(alpha.compareDocumentPosition(beta),36,'attr-list-forward');equal(beta.compareDocumentPosition(alpha),34,'attr-list-reverse');
    equal(root.compareDocumentPosition(alpha),20,'element-attr');equal(alpha.compareDocumentPosition(root),10,'attr-element');
    equal(alpha.compareDocumentPosition(text),4,'attr-before-descendant');equal(text.compareDocumentPosition(alpha),2,'descendant-after-attr');
    last.setAttribute('gamma','3');const gamma=last.getAttributeNode('gamma');
    equal(alpha.compareDocumentPosition(gamma),4,'different-owner-attrs');equal(gamma.compareDocumentPosition(alpha),2,'different-owner-attrs-reverse');
    root.setAttribute('alpha','changed');equal(root.getAttributeNode('alpha'),alpha,'attr-identity-on-value-change');
    equal(alpha.compareDocumentPosition(beta),36,'attr-list-retained');
    root.removeAttributeNode(alpha);disconnected(alpha,beta,'detached-attr');equal(alpha.compareDocumentPosition(alpha),0,'same-detached-attr');
    root.setAttributeNode(alpha);equal(alpha.compareDocumentPosition(beta),34,'attr-reinsert-list-order');
    const freeAttr=document.createAttribute('free');disconnected(freeAttr,alpha,'ownerless-attr');
    const host=document.createElement('div'),shadow=host.attachShadow({mode:'closed'}),shadowChild=document.createElement('b');
    shadow.appendChild(shadowChild);root.appendChild(host);
    equal(shadow.compareDocumentPosition(shadowChild),20,'shadow-internal-descendant');
    equal(shadowChild.compareDocumentPosition(shadow),10,'shadow-internal-ancestor');
    disconnected(host,shadowChild,'shadow-host-not-parent');disconnected(document,shadow,'shadow-separate-root');
    const template=document.createElement('template'),templateChild=document.createElement('em');
    template.content.appendChild(templateChild);root.appendChild(template);
    equal(template.content.compareDocumentPosition(templateChild),20,'template-content-descendant');
    disconnected(template,templateChild,'template-host-not-parent');
    // Native comparison must not evaluate author-overridable tree getters.
    Object.defineProperty(first,'parentNode',{configurable:true,get(){throw new Error('Author parentNode getter read');}});
    Object.defineProperty(last,'ownerDocument',{configurable:true,get(){throw new Error('Author ownerDocument getter read');}});
    equal(first.compareDocumentPosition(last),4,'native-parent-and-owner');
    delete first.parentNode;delete last.ownerDocument;
    throws(()=>compare.call({},root),'receiver-brand');throws(()=>compare.call(Object.create(Node.prototype),root),'forged-receiver');
    throws(()=>compare.call(null,root),'null-receiver');throws(()=>root.compareDocumentPosition(),'missing-argument');
    throws(()=>root.compareDocumentPosition(null),'null-argument');throws(()=>root.compareDocumentPosition(undefined),'undefined-argument');
    throws(()=>root.compareDocumentPosition({nodeType:1}),'object-argument');throws(()=>root.compareDocumentPosition(new Proxy(first,{})),'proxy-argument');
    const frame=document.createElement('iframe');document.body.appendChild(frame);
    try{
        const child=frame.contentDocument,childNode=child.createElement('p');child.body.appendChild(childNode);
        disconnected(document,child,'frame-document-root');
        disconnected(root,childNode,'cross-realm-root');
        equal(compare.call(child,childNode),20,'cross-realm-native-brand');
        const adopted=child.adoptNode(first);
        equal(first.compareDocumentPosition(adopted),0,'cross-realm-native-identity');
        child.body.appendChild(adopted);equal(childNode.compareDocumentPosition(adopted),4,'cross-realm-adopt-order');
    }finally{frame.remove();root.remove();}
    return count;
};
