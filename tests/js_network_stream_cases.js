/* These gates release body bytes only after the product publishes headers.
 * Full-response buffering cannot pass by waiting for a short artificial drip. */
async function runNetworkStreamCases(){
    let serial=0;
    const url=key=>'/api/stream?key='+key;
    const release=async(key,phase)=>{const r=await fetch('/api/stream-release?key='+key+'&phase='+phase);await r.text();};
    for(const compressed of [false,true]){
        const key='fetch-'+(++serial),response=await fetch(url(key)+(compressed?'&gzip=1':''));
        check('stream-headers-before-body-'+compressed,response.status===200);
        const reader=response.body.getReader();
        await release(key,1);
        const first=await reader.read();
        check('stream-first-before-end-'+compressed,!first.done&&new TextDecoder().decode(first.value)==='first');
        await release(key,2);
        let text='';for(;;){const part=await reader.read();if(part.done)break;text+=new TextDecoder().decode(part.value);}
        check('stream-final-'+compressed,text==='-last');
    }
    {
        const key='cancel-'+(++serial),response=await fetch(url(key));
        const reader=response.body.getReader();await release(key,1);await reader.read();await reader.cancel();
        check('stream-reader-cancel',(await reader.read()).done);
        await release(key,2);
    }
    {
        const controllers=[],responses=[];
        for(let i=0;i<5;i++){const c=new AbortController();controllers.push(c);responses.push(await fetch(url('unread-'+i),{signal:c.signal}));}
        check('stream-unread-fifth-progress',responses.length===5&&responses.every(r=>r.status===200));
        controllers.forEach(c=>c.abort());
        for(let i=0;i<5;i++){await release('unread-'+i,1);await release('unread-'+i,2);}
    }
    {
        const key='xhr-'+(++serial),x=new XMLHttpRequest(),states=[];let upload=false,partial=false;
        const done=new Promise((resolve,reject)=>{x.onload=resolve;x.onerror=()=>reject(new Error('XHR stream failed'));});
        x.upload.onload=()=>{upload=true;};
        x.onreadystatechange=()=>{
            states.push(x.readyState);
            if(x.readyState===2){check('xhr-upload-before-response-body',upload);release(key,1).catch(console.log);}
            if(x.readyState===3&&!partial){partial=true;check('xhr-first-before-end',x.responseText==='first');release(key,2).catch(console.log);}
        };
        x.open('POST',url(key));x.send('upload');await done;
        check('xhr-header-loading-done-order',states.indexOf(2)<states.indexOf(3)&&states.indexOf(3)<states.indexOf(4));
        check('xhr-stream-complete-text',x.responseText==='first-last');
    }
    {
        const key='empty-upload-'+(++serial),x=new XMLHttpRequest();let upload=false;
        const done=new Promise((resolve,reject)=>{x.onload=resolve;x.onerror=reject;});
        x.upload.onload=()=>upload=true;
        x.onreadystatechange=()=>{if(x.readyState===2){check('xhr-zero-upload-completes',upload);release(key,1).then(()=>release(key,2));x.responseType='arraybuffer';}};
        x.open('POST',url(key));x.send(new ArrayBuffer(0));await done;
        check('xhr-response-type-after-headers',new TextDecoder().decode(x.response)==='first-last');
    }
    const redirect=await fetch('/api/redirect-no-location');
    check('redirect-no-location-response',redirect.status===302&&await redirect.text()==='stay');
}
