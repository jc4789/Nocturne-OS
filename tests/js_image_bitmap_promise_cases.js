/* Only the newly repaired conversion/semantic rejection and task-order seam. */
(async()=>{
    let count=0;
    function ck(name,pass){count++;console.log((pass?'OK bitmap-promise ':'FAIL bitmap-promise ')+name);}
    async function early(call){
        const events=[];let promise,reason,escaped=false;
        try{promise=call();}catch(error){escaped=true;reason=error;}
        const isPromise=promise instanceof Promise;
        if(isPromise)promise.then(()=>events.push('fulfilled'),error=>{reason=error;events.push('rejected');});
        queueMicrotask(()=>events.push('microtask'));
        await Promise.resolve();
        console.log('BITMAP-PROMISE-ORDER '+events.join(',')+' name='+(reason&&reason.name||typeof reason));
        return {reason,correct:!escaped&&isPromise&&events.join(',')==='rejected,microtask'};
    }
    const data=new ImageData(1,1),order=[];
    const options={};
    for(const key of ['colorSpaceConversion','imageOrientation','premultiplyAlpha','resizeHeight','resizeQuality','resizeWidth'])
        Object.defineProperty(options,key,{get(){order.push(key);return key==='resizeWidth'?0:key==='premultiplyAlpha'?'premultiply':undefined;}});
    let result=await early(()=>createImageBitmap(data,0,0,0,1,options));
    ck('zero-crop-width-before-resize-and-unsupported',result.correct&&result.reason.name==='RangeError'&&order.join(',')==='colorSpaceConversion,imageOrientation,premultiplyAlpha,resizeHeight,resizeQuality,resizeWidth');
    result=await early(()=>createImageBitmap(data,0,0,1,0,{resizeHeight:0}));
    ck('zero-crop-height-before-resize',result.correct&&result.reason.name==='RangeError');
    result=await early(()=>createImageBitmap(data,{resizeWidth:0,resizeQuality:'high'}));
    ck('zero-resize-before-unsupported-raster',result.correct&&result.reason.name==='InvalidStateError');
    result=await early(()=>createImageBitmap(data,0,0,0,1,{get imageOrientation(){throw undefined;}}));
    ck('falsy-conversion-exception-before-crop',result.correct&&result.reason===undefined);
    const sentinel=Symbol('bitmap-option-conversion');
    result=await early(()=>createImageBitmap(data,{resizeWidth:{valueOf(){throw sentinel;}}}));
    ck('dimension-author-conversion-rejected-not-thrown',result.correct&&result.reason===sentinel);
    result=await early(()=>createImageBitmap(data,0n,0,1,1));
    ck('crop-bigint-conversion-reaction-order',result.correct&&result.reason.name==='TypeError');
    result=await early(()=>createImageBitmap(data,0,0,0,1,{imageOrientation:'invalid'}));
    ck('enum-conversion-before-zero-crop-algorithm',result.correct&&result.reason.name==='TypeError');
    result=await early(()=>createImageBitmap());
    ck('missing-source-is-immediately-rejected-promise',result.correct&&result.reason.name==='TypeError');
    let settled=false;
    const successful=createImageBitmap(data);successful.then(()=>{settled=true;});
    await Promise.resolve();const stillDeferred=!settled;
    const bitmap=await successful;
    ck('native-success-remains-deferred-bitmap-task',stillDeferred&&bitmap.width===1&&bitmap.height===1);bitmap.close();
    settled=false;
    const invalid=createImageBitmap(new Blob(['invalid image bytes']));invalid.catch(()=>{settled=true;});
    await Promise.resolve();const decodeDeferred=!settled;
    let decodeName;try{await invalid;}catch(error){decodeName=error.name;}
    ck('invalid-blob-decode-remains-deferred-task',decodeDeferred&&decodeName==='InvalidStateError');
    console.log('BITMAP-PROMISE-DONE '+count);
})().catch(error=>console.error('BITMAP-PROMISE-FATAL '+(error&&error.name)+': '+(error&&error.message)));
