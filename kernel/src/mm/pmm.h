#pragma once
#include <stdint.h>
struct limine_memmap_response;
void pmm_init(struct limine_memmap_response *mm);
uint64_t pmm_alloc(void);
uint64_t pmm_alloc_zeroed(void);
uint64_t pmm_alloc_contig(uint64_t count);
void pmm_free(uint64_t phys);
uint64_t pmm_free_pages(void);
uint64_t pmm_total_pages(void);
extern uint64_t pmm_highest_usable;
