/* Native-response/meta CSP acceptance helpers. Each mode requires a fresh
 * document. Loading this source without delivering its fixture policy is not
 * a test of header/meta authority. The Promise includes queued event checks. */
globalThis.HTMLCSPFixtures={
    enforce:"Content-Security-Policy: require-trusted-types-for 'script'; trusted-types allowed default duplicate 'allow-duplicates'; report-uri /__csp-report\r\nContent-Security-Policy-Report-Only: require-trusted-types-for 'script'; trusted-types allowed default duplicate; report-uri /__csp-report-only\r\n",
    report:"Content-Security-Policy-Report-Only: require-trusted-types-for 'script'; trusted-types allowed default duplicate; report-uri /__csp-report-only\r\n",
    meta:"require-trusted-types-for 'script'; trusted-types allowed default duplicate; report-uri /__csp-meta-must-not-send"
};
globalThis.runHTMLCSPCases=function(mode){
    if(!['enforce','report','meta'].includes(mode))throw new TypeError('Native CSP fixture mode required');
    let count=0;const events=[],strict=mode!=='report',expected=mode==='enforce'?10:6;
    const check=(name,value)=>{count++;if(!value)throw new Error('HTML CSP '+name);};
    const operation=(name,fn,blocked)=>{let error=null;try{fn();}catch(e){error=e;}check(name,blocked?error instanceof TypeError:error===null);};
    function receive(event){events.push(event);}
    document.addEventListener('securitypolicyviolation',receive);
    const node=document.createElement('div');
    operation('raw markup disposition',()=>{node.innerHTML='<i>blocked raw</i>';},strict);
    operation('raw eval disposition',()=>eval('1+2'),strict);
    const poison=()=>{throw new Error('Native policy must define own properties');};
    for(const key of ['required','enforce','policies','requirements'])Object.defineProperty(Object.prototype,key,{set:poison,configurable:true});
    try{operation('native state ignores inherited setters',()=>{node.innerHTML='<i>poison guarded</i>';},strict);}
    finally{for(const key of ['required','enforce','policies','requirements'])delete Object.prototype[key];}
    operation('policy restriction disposition',()=>trustedTypes.createPolicy('disallowed',{}),strict);
    const policy=trustedTypes.createPolicy('allowed',{createHTML:value=>value,createScript:value=>value,createScriptURL:value=>value});
    const html=policy.createHTML('<b>native trusted</b>');html.toString=()=>{throw new Error('Author stringifier must not run');};
    node.innerHTML=html;check('genuine native HTML bypass',node.firstChild.localName==='b');
    function lexical(){const hidden=40;return eval(policy.createScript('hidden+2'));}
    check('direct trusted eval lexical context',lexical()===42);
    const object={};check('nonstring eval identity',eval(object)===object);
    const code=policy.createScript('return 19');code.toString=()=>{throw new Error('Author script stringifier must not run');};
    check('Function native argument slot',Function(code)()===19);
    trustedTypes.createPolicy('duplicate',{});
    operation('duplicate restrictions per policy',()=>trustedTypes.createPolicy('duplicate',{}),mode==='meta');
    let seenType='',seenSink='';
    trustedTypes.createPolicy('default',{createHTML:(value,type,sink)=>{seenType=type;seenSink=sink;return '<b>'+value+'</b>';},createScript:value=>value,createScriptURL:value=>value});
    node.innerHTML='<i>default accepted</i>';check('default HTML policy',node.firstChild.localName==='b'&&seenType==='TrustedHTML'&&seenSink==='Element innerHTML');
    function defaultLexical(){const hidden=72;return eval('hidden+1');}
    check('default direct eval lexical context',defaultLexical()===73);
    operation('second default prohibited',()=>trustedTypes.createPolicy('default',{}),true);
    return new Promise((resolve,reject)=>{
        const started=performance.now();
        function finish(){
            try{
                if(events.length<expected){if(performance.now()-started>2000)throw new Error('CSP native events timed out: '+events.length+'/'+expected);setTimeout(finish,10);return;}
                check('one event per violated native rule',events.length===expected);
                for(const event of events){
                    check('trusted violation subclass',event instanceof SecurityPolicyViolationEvent&&event.isTrusted);
                    check('directive identity',event.effectiveDirective===event.violatedDirective&&['trusted-types','require-trusted-types-for'].includes(event.effectiveDirective));
                    check('original policy retained',event.originalPolicy.includes('trusted-types'));
                    check('actual source rather than sink label',event.sourceFile!=='Element innerHTML'&&event.sourceFile!=='TrustedTypePolicyFactory createPolicy');
                    if(event.effectiveDirective==='require-trusted-types-for')check('sink sample format',event.sample.includes('|')&&event.blockedURI==='trusted-types-sink');
                    else check('policy resource and sample',event.blockedURI==='trusted-types-policy'&&!event.sample.includes('|'));
                }
                check('correct enforcement dispositions',mode==='enforce'?events.some(e=>e.disposition==='enforce')&&events.some(e=>e.disposition==='report'):events.every(e=>e.disposition===(mode==='report'?'report':'enforce')));
                document.removeEventListener('securitypolicyviolation',receive);resolve(count);
            }catch(error){document.removeEventListener('securitypolicyviolation',receive);reject(error);}
        }
        setTimeout(finish,0);
    });
};
