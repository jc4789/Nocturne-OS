    /* Native Documents and independent QuickJS realms, behind WindowProxy
       guards that re-check the current origin after every frame navigation. */
    const frameBridge=(()=>{
        const proxies=new WeakMap(),locationProxies=new WeakMap();
        const methodNames=['addEventListener','removeEventListener','dispatchEvent','getComputedStyle','matchMedia','setTimeout','setInterval','clearTimeout','clearInterval','requestAnimationFrame','cancelAnimationFrame','requestIdleCallback','cancelIdleCallback','queueMicrotask'];
        const originalMethods=Object.create(null);
        for(const key of methodNames)originalMethods[key]=globalThis[key];
        let methodsFinalized=false;
        function finalizeMethods(){
            if(methodsFinalized)return;
            // Platform wrappers installed later in bootstrap are still native
            // delegates. Freeze their identities before any author execution;
            // subsequent author replacements remain ordinary property values.
            for(const key of methodNames)originalMethods[key]=globalThis[key];
            methodsFinalized=true;
        }
        function security(){throw new DOMException('Cross-origin Window access is forbidden','SecurityError');}
        function access(op,token,...args){
            try{return host.frame(op,token,...args);}catch(error){
                if(error && String(error.message).startsWith('SecurityError:'))security();
                throw error;
            }
        }
        function locationProxy(token){
            let location=locationProxies.get(token);if(location)return location;
            location=new Proxy(Object.create(null),{
                get(_,key){
                    if(key==='assign'||key==='replace')return value=>access('navigate',token,String(value),key==='replace'?2:0);
                    if(key==='href'||key==='toString'){
                        const target=access('get',token,'location');return key==='href'?target.href:()=>String(target);
                    }
                    if(typeof key==='symbol')return undefined;
                    return access('get',token,'location')[key];
                },
                set(_,key,value){if(key==='href'){access('navigate',token,String(value));return true;}access('get',token,'location')[key]=value;return true;}
            });locationProxies.set(token,location);return location;
        }
        function windowProxy(token){
            if(!token)return null;
            let proxy=proxies.get(token);if(proxy)return proxy;
            proxy=access('proxyGet',token);if(proxy){proxies.set(token,proxy);return proxy;}
            const methodDelegates=new WeakMap();
            proxy=new Proxy(Object.create(null),{
                get(_,key){
                    if(key==='window'||key==='self'||key==='frames')return proxy;
                    if(key==='parent'||key==='top')return windowProxy(access(key,token));
                    if(key==='opener')return access('opener',token);
                    if(key==='closed'||key==='length'||key==='frameElement')return access(key,token);
                    if(key==='location')return locationProxy(token);
                    if(key===Symbol.toStringTag)return 'Window';
                    if(typeof key==='symbol')return undefined;
                    if(/^(0|[1-9][0-9]*)$/.test(key))return windowProxy(access('index',token,Number(key)))||undefined;
                    // CrossOriginProperties' operations (frame_native.h crossOriginMethod)
                    if(key==='postMessage'||key==='focus'||key==='blur'||key==='close')return access('crossOriginMethod',token,key);
                    if(methodNames.includes(key)) {
                        const record=access('method',token,key),fn=record[0],documentToken=record[1];
                        // Author replacements are ordinary property values, not
                        // dynamically rebound native methods. Preserve their identity.
                        if(!record[2] || typeof fn!=='function')return fn;
                        let documents=methodDelegates.get(fn);
                        if(!documents)methodDelegates.set(fn,documents=new WeakMap());
                        let delegate=documents.get(documentToken);
                        if(!delegate){delegate=(...args)=>access('call',token,fn,args,documentToken);documents.set(documentToken,delegate);}
                        return delegate;
                    }
                    return access('get',token,key);
                },
                set(_,key,value){
                    if(key==='location'){access('navigate',token,String(value));return true;}
                    if(typeof key==='symbol')return false;
                    return access('set',token,key,value);
                },
                has(_,key){if(['window','self','frames','parent','top','opener','closed','length','location','postMessage','focus','blur','close'].includes(key))return true;return typeof key==='string'&&access('has',token,key);},
                defineProperty(){throw new DOMException('WindowProxy property definition is not implemented','NotSupportedError');},
                setPrototypeOf(){return false;},preventExtensions(){return false;}
            });proxy=access('proxySet',token,proxy);proxies.set(token,proxy);return proxy;
        }
        for(const [proto,tag] of [[HTMLIFrameElement.prototype,'iframe'],[HTMLFrameElement.prototype,'frame']]){
            Object.defineProperties(proto,{
                src:{configurable:true,enumerable:true,get(){return elementURL.attribute(this,tag,'src',false);},
                    set(value){htmlElementBrand(this,tag);reflectedAttr(this,'src',elementURL.scalar(value));}},
                name:{configurable:true,enumerable:true,get(){htmlElementBrand(this,tag);return reflectedAttr(this,'name')||'';},
                    set(value){htmlElementBrand(this,tag);reflectedAttr(this,'name',elementURL.string(value));}},
                contentWindow:{configurable:true,get(){return windowProxy(access('window',this));}},
                contentDocument:{configurable:true,get(){return access('document',this);}},
                srcdoc:{configurable:true,get(){return reflectedAttr(this,'srcdoc')||'';},set(value){reflectedAttr(this,'srcdoc',String(value));}},
                width:{configurable:true,get(){return reflectedAttr(this,'width')||'';},set(value){reflectedAttr(this,'width',String(value));}},
                height:{configurable:true,get(){return reflectedAttr(this,'height')||'';},set(value){reflectedAttr(this,'height',String(value));}}
            });
        }
        document.open=function(){
            if(arguments.length>2)throw new DOMException('Window-opening overload is not implemented','NotSupportedError');
            if(customElementsBridge.constructing())throw new DOMException('document.open during custom element construction','InvalidStateError');
            return host.documentStream(0);
        };
        document.close=function(){host.documentStream(1);};
        const selfToken=host.frame('self',null);
        /* WebIDLのnamed-properties object。普通のglobal own propertyへidを
           固定配置せず、作者var/代入・prototype built-inの優先を保全する。 */
        const namedGetOwn=Reflect.getOwnPropertyDescriptor,namedGetPrototype=Reflect.getPrototypeOf,
              namedGet=Reflect.get,namedHas=Reflect.has,namedSet=Reflect.set,namedOwnKeys=Reflect.ownKeys,
              namedDefine=Object.defineProperty,namedApply=Reflect.apply;
        const NamesMap=Map,mapGet=Map.prototype.get,mapSet=Map.prototype.set;
        const lookup=(map,key)=>namedApply(mapGet,map,[key]);
        const store=(map,key,value)=>namedApply(mapSet,map,[key,value]);
        let namedOwner=null,namedRevision=null,namedElements=new NamesMap(),namedChildren=new NamesMap();
        function refreshNames(){
            const version=host.frame('namedVersion',selfToken);
            if(version[0]===namedOwner&&version[1]===namedRevision)return;
            const rows=host.frame('namedElements',selfToken),elements=new NamesMap(),children=new NamesMap();
            const add=(name,node)=>{if(!name)return;let a=lookup(elements,name);if(!a)store(elements,name,a=[]);
                namedDefine(a,a.length,{value:node,writable:true,enumerable:true,configurable:true});};
            for(let i=0;i<rows.length;i++){
                const row=rows[i];add(row[1],row[0]);if(row[2]!==row[1])add(row[2],row[0]);
                if(row[3]&&!lookup(children,row[3]))store(children,row[3],row[0]);
            }
            namedOwner=version[0];namedRevision=version[1];namedElements=elements;namedChildren=children;
        }
        function namedValue(key){
            if(typeof key!=='string'||!key)return undefined;
            refreshNames();
            const matches=lookup(namedElements,key),child=lookup(namedChildren,key);
            if(child){const record=host.frame('namedChild',child);
                // supported-name生成は最初の同名childだけを判定。後のchildで
                // cross-origin最初のchildを隠して名前を復活させない。
                if(record[0]&&((matches&&matches.length)||record[1]))return windowProxy(record[0]);}
            if(!matches||!matches.length)return undefined;
            if(matches.length===1)return matches[0];
            // 保持されたcollectionは0/1要素へ変わってもDocumentへ根を持ち続ける。
            return collectionBridge.html(()=>{refreshNames();return lookup(namedElements,key)||[];});
        }
        let namedObject;
        // An array index is never an own property of the Window: the
        // WindowProxy refuses to define one, and the global object's indexed
        // properties are the WindowProxy's (its child navigables), which the
        // visibility algorithm does not ask. Template and >>> do not call
        // author code.
        const arrayIndex=key=>{const n=+key;return (n>>>0)===n&&n!==4294967295&&`${n}`===key;};
        function visibleName(key){
            if(typeof key!=='string'||!arrayIndex(key)&&namedGetOwn(globalThis,key))return false;
            for(let p=namedGetPrototype(globalThis);p;p=namedGetPrototype(p))
                if(p!==namedObject&&namedGetOwn(p,key))return false;
            return true;
        }
        const namedTarget=Object.create(EventTarget.prototype);
        namedDefine(namedTarget,Symbol.toStringTag,{value:'WindowProperties',configurable:true});
        namedObject=new Proxy(namedTarget,{
            get(t,key,receiver){if(visibleName(key)){const value=namedValue(key);if(value!==undefined)return value;}
                return namedGet(t,key,receiver);},
            has(t,key){return visibleName(key)&&namedValue(key)!==undefined||namedHas(t,key);},
            getOwnPropertyDescriptor(t,key){if(visibleName(key)){const value=namedValue(key);if(value!==undefined)
                return {value,writable:true,enumerable:false,configurable:true};}return namedGetOwn(t,key);},
            ownKeys(t){return namedOwnKeys(t);},
            set(t,key,value,receiver){return namedSet(t,key,value,receiver);},
            defineProperty(){return false;},deleteProperty(){return false;},
            setPrototypeOf(t,proto){return proto===namedGetPrototype(t);},preventExtensions(){return false;}
        });
        class Window extends EventTarget {constructor(){throw new TypeError('Illegal Window constructor');}}
        Object.setPrototypeOf(Window.prototype,namedObject);
        namedDefine(Window.prototype,Symbol.toStringTag,{value:'Window',configurable:true});
        namedDefine(globalThis,'Window',{value:Window,writable:true,configurable:true});
        Object.setPrototypeOf(globalThis,Window.prototype);
        // QuickJS creates a generic own "global" tag. Browser globals inherit
        // the WebIDL Window class string instead of shadowing that prototype.
        delete globalThis[Symbol.toStringTag];
        Object.defineProperty(document,'defaultView',{configurable:true,get(){return host.frame('top',selfToken)===selfToken?globalThis:windowProxy(selfToken);}});
        Object.defineProperty(Document.prototype,'defaultView',{configurable:true,get(){if(rawDom.get(this,'nodeType')!==9)throw new TypeError('Document receiver required');return this===document?document.defaultView:null;}});
        /* HTML: an auxiliary browsing context is a top-level one with an opener browsing
           context. Native new windows are independent (never fake opener proxies), there is
           no window.open, and child browsing contexts have no opener relationship: this
           browsing context never has an opener, so it is never auxiliary. */
        const openerContext=null;
        const scriptCreatedAuxiliary=()=>openerContext!==null && host.frame('top',selfToken)===selfToken;
        Object.defineProperties(globalThis,{
            opener:{configurable:true,enumerable:true,get(){return openerContext;},set(value){if(value!==null)Object.defineProperty(this,'opener',{value,writable:true,enumerable:true,configurable:true});}},
            parent:{configurable:true,get(){const token=host.frame('parent',selfToken);return token===selfToken?globalThis:windowProxy(token);}},
            top:{configurable:true,get(){const token=host.frame('top',selfToken);return token===selfToken?globalThis:windowProxy(token);}},
            frameElement:{configurable:true,get(){return access('frameElement',selfToken);}},
            // HTML: true when this Window's navigable is null or is closing.
            closed:{configurable:true,get(){return access('closed',selfToken);}},
            // As defaultView: a top-level Window's WindowProxy is its global
            // object, which answers frames[i] itself (quickjs.c's global index
            // methods); a child's is the proxy its embedder holds.
            frames:{configurable:true,get(){return host.frame('top',selfToken)===selfToken?globalThis:windowProxy(selfToken);}},
            length:{configurable:true,get(){return access('length',selfToken);}},
            name:{configurable:true,enumerable:true,
                get(){if(this!==globalThis)throw new TypeError('Illegal Window receiver');return access('name',selfToken);},
                set(value){if(this!==globalThis)throw new TypeError('Illegal Window receiver');access('name',selfToken,String(value));}}
        });
        return {windowProxy,contentWindow:element=>windowProxy(access('window',element)),methodOriginal:key=>originalMethods[key],finalizeMethods,scriptCreatedAuxiliary};
    })();
