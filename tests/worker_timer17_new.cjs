/* New Worker-only timer boundaries; no prior DOM/window suites. */
const fs=require('node:fs'),vm=require('node:vm'),crypto=require('node:crypto');
const path='user/libc/web/js_worker_runtime.js',source=fs.readFileSync(path,'utf8');
const begin=source.indexOf('    function timer('),timer=source.slice(begin,source.indexOf('\n',begin));
const tickAt=source.indexOf('        tick(now){'),tick=source.slice(tickAt,source.indexOf('\n',tickAt)).trim().replace(/^tick\(now\)/,'function tick(now)').replace(/,\s*$/,'');
const context=vm.createContext({console});
let checks=0;
function check(expression,name){if(vm.runInContext(expression,context)!==true)throw new Error(name);checks++;}
vm.runInContext('let closed=false,nextTimer=0;const timers=new Map(),host={now:()=>1000};let delivered=0,seen="";function report(e){throw e;}'+timer+'\n'+tick,context);
check('(()=>{for(let i=0;i<1050;i++)timer((a,b)=>{delivered++;seen=a+b;},1,["native-","arguments"],false);return timers.size===1050&&nextTimer===1050;})()','more than former 1024 Worker timers');
check('tick(1001)===true&&delivered===1&&seen==="native-arguments"&&timers.size===1049','finite one-timer tick and retained callback arguments');
check('(()=>{nextTimer=4294967294;let end=timer(()=>{},1,[],false),wrapped=timer(()=>{},1,[],false);return end===4294967295&&wrapped===1&&timers.has(2)&&timers.has(wrapped);})()','uint32 identifier wrap skips live entries and reuses released slot');
check('(()=>{closed=true;let count=timers.size;return timer(()=>{},1,[],false)===0&&timers.size===count;})()','closed Worker cannot enqueue new timer');
console.log(path+' SHA256 '+crypto.createHash('sha256').update(source).digest('hex'));
console.log('new Worker timer boundaries: '+checks+' checks / 0 failed');
