/* C:'s blocks in memory (fs/cache.h).

   One spinlock guards the hash, the list of blocks nothing holds and each block's count and state,
   held for a few steps at a time: blocks are read, allocated and freed with it let go. A block
   being read is in the hash as LOADING from before its read until after it, so that another reader
   of it waits instead of reading it again. A read that fails takes the block out of the hash, and
   whoever waited for it reads it once more itself: what it expects there may differ (it found the
   address in a newer catalog). */
#include "fs/cache.h"
#include "kernel.h"
#include "mm/heap.h"
#include "sys/sched.h"

#define STAGE_BLOCKS 64u   /* blocks read in one request */
#define FIRST_BUCKETS 1024u
#define STEP 256u          /* addresses or buckets forgotten before the lock is let go a moment */

enum { LOADING, READY, FAILED };

struct cblock {
    struct store_block b; /* the store's: first */
    uint32_t refs;
    uint8_t state;
    bool hashed;          /* until it is forgotten, discarded or its read fails: then it goes with its last put */
    struct cblock *hash_next, *older, *newer; /* the hash chain; the list of READY blocks nothing holds */
};

struct cache {
    struct store_supply supply; /* first */
    struct store_device *dev;
    struct spinlock lock;       /* everything below, and each block's refs, state, hashed and links */
    struct wait_queue loaded;   /* readers of LOADING blocks */
    struct cblock **hash;
    size_t hash_size, hashed, unheld, limit;
    struct cblock *oldest, *newest;
};

static struct cache *cache_of(struct store_supply *s) { return (struct cache *)s; }
static struct cblock *block_of(struct store_block *b) { return (struct cblock *)b; }

static size_t hash_of(struct cache *c, uint64_t address) {
    return (size_t)((address * 0x9E3779B97F4A7C15ULL) >> 32) & (c->hash_size - 1);
}

static struct cblock *lookup(struct cache *c, uint64_t address) {
    for (struct cblock *b = c->hash[hash_of(c, address)]; b; b = b->hash_next)
        if (b->b.address == address) return b;
    return NULL;
}

static void hash_in(struct cache *c, struct cblock *b) {
    struct cblock **at = &c->hash[hash_of(c, b->b.address)];
    b->hash_next = *at;
    *at = b;
    b->hashed = true;
    c->hashed++;
}

static void hash_out(struct cache *c, struct cblock *b) {
    struct cblock **at = &c->hash[hash_of(c, b->b.address)];
    while (*at != b) at = &(*at)->hash_next;
    *at = b->hash_next;
    b->hash_next = NULL;
    b->hashed = false;
    c->hashed--;
}

static void unheld_out(struct cache *c, struct cblock *b) {
    if (b->older) b->older->newer = b->newer;
    else c->oldest = b->newer;
    if (b->newer) b->newer->older = b->older;
    else c->newest = b->older;
    b->older = b->newer = NULL;
    c->unheld--;
}

static void hold(struct cache *c, struct cblock *b) {
    if (!b->refs++ && b->state == READY) unheld_out(c, b);
}

/* Blocks that go are chained here under the lock, and freed once it is let go. */
static void gone_add(struct cblock **gone, struct cblock *b) {
    b->hash_next = *gone;
    *gone = b;
}

static void block_free(struct cblock *b) {
    kfree(b->b.data);
    kfree(b);
}

static void gone_free(struct cblock *gone) {
    for (struct cblock *next; gone; gone = next) {
        next = gone->hash_next;
        block_free(gone);
    }
}

/* b out of the hash: it goes now when nothing holds it, else with its last put */
static void forget_one(struct cache *c, struct cblock *b, struct cblock **gone) {
    hash_out(c, b);
    if (b->refs) return;
    unheld_out(c, b);
    gone_add(gone, b);
}

/* one hold less; the lock held */
static void let_go(struct cache *c, struct cblock *b, struct cblock **gone) {
    if (--b->refs) return;
    if (!b->hashed) {
        gone_add(gone, b);
        return;
    }
    b->older = c->newest;
    b->newer = NULL;
    if (c->newest) c->newest->newer = b;
    else c->oldest = b;
    c->newest = b;
    c->unheld++;
    while (c->unheld > c->limit) {
        struct cblock *old = c->oldest;
        unheld_out(c, old);
        hash_out(c, old);
        gone_add(gone, old);
    }
}

/* data: STORE_BLOCK bytes the block takes (NULL: zeros of its own) */
static struct cblock *block_new(uint64_t address, uint8_t *data, uint8_t state) {
    struct cblock *b = kzalloc(sizeof *b);
    if (!b) return NULL;
    if (!data && !(data = kzalloc(STORE_BLOCK))) {
        kfree(b);
        return NULL;
    }
    b->b.address = address;
    b->b.data = data;
    b->refs = 1;
    b->state = state;
    return b;
}

/* Twice the buckets once there are twice as many blocks: the rehash runs under the lock, the
   allocation does not. Without memory, the chains grow longer. */
static void grow(struct cache *c) {
    uint64_t flags = spin_lock(&c->lock);
    size_t size = c->hash_size;
    bool want = c->hashed > 2 * size;
    spin_unlock(&c->lock, flags);
    if (!want) return;
    struct cblock **hash = kzalloc(2 * size * sizeof *hash), **old = NULL;
    if (!hash) return;
    flags = spin_lock(&c->lock);
    if (c->hash_size == size) {
        old = c->hash;
        c->hash = hash;
        c->hash_size = 2 * size;
        for (size_t i = 0; i < size; i++)
            for (struct cblock *b = old[i], *next; b; b = next) {
                next = b->hash_next;
                struct cblock **at = &hash[hash_of(c, b->b.address)];
                b->hash_next = *at;
                *at = b;
            }
        hash = NULL;
    }
    spin_unlock(&c->lock, flags);
    kfree(hash);
    kfree(old);
}

static int device_read(struct cache *c, uint64_t block, uint32_t count, void *buf) {
    if (block + count > c->dev->blocks || block + count < block) return -N_ERR_IO;
    return c->dev->read(c->dev, block, count, buf);
}

/* b's read is over: its readers go on */
static void loaded(struct cache *c, struct cblock *b, bool ok) {
    uint64_t flags = spin_lock(&c->lock);
    b->state = ok ? READY : FAILED;
    if (!ok && b->hashed) hash_out(c, b);
    spin_unlock(&c->lock, flags);
    wq_wake_all(&c->loaded);
}

/* until b is read; the lock held, interrupts off throughout */
static void wait_loaded(struct cache *c, struct cblock *b) {
    while (b->state == LOADING) {
        wq_wait_locked(&c->loaded, &c->lock);
        spin_take(&c->lock);
    }
}

static struct store_block *cache_find(struct store_supply *s, uint64_t address) {
    struct cache *c = cache_of(s);
    uint64_t flags = spin_lock(&c->lock);
    struct cblock *b = lookup(c, address);
    if (b && b->state == READY) hold(c, b);
    else b = NULL;
    spin_unlock(&c->lock, flags);
    return b ? &b->b : NULL;
}

static void cache_put(struct store_supply *s, struct store_block *sb) {
    struct cache *c = cache_of(s);
    struct cblock *gone = NULL;
    uint64_t flags = spin_lock(&c->lock);
    let_go(c, block_of(sb), &gone);
    spin_unlock(&c->lock, flags);
    gone_free(gone);
}

static struct store_block *cache_read(struct store_supply *s, struct volume *v, uint64_t address, store_block_check *valid,
                                      uint64_t expect, int *error) {
    struct cache *c = cache_of(s);
    struct cblock *fresh = NULL;
    for (unsigned failed = 0;;) {
        struct cblock *gone = NULL;
        uint64_t flags = spin_lock(&c->lock);
        struct cblock *b = lookup(c, address);
        if (!b && fresh) {
            hash_in(c, fresh);
            spin_unlock(&c->lock, flags);
            break;
        }
        if (b) {
            hold(c, b);
            wait_loaded(c, b);
            bool ready = b->state == READY;
            if (!ready) let_go(c, b, &gone);
            spin_unlock(&c->lock, flags);
            gone_free(gone);
            if (ready) {
                if (fresh) block_free(fresh);
                return &b->b;
            }
            if (++failed == 2) { /* another's read failed twice: so does this one */
                if (fresh) block_free(fresh);
                *error = -N_ERR_IO;
                return NULL;
            }
            continue;
        }
        spin_unlock(&c->lock, flags);
        if (!(fresh = block_new(address, NULL, LOADING))) {
            *error = -N_ERR_NO_MEMORY;
            return NULL;
        }
    }
    grow(c);
    int r = device_read(c, address, 1, fresh->b.data);
    bool ok = r >= 0 && (!valid || valid(v, fresh->b.data, address, expect));
    loaded(c, fresh, ok);
    if (!ok) {
        cache_put(s, &fresh->b);
        *error = r < 0 ? r : -N_ERR_IO;
        return NULL;
    }
    return &fresh->b;
}

static int cache_read_many(struct store_supply *s, struct volume *v, uint64_t start, uint32_t count,
                           store_block_check *valid, const uint32_t *expect, struct store_block **out) {
    struct cache *c = cache_of(s);
    int error = 0;
    uint8_t *stage = NULL;
    for (uint32_t k = 0; k < count; k++) out[k] = NULL;
    for (uint32_t i = 0; i < count && !error;) {
        uint64_t flags = spin_lock(&c->lock);
        struct cblock *b = lookup(c, start + i);
        if (b) { /* there, or being read: settled below */
            hold(c, b);
            spin_unlock(&c->lock, flags);
            out[i++] = &b->b;
            continue;
        }
        spin_unlock(&c->lock, flags);
        /* LOADING blocks for the ones from i on that memory does not have, then one read */
        uint32_t n = 0;
        while (i + n < count && n < STAGE_BLOCKS) {
            struct cblock *fresh = block_new(start + i + n, NULL, LOADING);
            if (!fresh) {
                error = -N_ERR_NO_MEMORY;
                break;
            }
            flags = spin_lock(&c->lock);
            bool there = lookup(c, start + i + n) != NULL;
            if (!there) hash_in(c, fresh);
            spin_unlock(&c->lock, flags);
            if (there) {
                block_free(fresh);
                break;
            }
            out[i + n++] = &fresh->b;
        }
        if (!n) continue;
        grow(c);
        if (!stage && !(stage = kmalloc(STAGE_BLOCKS * STORE_BLOCK))) error = -N_ERR_NO_MEMORY;
        int r = error ? error : device_read(c, start + i, n, stage);
        for (uint32_t k = 0; k < n; k++) {
            struct cblock *fresh = block_of(out[i + k]);
            bool ok = r >= 0;
            if (ok) {
                memcpy(fresh->b.data, stage + (size_t)k * STORE_BLOCK, STORE_BLOCK);
                ok = !valid || valid(v, fresh->b.data, fresh->b.address, expect ? expect[i + k] : 0);
            }
            loaded(c, fresh, ok);
            if (!ok && !error) error = r < 0 ? r : -N_ERR_IO;
        }
        i += n;
    }
    kfree(stage);
    /* the blocks others were reading, once they are there; one whose read failed, read again here */
    for (uint32_t k = 0; k < count && !error; k++) {
        struct cblock *b = block_of(out[k]);
        uint64_t flags = spin_lock(&c->lock);
        wait_loaded(c, b);
        bool failed = b->state == FAILED;
        spin_unlock(&c->lock, flags);
        if (!failed) continue;
        cache_put(s, out[k]);
        out[k] = cache_read(s, v, start + k, valid, expect ? expect[k] : 0, &error);
    }
    if (error)
        for (uint32_t k = 0; k < count; k++) {
            if (out[k]) cache_put(s, out[k]);
            out[k] = NULL;
        }
    return error;
}

static struct store_block *cache_make(struct store_supply *s, uint64_t address, uint8_t *data, int *error) {
    struct cache *c = cache_of(s);
    struct cblock *b = block_new(address, data, READY), *gone = NULL;
    if (!b) {
        *error = -N_ERR_NO_MEMORY;
        return NULL;
    }
    uint64_t flags = spin_lock(&c->lock);
    struct cblock *old = lookup(c, address);
    if (old) forget_one(c, old, &gone);
    hash_in(c, b);
    spin_unlock(&c->lock, flags);
    gone_free(gone);
    grow(c);
    return &b->b;
}

static void cache_rename(struct store_supply *s, struct store_block *sb, uint64_t address) {
    struct cache *c = cache_of(s);
    struct cblock *b = block_of(sb), *gone = NULL;
    uint64_t flags = spin_lock(&c->lock);
    struct cblock *old = lookup(c, address);
    if (old && old != b) forget_one(c, old, &gone);
    bool hashed = b->hashed;
    if (hashed) hash_out(c, b);
    b->b.address = address;
    if (hashed) hash_in(c, b);
    spin_unlock(&c->lock, flags);
    gone_free(gone);
}

static void cache_discard(struct store_supply *s, struct store_block *sb) {
    struct cache *c = cache_of(s);
    struct cblock *b = block_of(sb);
    uint64_t flags = spin_lock(&c->lock);
    if (b->hashed) hash_out(c, b);
    spin_unlock(&c->lock, flags);
}

/* A few steps at a time, the lock let go between them. A block a concurrent rehash moved past the
   look stays in memory: nothing reads it, and new content at its address forgets it (make, rename). */
static void cache_forget(struct store_supply *s, uint64_t start, uint64_t count) {
    struct cache *c = cache_of(s);
    uint64_t flags = spin_lock(&c->lock);
    bool each = count > c->hashed; /* fewer blocks in memory than in the range: look at each of them */
    spin_unlock(&c->lock, flags);
    for (uint64_t at = 0;; at += STEP) {
        struct cblock *gone = NULL;
        flags = spin_lock(&c->lock);
        bool done = each ? at >= c->hash_size : at >= count;
        for (uint64_t i = at; !done && i < at + STEP; i++) {
            if (each) {
                if (i >= c->hash_size) break;
                for (struct cblock *b = c->hash[i], *next; b; b = next) {
                    next = b->hash_next;
                    if (b->b.address >= start && b->b.address - start < count) forget_one(c, b, &gone);
                }
            } else {
                if (i >= count) break;
                struct cblock *b = lookup(c, start + i);
                if (b) forget_one(c, b, &gone);
            }
        }
        spin_unlock(&c->lock, flags);
        gone_free(gone);
        if (done) return;
    }
}

struct store_supply *cache_create(struct store_device *dev, size_t limit) {
    struct cache *c = kzalloc(sizeof *c);
    if (!c || !(c->hash = kzalloc(FIRST_BUCKETS * sizeof *c->hash))) {
        kfree(c);
        return NULL;
    }
    c->supply = (struct store_supply){
        .find = cache_find,
        .read = cache_read,
        .read_many = cache_read_many,
        .make = cache_make,
        .put = cache_put,
        .rename = cache_rename,
        .discard = cache_discard,
        .forget = cache_forget,
    };
    c->dev = dev;
    c->hash_size = FIRST_BUCKETS;
    c->limit = limit;
    return &c->supply;
}
