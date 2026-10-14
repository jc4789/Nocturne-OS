/* 製品の native Wasm と Promise / Fetch 接続を確かめる補助回帰試験。
 * 実サイト受入の代用ではない。section / opcode / 64KiB page は
 * WebAssembly Core binary format に由来する値。 */
const wasmCaseBinary=(()=>{
    const leb=value=>{const out=[];do{const part=value%128;value=Math.floor(value/128);out.push(part|(value?128:0));}while(value);return out;};
    const utf8=value=>{const bytes=Array.from(new TextEncoder().encode(value));return [...leb(bytes.length),...bytes];};
    const vector=items=>[...leb(items.length),...items.flat()];
    const section=(id,bytes)=>[id,...leb(bytes.length),...bytes];
    const binary=(...sections)=>new Uint8Array([0,97,115,109,1,0,0,0,...sections.flat()]);
    const type=(params,results)=>[0x60,...vector(params.map(x=>[x])),...vector(results.map(x=>[x]))];
    const exportEntry=(name,kind,index)=>[...utf8(name),kind,...leb(index)];
    const body=code=>{const bytes=[0,...code,0x0b];return [...leb(bytes.length),...bytes];};
    const single=(params,results,code,extra=[])=>binary(
        section(1,vector([type(params,results)])),section(3,vector([[0]])),...extra,
        section(7,vector([exportEntry('run',0,0)])),section(10,vector([body(code)])));
    return {leb,utf8,vector,section,binary,type,exportEntry,body,single};
})();
globalThis.runWasmCases = async function runWasmCases() {
    let checks=0;
    const assert=(ok,name)=>{if(!ok)throw new Error(name);checks++;};
    const throws=(fn,name,label)=>{let error;try{fn();}catch(e){error=e;}assert(error&&error.name===name,label);return error;};
    const rejects=async(fn,name,label)=>{
        let promise;try{promise=fn();}catch(e){throw new Error(label+': synchronous throw '+e);}
        assert(promise instanceof Promise,label+' Promise');
        let error;try{await promise;}catch(e){error=e;}assert(error&&error.name===name,label);return error;
    };
    const {leb,utf8,vector,section,binary,type,exportEntry,body,single}=wasmCaseBinary;
    const I32=0x7f,I64=0x7e;
    const add=single([I32,I32],[I32],[0x20,0,0x20,1,0x6a]);
    const module=new WebAssembly.Module(add),instance=new WebAssembly.Instance(module);
    assert(instance.exports.run(20,22)===42,'native i32 execution');
    assert(instance.exports.run(0x7fffffff,1)===-2147483648,'i32 wraparound');
    assert(instance.exports===instance.exports,'exports stable identity');
    assert(Object.getPrototypeOf(instance.exports)===null&&Object.isFrozen(instance.exports),'exports null prototype and frozen');
    const exported=instance.exports.run;
    assert(exported.call({notAnInstance:true},8,9)===17,'exported function receiver independent');
    assert(WebAssembly.validate(add)&&!WebAssembly.validate(new Uint8Array([0,97,115])),'native validation');
    throws(()=>new WebAssembly.Module(new Uint8Array([0])),'CompileError','decode failure');
    throws(()=>new WebAssembly.Module(single([],[],[0x41,0])),'CompileError','type validation failure');
    for(const value of [undefined,null,{},'wasm',[],1]){
        throws(()=>WebAssembly.validate(value),'TypeError','validate BufferSource');
        await rejects(()=>WebAssembly.compile(value),'TypeError','compile BufferSource');
    }
    for(const fn of [WebAssembly.Module.exports,WebAssembly.Module.imports])throws(()=>fn({}),'TypeError','Module static brand');
    assert(WebAssembly.Module.exports(module).map(x=>x.name+':'+x.kind).join()==='run:function','export descriptors');
    assert(WebAssembly.Module.imports(module).length===0,'empty imports');
    const custom=new WebAssembly.Module(binary(section(0,[...utf8('payload'),11,22,33])));
    const first=WebAssembly.Module.customSections(custom,'payload');
    assert(first.length===1&&new Uint8Array(first[0]).join()==='11,22,33','custom section bytes');
    new Uint8Array(first[0])[0]=99;
    assert(new Uint8Array(WebAssembly.Module.customSections(custom,'payload')[0])[0]===11,'custom section independent copy');

    const memory=new WebAssembly.Memory({initial:1,maximum:3}),old=memory.buffer;
    assert(memory.buffer===old&&old.byteLength===65536,'memory page size and buffer identity');
    new Uint8Array(old)[12]=83;
    assert(memory.grow(1)===1,'memory grow previous size');
    assert(old.byteLength===0&&memory.buffer.byteLength===2*65536&&new Uint8Array(memory.buffer)[12]===83,'memory grow detach and preserve');
    const beforeZero=memory.buffer;
    assert(memory.grow(0)===2&&beforeZero.byteLength===0,'zero grow detaches old buffer');
    throws(()=>memory.grow(2),'RangeError','memory maximum');
    throws(()=>new WebAssembly.Memory({initial:2,maximum:1}),'RangeError','memory invalid limits');
    const table=new WebAssembly.Table({element:'anyfunc',initial:1,maximum:3});
    assert(table.length===1&&table.get(0)===null,'function table initial');
    table.set(0,exported);assert(table.get(0)===exported,'table function identity');
    assert(table.grow(1,exported)===1&&table.length===2&&table.get(1)===exported,'table growth initializer');
    throws(()=>table.set(0,()=>1),'TypeError','table rejects JS function');
    throws(()=>table.get(2),'RangeError','table bounds');
    throws(()=>table.grow(2),'RangeError','table maximum');
    const reference={value:'external'},refs=new WebAssembly.Table({element:'externref',initial:1});
    refs.set(0,reference);assert(refs.get(0)===reference,'externref identity');
    const counter=new WebAssembly.Global({value:'i32',mutable:true},7);
    counter.value=4294967295;assert(counter.value===-1&&counter.valueOf()===-1,'global mutable conversion');
    counter.value=7;
    const immutable=new WebAssembly.Global({value:'i32'},5);
    throws(()=>{immutable.value=6;},'TypeError','immutable global');
    const wide=new WebAssembly.Global({value:'i64',mutable:true},9n);
    assert(wide.value===9n,'i64 global');throws(()=>{wide.value=9;},'TypeError','i64 BigInt required');

    const imported=binary(
        section(1,vector([type([I32],[I32])])),
        section(2,vector([
            [...utf8('env'),...utf8('inc'),0,0],
            [...utf8('env'),...utf8('memory'),2,1,1,3],
            [...utf8('env'),...utf8('table'),1,0x70,1,1,3],
            [...utf8('env'),...utf8('counter'),3,I32,1]
        ])),section(3,vector([[0]])),
        section(7,vector([exportEntry('run',0,1),exportEntry('memory',2,0),exportEntry('table',1,0),exportEntry('counter',3,0)])),
        section(10,vector([body([0x20,0,0x10,0])])));
    const imports={env:{inc:value=>value+1,memory,table,counter}};
    const linked=new WebAssembly.Instance(new WebAssembly.Module(imported),imports);
    assert(linked.exports.run(41)===42,'host import called');
    assert(linked.exports.memory===memory&&linked.exports.table===table&&linked.exports.counter===counter,'import export object cache identity');
    counter.value=17;assert(linked.exports.counter.value===17,'import global shared value');
    throws(()=>new WebAssembly.Instance(new WebAssembly.Module(imported),{env:{}}),'LinkError','missing import');
    const marker=new Error('import marker');
    const throwing={env:{...imports.env,inc(){throw marker;}}};
    const throwingInstance=new WebAssembly.Instance(new WebAssembly.Module(imported),throwing);
    let caught;try{throwingInstance.exports.run(1);}catch(e){caught=e;}assert(caught===marker,'host exception identity');
    const start=binary(section(1,vector([type([],[])])),section(2,vector([[...utf8('env'),...utf8('start'),0,0]])),
        section(3,vector([[0]])),section(8,[1]),section(10,vector([body([0x10,0])])));
    let starts=0;
    new WebAssembly.Instance(new WebAssembly.Module(start),{env:{start(){starts++;}}});
    assert(starts===1,'start function executes');
    const trap=new WebAssembly.Instance(new WebAssembly.Module(single([],[],[0x00])));
    throws(()=>trap.exports.run(),'RuntimeError','unreachable trap');
    const integer=new WebAssembly.Instance(new WebAssembly.Module(single([I64],[I64],[0x20,0,0x42,1,0x7c])));
    assert(integer.exports.run(9007199254740993n)===9007199254740994n,'i64 lossless import export');
    throws(()=>integer.exports.run(1),'TypeError','i64 function requires BigInt');
    const bulk=new WebAssembly.Instance(new WebAssembly.Module(single([], [I32],
        [0x41,0,0x41,7,0x41,8,0xfc,11,0,0x41,16,0x41,0,0x41,8,0xfc,10,0,0,0x41,16,0x2d,0,0],
        [section(5,vector([[0,1]]))])));
    assert(bulk.exports.run()===7,'bulk memory fill and copy');
    const simd=new WebAssembly.Instance(new WebAssembly.Module(single([],[I32],[0xfd,12,42,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0xfd,27,0])));
    assert(simd.exports.run()===42,'SIMD constant and lane extraction');
    // Core f32/f64.nearest は ties-to-even。libc の丸め関数が要求する
    // RC と例外状態を保ち、符号付きゼロ・NaN・無限大も実命令で検査する。
    for(const [valueType,opcode,name,tiny]of [[0x7d,0x90,'f32',2**-149],[0x7c,0x9e,'f64',Number.MIN_VALUE]]){
        const nearest=new WebAssembly.Instance(new WebAssembly.Module(single([valueType],[valueType],[0x20,0,opcode]))).exports.run;
        for(const [input,expected]of [[0,0],[-0,-0],[0.5,0],[-0.5,-0],[1.5,2],[2.5,2],[-1.5,-2],[-2.5,-2],[tiny,0],[-tiny,-0],[Infinity,Infinity],[-Infinity,-Infinity]])
            assert(Object.is(nearest(input),expected),name+' nearest '+String(input)+(Object.is(input,-0)?' negative zero':''));
        assert(Number.isNaN(nearest(NaN)),name+' nearest NaN');
    }

    const bytes=add.slice(),pending=WebAssembly.compile(bytes);bytes.fill(0);
    assert((await pending) instanceof WebAssembly.Module,'compile immediate byte snapshot');
    const framed=new Uint8Array(add.length+2);framed.set(add,1);
    const offsetView=new DataView(framed.buffer,1,add.length);
    assert((await WebAssembly.instantiate(offsetView)).instance.exports.run(19,23)===42,'DataView byte offset');
    const spoof=add.slice();Object.defineProperties(spoof,{buffer:{value:null},byteLength:{value:0},byteOffset:{value:999}});
    assert((await WebAssembly.instantiate(spoof)).instance.exports.run(2,3)===5,'intrinsic BufferSource access');
    const fromModule=await WebAssembly.instantiate(module);
    assert(fromModule instanceof WebAssembly.Instance&&fromModule.exports.run(8,9)===17,'instantiate Module overload');
    const importModule=new WebAssembly.Module(imported),importReads=[];
    const importPromise=WebAssembly.instantiate(importModule,{get env(){importReads.push('namespace');return {...imports.env,get inc(){importReads.push('function');return x=>x+2;}};}});
    assert(importReads.join()==='namespace,function,namespace,namespace,namespace','Module overload reads import getters synchronously in order');
    assert((await importPromise).exports.run(40)===42&&importReads.length===5,'Module overload replays captured imports once');
    let readFailure;
    const rejectedImports=WebAssembly.instantiate(importModule,{get env(){readFailure=marker;throw marker;}});
    assert(readFailure===marker,'Module overload import getter happens before return');
    assert(await rejects(()=>rejectedImports,'Error','import getter rejection')===marker,'import getter rejection identity');
    const duplicateImports=binary(section(1,vector([type([],[I32])])),
        section(2,vector([[...utf8('env'),...utf8('next'),0,0],[...utf8('env'),...utf8('next'),0,0]])),
        section(3,vector([[0]])),section(7,vector([exportEntry('run',0,2)])),section(10,vector([body([0x10,0,0x10,1,0x6a])])));
    let reads=0;
    const duplicatePromise=WebAssembly.instantiate(new WebAssembly.Module(duplicateImports),{env:{get next(){const value=++reads;return ()=>value;}}});
    assert(reads===2&&(await duplicatePromise).exports.run()===3&&reads===2,'duplicate import names preserve per-read values');
    const fromBytes=await WebAssembly.instantiate(add);
    assert(fromBytes.module instanceof WebAssembly.Module&&fromBytes.instance instanceof WebAssembly.Instance,'instantiate bytes pair');
    await rejects(()=>WebAssembly.instantiate(add,null),'TypeError','null import object');
    await rejects(()=>WebAssembly.instantiate(new Uint8Array([0])),'CompileError','async compile failure');
    await rejects(()=>WebAssembly.instantiate(new WebAssembly.Module(imported),{env:{}}),'LinkError','async link failure');
    let asyncStarts=0;
    const asyncStart=WebAssembly.instantiate(new WebAssembly.Module(start),{env:{start(){asyncStarts++;}}});
    assert(asyncStarts===0,'async start deferred');await asyncStart;assert(asyncStarts===1,'async start settled');
    const optionError=await rejects(()=>WebAssembly.compile(add,{get builtins(){throw marker;}}),'Error','options exception');
    assert(optionError===marker,'options exception identity');
    for(const value of [1,'bad',true])await rejects(()=>WebAssembly.compile(add,value),'TypeError','options dictionary');
    const compileOptions={builtins:[]};const optionPromise=WebAssembly.compile(add,compileOptions);compileOptions.builtins.push('js-string');
    assert((await optionPromise) instanceof WebAssembly.Module,'options immediate snapshot');
    const iteratorPromise=WebAssembly.compile(add,{builtins:['unknown-builtin']}),arrayIterator=Array.prototype[Symbol.iterator];
    try{
        Array.prototype[Symbol.iterator]=()=>{throw new Error('author array iterator invoked');};
        assert((await iteratorPromise) instanceof WebAssembly.Module,'options snapshot owns its iterable across tasks');
    }finally{Array.prototype[Symbol.iterator]=arrayIterator;}
    if(typeof ArrayBuffer.prototype.transfer==='function'){
        const detached=add.slice().buffer,view=new Uint8Array(detached);detached.transfer();
        await rejects(()=>WebAssembly.compile(detached),'TypeError','detached ArrayBuffer');
        await rejects(()=>WebAssembly.compile(view),'TypeError','detached view');
    }
    const response=(data=add,mime='application/wasm',status=200)=>new Response(data,{headers:{'Content-Type':mime},status});
    const streamResponse=response();
    assert((await WebAssembly.compileStreaming(Promise.resolve(streamResponse))) instanceof WebAssembly.Module&&streamResponse.bodyUsed,'streaming Response Promise consumption');
    const streamPair=await WebAssembly.instantiateStreaming({then(resolve){resolve(response());}});
    assert(streamPair.module instanceof WebAssembly.Module&&streamPair.instance.exports.run(21,21)===42,'streaming thenable execution');
    assert((await WebAssembly.instantiateStreaming(response(add,'\tAPPLICATION/WASM '))).instance.exports.run(1,2)===3,'streaming MIME ASCII case and whitespace');
    const overwritten=response();overwritten.arrayBuffer=()=>{throw new Error('author override invoked');};
    Object.defineProperty(overwritten,'headers',{get(){throw new Error('author headers invoked');}});
    assert((await WebAssembly.compileStreaming(overwritten)) instanceof WebAssembly.Module,'streaming captured Response methods');
    for(const mime of ['text/plain','application/wasm;','application/wasm;charset=utf-8','application/wasm, application/wasm']){
        const wrong=response(add,mime);await rejects(()=>WebAssembly.compileStreaming(wrong),'TypeError','streaming MIME rejected');
        assert(!wrong.bodyUsed,'invalid MIME does not consume body');
    }
    await rejects(()=>WebAssembly.compileStreaming(new Response(add)),'TypeError','missing MIME');
    await rejects(()=>WebAssembly.compileStreaming(response(add,'application/wasm',404)),'TypeError','HTTP error rejected');
    await rejects(()=>WebAssembly.compileStreaming(Response.error()),'TypeError','network error response');
    for(const value of [null,{},Object.create(Response.prototype)])await rejects(()=>WebAssembly.compileStreaming(value),'TypeError','Response internal brand');
    const consumed=response();await consumed.arrayBuffer();await rejects(()=>WebAssembly.compileStreaming(consumed),'TypeError','body already consumed');
    const locked=response(),reader=locked.body.getReader();await rejects(()=>WebAssembly.compileStreaming(locked),'TypeError','body reader locked');reader.releaseLock();
    const bad=response(new Uint8Array([0]));await rejects(()=>WebAssembly.compileStreaming(bad),'CompileError','streaming invalid module');assert(bad.bodyUsed,'invalid module consumed');
    const sourceError=await rejects(()=>WebAssembly.compileStreaming(Promise.reject(marker)),'Error','streaming source rejection');assert(sourceError===marker,'source rejection identity');
    const bodyError=new ReadableStream({start(controller){controller.error(marker);}});
    assert(await rejects(()=>WebAssembly.compileStreaming(response(bodyError)),'Error','body stream rejection')===marker,'stream failure identity');
    // bootstrap 時点の intrinsic / timer と、Response の内部状態を使うことを
    // 独立に確認する。作者が prototype を変更しても権限判定は変わらない。
    const clean=response(),wrongMIME=response(add,'text/plain'),wrongStatus=response(add,'application/wasm',404);
    const realm=globalThis,original={timer:globalThis.setTimeout,create:Object.create,module:WebAssembly.Module,
        symbol:Symbol,typeError:TypeError,
        get:Headers.prototype.get,bytes:Response.prototype.arrayBuffer,
        status:Object.getOwnPropertyDescriptor(Response.prototype,'status'),
        headers:Object.getOwnPropertyDescriptor(Response.prototype,'headers')};
    try{
        realm.setTimeout=()=>{throw new Error('author timer invoked');};
        realm.globalThis={};realm.Symbol={iterator:'author iterator'};
        realm.TypeError=function(){return new Error('author TypeError invoked');};
        Object.create=()=>{throw new Error('author Object.create invoked');};
        WebAssembly.Module=function(){throw new Error('author Module invoked');};
        Headers.prototype.get=()=> 'application/wasm';
        Response.prototype.arrayBuffer=()=>{throw new Error('author arrayBuffer invoked');};
        Object.defineProperty(Response.prototype,'status',{configurable:true,get(){return 200;}});
        Object.defineProperty(Response.prototype,'headers',{configurable:true,get(){throw new Error('author headers invoked');}});
        assert((await WebAssembly.compile(add)) instanceof original.module,'compile uses bootstrap constructors and timer');
        // upstream Streams の作者による組込み全体の変更耐性は別責務。
        // ここでは compile facade の捕捉を検査してから元へ戻す。
        Object.create=original.create;
        assert((await WebAssembly.compileStreaming(clean)) instanceof original.module,'streaming uses captured Response and MIME intrinsics');
        await rejects(()=>WebAssembly.compileStreaming(wrongMIME),'TypeError','prototype cannot forge MIME');
        await rejects(()=>WebAssembly.compileStreaming(wrongStatus),'TypeError','prototype cannot forge status');
    }finally{
        realm.globalThis=realm;realm.Symbol=original.symbol;realm.TypeError=original.typeError;
        realm.setTimeout=original.timer;Object.create=original.create;WebAssembly.Module=original.module;
        Headers.prototype.get=original.get;Response.prototype.arrayBuffer=original.bytes;
        Object.defineProperty(Response.prototype,'status',original.status);Object.defineProperty(Response.prototype,'headers',original.headers);
    }
    const dataWasm='data:application/wasm;base64,AGFzbQEAAAABBwFgAn9/AX8DAgEABwcBA2FkZAAACgkBBwAgACABags=';
    const dataResponse=await fetch(dataWasm),dataPair=await WebAssembly.instantiate(await dataResponse.arrayBuffer());
    assert(dataPair.instance.exports.add(17,25)===42,'data Fetch bytes instantiate real Wasm');
    const dataStream=await WebAssembly.instantiateStreaming(fetch(dataWasm));
    assert(dataStream.instance.exports.add(16,26)===42,'data Fetch streaming API MIME and body');
    return checks;
};

/* 現行 WebIDL の dictionary 厳密性を含む追加 native 境界。
 * host Node は primitive compile options を許容する版があるため、基本
 * facade suite と結果を分ける。Nocturne 側の期待を host に合わせて弱めない。 */
globalThis.runWasmNativeBoundaryCases = async function runWasmNativeBoundaryCases() {
    let checks=0;
    const assert=(ok,label)=>{if(!ok)throw new Error(label);checks++;};
    const throws=(fn,name,label)=>{let error;try{fn();}catch(e){error=e;}assert(error&&error.name===name,label);};
    const {utf8,vector,section,binary,type,exportEntry,body,single}=wasmCaseBinary,I32=0x7f;
    const add=single([I32,I32],[I32],[0x20,0,0x20,1,0x6a]);
    throws(()=>WebAssembly.Module(add),'TypeError','Module requires new');
    throws(()=>WebAssembly.Memory({initial:1}),'TypeError','Memory requires new');
    const unknown=new WebAssembly.Module(add,{builtins:['unknown-set']});
    assert(new WebAssembly.Instance(unknown).exports.run(19,23)===42,'unknown builtin set ignored');
    assert(WebAssembly.validate(add,{builtins:['unknown-set']}),'unknown builtin validation');
    // JS API は Wasm Memory の ArrayBufferDetachKey を設定する。作者の
    // transfer / structuredClone は host memory を detach できない。
    const protectedMemory=new WebAssembly.Memory({initial:1,maximum:2}),protectedBuffer=protectedMemory.buffer;
    new Uint8Array(protectedBuffer)[7]=61;
    for(const name of ['transfer','transferToFixedLength']){
        assert(typeof ArrayBuffer.prototype[name]==='function',name+' API present');
        throws(()=>ArrayBuffer.prototype[name].call(protectedBuffer),'TypeError','Wasm buffer '+name+' prohibited');
        assert(protectedBuffer.byteLength===65536&&new Uint8Array(protectedBuffer)[7]===61&&protectedMemory.buffer===protectedBuffer,name+' rejection preserves Wasm bytes');
    }
    assert(typeof structuredClone==='function','structuredClone API present');
    throws(()=>structuredClone(protectedBuffer,{transfer:[protectedBuffer]}),'DataCloneError','Wasm buffer structuredClone transfer prohibited');
    assert(protectedBuffer.byteLength===65536&&new Uint8Array(protectedBuffer)[7]===61,'clone transfer rejection preserves Wasm bytes');
    const untouched=new ArrayBuffer(4);new Uint8Array(untouched)[0]=33;
    throws(()=>structuredClone({untouched,protectedBuffer},{transfer:[untouched,protectedBuffer]}),'DataCloneError','transfer preflight rejects Wasm before other detach');
    assert(untouched.byteLength===4&&new Uint8Array(untouched)[0]===33&&protectedBuffer.byteLength===65536,'failed transfer list is atomic');
    const ordinary=new ArrayBuffer(4);new Uint8Array(ordinary)[0]=37;
    const moved=ordinary.transfer();
    assert(ordinary.byteLength===0&&moved.byteLength===4&&new Uint8Array(moved)[0]===37,'ordinary ArrayBuffer transfer remains functional');
    const fixed=moved.transferToFixedLength();
    assert(moved.byteLength===0&&fixed.byteLength===4&&new Uint8Array(fixed)[0]===37,'ordinary ArrayBuffer fixed transfer remains functional');
    const cloned=structuredClone(fixed,{transfer:[fixed]});
    assert(fixed.byteLength===0&&cloned.byteLength===4&&new Uint8Array(cloned)[0]===37,'ordinary structuredClone transfer remains functional');
    const copied=structuredClone(protectedBuffer);new Uint8Array(copied)[7]=62;
    assert(protectedBuffer.byteLength===65536&&new Uint8Array(protectedBuffer)[7]===61&&new Uint8Array(copied)[7]===62,'Wasm buffer nontransfer clone remains an independent copy');
    assert(protectedMemory.grow(0)===1&&protectedBuffer.byteLength===0,'host Memory grow zero still detaches protected buffer');
    const afterZero=protectedMemory.buffer;
    assert(protectedMemory.grow(1)===1&&afterZero.byteLength===0&&protectedMemory.buffer.byteLength===131072&&new Uint8Array(protectedMemory.buffer)[7]===61,'host Memory grow detaches protected buffer and preserves bytes');
    // memory.grow 命令による detach は JS の Memory.grow とは別経路。
    const growBytes=binary(section(1,vector([type([I32],[I32])])),section(3,vector([[0]])),
        section(5,vector([[1,1,3]])),section(7,vector([exportEntry('grow',0,0),exportEntry('memory',2,0)])),
        section(10,vector([body([0x20,0,0x40,0])])));
    const grow=new WebAssembly.Instance(new WebAssembly.Module(growBytes)).exports;
    let previous=grow.memory.buffer;new Uint8Array(previous)[0]=27;
    assert(grow.grow(0)===1&&previous.byteLength===0&&grow.memory.buffer.byteLength===65536,'Wasm memory.grow zero detaches');
    previous=grow.memory.buffer;
    assert(grow.grow(1)===1&&previous.byteLength===0&&grow.memory.buffer.byteLength===131072&&new Uint8Array(grow.memory.buffer)[0]===27,'Wasm memory.grow preserves bytes and detaches');
    assert(grow.grow(2)===-1&&grow.memory.buffer.byteLength===131072,'Wasm memory.grow failure result');
    const surviving=(()=>new WebAssembly.Instance(new WebAssembly.Module(add)).exports.run)();
    assert(surviving(11,31)===42,'exported function survives lost instance variable');
    const callTable=new WebAssembly.Table({element:'anyfunc',initial:1});
    (()=>{const owner=new WebAssembly.Instance(new WebAssembly.Module(add));callTable.set(0,owner.exports.run);})();
    assert(callTable.get(0)(15,27)===42,'table retains exported function after owner scope');
    const referenceGlobal=(()=>new WebAssembly.Global({value:'externref',mutable:true},{marker:'global reference'}))();
    const referenceTable=new WebAssembly.Table({element:'externref',initial:1});
    (()=>{const value={marker:'table reference'};referenceTable.set(0,value);})();
    assert(referenceGlobal.value.marker==='global reference'&&referenceTable.get(0).marker==='table reference','Global and Table retain externref values');
    const pairBytes=binary(section(1,vector([type([I32],[I32,I32]),type([I32],[I32])])),
        section(2,vector([[...utf8('env'),...utf8('pair'),0,0]])),section(3,vector([[1]])),
        section(7,vector([exportEntry('run',0,1)])),section(10,vector([body([0x20,0,0x10,0,0x6a])])));
    const pair=new WebAssembly.Instance(new WebAssembly.Module(pairBytes),{env:{pair(value){return {[Symbol.iterator]:function*(){yield value;yield 1;}};}}});
    const arrayFrom=Array.from;
    try{Array.from=()=>{throw new Error('author Array.from invoked');};assert(pair.exports.run(41)===42,'multi-value import accepts iterable with captured Array.from');}
    finally{Array.from=arrayFrom;}
    const namespace='env\0suffix',field='callback\0name',exportName='run\0suffix';
    const nulBytes=binary(section(1,vector([type([I32],[I32])])),
        section(2,vector([[...utf8(namespace),...utf8(field),0,0]])),section(7,vector([exportEntry(exportName,0,0)])));
    const nulModule=new WebAssembly.Module(nulBytes);
    const descriptor=WebAssembly.Module.imports(nulModule)[0];
    assert(descriptor.module===namespace&&descriptor.name===field&&WebAssembly.Module.exports(nulModule)[0].name===exportName,'descriptor names retain embedded NUL');
    const nulImports={[namespace]:{[field]:value=>value+1}};
    assert(new WebAssembly.Instance(nulModule,nulImports).exports[exportName](41)===42,'native import export names retain embedded NUL');
    assert((await WebAssembly.instantiate(nulModule,nulImports)).exports[exportName](41)===42,'Promise import snapshot retains embedded NUL');
    const nested=new WebAssembly.Instance(new WebAssembly.Module(add));
    const outer=new WebAssembly.Instance(nulModule,{[namespace]:{[field]:value=>nested.exports.run(value,2)+1}});
    assert(outer.exports[exportName](39)===42,'host import reenters another Wasm export');
    const trap=new WebAssembly.Instance(new WebAssembly.Module(single([],[],[0x00])));
    const recovered=new WebAssembly.Instance(nulModule,{[namespace]:{[field]:value=>{let failed=false;try{trap.exports.run();}catch(e){failed=e instanceof WebAssembly.RuntimeError;}if(!failed)throw new Error('nested trap absent');return value+1;}}});
    assert(recovered.exports[exportName](41)===42,'nested trap caught in import preserves outer execution');
    for(const value of [1,'bad',true]){
        throws(()=>new WebAssembly.Module(add,value),'TypeError','native Module compile options dictionary');
        throws(()=>WebAssembly.validate(add,value),'TypeError','native validate compile options dictionary');
    }
    throws(()=>new WebAssembly.Module(add,{builtins:['unknown','unknown']}),'CompileError','duplicate builtin names rejected');
    assert(!WebAssembly.validate(add,{builtins:['unknown','unknown']}),'duplicate builtin validation false');
    return checks;
};

/* HTTP URL の本物の Worker を使用する。Module の structured clone を
 * 代替成功に使わず、Worker 自身で bytes / Response から実行する。 */
globalThis.runWasmWorkerCases = async function runWasmWorkerCases(url) {
    let checks=0;
    const assert=(ok,label)=>{if(!ok)throw new Error(label);checks++;};
    const worker=new Worker(url);
    try{
        const memory=new WebAssembly.Memory({initial:1}),buffer=memory.buffer;new Uint8Array(buffer)[3]=41;
        let transferError;
        try{worker.postMessage({type:'memory-must-not-transfer',buffer},[buffer]);}catch(error){transferError=error;}
        assert(transferError&&transferError.name==='DataCloneError','Worker postMessage rejects Wasm memory transfer');
        assert(buffer.byteLength===65536&&memory.buffer===buffer&&new Uint8Array(buffer)[3]===41,'Worker rejected transfer leaves host memory attached');
        const result=await new Promise((resolve,reject)=>{
            worker.onerror=event=>reject(new Error('Wasm worker error: '+event.message));
            worker.onmessage=event=>{
                const data=event.data;
                if(!data||data.type!=='wasm-worker-result'){reject(new Error('Unexpected Wasm Worker message'));return;}
                if(!data.ok){reject(new Error('Wasm Worker assertion: '+data.error));return;}
                resolve(data);
            };
            worker.postMessage({type:'run-wasm'});
        });
        assert(result.compiled===true&&result.moduleOverload===42,'Worker compile and Module instantiate result');
        assert(result.bytesOverload===42,'Worker byte instantiate result');
        assert(result.callback===42,'Worker native import callback result');
        assert(result.memoryBytes===131072&&result.memoryValue===29&&result.oldDetached===true,'Worker memory ownership and grow result');
        assert(result.streaming===42&&result.streamBodyUsed===true,'Worker Response compileStreaming result');
        assert(result.streamingPair===42,'Worker instantiateStreaming result');
        return checks+result.checks;
    }finally{worker.terminate();}
};

/* Native document CSP: all five byte-compiling entry points share the gate,
 * while validate remains a non-executing query. */
globalThis.runWasmPolicyCases = async function runWasmPolicyCases(allowed,eventCount) {
    let checks=0;
    const assert=(ok,label)=>{if(!ok)throw new Error(label);checks++;};
    const bytes=new Uint8Array([0,97,115,109,1,0,0,0]),events=[];
    let complete;const reported=new Promise(resolve=>{complete=resolve;});
    const observe=event=>{events.push(event);if(events.length===eventCount)complete();};
    addEventListener('securitypolicyviolation',observe);
    try{
        assert(WebAssembly.validate(bytes),'CSP does not prohibit non-executing validation');
        const response=()=>new Response(bytes,{headers:{'Content-Type':'application/wasm'}});
        const operations=[()=>new WebAssembly.Module(bytes),()=>WebAssembly.compile(bytes),
            ()=>WebAssembly.instantiate(bytes),()=>WebAssembly.compileStreaming(response()),
            ()=>WebAssembly.instantiateStreaming(response())];
        for(const operation of operations){
            let result,error;try{result=await operation();}catch(e){error=e;}
            assert(allowed?!error:!!error&&error.name==='CompileError','CSP compilation authorization');
            if(allowed)assert(result instanceof WebAssembly.Module||result.module instanceof WebAssembly.Module,'CSP allowed result is executable module');
        }
        if(eventCount)await reported;
        assert(events.length===eventCount,'CSP native violation count');
        for(const event of events){
            assert(event.isTrusted&&event.effectiveDirective==='script-src'&&event.blockedURI==='wasm-eval','CSP native violation identity');
            assert(event.disposition===(allowed?'report':'enforce'),'CSP report-only versus enforcement');
        }
        return checks;
    }finally{removeEventListener('securitypolicyviolation',observe);}
};

globalThis.runWasmWorkerPolicyCases = async function runWasmWorkerPolicyCases(base) {
    let checks=0;
    for(const [variant,allowed,reports] of [['deny',false,5],['allow',true,0],['report',true,5]]){
        const worker=new Worker(base+'workers/wasm-policy-'+variant+'.js');
        try{
            const result=await new Promise((resolve,reject)=>{
                worker.onerror=e=>reject(new Error('Wasm policy Worker error: '+e.message));
                worker.onmessage=e=>e.data&&e.data.type==='wasm-policy-result'&&e.data.ok?resolve(e.data):reject(new Error('Wasm policy Worker failure: '+JSON.stringify(e.data)));
                worker.postMessage({type:'run-wasm-policy',allowed,reports});
            });
            if(result.events!==reports||!result.validated)throw new Error('Wasm policy Worker identity mismatch');
            checks+=result.checks+1;
        }finally{worker.terminate();}
    }
    return checks;
};

/* JSPI remains a separate entry until the native coroutine implementation is
 * in the tested image. The current proposal's normative JS API uses intrinsic
 * PromiseResolve + Await even for synchronous/thenable import return values;
 * its older Overview's IsPromise-only wording is not the expectation here. */
globalThis.runWasmJspiCases = async function runWasmJspiCases() {
    let checks=0;
    const assert=(ok,label)=>{if(!ok)throw new Error('JSPI '+label+' (passed '+checks+')');checks++;};
    const throws=(fn,name,label)=>{let error;try{fn();}catch(e){error=e;}assert(error&&error.name===name,label);return error;};
    const rejection=async(fn,label)=>{
        let promise;try{promise=fn();}catch(error){throw new Error('JSPI '+label+' threw synchronously after '+checks+' checks: '+error);}
        assert(promise instanceof Promise,label+' returns Promise');
        let rejected=false,reason;try{await promise;}catch(error){rejected=true;reason=error;}
        assert(rejected,label+' rejects');return reason;
    };
    const {binary,section,vector,type,utf8,exportEntry,body,single}=wasmCaseBinary;
    const I32=0x7f,I64=0x7e,EXTERNREF=0x6f;
    const imported=(params=[I32],results=[I32],code=[0x20,0,0x10,0,0x41,1,0x6a])=>binary(
        section(1,vector([type(params,results)])),
        section(2,vector([[...utf8('env'),...utf8('step'),0,0]])),
        section(3,vector([[0]])),section(7,vector([exportEntry('run',0,1)])),
        section(10,vector([body(code)])));
    const relayModule=new WebAssembly.Module(imported());
    const relay=step=>new WebAssembly.Instance(relayModule,{env:{step}}).exports.run;
    const marked=step=>relay(new WebAssembly.Suspending(step));
    assert(typeof WebAssembly.Suspending==='function'&&typeof WebAssembly.promising==='function','real API entry points');
    assert(typeof WebAssembly.SuspendError==='function','SuspendError exposed');
    const suspendError=new WebAssembly.SuspendError('suspend marker');
    assert(suspendError instanceof Error&&suspendError.name==='SuspendError'&&suspendError.message==='suspend marker','SuspendError native Error contract');
    for(const value of [undefined,null,0,{},new Promise(()=>{})])throws(()=>new WebAssembly.Suspending(value),'TypeError','Suspending requires callable');
    throws(()=>WebAssembly.Suspending(()=>1),'TypeError','Suspending requires new');
    const marker=new WebAssembly.Suspending(x=>x+1);
    assert(typeof marker==='object'&&marker instanceof WebAssembly.Suspending&&Object.getPrototypeOf(marker)===WebAssembly.Suspending.prototype,'Suspending is a marker object');
    assert(Reflect.ownKeys(marker).length===0,'marker exposes no wrapped-function data');
    throws(()=>Reflect.apply(marker,undefined,[1]),'TypeError','marker itself is not callable');
    throws(()=>new WebAssembly.Suspending(marker),'TypeError','marker is not a function argument');
    for(const spoof of [Object.create(WebAssembly.Suspending.prototype),new Proxy(marker,{})])throws(()=>relay(spoof),'LinkError','Suspending slot cannot be spoofed');

    const plain=new WebAssembly.Instance(new WebAssembly.Module(single([I32,I32],[I32],[0x20,0,0x20,1,0x6a]))).exports.run;
    for(const value of [undefined,null,0,{},()=>42,marker,plain.bind(null),new Proxy(plain,{})])throws(()=>WebAssembly.promising(value),'TypeError','promising requires an actual exported function');
    const promisedPlain=WebAssembly.promising(plain),plainPromise=promisedPlain.call({ignored:true},20,22);
    assert(typeof promisedPlain==='function'&&plainPromise instanceof Promise,'promising returns a Promise-bearing function');
    assert(await plainPromise===42,'no-suspension export fulfills');
    throws(()=>new WebAssembly.promising(plain),'TypeError','promising operation is not a constructor');
    throws(()=>new promisedPlain(20,22),'TypeError','promising result is not a constructor');
    const trap=new WebAssembly.Instance(new WebAssembly.Module(single([],[],[0x00]))).exports.run;
    const trapError=await rejection(()=>WebAssembly.promising(trap)(),'native Wasm trap');
    assert(trapError instanceof WebAssembly.RuntimeError,'Wasm trap rejects with RuntimeError');

    let syncCalls=0,receiver='not called';const syncTrace=[];
    const sync=marked(function(value){'use strict';receiver=this;syncCalls++;syncTrace.push('import');return value+1;});
    const syncPromise=WebAssembly.promising(sync)(40);syncTrace.push('returned');
    assert(syncPromise instanceof Promise&&syncTrace.join(',')==='import,returned','sync import starts before caller returns');
    let syncSettled=false;syncPromise.then(()=>{syncSettled=true;});assert(!syncSettled,'sync return still crosses Promise checkpoint');
    assert(await syncPromise===42&&syncCalls===1&&receiver===undefined,'synchronous import result and receiver');
    const syncBefore=syncCalls,unwrappedSync=throws(()=>sync(40),'SuspendError','ordinary export cannot enter a Suspending import');
    assert(unwrappedSync instanceof WebAssembly.SuspendError&&syncCalls===syncBefore,'ordinary export is rejected before calling wrapped JS');

    let resolvePending,pendingCalls=0;
    const pendingImport=new Promise(resolve=>{resolvePending=resolve;});
    const pending=WebAssembly.promising(marked(value=>{pendingCalls++;assert(value===40,'pending import argument');return pendingImport;}))(40);
    let pendingSettled=false;pending.then(()=>{pendingSettled=true;});
    assert(pending instanceof Promise&&pendingCalls===1&&!pendingSettled,'pending import returns control without replay');
    let timerRan=false;setTimeout(()=>{timerRan=true;resolvePending(41);},0);
    assert(await pending===42&&timerRan&&pendingCalls===1,'event-loop task resumes native continuation once');
    const rejectedReason={jspi:'rejected object'};
    const rejected=await rejection(()=>WebAssembly.promising(marked(()=>Promise.reject(rejectedReason)))(0),'rejected import');
    assert(rejected===rejectedReason,'Promise rejection preserves exact reason');
    const undefinedReason=await rejection(()=>WebAssembly.promising(marked(()=>Promise.reject(undefined)))(0),'undefined rejection reason');
    assert(undefinedReason===undefined,'undefined remains a rejection reason rather than successful completion');
    const thrownReason={jspi:'synchronous throw'};
    const thrown=await rejection(()=>WebAssembly.promising(marked(()=>{throw thrownReason;}))(0),'throwing import');
    assert(thrown===thrownReason,'synchronous JS throw preserves exact reason');

    let thenReads=0,thenCalls=0;
    const thenable={get then(){thenReads++;return resolve=>{thenCalls++;resolve(41);};},valueOf(){throw new Error('thenable was coerced before assimilation');}};
    assert(await WebAssembly.promising(marked(()=>thenable))(0)===42&&thenReads===1&&thenCalls===1,'PromiseResolve assimilates thenable once');
    const thenReason={jspi:'then getter'};
    const thenError=await rejection(()=>WebAssembly.promising(marked(()=>({get then(){throw thenReason;}})))(0),'throwing then getter');
    assert(thenError===thenReason,'then getter rejection identity');
    const intrinsicPromise=Promise.resolve(41);let authorThenReads=0;
    Object.defineProperty(intrinsicPromise,'then',{get(){authorThenReads++;throw new Error('author Promise.then called');}});
    assert(await WebAssembly.promising(marked(()=>intrinsicPromise))(0)===42&&authorThenReads===0,'native Promise reactions do not read author then');
    const unmarked=relay(()=>Promise.resolve(41));
    assert(await WebAssembly.promising(unmarked)(0)===1,'ordinary imports do not silently acquire suspension semantics');

    // Two suspension points in a single call. Resolve each gate explicitly;
    // neither a timer-based timeout nor a fixed microtask-count assumption is
    // used to turn incomplete work into success.
    const doubleModule=new WebAssembly.Module(imported([I32],[I32],[0x20,0,0x10,0,0x10,0]));
    const gates=[];let enteredSecond;
    const secondEntered=new Promise(resolve=>{enteredSecond=resolve;});
    const double=new WebAssembly.Instance(doubleModule,{env:{step:new WebAssembly.Suspending(value=>new Promise(resolve=>{gates.push({value,resolve});if(gates.length===2)enteredSecond();}))}}).exports.run;
    const twice=WebAssembly.promising(double)(40);let twiceSettled=false;twice.then(()=>{twiceSettled=true;});
    assert(gates.length===1&&gates[0].value===40&&!twiceSettled,'first suspension gate');
    gates[0].resolve(41);await secondEntered;
    assert(gates.length===2&&gates[1].value===41&&!twiceSettled,'second suspension keeps original call pending');
    gates[1].resolve(42);assert(await twice===42&&gates.length===2,'multiple suspensions resume without replaying imports');

    // Overlapping calls on the same export may settle in a different order.
    const waiting=new Map(),overlapTrace=[];
    const overlapping=WebAssembly.promising(marked(value=>new Promise(resolve=>{waiting.set(value,resolve);overlapTrace.push(value);})));
    const first=overlapping(10),second=overlapping(20);let firstSettled=false;first.then(()=>{firstSettled=true;});
    assert(waiting.size===2&&overlapTrace.join(',')==='10,20','independent overlapping native stacks');
    waiting.get(20)(21);assert(await second===22&&!firstSettled,'second call can finish while first remains suspended');
    waiting.get(10)(11);assert(await first===12&&overlapTrace.length===2,'first call resumes its own locals');

    const inner=marked(value=>Promise.resolve(value+1));
    const syncReentry=marked(value=>Promise.resolve(plain(value,1)));
    assert(await WebAssembly.promising(syncReentry)(40)===42,'author callback may reenter nonsuspending Wasm');
    const direct=relay(inner);
    assert(await WebAssembly.promising(direct)(40)===43,'direct Wasm-to-Wasm import remains suspension-transparent');
    const innerPromising=WebAssembly.promising(inner),nested=marked(value=>innerPromising(value));
    assert(await WebAssembly.promising(nested)(40)===43,'nested promising/Suspending pair is allowed');
    const trampoline=relay(value=>inner(value));
    const frameError=await rejection(()=>WebAssembly.promising(trampoline)(40),'author JS frame crossing');
    assert(frameError instanceof WebAssembly.SuspendError,'Wasm-to-JS-to-Wasm cannot suspend an author frame');
    const ordinaryAsync=marked(()=>Promise.resolve(41));
    assert(throws(()=>ordinaryAsync(0),'SuspendError','ordinary export cannot suspend') instanceof WebAssembly.SuspendError,'unbalanced suspension error brand');

    const wideModule=new WebAssembly.Module(imported([I64],[I64],[0x20,0,0x10,0]));
    const wide=new WebAssembly.Instance(wideModule,{env:{step:new WebAssembly.Suspending(value=>Promise.resolve(value+1n))}}).exports.run;
    assert(await WebAssembly.promising(wide)(41n)===42n,'i64 Promise fulfillment uses BigInt conversion');
    const externalModule=new WebAssembly.Module(imported([EXTERNREF],[EXTERNREF],[0x20,0,0x10,0]));
    const external=new WebAssembly.Instance(externalModule,{env:{step:new WebAssembly.Suspending(value=>Promise.resolve(value))}}).exports.run,object={jspi:'externref'};
    assert(await WebAssembly.promising(external)(object)===object,'externref survives suspension and result handoff');
    const pairModule=new WebAssembly.Module(imported([I32],[I32,I32],[0x20,0,0x10,0]));
    const pair=new WebAssembly.Instance(pairModule,{env:{step:new WebAssembly.Suspending(value=>Promise.resolve([value,1]))}}).exports.run;
    const pairResult=await WebAssembly.promising(pair)(41);
    assert(Array.isArray(pairResult)&&pairResult.length===2&&pairResult[0]===41&&pairResult[1]===1,'multi-value Promise fulfillment and export array');
    const voidModule=new WebAssembly.Module(imported([],[],[0x10,0]));
    const noResult=new WebAssembly.Instance(voidModule,{env:{step:new WebAssembly.Suspending(()=>Promise.resolve('discarded'))}}).exports.run;
    assert(await WebAssembly.promising(noResult)()===undefined,'void export fulfills rather than remaining pending');
    // The current formal draft evaluates argument coercion inside its async
    // runner. Some host engines still throw here synchronously; do not replace
    // this expectation with a host-derived weaker result or hide it with skip.
    const coercionError=await rejection(()=>promisedPlain(Symbol(),1),'argument conversion');
    assert(coercionError instanceof TypeError,'argument conversion rejects rather than escaping synchronously');
    return checks;
};
