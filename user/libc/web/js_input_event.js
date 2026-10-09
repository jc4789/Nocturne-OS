/* InputEvent for actual native text-control edits. StaticRange/DataTransfer
 * are not implemented, so reject their purported instances instead of
 * accepting a plain object and silently losing author data. */
const inputEventBridge=(()=>{
    const data=new WeakMap(),get=WeakMap.prototype.get,set=WeakMap.prototype.set;
    const define=Object.defineProperty,StringType=String,TypeErr=TypeError,ObjectType=Object,wellFormed=String.prototype.toWellFormed;
    function string(v){if(typeof v==='symbol')throw new TypeErr('Cannot convert Symbol to DOMString');return StringType(v);}
    function record(receiver){const r=apply(get,data,[receiver]);if(!r)throw new TypeErr('Illegal InputEvent receiver');return r;}
    class InputEvent extends UIEvent {
        constructor(type,init={}){
            if(!arguments.length)throw new TypeErr('InputEvent requires type');
            if(init!==null&&typeof init!=='object'&&typeof init!=='function')throw new TypeErr('Expected InputEventInit dictionary');
            init=init==null?{}:ObjectType(init);super(string(type),init);
            const text=init.data,kind=init.inputType,composing=!!init.isComposing;
            const transfer=init.dataTransfer;
            if(transfer!=null)throw new TypeErr('Expected a native DataTransfer');
            const ranges=init.targetRanges;
            if(ranges!==undefined){for(const range of ranges)throw new TypeErr('Expected a native StaticRange');}
            apply(set,data,[this,{text:text==null?null:apply(wellFormed,string(text),[]),kind:kind===undefined?'':string(kind),composing}]);
        }
        get data(){return record(this).text;}
        get inputType(){return record(this).kind;}
        get isComposing(){return record(this).composing;}
        get dataTransfer(){record(this);return null;}
        getTargetRanges(){record(this);return [];}
    }
    for(const name of ['data','inputType','isComposing','dataTransfer']){
        const d=Object.getOwnPropertyDescriptor(InputEvent.prototype,name);
        define(InputEvent.prototype,name,{...d,enumerable:true});
    }
    define(InputEvent.prototype,Symbol.toStringTag,{configurable:true,value:'InputEvent'});
    return {InputEvent};
})();
const InputEvent=inputEventBridge.InputEvent;
