/* Geometry Interfaces (https://drafts.csswg.org/geometry/): DOMPoint, DOMRect, DOMQuad and
   DOMMatrix with their read-only bases, one implementation for the Window and worker realms.
   Values live in private records, never in author-visible properties; a matrix keeps the
   spec's "is 2D" flag instead of reading 2D-ness back from its values.
   options.parse: the Window's "parse a string into an abstract matrix" (the native CSS
   transform parser: [16 column-major values, is2D] or null); without it, as in a worker,
   a string init throws TypeError. options.list: the Window's indexed collection factory;
   with it come the Window-only members (DOMRectList, the stringifier, setMatrixValue) and
   the legacy window aliases. */
function geometryInterfaces(options){
    const parse=options.parse,windowRealm=typeof options.list==='function';
    const records=new WeakMap(),mutable=new WeakSet();
    const TypeErrorImpl=TypeError,get=WeakMap.prototype.get,set=WeakMap.prototype.set,has=WeakSet.prototype.has,add=WeakSet.prototype.add;
    const call=Reflect.apply,define=Object.defineProperty,finite=Number.isFinite,isNaNImpl=Number.isNaN,pi=Math.PI;
    const sin=Math.sin,cos=Math.cos,tan=Math.tan,atan2=Math.atan2,sqrt=Math.sqrt,min=Math.min,max=Math.max;
    const iterator=Symbol.iterator,typedTag=Object.getOwnPropertyDescriptor(Object.getPrototypeOf(Float32Array.prototype),Symbol.toStringTag).get;
    function record(value,kind){const r=call(get,records,[value]);if(!r||r.kind!==kind)throw new TypeErrorImpl('Illegal '+kind+' receiver');return r;}
    function writable(value,kind){const r=record(value,kind);if(!call(has,mutable,[value]))throw new TypeErrorImpl('Illegal mutable '+kind+' receiver');return r;}
    function adopt(object,r,isMutable){call(set,records,[object,r]);if(isMutable)call(add,mutable,[object]);return object;}
    const number=v=>+v; /* unrestricted double: ToNumber, a Symbol or BigInt throws */
    /* A Web IDL dictionary: undefined or null is empty, any other non-object is a TypeError.
       Members are read once each, in lexicographic order. */
    function dictionary(v,name){if(v===undefined||v===null)return {};if(typeof v!=='object'&&typeof v!=='function')throw new TypeErrorImpl('Expected '+name+' dictionary');return v;}
    function member(d,key,fallback){const v=d[key];return v===undefined?fallback:number(v);}
    function sameValueZero(a,b){return a===b||(isNaNImpl(a)&&isNaNImpl(b));}

    /* ------------------------------------------------------------------ DOMPoint */
    function pointInit(v){v=dictionary(v,'DOMPointInit');const w=member(v,'w',1),x=member(v,'x',0),y=member(v,'y',0),z=member(v,'z',0);return [x,y,z,w];}
    function point(values,isMutable){return adopt(isMutable?Object.create(DOMPoint.prototype):Object.create(DOMPointReadOnly.prototype),{kind:'DOMPoint',v:values},isMutable);}
    class DOMPointReadOnly {
        constructor(x=0,y=0,z=0,w=1){adopt(this,{kind:'DOMPoint',v:[number(x),number(y),number(z),number(w)]},false);}
        static fromPoint(other={}){return point(pointInit(other),false);}
        get x(){return record(this,'DOMPoint').v[0];}get y(){return record(this,'DOMPoint').v[1];}
        get z(){return record(this,'DOMPoint').v[2];}get w(){return record(this,'DOMPoint').v[3];}
        matrixTransform(matrix={}){const p=record(this,'DOMPoint').v;return point(transformPoint(matrixDictionary(matrix).m,p),true);}
        toJSON(){const [x,y,z,w]=record(this,'DOMPoint').v;return {x,y,z,w};}
    }
    class DOMPoint extends DOMPointReadOnly {
        constructor(x=0,y=0,z=0,w=1){super(x,y,z,w);call(add,mutable,[this]);}
        static fromPoint(other={}){return point(pointInit(other),true);}
    }
    ['x','y','z','w'].forEach((key,i)=>define(DOMPoint.prototype,key,{configurable:true,enumerable:true,
        get(){return record(this,'DOMPoint').v[i];},set(v){writable(this,'DOMPoint').v[i]=number(v);}}));

    /* ------------------------------------------------------------------- DOMRect */
    function rectInit(v){v=dictionary(v,'DOMRectInit');const height=member(v,'height',0),width=member(v,'width',0),x=member(v,'x',0),y=member(v,'y',0);return [x,y,width,height];}
    function rect(values,isMutable){return adopt(Object.create((isMutable?DOMRect:DOMRectReadOnly).prototype),{kind:'DOMRect',v:values},isMutable);}
    /* top/right/bottom/left are NaN-safe minimums and maximums: any NaN makes NaN. */
    const rectSide=(a,b,pick)=>pick(a,a+b);
    class DOMRectReadOnly {
        constructor(x=0,y=0,width=0,height=0){adopt(this,{kind:'DOMRect',v:[number(x),number(y),number(width),number(height)]},false);}
        static fromRect(other={}){return rect(rectInit(other),false);}
        get x(){return record(this,'DOMRect').v[0];}get y(){return record(this,'DOMRect').v[1];}
        get width(){return record(this,'DOMRect').v[2];}get height(){return record(this,'DOMRect').v[3];}
        get top(){const v=record(this,'DOMRect').v;return rectSide(v[1],v[3],min);}
        get right(){const v=record(this,'DOMRect').v;return rectSide(v[0],v[2],max);}
        get bottom(){const v=record(this,'DOMRect').v;return rectSide(v[1],v[3],max);}
        get left(){const v=record(this,'DOMRect').v;return rectSide(v[0],v[2],min);}
        toJSON(){const v=record(this,'DOMRect').v,[x,y,width,height]=v;
            return {x,y,width,height,top:rectSide(y,height,min),right:rectSide(x,width,max),bottom:rectSide(y,height,max),left:rectSide(x,width,min)};}
    }
    class DOMRect extends DOMRectReadOnly {
        constructor(x=0,y=0,width=0,height=0){super(x,y,width,height);call(add,mutable,[this]);}
        static fromRect(other={}){return rect(rectInit(other),true);}
    }
    ['x','y','width','height'].forEach((key,i)=>define(DOMRect.prototype,key,{configurable:true,enumerable:true,
        get(){return record(this,'DOMRect').v[i];},set(v){writable(this,'DOMRect').v[i]=number(v);}}));
    /* A rect whose values another owner keeps (SVG's live viewBox): v reads and writes through. */
    function liveRect(read,write){
        const values=[];
        for(let i=0;i<4;i++)define(values,i,{get(){return read()[i];},...(write?{set(v){write(i,v);}}:{}),configurable:true});
        return rect(values,!!write);
    }

    /* ------------------------------------------------------------------- DOMQuad */
    function quad(points){return adopt(Object.create(DOMQuad.prototype),{kind:'DOMQuad',v:points.map(p=>point(p,true))},false);}
    function quadInit(v){v=dictionary(v,'DOMQuadInit');return ['p1','p2','p3','p4'].map(key=>pointInit(v[key]));}
    class DOMQuad {
        constructor(p1={},p2={},p3={},p4={}){const a=pointInit(p1),b=pointInit(p2),c=pointInit(p3),d=pointInit(p4);
            adopt(this,{kind:'DOMQuad',v:[a,b,c,d].map(p=>point(p,true))},false);}
        static fromRect(other={}){const [x,y,w,h]=rectInit(other);return quad([[x,y,0,1],[x+w,y,0,1],[x+w,y+h,0,1],[x,y+h,0,1]]);}
        static fromQuad(other={}){return quad(quadInit(other));}
        get p1(){return record(this,'DOMQuad').v[0];}get p2(){return record(this,'DOMQuad').v[1];}
        get p3(){return record(this,'DOMQuad').v[2];}get p4(){return record(this,'DOMQuad').v[3];}
        getBounds(){
            const ps=record(this,'DOMQuad').v.map(p=>record(p,'DOMPoint').v),xs=ps.map(p=>p[0]),ys=ps.map(p=>p[1]);
            const left=min(...xs),top=min(...ys),right=max(...xs),bottom=max(...ys);
            return rect([left,top,right-left,bottom-top],true);
        }
        toJSON(){const v=record(this,'DOMQuad').v;return {p1:v[0],p2:v[1],p3:v[2],p4:v[3]};}
    }

    /* ----------------------------------------------------------------- DOMMatrix */
    /* m is column-major: m[(column-1)*4+(row-1)] is m<column><row>, so m41/m42 translate. */
    function identity(){return [1,0,0,0,0,1,0,0,0,0,1,0,0,0,0,1];}
    function affine(a=1,b=0,c=0,d=1,e=0,f=0){return [a,b,0,0,c,d,0,0,0,0,1,0,e,f,0,1];}
    const index=key=>(key.charCodeAt(1)-49)*4+key.charCodeAt(2)-49;
    const names2D=['m11','m12','m21','m22','m41','m42'],aliases=['a','b','c','d','e','f'];
    const names3D=['m13','m14','m23','m24','m31','m32','m34','m43'];
    /* Post-multiply: a·b, the matrix that applies b first. */
    function multiply(a,b){
        const r=Array(16).fill(0);
        for(let c=0;c<4;c++)for(let row=0;row<4;row++){let s=0;for(let k=0;k<4;k++)s+=a[k*4+row]*b[c*4+k];r[c*4+row]=s;}
        return r;
    }
    function transformPoint(m,p){const r=[0,0,0,0];for(let row=0;row<4;row++)r[row]=m[row]*p[0]+m[4+row]*p[1]+m[8+row]*p[2]+m[12+row]*p[3];return r;}
    /* The adjugate over the determinant, from the 2x2 minors of the top and bottom row pairs: no
       pivoting, so a matrix times its inverse rounds to the exact identity where elimination leaves
       1e-17 residues (and isIdentity false). */
    function inverse(m){
        const [a00,a01,a02,a03,a10,a11,a12,a13,a20,a21,a22,a23,a30,a31,a32,a33]=m;
        const b00=a00*a11-a01*a10,b01=a00*a12-a02*a10,b02=a00*a13-a03*a10,b03=a01*a12-a02*a11,b04=a01*a13-a03*a11,b05=a02*a13-a03*a12,
            b06=a20*a31-a21*a30,b07=a20*a32-a22*a30,b08=a20*a33-a23*a30,b09=a21*a32-a22*a31,b10=a21*a33-a23*a31,b11=a22*a33-a23*a32;
        const det=b00*b11-b01*b10+b02*b09+b03*b08-b04*b07+b05*b06;
        if(det===0||!finite(det))return null;
        const i=1/det;
        const r=[(a11*b11-a12*b10+a13*b09)*i,(a02*b10-a01*b11-a03*b09)*i,(a31*b05-a32*b04+a33*b03)*i,(a22*b04-a21*b05-a23*b03)*i,
            (a12*b08-a10*b11-a13*b07)*i,(a00*b11-a02*b08+a03*b07)*i,(a32*b02-a30*b05-a33*b01)*i,(a20*b05-a22*b02+a23*b01)*i,
            (a10*b10-a11*b08+a13*b06)*i,(a01*b08-a00*b10-a03*b06)*i,(a30*b04-a31*b02+a33*b00)*i,(a21*b02-a20*b04-a23*b00)*i,
            (a11*b07-a10*b09-a12*b06)*i,(a00*b09-a01*b07+a02*b06)*i,(a31*b01-a30*b03-a32*b00)*i,(a20*b03-a21*b01+a22*b00)*i];
        return r.every(finite)?r:null;
    }
    function inverse2D(m){
        const [a,b,c,d,e,f]=names2D.map(k=>m[index(k)]),det=a*d-b*c;
        if(det===0||!finite(det))return null;
        const r=affine(d/det,-b/det,-c/det,a/det,(c*f-d*e)/det,(b*e-a*f)/det);
        return r.every(finite)?r:null;
    }
    /* CSS Transforms 2 rotate3d(): the axis is normalized; a zero axis is no rotation. */
    function rotation(x,y,z,degrees){
        const length=sqrt(x*x+y*y+z*z),m=identity();
        if(!(length>0)||!finite(length))return m;
        x/=length;y/=length;z/=length;
        const half=degrees*pi/360,sc=sin(half)*cos(half),sq=sin(half)*sin(half);
        m[0]=1-2*(y*y+z*z)*sq;m[1]=2*(x*y*sq+z*sc);m[2]=2*(x*z*sq-y*sc);
        m[4]=2*(x*y*sq-z*sc);m[5]=1-2*(x*x+z*z)*sq;m[6]=2*(y*z*sq+x*sc);
        m[8]=2*(x*z*sq+y*sc);m[9]=2*(y*z*sq-x*sc);m[10]=1-2*(x*x+y*y)*sq;
        return m;
    }
    /* Validate and fixup (2D): an alias and its long name must agree; absent long names take
       the alias or the identity's value. Returns the six 2D values. */
    function fixup2D(d){
        const out=[];
        for(let i=0;i<6;i++){
            const alias=d[aliases[i]],long=d[names2D[i]];
            if(alias!==undefined&&long!==undefined&&!sameValueZero(alias,long))throw new TypeErrorImpl('Inconsistent '+aliases[i]+' and '+names2D[i]);
            out.push(long!==undefined?long:alias!==undefined?alias:(i===0||i===3?1:0));
        }
        return out;
    }
    /* DOMMatrix2DInit, converted in lexicographic member order. */
    function init2D(v){
        v=dictionary(v,'DOMMatrix2DInit');const d={};
        for(const key of ['a','b','c','d','e','f','m11','m12','m21','m22','m41','m42']){const x=v[key];if(x!==undefined)d[key]=number(x);}
        return d;
    }
    function matrix2DDictionary(v){return affine(...fixup2D(init2D(v)));}
    /* Create a DOMMatrix from the dictionary: validate and fixup, then 2D or 3D. */
    function matrixDictionary(v){
        const d=init2D(v),source=dictionary(v,'DOMMatrixInit');
        const is2DValue=source.is2D,is2D=is2DValue===undefined?undefined:!!is2DValue;
        const defaults={m13:0,m14:0,m23:0,m24:0,m31:0,m32:0,m33:1,m34:0,m43:0,m44:1};
        for(const key of ['m13','m14','m23','m24','m31','m32','m33','m34','m43','m44'])d[key]=member(source,key,defaults[key]);
        const six=fixup2D(d);
        const nonIdentity3D=names3D.some(k=>d[k]!==0)||d.m33!==1||d.m44!==1;
        if(is2D===true&&nonIdentity3D)throw new TypeErrorImpl('A matrix with is2D true cannot have 3D values');
        const flat=is2D===undefined?!nonIdentity3D:is2D;
        if(flat)return {m:affine(...six),is2D:true};
        const m=identity();six.forEach((value,i)=>m[index(names2D[i])]=value);for(const key of [...names3D,'m33','m44'])m[index(key)]=d[key];
        return {m,is2D:false};
    }
    function matrixObject(type,m,is2D){return adopt(Object.create(type.prototype),{kind:'DOMMatrix',m,is2D},type===DOMMatrix);}
    function from6(type,v){return matrixObject(type,affine(...v),true);}
    function from16(type,v){return matrixObject(type,v.slice(),false);}
    function parsed(text){
        const r=parse(text);if(!r)throw new DOMException('Invalid transform list','SyntaxError');
        return r[1]?{m:affine(...names2D.map(k=>r[0][index(k)])),is2D:true}:{m:r[0].slice(0,16),is2D:false};
    }
    /* The (DOMString or sequence<unrestricted double>) init of a constructor. */
    function constructed(init){
        if(init===undefined)return {m:identity(),is2D:true};
        if((typeof init==='object'&&init!==null)||typeof init==='function'){
            const method=init[iterator];
            if(method!==undefined&&method!==null){
                if(typeof method!=='function')throw new TypeErrorImpl('Matrix sequence is not iterable');
                const values=[];for(const value of {[iterator]:()=>call(method,init,[])})values.push(number(value));
                if(values.length===6)return {m:affine(...values),is2D:true};
                if(values.length===16)return {m:values,is2D:false};
                throw new TypeErrorImpl('A matrix sequence needs 6 or 16 values');
            }
        }
        const text=String(init);
        if(!parse)throw new TypeErrorImpl('A matrix string init needs a Window');
        return parsed(text);
    }
    function typedArray(v,name){
        if(typeof v!=='object'||v===null||call(typedTag,v,[])!==name)throw new TypeErrorImpl('Expected '+name);
        const values=Array.from(v,x=>+x);
        if(values.length!==6&&values.length!==16)throw new TypeErrorImpl(name+' needs 6 or 16 values');
        return values;
    }
    const m=value=>record(value,'DOMMatrix');
    /* The Self operations on a record; the read-only methods run them on a copy. */
    const ops={
        multiply(r,other){const o=matrixDictionary(other);r.m=multiply(r.m,o.m);if(!o.is2D)r.is2D=false;},
        preMultiply(r,other){const o=matrixDictionary(other);r.m=multiply(o.m,r.m);if(!o.is2D)r.is2D=false;},
        translate(r,tx=0,ty=0,tz=0){tx=number(tx);ty=number(ty);tz=number(tz);const t=identity();t[12]=tx;t[13]=ty;t[14]=tz;r.m=multiply(r.m,t);if(tz!==0)r.is2D=false;},
        scale(r,scaleX=1,scaleY,scaleZ=1,originX=0,originY=0,originZ=0){
            scaleX=number(scaleX);if(scaleY!==undefined)scaleY=number(scaleY);scaleZ=number(scaleZ);
            originX=number(originX);originY=number(originY);originZ=number(originZ);
            ops.translate(r,originX,originY,originZ);if(scaleY===undefined)scaleY=scaleX;
            const s=identity();s[0]=scaleX;s[5]=scaleY;s[10]=scaleZ;r.m=multiply(r.m,s);
            ops.translate(r,-originX,-originY,-originZ);if(scaleZ!==1)r.is2D=false;
        },
        scale3d(r,scale=1,originX=0,originY=0,originZ=0){
            scale=number(scale);originX=number(originX);originY=number(originY);originZ=number(originZ);
            ops.translate(r,originX,originY,originZ);const s=identity();s[0]=s[5]=s[10]=scale;r.m=multiply(r.m,s);
            ops.translate(r,-originX,-originY,-originZ);if(scale!==1)r.is2D=false;
        },
        rotate(r,rotX=0,rotY,rotZ){
            rotX=number(rotX);if(rotY!==undefined)rotY=number(rotY);if(rotZ!==undefined)rotZ=number(rotZ);
            if(rotY===undefined&&rotZ===undefined){rotZ=rotX;rotX=0;}
            if(rotY===undefined)rotY=0;if(rotZ===undefined)rotZ=0;
            if(rotX!==0||rotY!==0)r.is2D=false;
            r.m=multiply(multiply(multiply(r.m,rotation(0,0,1,rotZ)),rotation(0,1,0,rotY)),rotation(1,0,0,rotX));
        },
        rotateFromVector(r,x=0,y=0){x=number(x);y=number(y);r.m=multiply(r.m,rotation(0,0,1,x===0&&y===0?0:atan2(y,x)*180/pi));},
        rotateAxisAngle(r,x=0,y=0,z=0,angle=0){x=number(x);y=number(y);z=number(z);angle=number(angle);
            r.m=multiply(r.m,rotation(x,y,z,angle));if(x!==0||y!==0)r.is2D=false;},
        skewX(r,sx=0){r.m=multiply(r.m,affine(1,0,tan(number(sx)*pi/180),1));},
        skewY(r,sy=0){r.m=multiply(r.m,affine(1,tan(number(sy)*pi/180),0,1));},
        invert(r){const i=r.is2D?inverse2D(r.m):inverse(r.m);if(i)r.m=i;else {r.m=Array(16).fill(NaN);r.is2D=false;}},
    };
    function derived(value,op,args){const r=m(value),copy={kind:'DOMMatrix',m:r.m.slice(),is2D:r.is2D};call(ops[op],null,[copy,...args]);return matrixObject(DOMMatrix,copy.m,copy.is2D);}
    function self(value,op,args){const r=writable(value,'DOMMatrix');call(ops[op],null,[r,...args]);return value;}
    class DOMMatrixReadOnly {
        constructor(init){const r=constructed(init);adopt(this,{kind:'DOMMatrix',m:r.m,is2D:r.is2D},false);}
        static fromMatrix(other={}){const r=matrixDictionary(other);return matrixObject(DOMMatrixReadOnly,r.m,r.is2D);}
        static fromFloat32Array(array32){const v=typedArray(array32,'Float32Array');return v.length===6?from6(DOMMatrixReadOnly,v):from16(DOMMatrixReadOnly,v);}
        static fromFloat64Array(array64){const v=typedArray(array64,'Float64Array');return v.length===6?from6(DOMMatrixReadOnly,v):from16(DOMMatrixReadOnly,v);}
        get is2D(){return m(this).is2D;}
        get isIdentity(){const v=m(this).m,i=identity();return v.every((x,k)=>x===i[k]);}
        translate(tx=0,ty=0,tz=0){return derived(this,'translate',[tx,ty,tz]);}
        scale(scaleX=1,scaleY,scaleZ=1,originX=0,originY=0,originZ=0){return derived(this,'scale',[scaleX,scaleY,scaleZ,originX,originY,originZ]);}
        scaleNonUniform(scaleX=1,scaleY=1){return derived(this,'scale',[scaleX,scaleY,1,0,0,0]);}
        scale3d(scale=1,originX=0,originY=0,originZ=0){return derived(this,'scale3d',[scale,originX,originY,originZ]);}
        rotate(rotX=0,rotY,rotZ){return derived(this,'rotate',[rotX,rotY,rotZ]);}
        rotateFromVector(x=0,y=0){return derived(this,'rotateFromVector',[x,y]);}
        rotateAxisAngle(x=0,y=0,z=0,angle=0){return derived(this,'rotateAxisAngle',[x,y,z,angle]);}
        skewX(sx=0){return derived(this,'skewX',[sx]);}
        skewY(sy=0){return derived(this,'skewY',[sy]);}
        multiply(other={}){return derived(this,'multiply',[other]);}
        flipX(){return derived(this,'multiply',[{a:-1,b:0,c:0,d:1,e:0,f:0}]);}
        flipY(){return derived(this,'multiply',[{a:1,b:0,c:0,d:-1,e:0,f:0}]);}
        inverse(){return derived(this,'invert',[]);}
        transformPoint(point={}){return pointObject(transformPoint(m(this).m,pointInit(point)));}
        toFloat32Array(){return new Float32Array(m(this).m);}
        toFloat64Array(){return new Float64Array(m(this).m);}
        toJSON(){const r=m(this),out={};aliases.forEach((key,i)=>out[key]=r.m[index(names2D[i])]);
            for(let c=1;c<=4;c++)for(let row=1;row<=4;row++)out['m'+c+row]=r.m[(c-1)*4+row-1];
            out.is2D=r.is2D;out.isIdentity=this.isIdentity;return out;}
    }
    const pointObject=values=>point(values,true);
    class DOMMatrix extends DOMMatrixReadOnly {
        constructor(init){super(init);call(add,mutable,[this]);}
        static fromMatrix(other={}){const r=matrixDictionary(other);return matrixObject(DOMMatrix,r.m,r.is2D);}
        static fromFloat32Array(array32){const v=typedArray(array32,'Float32Array');return v.length===6?from6(DOMMatrix,v):from16(DOMMatrix,v);}
        static fromFloat64Array(array64){const v=typedArray(array64,'Float64Array');return v.length===6?from6(DOMMatrix,v):from16(DOMMatrix,v);}
        multiplySelf(other={}){return self(this,'multiply',[other]);}
        preMultiplySelf(other={}){return self(this,'preMultiply',[other]);}
        translateSelf(tx=0,ty=0,tz=0){return self(this,'translate',[tx,ty,tz]);}
        scaleSelf(scaleX=1,scaleY,scaleZ=1,originX=0,originY=0,originZ=0){return self(this,'scale',[scaleX,scaleY,scaleZ,originX,originY,originZ]);}
        scale3dSelf(scale=1,originX=0,originY=0,originZ=0){return self(this,'scale3d',[scale,originX,originY,originZ]);}
        rotateSelf(rotX=0,rotY,rotZ){return self(this,'rotate',[rotX,rotY,rotZ]);}
        rotateFromVectorSelf(x=0,y=0){return self(this,'rotateFromVector',[x,y]);}
        rotateAxisAngleSelf(x=0,y=0,z=0,angle=0){return self(this,'rotateAxisAngle',[x,y,z,angle]);}
        skewXSelf(sx=0){return self(this,'skewX',[sx]);}
        skewYSelf(sy=0){return self(this,'skewY',[sy]);}
        invertSelf(){return self(this,'invert',[]);}
    }
    /* m11..m44 and the a..f aliases; DOMMatrix's own accessors also write. Writing a 3D
       element its identity value keeps is2D; any other value clears it for good. */
    const elements=[...aliases.map((key,i)=>[key,index(names2D[i])])];
    for(let c=1;c<=4;c++)for(let row=1;row<=4;row++)elements.push(['m'+c+row,(c-1)*4+row-1]);
    const identityOf={10:1,15:1};
    for(const [key,i] of elements){
        const getter=function(){return m(this).m[i];},flat=names2D.includes(key)||aliases.includes(key);
        define(DOMMatrixReadOnly.prototype,key,{configurable:true,enumerable:true,get:getter});
        define(DOMMatrix.prototype,key,{configurable:true,enumerable:true,get:getter,set(v){
            const r=writable(this,'DOMMatrix');v=number(v);r.m[i]=v;
            if(!flat&&v!==(identityOf[i]??0))r.is2D=false;
        }});
    }
    if(windowRealm){
        /* [Exposed=Window] stringifier and setMatrixValue(). */
        define(DOMMatrixReadOnly.prototype,'toString',{configurable:true,writable:true,enumerable:true,value:function toString(){
            const v=m(this);if(!v.m.every(finite))throw new DOMException('Cannot stringify a matrix with a non-finite value','InvalidStateError');
            return v.is2D?'matrix('+names2D.map(k=>v.m[index(k)]).join(', ')+')':'matrix3d('+v.m.join(', ')+')';
        }});
        define(DOMMatrix.prototype,'setMatrixValue',{configurable:true,writable:true,enumerable:true,value:function setMatrixValue(transformList){
            const r=writable(this,'DOMMatrix');if(!arguments.length)throw new TypeErrorImpl('setMatrixValue requires a transform list');
            const p=parsed(String(transformList));r.m=p.m;r.is2D=p.is2D;return this;
        }});
    }

    /* --------------------------------------------------------------- DOMRectList */
    let DOMRectList=null;
    const lists=new WeakMap();
    if(windowRealm){
        DOMRectList=class DOMRectList {
            constructor(){throw new TypeErrorImpl('Illegal constructor');}
            get length(){const v=call(get,lists,[this]);if(!v)throw new TypeErrorImpl('Illegal DOMRectList receiver');return v.length;}
            item(index){const v=call(get,lists,[this]);if(!v)throw new TypeErrorImpl('Illegal DOMRectList receiver');
                if(!arguments.length)throw new TypeErrorImpl('DOMRectList item requires an index');return v[index>>>0]||null;}
        };
        define(DOMRectList.prototype,iterator,{value:Array.prototype.values,writable:true,configurable:true});
    }
    function rectList(values){
        const rects=values.map(v=>rect([+v[0],+v[1],+v[2],+v[3]],true));
        const list=options.list(()=>rects,DOMRectList.prototype);call(set,lists,[list,rects]);return list;
    }

    /* [Serializable]: the values travel, never the author-visible properties. */
    if(typeof cloneData!=='undefined'){
        const brand=kind=>value=>{const r=call(get,records,[value]);return !!r&&r.kind===kind;};
        const isMutable=value=>call(has,mutable,[value]);
        cloneData.registerSerializable('DOMPoint',brand('DOMPoint'),
            value=>[isMutable(value),...record(value,'DOMPoint').v],data=>point(data.slice(1,5),data[0]));
        cloneData.registerSerializable('DOMRect',brand('DOMRect'),
            value=>[isMutable(value),...record(value,'DOMRect').v.slice(0,4)],data=>rect(data.slice(1,5),data[0]));
        cloneData.registerSerializable('DOMQuad',brand('DOMQuad'),
            value=>record(value,'DOMQuad').v.map(p=>record(p,'DOMPoint').v.slice()),data=>quad(data));
        cloneData.registerSerializable('DOMMatrix',brand('DOMMatrix'),value=>{const r=m(value);
            return [isMutable(value),r.is2D,...(r.is2D?names2D.map(k=>r.m[index(k)]):r.m)];},
            data=>{const type=data[0]?DOMMatrix:DOMMatrixReadOnly,v=data.slice(2);return data[1]?from6(type,v):from16(type,v);});
    }

    const exports={DOMPointReadOnly,DOMPoint,DOMRectReadOnly,DOMRect,DOMQuad,DOMMatrixReadOnly,DOMMatrix};
    if(DOMRectList)exports.DOMRectList=DOMRectList;
    for(const name of Object.keys(exports)){
        define(exports[name].prototype,Symbol.toStringTag,{configurable:true,value:name});
        define(globalThis,name,{configurable:true,writable:true,value:exports[name]});
    }
    /* [LegacyWindowAlias]. */
    if(windowRealm)for(const [alias,target] of [['SVGPoint',DOMPoint],['SVGRect',DOMRect],['SVGMatrix',DOMMatrix],['WebKitCSSMatrix',DOMMatrix]])
        define(globalThis,alias,{configurable:true,writable:true,value:target});
    return {DOMPoint,DOMPointReadOnly,DOMRect,DOMRectReadOnly,DOMMatrix,identity,affine,multiply,
        matrix:values=>matrixObject(DOMMatrix,values,false),matrix2D:values=>matrixObject(DOMMatrix,values,true),
        matrix2DInit:v=>names2D.map(k=>matrix2DDictionary(v)[index(k)]),
        rectInit,rectList,liveRect,
        rect:(values,isMutable)=>rect(values.slice(0,4).map(number),isMutable)};
}
