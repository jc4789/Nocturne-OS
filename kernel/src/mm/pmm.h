#pragma once
/* The physical page allocator: 4 KiB frames, from the memory of the calling CPU's node first.
   Any CPU, any context with interrupts on or off; it never sleeps. */
#include <stdbool.h>
#include <stdint.h>
struct limine_memmap_response;
void pmm_init(struct limine_memmap_response *mm); /* after topology_init and bootmem_by_node */
/* One frame; 0 when there is none. */
uint64_t pmm_alloc(void);
uint64_t pmm_alloc_zeroed(void);
/* One frame whose whole page has an address of at most bits bits (a device's DMA width; 64: any). */
uint64_t pmm_alloc_within(unsigned bits);
/* count contiguous frames whose addresses have at most bits bits, at an address that is a multiple
   of the power of two at or above count pages; 0 when there is no such run. */
uint64_t pmm_alloc_contig(uint64_t count, unsigned bits);
void pmm_free(uint64_t phys);
void pmm_free_contig(uint64_t phys, uint64_t count); /* count frames from phys, as pmm_free each */
/* Frames free on the nodes: the few waiting in the CPUs' caches are not counted. */
uint64_t pmm_free_pages(void);
/* Usable RAM below 1 MiB (the allocator never hands it out): base and page count, 0 when none. */
uint64_t pmm_low_range(uint64_t *pages);
uint64_t pmm_total_pages(void); /* the bootloader's usable frames */
extern uint64_t pmm_highest_usable;

/* Hibernation (dev/hibernate.c) */
struct pmm_range { uint64_t base, end; }; /* physical, page aligned, end exclusive */
unsigned pmm_ram_ranges(const struct pmm_range **out); /* the bootloader's RAM, merged and sorted */
uint64_t pmm_map_signature(void);  /* the same on every boot of the same machine and memory */
uint64_t pmm_page_count(void);     /* one past the highest frame of RAM */
bool pmm_page_used(uint64_t pfn);  /* not free: in use, the bootloader's or the kernel's, or no RAM */
bool pmm_claim(uint64_t pfn);      /* a free frame becomes used: true; false when it was not free */
