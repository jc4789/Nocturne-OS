/* WebAssembly の Promise / Fetch 境界。命令の検証・実行とオブジェクトの
 * 所有権は js_wasm.cpp が担い、ここでは公開前の byte snapshot と
 * Response 本体の消費を既存の Fetch / Streams に接続する。
 * https://webassembly.github.io/spec/js-api/ (2026-10-14)
 * https://webassembly.github.io/spec/web-api/ (2026-10-14)
 * native 初期化と Response 定義の後、同じ realm で読み込む。 */
(() => {
    'use strict';
    const realm=globalThis,wasm=realm.WebAssembly,Module=wasm.Module,Instance=wasm.Instance,TypeError=realm.TypeError;
    const moduleExports=Module.exports,moduleImports=Module.imports,LinkError=wasm.LinkError;
    const apply=Reflect.apply,define=Object.defineProperty,create=Object.create;
    const P=Promise, then=P.prototype.then, timer=globalThis.setTimeout;
    const iteratorKey=Symbol.iterator;
    const promiseConstructor=Object.freeze({[Symbol.species]:P});
    const U8=Uint8Array, AB=ArrayBuffer, isView=AB.isView;
    const getter=(p,k)=>Object.getOwnPropertyDescriptor(p,k).get;
    const abLength=getter(AB.prototype,'byteLength'), ta=Object.getPrototypeOf(U8.prototype);
    const taBuffer=getter(ta,'buffer'), taOffset=getter(ta,'byteOffset'), taLength=getter(ta,'byteLength');
    const taTag=getter(ta,Symbol.toStringTag), dvBuffer=getter(DataView.prototype,'buffer');
    const dvOffset=getter(DataView.prototype,'byteOffset'), dvLength=getter(DataView.prototype,'byteLength');
    const sabLength=typeof SharedArrayBuffer==='function'?getter(SharedArrayBuffer.prototype,'byteLength'):null;
    const set=U8.prototype.set, string=String, wellFormed=String.prototype.toWellFormed;
    const responseType=getter(Response.prototype,'type'), responseStatus=getter(Response.prototype,'status');
    const responseHeaders=getter(Response.prototype,'headers'), responseBytes=Response.prototype.arrayBuffer;
    const headerGet=Headers.prototype.get, code=String.prototype.charCodeAt, slice=String.prototype.slice, lower=String.prototype.toLowerCase;
    const memoryBuffer=getter(wasm.Memory.prototype,'buffer'),tableLength=getter(wasm.Table.prototype,'length');
    function rejected(error){return new P((resolve,reject)=>reject(error));}
    function react(promise,fulfilled){
        // 内部 Promise の species を作者の prototype 変更へ公開しない。
        define(promise,'constructor',{value:promiseConstructor});
        return apply(then,promise,[fulfilled]);
    }
    function snapshot(value){
        let buffer=value,offset=0,length;
        if(isView(value)){
            const typed=apply(taTag,value,[])!==undefined;
            buffer=apply(typed?taBuffer:dvBuffer,value,[]);
            offset=apply(typed?taOffset:dvOffset,value,[]);
            length=apply(typed?taLength:dvLength,value,[]);
        }
        let size;
        try{size=apply(abLength,buffer,[]);}
        catch(error){if(!sabLength)throw error;size=apply(sabLength,buffer,[]);}
        // detached と範囲外の view は実エンジンの検査に委ねる。
        const input=new U8(buffer,offset,length===undefined?size:length),out=new U8(apply(taLength,input,[]));
        apply(set,out,[input]);return apply(taBuffer,out,[]);
    }
    function usv(value){
        if(typeof value==='symbol')throw new TypeError('Cannot convert Symbol to string');
        return apply(wellFormed,string(value),[]);
    }
    function options(value){
        if(value==null)value={};
        else if(typeof value!=='object'&&typeof value!=='function')throw new TypeError('Expected WebAssembly compile options');
        const builtins=value.builtins,out=create(null);out.builtins=[];
        if(builtins!==undefined){
            if(builtins===null||(typeof builtins!=='object'&&typeof builtins!=='function'))throw new TypeError('Expected builtin sequence');
            for(const item of builtins)define(out.builtins,out.builtins.length,{value:usv(item),writable:true,enumerable:true,configurable:true});
        }
        // native 側でも dictionary を変換するため、その時点の作者変更で
        // snapshot の iterable が変わらないよう iterator 自身も所有する。
        const names=out.builtins;
        define(names,iteratorKey,{value(){
            let index=0;const iterator=create(null);
            define(iterator,'next',{value:()=>index<names.length?{done:false,value:names[index++]}:{done:true}});
            return iterator;
        }});
        const constants=value.importedStringConstants;
        out.importedStringConstants=constants==null?null:usv(constants);return out;
    }
    function imports(value){
        if(value!==undefined&&(value===null||(typeof value!=='object'&&typeof value!=='function')))
            throw new TypeError('Expected WebAssembly import object');
        return value;
    }
    function isModule(value){try{apply(moduleExports,Module,[value]);return true;}catch(_){return false;}}
    function preparedImports(module,object){
        const descriptors=apply(moduleImports,Module,[module]),replay=create(null),queues=create(null);
        if(descriptors.length&&object===undefined)throw new TypeError('WebAssembly module requires imports');
        for(let i=0;i<descriptors.length;i++){
            const descriptor=descriptors[i],name=descriptor.module,field=descriptor.name,namespace=object[name];
            if(namespace===null||(typeof namespace!=='object'&&typeof namespace!=='function'))
                throw new TypeError('Expected WebAssembly import namespace object');
            const value=namespace[field],kind=descriptor.kind;
            if(kind==='function'&&typeof value!=='function')throw new LinkError('WebAssembly function import is not callable');
            if(kind==='memory'||kind==='table'){
                try{apply(kind==='memory'?memoryBuffer:tableLength,value,[]);}
                catch(_){throw new LinkError('WebAssembly import has the wrong object kind');}
            }
            let queue=queues[name];
            if(!queue){
                queue=[];queues[name]=queue;let next=0;
                define(replay,name,{get(){return queue[next++];}});
            }
            const entry=create(null);define(entry,field,{value});
            define(queue,queue.length,{value:entry,writable:true,enumerable:true,configurable:true});
        }
        // native Instance は import ごとに namespace/name を読む。同名の
        // import が複数あっても、作者 getter の各回の値を順序通り再生する。
        // 作者コードはここで一度だけ呼び、start と core 実行だけを延期する。
        return replay;
    }
    function defer(operation){
        return new P((resolve,reject)=>{
            // 0 は遅延時間なしの次タスク。作者の setTimeout は呼び出さない。
            apply(timer,realm,[()=>{try{resolve(operation());}catch(error){reject(error);}},0]);
        });
    }
    function compile(bytes,compileOptions=undefined){
        try{const stable=snapshot(bytes),opts=options(compileOptions);return defer(()=>new Module(stable,opts));}
        catch(error){return rejected(error);}
    }
    function instantiate(bytes,importObject=undefined,compileOptions=undefined){
        try{
            if(isModule(bytes)){const object=preparedImports(bytes,imports(importObject));return defer(()=>new Instance(bytes,object));}
            const stable=snapshot(bytes),object=imports(importObject),opts=options(compileOptions);
            return defer(()=>{const module=new Module(stable,opts);return {module,instance:new Instance(module,object)};});
        }catch(error){return rejected(error);}
    }
    function streaming(source,opts){
        // Promise<Response> と thenable の拒否理由はそのまま伝播する。
        const input=new P(resolve=>resolve(source));
        return react(input,response=>{
            // 捕捉した getter は Response の内部 brand を検査する。instanceof
            // や作者が書き換えられる ok / headers / arrayBuffer は使わない。
            const headers=apply(responseHeaders,response,[]),mime=apply(headerGet,headers,['content-type']);
            let start=0,end=mime===null?0:mime.length;
            const space=index=>{const c=apply(code,mime,[index]);return c===9||c===32;};
            while(start<end&&space(start))start++;
            while(end>start&&space(end-1))end--;
            if(mime===null||apply(lower,apply(slice,mime,[start,end]),[])!=='application/wasm')
                throw new TypeError('WebAssembly response MIME type must be application/wasm without parameters');
            const type=apply(responseType,response,[]),status=apply(responseStatus,response,[]);
            if(type!=='basic'&&type!=='cors'&&type!=='default')throw new TypeError('WebAssembly response is not CORS-same-origin');
            if(status<200||status>=300)throw new TypeError('WebAssembly response status is not successful');
            // Fetch consume は locked/disturbed/abort を検査し、既存 stream の
            // 所有権と backpressure を保って全 body を取得する。仕様が許す
            // buffered streaming であり、並列コンパイルの性能は主張しない。
            return react(apply(responseBytes,response,[]),bytes=>defer(()=>new Module(bytes,opts)));
        });
    }
    function compileStreaming(source,compileOptions=undefined){
        try{return streaming(source,options(compileOptions));}catch(error){return rejected(error);}
    }
    function instantiateStreaming(source,importObject=undefined,compileOptions=undefined){
        try{
            const object=imports(importObject),opts=options(compileOptions);
            return react(streaming(source,opts),module=>defer(()=>({module,instance:new Instance(module,object)})));
        }catch(error){return rejected(error);}
    }
    for(const [name,value]of Object.entries({compile,instantiate,compileStreaming,instantiateStreaming}))
        define(wasm,name,{value,writable:true,enumerable:true,configurable:true});
})();
