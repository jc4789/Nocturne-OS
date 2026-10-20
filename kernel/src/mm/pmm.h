#pragma once
#include <stdint.h>
struct limine_memmap_response;
void pmm_init(struct limine_memmap_response *mm);
uint64_t pmm_alloc(void);
uint64_t pmm_alloc_zeroed(void);
uint64_t pmm_alloc_contig(uint64_t count);
void pmm_free(uint64_t phys);
void pmm_release_range(uint64_t phys, uint64_t size); /* consumed bootloader memory → allocator */
uint64_t pmm_free_pages(void);
/* Usable RAM below 1 MiB (the allocator never hands it out): base and page count, 0 when none. */
uint64_t pmm_low_range(uint64_t *pages);
uint64_t pmm_total_pages(void);
extern uint64_t pmm_highest_usable;
