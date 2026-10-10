/* Run these against the real Nocturne DOM. Positive and deliberately reported
 * constructor/callback failures are separate suites; neither is a site test. */
async function runCustomElementCases() {
    let count = 0, next = 0;
    const prefix = 'nocturne-ce-' + Date.now() + '-';
    const name = () => prefix + (++next);
    const equal = (a,b) => { count++; if (!Object.is(a,b)) throw new Error(String(a)+' != '+String(b)); };
    const throws = (fn,kind) => { count++; try { fn(); } catch(e) { if(e.name===kind)return; throw e; } throw new Error('Expected '+kind); };
    const root = document.createElement('div'); document.body.appendChild(root);
    try {
        const n1 = name(); let constructed = 0;
        class Basic extends HTMLElement { constructor(){super();this.token={};constructed++;} }
        customElements.define(n1,Basic);
        equal(customElements.get(n1),Basic); equal(customElements.getName(Basic),n1);
        equal(customElements.get(name()),undefined); equal(customElements.getName(class {}),null);
        const one = new Basic(), two = document.createElement(n1);
        equal(constructed,2); equal(one instanceof Basic,true); equal(two instanceof HTMLElement,true);
        equal(one.localName,n1); equal(one.nodeType,1); equal(one.ownerDocument,document); equal(one.parentNode,null);
        one.id = name(); one.textContent='native identity'; root.appendChild(one);
        equal(document.getElementById(one.id),one); equal(root.firstChild,one); equal(one.textContent,'native identity');
        throws(()=>new HTMLElement(),'TypeError'); throws(()=>new (class extends HTMLElement {})(),'TypeError');
        throws(()=>customElements.define(n1,class extends HTMLElement {}),'NotSupportedError');
        throws(()=>customElements.define(name(),Basic),'NotSupportedError');
        throws(()=>customElements.define('missing-glyph',class extends HTMLElement {}),'SyntaxError');
        throws(()=>customElements.define('uppercase-A',class extends HTMLElement {}),'SyntaxError');
        throws(()=>customElements.define(name(),()=>{}),'TypeError');
        throws(()=>customElements.define(name(),class {connectedCallback=1;} .prototype),'TypeError');
        const bad = class extends HTMLElement {}; bad.prototype.connectedCallback = null;
        throws(()=>customElements.define(name(),bad),'TypeError');
        const builtin = name(); throws(()=>customElements.define(builtin,class extends HTMLElement {},{extends:'unknown-local-name'}),'NotSupportedError');
        equal(customElements.get(builtin),undefined);
        throws(()=>customElements.get.call({},n1),'TypeError');

        const n2=name(), before=document.createElement(n2), log=[];
        before.setAttribute('data-x','old'); before.id=name(); root.appendChild(before);
        class Upgrade extends HTMLElement {
            static observedAttributes=['data-x'];
            constructor(){super();log.push('ctor');this.marker=77;}
            connectedCallback(){log.push('connected');}
            disconnectedCallback(){log.push('disconnected');}
            attributeChangedCallback(n,o,v,ns){log.push(n+':'+o+':'+v+':'+ns);}
        }
        customElements.define(n2,Upgrade);
        equal(log.join('|'),'ctor|data-x:null:old:null|connected');
        equal(before instanceof Upgrade,true); equal(before.marker,77); equal(document.getElementById(before.id),before);
        log.length=0; before.setAttribute('DATA-X','new'); before.setAttribute('data-x','new');
        before.removeAttribute('data-x'); before.removeAttribute('data-x'); before.setAttribute('unobserved','value');
        equal(log.join('|'),'data-x:old:new:null|data-x:new:new:null|data-x:new:null:null');
        log.length=0; before.remove(); root.appendChild(before); equal(log.join('|'),'disconnected|connected');
        log.length=0; root.appendChild(before); equal(log.join('|'),'disconnected|connected');
        log.length=0; Upgrade.prototype.connectedCallback=()=>log.push('wrong-new-callback');
        before.remove(); root.appendChild(before); equal(log.join('|'),'disconnected|connected');

        const n3=name(), disconnected=document.createElement(n3); let upgrades=0;
        class Later extends HTMLElement {constructor(){super();upgrades++;}}
        customElements.define(n3,Later); equal(upgrades,0); equal(disconnected instanceof Later,false);
        customElements.upgrade(disconnected); equal(upgrades,1); equal(disconnected instanceof Later,true);
        customElements.upgrade(disconnected); equal(upgrades,1); root.appendChild(disconnected); equal(upgrades,1);

        const n4=name(), late=document.createElement(n4); let onConnect=0;
        class OnConnect extends HTMLElement {connectedCallback(){onConnect++;}}
        customElements.define(n4,OnConnect); root.appendChild(late);
        equal(late instanceof OnConnect,true); equal(onConnect,1);
        const fragment=document.createDocumentFragment(), f1=new OnConnect(), f2=new OnConnect();
        fragment.appendChild(f1); fragment.appendChild(f2); equal(onConnect,1);
        root.appendChild(fragment); equal(onConnect,3); equal(fragment.childNodes.length,0);

        const n5=name(); let parsed=0, detached=0;
        class Parsed extends HTMLElement {constructor(){super();parsed++;}disconnectedCallback(){detached++;}}
        customElements.define(n5,Parsed);
        const holder=document.createElement('div'); root.appendChild(holder);
        holder.innerHTML='<'+n5+' data-test="1"></'+n5+'>';
        equal(holder.firstChild instanceof Parsed,true); equal(parsed,1);
        const retained=holder.firstChild; holder.textContent='gone'; equal(detached,1); equal(retained.parentNode,null);
        holder.innerHTML='<'+n5+'></'+n5+'>'; equal(parsed,2);
        holder.innerHTML=''; equal(detached,2);
        const detachedHolder=document.createElement('div'); detachedHolder.innerHTML='<'+n5+'></'+n5+'>';
        equal(detachedHolder.firstChild instanceof Parsed,true); equal(parsed,3);
        const clone=detachedHolder.cloneNode(true); equal(clone.firstChild instanceof Parsed,true); equal(parsed,4);

        const n6=name(), order=[], a=document.createElement(n6), b=document.createElement(n6);
        a.id=name(); b.id=name(); root.appendChild(a); root.appendChild(b);
        class Reentrant extends HTMLElement {
            constructor(){super();order.push(this.id);if(this===a){b.remove();root.appendChild(b);}}
        }
        customElements.define(n6,Reentrant);
        equal(order.filter(x=>x===a.id).length,1); equal(order.filter(x=>x===b.id).length,1);
        equal(a instanceof Reentrant,true); equal(b instanceof Reentrant,true);

        const n7=name(), waiting=customElements.whenDefined(n7); equal(waiting,customElements.whenDefined(n7));
        class Wait extends HTMLElement {}
        customElements.define(n7,Wait); equal(await waiting,Wait); equal(await customElements.whenDefined(n7),Wait);
        count++; try {await customElements.whenDefined('invalid');throw new Error('Expected rejection');} catch(e){if(e.name!=='SyntaxError')throw e;}
        const n8=name(); let recursiveDefinition=0;
        class Defining extends HTMLElement {
            static get observedAttributes(){
                try {customElements.define(name(),class extends HTMLElement {});} catch(e){if(e.name==='NotSupportedError')recursiveDefinition++;else throw e;}
                return ['data-a'];
            }
            attributeChangedCallback(){}
        }
        customElements.define(n8,Defining); equal(recursiveDefinition,1);
        const rollback=name(); class GetterError extends HTMLElement {static get observedAttributes(){throw new Error('getter');}attributeChangedCallback(){}}
        throws(()=>customElements.define(rollback,GetterError),'Error'); equal(customElements.get(rollback),undefined);
        customElements.define(rollback,class extends HTMLElement {}); equal(typeof customElements.get(rollback),'function');
        const n9=name(), mutating=document.createElement(n9), mutationLog=[];
        mutating.setAttribute('data-a','initial');root.appendChild(mutating);
        class MutatesWhileUpgrading extends HTMLElement {
            static observedAttributes=['data-a'];
            constructor(){super();mutationLog.push('ctor-start');this.setAttribute('data-a','during-construction');mutationLog.push('ctor-end');}
            attributeChangedCallback(n,o,v){mutationLog.push(n+':'+o+':'+v);}
        }
        customElements.define(n9,MutatesWhileUpgrading);
        equal(mutationLog.join('|'),'ctor-start|ctor-end|data-a:null:initial');
        equal(mutating.getAttribute('data-a'),'during-construction');
        mutating.setAttribute('data-a','after-construction');
        equal(mutationLog[mutationLog.length-1],'data-a:during-construction:after-construction');
        // Current HTML/DOM local-name rules intentionally permit punctuation
        // that the older PCENChar production rejected.
        for(const suffix of ['!',':','@','\u03c0','\u00e9','\ud83d\ude0d','\u037e','\u{f0000}']) {
            const n=name()+suffix,C=class extends HTMLElement {};
            customElements.define(n,C);equal(customElements.get(n),C);
            equal(document.createElement(n).localName,n);
        }
        for(const n of ['plain','\u03c0-x','\u{1f600}-x',name()+'A',name()+' ',name()+'\t',name()+'\n',name()+'\r',name()+'\f',name()+'\0',name()+'/',name()+'>'])
            throws(()=>customElements.define(n,class extends HTMLElement {}),'SyntaxError');
        return count;
    } finally { root.remove(); }
}

function runCustomElementFailureCases() {
    let count=0,next=0; const prefix='nocturne-ce-failure-'+Date.now()+'-';
    const name=()=>prefix+(++next);
    const equal=(a,b)=>{count++;if(!Object.is(a,b))throw new Error(String(a)+' != '+String(b));};
    const root=document.createElement('div');document.body.appendChild(root);
    try {
        // Exactly four expected reports: failed upgrade, double-super upgrade,
        // invalid synchronous construction, and a throwing lifecycle callback.
        const n1=name(), failed=document.createElement(n1); root.appendChild(failed); let attempts=0,connections=0;
        class Throws extends HTMLElement {constructor(){super();attempts++;throw new Error('expected-ce-upgrade');}connectedCallback(){connections++;}}
        customElements.define(n1,Throws);equal(attempts,1);equal(connections,0);
        customElements.upgrade(failed); failed.remove();root.appendChild(failed);equal(attempts,1);
        const n2=name(), double=document.createElement(n2);root.appendChild(double);
        class Twice extends HTMLElement {constructor(){super();super();}}
        customElements.define(n2,Twice);customElements.upgrade(double);equal(double.parentNode,root);
        const n3=name();let badAttempts=0;
        class BadFresh extends HTMLElement {constructor(){super();badAttempts++;this.id='forbidden-during-create';}}
        customElements.define(n3,BadFresh);const fresh=document.createElement(n3);
        equal(fresh instanceof BadFresh,false);equal(fresh.hasAttribute('id'),false);equal(badAttempts,1);
        root.appendChild(fresh);customElements.upgrade(fresh);equal(badAttempts,1);
        const n4=name();let siblingRan=0;
        class CallbackThrow extends HTMLElement {connectedCallback(){throw new Error('expected-ce-callback');}}
        class CallbackOK extends HTMLElement {connectedCallback(){siblingRan++;}}
        customElements.define(n4,CallbackThrow);customElements.define(name(),CallbackOK);
        const fragment=document.createDocumentFragment();fragment.appendChild(new CallbackThrow());fragment.appendChild(new CallbackOK());
        root.appendChild(fragment);equal(siblingRan,1);
        return count;
    } finally {root.remove();}
}

function runGlobalEventTargetCases() {
    let count=0, calls=0, seen=null;
    const equal=(a,b)=>{count++;if(!Object.is(a,b))throw new Error(String(a)+' != '+String(b));};
    const {addEventListener:add,removeEventListener:remove,dispatchEvent:send}=globalThis;
    const type='nocturne-global-receiver-'+Date.now();
    function listener(e){calls++;seen=e.currentTarget;}
    add(type,listener); equal(send(new Event(type)),true); equal(calls,1); equal(seen,globalThis);
    remove(type,listener); send(new Event(type)); equal(calls,1);
    add.call(null,type,listener); send.call(undefined,new Event(type)); equal(calls,2);
    remove.call(null,type,listener);
    const target=new EventTarget(); add.call(target,type,listener);
    send.call(target,new Event(type)); equal(calls,3); equal(seen,target);
    remove.call(target,type,listener); send.call(target,new Event(type)); equal(calls,3);
    send(new Event(type)); equal(calls,3);
    return count;
}

function runHTMLIFrameElementCases() {
    let count=0;
    const equal=(a,b)=>{count++;if(!Object.is(a,b))throw new Error(String(a)+' != '+String(b));};
    const illegal=fn=>{count++;try{fn();}catch(e){if(e instanceof TypeError)return;throw e;}throw new Error('Expected TypeError');};
    const frame=document.createElement('iframe'), ordinary=document.createElement('div');
    equal(HTMLIFrameElement===HTMLElement,false);
    equal(Object.getPrototypeOf(HTMLIFrameElement.prototype),HTMLElement.prototype);
    equal(Object.getPrototypeOf(frame),HTMLIFrameElement.prototype);
    equal(frame instanceof HTMLIFrameElement,true);equal(frame instanceof HTMLElement,true);equal(frame instanceof Node,true);
    equal(ordinary instanceof HTMLIFrameElement,false);
    equal(document.createElementNS('http://www.w3.org/2000/svg','iframe') instanceof HTMLIFrameElement,false);
    illegal(()=>new HTMLIFrameElement());illegal(()=>new(class extends HTMLIFrameElement {})());
    class Derived extends HTMLIFrameElement {}
    equal(frame instanceof Derived,false);
    frame.setAttribute('data-brand','native');equal(Element.prototype.getAttribute.call(frame,'data-brand'),'native');
    const holder=document.createElement('div');holder.innerHTML='<iframe id="nocturne-native-frame"></iframe>';
    const parsed=holder.firstChild;equal(parsed instanceof HTMLIFrameElement,true);equal(parsed.parentNode,holder);
    equal(holder.querySelector('iframe'),parsed);
    const clone=parsed.cloneNode(true);equal(clone instanceof HTMLIFrameElement,true);equal(clone!==parsed,true);
    equal(clone.id,parsed.id);equal(clone.parentNode,null);
    return count;
}
