/* Supplemental API checks; call in the real browser host, never as site proof. */
function runMediaCases() {
    let count=0;
    const eq=(a,b)=>{count++;if(!Object.is(a,b))throw new Error(String(a)+' != '+String(b));};
    const throws=fn=>{count++;try{fn();}catch(e){if(e.name==='TypeError')return;throw e;}throw new Error('Expected TypeError');};
    const w=innerWidth,h=innerHeight, m=matchMedia('screen and (min-width: '+w+'px)');
    eq(m instanceof MediaQueryList,true);eq(m instanceof EventTarget,true);eq(m.matches,true);
    eq(m.media,'screen and (min-width: '+w+'px)');eq(m.onchange,null);
    eq(matchMedia('(width: '+w+'px)').matches,true);eq(matchMedia('(height: '+h+'px)').matches,true);
    eq(matchMedia('(width > '+w+'px)').matches,false);eq(matchMedia('(width >= '+w+'px)').matches,true);
    eq(matchMedia('(scripting:enabled)').matches,true);eq(matchMedia('(scripting:none)').matches,false);
    eq(matchMedia('(prefers-reduced-motion:reduce)').matches,false);
    eq(matchMedia('not (unknown-feature)').matches,false);eq(matchMedia('screen trailing').matches,false);
    eq(matchMedia('(width: '+w+'px').matches,false);eq(matchMedia('not screen trailing').matches,false);
    eq(matchMedia('print, screen').matches,true);eq(matchMedia('').matches,true);
    const many=matchMedia(','.repeat(40)+'screen');eq(many.matches,true);eq(many.media.endsWith(', screen'),true);
    eq(matchMedia('(min-width:1px)')===matchMedia('(min-width:1px)'),false);
    throws(()=>matchMedia());throws(()=>matchMedia(Symbol()));throws(()=>new MediaQueryList());
    const getter=Object.getOwnPropertyDescriptor(MediaQueryList.prototype,'matches').get;
    throws(()=>getter.call({}));throws(()=>MediaQueryList.prototype.addListener.call({},()=>{}));
    const e=new MediaQueryListEvent('change',{media:'(color)',matches:true});
    eq(e instanceof Event,true);eq(e.matches,true);eq(e.media,'(color)');eq(e.bubbles,false);eq(e.cancelable,false);eq(e.isTrusted,false);
    eq(Object.prototype.toString.call(m),'[object MediaQueryList]');eq(Object.prototype.toString.call(e),'[object MediaQueryListEvent]');
    let seen=0, handler=0;
    function listener(event){eq(this,m);eq(event.target,m);seen++;}
    m.addListener(listener);m.addEventListener('change',listener);m.onchange=()=>handler++;
    m.dispatchEvent(e);eq(seen,1);eq(handler,1);
    m.removeListener(listener);m.dispatchEvent(new MediaQueryListEvent('change'));eq(seen,1);eq(handler,2);
    m.onchange=7;eq(m.onchange,null);
    m.addListener(listener);m.removeEventListener('change',listener);m.dispatchEvent(new MediaQueryListEvent('change'));eq(seen,1);
    const object={handleEvent(){seen++;}};m.addListener(object);m.dispatchEvent(new MediaQueryListEvent('change'));eq(seen,2);m.removeListener(object);
    return count;
}
