/* CSS Typed OM on the host: the product's QuickJS, js_typed_om.c over cssmath.c, cssprop.c,
 * color.c and css.c (Lexbor's CSS tokenizer), js_webidl.c's shapes, and js_geometry.js and
 * js_typed_om.js, running WPT's own css/css-typed-om files under WPT's testharness.js.
 *     typed_om_hosttest (<script> | -e <code>)...
 * The scripts run in order in one realm; then microtasks and the stand-in timers of
 * tests/typed_om_host.js run until nothing is left. Not the guest and not a document: an
 * element's inline style is a stand-in over css.c's declaration-block functions, the ones
 * js.c's inline style reads and writes (css_decl_find, css_decl_set, css_decl_serialize);
 * there is no cascade, no style sheet and no layout. */
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "quickjs.h"
#include "../user/libc/web/js_typed_om.h"
#include "../user/libc/web/js_webidl.h"

/* This file is built with the host's C library (its stdio); the engine's units with Nocturne's
   headers. What it calls of css.c, as webi.h declares it: */
typedef struct sbuf { char *p; size_t n, cap; } sbuf;
void sb_free(sbuf *b);
typedef bool css_decl_check(const char *name, const char *value, size_t n, sbuf *serialized);
char *css_preprocess(const char *css, size_t n, size_t *out_n);
bool css_decl_find(const char *css, size_t n, const char *property, const char **value, size_t *value_n, bool *important);
int css_decl_set(const char *css, size_t n, const char *property, const char *value, const char *priority,
                 css_decl_check *check, sbuf *out);
bool css_decl_serialize(const char *css, size_t n, css_decl_check *check, sbuf *out, sbuf *names);
void css_decl_names(const char *css, size_t n, sbuf *names);

/* js_webidl.c times its pass for the realm's setup trace, which the host does not print */
uint64_t uptime_ms(void) { return 0; }

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

/* host.declBlock(text): the text as js.c's inline style serializes it, [cssText, name...] */
static JSValue decl_block(JSContext *ctx, JSValueConst self, int argc, JSValueConst *argv) {
    (void)self;
    size_t n;
    const char *text = JS_ToCStringLen(ctx, &n, argc ? argv[0] : JS_UNDEFINED);
    if (!text) return JS_EXCEPTION;
    size_t clean_n;
    char *clean = css_preprocess(text, n, &clean_n);
    JS_FreeCString(ctx, text);
    sbuf block = {0}, names = {0};
    bool ok = clean && css_decl_serialize(clean, clean_n, NULL, &block, NULL);
    free(clean);
    if (!ok) { sb_free(&block); return JS_ThrowOutOfMemory(ctx); }
    css_decl_names(block.p ? block.p : "", block.n, &names);
    JSValue list = JS_NewArray(ctx);
    uint32_t i = 0;
    JS_SetPropertyUint32(ctx, list, i++, JS_NewStringLen(ctx, block.p ? block.p : "", block.n));
    for (size_t at = 0; at < names.n; at += strlen(names.p + at) + 1)
        JS_SetPropertyUint32(ctx, list, i++, JS_NewString(ctx, names.p + at));
    sb_free(&names);
    sb_free(&block);
    return list;
}

/* host.declGet(block, property): the declaration's value in a serialized block, or "" */
static JSValue decl_get(JSContext *ctx, JSValueConst self, int argc, JSValueConst *argv) {
    (void)self;
    size_t n;
    const char *block = JS_ToCStringLen(ctx, &n, argc > 0 ? argv[0] : JS_UNDEFINED);
    const char *property = JS_ToCString(ctx, argc > 1 ? argv[1] : JS_UNDEFINED);
    if (!block || !property) { JS_FreeCString(ctx, block); JS_FreeCString(ctx, property); return JS_EXCEPTION; }
    const char *v;
    size_t vn;
    bool important;
    bool found = css_decl_find(block, n, property, &v, &vn, &important);
    JSValue result = JS_NewStringLen(ctx, found ? v : "", found ? vn : 0);
    JS_FreeCString(ctx, block);
    JS_FreeCString(ctx, property);
    return result;
}

/* host.declSet(block, property, value): the block's serialization after setting (or, with "",
   removing) the property, as CSSOM's setProperty() writes it */
static JSValue decl_set(JSContext *ctx, JSValueConst self, int argc, JSValueConst *argv) {
    (void)self;
    size_t n;
    const char *block = JS_ToCStringLen(ctx, &n, argc > 0 ? argv[0] : JS_UNDEFINED);
    const char *property = JS_ToCString(ctx, argc > 1 ? argv[1] : JS_UNDEFINED);
    const char *value = JS_ToCString(ctx, argc > 2 ? argv[2] : JS_UNDEFINED);
    JSValue result;
    if (!block || !property || !value) result = JS_EXCEPTION;
    else {
        sbuf out = {0};
        int status = css_decl_set(block, n, property, value, NULL, NULL, &out);
        if (status == -2) result = JS_ThrowOutOfMemory(ctx);
        else if (status < 0) result = JS_ThrowTypeError(ctx, "Invalid CSS property name");
        else if (status > 0) result = JS_NewStringLen(ctx, out.p ? out.p : "", out.n);
        else result = JS_NewStringLen(ctx, block, n);
        sb_free(&out);
    }
    JS_FreeCString(ctx, block);
    JS_FreeCString(ctx, property);
    JS_FreeCString(ctx, value);
    return result;
}

/* host.readFile(path under the WPT root, $WPT_ROOT): its text, or null (idlharness's IDL files) */
static JSValue read_file(JSContext *ctx, JSValueConst self, int argc, JSValueConst *argv) {
    (void)self;
    const char *name = argc ? JS_ToCString(ctx, argv[0]) : NULL, *wpt = getenv("WPT_ROOT");
    if (!name) return JS_EXCEPTION;
    char path[4096];
    snprintf(path, sizeof path, "%s/%s", wpt ? wpt : ".", name[0] == '/' ? name + 1 : name);
    JS_FreeCString(ctx, name);
    size_t size = 0;
    char *bytes = slurp(path, &size);
    if (!bytes) return JS_NULL;
    JSValue result = JS_NewStringLen(ctx, bytes, size);
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
    if (argc < 2) return 2;
    JSRuntime *rt = JS_NewRuntime();
    JS_SetMaxStackSize(rt, 4u << 20);
    JSContext *ctx = JS_NewContext(rt);
    JSValue global = JS_GetGlobalObject(ctx), host = JS_NewObject(ctx);
    web_js_typed_om_init(ctx, host);
    web_js_webidl_init(ctx, host);
    JS_SetPropertyStr(ctx, host, "declBlock", JS_NewCFunction(ctx, decl_block, "declBlock", 1));
    JS_SetPropertyStr(ctx, host, "declGet", JS_NewCFunction(ctx, decl_get, "declGet", 2));
    JS_SetPropertyStr(ctx, host, "declSet", JS_NewCFunction(ctx, decl_set, "declSet", 3));
    JS_SetPropertyStr(ctx, host, "readFile", JS_NewCFunction(ctx, read_file, "readFile", 1));
    JS_SetPropertyStr(ctx, global, "host", host);
    JS_SetPropertyStr(ctx, global, "print", JS_NewCFunction(ctx, print, "print", 1));
    JS_FreeValue(ctx, global);
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "-e") && i + 1 < argc) {  /* -e CODE: a script given inline */
            i++;
            if (!run(ctx, argv[i], strlen(argv[i]), "inline")) return 2;
            continue;
        }
        size_t size = 0;
        char *source = slurp(argv[i], &size);
        if (!source) { printf("MISSING %s\n", argv[i]); continue; }  /* as a browser goes on past a 404 */
        /* a test's own script may throw at load, as in a browser: the harness reports it */
        run(ctx, source, size, argv[i]);
        free(source);
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
