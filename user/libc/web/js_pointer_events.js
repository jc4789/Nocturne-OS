/* The native desktop currently has one hover-capable mouse, not touch/pen
   devices. Pointer events share its real coordinates/buttons and DOM paths. */
const pointerData=new WeakMap();
const wheelData=new WeakMap();
const wheelFields=new Set(['deltaX','deltaY','deltaZ','deltaMode','momentum']);
class WheelEvent extends MouseEvent {
    constructor(type,init={}) {
        if(!arguments.length)throw new TypeError('WheelEvent requires type');
        init=init==null?{}:Object(init);
        const mouse={};for(const key of Object.keys(init))if(!wheelFields.has(key))mouse[key]=init[key];
        super(type,mouse);
        const data={};
        for(const key of ['deltaX','deltaY','deltaZ']){
            const value=init[key]===undefined?0:+init[key];
            if(!Number.isFinite(value))throw new TypeError('Invalid '+key);
            data[key]=value;
        }
        data.deltaMode=init.deltaMode===undefined?0:(+init.deltaMode)>>>0;
        data.momentum=!!init.momentum;wheelData.set(this,data);
    }
}
for(const key of wheelFields)Object.defineProperty(WheelEvent.prototype,key,{configurable:true,enumerable:true,get(){const data=wheelData.get(this);if(!data)throw new TypeError('Illegal WheelEvent receiver');return data[key];}});
for(const [key,value] of Object.entries({DOM_DELTA_PIXEL:0,DOM_DELTA_LINE:1,DOM_DELTA_PAGE:2}))
    for(const target of [WheelEvent,WheelEvent.prototype])Object.defineProperty(target,key,{enumerable:true,value});
Object.defineProperty(WheelEvent.prototype,Symbol.toStringTag,{configurable:true,value:'WheelEvent'});
const pointerFields=new Set(['pointerId','width','height','pressure','tangentialPressure','tiltX','tiltY','twist','pointerType','isPrimary']);
class PointerEvent extends MouseEvent {
    constructor(type,init={}) {
        init=init==null?{}:Object(init);
        const mouse={};for(const key of Object.keys(init))if(!pointerFields.has(key)&&key!=='x'&&key!=='y')mouse[key]=init[key];
        super(type,mouse);
        const number=(key,fallback)=>{const value=init[key]===undefined?fallback:Number(init[key]);if(!Number.isFinite(value))throw new TypeError('Invalid pointer '+key);return value;};
        pointerData.set(this,{
            pointerId:number('pointerId',0)>>0,width:number('width',1),height:number('height',1),
            pressure:number('pressure',0),tangentialPressure:number('tangentialPressure',0),
            tiltX:number('tiltX',0)>>0,tiltY:number('tiltY',0)>>0,twist:number('twist',0)>>0,
            pointerType:init.pointerType===undefined?'':String(init.pointerType),isPrimary:!!init.isPrimary
        });
    }
    getCoalescedEvents(){if(!pointerData.has(this))throw new TypeError('Illegal PointerEvent receiver');return [];}
    getPredictedEvents(){if(!pointerData.has(this))throw new TypeError('Illegal PointerEvent receiver');return [];}
}
for(const key of ['pointerId','width','height','pressure','tangentialPressure','tiltX','tiltY','twist','pointerType','isPrimary'])
    Object.defineProperty(PointerEvent.prototype,key,{configurable:true,enumerable:true,get(){const data=pointerData.get(this);if(!data)throw new TypeError('Illegal PointerEvent receiver');return data[key];}});
Object.defineProperties(PointerEvent.prototype,{x:{configurable:true,enumerable:true,get(){return this.clientX;}},y:{configurable:true,enumerable:true,get(){return this.clientY;}}});
for(const type of ['pointerover','pointerenter','pointerdown','pointermove','pointerup','pointercancel','pointerout','pointerleave','gotpointercapture','lostpointercapture'])globalHandlerTypes.add(type);
const mousePointerInit=init=>mouseAssign({},init,{pointerId:1,pointerType:'mouse',isPrimary:true,width:1,height:1,pressure:init.buttons ? .5 : 0});
let suppressCompatibilityMouse=false;
function nativeDispatch(target,type,init) {
    target=target===null?globalThis:target;
    if(init.isTrusted!==false && (type==='mousedown'||type==='mouseup')) {
        const pointer=new PointerEvent(type==='mousedown'?'pointerdown':'pointerup',mousePointerInit(init));
        pointer.composed=true;pointer.isTrusted=true;
        const allowed=dispatch(target,pointer);
        if(type==='mousedown')suppressCompatibilityMouse=!allowed;
        if(suppressCompatibilityMouse){if(type==='mouseup')suppressCompatibilityMouse=false;return true;}
    }
    const focus=/^(?:focus|blur|focusin|focusout)$/.test(type);
    const C=type==='submit'?formValidationBridge.SubmitEvent:focus?FocusEvent:type==='wheel'?WheelEvent:/^(key)/.test(type)?KeyboardEvent:/^pointer/.test(type)?PointerEvent:/^(mouse|click|dblclick)/.test(type)?MouseEvent:Event;
    const e=new C(type,focus?mouseAssign({},init,{view:globalThis}):init);
    for(const key of Object.keys(init))if(key!=='submitter'&&!(focus&&key==='relatedTarget')&&!(wheelData.has(e)&&wheelFields.has(key))&&!pointerData.has(e))e[key]=init[key];
    e.composed=/^(?:keydown|keyup|keypress|click|dblclick|mousedown|mouseup|mouseout|mousemove|mouseover|pointerdown|pointerup|pointermove|pointerout|pointerover|pointercancel|wheel|focus|blur|focusin|focusout|input)$/.test(type);
    e.isTrusted=init.isTrusted!==false;
    return dispatch(target,e);
}
