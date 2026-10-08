async function runAvmediaCases() {
    let checks=0;
    const check=(v,label)=>{checks++;if(!v)throw new Error('avmedia webm '+label);};
    const delay=ms=>new Promise(resolve=>setTimeout(resolve,ms));
    const fixture=name=>new URL(name,'https://avmedia.test/').href;
    const release=node=>{node.removeAttribute('src');node.load();check(node.readyState===0,'decoder released');};
    const video=document.getElementById('visible');video.preload='none';
    check(video.canPlayType('video/webm; codecs="vp9, opus"')==='probably','VP9 Opus declaration');
    check(video.canPlayType('audio/ogg; codecs=vorbis')==='probably','Vorbis declaration');
    check(video.canPlayType('video/webm; codecs=vp09.02.10.10')==='','no high-bit-depth declaration');
    check(video.canPlayType('video/webm; codecs=vp8')==='probably','VP8 declaration');
    check(video.canPlayType('video/webm; codecs=av01.0.04M.08')==='','AV1 remains unsupported');
    check(typeof MediaSource==='function'&&typeof MediaSource.isTypeSupported==='function'&&!('requestMediaKeySystemAccess' in navigator),'native MSE, no DRM');

    video.src=fixture('vp9.webm');await video.play();
    check(video.videoWidth===96&&video.videoHeight===64&&!video.paused,'VP9 native metadata/frame');
    console.log('AVMEDIA_PAINT_READY');await delay(20);video.pause();
    await delay(90);check(video.paused,'VP9 pause remains paused');
    video.currentTime=0.1;check(video.paused&&video.currentTime>=0.1,'VP9 native seek');
    await video.play();await delay(450);
    check(video.ended&&video.paused,'VP9 EOF');
    check(Math.abs(video.currentTime-video.duration)<0.05,'VP9 last frame duration');release(video);

    if(nativeAudioAvailable){
        for(const name of ['stereo.opus','stereo-silk.opus','stereo.ogg','opus.webm','vorbis.webm']){
            const audio=document.createElement('audio');audio.preload='none';audio.muted=true;audio.src=fixture(name);
            await audio.play();check(!audio.paused&&audio.readyState>=2,'native audio '+name);
            await delay(25);audio.pause();audio.currentTime=0.1;
            check(audio.paused&&audio.currentTime>=0.1,'audio seek/flush '+name);
            await audio.play();await delay(300);check(audio.ended&&audio.paused,'audio EOF '+name);release(audio);
        }
        for(const name of ['vp9-opus.webm','vp9-vorbis.webm']){
            video.muted=true;video.src=fixture(name);await video.play();
            check(!video.paused&&video.videoWidth===96,'combined native decode '+name);
            await delay(500);check(video.ended&&video.paused,'combined EOF '+name);release(video);
        }
    }
    for(const name of ['vp9-high10.webm','unsupported-av1.webm']){
        video.src=fixture(name);let rejected=false;try{await video.play();}catch(e){rejected=true;}
        check(rejected&&video.paused,'actual unsupported fixture '+name);release(video);
    }
    /* Preserve a visible decoded frame for the harness's final paint check. */
    video.src=fixture('vp9.webm');await video.play();video.pause();
    check(video.readyState>=2,'final native display ownership');
    console.log('AVMEDIA_DONE '+checks);return checks;
}
