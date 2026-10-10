/* SVG reflection/geometry over native nodes, not replaceable public properties.
 * Weak path indexes are rebuilt only when their actual d attribute changes. */
const svgBridge = (() => {
    const classes=new WeakMap(),animated=new WeakMap(),rects=new WeakMap(),rectRecords=new WeakMap(),paths=new WeakMap();
    const StringImpl=String, TypeErrorImpl=TypeError, create=Object.create, define=Object.defineProperty;
    const get=WeakMap.prototype.get, set=WeakMap.prototype.set, call=Reflect.apply;
    const nativeGeometry=host.svgGeometry,finite=Number.isFinite,hypot=Math.hypot;
    const {DOMPoint,DOMMatrix,identity,affine,multiply,matrix,numbers,svgTransform}=geometryBridge;
    function geometry(node,mode){return nativeGeometry(node,mode);}
    function string(value) {
        if(typeof value==='symbol')throw new TypeErrorImpl('Cannot convert Symbol to DOMString');
        return StringImpl(value);
    }
    function brand(node) { rawDom.get(node,'svgBrand'); return node; }
    function record(value) {
        const r=call(get,animated,[value]);
        if(!r)throw new TypeErrorImpl('Illegal SVGAnimatedString receiver');
        brand(r.node);return r;
    }
    class SVGAnimatedString {
        constructor(){throw new TypeErrorImpl('Illegal SVGAnimatedString constructor');}
        get baseVal(){const r=record(this);return reflectedAttr(r.node,r.attribute)||'';}
        set baseVal(value){const r=record(this);value=string(value);reflectedAttr(r.node,r.attribute,value);}
        get animVal(){const r=record(this);return reflectedAttr(r.node,r.attribute)||'';}
    }
    const animatedProto=SVGAnimatedString.prototype;
    class SVGAnimatedRect {
        constructor(){throw new TypeErrorImpl('Illegal SVGAnimatedRect constructor');}
        get baseVal(){const r=call(get,rectRecords,[this]);if(!r)throw new TypeErrorImpl('Illegal SVGAnimatedRect receiver');geometry(r.node,0);return r.base;}
        get animVal(){const r=call(get,rectRecords,[this]);if(!r)throw new TypeErrorImpl('Illegal SVGAnimatedRect receiver');geometry(r.node,0);return r.anim;}
    }
    const animatedRectProto=SVGAnimatedRect.prototype;
    class SVGElement extends Element {
        constructor(){throw new TypeErrorImpl('Illegal SVGElement constructor');}
        get className(){
            brand(this);let value=call(get,classes,[this]);
            if(!value){value=create(animatedProto);call(set,animated,[value,{node:this,attribute:'class'}]);call(set,classes,[this,value]);}
            return value;
        }
        get ownerSVGElement(){return rawDom.get(this,'svgOwner');}
        get viewportElement(){return rawDom.get(this,'svgViewport');}
        focus(){brand(this);dom('focus',this);}
        blur(){brand(this);const d=rawDom.get(this,'ownerDocument');if(rawDom.get(d,'activeElement')===this)dom('focus',null);}
    }
    function root(node){return rawDom.get(node,'localName')==='svg';}
    function length(node,key,fallback,extent){
        const text=reflectedAttr(node,key);if(text===null)return fallback;
        const m=/^\s*([+-]?(?:\d+\.?\d*|\.\d+)(?:[eE][+-]?\d+)?)\s*(%|px)?\s*$/.exec(text);if(!m)return fallback;
        const v=+m[1];return finite(v)?m[2]==='%'?v*extent/100:v:fallback;
    }
    function viewport(node,w,h,x=0,y=0){
        if(w<=0||h<=0)return null;const vb=geometry(node,0);
        if(vb[2]<=0||vb[3]<=0){
            const text=reflectedAttr(node,'viewBox'),v=text===null?null:numbers(text);
            // A valid zero viewBox disables rendering; invalid/missing uses initial values.
            if(v&&v.length===4&&v[2]>=0&&v[3]>=0&&(v[2]===0||v[3]===0))return null;
            return {m:affine(1,0,0,1,x,y),w,h};
        }
        let sx=w/vb[2],sy=h/vb[3],tx=x-vb[0]*sx,ty=y-vb[1]*sy;
        let p=(reflectedAttr(node,'preserveAspectRatio')||'xMidYMid meet').trim().split(/\s+/);if(p[0]==='defer')p.shift();
        const align=/^x(Min|Mid|Max)Y(Min|Mid|Max)$/.exec(p[0]);
        if(p[0]!=='none'){
            const a=align||['xMidYMid','Mid','Mid'];sx=sy=p[1]==='slice'?Math.max(sx,sy):Math.min(sx,sy);
            tx=x-vb[0]*sx+(w-vb[2]*sx)*(a[1]==='Min'?0:a[1]==='Mid'?.5:1);
            ty=y-vb[1]*sy+(h-vb[3]*sy)*(a[2]==='Min'?0:a[2]==='Mid'?.5:1);
        }
        return {m:affine(sx,0,0,sy,tx,ty),w:vb[2],h:vb[3]};
    }
    function ctm(node,screen){
        geometry(node,3);const chain=[];let outer=null;
        for(let n=node;n;n=rawDom.get(n,'parentNode')){try{brand(n);}catch(_){break;}chain.push(n);if(root(n))outer=n;}
        if(!outer)return null;const box=geometry(outer,2);if(!box)return null;
        chain.reverse();let m=screen||node===outer?affine(1,0,0,1,box[0],box[1]):identity(),w=box[2],h=box[3];
        for(const n of chain){
            const t=svgTransform(reflectedAttr(n,'transform'));
            if(root(n)){
                if(n!==outer){
                    // The nearest SVG viewport resets only getCTM, not screen CTM.
                    if(!screen&&n!==node)m=identity();const x=length(n,'x',0,w),y=length(n,'y',0,h),nw=length(n,'width',w,w),nh=length(n,'height',h,h);
                    if(t)m=multiply(m,t);const v=viewport(n,nw,nh,x,y);if(!v)return null;m=multiply(m,v.m);w=v.w;h=v.h;
                }else {if(t)m=multiply(m,t);const v=viewport(n,w,h);if(!v)return null;m=multiply(m,v.m);w=v.w;h=v.h;}
            }else if(t)m=multiply(m,t);
        }
        return matrix(m);
    }
    class SVGGraphicsElement extends SVGElement {
        constructor(){throw new TypeErrorImpl('Illegal SVGGraphicsElement constructor');}
        getCTM(){return ctm(this,false);}
        getScreenCTM(){return ctm(this,true);}
    }
    function metric(node){
        geometry(node,4);const d=reflectedAttr(node,'d')||'';let saved=call(get,paths,[node]);if(saved&&saved.d===d)return saved;
        const cubics=geometry(node,1),segments=[];let total=0,first=[0,0],started=false;
        function line(x,y,ex,ey){const size=hypot(ex-x,ey-y);if(size>0){segments.push([total,total+size,x,y,ex,ey]);total+=size;}}
        function cubic(x,y,ax,ay,bx,by,ex,ey,depth){
            const chord=hypot(ex-x,ey-y),polygon=hypot(ax-x,ay-y)+hypot(bx-ax,by-ay)+hypot(ex-bx,ey-by);
            if(depth===18||polygon-chord<=.0001){line(x,y,ex,ey);return;}
            const xa=(x+ax)/2,ya=(y+ay)/2,abx=(ax+bx)/2,aby=(ay+by)/2,beX=(bx+ex)/2,beY=(by+ey)/2;
            const lX=(xa+abx)/2,lY=(ya+aby)/2,rX=(abx+beX)/2,rY=(aby+beY)/2,mx=(lX+rX)/2,my=(lY+rY)/2;
            cubic(x,y,xa,ya,lX,lY,mx,my,depth+1);cubic(mx,my,rX,rY,beX,beY,ex,ey,depth+1);
        }
        for(const p of cubics){if(p.some(v=>!finite(v)))throw new DOMException('Non-finite path geometry','InvalidStateError');
            if(p.length<2)continue;if(!started){first=[p[0],p[1]];started=true;}
            for(let i=2;i+5<p.length;i+=6)cubic(p[i-2],p[i-1],p[i],p[i+1],p[i+2],p[i+3],p[i+4],p[i+5],0);}
        saved={d,segments,total,first};call(set,paths,[node,saved]);return saved;
    }
    class SVGGeometryElement extends SVGGraphicsElement {
        constructor(){throw new TypeErrorImpl('Illegal SVGGeometryElement constructor');}
        getTotalLength(){return metric(this).total;}
        getPointAtLength(distance){
            geometry(this,4);if(!arguments.length)throw new TypeErrorImpl('Missing distance');distance=+distance;if(!finite(distance))throw new TypeErrorImpl('Non-finite distance');
            const s=metric(this);if(!s.segments.length)return new DOMPoint(...s.first);distance=Math.min(s.total,Math.max(0,distance));
            let lo=0,hi=s.segments.length-1;while(lo<hi){const mid=(lo+hi)>>1;if(s.segments[mid][1]<distance)lo=mid+1;else hi=mid;}
            const p=s.segments[lo],t=(distance-p[0])/(p[1]-p[0]);return new DOMPoint(p[2]+(p[4]-p[2])*t,p[3]+(p[5]-p[3])*t);
        }
    }
    class SVGPathElement extends SVGGeometryElement {constructor(){throw new TypeErrorImpl('Illegal SVGPathElement constructor');}}
    class SVGSVGElement extends SVGGraphicsElement {
        constructor(){throw new TypeErrorImpl('Illegal SVGSVGElement constructor');}
        get viewBox(){
            geometry(this,0);let result=call(get,rects,[this]);if(!result){
                const node=this,read=()=>geometry(node,0),write=(i,v)=>{const a=read();a[i]=v;reflectedAttr(node,'viewBox',a.join(' '));};
                result=create(animatedRectProto);call(set,rectRecords,[result,{node,base:observerBridge.createLiveSVGRect(read,write),anim:observerBridge.createLiveSVGRect(read)}]);call(set,rects,[node,result]);
            }return result;
        }
        createSVGRect(){
            rawDom.get(this,'svgRootBrand');
            return observerBridge.createSVGRect();
        }
        createSVGPoint(){rawDom.get(this,'svgRootBrand');return new DOMPoint();}
        createSVGMatrix(){rawDom.get(this,'svgRootBrand');return new DOMMatrix();}
        getElementById(id){
            rawDom.get(this,'svgRootBrand');
            if(!arguments.length)throw new TypeErrorImpl('Missing elementId');
            return rawDom.id(this,string(id));
        }
    }
    const exports={SVGElement,SVGSVGElement,SVGAnimatedString,SVGAnimatedRect,SVGGraphicsElement,SVGGeometryElement,SVGPathElement};
    for(const name of Object.keys(exports))define(exports[name].prototype,Symbol.toStringTag,{configurable:true,value:name});
    svgHandlerTarget = node => { try{return rawDom.get(node,'svgBrand')===true;}catch(_){return false;} };
    installHandlers(SVGElement.prototype,globalHandlerTypes);
    Object.assign(globalThis,exports);
    return {nodeProtos:[SVGElement.prototype,SVGSVGElement.prototype],geometryNodeProtos:[SVGGraphicsElement.prototype,SVGPathElement.prototype]};
})();
