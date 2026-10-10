/* Native metadata interface/reflection/resource regressions for the combined
 * jstest web_live run. No CSSOM, media playback or public-site claims. */
async function runHTMLElementCases() {
    let count=0;
    const ok=(value,name)=>{count++;check('html-elements-'+name,!!value);if(!value)throw new Error(name);};
    const equal=(a,b,name)=>ok(Object.is(a,b),name);
    const rejects=(fn,name)=>{let e;try{fn();}catch(error){e=error;}ok(e instanceof TypeError,'reject-'+name);};
    const interfaces=[['meta',HTMLMetaElement],['link',HTMLLinkElement],['style',HTMLStyleElement],
        ['base',HTMLBaseElement],['title',HTMLTitleElement],['head',HTMLHeadElement]];
    const source='<html><head><base href="https://metadata.fixture/one/"><title>Parsed title</title>'+
        '<meta name="description" content="Parsed metadata"><link rel="stylesheet" href="parsed.css">'+
        '<style>.parsed{width:17px}</style></head><body></body></html>';
    const parsed=new DOMParser().parseFromString(source,'text/html');
    for(const [tag,C] of interfaces){
        const created=document.createElement(tag),found=parsed.querySelector(tag);
        ok(created instanceof C && created instanceof HTMLElement,tag+'-native-created-brand');
        equal(Object.getPrototypeOf(created),C.prototype,tag+'-actual-prototype');
        equal(Object.getPrototypeOf(C.prototype),HTMLElement.prototype,tag+'-inheritance');
        equal(Object.prototype.toString.call(created),'[object '+C.name+']',tag+'-tag');
        ok(found instanceof C,tag+'-parser-native-brand');
        ok(document.createElementNS('http://www.w3.org/1999/xhtml',tag) instanceof C,tag+'-html-namespace-brand');
        equal(document.createElementNS('http://www.w3.org/2000/svg',tag) instanceof C,false,tag+'-no-svg-brand');
        equal(document.createElement('div') instanceof C,false,tag+'-no-div-brand');
        rejects(()=>new C(),tag+'-constructor');rejects(()=>new(class extends C {})(),tag+'-subclass-constructor');
        const clone=found.cloneNode(true);ok(clone instanceof C && clone!==found,tag+'-native-clone');equal(clone.outerHTML,found.outerHTML,tag+'-clone-content');
        const imported=document.importNode(found,true);ok(imported instanceof C && imported.ownerDocument===document,tag+'-native-import');
    }
    ok(document.head instanceof HTMLHeadElement,'live-head-native-brand');
    const meta=document.createElement('meta');
    for(const [property,attribute] of [['name','name'],['httpEquiv','http-equiv'],['content','content'],['media','media'],['scheme','scheme']]){
        equal(meta[property],'',property+'-missing');meta[property]='native '+property;
        equal(meta.getAttribute(attribute),'native '+property,property+'-native-set');
        meta.setAttribute(attribute,'attribute '+property);equal(meta[property],'attribute '+property,property+'-live-get');
        meta.removeAttribute(attribute);equal(meta[property],'',property+'-removed');
        const d=Object.getOwnPropertyDescriptor(HTMLMetaElement.prototype,property);
        rejects(()=>d.get.call(document.createElement('div')),property+'-wrong-tag');
        rejects(()=>d.set.call(Object.create(HTMLMetaElement.prototype),'bad'),property+'-forged-brand');
        rejects(()=>{meta[property]=Symbol();},property+'-symbol');
    }
    meta.content=null;equal(meta.content,'null','domstring-null');meta.content=undefined;equal(meta.content,'undefined','domstring-undefined');
    equal(Object.getOwnPropertyDescriptor(HTMLMetaElement.prototype,'charset'),undefined,'no-nonspec-charset-idl');
    equal(parsed.querySelector('meta').content,'Parsed metadata','parsed-meta-value');
    let converted=0;meta.httpEquiv={toString(){converted++;return 'refresh';}};
    equal(converted,1,'reflection-one-conversion');equal(meta.getAttribute('http-equiv'),'refresh','mapped-attribute');
    const sentinel={};let caught;try{meta.content={toString(){throw sentinel;}};}catch(error){caught=error;}
    equal(caught,sentinel,'reflection-conversion-exception');equal(meta.content,'undefined','failed-reflection-unchanged');
    const oldString=globalThis.String,oldURL=globalThis.URL;
    try{globalThis.String=()=>{throw sentinel;};globalThis.URL=()=>{throw sentinel;};meta.content={toString(){return 'captured';}};
        equal(meta.content,'captured','captured-domstring');
        const link=document.createElement('link');link.href='https://metadata.fixture/captured.css';equal(link.href,'https://metadata.fixture/captured.css','captured-url');
    }finally{globalThis.String=oldString;globalThis.URL=oldURL;}
    const link=document.createElement('link');
    for(const [property,attribute] of [['rel','rel'],['media','media'],['integrity','integrity'],['hreflang','hreflang'],['type','type'],
        ['imageSrcset','imagesrcset'],['imageSizes','imagesizes'],['charset','charset'],['rev','rev'],['target','target']]){
        equal(link[property],'',property+'-link-missing');link[property]='value';equal(link.getAttribute(attribute),'value',property+'-link-native-set');
        link.setAttribute(attribute,'native');equal(link[property],'native',property+'-link-live');link.removeAttribute(attribute);
    }
    equal(link.href,'','link-href-missing');link.href='theme.css';equal(link.getAttribute('href'),'theme.css','link-href-raw');
    equal(link.href,new URL('theme.css',document.baseURI).href,'link-href-resolved');
    link.href='http://[';equal(link.href,'http://[','link-invalid-url-retained');
    link.href='https://metadata.fixture/\ud800';equal(link.getAttribute('href'),'https://metadata.fixture/\ufffd','link-usv-scalar');
    link.imageSrcset='broken\ud800 1x';equal(link.imageSrcset,'broken\ufffd 1x','image-srcset-usv-scalar');
    rejects(()=>{link.href=Symbol();},'link-symbol-url');
    rejects(()=>Object.getOwnPropertyDescriptor(HTMLLinkElement.prototype,'href').get.call(document.createElement('a')),'link-href-wrong-tag');
    equal(link.crossOrigin,null,'cors-missing');link.crossOrigin='';equal(link.crossOrigin,'anonymous','cors-empty');
    link.crossOrigin='USE-CREDENTIALS';equal(link.crossOrigin,'use-credentials','cors-insensitive');
    link.crossOrigin='unknown';equal(link.crossOrigin,'anonymous','cors-invalid-anonymous');equal(link.getAttribute('crossorigin'),'unknown','cors-preserves-source');
    link.crossOrigin=null;equal(link.hasAttribute('crossorigin'),false,'cors-null-removes');
    link.crossOrigin=undefined;equal(link.hasAttribute('crossorigin'),false,'cors-undefined-removes');
    equal(link.as,'','as-default');link.as='SCRIPT';equal(link.as,'script','as-known');equal(link.getAttribute('as'),'SCRIPT','as-retains-case');
    link.as='not-a-destination';equal(link.as,'','as-unknown');link.as='image';equal(link.as,'image','as-image');
    equal(link.referrerPolicy,'','referrer-default');link.referrerPolicy='STRICT-ORIGIN';equal(link.referrerPolicy,'strict-origin','referrer-known');
    link.referrerPolicy='bogus';equal(link.referrerPolicy,'','referrer-unknown');
    equal(link.fetchPriority,'auto','priority-default');link.fetchPriority='LOW';equal(link.fetchPriority,'low','priority-known');
    link.fetchPriority='bogus';equal(link.fetchPriority,'auto','priority-unknown');
    equal(link.disabled,false,'link-disabled-default');link.disabled=true;equal(link.disabled,true,'link-disabled-set');
    equal(link.getAttribute('disabled'),'','link-disabled-native-attribute');link.disabled=false;equal(link.hasAttribute('disabled'),false,'link-disabled-remove');
    for(const property of ['sizes','blocking']){
        const tokens=link[property];ok(tokens instanceof DOMTokenList && tokens===link[property],property+'-tokens-same-object');
        link[property]='one two';equal(tokens[1],'two',property+'-putforwards');tokens.add('three');equal(link.getAttribute(property),'one two three',property+'-native-token-write');
    }
    rejects(()=>link.sizes.supports('any'),'sizes-no-vocabulary');equal(link.blocking.supports('render'),false,'blocking-not-falsely-supported');
    equal(link.relList.supports('stylesheet'),true,'native-stylesheet-supported');
    for(const unsupported of ['preload','modulepreload','icon','preconnect','dns-prefetch','manifest'])equal(link.relList.supports(unsupported),false,'unsupported-'+unsupported);
    ok(typeof Object.getOwnPropertyDescriptor(HTMLLinkElement.prototype,'sheet').get==='function','native-link-sheet-getter');
    ok(typeof Object.getOwnPropertyDescriptor(HTMLStyleElement.prototype,'sheet').get==='function','native-style-sheet-getter');
    const nativeStyle=document.createElement('style');nativeStyle.textContent='.native-sheet-probe{color:rgb(17,31,47)}';document.head.appendChild(nativeStyle);
    ok(nativeStyle.sheet instanceof CSSStyleSheet&&nativeStyle.sheet===nativeStyle.sheet,'native-style-sheet-identity');
    equal(nativeStyle.sheet.cssRules.length,1,'native-style-sheet-real-rule');equal(nativeStyle.sheet.cssRules[0].selectorText,'.native-sheet-probe','native-style-sheet-selector');
    nativeStyle.remove();
    const bases=parsed.querySelectorAll('base'),firstBase=bases[0],secondBase=parsed.createElement('base');
    secondBase.href='other/';parsed.head.appendChild(secondBase);
    equal(firstBase.href,'https://metadata.fixture/one/','base-absolute-reflection');
    equal(secondBase.href,new URL('other/',parsed.URL).href,'base-uses-fallback-not-first-base');
    equal(parsed.baseURI,'https://metadata.fixture/one/','first-base-native-document');
    const parsedLink=parsed.querySelector('link');equal(parsedLink.href,'https://metadata.fixture/one/parsed.css','parsed-link-base');
    firstBase.href='https://metadata.fixture/two/';equal(parsed.baseURI,'https://metadata.fixture/two/','base-set-invalidation');
    equal(parsedLink.href,'https://metadata.fixture/two/parsed.css','link-live-base-invalidation');
    firstBase.remove();equal(parsed.baseURI,new URL('other/',parsed.URL).href,'base-removal-invalidation');
    secondBase.target='_self';equal(secondBase.getAttribute('target'),'_self','base-target-reflection');
    rejects(()=>Object.getOwnPropertyDescriptor(HTMLBaseElement.prototype,'href').get.call(parsedLink),'base-wrong-tag');
    const title=parsed.querySelector('title');equal(title.text,'Parsed title','title-native-text');
    const nested=parsed.createElement('span');nested.textContent='descendant';title.replaceChildren('first',nested,parsed.createComment('comment'),'last');
    equal(title.text,'firstlast','title-child-text-not-descendant');equal(title.textContent,'firstdescendantlast','title-descendant-text-distinct');
    title.text='replacement';equal(title.childNodes.length,1,'title-replaces-native-children');equal(title.firstChild.nodeType,3,'title-replacement-text-node');
    equal(title.textContent,'replacement','title-native-set');title.text=null;equal(title.text,'null','title-domstring-null');
    rejects(()=>{title.text=Symbol();},'title-symbol');rejects(()=>Object.getOwnPropertyDescriptor(HTMLTitleElement.prototype,'text').get.call(nested),'title-wrong-tag');
    const titleValue=document.title;
    try{document.title='  Native\t title  ';const liveTitle=document.head.querySelector('title');
        ok(liveTitle instanceof HTMLTitleElement,'document-title-native-interface');equal(liveTitle.text,'  Native\t title  ','document-title-native-node');
        equal(document.title,'Native title','document-title-metadata');liveTitle.text='Changed title';equal(document.title,'Changed title','title-text-metadata-invalidation');
    }finally{document.title=titleValue;}
    const style=document.createElement('style');style.media='screen';equal(style.getAttribute('media'),'screen','style-media-native');
    style.setAttribute('media','print');equal(style.media,'print','style-media-live');style.type='text/css';equal(style.getAttribute('type'),'text/css','style-type-native');
    const blocking=style.blocking;style.blocking='render';ok(blocking===style.blocking && blocking.contains('render'),'style-blocking-live');
    equal(blocking.supports('render'),false,'no-render-blocking-claim');
    const adoptList=parsedLink.relList;document.adoptNode(parsedLink);
    equal(parsedLink.ownerDocument,document,'native-adoption-owner');ok(parsedLink instanceof HTMLLinkElement,'native-adoption-interface');
    equal(parsedLink.relList,adoptList,'adoption-token-identity');parsedLink.href='adopted.css';
    equal(parsedLink.href,new URL('adopted.css',document.baseURI).href,'adoption-url-owner');
    const observed=document.createElement('meta');let delivered=[];
    const observer=new MutationObserver(records=>{delivered.push(...records);});observer.observe(observed,{attributes:true,attributeOldValue:true});
    observed.content='one';observed.content='two';await Promise.resolve();
    equal(delivered.length,2,'reflection-native-mutation-observer');equal(delivered[1].attributeName,'content','reflection-mutation-name');
    equal(delivered[1].oldValue,'one','reflection-mutation-oldvalue');observer.disconnect();
    const sheet=document.createElement('style'),resource=document.createElement('link'),box=document.createElement('div');
    box.id='nocturne-metadata-box';box.style.cssText='width:11px;height:4px;padding:0;border:0;margin:0';
    sheet.textContent='#nocturne-metadata-box{width:41px !important}';sheet.media='screen';
    resource.rel='stylesheet';resource.href='data:text/css,'+encodeURIComponent('#nocturne-metadata-box{width:61px !important}');
    document.body.appendChild(box);document.head.appendChild(sheet);
    try{
        equal(box.offsetWidth,41,'style-applies-native-css');sheet.media='print';equal(box.offsetWidth,11,'style-media-native-invalidation');
        sheet.media='screen';equal(box.offsetWidth,41,'style-media-reactivates');
        sheet.textContent='#nocturne-metadata-box{width:43px !important}';equal(box.offsetWidth,43,'style-text-native-invalidation');
        document.head.appendChild(resource);equal(box.offsetWidth,61,'link-data-resource-native-css');
        resource.disabled=true;equal(box.offsetWidth,43,'link-disabled-native-css-excluded');
        resource.disabled=false;equal(box.offsetWidth,61,'link-enabled-native-css-applied');
        resource.relList.remove('stylesheet');equal(box.offsetWidth,43,'relList-native-resource-invalidation');
        resource.relList.add('stylesheet');equal(box.offsetWidth,61,'relList-native-resource-reactivation');
        resource.media='print';equal(box.offsetWidth,43,'link-media-native-invalidation');resource.media='screen';equal(box.offsetWidth,61,'link-media-reactivation');
        resource.href='data:text/css,'+encodeURIComponent('#nocturne-metadata-box{width:63px !important}');equal(box.offsetWidth,63,'link-href-native-resource-invalidation');
    }finally{box.remove();sheet.remove();resource.remove();}
    return count;
}
