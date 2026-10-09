/* First-only JS keepalive transport binding, not a network/site fixture. */
const fs=require('node:fs'),vm=require('node:vm'),crypto=require('node:crypto');
const source=fs.readFileSync('user/libc/web/js_fetch.js','utf8');
console.log('source_sha256='+crypto.createHash('sha256').update(source).digest('hex'));
const context=vm.createContext({console,DOMException,EventTarget,Event,URL,URLSearchParams,TextEncoder,TextDecoder});
vm.runInContext(`
const document={},calls=[],pending=[];
let abortHandlerTarget;
function installHandlers(){}
function dispatch(target,event){return target.dispatchEvent(event);}
function rawDom(){return 'https://source.test/page';}
const host={url(){return 'https://source.test/page';},cancel(){},fetch(...args){calls.push(args);return {id:calls.length,promise:new Promise(resolve=>pending.push(resolve))};}};
`,context);
vm.runInContext(source,context,{filename:'js_fetch.js'});
vm.runInContext(`
fetchBridge.initialize();
let checks=0,failures=0;
function check(name,value){checks++;console.log((value?'PASS ':'FAIL ')+name);if(!value)failures++;}
const request=new fetchBridge.Request('https://target.test/collect',{method:'POST',keepalive:true,body:new Uint8Array([17,34,51]),credentials:'include'});
const overridden=request.clone();
fetchBridge.fetch(request);
check('Request keepalive reaches native transport true',calls.length===1&&calls[0][9]===true);
check('keepalive body and original credentials reach transport',calls[0][5]===2&&Array.from(new Uint8Array(calls[0][3])).join(',')==='17,34,51');
fetchBridge.fetch(overridden,{keepalive:false});
check('explicit keepalive override reaches native false',calls.length===2&&calls[1][9]===false);
console.log('FETCH_KEEPALIVE_BRIDGE_NEW checks='+checks+' failures='+failures);
if(failures)throw new Error('new bridge boundary failed');
`,context);
