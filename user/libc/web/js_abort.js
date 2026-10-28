/* DOM "Aborting ongoing activities" (https://dom.spec.whatwg.org/#aborting-ongoing-activities):
   AbortSignal and AbortController, the one definition of a Window's (js_bootstrap.js) and a
   Worker's (js_worker_runtime.js) realm. The realm passes only what its events are:
     fire(signal)              fires a trusted event named abort at signal (DOM "fire an event"),
     handlers(prototype,types) installs its event handler IDL attributes (onabort),
     report(error)             reports an exception an abort algorithm threw,
     now(), timer(fn,ms)       its monotonic clock and a task after a timeout.
   Its EventTarget calls listen(target, delta) when an abort event listener of target is added (1)
   or removed (-1), and asks handlerTarget(target, type) whether target has the onabort handler.
   signals is what Fetch (js_fetch.js) and the realm's EventTarget (addEventListener's signal
   option) ask of this realm's signals. */
function abortInterfaces(realm) {
    const {fire,handlers,report,now,timer}=realm;
    const apply=Reflect.apply,define=Object.defineProperty,NativeTypeError=TypeError,NativeDOMException=DOMException;
    const weakGet=WeakMap.prototype.get,weakSet=WeakMap.prototype.set,weakHas=WeakMap.prototype.has;
    const NativeSet=Set,setAdd=Set.prototype.add,setDelete=Set.prototype.delete,setHas=Set.prototype.has,setValues=Set.prototype.values,setClear=Set.prototype.clear;
    const setSize=Object.getOwnPropertyDescriptor(Set.prototype,'size').get,setNext=Object.getPrototypeOf(apply(setValues,new NativeSet(),[])).next;
    const NativeWeakRef=WeakRef,deref=WeakRef.prototype.deref,NativeRegistry=FinalizationRegistry,
        register=FinalizationRegistry.prototype.register,unregister=FinalizationRegistry.prototype.unregister;
    const trunc=Math.trunc,finite=Number.isFinite,iteratorKey=Symbol.iterator;
    const slots=new WeakMap(),controllers=new WeakMap(),token={};
    /* The garbage collection rule's strong references: every non-aborted dependent signal with
       source signals and an abort listener or abort algorithm. */
    const retained=new NativeSet();
    const size=set=>apply(setSize,set,[]);
    /* A Set's members now, in order, without the page's Array or iterator methods. */
    function members(set){const out=[];const iterator=apply(setValues,set,[]);for(;;){const item=apply(setNext,iterator,[]);if(item.done)return out;define(out,out.length,{value:item.value,writable:true,enumerable:true,configurable:true});}}
    const state=(signal,label='AbortSignal')=>{const s=apply(weakGet,slots,[signal]);if(!s)throw new NativeTypeError('Illegal '+label+' receiver');return s;};
    const isSignal=value=>(typeof value==='object'||typeof value==='function')&&value!==null&&apply(weakHas,slots,[value]);
    function retain(signal,s){
        const keep=!s.aborted&&s.dependent&&size(s.sources)>0&&(size(s.algorithms)>0||s.listeners>0);
        apply(keep?setAdd:setDelete,retained,[signal]);
    }
    /* A collected dependent signal leaves its sources' dependent signals. */
    const finalizer=new NativeRegistry(list=>{for(let i=0;i<list.length;i++){const p=apply(weakGet,slots,[list[i][0]]);if(p)apply(setDelete,p.dependents,[list[i][1]]);}});
    /* An aborted signal no longer needs its sources, nor they it. */
    function release(signal,s){
        for(let i=0;i<s.dependencies.length;i++){const p=apply(weakGet,slots,[s.dependencies[i][0]]);if(p)apply(setDelete,p.dependents,[s.dependencies[i][1]]);}
        s.dependencies=[];apply(setClear,s.sources,[]);apply(unregister,finalizer,[signal]);apply(setDelete,retained,[signal]);
    }
    /* DOM "run the abort steps". */
    function runAbortSteps(signal){
        const s=state(signal),algorithms=members(s.algorithms);
        for(let i=0;i<algorithms.length;i++){try{algorithms[i](s.reason);}catch(error){try{report(error);}catch(_){}}}
        apply(setClear,s.algorithms,[]);release(signal,s);
        fire(signal);
    }
    /* DOM "signal abort". */
    function signalAbort(signal,reason){
        const s=state(signal);if(s.aborted)return;
        s.aborted=true;s.reason=reason===undefined?new NativeDOMException('signal is aborted without reason','AbortError'):reason;
        const toAbort=[],dependents=members(s.dependents);
        for(let i=0;i<dependents.length;i++){
            const dependent=apply(deref,dependents[i],[]);if(!dependent){apply(setDelete,s.dependents,[dependents[i]]);continue;}
            const d=state(dependent);
            if(!d.aborted){d.aborted=true;d.reason=s.reason;define(toAbort,toAbort.length,{value:dependent,writable:true,enumerable:true,configurable:true});}
        }
        apply(setClear,s.dependents,[]);
        runAbortSteps(signal);
        for(let i=0;i<toAbort.length;i++)runAbortSteps(toAbort[i]);
    }
    function newSignal(){return new AbortSignal(token);}
    /* DOM "create a dependent abort signal" from a list of this realm's AbortSignals: any()'s
       converted sequence, or a list the realm made itself (Fetch's Request signals). */
    function dependentSignal(sources){
        const result=newSignal(),r=state(result);
        for(let i=0;i<sources.length;i++){const p=state(sources[i]);if(p.aborted){r.aborted=true;r.reason=p.reason;return result;}}
        r.dependent=true;
        const reference=new NativeWeakRef(result);
        const follow=source=>{
            if(apply(setHas,r.sources,[source]))return;
            apply(setAdd,r.sources,[source]);apply(setAdd,state(source).dependents,[reference]);
            define(r.dependencies,r.dependencies.length,{value:[source,reference],writable:true,enumerable:true,configurable:true});
        };
        for(let i=0;i<sources.length;i++){
            const p=state(sources[i]);
            if(!p.dependent)follow(sources[i]);
            else{const roots=members(p.sources);for(let j=0;j<roots.length;j++)follow(roots[j]);}
        }
        if(r.dependencies.length)apply(register,finalizer,[result,r.dependencies,result]);
        return result;
    }
    /* Web IDL: an ECMAScript value to sequence<AbortSignal> (create a sequence from an iterable),
       each value converted as it is taken. */
    function signalSequence(value){
        if(value===null||(typeof value!=='object'&&typeof value!=='function'))throw new NativeTypeError('AbortSignal.any requires a sequence of AbortSignals');
        const method=value[iteratorKey];
        if(method===undefined||method===null||typeof method!=='function')throw new NativeTypeError('AbortSignal.any requires an iterable');
        const iterator=apply(method,value,[]);
        if(iterator===null||(typeof iterator!=='object'&&typeof iterator!=='function'))throw new NativeTypeError('The iterator is not an object');
        const next=iterator.next,out=[];
        for(;;){
            const item=apply(next,iterator,[]);
            if(item===null||(typeof item!=='object'&&typeof item!=='function'))throw new NativeTypeError('The iterator result is not an object');
            if(item.done)return out;
            const signal=item.value;
            if(!isSignal(signal))throw new NativeTypeError('AbortSignal.any: a member is not an AbortSignal');
            define(out,out.length,{value:signal,writable:true,enumerable:true,configurable:true});
        }
    }
    class AbortSignal extends EventTarget {
        constructor(secret){
            super();if(secret!==token)throw new NativeTypeError('Illegal constructor');
            apply(weakSet,slots,[this,{aborted:false,reason:undefined,dependent:false,algorithms:new NativeSet(),
                sources:new NativeSet(),dependents:new NativeSet(),dependencies:[],listeners:0}]);
        }
        static abort(reason=undefined){const signal=newSignal(),s=state(signal);s.aborted=true;s.reason=reason===undefined?new NativeDOMException('signal is aborted without reason','AbortError'):reason;return signal;}
        static timeout(milliseconds){
            if(!arguments.length)throw new NativeTypeError('AbortSignal.timeout requires milliseconds');
            // Web IDL [EnforceRange] unsigned long long: 0 to 2^53 - 1.
            const number=+milliseconds;if(!finite(number))throw new NativeTypeError('AbortSignal.timeout: milliseconds is not finite');
            const ms=trunc(number)+0;if(ms<0||ms>9007199254740991)throw new NativeTypeError('AbortSignal.timeout: milliseconds is outside unsigned long long');
            const signal=newSignal(),started=now();
            // The timer's own wait is a signed 32-bit delay: a longer timeout waits in steps.
            const elapsed=()=>{const left=ms-(now()-started);if(left>0)timer(elapsed,left<2147483647?left:2147483647);else signalAbort(signal,new NativeDOMException('signal timed out','TimeoutError'));};
            timer(elapsed,ms<2147483647?ms:2147483647);return signal;
        }
        static any(signals){
            if(!arguments.length)throw new NativeTypeError('AbortSignal.any requires a sequence of AbortSignals');
            return dependentSignal(signalSequence(signals));
        }
        get aborted(){return state(this).aborted;}
        get reason(){return state(this).reason;}
        throwIfAborted(){const s=state(this);if(s.aborted)throw s.reason;}
    }
    class AbortController {
        constructor(){apply(weakSet,controllers,[this,newSignal()]);}
        get signal(){const signal=apply(weakGet,controllers,[this]);if(!signal)throw new NativeTypeError('Illegal AbortController receiver');return signal;}
        abort(reason=undefined){const signal=apply(weakGet,controllers,[this]);if(!signal)throw new NativeTypeError('Illegal AbortController receiver');signalAbort(signal,reason);}
    }
    handlers(AbortSignal.prototype,['abort']);
    for(const [C,name]of [[AbortSignal,'AbortSignal'],[AbortController,'AbortController']]) {
        define(C.prototype,Symbol.toStringTag,{value:name,configurable:true});
        for(const key of Object.getOwnPropertyNames(C.prototype)){if(key==='constructor')continue;const d=Object.getOwnPropertyDescriptor(C.prototype,key);d.enumerable=true;define(C.prototype,key,d);}
        for(const key of Object.getOwnPropertyNames(C)){if(key==='length'||key==='name'||key==='prototype')continue;const d=Object.getOwnPropertyDescriptor(C,key);d.enumerable=true;define(C,key,d);}
    }
    /* DOM "add" an algorithm (none to an aborted signal) and "remove" it: follow returns its
       removal. */
    function follow(signal,algorithm){
        const s=state(signal);if(s.aborted)return ()=>{};
        apply(setAdd,s.algorithms,[algorithm]);retain(signal,s);
        return ()=>{apply(setDelete,s.algorithms,[algorithm]);retain(signal,s);};
    }
    return {AbortSignal,AbortController,
        handlerTarget:(target,type)=>type==='abort'&&isSignal(target),
        listen(target,delta){if(!isSignal(target))return;const s=state(target);s.listeners+=delta;if(s.listeners<0)s.listeners=0;retain(target,s);},
        signals:Object.freeze({
            is:isSignal,
            aborted:signal=>state(signal).aborted,
            reason:signal=>state(signal).reason,
            follow,
            any:dependentSignal,
            controller(){const signal=newSignal();return {signal,abort(reason){signalAbort(signal,reason);}};}
        })};
}
