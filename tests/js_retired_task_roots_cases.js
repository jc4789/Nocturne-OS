/* 実web_live上の退役task回帰。native所有refの解放自体は別C補助で検査。 */
async function runRetiredTaskRootCases(){
    let count=0;const assert=(value,name)=>{count++;check('retired-task-'+name,!!value);if(!value)throw new Error(name);};
    const sleep=()=>new Promise(resolve=>setTimeout(resolve,30));
    function make(){const frame=document.createElement('iframe');document.body.appendChild(frame);return {frame,child:frame.contentWindow};}
    {
        const {frame,child}=make();let hits=0;
        child.setTimeout(()=>hits++,1000);child.setInterval(()=>hits++,1000);
        child.requestAnimationFrame(()=>hits++);child.requestIdleCallback(()=>hits++,{timeout:1000});
        const channel=new child.MessageChannel();channel.port2.onmessage=()=>hits++;channel.port1.postMessage('pending');
        const document=frame.contentDocument,saved=child.setTimeout;
        frame.remove();await sleep();assert(hits===0,'pending-never-dispatched');
        assert(child.closed,'old-proxy-closed');assert(document.body!==null,'escaped-document-retained');
        let error;try{saved(()=>hits++,1);}catch(e){error=e;}assert(error&&error.name==='TypeError','saved-delegate-generation');
        document.body.textContent='retained';assert(document.body.textContent==='retained','escaped-node-arena-retained');
        document.body.textContent='';
    }
    async function reentrant(kind){
        const {frame,child}=make();let signal=null,hits=0;
        const done=new Promise((resolve,reject)=>{
            const timeout=setTimeout(()=>reject(new Error(kind+' did not dispatch')),3000);
            signal=result=>{clearTimeout(timeout);resolve(result);};
        });
        // Nocturne interrupts an executing retired realm at its next engine
        // poll. Signal entry before removing it; deliberately reach that poll
        // instead of expecting arbitrary JS/URL parsing to finish afterwards.
        const callback=child.Function('retire','signal',`return function(){
            signal();retire();for(;;){}
        };`)(()=>frame.remove(),()=>signal(true));
        // Pending siblings are canceled even while one callback owns local refs.
        child.setTimeout(()=>hits++,1000);child.requestIdleCallback(()=>hits++,{timeout:1000});
        if(kind==='interval')child.setInterval(callback,1);
        else if(kind==='idle')child.requestIdleCallback(callback,{timeout:1});
        else{
            const channel=new child.MessageChannel();channel.port2.onmessage=callback;channel.port1.postMessage('dispatch');
        }
        assert(await done,kind+'-dispatched-before-retirement');
        await sleep();assert(hits===0,kind+'-pending-siblings-canceled');assert(child.closed,kind+'-generation-closed');
        document.body.appendChild(frame);
        assert(frame.contentWindow!==child,kind+'-new-generation');
        let active=0;frame.contentWindow.setTimeout(()=>active++,1);await sleep();
        assert(active===1,kind+'-new-generation-schedules');frame.remove();
    }
    await reentrant('interval');await reentrant('idle');await reentrant('posted');
    // Normal top task registration remains available after child teardown.
    let alive=0;setTimeout(()=>alive++,1);await sleep();assert(alive===1,'top-unaffected');
    return count;
}
