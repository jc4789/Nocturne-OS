#include "js_crypto.h"
#include <bearssl_hash.h>
#include <errno.h>
#include <fcntl.h>
#include <string.h>
#include <unistd.h>

static void crypto_buffer_free(JSRuntime *rt, void *opaque, void *ptr) {
    (void)opaque;
    js_free_rt(rt, ptr);
}

static JSValue crypto_random(JSContext *ctx, JSValueConst self, int argc, JSValueConst *argv) {
    (void)self;
    uint64_t length;
    if (argc < 1) return JS_ThrowTypeError(ctx, "Random request requires a length");
    if (JS_ToIndex(ctx, &length, argv[0]) < 0) return JS_EXCEPTION;
    if (length > 65536) return JS_ThrowRangeError(ctx, "Random request exceeds 65536 bytes");
    if (!length) return JS_NewArrayBufferCopy(ctx, NULL, 0);
    uint8_t *bytes = js_malloc(ctx, (size_t)length);
    if (!bytes) return JS_EXCEPTION;
    int fd = open("/dev/random", O_RDONLY);
    if (fd < 0) {
        js_free(ctx, bytes);
        return JS_ThrowInternalError(ctx, "System random source is unavailable");
    }
    size_t done = 0;
    while (done < length) {
        ssize_t n = read(fd, bytes + done, (size_t)length - done);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0 || (size_t)n > (size_t)length - done) {
            close(fd);
            memset(bytes, 0, (size_t)length);
            js_free(ctx, bytes);
            return JS_ThrowInternalError(ctx, "System random source could not fill the request");
        }
        done += (size_t)n;
    }
    close(fd);
    JSValue result = JS_NewArrayBuffer(ctx, bytes, (size_t)length, crypto_buffer_free, NULL, 0);
    /* JS_NewArrayBuffer does not take ownership on allocation failure. */
    if (JS_IsException(result)) {
        memset(bytes, 0, (size_t)length);
        js_free(ctx, bytes);
    }
    return result;
}

static JSValue crypto_digest(JSContext *ctx, JSValueConst self, int argc, JSValueConst *argv) {
    (void)self;
    if (argc < 2) return JS_ThrowTypeError(ctx, "Digest requires algorithm and data");
    const char *name = JS_ToCString(ctx, argv[0]);
    if (!name) return JS_EXCEPTION;
    const br_hash_class *hash = NULL;
    if (!strcmp(name, "SHA-1")) hash = &br_sha1_vtable;
    else if (!strcmp(name, "SHA-256")) hash = &br_sha256_vtable;
    else if (!strcmp(name, "SHA-384")) hash = &br_sha384_vtable;
    else if (!strcmp(name, "SHA-512")) hash = &br_sha512_vtable;
    JS_FreeCString(ctx, name);
    if (!hash) return JS_ThrowTypeError(ctx, "Unsupported digest algorithm");
    size_t length;
    uint8_t *data = JS_GetArrayBuffer(ctx, &length, argv[1]);
    if (!data && JS_HasException(ctx)) return JS_EXCEPTION;
    br_hash_compat_context context;
    uint8_t output[64];
    hash->init(&context.vtable);
    hash->update(&context.vtable, data, length);
    hash->out(&context.vtable, output);
    size_t output_length = (hash->desc >> BR_HASHDESC_OUT_OFF) & BR_HASHDESC_OUT_MASK;
    return JS_NewArrayBufferCopy(ctx, output, output_length);
}

void web_js_crypto_init(JSContext *ctx, JSValue host) {
    static const JSCFunctionListEntry functions[] = {
        JS_CFUNC_DEF("cryptoRandom", 1, crypto_random),
        JS_CFUNC_DEF("cryptoDigest", 2, crypto_digest),
    };
    JS_SetPropertyFunctionList(ctx, host, functions, sizeof functions / sizeof functions[0]);
}
