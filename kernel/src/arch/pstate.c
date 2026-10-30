/* CPU performance state. Firmware hands the OS each core at whatever clock it
   chose for booting; nothing raises it again unless the OS asks. On bare metal
   every CPU requests its highest performance here: AMD through CPPC, Intel
   through HWP. Under a
   hypervisor the host owns the clock and nothing is written.

   APERF/MPERF, where present, give the effective clock: MPERF counts at the
   nominal rate while the core is awake and APERF at the actual one. While the
   profiler samples (sys/prof.c), each CPU closes a window of them every second
   (pstate_sample), so it can print every CPU's real frequency and awake share. */
#include "kernel.h"
#include "arch/cpu.h"
#include "arch/smp.h"
#include "arch/pstate.h"
#include "sys/clock.h"
#include "vm/vm.h"

#define MSR_MPERF          0xE7
#define MSR_APERF          0xE8
#define MSR_HWP_ENABLE     0x770
#define MSR_HWP_CAPS       0x771
#define MSR_HWP_REQUEST    0x774
#define MSR_AMD_CPPC_CAP1  0xC00102B0
#define MSR_AMD_CPPC_EN    0xC00102B1
#define MSR_AMD_CPPC_REQ   0xC00102B3

enum pstate_method { PSTATE_NONE, PSTATE_AMD_CPPC, PSTATE_INTEL_HWP };

static enum pstate_method method;
static bool have_aperf;
static bool hypervisor;

PERCPU static struct pstate_sample {
    uint64_t aperf, mperf, tsc;
    uint64_t at;                    /* clock_now_ns when the window began */
    uint32_t mhz, awake_permille;   /* last completed window */
} sample;

static void detect(void) {
    hypervisor = vm_hosted();
    have_aperf = cpu_features.aperf_mperf;
    if (cpu_features.vendor == CPU_VENDOR_AMD && cpu_features.cppc) method = PSTATE_AMD_CPPC;
    if (cpu_features.vendor == CPU_VENDOR_INTEL && cpu_features.intel_hwp) method = PSTATE_INTEL_HWP;
    if (hypervisor) method = PSTATE_NONE;
}

/* Requests the highest performance on the calling CPU. Returns a short
   description for the boot log. */
static const char *raise(char *buf, size_t n) {
    switch (method) {
    case PSTATE_AMD_CPPC: {
        uint64_t caps = rdmsr(MSR_AMD_CPPC_CAP1);
        unsigned highest = (caps >> 24) & 0xFF, nominal = (caps >> 16) & 0xFF, lowest = caps & 0xFF;
        if (!highest) return "AMD CPPC without capabilities: left as is";
        if (nominal > highest || !nominal) nominal = highest;
        wrmsr(MSR_AMD_CPPC_EN, 1);
        /* max = highest (boost), min = nominal (never below the base clock
           while awake), desired 0 = hardware picks, EPP 0 = performance */
        wrmsr(MSR_AMD_CPPC_REQ, (uint64_t)highest | (uint64_t)nominal << 8);
        ksnprintf(buf, n, "AMD CPPC highest %u nominal %u lowest %u", highest, nominal, lowest);
        return buf;
    }
    case PSTATE_INTEL_HWP: {
        wrmsr(MSR_HWP_ENABLE, 1);
        uint64_t caps = rdmsr(MSR_HWP_CAPS);
        unsigned highest = caps & 0xFF, guaranteed = (caps >> 8) & 0xFF;
        if (!highest) return "Intel HWP without capabilities: left as is";
        if (guaranteed > highest || !guaranteed) guaranteed = highest;
        wrmsr(MSR_HWP_REQUEST, (uint64_t)guaranteed | (uint64_t)highest << 8);
        ksnprintf(buf, n, "Intel HWP highest %u guaranteed %u", highest, guaranteed);
        return buf;
    }
    default:
        return hypervisor ? "hypervisor owns the clock" : "no P-state control found";
    }
}

/* Effective clock of the calling CPU over a short busy interval. */
static unsigned measure_mhz(void) {
    uint64_t hz = clock_tsc_hz();
    if (!have_aperf || hz < 1000000) return 0;
    uint64_t a0 = rdmsr(MSR_APERF), m0 = rdmsr(MSR_MPERF);
    for (uint64_t until = clock_after(5 * NS_PER_MS); clock_now_ns() < until;) pause();
    uint64_t da = rdmsr(MSR_APERF) - a0, dm = rdmsr(MSR_MPERF) - m0;
    return dm ? (unsigned)(hz / 1000000 * da / dm) : 0;
}

void pstate_init_bsp(void) {
    detect();
    char buf[96];
    unsigned before = measure_mhz();
    const char *what = raise(buf, sizeof buf);
    unsigned after = measure_mhz();
    if (have_aperf && !hypervisor)
        kprintf("pstate: %s; cpu0 %u MHz -> %u MHz (TSC %lu MHz)\n", what, before, after, clock_tsc_hz() / 1000000);
    else
        kprintf("pstate: %s\n", what);
}

void pstate_init_ap(void) {
    char buf[96];
    (void)raise(buf, sizeof buf);
}

bool pstate_sampling(void) { return have_aperf && !hypervisor; }

void pstate_sample(void) {
    struct pstate_sample *s = this_cpu_ptr(sample);
    uint64_t now = clock_now_ns();
    if (s->tsc && now - s->at < NS_PER_S) return;
    s->at = now;
    uint64_t a = rdmsr(MSR_APERF), m = rdmsr(MSR_MPERF), t = rdtsc();
    if (s->tsc) {
        uint64_t da = a - s->aperf, dm = m - s->mperf, dt = t - s->tsc;
        if (dm) __atomic_store_n(&s->mhz, (uint32_t)(clock_tsc_hz() / 1000000 * da / dm), __ATOMIC_RELAXED);
        if (dt) __atomic_store_n(&s->awake_permille, (uint32_t)(dm * 1000 / dt), __ATOMIC_RELAXED);
    }
    s->aperf = a; s->mperf = m; s->tsc = t;
}

bool pstate_cpu(unsigned cpu, unsigned *mhz, unsigned *awake_permille) {
    if (cpu >= topology.cpus || !pstate_sampling()) return false;
    *mhz = __atomic_load_n(&per_cpu(sample, cpu).mhz, __ATOMIC_RELAXED);
    *awake_permille = __atomic_load_n(&per_cpu(sample, cpu).awake_permille, __ATOMIC_RELAXED);
    return *mhz != 0;
}
