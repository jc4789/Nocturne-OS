/* The space map (fs/space.h). */
#include "fs/space.h"
#include "mm/heap.h"

#define BITS STORE_BITS_PER_MAP
#define PER_INDEX STORE_COUNTS_PER_INDEX

static uint64_t *bits_of(uint8_t *map) { return (uint64_t *)(map + sizeof(struct store_head)); }

/* what catalog commits keep for themselves */
static uint64_t margin(struct volume *v) { return MAX(512, v->super.blocks / 256); }

static uint64_t map_address(struct volume *v, unsigned copy, uint64_t m) {
    return v->space.layout.copy[copy] + v->space.layout.indexes + m;
}

/* A bitmap block goes into a copy in the commit that changes it and in the next one: the generation
   that last wrote it into copy q, when it last changed in `changed`. */
static uint64_t written_in(uint64_t changed, unsigned q) { return changed % 2 == q ? changed : changed + 1; }

/* bitmap block m, read the first time from a copy that has it as the last commit left it */
static uint8_t *map_get(struct volume *v, uint64_t m, int *error) {
    struct space *s = &v->space;
    if (s->maps[m]) return s->maps[m];
    uint8_t *map = kmalloc(STORE_BLOCK);
    if (!map) {
        *error = -N_ERR_NO_MEMORY;
        return NULL;
    }
    int r = -N_ERR_IO;
    for (unsigned k = 0; k < 2; k++) {
        unsigned q = (unsigned)((v->durable + k) % 2);
        uint64_t generation = written_in(s->counts[m].changed, q), address = map_address(v, q, m);
        if (generation > v->durable) continue; /* this copy has it as it was before */
        r = v->dev->read(v->dev, address, 1, map);
        if (r >= 0 && head_valid(v, map, STORE_MAGIC_MAP, address, generation)) return s->maps[m] = map;
        kprintf("store: space map block %lu is damaged\n", (unsigned long)address);
        if (r >= 0) r = -N_ERR_IO;
    }
    kfree(map);
    *error = r;
    return NULL;
}

static int space_new(struct volume *v) {
    struct space *s = &v->space;
    s->layout = store_layout_of(&v->super);
    s->counts = kzalloc(s->layout.maps * sizeof *s->counts);
    s->maps = kzalloc(s->layout.maps * sizeof *s->maps);
    s->index_changed = kzalloc(s->layout.indexes * sizeof *s->index_changed);
    return s->counts && s->maps && s->index_changed ? 0 : -N_ERR_NO_MEMORY;
}

void space_destroy(struct volume *v) {
    struct space *s = &v->space;
    if (s->maps)
        for (uint64_t m = 0; m < s->layout.maps; m++) kfree(s->maps[m]);
    kfree(s->maps);
    kfree(s->counts);
    kfree(s->index_changed);
    ranges_free(&s->holds);
    kfree(s->pending);
    memset(s, 0, sizeof *s);
}

static void changed(struct volume *v, uint64_t m) {
    v->space.counts[m].changed = v->generation;
    v->space.index_changed[m / PER_INDEX] = v->generation;
    v->changed = true;
}

/* sets (or clears) the bits of start .. start + count - 1; how many changed */
static int mark(struct volume *v, uint64_t start, uint64_t count, bool use) {
    struct space *s = &v->space;
    if (start + count > v->super.blocks || start + count < start) return -N_ERR_INVALID;
    while (count) {
        uint64_t m = start / BITS, bit = start % BITS, k = MIN(count, BITS - bit);
        int error = 0;
        uint8_t *map = map_get(v, m, &error);
        if (!map) return error;
        uint64_t *w = bits_of(map);
        for (uint64_t i = bit; i < bit + k; i++) {
            uint64_t one = 1ULL << (i % 64);
            if (!(w[i / 64] & one) != use) panic("store: block %lu is already %s", (unsigned long)(m * BITS + i), use ? "used" : "free");
            w[i / 64] ^= one;
        }
        if (use) s->counts[m].free -= (uint32_t)k, s->used += k;
        else s->counts[m].free += (uint32_t)k, s->used -= k;
        changed(v, m);
        start += k;
        count -= k;
    }
    return 0;
}

/* ---- holds: sorted ranges apart from each other ---- */

/* the first hold that ends after block */
static size_t hold_find(struct space *s, uint64_t block) {
    size_t lo = 0, hi = s->holds.n;
    while (lo < hi) {
        size_t mid = (lo + hi) / 2;
        if (s->holds.r[mid].start + s->holds.r[mid].count <= block) lo = mid + 1;
        else hi = mid;
    }
    return lo;
}

static int hold_add(struct space *s, uint64_t start, uint64_t count) {
    size_t i = hold_find(s, start);
    struct range *r = s->holds.r;
    bool left = i > 0 && r[i - 1].start + r[i - 1].count == start;
    bool right = i < s->holds.n && r[i].start == start + count;
    if (i < s->holds.n && r[i].start < start + count) panic("store: blocks %lu.. held twice", (unsigned long)start);
    if (left && right) {
        r[i - 1].count += count + r[i].count;
        memmove(&r[i], &r[i + 1], (s->holds.n - i - 1) * sizeof *r);
        s->holds.n--;
    } else if (left) {
        r[i - 1].count += count;
    } else if (right) {
        r[i].start = start;
        r[i].count += count;
    } else {
        if (!kappend(&s->holds.r, &s->holds.n, &s->holds.room, sizeof *r)) return -N_ERR_NO_MEMORY;
        r = s->holds.r;
        memmove(&r[i + 1], &r[i], (s->holds.n - 1 - i) * sizeof *r);
        r[i] = (struct range){start, count};
    }
    s->held += count;
    return 0;
}

static int hold_remove(struct space *s, uint64_t start, uint64_t count) {
    size_t i = hold_find(s, start);
    struct range *r = s->holds.r;
    if (i == s->holds.n || r[i].start > start || r[i].start + r[i].count < start + count)
        panic("store: blocks %lu.. were not held", (unsigned long)start);
    uint64_t end = r[i].start + r[i].count;
    if (r[i].start == start && end == start + count) {
        memmove(&r[i], &r[i + 1], (s->holds.n - i - 1) * sizeof *r);
        s->holds.n--;
    } else if (r[i].start == start) {
        r[i].start += count;
        r[i].count -= count;
    } else if (end == start + count) {
        r[i].count -= count;
    } else { /* the middle: the hold splits in two */
        if (!kappend(&s->holds.r, &s->holds.n, &s->holds.room, sizeof *r)) return -N_ERR_NO_MEMORY;
        r = s->holds.r;
        memmove(&r[i + 2], &r[i + 1], (s->holds.n - 2 - i) * sizeof *r);
        r[i].count = start - r[i].start;
        r[i + 1] = (struct range){start + count, end - start - count};
    }
    s->held -= count;
    return 0;
}

/* ---- the public operations ---- */

uint64_t space_available(struct volume *v) {
    struct space *s = &v->space;
    uint64_t free = v->super.blocks - s->used, taken = s->held + s->reserved + margin(v);
    return free > taken ? free - taken : 0;
}

int space_used(struct volume *v, uint64_t block) {
    if (block >= v->super.blocks) return -N_ERR_INVALID;
    int error = 0;
    uint8_t *map = map_get(v, block / BITS, &error);
    if (!map) return error;
    uint64_t i = block % BITS;
    return (int)(bits_of(map)[i / 64] >> (i % 64) & 1);
}

int space_reserve(struct volume *v, uint64_t blocks) {
    if (blocks > space_available(v)) return -N_ERR_NO_SPACE;
    v->space.reserved += blocks;
    return 0;
}

void space_unreserve(struct volume *v, uint64_t blocks) {
    if (blocks > v->space.reserved) panic("store: more unreserved than reserved");
    v->space.reserved -= blocks;
}

int space_alloc(struct volume *v, uint64_t want, bool reserved, struct range *out) {
    struct space *s = &v->space;
    uint64_t free = v->super.blocks - s->used - s->held;
    if (!want || (!reserved && free <= s->reserved)) return -N_ERR_NO_SPACE;
    if (reserved && want > s->reserved) panic("store: more allocated than reserved");
    uint64_t total = v->super.blocks, start = s->cursor < total ? s->cursor : 0;
    for (uint64_t visited = 0, m = start / BITS; visited <= s->layout.maps; visited++, m = (m + 1) % s->layout.maps) {
        if (!s->counts[m].free) continue;
        int error = 0;
        uint8_t *map = map_get(v, m, &error);
        if (!map) return error;
        uint64_t *w = bits_of(map);
        uint64_t first = m * BITS, end = MIN(first + BITS, total);
        uint64_t b = visited == 0 && start > first ? start : first;
        while (b < end) {
            uint64_t i = b - first;
            uint64_t word = w[i / 64] >> (i % 64);
            if (word == UINT64_MAX >> (i % 64)) { /* the rest of this word is in use */
                b = first + (i / 64 + 1) * 64;
                continue;
            }
            if (word & 1) {
                b++;
                continue;
            }
            size_t h = hold_find(s, b);
            uint64_t limit = end;
            if (h < s->holds.n) {
                if (s->holds.r[h].start <= b) {
                    b = s->holds.r[h].start + s->holds.r[h].count;
                    continue;
                }
                limit = MIN(limit, s->holds.r[h].start);
            }
            uint64_t n = 0;
            while (n < want && b + n < limit && !(w[(i + n) / 64] >> ((i + n) % 64) & 1)) n++;
            int r = hold_add(s, b, n);
            if (r < 0) return r;
            if (reserved) s->reserved -= n;
            s->cursor = b + n;
            *out = (struct range){b, n};
            return 0;
        }
    }
    return -N_ERR_NO_SPACE;
}

int space_use(struct volume *v, uint64_t start, uint64_t count) {
    int r = hold_remove(&v->space, start, count);
    return r < 0 ? r : mark(v, start, count, true);
}

/* room for one more item of a kappend array, which it does not add */
static bool room_for_one(void *array_at, size_t *count, size_t *room, size_t size) {
    if (!kappend(array_at, count, room, size)) return false;
    (*count)--;
    return true;
}

/* the bitmap blocks of start .. start + count - 1 in memory, and room for one more hold: unmarking
   and holding those blocks cannot fail then */
static int unuse_ready(struct volume *v, uint64_t start, uint64_t count) {
    struct space *s = &v->space;
    if (start + count > v->super.blocks || start + count < start) return -N_ERR_INVALID;
    for (uint64_t m = start / BITS; count && m <= (start + count - 1) / BITS; m++) {
        int error = 0;
        if (!map_get(v, m, &error)) return error;
    }
    return room_for_one(&s->holds.r, &s->holds.n, &s->holds.room, sizeof *s->holds.r) ? 0 : -N_ERR_NO_MEMORY;
}

int space_unuse(struct volume *v, uint64_t start, uint64_t count) {
    int r = unuse_ready(v, start, count);
    if (r < 0) return r;
    mark(v, start, count, false);
    hold_add(&v->space, start, count);
    return 0;
}

int space_release(struct volume *v, uint64_t start, uint64_t count) {
    struct space *s = &v->space;
    struct pending *p = kappend(&s->pending, &s->npending, &s->pending_room, sizeof *p);
    if (!p) return -N_ERR_NO_MEMORY;
    *p = (struct pending){start, count, v->generation};
    v->changed = true; /* the hold ends with a commit, so there must be one */
    return 0;
}

int space_let_go(struct volume *v, uint64_t start, uint64_t count) {
    struct space *s = &v->space;
    int r = unuse_ready(v, start, count);
    if (r < 0) return r;
    if (!room_for_one(&s->pending, &s->npending, &s->pending_room, sizeof *s->pending)) return -N_ERR_NO_MEMORY;
    space_unuse(v, start, count);
    space_release(v, start, count);
    return 0;
}

void space_release_list(struct volume *v, struct ranges *list) {
    for (size_t i = 0; i < list->n; i++) space_release(v, list->r[i].start, list->r[i].count);
    ranges_free(list);
}

void space_committed(struct volume *v, uint64_t generation) {
    struct space *s = &v->space;
    size_t kept = 0;
    for (size_t i = 0; i < s->npending; i++) {
        struct pending p = s->pending[i];
        if (p.generation >= generation) {
            s->pending[kept++] = p;
            continue;
        }
        hold_remove(s, p.start, p.count); /* splitting a hold may need memory: it then stays held */
        cache_forget(v, p.start, p.count);
    }
    s->npending = kept;
    if (kept) v->changed = true; /* the commit that frees them */
}

/* ---- the medium ---- */

int space_format(struct volume *v) {
    int r = space_new(v);
    if (r < 0) return r;
    struct space *s = &v->space;
    uint64_t total = v->super.blocks;
    for (uint64_t m = 0; m < s->layout.maps; m++) {
        if (!(s->maps[m] = kzalloc(STORE_BLOCK))) return -N_ERR_NO_MEMORY;
        uint64_t first = m * BITS, end = MIN(first + BITS, total);
        s->counts[m].free = (uint32_t)(end - first);
        uint64_t *w = bits_of(s->maps[m]);
        for (uint64_t i = end - first; i < BITS; i++) w[i / 64] |= 1ULL << (i % 64); /* past the end */
        changed(v, m);
    }
    /* the superblocks and both copies of the map itself, right after block 0 */
    v->super.map_copy[0] = s->layout.copy[0];
    v->super.map_copy[1] = s->layout.copy[1];
    if ((r = mark(v, 0, 1 + 2 * (s->layout.indexes + s->layout.maps), true)) < 0) return r;
    return mark(v, total - 1, 1, true);
}

/* the bits of map block m for start .. end - 1 (all in it) cleared: free blocks of a larger volume */
static void map_open(struct volume *v, uint64_t m, uint64_t start, uint64_t end) {
    uint64_t *w = bits_of(v->space.maps[m]);
    for (uint64_t i = start - m * BITS; i < end - m * BITS; i++) w[i / 64] &= ~(1ULL << (i % 64));
    v->space.counts[m].free += (uint32_t)(end - start);
}

int space_grow(struct volume *v, uint64_t blocks) {
    struct space *s = &v->space;
    uint64_t old = v->super.blocks;
    struct store_layout from = s->layout, l = store_layout(blocks);
    uint64_t n = l.indexes + l.maps;
    if (blocks <= old || blocks - old < 2 * n + 1) return -N_ERR_INVALID;
    /* every bitmap block of the size before in memory: they all go to the new copies */
    for (uint64_t m = 0; m < from.maps; m++) {
        int error = 0;
        if (!map_get(v, m, &error)) return error;
    }
    struct store_space_count *counts = kzalloc(l.maps * sizeof *counts);
    uint8_t **maps = kzalloc(l.maps * sizeof *maps);
    uint64_t *index_changed = kzalloc(l.indexes * sizeof *index_changed);
    bool ok = counts && maps && index_changed;
    for (uint64_t m = from.maps; m < l.maps && ok; m++) ok = (maps[m] = kzalloc(STORE_BLOCK)) != NULL;
    /* room for the holds and pendings of the three ranges let go below: they cannot fail then */
    size_t holds = s->holds.n, pending = s->npending;
    for (int k = 0; k < 3 && ok; k++)
        ok = kappend(&s->holds.r, &s->holds.n, &s->holds.room, sizeof *s->holds.r) &&
             kappend(&s->pending, &s->npending, &s->pending_room, sizeof *s->pending);
    s->holds.n = holds;
    s->npending = pending;
    if (!ok) {
        if (maps)
            for (uint64_t m = from.maps; m < l.maps; m++) kfree(maps[m]);
        kfree(counts);
        kfree(maps);
        kfree(index_changed);
        return -N_ERR_NO_MEMORY;
    }
    memcpy(counts, s->counts, from.maps * sizeof *counts);
    memcpy(maps, s->maps, from.maps * sizeof *maps);
    kfree(s->counts);
    kfree(s->maps);
    kfree(s->index_changed);
    s->counts = counts;
    s->maps = maps;
    s->index_changed = index_changed;
    /* the blocks past the old end are free, those past the new end stay in use */
    for (uint64_t m = old / BITS; m < l.maps; m++) {
        uint64_t first = m * BITS, end = MIN(first + BITS, blocks);
        if (m >= from.maps) {
            uint64_t *w = bits_of(maps[m]);
            for (uint64_t i = end - first; i < BITS; i++) w[i / 64] |= 1ULL << (i % 64);
        }
        map_open(v, m, MAX(first, old), end);
    }
    /* the new copies at the start of the room grown by, the odd superblock at the new end */
    l.copy[0] = old;
    l.copy[1] = old + n;
    s->layout = l;
    v->super.blocks = blocks;
    v->super.map_copy[0] = l.copy[0];
    v->super.map_copy[1] = l.copy[1];
    for (uint64_t m = 0; m < l.maps; m++) changed(v, m);
    mark(v, old, 2 * n, true);
    mark(v, blocks - 1, 1, true);
    /* the copies and the odd superblock of the size before: the commit before this one stands on them */
    uint64_t was = from.indexes + from.maps;
    space_let_go(v, from.copy[0], was);
    space_let_go(v, from.copy[1], was);
    space_let_go(v, old - 1, 1);
    return 0;
}

void space_trim(struct volume *v) {
    struct space *s = &v->space;
    for (uint64_t m = 0; m < s->layout.maps; m++)
        if (s->maps[m] && s->counts[m].changed + 1 <= v->durable) {
            kfree(s->maps[m]);
            s->maps[m] = NULL;
        }
}

int space_mount(struct volume *v) {
    int r = space_new(v);
    if (r < 0) return r;
    struct space *s = &v->space;
    uint8_t *block = kmalloc(STORE_BLOCK);
    if (!block) return -N_ERR_NO_MEMORY;
    unsigned copy = v->durable % 2;
    uint64_t free = 0;
    for (uint64_t i = 0; i < s->layout.indexes && r >= 0; i++) {
        uint64_t address = s->layout.copy[copy] + i;
        r = v->dev->read(v->dev, address, 1, block);
        const struct store_head *h = (const void *)block;
        if (r >= 0 && (h->generation > v->durable || !head_valid(v, block, STORE_MAGIC_INDEX, address, h->generation)))
            r = -N_ERR_IO;
        if (r < 0) {
            kprintf("store: space index block %lu is damaged\n", (unsigned long)address);
            break;
        }
        /* the next commit writes every index block of its copy: what a commit cut short left in it
           is gone with the first that is not */
        s->index_changed[i] = v->durable;
        const struct store_space_count *c = (const void *)(block + sizeof(struct store_head));
        for (uint64_t k = 0; k < PER_INDEX && i * PER_INDEX + k < s->layout.maps; k++) {
            s->counts[i * PER_INDEX + k] = c[k];
            free += c[k].free;
        }
    }
    kfree(block);
    if (r >= 0 && free != v->super.blocks - v->super.used) {
        kprintf("store: the space map does not add up\n");
        r = -N_ERR_IO;
    }
    s->used = v->super.used;
    return r;
}

int space_freeze(struct volume *v, uint64_t generation, struct space_write **out, size_t *n) {
    struct space *s = &v->space;
    unsigned copy = generation % 2;
    size_t count = 0, room = 0;
    struct space_write *list = NULL;
    int r = 0;
    for (uint64_t m = 0; m < s->layout.maps && r >= 0; m++) {
        /* the copy of this parity last had it two commits ago */
        if (s->counts[m].changed + 1 < generation) continue;
        uint8_t *map = map_get(v, m, &r);
        struct space_write *w = map ? kappend(&list, &count, &room, sizeof *w) : NULL;
        if (map && !w) r = -N_ERR_NO_MEMORY;
        if (!w) break;
        w->address = map_address(v, copy, m);
        if (!(w->data = kmalloc(STORE_BLOCK))) {
            count--;
            r = -N_ERR_NO_MEMORY;
            break;
        }
        memcpy(w->data, map, STORE_BLOCK);
        head_seal(v, w->data, STORE_MAGIC_MAP, w->address, generation);
    }
    for (uint64_t i = 0; i < s->layout.indexes && r >= 0; i++) {
        if (s->index_changed[i] + 1 < generation) continue;
        struct space_write *w = kappend(&list, &count, &room, sizeof *w);
        if (!w || !(w->data = kzalloc(STORE_BLOCK))) {
            if (w) count--;
            r = -N_ERR_NO_MEMORY;
            break;
        }
        w->address = s->layout.copy[copy] + i;
        struct store_space_count *c = (void *)(w->data + sizeof(struct store_head));
        for (uint64_t k = 0; k < PER_INDEX && i * PER_INDEX + k < s->layout.maps; k++) c[k] = s->counts[i * PER_INDEX + k];
        head_seal(v, w->data, STORE_MAGIC_INDEX, w->address, generation);
    }
    if (r < 0) {
        for (size_t i = 0; i < count; i++) kfree(list[i].data);
        kfree(list);
        return r;
    }
    *out = list;
    *n = count;
    return 0;
}
