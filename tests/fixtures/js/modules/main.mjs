import { getCycle } from './cycle-a.mjs';
globalThis.moduleExecutions = (globalThis.moduleExecutions || 0) + 1;
globalThis.moduleStarted = true;
record('ES module relative import and circular graph', getCycle() === 'AB');
record('module currentScript is null', document.currentScript === null);
record('import.meta.url is an absolute source URL', /^(https?|file):/.test(import.meta.url));
const dynamic = await import('./dynamic.mjs');
record('dynamic import and top-level await', dynamic.answer === 42);
// The DOMContentLoaded listener releases this gate. Waiting for top-level
// await before that event would deadlock and is not the HTML processing model.
await globalThis.fixtureModuleGate;
globalThis.moduleRan = true;
fixtureOrder.push('module');
record('top-level await completes after DOMContentLoaded',globalThis.fixtureDCL===true&&fixtureOrder.indexOf('dom-ready')<fixtureOrder.indexOf('module'));
