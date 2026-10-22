/* Document content as pages (fs/content.h, docs/nosfs.md §3).

   The page table is a 512-way tree whose nodes are frames: a content of one page keeps its page
   entry in the root itself, a larger one grows levels as it needs them. A page entry is the frame's
   physical address with flags in the low bits. The extent map is a sorted array of runs. */
#include "fs/content.h"
#include "fs/vfs.h"
#include "mm/heap.h"
#include "mm/pmm.h"
#include "arch/cpu.h"
#include "sys/sched.h"

#define FAN_SHIFT 9
#define FAN       (1ull << FAN_SHIFT)
#define MAX_DEPTH 6 /* 2^54 pages: more than a 64-bit offset can name */

#define E_DIRTY 0x1ull /* the medium does not have this page yet */
#define E_REF   0x2ull /* used since reclaim last looked at it */
#define E_BUSY  0x4ull /* being read in: its bytes are not there yet */
#define E_WB    0x8ull /* snapshotted for write-back, not mapped to its new block yet */
#define E_FLAGS 0xFFFull
#define FRAME(e) ((e) & ~E_FLAGS)

#define READ_AHEAD 32  /* pages read with the one asked for, when the medium has them in a row */
#define CHUNK      256 /* pages a write reserves at a time */

struct run {
    uint64_t page, count, block;
};

struct content {
    int refs;
    uint64_t size;
    uint64_t root;  /* depth 0: the entry of page 0; otherwise the top node's frame (0: none) */
    unsigned depth; /* node levels above the page entries */
    const uint8_t *borrow;              /* initrd bytes read in place */
    uint64_t borrow_size, borrow_total; /* bytes still read there / borrowed at first */
    struct run *runs;
    size_t nruns, room;
    const struct content_store *store;
    void *owner;
    uint64_t resident, dirty; /* pages */
    unsigned reading;         /* page reads in flight */
    struct wait_queue io;
    struct content *next, *prev; /* contents with a medium: what reclaim walks */
    uint64_t clock;              /* reclaim's next page */
};

static struct content *stored, *reclaim_at;
static uint64_t cached; /* resident pages of contents with a medium */

static uint64_t *node(uint64_t frame) { return phys_to_virt(frame); }
static uint8_t *bytes(uint64_t entry) { return phys_to_virt(FRAME(entry)); }

static void wait_io(struct content *c) {
    uint64_t flags = irq_save();
    wq_wait(&c->io);
    irq_restore(flags);
}

static void wake_io(struct content *c) {
    uint64_t flags = irq_save();
    wq_wake_all(&c->io);
    irq_restore(flags);
}

/* ---------------------------------------------------------------- frames */

static uint64_t low_mark(void) { return pmm_total_pages() / 32 + 256; }

static uint64_t frame_alloc(void) {
    if (cached && pmm_free_pages() < low_mark()) content_reclaim(64);
    uint64_t f = pmm_alloc();
    if (!f && content_reclaim(64)) f = pmm_alloc();
    return f;
}

static void page_gone(struct content *c, uint64_t entry) {
    pmm_free(FRAME(entry));
    c->resident--;
    if (c->store) cached--;
}

static void page_came(struct content *c) {
    c->resident++;
    if (c->store) cached++;
}

/* ---------------------------------------------------------------- page table */

static uint64_t span(unsigned depth) { return 1ull << (FAN_SHIFT * depth); } /* pages under a root */

/* The entry of page, or NULL when the tree has no place for it (create: makes one; NULL then
   means no memory). */
static uint64_t *slot(struct content *c, uint64_t page, bool create) {
    while (c->depth < MAX_DEPTH && page >= span(c->depth)) {
        if (!create) return NULL;
        if (c->root) {
            uint64_t f = pmm_alloc_zeroed();
            if (!f) return NULL;
            node(f)[0] = c->root;
            c->root = f;
        }
        c->depth++;
    }
    if (page >= span(c->depth)) return NULL;
    if (!c->depth) return &c->root;
    uint64_t *at = &c->root;
    for (unsigned level = c->depth; level; level--) {
        if (!*at) {
            if (!create || !(*at = pmm_alloc_zeroed())) return NULL;
        }
        at = &node(*at)[(page >> (FAN_SHIFT * (level - 1))) & (FAN - 1)];
    }
    return at;
}

static uint64_t *walk(uint64_t frame, unsigned level, uint64_t base, uint64_t *page) {
    uint64_t each = span(level - 1);
    for (uint64_t i = *page > base ? (*page - base) / each : 0; i < FAN; i++) {
        uint64_t *e = &node(frame)[i];
        if (!*e) continue;
        if (level == 1) {
            *page = base + i;
            return e;
        }
        uint64_t *found = walk(*e, level - 1, base + i * each, page);
        if (found) return found;
    }
    return NULL;
}

/* The first page entry at or after *page, its index in *page; NULL when there is none. */
static uint64_t *next_entry(struct content *c, uint64_t *page) {
    if (!c->root) return NULL;
    if (!c->depth) return *page == 0 ? &c->root : NULL;
    if (*page >= span(c->depth)) return NULL;
    return walk(c->root, c->depth, 0, page);
}

/* Free the pages from page `from` on under *at (and the nodes that empties). *undirty counts the
   dirty ones. True when *at is empty now. */
static bool cut(struct content *c, uint64_t *at, unsigned level, uint64_t base, uint64_t from, int64_t *undirty) {
    if (!*at) return true;
    if (!level) {
        if (base < from) return false;
        if (*at & E_DIRTY) {
            c->dirty--;
            (*undirty)++;
        }
        page_gone(c, *at);
        *at = 0;
        return true;
    }
    uint64_t each = span(level - 1);
    bool empty = true;
    for (uint64_t i = 0; i < FAN; i++) {
        uint64_t *e = &node(*at)[i];
        if (!*e) continue;
        if (base + (i + 1) * each <= from || !cut(c, e, level - 1, base + i * each, from, undirty)) empty = false;
    }
    if (empty) {
        pmm_free(*at);
        *at = 0;
    }
    return empty;
}

/* ---------------------------------------------------------------- extent map */

/* The first run that ends after page (nruns: none). */
static size_t run_find(struct content *c, uint64_t page) {
    size_t lo = 0, hi = c->nruns;
    while (lo < hi) {
        size_t mid = (lo + hi) / 2;
        if (c->runs[mid].page + c->runs[mid].count <= page) lo = mid + 1;
        else hi = mid;
    }
    return lo;
}

/* The block holding page (0: a hole); *left: pages from there on that continue the same way. */
static uint64_t block_of(struct content *c, uint64_t page, uint64_t *left) {
    size_t i = run_find(c, page);
    if (i < c->nruns && c->runs[i].page <= page) {
        uint64_t in = page - c->runs[i].page;
        if (left) *left = c->runs[i].count - in;
        return c->runs[i].block + in;
    }
    if (left) *left = i < c->nruns ? c->runs[i].page - page : UINT64_MAX;
    return 0;
}

static void drop_blocks(struct content *c, uint64_t block, uint64_t count, bool drop) {
    if (drop && c->store && count) c->store->drop(c, block, count);
}

/* Pages [page, page + count) name the blocks from block on (0: none); the blocks they named
   before are dropped when drop. */
static int set_runs(struct content *c, uint64_t page, uint64_t count, uint64_t block, bool drop) {
    if (!count) return 0;
    if (c->nruns + 2 > c->room) {
        size_t room = MAX(MAX(c->room * 2, c->nruns + 2), (size_t)4);
        struct run *r = krealloc(c->runs, room * sizeof *r);
        if (!r) return -N_ERR_NO_MEMORY;
        c->runs = r;
        c->room = room;
    }
    uint64_t end = page + count;
    size_t i = run_find(c, page);
    if (i < c->nruns && c->runs[i].page < page) { /* a run from before reaches in: split it at page */
        struct run r = c->runs[i];
        memmove(&c->runs[i + 1], &c->runs[i], (c->nruns - i) * sizeof *c->runs);
        c->nruns++;
        c->runs[i].count = page - r.page;
        c->runs[++i] = (struct run){page, r.count - (page - r.page), r.block + (page - r.page)};
    }
    size_t j = i;
    for (; j < c->nruns && c->runs[j].page < end; j++) {
        struct run *r = &c->runs[j];
        if (r->page + r->count > end) { /* reaches past the range: keep its tail */
            uint64_t head = end - r->page;
            drop_blocks(c, r->block, head, drop);
            r->page = end;
            r->block += head;
            r->count -= head;
            break;
        }
        drop_blocks(c, r->block, r->count, drop);
    }
    memmove(&c->runs[i], &c->runs[j], (c->nruns - j) * sizeof *c->runs);
    c->nruns -= j - i;
    if (!block) return 0;
    struct run *prev = i ? &c->runs[i - 1] : NULL, *next = i < c->nruns ? &c->runs[i] : NULL;
    bool join_prev = prev && prev->page + prev->count == page && prev->block + prev->count == block;
    bool join_next = next && next->page == end && block + count == next->block;
    if (join_prev && join_next) {
        prev->count += count + next->count;
        memmove(next, next + 1, (c->nruns - i - 1) * sizeof *c->runs);
        c->nruns--;
    } else if (join_prev) {
        prev->count += count;
    } else if (join_next) {
        next->page = page;
        next->block = block;
        next->count += count;
    } else {
        memmove(&c->runs[i + 1], &c->runs[i], (c->nruns - i) * sizeof *c->runs);
        c->runs[i] = (struct run){page, count, block};
        c->nruns++;
    }
    return 0;
}

/* Everything from page on is a hole (never needs memory). */
static void cut_runs(struct content *c, uint64_t page, bool drop) {
    size_t i = run_find(c, page);
    if (i < c->nruns && c->runs[i].page < page) {
        struct run *r = &c->runs[i++];
        uint64_t keep = page - r->page;
        drop_blocks(c, r->block + keep, r->count - keep, drop);
        r->count = keep;
    }
    for (size_t j = i; j < c->nruns; j++) drop_blocks(c, c->runs[j].block, c->runs[j].count, drop);
    c->nruns = i;
}

/* ---------------------------------------------------------------- contents */

struct content *content_new(const struct content_store *store, void *owner) {
    struct content *c = kzalloc(sizeof *c);
    if (!c) return NULL;
    c->refs = 1;
    c->store = store;
    c->owner = owner;
    if (store) {
        c->next = stored;
        if (stored) stored->prev = c;
        stored = c;
    }
    return c;
}

struct content *content_ref(struct content *c) {
    c->refs++;
    return c;
}

void content_unref(struct content *c) {
    if (!c || --c->refs > 0) return;
    int64_t undirty = 0;
    cut(c, &c->root, c->depth, 0, 0, &undirty);
    if (undirty && c->store) c->store->dirty(c, -undirty, true);
    cut_runs(c, 0, true);
    if (c->borrow) initrd_unborrow(c->borrow, c->borrow_total);
    if (c->store) {
        if (reclaim_at == c) reclaim_at = c->next;
        if (c->prev) c->prev->next = c->next;
        else stored = c->next;
        if (c->next) c->next->prev = c->prev;
    }
    kfree(c->runs);
    kfree(c);
}

void *content_owner(struct content *c) { return c->owner; }
uint64_t content_size(struct content *c) { return c->size; }
uint64_t content_dirty_pages(struct content *c) { return c->dirty; }
uint64_t content_cached_pages(void) { return cached; }

void content_borrow(struct content *c, const uint8_t *data, uint64_t size) {
    c->borrow = data;
    c->borrow_size = c->borrow_total = c->size = size;
}

void content_load(struct content *c, uint64_t size) { c->size = size; }

int content_map(struct content *c, uint64_t page, uint64_t count, uint64_t block) {
    return set_runs(c, page, count, block, false);
}

/* Read the run of the medium that holds page (and the pages after it, up to want) into new
   frames. Others asking for those pages meanwhile wait for this read. */
static int fetch(struct content *c, uint64_t page, uint64_t want) {
    uint64_t left, block = block_of(c, page, &left);
    uint64_t frames[READ_AHEAD], n = MIN(MIN(left, want), (uint64_t)READ_AHEAD), k = 0;
    if (!block) return -N_ERR_IO;
    for (; k < n; k++) {
        uint64_t *e = slot(c, page + k, true);
        if (!e || *e) break;
        uint64_t f = frame_alloc();
        if (!f) break;
        e = slot(c, page + k, true); /* frame_alloc may have reclaimed the node it was in */
        if (!e || *e) {
            pmm_free(f);
            break;
        }
        *e = f | E_BUSY;
        frames[k] = f;
        page_came(c);
    }
    if (!k) return -N_ERR_NO_MEMORY;
    c->reading++;
    int r = c->store->read(c, block, k, frames);
    c->reading--;
    for (uint64_t i = 0; i < k; i++) {
        uint64_t *e = slot(c, page + i, false);
        if (r < 0) {
            page_gone(c, *e);
            *e = 0;
        } else {
            *e = (*e & ~E_BUSY) | E_REF;
        }
    }
    wake_io(c);
    return r;
}

/* The frame of page, to be written. whole: the caller writes all of it, so its old bytes are not
   read. 0 with *err when it cannot be had. */
static uint64_t frame_for_write(struct content *c, uint64_t page, bool whole, int *err) {
    for (;;) {
        uint64_t *e = slot(c, page, false);
        if (e && (*e & E_BUSY)) {
            wait_io(c);
            continue;
        }
        if (e && *e) return FRAME(*e);
        uint64_t at = page * PAGE_SIZE;
        bool borrowed = c->borrow && at < c->borrow_size;
        if (!whole && !borrowed && c->store && block_of(c, page, NULL)) {
            int r = fetch(c, page, 1);
            if (r < 0) {
                *err = r;
                return 0;
            }
            continue;
        }
        uint64_t f = frame_alloc();
        if (!f || !(e = slot(c, page, true)) || *e) {
            if (f) pmm_free(f);
            if (e && *e) continue; /* someone else made it while memory was found */
            *err = -N_ERR_NO_SPACE;
            return 0;
        }
        uint8_t *p = phys_to_virt(f);
        if (!whole) {
            uint64_t from = borrowed ? MIN(PAGE_SIZE, c->borrow_size - at) : 0;
            if (from) memcpy(p, c->borrow + at, from);
            memset(p + from, 0, PAGE_SIZE - from);
        }
        if (c->borrow && at < c->borrow_total) initrd_unborrow(c->borrow + at, MIN(PAGE_SIZE, c->borrow_total - at));
        *e = f;
        page_came(c);
        return f;
    }
}

int64_t content_read(struct content *c, void *buf, uint64_t off, uint64_t n) {
    if (off >= c->size) return 0;
    n = MIN(n, c->size - off);
    content_ref(c);
    uint64_t done = 0;
    int err = 0;
    while (done < n) {
        uint64_t at = off + done, page = at / PAGE_SIZE, in = at % PAGE_SIZE;
        uint64_t take = MIN(n - done, PAGE_SIZE - in);
        uint8_t *dst = (uint8_t *)buf + done;
        uint64_t *e = slot(c, page, false);
        if (e && (*e & E_BUSY)) {
            wait_io(c);
            continue;
        }
        if (e && *e) {
            memcpy(dst, bytes(*e) + in, take);
            *e |= E_REF;
        } else if (c->borrow && at < c->borrow_size) {
            uint64_t have = MIN(take, c->borrow_size - at);
            memcpy(dst, c->borrow + at, have);
            memset(dst + have, 0, take - have);
        } else if (c->store && block_of(c, page, NULL)) {
            uint64_t want = (n - done + in + PAGE_SIZE - 1) / PAGE_SIZE + READ_AHEAD / 2;
            if ((err = fetch(c, page, want)) < 0) break;
            continue;
        } else {
            memset(dst, 0, take);
        }
        done += take;
    }
    content_unref(c);
    return done ? (int64_t)done : err;
}

int64_t content_write(struct content *c, const void *buf, uint64_t off, uint64_t n) {
    if (!n) return 0;
    if (off + n < off) return -N_ERR_TOO_LARGE;
    content_ref(c);
    uint64_t done = 0;
    int err = 0;
    while (done < n && !err) {
        /* Room for the chunk's write-back first: waiting for it happens with no page in hand. */
        uint64_t first = (off + done) / PAGE_SIZE;
        uint64_t last = MIN((off + n - 1) / PAGE_SIZE, first + CHUNK - 1);
        int64_t reserved = c->store ? (int64_t)(last - first + 1) : 0, used = 0;
        if (reserved && (err = c->store->dirty(c, reserved, false)) < 0) break;
        for (uint64_t page = first; page <= last; page++) {
            uint64_t at = off + done, in = at % PAGE_SIZE;
            uint64_t take = MIN(n - done, PAGE_SIZE - in);
            bool whole = take == PAGE_SIZE || (in == 0 && at + take >= c->size);
            uint64_t f = frame_for_write(c, page, whole, &err);
            if (!f) break;
            uint8_t *p = phys_to_virt(f);
            memcpy(p + in, (const uint8_t *)buf + done, take);
            if (whole && take < PAGE_SIZE) memset(p + take, 0, PAGE_SIZE - take);
            uint64_t *e = slot(c, page, false);
            *e |= E_REF;
            if (c->store && !(*e & E_DIRTY)) {
                *e |= E_DIRTY;
                c->dirty++;
                used++;
            }
            done += take;
            if (off + done > c->size) c->size = off + done;
        }
        if (reserved > used) c->store->dirty(c, used - reserved, true);
    }
    content_unref(c);
    return done ? (int64_t)done : err;
}

int content_truncate(struct content *c, uint64_t size) {
    if (size >= c->size) {
        c->size = size;
        return 0;
    }
    content_ref(c);
    int err = 0;
    uint64_t page = size / PAGE_SIZE, in = size % PAGE_SIZE, keep = ALIGN_UP(size, PAGE_SIZE) / PAGE_SIZE;
    /* The new last page's bytes past the end become zero (the borrowed ones are cut off below). */
    uint64_t *e = slot(c, page, false);
    if (in && ((e && *e) || (c->store && block_of(c, page, NULL)))) {
        if (c->store && (err = c->store->dirty(c, 1, false)) < 0) goto out;
        uint64_t f = frame_for_write(c, page, false, &err);
        if (!f) {
            if (c->store) c->store->dirty(c, -1, true);
            goto out;
        }
        memset((uint8_t *)phys_to_virt(f) + in, 0, PAGE_SIZE - in);
        e = slot(c, page, false);
        if (c->store && !(*e & E_DIRTY)) {
            *e |= E_DIRTY;
            c->dirty++;
        } else if (c->store) {
            c->store->dirty(c, -1, true);
        }
    }
    while (c->reading) wait_io(c);
    int64_t undirty = 0;
    cut(c, &c->root, c->depth, 0, keep, &undirty);
    if (undirty && c->store) c->store->dirty(c, -undirty, true);
    cut_runs(c, keep, true);
    if (c->borrow && c->borrow_total > keep * PAGE_SIZE)
        initrd_unborrow(c->borrow + keep * PAGE_SIZE, c->borrow_total - keep * PAGE_SIZE);
    c->borrow_size = MIN(c->borrow_size, size);
    c->size = size;
out:
    content_unref(c);
    return err;
}

/* ---------------------------------------------------------------- write-back */

uint64_t content_dirty_run(struct content *c, uint64_t *page, uint64_t max) {
    if (!c->dirty) return 0;
    uint64_t p = *page, *e;
    while ((e = next_entry(c, &p)) && !(*e & E_DIRTY)) p++;
    if (!e) return 0;
    uint64_t n = 1;
    while (n < max && p + n < span(c->depth) && (e = slot(c, p + n, false)) && (*e & E_DIRTY)) n++;
    *page = p;
    return n;
}

void content_snapshot(struct content *c, uint64_t page, uint64_t count, void *buf) {
    for (uint64_t i = 0; i < count; i++) {
        uint8_t *dst = (uint8_t *)buf + i * PAGE_SIZE;
        uint64_t *e = slot(c, page + i, false);
        if (!e || !*e || (*e & E_BUSY)) {
            memset(dst, 0, PAGE_SIZE);
            continue;
        }
        memcpy(dst, bytes(*e), PAGE_SIZE);
        if (*e & E_DIRTY) c->dirty--;
        *e = (*e & ~E_DIRTY) | E_WB;
    }
}

static bool written_page(struct content *c, uint64_t page) {
    uint64_t *e = slot(c, page, false);
    return e && (*e & E_WB);
}

int content_written(struct content *c, uint64_t page, uint64_t count, uint64_t block) {
    int err = 0;
    for (uint64_t i = 0; i < count;) {
        if (!written_page(c, page + i)) { /* truncated away meanwhile */
            drop_blocks(c, block + i, 1, true);
            i++;
            continue;
        }
        uint64_t j = i + 1;
        while (j < count && written_page(c, page + j)) j++;
        int r = set_runs(c, page + i, j - i, block + i, true);
        int64_t redirty = 0;
        for (uint64_t k = i; k < j; k++) {
            uint64_t *e = slot(c, page + k, false);
            *e &= ~E_WB;
            if (r < 0 && !(*e & E_DIRTY)) { /* not recorded: written again later */
                *e |= E_DIRTY;
                c->dirty++;
                redirty++;
            }
        }
        if (r < 0) {
            c->store->dirty(c, redirty, true);
            drop_blocks(c, block + i, j - i, true);
            err = r;
        }
        i = j;
    }
    return err;
}

int content_extents(struct content *c, int (*run)(void *ctx, uint64_t page, uint64_t count, uint64_t block),
                    void *ctx) {
    for (size_t i = 0; i < c->nruns; i++) {
        int r = run(ctx, c->runs[i].page, c->runs[i].count, c->runs[i].block);
        if (r < 0) return r;
    }
    return 0;
}

/* ---------------------------------------------------------------- reclaim */

/* Clean pages read from a medium go in clock order: one used since the last look gets another
   round. Dirty pages, pages being read or written back, and pages with no block stay. */
uint64_t content_reclaim(uint64_t want) {
    if (!stored || !cached) return 0;
    struct content *start = reclaim_at ? reclaim_at : stored, *c = start;
    uint64_t freed = 0;
    for (int laps = 0; laps < 2 && freed < want;) {
        uint64_t page = c->clock, *e = NULL;
        for (unsigned looked = 0; freed < want && looked < 4096 && (e = next_entry(c, &page)); looked++, page++) {
            if (*e & (E_DIRTY | E_BUSY | E_WB)) continue;
            if (*e & E_REF) *e &= ~E_REF;
            else if (block_of(c, page, NULL)) {
                page_gone(c, *e);
                *e = 0;
                freed++;
            }
        }
        c->clock = e ? page : 0;
        if (freed >= want) break;
        c = c->next ? c->next : stored;
        if (c == start) laps++;
    }
    reclaim_at = c;
    return freed;
}
