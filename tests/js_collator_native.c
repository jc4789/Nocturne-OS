/* Standalone host/guest QuickJS decoder checks for the combined API batch.
 * Link the ordinary QuickJS objects and js_collator_native.c. No ICU or OS
 * locale service is used by these tests. The decoder is exposed only inside
 * this isolated test context, never by the browser's public JavaScript API. */
#include "quickjs.h"
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>

/* TinyCC receives the existing harness copy of quickjs.h. The actual helper
 * is linked from Nocturne libc; no decoder implementation is copied here. */
extern void web_js_collator_init(JSContext *ctx, JSValue host);

union allocation_header { size_t size; long double align; };
struct allocation_state {
    int armed, fail_at, calls, injected, quota_failures;
    size_t live;
};
static int checks, failures;
static void check(const char *name, int value) {
    checks++;
    if (!value) { failures++; printf("FAIL js_collator_native %s\n", name); }
}
static int fail_allocation(JSMallocState *s) {
    struct allocation_state *f = s->opaque;
    if (!f->armed) return 0;
    f->calls++;
    if (f->fail_at && f->calls == f->fail_at) { f->injected++; return 1; }
    return 0;
}
static void *accounted_malloc(JSMallocState *s, size_t size) {
    struct allocation_state *f = s->opaque;
    if (!size || size > SIZE_MAX - sizeof(union allocation_header) || fail_allocation(s)) return NULL;
    size_t total = size + sizeof(union allocation_header);
    if (total > SIZE_MAX - s->malloc_size || s->malloc_size + total > s->malloc_limit) {
        f->quota_failures++; return NULL;
    }
    union allocation_header *h = malloc(total);
    if (!h) return NULL;
    h->size = size; f->live++; s->malloc_count++; s->malloc_size += total;
    return h + 1;
}
static void accounted_free(JSMallocState *s, void *p) {
    if (!p) return;
    union allocation_header *h = (union allocation_header *)p - 1;
    ((struct allocation_state *)s->opaque)->live--;
    s->malloc_count--; s->malloc_size -= h->size + sizeof(*h);
    free(h);
}
static void *accounted_realloc(JSMallocState *s, void *p, size_t size) {
    if (!p) return accounted_malloc(s, size);
    if (!size) { accounted_free(s, p); return NULL; }
    if (size > SIZE_MAX - sizeof(union allocation_header) || fail_allocation(s)) return NULL;
    union allocation_header *old = (union allocation_header *)p - 1;
    size_t base = s->malloc_size - old->size;
    if (size > SIZE_MAX - base || base + size > s->malloc_limit) {
        ((struct allocation_state *)s->opaque)->quota_failures++; return NULL;
    }
    union allocation_header *next = malloc(sizeof(*next) + size);
    if (!next) return NULL;
    memcpy(next + 1, p, old->size < size ? old->size : size);
    next->size = size; s->malloc_size = base + size;
    free(old); return next + 1;
}
static size_t accounted_usable(const void *p) {
    return p ? ((const union allocation_header *)p - 1)->size : 0;
}
static const JSMallocFunctions allocator = {
    accounted_malloc, accounted_free, accounted_realloc, accounted_usable
};

static void consume_exception(JSContext *ctx) {
    if (JS_HasException(ctx)) { JSValue error = JS_GetException(ctx); JS_FreeValue(ctx, error); }
}
static JSValue private_decoder(JSContext *ctx) {
    JSValue host = JS_NewObject(ctx);
    if (JS_IsException(host)) return host;
    web_js_collator_init(ctx, host);
    JSValue decoder = JS_GetPropertyStr(ctx, host, "collationUnpack");
    JS_FreeValue(ctx, host);
    return decoder;
}

int js_collator_native_cases(JSContext *ctx) {
    JSValue host = JS_NewObject(ctx);
    if (JS_IsException(host)) return -1;
    web_js_collator_init(ctx, host);
    JSValue decoder = JS_GetPropertyStr(ctx, host, "collationUnpack");
    JS_FreeValue(ctx, host);
    if (JS_IsException(decoder)) return -1;
    JSValue global = JS_GetGlobalObject(ctx);
    if (JS_SetPropertyStr(ctx, global, "__testCollationDecoder", decoder) < 0) {
        JS_FreeValue(ctx, global); return -1;
    }
    JS_FreeValue(ctx, global);
    static const char source[] =
        "(()=>{'use strict';const decode=globalThis.__testCollationDecoder;"
        "delete globalThis.__testCollationDecoder;let n=0;"
        "const eq=(a,b)=>{n++;if(a!==b)throw Error('decoder '+a+' != '+b);};"
        "const bad=(x,name='TypeError')=>{n++;try{decode(x);}catch(e){if(e.name===name)return;throw e;}throw Error('decoder accepted invalid data');};"
        "eq(new Uint32Array(decode('')).length,0);"
        "eq(new Uint32Array(decode('AA=='))[0],0);"
        "eq(new Uint32Array(decode('fw=='))[0],127);"
        "eq(new Uint32Array(decode('gAE='))[0],128);"
        "eq(new Uint32Array(decode('//8D'))[0],65535);"
        "eq(new Uint32Array(decode('/////w8='))[0],4294967295);"
        "eq(new Uint32Array(decode('AAB/gAE=' )).join(','),'0,0,127,128');"
        "for(const x of [null,1,{},Symbol(),'A','AAA','!A==','AA=A','AA==AA==','AB==','AAB=','gA==','gAA=','/////xA=','//////8=','4w==','ＡＡ=='])bad(x);"
        "bad('A'.repeat(4194308),'RangeError');"
        "return n;})();";
    JSValue result = JS_Eval(ctx, source, sizeof(source) - 1, "collation-decoder", JS_EVAL_TYPE_GLOBAL);
    if (JS_IsException(result)) return -1;
    int32_t count = 0;
    int converted = JS_ToInt32(ctx, &count, result);
    JS_FreeValue(ctx, result);
    return converted < 0 ? -1 : count;
}

static int run_failure_case(int fault_at, int quota) {
    struct allocation_state f = {0};
    JSRuntime *rt = JS_NewRuntime2(&allocator, &f);
    check("runtime-created", rt != NULL); if (!rt) return 0;
    JS_SetMaxStackSize(rt, 512 * 1024);
    JSContext *ctx = JS_NewContext(rt);
    check("context-created", ctx != NULL);
    if (!ctx) { JS_FreeRuntime(rt); check("runtime-only-free", f.live == 0); return 0; }
    JSValue decoder = private_decoder(ctx);
    check("private-decoder-created", JS_IsFunction(ctx, decoder));
    char packed[4097]; memset(packed, 'A', sizeof(packed) - 1); packed[sizeof(packed) - 1] = 0;
    JSValue input = JS_NewStringLen(ctx, packed, sizeof(packed) - 1);
    check("packed-input-created", JS_IsString(input));
    /* Warm intrinsic buffer creation before measuring the bounded fault sites. */
    JSValue result = JS_Call(ctx, decoder, JS_UNDEFINED, 1, &input);
    check("warm-decode", !JS_IsException(result));
    JS_FreeValue(ctx, result); consume_exception(ctx); JS_RunGC(rt);
    JSMemoryUsage before;
    JS_ComputeMemoryUsage(rt, &before);
    if (quota) JS_SetMemoryLimit(rt, (size_t)before.malloc_size + 128);
    f.armed = 1; f.fail_at = fault_at;
    result = JS_Call(ctx, decoder, JS_UNDEFINED, 1, &input);
    f.armed = 0;
    int calls = f.calls;
    JS_SetMemoryLimit(rt, SIZE_MAX);
    if (quota || fault_at) {
        check("controlled-failure-exception", JS_IsException(result));
        check("pending-failure-consumed", JS_HasException(ctx));
        if (quota) check("runtime-quota-enforced", f.quota_failures > 0);
        else check("requested-fault-reached", f.injected == 1);
    } else {
        size_t size = 0; uint8_t *bytes = JS_GetArrayBuffer(ctx, &size, result);
        check("successful-owned-buffer", bytes && size == 12288);
        check("buffer-zero-boundaries", bytes && !bytes[0] && !bytes[size - 1]);
    }
    JS_FreeValue(ctx, result); consume_exception(ctx); JS_RunGC(rt);
    check("no-stale-exception-after-gc", !JS_HasException(ctx));
    result = JS_Call(ctx, decoder, JS_UNDEFINED, 1, &input);
    size_t size = 0; uint8_t *bytes = JS_IsException(result) ? NULL : JS_GetArrayBuffer(ctx, &size, result);
    check("decode-survives-oom-and-gc", !JS_IsException(result) && bytes && size == 12288);
    if (!JS_IsException(result)) JS_DetachArrayBuffer(ctx, result);
    JS_FreeValue(ctx, result); consume_exception(ctx); JS_RunGC(rt);
    result = JS_Eval(ctx, "21*2", 4, "decoder-after-oom", JS_EVAL_TYPE_GLOBAL);
    int32_t answer = 0;
    check("context-still-usable", !JS_IsException(result) && JS_ToInt32(ctx, &answer, result) == 0 && answer == 42);
    JS_FreeValue(ctx, result); consume_exception(ctx);
    JS_FreeValue(ctx, input); JS_FreeValue(ctx, decoder); JS_RunGC(rt);
    JS_FreeContext(ctx); JS_FreeRuntime(rt);
    check("all-accounted-allocations-freed", f.live == 0);
    return calls;
}

int main(void) {
    struct allocation_state f = {0};
    JSRuntime *rt = JS_NewRuntime2(&allocator, &f);
    check("surface-runtime", rt != NULL);
    if (rt) {
        JS_SetMaxStackSize(rt, 512 * 1024);
        JSContext *ctx = JS_NewContext(rt);
        check("surface-context", ctx != NULL);
        if (ctx) {
            int count = js_collator_native_cases(ctx);
            check("strict-format-cases", count >= 25);
            if (count < 0 && JS_HasException(ctx)) {
                JSValue error = JS_GetException(ctx);
                const char *message = JS_ToCString(ctx, error);
                printf("FAIL js_collator_native surface exception: %s\n", message ? message : "unavailable");
                JS_FreeCString(ctx, message); JS_FreeValue(ctx, error);
            }
            if (count > 0) checks += count;
            consume_exception(ctx); JS_RunGC(rt); JS_FreeContext(ctx);
        }
        JS_FreeRuntime(rt); check("surface-all-allocations-freed", f.live == 0);
    }
    int sites = run_failure_case(0, 0);
    check("bounded-decoder-allocation-sites", sites > 0 && sites <= 16);
    if (sites > 16) sites = 16;
    for (int at = 1; at <= sites; at++) run_failure_case(at, 0);
    run_failure_case(0, 1);
    printf("js_collator_native: %d checks, %d failed\n", checks, failures);
    return failures ? 1 : 0;
}
