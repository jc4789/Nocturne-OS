/* CSS Font Loading Module Level 3 (https://drafts.csswg.org/css-font-loading-3/):
   FontFace, FontFaceSet as document.fonts, and FontFaceSetLoadEvent.
   css.c owns the descriptor grammar, the document's @font-face rules, the
   FontFace records that take part in font matching, and "find the matching
   font faces"; request_fonts loads their sources (js_fonts.h binds them as
   host.fonts). This realm owns the objects: each FontFace's status and
   [[FontStatusPromise]], each set's entries, [[LoadingFonts]], [[LoadedFonts]],
   [[FailedFonts]], [[ReadyPromise]] and events. Native progress arrives as one
   "font loading" task (changed), queued when a load advances or document.fonts
   stops being pending on the environment while stuck on it. */
const fontLoadingBridge = (() => {
    const native = host.fonts, post = host.postTask;
    const apply = Reflect.apply, defineProperty = Object.defineProperty, freeze = Object.freeze,
        create = Object.create, getPrototypeOf = Object.getPrototypeOf, getDescriptor = Object.getOwnPropertyDescriptor;
    const NativeString = String, NativePromise = Promise, promiseThen = Promise.prototype.then, promiseReject = Promise.reject;
    const NativeSet = Set, setAdd = Set.prototype.add, setDelete = Set.prototype.delete, setHas = Set.prototype.has,
        setClear = Set.prototype.clear, setForEach = Set.prototype.forEach, setValues = Set.prototype.values,
        setEntries = Set.prototype.entries, setSize = getDescriptor(Set.prototype, 'size').get;
    const NativeMap = Map, mapGet = Map.prototype.get, mapSet = Map.prototype.set, mapForEach = Map.prototype.forEach;
    const NativeWeakMap = WeakMap, weakGet = WeakMap.prototype.get, weakSet = WeakMap.prototype.set, weakHas = WeakMap.prototype.has;
    const NativeWeakRef = WeakRef, deref = WeakRef.prototype.deref;
    const isView = ArrayBuffer.isView, abLength = getDescriptor(ArrayBuffer.prototype, 'byteLength').get, Bytes = Uint8Array;
    const typedPrototype = getPrototypeOf(Uint8Array.prototype), typedSet = typedPrototype.set;
    const typedBuffer = getDescriptor(typedPrototype, 'buffer').get, typedOffset = getDescriptor(typedPrototype, 'byteOffset').get,
        typedLength = getDescriptor(typedPrototype, 'byteLength').get, typedTag = getDescriptor(typedPrototype, Symbol.toStringTag).get;
    const dvBuffer = getDescriptor(DataView.prototype, 'buffer').get, dvOffset = getDescriptor(DataView.prototype, 'byteOffset').get,
        dvLength = getDescriptor(DataView.prototype, 'byteLength').get;
    const Exception = DOMException, realmDocument = document;
    /* enum css_font_status and enum css_font_descriptor (webi.h). */
    const UNLOADED = 0, LOADING = 1, LOADED = 2, ERROR = 3;
    const FAMILY = 0, STYLE = 1, WEIGHT = 2, WIDTH = 3, UNICODE_RANGE = 4, FEATURE_SETTINGS = 5, VARIATION_SETTINGS = 6,
        DISPLAY = 7, ASCENT_OVERRIDE = 8, DESCENT_OVERRIDE = 9, LINE_GAP_OVERRIDE = 10, SIZE_ADJUST = 11, DESCRIPTORS = 12;
    const loadEvents = new NativeSet(['loading', 'loadingdone', 'loadingerror']);
    const faces = new NativeWeakMap(), sets = new NativeWeakMap(), loadEventSlots = new NativeWeakMap(), documentSets = new NativeWeakMap();
    const constructing = freeze({});
    /* The realm's document.fonts, other documents' sets (DOMParser and the
       like: never pending on the environment), and FontFaces whose own load()
       the native side carries out while they are in no set. */
    let documentSet = null, otherSets = [];
    const loadingFaces = new NativeSet();

    function string(value) {
        if (typeof value === 'symbol') throw new TypeError('Cannot convert a Symbol to a string');
        return NativeString(value);
    }
    function append(list, value) { defineProperty(list, list.length, {value, writable: true, enumerable: true, configurable: true}); }
    function removeFrom(list, value) {
        for (let i = 0; i < list.length; i++) if (list[i] === value) {
            for (let j = i + 1; j < list.length; j++) list[j - 1] = list[j];
            list.length--;
            return true;
        }
        return false;
    }
    function queue(task) {
        try { post(task); } catch (_) { /* inactive browsing context: tasks no longer run */ }
    }
    function faceSlot(value) {
        const s = apply(weakGet, faces, [value]);
        if (!s) throw new TypeError('Illegal FontFace receiver');
        return s;
    }
    function setSlot(value) {
        const t = apply(weakGet, sets, [value]);
        if (!t) throw new TypeError('Illegal FontFaceSet receiver');
        return t;
    }
    const networkError = () => new Exception('The font could not be loaded', 'NetworkError');
    const syntaxError = message => new Exception(message, 'SyntaxError');
    function statusPromise(s) {
        s.promise = new NativePromise((resolve, reject) => { s.resolve = resolve; s.reject = reject; });
        /* Load failures reach the callers of load() and of FontFaceSet.load();
           the status promise itself is not an unhandled rejection. */
        apply(promiseThen, s.promise, [undefined, () => {}]);
    }

    /* ---- FontFaceSet state (CSS Font Loading 3.2 and 3.3) */
    function newReady(t) {
        t.ready = new NativePromise(resolve => { t.resolveReady = resolve; });
        t.readyFulfilled = false;
    }
    function fireLoad(set, type, list) {
        const event = new FontFaceSetLoadEvent(type);
        apply(weakGet, loadEventSlots, [event]).fontfaces = freeze(list);
        eventState(event).isTrusted = true;
        dispatch(set, event);
    }
    function switchToLoading(set, t) {
        t.status = 'loading';
        if (t.readyFulfilled) newReady(t);
        queue(() => fireLoad(set, 'loading', []));
    }
    function switchToLoaded(set, t) {
        /* Only a document's own set waits for its environment. */
        if (t.main && native('pending')) { t.stuck = true; native('stuck', true); return; }
        t.status = 'loaded';
        if (!t.readyFulfilled) { t.readyFulfilled = true; t.resolveReady(set); }
        queue(() => {
            const loaded = t.loaded, failed = t.failed;
            t.loaded = []; t.failed = [];
            fireLoad(set, 'loadingdone', loaded);
            if (failed.length) fireLoad(set, 'loadingerror', failed);
        });
    }
    /* A FontFace's status became loading, then loaded or error (CSS Font
       Loading 2.2): every set holding it follows. */
    function setLoading(face, s) {
        s.status = 'loading';
        apply(setForEach, s.sets, [set => {
            const t = setSlot(set);
            if (!t.loading.length) switchToLoading(set, t);
            append(t.loading, face);
        }]);
    }
    function settle(face, s, loaded, error) {
        if (s.status !== 'loading') return;
        apply(setDelete, loadingFaces, [face]);
        if (loaded) { s.status = 'loaded'; s.resolve(face); }
        else { s.status = 'error'; s.reject(error()); }
        apply(setForEach, s.sets, [set => {
            const t = setSlot(set);
            append(loaded ? t.loaded : t.failed, face);
            if (removeFrom(t.loading, face) && !t.loading.length) switchToLoaded(set, t);
        }]);
    }
    /* FontFace load() steps 2-5. A CSS-connected face loads its rule. */
    function startLoad(face, s) {
        if (!s.urls || s.status !== 'unloaded') return;
        setLoading(face, s);
        let status;
        if (s.css) {
            status = native('ruleLoad', setSlot(s.css.set).document, s.css.id, s.css.occurrence);
            if (status === null) status = ERROR;
        } else status = s.record ? native('load', s.record) : ERROR;
        if (status === LOADED || status === ERROR) queue(() => settle(face, s, status === LOADED, networkError));
        else if (!s.css) apply(setAdd, loadingFaces, [face]);
    }
    /* The native status of a record that load() or rendering started. */
    function poll(face) {
        const s = faceSlot(face);
        if (!s.record || s.data || s.broken || !s.urls || (s.status !== 'unloaded' && s.status !== 'loading')) return;
        const status = native('status', s.record);
        if (status === UNLOADED) return;
        if (s.status === 'unloaded') setLoading(face, s);
        if (status === LOADED || status === ERROR) settle(face, s, status === LOADED, networkError);
        else apply(setAdd, loadingFaces, [face]);
    }

    /* ---- CSS-connected FontFaces: one per @font-face rule of the document,
       in document order, before the faces scripts added. */
    const ruleKey = (id, occurrence) => id + ':' + occurrence;
    function blankValues() { return ['', '', '', '', '', '', '', '', '', '', '', '']; }
    function connected(set, t, id, occurrence) {
        const values = native('rule', t.document, id, occurrence);
        const face = create(FontFace.prototype);
        const s = {record: null, id: 0, css: {set, id, occurrence}, values: values || blankValues(), status: 'unloaded',
            sets: new NativeSet(), urls: true, data: false, broken: false, bytes: null};
        statusPromise(s);
        apply(weakSet, faces, [face, s]);
        apply(setAdd, s.sets, [set]);
        return face;
    }
    /* The status a newly found rule already has. A set being created takes it
       as its starting state; later, a loading rule switches the set. */
    function initial(face, s, t, status, quiet) {
        if (status === LOADING) {
            if (quiet) { s.status = 'loading'; append(t.loading, face); }
            else setLoading(face, s);
        } else if (status === LOADED) { s.status = 'loaded'; s.resolve(face); }
        else if (status === ERROR) { s.status = 'error'; s.reject(networkError()); }
    }
    function advance(face, s, status) {
        if (status === UNLOADED || s.status === 'loaded' || s.status === 'error') return;
        if (s.status === 'unloaded') setLoading(face, s);
        if (status === LOADED || status === ERROR) settle(face, s, status === LOADED, networkError);
    }
    /* Its rule left the document: the face is no longer CSS-connected. */
    function disconnect(set, t, face) {
        const s = faceSlot(face);
        s.css = null;
        apply(setDelete, s.sets, [set]);
        removeFrom(t.loaded, face); removeFrom(t.failed, face);
        if (removeFrom(t.loading, face) && !t.loading.length) switchToLoaded(set, t);
    }
    function rebuild(t) {
        apply(setClear, t.backing, []);
        for (let i = 0; i < t.order.length; i++) apply(setAdd, t.backing, [t.order[i]]);
        for (let i = 0; i < t.authors.length; i++) apply(setAdd, t.backing, [t.authors[i]]);
    }
    const MEMBERSHIP = 0, PROGRESS = 1, CREATION = 2;
    /* The set's entries follow the document's style sheets at once; the
       statuses of known rules move only in the font loading task. */
    function syncRules(set, t, mode) {
        const list = native('rules', t.document);
        if (list === null) return; /* unavailable now: keep what was known */
        const next = new NativeMap(), order = [];
        let changed = false;
        for (let i = 0; i + 2 < list.length; i += 3) {
            const id = list[i], occurrence = list[i + 1], status = list[i + 2], key = ruleKey(id, occurrence);
            let face = apply(mapGet, t.css, [key]);
            if (face === undefined) {
                face = connected(set, t, id, occurrence);
                initial(face, faceSlot(face), t, status, mode === CREATION);
                changed = true;
            } else {
                if (t.order[order.length] !== face) changed = true;
                if (mode === PROGRESS) advance(face, faceSlot(face), status);
            }
            apply(mapSet, next, [key, face]);
            append(order, face);
        }
        if (order.length !== t.order.length) changed = true;
        const previous = t.css;
        t.css = next; t.order = order;
        apply(mapForEach, previous, [(face, key) => { if (apply(mapGet, next, [key]) === undefined) disconnect(set, t, face); }]);
        if (changed) rebuild(t);
    }
    function refresh(set) {
        const t = setSlot(set);
        syncRules(set, t, MEMBERSHIP);
        return t;
    }
    /* Find the matching font faces (3.1) among this set's faces. */
    function matching(t, font, text) {
        const list = native('match', t.document, font, text);
        if (list === null) throw syntaxError('The font is not a valid font shorthand');
        const out = [];
        for (let i = 0; i + 2 < list.length; i += 3) {
            let face;
            if (list[i + 2]) {
                for (let j = 0; j < t.authors.length; j++)
                    if (faceSlot(t.authors[j]).id === list[i + 2]) { face = t.authors[j]; break; }
            } else face = apply(mapGet, t.css, [ruleKey(list[i], list[i + 1])]);
            if (face !== undefined) append(out, face);
        }
        return out;
    }
    function waitAll(list) {
        return new NativePromise((resolve, reject) => {
            const result = [];
            let remaining = list.length;
            if (!remaining) { resolve(result); return; }
            for (let i = 0; i < list.length; i++)
                apply(promiseThen, faceSlot(list[i]).promise, [face => {
                    defineProperty(result, i, {value: face, writable: true, enumerable: true, configurable: true});
                    if (!--remaining) resolve(result);
                }, reject]);
        });
    }

    /* ---- FontFace (2) */
    function bufferBytes(buffer, offset, length) {
        /* A copy now: [[Data]] is what the constructor was given. */
        try {
            const out = new Bytes(length);
            apply(typedSet, out, [new Bytes(buffer, offset, length)]);
            return apply(typedBuffer, out, []);
        } catch (_) { return new ArrayBuffer(0); } /* detached */
    }
    function isArrayBuffer(value) {
        try { apply(abLength, value, []); return true; } catch (_) { return false; }
    }
    /* (CSSOMString or BufferSource): an unshared ArrayBuffer or a view of one;
       null means the source converts as a string. */
    function bufferSource(value) {
        if (value === null || (typeof value !== 'object' && typeof value !== 'function')) return null;
        if (isView(value)) {
            const typed = apply(typedTag, value, []) !== undefined;
            const buffer = apply(typed ? typedBuffer : dvBuffer, value, []);
            if (!isArrayBuffer(buffer)) throw new TypeError('A view of a shared buffer is not a BufferSource');
            return bufferBytes(buffer, apply(typed ? typedOffset : dvOffset, value, []), apply(typed ? typedLength : dvLength, value, []));
        }
        return isArrayBuffer(value) ? bufferBytes(value, 0, apply(abLength, value, [])) : null;
    }
    /* FontFaceDescriptors, its members converted in lexicographic order; width
       (CSS Fonts 4's font-width) wins over its legacy alias stretch. */
    function descriptorValues(init) {
        if (init !== undefined && init !== null && typeof init !== 'object' && typeof init !== 'function')
            throw new TypeError('FontFaceDescriptors must be a dictionary');
        const read = (name, fallback) => {
            if (init === undefined || init === null) return fallback;
            const value = init[name];
            return value === undefined ? fallback : string(value);
        };
        const ascent = read('ascentOverride', 'normal'), descent = read('descentOverride', 'normal'), display = read('display', 'auto'),
            features = read('featureSettings', 'normal'), lineGap = read('lineGapOverride', 'normal'), sizeAdjust = read('sizeAdjust', '100%'),
            stretch = read('stretch', 'normal'), style = read('style', 'normal'), range = read('unicodeRange', 'U+0-10FFFF'),
            variations = read('variationSettings', 'normal'), weight = read('weight', 'normal'), width = read('width', undefined);
        return [null, style, weight, width === undefined ? stretch : width, range, features, variations, display,
            ascent, descent, lineGap, sizeAdjust];
    }
    /* The data steps of the constructor (step 5): status loading in one task,
       the parse result in a later one. */
    function decodeData(face, s) {
        if (s.status !== 'unloaded') return;
        setLoading(face, s);
        queue(() => {
            const bytes = s.bytes;
            s.bytes = null;
            let decoded = false;
            try { decoded = native('data', s.record, bytes); } catch (_) { decoded = false; }
            settle(face, s, decoded, () => syntaxError('The font data could not be parsed'));
        });
    }
    function describe(face, which, value) {
        const s = faceSlot(face);
        value = string(value);
        let text;
        if (s.css) {
            /* A CSS-connected face writes its rule's descriptor too. */
            text = native('ruleSet', setSlot(s.css.set).document, s.css.id, s.css.occurrence, which, value);
            if (text === null) text = native('set', null, which, value); /* the rule left; still validate */
        } else text = native('set', s.record && !s.broken ? s.record : null, which, value);
        if (text === null) throw syntaxError('The value is not valid for this font descriptor');
        s.values[which] = text;
    }
    class FontFace {
        constructor(family, source, descriptors = undefined) {
            if (arguments.length < 2) throw new TypeError('FontFace requires a family and a source');
            family = string(family);
            const bytes = bufferSource(source), text = bytes === null ? string(source) : null;
            const values = descriptorValues(descriptors);
            const record = native('create');
            const s = {record, id: native('id', record), css: null, values: blankValues(), status: 'unloaded',
                sets: new NativeSet(), urls: text !== null, data: bytes !== null, broken: false, bytes};
            statusPromise(s);
            apply(weakSet, faces, [this, s]);
            for (let which = STYLE; which < DESCRIPTORS; which++) {
                const value = native('set', record, which, values[which]);
                if (value === null) s.broken = true;
                s.values[which] = value === null ? '' : value;
            }
            if (text !== null && !native('source', record, text)) s.broken = true;
            /* The family goes on the record last: only a face whose descriptors
               and source parsed takes part in font matching. */
            s.values[FAMILY] = native('set', s.broken ? null : record, FAMILY, family);
            if (s.broken) {
                s.status = 'error'; s.bytes = null;
                s.reject(syntaxError('A FontFace descriptor or its source is not valid'));
            } else if (bytes !== null) queue(() => decodeData(this, s));
        }
        get status() { return faceSlot(this).status; }
        get loaded() { return faceSlot(this).promise; }
        load() {
            let s;
            try { s = faceSlot(this); } catch (e) { return apply(promiseReject, NativePromise, [e]); }
            startLoad(this, s);
            return s.promise;
        }
    }
    for (const [name, which] of [['family', FAMILY], ['style', STYLE], ['weight', WEIGHT], ['stretch', WIDTH],
        ['width', WIDTH], ['unicodeRange', UNICODE_RANGE], ['featureSettings', FEATURE_SETTINGS],
        ['variationSettings', VARIATION_SETTINGS], ['display', DISPLAY], ['ascentOverride', ASCENT_OVERRIDE],
        ['descentOverride', DESCENT_OVERRIDE], ['lineGapOverride', LINE_GAP_OVERRIDE], ['sizeAdjust', SIZE_ADJUST]])
        defineProperty(FontFace.prototype, name, {configurable: true, enumerable: true,
            get: {[name]() { return faceSlot(this).values[which]; }}[name],
            set: {[name](value) {
                if (!arguments.length) throw new TypeError('A value is required');
                describe(this, which, value);
            }}[name]});

    /* ---- FontFaceSetLoadEvent (3.1) */
    function faceSequence(value) {
        if (value === null || (typeof value !== 'object' && typeof value !== 'function'))
            throw new TypeError('fontfaces must be a sequence of FontFace');
        const list = [];
        for (const face of value) { faceSlot(face); append(list, face); }
        return list;
    }
    class FontFaceSetLoadEvent extends Event {
        constructor(type, init = undefined) {
            if (!arguments.length) throw new TypeError('FontFaceSetLoadEvent requires a type');
            if (init !== undefined && init !== null && typeof init !== 'object' && typeof init !== 'function')
                throw new TypeError('FontFaceSetLoadEventInit must be a dictionary');
            super(string(type), init === null ? undefined : init);
            const value = init === undefined || init === null ? undefined : init.fontfaces;
            apply(weakSet, loadEventSlots, [this, {fontfaces: freeze(value === undefined ? [] : faceSequence(value))}]);
        }
        get fontfaces() {
            const e = apply(weakGet, loadEventSlots, [this]);
            if (!e) throw new TypeError('Illegal FontFaceSetLoadEvent receiver');
            return e.fontfaces;
        }
    }

    /* ---- FontFaceSet (3) */
    function requireFace(args, method) {
        if (!args.length) throw new TypeError(`FontFaceSet.${method} requires a FontFace`);
        faceSlot(args[0]);
        return args[0];
    }
    class FontFaceSet extends EventTarget {
        constructor(token) {
            if (token !== constructing) throw new TypeError('Illegal constructor');
            super();
        }
        get size() { return apply(setSize, refresh(this).backing, []); }
        has(font) { const t = refresh(this); requireFace(arguments, 'has'); return apply(setHas, t.backing, [font]); }
        entries() { return apply(setEntries, refresh(this).backing, []); }
        values() { return apply(setValues, refresh(this).backing, []); }
        forEach(callback, thisArg = undefined) {
            const t = refresh(this), set = this;
            if (typeof callback !== 'function') throw new TypeError('FontFaceSet.forEach requires a callback');
            apply(setForEach, t.backing, [face => { apply(callback, thisArg, [face, face, set]); }]);
        }
        add(font) {
            const t = refresh(this);
            const s = faceSlot(requireFace(arguments, 'add'));
            if (apply(setHas, t.backing, [font])) return this;
            if (s.css) throw new Exception('A CSS-connected FontFace belongs to its style sheet', 'InvalidModificationError');
            if (s.record) native('add', t.document, s.record);
            append(t.authors, font);
            apply(setAdd, t.backing, [font]);
            apply(setAdd, s.sets, [this]);
            if (s.status === 'loading') {
                if (!t.loading.length) switchToLoading(this, t);
                append(t.loading, font);
            }
            return this;
        }
        delete(font) {
            const t = refresh(this);
            const s = faceSlot(requireFace(arguments, 'delete'));
            if (s.css || !removeFrom(t.authors, font)) return false;
            apply(setDelete, t.backing, [font]);
            apply(setDelete, s.sets, [this]);
            if (s.record) native('delete', t.document, s.record);
            removeFrom(t.loaded, font); removeFrom(t.failed, font);
            if (removeFrom(t.loading, font) && !t.loading.length) switchToLoaded(this, t);
            return true;
        }
        clear() {
            const t = refresh(this), authors = t.authors;
            t.authors = [];
            for (let i = 0; i < authors.length; i++) {
                apply(setDelete, t.backing, [authors[i]]);
                apply(setDelete, faceSlot(authors[i]).sets, [this]);
            }
            native('clear', t.document);
            const keep = list => { const out = []; for (let i = 0; i < list.length; i++) if (faceSlot(list[i]).css) append(out, list[i]); return out; };
            t.loaded = keep(t.loaded); t.failed = keep(t.failed);
            if (t.loading.length) { t.loading = []; switchToLoaded(this, t); }
        }
        load(font, text = undefined) {
            try {
                const t = refresh(this);
                if (!arguments.length) throw new TypeError('FontFaceSet.load requires a font');
                font = string(font);
                text = text === undefined ? ' ' : string(text);
                const list = matching(t, font, text);
                return new NativePromise((resolve, reject) => {
                    queue(() => {
                        for (let i = 0; i < list.length; i++) startLoad(list[i], faceSlot(list[i]));
                        apply(promiseThen, waitAll(list), [resolve, reject]);
                    });
                });
            } catch (e) { return apply(promiseReject, NativePromise, [e]); }
        }
        check(font, text = undefined) {
            const t = refresh(this);
            if (!arguments.length) throw new TypeError('FontFaceSet.check requires a font');
            font = string(font);
            text = text === undefined ? ' ' : string(text);
            const list = matching(t, font, text);
            for (let i = 0; i < list.length; i++) if (faceSlot(list[i]).status !== 'loaded') return false;
            return true;
        }
        get ready() { return refresh(this).ready; }
        get status() { return refresh(this).status; }
    }
    defineProperty(FontFaceSet.prototype, 'keys', {value: FontFaceSet.prototype.values, writable: true, configurable: true});
    defineProperty(FontFaceSet.prototype, Symbol.iterator, {value: FontFaceSet.prototype.values, writable: true, configurable: true});
    for (const type of ['loading', 'loadingdone', 'loadingerror'])
        defineProperty(FontFaceSet.prototype, 'on' + type, {configurable: true, enumerable: true,
            get: {['on' + type]() { setSlot(this); return handlerValue(this, type); }}['on' + type],
            set: {['on' + type](value) { setSlot(this); setHandler(this, type, value, null); }}['on' + type]});
    fontHandlerTarget = (target, type) => apply(weakHas, sets, [target]) && apply(setHas, loadEvents, [type]);

    /* Document includes FontFaceSource: each document has its own set. */
    function documentFonts(doc) {
        let set = apply(weakGet, documentSets, [doc]);
        if (set) return set;
        native('activate');
        set = new FontFaceSet(constructing);
        const t = {document: doc, main: doc === realmDocument, backing: new NativeSet(), css: new NativeMap(), order: [],
            authors: [], loading: [], loaded: [], failed: [], status: 'loaded', ready: null, resolveReady: null,
            readyFulfilled: false, stuck: false};
        apply(weakSet, sets, [set, t]);
        newReady(t);
        /* It has existed since the document did: no events for its start. */
        syncRules(set, t, CREATION);
        if (t.loading.length) t.status = 'loading';
        else if (t.main && native('pending')) { t.status = 'loading'; t.stuck = true; native('stuck', true); }
        else { t.readyFulfilled = true; t.resolveReady(set); }
        apply(weakSet, documentSets, [doc, set]);
        if (t.main) documentSet = set;
        else append(otherSets, new NativeWeakRef(set));
        return set;
    }
    defineProperty(Document.prototype, 'fonts', {configurable: true, enumerable: true,
        get: {fonts() {
            if (!rawDom.isNode(null, this) || rawDom.get(this, 'nodeType') !== 9) throw new TypeError('Illegal Document receiver');
            return documentFonts(this);
        }}.fonts});

    /* The font loading task (js.c web_js_tick): rules and records that load()
       or rendering advanced, then the environment document.fonts waits for. */
    function changed() {
        const live = [];
        if (documentSet) append(live, documentSet);
        const kept = [];
        for (let i = 0; i < otherSets.length; i++) {
            const set = apply(deref, otherSets[i], []);
            if (set) { append(kept, otherSets[i]); append(live, set); }
        }
        otherSets = kept;
        for (let i = 0; i < live.length; i++) {
            const set = live[i], t = setSlot(set);
            try { syncRules(set, t, PROGRESS); } catch (e) { report(e); }
            const authors = t.authors;
            for (let j = 0; j < authors.length; j++) try { poll(authors[j]); } catch (e) { report(e); }
        }
        apply(setForEach, loadingFaces, [face => { try { poll(face); } catch (e) { report(e); } }]);
        if (documentSet) {
            const t = setSlot(documentSet);
            if (t.stuck && !native('pending')) {
                t.stuck = false;
                if (!t.loading.length) switchToLoaded(documentSet, t);
            }
            native('stuck', t.stuck);
        }
    }

    for (const [name, value] of [['FontFace', FontFace], ['FontFaceSet', FontFaceSet], ['FontFaceSetLoadEvent', FontFaceSetLoadEvent]]) {
        defineProperty(value.prototype, Symbol.toStringTag, {value: name, configurable: true});
        defineProperty(globalThis, name, {value, writable: true, configurable: true});
    }
    return {changed};
})();
