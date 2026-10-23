/* The namespace over the catalog (fs/namespace.h). */
#include "fs/namespace.h"
#include "fs/space.h"
#include "lib/crc32c.h"
#include "mm/heap.h"

#define EXTENT_KEY 17
#define READ_BLOCKS 64u

/* ---- keys ---- */

static size_t key_head(uint8_t *key, uint64_t id, uint8_t type) {
    for (int i = 0; i < 8; i++) key[i] = (uint8_t)(id >> (56 - 8 * i));
    key[8] = type;
    return 9;
}

static uint64_t be64(const uint8_t *p) {
    uint64_t x = 0;
    for (int i = 0; i < 8; i++) x = x << 8 | p[i];
    return x;
}

static size_t extent_key(uint8_t *key, uint64_t doc, uint64_t index) {
    key_head(key, doc, STORE_KEY_EXTENT);
    for (int i = 0; i < 8; i++) key[9 + i] = (uint8_t)(index >> (56 - 8 * i));
    return EXTENT_KEY;
}

static int entry_key(struct volume *v, uint8_t *key, uint64_t folder, const char *name, size_t n) {
    int r = name_check(name, n);
    if (r < 0) return r;
    key_head(key, folder, STORE_KEY_ENTRY);
    r = name_fold(&v->fold, name, n, key + 9, STORE_KEY_MAX - 9);
    return r < 0 ? r : 9 + r;
}

/* A change that went part of the way: what was done stays in memory, so nothing more of it may
   reach the medium. */
static int broken(struct volume *v, int r) {
    if (!v->error) {
        kprintf("store: a change could not be finished (%d): C: is read only from now on\n", r);
        v->error = -N_ERR_READ_ONLY;
    }
    return r;
}

/* ---- records ---- */

int ns_record(struct volume *v, uint64_t id, struct store_record *out, void *content) {
    uint8_t key[9], value[STORE_VALUE_MAX];
    int n = catalog_get(v, key, key_head(key, id, STORE_KEY_RECORD), value, sizeof value);
    if (n < 0) return n;
    if ((size_t)n < sizeof *out || (size_t)n > sizeof value) return -N_ERR_IO;
    memcpy(out, value, sizeof *out);
    if (content) memcpy(content, value + sizeof *out, n - sizeof *out);
    return (int)(n - sizeof *out);
}

int ns_record_put(struct volume *v, uint64_t id, const struct store_record *r, const void *content, size_t n) {
    if (n > STORE_INLINE_MAX) return -N_ERR_TOO_LARGE;
    uint8_t key[9], value[STORE_VALUE_MAX];
    memcpy(value, r, sizeof *r);
    if (n) memcpy(value + sizeof *r, content, n);
    return catalog_put(v, key, key_head(key, id, STORE_KEY_RECORD), value, sizeof *r + n);
}

/* the folder's record, its modified time now */
static int touch(struct volume *v, uint64_t folder) {
    struct store_record r;
    int n = ns_record(v, folder, &r, NULL);
    if (n < 0) return n;
    r.modified = store_clock();
    return ns_record_put(v, folder, &r, NULL, 0);
}

/* ---- entries ---- */

static int entry_from(const struct item *it, struct ns_entry *out) {
    if (it->vlen < sizeof(struct store_entry) || it->vlen - sizeof(struct store_entry) > 255) return -N_ERR_IO;
    struct store_entry e;
    memcpy(&e, it->value, sizeof e);
    out->id = e.id;
    out->n = it->vlen - sizeof e;
    memcpy(out->name, it->value + sizeof e, out->n);
    out->name[out->n] = 0;
    out->klen = it->klen;
    memcpy(out->key, it->key, it->klen);
    return 0;
}

int ns_find(struct volume *v, uint64_t folder, const char *name, size_t n, struct ns_entry *out) {
    struct item it;
    int k = entry_key(v, it.key, folder, name, n);
    if (k < 0) return k;
    it.klen = (size_t)k;
    int r = catalog_get(v, it.key, it.klen, it.value, sizeof it.value);
    if (r < 0) return r;
    it.vlen = (size_t)r;
    return entry_from(&it, out);
}

int ns_lookup(struct volume *v, const struct path *p, struct ns_entry *entry, struct store_record *record) {
    *entry = (struct ns_entry){.id = STORE_ID_ROOT};
    int r = ns_record(v, STORE_ID_ROOT, record, NULL);
    for (size_t i = 0; i < p->count && r >= 0; i++) {
        if (record->kind != STORE_FOLDER) return -N_ERR_NOT_FOLDER;
        if ((r = ns_find(v, entry->id, p->part[i].name, p->part[i].n, entry)) >= 0) r = ns_record(v, entry->id, record, NULL);
    }
    return r < 0 ? r : 0;
}

int ns_next(struct volume *v, uint64_t folder, const struct ns_entry *after, struct ns_entry *out) {
    uint8_t start[9], limit[9];
    key_head(start, folder, STORE_KEY_ENTRY);
    key_head(limit, folder, STORE_KEY_ENTRY + 1);
    struct item it;
    int r = after ? catalog_next(v, after->key, after->klen, false, limit, 9, &it)
                  : catalog_next(v, start, 9, true, limit, 9, &it);
    return r < 0 ? r : entry_from(&it, out);
}

int ns_empty(struct volume *v, uint64_t folder) {
    struct ns_entry e;
    int r = ns_next(v, folder, NULL, &e);
    return r == -N_ERR_NOT_FOUND ? 1 : r < 0 ? r : 0;
}

static int entry_put(struct volume *v, const uint8_t *key, size_t klen, uint64_t id, const char *name, size_t n,
                     bool replace) {
    uint8_t value[sizeof(struct store_entry) + 255];
    struct store_entry e = {id};
    memcpy(value, &e, sizeof e);
    memcpy(value + sizeof e, name, n);
    return replace ? catalog_put(v, key, klen, value, sizeof e + n) : catalog_insert(v, key, klen, value, sizeof e + n);
}

int ns_create(struct volume *v, uint64_t folder, const char *name, size_t n, uint16_t kind, uint16_t attributes,
              uint16_t flags, uint64_t *id) {
    struct store_record parent;
    int r = ns_record(v, folder, &parent, NULL);
    if (r < 0) return r;
    if (parent.kind != STORE_FOLDER) return -N_ERR_NOT_FOLDER;
    uint8_t key[STORE_KEY_MAX];
    int k = entry_key(v, key, folder, name, n);
    if (k < 0) return k;
    if ((r = volume_writable(v)) < 0) return r;
    uint64_t new_id = v->next_id;
    if ((r = entry_put(v, key, (size_t)k, new_id, name, n, false)) < 0) return r; /* taken, or nothing changed */
    int64_t now = store_clock();
    struct store_record rec = {.parent = folder, .created = now, .modified = now, .kind = kind,
                               /* what may not change its folder may not change what is put in it */
                               .attributes = (uint16_t)(attributes | (parent.attributes & (STORE_SYSTEM | STORE_PRIVATE))),
                               .flags = (uint16_t)(flags | (kind == STORE_DOCUMENT ? STORE_INLINE : 0))};
    if ((r = ns_record_put(v, new_id, &rec, NULL, 0)) < 0 || (r = touch(v, folder)) < 0) return broken(v, r);
    v->next_id++;
    *id = new_id;
    return 0;
}

int ns_move(struct volume *v, uint64_t folder, const char *name, size_t n, uint64_t to_folder, const char *to_name,
            size_t to_n) {
    struct ns_entry e;
    int r = ns_find(v, folder, name, n, &e);
    if (r < 0) return r;
    uint8_t key[STORE_KEY_MAX];
    int k = entry_key(v, key, to_folder, to_name, to_n);
    if (k < 0) return k;
    if ((r = volume_writable(v)) < 0) return r;
    if ((size_t)k == e.klen && !memcmp(key, e.key, e.klen)) /* the same entry: only its case changes */
        return entry_put(v, key, (size_t)k, e.id, to_name, to_n, true);
    struct store_record rec, up;
    uint8_t content[STORE_INLINE_MAX];
    int inl = ns_record(v, e.id, &rec, content);
    if (inl < 0) return inl;
    /* the folder it goes to must be a folder, and not this one or inside it */
    for (uint64_t at = to_folder;; at = up.parent) {
        if (at == e.id) return -N_ERR_INVALID;
        if ((r = ns_record(v, at, &up, NULL)) < 0) return r;
        if (at == to_folder && up.kind != STORE_FOLDER) return -N_ERR_NOT_FOLDER;
        if (at == STORE_ID_ROOT) break;
    }
    if ((r = entry_put(v, key, (size_t)k, e.id, to_name, to_n, false)) < 0) return r;
    rec.parent = to_folder;
    if ((r = catalog_delete(v, e.key, e.klen)) < 0 || (r = ns_record_put(v, e.id, &rec, content, (size_t)inl)) < 0 ||
        (r = touch(v, folder)) < 0 || (r = touch(v, to_folder)) < 0)
        return broken(v, r);
    return 0;
}

int ns_remove(struct volume *v, uint64_t folder, const char *name, size_t n, struct ranges *dropped) {
    struct ns_entry e;
    struct store_record rec;
    int r = ns_find(v, folder, name, n, &e);
    if (r >= 0) r = ns_record(v, e.id, &rec, NULL);
    if (r < 0) return r;
    if ((r = volume_writable(v)) < 0) return r;
    if (rec.kind == STORE_FOLDER && (r = ns_empty(v, e.id)) <= 0) return r < 0 ? r : -N_ERR_NOT_EMPTY;
    if (rec.kind == STORE_DOCUMENT && !(rec.flags & STORE_INLINE) && (r = ns_cut(v, e.id, 0, dropped)) < 0) return r;
    uint8_t key[9];
    if ((r = catalog_delete(v, e.key, e.klen)) < 0 ||
        (r = catalog_delete(v, key, key_head(key, e.id, STORE_KEY_RECORD))) < 0 || (r = touch(v, folder)) < 0)
        return broken(v, r);
    return 0;
}

int ns_clear(struct volume *v, uint64_t folder) {
    struct ns_entry *e = kmalloc(sizeof *e);
    int r = e ? 0 : -N_ERR_NO_MEMORY;
    while (r >= 0 && (r = ns_next(v, folder, NULL, e)) >= 0) {
        struct store_record rec;
        if ((r = ns_record(v, e->id, &rec, NULL)) >= 0 && rec.kind == STORE_FOLDER) r = ns_clear(v, e->id);
        struct ranges dropped = {0};
        if (r >= 0) r = ns_remove(v, folder, e->name, e->n, &dropped);
        space_release_list(v, &dropped);
    }
    kfree(e);
    return r == -N_ERR_NOT_FOUND ? 0 : r;
}

/* ---- contents ---- */

static int extent_from(const struct item *it, struct ns_extent *out) {
    struct store_extent x;
    if (it->klen != EXTENT_KEY || it->vlen < sizeof x) return -N_ERR_IO;
    memcpy(&x, it->value, sizeof x);
    out->index = be64(it->key + 9);
    out->count = x.count;
    out->block = x.block;
    out->checked = it->vlen > sizeof x;
    if (!x.count || (out->checked && (x.count > STORE_EXTENT_CHECKS || it->vlen != sizeof x + 4 * (size_t)x.count)))
        return -N_ERR_IO;
    if (out->checked) memcpy(out->check, it->value + sizeof x, 4 * (size_t)x.count);
    return 0;
}

static int extent_put(struct volume *v, uint64_t doc, const struct ns_extent *e, bool replace) {
    uint8_t key[EXTENT_KEY], value[STORE_VALUE_MAX];
    struct store_extent x = {.block = e->block, .count = (uint32_t)e->count};
    memcpy(value, &x, sizeof x);
    size_t vlen = sizeof x;
    if (e->checked) {
        memcpy(value + sizeof x, e->check, 4 * (size_t)e->count);
        vlen += 4 * (size_t)e->count;
    }
    extent_key(key, doc, e->index);
    return replace ? catalog_put(v, key, EXTENT_KEY, value, vlen) : catalog_insert(v, key, EXTENT_KEY, value, vlen);
}

/* e from its block `from` on, `count` of them */
static struct ns_extent extent_part(const struct ns_extent *e, uint64_t from, uint64_t count) {
    struct ns_extent p = {.index = e->index + from, .count = count, .block = e->block + from, .checked = e->checked};
    if (e->checked) memcpy(p.check, e->check + from, 4 * (size_t)count);
    return p;
}

int ns_extent(struct volume *v, uint64_t doc, uint64_t index, struct ns_extent *out) {
    uint8_t key[EXTENT_KEY], floor[EXTENT_KEY], limit[9];
    struct item it;
    extent_key(key, doc, index);
    extent_key(floor, doc, 0);
    int r = catalog_last(v, key, EXTENT_KEY, floor, EXTENT_KEY, &it);
    if (r >= 0 && (r = extent_from(&it, out)) >= 0 && out->index + out->count > index) return 0;
    if (r < 0 && r != -N_ERR_NOT_FOUND) return r;
    /* a hole up to the next extent */
    key_head(limit, doc, STORE_KEY_EXTENT + 1);
    r = catalog_next(v, key, EXTENT_KEY, true, limit, 9, &it);
    if (r < 0 && r != -N_ERR_NOT_FOUND) return r;
    uint64_t end = UINT64_MAX;
    if (r >= 0) end = be64(it.key + 9);
    *out = (struct ns_extent){.index = index, .count = end - index};
    return 0;
}

int ns_extent_read(struct volume *v, const struct ns_extent *e, uint64_t first, uint32_t count, struct cblock **out) {
    if (!e->block || first + count > e->count) return -N_ERR_INVALID;
    return cache_read_many(v, e->block + first, count, e->checked ? content_valid : NULL,
                           e->checked ? e->check + first : NULL, out);
}

/* the blocks from index a up to b out of the document, into dropped (b UINT64_MAX: to its end) */
static int cut_range(struct volume *v, uint64_t doc, uint64_t a, uint64_t b, struct ranges *dropped) {
    uint8_t key[EXTENT_KEY], floor[EXTENT_KEY], limit[EXTENT_KEY];
    size_t llen = b == UINT64_MAX ? key_head(limit, doc, STORE_KEY_EXTENT + 1) : extent_key(limit, doc, b);
    struct item it;
    struct ns_extent e;
    extent_key(key, doc, a);
    extent_key(floor, doc, 0);
    bool before = a > 0, changed = false;
    for (;;) {
        /* the extent from before a that reaches into it, then each one that starts in [a, b) */
        int r = before ? catalog_last(v, key, EXTENT_KEY, floor, EXTENT_KEY, &it)
                       : catalog_next(v, key, EXTENT_KEY, true, limit, llen, &it);
        if (r == -N_ERR_NOT_FOUND && before) {
            before = false;
            continue;
        }
        if (r == -N_ERR_NOT_FOUND) return 0;
        if (r < 0 || (r = extent_from(&it, &e)) < 0) return changed ? broken(v, r) : r;
        bool was_before = before;
        before = false;
        if (was_before && (e.index >= a || e.index + e.count <= a)) continue; /* starts at a, or ends before it */
        uint64_t end = e.index + e.count, from = MAX(e.index, a), to = MIN(end, b);
        if (e.index < a) {
            struct ns_extent left = extent_part(&e, 0, a - e.index);
            r = extent_put(v, doc, &left, true);
        } else {
            r = catalog_delete(v, it.key, it.klen);
        }
        if (r >= 0 && end > b) {
            struct ns_extent right = extent_part(&e, b - e.index, end - b);
            r = extent_put(v, doc, &right, false);
        }
        uint64_t at = e.block + (from - e.index);
        if (r >= 0) r = space_unuse(v, at, to - from);
        if (r >= 0) r = ranges_add(dropped, at, to - from);
        if (r < 0) return broken(v, r);
        changed = true;
    }
}

int ns_cut(struct volume *v, uint64_t doc, uint64_t index, struct ranges *dropped) {
    int r = volume_writable(v);
    return r < 0 ? r : cut_range(v, doc, index, UINT64_MAX, dropped);
}

/* index .. index + count - 1 of the document are at block on: new extents for them, the one before
   grown when they follow it */
static int place(struct volume *v, uint64_t doc, uint64_t index, uint64_t count, uint64_t block, const uint32_t *check,
                 struct ranges *dropped) {
    int r = cut_range(v, doc, index, index + count, dropped);
    if (r < 0) return r;
    uint64_t room = check ? STORE_EXTENT_CHECKS : UINT32_MAX;
    struct ns_extent e;
    if (index > 0 && (r = ns_extent(v, doc, index - 1, &e)) >= 0 && e.block && e.block + e.count == block &&
        e.checked == !!check && e.count < room) {
        uint64_t k = MIN(count, room - e.count);
        if (check) memcpy(e.check + e.count, check, 4 * (size_t)k);
        e.count += k;
        if ((r = extent_put(v, doc, &e, true)) < 0) return broken(v, r);
        index += k, block += k, count -= k;
        if (check) check += k;
    }
    if (r < 0) return broken(v, r);
    while (count) {
        uint64_t k = MIN(count, room);
        e = (struct ns_extent){.index = index, .count = k, .block = block, .checked = !!check};
        if (check) memcpy(e.check, check, 4 * (size_t)k);
        if ((r = extent_put(v, doc, &e, false)) < 0) return broken(v, r);
        index += k, block += k, count -= k;
        if (check) check += k;
    }
    return 0;
}

int ns_write(struct volume *v, uint64_t doc, bool checked, uint64_t index, size_t n, uint8_t **data, bool reserved,
             struct ranges *dropped) {
    size_t done = 0, spent = 0;
    int r = volume_writable(v);
    uint32_t *check = NULL;
    if (r >= 0 && checked && !data) r = -N_ERR_INVALID;
    if (r >= 0 && checked && !(check = kmalloc(n * sizeof *check))) r = -N_ERR_NO_MEMORY;
    while (r >= 0 && done < n) {
        struct range got;
        if ((r = space_alloc(v, n - done, reserved, &got)) < 0) break;
        spent += got.count;
        for (uint64_t i = 0; data && i < got.count && r >= 0; i++) {
            uint8_t *d = data[done + i];
            if (check) check[done + i] = crc32c(v->seed, d, STORE_BLOCK);
            struct cblock *b = cache_take(v, got.start + i, d, &r);
            if (!b) break;
            data[done + i] = NULL;
            cache_unwritten(v, b);
        }
        if (r < 0) { /* the run goes back unnamed: what of it reached the cache is written for nothing */
            space_release(v, got.start, got.count);
            break;
        }
        if ((r = space_use(v, got.start, got.count)) < 0) {
            space_release(v, got.start, got.count);
            broken(v, r);
            break;
        }
        if ((r = place(v, doc, index + done, got.count, got.start, check ? check + done : NULL, dropped)) < 0) break;
        done += got.count;
    }
    if (r < 0 && done) broken(v, r);
    if (reserved && spent < n) space_unreserve(v, n - spent);
    for (size_t i = 0; data && i < n; i++) kfree(data[i]);
    kfree(check);
    return r < 0 ? r : 0;
}

int ns_extents(struct volume *v, uint64_t doc, struct ns_extent **out, size_t *n) {
    uint8_t key[STORE_KEY_MAX], limit[9];
    size_t klen = extent_key(key, doc, 0);
    key_head(limit, doc, STORE_KEY_EXTENT + 1);
    struct ns_extent *list = NULL;
    size_t count = 0, room = 0;
    struct item it;
    int r = catalog_next(v, key, klen, true, limit, 9, &it);
    while (r >= 0) {
        struct ns_extent *e = kappend(&list, &count, &room, sizeof *e);
        if (!e) r = -N_ERR_NO_MEMORY;
        else if ((r = extent_from(&it, e)) >= 0) {
            memcpy(key, it.key, klen = it.klen);
            r = catalog_next(v, key, klen, false, limit, 9, &it);
        }
    }
    if (r != -N_ERR_NOT_FOUND) {
        kfree(list);
        return r;
    }
    *out = list;
    *n = count;
    return 0;
}

int64_t ns_read(struct volume *v, uint64_t id, uint64_t offset, void *buf, size_t n) {
    struct store_record rec;
    uint8_t content[STORE_INLINE_MAX];
    int inl = ns_record(v, id, &rec, content);
    if (inl < 0) return inl;
    if (rec.kind != STORE_DOCUMENT) return -N_ERR_IS_FOLDER;
    if (offset >= rec.size) return 0;
    n = (size_t)MIN((uint64_t)n, rec.size - offset);
    if (rec.flags & STORE_INLINE) {
        if (rec.size > (uint64_t)inl) return -N_ERR_IO;
        memcpy(buf, content + offset, n);
        return (int64_t)n;
    }
    uint8_t *out = buf;
    for (size_t done = 0; done < n;) {
        uint64_t at = offset + done, index = at / STORE_BLOCK;
        struct ns_extent e;
        int r = ns_extent(v, id, index, &e);
        if (r < 0) return r;
        uint64_t blocks = MIN(e.index + e.count - index, (at % STORE_BLOCK + (n - done) + STORE_BLOCK - 1) / STORE_BLOCK);
        if (!e.block) { /* a hole */
            size_t k = (size_t)MIN(blocks * STORE_BLOCK - at % STORE_BLOCK, (uint64_t)(n - done));
            memset(out + done, 0, k);
            done += k;
            continue;
        }
        struct cblock *got[READ_BLOCKS];
        uint32_t count = (uint32_t)MIN(blocks, (uint64_t)READ_BLOCKS);
        if ((r = ns_extent_read(v, &e, index - e.index, count, got)) < 0) return r;
        for (uint32_t i = 0; i < count; i++) {
            size_t skip = i ? 0 : at % STORE_BLOCK, k = MIN(STORE_BLOCK - skip, n - done);
            memcpy(out + done, got[i]->data + skip, k);
            done += k;
            cache_put(v, got[i]);
        }
    }
    return (int64_t)n;
}

/* one more page, kept in index order */
static int page_add(struct ns_page **pages, size_t *n, size_t *room, uint64_t index, uint8_t *data) {
    if (!kappend(pages, n, room, sizeof **pages)) return -N_ERR_NO_MEMORY;
    size_t at = *n - 1;
    for (; at && (*pages)[at - 1].index > index; at--) (*pages)[at] = (*pages)[at - 1];
    (*pages)[at] = (struct ns_page){index, data};
    return 0;
}

static bool page_of(const struct ns_page *pages, size_t n, uint64_t index) {
    for (size_t i = 0; i < n; i++)
        if (pages[i].index == index) return true;
    return false;
}

int ns_update(struct volume *v, uint64_t id, uint64_t size, uint64_t cut, struct ns_page *given, size_t n, bool reserved,
              struct ranges *dropped) {
    struct store_record rec;
    struct ns_page *pages = NULL;
    size_t count = 0, room = 0;
    uint64_t unspent = reserved ? n : 0, blocks = (size + STORE_BLOCK - 1) / STORE_BLOCK, last = size / STORE_BLOCK;
    size_t tail = (size_t)(size % STORE_BLOCK);
    bool changed = false; /* something of the change is done: a failure from then on breaks the volume */
    uint8_t *content = kzalloc(STORE_INLINE_MAX);
    int r = content ? ns_record(v, id, &rec, content) : -N_ERR_NO_MEMORY;
    if (r >= 0) r = volume_writable(v);
    if (r >= 0 && rec.kind != STORE_DOCUMENT) r = -N_ERR_IS_FOLDER;
    bool was_inline = r >= 0 && (rec.flags & STORE_INLINE);
    cut = MIN(cut, blocks);
    for (size_t i = 0; i < n; i++) {
        if (r >= 0 && given[i].index < blocks && (r = page_add(&pages, &count, &room, given[i].index, given[i].data)) >= 0) {
            if (given[i].index == last && tail) memset(given[i].data + tail, 0, STORE_BLOCK - tail);
            continue;
        }
        kfree(given[i].data);
    }
    /* a block the content keeps that no page gives, and that must change: the small content as
       block 0, or the last block with zeros after the new end */
    uint64_t extra = UINT64_MAX;
    if (size > STORE_INLINE_MAX && was_inline && rec.size && cut) extra = 0;
    if (size > STORE_INLINE_MAX && !was_inline && tail && size < rec.size && cut > last) extra = last;
    if (r >= 0 && extra != UINT64_MAX && !page_of(pages, count, extra)) {
        uint8_t *data = kzalloc(STORE_BLOCK);
        if (!data) r = -N_ERR_NO_MEMORY;
        else if (was_inline) memcpy(data, content, (size_t)rec.size);
        else {
            int64_t got = ns_read(v, id, last * STORE_BLOCK, data, tail);
            if (got < 0) r = (int)got;
        }
        if (r >= 0 && reserved && (r = space_reserve(v, 1)) >= 0) unspent++;
        if (r >= 0) r = page_add(&pages, &count, &room, extra, data);
        if (r < 0) kfree(data);
    }
    if (r >= 0 && size <= STORE_INLINE_MAX) {
        /* into the record: what is kept, the page over it */
        uint64_t keep = MIN(MIN(rec.size, cut * STORE_BLOCK), size);
        if (was_inline) memset(content + keep, 0, STORE_INLINE_MAX - (size_t)keep);
        else {
            memset(content, 0, STORE_INLINE_MAX);
            int64_t got = keep ? ns_read(v, id, 0, content, (size_t)keep) : 0;
            if (got < 0) r = (int)got;
        }
        if (count) memcpy(content, pages[0].data, (size_t)size); /* page 0 is the only one under the size */
        if (r >= 0 && !was_inline && (r = ns_cut(v, id, 0, dropped)) >= 0) changed = true;
        rec.flags |= STORE_INLINE;
    } else if (r >= 0) {
        if (!was_inline && (r = ns_cut(v, id, cut, dropped)) >= 0) changed = true;
        /* each run of neighbouring pages in one write */
        for (size_t i = 0, k; i < count && r >= 0; i += k) {
            for (k = 1; i + k < count && pages[i + k].index == pages[i].index + k;) k++;
            uint8_t **data = kmalloc(k * sizeof *data);
            if (!data) {
                r = -N_ERR_NO_MEMORY;
                break;
            }
            for (size_t j = 0; j < k; j++) data[j] = pages[i + j].data, pages[i + j].data = NULL;
            r = ns_write(v, id, !(rec.flags & STORE_UNCHECKED), pages[i].index, k, data, reserved, dropped);
            if (reserved) unspent -= k;
            kfree(data);
            if (r >= 0) changed = true;
        }
        rec.flags &= (uint16_t)~STORE_INLINE;
    }
    if (r >= 0) {
        rec.size = size;
        rec.modified = store_clock();
        r = ns_record_put(v, id, &rec, content, rec.flags & STORE_INLINE ? (size_t)size : 0);
    }
    for (size_t i = 0; i < count; i++) kfree(pages[i].data);
    kfree(pages);
    kfree(content);
    if (unspent) space_unreserve(v, unspent);
    if (r < 0) return changed ? broken(v, r) : r;
    return 0;
}

int ns_take(struct volume *v, uint64_t doc, uint64_t from, struct ranges *dropped) {
    struct store_record to, rec;
    uint8_t *content = kmalloc(STORE_INLINE_MAX);
    int r = content ? ns_record(v, from, &rec, content) : -N_ERR_NO_MEMORY, inl = r;
    if (r >= 0) r = ns_record(v, doc, &to, NULL);
    if (r >= 0) r = volume_writable(v);
    if (r >= 0 && (rec.kind != STORE_DOCUMENT || to.kind != STORE_DOCUMENT)) r = -N_ERR_IS_FOLDER;
    if (r >= 0 && doc == from) r = -N_ERR_INVALID;
    bool changed = false; /* a failure from then on breaks the volume */
    if (r >= 0 && !(to.flags & STORE_INLINE) && (r = ns_cut(v, doc, 0, dropped)) >= 0) changed = true;
    /* each extent goes over as it is: its blocks stay where they are, named by doc instead */
    uint8_t key[EXTENT_KEY], limit[9], to_key[EXTENT_KEY];
    extent_key(key, from, 0);
    key_head(limit, from, STORE_KEY_EXTENT + 1);
    struct item it;
    while (r >= 0 && !(rec.flags & STORE_INLINE)) {
        if ((r = catalog_next(v, key, EXTENT_KEY, true, limit, 9, &it)) == -N_ERR_NOT_FOUND) {
            r = 0;
            break;
        }
        if (r >= 0 && it.klen != EXTENT_KEY) r = -N_ERR_IO;
        if (r >= 0 && (r = catalog_delete(v, it.key, it.klen)) >= 0) {
            changed = true;
            r = catalog_insert(v, to_key, extent_key(to_key, doc, be64(it.key + 9)), it.value, it.vlen);
        }
    }
    int64_t now = store_clock();
    if (r >= 0) {
        to.size = rec.size;
        to.modified = now;
        to.flags = (uint16_t)((to.flags & ~STORE_INLINE) | (rec.flags & STORE_INLINE));
        if ((r = ns_record_put(v, doc, &to, content, to.flags & STORE_INLINE ? (size_t)inl : 0)) >= 0) changed = true;
    }
    if (r >= 0) {
        rec.size = 0;
        rec.modified = now;
        rec.flags |= STORE_INLINE;
        r = ns_record_put(v, from, &rec, NULL, 0);
    }
    kfree(content);
    return r < 0 && changed ? broken(v, r) : r < 0 ? r : 0;
}

int ns_save(struct volume *v, uint64_t id, const void *data, size_t n) {
    size_t blocks = (n + STORE_BLOCK - 1) / STORE_BLOCK;
    struct ns_page *pages = blocks ? kzalloc(blocks * sizeof *pages) : NULL;
    int r = blocks && !pages ? -N_ERR_NO_MEMORY : 0;
    for (size_t i = 0; i < blocks && r >= 0; i++) {
        pages[i].index = i;
        if (!(pages[i].data = kzalloc(STORE_BLOCK))) r = -N_ERR_NO_MEMORY;
        else memcpy(pages[i].data, (const uint8_t *)data + i * STORE_BLOCK, MIN((size_t)STORE_BLOCK, n - i * STORE_BLOCK));
    }
    if (r < 0) {
        for (size_t i = 0; i < blocks; i++) kfree(pages[i].data);
        kfree(pages);
        return r;
    }
    struct ranges dropped = {0};
    r = ns_update(v, id, n, 0, pages, blocks, false, &dropped);
    kfree(pages);
    space_release_list(v, &dropped);
    return r;
}

/* ---- the volume's own ---- */

int ns_format(struct volume *v) {
    int64_t now = store_clock();
    struct store_record root = {.parent = STORE_ID_ROOT, .created = now, .modified = now, .kind = STORE_FOLDER};
    struct store_record fold = {.parent = STORE_ID_ROOT, .created = now, .modified = now, .kind = STORE_DOCUMENT,
                                .attributes = STORE_SYSTEM | STORE_PRIVATE, .flags = STORE_INLINE};
    uint8_t *table;
    size_t n;
    int r = fold_document(&table, &n);
    if (r < 0) return r;
    if ((r = ns_record_put(v, STORE_ID_ROOT, &root, NULL, 0)) >= 0 && (r = ns_record_put(v, STORE_ID_FOLD, &fold, NULL, 0)) >= 0 &&
        (r = ns_save(v, STORE_ID_FOLD, table, n)) >= 0)
        r = fold_load(table, n, &v->fold);
    kfree(table);
    return r;
}

int ns_mount(struct volume *v) {
    struct store_record rec;
    int r = ns_record(v, STORE_ID_FOLD, &rec, NULL);
    if (r < 0) return r;
    if (rec.kind != STORE_DOCUMENT || rec.size > 64 << 20) return -N_ERR_IO;
    uint8_t *table = kmalloc(rec.size ? rec.size : 1);
    if (!table) return -N_ERR_NO_MEMORY;
    int64_t got = ns_read(v, STORE_ID_FOLD, 0, table, rec.size);
    r = got < 0 ? (int)got : (uint64_t)got != rec.size ? -N_ERR_IO : fold_load(table, rec.size, &v->fold);
    kfree(table);
    if (r < 0) kprintf("store: the volume's case-folding table cannot be read (%d)\n", r);
    return r;
}
