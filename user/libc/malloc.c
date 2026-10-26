/* Boundary-tag allocator over n_memory regions, with segregated free lists.
   A region is [blocks ... end marker]: its first block has prev 0 and the
   marker is a used block of size 0, so blocks never coalesce across regions.
   A free tail goes back to the system page by page, and a region that becomes
   wholly free goes back at once (except the newest, kept warm for reuse).
   Large blocks get a region of their own: freeing one returns its pages,
   beyond a few regions kept for the next large request (map_free), and a
   growing large buffer never leaves a hole that only smaller requests could
   reuse. */
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>
#include "nocturne.h"

#define HDR      16
#define USED     1
#define MAPPED   2   /* the block is a whole region; size is its page-rounded length
                        and prev how far into the region the block starts */
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

/* Freed large regions kept for the next large request. A block allocated and
   freed over and over (a video frame, a network or decode buffer) otherwise
   costs, every time, a reservation, a page fault for each 64 KiB first
   written and a release with its TLB shootdown, all in the kernel. A request
   takes the smallest kept region that holds it with at most a quarter to
   spare. At most MAP_KEEP regions are kept, of at most MAP_KEEP_MIN bytes or
   a quarter of the large blocks in use, whichever is more; a free beyond that
   gives the oldest back first. */
#define MAP_KEEP     4
#define MAP_KEEP_MIN (8u * 1024 * 1024)
struct map_kept { char *region; size_t size; };
static struct map_kept map_kept[MAP_KEEP]; /* oldest first; under the heap lock */
static unsigned map_kept_count;
static size_t map_kept_bytes;

static void map_kept_remove(unsigned i) {
    map_kept_bytes -= map_kept[i].size;
    map_kept_count--;
    memmove(&map_kept[i], &map_kept[i + 1], (map_kept_count - i) * sizeof *map_kept);
}
static char *map_take(size_t total, size_t *size) {
    heap_lock();
    unsigned best = MAP_KEEP;
    for (unsigned i = 0; i < map_kept_count; i++)
        if (map_kept[i].size >= total && map_kept[i].size - total <= total / 4 &&
            (best == MAP_KEEP || map_kept[i].size < map_kept[best].size)) best = i;
    char *region = NULL;
    if (best < MAP_KEEP) {
        region = map_kept[best].region;
        *size = map_kept[best].size;
        map_kept_remove(best);
    }
    heap_unlock();
    return region;
}

/* Large blocks take the heap lock only for the kept regions: the region is
   the block. With zero, its bytes are zero (fresh pages come zeroed). */
static void *map_alloc(size_t n, bool zero) {
    if (n > ((size_t)1 << 40)) return NULL;
    size_t total = (n + HDR + PAGE - 1) & ~(size_t)(PAGE - 1), size = total;
    char *region = map_take(total, &size);
    if (region) {
        if (zero) memset(region + HDR, 0, n);
    } else {
        void *fresh;
        if (n_memory_reserve(total, N_MEMORY_READ_WRITE, &fresh) < 0) return NULL;
        region = fresh;
    }
    blk *b = (blk *)region;
    b->size = size | MAPPED | USED;
    b->prev = 0;
    __atomic_add_fetch(&mapped_total, size, __ATOMIC_RELAXED);
    return (char *)b + HDR;
}
/* An aligned_alloc block starts inside its region; the others at its start. */
static char *map_region(blk *b) { return (char *)b - b->prev; }
static void map_free(blk *b) {
    char *region = map_region(b);
    size_t total = bsize(b);
    size_t live = __atomic_sub_fetch(&mapped_total, total, __ATOMIC_RELAXED);
    size_t limit = live / 4 > MAP_KEEP_MIN ? live / 4 : MAP_KEEP_MIN;
    if (total > limit) { n_memory_release(region, total); return; }
    struct map_kept out[MAP_KEEP];
    unsigned released = 0;
    heap_lock();
    while (map_kept_count == MAP_KEEP || map_kept_bytes + total > limit) {
        out[released++] = map_kept[0];
        map_kept_remove(0);
    }
    map_kept[map_kept_count++] = (struct map_kept){region, total};
    map_kept_bytes += total;
    heap_unlock();
    for (unsigned i = 0; i < released; i++) n_memory_release(out[i].region, out[i].size);
}

/* A region whose block's bytes start at a multiple of align. The whole pages
   before and after the block go back at once. */
static void *map_aligned(size_t align, size_t n) {
    if (n > ((size_t)1 << 40) || align > ((size_t)1 << 40)) return NULL;
    size_t total = (align + n + PAGE - 1) & ~(size_t)(PAGE - 1);
    void *mem;
    if (n_memory_reserve(total, N_MEMORY_READ_WRITE, &mem) < 0) return NULL;
    char *region = mem;
    size_t offset = (0 - ((uintptr_t)region + HDR)) & (align - 1);
    size_t lead = offset & ~(size_t)(PAGE - 1);
    if (lead && n_memory_release(region, lead) == 0) region += lead, offset -= lead, total -= lead;
    size_t keep = (offset + HDR + n + PAGE - 1) & ~(size_t)(PAGE - 1);
    if (keep < total && n_memory_release(region + keep, total - keep) == 0) total = keep;
    blk *b = (blk *)(region + offset);
    b->size = total | MAPPED | USED;
    b->prev = offset;
    __atomic_add_fetch(&mapped_total, total, __ATOMIC_RELAXED);
    return (char *)b + HDR;
}

/* A heap block whose bytes start at a multiple of align: a block with room
   for the alignment, whose bytes before that point (at least a block's worth)
   and after the request go back as free blocks. */
static void *aligned_locked(size_t align, size_t n) {
    size_t need = (n + HDR + 15) & ~(size_t)15;
    if (need < MIN_BLK) need = MIN_BLK;
    char *p = malloc_locked(need - HDR + align + MIN_BLK);
    if (!p) return NULL;
    blk *b = (blk *)(p - HDR);
    uintptr_t at = ((uintptr_t)p + align - 1) & ~(uintptr_t)(align - 1);
    if (at != (uintptr_t)p && at - (uintptr_t)p < MIN_BLK) at += align;
    if (at != (uintptr_t)p) {
        size_t gap = at - (uintptr_t)p, total = bsize(b);
        blk *a = (blk *)(at - HDR);
        b->size = gap | USED;
        a->size = (total - gap) | USED;
        a->prev = gap;
        next_blk(a)->prev = total - gap;
        free_locked(p);
        b = a;
    }
    size_t rest = bsize(b) - need;
    if (rest >= MIN_BLK) {
        b->size = need | USED;
        blk *r = next_blk(b);
        r->size = rest | USED;
        r->prev = need;
        next_blk(r)->prev = rest;
        free_locked((char *)r + HDR);
    }
    return (char *)b + HDR;
}

void *calloc(size_t n, size_t m) {
    size_t t = n * m;
    if (m && t / m != n) return NULL;
    if (t >= MAP_MIN) return map_alloc(t, true);
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
    size_t need = (n + HDR + 15) & ~(size_t)15;
    if (need < MIN_BLK) need = MIN_BLK;
    if (bsize(b) >= need) {
        /* A shrink gives back the tail it no longer needs, as a free block of its own. */
        size_t rest = bsize(b) - need;
        if (rest >= MIN_BLK) {
            b->size = need | USED;
            blk *r = next_blk(b);
            r->size = rest | USED;
            r->prev = need;
            next_blk(r)->prev = rest;
            free_locked((char *)r + HDR);
        }
        return p;
    }
    size_t have = bsize(b) - HDR;
    blk *nx = next_blk(b);
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

/* Each thread keeps small freed blocks of each 16-byte class and hands them
   back to its own small requests without the lock. One lock shared by every
   CPU's frees and mallocs serialized parallel work on it (and its contended
   waits on the BSP). A cached block stays a used block to the heap, so it
   neither coalesces nor counts as free; a class holds at most a few KiB, and
   thread_exit returns the thread's cache. Any thread may free any block. */
#ifndef MALLOCTRIM_MODEL
struct malloc_cache {
    void *head[SMALL_BINS];
    uint16_t count[SMALL_BINS];
};
void **__libc_malloc_cache(void);
static unsigned cache_limit(size_t size) {
    size_t n = 8192 / size;
    return n < 4 ? 4 : n > 64 ? 64 : (unsigned)n;
}
static struct malloc_cache *cache_get(bool create) {
    void **slot = __libc_malloc_cache();
    if (!slot) return NULL;
    if (!*slot && create) {
        heap_lock();
        struct malloc_cache *c = malloc_locked(sizeof *c);
        heap_unlock();
        if (c) memset(c, 0, sizeof *c);
        *slot = c;
    }
    return *slot;
}
static void *cache_take(size_t need) {
    struct malloc_cache *c = cache_get(false);
    unsigned k = (unsigned)(need / 16u);
    void *p = c ? c->head[k] : NULL;
    if (p) { c->head[k] = *(void **)p; c->count[k]--; }
    return p;
}
/* A block of size below SMALL_LIMIT; a full class first returns half. */
static bool cache_put(void *p, size_t size) {
    struct malloc_cache *c = cache_get(true);
    if (!c) return false;
    unsigned k = (unsigned)(size / 16u);
    if (c->count[k] >= cache_limit(size)) {
        heap_lock();
        for (unsigned i = c->count[k] / 2; i; i--) {
            void *q = c->head[k];
            c->head[k] = *(void **)q; c->count[k]--;
            free_locked(q);
        }
        heap_unlock();
    }
    *(void **)p = c->head[k]; c->head[k] = p; c->count[k]++;
    return true;
}
void __malloc_thread_exit(void) {
    void **slot = __libc_malloc_cache();
    struct malloc_cache *c = slot ? *slot : NULL;
    if (!c) return;
    *slot = NULL;
    heap_lock();
    for (unsigned k = 0; k < SMALL_BINS; k++)
        while (c->head[k]) { void *q = c->head[k]; c->head[k] = *(void **)q; free_locked(q); }
    free_locked(c);
    heap_unlock();
}
#else
static void *cache_take(size_t need) { (void)need; return NULL; }
static bool cache_put(void *p, size_t size) { (void)p; (void)size; return false; }
#endif

void *malloc(size_t n) {
    if (n >= MAP_MIN) return map_alloc(n, false);
    size_t need = (n + HDR + 15) & ~(size_t)15;
    if (need < MIN_BLK) need = MIN_BLK;
    void *p = need < SMALL_LIMIT ? cache_take(need) : NULL;
    if (p) return p;
    heap_lock();
    p = malloc_locked(n);
    heap_unlock();
    return p;
}
void free(void *p) {
    if (!p) return;
    blk *b = (blk *)((char *)p - HDR);
    if (b->size & MAPPED) map_free(b);
    else if (bsize(b) >= SMALL_LIMIT || !cache_put(p, bsize(b))) {
        heap_lock();
        free_locked(p);
        heap_unlock();
    }
}
/* Heap blocks are 16-byte aligned already. Larger alignments up to a page
   come from the heap while the padded request stays below MAP_MIN. */
void *aligned_alloc(size_t align, size_t n) {
    if (!align || (align & (align - 1))) return NULL;
    if (align <= 16) return malloc(n);
    if (align > PAGE || n >= MAP_MIN - align - MIN_BLK) return map_aligned(align, n);
    heap_lock();
    void *p = aligned_locked(align, n);
    heap_unlock();
    return p;
}
void malloc_usage(size_t *heap, size_t *used) {
    size_t mapped = __atomic_load_n(&mapped_total, __ATOMIC_RELAXED);
    heap_lock();
    if (heap) *heap = heap_total + mapped + map_kept_bytes;
    if (used) *used = heap_used + mapped;
    heap_unlock();
}
void *realloc(void *p, size_t n) {
    if (!p) return malloc(n);
    if (!n) { free(p); return NULL; }
    blk *b = (blk *)((char *)p - HDR);
    size_t have = bsize(b) - HDR;
    if (b->size & MAPPED) {
        have -= b->prev;
        if (n <= have) {
            /* Give back whole pages a shrink no longer needs. */
            size_t keep = (b->prev + n + HDR + PAGE - 1) & ~(size_t)(PAGE - 1);
            if (n >= MAP_MIN && keep < bsize(b)) {
                size_t release = bsize(b) - keep;
                if (n_memory_release(map_region(b) + keep, release) == 0) {
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
