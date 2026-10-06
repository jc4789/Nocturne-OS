import { label } from './label.mjs';
await Promise.resolve();
document.getElementById('module').textContent=label+' — '+import.meta.url;
console.log('ES module and top-level await:',label);
