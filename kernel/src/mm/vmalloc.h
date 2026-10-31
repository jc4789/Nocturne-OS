/* Kernel virtual memory: runs of pages at addresses of their own in the kernel half, each followed
   by a guard page that stays unmapped (mm/vmalloc.c). */
#pragma once
#include <stddef.h>
#include <stdint.h>

void vmalloc_init(void);        /* after vmm_init */
void vmalloc_purge_start(void); /* once threads run */

/* pages zeroed frames; NULL without memory. Any context but an interrupt handler. */
void *vmalloc(size_t pages);
/* As vmalloc, the frames from node's memory first (mm/pmm.h): for memory another CPU will use. */
void *vmalloc_node(size_t pages, unsigned node);
/* What vmalloc gave, with its size. Changes no page table and waits for nothing: any context, a
   holder of spinlocks too. The addresses and frames come back after a TLB shootdown. */
void vfree(void *p, size_t pages);
/* Existing frames at fresh kernel addresses with the given leaf flags: a run from pa, or frames[i]
   (NULL when a page table could not be allocated; nothing stays mapped then). */
void *vmalloc_map_phys(uint64_t pa, size_t pages, uint64_t flags);
void *vmalloc_map_frames(const uint64_t *frames, size_t pages, uint64_t flags);
/* Takes such a mapping away; the frames stay their owner's, who may use them otherwise once this
   returns: it waits for the shootdown, interrupts on. */
void vunmap(void *p, size_t pages);
