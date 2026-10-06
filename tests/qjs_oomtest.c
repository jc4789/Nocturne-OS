/* 実QuickJSへbackend allocation故障を注入。OS/DOMの受入試験ではない。 */
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <quickjs.h>

union block_header { size_t size; long double align; };
struct fault_state {
    int armed, fail_at, calls, injected;
    size_t live_count;
    JSValue primary; /* 比較専用のborrowed値。意図的に追加ownerにしない。 */
};
static int checks, failures;
static void check(const char *name, int passed) {
    checks++;
    if (!passed) { failures++; printf("FAIL qjs_oomtest %s\n", name); }
}
static int fault(struct fault_state *f) {
    if (!f->armed) return 0;
    f->calls++;
    if (f->fail_at && f->calls == f->fail_at) { f->injected++; return 1; }
    return 0;
}
static void *test_malloc(JSMallocState *s, size_t size) {
    struct fault_state *f = s->opaque;
    if (!size || size > SIZE_MAX - sizeof(union block_header) || fault(f)) return NULL;
    union block_header *h = malloc(sizeof *h + size);
    if (!h) return NULL;
    h->size = size; f->live_count++;
    s->malloc_count++; s->malloc_size += size + sizeof *h;
    return h + 1;
}
static void test_free(JSMallocState *s, void *p) {
    if (!p) return;
    union block_header *h = (union block_header *)p - 1;
    ((struct fault_state *)s->opaque)->live_count--;
    s->malloc_count--; s->malloc_size -= h->size + sizeof *h;
    free(h);
}
static void *test_realloc(JSMallocState *s, void *p, size_t size) {
    if (!p) return test_malloc(s, size);
    if (!size) { test_free(s, p); return NULL; }
    if (size > SIZE_MAX - sizeof(union block_header) || fault(s->opaque)) return NULL;
    union block_header *old = (union block_header *)p - 1;
    union block_header *next = malloc(sizeof *next + size);
    if (!next) return NULL;
    memcpy(next + 1, p, old->size < size ? old->size : size);
    s->malloc_size = s->malloc_size - old->size + size;
    next->size = size; free(old);
    return next + 1;
}
static size_t test_usable(const void *p) {
    return p ? ((const union block_header *)p - 1)->size : 0;
}
static const JSMallocFunctions allocator = {test_malloc, test_free, test_realloc, test_usable};

static JSValue primary_error(JSContext *ctx, JSValueConst self, int argc, JSValueConst *argv) {
    struct fault_state *f = JS_GetContextOpaque(ctx);
    JSValue error = JS_NewError(ctx);
    if (JS_IsException(error)) return error;
    if (JS_DefinePropertyValueStr(ctx, error, "message", JS_NewString(ctx, "oom-backtrace-primary"), JS_PROP_C_W_E) < 0) {
        JS_FreeValue(ctx, error); return JS_EXCEPTION;
    }
    f->primary = error;
    /* JS_Throwへ唯一のownerを渡した後のbacktraceを故障対象にする。 */
    f->armed = 1;
    return JS_Throw(ctx, error);
}

static int run_case(int syntax, int fail_at) {
    struct fault_state f = {0};
    f.fail_at = fail_at;
    JSRuntime *rt = JS_NewRuntime2(&allocator, &f);
    check("runtime", rt != NULL); if (!rt) return 0;
    JS_SetMaxStackSize(rt, 512 * 1024);
    JSContext *ctx = JS_NewContext(rt);
    check("context", ctx != NULL);
    if (!ctx) { JS_FreeRuntime(rt); return 0; }
    JS_SetContextOpaque(ctx, &f);
    JSValue global = JS_GetGlobalObject(ctx);
    JS_SetPropertyStr(ctx, global, "primaryError", JS_NewCFunction(ctx, primary_error, "primaryError", 0));
    JS_FreeValue(ctx, global);
    const char *source = "function backtraceFaultFrame(n){if(n)return backtraceFaultFrame(n-1);return primaryError();}backtraceFaultFrame(48);";
    char filename[8193];
    memset(filename, 'f', sizeof filename - 1); filename[sizeof filename - 1] = 0;
    if (syntax) { source = "function {"; f.armed = 1; }
    printf("qjs_oomtest: %s fault_at=%d\n", syntax ? "syntax" : "backtrace", fail_at); fflush(stdout);
    JSValue result = JS_Eval(ctx, source, strlen(source), syntax ? filename : "nocturne-oom-test.js", JS_EVAL_TYPE_GLOBAL);
    f.armed = 0;
    check("exception-return", JS_IsException(result));
    JS_FreeValue(ctx, result);
    check("pending-exception", JS_HasException(ctx));
    JSValue error = JS_GetException(ctx);
    check("exception-not-sentinel", !JS_IsException(error) && !JS_IsUninitialized(error));
    if (!syntax) {
        check("primary-identity", JS_IsObject(error) && JS_VALUE_GET_PTR(error) == JS_VALUE_GET_PTR(f.primary));
        JSValue message = JS_GetPropertyStr(ctx, error, "message");
        const char *text = JS_ToCString(ctx, message);
        check("primary-message", text && !strcmp(text, "oom-backtrace-primary"));
        JS_FreeCString(ctx, text); JS_FreeValue(ctx, message);
    }
    if (JS_IsObject(error)) {
        JSValue stack = JS_GetPropertyStr(ctx, error, "stack");
        check("stack-not-exception-value", !JS_IsException(stack));
        check("stack-string-or-absent", JS_IsString(stack) || JS_IsUndefined(stack));
        JS_FreeValue(ctx, stack);
    }
    JS_FreeValue(ctx, error);
    check("no-secondary-pending", !JS_HasException(ctx));
    result = JS_Eval(ctx, "21*2", 4, "after-oom", JS_EVAL_TYPE_GLOBAL);
    int32_t number = 0;
    check("runtime-still-usable", !JS_IsException(result) && JS_ToInt32(ctx, &number, result) == 0 && number == 42);
    JS_FreeValue(ctx, result);
    if (JS_HasException(ctx)) { result = JS_GetException(ctx); JS_FreeValue(ctx, result); }
    check("fault-reached", !fail_at || f.injected == 1);
    int calls = f.calls;
    JS_FreeContext(ctx); JS_FreeRuntime(rt);
    check("all-backend-allocations-freed", f.live_count == 0);
    return calls;
}

int main(int argc, char **argv) {
    /* CLI指定は修正前の特定故障を独立プロセスで再現するため。 */
    if (argc > 1) {
        run_case(argc > 2 && !strcmp(argv[2], "syntax"), atoi(argv[1]));
    } else {
        for (int syntax = 0; syntax < 2; syntax++) {
            int calls = run_case(syntax, 0);
            check("bounded-fault-points", calls > 0 && calls <= 64);
            if (calls > 64) calls = 64;
            for (int at = 1; at <= calls; at++) run_case(syntax, at);
        }
    }
    printf("qjs_oomtest: %d checks, %d failed\n", checks, failures);
    return failures ? 1 : 0;
}
