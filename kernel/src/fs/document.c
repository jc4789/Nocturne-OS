/* Open documents (fs/store.h, docs/nocturne-native-abi.md §3).

   What is written to a document waits in memory as pages over the content the catalog has: a work,
   in ns_update's terms (the size, the block from which the catalog's content is gone, the pages).
   The EDIT openings of a document share its work. A REPLACE opening writes a document of its own in
   the temp folder, and its save swaps that content in whole for the one at its name (ns_take); the
   opening edits that document from then on. A work goes into the catalog when an opening saves,
   when it holds WORK_PAGES, when its last writer closes, and within seconds by the store's writer;
   a save then waits for the commit that holds it.

   A READ opening reads the content the catalog had when it was opened: a version. Before the
   catalog's content changes while a version is read, the version is written down (its extents),
   and the blocks the change drops are held until no version that reads them is left: each version
   keeps what the change after it dropped, and passes it on to the next older one still read when
   it goes. A read lets go of v->lock while blocks come from the disk; its version reference keeps
   them meanwhile. */
#include "fs/document.h"
#include "arch/cpu.h"
#include "fs/space.h"
#include "mm/heap.h"
#include "sys/object.h"

#define WORK_PAGES 4096u     /* 16 MiB of pages: then the work goes into the catalog */
#define COMMIT_BLOCKS 16384u /* 64 MiB written and not yet committed: then a writer commits */
#define LOCK_PAGES 64u       /* pages written before v->lock is let go a moment for others */
#define READ_BLOCKS 64u
#define BUCKETS 64u

struct version {
    struct version *older; /* the next older superseded version still read */
    unsigned refs;
    bool superseded;       /* the catalog has moved on; what follows was its content */
    uint64_t size;
    uint8_t *content;      /* a small document's bytes */
    struct ns_extent *extents;
    size_t n;
    struct ranges dropped; /* blocks it reads that no newer version does */
};

struct work {
    uint64_t size, cut;   /* the content is size bytes; the catalog's is gone from block cut on */
    uint64_t base;        /* the catalog's size */
    struct ns_page *pages; /* by index, each reserved on C:; bytes after size are zeros */
    size_t n, room;
    bool changed;
};

struct opening;

struct doc {
    struct doc *next;
    uint64_t id;
    unsigned users;             /* openings, and calls under way */
    unsigned writers;           /* openings that write to the work */
    struct work work;
    struct version *current;    /* the catalog's content, while something reads it */
    struct version *superseded; /* newest first */
    struct opening *owner;      /* the claim's */
    struct wait_queue claim_wait, reads_done;
    uint64_t changed_in;        /* the generation with its last change */
    int error;                  /* written content was lost: every later save reports it */
    uint64_t temp;              /* its name in the temp folder (0: none): removed with its last user */
    uint64_t folder;            /* a replacement's: the folder and name it is saved to */
    char *name;
    size_t n;
};

struct opening {
    struct object object;
    struct doc *doc;          /* NULL once closed */
    int access;               /* N_DOCUMENT_READ, _EDIT, or _REPLACE until its replacement is saved */
    bool user;                /* an app's: no SYSTEM document is replaced */
    struct version *version;  /* READ's */
    uint64_t saving;          /* the generation an N_COMMIT_START save waits for (0: none) */
    int outcome;              /* the last save's, once known */
    struct opening *next_saving;
};

static struct doc *docs[BUCKETS];
static struct opening *savings; /* N_COMMIT_START saves whose outcome is not told yet */
static const uint8_t zeros[STORE_BLOCK];

static uint64_t blocks_of(uint64_t size) { return (size + STORE_BLOCK - 1) / STORE_BLOCK; }

/* the calling thread sleeps on wq, with v->lock let go meanwhile */
static void sleep_on(struct volume *v, struct wait_queue *wq) {
    uint64_t flags = irq_save();
    mutex_unlock(&v->lock);
    wq_wait(wq);
    irq_restore(flags);
    mutex_lock(&v->lock);
}

static int zero_out(uint8_t *to, size_t n, bool user) {
    int r = 0;
    for (size_t done = 0, k; done < n && r >= 0; done += k) {
        k = MIN(n - done, sizeof zeros);
        r = copy_out(to + done, zeros, k, user);
    }
    return r;
}

/* blocks go to a version's list; without memory for that they stay held until C: is mounted again */
static void keep(struct version *ver, struct ranges *r) {
    for (size_t i = 0; i < r->n; i++)
        if (ranges_add(&ver->dropped, r->r[i].start, r->r[i].count) < 0) {
            kprintf("store: no memory to track freed blocks: they stay in use until the next start\n");
            break;
        }
    ranges_free(r);
}

/* ---- documents ---- */

static struct doc *doc_find(uint64_t id) {
    for (struct doc *d = docs[id % BUCKETS]; d; d = d->next)
        if (d->id == id) return d;
    return NULL;
}

static struct doc *doc_get(uint64_t id) {
    struct doc *d = doc_find(id);
    if (d) {
        d->users++;
        return d;
    }
    if (!(d = kzalloc(sizeof *d))) return NULL;
    d->id = id;
    d->users = 1;
    d->next = docs[id % BUCKETS];
    docs[id % BUCKETS] = d;
    return d;
}

static void discard(struct volume *v, struct work *w);

static void doc_put(struct volume *v, struct doc *d) {
    if (--d->users) return;
    discard(v, &d->work); /* a write that raced its opening's save leaves pages nothing saves */
    struct doc **at = &docs[d->id % BUCKETS];
    while (*at != d) at = &(*at)->next;
    *at = d->next;
    if (d->temp) { /* nothing names it but the temp folder */
        char name[17];
        size_t n = store_temp_name(d->temp, name);
        struct ranges dropped = {0};
        int r = ns_remove(v, store.temp, name, n, &dropped);
        space_release_list(v, &dropped);
        if (r < 0) kprintf("store: a document removed while open stays in C:\\Nocturne\\Temp (%d)\n", r);
        else store_dirty(false);
    }
    kfree(d->name);
    kfree(d);
}

/* ---- versions ---- */

/* the catalog's content now, held */
static struct version *version_get(struct doc *d) {
    if (!d->current && !(d->current = kzalloc(sizeof *d->current))) return NULL;
    d->current->refs++;
    return d->current;
}

static void version_put(struct volume *v, struct doc *d, struct version *ver) {
    if (--ver->refs) return;
    if (!ver->superseded) {
        d->current = NULL;
    } else {
        struct version **at = &d->superseded;
        while (*at != ver) at = &(*at)->older;
        *at = ver->older;
        if (ver->older) keep(ver->older, &ver->dropped);
        else space_release_list(v, &ver->dropped);
        kfree(ver->content);
        kfree(ver->extents);
    }
    kfree(ver);
    wq_wake_all(&d->reads_done);
}

/* d's content in the catalog is about to change: a version still read is written down first */
static int supersede(struct volume *v, struct doc *d) {
    struct version *ver = d->current;
    if (!ver) return 0;
    struct store_record rec;
    uint8_t *content = kmalloc(STORE_INLINE_MAX);
    int r = content ? ns_record(v, d->id, &rec, content) : -N_ERR_NO_MEMORY;
    if (r >= 0 && (rec.flags & STORE_INLINE)) {
        ver->content = content;
        content = NULL;
    } else if (r >= 0) {
        r = ns_extents(v, d->id, &ver->extents, &ver->n);
    }
    kfree(content);
    if (r < 0) return r;
    ver->size = rec.size;
    ver->superseded = true;
    ver->older = d->superseded;
    d->superseded = ver;
    d->current = NULL;
    return 0;
}

/* blocks a change took out of d's content: the newest version still read keeps them */
static void dropped_to(struct volume *v, struct doc *d, struct ranges *dropped) {
    if (d->superseded) keep(d->superseded, dropped);
    else space_release_list(v, dropped);
}

/* where block index of ver is (ns_extent's terms) */
static int version_extent(struct volume *v, struct doc *d, struct version *ver, uint64_t index, struct ns_extent *e) {
    if (!ver->superseded) return ns_extent(v, d->id, index, e);
    size_t lo = 0, hi = ver->n; /* the first extent after index */
    while (lo < hi) {
        size_t mid = (lo + hi) / 2;
        if (ver->extents[mid].index <= index) lo = mid + 1;
        else hi = mid;
    }
    if (lo && ver->extents[lo - 1].index + ver->extents[lo - 1].count > index) {
        *e = ver->extents[lo - 1];
        return 0;
    }
    uint64_t end = lo < ver->n ? ver->extents[lo].index : UINT64_MAX;
    *e = (struct ns_extent){.index = index, .count = end - index};
    return 0;
}

/* n bytes of ver from offset, up to its end, into buf (an app's memory when user): how many. v->lock
   is let go while blocks come from the disk, unless hold. */
static int64_t version_read(struct volume *v, struct doc *d, struct version *ver, uint64_t offset, uint8_t *buf,
                            size_t n, bool user, bool hold) {
    size_t done = 0;
    int r = 0;
    while (done < n) {
        uint64_t at = offset + done, size = ver->size;
        bool small = ver->content;
        struct store_record rec;
        if (!ver->superseded) {
            int got = ns_record(v, d->id, &rec, NULL); /* the inline content's length: not a result */
            if (got < 0) {
                r = got;
                break;
            }
            size = rec.size;
            small = rec.flags & STORE_INLINE;
        }
        if (at >= size) break;
        size_t want = (size_t)MIN((uint64_t)(n - done), size - at);
        if (small) {
            uint8_t *mine = NULL;
            const uint8_t *from = ver->content;
            if (!ver->superseded) {
                from = mine = kmalloc(STORE_INLINE_MAX);
                r = mine ? ns_record(v, d->id, &rec, mine) : -N_ERR_NO_MEMORY;
            }
            if (r >= 0 && (r = copy_out(buf + done, from + at, want, user)) >= 0) done += want;
            kfree(mine);
            break;
        }
        if (user && current_task->killed) {
            r = -N_ERR_STOPPED;
            break;
        }
        uint64_t index = at / STORE_BLOCK;
        size_t skip = (size_t)(at % STORE_BLOCK);
        struct ns_extent e;
        if ((r = version_extent(v, d, ver, index, &e)) < 0) break;
        uint64_t blocks = MIN(e.index + e.count - index, blocks_of(skip + want));
        if (!e.block) { /* a hole */
            size_t k = (size_t)MIN(blocks * STORE_BLOCK - skip, (uint64_t)want);
            if ((r = zero_out(buf + done, k, user)) < 0) break;
            done += k;
            continue;
        }
        uint32_t count = (uint32_t)MIN(blocks, (uint64_t)READ_BLOCKS);
        struct cblock *got[READ_BLOCKS];
        if (!hold) {
            ver->refs++;
            mutex_unlock(&v->lock);
        }
        bool read = (r = ns_extent_read(v, &e, index - e.index, count, got)) >= 0;
        for (uint32_t i = 0; r >= 0 && i < count; i++) {
            size_t from = i ? 0 : skip, k = MIN(STORE_BLOCK - from, want);
            if ((r = copy_out(buf + done, got[i]->data + from, k, user)) >= 0) done += k, want -= k;
        }
        for (uint32_t i = 0; read && i < count; i++) cache_put(v, got[i]);
        if (!hold) {
            mutex_lock(&v->lock);
            version_put(v, d, ver);
        }
        if (r < 0) break;
    }
    return done ? (int64_t)done : r;
}

/* ---- works ---- */

/* the first page of w at index or after it */
static size_t page_at(const struct work *w, uint64_t index) {
    size_t lo = 0, hi = w->n;
    while (lo < hi) {
        size_t mid = (lo + hi) / 2;
        if (w->pages[mid].index < index) lo = mid + 1;
        else hi = mid;
    }
    return lo;
}

static struct ns_page *page_find(struct work *w, uint64_t index) {
    size_t i = page_at(w, index);
    return i < w->n && w->pages[i].index == index ? &w->pages[i] : NULL;
}

/* the bytes of the catalog's content the work still has */
static uint64_t kept(const struct work *w) { return MIN(MIN(w->base, w->cut * STORE_BLOCK), w->size); }

/* A page for block index, reserved on C:, with the content the block has now and k bytes from src
   at skip over it (src NULL: none). */
static int page_new(struct volume *v, struct doc *d, uint64_t index, const uint8_t *src, size_t skip, size_t k,
                    bool user, struct ns_page **out) {
    struct work *w = &d->work;
    int r = space_reserve(v, 1);
    if (r < 0) return r;
    uint8_t *data = kzalloc(STORE_BLOCK);
    if (!data) r = -N_ERR_NO_MEMORY;
    uint64_t at = index * STORE_BLOCK, old = kept(w);
    if (r >= 0 && k < STORE_BLOCK && at < old) {
        struct version *ver = version_get(d);
        int64_t got = ver ? version_read(v, d, ver, at, data, (size_t)MIN((uint64_t)STORE_BLOCK, old - at), false, true)
                          : -N_ERR_NO_MEMORY;
        if (ver) version_put(v, d, ver);
        if (got < 0) r = (int)got;
    }
    if (r >= 0 && src) r = copy_in(data + skip, src, k, user);
    size_t i = page_at(w, index);
    if (r >= 0 && !kappend(&w->pages, &w->n, &w->room, sizeof *w->pages)) r = -N_ERR_NO_MEMORY;
    if (r < 0) {
        kfree(data);
        space_unreserve(v, 1);
        return r;
    }
    memmove(&w->pages[i + 1], &w->pages[i], (w->n - 1 - i) * sizeof *w->pages);
    w->pages[i] = (struct ns_page){index, data};
    if (out) *out = &w->pages[i];
    return 0;
}

/* Content growing out of the record: its small content becomes page 0, so that the catalog needs
   no block of its own for it (ns_update). */
static int grow(struct volume *v, struct doc *d, uint64_t size) {
    struct work *w = &d->work;
    if (size <= STORE_INLINE_MAX || !w->base || w->base > STORE_INLINE_MAX || !w->cut || page_find(w, 0)) return 0;
    return page_new(v, d, 0, NULL, 0, 0, false, NULL);
}

static void discard(struct volume *v, struct work *w) {
    for (size_t i = 0; i < w->n; i++) kfree(w->pages[i].data);
    space_unreserve(v, w->n);
    kfree(w->pages);
    w->pages = NULL;
    w->n = w->room = 0;
    w->changed = false;
}

/* the work as the catalog's content of size bytes, nothing written over it */
static void work_reset(struct work *w, uint64_t size) {
    w->size = w->base = size;
    w->cut = blocks_of(size);
}

/* the work into the catalog: what was written is the saved content from now on */
static int flush(struct volume *v, struct doc *d) {
    struct work *w = &d->work;
    if (!w->changed) return 0;
    int r = supersede(v, d);
    if (r < 0) return r; /* the work stays, for a later try */
    struct ranges dropped = {0};
    r = ns_update(v, d->id, w->size, w->cut, w->pages, w->n, true, &dropped);
    kfree(w->pages);
    w->pages = NULL;
    w->n = w->room = 0;
    w->changed = false;
    dropped_to(v, d, &dropped);
    d->changed_in = v->generation;
    uint64_t size = w->size;
    if (r < 0) {
        struct store_record rec;
        size = ns_record(v, d->id, &rec, NULL) >= 0 ? rec.size : 0;
        kprintf("store: what was written to a document is lost (%d)\n", r);
        d->error = r;
    }
    work_reset(w, size);
    return r;
}

static int64_t work_read(struct volume *v, struct doc *d, uint64_t offset, uint8_t *buf, size_t n, bool user) {
    struct work *w = &d->work;
    if (offset >= w->size) return 0;
    n = (size_t)MIN((uint64_t)n, w->size - offset);
    size_t done = 0;
    int r = 0;
    while (done < n && r >= 0) {
        uint64_t at = offset + done;
        size_t i = page_at(w, at / STORE_BLOCK), skip = (size_t)(at % STORE_BLOCK), k;
        if (i < w->n && w->pages[i].index == at / STORE_BLOCK) {
            k = MIN(STORE_BLOCK - skip, n - done);
            r = copy_out(buf + done, w->pages[i].data + skip, k, user);
        } else {
            /* up to the next page: the catalog's content where it is kept, zeros after it */
            uint64_t next = i < w->n ? w->pages[i].index * STORE_BLOCK : UINT64_MAX, old = kept(w);
            k = (size_t)MIN((uint64_t)(n - done), next - at);
            if (at < old) {
                k = (size_t)MIN((uint64_t)k, old - at);
                struct version *ver = version_get(d);
                int64_t got = ver ? version_read(v, d, ver, at, buf + done, k, user, false) : -N_ERR_NO_MEMORY;
                if (ver) version_put(v, d, ver);
                r = got < 0 ? (int)got : got ? 0 : -N_ERR_IO;
                k = got > 0 ? (size_t)got : 0;
            } else {
                r = zero_out(buf + done, k, user);
            }
        }
        if (r >= 0) done += k;
    }
    return done ? (int64_t)done : r;
}

static int64_t work_write(struct volume *v, struct doc *d, uint64_t offset, const uint8_t *buf, size_t n, bool user) {
    struct work *w = &d->work;
    if (offset + n < offset || offset + n > INT64_MAX) return -N_ERR_TOO_LARGE;
    int r = grow(v, d, offset + n);
    size_t done = 0;
    for (unsigned pages = 1; r >= 0 && done < n; pages++) {
        uint64_t at = offset + done, index = at / STORE_BLOCK;
        size_t skip = (size_t)(at % STORE_BLOCK), k = MIN(STORE_BLOCK - skip, n - done);
        struct ns_page *p = page_find(w, index);
        r = p ? copy_in(p->data + skip, buf + done, k, user) : page_new(v, d, index, buf + done, skip, k, user, NULL);
        if (r < 0) break;
        done += k;
        w->size = MAX(w->size, at + k);
        w->changed = true;
        if (w->n >= WORK_PAGES) r = flush(v, d);
        if (pages % LOCK_PAGES == 0) { /* d stays: the caller holds it */
            mutex_unlock(&v->lock);
            mutex_lock(&v->lock);
            if (user && current_task->killed) r = -N_ERR_STOPPED;
        }
    }
    return done ? (int64_t)done : r;
}

static int work_resize(struct volume *v, struct doc *d, uint64_t size) {
    struct work *w = &d->work;
    if (size > INT64_MAX) return -N_ERR_TOO_LARGE;
    int r = grow(v, d, size);
    if (r >= 0 && size < w->size) {
        uint64_t last = size / STORE_BLOCK;
        size_t tail = (size_t)(size % STORE_BLOCK);
        /* zeros after the new end in its last block: on a page of its own when the catalog has it */
        struct ns_page *p = tail ? page_find(w, last) : NULL;
        if (tail && !p && last * STORE_BLOCK < kept(w)) r = page_new(v, d, last, NULL, 0, 0, false, &p);
        if (r < 0) return r;
        if (p) memset(p->data + tail, 0, STORE_BLOCK - tail);
        size_t from = page_at(w, blocks_of(size));
        for (size_t i = from; i < w->n; i++) kfree(w->pages[i].data);
        space_unreserve(v, w->n - from);
        w->n = from;
        w->cut = MIN(w->cut, blocks_of(size));
    }
    if (r >= 0) {
        w->size = size;
        w->changed = true;
    }
    return r;
}

/* one more opening writes d: the work starts from the catalog's content with the first */
static int writer_add(struct volume *v, struct doc *d) {
    if (d->writers) {
        d->writers++;
        return 0;
    }
    struct store_record rec;
    int r = ns_record(v, d->id, &rec, NULL);
    if (r < 0) return r;
    d->writers = 1;
    work_reset(&d->work, rec.size);
    return 0;
}

static void writer_remove(struct volume *v, struct doc *d) {
    if (--d->writers) return;
    if (flush(v, d) < 0 && d->work.changed) {
        kprintf("store: what was written to a document is lost (no memory to save it)\n");
        discard(v, &d->work);
    }
}

/* ---- openings ---- */

static const struct object_ops document_ops;

static struct opening *opening_of(struct handle *h) { return handle_is(h, &document_ops) ? h->object->priv : NULL; }

static int opening_new(struct doc *d, int access, bool user, struct version *ver, struct handle **out) {
    struct opening *op = kzalloc(sizeof *op);
    if (!op) return -N_ERR_NO_MEMORY;
    *op = (struct opening){.doc = d, .access = access, .user = user, .version = ver};
    object_init(&op->object, &document_ops, op);
    if (!(*out = handle_open(&op->object, access != N_DOCUMENT_READ))) {
        kfree(op);
        return -N_ERR_NO_MEMORY;
    }
    return 0;
}

/* a replacement for the document at p: a document of its own in the temp folder until it is saved */
static int open_replacement(struct volume *v, const struct place *p, bool user, struct handle **out) {
    char name[17];
    uint64_t number = ++store.temp_names, id;
    size_t n = store_temp_name(number, name);
    int r = ns_create(v, store.temp, name, n, STORE_DOCUMENT, 0, 0, &id);
    if (r < 0) return r;
    struct doc *d = doc_get(id);
    if (!d) {
        struct ranges none = {0};
        ns_remove(v, store.temp, name, n, &none);
        return -N_ERR_NO_MEMORY;
    }
    d->temp = number;
    d->folder = p->folder;
    d->n = p->n;
    if (!(d->name = kmalloc(p->n))) r = -N_ERR_NO_MEMORY;
    else memcpy(d->name, p->name, p->n);
    if (r >= 0) r = writer_add(v, d);
    if (r >= 0 && (r = opening_new(d, N_DOCUMENT_REPLACE, user, NULL, out)) < 0) d->writers--;
    if (r < 0) doc_put(v, d);
    return r;
}

int document_open(const char *path, int access, bool user, struct handle **out) {
    if (access < N_DOCUMENT_READ || access > N_DOCUMENT_REPLACE) return -N_ERR_INVALID;
    struct volume *v = store.v;
    struct place p;
    mutex_lock(&v->lock);
    int r = place_find(path, user, &p);
    uint64_t id = p.entry.id;
    bool created = false;
    if (r < 0) {
    } else if (access == N_DOCUMENT_CREATE) {
        r = p.found ? -N_ERR_EXISTS : place_may_change(&p, user, false);
        if (r >= 0) r = volume_writable(v);
        if (r >= 0 && (r = ns_create(v, p.folder, p.name, p.n, STORE_DOCUMENT, 0, 0, &id)) >= 0) created = true;
        access = N_DOCUMENT_EDIT;
    } else if (p.found && p.record.kind != STORE_DOCUMENT) {
        r = -N_ERR_IS_FOLDER;
    } else if (access == N_DOCUMENT_REPLACE) {
        if ((r = place_may_change(&p, user, p.found)) >= 0 && (r = volume_writable(v)) >= 0)
            r = open_replacement(v, &p, user, out);
        place_free(&p);
        mutex_unlock(&v->lock);
        if (r >= 0) store_dirty(false);
        return r;
    } else if (!p.found) {
        r = -N_ERR_NOT_FOUND;
    } else if (access == N_DOCUMENT_EDIT) {
        r = place_may_change(&p, user, true);
    }
    place_free(&p);
    struct doc *d = r >= 0 ? doc_get(id) : NULL;
    if (r >= 0 && !d) r = -N_ERR_NO_MEMORY;
    struct version *ver = NULL;
    if (r >= 0) {
        if (created) d->changed_in = v->generation;
        if (access == N_DOCUMENT_READ) r = (ver = version_get(d)) ? 0 : -N_ERR_NO_MEMORY;
        else r = writer_add(v, d);
        if (r >= 0) r = opening_new(d, access, user, ver, out);
        if (r < 0 && ver) version_put(v, d, ver);
        else if (r < 0 && access != N_DOCUMENT_READ && d->writers) writer_remove(v, d);
        if (r < 0) doc_put(v, d);
    }
    mutex_unlock(&v->lock);
    if (created) store_dirty(false);
    return r;
}

int store_document_open(const char *path, int access, struct handle **out) {
    return document_open(path, access, true, out);
}

bool store_document_is(struct handle *h) { return opening_of(h) != NULL; }

int64_t store_document_read(struct handle *h, uint64_t offset, void *buf, size_t n, bool user) {
    struct opening *op = opening_of(h);
    if (!op) return -N_ERR_WRONG_KIND;
    struct volume *v = store.v;
    mutex_lock(&v->lock);
    struct doc *d = op->doc; /* held: the lock is let go meanwhile, and a save may move the opening */
    d->users++;
    int64_t r = op->access == N_DOCUMENT_READ ? version_read(v, d, op->version, offset, buf, n, user, false)
                                               : work_read(v, d, offset, buf, n, user);
    doc_put(v, d);
    mutex_unlock(&v->lock);
    return r;
}

int64_t store_document_write(struct handle *h, uint64_t offset, const void *buf, size_t n, bool user) {
    struct opening *op = opening_of(h);
    if (!op) return -N_ERR_WRONG_KIND;
    if (op->access == N_DOCUMENT_READ) return -N_ERR_DENIED;
    struct volume *v = store.v;
    mutex_lock(&v->lock);
    struct doc *d = op->doc; /* held, as for a read */
    d->users++;
    int64_t r = volume_writable(v);
    if (r >= 0) r = work_write(v, d, offset, buf, n, user);
    doc_put(v, d);
    bool full = v->unwritten_count >= COMMIT_BLOCKS;
    mutex_unlock(&v->lock);
    if (r > 0) store_dirty(false);
    if (full) store_commit(); /* what waits in memory for a commit has its limit: the writer waits for it */
    return r;
}

int64_t store_document_size(struct handle *h) {
    struct opening *op = opening_of(h);
    if (!op) return -N_ERR_WRONG_KIND;
    struct volume *v = store.v;
    mutex_lock(&v->lock);
    struct store_record rec;
    int64_t r = (int64_t)op->doc->work.size;
    if (op->access == N_DOCUMENT_READ && op->version->superseded) r = (int64_t)op->version->size;
    else if (op->access == N_DOCUMENT_READ) r = (r = ns_record(v, op->doc->id, &rec, NULL)) < 0 ? r : (int64_t)rec.size;
    mutex_unlock(&v->lock);
    return r;
}

int store_document_set_size(struct handle *h, uint64_t size) {
    struct opening *op = opening_of(h);
    if (!op) return -N_ERR_WRONG_KIND;
    if (op->access == N_DOCUMENT_READ) return -N_ERR_DENIED;
    struct volume *v = store.v;
    mutex_lock(&v->lock);
    int r = volume_writable(v);
    if (r >= 0) r = work_resize(v, op->doc, size);
    mutex_unlock(&v->lock);
    if (r >= 0) store_dirty(false);
    return r;
}

int store_document_claim(struct handle *h, bool wait) {
    struct opening *op = opening_of(h);
    if (!op) return -N_ERR_WRONG_KIND;
    struct volume *v = store.v;
    mutex_lock(&v->lock);
    int r = 0;
    while (op->doc->owner && op->doc->owner != op && r >= 0) {
        if (!wait) r = -N_ERR_NOT_READY;
        else if (current_task->killed) r = -N_ERR_STOPPED;
        else sleep_on(v, &op->doc->claim_wait);
    }
    if (r >= 0) op->doc->owner = op;
    mutex_unlock(&v->lock);
    return r;
}

static void claim_end(struct doc *d, struct opening *op) {
    if (d->owner != op) return;
    d->owner = NULL;
    wq_wake_all(&d->claim_wait);
}

int store_document_release(struct handle *h) {
    struct opening *op = opening_of(h);
    if (!op) return -N_ERR_WRONG_KIND;
    struct volume *v = store.v;
    mutex_lock(&v->lock);
    int r = op->doc->owner == op ? 0 : -N_ERR_INVALID;
    claim_end(op->doc, op);
    mutex_unlock(&v->lock);
    return r;
}

/* A REPLACE opening's save: its content goes to the name it replaces, and the opening edits the
   document there from now on. */
static int replace(struct volume *v, struct opening *op) {
    struct doc *t = op->doc;
    int r;
    /* reads of its own content end first: its blocks are another document's after this */
    for (;;) {
        if ((r = flush(v, t)) < 0) return r;
        if (!t->superseded && (!t->current || !t->current->refs)) break;
        sleep_on(v, &t->reads_done);
    }
    struct ns_entry e;
    struct store_record rec;
    uint64_t id = 0;
    if ((r = ns_find(v, t->folder, t->name, t->n, &e)) == -N_ERR_NOT_FOUND)
        r = ns_create(v, t->folder, t->name, t->n, STORE_DOCUMENT, 0, 0, &id);
    else if (r >= 0 && (r = ns_record(v, id = e.id, &rec, NULL)) >= 0)
        r = rec.kind != STORE_DOCUMENT ? -N_ERR_IS_FOLDER : op->user && (rec.attributes & STORE_SYSTEM) ? -N_ERR_DENIED : 0;
    if (r < 0) return r;
    struct doc *d = doc_get(id);
    if (!d) return -N_ERR_NO_MEMORY;
    struct ranges dropped = {0};
    if ((r = supersede(v, d)) >= 0) {
        r = ns_take(v, id, t->id, &dropped);
        dropped_to(v, d, &dropped);
    }
    if (r < 0) {
        doc_put(v, d);
        return r;
    }
    d->changed_in = v->generation;
    d->error = 0;
    /* what its writers had not saved is replaced as well */
    if (d->writers) discard(v, &d->work);
    d->writers++;
    work_reset(&d->work, t->work.size);
    work_reset(&t->work, 0);
    t->writers--;
    claim_end(t, op);
    op->doc = d;
    op->access = N_DOCUMENT_EDIT;
    doc_put(v, t);
    return 0;
}

int store_document_commit(struct handle *h, int how) {
    struct opening *op = opening_of(h);
    if (!op) return -N_ERR_WRONG_KIND;
    if (how != N_COMMIT_WAIT && how != N_COMMIT_START && how != N_COMMIT_RESULT) return -N_ERR_INVALID;
    struct volume *v = store.v;
    mutex_lock(&v->lock);
    int r = 0;
    uint64_t g = 0;
    if (how == N_COMMIT_RESULT) {
        if (op->saving && v->durable < op->saving && !v->error) {
            r = -N_ERR_NOT_READY;
        } else if (op->saving) {
            op->outcome = v->durable >= op->saving ? 0 : v->error;
            op->saving = 0;
        }
        mutex_unlock(&v->lock);
        return r < 0 ? r : op->outcome;
    }
    if (op->access != N_DOCUMENT_READ) {
        r = volume_writable(v);
        if (r >= 0) r = op->access == N_DOCUMENT_REPLACE ? replace(v, op) : flush(v, op->doc);
        if (r >= 0) r = op->doc->error;
        g = op->doc->changed_in;
    }
    if (how == N_COMMIT_START) {
        if (r >= 0) {
            /* a save with nothing to wait for has its outcome at once */
            if (!op->saving) {
                op->next_saving = savings;
                savings = op;
            }
            op->saving = MAX(g, 1);
            op->outcome = 0;
        }
        mutex_unlock(&v->lock);
        if (r >= 0) store_dirty(true);
        return r;
    }
    mutex_unlock(&v->lock);
    return r < 0 || !g ? r : store_wait(g);
}

void documents_committed(void) {
    struct volume *v = store.v;
    mutex_lock(&v->lock);
    for (struct opening **at = &savings, *op; (op = *at);) {
        if (v->durable < op->saving && !v->error) {
            at = &op->next_saving;
            continue;
        }
        *at = op->next_saving;
        op->next_saving = NULL;
        object_notify(&op->object);
    }
    mutex_unlock(&v->lock);
}

static uint32_t document_ready(struct handle *h) {
    struct opening *op = h->object->priv;
    uint64_t g = op->saving;
    return g && (store.v->durable >= g || store.v->error) ? N_EVENT_SAVED : 0;
}

static void document_close(struct handle *h) {
    struct opening *op = h->object->priv;
    struct volume *v = store.v;
    mutex_lock(&v->lock);
    struct doc *d = op->doc;
    for (struct opening **at = &savings; *at; at = &(*at)->next_saving)
        if (*at == op) {
            *at = op->next_saving;
            break;
        }
    claim_end(d, op);
    if (op->version) version_put(v, d, op->version);
    if (op->access == N_DOCUMENT_REPLACE) { /* never saved: it goes with its document */
        discard(v, &d->work);
        d->writers--;
    } else if (op->access != N_DOCUMENT_READ) {
        writer_remove(v, d);
    }
    doc_put(v, d);
    op->doc = NULL;
    mutex_unlock(&v->lock);
    store_dirty(false);
}

static void document_release(struct object *o) { kfree(o->priv); }

static const struct object_ops document_ops = {
    .kind = N_KIND_DOCUMENT,
    .ready = document_ready,
    .close = document_close,
    .release = document_release,
};

/* ---- for the rest of the store ---- */

int64_t document_size_now(uint64_t id) {
    struct doc *d = doc_find(id);
    return d && d->writers ? (int64_t)d->work.size : -N_ERR_NOT_FOUND;
}

int document_hide(uint64_t folder, const char *name, size_t n, uint64_t id) {
    struct doc *d = doc_find(id);
    if (!d) return 0;
    char temp[17];
    uint64_t number = ++store.temp_names;
    size_t k = store_temp_name(number, temp);
    int r = ns_move(store.v, folder, name, n, store.temp, temp, k);
    if (r < 0) return r;
    d->temp = number;
    d->changed_in = store.v->generation;
    return 1;
}

void document_changed(uint64_t id) {
    struct doc *d = doc_find(id);
    if (d) d->changed_in = store.v->generation;
}

int documents_flush(void) {
    int r = 0;
    for (unsigned b = 0; b < BUCKETS; b++)
        for (struct doc *d = docs[b]; d; d = d->next) {
            int e = flush(store.v, d);
            if (e < 0 && r >= 0) r = e;
        }
    return r;
}
