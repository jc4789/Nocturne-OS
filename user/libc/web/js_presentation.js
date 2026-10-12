/* Actual embedder windows and native top-layer ownership. No alternate DOM,
 * JS-selected surface, cloned video, or artificial playback is created. */
const presentationBridge=(()=>{
    const native=host.presentation;
    let full=null,pip=null,pipWindow=null,timer=0;
    const token={},windows=new WeakMap();
    function trusted(target,type,EventClass=Event,init={}){const e=new EventClass(type,init);eventState(e).isTrusted=true;dispatch(target,e);}
    class PictureInPictureWindow extends EventTarget{
        constructor(secret){super();if(secret!==token)throw new TypeError('Illegal constructor');windows.set(this,{width:0,height:0});}
        get width(){const s=windows.get(this);if(!s)throw new TypeError('Invalid PictureInPictureWindow');return s.width;}
        get height(){const s=windows.get(this);if(!s)throw new TypeError('Invalid PictureInPictureWindow');return s.height;}
    }
    class PictureInPictureEvent extends Event{
        constructor(type,init={}){super(type,init);if(!windows.has(init.pictureInPictureWindow))throw new TypeError('pictureInPictureWindow required');Object.defineProperty(this,'pictureInPictureWindow',{value:init.pictureInPictureWindow,enumerable:true});}
    }
    function sync(){
        const nextFull=native('fullscreenElement'),nextPip=native('pipElement');
        if(nextFull!==full){const previous=full;full=nextFull;const target=full||(previous&&previous.isConnected&&previous.ownerDocument===document?previous:document);trusted(target,'fullscreenchange',Event,{bubbles:true,composed:true});}
        if(nextPip!==pip){const previous=pip,previousWindow=pipWindow;pip=nextPip;
            if(previous)trusted(previous,'leavepictureinpicture',PictureInPictureEvent,{pictureInPictureWindow:previousWindow});
            if(pip){pipWindow=new PictureInPictureWindow(token);const size=native('pipSize'),s=windows.get(pipWindow);s.width=size[0];s.height=size[1];trusted(pip,'enterpictureinpicture',PictureInPictureEvent,{pictureInPictureWindow:pipWindow});}
        }
        if(pip&&pipWindow){const size=native('pipSize'),s=windows.get(pipWindow);if(size[0]!==s.width||size[1]!==s.height){s.width=size[0];s.height=size[1];trusted(pipWindow,'resize');}}
        if((full||pip)&&!timer)timer=setTimeout(()=>{timer=0;sync();},40);
    }
    const errors=['','InvalidStateError','NotAllowedError','NotSupportedError','InvalidStateError','NotSupportedError'];
    async function request(node,kind){
        if(!(node instanceof Element))throw new TypeError('Element receiver required');
        if(kind==='pip'&&!(node instanceof HTMLVideoElement))throw new TypeError('HTMLVideoElement receiver required');
        const result=native(kind,node);
        if(result){if(kind==='fullscreen'){setTimeout(()=>trusted(node.isConnected?node:document,'fullscreenerror',Event,{bubbles:true,composed:true}),0);throw new TypeError('Native fullscreen request rejected');}throw new DOMException('Native presentation request rejected',errors[result]||'NotSupportedError');}
        await new Promise(resolve=>setTimeout(resolve,0));sync();
        return kind==='pip'?pipWindow:undefined;
    }
    Object.defineProperty(Element.prototype,'requestFullscreen',{configurable:true,writable:true,value:function(options){if(options!==undefined&&options!==null){if(typeof options!=='object'&&typeof options!=='function')return Promise.reject(new TypeError('FullscreenOptions dictionary required'));const ui=options.navigationUI;if(ui!==undefined&&!['auto','hide','show'].includes(String(ui)))return Promise.reject(new TypeError('Invalid navigationUI'));if(options.screen!==undefined)return Promise.reject(new DOMException('Multi-screen selection is unsupported','NotSupportedError'));}return request(this,'fullscreen');}});
    function retarget(node,root){while(node){const tree=node.getRootNode();if(tree===root)return node;if(!(tree instanceof ShadowRoot))return null;node=tree.host;}return null;}
    if(typeof ShadowRoot==='function')Object.defineProperty(ShadowRoot.prototype,'fullscreenElement',{configurable:true,get(){return retarget(native('fullscreenElement'),this);}});
    Object.defineProperties(Document.prototype,{
        fullscreenEnabled:{configurable:true,get(){if(this!==document)throw new TypeError('Active Document required');return native('enabled');}},
        fullscreenElement:{configurable:true,get(){if(this!==document)return null;return retarget(native('fullscreenElement'),document);}},
        pictureInPictureEnabled:{configurable:true,get(){if(this!==document)return false;return native('enabled');}},
        pictureInPictureElement:{configurable:true,get(){if(this!==document)return null;return native('pipElement');}},
        exitFullscreen:{configurable:true,writable:true,value:async function(){if(this!==document)throw new TypeError('Active Document required');if(!native('fullscreenElement'))throw new TypeError('Document is not fullscreen');if(!native('exitFullscreen'))throw new DOMException('Cannot restore native window','InvalidStateError');await new Promise(resolve=>setTimeout(resolve,0));sync();}},
        exitPictureInPicture:{configurable:true,writable:true,value:async function(){if(this!==document||!native('pipElement'))throw new DOMException('Document is not in picture-in-picture','InvalidStateError');if(!native('exitPip'))throw new DOMException('Cannot close native picture-in-picture','InvalidStateError');await new Promise(resolve=>setTimeout(resolve,0));sync();}}
    });
    Object.defineProperties(HTMLVideoElement.prototype,{
        disablePictureInPicture:{configurable:true,get(){avmediaBridge.brand(this);return this.hasAttribute('disablepictureinpicture');},set(value){avmediaBridge.brand(this);this.toggleAttribute('disablepictureinpicture',!!value);}},
        requestPictureInPicture:{configurable:true,writable:true,value:function(){return request(this,'pip');}}
    });
    installHandlers(Element.prototype,['fullscreenchange','fullscreenerror']);installHandlers(Document.prototype,['fullscreenchange','fullscreenerror']);
    installHandlers(HTMLVideoElement.prototype,['enterpictureinpicture','leavepictureinpicture']);installHandlers(PictureInPictureWindow.prototype,['resize']);
    presentationHandlerTarget=(target,type)=>
        ((type==='fullscreenchange'||type==='fullscreenerror')&&(target instanceof Element||target instanceof Document))||
        ((type==='enterpictureinpicture'||type==='leavepictureinpicture')&&target instanceof HTMLVideoElement)||
        (type==='resize'&&windows.has(target));
    for(const cls of [PictureInPictureWindow,PictureInPictureEvent])Object.defineProperty(cls.prototype,Symbol.toStringTag,{value:cls.name,configurable:true});
    Object.assign(globalThis,{PictureInPictureWindow,PictureInPictureEvent});return {sync};
})();
