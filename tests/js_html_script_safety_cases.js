/* Run in a fresh active document delivered with native CSP
 * require-trusted-types-for 'script'. This changes its default policy and must
 * not be mixed into a real-site document or the permissive baseline suite. */
globalThis.runHTMLScriptSafetyCases=function(){
    let count=0;
    function check(name,value){count++;if(!value)throw new Error('HTML script safety '+name);}
    function denied(name,fn){let threw=false;try{fn();}catch(error){threw=error instanceof TypeError;}check(name,threw);}
    globalThis.__htmlScriptSafety=0;
    const policy=trustedTypes.createPolicy('script-snapshot-test',{createScript:value=>value});
    const make=()=>document.createElement('script');
    const insert=script=>document.body.appendChild(script);
    const approved=make();approved.text=policy.createScript('globalThis.__htmlScriptSafety=1;');insert(approved);
    check('approved text snapshot executes',__htmlScriptSafety===1);
    const approvedContent=make();approvedContent.textContent=policy.createScript('globalThis.__htmlScriptSafety=2;');insert(approvedContent);
    check('approved textContent snapshot executes',__htmlScriptSafety===2);
    denied('raw text immediate denial',()=>{make().text='globalThis.__htmlScriptSafety=-1;';});
    denied('raw textContent immediate denial',()=>{make().textContent='globalThis.__htmlScriptSafety=-1;';});
    const rawAppend=make();rawAppend.appendChild(document.createTextNode('globalThis.__htmlScriptSafety=-2;'));insert(rawAppend);
    check('raw child append blocked at prepare',__htmlScriptSafety===2);
    const mutated=make();mutated.text=policy.createScript('globalThis.__htmlScriptSafety=3;');mutated.firstChild.data='globalThis.__htmlScriptSafety=-3;';insert(mutated);
    check('text data mutation invalidates snapshot',__htmlScriptSafety===2);
    const appended=make();appended.text=policy.createScript('globalThis.__htmlScriptSafety=4;');appended.appendChild(document.createTextNode('globalThis.__htmlScriptSafety=-4;'));insert(appended);
    check('extra child invalidates snapshot',__htmlScriptSafety===2);
    trustedTypes.createPolicy('default',{createScript:input=>'globalThis.__htmlScriptSafety=100;'});
    const transformed=make(),raw='globalThis.__htmlScriptSafety=-5;';transformed.appendChild(document.createTextNode(raw));insert(transformed);
    check('default script policy execution source',__htmlScriptSafety===100);
    check('default conversion preserves observable DOM',transformed.textContent===raw);
    const afterStart=make();afterStart.text=policy.createScript('globalThis.__htmlScriptSafety=101;');insert(afterStart);afterStart.firstChild.data='globalThis.__htmlScriptSafety=-6;';
    check('started script is not executed again',__htmlScriptSafety===101);
    delete globalThis.__htmlScriptSafety;
    return count;
};

/* A separate fresh policy realm exercises reentry while a script is still
 * native preparation state, before it has entered the runnable script queue. */
globalThis.runHTMLScriptGenerationCases=async function(){
    let count=0,converted=false;
    const check=(name,value)=>{count++;if(!value)throw new Error('HTML script generation '+name);};
    const original=document,retained=document.body;
    globalThis.__streamStaleExecuted=false;
    globalThis.__streamSiblingExecuted=false;
    const policy=trustedTypes.createPolicy('script-snapshot-test',{createHTML:value=>value,createScript:value=>value});
    trustedTypes.createPolicy('default',{createScript:input=>{
        converted=true;
        document.open();document.write(policy.createHTML('<!doctype html><p id=policy-stream>new stream</p>'));document.close();
        return 'globalThis.__streamStaleExecuted=true;';
    }});
    if(document.readyState==='loading')await new Promise(resolve=>document.addEventListener('DOMContentLoaded',resolve,{once:true}));
    const stale=document.createElement('script');stale.appendChild(document.createTextNode('globalThis.__streamStaleExecuted=true;'));
    const sibling=document.createElement('script');sibling.text=policy.createScript('globalThis.__streamSiblingExecuted=true;');
    const fragment=document.createDocumentFragment();fragment.appendChild(stale);fragment.appendChild(sibling);
    document.body.appendChild(fragment);
    check('default policy replaced stream during prepare',converted&&!!document.getElementById('policy-stream'));
    check('Document and retained node keep identity',document===original&&retained.ownerDocument===original&&!retained.isConnected);
    check('unpublished old script does not run inline',!__streamStaleExecuted);
    check('old detached sibling is not prepared in new stream',!__streamSiblingExecuted&&!sibling.isConnected);
    await new Promise(resolve=>setTimeout(resolve,0));
    check('unpublished old script is not later runnable',!__streamStaleExecuted);
    check('old detached sibling is not later runnable',!__streamSiblingExecuted);
    delete globalThis.__streamStaleExecuted;
    delete globalThis.__streamSiblingExecuted;
    return count;
};

/* The native lifecycle calls must not publish an old event or its ending
 * timestamp after an event/focus callback has replaced the stream. */
globalThis.runHTMLStreamEventCases=async function(){
    let count=0,loads=0,focusReplaced=false;
    const check=(name,value)=>{count++;if(!value)throw new Error('HTML stream events '+name);};
    const policy=trustedTypes.createPolicy('script-snapshot-test',{createHTML:value=>value});
    check('fresh document awaits initial DOMContentLoaded',document.readyState==='loading');
    await new Promise(resolve=>document.addEventListener('DOMContentLoaded',()=>{
        document.open();document.write(policy.createHTML('<!doctype html><input id=stream-focus autofocus>'));
        resolve();
    },{once:true}));
    await new Promise(resolve=>setTimeout(resolve,0));
    check('old DOMContentLoaded end not recorded in replacement',performance.timing.domContentLoadedEventEnd===0&&document.readyState==='loading');
    const input=document.getElementById('stream-focus');
    const focused=new Promise(resolve=>input.addEventListener('focus',()=>{
        focusReplaced=true;
        document.open();document.write(policy.createHTML('<!doctype html><p id=focus-stream>new focus stream</p>'));
        addEventListener('load',()=>{loads++;});
        resolve();
    },{once:true}));
    document.close();
    await focused;
    await new Promise(resolve=>setTimeout(resolve,0));
    check('autofocus replaced stream',focusReplaced&&!!document.getElementById('focus-stream')&&!input.isConnected);
    check('old load not sent to new open stream',loads===0&&document.readyState==='loading');
    check('old load timestamps not recorded',performance.timing.loadEventStart===0&&performance.timing.loadEventEnd===0);
    const loaded=new Promise(resolve=>addEventListener('load',resolve,{once:true}));
    document.close();await loaded;
    check('replacement has exactly its own load',loads===1&&document.readyState==='complete');
    return count;
};
