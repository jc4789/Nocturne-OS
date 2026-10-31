/* Idle and performance (arch/idle.h).

   An idle CPU waits on a word of its own with MONITOR/MWAIT where the CPU has them (Intel SDM vol.
   3A 11.10.4): a CPU that gives it a task writes the word (cpu_idle_wake) instead of sending an
   IPI. The word is alone on its line, so that nothing else written there wakes the CPU (11.10.5:
   CPUID 5 gives the monitored line; a larger one than the alignment only adds false wakeups). The
   C-states below C1 are entered through the ACPI _CST methods' I/O reads on these CPUs, which the
   kernel does not evaluate yet: MWAIT here asks for C1 (hint 0), the state the LAPIC timer never
   stops in.

   Performance: CPPC on AMD (CPUID 8000_0008h EBX[27]). The MSRs are CPPC_CAP1 C001_02B0h
   (highest 31:24, nominal 23:16, lowest nonlinear 15:8, lowest 7:0), CPPC_ENABLE C001_02B1h and
   CPPC_REQ C001_02B3h (EPP 31:24, desired 23:16, minimum 15:8, maximum 7:0), the registers of ACPI
   6.5 8.4.6.1 in AMD's fixed hardware; their layout is in none of the manuals at hand (the Family
   19h PPR here lists only the CPUID bit). A CPU that refuses the enable write has no CPPC, whatever
   CPUID says (a hypervisor's). */
#include "kernel.h"
#include "arch/cpu.h"
#include "arch/cpuset.h"
#include "arch/idle.h"
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
#define MSR_CPPC_CAP1   0xC00102B0u
#define MSR_CPPC_ENABLE 0xC00102B1u
#define MSR_CPPC_REQ    0xC00102B3u
#define MSR_MPERF       0xE7u
#define MSR_APERF       0xE8u

static uint8_t preference = CPU_PERF_PREFERENCE_DEFAULT;
static bool cppc, effective; /* the first CPU's findings: every CPU has the same (cpu_features_same) */
static bool probed;

/* This CPU's request: anything from its lowest nonlinear performance (ACPI 6.5 8.4.6.1.1.4, the
   lowest at which a lower level still saves power out of proportion) to its highest, the hardware
   choosing (desired 0) by the preference. */
static void cppc_request(void) {
    uint64_t caps = rdmsr(MSR_CPPC_CAP1);
    unsigned highest = (caps >> 24) & 0xFF, nonlinear = (caps >> 8) & 0xFF;
    if (!highest) return;
    if (nonlinear > highest) nonlinear = highest;
    uint8_t epp = __atomic_load_n(&preference, __ATOMIC_RELAXED);
    wrmsr(MSR_CPPC_REQ, (uint64_t)highest | (uint64_t)nonlinear << 8 | (uint64_t)epp << 24);
}

void cpu_perf_setup(void) {
    bool here = cpu_features.vendor == CPU_VENDOR_AMD && cpu_features.cppc && wrmsr_checked(MSR_CPPC_ENABLE, 1);
    /* the counters' ratio is all that is read: both may start from 0 (APM 17.3) */
    bool counters = cpu_features.aperf_mperf && wrmsr_checked(MSR_MPERF, 0) && wrmsr_checked(MSR_APERF, 0);
    if (!probed) {
        probed = true;
        cppc = here;
        effective = counters;
        if (cppc) {
            uint64_t caps = rdmsr(MSR_CPPC_CAP1);
            kprintf("cpu: CPPC highest %u nominal %u lowest nonlinear %u lowest %u, preference %u\n",
                    (unsigned)(caps >> 24) & 0xFF, (unsigned)(caps >> 16) & 0xFF, (unsigned)(caps >> 8) & 0xFF,
                    (unsigned)caps & 0xFF, preference);
        } else {
            kprintf("cpu: no CPPC: the firmware's performance policy stays\n");
        }
    }
    if (cppc && here) cppc_request();
    this_cpu_ptr(window)->tsc = 0; /* the counters started again: a window from 0 */
}

static void perf_apply(void *unused) {
    (void)unused;
    uint64_t f = irq_save();
    cppc_request();
    irq_restore(f);
}

void cpu_perf_set_policy(uint8_t value) {
    __atomic_store_n(&preference, value, __ATOMIC_RELAXED);
    if (!cppc) return;
    /* each CPU writes its own request, on a thread bound to it; one that starts later reads the
       new value in its cpu_setup */
    cpuset_each(cpu, cpu_online_set(), topology.cpus)
        if (!kthread_create_on("perf", perf_apply, NULL, cpu)) kprintf("cpu: cpu%u keeps its performance request\n", cpu);
}

/* ---- the effective clock ----
   MPERF counts at the P0 frequency while the core is in C0, the TSC at the P0 frequency always
   (Family 19h PPR MSR0000_00E7, MSR0000_0010): APERF/MPERF times the TSC's rate is the clock it
   ran at, MPERF/TSC the share of the window it was awake. */
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
