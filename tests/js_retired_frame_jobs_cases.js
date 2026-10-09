/* 補助web_live回帰用。実サイト受入の代用ではない。 */
async function runRetiredFrameJobCases(){
    const results=[];
    async function check(name,queue,expected){
        const frame=document.createElement('iframe');document.body.appendChild(frame);
        const child=frame.contentWindow;let hits=0;
        const callback=child.Function('signal','return function(){signal()}')(()=>hits++);
        queue(child,callback,()=>hits++);
        frame.remove();
        await new Promise(resolve=>setTimeout(resolve,20));
        results.push({name,hits,expected});
        if(hits!==expected)throw new Error(name+': '+hits+' != '+expected);
    }
    await check('親Promiseから退役子callback',(child,callback)=>Promise.resolve().then(callback),0);
    await check('親microtaskから退役子callback',(child,callback)=>queueMicrotask(callback),0);
    await check('退役子Promiseから生存親callback',(child,callback,signal)=>child.Promise.resolve().then(signal),1);
    await check('親thenableから退役子then',(child,callback,signal)=>{
        const then=child.Function('signal','return function(resolve){signal();resolve(1)}')(signal);
        Promise.resolve({then});
    },0);
    await check('同realm子Promise',(child,callback,signal)=>{
        child.Function('signal','Promise.resolve().then(()=>signal())')(signal);
    },0);
    await check('後から解決する親Promise',(child,callback)=>{
        let resolve;new Promise(r=>resolve=r).then(callback);setTimeout(()=>resolve(1),5);
    },0);
    await check('空handler値の生存親への中継',(child,callback,signal)=>{
        child.Promise.resolve(42).then().then(value=>{if(value!==42)throw new Error('値不一致');signal()});
    },1);
    await check('空handler拒否の生存親への中継',(child,callback,signal)=>{
        child.Promise.reject(42).then().catch(reason=>{if(reason!==42)throw new Error('拒否理由不一致');signal()});
    },1);
    await check('all値の生存親への中継',(child,callback,signal)=>{
        child.Promise.all([child.Promise.resolve(42)]).then(values=>{if(values[0]!==42)throw new Error('all値不一致');signal()});
    },1);
    await check('all拒否の生存親への中継',(child,callback,signal)=>{
        child.Promise.all([child.Promise.reject(42)]).catch(reason=>{if(reason!==42)throw new Error('all拒否不一致');signal()});
    },1);
    await check('allSettledの生存親への中継',(child,callback,signal)=>{
        child.Promise.allSettled([child.Promise.resolve(42),child.Promise.reject(43)]).then(values=>{
            if(values[0].value!==42||values[1].reason!==43)throw new Error('allSettled不一致');signal();
        });
    },1);
    await check('raceの生存親への中継',(child,callback,signal)=>{
        child.Promise.race([child.Promise.resolve(42)]).then(value=>{if(value!==42)throw new Error('race値不一致');signal()});
    },1);
    await check('finally生存親と値の中継',(child,callback,signal)=>{
        child.Promise.resolve(42).finally(()=>{}).then(value=>{if(value!==42)throw new Error('finally値不一致');signal()});
    },1);
    await check('finally退役子callback',(child,callback)=>Promise.resolve(42).finally(callback),0);
    await check('async退役子の再開停止',(child,callback,signal)=>{
        let resolve;const pending=new Promise(r=>resolve=r);
        child.Function('pending','signal','return(async()=>{await pending;signal()})()')(pending,signal);
        setTimeout(()=>resolve(42),5);
    },0);
    await check('親Promiseによる子Promise採用',(child,callback,signal)=>{
        Promise.resolve(child.Promise.resolve(42)).then(value=>{if(value!==42)throw new Error('採用値不一致');signal()});
    },1);
    await check('async生存親の再開',(child,callback,signal)=>{
        (async()=>{const value=await child.Promise.resolve(42);if(value!==42)throw new Error('await値不一致');signal()})();
    },1);
    await check('asyncGenerator退役子の再開停止',(child,callback,signal)=>{
        let resolve;const pending=new Promise(r=>resolve=r);
        child.Function('pending','signal','return(async function*(){await pending;signal();yield 1})().next()')(pending,signal);
        setTimeout(()=>resolve(42),5);
    },0);
    console.log('退役フレームjob回帰完了 '+JSON.stringify(results));
    return results.length;
}
