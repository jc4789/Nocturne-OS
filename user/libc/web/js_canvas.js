/* Bounded, real Canvas2D subset. Unsupported GPU contexts return null rather
 * than a fabricated WebGL object. Native storage is painted by web_paint. */
const canvasBridge = (() => {
    'use strict';
    const define=Object.defineProperty, create=Object.create, number=Number, string=elementURL.string;
    const contexts=new WeakMap(), states=new WeakMap(), images=new WeakMap();
    const Float64=Float64Array, Bytes=Uint8ClampedArray, finite=Number.isFinite;
    const abs=Math.abs,ceil=Math.ceil,floor=Math.floor,cos=Math.cos,sin=Math.sin,atan2=Math.atan2;
    const native=host.canvas;
    class HTMLCanvasElement extends HTMLElement {constructor(){throw new TypeError('Illegal HTMLCanvasElement constructor');}}
    class CanvasRenderingContext2D {constructor(){throw new TypeError('Illegal CanvasRenderingContext2D constructor');}}
    function defaults(s){s.fill='#000000';s.stroke='#000000';s.fillColor=0xff000000;s.strokeColor=0xff000000;s.alpha=1;s.lineWidth=1;s.smoothing=true;s.matrix=[1,0,0,1,0,0];s.path=[];s.first=null;s.last=null;s.stack=[];}
    function state(ctx){const s=states.get(ctx);if(!s)throw new TypeError('Illegal CanvasRenderingContext2D receiver');const v=native(s.canvas,'version');if(v!==s.version){defaults(s);s.version=v;}return s;}
    function namedError(name,message){return new DOMException(message,name);}
    function args(values,count){if(values.length<count)throw new TypeError('Missing Canvas arguments');return values.slice(0,count).map(number);}
    function valid(values){return values.every(finite);}
    function point(s,x,y){const m=s.matrix;return [m[0]*x+m[2]*y+m[4],m[1]*x+m[3]*y+m[5]];}
    function color(s,stroke=false){const v=stroke?s.strokeColor:s.fillColor;return ((v&0xffffff)|((floor((v>>>24)*s.alpha+0.5)&255)<<24))>>>0;}
    function pathBuffer(points){if(points.length>512)throw new RangeError('Canvas path limit');return new Float64(points).buffer;}
    function append(s,p,move=false){if(s.path.length+(move&&s.path.length?4:2)>512)throw new RangeError('Canvas path limit');if(move&&s.path.length)s.path.push(NaN,NaN);s.path.push(p[0],p[1]);s.last=p;if(move||!s.first)s.first=p;}
    function resetPath(s){s.path=[];s.first=null;s.last=null;}
    function multiply(s,m){const a=s.matrix;s.matrix=[a[0]*m[0]+a[2]*m[1],a[1]*m[0]+a[3]*m[1],a[0]*m[2]+a[2]*m[3],a[1]*m[2]+a[3]*m[3],a[0]*m[4]+a[2]*m[5]+a[4],a[1]*m[4]+a[3]*m[5]+a[5]];}
    function rect(ctx,op,v){const s=state(ctx);v=args(v,4);if(!valid(v)||!v[2]||!v[3])return;const m=s.matrix;
        if(m[0]===1&&m[1]===0&&m[2]===0&&m[3]===1)native(s.canvas,op,v[0]+m[4],v[1]+m[5],v[2],v[3],color(s));
        else {const p=[point(s,v[0],v[1]),point(s,v[0]+v[2],v[1]),point(s,v[0]+v[2],v[1]+v[3]),point(s,v[0],v[1]+v[3])];native(s.canvas,'poly',pathBuffer(p.flat()),color(s),false,op==='clear');}}
    for(const name of ['width','height'])define(HTMLCanvasElement.prototype,name,{configurable:true,enumerable:true,
        get(){htmlElementBrand(this,'canvas');return native(this,name);},set(value){htmlElementBrand(this,'canvas');reflectedAttr(this,name,string(number(value)>>>0));}});
    define(HTMLCanvasElement.prototype,'getContext',{configurable:true,writable:true,value:function(type,options){
        htmlElementBrand(this,'canvas');if(!arguments.length)throw new TypeError('getContext requires context type');
        type=string(type);if(type!=='2d')return null;
        let ctx=contexts.get(this);if(ctx)return ctx;
        if(!native(this,'context'))return null;
        ctx=create(CanvasRenderingContext2D.prototype);const s={canvas:this,version:native(this,'version')};defaults(s);states.set(ctx,s);contexts.set(this,ctx);return ctx;
    }});
    define(CanvasRenderingContext2D.prototype,'canvas',{enumerable:true,configurable:true,get(){return state(this).canvas;}});
    for(const [property,key,colorkey] of [['fillStyle','fill','fillColor'],['strokeStyle','stroke','strokeColor']])define(CanvasRenderingContext2D.prototype,property,{enumerable:true,configurable:true,
        get(){return state(this)[key];},set(value){const s=state(this),text=string(value),c=native(s.canvas,'color',text);if(c!==null){s[key]=text;s[colorkey]=c>>>0;}}});
    define(CanvasRenderingContext2D.prototype,'globalAlpha',{enumerable:true,configurable:true,get(){return state(this).alpha;},set(value){const s=state(this),v=number(value);if(finite(v)&&v>=0&&v<=1)s.alpha=v;}});
    define(CanvasRenderingContext2D.prototype,'lineWidth',{enumerable:true,configurable:true,get(){return state(this).lineWidth;},set(value){const s=state(this),v=number(value);if(finite(v)&&v>0)s.lineWidth=v;}});
    define(CanvasRenderingContext2D.prototype,'imageSmoothingEnabled',{enumerable:true,configurable:true,get(){return state(this).smoothing;},set(value){state(this).smoothing=!!value;}});
    /* This subset implements source-over only; never reflect an unsupported
       compositing operator as though it changed the actual native drawing. */
    define(CanvasRenderingContext2D.prototype,'globalCompositeOperation',{enumerable:true,configurable:true,get(){state(this);return 'source-over';},set(value){state(this);string(value);}});
    const methods={
        fillRect(...v){rect(this,'rect',v);},clearRect(...v){rect(this,'clear',v);},
        strokeRect(...v){const s=state(this);v=args(v,4);if(!valid(v))return;const [x,y,w,h]=v,p=[point(s,x,y),point(s,x+w,y),point(s,x+w,y+h),point(s,x,y+h),point(s,x,y)];native(s.canvas,'stroke',pathBuffer(p.flat()),color(s,true),s.lineWidth);},
        save(){const s=state(this);if(s.stack.length===64)throw new RangeError('Canvas save stack limit');s.stack.push({fill:s.fill,stroke:s.stroke,fillColor:s.fillColor,strokeColor:s.strokeColor,alpha:s.alpha,lineWidth:s.lineWidth,smoothing:s.smoothing,matrix:s.matrix.slice()});},
        restore(){const s=state(this),saved=s.stack.pop();if(saved)Object.assign(s,saved);},
        reset(){const s=state(this);native(s.canvas,'reset');s.version=native(s.canvas,'version');defaults(s);},
        beginPath(){resetPath(state(this));},
        moveTo(...v){const s=state(this);v=args(v,2);if(valid(v))append(s,point(s,...v),true);},
        lineTo(...v){const s=state(this);v=args(v,2);if(valid(v))append(s,point(s,...v),!s.last);},
        closePath(){const s=state(this);if(s.first&&s.last)append(s,s.first);},
        rect(...v){const s=state(this);v=args(v,4);if(!valid(v))return;const [x,y,w,h]=v;append(s,point(s,x,y),true);append(s,point(s,x+w,y));append(s,point(s,x+w,y+h));append(s,point(s,x,y+h));append(s,point(s,x,y));},
        fill(rule='nonzero'){const s=state(this);rule=string(rule);if(rule!=='nonzero'&&rule!=='evenodd')throw new TypeError('Invalid fill rule');if(s.path.length)native(s.canvas,'poly',pathBuffer(s.path),color(s),rule==='evenodd',false);},
        stroke(){const s=state(this);if(s.path.length)native(s.canvas,'stroke',pathBuffer(s.path),color(s,true),s.lineWidth);},
        transform(...v){const s=state(this);v=args(v,6);if(valid(v))multiply(s,v);},
        setTransform(...v){const s=state(this);if(v.length===0){s.matrix=[1,0,0,1,0,0];return;}if(v.length===1&&typeof v[0]==='object'){const m=v[0];v=[m.a??1,m.b??0,m.c??0,m.d??1,m.e??0,m.f??0];}v=args(v,6);if(valid(v))s.matrix=v;},
        resetTransform(){state(this).matrix=[1,0,0,1,0,0];},
        translate(...v){const s=state(this);v=args(v,2);if(valid(v))multiply(s,[1,0,0,1,v[0],v[1]]);},
        scale(...v){const s=state(this);v=args(v,2);if(valid(v))multiply(s,[v[0],0,0,v[1],0,0]);},
        rotate(value){const s=state(this),v=number(value);if(finite(v)){const c=cos(v),t=sin(v);multiply(s,[c,t,-t,c,0,0]);}},
        quadraticCurveTo(...v){const s=state(this);v=args(v,4);if(!valid(v))return;const c=point(s,v[0],v[1]),b=point(s,v[2],v[3]);if(!s.last)append(s,c,true);const a=s.last;for(let i=1;i<=16;i++){const t=i/16,u=1-t;append(s,[u*u*a[0]+2*u*t*c[0]+t*t*b[0],u*u*a[1]+2*u*t*c[1]+t*t*b[1]]);}},
        bezierCurveTo(...v){const s=state(this);v=args(v,6);if(!valid(v))return;const b=point(s,v[0],v[1]),c=point(s,v[2],v[3]),d=point(s,v[4],v[5]);if(!s.last)append(s,b,true);const a=s.last;for(let i=1;i<=24;i++){const t=i/24,u=1-t;append(s,[u*u*u*a[0]+3*u*u*t*b[0]+3*u*t*t*c[0]+t*t*t*d[0],u*u*u*a[1]+3*u*u*t*b[1]+3*u*t*t*c[1]+t*t*t*d[1]]);}},
        arc(x,y,r,start,end,counterclockwise=false){const s=state(this),v=args([x,y,r,start,end],5);if(!valid(v))return;[x,y,r,start,end]=v;if(r<0)throw namedError('IndexSizeError','Negative arc radius');const tau=Math.PI*2;let delta=end-start;if(!counterclockwise){if(delta>=tau)delta=tau;else delta=((delta%tau)+tau)%tau;}else{if(-delta>=tau)delta=-tau;else delta=-(((-delta%tau)+tau)%tau);}const steps=Math.max(1,ceil(abs(delta)/tau*48));for(let i=0;i<=steps;i++){const a=start+delta*i/steps;append(s,point(s,x+r*cos(a),y+r*sin(a)),!s.last);}},
        createImageData(a,b){state(this);const image=images.get(a);if(image)return new ImageData(image.w,image.h);return new ImageData(a,b);},
        drawImage(image,dx,dy,...rest){state(this);const count=arguments.length-1;if(count!==2&&count!==4&&count<8)throw new TypeError('drawImage requires 3, 5 or 9 arguments');
            const v=args([dx,dy,...rest],count>=8?8:count),s=state(this);
            /* All author conversions precede borrowing the native bitmap. A
               conversion can resize/reset the canvas, so refresh state again. */
            const result=native(s.canvas,'drawImage',image,new Float64(v).buffer,new Float64(s.matrix).buffer,s.alpha,s.smoothing);
            if(result===1)throw namedError('InvalidStateError','The source image has no usable bitmap');},
        getImageData(...v){const s=state(this);v=args(v,4);if(!valid(v))throw new TypeError('Nonfinite ImageData coordinates');let [x,y,w,h]=v.map(Math.trunc);if(!w||!h)throw namedError('IndexSizeError','Empty ImageData rectangle');if(w<0){x+=w;w=-w;}if(h<0){y+=h;h=-h;}const data=native(s.canvas,'read',x,y,w,h);if(data===null)throw namedError('SecurityError','Canvas contains an image with unverified origin');return new ImageData(new Bytes(data),w,h);},
        putImageData(image,dx,dy,...dirty){const s=state(this),im=images.get(image);if(!im)throw new TypeError('Expected ImageData');dx=Math.trunc(number(dx));dy=Math.trunc(number(dy));if(!finite(dx)||!finite(dy))throw new TypeError('Nonfinite ImageData coordinates');
            let data=im.data,w=im.w,h=im.h;if(dirty.length){if(dirty.length<4)throw new TypeError('Missing dirty rectangle');let [x,y,dw,dh]=dirty.map(number).map(Math.trunc);if(!valid([x,y,dw,dh]))throw new TypeError('Nonfinite dirty rectangle');if(dw<0){x+=dw;dw=-dw;}if(dh<0){y+=dh;dh=-dh;}const x0=Math.max(0,x),y0=Math.max(0,y),x1=Math.min(w,x+dw),y1=Math.min(h,y+dh);if(x1<=x0||y1<=y0)return;const copy=new Bytes((x1-x0)*(y1-y0)*4);for(let j=y0;j<y1;j++)copy.set(data.subarray((j*w+x0)*4,(j*w+x1)*4),(j-y0)*(x1-x0)*4);data=copy;w=x1-x0;h=y1-y0;dx+=x0;dy+=y0;}
            native(s.canvas,'put',dx,dy,w,h,data.buffer.slice(data.byteOffset,data.byteOffset+data.byteLength));},
        getContextAttributes(){state(this);return {alpha:true,colorSpace:'srgb',desynchronized:false,willReadFrequently:false};}
    };
    for(const [name,value] of Object.entries(methods))define(CanvasRenderingContext2D.prototype,name,{value,writable:true,configurable:true});
    class ImageData {
        constructor(a,b,c){let data,w,h;if(a instanceof Bytes){data=a;w=number(b)>>>0;h=c===undefined?data.length/4/w:number(c)>>>0;if(!w||!finite(h)||h!==floor(h)||h<1||data.length!==w*h*4)throw namedError('IndexSizeError','Invalid ImageData dimensions');}
            else{w=abs(Math.trunc(number(a)));h=abs(Math.trunc(number(b)));if(!finite(w)||!finite(h)||!w||!h)throw namedError('IndexSizeError','Invalid ImageData dimensions');if(w*h>1048576)throw new RangeError('ImageData memory limit');data=new Bytes(w*h*4);}
            if(w*h>1048576)throw new RangeError('ImageData memory limit');images.set(this,{data,w,h});}
        get width(){const s=images.get(this);if(!s)throw new TypeError('Illegal ImageData receiver');return s.w;}
        get height(){const s=images.get(this);if(!s)throw new TypeError('Illegal ImageData receiver');return s.h;}
        get data(){const s=images.get(this);if(!s)throw new TypeError('Illegal ImageData receiver');return s.data;}
        get colorSpace(){if(!images.has(this))throw new TypeError('Illegal ImageData receiver');return 'srgb';}
    }
    for(const C of [HTMLCanvasElement,CanvasRenderingContext2D,ImageData])define(C.prototype,Symbol.toStringTag,{value:C.name,configurable:true});
    Object.assign(globalThis,{HTMLCanvasElement,CanvasRenderingContext2D,ImageData});
    return {nodeProtos:[HTMLCanvasElement.prototype]};
})();
