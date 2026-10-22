/* CPU performance state. Firmware hands the OS each core at whatever clock it
   chose for booting; nothing raises it again unless the OS asks. On bare metal
   every CPU requests its highest performance here: AMD through CPPC (or the
   P-state control MSR on parts without it), Intel through HWP. Under a
   hypervisor the host owns the clock and nothing is written.

   APERF/MPERF, where present, give the effective clock: MPERF counts at the
   nominal rate while the core is awake and APERF at the actual one. Each CPU
   samples them on its own timer tick (pstate_tick), so the profiler can print
   every CPU's real frequency and awake share. */
#include "kernel.h"
#include "arch/cpu.h"
#include "arch/smp.h"
#include "arch/pstate.h"

#define MSR_MPERF          0xE7
#define MSR_APERF          0xE8
#define MSR_HWP_ENABLE     0x770
#define MSR_HWP_CAPS       0x771
#define MSR_HWP_REQUEST    0x774
#define MSR_AMD_PSTATE_LIM 0xC0010061
#define MSR_AMD_PSTATE_CTL 0xC0010062
#define MSR_AMD_PSTATE_ST  0xC0010063
#define MSR_AMD_CPPC_CAP1  0xC00102B0
#define MSR_AMD_CPPC_EN    0xC00102B1
#define MSR_AMD_CPPC_REQ   0xC00102B3

enum pstate_method { PSTATE_NONE, PSTATE_AMD_CPPC, PSTATE_AMD_LEGACY, PSTATE_INTEL_HWP };

static enum pstate_method method;
static bool have_aperf;
static bool hypervisor;

static struct pstate_sample {
    uint64_t aperf, mperf, tsc;
    uint32_t ticks;
    uint32_t mhz, awake_permille;   /* last completed window */
} samples[CPU_MAX_COUNT];

static void detect(void) {
    uint32_t a, b, c, d, max, ext;
    cpuid(0, 0, &max, &b, &c, &d);
    enum cpu_vendor vendor = cpu_vendor_of(b, c, d);
    cpuid(1, 0, &a, &b, &c, &d);
    hypervisor = c & (1u << 31);
    if (max >= 6) {
        cpuid(6, 0, &a, &b, &c, &d);
        have_aperf = c & 1u;
        if (vendor == CPU_VENDOR_INTEL && (a & (1u << 7))) method = PSTATE_INTEL_HWP;
    }
    cpuid(0x80000000, 0, &ext, &b, &c, &d);
    if (vendor == CPU_VENDOR_AMD) {
        if (ext >= 0x80000008) {
            cpuid(0x80000008, 0, &a, &b, &c, &d);
            if (b & (1u << 27)) method = PSTATE_AMD_CPPC;
        }
        if (method == PSTATE_NONE && ext >= 0x80000007) {
            cpuid(0x80000007, 0, &a, &b, &c, &d);
            if (d & (1u << 7)) method = PSTATE_AMD_LEGACY;
        }
    }
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
    case PSTATE_AMD_LEGACY: {
        unsigned before = rdmsr(MSR_AMD_PSTATE_ST) & 7;
        unsigned best = rdmsr(MSR_AMD_PSTATE_LIM) & 7; /* CurPstateLimit: the fastest allowed */
        wrmsr(MSR_AMD_PSTATE_CTL, best);
        ksnprintf(buf, n, "AMD P-state P%u -> P%u", before, best);
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
    if (!have_aperf || tsc_hz < 1000000) return 0;
    uint64_t a0 = rdmsr(MSR_APERF), m0 = rdmsr(MSR_MPERF), t0 = rdtsc();
    while (rdtsc() - t0 < tsc_hz / 200) pause(); /* 5 ms */
    uint64_t da = rdmsr(MSR_APERF) - a0, dm = rdmsr(MSR_MPERF) - m0;
    return dm ? (unsigned)(tsc_hz / 1000000 * da / dm) : 0;
}

void pstate_init_bsp(void) {
    detect();
    char buf[96];
    unsigned before = measure_mhz();
    const char *what = raise(buf, sizeof buf);
    unsigned after = measure_mhz();
    if (have_aperf && !hypervisor)
        kprintf("pstate: %s; cpu0 %u MHz -> %u MHz (TSC %lu MHz)\n", what, before, after, tsc_hz / 1000000);
    else
        kprintf("pstate: %s\n", what);
}

void pstate_init_ap(void) {
    char buf[96];
    (void)raise(buf, sizeof buf);
}

bool pstate_sampling(void) { return have_aperf && !hypervisor; }

void pstate_tick(void) {
    unsigned cpu = cpu_current_index();
    if (cpu >= CPU_MAX_COUNT) return;
    struct pstate_sample *s = &samples[cpu];
    if (++s->ticks < 1000) return;
    s->ticks = 0;
    uint64_t a = rdmsr(MSR_APERF), m = rdmsr(MSR_MPERF), t = rdtsc();
    if (s->tsc) {
        uint64_t da = a - s->aperf, dm = m - s->mperf, dt = t - s->tsc;
        if (dm) __atomic_store_n(&s->mhz, (uint32_t)(tsc_hz / 1000000 * da / dm), __ATOMIC_RELAXED);
        if (dt) __atomic_store_n(&s->awake_permille, (uint32_t)(dm * 1000 / dt), __ATOMIC_RELAXED);
    }
    s->aperf = a; s->mperf = m; s->tsc = t;
}

bool pstate_cpu(unsigned cpu, unsigned *mhz, unsigned *awake_permille) {
    if (cpu >= CPU_MAX_COUNT || !pstate_sampling()) return false;
    *mhz = __atomic_load_n(&samples[cpu].mhz, __ATOMIC_RELAXED);
    *awake_permille = __atomic_load_n(&samples[cpu].awake_permille, __ATOMIC_RELAXED);
    return *mhz != 0;
}
