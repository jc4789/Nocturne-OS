/* WHATWG Console, shared by Window and Worker globals. The embedder passes
   print(level, values) (0 log, 1 warn, 2 error; values become one line) and
   a monotonic now() in milliseconds. The namespace object's [[Prototype]] is
   an empty object over %Object.prototype% (WebIDL namespaces), and its
   operations are plain non-constructor functions whose length counts no
   optional argument. */
const consoleNamespace = (print, now) => {
    'use strict';
    const StringImpl = String, counts = new Map(), timers = new Map();
    let depth = 0;
    const labelOf = label => label === undefined ? 'default' : StringImpl(label);
    /* Formatter: substitutions are read left to right, and a value put into
       the text is not scanned again (as engines do). %% is a literal %. */
    function format(args) {
        if (args.length < 2 || typeof args[0] !== 'string') return args;
        const target = args[0];
        let text = '', from = 0, next = 1;
        for (let i = 0; i + 1 < target.length; i++) {
            if (target[i] !== '%') continue;
            const kind = target[i + 1];
            let value;
            if (kind === '%') value = '%';
            else if (next >= args.length) continue;
            else if (kind === 's') value = StringImpl(args[next++]);
            else if (kind === 'd' || kind === 'i') { const v = args[next++]; value = typeof v === 'symbol' ? 'NaN' : StringImpl(parseInt(v, 10)); }
            else if (kind === 'f') { const v = args[next++]; value = typeof v === 'symbol' ? 'NaN' : StringImpl(parseFloat(v)); }
            else if (kind === 'o' || kind === 'O') value = StringImpl(args[next++]);
            else if (kind === 'c') { next++; value = ''; }
            else continue;
            text += target.slice(from, i) + value;
            from = i + 2; i++;
        }
        return [text + target.slice(from), ...args.slice(next)];
    }
    function printer(level, values) {
        values = values.map(v => typeof v === 'symbol' ? v.toString() : v);
        if (depth) {
            const indent = '  '.repeat(depth);
            if (typeof values[0] === 'string') values[0] = indent + values[0];
            else values.unshift(indent.slice(1));
        }
        print(level, values);
    }
    function logger(level, args) { if (args.length) printer(level, format(args)); }
    function table(data, properties) {
        if (data === null || typeof data !== 'object') return null;
        const rows = Object.keys(data), columns = [];
        let values = false;
        if (Array.isArray(properties)) for (const p of properties) columns.push(StringImpl(p));
        for (const row of rows) {
            const item = data[row];
            if (item !== null && typeof item === 'object') {
                if (!Array.isArray(properties)) for (const key of Object.keys(item)) if (!columns.includes(key)) columns.push(key);
            } else values = true;
        }
        const head = ['(index)', ...columns, ...(values ? ['Value'] : [])];
        const cell = v => typeof v === 'string' ? JSON.stringify(v) : v === null || typeof v !== 'object' ? StringImpl(v) : Array.isArray(v) ? 'Array(' + v.length + ')' : '{…}';
        const body = rows.map(row => {
            const item = data[row], isObject = item !== null && typeof item === 'object';
            return [row, ...columns.map(c => isObject && c in item ? cell(item[c]) : ''), ...(values ? [isObject ? '' : cell(item)] : [])];
        });
        const width = head.map((h, i) => Math.max(h.length, ...body.map(r => r[i].length)));
        const line = r => r.map((c, i) => c.padEnd(width[i])).join(' │ ');
        return [line(head), width.map(w => '─'.repeat(w)).join('─┼─'), ...body.map(line)].join('\n');
    }
    function startGroup(data, fallback) {
        printer(0, data.length ? format(data) : [fallback]);
        depth++;
    }
    function duration(label) { return label + ': ' + +(now() - timers.get(label)).toFixed(3) + ' ms'; }
    const operations = {
        assert(condition = false, ...data) {
            if (condition) return;
            const message = 'Assertion failed';
            if (!data.length) data.push(message);
            else if (typeof data[0] !== 'string') data.unshift(message);
            else data[0] = message + ': ' + data[0];
            logger(2, data);
        },
        clear() { depth = 0; },
        debug(...data) { logger(0, data); },
        error(...data) { logger(2, data); },
        info(...data) { logger(0, data); },
        log(...data) { logger(0, data); },
        table(tabularData = undefined, properties = undefined) {
            const text = table(tabularData, properties);
            if (text === null) logger(0, [tabularData]); else printer(0, [text]);
        },
        trace(...data) {
            /* QuickJS stacks have no header line: the first frame is this one. */
            const stack = StringImpl(new Error().stack || '').split('\n').slice(1).join('\n');
            const values = data.length ? format(data) : ['console.trace'];
            if (stack) values.push(values.pop() + '\n' + stack);
            printer(0, values);
        },
        warn(...data) { logger(1, data); },
        dir(item = undefined, options = undefined) { printer(0, [item]); },
        dirxml(...data) { logger(0, data); },
        count(label = undefined) {
            label = labelOf(label);
            const count = (counts.get(label) || 0) + 1;
            counts.set(label, count);
            logger(0, [label + ': ' + count]);
        },
        countReset(label = undefined) {
            label = labelOf(label);
            if (counts.has(label)) counts.set(label, 0);
            else logger(1, ["Count for '" + label + "' does not exist"]);
        },
        group(...data) { startGroup(data, 'console.group'); },
        groupCollapsed(...data) { startGroup(data, 'console.groupCollapsed'); },
        groupEnd() { if (depth) depth--; },
        time(label = undefined) {
            label = labelOf(label);
            if (timers.has(label)) logger(1, ["Timer '" + label + "' already exists"]);
            else timers.set(label, now());
        },
        timeLog(label = undefined, ...data) {
            label = labelOf(label);
            if (!timers.has(label)) { logger(1, ["Timer '" + label + "' does not exist"]); return; }
            printer(0, [duration(label), ...data]);
        },
        timeEnd(label = undefined) {
            label = labelOf(label);
            if (!timers.has(label)) { logger(1, ["Timer '" + label + "' does not exist"]); return; }
            const text = duration(label);
            timers.delete(label);
            printer(0, [text]);
        }
    };
    const namespace = Object.create(Object.create(Object.prototype));
    for (const name of Object.keys(operations))
        Object.defineProperty(namespace, name, { value: operations[name], writable: true, enumerable: true, configurable: true });
    Object.defineProperty(namespace, Symbol.toStringTag, { value: 'console', configurable: true });
    return namespace;
};
