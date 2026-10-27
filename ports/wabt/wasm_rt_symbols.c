/* Share the host runtime with in-memory C modules; never duplicate libc state. */
#include <libtcc.h>
#include "wasm-rt-impl.h"
#include <math.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>

/* TinyCC's x86-64 code generator calls out for conversions between unsigned
   64-bit integers and floating point (gen_cvt_itof1 and gen_cvt_ftoi1 in
   tccgen.c). With -nostdlib no libtcc1 is linked: the host's compiler does the
   conversion. wasm2c range-checks before every float-to-integer cast. */
static float u64_to_f32(unsigned long long value) { return (float)value; }
static double u64_to_f64(unsigned long long value) { return (double)value; }
static unsigned long long f32_to_u64(float value) { return (unsigned long long)value; }
static unsigned long long f64_to_u64(double value) { return (unsigned long long)value; }

int wasm_rt_add_tcc_symbols(TCCState *compiler) {
#define BIND_AS(symbol, function) do { if (tcc_add_symbol(compiler, symbol, (const void *)(function)) < 0) return -1; } while (0)
#define BIND(name) BIND_AS(#name, name)
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
    BIND_AS("__floatundisf", u64_to_f32); BIND_AS("__floatundidf", u64_to_f64);
    BIND_AS("__fixunssfdi", f32_to_u64); BIND_AS("__fixunsdfdi", f64_to_u64);
#undef BIND
#undef BIND_AS
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
