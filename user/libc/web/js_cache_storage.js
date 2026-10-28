/* Cache Storage: CacheStorage, Cache and self.caches for a Window or a Worker realm (Service
   Workers, "Caches", https://w3c.github.io/ServiceWorker/#cache-objects, consulted 2026-10-28).
   The storage and the algorithms that read it (Query Cache, Request Matches Cached Item, Batch
   Cache Operations) are the storage broker's (webcache.h, through js_cachestorage.c), so every
   window and worker of the origin works on one state. This binds them to the IDL: it converts the
   arguments, fetches for add and addAll, streams a body into the broker before the batch that
   stores it and out of it as it is read, and makes the Request and Response objects of the
   results. realm is what the realm's own bindings give (js_fetch.js in a Window, the Worker
   runtime's Fetch in a Worker): its Fetch objects' private records, its fetch, its streams and
   its broker. Included after the realm's Fetch, Streams and DOMException. */
function installCacheStorage(realm) {
    'use strict';
    /* [SecureContext]: neither the interfaces nor the attribute exist elsewhere. */
    if (!realm.secure) return;
    const apply = Reflect.apply, define = Object.defineProperty, freeze = Object.freeze;
    const wmGet = WeakMap.prototype.get, wmSet = WeakMap.prototype.set;
    const stringify = JSON.stringify, parse = JSON.parse, str = String, wellFormed = String.prototype.toWellFormed;
    const split = String.prototype.split, trim = String.prototype.trim, toLower = String.prototype.toLowerCase, startsWith = String.prototype.startsWith;
    const U8 = Uint8Array, u8Set = U8.prototype.set;
    const taLength = Object.getOwnPropertyDescriptor(Object.getPrototypeOf(U8.prototype), 'byteLength').get;
    const taBuffer = Object.getOwnPropertyDescriptor(Object.getPrototypeOf(U8.prototype), 'buffer').get;
    const Registry = FinalizationRegistry, register = Registry.prototype.register, unregister = Registry.prototype.unregister;
    const exception = domExceptionBridge.create, TypeErr = TypeError, iterator = Symbol.iterator;
    const {native, streams, embedderPolicy, isRequest, isResponse, newRequest, request: requestRecord, makeRequest,
        response: responseRecord, takeBody, makeResponse, fetch: realmFetch, controller: newController} = realm;
    const pulled = streams.pulled, chunks = streams.chunks;
    const IGNORE_SEARCH = 1, IGNORE_METHOD = 2, IGNORE_VARY = 4, FIRST = 64, PUT = 1, REMOVE = 2;
    const CHUNK = 262144;
    const token = {}, cacheSlots = new WeakMap(), storageSlots = new WeakMap();

    /* The broker's statuses (webcache.h). */
    function failure(status) {
        if (status === 6) return new TypeErr('Invalid cache operation');
        if (status === 2) return exception('SecurityError', 'This context has no storage for Cache Storage');
        if (status === 5) return exception('QuotaExceededError', 'The storage volume is full');
        if (status === 7) return exception('InvalidStateError', 'The batch stores the same request more than once');
        return exception('UnknownError', 'The cache storage operation failed');
    }
    /* One command: its value, or the failure it ended in. The host throws (a sandbox without
       allow-same-origin: SecurityError) where the method's promise rejects. */
    async function call(list) {
        const r = await apply(native, undefined, list);
        if (r[0] !== 0) throw failure(r[0]);
        return r[1];
    }
    function forget(list) { try { apply(native, undefined, list); } catch (_) {} }

    const usv = v => { if (typeof v === 'symbol') throw new TypeErr('Cannot convert a Symbol to a string'); return apply(wellFormed, str(v), []); };
    const domString = v => { if (typeof v === 'symbol') throw new TypeErr('Cannot convert a Symbol to a string'); return str(v); };
    /* RequestInfo: a Request, or a USVString. */
    const requestInfo = v => isRequest(v) ? v : usv(v);
    function dictionary(v, what) {
        if (v === undefined || v === null) return null;
        if (typeof v !== 'object' && typeof v !== 'function') throw new TypeErr(what + ' must be a dictionary');
        return v;
    }
    /* CacheQueryOptions, its members read in their order. */
    function queryFlags(d) {
        if (!d) return 0;
        let flags = 0;
        if (d.ignoreMethod) flags |= IGNORE_METHOD;
        if (d.ignoreSearch) flags |= IGNORE_SEARCH;
        if (d.ignoreVary) flags |= IGNORE_VARY;
        return flags;
    }
    const http = url => apply(startsWith, url, ['http:']) || apply(startsWith, url, ['https:']);
    /* A Vary field value that is "*". headers is [name, value, ...]. */
    function varyStar(headers) {
        for (let i = 0; i < headers.length; i += 2) {
            if (apply(toLower, headers[i], []) !== 'vary') continue;
            const values = apply(split, headers[i + 1], [',']);
            for (let k = 0; k < values.length; k++) if (apply(trim, values[k], []) === '*') return true;
        }
        return false;
    }
    /* The request a query names: a Request's own request, or a new Request made from the string
       (a TypeError for what is no URL). null when the method alone rules out every match. */
    function queryOf(info, flags) {
        if (isRequest(info)) {
            const r = requestRecord(info);
            return r.method !== 'GET' && !(flags & IGNORE_METHOD) ? null : r;
        }
        return requestRecord(newRequest(info));
    }
    const wire = q => [q.url, q.method, q.headers];

    /* A stored body, read from the broker as the stream is read, closed there at its end, on cancel,
       or when nothing reads it any more. */
    const bodies = new Registry(t => forget(['close', t]));
    function bodyStream(t) {
        const state = {open: true};
        const done = () => { if (!state.open) return false; state.open = false; apply(unregister, bodies, [state]); return true; };
        const stream = pulled(async () => {
            if (!state.open) return null;
            let bytes;
            try { bytes = await call(['read', t, CHUNK]); } catch (e) { if (done()) forget(['close', t]); throw e; }
            if (bytes === null) done(); /* the broker closed it at the end */
            return bytes;
        }, () => { if (done()) forget(['close', t]); });
        apply(register, bodies, [stream, t, state]);
        return stream;
    }
    /* Fetch's cross-origin resource policy check of an opaque response, with the realm's embedder
       policy: what the native fetch learned of the response it hid (c: 1 a valid
       Cross-Origin-Resource-Policy, 2 a URL of another origin, 4 a request with credentials). A
       policy that let the response through when it was fetched lets it through again: the storage
       key's origin is the one that fetched it. */
    function corpBlocked(x) {
        if (x.t !== 'opaque') return false;
        const bits = typeof x.c === 'number' ? x.c : 2, policy = embedderPolicy();
        if (bits & 1) return false;
        return (policy === 1 || (policy === 2 && !!(bits & 4))) && !!(bits & 2);
    }
    /* matchAll's steps on a cache (id) or the whole map (id null): the responses, new objects whose
       headers are immutable. */
    async function responses(id, info, flags, first, cacheName) {
        let query;
        if (info !== undefined) {
            query = queryOf(info, flags);
            if (query === null) return [];
        }
        const items = await call(['match', id, flags | (first ? FIRST : 0), cacheName, query === undefined ? undefined : wire(query)]);
        const extras = [];
        let blocked = false;
        for (let i = 0; i < items.length; i++) { extras[i] = parse(items[i][1]); if (corpBlocked(extras[i])) blocked = true; }
        if (blocked) {
            for (let i = 0; i < items.length; i++) if (items[i][2]) forget(['close', items[i][2]]);
            throw new TypeErr('An opaque cached response is blocked by Cross-Origin-Resource-Policy');
        }
        const out = [];
        for (let i = 0; i < items.length; i++) out[i] = makeResponse(extras[i], items[i][0], items[i][2] ? bodyStream(items[i][2]) : null);
        return out;
    }
    /* A body read whole into a staged body of the broker, from a reader the stream stays locked to
       (Fetch's "read all bytes"); the pieces go as they come, never the whole body at once. */
    async function stage(stream) {
        const reader = chunks(stream);
        const t = await call(['stage']);
        try {
            let pending = [], size = 0;
            const flush = async () => {
                const joined = new U8(size);
                let at = 0;
                for (let i = 0; i < pending.length; i++) { apply(u8Set, joined, [pending[i], at]); at += apply(taLength, pending[i], []); }
                pending = []; size = 0;
                await call(['write', t, apply(taBuffer, joined, [])]);
            };
            for (;;) {
                const chunk = await reader.next();
                if (chunk === null) break;
                const n = apply(taLength, chunk, []);
                if (!n) continue;
                pending[pending.length] = chunk; size += n;
                if (size >= CHUNK) await flush();
            }
            if (size) await flush();
            return t;
        } catch (e) {
            forget(['discard', t]);
            throw e;
        }
    }
    function abandon(results) {
        (async () => { for (let i = 0; i < results.length; i++) { const r = await results[i]; if (r && r.token) forget(['discard', r.token]); } })();
    }
    /* addAll's fetch of one request: its response, with its body staged once all of it came. */
    async function fetchOne(r, controller, failed) {
        try {
            const response = await realmFetch(r, controller.signal);
            const rec = responseRecord(response);
            if (rec.type === 'error' || rec.status < 200 || rec.status > 299 || rec.status === 206)
                throw new TypeErr('Cache.addAll: the response is not ok (' + rec.status + ')');
            if (varyStar(rec.headers)) { controller.abort(); throw new TypeErr('Cache.addAll: the response varies on *'); }
            const body = takeBody(response);
            const t = body === null ? 0 : await stage(body);
            return {token: t, headers: rec.headers, extra: rec.extra};
        } catch (e) {
            failed(e);
            return null;
        }
    }
    function cacheOf(object) {
        const s = apply(wmGet, cacheSlots, [object]);
        if (!s) throw new TypeErr('Illegal Cache invocation');
        return s;
    }
    function storageOf(object) {
        if (!apply(wmGet, storageSlots, [object])) throw new TypeErr('Illegal CacheStorage invocation');
    }
    function newCache(id) {
        const cache = new Cache(token);
        apply(wmSet, cacheSlots, [cache, {id}]);
        return cache;
    }

    class Cache {
        constructor(secret) { if (secret !== token) throw new TypeErr('Illegal constructor'); }
        async match(request, options) {
            const id = cacheOf(this).id;
            if (!arguments.length) throw new TypeErr('Cache.match requires a request');
            const info = requestInfo(request), flags = queryFlags(dictionary(options, 'CacheQueryOptions'));
            const list = await responses(id, info, flags, true, undefined);
            return list.length ? list[0] : undefined;
        }
        async matchAll(request, options) {
            const id = cacheOf(this).id;
            const info = request === undefined ? undefined : requestInfo(request), flags = queryFlags(dictionary(options, 'CacheQueryOptions'));
            return freeze(await responses(id, info, flags, false, undefined));
        }
        async add(request) {
            cacheOf(this);
            if (!arguments.length) throw new TypeErr('Cache.add requires a request');
            await addAll(this, [requestInfo(request)]);
        }
        async addAll(requests) {
            cacheOf(this);
            if (!arguments.length) throw new TypeErr('Cache.addAll requires requests');
            if (requests === null || (typeof requests !== 'object' && typeof requests !== 'function') || typeof requests[iterator] !== 'function')
                throw new TypeErr('Cache.addAll requires a sequence of requests');
            const list = [];
            for (const v of requests) list[list.length] = requestInfo(v);
            await addAll(this, list);
        }
        async put(request, response) {
            const id = cacheOf(this).id;
            if (arguments.length < 2) throw new TypeErr('Cache.put requires a request and a response');
            const info = requestInfo(request);
            if (!isResponse(response)) throw new TypeErr('Cache.put requires a Response');
            const inner = isRequest(info) ? requestRecord(info) : requestRecord(newRequest(info));
            if (!http(inner.url) || inner.method !== 'GET') throw new TypeErr('Cache.put requires an HTTP(S) GET request');
            const rec = responseRecord(response);
            if (rec.status === 206) throw new TypeErr('Cache.put does not store a partial response');
            if (varyStar(rec.headers)) throw new TypeErr('Cache.put does not store a response that varies on *');
            if (rec.unusable) throw new TypeErr('The response body is already used or locked');
            const body = takeBody(response);
            const t = body === null ? 0 : await stage(body);
            await call(['batch', id, [[PUT, 0, inner.url, inner.method, inner.headers, inner.extra, rec.headers, rec.extra, t]]]);
        }
        async delete(request, options) {
            const id = cacheOf(this).id;
            if (!arguments.length) throw new TypeErr('Cache.delete requires a request');
            const info = requestInfo(request), flags = queryFlags(dictionary(options, 'CacheQueryOptions'));
            const query = queryOf(info, flags);
            if (query === null) return false;
            return await call(['batch', id, [[REMOVE, flags, query.url, query.method, query.headers, query.extra]]]) > 0;
        }
        async keys(request, options) {
            const id = cacheOf(this).id;
            const info = request === undefined ? undefined : requestInfo(request), flags = queryFlags(dictionary(options, 'CacheQueryOptions'));
            let query;
            if (info !== undefined) {
                query = queryOf(info, flags);
                if (query === null) return freeze([]);
            }
            const items = await call(['keys', id, flags, query === undefined ? undefined : wire(query)]);
            const out = [];
            for (let i = 0; i < items.length; i++) out[i] = makeRequest(items[i][0], items[i][1], items[i][2], parse(items[i][3]));
            return freeze(out);
        }
    }
    async function addAll(cache, list) {
        const id = cacheOf(cache).id;
        for (let i = 0; i < list.length; i++) {
            if (!isRequest(list[i])) continue;
            const r = requestRecord(list[i]);
            if (!http(r.url) || r.method !== 'GET') throw new TypeErr('Cache.addAll requires HTTP(S) GET requests');
        }
        const controller = newController(), records = [], results = [];
        let first = null;
        const failed = e => { if (first === null) { first = {error: e}; controller.abort(); } };
        for (let i = 0; i < list.length; i++) {
            let r;
            try {
                r = newRequest(list[i]);
                records[i] = requestRecord(r);
                if (!http(records[i].url)) throw new TypeErr('Cache.addAll requires HTTP(S) requests');
            } catch (e) {
                controller.abort();
                abandon(results);
                throw e;
            }
            results[i] = fetchOne(r, controller, failed);
        }
        const responses = [];
        for (let i = 0; i < results.length; i++) responses[i] = await results[i];
        if (first !== null) {
            for (let i = 0; i < responses.length; i++) if (responses[i] && responses[i].token) forget(['discard', responses[i].token]);
            throw first.error;
        }
        const ops = [];
        for (let i = 0; i < records.length; i++) {
            const q = records[i], s = responses[i];
            ops[i] = [PUT, 0, q.url, q.method, q.headers, q.extra, s.headers, s.extra, s.token];
        }
        await call(['batch', id, ops]);
    }

    class CacheStorage {
        constructor(secret) { if (secret !== token) throw new TypeErr('Illegal constructor'); }
        async match(request, options) {
            storageOf(this);
            if (!arguments.length) throw new TypeErr('CacheStorage.match requires a request');
            const info = requestInfo(request), d = dictionary(options, 'MultiCacheQueryOptions');
            const flags = queryFlags(d), named = d ? d.cacheName : undefined, cacheName = named === undefined ? undefined : domString(named);
            /* The request is converted only by a cache's match: with no cache to ask (none by that
               name), even one that is no URL finds nothing. */
            try { queryOf(info, flags); } catch (e) {
                const names = await call(['names']);
                let any = cacheName === undefined ? names.length > 0 : false;
                for (let i = 0; !any && i < names.length; i++) any = names[i] === cacheName;
                if (!any) return undefined;
                throw e;
            }
            const list = await responses(null, info, flags, true, cacheName);
            return list.length ? list[0] : undefined;
        }
        async has(cacheName) {
            storageOf(this);
            if (!arguments.length) throw new TypeErr('CacheStorage.has requires a name');
            return await call(['has', domString(cacheName)]);
        }
        async open(cacheName) {
            storageOf(this);
            if (!arguments.length) throw new TypeErr('CacheStorage.open requires a name');
            return newCache(await call(['open', domString(cacheName)]));
        }
        async delete(cacheName) {
            storageOf(this);
            if (!arguments.length) throw new TypeErr('CacheStorage.delete requires a name');
            return await call(['delete', domString(cacheName)]);
        }
        async keys() {
            storageOf(this);
            return await call(['names']);
        }
    }
    for (const C of [Cache, CacheStorage]) {
        define(C.prototype, Symbol.toStringTag, {value: C.name, configurable: true});
        define(globalThis, C.name, {value: C, writable: true, configurable: true});
    }
    /* WindowOrWorkerGlobalScope's caches: [SameObject], the realm's one CacheStorage. */
    const storage = new CacheStorage(token), globalScope = globalThis;
    apply(wmSet, storageSlots, [storage, true]);
    define(globalThis, 'caches', {configurable: true, enumerable: true,
        get: Object.getOwnPropertyDescriptor({get caches() { if (this != null && this !== globalScope) throw new TypeErr('Illegal invocation'); return storage; }}, 'caches').get});
}
