(function(){'use strict';
const apply=Reflect.apply,rawDom=nativeDom;
class Document{};class CharacterData{};
globalThis.Document=Document;globalThis.CharacterData=CharacterData;
const documentBridge={brand(n){if(!rawDom('isNode',null,n)||rawDom('get',n,'nodeType')!==9)throw TypeError('Illegal Document receiver');}};
const characterDataBrand=n=>{if(!rawDom('isNode',null,n)||![3,4,7,8].includes(rawDom('get',n,'nodeType')))throw TypeError('Illegal CharacterData receiver');};
globalThis.DOMException=class DOMException extends Error{constructor(message,name){super(message);this.name=name;}};
const scheduled=[],reactions=[];let reactionDepth=0;
globalThis.setTimeout=fn=>{scheduled.push(fn);return scheduled.length;};
globalThis.Event=class Event{constructor(type){this.type=type}};
function dispatch(target,event){selectionEvents.push(event.type);}
const selectionEvents=[];
const customElementsBridge={reactions(fn){reactionDepth++;try{return fn();}finally{if(!--reactionDepth)while(reactions.length)reactions.shift()();}}};
let onRemoved=null;
function dom(...args){
    const action=()=>{const before=rangeBridge.before(...args),result=rawDom(...args);rangeBridge.after(before);if(args[0]==='remove'&&onRemoved)reactions.push(()=>onRemoved(args[1]));return result;};
    return customElementsBridge.reactions(action);
}
function make(type,text='',owner=document){return makeNative(type,owner,text);}
function append(p,...nodes){for(const n of nodes)rawDom('insert',p,n,null);return p;}
function text(n){return rawDom('get',n,'nodeValue');}
function kids(n){return rawDom('get',n,'childNodes');}
function range(a,ao,b,bo){const r=document.createRange();r.setStart(a,ao);r.setEnd(b,bo);return r;}
function flushSelection(){while(scheduled.length)scheduled.shift()();}
Object.setPrototypeOf(document,Document.prototype);
