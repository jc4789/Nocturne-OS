/* OffscreenCanvas shared by Window and Worker realms. A realm with a
 * raster backend (the Window bootstrap) attaches it from js_canvas.js; the
 * backing is a detached native canvas node, or the placeholder element after
 * transferControlToOffscreen. Transfers move the size and ownership; a Worker
 * realm has no raster backend, so its 2D context is unavailable (null) and
 * its blob encoding rejects with NotSupportedError.
 *
 * WebGL contexts work in both realms (native Mesa softpipe). A placeholder
 * sent to a Worker carries a frame link: shared memory whose header is
 * [serial, width, height, consumed] followed by straight 0xAARRGGBB rows.
 * The Worker presents a frame after rendering changed the drawing buffer,
 * once the Window consumed the previous one; the Window copies each new
 * frame into the placeholder canvas. */
const offscreenCanvasBridge = (() => {
    'use strict';
    const states=new WeakMap(), define=Object.defineProperty;
    const DomErr=DOMException, PromiseCtor=Promise, finite=Number.isFinite, trunc=Math.trunc, safe=Number.isSafeInteger, min=Math.min, apply=Reflect.apply;
    const glCreate=host.webglCreate, glResize=host.webglResize, glPresent=host.webglPresent, glBind=host.webglBind;
    const reserveShared=host.sharedBufferReserve, exportShared=host.sharedBufferExport, importShared=host.sharedBufferImport;
    const waitAsync=Atomics.waitAsync, atomicLoad=Atomics.load, atomicStore=Atomics.store, atomicNotify=Atomics.notify;
    const Int32=Int32Array, Uint32=Uint32Array, promiseThen=PromiseCtor.prototype.then, ArrayCtor=Array;
    /* Drawing buffers are clamped to 8192 per side natively. */
    const FRAME_HEADER=4096, FRAME_SIDE=8192, FRAME_BYTES=FRAME_HEADER+FRAME_SIDE*FRAME_SIDE*4;
    let raster=null;
    const eventMessages=new WeakMap();
    class WebGLContextEvent extends Event {
        constructor(type,init){
            if(!arguments.length)throw new TypeError('WebGLContextEvent requires a type');
            super(type,init);let message='';
            if(init!==undefined&&init!==null){
                if(typeof init!=='object'&&typeof init!=='function')throw new TypeError('WebGLContextEventInit must be a dictionary');
                const value=init.statusMessage;if(value!==undefined)message=String(value);
            }
            eventMessages.set(this,message);
        }
        get statusMessage(){const message=eventMessages.get(this);if(message===undefined)throw new TypeError('Illegal WebGLContextEvent receiver');return message;}
    }
    define(WebGLContextEvent.prototype,Symbol.toStringTag,{value:'WebGLContextEvent',configurable:true});
    globalThis.WebGLContextEvent=WebGLContextEvent;
    function webglVersion(type){return type==='webgl'||type==='experimental-webgl'?1:type==='webgl2'?2:0;}
    function waitChange(header,index,value,then){
        const result=waitAsync(header,index,value);
        if(result.async)apply(promiseThen,result.value,[then]);else then();
    }
    /* Window side: copy each frame the Worker publishes into the placeholder. */
    function showFrames(element,buffer){
        const header=new Int32(buffer,0,4);let seen=atomicLoad(header,0);
        const step=()=>{
            const serial=atomicLoad(header,0);
            if(serial!==seen){
                seen=serial;const width=header[1],height=header[2];
                if(width>0&&height>0&&width<=FRAME_SIDE&&height<=FRAME_SIDE&&raster)raster.frame(element,buffer,FRAME_HEADER,width,height);
                atomicStore(header,3,serial);atomicNotify(header,3);
            }
            waitChange(header,0,seen,step);
        };
        waitChange(header,0,seen,step);
    }
    /* Worker side: publish the drawing buffer once the last frame was shown. */
    function present(s){
        const link=s.link;if(!link||s.mode===null||s.detached)return;
        const header=link.header,serial=atomicLoad(header,0);
        if(atomicLoad(header,3)!==serial){
            if(!link.waiting){link.waiting=true;waitChange(header,3,atomicLoad(header,3),()=>{link.waiting=false;present(s);});}
            return;
        }
        const width=min(s.width,FRAME_SIDE),height=min(s.height,FRAME_SIDE);
        if(!width||!height)return;
        if(!glPresent(s.context,new Uint32(link.buffer,FRAME_HEADER,width*height),width,height))return;
        header[1]=width;header[2]=height;atomicStore(header,0,(serial+1)|0);atomicNotify(header,0);
    }
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
    /* A backing node follows the size; a node-less WebGL context (Worker)
       reallocates its drawing buffer. */
    function resized(s,name){
        if(s.element)raster.resize(s.element,name,s[name]);
        else if(s.context&&s.mode!=='2d')glResize(s.context,min(s.width,4294967295),min(s.height,4294967295));
    }
    function backing(s){
        if(!s.element&&raster)s.element=raster.create(s.width,s.height);
        return s.element;
    }
    class OffscreenCanvas extends EventTarget {
        constructor(width,height){
            if(arguments.length<2)throw new TypeError('OffscreenCanvas requires width and height');
            super();width=dimension(width);height=dimension(height);
            states.set(this,{width,height,element:null,detached:false,mode:null,context:null,link:null,placeholder:false});
        }
        get width(){return state(this).width;}
        set width(value){const s=state(this);value=dimension(value);live(s);s.width=value;resized(s,'width');}
        get height(){return state(this).height;}
        set height(value){const s=state(this);value=dimension(value);live(s);s.height=value;resized(s,'height');}
        getContext(type,options){
            const s=state(this);if(!arguments.length)throw new TypeError('getContext requires a context type');
            type=String(type);
            if(type!=='2d'&&type!=='bitmaprenderer'&&type!=='webgl'&&type!=='webgl2'&&type!=='webgpu')
                throw new TypeError('Invalid OffscreenCanvas context type');
            live(s);
            if(s.mode!==null)return s.mode===type?s.context:null;
            const version=webglVersion(type);
            if(version){
                let ctx=null;
                if(raster){const element=backing(s);ctx=element?raster.webgl(element,version,options,this):null;}
                else if(glCreate)ctx=glCreate(this,version,options,min(s.width,4294967295),min(s.height,4294967295),s.link?()=>present(s):undefined);
                if(!ctx)return null;
                s.mode=type;s.context=ctx;return ctx;
            }
            /* Canvas2D needs the raster backend; bitmaprenderer and webgpu are
               not fabricated and leave the canvas without a context mode. */
            if(type!=='2d'||!raster)return null;
            const element=backing(s),ctx=element?raster.context(element,this):null;
            if(!ctx)return null;
            s.mode=type;s.context=ctx;return ctx;
        }
        transferToImageBitmap(){
            const s=live(state(this));
            if(s.mode===null)throw new DomErr('The OffscreenCanvas has no rendering context','InvalidStateError');
            if(!raster||!s.element)throw new DomErr('This realm has no ImageBitmap support','NotSupportedError');
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
        return [{object:s,key:'detached',value:true},{object:s,key:'width',value:0},{object:s,key:'height',value:0},{object:s,key:'element',value:null},{object:s,key:'link',value:null}];
    }
    /* The frame link record for a receiving worker: an existing link is passed
       on; a placeholder going to a Worker gets a new one shown here. */
    function linkRecord(s,worker){
        if(s.link){try{return exportShared(s.link.buffer);}catch(_){return null;}}
        if(!worker||!s.placeholder||!s.element||!raster||!reserveShared)return null;
        const buffer=reserveShared(FRAME_BYTES),record=exportShared(buffer);
        showFrames(s.element,buffer);return record;
    }
    cloneData.registerTransfer({
        name:'OffscreenCanvas',
        brand:value=>states.has(value),
        validate(value){const s=state(value);if(s.detached||s.mode!==null)fail();},
        /* Same realm: the receiver takes over the raster backing (and any
           placeholder link) with the source's size. */
        prepare(value){const s=state(value),moved=make(s.width,s.height,s.element),t=states.get(moved);t.link=s.link;t.placeholder=s.placeholder;return {value:moved};},
        /* Another realm: the size and any frame link cross; the receiving
           realm builds its own canvas from the metadata. */
        prepareWorker(value){const s=state(value);return {receiver:{value:null},meta:['OffscreenCanvas',s.width,s.height,linkRecord(s,true)],route:null};},
        prepareExternal(value){const s=state(value);return {receiver:{value:null},meta:['OffscreenCanvas',s.width,s.height,s.link?s.link.buffer:null],route:null};},
        commitPlan(value){const s=state(value);if(s.detached)fail();return {writes:detachWrites(s)};},
        commit(value){const s=state(value),writes=detachWrites(s);for(let i=0;i<writes.length;i++)writes[i].object[writes[i].key]=writes[i].value;}
    });
    globalThis.OffscreenCanvas=OffscreenCanvas;
    return {
        OffscreenCanvas,
        attachRaster(api){raster=api;},
        webglVersion,
        /* Realm services for native WebGL: TexImageSource resolution and the
           task queue for context loss events. */
        bindWebGL(source,defer){if(glBind)glBind({source,event:WebGLContextEvent,defer,DOMException:DomErr});},
        /* The live backing node for drawImage/createImageBitmap, or null. */
        backing(value){const s=states.get(value);if(!s)return undefined;if(s.detached)return null;return backing(s);},
        placeholder(element,width,height){const value=make(width,height,element);states.get(value).placeholder=true;return value;},
        isMeta(m){return ArrayCtor.isArray(m)&&m[0]==='OffscreenCanvas';},
        receive(m){
            const width=m[1],height=m[2],record=m[3];
            if(m.length!==4||!safe(width)||width<0||!safe(height)||height<0)throw new TypeError('Invalid OffscreenCanvas transfer');
            const value=make(width,height,null);
            if(record!==null){const buffer=ArrayCtor.isArray(record)?importShared(record):record;states.get(value).link={buffer,header:new Int32(buffer,0,4),waiting:false};}
            return {value};
        }
    };
})();
