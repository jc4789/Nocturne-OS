/* Actual embedder windows and native top-layer ownership. No alternate DOM,
 * JS-selected surface, cloned video, or artificial playback is created. */
const presentationBridge=(()=>{
    const native=host.presentation,define=Object.defineProperty;
    let pip=null,pipWindow=null;
    let pending=null,pendingAt=0;
    const token={},windows=new WeakMap();
    const get=WeakMap.prototype.get,set=WeakMap.prototype.set,has=WeakMap.prototype.has;
    const PromiseImpl=Promise,sessions=new Map(),mapGet=Map.prototype.get,mapSet=Map.prototype.set,mapDelete=Map.prototype.delete;
    function trustedEvent(type,EventClass=Event,init={}){const e=new EventClass(type,init);eventState(e).isTrusted=true;return e;}
    function trusted(target,type,EventClass=Event,init={}){dispatch(target,trustedEvent(type,EventClass,init));}
    class PictureInPictureWindow extends EventTarget{
        constructor(secret){super();if(secret!==token)throw new TypeError('Illegal constructor');apply(set,windows,[this,{width:0,height:0}]);}
        get width(){const s=apply(get,windows,[this]);if(!s)throw new TypeError('Invalid PictureInPictureWindow');return s.width;}
        get height(){const s=apply(get,windows,[this]);if(!s)throw new TypeError('Invalid PictureInPictureWindow');return s.height;}
    }
    class PictureInPictureEvent extends Event{
        constructor(type,init={}){super(type,init);if(!apply(has,windows,[init.pictureInPictureWindow]))throw new TypeError('pictureInPictureWindow required');define(this,'pictureInPictureWindow',{value:init.pictureInPictureWindow,enumerable:true});}
    }
    function sync(){
        // Native transitions are queued, not inferred by 40ms state sampling.
        // An enter+exit in one author task must still publish both events.
        if(!pending){
            const events=native('events'),plans=[];
            // Allocate every Event and associated Window before consuming even
            // one native notification. OOM retains the entire native FIFO.
            for(let i=0;i<events.length;i++){const row=events[i],kind=row[0],node=row[1];let target=null,event=null,entry=null;
                if(kind!==1){entry=apply(mapGet,sessions,[row[4]]);
                    if(!entry&&kind===2){entry={window:new PictureInPictureWindow(token),node,resolve:null,ready:false};apply(mapSet,sessions,[row[4],entry]);}}
                if(kind===1){target=node;event=trustedEvent('fullscreenchange',Event,{bubbles:true,composed:true});}
                else if(kind===2){target=node;event=trustedEvent('enterpictureinpicture',PictureInPictureEvent,{bubbles:true,pictureInPictureWindow:entry.window});}
                else if(kind===3&&entry){target=node;event=trustedEvent('leavepictureinpicture',PictureInPictureEvent,{bubbles:true,pictureInPictureWindow:entry.window});}
                else if(kind===4&&entry){target=entry.window;event=trustedEvent('resize');}
                define(plans,i,{value:{kind,node,target,event,entry,session:row[4],width:row[2],height:row[3]},configurable:true,writable:true,enumerable:true});
            }
            pending=plans;pendingAt=0;
        }
        while(pendingAt<pending.length){const plan=pending[pendingAt];let target=plan.target;
            if(plan.kind===1)target=target&&rawDom.get(target,'isConnected')&&rawDom.get(target,'ownerDocument')===document?target:document;
            if(!native('eventPop')){pending=null;pendingAt=0;return;}
            pendingAt++;
            if(plan.kind===2){pip=plan.node;pipWindow=plan.entry.window;}
            if((plan.kind===2||plan.kind===4)&&plan.entry){const state=apply(get,windows,[plan.entry.window]);state.width=plan.width;state.height=plan.height;}
            // Consume the current row before invoking author listeners; retain
            // prepared later rows across an internal dispatch interruption.
            try{if(plan.event)dispatch(target,plan.event);}
            finally{
                if(plan.kind===2&&plan.entry){plan.entry.ready=true;const resolve=plan.entry.resolve;plan.entry.resolve=null;if(resolve)resolve(plan.entry.window);}
                if(plan.kind===3&&plan.entry){if(pipWindow===plan.entry.window){pip=null;pipWindow=null;}apply(mapDelete,sessions,[plan.session]);}
            }
            /* PiP enter resolves its Promise after that event; a leave or
               resize is a separate media task with a microtask checkpoint. */
            if(plan.kind!==1){if(pendingAt===pending.length){pending=null;pendingAt=0;}return;}
        }
        pending=null;pendingAt=0;
        pip=native('pipElement');if(!pip)pipWindow=null;
    }
    const errors=['','InvalidStateError','NotAllowedError','NotSupportedError','InvalidStateError','InvalidStateError','NotAllowedError'];
    function request(node,kind){
        return new PromiseImpl(resolve=>{
            if(!(node instanceof Element))throw new TypeError('Element receiver required');
            if(kind==='pip'&&!(node instanceof HTMLVideoElement))throw new TypeError('HTMLVideoElement receiver required');
            // Reserve the real association's JS object before entering native
            // PiP; expose it only when the native operation actually succeeds.
            const candidate=kind==='pip'?{window:new PictureInPictureWindow(token),node,resolve,ready:false}:null;
            const response=native(kind,node),status=kind==='pip'?response[0]:response;
            if(status){if(kind==='fullscreen'){setTimeout(()=>trusted(rawDom.get(node,'isConnected')?node:document,'fullscreenerror',Event,{bubbles:true,composed:true}),0);throw new TypeError('Native fullscreen request rejected');}throw new DOMException('Native presentation request rejected',errors[status]||'NotSupportedError');}
            if(kind==='fullscreen'){resolve(undefined);return;}
            const session=response[1];let entry=apply(mapGet,sessions,[session]);
            if(!entry){entry=candidate;apply(mapSet,sessions,[session,entry]);}
            const state=apply(get,windows,[entry.window]);state.width=response[2];state.height=response[3];
            // An already-associated request resolves with the existing native
            // session's Window. New requests resolve from their enter task.
            if(!response[4])resolve(entry.window);
        });
    }
    Object.defineProperty(Element.prototype,'requestFullscreen',{configurable:true,writable:true,value:function(options){if(options!==undefined&&options!==null){if(typeof options!=='object'&&typeof options!=='function')return Promise.reject(new TypeError('FullscreenOptions dictionary required'));const ui=options.navigationUI;if(ui!==undefined&&!['auto','hide','show'].includes(String(ui)))return Promise.reject(new TypeError('Invalid navigationUI'));if(options.screen!==undefined)return Promise.reject(new DOMException('Multi-screen selection is unsupported','NotSupportedError'));}return request(this,'fullscreen');}});
    function retarget(node,root){while(node){const tree=node.getRootNode();if(tree===root)return node;if(!(tree instanceof ShadowRoot))return null;node=tree.host;}return null;}
    if(typeof ShadowRoot==='function')Object.defineProperty(ShadowRoot.prototype,'fullscreenElement',{configurable:true,get(){return retarget(native('fullscreenElement'),this);}});
    Object.defineProperties(Document.prototype,{
        fullscreenEnabled:{configurable:true,get(){if(this!==document)throw new TypeError('Active Document required');return native('fullscreenEnabled');}},
        fullscreenElement:{configurable:true,get(){if(this!==document)return null;return retarget(native('fullscreenElement'),document);}},
        pictureInPictureEnabled:{configurable:true,get(){if(this!==document)return false;return native('pipEnabled');}},
        pictureInPictureElement:{configurable:true,get(){if(this!==document)return null;return native('pipElement');}},
        exitFullscreen:{configurable:true,writable:true,value:async function(){if(this!==document)throw new TypeError('Active Document required');if(!native('fullscreenElement'))throw new TypeError('Document is not fullscreen');if(!native('exitFullscreen'))throw new DOMException('Cannot restore native window','InvalidStateError');}},
        exitPictureInPicture:{configurable:true,writable:true,value:async function(){if(this!==document||!native('pipElement'))throw new DOMException('Document is not in picture-in-picture','InvalidStateError');if(!native('exitPip'))throw new DOMException('Cannot close native picture-in-picture','InvalidStateError');}}
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
        (type==='resize'&&apply(has,windows,[target]));
    for(const cls of [PictureInPictureWindow,PictureInPictureEvent])Object.defineProperty(cls.prototype,Symbol.toStringTag,{value:cls.name,configurable:true});
    Object.assign(globalThis,{PictureInPictureWindow,PictureInPictureEvent});return {sync};
})();
