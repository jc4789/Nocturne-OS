/* The Web Cryptography API on the host: the product's QuickJS, js_dom_exception.js,
 * js_encoding.js, js_crypto.js and js_crypto.c over BearSSL, running WPT's own
 * WebCryptoAPI/ files under WPT's testharness.js.
 *     webcrypto_hosttest (<script> | -e <code>)...
 * The scripts run in order in one realm; then microtasks, the finished native jobs (RSA
 * key generation on its own thread), the posted tasks (the crypto task source) and the
 * stand-in timers of tests/webcrypto_host.js run until nothing is left. Not the guest, not
 * a browser realm: URL, timers, posted tasks and the console are stand-ins
 * (webcrypto_host.js), and so are the thread and the word watch of the native ABI (below).
 * Only the random device is the host's (the Windows CSPRNG, as crypto_aes_hosttest.c). */
#include <windows.h>
#include <bcrypt.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "quickjs.h"

static int64_t webcrypto_test_random(void *bytes, size_t size) {
    if (size > 65536) return -1;
    return BCryptGenRandom(NULL, bytes, (ULONG)size, BCRYPT_USE_SYSTEM_PREFERRED_RNG) < 0 ? -1 : (int64_t)size;
}
#define n_random webcrypto_test_random
#include "../user/libc/web/js_crypto.c"
#undef n_random

static void exception(JSContext *ctx);
/* ---- the native ABI calls of js_crypto.c's parallel work, on Windows ---- */
struct thread_start { thrd_start_t fn; void *arg; };
static DWORD WINAPI thread_main(LPVOID p) {
    struct thread_start start = *(struct thread_start *)p;
    free(p);
    return (DWORD)start.fn(start.arg);
}
int thrd_create(thrd_t *thr, thrd_start_t func, void *arg) {
    struct thread_start *start = malloc(sizeof *start);
    if (!start) return thrd_nomem;
    start->fn = func; start->arg = arg;
    HANDLE h = CreateThread(NULL, 0, thread_main, start, 0, NULL);
    if (!h) { free(start); return thrd_error; }
    *thr = (thrd_t)h;
    return thrd_success;
}
int thrd_detach(thrd_t thr) { CloseHandle((HANDLE)thr); return thrd_success; }
n_result wake_address(volatile uint32_t *address, unsigned count) { (void)address; (void)count; return 0; }
/* web_atomics_watch_word: the main loop below looks at the words instead of being woken. */
static struct { volatile uint32_t *word; uint32_t expected; JSValue callback; } watches[64];
static int watch_count;
int web_atomics_watch_word(JSContext *ctx, volatile uint32_t *word, uint32_t expected, JSValueConst callback) {
    if (__atomic_load_n(word, __ATOMIC_ACQUIRE) != expected) return 1;
    if (watch_count == 64) { JS_ThrowRangeError(ctx, "Too many native watches"); return -1; }
    watches[watch_count].word = word;
    watches[watch_count].expected = expected;
    watches[watch_count].callback = JS_DupValue(ctx, callback);
    watch_count++;
    return 0;
}
/* The first watch whose word changed: its callback, as the Atomics.waitAsync task would. */
static bool run_watch(JSContext *ctx) {
    for (int i = 0; i < watch_count; i++) {
        if (__atomic_load_n(watches[i].word, __ATOMIC_ACQUIRE) == watches[i].expected) continue;
        JSValue callback = watches[i].callback;
        watches[i] = watches[--watch_count];
        JSValue ok = JS_NewString(ctx, "ok"), r = JS_Call(ctx, callback, JS_UNDEFINED, 1, &ok);
        if (JS_IsException(r)) exception(ctx);
        JS_FreeValue(ctx, r); JS_FreeValue(ctx, ok); JS_FreeValue(ctx, callback);
        return true;
    }
    return false;
}

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
    web_js_crypto_init(ctx, host);
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
        if (run_watch(ctx)) continue;
        JSValue ran = JS_Eval(ctx, "__runTask()", 11, "tasks", JS_EVAL_TYPE_GLOBAL);
        if (JS_IsException(ran)) { exception(ctx); break; }
        bool more = JS_ToBool(ctx, ran) > 0;
        JS_FreeValue(ctx, ran);
        if (!more && watch_count && !JS_IsJobPending(rt)) { Sleep(1); continue; } /* a job still at work */
        if (!more && !JS_IsJobPending(rt)) break;
    }
    JS_FreeContext(ctx);
    JS_FreeRuntime(rt);
    return 0;
}
