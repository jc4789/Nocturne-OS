/* Native independent-document contracts. Supplemental checks, not site acceptance. */
globalThis.runDocumentCases=function(){
    let count=0;
    function eq(actual,expected,label){count++;if(actual!==expected)throw Error('document: '+label+' '+String(actual)+' != '+String(expected));}
    function raises(fn,name,label){let error;try{fn();}catch(e){error=e;}eq(error&&error.name,name,label);}
    const parser=new DOMParser(),url=document.URL;
    eq(document.referrer,'','navigation without referrer');
    const referrerGet=Object.getOwnPropertyDescriptor(Document.prototype,'referrer').get;
    raises(()=>referrerGet.call({}),'TypeError','referrer receiver brand');
    globalThis.__documentScriptProbe=0;
    const a=parser.parseFromString('<!doctype html><!--before--><title>独立</title><body><p id="own">A &amp; B</p><script>__documentScriptProbe++<\/script><img src="never-fetch.png"><button onclick="__documentScriptProbe++">button</button>','text/html');
    eq(a===document,false,'independent identity');eq(a instanceof Document,true,'Document interface');eq(a instanceof HTMLDocument,true,'HTMLDocument interface');
    eq(a.ownerDocument,null,'document has no owner');eq(a.defaultView,null,'no window');eq(a.location,null,'no location');eq(a.currentScript,null,'no current script');
    eq(a.referrer,'','inactive document has no referrer');
    eq(a.URL,url,'inherited URL');eq(a.documentURI,url,'inherited documentURI');eq(a.contentType,'text/html','MIME');eq(a.characterSet,'UTF-8','encoding');eq(a.compatMode,'CSS1Compat','doctype mode');
    eq(a.title,'独立','title');eq(a.doctype instanceof DocumentType,true,'DocumentType interface');eq(a.doctype.name,'html','doctype name');eq(a.firstChild,a.doctype,'doctype tree position');eq(a.childNodes[1].nodeType,8,'comment retained');
    const p=a.getElementById('own');eq(p,a.querySelector('#own'),'wrapper identity');eq(p.ownerDocument,a,'element owner');eq(p.firstChild.ownerDocument,a,'text owner');eq(p.isConnected,true,'inert document connectivity');
    eq(p.textContent,'A & B','actual parsing');eq(p.cloneNode(true).ownerDocument,a,'clone same owner');eq(__documentScriptProbe,0,'parser scripts inert');
    a.querySelector('button').click();eq(__documentScriptProbe,0,'inline handler inert');eq(a.querySelector('button').onclick,null,'inline getter inert');
    const b=parser.parseFromString('<base href="https://example.test/base/"><a href="child">link</a>','text/html');
    eq(b.querySelector('a').href,'https://example.test/base/child','independent base resolution');eq(b.baseURI,'https://example.test/base/','base URI');
    const text=b.createTextNode('独立text'),comment=b.createComment('note'),fragment=b.createDocumentFragment();
    eq(text.ownerDocument,b,'create text owner');eq(comment.ownerDocument,b,'create comment owner');eq(fragment.ownerDocument,b,'create fragment owner');
    fragment.append(text,comment);eq(fragment.firstChild,text,'fragment mutation');eq(fragment.isConnected,false,'detached fragment');
    b.body.appendChild(fragment);eq(fragment.childNodes.length,0,'fragment emptied');eq(text.parentNode,b.body,'fragment actual insertion');
    const imported=b.importNode(p,true);eq(imported===p,false,'import new identity');eq(imported.ownerDocument,b,'import owner');eq(imported.firstChild.ownerDocument,b,'import descendants');eq(p.ownerDocument,a,'source unchanged');
    b.body.appendChild(imported);const adopted=a.adoptNode(imported);eq(adopted,imported,'adopt identity');eq(imported.parentNode,null,'adopt detaches');eq(imported.ownerDocument,a,'adopt owner');eq(imported.firstChild.ownerDocument,a,'adopt descendants');
    a.body.appendChild(imported);eq(imported.parentNode,a.body,'adopted insertion');
    const deep=a.cloneNode(true),shallow=a.cloneNode(false);eq(deep===a,false,'document clone identity');eq(deep.defaultView,null,'document clone inert');eq(deep.URL,a.URL,'clone URL');eq(deep.getElementById('own').ownerDocument,deep,'document clone owners');eq(shallow.childNodes.length,0,'shallow document empty');
    eq(a.implementation,a.implementation,'same implementation');eq(a.implementation===b.implementation,false,'implementation per document');
    const made=a.implementation.createHTMLDocument('新しい');eq(made.URL,'about:blank','created document URL');eq(made.title,'新しい','created title');eq(made.doctype.name,'html','created doctype');eq(made.head.ownerDocument,made,'created owner');
    const untitled=a.implementation.createHTMLDocument();eq(untitled.querySelector('title'),null,'omitted title');
    const template=document.createElement('template');eq(template instanceof HTMLTemplateElement,true,'template interface');eq(template.content instanceof DocumentFragment,true,'content fragment');eq(template.content,template.content,'content same object');eq(template.content.ownerDocument===document,false,'inert template owner');eq(template.content.ownerDocument.defaultView,null,'template owner inactive');
    template.innerHTML='<span id="inside">payload</span><template><b>nested</b></template>';
    eq(template.childNodes.length,0,'template children separate');eq(template.querySelector('#inside'),null,'query does not cross content');eq(template.content.querySelector('#inside').textContent,'payload','content parsed');eq(template.textContent,'','template text excludes content');
    eq(template.content.querySelector('template').content.firstChild.localName,'b','nested content parsed');
    const tcopy=template.cloneNode(true),tshallow=template.cloneNode(false);eq(tcopy.content===template.content,false,'clone distinct fragment');eq(tcopy.content.firstChild===template.content.firstChild,false,'clone distinct child');eq(tcopy.content.firstChild.ownerDocument,tcopy.content.ownerDocument,'clone content owner');eq(tshallow.content.childNodes.length,0,'shallow content empty');
    const tin=b.importNode(template,true);eq(tin.ownerDocument,b,'template import owner');eq(tin.content.ownerDocument===b,false,'import distinct content owner');eq(tin.content.firstChild.ownerDocument,tin.content.ownerDocument,'import content descendants');
    const oldcontent=tin.content;a.adoptNode(tin);eq(tin.content,oldcontent,'template adopt fragment identity');eq(tin.ownerDocument,a,'template adopted owner');eq(tin.content.ownerDocument===a,false,'template adopted content separate');eq(tin.content.firstChild.ownerDocument,tin.content.ownerDocument,'template adopt descendant owner');
    const inserted=b.createElement('i');template.content.appendChild(inserted);eq(inserted.ownerDocument,template.content.ownerDocument,'content append adoption');eq(template.content.innerHTML,undefined,'fragment has no innerHTML API');
    let cycle=false;try{template.content.appendChild(template);}catch(_){cycle=true;}eq(cycle,true,'host-inclusive cycle rejected');
    const source=parser.parseFromString('<template id="parsed"><em>parsed</em></template>','text/html');eq(source.getElementById('parsed').content.firstChild.localName,'em','parser content retained');eq(source.querySelector('em'),null,'document query excludes template');
    raises(()=>parser.parseFromString('','text/plain'),'TypeError','invalid MIME');raises(()=>parser.parseFromString('','application/xml'),'NotSupportedError','XML explicit unsupported');raises(()=>DOMParser.prototype.parseFromString.call({},'','text/html'),'TypeError','parser brand');raises(()=>parser.parseFromString(''),'TypeError','required MIME');raises(()=>parser.parseFromString(Symbol(),'text/html'),'TypeError','DOMString Symbol');
    raises(()=>a.importNode(document),'NotSupportedError','import document rejected');raises(()=>a.adoptNode(document),'NotSupportedError','adopt document rejected');raises(()=>Document.prototype.createTextNode.call({},'x'),'TypeError','document brand');raises(()=>a.write('<p>bad'),'NotSupportedError','inert write rejected');
    eq(__documentScriptProbe,0,'no parsed script or inline execution');delete globalThis.__documentScriptProbe;
    const liveText=Document.prototype.createTextNode.call(document,'live'),inertText=Document.prototype.createTextNode.call(a,'inert');
    eq(liveText.ownerDocument,document,'borrow create live');eq(inertText.ownerDocument,a,'borrow create inert');
    const liveElement=Document.prototype.createElement.call(document,'section'),inertElement=Document.prototype.createElement.call(a,'section');
    eq(liveElement.ownerDocument,document,'borrow element live');eq(inertElement.ownerDocument,a,'borrow element inert');
    eq(Document.prototype.importNode.call(document,p,true).ownerDocument,document,'borrow import live');
    eq(Document.prototype.importNode.call(b,p,true).ownerDocument,b,'borrow import inert');
    for(const [key,value] of [['defaultView',window],['location',location],['readyState',document.readyState],['URL',document.URL]]){
        const getter=Object.getOwnPropertyDescriptor(Document.prototype,key).get;
        eq(getter.call(document),value,'borrow live getter '+key);raises(()=>getter.call({}),'TypeError','borrow invalid getter '+key);
    }
    for(const name of ['createElement','createElementNS','createTextNode','createComment','createDocumentFragment','getElementById','getElementsByName','importNode','adoptNode','createEvent']){
        const args=name==='createElementNS'?['http://www.w3.org/1999/xhtml','div']:name==='importNode'||name==='adoptNode'?[p]:name==='createEvent'?['Event']:['x'];
        raises(()=>Reflect.apply(Document.prototype[name],{},args),'TypeError','borrow invalid '+name);
    }
    const ownerGetter=Object.getOwnPropertyDescriptor(Node.prototype,'ownerDocument').get;
    raises(()=>ownerGetter.call({}),'TypeError','owner getter invalid');
    const input=a.createElement('input');input.value='original';
    const initial=input.ownerDocument;eq(initial,a,'control initial owner');
    b.adoptNode(input);input.value='更新🙂';eq(input.ownerDocument,b,'control adopted owner');eq(input.value,'更新🙂','adopted value update');
    document.adoptNode(input);input.value='live updated';eq(input.ownerDocument,document,'control adopted live');eq(input.value,'live updated','live adopted value update');
    a.adoptNode(input);input.value='back';eq(input.ownerDocument,a,'control adopted back');eq(input.value,'back','control final value');
    const controlClone=b.importNode(input,true);eq(controlClone.value,'back','import control value');controlClone.value='independent';eq(input.value,'back','clone value independent');
    const nested=template.content.querySelector('template');eq(nested.content.ownerDocument,template.content.ownerDocument,'nested reuses template owner');
    const nestedContent=nested.content,oldOwner=nestedContent.ownerDocument;
    b.adoptNode(template);eq(template.content.querySelector('template'),nested,'nested adopt node identity');eq(nested.content,nestedContent,'nested adopt fragment identity');eq(nested.content.ownerDocument,template.content.ownerDocument,'nested adopted owner');eq(nested.content.ownerDocument===oldOwner,false,'nested changes owner');
    let nestedCycle=false;try{nested.content.appendChild(template);}catch(_){nestedCycle=true;}eq(nestedCycle,true,'nested host-inclusive cycle');
    eq(template.content.firstChild.localName,'span','rejected nested cycle preserves content');
    const persistent=a.createTextNode('persistent');document.adoptNode(persistent);persistent.nodeValue='updated after cross-arena adoption';
    eq(persistent.ownerDocument,document,'cross-arena text owner');eq(persistent.nodeValue,'updated after cross-arena adoption','cross-arena text update');
    const watched=a.createElement('div'),watchedChild=a.createElement('small');watched.appendChild(watchedChild);
    const mutationObserver=new MutationObserver(()=>{});mutationObserver.observe(watched,{childList:true});b.adoptNode(watchedChild);
    const removed=mutationObserver.takeRecords();eq(removed.length,1,'adopt emits removal');eq(removed[0].target,watched,'adopt mutation target');eq(removed[0].removedNodes[0],watchedChild,'adopt mutation identity');mutationObserver.disconnect();
    const contentObserver=new MutationObserver(()=>{});contentObserver.observe(tcopy.content,{childList:true});tcopy.innerHTML='<u>replacement</u>';
    const contentRecords=contentObserver.takeRecords();eq(contentRecords.length,1,'template setter emits content mutation');eq(contentRecords[0].target,tcopy.content,'template content mutation target');eq(contentRecords[0].addedNodes[0].localName,'u','template content mutation child');contentObserver.disconnect();
    const svgTemplate=document.createElementNS('http://www.w3.org/2000/svg','template'),svgChild=document.createElementNS('http://www.w3.org/2000/svg','g');
    svgTemplate.appendChild(svgChild);eq(svgTemplate instanceof HTMLTemplateElement,false,'SVG template not HTML interface');eq(svgTemplate.content,undefined,'SVG template no HTML content');eq(svgTemplate.firstChild,svgChild,'SVG template ordinary child');eq(svgTemplate.innerHTML,'<g></g>','SVG template serializes children');
    const svgCopy=svgTemplate.cloneNode(true);eq(svgCopy.firstChild.localName,'g','SVG template clone child');eq(svgCopy.innerHTML,'<g></g>','SVG template clone serialization');
    const svgImport=b.importNode(svgTemplate,true);eq(svgImport.innerHTML,'<g></g>','SVG template import children');eq(svgImport.firstChild.ownerDocument,b,'SVG template imported owner');
    const parsedScript=a.querySelector('script');globalThis.__documentScriptProbe=0;document.body.appendChild(parsedScript);eq(globalThis.__documentScriptProbe,0,'parsed script insertion stays inert');parsedScript.remove();delete globalThis.__documentScriptProbe;
    return count;
};

/* Run each mode in a FRESH native document fixture, then destroy that fixture.
   Deliberately reaches the real DOM limit; not suitable for the shared API fixture. */
globalThis.runDocumentLimitCases=function(mode){
    let count=0;function eq(a,b,label){count++;if(a!==b)throw Error('document limit: '+label);}
    const liveRoot=document.documentElement,liveBody=document.body;
    const safe=new DOMParser().parseFromString('<!doctype html><p id="kept">sentinel</p><input id="value">','text/html');
    const kept=safe.getElementById('kept'),control=safe.getElementById('value');control.value='safe';
    let succeeded=0,rejected=false;
    if(mode==='count'){
        for(let i=0;i<100;i++){try{safe.implementation.createHTMLDocument();succeeded++;}catch(_){rejected=true;break;}}
        eq(succeeded,63,'exact 64 independent document limit (one safe plus 63 created)');
    }else if(mode==='arena'){
        const payload='x'.repeat(1024*1024);
        for(let i=0;i<40;i++){try{safe.createTextNode(payload);succeeded++;}catch(_){rejected=true;break;}}
        eq(succeeded>0 && succeeded<32,true,'finite aggregate DOM arena');
    }else throw Error('Unknown document limit mode');
    console.log('Document limit mode '+mode+' successful allocations '+succeeded);
    eq(rejected,true,'native limit actually rejected');eq(safe.getElementById('kept'),kept,'old wrapper preserved');eq(kept.textContent,'sentinel','old text preserved');eq(control.value,'safe','old control preserved');
    document.adoptNode(control);control.value='OK';eq(control.ownerDocument,document,'adopt after limit');eq(control.value,'OK','value update after limit');
    eq(document.documentElement,liveRoot,'main root preserved');eq(document.body,liveBody,'main body preserved');
    safe.adoptNode(control);eq(control.value,'OK','adopt back after limit');
    return count;
};
