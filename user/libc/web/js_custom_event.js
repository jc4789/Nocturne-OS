/* CustomEvent (DOM §2.4), one definition for the Window and worker realms over each realm's
   Event and its private event state. detail is a read-only attribute on the prototype; its
   value lives beside the event, never in an own property an author can replace. */
function customEventInterface(EventImpl,eventStateOf){
    const details=new WeakMap(),get=WeakMap.prototype.get,set=WeakMap.prototype.set,has=WeakMap.prototype.has;
    const call=Reflect.apply,initEvent=EventImpl.prototype.initEvent,TypeErrorImpl=TypeError;
    function detailOf(event){if(!call(has,details,[event]))throw new TypeErrorImpl('Illegal CustomEvent receiver');return call(get,details,[event]);}
    class CustomEvent extends EventImpl {
        constructor(type,init={}){
            super(...arguments);
            const detail=init===undefined||init===null?undefined:init.detail;
            call(set,details,[this,detail===undefined?null:detail]);
        }
        get detail(){return detailOf(this);}
        initCustomEvent(type,bubbles=false,cancelable=false,detail=null){
            detailOf(this);
            if(!arguments.length)throw new TypeErrorImpl('initCustomEvent requires type');
            if(eventStateOf(this).dispatching)return;
            call(initEvent,this,[type,bubbles,cancelable]);call(set,details,[this,detail]);
        }
    }
    return CustomEvent;
}
