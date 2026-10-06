globalThis.runHistoryCases = async function() {
    const assert=(ok,name)=>{check('history-'+name,ok);if(!ok)throw new Error(name);};
    const rejects=(fn,name)=>{try{fn();}catch(e){return e.name===name;}return false;};
    const URLType=URL, apply=Reflect.apply;
    const locationKeys=['protocol','host','hostname','port','pathname','search','hash','origin'];
    const urlDescriptors=Object.create(null);
    for(const key of [...locationKeys,'href','username','password'])
        urlDescriptors[key]=Object.getOwnPropertyDescriptor(URLType.prototype,key);
    const locationMatches=href=>{
        const url=new URLType(href);
        return locationKeys.every(key=>location[key]===apply(urlDescriptors[key].get,url,[]));
    };
    assert(history instanceof History&&history.state===null&&history.length===1,'initial');
    assert(rejects(()=>new History(),'TypeError'),'illegal-constructor');
    assert(rejects(()=>History.prototype.pushState.call({},null,''),'TypeError'),'brand');
    assert(rejects(()=>history.pushState(null),'TypeError'),'arity');
    assert(rejects(()=>history.go(1n),'TypeError'),'go-bigint-rejected');
    const start=location.href, data={a:1,map:new Map(),date:new Date(123)};data.self=data;data.map.set(data,data);
    history.scrollRestoration='manual';
    history.pushState(data,'','?page=1#one');data.a=2;
    assert(history.length===2&&location.search==='?page=1'&&location.hash==='#one','push-url');
    assert(locationMatches(location.href),'location-fields-after-push');
    const encoder=TextEncoder.prototype.encode;let encodes=0,stable=true;
    const expected=locationKeys.map(key=>location[key]);
    try {
        TextEncoder.prototype.encode=function(...args){encodes++;return apply(encoder,this,args);};
        for(let i=0;i<128;i++)for(let j=0;j<locationKeys.length;j++)
            if(location[locationKeys[j]]!==expected[j])stable=false;
        assert(stable&&encodes===0,'location-unchanged-no-reparse');
    } finally {TextEncoder.prototype.encode=encoder;}
    let state=history.state;
    assert(state.a===1&&state!==data&&state.self===state&&state.map.get(state)===state&&state.date.getTime()===123,'clone');
    assert(history.state===state&&history.scrollRestoration==='manual','state-identity-mode');
    state.a=3;assert(history.state.a===3,'live-state-identity');
    assert(rejects(()=>history.pushState(null,'','https://evil.invalid/'),'SecurityError'),'cross-origin');
    assert(rejects(()=>history.pushState(()=>{},''),'DataCloneError'),'function-rejected');
    const thrown={};assert((()=>{try{history.pushState({get x(){throw thrown;}},'');}catch(e){return e===thrown;}return false;})(),'getter-exception');
    assert(history.length===2,'failed-operations-atomic');
    history.replaceState({second:true},'','?page=2#two');
    assert(history.length===2&&history.state.second&&location.hash==='#two','replace');
    assert(locationMatches(location.href),'location-fields-after-replace');
    const events=[];
    addEventListener('hashchange',e=>events.push(['hash',e.oldURL,e.newURL]));
    const nextPop=()=>new Promise(resolve=>addEventListener('popstate',resolve,{once:true}));
    history.go=()=>{throw new Error('overridden go must not affect back/forward');};
    let pop=nextPop();history.back();let event=await pop;
    assert(event.state===null&&history.state===null&&location.href===start,'back');
    assert(locationMatches(start),'location-fields-after-back');
    // Hashchange is delivered after popstate in the same traversal task.
    await new Promise(resolve=>setTimeout(resolve,0));
    assert(events.length===1&&events[0][1].endsWith('#two')&&events[0][2]===start,'hash-event');
    pop=nextPop();history.forward();event=await pop;
    assert(event.state.second&&event.state===history.state&&location.hash==='#two','forward');
    assert(locationMatches(location.href),'location-fields-after-forward');
    delete history.go;
    pop=nextPop();history.back();await pop;
    history.pushState({third:true},'','?page=3');
    assert(history.length===2&&history.state.third,'forward-discard');
    history.scrollRestoration='auto';assert(history.scrollRestoration==='auto','scroll-mode');
    const beforePoison=location.href;
    assert(locationMatches(beforePoison),'location-before-prototype-isolation');
    let poisoned=0;
    try {
        for(const key of Object.keys(urlDescriptors))Object.defineProperty(URLType.prototype,key,{
            configurable:true,get(){poisoned++;throw new Error('poisoned URL getter');},
            set(){poisoned++;throw new Error('poisoned URL setter');}
        });
        globalThis.URL=function(){poisoned++;throw new Error('replaced URL constructor');};
        assert(locationMatches(beforePoison),'location-warm-prototype-isolation');
        history.replaceState({third:true},'','?cache=4#cold');
        assert(locationMatches(location.href)&&location.search==='?cache=4'&&location.hash==='#cold','location-cold-prototype-isolation');
        assert(rejects(()=>history.pushState(null,'','https://evil.invalid/'),'SecurityError'),'history-origin-prototype-isolation');
        const sentinel={};
        assert((()=>{try{location.hash={toString(){throw sentinel;}};}catch(e){return e===sentinel;}return false;})(),'location-setter-conversion-exception');
        const unchanged=location.href;
        location.hash='cache-setter';
        // The native test embedder records navigation; it commits no document
        // replacement while this task is running. Do not cache the attempted URL.
        assert(locationMatches(unchanged)&&location.hash==='#cold','location-setter-not-speculative');
        assert(poisoned===0,'location-setter-prototype-isolation');
        let conversions=0;
        location.hash={toString(){conversions++;history.replaceState({third:true},'','?cache=5#inside');return 'outside';}};
        assert(conversions===1&&location.search==='?cache=5'&&location.hash==='#inside','location-setter-reentry-current-url');
        assert(rejects(()=>{location.hash=Symbol('x');},'TypeError'),'location-setter-symbol-rejected');
    } finally {
        globalThis.URL=URLType;
        for(const key of Object.keys(urlDescriptors))Object.defineProperty(URLType.prototype,key,urlDescriptors[key]);
        history.replaceState({third:true},'',beforePoison);
    }
    assert(locationMatches(beforePoison),'location-cache-restored');
};
