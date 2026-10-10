/* Window and Worker share the actual native HTTP identity and scheduler count.
 * Cookies remain a live Window-only host capability, not a write/read probe. */
const navigatorBridge=(()=>{
    const invoke=Reflect.apply,has=WeakSet.prototype.has,add=WeakSet.prototype.add;
    const define=Object.defineProperty,freeze=Object.freeze,cookieEnabled=host.cookieEnabled;
    function create(worker){
        const token={},brands=new WeakSet();
        function construct(receiver,value){
            if(value!==token)throw new TypeError('Illegal navigator constructor');
            invoke(add,brands,[receiver]);
        }
        class Navigator {constructor(value){construct(this,value);}}
        class WorkerNavigator {constructor(value){construct(this,value);}}
        const Interface=worker?WorkerNavigator:Navigator;
        function requireReceiver(receiver){
            if(!invoke(has,brands,[receiver]))throw new TypeError('Illegal navigator receiver');
        }
        const values={userAgent:host.userAgent,platform:host.platform,language:'en-US',
            languages:freeze(['en-US']),onLine:true,hardwareConcurrency:host.hardwareConcurrency};
        for(const [name,value] of Object.entries(values))
            define(Interface.prototype,name,{configurable:true,enumerable:true,get(){requireReceiver(this);return value;}});
        if(!worker)define(Interface.prototype,'cookieEnabled',{configurable:true,enumerable:true,get(){
            requireReceiver(this);return typeof cookieEnabled==='function'&&!!invoke(cookieEnabled,host,[]);
        }});
        define(Interface.prototype,Symbol.toStringTag,{value:worker?'WorkerNavigator':'Navigator',configurable:true});
        return {navigator:new Interface(token),Interface};
    }
    return {create};
})();
