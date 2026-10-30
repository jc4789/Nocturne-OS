/* Open documents (fs/store.h, docs/nocturne-native-abi.md §3).

   What is written to a document waits in memory as pages over the content the catalog has: a work,
   in ns_update's terms (the size, the block from which the catalog's content is gone, the pages).
   The EDIT openings of a document share its work. A REPLACE opening writes a document of its own in
   the temp folder, and its save puts that document at its name, or swaps its content in whole for
   the content of the one there (ns_take); the opening edits that document from then on. A work goes
   into the catalog when an opening saves, when it holds WORK_PAGES, when its last writer closes,
   and within seconds by the store's writer (the list of works); a save then waits for the commit
   that holds it.

   A READ opening reads the content the catalog had when it was opened: a version. Before the
   catalog's content changes while a version is read, the version is written down (its extents),
   and the blocks the change drops are held until no version that reads them is left: each version
   keeps what the change after it dropped, and passes it on to the next older one still read when
   it goes.

   Each document's lock (a mutex, fs/document.h's order) guards everything of its struct doc and
   its openings, and keeps the changes of its content one at a time. A read lets go of it while
   blocks come from the disk: its version reference keeps them meanwhile. The table of documents
   has a spinlock of its own: the users of each, and which document each opening is on. */
#include "fs/document.h"
#include "fs/watch.h"
#include "store/space.h"
#include "lib/idtable.h"
#include "mm/heap.h"
#include "sys/object.h"

#define WORK_PAGES 4096u     /* 16 MiB of pages: then the work goes into the catalog */
#define COMMIT_BLOCKS 16384u /* 64 MiB written and not yet committed: then a writer commits */
#define YIELD_PAGES 64u      /* pages written before the CPU is offered to others */
#define READ_BLOCKS 64u
#define NO_SIZE UINT64_MAX

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
    uint64_t size, cut;    /* the content is size bytes; the catalog's is gone from block cut on */
    uint64_t base;         /* the catalog's size */
    struct ns_page *pages; /* by index, each reserved on C:; bytes after size are zeros */
    size_t n, room;
    bool changed;
};

struct opening;

struct doc {
    struct id_link link;        /* by its id */
    unsigned users;             /* openings, calls under way and the list of works (the table's lock) */
    uint64_t size_now;          /* work.size while it has writers, else NO_SIZE (atomic) */
    struct mutex lock;          /* everything below */
    unsigned writers;           /* openings that write to the work */
    struct work work;
    bool listed;                /* on the list of works (which holds it) */
    struct doc *next_work;
    struct version *current;    /* the catalog's content, while something reads it */
    struct version *superseded; /* newest first */
    struct opening *owner;      /* the claim's */
    struct wait_queue claim_wait, reads_done;
    uint64_t changed_in;        /* the generation with its last change */
    int error;                  /* written content was lost: every later save reports it */
    uint64_t temp;              /* its name in the temp folder (0: none): removed with its last user */
    bool removed;               /* its entry and record are gone: nothing opens it any more */
    uint64_t folder;            /* a replacement's: the folder and name it is saved to */
    char *name;
    size_t n;
};

struct opening {
    struct object object;
    struct doc *doc;          /* the table's lock; NULL once closed */
    int access;               /* N_DOCUMENT_READ, _EDIT, or _REPLACE until its replacement is saved */
    bool user;                /* an app's: no SYSTEM document is replaced */
    struct version *version;  /* READ's */
    uint64_t saving;          /* the generation an N_COMMIT_START save waits for (0: none; savings' lock) */
    int outcome;              /* the last save's, once known */
    struct opening *next_saving;
};

static struct {
    struct spinlock lock;
    struct id_table table;
} docs;

/* Documents whose works changed since they last went into the catalog. */
static struct {
    struct spinlock lock;
    struct doc *first;
} works;

/* N_COMMIT_START saves whose outcome is not told yet. */
static struct {
    struct spinlock lock;
    struct opening *first;
} savings;

static const uint8_t zeros[STORE_BLOCK];

static uint64_t blocks_of(uint64_t size) { return (size + STORE_BLOCK - 1) / STORE_BLOCK; }

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
        if (ranges_add(store.v->env, &ver->dropped, r->r[i].start, r->r[i].count) < 0) {
            kprintf("store: no memory to track freed blocks: they stay in use until the next start\n");
            break;
        }
    ranges_free(store.v->env, r);
}

/* ---- documents ---- */

static struct doc *doc_of(struct id_link *l) { return (struct doc *)((char *)l - offsetof(struct doc, link)); }

/* document id, held: its struct made when nothing holds it yet */
static struct doc *doc_get(uint64_t id) {
    uint64_t flags = spin_lock(&docs.lock);
    struct id_link *l = id_table_find(&docs.table, id);
    if (l) doc_of(l)->users++;
    spin_unlock(&docs.lock, flags);
    if (l) return doc_of(l);
    struct doc *fresh = kzalloc(sizeof *fresh);
    if (!fresh) return NULL;
    fresh->link.id = id;
    fresh->users = 1;
    fresh->size_now = NO_SIZE;
    if (id_table_room(&docs.table, &docs.lock, &flags) < 0) {
        spin_unlock(&docs.lock, flags);
        kfree(fresh);
        return NULL;
    }
    struct doc *d = fresh;
    if ((l = id_table_find(&docs.table, id))) { /* made meanwhile */
        d = doc_of(l);
        d->users++;
    } else {
        id_table_add(&docs.table, &fresh->link);
        fresh = NULL;
    }
    spin_unlock(&docs.lock, flags);
    kfree(fresh);
    return d;
}

/* document id, held, when something holds it already */
static struct doc *doc_find(uint64_t id) {
    uint64_t flags = spin_lock(&docs.lock);
    struct id_link *l = id_table_find(&docs.table, id);
    if (l) doc_of(l)->users++;
    spin_unlock(&docs.lock, flags);
    return l ? doc_of(l) : NULL;
}

static unsigned doc_users(struct doc *d) {
    uint64_t flags = spin_lock(&docs.lock);
    unsigned n = d->users;
    spin_unlock(&docs.lock, flags);
    return n;
}

static void discard(struct work *w) {
    for (size_t i = 0; i < w->n; i++) kfree(w->pages[i].data);
    space_unreserve(store.v, w->n);
    kfree(w->pages);
    w->pages = NULL;
    w->n = w->room = 0;
    w->changed = false;
}

/* The document named number in the temp folder goes, nothing having it open: the temp folder
   locked. */
static int temp_remove(uint64_t number) {
    char name[17];
    size_t n = store_temp_text(number, name);
    struct ranges dropped = {0};
    change_begin();
    int r = ns_remove(store.v, store.temp, name, n, store_now(), &dropped, NULL);
    change_end();
    space_release_list(store.v, &dropped);
    if (r < 0) kprintf("store: a document stays in C:\\Nocturne\\Temp until the next start (%d)\n", r);
    else store_dirty(false);
    return r;
}

/* One user less, with no document's lock held. The last user of a document only the temp folder
   names removes it from there first, unless another finds it meanwhile. */
static void doc_put(struct doc *d) {
    for (;;) {
        uint64_t flags = spin_lock(&docs.lock);
        if (d->users > 1 || !d->temp || d->removed) {
            bool last = !--d->users;
            if (last) id_table_drop(&docs.table, &d->link);
            spin_unlock(&docs.lock, flags);
            if (!last) return;
            discard(&d->work); /* a write that raced its opening's save leaves pages nothing saves */
            kfree(d->name);
            kfree(d);
            return;
        }
        spin_unlock(&docs.lock, flags);
        struct folder_lock *t = folder_lock(store.temp);
        mutex_lock(&d->lock);
        if (doc_users(d) == 1 && !d->removed) {
            if (t) temp_remove(d->temp);
            else kprintf("store: a document stays in C:\\Nocturne\\Temp until the next start (no memory)\n");
            d->removed = true;
        }
        mutex_unlock(&d->lock);
        if (t) folder_unlock(t);
    }
}

/* what entry_fill shows of d from now on */
static void size_tell(struct doc *d) { __atomic_store_n(&d->size_now, d->writers ? d->work.size : NO_SIZE, __ATOMIC_RELEASE); }

/* d's work changed: the writer flushes it */
static void work_listed(struct doc *d) {
    if (d->listed) return;
    uint64_t flags = spin_lock(&docs.lock);
    d->users++;
    spin_unlock(&docs.lock, flags);
    flags = spin_lock(&works.lock);
    d->listed = true;
    d->next_work = works.first;
    works.first = d;
    spin_unlock(&works.lock, flags);
}

/* ---- versions ---- */

/* the catalog's content now, held */
static struct version *version_get(struct doc *d) {
    if (!d->current && !(d->current = kzalloc(sizeof *d->current))) return NULL;
    d->current->refs++;
    return d->current;
}

static void version_put(struct doc *d, struct version *ver) {
    if (--ver->refs) return;
    if (!ver->superseded) {
        d->current = NULL;
    } else {
        struct version **at = &d->superseded;
        while (*at != ver) at = &(*at)->older;
        *at = ver->older;
        if (ver->older) keep(ver->older, &ver->dropped);
        else space_release_list(store.v, &ver->dropped);
        kfree(ver->content);
        kfree(ver->extents);
    }
    kfree(ver);
    wq_wake_all(&d->reads_done);
}

/* d's content in the catalog is about to change: a version still read is written down first */
static int supersede(struct doc *d) {
    struct volume *v = store.v;
    struct version *ver = d->current;
    if (!ver) return 0;
    struct store_record rec;
    uint8_t *content = kmalloc(STORE_INLINE_MAX);
    int r = content ? ns_record(v, d->link.id, &rec, content) : -N_ERR_NO_MEMORY;
    if (r >= 0 && (rec.flags & STORE_INLINE)) {
        ver->content = content;
        content = NULL;
    } else if (r >= 0) {
        r = ns_extents(v, d->link.id, &ver->extents, &ver->n);
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
static void dropped_to(struct doc *d, struct ranges *dropped) {
    if (d->superseded) keep(d->superseded, dropped);
    else space_release_list(store.v, dropped);
}

/* where block index of ver is (ns_extent's terms) */
static int version_extent(struct doc *d, struct version *ver, uint64_t index, struct ns_extent *e) {
    if (!ver->superseded) return ns_extent(store.v, d->link.id, index, e);
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

/* n bytes of ver from offset, up to its end, into buf (an app's memory when user): how many. d's
   lock is let go while blocks come from the disk, unless hold. */
static int64_t version_read(struct doc *d, struct version *ver, uint64_t offset, uint8_t *buf, size_t n, bool user,
                            bool hold) {
    struct volume *v = store.v;
    size_t done = 0;
    int r = 0;
    while (done < n) {
        uint64_t at = offset + done, size = ver->size;
        bool small = ver->content;
        struct store_record rec;
        if (!ver->superseded) {
            int got = ns_record(v, d->link.id, &rec, NULL); /* the inline content's length: not a result */
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
                r = mine ? ns_record(v, d->link.id, &rec, mine) : -N_ERR_NO_MEMORY;
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
        if ((r = version_extent(d, ver, index, &e)) < 0) break;
        uint64_t blocks = MIN(e.index + e.count - index, blocks_of(skip + want));
        if (!e.block) { /* a hole */
            size_t k = (size_t)MIN(blocks * STORE_BLOCK - skip, (uint64_t)want);
            if ((r = zero_out(buf + done, k, user)) < 0) break;
            done += k;
            continue;
        }
        uint32_t count = (uint32_t)MIN(blocks, (uint64_t)READ_BLOCKS);
        struct store_block *got[READ_BLOCKS];
        if (!hold) {
            ver->refs++;
            mutex_unlock(&d->lock);
        }
        bool read = (r = ns_extent_read(v, &e, index - e.index, count, got)) >= 0;
        for (uint32_t i = 0; r >= 0 && i < count; i++) {
            size_t from = i ? 0 : skip, k = MIN(STORE_BLOCK - from, want);
            if ((r = copy_out(buf + done, got[i]->data + from, k, user)) >= 0) done += k, want -= k;
        }
        for (uint32_t i = 0; read && i < count; i++) store.blocks->put(store.blocks, got[i]);
        if (!hold) {
            mutex_lock(&d->lock);
            version_put(d, ver);
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
static int page_new(struct doc *d, uint64_t index, const uint8_t *src, size_t skip, size_t k, bool user,
                    struct ns_page **out) {
    struct volume *v = store.v;
    struct work *w = &d->work;
    int r = space_reserve(v, 1);
    if (r < 0) return r;
    uint8_t *data = kzalloc(STORE_BLOCK);
    if (!data) r = -N_ERR_NO_MEMORY;
    uint64_t at = index * STORE_BLOCK, old = kept(w);
    if (r >= 0 && k < STORE_BLOCK && at < old) {
        struct version *ver = version_get(d);
        int64_t got = ver ? version_read(d, ver, at, data, (size_t)MIN((uint64_t)STORE_BLOCK, old - at), false, true)
                          : -N_ERR_NO_MEMORY;
        if (ver) version_put(d, ver);
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
static int grow(struct doc *d, uint64_t size) {
    struct work *w = &d->work;
    if (size <= STORE_INLINE_MAX || !w->base || w->base > STORE_INLINE_MAX || !w->cut || page_find(w, 0)) return 0;
    return page_new(d, 0, NULL, 0, 0, false, NULL);
}

/* the work as the catalog's content of size bytes, nothing written over it */
static void work_reset(struct work *w, uint64_t size) {
    w->size = w->base = size;
    w->cut = blocks_of(size);
}

/* the work into the catalog: what was written is the saved content from now on */
static int flush(struct doc *d) {
    struct volume *v = store.v;
    struct work *w = &d->work;
    if (!w->changed) return 0;
    int r = supersede(d);
    if (r < 0) return r; /* the work stays, for a later try */
    struct ranges dropped = {0};
    struct ns_told told = {0};
    uint64_t g = change_begin();
    r = ns_update(v, d->link.id, w->size, w->cut, w->pages, w->n, true, store_now(), &dropped, &told);
    change_end();
    entry_watch_publish(&told);
    kfree(w->pages);
    w->pages = NULL;
    w->n = w->room = 0;
    w->changed = false;
    dropped_to(d, &dropped);
    d->changed_in = g;
    uint64_t size = w->size;
    if (r < 0) {
        struct store_record rec;
        size = ns_record(v, d->link.id, &rec, NULL) >= 0 ? rec.size : 0;
        kprintf("store: what was written to a document is lost (%d)\n", r);
        d->error = r;
    }
    work_reset(w, size);
    return r;
}

static int64_t work_read(struct doc *d, uint64_t offset, uint8_t *buf, size_t n, bool user) {
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
                int64_t got = ver ? version_read(d, ver, at, buf + done, k, user, false) : -N_ERR_NO_MEMORY;
                if (ver) version_put(d, ver);
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

static int64_t work_write(struct doc *d, uint64_t offset, const uint8_t *buf, size_t n, bool user) {
    struct work *w = &d->work;
    if (offset + n < offset || offset + n > INT64_MAX) return -N_ERR_TOO_LARGE;
    int r = grow(d, offset + n);
    size_t done = 0;
    for (unsigned pages = 1; r >= 0 && done < n; pages++) {
        uint64_t at = offset + done, index = at / STORE_BLOCK;
        size_t skip = (size_t)(at % STORE_BLOCK), k = MIN(STORE_BLOCK - skip, n - done);
        struct ns_page *p = page_find(w, index);
        r = p ? copy_in(p->data + skip, buf + done, k, user) : page_new(d, index, buf + done, skip, k, user, NULL);
        if (r < 0) break;
        done += k;
        w->size = MAX(w->size, at + k);
        w->changed = true;
        work_listed(d);
        if (w->n >= WORK_PAGES) r = flush(d);
        if (pages % YIELD_PAGES == 0) {
            if (user && current_task->killed) r = -N_ERR_STOPPED;
            if (need_resched) schedule();
        }
    }
    return done ? (int64_t)done : r;
}

static int work_resize(struct doc *d, uint64_t size) {
    struct work *w = &d->work;
    if (size > INT64_MAX) return -N_ERR_TOO_LARGE;
    int r = grow(d, size);
    if (r >= 0 && size < w->size) {
        uint64_t last = size / STORE_BLOCK;
        size_t tail = (size_t)(size % STORE_BLOCK);
        /* zeros after the new end in its last block: on a page of its own when the catalog has it */
        struct ns_page *p = tail ? page_find(w, last) : NULL;
        if (tail && !p && last * STORE_BLOCK < kept(w)) r = page_new(d, last, NULL, 0, 0, false, &p);
        if (r < 0) return r;
        if (p) memset(p->data + tail, 0, STORE_BLOCK - tail);
        size_t from = page_at(w, blocks_of(size));
        for (size_t i = from; i < w->n; i++) kfree(w->pages[i].data);
        space_unreserve(store.v, w->n - from);
        w->n = from;
        w->cut = MIN(w->cut, blocks_of(size));
    }
    if (r >= 0) {
        w->size = size;
        w->changed = true;
        work_listed(d);
    }
    return r;
}

/* one more opening writes d: the work starts from the catalog's content with the first */
static int writer_add(struct doc *d) {
    if (d->writers) {
        d->writers++;
        return 0;
    }
    struct store_record rec;
    int r = ns_record(store.v, d->link.id, &rec, NULL);
    if (r < 0) return r;
    d->writers = 1;
    work_reset(&d->work, rec.size);
    return 0;
}

static void writer_remove(struct doc *d) {
    if (--d->writers) return;
    if (flush(d) < 0 && d->work.changed) {
        kprintf("store: what was written to a document is lost (no memory to save it)\n");
        discard(&d->work);
    }
}

/* ---- openings ---- */

static const struct object_ops document_ops;

static struct opening *opening_of(struct handle *h) { return handle_is(h, &document_ops) ? h->object->priv : NULL; }

/* op's document, held and locked: a save may move the opening to another one meanwhile. NULL once
   it is closed. */
static struct doc *opening_lock(struct opening *op) {
    for (;;) {
        uint64_t flags = spin_lock(&docs.lock);
        struct doc *d = op->doc;
        if (d) d->users++;
        spin_unlock(&docs.lock, flags);
        if (!d) return NULL;
        mutex_lock(&d->lock);
        if (op->doc == d) return d;
        mutex_unlock(&d->lock);
        doc_put(d);
    }
}

static void opening_unlock(struct doc *d) {
    mutex_unlock(&d->lock);
    doc_put(d);
}

/* the opening goes to d (held for it), the document it was on still locked */
static void opening_move(struct opening *op, struct doc *d) {
    uint64_t flags = spin_lock(&docs.lock);
    op->doc = d;
    spin_unlock(&docs.lock, flags);
}

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
static int open_replacement(const struct place *p, bool user, struct handle **out) {
    struct volume *v = store.v;
    char name[17];
    size_t n;
    uint64_t number = store_temp_name(name, &n), id;
    struct folder_lock *t = folder_lock(store.temp);
    if (!t) return -N_ERR_NO_MEMORY;
    change_begin();
    int r = ns_create(v, store.temp, name, n, STORE_DOCUMENT, 0, 0, store_now(), &id, NULL);
    change_end();
    struct doc *d = r >= 0 ? doc_get(id) : NULL;
    if (r >= 0 && !d) {
        temp_remove(number);
        r = -N_ERR_NO_MEMORY;
    }
    folder_unlock(t);
    if (r < 0) return r;
    mutex_lock(&d->lock);
    d->temp = number;
    d->folder = p->folder;
    d->n = p->n;
    if (!(d->name = kmalloc(p->n))) r = -N_ERR_NO_MEMORY;
    else memcpy(d->name, p->name, p->n);
    bool writing = r >= 0 && (r = writer_add(d)) >= 0;
    if (r >= 0) r = opening_new(d, N_DOCUMENT_REPLACE, user, NULL, out);
    if (r < 0 && writing) d->writers--;
    size_tell(d);
    mutex_unlock(&d->lock);
    if (r < 0) doc_put(d);
    return r;
}

int document_open(const char *path, int access, bool user, struct handle **out) {
    if (access < N_DOCUMENT_READ || access > N_DOCUMENT_REPLACE) return -N_ERR_INVALID;
    struct volume *v = store.v;
    struct place p;
    int r = place_find(path, user, &p);
    if (r < 0) return r;
    uint64_t id = p.entry.id, g = 0;
    bool created = false;
    if (access == N_DOCUMENT_CREATE) {
        struct folder_lock *f = NULL;
        if ((r = place_lock(&p, user, &f)) >= 0) {
            r = p.found ? -N_ERR_EXISTS : place_may_change(&p, user, false);
            if (r >= 0) r = volume_writable(v);
            if (r >= 0) {
                struct ns_told told = {0};
                g = change_begin();
                r = ns_create(v, p.folder, p.name, p.n, STORE_DOCUMENT, 0, 0, store_now(), &id, &told);
                change_end();
                entry_watch_publish(&told);
                created = r >= 0;
            }
            folder_unlock(f);
        }
        access = N_DOCUMENT_EDIT;
    } else if (p.found && p.record.kind != STORE_DOCUMENT) {
        r = -N_ERR_IS_FOLDER;
    } else if (access == N_DOCUMENT_REPLACE) {
        if ((r = place_may_change(&p, user, p.found)) >= 0 && (r = volume_writable(v)) >= 0)
            r = open_replacement(&p, user, out);
        place_free(&p);
        if (r >= 0) store_dirty(false);
        return r;
    } else if (!p.found) {
        r = -N_ERR_NOT_FOUND;
    } else if (access == N_DOCUMENT_EDIT) {
        r = place_may_change(&p, user, true);
    }
    place_free(&p);
    if (r < 0) return r;
    struct doc *d = doc_get(id);
    if (!d) return -N_ERR_NO_MEMORY;
    mutex_lock(&d->lock);
    /* it was found without a lock: it may have gone since */
    struct store_record rec;
    r = d->removed ? -N_ERR_NOT_FOUND : ns_record(v, id, &rec, NULL);
    struct version *ver = NULL;
    bool writing = false;
    if (r >= 0) {
        if (created) d->changed_in = MAX(d->changed_in, g);
        if (access == N_DOCUMENT_READ) r = (ver = version_get(d)) ? 0 : -N_ERR_NO_MEMORY;
        else writing = (r = writer_add(d)) >= 0;
        if (r >= 0) r = opening_new(d, access, user, ver, out);
        if (r < 0 && ver) version_put(d, ver);
        if (r < 0 && writing) writer_remove(d);
    }
    size_tell(d);
    mutex_unlock(&d->lock);
    if (r < 0) doc_put(d);
    if (created) store_dirty(false);
    return r;
}

int store_document_open(const char *path, int access, bool user, struct handle **out) {
    return document_open(path, access, user, out);
}

bool store_document_is(struct handle *h) { return opening_of(h) != NULL; }

int64_t store_document_read(struct handle *h, uint64_t offset, void *buf, size_t n, bool user) {
    struct opening *op = opening_of(h);
    if (!op) return -N_ERR_WRONG_KIND;
    struct doc *d = opening_lock(op);
    if (!d) return -N_ERR_CLOSED;
    int64_t r = op->access == N_DOCUMENT_READ ? version_read(d, op->version, offset, buf, n, user, false)
                                               : work_read(d, offset, buf, n, user);
    opening_unlock(d);
    return r;
}

int64_t store_document_write(struct handle *h, uint64_t offset, const void *buf, size_t n, bool user) {
    struct opening *op = opening_of(h);
    if (!op) return -N_ERR_WRONG_KIND;
    if (op->access == N_DOCUMENT_READ) return -N_ERR_DENIED;
    struct volume *v = store.v;
    struct doc *d = opening_lock(op);
    if (!d) return -N_ERR_CLOSED;
    int64_t r = volume_writable(v);
    if (r >= 0) r = work_write(d, offset, buf, n, user);
    size_tell(d);
    opening_unlock(d);
    if (r > 0) store_dirty(false);
    /* what waits in memory for a commit has its limit: the writer waits for it */
    if (volume_unwritten(v) >= COMMIT_BLOCKS) store_commit();
    return r;
}

/* The size op reads: a READ opening keeps the version it opened, the catalog's until a save
   supersedes it. rec: its document's record, read here when NULL. */
static int64_t opening_size(const struct opening *op, struct doc *d, const struct store_record *rec) {
    if (op->access != N_DOCUMENT_READ) return (int64_t)d->work.size;
    if (op->version->superseded) return (int64_t)op->version->size;
    struct store_record own;
    if (!rec) {
        int r = ns_record(store.v, d->link.id, &own, NULL);
        if (r < 0) return r;
        rec = &own;
    }
    return (int64_t)rec->size;
}

int64_t store_document_size(struct handle *h) {
    struct opening *op = opening_of(h);
    if (!op) return -N_ERR_WRONG_KIND;
    struct doc *d = opening_lock(op);
    if (!d) return -N_ERR_CLOSED;
    int64_t r = opening_size(op, d, NULL);
    opening_unlock(d);
    return r;
}

int store_document_info(struct handle *h, struct n_entry *out) {
    struct opening *op = opening_of(h);
    if (!op) return -N_ERR_WRONG_KIND;
    struct doc *d = opening_lock(op);
    if (!d) return -N_ERR_CLOSED;
    struct store_record rec;
    int r = ns_record(store.v, d->link.id, &rec, NULL);
    if (r >= 0) {
        entry_fill(out, NULL, 0, d->link.id, &rec);
        out->size = (uint64_t)opening_size(op, d, &rec);
    }
    opening_unlock(d);
    return r < 0 ? r : 0;
}

/* A change writes the content it changes into new blocks, each with the CRC-32C of its bytes, so
   the size, the blocks and their checks tell one content of a document from the others. */
int store_document_identity(struct handle *h, void **out, size_t *n) {
    struct opening *op = opening_of(h);
    if (!op) return -N_ERR_WRONG_KIND;
    if (op->access != N_DOCUMENT_READ) return -N_ERR_UNSUPPORTED; /* a work changes under it */
    struct volume *v = store.v;
    struct doc *d = opening_lock(op);
    if (!d) return -N_ERR_CLOSED;
    struct version *ver = op->version;
    struct ns_extent *list = ver->extents;
    size_t count = ver->n;
    uint64_t size = ver->size;
    int r = ver->content ? -N_ERR_UNSUPPORTED : 0;
    if (!ver->superseded) {
        struct store_record rec;
        list = NULL;
        if ((r = ns_record(v, d->link.id, &rec, NULL)) >= 0) {
            size = rec.size;
            r = rec.flags & STORE_INLINE ? -N_ERR_UNSUPPORTED : ns_extents(v, d->link.id, &list, &count);
        }
    }
    size_t bytes = sizeof size;
    for (size_t i = 0; r >= 0 && i < count; i++) {
        if (!list[i].checked) r = -N_ERR_UNSUPPORTED; /* a raw document's blocks change in place */
        bytes += 3 * sizeof(uint64_t) + 4 * list[i].count;
    }
    uint8_t *id = r >= 0 ? kmalloc(bytes) : NULL;
    if (r >= 0 && !id) r = -N_ERR_NO_MEMORY;
    if (r >= 0) {
        uint8_t *at = id;
        memcpy(at, &size, sizeof size);
        at += sizeof size;
        for (size_t i = 0; i < count; i++) {
            uint64_t place[3] = {list[i].index, list[i].count, list[i].block};
            memcpy(at, place, sizeof place);
            memcpy(at + sizeof place, list[i].check, 4 * list[i].count);
            at += sizeof place + 4 * list[i].count;
        }
        *out = id;
        *n = bytes;
    }
    if (!ver->superseded) kfree(list);
    opening_unlock(d);
    return r < 0 ? r : 0;
}

int store_document_set_size(struct handle *h, uint64_t size) {
    struct opening *op = opening_of(h);
    if (!op) return -N_ERR_WRONG_KIND;
    if (op->access == N_DOCUMENT_READ) return -N_ERR_DENIED;
    struct doc *d = opening_lock(op);
    if (!d) return -N_ERR_CLOSED;
    int r = volume_writable(store.v);
    if (r >= 0) r = work_resize(d, size);
    size_tell(d);
    opening_unlock(d);
    if (r >= 0) store_dirty(false);
    return r;
}

int store_document_claim(struct handle *h, bool wait) {
    struct opening *op = opening_of(h);
    if (!op) return -N_ERR_WRONG_KIND;
    for (;;) {
        struct doc *d = opening_lock(op);
        if (!d) return -N_ERR_CLOSED;
        int r = 0;
        while (d->owner && d->owner != op && op->doc == d) {
            if (!wait) r = -N_ERR_NOT_READY;
            else if (current_task->killed) r = -N_ERR_STOPPED;
            if (r < 0) break;
            mutex_wait(&d->lock, &d->claim_wait);
        }
        bool moved = op->doc != d; /* a save moved the opening meanwhile: its claim is on the other one */
        if (!moved && r >= 0) d->owner = op;
        opening_unlock(d);
        if (!moved || r < 0) return r;
    }
}

static void claim_end(struct doc *d, struct opening *op) {
    if (d->owner != op) return;
    d->owner = NULL;
    wq_wake_all(&d->claim_wait);
}

int store_document_release(struct handle *h) {
    struct opening *op = opening_of(h);
    if (!op) return -N_ERR_WRONG_KIND;
    struct doc *d = opening_lock(op);
    if (!d) return -N_ERR_CLOSED;
    int r = d->owner == op ? 0 : -N_ERR_INVALID;
    claim_end(d, op);
    opening_unlock(d);
    return r;
}

/* A REPLACE opening's save, its own document t locked: t goes to the name it replaces when nothing
   is there; else the document there takes t's content in whole, and the opening edits that one from
   now on (*moved: t's hold for it is the caller's to put). Either is one change to whatever watches
   the name; *g its generation. */
static int replace(struct opening *op, struct doc *t, uint64_t *g, bool *moved) {
    struct volume *v = store.v;
    int r;
    for (;;) {
        if ((r = flush(t)) < 0) return r;
        /* reads of its own content end first: its blocks may be another document's after this */
        if (!t->superseded && (!t->current || !t->current->refs)) break;
        mutex_wait(&t->lock, &t->reads_done);
    }
    struct folder_lock *f = folder_lock(t->folder);
    if (!f) return -N_ERR_NO_MEMORY;
    struct store_record rec;
    struct ns_entry *e = kmalloc(sizeof *e);
    r = e ? ns_record(v, t->folder, &rec, NULL) : -N_ERR_NO_MEMORY;
    if (r >= 0 && rec.kind != STORE_FOLDER) r = -N_ERR_NOT_FOUND; /* the folder went meanwhile */
    if (r >= 0) r = ns_find(v, t->folder, t->name, t->n, e);
    struct ns_told told = {0};
    if (r == -N_ERR_NOT_FOUND) {
        /* nothing at the name: the replacement itself goes there */
        struct folder_lock *tl = folder_lock(store.temp);
        char temp[17];
        size_t k = store_temp_text(t->temp, temp);
        r = tl ? 0 : -N_ERR_NO_MEMORY;
        if (r >= 0) {
            *g = change_begin();
            r = ns_move(v, store.temp, temp, k, t->folder, t->name, t->n, store_now(), &told);
            change_end();
            if (r >= 0) {
                t->temp = 0;
                t->changed_in = *g;
            }
            folder_unlock(tl);
        }
        entry_watch_publish(&told);
        folder_unlock(f);
        kfree(e);
        if (r < 0) return r;
        t->error = 0;
        claim_end(t, op);
        op->access = N_DOCUMENT_EDIT;
        return 0;
    }
    uint64_t id = r >= 0 ? e->id : 0;
    kfree(e);
    if (r >= 0 && (r = ns_record(v, id, &rec, NULL)) >= 0)
        r = rec.kind != STORE_DOCUMENT ? -N_ERR_IS_FOLDER : op->user && (rec.attributes & STORE_SYSTEM) ? -N_ERR_DENIED : 0;
    struct doc *d = r >= 0 ? doc_get(id) : NULL;
    if (r >= 0 && !d) r = -N_ERR_NO_MEMORY;
    if (r < 0) {
        folder_unlock(f);
        return r;
    }
    mutex_lock(&d->lock);
    struct ranges dropped = {0};
    if (!(r = d->removed ? -N_ERR_NOT_FOUND : 0) && (r = supersede(d)) >= 0) {
        *g = change_begin();
        r = ns_take(v, id, t->link.id, store_now(), &dropped, &told);
        change_end();
        dropped_to(d, &dropped);
        if (r >= 0) d->changed_in = *g;
    }
    entry_watch_publish(&told);
    folder_unlock(f);
    if (r < 0) {
        /* a folder names d: putting it takes no lock (doc_put), though t is still locked */
        mutex_unlock(&d->lock);
        doc_put(d);
        return r;
    }
    d->error = 0;
    /* what its writers had not saved is replaced as well */
    if (d->writers) discard(&d->work);
    d->writers++;
    work_reset(&d->work, t->work.size);
    size_tell(d);
    work_reset(&t->work, 0);
    t->writers--;
    claim_end(t, op);
    op->access = N_DOCUMENT_EDIT;
    opening_move(op, d);
    *moved = true;
    mutex_unlock(&d->lock);
    return 0;
}

/* op's N_COMMIT_START save waits for generation g from now on */
static void saving_start(struct opening *op, uint64_t g) {
    uint64_t flags = spin_lock(&savings.lock);
    /* a save with nothing to wait for has its outcome at once */
    if (!op->saving) {
        op->next_saving = savings.first;
        savings.first = op;
    }
    __atomic_store_n(&op->saving, MAX(g, 1), __ATOMIC_RELEASE);
    op->outcome = 0;
    spin_unlock(&savings.lock, flags);
}

int store_document_commit(struct handle *h, int how) {
    struct opening *op = opening_of(h);
    if (!op) return -N_ERR_WRONG_KIND;
    if (how != N_COMMIT_WAIT && how != N_COMMIT_START && how != N_COMMIT_RESULT) return -N_ERR_INVALID;
    struct volume *v = store.v;
    int r = 0;
    if (how == N_COMMIT_RESULT) {
        uint64_t flags = spin_lock(&savings.lock);
        uint64_t durable = __atomic_load_n(&v->durable, __ATOMIC_ACQUIRE);
        if (op->saving && durable < op->saving && volume_writable(v) >= 0) {
            r = -N_ERR_NOT_READY;
        } else if (op->saving) {
            op->outcome = durable >= op->saving ? 0 : volume_writable(v);
            __atomic_store_n(&op->saving, 0, __ATOMIC_RELEASE);
        }
        int outcome = op->outcome;
        spin_unlock(&savings.lock, flags);
        return r < 0 ? r : outcome;
    }
    uint64_t g = 0;
    if (op->access != N_DOCUMENT_READ) {
        struct doc *d = opening_lock(op);
        if (!d) return -N_ERR_CLOSED;
        bool moved = false;
        r = volume_writable(v);
        if (r >= 0 && op->access == N_DOCUMENT_REPLACE) {
            r = replace(op, d, &g, &moved);
        } else if (r >= 0) {
            r = flush(d);
            if (r >= 0) r = d->error;
            g = d->changed_in;
        }
        opening_unlock(d);
        if (moved) doc_put(d); /* the opening's hold on its own document */
    }
    if (how == N_COMMIT_START) {
        if (r >= 0) {
            saving_start(op, g);
            store_dirty(true);
        }
        return r;
    }
    return r < 0 || !g ? r : store_wait(g);
}

void documents_committed(void) {
    struct volume *v = store.v;
    uint64_t durable = __atomic_load_n(&v->durable, __ATOMIC_ACQUIRE);
    bool failed = volume_writable(v) < 0;
    uint64_t flags = spin_lock(&savings.lock);
    for (struct opening **at = &savings.first, *op; (op = *at);) {
        if (durable < op->saving && !failed) {
            at = &op->next_saving;
            continue;
        }
        *at = op->next_saving;
        op->next_saving = NULL;
        object_notify(&op->object);
    }
    spin_unlock(&savings.lock, flags);
}

static uint32_t document_ready(struct handle *h) {
    struct opening *op = h->object->priv;
    uint64_t g = __atomic_load_n(&op->saving, __ATOMIC_ACQUIRE);
    return g && (__atomic_load_n(&store.v->durable, __ATOMIC_ACQUIRE) >= g || volume_writable(store.v) < 0) ? N_EVENT_SAVED : 0;
}

static void document_close(struct handle *h) {
    struct opening *op = h->object->priv;
    uint64_t flags = spin_lock(&savings.lock);
    for (struct opening **at = &savings.first; *at; at = &(*at)->next_saving)
        if (*at == op) {
            *at = op->next_saving;
            break;
        }
    spin_unlock(&savings.lock, flags);
    struct doc *d = opening_lock(op);
    if (!d) return;
    claim_end(d, op);
    if (op->version) version_put(d, op->version);
    if (op->access == N_DOCUMENT_REPLACE) { /* never saved: it goes with its document */
        discard(&d->work);
        d->writers--;
    } else if (op->access != N_DOCUMENT_READ) {
        writer_remove(d);
    }
    size_tell(d);
    opening_move(op, NULL);
    opening_unlock(d);
    doc_put(d); /* the opening's */
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
    uint64_t flags = spin_lock(&docs.lock);
    struct id_link *l = id_table_find(&docs.table, id);
    uint64_t size = l ? __atomic_load_n(&doc_of(l)->size_now, __ATOMIC_ACQUIRE) : NO_SIZE;
    spin_unlock(&docs.lock, flags);
    return size == NO_SIZE ? -N_ERR_NOT_FOUND : (int64_t)size;
}

int document_remove(uint64_t folder, const char *name, size_t n, uint64_t id, struct ns_told *told) {
    struct volume *v = store.v;
    struct doc *d = doc_get(id);
    if (!d) return -N_ERR_NO_MEMORY;
    /* open: it goes to the temp folder; one opened meanwhile finds it removed */
    bool open = doc_users(d) > 1;
    struct folder_lock *t = open ? folder_lock(store.temp) : NULL;
    if (open && !t) {
        doc_put(d);
        return -N_ERR_NO_MEMORY;
    }
    mutex_lock(&d->lock);
    int r;
    if (open) {
        char temp[17];
        size_t k;
        uint64_t number = store_temp_name(temp, &k);
        uint64_t g = change_begin();
        r = ns_move(v, folder, name, n, store.temp, temp, k, store_now(), told);
        change_end();
        if (r >= 0) {
            d->temp = number;
            d->changed_in = g;
        }
    } else {
        struct ranges dropped = {0};
        change_begin();
        r = ns_remove(v, folder, name, n, store_now(), &dropped, told);
        change_end();
        dropped_to(d, &dropped);
        if (r >= 0) d->removed = true;
    }
    mutex_unlock(&d->lock);
    if (t) folder_unlock(t);
    doc_put(d);
    return r;
}

void document_aside(uint64_t id, uint64_t number, uint64_t g) {
    struct doc *d = doc_get(id);
    if (!d) {
        kprintf("store: a document stays in C:\\Nocturne\\Temp until the next start (no memory)\n");
        return;
    }
    mutex_lock(&d->lock);
    if (doc_users(d) > 1) {
        d->temp = number;
        d->changed_in = MAX(d->changed_in, g);
    } else {
        if (temp_remove(number) >= 0) d->removed = true;
    }
    mutex_unlock(&d->lock);
    doc_put(d);
}

void document_changed(uint64_t id, uint64_t g) {
    struct doc *d = doc_find(id);
    if (!d) return;
    mutex_lock(&d->lock);
    d->changed_in = MAX(d->changed_in, g);
    mutex_unlock(&d->lock);
    doc_put(d);
}

int document_raw(uint64_t id, uint64_t size, uint64_t *g) {
    struct volume *v = store.v;
    struct doc *d = doc_get(id);
    if (!d) return -N_ERR_NO_MEMORY;
    mutex_lock(&d->lock);
    struct store_record rec;
    int r = d->removed ? -N_ERR_NOT_FOUND : ns_record(v, id, &rec, NULL);
    uint64_t have = r >= 0 ? blocks_of(rec.size) : 0, want = blocks_of(size);
    if (r >= 0) r = supersede(d);
    if (r >= 0 && want > have) r = space_reserve(v, want - have);
    struct ranges dropped = {0};
    if (r >= 0) {
        *g = change_begin();
        if (want < have) r = ns_cut(v, id, want, &dropped);
        else if (want > have) r = ns_write(v, id, false, have, (size_t)(want - have), NULL, true, &dropped);
        if (r >= 0) {
            struct ns_amend a = {.fields = NS_SIZE | NS_MODIFIED | NS_FLAGS, .size = size, .modified = store_now(),
                                 .flags = (uint16_t)((rec.flags & ~STORE_INLINE) | STORE_UNCHECKED)};
            r = ns_record_amend(v, id, &a);
        }
        change_end();
        d->changed_in = *g;
    }
    dropped_to(d, &dropped);
    mutex_unlock(&d->lock);
    doc_put(d);
    return r;
}

int documents_flush(void) {
    uint64_t flags = spin_lock(&works.lock);
    struct doc *list = works.first;
    works.first = NULL;
    spin_unlock(&works.lock, flags);
    int r = 0;
    for (struct doc *d = list, *next; d; d = next) {
        next = d->next_work;
        mutex_lock(&d->lock);
        int e = flush(d);
        if (e < 0 && r >= 0) r = e;
        bool again = d->work.changed; /* it could not go into the catalog: the next time, then */
        if (again) {
            flags = spin_lock(&works.lock);
            d->next_work = works.first;
            works.first = d;
            spin_unlock(&works.lock, flags);
        } else {
            d->listed = false;
        }
        mutex_unlock(&d->lock);
        if (!again) doc_put(d);
    }
    return r;
}
