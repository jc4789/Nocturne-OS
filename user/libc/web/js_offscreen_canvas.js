/* OffscreenCanvas shared by Window and Worker realms. A realm with a
 * raster backend (the Window bootstrap) attaches it from js_canvas.js; the
 * backing is a detached native canvas node, or the placeholder element after
 * transferControlToOffscreen. Transfers move the size and ownership; a Worker
 * realm has no raster backend, so its 2D context is unavailable (null) and
 * its blob encoding rejects with NotSupportedError. */
const offscreenCanvasBridge = (() => {
    'use strict';
    const states=new WeakMap(), define=Object.defineProperty;
    const DomErr=DOMException, PromiseCtor=Promise, finite=Number.isFinite, trunc=Math.trunc, safe=Number.isSafeInteger;
    let raster=null;
    function state(value){const s=states.get(value);if(!s)throw new TypeError('Illegal OffscreenCanvas receiver');return s;}
    function live(s){if(s.detached)throw new DomErr('The OffscreenCanvas is detached','InvalidStateError');return s;}
    function dimension(value){
        /* [EnforceRange] unsigned long long */
        value=+value;if(!finite(value))throw new TypeError('OffscreenCanvas dimension must be finite');
        value=trunc(value);if(value<0||value>Number.MAX_SAFE_INTEGER)throw new TypeError('OffscreenCanvas dimension outside unsigned long long');
        return value===0?0:value;
    }
    function make(width,height,element){
        const value=new OffscreenCanvas(width,height);
        states.get(value).element=element;
        return value;
    }
    function backing(s){
        if(!s.element&&raster)s.element=raster.create(s.width,s.height);
        return s.element;
    }
    class OffscreenCanvas extends EventTarget {
        constructor(width,height){
            if(arguments.length<2)throw new TypeError('OffscreenCanvas requires width and height');
            super();width=dimension(width);height=dimension(height);
            states.set(this,{width,height,element:null,detached:false,mode:null,context:null});
        }
        get width(){return state(this).width;}
        set width(value){const s=state(this);value=dimension(value);live(s);s.width=value;if(s.element)raster.resize(s.element,'width',value);}
        get height(){return state(this).height;}
        set height(value){const s=state(this);value=dimension(value);live(s);s.height=value;if(s.element)raster.resize(s.element,'height',value);}
        getContext(type,options){
            const s=state(this);if(!arguments.length)throw new TypeError('getContext requires a context type');
            type=String(type);
            if(type!=='2d'&&type!=='bitmaprenderer'&&type!=='webgl'&&type!=='webgl2'&&type!=='webgpu')
                throw new TypeError('Invalid OffscreenCanvas context type');
            live(s);
            if(s.mode!==null)return s.mode===type?s.context:null;
            /* Only Canvas2D has a raster implementation; other modes are not
               fabricated and leave the canvas without a context mode. */
            if(type!=='2d'||!raster)return null;
            const element=backing(s),ctx=element?raster.context(element,this):null;
            if(!ctx)return null;
            s.mode=type;s.context=ctx;return ctx;
        }
        transferToImageBitmap(){
            const s=live(state(this));
            if(s.mode===null)throw new DomErr('The OffscreenCanvas has no rendering context','InvalidStateError');
            return raster.bitmap(s.element);
        }
        convertToBlob(options){
            let s,type='image/png';
            try{
                s=state(this);
                if(options!==undefined&&options!==null){
                    if(typeof options!=='object'&&typeof options!=='function')throw new TypeError('ImageEncodeOptions must be a dictionary');
                    /* Dictionary members convert in lexicographic order. */
                    const quality=options.quality;if(quality!==undefined)Number(quality);
                    const requested=options.type;if(requested!==undefined)type=String(requested);
                }
                live(s);
                if(!s.width||!s.height)throw new DomErr('The OffscreenCanvas has no pixels','IndexSizeError');
                if(!raster)throw new DomErr('This realm has no OffscreenCanvas encoder','NotSupportedError');
            }catch(error){return PromiseCtor.reject(error);}
            /* Snapshot now; later drawing must not alter the result. */
            let blob;
            try{blob=raster.blob(backing(s));}catch(error){return PromiseCtor.reject(error);}
            return new PromiseCtor((resolve,reject)=>{
                setTimeout(()=>{if(blob===null)reject(new DomErr('The OffscreenCanvas could not be encoded','EncodingError'));else resolve(blob);},0);
            });
        }
    }
    for(const name of ['oncontextlost','oncontextrestored']){
        const handlers=new WeakMap();
        define(OffscreenCanvas.prototype,name,{configurable:true,enumerable:true,
            get(){state(this);return handlers.get(this)??null;},
            set(value){state(this);const old=handlers.get(this),type=name.slice(2);
                if(old)this.removeEventListener(type,old);
                if(typeof value==='function'){handlers.set(this,value);this.addEventListener(type,value);}else handlers.delete(this);}});
    }
    define(OffscreenCanvas.prototype,Symbol.toStringTag,{value:'OffscreenCanvas',configurable:true});
    function fail(){throw new DomErr('The OffscreenCanvas cannot be transferred','DataCloneError');}
    function detachWrites(s){
        return [{object:s,key:'detached',value:true},{object:s,key:'width',value:0},{object:s,key:'height',value:0},{object:s,key:'element',value:null}];
    }
    cloneData.registerTransfer({
        name:'OffscreenCanvas',
        brand:value=>states.has(value),
        validate(value){const s=state(value);if(s.detached||s.mode!==null)fail();},
        /* Same realm: the receiver takes over the raster backing (and any
           placeholder link) with the source's size. */
        prepare(value){const s=state(value);return {value:make(s.width,s.height,s.element)};},
        /* Another realm: only the size crosses; the receiving realm builds its
           own canvas from the metadata. */
        prepareWorker(value){const s=state(value);return {receiver:{value:null},meta:['OffscreenCanvas',s.width,s.height],route:null};},
        prepareExternal(value){const s=state(value);return {receiver:{value:null},meta:['OffscreenCanvas',s.width,s.height],route:null};},
        commitPlan(value){const s=state(value);if(s.detached)fail();return {writes:detachWrites(s)};},
        commit(value){const s=state(value),writes=detachWrites(s);for(let i=0;i<writes.length;i++)writes[i].object[writes[i].key]=writes[i].value;}
    });
    globalThis.OffscreenCanvas=OffscreenCanvas;
    return {
        OffscreenCanvas,
        attachRaster(api){raster=api;},
        /* The live backing node for drawImage/createImageBitmap, or null. */
        backing(value){const s=states.get(value);if(!s)return undefined;if(s.detached)return null;return backing(s);},
        placeholder(element,width,height){return make(width,height,element);},
        isMeta(m){return Array.isArray(m)&&m[0]==='OffscreenCanvas';},
        receive(m){
            const width=m[1],height=m[2];
            if(m.length!==3||!safe(width)||width<0||!safe(height)||height<0)throw new TypeError('Invalid OffscreenCanvas transfer');
            return {value:make(width,height,null)};
        }
    };
})();
