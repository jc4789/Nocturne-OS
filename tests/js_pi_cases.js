/* Uses the native product DOM. These are supporting parser/DOM regressions,
 * not substitutes for requested visible real-site and scrolling tests. */
globalThis.runProcessingInstructionCases=async function(){
    let count=0;
    const equal=(a,b,label)=>{count++;if(!Object.is(a,b))throw new Error((label||'PI')+': '+String(a)+' != '+String(b));};
    const throws=(fn,name,label)=>{count++;try{fn();}catch(e){if(e.name===name)return;throw e;}throw new Error((label||'PI')+': expected '+name);};
    const make=data=>document.createProcessingInstruction('nocturne-pi',data);
    const pi=make(' First="one" first=\'two\' empty="" decoded="&amp;&lt;&gt;&quot;&apos;&#65;&#x1F600;" ');
    equal(pi.nodeType,7);equal(pi.target,'nocturne-pi');equal(pi instanceof ProcessingInstruction,true);
    equal(pi.hasAttributes(),true);equal(pi.getAttributeNames().join('|'),'First|first|empty|decoded');
    equal(pi.getAttribute('First'),'one');equal(pi.getAttribute('first'),'two');equal(pi.getAttribute('FIRST'),null);
    equal(pi.hasAttribute('empty'),true);equal(pi.getAttribute('empty'),'');equal(pi.hasAttribute('absent'),false);
    equal(pi.getAttribute('decoded'),'&<>"\'A😀');
    const names=pi.getAttributeNames();names[0]='modified';equal(pi.getAttributeNames()[0],'First');
    let inheritedSetterCalls=0;
    const prior=Object.getOwnPropertyDescriptor(Object.prototype,'0');let ownNames;
    try {
        Object.defineProperty(Object.prototype,'0',{configurable:true,set(){inheritedSetterCalls++;pi.data='hijacked="yes"';}});
        ownNames=pi.getAttributeNames();
    } finally {
        if(prior)Object.defineProperty(Object.prototype,'0',prior);else delete Object.prototype[0];
    }
    equal(inheritedSetterCalls,0,'names use own data, not inherited setters');equal(ownNames[0],'First');
    equal(pi.attributes,undefined);equal(pi.getAttributeNode,undefined);
    for(const invalid of [
        'a="ok" b=unquoted','a="ok" b="unterminated','a="ok" b="&unknown;"',
        'a="ok" a="duplicate"','a="ok"b="unseparated"','a="ok" b="&#0;"',
        'a="ok" b="&#xD800;"','a="ok" b="&#x110000;"','a="ok" b="&amp"',
        'a="ok" b="<"','a="ok" 0name="not-an-XML-Name"'
    ]) {
        const bad=make(invalid);equal(bad.hasAttributes(),false);equal(bad.getAttributeNames().length,0);
        equal(bad.getAttribute('a'),null);equal(bad.hasAttribute('a'),false);
    }
    const proto=ProcessingInstruction.prototype;
    equal(proto.toggleAttribute.length,1);equal(proto.setAttribute.length,2);equal(proto.getAttributeNames.length,0);
    for(const method of ['hasAttributes','getAttributeNames','getAttribute','hasAttribute','setAttribute','removeAttribute','toggleAttribute']) {
        const args=method==='setAttribute'?['x','y']:['x'];
        for(const receiver of [{},null,document.createTextNode('text'),document.createElement('div'),Object.create(proto)])
            throws(()=>Reflect.apply(proto[method],receiver,args),'TypeError',method+' brand');
        const descriptor=Object.getOwnPropertyDescriptor(proto,method);
        equal(descriptor.enumerable,true);equal(descriptor.configurable,true);equal(descriptor.writable,true);
    }
    for(const method of ['getAttribute','hasAttribute','setAttribute','removeAttribute','toggleAttribute'])
        throws(()=>pi[method](),'TypeError',method+' argument');
    throws(()=>pi.setAttribute('x'),'TypeError');
    for(const method of ['getAttribute','hasAttribute','setAttribute','removeAttribute','toggleAttribute'])
        throws(()=>pi[method](Symbol('name'),'value'),'TypeError',method+' DOMString');
    throws(()=>pi.setAttribute('x',Symbol('value')),'TypeError');
    let converted=0;
    throws(()=>Reflect.apply(proto.setAttribute,{},[{toString(){converted++;return 'x';}},'v']),'TypeError');
    equal(converted,0,'brand before conversion');
    const changed=make('a=\'old\' b="two"');
    equal(changed.setAttribute('a','new & < > " \' &amp;'),undefined);
    equal(changed.data,'a="new &amp; &lt; &gt; &quot; \' &amp;amp;" b="two"');
    equal(changed.getAttribute('a'),'new & < > " \' &amp;');
    equal(changed.getAttributeNames().join('|'),'a|b','update preserves key order');
    changed.setAttribute('C',null);changed.setAttribute('d',undefined);
    equal(changed.getAttribute('C'),'null');equal(changed.getAttribute('d'),'undefined');
    changed.removeAttribute('a');changed.setAttribute('a','again');
    equal(changed.getAttributeNames().join('|'),'b|C|d|a','remove and re-add appends');
    const toggle=make('keep=\'value\'');
    equal(toggle.toggleAttribute('keep',true),true);equal(toggle.data,"keep='value'",'forced existing is no-op');
    equal(toggle.toggleAttribute('missing',false),false);equal(toggle.data,"keep='value'",'forced absent is no-op');
    equal(toggle.toggleAttribute('added'),true);equal(toggle.getAttribute('added'),'');
    equal(toggle.toggleAttribute('added',undefined),false);equal(toggle.hasAttribute('added'),false);
    equal(toggle.toggleAttribute('keep',0),false);equal(toggle.hasAttributes(),false);equal(toggle.data,'');
    equal(toggle.toggleAttribute('x',{}),true);equal(toggle.getAttribute('x'),'');
    for(const name of ['', 'a b','a\tb','a\nb','a\rb','a\fb','a/b','a=b','a>b','a\0b']) {
        const before=toggle.data;
        throws(()=>toggle.setAttribute(name,'v'),'InvalidCharacterError');
        throws(()=>toggle.toggleAttribute(name,true),'InvalidCharacterError');
        equal(toggle.data,before);equal(toggle.hasAttribute(name),false);equal(toggle.getAttribute(name),null);
        equal(toggle.removeAttribute(name),undefined,'remove does not validate names');
    }
    // DOM's setter uses valid attribute local name, not XML Name. Its ordered
    // map is preserved across attribute-origin serialization, even if the
    // resulting data could not be parsed by XML pseudo-attribute grammar.
    const preserved=make('xml="ok"');
    for(const name of ['0name','<name','?name',':', '名前'])preserved.setAttribute(name,'v');
    equal(preserved.getAttributeNames().join('|'),'xml|0name|<name|?name|:|名前');
    equal(preserved.getAttribute('0name'),'v');equal(preserved.getAttribute('<name'),'v');
    preserved.setAttribute('nul','a\0b');equal(preserved.getAttribute('nul'),'a\0b');
    preserved.removeAttribute('xml');equal(preserved.getAttribute('<name'),'v','remove preserves map');
    const same=preserved.data;preserved.data=same;
    equal(preserved.hasAttributes(),false,'same-value CharacterData re-parses invalid data');
    preserved.data='new="one"';equal(preserved.getAttribute('new'),'one');
    preserved.nodeValue='value="two"';equal(preserved.hasAttribute('new'),false);equal(preserved.getAttribute('value'),'two');
    preserved.textContent='text="three"';equal(preserved.getAttribute('text'),'three');
    preserved.replaceData(0,preserved.length,'replace="four"');equal(preserved.getAttribute('replace'),'four');
    preserved.appendData(' tail="five"');equal(preserved.getAttribute('tail'),'five');
    const reentrant=make('old="one"');
    reentrant.setAttribute({toString(){reentrant.data='during="two"';return 'after';}},'three');
    equal(reentrant.getAttributeNames().join('|'),'during|after');
    equal(reentrant.getAttribute('old'),null);equal(reentrant.getAttribute('after'),'three');
    const ordinary=make('name="value"'),copy=ordinary.cloneNode();
    equal(copy!==ordinary,true);equal(copy.target,ordinary.target);equal(copy.data,ordinary.data);equal(copy.getAttribute('name'),'value');
    copy.setAttribute('name','clone');equal(ordinary.getAttribute('name'),'value');
    const parsed=new DOMParser().parseFromString('<?marker name="from-parser">','text/html');
    const parsedPi=Array.from(parsed.childNodes).find(n=>n.nodeType===7);
    equal(!!parsedPi,true);equal(parsedPi.getAttribute('name'),'from-parser');
    // All PI updates must retain native CharacterData mutation/Range paths.
    const holder=document.createElement('div'),observed=make('a=\'x\'');
    holder.appendChild(observed);document.body.appendChild(holder);
    const observer=new MutationObserver(()=>{}),range=document.createRange();
    try {
        observer.observe(observed,{characterData:true,characterDataOldValue:true});
        range.setStart(observed,1);range.setEnd(observed,3);
        observed.setAttribute('a','long');
        equal(range.startContainer,observed);equal(range.endContainer,observed);
        equal(range.startOffset,0);equal(range.endOffset,0);
        observed.removeAttribute('absent');observed.toggleAttribute('a',true);observed.toggleAttribute('missing',false);
        const records=observer.takeRecords();equal(records.length,2,'normalization records and toggle no-op');
        equal(records[0].type,'characterData');equal(records[0].target,observed);equal(records[0].oldValue,"a='x'");
        equal(records[1].oldValue,'a="long"');equal(records[0].attributeName,null);
        const imported=document.implementation.createHTMLDocument('').importNode(observed,false);
        equal(imported.getAttribute('a'),'long');equal(imported.ownerDocument!==document,true);
    } finally {observer.disconnect();range.detach();holder.remove();}
    await Promise.resolve();return count;
};
