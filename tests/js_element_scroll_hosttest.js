// Exact production conversion/dispatch block. Native positions/posted queue
// are harness shims; actual paint/hit validation is the native guest supplement.
const fs=require('fs'),vm=require('vm'),assert=require('assert');
const source=fs.readFileSync('user/libc/web/js_html_elements.js','utf8');
const start=source.indexOf('    const finiteScroll='),end=source.indexOf("    define(HTMLElement.prototype,'innerText'",start);
let total=0;const ok=(v,n)=>{total++;assert.ok(v,n);};
const state=new WeakMap(),tasks=[],events=[],logs=[];let writes=0,reads=0;
class Element{};
const rawDom=(op,node,...args)=>{
    const s=state.get(node);if(!s)throw new TypeError('Illegal element receiver');
    if(op==='get')return args[0]==='ownerDocument'?s.owner:args[0]==='elementScrollActive'?s.active:undefined;
    if(op==='geometry'){reads++;return {scrollLeft:s.x,scrollTop:s.y,scrollWidth:s.w,scrollHeight:s.h}[args[0]];}
    if(op==='elementScroll'){
        writes++;if(!s.active)return false;
        let [x,y]=args;x=Number.isFinite(x)?Math.max(0,Math.min(x,s.w-100)):0;y=Number.isFinite(y)?Math.max(0,Math.min(y,s.h-50)):0;
        const changed=x!==s.x||y!==s.y;s.x=x;s.y=y;return changed;
    }
    throw new Error('Unknown bridge');
};
const context=vm.createContext({Element,rawDom,define:Object.defineProperty,TypeErrorImpl:TypeError,string:v=>{if(typeof v==='symbol')throw new TypeError();return String(v);},
    host:{postTask:fn=>tasks.push(fn),log:(l,m)=>logs.push(m)},dispatch:(n,e)=>events.push([n,e]),Event:class{constructor(t){this.type=t;this.bubbles=false;this.cancelable=false;}}});
vm.runInContext(source.slice(start,end),context);
const node=new Element(),s={owner:{},active:true,x:0,y:0,w:500,h:300};state.set(node,s);
ok(node.scrollTo(100.5,80.25)===undefined,'legacy undefined return');ok(s.x===100.5&&s.y===80.25,'two coordinates');
node.scroll({top:33.5});ok(s.x===100.5&&s.y===33.5,'omitted axis preserved');
node.scrollBy({left:25.25});ok(s.x===125.75&&s.y===33.5,'relative omitted axis zero');
node.scrollBy(-5,-3);ok(s.x===120.75&&s.y===30.5,'relative two args');
node.scrollLeft=190.125;ok(s.x===190.125&&s.y===30.5,'fractional left setter');node.scrollTop=12.125;ok(s.x===190.125&&s.y===12.125,'fractional top setter');
ok(node.scrollWidth===500&&node.scrollHeight===300,'native sizes');ok(tasks.length===1,'same task scroll events coalesced');tasks.shift()();ok(events.length===1&&events[0][1].isTrusted&&!events[0][1].bubbles,'posted trusted nonbubbling event');
node.scrollTo(1e300,1e300);ok(s.x===400&&s.y===250,'huge finite clamps native extent');
for(const value of [NaN,Infinity,-Infinity,-1]){node.scroll(value,value);ok(s.x===0&&s.y===0,'nonfinite and negative bounded');}
const order=[];node.scroll({get behavior(){order.push('b');return 'instant';},get left(){order.push('l');return {valueOf(){order.push('lv');return 2;}};},get top(){order.push('t');return {valueOf(){order.push('tv');return 3;}};}});
ok(order.join(',')==='b,l,lv,t,tv','dictionary conversion order');
node.scroll({get left(){node.scrollTo(50,60);return undefined;},top:70});ok(s.x===50&&s.y===70,'positions snapshot after reentrant conversion');
for(const method of ['scroll','scrollTo','scrollBy']){
    const d=Object.getOwnPropertyDescriptor(Element.prototype,method);ok(d.enumerable&&d.configurable&&d.writable&&d.value.length===0,'IDL descriptor');
    for(const receiver of [{},null,undefined]){let thrown=false;try{d.value.call(receiver,{left:1});}catch(e){thrown=e instanceof TypeError;}ok(thrown,'native brand rejected');}
    for(const value of [1,true,'2',1n,Symbol()]){let thrown=false;try{node[method](value);}catch(e){thrown=e instanceof TypeError;}ok(thrown,'primitive dictionary rejected');}
    for(const value of [1n,Symbol()]){const old=writes;let thrown=false;try{node[method](value,2);}catch(e){thrown=e.name==='TypeError';}ok(thrown&&writes===old,'BigInt/Symbol ToNumber before write');}
}
for(const value of ['SMOOTH','invalid',null,Symbol()]){const old=writes;let thrown=false;try{node.scroll({behavior:value,left:1});}catch(e){thrown=e instanceof TypeError;}ok(thrown&&writes===old,'invalid behavior rejected before write');}
node.scrollTo({behavior:'smooth',left:120});node.scrollBy({behavior:'smooth',left:5});ok(s.x===125&&logs.length===1&&logs[0].includes('instant'),'smooth honestly instant once reported');
node.scroll();node.scroll(null);node.scroll(undefined);ok(s.x===125,'empty options retain offsets');
node.scrollBy();node.scrollBy(null);ok(s.x===125,'empty relative options zero');
const before=writes;s.active=false;node.scrollBy({left:10});ok(s.x===125&&writes===before+1,'native activity recheck no fake JS offset');s.active=true;
while(tasks.length)tasks.shift()();events.length=0;
node.scrollBy(1,0);s.owner={};while(tasks.length)tasks.shift()();ok(events.length===0,'adoption before posted task suppresses wrong document event');
node.scrollBy(1,0);s.active=false;while(tasks.length)tasks.shift()();ok(events.length===0,'retired target iframe suppresses escaped posted event');
console.log('element scroll JS host '+total+' checks / 0 failed');
