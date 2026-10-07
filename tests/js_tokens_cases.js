/* Exercises the real native class/rel attributes, CE reactions and style
 * invalidation. Run with jstest's web_live host after the API batch is complete. */
async function runTokenListCases() {
    let count=0;
    const ok=(value,name)=>{count++;check('tokens-'+name,!!value);if(!value)throw new Error(name);};
    const equal=(a,b,name)=>ok(Object.is(a,b),name);
    const rejects=(fn,name,type=TypeError)=>{let e;try{fn();}catch(error){e=error;}ok(e instanceof type && (type!==DOMException || e.name===name),'reject-'+name);};
    const node=document.createElement('div'),list=node.classList;
    equal(list,node.classList,'same-object');ok(list instanceof DOMTokenList,'brand');
    equal(Object.getPrototypeOf(list),DOMTokenList.prototype,'prototype');
    equal(Object.prototype.toString.call(list),'[object DOMTokenList]','tag');
    rejects(()=>new DOMTokenList(),'constructor');rejects(()=>new DOMTokenList(node),'constructor-with-node');
    rejects(()=>new(class extends DOMTokenList {})(),'subclass-constructor');
    equal(list.length,0,'empty-length');equal(list.item(0),null,'empty-item');equal(list[0],undefined,'empty-index');
    equal(list.value,'','empty-value');equal(String(list),'','empty-stringifier');
    list.add();list.remove();equal(node.hasAttribute('class'),false,'no-empty-attribute-created');
    node.setAttribute('class',' a\ta\nb\fc\r b  ');
    equal(list.length,3,'ascii-space-and-dedup');equal([...list].join(','),'a,b,c','ordered-set');
    equal(list.value,' a\ta\nb\fc\r b  ','unserialized-value');equal(String(list),list.value,'unserialized-stringifier');
    equal(list[0],'a','index-zero');equal(list[2],'c','index-last');equal(list[3],undefined,'index-oob');
    equal(list.item(2),'c','item-last');equal(list.item(-1),null,'item-unsigned');equal(list.item(4294967296),'a','item-wrap');
    equal(list.item(1.9),'b','item-truncate');equal(list.item(NaN),'a','item-nan');
    equal(Object.keys(list).join(','),'0,1,2','enumerable-indices');ok('2' in list && !('3' in list),'live-index-has');
    const descriptor=Object.getOwnPropertyDescriptor(list,'1');
    ok(descriptor.value==='b' && !descriptor.writable && descriptor.enumerable && descriptor.configurable,'index-descriptor');
    equal(Reflect.set(list,'0','x'),false,'index-readonly');equal(Reflect.defineProperty(list,'1',{value:'x'}),false,'index-define-reject');
    equal(Reflect.deleteProperty(list,'0'),false,'index-delete-reject');equal(Reflect.deleteProperty(list,'4'),true,'missing-index-delete');
    equal(Reflect.set(list,'99','x'),false,'oob-index-readonly');equal(Reflect.preventExtensions(list),false,'cannot-freeze-live');
    list.extra=4;equal(list.extra,4,'expando');delete list.extra;
    list['01']='named';equal(list['01'],'named','noncanonical-index');delete list['01'];
    equal(list.contains('a'),true,'contains-present');equal(list.contains(''),false,'contains-empty-no-validation');
    equal(list.contains('a b'),false,'contains-whitespace-no-validation');
    node.className='native changed';equal([...list].join(','),'native,changed','className-live');
    node.classList='forward value';equal(node.getAttribute('class'),'forward value','classList-putforwards');equal(node.classList,list,'putforwards-same-object');
    list.value='x\u00a0y';equal(list.length,1,'nonascii-whitespace-token');equal(list[0],'x\u00a0y','nonascii-preserved');
    list.add('z');equal(node.getAttribute('class'),'x\u00a0y z','native-add');
    list.remove('z');equal(node.getAttribute('class'),'x\u00a0y','native-remove');
    list.value='a b c';list.add('a','d','d');equal(list.value,'a b c d','add-unique');
    list.remove('b','missing');equal(list.value,'a c d','remove-order');
    equal(list.toggle('a'),false,'toggle-remove');equal(list.toggle('a'),true,'toggle-add');
    equal(list.toggle('a',true),true,'force-present');equal(list.toggle('absent',false),false,'force-absent');
    equal(list.toggle('a',undefined),false,'undefined-force-is-omitted');
    equal(list.toggle('a',{}),true,'truthy-force');equal(list.toggle('a',0),false,'falsey-force');
    list.value='a b c';equal(list.replace('a','c'),true,'replace-forward-existing');equal(list.value,'c b','replace-first-either');
    list.value='c b a';equal(list.replace('a','c'),true,'replace-backward-existing');equal(list.value,'c b','replace-backward-order');
    equal(list.replace('c','c'),true,'replace-self');equal(list.replace('missing','x'),false,'replace-missing');equal(list.value,'c b','replace-missing-no-update');
    list.value='x x  y';list.add();equal(list.value,'x y','empty-add-normalizes');
    list.value='x x  y';list.remove();equal(list.value,'x y','empty-remove-normalizes');
    for(const name of ['add','remove']){
        const before=list.value;rejects(()=>list[name]('first',''),'SyntaxError',DOMException);equal(list.value,before,name+'-atomic-empty');
        rejects(()=>list[name]('first','bad token'),'InvalidCharacterError',DOMException);equal(list.value,before,name+'-atomic-whitespace');
    }
    rejects(()=>list.toggle(''),'SyntaxError',DOMException);rejects(()=>list.toggle('\t'),'InvalidCharacterError',DOMException);
    rejects(()=>list.replace('bad token',''),'SyntaxError',DOMException);rejects(()=>list.replace('','bad token'),'SyntaxError',DOMException);
    rejects(()=>list.replace('x','bad\rtoken'),'InvalidCharacterError',DOMException);
    for(const name of ['item','contains','toggle','replace','supports'])rejects(()=>list[name](),'required-'+name);
    for(const name of ['contains','add','remove','toggle','supports'])rejects(()=>list[name](Symbol()),'symbol-'+name);
    rejects(()=>list.replace('x',Symbol()),'symbol-replacement');rejects(()=>{list.value=Symbol();},'symbol-value');
    rejects(()=>list.item(Symbol()),'symbol-index');rejects(()=>list.item(1n),'bigint-index');rejects(()=>list.supports('x'),'no-class-vocabulary');
    for(const name of ['item','contains','add','remove','toggle','replace','supports','toString'])
        rejects(()=>DOMTokenList.prototype[name].call({},'x','y'),'brand-'+name);
    for(const property of ['value','length'])rejects(()=>Object.getOwnPropertyDescriptor(DOMTokenList.prototype,property).get.call({}),'brand-'+property);
    rejects(()=>Object.getOwnPropertyDescriptor(DOMTokenList.prototype,'value').set.call({},'x'),'brand-value-set');
    list.value='start';const conversion=[];
    list.add({toString(){conversion.push('first');node.className='reentrant';return 'one';}},
        {toString(){conversion.push('second');return 'two';}});
    equal(conversion.join(','),'first,second','conversion-order');equal(list.value,'reentrant one two','conversion-before-read');
    const sentinel={};let caught;try{list.add('x',{toString(){throw sentinel;}});}catch(e){caught=e;}
    equal(caught,sentinel,'conversion-exception-propagates');equal(list.value,'reentrant one two','conversion-exception-atomic');
    const originalString=globalThis.String,originalException=globalThis.DOMException;
    try{globalThis.String=()=>{throw sentinel;};globalThis.DOMException=()=>{throw sentinel;};list.add('captured');
        equal(list.contains('captured'),true,'captured-string');rejects(()=>list.add(''),'SyntaxError',originalException);
    }finally{globalThis.String=originalString;globalThis.DOMException=originalException;}
    equal(DOMTokenList.prototype[Symbol.iterator],Array.prototype.values,'actual-array-iterator');
    for(const name of ['keys','values','entries','forEach'])equal(DOMTokenList.prototype[name],Array.prototype[name],'actual-array-'+name);
    equal([...DOMTokenList.prototype.values.call({0:'borrowed',length:1})][0],'borrowed','generic-iterators');
    list.value='a b';const iterator=list.values();equal(iterator.next().value,'a','iterator-first');
    list.add('c');equal(iterator.next().value,'b','iterator-second');equal(iterator.next().value,'c','iterator-observes-appended');
    equal(iterator.next().done,true,'iterator-done');list.add('d');equal(iterator.next().done,true,'completed-iterator-stays-done');
    equal([...list.keys()].join(','),'0,1,2,3','keys');equal([...list.entries()].map(pair=>pair.join(':')).join(','),'0:a,1:b,2:c,3:d','entries');
    list.value='a b c';const context={},seen=[];
    list.forEach(function(value,index,receiver){seen.push([value,index,this===context,receiver===list]);if(index===0)list.add('d');},context);
    equal(seen.length,3,'forEach-initial-length');ok(seen.every(row=>row[2] && row[3]),'forEach-callback-arguments');
    list.value='a b c';const shrunk=[];list.forEach((value,index)=>{shrunk.push(value);if(index===0)list.remove('b');});
    equal(shrunk.join(','),'a,c','forEach-live-and-skips-deleted');rejects(()=>list.forEach(null),'forEach-callback');
    node.removeAttribute('class');equal(list.length,0,'removed-attribute-live');equal('0' in list,false,'indices-disappear');
    equal(Object.keys(list).length,0,'own-keys-disappear');
    const svg=document.createElementNS('http://www.w3.org/2000/svg','svg'),svgList=svg.classList;
    svgList.add('svg-token');equal(svg.getAttribute('class'),'svg-token','svg-native-attribute');equal(svg.classList,svgList,'svg-same-object');
    const detached=document.implementation.createHTMLDocument();detached.adoptNode(node);
    equal(node.classList,list,'adoption-retains-object');list.add('adopted');equal(node.getAttribute('class'),'adopted','adopted-native-write');
    document.adoptNode(node);equal(node.classList,list,'readoption-retains-object');
    const clone=node.cloneNode(true);ok(clone.classList!==list && clone.classList.contains('adopted'),'clone-new-list');
    clone.classList.add('clone');equal(list.contains('clone'),false,'clone-not-shared');
    for(const tag of ['a','area','link']){
        const el=document.createElement(tag),rel=el.relList;
        ok(rel instanceof DOMTokenList && rel===el.relList,tag+'-rel-brand-and-same-object');
        el.rel='author external';equal(rel[1],'external',tag+'-rel-reflection');rel.add('native');equal(el.getAttribute('rel'),'author external native',tag+'-rel-write');
        el.relList='stylesheet';equal(el.rel,'stylesheet',tag+'-rel-putforwards');equal(rel.contains('stylesheet'),true,tag+'-rel-live');
        equal(rel.supports('STYLESHEET'),tag==='link',tag+'-supported-native-vocabulary');equal(rel.supports('preload'),false,tag+'-no-preload-claim');
        equal(rel.supports(''),false,tag+'-supports-empty');equal(rel.supports('bad token'),false,tag+'-supports-no-token-validation');
        equal(el.classList.contains('stylesheet'),false,tag+'-separate-attributes');
    }
    const observed=document.createElement('div'),observedList=observed.classList;let delivered=[];
    const observer=new MutationObserver(records=>{delivered.push(...records);});observer.observe(observed,{attributes:true,attributeOldValue:true});
    observedList.add();equal(observer.takeRecords().length,0,'empty-add-no-native-mutation');
    observedList.value='a a  b';observer.takeRecords();observedList.add();
    let records=observer.takeRecords();equal(records.length,1,'normalization-one-mutation');equal(records[0].oldValue,'a a  b','normalization-oldvalue');
    observedList.toggle('a',true);observedList.toggle('none',false);observedList.replace('none','x');
    equal(observer.takeRecords().length,0,'no-op-toggle-replace-no-mutation');
    observedList.add('c');equal(delivered.length,0,'delivery-not-synchronous');await Promise.resolve();
    equal(delivered.length,1,'microtask-native-mutation');equal(delivered[0].target,observed,'native-mutation-target');observer.disconnect();
    class TokenHost extends HTMLElement {
        static get observedAttributes(){return ['class'];}
        attributeChangedCallback(name,oldValue,newValue){this.changes??=[];this.changes.push([name,oldValue,newValue]);}
    }
    customElements.define('nocturne-token-host',TokenHost);const host=document.createElement('nocturne-token-host');
    host.classList.add('one');equal(host.changes.length,1,'custom-element-sync-reaction');equal(host.changes[0][2],'one','custom-element-native-value');
    host.classList.value='two';equal(host.changes.length,2,'custom-element-value-reaction');equal(host.changes[1][1],'one','custom-element-oldvalue');
    const style=document.createElement('style'),box=document.createElement('div');
    style.textContent='.nocturne-token-wide{width:37px !important}';box.style.cssText='width:11px;height:4px;padding:0;border:0;margin:0';
    document.head.appendChild(style);document.body.appendChild(box);
    try{equal(box.offsetWidth,11,'native-style-baseline');box.classList.add('nocturne-token-wide');equal(box.offsetWidth,37,'native-class-style-invalidation');
        box.classList.remove('nocturne-token-wide');equal(box.offsetWidth,11,'native-class-style-removal');
    }finally{box.remove();style.remove();}
    return count;
}
