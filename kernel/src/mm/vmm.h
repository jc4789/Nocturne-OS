#pragma once
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include "mm/pmm.h"

#define PTE_P      (1ULL << 0)
#define PTE_W      (1ULL << 1)
#define PTE_U      (1ULL << 2)
#define PTE_PWT    (1ULL << 3)
#define PTE_PCD    (1ULL << 4)
#define PTE_PS     (1ULL << 7)
#define PTE_PAT4K  (1ULL << 7)
#define PTE_G      (1ULL << 8)
#define PTE_SHARED (1ULL << 9) /* not owned by this address space: don't free */
#define PTE_SHARED_RO (1ULL << 10) /* read-only shared ABI page: never grant W/X */
#define PTE_DEMAND (1ULL << 11) /* in a non-present user leaf: a reserved page, zero-filled on first touch */
#define PTE_PAT2M  (1ULL << 12)
#define PTE_ADDR   0x000FFFFFFFFFF000ULL
#define PTE_NX     (1ULL << 63)

/* protection for user mappings (readable is implied) */
#define VM_W 1
#define VM_X 2

#define USER_TOP       0x0000800000000000ULL
#define USER_STACK_TOP 0x00007FFFFFF00000ULL
#define USER_STACK_MAX (8ULL << 20)
#define USER_MMAP_BASE 0x0000600000000000ULL
/* The low region ends at 2 GiB, the reach of sign-extended 32-bit addresses;
   the program image and its sbrk heap stay below USER_LOW_BASE. */
#define USER_LOW_BASE  0x0000000040000000ULL
#define USER_LOW_TOP   0x0000000080000000ULL

#define KHEAP_BASE   0xFFFFC00000000000ULL
#define VMALLOC_BASE 0xFFFFD00000000000ULL /* mm/vmalloc.c's ranges, 16 TiB */
#define VMALLOC_END  0xFFFFE00000000000ULL

struct limine_memmap_response;
struct limine_executable_address_response;

extern uint64_t kernel_pml4;

void vmm_init(struct limine_memmap_response *mm, struct limine_executable_address_response *ka);
bool vmm_map_page(uint64_t pml4, uint64_t va, uint64_t pa, uint64_t flags);
uint64_t vmm_unmap_page(uint64_t pml4, uint64_t va);
uint64_t vmm_translate(uint64_t pml4, uint64_t va); /* returns phys or 0 */
uint64_t vmm_get_pte(uint64_t pml4, uint64_t va);
uint64_t vmm_get_leaf(uint64_t pml4, uint64_t va); /* the raw 4 KiB leaf, present or not; 0 when none */
/* Device memory, uncached, at its direct-map address; NULL when a page table could not be allocated
   (the pages mapped before it stay mapped, as any later call for them would map them again). */
void *vmm_map_mmio(uint64_t phys, uint64_t size);
/* Any physical range: RAM through the write-back direct map, everything else as vmm_map_mmio
   (NULL likewise). */
void *vmm_map_phys(uint64_t phys, uint64_t size);
/* The direct map's memory type of RAM pages (frames, or base on when frames is NULL): the leaf's
   PWT/PCD/PAT bits, 0 for write-back. False when splitting large pages found no memory. */
bool vmm_direct_type(const uint64_t *frames, uint64_t base, size_t pages, uint64_t type);
uint64_t vmm_new_space(void);
void vmm_free_space(uint64_t pml4);
uint64_t vmm_user_pages(uint64_t pml4); /* present, non-shared lower-half 4K leaves */
int vmm_user_alloc(uint64_t pml4, uint64_t va, uint64_t size, int prot);
int vmm_user_reserve(uint64_t pml4, uint64_t va, uint64_t size, int prot); /* zeroed on first touch */
/* First touch of a reserved page: 1 when mapped (or already present), 0 when
   va is no reserved page, -N_ERR_NO_MEMORY when no frame is left. */
int vmm_demand_fault(uint64_t pml4, uint64_t va, bool write);
/* shared: pages of one shared mapping whose access the caller checked; absent pages are skipped */
int vmm_user_protect(uint64_t pml4, uint64_t va, uint64_t size, int prot, bool shared);
void vmm_user_free(uint64_t pml4, uint64_t va, uint64_t size);
void vmm_user_write_protect(uint64_t pml4, uint64_t va, uint64_t size); /* present pages lose W for good */
int vmm_copy_to_space(uint64_t pml4, uint64_t va, const void *src, size_t n);
/* The current app's page fault (ring 3, or user_copy), interrupts on; may sleep. */
bool vmm_handle_user_fault(uint64_t addr, uint64_t err);
void vmm_switch(uint64_t pml4); /* PCID対応。gen が進んだ space のtagだけ破棄 */
void vmm_flush_all(void); /* PGEのglobal entryと全PCIDを含めてローカルTLBを破棄 */
void vmm_init_cpu(void);  /* 各CPU: CR3=kernel_pml4の後、PCIDEを揃える */
void vmm_tlb_ipi(void);   /* VEC_TLB_SHOOTDOWN: a shootdown another CPU asked for */
void vmm_tlb_stats(uint64_t *shootdowns, uint64_t *ipis);

/* What a change of page tables leaves to do once their locks are let go: drop the old
   translations of [lo, hi) from the TLBs of the CPUs that may hold them (every CPU, for the kernel
   half), then give back the frames that were mapped there. */
struct vmm_gather {
    uint64_t pml4;
    uint64_t lo, hi; /* lo >= hi: nothing to drop */
    struct pmm_list frames;
};
static inline void vmm_gather_init(struct vmm_gather *g, uint64_t pml4) {
    g->pml4 = pml4;
    g->lo = UINT64_MAX;
    g->hi = 0;
    g->frames.first = NULL;
}
void vmm_gather_finish(struct vmm_gather *g);
/* The kernel half's 4 KiB leaves of [va, va + pages) cleared; their frames join g's when frames. */
void vmm_kernel_unmap(struct vmm_gather *g, uint64_t va, size_t pages, bool frames);
