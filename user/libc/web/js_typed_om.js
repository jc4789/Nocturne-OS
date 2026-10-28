/* CSS Typed OM Level 1 (https://drafts.css-houdini.org/css-typed-om-1/): CSSStyleValue and its
   subclasses, CSSNumericValue's arithmetic, CSS.px() and the other factories, transforms, colours,
   reification and the style property maps, one implementation for the Window and worker realms.

   The values live in private records, never in author-visible properties. What CSS itself owns stays
   native (js_typed_om.c): the numeric types and units are cssmath.c's (§10.7), a math function is
   read as cssmath.c simplifies and serializes the specified value, a property's grammar is
   cssprop.c's, a colour color.c's and the tokens CSS Syntax's. Web IDL's shape (lengths, enumerable
   members, the value iterators' %Array.prototype% functions) is js_webidl.c's.

   options.native: host's typed* functions. options.geometry: the realm's geometry bridge
   (js_geometry.js) for toMatrix() and CSSMatrixComponent. options.DOMException: the realm's.
   Window only: options.css, the CSS namespace for the factories; options.declarations, how a style
   property map reaches a CSSStyleDeclaration's block (read, write, names) and an element's computed
   values (computed, computedNames); with it come CSSStyleValue.parse(), CSSNumericValue.parse(),
   CSSColorValue.parse() and the maps. */
function typedOMInterfaces(options){
    const native=options.native,geometry=options.geometry,DOMExceptionImpl=options.DOMException;
    const declarations=options.declarations||null,windowRealm=!!declarations;
    const TypeErrorImpl=TypeError,RangeErrorImpl=RangeError,call=Reflect.apply,create=Object.create,define=Object.defineProperty;
    const recordOf=WeakMap.prototype.get,recordSet=WeakMap.prototype.set,isFinite=Number.isFinite,abs=Math.abs;
    const tan=Math.tan,PI=Math.PI,wellFormed=String.prototype.toWellFormed;
    const records=new WeakMap(),token={};
    const syntaxError=message=>new DOMExceptionImpl(message,'SyntaxError');
    function rec(v){return (typeof v==='object'&&v!==null)||typeof v==='function'?call(recordOf,records,[v]):undefined;}
    function adopt(o,r){call(recordSet,records,[o,r]);return o;}
    function make(C,r){return adopt(create(C.prototype),r);}
    function own(v,kinds){const r=rec(v);if(!r||!kinds.includes(r.kind))throw new TypeErrorImpl('Illegal invocation');return r;}
    function required(count,n,what){if(count<n)throw new TypeErrorImpl(what+' requires '+n+' argument'+(n>1?'s':''));}
    /* Web IDL conversions */
    function usv(v){const s=`${v}`;return wellFormed?call(wellFormed,s,[]):s;}
    function double(v){const n=+v;if(!isFinite(n))throw new TypeErrorImpl('The value is not a finite number');return n;}

    /* ------------------------------------------------------------------ units and types (§4.3.2) */
    const CALC={number:1,percent:2,length:4,angle:8,time:16,frequency:32,resolution:64,flex:128,any:255};
    const BASES=['length','angle','time','frequency','resolution','flex','percent'];
    const BASE_CALC=[CALC.length,CALC.angle,CALC.time,CALC.frequency,CALC.resolution,CALC.flex,CALC.percent];
    const unitRows=native.typedUnits(),units=new Map(),canonicalOf=[];
    const unitName=i=>unitRows[i][0]===''?'number':unitRows[i][0]==='%'?'percent':unitRows[i][0];
    for(let i=0;i<unitRows.length;i++)units.set(unitName(i),i);
    /* the canonical unit of each base type: the first absolute unit of factor 1 the table lists */
    for(let i=0;i<unitRows.length;i++){const [,base,relative,factor]=unitRows[i];if(base>=0&&base<6&&!relative&&factor===1&&canonicalOf[base]===undefined)canonicalOf[base]=i;}
    const NUMBER=units.get('number'),PERCENT=units.get('percent'),PX=units.get('px'),DEG=units.get('deg');
    /* "create a type" from a unit: [7 exponents, percent hint], or null */
    function unitType(i){const t=[0,0,0,0,0,0,0,-1],base=unitRows[i][1];if(base>=0)t[base]=1;return t;}
    function unitIndex(name){const i=units.get(usv(name).toLowerCase());return i===undefined?-1:i;}
    const addTypes=(a,b)=>native.typedType(0,a,b),multiplyTypes=(a,b)=>native.typedType(1,a,b);
    function invertType(t){const r=t.slice();for(let i=0;i<7;i++)r[i]=-r[i]||0;return r;}
    const matches=(value,accept)=>native.typedMatch(numericRecord(value).type,accept)!==0;
    /* two units are compatible when one converts to the other: the same absolute type, or the same unit */
    function compatible(a,b){
        if(a===b)return true;
        const [,ba,ra]=unitRows[a],[,bb,rb]=unitRows[b];
        return ba===bb&&ba>=0&&ba<6&&!ra&&!rb;
    }
    function convertValue(v,from,to){return from===to?v:v*unitRows[from][3]/unitRows[to][3];}

    /* ------------------------------------------------------------------ CSSStyleValue (§4.1) */
    class CSSStyleValue {
        constructor(key){if(key!==token)throw new TypeErrorImpl('Illegal constructor');}
        toString(){return serialize(this);}
    }
    /* a value Typed OM has no object for (§5.2): its text, valid only for its associated property */
    function styleValue(text,property){return make(CSSStyleValue,{kind:'style',text,property});}

    /* ------------------------------------------------------------------ CSSKeywordValue (§4.4) */
    class CSSKeywordValue extends CSSStyleValue {
        constructor(value){required(arguments.length,1,'CSSKeywordValue');super(token);
            value=usv(value);if(!value)throw new TypeErrorImpl('A keyword cannot be empty');adopt(this,{kind:'keyword',value});}
        get value(){return own(this,['keyword']).value;}
        set value(v){const r=own(this,['keyword']);v=usv(v);if(!v)throw new TypeErrorImpl('A keyword cannot be empty');r.value=v;}
    }
    const keyword=value=>make(CSSKeywordValue,{kind:'keyword',value});
    /* "rectify a keywordish value" */
    function keywordish(v){return rec(v)&&rec(v).kind==='keyword'?v:keyword(usv(v));}

    /* ------------------------------------------------------------------ CSSUnparsedValue (§4.2) */
    const indexKey=k=>typeof k==='string'&&/^(0|[1-9][0-9]*)$/.test(k)&&+k<4294967295?+k:-1;
    /* a platform object with an indexed property getter (and setter when put is given) over r.items */
    function indexed(target,r,put){
        const proxy=new Proxy(target,{
            get(t,k,receiver){const i=indexKey(k);return i>=0?r.items[i]:Reflect.get(t,k,receiver);},
            set(t,k,v,receiver){const i=indexKey(k);if(i<0)return Reflect.set(t,k,v,receiver);if(!put)return false;put(r,i,v);return true;},
            has(t,k){const i=indexKey(k);return i>=0?i<r.items.length:Reflect.has(t,k);},
            ownKeys(t){const keys=[];for(let i=0;i<r.items.length;i++)keys.push(String(i));return keys.concat(Reflect.ownKeys(t));},
            getOwnPropertyDescriptor(t,k){const i=indexKey(k);if(i<0)return Reflect.getOwnPropertyDescriptor(t,k);
                return i<r.items.length?{value:r.items[i],writable:!!put,enumerable:true,configurable:true}:undefined;},
            defineProperty(t,k,d){const i=indexKey(k);if(i<0)return Reflect.defineProperty(t,k,d);
                if(!put||!('value' in d)||d.get||d.set)return false;put(r,i,d.value);return true;},
            deleteProperty(t,k){const i=indexKey(k);return i<0?Reflect.deleteProperty(t,k):i>=r.items.length;}
        });
        adopt(target,r);return adopt(proxy,r);
    }
    /* Web IDL indexed setters of a list: an index past the end appends; further is a RangeError */
    function listSetter(convert){return (r,i,v)=>{v=convert(v);if(i>r.items.length)throw new RangeErrorImpl('Index '+i+' is past the end');r.items[i]=v;};}
    const segment=v=>rec(v)&&rec(v).kind==='varref'?v:usv(v);
    class CSSUnparsedValue extends CSSStyleValue {
        constructor(members){required(arguments.length,1,'CSSUnparsedValue');super(token);
            return indexed(this,{kind:'unparsed',items:sequence(members,segment)},listSetter(segment));}
        get length(){return own(this,['unparsed']).items.length;}
    }
    function unparsed(items){return indexed(create(CSSUnparsedValue.prototype),{kind:'unparsed',items},listSetter(segment));}
    /* a Web IDL sequence<T>: an iterable object, converted item by item */
    function sequence(v,convert){
        if((typeof v!=='object'||v===null)&&typeof v!=='function')throw new TypeErrorImpl('Expected a sequence');
        const iterator=v[Symbol.iterator];if(typeof iterator!=='function')throw new TypeErrorImpl('Expected a sequence');
        const out=[];for(const item of {[Symbol.iterator]:()=>call(iterator,v,[])})out.push(convert(item));return out;
    }
    function customName(v){v=usv(v);if(!v.startsWith('--'))throw new TypeErrorImpl("A custom property name starts with '--'");return v;}
    class CSSVariableReferenceValue {
        constructor(variable,fallback=null){required(arguments.length,1,'CSSVariableReferenceValue');
            variable=customName(variable);
            if(fallback!==null&&fallback!==undefined&&(!rec(fallback)||rec(fallback).kind!=='unparsed'))throw new TypeErrorImpl('The fallback is a CSSUnparsedValue');
            adopt(this,{kind:'varref',variable,fallback:fallback??null});}
        get variable(){return own(this,['varref']).variable;}
        set variable(v){own(this,['varref']).variable=customName(v);}
        get fallback(){return own(this,['varref']).fallback;}
    }
    const varref=(variable,fallback)=>make(CSSVariableReferenceValue,{kind:'varref',variable,fallback});

    /* ------------------------------------------------------------------ CSSNumericValue (§4.3) */
    const NUMERIC=['unit','sum','product','negate','invert','min','max','clamp'];
    function numericRecord(v){return own(v,NUMERIC);}
    function isNumeric(v){const r=rec(v);return !!r&&NUMERIC.includes(r.kind);}
    /* "rectify a numberish value" */
    function numberish(v){return isNumeric(v)?v:unitValue(double(v),NUMBER);}
    function unitValue(value,unit){return make(CSSUnitValue,{kind:'unit',value,unit,type:unitType(unit)});}
    /* the type of a math value's items: add (sum, min, max, clamp) or multiply (product); null: they cannot be */
    function combinedType(items,multiply){
        let type=numericRecord(items[0]).type;
        for(let i=1;i<items.length&&type;i++)type=(multiply?multiplyTypes:addTypes)(type,numericRecord(items[i]).type);
        return type;
    }
    function mathValue(C,kind,items){
        const type=combinedType(items,kind==='product');
        if(!type)throw new TypeErrorImpl('The values cannot be combined: their types do not match');
        if(kind==='clamp')return make(C,{kind,lower:items[0],value:items[1],upper:items[2],type});
        const r={kind,items,type};r.values=numericArray(items);return make(C,r);
    }
    const allUnits=(items,unit)=>items.every(v=>{const r=numericRecord(v);return r.kind==='unit'&&(unit===undefined||r.unit===unit);});
    function negate(v){const r=numericRecord(v);
        if(r.kind==='negate')return r.value;
        if(r.kind==='unit')return unitValue(-r.value,r.unit);
        return make(CSSMathNegate,{kind:'negate',value:v,type:r.type});}
    function invert(v){const r=numericRecord(v);
        if(r.kind==='invert')return r.value;
        if(r.kind==='unit'&&r.unit===NUMBER){if(r.value===0)throw new RangeErrorImpl('Cannot divide by zero');return unitValue(1/r.value,NUMBER);}
        return make(CSSMathInvert,{kind:'invert',value:v,type:invertType(r.type)});}
    function prepend(self,kind,values){const r=numericRecord(self);return (r.kind===kind?r.items:[self]).concat(values);}
    function add(self,values){
        values=prepend(self,'sum',values);
        const first=numericRecord(values[0]);
        if(allUnits(values,first.kind==='unit'?first.unit:-1)){let sum=0;for(const v of values)sum+=numericRecord(v).value;return unitValue(sum,first.unit);}
        return mathValue(CSSMathSum,'sum',values);
    }
    function multiply(self,values){
        values=prepend(self,'product',values);
        if(allUnits(values)){
            let product=1,unit=NUMBER,others=0;
            for(const v of values){const r=numericRecord(v);product*=r.value;if(r.unit!==NUMBER){unit=r.unit;others++;}}
            if(others<=1)return unitValue(product,unit);
        }
        return mathValue(CSSMathProduct,'product',values);
    }
    function extreme(self,kind,values){
        values=prepend(self,kind,values);
        const first=numericRecord(values[0]);
        if(allUnits(values,first.kind==='unit'?first.unit:-1)){
            let best=first.value;for(const v of values){const x=numericRecord(v).value;best=kind==='min'?Math.min(best,x):Math.max(best,x);}
            return unitValue(best,first.unit);
        }
        return mathValue(kind==='min'?CSSMathMin:CSSMathMax,kind,values);
    }
    /* "equal numeric value" */
    function equal(a,b){
        const x=numericRecord(a),y=numericRecord(b);
        if(x.kind!==y.kind)return false;
        switch(x.kind){
        case 'unit':return x.value===y.value&&x.unit===y.unit;
        case 'negate':case 'invert':return equal(x.value,y.value);
        case 'clamp':return equal(x.lower,y.lower)&&equal(x.value,y.value)&&equal(x.upper,y.upper);
        }
        return x.items.length===y.items.length&&x.items.every((v,i)=>equal(v,y.items[i]));
    }
    /* "create a sum value": [[value, Map(unit -> power)], ...], or null */
    function mapKey(map){return [...map].sort((a,b)=>a[0]-b[0]).map(e=>e[0]+':'+e[1]).join(',');}
    function sameMaps(items){const key=mapKey(items[0][0][1]);return items.every(s=>mapKey(s[0][1])===key);}
    function sumValue(v){
        const r=numericRecord(v);
        switch(r.kind){
        case 'unit':{
            let value=r.value,unit=r.unit;const base=unitRows[unit][1];
            if(base>=0&&base<6&&!unitRows[unit][2]){value=convertValue(value,unit,canonicalOf[base]);unit=canonicalOf[base];}
            return [[value,unit===NUMBER?new Map():new Map([[unit,1]])]];
        }
        case 'sum':{
            const out=[];
            for(const item of r.items){
                const s=sumValue(item);if(!s)return null;
                for(const [value,map] of s){const same=out.find(o=>mapKey(o[1])===mapKey(map));if(same)same[0]+=value;else out.push([value,map]);}
            }
            let type=null;
            for(const [,map] of out){
                let t=[0,0,0,0,0,0,0,-1];
                for(const [unit,power] of map){const base=unitRows[unit][1];if(base>=0)t[base]+=power;}
                type=type?addTypes(type,t):t;if(!type)return null;
            }
            return out;
        }
        case 'negate':{const s=sumValue(r.value);if(!s)return null;return s.map(([value,map])=>[-value,map]);}
        case 'product':{
            let out=[[1,new Map()]];
            for(const item of r.items){
                const s=sumValue(item);if(!s)return null;
                const next=[];
                for(const [v1,m1] of out)for(const [v2,m2] of s){
                    const map=new Map(m1);
                    for(const [unit,power] of m2){const p=(map.get(unit)||0)+power;if(p)map.set(unit,p);else map.delete(unit);}
                    next.push([v1*v2,map]);
                }
                out=next;
            }
            return out;
        }
        case 'invert':{
            const s=sumValue(r.value);if(!s||s.length!==1)return null;
            const map=new Map();for(const [unit,power] of s[0][1])map.set(unit,-power);
            return [[1/s[0][0],map]];
        }
        case 'min':case 'max':{
            const args=r.items.map(sumValue);
            if(args.some(a=>!a||a.length!==1)||!sameMaps(args))return null;
            let best=args[0];for(const a of args)if(r.kind==='min'?a[0][0]<best[0][0]:a[0][0]>best[0][0])best=a;
            return best;
        }
        case 'clamp':{
            const args=[r.lower,r.value,r.upper].map(sumValue);
            if(args.some(a=>!a||a.length!==1)||!sameMaps(args))return null;
            return [[Math.max(args[0][0][0],Math.min(args[1][0][0],args[2][0][0])),args[0][0][1]]];
        }
        }
        return null;
    }
    /* "create a CSSUnitValue from a sum value item" */
    function itemUnitValue([value,map]){
        if(map.size>1)return null;
        if(!map.size)return unitValue(value,NUMBER);
        const [[unit,power]]=map;return power===1?unitValue(value,unit):null;
    }
    /* "convert a CSSUnitValue": null when the units are not compatible */
    function convert(v,unit){const r=numericRecord(v);return compatible(r.unit,unit)?unitValue(convertValue(r.value,r.unit,unit),unit):null;}
    /* to(unit): TypeError where the value is not one of unit's compatible units */
    function to(v,unit){
        const sum=sumValue(v);
        if(!sum||sum.length!==1)throw new TypeErrorImpl('The value cannot be expressed in one unit');
        const item=itemUnitValue(sum[0]),out=item&&convert(item,unit);
        if(!out)throw new TypeErrorImpl('The value cannot be converted to '+unitName(unit));
        return out;
    }
    function unitArgument(name){const i=unitIndex(name);if(i<0)throw syntaxError("'"+usv(name)+"' is not a CSS unit");return i;}
    class CSSNumericValue extends CSSStyleValue {
        add(...values){numericRecord(this);return add(this,values.map(numberish));}
        sub(...values){numericRecord(this);return add(this,values.map(v=>negate(numberish(v))));}
        mul(...values){numericRecord(this);return multiply(this,values.map(numberish));}
        div(...values){numericRecord(this);return multiply(this,values.map(v=>invert(numberish(v))));}
        min(...values){numericRecord(this);return extreme(this,'min',values.map(numberish));}
        max(...values){numericRecord(this);return extreme(this,'max',values.map(numberish));}
        equals(...values){numericRecord(this);values=values.map(numberish);return values.every(v=>equal(this,v));}
        to(unit){numericRecord(this);required(arguments.length,1,'to');return to(this,unitArgument(unit));}
        toSum(...names){
            numericRecord(this);
            const wanted=names.map(unitArgument),sum=sumValue(this);
            if(!sum)throw new TypeErrorImpl('The value has no sum value');
            let values=sum.map(itemUnitValue);
            if(values.some(v=>!v))throw new TypeErrorImpl('The value has a term of no single unit');
            if(!wanted.length){
                values.sort((a,b)=>{const x=unitName(numericRecord(a).unit),y=unitName(numericRecord(b).unit);return x<y?-1:x>y?1:0;});
                return mathValue(CSSMathSum,'sum',values);
            }
            const result=[];
            for(const unit of wanted){
                let total=0;
                values=values.filter(v=>{const c=convert(v,unit);if(!c)return true;total+=numericRecord(c).value;return false;});
                result.push(unitValue(total,unit));
            }
            if(values.length)throw new TypeErrorImpl('The value has terms none of the units take');
            return mathValue(CSSMathSum,'sum',result);
        }
        type(){
            const t=numericRecord(this).type,out={};
            for(let i=0;i<7;i++)if(t[i])out[BASES[i]]=t[i];
            if(t[7]>=0)out.percentHint=BASES[t[7]];
            return out;
        }
    }
    class CSSUnitValue extends CSSNumericValue {
        constructor(value,unit){required(arguments.length,2,'CSSUnitValue');super(token);
            value=double(value);const i=unitIndex(unit);if(i<0)throw new TypeErrorImpl("'"+usv(unit)+"' is not a CSS unit");
            adopt(this,{kind:'unit',value,unit:i,type:unitType(i)});}
        get value(){return own(this,['unit']).value;}
        set value(v){own(this,['unit']).value=double(v);}
        get unit(){return unitName(own(this,['unit']).unit);}
    }
    class CSSMathValue extends CSSNumericValue {
        get operator(){const r=own(this,['sum','product','negate','invert','min','max','clamp']);return r.kind;}
    }
    function variadic(C,kind,args){if(!args.length)throw syntaxError(C.name+' needs at least one value');return mathValue(C,kind,args.map(numberish));}
    class CSSMathSum extends CSSMathValue {
        constructor(...args){super(token);return adopt(this,rec(variadic(CSSMathSum,'sum',args)));}
        get values(){return own(this,['sum']).values;}
    }
    class CSSMathProduct extends CSSMathValue {
        constructor(...args){super(token);return adopt(this,rec(variadic(CSSMathProduct,'product',args)));}
        get values(){return own(this,['product']).values;}
    }
    class CSSMathMin extends CSSMathValue {
        constructor(...args){super(token);return adopt(this,rec(variadic(CSSMathMin,'min',args)));}
        get values(){return own(this,['min']).values;}
    }
    class CSSMathMax extends CSSMathValue {
        constructor(...args){super(token);return adopt(this,rec(variadic(CSSMathMax,'max',args)));}
        get values(){return own(this,['max']).values;}
    }
    class CSSMathNegate extends CSSMathValue {
        constructor(arg){required(arguments.length,1,'CSSMathNegate');super(token);
            const value=numberish(arg);adopt(this,{kind:'negate',value,type:numericRecord(value).type});}
        get value(){return own(this,['negate','invert']).value;}
    }
    class CSSMathInvert extends CSSMathValue {
        constructor(arg){required(arguments.length,1,'CSSMathInvert');super(token);
            const value=numberish(arg);adopt(this,{kind:'invert',value,type:invertType(numericRecord(value).type)});}
        get value(){return own(this,['invert']).value;}
    }
    class CSSMathClamp extends CSSMathValue {
        constructor(lower,value,upper){required(arguments.length,3,'CSSMathClamp');super(token);
            return adopt(this,rec(mathValue(CSSMathClamp,'clamp',[numberish(lower),numberish(value),numberish(upper)])));}
        get lower(){return own(this,['clamp']).lower;}
        get value(){return own(this,['clamp']).value;}
        get upper(){return own(this,['clamp']).upper;}
    }
    /* CSSMathNegate's value getter serves CSSMathInvert's too: keep each on its own */
    define(CSSMathNegate.prototype,'value',{configurable:true,enumerable:true,get:Object.getOwnPropertyDescriptor({get value(){return own(this,['negate']).value;}},'value').get});
    class CSSNumericArray {
        constructor(key){if(key!==token)throw new TypeErrorImpl('Illegal constructor');}
        get length(){return own(this,['array']).items.length;}
    }
    function numericArray(items){return indexed(create(CSSNumericArray.prototype),{kind:'array',items},null);}

    /* ------------------------------------------------------------------ serialization (§6) */
    function numberText(v){return native.typedNumber(v);}
    function unitText(r){return numberText(r.value)+(r.unit===NUMBER?'':r.unit===PERCENT?'%':unitName(r.unit));}
    /* "serialize a CSSMathValue" with its nested and paren-less flags */
    function mathText(v,nested,parenless){
        const r=numericRecord(v);
        if(r.kind==='unit')return unitText(r);
        if(r.kind==='min'||r.kind==='max')return r.kind+'('+r.items.map(x=>mathText(x,true,true)).join(', ')+')';
        if(r.kind==='clamp')return 'clamp('+[r.lower,r.value,r.upper].map(x=>mathText(x,true,true)).join(', ')+')';
        let s=parenless?'':nested?'(':'calc(';
        if(r.kind==='sum'||r.kind==='product'){
            const other=r.kind==='sum'?'negate':'invert';
            s+=mathText(r.items[0],true,false);
            for(let i=1;i<r.items.length;i++){
                const x=numericRecord(r.items[i]);
                if(x.kind===other)s+=(r.kind==='sum'?' - ':' / ')+mathText(x.value,true,false);
                else s+=(r.kind==='sum'?' + ':' * ')+mathText(r.items[i],true,false);
            }
        } else if(r.kind==='negate')s+='-'+mathText(r.value,true,false);
        else s+='1 / '+mathText(r.value,true,false);
        return parenless?s:s+')';
    }
    function serialize(v){
        const r=rec(v);
        if(!r)throw new TypeErrorImpl('Illegal invocation');
        switch(r.kind){
        case 'style':case 'image':return r.text;
        case 'keyword':return r.value;
        case 'unparsed':return unparsedText(r.items);
        case 'transform':return r.items.map(serialize).join(' ');
        case 'translate':return r.is2D?'translate('+serialize(r.x)+', '+serialize(r.y)+')':
            'translate3d('+serialize(r.x)+', '+serialize(r.y)+', '+serialize(r.z)+')';
        case 'rotate':return r.is2D?'rotate('+serialize(r.angle)+')':
            'rotate3d('+serialize(r.x)+', '+serialize(r.y)+', '+serialize(r.z)+', '+serialize(r.angle)+')';
        case 'scale':return r.is2D?(equal(r.x,r.y)?'scale('+serialize(r.x)+')':'scale('+serialize(r.x)+', '+serialize(r.y)+')'):
            'scale3d('+serialize(r.x)+', '+serialize(r.y)+', '+serialize(r.z)+')';
        case 'skew':{const ay=numericRecord(r.ay);
            return ay.kind==='unit'&&ay.value===0?'skew('+serialize(r.ax)+')':'skew('+serialize(r.ax)+', '+serialize(r.ay)+')';}
        case 'skewx':return 'skewX('+serialize(r.ax)+')';
        case 'skewy':return 'skewY('+serialize(r.ay)+')';
        case 'perspective':{const l=rec(r.length);
            /* a length with a minimum of 0px */
            return 'perspective('+(l.kind==='unit'&&l.value<0?'calc('+unitText(l)+')':serialize(r.length))+')';}
        case 'matrix':{const m=geometry.matrixValues(r.matrix).m;
            return r.is2D?'matrix('+[m[0],m[1],m[4],m[5],m[12],m[13]].join(', ')+')':'matrix3d('+m.join(', ')+')';}
        case 'rgb':case 'hsl':case 'hwb':case 'lab':case 'lch':case 'oklab':case 'oklch':case 'color':return colorText(r);
        }
        return mathText(v,false,false);
    }
    function varrefText(r){return 'var('+r.variable+(r.fallback?','+serialize(r.fallback):'')+')';}
    /* CSS Syntax §9: a comment where two pieces would otherwise tokenize as one */
    function unparsedText(items){
        let out='',previous=null;
        for(const item of items){
            const ref=rec(item),text=ref&&ref.kind==='varref'?varrefText(ref):item;
            if(previous&&text&&native.typedBoundary(previous,text))out+='/**/';
            out+=text;if(text)previous=text;
        }
        return out;
    }

    /* ------------------------------------------------------------------ transforms (§4.6) */
    function numericArg(v,what){if(!isNumeric(v))throw new TypeErrorImpl(what+' is a CSSNumericValue');return v;}
    function lengthPercentage(v){numericArg(v,'The coordinate');if(!matches(v,CALC.length|CALC.percent))throw new TypeErrorImpl('The coordinate must be a <length-percentage>');return v;}
    function length(v){numericArg(v,'The value');if(!matches(v,CALC.length))throw new TypeErrorImpl('The value must be a <length>');return v;}
    function angle(v){numericArg(v,'The angle');if(!matches(v,CALC.angle))throw new TypeErrorImpl('The angle must be an <angle>');return v;}
    function number(v){v=numberish(v);if(!matches(v,CALC.number))throw new TypeErrorImpl('The value must be a <number>');return v;}
    const COMPONENTS=['translate','rotate','scale','skew','skewx','skewy','perspective','matrix'];
    class CSSTransformComponent {
        constructor(key){if(key!==token)throw new TypeErrorImpl('Illegal constructor');}
        toString(){own(this,COMPONENTS);return serialize(this);}
        get is2D(){return own(this,COMPONENTS).is2D;}
        set is2D(v){const r=own(this,COMPONENTS);v=!!v;if(r.kind!=='skew'&&r.kind!=='skewx'&&r.kind!=='skewy'&&r.kind!=='perspective')r.is2D=v;}
        toMatrix(){const m=componentMatrix(this);return m.is2D?geometry.matrix2D(m.m):geometry.matrix(m.m);}
    }
    function attribute(C,kind,name,check){
        define(C.prototype,name,{configurable:true,enumerable:true,
            get:{[name](){return own(this,[kind])[name];}}[name],
            set:{[name](v){const r=own(this,[kind]);r[name]=check(v);}}[name]});
    }
    class CSSTranslate extends CSSTransformComponent {
        constructor(x,y,z){required(arguments.length,2,'CSSTranslate');super(token);
            x=lengthPercentage(x);y=lengthPercentage(y);const is2D=z===undefined;z=is2D?unitValue(0,PX):length(z);
            adopt(this,{kind:'translate',x,y,z,is2D});}
    }
    attribute(CSSTranslate,'translate','x',lengthPercentage);attribute(CSSTranslate,'translate','y',lengthPercentage);attribute(CSSTranslate,'translate','z',length);
    class CSSRotate extends CSSTransformComponent {
        constructor(...args){super(token);
            if(args.length===1)adopt(this,{kind:'rotate',x:unitValue(0,NUMBER),y:unitValue(0,NUMBER),z:unitValue(1,NUMBER),angle:angle(args[0]),is2D:true});
            else if(args.length>=4)adopt(this,{kind:'rotate',x:number(args[0]),y:number(args[1]),z:number(args[2]),angle:angle(args[3]),is2D:false});
            else throw new TypeErrorImpl('CSSRotate takes an angle, or x, y, z and an angle');}
    }
    for(const name of ['x','y','z'])attribute(CSSRotate,'rotate',name,number);attribute(CSSRotate,'rotate','angle',angle);
    class CSSScale extends CSSTransformComponent {
        constructor(x,y,z){required(arguments.length,2,'CSSScale');super(token);
            x=number(x);y=number(y);const is2D=z===undefined;z=is2D?unitValue(1,NUMBER):number(z);
            adopt(this,{kind:'scale',x,y,z,is2D});}
    }
    for(const name of ['x','y','z'])attribute(CSSScale,'scale',name,number);
    class CSSSkew extends CSSTransformComponent {
        constructor(ax,ay){required(arguments.length,2,'CSSSkew');super(token);adopt(this,{kind:'skew',ax:angle(ax),ay:angle(ay),is2D:true});}
    }
    attribute(CSSSkew,'skew','ax',angle);attribute(CSSSkew,'skew','ay',angle);
    class CSSSkewX extends CSSTransformComponent {
        constructor(ax){required(arguments.length,1,'CSSSkewX');super(token);adopt(this,{kind:'skewx',ax:angle(ax),is2D:true});}
    }
    attribute(CSSSkewX,'skewx','ax',angle);
    class CSSSkewY extends CSSTransformComponent {
        constructor(ay){required(arguments.length,1,'CSSSkewY');super(token);adopt(this,{kind:'skewy',ay:angle(ay),is2D:true});}
    }
    attribute(CSSSkewY,'skewy','ay',angle);
    /* CSSPerspectiveValue: a <length>, or the keyword none */
    function perspectiveLength(v){
        if(isNumeric(v))return length(v);
        v=keywordish(v);if(rec(v).value.toLowerCase()!=='none')throw new TypeErrorImpl("A perspective is a <length> or 'none'");return v;
    }
    class CSSPerspective extends CSSTransformComponent {
        constructor(length){required(arguments.length,1,'CSSPerspective');super(token);adopt(this,{kind:'perspective',length:perspectiveLength(length),is2D:false});}
    }
    attribute(CSSPerspective,'perspective','length',perspectiveLength);
    function domMatrix(v){const {m,is2D}=geometry.matrixValues(v);return {copy:is2D?geometry.matrix2D(m):geometry.matrix(m),is2D};}
    class CSSMatrixComponent extends CSSTransformComponent {
        constructor(matrix,options={}){required(arguments.length,1,'CSSMatrixComponent');super(token);
            const {copy,is2D}=domMatrix(matrix);
            if(options!==undefined&&options!==null&&typeof options!=='object'&&typeof options!=='function')throw new TypeErrorImpl('Expected CSSMatrixComponentOptions');
            const given=options==null?undefined:options.is2D;
            adopt(this,{kind:'matrix',matrix:copy,is2D:given===undefined?is2D:!!given});}
        get matrix(){return own(this,['matrix']).matrix;}
        set matrix(v){const r=own(this,['matrix']);geometry.matrixValues(v);if(!(v instanceof geometry.DOMMatrix))throw new TypeErrorImpl('Expected a DOMMatrix');r.matrix=v;}
    }
    /* a component's matrix (CSS Transforms 2): values in px, deg and numbers, or a TypeError */
    function px(v){return numericRecord(to(v,PX)).value;}
    function degrees(v){return numericRecord(to(v,DEG)).value;}
    function scalar(v){return numericRecord(to(v,NUMBER)).value;}
    function componentMatrix(v){
        const r=own(v,COMPONENTS),m=geometry.identity();
        switch(r.kind){
        case 'translate':m[12]=px(r.x);m[13]=px(r.y);if(!r.is2D)m[14]=px(r.z);break;
        case 'rotate':return {m:r.is2D?geometry.rotation(0,0,1,degrees(r.angle)):geometry.rotation(scalar(r.x),scalar(r.y),scalar(r.z),degrees(r.angle)),is2D:r.is2D};
        case 'scale':m[0]=scalar(r.x);m[5]=scalar(r.y);if(!r.is2D)m[10]=scalar(r.z);break;
        case 'skew':m[4]=tan(degrees(r.ax)*PI/180);m[1]=tan(degrees(r.ay)*PI/180);break;
        case 'skewx':m[4]=tan(degrees(r.ax)*PI/180);break;
        case 'skewy':m[1]=tan(degrees(r.ay)*PI/180);break;
        case 'perspective':if(isNumeric(r.length)){const d=px(r.length);m[11]=-1/Math.max(d,1);}break;
        case 'matrix':{const values=geometry.matrixValues(r.matrix);
            if(r.is2D){const a=values.m;return {m:geometry.affine(a[0],a[1],a[4],a[5],a[12],a[13]),is2D:true};}
            return {m:values.m,is2D:false};}
        }
        return {m,is2D:r.is2D};
    }
    const component=v=>{own(v,COMPONENTS);return v;};
    class CSSTransformValue extends CSSStyleValue {
        constructor(transforms){required(arguments.length,1,'CSSTransformValue');super(token);
            const items=sequence(transforms,component);if(!items.length)throw new TypeErrorImpl('A transform list has at least one component');
            return indexed(this,{kind:'transform',items},listSetter(component));}
        get length(){return own(this,['transform']).items.length;}
        get is2D(){return own(this,['transform']).items.every(c=>rec(c).is2D);}
        toMatrix(){
            let m=geometry.identity(),is2D=true;
            for(const c of own(this,['transform']).items){const x=componentMatrix(c);m=geometry.multiply(m,x.m);is2D=is2D&&x.is2D;}
            return is2D?geometry.matrix2D(m):geometry.matrix(m);
        }
    }
    const transformValue=items=>indexed(create(CSSTransformValue.prototype),{kind:'transform',items},listSetter(component));

    /* ------------------------------------------------------------------ CSSImageValue (§4.5) */
    class CSSImageValue extends CSSStyleValue {}

    /* ------------------------------------------------------------------ colours (§4.6) */
    /* "rectify a CSSColorRGBComp / CSSColorPercent / CSSColorNumber / CSSColorAngle": a keyword is
       none only, a number a percentage (RGBComp: a number), and the value's type must be the channel's */
    /* (CSSNumberish or CSSKeywordish): a number stays one, a CSSNumericValue or CSSKeywordValue
       itself, anything else is a DOMString and so a keyword (Web IDL's union conversion) */
    function channel(v,kinds,numbersArePercent,anyKeyword){
        if(typeof v!=='number'&&!isNumeric(v)){
            v=keywordish(v);
            if(!anyKeyword&&rec(v).value.toLowerCase()!=='none')throw syntaxError("A colour channel's keyword is 'none'");
            return v;
        }
        if(typeof v==='number'){const n=double(v);v=numbersArePercent?unitValue(n*100,PERCENT):unitValue(n,NUMBER);}
        const r=numericRecord(v);
        if(!kinds.some(k=>native.typedMatch(r.type,k)===k&&(k!==CALC.percent||r.type[6]===1)))throw syntaxError('The colour channel has the wrong type');
        return v;
    }
    const RGB=v=>channel(v,[CALC.number,CALC.percent],true),PCT=v=>channel(v,[CALC.percent],true);
    const NUM=v=>channel(v,[CALC.number],false);
    /* an angle written as a number is in degrees */
    const HUE=v=>typeof v==='number'?unitValue(double(v),DEG):channel(v,[CALC.angle],false,true);
    /* CSSHWB's hue is a CSSNumericValue (anything else a TypeError), an <angle> */
    const HWB_HUE=v=>{if(!isNumeric(v))throw new TypeErrorImpl("CSSHWB's hue is a CSSNumericValue");return channel(v,[CALC.angle],false);};
    const NUMPCT=v=>channel(v,[CALC.number,CALC.percent],true);
    const COLOR_KINDS={
        rgb:[['r',RGB],['g',RGB],['b',RGB],['alpha',PCT]],hsl:[['h',HUE],['s',PCT],['l',PCT],['alpha',PCT]],
        hwb:[['h',HWB_HUE],['w',PCT],['b',PCT],['alpha',PCT]],
        lab:[['l',PCT],['a',NUM],['b',NUM],['alpha',PCT]],lch:[['l',PCT],['c',PCT],['h',HUE],['alpha',PCT]],
        oklab:[['l',PCT],['a',NUM],['b',NUM],['alpha',PCT]],oklch:[['l',PCT],['c',PCT],['h',HUE],['alpha',PCT]]};
    class CSSColorValue extends CSSStyleValue {}
    function colorClass(kind,name){
        const fields=COLOR_KINDS[kind];
        const C={[name]:class extends CSSColorValue {
            constructor(...args){required(args.length,3,name);super(token);
                const r={kind};for(let i=0;i<4;i++){const [field,rectify]=fields[i];r[field]=rectify(i===3&&args[3]===undefined?1:args[i]);}
                adopt(this,r);}
        }}[name];
        for(const [field,rectify] of fields)
            define(C.prototype,field,{configurable:true,enumerable:true,get:{[field](){return own(this,[kind])[field];}}[field],
                set:{[field](v){const r=own(this,[kind]);r[field]=rectify(v);}}[field]});
        return C;
    }
    const CSSRGB=colorClass('rgb','CSSRGB'),CSSHSL=colorClass('hsl','CSSHSL'),CSSHWB=colorClass('hwb','CSSHWB');
    const CSSLab=colorClass('lab','CSSLab'),CSSLCH=colorClass('lch','CSSLCH'),CSSOKLab=colorClass('oklab','CSSOKLab'),CSSOKLCH=colorClass('oklch','CSSOKLCH');
    class CSSColor extends CSSColorValue {
        constructor(colorSpace,channels,alpha=1){required(arguments.length,2,'CSSColor');super(token);
            adopt(this,{kind:'color',colorSpace:keywordish(colorSpace),channels:sequence(channels,NUMPCT),alpha:NUMPCT(alpha)});}
        get colorSpace(){return own(this,['color']).colorSpace;}
        set colorSpace(v){own(this,['color']).colorSpace=keywordish(v);}
        get channels(){return own(this,['color']).channels.slice();}
        set channels(v){const r=own(this,['color']);r.channels=sequence(v,NUMPCT);}
        get alpha(){return own(this,['color']).alpha;}
        set alpha(v){own(this,['color']).alpha=NUMPCT(v);}
    }
    function colorText(r){
        if(r.kind==='color')return 'color('+serialize(r.colorSpace)+' '+r.channels.map(serialize).join(' ')+' / '+serialize(r.alpha)+')';
        const [a,b,c,d]=COLOR_KINDS[r.kind].map(f=>serialize(r[f[0]]));
        return r.kind+'('+a+' '+b+' '+c+' / '+d+')';
    }
    const COLOR_CLASSES={rgb:CSSRGB,hsl:CSSHSL,hwb:CSSHWB,lab:CSSLab,lch:CSSLCH,oklab:CSSOKLab,oklch:CSSOKLCH};
    /* §5.7 "reify a color value", from color.c's reading of the colour as written */
    function reifyColor(text){
        const c=native.typedColor(text);
        if(!c)return null;
        if(c[0]==='keyword')return keyword(c[1]);
        if(c[0]==='other')return null;
        const [kind,space,...channels]=c;
        const values=channels.map((ch,i)=>{
            if(ch===null)return i===3?unitValue(100,PERCENT):unitValue(0,NUMBER);
            if(ch==='none')return keyword('none');
            if(ch[0]==='math'){const t=native.typedMath(ch[1]);return t?fromTree(t,false):null;}
            const [value,unit]=ch;
            if(i===3)return unit==='%'?unitValue(value,PERCENT):unitValue(value*100,PERCENT);
            if(unit==='')return (kind==='hsl'||kind==='hwb'||kind==='lch'||kind==='oklch')&&i===(kind==='hsl'||kind==='hwb'?0:2)?
                unitValue(value,DEG):unitValue(value,NUMBER);
            const u=units.get(unit==='%'?'percent':unit);return u===undefined?null:unitValue(value,u);
        });
        if(values.includes(null))return null;
        if(kind==='color')return make(CSSColor,{kind:'color',colorSpace:keyword(space),channels:values.slice(0,3),alpha:values[3]});
        const r={kind};COLOR_KINDS[kind].forEach(([field],i)=>r[field]=values[i]);
        return make(COLOR_CLASSES[kind],r);
    }

    /* ------------------------------------------------------------------ reification (§5) */
    const REIFY={value:0,style:1,color:2,transform:3,image:4};
    const WIDE=['initial','inherit','unset','revert','revert-layer'];
    const propertyInfo=new Map();
    function info(name){
        let i=propertyInfo.get(name);
        if(i===undefined){const r=native.typedProperty(name);i=r&&{kind:r[0],shorthand:r[1],list:r[2],accept:r[3],typed:r[4],integer:r[5]};propertyInfo.set(name,i);}
        return i;
    }
    /* the property name as the maps and parse() take it: a custom property as written, else ASCII
       lowercase, and it must be a property this engine has */
    function propertyName(v){
        v=usv(v);
        if(v.startsWith('--'))return v;
        v=v.toLowerCase();if(!info(v))throw new TypeErrorImpl("'"+v+"' is not a CSS property");return v;
    }
    /* a math tree from js_typed_om.c as objects; computed: absolute units in their canonical one */
    function fromTree(t,computed){
        if(typeof t[0]==='number'){
            let [value,unit]=t;
            const base=unitRows[unit][1];
            if(computed&&base>=0&&base<6&&!unitRows[unit][2]){value=convertValue(value,unit,canonicalOf[base]);unit=canonicalOf[base];}
            return unitValue(value,unit);
        }
        const items=t.slice(1).map(x=>fromTree(x,computed));
        switch(t[0]){
        case 'sum':return mathValue(CSSMathSum,'sum',items);
        case 'product':return mathValue(CSSMathProduct,'product',items);
        case 'min':return mathValue(CSSMathMin,'min',items);
        case 'max':return mathValue(CSSMathMax,'max',items);
        case 'clamp':return mathValue(CSSMathClamp,'clamp',items);
        case 'negate':return make(CSSMathNegate,{kind:'negate',value:items[0],type:numericRecord(items[0]).type});
        case 'invert':return make(CSSMathInvert,{kind:'invert',value:items[0],type:invertType(numericRecord(items[0]).type)});
        }
        throw new TypeErrorImpl('Unknown math node');
    }
    /* §5.6 "reify a numeric value" in a context that accepts accept: null if it is not one there */
    function reifyNumeric(text,accept,computed,zeroUnit){
        const t=native.typedMath(text,accept||CALC.any);
        if(!t)return null;
        let v;
        try{v=fromTree(t,computed);}catch(e){return null;}
        const r=numericRecord(v);
        /* a unitless zero where the grammar takes a dimension and no number */
        if(r.kind==='unit'&&r.unit===NUMBER&&r.value===0&&zeroUnit!==undefined&&!(accept&CALC.number))return unitValue(0,zeroUnit);
        return accept&&native.typedMatch(r.type,accept)?v:null;
    }
    function zeroUnitOf(accept){return accept&CALC.length?PX:accept&CALC.angle?DEG:undefined;}
    /* §5.3 "reify a list of component values" */
    function segmentsValue(list){return unparsed(list.map(x=>typeof x==='string'?x:varref(x[0],x[1]===null?null:segmentsValue(x[1]))));}
    function reifyUnparsed(text){return segmentsValue(native.typedSegments(text));}
    const IMAGE_FUNCTIONS=['url','src','image','image-set','-webkit-image-set','cross-fade','element','paint','linear-gradient',
        'radial-gradient','conic-gradient','repeating-linear-gradient','repeating-radial-gradient','repeating-conic-gradient',
        '-webkit-linear-gradient','-webkit-radial-gradient','-webkit-repeating-linear-gradient','-webkit-repeating-radial-gradient'];
    /* §5.8: a <transform-list> as components, or null when one has no object */
    function reifyTransform(parts,computed){
        const items=[];
        const len=x=>reifyNumeric(x,CALC.length|CALC.percent,computed,PX),zlen=x=>reifyNumeric(x,CALC.length,computed,PX);
        const ang=x=>reifyNumeric(x,CALC.angle,computed,DEG),num=x=>reifyNumeric(x,CALC.number,computed);
        const n0=()=>unitValue(0,NUMBER),n1=()=>unitValue(1,NUMBER),p0=()=>unitValue(0,PX),d0=()=>unitValue(0,DEG);
        for(const part of parts){
            if(part[0]!=='function')return null;
            const name=part[1],a=part[3];let r=null;
            const all=(f,n,m=n)=>a.length>=n&&a.length<=m&&a.every(x=>f(x)!==null);
            switch(name){
            case 'translate':if(all(len,1,2))r={kind:'translate',x:len(a[0]),y:a[1]?len(a[1]):p0(),z:p0(),is2D:true};break;
            case 'translatex':if(all(len,1))r={kind:'translate',x:len(a[0]),y:p0(),z:p0(),is2D:true};break;
            case 'translatey':if(all(len,1))r={kind:'translate',x:p0(),y:len(a[0]),z:p0(),is2D:true};break;
            case 'translatez':if(all(zlen,1))r={kind:'translate',x:p0(),y:p0(),z:zlen(a[0]),is2D:false};break;
            case 'translate3d':if(a.length===3&&len(a[0])&&len(a[1])&&zlen(a[2]))r={kind:'translate',x:len(a[0]),y:len(a[1]),z:zlen(a[2]),is2D:false};break;
            case 'scale':if(all(num,1,2))r={kind:'scale',x:num(a[0]),y:num(a[a.length-1]),z:n1(),is2D:true};break;
            case 'scalex':if(all(num,1))r={kind:'scale',x:num(a[0]),y:n1(),z:n1(),is2D:true};break;
            case 'scaley':if(all(num,1))r={kind:'scale',x:n1(),y:num(a[0]),z:n1(),is2D:true};break;
            case 'scalez':if(all(num,1))r={kind:'scale',x:n1(),y:n1(),z:num(a[0]),is2D:false};break;
            case 'scale3d':if(all(num,3))r={kind:'scale',x:num(a[0]),y:num(a[1]),z:num(a[2]),is2D:false};break;
            case 'rotate':if(all(ang,1))r={kind:'rotate',x:n0(),y:n0(),z:n1(),angle:ang(a[0]),is2D:true};break;
            case 'rotatex':if(all(ang,1))r={kind:'rotate',x:n1(),y:n0(),z:n0(),angle:ang(a[0]),is2D:false};break;
            case 'rotatey':if(all(ang,1))r={kind:'rotate',x:n0(),y:n1(),z:n0(),angle:ang(a[0]),is2D:false};break;
            case 'rotatez':if(all(ang,1))r={kind:'rotate',x:n0(),y:n0(),z:n1(),angle:ang(a[0]),is2D:false};break;
            case 'rotate3d':if(a.length===4&&num(a[0])&&num(a[1])&&num(a[2])&&ang(a[3]))r={kind:'rotate',x:num(a[0]),y:num(a[1]),z:num(a[2]),angle:ang(a[3]),is2D:false};break;
            case 'skew':if(all(ang,1,2))r={kind:'skew',ax:ang(a[0]),ay:a[1]?ang(a[1]):d0(),is2D:true};break;
            case 'skewx':if(all(ang,1))r={kind:'skewx',ax:ang(a[0]),is2D:true};break;
            case 'skewy':if(all(ang,1))r={kind:'skewy',ay:ang(a[0]),is2D:true};break;
            case 'perspective':if(a.length===1)r=/^none$/i.test(a[0])?{kind:'perspective',length:keyword('none'),is2D:false}:
                zlen(a[0])?{kind:'perspective',length:zlen(a[0]),is2D:false}:null;break;
            case 'matrix':case 'matrix3d':{
                const want=name==='matrix'?6:16,values=a.map(x=>{const v=num(x);return v&&numericRecord(v).kind==='unit'?numericRecord(v).value:null;});
                if(values.length===want&&!values.includes(null))r={kind:'matrix',matrix:want===6?geometry.matrix2D(geometry.affine(...values)):geometry.matrix(values),is2D:want===6};
                break;}
            }
            if(!r)return null;
            const C={translate:CSSTranslate,scale:CSSScale,rotate:CSSRotate,skew:CSSSkew,skewx:CSSSkewX,skewy:CSSSkewY,perspective:CSSPerspective,matrix:CSSMatrixComponent}[r.kind];
            items.push(make(C,r));
        }
        return items.length?transformValue(items):null;
    }
    /* one item of a property's value (a list-valued property's are split at top-level commas) */
    function reifyItem(name,i,text,computed){
        const parts=native.typedComponents(text),single=parts.length===1?parts[0]:null;
        if(i.kind===REIFY.style)return styleValue(text,name);
        if(single&&single[0]==='ident'&&single[1].toLowerCase()==='none'&&i.kind!==REIFY.value&&i.kind!==REIFY.color)return keyword(single[1]);
        switch(i.kind){
        case REIFY.color:
            /* §5.1: currentcolor is a keyword; a colour is not reified in Level 1; other keywords
               (none, context-fill) are keywords */
            if(single&&single[0]==='ident'&&(single[1].toLowerCase()==='currentcolor'||!native.typedColor(text)))return keyword(single[1]);
            return styleValue(text,name);
        case REIFY.transform:return reifyTransform(parts,computed)||styleValue(text,name);
        case REIFY.image:
            if(single&&(single[0]==='url'||(single[0]==='function'&&IMAGE_FUNCTIONS.includes(single[1]))))
                return make(CSSImageValue,{kind:'image',text,property:name});
            /* a keyword the grammar has beside its images (shape-outside's boxes) */
            if(single&&single[0]==='ident')return keyword(single[1]);
            return styleValue(text,name);
        }
        if(single&&single[0]==='ident')return keyword(single[1]);
        if(single&&(single[0]==='numeric'||single[0]==='function')){
            const v=reifyNumeric(text,i.accept,computed,zeroUnitOf(i.accept));
            if(v)return v;
        }
        return styleValue(text,name);
    }
    /* the top-level comma-separated items of a value */
    function splitList(text){
        if(!native.typedComponents(text).some(p=>p[0]===','))return [text];
        /* the items keep their source text: as a function's arguments, js_typed_om.c cuts them at
           the top-level commas */
        const parts=native.typedComponents('x('+text+')');
        return parts.length===1&&parts[0][0]==='function'?parts[0][3]:[text];
    }
    /* "reify" a declared or computed value of name: the list of its items */
    function reifyValue(name,text,computed){
        text=text.trim();
        if(name.startsWith('--'))return [reifyUnparsed(text)];
        const lower=text.toLowerCase();
        if(WIDE.includes(lower))return [keyword(lower)];
        const i=info(name);
        if(/var\(/i.test(text)){const u=reifyUnparsed(text);if(u&&rec(u).items.some(x=>typeof x!=='string'))return [u];}
        if(!i||i.shorthand)return [styleValue(text,name)];
        return (i.list?splitList(text):[text]).map(item=>reifyItem(name,i,item,computed));
    }

    /* ------------------------------------------------------------------ "create an internal representation" (§3.5) */
    /* the text a value is set as on property, or a TypeError when the property's grammar refuses it */
    function internal(name,value){
        const r=rec(value),custom=name.startsWith('--');
        if(!r){
            const text=usv(value);
            if(!native.typedValid(name,text)||(custom&&!text.trim()))throw new TypeErrorImpl("'"+text+"' is not a valid value for "+name);
            return text;
        }
        /* a CSSStyleValue stands for its property's value; a CSSImageValue is an <image> any
           property that reifies images takes */
        if(r.kind==='style'){
            if(r.property!==name)throw new TypeErrorImpl('The value belongs to '+r.property);
            return r.text;
        }
        if(r.kind==='image'){
            const i=custom?null:info(name);
            if(!i||i.kind!==REIFY.image)throw new TypeErrorImpl(name+' does not take an image');
            return r.text;
        }
        if(!(value instanceof CSSStyleValue))throw new TypeErrorImpl('Expected a CSSStyleValue');
        if(custom&&r.kind!=='unparsed')throw new TypeErrorImpl('A custom property takes a CSSUnparsedValue');
        const i=custom?null:info(name);
        /* a shorthand reifies as a CSSStyleValue (§5.1): no subclass but a CSS-wide keyword and var()
           references matches it */
        if(i&&i.shorthand&&r.kind!=='unparsed'&&!(r.kind==='keyword'&&WIDE.includes(r.value.toLowerCase())))
            throw new TypeErrorImpl(name+' is a shorthand: set it from a string or a value it reified');
        /* a numeric value must have a type the grammar takes */
        if(NUMERIC.includes(r.kind)&&i&&i.typed&&!(i.accept&&native.typedMatch(r.type,i.accept)))
            throw new TypeErrorImpl("A value of this type is not valid for "+name);
        /* a CSSTransformValue is a <transform-list>: only a property that reifies one takes it */
        if(r.kind==='transform'&&!(i&&i.kind===REIFY.transform))throw new TypeErrorImpl(name+' does not take a transform');
        const text=serialize(value);
        /* a CSSUnitValue outside the grammar's range (an integer's fraction, a negative value where
           none is allowed) is set as a CSSMathSum of it, calc() (§3.5) */
        const range=r.kind==='unit'&&i&&i.integer&&r.unit===NUMBER&&!Number.isInteger(r.value);
        if(!range&&native.typedValid(name,text)&&(!custom||text.trim()))return text;
        if(r.kind==='unit'){const wrapped='calc('+text+')';if(native.typedValid(name,wrapped))return wrapped;}
        throw new TypeErrorImpl("'"+text+"' is not a valid value for "+name);
    }

    /* ------------------------------------------------------------------ StylePropertyMap (§3) */
    /* A map is a declared block (inline style or a rule's: its CSSStyleDeclaration) or an element's
       computed values. */
    class StylePropertyMapReadOnly {
        constructor(key){if(key!==token)throw new TypeErrorImpl('Illegal constructor');}
        get(property){required(arguments.length,1,'get');const r=own(this,['declared','computed']);const name=propertyName(property);
            const values=mapValues(r,name);return values?values[0]:undefined;}
        getAll(property){required(arguments.length,1,'getAll');const r=own(this,['declared','computed']);return mapValues(r,propertyName(property))||[];}
        has(property){required(arguments.length,1,'has');const r=own(this,['declared','computed']);return !!mapValues(r,propertyName(property));}
        get size(){return mapNames(own(this,['declared','computed'])).length;}
        *entries(){const r=own(this,['declared','computed']);for(const name of mapNames(r)){const v=mapValues(r,name);if(v)yield [name,v];}}
        *keys(){for(const [name] of this.entries())yield name;}
        *values(){for(const [,v] of this.entries())yield v;}
        forEach(callback,thisArg=undefined){
            if(typeof callback!=='function')throw new TypeErrorImpl('forEach requires a function');
            for(const [name,v] of this.entries())call(callback,thisArg,[v,name,this]);
        }
    }
    class StylePropertyMap extends StylePropertyMapReadOnly {
        set(property,...values){required(arguments.length,1,'set');const r=own(this,['declared']);const name=propertyName(property);
            const i=name.startsWith('--')?null:info(name);
            if(values.length>1&&!(i&&i.list))throw new TypeErrorImpl(name+' is not list-valued');
            if(values.length>1&&values.some(unparsedOrReference))throw new TypeErrorImpl('A var() reference is the whole value');
            const text=values.map(v=>internal(name,v)).join(', ');
            declarations.write(r.block,name,text);}
        append(property,...values){required(arguments.length,1,'append');const r=own(this,['declared']);const name=propertyName(property);
            const i=name.startsWith('--')?null:info(name);
            if(!i||!i.list)throw new TypeErrorImpl(name+' is not list-valued');
            if(values.some(unparsedOrReference))throw new TypeErrorImpl('A var() reference cannot be appended');
            const old=declarations.read(r.block,name);
            if(old&&rec(reifyUnparsed(old)).items.some(x=>typeof x!=='string'))throw new TypeErrorImpl(name+' holds a var() reference');
            if(!values.length)return;
            const added=values.map(v=>internal(name,v)).join(', ');
            declarations.write(r.block,name,old?old+', '+added:added);}
        delete(property){required(arguments.length,1,'delete');const r=own(this,['declared']);declarations.write(r.block,propertyName(property),'');}
        clear(){const r=own(this,['declared']);for(const name of declarations.names(r.block).slice())declarations.write(r.block,name,'');}
    }
    /* a CSSUnparsedValue, a CSSVariableReferenceValue or a string with var() in it (§3.3 set, append) */
    function unparsedOrReference(v){
        const r=rec(v);
        if(r)return r.kind==='unparsed'||r.kind==='varref';
        return typeof v==='string'&&rec(reifyUnparsed(v)).items.some(x=>typeof x!=='string');
    }
    function mapValues(r,name){
        const text=r.kind==='declared'?declarations.read(r.block,name):declarations.computed(r.element,name);
        if(text===''&&(r.kind==='declared'||name.startsWith('--')))return null;
        return reifyValue(name,text,r.kind==='computed');
    }
    function mapNames(r){return r.kind==='declared'?declarations.names(r.block):declarations.computedNames(r.element);}
    const declaredMaps=new WeakMap(),computedMaps=new WeakMap();
    /* [SameObject] maps: an inline style's or a rule's over its CSSStyleDeclaration, an element's computed one */
    function declaredMap(block){let m=call(recordOf,declaredMaps,[block]);if(!m){m=make(StylePropertyMap,{kind:'declared',block});call(recordSet,declaredMaps,[block,m]);}return m;}
    function computedMap(element){let m=call(recordOf,computedMaps,[element]);if(!m){m=make(StylePropertyMapReadOnly,{kind:'computed',element});call(recordSet,computedMaps,[element,m]);}return m;}

    /* ------------------------------------------------------------------ parse() (§3.2, §4.3, §4.6) */
    function parseStyle(property,cssText,all){
        const name=propertyName(property),text=usv(cssText);
        if(!native.typedValid(name,text)||(name.startsWith('--')&&!text.trim()))throw new TypeErrorImpl("'"+text+"' is not a valid value for "+name);
        const values=reifyValue(name,text,false);
        return all?values:values[0];
    }
    if(windowRealm){
        define(CSSStyleValue,'parse',{configurable:true,writable:true,value:{parse(property,cssText){required(arguments.length,2,'parse');return parseStyle(property,cssText,false);}}.parse});
        define(CSSStyleValue,'parseAll',{configurable:true,writable:true,value:{parseAll(property,cssText){required(arguments.length,2,'parseAll');return parseStyle(property,cssText,true);}}.parseAll});
        define(CSSNumericValue,'parse',{configurable:true,writable:true,value:{parse(cssText){required(arguments.length,1,'parse');
            const t=native.typedMath(usv(cssText));if(!t)throw syntaxError("'"+usv(cssText)+"' is not a numeric value Typed OM can represent");
            return fromTree(t,false);}}.parse});
        define(CSSColorValue,'parse',{configurable:true,writable:true,value:{parse(cssText){required(arguments.length,1,'parse');
            const text=usv(cssText),c=native.typedColor(text);
            if(!c)throw syntaxError("'"+text+"' is not a <color>");
            return reifyColor(text)||styleValue(text.trim(),'color');}}.parse});
    }
    /* CSS.number(), CSS.px() and the other factories (§4.3.5) */
    if(options.css)for(const name of ['number','percent','cap','ch','em','ex','ic','lh','rcap','rch','rem','rex','ric','rlh',
        'vw','vh','vi','vb','vmin','vmax','svw','svh','svi','svb','svmin','svmax','lvw','lvh','lvi','lvb','lvmin','lvmax',
        'dvw','dvh','dvi','dvb','dvmin','dvmax','cqw','cqh','cqi','cqb','cqmin','cqmax','cm','mm','Q','in','pt','pc','px',
        'deg','grad','rad','turn','s','ms','Hz','kHz','dpi','dpcm','dppx','fr']){
        const unit=units.get(name.toLowerCase());
        if(unit===undefined)continue;
        define(options.css,name,{configurable:true,enumerable:true,writable:true,
            value:{[name](value){required(arguments.length,1,name);return unitValue(double(value),unit);}}[name]});
    }

    const exports={CSSStyleValue,CSSKeywordValue,CSSUnparsedValue,CSSVariableReferenceValue,CSSNumericValue,CSSUnitValue,
        CSSMathValue,CSSMathSum,CSSMathProduct,CSSMathNegate,CSSMathInvert,CSSMathMin,CSSMathMax,CSSMathClamp,CSSNumericArray,
        CSSTransformValue,CSSTransformComponent,CSSTranslate,CSSRotate,CSSScale,CSSSkew,CSSSkewX,CSSSkewY,CSSPerspective,
        CSSMatrixComponent,CSSImageValue,CSSColorValue,CSSRGB,CSSHSL,CSSHWB,CSSLab,CSSLCH,CSSOKLab,CSSOKLCH,CSSColor,
        StylePropertyMapReadOnly};
    if(windowRealm)exports.StylePropertyMap=StylePropertyMap;
    for(const name of Object.keys(exports)){
        define(exports[name].prototype,Symbol.toStringTag,{configurable:true,value:name});
        define(globalThis,name,{configurable:true,writable:true,value:exports[name]});
    }
    return {declaredMap,computedMap};
}
