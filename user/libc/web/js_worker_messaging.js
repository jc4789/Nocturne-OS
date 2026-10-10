/* Worker channels: local endpoints or generation-bound native broker routes. */
const workerMessagingBridge=(()=>{
    const apply=Reflect.apply,construct=Reflect.construct,define=Object.defineProperty;
    const create=Object.create,freeze=Object.freeze,keys=Object.keys,WM=WeakMap;
    const wmGet=WM.prototype.get,wmSet=WM.prototype.set,wmHas=WM.prototype.has;
    const states=new WM(),channels=new WM(),events=new WM();
    const get=(map,key)=>apply(wmGet,map,[key]),put=(map,key,value)=>apply(wmSet,map,[key,value]);
    const has=(map,key)=>apply(wmHas,map,[key]);
    const append=(list,value)=>define(list,list.length,{value,writable:true,enumerable:true,configurable:true});
    const record=fields=>{const r=create(null),names=keys(fields);for(let i=0;i<names.length;i++){const key=names[i];define(r,key,{value:fields[key],writable:true,enumerable:true,configurable:true});}return r;};
    const tasks=record({head:null,tail:null,closed:false});
    const routes=new Map(),mapGet=Map.prototype.get,mapSet=Map.prototype.set,mapDelete=Map.prototype.delete,mapEach=Map.prototype.forEach,mapClear=Map.prototype.clear,isInteger=Number.isInteger;let nextRoute=0;
    const routeGet=key=>apply(mapGet,routes,[key]),routePut=(key,value)=>apply(mapSet,routes,[key,value]);
    const portSend=host.portSend,portLease=host.portLease;
    const routeKey=(id,creator)=>creator+':'+id;
    const nativeCommit=host.transferCommit,iterator=Symbol.iterator,string=String;
    const wellFormed=String.prototype.toWellFormed;
    const domString=value=>{if(typeof value==='symbol')throw new TypeErr('Symbol is not a DOMString');return string(value);};
    const dispatch=EventTarget.prototype.dispatchEvent,EventType=Event,TargetType=EventTarget;
    const ErrorType=DOMException,TypeErr=TypeError;
    const fail=()=>{throw new ErrorType('Invalid MessagePort transfer','DataCloneError');};
    const state=value=>{const s=get(states,value);if(!s)throw new TypeErr('Illegal MessagePort invocation');return s;};
    const brand=value=>has(states,value);
    const write=(plan,object,key,value)=>append(plan.writes,{object,key,value});
    function reservation(plan,endpoint,owner){
        if(tasks.closed||endpoint.scheduled||!owner.enabled)return;
        const task=record({endpoint,owner,next:null,cancelled:false});
        write(plan,endpoint,'scheduled',task);
        if(tasks.tail)write(plan,tasks.tail,'next',task);else write(plan,tasks,'head',task);
        write(plan,tasks,'tail',task);
    }
    function schedule(endpoint){
        const owner=endpoint.owner;if(!endpoint.head||!owner)return;
        const plan={buffers:[],writes:[],cancels:[]};reservation(plan,endpoint,owner);
        if(plan.writes.length)nativeCommit(plan);
    }
    function enable(s){if(!s.endpoint||tasks.closed)return;s.enabled=true;schedule(s.endpoint);}
    function optionsTransfer(options){
        if(options==null)return [];
        if(typeof options!=='object'&&typeof options!=='function')throw new TypeErr('Expected postMessage options or sequence');
        return cloneData.transferList(options[iterator]!==undefined?options:options.transfer);
    }
    function eventState(value){const s=get(events,value);if(!s)throw new TypeErr('Illegal MessageEvent invocation');return s;}
    class MessageEvent extends EventType{
        constructor(type,init={}){
            if(!arguments.length)throw new TypeErr('MessageEvent type required');
            if(init==null)init={};if(typeof init!=='object'&&typeof init!=='function')throw new TypeErr('Expected MessageEvent dictionary');
            super(domString(type),init);const d=init.data,o=init.origin,l=init.lastEventId,p=init.source;
            const data=d===undefined?null:d,rawOrigin=o===undefined?'':domString(o);
            const origin=wellFormed?apply(wellFormed,rawOrigin,[]):rawOrigin,lastEventId=l===undefined?'':domString(l);
            const source=p===undefined?null:p;if(source!==null&&!brand(source))throw new TypeErr('Invalid MessageEvent source');
            const ports=cloneData.transferList(init.ports);for(let i=0;i<ports.length;i++)if(!brand(ports[i]))throw new TypeErr('Invalid MessageEvent port');
            put(events,this,record({data,origin,lastEventId,source,ports:freeze(ports)}));
        }
        get data(){return eventState(this).data;}get origin(){return eventState(this).origin;}
        get lastEventId(){return eventState(this).lastEventId;}get source(){return eventState(this).source;}
        get ports(){return eventState(this).ports;}
    }
    class MessagePort extends TargetType{
        constructor(){throw new TypeErr('Illegal constructor');}
        postMessage(value,options){
            const s=state(this);if(!arguments.length)throw new TypeErr('Message required');
            // Do not re-read a detached source after author getters have run.
            const target=s.detached||s.closed?null:s.endpoint?.peer,route=s.endpoint?.broker;let transfers;const ports=[];
            if(route){
                const capture=!s.detached&&!route.closed?portLease(0,route.id,route.creator):0;
                try{
                    transfers=optionsTransfer(options);
                    for(let i=0;i<transfers.length;i++)if(brand(transfers[i]))throw new ErrorType('Brokered port re-transfer is not supported','DataCloneError');
                    const packet=cloneData.prepare({data:value,ports:[]},transfers,true),plan=cloneData.commitPlan(packet);cloneData.validate(packet);
                    if(capture)portSend(8,route.id,route.creator,packet.data,plan,capture);else nativeCommit(plan);
                }finally{if(capture)portLease(1,capture,0);}
                return;
            }
            transfers=optionsTransfer(options);
            for(let i=0;i<transfers.length;i++){if(transfers[i]===this)fail();if(brand(transfers[i]))append(ports,transfers[i]);}
            const packet=cloneData.prepare({data:value,ports},transfers),message=cloneData.deserialize(packet.data);
            const plan=cloneData.commitPlan(packet);
            // Author getters may have closed/transferred either endpoint.
            cloneData.validate(packet);
            let doomed=false;for(let i=0;i<ports.length;i++)if(get(states,ports[i]).endpoint===target)doomed=true;
            const owner=target?.owner;
            if(!tasks.closed&&target&&owner&&!doomed){
                const entry=record({message,next:null});
                if(target.tail)write(plan,target.tail,'next',entry);else write(plan,target,'head',entry);
                write(plan,target,'tail',entry);reservation(plan,target,owner);
            }
            // Publication and transfer share one allocation-free native commit.
            // No JS callback/argument allocation follows the first detach.
            nativeCommit(plan);
        }
        start(){enable(state(this));}
        close(){
            const s=state(this);if(s.detached||s.closed)return;
            const endpoint=s.endpoint;
            if(endpoint.broker&&!endpoint.broker.closed){portSend(9,endpoint.broker.id,endpoint.broker.creator,null,{buffers:[],writes:[],cancels:[]});endpoint.broker.closed=true;}
            s.closed=true;s.detached=true;
            const peer=endpoint.peer;endpoint.peer=null;if(peer&&peer.peer===endpoint)peer.peer=null;
            // Retain the closed port queue; tasks captured earlier still run.
        }
        get onmessage(){return state(this).onmessage;}
        set onmessage(value){const s=state(this);s.onmessage=typeof value==='function'?value:null;enable(s);}
        get onmessageerror(){return state(this).onmessageerror;}
        set onmessageerror(value){state(this).onmessageerror=typeof value==='function'?value:null;}
    }
    function makePort(endpoint,closed=false){
        const value=construct(TargetType,[],MessagePort),s=record({value,endpoint,closed,detached:false,enabled:false,onmessage:null,onmessageerror:null});
        put(states,value,s);return s;
    }
    class MessageChannel{
        constructor(){
            const left=record({peer:null,owner:null,head:null,tail:null,scheduled:null,broker:null}),right=record({peer:left,owner:null,head:null,tail:null,scheduled:null,broker:null});left.peer=right;
            const a=makePort(left),b=makePort(right);left.owner=a;right.owner=b;
            put(channels,this,record({port1:a.value,port2:b.value}));
        }
        get port1(){const s=get(channels,this);if(!s)throw new TypeErr('Illegal MessageChannel invocation');return s.port1;}
        get port2(){const s=get(channels,this);if(!s)throw new TypeErr('Illegal MessageChannel invocation');return s.port2;}
    }
    cloneData.registerTransfer({name:'MessagePort',brand,validate(value){const s=state(value);if(s.detached||s.endpoint?.broker)fail();},
        prepare(value){const s=state(value);return makePort(s.endpoint,s.closed);},
        prepareWorker(value){
            const s=state(value),endpoint=s.endpoint,peer=endpoint?.peer;
            if(endpoint?.broker||!peer||peer.broker)throw new ErrorType('Only initial entangled Worker port transfers are supported','DataCloneError');
            if(nextRoute===4294967295)throw new RangeError('Worker port identifier space exhausted');
            const id=++nextRoute,key=routeKey(id,1),queued=[];
            for(let entry=endpoint.head;entry;entry=entry.next){if(entry.message.ports.length)throw new ErrorType('Queued nested Port transfer is not supported','DataCloneError');append(queued,cloneData.prepare(entry.message,[],true).data);}
            const route=record({key,id,creator:1,endpoint:peer,closed:false,sealed:false,active:false});
            const receiver=record({value:null,endpoint,closed:false,detached:false,enabled:false,onmessage:null,onmessageerror:null,exportRoute:route});
            const token=record({receiver,meta:[id,1,queued],route});routePut(key,route);return token;
        },
        commitPlan(value,receiver){
            const s=state(value);if(s.detached||receiver.endpoint!==s.endpoint)fail();
            const endpoint=s.endpoint,plan={writes:[]};
            write(plan,s,'detached',true);write(plan,s,'endpoint',null);write(plan,endpoint,'owner',receiver);
            if(endpoint.scheduled)write(plan,endpoint.scheduled,'cancelled',true);
            write(plan,endpoint,'scheduled',null);
            if(receiver.exportRoute){const route=receiver.exportRoute;write(plan,route,'active',true);write(plan,endpoint,'broker',route);write(plan,endpoint.peer,'broker',route);write(plan,endpoint,'head',null);write(plan,endpoint,'tail',null);}
            return plan;
        }
        // Other external realms/re-transfers remain fail-closed.
    });
    cloneData.registerUncloneable(value=>has(channels,value),'MessageChannel');
    cloneData.registerUncloneable(value=>has(events,value),'MessageEvent');
    define(MessagePort.prototype,Symbol.toStringTag,{value:'MessagePort',configurable:true});
    define(MessageChannel.prototype,Symbol.toStringTag,{value:'MessageChannel',configurable:true});
    define(MessageEvent.prototype,Symbol.toStringTag,{value:'MessageEvent',configurable:true});
    function runOne(){
        while(tasks.head){
            const task=tasks.head;tasks.head=task.next;if(!tasks.head)tasks.tail=null;
            const endpoint=task.endpoint,s=task.owner;
            if(task.cancelled||tasks.closed||endpoint.owner!==s||endpoint.scheduled!==task||!s.enabled)continue;
            endpoint.scheduled=null;const entry=endpoint.head;if(!entry)continue;
            endpoint.head=entry.next;if(!endpoint.head)endpoint.tail=null;
            try{dispatchNativeWorkerEvent(s.value,new MessageEvent('message',{data:entry.message.data,ports:entry.message.ports}));}
            finally{schedule(endpoint);}return true;
        }return false;
    }
    function prepareExternal(value,transfers){
        try{
        const transferred=[],seen=[];
        for(let i=0;i<transfers.length;i++)if(brand(transfers[i])){const endpoint=state(transfers[i]).endpoint;for(let j=0;j<seen.length;j++)if(seen[j]===endpoint||seen[j]===endpoint?.peer)throw new ErrorType('Both ends cannot be exported together','DataCloneError');append(seen,endpoint);append(transferred,transfers[i]);}
        const packet=cloneData.prepare({data:value,ports:transferred},transfers,'worker'),receivers=[],metadata=[];
        for(let i=0;i<packet.tokens.length;i++){const token=packet.tokens[i];if(!token){append(receivers,null);append(metadata,null);continue;}
            append(receivers,token.receiver);append(metadata,token.meta);
        }
        const plan=cloneData.commitPlan(packet,receivers);cloneData.validate(packet);return {packet:[packet.data,metadata],plan,tokens:packet.tokens};
        }catch(error){apply(mapEach,routes,[(route,key)=>{if(!route.active)apply(mapDelete,routes,[key]);}]);throw error;}
    }
    function abortExternal(prepared){for(let i=0;i<prepared.tokens.length;i++){const token=prepared.tokens[i];if(token&&!token.route.active)apply(mapDelete,routes,[token.route.key]);}}
    function importExternal(wire){
        const metadata=wire[1],receivers=[],added=[];
        try{
        for(let i=0;i<metadata.length;i++){const m=metadata[i];if(m===null){append(receivers,null);continue;}
            const id=m[0],creator=m[1],key=routeKey(id,creator);if(!isInteger(id)||id<=0||id>4294967295||creator!==0||routeGet(key))throw new TypeErr('Invalid Worker port ownership');
            const endpoint=record({peer:null,owner:null,head:null,tail:null,scheduled:null,broker:null}),owner=makePort(endpoint);
            const route=record({key,id,creator,endpoint,closed:false,sealed:false,active:true});endpoint.owner=owner;endpoint.broker=route;append(added,route);routePut(key,route);
            for(let j=0;j<m[2].length;j++){const message=cloneData.deserialize(m[2][j],null,true);if(message.ports.length)throw new TypeErr('Nested Worker port queue unsupported');const entry=record({message,next:null});if(endpoint.tail)endpoint.tail.next=entry;else endpoint.head=entry;endpoint.tail=entry;}
            append(receivers,owner);
        }
        if(tasks.closed)throw new TypeErr('Worker port destination retired');return cloneData.deserialize(wire[0],receivers,true);
        }catch(error){for(let i=0;i<added.length;i++){const route=added[i];route.closed=true;route.endpoint.head=route.endpoint.tail=null;apply(mapDelete,routes,[route.key]);}throw error;}
    }
    function receivePort(id,creator,data,closing=false){
        const route=routeGet(routeKey(id,creator));if(!route||!route.active||route.sealed||tasks.closed)return;
        const endpoint=route.endpoint;if(closing){route.closed=route.sealed=true;apply(mapDelete,routes,[route.key]);return;}
        const message=cloneData.deserialize(data,null,true);if(message.ports.length)throw new TypeErr('Nested Worker port transfer unsupported');
        const entry=record({message,next:null}),plan={buffers:[],writes:[],cancels:[]};
        if(endpoint.tail)write(plan,endpoint.tail,'next',entry);else write(plan,endpoint,'head',entry);write(plan,endpoint,'tail',entry);reservation(plan,endpoint,endpoint.owner);nativeCommit(plan);
    }
    return {MessageEvent,MessagePort,MessageChannel,runOne,ready:()=>!tasks.closed&&tasks.head!==null,
        prepareExternal,abortExternal,importExternal,receivePort,
        close(){tasks.closed=true;tasks.head=tasks.tail=null;apply(mapClear,routes,[]);}};
})();
