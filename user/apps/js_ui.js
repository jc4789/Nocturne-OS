/* The "nocturne/ui" module of /bin/js: windows with declarative widgets.
 * A program describes its window with a view function that returns a tree of
 * widgets (column, row, text, button, segmented, toggle, slider, textField,
 * ...). This module only translates: it encodes the tree in the view format of
 * the system's interface engine (libc/ui.c, docs/nocturne-native-abi.md §11),
 * which lays it out, paints it and runs hover, presses, focus, scrolling and
 * animations, and it turns the engine's events back into the program's
 * callbacks, then runs the view again. C and Go programs use the same engine.
 *
 * js.c compiles this file as the body of function (host, internal) when a
 * program first imports "nocturne/ui". */
'use strict';

/* ---- bytes ---- */
class Writer {
    #buf = new Uint8Array(256);
    #view = new DataView(this.#buf.buffer);
    length = 0;
    #room(n) {
        if (this.length + n <= this.#buf.length) return;
        let size = this.#buf.length * 2;
        while (size < this.length + n) size *= 2;
        const buf = new Uint8Array(size);
        buf.set(this.#buf.subarray(0, this.length));
        this.#buf = buf;
        this.#view = new DataView(buf.buffer);
    }
    u8(v) { this.#room(1); this.#buf[this.length++] = v; return this; }
    u16(v) { this.#room(2); this.#view.setUint16(this.length, v, true); this.length += 2; return this; }
    u32(v) { this.#room(4); this.#view.setUint32(this.length, v >>> 0, true); this.length += 4; return this; }
    f64(v) { this.#room(8); this.#view.setFloat64(this.length, v, true); this.length += 8; return this; }
    patch16(at, v) { this.#view.setUint16(at, v, true); }
    bytes(b) { this.#room(b.length); this.#buf.set(b, this.length); this.length += b.length; return this; }
    /* UTF-8 without the length */
    utf8(s) {
        this.#room(s.length * 3);
        const b = this.#buf;
        let at = this.length;
        for (let i = 0; i < s.length; i++) {
            let c = s.charCodeAt(i);
            if (c >= 0xD800 && c < 0xDC00 && i + 1 < s.length) {
                const d = s.charCodeAt(i + 1);
                if (d >= 0xDC00 && d < 0xE000) { c = 0x10000 + ((c - 0xD800) << 10) + (d - 0xDC00); i++; }
            }
            if (c < 0x80) b[at++] = c;
            else if (c < 0x800) { b[at++] = 0xC0 | c >> 6; b[at++] = 0x80 | (c & 63); }
            else if (c < 0x10000) { b[at++] = 0xE0 | c >> 12; b[at++] = 0x80 | (c >> 6 & 63); b[at++] = 0x80 | (c & 63); }
            else { b[at++] = 0xF0 | c >> 18; b[at++] = 0x80 | (c >> 12 & 63); b[at++] = 0x80 | (c >> 6 & 63); b[at++] = 0x80 | (c & 63); }
        }
        this.length = at;
        return this;
    }
    str(s) {
        const at = this.length;
        this.u32(0).utf8(String(s));
        new DataView(this.#buf.buffer).setUint32(at, this.length - at - 4, true);
        return this;
    }
    /* u8 length + bytes, for names */
    name(s) { this.u8(s.length); for (let i = 0; i < s.length; i++) this.u8(s.charCodeAt(i)); return this; }
    done() { return this.#buf.buffer.slice(0, this.length); }
    get array() { return this.#buf.subarray(0, this.length); }
}

class Reader {
    #view; #at = 0;
    constructor(buffer) { this.#view = new DataView(buffer); this.buffer = buffer; }
    u8() { return this.#view.getUint8(this.#at++); }
    u32() { const v = this.#view.getUint32(this.#at, true); this.#at += 4; return v; }
    i32() { const v = this.#view.getInt32(this.#at, true); this.#at += 4; return v; }
    f64() { const v = this.#view.getFloat64(this.#at, true); this.#at += 8; return v; }
    str() {
        const n = this.u32(), s = utf8Decode(new Uint8Array(this.buffer, this.#at, n));
        this.#at += n;
        return s;
    }
    name() { const n = this.u8(); let s = ''; for (let i = 0; i < n; i++) s += String.fromCharCode(this.u8()); return s; }
}

function utf8Decode(b) {
    let s = '';
    for (let i = 0; i < b.length;) {
        const c = b[i];
        let cp, n;
        if (c < 0x80) { cp = c; n = 1; }
        else if (c < 0xE0) { cp = c & 0x1F; n = 2; }
        else if (c < 0xF0) { cp = c & 0x0F; n = 3; }
        else { cp = c & 0x07; n = 4; }
        for (let k = 1; k < n; k++) cp = cp << 6 | (b[i + k] & 0x3F);
        s += String.fromCodePoint(cp);
        i += n;
    }
    return s;
}

const request = w => {
    const reply = host.uiRequest(w.done());
    return reply ? new Reader(reply) : null;
};

/* ---- look ---- */
const theme = (() => {
    const r = request(new Writer().u8(0x51)), out = {};
    if (r && r.u8() === 0x71) for (let n = r.u32(); n > 0; n--) { const name = r.name(); out[name] = r.f64(); }
    return Object.freeze(out);
})();

/* ---- colour helpers ---- */
function mix(a, b, t) {
    if (t <= 0) return a >>> 0;
    if (t >= 1) return b >>> 0;
    let out = 0;
    for (let shift = 0; shift < 32; shift += 8) {
        const ca = (a >>> shift) & 255, cb = (b >>> shift) & 255;
        out |= Math.round(ca + (cb - ca) * t) << shift;
    }
    return out >>> 0;
}
const withAlpha = (colour, alpha) => ((Math.round(alpha * 255) & 255) << 24 | (colour & 0xFFFFFF)) >>> 0;

/* UI_TEXT_* bits of {weight, mono} */
const textStyle = opts => (opts.weight ? 1 : 0) | (opts.mono ? 2 : 0);

/* [width, line height, baseline] of a text, from the engine */
function measure(text, size, weight) {
    const r = request(new Writer().u8(0x4D).u32(0).f64(size).u32(weight).str(text));
    if (!r || r.u8() !== 0x6D) return [0, 0, 0];
    r.u32();
    return [r.f64(), r.f64(), r.f64()];
}

/* ---- widgets ---- */
/* A widget is a plain object: kind, props, children; the engine applies the defaults. */
function flatten(children, out = []) {
    for (const child of children) {
        if (child === null || child === undefined || child === false || child === true) continue;
        if (Array.isArray(child)) flatten(child, out);
        else out.push(typeof child === 'string' || typeof child === 'number' ? text(String(child)) : child);
    }
    return out;
}
const widget = (kind, props, children = []) => ({kind, props: props ?? {}, children: flatten(children)});
const isProps = value => value !== null && typeof value === 'object' && !Array.isArray(value) && !('kind' in value);

function container(kind) {
    return (props, ...children) => isProps(props) ? widget(kind, props, children) : widget(kind, {}, [props, ...children]);
}
const column = container('column');
const row = container('row');
const scroll = container('scroll');
const card = container('card');
function text(content, props) { return widget('text', {...props, text: String(content)}); }
function heading(content, props) { return text(content, {style: 'heading', ...props}); }
function title(content, props) { return text(content, {style: 'title', ...props}); }
function caption(content, props) { return text(content, {style: 'caption', ...props}); }
function button(label, onClick, props) { return widget('button', {...props, text: String(label), onClick}); }
function toggle(on, onChange, props) { return widget('toggle', {...props, on: !!on, onChange}); }
function checkbox(checked, label, onChange, props) {
    return widget('checkbox', {...props, on: !!checked, text: String(label), onChange});
}
function slider(value, min, max, onChange, props) { return widget('slider', {...props, value, min, max, onChange}); }
function segmented(options, value, onChange, props) {
    const list = options.map(o => typeof o === 'object' && o !== null ? {label: String(o.label), value: o.value} : {label: String(o), value: o});
    return widget('segmented', {...props, choices: list, value, onChange});
}
function textField(value, onChange, props) { return widget('field', {...props, value: String(value), onChange}); }
function listItem(label, selected, onClick, props) {
    return widget('item', {...props, text: String(label), selected: !!selected, onClick});
}
function icon(name, props) { return widget('icon', {...props, icon: name}); }
function divider(props) { return widget('divider', props); }
function spacer(size) { return widget('spacer', size === undefined ? {} : {size}); }
function progress(value, props) { return widget('progress', {...props, value}); }
/* Free drawing: draw(painter, width, height) records what to paint inside the widget's box. */
function canvas(draw, props) { return widget('canvas', {...props, draw}); }

/* ---- encoding ---- */
const CONTAINERS = new Set(['column', 'row', 'scroll', 'card']);
const SKIP = new Set(['draw', 'choices']);

function prop(w, name, value) {
    if (typeof value === 'boolean') value = value ? 1 : 0;
    if (typeof value === 'number') w.name(name).u8(1).f64(value);
    else if (typeof value === 'string') w.name(name).u8(2).str(value);
    else if (Array.isArray(value) && value.every(v => typeof v === 'number')) {
        w.name(name).u8(3).u32(value.length);
        for (const v of value) w.f64(v);
    } else if (Array.isArray(value) && value.every(v => typeof v === 'string')) {
        w.name(name).u8(4).u32(value.length);
        for (const v of value) w.str(v);
    } else return 0;
    return 1;
}

/* What a canvas draws, as the engine's drawing operations. */
function painter(ops) {
    const num = (op, ...args) => { ops.u8(op); for (const a of args) ops.f64(Number(a) || 0); };
    return Object.freeze({
        rect: (x, y, w, h, colour) => num(1, x, y, w, h, colour >>> 0),
        round: (x, y, w, h, r, colour, bottom) => num(2, x, y, w, h, r, colour >>> 0, (bottom ?? colour) >>> 0),
        stroke: (x, y, w, h, r, width, colour) => num(3, x, y, w, h, r, width, colour >>> 0),
        shadow: (x, y, w, h, r, blur, colour) => num(4, x, y, w, h, r, blur, colour >>> 0),
        circle: (cx, cy, r, colour, bottom) => num(2, cx - r, cy - r, 2 * r, 2 * r, r, colour >>> 0, (bottom ?? colour) >>> 0),
        line: (x0, y0, x1, y1, width, colour) => num(5, x0, y0, x1, y1, width, colour >>> 0),
        text: (x, y, content, opts = {}) => {
            const size = opts.size ?? theme.fontSize, weight = textStyle(opts), s = String(content);
            num(6, x, y, size, weight, (opts.colour ?? theme.text) >>> 0);
            ops.str(s);
            return measure(s, size, weight)[0];
        },
        measure: (content, opts = {}) => measure(String(content), opts.size ?? theme.fontSize, textStyle(opts))[0],
        icon: (name, x, y, size, colour) => { num(7, x, y, size, (colour ?? theme.textDim) >>> 0); ops.str(String(name)); },
    });
}

/* Writes a widget and its children in pre-order; handlers[i] is the widget at place i. */
function encode(w, node, handlers, app) {
    const index = handlers.length;
    handlers.push(node);
    const p = node.props;
    w.name(node.kind);
    const counts = w.length;
    w.u16(0).u16(node.children.length);
    let n = 0;
    for (const key in p) {
        const value = p[key];
        if (SKIP.has(key) || (key === 'value' && node.kind === 'segmented') || value === undefined || value === null || typeof value === 'function') continue;
        n += prop(w, key === 'align' && !CONTAINERS.has(node.kind) ? 'self' : key, value);
    }
    if (p.onClick && CONTAINERS.has(node.kind)) n += prop(w, 'clickable', 1);
    if (node.kind === 'segmented') {
        n += prop(w, 'options', p.choices.map(o => o.label));
        n += prop(w, 'selected', p.choices.findIndex(o => Object.is(o.value, p.value)));
    }
    if (node.kind === 'canvas' && (p.onPointer || p.onWheel)) n += prop(w, 'pointer', 1);
    if (node.kind === 'canvas' && typeof p.draw === 'function') {
        const size = app.canvasSize(p.key ?? index, p);
        const ops = new Writer();
        try { p.draw(painter(ops), size[0], size[1]); }
        catch (e) { internal.reportError?.(e); }
        w.name('draw').u8(5).u32(ops.length).bytes(ops.array);
        n += 1 + prop(w, 'drawn', size);
    }
    w.patch16(counts, n);
    for (const child of node.children) encode(w, child, handlers, app);
}

/* ---- apps ---- */
const EVENT = {CLICK: 1, TOGGLE: 2, SLIDE: 3, CHOOSE: 4, TEXT: 5, SUBMIT: 6, KEY: 7, CLOSE: 8, CANVAS: 9, RESIZE: 10, POINTER: 11, WHEEL: 12, FOCUS: 13};
const MOD_SHIFT = 1, MOD_CTRL = 2, MOD_ALT = 4, DONE = 1, GONE = 2, DOWN = 4, UP = 8, CANCEL = 16;
const windows = new Map();
let nextWindow = 1;

class App {
    #view; #options; #renderTimer = 0; #views = new Map(); #sent = 0; #canvases = new Map();
    #width; #height;
    constructor(options) {
        this.#options = options;
        this.#view = options.view;
        this.id = nextWindow++;
        this.closed = false;
        const flags = (options.resizable ? 1 : 0) | (options.x === undefined ? 4 : 0);
        const r = request(new Writer().u8(0x4F).u32(this.id).u32(Math.round(options.width)).u32(Math.round(options.height))
                                      .u32(flags).u32(options.background ?? 0).str(String(options.title ?? '')));
        if (!r || r.u8() !== 0x6F || (r.u32(), r.i32()) < 0) throw new Error('cannot open a window (is the desktop running?)');
        this.#width = r.u32();
        this.#height = r.u32();
        windows.set(this.id, this);
        this.render();
    }
    get width() { return this.#width; }
    get height() { return this.#height; }
    setTitle(text) { if (!this.closed) host.uiSend(new Writer().u8(0x54).u32(this.id).str(String(text)).done()); }
    /* the size a canvas was last placed at (its own size until the engine says) */
    canvasSize(key, p) {
        return this.#canvases.get(key) ?? [typeof p.width === 'number' ? p.width : 100, typeof p.height === 'number' ? p.height : 100];
    }
    invalidate() {
        if (this.closed || this.#renderTimer) return;
        this.#renderTimer = setTimeout(() => { this.#renderTimer = 0; this.render(); }, 0);
    }
    close() {
        if (this.closed) return;
        this.closed = true;
        clearTimeout(this.#renderTimer);
        windows.delete(this.id);
        host.uiSend(new Writer().u8(0x43).u32(this.id).done());
    }
    render() {
        if (this.closed) return;
        const root = flatten([this.#view(this)]);
        const tree = root.length === 1 ? root[0] : column({}, root);
        const w = new Writer().u8(0x56).u32(this.id), handlers = [];
        encode(w, tree, handlers, this);
        const result = host.uiSend(w.done());
        if (result < 0) throw new Error(`the interface engine refused the view (${result})`);
        this.#views.set(++this.#sent, handlers);
        this.#views.delete(this.#sent - 8); /* events refer to recent views */
    }
    event(r) {
        const view = r.u32(), type = r.u32(), index = r.u32(), mods = r.u32(), flags = r.u32();
        const a = r.f64(), b = r.f64(), str = r.str();
        const node = this.#views.get(view)?.[index], p = node?.props ?? {};
        switch (type) {
        case EVENT.CLICK: p.onClick?.(); break;
        case EVENT.TOGGLE: p.onChange?.(!!a); break;
        case EVENT.SLIDE: p.onChange?.(a, !!(flags & DONE)); break;
        case EVENT.CHOOSE: { const o = p.choices?.[a]; if (o) p.onChange?.(o.value); break; }
        case EVENT.TEXT: p.onChange?.(str); break;
        case EVENT.SUBMIT: p.onSubmit?.(str); break;
        case EVENT.KEY: {
            const ev = {key: str, shift: !!(mods & MOD_SHIFT), ctrl: !!(mods & MOD_CTRL), alt: !!(mods & MOD_ALT)};
            if (flags & UP) this.#options.onKeyUp?.(ev, this);
            else if (this.#options.onKey?.(ev, this) !== true && str === 'Escape' && this.#options.closeOnEscape) this.#closeRequested(false);
            break;
        }
        case EVENT.CLOSE: this.#closeRequested(!!(flags & GONE)); return;
        case EVENT.CANVAS:
            if (node) this.#canvases.set(p.key ?? index, [a, b]);
            break;
        case EVENT.RESIZE: this.#width = a; this.#height = b; break;
        case EVENT.POINTER:
            p.onPointer?.({x: a, y: b, down: !!(flags & DOWN), up: !!(flags & UP), buttons: flags >> 8 & 255,
                           shift: !!(mods & MOD_SHIFT), ctrl: !!(mods & MOD_CTRL), alt: !!(mods & MOD_ALT)});
            break;
        case EVENT.WHEEL: p.onWheel?.(a); break;
        case EVENT.FOCUS:
            if (node) p.onFocus?.(!!a, !!(flags & CANCEL));
            else if (index === 0xFFFFFFFF) this.#options.onFocus?.(!!a, this);
            break;
        }
        this.invalidate();
    }
    #closeRequested(gone) {
        if (gone) {
            this.closed = true; /* the engine has let the window go */
            windows.delete(this.id);
            clearTimeout(this.#renderTimer);
            this.#options.onClose?.(this);
            return;
        }
        if (this.#options.onClose?.(this) === false) return;
        this.close();
    }
}

internal.uiEvent = buffer => {
    const r = new Reader(buffer);
    if (r.u8() !== 0x65) return;
    windows.get(r.u32())?.event(r);
};

function run(options) {
    if (typeof options?.view !== 'function') throw new TypeError('run() needs a view function');
    return new App(options);
}

internal.ui = Object.freeze({
    run, theme, mix, withAlpha,
    column, row, scroll, card, text, heading, title, caption, button, toggle, checkbox, slider, segmented,
    textField, listItem, icon, divider, spacer, progress, canvas,
    /* The computer: processor, memory, uptime and running tasks (null if the system will not say). */
    systemInfo: () => host.systemInfo(),
    launch: (path, arg) => host.uiLaunch(String(path), arg === undefined ? undefined : String(arg)),
    /* The desktop's own settings (what Settings changes). */
    desktop: Object.freeze({
        get: () => host.desktop(),
        set: s => host.desktopSet(s.refreshRate | 0, s.pointerSpeed | 0, s.pointerY | 0, !!s.deepColour),
        setVolume: volume => host.setVolume(Math.round(Number(volume))),
    }),
});
