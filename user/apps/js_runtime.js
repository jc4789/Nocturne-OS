/* /bin/js core: module loading (ES modules, CommonJS, JSON), timers, the
 * "nocturne" module and the TypeScript entry. js.c compiles this file as the
 * body of function (host, internal) after DOMException and the Encoding API;
 * js_console.js and js_web.js follow. Everything js.c calls is on internal. */
'use strict';
const define = (object, name, value) =>
    Object.defineProperty(object, name, {value, writable: true, configurable: true});

/* ---- reporting ---- */
const isError = value => value instanceof Error ||
    (value !== null && typeof value === 'object' && typeof value.stack === 'string' && 'message' in value);
const describe = value => {
    try { return internal.inspect(value, {colors: host.isTerminal(2)}); }
    catch { try { return String(value); } catch { return '[value]'; } }
};
internal.printError = (prefix, value) => host.write(2, (isError(value) ? '' : prefix) + describe(value) + '\n');
// An exception nothing caught ends the app with status 1, like a crash; at
// the interactive prompt it is only reported.
internal.interactive = false;
internal.reportError = value => {
    internal.printError('Uncaught ', value);
    if (!internal.interactive) host.exit(1);
};
internal.unhandledRejection = value => {
    internal.printError('Uncaught (in promise) ', value);
    if (!internal.interactive) host.exit(1);
};

/* ---- timers (HTML timer tasks; js.c runs microtasks after each one) ---- */
const timers = new Map(), heap = [];
let timerId = 0, timerOrder = 0;
const before = (a, b) => a.at < b.at || (a.at === b.at && a.order < b.order);
function heapPush(t) {
    t.order = ++timerOrder;
    let i = heap.push(t) - 1;
    while (i > 0) {
        const p = (i - 1) >> 1;
        if (!before(heap[i], heap[p])) break;
        [heap[i], heap[p]] = [heap[p], heap[i]];
        i = p;
    }
}
function heapPop() {
    const top = heap[0], last = heap.pop();
    if (heap.length) {
        heap[0] = last;
        for (let i = 0;;) {
            const l = 2 * i + 1, r = l + 1;
            let m = i;
            if (l < heap.length && before(heap[l], heap[m])) m = l;
            if (r < heap.length && before(heap[r], heap[m])) m = r;
            if (m === i) break;
            [heap[i], heap[m]] = [heap[m], heap[i]];
            i = m;
        }
    }
    return top;
}
const live = t => timers.get(t.id) === t;
function startTimer(name, callback, delay, args, repeat) {
    if (typeof callback !== 'function') throw new TypeError(`${name}: the callback must be a function`);
    delay = Number(delay);
    delay = delay > 0 && delay < 2147483648 ? delay : 0;
    const t = {id: ++timerId, callback, args, at: host.now() + delay, every: repeat ? Math.max(delay, 1) : 0};
    timers.set(t.id, t);
    heapPush(t);
    return t.id;
}
const stopTimer = id => { timers.delete(Number(id)); };
define(globalThis, 'setTimeout', (callback, delay, ...args) => startTimer('setTimeout', callback, delay, args, false));
define(globalThis, 'setInterval', (callback, delay, ...args) => startTimer('setInterval', callback, delay, args, true));
define(globalThis, 'clearTimeout', stopTimer);
define(globalThis, 'clearInterval', stopTimer);
// The earliest live deadline (host.now() milliseconds), or -1 when none waits.
internal.nextTimer = () => {
    while (heap.length && !live(heap[0])) heapPop();
    return heap.length ? heap[0].at : -1;
};
// Runs one due timer; false when none is due.
internal.fireTimer = now => {
    if (internal.nextTimer() < 0 || heap[0].at > now) return false;
    const t = heapPop();
    if (t.every) { t.at = now + t.every; heapPush(t); }
    else timers.delete(t.id);
    try { t.callback(...t.args); }
    catch (e) { internal.reportError(e); }
    return true;
};
define(globalThis, 'queueMicrotask', callback => {
    if (typeof callback !== 'function') throw new TypeError('queueMicrotask: the callback must be a function');
    Promise.resolve().then(() => { try { callback(); } catch (e) { internal.reportError(e); } });
});

/* ---- performance (High Resolution Time, User Timing marks) ---- */
{
    const origin = Date.now(), start = host.now(), entries = [];
    const now = () => host.now() - start;
    const time = (value, name) => {
        if (value === undefined) return now();
        if (typeof value === 'number') return value;
        const mark = entries.findLast(e => e.entryType === 'mark' && e.name === String(value));
        if (!mark) throw new DOMException(`The mark '${value}' does not exist.`, 'SyntaxError');
        return mark.startTime;
    };
    const performance = {
        timeOrigin: origin,
        now,
        mark(name, options = {}) {
            const e = {name: String(name), entryType: 'mark', startTime: options.startTime ?? now(), duration: 0, detail: options.detail ?? null};
            entries.push(e);
            return e;
        },
        measure(name, start, end) {
            let detail = null;
            if (start !== null && typeof start === 'object') ({start, end, detail = null} = start);
            const s = start === undefined ? 0 : time(start), f = time(end);
            const e = {name: String(name), entryType: 'measure', startTime: s, duration: f - s, detail};
            entries.push(e);
            return e;
        },
        getEntries: () => entries.slice(),
        getEntriesByName: (name, type) => entries.filter(e => e.name === name && (type === undefined || e.entryType === type)),
        getEntriesByType: type => entries.filter(e => e.entryType === type),
        clearMarks(name) { for (let i = entries.length; i--;) if (entries[i].entryType === 'mark' && (name === undefined || entries[i].name === name)) entries.splice(i, 1); },
        clearMeasures(name) { for (let i = entries.length; i--;) if (entries[i].entryType === 'measure' && (name === undefined || entries[i].name === name)) entries.splice(i, 1); },
        toJSON: () => ({timeOrigin: origin}),
    };
    define(globalThis, 'performance', performance);
}

/* ---- large Web APIs load on first use: each costs parse time at startup ---- */
function lazy(names, chunk) {
    let loaded = false;
    const load = () => {
        if (loaded) return;
        loaded = true;
        for (const name of names) delete globalThis[name];
        host.loadChunk(chunk);
    };
    for (const name of names) Object.defineProperty(globalThis, name, {
        configurable: true,
        get() { load(); return globalThis[name]; },
        set(value) { load(); define(globalThis, name, value); },
    });
}
lazy(['URL', 'URLSearchParams'], 'url');
lazy(['crypto', 'Crypto', 'CryptoKey', 'SubtleCrypto', 'isSecureContext'], 'crypto');

/* ---- paths ---- */
function normalize(path) {
    const out = [];
    for (const part of path.split('/')) {
        if (!part || part === '.') continue;
        if (part === '..') out.pop();
        else out.push(part);
    }
    return '/' + out.join('/');
}
const join = (...parts) => normalize(parts.join('/'));
const dirname = path => { const i = path.lastIndexOf('/'); return i <= 0 ? '/' : path.slice(0, i); };
const basename = path => path.slice(path.lastIndexOf('/') + 1);
const extension = path => { const name = basename(path), i = name.lastIndexOf('.'); return i > 0 ? name.slice(i) : ''; };

/* ---- the "nocturne" module ---- */
const kinds = ['', 'document', 'folder', '', '', '', '', '', 'device'];
const entry = raw => raw && {name: raw.name, path: raw.path, kind: kinds[raw.kind] || 'device', size: raw.size, modified: new Date(raw.modified * 1000)};
const bytes = data => {
    if (typeof data === 'string' || data instanceof ArrayBuffer) return data;
    if (ArrayBuffer.isView(data)) return data instanceof Uint8Array ? data : new Uint8Array(data.buffer, data.byteOffset, data.byteLength);
    return String(data);
};
const info = path => entry(host.info(String(path)));
function createFolder(path, options) {
    const full = host.complete(String(path));
    if (!options?.recursive) return void host.createFolder(full);
    let at = '';
    for (const part of full.split('/').filter(Boolean)) {
        at += '/' + part;
        const found = host.info(at);
        if (!found || found.kind !== 2) host.createFolder(at);
    }
}
function remove(path, options) {
    const full = host.complete(String(path));
    if (options?.recursive && host.info(full)?.kind === 2)
        for (const child of host.list(full)) remove(child.path, options);
    host.remove(full);
}
const streams = {input: 0, output: 1, error: 2};
const nocturne = {
    args: Object.freeze(host.args.slice()),
    exit: code => host.exit(code === undefined ? 0 : Number(code) | 0),
    cwd: () => host.cwd(),
    chdir: path => host.chdir(String(path)),
    getSetting: name => host.getSetting(String(name)),
    setSetting: (name, value) => host.setSetting(String(name), String(value)),
    removeSetting: name => host.removeSetting(String(name)),
    info,
    exists: path => info(path) !== null,
    list: (path = '.') => host.list(String(path)).map(entry),
    readText: path => host.readFile(String(path), true),
    readBytes: path => new Uint8Array(host.readFile(String(path), false)),
    writeText: (path, text) => host.writeFile(String(path), String(text), false),
    writeBytes: (path, data) => host.writeFile(String(path), bytes(data), false),
    appendText: (path, text) => host.writeFile(String(path), String(text), true),
    appendBytes: (path, data) => host.writeFile(String(path), bytes(data), true),
    createFolder,
    remove,
    move: (from, to) => host.move(String(from), String(to)),
    copy: (from, to) => host.writeFile(String(to), host.readFile(String(from), false), false),
    resolvePath: path => host.complete(String(path)),
    write: data => host.write(1, bytes(data)),
    writeError: data => host.write(2, bytes(data)),
    readLine: (prompt = '') => host.readLine(String(prompt)),
    readAll: () => host.readAll(),
    isTerminal: (stream = 'output') => host.isTerminal(streams[stream] ?? 1),
    run: (program, args = []) => host.run(String(program), Array.from(args, String)),
    sleep: ms => new Promise(resolve => setTimeout(resolve, ms)),
};
define(nocturne, 'default', nocturne);
Object.freeze(nocturne);
internal.nocturne = nocturne;

/* ---- module resolution: relative and absolute paths, node_modules packages ---- */
const extensions = ['.js', '.mjs', '.cjs', '.json'];
const kindOf = path => { try { return host.info(path)?.kind ?? 0; } catch { return 0; } };
const packages = new Map();
function readPackage(folder) {
    if (packages.has(folder)) return packages.get(folder);
    let pkg = null;
    if (kindOf(folder + '/package.json') === 1) {
        try { pkg = JSON.parse(host.readFile(folder + '/package.json', true)); }
        catch (e) { throw new SyntaxError(`${folder}/package.json: ${e.message}`); }
        if (pkg === null || typeof pkg !== 'object') pkg = {};
    }
    packages.set(folder, pkg);
    return pkg;
}
function asFile(path) {
    const kind = kindOf(path);
    if (kind === 1) return path;
    for (const ext of extensions) if (kindOf(path + ext) === 1) return path + ext;
    if (kind === 2) {
        const pkg = readPackage(path);
        if (typeof pkg?.main === 'string') {
            const main = asFile(join(path, pkg.main));
            if (main) return main;
        }
        for (const ext of extensions) if (kindOf(path + '/index' + ext) === 1) return path + '/index' + ext;
    }
    return null;
}
// Package "exports" (conditions: nocturne, import or require, default).
function exportTarget(target, conditions, star) {
    if (typeof target === 'string') return target.replaceAll('*', star);
    if (Array.isArray(target)) {
        for (const t of target) { const r = exportTarget(t, conditions, star); if (r) return r; }
        return null;
    }
    if (target && typeof target === 'object')
        for (const [key, value] of Object.entries(target))
            if (key === 'default' || conditions.includes(key)) {
                const r = exportTarget(value, conditions, star);
                if (r) return r;
            }
    return null;
}
function packageExport(exports, sub, conditions) {
    if (typeof exports === 'string' || Array.isArray(exports) ||
        (exports && typeof exports === 'object' && !Object.keys(exports).some(k => k.startsWith('.'))))
        exports = {'.': exports};
    if (Object.hasOwn(exports, sub)) return exportTarget(exports[sub], conditions, '');
    for (const key of Object.keys(exports)) {
        const star = key.indexOf('*');
        if (star < 0) continue;
        const head = key.slice(0, star), tail = key.slice(star + 1);
        if (sub.startsWith(head) && sub.endsWith(tail) && sub.length >= key.length - 1)
            return exportTarget(exports[key], conditions, sub.slice(head.length, sub.length - tail.length));
    }
    return null;
}
function resolvePackage(base, spec, conditions) {
    const m = /^((?:@[^/]+\/)?[^/]+)(\/.*)?$/.exec(spec);
    if (!m) return null;
    const name = m[1], sub = m[2] ? '.' + m[2] : '.';
    for (let folder = dirname(base); ; folder = dirname(folder)) {
        const root = join(folder, 'node_modules', name);
        if (kindOf(root) === 2) {
            const pkg = readPackage(root);
            if (pkg?.exports !== undefined && pkg.exports !== null) {
                const target = packageExport(pkg.exports, sub, conditions);
                if (!target) throw notFound(spec, base, `'${sub}' is not exported by ${root}/package.json`);
                return asFile(join(root, target));
            }
            return asFile(sub === '.' ? root : join(root, sub));
        }
        if (folder === '/') return null;
    }
}
function notFound(spec, base, why) {
    let message = `Cannot find module '${spec}' imported from ${base}`;
    if (why) message += ` (${why})`;
    else if (/^node:/.test(spec) || ['fs', 'path', 'os', 'process', 'child_process', 'http', 'https', 'events', 'util', 'buffer', 'stream', 'url', 'crypto'].includes(spec))
        message += '. Node.js modules are not part of Nocturne: use the "nocturne" module and the Web APIs (fetch, URL, TextEncoder, crypto)';
    const e = new Error(message);
    e.code = 'MODULE_NOT_FOUND';
    return e;
}
/* js file.ts runs what tsgo wrote under tsOut (the source's own path inside
   it): packages, and files tsgo did not write, are found beside the source. */
const tsOut = '/tmp/js-ts';
const fromTs = path => path.startsWith(tsOut + '/');
const besideSource = path => fromTs(path) ? path.slice(tsOut.length) : path;
function tsSource(path) {
    if (!fromTs(path)) return path;
    const original = besideSource(path), ts = original.replace(/\.([cm]?)js$/, '.$1ts');
    return ts !== original && kindOf(ts) === 1 ? ts : original;
}
function resolve(base, spec, conditions) {
    if (spec === 'nocturne' || spec === 'nocturne:') return 'nocturne';
    if (spec.startsWith('file://')) spec = decodeURIComponent(spec.slice(7));
    let found;
    if (spec.startsWith('/')) found = asFile(normalize(spec));
    else if (spec === '.' || spec === '..' || spec.startsWith('./') || spec.startsWith('../')) {
        found = asFile(join(base.startsWith('/') ? dirname(base) : host.cwd(), spec));
        if (!found && fromTs(base)) found = asFile(join(dirname(besideSource(base)), spec));
    } else found = resolvePackage(base.startsWith('/') ? besideSource(base) : host.cwd() + '/', spec, conditions);
    if (!found) throw notFound(spec, base);
    return found;
}
const importConditions = ['nocturne', 'import'], requireConditions = ['nocturne', 'require'];
// QuickJS's module normalizer: base is the importing module's name.
internal.resolve = (base, spec) => resolve(base, spec, importConditions);

/* ---- formats: .mjs and .cjs by name, .js by package.json "type", else by syntax ---- */
const sources = new Map(), compiled = new Map();
const source = path => {
    let text = sources.get(path);
    if (text === undefined) {
        text = host.readFile(path, true);
        if (text.charCodeAt(0) === 0xfeff) text = text.slice(1);
        sources.set(path, text);
    }
    return text;
};
function packageType(folder) {
    for (;; folder = dirname(folder)) {
        const pkg = readPackage(folder);
        if (pkg) return pkg.type;
        if (folder === '/') return undefined;
    }
}
const cjsWrapper = text => text.startsWith('#!') ? '//' + text : text;
internal.moduleFormat = path => {
    const ext = extension(path);
    if (ext === '.json') return 'json';
    if (ext === '.mjs') return 'module';
    if (ext === '.cjs') return 'commonjs';
    const type = packageType(dirname(path));
    if (type === 'module') return 'module';
    if (type === 'commonjs') return 'commonjs';
    const text = source(path);
    if (host.detectModule(text)) return 'module';
    // Top-level await without import/export only parses as a module.
    try { compiled.set(path, host.compileFunction(cjsWrapper(text), path)); }
    catch (e) { if (e instanceof SyntaxError) return 'module'; throw e; }
    return 'commonjs';
};
internal.source = source;

/* ---- CommonJS ---- */
const cache = Object.create(null);
let mainModule;
class Module {
    constructor(id, parent) {
        this.id = id;
        this.filename = id;
        this.path = dirname(id);
        this.exports = {};
        this.parent = parent ?? null;
        this.loaded = false;
        this.children = [];
        const require = spec => requireFrom(this, String(spec));
        require.resolve = spec => resolve(this.filename, String(spec), requireConditions);
        require.cache = cache;
        Object.defineProperty(require, 'main', {get: () => mainModule, enumerable: true});
        this.require = require;
    }
}
function requireFrom(parent, spec) {
    const path = resolve(parent.filename, spec, requireConditions);
    return path === 'nocturne' ? nocturne : load(path, parent);
}
function load(path, parent, main) {
    const cached = cache[path];
    if (cached) return cached.exports;
    const format = internal.moduleFormat(path);
    if (format === 'module') return host.awaitSync(import(path));
    const module = new Module(path, parent);
    if (main) { module.id = '.'; mainModule = module; }
    cache[path] = module;
    parent?.children.push(module);
    try {
        if (format === 'json') module.exports = JSON.parse(source(path));
        else {
            const fn = compiled.get(path) ?? host.compileFunction(cjsWrapper(source(path)), path);
            compiled.delete(path);
            fn.call(module.exports, module.exports, module.require, module, path, dirname(path));
        }
    } catch (e) {
        delete cache[path];
        throw e;
    }
    module.loaded = true;
    return module.exports;
}
internal.runMain = path => { load(path, null, true); };
// require() for code from -e, the input and the interactive prompt.
internal.requireFrom = path => new Module(path, null).require;

/* What an ES module sees of a CommonJS module: default is module.exports, the
   names come from the source (exports.x =, defineProperty, re-exports). */
function exportNames(path, seen = new Set()) {
    if (seen.has(path)) return [];
    seen.add(path);
    const text = source(path), names = new Set();
    for (const m of text.matchAll(/\b(?:module\.)?exports\.([A-Za-z_$][\w$]*)\s*=(?!=)/g)) names.add(m[1]);
    for (const m of text.matchAll(/\bdefineProperty\(\s*(?:module\.)?exports\s*,\s*["']([^"']+)["']/g)) names.add(m[1]);
    const literal = /\bmodule\.exports\s*=\s*\{([^{}]*)\}/.exec(text);
    if (literal)
        for (const part of literal[1].split(',')) {
            const key = /^\s*(?:["']([^"']+)["']|([A-Za-z_$][\w$]*))/.exec(part);
            if (key) names.add(key[1] ?? key[2]);
        }
    const reexports = [...text.matchAll(/__exportStar\(\s*require\(\s*["']([^"']+)["']\s*\)/g),
                       ...text.matchAll(/\bmodule\.exports\s*=\s*require\(\s*["']([^"']+)["']\s*\)/g)];
    for (const m of reexports) {
        try {
            const target = resolve(path, m[1], requireConditions);
            if (target !== 'nocturne' && internal.moduleFormat(target) === 'commonjs')
                for (const name of exportNames(target, seen)) names.add(name);
            else if (target === 'nocturne') for (const name of Object.keys(nocturne)) names.add(name);
        } catch {}
    }
    names.delete('default');
    names.delete('__esModule');
    return [...names];
}
internal.exportNames = (path, format) =>
    path === 'nocturne' ? Object.keys(nocturne) : format === 'json' ? [] : exportNames(path);
internal.namespace = (path, format) => {
    if (path === 'nocturne') return nocturne;
    const value = format === 'json' ? JSON.parse(source(path)) : load(path, null);
    const ns = {default: value};
    if (format !== 'json' && value !== null && (typeof value === 'object' || typeof value === 'function'))
        for (const name of exportNames(path)) { try { ns[name] = value[name]; } catch {} }
    return ns;
};
internal.fillMeta = (meta, path, main) => {
    const shown = tsSource(path);
    meta.url = 'file://' + encodeURI(shown).replaceAll('?', '%3F').replaceAll('#', '%23');
    meta.filename = shown;
    meta.dirname = dirname(shown);
    meta.main = main;
    meta.resolve = spec => {
        const found = resolve(path, String(spec), importConditions);
        return found === 'nocturne' ? 'nocturne:' : 'file://' + encodeURI(found);
    };
};

/* ---- TypeScript: js file.ts type-checks and compiles with tsgo, then runs ---- */
internal.compileTypeScript = path => {
    const args = [path, '--ignoreConfig', '--rootDir', '/', '--outDir', tsOut,
        '--module', 'esnext', '--target', 'esnext', '--moduleResolution', 'bundler',
        '--moduleDetection', 'force', '--strict', '--skipLibCheck', '--noEmitOnError',
        '--sourceMap', 'false', '--declaration', 'false'];
    if (kindOf('/node_modules/@types/nocturne') === 2) args.push('--types', 'nocturne');
    if (host.isTerminal(2)) args.push('--pretty');
    const status = host.run('tsgo', args);
    if (status !== 0) host.exit(status < 0 ? 1 : status);
    const js = path.replace(/\.([cm]?)ts$/, (_, m) => `.${m}js`);
    return tsOut + js;
};

/* ---- js --init: a TypeScript project for tsgo and js ---- */
internal.init = folder => {
    const at = host.complete(folder ?? '.');
    createFolder(at, {recursive: true});
    const name = basename(at) || 'app';
    const files = {
        'package.json': JSON.stringify({name: name.toLowerCase().replace(/[^a-z0-9._-]+/g, '-'), version: '1.0.0', type: 'module', main: 'dist/main.js'}, null, 2) + '\n',
        'tsconfig.json': JSON.stringify({compilerOptions: {
            rootDir: 'src', outDir: 'dist', module: 'nodenext', target: 'esnext', types: ['nocturne'],
            strict: true, noUncheckedIndexedAccess: true, exactOptionalPropertyTypes: true,
            verbatimModuleSyntax: true, isolatedModules: true, moduleDetection: 'force', skipLibCheck: true,
        }}, null, 2) + '\n',
        'src/main.ts': [
            'import { args } from "nocturne";',
            '',
            'const name: string = args[0] ?? "Nocturne";',
            'console.log(`Hello, ${name}!`);',
            '',
        ].join('\n'),
    };
    for (const [file, text] of Object.entries(files)) {
        const full = join(at, file);
        if (kindOf(full)) { host.write(1, `kept     ${full}\n`); continue; }
        createFolder(dirname(full), {recursive: true});
        host.writeFile(full, text, false);
        host.write(1, `created  ${full}\n`);
    }
    host.write(1, `\nBuild with tsgo and run the result:\n  cd ${at}\n  tsgo\n  js dist/main.js\n` +
        `Or type-check and run one file at once: js src/main.ts\n`);
};

/* ---- the interactive prompt ---- */
internal.replResult = value => {
    if (value !== undefined) host.write(1, internal.inspect(value, {colors: host.isTerminal(1)}) + '\n');
};
