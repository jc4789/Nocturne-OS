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
    outer.remove();sheet.remove();return count;
};
