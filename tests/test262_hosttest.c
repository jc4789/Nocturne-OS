/* Host runner for one test262 script on the product QuickJS (third_party/quickjs): the test's
   harness files and body come concatenated in one file, as tests/check_test262.py builds it.
   It defines print and the $262 host hooks test262's INTERPRETING.md asks of a host that this
   engine can give (createRealm, evalScript, detachArrayBuffer, gc, global), evaluates the
   script, runs the job queue empty, and prints what the test printed and any uncaught
   exception as "exception: Name: message". */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "quickjs.h"

static JSValue print(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
    for (int i = 0; i < argc; i++) {
        const char *s = JS_ToCString(ctx, argv[i]);
        fputs(s ? s : "<string conversion failed>", stdout);
        JS_FreeCString(ctx, s);
        fputc(i + 1 < argc ? ' ' : '\n', stdout);
    }
    return JS_UNDEFINED;
}

static void report(JSContext *ctx, JSValue error) {
    JSValue name = JS_GetPropertyStr(ctx, error, "name"), message = JS_GetPropertyStr(ctx, error, "message");
    const char *n = JS_IsString(name) ? JS_ToCString(ctx, name) : NULL;
    const char *m = JS_IsString(message) ? JS_ToCString(ctx, message) : JS_ToCString(ctx, error);
    printf("exception: %s: %s\n", n ? n : "(not an Error)", m ? m : "");
    JS_FreeCString(ctx, n);
    JS_FreeCString(ctx, m);
    JS_FreeValue(ctx, name);
    JS_FreeValue(ctx, message);
}

static JSValue host_object(JSContext *ctx);

static JSValue create_realm(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
    JSContext *realm = JS_NewContext(JS_GetRuntime(ctx));
    if (!realm) return JS_ThrowOutOfMemory(ctx);
    JSValue host = host_object(realm);
    /* The realm lives as long as the runtime: the test may keep its objects. */
    return host;
}

static JSValue eval_script(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
    size_t n;
    const char *source = JS_ToCStringLen(ctx, &n, argv[0]);
    if (!source) return JS_EXCEPTION;
    JSValue r = JS_Eval(ctx, source, n, "<evalScript>", JS_EVAL_TYPE_GLOBAL);
    JS_FreeCString(ctx, source);
    return r;
}

static JSValue detach_buffer(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
    JS_DetachArrayBuffer(ctx, argv[0]);
    return JS_UNDEFINED;
}

static JSValue run_gc(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
    JS_RunGC(JS_GetRuntime(ctx));
    return JS_UNDEFINED;
}

/* The realm's $262, also defined on its global object with print. */
static JSValue host_object(JSContext *ctx) {
    JSValue global = JS_GetGlobalObject(ctx), host = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, host, "createRealm", JS_NewCFunction(ctx, create_realm, "createRealm", 0));
    JS_SetPropertyStr(ctx, host, "evalScript", JS_NewCFunction(ctx, eval_script, "evalScript", 1));
    JS_SetPropertyStr(ctx, host, "detachArrayBuffer", JS_NewCFunction(ctx, detach_buffer, "detachArrayBuffer", 1));
    JS_SetPropertyStr(ctx, host, "gc", JS_NewCFunction(ctx, run_gc, "gc", 0));
    JS_SetPropertyStr(ctx, host, "global", JS_DupValue(ctx, global));
    JS_SetPropertyStr(ctx, global, "$262", JS_DupValue(ctx, host));
    JS_SetPropertyStr(ctx, global, "print", JS_NewCFunction(ctx, print, "print", 1));
    JS_FreeValue(ctx, global);
    return host;
}

int main(int argc, char **argv) {
    if (argc < 2) return 2;
    FILE *f = fopen(argv[1], "rb");
    if (!f) return 2;
    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);
    char *script = malloc((size_t)size + 1);
    size_t n = fread(script, 1, (size_t)size, f);
    fclose(f);
    script[n] = 0;
    JSRuntime *rt = JS_NewRuntime();
    JSContext *ctx = JS_NewContext(rt);
    JS_FreeValue(ctx, host_object(ctx));
    JSValue r = JS_Eval(ctx, script, n, argv[1], JS_EVAL_TYPE_GLOBAL);
    if (JS_IsException(r)) {
        JSValue error = JS_GetException(ctx);
        report(ctx, error);
        JS_FreeValue(ctx, error);
    }
    JS_FreeValue(ctx, r);
    for (;;) {
        JSContext *job_ctx;
        int ret = JS_ExecutePendingJob(rt, &job_ctx);
        if (ret == 0) break;
        if (ret < 0) {
            JSValue error = JS_GetException(job_ctx);
            report(job_ctx, error);
            JS_FreeValue(job_ctx, error);
        }
    }
    fflush(stdout);
    /* The realms createRealm made stay with the runtime; the process ends here. */
    return 0;
}
