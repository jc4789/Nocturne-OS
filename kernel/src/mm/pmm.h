#pragma once
#include <stdbool.h>
#include <stdint.h>
struct limine_memmap_response;
void pmm_init(struct limine_memmap_response *mm);
uint64_t pmm_alloc(void);
/* One frame whose entire page is below the exclusive byte limit. Uses the ordinary
   allocation hint within that range, including for repeated restricted DMA allocations. */
uint64_t pmm_alloc_below(uint64_t limit);
uint64_t pmm_alloc_zeroed(void);
uint64_t pmm_alloc_contig(uint64_t count);
void pmm_free(uint64_t phys);
void pmm_release_range(uint64_t phys, uint64_t size); /* consumed bootloader memory → allocator */
uint64_t pmm_free_pages(void);
/* Usable RAM below 1 MiB (the allocator never hands it out): base and page count, 0 when none. */
uint64_t pmm_low_range(uint64_t *pages);
uint64_t pmm_total_pages(void);
extern uint64_t pmm_highest_usable;

/* Hibernation (dev/hibernate.c) */
struct pmm_range { uint64_t base, end; }; /* physical, page aligned, end exclusive */
unsigned pmm_ram_ranges(const struct pmm_range **out); /* the bootloader's RAM, merged and sorted */
bool pmm_ram_complete(void);       /* false: the map had more ranges than are kept */
uint64_t pmm_map_signature(void);  /* the same on every boot of the same machine and memory */
uint64_t pmm_page_count(void);     /* frames the bitmap covers */
bool pmm_page_used(uint64_t pfn);
bool pmm_claim(uint64_t pfn);      /* a free frame becomes used: true; false when it was in use */
