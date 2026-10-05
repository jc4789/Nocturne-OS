/* Boundary-tag allocator on top of sbrk(), with an explicit free list. */
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include "nocturne.h"

#define HDR      16
#define USED     1
#define MIN_BLK  32
#define GROW_MIN (256 * 1024)

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
    size_t n = need < GROW_MIN ? GROW_MIN : (need + 4095) & ~(size_t)4095;
    char *p = sbrk(n);
    if (p == (void *)-1) return -1;
    if (p != (char *)sentinel + HDR) return -1; /* heap is not contiguous */
    blk *b = sentinel;
    b->size = n; /* free */
    sentinel = next_blk(b);
    sentinel->size = USED;
    sentinel->prev = n;
    b = coalesce(b);
    fl_push((fblk *)b);
    return 0;
}

void *malloc(size_t n) {
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

void free(void *p) {
    if (!p) return;
    blk *b = (blk *)((char *)p - HDR);
    b->size &= ~(size_t)USED;
    b = coalesce(b);
    fl_push((fblk *)b);
}

void *calloc(size_t n, size_t m) {
    size_t t = n * m;
    if (m && t / m != n) return NULL;
    void *p = malloc(t);
    if (p) memset(p, 0, t);
    return p;
}

void *realloc(void *p, size_t n) {
    if (!p) return malloc(n);
    if (!n) {
        free(p);
        return NULL;
    }
    blk *b = (blk *)((char *)p - HDR);
    size_t have = bsize(b) - HDR;
    if (have >= n) return p;
    blk *nx = next_blk(b);
    size_t need = (n + HDR + 15) & ~(size_t)15;
    if (!(nx->size & USED) && bsize(b) + bsize(nx) >= need) {
        fl_remove((fblk *)nx);
        b->size = (bsize(b) + bsize(nx)) | USED;
        next_blk(b)->prev = bsize(b);
        return p;
    }
    void *q = malloc(n);
    if (!q) return NULL;
    memcpy(q, p, have);
    free(p);
    return q;
}
