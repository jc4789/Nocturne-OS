/* Document content as pages (fs/content.h, docs/nosfs.md §4).

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
#define E_WB    0x8ull /* snapshotted for write-back, not on its block yet */
#define E_FLAGS 0xFFFull
#define FRAME(e) ((e) & ~E_FLAGS)

#define READ_AHEAD 32  /* pages read with the one asked for, when the medium has them in a row */
#define CHUNK      256 /* pages a write reserves at a time */

struct run {
    uint64_t page, count, block;
};

struct content {
    int refs;
    enum content_state state;
    uint64_t size;
    uint64_t root;  /* depth 0: the entry of page 0; otherwise the top node's frame (0: none) */
    unsigned depth; /* node levels above the page entries */
    uint8_t *tiny;  /* CONTENT_TINY bytes on the heap; the content then owns nothing else */
    const uint8_t *borrow;              /* initrd bytes read in place */
    uint64_t borrow_size, borrow_total; /* bytes still read there / borrowed at first */
    struct run *runs;
    size_t nruns, room;
    struct content *base;  /* the version this one was made from (a reference) */
    struct content *child; /* the version made from this one */
    uint64_t base_limit;   /* pages below it that this one does not own are read from base */
    const struct content_store *store;
    uint64_t resident, dirty, wb; /* pages in frames / of them dirty / being written back */
    unsigned reading;             /* page reads in flight */
    unsigned writers;             /* writes and truncations in progress */
    struct wait_queue io;
    struct content *next, *prev; /* contents with a medium: what reclaim walks */
    uint64_t clock;              /* reclaim's next page */
};

static struct content *stored, *reclaim_at;
static uint64_t cached; /* resident pages of contents with a medium */

static uint64_t *node(uint64_t frame) { return phys_to_virt(frame); }
static uint8_t *bytes(uint64_t entry) { return phys_to_virt(FRAME(entry)); }
static uint64_t pages_of(uint64_t size) { return size / PAGE_SIZE + (size % PAGE_SIZE != 0); }

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

static uint64_t block_of(struct content *c, uint64_t page, uint64_t *left);

/* Free the pages from page `from` on under *at (and the nodes that empties). *undirty counts the
   dirty ones that had no block (their reservation). True when *at is empty now. */
static bool cut(struct content *c, uint64_t *at, unsigned level, uint64_t base, uint64_t from, int64_t *undirty) {
    if (!*at) return true;
    if (!level) {
        if (base < from) return false;
        if (*at & E_DIRTY) {
            c->dirty--;
            if (!block_of(c, base, NULL)) (*undirty)++;
        }
        if (*at & E_WB) c->wb--;
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

/* Blocks [block, block + count) are no longer named, except those in [keep, keep + kept). */
static void drop_blocks(struct content *c, uint64_t block, uint64_t count, uint64_t keep, uint64_t kept) {
    if (!c->store || !count) return;
    uint64_t end = block + count, kend = keep + kept;
    if (!kept || kend <= block || keep >= end) {
        c->store->drop(c, block, count);
        return;
    }
    if (block < keep) c->store->drop(c, block, keep - block);
    if (kend < end) c->store->drop(c, kend, end - kend);
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
    uint64_t end = page + count, kept = drop && block ? count : 0;
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
            if (drop) drop_blocks(c, r->block, head, block, kept);
            r->page = end;
            r->block += head;
            r->count -= head;
            break;
        }
        if (drop) drop_blocks(c, r->block, r->count, block, kept);
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
static void cut_runs(struct content *c, uint64_t page) {
    size_t i = run_find(c, page);
    if (i < c->nruns && c->runs[i].page < page) {
        struct run *r = &c->runs[i++];
        uint64_t keep = page - r->page;
        drop_blocks(c, r->block + keep, r->count - keep, 0, 0);
        r->count = keep;
    }
    for (size_t j = i; j < c->nruns; j++) drop_blocks(c, c->runs[j].block, c->runs[j].count, 0, 0);
    c->nruns = i;
}

/* Runs emptied by a move that stopped half way go. */
static void prune_runs(struct content *c) {
    size_t n = 0;
    for (size_t i = 0; i < c->nruns; i++)
        if (c->runs[i].count) c->runs[n++] = c->runs[i];
    c->nruns = n;
}

/* ---------------------------------------------------------------- ownership */

/* Whether the content itself holds page (rather than its base, or zero). */
static bool owns(struct content *c, uint64_t page) {
    uint64_t *e = slot(c, page, false);
    return (e && *e) || block_of(c, page, NULL) || (c->borrow && page < pages_of(c->borrow_size)) ||
           (c->tiny && page == 0);
}

/* Everything from page keep on is gone: frames, blocks, borrowed bytes, and what base showed. */
static void cut_from(struct content *c, uint64_t keep) {
    int64_t undirty = 0;
    cut(c, &c->root, c->depth, 0, keep, &undirty);
    if (undirty && c->store) c->store->dirty(c, -undirty, true);
    cut_runs(c, keep);
    if (c->borrow && keep <= c->borrow_total / PAGE_SIZE) {
        uint64_t at = keep * PAGE_SIZE;
        if (c->borrow_total > at) initrd_unborrow(c->borrow + at, c->borrow_total - at);
        c->borrow_size = MIN(c->borrow_size, at);
    }
    if (c->base_limit > keep) c->base_limit = keep;
}

static void detach_base(struct content *c) {
    struct content *b = c->base;
    c->base = NULL;
    c->base_limit = 0;
    if (!b) return;
    if (b->child == c) b->child = NULL;
    content_unref(b);
}

/* ---------------------------------------------------------------- contents */

struct content *content_new(const struct content_store *store) {
    struct content *c = kzalloc(sizeof *c);
    if (!c) return NULL;
    c->refs = 1;
    c->state = CONTENT_COMMITTED;
    c->store = store;
    if (store) {
        c->next = stored;
        if (stored) stored->prev = c;
        stored = c;
    }
    return c;
}

struct content *content_version(struct content *base, const struct content_store *store) {
    struct content *c = content_new(store);
    if (!c) return NULL;
    c->state = CONTENT_WORKING;
    if (base && base->size) {
        c->base = content_ref(base);
        base->child = c;
        c->base_limit = pages_of(base->size);
        c->size = base->size;
    }
    return c;
}

static void content_free(struct content *c) {
    cut_from(c, 0);
    if (c->borrow) initrd_unborrow(c->borrow, c->borrow_total);
    kfree(c->tiny);
    detach_base(c);
    if (c->store) {
        if (reclaim_at == c) reclaim_at = c->next;
        if (c->prev) c->prev->next = c->next;
        else stored = c->next;
        if (c->next) c->next->prev = c->prev;
    }
    kfree(c->runs);
    kfree(c);
}

static bool merge(struct content *x);

/* x is committed and only its committed child reads it: the two can be one version. */
static bool mergeable(struct content *x) {
    struct content *f = x->child;
    return x->refs == 1 && f && f->base == x && x->state == CONTENT_COMMITTED && f->state == CONTENT_COMMITTED &&
           !x->reading && !f->reading;
}

struct content *content_ref(struct content *c) {
    c->refs++;
    return c;
}

void content_unref(struct content *c) {
    if (!c) return;
    if (--c->refs > 0) {
        if (c->refs == 1 && mergeable(c)) merge(c);
        return;
    }
    content_free(c);
}

uint64_t content_size(struct content *c) { return c->size; }
enum content_state content_state(struct content *c) { return c->state; }
struct content *content_base(struct content *c) { return c->base; }
uint64_t content_base_limit(struct content *c) { return c->base ? c->base_limit : 0; }
uint64_t content_dirty_pages(struct content *c) { return c->dirty; }
uint64_t content_cached_pages(void) { return cached; }
bool content_is_tiny(struct content *c) { return c->tiny != NULL; }

void content_borrow(struct content *c, const uint8_t *data, uint64_t size) {
    c->borrow = data;
    c->borrow_size = c->borrow_total = c->size = size;
}

void content_load(struct content *c, uint64_t size) { c->size = size; }

int content_map(struct content *c, uint64_t page, uint64_t count, uint64_t block) {
    return set_runs(c, page, count, block, false);
}

int content_load_tiny(struct content *c, const void *data, uint64_t size) {
    if (size > CONTENT_TINY) return -N_ERR_INVALID;
    uint8_t *t = kzalloc(CONTENT_TINY);
    if (!t) return -N_ERR_NO_MEMORY;
    memcpy(t, data, size);
    kfree(c->tiny);
    c->tiny = t;
    c->size = size;
    return 0;
}

/* ---------------------------------------------------------------- reading */

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

/* Bytes [in, in + take) of page as c shows it, into dst; want: pages the caller reads from here. */
static int page_get(struct content *c, uint64_t page, uint64_t in, uint64_t take, uint8_t *dst, uint64_t want) {
    struct content *x = content_ref(c);
    int r = 0;
    for (;;) {
        if (x->tiny) {
            uint64_t at = page * PAGE_SIZE + in, have = !page && in < CONTENT_TINY ? MIN(take, CONTENT_TINY - in) : 0;
            if (have) memcpy(dst, x->tiny + at, have);
            memset(dst + have, 0, take - have);
            break;
        }
        uint64_t *e = slot(x, page, false);
        if (e && (*e & E_BUSY)) {
            wait_io(x);
            continue;
        }
        if (e && *e) {
            memcpy(dst, bytes(*e) + in, take);
            *e |= E_REF;
            break;
        }
        if (x->store && block_of(x, page, NULL)) {
            if ((r = fetch(x, page, want)) < 0) break;
            continue;
        }
        uint64_t at = page * PAGE_SIZE + in;
        if (x->borrow && page < pages_of(x->borrow_size)) {
            uint64_t have = at < x->borrow_size ? MIN(take, x->borrow_size - at) : 0;
            memcpy(dst, x->borrow + at, have);
            memset(dst + have, 0, take - have);
            break;
        }
        if (x->base && page < x->base_limit) {
            struct content *b = content_ref(x->base);
            content_unref(x);
            x = b;
            continue;
        }
        memset(dst, 0, take);
        break;
    }
    content_unref(x);
    return r;
}

int64_t content_read(struct content *c, void *buf, uint64_t off, uint64_t n) {
    if (off >= c->size) return 0;
    n = MIN(n, c->size - off);
    uint64_t done = 0;
    int err = 0;
    while (done < n) {
        uint64_t at = off + done, in = at % PAGE_SIZE;
        uint64_t take = MIN(n - done, PAGE_SIZE - in);
        uint64_t want = (n - done + in + PAGE_SIZE - 1) / PAGE_SIZE + READ_AHEAD / 2;
        if ((err = page_get(c, at / PAGE_SIZE, in, take, (uint8_t *)buf + done, want)) < 0) break;
        done += take;
    }
    return done ? (int64_t)done : err;
}

/* ---------------------------------------------------------------- writing */

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
        if (!whole && c->store && block_of(c, page, NULL)) {
            int r = fetch(c, page, 1);
            if (r < 0) {
                *err = r;
                return 0;
            }
            continue;
        }
        uint64_t f = frame_alloc();
        if (!f) {
            *err = -N_ERR_NO_MEMORY;
            return 0;
        }
        uint8_t *p = phys_to_virt(f);
        int r = 0;
        if (!whole && c->base && page < c->base_limit) { /* copy on write from the version under it */
            c->reading++; /* a truncation waits for it */
            r = page_get(c->base, page, 0, PAGE_SIZE, p, 1);
            c->reading--;
            wake_io(c);
        } else if (!whole) {
            memset(p, 0, PAGE_SIZE);
        }
        e = r < 0 ? NULL : slot(c, page, true);
        if (!e || *e) {
            pmm_free(f);
            if (e) continue; /* someone else made it meanwhile */
            *err = r < 0 ? r : -N_ERR_NO_MEMORY;
            return 0;
        }
        *e = f;
        page_came(c);
        return f;
    }
}

/* The page's entry is written: dirty for the medium. *used counts the pages this needs room for. */
static void mark_dirty(struct content *c, uint64_t *e, uint64_t page, int64_t *used) {
    *e |= E_REF;
    if (!c->store || (*e & E_DIRTY)) return;
    *e |= E_DIRTY;
    c->dirty++;
    if (!block_of(c, page, NULL)) (*used)++;
}

/* A tiny content becomes a page of its own (dirty for the medium when it is being written). */
static int untiny(struct content *c) {
    if (!c->tiny) return 0;
    bool dirty = c->store && c->state != CONTENT_COMMITTED;
    int err;
    if (dirty && (err = c->store->dirty(c, 1, false)) < 0) return err;
    if (!c->tiny) { /* another writer did it while room was found */
        if (dirty) c->store->dirty(c, -1, true);
        return 0;
    }
    uint64_t f = frame_alloc(), *e = f ? slot(c, 0, true) : NULL;
    if (!e) {
        if (f) pmm_free(f);
        if (dirty) c->store->dirty(c, -1, true);
        return -N_ERR_NO_MEMORY;
    }
    uint8_t *p = phys_to_virt(f);
    memcpy(p, c->tiny, CONTENT_TINY);
    memset(p + CONTENT_TINY, 0, PAGE_SIZE - CONTENT_TINY);
    *e = f | E_REF | (dirty ? E_DIRTY : 0);
    if (dirty) c->dirty++;
    page_came(c);
    kfree(c->tiny);
    c->tiny = NULL;
    return 0;
}

/* Whether bytes up to end fit the heap: the content owns nothing else. */
static bool tiny_fits(struct content *c, uint64_t end) {
    if (end > CONTENT_TINY || c->size > CONTENT_TINY) return false;
    if (c->tiny) return true;
    if (c->root || c->nruns || c->borrow || (c->base && c->base_limit)) return false;
    return (c->tiny = kzalloc(CONTENT_TINY)) != NULL;
}

static void writer_done(struct content *c) {
    c->writers--;
    if (c->state != CONTENT_WORKING) wake_io(c); /* a freeze waits for it */
    content_unref(c);
}

int64_t content_write(struct content *c, const void *buf, uint64_t off, uint64_t n) {
    if (!n) return 0;
    if (off + n < off) return -N_ERR_TOO_LARGE;
    if (c->state != CONTENT_WORKING) return -N_ERR_BUSY;
    content_ref(c);
    c->writers++;
    uint64_t done = 0;
    int err = 0;
    if (tiny_fits(c, off + n)) {
        memcpy(c->tiny + off, buf, n);
        if (off + n > c->size) c->size = off + n;
        done = n;
    } else if ((err = untiny(c)) < 0) {
        n = 0;
    }
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
            mark_dirty(c, slot(c, page, false), page, &used);
            done += take;
            if (off + done > c->size) c->size = off + done;
        }
        if (reserved > used) c->store->dirty(c, used - reserved, true);
    }
    writer_done(c);
    return done ? (int64_t)done : err;
}

int content_truncate(struct content *c, uint64_t size) {
    if (c->state != CONTENT_WORKING) return -N_ERR_BUSY;
    content_ref(c);
    c->writers++;
    int err = 0;
    if (c->tiny && size <= CONTENT_TINY) {
        if (size < c->size) memset(c->tiny + size, 0, c->size - size);
        c->size = size;
        goto out;
    }
    if (size >= c->size) {
        if (size > CONTENT_TINY) err = untiny(c);
        if (!err) c->size = size;
        goto out;
    }
    if ((err = untiny(c)) < 0) goto out;
    uint64_t page = size / PAGE_SIZE, in = size % PAGE_SIZE, keep = pages_of(size);
    /* The new last page's bytes past the end become zero: it is the content's own from now on. */
    if (in && (owns(c, page) || (c->base && page < c->base_limit))) {
        if (c->store && (err = c->store->dirty(c, 1, false)) < 0) goto out;
        uint64_t f = frame_for_write(c, page, false, &err);
        int64_t used = 0;
        if (f) {
            memset((uint8_t *)phys_to_virt(f) + in, 0, PAGE_SIZE - in);
            mark_dirty(c, slot(c, page, false), page, &used);
        }
        if (c->store && !used) c->store->dirty(c, -1, true);
        if (!f) goto out;
    }
    while (c->reading) wait_io(c);
    cut_from(c, keep);
    c->size = size;
out:
    writer_done(c);
    return err;
}

/* ---------------------------------------------------------------- saving */

void content_freeze(struct content *c) {
    if (c->state != CONTENT_WORKING) return;
    c->state = CONTENT_FROZEN;
    content_ref(c);
    while (c->writers || c->wb) wait_io(c);
    content_unref(c);
}

void content_thaw(struct content *c) {
    if (c->state == CONTENT_FROZEN) c->state = CONTENT_WORKING;
}

int content_own_all(struct content *c) {
    if (!c->base) return 0;
    content_ref(c);
    int err = 0;
    for (uint64_t page = 0, end = MIN(c->base_limit, pages_of(c->size)); page < end && !err; page++) {
        if (owns(c, page)) continue;
        if (c->store && (err = c->store->dirty(c, 1, false)) < 0) break;
        int64_t used = 0;
        uint64_t f = frame_for_write(c, page, false, &err);
        if (f) mark_dirty(c, slot(c, page, false), page, &used);
        if (c->store && !used) c->store->dirty(c, -1, true);
    }
    if (!err) detach_base(c);
    content_unref(c);
    return err;
}

int content_compact(struct content *c) {
    if (c->size > CONTENT_TINY || (c->tiny && !c->base)) return 0;
    uint8_t *t = kzalloc(CONTENT_TINY);
    if (!t) return -N_ERR_NO_MEMORY;
    content_ref(c);
    uint64_t size = c->size;
    int64_t r = content_read(c, t, 0, size);
    while (c->reading) wait_io(c);
    if (r >= 0 && (uint64_t)r == size && c->size == size) {
        cut_from(c, 0);
        if (c->borrow) initrd_unborrow(c->borrow, c->borrow_total);
        c->borrow = NULL;
        c->borrow_size = c->borrow_total = 0;
        detach_base(c);
        kfree(c->tiny);
        c->tiny = t;
        t = NULL;
        r = 0;
    }
    kfree(t);
    content_unref(c);
    return r < 0 ? (int)r : 0;
}

void content_saving(struct content *c) {
    c->state = CONTENT_SAVING;
    content_ref(c);
    while (c->wb) wait_io(c); /* pages written ahead are on their blocks before a commit names them */
    content_unref(c);
}

/* Merge what the chain under c lets merge. */
static void settle(struct content *c) {
    for (struct content *x = c->base; x;) {
        if (!mergeable(x)) {
            x = x->base;
            continue;
        }
        if (!merge(x)) return;
        x = c->base;
    }
}

void content_saved(struct content *c) {
    c->state = CONTENT_COMMITTED;
    if (c->base && !c->base_limit) detach_base(c);
    if (!c->store && c->size <= CONTENT_TINY) content_compact(c); /* in memory only: never sleeps */
    settle(c);
}

/* ---------------------------------------------------------------- merging versions

   x (committed, read by nothing but its committed child f) and f become one version with f's
   identity. The side with fewer pages moves into the other, so the cost follows the change, not
   the document. A move that runs out of memory stops where it is: each step leaves both versions
   showing what they showed. */

/* x's pages that f shows and does not own move into f; the rest go. */
static bool give(struct content *x, struct content *f, uint64_t limit) {
    uint64_t *e;
    for (uint64_t p = 0; (e = next_entry(x, &p)); p++) {
        if (p >= limit || owns(f, p)) {
            page_gone(x, *e);
            *e = 0;
            continue;
        }
        uint64_t *s = slot(f, p, true);
        if (!s) return false;
        *s = *e;
        *e = 0;
        x->resident--;
        f->resident++;
    }
    for (size_t i = 0; i < x->nruns; i++) {
        struct run *r = &x->runs[i];
        while (r->count) {
            uint64_t left, n = r->count;
            bool keep = r->page < limit && !block_of(f, r->page, &left);
            if (r->page < limit) n = MIN(n, MIN(limit - r->page, left));
            if (keep && set_runs(f, r->page, n, r->block, false) < 0) {
                prune_runs(x);
                return false;
            }
            if (!keep) drop_blocks(x, r->block, n, 0, 0);
            r->page += n;
            r->block += n;
            r->count -= n;
        }
    }
    x->nruns = 0;
    if (x->borrow && !f->borrow) { /* f reads the archive where it does not own the page */
        f->borrow = x->borrow;
        f->borrow_total = x->borrow_total;
        f->borrow_size = limit <= x->borrow_size / PAGE_SIZE ? limit * PAGE_SIZE : x->borrow_size;
        x->borrow = NULL;
    }
    return true;
}

/* f's pages move into x, over x's own (x is cut at limit first); f reads all through x meanwhile. */
static bool take(struct content *f, struct content *x, uint64_t limit) {
    cut_from(x, limit);
    f->base_limit = UINT64_MAX;
    uint64_t *e;
    for (uint64_t p = 0; (e = next_entry(f, &p)); p++) {
        uint64_t *s = slot(x, p, true);
        if (!s) return false;
        if (*s) page_gone(x, *s);
        if (x->borrow && p < pages_of(x->borrow_size)) {
            uint64_t at = p * PAGE_SIZE;
            initrd_unborrow(x->borrow + at, MIN(PAGE_SIZE, x->borrow_total - at));
        }
        *s = *e;
        *e = 0;
        f->resident--;
        x->resident++;
    }
    for (size_t i = 0; i < f->nruns; i++) {
        struct run *r = &f->runs[i];
        if (set_runs(x, r->page, r->count, r->block, true) < 0) {
            prune_runs(f);
            return false;
        }
        r->count = 0;
    }
    f->nruns = 0;
    x->size = f->size;
    return true;
}

static void swap_pages(struct content *a, struct content *b) {
#define SWAP(field)                \
    do {                           \
        __typeof__(a->field) t = a->field; \
        a->field = b->field;       \
        b->field = t;              \
    } while (0)
    SWAP(size);
    SWAP(root);
    SWAP(depth);
    SWAP(tiny);
    SWAP(borrow);
    SWAP(borrow_size);
    SWAP(borrow_total);
    SWAP(runs);
    SWAP(nruns);
    SWAP(room);
    SWAP(resident);
    SWAP(dirty);
    SWAP(wb);
    SWAP(clock);
#undef SWAP
}

static bool merge(struct content *x) {
    struct content *f = x->child;
    uint64_t limit = f->base_limit;
    if (limit) {
        if (untiny(x) < 0) return false;
        bool up = f->resident + f->nruns < x->resident + x->nruns;
        if (up ? !take(f, x, limit) : !give(x, f, limit)) return false;
        if (up) swap_pages(x, f);
    }
    /* f takes x's place over x's base */
    struct content *b = limit ? x->base : NULL;
    f->base = b;
    f->base_limit = b ? MIN(limit, x->base_limit) : 0;
    if (b) {
        b->child = f;
        x->base = NULL; /* its reference is f's now */
    }
    x->child = NULL;
    content_unref(x); /* f's reference: frees what is left of x */
    if (!f->store && f->size <= CONTENT_TINY) content_compact(f);
    return true;
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

uint64_t content_block(struct content *c, uint64_t page, uint64_t *left) { return block_of(c, page, left); }

int content_snapshot(struct content *c, uint64_t page, uint64_t count, void *buf, uint64_t block) {
    uint64_t left;
    if (block_of(c, page, &left) != block || left < count) {
        int r = set_runs(c, page, count, block, true);
        if (r < 0) return r;
    }
    for (uint64_t i = 0; i < count; i++) {
        uint8_t *dst = (uint8_t *)buf + i * PAGE_SIZE;
        uint64_t *e = slot(c, page + i, false);
        if (!e || !*e || (*e & E_BUSY)) {
            memset(dst, 0, PAGE_SIZE);
            continue;
        }
        memcpy(dst, bytes(*e), PAGE_SIZE);
        if (*e & E_DIRTY) c->dirty--;
        if (!(*e & E_WB)) c->wb++;
        *e = (*e & ~E_DIRTY) | E_WB;
    }
    return 0;
}

void content_written(struct content *c, uint64_t page, uint64_t count) {
    for (uint64_t i = 0; i < count; i++) {
        uint64_t *e = slot(c, page + i, false);
        if (!e || !(*e & E_WB)) continue; /* truncated away meanwhile */
        *e &= ~E_WB;
        c->wb--;
    }
    wake_io(c);
}

void content_write_failed(struct content *c, uint64_t page, uint64_t count) {
    for (uint64_t i = 0; i < count; i++) {
        uint64_t *e = slot(c, page + i, false);
        if (!e || !(*e & E_WB)) continue;
        *e &= ~E_WB;
        c->wb--;
        if (!(*e & E_DIRTY)) {
            *e |= E_DIRTY;
            c->dirty++;
        }
    }
    wake_io(c);
}

int content_extents(struct content *c, int (*run)(void *ctx, uint64_t page, uint64_t count, uint64_t block),
                    void *ctx) {
    for (size_t i = 0; i < c->nruns; i++) {
        int r = run(ctx, c->runs[i].page, c->runs[i].count, c->runs[i].block);
        if (r < 0) return r;
    }
    return 0;
}

struct content *content_spill_pick(void) {
    struct content *best = NULL;
    for (struct content *c = stored; c; c = c->next)
        if ((c->state == CONTENT_WORKING || c->state == CONTENT_FROZEN) && c->dirty && (!best || c->dirty > best->dirty))
            best = c;
    return best;
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
