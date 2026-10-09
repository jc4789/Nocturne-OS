/* Only new logical alignment / real rendered tree boundaries. */
async function runScrollIntoViewCases(){
    let count=0;const ok=(name,v)=>{count++;check('into-view-'+name,!!v);};
    const outer=document.createElement('div');outer.style.cssText='position:relative;width:200px;height:160px;overflow:auto';
    const inner=document.createElement('div');inner.style.cssText='position:absolute;left:400px;top:300px;width:100px;height:80px;overflow:auto';
    const filler=document.createElement('div');filler.style.cssText='width:600px;height:500px';inner.appendChild(filler);
    const target=document.createElement('a');target.href='/dir/into-view';target.style.cssText='position:absolute;left:300px;top:220px;width:20px;height:10px;background:rgb(0,255,0)';
    inner.appendChild(target);outer.appendChild(inner);document.body.appendChild(outer);
    const expected={start:[300,220],end:[220,150],center:[260,185],nearest:[220,150]};
    for(const align of ['start','end','center','nearest']){
        inner.scrollTo(0,0);const before=target.getBoundingClientRect();
        target.scrollIntoView({block:align,inline:align,container:'nearest'});
        const pos=expected[align],after=target.getBoundingClientRect();
        ok('align-'+align,inner.scrollLeft===pos[0]&&inner.scrollTop===pos[1]);
        ok('actual-rect-'+align,Math.abs(before.left-after.left-pos[0])<=1&&Math.abs(before.top-after.top-pos[1])<=1);
        ok('nearest-container-'+align,outer.scrollLeft===0&&outer.scrollTop===0);
    }
    inner.scrollTo(0,0);target.scrollIntoView(false);ok('boolean-end',inner.scrollTop===150&&outer.scrollTop>0);
    inner.scrollTo(0,0);outer.scrollTo(0,0);target.scrollIntoView(true);ok('boolean-start',inner.scrollTop===220&&outer.scrollTop>0);
    inner.scrollTo(0,0);outer.scrollTo(0,0);target.scrollIntoView({block:'nearest',inline:'nearest'});
    const visible=target.getBoundingClientRect(),clip=outer.getBoundingClientRect();
    ok('nested-actual-visible',visible.left>=clip.left&&visible.right<=clip.right&&visible.top>=clip.top&&visible.bottom<=clip.bottom);
    for(const [key,value] of [['behavior','invalid'],['block','middle'],['inline','left'],['container','parent']]){
        let thrown=false;try{target.scrollIntoView({[key]:value});}catch(e){thrown=e instanceof TypeError;}ok('invalid-'+key,thrown);
    }
    let thrown=false;try{Element.prototype.scrollIntoView.call({getBoundingClientRect(){return visible;}});}catch(e){thrown=e instanceof TypeError;}ok('native-brand',thrown);
    inner.scrollTo(0,0);outer.scrollTo(0,0);target.style.display='none';target.scrollIntoView();ok('nonrendered-return',inner.scrollTop===0&&outer.scrollTop===0);
    target.style.display='block';target.remove();target.scrollIntoView();ok('detached-return',inner.scrollTop===0&&outer.scrollTop===0);inner.appendChild(target);
    // A native shadow tree participates by its real flattened/box ancestry;
    // no JS parent is added to ShadowRoot or its child.
    const host=document.createElement('section');host.style.cssText='width:100px;height:80px;overflow:auto';
    const shadow=host.attachShadow({mode:'open'}),content=document.createElement('div');content.style.cssText='width:100px;height:400px;position:relative';
    const child=document.createElement('div');child.style.cssText='position:absolute;top:260px;width:20px;height:10px';content.appendChild(child);shadow.appendChild(content);document.body.appendChild(host);
    child.scrollIntoView({block:'nearest',container:'nearest'});ok('shadow-native-scroll',host.scrollTop>0);
    ok('shadow-parent-not-faked',child.parentNode===content&&content.parentNode===shadow&&shadow.parentNode===null);
    const slotHost=document.createElement('section');slotHost.style.cssText='width:100px;height:80px;overflow:auto';
    const slotShadow=slotHost.attachShadow({mode:'open'});const slotContainer=document.createElement('div');slotContainer.style.cssText='height:400px;position:relative';
    const slot=document.createElement('slot');slotContainer.appendChild(slot);slotShadow.appendChild(slotContainer);
    const slotted=document.createElement('div');slotted.style.cssText='position:absolute;top:240px;width:20px;height:10px';slotHost.appendChild(slotted);document.body.appendChild(slotHost);
    slotted.scrollIntoView({block:'nearest',container:'nearest'});ok('slot-native-scroll',slotHost.scrollTop>0);ok('slot-parent-not-faked',slotted.parentNode===slotHost&&slotted.assignedSlot===slot);
    outer.remove();host.remove();slotHost.remove();return count;
}
