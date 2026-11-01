/* Paged memory (docs/nocturne-native-abi.md §7). A pager is the app that made it: through its pager
 * handle, which only it can use (no other app may hold or receive one), it answers the faults of
 * every app mapping the memory. The memory has no pages of its own: the pager binds each page to a
 * frame of RAM it holds (n_pager_supply), with read or read/write access, and takes frames or write
 * access away again (n_pager_revoke). A thread touching a page that is unbound, or bound for reading
 * when it writes, files a fault (one per page and access, which later touches join) and sleeps until
 * the pager supplies the page, takes the fault as failed, or ends; it is woken at once when it is
 * stopped. The pager takes faults in batches; a supply covers whole runs of pages and may bind pages
 * nobody asked for yet. Bound pages need no pager at all: a later touch by any mapper maps the frame
 * itself.
 *
 * The table has two leaves per 512 pages: the bound frames with their access bits, and the objects
 * they were supplied from (a root of RAM or a view of one), each bound page holding a reference of
 * its own (a DMA root also a mapping reference: its type stays). Revocation unmaps a run everywhere
 * (or takes its write access away) and shoots it down before the frames go; so does n_shared_revoke
 * of a view the pages were supplied from (or one it descends from), which unbinds them: the pager
 * is asked again on the next touch. When the pager ends, bound pages stay and unbound ones are
 * access violations; when the memory is gone (no handle, no mapping), the pager handle reports
 * N_EVENT_CLOSED.
 *
 * The pager's lock guards the entries and sources with its faults, so a thread that saw a page
 * unbound files its fault before a supply can settle it. The leaves are made with no lock held and
 * published once (shared_table). Its order is in mm/shared.c. */
#include "mm/shared_parts.h"
#include "mm/heap.h"
#include "mm/pmm.h"
#include "sys/clock.h"
#include "sys/thread.h"

struct pager_fault {
    struct pager_fault *next;
    uint64_t id;            /* a number of its own, to find it again after a copy to the pager */
    size_t page;
    uint32_t access, app;   /* N_MEMORY_READ or _READ_WRITE; the app that touched it first */
    size_t waiters;
    uint8_t state;
    bool queued;            /* not taken by the pager yet */
};
#define PAGER_BATCH 128

uint64_t *paged_slot(struct shared_pages *p, size_t index, struct object ***source) {
    size_t group = 2 * (index / SHARED_LEAF);
    uint64_t *entries = shared_table(p, group, false), *sources = shared_table(p, group + 1, false);
    if (!entries || !sources) return NULL;
    *source = (struct object **)sources + index % SHARED_LEAF;
    return entries + index % SHARED_LEAF;
}
/* Both leaves of page index are there: false without memory. */
static bool paged_leaves_make(struct shared_pages *p, size_t index) {
    size_t group = 2 * (index / SHARED_LEAF);
    return shared_table(p, group, true) && shared_table(p, group + 1, true);
}
/* The lock held: f leaves g's faults for *gone, which its caller frees after the lock. */
static void pager_unlink(struct pager *g, struct pager_fault *f, struct pager_fault **gone) {
    struct pager_fault **link = &g->faults;
    while (*link != f) link = &(*link)->next;
    *link = f->next;
    if (f->queued) __atomic_sub_fetch(&g->queued, 1, __ATOMIC_RELAXED);
    f->next = *gone;
    *gone = f;
}
static void pager_faults_free(struct pager_fault *f) {
    while (f) {
        struct pager_fault *next = f->next;
        kfree(f);
        f = next;
    }
}
/* Faults of [first, first+count) that the memory now answers (supplied: their access is bound;
   failed: all of them) leave the queue and wake their threads; those no thread waits for go to
   *gone. The lock held. */
static void pager_settle(struct pager *g, size_t first, size_t count, uint8_t state, struct pager_fault **gone) {
    bool any = false;
    for (struct pager_fault *f = g->faults, *next; f; f = next) {
        next = f->next;
        if (f->state != PAGER_PENDING || f->page < first || f->page - first >= count) continue;
        if (state == PAGER_SUPPLIED) {
            struct object **source;
            uint64_t *slot = g->memory ? paged_slot(g->memory, f->page, &source) : NULL, entry = slot ? *slot : 0;
            if (!(entry & PAGED_BOUND) || (f->access == N_MEMORY_READ_WRITE && !(entry & PAGED_WRITE))) continue;
        }
        f->state = state;
        any = true;
        if (f->queued) {
            f->queued = false;
            __atomic_sub_fetch(&g->queued, 1, __ATOMIC_RELAXED);
        }
        if (!f->waiters) pager_unlink(g, f, gone);
    }
    if (any) {
        wq_wake_all(&g->waiters);
        object_notify(&g->object);
    }
}
int pager_wait(struct pager *g, size_t page, bool write, struct pager_fault **spare) {
    uint32_t access = write ? N_MEMORY_READ_WRITE : N_MEMORY_READ;
    if (g->ended || thread_current()->killed) {
        spin_give(&g->lock);
        return -1;
    }
    struct pager_fault *f = g->faults, **tail = &g->faults, *gone = NULL;
    for (; f; tail = &f->next, f = f->next) /* one per page and access: a binding for reading answers a read while a write waits */
        if (f->page == page && f->state == PAGER_PENDING && f->access == access) break;
    if (!f) {
        if (!*spare) {
            spin_give(&g->lock);
            return (*spare = kzalloc(sizeof **spare)) ? 0 : -1;
        }
        f = *spare;
        *spare = NULL;
        *f = (struct pager_fault){.id = ++g->next_id, .page = page, .access = access, .app = (uint32_t)process_current()->pid,
                                  .state = PAGER_PENDING, .queued = true};
        *tail = f;
        __atomic_add_fetch(&g->queued, 1, __ATOMIC_RELAXED);
        wq_wake_all(&g->taker);
        object_notify(&g->object);
    }
    f->waiters++;
    object_ref(&g->object);
    while (f->state == PAGER_PENDING && !g->ended && !thread_current()->killed) shared_sleep(&g->waiters, &g->lock);
    bool supplied = f->state == PAGER_SUPPLIED;
    if (!--f->waiters && (f->state != PAGER_PENDING || g->ended || thread_current()->killed)) pager_unlink(g, f, &gone);
    spin_give(&g->lock);
    pager_faults_free(gone);
    object_unref(&g->object);
    return supplied ? 1 : -1;
}
/* Drops the references bound pages held on their sources, once nothing reaches the frames. */
static void paged_drop(struct object **sources, size_t count) {
    for (size_t i = 0; i < count; i++) {
        struct shared_pages *s = shared_root(sources[i]->priv);
        if (s->dma) {
            spin_take(&s->lock);
            ASSERT(s->maps);
            s->maps--;
            spin_give(&s->lock);
        }
        object_unref(sources[i]);
    }
}
/* Unbinds [first, first+count) of p (with revoked_only: only the pages supplied from an object
   whose scope was revoked): unmapped everywhere first, then the frames are let go. */
static void paged_unbind(struct shared_pages *p, size_t first, size_t count, bool revoked_only) {
    struct pager *g = p->pager;
    while (count) {
        size_t n = MIN(count, (size_t)PAGER_BATCH), dropped = 0;
        struct object *sources[PAGER_BATCH];
        spin_take(&g->lock);
        for (size_t i = 0; i < n; i++) {
            struct object **source;
            uint64_t *entry = paged_slot(p, first + i, &source);
            if (!entry) { /* no leaves: nothing bound there */
                i += SHARED_LEAF - 1 - (first + i) % SHARED_LEAF;
                continue;
            }
            if (!*entry) continue;
            if (revoked_only && !__atomic_load_n(&((struct shared_pages *)(*source)->priv)->cancelled, __ATOMIC_ACQUIRE)) continue;
            sources[dropped++] = *source;
            *entry = 0;
            *source = NULL;
        }
        spin_give(&g->lock);
        if (dropped) {
            shared_withdraw(p, (uint64_t)first * PAGE_SIZE, (uint64_t)n * PAGE_SIZE, false, false);
            paged_drop(sources, dropped);
        }
        first += n;
        count -= n;
    }
}
/* g's memory with a reference of the caller's, or NULL once it went. */
static struct shared_pages *pager_memory(struct pager *g) {
    spin_take(&g->lock);
    struct shared_pages *p = g->memory;
    if (p && !object_ref_live(&p->object)) p = NULL;
    spin_give(&g->lock);
    return p;
}
void pager_source_revoked(struct pager *g) {
    struct shared_pages *p = pager_memory(g);
    if (!p) return;
    paged_unbind(p, 0, p->count, true);
    object_unref(&p->object);
}
void paged_release(struct shared_pages *p) {
    struct pager *g = p->pager;
    spin_take(&g->lock);
    __atomic_store_n(&g->memory, NULL, __ATOMIC_RELEASE); /* no supply or take reaches its leaves now */
    struct shared_binding *bindings = g->bindings;
    g->bindings = NULL;
    spin_give(&g->lock);
    shared_bindings_end(bindings);
    paged_unbind(p, 0, p->count, false);
    for (size_t i = 0; i < 2 * ((p->count + SHARED_LEAF - 1) / SHARED_LEAF); i++)
        if (p->pages[i]) pmm_free(p->pages[i]);
    struct pager_fault *gone = NULL;
    spin_take(&g->lock);
    for (struct pager_fault *f = g->faults; f; f = f->next) f->state = PAGER_FAILED;
    while (g->faults && !g->faults->waiters) pager_unlink(g, g->faults, &gone);
    wq_wake_all(&g->waiters);
    wq_wake_all(&g->taker);
    object_notify(&g->object);
    spin_give(&g->lock);
    pager_faults_free(gone);
    object_unref(&g->object);
}
static uint32_t pager_ready(struct handle *h) {
    struct pager *g = h->object->priv;
    return (__atomic_load_n(&g->queued, __ATOMIC_RELAXED) ? N_EVENT_RECEIVABLE : 0) |
           (__atomic_load_n(&g->memory, __ATOMIC_ACQUIRE) ? 0 : N_EVENT_CLOSED);
}
/* The pager's handle closed: what waits fails, and what comes later fails at once. */
static void pager_close(struct handle *h) {
    struct pager *g = h->object->priv;
    struct pager_fault *gone = NULL;
    spin_take(&g->lock);
    g->ended = true;
    for (struct pager_fault *f = g->faults, *next; f; f = next) {
        next = f->next;
        f->state = PAGER_FAILED;
        if (!f->waiters) pager_unlink(g, f, &gone);
    }
    wq_wake_all(&g->waiters);
    spin_give(&g->lock);
    pager_faults_free(gone);
}
static void pager_release(struct object *o) {
    struct pager *g = o->priv;
    pager_faults_free(g->faults);
    kfree(g);
}
static const struct object_ops pager_ops = {.kind = N_KIND_PAGER, .ready = pager_ready, .close = pager_close, .release = pager_release};

int64_t pager_allowed(struct handle *h) {
    if (!handle_is(h, &pager_ops)) return -N_ERR_WRONG_KIND;
    struct pager *g = h->object->priv;
    return g->creator == process_current()->pid ? 0 : -N_ERR_DENIED;
}
int64_t pager_create(size_t bytes, struct handle **memory, struct handle **pager) {
    if (!bytes || bytes > SIZE_MAX - (PAGE_SIZE - 1)) return -N_ERR_INVALID;
    bytes = ALIGN_UP(bytes, PAGE_SIZE);
    size_t count = bytes / PAGE_SIZE, groups = (count + SHARED_LEAF - 1) / SHARED_LEAF;
    struct pager *g = kzalloc(sizeof *g);
    if (!g) return -N_ERR_NO_MEMORY;
    object_init(&g->object, &pager_ops, g);
    g->lock.rank = SPIN_PAGER;
    g->creator = process_current()->pid;
    struct shared_pages *p = shared_new(bytes, SHARED_PAGED, 2 * groups);
    if (!p) {
        kfree(g);
        return -N_ERR_NO_MEMORY;
    }
    p->count = count;
    p->pager = g;
    g->memory = p;
    object_ref(&g->object); /* the memory's reference */
    int64_t r = shared_open(p, HANDLE_WRITE, memory); /* on failure the memory goes, and with it the pager */
    if (r < 0) return r;
    if (!(*pager = handle_open(&g->object, HANDLE_WRITE))) {
        handle_close(*memory);
        return -N_ERR_NO_MEMORY;
    }
    return 0;
}
int64_t pager_take(struct handle *h, struct n_pager_fault *out, uint64_t *ids, size_t max, uint64_t deadline) {
    struct pager *g = h->object->priv;
    if (!max) return -N_ERR_INVALID;
    int64_t r = 0;
    size_t k = 0;
    spin_take(&g->lock);
    for (;;) {
        for (struct pager_fault *q = g->faults; q && k < max; q = q->next) {
            if (!q->queued) continue;
            out[k] = (struct n_pager_fault){.offset = (uint64_t)q->page * PAGE_SIZE, .access = q->access, .app = q->app};
            ids[k++] = q->id;
            q->queued = false;
            __atomic_sub_fetch(&g->queued, 1, __ATOMIC_RELAXED);
        }
        if (k) {
            r = (int64_t)k;
            break;
        }
        if (!g->memory) {
            r = -N_ERR_CLOSED;
            break;
        }
        if (thread_current()->killed) {
            r = -N_ERR_STOPPED;
            break;
        }
        if (clock_now_ns() >= deadline) {
            r = -N_ERR_TIMEOUT;
            break;
        }
        uint64_t f = irq_save();
        wq_wait_locked_until(&g->taker, &g->lock, deadline);
        irq_restore(f);
        spin_take(&g->lock);
    }
    spin_give(&g->lock);
    return r;
}
void pager_untake(struct handle *h, const uint64_t *ids, size_t count) {
    struct pager *g = h->object->priv;
    spin_take(&g->lock);
    for (struct pager_fault *q = g->faults; q; q = q->next)
        for (size_t i = 0; i < count; i++)
            if (q->id == ids[i] && q->state == PAGER_PENDING && !q->queued) {
                q->queued = true;
                __atomic_add_fetch(&g->queued, 1, __ATOMIC_RELAXED);
            }
    spin_give(&g->lock);
}
/* [offset, offset+bytes) of p, g's memory (held by the caller), bound to the pages of source from
   source_offset on. Pages of RAM only (write-back DMA RAM included), not paged or program memory,
   and from a root or a view, not a composite (its parts are supplied themselves); read/write access
   needs a writable source. Each page holds the source it came through, so revoking that source (or
   a view it descends from) unbinds the page (pager_source_revoked): the binding is there before the
   first page, and a source revoked meanwhile binds nothing more. Frames, demand pages' and the
   table's, are made with no lock held. */
static int64_t pager_supply_run(struct pager *g, struct shared_pages *p, const struct n_pager_supply *run) {
    if (!run->bytes || (run->offset | run->bytes | run->source_offset) & (PAGE_SIZE - 1) || run->offset > p->bytes ||
        run->bytes > p->bytes - run->offset || (run->access != N_MEMORY_READ && run->access != N_MEMORY_READ_WRITE))
        return -N_ERR_INVALID;
    struct handle *sh = handle_get(run->source);
    if (!sh) return -N_ERR_NO_HANDLE;
    struct shared_pages *src = shared_of(sh), *leaf = NULL;
    int64_t r = 0;
    size_t from = run->source_offset / PAGE_SIZE;
    uint8_t cache = 0;
    if (!src) r = -N_ERR_WRONG_KIND;
    else if (run->source_offset > src->bytes || run->bytes > src->bytes - run->source_offset) r = -N_ERR_INVALID;
    else if (run->access == N_MEMORY_READ_WRITE && !handle_allows(sh, HANDLE_WRITE)) r = -N_ERR_DENIED;
    else if (shared_root(src)->kind == SHARED_COMPOSITE) r = -N_ERR_DENIED;
    else if (__atomic_load_n(&src->cancelled, __ATOMIC_ACQUIRE) || shared_device_gone(src)) r = -N_ERR_CLOSED;
    else {
        leaf = shared_leaf(src, &from, &cache, NULL, NULL); /* from: its page there */
        bool ram = leaf->kind == SHARED_RAM || (leaf->kind == SHARED_RANGE && leaf->owned) || leaf->kind == SHARED_DEMAND;
        if (!ram || cache != N_CACHE_WRITE_BACK) r = -N_ERR_DENIED;
        else r = shared_binding_add(src, g);
    }
    size_t first = run->offset / PAGE_SIZE, count = run->bytes / PAGE_SIZE;
    while (r >= 0 && count) {
        size_t n = MIN(count, (size_t)PAGER_BATCH), dropped = 0, kept = 0;
        bool unmap = false, protect = false;
        struct object *sources[PAGER_BATCH];
        uint64_t frames[PAGER_BATCH];
        struct pager_fault *gone = NULL;
        for (size_t i = 0; i < n; i++) {
            size_t index = from + i;
            if (leaf->kind == SHARED_DEMAND && demand_make(leaf, index) < 0) {
                r = -N_ERR_NO_MEMORY;
                n = i;
                break;
            }
            frames[i] = leaf->kind == SHARED_RAM ? leaf->pages[index] : leaf->kind == SHARED_RANGE ? leaf->base + index * PAGE_SIZE :
                        demand_frame(leaf, index);
            if (!paged_leaves_make(p, first + i)) {
                r = -N_ERR_NO_MEMORY;
                n = i;
                break;
            }
        }
        if (leaf->dma) {
            /* Each page to bind holds the type; the type is read with the count. */
            spin_take(&leaf->lock);
            while (leaf->typing) shared_sleep(&leaf->typed, &leaf->lock);
            if (leaf->cache != N_CACHE_WRITE_BACK) {
                r = -N_ERR_DENIED;
                n = 0;
            }
            leaf->maps += n;
            spin_give(&leaf->lock);
        }
        spin_take(&g->lock);
        /* checked again with each batch, with the binding: a revocation between two finds what was
           bound before, and none is bound after it */
        if (__atomic_load_n(&src->cancelled, __ATOMIC_ACQUIRE)) {
            r = -N_ERR_CLOSED;
            kept = n;
            n = 0;
        }
        for (size_t i = 0; i < n; i++) {
            struct object **source;
            uint64_t *entry = paged_slot(p, first + i, &source);
            uint64_t now = frames[i] | PAGED_BOUND | (run->access == N_MEMORY_READ_WRITE ? PAGED_WRITE : 0);
            bool same = (*entry & PTE_ADDR) == frames[i] && (*entry & PAGED_BOUND);
            if (same) {
                protect |= (*entry & PAGED_WRITE) && !(now & PAGED_WRITE);
                *entry = now;
                if (*source == sh->object) {
                    kept++;
                    continue;
                }
            }
            /* a bound page lets its old source go (bound to another frame, it is unmapped first):
               it answers to the source it came through last */
            if (*entry) {
                sources[dropped++] = *source;
                unmap |= !same;
            }
            *entry = now;
            *source = sh->object;
            object_ref(sh->object);
        }
        spin_give(&g->lock);
        if (kept && leaf->dma) { /* those that hold no new reference: no count of theirs */
            spin_take(&leaf->lock);
            leaf->maps -= kept;
            spin_give(&leaf->lock);
        }
        /* a page bound elsewhere before is unmapped everywhere before its old frame goes */
        if (unmap) shared_withdraw(p, (uint64_t)first * PAGE_SIZE, (uint64_t)n * PAGE_SIZE, false, false);
        else if (protect) shared_withdraw(p, (uint64_t)first * PAGE_SIZE, (uint64_t)n * PAGE_SIZE, true, false);
        if (dropped) paged_drop(sources, dropped);
        spin_take(&g->lock);
        pager_settle(g, first, n, PAGER_SUPPLIED, &gone);
        spin_give(&g->lock);
        pager_faults_free(gone);
        first += n;
        from += n;
        count -= n;
    }
    handle_close(sh);
    return r;
}
int64_t pager_supply(struct handle *h, const struct n_pager_supply *run) {
    struct pager *g = h->object->priv;
    struct shared_pages *p = pager_memory(g);
    if (!p) return -N_ERR_CLOSED;
    int64_t r = pager_supply_run(g, p, run);
    object_unref(&p->object);
    return r;
}
int64_t pager_revoke(struct handle *h, uint64_t offset, uint64_t bytes, uint32_t access) {
    struct pager *g = h->object->priv;
    struct shared_pages *p = pager_memory(g);
    if (!p) return -N_ERR_CLOSED;
    int64_t r = 0;
    if (!bytes || (offset | bytes) & (PAGE_SIZE - 1) || offset > p->bytes || bytes > p->bytes - offset ||
        (access && access != N_MEMORY_READ))
        r = -N_ERR_INVALID;
    else if (!access) paged_unbind(p, offset / PAGE_SIZE, bytes / PAGE_SIZE, false);
    else for (size_t first = offset / PAGE_SIZE, count = bytes / PAGE_SIZE; count;) {
        size_t n = MIN(count, (size_t)PAGER_BATCH);
        bool any = false;
        spin_take(&g->lock);
        for (size_t i = 0; i < n; i++) {
            struct object **source;
            uint64_t *entry = paged_slot(p, first + i, &source);
            if (entry && (*entry & PAGED_WRITE)) {
                *entry &= ~PAGED_WRITE;
                any = true;
            }
        }
        spin_give(&g->lock);
        if (any) shared_withdraw(p, (uint64_t)first * PAGE_SIZE, (uint64_t)n * PAGE_SIZE, true, false);
        first += n;
        count -= n;
    }
    object_unref(&p->object);
    return r;
}
int64_t pager_fail(struct handle *h, uint64_t offset, uint64_t bytes) {
    struct pager *g = h->object->priv;
    if (!bytes || (offset | bytes) & (PAGE_SIZE - 1) || offset + bytes < offset) return -N_ERR_INVALID;
    struct pager_fault *gone = NULL;
    spin_take(&g->lock);
    pager_settle(g, offset / PAGE_SIZE, bytes / PAGE_SIZE, PAGER_FAILED, &gone);
    spin_give(&g->lock);
    pager_faults_free(gone);
    return 0;
}
