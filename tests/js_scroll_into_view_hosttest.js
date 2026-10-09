// Only the new scrollIntoView IDL/dispatch boundary; no old suite rerun.
const fs=require('fs'),vm=require('vm'),assert=require('assert');
const source=fs.readFileSync('user/libc/web/js_html_elements.js','utf8');
const start=source.indexOf('    const finiteScroll='),end=source.indexOf("    define(HTMLElement.prototype,'innerText'",start);
let total=0;const ok=(v,n)=>{total++;assert.ok(v,n);};
class Element{};
const known=new WeakSet(),active=new WeakMap(),owner={},posted=[],events=[],logs=[],requests=[];
const target=new Element(),parent=new Element();known.add(target);known.add(parent);active.set(target,true);active.set(parent,true);
const rawDom=(op,node,...args)=>{
    if(!known.has(node))throw new TypeError('Illegal receiver');
    if(op==='get')return args[0]==='ownerDocument'?owner:args[0]==='elementScrollActive'?active.get(node):undefined;
    if(op==='elementScrollIntoView'){requests.push(args);return active.get(node)?[parent]:[];}
    throw new Error('Old path must not run');
};
vm.runInNewContext(source.slice(start,end),{Element,rawDom,define:Object.defineProperty,TypeErrorImpl:TypeError,
    string:v=>{if(typeof v==='symbol')throw new TypeError();return String(v);},Event:class{constructor(t){this.type=t;}},
    host:{postTask:fn=>posted.push(fn),log:(l,m)=>logs.push(m)},dispatch:(n,e)=>events.push([n,e])});
const method=Element.prototype.scrollIntoView,d=Object.getOwnPropertyDescriptor(Element.prototype,'scrollIntoView');
ok(d.enumerable&&d.configurable&&d.writable&&method.length===0,'IDL descriptor');
for(const [arg,block] of [[undefined,0],[null,0],[true,0],[false,1],[0,1],['',1],['false',0],[1n,0],[Symbol(),0]]){
    ok(method.call(target,arg)===undefined,'legacy undefined');const r=requests.at(-1);ok(r[0]===block&&r[1]===3&&!r[2],'boolean union/default');
}
for(const [name,code] of [['start',0],['end',1],['center',2],['nearest',3]]){
    target.scrollIntoView({block:name,inline:name,container:'nearest'});const r=requests.at(-1);ok(r[0]===code&&r[1]===code&&r[2],'logical enum/native contract');
}
const order=[];target.scrollIntoView({get behavior(){order.push('behavior');return 'instant';},get block(){order.push('block');return 'center';},get container(){order.push('container');return 'all';},get inline(){order.push('inline');return 'end';}});
ok(order.join(',')==='behavior,block,container,inline','dictionary IDL read order');
for(const [key,value] of [['behavior','SMOOTH'],['block','middle'],['inline','left'],['container','parent'],['block',null],['inline',Symbol()]]){
    const before=requests.length;let thrown=false;try{target.scrollIntoView({[key]:value});}catch(e){thrown=e.name==='TypeError';}ok(thrown&&requests.length===before,'invalid dictionary before native');
}
for(const receiver of [null,undefined,{},Object.create(Element.prototype)]){
    let thrown=false;try{method.call(receiver);}catch(e){thrown=e.name==='TypeError';}ok(thrown,'native brand not duck type');
}
target.scrollIntoView({get block(){active.set(target,false);return 'start';}});ok(requests.length>0,'reentry reaches native activity recheck');
while(posted.length)posted.shift()();ok(events.length===1&&events[0][0]===parent,'only changed ancestor scroll event coalesced');
active.set(target,true);target.scrollIntoView({behavior:'smooth'});target.scrollIntoView({behavior:'smooth'});ok(logs.length===1&&logs[0].includes('instant'),'shared honest smooth fallback');
active.set(parent,false);while(posted.length)posted.shift()();ok(events.length===1,'retired changed ancestor no event');
console.log('scrollIntoView JS host '+total+' checks / 0 failed');
