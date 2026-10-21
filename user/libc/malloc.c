/* Boundary-tag allocator over n_memory regions, with segregated free lists.
   A region is [blocks ... end marker]: its first block has prev 0 and the
   marker is a used block of size 0, so blocks never coalesce across regions.
   A free tail goes back to the system page by page, and a region that becomes
   wholly free goes back at once (except the newest, kept warm for reuse).
   Large blocks get a region of their own: freeing one returns its pages at
   once, and a growing large buffer never leaves a hole that only smaller
   requests could reuse. */
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>
#include "nocturne.h"

#define HDR      16
#define USED     1
#define MAPPED   2   /* the block is a whole region; size is its page-rounded length */
#define MAP_MIN  (1024 * 1024)
#define MIN_BLK  32
#define GROW_MIN (256 * 1024)
#define GROW_MAX (8 * 1024 * 1024)
#define TRIM_MIN  GROW_MIN
#define PAGE     4096u

typedef struct blk {
    size_t size; /* including header; bit0 = used */
    size_t prev; /* size of the previous block (0 if first) */
} blk;

typedef struct fblk {
    size_t size, prev;
    struct fblk *next, *pprev;
} fblk;

static blk *newest_end; /* end marker of the newest region */
/* Segregated free lists: exact 16-byte classes below SMALL_LIMIT, then eight
   classes per power of two. A bitmap finds the next non-empty class, so a
   request never walks blocks that cannot satisfy it. */
#define SMALL_LIMIT 1024u
#define SMALL_BINS  (SMALL_LIMIT / 16u)
#define SUB_BITS    3u
#define SUB_BINS    (1u << SUB_BITS)
#define FIRST_LOG   10u /* log2(SMALL_LIMIT) */
#define BINS        (SMALL_BINS + (64u - FIRST_LOG) * SUB_BINS)
#define MAP_WORDS   ((BINS + 63u) / 64u)
static fblk *bins[BINS];
static uint64_t bin_map[MAP_WORDS];
static uint32_t allocator_lock;
/* Bytes in regions and bytes in allocated blocks (headers included). */
static size_t heap_total, heap_used;
static size_t mapped_total; /* bytes in large-block mappings, updated atomically */

static void heap_lock(void) {
    uint32_t expected = 0;
    if (__atomic_compare_exchange_n(&allocator_lock, &expected, 1, false,
                                    __ATOMIC_ACQUIRE, __ATOMIC_RELAXED)) return;
    /* Critical sections are a few hundred instructions, while a wait and its
       wake are system calls the BSP services for every CPU. Spin briefly
       first, or parallel workers allocating at once serialize on the BSP. A
       sleeper re-marks the lock contended when it wakes, so taking it
       uncontended here never loses that sleeper's wake. */
    for (unsigned spin = 0; spin < 256; spin++) {
        __asm__ volatile("pause");
        expected = 0;
        if (!__atomic_load_n(&allocator_lock, __ATOMIC_RELAXED) &&
            __atomic_compare_exchange_n(&allocator_lock, &expected, 1, false,
                                        __ATOMIC_ACQUIRE, __ATOMIC_RELAXED)) return;
    }
    while (__atomic_exchange_n(&allocator_lock, 2, __ATOMIC_ACQUIRE)) {
#ifndef MALLOCTRIM_MODEL
        wait_on_address(&allocator_lock, 2, UINT32_MAX);
#else
        __asm__ volatile("pause");
#endif
    }
}
static void heap_unlock(void) {
    if (__atomic_exchange_n(&allocator_lock, 0, __ATOMIC_RELEASE) == 2) {
#ifndef MALLOCTRIM_MODEL
        wake_address(&allocator_lock, 1);
#endif
    }
}

static size_t bsize(blk *b) { return b->size & ~(size_t)(USED | MAPPED); }
static blk *next_blk(blk *b) { return (blk *)((char *)b + bsize(b)); }
static blk *prev_blk(blk *b) { return (blk *)((char *)b - b->prev); }

/* The class holding blocks of this size: every block in a class is at least
   the class's lower bound and below the next class's. */
static unsigned bin_of(size_t size) {
    if (size < SMALL_LIMIT) return (unsigned)(size / 16u);
    unsigned log = 63u - (unsigned)__builtin_clzll((unsigned long long)size);
    unsigned sub = (unsigned)(size >> (log - SUB_BITS)) & (SUB_BINS - 1u);
    return SMALL_BINS + (log - FIRST_LOG) * SUB_BINS + sub;
}
/* A class whose every block fits a request of this size. */
static unsigned bin_fitting(size_t size) {
    if (size < SMALL_LIMIT) return (unsigned)(size / 16u);
    unsigned log = 63u - (unsigned)__builtin_clzll((unsigned long long)size);
    size_t step = (size_t)1 << (log - SUB_BITS);
    unsigned bin = bin_of(size);
    return (size & (step - 1)) ? bin + 1u : bin;
}
static unsigned bin_next(unsigned from) {
    for (unsigned word = from / 64u; word < MAP_WORDS; word++) {
        uint64_t bits = bin_map[word];
        if (word == from / 64u) bits &= ~(uint64_t)0 << (from % 64u);
        if (bits) return word * 64u + (unsigned)__builtin_ctzll(bits);
    }
    return BINS;
}

static void fl_remove(fblk *f) {
    unsigned bin = bin_of(f->size);
    if (f->pprev) f->pprev->next = f->next;
    else if (!(bins[bin] = f->next)) bin_map[bin / 64u] &= ~((uint64_t)1 << (bin % 64u));
    if (f->next) f->next->pprev = f->pprev;
}

static void fl_push(fblk *f) {
    unsigned bin = bin_of(f->size);
    f->next = bins[bin];
    f->pprev = NULL;
    if (bins[bin]) bins[bin]->pprev = f;
    else bin_map[bin / 64u] |= (uint64_t)1 << (bin % 64u);
    bins[bin] = f;
}

/* A free block of at least need bytes: the head of the first class that
   wholly fits, or else a first fit among the smaller blocks of need's own
   class when no larger block exists. */
static fblk *fl_find(size_t need) {
    unsigned bin = bin_next(bin_fitting(need));
    if (bin < BINS) return bins[bin];
    for (fblk *f = bins[bin_of(need)]; f; f = f->next)
        if (f->size >= need) return f;
    return NULL;
}

static blk *coalesce(blk *b) {
    if (b->prev) {
        blk *p = prev_blk(b);
        if (!(p->size & USED)) {
            fl_remove((fblk *)p);
            p->size += bsize(b);
            b = p;
        }
    }
    blk *n = next_blk(b);
    if (!(n->size & USED)) {
        fl_remove((fblk *)n);
        b->size += bsize(n);
    }
    next_blk(b)->prev = bsize(b);
    return b;
}

static bool is_end(blk *b) { return b->size == USED; }

/* The size grow() reserves next: regions grow with the heap. */
static size_t grow_step(void) {
    size_t step = heap_total / 16;
    if (step < GROW_MIN) step = GROW_MIN;
    if (step > GROW_MAX) step = GROW_MAX;
    return step;
}

/* Give pages back after free block b, which is not on a free list yet.
   Returns true when b's whole region went back (b is gone). Interior and
   small frees make no system call; a live block after b is never touched. */
static bool trim(blk *b) {
    blk *end = next_blk(b);
    if (!is_end(end)) return false;
    size_t size = bsize(b);
    if (!b->prev && end != newest_end) {
        /* the region is wholly free: regions start at the first block */
        if (n_memory_release(b, size + HDR) < 0) return false;
        heap_total -= size + HDR;
        return true;
    }
    /* Keep one growth quantum warm in the newest region: the one grow() would
       reserve next, so freeing and reallocating a block of up to that size never
       releases and re-reserves the region's tail. Elsewhere keep only what the
       block's own tags need. */
    size_t keep = end == newest_end ? grow_step() : MIN_BLK;
    if (size < keep + TRIM_MIN) return false;
    uintptr_t kept_end = ((uintptr_t)b + keep + HDR + PAGE - 1) & ~(uintptr_t)(PAGE - 1);
    uintptr_t old_end = (uintptr_t)end + HDR;
    size_t release = old_end - kept_end;
    /* Publish new tags only after the pages are really returned; the old end
       marker lies inside the released range. */
    if (n_memory_release((void *)kept_end, release) == 0) {
        heap_total -= release;
        b->size = kept_end - HDR - (uintptr_t)b;
        blk *marker = next_blk(b);
        marker->size = USED;
        marker->prev = bsize(b);
        if (end == newest_end) newest_end = marker;
    }
    return false;
}

/* A new region for at least need bytes. Regions grow with the heap, so a
   large heap is not split into thousands of small ones; pages are committed
   when reserved, so the growth stays a small part of what is in use. */
static int grow(size_t need) {
    size_t step = grow_step();
    size_t n = need + HDR > step ? need + HDR : step;
    n = (n + PAGE - 1) & ~(size_t)(PAGE - 1);
    void *region;
    if (n_memory_reserve(n, N_MEMORY_READ_WRITE, &region) < 0) return -1;
    blk *b = region;
    heap_total += n;
    b->prev = 0;
    b->size = n - HDR; /* free */
    blk *old_end = newest_end;
    newest_end = next_blk(b);
    newest_end->size = USED;
    newest_end->prev = bsize(b);
    fl_push((fblk *)b);
    /* The previous newest region kept its free tail warm; that is now the new
       region's job, so trim it like any older region. */
    if (old_end) {
        blk *last = prev_blk(old_end);
        if (!(last->size & USED)) {
            fl_remove((fblk *)last);
            if (!trim(last)) fl_push((fblk *)last);
        }
    }
    return 0;
}

static void *malloc_locked(size_t n) {
    if (n > ((size_t)1 << 40)) return NULL;
    size_t need = (n + HDR + 15) & ~(size_t)15;
    if (need < MIN_BLK) need = MIN_BLK;
    for (int attempt = 0; attempt < 2; attempt++) {
        fblk *f = fl_find(need);
        if (f) {
            fl_remove(f);
            blk *b = (blk *)f;
            size_t rest = bsize(b) - need;
            if (rest >= MIN_BLK) {
                b->size = need;
                blk *r = next_blk(b);
                r->size = rest;
                r->prev = need;
                next_blk(r)->prev = rest;
                fl_push((fblk *)r);
            }
            b->size |= USED;
            heap_used += bsize(b);
            return (char *)b + HDR;
        }
        if (grow(need) < 0) return NULL;
    }
    return NULL;
}

static void free_locked(void *p) {
    if (!p) return;
    blk *b = (blk *)((char *)p - HDR);
    b->size &= ~(size_t)USED;
    heap_used -= bsize(b);
    b = coalesce(b);
    if (!trim(b)) fl_push((fblk *)b);
}

/* Large blocks need no lock: the region is the block. Pages come zeroed. */
static void *map_alloc(size_t n) {
    if (n > ((size_t)1 << 40)) return NULL;
    size_t total = (n + HDR + PAGE - 1) & ~(size_t)(PAGE - 1);
    void *region;
    if (n_memory_reserve(total, N_MEMORY_READ_WRITE, &region) < 0) return NULL;
    blk *b = region;
    b->size = total | MAPPED | USED;
    b->prev = 0;
    __atomic_add_fetch(&mapped_total, total, __ATOMIC_RELAXED);
    return (char *)b + HDR;
}
static void map_free(blk *b) {
    size_t total = bsize(b);
    __atomic_sub_fetch(&mapped_total, total, __ATOMIC_RELAXED);
    n_memory_release(b, total);
}

void *calloc(size_t n, size_t m) {
    size_t t = n * m;
    if (m && t / m != n) return NULL;
    if (t >= MAP_MIN) return map_alloc(t);
    void *p = malloc(t);
    if (p) memset(p, 0, t);
    return p;
}

static void *realloc_locked(void *p, size_t n) {
    if (!p) return malloc_locked(n);
    if (!n) {
        free_locked(p);
        return NULL;
    }
    blk *b = (blk *)((char *)p - HDR);
    size_t have = bsize(b) - HDR;
    if (have >= n) return p;
    blk *nx = next_blk(b);
    size_t need = (n + HDR + 15) & ~(size_t)15;
    if (!(nx->size & USED) && bsize(b) + bsize(nx) >= need) {
        size_t total = bsize(b) + bsize(nx), rest = total - need;
        fl_remove((fblk *)nx);
        heap_used -= bsize(b);
        /* Growing a small buffer must not pin its entire adjacent free hole. */
        if (rest >= MIN_BLK) {
            b->size = need | USED;
            blk *r = next_blk(b);
            r->size = rest;
            r->prev = need;
            next_blk(r)->prev = rest;
            fl_push((fblk *)r);
        } else {
            b->size = total | USED;
            next_blk(b)->prev = total;
        }
        heap_used += bsize(b);
        return p;
    }
    void *q = malloc_locked(n);
    if (!q) return NULL;
    memcpy(q, p, have);
    free_locked(p);
    return q;
}

void *malloc(size_t n) {
    if (n >= MAP_MIN) return map_alloc(n);
    heap_lock();
    void *p = malloc_locked(n);
    heap_unlock();
    return p;
}
void free(void *p) {
    if (!p) return;
    blk *b = (blk *)((char *)p - HDR);
    if (b->size & MAPPED) map_free(b);
    else {
        heap_lock();
        free_locked(p);
        heap_unlock();
    }
}
void malloc_usage(size_t *heap, size_t *used) {
    size_t mapped = __atomic_load_n(&mapped_total, __ATOMIC_RELAXED);
    heap_lock();
    if (heap) *heap = heap_total + mapped;
    if (used) *used = heap_used + mapped;
    heap_unlock();
}
void *realloc(void *p, size_t n) {
    if (!p) return malloc(n);
    if (!n) { free(p); return NULL; }
    blk *b = (blk *)((char *)p - HDR);
    size_t have = bsize(b) - HDR;
    if (b->size & MAPPED) {
        if (n <= have) {
            /* Give back whole pages a shrink no longer needs. */
            size_t keep = (n + HDR + PAGE - 1) & ~(size_t)(PAGE - 1);
            if (n >= MAP_MIN && keep < bsize(b)) {
                size_t release = bsize(b) - keep;
                if (n_memory_release((char *)b + keep, release) == 0) {
                    __atomic_sub_fetch(&mapped_total, release, __ATOMIC_RELAXED);
                    b->size = keep | MAPPED | USED;
                }
            }
            if (n >= MAP_MIN) return p;
        }
    } else if (n < MAP_MIN || have >= n) {
        heap_lock();
        void *q = realloc_locked(p, n);
        heap_unlock();
        return q;
    }
    /* Crossing the threshold either way moves the block. */
    void *q = malloc(n);
    if (!q) return NULL;
    memcpy(q, p, have < n ? have : n);
    free(p);
    return q;
}
