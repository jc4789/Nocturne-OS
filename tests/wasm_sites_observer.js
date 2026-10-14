/* 実サイト用の読み取り専用観測。DOM・style・作者関数・媒体状態を変更しない。
 * 5秒は手動操作の前後を区別する採取間隔であり、製品の実行制限ではない。 */
(() => {
    const observationIntervalMs=5000;
    const media=new WeakSet();
    function reportMedia(video,event){
        console.log('WASM_SITE_MEDIA '+JSON.stringify({event,time:video.currentTime,duration:video.duration,
            paused:video.paused,ended:video.ended,ready:video.readyState,network:video.networkState,
            width:video.videoWidth,height:video.videoHeight,error:video.error?.code||0}));
    }
    function inspect(){
        console.log('WASM_SITE_VIEW '+JSON.stringify({url:location.href,title:document.title,
            ready:document.readyState,width:innerWidth,height:innerHeight,x:scrollX,y:scrollY,
            module:typeof WebAssembly.Module,streaming:typeof WebAssembly.instantiateStreaming}));
        for(const video of document.querySelectorAll('video')){
            if(!media.has(video)){
                media.add(video);
                for(const event of ['play','pause','seeking','seeked','ended','error'])
                    video.addEventListener(event,()=>reportMedia(video,event));
            }
            reportMedia(video,'sample');
        }
    }
    for(const type of ['mouseover','click'])document.addEventListener(type,event=>{
        if(!event.isTrusted)return;
        const node=event.target;if(!node||node.nodeType!==1)return;
        const rect=node.getBoundingClientRect(),hit=document.elementFromPoint(event.clientX,event.clientY);
        console.log('WASM_SITE_INPUT '+JSON.stringify({type,tag:node.tagName,id:node.id,
            x:event.clientX,y:event.clientY,hit:hit?.tagName||'',same:hit===node,
            rect:[rect.x,rect.y,rect.width,rect.height]}));
    },true);
    inspect();setInterval(inspect,observationIntervalMs);
})();
