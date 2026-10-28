/* DOM dispatch's activation steps in the exact product source (js_bootstrap.js dispatch, Event
 * and MouseEvent, js_pointer_events.js nativeDispatch and activationForward), against a host
 * whose activation table is a recording stand-in for js_activation_native.h. What is checked:
 * the activation target's choice on the path, the order pre-activation / listeners / activation
 * or canceled activation, the event's state when the behavior runs, the trust and the user's
 * keys handed to it, and the label's forwarded click. The element behaviors themselves (C), the
 * native path without a realm and WPT in the guest are separate.
 *
 *   node tests/js_activation_hosttest.js [source directory, default user/libc/web/]
 * Prints "activation host: N checks, M failed" and exits 1 when any failed. */
const fs=require('node:fs'),path=require('node:path');
const web=process.argv[2]||'user/libc/web/',read=name=>fs.readFileSync(path.join(web,name),'utf8');
const source=read('js_bootstrap.js');
function slice(start,end){const a=source.indexOf(start),b=source.indexOf(end,a);if(a<0||b<=a)throw new Error('marker '+start);return source.slice(a,b);}
const capturesSource=slice('    // Dispatch reads the listener lists','    const listenerMap = new WeakMap()');
const currentSource=slice("    // This Window's current event",'    let errorReporting=false;');
const eventSource=slice('    /* Event state: one table','    /* @include js_error_event.js */');
const uiSource=slice('    class UIEvent extends Event {','    /* The native side counts').replace('    /* @include js_input_event.js */','');
const listenerSource=slice('    /* The native side counts','    function resetDocumentEvents(');
const targetSource=slice('    class EventTarget {','    function handlerTarget(');
const handlerSource=slice('    /* HTML "activate an event handler"','    function handlerRecord(');
const dispatchSource=slice('    /* DOM "inner invoke"','    class Node extends EventTarget {');
const init=new Function('table',`
    const apply=Reflect.apply,eventSlice=Array.prototype.slice,mouseAssign=Object.assign;
    ${capturesSource}
    const listenerMap=new WeakMap(),inlineMap=new WeakMap(),inputWindow=globalThis;
    const reports=[];
    const host={now:()=>1,scroll:()=>0,pointerCapturePending:()=>{},inputListener:()=>false,storageListener:()=>{},
        activation:(...args)=>table(...args),
        eventRealmInit(port,table){host.port=port;return table;},eventRealm(){return host.port;},
        eventActive(){return true;},reportException(e){reports.push(e);}};
    function report(e){host.reportException(e);}
    ${currentSource}
    const document={},globalHandlerTypes=new Set();
    function rawDom(op,target,key){if(key==='elementBrand'){if(!(target instanceof Element))throw new TypeError('Element required');return true;}return key==='isConnected'?true:key==='ownerDocument'?document:undefined;}
    rawDom.isNode=()=>false;
    const handlerTarget=()=>false,handlerRecord=()=>null;
    function handlerValue(){return null;}
    /* The path: the target, then each node's parent; a node marked host is a shadow host the
       event reaches as its own target (its shadow-adjusted target is itself); the Window ends a
       path that reaches the document. */
    const shadowBridge={path(target){
        const nodes=[target];for(let n=target.parent;n;n=n.parent)nodes.push(n);
        if(nodes.at(-1).document)nodes.push(globalThis);
        let adjusted=target;const entries=[];
        for(const node of nodes){if(node.host)adjusted=node;entries.push({node,target:adjusted,related:null,visible:nodes,atTarget:node===adjusted,scopes:[]});}
        return {entries,finalTarget:target,finalRelated:null};}};
    ${eventSource}
    ${read('js_error_event.js')}
    ${uiSource}${listenerSource}${targetSource}${handlerSource}${dispatchSource}
    class Node extends EventTarget {}
    class Element extends Node {}
    const formValidationBridge={SubmitEvent:Event};
    ${read('js_pointer_events.js')}
    return {Event,MouseEvent,PointerEvent,Element,state:eventState,native:nativeDispatch,dispatch,reports,
        forward:typeof activationForward==='function'?activationForward:null};
`);
/* The stand-in table: kind 'checkbox' (pre flips checked, cancel restores it), 'label' (runs the
   product's forwarding to its control), anything else only records. */
const calls=[];
let product;
function table(op,node,...rest){
    if(op===0){calls.push(['has',node.name]);return !!node.kind;}
    if(op===1){calls.push(['pre',node.name]);if(node.kind!=='checkbox')return undefined;const record=[node.checked];node.checked=!node.checked;return record;}
    if(op===2){
        const [event,target,trusted,ctrl,shift]=rest,s=product.state(event);
        calls.push(['run',node.name,{trusted,ctrl,shift,target,phase:event.eventPhase,current:event.currentTarget,
            path:event.composedPath().length,prevented:event.defaultPrevented,dispatching:s.dispatching,eventTarget:event.target}]);
        if(node.run)node.run(event);
        if(node.kind==='label' && product.forward)product.forward(node.control,event);
        return undefined;
    }
    if(op===3){calls.push(['cancel',node.name]);const [record]=rest;if(node.kind==='checkbox'&&record)node.checked=record[0];return undefined;}
    throw new RangeError('op '+op);
}
product=init(table);
const {Event,MouseEvent,PointerEvent,Element}=product;
let checks=0,failed=0;
function check(label,fn){
    checks++;calls.length=0;
    try{if(!fn())throw new Error('false');}
    catch(e){failed++;console.log('FAIL '+label+': '+(e&&e.message));}
}
function node(name,kind,parent){const n=new Element();n.name=name;n.kind=kind;n.parent=parent||null;n.checked=false;return n;}
const runs=()=>calls.filter(c=>c[0]==='run'),names=kind=>calls.filter(c=>c[0]===kind).map(c=>c[1]);

check('a click MouseEvent from dispatchEvent runs the target\'s activation behavior',()=>{
    const input=node('input','checkbox');
    let seen;input.addEventListener('click',()=>{seen=input.checked;});
    input.dispatchEvent(new MouseEvent('click',{bubbles:true}));
    return seen===true && input.checked===true && runs().length===1 && runs()[0][2].trusted===false;
});
check('an Event that is not a MouseEvent has no activation, nor is any element asked',()=>{
    const input=node('input','checkbox');input.dispatchEvent(new Event('click',{bubbles:true}));
    return !input.checked && calls.length===0;
});
check('a click MouseEvent of another type name is no activation event',()=>{
    const input=node('input','checkbox');input.dispatchEvent(new MouseEvent('mouseup',{bubbles:true}));
    return !input.checked && calls.length===0;
});
check('parents are looked at only when the event bubbles',()=>{
    const input=node('input','checkbox'),text=node('text',null,input);
    text.dispatchEvent(new MouseEvent('click'));
    return !input.checked && names('has').join()==='text';
});
check('a bubbling click activates the first parent with activation behavior',()=>{
    const input=node('input','checkbox'),text=node('text',null,input);
    text.dispatchEvent(new MouseEvent('click',{bubbles:true}));
    return input.checked && names('run').join()==='input';
});
check('the first with activation behavior is chosen, not an outer one',()=>{
    const outer=node('outer','checkbox'),inner=node('inner','checkbox',outer);
    let outerSeen,innerSeen;outer.addEventListener('click',()=>{outerSeen=outer.checked;});inner.addEventListener('click',()=>{innerSeen=inner.checked;});
    inner.dispatchEvent(new MouseEvent('click',{bubbles:true}));
    return innerSeen===true && outerSeen===false && names('pre').join()==='inner' && names('run').join()==='inner';
});
check('a shadow host the event reaches as its own target is chosen without bubbles',()=>{
    const host=node('host','summary');host.host=true;const inside=node('inside',null,host);
    const event=new MouseEvent('click');inside.dispatchEvent(event);
    return names('run').join()==='host';
});
check('the Window is never asked',()=>{
    const doc=node('document',null);doc.document=true;const div=node('div',null,doc);
    div.dispatchEvent(new MouseEvent('click',{bubbles:true}));
    return names('has').join()==='div,document';
});
check('legacy-pre-activation runs before the capture listeners',()=>{
    const parent=node('parent',null),input=node('input','checkbox',parent);let seen;
    parent.addEventListener('click',()=>{seen=input.checked;},true);
    input.dispatchEvent(new MouseEvent('click',{bubbles:true}));
    return seen===true;
});
check('a canceled click runs legacy-canceled-activation with the pre-activation record, not the behavior',()=>{
    const input=node('input','checkbox');input.addEventListener('click',e=>e.preventDefault());
    const allowed=input.dispatchEvent(new MouseEvent('click',{bubbles:true,cancelable:true}));
    return !allowed && !input.checked && names('cancel').join()==='input' && runs().length===0;
});
check('an event canceled before dispatch is canceled activation too',()=>{
    const input=node('input','checkbox'),event=new MouseEvent('click',{cancelable:true});event.preventDefault();
    input.dispatchEvent(event);return !input.checked && names('cancel').join()==='input' && runs().length===0;
});
check('a canceled click of an element without legacy-pre-activation runs neither',()=>{
    const link=node('a','link');link.addEventListener('click',e=>e.preventDefault());
    link.dispatchEvent(new MouseEvent('click',{bubbles:true,cancelable:true}));
    return names('pre').join()==='a' && names('cancel').length===0 && runs().length===0;
});
check('the behavior runs after dispatch: no phase, no currentTarget, empty path, the target kept',()=>{
    const input=node('input','checkbox');input.dispatchEvent(new MouseEvent('click'));
    const r=runs()[0][2];
    return r.phase===0 && r.current===null && r.path===0 && !r.prevented && !r.dispatching && r.eventTarget===input && r.target===input;
});
check('the event can be redispatched from the activation behavior',()=>{
    const input=node('input','checkbox'),other=node('other',null),event=new MouseEvent('click');
    let target;other.addEventListener('click',e=>{target=e.target;});
    input.run=e=>other.dispatchEvent(e);input.dispatchEvent(event);
    return target===other && event.target===other;
});
check('dispatch returns false when canceled and true otherwise',()=>{
    const a=node('a','link'),b=node('b','link');a.addEventListener('click',e=>e.preventDefault());
    return a.dispatchEvent(new MouseEvent('click',{cancelable:true}))===false && b.dispatchEvent(new MouseEvent('click',{cancelable:true}))===true;
});
check('a native trusted click is trusted with the user\'s own keys, whatever the page writes',()=>{
    const link=node('a','link');link.addEventListener('click',e=>{e.ctrlKey=false;e.shiftKey=true;});
    product.native(link,'click',{bubbles:true,cancelable:true,ctrlKey:true,shiftKey:false});
    const r=runs()[0][2];return r.trusted===true && r.ctrl===true && r.shift===false;
});
check('the page\'s click() (a native untrusted click) is untrusted, with no keys',()=>{
    const link=node('a','link');product.native(link,'click',{bubbles:true,cancelable:true,isTrusted:false,ctrlKey:true});
    const r=runs()[0][2];return r.trusted===false && r.ctrl===false;
});
check('a script MouseEvent with ctrlKey is no preference of the user',()=>{
    const link=node('a','link');link.dispatchEvent(new MouseEvent('click',{ctrlKey:true,shiftKey:true}));
    const r=runs()[0][2];return r.trusted===false && r.ctrl===false && r.shift===false;
});
check('a trusted event redispatched by the page loses its trust for the activation too',()=>{
    const link=node('a','link');let kept;link.addEventListener('click',e=>{kept=e;},{once:true});
    product.native(link,'click',{bubbles:true,cancelable:true});calls.length=0;
    link.dispatchEvent(kept);const r=runs()[0][2];return r.trusted===false;
});
check('a label forwards a trusted click with its pointer and keys to its control',()=>{
    const control=node('control','checkbox'),label=node('label','label');label.control=control;
    let forwarded;control.addEventListener('click',e=>{forwarded=e;});
    product.native(label,'click',{bubbles:true,cancelable:true,pointerId:1,pointerType:'mouse',isPrimary:true,width:1,height:1,pressure:0,ctrlKey:true,clientX:7,clientY:9,detail:1});
    return forwarded && forwarded.isTrusted===true && forwarded instanceof PointerEvent && forwarded.pointerId===1 &&
        forwarded.pointerType==='mouse' && forwarded.isPrimary===true && forwarded.clientX===7 && forwarded.clientY===9 &&
        forwarded.detail===1 && forwarded.ctrlKey===true && control.checked && names('run').join()==='label,control' &&
        runs()[1][2].trusted===true && runs()[1][2].ctrl===true;
});
check('a label forwards an untrusted click untrusted',()=>{
    const control=node('control','checkbox'),label=node('label','label');label.control=control;let forwarded;
    control.addEventListener('click',e=>{forwarded=e;});
    product.native(label,'click',{bubbles:true,cancelable:true,isTrusted:false,pointerId:-1,pointerType:''});
    return forwarded && forwarded.isTrusted===false && forwarded.pointerId===-1 && runs()[1][2].trusted===false;
});
check('a canceled forwarded click puts the control back and the label\'s own event is untouched',()=>{
    const control=node('control','checkbox'),label=node('label','label');label.control=control;
    control.addEventListener('click',e=>e.preventDefault());
    const event=new MouseEvent('click',{bubbles:true,cancelable:true});const allowed=label.dispatchEvent(event);
    return allowed && !event.defaultPrevented && !control.checked && names('cancel').join()==='control';
});
console.log('activation host: '+checks+' checks, '+failed+' failed');
if(product.reports.length)console.log('reported: '+product.reports.map(String).join('; '));
process.exitCode=failed?1:0;
