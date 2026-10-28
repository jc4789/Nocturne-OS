/* The Compression Standard's streams on the host: the product's QuickJS, js_encoding.js,
 * js_streams.js (with the pinned Streams ponyfill) and js_compression.c over deflate.c and
 * the brotli library, running WPT's own compression/ files under WPT's testharness.js.
 *     compression_hosttest <wpt root> (<script> | -e <code>)...
 * The scripts run in order in one realm; then microtasks and the stand-in timers of
 * tests/compression_host.js run until nothing is left. Not the guest, not a browser
 * realm: fetch, Blob, postMessage and the like are stand-ins (compression_host.js). */
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "quickjs.h"
#include "../user/libc/web/js_compression.h"

static const char *wpt_root;

static void exception(JSContext *ctx) {
    JSValue e = JS_GetException(ctx), stack = JS_GetPropertyStr(ctx, e, "stack");
    const char *s = JS_ToCString(ctx, e), *t = JS_ToCString(ctx, stack);
    printf("EXCEPTION %s\n%s\n", s ? s : "?", t ? t : "");
    JS_FreeCString(ctx, s);
    JS_FreeCString(ctx, t);
    JS_FreeValue(ctx, stack);
    JS_FreeValue(ctx, e);
}

static char *slurp(const char *path, size_t *size) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    rewind(f);
    char *bytes = malloc((size_t)n + 1);
    if (bytes && fread(bytes, 1, (size_t)n, f) != (size_t)n) { free(bytes); bytes = NULL; }
    fclose(f);
    if (bytes) { bytes[n] = 0; *size = (size_t)n; }
    return bytes;
}

static JSValue print(JSContext *ctx, JSValueConst self, int argc, JSValueConst *argv) {
    (void)self;
    for (int i = 0; i < argc; i++) {
        const char *s = JS_ToCString(ctx, argv[i]);
        printf(i ? " %s" : "%s", s ? s : "?");
        JS_FreeCString(ctx, s);
    }
    printf("\n");
    fflush(stdout);
    return JS_UNDEFINED;
}

/* host.readFile(path under the WPT root): an ArrayBuffer, or null */
static JSValue read_file(JSContext *ctx, JSValueConst self, int argc, JSValueConst *argv) {
    (void)self;
    const char *name = argc ? JS_ToCString(ctx, argv[0]) : NULL;
    if (!name) return JS_EXCEPTION;
    char path[4096];
    snprintf(path, sizeof path, "%s/%s", wpt_root, name[0] == '/' ? name + 1 : name);
    JS_FreeCString(ctx, name);
    size_t size = 0;
    char *bytes = slurp(path, &size);
    if (!bytes) return JS_NULL;
    JSValue result = JS_NewArrayBufferCopy(ctx, (const uint8_t *)bytes, size);
    free(bytes);
    return result;
}

static bool run(JSContext *ctx, const char *source, size_t size, const char *name) {
    JSValue v = JS_Eval(ctx, source, size, name, JS_EVAL_TYPE_GLOBAL);
    bool ok = !JS_IsException(v);
    if (!ok) exception(ctx);
    JS_FreeValue(ctx, v);
    return ok;
}

int main(int argc, char **argv) {
    if (argc < 3) return 2;
    wpt_root = argv[1];
    JSRuntime *rt = JS_NewRuntime();
    JS_SetMaxStackSize(rt, 4u << 20);
    JSContext *ctx = JS_NewContext(rt);
    JSValue global = JS_GetGlobalObject(ctx), host = JS_NewObject(ctx);
    web_js_compression_init(ctx, host);
    JS_SetPropertyStr(ctx, host, "readFile", JS_NewCFunction(ctx, read_file, "readFile", 1));
    JS_SetPropertyStr(ctx, global, "host", host);
    JS_SetPropertyStr(ctx, global, "print", JS_NewCFunction(ctx, print, "print", 1));
    JS_FreeValue(ctx, global);
    for (int i = 2; i < argc; i++) {
        if (!strcmp(argv[i], "-e") && i + 1 < argc) {  /* -e CODE: a script given inline */
            i++;
            if (!run(ctx, argv[i], strlen(argv[i]), "inline")) return 2;
            continue;
        }
        size_t size = 0;
        char *source = slurp(argv[i], &size);
        if (!source) { printf("MISSING %s\n", argv[i]); return 2; }
        bool ok = run(ctx, source, size, argv[i]);
        free(source);
        if (!ok) return 2;
    }
    for (;;) {
        JSContext *job;
        int n;
        while ((n = JS_ExecutePendingJob(rt, &job)) > 0) {}
        if (n < 0) exception(job);
        JSValue ran = JS_Eval(ctx, "__runTimer()", 12, "timers", JS_EVAL_TYPE_GLOBAL);
        if (JS_IsException(ran)) { exception(ctx); break; }
        bool more = JS_ToBool(ctx, ran) > 0;
        JS_FreeValue(ctx, ran);
        if (!more && !JS_IsJobPending(rt)) break;
    }
    JS_FreeContext(ctx);
    JS_FreeRuntime(rt);
    return 0;
}
