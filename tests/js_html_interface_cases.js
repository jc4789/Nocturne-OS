/* WHATWG HTML interfaces on real native DOM nodes; no site-content injection. */
function runHTMLInterfaceCases() {
    let count=0;
    const ok=(v,n)=>{count++;check('html-interfaces-'+n,!!v);if(!v)throw new Error(n);};
    const eq=(a,b,n)=>ok(Object.is(a,b),n);
    const throws=(fn,name,n)=>{let e;try{fn();}catch(x){e=x;}ok(e&&e.name===name,n);};
    const htmlNS='http://www.w3.org/1999/xhtml';
    const groups=[['HTMLBodyElement','body'],['HTMLHtmlElement','html'],['HTMLParagraphElement','p'],
        ['HTMLSpanElement','span'],['HTMLBRElement','br'],['HTMLHRElement','hr'],
        ['HTMLPreElement','pre','listing','xmp'],['HTMLQuoteElement','q','blockquote'],['HTMLModElement','ins','del'],
        ['HTMLUListElement','ul'],['HTMLDListElement','dl'],['HTMLDirectoryElement','dir'],
        ['HTMLTableElement','table'],['HTMLTableCaptionElement','caption'],['HTMLTableSectionElement','thead','tbody','tfoot'],
        ['HTMLTableRowElement','tr'],['HTMLTableCellElement','td','th'],['HTMLTableColElement','col','colgroup'],
        ['HTMLLegendElement','legend'],['HTMLOptGroupElement','optgroup'],['HTMLMapElement','map'],
        ['HTMLEmbedElement','embed'],['HTMLParamElement','param'],['HTMLFontElement','font'],
        ['HTMLFrameSetElement','frameset'],['HTMLMarqueeElement','marquee']];
    const markup=tag=>tag==='html'||tag==='body'?'<html><body>parsed</body></html>':
        tag==='frameset'?'<html><head></head><frameset></frameset></html>':
        ['caption','thead','tbody','tfoot','colgroup'].includes(tag)?'<table><'+tag+'></'+tag+'></table>':
        tag==='tr'?'<table><tbody><tr></tr></tbody></table>':
        tag==='td'||tag==='th'?'<table><tbody><tr><'+tag+'></'+tag+'></tr></tbody></table>':
        tag==='col'?'<table><colgroup><col></colgroup></table>':
        '<'+tag+'></'+tag+'>';
    for(const [name,...tags] of groups){
        const C=globalThis[name];ok(typeof C==='function',name+'-global');
        const globalDescriptor=Object.getOwnPropertyDescriptor(globalThis,name);
        ok(globalDescriptor.writable&&globalDescriptor.configurable&&!globalDescriptor.enumerable,name+'-global-descriptor');
        eq(C.name,name,name+'-constructor-name');eq(Object.getPrototypeOf(C.prototype),HTMLElement.prototype,name+'-inheritance');
        throws(()=>new C(),'TypeError',name+'-illegal-constructor');
        throws(()=>new(class extends C {})(),'TypeError',name+'-unregistered-constructor');
        for(const tag of tags){
            const n=document.createElement(tag),parsed=new DOMParser().parseFromString(markup(tag),'text/html'),p=parsed.querySelector(tag);
            ok(n instanceof C&&n instanceof HTMLElement,tag+'-created-brand');eq(Object.getPrototypeOf(n),C.prototype,tag+'-prototype');
            eq(Object.prototype.toString.call(n),'[object '+name+']',tag+'-to-string-tag');
            ok(p instanceof C,tag+'-parser-brand');ok(document.createElementNS(htmlNS,tag) instanceof C,tag+'-namespace');
            eq(document.createElementNS('http://www.w3.org/2000/svg',tag) instanceof C,false,tag+'-svg-excluded');
            eq(document.createElementNS(null,tag) instanceof C,false,tag+'-null-namespace-excluded');
            const clone=p.cloneNode(true);ok(clone instanceof C&&clone!==p,tag+'-clone');eq(clone.outerHTML,p.outerHTML,tag+'-clone-content');
            const imported=document.importNode(p,true);ok(imported instanceof C&&imported.ownerDocument===document,tag+'-import');
            const adopted=document.adoptNode(p);ok(adopted===p&&adopted instanceof C&&adopted.ownerDocument===document,tag+'-adopt');
        }
    }
    ok(document.body instanceof HTMLBodyElement,'live-body');ok(document.documentElement instanceof HTMLHtmlElement,'live-html');
    const strings=[['html','version','version'],['p','align','align'],['br','clear','clear'],['hr','width','width'],
        ['ins','dateTime','datetime'],['del','dateTime','datetime'],
        ['ul','type','type'],['legend','align','align'],['optgroup','label','label'],['map','name','name'],
        ['embed','type','type'],['embed','width','width'],['embed','height','height'],['param','name','name'],
        ['param','value','value'],['param','valueType','valuetype'],['param','type','type'],
        ['font','face','face'],['font','size','size'],['frameset','cols','cols'],['frameset','rows','rows'],
        ['marquee','behavior','behavior'],['marquee','bgColor','bgcolor'],['marquee','direction','direction']];
    for(const [tag,prop,attr] of strings){
        const n=document.createElement(tag),d=Object.getOwnPropertyDescriptor(Object.getPrototypeOf(n),prop);
        ok(d&&typeof d.get==='function'&&typeof d.set==='function',tag+'-'+prop+'-accessor');
        eq(d.get.name,'get '+prop,tag+'-'+prop+'-getter-name');eq(d.set.name,'set '+prop,tag+'-'+prop+'-setter-name');
        eq(n[prop],'',tag+'-'+prop+'-default');n[prop]='native';eq(n.getAttribute(attr),'native',tag+'-'+prop+'-write');
        n.setAttribute(attr,'attribute');eq(n[prop],'attribute',tag+'-'+prop+'-live-read');
        n.setAttributeNS('urn:test','x:'+attr,'foreign');n.removeAttribute(attr);eq(n[prop],'',tag+'-'+prop+'-namespace-isolation');
        n[prop]=undefined;eq(n.getAttribute(attr),'undefined',tag+'-'+prop+'-undefined');
        throws(()=>{n[prop]=Symbol();},'TypeError',tag+'-'+prop+'-symbol');
        throws(()=>d.get.call(document.createElement('div')),'TypeError',tag+'-'+prop+'-wrong-brand');
        throws(()=>d.set.call(Object.create(Object.getPrototypeOf(n)),'forged'),'TypeError',tag+'-'+prop+'-forged-brand');
    }
    for(const [tag,prop] of [['body','text'],['body','bgColor'],['body','link'],['body','vLink'],['body','aLink'],['font','color']]){
        const n=document.createElement(tag);n[prop]='red';n[prop]=null;eq(n.getAttribute(prop.toLowerCase()),'',tag+'-'+prop+'-legacy-null-empty');
    }
    const body=document.createElement('body');body.background='relative.jpg';eq(body.background,'relative.jpg','body-background-raw');
    const previousHashHandler=globalThis.onhashchange;let bodyEvents=0;
    const bodyHandler=()=>bodyEvents++;
    try{document.body.onhashchange=bodyHandler;eq(globalThis.onhashchange,bodyHandler,'body-window-handler-forward');
        globalThis.dispatchEvent(new Event('hashchange'));eq(bodyEvents,1,'body-window-handler-dispatch');
    }finally{globalThis.onhashchange=previousHashHandler;}
    const previousErrorHandler=globalThis.onerror,previousLoadHandler=globalThis.onload,errorValue={};
    const previousErrorAttribute=document.body.getAttribute('onerror');
    let errorArgs,errorThis,loadArgs,loadThis;
    try{
        const errorHandler=function(...args){errorArgs=args;errorThis=this;return true;};
        document.body.onerror=errorHandler;eq(globalThis.onerror,errorHandler,'body-error-window-handler-shared');
        const errorEvent=new ErrorEvent('error',{message:'interface error',filename:'https://fixture.test/error.js',lineno:7,colno:9,error:errorValue,cancelable:true});
        eq(globalThis.dispatchEvent(errorEvent),false,'body-error-true-cancels');eq(errorEvent.defaultPrevented,true,'body-error-canceled-event');
        eq(errorArgs.length,5,'body-error-five-arguments');eq(errorArgs[0],'interface error','body-error-message');
        eq(errorArgs[1],'https://fixture.test/error.js','body-error-filename');eq(errorArgs[2],7,'body-error-line');eq(errorArgs[3],9,'body-error-column');
        eq(errorArgs[4],errorValue,'body-error-identity');eq(errorThis,globalThis,'body-error-window-this');
        globalThis.onerror=()=>false;eq(document.body.onerror,globalThis.onerror,'body-error-window-setter-shared');
        eq(globalThis.dispatchEvent(new ErrorEvent('error',{cancelable:true})),true,'window-error-false-does-not-cancel');
        document.body.setAttribute('onerror','document._htmlInterfaceInlineError=[event,source,lineno,colno,error,this,arguments.length];return true;');
        eq(document.body.onerror.length,5,'body-inline-error-five-parameters');
        const inlineErrorEvent=new ErrorEvent('error',{message:'inline error',filename:'https://fixture.test/inline.js',lineno:11,colno:13,error:errorValue,cancelable:true});
        eq(globalThis.dispatchEvent(inlineErrorEvent),false,'body-inline-error-true-cancels');
        const inlineError=document._htmlInterfaceInlineError;
        eq(inlineError[0],'inline error','body-inline-error-event-parameter');eq(inlineError[1],'https://fixture.test/inline.js','body-inline-error-source-parameter');
        eq(inlineError[2],11,'body-inline-error-lineno-parameter');eq(inlineError[3],13,'body-inline-error-colno-parameter');
        eq(inlineError[4],errorValue,'body-inline-error-error-parameter');eq(inlineError[5],globalThis,'body-inline-error-window-this');eq(inlineError[6],5,'body-inline-error-five-arguments');
        const errorElement=document.createElement('span');
        errorElement.setAttribute('onerror','this._inlineError=[event,arguments.length];return false;');
        eq(errorElement.onerror.length,1,'element-inline-error-one-parameter');
        const elementErrorEvent=new ErrorEvent('error',{cancelable:true});
        eq(errorElement.dispatchEvent(elementErrorEvent),false,'element-inline-error-false-cancels');
        eq(errorElement._inlineError[0],elementErrorEvent,'element-inline-error-event-argument');eq(errorElement._inlineError[1],1,'element-inline-error-one-argument');
        document.body.onload=function(...args){loadArgs=args;loadThis=this;return false;};
        const loadEvent=new Event('load',{cancelable:true});eq(globalThis.dispatchEvent(loadEvent),false,'body-load-false-cancels');
        eq(loadArgs.length,1,'body-load-one-argument');eq(loadArgs[0],loadEvent,'body-load-event-argument');eq(loadThis,globalThis,'body-load-window-this');
    }finally{
        if(previousErrorAttribute===null)document.body.removeAttribute('onerror');else document.body.setAttribute('onerror',previousErrorAttribute);
        delete document._htmlInterfaceInlineError;
        globalThis.onerror=previousErrorHandler;globalThis.onload=previousLoadHandler;
    }
    const xhr=new XMLHttpRequest();let xhrLoadArgs,xhrLoadThis;
    xhr.onload=function(...args){xhrLoadArgs=args;xhrLoadThis=this;return false;};
    const xhrLoadEvent=new Event('load',{cancelable:true});eq(xhr.dispatchEvent(xhrLoadEvent),false,'xhr-load-false-cancels');
    eq(xhrLoadArgs.length,1,'xhr-load-one-argument');eq(xhrLoadArgs[0],xhrLoadEvent,'xhr-load-event-argument');eq(xhrLoadThis,xhr,'xhr-load-receiver-preserved');
    const embed=document.createElement('embed');eq(embed.getSVGDocument(),null,'embed-without-svg-document');
    throws(()=>HTMLEmbedElement.prototype.getSVGDocument.call(document.createElement('div')),'TypeError','embed-svg-method-brand');
    const paragraph=document.createElement('p'),align=Object.getOwnPropertyDescriptor(HTMLParagraphElement.prototype,'align');
    let convertedString=0;paragraph.align={toString(){convertedString++;return 'left';}};eq(convertedString,1,'reflection-one-conversion');
    const sentinel={};let caught;try{paragraph.align={toString(){throw sentinel;}};}catch(e){caught=e;}
    eq(caught,sentinel,'reflection-preserves-conversion-exception');eq(paragraph.getAttribute('align'),'left','failed-conversion-does-not-mutate');
    throws(()=>align.set.call({}, {toString(){convertedString++;return 'right';}}),'TypeError','brand-before-conversion');eq(convertedString,1,'brand-does-not-convert');
    for(const tag of ['q','blockquote','ins','del','embed']){
        const n=document.createElement(tag),prop=tag==='embed'?'src':'cite';n[prop]='path';
        eq(n.getAttribute(prop),'path',tag+'-url-source');eq(n[prop],new URL('path',document.baseURI).href,tag+'-resolved-url');
        n[prop]='https://fixture.test/\ud800';eq(n.getAttribute(prop),'https://fixture.test/\ufffd',tag+'-usv-scalar');
        n.setAttribute(prop,'attribute');eq(n[prop],new URL('attribute',document.baseURI).href,tag+'-url-live-read');
        const descriptor=Object.getOwnPropertyDescriptor(Object.getPrototypeOf(n),prop);
        throws(()=>descriptor.get.call(document.createElement('div')),'TypeError',tag+'-url-wrong-brand');
    }
    for(const [tag,prop] of [['ul','compact'],['dl','compact'],['dir','compact'],['hr','noShade'],['optgroup','disabled'],['marquee','trueSpeed']]){
        const n=document.createElement(tag),attr=prop.toLowerCase();eq(n[prop],false,tag+'-boolean-default');
        n[prop]='false';eq(n.getAttribute(attr),'',tag+'-boolean-presence');n[prop]=0;eq(n.hasAttribute(attr),false,tag+'-boolean-removal');
    }
    const pre=document.createElement('pre');eq(pre.width,0,'pre-width-default');pre.width=4294967297;eq(pre.width,1,'pre-width-long-wrap');
    pre.width=-9.8;eq(pre.width,-9,'pre-width-long-truncate');throws(()=>{pre.width=1n;},'TypeError','pre-width-bigint');
    const marq=document.createElement('marquee');eq(marq.scrollAmount,6,'marquee-amount-default');eq(marq.scrollDelay,85,'marquee-delay-default');
    eq(marq.loop,-1,'marquee-loop-default');marq.loop=3;eq(marq.loop,3,'marquee-loop-set');marq.loop=0;eq(marq.loop,3,'marquee-loop-ignore-zero');
    marq.loop=-2;eq(marq.loop,3,'marquee-loop-ignore-negative');marq.setAttribute('loop','0');eq(marq.loop,-1,'marquee-loop-invalid-attribute');
    throws(()=>HTMLMarqueeElement.prototype.start.call(document.createElement('div')),'TypeError','marquee-start-brand');
    throws(()=>HTMLMarqueeElement.prototype.stop.call({}),'TypeError','marquee-stop-brand');
    const form=document.createElement('form'),fieldset=document.createElement('fieldset'),legend=document.createElement('legend');
    form.appendChild(fieldset);fieldset.appendChild(legend);eq(legend.form,form,'legend-direct-fieldset-form');
    const wrapper=document.createElement('div');fieldset.appendChild(wrapper);wrapper.appendChild(legend);eq(legend.form,null,'legend-not-ancestor-search');
    const map=document.createElement('map'),areas=map.areas,a=document.createElement('area'),nested=document.createElement('map');
    ok(areas instanceof HTMLCollection&&areas===map.areas,'map-same-object-collection');eq(areas.length,0,'map-empty');
    a.id='interface-area';nested.appendChild(a);map.appendChild(nested);eq(areas.length,1,'map-all-descendants');
    eq(areas.namedItem('interface-area'),a,'map-named-item');a.remove();eq(areas.length,0,'map-live-remove');
    let serial=0;
    for(const [tag,C,property,attribute,value] of [['p',HTMLParagraphElement,'align','align','right'],['pre',HTMLPreElement,'width','width',23],['ul',HTMLUListElement,'compact','compact',true]]){
        const name='nocturne-interface-'+Date.now()+'-'+(++serial),log=[];
        class Builtin extends C {static observedAttributes=[attribute];attributeChangedCallback(n,o,v){log.push([n,o,v].join(':'));}}
        customElements.define(name,Builtin,{extends:tag});const n=new Builtin();
        ok(n instanceof C&&n instanceof Builtin&&n.localName===tag,'custom-'+tag+'-constructor');
        n[property]=value;eq(log.length,1,'custom-'+tag+'-reaction-synchronous');eq(log[0],attribute+':'+':'+n.getAttribute(attribute),'custom-'+tag+'-reaction-native');
        const created=document.createElement(tag,{is:name});ok(created instanceof Builtin,'custom-'+tag+'-create');
    }
    runHTMLTableInterfaceCases(ok,eq,throws);
    return count;
}

function runHTMLTableInterfaceCases(ok,eq,throws) {
    const table=document.createElement('table'),foot=document.createElement('tfoot'),body=document.createElement('tbody'),head=document.createElement('thead');
    table.appendChild(foot);table.appendChild(body);table.appendChild(head);
    const f=foot.insertRow(),b=body.insertRow(),h=head.insertRow();f.id='if-foot';b.id='if-body';h.id='if-head';
    const direct=document.createElement('tr');direct.id='if-direct';table.insertBefore(direct,body);
    const rows=table.rows;ok(rows instanceof HTMLCollection&&rows===table.rows,'table-rows-same-object');
    eq(Array.from(rows,n=>n.id).join(','),'if-head,if-direct,if-body,if-foot','table-ordered-rows');
    eq(h.rowIndex,0,'table-head-row-index');eq(b.rowIndex,2,'table-body-row-index');eq(f.rowIndex,3,'table-foot-row-index');
    eq(direct.sectionRowIndex,1,'table-direct-section-row-index');eq(table.tBodies.length,1,'table-body-collection');
    eq(table.tBodies,table.tBodies,'table-bodies-same-object');eq(body.rows,body.rows,'section-rows-same-object');
    const cell=b.insertCell(),nested=document.createElement('table');cell.appendChild(nested);nested.insertRow().insertCell();
    eq(rows.length,4,'table-nested-rows-excluded');eq(b.cells.length,1,'row-nested-cells-excluded');eq(b.cells,b.cells,'row-cells-same-object');
    const th=document.createElement('th');b.insertBefore(th,cell);eq(th.cellIndex,0,'th-cell-index');eq(cell.cellIndex,1,'td-cell-index');
    cell.remove();eq(cell.cellIndex,-1,'detached-cell-index');eq(b.cells.length,1,'cells-live-removal');
    const detached=document.createElement('tr');eq(detached.rowIndex,-1,'detached-row-index');eq(detached.sectionRowIndex,-1,'detached-section-index');
    const appended=table.insertRow();eq(appended.parentNode,foot,'table-default-insertion-last-row-parent');eq(rows.length,5,'table-live-insertion');
    table.deleteRow(-1);eq(rows.length,4,'table-delete-last');
    throws(()=>table.insertRow(5),'IndexSizeError','table-index-too-large');throws(()=>table.insertRow(-2),'IndexSizeError','table-index-negative');
    throws(()=>table.deleteRow(4),'IndexSizeError','table-delete-too-large');throws(()=>table.deleteRow(),'TypeError','table-delete-required');
    throws(()=>table.insertRow(Symbol()),'TypeError','table-index-symbol');throws(()=>table.insertRow(1n),'TypeError','table-index-bigint');
    const converted=table.insertRow(4294967296);eq(rows[0],converted,'table-index-long-conversion');converted.remove();
    const caption=table.createCaption();eq(table.createCaption(),caption,'table-create-caption-existing');eq(table.firstChild,caption,'table-caption-insertion');
    const replacement=document.createElement('caption');table.caption=replacement;eq(caption.parentNode,null,'table-caption-replaces');eq(table.caption,replacement,'table-caption-live-get');
    throws(()=>{table.caption=document.createElement('div');},'TypeError','table-caption-idl-brand');table.caption=null;eq(table.caption,null,'table-caption-null');
    eq(table.createTHead(),head,'table-create-head-existing');eq(table.createTFoot(),foot,'table-create-foot-existing');
    throws(()=>{table.tHead=body;},'HierarchyRequestError','table-head-wrong-section');throws(()=>{table.tFoot=head;},'HierarchyRequestError','table-foot-wrong-section');
    const body2=table.createTBody();eq(body2.previousElementSibling,body,'table-create-body-after-last-body');eq(table.tBodies.length,2,'table-create-body-live');
    table.deleteTHead();eq(table.tHead,null,'table-delete-head');table.deleteTFoot();eq(table.tFoot,null,'table-delete-foot');
    const empty=document.createElement('table'),first=empty.insertRow();eq(first.parentNode.localName,'tbody','table-empty-inserts-tbody');
    eq(empty.tBodies[0],first.parentNode,'table-created-body-native');empty.deleteRow(-1);empty.deleteRow(-1);eq(empty.rows.length,0,'table-empty-delete-minus-one');
    const sec=document.createElement('tbody');throws(()=>sec.deleteRow(),'TypeError','section-delete-required');
    throws(()=>sec.insertRow(1),'IndexSizeError','section-empty-insert-index');const r=sec.insertRow();
    const c=r.insertCell();eq(c.localName,'td','row-insert-td');eq(r.insertCell(0).cellIndex,0,'row-insert-at-zero');
    throws(()=>r.insertCell(3),'IndexSizeError','cell-insert-large');throws(()=>r.deleteCell(-2),'IndexSizeError','cell-delete-negative');
    throws(()=>r.deleteCell(),'TypeError','cell-delete-required');r.deleteCell(-1);eq(r.cells.length,1,'cell-delete-last');
    c.colSpan=0;eq(c.colSpan,1,'cell-colspan-minimum');c.colSpan=2000;eq(c.colSpan,1000,'cell-colspan-limit');
    c.rowSpan=0;eq(c.rowSpan,0,'cell-rowspan-zero');c.rowSpan=70000;eq(c.rowSpan,65534,'cell-rowspan-limit');
    c.setAttribute('rowspan','-0');eq(c.rowSpan,0,'cell-rowspan-parsed-negative-zero');
    c.setAttribute('rowspan','-7');eq(c.rowSpan,1,'cell-rowspan-negative-default');
    c.colSpan=9000;eq(c.getAttribute('colspan'),'9000','cell-colspan-setter-preserves-source');eq(c.colSpan,1000,'cell-colspan-getter-clamps');
    c.colSpan=-1;eq(c.getAttribute('colspan'),'1','cell-colspan-unsigned-overflow-default');
    const col=document.createElement('col');col.span=0;eq(col.span,1,'col-span-minimum');col.span=2000;eq(col.span,1000,'col-span-limit');
    throws(()=>{c.colSpan=1n;},'TypeError','cell-span-bigint');
    throws(()=>HTMLTableElement.prototype.insertRow.call(document.createElement('div')),'TypeError','table-method-brand');
    const getter=Object.getOwnPropertyDescriptor(HTMLTableRowElement.prototype,'cells').get;
    throws(()=>getter.call(Object.create(HTMLTableRowElement.prototype)),'TypeError','row-collection-forged-brand');
    const attrs=[['table','cellPadding','cellpadding'],['caption','align','align'],['tbody','chOff','charoff'],
        ['tr','vAlign','valign'],['td','headers','headers'],['th','abbr','abbr'],['th','scope','scope'],['colgroup','ch','char']];
    for(const [tag,prop,attr] of attrs){const n=document.createElement(tag);n[prop]='native';eq(n.getAttribute(attr),'native',tag+'-'+prop+'-reflection');}
    const sh=document.createElement('th');sh.scope='ROW';eq(sh.scope,'row','cell-scope-canonical');sh.scope='invalid';eq(sh.scope,'','cell-scope-invalid');
    const dom=document.createElement('table');dom.innerHTML='<tbody><tr><td>initial</td></tr></tbody>';
    const live=dom.rows,original=live[0];dom.tBodies[0].innerHTML='<tr><th>changed</th></tr><tr><td>second</td></tr>';
    eq(live.length,2,'table-collection-parser-mutation');ok(live[0]!==original&&live[0].cells[0] instanceof HTMLTableCellElement,'table-native-parser-interface');
    const geometry=document.createElement('div');
    eq(geometry.getClientRects().length,0,'rects-detached-empty');
    throws(()=>Element.prototype.getClientRects.call(document.createTextNode('x')),'TypeError','rects-element-brand');
    geometry.style.cssText='position:relative;width:90px;font-size:16px;line-height:20px';
    document.body.appendChild(geometry);
    try {
        const block=document.createElement('div');block.style.cssText='width:70px;height:20px;padding:3px;border:2px solid black';geometry.appendChild(block);
        const first=block.getClientRects();eq(first.length,1,'rects-block-single');
        ok(first instanceof DOMRectList&&first[0] instanceof DOMRect,'rects-native-interface');
        eq(first[0].width,80,'rects-border-width');eq(first[0].height,30,'rects-border-height');
        eq(first.item(0),first[0],'rects-item-identity');eq(first.item(9),null,'rects-item-missing');
        eq([...first][0],first[0],'rects-default-iterator');
        throws(()=>first.item(),'TypeError','rects-item-required');throws(()=>first.item(Symbol()),'TypeError','rects-item-symbol');
        throws(()=>first.item(1n),'TypeError','rects-item-bigint');
        eq(Reflect.set(first,'0',null),false,'rects-index-readonly');eq(Reflect.deleteProperty(first,'0'),false,'rects-index-retained');
        block.style.width='100px';eq(first[0].width,80,'rects-snapshot');eq(block.getClientRects()[0].width,110,'rects-new-layout');
        block.style.display='none';eq(block.getClientRects().length,0,'rects-display-none-empty');
        const inline=document.createElement('span');inline.textContent='one two three four five six seven eight nine ten';geometry.appendChild(inline);
        const fragments=inline.getClientRects();ok(fragments.length>1,'rects-inline-fragments');
        ok(fragments[fragments.length-1].top>fragments[0].top,'rects-inline-line-position');
        const bound=inline.getBoundingClientRect();ok(bound instanceof DOMRect,'rects-bounding-interface');
        ok(fragments.every===undefined&&Array.from(fragments).every(r=>r.left>=bound.left-1&&r.right<=bound.right+1&&r.top>=bound.top-1&&r.bottom<=bound.bottom+1),'rects-inline-bounding-envelope');
        const scroller=document.createElement('div');scroller.style.cssText='width:80px;height:25px;overflow:auto';
        const tall=document.createElement('div');tall.style.cssText='width:60px;height:200px';scroller.appendChild(tall);geometry.appendChild(scroller);
        const before=tall.getClientRects()[0].top;scroller.scrollTop=20;eq(tall.getClientRects()[0].top,before-20,'rects-ancestor-scroll');
        const table=document.createElement('table');table.createCaption().textContent='caption';table.insertRow().insertCell().textContent='cell';geometry.appendChild(table);
        eq(table.getClientRects().length,2,'rects-table-caption');
    } finally { geometry.remove(); }
    eq(geometry.getClientRects().length,0,'rects-removed-empty');
}
