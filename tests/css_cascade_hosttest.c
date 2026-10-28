/* A document's cascade on the host: the product's QuickJS, the stand-in document of
 * tests/css_cascade_host.js over tests/css_cascade_host.c (dom.c's nodes, css.c's sheets and
 * cascade, cssom.c's rules), and the product's js_dom_exception.js and js_css_supports.js, running
 * WPT's own pages under WPT's testharness.js.
 *     css_cascade_hosttest (<script> | -e <code>)...
 * The scripts run in order in one realm; then microtasks and the stand-in timers run until nothing
 * is left. This file is built with the host's C library (its stdio); the engine's units with
 * Nocturne's headers. */
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "quickjs.h"

void css_cascade_host_init(JSContext *ctx, JSValue host);
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
/* readFile(path): a file's text (the page's markup, as JSON the check wrote), or null */
static JSValue read_file(JSContext *ctx, JSValueConst self, int argc, JSValueConst *argv) {
    (void)self;
    const char *name = argc ? JS_ToCString(ctx, argv[0]) : NULL;
    if (!name) return JS_EXCEPTION;
    size_t size = 0;
    char *bytes = slurp(name, &size);
    JS_FreeCString(ctx, name);
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
/* a process that dies says how: abort() and an invalid C library argument print the return
   addresses (offsets in this program, for addr2line) */
#ifdef _WIN32
#include <signal.h>
#include <windows.h>
static void died(const char *how) {
    void *frames[48];
    USHORT n = CaptureStackBackTrace(1, 48, frames, NULL);
    uintptr_t base = (uintptr_t)GetModuleHandleA(NULL);
    fprintf(stderr, "DIED %s:", how);
    for (USHORT i = 0; i < n; i++) fprintf(stderr, " %#llx", (unsigned long long)((uintptr_t)frames[i] - base + 0x140000000ull));
    fprintf(stderr, "\n");
    fflush(stderr);
}
static void on_abort(int sig) { (void)sig; died("abort"); }
static void on_parameter(const wchar_t *e, const wchar_t *f, const wchar_t *file, unsigned line, uintptr_t r) {
    (void)e; (void)f; (void)file; (void)line; (void)r;
    died("invalid parameter");
}
#endif
int main(int argc, char **argv) {
    if (argc < 2) return 2;
#ifdef _WIN32
    signal(SIGABRT, on_abort);
    _set_invalid_parameter_handler(on_parameter);
#endif
    /* what a page printed before the process dies stays in the log */
    setvbuf(stdout, NULL, _IONBF, 0);
    /* --args FILE: the arguments, NUL-separated, from a file (a debugger's command line quotes no scripts) */
    if (argc == 3 && !strcmp(argv[1], "--args")) {
        size_t size = 0;
        char *all = slurp(argv[2], &size);
        if (!all) return 2;
        int count = 1;
        for (size_t i = 0; i < size; i++) count += !all[i];
        char **list = calloc((size_t)count + 1, sizeof *list);
        if (!list) return 2;
        list[0] = argv[0];
        argc = 1;
        for (size_t i = 0; i < size; i += strlen(all + i) + 1) list[argc++] = all + i;
        argv = list;
    }
    JSRuntime *rt = JS_NewRuntime();
    JS_SetMaxStackSize(rt, 4u << 20);
    JSContext *ctx = JS_NewContext(rt);
    JSValue global = JS_GetGlobalObject(ctx), host = JS_NewObject(ctx);
    css_cascade_host_init(ctx, host);
    JS_SetPropertyStr(ctx, host, "readFile", JS_NewCFunction(ctx, read_file, "readFile", 1));
    JS_SetPropertyStr(ctx, global, "host", host);
    JS_SetPropertyStr(ctx, global, "print", JS_NewCFunction(ctx, print, "print", 1));
    JS_FreeValue(ctx, global);
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "-e") && i + 1 < argc) {
            i++;
            if (!run(ctx, argv[i], strlen(argv[i]), "inline")) return 2;
            continue;
        }
        size_t size = 0;
        char *source = slurp(argv[i], &size);
        if (!source) { printf("MISSING %s\n", argv[i]); continue; }
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
