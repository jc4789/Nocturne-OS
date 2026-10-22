/* NOSfs: the persistent volume behind the namespace (fs/nos.h, docs/nosfs.md §5–7, §9, §11).

   The index is one copy-on-write B+tree of 4 KiB nodes. A change makes the nodes from its leaf up
   to the root dirty; a dirty node is changed in place until its commit reaches the medium, and the
   commit writes it once, to a block nothing on the medium names. The anchor, written last, names
   the new root. Free space is a bitmap in map blocks with two places each, and a summary of their
   free counts, also in two places: the anchor names the summary's place and the summary each map's,
   so a commit never writes over what the commit before it needs.

   A block is used when its bit is set (the index names it) or it is held: written by a version
   that is not committed yet (a spill, a commit on its way), let go of by the index while a version
   in RAM still reads it, or let go of by the commit on its way. Holds live in RAM only; after a
   power loss what they kept is free again.

   One worker thread commits and spills; the other hooks run in the caller's task and only add to
   the open commit. The tree and the maps change in the worker only, the tree under the lock that
   folder and document reads take too. */
#include "fs/nos.h"
#include "fs/ramfs.h"
#include "fs/content.h"
#include "dev/blk.h"
#include "dev/entropy.h"
#include "lib/crc32c.h"
#include "mm/heap.h"
#include "mm/pmm.h"
#include "arch/cpu.h"

#define BS          4096
#define SECTORS     (BS / 512)
#define VERSION     1
#define ID_ROOT     1
#define MAX_HEIGHT  16
#define BOUNCE      64                      /* blocks one transfer carries */
#define FAILS       64                      /* failed commits remembered for their tickets */
#define PAGE_END    (1ull << 56)            /* past any page an extent key can name */
#define SEQ_MAX     0xFFFFFFull             /* names in one hash bucket */
#define MIN_BLOCKS  ((64ull << 20) / BS)    /* a smaller volume is not made */
#define GAP_SECTORS ((256ull << 20) / 512)  /* room a boot image may grow into */
#define TYPE_NOS    0x7F
#define SIG_NOCT    0x4E4F4354u
#define SIG_DATA    0x44415441u

enum { K_ENTRY = 1, K_EXTENT = 2, K_HIDE = 3 };
#define KIND(k) ((uint64_t)(k) << 56)
#define EF_BASE_CONTENT 0x1 /* the document shows its base item's content */
#define EF_TINY         0x2 /* the content is in the entry */

/* ---------------------------------------------------------------- on the medium (little-endian) */

struct head { /* block 0, and the volume's last block */
    uint32_t crc;
    char magic[8];
    uint32_t version;
    uint64_t blocks, volume_id, first_lba;
    uint64_t sum_at, sum_blocks, map_at, maps, data_at;
} PACKED;

struct anchor { /* blocks 1 and 2, written in turn */
    uint32_t crc;
    char magic[4];
    uint64_t volume_id, gen, root, next_id, free;
    uint32_t height, sum_slot;
} PACKED;

struct sum_head { /* then a u16 per map: its clear bits | its place << 15 */
    uint32_t crc;
    char magic[4];
    uint64_t volume_id, gen;
    uint32_t index, pad;
} PACKED;

struct map_head { /* then a bit per block */
    uint32_t crc;
    char magic[4];
    uint32_t index, volume;
} PACKED;

struct node_head {
    uint32_t crc;
    char magic[4];
    uint16_t count;
    uint8_t level, pad;
    uint16_t top, pad2; /* leaves: where the values start */
} PACKED;

struct leaf_slot {
    uint64_t a, b;
    uint16_t off, len;
} PACKED;

struct inner_slot { /* slot 0's key is below everything */
    uint64_t a, b, child;
} PACKED;

struct entry_rec { /* then the name, the base path and the tiny content */
    uint64_t id, size;
    int64_t mtime;
    uint32_t mode;
    uint8_t type, flags;
    uint16_t name_len, origin_len, tiny_len;
} PACKED;

struct extent_rec {
    uint64_t block, count;
} PACKED;

struct part {
    uint8_t status, first[3], type, last[3];
    uint32_t start, sectors;
} PACKED;

#define MAP_BITS  ((uint64_t)(BS - sizeof(struct map_head)) * 8)
#define MAP_WORDS (MAP_BITS / 64)
#define SUM_PER   ((BS - sizeof(struct sum_head)) / 2)
#define NODE_ROOM (BS - sizeof(struct node_head))
#define LEAF_MAX  (NODE_ROOM / sizeof(struct leaf_slot))
#define INNER_MAX (NODE_ROOM / sizeof(struct inner_slot))

/* ---------------------------------------------------------------- in RAM */

struct key {
    uint64_t a, b;
};

struct node {
    uint64_t block; /* where it was last written (0: not yet) */
    bool dirty;     /* changed since the last commit that reached the medium */
    uint8_t *img;
    struct node **kids; /* inner nodes: the children read in so far, by slot */
};

struct map {
    uint8_t *img;    /* the map block (NULL: not read in) */
    uint64_t *hold;  /* held blocks (all with a clear bit) */
    uint32_t zeros;  /* clear bits */
    uint32_t held;
    uint8_t slot;    /* the place the medium has it in */
    bool dirty;
};

struct span {
    uint64_t block, count;
};

struct item {
    struct vnode *v;
    /* what the commit records, taken when it closes */
    struct content *last; /* the newest version it saves of the document */
    uint64_t parent, size;
    int64_t mtime;
    uint32_t mode;
    uint8_t type;
    bool gone, base_content, tiny;
    char name[NAME_MAX_LEN];
    char origin[PATH_MAX_LEN];
    uint8_t tiny_bytes[CONTENT_TINY];
};

struct save {
    struct vnode *v;
    struct content *c;
    size_t item;
    uint64_t fill; /* pages that reserve still gets blocks for */
};

struct txn {
    uint64_t gen;
    uint64_t since; /* uptime of its first change (0: none) */
    struct item **items;
    size_t nitems, items_room;
    struct save *saves;
    size_t nsaves, saves_room;
    char **hides; /* base paths */
    size_t nhides, hides_room;
    struct span *unfreed; /* blocks versions let go of */
    size_t nunfreed, unfreed_room;
    struct span *held; /* blocks free once the commit is on the medium */
    size_t nheld, held_room;
};

static struct {
    struct ramfs_volume vol;
    struct blkdev *disk;
    struct head hd;
    struct node *root;
    unsigned height;
    struct map *maps;
    uint8_t sum_slot, anchor_slot; /* the places the medium's newest summary and anchor are in */
    uint64_t cursor;               /* where allocation looks first */
    uint64_t next_id, durable;     /* durable: the newest commit on the medium */
    uint64_t avail;                /* blocks neither used nor held */
    uint64_t reserved;             /* dirty pages with no block yet */
    uint64_t meta;                 /* blocks writes leave to the index */
    struct txn open, busy;         /* the commit changes go into / the one on its way */
    bool committing, broken;
    uint64_t retry_at, spill_at;
    unsigned waiters;
    struct {
        uint64_t gen;
        int err;
    } fails[FAILS];
    unsigned nfails;
    bool tree_busy;
    struct wait_queue tree_wq, work_wq, done_wq, room_wq;
    struct task *worker;
    uint8_t *bounce;
} nos;

static void wake(struct wait_queue *wq) {
    uint64_t fl = irq_save();
    wq_wake_all(wq);
    irq_restore(fl);
}

static void seal(void *b) { *(uint32_t *)b = crc32c(0, (uint8_t *)b + 4, BS - 4); }
static bool sealed(const void *b) { return *(const uint32_t *)b == crc32c(0, (const uint8_t *)b + 4, BS - 4); }

static int io(uint64_t block, uint64_t count, void *buf, bool write) {
    uint64_t lba = nos.hd.first_lba + block * SECTORS;
    int r = write ? blk_write(nos.disk, lba, count * SECTORS, buf) : blk_read(nos.disk, lba, count * SECTORS, buf);
    return r < 0 ? r : 0;
}

static uint64_t fnv64(const char *s) {
    uint64_t h = 0xcbf29ce484222325ull;
    while (*s) h = (h ^ (uint8_t)*s++) * 0x100000001b3ull;
    return h;
}

static uint32_t fnv32(const char *s) {
    uint32_t h = 2166136261u;
    while (*s) h = (h ^ (uint8_t)*s++) * 16777619u;
    return h;
}

static void tree_lock(void) {
    uint64_t fl = irq_save();
    while (nos.tree_busy) wq_wait(&nos.tree_wq);
    nos.tree_busy = true;
    irq_restore(fl);
}

static void tree_unlock(void) {
    uint64_t fl = irq_save();
    nos.tree_busy = false;
    wq_wake_all(&nos.tree_wq);
    irq_restore(fl);
}

/* ---------------------------------------------------------------- free space */

static uint64_t *map_bits(struct map *m) { return (uint64_t *)(m->img + sizeof(struct map_head)); }

static void map_count(struct map *m, int64_t zeros, int64_t held) {
    m->zeros += zeros;
    m->held += held;
    nos.avail += zeros - held;
}

static int map_load(uint64_t i) {
    struct map *m = &nos.maps[i];
    if (m->img) return 0;
    uint8_t *img = kmalloc(BS);
    uint64_t *hold = kzalloc(MAP_WORDS * 8);
    int r = img && hold ? io(nos.hd.map_at + m->slot * nos.hd.maps + i, 1, img, false) : -N_ERR_NO_MEMORY;
    const struct map_head *h = (const void *)img;
    if (!r && (!sealed(img) || memcmp(h->magic, "NOSm", 4) || h->index != i || h->volume != (uint32_t)nos.hd.volume_id))
        r = -N_ERR_IO;
    if (r) {
        kprintf("nos: map %lu cannot be read (%d)\n", i, r);
        kfree(img);
        kfree(hold);
        return r;
    }
    m->img = img;
    m->hold = hold;
    uint32_t zeros = 0;
    for (uint64_t w = 0; w < MAP_WORDS; w++) zeros += __builtin_popcountll(~map_bits(m)[w]);
    if (zeros != m->zeros) map_count(m, (int64_t)zeros - m->zeros, 0); /* the bits are what counts */
    return 0;
}

/* The first free block of the map at or after bit i (MAP_BITS: none). */
static uint64_t free_from(struct map *m, uint64_t i) {
    for (uint64_t w = i / 64; w < MAP_WORDS; w++) {
        uint64_t used = map_bits(m)[w] | m->hold[w];
        if (w == i / 64) used |= (1ull << (i % 64)) - 1;
        if (used != UINT64_MAX) return w * 64 + __builtin_ctzll(~used);
    }
    return MAP_BITS;
}

static bool is_free(struct map *m, uint64_t i) { return !((map_bits(m)[i / 64] | m->hold[i / 64]) >> (i % 64) & 1); }

/* Up to want free blocks in a row, held (*got of them; 0: none in the maps read in, *load then
   names one that has room, UINT64_MAX when none has). Never sleeps. */
static uint64_t take(uint64_t want, uint64_t *got, uint64_t *load) {
    *load = UINT64_MAX;
    if (nos.cursor >= nos.hd.blocks) nos.cursor = 0;
    uint64_t first = nos.cursor / MAP_BITS;
    for (uint64_t k = 0; k <= nos.hd.maps; k++) {
        uint64_t i = (first + k) % nos.hd.maps;
        struct map *m = &nos.maps[i];
        if (m->zeros <= m->held) continue;
        if (!m->img) {
            if (*load == UINT64_MAX) *load = i;
            continue;
        }
        uint64_t at = free_from(m, k ? 0 : nos.cursor % MAP_BITS);
        if (at == MAP_BITS) continue;
        uint64_t n = 0;
        while (n < want && at + n < MAP_BITS && is_free(m, at + n)) {
            m->hold[(at + n) / 64] |= 1ull << ((at + n) % 64);
            n++;
        }
        map_count(m, 0, (int64_t)n);
        nos.cursor = i * MAP_BITS + at + n;
        *got = n;
        return i * MAP_BITS + at;
    }
    return 0;
}

enum how {
    B_OWN,     /* the index names it now */
    B_UNINDEX, /* the index lets go of it; a version in RAM may still read it */
    B_FREE,    /* nothing on the medium names it: free now */
    B_RELEASE, /* the commit that let go of it is on the medium */
};

static int mark(uint64_t block, uint64_t count, enum how how) {
    while (count) {
        uint64_t i = block / MAP_BITS, at = block % MAP_BITS, n = MIN(count, MAP_BITS - at);
        int r = map_load(i);
        if (r) return r;
        struct map *m = &nos.maps[i];
        uint64_t *bits = map_bits(m);
        for (uint64_t k = at; k < at + n; k++) {
            uint64_t w = k / 64, bit = 1ull << (k % 64);
            bool used = bits[w] & bit, held = m->hold[w] & bit;
            if (how == B_OWN && !used) {
                bits[w] |= bit;
                map_count(m, -1, 0);
                m->dirty = true;
            }
            if ((how == B_UNINDEX || how == B_FREE) && used) {
                bits[w] &= ~bit;
                map_count(m, 1, 0);
                m->dirty = true;
            }
            if (how == B_UNINDEX && !held) {
                m->hold[w] |= bit;
                map_count(m, 0, 1);
            }
            if (how != B_UNINDEX && held && (how != B_RELEASE || !used)) {
                m->hold[w] &= ~bit;
                map_count(m, 0, -1);
            }
        }
        block += n;
        count -= n;
    }
    return 0;
}

static int add_span(struct span **arr, size_t *n, size_t *room, uint64_t block, uint64_t count) {
    if (*n && (*arr)[*n - 1].block + (*arr)[*n - 1].count == block) {
        (*arr)[*n - 1].count += count;
        return 0;
    }
    struct span *s = kappend(arr, n, room, sizeof *s);
    if (!s) return -N_ERR_NO_MEMORY;
    *s = (struct span){block, count};
    return 0;
}

/* A block the medium's index names goes once the commit on its way is there. */
static int retire(uint64_t block, uint64_t count) {
    struct txn *t = &nos.busy;
    int r = add_span(&t->held, &t->nheld, &t->held_room, block, count);
    return r ? r : mark(block, count, B_UNINDEX);
}

/* One block for the index, used from now on. */
static int meta_take(uint64_t *block) {
    for (;;) {
        uint64_t got, load, b = take(1, &got, &load);
        if (b) {
            *block = b;
            return mark(b, 1, B_OWN);
        }
        if (load == UINT64_MAX) return -N_ERR_NO_SPACE;
        int r = map_load(load);
        if (r) return r;
    }
}

/* ---------------------------------------------------------------- index nodes */

static struct node_head *head_of(struct node *n) { return (struct node_head *)n->img; }
static struct leaf_slot *leaf_slots(struct node *n) { return (struct leaf_slot *)(n->img + sizeof(struct node_head)); }
static struct inner_slot *inner_slots(struct node *n) { return (struct inner_slot *)(n->img + sizeof(struct node_head)); }
static unsigned count_of(struct node *n) { return head_of(n)->count; }
static unsigned level_of(struct node *n) { return head_of(n)->level; }

static int kcmp(struct key x, struct key y) {
    if (x.a != y.a) return x.a < y.a ? -1 : 1;
    if (x.b != y.b) return x.b < y.b ? -1 : 1;
    return 0;
}

static struct key key_at(struct node *n, unsigned i) {
    if (!level_of(n)) return (struct key){leaf_slots(n)[i].a, leaf_slots(n)[i].b};
    return (struct key){inner_slots(n)[i].a, inner_slots(n)[i].b};
}

static void node_free(struct node *n) {
    kfree(n->kids);
    kfree(n->img);
    kfree(n);
}

static struct node *node_blank(void) {
    struct node *n = kzalloc(sizeof *n);
    if (n) n->img = kzalloc(BS);
    if (n && n->img) n->kids = kzalloc((INNER_MAX + 1) * sizeof *n->kids);
    if (n && (!n->img || !n->kids)) {
        node_free(n);
        n = NULL;
    }
    return n;
}

static bool node_valid(struct node *n, unsigned level) {
    struct node_head *h = head_of(n);
    if (!sealed(n->img) || memcmp(h->magic, "NOSn", 4) || h->level != level) return false;
    if (level) return h->count >= 1 && h->count <= INNER_MAX;
    if (h->count > LEAF_MAX || h->top > BS || h->top < sizeof *h + h->count * sizeof(struct leaf_slot)) return false;
    for (unsigned i = 0; i < h->count; i++) {
        struct leaf_slot *s = &leaf_slots(n)[i];
        if (s->off < h->top || s->off + s->len > BS) return false;
    }
    return true;
}

static struct node *node_read(uint64_t block, unsigned level) {
    struct node *n = node_blank();
    if (!n) return NULL;
    if (!level) {
        kfree(n->kids);
        n->kids = NULL;
    }
    if (block < nos.hd.data_at || block >= nos.hd.blocks - 1 || io(block, 1, n->img, false) || !node_valid(n, level)) {
        kprintf("nos: index node %lu cannot be read\n", block);
        node_free(n);
        return NULL;
    }
    n->block = block;
    return n;
}

static struct node *child(struct node *n, unsigned i) {
    if (!n->kids[i]) n->kids[i] = node_read(inner_slots(n)[i].child, level_of(n) - 1);
    return n->kids[i];
}

/* Nodes for splits, had before a change starts so that a change never stops half way. */
static struct node *pool[MAX_HEIGHT + 1];
static unsigned pooled;

static bool pool_fill(unsigned need) {
    while (pooled < need) {
        struct node *n = node_blank();
        if (!n) return false;
        pool[pooled++] = n;
    }
    return true;
}

static struct node *node_new(unsigned level) {
    struct node *n = pool[--pooled];
    memset(n->img, 0, BS);
    memcpy(head_of(n)->magic, "NOSn", 4);
    head_of(n)->level = level;
    head_of(n)->top = BS;
    n->dirty = true;
    return n;
}

/* A node that left the tree (changed in this commit, so the medium's index does not name it). */
static void node_drop(struct node *n) {
    if (n->block) mark(n->block, 1, B_FREE);
    node_free(n);
}

/* ---------------------------------------------------------------- index paths */

struct path {
    unsigned depth;
    struct node *n[MAX_HEIGHT];
    unsigned i[MAX_HEIGHT];
};

static unsigned lower_bound(struct node *n, struct key k) {
    unsigned lo = 0, hi = count_of(n);
    while (lo < hi) {
        unsigned mid = (lo + hi) / 2;
        if (kcmp(key_at(n, mid), k) < 0) lo = mid + 1;
        else hi = mid;
    }
    return lo;
}

/* The leaf where k is or goes, at the first slot at or after it. */
static int descend(struct key k, struct path *p) {
    struct node *n = nos.root;
    for (p->depth = 0;;) {
        unsigned at = lower_bound(n, k);
        p->n[p->depth] = n;
        if (!level_of(n)) {
            p->i[p->depth++] = at;
            return 0;
        }
        if (at == count_of(n) || kcmp(key_at(n, at), k) > 0) at = at ? at - 1 : 0;
        p->i[p->depth++] = at;
        if (!(n = child(n, at))) return -N_ERR_IO;
    }
}

/* The path moves to the next (forward) or previous leaf's first or last slot; 0: there is none. */
static int leaf_step(struct path *p, bool forward) {
    int d = (int)p->depth - 2;
    while (d >= 0 && (forward ? p->i[d] + 1 >= count_of(p->n[d]) : p->i[d] == 0)) d--;
    if (d < 0) return 0;
    p->i[d] += forward ? 1 : -1;
    for (; d + 1 < (int)p->depth; d++) {
        struct node *c = child(p->n[d], p->i[d]);
        if (!c) return -N_ERR_IO;
        p->n[d + 1] = c;
        p->i[d + 1] = forward ? 0 : count_of(c) - 1;
    }
    return 1;
}

/* The slot at the path (moving on to the next leaf at the end of one); 0: past the last. */
static int at_slot(struct path *p, struct key *k, const uint8_t **val, uint16_t *len) {
    for (;;) {
        struct node *l = p->n[p->depth - 1];
        unsigned i = p->i[p->depth - 1];
        if (i < count_of(l)) {
            struct leaf_slot *s = &leaf_slots(l)[i];
            *k = (struct key){s->a, s->b};
            *val = l->img + s->off;
            *len = s->len;
            return 1;
        }
        int r = leaf_step(p, true);
        if (r <= 0) return r;
    }
}

static int step_back(struct path *p) {
    if (p->i[p->depth - 1]) {
        p->i[p->depth - 1]--;
        return 1;
    }
    return leaf_step(p, false);
}

typedef int (*visit_fn)(void *ctx, struct key k, const uint8_t *val, uint16_t len);

/* The keys from lo to hi in order; a visit that returns nonzero stops (an error, or 1: done). */
static int scan(struct key lo, struct key hi, visit_fn visit, void *ctx) {
    struct path p;
    int r = descend(lo, &p);
    if (r) return r;
    for (;; p.i[p.depth - 1]++) {
        struct key k;
        const uint8_t *v;
        uint16_t len;
        if ((r = at_slot(&p, &k, &v, &len)) <= 0) return r;
        if (kcmp(k, hi) > 0) return 0;
        if ((r = visit(ctx, k, v, len))) return r < 0 ? r : 0;
    }
}

/* 1 with k's value (valid until the tree changes), 0: none. */
static int tree_get(struct key k, const uint8_t **val, uint16_t *len) {
    struct path p;
    struct key at;
    int r = descend(k, &p);
    if (!r) r = at_slot(&p, &at, val, len);
    return r < 0 ? r : r && !kcmp(at, k);
}

/* ---------------------------------------------------------------- changing the index */

/* The path's nodes may change: one the medium has gets a new block when the commit writes it, and
   its old block is free once that commit is there. */
static int writable(struct path *p) {
    for (unsigned d = 0; d < p->depth; d++) {
        struct node *n = p->n[d];
        if (n->dirty) continue;
        int r = n->block ? retire(n->block, 1) : 0;
        if (r) return r;
        n->block = 0;
        n->dirty = true;
    }
    return 0;
}

static int node_remove(struct path *p, unsigned d);

/* Child c goes into the node at depth d of the path, after the path's slot (a new root over the
   old one when d is -1); the node splits when it is full. Never fails: pool_fill came first. */
static void inner_insert(struct path *p, int d, struct key k, struct node *c) {
    static struct inner_slot slots[INNER_MAX + 1];
    static struct node *kids[INNER_MAX + 1];
    if (d < 0) {
        struct node *r = node_new(nos.height), *old = nos.root;
        inner_slots(r)[0] = (struct inner_slot){0, 0, old->block};
        inner_slots(r)[1] = (struct inner_slot){k.a, k.b, c->block};
        r->kids[0] = old;
        r->kids[1] = c;
        head_of(r)->count = 2;
        nos.root = r;
        nos.height++;
        return;
    }
    struct node *n = p->n[d];
    unsigned at = p->i[d] + 1, count = count_of(n);
    memcpy(slots, inner_slots(n), at * sizeof *slots);
    memcpy(kids, n->kids, at * sizeof *kids);
    slots[at] = (struct inner_slot){k.a, k.b, c->block};
    kids[at] = c;
    memcpy(slots + at + 1, inner_slots(n) + at, (count - at) * sizeof *slots);
    memcpy(kids + at + 1, n->kids + at, (count - at) * sizeof *kids);
    count++;
    unsigned keep = count <= INNER_MAX ? count : count / 2;
    memcpy(inner_slots(n), slots, keep * sizeof *slots);
    memcpy(n->kids, kids, keep * sizeof *kids);
    memset(n->kids + keep, 0, (INNER_MAX + 1 - keep) * sizeof *kids);
    head_of(n)->count = keep;
    if (keep == count) return;
    struct node *r = node_new(level_of(n));
    memcpy(inner_slots(r), slots + keep, (count - keep) * sizeof *slots);
    memcpy(r->kids, kids + keep, (count - keep) * sizeof *kids);
    head_of(r)->count = count - keep;
    inner_insert(p, d - 1, (struct key){slots[keep].a, slots[keep].b}, r);
}

static struct leaf_rec {
    struct key k;
    uint16_t len;
    const uint8_t *val;
} recs[LEAF_MAX + 1];
static uint8_t spare[2 * BS];

static void leaf_fill(struct node *n, const struct leaf_rec *r, unsigned count) {
    uint16_t top = BS;
    for (unsigned i = 0; i < count; i++) {
        top -= r[i].len;
        memcpy(n->img + top, r[i].val, r[i].len);
        leaf_slots(n)[i] = (struct leaf_slot){r[i].k.a, r[i].k.b, top, r[i].len};
    }
    head_of(n)->count = count;
    head_of(n)->top = top;
}

/* The path's leaf (writable) with k put at the path's slot (replace: over the one there) or that
   slot removed (val NULL); split in two when it no longer fits, gone when it empties. */
static int leaf_change(struct path *p, struct key k, const void *val, uint16_t len, bool replace) {
    struct node *l = p->n[p->depth - 1];
    unsigned at = p->i[p->depth - 1], count = count_of(l), n = 0;
    size_t used = 0;
    for (unsigned i = 0; i <= count; i++) {
        if (i == at && val) {
            memcpy(spare + used, val, len);
            recs[n++] = (struct leaf_rec){k, len, spare + used};
            used += len;
        }
        if (i == count) break;
        if (i == at && (replace || !val)) continue;
        struct leaf_slot *s = &leaf_slots(l)[i];
        memcpy(spare + used, l->img + s->off, s->len);
        recs[n++] = (struct leaf_rec){{s->a, s->b}, s->len, spare + used};
        used += s->len;
    }
    size_t need = used + n * sizeof(struct leaf_slot);
    if (need <= NODE_ROOM) {
        leaf_fill(l, recs, n);
        return n || p->depth == 1 ? 0 : node_remove(p, p->depth - 1);
    }
    /* split where the larger half is smallest */
    size_t best = SIZE_MAX, acc = 0;
    unsigned split = 1;
    for (unsigned i = 1; i < n; i++) {
        acc += recs[i - 1].len + sizeof(struct leaf_slot);
        size_t worst = MAX(acc, need - acc);
        if (worst < best) {
            best = worst;
            split = i;
        }
    }
    if (best > NODE_ROOM) return -N_ERR_TOO_LARGE;
    unsigned grow = 1; /* the new leaf, and an inner node for each full one above it */
    int d = (int)p->depth - 2;
    while (d >= 0 && count_of(p->n[d]) == INNER_MAX) d--, grow++;
    if (d < 0) grow++;
    if (nos.height + (d < 0) > MAX_HEIGHT) return -N_ERR_TOO_LARGE;
    if (!pool_fill(grow)) return -N_ERR_NO_MEMORY;
    struct node *r = node_new(0);
    leaf_fill(r, recs + split, n - split);
    leaf_fill(l, recs, split);
    inner_insert(p, (int)p->depth - 2, recs[split].k, r);
    return 0;
}

/* The node at depth d of the path is empty: it goes, and so does a parent it empties; a root with
   one child gives way to that child. */
static int node_remove(struct path *p, unsigned d) {
    if (!d) return 0; /* an empty root leaf: the empty index */
    struct node *up = p->n[d - 1];
    unsigned at = p->i[d - 1], count = count_of(up);
    node_drop(p->n[d]);
    memmove(&inner_slots(up)[at], &inner_slots(up)[at + 1], (count - at - 1) * sizeof(struct inner_slot));
    memmove(&up->kids[at], &up->kids[at + 1], (count - at - 1) * sizeof *up->kids);
    up->kids[--count] = NULL;
    head_of(up)->count = count;
    if (!count) return node_remove(p, d - 1);
    while (nos.height > 1 && count_of(nos.root) == 1) {
        struct node *old = nos.root, *c = child(old, 0);
        if (!c) return -N_ERR_IO;
        old->kids[0] = NULL;
        node_drop(old);
        nos.root = c;
        nos.height--;
    }
    return 0;
}

static int tree_put(struct key k, const void *val, uint16_t len) {
    struct path p;
    int r = descend(k, &p);
    if (r) return r;
    struct node *l = p.n[p.depth - 1];
    unsigned i = p.i[p.depth - 1];
    bool same = i < count_of(l) && !kcmp(key_at(l, i), k);
    if ((r = writable(&p))) return r;
    return leaf_change(&p, k, val, len, same);
}

static int tree_del(struct key k) {
    struct path p;
    int r = descend(k, &p);
    if (r) return r;
    struct node *l = p.n[p.depth - 1];
    unsigned i = p.i[p.depth - 1];
    if (i >= count_of(l) || kcmp(key_at(l, i), k)) return 0;
    if ((r = writable(&p))) return r;
    return leaf_change(&p, k, NULL, 0, true);
}

/* Blocks for the dirty nodes (children first: their parents name the blocks), sealed. */
static int place(struct node *n) {
    if (!n->dirty) return 0;
    for (unsigned i = 0; level_of(n) && i < count_of(n); i++) {
        struct node *c = n->kids[i];
        if (!c || !c->dirty) continue;
        int r = place(c);
        if (r) return r;
        inner_slots(n)[i].child = c->block;
    }
    int r = n->block ? 0 : meta_take(&n->block);
    if (!r) seal(n->img);
    return r;
}

static int write_nodes(struct node *n) {
    if (!n->dirty) return 0;
    for (unsigned i = 0; level_of(n) && i < count_of(n); i++) {
        int r = n->kids[i] ? write_nodes(n->kids[i]) : 0;
        if (r) return r;
    }
    return io(n->block, 1, n->img, true);
}

static void nodes_settled(struct node *n) {
    if (!n->dirty) return;
    for (unsigned i = 0; level_of(n) && i < count_of(n); i++)
        if (n->kids[i]) nodes_settled(n->kids[i]);
    n->dirty = false;
}

/* ---------------------------------------------------------------- extents */

static struct key ext_key(uint64_t id, uint64_t page) { return (struct key){id, KIND(K_EXTENT) | page}; }

static bool ext_of(uint64_t id, struct key k, uint16_t len) {
    return k.a == id && k.b >> 56 == K_EXTENT && len == sizeof(struct extent_rec);
}

/* The extent of document id that overlaps pages [page, end) first: 1, its first page in *start. */
static int ext_first(uint64_t id, uint64_t page, uint64_t end, uint64_t *start, struct extent_rec *e) {
    struct path p, back;
    struct key k;
    const uint8_t *v;
    uint16_t len;
    int r = descend(ext_key(id, page), &p);
    if (r) return r;
    back = p;
    /* only the one before can reach in from below: extents do not overlap */
    if ((r = step_back(&back)) > 0 && (r = at_slot(&back, &k, &v, &len)) > 0 && ext_of(id, k, len)) {
        memcpy(e, v, sizeof *e);
        *start = k.b & (PAGE_END - 1);
        if (*start + e->count > page) return 1;
    }
    if (r < 0 || (r = at_slot(&p, &k, &v, &len)) <= 0) return r;
    if (!ext_of(id, k, len)) return 0;
    memcpy(e, v, sizeof *e);
    *start = k.b & (PAGE_END - 1);
    return *start < end;
}

static int put_extent(uint64_t id, uint64_t page, uint64_t block, uint64_t count) {
    struct extent_rec e = {block, count};
    return tree_put(ext_key(id, page), &e, sizeof e);
}

/* Pages [page, end) of document id are at the blocks from block on (0: nowhere). What the index
   named there before is let go of; the version that has those blocks drops them later. */
static int extents_set(uint64_t id, uint64_t page, uint64_t end, uint64_t block) {
    for (;;) {
        uint64_t s;
        struct extent_rec e;
        int r = ext_first(id, page, end, &s, &e);
        if (r <= 0) {
            if (r) return r;
            break;
        }
        uint64_t from = MAX(s, page), to = MIN(s + e.count, end);
        if ((r = tree_del(ext_key(id, s)))) return r;
        if (s < page && (r = put_extent(id, s, e.block, page - s))) return r;
        if (s + e.count > end && (r = put_extent(id, end, e.block + (end - s), s + e.count - end))) return r;
        if ((r = mark(e.block + (from - s), to - from, B_UNINDEX))) return r;
    }
    if (!block) return 0;
    int r = put_extent(id, page, block, end - page);
    return r ? r : mark(block, end - page, B_OWN);
}

static int put_run(void *ctx, uint64_t page, uint64_t count, uint64_t block) {
    return extents_set(*(uint64_t *)ctx, page, page + count, block);
}

/* ---------------------------------------------------------------- entries and hides */

static uint8_t entry_buf[sizeof(struct entry_rec) + NAME_MAX_LEN + PATH_MAX_LEN + CONTENT_TINY];

struct seq_ctx {
    const char *path; /* hides: the one looked for */
    uint64_t next;
    bool found;
};

static int seq_visit(void *ctx, struct key k, const uint8_t *val, uint16_t len) {
    struct seq_ctx *c = ctx;
    if (c->path && len == strlen(c->path) && !memcmp(val, c->path, len)) {
        c->found = true;
        return 1;
    }
    if ((k.b & SEQ_MAX) == c->next) c->next++;
    return 0;
}

/* The first key of bucket (a, b) nothing uses (hides: *found when path is there already). */
static int bucket_free(uint64_t a, uint64_t b, const char *path, struct key *out, bool *found) {
    struct seq_ctx c = {path, 0, false};
    int r = scan((struct key){a, b}, (struct key){a, b | SEQ_MAX}, seq_visit, &c);
    if (r) return r;
    if (found) *found = c.found;
    if (c.next > SEQ_MAX) return -N_ERR_NO_SPACE;
    *out = (struct key){a, b | c.next};
    return 0;
}

static int apply_item(struct item *it) {
    struct vnode *v = it->v;
    struct key was = {v->entry_dir, v->entry_pos};
    const uint8_t *old = NULL;
    uint16_t old_len = 0;
    int r = v->entry_dir ? tree_get(was, &old, &old_len) : 0;
    if (r < 0) return r;
    if (!r) old = NULL;
    const struct entry_rec *o = (const void *)old;
    if (it->gone) {
        if (old && (r = tree_del(was))) return r;
        v->entry_dir = 0;
        return it->type == VT_FILE ? extents_set(v->id, 0, PAGE_END, 0) : 0;
    }
    struct entry_rec *e = (struct entry_rec *)entry_buf;
    size_t nl = strlen(it->name), ol = strlen(it->origin);
    *e = (struct entry_rec){.id = v->id, .size = it->size, .mtime = it->mtime, .mode = it->mode, .type = it->type,
                            .name_len = nl, .origin_len = ol};
    const uint8_t *tiny = it->tiny_bytes;
    if (it->last) {
        e->flags = it->tiny ? EF_TINY : 0;
        e->tiny_len = it->tiny ? it->size : 0;
    } else if (old && old_len >= sizeof *o) { /* the content is as the entry had it */
        e->size = o->size;
        e->flags = o->flags;
        e->tiny_len = o->tiny_len;
        tiny = old + sizeof *o + o->name_len + o->origin_len;
    } else {
        e->flags = it->base_content ? EF_BASE_CONTENT : 0;
    }
    uint8_t *at = entry_buf + sizeof *e;
    memcpy(at, it->name, nl);
    memcpy(at + nl, it->origin, ol);
    memcpy(at + nl + ol, tiny, e->tiny_len);
    uint16_t len = sizeof *e + nl + ol + e->tiny_len;
    bool same = old && was.a == it->parent && o->name_len == nl && !memcmp(old + sizeof *o, it->name, nl);
    if (same) return tree_put(was, entry_buf, len);
    if (old && (r = tree_del(was))) return r;
    struct key k;
    if ((r = bucket_free(it->parent, KIND(K_ENTRY) | (uint64_t)fnv32(it->name) << 24, NULL, &k, NULL)) ||
        (r = tree_put(k, entry_buf, len)))
        return r;
    v->entry_dir = k.a;
    v->entry_pos = k.b;
    return 0;
}

/* The base path's folder part ("/" for an item at the top), into out. */
static const char *path_split(const char *path, char *out) {
    const char *slash = strrchr(path, '/');
    size_t n = slash > path ? (size_t)(slash - path) : 1;
    memcpy(out, path, n);
    out[n] = 0;
    return slash + 1;
}

static int apply_hide(const char *path) {
    char folder[PATH_MAX_LEN];
    const char *name = path_split(path, folder);
    struct key k;
    bool found;
    int r = bucket_free(fnv64(folder), KIND(K_HIDE) | (uint64_t)fnv32(name) << 24, path, &k, &found);
    return r || found ? r : tree_put(k, path, strlen(path));
}

/* ---------------------------------------------------------------- the open commit */

static bool pending(struct txn *t) { return t->nitems || t->nsaves || t->nhides || t->nunfreed || t->nheld; }

static void touch(void) {
    if (nos.open.since) return;
    nos.open.since = MAX(uptime_ms(), 1);
    wake(&nos.work_wq);
}

static struct item *txn_item(struct vnode *v) {
    struct txn *t = &nos.open;
    if (v->vol_txn == t->gen) return t->items[v->vol_item];
    struct item *it = kzalloc(sizeof *it);
    struct item **at = it ? kappend(&t->items, &t->nitems, &t->items_room, sizeof *at) : NULL;
    if (!at) {
        kfree(it);
        return NULL;
    }
    vnode_ref(v);
    it->v = v;
    *at = it;
    v->vol_txn = t->gen;
    v->vol_item = t->nitems - 1;
    touch();
    return it;
}

/* What the item records, as the document is when its commit closes (never sleeps). */
static void snapshot(struct item *it) {
    struct vnode *v = it->v;
    it->parent = v->parent ? v->parent->id : 0;
    strlcpy(it->name, v->name, sizeof it->name);
    it->type = v->type;
    it->mode = v->mode;
    it->mtime = v->mtime;
    it->gone = v->unlinked;
    if (!v->origin || ramfs_base_path(v->origin, it->origin, sizeof it->origin)) it->origin[0] = 0;
    it->tiny = it->last && content_is_tiny(it->last);
    it->size = it->last ? content_size(it->last) : v->content ? content_size(v->content) : 0;
    it->base_content = !it->last && ramfs_reads_base(v);
    if (it->tiny) content_read(it->last, it->tiny_bytes, 0, it->size);
}

static void close_txn(void) {
    struct txn *t = &nos.busy;
    *t = nos.open;
    memset(&nos.open, 0, sizeof nos.open);
    nos.open.gen = t->gen + 1;
    for (size_t i = 0; i < t->nitems; i++) t->items[i]->last = NULL;
    for (size_t i = 0; i < t->nsaves; i++) {
        struct save *s = &t->saves[i];
        s->item = s->v->vol_item; /* v->vol_txn is t's until something joins the new one */
        t->items[s->item]->last = s->c;
    }
    for (size_t i = 0; i < t->nitems; i++) snapshot(t->items[i]);
}

static void txn_free(struct txn *t) {
    for (size_t i = 0; i < t->nitems; i++)
        if (t->items[i]) {
            vnode_unref(t->items[i]->v);
            kfree(t->items[i]);
        }
    for (size_t i = 0; i < t->nsaves; i++) {
        vnode_unref(t->saves[i].v);
        content_unref(t->saves[i].c);
    }
    for (size_t i = 0; i < t->nhides; i++) kfree(t->hides[i]);
    kfree(t->items);
    kfree(t->saves);
    kfree(t->hides);
    kfree(t->unfreed);
    kfree(t->held);
    uint64_t gen = t->gen;
    memset(t, 0, sizeof *t);
    t->gen = gen;
}

/* src's n elements go before dst's (nothing is lost but on no memory, which is logged). */
static void prepend(void *dst_at, size_t *dn, size_t *droom, void *src, size_t n, size_t size) {
    void **dst = dst_at;
    if (!n) return;
    uint8_t *grown = krealloc(*dst, (*dn + n) * size);
    if (!grown) {
        kprintf("nos: no memory to keep a failed commit's changes\n");
        return;
    }
    memmove(grown + n * size, grown, *dn * size);
    memcpy(grown, src, n * size);
    *dst = grown;
    *dn += n;
    *droom = *dn;
}

/* A commit that failed: its changes go into the open one, before what came since. */
static void carry(struct txn *t) {
    struct txn *o = &nos.open;
    for (size_t i = 0; i < t->nitems; i++) {
        struct item *it = t->items[i], **at;
        t->items[i] = NULL;
        if (it->v->vol_txn == o->gen || !(at = kappend(&o->items, &o->nitems, &o->items_room, sizeof *at))) {
            vnode_unref(it->v); /* the open one records it anyway */
            kfree(it);
            continue;
        }
        *at = it;
        it->v->vol_txn = o->gen;
        it->v->vol_item = o->nitems - 1;
    }
    size_t n = o->nsaves;
    prepend(&o->saves, &o->nsaves, &o->saves_room, t->saves, t->nsaves, sizeof *t->saves);
    if (o->nsaves != n) t->nsaves = 0;
    n = o->nhides;
    prepend(&o->hides, &o->nhides, &o->hides_room, t->hides, t->nhides, sizeof *t->hides);
    if (o->nhides != n) t->nhides = 0;
    prepend(&o->unfreed, &o->nunfreed, &o->unfreed_room, t->unfreed, t->nunfreed, sizeof *t->unfreed);
    prepend(&o->held, &o->nheld, &o->held_room, t->held, t->nheld, sizeof *t->held);
    touch();
}

/* ---------------------------------------------------------------- the store */

static int store_read(struct content *c, uint64_t block, uint64_t count, const uint64_t *frames) {
    (void)c;
    uint8_t *buf = count > 1 ? kmalloc(count * BS) : NULL;
    if (buf) {
        int r = io(block, count, buf, false);
        for (uint64_t i = 0; !r && i < count; i++) memcpy(phys_to_virt(frames[i]), buf + i * BS, BS);
        kfree(buf);
        return r;
    }
    for (uint64_t i = 0; i < count; i++) {
        int r = io(block + i, 1, phys_to_virt(frames[i]), false);
        if (r) return r;
    }
    return 0;
}

static bool frees_pending(void) {
    return nos.open.nunfreed || nos.open.nheld || (nos.committing && (nos.busy.nheld || nos.busy.nunfreed));
}

static int nos_saved(struct ramfs_volume *vol, uint64_t ticket, bool wait);

/* Room for pages more blocks than are promised, past what the index keeps for itself. Blocks on
   their way back come with a commit or two. */
static int room(uint64_t pages) {
    for (int tries = 0;; tries++) {
        if (nos.avail >= nos.reserved + pages + nos.meta) return 0;
        if (tries == 2 || !frees_pending() || current_task == nos.worker) return -N_ERR_NO_SPACE;
        nos_saved(&nos.vol, pending(&nos.open) ? nos.open.gen : nos.busy.gen, true);
    }
}

static uint64_t spill_mark(void) { return pmm_total_pages() / 8; }

static int store_dirty(struct content *c, int64_t pages, bool forced) {
    (void)c;
    if (pages <= 0) {
        nos.reserved -= MIN(nos.reserved, (uint64_t)-pages);
        wake(&nos.room_wq);
        return 0;
    }
    if (!forced && current_task != nos.worker) {
        /* dirty pages past a quarter of RAM wait for the spill a while (then go on: RAM decides) */
        for (int i = 0; i < 20 && nos.reserved + pages > 2 * spill_mark(); i++) {
            wake(&nos.work_wq);
            uint64_t fl = irq_save();
            wq_wait_timeout(&nos.room_wq, 50);
            irq_restore(fl);
        }
        int r = room(pages);
        if (r) return r;
    }
    nos.reserved += pages;
    if (nos.reserved > spill_mark()) wake(&nos.work_wq);
    return 0;
}

static void store_drop(struct content *c, uint64_t block, uint64_t count) {
    (void)c;
    struct txn *t = &nos.open;
    if (add_span(&t->unfreed, &t->nunfreed, &t->unfreed_room, block, count))
        kprintf("nos: no memory: blocks %lu+%lu stay used until restart\n", block, count);
    touch();
}

static const struct content_store store = {store_read, store_dirty, store_drop};

/* ---------------------------------------------------------------- the worker */

/* c's dirty pages to blocks: its own where it has them, new ones where not. commit: all of them;
   otherwise a spill, which stops once a save took c. */
static int write_back(struct content *c, bool commit) {
    for (uint64_t page = 0;;) {
        enum content_state st = content_state(c);
        if (!commit && st != CONTENT_WORKING && st != CONTENT_FROZEN) return 0;
        uint64_t n = content_dirty_run(c, &page, BOUNCE);
        if (!n) return 0;
        uint64_t left, block = content_block(c, page, &left);
        bool fresh = !block;
        n = MIN(n, left);
        if (fresh) {
            uint64_t got, load;
            if (!(block = take(n, &got, &load))) {
                if (load == UINT64_MAX) return -N_ERR_NO_SPACE;
                int r = map_load(load);
                if (r) return r;
                continue;
            }
            n = got;
        }
        int r = content_snapshot(c, page, n, nos.bounce, block);
        if (r < 0) {
            if (fresh) mark(block, n, B_RELEASE);
            return r;
        }
        if (fresh) store_dirty(c, -(int64_t)n, true); /* the reservation became these blocks */
        if ((r = io(block, n, nos.bounce, true))) {
            content_write_failed(c, page, n);
            return r;
        }
        content_written(c, page, n);
        page += n;
    }
}

/* Blocks for a reserved version, nothing written to them. */
static int fill(struct save *s) {
    uint64_t pages = (content_size(s->c) + BS - 1) / BS;
    while (s->fill) {
        uint64_t got, load, b = take(s->fill, &got, &load);
        if (!b) {
            int r = load == UINT64_MAX ? -N_ERR_NO_SPACE : map_load(load);
            if (r) return r;
            continue;
        }
        if (content_map(s->c, pages - s->fill, got, b)) {
            mark(b, got, B_RELEASE);
            return -N_ERR_NO_MEMORY;
        }
        store_dirty(s->c, -(int64_t)got, true);
        s->fill -= got;
    }
    return 0;
}

/* The commit's changes go into the index (and its frees into its held blocks). */
static int apply(struct txn *t) {
    int r = 0;
    for (size_t i = 0; i < t->nsaves && !r; i++) {
        struct save *s = &t->saves[i];
        if (t->items[s->item]->gone) continue;
        uint64_t id = s->v->id;
        if (!(r = extents_set(id, content_base_limit(s->c), PAGE_END, 0))) r = content_extents(s->c, put_run, &id);
    }
    for (size_t i = 0; i < t->nitems && !r; i++) r = apply_item(t->items[i]);
    for (size_t i = 0; i < t->nhides && !r; i++) r = apply_hide(t->hides[i]);
    /* let go of after the index did: what it still names stays (all of them or, failing, none:
       the retry adds them again, and a block held twice would be freed under its next owner) */
    size_t held = t->nheld;
    struct span tail = held ? t->held[held - 1] : (struct span){0, 0}; /* add_span may grow it */
    for (size_t i = 0; i < t->nunfreed && !r; i++) {
        for (uint64_t b = t->unfreed[i].block, end = b + t->unfreed[i].count; b < end && !r; b++) {
            struct map *m = &nos.maps[b / MAP_BITS];
            uint64_t at = b % MAP_BITS;
            if ((r = map_load(b / MAP_BITS))) break;
            if (!(map_bits(m)[at / 64] >> (at % 64) & 1) && (m->hold[at / 64] >> (at % 64) & 1))
                r = add_span(&t->held, &t->nheld, &t->held_room, b, 1);
        }
    }
    if (r && held) t->held[held - 1] = tail;
    if (r) t->nheld = held;
    else t->nunfreed = 0;
    return r;
}

/* Nodes, maps, summary; persist; the anchor; persist. */
static int write_meta(struct txn *t) {
    tree_lock();
    int r = place(nos.root);
    tree_unlock();
    if (!r) r = write_nodes(nos.root);
    for (uint64_t i = 0; i < nos.hd.maps && !r; i++) {
        struct map *m = &nos.maps[i];
        if (!m->dirty) continue;
        struct map_head *h = (struct map_head *)m->img;
        memcpy(h->magic, "NOSm", 4);
        h->index = i;
        h->volume = (uint32_t)nos.hd.volume_id;
        seal(m->img);
        r = io(nos.hd.map_at + (m->slot ^ 1) * nos.hd.maps + i, 1, m->img, true);
    }
    uint64_t free = 0;
    for (uint64_t k = 0; k < nos.hd.sum_blocks && !r; k++) {
        uint8_t *b = nos.bounce;
        memset(b, 0, BS);
        struct sum_head *h = (struct sum_head *)b;
        memcpy(h->magic, "NOSs", 4);
        h->volume_id = nos.hd.volume_id;
        h->gen = t->gen;
        h->index = k;
        uint16_t *e = (uint16_t *)(b + sizeof *h);
        for (uint64_t j = 0, i = k * SUM_PER; j < SUM_PER && i < nos.hd.maps; j++, i++) {
            struct map *m = &nos.maps[i];
            e[j] = m->zeros | (m->slot ^ m->dirty) << 15;
            free += m->zeros;
        }
        seal(b);
        r = io(nos.hd.sum_at + (nos.sum_slot ^ 1) * nos.hd.sum_blocks + k, 1, b, true);
    }
    if (!r) r = blk_persist(nos.disk);
    if (r) return r < 0 ? r : -N_ERR_IO;
    struct anchor *a = (struct anchor *)nos.bounce;
    memset(a, 0, BS);
    memcpy(a->magic, "NOSa", 4);
    a->volume_id = nos.hd.volume_id;
    a->gen = t->gen;
    a->root = nos.root->block;
    a->next_id = nos.next_id;
    a->free = free;
    a->height = nos.height;
    a->sum_slot = nos.sum_slot ^ 1;
    seal(a);
    /* Whether a failed anchor reached the medium is not known: once more, and when that fails too
       nothing is written any more (the next commit could write over what it names). */
    for (int tries = 0; tries < 2; tries++) {
        r = io(1 + (nos.anchor_slot ^ 1), 1, a, true);
        if (!r) r = blk_persist(nos.disk);
        if (!r) return 0;
    }
    nos.broken = true;
    kprintf("nos: %s: the anchor cannot be written (%d); nothing more is saved until restart\n", nos.disk->name, r);
    return r < 0 ? r : -N_ERR_IO;
}

static void finish(struct txn *t, int r) {
    if (!r) {
        for (uint64_t i = 0; i < nos.hd.maps; i++)
            if (nos.maps[i].dirty) {
                nos.maps[i].slot ^= 1;
                nos.maps[i].dirty = false;
            }
        nos.sum_slot ^= 1;
        nos.anchor_slot ^= 1;
        nodes_settled(nos.root);
        nos.durable = t->gen;
        for (size_t i = 0; i < t->nheld; i++) mark(t->held[i].block, t->held[i].count, B_RELEASE);
        t->nheld = 0;
    } else {
        nos.fails[nos.nfails++ % FAILS] = (typeof(nos.fails[0])){t->gen, r};
        nos.retry_at = uptime_ms() + 1000;
        kprintf("nos: commit %lu failed (%d)\n", t->gen, r);
    }
    for (size_t i = 0; i < t->nsaves; i++) ramfs_saved(t->saves[i].v, t->saves[i].c, r);
    if (r && !nos.broken) carry(t);
    txn_free(t);
    wake(&nos.done_wq);
    wake(&nos.room_wq);
}

static void commit(void) {
    struct txn *t = &nos.busy;
    close_txn();
    nos.committing = true;
    int r = nos.broken ? -N_ERR_IO : 0;
    for (size_t i = 0; i < t->nsaves && !r; i++) {
        struct save *s = &t->saves[i];
        if (t->items[s->item]->gone) continue;
        if (s->fill) {
            r = fill(s);
        } else {
            content_saving(s->c);
            r = write_back(s->c, true);
        }
    }
    if (!r) {
        tree_lock();
        r = apply(t);
        tree_unlock();
    }
    if (!r) r = write_meta(t);
    finish(t, r);
    nos.committing = false;
}

static bool commit_due(uint64_t now) {
    if (!pending(&nos.open) || now < nos.retry_at) return false;
    return nos.open.nsaves || nos.waiters || now >= nos.open.since + 1000;
}

static bool spill_due(uint64_t now) { return nos.reserved > spill_mark() && now >= nos.spill_at; }

static void spill(void) {
    struct content *c = content_spill_pick();
    int r = c ? 0 : -N_ERR_NOT_FOUND;
    if (c) {
        content_ref(c);
        r = write_back(c, false);
        content_unref(c);
    }
    if (r) nos.spill_at = uptime_ms() + 1000; /* nothing to spill, or no room: not again at once */
    wake(&nos.room_wq);
}

static void worker(void *arg) {
    (void)arg;
    for (;;) {
        uint64_t fl = irq_save();
        for (;;) {
            uint64_t now = uptime_ms();
            if (commit_due(now) || spill_due(now)) break;
            uint64_t at = UINT64_MAX;
            if (pending(&nos.open)) at = MAX(nos.open.since + 1000, nos.retry_at);
            if (nos.reserved > spill_mark()) at = MIN(at, nos.spill_at);
            if (at == UINT64_MAX) wq_wait(&nos.work_wq);
            else wq_wait_timeout(&nos.work_wq, at > now ? at - now : 1);
        }
        irq_restore(fl);
        if (commit_due(uptime_ms())) commit();
        else spill();
    }
}

/* ---------------------------------------------------------------- the namespace's hooks */

static int entry_visit(void *ctx, struct key k, const uint8_t *val, uint16_t len) {
    const struct entry_rec *e = (const void *)val;
    if (len < sizeof *e || sizeof *e + e->name_len + e->origin_len + e->tiny_len > len || !e->name_len ||
        e->name_len >= NAME_MAX_LEN || e->origin_len >= PATH_MAX_LEN)
        return -N_ERR_IO;
    char name[NAME_MAX_LEN], origin[PATH_MAX_LEN];
    memcpy(name, val + sizeof *e, e->name_len);
    name[e->name_len] = 0;
    memcpy(origin, val + sizeof *e + e->name_len, e->origin_len);
    origin[e->origin_len] = 0;
    if (e->id >= nos.next_id) nos.next_id = e->id + 1;
    struct ramfs_entry re = {name, e->type, e->mode, e->id, e->size, e->mtime, origin, e->flags & EF_BASE_CONTENT, k.b};
    return ramfs_found(ctx, &re);
}

struct hide_ctx {
    struct ramfs_load *ld;
    const char *folder;
};

static int hide_visit(void *ctx, struct key k, const uint8_t *val, uint16_t len) {
    (void)k;
    struct hide_ctx *h = ctx;
    char path[PATH_MAX_LEN], folder[PATH_MAX_LEN];
    if (!len || len >= PATH_MAX_LEN) return -N_ERR_IO;
    memcpy(path, val, len);
    path[len] = 0;
    const char *name = path_split(path, folder);
    return strcmp(folder, h->folder) ? 0 : ramfs_hidden(h->ld, name); /* another folder's, same hash */
}

static int nos_load(struct ramfs_volume *vol, struct vnode *dir, struct ramfs_load *ld) {
    (void)vol;
    char folder[PATH_MAX_LEN];
    tree_lock();
    int r = scan((struct key){dir->id, KIND(K_ENTRY)}, (struct key){dir->id, KIND(K_ENTRY + 1) - 1}, entry_visit, ld);
    if (!r && dir->origin && !(r = ramfs_base_path(dir->origin, folder, sizeof folder))) {
        struct hide_ctx h = {ld, folder};
        uint64_t a = fnv64(folder);
        r = scan((struct key){a, KIND(K_HIDE)}, (struct key){a, KIND(K_HIDE + 1) - 1}, hide_visit, &h);
    }
    tree_unlock();
    return r;
}

static int extent_visit(void *ctx, struct key k, const uint8_t *val, uint16_t len) {
    struct extent_rec e;
    if (len != sizeof e) return -N_ERR_IO;
    memcpy(&e, val, sizeof e);
    return content_map(ctx, k.b & (PAGE_END - 1), e.count, e.block);
}

static int nos_content(struct ramfs_volume *vol, struct vnode *v, struct content **out) {
    (void)vol;
    struct content *c = content_new(&store);
    if (!c) return -N_ERR_NO_MEMORY;
    const uint8_t *val;
    uint16_t len;
    tree_lock();
    int r = tree_get((struct key){v->entry_dir, v->entry_pos}, &val, &len);
    const struct entry_rec *e = (const void *)val;
    if (r == 1 && len >= sizeof *e && e->id == v->id) {
        if (e->flags & EF_TINY) {
            r = content_load_tiny(c, val + sizeof *e + e->name_len + e->origin_len, e->tiny_len);
        } else {
            content_load(c, e->size);
            r = scan(ext_key(v->id, 0), ext_key(v->id, PAGE_END - 1), extent_visit, c);
        }
    } else if (r >= 0) {
        r = -N_ERR_IO;
    }
    tree_unlock();
    if (r) {
        content_unref(c);
        return r;
    }
    *out = c;
    return 0;
}

static int nos_changed(struct ramfs_volume *vol, struct vnode *v) {
    (void)vol;
    if (v == vfs_root) return 0;
    if (!v->unlinked && nos.avail < nos.reserved + nos.meta / 2) return -N_ERR_NO_SPACE;
    if (!v->id) v->id = nos.next_id++;
    return txn_item(v) ? 0 : -N_ERR_NO_MEMORY;
}

static int nos_hide(struct ramfs_volume *vol, struct vnode *item) {
    (void)vol;
    char path[PATH_MAX_LEN];
    int r = ramfs_base_path(item, path, sizeof path);
    if (r) return r;
    char *copy = strdup(path), **at = copy ? kappend(&nos.open.hides, &nos.open.nhides, &nos.open.hides_room, sizeof *at) : NULL;
    if (!at) {
        kfree(copy);
        return -N_ERR_NO_MEMORY;
    }
    *at = copy;
    touch();
    return 0;
}

/* A version saved after one kept in an entry holds the pages it read from it: the entry changes. */
static bool reads_tiny(struct content *c) {
    for (c = content_base(c); c; c = content_base(c))
        if (content_is_tiny(c)) return true;
    return false;
}

static int add_save(struct vnode *v, struct content *c, uint64_t fill, uint64_t *ticket) {
    struct txn *t = &nos.open;
    if (!txn_item(v)) return -N_ERR_NO_MEMORY;
    struct save *s = kappend(&t->saves, &t->nsaves, &t->saves_room, sizeof *s);
    if (!s) return -N_ERR_NO_MEMORY;
    vnode_ref(v);
    *s = (struct save){v, content_ref(c), 0, fill};
    *ticket = t->gen;
    wake(&nos.work_wq);
    return 0;
}

static int nos_save(struct ramfs_volume *vol, struct vnode *v, struct content *c, uint64_t *ticket) {
    (void)vol;
    int r = content_size(c) <= CONTENT_TINY ? content_compact(c) : reads_tiny(c) ? content_own_all(c) : 0;
    return r ? r : add_save(v, c, 0, ticket);
}

static bool failed(uint64_t ticket, int *err) {
    for (unsigned i = 0; i < MIN(nos.nfails, (unsigned)FAILS); i++)
        if (nos.fails[i].gen == ticket) {
            *err = nos.fails[i].err;
            return true;
        }
    return false;
}

static int nos_saved(struct ramfs_volume *vol, uint64_t ticket, bool wait) {
    (void)vol;
    uint64_t fl = irq_save();
    int r;
    for (;;) {
        if (!failed(ticket, &r)) r = ticket <= nos.durable ? 0 : -N_ERR_NOT_READY;
        if (r != -N_ERR_NOT_READY || !wait) break;
        nos.waiters++;
        wq_wake_all(&nos.work_wq);
        wq_wait(&nos.done_wq);
        nos.waiters--;
    }
    irq_restore(fl);
    return r;
}

static int nos_sync(struct ramfs_volume *vol) {
    uint64_t ticket = pending(&nos.open) ? nos.open.gen : nos.committing ? nos.busy.gen : 0;
    return ticket ? nos_saved(vol, ticket, true) : 0;
}

static int nos_reserve(struct ramfs_volume *vol, struct vnode *v, uint64_t size, struct content **out,
                       uint64_t *ticket) {
    (void)vol;
    uint64_t pages = (size + BS - 1) / BS;
    int r = room(pages);
    if (r) return r;
    struct content *c = content_version(NULL, &store);
    if (!c) return -N_ERR_NO_MEMORY;
    content_load(c, size);
    content_freeze(c);
    nos.reserved += pages; /* fill turns them into blocks */
    if ((r = add_save(v, c, pages, ticket))) {
        nos.reserved -= pages;
        content_unref(c);
        return r;
    }
    *out = c;
    return 0;
}

struct ext_ctx {
    int (*run)(void *ctx, struct blkdev *d, uint64_t lba, uint64_t sectors);
    void *ctx;
};

static int ext_run(void *ctx, uint64_t page, uint64_t count, uint64_t block) {
    (void)page;
    struct ext_ctx *e = ctx;
    return e->run(e->ctx, nos.disk, nos.hd.first_lba + block * SECTORS, count * SECTORS);
}

static int nos_extents(struct ramfs_volume *vol, struct content *c,
                       int (*run)(void *ctx, struct blkdev *d, uint64_t lba, uint64_t sectors), void *ctx) {
    (void)vol;
    struct ext_ctx e = {run, ctx};
    return content_extents(c, ext_run, &e);
}

/* ---------------------------------------------------------------- finding and making the volume */

static void layout(struct head *h, uint64_t start, uint64_t sectors) {
    memset(h, 0, sizeof *h);
    memcpy(h->magic, "NOSfs", 5);
    h->version = VERSION;
    h->first_lba = start;
    h->blocks = ((sectors & ~7ull) - start) / SECTORS;
    h->maps = (h->blocks + MAP_BITS - 1) / MAP_BITS;
    h->sum_blocks = (h->maps + SUM_PER - 1) / SUM_PER;
    h->sum_at = 3;
    h->map_at = h->sum_at + 2 * h->sum_blocks;
    h->data_at = h->map_at + 2 * h->maps;
}

static bool head_valid(const uint8_t *blk, uint64_t start, uint64_t sectors) {
    const struct head *h = (const void *)blk;
    struct head want;
    if (!sealed(blk) || memcmp(h->magic, "NOSfs\0\0\0", 8) || h->version != VERSION || h->first_lba != start ||
        start + h->blocks * SECTORS > sectors)
        return false;
    layout(&want, start, start + h->blocks * SECTORS);
    return h->blocks >= MIN_BLOCKS && h->maps == want.maps && h->sum_blocks == want.sum_blocks &&
           h->sum_at == want.sum_at && h->map_at == want.map_at && h->data_at == want.data_at;
}

static int mount(struct blkdev *d, const uint8_t *head) {
    nos.disk = d;
    memcpy(&nos.hd, head, sizeof nos.hd);
    uint8_t *b = nos.bounce;
    struct anchor best;
    int slot = -1;
    for (int s = 0; s < 2; s++) {
        const struct anchor *a = (const void *)b;
        if (io(1 + s, 1, b, false) || !sealed(b) || memcmp(a->magic, "NOSa", 4) || a->volume_id != nos.hd.volume_id ||
            a->sum_slot > 1 || !a->height || a->height > MAX_HEIGHT || a->root < nos.hd.data_at || a->root >= nos.hd.blocks)
            continue;
        if (slot < 0 || a->gen > best.gen) {
            best = *a;
            slot = s;
        }
    }
    const char *why = "no anchor can be read";
    if (slot < 0) goto fail;
    why = "no memory";
    if (!(nos.maps = kzalloc(nos.hd.maps * sizeof *nos.maps))) goto fail;
    why = "the summary cannot be read";
    for (uint64_t k = 0; k < nos.hd.sum_blocks; k++) {
        const struct sum_head *h = (const void *)b;
        if (io(nos.hd.sum_at + best.sum_slot * nos.hd.sum_blocks + k, 1, b, false) || !sealed(b) ||
            memcmp(h->magic, "NOSs", 4) || h->volume_id != nos.hd.volume_id || h->gen != best.gen || h->index != k)
            goto fail;
        const uint16_t *e = (const uint16_t *)(b + sizeof *h);
        for (uint64_t j = 0, i = k * SUM_PER; j < SUM_PER && i < nos.hd.maps; j++, i++) {
            if ((e[j] & 0x7FFF) > MAP_BITS) goto fail;
            nos.maps[i].zeros = e[j] & 0x7FFF;
            nos.maps[i].slot = e[j] >> 15;
            nos.avail += nos.maps[i].zeros;
        }
    }
    why = "the index's root cannot be read";
    if (!(nos.root = node_read(best.root, best.height - 1))) goto fail;
    nos.height = best.height;
    nos.next_id = MAX(best.next_id, (uint64_t)ID_ROOT + 1);
    nos.durable = best.gen;
    nos.open.gen = best.gen + 1;
    nos.sum_slot = best.sum_slot;
    nos.anchor_slot = slot;
    nos.meta = MIN(MAX(1024, nos.hd.blocks / 100), nos.hd.blocks / 4);
    return 1;
fail:
    kprintf("nos: %s: the volume at sector %lu cannot be used: %s\n", d->name, nos.hd.first_lba, why);
    kfree(nos.maps);
    nos.maps = NULL;
    nos.avail = 0;
    return -N_ERR_IO;
}

/* A partition entry for the volume, where the table has room for one. */
static void mbr_add(struct blkdev *d, uint64_t start, uint64_t blocks) {
    uint8_t mbr[512];
    if (start > 0xFFFFFFFFull || blk_read(d, 0, 1, mbr) < 0) return;
    struct part *pt = (struct part *)(mbr + 446);
    int slot = -1;
    for (int p = 3; p >= 0; p--) {
        if (pt[p].type == TYPE_NOS) return;
        if (!pt[p].type) slot = p;
    }
    if (slot < 0) {
        kprintf("nos: %s: the partition table is full; the volume is found by its copy\n", d->name);
        return;
    }
    pt[slot] = (struct part){0, {0xFE, 0xFF, 0xFF}, TYPE_NOS, {0xFE, 0xFF, 0xFF}, (uint32_t)start,
                             (uint32_t)MIN(blocks * SECTORS, 0xFFFFFFFFull)};
    mbr[510] = 0x55;
    mbr[511] = 0xAA;
    if (blk_write(d, 0, 1, mbr) < 0 || blk_persist(d) < 0) kprintf("nos: %s: the partition entry cannot be written\n", d->name);
}

static bool overlaps(const struct part *p, uint64_t start, uint64_t end) {
    return p->type && p->type != TYPE_NOS && p->sectors && p->start < end && p->start + (uint64_t)p->sectors > start;
}

/* 1: d's volume is mounted; 0: d has none; an error: it has one that cannot be used. */
static int find(struct blkdev *d) {
    uint8_t mbr[512], *b = nos.bounce;
    if (blk_read(d, 0, 1, mbr) < 0) return 0;
    struct part *pt = (struct part *)(mbr + 446);
    bool table = mbr[510] == 0x55 && mbr[511] == 0xAA;
    for (int p = 0; table && p < 4; p++) {
        if (pt[p].type != TYPE_NOS || !pt[p].start) continue;
        if (blk_read(d, pt[p].start, SECTORS, b) < 0 || !head_valid(b, pt[p].start, d->sectors)) {
            kprintf("nos: %s: partition %d is not a volume that can be read\n", d->name, p + 1);
            return -N_ERR_IO;
        }
        return mount(d, b);
    }
    /* the copy at the end: the partition table was written over (a boot image put on the disk again) */
    uint64_t end = d->sectors & ~7ull;
    if (end < 2 * SECTORS || blk_read(d, end - SECTORS, SECTORS, b) < 0) return 0;
    const struct head *h = (const void *)b;
    uint64_t start = h->first_lba;
    if (!sealed(b) || memcmp(h->magic, "NOSfs\0\0\0", 8) || start >= end || !head_valid(b, start, d->sectors) ||
        start + h->blocks * SECTORS != end)
        return 0;
    for (int p = 0; table && p < 4; p++)
        if (overlaps(&pt[p], start, end)) {
            kprintf("nos: %s: partition %d lies over the volume at sector %lu\n", d->name, p + 1, start);
            return -N_ERR_IO;
        }
    if (blk_read(d, start, SECTORS, b) < 0 || !head_valid(b, start, d->sectors)) return -N_ERR_IO;
    int r = mount(d, b);
    if (r > 0) mbr_add(d, start, nos.hd.blocks);
    return r;
}

static void bits_set(uint64_t *bits, uint64_t from, uint64_t to) {
    for (uint64_t i = from; i < to; i++) bits[i / 64] |= 1ull << (i % 64);
}

/* A new volume at sector start: the empty index, every block free but the volume's own. */
static int format(struct blkdev *d, uint64_t start) {
    struct head *h = &nos.hd;
    uint8_t *b = nos.bounce;
    nos.disk = d;
    layout(h, start, d->sectors);
    do entropy_get(&h->volume_id, sizeof h->volume_id);
    while (!h->volume_id);
    int r = 0;
    /* maps, place 0 */
    uint16_t *zeros = kzalloc(h->maps * sizeof *zeros);
    if (!zeros) return -N_ERR_NO_MEMORY;
    for (uint64_t m = 0; m < h->maps && !r; m += BOUNCE) {
        uint64_t k = MIN((uint64_t)BOUNCE, h->maps - m);
        memset(b, 0, k * BS);
        for (uint64_t j = 0; j < k; j++) {
            uint8_t *blk = b + j * BS;
            struct map_head *mh = (struct map_head *)blk;
            uint64_t *bits = (uint64_t *)(blk + sizeof *mh), first = (m + j) * MAP_BITS, last = first + MAP_BITS;
            memcpy(mh->magic, "NOSm", 4);
            mh->index = m + j;
            mh->volume = (uint32_t)h->volume_id;
            if (first <= h->data_at) bits_set(bits, 0, MIN(h->data_at + 1, last) - first);
            if (last >= h->blocks) bits_set(bits, h->blocks - 1 - first, MAP_BITS);
            uint32_t z = 0;
            for (uint64_t w = 0; w < MAP_WORDS; w++) z += __builtin_popcountll(~bits[w]);
            zeros[m + j] = z;
            seal(blk);
        }
        r = io(h->map_at + m, k, b, true);
    }
    /* summary, place 0 */
    for (uint64_t k = 0; k < h->sum_blocks && !r; k++) {
        memset(b, 0, BS);
        struct sum_head *sh = (struct sum_head *)b;
        memcpy(sh->magic, "NOSs", 4);
        sh->volume_id = h->volume_id;
        sh->gen = 1;
        sh->index = k;
        uint16_t *e = (uint16_t *)(b + sizeof *sh);
        for (uint64_t j = 0, i = k * SUM_PER; j < SUM_PER && i < h->maps; j++, i++) e[j] = zeros[i];
        seal(b);
        r = io(h->sum_at + k, 1, b, true);
    }
    kfree(zeros);
    /* the empty root leaf */
    if (!r) {
        memset(b, 0, BS);
        struct node_head *nh = (struct node_head *)b;
        memcpy(nh->magic, "NOSn", 4);
        nh->top = BS;
        seal(b);
        r = io(h->data_at, 1, b, true);
    }
    /* anchors: the first commit in place 0, nothing in place 1 */
    if (!r) {
        memset(b, 0, 2 * BS);
        struct anchor *a = (struct anchor *)b;
        memcpy(a->magic, "NOSa", 4);
        a->volume_id = h->volume_id;
        a->gen = 1;
        a->root = h->data_at;
        a->next_id = ID_ROOT + 1;
        a->height = 1;
        seal(a);
        r = io(1, 2, b, true);
    }
    /* the head and its copy */
    if (!r) {
        memset(b, 0, BS);
        memcpy(b, h, sizeof *h);
        seal(b);
        memcpy(h, b, sizeof *h);
        r = io(0, 1, b, true);
        if (!r) r = io(h->blocks - 1, 1, b, true);
    }
    if (!r) r = blk_persist(d);
    if (r) kprintf("nos: %s: the volume cannot be made (%d)\n", d->name, r);
    return r < 0 ? r : 0;
}

/* Only Nocturne's own disks get a volume: its boot medium (past the boot partition, with room for
   a larger image) or an empty data disk. 1: made and mounted. */
static int create(struct blkdev *d) {
    uint8_t mbr[512];
    if (blk_read(d, 0, 1, mbr) < 0) return 0;
    uint32_t sig = *(uint32_t *)(mbr + 440);
    struct part *pt = (struct part *)(mbr + 446);
    bool table = mbr[510] == 0x55 && mbr[511] == 0xAA;
    uint64_t end = 0, start;
    for (int p = 0; table && p < 4; p++)
        if (pt[p].type) end = MAX(end, (uint64_t)pt[p].start + pt[p].sectors);
    if (sig == SIG_NOCT && end) start = ALIGN_UP(MAX(2 * end, end + GAP_SECTORS), 2048);
    else if (sig == SIG_DATA && !end) start = 2048;
    else return 0;
    uint64_t last = d->sectors & ~7ull;
    if (start >= last || (last - start) / SECTORS < MIN_BLOCKS) {
        kprintf("nos: %s: no room for a volume past sector %lu\n", d->name, end);
        return 0;
    }
    int r = format(d, start);
    if (r) return r;
    mbr_add(d, start, nos.hd.blocks);
    uint8_t *b = nos.bounce;
    if ((r = io(0, 1, b, false))) return r;
    return mount(d, b);
}

bool nos_attach(void) {
    nos.vol = (struct ramfs_volume){&store,    nos_load,  nos_content, nos_changed, nos_hide,
                                    nos_save,  nos_saved, nos_sync,    nos_reserve, nos_extents};
    nos.bounce = kmalloc(BOUNCE * BS);
    int found = 0;
    bool made = false;
    for (int i = 0; nos.bounce && i < blk_count() && !found; i++) found = find(blk_get(i));
    for (int i = 0; nos.bounce && i < blk_count() && !found; i++) made = (found = create(blk_get(i))) > 0;
    if (found <= 0) {
        kfree(nos.bounce);
        nos.bounce = NULL;
        ramfs_attach(NULL, 0);
        kprintf("nos: no volume: / is not persistent\n");
        return false;
    }
    ramfs_attach(&nos.vol, ID_ROOT);
    nos.worker = kthread_create("nos", worker, NULL);
    kprintf("nos: %s: %lu MiB from sector %lu, %lu MiB free, commit %lu%s\n", nos.disk->name,
            nos.hd.blocks * BS >> 20, nos.hd.first_lba, nos.avail * BS >> 20, nos.durable, made ? " (made now)" : "");
    if (made) {
        static const char *const folders[] = {"/data", "/data/bin", "/data/etc", "/data/src", "/data/projects"};
        for (size_t i = 0; i < ARRAY_SIZE(folders); i++) vfs_mkdir(folders[i]);
    }
    return true;
}
