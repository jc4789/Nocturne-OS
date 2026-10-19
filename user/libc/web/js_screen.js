/* The OS reports both the current desktop and its available work area. */
{
    const screens=new WeakSet();
    const value=(screen,index)=>{if(!screens.has(screen))throw new TypeError('Illegal Screen receiver');return host.screen()[index];};
    class Screen extends EventTarget {
        constructor(){super();throw new TypeError('Illegal constructor');}
        get width(){return value(this,0);}get height(){return value(this,1);}
        get availWidth(){return value(this,2);}get availHeight(){return value(this,3);}
        get availLeft(){value(this,0);return 0;}get availTop(){value(this,0);return 0;}
        // The desktop's bits per channel times three: 24 on an 8 bpc display,
        // 30 on a 10 bpc one. Alpha is not counted (CSSOM View).
        get colorDepth(){return value(this,4);}get pixelDepth(){return value(this,4);}
    }
    const screen=Object.create(Screen.prototype);screens.add(screen);
    Object.defineProperty(globalThis,'screen',{configurable:true,enumerable:true,get(){return screen;}});
    Object.defineProperty(globalThis,'devicePixelRatio',{configurable:true,enumerable:true,get(){return 1;}});
    globalThis.Screen=Screen;
}
