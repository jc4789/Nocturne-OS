    /* Independent native HTML documents. No second JS DOM and no active loader. */
    const documentBridge = (() => {
        const parserBrands=new WeakSet(), implementations=new WeakMap(), implementationBrands=new WeakMap();
        const formCollections=new WeakMap(), weakGet=WeakMap.prototype.get, weakSet=WeakMap.prototype.set;
        const filter=Array.prototype.filter;
        const nodeTypeGetter=Object.getOwnPropertyDescriptor(Node.prototype,'nodeType').get;
        const StringImpl=String;
        function string(value){if(typeof value==='symbol')throw new TypeError('Cannot convert Symbol to DOMString');return StringImpl(value);}
        function brand(value){if(apply(nodeTypeGetter,value,[])!==9)throw new TypeError('Document receiver required');return value;}
        function transfer(receiver,node,deep,adopt){
            brand(receiver);
            const kind=apply(nodeTypeGetter,node,[]);
            if(kind===9)throw new DOMException('A Document cannot be transferred','NotSupportedError');
            if(rawDom('get',node,'shadowHost'))throw new DOMException('A shadow root cannot be transferred directly',adopt?'HierarchyRequestError':'NotSupportedError');
            return dom(adopt?'adopt':'import',receiver,node,!!deep);
        }
        class DOMParser {
            constructor(){parserBrands.add(this);}
            parseFromString(input,type){
                if(!parserBrands.has(this))throw new TypeError('Illegal DOMParser receiver');
                if(arguments.length<2)throw new TypeError('parseFromString requires two arguments');
                input=string(input);type=string(type);
                if(!['text/html','text/xml','application/xml','application/xhtml+xml','image/svg+xml'].includes(type))throw new TypeError('Invalid parser MIME type');
                if(type!=='text/html')throw new DOMException('XML parsing is not implemented','NotSupportedError');
                return rawDom('parseDocument',null,input,false);
            }
        }
        class DOMImplementation {
            constructor(){throw new TypeError('Illegal DOMImplementation constructor');}
            createHTMLDocument(title){
                if(!implementationBrands.has(this))throw new TypeError('Illegal DOMImplementation receiver');
                const present=arguments.length>0;
                if(present)title=string(title);
                const made=rawDom('parseDocument',null,'<!doctype html><html><head></head><body></body></html>',true);
                if(present){const element=made.createElement('title');element.appendChild(made.createTextNode(title));made.head.appendChild(element);}
                return made;
            }
            createDocument(){if(!implementationBrands.has(this))throw new TypeError('Illegal DOMImplementation receiver');throw new DOMException('XML documents are not implemented','NotSupportedError');}
            createDocumentType(){if(!implementationBrands.has(this))throw new TypeError('Illegal DOMImplementation receiver');throw new DOMException('DocumentType creation is not implemented','NotSupportedError');}
        }
        Object.defineProperty(Document.prototype,'implementation',{configurable:true,get(){
            brand(this);let implementation=implementations.get(this);
            if(!implementation){implementation=Object.create(DOMImplementation.prototype);implementationBrands.set(implementation,this);implementations.set(this,implementation);}
            return implementation;
        }});
        for(const key of ['documentElement','head','body','doctype','activeElement','title','URL','documentURI','referrer','baseURI','contentType','characterSet','compatMode']){
            const descriptor={configurable:true,enumerable:true,get(){brand(this);return dom('get',this,key);}};
            if(key==='title')descriptor.set=function(value){brand(this);dom('set',this,key,string(value));};
            Object.defineProperty(Document.prototype,key,descriptor);
        }
        for(const key of ['charset','inputEncoding'])Object.defineProperty(Document.prototype,key,{configurable:true,get(){brand(this);return dom('get',this,'characterSet');}});
        for(const key of ['defaultView','location','currentScript'])Object.defineProperty(Document.prototype,key,{configurable:true,get(){brand(this);return this!==document?null:key==='defaultView'?globalThis:key==='location'?location:host.current();}});
        Object.defineProperty(Document.prototype,'readyState',{configurable:true,get(){brand(this);return this===document?host.ready():'complete';}});
        // [SameObject] live native-tree collection. Hidden forms participate,
        // but SVG lookalikes, shadow roots and inert template contents do not.
        Object.defineProperty(Document.prototype,'forms',{configurable:true,enumerable:true,get(){
            brand(this);let collection=apply(weakGet,formCollections,[this]);
            if(!collection){
                const owner=this;
                collection=collectionBridge.html(()=>apply(filter,dom('query',owner,'form',false),[
                    node=>rawDom('get',node,'namespaceURI')==='http://www.w3.org/1999/xhtml'
                ]));
                apply(weakSet,formCollections,[owner,collection]);
            }
            return collection;
        }});
        Document.prototype.getElementById=function(id){brand(this);if(!arguments.length)throw new TypeError('Missing id');return dom('id',this,string(id));};
        Document.prototype.getElementsByName=function(name){brand(this);if(!arguments.length)throw new TypeError('Missing name');const query='[name="'+CSS.escape(string(name))+'"]';return collectionBridge.live(()=>dom('query',this,query,false));};
        function create(receiver,name,options,foreign){
            brand(receiver);name=string(name);
            if(receiver===document && !foreign)return customElementsBridge.create(name,options);
            if(options && options.is!==undefined)throw new DOMException('Customized built-ins are not implemented','NotSupportedError');
            if(!/^[A-Za-z_\u0080-\uFFFF][A-Za-z0-9_.:\-\u0080-\uFFFF]*$/.test(name))throw new DOMException('Invalid element name','InvalidCharacterError');
            return dom('create',receiver,1,name,'',!!foreign);
        }
        Document.prototype.createElement=function(name,options){if(!arguments.length)throw new TypeError('Missing name');return create(this,name,options,false);};
        Document.prototype.createElementNS=function(namespace,name,options){
            brand(this);
            if(arguments.length<2)throw new TypeError('Missing namespace or name');
            namespace=namespace==null?null:string(namespace);
            if(namespace!=='http://www.w3.org/1999/xhtml' && namespace!=='http://www.w3.org/2000/svg')throw new DOMException('Unsupported namespace','NotSupportedError');
            return create(this,name,options,namespace==='http://www.w3.org/2000/svg');
        };
        for(const [method,kind,label] of [['createTextNode',3,'#text'],['createComment',8,'#comment']])Document.prototype[method]=function(value){brand(this);if(!arguments.length)throw new TypeError('Missing data');return dom('create',this,kind,label,string(value));};
        Document.prototype.createDocumentFragment=function(){brand(this);return dom('create',this,11,'#document-fragment','');};
        Document.prototype.importNode=function(node,deep=false){if(!arguments.length)throw new TypeError('Missing node');return transfer(this,node,deep,false);};
        Document.prototype.adoptNode=function(node){if(!arguments.length)throw new TypeError('Missing node');return transfer(this,node,false,true);};
        const createEvent=document.createEvent;
        Document.prototype.createEvent=function(...args){brand(this);return apply(createEvent,this,args);};
        for(const method of ['write','writeln'])Document.prototype[method]=function(){brand(this);throw new DOMException('Writing to an inactive document is not implemented','NotSupportedError');};
        Object.assign(globalThis,{DOMParser,DOMImplementation});
        return {brand};
    })();
