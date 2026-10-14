/* 実NocturneでTinyCCのC回復境界、取消後の再利用、実OOMを確認する。 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <limits.h>
#include <nocturne.h>
#include "../third_party/tinycc/libtcc.h"
#include "../ports/wabt/include/wasm-rt-nocturne.h"

static unsigned passed, failed;
static unsigned long allocations, fail_at, entry_allocations;
static unsigned polls, cancel_at;
#define DISPATCH_SOURCE \
    "void dispatch(void*p,unsigned i,const void*a,void*r){*(int*)r=answer();}"
static const char source[] = "int answer(void){return 6*7;}" DISPATCH_SOURCE;

static void check(int condition, const char *name) {
    printf("%s tccguardtest: %s\n", condition ? "PASS" : "FAIL", name);
    if (condition) ++passed; else ++failed;
}
static void *injected_reallocator(void *ptr, unsigned long bytes) {
    if (!bytes) { free(ptr); return NULL; }
    ++allocations;
    if (fail_at && allocations == fail_at) return NULL;
    return realloc(ptr, bytes);
}
static int compiler_poll(void *opaque) {
    (void)opaque;
    if (!polls) entry_allocations = allocations;
    ++polls;
    return cancel_at && polls >= cancel_at;
}
struct invocation {
    void (*invoke)(struct wasm_host_instance*, uint32_t,
                   const union wasm_host_value*, union wasm_host_value*);
    int result;
};
static void invoke_answer(void *opaque) {
    struct invocation *call = opaque;
    wasm_rt_native_invoke(call->invoke, NULL, 0, NULL,
                         (union wasm_host_value *)&call->result);
}
static int run_answer(TCCState *compiler, int *result) {
    struct invocation call = {tcc_get_symbol(compiler, "dispatch"), 0};
    if (!call.invoke) return -1;
    int status = wasm_rt_run(invoke_answer, &call);
    *result = call.result;
    return status;
}
static int execute_answer(TCCState *compiler, int expected) {
    int result;
    return run_answer(compiler, &result) == 0 && result == expected;
}
static int ordinary_success(void) {
    TCCState *compiler = NULL;
    int status = wasm_rt_compile_tcc(&compiler, source, NULL, NULL, NULL);
    int ok = status == 0 && compiler && execute_answer(compiler, 6 * 7);
    tcc_delete(compiler);
    return ok;
}
static void entry_cancellation(void) {
    check(ordinary_success(), "baseline native compile/run");
    TCCState *compiler = NULL;
    polls = 0; cancel_at = 1;
    int status = wasm_rt_compile_tcc(&compiler, source, NULL, compiler_poll, NULL);
    check(status == -1 && !compiler && polls == cancel_at,
          "entry cancellation after a completed parser");
    cancel_at = 0;
    check(ordinary_success(), "normal compile/run after entry cancellation");
}
static void allocation_failures(void) {
    TCCState *compiler = NULL;
    allocations = fail_at = entry_allocations = 0;
    polls = cancel_at = 0;
    tcc_set_realloc(injected_reallocator);
    int status = wasm_rt_compile_tcc(&compiler, source, NULL, compiler_poll, NULL);
    unsigned long total = allocations, entry = entry_allocations;
    tcc_set_realloc(NULL);
    check(status == 0 && compiler && execute_answer(compiler, 6 * 7),
          "allocation-counted baseline");
    tcc_delete(compiler);
    if (status != 0 || !entry || total <= entry) {
        check(0, "actual setup/parser/relocation allocation boundaries");
        return;
    }
    /* All indices name allocations observed above: state creation, end of
       setup, first parser allocation, parser body, and final relocation work.
       They are failure-injection samples, not accepted-source limits. */
    const unsigned long points[] = {1, entry, entry + 1,
                                   entry + (total - entry) / 2, total};
    for (size_t i = 0; i < sizeof points / sizeof *points; ++i) {
        allocations = 0; fail_at = points[i]; polls = 0;
        tcc_set_realloc(injected_reallocator);
        compiler = NULL;
        status = wasm_rt_compile_tcc(&compiler, source, NULL, compiler_poll, NULL);
        tcc_set_realloc(NULL); /* Restore the process's default allocator. */
        printf("tccguardtest: allocation failure %lu/%lu (entry=%lu) returned %d\n",
               points[i], total, entry, status);
        check(status == -2 && !compiler, "actual allocation failure is OOM");
        tcc_delete(compiler);
        fail_at = 0;
        check(ordinary_success(), "normal compile/run after restoring allocator");
    }
}
static char *single_function(size_t statements) {
    const char prefix[] = "int answer(void){unsigned x=0;";
    const char statement[] = "x+=1;";
    const char suffix[] = "return x;}" DISPATCH_SOURCE;
    const size_t fixed = sizeof prefix - 1 + sizeof suffix;
    if (statements > (SIZE_MAX - fixed) / (sizeof statement - 1)) return NULL;
    char *text = malloc(fixed + statements * (sizeof statement - 1));
    if (!text) return NULL;
    char *at = text;
    memcpy(at, prefix, sizeof prefix - 1); at += sizeof prefix - 1;
    for (size_t i = 0; i < statements; ++i) {
        memcpy(at, statement, sizeof statement - 1); at += sizeof statement - 1;
    }
    memcpy(at, suffix, sizeof suffix);
    return text;
}
static void function_cancellation(void) {
    /* Calibrate against actual lexer callback work, rather than copying an
       internal compiler quantum or imposing a source-size ceiling. */
    size_t statements = 1;
    char *text = NULL;
    for (;;) {
        free(text); text = single_function(statements);
        if (!text) { check(0, "allocate single-function probe"); return; }
        TCCState *compiler = NULL;
        polls = cancel_at = 0;
        int status = wasm_rt_compile_tcc(&compiler, text, NULL, compiler_poll, NULL);
        int ok = status == 0 && compiler &&
                 execute_answer(compiler, (int)(unsigned)statements);
        tcc_delete(compiler);
        if (!ok) { check(0, "single-function normal compile/run"); free(text); return; }
        if (polls > 1) break;
        if (statements > SIZE_MAX / 2) {
            check(0, "representable single-function probe"); free(text); return;
        }
        statements *= 2;
    }
    printf("tccguardtest: single-function statements=%zu, normal callbacks=%u\n",
           statements, polls);
    TCCState *compiler = NULL;
    polls = 0; cancel_at = 2; /* Entry is first; cancel inside this function. */
    int status = wasm_rt_compile_tcc(&compiler, text, NULL, compiler_poll, NULL);
    check(status == -1 && !compiler && polls == cancel_at,
          "lexer work cancellation inside one function");
    tcc_delete(compiler); free(text); cancel_at = 0;
    check(ordinary_success(), "normal compile/run after function cancellation");
}
static void stack_boundaries(void) {
    uintptr_t low, high;
    if (thread_stack_bounds(&low, &high) < 0 || low >= high) {
        check(0, "obtain actual native stack reservation"); return;
    }
    /* A single C frame equal to the entire actual thread reservation cannot
       fit below the live caller. It must trap before push/sub touches it. */
    const char format[] = "int answer(void){volatile char frame[%zu];"
                          "frame[0]=1;return frame[0];}" DISPATCH_SOURCE;
    size_t capacity = sizeof format + sizeof(size_t) * CHAR_BIT;
    char *text = malloc(capacity);
    if (!text) { check(0, "allocate actual-stack boundary source"); return; }
    snprintf(text, capacity, format, (size_t)(high - low));
    TCCState *compiler = NULL;
    int status = wasm_rt_compile_tcc(&compiler, text, NULL, NULL, NULL), result;
    check(status == 0 && compiler && run_answer(compiler, &result) > 0,
          "actual reservation-sized frame traps before stack write");
    tcc_delete(compiler); free(text);
    check(ordinary_success(), "native execution recovers after oversized frame");

    const char recursive[] = "int answer(void){return answer()+1;}" DISPATCH_SOURCE;
    compiler = NULL;
    status = wasm_rt_compile_tcc(&compiler, recursive, NULL, NULL, NULL);
    check(status == 0 && compiler && run_answer(compiler, &result) > 0,
          "recursive native calls trap at actual stack bounds");
    tcc_delete(compiler);
    check(ordinary_success(), "native execution recovers after recursion");
}

extern uintptr_t test_yield_r15(wasm_rt_coroutine*, uintptr_t);
/* Exercise a callee-saved register across the real context switch, while
 * preserving the ordinary C caller's original R15 value. */
__asm__(".text\n.global test_yield_r15\n.type test_yield_r15,@function\n"
        "test_yield_r15:\npush %r15\nmov %rsi,%r15\n"
        "call wasm_rt_coroutine_yield\nmov %r15,%rax\npop %r15\nret\n"
        ".size test_yield_r15,.-test_yield_r15\n");
struct co_probe {
    wasm_rt_coroutine *co;
    TCCState *compiler;
    unsigned phase, errors, polls;
    int nested, cancelled, unwound, trap;
    uintptr_t low, high;
};
enum {
    X87_ROUNDING_MASK = 3u << 10,
    X87_ROUND_DOWN = 1u << 10,
    X87_ROUND_UP = 2u << 10,
    MXCSR_ROUNDING_MASK = 3u << 13,
    MXCSR_ROUND_DOWN = 1u << 13,
    MXCSR_ROUND_UP = 2u << 13,
    MXCSR_STATUS_MASK = (1u << 6) - 1
};
static void fp_read(unsigned *mxcsr, unsigned short *cw) {
    __asm__ volatile("stmxcsr %0" : "=m"(*mxcsr));
    __asm__ volatile("fnstcw %0" : "=m"(*cw));
}
static void fp_write(unsigned mxcsr, unsigned short cw) {
    __asm__ volatile("ldmxcsr %0" : : "m"(mxcsr));
    __asm__ volatile("fldcw %0" : : "m"(cw));
}
static int probe_poll(void *opaque) {
    ++((struct co_probe *)opaque)->polls;
    return 0;
}
static void coroutine_entry(void *opaque);
static void coroutine_body(void *opaque) {
    struct co_probe *p = opaque;
    volatile uintptr_t local = (uintptr_t)p ^ (uintptr_t)&local;
    uintptr_t token = local;
    if ((uintptr_t)&local < p->low || (uintptr_t)&local >= p->high) ++p->errors;
    if (!execute_answer(p->compiler, 6 * 7)) ++p->errors;
    p->phase = 1;
    if (test_yield_r15(p->co, token) != token || local != token) ++p->errors;
    unsigned mxcsr; unsigned short cw;
    fp_read(&mxcsr, &cw);
    unsigned expected_mxcsr = p->nested ? MXCSR_ROUND_UP : MXCSR_ROUND_DOWN;
    unsigned expected_cw = p->nested ? X87_ROUND_UP : X87_ROUND_DOWN;
    if ((mxcsr & MXCSR_ROUNDING_MASK) != expected_mxcsr ||
        (cw & X87_ROUNDING_MASK) != expected_cw) ++p->errors;
    if (!execute_answer(p->compiler, 6 * 7)) ++p->errors;
    if (!p->nested) {
        struct co_probe child = {.compiler=p->compiler, .nested=1};
        child.co = wasm_rt_coroutine_create(coroutine_entry, &child);
        if (!child.co || wasm_rt_coroutine_bounds(child.co, &child.low, &child.high) < 0) {
            ++p->errors;
        } else {
            if (wasm_rt_coroutine_resume(child.co) != WASM_RT_COROUTINE_SUSPENDED ||
                child.phase != 1 || !execute_answer(p->compiler, 6 * 7)) ++p->errors;
            if (wasm_rt_coroutine_destroy(child.co) == 0) ++p->errors;
            child.cancelled = 1;
            if (wasm_rt_coroutine_resume(child.co) != WASM_RT_COROUTINE_DONE ||
                child.errors || !child.unwound || child.trap ||
                wasm_rt_coroutine_destroy(child.co) < 0) ++p->errors;
            fp_read(&mxcsr, &cw);
            if ((mxcsr & MXCSR_ROUNDING_MASK) != MXCSR_ROUND_DOWN ||
                (cw & X87_ROUNDING_MASK) != X87_ROUND_DOWN) ++p->errors;
        }
        p->phase = 2;
        if (test_yield_r15(p->co, token) != token || local != token) ++p->errors;
    }
    if (!p->cancelled || !execute_answer(p->compiler, 6 * 7)) ++p->errors;
    p->unwound = 1;
}
static void coroutine_entry(void *opaque) {
    struct co_probe *p = opaque;
    unsigned mxcsr; unsigned short cw;
    fp_read(&mxcsr, &cw);
    fp_write((mxcsr & ~MXCSR_ROUNDING_MASK) |
             (p->nested ? MXCSR_ROUND_UP : MXCSR_ROUND_DOWN),
             (cw & ~X87_ROUNDING_MASK) |
             (p->nested ? X87_ROUND_UP : X87_ROUND_DOWN));
    wasm_rt_poll_state previous;
    if (wasm_rt_set_poll(probe_poll, p, &previous) < 0) { ++p->errors; return; }
    p->trap = wasm_rt_run(coroutine_body, p);
    wasm_rt_set_poll(previous.callback, previous.opaque, NULL);
    fp_write(mxcsr, cw);
}
static void coroutine_boundaries(void) {
    struct co_probe parent = {0}, main = {0};
    unsigned main_mxcsr; unsigned short main_cw;
    fp_read(&main_mxcsr, &main_cw);
    wasm_rt_poll_state previous;
    if (wasm_rt_set_poll(probe_poll, &main, &previous) < 0) {
        check(0, "initialize main coroutine runtime"); return;
    }
    int compiled = wasm_rt_compile_tcc(&parent.compiler, source, NULL, NULL, NULL);
    parent.co = wasm_rt_coroutine_create(coroutine_entry, &parent);
    if (compiled || !parent.co ||
        wasm_rt_coroutine_bounds(parent.co, &parent.low, &parent.high) < 0) {
        check(0, "allocate coroutine with actual native stack capacity");
        if (parent.co) wasm_rt_coroutine_destroy(parent.co);
        tcc_delete(parent.compiler);
        wasm_rt_set_poll(previous.callback, previous.opaque, NULL);
        return;
    }
    uintptr_t task_low, task_high;
    check(thread_stack_bounds(&task_low, &task_high) == 0 &&
          parent.high - parent.low == task_high - task_low &&
          ((uintptr_t)&parent < parent.low || (uintptr_t)&parent >= parent.high),
          "coroutine has distinct actual-resource-derived stack bounds");
    int status = wasm_rt_coroutine_resume(parent.co);
    check(status == WASM_RT_COROUTINE_SUSPENDED && parent.phase == 1 && !parent.errors,
          "first yield preserves native C stack and guard descriptor");
    unsigned co_polls = parent.polls, main_polls = main.polls;
    unsigned mxcsr; unsigned short cw;
    fp_read(&mxcsr, &cw);
    check((mxcsr & ~MXCSR_STATUS_MASK) == (main_mxcsr & ~MXCSR_STATUS_MASK) && cw == main_cw,
          "yield restores main floating-point control");
    check(execute_answer(parent.compiler, 6 * 7) && parent.polls == co_polls &&
          main.polls > main_polls, "paused coroutine trap/poll/bounds do not leak to main");
    check(wasm_rt_coroutine_destroy(parent.co) < 0,
          "destroy rejects suspended live frames");
    status = wasm_rt_coroutine_resume(parent.co);
    check(status == WASM_RT_COROUTINE_SUSPENDED && parent.phase == 2 && !parent.errors,
          "nested coroutine restores parent TLS, R15, locals and FP control");
    parent.cancelled = 1;
    status = wasm_rt_coroutine_resume(parent.co);
    check(status == WASM_RT_COROUTINE_DONE && parent.unwound && !parent.errors && !parent.trap,
          "cancellation resumes saved frames to ordinary completion");
    check(wasm_rt_coroutine_destroy(parent.co) == 0,
          "completed coroutine mapping is released");
    check(execute_answer(parent.compiler, 6 * 7), "main execution survives coroutine destruction");
    tcc_delete(parent.compiler);
    wasm_rt_set_poll(previous.callback, previous.opaque, NULL);
}
int main(void) {
    entry_cancellation();
    allocation_failures();
    function_cancellation();
    stack_boundaries();
    coroutine_boundaries();
    tcc_set_realloc(NULL);
    printf("tccguardtest: %u passed, %u failed\n", passed, failed);
    return failed ? 1 : 0;
}
