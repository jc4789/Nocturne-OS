/* Native semantics/disclosure regressions for jstest's web_live route.
 * This is not a synthetic HTML5test score or modal-dialog/ruby acceptance. */
async function runSemanticElementCases() {
    let count=0;
    const ok=(value,name)=>{count++;check('semantic-'+name,!!value);if(!value)throw new Error(name);};
    const eq=(a,b,name)=>ok(Object.is(a,b),name);
    const rejects=(fn,name)=>{let e;try{fn();}catch(error){e=error;}ok(e instanceof TypeError,'reject-'+name);};
    const idle=()=>new Promise(resolve=>setTimeout(resolve,0));
    const fixture=document.createElement('div');document.body.appendChild(fixture);
    try {
        for(const tag of ['section','nav','article','aside','header','footer','main','figure','figcaption','mark','summary','wbr']){
            const node=document.createElement(tag);fixture.appendChild(node);
            ok(node instanceof HTMLElement && !(node instanceof HTMLUnknownElement),tag+'-native-recognition');
            eq(Object.getPrototypeOf(node),HTMLElement.prototype,tag+'-generic-interface');
            eq(node.cloneNode(false).tagName,tag.toUpperCase(),tag+'-clone');
            eq(document.createElementNS('http://www.w3.org/1999/xhtml',tag) instanceof HTMLElement,true,tag+'-namespace');
            if(['section','nav','article','aside','header','footer','main','figure','figcaption'].includes(tag))
                eq(getComputedStyle(node).display,'block',tag+'-native-block');
            if(['section','nav','article','aside','header','footer','figure'].includes(tag)){
                const parsed=document.createElement('div');parsed.innerHTML='<p><'+tag+'></'+tag+'>';
                eq(parsed.childNodes.length,2,tag+'-implicit-p-close');
                eq(parsed.firstChild.tagName,'P',tag+'-p-sibling');
            }
            node.remove();
        }
        const unknown=document.createElement('notanhtmlelement');
        ok(unknown instanceof HTMLUnknownElement && unknown instanceof HTMLElement,'actual-unknown-interface');
        eq(Object.getPrototypeOf(unknown),HTMLUnknownElement.prototype,'unknown-native-prototype');
        eq(document.createElement('x-semantic-fixture') instanceof HTMLUnknownElement,false,'custom-candidate-not-unknown');
        rejects(()=>new HTMLUnknownElement(),'unknown-constructor');
        const marker=document.createElement('mark');fixture.appendChild(marker);marker.textContent='highlight';
        eq(getComputedStyle(marker).backgroundColor,'rgba(255, 255, 0, 1)','mark-real-highlight');
        eq(getComputedStyle(marker).color,'rgba(0, 0, 0, 1)','mark-real-foreground');marker.remove();
        const interfaces=[['time',HTMLTimeElement],['data',HTMLDataElement],['details',HTMLDetailsElement],
            ['ol',HTMLOListElement],['li',HTMLLIElement]];
        const parsed=new DOMParser().parseFromString('<time datetime="2026-10-07">today</time><data value="7">seven</data>'+
            '<details><summary>parsed</summary>body</details><ol reversed start="3"><li value="8">list</li></ol>','text/html');
        for(const [tag,C] of interfaces){
            const created=document.createElement(tag),found=parsed.querySelector(tag);
            ok(created instanceof C && created instanceof HTMLElement,tag+'-native-created-brand');
            eq(Object.getPrototypeOf(created),C.prototype,tag+'-actual-prototype');
            eq(Object.getPrototypeOf(C.prototype),HTMLElement.prototype,tag+'-inheritance');
            eq(Object.prototype.toString.call(created),'[object '+C.name+']',tag+'-tag');
            ok(found instanceof C,tag+'-parser-native-brand');
            eq(document.createElementNS('http://www.w3.org/1999/xhtml',tag) instanceof C,true,tag+'-html-brand');
            eq(document.createElementNS('http://www.w3.org/2000/svg',tag) instanceof C,false,tag+'-no-svg-brand');
            eq(document.createElement('div') instanceof C,false,tag+'-no-div-brand');
            rejects(()=>new C(),tag+'-constructor');
            const clone=found.cloneNode(true);ok(clone instanceof C && clone!==found,tag+'-native-clone');
            eq(clone.outerHTML,found.outerHTML,tag+'-clone-content');
            const imported=document.importNode(found,true);ok(imported instanceof C && imported.ownerDocument===document,tag+'-native-import');
        }
        for(const [tag,C,property,attribute] of [['time',HTMLTimeElement,'dateTime','datetime'],['data',HTMLDataElement,'value','value'],
            ['details',HTMLDetailsElement,'name','name'],['ol',HTMLOListElement,'type','type']]){
            const n=document.createElement(tag),d=Object.getOwnPropertyDescriptor(C.prototype,property);
            eq(n[property],'',property+'-default');n[property]='native';eq(n.getAttribute(attribute),'native',property+'-native-write');
            n.setAttribute(attribute,'attribute');eq(n[property],'attribute',property+'-native-read');
            n.setAttributeNS('urn:semantic','p:'+attribute,'namespaced');n.removeAttribute(attribute);
            eq(n[property],'',property+'-null-namespace-only');n[property]=null;eq(n[property],'null',property+'-null-domstring');
            n[property]=undefined;eq(n[property],'undefined',property+'-undefined-domstring');
            rejects(()=>d.get.call(document.createElement('span')),property+'-wrong-tag');
            rejects(()=>d.set.call(Object.create(C.prototype),'bad'),property+'-forged-brand');
            rejects(()=>{n[property]=Symbol();},property+'-symbol');
        }
        for(const [tag,C,property] of [['details',HTMLDetailsElement,'open'],['ol',HTMLOListElement,'reversed']]){
            const n=document.createElement(tag),d=Object.getOwnPropertyDescriptor(C.prototype,property);
            eq(n[property],false,property+'-default');n[property]=true;eq(n.getAttribute(property),'',property+'-native-boolean');
            n.setAttribute(property,'false');eq(n[property],true,property+'-presence-not-string');
            n[property]=0;eq(n.hasAttribute(property),false,property+'-false-removes');
            n[property]='';eq(n[property],false,property+'-empty-false');n[property]='false';eq(n[property],true,property+'-truthy');
            rejects(()=>d.get.call(document.createElement('span')),property+'-wrong-brand');
        }
        for(const [tag,C,property,fallback] of [['ol',HTMLOListElement,'start',1],['li',HTMLLIElement,'value',0]]){
            const n=document.createElement(tag),d=Object.getOwnPropertyDescriptor(C.prototype,property);
            eq(n[property],fallback,property+'-integer-default');
            for(const [text,expected] of [['',fallback],['bogus',fallback],[' -7tail',-7],['+3',3],['0',0],['-0',0],['2147483648',fallback]]){
                n.setAttribute(property,text);eq(n[property],expected,property+'-parse-'+text);
            }
            for(const [value,expected] of [[9.9,9],[-9.9,-9],[4294967297,1],[2147483648,-2147483648],[null,0],[undefined,0],[NaN,0],[Infinity,0]]){
                n[property]=value;eq(n[property],expected,property+'-convert-'+String(value));
                eq(n.getAttribute(property),String(expected),property+'-reflected-'+String(value));
            }
            rejects(()=>d.get.call(document.createElement('span')),property+'-wrong-tag');
            rejects(()=>{n[property]=Symbol();},property+'-symbol');
            rejects(()=>{n[property]=1n;},property+'-bigint');
        }
        const disclosure=document.createElement('details');
        disclosure.innerHTML='before<summary><span id="semantic-summary-child">heading</span></summary>text'+
            '<p id="semantic-details-body">body</p><summary id="semantic-second-summary">second</summary>';
        fixture.appendChild(disclosure);
        const summary=disclosure.querySelector('summary'),body=disclosure.querySelector('p'),second=disclosure.querySelector('#semantic-second-summary');
        const closedHeight=disclosure.offsetHeight;
        ok(closedHeight>0,'closed-summary-visible');eq(body.offsetHeight,0,'closed-body-no-native-box');
        eq(second.offsetHeight,0,'closed-second-summary-hidden');eq(getComputedStyle(body).display,'block','hidden-content-computed-display-preserved');
        disclosure.open=true;ok(disclosure.offsetHeight>closedHeight,'open-real-layout-height');
        ok(body.offsetHeight>0 && second.offsetHeight>0,'open-all-content-boxes');
        disclosure.removeAttribute('open');eq(disclosure.offsetHeight,closedHeight,'attribute-closes-native-renderer');
        summary.click();eq(disclosure.open,true,'summary-native-activation');summary.click();eq(disclosure.open,false,'summary-native-close');
        second.click();eq(disclosure.open,false,'nonfirst-summary-no-activation');
        const cancel=event=>event.preventDefault();summary.addEventListener('click',cancel);summary.click();
        eq(disclosure.open,false,'canceled-click-no-default');summary.removeEventListener('click',cancel);
        disclosure.querySelector('span').click();eq(disclosure.open,true,'summary-child-native-activation');
        const button=document.createElement('button');button.type='button';button.textContent='button';summary.appendChild(button);
        button.click();eq(disclosure.open,true,'interactive-descendant-no-toggle');
        await idle();
        const events=[];let bubbled=0,handlerEvent=null;
        disclosure.addEventListener('toggle',event=>events.push(event));fixture.addEventListener('toggle',()=>bubbled++);
        disclosure.ontoggle=event=>{handlerEvent=event;};
        disclosure.open=false;disclosure.open=true;disclosure.open=false;
        eq(events.length,0,'toggle-not-synchronous');await Promise.resolve();eq(events.length,0,'toggle-not-microtask');
        await idle();eq(events.length,1,'toggle-element-task-coalesced');
        eq(events[0].oldState,'open','toggle-first-old-state');eq(events[0].newState,'closed','toggle-last-new-state');
        ok(events[0] instanceof ToggleEvent && events[0].isTrusted,'native-toggle-event');
        eq(events[0].bubbles,false,'toggle-nonbubble');eq(events[0].cancelable,false,'toggle-noncancelable');
        eq(events[0].source,null,'details-toggle-no-source');eq(bubbled,0,'toggle-no-parent-bubble');
        eq(handlerEvent,events[0],'ontoggle-native-handler');
        disclosure.setAttribute('open','');await idle();eq(events.length,2,'setattribute-toggle-task');
        disclosure.setAttribute('open','changed-presence');await idle();eq(events.length,2,'unchanged-presence-no-toggle');
        disclosure.ontoggle=null;
        const empty=document.createElement('details');fixture.appendChild(empty);
        ok(empty.offsetHeight>0,'default-summary-real-layout');eq(empty.childNodes.length,0,'default-summary-not-fake-dom');
        empty.textContent='hidden detail body';const emptyClosed=empty.offsetHeight;empty.open=true;
        ok(empty.offsetHeight>emptyClosed,'default-summary-body-disclosure');eq(empty.childNodes.length,1,'default-summary-still-not-dom');
        const a=document.createElement('details'),b=document.createElement('details');a.name=b.name='semantic-group';
        fixture.appendChild(a);fixture.appendChild(b);
        const ordered=[];a.addEventListener('toggle',event=>ordered.push('a:'+event.oldState+':'+event.newState));
        b.addEventListener('toggle',event=>ordered.push('b:'+event.oldState+':'+event.newState));
        a.open=true;b.open=true;
        eq(a.open,false,'named-group-closes-old-member');eq(b.open,true,'named-group-opens-new-member');
        await idle();eq(ordered.join(','),'b:closed:open,a:closed:closed','toggle-coalescing-requeues-task-after-other-element');
        a.setAttribute('open','');eq(b.open,false,'attribute-enforces-named-group');
        b.open=true;a.name='';a.open=true;ok(a.open && b.open,'empty-name-is-not-a-group');
        a.name='semantic-group';eq(a.open,false,'name-change-closes-new-conflict');
        const groupParsed=new DOMParser().parseFromString('<details id="semantic-first" name="same" open></details>'+
            '<details id="semantic-last" name="same" open></details>','text/html');
        eq(groupParsed.querySelector('#semantic-first').open,true,'parser-keeps-first-group-member');
        eq(groupParsed.querySelector('#semantic-last').open,false,'parser-closes-second-group-member');
        const host=document.createElement('div');fixture.appendChild(host);const shadow=host.attachShadow({mode:'open'});
        const shadowDetails=document.createElement('details');shadowDetails.name='semantic-group';shadow.appendChild(shadowDetails);shadowDetails.open=true;
        ok(shadowDetails.open && b.open,'name-groups-do-not-cross-shadow-trees');
        const source=document.createElement('span'),event=new ToggleEvent('toggle',{oldState:'closed',newState:'open',source});
        eq(event.oldState,'closed','toggle-constructor-old');eq(event.newState,'open','toggle-constructor-new');
        eq(event.source,source,'toggle-constructor-source');eq(event.isTrusted,false,'constructed-toggle-untrusted');
        const getter=Object.getOwnPropertyDescriptor(ToggleEvent.prototype,'oldState').get;
        rejects(()=>getter.call(new Event('toggle')),'toggle-event-brand');rejects(()=>new ToggleEvent(),'toggle-type-required');
        rejects(()=>new ToggleEvent('toggle',{source:{}}),'toggle-source-not-element');
        const wrap=document.createElement('div');wrap.style.cssText='width:90px;font-family:monospace;font-size:16px;line-height:20px';
        wrap.innerHTML='<span id="semantic-left">AAAAAA</span><wbr><span id="semantic-right">BBBBBB</span>';fixture.appendChild(wrap);
        const narrow=wrap.offsetHeight;
        wrap.style.width='400px';const wide=wrap.offsetHeight;
        ok(narrow>wide,'wbr-real-soft-line-break');ok(wide>0,'wbr-not-forced-line-break');
        wrap.style.width='90px';wrap.style.whiteSpace='nowrap';
        eq(wrap.offsetHeight,wide,'wbr-obeys-nowrap');
    } finally { fixture.remove(); }
    return count;
}
