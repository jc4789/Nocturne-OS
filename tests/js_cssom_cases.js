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
    const make=(parent,css)=>{const n=document.createElement('div');n.style.cssText=css;parent.appendChild(n);return n;};
    const scope=make(document.body,'position:relative;width:200px;height:100px;padding:10px;border:2px solid;margin:0');
    const positioned=make(scope,'position:absolute;left:10%;top:calc(10% + 5px);width:20px;height:10px;margin:3px 5px;padding:2px;border:1px solid;z-index:-7;box-sizing:content-box');
    const used=getComputedStyle(positioned);
    equal('実左insetのpadding包含基準',used.left,'22px');
    equal('実上insetの百分率とcalc',used.top,'17px');
    equal('auto右insetの実margin矩形',used.right,'162px');
    equal('auto下insetの実margin矩形',used.bottom,'81px');
    equal('負z-indexの実state',used.zIndex,'-7');
    equal('実box-sizing',used.boxSizing,'content-box');
    equal('content-box使用寸法',used.width==='20px'&&used.height==='10px',true);
    positioned.style.boxSizing='border-box';positioned.style.width='100px';positioned.style.height='50px';
    equal('border-box使用寸法',used.width==='100px'&&used.height==='50px',true);
    positioned.style.right='20%';positioned.style.margin='3px auto';
    equal('auto-margin解決時も実inset百分率を解決',used.left==='22px'&&used.right==='44px',true);
    positioned.style.right='auto';positioned.style.margin='3px 5px';
    const wrapped=make(scope,'display:inline-flex;width:80px;height:30px;padding:4px;border:2px solid;box-sizing:content-box');
    const wrapperStyle=getComputedStyle(wrapped);
    equal('inline-flex実principal寸法と辺',wrapperStyle.width==='80px'&&wrapperStyle.height==='30px'&&
          wrapperStyle.paddingLeft==='4px'&&wrapperStyle.borderLeftWidth==='2px',true);
    const contents=make(scope,'display:contents;position:relative;width:50%;height:calc(20% + 3px);left:calc(10% + 2px);top:min(5px,10%);margin-left:12%;padding-left:7%;border:3px solid');
    const noBox=getComputedStyle(contents);
    equal('contentsは架空0pxでなくcomputed長さ',noBox.width==='50%'&&noBox.height.includes('3px')&&
          noBox.height.includes('20%')&&noBox.left.includes('2px')&&noBox.left.includes('10%')&&
          noBox.top.includes('min(')&&noBox.top.includes('5px')&&noBox.top.includes('10%'),true);
    equal('box無しのcomputed辺',noBox.marginLeft==='12%'&&noBox.paddingLeft==='7%'&&noBox.borderLeftWidth==='3px',true);
    const hidden=make(scope,'display:none;width:111px;height:22px;left:25%;top:7px');
    const hiddenStyle=getComputedStyle(hidden);
    equal('display-noneのcomputed寸法とinset',hiddenStyle.width==='111px'&&hiddenStyle.height==='22px'&&
          hiddenStyle.left==='25%'&&hiddenStyle.top==='7px',true);
    const relative=make(scope,'position:relative;left:10px;right:25%;height:10px');
    const relativeStyle=getComputedStyle(relative);
    equal('over-constrained右はcomputed百分率を保持',relativeStyle.left==='10px'&&relativeStyle.right==='25%',true);
    equal('auto z-indexの実初期state',relativeStyle.zIndex,'auto');
    equal('実filter初期none',relativeStyle.filter,'none');
    relative.style.filter='opacity(125%) opacity(.25)';
    equal('filter computed関数列と百分率を保全',relativeStyle.filter,'opacity(125%) opacity(0.25)');
    equal('filter実対応grammarだけsupports',CSS.supports('filter','opacity(1)')&&
          CSS.supports('filter','opacity(125%) opacity(.25)')&&!CSS.supports('filter','blur(2px)'),true);
    relative.style.filter='opacity()';
    equal('filter省略引数の実computed既定',relativeStyle.filter,'opacity(1)');
    relative.style.filter='opacity(-.1)';
    equal('負filter拒否で既存実stateを保持',relativeStyle.filter,'opacity(1)');
    relative.style.filter='none';
    equal('filter noneへのlive無効化',relativeStyle.filter,'none');
    const inlineCB=document.createElement('span');
    inlineCB.style.cssText='filter:opacity(1);padding:4px;border:2px solid;font:20px monospace';
    inlineCB.appendChild(document.createTextNode('inline'));scope.appendChild(inlineCB);
    const inlineAbs=make(inlineCB,'position:absolute;left:25%;top:25%;width:20px;height:10px;margin:0;padding:0;border:0');
    const inlineBounds=inlineCB.getBoundingClientRect(),inlineRect=inlineAbs.getBoundingClientRect(),inlineUsed=getComputedStyle(inlineAbs);
    const inlineW=inlineBounds.width-4,inlineH=inlineBounds.height-4,near=(a,b)=>Math.abs(a-b)<.001;
    equal('inline filter実CBとpercentage inset基準一致',inlineAbs.offsetParent===inlineCB&&
          near(parseFloat(inlineUsed.left),inlineW*.25)&&near(parseFloat(inlineUsed.top),inlineH*.25)&&
          near(inlineRect.left,inlineBounds.left+2+inlineW*.25)&&near(inlineRect.top,inlineBounds.top+2+inlineH*.25),true);
    equal('inline filter auto inset原点と幅高一致',near(parseFloat(inlineUsed.right),inlineW*.75-inlineRect.width)&&
          near(parseFloat(inlineUsed.bottom),inlineH*.75-inlineRect.height),true);
    let brand=false;try{getComputedStyle(document.createTextNode('')).left;}catch(e){brand=e instanceof TypeError;}
    equal('computed-native非element受信者を拒否',brand,true);
    positioned.style.left='30px';equal('保持proxyも現在の実insetを再取得',used.left,'30px');
    const rule=document.createElement('style');rule.textContent='.cssom-live-inset{left:44px!important}';document.head.appendChild(rule);
    positioned.className='cssom-live-inset';const changed=used.left;rule.remove();
    equal('stylesheet変更の実inset無効化',changed==='44px'&&used.left==='30px',true);
    scope.remove();
    console.log('CSSOM_DONE '+count);
    return count;
}
