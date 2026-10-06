/* The OS reports both the current desktop and its available work area. */
{
    const screens=new WeakSet();
    const value=(screen,index)=>{if(!screens.has(screen))throw new TypeError('Illegal Screen receiver');return host.screen()[index];};
    class Screen extends EventTarget {
        constructor(){super();throw new TypeError('Illegal constructor');}
        get width(){return value(this,0);}get height(){return value(this,1);}
        get availWidth(){return value(this,2);}get availHeight(){return value(this,3);}
        get availLeft(){value(this,0);return 0;}get availTop(){value(this,0);return 0;}
        // Nocturne's canvas is 8-bit RGB in an ARGB32 buffer. Alpha is not
        // counted in the CSSOM screen color/pixel depth.
        get colorDepth(){value(this,0);return 24;}get pixelDepth(){value(this,0);return 24;}
    }
    const screen=Object.create(Screen.prototype);screens.add(screen);
    Object.defineProperty(globalThis,'screen',{configurable:true,enumerable:true,get(){return screen;}});
    Object.defineProperty(globalThis,'devicePixelRatio',{configurable:true,enumerable:true,get(){return 1;}});
    globalThis.Screen=Screen;
}
