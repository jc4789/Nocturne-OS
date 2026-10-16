/* Origin private file system and StorageManager (File System Standard,
 * Storage Standard). A handle is a path of names below the realm's origin
 * directory; js_opfs.c owns the storage, name encoding and file locks.
 * https://fs.spec.whatwg.org/ https://storage.spec.whatwg.org/ (2026-10-15) */
function installOriginPrivateFileSystem(host, secure, NavigatorInterface, dedicated) {
    'use strict';
    if (!secure) return;
    const apply = Reflect.apply, define = Object.defineProperty, create = Object.create, freeze = Object.freeze;
    const get = WeakMap.prototype.get, put = WeakMap.prototype.set, P = Promise, U8 = Uint8Array, AB = ArrayBuffer;
    const FileType = File, Domx = DOMException, Writable = WritableStream, Encoder = TextEncoder;
    const encode = Encoder.prototype.encode, wellFormed = String.prototype.toWellFormed, str = String;
    const push = Array.prototype.push, slice = Array.prototype.slice, isArray = Array.isArray;
    const getter = (p, k) => Object.getOwnPropertyDescriptor(p, k).get;
    const ta = Object.getPrototypeOf(U8.prototype), taBuffer = getter(ta, 'buffer'), taOffset = getter(ta, 'byteOffset');
    const taLength = getter(ta, 'byteLength'), taTag = getter(ta, Symbol.toStringTag), isView = AB.isView;
    const dvBuffer = getter(DataView.prototype, 'buffer'), dvOffset = getter(DataView.prototype, 'byteOffset');
    const dvLength = getter(DataView.prototype, 'byteLength'), abLength = getter(AB.prototype, 'byteLength');
    const sabLength = typeof SharedArrayBuffer === 'function' ? getter(SharedArrayBuffer.prototype, 'byteLength') : null;
    const u8Set = U8.prototype.set, u8Sub = U8.prototype.subarray, u8Fill = U8.prototype.fill;
    const writerOf = Writable.prototype.getWriter;
    const asyncIteratorPrototype = Object.getPrototypeOf(Object.getPrototypeOf(Object.getPrototypeOf((async function* () {})())));
    const native = {kind: host.opfsKind, ensure: host.opfsEnsure, list: host.opfsList, remove: host.opfsRemove,
        read: host.opfsRead, replace: host.opfsReplace, locked: host.opfsLocked, move: host.opfsMove,
        open: host.opfsOpen, access: host.opfsAccess, usage: host.opfsUsage};
    const token = {}, handles = new WeakMap(), syncs = new WeakMap(), writables = new WeakMap(), iterators = new WeakMap();
    const names = {'-1': ['NotFoundError', 'The entry was not found'], '-2': ['TypeMismatchError', 'The entry has a different kind'],
        '-3': ['InvalidModificationError', 'The directory is not empty'], '-4': ['NoModificationAllowedError', 'The file is locked'],
        '-5': ['SecurityError', 'This origin has no private file system'], '-6': ['InvalidStateError', 'The entry is in an invalid state'],
        '-7': ['QuotaExceededError', 'The storage volume is full'], '-9': ['UnknownError', 'The storage operation failed']};
    function check(r) {
        if (typeof r !== 'number' || r >= 0) return r;
        if (r === -8) throw new TypeError('Invalid file system entry name');
        const e = names[r] || names[-9];
        throw new Domx(e[1], e[0]);
    }
    const run = fn => { try { return P.resolve(fn()); } catch (e) { return P.reject(e); } };
    const call = (fn, ...args) => check(apply(fn, host, args));
    const usv = v => { if (typeof v === 'symbol') throw new TypeError('Cannot convert Symbol to string'); return apply(wellFormed, str(v), []); };
    const dictionary = v => { if (v == null) return {}; if (typeof v !== 'object' && typeof v !== 'function') throw new TypeError('Expected dictionary'); return v; };
    function unsigned(v, what) {
        const n = Number(v);
        if (!Number.isFinite(n)) throw new TypeError(what + ' is not a finite number');
        const t = Math.trunc(n);
        if (t < 0 || t > Number.MAX_SAFE_INTEGER) throw new TypeError(what + ' is out of range');
        return t;
    }
    function slot(v, kind) {
        const s = apply(get, handles, [v]);
        if (!s || (kind && s.kind !== kind)) throw new TypeError('Illegal FileSystemHandle receiver');
        return s;
    }
    const childParts = (s, name) => { const parts = apply(slice, s.parts, []); apply(push, parts, [name]); return parts; };
    function makeHandle(kind, parts) {
        const handle = new (kind === 'file' ? FileSystemFileHandle : FileSystemDirectoryHandle)(token);
        apply(put, handles, [handle, {kind, parts, name: parts.length ? parts[parts.length - 1] : ''}]);
        return handle;
    }
    /* [buffer, offset, length] of a BufferSource (shared allowed). */
    function bufferRange(value, allowShared) {
        if (isView(value)) {
            const typed = apply(taTag, value, []) !== undefined;
            const buffer = apply(typed ? taBuffer : dvBuffer, value, []);
            if (!allowShared && sabLength && !(buffer instanceof AB)) throw new TypeError('SharedArrayBuffer views are not allowed');
            return [buffer, apply(typed ? taOffset : dvOffset, value, []), apply(typed ? taLength : dvLength, value, [])];
        }
        try { return [value, 0, apply(abLength, value, [])]; } catch (_) {}
        if (allowShared && sabLength) try { return [value, 0, apply(sabLength, value, [])]; } catch (_) {}
        throw new TypeError('Expected a BufferSource');
    }

    class FileSystemHandle {
        constructor() { if (arguments[0] !== token) throw new TypeError('Illegal constructor'); }
        get kind() { return slot(this).kind; }
        get name() { return slot(this).name; }
        isSameEntry(other) {
            return run(() => {
                const a = slot(this), b = apply(get, handles, [other]);
                if (!b || a.kind !== b.kind || a.parts.length !== b.parts.length) return false;
                for (let i = 0; i < a.parts.length; i++) if (a.parts[i] !== b.parts[i]) return false;
                return true;
            });
        }
    }
    class FileSystemFileHandle extends FileSystemHandle {
        constructor() { super(arguments[0]); }
        getFile() {
            return run(() => {
                const s = slot(this, 'file'), data = call(native.read, s.parts);
                return new FileType([data[0]], s.name, {lastModified: data[1]});
            });
        }
        createWritable(options) {
            return run(() => {
                const s = slot(this, 'file'), o = dictionary(options), keep = !!o.keepExistingData;
                if (call(native.kind, s.parts) !== 1) check(-1);
                call(native.locked, s.parts);
                let bytes = new U8(0);
                if (keep) bytes = new U8(call(native.read, s.parts)[0]);
                return makeWritable(this, s, bytes);
            });
        }
        move(destination, name) {
            return run(() => {
                const s = slot(this, 'file');
                let parent, newName;
                if (typeof destination === 'string' || (destination !== null && typeof destination !== 'object')) {
                    parent = apply(slice, s.parts, [0, -1]);
                    newName = usv(destination);
                } else {
                    const d = slot(destination, 'directory');
                    parent = d.parts;
                    newName = name === undefined ? s.name : usv(name);
                }
                const parts = apply(slice, parent, []);
                apply(push, parts, [newName]);
                call(native.move, s.parts, parts);
                s.parts = parts;
                s.name = newName;
            });
        }
    }
    if (dedicated) {
        define(FileSystemFileHandle.prototype, 'createSyncAccessHandle', {
            value: function createSyncAccessHandle() {
                return run(() => {
                    const s = slot(this, 'file'), id = call(native.open, s.parts);
                    const handle = new FileSystemSyncAccessHandle(token);
                    apply(put, syncs, [handle, {id, cursor: 0, closed: false}]);
                    return handle;
                });
            }, writable: true, enumerable: true, configurable: true});
    }

    class FileSystemDirectoryHandle extends FileSystemHandle {
        constructor() { super(arguments[0]); }
        getFileHandle(name, options) { return lookup(this, name, options, 'file'); }
        getDirectoryHandle(name, options) { return lookup(this, name, options, 'directory'); }
        removeEntry(name, options) {
            return run(() => {
                const s = slot(this, 'directory'), entry = usv(name), o = dictionary(options);
                call(native.remove, childParts(s, entry), !!o.recursive);
            });
        }
        resolve(possibleDescendant) {
            return run(() => {
                const s = slot(this, 'directory'), d = apply(get, handles, [possibleDescendant]);
                if (!d || d.parts.length < s.parts.length) return null;
                for (let i = 0; i < s.parts.length; i++) if (s.parts[i] !== d.parts[i]) return null;
                return apply(slice, d.parts, [s.parts.length]);
            });
        }
        entries() { return iterate(this, 'entries'); }
        keys() { return iterate(this, 'keys'); }
        values() { return iterate(this, 'values'); }
    }
    define(FileSystemDirectoryHandle.prototype, Symbol.asyncIterator, {value: FileSystemDirectoryHandle.prototype.entries, writable: true, configurable: true});
    function lookup(directory, name, options, kind) {
        return run(() => {
            const s = slot(directory, 'directory'), entry = usv(name), o = dictionary(options);
            const parts = childParts(s, entry);
            call(native.ensure, parts, kind === 'file' ? 1 : 2, !!o.create);
            return makeHandle(kind, parts);
        });
    }
    const DirectoryIterator = create(asyncIteratorPrototype);
    define(DirectoryIterator, 'next', {value: function next() {
        return run(() => {
            const s = apply(get, iterators, [this]);
            if (!s) throw new TypeError('Illegal FileSystemDirectoryHandle iterator');
            if (!s.list) s.list = call(native.list, s.parts);
            if (s.index >= s.list.length) return {value: undefined, done: true};
            const [name, kind] = s.list[s.index++], handle = makeHandle(kind === 1 ? 'file' : 'directory', childParts(s, name));
            return {value: s.mode === 'keys' ? name : s.mode === 'values' ? handle : [name, handle], done: false};
        });
    }, writable: true, configurable: true});
    define(DirectoryIterator, Symbol.toStringTag, {value: 'FileSystemDirectoryHandle AsyncIterator', configurable: true});
    function iterate(directory, mode) {
        const s = slot(directory, 'directory'), iterator = create(DirectoryIterator);
        apply(put, iterators, [iterator, {parts: s.parts, mode, list: null, index: 0}]);
        return iterator;
    }

    function syncSlot(v) {
        const s = apply(get, syncs, [v]);
        if (!s) throw new TypeError('Illegal FileSystemSyncAccessHandle receiver');
        if (s.closed) throw new Domx('The access handle is closed', 'InvalidStateError');
        return s;
    }
    function transfer(handle, buffer, options, write) {
        const s = syncSlot(handle), range = bufferRange(buffer, true), o = dictionary(options);
        const at = o.at === undefined ? s.cursor : unsigned(o.at, 'at');
        const n = call(native.access, write ? 1 : 0, s.id, range[0], range[1], range[2], at);
        s.cursor = at + n;
        return n;
    }
    class FileSystemSyncAccessHandle {
        constructor() { if (arguments[0] !== token) throw new TypeError('Illegal constructor'); }
        read(buffer, options) { return transfer(this, buffer, options, false); }
        write(buffer, options) { return transfer(this, buffer, options, true); }
        truncate(newSize) {
            const s = syncSlot(this), size = unsigned(newSize, 'newSize');
            call(native.access, 2, s.id, undefined, 0, 0, size);
            if (s.cursor > size) s.cursor = size;
        }
        getSize() { return call(native.access, 3, syncSlot(this).id, undefined, 0, 0, 0); }
        flush() { call(native.access, 4, syncSlot(this).id, undefined, 0, 0, 0); }
        close() {
            const s = apply(get, syncs, [this]);
            if (!s) throw new TypeError('Illegal FileSystemSyncAccessHandle receiver');
            if (s.closed) return;
            s.closed = true;
            call(native.access, 5, s.id, undefined, 0, 0, 0);
        }
    }

    /* A writable stream edits a swap copy; close() replaces the file. */
    function reserve(w, end) {
        if (end <= w.bytes.length) return;
        let capacity = w.bytes.length || 64;
        while (capacity < end) capacity *= 2;
        const grown = new U8(capacity);
        apply(u8Set, grown, [apply(u8Sub, w.bytes, [0, w.size])]);
        w.bytes = grown;
    }
    function chunkBytes(data) {
        if (typeof data === 'string') return apply(encode, new Encoder(), [data]);
        if (blobBridge.brand(data)) return new U8(blobBridge.bytes(data));
        const range = bufferRange(data, false);
        return new U8(range[0], range[1], range[2]);
    }
    function writeChunk(w, chunk) {
        let params = null;
        if (chunk !== null && typeof chunk === 'object' && !isView(chunk) && !blobBridge.brand(chunk)) {
            try { bufferRange(chunk, false); } catch (_) { params = chunk; }
        }
        if (params) {
            const type = str(params.type);
            if (type === 'seek') {
                if (params.position == null) throw new Domx('seek requires a position', 'SyntaxError');
                w.cursor = unsigned(params.position, 'position');
                return;
            }
            if (type === 'truncate') {
                if (params.size == null) throw new Domx('truncate requires a size', 'SyntaxError');
                const size = unsigned(params.size, 'size');
                reserve(w, size);
                if (size > w.size) apply(u8Fill, w.bytes, [0, w.size, size]);
                w.size = size;
                if (w.cursor > size) w.cursor = size;
                return;
            }
            if (type !== 'write') throw new TypeError('Unknown write type');
            if (params.data == null) throw new Domx('write requires data', 'SyntaxError');
            if (params.position != null) w.cursor = unsigned(params.position, 'position');
            chunk = params.data;
        }
        const bytes = chunkBytes(chunk), end = w.cursor + bytes.length;
        reserve(w, end);
        if (w.cursor > w.size) apply(u8Fill, w.bytes, [0, w.size, w.cursor]);
        apply(u8Set, w.bytes, [bytes, w.cursor]);
        w.cursor = end;
        if (end > w.size) w.size = end;
    }
    function makeWritable(fileHandle, s, bytes) {
        const w = {bytes, size: bytes.length, cursor: 0, parts: s.parts};
        const stream = new FileSystemWritableFileStream(token, {
            write(chunk) { writeChunk(w, chunk); },
            close() { call(native.replace, w.parts, apply(taBuffer, w.bytes, []), 0, w.size); w.bytes = new U8(0); },
            abort() { w.bytes = new U8(0); },
        });
        apply(put, writables, [stream, w]);
        return stream;
    }
    function viaWriter(stream, chunk) {
        if (!apply(get, writables, [stream])) return P.reject(new TypeError('Illegal FileSystemWritableFileStream receiver'));
        let writer;
        try { writer = apply(writerOf, stream, []); } catch (e) { return P.reject(e); }
        const result = writer.write(chunk);
        writer.releaseLock();
        return result;
    }
    class FileSystemWritableFileStream extends Writable {
        constructor(key, sink) {
            if (key !== token) throw new TypeError('Illegal constructor');
            super(sink);
        }
        write(data) { return viaWriter(this, data); }
        seek(position) { return viaWriter(this, {type: 'seek', position}); }
        truncate(size) { return viaWriter(this, {type: 'truncate', size}); }
    }

    class StorageManager {
        constructor() { if (arguments[0] !== token) throw new TypeError('Illegal constructor'); }
        getDirectory() {
            return run(() => {
                if (call(native.kind, []) !== 2) check(-6);
                return makeHandle('directory', []);
            });
        }
        /* /data storage is never evicted by the browser. */
        persisted() { return P.resolve(true); }
        persist() { return P.resolve(true); }
    }
    const manager = new StorageManager(token);

    const classes = [FileSystemHandle, FileSystemFileHandle, FileSystemDirectoryHandle, FileSystemWritableFileStream, StorageManager];
    if (dedicated) apply(push, classes, [FileSystemSyncAccessHandle]);
    for (const C of classes) {
        define(C.prototype, Symbol.toStringTag, {value: C.name, configurable: true});
        for (const key of Object.getOwnPropertyNames(C.prototype)) if (key !== 'constructor') {
            const d = Object.getOwnPropertyDescriptor(C.prototype, key);
            d.enumerable = true;
            define(C.prototype, key, d);
        }
        define(globalThis, C.name, {value: C, writable: true, configurable: true});
    }
    define(NavigatorInterface.prototype, 'storage', {get: function storage() { return manager; }, enumerable: true, configurable: true});
    freeze(names);
}
