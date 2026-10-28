/* Cache Storage: the native half of js_cache_storage.js (js_cachestorage.h). It turns the realm's
 * requests into the broker's commands (webcache.h's fields) and their results back into values;
 * Query Cache and Batch Cache Operations run in the broker, on what the disk has. */
#include "js_cachestorage.h"
#include <webcache.h>
#include <stdlib.h>
#include <string.h>

static const struct web_idb_realm *owner;

static bool cache_origin_operation(unsigned operation) {
    return operation != WEB_CACHE_WRITE && operation != WEB_CACHE_DISCARD && operation != WEB_CACHE_READ &&
        operation != WEB_CACHE_CLOSE;
}
static bool element(JSContext *ctx, JSValueConst list, uint32_t index, JSValue *out) {
    *out = JS_GetPropertyUint32(ctx, list, index);
    return !JS_IsException(*out);
}
static bool length_of(JSContext *ctx, JSValueConst list, uint32_t *n) {
    JSValue v = JS_GetPropertyStr(ctx, list, "length");
    int r = JS_IsException(v) ? -1 : JS_ToUint32(ctx, n, v);
    JS_FreeValue(ctx, v);
    return r >= 0;
}
static bool put_string(JSContext *ctx, struct webcache_out *o, JSValueConst v) {
    size_t n;
    const char *s = JS_ToCStringLen(ctx, &n, v);
    if (!s) return false;
    webcache_put_field(o, s, n);
    JS_FreeCString(ctx, s);
    return true;
}
static bool put_item(JSContext *ctx, struct webcache_out *o, JSValueConst list, uint32_t index) {
    JSValue v;
    if (!element(ctx, list, index, &v)) return false;
    bool ok = put_string(ctx, o, v);
    JS_FreeValue(ctx, v);
    return ok;
}
static bool put_number(JSContext *ctx, JSValueConst list, uint32_t index, uint32_t *out) {
    JSValue v;
    if (!element(ctx, list, index, &v)) return false;
    int r = JS_ToUint32(ctx, out, v);
    JS_FreeValue(ctx, v);
    return r >= 0;
}
/* A header list given as [name, value, name, value, ...]. */
static bool put_headers(JSContext *ctx, struct webcache_out *o, JSValueConst list, uint32_t index) {
    JSValue headers;
    uint32_t n;
    if (!element(ctx, list, index, &headers)) return false;
    bool ok = length_of(ctx, headers, &n) && !(n & 1);
    if (ok) webcache_put_u32(o, n / 2);
    for (uint32_t i = 0; ok && i < n; i++) ok = put_item(ctx, o, headers, i);
    if (!ok && !JS_HasException(ctx)) JS_ThrowTypeError(ctx, "Invalid cache header list");
    JS_FreeValue(ctx, headers);
    return ok;
}
/* A request query [url, method, headers]. */
static bool put_query(JSContext *ctx, struct webcache_out *o, JSValueConst q) {
    return put_item(ctx, o, q, 0) && put_item(ctx, o, q, 1) && put_headers(ctx, o, q, 2);
}
/* Each operation: [type, flags, url, method, headers, extra] and for a put
   [..., response headers, response extra, staged body token]. */
static bool put_batch(JSContext *ctx, struct webcache_out *o, JSValueConst ops) {
    uint32_t n;
    if (!length_of(ctx, ops, &n)) return false;
    webcache_put_u32(o, n);
    for (uint32_t i = 0; i < n; i++) {
        JSValue op;
        if (!element(ctx, ops, i, &op)) return false;
        uint32_t type = 0, flags = 0, token = 0;
        bool ok = put_number(ctx, op, 0, &type) && put_number(ctx, op, 1, &flags);
        if (ok) { webcache_put_u32(o, type); webcache_put_u32(o, flags); }
        ok = ok && put_item(ctx, o, op, 2) && put_item(ctx, o, op, 3) && put_headers(ctx, o, op, 4) && put_item(ctx, o, op, 5);
        if (ok && type == WEB_CACHE_PUT) {
            ok = put_headers(ctx, o, op, 6) && put_item(ctx, o, op, 7) && put_number(ctx, op, 8, &token);
            if (ok) { webcache_put_u32(o, token); webcache_put_u64(o, 0); }
        }
        JS_FreeValue(ctx, op);
        if (!ok) return false;
    }
    return true;
}

static void buffer_free(JSRuntime *runtime, void *opaque, void *bytes) { (void)runtime; (void)opaque; free(bytes); }
static JSValue take_buffer(JSContext *ctx, struct webidb_async_result *out) {
    JSValue value = JS_NewArrayBuffer(ctx, out->bytes, out->length, buffer_free, NULL, false);
    if (!JS_IsException(value)) { out->bytes = NULL; out->length = 0; }
    return value;
}
static JSValue get_string(JSContext *ctx, struct webcache_in *in) {
    const uint8_t *p;
    size_t n;
    if (!webcache_get_field(in, &p, &n)) return JS_ThrowInternalError(ctx, "Invalid Cache Storage result");
    return JS_NewStringLen(ctx, (const char *)p, n);
}
static int set(JSContext *ctx, JSValueConst list, uint32_t index, JSValue value) {
    if (JS_IsException(value)) return -1;
    return JS_DefinePropertyValueUint32(ctx, list, index, value, JS_PROP_C_W_E);
}
static JSValue get_headers(JSContext *ctx, struct webcache_in *in) {
    uint32_t count = webcache_get_u32(in);
    if (in->failed || count > (in->length - in->at) / 8) return JS_ThrowInternalError(ctx, "Invalid Cache Storage result");
    JSValue list = JS_NewArray(ctx);
    for (uint32_t i = 0; !JS_IsException(list) && i < count * 2; i++)
        if (set(ctx, list, i, get_string(ctx, in)) < 0) { JS_FreeValue(ctx, list); list = JS_EXCEPTION; }
    return list;
}
/* A matched response: [headers, extra, body token (0: none), body size]. */
static JSValue get_response(JSContext *ctx, struct webcache_in *in) {
    JSValue r = JS_NewArray(ctx);
    if (JS_IsException(r)) return r;
    if (set(ctx, r, 0, get_headers(ctx, in)) < 0 || set(ctx, r, 1, get_string(ctx, in)) < 0) goto failed;
    uint32_t token = webcache_get_u32(in);
    uint64_t size = webcache_get_u64(in);
    if (in->failed) { JS_ThrowInternalError(ctx, "Invalid Cache Storage result"); goto failed; }
    if (set(ctx, r, 2, JS_NewUint32(ctx, token)) < 0 || set(ctx, r, 3, JS_NewFloat64(ctx, (double)size)) < 0) goto failed;
    return r;
failed:
    JS_FreeValue(ctx, r);
    return JS_EXCEPTION;
}
/* A request: [url, method, headers, extra]. */
static JSValue get_request(JSContext *ctx, struct webcache_in *in) {
    JSValue r = JS_NewArray(ctx);
    if (JS_IsException(r)) return r;
    if (set(ctx, r, 0, get_string(ctx, in)) < 0 || set(ctx, r, 1, get_string(ctx, in)) < 0 ||
        set(ctx, r, 2, get_headers(ctx, in)) < 0 || set(ctx, r, 3, get_string(ctx, in)) < 0) {
        JS_FreeValue(ctx, r);
        return JS_EXCEPTION;
    }
    return r;
}
/* The bodies a match opened whose responses never reached the realm: the broker closes them (their
   results have no request waiting, js_indexeddb.c drops them). */
static void close_bodies(JSContext *ctx, const struct webidb_async_result *out) {
    struct web_idb *idb = owner ? owner->state(ctx) : NULL;
    if (!idb || !idb->broker) return;
    struct webcache_in in = {out->bytes, out->length, 0, false};
    for (size_t i = 0; i < out->count && !in.failed; i++) {
        uint32_t headers = webcache_get_u32(&in);
        const uint8_t *p;
        size_t n;
        for (uint32_t k = 0; k < headers * 2 && !in.failed; k++) webcache_get_field(&in, &p, &n);
        webcache_get_field(&in, &p, &n);
        uint32_t token = webcache_get_u32(&in);
        (void)webcache_get_u64(&in);
        if (in.failed || !token) continue;
        struct webidb_async_request close = {.operation = WEB_CACHE_CLOSE, .token = token};
        (void)webidb_async_submit(idb->broker, &close);
    }
}
static JSValue cache_completion(JSContext *ctx, struct webidb_async_result *out) {
    JSValue result = JS_NewArray(ctx);
    if (JS_IsException(result)) { if (out->operation == WEB_CACHE_MATCH) close_bodies(ctx, out); return result; }
    if (set(ctx, result, 0, JS_NewInt32(ctx, out->status)) < 0) goto failed;
    if (out->status != WEB_CACHE_OK) return result;
    struct webcache_in in = {out->bytes, out->length, 0, false};
    JSValue value = JS_UNDEFINED;
    switch (out->operation) {
    case WEB_CACHE_OPEN: value = JS_NewStringLen(ctx, (const char *)out->bytes, out->length); break;
    case WEB_CACHE_HAS: case WEB_CACHE_DELETE: value = JS_NewBool(ctx, out->count != 0); break;
    case WEB_CACHE_STAGE: value = JS_NewUint32(ctx, out->token); break;
    case WEB_CACHE_BATCH: value = JS_NewFloat64(ctx, (double)out->count); break;
    case WEB_CACHE_READ: value = out->length ? take_buffer(ctx, out) : JS_NULL; break;
    case WEB_CACHE_NAMES: case WEB_CACHE_MATCH: case WEB_CACHE_KEYS:
        if (out->count > UINT32_MAX) { value = JS_ThrowOutOfMemory(ctx); break; }
        value = JS_NewArray(ctx);
        for (uint32_t i = 0; !JS_IsException(value) && i < out->count; i++) {
            JSValue item = out->operation == WEB_CACHE_NAMES ? get_string(ctx, &in) :
                out->operation == WEB_CACHE_MATCH ? get_response(ctx, &in) : get_request(ctx, &in);
            if (set(ctx, value, i, item) < 0) { JS_FreeValue(ctx, value); value = JS_EXCEPTION; }
        }
        if (!JS_IsException(value) && in.at != in.length) { JS_FreeValue(ctx, value); value = JS_ThrowInternalError(ctx, "Invalid Cache Storage result"); }
        break;
    default: return result;
    }
    if (set(ctx, result, 1, value) < 0) goto failed;
    return result;
failed:
    if (out->operation == WEB_CACHE_MATCH) close_bodies(ctx, out);
    JS_FreeValue(ctx, result);
    return JS_EXCEPTION;
}

/* host.cacheStorage(operation, ...): see js_cache_storage.js for each operation's arguments. */
static JSValue cache_native(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
    (void)this_val;
    struct web_idb *idb = owner ? owner->state(ctx) : NULL;
    if (!idb) return JS_ThrowTypeError(ctx, "Retired Cache Storage realm");
    JSValue refused = owner->authority(ctx);
    if (JS_IsException(refused)) return refused;
    if (!argc) return JS_ThrowTypeError(ctx, "Cache Storage operation required");
    const char *op = JS_ToCString(ctx, argv[0]);
    if (!op) return JS_EXCEPTION;
    static const struct { const char *name; unsigned operation; int arguments; } operations[] = {
        {"open", WEB_CACHE_OPEN, 2}, {"has", WEB_CACHE_HAS, 2}, {"delete", WEB_CACHE_DELETE, 2},
        {"names", WEB_CACHE_NAMES, 1}, {"match", WEB_CACHE_MATCH, 5}, {"keys", WEB_CACHE_KEYS, 4},
        {"stage", WEB_CACHE_STAGE, 1}, {"write", WEB_CACHE_WRITE, 3}, {"discard", WEB_CACHE_DISCARD, 2},
        {"batch", WEB_CACHE_BATCH, 3}, {"read", WEB_CACHE_READ, 3}, {"close", WEB_CACHE_CLOSE, 2}};
    struct webidb_async_request request = {0};
    int arguments = 0;
    for (unsigned i = 0; i < sizeof operations / sizeof *operations; i++)
        if (!strcmp(op, operations[i].name)) { request.operation = operations[i].operation; arguments = operations[i].arguments; break; }
    JS_FreeCString(ctx, op);
    if (!request.operation) return JS_ThrowTypeError(ctx, "Invalid Cache Storage operation");
    if (argc < arguments) return JS_ThrowTypeError(ctx, "Cache Storage arguments required");
    JSValue result = JS_EXCEPTION;
    const char *origin = NULL, *name = NULL;
    struct webcache_out payload = {0};
    uint32_t flags = 0;
    bool ok = true;
    switch (request.operation) {
    case WEB_CACHE_OPEN: case WEB_CACHE_HAS: case WEB_CACHE_DELETE:
        ok = (name = JS_ToCStringLen(ctx, &request.name_length, argv[1])) != NULL;
        break;
    case WEB_CACHE_MATCH: case WEB_CACHE_KEYS: {
        /* (id or null, flags, [cache name], [query]) */
        int query = request.operation == WEB_CACHE_MATCH ? 4 : 3;
        if (!JS_IsNull(argv[1])) ok = (name = JS_ToCStringLen(ctx, &request.name_length, argv[1])) != NULL;
        ok = ok && JS_ToUint32(ctx, &flags, argv[2]) >= 0;
        flags &= WEB_CACHE_IGNORE_SEARCH | WEB_CACHE_IGNORE_METHOD | WEB_CACHE_IGNORE_VARY | WEB_CACHE_FIRST;
        if (ok && request.operation == WEB_CACHE_MATCH && !JS_IsUndefined(argv[3])) {
            flags |= WEB_CACHE_NAMED;
            ok = put_string(ctx, &payload, argv[3]);
        }
        if (ok && !JS_IsUndefined(argv[query])) { flags |= WEB_CACHE_QUERY; ok = put_query(ctx, &payload, argv[query]); }
        request.version = flags;
        break;
    }
    case WEB_CACHE_WRITE: {
        size_t n;
        ok = JS_ToUint32(ctx, &request.token, argv[1]) >= 0;
        uint8_t *bytes = ok ? JS_GetArrayBuffer(ctx, &n, argv[2]) : NULL;
        if (ok && !bytes && JS_HasException(ctx)) ok = false;
        if (ok) webcache_put(&payload, bytes, n);
        break;
    }
    case WEB_CACHE_DISCARD: case WEB_CACHE_CLOSE:
        ok = JS_ToUint32(ctx, &request.token, argv[1]) >= 0;
        break;
    case WEB_CACHE_READ: {
        uint32_t most = 0;
        ok = JS_ToUint32(ctx, &request.token, argv[1]) >= 0 && JS_ToUint32(ctx, &most, argv[2]) >= 0;
        request.version = most;
        break;
    }
    case WEB_CACHE_BATCH:
        ok = (name = JS_ToCStringLen(ctx, &request.name_length, argv[1])) != NULL && put_batch(ctx, &payload, argv[2]);
        break;
    default: break;
    }
    if (!ok) goto done;
    if (payload.failed) { JS_ThrowOutOfMemory(ctx); goto done; }
    request.name = name;
    request.bytes = payload.bytes;
    request.length = payload.length;
    int status = 0;
    if (cache_origin_operation(request.operation)) {
        origin = owner->origin(ctx);
        if (JS_HasException(ctx)) goto done;
        request.origin = origin;
        if (!origin) status = WEB_CACHE_SECURITY; /* an opaque origin has no storage key */
    }
    /* The conversions above may have run code: the realm may have gone meanwhile. */
    if (owner->state(ctx) != idb) { JS_ThrowTypeError(ctx, "Retired Cache Storage realm"); goto done; }
    refused = owner->authority(ctx);
    if (JS_IsException(refused)) goto done;
    result = web_idb_request(ctx, idb, &request, cache_completion, status);
done:
    free(payload.bytes);
    JS_FreeCString(ctx, name);
    JS_FreeCString(ctx, origin);
    return result;
}

/* host.cacheEmbedderPolicy(): the realm's embedder policy now (js_indexeddb.h). */
static JSValue cache_embedder_policy(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
    (void)this_val; (void)argc; (void)argv;
    return JS_NewInt32(ctx, owner && owner->embedder_policy && owner->state(ctx) ? owner->embedder_policy(ctx) : 0);
}
void web_cache_host_init(JSContext *ctx, JSValueConst host, const struct web_idb_realm *realm) {
    static const JSCFunctionListEntry functions[] = {
        JS_CFUNC_DEF("cacheStorage", 1, cache_native),
        JS_CFUNC_DEF("cacheEmbedderPolicy", 0, cache_embedder_policy),
    };
    owner = realm;
    JS_SetPropertyFunctionList(ctx, host, functions, sizeof functions / sizeof functions[0]);
}
