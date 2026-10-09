/* Exact product bridge regression with native query/version counting stubs. */
const fs=require('node:fs'),path=require('node:path');
const source=name=>fs.readFileSync(path.join(__dirname,'../user/libc/web',name),'utf8');
const known=new WeakSet();let checks=0,reads=0;
const equal=(actual,expected,label)=>{checks++;if(!Object.is(actual,expected))throw Error(label+': '+String(actual)+' != '+String(expected));};
const rejects=(fn,label)=>{let error;try{fn();}catch(e){error=e;}equal(error instanceof TypeError,true,label);};
class NativeNode {constructor(type=1,tag='',namespace='http://www.w3.org/1999/xhtml'){known.add(this);this.type=type;this.tag=tag;this.namespace=namespace;this.attrs={};}}
class NativeDocument extends NativeNode {constructor(){super(9);this.owner=this;this.revision=1n;this.nodes=[];this.queryReads=0;}}
NativeDocument.prototype.createEvent=()=>({});
const doc=new NativeDocument(),other=new NativeDocument();
const matches=(n,query)=>{
    if(query==='a[href],area[href]')return (n.tag==='a'||n.tag==='area') && Object.hasOwn(n.attrs,'href');
    if(query==='a[name]')return n.tag==='a' && Object.hasOwn(n.attrs,'name');
    return n.tag===query;
};
const rawDom=(op,n,key)=>{
    if(!known.has(n))throw new TypeError('Expected native node');
    if(op==='get'){
        if(key==='nodeType')return n.type;
        if(key==='namespaceURI')return n.namespace;
        if(key==='collectionVersion')return [n.owner,n.owner.revision];
    }
    if(op==='query'){reads++;n.queryReads++;return n.nodes.filter(x=>matches(x,key));}
    throw Error('Unexpected native operation '+op+' '+key);
};
Object.defineProperty(NativeNode.prototype,'nodeType',{get(){return rawDom('get',this,'nodeType');}});
const reflectedAttr=(n,key)=>Object.hasOwn(n.attrs,key)?n.attrs[key]:null;
new Function('rawDom','dom','reflectedAttr','Node','Document','document','CSS','host','apply',
    source('js_collections.js')+source('js_document.js'))(rawDom,rawDom,reflectedAttr,NativeNode,NativeDocument,doc,
        {escape:String},{ready:()=> 'complete'},Reflect.apply);
const add=(tag,attrs={},namespace)=>{const n=new NativeNode(1,tag,namespace);n.owner=doc;n.attrs=attrs;doc.nodes.push(n);return n;};
const a=add('a',{href:'#a',id:'link-id',name:'link-name'}),area=add('area',{href:'',name:'area-name'});
const empty=add('a',{name:''}),image=add('img',{id:'image-id'}),script=add('script',{type:'application/json',name:'script-name'}),embed=add('embed',{name:'embed-name'});
add('input',{type:'image'});add('object');add('link',{href:'#css'});
for(const tag of ['a','area','img','script','embed'])add(tag,{href:'#svg',name:'foreign'},'http://www.w3.org/2000/svg');
const lists={};for(const key of ['links','images','scripts','embeds','anchors']){
    lists[key]=doc[key];equal(lists[key],doc[key],key+' same object');equal(lists[key] instanceof HTMLCollection,true,key+' HTMLCollection');
    const get=Object.getOwnPropertyDescriptor(NativeDocument.prototype,key).get;
    rejects(()=>get.call({type:9}),key+' forged receiver');rejects(()=>get.call(a),key+' Element receiver');
    rejects(()=>get.call(Object.create(NativeDocument.prototype)),key+' prototype forged receiver');
}
equal(lists.links.length,2,'links only a/area href');equal(lists.links[0],a,'links order');equal(lists.links[1],area,'empty href');
equal(lists.links.namedItem('link-id'),a,'named id');equal(lists.links['link-name'],a,'named name');
for(let i=0;i<100;i++){equal(doc.links.length,2,'cached length');equal(doc.links[i%2],i%2?area:a,'cached index');}
equal(doc.queryReads,1,'one query for repeated length/index');
equal(lists.images.length,1,'images img only');equal(lists.images[0],image,'image identity');
equal(lists.scripts.length,1,'scripts HTML even non-executable');equal(lists.scripts[0],script,'script identity');
equal(lists.embeds.length,1,'embeds no object');equal(lists.embeds[0],embed,'embed identity');
equal(doc.plugins,lists.embeds,'plugins same native embed collection');
Object.defineProperty(doc,'embeds',{configurable:true,value:'public override'});equal(doc.plugins,lists.embeds,'plugins ignores override');delete doc.embeds;
equal(lists.anchors.length,2,'anchors name presence');equal(lists.anchors[1],empty,'empty named anchor');
delete a.attrs.href;doc.revision++;equal(lists.links.length,1,'href removal invalidates');
empty.attrs.href='';doc.revision++;equal(lists.links.length,2,'href insertion invalidates');equal(lists.links[1],empty,'href insertion order');
area.attrs.name='area-renamed';doc.revision++;equal(lists.links['area-name'],undefined,'old name gone');equal(lists.links['area-renamed'],area,'new name live');
other.revision=doc.revision;const otherLink=new NativeNode(1,'a');otherLink.attrs={href:'#other',id:'other-id'};otherLink.owner=other;other.nodes=[otherLink];
equal(other.links.length,1,'equal revision different Document');equal(other.links[0],otherLink,'different Document native node');
equal(other.links===doc.links,false,'different Document collection object');equal(doc.links.length,2,'other Document does not replace snapshot');
other.nodes=[];other.revision++;equal(other.links.length,0,'other Document mutation');equal(doc.links.length,2,'owner revision independent');
const forms=doc.forms;add('form',{name:'form-name'});doc.revision++;equal(doc.forms,forms,'forms retained object');equal(forms.length,1,'forms retained live read');
const pluginsGet=Object.getOwnPropertyDescriptor(NativeDocument.prototype,'plugins').get;rejects(()=>pluginsGet.call({}),'plugins Document brand');
console.log('document collections host: '+checks+' checks, 0 failed; native queries '+reads);
