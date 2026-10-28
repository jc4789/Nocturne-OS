/* URL and URLSearchParams (https://url.spec.whatwg.org/#api). The parser, the
   setters and the urlencoded format are native (js_url.c over Lexbor's WHATWG
   parser). A URL is the record host.urlParse returns: its serialization, then
   where each component lies in it (web_url.h's enum web_url_mark).
   urlBridge holds what HTML adds for its documents: the parser with the
   document's character encoding for the query. */
const urlBridge = (() => {
    'use strict';
    const apply = Reflect.apply, define = Object.defineProperty, ownKeys = Reflect.ownKeys;
    const describe = Reflect.getOwnPropertyDescriptor, setPrototypeOf = Object.setPrototypeOf;
    const slice = String.prototype.slice, wellFormed = String.prototype.toWellFormed, sort = Array.prototype.sort;
    const parse = host.urlParse, update = host.urlSet;
    const formParse = host.urlFormParse, formSerialize = host.urlFormSerialize;
    const HREF = 0, PROTOCOL_END = 1, USERNAME_START = 2, USERNAME_END = 3, PASSWORD_START = 4,
        PASSWORD_END = 5, HOST_START = 6, HOST_END = 7, PORT_START = 8, PORT_END = 9, PATH_START = 10,
        QUERY_START = 11, FRAGMENT_START = 12;
    /* web_url.h's enum web_url_part */
    const PROTOCOL = 0, USERNAME = 1, PASSWORD = 2, HOST = 3, HOSTNAME = 4, PORT = 5, PATHNAME = 6,
        SEARCH = 7, HASH = 8;
    const KEYS = 0, VALUES = 1, ENTRIES = 2;
    /* Hands URL.parse's record to the constructor; no page script can hold it. */
    const RECORD = {};
    /* WebIDL's USVString: ToString, then each lone surrogate as U+FFFD. */
    const usv = value => apply(wellFormed, `${value}`, []);
    const span = (r, start, end) => apply(slice, r[HREF], [r[start], r[end]]);
    const protocolOf = r => apply(slice, r[HREF], [0, r[PROTOCOL_END]]);
    const queryOf = r => r[FRAGMENT_START] > r[QUERY_START] ? formParse(apply(slice, r[HREF], [r[QUERY_START] + 1, r[FRAGMENT_START]])) : [];
    function need(count, wanted, what) {
        if (count < wanted) throw new TypeError(`${what}: ${wanted} argument${wanted > 1 ? 's' : ''} required, but only ${count} present.`);
    }
    function origin(r) {
        const protocol = protocolOf(r);
        switch (protocol) {
        case 'http:': case 'https:': case 'ws:': case 'wss:': case 'ftp:':
            return protocol + '//' + span(r, HOST_START, PORT_END);
        case 'blob:': {
            const path = parse(span(r, PATH_START, QUERY_START), undefined), inner = path && protocolOf(path);
            return inner === 'http:' || inner === 'https:' ? origin(path) : 'null';
        }
        }
        return 'null';
    }

    /* URLSearchParams' own, for the URL it belongs to and its iterators. */
    let attachParams, replaceParams, listOf, setQuery;
    class URL {
        #r;
        #query = null;
        constructor(url, base = undefined) {
            if (url === RECORD) { this.#r = base; return; }
            need(arguments.length, 1, "Failed to construct 'URL'");
            url = usv(url);
            if (base !== undefined) base = usv(base);
            const r = parse(url, base);
            if (r === null) throw new TypeError("Failed to construct 'URL': Invalid URL");
            this.#r = r;
        }
        static parse(url, base = undefined) {
            need(arguments.length, 1, "Failed to execute 'parse' on 'URL'");
            url = usv(url);
            if (base !== undefined) base = usv(base);
            const r = parse(url, base);
            return r === null ? null : new URL(RECORD, r);
        }
        static canParse(url, base = undefined) {
            need(arguments.length, 1, "Failed to execute 'canParse' on 'URL'");
            url = usv(url);
            if (base !== undefined) base = usv(base);
            return parse(url, base) !== null;
        }
        get href() { return this.#r[HREF]; }
        set href(value) {
            void this.#r;
            const r = parse(usv(value), undefined);
            if (r === null) throw new TypeError("Failed to set the 'href' property on 'URL': Invalid URL");
            this.#r = r;
            if (this.#query !== null) replaceParams(this.#query, queryOf(r));
        }
        get origin() { return origin(this.#r); }
        get protocol() { return protocolOf(this.#r); }
        set protocol(value) { this.#set(PROTOCOL, value); }
        get username() { return span(this.#r, USERNAME_START, USERNAME_END); }
        set username(value) { this.#set(USERNAME, value); }
        get password() { return span(this.#r, PASSWORD_START, PASSWORD_END); }
        set password(value) { this.#set(PASSWORD, value); }
        get host() { return span(this.#r, HOST_START, PORT_END); }
        set host(value) { this.#set(HOST, value); }
        get hostname() { return span(this.#r, HOST_START, HOST_END); }
        set hostname(value) { this.#set(HOSTNAME, value); }
        get port() { return span(this.#r, PORT_START, PORT_END); }
        set port(value) { this.#set(PORT, value); }
        get pathname() { return span(this.#r, PATH_START, QUERY_START); }
        set pathname(value) { this.#set(PATHNAME, value); }
        get search() {
            const r = this.#r;
            return r[FRAGMENT_START] - r[QUERY_START] > 1 ? span(r, QUERY_START, FRAGMENT_START) : '';
        }
        set search(value) {
            this.#set(SEARCH, value);
            if (this.#query !== null) replaceParams(this.#query, queryOf(this.#r));
        }
        get searchParams() { return this.#query ??= attachParams(this, queryOf(this.#r)); }
        get hash() {
            const r = this.#r, href = r[HREF];
            return href.length - r[FRAGMENT_START] > 1 ? apply(slice, href, [r[FRAGMENT_START]]) : '';
        }
        set hash(value) { this.#set(HASH, value); }
        toJSON() { return this.#r[HREF]; }
        toString() { return this.#r[HREF]; }
        #set(part, value) {
            value = usv(value);
            const r = update(this.#r[HREF], part, value);
            if (r !== null) this.#r = r;
        }
        static {
            setQuery = (url, query) => url.#set(SEARCH, query);
        }
    }

    /* A WebIDL sequence from an iterable's iterator method, one item at a time. */
    function iterate(value, method, each) {
        const iterator = apply(method, value, []);
        if (iterator === null || typeof iterator !== 'object' && typeof iterator !== 'function')
            throw new TypeError('Result of the Symbol.iterator method is not an object');
        const next = iterator.next;
        for (;;) {
            const step = apply(next, iterator, []);
            if (step === null || typeof step !== 'object' && typeof step !== 'function')
                throw new TypeError(`Iterator result ${String(step)} is not an object`);
            if (step.done) return;
            each(step.value);
        }
    }
    const isObject = value => value !== null && (typeof value === 'object' || typeof value === 'function');
    function fromPairs(init, method) {
        const list = [];
        iterate(init, method, item => {
            const inner = isObject(item) ? item[Symbol.iterator] : undefined, pair = [];
            if (typeof inner !== 'function')
                throw new TypeError("Failed to construct 'URLSearchParams': The provided value cannot be converted to a sequence.");
            iterate(item, inner, value => { pair[pair.length] = usv(value); });
            if (pair.length !== 2)
                throw new TypeError("Failed to construct 'URLSearchParams': Sequence initializer must only contain pair elements");
            list[list.length] = pair[0];
            list[list.length] = pair[1];
        });
        return list;
    }
    function fromRecord(init) {
        const list = [], at = Object.create(null), keys = ownKeys(init);
        for (let i = 0; i < keys.length; i++) {
            const key = keys[i], descriptor = describe(init, key);
            if (descriptor === undefined || !descriptor.enumerable) continue;
            const name = usv(key), value = usv(init[key]);
            if (at[name] === undefined) {
                at[name] = list.length;
                list[list.length] = name;
                list[list.length] = value;
            } else list[at[name] + 1] = value;
        }
        return list;
    }

    class URLSearchParams {
        #list = [];
        #url = null;
        constructor(init = '') {
            if (isObject(init)) {
                const method = init[Symbol.iterator];
                if (method === undefined || method === null) this.#list = fromRecord(init);
                else if (typeof method !== 'function')
                    throw new TypeError("Failed to construct 'URLSearchParams': The object's Symbol.iterator is not a function.");
                else this.#list = fromPairs(init, method);
            } else {
                init = usv(init);
                this.#list = formParse(init[0] === '?' ? apply(slice, init, [1]) : init);
            }
        }
        get size() { return this.#list.length / 2; }
        append(name, value) {
            void this.#list;
            need(arguments.length, 2, "Failed to execute 'append' on 'URLSearchParams'");
            name = usv(name);
            value = usv(value);
            const list = this.#list;
            list[list.length] = name;
            list[list.length] = value;
            this.#update();
        }
        delete(name, value = undefined) {
            void this.#list;
            need(arguments.length, 1, "Failed to execute 'delete' on 'URLSearchParams'");
            name = usv(name);
            if (value !== undefined) value = usv(value);
            const list = this.#list, kept = [];
            for (let i = 0; i < list.length; i += 2) {
                if (list[i] === name && (value === undefined || list[i + 1] === value)) continue;
                kept[kept.length] = list[i];
                kept[kept.length] = list[i + 1];
            }
            this.#list = kept;
            this.#update();
        }
        get(name) {
            void this.#list;
            need(arguments.length, 1, "Failed to execute 'get' on 'URLSearchParams'");
            name = usv(name);
            const list = this.#list;
            for (let i = 0; i < list.length; i += 2) if (list[i] === name) return list[i + 1];
            return null;
        }
        getAll(name) {
            void this.#list;
            need(arguments.length, 1, "Failed to execute 'getAll' on 'URLSearchParams'");
            name = usv(name);
            const list = this.#list, values = [];
            for (let i = 0; i < list.length; i += 2) if (list[i] === name) values[values.length] = list[i + 1];
            return values;
        }
        has(name, value = undefined) {
            void this.#list;
            need(arguments.length, 1, "Failed to execute 'has' on 'URLSearchParams'");
            name = usv(name);
            if (value !== undefined) value = usv(value);
            const list = this.#list;
            for (let i = 0; i < list.length; i += 2)
                if (list[i] === name && (value === undefined || list[i + 1] === value)) return true;
            return false;
        }
        set(name, value) {
            void this.#list;
            need(arguments.length, 2, "Failed to execute 'set' on 'URLSearchParams'");
            name = usv(name);
            value = usv(value);
            const list = this.#list, kept = [];
            let found = false;
            for (let i = 0; i < list.length; i += 2) {
                if (list[i] === name) {
                    if (found) continue;
                    found = true;
                    kept[kept.length] = name;
                    kept[kept.length] = value;
                } else {
                    kept[kept.length] = list[i];
                    kept[kept.length] = list[i + 1];
                }
            }
            if (!found) {
                kept[kept.length] = name;
                kept[kept.length] = value;
            }
            this.#list = kept;
            this.#update();
        }
        sort() {
            /* By the names' code units; a pair's index keeps equal names in order. */
            const list = this.#list, count = list.length / 2, order = [], sorted = [];
            for (let i = 0; i < count; i++) order[i] = i;
            apply(sort, order, [(a, b) => {
                const x = list[2 * a], y = list[2 * b];
                return x < y ? -1 : x > y ? 1 : a - b;
            }]);
            for (let i = 0; i < count; i++) {
                sorted[2 * i] = list[2 * order[i]];
                sorted[2 * i + 1] = list[2 * order[i] + 1];
            }
            this.#list = sorted;
            this.#update();
        }
        toString() { return formSerialize(this.#list); }
        forEach(callback, thisArg = undefined) {
            void this.#list;
            need(arguments.length, 1, "Failed to execute 'forEach' on 'URLSearchParams'");
            if (typeof callback !== 'function')
                throw new TypeError("Failed to execute 'forEach' on 'URLSearchParams': parameter 1 is not of type 'Function'.");
            for (let i = 0; 2 * i < this.#list.length; i++) {
                const list = this.#list;
                apply(callback, thisArg, [list[2 * i + 1], list[2 * i], this]);
            }
        }
        entries() { void this.#list; return new ParamsIterator(this, ENTRIES); }
        keys() { void this.#list; return new ParamsIterator(this, KEYS); }
        values() { void this.#list; return new ParamsIterator(this, VALUES); }
        /* The update steps: the URL's query follows the list. */
        #update() { if (this.#url !== null) setQuery(this.#url, formSerialize(this.#list)); }
        static {
            attachParams = (url, list) => {
                const params = new URLSearchParams();
                params.#list = list;
                params.#url = url;
                return params;
            };
            replaceParams = (params, list) => { params.#list = list; };
            listOf = params => params.#list;
        }
    }

    /* The default iterator: live over the list, as WebIDL's pair iterators are. */
    class ParamsIterator {
        #target;
        #kind;
        #index = 0;
        constructor(target, kind) {
            this.#target = target;
            this.#kind = kind;
        }
        next() {
            const list = listOf(this.#target), at = this.#index;
            if (2 * at >= list.length) return {value: undefined, done: true};
            this.#index = at + 1;
            const name = list[2 * at], value = list[2 * at + 1], kind = this.#kind;
            return {value: kind === KEYS ? name : kind === VALUES ? value : [name, value], done: false};
        }
    }

    /* WebIDL's shape: enumerable members and a toStringTag. */
    function expose(prototype, tag) {
        const keys = ownKeys(prototype);
        for (let i = 0; i < keys.length; i++) if (keys[i] !== 'constructor') define(prototype, keys[i], {enumerable: true});
        define(prototype, Symbol.toStringTag, {value: tag, configurable: true});
    }
    expose(URL.prototype, 'URL');
    define(URL, 'parse', {enumerable: true});
    define(URL, 'canParse', {enumerable: true});
    expose(URLSearchParams.prototype, 'URLSearchParams');
    define(URLSearchParams.prototype, Symbol.iterator, {value: URLSearchParams.prototype.entries, writable: true, configurable: true});
    const iteratorPrototype = ParamsIterator.prototype;
    delete iteratorPrototype.constructor;
    setPrototypeOf(iteratorPrototype, Object.getPrototypeOf(Object.getPrototypeOf([][Symbol.iterator]())));
    expose(iteratorPrototype, 'URLSearchParams Iterator');
    define(globalThis, 'URL', {value: URL, writable: true, configurable: true});
    define(globalThis, 'URLSearchParams', {value: URLSearchParams, writable: true, configurable: true});
    return Object.freeze({
        /* HTML's encoding-parse a URL: url against base (or none) with the
           encoding named for the query; a URL, or null for failure. */
        encodingParse(url, base, encoding) {
            const r = parse(usv(url), base === undefined ? undefined : usv(base), encoding);
            return r === null ? null : new URL(RECORD, r);
        },
        /* Location's search setter on href: the query from value in the
           encoding named; the serialization. */
        search(href, value, encoding) {
            const r = update(href, SEARCH, usv(value), encoding);
            return r === null ? href : r[HREF];
        }
    });
})();
