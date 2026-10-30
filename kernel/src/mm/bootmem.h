#pragma once
/* Memory before the page allocator: what the machine's tables (topology, ACPI, the page
   allocator's own records) need while nothing can allocate yet. It is carved from the
   bootloader's usable ranges, from the top of the highest one down, and never freed: the page
   allocator, once built, takes every span given out as used (bootmem_handoff). The first
   megabyte is never given out; it is the wake code's (pmm_low_range). */
#include <stddef.h>
#include <stdint.h>

struct limine_memmap_response;
void bootmem_init(struct limine_memmap_response *mm); /* the first CPU, before anything allocates */
/* bytes, zeroed, at an address that is a multiple of align (a power of two up to PAGE_SIZE);
   through the direct map. Stops the system when no range holds it. */
void *bootmem_alloc(size_t bytes, size_t align);
/* The page allocator, once: taken(base, end, context) for each span given out, page aligned,
   end exclusive. Nothing is given out after it. */
void bootmem_handoff(void (*taken)(uint64_t base, uint64_t end, void *context), void *context);
