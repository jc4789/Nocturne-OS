/* Common SVG DOM identity/reflection over native nodes. Not a geometry shim.
 * Native wrappers use these private prototypes, never public constructors. */
const svgBridge = (() => {
    const classes = new WeakMap(), animated = new WeakMap();
    const StringImpl=String, TypeErrorImpl=TypeError, create=Object.create, define=Object.defineProperty;
    const get=WeakMap.prototype.get, set=WeakMap.prototype.set, call=Reflect.apply;
    function string(value) {
        if(typeof value==='symbol')throw new TypeErrorImpl('Cannot convert Symbol to DOMString');
        return StringImpl(value);
    }
    function brand(node) { rawDom('get',node,'svgBrand'); return node; }
    function record(value) {
        const r=call(get,animated,[value]);
        if(!r)throw new TypeErrorImpl('Illegal SVGAnimatedString receiver');
        brand(r.node);return r;
    }
    class SVGAnimatedString {
        constructor(){throw new TypeErrorImpl('Illegal SVGAnimatedString constructor');}
        get baseVal(){const r=record(this);return rawDom('attr',r.node,r.attribute)||'';}
        set baseVal(value){const r=record(this);value=string(value);dom('attr',r.node,r.attribute,value);}
        get animVal(){const r=record(this);return rawDom('attr',r.node,r.attribute)||'';}
    }
    const animatedProto=SVGAnimatedString.prototype;
    class SVGElement extends Element {
        constructor(){throw new TypeErrorImpl('Illegal SVGElement constructor');}
        get className(){
            brand(this);let value=call(get,classes,[this]);
            if(!value){value=create(animatedProto);call(set,animated,[value,{node:this,attribute:'class'}]);call(set,classes,[this,value]);}
            return value;
        }
        get ownerSVGElement(){return rawDom('get',this,'svgOwner');}
        get viewportElement(){return rawDom('get',this,'svgViewport');}
        focus(){brand(this);dom('focus',this);}
        blur(){brand(this);const d=rawDom('get',this,'ownerDocument');if(rawDom('get',d,'activeElement')===this)dom('focus',null);}
    }
    class SVGSVGElement extends SVGElement {
        constructor(){throw new TypeErrorImpl('Illegal SVGSVGElement constructor');}
        getElementById(id){
            rawDom('get',this,'svgRootBrand');
            if(!arguments.length)throw new TypeErrorImpl('Missing elementId');
            return rawDom('id',this,string(id));
        }
    }
    for(const [C,name] of [[SVGElement,'SVGElement'],[SVGSVGElement,'SVGSVGElement'],[SVGAnimatedString,'SVGAnimatedString']])
        define(C.prototype,Symbol.toStringTag,{configurable:true,value:name});
    svgHandlerTarget = node => { try{return rawDom('get',node,'svgBrand')===true;}catch(_){return false;} };
    installHandlers(SVGElement.prototype,globalHandlerTypes);
    Object.assign(globalThis,{SVGElement,SVGSVGElement,SVGAnimatedString});
    return {nodeProtos:[SVGElement.prototype,SVGSVGElement.prototype]};
})();
