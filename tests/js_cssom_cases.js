/* CSSOM regressions from the live Amazon jQuery feature probe, plus controls.
   These checks exercise Nocturne's real DOM and do not imply site acceptance. */
function runCSSOMCases() {
    let count=0;
    function equal(name, actual, expected) {
        count++;
        if(actual!==expected) throw new Error(name+': '+JSON.stringify(actual)+' != '+JSON.stringify(expected));
    }
    const a=document.createElement('a');
    a.setAttribute('style','top:1px;float:left;opacity:.55;');
    equal('amazon-opacity-probe',/^0.55$/.test(a.style.opacity),true);
    equal('amazon-float-probe',!!a.style.cssFloat,true);
    equal('float-alias-read',a.style.cssFloat,'left');
    a.style.cssFloat='right';
    equal('float-alias-write',a.style.getPropertyValue('float'),'right');
    equal('no-css-float-property',a.style.getPropertyValue('css-float'),'');
    for(const [input,output] of [['.55','0.55'],['+000.55000','0.55'],['5.5e-1','0.55'],['-0','0'],['55%','55%']]) {
        a.style.opacity=input;equal('opacity-'+input,a.style.opacity,output);
    }
    a.style.opacity='var(--alpha)';equal('opacity-var-preserved',a.style.opacity,'var(--alpha)');
    a.style.cssText='opacity:.55 ! IMPORTANT;float:left!important;content:"!important; :";--Case:.55;--case:.7;';
    equal('important-value',a.style.opacity,'0.55');
    equal('important-priority',a.style.getPropertyPriority('OPACITY'),'important');
    equal('quoted-value',a.style.getPropertyValue('content'),'"!important; :"');
    equal('quoted-priority',a.style.getPropertyPriority('content'),'');
    equal('custom-case-upper',a.style.getPropertyValue('--Case'),'.55');
    equal('custom-case-lower',a.style.getPropertyValue('--case'),'.7');
    equal('remove-return-value',a.style.removeProperty('opacity'),'0.55');
    equal('remove-value',a.style.opacity,'');
    equal('remove-priority',a.style.getPropertyPriority('opacity'),'');
    a.style.setProperty('opacity','.25','important');
    equal('setter-important-value',a.style.opacity,'0.25');
    equal('setter-important-priority',a.style.getPropertyPriority('opacity'),'important');
    a.style.setProperty('opacity','.5','wrong');
    equal('invalid-priority-no-change',a.style.opacity,'0.25');
    a.style.setProperty('opacity','.75 !important');
    equal('value-important-rejected',a.style.opacity,'0.25');
    a.style.cssText='opacity:.2!important;opacity:.7;';
    equal('important-cascade',a.style.opacity,'0.2');
    a.style.cssText='opacity:.2;opacity:.7;';
    equal('last-declaration',a.style.opacity,'0.7');
    a.style.cssText='opacity:.3 !/**/important/**/;';
    equal('comment-priority-value',a.style.opacity,'0.3');
    equal('comment-priority',a.style.getPropertyPriority('opacity'),'important');
    console.log('CSSOM_DONE '+count);
    return count;
}
