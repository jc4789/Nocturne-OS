/* Product dedicated-Worker fixture. Serve this same file as plain/enforce/
 * report-only worker URLs. This is supporting regression, not site acceptance.
 * Enforce/report-only policy: trusted-types tt-worker 'allow-duplicates';
 * require-trusted-types-for 'script'. importURL sets self.__workerImport. */
function parserWorkerSafetyProgram(){
    'use strict';
    let synchronouslyRunning=true,firstBlocked=false,firstResult,checks=0;
    const errors=[],events=[];
    const check=(name,value)=>{checks++;if(!value)errors.push(name);};
    const blocked=fn=>{try{fn();return false;}catch(e){return e instanceof TypeError;}};
    self.onsecuritypolicyviolation=e=>events.push({trusted:e.isTrusted,async:!synchronouslyRunning,
        directive:e.effectiveDirective,disposition:e.disposition,uri:e.documentURI});
    try{firstResult=eval('21+21');}catch(e){firstBlocked=e instanceof TypeError;}
    self.onmessage=e=>{
        synchronouslyRunning=true;
        const {mode,importURL}=e.data,enforce=mode==='enforce',report=mode==='report';
        let jsonCalls=0;const poisoned=[];
        const restore=()=>{for(const [prototype,key,descriptor] of poisoned){
            if(descriptor)Object.defineProperty(prototype,key,descriptor);else delete prototype[key];}};
        try{
            for(const prototype of [Object.prototype,Array.prototype]){
                poisoned.push([prototype,'toJSON',Object.getOwnPropertyDescriptor(prototype,'toJSON')]);
                Object.defineProperty(prototype,'toJSON',{configurable:true,value(){jsonCalls++;throw new Error('author toJSON in native CSP');}});
            }
            check('first author source sees CSP',firstBlocked===enforce&&(enforce||firstResult===42));
            check('Worker safety interfaces',typeof trustedTypes==='object'&&typeof TrustedScript==='function'&&typeof Sanitizer==='undefined');
            const publicEvent=new SecurityPolicyViolationEvent('securitypolicyviolation');check('public event untrusted',!publicEvent.isTrusted);
            const p=trustedTypes.createPolicy('tt-worker',{createScript:v=>v,createScriptURL:v=>v});
            check('policy allowlist',blocked(()=>trustedTypes.createPolicy('not-permitted',{}))===enforce);
            check('untrusted eval',blocked(()=>eval('1+1'))===enforce);
            let lexical=40;check('direct eval preserves lexical scope',eval(p.createScript('lexical+2'))===42);
            check('Function trusted arguments',new Function(p.createScript('x'),p.createScript('return x+1'))(41)===42);
            check('Function untrusted arguments',blocked(()=>new Function('return 1'))===enforce);
            const script=p.createScript('17'),original=TrustedScript.prototype.toString;
            try{TrustedScript.prototype.toString=()=> '999';check('native payload not author toString',eval(script)===17);}
            finally{TrustedScript.prototype.toString=original;}
            const fake=Object.create(TrustedScript.prototype);check('eval does not coerce nonstring forgery',eval(fake)===fake);
            check('forged script brand',!trustedTypes.isScript(fake));
            check('TrustedScript constructor unavailable',blocked(()=>new TrustedScript()));
            let cloneRejected=false;try{structuredClone(script);}catch(x){cloneRejected=x.name==='DataCloneError';}check('native trusted value not cloneable',cloneRejected);
            let setters=0;const saved=[];
            try{
                for(const key of ['required','enforce','policies','names','allowDuplicates','reportOnly']){
                    saved.push([key,Object.getOwnPropertyDescriptor(Object.prototype,key)]);
                    Object.defineProperty(Object.prototype,key,{configurable:true,set(){setters++;}});
                }
                check('prototype setters cannot remove native CSP',blocked(()=>eval('3+4'))===enforce);
                check('native policy records do not invoke author setters',setters===0);
            }finally{for(const [key,d] of saved){if(d)Object.defineProperty(Object.prototype,key,d);else delete Object.prototype[key];}}
            check('string timer sink',blocked(()=>setTimeout('self.__workerTimer=7',0))===enforce);
            check('importScripts untrusted URL',blocked(()=>importScripts(importURL))===enforce);
            importScripts(p.createScriptURL(importURL));check('importScripts trusted URL',self.__workerImport===true);
            setTimeout(()=>{
                check('timer result',enforce?self.__workerTimer===undefined:self.__workerTimer===7);
                check('native CSP event asynchronous and trusted',events.every(x=>x.async&&x.trusted));
                check('violation event policy',mode==='plain'?events.length===0:events.some(x=>x.disposition===(report?'report':'enforce')&&x.directive==='require-trusted-types-for'));
                check('violation event real Worker URI',events.every(x=>x.uri===location.href));
                check('Worker native report ignores author toJSON',jsonCalls===0);restore();
                postMessage({checks,errors,events:events.length});
            },30);
        }catch(error){restore();errors.push(String(error));postMessage({checks,errors});}
        synchronouslyRunning=false;
    };
    synchronouslyRunning=false;
}

globalThis.runParserWorkerSafetyCases=async function(config){
    let count=0;const check=(name,value)=>{count++;if(!value)throw new Error('Worker safety '+name);};
    const p=config.policy||trustedTypes.createPolicy('tt-worker',{createScriptURL:v=>v});
    async function run(url,mode){
        const w=new Worker(p.createScriptURL(url));
        try{
            const result=await new Promise((resolve,reject)=>{
                const timeout=setTimeout(()=>reject(new Error('Worker safety completion timeout')),10000);
                w.onmessage=e=>{clearTimeout(timeout);resolve(e.data);};
                w.onerror=e=>{clearTimeout(timeout);reject(new Error(e.message));};
                w.postMessage({mode,importURL:config.importURL});
            });
            check(mode+' completed',result.checks>=20);check(mode+' assertions',result.errors.length===0);count+=result.checks;
        }finally{w.terminate();}
    }
    let authorCalls=0;const poisoned=[];
    for(const prototype of [Object.prototype,Array.prototype]){
        poisoned.push([prototype,'toJSON',Object.getOwnPropertyDescriptor(prototype,'toJSON')]);
        Object.defineProperty(prototype,'toJSON',{configurable:true,value(){authorCalls++;throw new Error('parent author toJSON in native CSP');}});
    }
    for(const key of ['referrer','lineNumber','columnNumber']){
        poisoned.push([Object.prototype,key,Object.getOwnPropertyDescriptor(Object.prototype,key)]);
        Object.defineProperty(Object.prototype,key,{configurable:true,get(){authorCalls++;throw new Error('parent author getter in native CSP');}});
    }
    try{
        await run(config.plainURL,'plain');await run(config.enforceURL,'enforce');await run(config.reportURL,'report');
        check('parent private Worker report ignores inherited JSON/getters',authorCalls===0);
    }finally{for(const [prototype,key,descriptor] of poisoned){
        if(descriptor)Object.defineProperty(prototype,key,descriptor);else delete prototype[key];}}
    const blob=new Blob(['('+parserWorkerSafetyProgram.toString()+')();'],{type:'text/javascript'}),url=URL.createObjectURL(blob);
    try{
        // This entry predates the meta policy. Blob inherits its URL-entry
        // creation environment, not whichever policy is active at Worker().
        const meta=document.createElement('meta');meta.httpEquiv='Content-Security-Policy';
        meta.content="trusted-types tt-worker 'allow-duplicates'; require-trusted-types-for 'script'";document.head.appendChild(meta);
        await run(url,'plain');
        const inherited=URL.createObjectURL(blob);try{await run(inherited,'enforce');}finally{URL.revokeObjectURL(inherited);}
    }finally{URL.revokeObjectURL(url);}
    console.log('PARSER_WORKER_SAFETY_DONE '+count);return count;
};
if(typeof document==='undefined')parserWorkerSafetyProgram();
