#ifndef NOCTURNE_WEB_JS_WASM_H
#define NOCTURNE_WEB_JS_WASM_H

#include "quickjs.h"

#ifdef __cplusplus
extern "C" {
#endif

/* poll follows the owning browser/worker's existing interrupt contract.
 * can_compile: positive permits, zero denies with CompileError, negative
 * propagates the callback's pending JS exception (for example allocation failure).
 * A null callback is the unrestricted non-Web embedding default. */
int web_js_wasm_init(JSContext *ctx, int (*poll)(JSContext *ctx),
                    int (*can_compile)(JSContext *ctx));
typedef struct web_js_wasm_stats {
    uint64_t compiled_modules;
    uint64_t compiled_bytes;
    uint64_t instances;
    uint64_t calls;
    uint64_t traps;
    uint64_t live_wrappers;
    uint64_t native_modules;
    uint64_t native_calls;
    uint64_t native_fallbacks;
    uint64_t jspi_suspensions;
    uint64_t jspi_resumptions;
    uint64_t jspi_pending;
} web_js_wasm_stats;
/* Read-only counters; no logging or clock syscalls in the execution path. */
int web_js_wasm_get_stats(JSContext *ctx, web_js_wasm_stats *out);
/* Borrowed until the next compile or context destruction; empty if no fallback. */
const char *web_js_wasm_native_fallback(JSContext *ctx);
/* Coalesce native Store reclamation at a safe browser/worker task boundary. */
void web_js_wasm_collect(JSContext *ctx);
/* Retire suspended executions before the realm's external state is freed. */
void web_js_wasm_cancel(JSContext *ctx);

#ifdef __cplusplus
}
#endif
#endif
