/* Execute exact Event/EventTarget/dispatch product source. The path stub only
 * supplies a two-target tree (a target marked shadow is in a shadow tree), and
 * the host stub one realm; frames, native DOM and real-site acceptance are separate. */
const fs=require('node:fs'),assert=require('node:assert/strict');
const web='user/libc/web/',read=name=>fs.readFileSync(web+name,'utf8');
const source=read('js_bootstrap.js');
function slice(start,end){const a=source.indexOf(start),b=source.indexOf(end,a);assert(a>=0&&b>a,'marker '+start);return source.slice(a,b);}
const capturesSource=slice('    // Dispatch reads the listener lists','    const listenerMap = new WeakMap()');
const currentSource=slice("    // This Window's current event",'    let errorReporting=false;');
const eventSource=slice('    /* Event state: one table','    /* @include js_error_event.js */');
const uiSource=slice('    class UIEvent extends Event {','    /* The native side counts').replace('    /* @include js_input_event.js */','');
const listenerSource=slice('    /* The native side counts','    function resetDocumentEvents(');
const targetSource=slice('    class EventTarget {','    function handlerTarget(');
const handlerSource=slice('    /* HTML "activate an event handler"','    function handlerRecord(');
const dispatchSource=slice('    /* DOM "inner invoke"','    class Node extends EventTarget {');
const init=new Function(`
    const apply=Reflect.apply,eventSlice=Array.prototype.slice,mouseAssign=Object.assign;
    ${capturesSource}
    const listenerMap=new WeakMap(),inlineMap=new WeakMap(),inputWindow=globalThis;
    const reports=[],inactive=new Set();
    // No target here has activation behavior (tests/js_activation_hosttest.js covers activation).
    const host={now:()=>1,scroll:()=>0,pointerCapturePending:()=>{},inputListener:()=>false,storageListener:()=>{},activation:()=>false,
        eventRealmInit(port,table){host.port=port;return table;},eventRealm(){return host.port;},
        eventActive(target){return !inactive.has(target);},reportException(e){reports.push({error:e,current:currentEvent});}};
    function report(e){host.reportException(e);}
    ${currentSource}
    const document={},globalHandlerTypes=new Set();
    function rawDom(op,target,key){if(key==='elementBrand'){if(!(target instanceof Element))throw new TypeError('Element required');return true;}return key==='isConnected'?true:key==='ownerDocument'?document:undefined;}
    rawDom.isNode=()=>false;
    const handlerTarget=()=>false,handlerRecord=()=>null,handlerValues=new Map();
    function handlerValue(target,type){return handlerValues.get(type)??null;}
    const shadowBridge={path(target,event){const nodes=[target];if(target.parent)nodes.push(target.parent);
        return {entries:nodes.map(node=>({node,target,related:null,visible:nodes,atTarget:node===target,scopes:node.shadow?[{}]:[]})),finalTarget:target,finalRelated:null};}};
    ${eventSource}
    ${read('js_error_event.js')}
    ${uiSource}${listenerSource}${targetSource}${handlerSource}${dispatchSource}
    class Node extends EventTarget {}
    class Element extends Node {}
    const formValidationBridge={SubmitEvent:Event};
    ${read('js_pointer_events.js')}
    return {Event,ErrorEvent,EventTarget,Element,state:eventState,native:nativeDispatch,pointer:pointerCaptureBridge,
        current:()=>currentEvent,reports,inactive,handlerValues,activateHandler,dispatch,
        setAbortSignals(bridge){abortSignals=bridge;}};
`);
const product=init(),{Event,ErrorEvent,EventTarget,Element,state,native:nativeMouse,pointer,reports}=product;let checks=0;
function ok(value,label){checks++;assert(value,label);}
const parent=new EventTarget(),target=new EventTarget();target.parent=parent;
const order=[];
target.addEventListener('click',e=>{order.push('first');Object.defineProperty(e,'type',{value:'other'});Object.defineProperty(e,'bubbles',{value:false});e._stop=true;e._immediate=true;});
target.addEventListener('click',()=>order.push('second'));parent.addEventListener('click',()=>order.push('parent'));
const event=new Event('click',{bubbles:true});ok(target.dispatchEvent(event),'dispatch allowed');ok(order.join(',')==='first,second,parent','author fields cannot change private type/bubbles/stop');
const untrusted=new Event('probe');ok(!Reflect.set(untrusted,'isTrusted',true),'trust readonly');ok(!Object.getOwnPropertyDescriptor(untrusted,'isTrusted').configurable,'trust unforgeable');
ok(!Reflect.set(untrusted,'type','changed')&&untrusted.type==='probe','type readonly');
Object.defineProperty(untrusted,'defaultPrevented',{value:true});Object.defineProperty(untrusted,'cancelable',{value:true});untrusted.preventDefault();ok(target.dispatchEvent(untrusted),'public canceled flag cannot forge cancellation');
let trustedSeen;state(untrusted).isTrusted=true;target.addEventListener('probe',e=>{trustedSeen=e.isTrusted;});target.dispatchEvent(untrusted);ok(trustedSeen===false,'script dispatch clears native trust');
const stopped=new Event('before');let calls=0;target.addEventListener('before',()=>calls++);stopped.stopImmediatePropagation();target.dispatchEvent(stopped);ok(calls===0&&!stopped.cancelBubble,'preexisting stop respected and cleared after dispatch');
target.dispatchEvent(stopped);ok(calls===1,'reused event no stale stop');
const cancel=new Event('cancel',{cancelable:true});target.addEventListener('cancel',e=>e.preventDefault());ok(!target.dispatchEvent(cancel)&&cancel.defaultPrevented,'real cancellation preserved');
const passive=new Event('passive',{cancelable:true});target.addEventListener('passive',e=>e.preventDefault(),{passive:true});ok(target.dispatchEvent(passive)&&!passive.defaultPrevented,'passive cancellation blocked');
let nestedThrew=false;const nested=new Event('nested');target.addEventListener('nested',e=>{e._dispatching=false;try{target.dispatchEvent(e);}catch(error){nestedThrew=error.name==='InvalidStateError';}});target.dispatchEvent(nested);ok(nestedThrew,'redispatch while dispatching throws');
const old=new Event('old');state(old).initialized=false;assert.throws(()=>target.dispatchEvent(old),{name:'InvalidStateError'});checks++;old.initEvent('new',true,true);ok(old.type==='new'&&old.bubbles&&old.cancelable,'legacy initialization preserved');
ok(reports.length===0,'no listener threw so far');

// window.event: inner invoke steps 9, 10 and 15, in the listener's realm (one realm here).
const current=product.current,windowTarget=new EventTarget(),outer=new Event('outer'),inner=new Event('inner');
const seen=[];
windowTarget.addEventListener('inner',e=>seen.push(['inner',current()===e]));
windowTarget.addEventListener('outer',e=>{seen.push(['outer',current()===e]);windowTarget.dispatchEvent(inner);seen.push(['after inner',current()===e]);});
ok(current()===undefined,'no current event before dispatch');windowTarget.dispatchEvent(outer);
ok(current()===undefined,'current event restored after dispatch');
ok(JSON.stringify(seen)==='[["outer",true],["inner",true],["after inner",true]]','nested dispatch sets and restores the current event');
const shadowNode=new EventTarget();shadowNode.shadow=true;shadowNode.parent=windowTarget;let inShadow,atHost;
shadowNode.addEventListener('composed',()=>{inShadow=current();});windowTarget.addEventListener('composed',e=>{atHost=current()===e;});
const outerShadow=new Event('outer');windowTarget.addEventListener('outer',()=>{if(current()===outerShadow)shadowNode.dispatchEvent(new Event('composed',{bubbles:true}));});
windowTarget.dispatchEvent(outerShadow);
ok(inShadow===outerShadow,'a listener in a shadow tree leaves the current event as it was');ok(atHost===true,'outside the shadow tree the current event is set');
const throwing=new EventTarget(),thrown=new Event('throw');let afterThrow;
throwing.addEventListener('throw',()=>{throw new RangeError('listener failure');});
throwing.addEventListener('throw',e=>{afterThrow=current()===e;});
throwing.dispatchEvent(thrown);
ok(reports.length===1 && reports[0].error instanceof RangeError,'what a listener throws is reported');
ok(reports[0].current===thrown,'the exception is reported while the current event is still set');
ok(afterThrow===true && current()===undefined,'the next listener runs and the current event is restored');
ok(state(thrown).listenerException===true,'legacyOutputDidListenersThrowFlag set');

// Web IDL "call a user object's operation": handleEvent is got at dispatch, with the current event set.
const objectTarget=new EventTarget(),listenerObject={};let lookupEvent,thisValue;
objectTarget.addEventListener('object',listenerObject);
Object.defineProperty(listenerObject,'handleEvent',{get(){lookupEvent=current();return function(){thisValue=this;};}});
const objectEvent=new Event('object');objectTarget.dispatchEvent(objectEvent);
ok(lookupEvent===objectEvent,'handleEvent is looked up after the current event is set');ok(thisValue===listenerObject,'handleEvent is called with the object as this');
const missing=new EventTarget();missing.addEventListener('missing',{handleEvent:1});missing.dispatchEvent(new Event('missing'));
ok(reports.length===2 && reports[1].error instanceof TypeError,'a non-callable handleEvent reports a TypeError');
let functionThis;const functionTarget=new EventTarget();functionTarget.addEventListener('this',function(){functionThis=this;});functionTarget.dispatchEvent(new Event('this'));
ok(functionThis===functionTarget,'a function listener is called with the currentTarget as this');

// addEventListener's arguments: callback type, and the (options or boolean) union.
assert.throws(()=>new EventTarget().addEventListener('x',1),TypeError);checks++;
let optionReads=[];const options={get capture(){optionReads.push('capture');return false;},get once(){optionReads.push('once');return false;},
    get passive(){optionReads.push('passive');return undefined;},get signal(){optionReads.push('signal');return undefined;}};
new EventTarget().addEventListener('x',null,options);ok(optionReads.join()==='capture,once,passive,signal','options are read in Web IDL order even for a null callback');
assert.throws(()=>new EventTarget().addEventListener('x',()=>{}, {signal:null}),TypeError);checks++;
let captureCalls=0;const numberTarget=new EventTarget(),numberListener=()=>captureCalls++;numberTarget.addEventListener('n',numberListener,1);
numberTarget.removeEventListener('n',numberListener,true);numberTarget.dispatchEvent(new Event('n'));ok(captureCalls===0,'a number option is the boolean capture');

// The signal option: removal is one of the signal's abort algorithms.
const signals=new Map();product.setAbortSignals({is:v=>signals.has(v),aborted:v=>signals.get(v).aborted,
    follow(v,steps){const s=signals.get(v);s.algorithms.add(steps);return ()=>s.algorithms.delete(steps);}});
const signal={},signalState={aborted:false,algorithms:new Set()};signals.set(signal,signalState);
let signalCalls=0;const signalTarget=new EventTarget(),signalListener=()=>signalCalls++;
signalTarget.addEventListener('s',signalListener,{signal});ok(signalState.algorithms.size===1,'the listener follows the signal');
signalTarget.dispatchEvent(new Event('s'));for(const steps of [...signalState.algorithms])steps();signalState.aborted=true;
signalTarget.dispatchEvent(new Event('s'));ok(signalCalls===1,'the abort algorithm removes the listener');
signalTarget.addEventListener('s',signalListener,{signal});signalTarget.dispatchEvent(new Event('s'));ok(signalCalls===1,'an aborted signal adds nothing');
const kept={},keptState={aborted:false,algorithms:new Set()};signals.set(kept,keptState);signalTarget.addEventListener('k',signalListener,{signal:kept});
signalTarget.removeEventListener('k',signalListener);ok(keptState.algorithms.size===0,'removing the listener stops following the signal');

// DOM "invoke" step 10: legacy event types for trusted events without a listener of their type.
const legacyTarget=new EventTarget();let legacyType=null;legacyTarget.addEventListener('webkitAnimationEnd',e=>{legacyType=e.type;});
const trustedEnd=new Event('animationend');state(trustedEnd).isTrusted=true;product.dispatch(legacyTarget,trustedEnd);
ok(legacyType==='webkitAnimationEnd' && trustedEnd.type==='animationend','a trusted animationend reaches webkitAnimationEnd listeners, then gets its type back');
legacyType=null;legacyTarget.dispatchEvent(new Event('animationend'));ok(legacyType===null,'an untrusted event does not');
legacyTarget.addEventListener('animationend',()=>{});const foundEnd=new Event('animationend');state(foundEnd).isTrusted=true;product.dispatch(legacyTarget,foundEnd);
ok(legacyType===null,'a listener of the type itself means found');

// Inner invoke step 8: a target whose global is not fully active runs no listener.
const inactiveTarget=new EventTarget();let inactiveCalls=0;inactiveTarget.addEventListener('i',()=>inactiveCalls++);
product.inactive.add(inactiveTarget);inactiveTarget.dispatchEvent(new Event('i'));product.inactive.delete(inactiveTarget);inactiveTarget.dispatchEvent(new Event('i'));
ok(inactiveCalls===1,'listeners are skipped while the target is not fully active');

// HTML "the event handler processing algorithm".
const handlerTarget=new EventTarget(),record={value:null,text:null,compiled:true,entry:null};let handlerThis;
product.handlerValues.set('h',function(e){handlerThis=this;return false;});product.activateHandler(handlerTarget,'h',record);
const handled=new Event('h',{cancelable:true});ok(!handlerTarget.dispatchEvent(handled)&&handlerThis===handlerTarget,'a handler returning false cancels, called with the currentTarget as this');
ok(handlerTarget.dispatchEvent(new Event('h')),'returning false does not cancel a non-cancelable event');
product.handlerValues.set('h',{});ok(handlerTarget.dispatchEvent(new Event('h',{cancelable:true})),'a non-callable handler object is not invoked');
const errorRecord={value:null,text:null,compiled:true,entry:null},failure=new Error('boom');let errorArgs;
product.handlerValues.set('error',function(...args){errorArgs=args;return true;});product.activateHandler(globalThis,'error',errorRecord);
const errorEvent=new ErrorEvent('error',{message:'m',filename:'f',lineno:3,colno:4,error:failure,cancelable:true});
ok(!product.dispatch(globalThis,errorEvent),'a global error handler returning true cancels');
ok(errorArgs.length===5 && errorArgs[0]==='m' && errorArgs[1]==='f' && errorArgs[2]===3 && errorArgs[3]===4 && errorArgs[4]===failure,'special error event handling passes the five values');
product.handlerValues.set('error',()=>{throw new SyntaxError('handler failure');});const errorReports=reports.length;
product.dispatch(globalThis,new ErrorEvent('error',{message:'again'}));
ok(reports.length===errorReports+1 && reports.at(-1).error instanceof SyntaxError,'what a handler throws propagates to inner invoke, which reports it');

// Built-ins a page can replace do not change dispatch.
const savedSlice=Array.prototype.slice,savedGet=WeakMap.prototype.get,savedCall=Function.prototype.call;
const builtinTarget=new EventTarget();let builtinCalls=0;builtinTarget.addEventListener('b',()=>builtinCalls++);
Array.prototype.slice=()=>[];WeakMap.prototype.get=()=>undefined;Function.prototype.call=()=>{};
try{builtinTarget.dispatchEvent(new Event('b'));}finally{Array.prototype.slice=savedSlice;WeakMap.prototype.get=savedGet;Function.prototype.call=savedCall;}
ok(builtinCalls===1,'replaced slice, WeakMap get and Function call leave dispatch as it was');
ok(reports.length===errorReports+1,'no unexpected reports');
console.log('event internal state host: '+checks+' checks, 0 failed');

let pointerChecks=0;
function pok(value,label){pointerChecks++;assert(value,label);}
const control=new Element(),down={bubbles:true,cancelable:true,buttons:1,button:0},up={...down,buttons:0};
let pointerDown=0,mouseDown=0,mouseUp=0,clicks=0;
const cancelPointer=e=>{pointerDown++;e.preventDefault();};
control.addEventListener('pointerdown',cancelPointer);
control.addEventListener('mousedown',()=>mouseDown++);
control.addEventListener('mouseup',()=>mouseUp++);
let clickTrusted=true;control.addEventListener('click',e=>{clicks++;clickTrusted=clickTrusted&&e.isTrusted;});
pok(!nativeMouse(control,'mousedown',down),'canceled pointerdown blocks mouse focus default');
pok(pointerDown===1&&mouseDown===0,'canceled pointerdown suppresses compatibility mousedown');
pok(nativeMouse(control,'mouseup',up)&&mouseUp===0,'suppression preserves native release without mouseup');
pok(nativeMouse(control,'click',up)&&clicks===1&&clickTrusted,'pointerdown cancellation does not cancel independent trusted click');
control.removeEventListener('pointerdown',cancelPointer);
pok(nativeMouse(control,'mousedown',down)&&mouseDown===1,'next primary press resets compatibility suppression');
pok(nativeMouse(control,'mouseup',up)&&mouseUp===1,'next release delivers compatibility mouseup');
const cancelMouse=e=>e.preventDefault();control.addEventListener('mousedown',cancelMouse);
pok(!nativeMouse(control,'mousedown',down)&&mouseDown===2,'canceled mousedown blocks focus default');
nativeMouse(control,'mouseup',up);control.removeEventListener('mousedown',cancelMouse);
control.addEventListener('pointerdown',cancelPointer,{passive:true});
pok(nativeMouse(control,'mousedown',down)&&mouseDown===3,'passive pointer listener cannot suppress focus default');
nativeMouse(control,'mouseup',up);control.removeEventListener('pointerdown',cancelPointer);
const oldPointerDown=pointerDown;
pok(nativeMouse(control,'mousedown',{...down,isTrusted:false})&&mouseDown===4,'synthetic compatibility event does not activate native pointer');
pok(pointerDown===oldPointerDown,'synthetic event does not dispatch pointerdown');
pointer.reset();
console.log('native pointer cancellation host: '+pointerChecks+' checks, 0 failed');

const workerSource=read('js_worker_runtime.js');
const workerStart=workerSource.indexOf('    const eventStates='),workerEnd=workerSource.indexOf('    Object.assign(globalThis,{DOMException,QuotaExceededError',workerStart);
assert(workerStart>=0&&workerEnd>workerStart);
const workerEvents=workerSource.slice(workerStart,workerEnd)
    .replace('    /* @include js_error_event.js */',read('js_error_event.js'))
    .replace('    /* @include js_promise_rejection.js */',read('js_promise_rejection.js'))
    .replace('    /* @include js_custom_event.js */',read('js_custom_event.js'));
const workerInit=new Function(`const apply=Reflect.apply,host={now:()=>1},report=e=>{throw e;},abortBridge={signals:{is:()=>false},listen(){}};
    ${workerEvents}
    const target=new EventTarget();return {Event,ErrorEvent,EventTarget,native:dispatchNativeWorkerEvent,target};`);
const worker=workerInit(),wt=new worker.EventTarget();let workerChecks=0;
function wok(value,label){workerChecks++;assert(value,label);}
let wc=0;wt.addEventListener('real',()=>wc++);wt.addEventListener('fake',()=>wc+=100);
const we=new worker.Event('real');Object.defineProperty(we,'type',{value:'fake'});we._stop=true;we._dispatching=true;wt.dispatchEvent(we);wok(wc===1,'Worker dispatch uses private fields');
Object.defineProperty(we,'cancelable',{value:true});Object.defineProperty(we,'defaultPrevented',{value:true});we.preventDefault();wok(wt.dispatchEvent(we),'Worker cancellation cannot be forged');
wok(!Reflect.set(we,'isTrusted',true)&&!Object.getOwnPropertyDescriptor(we,'isTrusted').configurable,'Worker trust unforgeable');
const native=new worker.Event('native');let nativeTrusted,nativeShape;wt.addEventListener('native',e=>{nativeTrusted=e.isTrusted;nativeShape=e.target===wt&&e.currentTarget===wt&&e.eventPhase===2&&e.composedPath()[0]===wt;},{once:true});
worker.native(wt,native);wok(nativeTrusted&&nativeShape,'Worker native dispatch trusted, with target, phase and path');wt.dispatchEvent(native);wok(!native.isTrusted,'Worker public redispatch clears trust');
wok(native.currentTarget===null&&native.eventPhase===0&&!native.composedPath().length,'Worker dispatch state cleaned');
const before=new worker.Event('real');before.stopPropagation();wt.dispatchEvent(before);wok(wc===2,'Worker pre-stop suppresses target');wt.dispatchEvent(before);wok(wc===3,'Worker pre-stop cleared after dispatch');
const phased=new worker.EventTarget(),phases=[];phased.addEventListener('phase',()=>phases.push('b'));phased.addEventListener('phase',e=>{phases.push('c');e.stopPropagation();},true);phased.dispatchEvent(new worker.Event('phase'));wok(phases.join('')==='c','Worker at-target capture runs first, and its stopPropagation ends the bubbling invocation (DOM invoke step 6)');
const wp=new worker.EventTarget();wp.addEventListener('cancel',e=>e.preventDefault(),{passive:true});wok(wp.dispatchEvent(new worker.Event('cancel',{cancelable:true})),'Worker passive cannot cancel');
let workerNested=false;const recursive=new worker.EventTarget();recursive.addEventListener('nested',e=>{try{recursive.dispatchEvent(e);}catch(error){workerNested=error.name==='InvalidStateError';}e.initEvent('changed');});const wn=new worker.Event('nested');recursive.dispatchEvent(wn);wok(workerNested&&wn.type==='nested','Worker redispatch throws and initEvent ignores a dispatching event');
const error=new worker.ErrorEvent('error',{message:'native failure',cancelable:true});wok(!Reflect.set(error,'message','forged')&&error.message==='native failure','Worker shared ErrorEvent readonly IDL');
let workerErrorArgs;worker.target.onerror=(...args)=>{workerErrorArgs=args;return true;};
const workerError=new worker.ErrorEvent('error',{message:'w',filename:'u',lineno:1,colno:2,error:failure,cancelable:true});
wok(!worker.native(worker.target,workerError)&&workerErrorArgs.length===5&&workerErrorArgs[0]==='w'&&workerErrorArgs[4]===failure,'Worker global onerror gets the five values and true cancels');
worker.target.onerror=()=>false;wok(worker.native(worker.target,new worker.ErrorEvent('error',{cancelable:true})),'Worker global onerror returning false does not cancel');
console.log('worker event internal state host: '+workerChecks+' checks, 0 failed');
