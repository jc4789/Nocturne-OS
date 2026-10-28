// CSSOM View bindings against a model native layer: scrolling (js_scroll.js: perform a scroll,
// smooth scrolls, ScrollResult promises, scrollIntoView's plan), MouseEvent's prototype
// coordinates (js_bootstrap.js) and GeometryUtils (js_geometry_utils.js over js_geometry.js).
// The native steps themselves (layout, transforms, limits) are js_geometry_native.h's and
// js.c's, checked by tests/check_cssom_view_native.py.
// Run from the repository root: node tests/cssom_view_contract.cjs
'use strict';
const fs=require('fs'),vm=require('vm');
const web='user/libc/web/';
let passed=0,failed=0;
const check=(value,name)=>{if(value)passed++;else{failed++;console.log('FAIL '+name);}};
const throwsName=(fn,name)=>{try{fn();return false;}catch(e){return e&&e.name===name;}};
async function settled(promise){
    let state='pending',value;
    promise.then(v=>{state='fulfilled';value=v;},e=>{state='rejected';value=e;});
    for(let i=0;i<4;i++)await null;
    return {state,value};
}

/* ------------------------------------------------------------ scrolling */
async function scrolling(){
    const source=fs.readFileSync(web+'js_scroll.js','utf8');
    class Element{}
    const boxes=new Map(),documentNode={},root=new Element(),posted=[],events=[];
    let clock=0,frames=[],frameId=0;
    const viewport={x:0,y:0,maxX:500,maxY:4000};
    function box(kind,maxX,maxY,behavior='auto'){const e=new Element();boxes.set(e,{kind,x:0,y:0,maxX,maxY,behavior,active:true});return e;}
    boxes.set(root,{kind:2,x:0,y:0,maxX:0,maxY:0,behavior:'auto',active:true});
    const clamp=(v,max)=>Math.max(0,Math.min(v,max));
    const rawDom={
        get(node,name){
            if(node===documentNode&&name==='documentElement')return root;
            const b=boxes.get(node);if(!b)throw new TypeError('Illegal Element receiver');
            if(name==='elementBrand')return true;
            if(name==='ownerDocument')return documentNode;
            if(name==='elementScrollActive')return b.active;
            throw new Error('unexpected get '+name);
        },
        geometry(node,name){
            const b=boxes.get(node);if(!b)throw new TypeError('Invalid DOM receiver');
            if(name==='scrollBox')return b.kind;
            if(name==='scrollLeft')return b.kind===2?viewport.x:b.x;
            if(name==='scrollTop')return b.kind===2?viewport.y:b.y;
            if(name==='scrollMaxLeft')return b.maxX;
            if(name==='scrollMaxTop')return b.maxY;
            if(name==='scrollWidth')return b.maxX+100;
            throw new Error('unexpected geometry '+name);
        },
        elementScroll(node,x,y){
            const b=boxes.get(node);x=clamp(x,b.maxX);y=clamp(y,b.maxY);
            if(b.x===x&&b.y===y)return false;b.x=x;b.y=y;return true;
        },
        elementScrollIntoView(node,block,inline,nearest){rawDom.lastIntoView=[block,inline,nearest];return rawDom.plan(node);},
        plan:()=>[]
    };
    const host={
        postTask:fn=>posted.push(fn),now:()=>clock,
        timer(kind,fn){if(kind!==2)throw new Error('frame timer expected');frames.push([++frameId,fn]);return frameId;},
        clear(id){frames=frames.filter(f=>f[0]!==id);},
        scroll(a,b){
            if(arguments.length>1){viewport.x=Math.trunc(clamp(a,viewport.maxX));viewport.y=Math.trunc(clamp(b,viewport.maxY));viewport.writes=(viewport.writes||0)+1;return;}
            return [viewport.x,viewport.y,viewport.maxX,viewport.maxY][a];
        }
    };
    const runFrames=(ms=16)=>{clock+=ms;const now=frames;frames=[];for(const f of now)f[1](clock);};
    const context=vm.createContext({Element,rawDom,host,Promise,TypeError,Map,WeakMap,Math,Number,Object,
        elementURL:{string:v=>{if(typeof v==='symbol')throw new TypeError('symbol');return String(v);}},
        Event:class{constructor(type){this.type=type;}},eventState:()=>({}),dispatch:(node,event)=>events.push([node,event.type]),
        dom:(op,node,name)=>{if(op!=='computed'||name!=='scroll-behavior')throw new Error('unexpected dom '+op);return boxes.get(node).behavior;},
        document:documentNode});
    const bridge=vm.runInContext(source+'\nscrollBridge',context);
    const windowReceiver=self=>{if(self!==undefined&&self!==null&&self!==context)throw new TypeError('Illegal Window receiver');};
    bridge.install(windowReceiver);
    const proto=Element.prototype;
    for(const name of ['scroll','scrollTo','scrollBy','scrollIntoView'])
        check(typeof proto[name]==='function'&&Object.getOwnPropertyDescriptor(proto,name).enumerable,'Element.'+name+' is an operation');
    for(const name of ['scroll','scrollTo','scrollBy'])check(typeof context[name]==='function','Window.'+name+' is an operation');

    const scroller=box(1,0,9000);
    let p=scroller.scrollTo({top:5000,behavior:'instant'});
    check(p instanceof Promise,'instant: a Promise');
    check(boxes.get(scroller).y===5000,'instant: scrolled before the Promise settles');
    let r=await settled(p);
    check(r.state==='fulfilled'&&r.value.interrupted===false&&typeof r.value==='object','instant: non-interrupted ScrollResult');
    posted.splice(0).forEach(fn=>fn());
    check(events.length===1&&events[0][0]===scroller&&events[0][1]==='scroll','instant: one scroll event queued');

    scroller.scrollTop=0;posted.splice(0).forEach(fn=>fn());events.length=0;
    p=scroller.scroll({top:5000,behavior:'smooth'});
    check(boxes.get(scroller).y===0,'smooth: not moved before a frame');
    for(let i=0;i<4;i++)runFrames(16);
    const mid=boxes.get(scroller).y;
    check(mid>0&&mid<5000,'smooth: moves over frames');
    check((await settled(p)).state==='pending','smooth: pending while moving');
    for(let i=0;i<60&&frames.length;i++)runFrames(16);
    r=await settled(p);
    check(boxes.get(scroller).y===5000&&r.value&&r.value.interrupted===false,'smooth: reaches its position, not interrupted');

    scroller.scrollTop=0;
    p=scroller.scrollTo({top:5000,behavior:'smooth'});
    scroller.scrollTop=7000;
    check(boxes.get(scroller).y===7000,'setter during smooth: applied at once');
    r=await settled(p);
    check(r.value&&r.value.interrupted===true,'setter during smooth: interrupted result');
    check(frames.length===0,'setter during smooth: the frame is cancelled');

    scroller.scrollTop=0;
    const first=scroller.scrollTo({top:5000,behavior:'smooth'}),second=scroller.scrollTo({top:7000,behavior:'smooth'});
    check((await settled(first)).value.interrupted===true,'second smooth scroll interrupts the first');
    for(let i=0;i<60&&frames.length;i++)runFrames(16);
    check((await settled(second)).value.interrupted===false&&boxes.get(scroller).y===7000,'second smooth scroll completes');

    scroller.scrollTop=0;
    p=scroller.scrollBy({top:3000,behavior:'smooth'});
    runFrames(16);runFrames(16);
    boxes.get(scroller).y=10; // the user (native input) scrolls it
    runFrames(16);
    check((await settled(p)).value.interrupted===true,'native scroll during smooth: interrupted');

    const other=box(1,0,900);
    scroller.scrollTop=0;
    p=scroller.scrollTo({top:5000,behavior:'smooth'});
    other.scrollTop=100;
    for(let i=0;i<60&&frames.length;i++)runFrames(16);
    check((await settled(p)).value.interrupted===false&&boxes.get(scroller).y===5000,'an unrelated box does not interrupt');

    const styled=box(1,0,800,'smooth');
    p=styled.scrollTo({top:400});
    check(boxes.get(styled).y===0,'scroll-behavior: smooth with behavior auto is smooth');
    for(let i=0;i<60&&frames.length;i++)runFrames(16);
    check((await settled(p)).value.interrupted===false&&boxes.get(styled).y===400,'scroll-behavior smooth completes');
    styled.scrollTo({top:0,behavior:'instant'});
    check(boxes.get(styled).y===0,'behavior instant overrides scroll-behavior');

    scroller.scrollTop=0;
    scroller.scrollBy(0,250);
    check(boxes.get(scroller).y===250,'scrollBy(x, y) is relative');
    scroller.scroll({left:7});
    check(boxes.get(scroller).y===250,'missing top keeps the current position');

    const none=box(0,0,0);
    r=await settled(none.scrollTo({top:10}));
    check(r.value.interrupted===false,'no scrolling box: resolved');

    viewport.y=0;
    p=root.scrollTo({top:300});
    check(viewport.y===300,'root element scrolls the viewport');
    boxes.get(root).behavior='smooth';
    p=context.scrollTo({top:900});
    check(viewport.y===300,'Window scroll uses the root element scroll-behavior');
    for(let i=0;i<60&&frames.length;i++)runFrames(16);
    check((await settled(p)).value.interrupted===false&&viewport.y===900,'Window smooth scroll completes');
    boxes.get(root).behavior='auto';
    context.scrollBy(0,100);
    check(viewport.y===1000,'Window scrollBy');
    context.scroll(Infinity,NaN);
    check(viewport.x===0&&viewport.y===0,'non-finite coordinates are 0');

    r=await settled(proto.scroll.call({},{top:1}));
    check(r.state==='rejected'&&r.value.name==='TypeError','Element.scroll on another object rejects');
    r=await settled(context.scrollTo.call({},0,0));
    check(r.state==='rejected'&&r.value.name==='TypeError','Window.scrollTo on another object rejects');
    r=await settled(scroller.scroll({behavior:'fast'}));
    check(r.state==='rejected'&&r.value.name==='TypeError','invalid behavior rejects');
    r=await settled(scroller.scroll(5));
    check(r.state==='rejected','a number is no ScrollToOptions');
    const order=[];
    scroller.scroll({get behavior(){order.push('behavior');return 'auto';},get left(){order.push('left');return 0;},get top(){order.push('top');return 0;}});
    check(order.join()==='behavior,left,top','ScrollToOptions member order');

    /* scrollIntoView performs each box of the native plan, innermost first, then the viewport */
    const inner=box(1,0,3000),outer=box(1,0,3000);
    viewport.y=0;boxes.get(inner).y=0;boxes.get(outer).y=0;
    rawDom.plan=()=>[0,600,inner,0,1000,outer,0,2000];
    p=inner.scrollIntoView({behavior:'smooth',block:'center',inline:'end',container:'all'});
    check(rawDom.lastIntoView.join()==='2,1,false','scrollIntoView passes the alignment');
    check(boxes.get(inner).y===0&&boxes.get(outer).y===0&&viewport.y===0,'smooth scrollIntoView: nothing moved yet');
    for(let i=0;i<60&&frames.length;i++)runFrames(16);
    r=await settled(p);
    check(boxes.get(inner).y===1000&&boxes.get(outer).y===2000&&viewport.y===600&&r.value.interrupted===false,'scrollIntoView: every box reaches the plan');
    boxes.get(inner).y=0;
    p=inner.scrollIntoView({behavior:'smooth'});
    inner.scrollTop=5;
    for(let i=0;i<60&&frames.length;i++)runFrames(16);
    check((await settled(p)).value.interrupted===true,'scrollIntoView: interrupted when one box is');
    rawDom.plan=()=>[];
    check((await settled(inner.scrollIntoView(false))).value.interrupted===false,'scrollIntoView without a box resolves');
    check(rawDom.lastIntoView[0]===1,'scrollIntoView(false) aligns to the end');
    r=await settled(inner.scrollIntoView({block:'middle'}));
    check(r.state==='rejected'&&r.value.name==='TypeError','invalid alignment rejects');
    const intoOrder=[];
    inner.scrollIntoView({get behavior(){intoOrder.push('behavior');return 'instant';},get block(){intoOrder.push('block');return 'center';},
        get container(){intoOrder.push('container');return 'all';},get inline(){intoOrder.push('inline');return 'end';}});
    check(intoOrder.join()==='behavior,block,container,inline','ScrollIntoViewOptions member order');
    for(const [key,value] of [['behavior','SMOOTH'],['block','middle'],['inline','left'],['container','parent'],['block',null],['inline',Symbol()]]){
        rawDom.lastIntoView=null;
        r=await settled(inner.scrollIntoView({[key]:value}));
        check(r.state==='rejected'&&r.value.name==='TypeError'&&rawDom.lastIntoView===null,'invalid '+key+' rejects before native');
    }
    for(const [arg,block] of [[undefined,0],[null,0],[true,0],[false,1],[0,1],['',1],['false',0]]){
        inner.scrollIntoView(arg);check(rawDom.lastIntoView[0]===block&&rawDom.lastIntoView[1]===3&&!rawDom.lastIntoView[2],'boolean or options: '+String(arg));
    }
}

/* ------------------------------------------------------------ MouseEvent */
function mouse(){
    const source=fs.readFileSync(web+'js_bootstrap.js','utf8');
    const start=source.indexOf('    const mouseRecords=new WeakMap();'),end=source.indexOf('    class KeyboardEvent');
    check(start>0&&end>start,'MouseEvent section found');
    const states=new WeakMap();
    class UIEvent{constructor(type,init={}){states.set(this,{type,dispatching:false,target:null});this.view=init.view??null;}
        initUIEvent(){}}
    const target={},doc={};let scrollX=10,scrollY=20;const converted=[];
    const context=vm.createContext({UIEvent,apply:Reflect.apply,eventGet:WeakMap.prototype.get,eventSet:WeakMap.prototype.set,
        mouseAssign:Object.assign,EventTarget:class{},Object,TypeError,WeakMap,
        eventState:e=>{const s=states.get(e);if(!s)throw new TypeError('Illegal Event receiver');return s;},
        host:{scroll:axis=>axis?scrollY:scrollX},
        rawDom:{isNode:(_,n)=>n===target||n===doc,get:(n,name)=>name==='nodeType'?(n===doc?9:1):doc},
        dom:(op,...args)=>{if(op!=='convertPoints')throw new Error(op);converted.push(args);return [args[4][0]-100,args[4][1]-200];}});
    const api=vm.runInContext(source.slice(start,end)+'\n({MouseEvent,mouseNativePosition})',context);
    const {MouseEvent,mouseNativePosition}=api,proto=MouseEvent.prototype;
    for(const name of ['pageX','pageY','x','y','offsetX','offsetY']){
        const d=Object.getOwnPropertyDescriptor(proto,name);
        check(d&&typeof d.get==='function'&&!d.set,'MouseEvent.prototype.'+name+' is a readonly attribute');
    }
    const e=new MouseEvent('click',{clientX:5,clientY:6,pageX:999});
    check(!Object.prototype.hasOwnProperty.call(e,'pageX'),'pageX is not an own property');
    check(e.pageX===15&&e.pageY===26,'pageX/pageY: clientX/Y plus the scroll position');
    check(e.x===5&&e.y===6,'x/y are clientX/clientY');
    check(e.offsetX===15&&e.offsetY===26,'offsetX/offsetY of an event not being dispatched: pageX/pageY');
    scrollX=40;check(e.pageX===45,'pageX of a script event follows the current scroll position');
    mouseNativePosition(e,300,400);
    check(e.pageX===300&&e.pageY===400,'a native event keeps where it occurred');
    const s=states.get(e);s.dispatching=true;s.target=target;
    check(e.offsetX===-95&&e.offsetY===-194,'offsetX/offsetY while dispatched: the target padding edge');
    check(converted[0][0]===target&&converted[0][1]===doc&&converted[0][2]===1&&converted[0][3]===2&&converted[0][5]===true,'offset: client point from the document into the padding box, rendered ancestor allowed');
    s.dispatching=false;
    check(e.offsetX===-95&&converted.length===1,'offset kept after dispatch');
    check(throwsName(()=>Object.getOwnPropertyDescriptor(proto,'pageX').get.call({}),'TypeError'),'another receiver throws');
}

/* ------------------------------------------------------------ GeometryUtils */
async function geometry(){
    class Node{} class Text extends Node{} class Element extends Node{} class Document extends Node{}
    const doc=new Document(),el=new Element(),text=new Text(),other=new Element();
    const types=new Map([[doc,9],[el,1],[text,3],[other,1]]);
    const calls=[];let reply=null;
    const context=vm.createContext({Text,Element,Document,TypeError,RangeError,Symbol,Object,Array,Math,Number,Reflect,WeakMap,WeakSet,Float32Array,Float64Array,
        DOMException:class extends Error{constructor(m,n){super(m);this.name=n;}},
        elementURL:{string:v=>{if(typeof v==='symbol')throw new TypeError('symbol');return String(v);}},
        rawDom:{isNode:(_,n)=>types.has(n),get:(n,name)=>name==='nodeType'?types.get(n):name==='ownerDocument'?doc:undefined},
        dom:(op,...args)=>{calls.push([op,...args]);return typeof reply==='function'?reply(op,...args):reply;}});
    const geometrySource=fs.readFileSync(web+'js_geometry.js','utf8'),utils=fs.readFileSync(web+'js_geometry_utils.js','utf8');
    vm.runInContext(geometrySource+'\nconst geometryBridge=geometryInterfaces({parse:null,list:null});\n'+utils+'\nthis.bridge=geometryBridge;',context);
    const bridge=context.bridge;
    for(const proto of [Text.prototype,Element.prototype,Document.prototype])
        for(const name of ['getBoxQuads','convertQuadFromNode','convertRectFromNode','convertPointFromNode'])
            check(typeof proto[name]==='function','GeometryUtils '+name+' on '+proto.constructor.name);
    reply=[0,0,10,0,10,5,0,5, 1,2,3,4,5,6,7,8];
    let quads=el.getBoxQuads();
    check(Array.isArray(quads)&&quads.length===2,'getBoxQuads: one DOMQuad per fragment');
    check(calls.at(-1)[0]==='boxQuads'&&calls.at(-1)[1]===el&&calls.at(-1)[2]===1&&calls.at(-1)[3]===doc,'getBoxQuads: border box relative to the node document');
    check(quads[0] instanceof context.DOMQuad&&quads[0].p3.x===10&&quads[0].p3.y===5&&quads[1].p4.y===8,'getBoxQuads: the native points');
    el.getBoxQuads({box:'content',relativeTo:other});
    check(calls.at(-1)[2]===3&&calls.at(-1)[3]===other,'getBoxQuads: box and relativeTo');
    doc.getBoxQuads();
    check(calls.at(-1)[1]===doc&&calls.at(-1)[3]===doc,'Document.getBoxQuads is relative to itself');
    check(throwsName(()=>el.getBoxQuads({box:'outline'}),'TypeError'),'invalid CSSBoxType');
    check(throwsName(()=>el.getBoxQuads({relativeTo:{}}),'TypeError'),'relativeTo must be a GeometryNode');
    check(throwsName(()=>Element.prototype.getBoxQuads.call(text),'TypeError'),'Element.getBoxQuads brand');
    reply=1;check(throwsName(()=>el.getBoxQuads({relativeTo:other}),'NotFoundError'),'unrendered relativeTo: NotFoundError');
    reply=2;check(throwsName(()=>el.getBoxQuads({relativeTo:other}),'SecurityError'),'cross-origin documents: SecurityError');
    reply=(op,node,from,fromBox,toBox,points)=>points.map(v=>v+1);
    const point=el.convertPointFromNode({x:3,y:4,z:9,w:2},other,{fromBox:'padding',toBox:'margin'});
    check(point.x===4&&point.y===5&&point.z===0&&point.w===1,'convertPointFromNode: a 2D DOMPoint');
    check(calls.at(-1)[2]===other&&calls.at(-1)[3]===2&&calls.at(-1)[4]===0&&calls.at(-1)[6]===false,'convertPointFromNode: boxes');
    const quad=el.convertQuadFromNode({p1:{x:1,y:1},p2:{x:2,y:1},p3:{x:2,y:2},p4:{x:1,y:2}},doc);
    check(quad.p2.x===3&&quad.p4.y===3,'convertQuadFromNode converts every point');
    const rect=el.convertRectFromNode(new bridge.DOMRectReadOnly(1,2,3,4),text);
    check(rect.p1.x===2&&rect.p3.x===5&&rect.p3.y===7,'convertRectFromNode converts the rectangle corners');
    check(throwsName(()=>el.convertRectFromNode({x:1,y:2,width:3,height:4},text),'TypeError'),'convertRectFromNode needs a DOMRectReadOnly');
    check(throwsName(()=>el.convertPointFromNode({x:1}),'TypeError'),'two arguments required');
    check(throwsName(()=>el.convertPointFromNode({x:1},{}),'TypeError'),'from must be a GeometryNode');
    reply=1;check(throwsName(()=>el.convertPointFromNode({x:1},other),'NotFoundError'),'unrendered from: NotFoundError');
}

(async()=>{
    await scrolling();
    mouse();
    await geometry();
    console.log(`cssom view contract: ${passed} passed, ${failed} failed`);
    if(failed)process.exitCode=1;
})().catch(e=>{console.log('ERROR',e&&e.stack||e);process.exitCode=1;});
