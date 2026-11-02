/* WPT's editing pages (editing/, run by WPT's own testharness.js) against the product's editing
 * layer: js_document_commands.js (the Document command methods, designMode, contentEditable,
 * HTML's "editing host" and "editable") and the js_editing program (js_editing.js with the
 * execCommand draft's implementation from third_party/editing in it, put together as
 * js_embed.py puts it), loaded into each test page before the page's own scripts and installed
 * over the host browser's own command methods.
 *
 * The host is a headless Chrome (the installed one, or --chrome=PATH) driven over the DevTools
 * protocol, with one in-process server for WPT's static files. So the DOM, the HTML parser and
 * serializer, Range, Selection, MutationObserver, events and computed style are Chrome's, not
 * the product's (dom.c, js_range.js, cssom.c): this shows what the editing layer does over a
 * mature DOM, which separates the layer's own faults from the guest's DOM and CSSOM ones. It is
 * not the guest browser and not wptrunner. The bridge's native questions are answered here as
 * js_document_commands.h answers them: a color as getComputedStyle() serializes it (here through
 * a canvas's fillStyle) or null for no color and currentcolor; the font keywords' pixel sizes
 * (cssprop.c's table). A focused text control's plain-text commands (js_document_commands.h's
 * native path) are not here: with a focused input or textarea, the rich-text commands are not
 * enabled, as in the product, and the plain-text ones do nothing. testdriver.js is WPT's own, its
 * vendor file only says it runs in automation, so its actions (keys, clicks, the clipboard) reject
 * at once, and the subtests that need them fail in every run. Chrome also computes white-space
 * pre-wrap (white-space-collapse preserve) for everything in a Plaintext-Only editing host,
 * whatever the page's style says, where the product computes the cascaded value: in the
 * editing/plaintext-only pages, the subtests of a white-space:normal context see line feeds kept
 * and collapsible spaces preserved here, so their results there are not the product's.
 *
 * With --base=REV, the same pages also run with REV's three sources, and the check fails when a
 * subtest REV passed does not pass now. Results: build/editing-wpt/<label>.jsonl (--label, else
 * work-tree) and <REV>.jsonl, in the guest's results.jsonl form.
 * Run: node tests/editing_wpt_contract.cjs [--base=REV] [--label=NAME] [--wpt=DIR] [--chrome=PATH] [--verbose]
 *      [--dirs=DIR,...] [page...]   (a path under editing/: a page with its variant, or with --dirs a
 *      directory or a page, every variant) */
'use strict';
const fs = require('node:fs'), path = require('node:path'), http = require('node:http'), cp = require('node:child_process');
const root = path.resolve(__dirname, '..');
const option = name => { const a = process.argv.find(x => x.startsWith('--' + name + '=')); return a ? a.slice(name.length + 3) : null; };
const wpt = option('wpt') || process.env.WPT_ROOT || 'D:/Programes/cloned repos/wpt';
const base = option('base'), verbose = process.argv.includes('--verbose');
const chrome = option('chrome') || ['C:/Program Files/Google/Chrome/Application/chrome.exe',
    'C:/Program Files (x86)/Google/Chrome/Application/chrome.exe',
    'C:/Program Files (x86)/Microsoft/Edge/Application/msedge.exe'].find(p => fs.existsSync(p));
if (!chrome) { console.error('no Chrome found: pass --chrome=PATH'); process.exit(2); }
/* The pages of a directory under editing/ (or one page), each with its variants from its
   <meta name=variant>. */
function pagesIn(where) {
    const full = path.join(wpt, 'editing', where), pages = [];
    const files = fs.statSync(full).isDirectory()
        ? fs.readdirSync(full).filter(n => n.endsWith('.html') && !n.endsWith('-ref.html')).sort().map(n => where + '/' + n) : [where];
    for (const file of files) {
        const text = fs.readFileSync(path.join(wpt, 'editing', file), 'utf8');
        if (!/testharness\.js/.test(text)) continue;
        const variants = [...text.matchAll(/<meta name="?variant"? content="([^"]*)"/g)].map(m => m[1]);
        for (const v of variants.length ? variants : ['']) pages.push(file + v);
    }
    return pages;
}
const defaults = () => [...pagesIn('plaintext-only'), ...pagesIn('other/editing-around-select-element.tentative.html'),
    ...pagesIn('whitespaces/chrome-compat/inserttext.tentative.html')];
const named = process.argv.slice(2).filter(a => !a.startsWith('--'));
const groups = option('dirs');
const pages = named.length ? named : groups ? groups.split(',').flatMap(pagesIn) : defaults();
const git = (rev, file) => cp.execFileSync('git', ['show', rev + ':' + file], {cwd: root, encoding: 'utf8', maxBuffer: 1 << 26});
const source = (rev, file) => rev ? git(rev, file) : fs.readFileSync(path.join(root, file), 'utf8');

/* The page script: the bridge's natives over the host's DOM, then the product's sources. */
function layerScript(rev) {
    const web = 'user/libc/web/';
    let editing = source(rev, web + 'js_editing.js');
    if (editing.split('/* @editing-implementation */').length !== 2) throw new Error('editing include contract changed');
    editing = editing.replace('/* @editing-implementation */', () => source(rev, 'third_party/editing/implementation.js'));
    const commands = source(rev, web + 'js_document_commands.js');
    return `(function(){'use strict';
const editingProgram=${editing};
const errors=[];Object.defineProperty(window,'__nocturneEditingErrors',{value:errors});
const canvas=document.createElement('canvas').getContext('2d');
function serializeColor(text){
    if(!CSS.supports('color',text)||/^\\s*currentcolor\\s*$/i.test(text))return null;
    canvas.fillStyle='#000';canvas.fillStyle=text;const v=canvas.fillStyle;
    const m=/^#([0-9a-f]{2})([0-9a-f]{2})([0-9a-f]{2})$/.exec(v);
    return m?'rgb('+parseInt(m[1],16)+', '+parseInt(m[2],16)+', '+parseInt(m[3],16)+')':v;
}
const rawDom={
    get:(n,key)=>n[key],
    isNode:(_,n)=>n instanceof Node,
    attrNS:(n,ns,name)=>n.getAttributeNS(ns,name),
    documentCommand(doc,mode,cmd){
        if(mode==='textControl'){const a=doc.activeElement;return !!a&&(a.localName==='textarea'||a.localName==='input');}
        if(mode==='color')return serializeColor(cmd);
        if(mode==='fontKeywords')return [9,10,13,16,18,24,32,48];
        if(mode==='designMode')return false;
        if(mode==='value')return '';
        if(mode==='prepare')return null;
        return false;
    },
};
const documentBridge={brand(r){if(!(r instanceof Document))throw new TypeError('Illegal invocation');}};
const host={editing:()=>editingProgram,log(level,...args){errors.push(args.map(String).join(' '));console.error(...args);}};
function CSSStyleProperties(element){return getComputedStyle(element);}
const styleToken=undefined;
function dom(op,n,ns,name,value){
    if(op==='focus')return HTMLElement.prototype.focus.call(n);
    if(op==='attrNS')return value===null?n.removeAttributeNS(ns,name):n.setAttributeNS(ns,name,value);
    throw new Error('dom '+op);
}
const states=new WeakMap();function eventState(e){let s=states.get(e);if(!s)states.set(e,s={});return s;}
function dispatch(target,event){return target.dispatchEvent(event);}
${commands}
Object.defineProperty(window,'__nocturneEditing',{value:documentCommandBridge});
})();
`;
}

const reporter = `(function(){
  if (window !== window.top) return;
  var stringify = JSON.stringify, url = String(location.href);
  setup({output: false, timeout_multiplier: 1, explicit_timeout: false});
  add_completion_callback(function (tests, status) {
    var list = [];
    for (var i = 0; i < tests.length; i++) list.push({name: String(tests[i].name), status: tests[i].status,
        message: tests[i].message == null ? null : String(tests[i].message)});
    var body = stringify({url: url, status: status.status, message: status.message == null ? null : String(status.message),
        tests: list, errors: (window.__nocturneEditingErrors || []).slice(0, 20)});
    var xhr = new XMLHttpRequest();
    xhr.open("POST", "/__nocturne/result");
    xhr.send(body);
  });
})();
`;

const types = {'.html': 'text/html;charset=utf-8', '.htm': 'text/html;charset=utf-8', '.js': 'text/javascript;charset=utf-8',
    '.css': 'text/css;charset=utf-8', '.json': 'application/json', '.png': 'image/png', '.svg': 'image/svg+xml',
    '.txt': 'text/plain;charset=utf-8', '.gif': 'image/gif', '.jpg': 'image/jpeg'};
let layer = '', pending = null;
const server = http.createServer((req, res) => {
    const url = new URL(req.url, 'http://localhost');
    if (req.method === 'POST' && url.pathname === '/__nocturne/result') {
        const chunks = [];
        req.on('data', c => chunks.push(c));
        req.on('end', () => {
            res.writeHead(200, {'Content-Type': 'text/plain'}); res.end('');
            if (pending) pending(JSON.parse(Buffer.concat(chunks).toString('utf8')));
        });
        return;
    }
    if (url.pathname === '/__nocturne/editing.js') { res.writeHead(200, {'Content-Type': types['.js'], 'Cache-Control': 'no-store'}); res.end(layer); return; }
    if (url.pathname === '/resources/testharnessreport.js') { res.writeHead(200, {'Content-Type': types['.js']}); res.end(reporter); return; }
    /* In automation, testdriver.js's actions reject at once; otherwise they wait for a person. */
    if (url.pathname === '/resources/testdriver-vendor.js') {
        res.writeHead(200, {'Content-Type': types['.js']});
        res.end('window.test_driver_internal.in_automation = true;\n');
        return;
    }
    const file = path.join(wpt, decodeURIComponent(url.pathname));
    if (!file.startsWith(path.resolve(wpt)) || !fs.existsSync(file) || fs.statSync(file).isDirectory()) { res.writeHead(404); res.end(); return; }
    let body = fs.readFileSync(file);
    const ext = path.extname(file).toLowerCase();
    if (ext === '.html' || ext === '.htm') {
        /* The layer goes first, after the doctype (before it the page would be in quirks mode). */
        const text = body.toString('utf8'), tag = '<script src="/__nocturne/editing.js"></script>';
        const m = /^\s*<!doctype[^>]*>/i.exec(text);
        body = Buffer.from(m ? text.slice(0, m[0].length) + tag + text.slice(m[0].length) : tag + text, 'utf8');
    }
    res.writeHead(200, {'Content-Type': types[ext] || 'application/octet-stream', 'Cache-Control': 'no-store'});
    res.end(body);
});

/* One headless Chrome, one page, over the DevTools protocol; a profile of its own per run (two
   runs at once with one profile would meet in one Chrome), removed when the run ends. */
async function browser() {
    const profile = path.join(root, 'build/editing-wpt/profile-' + process.pid);
    fs.mkdirSync(profile, {recursive: true});
    const proc = cp.spawn(chrome, ['--headless=new', '--remote-debugging-port=0', '--user-data-dir=' + profile,
        '--no-first-run', '--no-default-browser-check', '--disable-extensions', '--disable-background-networking',
        '--disable-component-update', '--disable-sync', '--disable-default-apps', '--window-size=1280,1024', 'about:blank'],
        {stdio: ['ignore', 'ignore', 'pipe']});
    const endpoint = await new Promise((resolve, reject) => {
        let text = '';
        proc.stderr.on('data', d => { text += d; const m = /DevTools listening on (ws:\S+)/.exec(text); if (m) resolve(m[1]); });
        proc.on('exit', code => reject(new Error('Chrome exited ' + code + ': ' + text)));
    });
    const ws = new WebSocket(endpoint);
    await new Promise((resolve, reject) => { ws.onopen = resolve; ws.onerror = reject; });
    let id = 0; const waiting = new Map();
    ws.onmessage = event => {
        const msg = JSON.parse(event.data);
        if (msg.id && waiting.has(msg.id)) { const w = waiting.get(msg.id); waiting.delete(msg.id); msg.error ? w.reject(new Error(msg.error.message)) : w.resolve(msg.result); }
    };
    const send = (method, params = {}, sessionId) => new Promise((resolve, reject) => {
        const n = ++id; waiting.set(n, {resolve, reject});
        ws.send(JSON.stringify(sessionId ? {id: n, method, params, sessionId} : {id: n, method, params}));
    });
    const {targetId} = await send('Target.createTarget', {url: 'about:blank'});
    const {sessionId} = await send('Target.attachToTarget', {targetId, flatten: true});
    await send('Page.enable', {}, sessionId);
    return {
        navigate: url => send('Page.navigate', {url}, sessionId),
        async close() {
            try { ws.close(); } catch (e) {}
            await new Promise(resolve => { proc.on('exit', resolve); proc.kill(); setTimeout(resolve, 5000); });
            try { fs.rmSync(profile, {recursive: true, force: true, maxRetries: 5}); } catch (e) {}
        },
    };
}

const names = {test: ['PASS', 'FAIL', 'TIMEOUT', 'NOTRUN', 'PRECONDITION_FAILED'], harness: ['OK', 'ERROR', 'TIMEOUT', 'PRECONDITION_FAILED']};
async function runAll(page, origin, rev) {
    layer = layerScript(rev);
    const results = [];
    for (const name of pages) {
        const url = origin + '/editing/' + name;
        const result = await new Promise(resolve => {
            const timer = setTimeout(() => { pending = null; resolve(null); }, 240000);
            pending = r => { clearTimeout(timer); pending = null; resolve(r); };
            page.navigate(url).catch(() => {});
        });
        const record = result ? {test: '/editing/' + name, status: names.harness[result.status] || String(result.status), message: result.message,
            subtests: result.tests.map(t => ({name: t.name, status: names.test[t.status] || String(t.status), message: t.message})),
            errors: result.errors} : {test: '/editing/' + name, status: 'TIMEOUT', message: 'no result', subtests: [], errors: []};
        results.push(record);
        const pass = record.subtests.filter(t => t.status === 'PASS').length;
        console.log(`${rev || 'work tree'}: ${pass}/${record.subtests.length} ${record.status} /editing/${name}`);
        if (verbose) for (const t of record.subtests) if (t.status !== 'PASS') console.log('   ', t.status, t.name, '--', t.message);
        if (verbose && record.errors.length) console.log('    editing errors:', record.errors.join(' | '));
        await page.navigate('about:blank').catch(() => {});
    }
    return results;
}

(async () => {
    await new Promise(r => server.listen(0, '127.0.0.1', r));
    const origin = 'http://127.0.0.1:' + server.address().port;
    const page = await browser();
    let failed = false;
    try {
        const out = path.join(root, 'build/editing-wpt');
        fs.mkdirSync(out, {recursive: true});
        const now = await runAll(page, origin, null);
        fs.writeFileSync(path.join(out, (option('label') || 'work-tree') + '.jsonl'), now.map(r => JSON.stringify(r)).join('\n') + '\n');
        const total = rs => rs.reduce((n, r) => n + r.subtests.filter(t => t.status === 'PASS').length, 0);
        const count = rs => rs.reduce((n, r) => n + r.subtests.length, 0);
        console.log(`work tree: ${total(now)}/${count(now)} subtests pass`);
        if (base) {
            const before = await runAll(page, origin, base);
            fs.writeFileSync(path.join(out, base.replace(/[^\w.-]/g, '_') + '.jsonl'), before.map(r => JSON.stringify(r)).join('\n') + '\n');
            console.log(`${base}: ${total(before)}/${count(before)} subtests pass; work tree: ${total(now)}/${count(now)}`);
            for (let i = 0; i < now.length; i++) {
                const was = new Map(before[i].subtests.map(t => [t.name, t.status]));
                const a = before[i].subtests.filter(t => t.status === 'PASS').length, b = now[i].subtests.filter(t => t.status === 'PASS').length;
                console.log(`  ${String(a).padStart(5)} -> ${String(b).padStart(5)} / ${now[i].subtests.length}  ${now[i].test}`);
                for (const t of now[i].subtests) if (was.get(t.name) === 'PASS' && t.status !== 'PASS') {
                    failed = true;
                    console.log('    no longer passes:', t.name, '--', t.message);
                }
            }
        }
    } finally {
        await page.close();
        server.close();
    }
    console.log('editing WPT contract:', failed ? 'FAIL' : 'done');
    process.exit(failed ? 1 : 0);
})().catch(e => { console.error(e); process.exit(2); });
