/* Real Browser worker path. Same 0.4s compressed fixture as Range cases.
 * This is not a mock of metadata, native IPC, seek ACK or loop playback. */
async function runAvmediaWorkerCases(config) {
    let checks=0;
    const check=(value,label)=>{checks++;if(!value)throw new Error('Media worker '+label);};
    const delay=ms=>new Promise(resolve=>setTimeout(resolve,ms));
    async function until(predicate,label) {
        const end=performance.now()+5000;
        while(!predicate()) {if(performance.now()>=end)throw new Error('Media worker timeout '+label);await delay(10);}
    }
    const v=document.createElement('video');v.preload='none';v.crossOrigin='anonymous';
    v.controls=true;v.width=96;v.height=64;v.src=config.same;v.loop=true;
    document.body.appendChild(v);
    let starts=0,pauseQueued=false,replacementQueued=false;
    v.onplaying=()=>starts++;
    await v.play();check(v.videoWidth>0&&v.readyState===2,'real first decoded frame');
    await until(()=>starts>=2,'one complete loop ACK and restart');
    check(!v.paused&&!v.ended,'loop restarts rather than stops at EOF');
    v.ontimeupdate=()=>{
        if(v.ended&&!pauseQueued){pauseQueued=true;setTimeout(()=>v.pause(),0);}
    };
    await until(()=>pauseQueued&&v.paused,'pause cancels queued loop restart');
    const pausedStarts=starts;await delay(100);
    check(v.paused&&starts===pausedStarts,'no delayed play after pause intent');
    v.ontimeupdate=null;await v.play();
    check(!v.paused,'explicit replay after pause works');
    v.ontimeupdate=()=>{
        if(v.ended&&!replacementQueued){replacementQueued=true;setTimeout(()=>{v.loop=false;v.src=config.cross;},0);}
    };
    await until(()=>replacementQueued&&v.src===new URL(config.cross,document.baseURI).href&&v.paused,'source cancels loop generation');
    const replacedStarts=starts;await delay(100);
    check(v.paused&&starts===replacedStarts&&v.readyState===0,'old loop cannot start replacement');
    v.ontimeupdate=null;await v.play();
    check(v.readyState===2&&v.videoWidth>0&&!v.paused,'replacement has real metadata and frame');
    v.removeAttribute('src');v.load();check(v.readyState===0&&v.paused,'worker release');v.remove();
    console.log('AVMEDIA_WORKER_DONE '+checks);return checks;
}
