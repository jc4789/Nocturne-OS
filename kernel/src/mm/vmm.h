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
#define PTE_A      (1ULL << 5)
#define PTE_D      (1ULL << 6)
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

#define VMALLOC_BASE 0xFFFFD00000000000ULL /* mm/vmalloc.c's ranges, 16 TiB */
#define VMALLOC_END  0xFFFFE00000000000ULL

struct limine_memmap_response;
struct limine_executable_address_response;

extern uint64_t kernel_pml4;

void vmm_init(struct limine_memmap_response *mm, struct limine_executable_address_response *ka);

/* ---- the kernel half ----
   The same in every space (each space's PML4 shares its PDPTs), its leaves global. */

/* A page of a fresh range of the kernel's (vmalloc's): false when a page table could not be made. */
bool vmm_kernel_map(uint64_t va, uint64_t pa, uint64_t flags);
uint64_t vmm_kernel_translate(uint64_t va); /* phys, 0 when not mapped */
/* Device memory, uncached, at its direct-map address; NULL when a page table could not be allocated
   (the pages mapped before it stay mapped, as any later call for them would map them again). */
void *vmm_map_mmio(uint64_t phys, uint64_t size);
/* Any physical range: RAM through the write-back direct map, everything else as vmm_map_mmio
   (NULL likewise). */
void *vmm_map_phys(uint64_t phys, uint64_t size);
/* The direct map's memory type of RAM pages (frames, or base on when frames is NULL): the leaf's
   PWT/PCD/PAT bits, 0 for write-back. False when splitting large pages found no memory. No CPU
   reaches them through the old type once it returns. */
bool vmm_direct_type(const uint64_t *frames, uint64_t base, size_t pages, uint64_t type);

/* ---- address spaces ----
   An app's: the lower half of its own tables (mm/vmm.c). Its holders (the tasks of the app, the
   windows it made) hold a reference each. Its app ends it once: its mappings and tables go, the
   record stays for the holders left, and later calls on it change nothing (those that would map
   fail). */
struct vmm_space;
struct vmm_space *vmm_space_new(void); /* NULL without memory; the caller holds its reference */
void vmm_space_get(struct vmm_space *s);
void vmm_space_put(struct vmm_space *s); /* the last one frees what vmm_space_end left */
void vmm_space_end(struct vmm_space *s); /* no task of it runs any more; no spinlock held */

/* A page into the space: 0 when it is mapped so now (also when the page was there already with
   these permissions or fewer, which it gains), -N_ERR_NO_MEMORY without a page table,
   -N_ERR_EXISTS when another page or wider permissions are there (left as they are: a narrowing is
   shot down, which this never does), -N_ERR_CLOSED when the space ended. Any context. */
int vmm_map_page(struct vmm_space *s, uint64_t va, uint64_t pa, uint64_t flags);
uint64_t vmm_translate(struct vmm_space *s, uint64_t va); /* phys, 0 when not mapped */
uint64_t vmm_get_pte(struct vmm_space *s, uint64_t va);   /* the present leaf, else 0 */
uint64_t vmm_get_leaf(struct vmm_space *s, uint64_t va);  /* the raw 4 KiB leaf, present or not; 0 when none */
uint64_t vmm_user_pages(struct vmm_space *s); /* present, non-shared lower-half 4K leaves */
/* Diagnostic split of the private pages: [0] program image and sbrk heap, [1] low reservations,
   [2] the reservation region, [3] main stack, [4] shared leaves mapped, [5] page-table frames. A
   full walk: only for the rare out-of-memory report. */
void vmm_user_pages_split(struct vmm_space *s, uint64_t out[6]);
/* Fresh zeroed pages; present ones keep their contents and gain prot. Any context. */
int vmm_user_alloc(struct vmm_space *s, uint64_t va, uint64_t size, int prot);
int vmm_user_reserve(struct vmm_space *s, uint64_t va, uint64_t size, int prot); /* zeroed on first touch */
/* First touch of a reserved page: 1 when mapped (or already present), 0 when va is no reserved
   page, -N_ERR_NO_MEMORY when no frame is left. Any context. */
int vmm_demand_fault(struct vmm_space *s, uint64_t va, bool write);
/* The calls that narrow or remove translations return once no CPU uses the old ones (they shoot
   them down): no spinlock held, not in an interrupt's handler.
   protect: shared: pages of one shared mapping whose access the caller checked; absent pages are
   skipped. */
int vmm_user_protect(struct vmm_space *s, uint64_t va, uint64_t size, int prot, bool shared);
void vmm_user_free(struct vmm_space *s, uint64_t va, uint64_t size);
void vmm_user_write_protect(struct vmm_space *s, uint64_t va, uint64_t size); /* present pages lose W for good */
int vmm_copy_to_space(struct vmm_space *s, uint64_t va, const void *src, size_t n);
/* The current app's page fault (ring 3, or user_copy), interrupts on; may sleep. */
bool vmm_handle_user_fault(uint64_t addr, uint64_t err);
/* The access a page fault's error code err names at addr is one the current app's tables give:
   the fault came from a narrower translation cached before (a widening is not shot down), which
   the fault took out of the TLB. Any context. */
bool vmm_fault_spurious(uint64_t addr, uint64_t err);

/* ---- this CPU ---- */
/* The scheduler's switch to a task of space s (NULL: a task of no space, which runs over the
   space in CR3 until that space's next shootdown). Interrupts off. */
void vmm_switch(struct vmm_space *s);
/* This CPU off its space, to the kernel's tables: a task that ends, a CPU that stops. */
void vmm_leave(void);
void vmm_flush_all(void); /* every translation of this CPU's, global ones and every PCID's */
/* Each CPU as it starts, CR3 = kernel_pml4: PCIDs as the first CPU has them. vmm_online once it is
   in the online set (arch/smp.c): what it cached before that is dropped. */
void vmm_init_cpu(void);
void vmm_online(void);
/* The shootdowns other CPUs asked of this one: VEC_TLB_SHOOTDOWN's handler, and a CPU that waits
   with its interrupts off (arch/spin.h), so that no shootdown waits on a CPU that waits on it. */
void vmm_tlb_poll(void);
void vmm_tlb_stats(uint64_t *shootdowns, uint64_t *ipis, uint64_t *evictions);

/* What a change of the tables leaves to do once their lock is let go: drop the old translations
   of [lo, hi) from every CPU's TLB, then give back the frames that were mapped there. */
struct vmm_gather {
    struct vmm_space *space;
    uint64_t lo, hi; /* lo >= hi: nothing to drop */
    bool counted;    /* space's pending counts this */
    struct pmm_list frames;
};
void vmm_gather_init(struct vmm_gather *g); /* a change of the kernel half */
void vmm_gather_finish(struct vmm_gather *g); /* no spinlock held */
/* The kernel half's 4 KiB leaves of [va, va + pages) cleared; their frames join g's when frames. */
void vmm_kernel_unmap(struct vmm_gather *g, uint64_t va, size_t pages, bool frames);
