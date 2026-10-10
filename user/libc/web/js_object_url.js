/* 文書単位の実 Blob / MediaSource URL。ネイティブのfile権限へ変換しない。 */
const objectURLBridge = (() => {
    const URLType=URL, apply=Reflect.apply, define=Object.defineProperty;
    const blobSize=Object.getOwnPropertyDescriptor(Blob.prototype,'size').get;
    const urls=new Map(), get=Map.prototype.get, put=Map.prototype.set, remove=Map.prototype.delete;
    const origin=new URLType(host.url()).origin, random=crypto.getRandomValues, cryptoObject=crypto;
    const hex=Number.prototype.toString,pad=String.prototype.padStart, LIMIT=64*1024*1024;
    let bytes=0,count=0,mediaBrand=()=>false;
    const string=v=>{if(typeof v==='symbol')throw new TypeError('Cannot convert Symbol to string');return String(v);};
    function key(value){return string(value).split('#')[0];}
    function lookup(value){return apply(get,urls,[key(value)]);}
    function createObjectURL(value){
        const blob=blobBridge.brand(value),media=mediaBrand(value);
        if(!blob&&!media)throw new TypeError('Blob or MediaSource required');
        const size=blob?apply(blobSize,value,[]):0;
        if(count>=256||size>LIMIT-bytes)throw new DOMException('Object URL quota exceeded','QuotaExceededError');
        let id;do{const data=new Uint8Array(16);apply(random,cryptoObject,[data]);id='';for(let i=0;i<data.length;i++)id+=apply(pad,apply(hex,data[i],[16]),[2,'0']);id='blob:'+origin+'/'+id;}while(lookup(id));
        const policyHeaders=host.safety('policyHeaders',null);
        apply(put,urls,[id,{value,size,blob,policyHeaders}]);bytes+=size;count++;return id;
    }
    function revokeObjectURL(value){const id=key(value),entry=apply(get,urls,[id]);if(entry){apply(remove,urls,[id]);bytes-=entry.size;count--;}}
    define(URLType,'createObjectURL',{value:createObjectURL,writable:true,configurable:true,enumerable:true});
    define(URLType,'revokeObjectURL',{value:revokeObjectURL,writable:true,configurable:true,enumerable:true});
    return {
        registerMediaSource(fn){mediaBrand=fn;},
        mediaSource(value){const entry=lookup(value);return entry&&!entry.blob?entry.value:null;},
        blob(value){const entry=lookup(value);return entry&&entry.blob?entry.value:null;},
        policy(value){const entry=lookup(value);return entry&&entry.blob?entry.policyHeaders:null;},
        fetch(value,method,range=null){
            const entry=lookup(value);if(!entry||!entry.blob||method!=='GET'&&method!=='HEAD')throw new TypeError('Blob URL is revoked, unavailable, or not fetchable');
            const type=blobBridge.type(entry.value);let first=0,last=entry.size-1,status=200;
            if(method==='GET'&&range!==null){
                const match=/^bytes=(\d*)-(\d*)$/.exec(range);
                if(!match||!match[1]&&!match[2]||!entry.size)throw new TypeError('Invalid Blob range');
                const a=match[1]?Number(match[1]):null,b=match[2]?Number(match[2]):null;
                if(a!==null&&!Number.isSafeInteger(a)||b!==null&&!Number.isSafeInteger(b))throw new TypeError('Invalid Blob range');
                if(a===null){if(!b)throw new TypeError('Invalid Blob suffix range');first=Math.max(0,entry.size-b);}
                else {first=a;if(b!==null)last=Math.min(last,b);}
                if(first>=entry.size||last<first)throw new TypeError('Blob range is not satisfiable');status=206;
            }
            const full=method==='HEAD'?null:blobBridge.bytes(entry.value);
            const data=full===null?null:status===206?full.slice(first,last+1):full;
            return {bytes:data,type,size:method==='HEAD'?entry.size:last-first+1,status,range:status===206?'bytes '+first+'-'+last+'/'+entry.size:null};
        }
    };
})();
