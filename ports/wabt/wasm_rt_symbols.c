/* Share the host runtime with in-memory C modules; never duplicate libc state. */
#include <libtcc.h>
#include "wasm-rt-impl.h"
#include <math.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>

int wasm_rt_add_tcc_symbols(TCCState *compiler) {
#define BIND(name) do { if (tcc_add_symbol(compiler, #name, (const void *)(name)) < 0) return -1; } while (0)
    BIND(wasm_rt_init);
    BIND(wasm_rt_is_initialized);
    BIND(wasm_rt_free);
    BIND(wasm_rt_init_thread);
    BIND(wasm_rt_free_thread);
    BIND(wasm_rt_trap);
    BIND(wasm_rt_strerror);
    BIND(wasm_rt_poll);
    BIND(wasm_rt_check_native_stack);
    BIND(wasm_rt_allocate_memory);
    BIND(wasm_rt_grow_memory);
    BIND(wasm_rt_free_memory);
    BIND(wasm_rt_allocate_funcref_table);
    BIND(wasm_rt_free_funcref_table);
    BIND(wasm_rt_grow_funcref_table);
    BIND(wasm_rt_allocate_externref_table);
    BIND(wasm_rt_free_externref_table);
    BIND(wasm_rt_grow_externref_table);
    BIND(memcpy); BIND(memmove); BIND(memset); BIND(memcmp);
    BIND(malloc); BIND(calloc); BIND(realloc); BIND(free); BIND(abort);
    /* Nocturne's assert macro reports through the host's FILE object. */
    BIND(fprintf);
    if (tcc_add_symbol(compiler, "stderr", &stderr) < 0) return -1;
    BIND(fabs); BIND(fabsf); BIND(ceil); BIND(ceilf); BIND(floor); BIND(floorf);
    BIND(trunc); BIND(truncf); BIND(sqrt); BIND(sqrtf);
    BIND(copysign); BIND(nearbyint); BIND(nearbyintf);
    BIND(fmin); BIND(fminf); BIND(fmax); BIND(fmaxf);
    BIND(setjmp); BIND(longjmp);
#undef BIND
    return 0;
}

static int compile_memory(TCCState *compiler, void *source) {
    if (tcc_set_nocturne_stack_guard(compiler,
            offsetof(wasm_rt_native_stack_guard, low),
            offsetof(wasm_rt_native_stack_guard, recovery_sp),
            offsetof(wasm_rt_native_stack_guard, exhausted)) < 0 ||
        tcc_set_options(compiler, "-nostdlib") < 0 ||
        tcc_set_output_type(compiler, TCC_OUTPUT_MEMORY) < 0 ||
        wasm_rt_add_tcc_symbols(compiler) < 0 ||
        tcc_compile_string(compiler, source) < 0)
        return -1;
    return tcc_relocate(compiler);
}

int wasm_rt_compile_tcc(TCCState **output, const char *source,
                        void (*diagnostic)(void*, const char*),
                        int (*poll)(void*), void *opaque) {
    *output = NULL;
    TCCState *compiler = tcc_new();
    if (!compiler) return -2;
    tcc_set_error_func(compiler, opaque, diagnostic);
    tcc_set_interrupt_func(compiler, poll, opaque);
    int result = tcc_run_guarded(compiler, compile_memory, (void *)source);
    /* The context can outlive its synchronous caller, its opaque cannot. */
    tcc_set_error_func(compiler, NULL, NULL);
    tcc_set_interrupt_func(compiler, NULL, NULL);
    if (result < 0) tcc_delete(compiler);
    else *output = compiler;
    return result;
}
