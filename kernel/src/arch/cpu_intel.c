/* Intel's CPUs (arch/cpu_maker.h). */
#include "kernel.h"
#include "arch/apic.h"
#include "arch/cpu.h"
#include "arch/cpu_maker.h"
#include "arch/mca.h"
#include "arch/spin.h"
#include "arch/tsc_rate.h"

/* ---- what only an MSR states ----
   CMCI: MCG_CAP[MCG_CMCI_P], the banks have CTL2 and corrected errors an LVT entry (SDM vol. 3B
   18.3.1.1). Enhanced IBRS: IA32_ARCH_CAPABILITIES[IBRS_ALL], MSR 10AH (SDM vol. 4 Table 2-2), the
   counterpart of AMD's "IBRS always on". And LFENCE is dispatch serializing, always (SDM vol. 2A,
   LFENCE). */
#define MCG_CMCI_P (1ULL << 10)
#define MSR_ARCH_CAPABILITIES 0x10Au
#define ARCH_CAP_IBRS_ALL (1ULL << 1)

static void intel_read_features(void) {
    cpu_features.lfence_serializing = true;
    cpu_features.cmci = rdmsr(MSR_MCG_CAP) & MCG_CMCI_P;
    if (cpu_features.arch_capabilities && (rdmsr(MSR_ARCH_CAPABILITIES) & ARCH_CAP_IBRS_ALL))
        cpu_features.ibrs_always_on = true;
}

/* its banks would have no CTL2 for cmci_on */
static bool intel_same_features(void) { return !cpu_features.cmci || (rdmsr(MSR_MCG_CAP) & MCG_CMCI_P); }

/* ---- the TSC's rate ---- */
static void intel_tsc_stated(struct tsc_rate *r) {
    uint32_t max, a, b, c, d;
    cpuid(0, 0, &max, &b, &c, &d);
    cpuid(0x15, 0, &a, &b, &c, &d);
    tsc_rate_intel(max, a, b, c, r);
}

/* ---- performance ----
   HWP (CPUID 06H EAX[7]; SDM vol. 3B 17.4): IA32_PM_ENABLE 770h (bit 0, set once, for the package;
   the others may be touched only after it), IA32_HWP_CAPABILITIES 771h (lowest 31:24, most efficient
   23:16, guaranteed 15:8, highest 7:0) and IA32_HWP_REQUEST 774h (EPP 31:24 where CPUID 06H EAX[10]
   says, desired 23:16, maximum 15:8, minimum 7:0). The range is the whole of it (17.4.10). */
#define MSR_PM_ENABLE   0x770u
#define MSR_HWP_CAPS    0x771u
#define MSR_HWP_REQUEST 0x774u

static void hwp_caps(struct cpu_perf_caps *c) {
    uint64_t caps = rdmsr(MSR_HWP_CAPS);
    *c = (struct cpu_perf_caps){caps & 0xFF, (caps >> 8) & 0xFF, (caps >> 24) & 0xFF, (caps >> 24) & 0xFF};
}
static void hwp_request(const struct cpu_perf_caps *c, uint8_t preference) {
    wrmsr(MSR_HWP_REQUEST, (uint64_t)c->floor | (uint64_t)c->highest << 8 |
                               (cpu_features.hwp_epp ? (uint64_t)preference << 24 : 0));
}
static const struct cpu_perf_way hwp_way = {"HWP", MSR_PM_ENABLE, hwp_caps, hwp_request};

static const struct cpu_perf_way *intel_perf_way(void) { return cpu_features.hwp ? &hwp_way : NULL; }

/* ---- machine checks ----
   CMCI (SDM vol. 3B 18.3.2.5, 18.5): a bank signals the LVT's CMCI entry when its corrected count
   reaches the threshold in IA32_MCi_CTL2. Sibling threads may share a bank: each CPU in turn claims
   the banks whose CMCI_EN no one set yet and that keep it when written (18.5.2.1), and looks only
   at its own when the interrupt comes, which may reach several. */
#define MSR_MC_CTL2 0x280u
#define CTL2_CMCI_EN (1ULL << 30)
#define CTL2_THRESHOLD 0x7FFFULL
static struct spinlock cmci_lock; /* a bank's claim: the check and the write. A leaf */
PERCPU static uint32_t cmci_banks; /* the banks this CPU claimed */
PERCPU static bool cmci_claimed;   /* it has made its claims */

static enum irq_result cmci_told(void *context) {
    (void)context;
    unsigned banks = mca_banks();
    for (unsigned i = 0; i < banks; i++)
        if (this_cpu(cmci_banks) & (1u << i)) mca_take_corrected(i, "told");
    return IRQ_HANDLED;
}

/* 18.5.2.1: the claims, the LVT entry where any was made, then what the claimed banks hold logged
   and cleared. The threshold is 1, so that each error is told as it happens (18.5.2.2).
   A claim holds across a sleep, one owner for each bank: a CPU reset cleared CTL2 (18.3.2.5), while
   the first CPU of a sleep that was called off, or back from hibernating, had no reset and finds
   CMCI_EN set, on its own banks too. So once it has claimed, a CPU takes up its own banks again
   and no others. */
static void cmci_on(void) {
    if (!cpu_features.cmci) return;
    unsigned banks = mca_banks();
    bool again = this_cpu(cmci_claimed);
    uint32_t held = this_cpu(cmci_banks), mine = 0;
    uint64_t f = spin_lock(&cmci_lock);
    for (unsigned i = 0; i < banks; i++) {
        uint64_t ctl2 = rdmsr(MSR_MC_CTL2 + i);
        if (again ? !(held & (1u << i)) : (ctl2 & CTL2_CMCI_EN) != 0) continue; /* a sibling's */
        if (!wrmsr_checked(MSR_MC_CTL2 + i, (ctl2 & ~CTL2_THRESHOLD) | CTL2_CMCI_EN | 1)) continue;
        if (rdmsr(MSR_MC_CTL2 + i) & CTL2_CMCI_EN) mine |= 1u << i;
    }
    spin_unlock(&cmci_lock, f);
    this_cpu(cmci_banks) = mine;
    this_cpu(cmci_claimed) = true;
    mca_threshold_vector(cmci_told);
    if (mine) lapic_write(LAPIC_LVT_CMCI, VEC_MCE_THRESHOLD); /* fixed, unmasked */
    for (unsigned i = 0; i < banks; i++)
        if (mine & (1u << i)) mca_take_corrected(i, "from before");
    if (!cpu_current_index())
        kprintf("cpu: corrected machine checks of %u banks interrupt on vector %u (CMCI)\n",
                (unsigned)__builtin_popcount(mine), VEC_MCE_THRESHOLD);
}

/* ---- topology ----
   The caches' sharing is in leaf 4 (SDM vol. 2A, CPUID). */
static uint32_t intel_cache_leaf(void) {
    uint32_t max, b, c, d;
    cpuid(0, 0, &max, &b, &c, &d);
    return max >= 4 ? 4 : 0;
}

const struct cpu_maker cpu_intel = {
    .cpuid_names = {"GenuineIntel"},
    .read_features = intel_read_features,
    .same_features = intel_same_features,
    .tsc_stated = intel_tsc_stated,
    .perf_way = intel_perf_way,
    .mca_interrupt_on = cmci_on,
    .cache_leaf = intel_cache_leaf,
};
