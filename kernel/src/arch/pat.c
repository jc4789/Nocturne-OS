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
