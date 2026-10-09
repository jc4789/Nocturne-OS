/* New binding-only Proxy invariant boundary. Native parser cases are not rerun. */
const fs = require('node:fs'), vm = require('node:vm'), crypto = require('node:crypto');
const source = fs.readFileSync('user/libc/web/js_stylesheets.js', 'utf8');
console.log('source_sha256=' + crypto.createHash('sha256').update(source).digest('hex'));
const context = vm.createContext({console, DOMException});
vm.runInContext(`
class HTMLStyleElement {}
function htmlElementBrand(value) { if (!(value instanceof HTMLStyleElement)) throw new TypeError('brand'); }
function reflectedAttr() { return ''; }
function rawDom(op,node,key,id,index) {
 if(op==='get') return true;
 if(op==='cssom') {
  if(key==='sheet')return 1;
  if(key==='length')return 1;
  if(key==='current')return true;
  if(key==='rule'||key==='ruleId')return index===0||key==='ruleId'?{id:1,type:1,attached:true,text:'.x{}',selector:'.x'}:null;
 }
 return false;
}
`, context);
vm.runInContext(source, context, {filename:'js_stylesheets.js'});
vm.runInContext(`
let checks=0, failures=0;
function check(name,value) { checks++; if(!value){failures++;console.log('FAIL '+name);} }
const list=new HTMLStyleElement().sheet.cssRules;
check('nonconfigurable numeric define rejected',Reflect.defineProperty(list,'0',{value:'attack',configurable:false})===false);
check('outside numeric define rejected',Reflect.defineProperty(list,'1',{value:'attack',configurable:true})===false);
check('numeric delete rejected',Reflect.deleteProperty(list,'0')===false);
check('numeric lookup remains invariant safe',list[0] instanceof CSSStyleRule);
check('virtual descriptor remains configurable',Reflect.getOwnPropertyDescriptor(list,'0').configurable===true);
check('preventExtensions rejected',Reflect.preventExtensions(list)===false);
let threw=false;try{Object.preventExtensions(list);}catch(e){threw=e instanceof TypeError;}
check('Object.preventExtensions throws',threw);
check('normal configurable expando allowed',Reflect.defineProperty(list,'extra',{value:42,configurable:true})&&list.extra===42);
const keys=Reflect.ownKeys(list);
check('ownKeys no numeric duplicates',keys.filter(x=>x==='0').length===1&&keys.filter(x=>x==='1').length===0);
console.log('CSS_RULE_LIST_PROXY_NEW checks='+checks+' failures='+failures);
if(failures)throw new Error('new binding boundary failed');
`, context);
