/* Exact constructor source with a branded native-bridge stub. */
const fs=require('node:fs');
const source=fs.readFileSync('user/libc/web/js_bootstrap.js','utf8');
const start=source.indexOf('    class DocumentFragment extends Node {'),end=source.indexOf('    class DocumentType extends Node {',start);
if(start<0||end<0)throw Error('Product constructor not found');
let checks=0,creates=0,expectedOwner;
const eq=(a,b,label)=>{checks++;if(!Object.is(a,b))throw Error(label);};
class Node {constructor(){throw new TypeError('Illegal Node constructor');}}
const branded=new WeakSet();
function realm(document){
    let Fragment;
    const dom=(op,owner,type,name,text)=>{
        eq(op,'create','native create');eq(owner,expectedOwner,'associated document');eq(type,11,'fragment type');
        eq(name,'#document-fragment','native name');eq(text,'','empty data');
        creates++;const n=Object.create(Fragment.prototype);branded.add(n);return n;
    };
    Fragment=new Function('Node','dom','document',source.slice(start,end)+'return DocumentFragment;')(Node,dom,document);
    return Fragment;
}
const document={},child={},Fragment=realm(document),ChildFragment=realm(child);
expectedOwner=document;
const f=new Fragment();eq(branded.has(f),true,'native brand');eq(f instanceof Node,true,'native inheritance');
eq(Object.getPrototypeOf(f),Fragment.prototype,'native prototype');eq(new Fragment()===f,false,'distinct fragments');
const ignored=new Fragment({toString(){throw Error('argument converted');}});eq(branded.has(ignored),true,'arguments ignored');
class Derived extends Fragment {constructor(){super();this.extra=42;}}
const derived=new Derived();eq(branded.has(derived),true,'derived native brand');eq(derived.extra,42,'derived constructor');eq(Object.getPrototypeOf(derived),Derived.prototype,'derived prototype');
function Alternate(){}const other=Reflect.construct(Fragment,[],Alternate);eq(branded.has(other),true,'reflect native brand');eq(Object.getPrototypeOf(other),Alternate.prototype,'reflect prototype');
function NullPrototype(){}NullPrototype.prototype=null;
const fallback=Reflect.construct(Fragment,[],NullPrototype);eq(Object.getPrototypeOf(fallback),Fragment.prototype,'native fallback prototype');
expectedOwner=child;const fromParentCall=new ChildFragment();eq(branded.has(fromParentCall),true,'child native brand');eq(Object.getPrototypeOf(fromParentCall),ChildFragment.prototype,'child associated realm');
let error;try{Fragment();}catch(e){error=e;}eq(error instanceof TypeError,true,'requires new');
eq(creates,7,'native create count');console.log('fragment constructor host: '+checks+' checks, 0 failed');
