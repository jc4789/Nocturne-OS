/* White-box supporting regression: execute the unmodified product bridge and
 * count native-tree reads. Actual DOM ownership is covered by jstest below. */
const fs=require('node:fs'),path=require('node:path');
const source=fs.readFileSync(path.join(__dirname,'../user/libc/web/js_collections.js'),'utf8');
let checks=0,reads=0,versions=0;
const equal=(actual,expected,label)=>{checks++;if(!Object.is(actual,expected))throw Error(label+': '+String(actual)+' != '+String(expected));};
const ownerA={revision:1n},ownerB={revision:1n};
const node=(type=1,owner=ownerA,id='')=>({type,owner,id,name:'',children:[]});
const root=node(),one=node(1,ownerA,'one'),two=node(1,ownerA,'two'),text=node(3);
root.children=[one,two,text];
const rawDom=(op,n,key)=>{
    if(op!=='get')throw Error('Unexpected operation');
    if(key==='collectionVersion'){versions++;return [n.owner,n.owner.revision];}
    if(key==='childNodes'){reads++;return n.children.slice();}
    if(key==='nodeType')return n.type;
    if(key==='namespaceURI')return 'http://www.w3.org/1999/xhtml';
    throw Error('Unexpected property '+key);
};
const bridge=new Function('rawDom','reflectedAttr',source+';return collectionBridge;')(rawDom,(n,key)=>n[key]||null);
const query=()=>{reads++;return root.children.filter(n=>n.type===1);};
const live=bridge.domHTML(root,query);
equal(live.length,2,'initial length');equal(live[0],one,'initial index');equal(live.item(1),two,'item');
equal(live.namedItem('one'),one,'named item');equal(live.one,one,'named property');equal('1' in live,true,'has index');
equal(Object.getOwnPropertyDescriptor(live,'0').value,one,'descriptor');equal(Object.keys(live).join(','),'0,1','keys');
equal(Array.from(live).length,2,'iterator');equal(reads,1,'all reads share one snapshot');
for(let i=0;i<100;i++){equal(live.length,2,'repeated length');equal(live[i%2],i%2?two:one,'repeated index');}
equal(reads,1,'length/index loop does not repeat query');
const three=node(1,ownerA,'three');root.children.push(three);ownerA.revision++;
equal(live.length,3,'tree mutation live');equal(live[2],three,'new index');equal(reads,2,'one new query');
one.id='renamed';ownerA.revision++;
equal(live.one,undefined,'old named property gone');equal(live.renamed,one,'new named property live');equal(reads,3,'attribute revision invalidates');
root.children.splice(1,1);ownerA.revision++;
equal(live.item(1),three,'remove updates order');root.children.unshift(two);ownerA.revision++;
equal(live[0],two,'reinsert updates order');
// Adoption into another owner with exactly the SAME counter must invalidate.
ownerB.revision=ownerA.revision;const beforeAdopt=reads;root.owner=ownerB;root.children=[two];
equal(live.length,1,'owner identity invalidates');equal(reads,beforeAdopt+1,'adoption read with equal revisions');
root.children.push(one);ownerA.revision++;
equal(live.length,1,'other document revision is not the owner revision');
ownerB.revision++;equal(live.length,2,'actual owner mutation invalidates');
// Avoid counter rounding above Number.MAX_SAFE_INTEGER.
ownerB.revision=9007199254740992n;equal(live.length,2,'large revision');
root.children.push(three);ownerB.revision++;equal(live.length,3,'adjacent uint64 revisions distinct');
const nodes=bridge.children(root,false),elements=bridge.children(root,true);
equal(nodes,bridge.children(root,false),'same childNodes object');equal(elements,bridge.children(root,true),'same children object');
root.children.push(text);ownerB.revision++;const beforeChildren=reads;
equal(nodes.length,4,'childNodes text included');equal(nodes[3],text,'childNodes text index');equal(nodes.item(0),two,'childNodes item');
equal(elements.length,3,'children elements only');equal(elements[2],three,'children index');
equal(reads,beforeChildren+2,'one read per cached children type');
// A replaced Array helper must not receive the persistent native snapshot.
const savedFind=Array.prototype.find,savedMap=Array.prototype.map,savedFilter=Array.prototype.filter;
try{
    Array.prototype.find=function(){throw Error('author find saw backing array');};
    equal(live.namedItem('two'),two,'captured find');
    Array.prototype.map=function(){throw Error('author map saw backing array');};
    equal(Object.keys(live).length,3,'captured map');
    Array.prototype.filter=function(){throw Error('author filter saw backing array');};
    ownerB.revision++;equal(elements.length,3,'captured child filter');
}finally{Array.prototype.find=savedFind;Array.prototype.map=savedMap;Array.prototype.filter=savedFilter;}
const nodeList=bridge.domLive(root,()=>{reads++;return root.children.slice();});
let visited=[];nodeList.forEach((n,i)=>{visited.push(n);if(i===0){root.children.splice(1,1);ownerB.revision++;}});
equal(visited.length,3,'forEach skips vanished last index');equal(visited[1],three,'forEach mutation live');
const iter=nodeList.values();equal(iter.next().value,two,'iterator first');
root.children.unshift(one);ownerB.revision++;equal(iter.next().value,two,'iterator sees shifted native index');
let attempts=0;const retry=bridge.domLive(root,()=>{if(++attempts===1)throw Error('read failed');return [one];});
let threw=false;try{void retry.length;}catch(e){threw=e.message==='read failed';}
equal(threw,true,'read failure propagated');equal(retry.length,1,'read failure not cached');equal(retry[0],one,'retry index');equal(attempts,2,'successful retry reused');
const revisionFailure=bridge.domLive(null,()=>{throw Error('reader must not run');});
threw=false;try{void revisionFailure.length;}catch(e){threw=true;}equal(threw,true,'native version failure propagated');
// Generic readers depend on non-DOM state; no cache may be imposed on them.
let current=[one],genericReads=0;const generic=()=>{genericReads++;return current;};
for(const build of [bridge.live,bridge.html,r=>bridge.typed(r,NodeList.prototype),
    r=>bridge.options(r,HTMLCollection.prototype,()=>{}),r=>bridge.form(r,HTMLCollection.prototype,()=>null)]){
    current=[one];const list=build(generic),before=genericReads;equal(list.length,1,'generic length');
    current=[two,three];equal(list[0],two,'generic state change without DOM revision');equal(list.length,2,'generic new length');
    equal(genericReads,before+3,'generic read remains uncached');
}
const staticList=bridge.list([one,two]);root.children=[];ownerB.revision++;
equal(staticList.length,2,'static list unchanged');equal(staticList[1],two,'static list identity');
console.log('collections host: '+checks+' checks, 0 failed; native snapshot reads '+reads+', version checks '+versions);
