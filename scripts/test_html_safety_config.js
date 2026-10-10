/* Pure configuration/policy algorithm checks. No native DOM, QuickJS, CSP
 * response delivery or real-site acceptance is claimed by this host test. */
const fs=require('node:fs'),vm=require('node:vm');
const context=vm.createContext({console});
vm.runInContext(`
const apply=Reflect.apply;
class Event{constructor(type,init={}){this.type=String(type);this.bubbles=!!init.bubbles;this.cancelable=!!init.cancelable;this.composed=!!init.composed;}}
let securityState={required:false,enforce:false,policies:[]};
const violations=[],brands=new WeakMap();
const host={
 trusted(op,...args){
  if(op==='create'){const value=Object.create(args[2]);brands.set(value,{kind:args[0],payload:args[1]});return value;}
  if(op==='kind')return brands.get(args[0])?.kind||0;
  if(op==='payload'){const data=brands.get(args[0]);if(!data||data.kind!==args[1])throw new TypeError('Brand');return data.payload;}
 },
 safety(op,...args){if(op==='state')return securityState;if(op==='violation')violations.push(args);}
};
`,context);
for(const source of ['js_sanitizer_constants.js','js_html_safety.js']){
 let text=fs.readFileSync('user/libc/web/'+source,'utf8');
 if(source==='js_html_safety.js')text=text.replace('return {check,options,','return {Sanitizer,check,options,');
 vm.runInContext(text,context,{filename:source});
}
vm.runInContext(`
let count=0;
function check(label,value){count++;if(!value)throw new Error(label);}
function throws(label,fn){let caught=false;try{fn();}catch(e){caught=e instanceof TypeError;}check(label,caught);}
const Sanitizer=htmlSafetyBridge.Sanitizer;
const permissive=new Sanitizer({});check('permissive defaults',permissive.get().comments&&permissive.get().javascriptURLs);
check('baseline',permissive.removeUnsafe()&&!permissive.removeUnsafe());
check('baseline script',permissive.get().removeElements.some(e=>e.name==='script'));
throws('contradictory',()=>new Sanitizer({elements:[],removeElements:[]}));
throws('duplicates',()=>new Sanitizer({attributes:['x','x']}));
throws('primitive sequence rejected',()=>new Sanitizer({elements:'p'}));
throws('arraylike sequence rejected',()=>new Sanitizer({attributes:{0:'title',length:1}}));
throws('data invariants',()=>new Sanitizer({dataAttributes:false}));
throws('nonreplaceable',()=>new Sanitizer({replaceWithChildrenElements:['html']}));
throws('local redundant',()=>new Sanitizer({elements:[{name:'p',attributes:['title']}],attributes:['title']}));
const defaults=new Sanitizer();check('generated defaults',defaults.get().elements.length===122);
const snapshot=defaults.get();snapshot.elements.length=0;check('copy isolated',defaults.get().elements.length===122);
const local=new Sanitizer({elements:['p'],attributes:['title'],dataAttributes:false});
check('allow element',local.allowElement({name:'b',attributes:['id']}));
check('allow attribute',local.allowAttribute('id')&&!local.allowAttribute('id'));
check('promotion valid',local.get().elements.find(e=>e.name==='b').attributes.length===0);
check('data mutator',local.setDataAttributes(true)&&!local.setDataAttributes(true));
check('nonreplaceable mutator',!local.replaceElementWithChildren('html'));
let reads=0;const once=new Sanitizer({get elements(){reads++;return ['p'];}});check('dictionary getter read once',reads===1&&once.get().elements.length===1);
const c=htmlSafetyBridge.options({},'unit');
check('options default',c.runScripts===false);
const p=trustedTypes.createPolicy('unit',{createHTML:x=>'<b>'+x+'</b>',createScript:x=>x});
const html=p.createHTML('x');check('brand algorithm',trustedTypes.isHTML(html));
html.toString=()=>{throw new Error('Stringifier must not run');};
check('native payload route',htmlSafetyBridge.check(html,1,'unit')==='<b>x</b>');
check('unrequired string',htmlSafetyBridge.check('x',1,'unit')==='x');
securityState={required:true,enforce:true,policies:[]};
throws('enforced raw string',()=>htmlSafetyBridge.check('x',1,'unit'));
check('reports mismatch',violations.length===1);
securityState={required:true,enforce:true,policies:[],requirements:[{policyIndex:4,reportOnly:false},{policyIndex:7,reportOnly:true}]};
const beforeRules=violations.length;
throws('multiple rule raw rejection',()=>htmlSafetyBridge.check('raw',1,'unit'));
check('per native rule report',violations.length===beforeRules+2&&violations[beforeRules][5]===4&&violations[beforeRules+1][5]===7);
check('per rule dispositions and sink sample',violations[beforeRules][4]==='enforce'&&violations[beforeRules+1][4]==='report'&&violations[beforeRules][3]==='unit|raw');
throws('untrusted options vetted',()=>htmlSafetyBridge.compliantInput(html,{runScripts:true},'unit',null));
securityState={required:true,enforce:false,policies:[]};
check('report only accepts',htmlSafetyBridge.check('raw',1,'unit')==='raw');
securityState={required:true,enforce:true,policies:[{names:['allowed','default'],allowDuplicates:false,reportOnly:false}]};
throws('CSP policy disallow',()=>trustedTypes.createPolicy('bad',{}));
trustedTypes.createPolicy('allowed',{});
throws('CSP duplicate',()=>trustedTypes.createPolicy('allowed',{}));
trustedTypes.createPolicy('default',{createHTML:x=>{if(x==='reenter')return htmlSafetyBridge.check('nested',1,'nested');if(x==='callback-throw')throw new TypeError('callback');return 'checked:'+x;},createScript:x=>x});
check('default callback',htmlSafetyBridge.check('raw',1,'unit')==='checked:raw');
throws('automatic policy reentry denied',()=>htmlSafetyBridge.check('reenter',1,'unit'));
throws('policy exception propagates',()=>htmlSafetyBridge.check('callback-throw',1,'unit'));
check('automatic guard restored',htmlSafetyBridge.check('after-error',1,'unit')==='checked:after-error');
throws('default duplicate',()=>trustedTypes.createPolicy('default',{}));
check('nonstring eval passthrough',htmlSafetyBridge.dynamicCode(123,0)===123);
const s=p.createScript('42');s.toString=()=>{throw new Error('Cannot unwrap with author method');};
check('argument native unwrap',htmlSafetyBridge.dynamicCode(s,2)==='42');
check('trusted eval unwrap',htmlSafetyBridge.dynamicCode(s,0)==='42');
const event=new SecurityPolicyViolationEvent('custom',{sample:'source',lineNumber:-1,statusCode:65537});
check('violation event fields',event.sample==='source'&&event.lineNumber===4294967295&&event.statusCode===1&&!event.isTrusted);
event.isTrusted=true;check('author cannot promote violation trust',!event.isTrusted);
const trustedEvent=htmlSafetyBridge.violationEvent({effectiveDirective:'trusted-types',disposition:'report'});
check('private violation trusted event',trustedEvent.isTrusted&&trustedEvent.bubbles&&trustedEvent.composed&&trustedEvent.disposition==='report');
throws('violation enum',()=>new SecurityPolicyViolationEvent('custom',{disposition:'invalid'}));
console.log('HTML safety policy algorithms: '+count+' passed (host mock only)');
`,context);
