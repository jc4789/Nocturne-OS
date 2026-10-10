    /* Native Documents and independent QuickJS realms, behind WindowProxy
       guards that re-check the current origin after every frame navigation. */
    const frameBridge=(()=>{
        const proxies=new WeakMap(),locationProxies=new WeakMap();
        const methodNames=['addEventListener','removeEventListener','dispatchEvent','getComputedStyle','matchMedia','setTimeout','setInterval','clearTimeout','clearInterval','requestAnimationFrame','cancelAnimationFrame','requestIdleCallback','cancelIdleCallback','queueMicrotask'];
        const originalMethods=Object.create(null);
        for(const key of methodNames)originalMethods[key]=globalThis[key];
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
                    if(key==='assign'||key==='replace')return value=>access('navigate',token,String(value));
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
                    if(key==='postMessage')return access('messageMethod',token);
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
                has(_,key){if(['window','self','frames','parent','top','opener','closed','length','location','postMessage'].includes(key))return true;return typeof key==='string'&&access('has',token,key);},
                defineProperty(){throw new DOMException('WindowProxy property definition is not implemented','NotSupportedError');},
                setPrototypeOf(){return false;},preventExtensions(){return false;}
            });proxy=access('proxySet',token,proxy);proxies.set(token,proxy);return proxy;
        }
        for(const proto of [HTMLIFrameElement.prototype,HTMLFrameElement.prototype]){
            Object.defineProperties(proto,{
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
        Object.defineProperty(document,'defaultView',{configurable:true,get(){return host.frame('top',selfToken)===selfToken?globalThis:windowProxy(selfToken);}});
        Object.defineProperty(Document.prototype,'defaultView',{configurable:true,get(){if(rawDom('get',this,'nodeType')!==9)throw new TypeError('Document receiver required');return this===document?document.defaultView:null;}});
        Object.defineProperties(globalThis,{
            // Native new windows are independent, never fake opener proxies.
            // Child browsing contexts likewise have no opener relationship.
            opener:{configurable:true,enumerable:true,get(){return null;},set(value){if(value!==null)Object.defineProperty(this,'opener',{value,writable:true,enumerable:true,configurable:true});}},
            parent:{configurable:true,get(){const token=host.frame('parent',selfToken);return token===selfToken?globalThis:windowProxy(token);}},
            top:{configurable:true,get(){const token=host.frame('top',selfToken);return token===selfToken?globalThis:windowProxy(token);}},
            frameElement:{configurable:true,get(){return access('frameElement',selfToken);}},
            frames:{configurable:true,get(){return windowProxy(selfToken);}},
            length:{configurable:true,get(){return access('length',selfToken);}}
        });
        return {windowProxy,methodOriginal:key=>originalMethods[key]};
    })();
