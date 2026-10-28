/* HTTP fixture から独立 Worker realm で読み込む実 Wasm 補助試験。 */
let wasmWorkerStarted=false;
onmessage=async event=>{
    if(event.data&&event.data.type==='run-wasm-policy'){
        let checks=0;
        try{
            const {allowed,reports}=event.data,bytes=new Uint8Array([0,97,115,109,1,0,0,0]),events=[];
            const assert=(ok,label)=>{if(!ok)throw new Error(label);checks++;};
            let complete;const reported=new Promise(resolve=>{complete=resolve;});
            addEventListener('securitypolicyviolation',e=>{events.push(e);if(events.length===reports)complete();});
            const validated=WebAssembly.validate(bytes);assert(validated,'Worker CSP validate');
            const response=()=>new Response(bytes,{headers:{'Content-Type':'application/wasm'}});
            for(const operation of [()=>new WebAssembly.Module(bytes),()=>WebAssembly.compile(bytes),()=>WebAssembly.instantiate(bytes),()=>WebAssembly.compileStreaming(response()),()=>WebAssembly.instantiateStreaming(response())]){
                let result,error;try{result=await operation();}catch(e){error=e;}
                assert(allowed?!error:!!error&&error.name==='CompileError','Worker native CSP byte compilation');
                if(allowed)assert(result instanceof WebAssembly.Module||result.module instanceof WebAssembly.Module,'Worker CSP executable module');
            }
            if(reports)await reported;
            assert(events.length===reports,'Worker native CSP violation count');
            for(const e of events)assert(e.isTrusted&&e.effectiveDirective==='script-src'&&e.blockedURI==='wasm-eval'&&e.disposition===(allowed?'report':'enforce'),'Worker CSP violation fields');
            postMessage({type:'wasm-policy-result',ok:true,checks,events:events.length,validated});
        }catch(error){postMessage({type:'wasm-policy-result',ok:false,checks,error:String(error)});}
        return;
    }
    if(wasmWorkerStarted||!event.data||event.data.type!=='run-wasm')return;
    wasmWorkerStarted=true;
    let checks=0;
    const assert=(ok,label)=>{if(!ok)throw new Error(label);checks++;};
    try{
        // Core binary encoding: (func (export "add") (param i32 i32)
        // (result i32) local.get 0 local.get 1 i32.add).
        const add=new Uint8Array([0,97,115,109,1,0,0,0,1,7,1,0x60,2,0x7f,0x7f,1,0x7f,
            3,2,1,0,7,7,1,3,97,100,100,0,0,10,9,1,7,0,0x20,0,0x20,1,0x6a,0x0b]);
        const module=await WebAssembly.compile(add);
        assert(module instanceof WebAssembly.Module,'Worker compile Module');
        const instance=await WebAssembly.instantiate(module),moduleOverload=instance.exports.add(19,23);
        assert(moduleOverload===42,'Worker Module instantiate');
        const pair=await WebAssembly.instantiate(add),bytesOverload=pair.instance.exports.add(20,22);
        assert(pair.module instanceof WebAssembly.Module&&bytesOverload===42,'Worker bytes instantiate');
        const memory=new WebAssembly.Memory({initial:1,maximum:2}),old=memory.buffer;
        new Uint8Array(old)[5]=29;
        for(const name of ['transfer','transferToFixedLength']){
            assert(typeof ArrayBuffer.prototype[name]==='function','Worker '+name+' API present');
            let error;try{ArrayBuffer.prototype[name].call(old);}catch(e){error=e;}
            assert(error&&error.name==='TypeError','Worker Wasm '+name+' prohibited');
            assert(old.byteLength===65536&&new Uint8Array(old)[5]===29,'Worker '+name+' rejection keeps bytes');
        }
        assert(typeof structuredClone==='function','Worker structuredClone API present');
        let cloneError;try{structuredClone(old,{transfer:[old]});}catch(error){cloneError=error;}
        assert(cloneError&&cloneError.name==='DataCloneError','Worker Wasm structuredClone transfer prohibited');
        assert(old.byteLength===65536&&new Uint8Array(old)[5]===29,'Worker clone rejection keeps bytes');
        const atomic=new ArrayBuffer(4);new Uint8Array(atomic)[0]=31;
        let atomicError;try{structuredClone({atomic,old},{transfer:[atomic,old]});}catch(error){atomicError=error;}
        assert(atomicError&&atomicError.name==='DataCloneError'&&atomic.byteLength===4&&new Uint8Array(atomic)[0]===31&&old.byteLength===65536,'Worker transfer preflight preserves all source buffers');
        let postError;try{postMessage({type:'memory-must-not-transfer',buffer:old},[old]);}catch(error){postError=error;}
        assert(postError&&postError.name==='DataCloneError'&&old.byteLength===65536&&new Uint8Array(old)[5]===29,'Worker postMessage transfer prohibition');
        const ordinary=new ArrayBuffer(4);new Uint8Array(ordinary)[0]=37;
        const moved=ordinary.transfer(),fixed=moved.transferToFixedLength(),cloned=structuredClone(fixed,{transfer:[fixed]});
        assert(ordinary.byteLength===0&&moved.byteLength===0&&fixed.byteLength===0&&cloned.byteLength===4&&new Uint8Array(cloned)[0]===37,'Worker ordinary ArrayBuffer transfer controls');
        const copied=structuredClone(old);new Uint8Array(copied)[5]=30;
        assert(old.byteLength===65536&&new Uint8Array(old)[5]===29&&new Uint8Array(copied)[5]===30,'Worker nontransfer memory clone is independent');
        assert(memory.grow(1)===1,'Worker Memory grow');
        const memoryBytes=memory.buffer.byteLength,memoryValue=new Uint8Array(memory.buffer)[5],oldDetached=old.byteLength===0;
        assert(memoryBytes===131072&&memoryValue===29&&oldDetached,'Worker Memory buffer lifetime');
        // (import "env" "callback" (func (param i32) (result i32)))
        // (export "run" (func 0)).
        const imported=new Uint8Array([0,97,115,109,1,0,0,0,1,6,1,0x60,1,0x7f,1,0x7f,
            2,16,1,3,101,110,118,8,99,97,108,108,98,97,99,107,0,0,
            7,7,1,3,114,117,110,0,0]);
        const linked=await WebAssembly.instantiate(imported,{env:{callback:value=>value+1}}),callback=linked.instance.exports.run(41);
        assert(callback===42,'Worker import callback');
        const response=new Response(add,{headers:{'Content-Type':'application/wasm'}});
        const streamModule=await WebAssembly.compileStreaming(Promise.resolve(response));
        const streaming=(await WebAssembly.instantiate(streamModule)).exports.add(21,21),streamBodyUsed=response.bodyUsed;
        assert(streaming===42&&streamBodyUsed,'Worker compileStreaming consumes Response');
        const streamPair=await WebAssembly.instantiateStreaming(new Response(add,{headers:{'Content-Type':'application/wasm'}}));
        const streamingPair=streamPair.instance.exports.add(18,24);
        assert(streamPair.module instanceof WebAssembly.Module&&streamingPair===42,'Worker instantiateStreaming');
        const dataResponse=await fetch('data:,%00A%FF%2B+%25%G0#lost');
        assert(dataResponse.status===200&&dataResponse.statusText==='OK'&&dataResponse.type==='basic'&&!dataResponse.redirected,'Worker data Fetch metadata');
        assert(dataResponse.url==='data:,%00A%FF%2B+%25%G0'&&dataResponse.headers.get('content-type')==='text/plain;charset=US-ASCII','Worker data URL/MIME');
        assert(Array.from(new Uint8Array(await dataResponse.arrayBuffer())).join(',')==='0,65,255,43,43,37,37,71,48','Worker data bytes preserve NUL/nonASCII/percent');
        let headerError;try{dataResponse.headers.set('x-test','bad');}catch(error){headerError=error;}
        assert(headerError&&headerError.name==='TypeError','Worker immutable data headers');
        const parameterResponse=await fetch('data:TEXT/PLAIN;CHARSET="utf-8";charset=bad,x');
        assert(parameterResponse.headers.get('content-type')==='text/plain;charset=utf-8'&&await parameterResponse.text()==='x','Worker data MIME normalization');
        for(const mode of ['cors','same-origin','no-cors']){const response=await fetch('data:,mode',{mode,redirect:'manual'});assert(response.type==='basic'&&await response.text()==='mode','Worker data mode '+mode);}
        assert(await (await fetch('data:,cache',{cache:'only-if-cached',mode:'same-origin',credentials:'include',keepalive:true})).text()==='cache','Worker data bypasses only HTTP policy limits');
        assert(await (await fetch('data:,post',{method:'POST',body:'ignored'})).text()==='post','Worker data POST is not GET-only');
        const head=await fetch('data:,head',{method:'HEAD'});assert(head.body===null&&await head.text()==='','Worker data HEAD');
        for(const url of ['data:','data:;base64,YQ=','data:;base64,YQ==%00evil']){let error;try{await fetch(url);}catch(e){error=e;}assert(error&&error.name==='TypeError','Worker malformed data rejects');}
        const pre=new AbortController(),reason={workerDataAbort:true};pre.abort(reason);let preError;try{await fetch('data:,pre',{signal:pre.signal});}catch(error){preError=error;}
        assert(preError===reason,'Worker data pre-abort reason identity');
        const pending=new AbortController(),pendingPromise=fetch('data:,pending',{signal:pending.signal});pending.abort(reason);let pendingError;try{await pendingPromise;}catch(error){pendingError=error;}
        assert(pendingError===reason,'Worker data pending abort reason identity');
        const late=new AbortController(),lateResponse=await fetch('data:,late',{signal:late.signal}),lateClone=lateResponse.clone();late.abort(reason);
        for(const response of [lateResponse,lateClone]){let error;try{await response.text();}catch(e){error=e;}assert(error===reason,'Worker data response body/clone abort');}
        // A Worker's Fetch is the Window's (js_fetch.js): Subresource Integrity checks the bytes.
        assert(await (await fetch('data:,hello',{integrity:'sha256-LPJNul+wow4m6DsqxbninhsWHlwfp0JecwQzYpOLmCQ='})).text()==='hello','Worker integrity match');
        let integrityError;try{await fetch('data:,hello',{integrity:'sha256-47DEQpj8HBSa+/TImW+5JCeuQeRkm5NMpJWZG3hSuFU='});}catch(error){integrityError=error;}
        assert(integrityError&&integrityError.name==='TypeError','Worker integrity mismatch is a network error');
        const dataWasm='data:application/wasm;base64,AGFzbQEAAAABBwFgAn9/AX8DAgEABwcBA2FkZAAACgkBBwAgACABags=';
        const dataPair=await WebAssembly.instantiate(await (await fetch(dataWasm)).arrayBuffer()),dataWasmValue=dataPair.instance.exports.add(17,25);
        assert(dataWasmValue===42,'Worker data fetch bytes execute Wasm');
        const dataStream=await WebAssembly.instantiateStreaming(fetch(dataWasm));
        assert(dataStream.instance.exports.add(16,26)===42,'Worker data fetch streaming API executes Wasm');
        // A defined Wasm function calls a Suspending import, then adds one
        // after resumption. An exported import alone would not exercise the
        // Worker's suspended native Wasm frame and its post-await arithmetic.
        const jspiBytes=new Uint8Array([0,97,115,109,1,0,0,0,
            1,6,1,0x60,1,0x7f,1,0x7f,
            2,16,1,3,101,110,118,8,99,97,108,108,98,97,99,107,0,0,
            3,2,1,0,7,7,1,3,114,117,110,0,1,
            10,11,1,9,0,0x20,0,0x10,0,0x41,1,0x6a,0x0b]);
        assert(typeof WebAssembly.Suspending==='function'&&typeof WebAssembly.promising==='function'&&typeof WebAssembly.SuspendError==='function','Worker real JSPI APIs');
        const jspiModule=new WebAssembly.Module(jspiBytes);
        const relay=marker=>new WebAssembly.Instance(jspiModule,{env:{callback:marker}}).exports.run;
        const marked=callback=>relay(new WebAssembly.Suspending(callback));
        let promiseCalls=0;
        const marker=new WebAssembly.Suspending(value=>{promiseCalls++;return Promise.resolve(value+1);});
        assert(typeof marker==='object'&&marker instanceof WebAssembly.Suspending,'Worker Suspending marker brand');
        let markerError;try{Reflect.apply(marker,undefined,[40]);}catch(error){markerError=error;}
        assert(markerError instanceof TypeError,'Worker Suspending marker is not callable');
        assert(await WebAssembly.promising(marked(value=>value+1))(40)===42,'Worker synchronous marked import resumes');
        const promiseRun=relay(marker),promiseResult=WebAssembly.promising(promiseRun)(40);
        assert(promiseResult instanceof Promise&&promiseCalls===1,'Worker promising returns actual Promise without replay');
        assert(await promiseResult===42&&promiseCalls===1,'Worker Promise fulfillment resumes native arithmetic');
        let ordinaryError;try{promiseRun(40);}catch(error){ordinaryError=error;}
        assert(ordinaryError instanceof WebAssembly.SuspendError&&promiseCalls===1,'Worker ordinary export rejects suspension before callback');
        let thenReads=0,thenCalls=0;
        const thenable={get then(){thenReads++;return resolve=>{thenCalls++;resolve(41);};}};
        assert(await WebAssembly.promising(marked(()=>thenable))(0)===42&&thenReads===1&&thenCalls===1,'Worker thenable assimilates once before native resume');
        let timerRan=false,timerCalls=0,timerSettled=false;
        const timerResult=WebAssembly.promising(marked(value=>{timerCalls++;return new Promise(resolve=>setTimeout(()=>{timerRan=true;resolve(value+1);},0));}))(40);
        timerResult.then(()=>{timerSettled=true;});
        assert(timerResult instanceof Promise&&timerCalls===1&&!timerRan&&!timerSettled,'Worker timer import returns control while pending');
        assert(await timerResult===42&&timerRan&&timerCalls===1,'Worker event-loop task resumes native continuation once');
        assert(await WebAssembly.promising(marked(async()=>Number(await (await fetch('data:,41')).text())))(0)===42,'Worker data Fetch asynchronous import resumes native arithmetic');
        const rejectionReason={workerJspi:'reject'};let rejection;
        try{await WebAssembly.promising(marked(()=>Promise.reject(rejectionReason)))(0);}catch(error){rejection=error;}
        assert(rejection===rejectionReason,'Worker rejected import preserves exact reason');
        postMessage({type:'wasm-worker-result',ok:true,checks,compiled:true,moduleOverload,bytesOverload,callback,
            memoryBytes,memoryValue,oldDetached,streaming,streamBodyUsed,streamingPair,dataWasmValue});
    }catch(error){postMessage({type:'wasm-worker-result',ok:false,checks,error:String(error)+' '+(error.stack||'')});}
};
