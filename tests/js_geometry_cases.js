/* Native geometry and invalidation contracts; live website acceptance is separate. */
globalThis.runGeometryCases = function() {
    let count=0;
    const assert=(ok,name)=>{count++;check('geometry-'+name,ok);if(!ok)throw new Error(name);};
    const sheet=document.createElement('style');
    sheet.textContent='.geometry-wide{width:150px !important}';document.head.appendChild(sheet);
    const outer=document.createElement('div');
    outer.style.cssText='position:relative;margin:0;padding:10px;border:3px solid;width:300px;height:100px';
    const inner=document.createElement('div');
    inner.style.cssText='position:absolute;left:21px;top:12px;width:100px;height:30px;padding:4px 6px;border:2px solid;margin:5px 7px';
    outer.appendChild(inner);document.body.appendChild(outer);
    assert(inner.offsetParent===outer,'offset-parent');
    assert(inner.offsetLeft===28&&inner.offsetTop===17,'offset-position');
    assert(inner.offsetWidth===116&&inner.offsetHeight===42,'border-box');
    assert(inner.clientWidth===112&&inner.clientHeight===38,'padding-box');
    assert(inner.clientLeft===2&&inner.clientTop===2,'client-border');
    assert(getComputedStyle(inner).marginRight==='7px'&&getComputedStyle(inner).paddingTop==='4px','used-spacing');
    let sum=0;for(let i=0;i<100;i++)sum+=inner.offsetWidth+inner.offsetLeft;
    assert(sum===14400,'stable-repeat');
    inner.className='geometry-wide';assert(inner.offsetWidth===166,'class-invalidates');
    inner.className='';assert(inner.offsetWidth===116,'class-remove-invalidates');
    inner.style.width='120px';assert(inner.offsetWidth===136,'style-invalidates');
    inner.remove();assert(inner.offsetParent===null&&inner.offsetWidth===0&&inner.clientWidth===0,'detached');
    outer.appendChild(inner);assert(inner.offsetWidth===136,'reattach');
    inner.style.display='none';assert(inner.offsetWidth===0&&inner.offsetParent===null,'display-none');
    inner.style.display='block';inner.style.position='fixed';assert(inner.offsetParent===null,'fixed-parent');
    assert(document.body.offsetTop===0&&document.body.offsetLeft===0,'body-origin');
    const make=(parent,tag,css)=>{const n=document.createElement(tag);if(css)n.style.cssText=css;parent.appendChild(n);return n;};
    const host=make(document.body,'div','position:relative;margin-left:70px;width:200px;height:80px;padding:5px;border:2px solid');
    const shadow=host.attachShadow({mode:'open'});
    const child=make(shadow,'div','position:absolute;left:11px;top:13px;width:20px;height:10px');
    assert(child.offsetParent===host,'shadow-host-offset-parent');
    const hr=host.getBoundingClientRect(),cr=child.getBoundingClientRect();
    assert(child.offsetLeft===11&&child.offsetTop===13&&cr.left===hr.left+host.clientLeft+11&&cr.top===hr.top+host.clientTop+13,
           'shadow-local-and-viewport-origins');
    const closedHost=make(document.body,'div','position:relative;width:200px;height:80px');
    const closed=closedHost.attachShadow({mode:'closed'});
    const privateChild=make(closed,'div','position:absolute;left:7px;top:9px;width:20px;height:10px');
    assert(privateChild.offsetParent===closedHost,'closed-root-own-child-host-parent');
    const openSlot=make(shadow,'slot','display:block;position:relative;width:100px;height:40px');
    const slotted=make(host,'div','position:absolute;left:3px;top:4px;width:10px;height:8px');
    assert(slotted.offsetParent===openSlot,'open-slot-flat-parent');
    const closedSlot=make(closed,'slot','display:block;position:relative;width:100px;height:40px');
    const closedLight=make(closedHost,'div','position:absolute;left:3px;top:4px;width:10px;height:8px');
    assert(closedLight.offsetParent===closedHost,'closed-slot-not-exposed-to-light-child');
    const nestedHost=make(document.body,'div','position:relative;width:200px;height:80px');
    const nestedClosed=nestedHost.attachShadow({mode:'closed'});
    const openHost=make(nestedClosed,'div','display:block;width:100px;height:40px');
    const nestedOpen=openHost.attachShadow({mode:'open'});
    make(nestedOpen,'slot','display:block;position:relative;width:100px;height:40px');
    make(openHost,'slot','display:block');
    const nestedLight=make(nestedHost,'div','position:absolute;left:2px;top:3px;width:10px;height:8px');
    assert(nestedLight.offsetParent===nestedHost,'open-root-under-closed-root-not-exposed');
    closedSlot.style.position='fixed';
    assert(closedLight.offsetParent===null,'closed-hidden-fixed-ancestor-stops-offset-parent');
    const contents=make(shadow,'div','display:contents;position:relative');
    const contentsChild=make(contents,'div','position:absolute;left:5px;top:6px;width:10px;height:8px');
    assert(contentsChild.offsetParent===host,'boxless-composed-parent-does-not-establish-offset-parent');
    host.remove();
    assert(child.offsetParent===null&&slotted.offsetParent===null,'detached-shadow-and-slot-no-offset-parent');
    closedHost.remove();nestedHost.remove();
    const fractionalScope=make(document.body,'div','position:relative;width:150px;height:80px;margin:0;padding:0;border:0');
    const wrapped=make(fractionalScope,'div','display:inline-flex;width:80px;height:30px;padding:4px;border:3px solid;margin:7px');
    const wr=wrapped.getBoundingClientRect(),wf=wrapped.getClientRects()[0];
    assert(wrapped.clientLeft===3&&wrapped.clientTop===3&&wrapped.clientWidth===88&&wrapped.clientHeight===38&&
           wr.width===94&&wr.height===44&&wf.width===wr.width&&wf.height===wr.height,
           'wrapped-principal-client-border-and-fragment');
    const fractional=make(fractionalScope,'div','position:absolute;left:10.25px;top:11.5px;width:20.75px;height:9.125px;margin:0;padding:0;border:0');
    const sr=fractionalScope.getBoundingClientRect(),fr=fractional.getBoundingClientRect(),ff=fractional.getClientRects()[0];
    assert(fr.left-sr.left===10.25&&fr.top-sr.top===11.5&&fr.width===20.75&&fr.height===9.125&&
           fr.x===ff.x&&fr.y===ff.y&&fr.width===ff.width&&fr.height===ff.height,
           'fractional-bounding-and-client-fragment-share-native-values');
    fractional.style.width='21.375px';
    assert(fr.width===20.75&&fractional.getBoundingClientRect().width===21.375,'fractional-rect-is-snapshot');
    const line=make(fractionalScope,'div','width:40px');
    const inline=make(line,'span','padding:2px;border:1px solid');inline.textContent='one two three four five six';
    assert(inline.clientLeft===0&&inline.clientTop===0&&inline.clientWidth===0&&inline.clientHeight===0,
           'inline-client-members-ignore-fragment-padding-and-border');
    const pieces=Array.from(inline.getClientRects()),area=pieces.filter(r=>r.width!==0||r.height!==0),ir=inline.getBoundingClientRect();
    const l=Math.min(...area.map(r=>r.left)),t=Math.min(...area.map(r=>r.top)),
          r=Math.max(...area.map(r=>r.right)),b=Math.max(...area.map(r=>r.bottom));
    assert(pieces.length>1&&ir.left===l&&ir.top===t&&ir.width===r-l&&ir.height===b-t,
           'multi-line-native-fragment-bounding-union');
    const zero=make(fractionalScope,'div','position:absolute;left:1.25px;top:2.5px;width:0;height:0;margin:0;padding:0;border:0');
    const zr=zero.getBoundingClientRect(),zf=zero.getClientRects()[0];
    assert(zr.x-sr.x===1.25&&zr.y-sr.y===2.5&&zr.width===0&&zr.height===0&&zr.x===zf.x&&zr.y===zf.y,
           'zero-area-first-fragment-retains-fractional-origin');
    fractionalScope.remove();
    const detachedRect=fractional.getBoundingClientRect();
    assert(detachedRect.x===0&&detachedRect.y===0&&detachedRect.width===0&&detachedRect.height===0&&fractional.getClientRects().length===0,
           'detached-fractional-fragment-is-empty');
    const filteredHost=make(document.body,'div','filter:opacity(1);margin-left:80px;width:200px;height:100px;padding:4px;border:3px solid');
    const filteredShadow=filteredHost.attachShadow({mode:'open'});
    const filteredAbs=make(filteredShadow,'div','position:absolute;left:11px;top:13px;width:20px;height:10px');
    const filteredFixed=make(filteredShadow,'div','position:fixed;left:17px;top:19px;width:20px;height:10px');
    const fhr=filteredHost.getBoundingClientRect(),far=filteredAbs.getBoundingClientRect(),fxr=filteredFixed.getBoundingClientRect();
    assert(filteredAbs.offsetParent===filteredHost&&far.left===fhr.left+filteredHost.clientLeft+11&&
           far.top===fhr.top+filteredHost.clientTop+13,'real-filter-shadow-absolute-containing-block');
    assert(filteredFixed.offsetParent===filteredHost&&fxr.left===fhr.left+filteredHost.clientLeft+17&&
           fxr.top===fhr.top+filteredHost.clientTop+19,'real-filter-shadow-fixed-containing-block');
    filteredHost.style.filter='none';
    const viewportFixed=filteredFixed.getBoundingClientRect();
    assert(filteredFixed.offsetParent===null&&filteredAbs.offsetParent===document.body&&viewportFixed.left===17&&viewportFixed.top===19,
           'filter-none-removes-actual-containing-block');
    filteredHost.remove();
    outer.remove();sheet.remove();return count;
};
