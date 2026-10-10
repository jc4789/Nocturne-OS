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
