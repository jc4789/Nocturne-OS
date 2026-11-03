/* Idle and performance (arch/idle.h).

   An idle CPU waits on a word of its own with MONITOR/MWAIT where the CPU has them (Intel SDM vol.
   3A 11.10.4): a CPU that gives it a task writes the word (cpu_idle_wake) instead of sending an
   IPI. The word is alone on its line, so that nothing else written there wakes the CPU (11.10.5:
   CPUID 5 gives the monitored line; a larger one than the alignment only adds false wakeups). The
   C-states below C1 are what the ACPI _CST methods name, which the kernel does not evaluate yet:
   MWAIT here asks for C1 (hint 0), the state the LAPIC timer never stops in.

   Performance: the hardware chooses within a range the kernel asks for, by a preference. Each
   maker has its own registers for the same request (struct cpu_perf_way, arch/cpu_maker.h). A CPU
   that refuses the enable write has none, whatever CPUID says (a hypervisor's). */
#include "kernel.h"
#include "arch/cpu.h"
#include "arch/cpu_maker.h"
#include "arch/cpuset.h"
#include "arch/idle.h"
#include "sys/settings.h"
#include "arch/smp.h"
#include "sys/clock.h"
#include "sys/thread.h"

enum { IDLE_RUNNING, IDLE_WATCHING, IDLE_WOKEN };

PERCPU static struct perf_window {
    uint64_t aperf, mperf, tsc;
    uint64_t at;                  /* clock_now_ns when the window began */
    uint32_t mhz, awake_permille; /* the last window's */
} window;

PERCPU static struct {
    uint32_t state;
} __attribute__((aligned(64))) idle_word;

static inline void monitor(const void *address) {
    __asm__ volatile("monitor" : : "a"(address), "c"(0u), "d"(0u) : "memory");
}

void cpu_idle(void) {
    if (!cpu_features.mwait) {
        __asm__ volatile("sti; hlt; cli" : : : "memory");
        return;
    }
    uint32_t *word = &this_cpu_ptr(idle_word)->state;
    __atomic_store_n(word, IDLE_WATCHING, __ATOMIC_SEQ_CST);
    monitor(word);
    /* a write between the store and MONITOR armed nothing: looked at after it (11.10.4) */
    if (__atomic_load_n(word, __ATOMIC_SEQ_CST) == IDLE_WATCHING)
        /* STI's shadow covers MWAIT: an interrupt pending since cli wakes it, and is taken */
        __asm__ volatile("sti; mwait; cli" : : "a"(0u), "c"(0u) : "memory");
    __atomic_store_n(word, IDLE_RUNNING, __ATOMIC_SEQ_CST);
}

bool cpu_idle_wake(unsigned cpu) {
    if (!cpu_features.mwait) return false;
    uint32_t watching = IDLE_WATCHING;
    return __atomic_compare_exchange_n(&per_cpu(idle_word, cpu).state, &watching, IDLE_WOKEN, false,
                                       __ATOMIC_SEQ_CST, __ATOMIC_RELAXED);
}

/* ---- performance ---- */
#define MSR_MPERF       0xE7u
#define MSR_APERF       0xE8u

static uint8_t preference = CPU_PERF_PREFERENCE_DEFAULT;
static const struct cpu_perf_way *way; /* the first CPU's finding: every CPU has the same (cpu_features_same) */
static bool effective, probed;

/* This CPU's request: its range, the hardware choosing within it (desired 0) by the preference. */
static void perf_request(void) {
    struct cpu_perf_caps c;
    way->caps(&c);
    if (!c.highest) return;
    if (c.floor > c.highest) c.floor = c.highest;
    way->request(&c, __atomic_load_n(&preference, __ATOMIC_RELAXED));
}

void cpu_perf_setup(void) {
    const struct cpu_maker *maker = cpu_features.maker;
    const struct cpu_perf_way *w = maker->perf_way ? maker->perf_way() : NULL;
    bool here = w && wrmsr_checked(w->enable_msr, 1);
    /* the counters' ratio is all that is read: both may start from 0 (APM 17.3) */
    bool counters = cpu_features.aperf_mperf && wrmsr_checked(MSR_MPERF, 0) && wrmsr_checked(MSR_APERF, 0);
    if (!probed) {
        probed = true;
        way = here ? w : NULL;
        effective = counters;
        if (way) {
            struct cpu_perf_caps c;
            way->caps(&c);
            kprintf("cpu: %s highest %u nominal %u floor %u lowest %u, preference %u\n", way->name, c.highest, c.nominal,
                    c.floor, c.lowest, preference);
        } else {
            kprintf("cpu: no way to ask for performance: the firmware's policy stays\n");
        }
    }
    if (way && here) perf_request();
    this_cpu_ptr(window)->tsc = 0; /* the counters started again: a window from 0 */
}

static void perf_apply(void *unused) {
    (void)unused;
    uint64_t f = irq_save();
    perf_request();
    irq_restore(f);
}

int perf_preference_setting_put(uint32_t key, const void *value) {
    int32_t v;
    memcpy(&v, value, sizeof v);
    cpu_perf_set_policy((uint8_t)v);
    settings_changed(N_DOMAIN_POWER);
    return 0;
}

int64_t perf_preference_setting_get(uint32_t key, void *out, size_t cap) {
    return setting_int(out, cap, __atomic_load_n(&preference, __ATOMIC_RELAXED));
}

void cpu_perf_set_policy(uint8_t value) {
    __atomic_store_n(&preference, value, __ATOMIC_RELAXED);
    if (!way) return;
    /* each CPU writes its own request, on a thread bound to it; one that starts later reads the
       new value in its cpu_setup */
    cpuset_each(cpu, cpu_online_set(), topology.cpus)
        if (!kthread_create_on("perf", perf_apply, NULL, cpu)) kprintf("cpu: cpu%u keeps its performance request\n", cpu);
}

/* ---- the effective clock ----
   MPERF counts at a fixed frequency while the core is in C0 (SDM vol. 3B 17.2), on AMD's the P0
   frequency, the TSC's (PPR 57238 MSR0000_00E7, MSR0000_0010): APERF/MPERF times the TSC's rate is
   the clock it ran at, MPERF/TSC the share of the window it was awake. Intel's manual defines only
   the ratio, and does not tie MPERF's frequency to the TSC's, so where they differ these two
   numbers are off by that much. */
bool cpu_perf_sampling(void) { return effective; }

void cpu_perf_sample(void) {
    struct perf_window *w = this_cpu_ptr(window);
    uint64_t now = clock_now_ns();
    if (w->tsc && now - w->at < NS_PER_S) return;
    w->at = now;
    uint64_t a = rdmsr(MSR_APERF), m = rdmsr(MSR_MPERF), t = rdtsc();
    if (w->tsc) {
        uint64_t da = a - w->aperf, dm = m - w->mperf, dt = t - w->tsc;
        if (dm) __atomic_store_n(&w->mhz, (uint32_t)(clock_tsc_hz() / 1000000 * da / dm), __ATOMIC_RELAXED);
        if (dt) __atomic_store_n(&w->awake_permille, (uint32_t)(dm * 1000 / dt), __ATOMIC_RELAXED);
    }
    w->aperf = a;
    w->mperf = m;
    w->tsc = t;
}

bool cpu_perf_of(unsigned cpu, unsigned *mhz, unsigned *awake_permille) {
    if (cpu >= topology.cpus || !effective) return false;
    *mhz = __atomic_load_n(&per_cpu(window, cpu).mhz, __ATOMIC_RELAXED);
    *awake_permille = __atomic_load_n(&per_cpu(window, cpu).awake_permille, __ATOMIC_RELAXED);
    return *mhz != 0;
}
