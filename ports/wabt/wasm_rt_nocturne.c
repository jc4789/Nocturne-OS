/* Native per-thread wasm2c execution state. No signals, POSIX TLS or ELF TLS. */
#include "wasm-rt-impl.h"
#include <nocturne.h>
#include <stddef.h>
#include <errno.h>

/* Match the interpreter's existing 1000-instruction scheduling batch. This
 * counts native loop/function checkpoints, not accepted-module capacity. */
enum { NATIVE_POLL_WORK_QUANTUM = 1000 };

struct wasm_native_thread {
    wasm_rt_jmp_buf default_trap;
    wasm_rt_jmp_buf *active_trap;
    wasm_rt_poll_state poll;
    unsigned poll_work;
    bool interrupted;
    uintptr_t stack_low, stack_high;
    wasm_rt_coroutine *coroutine;
};

static struct wasm_native_thread *thread_state(void) {
    struct wasm_native_thread *state = thread_local_get(N_TLS_WASM_RUNTIME);
    if (state) return state;
    state = calloc(1, sizeof *state);
    if (!state) return NULL;
    if (thread_stack_bounds(&state->stack_low, &state->stack_high) < 0) {
        free(state);
        return NULL;
    }
    thread_local_set(N_TLS_WASM_RUNTIME, state);
    return state;
}

void wasm_rt_native_init_thread(void) {
    if (!thread_state()) abort();
}
int wasm_rt_native_initialized(void) {
    return thread_local_get(N_TLS_WASM_RUNTIME) != NULL;
}
void wasm_rt_native_free_thread(void) {
    struct wasm_native_thread *state = thread_local_get(N_TLS_WASM_RUNTIME);
    if (!state) return;
    if (state->active_trap) abort();
    /* A coroutine owns its embedded state until its C++ entry has unwound. */
    if (state->coroutine) return;
    thread_local_set(N_TLS_WASM_RUNTIME, NULL);
    free(state);
}
void *wasm_rt_default_trap(void) {
    struct wasm_native_thread *state = thread_state();
    if (!state) abort();
    return &state->default_trap;
}
int wasm_rt_set_poll(wasm_rt_poll_fn callback, void *opaque, wasm_rt_poll_state *previous) {
    struct wasm_native_thread *state = thread_state();
    if (!state) return -1;
    if (previous) *previous = state->poll;
    state->poll = (wasm_rt_poll_state){callback, opaque};
    state->poll_work = 0;
    /* A setter cannot longjmp through its C++ caller. Remember cancellation
       and let the next C run guard turn it into the interruption trap. */
    state->interrupted = callback && callback(opaque);
    return 0;
}
WASM_RT_NO_RETURN void wasm_rt_native_trap(wasm_rt_trap_t code) {
    struct wasm_native_thread *state = thread_state();
    if (!state) abort();
    wasm_rt_jmp_buf *target = state->active_trap ? state->active_trap : &state->default_trap;
    WASM_RT_LONGJMP(*target, code);
}
void wasm_rt_poll(void) {
    struct wasm_native_thread *state = thread_state();
    if (!state) wasm_rt_native_trap(WASM_RT_TRAP_EXHAUSTION);
    if (state->interrupted) wasm_rt_native_trap(WASM_RT_TRAP_INTERRUPTED);
    if (++state->poll_work < NATIVE_POLL_WORK_QUANTUM) return;
    state->poll_work = 0;
    if (state->poll.callback && state->poll.callback(state->poll.opaque))
        wasm_rt_native_trap(WASM_RT_TRAP_INTERRUPTED);
}
uintptr_t wasm_rt_native_stack_low(void) {
    struct wasm_native_thread *state = thread_state();
    return state ? state->stack_low : UINTPTR_MAX;
}
void wasm_rt_check_native_stack(void) {
    struct wasm_native_thread *state = thread_state();
    char marker;
    if (!state || (uintptr_t)&marker < state->stack_low ||
        (uintptr_t)&marker >= state->stack_high)
        wasm_rt_native_trap(WASM_RT_TRAP_EXHAUSTION);
}
int wasm_rt_run(void (*entry)(void *), void *opaque) {
    struct wasm_native_thread *state = thread_state();
    if (!state) return WASM_RT_TRAP_EXHAUSTION;
    wasm_rt_jmp_buf guard = {0};
    wasm_rt_jmp_buf *previous = state->active_trap;
    wasm_rt_poll_state saved_poll = state->poll;
    unsigned saved_work = state->poll_work;
    bool saved_interrupted = state->interrupted;
    state->active_trap = &guard;
    int result = WASM_RT_SETJMP(guard);
    if (!result) {
        /* Entry always polls, including a fresh/short invocation. */
        state->poll_work = NATIVE_POLL_WORK_QUANTUM;
        wasm_rt_poll();
        entry(opaque);
    }
    state->active_trap = previous;
    state->poll = saved_poll;
    state->poll_work = saved_work;
    state->interrupted = saved_interrupted;
    return result;
}

/* Own assembly for Nocturne's x86-64 ABI. It performs no stack writes after
 * an inline check fails: generated code restores this entry's safe anchor
 * and jumps to exhausted(), which returns only through the C trap guard. */
_Static_assert(sizeof(uintptr_t) == 8, "native x86-64 pointer ABI");
_Static_assert(offsetof(wasm_rt_native_stack_guard, low) == 0, "guard low ABI");
_Static_assert(offsetof(wasm_rt_native_stack_guard, recovery_sp) == sizeof(uintptr_t), "guard stack ABI");
_Static_assert(offsetof(wasm_rt_native_stack_guard, exhausted) == 2 * sizeof(uintptr_t), "guard handler ABI");
typedef void (*native_invoke_fn)(struct wasm_host_instance*, uint32_t,
                                const union wasm_host_value*, union wasm_host_value*);
extern void wasm_rt_enter_native(native_invoke_fn, struct wasm_host_instance*,
                                 uint32_t, const union wasm_host_value*,
                                 union wasm_host_value*, wasm_rt_native_stack_guard*);
__asm__(
    ".text\n"
    ".global wasm_rt_enter_native\n"
    ".type wasm_rt_enter_native,@function\n"
    "wasm_rt_enter_native:\n"
    "push %r12\n"
    "push %r15\n"
    "sub $8,%rsp\n" /* SysV call alignment after the two saved registers. */
    "mov %r9,%r15\n"
    "lea -8(%rsp),%rax\n" /* The following call's return-address slot. */
    "mov %rax,8(%r15)\n"
    "mov %rdi,%rax\n"
    "mov %rsi,%rdi\n"
    "mov %rdx,%rsi\n"
    "mov %rcx,%rdx\n"
    "mov %r8,%rcx\n"
    "call *%rax\n"
    "add $8,%rsp\n"
    "pop %r15\n"
    "pop %r12\n"
    "ret\n"
    ".size wasm_rt_enter_native,.-wasm_rt_enter_native\n");

static WASM_RT_NO_RETURN void native_stack_exhausted(void) {
    wasm_rt_native_trap(WASM_RT_TRAP_EXHAUSTION);
}
void wasm_rt_native_invoke(native_invoke_fn invoke, struct wasm_host_instance *instance,
                           uint32_t index, const union wasm_host_value *params,
                           union wasm_host_value *results) {
    struct wasm_native_thread *state = thread_state();
    if (!state || !state->active_trap) wasm_rt_native_trap(WASM_RT_TRAP_EXHAUSTION);
    wasm_rt_native_stack_guard guard = {state->stack_low, 0, native_stack_exhausted};
    wasm_rt_enter_native(invoke, instance, index, params, results, &guard);
}

struct coroutine_context {
    uintptr_t rbx, rbp, r12, r13, r14, r15, rsp, rip;
    uint32_t mxcsr;
    uint16_t x87_control;
};
struct wasm_rt_coroutine {
    struct coroutine_context context, caller;
    struct wasm_native_thread runtime;
    struct wasm_native_thread *caller_runtime;
    void (*entry)(void *);
    void *opaque;
    int owner_tid;
    bool started, running, done;
};
_Static_assert(offsetof(struct coroutine_context, rsp) == 6 * sizeof(uintptr_t), "coroutine rsp ABI");
_Static_assert(offsetof(struct coroutine_context, rip) == 7 * sizeof(uintptr_t), "coroutine rip ABI");
_Static_assert(offsetof(struct coroutine_context, mxcsr) == 8 * sizeof(uintptr_t), "coroutine MXCSR ABI");
_Static_assert(offsetof(struct coroutine_context, x87_control) == 8 * sizeof(uintptr_t) + sizeof(uint32_t), "coroutine x87 ABI");

extern void wasm_rt_coroutine_switch(struct coroutine_context*, const struct coroutine_context*);
extern void wasm_rt_coroutine_bootstrap(void);
/* Cooperative call boundary: SysV caller-saved values are already spilled
 * by the calling compiler. Preserve all callee-saved registers and FP control
 * without an OS thread, signal frame, POSIX context or copied live C stack. */
__asm__(
    ".text\n"
    ".global wasm_rt_coroutine_switch\n"
    ".type wasm_rt_coroutine_switch,@function\n"
    "wasm_rt_coroutine_switch:\n"
    "mov %rbx,0(%rdi)\n"
    "mov %rbp,8(%rdi)\n"
    "mov %r12,16(%rdi)\n"
    "mov %r13,24(%rdi)\n"
    "mov %r14,32(%rdi)\n"
    "mov %r15,40(%rdi)\n"
    "lea 8(%rsp),%rax\n"
    "mov %rax,48(%rdi)\n"
    "mov (%rsp),%rax\n"
    "mov %rax,56(%rdi)\n"
    "stmxcsr 64(%rdi)\n"
    "fnstcw 68(%rdi)\n"
    "ldmxcsr 64(%rsi)\n"
    "fldcw 68(%rsi)\n"
    "mov 0(%rsi),%rbx\n"
    "mov 8(%rsi),%rbp\n"
    "mov 16(%rsi),%r12\n"
    "mov 24(%rsi),%r13\n"
    "mov 32(%rsi),%r14\n"
    "mov 40(%rsi),%r15\n"
    "mov 48(%rsi),%rsp\n"
    "jmp *56(%rsi)\n"
    ".size wasm_rt_coroutine_switch,.-wasm_rt_coroutine_switch\n"
    ".global wasm_rt_coroutine_bootstrap\n"
    ".type wasm_rt_coroutine_bootstrap,@function\n"
    "wasm_rt_coroutine_bootstrap:\n"
    "mov %r12,%rdi\n"
    "jmp wasm_rt_coroutine_entry\n"
    ".size wasm_rt_coroutine_bootstrap,.-wasm_rt_coroutine_bootstrap\n");

/* Return through the saved caller context, never through an invented native
 * return address. Kept as a named entry because the assembly bootstrap jumps
 * here with the ordinary Nocturne x86-64 C entry alignment. */
WASM_RT_NO_RETURN void wasm_rt_coroutine_entry(wasm_rt_coroutine *co) {
    co->entry(co->opaque);
    co->done = true;
    co->running = false;
    thread_local_set(N_TLS_WASM_RUNTIME, co->caller_runtime);
    wasm_rt_coroutine_switch(&co->context, &co->caller);
    abort(); /* resume() rejects a completed continuation. */
}

static int coroutine_owner(const wasm_rt_coroutine *co) {
    if (!co || co->owner_tid != thread_id()) { errno = EINVAL; return -1; }
    return 0;
}

wasm_rt_coroutine *wasm_rt_coroutine_create(void (*entry)(void *), void *opaque) {
    if (!entry) { errno = EINVAL; return NULL; }
    uintptr_t task_low, task_high;
    if (thread_stack_bounds(&task_low, &task_high) < 0) return NULL;
    if (task_high <= task_low || task_high - task_low > SIZE_MAX - N_PAGE_SIZE) {
        errno = ENOMEM; return NULL;
    }
    size_t usable = task_high - task_low;
    wasm_rt_coroutine *co = calloc(1, sizeof *co);
    if (!co) { errno = ENOMEM; return NULL; }
    /* Use the real native task's reserved stack capacity. n_memory_reserve
       leaves a trailing unmapped page; release the leading page for a lower
       guard. */
    char *mapping = n_memory_reserve(usable + N_PAGE_SIZE, N_MEMORY_READ_WRITE);
    if (!mapping) { free(co); return NULL; } /* errno set by the reserve */
    if (n_memory_release(mapping, N_PAGE_SIZE) < 0) {
        int error = errno;
        n_memory_release(mapping, usable + N_PAGE_SIZE);
        errno = error; free(co); return NULL;
    }
    co->runtime.stack_low = (uintptr_t)mapping + N_PAGE_SIZE;
    co->runtime.stack_high = co->runtime.stack_low + usable;
    co->runtime.coroutine = co;
    co->owner_tid = thread_id(); co->entry = entry; co->opaque = opaque;
    /* SysV x86-64 entry is 8 mod 16, as after a native call instruction.
       Only this fresh, not-yet-live frame is initialized by the creator. */
    co->context.rsp = (co->runtime.stack_high & ~(uintptr_t)15) - sizeof(uintptr_t);
    *(uintptr_t *)co->context.rsp = 0;
    co->context.rip = (uintptr_t)wasm_rt_coroutine_bootstrap;
    co->context.r12 = (uintptr_t)co;
    __asm__ volatile("stmxcsr %0" : "=m"(co->context.mxcsr));
    __asm__ volatile("fnstcw %0" : "=m"(co->context.x87_control));
    return co;
}

int wasm_rt_coroutine_resume(wasm_rt_coroutine *co) {
    if (coroutine_owner(co) < 0) return WASM_RT_COROUTINE_ERROR;
    if (co->running) { errno = EBUSY; return WASM_RT_COROUTINE_ERROR; }
    if (co->done) return WASM_RT_COROUTINE_DONE;
    co->caller_runtime = thread_local_get(N_TLS_WASM_RUNTIME);
    co->started = co->running = true;
    thread_local_set(N_TLS_WASM_RUNTIME, &co->runtime);
    wasm_rt_coroutine_switch(&co->caller, &co->context);
    return co->done ? WASM_RT_COROUTINE_DONE : WASM_RT_COROUTINE_SUSPENDED;
}

int wasm_rt_coroutine_yield(wasm_rt_coroutine *co) {
    if (coroutine_owner(co) < 0) return -1;
    if (!co->running || thread_local_get(N_TLS_WASM_RUNTIME) != &co->runtime) {
        errno = EINVAL; return -1;
    }
    co->running = false;
    thread_local_set(N_TLS_WASM_RUNTIME, co->caller_runtime);
    wasm_rt_coroutine_switch(&co->context, &co->caller);
    return 0;
}

int wasm_rt_coroutine_bounds(const wasm_rt_coroutine *co, uintptr_t *low, uintptr_t *high) {
    if (coroutine_owner(co) < 0) return -1;
    if (!low || !high) { errno = EINVAL; return -1; }
    *low = co->runtime.stack_low; *high = co->runtime.stack_high;
    return 0;
}

int wasm_rt_coroutine_destroy(wasm_rt_coroutine *co) {
    if (coroutine_owner(co) < 0) return -1;
    if (co->running || (co->started && !co->done)) { errno = EBUSY; return -1; }
    if (n_memory_release((void *)co->runtime.stack_low,
                         co->runtime.stack_high - co->runtime.stack_low) < 0)
        return -1;
    free(co);
    return 0;
}
