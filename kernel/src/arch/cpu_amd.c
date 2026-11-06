/* AMD's CPUs, and Hygon's, which follow AMD's manuals (arch/cpu_maker.h). */
#include "kernel.h"
#include "arch/apic.h"
#include "arch/cpu.h"
#include "arch/cpu_maker.h"
#include "arch/mca.h"
#include "arch/smp.h"
#include "arch/tsc_rate.h"

/* ---- the TSC's rate ----
   The MSRs are read before the IDT is up, where a fault would end the boot with nothing said: only
   for a family whose manual gives them. */
#define MSR_HWCR 0xC0010015u
#define MSR_PSTATE_DEF0 0xC0010064u

static void amd_tsc_stated(struct tsc_rate *r) {
    unsigned family = cpu_features.family;
    if (!tsc_amd_family_known(family)) return tsc_rate_none(r, "no manual here gives this family's P-states");
    tsc_rate_amd(family, rdmsr(MSR_HWCR), rdmsr(MSR_PSTATE_DEF0), r);
}

/* ---- performance ----
   CPPC (CPUID 8000_0008h EBX[27]; PPR 57238 MSRC001_02B0-02B3): CPPC_CAP1 C001_02B0h (highest
   31:24, nominal 23:16, lowest nonlinear 15:8, lowest 7:0), CPPC_ENABLE C001_02B1h and CPPC_REQ
   C001_02B3h (EPP 31:24, desired 23:16, minimum 15:8, maximum 7:0), the registers of ACPI 6.5
   8.4.6.1 in AMD's fixed hardware. */
#define MSR_CPPC_CAP1   0xC00102B0u
#define MSR_CPPC_ENABLE 0xC00102B1u
#define MSR_CPPC_REQ    0xC00102B3u

/* The floor is the lowest nonlinear performance (ACPI 6.5 8.4.6.1.1.4, the lowest at which a lower
   level still saves power out of proportion). */
static void cppc_caps(struct cpu_perf_caps *c) {
    uint64_t caps = rdmsr(MSR_CPPC_CAP1);
    *c = (struct cpu_perf_caps){(caps >> 24) & 0xFF, (caps >> 16) & 0xFF, (caps >> 8) & 0xFF, caps & 0xFF};
}
static void cppc_request(const struct cpu_perf_caps *c, uint8_t preference) {
    wrmsr(MSR_CPPC_REQ, (uint64_t)c->highest | (uint64_t)c->floor << 8 | (uint64_t)preference << 24);
}
static const struct cpu_perf_way cppc_way = {"CPPC", MSR_CPPC_ENABLE, cppc_caps, cppc_request};

static const struct cpu_perf_way *amd_perf_way(void) { return cpu_features.cppc ? &cppc_way : NULL; }

/* ---- the APIC's extended LVT ----
   APM vol. 2 16.3.4 and 16.3.5, 16.4.5: the version register's EAS (bit 31) says the extended space
   from 400h is there; its feature register's XLC (bits 23:16) counts the extended LVT entries, at
   500h + 10h * n (in x2APIC mode the MSRs 840h and 850h + n, 16.11.1). The sources that signal
   through an entry name it in their own control registers. */
#define APIC_VERSION_EAS     (1u << 31)
#define APIC_EXT_FEATURE     0x400u
#define APIC_EXT_LVT(n)      (0x500u + 0x10u * (n))

static unsigned extended_lvts(void) {
    if (!(lapic_read(LAPIC_VERSION) & APIC_VERSION_EAS)) return 0;
    return (lapic_read(APIC_EXT_FEATURE) >> 16) & 0xFF;
}

/* What the firmware left in them goes: a source the kernel takes up unmasks its entry itself. */
static void amd_mask_own_lvt(void) {
    for (unsigned n = 0, count = extended_lvts(); n < count; n++) lapic_write(APIC_EXT_LVT(n), LVT_UNUSED);
}

/* ---- machine checks ----
   With scalable MCA (CPUID 8000_0007h EBX[3]) every bank has its own block at C000_2000h + 10h * i,
   up to 64 banks (APM vol. 2 9.3.3, Figure 9-10), the first four registers the same and MCA_CONFIG
   fifth. Each bank is told the kernel knows its registers (McaxEn): without it an uncorrected error
   is a system fatal event with no #MC to log it (9.3.3.1). */
#define SCALABLE_FIRST 0xC0002000u
#define SCALABLE_STRIDE 0x10u
#define MCA_CONFIG(bank) (SCALABLE_FIRST + SCALABLE_STRIDE * (bank) + 4u)
#define CONFIG_MCAX   (1ULL << 0)  /* MCA_CONFIG (9.3.3.1) */
#define CONFIG_MCAXEN (1ULL << 32)
#define CONFIG_INTEN  (1ULL << 40)

static const struct mca_layout scalable_layout = {SCALABLE_FIRST, SCALABLE_STRIDE, 64};

static const struct mca_layout *amd_mca_layout(void) { return cpu_features.scalable_mca ? &scalable_layout : NULL; }

static void amd_mca_bank_on(unsigned bank) {
    if (!cpu_features.scalable_mca) return;
    uint64_t config = rdmsr(MCA_CONFIG(bank));
    if (config & CONFIG_MCAX) wrmsr_checked(MCA_CONFIG(bank), config | CONFIG_MCAXEN);
}

/* The corrected errors: each bank whose counter has an interrupt (MISC0: VAL, CNTP, IntP, not LKD,
   9.3.2.6) signals it through one entry of the APIC's extended LVT (16.4.5), the offset the firmware
   set (LVTOFF, the same for every bank: an alias of MCA_INTR_CFG's, 9.3.1.4). With scalable MCA a
   bank interrupts on each corrected error through MCA_CONFIG's IntEn instead (9.3.3.1). */
#define MISC_VAL   (1ULL << 63) /* the thresholding format of MCi_MISC0 (9.3.2.6, Figure 9-9) */
#define MISC_CNTP  (1ULL << 62)
#define MISC_LKD   (1ULL << 61)
#define MISC_INTP  (1ULL << 60)
#define MISC_LVTOFF(m) ((unsigned)((m) >> 52) & 0xF)
#define MISC_CNTE  (1ULL << 51)
#define MISC_INTT  (3ULL << 49)
#define MISC_INTT_APIC (1ULL << 49)
#define MISC_OF    (1ULL << 48)
#define MISC_ERRCT_SHIFT 32
#define MISC_ERRCT (0xFFFFULL << MISC_ERRCT_SHIFT)
#define LEGACY_BANKS 32u
/* The corrected errors a legacy bank's counter takes before it interrupts: one, so that each is
   told as it happens (9.3.2.6: starting count = threshold level - this). */
#define THRESHOLD_ERRORS 1u
static uint16_t threshold_start[LEGACY_BANKS]; /* the count a legacy bank's counter starts from */

static bool threshold_bank(uint64_t misc) {
    return (misc & (MISC_VAL | MISC_CNTP | MISC_INTP | MISC_LKD)) == (MISC_VAL | MISC_CNTP | MISC_INTP);
}

static enum irq_result threshold_told(void *context) {
    (void)context;
    unsigned banks = mca_banks();
    for (unsigned i = 0; i < banks; i++) {
        mca_take_corrected(i, "told");
        if (cpu_features.scalable_mca) continue;
        uint64_t misc = rdmsr(mca_msr(i, MCA_MISC0));
        if (threshold_bank(misc) && (misc & MISC_OF))
            wrmsr_checked(mca_msr(i, MCA_MISC0),
                          (misc & ~(MISC_OF | MISC_ERRCT)) | (uint64_t)threshold_start[i] << MISC_ERRCT_SHIFT);
    }
    return IRQ_HANDLED;
}

static void amd_mca_interrupt_on(void) {
    unsigned banks = mca_banks(), offset = 16;
    for (unsigned i = 0; i < banks && offset == 16; i++) {
        uint64_t misc = rdmsr(mca_msr(i, MCA_MISC0));
        if (threshold_bank(misc)) offset = MISC_LVTOFF(misc);
    }
    if (offset == 16) return; /* no bank interrupts */
    if (offset >= extended_lvts()) {
        if (!cpu_current_index()) kprintf("cpu: machine-check thresholds name LVT entry %u, which the APIC lacks\n", offset);
        return;
    }
    mca_threshold_vector(threshold_told);
    /* the entry first: a counter enabled before it may interrupt through anything (9.3.2.6) */
    lapic_write(APIC_EXT_LVT(offset), VEC_MCE_THRESHOLD); /* fixed, unmasked */
    unsigned told = 0;
    for (unsigned i = 0; i < banks; i++) {
        uint64_t misc = rdmsr(mca_msr(i, MCA_MISC0));
        if (!threshold_bank(misc)) continue;
        if (cpu_features.scalable_mca) {
            uint64_t config = rdmsr(MCA_CONFIG(i));
            if (wrmsr_checked(MCA_CONFIG(i), config | CONFIG_INTEN)) told++;
            continue;
        }
        /* the counter's width: its implemented bits read back as written (9.3.2.6) */
        misc &= ~(MISC_CNTE | MISC_INTT | MISC_OF | MISC_ERRCT);
        if (!wrmsr_checked(mca_msr(i, MCA_MISC0), misc | MISC_ERRCT)) continue;
        uint16_t level = (uint16_t)(rdmsr(mca_msr(i, MCA_MISC0)) >> MISC_ERRCT_SHIFT);
        if (level < THRESHOLD_ERRORS) continue;
        threshold_start[i] = (uint16_t)(level - THRESHOLD_ERRORS);
        if (wrmsr_checked(mca_msr(i, MCA_MISC0), misc | MISC_CNTE | MISC_INTT_APIC |
                                                     (uint64_t)threshold_start[i] << MISC_ERRCT_SHIFT))
            told++;
    }
    if (!cpu_current_index())
        kprintf("cpu: corrected machine checks of %u banks interrupt on vector %u (LVT entry %u)\n", told,
                VEC_MCE_THRESHOLD, offset);
}

/* ---- topology ----
   AMD names its die in leaf 8000_0026h, where 0xB has no such level (APM vol. 3 E.4.24): level
   type 3, its own width the shift to the die's ID ("all logical processors with the same next level
   ID share current level", PPR 57238). 8 levels of 4 types of cache at most in 8000_001Dh. */
static uint32_t highest_extended_leaf(void) {
    uint32_t ext, b, c, d;
    cpuid(0x80000000, 0, &ext, &b, &c, &d);
    return ext;
}

static bool amd_die_shift(unsigned *shift) {
    if (highest_extended_leaf() < 0x80000026) return false;
    bool found = false;
    for (uint32_t m = 0; m < 256; m++) {
        uint32_t a, b, c, d;
        cpuid(0x80000026, m, &a, &b, &c, &d);
        unsigned type = (c >> 8) & 0xFF;
        if (!type || !(b & 0xFFFF)) break;
        if (type == 3) {
            *shift = a & 0x1F;
            found = true;
        }
    }
    return found;
}

static uint32_t amd_cache_leaf(void) {
    return cpu_features.topology_extensions && highest_extended_leaf() >= 0x8000001D ? 0x8000001D : 0;
}

const struct cpu_maker cpu_amd = {
    .cpuid_names = {"AuthenticAMD", "HygonGenuine"},
    .tsc_stated = amd_tsc_stated,
    .perf_way = amd_perf_way,
    .mca_layout = amd_mca_layout,
    .mca_bank_on = amd_mca_bank_on,
    .mca_interrupt_on = amd_mca_interrupt_on,
    .die_shift = amd_die_shift,
    .cache_leaf = amd_cache_leaf,
    .mask_own_lvt = amd_mask_own_lvt,
    /* APM vol. 2 16.9.1 enters x2APIC mode with nothing asked before it */
    .x2apic_unremapped = true,
};
