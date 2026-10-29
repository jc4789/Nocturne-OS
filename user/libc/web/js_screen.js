/* CSSOM View's objects of the Window: its Screen, devicePixelRatio and its VisualViewport.
   The OS reports both the current desktop and its available work area. */
const viewportBridge=(()=>{
    const screens=new WeakSet();
    const value=(screen,index)=>{if(!screens.has(screen))throw new TypeError('Illegal Screen receiver');return host.screen()[index];};
    // CSSOM View's Screen is no EventTarget: its prototype's prototype is Object.prototype.
    class Screen {
        constructor(){throw new TypeError('Illegal constructor');}
        get width(){return value(this,0);}get height(){return value(this,1);}
        get availWidth(){return value(this,2);}get availHeight(){return value(this,3);}
        get availLeft(){value(this,0);return 0;}get availTop(){value(this,0);return 0;}
        // The desktop's bits per channel times three: 24 on an 8 bpc display,
        // 30 on a 10 bpc one. Alpha is not counted (CSSOM View).
        get colorDepth(){return value(this,4);}get pixelDepth(){return value(this,4);}
    }
    const screen=Object.create(Screen.prototype);screens.add(screen);
    // Window attributes: another receiver than this Window throws (windowReceiver).
    Object.defineProperty(globalThis,'screen',{configurable:true,enumerable:true,get(){windowReceiver(this);return screen;}});
    Object.defineProperty(globalThis,'devicePixelRatio',{configurable:true,enumerable:true,get(){windowReceiver(this);return 1;}});
    globalThis.Screen=Screen;
    /* The visual viewport has no scale factor of its own (no pinch zoom) and no classic
       scrollbars here, so it is the layout viewport at the layout viewport's scroll position:
       offsets 0, scale 1, the page position the document's scroll position. Every attribute is
       0 while the document is not fully active. */
    const viewports=new WeakSet();
    const active=v=>{if(!viewports.has(v))throw new TypeError('Illegal VisualViewport receiver');return dom('fullyActive',null);};
    class VisualViewport extends EventTarget {
        constructor(){super();throw new TypeError('Illegal constructor');}
        get offsetLeft(){active(this);return 0;}
        get offsetTop(){active(this);return 0;}
        get pageLeft(){return active(this)?host.scroll(0):0;}
        get pageTop(){return active(this)?host.scroll(1):0;}
        get width(){return active(this)?dom('viewport',null,0):0;}
        get height(){return active(this)?dom('viewport',null,1):0;}
        get scale(){return active(this)?1:0;}
    }
    const viewportTypes=['resize','scroll','scrollend'];
    installHandlers(VisualViewport.prototype,viewportTypes);
    const visualViewport=Object.create(VisualViewport.prototype);viewports.add(visualViewport);
    // [SameObject, Replaceable] (js_webidl.c gives the setter); null while the document is not
    // fully active.
    Object.defineProperty(globalThis,'visualViewport',{configurable:true,enumerable:true,
        get(){windowReceiver(this);return dom('fullyActive',null)?visualViewport:null;}});
    globalThis.VisualViewport=VisualViewport;
    const fire=target=>{const event=new Event('resize');eventState(event).isTrusted=true;dispatch(target,event);};
    return {
        handlerTarget:(target,type)=>viewports.has(target)&&viewportTypes.includes(type),
        // The resize steps (js.c run_resize_steps): the Window first, then its visual viewport.
        resize(){fire(globalThis);fire(visualViewport);}
    };
})();
viewportHandlerTarget=viewportBridge.handlerTarget;
