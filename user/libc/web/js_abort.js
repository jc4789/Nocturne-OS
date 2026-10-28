/* DOM "Aborting ongoing activities" (https://dom.spec.whatwg.org/#aborting-ongoing-activities):
   a Window's AbortSignal and AbortController. abortBridge.signals is what Fetch (js_fetch.js)
   asks of this realm's signals: the brand, the state, following one (an abort algorithm), a
   dependent signal and a controller of its own. The Worker realm has its own AbortSignal
   (js_worker_runtime.js's workerAbortBridge) behind the same five operations. Included after
   DOMException and the bootstrap's EventTarget, installHandlers and dispatch. */
const abortBridge = (() => {
    const signalSlots=new WeakMap(), controllerSlots=new WeakMap();
    const apply=Reflect.apply, define=Object.defineProperty, NativeArray=Array, arrayFrom=Array.from;
    const NativeWeakRef=WeakRef, deref=WeakRef.prototype.deref;
    const weakGet=WeakMap.prototype.get, weakSet=WeakMap.prototype.set, weakHas=WeakMap.prototype.has;
    const setAdd=Set.prototype.add, setDelete=Set.prototype.delete, setValues=Set.prototype.values, setClear=Set.prototype.clear, NativeSet=Set;
    const setNext=Object.getPrototypeOf(apply(setValues,new NativeSet(),[])).next;
    for(const map of [signalSlots,controllerSlots]) {
        define(map,'get',{value:key=>apply(weakGet,map,[key])});
        define(map,'set',{value:(key,value)=>apply(weakSet,map,[key,value])});
        define(map,'has',{value:key=>apply(weakHas,map,[key])});
    }
    const slot=(map,v,label)=>{const s=map.get(v);if(!s)throw new TypeError('Illegal '+label+' receiver');return s;};
    /* A Set's members now, in order, without the page's Array or iterator methods. */
    function members(set){const out=[];const iterator=apply(setValues,set,[]);for(;;){const item=apply(setNext,iterator,[]);if(item.done)return out;define(out,out.length,{value:item.value,writable:true,enumerable:true,configurable:true});}}
    const dependentFinalizer=new FinalizationRegistry(cleanups=>{for(let i=0;i<cleanups.length;i++)apply(setDelete,cleanups[i][0].algorithms,[cleanups[i][1]]);});
    const signalToken={};
    function newSignal(){return new AbortSignal(signalToken);}
    function abortSignal(signal,reason) {
        const s=slot(signalSlots,signal,'AbortSignal');if(s.aborted)return;
        s.aborted=true;s.reason=reason===undefined?new DOMException('The operation was aborted','AbortError'):reason;
        s.sources=null;
        const algorithms=members(s.algorithms);apply(setClear,s.algorithms,[]);
        for(let i=0;i<algorithms.length;i++){try{algorithms[i](s.reason);}catch(e){report(e);}}
        const e=new Event('abort');eventState(e).isTrusted=true;dispatch(signal,e);
    }
    /* sources: an array of this realm's AbortSignals the caller made itself. */
    function dependentSignal(sources) {
        const signal=newSignal(),out=signalSlots.get(signal),cleanups=[],reference=new NativeWeakRef(signal);
        // A live dependent signal keeps its intermediate source signals alive.
        // Otherwise any([...]) temporaries could disappear before fetch aborts.
        const kept=[];for(let i=0;i<sources.length;i++)define(kept,i,{value:sources[i],writable:true,enumerable:true,configurable:true});
        out.sources=kept;
        const cleanup=()=>{for(let i=0;i<cleanups.length;i++)apply(setDelete,cleanups[i][0].algorithms,[cleanups[i][1]]);cleanups.length=0;};
        apply(setAdd,out.algorithms,[cleanup]);
        for(let i=0;i<kept.length;i++) {
            const s=slot(signalSlots,kept[i],'AbortSignal');
            if(s.aborted){abortSignal(signal,s.reason);break;}
            const fn=reason=>{const target=apply(deref,reference,[]);if(target)abortSignal(target,reason);else cleanup();};
            apply(setAdd,s.algorithms,[fn]);define(cleanups,cleanups.length,{value:[s,fn],writable:true,enumerable:true,configurable:true});
        }
        if(cleanups.length)dependentFinalizer.register(signal,cleanups);
        return signal;
    }
    class AbortSignal extends EventTarget {
        constructor(t){super();if(t!==signalToken)throw new TypeError('Illegal constructor');signalSlots.set(this,{aborted:false,reason:undefined,algorithms:new NativeSet()});}
        get aborted(){return slot(signalSlots,this,'AbortSignal').aborted;}
        get reason(){return slot(signalSlots,this,'AbortSignal').reason;}
        throwIfAborted(){const s=slot(signalSlots,this,'AbortSignal');if(s.aborted)throw s.reason;}
        static abort(reason){const signal=newSignal();abortSignal(signal,reason);return signal;}
        static timeout(ms) {
            if(!arguments.length)throw new TypeError('timeout requires milliseconds');
            ms=+ms;if(!Number.isFinite(ms))throw new TypeError('Invalid timeout');
            ms=Math.trunc(ms);if(ms<0||ms>=18446744073709551616)throw new TypeError('Invalid timeout');
            // AbortSignal takes unsigned long long, not setTimeout's signed
            // long. Keep its complete duration and split only the native wait.
            const signal=newSignal(),started=host.now();
            const elapsed=()=>{
                const remaining=ms-(host.now()-started);
                if(remaining>0)host.timer(0,elapsed,Math.min(remaining,2147483647),[]);
                else abortSignal(signal,new DOMException('Timed out','TimeoutError'));
            };
            host.timer(0,elapsed,Math.min(ms,2147483647),[]);return signal;
        }
        static any(signals) {
            if(!arguments.length)throw new TypeError('any requires signals');
            if(signals===null||(typeof signals!=='object'&&typeof signals!=='function'))throw new TypeError('Expected signal sequence');
            if(typeof signals[Symbol.iterator]!=='function')throw new TypeError('Expected iterable signal sequence');
            const sources=apply(arrayFrom,NativeArray,[signals]);for(let i=0;i<sources.length;i++)slot(signalSlots,sources[i],'AbortSignal');return dependentSignal(sources);
        }
    }
    class AbortController {
        constructor(){controllerSlots.set(this,newSignal());}
        get signal(){return slot(controllerSlots,this,'AbortController');}
        abort(reason){abortSignal(slot(controllerSlots,this,'AbortController'),reason);}
    }
    abortHandlerTarget=(target,type)=>signalSlots.has(target)&&type==='abort';
    // addEventListener's signal option: the listener's removal is one of the signal's abort
    // algorithms, which run before its abort event (DOM "signal abort").
    abortSignals={
        is:value=>apply(mapGet,signalSlots,[value])!==undefined,
        aborted:signal=>apply(mapGet,signalSlots,[signal]).aborted,
        follow(signal,steps){
            const s=apply(mapGet,signalSlots,[signal]);apply(setAdd,s.algorithms,[steps]);
            return ()=>apply(setDelete,s.algorithms,[steps]);
        }
    };
    installHandlers(AbortSignal.prototype,['abort']);
    for(const [C,name]of [[AbortSignal,'AbortSignal'],[AbortController,'AbortController']]) {
        define(C.prototype,Symbol.toStringTag,{value:name,configurable:true});
        for(const key of Object.getOwnPropertyNames(C.prototype)){if(key==='constructor')continue;const d=Object.getOwnPropertyDescriptor(C.prototype,key);d.enumerable=true;define(C.prototype,key,d);}
    }
    return {AbortSignal,AbortController,
        signals:Object.freeze({
            is:value=>signalSlots.has(value),
            aborted:signal=>slot(signalSlots,signal,'AbortSignal').aborted,
            reason:signal=>slot(signalSlots,signal,'AbortSignal').reason,
            /* steps runs as one of signal's abort algorithms; the result stops following it. */
            follow(signal,steps){const s=slot(signalSlots,signal,'AbortSignal');apply(setAdd,s.algorithms,[steps]);return ()=>apply(setDelete,s.algorithms,[steps]);},
            any:dependentSignal,
            controller(){const signal=newSignal();return {signal,abort(reason){abortSignal(signal,reason);}};}
        })};
})();
const {AbortSignal,AbortController}=abortBridge;
