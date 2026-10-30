/* Kernel virtual memory (mm/vmalloc.h): the ranges of [VMALLOC_BASE, VMALLOC_END), each a run of
   pages and the guard page after it.

   A range in use is in `busy`, found by its address when it comes back (its size is checked
   against the one given then). A free range is in `space`, joined with its free neighbours, and
   a new one is the lowest free range long enough. A range given back by vfree waits on `retired`
   with its pages still mapped: vfree changes no page table, so it runs anywhere. Once the retired
   pages pass 1/1024 of RAM (the share pmm's CPU caches keep, mm/pmm.c), the purge thread takes
   them all at once: it unmaps them, shoots them down from every TLB once, then gives back the
   frames and the ranges. Neither an address nor a frame is used again while a CPU may still hold
   its old translation.

   lock guards the three; it is a spinlock (vfree under others' spinlocks), held for tree steps
   only: records are allocated before it is taken and freed after. */
#include "kernel.h"
#include "arch/spin.h"
#include "lib/span.h"
#include "mm/heap.h"
#include "mm/pmm.h"
#include "mm/vmalloc.h"
#include "mm/vmm.h"
#include "sys/sched.h"
#include <stddef.h>

struct area {
    struct span span; /* bytes: the pages and the guard page */
    struct area *next; /* retired, or a record to free */
    bool frames;       /* the frames are the area's (vmalloc), not a mapper's (vmalloc_map_*) */
};
#define AREA(s) ((struct area *)((char *)(s) - offsetof(struct area, span)))

static struct spinlock lock;
static struct span_tree busy, space;
static struct area *retired;
static uint64_t retired_pages, retired_most;
static struct wait_queue purge_wq;

void vmalloc_init(void) {
    struct area *all = kzalloc(sizeof *all);
    if (!all) panic("vmalloc: no memory");
    all->span.start = VMALLOC_BASE;
    all->span.length = VMALLOC_END - VMALLOC_BASE;
    span_insert(&space, &all->span);
    retired_most = MAX(pmm_total_pages() / 1024, (uint64_t)1);
}

/* lock held: a's range joins the free space; the records that are no longer needed go on *spare. */
static void give_back(struct area *a, struct area **spare) {
    uint64_t start = a->span.start, end = start + a->span.length;
    struct span *below = span_below(&space, start), *above = span_from(&space, end);
    if (below && below->start + below->length != start) below = NULL;
    if (above && above->start != end) above = NULL;
    if (!below && !above) {
        span_insert(&space, &a->span);
        return;
    }
    a->next = *spare;
    *spare = a;
    if (above) span_remove(&space, above);
    if (below) {
        span_remove(&space, below);
        below->length += a->span.length + (above ? above->length : 0);
        span_insert(&space, below);
        if (above) {
            AREA(above)->next = *spare;
            *spare = AREA(above);
        }
    } else {
        above->start = start;
        above->length += a->span.length;
        span_insert(&space, above);
    }
}

static void free_records(struct area *list) {
    for (struct area *next; list; list = next) {
        next = list->next;
        kfree(list);
    }
}

/* A range of pages and a guard page, in busy; 0 when there is no record or room for it. */
static uint64_t take(size_t pages, bool frames) {
    if (!pages || pages >= (VMALLOC_END - VMALLOC_BASE) / PAGE_SIZE) return 0;
    uint64_t length = (pages + 1) * PAGE_SIZE;
    struct area *a = kzalloc(sizeof *a), *spare = NULL;
    if (!a) return 0;
    uint64_t f = spin_lock(&lock);
    struct span *s = span_fit(&space, length);
    if (!s) {
        spin_unlock(&lock, f);
        kfree(a);
        return 0;
    }
    uint64_t start = s->start;
    span_remove(&space, s);
    if (s->length > length) {
        s->start += length;
        s->length -= length;
        span_insert(&space, s);
    } else {
        spare = AREA(s);
    }
    a->span.start = start;
    a->span.length = length;
    a->frames = frames;
    span_insert(&busy, &a->span);
    spin_unlock(&lock, f);
    kfree(spare);
    return start;
}

/* lock held: the busy area at va of pages, taken out of busy. A wrong address or size is a bug of
   the caller's that would free another's memory. */
static struct area *busy_take(uint64_t va, size_t pages, bool frames) {
    struct span *s = span_at(&busy, va);
    if (!s || s->start != va || s->length != (pages + 1) * PAGE_SIZE || AREA(s)->frames != frames)
        panic("vmalloc: %s of %p, %lu pages, which is no such range", frames ? "vfree" : "vunmap", (void *)va,
              (unsigned long)pages);
    span_remove(&busy, s);
    return AREA(s);
}

void vfree(void *p, size_t pages) {
    uint64_t f = spin_lock(&lock);
    struct area *a = busy_take((uint64_t)p, pages, true);
    a->next = retired;
    retired = a;
    retired_pages += pages;
    bool full = retired_pages >= retired_most;
    spin_unlock(&lock, f);
    if (full) wq_wake_all(&purge_wq);
}

void *vmalloc(size_t pages) {
    uint64_t va = take(pages, true);
    if (!va) return NULL;
    for (size_t i = 0; i < pages; i++) {
        uint64_t frame = pmm_alloc_zeroed();
        if (!frame || !vmm_map_page(kernel_pml4, va + i * PAGE_SIZE, frame, PTE_W | PTE_NX)) {
            if (frame) pmm_free(frame);
            vfree((void *)va, pages); /* the pages mapped so far go with the range */
            return NULL;
        }
    }
    return (void *)va;
}

static void give_back_now(struct area *a) {
    struct area *spare = NULL;
    uint64_t f = spin_lock(&lock);
    give_back(a, &spare);
    spin_unlock(&lock, f);
    free_records(spare);
}

/* Unmapped and shot down at once: the frames stay the mapper's. */
static void unmap_now(uint64_t va, size_t pages) {
    struct vmm_gather g;
    vmm_gather_init(&g, kernel_pml4);
    vmm_kernel_unmap(&g, va, pages, false);
    vmm_gather_finish(&g);
}

static void *map_frames(const uint64_t *frames, uint64_t pa, size_t pages, uint64_t flags) {
    uint64_t va = take(pages, false);
    if (!va) return NULL;
    for (size_t i = 0; i < pages; i++)
        if (!vmm_map_page(kernel_pml4, va + i * PAGE_SIZE, frames ? frames[i] : pa + i * PAGE_SIZE, flags)) {
            uint64_t f = spin_lock(&lock);
            struct area *a = busy_take(va, pages, false);
            spin_unlock(&lock, f);
            unmap_now(va, i);
            give_back_now(a);
            return NULL;
        }
    return (void *)va;
}

void *vmalloc_map_phys(uint64_t pa, size_t pages, uint64_t flags) { return map_frames(NULL, pa, pages, flags); }
void *vmalloc_map_frames(const uint64_t *frames, size_t pages, uint64_t flags) {
    return map_frames(frames, 0, pages, flags);
}

void vunmap(void *p, size_t pages) {
    uint64_t f = spin_lock(&lock);
    struct area *a = busy_take((uint64_t)p, pages, false);
    spin_unlock(&lock, f);
    unmap_now((uint64_t)p, pages);
    give_back_now(a);
}

/* Every retired range at once: one shootdown for all of them. */
static void purge(struct area *list) {
    struct vmm_gather g;
    vmm_gather_init(&g, kernel_pml4);
    for (struct area *a = list; a; a = a->next)
        vmm_kernel_unmap(&g, a->span.start, a->span.length / PAGE_SIZE - 1, true);
    vmm_gather_finish(&g);
    struct area *spare = NULL;
    uint64_t f = spin_lock(&lock);
    for (struct area *a = list, *next; a; a = next) {
        next = a->next;
        give_back(a, &spare);
    }
    spin_unlock(&lock, f);
    free_records(spare);
}

static void purge_thread(void *arg) {
    (void)arg;
    for (;;) {
        /* vfree adds under lock and wakes after: looked at under lock, no wake is missed */
        uint64_t f = spin_lock(&lock);
        while (retired_pages < retired_most) {
            wq_wait_locked(&purge_wq, &lock);
            spin_take(&lock);
        }
        struct area *list = retired;
        retired = NULL;
        retired_pages = 0;
        spin_unlock(&lock, f);
        purge(list);
    }
}

void vmalloc_purge_start(void) {
    if (!kthread_create("vmalloc", purge_thread, NULL)) panic("vmalloc: no purge thread");
}
