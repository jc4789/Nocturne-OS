function runErrorEventCases() {
    let count = 0;
    const eq = (a,b,name) => { count++; check('error-event-'+name,Object.is(a,b)); if(!Object.is(a,b))throw new Error(name); };
    const throws = (fn,name) => { let error; try { fn(); } catch(e) { error=e; } eq(error instanceof TypeError,true,name); };
    const blank = new ErrorEvent('error');
    eq(blank instanceof Event,true,'inherits-event'); eq(blank.type,'error','type');
    eq(blank.message,'','default-message'); eq(blank.filename,'','default-filename');
    eq(blank.lineno,0,'default-line'); eq(blank.colno,0,'default-column'); eq(blank.error,undefined,'default-error');
    const token={}, values=new ErrorEvent('error',{message:42,filename:'x\ud800y\ud83d\ude00',lineno:-1,colno:4294967298,error:token,cancelable:true});
    eq(values.message,'42','message-conversion'); eq(values.filename,'x\ufffdy\ud83d\ude00','filename-scalars');
    eq(values.lineno,4294967295,'line-modulo'); eq(values.colno,2,'column-modulo'); eq(values.error,token,'error-identity');
    eq(values.cancelable,true,'event-init'); values.preventDefault(); eq(values.defaultPrevented,true,'cancelable');
    eq(Object.prototype.toString.call(values),'[object ErrorEvent]','tag');
    const descriptor=Object.getOwnPropertyDescriptor(ErrorEvent.prototype,'message');
    eq(descriptor.enumerable,true,'enumerable'); eq(descriptor.set,undefined,'readonly');
    throws(()=>descriptor.get.call(Object.create(ErrorEvent.prototype)),'forged');
    throws(()=>descriptor.get.call(new Proxy(values,{})),'proxy');
    throws(()=>new ErrorEvent(),'required-type'); throws(()=>ErrorEvent('error'),'requires-new');
    throws(()=>new ErrorEvent(Symbol()),'symbol-type'); throws(()=>new ErrorEvent('error',{message:Symbol()}),'symbol-message');
    throws(()=>new ErrorEvent('error',{lineno:1n}),'bigint-line'); throws(()=>new ErrorEvent('error',7),'primitive-dictionary');
    eq(new ErrorEvent('error',null).message,'','null-dictionary');
    const finite=new ErrorEvent('error',{colno:Infinity,lineno:NaN});eq(finite.colno,0,'infinite');eq(finite.lineno,0,'nan');
    class Derived extends ErrorEvent {} const derived=new Derived('error',{error:null});
    eq(Object.getPrototypeOf(derived),Derived.prototype,'derived');eq(derived.error,null,'null-error');
    let conversions=0; const type={toString(){conversions++;return 'error';}};
    new ErrorEvent(type);eq(conversions,1,'single-type-conversion');
    return count;
}
