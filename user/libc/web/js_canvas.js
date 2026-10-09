/* Real Canvas2D subset. Unsupported GPU contexts return null rather
 * than a fabricated WebGL object. Native storage is painted by web_paint. */
const canvasBridge = (() => {
    'use strict';
    const define=Object.defineProperty, create=Object.create, number=Number, string=elementURL.string;
    const contexts=new WeakMap(), states=new WeakMap(), images=new WeakMap(), metrics=new WeakMap(), paths=new WeakMap();
    const Float64=Float64Array, Bytes=Uint8ClampedArray, finite=Number.isFinite;
    const abs=Math.abs,ceil=Math.ceil,floor=Math.floor,cos=Math.cos,sin=Math.sin,atan2=Math.atan2;
    const native=host.canvas, defer=setTimeout, apply=Reflect.apply, blobFrom=blobBridge.fromBytes;
    /* Array length is a uint32 representation in this engine. Typed-array
       backing buffers enforce their own representation/allocation failures. */
    const ARRAY_LENGTH=0xffffffff;
    const byteProto=Object.getPrototypeOf(Bytes.prototype), byteBuffer=Object.getOwnPropertyDescriptor(byteProto,'buffer').get, byteOffset=Object.getOwnPropertyDescriptor(byteProto,'byteOffset').get;
    class HTMLCanvasElement extends HTMLElement {constructor(){throw new TypeError('Illegal HTMLCanvasElement constructor');}}
    class CanvasRenderingContext2D {constructor(){throw new TypeError('Illegal CanvasRenderingContext2D constructor');}}
    class TextMetrics {constructor(){throw new TypeError('Illegal TextMetrics constructor');}}
    function defaults(s){s.fill='#000000';s.stroke='#000000';s.fillColor=0xff000000;s.strokeColor=0xff000000;s.alpha=1;s.lineWidth=1;s.dash=[];s.dashOffset=0;s.smoothing=true;s.font='10px sans-serif';s.fontSize=10;s.fontStyle=0;s.fontFamily=0;s.align='start';s.baseline='alphabetic';s.direction='inherit';s.matrix=[1,0,0,1,0,0];s.path=[];s.first=null;s.last=null;s.subpathCount=0;s.stack=[];}
    function state(ctx){const s=states.get(ctx);if(!s)throw new TypeError('Illegal CanvasRenderingContext2D receiver');const v=native(s.canvas,'version');if(v!==s.version){defaults(s);s.version=v;}return s;}
    function pathState(value){const s=paths.get(value);return s||state(value);}
    function emptyPath(){return {matrix:[1,0,0,1,0,0],path:[],first:null,last:null,subpathCount:0};}
    function copyPath(s){return {matrix:[1,0,0,1,0,0],path:s.path.slice(),first:s.first&&s.first.slice(),last:s.last&&s.last.slice(),subpathCount:s.subpathCount};}
    function namedError(name,message){return new DOMException(message,name);}
    function args(values,count){if(values.length<count)throw new TypeError('Missing Canvas arguments');return values.slice(0,count).map(number);}
    function valid(values){return values.every(finite);}
    function point(s,x,y){const m=s.matrix;return [m[0]*x+m[2]*y+m[4],m[1]*x+m[3]*y+m[5]];}
    function color(s,stroke=false){const v=stroke?s.strokeColor:s.fillColor;return ((v&0xffffff)|((floor((v>>>24)*s.alpha+0.5)&255)<<24))>>>0;}
    function pathBuffer(points){return new Float64(points).buffer;}
    function append(s,p,move=false){capacity(s,move&&s.path.length?2:1);if(move&&s.path.length)s.path.push(NaN,NaN);s.path.push(p[0],p[1]);s.last=p;if(move||!s.first){s.first=p;s.subpathCount=1;}else s.subpathCount++;}
    function resetPath(s){s.path=[];s.first=null;s.last=null;s.subpathCount=0;}
    function capacity(s,points){if(s.path.length+points*2>ARRAY_LENGTH)throw new RangeError('Canvas path array length representation');}
    function matrixDictionary(value){
        if(value==null)return [1,0,0,1,0,0];
        if(typeof value!=='object'&&typeof value!=='function')throw new TypeError('Expected matrix dictionary');
        const out=[];
        for(const [short,long,fallback] of [['a','m11',1],['b','m12',0],['c','m21',0],['d','m22',1],['e','m41',0],['f','m42',0]]){
            const a=value[short],b=value[long],x=a===undefined?undefined:number(a),y=b===undefined?undefined:number(b);
            if(x!==undefined&&y!==undefined&&x!==y&&!(Number.isNaN(x)&&Number.isNaN(y)))throw new TypeError('Conflicting matrix aliases');
            out.push(x===undefined?(y===undefined?fallback:y):x);
        }
        return out;
    }
    function transformedPath(s,path){const p=paths.get(path);if(!p)throw new TypeError('Expected Path2D');const out=[];for(let i=0;i<p.path.length;i+=2){if(Number.isNaN(p.path[i]))out.push(NaN,p.path[i+1]);else out.push(...point(s,p.path[i],p.path[i+1]));}return out;}
    function ellipsePath(s,x,y,rx,ry,rotation,start,end,ccw){
        const tau=Math.PI*2;let delta=end-start;
        if(!ccw){if(delta>=tau)delta=tau;else delta=((delta%tau)+tau)%tau;}
        else{if(-delta>=tau)delta=-tau;else delta=-(((-delta%tau)+tau)%tau);}
        const steps=Math.max(1,ceil(abs(delta)/tau*48));capacity(s,steps+1);
        const cr=cos(rotation),sr=sin(rotation);
        for(let i=0;i<=steps;i++){const a=start+delta*i/steps,xx=rx*cos(a),yy=ry*sin(a);append(s,point(s,x+cr*xx-sr*yy,y+sr*xx+cr*yy),!s.last);}
    }
    const pathMethods={
        moveTo(...v){pathState(this);v=args(v,2);const s=pathState(this);if(valid(v))append(s,point(s,...v),true);},
        lineTo(...v){pathState(this);v=args(v,2);const s=pathState(this);if(valid(v))append(s,point(s,...v),!s.last);},
        closePath(){const s=pathState(this);if(s.first&&s.last&&s.subpathCount>1){capacity(s,3);append(s,s.first);/* NaN/Infinity marks a closed preceding contour; a following line starts at its first point. */append(s,s.first,true);s.path[s.path.length-3]=Infinity;}},
        rect(...v){pathState(this);v=args(v,4);const s=pathState(this);if(!valid(v))return;capacity(s,8);const [x,y,w,h]=v;append(s,point(s,x,y),true);append(s,point(s,x+w,y));append(s,point(s,x+w,y+h));append(s,point(s,x,y+h));append(s,point(s,x,y));append(s,point(s,x,y),true);s.path[s.path.length-3]=Infinity;},
        quadraticCurveTo(...v){pathState(this);v=args(v,4);const s=pathState(this);if(!valid(v))return;capacity(s,18);const c=point(s,v[0],v[1]),b=point(s,v[2],v[3]);if(!s.last)append(s,c,true);const a=s.last;for(let i=1;i<=16;i++){const t=i/16,u=1-t;append(s,[u*u*a[0]+2*u*t*c[0]+t*t*b[0],u*u*a[1]+2*u*t*c[1]+t*t*b[1]]);}},
        bezierCurveTo(...v){pathState(this);v=args(v,6);const s=pathState(this);if(!valid(v))return;capacity(s,26);const b=point(s,v[0],v[1]),c=point(s,v[2],v[3]),d=point(s,v[4],v[5]);if(!s.last)append(s,b,true);const a=s.last;for(let i=1;i<=24;i++){const t=i/24,u=1-t;append(s,[u*u*u*a[0]+3*u*u*t*b[0]+3*u*t*t*c[0]+t*t*t*d[0],u*u*u*a[1]+3*u*u*t*b[1]+3*u*t*t*c[1]+t*t*t*d[1]]);}},
        arc(...values){pathState(this);const v=args(values,5),s=pathState(this);if(!valid(v))return;if(v[2]<0)throw namedError('IndexSizeError','Negative arc radius');ellipsePath(s,v[0],v[1],v[2],v[2],0,v[3],v[4],!!values[5]);},
        ellipse(...values){pathState(this);const v=args(values,7),s=pathState(this);if(!valid(v))return;if(v[2]<0||v[3]<0)throw namedError('IndexSizeError','Negative ellipse radius');ellipsePath(s,...v,!!values[7]);},
        arcTo(...values){
            pathState(this);const v=args(values,5),s=pathState(this);if(!valid(v))return;const [x1,y1,x2,y2,r]=v;if(r<0)throw namedError('IndexSizeError','Negative arc radius');
            const p1=point(s,x1,y1);if(!s.last){append(s,p1,true);return;}
            const m=s.matrix,det=m[0]*m[3]-m[1]*m[2];if(!det||!finite(det)){append(s,p1);return;}
            const dx=s.last[0]-m[4],dy=s.last[1]-m[5],x0=(dx*m[3]-dy*m[2])/det,y0=(dy*m[0]-dx*m[1])/det;
            let ax=x0-x1,ay=y0-y1,bx=x2-x1,by=y2-y1;const la=Math.hypot(ax,ay),lb=Math.hypot(bx,by),cross=ax*by-ay*bx;
            if(!r||!la||!lb||!cross||!valid([la,lb,cross])){append(s,p1);return;}
            ax/=la;ay/=la;bx/=lb;by/=lb;const dot=Math.max(-1,Math.min(1,ax*bx+ay*by)),angle=Math.acos(dot),distance=r/Math.tan(angle/2),mid=Math.hypot(ax+bx,ay+by);
            const cx=x1+(ax+bx)/mid*r/Math.sin(angle/2),cy=y1+(ay+by)/mid*r/Math.sin(angle/2),tx=x1+ax*distance,ty=y1+ay*distance,ux=x1+bx*distance,uy=y1+by*distance;
            if(!valid([cx,cy,tx,ty,ux,uy])){append(s,p1);return;}capacity(s,51);append(s,point(s,tx,ty));ellipsePath(s,cx,cy,r,r,0,atan2(ty-cy,tx-cx),atan2(uy-cy,ux-cx),cross>0);
        }
    };
    /* SVG endpoint-arc conversion uses the SVG radii correction before
       generating the same real points as ellipse(). */
    function svgArc(s,rx,ry,degrees,large,sweep,x,y){
        const a=s.last;if(!a){append(s,[x,y],true);return;}rx=abs(rx);ry=abs(ry);
        if(!rx||!ry){append(s,[x,y]);return;}if(a[0]===x&&a[1]===y)return;
        const rotation=degrees*Math.PI/180,c=cos(rotation),t=sin(rotation),dx=(a[0]-x)/2,dy=(a[1]-y)/2,xp=c*dx+t*dy,yp=-t*dx+c*dy;
        const scale=xp*xp/(rx*rx)+yp*yp/(ry*ry);if(scale>1){const f=Math.sqrt(scale);rx*=f;ry*=f;}
        const numerator=rx*rx*ry*ry-rx*rx*yp*yp-ry*ry*xp*xp,denominator=rx*rx*yp*yp+ry*ry*xp*xp;
        const f=(large===sweep?-1:1)*Math.sqrt(Math.max(0,numerator/denominator)),cxp=f*rx*yp/ry,cyp=-f*ry*xp/rx,cx=c*cxp-t*cyp+(a[0]+x)/2,cy=t*cxp+c*cyp+(a[1]+y)/2;
        const start=atan2((yp-cyp)/ry,(xp-cxp)/rx),end=atan2((-yp-cyp)/ry,(-xp-cxp)/rx);
        if(valid([rx,ry,cx,cy,start,end])){ellipsePath(s,cx,cy,rx,ry,rotation,start,end,!sweep);s.path[s.path.length-2]=x;s.path[s.path.length-1]=y;s.last=[x,y];}
    }
    function svgPath(receiver,text){
        const s=paths.get(receiver);let at=0,command='',previous='',cx=0,cy=0,first=null,control=null;
        /* Match at the cursor without copying the remaining source once for
           every token: large real SVG paths stay linear in source storage. */
        const scalar=/[+-]?(?:\d+\.?\d*|\.\d+)(?:[eE][+-]?\d+)?/y,arcFlag=/[01]/y;
        const skip=()=>{while(at<text.length&&/[\t\n\f\r ,]/.test(text[at]))at++;};
        const take=(flag=false)=>{skip();const parser=flag?arcFlag:scalar;parser.lastIndex=at;const m=parser.exec(text);if(!m)return null;at=parser.lastIndex;const n=number(m[0]);return finite(n)?n:null;};
        while(at<text.length){skip();if(at===text.length)break;if(/[A-Za-z]/.test(text[at]))command=text[at++];
            const upper=command.toUpperCase(),relative=command!==upper,arity={M:2,L:2,H:1,V:1,C:6,S:4,Q:4,T:2,A:7,Z:0}[upper];
            if(arity===undefined||(!first&&upper!=='M'))break;
            if(upper==='Z'){apply(pathMethods.closePath,receiver,[]);cx=first[0];cy=first[1];control=null;previous=upper;command='';continue;}
            const values=[];for(let i=0;i<arity;i++){const n=take(upper==='A'&&(i===3||i===4));if(n===null)break;values.push(n);}if(values.length!==arity)break;
            const baseX=relative?cx:0,baseY=relative?cy:0,xy=(i)=>[values[i]+baseX,values[i+1]+baseY];let nextControl=null;
            if(upper==='M'||upper==='L'){[cx,cy]=xy(0);apply(pathMethods[upper==='M'?'moveTo':'lineTo'],receiver,[cx,cy]);if(upper==='M'){first=[cx,cy];command=relative?'l':'L';}}
            else if(upper==='H'){cx=values[0]+baseX;apply(pathMethods.lineTo,receiver,[cx,cy]);}
            else if(upper==='V'){cy=values[0]+baseY;apply(pathMethods.lineTo,receiver,[cx,cy]);}
            else if(upper==='C'){const b=xy(0),c=xy(2),d=xy(4);apply(pathMethods.bezierCurveTo,receiver,[...b,...c,...d]);[cx,cy]=d;nextControl=c;}
            else if(upper==='S'){const b=control&&(previous==='C'||previous==='S')?[2*cx-control[0],2*cy-control[1]]:[cx,cy],c=xy(0),d=xy(2);apply(pathMethods.bezierCurveTo,receiver,[...b,...c,...d]);[cx,cy]=d;nextControl=c;}
            else if(upper==='Q'){const b=xy(0),d=xy(2);apply(pathMethods.quadraticCurveTo,receiver,[...b,...d]);[cx,cy]=d;nextControl=b;}
            else if(upper==='T'){const b=control&&(previous==='Q'||previous==='T')?[2*cx-control[0],2*cy-control[1]]:[cx,cy],d=xy(0);apply(pathMethods.quadraticCurveTo,receiver,[...b,...d]);[cx,cy]=d;nextControl=b;}
            else if(upper==='A'){const d=xy(5);svgArc(s,...values.slice(0,5),...d);[cx,cy]=d;}
            previous=upper;control=nextControl;
        }
        if(s.last)append(s,s.last,true);
    }
    class Path2D {
        constructor(value){const source=paths.get(value);paths.set(this,source?copyPath(source):emptyPath());if(value!==undefined&&!source)svgPath(this,string(value));}
        addPath(path,transform){const s=paths.get(this),source=paths.get(path);if(!s||!source)throw new TypeError('Expected Path2D receiver and path');const matrix=matrixDictionary(transform);if(!valid(matrix)||!source.path.length)return;
            const copy=transformedPath({matrix},path);capacity(s,copy.length/2+(s.path.length?1:0)+2);if(s.path.length)s.path.push(NaN,NaN);for(let i=0;i<copy.length;i++)s.path.push(copy[i]);
            if(source.first)s.first=point({matrix},...source.first);if(source.last){s.last=point({matrix},...source.last);append(s,s.last,true);}}
    }
    function strokeGeometry(s,points){
        /* Trace in the current pen's coordinate system, then apply the CTM.
           Existing default-path vertices already contain their creation-time
           transform; an inverse here changes the pen, not that saved path. */
        const m=s.matrix,scale=Math.max(abs(m[0]),abs(m[1]),abs(m[2]),abs(m[3]));
        if(!finite(scale)||!scale)return {points:[],width:s.lineWidth};
        const a=m[0]/scale,b=m[1]/scale,c=m[2]/scale,d=m[3]/scale,det=a*d-b*c;
        if(!finite(det)||!det)return {points:[],width:s.lineWidth};
        const local=[];
        for(let i=0;i<points.length;i+=2){
            if(Number.isNaN(points[i])){local.push(NaN,points[i+1]);continue;}
            const x=points[i]/scale-m[4]/scale,y=points[i+1]/scale-m[5]/scale;
            const xx=(d*x-c*y)/det,yy=(a*y-b*x)/det;
            if(!finite(xx)||!finite(yy))throw new RangeError('Canvas stroke transform limit');local.push(xx,yy);
        }
        const pen={matrix:[1,0,0,1,0,0],lineWidth:s.lineWidth,dash:s.dash,dashOffset:s.dashOffset};
        if(!s.dash.some(v=>v>0))return canvasDash.prepare(pen,local);
        const out=[];let first=0;
        while(first<local.length){
            while(first<local.length&&Number.isNaN(local[first]))first+=2;
            let end=first;while(end<local.length&&!Number.isNaN(local[end]))end+=2;
            if(end===first)break;
            const input=local.slice(first,end),stroke=canvasDash.prepare(pen,input);let dashed=stroke.points;
            /* A dash crossing a closed contour's origin is one connected run,
               not two butt caps. Merge the last/first runs without mutating
               the source path or the dash state. */
            if(local[end+1]===Infinity&&dashed.length>=4&&dashed[0]===input[0]&&dashed[1]===input[1]&&dashed[dashed.length-2]===input[0]&&dashed[dashed.length-1]===input[1]){
                let split=-1,last=-1;for(let i=0;i<dashed.length;i+=2)if(Number.isNaN(dashed[i])){if(split<0)split=i;last=i;}
                if(split<0)dashed=[...dashed,NaN,Infinity];
                else dashed=[...dashed.slice(last+2),...dashed.slice(2,split),...dashed.slice(split,last)];
            }
            if(dashed.length){if(out.length+dashed.length+(out.length?2:0)>ARRAY_LENGTH)throw new RangeError('Canvas path array length representation');if(out.length)out.push(NaN,NaN);for(let i=0;i<dashed.length;i++)out.push(dashed[i]);}
            first=end+2;
        }
        return {points:out,width:s.lineWidth};
    }
    function strokePath(s,points){if(!points.length)return;const stroke=strokeGeometry(s,points);if(stroke.points.length)native(s.canvas,'stroke',pathBuffer(stroke.points),color(s,true),stroke.width,new Float64(s.matrix).buffer);}
    function hitArguments(ctx,values,fill){
        state(ctx);const external=paths.has(values[0]),start=external?1:0;
        if(values.length<start+2)throw new TypeError('Missing Canvas hit-test arguments');
        const x=+values[start],y=+values[start+1];
        const rule=fill?string(values[start+2]===undefined?'nonzero':values[start+2]):'nonzero';
        if(rule!=='nonzero'&&rule!=='evenodd')throw new TypeError('Invalid fill rule');
        /* Conversions can resize the canvas. Refresh before borrowing its
           current path/state; the query point never receives the CTM. */
        const s=state(ctx);return {s,x,y,rule,points:external?transformedPath(s,values[0]):s.path};
    }
    function multiply(s,m){const a=s.matrix;s.matrix=[a[0]*m[0]+a[2]*m[1],a[1]*m[0]+a[3]*m[1],a[0]*m[2]+a[2]*m[3],a[1]*m[2]+a[3]*m[3],a[0]*m[4]+a[2]*m[5]+a[4],a[1]*m[4]+a[3]*m[5]+a[5]];}
    function alignment(s){if(s.align==='center')return 0.5;if(s.align==='right')return 1;if(s.align==='left')return 0;const rtl=s.direction==='rtl'||(s.direction==='inherit'&&native(s.canvas,'rtl'));return s.align==='start'?(rtl?1:0):(rtl?0:1);}
    const baselines=['alphabetic','top','hanging','middle','ideographic','bottom'];
    function textValue(value){return string(value).replace(/[\t\n\f\r]/g,' ');}
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
    define(HTMLCanvasElement.prototype,'toDataURL',{configurable:true,writable:true,value:function(type='image/png',quality){htmlElementBrand(this,'canvas');string(type);const result=native(this,'dataURL');if(result===false)throw namedError('SecurityError','Canvas is not origin-clean');return result;}});
    define(HTMLCanvasElement.prototype,'toBlob',{configurable:true,writable:true,value:function(callback,type='image/png',quality){htmlElementBrand(this,'canvas');if(typeof callback!=='function')throw new TypeError('toBlob requires a callback');string(type);const bytes=native(this,'png');if(bytes===false)throw namedError('SecurityError','Canvas is not origin-clean');defer(()=>{let blob=null;if(bytes!==null){try{blob=blobFrom(bytes,'image/png');}catch(e){/* A failed serialization/Blob allocation reports null, never a fake Blob. */}}apply(callback,undefined,[blob]);},0);}});
    define(CanvasRenderingContext2D.prototype,'canvas',{enumerable:true,configurable:true,get(){return state(this).canvas;}});
    for(const [property,key,colorkey] of [['fillStyle','fill','fillColor'],['strokeStyle','stroke','strokeColor']])define(CanvasRenderingContext2D.prototype,property,{enumerable:true,configurable:true,
        get(){return state(this)[key];},set(value){const s=state(this),text=string(value),c=native(s.canvas,'color',text);if(c!==null){s[key]=text;s[colorkey]=c>>>0;}}});
    define(CanvasRenderingContext2D.prototype,'globalAlpha',{enumerable:true,configurable:true,get(){return state(this).alpha;},set(value){const s=state(this),v=number(value);if(finite(v)&&v>=0&&v<=1)s.alpha=v;}});
    define(CanvasRenderingContext2D.prototype,'lineWidth',{enumerable:true,configurable:true,get(){return state(this).lineWidth;},set(value){const s=state(this),v=number(value);if(finite(v)&&v>0)s.lineWidth=v;}});
    define(CanvasRenderingContext2D.prototype,'lineDashOffset',{enumerable:true,configurable:true,get(){return state(this).dashOffset;},set(value){state(this);const v=+value,s=state(this);if(finite(v))s.dashOffset=v;}});
    define(CanvasRenderingContext2D.prototype,'imageSmoothingEnabled',{enumerable:true,configurable:true,get(){return state(this).smoothing;},set(value){state(this).smoothing=!!value;}});
    define(CanvasRenderingContext2D.prototype,'font',{enumerable:true,configurable:true,get(){return state(this).font;},set(value){state(this);const text=string(value),s=state(this),parsed=native(s.canvas,'font',text);if(parsed!==null){s.font=text;s.fontSize=parsed[0];s.fontStyle=parsed[1];s.fontFamily=parsed[2];}}});
    for(const [property,key,allowed] of [['textAlign','align',['start','end','left','right','center']],['textBaseline','baseline',baselines],['direction','direction',['inherit','ltr','rtl']]])define(CanvasRenderingContext2D.prototype,property,{enumerable:true,configurable:true,get(){return state(this)[key];},set(value){state(this);const text=string(value),s=state(this);if(allowed.includes(text))s[key]=text;}});
    /* This subset implements source-over only; never reflect an unsupported
       compositing operator as though it changed the actual native drawing. */
    define(CanvasRenderingContext2D.prototype,'globalCompositeOperation',{enumerable:true,configurable:true,get(){state(this);return 'source-over';},set(value){state(this);string(value);}});
    const methods={
        ...pathMethods,
        setLineDash(segments){state(this);if(!arguments.length)throw new TypeError('setLineDash requires segments');const dash=canvasDash.convert(segments),s=state(this);if(dash!==null)s.dash=dash;},
        getLineDash(){return canvasDash.copy(state(this).dash);},
        fillRect(...v){rect(this,'rect',v);},clearRect(...v){rect(this,'clear',v);},
        strokeRect(...v){state(this);v=args(v,4);const s=state(this);if(!valid(v))return;const [x,y,w,h]=v,p=[point(s,x,y),point(s,x+w,y),point(s,x+w,y+h),point(s,x,y+h),point(s,x,y)];strokePath(s,[...p.flat(),NaN,Infinity]);},
        save(){const s=state(this);const saved={fill:s.fill,stroke:s.stroke,fillColor:s.fillColor,strokeColor:s.strokeColor,alpha:s.alpha,lineWidth:s.lineWidth,dash:canvasDash.copy(s.dash),dashOffset:s.dashOffset,smoothing:s.smoothing,font:s.font,fontSize:s.fontSize,fontStyle:s.fontStyle,fontFamily:s.fontFamily,align:s.align,baseline:s.baseline,direction:s.direction,matrix:s.matrix.slice()};s.stack.push(saved);try{native(s.canvas,'save');}catch(e){s.stack.pop();throw e;}},
        restore(){const s=state(this),saved=s.stack[s.stack.length-1];if(saved){native(s.canvas,'restore');s.stack.pop();Object.assign(s,saved);}},
        reset(){const s=state(this);native(s.canvas,'reset');s.version=native(s.canvas,'version');defaults(s);},
        beginPath(){resetPath(state(this));},
        fill(pathOrRule='nonzero',rule='nonzero'){state(this);const external=paths.has(pathOrRule);rule=string(external?rule:pathOrRule);if(rule!=='nonzero'&&rule!=='evenodd')throw new TypeError('Invalid fill rule');const s=state(this),path=external?transformedPath(s,pathOrRule):s.path;if(path.length)native(s.canvas,'poly',pathBuffer(path),color(s),rule==='evenodd',false);},
        clip(pathOrRule='nonzero',rule='nonzero'){state(this);const external=paths.has(pathOrRule);rule=string(external?rule:pathOrRule);if(rule!=='nonzero'&&rule!=='evenodd')throw new TypeError('Invalid fill rule');const s=state(this),path=external?transformedPath(s,pathOrRule):s.path;native(s.canvas,'clip',pathBuffer(path),0,rule==='evenodd');},
        stroke(path){const s=state(this),points=path===undefined?s.path:transformedPath(s,path);strokePath(s,points);},
        isPointInPath(...values){const hit=hitArguments(this,values,true);if(!finite(hit.x)||!finite(hit.y))return false;return native(hit.s.canvas,'hitPath',pathBuffer(hit.points),hit.x,hit.y,hit.rule==='evenodd');},
        isPointInStroke(...values){const hit=hitArguments(this,values,false);if(!finite(hit.x)||!finite(hit.y)||!hit.points.length)return false;const stroke=strokeGeometry(hit.s,hit.points);return native(hit.s.canvas,'hitStroke',pathBuffer(stroke.points),hit.x,hit.y,stroke.width,new Float64(hit.s.matrix).buffer);},
        measureText(value){state(this);if(!arguments.length)throw new TypeError('measureText requires text');const text=textValue(value),s=state(this),result=native(s.canvas,'measureText',text,s.fontSize,s.fontStyle,alignment(s),baselines.indexOf(s.baseline),s.fontFamily);const out=create(TextMetrics.prototype);metrics.set(out,result);return out;},
        fillText(value,x,y,maxWidth){state(this);if(arguments.length<3)throw new TypeError('fillText requires text and coordinates');const text=textValue(value),v=[number(x),number(y)],width=arguments.length>3?number(maxWidth):Infinity;if(!valid(v)||!(width>0))return;const s=state(this);native(s.canvas,'fillText',text,s.fontSize,s.fontStyle,alignment(s),baselines.indexOf(s.baseline),s.fontFamily,new Float64([...v,width,...s.matrix]).buffer,color(s));},
        transform(...v){const s=state(this);v=args(v,6);if(valid(v))multiply(s,v);},
        setTransform(...v){const s=state(this);if(v.length===0){s.matrix=[1,0,0,1,0,0];return;}if(v.length===1&&typeof v[0]==='object'){const m=v[0];v=[m.a??1,m.b??0,m.c??0,m.d??1,m.e??0,m.f??0];}v=args(v,6);if(valid(v))s.matrix=v;},
        resetTransform(){state(this).matrix=[1,0,0,1,0,0];},
        translate(...v){const s=state(this);v=args(v,2);if(valid(v))multiply(s,[1,0,0,1,v[0],v[1]]);},
        scale(...v){const s=state(this);v=args(v,2);if(valid(v))multiply(s,[v[0],0,0,v[1],0,0]);},
        rotate(value){const s=state(this),v=number(value);if(finite(v)){const c=cos(v),t=sin(v);multiply(s,[c,t,-t,c,0,0]);}},
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
            native(s.canvas,'put',dx,dy,w,h,apply(byteBuffer,data,[]),apply(byteOffset,data,[]));},
        getContextAttributes(){state(this);return {alpha:true,colorSpace:'srgb',desynchronized:false,willReadFrequently:false};}
    };
    for(const [name,value] of Object.entries(methods))define(CanvasRenderingContext2D.prototype,name,{value:pathMethods[name]?function(...args){state(this);return apply(value,this,args);}:value,writable:true,configurable:true});
    for(const [name,value] of Object.entries(pathMethods))define(Path2D.prototype,name,{value:function(...args){if(!paths.has(this))throw new TypeError('Illegal Path2D receiver');return apply(value,this,args);},writable:true,configurable:true});
    class ImageData {
        constructor(a,b,c){let data,w,h;if(a instanceof Bytes){data=a;w=number(b)>>>0;h=c===undefined?data.length/4/w:number(c)>>>0;if(!w||!finite(h)||h!==floor(h)||h<1||data.length!==w*h*4)throw namedError('IndexSizeError','Invalid ImageData dimensions');}
            else{w=abs(Math.trunc(number(a)));h=abs(Math.trunc(number(b)));if(!finite(w)||!finite(h)||!w||!h||w>ARRAY_LENGTH||h>ARRAY_LENGTH)throw namedError('IndexSizeError','Invalid ImageData dimensions');if(!Number.isSafeInteger(w*h*4))throw new RangeError('ImageData byte size representation');data=new Bytes(w*h*4);}
            images.set(this,{data,w,h});}
        get width(){const s=images.get(this);if(!s)throw new TypeError('Illegal ImageData receiver');return s.w;}
        get height(){const s=images.get(this);if(!s)throw new TypeError('Illegal ImageData receiver');return s.h;}
        get data(){const s=images.get(this);if(!s)throw new TypeError('Illegal ImageData receiver');return s.data;}
        get colorSpace(){if(!images.has(this))throw new TypeError('Illegal ImageData receiver');return 'srgb';}
    }
    for(const [i,name] of ['width','actualBoundingBoxLeft','actualBoundingBoxRight','actualBoundingBoxAscent','actualBoundingBoxDescent','fontBoundingBoxAscent','fontBoundingBoxDescent'].entries())define(TextMetrics.prototype,name,{enumerable:true,configurable:true,get(){const values=metrics.get(this);if(!values)throw new TypeError('Illegal TextMetrics receiver');return values[i];}});
    for(const C of [HTMLCanvasElement,CanvasRenderingContext2D,ImageData,TextMetrics,Path2D])define(C.prototype,Symbol.toStringTag,{value:C.name,configurable:true});
    Object.assign(globalThis,{HTMLCanvasElement,CanvasRenderingContext2D,ImageData,TextMetrics,Path2D});
    return {nodeProtos:[HTMLCanvasElement.prototype]};
})();
