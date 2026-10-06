/* Session history is owned by the native browser, not an unrelated JS stack. */
{
    const URLImpl = globalThis.URL;
    const apply = Reflect.apply, create = Object.create;
    const urlKeys = ['href','origin','username','password','protocol','host','hostname','port','pathname','search','hash'];
    const urlGet = create(null), urlSet = create(null);
    for (const key of urlKeys) {
        const descriptor = Object.getOwnPropertyDescriptor(URLImpl.prototype, key);
        urlGet[key] = descriptor.get; urlSet[key] = descriptor.set;
    }
    const fields = url => {
        const result = create(null);
        for (const key of urlKeys) result[key] = apply(urlGet[key], url, []);
        return result;
    };
    let cachedURL = null, cachedLocation = null;
    function currentLocation() {
        // The native document URL is authoritative, including changes that did
        // not pass through a JS setter. Never retain a mutable public URL object.
        const source = host.url();
        if (source !== cachedURL || cachedLocation === null) {
            const next = fields(new URLImpl(source));
            cachedLocation = next; cachedURL = source;
        }
        return cachedLocation;
    }
    let cachedEntry = -1, cachedState = null;
    const check = receiver => { if (receiver !== history) throw new TypeError('Illegal invocation'); };
    const string = value => { if(typeof value==='symbol') throw new TypeError('Cannot convert Symbol to string'); return String(value); };
    function update(replace, data, unused, url, argc) {
        if (argc < 2) throw new TypeError('History state and unused title are required');
        string(unused); url=url==null?null:string(url);
        const serialized = host.pack(cloneData.serialize(data));
        const current = currentLocation();
        let next;
        try { next = url == null || url === '' ? current : fields(new URLImpl(url, document.baseURI)); }
        catch (_) { throw new DOMException('Invalid history URL','SecurityError'); }
        const opaque = current.origin === 'null';
        if (current.origin !== next.origin || current.username !== next.username || current.password !== next.password ||
            (opaque && current.href.split('#')[0] !== next.href.split('#')[0]))
            throw new DOMException('History URL must have the same origin', 'SecurityError');
        host.history(replace ? 2 : 1, next.href, serialized, 0);
        cachedEntry = -1; cachedLocation = null;
    }
    class History {
        constructor() { throw new TypeError('Illegal constructor'); }
        get length() { check(this); return host.history(0).length; }
        get state() {
            check(this); const info = host.history(0,null,null,1);
            if (info.entry !== cachedEntry) {
                cachedState = info.data === null ? null : cloneData.deserialize(host.unpack(info.data)); cachedEntry = info.entry;
            }
            return cachedState;
        }
        get scrollRestoration() { check(this); return host.history(0).manual ? 'manual' : 'auto'; }
        set scrollRestoration(value) {
            check(this); value = String(value);
            if (value !== 'auto' && value !== 'manual') throw new TypeError('Invalid scroll restoration mode');
            host.history(4, null, null, value === 'manual' ? 1 : 0);
        }
        pushState(data, unused, url) { check(this); update(false, data, unused, url, arguments.length); }
        replaceState(data, unused, url) { check(this); update(true, data, unused, url, arguments.length); }
        go(delta = 0) { check(this); host.history(3, null, null, +delta | 0); }
        back() { check(this); host.history(3, null, null, -1); }
        forward() { check(this); host.history(3, null, null, 1); }
    }
    class PopStateEvent extends Event {
        constructor(type, init = {}) { super(type, init); this.state = init.state === undefined ? null : init.state; }
    }
    class HashChangeEvent extends Event {
        constructor(type, init = {}) { super(type, init); this.oldURL = String(init.oldURL || ''); this.newURL = String(init.newURL || ''); }
    }
    const history = Object.create(History.prototype);
    Object.assign(globalThis, {History, history, PopStateEvent, HashChangeEvent});
    historyEvent = (oldURL, popstate) => {
        if (popstate && document.readyState === 'loading') {
            document.addEventListener('DOMContentLoaded',()=>historyEvent(oldURL,popstate),{once:true}); return;
        }
        cachedEntry = -1; cachedLocation = null;
        if (popstate) dispatch(globalThis, new PopStateEvent('popstate', {state:history.state}));
        const fragment = url => { const i = url.indexOf('#'); return i < 0 ? null : url.slice(i + 1); };
        if (fragment(oldURL) !== fragment(host.url()))
            dispatch(globalThis, new HashChangeEvent('hashchange', {oldURL, newURL:host.url()}));
    };
    for (const key of ['protocol','host','hostname','port','pathname','search','hash']) {
        Object.defineProperty(location, key, {configurable:true,
            get() { return currentLocation()[key]; },
            set(value) {
                // IDL conversion can reenter history/navigation. Copy the
                // native document URL only after that conversion completes.
                value = string(value);
                const url = new URLImpl(host.url());
                apply(urlSet[key], url, [value]);
                host.navigate(apply(urlGet.href, url, []));
                cachedLocation = null;
            }
        });
    }
    Object.defineProperty(location, 'origin', {configurable:true,get(){ return currentLocation().origin; }});
}
