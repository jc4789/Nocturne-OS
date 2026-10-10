/* Native-tree HTML sanitization and native-slot Trusted Types. Configuration
 * follows WHATWG HTML; Trusted Types follows the current W3C editor draft.
 * The host-private scope never exposes rawDom or native brand creation. */
const htmlSafetyBridge=(()=>{
    const windowRealm=typeof Document!=='undefined';
    const HTML="http://www.w3.org/1999/xhtml",SVG="http://www.w3.org/2000/svg",MATH="http://www.w3.org/1998/Math/MathML",XLINK="http://www.w3.org/1999/xlink";
    const define=Object.defineProperty,ownKeys=Object.keys,freeze=Object.freeze,create=Object.create;
    const stringify=JSON.stringify,StringImpl=String,arrayFrom=Array.from,arraySplice=Array.prototype.splice,arrayFilter=Array.prototype.filter;
    const weakGet=WeakMap.prototype.get,weakSet=WeakMap.prototype.set,configs=new WeakMap(),policies=new WeakMap();
    const nativeTrusted=host.trusted,nativeSafety=host.safety,get=(n,k)=>rawDom.get(n,k);
    const wGet=(m,k)=>apply(weakGet,m,[k]),wSet=(m,k,v)=>apply(weakSet,m,[k,v]),filter=(a,fn)=>apply(arrayFilter,a,[fn]);
    const names=[null,"TrustedHTML","TrustedScript","TrustedScriptURL","TrustedHTMLParserOptions"];
    const methods=[null,"createHTML","createScript","createScriptURL","createParserOptions"];
    const constructors=[];let defaultPolicy=null,defaultBusy=false;const createdNames=[];
    function string(v){if(typeof v==='symbol')throw new TypeError('Cannot convert Symbol to DOMString');return StringImpl(v);}
    function dictionary(v){if(v==null)return {};if(typeof v!=='object'&&typeof v!=='function')throw new TypeError('Dictionary required');return v;}
    function sequence(v){if(v===null||(typeof v!=='object'&&typeof v!=='function')||typeof v[Symbol.iterator]!=='function')throw new TypeError('Iterable sequence required');return arrayFrom(v);}
    function copy(v){if(v===null||typeof v!=='object')return v;const o=Array.isArray(v)?[]:{};const keys=ownKeys(v);for(let i=0;i<keys.length;i++)o[keys[i]]=copy(v[keys[i]]);return o;}
    function deepFreeze(v){if(v&&typeof v==='object'){const keys=ownKeys(v);for(let i=0;i<keys.length;i++)deepFreeze(v[keys[i]]);freeze(v);}return v;}
    function same(a,b){return a.name===b.name&&a.namespace===b.namespace;}
    function index(list,item){if(list)for(let i=0;i<list.length;i++)if(same(list[i],item))return i;return -1;}
    function has(list,item){return index(list,item)>=0;}
    function remove(list,item){const i=index(list,item);if(i<0)return false;apply(arraySplice,list,[i,1]);return true;}
    function contains(list,value){for(let i=0;i<list.length;i++)if(list[i]===value)return true;return false;}
    function removeString(list,value){for(let i=0;i<list.length;i++)if(list[i]===value){apply(arraySplice,list,[i,1]);return true;}return false;}
    function canonical(item,attribute=false,local=false){
        const object=typeof item==='object'&&item!==null;
        const name=object?item.name:item,namespace=object?item.namespace:undefined;
        if(object&&name===undefined)throw new TypeError('Sanitizer name required');
        const result={name:string(name),namespace:namespace===undefined?attribute?null:HTML:namespace===null?null:string(namespace)};
        if(local){
            const attributes=object?item.attributes:undefined,removeAttributes=object?item.removeAttributes:undefined;
            if(attributes!==undefined)result.attributes=list(attributes,true);
            if(removeAttributes!==undefined)result.removeAttributes=list(removeAttributes,true);
            if(result.attributes===undefined&&result.removeAttributes===undefined)result.removeAttributes=[];
        }
        return result;
    }
    function pi(item){if(typeof item==='object'&&item!==null){const target=item.target;if(target===undefined)throw new TypeError('Processing instruction target required');return string(target);}return string(item);}
    function list(value,attribute=false,local=false,instruction=false){const values=sequence(value),result=[];for(let i=0;i<values.length;i++)result[i]=instruction?pi(values[i]):canonical(values[i],attribute,local);return result;}
    function data(item){return item.namespace===null&&item.name.slice(0,5)==='data-';}
    function duplicate(items,instruction=false){for(let i=0;i<items.length;i++)for(let j=0;j<i;j++)if(instruction?items[i]===items[j]:same(items[i],items[j]))return true;return false;}
    function overlap(a,b){if(!a||!b)return false;for(let i=0;i<a.length;i++)if(has(b,a[i]))return true;return false;}
    function nonreplaceable(e){return e.namespace===HTML&&e.name==='html'||e.namespace===SVG&&e.name==='svg'||e.namespace===MATH&&e.name==='math';}
    function valid(c){
        for(const pair of [['elements','removeElements'],['attributes','removeAttributes'],['processingInstructions','removeProcessingInstructions']])if(c[pair[0]]!==undefined&&c[pair[1]]!==undefined)return false;
        for(const key of ['elements','removeElements','replaceWithChildrenElements','attributes','removeAttributes','processingInstructions','removeProcessingInstructions'])if(c[key]!==undefined&&duplicate(c[key],key.includes('Processing')||key==='processingInstructions'))return false;
        if(c.attributes===undefined&&c.dataAttributes!==undefined)return false;
        if(overlap(c.replaceWithChildrenElements,c.elements||c.removeElements))return false;
        if(c.replaceWithChildrenElements)for(let i=0;i<c.replaceWithChildrenElements.length;i++)if(nonreplaceable(c.replaceWithChildrenElements[i]))return false;
        if(c.attributes&&c.dataAttributes)for(let i=0;i<c.attributes.length;i++)if(data(c.attributes[i]))return false;
        if(c.elements)for(let i=0;i<c.elements.length;i++){
            const e=c.elements[i],a=e.attributes,r=e.removeAttributes;
            if(a&&duplicate(a)||r&&duplicate(r))return false;
            if(c.attributes){
                if(overlap(c.attributes,a))return false;
                if(r)for(let j=0;j<r.length;j++)if(!has(c.attributes,r[j]))return false;
                if(c.dataAttributes&&a)for(let j=0;j<a.length;j++)if(data(a[j]))return false;
            }else if(a&&r)return false;
        }
        return true;
    }
    function defaultConfiguration(){return {elements:copy(sanitizerConstants.elements),attributes:copy(sanitizerConstants.attributes),processingInstructions:[],comments:false,dataAttributes:false,javascriptURLs:false};}
    function configure(input,permissive=true){
        if(typeof input==='string'){if(input!=='default')throw new TypeError('Invalid Sanitizer preset');input=defaultConfiguration();}
        input=dictionary(input);const c={};
        for(const key of ['attributes','comments','dataAttributes','elements','javascriptURLs','processingInstructions','removeAttributes','removeElements','removeProcessingInstructions','replaceWithChildrenElements']){const value=input[key];if(value!==undefined)c[key]=key==='comments'||key==='dataAttributes'||key==='javascriptURLs'?!!value:list(value,key==='attributes'||key==='removeAttributes',key==='elements',key==='processingInstructions'||key==='removeProcessingInstructions');}
        if(c.elements===undefined&&c.removeElements===undefined)c.removeElements=[];
        if(c.attributes===undefined&&c.removeAttributes===undefined)c.removeAttributes=[];
        if(c.processingInstructions===undefined&&c.removeProcessingInstructions===undefined)c[permissive?'removeProcessingInstructions':'processingInstructions']=[];
        if(c.comments===undefined)c.comments=permissive;
        if(c.javascriptURLs===undefined)c.javascriptURLs=permissive;
        if(c.dataAttributes===undefined&&c.attributes)c.dataAttributes=permissive;
        if(!valid(c))throw new TypeError('Contradictory or duplicate Sanitizer configuration');return c;
    }
    function state(s){const c=trustedKind(s)===5?payload(s,5):wGet(configs,s);if(!c)throw new TypeError('Illegal Sanitizer receiver');return c;}
    function removeElement(c,e){let changed=remove(c.replaceWithChildrenElements,e);if(c.elements)return remove(c.elements,e)||changed;if(has(c.removeElements,e))return changed;c.removeElements[c.removeElements.length]=e;return true;}
    function removeAttribute(c,a){
        let changed=false;
        if(c.attributes){changed=remove(c.attributes,a);if(c.elements)for(let i=0;i<c.elements.length;i++){changed=remove(c.elements[i].attributes,a)||changed;changed=remove(c.elements[i].removeAttributes,a)||changed;}return changed;}
        if(has(c.removeAttributes,a))return false;
        if(c.elements)for(let i=0;i<c.elements.length;i++){remove(c.elements[i].attributes,a);remove(c.elements[i].removeAttributes,a);}
        c.removeAttributes[c.removeAttributes.length]=a;return true;
    }
    function removeUnsafe(c){let changed=false;for(let i=0;i<sanitizerConstants.unsafeElements.length;i++)changed=removeElement(c,canonical(sanitizerConstants.unsafeElements[i]))||changed;for(let i=0;i<sanitizerConstants.eventAttributes.length;i++)changed=removeAttribute(c,canonical(sanitizerConstants.eventAttributes[i],true))||changed;if(c.javascriptURLs){c.javascriptURLs=false;changed=true;}return changed;}
    class Sanitizer {
        constructor(configuration='default'){return nativeTrusted('create',5,configure(configuration),new.target.prototype);}
        get(){return copy(state(this));}
        allowElement(element){
            const c=state(this),e=canonical(element,false,true);
            if(!c.elements){if(e.attributes||e.removeAttributes.length)return false;const changed=remove(c.replaceWithChildrenElements,e);return remove(c.removeElements,e)||changed;}
            const changed=remove(c.replaceWithChildrenElements,e);
            if(c.attributes){
                if(e.attributes)e.attributes=filter(e.attributes,a=>!has(c.attributes,a)&&!(c.dataAttributes&&data(a)));
                if(e.removeAttributes)e.removeAttributes=filter(e.removeAttributes,a=>has(c.attributes,a));
                if(e.attributes)e.attributes=filter(e.attributes,a=>!has(e.removeAttributes,a));
            }else if(e.attributes){e.attributes=filter(e.attributes,a=>!has(e.removeAttributes,a)&&!has(c.removeAttributes,a));delete e.removeAttributes;}
            else if(e.removeAttributes)e.removeAttributes=filter(e.removeAttributes,a=>!has(c.removeAttributes,a));
            const i=index(c.elements,e);if(i>=0&&stringify(c.elements[i])===stringify(e))return changed;if(i>=0)apply(arraySplice,c.elements,[i,1]);c.elements[c.elements.length]=e;return true;
        }
        removeElement(e){return removeElement(state(this),canonical(e));}
        replaceElementWithChildren(e){const c=state(this);e=canonical(e);if(nonreplaceable(e))return false;const changed=remove(c.elements||c.removeElements,e);if(has(c.replaceWithChildrenElements,e))return changed;if(!c.replaceWithChildrenElements)c.replaceWithChildrenElements=[];c.replaceWithChildrenElements[c.replaceWithChildrenElements.length]=e;return true;}
        allowAttribute(a){const c=state(this);a=canonical(a,true);if(!c.attributes)return remove(c.removeAttributes,a);if(has(c.attributes,a)||c.dataAttributes&&data(a))return false;if(c.elements)for(let i=0;i<c.elements.length;i++)remove(c.elements[i].attributes,a);c.attributes[c.attributes.length]=a;return true;}
        removeAttribute(a){return removeAttribute(state(this),canonical(a,true));}
        allowProcessingInstruction(p){const c=state(this);p=pi(p);if(c.processingInstructions){if(contains(c.processingInstructions,p))return false;c.processingInstructions[c.processingInstructions.length]=p;return true;}return removeString(c.removeProcessingInstructions,p);}
        removeProcessingInstruction(p){const c=state(this);p=pi(p);if(c.processingInstructions)return removeString(c.processingInstructions,p);if(contains(c.removeProcessingInstructions,p))return false;c.removeProcessingInstructions[c.removeProcessingInstructions.length]=p;return true;}
        setComments(v){const c=state(this);v=!!v;if(c.comments===v)return false;c.comments=v;return true;}
        setJavascriptURLs(v){const c=state(this);v=!!v;if(c.javascriptURLs===v)return false;c.javascriptURLs=v;return true;}
        setDataAttributes(v){const c=state(this);v=!!v;if(!c.attributes||c.dataAttributes===v)return false;if(v){c.attributes=filter(c.attributes,a=>!data(a));if(c.elements)for(let i=0;i<c.elements.length;i++)if(c.elements[i].attributes)c.elements[i].attributes=filter(c.elements[i].attributes,a=>!data(a));}c.dataAttributes=v;return true;}
        removeUnsafe(){return removeUnsafe(state(this));}
    }
    function sanitizer(input,safe){if(input===undefined)return safe?configure('default',false):null;const c=trustedKind(input)===5?payload(input,5):wGet(configs,input);return c?copy(c):configure(input,!safe);}
    function javascriptURL(value){return /^[\x00-\x20]*javascript:/i.test(value.replace(/[\t\n\r]/g,''));}
    function sanitize(root,c,safe){
        if(!c)return;if(safe)removeUnsafe(c);
        function visit(parent){for(let child=get(parent,'firstChild');child;){const next=get(child,'nextSibling'),type=get(child,'nodeType');
            if(type===8){if(!c.comments)rawDom.remove(child);}
            else if(type===7){const t=get(child,'piTarget');if(c.processingInstructions?!contains(c.processingInstructions,t):contains(c.removeProcessingInstructions,t))rawDom.remove(child);}
            else if(type===1){
                const e={name:get(child,'localName'),namespace:get(child,'namespaceURI')};
                if(has(c.replaceWithChildrenElements,e)){visit(child);while(get(child,'firstChild'))rawDom.insert(parent,get(child,'firstChild'),child);rawDom.remove(child);}
                else if(c.elements?!has(c.elements,e):has(c.removeElements,e))rawDom.remove(child);
                else{
                    const local=c.elements?c.elements[index(c.elements,e)]:{},template=e.namespace===HTML&&e.name==='template'?get(child,'templateContent'):null,shadow=get(child,'shadowRoot');
                    if(template)visit(template);if(shadow)visit(shadow);
                    const attrs=rawDom.attrList(child);
                    for(let i=0;i<attrs.length;i++){
                        const a=attrs[i],name={name:get(a,'localName'),namespace:get(a,'namespaceURI')},value=get(a,'attrValue');let drop=has(local.removeAttributes,name);
                        if(!drop&&c.attributes)drop=!has(c.attributes,name)&&!has(local.attributes,name)&&!(c.dataAttributes&&data(name));
                        else if(!drop&&!c.attributes)drop=local.attributes?!has(local.attributes,name):has(c.removeAttributes,name);
                        if(!c.javascriptURLs){
                            const nav=e.namespace===HTML&&name.namespace===null&&(e.name==='a'||e.name==='area')&&name.name==='href'||e.namespace===HTML&&name.namespace===null&&(name.name==='action'||name.name==='formaction')||e.namespace===SVG&&e.name==='a'&&(name.namespace===null||name.namespace===XLINK)&&name.name==='href'||e.namespace===MATH&&(name.namespace===null||name.namespace===XLINK)&&name.name==='href';
                            if(nav&&javascriptURL(value))drop=true;
                            if(e.namespace===SVG&&(e.name==='animate'||e.name==='animateTransform'||e.name==='set')&&name.namespace===null&&name.name==='attributeName'&&(value==='href'||value==='xlink:href'))drop=true;
                        }
                        if(drop)rawDom.attrRemoveNode(child,a);
                    }
                    visit(child);
                }
            }
            child=next;
        }}visit(root);
    }
    function trustedKind(v){return nativeTrusted('kind',v);}
    function payload(v,k){return nativeTrusted('payload',v,k);}
    function make(k,v){return nativeTrusted('create',k,v,constructors[k].prototype);}
    function ttString(v){const k=trustedKind(v);return k>0&&k<4?payload(v,k):string(v);}
    class TrustedHTML {constructor(){throw new TypeError('Illegal TrustedHTML constructor');}toString(){return payload(this,1);}toJSON(){return payload(this,1);}}
    class TrustedScript {constructor(){throw new TypeError('Illegal TrustedScript constructor');}toString(){return payload(this,2);}toJSON(){return payload(this,2);}}
    class TrustedScriptURL {constructor(){throw new TypeError('Illegal TrustedScriptURL constructor');}toString(){return payload(this,3);}toJSON(){return payload(this,3);}}
    class TrustedHTMLParserOptions {constructor(){throw new TypeError('Illegal TrustedHTMLParserOptions constructor');}}
    constructors[1]=TrustedHTML;constructors[2]=TrustedScript;constructors[3]=TrustedScriptURL;constructors[4]=TrustedHTMLParserOptions;
    function policyState(v){const p=wGet(policies,v);if(!p)throw new TypeError('Illegal TrustedTypePolicy receiver');return p;}
    function parserDictionary(input){if(trustedKind(input)===4)return copy(payload(input,4));const d=dictionary(input),o={runScripts:!!d.runScripts},value=d.sanitizer;if(value!==undefined)o.sanitizer=value;return o;}
    function sanitizerDictionary(input){const d=dictionary(input),o={},value=d.sanitizer;if(value!==undefined)o.sanitizer=value;return o;}
    function parserPayload(input){const d=parserDictionary(input);return deepFreeze({runScripts:d.runScripts,sanitizer:d.sanitizer===undefined||d.sanitizer===null?null:sanitizer(d.sanitizer,false)});}
    function parserCallbackArgument(input){const d=parserDictionary(input);if(d.sanitizer!==undefined&&d.sanitizer!==null){const instance=create(Sanitizer.prototype);wSet(configs,instance,sanitizer(d.sanitizer,false));d.sanitizer=instance;}return d;}
    function policyValue(policy,k,value,args,missing){const p=policyState(policy),fn=p.callbacks[methods[k]];if(!fn){if(missing)throw new TypeError('Trusted Type policy callback is absent');return null;}const input=k===4?parserCallbackArgument(value):ttString(value);return apply(fn,undefined,[input,...args]);}
    function policyCreate(policy,k,value,args){const result=policyValue(policy,k,value,args,true);if(k===4){if(result==null)throw new TypeError('Parser options policy rejected value');return make(4,parserPayload(result));}return make(k,result==null?'':string(result));}
    // Reentrant automatic policy execution cannot recursively approve a sink.
    // Explicit calls to a named policy remain ordinary author callbacks. Keep
    // result/dictionary conversion within the guard and always restore it.
    function defaultValue(k,value,args){
        if(!defaultPolicy||defaultBusy)return null;defaultBusy=true;
        try{const result=policyValue(defaultPolicy,k,value,args,false);return result==null?null:k===4?make(4,parserPayload(result)):string(result);}
        finally{defaultBusy=false;}
    }
    class TrustedTypePolicy {
        constructor(){throw new TypeError('Illegal TrustedTypePolicy constructor');}
        get name(){return policyState(this).name;}
        createHTML(input,...args){if(!arguments.length)throw new TypeError('Input required');return policyCreate(this,1,input,args);}
        createScript(input,...args){if(!arguments.length)throw new TypeError('Input required');return policyCreate(this,2,input,args);}
        createScriptURL(input,...args){if(!arguments.length)throw new TypeError('Input required');const value=policyCreate(this,3,input,args);return make(3,payload(value,3).toWellFormed());}
        createParserOptions(options={},...args){return policyCreate(this,4,options,args);}
    }
    function security(node){return nativeSafety('state',node||null);}
    function violation(node,directive,sink,source,enforce,policyIndex=-1){
        let sample=string(source);if(sink==='Function')sample=sample.replace(/^\(?(?:async )?function\*? anonymous/,'');
        sample=sample.slice(0,40);if(directive==='require-trusted-types-for')sample=sink+'|'+sample;
        nativeSafety('violation',node||null,directive,sink,sample,enforce?'enforce':'report',policyIndex);
    }
    function requirementViolation(node,sink,sample,c){const rules=c.requirements||[];if(!rules.length){violation(node,'require-trusted-types-for',sink,sample,c.enforce);return;}for(let i=0;i<rules.length;i++)violation(node,'require-trusted-types-for',sink,sample,!rules[i].reportOnly,rules[i].policyIndex);}
    const violationSlots=new WeakMap();
    const violationStrings=['documentURI','referrer','blockedURI','violatedDirective','effectiveDirective','originalPolicy','sourceFile','sample','disposition'];
    const violationNumbers=['statusCode','lineNumber','columnNumber'];
    class SecurityPolicyViolationEvent extends Event {
        constructor(type,init={}){
            if(!arguments.length)throw new TypeError('Event type required');init=dictionary(init);super(string(type),init);
            const data={};for(let i=0;i<violationStrings.length;i++){const key=violationStrings[i];data[key]=init[key]===undefined?(key==='disposition'?'enforce':''):string(init[key]);}
            for(const key of ['documentURI','referrer','blockedURI','sourceFile'])data[key]=data[key].toWellFormed();
            if(data.disposition!=='enforce'&&data.disposition!=='report')throw new TypeError('Invalid policy violation disposition');
            for(let i=0;i<violationNumbers.length;i++){const key=violationNumbers[i],number=+init[key]>>>0;data[key]=key==='statusCode'?number&65535:number;}
            data.trusted=false;wSet(violationSlots,this,data);
            // The existing EventTarget resets trust on author redispatch.
            // Its false assignment can downgrade this private slot, while an
            // author cannot promote an event by assigning true.
            define(this,'isTrusted',{enumerable:true,get(){return wGet(violationSlots,this).trusted;},set(value){if(value===false)wGet(violationSlots,this).trusted=false;}});
        }
    }
    for(const key of [...violationStrings,...violationNumbers])define(SecurityPolicyViolationEvent.prototype,key,{configurable:true,enumerable:true,get(){const data=wGet(violationSlots,this);if(!data)throw new TypeError('Illegal SecurityPolicyViolationEvent receiver');return data[key];}});
    function violationEvent(init){const event=new SecurityPolicyViolationEvent('securitypolicyviolation',{...init,bubbles:true,composed:true});wGet(violationSlots,event).trusted=true;return event;}
    function ownerRoute(node,hook,...args){return node?nativeSafety('owner',node,hook,...args):null;}
    const factoryBrands=new WeakMap();
    function factory(v){if(!wGet(factoryBrands,v))throw new TypeError('Illegal TrustedTypePolicyFactory receiver');}
    function attributeType(tag,attribute,ns=HTML,attrNS=null,exact=false){tag=string(tag).toLowerCase();attribute=string(attribute);ns=ns==null||ns===''?HTML:string(ns);attrNS=attrNS==null||attrNS===''?null:string(attrNS);if(!exact)attribute=attribute.toLowerCase();
        if(attrNS===null&&(ns===HTML||ns===SVG||ns===MATH)&&contains(sanitizerConstants.eventAttributes,attribute))return 2;
        if(ns===HTML&&tag==='iframe'&&attribute==='srcdoc'&&attrNS===null)return 1;
        if(ns===HTML&&tag==='script'&&attribute==='src'&&attrNS===null||ns===SVG&&tag==='script'&&attribute==='href'&&(attrNS===null||attrNS===XLINK))return 3;return 0;
    }
    function attributeSink(node,name,ns=null){
        const elementNS=get(node,'namespaceURI'),tag=get(node,'localName');name=string(name);if(elementNS===HTML)name=name.replace(/[A-Z]/g,c=>c.toLowerCase());
        const kind=attributeType(tag,name,elementNS,ns,true);
        return {kind,sink:kind===2?'Element '+name:elementNS===SVG?'SVGScriptElement href':tag==='iframe'?'HTMLIFrameElement srcdoc':'HTMLScriptElement src'};
    }
    function propertyType(tag,property,ns=HTML){tag=string(tag).toLowerCase();property=string(property);ns=ns==null||ns===''?HTML:string(ns);if(property==='innerHTML'||property==='outerHTML')return 1;if(ns===HTML&&tag==='iframe'&&property==='srcdoc')return 1;if(ns===HTML&&tag==='script'){if(property==='src')return 3;if(property==='text'||property==='textContent'||property==='innerText')return 2;}return 0;}
    class TrustedTypePolicyFactory {
        constructor(){throw new TypeError('Illegal TrustedTypePolicyFactory constructor');}
        createPolicy(name,options={}){
            factory(this);if(!arguments.length)throw new TypeError('Policy name required');name=string(name);options=dictionary(options);const callbacks={},c=security(null);
            for(const k of [1,4,2,3]){const fn=options[methods[k]];if(fn!==undefined&&typeof fn!=='function')throw new TypeError('Policy callback must be callable');callbacks[methods[k]]=fn||null;}
            const restrictions=c.policies||[];let blocked=false;
            for(let i=0;i<restrictions.length;i++){const r=restrictions[i],allowed=contains(r.names||[],name)||contains(r.names||[],'*'),duplicate=!r.allowDuplicates&&contains(createdNames,name);if(!allowed||duplicate){violation(null,'trusted-types','TrustedTypePolicyFactory createPolicy',name,!r.reportOnly,r.policyIndex);if(!r.reportOnly)blocked=true;}}
            if(blocked||name==='default'&&defaultPolicy!==null)throw new TypeError('Trusted Type policy creation denied');
            const value=create(TrustedTypePolicy.prototype);wSet(policies,value,{name,callbacks});createdNames[createdNames.length]=name;if(name==='default')defaultPolicy=value;return value;
        }
        isHTML(v){factory(this);return trustedKind(v)===1;}
        isScript(v){factory(this);return trustedKind(v)===2;}
        isScriptURL(v){factory(this);return trustedKind(v)===3;}
        getAttributeType(...args){factory(this);if(args.length<2)throw new TypeError('Tag and attribute required');return names[attributeType(args[0],args[1],args[2],args[3])]||null;}
        getPropertyType(...args){factory(this);if(args.length<2)throw new TypeError('Tag and property required');return names[propertyType(...args)]||null;}
        get emptyHTML(){factory(this);return emptyHTML;}
        get emptyScript(){factory(this);return emptyScript;}
        get defaultPolicy(){factory(this);return defaultPolicy;}
    }
    const trustedTypes=create(TrustedTypePolicyFactory.prototype);wSet(factoryBrands,trustedTypes,true);
    const emptyHTML=make(1,''),emptyScript=make(2,'');
    function check(value,k,sink,node=null){
        if(typeof k==='string'){for(let i=1;i<names.length;i++)if(names[i]===k){k=i;break;}}
        if(trustedKind(value)===k)return payload(value,k);
        const input=ttString(value),routed=ownerRoute(node,'check',input,k,sink);if(routed)return routed.value;
        const c=security(node);if(!c.required)return input;
        const result=defaultValue(k,input,[names[k],sink]);
        if(result!=null)return result;
        requirementViolation(node,sink,input,c);
        if(c.enforce)throw new TypeError(sink+' requires '+names[k]);return input;
    }
    function options(value,sink,node=null,fromAuthor=true){
        if(trustedKind(value)===4)return value;
        const d=parserDictionary(value),routed=ownerRoute(node,'options',d,sink,fromAuthor);if(routed)return routed.value;
        const c=security(node);if(!c.required)return d;
        const callback=defaultPolicy&&policyState(defaultPolicy).callbacks.createParserOptions;
        if(callback){const result=defaultValue(4,d,['TrustedHTMLParserOptions',sink]);if(result!=null)return result;requirementViolation(node,sink,'',c);if(c.enforce)throw new TypeError('Parser options rejected');return d;}
        const vetting=fromAuthor&&(d.runScripts||d.sanitizer!==undefined&&(typeof d.sanitizer!=='object'||d.sanitizer===null||ownKeys(d.sanitizer).length!==0||trustedKind(d.sanitizer)===5||wGet(configs,d.sanitizer)));
        if(vetting){requirementViolation(node,sink,'',c);if(c.enforce)throw new TypeError('Untrusted parser options');}return d;
    }
    function compliantInput(html,input,sink,node,fromAuthor=true){const routed=ownerRoute(node,'input',html,input,sink,fromAuthor);if(routed)return routed.value;const o=options(input,sink,node,fromAuthor);return {html:trustedKind(o)===4?ttString(html):check(html,1,sink,node),options:trustedKind(o)===4?copy(payload(o,4)):o};}
    function parseConfig(o,safe){return sanitizer(o.sanitizer===null?undefined:o.sanitizer,safe);}
    function target(node){const type=get(node,'nodeType');if(type!==1&&!(type===11&&get(node,'shadowHost')))throw new TypeError('Element or ShadowRoot receiver required');return type===1&&get(node,'namespaceURI')===HTML&&get(node,'localName')==='template'?get(node,'templateContent'):node;}
    function setHTML(node,html,input,safe,allowShadow=true,legacy=false,originalSetter=null){
        const into=target(node),sink=(get(node,'nodeType')===11?'ShadowRoot':'Element')+(legacy?' innerHTML':safe?' setHTML':' setHTMLUnsafe');
        const values=safe?{html:string(html),options:sanitizerDictionary(input)}:compliantInput(html,input,sink,node,!legacy),o=values.options,c=parseConfig(o,safe);
        // Keep the established atomic native replacement, control-state and
        // mutation-observer path for ordinary legacy insertion.
        if(legacy&&!c&&!o.runScripts&&originalSetter)return apply(originalSetter,node,[values.html]);
        const priorShadow=get(node,'shadowRoot');
        const fragment=rawDom.parseFragment(node,values.html,false,{allowShadow,runScripts:!safe&&!!o.runScripts,sanitizing:!!c});sanitize(fragment,c,safe);
        const addedShadow=get(node,'shadowRoot');
        if(addedShadow&&addedShadow!==priorShadow)sanitize(addedShadow,c,safe);
        customElementsBridge.reactions(()=>{while(get(into,'firstChild'))dom('remove',get(into,'firstChild'));dom('insert',into,fragment,null);});
        if(!safe&&o.runScripts)nativeSafety('scripts',into);
    }
    if(windowRealm){
    for(const proto of [Element.prototype,ShadowRoot.prototype]){
        define(proto,'setHTML',{configurable:true,writable:true,enumerable:true,value:function(html,options={}){if(!arguments.length)throw new TypeError('HTML required');setHTML(this,html,options,true);}});
        define(proto,'setHTMLUnsafe',{configurable:true,writable:true,enumerable:true,value:function(html,options={}){if(!arguments.length)throw new TypeError('HTML required');setHTML(this,html,options,false);}});
        const descriptor=Object.getOwnPropertyDescriptor(proto,'innerHTML');
        define(proto,'innerHTML',{...descriptor,set(value){setHTML(this,value===null?'':value,{},false,false,true,descriptor.set);}});
        define(proto,'getHTML',{configurable:true,writable:true,enumerable:true,value:function(options={}){target(this);const o=dictionary(options),serializable=!!o.serializableShadowRoots,supplied=o.shadowRoots,roots=supplied===undefined?[]:sequence(supplied);for(let i=0;i<roots.length;i++)if(!get(roots[i],'shadowHost'))throw new TypeError('ShadowRoot list required');return nativeSafety('serialize',this,serializable,roots);}});
    }
    for(const [name,safe] of [['parseHTML',true],['parseHTMLUnsafe',false]])define(Document,name,{configurable:true,writable:true,enumerable:true,value:function(html,input={}){
        if(!arguments.length)throw new TypeError('HTML required');const supplied=trustedKind(input)===4?input:sanitizerDictionary(input),v=safe?{html:string(html),options:supplied}:compliantInput(html,supplied,'Document '+name,null),c=parseConfig(v.options,safe);
        const made=rawDom.parseDocument(null,v.html,false,{allowShadow:true,runScripts:false,sanitizing:!!c});sanitize(made,c,safe);return made;
    }});
    // Intercept original union values before existing DOMString conversions.
    function approvedTextSetter(node,value,setter){
        const previous=nativeSafety('scriptText',node,value);
        try{return apply(setter,node,[value]);}catch(error){nativeSafety('scriptText',node,previous);throw error;}
    }
    function wrapSetter(proto,key,kind,sink,condition){const d=Object.getOwnPropertyDescriptor(proto,key);if(!d||!d.set||!d.configurable)return;define(proto,key,{...d,set(v){if(!condition||condition(this)){v=check(v==null?'':v,kind,sink,this);if(kind===2)return approvedTextSetter(this,v,d.set);}return apply(d.set,this,[v]);}});}
    const script=n=>get(n,'nodeType')===1&&get(n,'namespaceURI')===HTML&&get(n,'localName')==='script';
    wrapSetter(HTMLScriptElement.prototype,'src',3,'HTMLScriptElement src');wrapSetter(HTMLScriptElement.prototype,'text',2,'HTMLScriptElement text');
    wrapSetter(HTMLIFrameElement.prototype,'srcdoc',1,'HTMLIFrameElement srcdoc');
    wrapSetter(HTMLElement.prototype,'innerText',2,'HTMLScriptElement innerText',script);
    for(const key of ['textContent','nodeValue']){
        const d=Object.getOwnPropertyDescriptor(Node.prototype,key);
        define(Node.prototype,key,{...d,set(value){
            const type=get(this,'nodeType');
            if(type===2){const owner=get(this,'attrOwner');if(owner){const data=attributeSink(owner,get(this,'localName'),get(this,'namespaceURI'));if(data.kind)value=check(value==null?'':value,data.kind,data.sink,owner);}}
            else if(key==='textContent'&&script(this)){value=check(value==null?'':value,2,'HTMLScriptElement textContent',this);return approvedTextSetter(this,value,d.set);}
            return apply(d.set,this,[value]);
        }});
    }
    const outer=Object.getOwnPropertyDescriptor(Element.prototype,'outerHTML');
    define(Element.prototype,'outerHTML',{...outer,set(value){
        get(this,'elementBrand');const v=compliantInput(value===null?'':value,{},'Element outerHTML',this,false),parent=get(this,'parentNode');if(!parent)return;
        if(!v.options.sanitizer&&!v.options.runScripts)return apply(outer.set,this,[v.html]);
        if(get(parent,'nodeType')===9)throw new DOMException('Document element cannot be replaced','NoModificationAllowedError');
        const owner=get(this,'ownerDocument'),context=get(parent,'nodeType')===11?rawDom.create(owner,1,'body',''):parent,fragment=rawDom.parseFragment(context,v.html,false,{allowShadow:false,runScripts:!!v.options.runScripts,sanitizing:!!v.options.sanitizer});
        sanitize(fragment,parseConfig(v.options,false),false);const inserted=[];for(let n=get(fragment,'firstChild');n;n=get(n,'nextSibling'))inserted[inserted.length]=n;
        customElementsBridge.reactions(()=>{dom('insert',parent,fragment,this);dom('remove',this);});if(v.options.runScripts)for(let i=0;i<inserted.length;i++)nativeSafety('scripts',inserted[i]);
    }});
    const adjacentOriginal=Element.prototype.insertAdjacentHTML;
    define(Element.prototype,'insertAdjacentHTML',{configurable:true,writable:true,enumerable:true,value:function(position,html){
        get(this,'elementBrand');if(arguments.length<2)throw new TypeError('Position and HTML required');position=string(position).toLowerCase();const v=compliantInput(html,{},'Element insertAdjacentHTML',this,false);
        if(!v.options.sanitizer&&!v.options.runScripts)return apply(adjacentOriginal,this,[position,v.html]);
        let parent,before;if(position==='beforebegin'||position==='afterend'){parent=get(this,'parentNode');if(!parent||get(parent,'nodeType')===9)throw new DOMException('No insertion parent','NoModificationAllowedError');before=position==='beforebegin'?this:get(this,'nextSibling');}
        else if(position==='afterbegin'||position==='beforeend'){parent=this;before=position==='afterbegin'?get(this,'firstChild'):null;}else throw new DOMException('Invalid position','SyntaxError');
        let context=parent;if(get(parent,'nodeType')!==1||get(parent,'namespaceURI')===HTML&&get(parent,'localName')==='html')context=rawDom.create(get(this,'ownerDocument'),1,'body','');
        const fragment=rawDom.parseFragment(context,v.html,false,{allowShadow:false,runScripts:!!v.options.runScripts,sanitizing:!!v.options.sanitizer});sanitize(fragment,parseConfig(v.options,false),false);
        const inserted=[];for(let n=get(fragment,'firstChild');n;n=get(n,'nextSibling'))inserted[inserted.length]=n;dom('insert',parent,fragment,before);if(v.options.runScripts)for(let i=0;i<inserted.length;i++)nativeSafety('scripts',inserted[i]);
    }});
    const parserOriginal=DOMParser.prototype.parseFromString;DOMParser.prototype.parseFromString=function(input,type){if(arguments.length<2)return apply(parserOriginal,this,arguments);return apply(parserOriginal,this,[check(input,1,'DOMParser parseFromString',null),type]);};
    for(const key of ['setAttribute','setAttributeNS']){
        const original=Element.prototype[key];Element.prototype[key]=function(...args){get(this,'elementBrand');if(args.length<(key==='setAttributeNS'?3:2))return apply(original,this,args);const isNS=key==='setAttributeNS',ns=isNS?(args[0]==null?null:string(args[0])||null):null,name=string(args[isNS?1:0]),valueIndex=isNS?2:1,local=name.includes(':')?name.slice(name.indexOf(':')+1):name,data=attributeSink(this,isNS?local:name,ns);args[isNS?1:0]=name;if(isNS)args[0]=ns;if(data.kind)args[valueIndex]=check(args[valueIndex],data.kind,data.sink,this);return apply(original,this,args);};
    }
    for(const key of ['value','nodeValue','textContent']){
        const d=Object.getOwnPropertyDescriptor(Attr.prototype,key);define(Attr.prototype,key,{...d,set(value){get(this,'attrBrand');const owner=get(this,'attrOwner');if(owner){const data=attributeSink(owner,get(this,'localName'),get(this,'namespaceURI'));if(data.kind)value=check(value==null?'':value,data.kind,data.sink,owner);}return apply(d.set,this,[value]);}});
    }
    function checkAttributeNode(element,attribute,sink){
        get(element,'elementBrand');get(attribute,'attrBrand');
        const used=get(attribute,'attrOwner');if(used&&used!==element)throw new DOMException('Attribute is owned by another element','InUseAttributeError');
        const data=attributeSink(element,get(attribute,'localName'),get(attribute,'namespaceURI'));
        if(data.kind){const approved=check(get(attribute,'attrValue'),data.kind,data.sink,element);rawDom.set(attribute,'attrValue',approved);}
    }
    for(const key of ['setAttributeNode','setAttributeNodeNS']){
        const original=Element.prototype[key];Element.prototype[key]=function(attribute){if(arguments.length)checkAttributeNode(this,attribute,'Element '+key);return apply(original,this,arguments);};
    }
    for(const key of ['setNamedItem','setNamedItemNS']){
        const original=NamedNodeMap.prototype[key];NamedNodeMap.prototype[key]=function(attribute){if(arguments.length)checkAttributeNode(attributeBridge.owner(this),attribute,'NamedNodeMap '+key);return apply(original,this,arguments);};
    }
    // Contextual fragments keep Range's current native start node, while a
    // default parser policy may disable execution or add a sanitizer.
    const rangeOriginal=Range.prototype.createContextualFragment;
    Range.prototype.createContextualFragment=function(html){
        if(!arguments.length)return apply(rangeOriginal,this,arguments);
        const initial=this.startContainer,initialType=get(initial,'nodeType'),owner=initialType===9?initial:get(initial,'ownerDocument');
        const v=compliantInput(html,{runScripts:true},'Range createContextualFragment',owner,false);
        const start=this.startContainer,type=get(start,'nodeType');let context=type===1?start:type===3||type===4||type===7||type===8?get(start,'parentNode'):null;
        if(!context||get(context,'nodeType')!==1||get(context,'namespaceURI')===HTML&&get(context,'localName')==='html')context=rawDom.create(owner,1,'body','');
        const c=parseConfig(v.options,false),fragment=rawDom.parseFragment(context,v.html,!!v.options.runScripts,{allowShadow:false,runScripts:!!v.options.runScripts,sanitizing:!!c});
        sanitize(fragment,c,false);if(get(fragment,'scripting'))customElementsBridge.upgradeTree(fragment);return fragment;
    };
    for(const method of ['write','writeln']){
        const original=document[method];
        document[method]=function(...values){
            if(get(this,'nodeType')!==9)throw new TypeError('Document receiver required');
            return apply(original,this,[writeInput(values,'Document '+method,this)]);
        };
    }
    for(const method of ['setTimeout','setInterval']){
        const original=globalThis[method];
        globalThis[method]=function(callback,delay,...args){
            if(typeof callback!=='function')callback=make(2,check(callback,2,method));
            // Pass a function to the established timer to avoid converting the
            // approved TrustedScript into an untrusted string a second time.
            if(typeof callback!=='function')callback=new DynamicFunction(callback);
            return apply(original,this,[callback,delay,...args]);
        };
    }
    for(const method of ['assign','replace']){
        const original=location[method];location[method]=function(value){return apply(original,this,[navigationInput(value)]);};
    }
    const href=Object.getOwnPropertyDescriptor(location,'href');
    if(href&&href.configurable)define(location,'href',{...href,set(value){return apply(href.set,this,[navigationInput(value)]);}});
    const windowLocation=Object.getOwnPropertyDescriptor(globalThis,'location');
    if(windowLocation&&windowLocation.configurable)define(globalThis,'location',{...windowLocation,set(value){return apply(windowLocation.set,this,[navigationInput(value)]);}});
    }
    const DynamicFunction=Function;
    function writeInput(values,sink,node){
        let html='',trusted=true;
        for(let i=0;i<values.length;i++){const genuine=trustedKind(values[i])===1;trusted=trusted&&genuine;html+=genuine?payload(values[i],1):string(values[i]);}
        return trusted?html:check(html,1,sink,node);
    }
    function navigationInput(value,node=null){const text=ttString(value);if(!javascriptURL(text))return text;const offset=text.indexOf(':');return text.slice(0,offset+1)+check(text.slice(offset+1),2,'Location javascript',node);}
    function timerCode(value,sink){return make(2,check(value,2,sink));}
    function dynamicCode(value,kind,args=[]){
        if(kind===2)return trustedKind(value)===2?payload(value,2):value;
        if(kind===0){if(trustedKind(value)===2)return payload(value,2);if(typeof value!=='string')return value;return check(value,2,'eval',null);}
        let trusted=args.length>0;for(let i=0;i<args.length;i++)if(trustedKind(args[i])!==2)trusted=false;if(trusted)return value;return check(value,2,'Function',null);
    }
    // Native callers must pass the sink's real owner, not mutable JS fields.
    function nativeMutation(op,node,key,value,namespace=null){
        if(op==='attr'||op==='attrNS'){const data=attributeSink(node,key,namespace);return data.kind?check(value,data.kind,data.sink,node):value;}
        if(op==='set'){
            if(get(node,'nodeType')===2){const owner=get(node,'attrOwner');if(owner){const data=attributeSink(owner,get(node,'localName'),get(node,'namespaceURI'));if(data.kind)return check(value,data.kind,data.sink,owner);}}
            const k=get(node,'nodeType')===1?propertyType(get(node,'localName'),key,get(node,'namespaceURI')):0;return k?check(value,k,'Element '+key,node):value;
        }return value;
    }
    for(const C of [Sanitizer,TrustedHTML,TrustedScript,TrustedScriptURL,TrustedHTMLParserOptions,TrustedTypePolicy,TrustedTypePolicyFactory,SecurityPolicyViolationEvent])define(C.prototype,Symbol.toStringTag,{value:C.name,configurable:true});
    Object.assign(globalThis,{TrustedHTML,TrustedScript,TrustedScriptURL,TrustedTypePolicy,TrustedTypePolicyFactory,SecurityPolicyViolationEvent});
    if(windowRealm)Object.assign(globalThis,{Sanitizer,TrustedHTMLParserOptions});
    else delete TrustedTypePolicy.prototype.createParserOptions;
    define(globalThis,'trustedTypes',{configurable:true,enumerable:true,get(){return trustedTypes;}});
    if(typeof cloneData!=='undefined')cloneData.registerUncloneable(value=>trustedKind(value)!==0||!!wGet(configs,value)||!!wGet(policies,value)||!!wGet(factoryBrands,value),'HTML safety object');
    return {check,options,dynamicCode,mutation:nativeMutation,compliantInput,sanitize,parserDictionary,writeInput,navigationInput,timerCode,violationEvent};
})();
