/* Real-mode entry (arch/wake.h). The wake page holds the code of arch/wake.asm and its data block;
   the three pages after it are the page table it switches on: a PML4 whose lower entry maps the
   first 2 MiB one to one and whose upper half is the kernel's, so that the jump into the kernel
   works before the kernel's own CR3 is loaded. */
#include "kernel.h"
#include "arch/cpu.h"
#include "arch/fpu.h"
#include "arch/pat.h"
#include "arch/pstate.h"
#include "arch/wake.h"
#include "mm/pmm.h"
#include "mm/vmm.h"
#include "mm/heap.h"

#define TRAMP_DATA 0x200 /* keep in step with arch/wake.asm */
#define WAKE_PAGES 4     /* code and data, PML4, PDPT, PD */
#define CR4_PCIDE  (1ULL << 17)

struct tramp_data {
    uint64_t gdt[4];      /* null, 0x08 code32, 0x10 data, 0x18 code64 */
    uint16_t gdt_limit;   /* 32 */
    uint32_t gdt_base;
    uint16_t pad0;
    uint32_t far32;       /* 40: tramp_pm32 */
    uint16_t far32_sel, pad1;
    uint32_t far64;       /* 48: tramp_lm64 */
    uint16_t far64_sel, pad2;
    uint32_t cr3, pad3;   /* 56 */
    uint32_t efer, pad4;  /* 64 */
    uint64_t entry, stack, arg; /* 72, 80, 88: stack and arg TDF_ANY's */
    uint64_t id_reg;      /* 96: struct lapic_ident's */
    uint32_t count;       /* 104: the table's rows */
    uint32_t id_mask;     /* 108 */
    uint64_t base, base_mask; /* 112, 120 */
    uint32_t flags;       /* 128: TDF_ */
    uint32_t pad5;
    uint64_t table;       /* 136: struct wake_cpu[count] */
} PACKED;
#define TDF_X2    1u
#define TDF_XAIDN 2u
#define TDF_ANY   4u
_Static_assert(__builtin_offsetof(struct tramp_data, far32) == 40, "wake.asm TD_FAR32");
_Static_assert(__builtin_offsetof(struct tramp_data, cr3) == 56, "wake.asm TD_CR3");
_Static_assert(__builtin_offsetof(struct tramp_data, arg) == 88, "wake.asm TD_ARG");
_Static_assert(__builtin_offsetof(struct tramp_data, id_reg) == 96, "wake.asm TD_ID_REG");
_Static_assert(__builtin_offsetof(struct tramp_data, count) == 104, "wake.asm TD_COUNT");
_Static_assert(__builtin_offsetof(struct tramp_data, id_mask) == 108, "wake.asm TD_ID_MASK");
_Static_assert(__builtin_offsetof(struct tramp_data, base) == 112, "wake.asm TD_BASE");
_Static_assert(__builtin_offsetof(struct tramp_data, base_mask) == 120, "wake.asm TD_BASE_MASK");
_Static_assert(__builtin_offsetof(struct tramp_data, flags) == 128, "wake.asm TD_FLAGS");
_Static_assert(__builtin_offsetof(struct tramp_data, table) == 136, "wake.asm TD_TABLE");
_Static_assert(__builtin_offsetof(struct wake_cpu, arg) == 8 && __builtin_offsetof(struct wake_cpu, apic_id) == 16 &&
                   sizeof(struct wake_cpu) == 24, "wake.asm ROW_");

extern const uint8_t tramp_start[], tramp_pm32[], tramp_lm64[], tramp_end[];
extern struct { uint16_t limit; uint64_t base; } PACKED wake_gdtr;
extern uint64_t wake_suspend(void (*enter)(void));
extern void wake_resume64(void);

extern const uint8_t hib_copy_start[], hib_copy_end[];

static uint64_t page; /* physical; 0 without one */
static uint64_t spare; /* the page after them when there is one: a hibernation restore's last copy */
static uint8_t resume_stack[4096] __attribute__((aligned(16))); /* wake_resume64's few pushes */

void wake_init(void) {
    uint64_t n, base = pmm_low_range(&n);
    if (n < WAKE_PAGES || (size_t)(tramp_end - tramp_start) > TRAMP_DATA) {
        kprintf("wake: no room below 1 MiB, no sleep and no AP restart\n");
        return;
    }
    page = base;
    uint64_t *pml4 = phys_to_virt(page + 0x1000), *pdpt = phys_to_virt(page + 0x2000),
             *pd = phys_to_virt(page + 0x3000);
    memset(pml4, 0, 3 * PAGE_SIZE);
    pml4[0] = (page + 0x2000) | PTE_P | PTE_W;
    pdpt[0] = (page + 0x3000) | PTE_P | PTE_W;
    pd[0] = PTE_P | PTE_W | PTE_PS; /* 0 - 2 MiB, the wake page included */
    if (n > WAKE_PAGES && (size_t)(hib_copy_end - hib_copy_start) <= PAGE_SIZE) spare = page + WAKE_PAGES * PAGE_SIZE;
}

bool wake_available(void) { return page != 0; }
uint64_t wake_spare_page(void) { return spare; }

NORETURN void wake_hibernate_jump(uint64_t pml4, uint64_t list, uint64_t image_wake_page) {
    memcpy(phys_to_virt(spare), hib_copy_start, (size_t)(hib_copy_end - hib_copy_start));
    /* the kernel half stays mapped until the jump into the spare page */
    uint64_t *p = phys_to_virt(pml4), *k = phys_to_virt(kernel_pml4 & PTE_ADDR);
    for (unsigned i = 256; i < 512; i++) p[i] = k[i];
    cli();
    write_cr4(read_cr4() & ~(1ULL << 7)); /* PGE off: every TLB entry goes, global ones too */
    write_cr3(pml4);
    ((void (*)(uint64_t, uint64_t))spare)(list, image_wake_page);
    for (;;) hlt();
}
uint64_t wake_page(void) { return page; }

/* The page as the next CPUs find it: the code, the GDT, the way into long mode, entry and how a
   CPU reads its APIC ID; the caller adds whom it is for, then wake_publish. */
static struct tramp_data *wake_fill(uint64_t entry) {
    uint8_t *p = phys_to_virt(page);
    memcpy(p, tramp_start, (size_t)(tramp_end - tramp_start));
    struct tramp_data *d = (struct tramp_data *)(p + TRAMP_DATA);
    d->gdt[0] = 0;
    d->gdt[1] = 0x00CF9A000000FFFFULL;
    d->gdt[2] = 0x00CF92000000FFFFULL;
    d->gdt[3] = 0x00AF9A000000FFFFULL;
    d->gdt_limit = sizeof d->gdt - 1;
    d->gdt_base = (uint32_t)(page + TRAMP_DATA);
    d->far32 = (uint32_t)(page + (uint64_t)(tramp_pm32 - tramp_start));
    d->far32_sel = 0x08;
    d->far64 = (uint32_t)(page + (uint64_t)(tramp_lm64 - tramp_start));
    d->far64_sel = 0x18;
    d->cr3 = (uint32_t)(page + 0x1000);
    d->efer = EFER_LME | EFER_NXE;
    d->entry = entry;
    struct lapic_ident id;
    lapic_ident(&id);
    d->id_reg = (uint64_t)(uintptr_t)id.id_reg;
    d->id_mask = id.id_mask;
    d->base = id.base;
    d->base_mask = id.base_mask;
    d->flags = (id.x2 ? TDF_X2 : 0) | (id.xaidn ? TDF_XAIDN : 0);
    /* the kernel half as it is now: entries added since the last time count too */
    uint64_t *low = phys_to_virt(page + 0x1000), *kernel = phys_to_virt(kernel_pml4 & PTE_ADDR);
    for (unsigned i = 256; i < 512; i++) low[i] = kernel[i];
    return d;
}

/* The firmware and a starting AP read the page and the table uncached. */
static void wake_publish(void) { __asm__ volatile("wbinvd" : : : "memory"); }

void wake_prepare_any(uint64_t entry, uint64_t stack, uint64_t arg) {
    struct tramp_data *d = wake_fill(entry);
    d->stack = stack;
    d->arg = arg;
    d->flags |= TDF_ANY;
    wake_publish();
}

void wake_prepare_cpus(uint64_t entry, const struct wake_cpu *table, uint32_t count) {
    struct tramp_data *d = wake_fill(entry);
    d->table = (uint64_t)(uintptr_t)table;
    d->count = count;
    wake_publish();
}

/* ---- the BSP across S3 ---- */
static struct {
    uint64_t cr0, cr3, cr4, efer, pat, star, lstar, cstar, sfmask, fs, gs, kgs, tsc_aux;
    struct { uint16_t limit; uint64_t base; } PACKED idtr;
} cpu;
static void *fpu_storage; /* the first CPU's extended state across the sleep */

/* Called by wake_resume64 on the saved stack, with the kernel's GDT loaded and nothing else:
   no IDT, no GS base. Touches no per-CPU data. */
void wake_restore_cpu(void) {
    write_cr3(cpu.cr3 & PTE_ADDR); /* PCID 0: CR4.PCIDE may only be set then */
    write_cr4(cpu.cr4 & ~CR4_PCIDE);
    if (cpu.cr4 & CR4_PCIDE) write_cr4(cpu.cr4);
    write_cr3(cpu.cr3);
    write_cr0(cpu.cr0);
    wrmsr(MSR_EFER, cpu.efer & ~(uint64_t)EFER_LMA);
    wrmsr(MSR_PAT, cpu.pat);
    wrmsr(MSR_STAR, cpu.star);
    wrmsr(MSR_LSTAR, cpu.lstar);
    wrmsr(MSR_CSTAR, cpu.cstar);
    wrmsr(MSR_SFMASK, cpu.sfmask);
    wrmsr(MSR_FS_BASE, cpu.fs);
    wrmsr(MSR_GS_BASE, cpu.gs);
    wrmsr(MSR_KERNEL_GS_BASE, cpu.kgs);
    wrmsr(MSR_TSC_AUX, cpu.tsc_aux);
    ((uint64_t *)wake_gdtr.base)[TSS_SEL / 8] &= ~(1ULL << 41); /* the TSS is marked busy: ltr refuses that */
    __asm__ volatile("ltr %w0" : : "r"((uint16_t)TSS_SEL));
    __asm__ volatile("lidt %0" : : "m"(cpu.idtr));
}

bool wake_sleep(void (*enter)(void)) {
    if (!page) return false;
    cpu.cr0 = read_cr0();
    cpu.cr3 = read_cr3();
    cpu.cr4 = read_cr4();
    cpu.efer = rdmsr(MSR_EFER);
    cpu.pat = rdmsr(MSR_PAT);
    cpu.star = rdmsr(MSR_STAR);
    cpu.lstar = rdmsr(MSR_LSTAR);
    cpu.cstar = rdmsr(MSR_CSTAR);
    cpu.sfmask = rdmsr(MSR_SFMASK);
    cpu.fs = rdmsr(MSR_FS_BASE);
    cpu.gs = rdmsr(MSR_GS_BASE);
    cpu.kgs = rdmsr(MSR_KERNEL_GS_BASE);
    cpu.tsc_aux = rdmsr(MSR_TSC_AUX);
    __asm__ volatile("sgdt %0" : "=m"(wake_gdtr));
    __asm__ volatile("sidt %0" : "=m"(cpu.idtr));
    if (!fpu_storage && !(fpu_storage = kmalloc(fpu_storage_bytes()))) return false;
    fpu_save(fpu_state_in(fpu_storage));
    mtrr_save();
    wake_prepare_any((uint64_t)wake_resume64, (uint64_t)resume_stack + sizeof resume_stack, 0);
    if (!wake_suspend(enter)) return false;
    /* back from the firmware: memory types first, everything after runs faster for it */
    mtrr_sync();
    vmm_flush_all();
    if (!fpu_init_ap()) panic("wake: the FPU came back different");
    fpu_restore(fpu_state_in(fpu_storage));
    pstate_init_ap(); /* the firmware's clock policy, again ours */
    return true;
}
