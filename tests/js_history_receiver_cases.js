async function runHistoryReceiverCases(){
    let count=0;
    const assert=(value,name)=>{count++;check('history-receiver-'+name,!!value);if(!value)throw new Error(name);};
    const throws=(call,name,label)=>{let error;try{call();}catch(e){error=e;}assert(error&&error.name===name,label);};
    const frame=document.createElement('iframe');document.body.appendChild(frame);
    const topHistory=history,start=location.href,childWindow=frame.contentWindow,childHistory=childWindow.history;
    const replace=childHistory.replaceState,push=childHistory.pushState;
    const stateGetter=Object.getOwnPropertyDescriptor(childWindow.History.prototype,'state').get;
    assert(childHistory!==topHistory,'independent-objects');assert(childWindow.History!==History,'independent-realms');
    assert(replace!==History.prototype.replaceState,'independent-functions');
    assert(childHistory instanceof childWindow.History,'child-brand-prototype');
    const value={array:new childWindow.Array(1,2),map:new childWindow.Map([['key',{ok:true}]])};
    // Actual YouTube pattern: a clean iframe method bound to top.history.
    replace.bind(topHistory)(value,'','?historyReceiver=top#borrowed');
    assert(location.search==='?historyReceiver=top'&&location.hash==='#borrowed','receiver-url');
    assert(childWindow.document.URL==='about:blank','donor-url-unchanged');
    let state=stateGetter.call(topHistory);
    assert(state===topHistory.state,'receiver-state-cache');assert(state!==value&&state.array!==value.array,'receiver-state-clone');
    assert(state.array instanceof Array&&!(state.array instanceof childWindow.Array),'receiver-array-realm');
    assert(state.map instanceof Map&&state.map.get('key').ok,'receiver-map-realm');
    assert(topHistory.length===1&&childHistory.length===1,'replace-lengths');
    const lengthGetter=Object.getOwnPropertyDescriptor(childWindow.History.prototype,'length').get;
    assert(lengthGetter.call(topHistory)===topHistory.length,'borrowed-length-getter');
    const mode=Object.getOwnPropertyDescriptor(childWindow.History.prototype,'scrollRestoration');
    mode.set.call(topHistory,'manual');assert(topHistory.scrollRestoration==='manual','borrowed-mode-setter');
    assert(mode.get.call(topHistory)==='manual','borrowed-mode-getter');
    const oldDescriptor=Object.getOwnPropertyDescriptor(topHistory,'replaceState');
    try{
        Object.defineProperty(topHistory,'replaceState',{configurable:true,value(){throw new Error('Author method was redispatched');}});
        replace.bind(topHistory)({privateOwner:true},'','?historyReceiver=private');
        assert(topHistory.state.privateOwner&&location.search==='?historyReceiver=private','private-owner-hook');
    }finally{if(oldDescriptor)Object.defineProperty(topHistory,'replaceState',oldDescriptor);else delete topHistory.replaceState;}
    throws(()=>replace.call({},null,''),'TypeError','fake-object');
    throws(()=>replace.call(Object.create(History.prototype),null,''),'TypeError','fake-top-prototype');
    throws(()=>replace.call(Object.create(childWindow.History.prototype),null,''),'TypeError','fake-child-prototype');
    throws(()=>replace.call(new Proxy(topHistory,{}),null,''),'TypeError','proxy-receiver');
    throws(()=>replace.call(topHistory,null),'TypeError','borrowed-arity');
    throws(()=>replace.call(topHistory,null,'','http://other.test/'),'SecurityError','borrowed-origin');
    const before=topHistory.state;
    throws(()=>History.prototype.replaceState.call(childHistory,null,''),'TypeError','child-mutation-explicitly-unsupported');
    assert(topHistory.state===before,'child-not-top-alias');
    const sentinel={get message(){throw new Error('Thrown object was inspected');}};
    let actual;try{replace.call(topHistory,{get data(){throw sentinel;}},'');}catch(e){actual=e;}
    assert(actual===sentinel,'clone-getter-exception-identity');
    push.bind(topHistory)({pushed:true},'','?historyReceiver=push');
    assert(topHistory.length===2&&topHistory.state.pushed&&location.search==='?historyReceiver=push','borrowed-push');
    await new Promise((resolve,reject)=>{
        const timeout=setTimeout(()=>reject(new Error('History frame navigation load missing')),2000);
        frame.onload=()=>{clearTimeout(timeout);frame.onload=null;resolve();};frame.srcdoc='<body>new history owner';
    });
    const nextHistory=frame.contentWindow.history;
    assert(nextHistory!==childHistory,'navigation-new-receiver');
    throws(()=>stateGetter.call(childHistory),'SecurityError','retired-getter');
    throws(()=>History.prototype.replaceState.call(childHistory,null,''),'SecurityError','retired-method-receiver');
    // A generic interface function's donor realm is not its receiver owner.
    replace.call(topHistory,{savedDonor:true},'','?historyReceiver=saved');
    assert(topHistory.state.savedDonor&&location.search==='?historyReceiver=saved','saved-donor-active-receiver');
    frame.remove();
    throws(()=>History.prototype.replaceState.call(nextHistory,null,''),'SecurityError','detached-receiver');
    document.body.appendChild(frame);assert(frame.contentWindow.history!==nextHistory,'reinsert-new-generation');
    throws(()=>History.prototype.replaceState.call(nextHistory,null,''),'SecurityError','stale-receiver-not-revived');frame.remove();
    topHistory.scrollRestoration='auto';topHistory.replaceState(null,'',start);
    return count;
}
