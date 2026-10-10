/* Shared table: executed by real Window/Worker realms, and the native host probe.
 * No site acceptance is implied by these offline capability checks. */
function runNavigatorChecks(Interface,nav,expected,worker,check){
    const prefix=worker?'worker-':'window-';
    const ck=(name,value)=>check(prefix+name,!!value);
    ck('instance',nav instanceof Interface);
    ck('tag',Object.prototype.toString.call(nav)===(worker?'[object WorkerNavigator]':'[object Navigator]'));
    let error;try{new Interface();}catch(e){error=e;}ck('illegal-constructor',error instanceof TypeError);
    const values={userAgent:expected.userAgent,platform:'Nocturne',language:'en-US',onLine:true,hardwareConcurrency:expected.cpus};
    if(!worker)values.cookieEnabled=expected.cookies;
    for(const [name,value]of Object.entries(values)){
        const descriptor=Object.getOwnPropertyDescriptor(Interface.prototype,name);
        ck(name+'-descriptor',descriptor?.enumerable===true&&descriptor.configurable===true&&typeof descriptor.get==='function'&&descriptor.set===undefined);
        ck(name+'-value',nav[name]===value);
        if(!descriptor?.get)continue;
        for(const [label,receiver]of [['object',{}],['null',null],['prototype',Interface.prototype],['inherited',Object.create(nav)],['proxy',new Proxy(nav,{})]]){
            let caught;try{Reflect.apply(descriptor.get,receiver,[]);}catch(e){caught=e;}
            ck(name+'-receiver-'+label,caught instanceof TypeError);
        }
        const before=nav[name];try{nav[name]='not-native';}catch(_){}ck(name+'-readonly',nav[name]===before);
    }
    ck('native-count-integer',Number.isInteger(nav.hardwareConcurrency)&&nav.hardwareConcurrency>=1);
    ck('languages',Array.isArray(nav.languages)&&Object.isFrozen(nav.languages)&&nav.languages.length===1&&nav.languages[0]===nav.language);
    if(worker)ck('no-cookie-capability',!('cookieEnabled'in nav)&&!Object.prototype.hasOwnProperty.call(Interface.prototype,'cookieEnabled'));
    const original=WeakSet.prototype.has;
    try{
        WeakSet.prototype.has=()=>true;
        let caught;try{Reflect.apply(Object.getOwnPropertyDescriptor(Interface.prototype,'userAgent').get,{},[]);}catch(e){caught=e;}
        ck('brand-not-forgeable',caught instanceof TypeError);
    }finally{WeakSet.prototype.has=original;}
}
