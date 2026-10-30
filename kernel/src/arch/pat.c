/* Page Attribute Table setup. The bootloader leaves IA32_PAT at whatever the firmware chose (usually
   index 5 = WT), but the kernel's page tables assume the layout in pat.h. */
#include "kernel.h"
#include "arch/cpu.h"
#include "arch/pat.h"

#define MSR_PAT   0x277
#define CR0_NW    (1ULL << 29)
#define CR0_CD    (1ULL << 30)
#define CR4_PGE   (1ULL << 7)

bool pat_supported(void) {
    uint32_t a, b, c, d;
    cpuid(1, 0, &a, &b, &c, &d);
    return (d & (1u << 16)) != 0;
}

static void flush_tlb_all(uint64_t cr4) {
    if (cr4 & CR4_PGE) write_cr4(cr4 & ~CR4_PGE); /* PGE off so global entries go too */
    write_cr3(read_cr3());
    if (cr4 & CR4_PGE) write_cr4(cr4);
}

/* CR0, the caches and IA32_PAT are this CPU's own: closing its interrupts is the whole guard. */
uint64_t pat_init_cpu(void) {
    if (!pat_supported()) return 0;
    if (rdmsr(MSR_PAT) == PAT_NOCTURNE) return PAT_NOCTURNE; /* already right: nothing to flush */
    uint64_t flags = irq_save();
    uint64_t cr0 = read_cr0(), cr4 = read_cr4();
    /* SDM 11.11.8 / APM 7.8 sequence: no-fill cache mode, write back and invalidate, flush the TLB
       (global pages included), change the MSR, then flush both again and restore the caching mode. */
    write_cr0((cr0 | CR0_CD) & ~CR0_NW);
    __asm__ volatile("wbinvd" : : : "memory");
    flush_tlb_all(cr4);
    wrmsr(MSR_PAT, PAT_NOCTURNE);
    __asm__ volatile("wbinvd" : : : "memory");
    flush_tlb_all(cr4);
    write_cr0(cr0);
    irq_restore(flags);
    return rdmsr(MSR_PAT);
}

/* ---- MTRRs across a sleep ----
   The firmware sets the memory types at power on; after S3 it may not set them as before, and a
   CPU the kernel starts itself gets them from nobody. The set CPU 0 had before (mtrr_save, while
   every other CPU is parked) is the reference; each CPU writes it into its own MTRRs. */
#define MSR_MTRRCAP 0xFE
#define MSR_MTRR_DEF 0x2FF
#define MTRR_VAR_MAX 16
static const uint32_t fixed_msr[11] = {0x250, 0x258, 0x259, 0x268, 0x269, 0x26A, 0x26B, 0x26C, 0x26D, 0x26E, 0x26F};
static struct {
    bool valid, fixed;
    unsigned nvar;
    uint64_t def, fix[11], var[2 * MTRR_VAR_MAX];
} mtrr;

void mtrr_save(void) {
    uint32_t a, b, c, d;
    cpuid(1, 0, &a, &b, &c, &d);
    if (!(d & (1u << 12))) return;
    uint64_t cap = rdmsr(MSR_MTRRCAP);
    mtrr.nvar = MIN((unsigned)(cap & 0xFF), MTRR_VAR_MAX);
    mtrr.fixed = (cap & (1u << 8)) != 0;
    mtrr.def = rdmsr(MSR_MTRR_DEF);
    if (mtrr.fixed)
        for (unsigned i = 0; i < 11; i++) mtrr.fix[i] = rdmsr(fixed_msr[i]);
    for (unsigned i = 0; i < 2 * mtrr.nvar; i++) mtrr.var[i] = rdmsr(0x200 + i);
    mtrr.valid = true;
}

static bool mtrr_same(void) {
    if (rdmsr(MSR_MTRR_DEF) != mtrr.def) return false;
    if (mtrr.fixed)
        for (unsigned i = 0; i < 11; i++) if (rdmsr(fixed_msr[i]) != mtrr.fix[i]) return false;
    for (unsigned i = 0; i < 2 * mtrr.nvar; i++) if (rdmsr(0x200 + i) != mtrr.var[i]) return false;
    return true;
}

void mtrr_sync(void) {
    if (!mtrr.valid || mtrr_same()) return;
    uint64_t flags = irq_save();
    uint64_t cr0 = read_cr0(), cr4 = read_cr4();
    /* the same sequence as for the PAT, with the MTRRs off while they change */
    write_cr0((cr0 | CR0_CD) & ~CR0_NW);
    __asm__ volatile("wbinvd" : : : "memory");
    flush_tlb_all(cr4);
    wrmsr(MSR_MTRR_DEF, mtrr.def & ~(1ULL << 11));
    if (mtrr.fixed)
        for (unsigned i = 0; i < 11; i++) wrmsr(fixed_msr[i], mtrr.fix[i]);
    for (unsigned i = 0; i < 2 * mtrr.nvar; i++) wrmsr(0x200 + i, mtrr.var[i]);
    wrmsr(MSR_MTRR_DEF, mtrr.def);
    __asm__ volatile("wbinvd" : : : "memory");
    flush_tlb_all(cr4);
    write_cr0(cr0);
    irq_restore(flags);
}
