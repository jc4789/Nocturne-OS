/* Native Canvas stroke accepts at most 8192 points, including NaN subpath
 * separators. Prepare the entire dashed path before any native draw. */
const canvasDash = (() => {
    'use strict';
    const finite=Number.isFinite, nan=Number.isNaN, hypot=Math.hypot, abs=Math.abs, min=Math.min, max=Math.max;
    const TypeErrorImpl=TypeError, RangeErrorImpl=RangeError, DOMExceptionImpl=DOMException;
    const POINTS=8192, PATTERN=256, STEPS=32768;
    function limit(){throw new RangeErrorImpl('Canvas dash tessellation limit');}
    function copy(values){const out=[];for(let i=0;i<values.length;i++)out[i]=values[i];return out;}
    function convert(value){
        if(value===null || (typeof value!=='object' && typeof value!=='function'))
            throw new TypeErrorImpl('Line dash requires an iterable sequence');
        const out=[];let invalid=false;
        for(const entry of value){
            if(out.length===PATTERN)limit();
            const n=+entry;out[out.length]=n;if(!finite(n)||n<0)invalid=true;
        }
        if(invalid)return null;
        if(out.length&1){const count=out.length;if(count*2>PATTERN)limit();for(let i=0;i<count;i++)out[count+i]=out[i];}
        return out;
    }
    function uniform(matrix){
        const a=hypot(matrix[0],matrix[1]),b=hypot(matrix[2],matrix[3]);
        if(!finite(a)||!finite(b))limit();
        if(!a&&!b)return 0;
        if(!a||!b||abs(a-b)>max(a,b)*1e-10)return null;
        const dot=matrix[0]/a*(matrix[2]/b)+matrix[1]/a*(matrix[3]/b);
        return abs(dot)<=1e-10?a:null;
    }
    function prepare(state,points){
        if(points.length&1 || points.length>POINTS*2)limit();
        const source=state.dash,scale=uniform(state.matrix);
        let total=0,on=false,off=false;
        for(let i=0;i<source.length;i++){total+=source[i];if(i&1)off=off||source[i]>0;else on=on||source[i]>0;}
        if(!finite(total))limit();
        if(scale===null){
            if(total)throw new DOMExceptionImpl('Nonuniform Canvas dash transform','NotSupportedError');
            /* Retain the existing solid-stroke subset's device-space width. */
            return {points,width:state.lineWidth};
        }
        if(!scale)return {points:[],width:0};
        const width=state.lineWidth*scale;if(!finite(width)||!width)limit();
        if(!total || !off)return {points,width};
        if(!on)return {points:[],width}; /* Zero-length on intervals: butt caps paint nothing. */
        const pattern=[];let cycle=0;
        for(let i=0;i<source.length;i++){
            const n=source[i]*scale;if(!finite(n)||(source[i]>0&&!n))limit();pattern[i]=n;cycle+=n;
        }
        if(!finite(cycle)||!cycle)limit();
        const phase=(state.dashOffset%total)*scale;
        if(!finite(phase))limit();
        const offset=phase<0?phase+cycle:phase;
        const out=[];let steps=0,index=0,left=0,pen=false,previous=null;
        const step=()=>{if(++steps>STEPS)limit();};
        const add=(x,y)=>{
            if(!finite(x)||!finite(y))limit();
            if(out.length>=2 && out[out.length-2]===x && out[out.length-1]===y)return;
            if(out.length+2>POINTS*2)limit();out[out.length]=x;out[out.length]=y;
        };
        const advance=()=>{
            do{step();index=(index+1)%pattern.length;left=pattern[index];}while(!left);
        };
        const restart=()=>{
            index=0;let at=offset;
            for(;;){step();left=pattern[index];if(left>at){left-=at;break;}at-=left;index=(index+1)%pattern.length;}
            pen=false;
        };
        for(let i=0;i<points.length;i+=2){
            step();const x=points[i],y=points[i+1];
            if(nan(x)&&nan(y)){previous=null;pen=false;continue;}
            if(!finite(x)||!finite(y))limit();
            if(!previous){previous=[x,y];restart();continue;}
            const ax=previous[0],ay=previous[1],dx=x-ax,dy=y-ay,length=hypot(dx,dy);previous=[x,y];
            if(!finite(length))limit();if(!length)continue;
            let at=0;
            while(at<length){
                step();const count=min(left,length-at),end=at+count;
                if(!(count>0)||!(end>at))limit();
                if(!(index&1)){
                    if(!pen){
                        if(out.length){if(out.length+2>POINTS*2)limit();out[out.length]=NaN;out[out.length]=NaN;}
                        add(at?ax+dx*(at/length):ax,at?ay+dy*(at/length):ay);pen=true;
                    }
                    add(end===length?x:ax+dx*(end/length),end===length?y:ay+dy*(end/length));
                }else pen=false;
                at=end;left-=count;if(left<=0)advance();
            }
        }
        return {points:out,width};
    }
    return {convert,copy,prepare};
})();
