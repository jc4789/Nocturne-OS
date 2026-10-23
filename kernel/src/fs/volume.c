/* A mounted store volume: its cache, mount and commits (fs/volume.h). */
#include "fs/volume.h"
#include "arch/cpu.h"
#include "fs/catalog.h"
#include "fs/space.h"
#include "lib/crc32c.h"
#include "mm/heap.h"

#define STAGE_BLOCKS 64u

int ranges_add(struct ranges *list, uint64_t start, uint64_t count) {
    if (!count) return 0;
    if (list->n && list->r[list->n - 1].start + list->r[list->n - 1].count == start) {
        list->r[list->n - 1].count += count;
        return 0;
    }
    struct range *r = kappend(&list->r, &list->n, &list->room, sizeof *r);
    if (!r) return -N_ERR_NO_MEMORY;
    *r = (struct range){start, count};
    return 0;
}

void ranges_free(struct ranges *list) {
    kfree(list->r);
    *list = (struct ranges){0};
}

uint32_t store_seed(const uint8_t volume_id[16]) { return crc32c(0, volume_id, 16); }

void head_seal(struct volume *v, void *block, uint32_t magic, uint64_t address, uint64_t generation) {
    struct store_head *h = block;
    h->magic = magic;
    h->check = 0;
    h->address = address;
    h->generation = generation;
    h->check = crc32c(v->seed, block, STORE_BLOCK);
}

bool head_valid(struct volume *v, const void *block, uint32_t magic, uint64_t address, uint64_t generation) {
    const struct store_head *h = block;
    if (h->magic != magic || h->address != address || h->generation != generation) return false;
    struct store_head copy = *h;
    copy.check = 0;
    uint32_t c = crc32c(v->seed, &copy, sizeof copy);
    return crc32c(c, (const uint8_t *)block + sizeof copy, STORE_BLOCK - sizeof copy) == h->check;
}

bool content_valid(struct volume *v, const uint8_t *data, uint64_t address, uint64_t check) {
    return crc32c(v->seed, data, STORE_BLOCK) == (uint32_t)check;
}

int volume_writable(struct volume *v) { return v->error ? -N_ERR_READ_ONLY : 0; }

/* ---- the cache (interrupts off around its lists: readers come without v->lock) ---- */

static size_t hash_of(struct volume *v, uint64_t address) {
    return (size_t)((address * 0x9E3779B97F4A7C15ULL) >> 32) & (v->hash_size - 1);
}

static struct cblock *lookup(struct volume *v, uint64_t address) {
    for (struct cblock *b = v->hash[hash_of(v, address)]; b; b = b->hash_next)
        if (b->address == address) return b;
    return NULL;
}

static void unhash(struct volume *v, struct cblock *b) {
    for (struct cblock **p = &v->hash[hash_of(v, b->address)]; *p; p = &(*p)->hash_next)
        if (*p == b) {
            *p = b->hash_next;
            b->hash_next = NULL;
            return;
        }
}

static void grow_hash(struct volume *v) {
    size_t size = v->hash_size * 2;
    struct cblock **hash = kzalloc(size * sizeof *hash);
    if (!hash) return; /* longer chains, nothing worse */
    struct cblock **old = v->hash;
    size_t old_size = v->hash_size;
    v->hash = hash;
    v->hash_size = size;
    for (size_t i = 0; i < old_size; i++)
        for (struct cblock *b = old[i], *next; b; b = next) {
            next = b->hash_next;
            size_t h = hash_of(v, b->address);
            b->hash_next = hash[h];
            hash[h] = b;
        }
    kfree(old);
}

static void insert(struct volume *v, struct cblock *b) {
    size_t h = hash_of(v, b->address);
    b->hash_next = v->hash[h];
    v->hash[h] = b;
    if (++v->blocks > 2 * v->hash_size) grow_hash(v);
}

static void lru_remove(struct volume *v, struct cblock *b) {
    if (b->older) b->older->newer = b->newer;
    else v->oldest = b->newer;
    if (b->newer) b->newer->older = b->older;
    else v->newest = b->older;
    b->older = b->newer = NULL;
    v->unused--;
}

static void discard(struct volume *v, struct cblock *b) {
    unhash(v, b);
    v->blocks--;
    kfree(b->data);
    kfree(b);
}

/* a block taken out of the hash while someone held it: freed with its last reference */
#define ORPHAN UINT64_MAX

static void hold(struct volume *v, struct cblock *b) {
    if (!b->refs++ && b->state == BLOCK_CLEAN) lru_remove(v, b);
}

void cache_put(struct volume *v, struct cblock *b) {
    if (!b) return;
    uint64_t flags = irq_save();
    if (--b->refs) {
        irq_restore(flags);
        return;
    }
    if (b->address == ORPHAN || b->state == BLOCK_FAILED) {
        if (b->address == ORPHAN) {
            v->blocks--;
            kfree(b->data);
            kfree(b);
        } else {
            discard(v, b);
        }
        irq_restore(flags);
        return;
    }
    if (b->state == BLOCK_CLEAN) {
        b->older = v->newest;
        b->newer = NULL;
        if (v->newest) v->newest->newer = b;
        else v->oldest = b;
        v->newest = b;
        v->unused++;
        while (v->unused > v->limit && v->oldest) {
            struct cblock *old = v->oldest;
            lru_remove(v, old);
            discard(v, old);
        }
    }
    irq_restore(flags);
}

static struct cblock *block_alloc(uint64_t address, uint8_t state) {
    struct cblock *b = kzalloc(sizeof *b);
    if (b && !(b->data = kmalloc(STORE_BLOCK))) {
        kfree(b);
        return NULL;
    }
    if (b) {
        b->address = address;
        b->state = state;
        b->refs = 1;
    }
    return b;
}

struct cblock *cache_find(struct volume *v, uint64_t address) {
    uint64_t flags = irq_save();
    struct cblock *b = lookup(v, address);
    if (b) hold(v, b);
    while (b && b->state == BLOCK_LOADING) wq_wait(&v->loaded);
    irq_restore(flags);
    if (b && b->state == BLOCK_FAILED) {
        cache_put(v, b);
        return NULL;
    }
    return b;
}

static int device_read(struct volume *v, uint64_t block, uint32_t count, void *buf) {
    if (block + count > v->dev->blocks || block + count < block) return -N_ERR_IO;
    return v->dev->read(v->dev, block, count, buf);
}

static void loaded(struct volume *v, struct cblock *b, bool ok) {
    uint64_t flags = irq_save();
    b->state = ok ? BLOCK_CLEAN : BLOCK_FAILED;
    if (!ok) unhash(v, b), b->address = ORPHAN; /* the next reader tries again */
    wq_wake_all(&v->loaded);
    irq_restore(flags);
}

struct cblock *cache_read(struct volume *v, uint64_t address, block_check *valid, uint64_t expect, int *error) {
    struct cblock *b = cache_find(v, address);
    if (b) return b;
    struct cblock *fresh = block_alloc(address, BLOCK_LOADING);
    if (!fresh) {
        *error = -N_ERR_NO_MEMORY;
        return NULL;
    }
    uint64_t flags = irq_save();
    if ((b = lookup(v, address))) { /* another reader came first */
        irq_restore(flags);
        kfree(fresh->data);
        kfree(fresh);
        if ((b = cache_find(v, address))) return b;
        *error = -N_ERR_IO;
        return NULL;
    }
    insert(v, fresh);
    irq_restore(flags);
    int r = device_read(v, address, 1, fresh->data);
    bool ok = r >= 0 && (!valid || valid(v, fresh->data, address, expect));
    if (r >= 0 && !ok) kprintf("store: block %lu does not hold what the catalog says\n", (unsigned long)address);
    loaded(v, fresh, ok);
    if (!ok) {
        cache_put(v, fresh);
        *error = r < 0 ? r : -N_ERR_IO;
        return NULL;
    }
    return fresh;
}

int cache_read_many(struct volume *v, uint64_t start, uint32_t count, block_check *valid, const uint32_t *expect,
                    struct cblock **out) {
    int error = 0;
    uint8_t *stage = NULL;
    uint32_t i = 0;
    for (uint32_t k = 0; k < count; k++) out[k] = NULL;
    while (i < count && !error) {
        /* the blocks from i on that are not in memory: placeholders for them, then one read */
        uint64_t flags = irq_save();
        struct cblock *b = lookup(v, start + i);
        if (b) {
            hold(v, b);
            out[i++] = b;
            irq_restore(flags);
            continue;
        }
        irq_restore(flags);
        uint32_t n = 0;
        while (i + n < count && n < STAGE_BLOCKS) {
            struct cblock *fresh = block_alloc(start + i + n, BLOCK_LOADING);
            if (!fresh) {
                error = -N_ERR_NO_MEMORY;
                break;
            }
            flags = irq_save();
            if (lookup(v, start + i + n)) {
                irq_restore(flags);
                kfree(fresh->data);
                kfree(fresh);
                break;
            }
            insert(v, fresh);
            irq_restore(flags);
            out[i + n++] = fresh;
        }
        if (!n) continue;
        if (!stage && !(stage = kmalloc(STAGE_BLOCKS * STORE_BLOCK))) error = -N_ERR_NO_MEMORY;
        int r = error ? error : device_read(v, start + i, n, stage);
        for (uint32_t k = 0; k < n; k++) {
            struct cblock *fresh = out[i + k];
            bool ok = r >= 0;
            if (ok) {
                memcpy(fresh->data, stage + (size_t)k * STORE_BLOCK, STORE_BLOCK);
                ok = !valid || valid(v, fresh->data, fresh->address, expect ? expect[i + k] : 0);
                if (!ok) kprintf("store: block %lu does not hold what the catalog says\n", (unsigned long)fresh->address);
            }
            loaded(v, fresh, ok);
            if (!ok && !error) error = r < 0 ? r : -N_ERR_IO;
        }
        i += n;
    }
    kfree(stage);
    /* blocks others were reading: wait for them */
    uint64_t flags = irq_save();
    for (uint32_t k = 0; k < count && !error; k++) {
        while (out[k] && out[k]->state == BLOCK_LOADING) wq_wait(&v->loaded);
        if (out[k] && out[k]->state == BLOCK_FAILED) error = -N_ERR_IO;
    }
    irq_restore(flags);
    if (error)
        for (uint32_t k = 0; k < count; k++) {
            cache_put(v, out[k]);
            out[k] = NULL;
        }
    return error;
}

static void forget_one(struct volume *v, struct cblock *b) {
    if (b->refs) { /* still being read: it goes with its last reference */
        unhash(v, b);
        b->address = ORPHAN;
        return;
    }
    if (b->state == BLOCK_CLEAN) lru_remove(v, b);
    discard(v, b);
}

struct cblock *cache_new(struct volume *v, uint64_t address, int *error) {
    uint8_t *data = kzalloc(STORE_BLOCK);
    struct cblock *b = data ? cache_take(v, address, data, error) : NULL;
    if (!b) {
        kfree(data);
        *error = -N_ERR_NO_MEMORY;
    }
    return b;
}

struct cblock *cache_take(struct volume *v, uint64_t address, uint8_t *data, int *error) {
    struct cblock *b = kzalloc(sizeof *b);
    if (!b) {
        *error = -N_ERR_NO_MEMORY;
        return NULL;
    }
    b->address = address;
    b->data = data;
    b->state = BLOCK_DIRTY;
    b->refs = 1;
    uint64_t flags = irq_save();
    struct cblock *old = lookup(v, address);
    if (old) forget_one(v, old);
    insert(v, b);
    irq_restore(flags);
    return b;
}

void cache_rename(struct volume *v, struct cblock *b, uint64_t address) {
    uint64_t flags = irq_save();
    struct cblock *old = lookup(v, address);
    if (old && old != b) forget_one(v, old);
    unhash(v, b);
    b->address = address;
    size_t h = hash_of(v, address);
    b->hash_next = v->hash[h];
    v->hash[h] = b;
    irq_restore(flags);
}

void cache_forget(struct volume *v, uint64_t start, uint64_t count) {
    uint64_t flags = irq_save();
    if (count > v->blocks) { /* fewer blocks in memory than in the range: look at each of them */
        for (size_t i = 0; i < v->hash_size; i++)
            for (struct cblock *b = v->hash[i], *next; b; b = next) {
                next = b->hash_next;
                if (b->address >= start && b->address - start < count && b->state == BLOCK_CLEAN) forget_one(v, b);
            }
    } else {
        for (uint64_t a = start; a < start + count; a++) {
            struct cblock *b = lookup(v, a);
            if (b && b->state == BLOCK_CLEAN) forget_one(v, b);
        }
    }
    irq_restore(flags);
}

void cache_discard(struct volume *v, struct cblock *b) {
    uint64_t flags = irq_save();
    unhash(v, b);
    if (--b->refs) {
        b->address = ORPHAN;
    } else {
        v->blocks--;
        kfree(b->data);
        kfree(b);
    }
    irq_restore(flags);
}

void cache_unwritten(struct volume *v, struct cblock *b) {
    uint64_t flags = irq_save();
    b->state = BLOCK_UNWRITTEN;
    b->next_unwritten = v->unwritten;
    v->unwritten = b;
    v->unwritten_count++;
    irq_restore(flags);
}

/* ---- commits ---- */

static int device_write(struct volume *v, uint64_t block, uint32_t count, const void *buf) {
    if (block + count > v->dev->blocks || block + count < block) return -N_ERR_IO;
    return v->dev->write(v->dev, block, count, buf);
}

static int device_persist(struct volume *v) { return v->dev->persist ? v->dev->persist(v->dev) : 0; }

static void sort_blocks(struct cblock **a, size_t n) {
    /* shell sort by address: the lists are a commit's worth of blocks */
    for (size_t gap = n / 2; gap; gap /= 2)
        for (size_t i = gap; i < n; i++) {
            struct cblock *b = a[i];
            size_t j = i;
            for (; j >= gap && a[j - gap]->address > b->address; j -= gap) a[j] = a[j - gap];
            a[j] = b;
        }
}

/* every block of the list, in runs of neighbours */
static int write_blocks(struct volume *v, struct cblock **list, size_t n, uint8_t *stage) {
    for (size_t i = 0; i < n;) {
        size_t k = 1;
        while (i + k < n && k < STAGE_BLOCKS && list[i + k]->address == list[i]->address + k) k++;
        for (size_t j = 0; j < k; j++) memcpy(stage + j * STORE_BLOCK, list[i + j]->data, STORE_BLOCK);
        int r = device_write(v, list[i]->address, (uint32_t)k, stage);
        if (r < 0) return r;
        i += k;
    }
    return 0;
}

static void super_seal(struct store_super *s, uint64_t address, uint64_t generation, uint8_t *block) {
    memset(block, 0, STORE_BLOCK);
    s->head.magic = STORE_MAGIC_SUPER;
    s->head.check = 0;
    s->head.address = address;
    s->head.generation = generation;
    memcpy(block, s, sizeof *s);
    s->head.check = crc32c(0, block, STORE_BLOCK);
    ((struct store_super *)block)->head.check = s->head.check;
}

/* waiting for a commit under way, with v->lock let go meanwhile */
static void wait_commit(struct volume *v) {
    while (v->committing) {
        uint64_t flags = irq_save();
        mutex_unlock(&v->lock);
        wq_wait(&v->committed);
        irq_restore(flags);
        mutex_lock(&v->lock);
    }
}

int volume_commit(struct volume *v) {
    mutex_lock(&v->lock);
    wait_commit(v);
    if (v->error || !v->changed) {
        int r = v->error;
        mutex_unlock(&v->lock);
        return r;
    }
    uint64_t generation = v->generation;
    struct space_write *maps = NULL;
    size_t nmaps = 0;
    int r = catalog_freeze(v, generation);
    if (r >= 0) r = space_freeze(v, generation, &maps, &nmaps);
    if (r < 0) {
        /* the reservations leave room for every commit: this one cannot be made whole */
        kprintf("store: a commit could not be prepared (%d): C: is read only from now on\n", r);
        v->error = -N_ERR_READ_ONLY;
        mutex_unlock(&v->lock);
        return v->error;
    }
    struct store_super super = v->super;
    super.catalog = v->root;
    super.catalog_generation = v->root_generation;
    super.next_id = v->next_id;
    super.used = v->space.used;
    uint64_t flags = irq_save();
    struct cblock *unwritten = v->unwritten;
    v->unwritten = NULL;
    v->unwritten_count = 0;
    irq_restore(flags);
    v->generation++;
    v->changed = false;
    v->committing = true;
    mutex_unlock(&v->lock);

    size_t n = 0;
    for (struct cblock *b = unwritten; b; b = b->next_unwritten) n++;
    struct cblock **list = kmalloc((n + 1) * sizeof *list);
    uint8_t *stage = kmalloc(STAGE_BLOCKS * STORE_BLOCK);
    r = list && stage ? 0 : -N_ERR_NO_MEMORY;
    if (!r) {
        n = 0;
        for (struct cblock *b = unwritten; b; b = b->next_unwritten) list[n++] = b;
        sort_blocks(list, n);
        r = write_blocks(v, list, n, stage);
    }
    for (size_t i = 0; i < nmaps && r >= 0; i++) r = device_write(v, maps[i].address, 1, maps[i].data);
    if (r >= 0) r = device_persist(v);
    if (r >= 0) {
        uint64_t address = generation % 2 ? v->super.blocks - 1 : 0;
        super_seal(&super, address, generation, stage);
        r = device_write(v, address, 1, stage);
    }
    if (r >= 0) r = device_persist(v);
    kfree(stage);
    for (size_t i = 0; i < nmaps; i++) kfree(maps[i].data);
    kfree(maps);

    mutex_lock(&v->lock);
    if (r < 0) {
        kprintf("store: a commit could not be written (%d): C: is read only from now on\n", r);
        v->error = -N_ERR_READ_ONLY;
    } else {
        v->super = super;
        v->durable = generation;
        space_committed(v, generation);
    }
    /* the blocks are as on the medium now (after a failure, memory keeps what reads still find) */
    for (struct cblock *b = unwritten, *next; b; b = next) {
        next = b->next_unwritten;
        flags = irq_save();
        b->state = BLOCK_CLEAN;
        irq_restore(flags);
        cache_put(v, b);
    }
    kfree(list);
    v->committing = false;
    wq_wake_all(&v->committed);
    mutex_unlock(&v->lock);
    return r < 0 ? v->error : 0;
}

/* ---- mounting ---- */

static struct volume *volume_new(struct store_device *dev) {
    struct volume *v = kzalloc(sizeof *v);
    if (!v) return NULL;
    v->dev = dev;
    v->hash_size = 1024;
    v->limit = 1024;
    if (!(v->hash = kzalloc(v->hash_size * sizeof *v->hash))) {
        kfree(v);
        return NULL;
    }
    return v;
}

void volume_free(struct volume *v) {
    if (!v) return;
    for (size_t i = 0; i < v->hash_size; i++)
        for (struct cblock *b = v->hash[i], *next; b; b = next) {
            next = b->hash_next;
            kfree(b->data);
            kfree(b);
        }
    kfree(v->hash);
    space_destroy(v);
    fold_free(&v->fold);
    kfree(v);
}

static bool super_valid(const uint8_t *block, uint64_t address, uint64_t device_blocks, struct store_super *out) {
    struct store_super s;
    memcpy(&s, block, sizeof s);
    struct store_head head = s.head;
    head.check = 0;
    uint32_t check = crc32c(crc32c(0, &head, sizeof head), block + sizeof head, STORE_BLOCK - sizeof head);
    if (s.head.magic != STORE_MAGIC_SUPER || check != s.head.check) return false;
    if (memcmp(s.signature, STORE_SIGNATURE, 8) || s.version != STORE_VERSION || s.block_size != STORE_BLOCK) return false;
    struct store_layout l = store_layout(s.blocks);
    if (s.blocks > device_blocks || s.blocks < l.first + 16) return false;
    if (s.head.address != (s.head.generation % 2 ? s.blocks - 1 : 0) || address != s.head.address) return false;
    *out = s;
    return true;
}

int volume_mount(struct store_device *dev, struct volume **out) {
    uint8_t *block = kmalloc(STORE_BLOCK);
    if (!block) return -N_ERR_NO_MEMORY;
    struct store_super found[2];
    int n = 0;
    if (dev->read(dev, 0, 1, block) >= 0 && super_valid(block, 0, dev->blocks, &found[n])) n++;
    /* the odd generations' superblock is at the volume's last block: try each candidate's size */
    struct store_super odd;
    uint64_t last = n ? found[0].blocks - 1 : dev->blocks - 1;
    if (dev->read(dev, last, 1, block) >= 0 && super_valid(block, last, dev->blocks, &odd)) found[n++] = odd;
    kfree(block);
    if (n == 2 && found[1].head.generation > found[0].head.generation) {
        struct store_super t = found[0];
        found[0] = found[1];
        found[1] = t;
    }
    int r = -N_ERR_NOT_FOUND;
    for (int i = 0; i < n; i++) {
        /* the newest commit whose catalog and space map are whole */
        struct volume *v = volume_new(dev);
        if (!v) return -N_ERR_NO_MEMORY;
        v->super = found[i];
        v->seed = store_seed(found[i].volume_id);
        v->durable = found[i].head.generation;
        v->generation = v->durable + 1;
        v->root = found[i].catalog;
        v->root_generation = found[i].catalog_generation;
        v->next_id = found[i].next_id;
        v->next_temp = TEMP_ADDRESS;
        r = catalog_check(v);
        if (r >= 0) r = space_mount(v);
        if (r >= 0) {
            if (i) kprintf("store: the last commit is damaged; C: is as it was one commit before\n");
            *out = v;
            return 0;
        }
        volume_free(v);
    }
    return r;
}

int volume_format(struct store_device *dev, const char *label, const uint8_t volume_id[16], struct volume **out) {
    struct store_layout l = store_layout(dev->blocks);
    if (dev->blocks < l.first + 16) return -N_ERR_NO_SPACE;
    struct volume *v = volume_new(dev);
    uint8_t *zero = kzalloc(STORE_BLOCK);
    if (!v || !zero) {
        kfree(zero);
        volume_free(v);
        return -N_ERR_NO_MEMORY;
    }
    /* no superblock of whatever was here before may outlive the new volume */
    int r = dev->write(dev, 0, 1, zero);
    if (r >= 0) r = dev->write(dev, dev->blocks - 1, 1, zero);
    kfree(zero);
    struct store_super *s = &v->super;
    memcpy(s->signature, STORE_SIGNATURE, 8);
    s->version = STORE_VERSION;
    s->block_size = STORE_BLOCK;
    memcpy(s->volume_id, volume_id, 16);
    s->blocks = dev->blocks;
    s->created = store_clock();
    strlcpy(s->label, label, sizeof s->label);
    v->seed = store_seed(volume_id);
    v->generation = 1;
    v->next_id = STORE_ID_FIRST;
    v->next_temp = TEMP_ADDRESS;
    v->changed = true;
    if (r >= 0) r = space_format(v);
    if (r >= 0) r = catalog_format(v);
    if (r < 0) {
        volume_free(v);
        return r;
    }
    *out = v;
    return 0;
}
