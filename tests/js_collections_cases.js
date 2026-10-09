/* Supporting native web_live regression, not public-site acceptance. */
async function runCollectionCases(){
    let checks=0;
    const eq=(actual,expected,label)=>{checks++;check('collections-'+label,Object.is(actual,expected));if(!Object.is(actual,expected))throw Error(label+': '+String(actual)+' != '+String(expected));};
    const make=(tag,parent,owner=document)=>{const n=owner.createElement(tag);if(parent)parent.appendChild(n);return n;};
    const root=make('section',document.body),a=make('span',root),b=make('span',root);
    a.className='collection-probe first';a.id='collection-first';a.setAttribute('name','collection-name');
    b.className='collection-probe';b.setAttribute('name','collection-name');
    const tags=root.getElementsByTagName('span'),classes=root.getElementsByClassName('collection-probe');
    const names=document.getElementsByName('collection-name'),nodes=root.childNodes,elements=root.children;
    const snapshot=root.querySelectorAll('span');
    eq(tags instanceof HTMLCollection,true,'tag-brand');eq(classes instanceof HTMLCollection,true,'class-brand');
    eq(names instanceof NodeList,true,'name-brand');eq(nodes,root.childNodes,'child-nodes-same');eq(elements,root.children,'children-same');
    for(let i=0;i<12;i++){eq(tags.length,2,'tag-loop-length-'+i);eq(tags[i%2],i%2?b:a,'tag-loop-index-'+i);}
    eq(tags.namedItem('collection-first'),a,'named-id');eq(tags['collection-first'],a,'named-property');
    a.id='collection-renamed';eq(tags.namedItem('collection-first'),null,'named-old-id');eq(tags.namedItem('collection-renamed'),a,'named-new-id');
    a.classList.remove('collection-probe');eq(classes.length,1,'class-removed');eq(classes[0],b,'class-order');
    a.classList.add('collection-probe');eq(classes.length,2,'class-readded');
    a.removeAttribute('name');eq(names.length,1,'name-removed');eq(names[0],b,'name-order');
    a.setAttribute('name','collection-name');eq(names.length,2,'name-readded');
    const text=document.createTextNode('text');root.insertBefore(text,b);
    eq(nodes.length,3,'child-text-length');eq(nodes[1],text,'child-text-index');eq(elements.length,2,'children-skip-text');
    b.remove();eq(tags.length,1,'tag-removed');eq(nodes.length,2,'child-removed');eq(names.length,1,'document-name-removed');
    root.insertBefore(b,a);eq(tags[0],b,'tag-reinsert-order');eq(elements[0],b,'children-reinsert-order');eq(names[0],b,'name-reinsert-order');
    eq(snapshot.length,2,'query-selector-static-length');eq(snapshot[0],a,'query-selector-static-order');
    root.remove();eq(names.length,0,'detached-document-query');eq(tags.length,2,'detached-root-query');
    const c=make('span',root);c.className='collection-probe';eq(tags.length,3,'detached-insertion');eq(classes.length,3,'detached-class');
    document.body.appendChild(root);eq(names.length,2,'reconnected-document-query');eq(tags[2],c,'reconnected-root-query');
    const detached=document.implementation.createHTMLDocument('collections');
    const ownNames=detached.getElementsByName('collection-name');eq(ownNames.length,0,'inactive-name-empty');
    detached.adoptNode(root);eq(root.ownerDocument,detached,'adoption-owner');eq(names.length,0,'adoption-old-document');
    eq(tags.length,3,'adoption-retained-tags');eq(nodes.length,4,'adoption-retained-children');
    detached.body.appendChild(root);eq(ownNames.length,2,'adoption-destination-name');
    c.remove();eq(tags.length,2,'adoption-new-owner-mutation');eq(classes.length,2,'adoption-class-mutation');
    Object.defineProperty(root,'ownerDocument',{configurable:true,value:document});
    const d=make('span',root,detached);eq(tags.length,3,'page-owner-spoof-does-not-key-cache');delete root.ownerDocument;
    document.body.appendChild(root);eq(root.ownerDocument,document,'implicit-adopt-back');eq(ownNames.length,0,'destination-name-after-adopt-back');
    eq(names.length,2,'source-name-after-adopt-back');d.remove();eq(tags.length,2,'back-owner-mutation');
    const host=make('div',document.body),shadow=host.attachShadow({mode:'open'});
    const light=make('span',host),inner=make('span',shadow);light.className=inner.className='collection-shadow';
    const shadowNodes=shadow.childNodes,shadowElements=shadow.children,lightTags=host.getElementsByTagName('span');
    eq(shadowNodes.length,1,'shadow-initial');eq(shadowElements[0],inner,'shadow-initial-element');eq(lightTags.length,1,'shadow-query-boundary');
    inner.remove();eq(shadowNodes.length,0,'shadow-removal');eq(shadowElements.length,0,'shadow-elements-removal');
    shadow.appendChild(inner);eq(shadowNodes[0],inner,'shadow-reinsert');eq(lightTags[0],light,'shadow-still-excluded');
    detached.adoptNode(host);eq(shadow.ownerDocument,detached,'shadow-adoption-owner');
    make('b',shadow,detached);eq(shadowNodes.length,2,'shadow-adoption-mutation');eq(shadowElements.length,2,'shadow-adoption-element-mutation');
    // forEach captures length, but index reads remain live within the callback.
    const iterate=make('div',document.body),x=make('i',iterate),y=make('i',iterate),z=make('i',iterate);
    const seen=[];iterate.childNodes.forEach((n,i)=>{seen.push(n);if(i===0)y.remove();});
    eq(seen.length,2,'for-each-mutation-length');eq(seen[0],x,'for-each-first');eq(seen[1],z,'for-each-next-live');
    // Selectedness is control state, not a DOM revision. Generic HTML collection
    // readers must keep seeing it without an attribute/structure mutation.
    const select=make('select',root),o1=make('option',select),o2=make('option',select);o1.text='one';o2.text='two';
    const selected=select.selectedOptions;select.selectedIndex=0;eq(selected[0],o1,'selected-before');
    select.selectedIndex=1;eq(selected.length,1,'selected-count-after');eq(selected[0],o2,'selected-state-not-cached');
    const form=make('form',root),field=make('input',form);field.name='collection-field';
    const controls=form.elements;eq(controls.length,1,'form-initial');field.name='renamed-field';eq(controls.namedItem('renamed-field'),field,'form-name-live');
    // Independent same-origin frame owners must not borrow the top revision.
    const frame=make('iframe',document.body),child=frame.contentDocument;
    eq(child!==null,true,'same-origin-frame-document');
    const childRoot=make('div',child.body,child),childA=make('span',childRoot,child);
    childA.className='collection-child';childA.setAttribute('name','child-name');
    const childTags=childRoot.getElementsByTagName('span'),childClasses=childRoot.getElementsByClassName('collection-child');
    const childNames=child.getElementsByName('child-name'),childNodes=childRoot.childNodes;
    eq(childTags.length,1,'frame-initial-tag');eq(childClasses[0],childA,'frame-initial-class');eq(childNames[0],childA,'frame-initial-name');eq(childNodes[0],childA,'frame-initial-child');
    const peerFrame=make('iframe',document.body),peer=peerFrame.contentDocument;
    const peerRoot=make('div',peer.body,peer),peerA=make('span',peerRoot,peer);
    peerA.className='collection-child';peerA.setAttribute('name','child-name');
    const peerTags=peerRoot.getElementsByTagName('span'),peerNodes=peerRoot.childNodes;
    eq(peerTags.length,1,'peer-frame-initial');eq(peerTags[0],peerA,'peer-frame-identity');
    make('span',peerRoot,peer);eq(peerTags.length,2,'peer-frame-own-mutation');eq(peerNodes.length,2,'peer-frame-own-children');
    eq(childTags.length,1,'peer-frame-does-not-substitute-child-results');eq(childNodes[0],childA,'peer-frame-does-not-substitute-child-identity');
    make('span',root);eq(childTags.length,1,'top-mutation-frame-stable');
    const childB=make('span',childRoot,child);childB.className='collection-child';
    eq(childTags.length,2,'frame-own-insertion');eq(childNodes.length,2,'frame-own-children');eq(childClasses.length,2,'frame-own-class-insertion');
    childA.removeAttribute('name');eq(childNames.length,0,'frame-own-name-mutation');
    childA.className='other';eq(childClasses.length,1,'frame-own-class-mutation');eq(childClasses[0],childB,'frame-own-class-order');
    document.adoptNode(childRoot);root.appendChild(childRoot);eq(childTags.length,2,'frame-root-adoption-retained');
    childB.remove();eq(childTags.length,1,'frame-root-new-owner-removal');eq(childNodes.length,1,'frame-root-new-owner-child');
    const parserRoot=make('div',root),parserTags=parserRoot.getElementsByTagName('b'),parserChildren=parserRoot.children;
    eq(parserTags.length,0,'parser-before');parserRoot.innerHTML='<b>one</b><b>two</b>';
    eq(parserTags.length,2,'parser-insertion-live');eq(parserChildren.length,2,'parser-children-live');
    parserRoot.innerHTML='<i>replacement</i>';eq(parserTags.length,0,'parser-replacement-live');eq(parserChildren[0].tagName,'I','parser-replacement-type');
    peerFrame.remove();frame.remove();root.remove();host.remove();iterate.remove();
    return checks;
}
