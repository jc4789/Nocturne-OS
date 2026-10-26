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
    check(typeof MediaSource==='function'&&typeof MediaSource.isTypeSupported==='function'&&typeof navigator.requestMediaKeySystemAccess==='function'&&typeof MediaKeys==='function','native MSE and ClearKey API');
    let unsupported=false;
    try{await navigator.requestMediaKeySystemAccess('unsupported.test.keysystem',[{initDataTypes:['cenc'],videoCapabilities:[{contentType:'video/mp4; codecs="avc1.42e01e"'}]}]);}
    catch(e){unsupported=e.name==='NotSupportedError';}
    check(unsupported,'unsupported DRM is rejected rather than advertised');
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

    /* WPT event_volumechange, volumechange-ratechange-queue-task, volume_nonfinite. */
    for(const bad of [NaN,Infinity,-Infinity]){let type=false;try{new Audio().volume=bad;}catch(e){type=e instanceof TypeError;}check(type,'non-finite volume TypeError');}
    const quiet=new Audio();let changes=0;quiet.onvolumechange=()=>changes++;
    quiet.volume=quiet.volume;quiet.muted=quiet.muted;
    quiet.muted=true;check(changes===0,'volumechange is not dispatched synchronously');
    await Promise.resolve();check(changes===0,'volumechange is a task, not a microtask');
    quiet.volume=0.5;quiet.muted=false;quiet.volume=1;
    await delay(30);check(changes===4,'one volumechange per change, none for the same value');
    const echo=new Audio();let echoes=0;echo.onvolumechange=()=>{echoes++;echo.volume=echo.volume;echo.muted=echo.muted;};
    echo.volume=0.25;await delay(30);check(echoes===1,'listener writing the same volume does not refire');

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

    /* Real fragmented VP9 input through SourceBuffer -> native helper. The
       initialization segment alone must expose metadata without waiting for
       frames, while canplay must not precede actual future coded-frame data. */
    const bytes=await (await fetch(fixture('mp4-vp9.mp4'))).arrayBuffer();
    const view=new DataView(bytes);let mediaAt=0;
    while(mediaAt+8<=bytes.byteLength){
        const size=view.getUint32(mediaAt),type=view.getUint32(mediaAt+4);
        if(type===0x6d6f6f66)break;
        check(size>=8&&size<=bytes.byteLength-mediaAt,'valid native MSE fixture box');
        mediaAt+=size;
    }
    check(mediaAt>0&&mediaAt+8<bytes.byteLength,'fixture separates initialization and media');
    const ms=new MediaSource(),m=document.createElement('video'),url=URL.createObjectURL(ms);
    const wait=(target,name)=>new Promise((resolve,reject)=>{
        const fail=setTimeout(()=>{target.removeEventListener(name,ok);reject(new Error('avmedia timeout '+name+' ready='+m.readyState+' paused='+m.paused+' time='+m.currentTime+' duration='+ms.duration));},3000);
        function ok(){clearTimeout(fail);resolve();}
        target.addEventListener(name,ok,{once:true});
    });
    let metadataCount=0,dataCount=0,canplayCount=0;
    m.addEventListener('loadedmetadata',()=>metadataCount++);
    m.addEventListener('loadeddata',()=>dataCount++);
    m.addEventListener('canplay',()=>canplayCount++);
    const opened=wait(ms,'sourceopen');m.src=url;await opened;
    const sb=ms.addSourceBuffer('video/mp4; codecs="vp09.00.10.08"');
    const metadataReady=wait(m,'loadedmetadata'),initDone=wait(sb,'updateend');
    sb.appendBuffer(bytes.slice(0,mediaAt));await initDone;await metadataReady;
    check(m.readyState===1&&m.paused&&m.currentTime===0,'paused MSE exposes real initialization metadata');
    check(metadataCount===1&&dataCount===0&&canplayCount===0,'metadata does not invent data or canplay');
    const dataReady=wait(m,'loadeddata'),canplayReady=wait(m,'canplay'),appendDone=wait(sb,'updateend');
    sb.appendBuffer(bytes.slice(mediaAt));await appendDone;await dataReady;await canplayReady;
    check(m.readyState>=3&&m.paused&&m.currentTime===0,'paused buffered MSE exposes future data without starting clock');
    check(metadataCount===1&&dataCount===1&&canplayCount===1,'ready events occur once at their actual transitions');
    const through=wait(m,'canplaythrough');ms.endOfStream();await through;
    check(m.readyState===4,'complete buffered EOS has enough data');
    await m.play();await delay(100);
    check(!m.paused&&m.currentTime>0&&m.getVideoPlaybackQuality().totalVideoFrames>0,'preloaded MSE progresses through native playback');
    m.pause();release(m);URL.revokeObjectURL(url);

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
