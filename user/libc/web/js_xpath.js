/* DOM XPath (https://dom.spec.whatwg.org/#xpath, DOM Level 3 XPath): the Web IDL
 * surface of XPathResult, XPathExpression, XPathEvaluator and Document's
 * XPathEvaluatorBase. Parsing and evaluation are native (xpath.c, through
 * js_xpath_native.h's host.xpathCompile and the xpath DOM operation); this file
 * owns argument conversion, the XPathNSResolver callback interface and each
 * result's state. [Exposed=Window] only. Include after js_document.js. */
(() => {
    const apply=Reflect.apply, define=Object.defineProperty, createObject=Object.create, isArray=Array.isArray;
    const StringImpl=String, NumberImpl=Number, TypeErr=TypeError, DOMExceptionImpl=DOMException;
    const WeakMapImpl=WeakMap, weakGet=WeakMap.prototype.get, weakSet=WeakMap.prototype.set;
    const WeakSetImpl=WeakSet, setHas=WeakSet.prototype.has, setAdd=WeakSet.prototype.add;
    const compile=host.xpathCompile, evaluateNative=rawDom.xpath, getNative=rawDom.get, isNode=rawDom.isNode;
    const brandDocument=documentBridge.brand, reportException=report;
    const results=new WeakMapImpl(), expressions=new WeakMapImpl(), evaluators=new WeakSetImpl();
    const NUMBER=1, STRING=2, BOOLEAN=3, UNORDERED_ITERATOR=4, ORDERED_ITERATOR=5,
        UNORDERED_SNAPSHOT=6, ORDERED_SNAPSHOT=7, ANY_UNORDERED=8, FIRST_ORDERED=9;
    const constants=['ANY_TYPE','NUMBER_TYPE','STRING_TYPE','BOOLEAN_TYPE','UNORDERED_NODE_ITERATOR_TYPE',
        'ORDERED_NODE_ITERATOR_TYPE','UNORDERED_NODE_SNAPSHOT_TYPE','ORDERED_NODE_SNAPSHOT_TYPE',
        'ANY_UNORDERED_NODE_TYPE','FIRST_ORDERED_NODE_TYPE'];

    /* Web IDL conversions. */
    function domString(value){
        if(typeof value==='symbol')throw new TypeErr('Cannot convert a Symbol to a DOMString');
        return StringImpl(value);
    }
    function nodeArgument(value){
        if(!isNode(null,value))throw new TypeErr('The argument is not a Node');
        return value;
    }
    /* XPathNSResolver? is a callback interface: any object, or null. */
    function resolverArgument(value){
        if(value===undefined || value===null)return null;
        if(typeof value!=='object' && typeof value!=='function')throw new TypeErr('The resolver is not an XPathNSResolver');
        return value;
    }
    const unsignedShort=value=>(NumberImpl(value)>>>0)&0xffff;
    const unsignedLong=value=>NumberImpl(value)>>>0;
    function resultArgument(value){
        if(value===undefined || value===null)return null;
        if(apply(weakGet,results,[value])===undefined)throw new TypeErr('The result is not an XPathResult');
        return value;
    }

    /* The prefix lookup of an XPathNSResolver: Web IDL "call a user object's
       operation" lookupNamespaceURI (the object itself when it is callable),
       its DOMString? result converted. An abrupt completion is reported and
       the prefix stays unbound, so the expression fails with NamespaceError. */
    function lookup(resolver){
        if(resolver===null)return null;
        return prefix=>{
            try{
                let operation=resolver, thisArg;
                if(typeof resolver!=='function'){
                    operation=resolver.lookupNamespaceURI;
                    if(typeof operation!=='function')throw new TypeErr('XPathNSResolver.lookupNamespaceURI is not callable');
                    thisArg=resolver;
                }
                const namespace=apply(operation,thisArg,[prefix]);
                return namespace===undefined || namespace===null ? null : domString(namespace);
            }catch(e){reportException(e);return null;}
        };
    }
    /* xpath.h's statuses. */
    function failure(status,detail){
        switch(status){
        case 1: return new DOMExceptionImpl('The string is not a valid XPath expression: '+detail,'SyntaxError');
        case 2: return new DOMExceptionImpl('The prefix "'+detail+'" is not bound to a namespace','NamespaceError');
        case 3: return new TypeErr('The XPath value cannot be converted to the requested result type');
        case 4: return new DOMExceptionImpl('The node is not a valid XPath context node','NotSupportedError');
        case 5: return new DOMExceptionImpl('The XPath result type is not supported','NotSupportedError');
        case 6: return new DOMExceptionImpl('An XPath namespace node cannot be returned as a Node','NotSupportedError');
        default: return new TypeErr('XPath evaluation failed');
        }
    }
    function compiled(expression,resolver){
        const made=compile(expression,lookup(resolver));
        if(isArray(made))throw failure(made[0],made[1]);
        return made;
    }
    const iterator=type=>type===UNORDERED_ITERATOR || type===ORDERED_ITERATOR;
    /* An iterator result is invalid once the context node's document changed. */
    function changed(state){
        const version=getNative(state.context,'collectionVersion');
        return version[0]!==state.version[0] || version[1]!==state.version[1];
    }
    function evaluated(handle,context,type){
        const outcome=evaluateNative(context,handle,type);
        if(typeof outcome==='number')throw failure(outcome,'');
        const result=createObject(XPathResult.prototype), resultType=outcome[0];
        apply(weakSet,results,[result,{type:resultType,value:outcome[1],index:0,context,
            version:iterator(resultType)?getNative(context,'collectionVersion'):null}]);
        return result;
    }
    function state(receiver){
        const s=apply(weakGet,results,[receiver]);
        if(s===undefined)throw new TypeErr('Illegal XPathResult receiver');
        return s;
    }
    function typed(receiver,accepts,what){
        const s=state(receiver);
        if(!accepts(s.type))throw new TypeErr('The XPathResult is not '+what);
        return s;
    }

    class XPathResult {
        constructor(){throw new TypeErr('Illegal constructor');}
        get resultType(){return state(this).type;}
        get numberValue(){return typed(this,t=>t===NUMBER,'a number').value;}
        get stringValue(){return typed(this,t=>t===STRING,'a string').value;}
        get booleanValue(){return typed(this,t=>t===BOOLEAN,'a boolean').value;}
        get singleNodeValue(){
            const s=typed(this,t=>t===ANY_UNORDERED || t===FIRST_ORDERED,'a single node');
            return s.value.length?s.value[0]:null;
        }
        get invalidIteratorState(){const s=state(this);return iterator(s.type) && changed(s);}
        get snapshotLength(){return typed(this,t=>t===UNORDERED_SNAPSHOT || t===ORDERED_SNAPSHOT,'a snapshot').value.length;}
        iterateNext(){
            const s=typed(this,iterator,'a node iterator');
            if(changed(s))throw new DOMExceptionImpl('The document changed since the XPath result was returned','InvalidStateError');
            return s.index<s.value.length?s.value[s.index++]:null;
        }
        snapshotItem(index){
            const s=typed(this,t=>t===UNORDERED_SNAPSHOT || t===ORDERED_SNAPSHOT,'a snapshot');
            if(!arguments.length)throw new TypeErr('snapshotItem requires an index');
            index=unsignedLong(index);
            return index<s.value.length?s.value[index]:null;
        }
    }
    class XPathExpression {
        constructor(){throw new TypeErr('Illegal constructor');}
        evaluate(contextNode,type=0,result=null){
            const handle=apply(weakGet,expressions,[this]);
            if(handle===undefined)throw new TypeErr('Illegal XPathExpression receiver');
            if(!arguments.length)throw new TypeErr('evaluate requires a context node');
            nodeArgument(contextNode);type=unsignedShort(type);resultArgument(result);
            return evaluated(handle,contextNode,type);
        }
    }
    /* XPathEvaluatorBase, for XPathEvaluator and Document. */
    function evaluatorBase(brand){
        return {
            createExpression(expression,resolver=null){
                brand(this);
                if(!arguments.length)throw new TypeErr('createExpression requires an expression');
                expression=domString(expression);resolver=resolverArgument(resolver);
                const made=createObject(XPathExpression.prototype);
                apply(weakSet,expressions,[made,compiled(expression,resolver)]);
                return made;
            },
            createNSResolver(nodeResolver){
                brand(this);
                if(!arguments.length)throw new TypeErr('createNSResolver requires a Node');
                return nodeArgument(nodeResolver);
            },
            evaluate(expression,contextNode,resolver=null,type=0,result=null){
                brand(this);
                if(arguments.length<2)throw new TypeErr('evaluate requires an expression and a context node');
                expression=domString(expression);nodeArgument(contextNode);resolver=resolverArgument(resolver);
                type=unsignedShort(type);resultArgument(result);
                return evaluated(compiled(expression,resolver),contextNode,type);
            }
        };
    }
    class XPathEvaluator {
        constructor(){apply(setAdd,evaluators,[this]);}
    }
    function members(target,source){
        for(const key of Object.getOwnPropertyNames(source))if(key!=='constructor'){
            const descriptor=Object.getOwnPropertyDescriptor(source,key);
            define(target,key,{...descriptor,enumerable:true,configurable:true});
        }
    }
    members(XPathEvaluator.prototype,evaluatorBase(receiver=>{
        if(!apply(setHas,evaluators,[receiver]))throw new TypeErr('Illegal XPathEvaluator receiver');
    }));
    members(Document.prototype,evaluatorBase(brandDocument));
    for(const C of [XPathResult,XPathExpression])members(C.prototype,C.prototype);
    constants.forEach((name,value)=>{
        for(const target of [XPathResult,XPathResult.prototype])
            define(target,name,{value,writable:false,enumerable:true,configurable:false});
    });
    for(const C of [XPathResult,XPathExpression,XPathEvaluator]){
        define(C.prototype,Symbol.toStringTag,{value:C.name,configurable:true});
        define(globalThis,C.name,{value:C,writable:true,enumerable:false,configurable:true});
    }
})();
