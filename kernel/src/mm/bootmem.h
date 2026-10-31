#pragma once
/* Memory before the page allocator: what the machine's tables (topology, ACPI, the page
   allocator's own records) need while nothing can allocate yet. It is carved from the
   bootloader's usable ranges, from the top down, and never freed: the page allocator, once
   built, takes what was never given out as its free memory (bootmem_handoff). The first
   megabyte is never given out; it is the wake code's (pmm_low_range). */
#include <stddef.h>
#include <stdint.h>

struct limine_memmap_response;
void bootmem_init(struct limine_memmap_response *mm); /* the first CPU, before anything allocates */
/* Once the nodes are known (topology_init): each usable range is cut where the nodes' memory
   meets, so that an allocation can be placed on a node. */
void bootmem_by_node(void);
/* bytes, zeroed, at an address that is a multiple of align (a power of two up to PAGE_SIZE);
   through the direct map. Stops the system when no range holds it. _node: on that node's memory
   when it has room, elsewhere when not. */
void *bootmem_alloc(size_t bytes, size_t align);
void *bootmem_alloc_node(size_t bytes, size_t align, unsigned node);
/* The page allocator, once: untaken(base, end, node, context) for each span never given out, page
   aligned, end exclusive, in address order, each on one node. Nothing is given out after it. */
void bootmem_handoff(void (*untaken)(uint64_t base, uint64_t end, unsigned node, void *context), void *context);
