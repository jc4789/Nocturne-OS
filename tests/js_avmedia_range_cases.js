/* Run in the real Browser (host.media_range=true), with real compressed media
 * supplied by the caller. No native decoder or CORS success is mocked here. */
async function runAvmediaRangeCases(config) {
    let checks=0;
    const check=(v,label)=>{checks++;if(!v)throw new Error('Range media '+label);};
    function video(url) {
        const v=document.createElement('video');v.preload='none';v.crossOrigin='anonymous';v.src=url;return v;
    }
    function release(v) {v.removeAttribute('src');v.load();check(v.readyState===0,'decoder release');}
    const v=video(config.same);let through=0;v.oncanplaythrough=()=>through++;
    const old=v.play();release(v);
    let cancelled=false;try{await old;}catch(e){cancelled=e.name==='AbortError';}
    check(cancelled&&v.paused,'generation cancels before native IO');
    v.controls=true;v.width=96;v.height=64;document.body.appendChild(v);
    v.src=config.same;
    await v.play();check(v.readyState===2&&!v.paused,'bounded native Range playback');
    check(v.videoWidth>0&&v.videoHeight>0,'real decoded video');
    check(through===0,'one cache window does not promise canplaythrough');
    /* Keep the real Browser pump/PCM/paint path alive before pausing or
     * releasing this 0.4-second compressed fixture. */
    await new Promise(resolve=>setTimeout(resolve,120));
    v.pause();
    const seeked=new Promise(resolve=>v.addEventListener('seeked',resolve,{once:true}));
    v.currentTime=0.1;await seeked;
    check(v.paused&&v.currentTime>=0.1,'real native Range seek preserves pause');
    release(v);
    const cross=video(config.cross);await cross.play();
    check(cross.readyState===2&&cross.videoWidth>0,'anonymous cross-origin CORS decode');release(cross);
    for(const url of [config.denied,config.redirect]) {
        const bad=video(url);let failed=false;try{await bad.play();}catch(e){failed=true;}
        check(failed&&bad.paused&&bad.readyState===0,'denied CORS/redirect cannot decode');release(bad);
    }
    const changed=video(config.same);let stale=0;
    changed.onloadstart=()=>{changed.onloadstart=null;changed.src=config.cross;};
    changed.onloadeddata=()=>{if(changed.currentSrc!==new URL(config.cross,document.baseURI).href)stale++;};
    cancelled=false;try{await changed.play();}catch(e){cancelled=e.name==='AbortError';}
    check(cancelled,'loadstart source replacement cancels old generation');
    await changed.play();check(stale===0&&changed.readyState===2,'replacement native ownership');release(changed);
    console.log('AVMEDIA_RANGE_DONE '+checks);return checks;
}
