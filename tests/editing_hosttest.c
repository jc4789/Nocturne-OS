/* The editing layer on the host (tests/check_editing.py): the product's QuickJS
 * compiles the four programs of the generated js_bootstrap.inc as the browser
 * does (a syntax error in any of them fails here), then runs the scripts named
 * on the command line in one realm, with editingProgram set to the value of the
 * js_editing program, as host.editing() returns it in a realm.
 *     editing_hosttest <script>...
 * Prints what the scripts print; exits 1 on an exception. */
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "quickjs.h"
#include "../user/libc/web/js_bootstrap.inc"

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

int main(int argc, char **argv) {
    setvbuf(stdout, NULL, _IONBF, 0);
    JSRuntime *rt = JS_NewRuntime();
    JS_SetMaxStackSize(rt, 4u << 20);
    JSContext *ctx = JS_NewContext(rt);
    static const struct { const char *name, *source; size_t len; } programs[] = {
        {"nocturne:browser-bindings", js_bootstrap, sizeof js_bootstrap - 1},
        {"nocturne:locale", js_locale, sizeof js_locale - 1},
        {"nocturne:webaudio", js_webaudio, sizeof js_webaudio - 1},
        {"nocturne:editing", js_editing, sizeof js_editing - 1},
    };
    int failed = 0;
    for (size_t i = 0; i < sizeof programs / sizeof *programs; i++) {
        JSValue code = JS_Eval(ctx, programs[i].source, programs[i].len, programs[i].name,
                               JS_EVAL_TYPE_GLOBAL | JS_EVAL_FLAG_COMPILE_ONLY);
        if (JS_IsException(code)) { printf("COMPILE FAIL %s\n", programs[i].name); exception(ctx); failed = 1; }
        else printf("COMPILE OK %s\n", programs[i].name);
        JS_FreeValue(ctx, code);
    }
    JSValue global = JS_GetGlobalObject(ctx);
    JS_SetPropertyStr(ctx, global, "print", JS_NewCFunction(ctx, print, "print", 1));
    JSValue editing = JS_Eval(ctx, js_editing, sizeof js_editing - 1, "nocturne:editing", JS_EVAL_TYPE_GLOBAL);
    if (JS_IsException(editing)) { exception(ctx); failed = 1; editing = JS_UNDEFINED; }
    JS_SetPropertyStr(ctx, global, "editingProgram", editing);
    JS_FreeValue(ctx, global);
    for (int i = 1; i < argc && !failed; i++) {
        size_t size = 0;
        char *source = slurp(argv[i], &size);
        if (!source) { printf("MISSING %s\n", argv[i]); failed = 1; break; }
        JSValue result = JS_Eval(ctx, source, size, argv[i], JS_EVAL_TYPE_GLOBAL);
        free(source);
        if (JS_IsException(result)) { exception(ctx); failed = 1; }
        JS_FreeValue(ctx, result);
        JSContext *job;
        while (JS_ExecutePendingJob(rt, &job) > 0) {}
    }
    JS_FreeContext(ctx);
    JS_FreeRuntime(rt);
    return failed;
}
