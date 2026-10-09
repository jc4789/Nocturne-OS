/* 次25の新diagnostic／DOMException serialization境界のみ。 */
const check25=(name,ok)=>__check(name,!!ok),error25=fn=>{try{fn();return null;}catch(e){return e;}};
let original25,copy25,side25=0;
globalThis.__suite=[
 {steps:0,run(){
    const defaults=new DOMException();check25('exception-default-fields',defaults.name==='Error'&&defaults.message===''&&defaults.code===0);
    check25('exception-symbol-name-domstring',error25(()=>new DOMException('',Symbol())).name==='TypeError');
    const getter=Object.getOwnPropertyDescriptor(DOMException.prototype,'name').get;
    check25('exception-native-private-brand-getter',error25(()=>getter.call({})).name==='TypeError');
    original25=new DOMException('fixed-fixture-message','AbortError');
    Object.defineProperty(original25,'name',{get(){side25++;throw new Error('author getter');},configurable:true});
    Object.defineProperty(original25,'message',{get(){side25++;throw new Error('author getter');},configurable:true});
    original25.authorFunction=()=>{};
    copy25=structuredClone({first:original25,again:original25});
    check25('domexception-real-clone-brand',copy25.first instanceof DOMException&&copy25.first instanceof Error);
    check25('domexception-clone-preserves-name-message-code',copy25.first.name==='AbortError'&&copy25.first.message==='fixed-fixture-message'&&copy25.first.code===20);
    check25('domexception-repeated-reference',copy25.first===copy25.again&&copy25.first!==original25);
    check25('domexception-internal-slots-not-author-getters',side25===0);
    check25('domexception-does-not-clone-author-properties',!Object.hasOwn(copy25.first,'authorFunction'));
    const e=new DOMException('fixture','QuotaExceededError');postMessage({e});
    check25('exception-wire-record-is-data-not-host-object',__packet[1].some(r=>r[0]==='DOMException'&&r[1][0]==='QuotaExceededError'));
    const ordinary=structuredClone(new TypeError('ordinary'));check25('ordinary-error-path-stays-error',ordinary instanceof TypeError&&!(ordinary instanceof DOMException));
    let reject=error25(()=>structuredClone(Promise.resolve()));
    check25('diagnostic-real-promise-brand-and-stage',reject.message.includes('stage unknown-native-class, brand Promise, nativeclass '));
    check25('diagnostic-local-transfer-context',reject.message.includes('transfers 0, external no'));
    const p=new MessageChannel();reject=error25(()=>postMessage(null,[p.port1]));
    check25('diagnostic-external-port-stage-and-private-brand',reject.message.includes('stage external-transfer-unsupported, brand MessagePort'));
    check25('diagnostic-external-port-transfer-context',reject.message.includes('transfers 1, external yes'));
    // 未実行補正：enumerable constructorは通常のclone walk自体が読む。
    const privateValue={secret:'must-not-be-logged'};
    Object.defineProperty(privateValue,'constructor',{get(){side25++;throw new Error('constructor must not be inspected');},configurable:true});
    privateValue.f=function authorSecret(){};reject=error25(()=>structuredClone(privateValue));
    check25('diagnostic-no-author-value-key-constructor-name',!reject.message.includes('secret')&&!reject.message.includes('authorSecret')&&side25===0);
    const beforeProxy=side25,proxy=new Proxy({},{get(){side25++;throw new Error('proxy trap');},ownKeys(){side25++;throw new Error('proxy trap');}});
    reject=error25(()=>structuredClone(proxy));check25('diagnostic-proxy-class-without-traps',reject.message.includes('brand Proxy')&&side25===beforeProxy);
 }}
];
