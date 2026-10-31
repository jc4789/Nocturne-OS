/* Memory types: the Page Attribute Table and the MTRRs, each CPU's own registers. The bootloader
   leaves IA32_PAT at whatever the firmware chose (usually index 5 = WT), but the kernel's page
   tables assume the layout in pat.h. The firmware sets the MTRRs at power on, the same on every
   CPU; after S3 it may not set them as before, and a CPU the kernel starts itself gets them from
   nobody: the first CPU's at boot are the reference every CPU is held to. */
#include "kernel.h"
#include "arch/cpu.h"
#include "arch/pat.h"

/* Every TLB entry, global ones too: a CR4.PGE change flushes them all, every PCID's included (Intel
   SDM vol. 3A 5.10.4.1); without PGE (and PCIDE) a CR3 load does (14.11.8 step 7). */
static void flush_tlb_all(uint64_t cr4) {
    if (!(cr4 & CR4_PGE)) {
        write_cr3(read_cr3());
        return;
    }
    write_cr4(cr4 & ~CR4_PGE);
    write_cr4(cr4);
}

/* The sequence of Intel SDM vol. 3A 14.11.8 (steps 4 to 13) and 14.12.4, AMD APM vol. 2 7.8: no-fill
   cache mode, write back and invalidate, flush the TLB, change the registers (with the MTRRs off
   while they change), then flush both again and restore the caching mode. CR0, the caches and the
   registers are this CPU's own; its interrupts are off (cpu_setup). The other CPUs are each held
   to the same values by their own cpu_setup. */
static void cache_off(uint64_t *cr0, uint64_t *cr4) {
    *cr0 = read_cr0();
    *cr4 = read_cr4();
    write_cr0((*cr0 | CR0_CD) & ~CR0_NW);
    __asm__ volatile("wbinvd" : : : "memory");
    flush_tlb_all(*cr4);
}

static void cache_on(uint64_t cr0, uint64_t cr4) {
    __asm__ volatile("wbinvd" : : : "memory");
    flush_tlb_all(cr4);
    write_cr0(cr0);
}

/* The MTRRs (SDM vol. 3A 14.11.1, Figure 14-5): IA32_MTRRCAP's VCNT (bits 7:0) variable pairs at
   200h, the eleven fixed ones where FIX (bit 8) says, IA32_MTRR_DEF_TYPE with E (bit 11). VCNT is
   an 8-bit field: 255 pairs at most. */
#define MSR_MTRRCAP 0xFEu
#define MSR_MTRR_DEF 0x2FFu
#define MTRR_DEF_E (1ULL << 11)
#define MTRR_FIX (1ULL << 8)
#define MTRR_VCNT_MAX 255u
static const uint32_t fixed_msr[11] = {0x250, 0x258, 0x259, 0x268, 0x269, 0x26A, 0x26B, 0x26C, 0x26D, 0x26E, 0x26F};
static struct {
    bool kept, fixed;
    unsigned pairs;
    uint64_t def, fix[11], var[2 * MTRR_VCNT_MAX];
} mtrr;

static void mtrr_keep(void) {
    uint64_t cap = rdmsr(MSR_MTRRCAP);
    mtrr.pairs = (unsigned)(cap & 0xFF);
    mtrr.fixed = (cap & MTRR_FIX) != 0;
    mtrr.def = rdmsr(MSR_MTRR_DEF);
    if (mtrr.fixed)
        for (unsigned i = 0; i < 11; i++) mtrr.fix[i] = rdmsr(fixed_msr[i]);
    for (unsigned i = 0; i < 2 * mtrr.pairs; i++) mtrr.var[i] = rdmsr(0x200 + i);
    mtrr.kept = true;
}

static bool mtrr_same(void) {
    if (rdmsr(MSR_MTRR_DEF) != mtrr.def) return false;
    if (mtrr.fixed)
        for (unsigned i = 0; i < 11; i++)
            if (rdmsr(fixed_msr[i]) != mtrr.fix[i]) return false;
    for (unsigned i = 0; i < 2 * mtrr.pairs; i++)
        if (rdmsr(0x200 + i) != mtrr.var[i]) return false;
    return true;
}

bool pat_cpu_setup(void) {
    if (!mtrr.kept) mtrr_keep();
    bool mtrr_differ = !mtrr_same(), pat_differ = rdmsr(MSR_PAT) != PAT_NOCTURNE;
    if (!mtrr_differ && !pat_differ) return true;
    uint64_t cr0, cr4;
    cache_off(&cr0, &cr4);
    if (mtrr_differ) {
        wrmsr(MSR_MTRR_DEF, mtrr.def & ~MTRR_DEF_E);
        if (mtrr.fixed)
            for (unsigned i = 0; i < 11; i++) wrmsr(fixed_msr[i], mtrr.fix[i]);
        for (unsigned i = 0; i < 2 * mtrr.pairs; i++) wrmsr(0x200 + i, mtrr.var[i]);
        wrmsr(MSR_MTRR_DEF, mtrr.def);
    }
    if (pat_differ) wrmsr(MSR_PAT, PAT_NOCTURNE);
    cache_on(cr0, cr4);
    return rdmsr(MSR_PAT) == PAT_NOCTURNE;
}
