/* The Web IDL binding layer: the ECMAScript shape of every interface, namespace and member a
   realm's bindings define (Web IDL, "ECMAScript binding",
   https://webidl.spec.whatwg.org/#ecmascript-binding), from the specifications' own IDL
   (webidl_data.inc, which scripts/build_webidl.py makes from third_party/webref/idl).

   The bindings (js_bootstrap.js, js_worker_runtime.js and the programs they include) implement
   each interface as a JavaScript class or function and its members as methods and accessors.
   This layer then gives what they defined the shape the IDL decides, so no binding writes it
   by hand: an interface object is a non-enumerable property of its global (or of its
   [LegacyNamespace]) named for the interface, with the length of its shortest constructor
   and a non-writable prototype whose class string is the interface's name; an operation is
   an enumerable method named for its identifier with the length of its shortest overload; an
   attribute is an enumerable accessor whose getter is "get <name>" of length 0 and whose
   setter, which only a writable, [Replaceable], [PutForwards] or [LegacyLenientSetter]
   attribute has, is "set <name>" of length 1; each of those functions has the shape of a
   built-in function (not a constructor, no "prototype", %Function.prototype% its prototype);
   a constant is a non-writable, non-configurable property of both the interface object and
   its prototype; [Unscopable] members are listed in the prototype's @@unscopables. The members of the realm's own [Global] interface are the
   global object's own properties.

   Behaviour stays where it is: a member the IDL names but the bindings lack stays missing, and
   one defined where the IDL has none, or in another form (an attribute as a data property, a
   member on the wrong object, a prototype chain the IDL does not give), is left as it is and
   counted in the report. Constants and class strings are data the IDL itself gives, so they
   are defined whether or not the bindings did. */
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <nocturne.h>
#include "js_webidl.h"

enum { WEBIDL_CONST, WEBIDL_ATTRIBUTE, WEBIDL_OPERATION };
enum { /* members */
    WEBIDL_STATIC = 1 << 0, WEBIDL_READONLY = 1 << 1, WEBIDL_UNFORGEABLE = 1 << 2, WEBIDL_REPLACEABLE = 1 << 3,
    WEBIDL_PUT_FORWARDS = 1 << 4, WEBIDL_LENIENT_SETTER = 1 << 5, WEBIDL_UNSCOPABLE = 1 << 6,
    WEBIDL_LENIENT_THIS = 1 << 7, WEBIDL_SECURE = 1 << 8, WEBIDL_ISOLATED = 1 << 9, WEBIDL_BOOLEAN = 1 << 10,
    WEBIDL_ITERATOR = 1 << 11, WEBIDL_ASYNC_ITERATOR = 1 << 12, WEBIDL_ARRAY_ITERATOR = 1 << 13,
    WEBIDL_SAME_AS_VALUES = 1 << 14,
};
enum { /* interfaces */
    WEBIDL_IS_NAMESPACE = 1 << 0, WEBIDL_IS_CALLBACK = 1 << 1, WEBIDL_IS_GLOBAL = 1 << 2, WEBIDL_IS_SECURE = 1 << 3,
    WEBIDL_IS_ISOLATED = 1 << 4, WEBIDL_NO_INTERFACE_OBJECT = 1 << 5, WEBIDL_CONSTRUCTOR = 1 << 6,
};
struct webidl_member {
    const char *name;
    uint8_t kind;
    uint16_t flags;
    uint8_t length;   /* an operation's shortest overload */
    uint16_t exposed; /* WEBIDL_GLOBAL_* */
    double value;     /* a constant's */
};
struct webidl_interface {
    const char *name;
    int16_t parent; /* index in webidl_interfaces, -1 for none */
    uint16_t flags;
    uint16_t exposed;
    uint8_t length; /* the shortest constructor's */
    uint16_t first, count;
    const char *aliases;   /* [LegacyWindowAlias] names, comma-separated */
    const char *factory;   /* [LegacyFactoryFunction] */
    uint8_t factory_length;
    const char *namespace; /* [LegacyNamespace] */
};
#include "webidl_data.inc"

enum role { ROLE_VALUE, ROLE_GET, ROLE_SET };
/* A function this pass has named: one function object serving two members of different names
   gets a function of its own for the second, which calls the first. shaped is the native
   function that stands for it when it lacked the built-in shape (JS_UNDEFINED otherwise), so
   every holder of the same member gets the same one. */
struct named { void *function; const struct webidl_member *member; enum role role; JSValue shaped; };
struct shaper {
    JSContext *ctx;
    JSValue global;
    unsigned bit;            /* the realm's WEBIDL_GLOBAL_* */
    const char *global_name; /* its [Global] interface */
    JSAtom name, length, prototype, constructor, iterator, async_iterator, to_string_tag, unscopables;
    JSValue object_prototype, function_prototype, error_prototype, array_prototype;
    struct named *named;
    size_t named_count, named_capacity;
    struct web_js_webidl_report report;
};
static struct web_js_webidl_report last_report;

static void forget_exception(JSContext *ctx) {
    JSValue e = JS_GetException(ctx);
    JS_FreeValue(ctx, e);
}

static void note(char *list, size_t size, const char *owner, const char *member) {
    size_t used = strlen(list), room = size - used;
    if (room > 1)
        snprintf(list + used, room, "%s%s%s%s", used ? ", " : "", owner, member ? "." : "", member ? member : "");
}

static void misplaced(struct shaper *s, const char *owner, const char *member) {
    s->report.misplaced++;
    note(s->report.misplaced_names, sizeof s->report.misplaced_names, owner, member);
}

static const struct webidl_interface *find_interface(const char *name) {
    size_t low = 0, high = WEBIDL_INTERFACE_COUNT;
    while (low < high) {
        size_t mid = (low + high) / 2;
        int c = strcmp(webidl_interfaces[mid].name, name);
        if (!c) return &webidl_interfaces[mid];
        if (c < 0) low = mid + 1;
        else high = mid;
    }
    return NULL;
}

static size_t hash_pointer(void *p, size_t mask) {
    uintptr_t v = (uintptr_t)p;
    v ^= v >> 17;
    v *= (uintptr_t)0x9E3779B97F4A7C15ull;
    return (size_t)(v >> 7) & mask;
}

/* The member a function was named for earlier in this pass, or NULL, after which it is named
   for this one. */
static const struct named *remember(struct shaper *s, void *function, const struct webidl_member *m, enum role role) {
    if (2 * (s->named_count + 1) > s->named_capacity) {
        size_t capacity = s->named_capacity ? 2 * s->named_capacity : 4096;
        struct named *table = calloc(capacity, sizeof *table);
        if (!table) return NULL;
        for (size_t i = 0; i < s->named_capacity; i++)
            if (s->named[i].function) {
                size_t at = hash_pointer(s->named[i].function, capacity - 1);
                while (table[at].function) at = (at + 1) & (capacity - 1);
                table[at] = s->named[i];
            }
        free(s->named);
        s->named = table;
        s->named_capacity = capacity;
    }
    size_t at = hash_pointer(function, s->named_capacity - 1);
    while (s->named[at].function) {
        if (s->named[at].function == function) return &s->named[at];
        at = (at + 1) & (s->named_capacity - 1);
    }
    s->named[at] = (struct named){function, m, role, JS_UNDEFINED};
    s->named_count++;
    return NULL;
}

static struct named *find_named(struct shaper *s, void *function) {
    if (!s->named_capacity) return NULL;
    size_t at = hash_pointer(function, s->named_capacity - 1);
    while (s->named[at].function) {
        if (s->named[at].function == function) return &s->named[at];
        at = (at + 1) & (s->named_capacity - 1);
    }
    return NULL;
}

static JSValue delegate(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv, int magic, JSValue *data) {
    (void)magic;
    return JS_Call(ctx, data[0], this_val, argc, argv);
}

/* Web IDL makes an operation, and an attribute's getter and setter, with CreateBuiltinFunction:
   not a constructor, no own "prototype", and %Function.prototype% as its [[Prototype]]. A
   binding written as an ordinary function (constructible, with a prototype) is called through a
   native function of that shape; an async or generator method keeps its body and gets the
   [[Prototype]]. Whether f needs the native function, after giving it the [[Prototype]]. */
static bool builtin_shape(struct shaper *s, JSValueConst f) {
    JSContext *ctx = s->ctx;
    int own = JS_GetOwnProperty(ctx, NULL, f, s->prototype);
    if (own < 0) { forget_exception(ctx); own = 0; }
    if (JS_IsConstructor(ctx, f) || own) return true;
    JSValue proto = JS_GetPrototype(ctx, f);
    if (JS_IsException(proto)) { forget_exception(ctx); return false; }
    if (JS_VALUE_GET_PTR(proto) != JS_VALUE_GET_PTR(s->function_prototype)) {
        if (JS_SetPrototype(ctx, f, s->function_prototype) < 0) forget_exception(ctx);
        else s->report.builtin_reprototyped++;
    }
    JS_FreeValue(ctx, proto);
    return false;
}

/* function (owned) as the member's operation, getter or setter: named, of the IDL's length, and
   of the built-in shape. */
static JSValue named_function(struct shaper *s, JSValue function, const struct webidl_member *m, enum role role, int length) {
    JSContext *ctx = s->ctx;
    void *key = JS_VALUE_GET_PTR(function);
    const struct named *earlier = remember(s, key, m, role);
    if (earlier && (earlier->role != role || strcmp(earlier->member->name, m->name)) &&
        !(m->flags & WEBIDL_SAME_AS_VALUES)) {
        JSValue own = JS_NewCFunctionData(ctx, delegate, length, 0, 1, &function);
        JS_FreeValue(ctx, function);
        if (JS_IsException(own)) return own;
        function = own;
        s->report.wrapped++;
    } else if (earlier) {
        if (JS_IsUndefined(earlier->shaped)) return function; /* named already */
        JS_FreeValue(ctx, function);
        return JS_DupValue(ctx, earlier->shaped);
    } else if (builtin_shape(s, function)) {
        JSValue own = JS_NewCFunctionData(ctx, delegate, length, 0, 1, &function);
        JS_FreeValue(ctx, function);
        if (JS_IsException(own)) return own;
        function = own;
        struct named *entry = find_named(s, key);
        if (entry) entry->shaped = JS_DupValue(ctx, function);
        s->report.builtin_wrapped++;
    }
    char text[96];
    snprintf(text, sizeof text, "%s%s", role == ROLE_GET ? "get " : role == ROLE_SET ? "set " : "", m->name);
    JS_DefinePropertyValue(ctx, function, s->name, JS_NewString(ctx, text), JS_PROP_CONFIGURABLE);
    JS_DefinePropertyValue(ctx, function, s->length, JS_NewInt32(ctx, length), JS_PROP_CONFIGURABLE);
    if (JS_HasException(ctx)) forget_exception(ctx);
    return function;
}

static JSValue constant_value(JSContext *ctx, const struct webidl_member *m) {
    if (m->flags & WEBIDL_BOOLEAN) return JS_NewBool(ctx, m->value != 0);
    if (m->value == floor(m->value) && fabs(m->value) <= 2147483647.0) return JS_NewInt32(ctx, (int32_t)m->value);
    return JS_NewFloat64(ctx, m->value);
}

/* A symbol-keyed method of holder (@@iterator, @@asyncIterator): writable, configurable, not
   enumerable. */
static void shape_symbol_method(struct shaper *s, JSValueConst holder, JSAtom atom) {
    JSPropertyDescriptor d;
    int has = JS_GetOwnProperty(s->ctx, &d, holder, atom);
    if (has < 0) { forget_exception(s->ctx); return; }
    if (!has) return;
    if (!(d.flags & JS_PROP_GETSET) && JS_IsFunction(s->ctx, d.value))
        JS_DefineProperty(s->ctx, holder, atom, JS_UNDEFINED, JS_UNDEFINED, JS_UNDEFINED,
                          JS_PROP_HAS_WRITABLE | JS_PROP_WRITABLE | JS_PROP_HAS_ENUMERABLE |
                          JS_PROP_HAS_CONFIGURABLE | JS_PROP_CONFIGURABLE);
    JS_FreeValue(s->ctx, d.value); JS_FreeValue(s->ctx, d.getter); JS_FreeValue(s->ctx, d.setter);
}

static void shape_member(struct shaper *s, const struct webidl_interface *i, JSValueConst holder,
                         const struct webidl_member *m) {
    JSContext *ctx = s->ctx;
    JSAtom atom = JS_NewAtom(ctx, m->name);
    if (atom == JS_ATOM_NULL) { forget_exception(ctx); return; }
    JSPropertyDescriptor d;
    int has = JS_GetOwnProperty(ctx, &d, holder, atom);
    if (has < 0) { forget_exception(ctx); JS_FreeAtom(ctx, atom); return; }
    /* [LegacyUnforgeable] members are non-configurable own properties of each instance; the
       bindings keep theirs where they are, so this layer locks nothing but constants. */
    if (m->kind == WEBIDL_CONST) {
        JS_DefinePropertyValue(ctx, holder, atom, constant_value(ctx, m), JS_PROP_ENUMERABLE);
        if (JS_HasException(ctx)) forget_exception(ctx);
        s->report.members++;
    } else if (m->kind == WEBIDL_OPERATION && (m->flags & WEBIDL_ARRAY_ITERATOR)) {
        /* A value iterator's entries, keys, values and forEach, and its @@iterator, are the
           %Array.prototype% functions themselves (Web IDL "iterable declarations"): they read the
           object's length and indices, which the interface's indexed property getter answers, so
           no binding of its own stands in for them. */
        JSValue fn = JS_GetPropertyStr(ctx, s->array_prototype, m->name);
        if (JS_IsException(fn) || JS_DefinePropertyValue(ctx, holder, atom, JS_DupValue(ctx, fn), JS_PROP_C_W_E) < 0 ||
            ((m->flags & WEBIDL_ITERATOR) &&
             JS_DefinePropertyValue(ctx, holder, s->iterator, JS_DupValue(ctx, fn), JS_PROP_WRITABLE | JS_PROP_CONFIGURABLE) < 0))
            forget_exception(ctx);
        else s->report.members++;
        JS_FreeValue(ctx, fn);
    } else if (has && m->kind == WEBIDL_OPERATION) {
        if (!(d.flags & JS_PROP_GETSET) && JS_IsFunction(ctx, d.value)) {
            JSValue fn = named_function(s, JS_DupValue(ctx, d.value), m, ROLE_VALUE, m->length);
            if (JS_IsException(fn)) forget_exception(ctx);
            else JS_DefinePropertyValue(ctx, holder, atom, fn, JS_PROP_C_W_E);
            if (m->flags & WEBIDL_ITERATOR) shape_symbol_method(s, holder, s->iterator);
            if (m->flags & WEBIDL_ASYNC_ITERATOR) shape_symbol_method(s, holder, s->async_iterator);
            s->report.members++;
        } else {
            misplaced(s, i->name, m->name);
        }
    } else if (has && m->kind == WEBIDL_ATTRIBUTE) {
        if (d.flags & JS_PROP_GETSET) {
            bool settable = !(m->flags & WEBIDL_READONLY) ||
                            (m->flags & (WEBIDL_REPLACEABLE | WEBIDL_PUT_FORWARDS | WEBIDL_LENIENT_SETTER));
            JSValue get = JS_IsUndefined(d.getter) ? JS_UNDEFINED : named_function(s, JS_DupValue(ctx, d.getter), m, ROLE_GET, 0);
            JSValue set = JS_UNDEFINED;
            if (!JS_IsUndefined(d.setter)) {
                if (settable) set = named_function(s, JS_DupValue(ctx, d.setter), m, ROLE_SET, 1);
                else {
                    s->report.setters_dropped++;
                    note(s->report.dropped_names, sizeof s->report.dropped_names, i->name, m->name);
                }
            }
            if (JS_IsException(get) || JS_IsException(set)) forget_exception(ctx);
            else JS_DefineProperty(ctx, holder, atom, JS_UNDEFINED, get, set,
                                   JS_PROP_HAS_GET | JS_PROP_HAS_SET | JS_PROP_HAS_ENUMERABLE | JS_PROP_ENUMERABLE |
                                   JS_PROP_HAS_CONFIGURABLE | JS_PROP_CONFIGURABLE);
            JS_FreeValue(ctx, get); JS_FreeValue(ctx, set);
            if (JS_HasException(ctx)) forget_exception(ctx);
            s->report.members++;
        } else {
            misplaced(s, i->name, m->name);
        }
    }
    if (has) { JS_FreeValue(ctx, d.value); JS_FreeValue(ctx, d.getter); JS_FreeValue(ctx, d.setter); }
    JS_FreeAtom(ctx, atom);
}

/* A data property of holder: writable, configurable, not enumerable (an interface object or
   namespace on its global). Its value, or JS_UNDEFINED when holder has none (or an accessor). */
static JSValue global_property(struct shaper *s, JSValueConst holder, const char *name) {
    JSContext *ctx = s->ctx;
    JSAtom atom = JS_NewAtom(ctx, name);
    JSPropertyDescriptor d;
    int has = JS_GetOwnProperty(ctx, &d, holder, atom);
    JSValue value = JS_UNDEFINED;
    if (has < 0) forget_exception(ctx);
    else if (has) {
        if (d.flags & JS_PROP_GETSET)
            JS_DefineProperty(ctx, holder, atom, JS_UNDEFINED, JS_UNDEFINED, JS_UNDEFINED, JS_PROP_HAS_ENUMERABLE);
        else {
            JS_DefineProperty(ctx, holder, atom, JS_UNDEFINED, JS_UNDEFINED, JS_UNDEFINED,
                              JS_PROP_HAS_ENUMERABLE | JS_PROP_HAS_WRITABLE | JS_PROP_WRITABLE |
                              JS_PROP_HAS_CONFIGURABLE | JS_PROP_CONFIGURABLE);
            value = JS_DupValue(ctx, d.value);
        }
        if (JS_HasException(ctx)) forget_exception(ctx);
        JS_FreeValue(ctx, d.value); JS_FreeValue(ctx, d.getter); JS_FreeValue(ctx, d.setter);
    }
    JS_FreeAtom(ctx, atom);
    return value;
}

static bool exposed(const struct shaper *s, const struct webidl_member *m) { return m->exposed & s->bit; }

static void unscopables(struct shaper *s, const struct webidl_interface *i, JSValueConst prototype) {
    JSContext *ctx = s->ctx;
    JSValue list = JS_UNDEFINED;
    for (unsigned k = 0; k < i->count; k++) {
        const struct webidl_member *m = &webidl_members[i->first + k];
        if (!(m->flags & WEBIDL_UNSCOPABLE) || !exposed(s, m)) continue;
        if (JS_IsUndefined(list)) list = JS_NewObjectProto(ctx, JS_NULL);
        if (JS_IsException(list)) { forget_exception(ctx); return; }
        JS_SetPropertyStr(ctx, list, m->name, JS_TRUE);
    }
    if (!JS_IsUndefined(list))
        JS_DefinePropertyValue(ctx, prototype, s->unscopables, list, JS_PROP_CONFIGURABLE);
    if (JS_HasException(ctx)) forget_exception(ctx);
}

/* The interface object c and its prototype: name, length, prototype, constructor, class string,
   constants, members and @@unscopables. */
static void shape_interface_object(struct shaper *s, const struct webidl_interface *i, JSValueConst c) {
    JSContext *ctx = s->ctx;
    bool global = (i->flags & WEBIDL_IS_GLOBAL) && !strcmp(i->name, s->global_name);
    JS_DefinePropertyValue(ctx, c, s->name, JS_NewString(ctx, i->name), JS_PROP_CONFIGURABLE);
    JS_DefinePropertyValue(ctx, c, s->length, JS_NewInt32(ctx, i->length), JS_PROP_CONFIGURABLE);
    JSValue prototype = JS_GetProperty(ctx, c, s->prototype);
    if (JS_IsObject(prototype)) {
        JS_DefineProperty(ctx, c, s->prototype, JS_UNDEFINED, JS_UNDEFINED, JS_UNDEFINED,
                          JS_PROP_HAS_WRITABLE | JS_PROP_HAS_ENUMERABLE | JS_PROP_HAS_CONFIGURABLE);
        JS_DefinePropertyValue(ctx, prototype, s->constructor, JS_DupValue(ctx, c), JS_PROP_WRITABLE | JS_PROP_CONFIGURABLE);
        JS_DefinePropertyValue(ctx, prototype, s->to_string_tag, JS_NewString(ctx, i->name), JS_PROP_CONFIGURABLE);
        if (JS_HasException(ctx)) forget_exception(ctx);
        /* The prototype chain the IDL gives: the parent's interface object and prototype. */
        JSValue parent_c = JS_UNDEFINED, parent_p = JS_UNDEFINED;
        if (i->parent >= 0) {
            parent_c = JS_GetPropertyStr(ctx, s->global, webidl_interfaces[i->parent].name);
            parent_p = JS_IsObject(parent_c) ? JS_GetProperty(ctx, parent_c, s->prototype) : JS_UNDEFINED;
        } else {
            parent_c = JS_DupValue(ctx, s->function_prototype);
            parent_p = JS_DupValue(ctx, !strcmp(i->name, "DOMException") ? s->error_prototype : s->object_prototype);
        }
        if (JS_HasException(ctx)) forget_exception(ctx);
        JSValue have_c = JS_GetPrototype(ctx, c), have_p = JS_GetPrototype(ctx, prototype);
        if (JS_IsObject(parent_p) && (JS_VALUE_GET_PTR(have_p) != JS_VALUE_GET_PTR(parent_p) ||
                                      JS_VALUE_GET_PTR(have_c) != JS_VALUE_GET_PTR(parent_c)))
            misplaced(s, i->name, NULL);
        JS_FreeValue(ctx, have_c); JS_FreeValue(ctx, have_p); JS_FreeValue(ctx, parent_c); JS_FreeValue(ctx, parent_p);
        for (unsigned k = 0; k < i->count; k++) {
            const struct webidl_member *m = &webidl_members[i->first + k];
            if (!exposed(s, m)) continue;
            if (m->kind == WEBIDL_CONST) {
                shape_member(s, i, c, m);
                shape_member(s, i, prototype, m);
            } else {
                shape_member(s, i, m->flags & WEBIDL_STATIC ? c : global ? s->global : prototype, m);
            }
        }
        unscopables(s, i, prototype);
    }
    if (JS_HasException(ctx)) forget_exception(ctx);
    JS_FreeValue(ctx, prototype);
    s->report.interfaces++;
}

static void shape_namespace(struct shaper *s, const struct webidl_interface *i, JSValueConst object) {
    JSContext *ctx = s->ctx;
    JS_DefinePropertyValue(ctx, object, s->to_string_tag, JS_NewString(ctx, i->name), JS_PROP_CONFIGURABLE);
    if (JS_HasException(ctx)) forget_exception(ctx);
    for (unsigned k = 0; k < i->count; k++) {
        const struct webidl_member *m = &webidl_members[i->first + k];
        if (exposed(s, m)) shape_member(s, i, object, m);
    }
    s->report.interfaces++;
}

/* A legacy callback interface object (NodeFilter): its constants. */
static void shape_callback(struct shaper *s, const struct webidl_interface *i, JSValueConst object) {
    for (unsigned k = 0; k < i->count; k++) {
        const struct webidl_member *m = &webidl_members[i->first + k];
        if (m->kind == WEBIDL_CONST && exposed(s, m)) shape_member(s, i, object, m);
    }
    s->report.interfaces++;
}

static void shape_global_names(struct shaper *s, const char *list, JSValueConst holder, int length) {
    while (list && *list) {
        const char *comma = strchr(list, ',');
        size_t n = comma ? (size_t)(comma - list) : strlen(list);
        char name[64];
        if (n < sizeof name) {
            memcpy(name, list, n);
            name[n] = 0;
            JSValue value = global_property(s, holder, name);
            if (length >= 0 && JS_IsFunction(s->ctx, value)) {
                JS_DefinePropertyValue(s->ctx, value, s->name, JS_NewString(s->ctx, name), JS_PROP_CONFIGURABLE);
                JS_DefinePropertyValue(s->ctx, value, s->length, JS_NewInt32(s->ctx, length), JS_PROP_CONFIGURABLE);
                if (JS_HasException(s->ctx)) forget_exception(s->ctx);
            }
            JS_FreeValue(s->ctx, value);
        }
        list = comma ? comma + 1 : NULL;
    }
}

static void shape(struct shaper *s, const struct webidl_interface *i, bool global_property_too) {
    JSContext *ctx = s->ctx;
    if (!(i->exposed & s->bit) || (i->flags & WEBIDL_NO_INTERFACE_OBJECT)) return;
    JSValue holder = i->namespace ? JS_GetPropertyStr(ctx, s->global, i->namespace) : JS_DupValue(ctx, s->global);
    if (!JS_IsObject(holder)) {
        if (JS_IsException(holder)) forget_exception(ctx);
        JS_FreeValue(ctx, holder);
        return;
    }
    JSValue value;
    if (global_property_too) value = global_property(s, holder, i->name);
    else value = JS_GetPropertyStr(ctx, holder, i->name);
    if (JS_IsException(value)) { forget_exception(ctx); value = JS_UNDEFINED; }
    if (i->flags & WEBIDL_IS_NAMESPACE) {
        if (JS_IsObject(value)) shape_namespace(s, i, value);
    } else if (i->flags & WEBIDL_IS_CALLBACK) {
        if (JS_IsObject(value)) shape_callback(s, i, value);
    } else if (JS_IsFunction(ctx, value)) {
        shape_interface_object(s, i, value);
        if (global_property_too) {
            shape_global_names(s, i->aliases, holder, -1);
            if (i->factory) shape_global_names(s, i->factory, holder, i->factory_length);
        }
    }
    JS_FreeValue(ctx, value);
    JS_FreeValue(ctx, holder);
}

static bool shaper_open(struct shaper *s, JSContext *ctx, const char *global) {
    memset(s, 0, sizeof *s);
    s->ctx = ctx;
    for (unsigned g = 0; g < WEBIDL_GLOBAL_COUNT; g++)
        if (!strcmp(webidl_global_names[g], global)) { s->bit = 1u << g; s->global_name = webidl_global_names[g]; }
    if (!s->bit) return false;
    s->global = JS_GetGlobalObject(ctx);
    s->name = JS_NewAtom(ctx, "name");
    s->length = JS_NewAtom(ctx, "length");
    s->prototype = JS_NewAtom(ctx, "prototype");
    s->constructor = JS_NewAtom(ctx, "constructor");
    JSValue symbol = JS_GetPropertyStr(ctx, s->global, "Symbol");
    JSValue v = JS_GetPropertyStr(ctx, symbol, "iterator");
    s->iterator = JS_ValueToAtom(ctx, v); JS_FreeValue(ctx, v);
    v = JS_GetPropertyStr(ctx, symbol, "asyncIterator");
    s->async_iterator = JS_ValueToAtom(ctx, v); JS_FreeValue(ctx, v);
    v = JS_GetPropertyStr(ctx, symbol, "toStringTag");
    s->to_string_tag = JS_ValueToAtom(ctx, v); JS_FreeValue(ctx, v);
    v = JS_GetPropertyStr(ctx, symbol, "unscopables");
    s->unscopables = JS_ValueToAtom(ctx, v); JS_FreeValue(ctx, v);
    JS_FreeValue(ctx, symbol);
    JSValue object = JS_GetPropertyStr(ctx, s->global, "Object");
    s->object_prototype = JS_GetProperty(ctx, object, s->prototype);
    s->function_prototype = JS_GetPrototype(ctx, object);
    JS_FreeValue(ctx, object);
    JSValue array = JS_GetPropertyStr(ctx, s->global, "Array");
    s->array_prototype = JS_GetProperty(ctx, array, s->prototype);
    JS_FreeValue(ctx, array);
    JSValue error = JS_GetPropertyStr(ctx, s->global, "Error");
    s->error_prototype = JS_GetProperty(ctx, error, s->prototype);
    JS_FreeValue(ctx, error);
    if (JS_HasException(ctx)) forget_exception(ctx);
    return true;
}

static void shaper_close(struct shaper *s) {
    JSContext *ctx = s->ctx;
    JSAtom atoms[] = {s->name, s->length, s->prototype, s->constructor, s->iterator, s->async_iterator, s->to_string_tag, s->unscopables};
    for (size_t k = 0; k < sizeof atoms / sizeof atoms[0]; k++) JS_FreeAtom(ctx, atoms[k]);
    JS_FreeValue(ctx, s->object_prototype); JS_FreeValue(ctx, s->function_prototype); JS_FreeValue(ctx, s->error_prototype); JS_FreeValue(ctx, s->array_prototype);
    JS_FreeValue(ctx, s->global);
    for (size_t k = 0; k < s->named_capacity; k++)
        if (s->named[k].function) JS_FreeValue(ctx, s->named[k].shaped);
    free(s->named);
}

static JSValue native_webidl(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
    (void)this_val;
    const char *global = argc ? JS_ToCString(ctx, argv[0]) : NULL;
    if (!global) return JS_EXCEPTION;
    uint64_t start = uptime_ms();
    struct shaper s;
    bool known = shaper_open(&s, ctx, global);
    JS_FreeCString(ctx, global);
    if (!known) return JS_ThrowTypeError(ctx, "Unknown Web IDL global");
    for (unsigned k = 0; k < WEBIDL_INTERFACE_COUNT; k++) shape(&s, &webidl_interfaces[k], true);
    s.report.milliseconds = (unsigned)(uptime_ms() - start);
    last_report = s.report;
    shaper_close(&s);
    return JS_UNDEFINED;
}

void web_js_webidl_init(JSContext *ctx, JSValueConst host) {
    JS_SetPropertyStr(ctx, host, "webidl", JS_NewCFunction(ctx, native_webidl, "webidl", 1));
}

void web_js_webidl_shape_exports(JSContext *ctx, JSValueConst exports, const char *global) {
    struct shaper s;
    if (!shaper_open(&s, ctx, global)) return;
    JSPropertyEnum *names = NULL;
    uint32_t count = 0;
    if (JS_GetOwnPropertyNames(ctx, &names, &count, exports, JS_GPN_STRING_MASK) == 0) {
        for (uint32_t k = 0; k < count; k++) {
            const char *name = JS_AtomToCString(ctx, names[k].atom);
            const struct webidl_interface *i = name ? find_interface(name) : NULL;
            if (i && (i->exposed & s.bit) && !(i->flags & (WEBIDL_IS_NAMESPACE | WEBIDL_IS_CALLBACK))) {
                JSValue c = JS_GetProperty(ctx, exports, names[k].atom);
                if (JS_IsFunction(ctx, c)) shape_interface_object(&s, i, c);
                else if (JS_IsException(c)) forget_exception(ctx);
                JS_FreeValue(ctx, c);
            }
            JS_FreeCString(ctx, name);
        }
        JS_FreePropertyEnum(ctx, names, count);
    } else {
        forget_exception(ctx);
    }
    shaper_close(&s);
}

const struct web_js_webidl_report *web_js_webidl_last(void) { return &last_report; }
