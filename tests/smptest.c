/* 独自n_app_launch/n_channel_pairによる2 appの同時進行とAP間XSTATE/stack保存。 */
#include <nocturne.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define SAMPLES 40
struct sample { unsigned long begin, end; unsigned cpu_begin, cpu_end; };
struct report { unsigned magic, count, xstate, stack, gs_tls; unsigned long value; struct sample s[SAMPLES]; };
static unsigned long ticks(void) {
    unsigned a, d;
    __asm__ volatile("rdtsc" : "=a"(a), "=d"(d));
    return ((unsigned long)d << 32) | a;
}
/* SYS_SLEEP's result register: 0 unless the continuation lost the user context. */
static long sleep_result(unsigned long ms) { return __syscall(SYS_SLEEP, (long)ms, 0, 0, 0, 0); }
static unsigned apic_id(void) {
    unsigned a, b, c, d;
    __asm__ volatile("cpuid" : "=a"(a), "=b"(b), "=c"(c), "=d"(d) : "a"(1u), "c"(0u));
    return b >> 24;
}
static int gs_tls_probe(const struct n_cpuinfo *info, unsigned seed) {
    unsigned short saved;
    __asm__ volatile("movw %%gs, %0" : "=r"(saved));
    void *previous = thread_local_get(7);
    void *sentinel = (void *)(uintptr_t)(seed + 1);
    n_result pid = n_app_self(), tid = thread_id(); int ok = 1;
    thread_local_set(7, sentinel);
    /* User code can reload GS independently of the kernel base. Both selectors
       are valid zero-base user states; FS must still carry this task's TLS. */
    static const unsigned short selectors[] = {0, 0x1B};
    for (unsigned i = 0; i < sizeof selectors / sizeof selectors[0]; i++) {
        __asm__ volatile("movw %0, %%gs" : : "r"(selectors[i]) : "memory");
        if (n_app_self() != pid || thread_id() != tid || cpu_index() >= info->detected_cpus ||
            thread_local_get(7) != sentinel) ok = 0;
        /* Unlike the AP's read-only identity calls, sleep retires the AP user
           context and executes the syscall continuation on the BSP. */
        if (sleep_result(1) || n_app_self() != pid || thread_id() != tid ||
            cpu_index() >= info->detected_cpus || thread_local_get(7) != sentinel) ok = 0;
    }
    __asm__ volatile("movw %0, %%gs" : : "r"(saved) : "memory");
    thread_local_set(7, previous);
    return ok;
}
static __attribute__((noinline)) int stack_probe(void) {
    volatile unsigned char buffer[96 * 1024];
    for (unsigned i = 0; i < sizeof buffer; i += 4096) buffer[i] = (unsigned char)(i / 4096 + 17);
    for (unsigned i = 0; i < sizeof buffer; i += 4096)
        if (buffer[i] != (unsigned char)(i / 4096 + 17)) return 0;
    return 1;
}
static __attribute__((noinline)) long avx_sleep(const unsigned long *in, unsigned long *out) {
    long result;
    __asm__ volatile(".byte 0xc5,0xfe,0x6f,0x06\nint $0x80\n"
                     ".byte 0xc5,0xfe,0x7f,0x02\n.byte 0xc5,0xf8,0x77\n"
                     : "=a"(result) : "0"((long)SYS_SLEEP), "D"(2L), "S"(in), "d"(out) : "memory", "cc");
    return result;
}
static int child(unsigned long start, unsigned seed) {
    struct report r;
    memset(&r, 0, sizeof r);
    struct n_cpuinfo info;
    if (cpu_info(&info)) return 2;
    r.gs_tls = gs_tls_probe(&info, seed);
    r.stack = stack_probe(); /* top64KiB外の本物user page faultをBSPへ移す */
    r.xstate = 1;
    for (unsigned round = 0; round < 4; round++) {
        if (info.flags & N_CPU_AVX) {
            unsigned long in[4] = {seed + round, seed * 17UL, 0x123456789abcdef0UL + seed,
                                   0xfedcba9876543210UL - round}, out[4];
            if (avx_sleep(in, out) || memcmp(in, out, sizeof in)) r.xstate = 0;
        } else if (sleep_result(2)) r.xstate = 0;
    }
    unsigned long t0 = ticks();
    if (sleep_result(20)) return 2;
    unsigned long per_ms = (ticks() - t0) / 20;
    if (!per_ms) return 2;
    unsigned long now = uptime_ms();
    if (now < start && sleep_result((unsigned)(start - now))) return 2;
    volatile unsigned long value = seed;
    r.magic = 0x534d5031;
    for (unsigned i = 0; i < SAMPLES; i++) {
        struct sample *s = &r.s[i];
        s->cpu_begin = apic_id();
        s->begin = ticks();
        do {
            for (unsigned n = 0; n < 4096; n++) value = value * 6364136223846793005UL + 1442695040888963407UL;
        } while (ticks() - s->begin < per_ms * 5);
        s->end = ticks();
        s->cpu_end = apic_id();
        r.count++;
    }
    r.value = value;
    return n_channel_send(1, &r, sizeof r, -1) == (n_result)sizeof r && r.stack && r.xstate && r.gs_tls ? 0 : 2;
}
/* Starts self with args. The child's output (or, with on_log, its log) is a message channel whose
   parent end is returned in *reader. */
static n_handle start_child(const char *self, const char *const *args, uint32_t argc, bool on_log, n_handle *reader) {
    n_handle c[2];
    if (n_channel_pair(c) < 0) return -1;
    int32_t handles[3] = {-1, on_log ? -1 : c[1], on_log ? c[1] : -1};
    struct n_launch l = {.arguments = args, .argument_count = argc, .handles = handles, .handle_count = 3};
    n_handle app = n_app_launch(self, &l);
    n_handle_close(c[1]);
    if (app < 0) { n_handle_close(c[0]); return app; }
    *reader = c[0];
    return app;
}
static int finish_child(n_handle app, int *status) {
    n_result r = n_app_result(app, -1, status);
    n_handle_close(app);
    return r >= 0;
}
int main(int argc, char **argv) {
    if (argc == 4 && !strcmp(argv[1], "--child")) return child(strtoul(argv[2], NULL, 10), (unsigned)atoi(argv[3]));
    if (argc == 2 && !strcmp(argv[1], "--fault")) {
        *(volatile unsigned char *)0x12345000UL = 1;
        return 3;
    }
    if (argc == 2 && !strcmp(argv[1], "--kill")) {
        char ready = 'K';
        if (n_channel_send(1, &ready, 1, -1) != 1) return 3;
        for (;;) __asm__ volatile("pause" : : : "memory");
    }
    struct n_cpuinfo info;
    if (cpu_info(&info)) return 1;
    n_handle app[2] = {-1, -1}, reader[2] = {-1, -1};
    int status;
    struct report r[2];
    memset(r, 0, sizeof r);
    char start[32];
    snprintf(start, sizeof start, "%lu", (unsigned long)uptime_ms() + 200);
    for (unsigned i = 0; i < 2; i++) {
        const char *args[4] = {argv[0], "--child", start, i ? "29" : "11"};
        app[i] = start_child(argv[0], args, 4, false, &reader[i]);
        if (app[i] < 0) {
            if (i && app[0] >= 0) { n_app_stop(app[0], 0); finish_child(app[0], &status); n_handle_close(reader[0]); }
            return 1;
        }
    }
    int ok = 1;
    for (unsigned i = 0; i < 2; i++) {
        if (n_channel_receive(reader[i], &r[i], sizeof r[i], N_CHANNEL_WAIT, NULL) != (n_result)sizeof r[i]) ok = 0;
        n_handle_close(reader[i]);
        if (!finish_child(app[i], &status) || status) ok = 0;
        if (r[i].magic != 0x534d5031 || r[i].count != SAMPLES || !r[i].stack || !r[i].xstate || !r[i].gs_tls) ok = 0;
    }
    unsigned overlap = 0, cpu_a = 0, cpu_b = 0;
    unsigned first_a = 0, first_b = 0;
    if (ok) for (unsigned a = 0; a < SAMPLES; a++) for (unsigned b = 0; b < SAMPLES; b++) {
        struct sample *x = &r[0].s[a], *y = &r[1].s[b];
        if (x->cpu_begin == x->cpu_end && y->cpu_begin == y->cpu_end && x->cpu_begin != y->cpu_begin &&
            x->begin < y->end && y->begin < x->end) {
            overlap++;
            if (overlap == 1) { first_a = a; first_b = b; }
            cpu_a = x->cpu_begin;
            cpu_b = y->cpu_begin;
        }
    }
    if (info.scheduler_cpus > 1 && !overlap) ok = 0;
    if (info.scheduler_cpus == 1 && overlap) ok = 0;
    printf("smptest: user2 scheduler=%u apic=%u/%u overlap=%u XSTATE/stack=%s\n", info.scheduler_cpus,
           cpu_a, cpu_b, overlap, ok ? "PASS" : "FAIL");
    printf("smptest: GS-selector/FS-TLS/CPU-index %s\n", r[0].gs_tls && r[1].gs_tls ? "PASS" : "FAIL");
    if (overlap) printf("smptest: raw overlap cpu%u [%lu,%lu] cpu%u [%lu,%lu]\n",
        r[0].s[first_a].cpu_begin, r[0].s[first_a].begin, r[0].s[first_a].end,
        r[1].s[first_b].cpu_begin, r[1].s[first_b].begin, r[1].s[first_b].end);
    /* CR2はBSPがfault処理。stderrの本物addrとexit状態を要求する。 */
    const char *fargs[2] = {argv[0], "--fault"};
    n_handle fault, fapp = start_child(argv[0], fargs, 2, true, &fault);
    if (fapp < 0) return 1;
    char text[512];
    size_t used = 0;
    for (;;) { /* the kernel reports the fault on the child's log; it closes when the child is gone */
        n_result got = n_channel_receive(fault, text + used, sizeof text - 1 - used, N_CHANNEL_WAIT, NULL);
        if (got < 0) break;
        used += (size_t)got;
    }
    text[used] = 0;
    n_handle_close(fault);
    if (!finish_child(fapp, &status) || status != 142 || !strstr(text, "12345000")) ok = 0;
    const char *kargs[2] = {argv[0], "--kill"};
    n_handle kreader, kapp = start_child(argv[0], kargs, 2, false, &kreader);
    if (kapp < 0) return 1;
    char ready = 0;
    if (n_channel_receive(kreader, &ready, 1, N_CHANNEL_WAIT, NULL) != 1 || ready != 'K' || sleep_result(20) ||
        n_app_stop(kapp, 0) < 0 || !finish_child(kapp, &status) || status != 130) ok = 0;
    n_handle_close(kreader);
    printf("smptest: fault/CR2/kill/retire %s\n", ok ? "PASS" : "FAIL");
    printf("smptest: %s\n", ok ? "ok" : "FAIL");
    return !ok;
}
