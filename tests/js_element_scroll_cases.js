/* New scrolling boundaries only; native/real-site acceptance is separate. */
async function runElementScrollCases(){
    let count=0;const ok=(name,v)=>{count++;check('element-scroll-'+name,!!v);};
    const s=document.createElement('div');s.style.cssText='width:100px;height:60px;overflow:auto;position:relative';
    const inner=document.createElement('div');inner.style.cssText='width:500px;height:300px';s.appendChild(inner);document.body.appendChild(s);
    ok('native-dimensions',s.scrollWidth===500&&s.scrollHeight===300);
    const original=inner.getBoundingClientRect(),left=inner.offsetLeft,top=inner.offsetTop;
    s.scroll(25.5,30.25);ok('fractional',s.scrollLeft===25.5&&s.scrollTop===30.25);
    const moved=inner.getBoundingClientRect();ok('visual-rect',Math.abs(original.left-moved.left-25.5)<1.1&&Math.abs(original.top-moved.top-30.25)<1.1);
    ok('layout-offset-stable',inner.offsetLeft===left&&inner.offsetTop===top);
    s.scrollTo({top:80});ok('omitted-left',s.scrollLeft===25.5&&s.scrollTop===80);
    s.scrollBy({left:10});ok('relative',s.scrollLeft===35.5&&s.scrollTop===80);
    s.scrollLeft=50.25;s.scrollTop=60.5;ok('native-setter',s.scrollLeft===50.25&&s.scrollTop===60.5);
    s.scrollTo(1e300,1e300);ok('upper-clamp',s.scrollLeft===400&&s.scrollTop===240);
    s.scrollTo(NaN,Infinity);ok('finite',s.scrollLeft===0&&s.scrollTop===0);
    for(const method of ['scroll','scrollTo','scrollBy']){
        for(const args of [[1],[1n,2],[Symbol(),2],[{behavior:'unknown',left:2}]]){
            let bad=false;try{s[method](...args);}catch(e){bad=e instanceof TypeError;}ok('IDL-'+method,bad);
        }
        let bad=false;try{Element.prototype[method].call({clientWidth:100,getBoundingClientRect(){return original;}},0,0);}catch(e){bad=e instanceof TypeError;}ok('brand-'+method,bad);
    }
    const order=[];s.scrollTo({get behavior(){order.push('b');return 'instant';},get left(){order.push('l');return 3;},get top(){order.push('t');return 4;}});
    ok('dictionary-order',order.join(',')==='b,l,t');
    s.scrollTo({get left(){s.scroll(40,50);return undefined;},top:60});ok('reentry',s.scrollLeft===40&&s.scrollTop===60);
    s.style.overflow='hidden';s.scroll(20,30);ok('hidden-programmatic',s.scrollLeft===20&&s.scrollTop===30);
    s.style.overflow='clip';s.scroll(50,70);ok('clip-no-scroll',s.scrollLeft===0&&s.scrollTop===0);
    s.style.overflow='visible';s.scroll(50,70);ok('visible-no-scroll',s.scrollLeft===0&&s.scrollTop===0);
    s.style.overflow='auto';s.scroll(70,80);s.style.display='none';ok('hidden-no-box',s.scrollLeft===0&&s.scrollTop===0&&s.scrollWidth===0);
    s.style.display='block';ok('box-rebuild-persists',s.scrollLeft===70&&s.scrollTop===80);
    s.remove();s.scroll(90,100);ok('detached-no-update',s.scrollLeft===0&&s.scrollTop===0);document.body.appendChild(s);
    ok('reinsert-persists',s.scrollLeft===70&&s.scrollTop===80);
    inner.style.width='10px';inner.style.height='10px';ok('shrink-clamp',s.scrollLeft===0&&s.scrollTop===0);
    inner.style.width='500px';inner.style.height='300px';
    let events=0;s.addEventListener('scroll',e=>{events++;ok('event-properties',e.isTrusted&&!e.bubbles&&!e.cancelable);});
    s.scroll(1,2);s.scrollBy(2,3);s.scrollTop=7;ok('event-not-inline',events===0);
    await new Promise(resolve=>setTimeout(resolve,20));ok('event-coalesced',events===1);
    s.scrollBy(0,0);await new Promise(resolve=>setTimeout(resolve,20));ok('unchanged-no-event',events===1);
    s.remove();return count;
}

/* Real native value/line layout, not the host coordinate shim. */
function runTextareaScrollCases(){
    let count=0;const ok=(name,value)=>{count++;check('textarea-scroll-'+name,!!value);};
    const t=document.createElement('textarea');
    t.style.cssText='display:block;box-sizing:content-box;width:80px;height:0;padding:3px 4px;border:2px solid black;font:16px monospace;line-height:20px;white-space:pre-wrap;overflow:auto';
    document.body.appendChild(t);
    ok('empty-line',t.scrollHeight===26);
    ok('zero-content-height',t.getBoundingClientRect().height===10);
    t.value='a\nb\n';ok('final-empty-line',t.scrollHeight===66);
    t.value='a\r\nb\r';ok('normalized-line-breaks',t.value==='a\nb\n'&&t.scrollHeight===66);
    t.value='M'.repeat(40);const wrapped=t.scrollHeight;
    ok('soft-wrap',wrapped>26);
    t.style.whiteSpace='';t.wrap='off';ok('wrap-off',t.scrollHeight===26&&t.scrollWidth>88);
    t.style.whiteSpace='pre-wrap';ok('author-overrides-wrap-hint',t.scrollHeight===wrapped);
    t.wrap='soft';ok('wrap-restored',t.scrollHeight===wrapped);
    t.style.whiteSpace='pre';ok('css-nowrap',t.scrollHeight===26);
    t.style.whiteSpace='pre-wrap';ok('css-wrap-restored',t.scrollHeight===wrapped);
    t.style.width='0px';
    for(const [name,value] of [['combining','e\u0301'],['emoji-zwj','\ud83d\udc69\u200d\ud83d\udcbb'],['regional-pair','\ud83c\uddef\ud83c\uddf5']]){
        t.value=value;ok('whole-grapheme-'+name,t.scrollHeight===26);
    }
    t.value='ab';ok('zero-width-progress',t.scrollHeight===46);
    t.style.width='80px';t.value='a\nb\nc\nd';t.style.height='20px';
    t.setSelectionRange(2,5,'backward');t.scrollTop=40;
    ok('native-scroll-range',t.scrollTop===40);
    ok('selection-preserved',t.selectionStart===2&&t.selectionEnd===5&&t.selectionDirection==='backward');
    t.scrollTop=1e6;ok('native-scroll-clamp',t.scrollTop===60);
    t.value='one';ok('value-change-remeasures',t.scrollHeight===26&&t.scrollTop===0);
    t.style.height='0px';t.value='one\ntwo';const measured=t.scrollHeight;
    t.style.height=(measured-6)+'px';ok('author-autosize',t.clientHeight===measured&&t.scrollHeight===measured);
    t.style.display='none';ok('display-none',t.scrollHeight===0);
    t.style.display='block';ok('display-restored',t.scrollHeight===measured);
    t.remove();ok('detached',t.scrollHeight===0);document.body.appendChild(t);ok('reconnected',t.scrollHeight===measured);
    t.value='';t.placeholder='first\nsecond';t.style.height='0px';ok('placeholder-lines',t.scrollHeight===46);
    const host=document.createElement('div'),root=host.attachShadow({mode:'open'}),inner=document.createElement('textarea');
    inner.style.cssText=t.style.cssText;inner.value='shadow\nvalue';root.appendChild(inner);document.body.appendChild(host);
    ok('shadow-native-value',inner.scrollHeight===46);
    const outer=document.createElement('div');outer.style.cssText='width:150px;height:20px;overflow:auto';
    t.style.cssText='display:block;width:80px;height:0;padding:0;border:0;margin:0;font:16px monospace;line-height:20px;overflow:visible';
    t.placeholder='';t.value='M'.repeat(160);outer.appendChild(t);document.body.appendChild(outer);
    ok('private-native-scroll-area',t.scrollHeight>20);
    ok('private-content-does-not-enlarge-parent',outer.scrollHeight===20);
    outer.remove();host.remove();return count;
}
