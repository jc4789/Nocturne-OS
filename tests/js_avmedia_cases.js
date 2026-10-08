async function runAvmediaCases() {
    let checks=0;
    const check=(v,label)=>{checks++;if(!v)throw new Error('avmedia '+label);};
    const delay=ms=>new Promise(resolve=>setTimeout(resolve,ms));
    const fixture=name=>new URL(name,'https://avmedia.test/').href;
    function video(name){const v=document.createElement('video');v.preload='none';v.src=fixture(name);return v;}
    function release(node){node.removeAttribute('src');node.load();check(node.readyState===0,'explicit decoder resource release');}
    const v=video('h264.mp4');
    check(v instanceof HTMLVideoElement&&v instanceof HTMLMediaElement,'prototype');
    check(new Audio() instanceof HTMLAudioElement,'Audio constructor');
    check(v.canPlayType('video/mp4; codecs="avc1.42e01e, mp4a.40.2"')==='probably','real codecs');
    check(v.canPlayType('video/webm; codecs=vp8')==='probably','native VP8 codec');
    check(typeof MediaSource==='function'&&typeof MediaSource.isTypeSupported==='function'&&!('requestMediaKeySystemAccess' in navigator),'native MSE, no DRM');
    let throws=false;try{HTMLMediaElement.prototype.canPlayType.call({localName:'video',namespaceURI:'http://www.w3.org/1999/xhtml'},'video/mp4');}catch(e){throws=true;}
    check(throws,'native brand');
    const pending=v.play();v.pause();
    let cancelled=false;try{await pending;}catch(e){cancelled=e.name==='AbortError';}
    check(cancelled&&v.paused,'pending play pause cancellation');
    await delay(80);check(v.paused,'late completion cannot play');
    release(v);
    const loadstartPause=video('h264.mp4');loadstartPause.onloadstart=()=>loadstartPause.pause();
    cancelled=false;try{await loadstartPause.play();}catch(e){cancelled=e.name==='AbortError';}
    check(cancelled&&loadstartPause.paused,'loadstart reentrant pause cancels play');
    loadstartPause.onloadstart=null;release(loadstartPause);

    const reentrant=video('h264.mp4');let metadata=0,loaded=0,wrong=0;
    let done;const replacementReady=new Promise(resolve=>done=resolve);
    reentrant.onloadedmetadata=()=>{metadata++;if(reentrant.currentSrc===fixture('h264.mp4')){reentrant.src=fixture('mjpeg.avi');reentrant.load();}};
    reentrant.onloadeddata=()=>{loaded++;if(reentrant.currentSrc!==fixture('mjpeg.avi'))wrong++;done();};
    const first=reentrant.play();cancelled=false;try{await first;}catch(e){cancelled=e.name==='AbortError';}
    check(cancelled,'reentrant metadata invalidates pending play');
    await replacementReady;
    check(metadata===2&&loaded===1&&wrong===0,'old generation events suppressed');
    check(reentrant.currentSrc===fixture('mjpeg.avi')&&reentrant.paused,'replacement ownership');
    await reentrant.play();check(!reentrant.paused,'actual native video play');
    check(reentrant.videoWidth===96&&reentrant.videoHeight===64,'native video metadata');
    await delay(330);check(!reentrant.ended&&!reentrant.paused,'last native video frame retains duration');
    await delay(170);check(reentrant.ended&&reentrant.paused,'native decode drained');
    check(Math.abs(reentrant.currentTime-reentrant.duration)<0.04,'native video EOF time equals duration');
    release(reentrant);
    const overlong=video('overlong.avi');await overlong.play();
    check(overlong.duration>1000,'overlong duration metadata is actual demux input');
    await delay(550);check(overlong.ended&&overlong.paused&&overlong.currentTime<2,'corrupt duration cannot hold EOF indefinitely');
    release(overlong);

    const visible=document.getElementById('visible');visible.preload='none';visible.src=fixture('h264.mp4');
    await visible.play();check(!visible.paused&&visible.readyState>=2,'visible native frame');
    console.log('AVMEDIA_PAINT_READY');await delay(20);visible.pause();visible.currentTime=0.1;
    check(visible.paused&&visible.currentTime>=0.1,'pause seek keeps paused');
    let eventCancelled=false;visible.onplay=()=>visible.pause();
    try{await visible.play();}catch(e){eventCancelled=e.name==='AbortError';}
    check(eventCancelled&&visible.paused,'play event pause cancels playing');
    visible.onplay=null;

    if(nativeAudioAvailable){
        const audio=document.createElement('audio');audio.preload='none';audio.src=fixture('stereo.flac');
        audio.muted=true; /* Do not inject our tone into audiotest's strict 8-sound recording. */
        await audio.play();check(!audio.paused,'native audio output opened');
        await delay(30);audio.pause();audio.currentTime=0.1;
        check(audio.paused&&audio.currentTime>=0.1,'audio pause seek queue reset');
        audio.volume=0.25;audio.muted=true;check(audio.volume===0.25&&audio.muted,'native gain');
    }
    console.log('AVMEDIA_DONE '+checks);return checks;
}
