/* A store volume: mounting, commits and the volume's own state (store/volume.h). */
#include "store/volume.h"
#include "store/catalog.h"
#include "store/crc32c.h"
#include "store/space.h"

#define STAGE_BLOCKS 64u

const char *store_trouble_text(enum store_trouble t) {
    switch (t) {
    case STORE_TROUBLE_CONTENT: return "a block does not hold what the catalog says";
    case STORE_TROUBLE_NODE: return "a catalog node cannot be read";
    case STORE_TROUBLE_ORDER: return "a catalog node's keys are out of order";
    case STORE_TROUBLE_MAP: return "a space map block is damaged";
    case STORE_TROUBLE_INDEX: return "a space index block is damaged";
    case STORE_TROUBLE_SUM: return "the space map does not add up";
    case STORE_TROUBLE_FOLD: return "the case-folding table cannot be read";
    case STORE_TROUBLE_OLDER: return "the last commit is damaged: the volume is as it was one commit before";
    case STORE_TROUBLE_PREPARE: return "a commit could not be prepared: read only from now on";
    case STORE_TROUBLE_WRITE: return "a commit could not be written: read only from now on";
    case STORE_TROUBLE_UNFINISHED: return "a change could not be finished: read only from now on";
    case STORE_TROUBLE_RULE: return "the store broke a rule of its own: read only from now on";
    }
    return "trouble";
}

void *store_array_add(const struct store_env *env, void *array_at, size_t *count, size_t *room, size_t size) {
    void **array = array_at;
    if (*count == *room) {
        size_t more = *room ? *room * 2 : 8;
        if (more < *room || more > SIZE_MAX / size) return NULL;
        void *grown = env->realloc(*array, more * size);
        if (!grown) return NULL;
        *array = grown;
        *room = more;
    }
    return (uint8_t *)*array + (*count)++ * size;
}

int ranges_add(const struct store_env *env, struct ranges *list, uint64_t start, uint64_t count) {
    if (!count) return 0;
    if (list->n && list->r[list->n - 1].start + list->r[list->n - 1].count == start) {
        list->r[list->n - 1].count += count;
        return 0;
    }
    struct range *r = store_array_add(env, &list->r, &list->n, &list->room, sizeof *r);
    if (!r) return -N_ERR_NO_MEMORY;
    *r = (struct range){start, count};
    return 0;
}

void ranges_free(const struct store_env *env, struct ranges *list) {
    env->free(list->r);
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
    if (crc32c(v->seed, data, STORE_BLOCK) == (uint32_t)check) return true;
    v->env->trouble(STORE_TROUBLE_CONTENT, address, 0);
    return false;
}

int volume_writable(struct volume *v) { return __atomic_load_n(&v->error, __ATOMIC_ACQUIRE) ? -N_ERR_READ_ONLY : 0; }

int volume_broken(struct volume *v, enum store_trouble t, uint64_t block, int r) {
    int none = 0;
    if (__atomic_compare_exchange_n(&v->error, &none, -N_ERR_READ_ONLY, false, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE))
        v->env->trouble(t, block, r);
    return r;
}

void volume_changed(struct volume *v) { __atomic_store_n(&v->changed, true, __ATOMIC_RELEASE); }

void volume_dirty(struct volume *v, struct store_block *b) {
    latch_take(&v->dirty_latch);
    if (!v->dirty_reserved || v->ndirty == v->dirty_room) { /* no room made for it */
        latch_give(&v->dirty_latch);
        volume_broken(v, STORE_TROUBLE_RULE, b->address, -N_ERR_IO);
        v->blocks->put(v->blocks, b);
        return;
    }
    v->dirty[v->ndirty++] = b;
    v->dirty_reserved--;
    latch_give(&v->dirty_latch);
}

int volume_dirty_room(struct volume *v, size_t n) {
    for (;;) {
        latch_take(&v->dirty_latch);
        size_t need = v->ndirty + v->dirty_reserved + n, room = v->dirty_room;
        if (need <= room) {
            v->dirty_reserved += n;
            latch_give(&v->dirty_latch);
            return 0;
        }
        latch_give(&v->dirty_latch);
        /* a larger list made apart, then swapped in when no other grew it meanwhile */
        size_t more = room ? room : 64;
        while (more < need) more *= 2;
        struct store_block **list = v->env->alloc(more * sizeof *list);
        if (!list) return -N_ERR_NO_MEMORY;
        latch_take(&v->dirty_latch);
        if (v->dirty_room == room) {
            memcpy(list, v->dirty, v->ndirty * sizeof *list);
            struct store_block **old = v->dirty;
            v->dirty = list;
            v->dirty_room = more;
            list = old;
        }
        latch_give(&v->dirty_latch);
        v->env->free(list);
    }
}

void volume_dirty_unreserve(struct volume *v, size_t n) {
    latch_take(&v->dirty_latch);
    v->dirty_reserved -= n;
    latch_give(&v->dirty_latch);
}

size_t volume_unwritten(struct volume *v) {
    latch_share(&v->dirty_latch);
    size_t n = v->ndirty;
    latch_unshare(&v->dirty_latch);
    return n;
}

/* ---- commits ---- */

static int device_write(struct volume *v, uint64_t block, uint32_t count, const void *buf) {
    if (block + count > v->dev->blocks || block + count < block) return -N_ERR_IO;
    return v->dev->write(v->dev, block, count, buf);
}

static int device_persist(struct volume *v) { return v->dev->persist ? v->dev->persist(v->dev) : 0; }

static void sort_blocks(struct store_block **a, size_t n) {
    /* shell sort by address: the lists are a commit's worth of blocks */
    for (size_t gap = n / 2; gap; gap /= 2)
        for (size_t i = gap; i < n; i++) {
            struct store_block *b = a[i];
            size_t j = i;
            for (; j >= gap && a[j - gap]->address > b->address; j -= gap) a[j] = a[j - gap];
            a[j] = b;
        }
}

/* every block of the list, in runs of neighbours */
static int write_blocks(struct volume *v, struct store_block **list, size_t n, uint8_t *stage) {
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

/* The blocks the catalog's nodes of the open generation go to, held, and room for them on the
   dirty list: before the tree is held, for their bitmap blocks may have to be read. No change is
   under way, so the nodes stay as many. */
static int freeze_ready(struct volume *v, size_t nodes, struct ranges *blocks) {
    int r = 0;
    for (uint64_t got = 0; r >= 0 && got < nodes;) {
        struct range one;
        if ((r = space_alloc(v, nodes - got, false, &one)) < 0) break;
        if ((r = ranges_add(v->env, blocks, one.start, one.count)) < 0) space_release(v, one.start, one.count);
        got += one.count;
    }
    if (r >= 0) r = volume_dirty_room(v, nodes);
    if (r < 0) space_release_list(v, blocks);
    return r;
}

int volume_freeze(struct volume *v, struct store_commit *out) {
    *out = (struct store_commit){0};
    int r = volume_writable(v);
    if (r < 0) return r;
    if (!__atomic_load_n(&v->changed, __ATOMIC_ACQUIRE)) return 1;
    size_t nodes = __atomic_load_n(&v->temp_nodes, __ATOMIC_ACQUIRE);
    struct ranges blocks = {0};
    /* the reservations leave room for every commit: one that cannot be made is not made whole */
    if ((r = freeze_ready(v, nodes, &blocks)) < 0) return volume_broken(v, STORE_TROUBLE_PREPARE, 0, r);
    latch_take(&v->tree);
    uint64_t generation = v->generation;
    r = catalog_freeze(v, generation, &blocks);
    ranges_free(v->env, &blocks);
    if (r >= 0) r = space_freeze(v, generation, &out->maps, &out->nmaps);
    if (r < 0) {
        latch_give(&v->tree);
        volume_dirty_unreserve(v, __atomic_load_n(&v->temp_nodes, __ATOMIC_ACQUIRE)); /* the nodes not placed */
        return volume_broken(v, STORE_TROUBLE_PREPARE, 0, r);
    }
    struct store_super *super = &out->super;
    *super = v->super;
    super->catalog = v->root;
    super->catalog_generation = v->root_generation;
    super->next_id = __atomic_load_n(&v->next_id, __ATOMIC_ACQUIRE);
    latch_take(&v->space.latch);
    super->used = v->space.used;
    latch_give(&v->space.latch);
    latch_take(&v->dirty_latch);
    out->blocks = v->dirty;
    out->n = v->ndirty;
    v->dirty = NULL;
    v->ndirty = v->dirty_room = 0;
    latch_give(&v->dirty_latch);
    out->generation = generation;
    __atomic_store_n(&v->changed, false, __ATOMIC_RELEASE);
    __atomic_store_n(&v->generation, generation + 1, __ATOMIC_RELEASE);
    latch_give(&v->tree);
    return 0;
}

int volume_write(struct volume *v, struct store_commit *c) {
    uint8_t *stage = v->env->alloc(STAGE_BLOCKS * STORE_BLOCK);
    if (!stage) return -N_ERR_NO_MEMORY;
    sort_blocks(c->blocks, c->n);
    int r = write_blocks(v, c->blocks, c->n, stage);
    for (size_t i = 0; i < c->nmaps && r >= 0; i++) r = device_write(v, c->maps[i].address, 1, c->maps[i].data);
    if (r >= 0) r = device_persist(v);
    if (r >= 0) {
        uint64_t address = c->generation % 2 ? c->super.blocks - 1 : 0;
        super_seal(&c->super, address, c->generation, stage);
        r = device_write(v, address, 1, stage);
    }
    if (r >= 0) r = device_persist(v);
    v->env->free(stage);
    return r;
}

void volume_written(struct volume *v, struct store_commit *c, int r) {
    if (r < 0) {
        volume_broken(v, STORE_TROUBLE_WRITE, 0, r);
    } else {
        v->super = c->super;
        __atomic_store_n(&v->durable, c->generation, __ATOMIC_RELEASE);
        space_committed(v, c->generation);
    }
    /* the blocks are as on the medium now (after a failure, memory keeps what reads still find) */
    for (size_t i = 0; i < c->n; i++) v->blocks->put(v->blocks, c->blocks[i]);
    for (size_t i = 0; i < c->nmaps; i++) v->env->free(c->maps[i].data);
    v->env->free(c->blocks);
    v->env->free(c->maps);
    *c = (struct store_commit){0};
}

int volume_commit(struct volume *v) {
    struct store_commit c;
    int r = volume_freeze(v, &c);
    if (r) return r < 0 ? r : 0;
    r = volume_write(v, &c);
    volume_written(v, &c, r);
    return r < 0 ? volume_writable(v) : 0;
}

int volume_even(struct volume *v) {
    if (v->durable % 2) volume_changed(v); /* a commit with nothing new in it */
    return v->durable % 2 ? volume_commit(v) : volume_writable(v);
}

int volume_grow(struct volume *v, uint64_t blocks) {
    int r = volume_writable(v);
    if (r >= 0 && (v->durable % 2 || blocks > v->dev->blocks)) r = -N_ERR_INVALID;
    if (r >= 0) r = space_grow(v, blocks);
    /* the growth, then the commit that writes the other copy of the map and frees the old one */
    if (r >= 0) r = volume_commit(v);
    if (r >= 0) r = volume_commit(v);
    if (r >= 0) space_trim(v);
    return r;
}

/* ---- mounting ---- */

static struct volume *volume_new(const struct store_env *env, struct store_device *dev, struct store_supply *blocks) {
    struct volume *v = env->alloc(sizeof *v);
    if (!v) return NULL;
    memset(v, 0, sizeof *v);
    v->env = env;
    v->dev = dev;
    v->blocks = blocks;
    v->next_temp = TEMP_ADDRESS;
    return v;
}

void volume_free(struct volume *v) {
    if (!v) return;
    catalog_destroy(v);
    for (size_t i = 0; i < v->ndirty; i++) v->blocks->put(v->blocks, v->dirty[i]);
    v->env->free(v->dirty);
    space_destroy(v);
    fold_free(v->env, &v->fold);
    v->env->free(v);
}

static bool super_valid(const uint8_t *block, uint64_t address, uint64_t device_blocks, struct store_super *out) {
    struct store_super s;
    memcpy(&s, block, sizeof s);
    struct store_head head = s.head;
    head.check = 0;
    uint32_t check = crc32c(crc32c(0, &head, sizeof head), block + sizeof head, STORE_BLOCK - sizeof head);
    if (s.head.magic != STORE_MAGIC_SUPER || check != s.head.check) return false;
    if (memcmp(s.signature, STORE_SIGNATURE, 8) || s.version != STORE_VERSION || s.block_size != STORE_BLOCK) return false;
    struct store_layout l = store_layout_of(&s);
    uint64_t n = l.indexes + l.maps;
    if (s.blocks > device_blocks || s.blocks < 2 * n + 17) return false;
    /* the map's copies between the superblocks, apart */
    for (int q = 0; q < 2; q++)
        if (l.copy[q] < 1 || l.copy[q] > s.blocks - 1 - n) return false;
    if (l.copy[0] < l.copy[1] ? l.copy[0] + n > l.copy[1] : l.copy[1] + n > l.copy[0]) return false;
    if (s.head.address != (s.head.generation % 2 ? s.blocks - 1 : 0) || address != s.head.address) return false;
    *out = s;
    return true;
}

int volume_mount(const struct store_env *env, struct store_device *dev, struct store_supply *blocks, struct volume **out) {
    uint8_t *block = env->alloc(STORE_BLOCK);
    if (!block) return -N_ERR_NO_MEMORY;
    struct store_super found[3];
    int n = 0;
    if (dev->read(dev, 0, 1, block) >= 0 && super_valid(block, 0, dev->blocks, &found[n])) n++;
    /* The odd generations' superblock is at the volume's last block: where block 0's size puts it,
       and at the device's end, where a volume grown to it in an odd generation has it while block 0
       still has the size before. Another volume's left there is not this one's. */
    uint64_t last[2] = {n ? found[0].blocks - 1 : dev->blocks - 1, dev->blocks - 1};
    for (int k = 0; k < 2; k++) {
        struct store_super odd;
        if (k && last[1] == last[0]) break;
        if (dev->read(dev, last[k], 1, block) < 0 || !super_valid(block, last[k], dev->blocks, &odd)) continue;
        if (n && memcmp(odd.volume_id, found[0].volume_id, 16)) continue;
        found[n++] = odd;
    }
    env->free(block);
    for (int i = 1; i < n; i++) /* the newest first */
        for (int j = i; j > 0 && found[j].head.generation > found[j - 1].head.generation; j--) {
            struct store_super t = found[j];
            found[j] = found[j - 1];
            found[j - 1] = t;
        }
    int r = -N_ERR_NOT_FOUND;
    for (int i = 0; i < n; i++) {
        /* the newest commit whose catalog and space map are whole */
        struct volume *v = volume_new(env, dev, blocks);
        if (!v) return -N_ERR_NO_MEMORY;
        v->super = found[i];
        v->seed = store_seed(found[i].volume_id);
        v->durable = found[i].head.generation;
        v->generation = v->durable + 1;
        v->root = found[i].catalog;
        v->root_generation = found[i].catalog_generation;
        v->next_id = found[i].next_id;
        r = catalog_check(v);
        if (r >= 0) r = space_mount(v);
        if (r >= 0) {
            if (i) env->trouble(STORE_TROUBLE_OLDER, 0, 0);
            *out = v;
            return 0;
        }
        volume_free(v);
    }
    return r;
}

int volume_format(const struct store_env *env, struct store_device *dev, struct store_supply *blocks, const char *label,
                  const uint8_t volume_id[16], int64_t now, struct volume **out) {
    struct store_layout l = store_layout(dev->blocks);
    if (dev->blocks < 2 * (l.indexes + l.maps) + 17) return -N_ERR_NO_SPACE;
    struct volume *v = volume_new(env, dev, blocks);
    uint8_t *zero = env->alloc(STORE_BLOCK);
    if (!v || !zero) {
        env->free(zero);
        volume_free(v);
        return -N_ERR_NO_MEMORY;
    }
    memset(zero, 0, STORE_BLOCK);
    /* no superblock of whatever was here before may outlive the new volume */
    int r = device_write(v, 0, 1, zero);
    if (r >= 0) r = device_write(v, dev->blocks - 1, 1, zero);
    env->free(zero);
    struct store_super *s = &v->super;
    memcpy(s->signature, STORE_SIGNATURE, 8);
    s->version = STORE_VERSION;
    s->block_size = STORE_BLOCK;
    memcpy(s->volume_id, volume_id, 16);
    s->blocks = dev->blocks;
    s->created = now;
    size_t label_n = STORE_MIN(strlen(label), sizeof s->label - 1); /* the rest stays zero */
    memcpy(s->label, label, label_n);
    v->seed = store_seed(volume_id);
    v->generation = 1;
    v->next_id = STORE_ID_FIRST;
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

