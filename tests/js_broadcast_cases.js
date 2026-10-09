/* Supplemental real web_live/QuickJS checks, not public-site acceptance. */
async function runBroadcastCases() {
    let n=0;
    const assert=(name,value)=>{n++;if(!value)throw new Error(name);check('broadcast-'+name,true);};
    const throws=(fn,name)=>{try{fn();return false;}catch(e){return e.name===name;}};
    const pause=()=>new Promise(resolve=>setTimeout(resolve,10));
    assert('requires-name',throws(()=>new BroadcastChannel(),'TypeError'));
    assert('symbol-name',throws(()=>new BroadcastChannel(Symbol()),'TypeError'));
    assert('illegal-name-receiver',throws(()=>Object.getOwnPropertyDescriptor(BroadcastChannel.prototype,'name').get.call({}),'TypeError'));
    assert('illegal-post-receiver',throws(()=>BroadcastChannel.prototype.postMessage.call({},1),'TypeError'));
    const name='nocturne-broadcast-猫\0x',a=new BroadcastChannel(name),b=new BroadcastChannel(name),c=new BroadcastChannel(name),other=new BroadcastChannel(name+'other');
    try {
        assert('event-target',a instanceof EventTarget);
        assert('name-and-tag',a.name===name&&Object.prototype.toString.call(a)==='[object BroadcastChannel]');
        assert('name-readonly',Object.getOwnPropertyDescriptor(BroadcastChannel.prototype,'name').set===undefined);
        assert('requires-message',throws(()=>a.postMessage(),'TypeError'));
        assert('reject-function',throws(()=>a.postMessage(()=>{}),'DataCloneError'));
        assert('reject-port',throws(()=>a.postMessage(new MessageChannel().port1),'DataCloneError'));
        assert('reject-channel',throws(()=>structuredClone(a),'DataCloneError'));
        let self=0,foreign=0,events=[],second=[];
        a.onmessage=()=>self++;other.onmessage=()=>foreign++;
        b.onmessage=e=>events.push(e);c.addEventListener('message',e=>second.push(e));
        assert('handler-roundtrip',typeof b.onmessage==='function'&&b.onmessageerror===null);
        const bytes=new Uint8Array([7,8]),data={bytes,alias:bytes,map:new Map()};data.self=data;data.map.set(data,bytes);
        a.postMessage(data);bytes[0]=99;
        assert('async-delivery',events.length===0&&second.length===0);
        await pause();
        assert('no-self-or-other-name',self===0&&foreign===0);
        assert('all-receivers',events.length===1&&second.length===1);
        const e=events[0],x=e.data,y=second[0].data;
        assert('message-event',e instanceof MessageEvent&&e.type==='message'&&e.isTrusted);
        assert('event-metadata',e.origin===location.origin&&e.source===null&&e.ports.length===0&&e.lastEventId==='');
        assert('clone-cycle',x!==data&&x.self===x);
        assert('clone-alias',x.alias===x.bytes&&x.map.get(x)===x.bytes);
        assert('buffer-snapshot',x.bytes[0]===7&&bytes[0]===99&&bytes.byteLength===2);
        x.bytes[0]=31;
        assert('separate-recipient-clones',y!==x&&y.bytes[0]===7&&y.self===y);
        events=[];second=[];a.postMessage(1);a.postMessage(2);a.postMessage(3);await pause();
        assert('fifo',events.map(e=>e.data).join()==='1,2,3'&&second.map(e=>e.data).join()==='1,2,3');
        const closed=new BroadcastChannel(name);let closedCount=0;closed.onmessage=()=>closedCount++;
        a.postMessage('queued');closed.close();closed.close();await pause();
        assert('close-drops-queued',closedCount===0);
        assert('closed-post-error',throws(()=>closed.postMessage(1),'InvalidStateError'));
        events=[];a.postMessage('before-close');a.close();await pause();
        assert('sender-close-keeps-sent',events.length===1&&events[0].data==='before-close');
        assert('closed-name-kept',a.name===name);
        b.onmessage=null;assert('clear-handler',b.onmessage===null);
    } finally {a.close();b.close();c.close();other.close();}
    return n;
}
