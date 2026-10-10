/* Boundary-tag allocator on top of sbrk(), with an explicit free list. */
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>
#include <errno.h>
#include "nocturne.h"

#define HDR      16
#define USED     1
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
static fblk *freelist;
static uint32_t allocator_lock;

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

static size_t bsize(blk *b) { return b->size & ~(size_t)USED; }
static blk *next_blk(blk *b) { return (blk *)((char *)b + bsize(b)); }
static blk *prev_blk(blk *b) { return (blk *)((char *)b - b->prev); }

static void fl_remove(fblk *f) {
    if (f->pprev) f->pprev->next = f->next;
    else freelist = f->next;
    if (f->next) f->next->pprev = f->pprev;
}

static void fl_push(fblk *f) {
    f->next = freelist;
    f->pprev = NULL;
    if (freelist) freelist->pprev = f;
    freelist = f;
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
        for (fblk *f = freelist; f; f = f->next) {
            if (f->size < need) continue;
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
    b = coalesce(b);
    trim_top(b);
    fl_push((fblk *)b);
}

void *calloc(size_t n, size_t m) {
    size_t t = n * m;
    if (m && t / m != n) return NULL;
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
        return p;
    }
    void *q = malloc_locked(n);
    if (!q) return NULL;
    memcpy(q, p, have);
    free_locked(p);
    return q;
}

void *malloc(size_t n) {
    heap_lock();
    void *p = malloc_locked(n);
    heap_unlock();
    return p;
}
void free(void *p) {
    if (!p) return;
    int saved_errno = errno;
    heap_lock();
    free_locked(p);
    heap_unlock();
    errno = saved_errno;
}
void *realloc(void *p, size_t n) {
    heap_lock();
    void *q = realloc_locked(p, n);
    heap_unlock();
    return q;
}
