/* New parser/registry cases use the real native Nocturne DOM. These are
 * supporting regressions, never substitutes for visible real-site testing. */
async function runParserCustomElementCases(){
    let count=0,next=0;
    const prefix='nocturne-parser-ce-'+Date.now()+'-',name=()=>prefix+(++next);
    const equal=(a,b)=>{count++;if(!Object.is(a,b))throw new Error(String(a)+' != '+String(b));};
    const throws=(fn,kind)=>{count++;try{fn();}catch(e){if(e.name===kind)return;throw e;}throw new Error('Expected '+kind);};
    const root=document.createElement('div');document.body.appendChild(root);
    try{
        equal(document.customElementRegistry,customElements);
        // Exercise the metadata-only clone fast path before any definition.
        const emptyRegistry=new CustomElementRegistry(),emptyName=name();
        const emptySource=document.createElement(emptyName,{customElementRegistry:emptyRegistry});
        const emptyDocument=Document.parseHTMLUnsafe('<p>empty registry</p>');
        const emptyImported=emptyDocument.importNode(emptySource,true);emptyDocument.body.appendChild(emptyImported);
        class EmptyImported extends HTMLElement{constructor(){super();this.fastImported=13;}}
        equal(emptyImported instanceof EmptyImported,false);emptyRegistry.define(emptyName,EmptyImported);
        equal(emptyImported instanceof EmptyImported,true);equal(emptyImported.fastImported,13);
        const first=new CustomElementRegistry(),second=new CustomElementRegistry(),shared=name();
        class One extends HTMLElement{constructor(){super();this.registryToken=1;}}
        class Two extends HTMLElement{constructor(){super();this.registryToken=2;}}
        first.define(shared,One);second.define(shared,Two);
        equal(first.get(shared),One);equal(second.get(shared),Two);equal(customElements.get(shared),undefined);
        equal(first.getName(One),shared);equal(second.getName(One),null);
        const a=document.createElement(shared,{customElementRegistry:first}),b=document.createElement(shared,{customElementRegistry:second});
        equal(a instanceof One,true);equal(b instanceof Two,true);equal(a.registryToken,1);equal(b.registryToken,2);
        equal(a.ownerDocument,document);equal(a.getAttribute('is'),null);
        root.appendChild(a);root.appendChild(b);equal(root.firstChild,a);equal(root.lastChild,b);
        throws(()=>new One(),'TypeError');
        throws(()=>document.createElement(name(),{customElementRegistry:{}}),'TypeError');
        throws(()=>first.get.call({},shared),'TypeError');
        throws(()=>first.define(name(),class extends HTMLButtonElement {},{extends:'button'}),'NotSupportedError');
        throws(()=>customElements.define(name(),class extends HTMLElement {},{extends:'unrecognized-nocturne-element'}),'NotSupportedError');

        // One constructor can be registered independently in scoped registries.
        const common=name();class Common extends HTMLElement{constructor(){super();this.common=true;}}
        first.define(common,Common);second.define(common,Common);
        equal(document.createElement(common,{customElementRegistry:first}) instanceof Common,true);
        equal(document.createElement(common,{customElementRegistry:second}) instanceof Common,true);
        const lateName=name(),late=document.createElement(lateName,{customElementRegistry:first});root.appendChild(late);
        let upgrades=0;class Late extends HTMLElement{constructor(){super();upgrades++;}}
        second.define(lateName,class extends HTMLElement {});equal(upgrades,0);equal(late instanceof Late,false);
        const pending=first.whenDefined(lateName);equal(pending,first.whenDefined(lateName));
        first.define(lateName,Late);equal(await pending,Late);equal(upgrades,1);equal(late instanceof Late,true);
        second.upgrade(late);first.upgrade(late);equal(upgrades,1);

        const builtin=name(),events=[];
        class Button extends HTMLButtonElement{
            static observedAttributes=['data-check'];
            constructor(){super();this.buttonToken=7;events.push('ctor:'+this.localName);}
            connectedCallback(){events.push('connected');}
            attributeChangedCallback(n,o,v){events.push(n+':'+o+':'+v);}
        }
        customElements.define(builtin,Button,{extends:'button'});
        const button=document.createElement('button',{is:builtin}),direct=new Button();
        equal(button instanceof Button,true);equal(button instanceof HTMLButtonElement,true);equal(button.localName,'button');
        equal(button.buttonToken,7);equal(direct.localName,'button');equal(direct instanceof Button,true);
        equal(button.hasAttribute('is'),false);equal(button.outerHTML.includes('is="'+builtin+'"'),true);
        button.setAttribute('data-check','one');root.appendChild(button);
        equal(events.join('|'),'ctor:button|ctor:button|data-check:null:one|connected');
        throws(()=>button.attachInternals(),'NotSupportedError');
        const plain=document.createElement('button');plain.setAttribute('is',builtin);root.appendChild(plain);customElements.upgrade(plain);
        equal(plain instanceof Button,false);
        const cloned=button.cloneNode(true);equal(cloned.localName,'button');equal(cloned instanceof Button,true);equal(cloned.buttonToken,7);
        const parsed=document.createElement('div');parsed.innerHTML='<button is="'+builtin+'" data-check="parsed">child</button>';
        equal(parsed.firstChild instanceof Button,true);equal(parsed.firstChild.textContent,'child');
        const lateBuiltin=name(),lateHolder=document.createElement('div');root.appendChild(lateHolder);
        lateHolder.innerHTML='<button is="'+lateBuiltin+'">late</button>';
        const lateButton=lateHolder.firstChild;lateButton.setAttribute('is','changed-after-creation');
        let builtUpgrades=0;class LateButton extends HTMLButtonElement{constructor(){super();builtUpgrades++;}}
        customElements.define(lateBuiltin,LateButton,{extends:'button'});
        equal(lateButton instanceof LateButton,true);equal(builtUpgrades,1);
        equal(lateButton.getAttribute('is'),'changed-after-creation');equal(lateButton.cloneNode(true) instanceof LateButton,true);
        const divName=name();class Div extends HTMLDivElement{}customElements.define(divName,Div,{extends:'div'});
        equal(document.createElement('div',{is:divName}) instanceof Div,true);equal(new Div().localName,'div');

        // Null registry is deliberately inert until initialize is requested.
        const nullName=name(),nullElement=document.createElement(nullName,{customElementRegistry:null});
        const fragment=document.createDocumentFragment();fragment.appendChild(nullElement);
        class Initialized extends HTMLElement{constructor(){super();this.initialized=true;}}
        first.define(nullName,Initialized);equal(nullElement instanceof Initialized,false);
        first.upgrade(fragment);equal(nullElement instanceof Initialized,false);
        first.initialize(fragment);equal(nullElement instanceof Initialized,true);equal(nullElement.initialized,true);
        second.initialize(fragment);equal(nullElement instanceof Initialized,true);
        throws(()=>customElements.initialize(document),'NotSupportedError');

        // Template parsing stays inert; importing its contents supplies the
        // destination registry before the clone's normal upgrade reactions.
        const importName=name();let importedConstructors=0;
        class Imported extends HTMLElement{constructor(){super();importedConstructors++;}reposition(){return 7;}}
        customElements.define(importName,Imported);
        const template=document.createElement('template');template.innerHTML='<'+importName+'><b>child</b></'+importName+'>';
        equal(template.content.firstChild instanceof Imported,false);equal(importedConstructors,0);
        const inertClone=template.content.cloneNode(true);
        equal(inertClone.firstChild instanceof Imported,false);equal(importedConstructors,0);
        const imported=document.importNode(template.content,true);
        equal(imported.firstChild instanceof Imported,true);equal(imported.firstChild.reposition(),7);
        equal(imported.firstChild.ownerDocument,document);equal(imported.firstChild.firstChild.textContent,'child');
        equal(importedConstructors,1);equal(imported.firstChild.isConnected,false);
        const templateCopy=document.importNode(template,true);
        equal(templateCopy.content.firstChild instanceof Imported,false);
        equal(templateCopy.content.firstChild.ownerDocument,templateCopy.content.ownerDocument);
        equal(templateCopy.content.ownerDocument===document,false);equal(importedConstructors,1);
        const deepTemplate=document.createElement('template');
        deepTemplate.innerHTML='<div>'.repeat(24)+'<'+importName+'></'+importName+'>'+'</div>'.repeat(24);
        equal(document.importNode(deepTemplate.content,true).querySelector(importName) instanceof Imported,true);
        equal(deepTemplate.content.cloneNode(true).querySelector(importName) instanceof Imported,false);
        equal(document.importNode(a,true) instanceof One,true);
        const inertDocument=Document.parseHTMLUnsafe('<p>inert</p>');
        equal(inertDocument.customElementRegistry,null);
        const beforeInertImport=importedConstructors,inertImported=inertDocument.importNode(imported.firstChild,true);
        equal(inertImported instanceof Imported,false);equal(inertImported.ownerDocument,inertDocument);
        equal(inertImported.firstChild.ownerDocument,inertDocument);equal(importedConstructors,beforeInertImport);
        equal(inertDocument.createElement(importName) instanceof Imported,false);
        // A null-registry template clone stays inert while detached, then the
        // cross-document adopting steps choose the active effective global.
        const adoptedClone=template.content.firstChild.cloneNode(true),beforeAdopt=importedConstructors;
        equal(adoptedClone instanceof Imported,false);customElements.upgrade(adoptedClone);equal(importedConstructors,beforeAdopt);
        root.appendChild(adoptedClone);
        equal(adoptedClone instanceof Imported,true);equal(adoptedClone.reposition(),7);
        equal(adoptedClone.ownerDocument,document);equal(adoptedClone.firstChild.ownerDocument,document);
        equal(importedConstructors,beforeAdopt+1);equal(template.content.firstChild instanceof Imported,false);
        adoptedClone.remove();
        const sameDocumentNull=document.createElement(importName,{customElementRegistry:null}),beforeSameDocument=importedConstructors;
        document.adoptNode(sameDocumentNull);root.appendChild(sameDocumentNull);
        customElements.upgrade(sameDocumentNull);equal(importedConstructors,beforeSameDocument);equal(sameDocumentNull instanceof Imported,false);sameDocumentNull.remove();
        const imperativeHost=inertDocument.createElement('div'),imperativeRoot=imperativeHost.attachShadow({mode:'open',customElementRegistry:null});
        const imperativeChild=inertDocument.createElement(importName,{customElementRegistry:null});imperativeRoot.appendChild(imperativeChild);
        root.appendChild(imperativeHost);
        equal(imperativeRoot.customElementRegistry,customElements);equal(imperativeChild instanceof Imported,true);
        equal(imperativeChild.ownerDocument,document);imperativeHost.remove();
        const declarativeDocument=Document.parseHTMLUnsafe('<div><template shadowrootmode="open" shadowrootcustomelementregistry><'+importName+'></'+importName+'></template></div>');
        const declarativeHost=declarativeDocument.body.firstChild,declarativeRoot=declarativeHost.shadowRoot;
        root.appendChild(declarativeHost);
        equal(declarativeRoot.customElementRegistry,null);equal(declarativeRoot.firstChild instanceof Imported,false);
        equal(declarativeRoot.firstChild.ownerDocument,document);declarativeHost.remove();
        const scopedParentName=name(),scopedParent=inertDocument.createElement(scopedParentName,{customElementRegistry:first}),nullDescendant=inertDocument.createElement(shared,{customElementRegistry:null});
        class GlobalParent extends HTMLElement{}class ScopedParent extends HTMLElement{parentToken(){return 19;}}
        class GlobalShared extends HTMLElement{}
        customElements.define(scopedParentName,GlobalParent);customElements.define(shared,GlobalShared);
        scopedParent.appendChild(nullDescendant);root.appendChild(scopedParent);
        equal(scopedParent instanceof GlobalParent,false);first.define(scopedParentName,ScopedParent);
        equal(scopedParent instanceof ScopedParent&&scopedParent.parentToken()===19,true);
        first.upgrade(nullDescendant);customElements.upgrade(nullDescendant);
        equal(nullDescendant instanceof One||nullDescendant instanceof GlobalShared,false);equal(nullDescendant.ownerDocument,document);scopedParent.remove();
        const lateImportedName=name(),lateImportedSource=document.createElement(lateImportedName,{customElementRegistry:first});
        const lateImported=inertDocument.importNode(lateImportedSource,true);inertDocument.body.appendChild(lateImported);
        class LateImported extends HTMLElement{constructor(){super();this.importedToken=11;}}
        equal(lateImported instanceof LateImported,false);first.define(lateImportedName,LateImported);
        equal(lateImported instanceof LateImported,true);equal(lateImported.importedToken,11);

        // Clonable shadow children intentionally receive no import fallback,
        // even though ordinary null-registry descendants use it.
        const cloneHost=document.createElement('div');
        const cloneShadow=cloneHost.attachShadow({mode:'open',clonable:true,customElementRegistry:null});
        cloneShadow.appendChild(document.createElement(importName,{customElementRegistry:null}));
        const importedHost=document.importNode(cloneHost,true);
        equal(importedHost.shadowRoot.customElementRegistry,null);
        equal(importedHost.shadowRoot.firstChild instanceof Imported,false);
        equal(importedHost.shadowRoot.firstChild.ownerDocument,document);
        const scopedCloneHost=document.createElement('div');
        const scopedCloneShadow=scopedCloneHost.attachShadow({mode:'open',clonable:true,customElementRegistry:first});
        scopedCloneShadow.appendChild(document.createElement(shared,{customElementRegistry:null}));
        const importedScopedHost=document.importNode(scopedCloneHost,true);
        equal(importedScopedHost.shadowRoot.customElementRegistry,first);
        equal(importedScopedHost.shadowRoot.firstChild instanceof One,false);
        first.initialize(importedScopedHost.shadowRoot);
        equal(importedScopedHost.shadowRoot.firstChild instanceof One,true);

        const host=document.createElement('div');root.appendChild(host);
        const shadow=host.attachShadow({mode:'open',customElementRegistry:first});
        equal(shadow.customElementRegistry,first);equal(shadow.host,host);
        shadow.innerHTML='<'+shared+'></'+shared+'>';
        equal(shadow.firstChild instanceof One,true);equal(shadow.firstChild.registryToken,1);
        shadow.setHTML('<'+shared+'></'+shared+'>',{sanitizer:{}});
        equal(shadow.firstChild instanceof One,true);equal(shadow.firstChild.registryToken,1);
        equal(shadow.firstChild.ownerDocument,document);equal(shadow.customElementRegistry,first);
        shadow.setHTMLUnsafe('<'+shared+'></'+shared+'>');
        equal(shadow.firstChild instanceof One,true);equal(shadow.firstChild.registryToken,1);
        equal(shadow.firstChild.ownerDocument,document);equal(shadow.customElementRegistry,first);
        const nullHost=document.createElement('div');root.appendChild(nullHost);
        const nullShadow=nullHost.attachShadow({mode:'closed',customElementRegistry:null});
        equal(nullShadow.customElementRegistry,null);equal(nullHost.shadowRoot,null);
        nullShadow.innerHTML='<'+nullName+'></'+nullName+'>';
        equal(nullShadow.firstChild instanceof Initialized,false);
        nullShadow.setHTML('<'+nullName+'></'+nullName+'>',{sanitizer:{}});
        equal(nullShadow.firstChild instanceof Initialized,false);equal(nullShadow.customElementRegistry,null);
        equal(nullShadow.firstChild.ownerDocument,document);
        nullShadow.setHTMLUnsafe('<'+nullName+'></'+nullName+'>');
        equal(nullShadow.firstChild instanceof Initialized,false);equal(nullShadow.customElementRegistry,null);
        equal(nullShadow.firstChild.ownerDocument,document);
        first.initialize(nullShadow);equal(nullShadow.customElementRegistry,first);equal(nullShadow.firstChild instanceof Initialized,true);
        const globalHost=document.createElement('div');root.appendChild(globalHost);
        equal(globalHost.attachShadow({mode:'open'}).customElementRegistry,customElements);
        const nestedHost=document.createElement('div',{customElementRegistry:null});root.appendChild(nestedHost);
        const nestedShadow=nestedHost.attachShadow({mode:'closed',customElementRegistry:null});
        nestedShadow.innerHTML='<'+shared+'></'+shared+'>';
        first.initialize(nestedHost);equal(nestedShadow.customElementRegistry,null);equal(nestedShadow.firstChild instanceof One,false);
        first.initialize(nestedShadow);equal(nestedShadow.customElementRegistry,first);equal(nestedShadow.firstChild instanceof One,true);
        return count;
    }finally{root.remove();}
}

// Called by a parser-stream integration case after the preceding script has
// defined its names. The constructor must observe no token attrs/children and
// must not see later input; this cannot be tested by post-parse upgrade alone.
function installParserCustomElementTiming(name,log){
    class Timing extends HTMLElement{
        static observedAttributes=['data-token'];
        constructor(){
            super();log.push(['ctor',this.attributes.length,this.childNodes.length,this.isConnected,
                document.getElementById('nocturne-parser-future')===null]);
            try{document.write('<p>forbidden</p>');log.push(['write','allowed']);}
            catch(e){log.push(['write',e.name]);}
        }
        attributeChangedCallback(n,o,v){log.push(['attr',n,o,v]);}
        connectedCallback(){log.push(['connected',this.childNodes.length]);}
    }
    customElements.define(name,Timing);return Timing;
}
