/* Geometry Interfaces: numeric 2D/4D math, private branded storage. */
const geometryBridge=(()=>{
    const matrices=new WeakMap(),points=new WeakMap(),mutableMatrices=new WeakSet(),mutablePoints=new WeakSet();
    const TypeErrorImpl=TypeError,get=WeakMap.prototype.get,set=WeakMap.prototype.set,has=WeakSet.prototype.has,add=WeakSet.prototype.add,call=Reflect.apply,define=Object.defineProperty;
    const finite=Number.isFinite,pi=Math.PI;
    function slot(map,value,label){const r=call(get,map,[value]);if(!r)throw new TypeErrorImpl('Illegal '+label+' receiver');return r;}
    function identity(){return [1,0,0,0,0,1,0,0,0,0,1,0,0,0,0,1];}
    function affine(a=1,b=0,c=0,d=1,e=0,f=0){return [a,b,0,0,c,d,0,0,0,0,1,0,e,f,0,1];}
    function is2D(m){return m[2]===0&&m[3]===0&&m[6]===0&&m[7]===0&&m[8]===0&&m[9]===0&&m[10]===1&&m[11]===0&&m[14]===0&&m[15]===1;}
    function multiply(a,b){
        if(is2D(a)&&is2D(b))return affine(a[0]*b[0]+a[4]*b[1],a[1]*b[0]+a[5]*b[1],a[0]*b[4]+a[4]*b[5],
            a[1]*b[4]+a[5]*b[5],a[0]*b[12]+a[4]*b[13]+a[12],a[1]*b[12]+a[5]*b[13]+a[13]);
        const r=Array(16).fill(0);for(let c=0;c<4;c++)for(let row=0;row<4;row++)for(let k=0;k<4;k++)r[c*4+row]+=a[k*4+row]*b[c*4+k];return r;
    }
    function inverse(m){
        if(is2D(m)){const d=m[0]*m[5]-m[1]*m[4];if(d===0||!finite(d))return Array(16).fill(NaN);
            return affine(m[5]/d,-m[1]/d,-m[4]/d,m[0]/d,(m[4]*m[13]-m[5]*m[12])/d,(m[1]*m[12]-m[0]*m[13])/d);}
        const a=Array.from({length:4},(_,r)=>Array.from({length:8},(_,c)=>c<4?m[c*4+r]:(c-4===r?1:0)));
        for(let c=0;c<4;c++){
            let pivot=c;for(let r=c+1;r<4;r++)if(Math.abs(a[r][c])>Math.abs(a[pivot][c]))pivot=r;
            if(a[pivot][c]===0||!finite(a[pivot][c]))return Array(16).fill(NaN);
            [a[c],a[pivot]]=[a[pivot],a[c]];const d=a[c][c];for(let k=0;k<8;k++)a[c][k]/=d;
            for(let r=0;r<4;r++)if(r!==c){const f=a[r][c];for(let k=0;k<8;k++)a[r][k]-=f*a[c][k];}
        }
        return Array.from({length:16},(_,i)=>a[i%4][4+(i>>2)]);
    }
    function dictionary(v){if(v===undefined||v===null)return {};if(typeof v!=='object'&&typeof v!=='function')throw new TypeErrorImpl('Expected geometry dictionary');return v;}
    const aliases=['a','b','c','d','e','f'],aliasIndices=[0,1,4,5,12,13];
    function matrixInit(value){
        value=dictionary(value);const m=identity();
        for(let c=1;c<=4;c++)for(let r=1;r<=4;r++){const k='m'+c+r,v=value[k];if(v!==undefined)m[(c-1)*4+r-1]=+v;}
        for(let i=0;i<6;i++){const v=value[aliases[i]],index=aliasIndices[i];if(v!==undefined){const n=+v,k='m'+((index>>2)+1)+(index%4+1);
            if(value[k]!==undefined&&!(Object.is(m[index],n)||m[index]===n))throw new TypeErrorImpl('Inconsistent matrix aliases');m[index]=n;}}
        if(value.is2D===true&&!is2D(m))throw new TypeErrorImpl('Non-2D matrix with is2D=true');return m;
    }
    function pointInit(v){v=dictionary(v);const member=(k,d)=>v[k]===undefined?d:+v[k];return [member('x',0),member('y',0),member('z',0),member('w',1)];}
    function transformed(p,m){return Array.from({length:4},(_,r)=>m[r]*p[0]+m[4+r]*p[1]+m[8+r]*p[2]+m[12+r]*p[3]);}
    class DOMPointReadOnly {
        constructor(x=0,y=0,z=0,w=1){call(set,points,[this,[+x,+y,+z,+w]]);}
        static fromPoint(v={}){return new DOMPointReadOnly(...pointInit(v));}
        get x(){return slot(points,this,'DOMPoint')[0];}get y(){return slot(points,this,'DOMPoint')[1];}
        get z(){return slot(points,this,'DOMPoint')[2];}get w(){return slot(points,this,'DOMPoint')[3];}
        matrixTransform(m={}){return new DOMPoint(...transformed(slot(points,this,'DOMPoint'),matrixInit(m)));}
        toJSON(){const [x,y,z,w]=slot(points,this,'DOMPoint');return {x,y,z,w};}
    }
    class DOMPoint extends DOMPointReadOnly {
        constructor(x=0,y=0,z=0,w=1){super(x,y,z,w);call(add,mutablePoints,[this]);}
        static fromPoint(v={}){return new DOMPoint(...pointInit(v));}
    }
    for(let i=0;i<4;i++)define(DOMPoint.prototype,['x','y','z','w'][i],{
        configurable:true,enumerable:true,get(){return slot(points,this,'DOMPoint')[i];},
        set(v){if(!call(has,mutablePoints,[this]))throw new TypeErrorImpl('Illegal mutable DOMPoint receiver');slot(points,this,'DOMPoint')[i]=+v;}});
    function numbers(text){
        const out=[];let rest=text.trim(),need=false;
        while(rest){const m=/^[+-]?(?:\d+\.?\d*|\.\d+)(?:[eE][+-]?\d+)?/.exec(rest);if(!m)return null;
            const n=+m[0];if(!finite(n))return null;out.push(n);rest=rest.slice(m[0].length);need=false;
            const ws=/^[\t\n\r ]+/.exec(rest);if(ws)rest=rest.slice(ws[0].length);
            if(rest[0]===','){rest=rest.slice(1).trim();need=true;}
            else if(rest&&!ws&&rest[0]!=='+'&&rest[0]!=='-')return null;
        }return need?null:out;
    }
    function rotation(angle){const r=angle*pi/180,c=Math.cos(r),s=Math.sin(r);return affine(c,s,-s,c);}
    function svgTransform(text){
        let out=identity(),rest=(text||'').trim();
        while(rest){const match=/^([a-zA-Z]+)\s*\(([^)]*)\)/.exec(rest);if(!match)return null;
            const v=numbers(match[2]);if(!v)return null;let m;
            switch(match[1]){
                case 'matrix':if(v.length===6)m=affine(...v);break;
                case 'translate':if(v.length===1||v.length===2)m=affine(1,0,0,1,v[0],v[1]||0);break;
                case 'scale':if(v.length===1||v.length===2)m=affine(v[0],0,0,v.length===2?v[1]:v[0]);break;
                case 'rotate':if(v.length===1)m=rotation(v[0]);else if(v.length===3)m=multiply(multiply(affine(1,0,0,1,v[1],v[2]),rotation(v[0])),affine(1,0,0,1,-v[1],-v[2]));break;
                case 'skewX':if(v.length===1)m=affine(1,0,Math.tan(v[0]*pi/180),1);break;
                case 'skewY':if(v.length===1)m=affine(1,Math.tan(v[0]*pi/180),0,1);break;
            }
            if(!m)return null;out=multiply(out,m);rest=rest.slice(match[0].length).trim();if(rest[0]===',')rest=rest.slice(1).trim();
        }return out;
    }
    function matrixConstructor(v){
        if(v===undefined)return identity();
        if(typeof v==='string'){
            if(v==='none'||v.trim()==='')return identity();
            const m=/^\s*matrix3d\(([^)]*)\)\s*$/.exec(v);if(m){const n=numbers(m[1]);if(n&&n.length===16)return n;}
            const a=svgTransform(v);if(a)return a;throw new DOMException('Invalid matrix transform','SyntaxError');
        }
        const a=Array.from(v,x=>+x);if(a.length===6)return affine(...a);if(a.length===16)return a;throw new TypeErrorImpl('Matrix requires 6 or 16 values');
    }
    function matrix(m){const result=new DOMMatrix();call(set,matrices,[result,m]);return result;}
    function mutateMatrix(value,m){if(!call(has,mutableMatrices,[value]))throw new TypeErrorImpl('Illegal mutable DOMMatrix receiver');call(set,matrices,[value,m]);return value;}
    class DOMMatrixReadOnly {
        constructor(v){call(set,matrices,[this,matrixConstructor(v)]);}
        static fromMatrix(v={}){const r=new DOMMatrixReadOnly();call(set,matrices,[r,matrixInit(v)]);return r;}
        static fromFloat32Array(v){return new DOMMatrixReadOnly(v);}
        static fromFloat64Array(v){return new DOMMatrixReadOnly(v);}
        get is2D(){return is2D(slot(matrices,this,'DOMMatrix'));}
        get isIdentity(){const m=slot(matrices,this,'DOMMatrix'),i=identity();return m.every((v,k)=>v===i[k]);}
        multiply(other={}){return matrix(multiply(slot(matrices,this,'DOMMatrix'),matrixInit(other)));}
        inverse(){return matrix(inverse(slot(matrices,this,'DOMMatrix')));}
        translate(x=0,y=0,z=0){const m=identity();m[12]=+x;m[13]=+y;m[14]=+z;return matrix(multiply(slot(matrices,this,'DOMMatrix'),m));}
        scale(x=1,y=x,z=1,ox=0,oy=0,oz=0){const m=identity();m[0]=+x;m[5]=+y;m[10]=+z;
            const t=identity();t[12]=+ox;t[13]=+oy;t[14]=+oz;const u=identity();u[12]=-ox;u[13]=-oy;u[14]=-oz;
            return matrix(multiply(multiply(multiply(slot(matrices,this,'DOMMatrix'),t),m),u));}
        scaleNonUniform(x=1,y=1){return this.scale(x,y);}
        rotate(x=0,y,z){if(y===undefined&&z===undefined){z=x;x=0;y=0;}else {y=y===undefined?0:+y;z=z===undefined?0:+z;}x=+x;z=+z;
            const cx=Math.cos(x*pi/180),sx=Math.sin(x*pi/180),cy=Math.cos(y*pi/180),sy=Math.sin(y*pi/180);
            const rx=identity();rx[5]=cx;rx[6]=sx;rx[9]=-sx;rx[10]=cx;const ry=identity();ry[0]=cy;ry[2]=-sy;ry[8]=sy;ry[10]=cy;
            return matrix(multiply(multiply(multiply(slot(matrices,this,'DOMMatrix'),rotation(z)),ry),rx));}
        rotateFromVector(x=0,y=0){return this.rotate(Math.atan2(+y,+x)*180/pi);}
        skewX(v=0){return matrix(multiply(slot(matrices,this,'DOMMatrix'),affine(1,0,Math.tan(+v*pi/180),1)));}
        skewY(v=0){return matrix(multiply(slot(matrices,this,'DOMMatrix'),affine(1,Math.tan(+v*pi/180),0,1)));}
        flipX(){return this.scale(-1,1);}
        flipY(){return this.scale(1,-1);}
        transformPoint(v={}){return new DOMPoint(...transformed(pointInit(v),slot(matrices,this,'DOMMatrix')));}
        toFloat32Array(){return new Float32Array(slot(matrices,this,'DOMMatrix'));}
        toFloat64Array(){return new Float64Array(slot(matrices,this,'DOMMatrix'));}
        toJSON(){const m=slot(matrices,this,'DOMMatrix'),r={};for(let c=1;c<=4;c++)for(let row=1;row<=4;row++)r['m'+c+row]=m[(c-1)*4+row-1];
            for(let i=0;i<6;i++)r[aliases[i]]=m[aliasIndices[i]];r.is2D=is2D(m);r.isIdentity=this.isIdentity;return r;}
        toString(){const m=slot(matrices,this,'DOMMatrix');if(m.some(v=>!finite(v)))throw new DOMException('Matrix contains a non-finite value','InvalidStateError');
            return is2D(m)?'matrix('+aliasIndices.map(i=>m[i]).join(', ')+')':'matrix3d('+m.join(', ')+')';}
    }
    class DOMMatrix extends DOMMatrixReadOnly {
        constructor(v){super(v);call(add,mutableMatrices,[this]);}
        static fromMatrix(v={}){return matrix(matrixInit(v));}
        static fromFloat32Array(v){return new DOMMatrix(v);}
        static fromFloat64Array(v){return new DOMMatrix(v);}
        multiplySelf(v={}){return mutateMatrix(this,multiply(slot(matrices,this,'DOMMatrix'),matrixInit(v)));}
        preMultiplySelf(v={}){return mutateMatrix(this,multiply(matrixInit(v),slot(matrices,this,'DOMMatrix')));}
        invertSelf(){return mutateMatrix(this,inverse(slot(matrices,this,'DOMMatrix')));}
        setMatrixValue(v){slot(matrices,this,'DOMMatrix');if(!arguments.length)throw new TypeErrorImpl('Missing transform');return mutateMatrix(this,matrixConstructor(String(v)));}
    }
    for(const name of ['translate','scale','scaleNonUniform','rotate','rotateFromVector','skewX','skewY'])
        define(DOMMatrix.prototype,name+'Self',{configurable:true,writable:true,value:function(...v){const r=call(DOMMatrixReadOnly.prototype[name],this,v);return mutateMatrix(this,slot(matrices,r,'DOMMatrix'));}});
    for(let i=0;i<16;i++){
        const key='m'+((i>>2)+1)+(i%4+1),descriptor={configurable:true,enumerable:true,get(){return slot(matrices,this,'DOMMatrix')[i];}};
        define(DOMMatrixReadOnly.prototype,key,descriptor);define(DOMMatrix.prototype,key,{...descriptor,set(v){if(!call(has,mutableMatrices,[this]))throw new TypeErrorImpl('Illegal mutable DOMMatrix receiver');slot(matrices,this,'DOMMatrix')[i]=+v;}});
    }
    for(let k=0;k<6;k++){const i=aliasIndices[k],descriptor={configurable:true,enumerable:true,get(){return slot(matrices,this,'DOMMatrix')[i];}};
        define(DOMMatrixReadOnly.prototype,aliases[k],descriptor);define(DOMMatrix.prototype,aliases[k],{...descriptor,set(v){if(!call(has,mutableMatrices,[this]))throw new TypeErrorImpl('Illegal mutable DOMMatrix receiver');slot(matrices,this,'DOMMatrix')[i]=+v;}});}
    const exports={DOMPointReadOnly,DOMPoint,DOMMatrixReadOnly,DOMMatrix};
    for(const name of Object.keys(exports))define(exports[name].prototype,Symbol.toStringTag,{configurable:true,value:name});
    Object.assign(globalThis,exports,{SVGPoint:DOMPoint,SVGMatrix:DOMMatrix});
    return {DOMPoint,DOMMatrix,identity,affine,multiply,matrix,numbers,svgTransform};
})();
