/* Boundary-tag allocator on top of sbrk(), with an explicit free list.
   Large blocks get their own anonymous mapping instead: freeing one returns
   its pages at once, and a growing large buffer never leaves a hole in the
   break heap that only smaller requests could reuse. */
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>
#include <errno.h>
#include <sys/mman.h>
#include "nocturne.h"

#define HDR      16
#define USED     1
#define MAPPED   2   /* the block is a whole mapping; size is its page-rounded length */
#define MAP_MIN  (1024 * 1024)
#define MIN_BLK  32
#define GROW_MIN (256 * 1024)
#define TRIM_KEEP GROW_MIN
#define TRIM_MIN  GROW_MIN

typedef struct blk {
    size_t size; /* including header; bit0 = used */
    size_t prev; /* size of the previous block (0 if first) */
} blk;

typedef struct fblk {
    size_t size, prev;
    struct fblk *next, *pprev;
} fblk;

static blk *sentinel;
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
/* Bytes obtained from sbrk and bytes in allocated blocks (headers included). */
static size_t heap_total, heap_used;
static size_t mapped_total; /* bytes in large-block mappings, updated atomically */

static void heap_lock(void) {
    uint32_t expected = 0;
    if (__atomic_compare_exchange_n(&allocator_lock, &expected, 1, false,
                                    __ATOMIC_ACQUIRE, __ATOMIC_RELAXED)) return;
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

static void trim_top(blk *b) {
    size_t size = bsize(b);
    /* Keep one normal growth quantum warm. Interior/small frees make no syscall;
       a live tail or a different allocator's break must never be reclaimed. */
    if (size < TRIM_KEEP + TRIM_MIN || next_blk(b) != sentinel) return;
    size_t release = (size - TRIM_KEEP) & ~(size_t)4095;
    char *end = (char *)sentinel + HDR;
    int saved_errno = errno;
    if (sbrk(0) == end && sbrk(-(long)release) == end) {
        heap_total -= release;
        /* Do not touch the old sentinel after its page may have been returned.
           Publish new tags only after the atomic native shrink succeeds. */
        b->size = size - release;
        sentinel = next_blk(b);
        sentinel->size = USED;
        sentinel->prev = bsize(b);
    }
    errno = saved_errno;
}

static int heap_init(void) {
    uintptr_t base = (uintptr_t)sbrk(0);
    size_t pad = (16 - (base & 15)) & 15;
    if (sbrk(pad + HDR) == (void *)-1) return -1;
    sentinel = (blk *)(base + pad);
    sentinel->size = USED;
    sentinel->prev = 0;
    return 0;
}

static int grow(size_t need) {
    size_t n = need + 64 < GROW_MIN ? GROW_MIN : (need + 64 + 4095) & ~(size_t)4095;
    char *p = sbrk(n);
    if (p == (void *)-1) return -1;
    heap_total += n;
    if (p != (char *)sentinel + HDR) {
        /* someone else moved the break (another allocator, as with a program run by `tcc -run`):
           start a new region. The old sentinel stays as the end marker of the previous one. */
        uintptr_t base = (uintptr_t)p;
        size_t pad = (16 - (base & 15)) & 15;
        blk *b = (blk *)(base + pad);
        b->prev = 0;
        b->size = (n - pad - HDR) & ~(size_t)15; /* free */
        sentinel = next_blk(b);
        sentinel->size = USED;
        sentinel->prev = bsize(b);
        fl_push((fblk *)b);
        return 0;
    }
    blk *b = sentinel;
    b->size = n; /* free */
    sentinel = next_blk(b);
    sentinel->size = USED;
    sentinel->prev = n;
    b = coalesce(b);
    fl_push((fblk *)b);
    return 0;
}

static void *malloc_locked(size_t n) {
    if (!sentinel && heap_init() < 0) return NULL;
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
        if (grow(need + HDR) < 0) return NULL;
    }
    return NULL;
}

static void free_locked(void *p) {
    if (!p) return;
    blk *b = (blk *)((char *)p - HDR);
    b->size &= ~(size_t)USED;
    heap_used -= bsize(b);
    b = coalesce(b);
    trim_top(b);
    fl_push((fblk *)b);
}

/* Mapped blocks need no lock: the mapping is the block. Pages come zeroed. */
static void *map_alloc(size_t n) {
    if (n > ((size_t)1 << 40)) return NULL;
    size_t total = (n + HDR + 4095) & ~(size_t)4095;
    blk *b = mmap(NULL, total, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (b == MAP_FAILED) return NULL;
    b->size = total | MAPPED | USED;
    b->prev = 0;
    __atomic_add_fetch(&mapped_total, total, __ATOMIC_RELAXED);
    return (char *)b + HDR;
}
static void map_free(blk *b) {
    size_t total = bsize(b);
    __atomic_sub_fetch(&mapped_total, total, __ATOMIC_RELAXED);
    munmap(b, total);
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
    int saved_errno = errno;
    blk *b = (blk *)((char *)p - HDR);
    if (b->size & MAPPED) map_free(b);
    else {
        heap_lock();
        free_locked(p);
        heap_unlock();
    }
    errno = saved_errno;
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
            size_t keep = (n + HDR + 4095) & ~(size_t)4095;
            if (n >= MAP_MIN && keep < bsize(b)) {
                size_t release = bsize(b) - keep;
                if (!munmap((char *)b + keep, release)) {
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
