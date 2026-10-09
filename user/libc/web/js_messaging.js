/* Same-runtime channels use Nocturne's native posted-task queue. No timer slots,
 * Promise polling, worker emulation, or public page-controlled clone hooks. */
const messagingBridge=(() => {
    const apply=Reflect.apply,create=Object.create,define=Object.defineProperty,freeze=Object.freeze;
    const WM=WeakMap,wmGet=WM.prototype.get,wmSet=WM.prototype.set,wmHas=WM.prototype.has;
    const ports=new WM(),channels=new WM(),events=new WM(),broadcastChannels=new WM();
    const readyPackets=new WM(),imports=new WM(),SetType=Set,setHas=Set.prototype.has,setAdd=Set.prototype.add;
    const get=(map,key)=>apply(wmGet,map,[key]),put=(map,key,v)=>apply(wmSet,map,[key,v]);
    const has=(map,key)=>apply(wmHas,map,[key]);
    const TypeErr=TypeError,DomError=DOMException,URLType=URL;
    const originGet=Object.getOwnPropertyDescriptor(URLType.prototype,'origin').get;
    const postTask=host.postTask,cancelPost=host.cancelPost,string=elementURL.string;
    const iteratorSymbol=Symbol.iterator,eventInitMethod=Event.prototype.initEvent;
    const portBrand=p=>has(ports,p);
    const workerRoutes=new Map(),routeGet=Map.prototype.get,routeSet=Map.prototype.set,routeDelete=Map.prototype.delete,routeEach=Map.prototype.forEach,isInteger=Number.isInteger;
    const workerNative=host.worker;
    const workerKey=(child,id,creator)=>child+':'+creator+':'+id;
    const workerRoute=(child,id,creator)=>apply(routeGet,workerRoutes,[workerKey(child,id,creator)]);
    const appendWorker=(a,v)=>define(a,a.length,{value:v,writable:true,enumerable:true,configurable:true});
    function workerPlan(){return {buffers:[],writes:[],cancels:[],generations:[realm.generation]};}
    function workerWrite(plan,object,key,value){appendWorker(plan.writes,{object,key,value});}
    const fail=()=>{throw new DomError('Value cannot be transferred','DataCloneError');};
    function check(p){const s=get(ports,p);if(!s)throw new TypeErr('Illegal MessagePort receiver');return s;}
    function eventCheck(e){const s=get(events,e);if(!s)throw new TypeErr('Illegal MessageEvent receiver');return s;}
    function eventInit(init){
        if(init==null)init={};
        if(typeof init!=='object'&&typeof init!=='function')throw new TypeErr('Expected a dictionary');
        const d=init.data,data=d===undefined?null:d;
        const lid=init.lastEventId,id=lid===undefined?'':string(lid);
        const og=init.origin,o=og===undefined?'':string(og);
        const ps=cloneData.transferList(init.ports);
        for(let i=0;i<ps.length;i++)if(!portBrand(ps[i]))throw new TypeErr('Expected MessagePort');
        const source=init.source??null;
        if(source!==null&&source!==globalThis&&!portBrand(source)&&!host.frame('messageBrand',source))throw new TypeErr('Invalid message source');
        return {data,origin:o,lastEventId:id,source,ports:freeze(ps)};
    }
    class MessageEvent extends Event {
        constructor(type,init={}){
            if(!arguments.length)throw new TypeErr('MessageEvent requires a type');
            super(string(type),init??{});put(events,this,eventInit(init));
        }
        get data(){return eventCheck(this).data;}
        get origin(){return eventCheck(this).origin;}
        get lastEventId(){return eventCheck(this).lastEventId;}
        get source(){return eventCheck(this).source;}
        get ports(){return eventCheck(this).ports;}
        initMessageEvent(type,bubbles=false,cancelable=false,data=null,origin='',lastEventId='',source=null,ports=[]){
            eventCheck(this);if(!arguments.length)throw new TypeErr('MessageEvent requires a type');
            const t=string(type),init=eventInit({data,origin,lastEventId,source,ports});
            if(this._dispatching)return;
            apply(eventInitMethod,this,[t,bubbles,cancelable]);put(events,this,init);
        }
    }
    function makePort(endpoint){
        const value=create(MessagePort.prototype),s={value,endpoint,detached:false,enabled:false,realm,queue:null};
        put(ports,value,s);return s;
    }
    function deliver(target,packet,source=null,senderOrigin=''){
        let event;
        try{
            const payload=get(readyPackets,packet)||cloneData.deserialize(packet);
            event=new MessageEvent('message',{data:payload.data,ports:payload.ports,source,origin:senderOrigin});
        }catch(_){event=new MessageEvent('messageerror',{source,origin:senderOrigin});}
        event.isTrusted=true;dispatch(target,event);
    }
    function schedule(endpoint,reserve=false){
        if(endpoint.owner.realm!==realm)return endpoint.owner.realm.schedule(endpoint,reserve);
        if(endpoint.scheduled||(!endpoint.head&&!reserve)||!endpoint.owner.enabled)return;
        const owner=endpoint.owner;
        endpoint.scheduled=postTask(()=>{
            if(endpoint.owner!==owner)return;
            endpoint.scheduled=0;
            // A transfer moves the queue and disables the new port until start.
            if(!endpoint.owner.enabled||!endpoint.head)return;
            const packet=endpoint.head;endpoint.head=packet.next;
            if(!endpoint.head)endpoint.tail=null;
            endpoint.count--;
            // A full native queue must not discard the already-popped message.
            // Each native callback ends with its own microtask checkpoint.
            try{deliver(owner.value,packet.data);}finally{schedule(endpoint);}
        });
        endpoint.scheduledRealm=realm;
    }
    function cancelScheduled(endpoint){
        if(endpoint.scheduled){endpoint.scheduledRealm.cancel(endpoint.scheduled);endpoint.scheduled=0;}
    }
    function enable(s){s.enabled=true;if(s.endpoint)schedule(s.endpoint);}
    function prepare(message,transfers){
        const transferred=[];
        for(let i=0;i<transfers.length;i++)if(portBrand(transfers[i]))
            define(transferred,transferred.length,{value:transfers[i],writable:true,enumerable:true,configurable:true});
        const packet=cloneData.prepare({data:message,ports:transferred},transfers,'ports');
        return {data:packet.data,tokens:packet.tokens,
            generation:realm.generation,validate:()=>cloneData.validate(packet),
            plan:receivers=>cloneData.commitPlan(packet,receivers)};
    }
    // Endpoints and these closures are private same-runtime capabilities. Only
    // the ordinary graph is written to the Window message wire representation.
    function importPacket(data,envelope,transaction=null,depth=0){
        if(depth>64)throw new DomError('Nested port transfer limit reached','QuotaExceededError');
        const t=transaction||{jobs:[],endpoints:new SetType(),messages:0,committed:false},receivers=create(null);
        const tokens=envelope.tokens;
        for(let i=0;i<tokens.length;i++){
            const token=tokens[i];
            if(token===null){receivers[i]=null;continue;}
            const endpoint=token.endpoint;
            if(apply(setHas,t.endpoints,[endpoint]))fail();
            apply(setAdd,t.endpoints,[endpoint]);
            const receiver=makePort(endpoint);receivers[i]=receiver;
            let head=null,tail=null,count=0;
            for(let entry=endpoint.head;entry;entry=entry.next){
                if(++t.messages>4096)throw new DomError('Transferred port queue quota reached','QuotaExceededError');
                const pending=endpoint.owner.realm.exportPacket(entry.data);
                const imported=importPacket(pending.data,pending,t,depth+1);
                const copy={data:imported.packet,next:null};
                if(tail)tail.next=copy;else head=copy;tail=copy;count++;
            }
            receiver.queue={head,tail,count};
        }
        const payload=cloneData.deserialize(data,receivers,true),packet={};
        put(readyPackets,packet,payload);
        define(t.jobs,t.jobs.length,{value:{envelope,receivers},writable:true,enumerable:true,configurable:true});
        const imported={packet};put(imports,imported,t);return imported;
    }
    function commitImported(imported){
        const t=get(imports,imported);if(!t||t.committed)throw new TypeErr('Invalid message transfer commit');
        // All allocation and every late validation precede *any* detach.
        for(let i=0;i<t.jobs.length;i++)t.jobs[i].envelope.validate();
        const plan={buffers:[],writes:[],cancels:[],generations:[realm.generation]};
        const append=(array,value)=>define(array,array.length,{value,writable:true,enumerable:true,configurable:true});
        for(let i=0;i<t.jobs.length;i++){
            const job=t.jobs[i],p=job.envelope.plan(job.receivers);
            append(plan.generations,job.envelope.generation);
            const keys=['buffers','writes','cancels'];
            for(let k=0;k<keys.length;k++){const key=keys[k];for(let j=0;j<p[key].length;j++)append(plan[key],p[key][j]);}
        }
        append(plan.writes,{object:t,key:'committed',value:true});
        // All JS calls/argument arrays finish before this single native commit.
        // It validates/materializes its C plan before detach and never calls JS.
        return host.frame('transferCommit',plan);
    }
    const realm={schedule,cancel:cancelPost,generation:host.frame('transferGeneration'),importPacket,commitImported,exportPacket(packet){
        const payload=get(readyPackets,packet);
        if(!payload)throw new TypeErr('Invalid private port packet');
        return prepare(payload.data,payload.ports);
    }};
    function listOrOptions(options){
        if(options==null)return [];
        if(typeof options!=='object'&&typeof options!=='function')throw new TypeErr('Expected transfer sequence or options');
        const iterator=options[iteratorSymbol];
        return cloneData.transferList(iterator!=null?{[iteratorSymbol]:()=>apply(iterator,options,[])}:options.transfer);
    }
    class MessagePort extends EventTarget {
        constructor(){super();throw new TypeErr('Illegal constructor');}
        postMessage(message,options={}){
            const s=check(this);if(!arguments.length)throw new TypeErr('Message required');
            const target=s.endpoint?s.endpoint.peer:null,transfers=listOrOptions(options);
            if(s.endpoint?.broker){
                for(let i=0;i<transfers.length;i++)if(portBrand(transfers[i]))throw new DomError('Brokered port re-transfer is not supported','DataCloneError');
                const packet=cloneData.prepare({data:message,ports:[]},transfers,true),plan=cloneData.commitPlan(packet);plan.generations=[realm.generation];cloneData.validate(packet);
                const route=s.endpoint.broker;if(route.closed){host.frame('transferCommit',plan);return;}
                workerNative(6,route.child,packet.data,plan,route.id,route.creator);return;
            }
            let doomed=false;
            for(let i=0;i<transfers.length;i++){
                if(transfers[i]===this)fail();
                if(target&&transfers[i]===target.owner.value)doomed=true;
            }
            if(target&&target.count>=4096)throw new DomError('Port message queue quota reached','QuotaExceededError');
            const p={data:null,next:null};
            const envelope=prepare(message,transfers);
            if(target&&target.count>=4096)throw new DomError('Port message queue quota reached','QuotaExceededError');
            // The current endpoint owner, not the sending realm, allocates the
            // real target brands and reserves its native posted task.
            const owner=target?target.owner:null;
            let reserved=false;
            try{
                const imported=(owner?owner.realm:realm).importPacket(envelope.data,envelope);
                if(target&&!doomed){reserved=!target.scheduled;schedule(target,true);}
                (owner?owner.realm:realm).commitImported(imported);
                p.data=imported.packet;
            }catch(error){
                if(reserved&&target&&target.scheduled&&!target.head)cancelScheduled(target);
                throw error;
            }
            if(!target||doomed)return;
            if(target.tail)target.tail.next=p;else target.head=p;
            // Enabled destinations already reserved their native task before
            // commit. No JS schedule call/allocation is allowed after detach.
            target.tail=p;target.count++;
        }
        start(){enable(check(this));}
        close(){
            const s=check(this);
            if(s.endpoint?.broker&&!s.endpoint.broker.closed){const r=s.endpoint.broker;workerNative(7,r.child,null,workerPlan(),r.id,r.creator);r.closed=true;}
            s.detached=true;
            if(s.endpoint&&s.endpoint.peer){s.endpoint.peer.peer=null;s.endpoint.peer=null;}
            if(s.endpoint){cancelScheduled(s.endpoint);s.endpoint.head=s.endpoint.tail=null;s.endpoint.count=0;}
        }
        get onmessage(){check(this);return handlerValue(this,'message');}
        set onmessage(v){const s=check(this);setHandler(this,'message',v,null);enable(s);}
        get onmessageerror(){check(this);return handlerValue(this,'messageerror');}
        set onmessageerror(v){check(this);setHandler(this,'messageerror',v,null);}
    }
    class MessageChannel {
        constructor(){
            const a={peer:null,owner:null,head:null,tail:null,count:0,scheduled:false,scheduledRealm:null,broker:null};
            const b={peer:a,owner:null,head:null,tail:null,count:0,scheduled:false,scheduledRealm:null,broker:null};
            a.peer=b;a.owner=makePort(a);b.owner=makePort(b);
            put(channels,this,[a.owner.value,b.owner.value]);
        }
        get port1(){const c=get(channels,this);if(!c)throw new TypeErr('Illegal MessageChannel receiver');return c[0];}
        get port2(){const c=get(channels,this);if(!c)throw new TypeErr('Illegal MessageChannel receiver');return c[1];}
    }
    cloneData.registerTransfer({
        name:'MessagePort',
        brand:portBrand,
        validate(p){const s=check(p);if(s.detached||s.endpoint?.broker)fail();},
        prepare(p){return makePort(check(p).endpoint);},
        prepareExternal(p){return {endpoint:check(p).endpoint};},
        prepareWorker(p,child){
            const source=check(p),endpoint=source.endpoint,peer=endpoint?.peer;
            if(endpoint?.broker||!peer||peer.broker)throw new DomError('Only initial entangled Worker port transfers are supported','DataCloneError');
            const id=workerNative(4,child),key=workerKey(child,id,0),queued=[];
            for(let entry=endpoint.head;entry;entry=entry.next){const payload=get(readyPackets,entry.data);if(!payload||payload.ports.length)throw new DomError('Queued nested Port transfer is not supported','DataCloneError');appendWorker(queued,cloneData.prepare(payload,[],true).data);}
            const route={child,id,creator:0,key,endpoint:peer,active:false,closed:false};
            const receiver={value:null,endpoint,detached:false,enabled:false,realm,queue:{head:null,tail:null,count:0},exportRoute:route};
            const token={receiver,meta:[id,0,queued],route};apply(routeSet,workerRoutes,[key,route]);return token;
        },
        commitPlan(p,receiver){
            const source=check(p),endpoint=source.endpoint,queue=receiver.queue;
            const writes=[{object:source,key:'detached',value:true},{object:source,key:'endpoint',value:null},
                {object:endpoint,key:'scheduled',value:0},{object:endpoint,key:'owner',value:receiver}];
            if(queue){const keys=['head','tail','count'];for(let i=0;i<keys.length;i++){
                const key=keys[i];define(writes,writes.length,{value:{object:endpoint,key,value:queue[key]},writable:true,enumerable:true,configurable:true});
            }}
            define(writes,writes.length,{value:{object:receiver,key:'queue',value:null},writable:true,enumerable:true,configurable:true});
            if(receiver.exportRoute){const route=receiver.exportRoute;appendWorker(writes,{object:route,key:'active',value:true});appendWorker(writes,{object:endpoint,key:'broker',value:route});appendWorker(writes,{object:endpoint.peer,key:'broker',value:route});}
            return {writes,cancel:endpoint.scheduled?{generation:endpoint.scheduledRealm.generation,id:endpoint.scheduled}:null};
        },
        commit(p,prepared){
            const s=check(p),receiver=prepared,endpoint=s.endpoint;
            cancelScheduled(endpoint);
            s.detached=true;s.endpoint=null;endpoint.owner=receiver;
            if(receiver.queue){endpoint.head=receiver.queue.head;endpoint.tail=receiver.queue.tail;endpoint.count=receiver.queue.count;receiver.queue=null;}
        }
    });
    cloneData.registerUncloneable(v=>has(channels,v)||has(events,v));
    const broadcast=host.broadcast,nativeOrigin=host.origin;
    function broadcastCheck(value){const s=get(broadcastChannels,value);if(!s)throw new TypeErr('Illegal BroadcastChannel receiver');return s;}
    class BroadcastChannel extends EventTarget {
        constructor(name){
            super();if(!arguments.length)throw new TypeErr('Channel name required');
            const s={value:this,name:string(name),id:0,closed:false};put(broadcastChannels,this,s);
            s.id=broadcast(0,s.name,packet=>{
                if(s.closed)return;
                let event;
                try{event=new MessageEvent('message',{data:cloneData.deserialize(packet),origin:nativeOrigin()});}
                catch(_){event=new MessageEvent('messageerror',{origin:nativeOrigin()});}
                event.isTrusted=true;dispatch(s.value,event);
            });
        }
        get name(){return broadcastCheck(this).name;}
        postMessage(message){
            const s=broadcastCheck(this);if(!arguments.length)throw new TypeErr('Message required');
            if(s.closed)throw new DomError('Channel is closed','InvalidStateError');
            const packet=cloneData.prepare(message,[]);
            broadcast(2,s.id,packet.data);
        }
        close(){const s=broadcastCheck(this);if(!s.closed){broadcast(1,s.id);s.closed=true;}}
        get onmessage(){broadcastCheck(this);return handlerValue(this,'message');}
        set onmessage(v){broadcastCheck(this);setHandler(this,'message',v,null);}
        get onmessageerror(){broadcastCheck(this);return handlerValue(this,'messageerror');}
        set onmessageerror(v){broadcastCheck(this);setHandler(this,'messageerror',v,null);}
    }
    cloneData.registerUncloneable(v=>has(broadcastChannels,v));
    messageHandlerTarget=(target,type)=>(portBrand(target)||has(broadcastChannels,target))&&(type==='message'||type==='messageerror');
    function postMessage(message,targetOrOptions={},transfer){
        const receiver=this===undefined||this===null?globalThis:this;
        if(!host.frame('messageBrand',receiver))throw new TypeErr('Illegal Window receiver');
        if(!arguments.length)throw new TypeErr('Message required');
        let target='/',list;
        if(arguments.length>=3){target=string(targetOrOptions);list=transfer;}
        else if(targetOrOptions!==null&&(typeof targetOrOptions==='object'||typeof targetOrOptions==='function')){
            const o=targetOrOptions.targetOrigin;target=o===undefined?'/':string(o);list=targetOrOptions.transfer;
        }else if(targetOrOptions!==undefined&&targetOrOptions!==null){target=string(targetOrOptions);list=transfer;}
        let expected=target;
        if(target!=='/'&&target!=='*'){
            try{expected=apply(originGet,new URLType(target),[]);}
            catch(_){throw new DomError('Invalid target origin','SyntaxError');}
        }
        return host.frame('message',receiver,message,expected,cloneData.transferList(list));
    }
    function local(message,target,transfers){
        const senderOrigin=nativeOrigin(),expected=target==='/'?senderOrigin:target;let data;
        const reserved=postTask(()=>{
            // Opaque origins only match the same Window through the '/' default,
            // never an arbitrary URL whose serialized origin is also 'null'.
            if(target!=='*'&&(expected!==nativeOrigin()||(expected==='null'&&target!=='/')))return;
            deliver(globalThis,data,globalThis,senderOrigin);
        });
        try{const envelope=prepare(message,transfers),imported=importPacket(envelope.data,envelope);
            commitImported(imported);data=imported.packet;
        }catch(error){cancelPost(reserved);throw error;}
    }
    function prepareWorker(message,transfers,child){
        try{
        const transferred=[],seen=[];
        for(let i=0;i<transfers.length;i++)if(portBrand(transfers[i])){const endpoint=check(transfers[i]).endpoint;for(let j=0;j<seen.length;j++)if(seen[j]===endpoint||seen[j]===endpoint?.peer)throw new DomError('Both ends cannot be exported together','DataCloneError');appendWorker(seen,endpoint);appendWorker(transferred,transfers[i]);}
        const packet=cloneData.prepare({data:message,ports:transferred},transfers,'worker',child),receivers=[],metadata=[];
        for(let i=0;i<packet.tokens.length;i++){const token=packet.tokens[i];appendWorker(receivers,token?token.receiver:null);appendWorker(metadata,token?token.meta:null);}
        const plan=cloneData.commitPlan(packet,receivers);plan.generations=[realm.generation];cloneData.validate(packet);return {packet:[packet.data,metadata],plan,tokens:packet.tokens};
        }catch(error){apply(routeEach,workerRoutes,[(route,key)=>{if(!route.active)apply(routeDelete,workerRoutes,[key]);}]);throw error;}
    }
    function abortWorker(prepared){for(let i=0;i<prepared.tokens.length;i++){const token=prepared.tokens[i];if(token&&!token.route.active)apply(routeDelete,workerRoutes,[token.route.key]);}}
    function importWorker(wire,child){
        const metadata=wire[1],receivers=[],added=[];
        try{
        for(let i=0;i<metadata.length;i++){const m=metadata[i];if(m===null){appendWorker(receivers,null);continue;}
            const id=m[0],creator=m[1],key=workerKey(child,id,creator);if(!isInteger(id)||id<=0||id>4294967295||creator!==1||workerRoute(child,id,creator))throw new TypeErr('Invalid Worker port ownership');
            const endpoint={peer:null,owner:null,head:null,tail:null,count:0,scheduled:0,scheduledRealm:null,broker:null},owner=makePort(endpoint);
            const route={key,child,id,creator,endpoint,closed:false,active:true};endpoint.owner=owner;endpoint.broker=route;appendWorker(added,route);apply(routeSet,workerRoutes,[key,route]);
            for(let j=0;j<m[2].length;j++){const payload=cloneData.deserialize(m[2][j],null,true);if(payload.ports.length)throw new TypeErr('Nested Worker port queue unsupported');const packet={};put(readyPackets,packet,payload);const entry={data:packet,next:null};if(endpoint.tail)endpoint.tail.next=entry;else endpoint.head=entry;endpoint.tail=entry;endpoint.count++;}
            appendWorker(receivers,owner);
        }
        return cloneData.deserialize(wire[0],receivers,true);
        }catch(error){for(let i=0;i<added.length;i++){const route=added[i];route.closed=true;route.endpoint.head=route.endpoint.tail=null;route.endpoint.count=0;apply(routeDelete,workerRoutes,[route.key]);}throw error;}
    }
    function receiveWorkerPort(child,id,creator,data,closing=false){
        const route=workerRoute(child,id,creator);if(!route||!route.active||route.closed)return;const endpoint=route.endpoint;
        if(closing){route.closed=true;cancelScheduled(endpoint);endpoint.head=endpoint.tail=null;endpoint.count=0;return;}
        const payload=cloneData.deserialize(data,null,true);if(payload.ports.length)throw new TypeErr('Nested Worker port transfer unsupported');
        const packet={};put(readyPackets,packet,payload);const entry={data:packet,next:null};
        // Reserve before publishing a new head: OOM cannot create an un-wakeable queue.
        schedule(endpoint,true);if(endpoint.tail)endpoint.tail.next=entry;else endpoint.head=entry;endpoint.tail=entry;endpoint.count++;
    }
    function closeWorker(child){apply(routeEach,workerRoutes,[(route,key)=>{if(route.child!==child)return;route.closed=true;const endpoint=route.endpoint;cancelScheduled(endpoint);endpoint.head=endpoint.tail=null;endpoint.count=0;apply(routeDelete,workerRoutes,[key]);}]);}
    for(const [C,name] of [[MessagePort,'MessagePort'],[MessageChannel,'MessageChannel'],[MessageEvent,'MessageEvent'],[BroadcastChannel,'BroadcastChannel']])
        define(C.prototype,Symbol.toStringTag,{value:name,configurable:true});
    Object.assign(globalThis,{MessageChannel,MessagePort,MessageEvent,BroadcastChannel,postMessage});
    return {postMessage,local,prepare,importPacket,commitImported,receive:deliver,prepareWorker,abortWorker,importWorker,receiveWorkerPort,closeWorker};
})();
