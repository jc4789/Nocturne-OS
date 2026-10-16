/* PromiseRejectionEvent and HTML "notify about rejected promises", shared by
 * Window and Worker realms after Event and eventState exist. */
const PromiseRejectionEvent=(()=>{
    const data=new WeakMap(),get=WeakMap.prototype.get,set=WeakMap.prototype.set,call=Reflect.apply;
    function record(value){
        const result=call(get,data,[value]);
        if(!result)throw new TypeError('Illegal PromiseRejectionEvent receiver');
        return result;
    }
    class PromiseRejectionEvent extends Event {
        constructor(type,init){
            if(arguments.length<2)throw new TypeError('PromiseRejectionEvent requires a type and an init dictionary');
            if(typeof type==='symbol')throw new TypeError('Cannot convert Symbol to DOMString');
            const t=String(type);
            if(init!=null && typeof init!=='object' && typeof init!=='function')throw new TypeError('Expected event dictionary');
            const source=init==null?{}:init;
            // Dictionary members in Web IDL (lexicographic) order.
            const base={bubbles:!!source.bubbles,cancelable:!!source.cancelable,composed:!!source.composed};
            const promise=source.promise;
            if(promise===undefined)throw new TypeError("PromiseRejectionEventInit requires 'promise'");
            if(promise===null || (typeof promise!=='object' && typeof promise!=='function'))
                throw new TypeError("PromiseRejectionEventInit 'promise' must be an object");
            const reason=source.reason;
            super(t,base);call(set,data,[this,{promise,reason}]);
        }
        get promise(){return record(this).promise;}
        get reason(){return record(this).reason;}
    }
    Object.defineProperty(PromiseRejectionEvent.prototype,Symbol.toStringTag,{configurable:true,value:'PromiseRejectionEvent'});
    for(const key of ['promise','reason'])
        Object.defineProperty(PromiseRejectionEvent.prototype,key,{...Object.getOwnPropertyDescriptor(PromiseRejectionEvent.prototype,key),enumerable:true});
    return PromiseRejectionEvent;
})();
/* Native code hands over each rejection still unhandled after the microtask
   checkpoint; a promise that stays unhandled after its unhandledrejection
   dispatch joins the weak outstanding set, and only those fire
   rejectionhandled when handled. fire(event) dispatches at the global scope;
   defer(fn) queues a task. */
function promiseRejectionBridge(fire,isHandled,defer){
    const outstanding=new WeakSet(),has=WeakSet.prototype.has,add=WeakSet.prototype.add,remove=WeakSet.prototype.delete;
    const call=Reflect.apply;
    function notify(type,promise,reason,cancelable){
        const event=new PromiseRejectionEvent(type,{__proto__:null,cancelable,promise,reason});
        eventState(event).isTrusted=true;
        return fire(event);
    }
    // Returns notCanceled: whether the console still reports the reason.
    function unhandled(promise,reason){
        const report=notify('unhandledrejection',promise,reason,true);
        if(!isHandled(promise))call(add,outstanding,[promise]);
        return report;
    }
    function handled(promise,reason){
        if(!call(has,outstanding,[promise]))return;
        call(remove,outstanding,[promise]);
        defer(()=>{notify('rejectionhandled',promise,reason,false);});
    }
    return {unhandled,handled};
}
