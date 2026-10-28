    /* Independent native HTML documents. No second JS DOM and no active loader. */
    const documentBridge = (() => {
        const parserBrands=new WeakSet(), serializerBrands=new WeakSet(), implementations=new WeakMap(), implementationBrands=new WeakMap();
        const formCollections=new WeakMap(), treeCollections=new WeakMap(), weakGet=WeakMap.prototype.get, weakSet=WeakMap.prototype.set;
        const filter=Array.prototype.filter, createObject=Object.create;
        const nodeTypeGetter=Object.getOwnPropertyDescriptor(Node.prototype,'nodeType').get;
        const StringImpl=String;
        function string(value){if(typeof value==='symbol')throw new TypeError('Cannot convert Symbol to DOMString');return StringImpl(value);}
        function brand(value){if(apply(nodeTypeGetter,value,[])!==9)throw new TypeError('Document receiver required');return value;}
        const HTML_NS='http://www.w3.org/1999/xhtml',SVG_NS='http://www.w3.org/2000/svg',
            XML_NS='http://www.w3.org/XML/1998/namespace',XMLNS_NS='http://www.w3.org/2000/xmlns/';
        // DOM "valid element local name" and "validate and extract" given "element".
        const elementLocalName=/^(?:[A-Za-z][^\0\t\n\f\r\x20/>]*|[:_\u0080-\u{10FFFF}][A-Za-z0-9\-.:_\u0080-\u{10FFFF}]*)$/u;
        function validLocalName(name){if(!elementLocalName.test(name))throw new DOMException('Invalid element name','InvalidCharacterError');}
        function extract(namespace,qualifiedName){
            if(namespace==='')namespace=null;
            let prefix=null,localName=qualifiedName;const colon=qualifiedName.indexOf(':');
            if(colon>=0){
                prefix=qualifiedName.slice(0,colon);localName=qualifiedName.slice(colon+1);
                if(!prefix || /[\0\t\n\f\r />]/.test(prefix))throw new DOMException('Invalid namespace prefix','InvalidCharacterError');
            }
            validLocalName(localName);
            if(prefix!==null && namespace===null || prefix==='xml' && namespace!==XML_NS ||
               (qualifiedName==='xmlns' || prefix==='xmlns') && namespace!==XMLNS_NS ||
               namespace===XMLNS_NS && qualifiedName!=='xmlns' && prefix!=='xmlns')
                throw new DOMException('Namespace and prefix disagree','NamespaceError');
            return {namespace,prefix,localName};
        }
        // DOM "internal createElementNS steps": an HTML-namespace element may be custom.
        function createNS(receiver,namespace,qualifiedName,options){
            const x=extract(namespace,qualifiedName);
            if(x.namespace===HTML_NS)return customElementsBridge.create(x.localName,options,true,receiver,x.prefix);
            return dom('create',receiver,1,x.localName,'',x.namespace,x.prefix);
        }
        const xmlTypes=['text/xml','application/xml','application/xhtml+xml','image/svg+xml'];
        function transfer(receiver,node,deep,adopt){
            brand(receiver);
            const kind=apply(nodeTypeGetter,node,[]);
            if(kind===9)throw new DOMException('A Document cannot be transferred','NotSupportedError');
            if(rawDom.get(node,'shadowHost'))throw new DOMException('A shadow root cannot be transferred directly',adopt?'HierarchyRequestError':'NotSupportedError');
            return dom(adopt?'adopt':'import',receiver,node,!!deep);
        }
        class DOMParser {
            constructor(){parserBrands.add(this);}
            parseFromString(input,type){
                if(!parserBrands.has(this))throw new TypeError('Illegal DOMParser receiver');
                if(arguments.length<2)throw new TypeError('parseFromString requires two arguments');
                input=string(input);type=string(type);
                if(type!=='text/html' && !xmlTypes.includes(type))throw new TypeError('Invalid parser MIME type');
                if(type==='text/html')return rawDom.parseDocument(null,input,false);
                // HTML: an XML parser with XML scripting support disabled; a
                // well-formedness error gives the parsererror document.
                return rawDom.parseXML(null,input,type,true);
            }
        }
        const serializerHas=WeakSet.prototype.has,serializerAdd=WeakSet.prototype.add;
        class XMLSerializer {
            constructor(){apply(serializerAdd,serializerBrands,[this]);}
            serializeToString(root){
                if(!apply(serializerHas,serializerBrands,[this]))throw new TypeError('Illegal XMLSerializer receiver');
                if(!arguments.length)throw new TypeError('serializeToString requires a Node');
                apply(nodeTypeGetter,root,[]); // Native Node brand, including another document's nodes.
                const serialized=rawDom.get(root,'xmlSerialization');
                if(serialized===null)throw new DOMException('The node cannot be serialized as XML','InvalidStateError');
                return serialized;
            }
        }
        Object.defineProperty(XMLSerializer.prototype,'serializeToString',{enumerable:true});
        Object.defineProperty(XMLSerializer.prototype,Symbol.toStringTag,{value:'XMLSerializer',configurable:true});
        class DOMImplementation {
            constructor(){throw new TypeError('Illegal DOMImplementation constructor');}
            createHTMLDocument(title){
                if(!implementationBrands.has(this))throw new TypeError('Illegal DOMImplementation receiver');
                const present=arguments.length>0;
                if(present)title=string(title);
                const made=rawDom.parseDocument(null,'<!doctype html><html><head></head><body></body></html>',true);
                if(present){const element=made.createElement('title');element.appendChild(made.createTextNode(title));made.head.appendChild(element);}
                return made;
            }
            createDocument(namespace,qualifiedName,doctype=null){
                if(!implementationBrands.has(this))throw new TypeError('Illegal DOMImplementation receiver');
                if(arguments.length<2)throw new TypeError('Namespace and qualified name are required');
                namespace=namespace==null?null:string(namespace);
                qualifiedName=qualifiedName===null?'':string(qualifiedName); // [LegacyNullToEmptyString]
                if(doctype!==null && doctype!==undefined && apply(nodeTypeGetter,doctype,[])!==10)throw new TypeError('DocumentType required');
                const x=qualifiedName===''?null:extract(namespace,qualifiedName);
                const made=rawDom.documentCreate(null,namespace===HTML_NS?'application/xhtml+xml':namespace===SVG_NS?'image/svg+xml':'application/xml',true);
                if(doctype!==null && doctype!==undefined)made.appendChild(doctype);
                if(x)made.appendChild(dom('create',made,1,x.localName,'',x.namespace,x.prefix));
                return made;
            }
            createDocumentType(name,publicId,systemId){
                const owner=implementationBrands.get(this);
                if(!owner)throw new TypeError('Illegal DOMImplementation receiver');
                if(arguments.length<3)throw new TypeError('Three doctype arguments are required');
                name=string(name);publicId=string(publicId);systemId=string(systemId);
                if(/[\0\t\n\f\r >]/.test(name))throw new DOMException('Invalid doctype name','InvalidCharacterError');
                return rawDom.doctypeCreate(owner,name,publicId,systemId);
            }
            hasFeature(){if(!implementationBrands.has(this))throw new TypeError('Illegal DOMImplementation receiver');return true;}
        }
        // DOMImplementation.createDocument's document; no constructor of its own.
        class XMLDocument extends Document { constructor(){throw new TypeError('Illegal XMLDocument constructor');} }
        Object.defineProperty(XMLDocument.prototype,Symbol.toStringTag,{value:'XMLDocument',configurable:true});
        Object.defineProperty(Document.prototype,'implementation',{configurable:true,get(){
            brand(this);let implementation=implementations.get(this);
            if(!implementation){implementation=Object.create(DOMImplementation.prototype);implementationBrands.set(implementation,this);implementations.set(this,implementation);}
            return implementation;
        }});
        for(const key of ['documentElement','head','body','doctype','activeElement','title','URL','documentURI','referrer','baseURI','contentType','characterSet','compatMode']){
            const descriptor={configurable:true,enumerable:true,get(){brand(this);return domGet(this,key);}};
            if(key==='title')descriptor.set=function(value){brand(this);dom('set',this,key,string(value));};
            Object.defineProperty(Document.prototype,key,descriptor);
        }
        for(const key of ['charset','inputEncoding'])Object.defineProperty(Document.prototype,key,{configurable:true,get(){brand(this);return domGet(this,'characterSet');}});
        for(const key of ['defaultView','location','currentScript'])Object.defineProperty(Document.prototype,key,{configurable:true,get(){brand(this);return this!==document?null:key==='defaultView'?globalThis:key==='location'?location:host.current();}});
        Object.defineProperty(Document.prototype,'readyState',{configurable:true,get(){brand(this);return this===document?host.ready():'complete';}});
        function treeCollection(owner,key,query){
            let cache=apply(weakGet,treeCollections,[owner]);
            if(!cache){cache=createObject(null);apply(weakSet,treeCollections,[owner,cache]);}
            if(!cache[key])cache[key]=collectionBridge.domHTML(owner,()=>apply(filter,dom('query',owner,query,false),[
                node=>rawDom.get(node,'namespaceURI')==='http://www.w3.org/1999/xhtml'
            ]));
            return cache[key];
        }
        // Native document-tree queries exclude shadow roots, frame documents,
        // inert template contents and non-HTML namespace lookalikes.
        for(const [key,query] of [['links','a[href],area[href]'],['images','img'],['scripts','script'],
            ['embeds','embed'],['anchors','a[name]']])Object.defineProperty(Document.prototype,key,{
            configurable:true,enumerable:true,get(){brand(this);return treeCollection(this,key,query);}
        });
        Object.defineProperty(Document.prototype,'plugins',{
            configurable:true,enumerable:true,get(){brand(this);return treeCollection(this,'embeds','embed');}
        });
        // [SameObject] live native-tree collection. Hidden forms participate,
        // but SVG lookalikes, shadow roots and inert template contents do not.
        Object.defineProperty(Document.prototype,'forms',{configurable:true,enumerable:true,get(){
            brand(this);let collection=apply(weakGet,formCollections,[this]);
            if(!collection){
                const owner=this;
                collection=collectionBridge.html(()=>apply(filter,dom('query',owner,'form',false),[
                    node=>rawDom.get(node,'namespaceURI')==='http://www.w3.org/1999/xhtml'
                ]));
                apply(weakSet,formCollections,[owner,collection]);
            }
            return collection;
        }});
        Document.prototype.getElementById=function(id){brand(this);if(!arguments.length)throw new TypeError('Missing id');return dom('id',this,string(id));};
        Document.prototype.elementFromPoint=function(x,y){
            brand(this);if(arguments.length<2)throw new TypeError('Two coordinates required');
            return rawDom.elementFromPoint(this,x,y);
        };
        Document.prototype.elementsFromPoint=function(x,y){
            brand(this);if(arguments.length<2)throw new TypeError('Two coordinates required');
            return rawDom.elementsFromPoint(this,x,y);
        };
        Document.prototype.getElementsByName=function(name){brand(this);if(!arguments.length)throw new TypeError('Missing name');const query='[name="'+CSS.escape(string(name))+'"]';return collectionBridge.domLive(this,()=>dom('query',this,query,false));};
        // DOM createElement: an HTML document lowercases; its namespace is HTML
        // for an HTML document or an application/xhtml+xml one, else null.
        Document.prototype.createElement=function(name){
            brand(this);if(!arguments.length)throw new TypeError('Missing name');
            const options=arguments[1];
            name=string(name);validLocalName(name);
            const html=rawDom.get(this,'htmlDocument');
            if(html || rawDom.get(this,'contentType')==='application/xhtml+xml')return customElementsBridge.create(name,options,!html,this);
            return dom('create',this,1,name,'',null,null);
        };
        Document.prototype.createElementNS=function(namespace,name){
            brand(this);
            if(arguments.length<2)throw new TypeError('Missing namespace or name');
            return createNS(this,namespace==null?null:string(namespace),string(name),arguments[2]);
        };
        for(const [method,kind,label] of [['createTextNode',3,'#text'],['createComment',8,'#comment']])Document.prototype[method]=function(value){brand(this);if(!arguments.length)throw new TypeError('Missing data');return dom('create',this,kind,label,string(value));};
        Document.prototype.createDocumentFragment=function(){brand(this);return dom('create',this,11,'#document-fragment','');};
        Document.prototype.createCDATASection=function(data){
            brand(this);if(!arguments.length)throw new TypeError('Missing data');data=string(data);
            if(rawDom.get(this,'htmlDocument'))throw new DOMException('CDATA sections cannot be created in an HTML document','NotSupportedError');
            if(data.includes(']]>'))throw new DOMException('CDATA section data cannot contain "]]>"','InvalidCharacterError');
            return dom('create',this,4,'#cdata-section',data);
        };
        // DOM "locate a namespace" and "locate a namespace prefix" on the native tree.
        Node.prototype.lookupPrefix=function(namespace){
            apply(nodeTypeGetter,this,[]);if(!arguments.length)throw new TypeError('Missing namespace');
            namespace=namespace==null?null:string(namespace);
            if(namespace===null || namespace==='')return null;
            return rawDom.namespaceLookup(this,namespace,true);
        };
        Node.prototype.lookupNamespaceURI=function(prefix){
            apply(nodeTypeGetter,this,[]);if(!arguments.length)throw new TypeError('Missing prefix');
            prefix=prefix==null?null:string(prefix);
            return rawDom.namespaceLookup(this,prefix===''?null:prefix,false);
        };
        Node.prototype.isDefaultNamespace=function(namespace){
            apply(nodeTypeGetter,this,[]);if(!arguments.length)throw new TypeError('Missing namespace');
            namespace=namespace==null?null:string(namespace);
            return rawDom.namespaceLookup(this,null,false)===(namespace===''?null:namespace);
        };
        Document.prototype.importNode=function(node,deep=false){if(!arguments.length)throw new TypeError('Missing node');return transfer(this,node,deep,false);};
        Document.prototype.adoptNode=function(node){if(!arguments.length)throw new TypeError('Missing node');return transfer(this,node,false,true);};
        const createEvent=document.createEvent;
        Document.prototype.hasFocus=function(){brand(this);return this===document && !!(host.windowState()&2);};
        /* HTML 6.2: the Document's visibility state, which only the "update the visibility state"
           task changes (host.visible); a document without a browsing context stays "hidden". */
        Object.defineProperties(Document.prototype,{
            hidden:{configurable:true,enumerable:true,get(){brand(this);return this!==document || !host.visible();}},
            visibilityState:{configurable:true,enumerable:true,get(){brand(this);return this===document && host.visible()?'visible':'hidden';}}
        });
        Document.prototype.createEvent=function(...args){brand(this);return apply(createEvent,this,args);};
        // HTML "document write steps": an XML document has no input stream.
        for(const method of ['write','writeln'])Document.prototype[method]=function(){
            brand(this);
            if(!rawDom.get(this,'htmlDocument'))throw new DOMException('An XML document cannot be written to','InvalidStateError');
            throw new DOMException('Writing to an inactive document is not implemented','NotSupportedError');
        };
        Object.assign(globalThis,{DOMParser,XMLSerializer,DOMImplementation,XMLDocument});
        return {brand,xmlDocumentPrototype:XMLDocument.prototype};
    })();
