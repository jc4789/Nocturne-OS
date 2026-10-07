/* Run only in the isolated Nocturne storage fixture origin, not a user's site. */
function runStorageCases(){
    let checks=0;
    function check(value,message){checks++;if(!value)throw new Error('Storage: '+message);}
    function throws(fn,name,message){let error;try{fn();}catch(e){error=e;}check(error && error.name===name,message);}
    const C=Storage, proto=C.prototype, local=localStorage,session=sessionStorage;
    const get=proto.getItem,set=proto.setItem,clear=proto.clear,remove=proto.removeItem,key=proto.key;
    check(local===localStorage && session===sessionStorage,'same window objects');
    check(local!==session && local instanceof C && session instanceof C,'separate branded areas');
    check(Object.prototype.toString.call(local)==='[object Storage]','toStringTag');
    throws(()=>new C(),'TypeError','illegal constructor');
    throws(()=>get.call({},'x'),'TypeError','get brand');
    throws(()=>set.call(Object.create(local),'x','y'),'TypeError','inherited fake brand');
    throws(()=>Object.getOwnPropertyDescriptor(proto,'length').get.call({}),'TypeError','length brand');
    throws(()=>Object.getOwnPropertyDescriptor(globalThis,'localStorage').get.call({}),'TypeError','Window getter brand');
    clear.call(local);clear.call(session);
    check(local.length===0 && local.key(0)===null,'initial clear');
    check(get.call(local,'missing')===null && local.missing===undefined && !('missing' in local),'absent semantics');
    set.call(local,'first','one');set.call(local,'second','two');
    check(local.length===2 && local.key(0)==='first' && local.key(1)==='second','live insertion order');
    set.call(local,'first','changed');check(local.length===2 && local.first==='changed' && key.call(local,0)==='first','replace retains order');
    remove.call(local,'first');check(local.length===1 && key.call(local,0)==='second','remove exposes new live key');
    local.named=17;check(get.call(local,'named')==='17' && local.named==='17','named setter DOMString');
    const descriptor=Object.getOwnPropertyDescriptor(local,'named');
    check(descriptor && descriptor.value==='17' && descriptor.enumerable && descriptor.writable && descriptor.configurable,'virtual descriptor');
    check(Object.keys(local).join('|')==='second|named','live ownKeys');
    delete local.named;check(get.call(local,'named')===null && !Object.hasOwn(local,'named'),'named delete');
    local.getItem='hidden';check(typeof local.getItem==='function' && get.call(local,'getItem')==='hidden','setter retains builtin precedence');
    check(!Object.keys(local).includes('getItem'),'hidden builtin key not enumerated');
    delete local.getItem;check(get.call(local,'getItem')==='hidden','hidden named delete does not remove');
    local.length='hidden-length';check(typeof local.length==='number' && get.call(local,'length')==='hidden-length','readonly builtin precedence');
    local['__proto__']='data';check(Object.getPrototypeOf(local)===proto && get.call(local,'__proto__')==='data','no prototype poisoning');
    const symbol=Symbol('ordinary');local[symbol]=19;
    check(local[symbol]===19 && get.call(local,String(symbol))===null,'symbol expando not storage');
    Object.defineProperty(local,'defined',{value:'yes',configurable:true});
    check(get.call(local,'defined')==='yes' && local.defined==='yes','define named value');
    check(!Reflect.defineProperty(local,'accessor',{get(){return 'no';},configurable:true}) && get.call(local,'accessor')===null,'accessor rejection');
    check(!Reflect.preventExtensions(local) && Object.isExtensible(local),'legacy object extensible');
    const child=Object.create(local);child.ordinary='child';
    check(child.ordinary==='child' && get.call(local,'ordinary')===null,'different receiver ordinary set');
    check(get.call(session,'second')===null,'session/local isolation');set.call(session,'second','session');
    check(get.call(session,'second')==='session' && get.call(local,'second')==='two','separate values');
    const nulKey='a\0b', text='\ud800\0\udc00日本語\ud83d\ude00\uffff';
    set.call(local,nulKey,text);check(get.call(local,nulKey)===text,'embedded NUL and lone surrogate exact');
    set.call(local,'','');check(get.call(local,'')==='' && local['']==='','empty key/value not missing');
    set.call(local,null,undefined);check(get.call(local,'null')==='undefined','null undefined DOMString');
    let conversion=[];
    set.call(local,{toString(){conversion.push('key');return 'convert';}},{toString(){conversion.push('value');return 'ok';}});
    check(conversion.join('|')==='key|value' && get.call(local,'convert')==='ok','conversion order');
    throws(()=>get.call(local,Symbol()),'TypeError','Symbol key');
    throws(()=>set.call(local,'symbol',Symbol()),'TypeError','Symbol value');
    check(get.call(local,'symbol')===null,'conversion failure does not mutate');
    throws(()=>get.call(local),'TypeError','missing key');throws(()=>set.call(local,'x'),'TypeError','missing value');
    throws(()=>key.call(local),'TypeError','missing index');throws(()=>key.call(local,1n),'TypeError','BigInt index');
    check(key.call(local,-1)===null && key.call(local,4294967296)===key.call(local,0),'unsigned index');
    const oldURL=URL, oldStorage=globalThis.Storage, oldException=globalThis.DOMException;
    const originDescriptor=Object.getOwnPropertyDescriptor(oldURL.prototype,'origin');
    try{
        globalThis.URL=function(){throw new Error('public URL used');};globalThis.Storage=function(){};
        globalThis.DOMException=function(){throw new Error('public exception used');};
        Object.defineProperty(oldURL.prototype,'origin',{configurable:true,get(){return 'https://wrong.invalid';}});
        set.call(local,'private','ok');check(get.call(local,'private')==='ok' && local instanceof C,'public replacement cannot redirect');
        throws(()=>set.call(local,'private','x'.repeat(5*1024*1024)),'QuotaExceededError','quota failure');
        check(get.call(local,'private')==='ok','quota preserves old value');
    }finally{globalThis.URL=oldURL;globalThis.Storage=oldStorage;globalThis.DOMException=oldException;Object.defineProperty(oldURL.prototype,'origin',originDescriptor);}
    clear.call(local);clear.call(session);
    check(local.length===0 && session.length===0 && local[symbol]===19,'clear removes native pairs not symbols');
    delete local[symbol];return checks;
}
