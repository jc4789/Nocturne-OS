/* Nocturne's wasm2c target configuration and native execution boundary. */
#ifndef NOCTURNE_WASM_RT_H
#define NOCTURNE_WASM_RT_H
#include <stdint.h>
#define WASM_RT_NOCTURNE 1
#define WASM_RT_USE_MMAP 0
#define WASM_RT_MEMCHECK_GUARD_PAGES 0
#define WASM_RT_MEMCHECK_BOUNDS_CHECK 1
#define WASM_RT_USE_SEGUE 0
#define WASM_RT_USE_LOCAL_MEMORY_BASE_SIZE 0
#define WASM_RT_STACK_DEPTH_COUNT 0
#define WASM_RT_STACK_EXHAUSTION_HANDLER 0
#define WASM_RT_NATIVE_STACK_CHECK 1
#define WASM_RT_TRAP_HANDLER wasm_rt_native_trap
#ifdef __cplusplus
extern "C" {
#endif
typedef int (*wasm_rt_poll_fn)(void*);
typedef struct {
  wasm_rt_poll_fn callback;
  void* opaque;
} wasm_rt_poll_state;
int wasm_rt_set_poll(wasm_rt_poll_fn, void*, wasm_rt_poll_state* previous);
void wasm_rt_poll(void);
void wasm_rt_check_native_stack(void);
int wasm_rt_run(void (*entry)(void*), void* opaque);
void* wasm_rt_default_trap(void);
void wasm_rt_native_init_thread(void);
void wasm_rt_native_free_thread(void);
int wasm_rt_native_initialized(void);
struct TCCState;
int wasm_rt_add_tcc_symbols(struct TCCState*);
/* Owns all TinyCC setup/compilation/relocation in a C recovery frame. On
 * failure no compiler escapes; -2 means actual allocation failure. */
int wasm_rt_compile_tcc(struct TCCState** output, const char* source,
                        void (*diagnostic)(void*, const char*),
                        int (*poll)(void*), void* opaque);
struct wasm_host_instance;
union wasm_host_value;
void wasm_rt_native_invoke(
    void (*invoke)(struct wasm_host_instance*, uint32_t,
                   const union wasm_host_value*, union wasm_host_value*),
    struct wasm_host_instance*, uint32_t,
    const union wasm_host_value*, union wasm_host_value*);
/* The native x86-64 entry owns R15 (descriptor) and R12 (check scratch).
 * Offsets are passed to TinyCC from this type, not embedded C assumptions. */
typedef struct wasm_rt_native_stack_guard {
    uintptr_t low;
    uintptr_t recovery_sp;
    void (*exhausted)(void);
} wasm_rt_native_stack_guard;
typedef struct wasm_rt_coroutine wasm_rt_coroutine;
enum {
    WASM_RT_COROUTINE_ERROR = -1,
    WASM_RT_COROUTINE_SUSPENDED = 0,
    WASM_RT_COROUTINE_DONE = 1
};
/* Same-native-thread stackful continuation. resume/yield swap runtime TLS
 * (trap target, poll state and actual stack bounds) as well as callee-saved
 * registers and floating-point control. JS/QuickJS execution state belongs
 * to the caller and must be swapped separately around these operations.
 * A suspended C++ frame must be resumed/cancelled and normally unwound before
 * destroy; destroy rejects a started, unfinished continuation. */
wasm_rt_coroutine* wasm_rt_coroutine_create(void (*entry)(void*), void* opaque);
int wasm_rt_coroutine_resume(wasm_rt_coroutine*);
int wasm_rt_coroutine_yield(wasm_rt_coroutine*);
int wasm_rt_coroutine_bounds(const wasm_rt_coroutine*, uintptr_t* low, uintptr_t* high);
int wasm_rt_coroutine_destroy(wasm_rt_coroutine*);
#ifdef __cplusplus
}
#endif
#endif
