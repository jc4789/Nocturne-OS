/* Cooperative scheduling. Include after js_clone.js. The native idle-task
 * source supplies the real monotonic deadline; this is not a timer shim. */
const idleBridge = (() => {
    const call=Reflect.apply,define=Object.defineProperty,create=Object.create;
    const TypeErr=TypeError,now=host.now,request=host.idle,cancel=host.cancelIdle,window=globalThis;
    const deadlines=new WeakMap(),getWeak=WeakMap.prototype.get,setWeak=WeakMap.prototype.set;
    const get=value=>call(getWeak,deadlines,[value]);
    cloneData.registerUncloneable(value=>get(value)!==undefined);
    function slot(value){const s=get(value);if(s===undefined)throw new TypeErr('Illegal invocation');return s;}
    class IdleDeadline {
        constructor(){throw new TypeErr('Illegal constructor');}
        timeRemaining(){
            const s=slot(this),remaining=s.end-now();
            return remaining>0?remaining:0;
        }
        get didTimeout(){return slot(this).timedOut;}
    }
    define(IdleDeadline.prototype,Symbol.toStringTag,{value:'IdleDeadline',configurable:true});
    for(const key of ['timeRemaining','didTimeout']){
        const d=Object.getOwnPropertyDescriptor(IdleDeadline.prototype,key);
        d.enumerable=true;define(IdleDeadline.prototype,key,d);
    }
    function windowReceiver(value){if(value!=null&&value!==window)throw new TypeErr('Illegal invocation');}
    const windowMethods={requestIdleCallback(callback,options={}){
        windowReceiver(this);
        if(typeof callback!=='function')throw new TypeErr('An idle callback is required');
        if(options!=null&&typeof options!=='object'&&typeof options!=='function')throw new TypeErr('Expected a dictionary');
        const value=options==null?undefined:options.timeout;
        // Web IDL unsigned long conversion: unary + rejects BigInt/Symbol,
        // then ToUint32 truncates/modulos without reading the member twice.
        const timeout=value===undefined?0:(+value)>>>0;
        return request(function(end,timedOut){
            const deadline=create(IdleDeadline.prototype);
            call(setWeak,deadlines,[deadline,{end,timedOut}]);
            return call(callback,undefined,[deadline]);
        },timeout);
    },cancelIdleCallback(handle){
        windowReceiver(this);
        if(!arguments.length)throw new TypeErr('An idle callback handle is required');
        cancel((+handle)>>>0);
    }};
    Object.assign(window,{IdleDeadline,requestIdleCallback:windowMethods.requestIdleCallback,cancelIdleCallback:windowMethods.cancelIdleCallback});
    return {};
})();
