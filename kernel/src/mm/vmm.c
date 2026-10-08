/* Virtual memory: 4-level paging, our own kernel page tables, per-process address spaces. */
#include "kernel.h"
#include "arch/cpu.h"
#include "mm/vmm.h"
#include "mm/pmm.h"
#include "sys/sched.h"
#include "limine.h"

uint64_t kernel_pml4;
uint64_t pte_nx;

extern char __kernel_start[], __kernel_end[], __text_start[], __rodata_start[], __data_start[];

static inline uint64_t *tbl(uint64_t phys) { return (uint64_t *)phys_to_virt(phys & PTE_ADDR); }

/* Split a 2 MiB / 1 GiB mapping into the next level down. */
static void split_huge(uint64_t *entry, int level) {
    uint64_t e = *entry;
    uint64_t base = e & PTE_ADDR & ~((level == 2 ? (1ULL << 21) : (1ULL << 30)) - 1);
    uint64_t newt = pmm_alloc_zeroed();
    if (!newt) panic("vmm: out of memory splitting page");
    uint64_t *t = tbl(newt);
    uint64_t flags = e & (0xFFF | PTE_NX) & ~PTE_PS;
    bool pat = e & PTE_PAT2M;
    for (int i = 0; i < 512; i++) {
        if (level == 2) {
            t[i] = (base + i * PAGE_SIZE) | flags | (pat ? PTE_PAT4K : 0);
        } else {
            t[i] = (base + (uint64_t)i * (1ULL << 21)) | flags | PTE_PS | (pat ? PTE_PAT2M : 0);
        }
    }
    *entry = newt | (e & (PTE_P | PTE_W | PTE_U));
}

static uint64_t *next_level(uint64_t *table, int idx, bool create, bool user, int level) {
    uint64_t e = table[idx];
    if (e & PTE_P) {
        if (e & PTE_PS) {
            split_huge(&table[idx], level);
            e = table[idx];
        }
        if (user && !(e & PTE_U)) table[idx] |= PTE_U;
        return tbl(e);
    }
    if (!create) return NULL;
    uint64_t n = pmm_alloc_zeroed();
    if (!n) return NULL;
    table[idx] = n | PTE_P | PTE_W | (user ? PTE_U : 0);
    return tbl(n);
}

bool vmm_map_page(uint64_t pml4, uint64_t va, uint64_t pa, uint64_t flags) {
    sched_quiesce_space(va >= USER_TOP ? 0 : pml4);
    if ((flags & (PTE_U | PTE_W | PTE_SHARED)) == (PTE_U | PTE_W | PTE_SHARED))
        sched_pin_space(pml4);
    bool user = flags & PTE_U;
    uint64_t *l4 = tbl(pml4);
    uint64_t *l3 = next_level(l4, (va >> 39) & 511, true, user, 4);
    if (!l3) return false;
    uint64_t *l2 = next_level(l3, (va >> 30) & 511, true, user, 3);
    if (!l2) return false;
    uint64_t *l1 = next_level(l2, (va >> 21) & 511, true, user, 2);
    if (!l1) return false;
    l1[(va >> 12) & 511] = (pa & PTE_ADDR) | flags | PTE_P;
    invlpg(va);
    return true;
}

static bool map_2m(uint64_t pml4, uint64_t va, uint64_t pa, uint64_t flags) {
    uint64_t *l4 = tbl(pml4);
    uint64_t *l3 = next_level(l4, (va >> 39) & 511, true, false, 4);
    if (!l3) return false;
    uint64_t *l2 = next_level(l3, (va >> 30) & 511, true, false, 3);
    if (!l2) return false;
    uint64_t *e = &l2[(va >> 21) & 511];
    if ((*e & PTE_P) && !(*e & PTE_PS)) {
        /* a page table already lives here; fall back to 4K pages */
        uint64_t f4 = flags & ~PTE_PAT2M;
        if (flags & PTE_PAT2M) f4 |= PTE_PAT4K;
        for (int i = 0; i < 512; i++) vmm_map_page(pml4, va + i * PAGE_SIZE, pa + i * PAGE_SIZE, f4);
        return true;
    }
    *e = (pa & PTE_ADDR) | flags | PTE_PS | PTE_P;
    invlpg(va);
    return true;
}

static uint64_t *walk(uint64_t pml4, uint64_t va, bool *huge) {
    uint64_t *l4 = tbl(pml4);
    uint64_t e = l4[(va >> 39) & 511];
    if (!(e & PTE_P)) return NULL;
    uint64_t *l3 = tbl(e);
    e = l3[(va >> 30) & 511];
    if (!(e & PTE_P)) return NULL;
    if (e & PTE_PS) { *huge = true; return &l3[(va >> 30) & 511]; }
    uint64_t *l2 = tbl(e);
    e = l2[(va >> 21) & 511];
    if (!(e & PTE_P)) return NULL;
    if (e & PTE_PS) { *huge = true; return &l2[(va >> 21) & 511]; }
    uint64_t *l1 = tbl(e);
    *huge = false;
    return &l1[(va >> 12) & 511];
}

uint64_t vmm_get_pte(uint64_t pml4, uint64_t va) {
    bool huge = false;
    uint64_t *p = walk(pml4, va, &huge);
    if (!p || !(*p & PTE_P)) return 0;
    return *p;
}

uint64_t vmm_translate(uint64_t pml4, uint64_t va) {
    bool huge = false;
    uint64_t *p = walk(pml4, va, &huge);
    if (!p || !(*p & PTE_P)) return 0;
    if (huge) return (*p & PTE_ADDR & ~((1ULL << 21) - 1)) + (va & ((1ULL << 21) - 1));
    return (*p & PTE_ADDR) + (va & 0xFFF);
}

uint64_t vmm_unmap_page(uint64_t pml4, uint64_t va) {
    sched_quiesce_space(va >= USER_TOP ? 0 : pml4);
    bool huge = false;
    uint64_t *p = walk(pml4, va, &huge);
    if (!p || !(*p & PTE_P) || huge) return 0;
    uint64_t old = *p;
    *p = 0;
    invlpg(va);
    return old;
}

static void map_range(uint64_t va, uint64_t pa, uint64_t size, uint64_t flags4k, uint64_t flags2m) {
    uint64_t end = pa + size;
    while (pa < end) {
        if ((pa & ((1ULL << 21) - 1)) == 0 && (va & ((1ULL << 21) - 1)) == 0 && end - pa >= (1ULL << 21)) {
            map_2m(kernel_pml4, va, pa, flags2m);
            pa += 1ULL << 21;
            va += 1ULL << 21;
        } else {
            vmm_map_page(kernel_pml4, va, pa, flags4k);
            pa += PAGE_SIZE;
            va += PAGE_SIZE;
        }
    }
}

void vmm_switch(uint64_t pml4) {
    if (read_cr3() != pml4) write_cr3(pml4);
}

void vmm_flush_all(void) {
    uint64_t cr4 = read_cr4();
    if (cr4 & (1ULL << 7)) write_cr4(cr4 & ~(1ULL << 7)); /* PGE */
    write_cr3(read_cr3());
    if (cr4 & (1ULL << 7)) write_cr4(cr4);
}

void vmm_init(struct limine_memmap_response *mm, struct limine_executable_address_response *ka) {
    /* no-execute pages, if the CPU has them (EFER.NXE must be on before any PTE uses bit 63) */
    uint32_t a, b, c, d;
    cpuid(0x80000000, 0, &a, &b, &c, &d);
    if (a >= 0x80000001) {
        cpuid(0x80000001, 0, &a, &b, &c, &d);
        if (d & (1u << 20)) {
            wrmsr(0xC0000080, rdmsr(0xC0000080) | (1ULL << 11));
            pte_nx = PTE_NX;
        }
    }
    /* write-protect also applies to the kernel, so read-only pages really are read-only */
    write_cr0(read_cr0() | (1ULL << 16));

    kernel_pml4 = pmm_alloc_zeroed();
    uint64_t *l4 = tbl(kernel_pml4);
    /* pre-create every upper-half PDPT so all address spaces share kernel mappings */
    for (int i = 256; i < 512; i++) l4[i] = pmm_alloc_zeroed() | PTE_P | PTE_W;

    /* higher-half direct map of every memory map region */
    for (uint64_t i = 0; i < mm->entry_count; i++) {
        struct limine_memmap_entry *e = mm->entries[i];
        uint64_t base = ALIGN_DOWN(e->base, PAGE_SIZE);
        uint64_t end = ALIGN_UP(e->base + e->length, PAGE_SIZE);
        if (e->type == LIMINE_MEMMAP_FRAMEBUFFER) {
            /* write-combining: PAT index 5 */
            map_range(hhdm_offset + base, base, end - base, PTE_W | PTE_PWT | PTE_PAT4K | pte_nx,
                      PTE_W | PTE_PWT | PTE_PAT2M | pte_nx);
        } else if (e->type == LIMINE_MEMMAP_BAD_MEMORY) {
            continue;
        } else {
            map_range(hhdm_offset + base, base, end - base, PTE_W | pte_nx, PTE_W | pte_nx);
        }
    }

    /* the kernel image: code read-only + executable, rodata read-only, everything else writable;
       only code is executable */
    uint64_t kstart = ALIGN_DOWN((uint64_t)__kernel_start, PAGE_SIZE);
    uint64_t kend = ALIGN_UP((uint64_t)__kernel_end, PAGE_SIZE);
    for (uint64_t va = kstart; va < kend; va += PAGE_SIZE) {
        uint64_t f = PTE_G | PTE_W | pte_nx;
        if (va >= (uint64_t)__text_start && va < (uint64_t)__rodata_start) f = PTE_G;
        else if (va >= (uint64_t)__rodata_start && va < (uint64_t)__data_start) f = PTE_G | pte_nx;
        vmm_map_page(kernel_pml4, va, va - ka->virtual_base + ka->physical_base, f);
    }
    write_cr3(kernel_pml4);
}

void *vmm_map_mmio(uint64_t phys, uint64_t size) {
    uint64_t base = ALIGN_DOWN(phys, PAGE_SIZE);
    uint64_t end = ALIGN_UP(phys + size, PAGE_SIZE);
    for (uint64_t p = base; p < end; p += PAGE_SIZE) {
        vmm_map_page(kernel_pml4, hhdm_offset + p, p, PTE_W | PTE_PCD | PTE_PWT | pte_nx);
    }
    return phys_to_virt(phys);
}

/* ---- user address spaces ---- */

uint64_t vmm_new_space(void) {
    uint64_t p = pmm_alloc_zeroed();
    if (!p) return 0;
    uint64_t *n = tbl(p), *k = tbl(kernel_pml4);
    for (int i = 256; i < 512; i++) n[i] = k[i];
    return p;
}

static void free_level(uint64_t phys, int level) {
    uint64_t *t = tbl(phys);
    for (int i = 0; i < 512; i++) {
        uint64_t e = t[i];
        if (!(e & PTE_P)) continue;
        if (level == 1) {
            if (!(e & PTE_SHARED)) pmm_free(e & PTE_ADDR);
        } else if (!(e & PTE_PS)) {
            free_level(e & PTE_ADDR, level - 1);
        }
    }
    pmm_free(phys);
}

void vmm_free_space(uint64_t pml4) {
    sched_quiesce_space(pml4);
    uint64_t *t = tbl(pml4);
    for (int i = 0; i < 256; i++) {
        if (t[i] & PTE_P) free_level(t[i] & PTE_ADDR, 3);
    }
    pmm_free(pml4);
}

static uint64_t prot_flags(int prot) {
    return PTE_U | (prot & VM_W ? PTE_W : 0) | (prot & VM_X ? 0 : pte_nx);
}

/* Map fresh zeroed pages. Pages that are already mapped keep their contents and gain the
   requested permissions (two ELF segments can share a page). */
int vmm_user_alloc(uint64_t pml4, uint64_t va, uint64_t size, int prot) {
    sched_quiesce_space(pml4);
    uint64_t start = ALIGN_DOWN(va, PAGE_SIZE), end = ALIGN_UP(va + size, PAGE_SIZE);
    for (uint64_t a = start; a < end; a += PAGE_SIZE) {
        bool huge = false;
        uint64_t *pte = walk(pml4, a, &huge);
        if (pte && (*pte & PTE_P)) {
            if (huge) return -EINVAL;
            if (prot & VM_W) *pte |= PTE_W;
            if (prot & VM_X) *pte &= ~PTE_NX;
            invlpg(a);
            continue;
        }
        uint64_t p = pmm_alloc_zeroed();
        if (!p) return -ENOMEM;
        if (!vmm_map_page(pml4, a, p, prot_flags(prot))) {
            pmm_free(p);
            return -ENOMEM;
        }
    }
    return 0;
}

/* mprotect: set exactly these permissions on already-mapped user pages */
int vmm_user_protect(uint64_t pml4, uint64_t va, uint64_t size, int prot) {
    sched_quiesce_space(pml4);
    uint64_t start = ALIGN_DOWN(va, PAGE_SIZE), end = ALIGN_UP(va + size, PAGE_SIZE);
    for (uint64_t a = start; a < end; a += PAGE_SIZE) {
        bool huge = false;
        uint64_t *pte = walk(pml4, a, &huge);
        if (!pte || !(*pte & PTE_P) || huge || !(*pte & PTE_U)) return -ENOMEM;
    }
    for (uint64_t a = start; a < end; a += PAGE_SIZE) {
        bool huge = false;
        uint64_t *pte = walk(pml4, a, &huge);
        *pte = (*pte & ~(PTE_W | PTE_NX)) | (prot_flags(prot) & (PTE_W | PTE_NX));
        invlpg(a);
    }
    return 0;
}

void vmm_user_free(uint64_t pml4, uint64_t va, uint64_t size) {
    uint64_t start = ALIGN_DOWN(va, PAGE_SIZE), end = ALIGN_UP(va + size, PAGE_SIZE);
    for (uint64_t a = start; a < end; a += PAGE_SIZE) {
        uint64_t old = vmm_unmap_page(pml4, a);
        if ((old & PTE_P) && !(old & PTE_SHARED)) pmm_free(old & PTE_ADDR);
    }
}

int vmm_copy_to_space(uint64_t pml4, uint64_t va, const void *src, size_t n) {
    sched_quiesce_space(pml4);
    const uint8_t *s = src;
    while (n) {
        uint64_t pa = vmm_translate(pml4, va);
        if (!pa) return -EFAULT;
        size_t chunk = MIN(n, PAGE_SIZE - (va & 0xFFF));
        if (s) memcpy(phys_to_virt(pa), s, chunk);
        else memset(phys_to_virt(pa), 0, chunk);
        if (s) s += chunk;
        va += chunk;
        n -= chunk;
    }
    return 0;
}

/* Demand-grow user stacks. */
bool vmm_handle_user_fault(uint64_t addr, uint64_t err) {
    struct task *t = current_task;
    if (!t || !t->pml4 || t->pml4 == kernel_pml4) return false;
    if (err & 1) return false; /* protection violation, not a missing page */
    if (addr >= USER_STACK_TOP - USER_STACK_MAX && addr < USER_STACK_TOP) {
        return vmm_user_alloc(t->pml4, ALIGN_DOWN(addr, PAGE_SIZE), PAGE_SIZE, VM_W) == 0;
    }
    return false;
}

/* ---- vmalloc: virtually contiguous kernel memory from scattered frames ---- */
static uint64_t vmalloc_next = VMALLOC_BASE;

void *vmalloc(size_t pages) {
    uint64_t f = irq_save();
    uint64_t va = vmalloc_next;
    vmalloc_next += (pages + 1) * PAGE_SIZE; /* leave a guard page */
    irq_restore(f);
    for (size_t i = 0; i < pages; i++) {
        uint64_t p = pmm_alloc_zeroed();
        if (!p) {
            vfree((void *)va, i);
            return NULL;
        }
        vmm_map_page(kernel_pml4, va + i * PAGE_SIZE, p, PTE_W | pte_nx);
    }
    return (void *)va;
}

/* Map existing frames at fresh kernel addresses with the given flags (e.g. read+execute only). */
void *vmalloc_map_phys(uint64_t pa, size_t pages, uint64_t flags) {
    uint64_t f = irq_save();
    uint64_t va = vmalloc_next;
    vmalloc_next += (pages + 1) * PAGE_SIZE;
    irq_restore(f);
    for (size_t i = 0; i < pages; i++) vmm_map_page(kernel_pml4, va + i * PAGE_SIZE, pa + i * PAGE_SIZE, flags);
    return (void *)va;
}

void vfree(void *ptr, size_t pages) {
    uint64_t va = (uint64_t)ptr;
    for (size_t i = 0; i < pages; i++) {
        uint64_t old = vmm_unmap_page(kernel_pml4, va + i * PAGE_SIZE);
        if (old & PTE_P) pmm_free(old & PTE_ADDR);
    }
}
