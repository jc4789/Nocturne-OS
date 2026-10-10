/* 製品 js_frames.js を realm の native adapter だけ置換して一度実行。 */
const fs=require('fs'),vm=require('vm');
let checks=0;function check(value){checks++;if(!value)throw new Error('新29 JS 条件 '+checks+' 失敗');}
const token={},proxyCache=new WeakMap(),remote={opener:null};
const box={console,DOMException,document:{},HTMLIFrameElement:class{},HTMLFrameElement:class{},Document:class{},rawDom(){return 9;},reflectedAttr(){return null;}};
box.host={documentStream(){},frame(op,t,...args){
    if(op==='self'||op==='top'||op==='parent')return token;
    if(op==='proxyGet')return proxyCache.get(t)||null;
    if(op==='proxySet'){proxyCache.set(t,args[0]);return args[0];}
    if(op==='opener')return remote.opener;
    if(op==='length')return 0;
    if(op==='frameElement')return null;
    if(op==='closed')return false;
    if(op==='set'){remote[args[0]]=args[1];return true;}
    if(op==='get')return remote[args[0]];
    throw new Error('未想定 adapter 操作 '+op);
}};
const context=vm.createContext(box);
vm.runInContext(fs.readFileSync('user/libc/web/js_frames.js','utf8')+'\nglobalThis.__frame29=frameBridge;',context);
check(vm.runInContext('opener===null && ("opener" in globalThis)',context));
check(vm.runInContext('opener=null; opener===null',context));
check(vm.runInContext('opener=42; opener===42 && Object.getOwnPropertyDescriptor(globalThis,"opener").writable',context));
check(vm.runInContext('opener=null; opener===null',context));
const proxy=context.__frame29.windowProxy(token);
check(proxy.opener===null&&('opener' in proxy));
remote.opener=7;check(proxy.opener===7);
proxy.opener=9;check(remote.opener===9);
console.log('新29 JS realm 固有条件 '+checks+'、失敗 0');
