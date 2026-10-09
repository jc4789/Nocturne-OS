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
