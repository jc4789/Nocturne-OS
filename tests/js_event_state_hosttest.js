/* Execute exact Event/EventTarget/dispatch product source. The path stub only
 * supplies a two-target tree; native DOM and real-site acceptance are separate. */
const fs=require('node:fs'),assert=require('node:assert/strict');
const source=fs.readFileSync('user/libc/web/js_bootstrap.js','utf8');
function slice(start,end){const a=source.indexOf(start),b=source.indexOf(end,a);assert(a>=0&&b>a);return source.slice(a,b);}
const eventSource=slice('    const eventStates=', '    /* @include js_error_event.js */');
const targetSource=slice('    class EventTarget {','    function handlerTarget(');
const dispatchSource=slice('    function invoke(target, event, capture) {','    class Node extends EventTarget {');
const init=new Function(`
    const apply=Reflect.apply,eventSlice=Array.prototype.slice,eventPaths=new WeakMap(),focusData=new WeakMap(),listenerMap=new WeakMap(),inlineMap=new WeakMap();
    const host={now:()=>1},report=e=>{throw e;},options=o=>typeof o==='boolean'?{capture:o}:o||{};
    const handlerRecord=()=>null,handlerTarget=()=>false;
    function removeListener(target,entry){entry.removed=true;const a=listenerMap.get(target),i=a.indexOf(entry);if(i>=0)a.splice(i,1);}
    const shadowBridge={path(target,event){const nodes=[target];if(target.parent)nodes.push(target.parent);return {entries:nodes.map(node=>({node,target,related:null,visible:nodes,atTarget:node===target})),finalTarget:target,finalRelated:null};}};
    ${eventSource}${targetSource}${dispatchSource}
    class Node extends EventTarget {}
    return {Event,EventTarget,state:eventState};
`);
const {Event,EventTarget,state}=init();let checks=0;
function ok(value,label){checks++;assert(value,label);}
const parent=new EventTarget(),target=new EventTarget();target.parent=parent;
const order=[];
target.addEventListener('click',e=>{order.push('first');Object.defineProperty(e,'type',{value:'other'});Object.defineProperty(e,'bubbles',{value:false});e._stop=true;e._immediate=true;});
target.addEventListener('click',()=>order.push('second'));parent.addEventListener('click',()=>order.push('parent'));
const event=new Event('click',{bubbles:true});ok(target.dispatchEvent(event),'dispatch allowed');ok(order.join(',')==='first,second,parent','author fields cannot change private type/bubbles/stop');
const untrusted=new Event('probe');ok(!Reflect.set(untrusted,'isTrusted',true),'trust readonly');ok(!Object.getOwnPropertyDescriptor(untrusted,'isTrusted').configurable,'trust unforgeable');
ok(!Reflect.set(untrusted,'type','changed')&&untrusted.type==='probe','type readonly');
Object.defineProperty(untrusted,'defaultPrevented',{value:true});Object.defineProperty(untrusted,'cancelable',{value:true});untrusted.preventDefault();ok(target.dispatchEvent(untrusted),'public canceled flag cannot forge cancellation');
state(untrusted).isTrusted=true;target.addEventListener('probe',e=>ok(!e.isTrusted,'script dispatch clears native trust'));target.dispatchEvent(untrusted);
const stopped=new Event('before');let calls=0;target.addEventListener('before',()=>calls++);stopped.stopImmediatePropagation();target.dispatchEvent(stopped);ok(calls===0&&!stopped.cancelBubble,'preexisting stop respected and cleared after dispatch');
target.dispatchEvent(stopped);ok(calls===1,'reused event no stale stop');
const cancel=new Event('cancel',{cancelable:true});target.addEventListener('cancel',e=>e.preventDefault());ok(!target.dispatchEvent(cancel)&&cancel.defaultPrevented,'real cancellation preserved');
const passive=new Event('passive',{cancelable:true});target.addEventListener('passive',e=>e.preventDefault(),{passive:true});ok(target.dispatchEvent(passive)&&!passive.defaultPrevented,'passive cancellation blocked');
const nested=new Event('nested');target.addEventListener('nested',e=>{e._dispatching=false;assert.throws(()=>target.dispatchEvent(e),{name:'InvalidStateError'});checks++;});target.dispatchEvent(nested);
const old=new Event('old');state(old).initialized=false;assert.throws(()=>target.dispatchEvent(old),{name:'InvalidStateError'});checks++;old.initEvent('new',true,true);ok(old.type==='new'&&old.bubbles&&old.cancelable,'legacy initialization preserved');
console.log('event internal state host: '+checks+' checks, 0 failed');
const workerSource=fs.readFileSync('user/libc/web/js_worker_runtime.js','utf8');
const workerStart=workerSource.indexOf('    const eventStates='),workerEnd=workerSource.indexOf('    Object.assign(globalThis,{DOMException,Event,ErrorEvent,EventTarget});',workerStart);
assert(workerStart>=0&&workerEnd>workerStart);
const workerEvents=workerSource.slice(workerStart,workerEnd).replace('    /* @include js_error_event.js */',fs.readFileSync('user/libc/web/js_error_event.js','utf8'));
const workerInit=new Function(`const apply=Reflect.apply,host={now:()=>1},report=e=>{throw e;},workerAbortBridge={brand:()=>false};
    ${workerEvents}
    const target=new EventTarget();return {Event,ErrorEvent,EventTarget,native:dispatchNativeWorkerEvent};`);
const worker=workerInit(),wt=new worker.EventTarget();let workerChecks=0;
function wok(value,label){workerChecks++;assert(value,label);}
let wc=0;wt.addEventListener('real',()=>wc++);wt.addEventListener('fake',()=>wc+=100);
const we=new worker.Event('real');Object.defineProperty(we,'type',{value:'fake'});we._stop=true;we._dispatching=true;wt.dispatchEvent(we);wok(wc===1,'Worker dispatch uses private fields');
Object.defineProperty(we,'cancelable',{value:true});Object.defineProperty(we,'defaultPrevented',{value:true});we.preventDefault();wok(wt.dispatchEvent(we),'Worker cancellation cannot be forged');
wok(!Reflect.set(we,'isTrusted',true)&&!Object.getOwnPropertyDescriptor(we,'isTrusted').configurable,'Worker trust unforgeable');
const native=new worker.Event('native');let nativeTrusted;wt.addEventListener('native',e=>{nativeTrusted=e.isTrusted;wok(e.target===wt&&e.currentTarget===wt&&e.eventPhase===2&&e.composedPath()[0]===wt,'Worker native target phase path');},{once:true});
worker.native(wt,native);wok(nativeTrusted,'Worker native dispatch trusted');wt.dispatchEvent(native);wok(!native.isTrusted,'Worker public redispatch clears trust');
wok(native.currentTarget===null&&native.eventPhase===0&&!native.composedPath().length,'Worker dispatch state cleaned');
const before=new worker.Event('real');before.stopPropagation();wt.dispatchEvent(before);wok(wc===2,'Worker pre-stop suppresses target');wt.dispatchEvent(before);wok(wc===3,'Worker pre-stop cleared after dispatch');
const phased=new worker.EventTarget(),phases=[];phased.addEventListener('phase',()=>phases.push('b'));phased.addEventListener('phase',e=>{phases.push('c');e.stopPropagation();},true);phased.dispatchEvent(new worker.Event('phase'));wok(phases.join('')==='cb','Worker stopPropagation permits same target listeners');
const wp=new worker.EventTarget();wp.addEventListener('cancel',e=>e.preventDefault(),{passive:true});wok(wp.dispatchEvent(new worker.Event('cancel',{cancelable:true})),'Worker passive cannot cancel');
const recursive=new worker.EventTarget();recursive.addEventListener('nested',e=>{assert.throws(()=>recursive.dispatchEvent(e),{name:'InvalidStateError'});workerChecks++;e.initEvent('changed');});const wn=new worker.Event('nested');recursive.dispatchEvent(wn);wok(wn.type==='nested','Worker initEvent ignores dispatching event');
const error=new worker.ErrorEvent('error',{message:'native failure',cancelable:true});wok(!Reflect.set(error,'message','forged')&&error.message==='native failure','Worker shared ErrorEvent readonly IDL');
console.log('worker event internal state host: '+workerChecks+' checks, 0 failed');
