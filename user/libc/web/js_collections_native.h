/* The DOM collections' native half, included by js.c after native_dom_impl.
   NodeList, HTMLCollection and the interfaces built on them
   (HTMLOptionsCollection, HTMLFormControlsCollection, RadioNodeList,
   DOMRectList) are objects of one exotic class whose indexed and named
   properties are Web IDL's legacy platform objects
   (https://webidl.spec.whatwg.org/#es-legacy-platform-objects): its
   [[GetOwnProperty]], [[DefineOwnProperty]], [[Set]], [[Delete]],
   [[PreventExtensions]] and [[OwnPropertyKeys]] are the class methods below.
   QuickJS looks at an object's ordinary own properties before it calls them,
   and lists the class's keys before the ordinary ones (README.nocturne.md).

   js_collections.js owns the interfaces' members and what each collection
   holds: a static list keeps its array; a live collection calls its reader on
   every access (control state such as selectedness changes without a DOM
   revision); a DOM-tree collection calls it only when its root's owner
   Document or that Document's DOM revision changed since the last read. An
   HTMLCollection's supported property names (DOM) are the elements' IDs and
   HTML-namespace name attributes; HTMLFormControlsCollection supplies its own
   named getter. */

static JSClassID collection_class;

/* The interface whose members a collection has: item(), namedItem() and the
   length getter take only an object that implements theirs (RadioNodeList is
   a NodeList, HTMLOptionsCollection and HTMLFormControlsCollection are
   HTMLCollections; DOMRectList is neither). */
enum collection_family { COLLECTION_OTHER, COLLECTION_NODE_LIST, COLLECTION_HTML };

struct collection_name { JSAtom atom; uint32_t index; };
struct js_collection {
    JSValue read;   /* the reader; JS_UNDEFINED for a static list */
    JSValue node;   /* a DOM-tree collection's root, else JS_UNDEFINED */
    JSValue owner;  /* the root's owner Document at the last read */
    uint64_t revision;
    JSValue array;  /* the items: the static list's array or the last read */
    uint32_t count;
    JSValue setter; /* the indexed property setter (HTMLOptionsCollection) */
    JSValue lookup; /* a named getter of the interface's own */
    uint8_t family; /* COLLECTION_HTML has named properties ([LegacyUnenumerableNamedProperties]) */
    bool read_once; /* a DOM-tree collection has read at 'owner'/'revision' */
    /* The supported property names of 'array', in order, and an open
       addressing table of them (a slot holds an entry's position + 1). */
    bool names_valid;
    struct collection_name *names;
    uint32_t name_count, *table, table_mask;
};

static void collection_names_free(JSRuntime *rt, struct js_collection *c) {
    for (uint32_t i = 0; i < c->name_count; i++) JS_FreeAtomRT(rt, c->names[i].atom);
    js_free_rt(rt, c->names); js_free_rt(rt, c->table);
    c->names = NULL; c->table = NULL; c->name_count = c->table_mask = 0; c->names_valid = false;
}
static void collection_finalizer(JSRuntime *rt, JSValue object) {
    struct js_collection *c = JS_GetOpaque(object, collection_class);
    if (!c) return;
    collection_names_free(rt, c);
    JS_FreeValueRT(rt, c->read); JS_FreeValueRT(rt, c->node); JS_FreeValueRT(rt, c->owner);
    JS_FreeValueRT(rt, c->array); JS_FreeValueRT(rt, c->setter); JS_FreeValueRT(rt, c->lookup);
    js_free_rt(rt, c);
}
static void collection_gc_mark(JSRuntime *rt, JSValueConst object, JS_MarkFunc *mark) {
    struct js_collection *c = JS_GetOpaque(object, collection_class);
    if (!c) return;
    JS_MarkValue(rt, c->read, mark); JS_MarkValue(rt, c->node, mark); JS_MarkValue(rt, c->owner, mark);
    JS_MarkValue(rt, c->array, mark); JS_MarkValue(rt, c->setter, mark); JS_MarkValue(rt, c->lookup, mark);
}

static int collection_array_length(JSContext *ctx, JSValueConst array, uint32_t *count) {
    JSValue length = JS_GetPropertyStr(ctx, array, "length");
    if (JS_IsException(length)) return -1;
    int ok = JS_ToUint32(ctx, count, length);
    JS_FreeValue(ctx, length);
    return ok;
}
/* The items become what the reader returns now. */
static int collection_read(JSContext *ctx, struct js_collection *c) {
    JSValue array = JS_Call(ctx, c->read, JS_UNDEFINED, 0, NULL);
    if (JS_IsException(array)) return -1;
    uint32_t count = 0;
    if (collection_array_length(ctx, array, &count) < 0) { JS_FreeValue(ctx, array); return -1; }
    JSValue old = c->array;
    c->array = array; c->count = count;
    collection_names_free(JS_GetRuntime(ctx), c);
    JS_FreeValue(ctx, old);
    return 0;
}
/* The items as of now. A DOM-tree collection checks what the previous
   bindings read through the native DOM for each access: its root's owner
   Document (adoption changes it without changing either revision) and that
   Document's DOM revision, with the authority check of every DOM operation. */
static int collection_current(JSContext *ctx, struct js_collection *c) {
    if (JS_IsUndefined(c->read)) return 0;
    if (JS_IsUndefined(c->node)) return collection_read(ctx, c);
    struct web_js_state *s = state(ctx);
    node_t *n = node_opaque(c->node);
    if (!s || !n) { JS_ThrowTypeError(ctx, "Invalid DOM receiver"); return -1; }
    web_doc *d = n->owner ? n->owner : s->doc;
    if (dom_authority_denied(s, d)) {
        history_security_error(ctx, "Sandbox denied borrowed document authority");
        return -1;
    }
    doc_dom_budget(d);
    if (c->read_once && node_opaque(c->owner) == d->root && c->revision == d->dom_revision) return 0;
    uint64_t revision = d->dom_revision;
    JSValue owner = wrap(s, d->root);
    if (JS_IsException(owner)) return -1;
    if (collection_read(ctx, c) < 0) { JS_FreeValue(ctx, owner); return -1; }
    /* Published only after a successful read. */
    JSValue old = c->owner;
    c->owner = owner; c->revision = revision; c->read_once = true;
    JS_FreeValue(ctx, old);
    return 0;
}
static JSValue collection_item(JSContext *ctx, struct js_collection *c, uint32_t index) {
    return JS_GetPropertyUint32(ctx, c->array, index);
}

static uint32_t collection_slot(JSAtom atom, uint32_t mask) {
    uint32_t h = atom;
    h ^= h >> 16; h *= 0x45d9f3bu; h ^= h >> 16;
    return h & mask;
}
static int64_t collection_name_entry(const struct collection_name *names, const uint32_t *table, uint32_t mask, JSAtom atom) {
    if (!table) return -1;
    for (uint32_t slot = collection_slot(atom, mask);; slot = (slot + 1) & mask) {
        uint32_t entry = table[slot];
        if (!entry) return -1;
        if (names[entry - 1].atom == atom) return entry - 1;
    }
}
/* An attribute value as a property key: a string's atom, a canonical
   numeric string's index atom. */
static JSAtom collection_atom(JSContext *ctx, const char *text) {
    for (const unsigned char *p = (const unsigned char *)text; *p; p++)
        if (*p >= 0x80) {
            /* JS_NewAtom matches bytes against Latin-1 atoms; decode UTF-8 first. */
            JSValue string = JS_NewString(ctx, text);
            if (JS_IsException(string)) return JS_ATOM_NULL;
            JSAtom atom = JS_ValueToAtom(ctx, string);
            JS_FreeValue(ctx, string);
            return atom;
        }
    return JS_NewAtom(ctx, text);
}
/* DOM's supported property names of an HTMLCollection: for each element in
   order, its ID, then, in the HTML namespace, its name attribute, each unless
   empty or listed already. The ID and name are the null-namespace attributes
   IDL reflection reads. */
static int collection_names(JSContext *ctx, struct js_collection *c) {
    if (c->names_valid) return 0;
    collection_names_free(JS_GetRuntime(ctx), c);
    struct collection_name *names = NULL;
    uint32_t count = 0, capacity = 0, *table = NULL, mask = 0;
    for (uint32_t i = 0; i < c->count; i++) {
        JSValue item = collection_item(ctx, c, i);
        if (JS_IsException(item)) goto fail;
        /* The array holds the wrapper, so its node outlives this loop. */
        node_t *n = node_opaque(item);
        JS_FreeValue(ctx, item);
        if (!n || n->type != N_ELEM) continue;
        const char *ns = doc_element_namespace(n);
        bool html = ns && !strcmp(ns, WEB_NS_HTML);
        for (int k = 0; k < (html ? 2 : 1); k++) {
            int a = doc_attr_index(n, NULL, k ? "name" : "id", true);
            const char *value = a >= 0 ? n->attrs[a].value : NULL;
            if (!value || !*value) continue;
            JSAtom atom = collection_atom(ctx, value);
            if (atom == JS_ATOM_NULL) goto fail;
            if (collection_name_entry(names, table, mask, atom) >= 0) { JS_FreeAtom(ctx, atom); continue; }
            if (count == capacity) {
                uint32_t more = capacity ? capacity * 2 : 8;
                if (more <= capacity) { JS_FreeAtom(ctx, atom); JS_ThrowOutOfMemory(ctx); goto fail; }
                struct collection_name *grown = js_realloc(ctx, names, (size_t)more * sizeof *grown);
                if (!grown) { JS_FreeAtom(ctx, atom); goto fail; }
                names = grown; capacity = more;
            }
            if ((uint64_t)(count + 1) * 2 > (uint64_t)mask + 1 || !table) {
                uint32_t slots = table ? (mask + 1) * 2 : 16;
                uint32_t *grown = js_mallocz(ctx, (size_t)slots * sizeof *grown);
                if (!grown) { JS_FreeAtom(ctx, atom); goto fail; }
                for (uint32_t e = 0; e < count; e++) {
                    uint32_t slot = collection_slot(names[e].atom, slots - 1);
                    while (grown[slot]) slot = (slot + 1) & (slots - 1);
                    grown[slot] = e + 1;
                }
                js_free(ctx, table); table = grown; mask = slots - 1;
            }
            names[count] = (struct collection_name){atom, i};
            uint32_t slot = collection_slot(atom, mask);
            while (table[slot]) slot = (slot + 1) & mask;
            table[slot] = ++count;
        }
    }
    c->names = names; c->name_count = count; c->table = table; c->table_mask = mask; c->names_valid = true;
    return 0;
fail:
    for (uint32_t e = 0; e < count; e++) JS_FreeAtom(ctx, names[e].atom);
    js_free(ctx, names); js_free(ctx, table);
    return -1;
}
/* namedItem(key): the first element whose ID or HTML-namespace name is key,
   or null (also for the empty string). */
static JSValue collection_named_item(JSContext *ctx, struct js_collection *c, JSAtom atom) {
    if (collection_current(ctx, c) < 0 || collection_names(ctx, c) < 0) return JS_EXCEPTION;
    int64_t entry = collection_name_entry(c->names, c->table, c->table_mask, atom);
    return entry < 0 ? JS_NULL : collection_item(ctx, c, c->names[entry].index);
}
/* The named property P's value, or JS_UNDEFINED when P is no supported
   property name. */
static JSValue collection_named_value(JSContext *ctx, struct js_collection *c, JSAtom atom) {
    if (JS_IsUndefined(c->lookup)) {
        JSValue value = collection_named_item(ctx, c, atom);
        return JS_IsNull(value) ? JS_UNDEFINED : value;
    }
    JSValue name = JS_AtomToString(ctx, atom);
    if (JS_IsException(name)) return name;
    JSValue value = JS_Call(ctx, c->lookup, JS_UNDEFINED, 1, (JSValueConst *)&name);
    JS_FreeValue(ctx, name);
    if (JS_IsException(value)) return value;
    if (JS_ToBool(ctx, value) <= 0) { JS_FreeValue(ctx, value); return JS_UNDEFINED; }
    return value;
}
/* Web IDL's named property visibility for a key the object does not own
   (QuickJS has looked there first): no object on its prototype chain has the
   property. 1 visible, 0 not, -1 exception. */
static int collection_name_visible(JSContext *ctx, JSValueConst object, JSAtom atom) {
    JSValue prototype = JS_GetPrototype(ctx, object);
    if (JS_IsException(prototype)) return -1;
    int has = JS_IsObject(prototype) ? JS_HasProperty(ctx, prototype, atom) : 0;
    JS_FreeValue(ctx, prototype);
    return has < 0 ? -1 : !has;
}
static int collection_set_index(JSContext *ctx, struct js_collection *c, uint32_t index, JSValueConst value) {
    JSValueConst args[2] = {JS_NewUint32(ctx, index), value};
    JSValue result = JS_Call(ctx, c->setter, JS_UNDEFINED, 2, args);
    if (JS_IsException(result)) return -1;
    JS_FreeValue(ctx, result);
    return 1;
}

/* [[GetOwnProperty]] (LegacyPlatformObjectGetOwnProperty with ignoreNamedProps
   false): a supported index's item, enumerable and configurable, writable only
   with an indexed setter; an array index is never a named property; a visible
   named property is configurable and neither enumerable nor writable. */
static int collection_get_own_property(JSContext *ctx, JSPropertyDescriptor *desc, JSValueConst object, JSAtom atom) {
    struct js_collection *c = JS_GetOpaque(object, collection_class);
    if (!c) return 0;
    uint32_t index;
    int kind = JS_AtomKeyKind(ctx, atom, &index);
    if (kind == JS_ATOM_KEY_INDEX) {
        if (collection_current(ctx, c) < 0) return -1;
        if (index >= c->count) return 0;
        if (desc) {
            JSValue value = collection_item(ctx, c, index);
            if (JS_IsException(value)) return -1;
            desc->flags = JS_PROP_ENUMERABLE | JS_PROP_CONFIGURABLE | (JS_IsUndefined(c->setter) ? 0 : JS_PROP_WRITABLE);
            desc->value = value; desc->getter = desc->setter = JS_UNDEFINED;
        }
        return 1;
    }
    if (c->family != COLLECTION_HTML || kind != JS_ATOM_KEY_STRING) return 0;
    int visible = collection_name_visible(ctx, object, atom);
    if (visible <= 0) return visible;
    JSValue value = collection_named_value(ctx, c, atom);
    if (JS_IsException(value)) return -1;
    if (JS_IsUndefined(value)) return 0;
    if (desc) {
        desc->flags = JS_PROP_CONFIGURABLE;
        desc->value = value; desc->getter = desc->setter = JS_UNDEFINED;
    } else JS_FreeValue(ctx, value);
    return 1;
}
/* [[DefineOwnProperty]] for a key the object does not own: an array index
   only through the indexed setter, with a data descriptor; a supported
   property name not at all (no named setter); anything else ordinarily. */
static int collection_define_own_property(JSContext *ctx, JSValueConst object, JSAtom atom, JSValueConst value,
                                          JSValueConst getter, JSValueConst setter, int flags) {
    struct js_collection *c = JS_GetOpaque(object, collection_class);
    uint32_t index;
    int kind = c ? JS_AtomKeyKind(ctx, atom, &index) : JS_ATOM_KEY_SYMBOL;
    if (kind == JS_ATOM_KEY_INDEX) {
        if (JS_IsUndefined(c->setter) || (flags & (JS_PROP_HAS_GET | JS_PROP_HAS_SET)) ||
            !(flags & (JS_PROP_HAS_VALUE | JS_PROP_HAS_WRITABLE)))
            return JS_ThrowTypeErrorOrFalseStr(ctx, flags, "cannot define an indexed property of this collection");
        return collection_set_index(ctx, c, index, (flags & JS_PROP_HAS_VALUE) ? value : JS_UNDEFINED);
    }
    if (kind == JS_ATOM_KEY_STRING && c->family == COLLECTION_HTML) {
        JSValue named = collection_named_value(ctx, c, atom);
        if (JS_IsException(named)) return -1;
        bool supported = !JS_IsUndefined(named);
        JS_FreeValue(ctx, named);
        if (supported) return JS_ThrowTypeErrorOrFalseStr(ctx, flags, "cannot define a named property of this collection");
    }
    return JS_DefineProperty(ctx, object, atom, value, getter, setter, flags | JS_PROP_NO_EXOTIC);
}
/* OrdinarySetWithOwnDescriptor with a writable data descriptor (or none at the
   end of the prototype chain): the value goes to the receiver. */
static int collection_set_receiver(JSContext *ctx, JSAtom atom, JSValueConst value, JSValueConst receiver, int flags) {
    int throw_flags = flags & (JS_PROP_THROW | JS_PROP_THROW_STRICT);
    if (!JS_IsObject(receiver)) return JS_ThrowTypeErrorOrFalseStr(ctx, flags, "cannot set a property of a primitive");
    JSPropertyDescriptor d;
    int has = JS_GetOwnProperty(ctx, &d, receiver, atom);
    if (has < 0) return -1;
    if (has) {
        bool data = !(d.flags & JS_PROP_GETSET), writable = (d.flags & JS_PROP_WRITABLE) != 0;
        JS_FreeValue(ctx, d.value); JS_FreeValue(ctx, d.getter); JS_FreeValue(ctx, d.setter);
        if (!data || !writable) return JS_ThrowTypeErrorOrFalseStr(ctx, flags, "property is read-only");
        return JS_DefineProperty(ctx, receiver, atom, value, JS_UNDEFINED, JS_UNDEFINED, JS_PROP_HAS_VALUE | throw_flags);
    }
    return JS_DefineProperty(ctx, receiver, atom, value, JS_UNDEFINED, JS_UNDEFINED,
                             JS_PROP_HAS_VALUE | JS_PROP_HAS_WRITABLE | JS_PROP_HAS_ENUMERABLE |
                             JS_PROP_HAS_CONFIGURABLE | JS_PROP_C_W_E | throw_flags);
}
/* [[Set]] for a key the object does not own: an array index on the object
   itself goes to the indexed setter; otherwise the own descriptor ignores the
   named properties (LegacyPlatformObjectGetOwnProperty with ignoreNamedProps
   true) and OrdinarySetWithOwnDescriptor goes on. */
static int collection_set_property(JSContext *ctx, JSValueConst object, JSAtom atom, JSValueConst value,
                                   JSValueConst receiver, int flags) {
    struct js_collection *c = JS_GetOpaque(object, collection_class);
    uint32_t index;
    if (c && JS_AtomKeyKind(ctx, atom, &index) == JS_ATOM_KEY_INDEX) {
        bool same = JS_IsObject(receiver) && JS_VALUE_GET_PTR(receiver) == JS_VALUE_GET_PTR(object);
        if (same && !JS_IsUndefined(c->setter)) return collection_set_index(ctx, c, index, value);
        if (collection_current(ctx, c) < 0) return -1;
        if (index < c->count) {
            if (JS_IsUndefined(c->setter))
                return JS_ThrowTypeErrorOrFalseStr(ctx, flags, "indexed property of this collection is read-only");
            return collection_set_receiver(ctx, atom, value, receiver, flags);
        }
    }
    JSValue prototype = JS_GetPrototype(ctx, object);
    if (JS_IsException(prototype)) return -1;
    if (!JS_IsObject(prototype)) return collection_set_receiver(ctx, atom, value, receiver, flags);
    int set = JS_SetPropertyInternal(ctx, prototype, atom, JS_DupValue(ctx, value), receiver, flags);
    JS_FreeValue(ctx, prototype);
    return set;
}
/* [[Delete]] for a key the object does not own: a supported index or a
   visible named property cannot be deleted (no deleters). */
static int collection_delete_property(JSContext *ctx, JSValueConst object, JSAtom atom) {
    struct js_collection *c = JS_GetOpaque(object, collection_class);
    if (!c) return 1;
    uint32_t index;
    int kind = JS_AtomKeyKind(ctx, atom, &index);
    if (kind == JS_ATOM_KEY_INDEX) {
        if (collection_current(ctx, c) < 0) return -1;
        return index >= c->count;
    }
    if (kind != JS_ATOM_KEY_STRING || c->family != COLLECTION_HTML) return 1;
    int visible = collection_name_visible(ctx, object, atom);
    if (visible <= 0) return visible < 0 ? -1 : 1;
    JSValue named = collection_named_value(ctx, c, atom);
    if (JS_IsException(named)) return -1;
    bool supported = !JS_IsUndefined(named);
    JS_FreeValue(ctx, named);
    return !supported;
}
static int collection_prevent_extensions(JSContext *ctx, JSValueConst object) {
    (void)ctx; (void)object;
    return 0;
}
/* [[OwnPropertyKeys]]'s host part: the supported indices in order, then the
   visible supported property names in order (a name that is an array index
   is never a named property). QuickJS adds the ordinary keys after these. */
static int collection_get_own_property_names(JSContext *ctx, JSPropertyEnum **table, uint32_t *length, JSValueConst object) {
    *table = NULL; *length = 0;
    struct js_collection *c = JS_GetOpaque(object, collection_class);
    if (!c) return 0;
    if (collection_current(ctx, c) < 0) return -1;
    uint32_t count = c->count, names = 0;
    JSAtom *candidates = NULL;
    if (c->family == COLLECTION_HTML) {
        if (collection_names(ctx, c) < 0) return -1;
        /* Copied: the visibility checks below may run JavaScript that reads
           the collection again. */
        candidates = js_malloc(ctx, (size_t)(c->name_count ? c->name_count : 1) * sizeof *candidates);
        if (!candidates) return -1;
        for (uint32_t i = 0; i < c->name_count; i++) {
            uint32_t ignored;
            if (JS_AtomKeyKind(ctx, c->names[i].atom, &ignored) != JS_ATOM_KEY_INDEX)
                candidates[names++] = JS_DupAtom(ctx, c->names[i].atom);
        }
    }
    JSPropertyEnum *out = js_mallocz(ctx, ((size_t)count + names + 1) * sizeof *out);
    uint32_t n = 0, next = 0;
    if (!out) goto fail;
    for (; n < count; n++) {
        out[n].atom = JS_NewAtomUInt32(ctx, n);
        if (out[n].atom == JS_ATOM_NULL) goto fail;
    }
    for (; next < names; next++) {
        int visible = collection_name_visible(ctx, object, candidates[next]);
        if (visible < 0) goto fail;
        if (visible) { out[n++].atom = candidates[next]; candidates[next] = JS_ATOM_NULL; }
        else { JS_FreeAtom(ctx, candidates[next]); candidates[next] = JS_ATOM_NULL; }
    }
    js_free(ctx, candidates);
    *table = out; *length = n;
    return 0;
fail:
    for (uint32_t i = next; i < names; i++) JS_FreeAtom(ctx, candidates[i]);
    js_free(ctx, candidates);
    if (out) { for (uint32_t i = 0; i < n; i++) JS_FreeAtom(ctx, out[i].atom); js_free(ctx, out); }
    return -1;
}
static JSClassExoticMethods collection_exotic = {
    .get_own_property = collection_get_own_property,
    .get_own_property_names = collection_get_own_property_names,
    .delete_property = collection_delete_property,
    .define_own_property = collection_define_own_property,
    .set_property = collection_set_property,
    .prevent_extensions = collection_prevent_extensions,
};

/* A collection implementing family's interface, or a TypeError. */
static struct js_collection *collection_receiver(JSContext *ctx, JSValueConst value, int family) {
    struct js_collection *c = JS_GetOpaque(value, collection_class);
    if (!c || c->family != family) {
        JS_ThrowTypeError(ctx, family == COLLECTION_HTML ? "Illegal HTMLCollection receiver" : "Illegal NodeList receiver");
        return NULL;
    }
    return c;
}
static int collection_family_arg(JSContext *ctx, JSValueConst value) {
    int32_t family;
    if (JS_ToInt32(ctx, &family, value) < 0) return -1;
    if (family != COLLECTION_NODE_LIST && family != COLLECTION_HTML) {
        JS_ThrowRangeError(ctx, "Invalid collection family");
        return -1;
    }
    return family;
}
/* make(prototype, family, source, root, indexedSetter, namedGetter): source is
   a static list's array or a live collection's reader; a root (undefined for
   none) makes the reader a DOM-tree one and must be a node. */
static JSValue collection_make(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
    (void)this_val;
    if (argc < 6 || !JS_IsObject(argv[0])) return JS_ThrowTypeError(ctx, "Invalid collection");
    if (!JS_IsUndefined(argv[3]) && !node_opaque(argv[3])) return JS_ThrowTypeError(ctx, "Invalid DOM receiver");
    int32_t family;
    if (JS_ToInt32(ctx, &family, argv[1]) < 0) return JS_EXCEPTION;
    if (family < COLLECTION_OTHER || family > COLLECTION_HTML) return JS_ThrowRangeError(ctx, "Invalid collection family");
    JSValue object = JS_NewObjectProtoClass(ctx, argv[0], collection_class);
    if (JS_IsException(object)) return object;
    struct js_collection *c = js_mallocz(ctx, sizeof *c);
    if (!c) { JS_FreeValue(ctx, object); return JS_EXCEPTION; }
    c->read = c->node = c->owner = c->array = c->setter = c->lookup = JS_UNDEFINED;
    JS_SetOpaque(object, c);
    c->family = (uint8_t)family;
    if (JS_IsFunction(ctx, argv[2])) {
        c->read = JS_DupValue(ctx, argv[2]);
        c->node = JS_DupValue(ctx, argv[3]);
    } else {
        c->array = JS_DupValue(ctx, argv[2]);
        if (collection_array_length(ctx, c->array, &c->count) < 0) { JS_FreeValue(ctx, object); return JS_EXCEPTION; }
    }
    if (JS_IsFunction(ctx, argv[4])) c->setter = JS_DupValue(ctx, argv[4]);
    if (JS_IsFunction(ctx, argv[5])) c->lookup = JS_DupValue(ctx, argv[5]);
    return object;
}
/* The length getter of family's interface; lengthGetter(family) makes one for
   each interface. */
static JSValue collection_length(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv, int family) {
    (void)argc; (void)argv;
    struct js_collection *c = collection_receiver(ctx, this_val, family);
    if (!c || collection_current(ctx, c) < 0) return JS_EXCEPTION;
    return JS_NewUint32(ctx, c->count);
}
static JSValue collection_length_getter(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
    (void)this_val;
    int family = collection_family_arg(ctx, argc > 0 ? argv[0] : JS_UNDEFINED);
    if (family < 0) return JS_EXCEPTION;
    return JS_NewCFunctionMagic(ctx, collection_length, "length", 0, JS_CFUNC_generic_magic, family);
}
/* item(collection, family, argumentCount, index): the operation's steps once
   its function is called. The receiver implements the interface, the index
   argument is there and becomes an unsigned long; the item or null. */
static JSValue collection_item_at(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
    (void)this_val;
    if (argc < 4) return JS_ThrowTypeError(ctx, "Invalid collection call");
    int family = collection_family_arg(ctx, argv[1]);
    struct js_collection *c = family < 0 ? NULL : collection_receiver(ctx, argv[0], family);
    int32_t supplied;
    uint32_t index;
    if (!c || JS_ToInt32(ctx, &supplied, argv[2]) < 0) return JS_EXCEPTION;
    if (supplied < 1) return JS_ThrowTypeError(ctx, "item requires an index");
    if (JS_ToUint32(ctx, &index, argv[3]) < 0 || collection_current(ctx, c) < 0) return JS_EXCEPTION;
    return index < c->count ? collection_item(ctx, c, index) : JS_NULL;
}
/* named(collection, argumentCount, name): HTMLCollection's namedItem with the
   same steps; the name becomes a DOMString. */
static JSValue collection_named(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
    (void)this_val;
    if (argc < 3) return JS_ThrowTypeError(ctx, "Invalid collection call");
    struct js_collection *c = collection_receiver(ctx, argv[0], COLLECTION_HTML);
    int32_t supplied;
    if (!c || JS_ToInt32(ctx, &supplied, argv[1]) < 0) return JS_EXCEPTION;
    if (supplied < 1) return JS_ThrowTypeError(ctx, "namedItem requires a name");
    JSValue name = JS_ToString(ctx, argv[2]);
    if (JS_IsException(name)) return name;
    JSAtom atom = JS_ValueToAtom(ctx, name);
    JS_FreeValue(ctx, name);
    if (atom == JS_ATOM_NULL) return JS_EXCEPTION;
    JSValue item = collection_named_item(ctx, c, atom);
    JS_FreeAtom(ctx, atom);
    return item;
}
/* host.collection for js_collections.js. */
static int collections_native_init(JSContext *ctx, JSValueConst api) {
    JSRuntime *rt = JS_GetRuntime(ctx);
    if (!collection_class) JS_NewClassID(&collection_class);
    if (!JS_IsRegisteredClass(rt, collection_class)) {
        JSClassDef definition = {.class_name = "NocturneCollection", .finalizer = collection_finalizer,
                                 .gc_mark = collection_gc_mark, .exotic = &collection_exotic};
        if (JS_NewClass(rt, collection_class, &definition) < 0) return -1;
    }
    static const JSCFunctionListEntry functions[] = {
        JS_CFUNC_DEF("make", 6, collection_make), JS_CFUNC_DEF("lengthGetter", 1, collection_length_getter),
        JS_CFUNC_DEF("item", 4, collection_item_at), JS_CFUNC_DEF("named", 3, collection_named),
    };
    JSValue object = JS_NewObject(ctx);
    if (JS_IsException(object)) return -1;
    if (JS_SetPropertyFunctionList(ctx, object, functions, sizeof functions / sizeof *functions) < 0) {
        JS_FreeValue(ctx, object); return -1;
    }
    return JS_SetPropertyStr(ctx, api, "collection", object) < 0 ? -1 : 0;
}
