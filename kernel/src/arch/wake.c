/* Real-mode entry (arch/wake.h). The wake page holds the code of arch/wake.asm and its data block;
   the three pages after it are the page table it switches on: a PML4 whose lower entry maps the
   first 2 MiB one to one and whose upper half is the kernel's, so that the jump into the kernel
   works before the kernel's own CR3 is loaded. */
#include "kernel.h"
#include "arch/cpu.h"
#include "arch/fpu.h"
#include "arch/wake.h"
#include "mm/pmm.h"
#include "mm/vmm.h"
#include "mm/heap.h"

#define TRAMP_DATA 0x200 /* keep in step with arch/wake.asm */
#define WAKE_PAGES 4     /* code and data, PML4, PDPT, PD */

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
    uint32_t count;       /* 96: the table's rows */
    uint32_t flags;       /* 100: TDF_ */
    uint64_t table;       /* 104: struct wake_cpu[count] */
} PACKED;
#define TDF_ANY 1u
_Static_assert(__builtin_offsetof(struct tramp_data, far32) == 40, "wake.asm TD_FAR32");
_Static_assert(__builtin_offsetof(struct tramp_data, cr3) == 56, "wake.asm TD_CR3");
_Static_assert(__builtin_offsetof(struct tramp_data, arg) == 88, "wake.asm TD_ARG");
_Static_assert(__builtin_offsetof(struct tramp_data, count) == 96, "wake.asm TD_COUNT");
_Static_assert(__builtin_offsetof(struct tramp_data, flags) == 100, "wake.asm TD_FLAGS");
_Static_assert(__builtin_offsetof(struct tramp_data, table) == 104, "wake.asm TD_TABLE");
_Static_assert(__builtin_offsetof(struct wake_cpu, arg) == 8 && __builtin_offsetof(struct wake_cpu, apic_id) == 16 &&
                   sizeof(struct wake_cpu) == 24, "wake.asm ROW_");

extern const uint8_t tramp_start[], tramp_pm32[], tramp_lm64[], tramp_end[];
extern uint64_t wake_suspend(void (*enter)(void));
extern void wake_resume64(void);
void wake_resume_cpu(void);

static uint64_t page;  /* physical; 0 without one */
static uint64_t spare; /* the page after them when there is one: a hibernation restore's last copy */

void wake_init(void) {
    uint64_t n, base = pmm_low_range(&n);
    if (n < WAKE_PAGES || (size_t)(tramp_end - tramp_start) > TRAMP_DATA) {
        kprintf("wake: no room below 1 MiB, no sleep and no other CPU\n");
        return;
    }
    page = base;
    uint64_t *pml4 = phys_to_virt(page + 0x1000), *pdpt = phys_to_virt(page + 0x2000),
             *pd = phys_to_virt(page + 0x3000);
    memset(pml4, 0, 3 * PAGE_SIZE);
    pml4[0] = (page + 0x2000) | PTE_P | PTE_W;
    pdpt[0] = (page + 0x3000) | PTE_P | PTE_W;
    pd[0] = PTE_P | PTE_W | PTE_PS; /* 0 - 2 MiB, the wake page included */
    if (n > WAKE_PAGES) spare = page + WAKE_PAGES * PAGE_SIZE;
}

bool wake_available(void) { return page != 0; }
uint64_t wake_spare_page(void) { return spare; }
uint64_t wake_page(void) { return page; }
uint64_t wake_long_entry(uint64_t at) { return at + (uint64_t)(tramp_lm64 - tramp_start); }

/* The page as the next CPUs find it: the code, the GDT, the way into long mode and entry; the
   caller adds whom it is for, then wake_publish. */
static struct tramp_data *wake_fill(uint64_t entry) {
    uint8_t *p = phys_to_virt(page);
    memcpy(p, tramp_start, (size_t)(tramp_end - tramp_start));
    struct tramp_data *d = (struct tramp_data *)(p + TRAMP_DATA);
    memset(d, 0, sizeof *d);
    d->gdt[1] = 0x00CF9A000000FFFFULL;
    d->gdt[2] = 0x00CF92000000FFFFULL;
    d->gdt[3] = 0x00AF9A000000FFFFULL;
    d->gdt_limit = sizeof d->gdt - 1;
    d->gdt_base = (uint32_t)(page + TRAMP_DATA);
    d->far32 = (uint32_t)(page + (uint64_t)(tramp_pm32 - tramp_start));
    d->far32_sel = 0x08;
    d->far64 = (uint32_t)wake_long_entry(page);
    d->far64_sel = 0x18;
    d->cr3 = (uint32_t)(page + 0x1000);
    d->efer = EFER_LME | EFER_NXE;
    d->entry = entry;
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
    d->flags = TDF_ANY;
    wake_publish();
}

void wake_prepare_cpus(uint64_t entry, const struct wake_cpu *table, uint32_t count) {
    struct tramp_data *d = wake_fill(entry);
    d->table = (uint64_t)(uintptr_t)table;
    d->count = count;
    wake_publish();
}

/* ---- the BSP across S3 ---- */
static void *fpu_storage; /* the sleeping task's extended state */

/* Called by wake_resume64 on the stack wake_suspend saved, on the wake page's tables and GDT: the
   CPU as at boot, then the kernel's tables at PCID 0 (vmm_init_cpu may turn PCIDs on then; the
   space this CPU held it left before the sleep). */
void wake_resume_cpu(void) {
    if (!cpu_setup(0)) panic("wake: the first CPU cannot be set up again after sleep");
    write_cr3(kernel_pml4);
    vmm_init_cpu();
}

bool wake_sleep(void (*enter)(void)) {
    if (!page) return false;
    if (!fpu_storage && !(fpu_storage = kmalloc(fpu_storage_bytes()))) return false;
    uint64_t fs = fs_base_read(), gs = user_gs_base_read();
    fpu_save(fpu_state_in(fpu_storage));
    vmm_leave();
    /* wake_resume64 takes the saved stack itself */
    wake_prepare_any((uint64_t)wake_resume64, 0, 0);
    if (!wake_suspend(enter)) return false;
    fpu_restore(fpu_state_in(fpu_storage));
    fs_base_write(fs);
    user_gs_base_write(gs);
    return true;
}
