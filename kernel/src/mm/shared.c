/* Shared memory: objects of pages behind handles. A descriptor and every mapping hold references
 * of their own: closing or killing either peer cannot free pages the other still reads.
 *
 * The kinds. RAM owns one frame per page from creation. Demand reserves address space (a growable
 * buffer's or shared wasm memory's maximum): a two-level table of frames receives zeroed pages at
 * the first touch by any mapper, and every later mapper sees the same frame. A range is a run of
 * physical pages, a device's BAR or a contiguous DMA run allocated whole. A view maps part of
 * another object, which it holds open, with its own caching. A composite concatenates slices of
 * roots. Paged memory has no pages of its own (mm/pager.c). A program's segments are demand objects
 * read from a document (mm/program.c).
 *
 * Drivers in apps (dev/pciuser.c): DMA objects are RAM or an owned range whose bus addresses
 * n_dma_addresses reports. BAR and DMA objects hold their PCI claim. A BAR object dies with the
 * claim's last handle (shared_revoke_claim). Where an IOMMU isolates the function, a DMA object's
 * pages are in its domain from creation until release, which returns them once the IOMMU confirmed
 * the device can no longer reach them. After a nonisolated claim ends, remaining DMA RAM stays
 * quarantined: BME clear and Transactions Pending alone do not prove all posted DMA drained. While
 * the claim is live, caller-driven free still requires the driver's own device synchronization.
 * DMA RAM has one memory type for all its mappings and the direct map: write-back until the first
 * uncached or write-combining view while nothing maps it, then that type for good (x86 forbids one
 * page with two types).
 *
 * Who holds what. Frames are published without a lock: a demand slot, and the frames of the tables
 * above slots, go from 0 to their frame once, by one compare and exchange, so a frame is prepared
 * (allocated and zeroed) with nothing held and a peer that came first keeps its own. They stay until
 * the object goes.
 *
 * Revocation runs from the owner down, not over everything:
 *   - An object lists its mappings. shared_withdraw walks those of one object.
 *   - A scope is the revocable part of a view or a composite. Its scopes form trees: a view's scope
 *     is a child of the scope of what it was made from. A composite's scope is a tree of its own,
 *     and a dependent of the scope of each slice that has one. Revoking pushes down: each scope
 *     reached is marked, its object's cancelled set, and its object's mappings withdrawn; the bound
 *     pages of the paged memories supplied from within it unbound. A fault reads one flag of its
 *     object and walks no ancestors.
 *   - A claim lists its BAR objects, and an object the views and composites made from it. A view
 *     stands on a root or a composite and a composite on roots, so this is three levels at most.
 *
 * The locks, each taken before the next when two nest:
 *   a process's space lock (sys/process.h; a wait queue's, interrupts off)
 *   a process's mappings_lock (SPIN_SHARED_MAPPINGS): its tree of mappings. A fault holds it only
 *     to find its mapping, and takes the object's lock before it lets go.
 *   an object's lock (SPIN_SHARED_OBJECT): its list of mappings and their fields, its derived list,
 *     a DMA root's type and maps, a program segment's source. A fault checks its mapping and object
 *     and maps the page with it held, so a revocation or a removal that changes the mapping after
 *     finds the page. Two objects' locks are never held at once: a composite's fault holds the
 *     composite's alone and reads its part's frames without a lock.
 *   a pager's lock (SPIN_PAGER): under the memory's object lock.
 *   device_pin_lock (SPIN_DEVICE_PINS).
 *   the page tables' locks, pmm's, the heap's (arch/spin.h).
 * These four ranks are held with interrupts on: no interrupt's handler takes them, nor do those who
 * hold a lock a handler takes wait for them. Held, nothing sleeps, copies user memory, waits for the
 * disk, allocates from the heap or zeroes a frame. The waits are shared_sleep's.
 * Beside them, two sleeping locks for what only revocation walks:
 *   revoke_mutex: one revocation at a time. A scope marked is one whose revocation ended, or is the
 *     caller's own, so each is visited once however many paths lead to it. No fault and no mapping
 *     takes it.
 *   a tree's mutex (the root scope's tree), after revoke_mutex: its scopes' links, owners and
 *     revoked, their dependents and bindings. Two are never held at once. */
#include "mm/shared_parts.h"
#include "mm/heap.h"
#include "mm/pmm.h"
#include "mm/vmalloc.h"
#include "dev/fb.h"
#include "dev/pci.h"
#include "dev/pciuser.h"
#include "sys/log.h"
#include "sys/thread.h"

/* ---- scopes ---- */

/* The revocable part of a view or composite. refs counts its owner, its children, the composites
   that depend on it, and the paged memories bound from within it. An object that is gone leaves
   its scope for those. */
struct shared_scope {
    struct shared_scope *parent, *root;
    size_t refs;          /* atomic */
    int issuer;           /* the app that may revoke it */
    bool revoked;         /* set under the tree's mutex, read atomically */
    bool queued;          /* revoke_mutex */
    struct shared_scope *work_next; /* revoke_mutex */
    struct shared_pages *owner;     /* NULL once it is gone */
    struct shared_link child;       /* in its parent's children */
    struct shared_link *children, *dependents, *bindings, *bindings_ended;
    struct mutex tree;    /* a root scope's, for all of its tree */
};
/* Pages of pager's memory were supplied from an object whose scope is this: the memory's going
   ends it, under the tree's mutex. While it is on the list its pager stays. */
struct shared_binding {
    struct shared_link link; /* in scope's bindings */
    struct shared_scope *scope;
    struct pager *pager;
    struct shared_binding *pager_next; /* in the pager's */
};
static struct mutex revoke_mutex;

static bool cancelled(struct shared_pages *p) { return __atomic_load_n(&p->cancelled, __ATOMIC_ACQUIRE); }

static bool scope_hold_live(struct shared_scope *s) {
    size_t refs = __atomic_load_n(&s->refs, __ATOMIC_RELAXED);
    while (refs)
        if (__atomic_compare_exchange_n(&s->refs, &refs, refs + 1, false, __ATOMIC_ACQUIRE, __ATOMIC_RELAXED)) return true;
    return false;
}
/* Iterative: a chain of views is as long as an app makes it. Each scope's reference on its parent
   keeps the root, and so the tree's mutex, while it is taken. */
static void scope_drop(struct shared_scope *s) {
    while (s && !__atomic_sub_fetch(&s->refs, 1, __ATOMIC_ACQ_REL)) {
        struct shared_scope *parent = s->parent;
        if (parent) {
            mutex_lock(&s->root->tree);
            link_del(&s->child);
            mutex_unlock(&s->root->tree);
        }
        kfree(s);
        s = parent;
    }
}
/* A new scope of p's in the calling app's name, under parent (NULL: a tree of its own): 0, or
   -N_ERR_CLOSED when parent was revoked, or -N_ERR_NO_MEMORY. p's release takes it back. */
static int scope_new(struct shared_pages *p, struct shared_scope *parent) {
    struct shared_scope *s = kzalloc(sizeof *s);
    if (!s) return -N_ERR_NO_MEMORY;
    s->refs = 1;
    s->issuer = process_current()->pid;
    s->owner = p;
    s->parent = parent;
    s->root = parent ? parent->root : s;
    if (parent) {
        mutex_lock(&s->root->tree);
        bool revoked = parent->revoked;
        if (!revoked) {
            __atomic_add_fetch(&parent->refs, 1, __ATOMIC_RELAXED);
            link_add(&parent->children, &s->child, s);
        }
        mutex_unlock(&s->root->tree);
        if (revoked) {
            kfree(s);
            return -N_ERR_CLOSED;
        }
    }
    p->scope = s;
    return 0;
}
/* Composite p depends on scope, d its record: 0, or -N_ERR_CLOSED when it was revoked. */
static int scope_depend(struct shared_scope *scope, struct shared_pages *p, struct shared_dependency *d) {
    mutex_lock(&scope->root->tree);
    bool revoked = scope->revoked;
    if (!revoked) {
        __atomic_add_fetch(&scope->refs, 1, __ATOMIC_RELAXED);
        d->scope = scope;
        link_add(&scope->dependents, &d->link, p);
    }
    mutex_unlock(&scope->root->tree);
    return revoked ? -N_ERR_CLOSED : 0;
}
/* revoke_mutex held. */
static void scope_queue(struct shared_scope *s, struct shared_scope **work) {
    if (s->queued || !scope_hold_live(s)) return;
    s->queued = true;
    s->work_next = *work;
    *work = s;
}
/* One scope of a revocation: marked with its object, what depends on it queued, its object's
   mappings withdrawn and the pages bound from it unbound. A view or dependent made after the mark
   is refused at its making, so none is missed. The mutex is let go before an object is withdrawn
   or dropped, whose release takes it. */
static void scope_visit(struct shared_scope *s, struct shared_scope **work) {
    mutex_lock(&s->root->tree);
    __atomic_store_n(&s->revoked, true, __ATOMIC_RELEASE);
    struct shared_pages *owner = s->owner;
    if (owner) {
        __atomic_store_n(&owner->cancelled, true, __ATOMIC_SEQ_CST);
        if (!object_ref_live(&owner->object)) owner = NULL; /* being made or gone: it has no mapping */
    }
    for (struct shared_link *l = s->children; l; l = l->next) scope_queue(l->owner, work);
    for (struct shared_link *l = s->dependents; l; l = l->next) scope_queue(((struct shared_pages *)l->owner)->scope, work);
    mutex_unlock(&s->root->tree);
    if (owner) {
        shared_withdraw(owner, 0, 0, false, true);
        object_unref(&owner->object);
    }
    for (;;) {
        mutex_lock(&s->root->tree);
        struct shared_link *l = s->bindings;
        struct pager *g = NULL;
        if (l) {
            g = ((struct shared_binding *)l->owner)->pager;
            link_del(l);
            link_add(&s->bindings_ended, l, l->owner);
            object_ref(&g->object);
        }
        mutex_unlock(&s->root->tree);
        if (!l) break;
        pager_source_revoked(g);
        object_unref(&g->object);
    }
}
int shared_binding_add(struct shared_pages *src, struct pager *g) {
    struct shared_scope *s = src->scope;
    if (!s) return 0; /* no scope: nothing can revoke it */
    struct shared_binding *spare = NULL;
    int r = 0;
    for (;;) {
        bool found = false;
        mutex_lock(&s->root->tree);
        if (s->revoked) r = -N_ERR_CLOSED;
        for (struct shared_link *l = s->bindings; !r && l && !found; l = l->next) found = ((struct shared_binding *)l->owner)->pager == g;
        if (!r && !found && spare) {
            __atomic_add_fetch(&s->refs, 1, __ATOMIC_RELAXED);
            spare->scope = s;
            spare->pager = g;
            link_add(&s->bindings, &spare->link, spare);
            spin_take(&g->lock);
            spare->pager_next = g->bindings;
            g->bindings = spare;
            spin_give(&g->lock);
            spare = NULL;
            found = true;
        }
        mutex_unlock(&s->root->tree);
        if (r || found) break;
        if (!(spare = kzalloc(sizeof *spare))) return -N_ERR_NO_MEMORY;
    }
    kfree(spare);
    return r;
}
void shared_bindings_end(struct shared_binding *b) {
    while (b) {
        struct shared_binding *next = b->pager_next;
        struct shared_scope *s = b->scope;
        mutex_lock(&s->root->tree);
        link_del(&b->link);
        mutex_unlock(&s->root->tree);
        scope_drop(s);
        kfree(b);
        b = next;
    }
}

/* ---- resolving pages ---- */

struct shared_pages *shared_root(struct shared_pages *p) { return p->kind == SHARED_VIEW ? p->parent->priv : p; }

static struct shared_part *shared_part_at(struct shared_pages *p, size_t page) {
    size_t lo = 0, hi = p->part_count;
    while (lo < hi) {
        size_t mid = lo + (hi - lo) / 2;
        if (page < p->parts[mid].end) hi = mid;
        else lo = mid + 1;
    }
    ASSERT(lo < p->part_count);
    return &p->parts[lo];
}
struct shared_pages *shared_leaf(struct shared_pages *p, size_t *index, uint8_t *cache, struct object **backing, size_t *span) {
    *cache = p->cache;
    if (p->kind == SHARED_VIEW) {
        *index += p->first;
        if (backing) *backing = p->parent;
        p = p->parent->priv;
    }
    if (p->kind == SHARED_COMPOSITE) {
        struct shared_part *part = shared_part_at(p, *index);
        size_t start = part == p->parts ? 0 : part[-1].end;
        if (span) *span = part->end - *index;
        *index = part->first + *index - start;
        *cache = part->cache;
        if (backing) *backing = part->object;
        p = part->object->priv;
    } else if (span) *span = p->count - *index;
    if (p->dma) *cache = __atomic_load_n(&p->cache, __ATOMIC_ACQUIRE);
    return p;
}
static bool range_gone(const struct shared_pages *p) { return p->kind == SHARED_RANGE && !p->owned && pci_user_revoked(p->claim); }
bool shared_device_gone(struct shared_pages *p) {
    struct shared_pages *root = shared_root(p);
    if (root->kind != SHARED_COMPOSITE) return range_gone(root);
    for (size_t i = 0; i < root->part_count; i++)
        if (range_gone(root->parts[i].object->priv)) return true;
    return false;
}
uint64_t *shared_table(struct shared_pages *p, size_t leaf, bool create) {
    uint64_t table = __atomic_load_n(&p->pages[leaf], __ATOMIC_ACQUIRE);
    if (!table && create) {
        uint64_t made = pmm_alloc_zeroed();
        if (!made) return NULL;
        if (__atomic_compare_exchange_n(&p->pages[leaf], &table, made, false, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) table = made;
        else pmm_free(made);
    }
    return table ? phys_to_virt(table) : NULL;
}
uint64_t demand_frame(struct shared_pages *p, size_t index) {
    uint64_t *table = shared_table(p, index / SHARED_LEAF, false);
    return table ? __atomic_load_n(&table[index % SHARED_LEAF], __ATOMIC_ACQUIRE) : 0;
}
int demand_adopt(struct shared_pages *p, size_t index, uint64_t frame) {
    uint64_t *table = shared_table(p, index / SHARED_LEAF, true), none = 0;
    if (table && __atomic_compare_exchange_n(&table[index % SHARED_LEAF], &none, frame, false, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) {
        __atomic_add_fetch(&p->resident, 1, __ATOMIC_RELAXED);
        return 0;
    }
    pmm_free(frame);
    return table ? 0 : -N_ERR_NO_MEMORY;
}
int demand_make(struct shared_pages *p, size_t index) {
    if (demand_frame(p, index)) return 0;
    uint64_t frame = pmm_alloc_zeroed();
    return frame ? demand_adopt(p, index, frame) : -N_ERR_NO_MEMORY;
}
/* The frame of page index of leaf, 0 when it has none yet. RAM, a range or demand memory. */
static uint64_t leaf_frame(struct shared_pages *leaf, size_t index) {
    return leaf->kind == SHARED_RAM ? leaf->pages[index] : leaf->kind == SHARED_RANGE ? leaf->base + index * PAGE_SIZE :
           demand_frame(leaf, index);
}
/* Not paged memory. */
static uint64_t shared_frame(struct shared_pages *p, size_t index) {
    uint8_t cache;
    struct shared_pages *leaf = shared_leaf(p, &index, &cache, NULL, NULL);
    return leaf_frame(leaf, index);
}
/* The PTE flags a mapping with `flags` gives a page of paged memory with this entry: bound for
   reading only, it is mapped without write access, as a read-only shared page n_memory_protect
   cannot change. */
static uint64_t paged_page_flags(uint64_t entry, uint64_t flags) {
    return entry & PAGED_WRITE ? flags : (flags & ~PTE_W) | PTE_SHARED_RO;
}
static uint64_t cache_pte(unsigned cache) {
    if (cache == N_CACHE_WRITE_COMBINE) return PTE_PWT | PTE_PAT4K; /* PAT index 5 */
    if (cache == N_CACHE_UNCACHED) return PTE_PCD | PTE_PWT; /* index 3 */
    return 0;
}

/* ---- DMA types ---- */

/* The maps of each DMA root p reaches count one more: its type stays. A mapping takes the type
   being set when it waits for it. */
static void shared_dma_hold(struct shared_pages *p) {
    for (size_t at = 0; at < p->count;) {
        size_t page = at, span;
        uint8_t cache;
        struct shared_pages *leaf = shared_leaf(p, &page, &cache, NULL, &span);
        if (leaf->dma) {
            spin_take(&leaf->lock);
            while (leaf->typing) shared_sleep(&leaf->typed, &leaf->lock);
            leaf->maps++;
            spin_give(&leaf->lock);
        }
        at += MIN(span, p->count - at);
    }
}
static void shared_dma_release(struct shared_pages *p) {
    for (size_t at = 0; at < p->count;) {
        size_t page = at, span;
        uint8_t cache;
        struct shared_pages *leaf = shared_leaf(p, &page, &cache, NULL, &span);
        if (leaf->dma) {
            spin_take(&leaf->lock);
            ASSERT(leaf->maps);
            leaf->maps--;
            spin_give(&leaf->lock);
        }
        at += MIN(span, p->count - at);
    }
}

/* ---- objects ---- */

static size_t shared_slots(const struct shared_pages *p) {
    return p->kind == SHARED_DEMAND ? (p->count + SHARED_LEAF - 1) / SHARED_LEAF : p->kind == SHARED_RAM ? p->count : 0;
}
void shared_release(struct object *o) {
    struct shared_pages *p = o->priv;
    if (p->kind == SHARED_PAGED) paged_release(p); /* its bindings, leaves and pager */
    /* It leaves what it was made from and what its revocation reaches first. */
    for (size_t i = 0; i < p->dependency_count; i++) {
        struct shared_dependency *d = &p->dependencies[i];
        mutex_lock(&d->scope->root->tree);
        link_del(&d->link);
        mutex_unlock(&d->scope->root->tree);
        scope_drop(d->scope);
    }
    if (p->scope) {
        mutex_lock(&p->scope->root->tree);
        p->scope->owner = NULL;
        mutex_unlock(&p->scope->root->tree);
    }
    if (p->parent) {
        struct shared_pages *parent = p->parent->priv;
        spin_take(&parent->lock);
        link_del(&p->derived_link);
        spin_give(&parent->lock);
    }
    for (size_t i = 0; i < p->part_count; i++) {
        struct shared_pages *part = p->parts[i].object->priv;
        spin_take(&part->lock);
        link_del(&p->parts[i].derived);
        spin_give(&part->lock);
    }
    if (p->claim_link.pprev) {
        struct shared_claim *c = pci_user_shared(p->claim);
        mutex_lock(&c->lock);
        link_del(&p->claim_link);
        mutex_unlock(&c->lock);
    }
    bool keep = p->dma && (!pci_user_dma_unmap(p->claim, p->kind == SHARED_RAM ? p->pages : NULL, p->base, p->count) ||
                           !pci_user_dma_reusable(p->claim)); /* the device may still write there */
    if (keep) kprintf("[dma] %lu pages stay out of use: their device may still reach them\n", (unsigned long)p->count);
    else if (p->dma && p->cache != N_CACHE_WRITE_BACK)
        vmm_direct_type(p->kind == SHARED_RAM ? p->pages : NULL, p->base, p->count, 0);
    for (size_t i = 0; !keep && i < shared_slots(p); i++) {
        if (!p->pages[i]) continue;
        if (p->kind == SHARED_DEMAND) {
            uint64_t *leaf = phys_to_virt(p->pages[i]);
            for (size_t j = 0; j < SHARED_LEAF; j++)
                if (leaf[j]) pmm_free(leaf[j]);
        }
        pmm_free(p->pages[i]);
    }
    if (p->kind == SHARED_RANGE && p->owned && !keep) pmm_free_contig(p->base, p->count);
    object_unref(p->parent);
    for (size_t i = 0; i < p->part_count; i++) object_unref(p->parts[i].object);
    kfree(p->parts);
    kfree(p->dependencies);
    scope_drop(p->scope);
    pci_user_drop(p->claim);
    handle_close(p->source);
    kfree(p);
}
static const struct object_ops shared_ops = {.kind = N_KIND_SHARED, .release = shared_release};

struct shared_pages *shared_new(size_t bytes, uint8_t kind, size_t slots) {
    if (slots > (SIZE_MAX - sizeof(struct shared_pages)) / sizeof(uint64_t)) return NULL;
    struct shared_pages *p = kzalloc(sizeof *p + slots * sizeof(uint64_t));
    if (!p) return NULL;
    p->kind = kind;
    p->cache = N_CACHE_WRITE_BACK;
    p->bytes = bytes;
    p->lock.rank = SPIN_SHARED_OBJECT;
    object_init(&p->object, &shared_ops, p);
    return p;
}
int64_t shared_open(struct shared_pages *p, uint32_t rights, struct handle **out) {
    struct handle *h = handle_open(&p->object, rights);
    if (!h) {
        shared_release(&p->object);
        return -N_ERR_NO_MEMORY;
    }
    *out = h;
    return 0;
}
struct shared_pages *shared_of(struct handle *h) { return handle_is(h, &shared_ops) ? h->object->priv : NULL; }

int64_t shared_object(size_t bytes, bool demand, struct shared_pages **out) {
    if (!bytes || bytes > SIZE_MAX - (PAGE_SIZE - 1)) return -N_ERR_INVALID;
    bytes = ALIGN_UP(bytes, PAGE_SIZE);
    size_t count = bytes / PAGE_SIZE;
    size_t slots = demand ? (count + SHARED_LEAF - 1) / SHARED_LEAF : count;
    if (!demand && count > pmm_free_pages()) return -N_ERR_NO_MEMORY;
    struct shared_pages *p = shared_new(bytes, demand ? SHARED_DEMAND : SHARED_RAM, slots);
    if (!p) return -N_ERR_NO_MEMORY;
    if (demand) p->count = count;
    else for (; p->count < count; p->count++) {
        p->pages[p->count] = pmm_alloc_zeroed();
        if (!p->pages[p->count]) {
            shared_release(&p->object);
            return -N_ERR_NO_MEMORY;
        }
    }
    *out = p;
    return 0;
}
int64_t shared_create(size_t bytes, bool demand, struct handle **out) {
    struct shared_pages *p;
    int64_t r = shared_object(bytes, demand, &p);
    return r < 0 ? r : shared_open(p, HANDLE_WRITE, out);
}

struct object *shared_kernel_new(size_t bytes, void **view) {
    struct shared_pages *p;
    if (shared_object(bytes, false, &p) < 0) return NULL;
    void *v = vmalloc_map_frames(p->pages, p->count, PTE_W | PTE_NX);
    if (!v) {
        shared_release(&p->object);
        return NULL;
    }
    object_ref(&p->object);
    *view = v;
    return &p->object;
}
void shared_kernel_end(struct object *o, void *view) {
    struct shared_pages *p = o->priv;
    vunmap(view, p->count);
    object_unref(o);
}
int64_t shared_kernel_handle(struct object *o) {
    struct handle *h = handle_open(o, HANDLE_WRITE);
    return h ? handle_install(h) : -N_ERR_NO_MEMORY;
}

int64_t shared_bar(struct handle *pci, uint32_t bar, struct handle **out) {
    uint64_t address = 0, size = 0;
    int64_t r = pci_user_bar(pci, bar, &address, &size);
    if (r < 0) return r;
    if (size > SIZE_MAX) return -N_ERR_TOO_LARGE;
    struct shared_pages *p = shared_new(size, SHARED_RANGE, 0);
    if (!p) return -N_ERR_NO_MEMORY;
    p->wc_allowed = pci_user_bar_prefetchable(pci, bar);
    p->claim = pci_user_hold(pci);
    p->base = address;
    p->count = size / PAGE_SIZE;
    p->cache = N_CACHE_UNCACHED;
    struct shared_claim *c = pci_user_shared(p->claim);
    mutex_lock(&c->lock);
    link_add(&c->ranges, &p->claim_link, p);
    mutex_unlock(&c->lock);
    return shared_open(p, HANDLE_WRITE, out);
}
int64_t shared_dma_create(struct handle *pci, size_t bytes, uint32_t flags, struct handle **out) {
    struct pci_claim *claim = pci_user_hold(pci);
    if (!claim) return -N_ERR_WRONG_KIND;
    int64_t r = 0;
    if (flags & ~(N_DMA_CONTIGUOUS | N_DMA_32BIT) || !bytes || bytes > SIZE_MAX - (PAGE_SIZE - 1)) r = -N_ERR_INVALID;
    else if (ALIGN_UP(bytes, PAGE_SIZE) / PAGE_SIZE > pmm_free_pages()) r = -N_ERR_NO_MEMORY;
    if (r < 0) {
        pci_user_drop(claim);
        return r;
    }
    bytes = ALIGN_UP(bytes, PAGE_SIZE);
    size_t count = bytes / PAGE_SIZE;
    unsigned bits = MIN(flags & N_DMA_32BIT ? 32u : 64u, pci_user_dma_bits(claim));
    struct shared_pages *p;
    if (flags & N_DMA_CONTIGUOUS) {
        uint64_t base = pmm_alloc_contig(count, bits);
        if (!base) {
            pci_user_drop(claim);
            return -N_ERR_NO_MEMORY;
        }
        if (!(p = shared_new(bytes, SHARED_RANGE, 0))) {
            pmm_free_contig(base, count);
            pci_user_drop(claim);
            return -N_ERR_NO_MEMORY;
        }
        p->claim = claim;
        memset(phys_to_virt(base), 0, bytes);
        p->base = base;
        p->count = count;
        p->owned = true;
    } else {
        if (!(p = shared_new(bytes, SHARED_RAM, count))) {
            pci_user_drop(claim);
            return -N_ERR_NO_MEMORY;
        }
        p->claim = claim;
        for (; p->count < count; p->count++) {
            uint64_t frame = pmm_alloc_contig(1, bits);
            if (!frame) {
                shared_release(&p->object);
                return -N_ERR_NO_MEMORY;
            }
            memset(phys_to_virt(frame), 0, PAGE_SIZE);
            p->pages[p->count] = frame;
        }
    }
    p->dma = true;
    if (!pci_user_dma_map(claim, p->kind == SHARED_RAM ? p->pages : NULL, p->base, p->count)) {
        shared_release(&p->object);
        return -N_ERR_NO_MEMORY;
    }
    return shared_open(p, HANDLE_WRITE, out);
}
int64_t shared_dma_pages(struct handle *h, size_t first, size_t count) {
    struct shared_pages *p = shared_of(h);
    if (!p) return -N_ERR_WRONG_KIND;
    if (cancelled(p) || shared_device_gone(p)) return -N_ERR_CLOSED;
    if (first >= p->count) return -N_ERR_INVALID;
    size_t n = MIN(count, p->count - first);
    for (size_t at = first; at - first < n;) {
        size_t page = at, span;
        uint8_t cache;
        struct shared_pages *leaf = shared_leaf(p, &page, &cache, NULL, &span);
        if (!leaf->dma && leaf->kind != SHARED_RANGE) return -N_ERR_WRONG_KIND;
        at += MIN(span, n - (at - first));
    }
    return (int64_t)n;
}
void shared_dma_frames(struct handle *h, size_t first, uint64_t *frames, size_t count) {
    struct shared_pages *p = shared_of(h);
    for (size_t i = 0; i < count; i++) frames[i] = shared_frame(p, first + i);
}

int64_t shared_view(struct handle *h, size_t offset, size_t bytes, uint32_t cache, uint32_t maximum, struct handle **out) {
    struct shared_pages *src = shared_of(h);
    if (!src) return -N_ERR_WRONG_KIND;
    if (!bytes || (offset | bytes) & (PAGE_SIZE - 1) || offset > src->bytes || bytes > src->bytes - offset ||
        cache > N_CACHE_UNCACHED || (maximum && maximum != N_MEMORY_READ && maximum != N_MEMORY_READ_WRITE))
        return -N_ERR_INVALID;
    struct shared_pages *root = shared_root(src);
    /* paged memory is its pager's to hand out page by page: no views of it */
    if (root->kind == SHARED_PAGED) return -N_ERR_WRONG_KIND;
    if (cancelled(src) || shared_device_gone(src)) return -N_ERR_CLOSED;
    uint32_t rights = h->rights;
    if (maximum == N_MEMORY_READ_WRITE && !(rights & HANDLE_WRITE)) return -N_ERR_DENIED;
    if (maximum == N_MEMORY_READ) rights &= ~HANDLE_WRITE;
    bool device = root->kind == SHARED_RANGE && !root->owned;
    int64_t r = 0;
    if (root->kind == SHARED_COMPOSITE) {
        /* A default view preserves each segment's type. An explicit single type must describe all
           selected pages; it cannot silently collapse heterogeneous BAR/RAM attributes into a
           different cache alias. */
        if (cache != N_CACHE_DEFAULT)
            for (size_t at = offset / PAGE_SIZE, left = bytes / PAGE_SIZE; left;) {
                size_t page = at, span;
                uint8_t type;
                shared_leaf(src, &page, &type, NULL, &span);
                if (cache != type) return -N_ERR_DENIED;
                span = MIN(span, left);
                at += span;
                left -= span;
            }
    } else if (root->dma) {
        /* The type changes only while nothing maps the root (maps), checked under its lock with
           typing set: a mapping made meanwhile counts first, or waits for the new type. The direct
           map changes with no lock held, as its shootdown waits for every CPU. */
        spin_take(&root->lock);
        while (root->typing) shared_sleep(&root->typed, &root->lock);
        bool retype = false;
        if (cache == N_CACHE_DEFAULT) cache = root->cache;
        else if (cache != root->cache) {
            if (root->cache != N_CACHE_WRITE_BACK || root->maps) r = -N_ERR_DENIED;
            else retype = root->typing = true;
        }
        spin_give(&root->lock);
        if (retype) {
            bool typed = vmm_direct_type(root->kind == SHARED_RAM ? root->pages : NULL, root->base, root->count, cache_pte(cache));
            spin_take(&root->lock);
            if (typed) __atomic_store_n(&root->cache, (uint8_t)cache, __ATOMIC_RELEASE);
            else r = -N_ERR_NO_MEMORY;
            root->typing = false;
            spin_give(&root->lock);
            wq_wake_all(&root->typed);
        }
    } else if (cache == N_CACHE_DEFAULT) cache = src->cache;
    else if (device ? (cache == N_CACHE_WRITE_BACK || (cache == N_CACHE_WRITE_COMBINE && !root->wc_allowed)) : cache != src->cache)
        r = -N_ERR_DENIED;
    if (r < 0) return r;
    struct shared_pages *p = shared_new(bytes, SHARED_VIEW, 0);
    if (!p) return -N_ERR_NO_MEMORY;
    p->count = bytes / PAGE_SIZE;
    p->first = offset / PAGE_SIZE;
    p->cache = (uint8_t)cache;
    p->parent = h->object;
    if (src->kind == SHARED_VIEW) {
        /* A view of a view points at the root object, so a chain of views (which any app can
           build, closing each parent) never grows what a page's resolving or a release walks. */
        p->parent = src->parent;
        p->first += src->first;
    }
    object_ref(p->parent);
    struct shared_pages *parent = p->parent->priv;
    spin_take(&parent->lock);
    link_add(&parent->derived, &p->derived_link, p);
    spin_give(&parent->lock);
    r = scope_new(p, src->scope);
    if (r < 0) {
        shared_release(&p->object);
        return r;
    }
    return shared_open(p, rights, out);
}
int64_t shared_compose(const struct shared_slice *slices, size_t count, uint32_t access, struct handle **out) {
    size_t bytes = 0, parts = 0, dependencies = 0;
    for (size_t i = 0; i < count; i++) {
        struct handle *f = slices[i].handle;
        struct shared_pages *src = shared_of(f);
        if (!src) return -N_ERR_WRONG_KIND;
        if (slices[i].offset > src->bytes || slices[i].bytes > src->bytes - slices[i].offset) return -N_ERR_INVALID;
        if (access == N_MEMORY_READ_WRITE && !handle_allows(f, HANDLE_WRITE)) return -N_ERR_DENIED;
        struct shared_pages *root = shared_root(src);
        if (root->kind == SHARED_PAGED) return -N_ERR_WRONG_KIND;
        if (cancelled(src) || shared_device_gone(src)) return -N_ERR_CLOSED;
        if (slices[i].bytes > SIZE_MAX - bytes) return -N_ERR_TOO_LARGE;
        bytes += slices[i].bytes;
        dependencies += src->scope != NULL;
        for (size_t at = slices[i].offset / PAGE_SIZE, left = slices[i].bytes / PAGE_SIZE; left;) {
            size_t page = at, span;
            uint8_t cache;
            shared_leaf(src, &page, &cache, NULL, &span);
            if (parts == SIZE_MAX) return -N_ERR_TOO_LARGE;
            parts++;
            span = MIN(span, left);
            at += span;
            left -= span;
        }
    }
    if (parts > SIZE_MAX / sizeof(struct shared_part)) return -N_ERR_TOO_LARGE;
    struct shared_pages *p = shared_new(bytes, SHARED_COMPOSITE, 0);
    if (!p) return -N_ERR_NO_MEMORY;
    p->count = bytes / PAGE_SIZE;
    p->parts = kzalloc(parts * sizeof *p->parts);
    if (dependencies) p->dependencies = kzalloc(dependencies * sizeof *p->dependencies);
    int64_t r = !p->parts || (dependencies && !p->dependencies) ? -N_ERR_NO_MEMORY : scope_new(p, NULL);
    size_t end = 0;
    for (size_t i = 0; r >= 0 && i < count; i++) {
        struct shared_pages *src = shared_of(slices[i].handle);
        if (src->scope) {
            r = scope_depend(src->scope, p, &p->dependencies[p->dependency_count]);
            if (r < 0) break;
            p->dependency_count++;
        }
        for (size_t at = slices[i].offset / PAGE_SIZE, left = slices[i].bytes / PAGE_SIZE; left;) {
            size_t page = at, span;
            uint8_t cache;
            struct object *backing = slices[i].handle->object;
            shared_leaf(src, &page, &cache, &backing, &span);
            span = MIN(span, left);
            end += span;
            object_ref(backing);
            struct shared_part *part = &p->parts[p->part_count++];
            part->object = backing;
            part->first = page;
            part->end = end;
            part->cache = cache;
            struct shared_pages *root = backing->priv;
            spin_take(&root->lock);
            link_add(&root->derived, &part->derived, p);
            spin_give(&root->lock);
            at += span;
            left -= span;
        }
    }
    if (r >= 0 && (cancelled(p) || shared_device_gone(p))) r = -N_ERR_CLOSED;
    if (r < 0) {
        shared_release(&p->object);
        return r;
    }
    return shared_open(p, access == N_MEMORY_READ_WRITE ? HANDLE_WRITE : 0, out);
}
int64_t shared_revoke(struct handle *h) {
    struct shared_pages *p = shared_of(h);
    if (!p) return -N_ERR_WRONG_KIND;
    struct shared_scope *scope = p->scope, *work = NULL;
    if (!scope || scope->issuer != process_current()->pid) return -N_ERR_DENIED;
    mutex_lock(&revoke_mutex);
    scope_queue(scope, &work);
    while (work) {
        struct shared_scope *s = work;
        work = s->work_next;
        scope_visit(s, &work);
        scope_drop(s);
    }
    mutex_unlock(&revoke_mutex);
    return 0;
}

/* ---- withdrawing mappings ---- */

void shared_withdraw(struct shared_pages *p, uint64_t offset, uint64_t bytes, bool write_only, bool cancel) {
    spin_take(&p->lock);
    for (struct shared_link *l = p->mappings; l;) {
        struct shared_mapping *m = l->owner;
        if (cancel && m->building) m->cancelled = true;
        m->busy++;
        struct vmm_space *space = m->owner->vmm_space;
        uint64_t va = m->va + offset, n = bytes ? bytes : m->bytes;
        spin_give(&p->lock);
        if (write_only) vmm_user_write_protect(space, va, n);
        else vmm_user_free(space, va, n);
        spin_take(&p->lock);
        l = l->next;
        if (!--m->busy) wq_wake_all(&m->idle);
    }
    spin_give(&p->lock);
}

/* ---- device memory's types ---- */

/* Device memory has one memory type at a time. x86 leaves a physical page reached through linear
   addresses of different types undefined (Intel SDM vol. 3A, PAT: aliasing is not supported), so
   every mapping of a device range pins the type of its pages and a mapping asking another type of
   a pinned page is refused while the first lives. The boot screen stays write-combined in the
   direct map for as long as the system runs. A pin belongs to the claim its range came through and
   goes with that claim (shared_revoke_claim): no mapping reaches those pages after. Only making a
   mapping and taking one down come here. */
struct device_pin {
    struct device_pin *next;
    const struct pci_claim *claim;
    uint64_t start, end;
    size_t refs;
    uint8_t cache;
};
static struct spinlock device_pin_lock = {.rank = SPIN_DEVICE_PINS};
static struct device_pin *device_pins;
/* One change of the pins: the records it may take for new pins (allocated before), the records it
   let go and a refused type, both dealt with after (pin_work_end). */
struct pin_work {
    struct device_pin *spares, *gone;
    int64_t error;
    uint64_t start, end;
    uint8_t cache, other; /* the refused mapping, when error is -N_ERR_DENIED */
};
static void pin_work_spares(struct pin_work *w, size_t n) {
    for (; n; n--) {
        struct device_pin *d = kmalloc(sizeof *d);
        if (!d) return; /* a pin that finds no record fails with -N_ERR_NO_MEMORY */
        d->next = w->spares;
        w->spares = d;
    }
}
static const char *cache_name(uint8_t cache) {
    return cache == N_CACHE_WRITE_COMBINE ? "write-combined" : cache == N_CACHE_UNCACHED ? "uncached" : "write-back";
}
/* The refusal said, the records freed; the error. */
static int64_t pin_work_end(struct pin_work *w) {
    if (w->error == -N_ERR_DENIED) {
        struct process *t = process_current();
        kprintf("[shared] pid %d (%s): device memory in %#lx-%#lx is mapped %s; a %s mapping is refused\n",
                t ? t->pid : 0, t ? t->name : "kernel", (unsigned long)w->start, (unsigned long)w->end - 1, cache_name(w->other),
                cache_name(w->cache));
    }
    for (int list = 0; list < 2; list++)
        for (struct device_pin *d = list ? w->gone : w->spares, *next; d; d = next) {
            next = d->next;
            kfree(d);
        }
    w->spares = w->gone = NULL;
    return w->error;
}
/* The device segments p may map (one per BAR or range part): the most records pinning it takes. */
static size_t device_segments(struct shared_pages *p) {
    struct shared_pages *root = shared_root(p);
    if (root->kind != SHARED_COMPOSITE) return root->kind == SHARED_RANGE && !root->owned;
    size_t n = 0;
    for (size_t i = 0; i < root->part_count; i++) {
        struct shared_pages *leaf = root->parts[i].object->priv;
        n += leaf->kind == SHARED_RANGE && !leaf->owned;
    }
    return n;
}
static bool device_pin(const struct pci_claim *claim, uint64_t start, uint64_t end, uint8_t cache, struct pin_work *w) {
    uint64_t screen = ALIGN_DOWN(fb.phys, PAGE_SIZE), screen_end = ALIGN_UP(fb.phys + (uint64_t)fb.pitch * fb.height, PAGE_SIZE);
    uint8_t other = fb.addr && start < screen_end && screen < end && cache != N_CACHE_WRITE_COMBINE ?
                    N_CACHE_WRITE_COMBINE : N_CACHE_DEFAULT;
    struct device_pin *same = NULL;
    spin_take(&device_pin_lock);
    for (struct device_pin *d = device_pins; d && other == N_CACHE_DEFAULT; d = d->next) {
        if (start < d->end && d->start < end && d->cache != cache) other = d->cache;
        if (d->claim == claim && d->start == start && d->end == end && d->cache == cache) same = d;
    }
    bool ok = false;
    if (other != N_CACHE_DEFAULT) {
        w->error = -N_ERR_DENIED;
        w->start = start;
        w->end = end;
        w->cache = cache;
        w->other = other;
    } else if (same) {
        same->refs++;
        ok = true;
    } else if (w->spares) {
        struct device_pin *d = w->spares;
        w->spares = d->next;
        *d = (struct device_pin){.next = device_pins, .claim = claim, .start = start, .end = end, .refs = 1, .cache = cache};
        device_pins = d;
        ok = true;
    } else {
        w->error = -N_ERR_NO_MEMORY;
    }
    spin_give(&device_pin_lock);
    return ok;
}
static void device_unpin(const struct pci_claim *claim, uint64_t start, uint64_t end, uint8_t cache, struct pin_work *w) {
    spin_take(&device_pin_lock);
    for (struct device_pin **link = &device_pins; *link; link = &(*link)->next) {
        struct device_pin *d = *link;
        if (d->claim != claim || d->start != start || d->end != end || d->cache != cache) continue;
        if (!--d->refs) {
            *link = d->next;
            d->next = w->gone;
            w->gone = d;
        }
        spin_give(&device_pin_lock);
        return;
    }
    spin_give(&device_pin_lock);
    ASSERT(pci_user_revoked(claim)); /* its pins went with the claim */
}
/* Pins (add) or unpins the type of the first `limit` device segments p maps. Adding stops at a
   segment whose pages have another type, or without a record for it (w->error); the return is the
   number of segments done. */
static size_t device_pins_walk(struct shared_pages *p, bool add, size_t limit, struct pin_work *w) {
    size_t done = 0;
    for (size_t at = 0; at < p->count && done < limit;) {
        size_t page = at, span;
        uint8_t cache;
        struct shared_pages *leaf = shared_leaf(p, &page, &cache, NULL, &span);
        span = MIN(span, p->count - at);
        at += span;
        if (leaf->kind != SHARED_RANGE || leaf->owned) continue;
        uint64_t start = leaf->base + page * PAGE_SIZE, end = start + span * PAGE_SIZE;
        if (add && !device_pin(leaf->claim, start, end, cache, w)) break;
        if (!add) device_unpin(leaf->claim, start, end, cache, w);
        done++;
    }
    return done;
}
/* Every device segment of p pinned, or none (the error). */
static int64_t shared_device_pin(struct shared_pages *p, struct pin_work *w) {
    size_t done = device_pins_walk(p, true, SIZE_MAX, w);
    if (w->error) device_pins_walk(p, false, done, w);
    return w->error;
}
static void shared_device_unpin(struct shared_pages *p, struct pin_work *w) { device_pins_walk(p, false, SIZE_MAX, w); }
static void device_pins_drop(const struct pci_claim *claim, struct pin_work *w) {
    spin_take(&device_pin_lock);
    for (struct device_pin **link = &device_pins; *link;) {
        struct device_pin *d = *link;
        if (d->claim == claim) {
            *link = d->next;
            d->next = w->gone;
            w->gone = d;
        } else link = &d->next;
    }
    spin_give(&device_pin_lock);
}

/* The screen drawn into a driver's surface (mm/shared.h). It counts as a mapping of the surface:
   its pages keep their type while the screen is there, and a DMA object's type stays fixed. A view
   or root of a BAR or a DMA object: one root. */
int64_t shared_scanout_open(struct handle *surface, const struct pci_claim *claim, uint64_t offset, uint64_t bytes,
                            struct shared_scanout *out) {
    struct shared_pages *p = shared_of(surface);
    if (!p) return -N_ERR_WRONG_KIND;
    if (!handle_allows(surface, HANDLE_WRITE)) return -N_ERR_DENIED;
    if (cancelled(p) || shared_device_gone(p)) return -N_ERR_CLOSED;
    struct shared_pages *root = shared_root(p);
    bool bar = root->kind == SHARED_RANGE && !root->owned && !root->dma;
    if (root->claim != claim || (!bar && !root->dma)) return -N_ERR_DENIED; /* another claim's, or no device's */
    if (!bytes || offset > p->bytes || bytes > p->bytes - offset) return -N_ERR_INVALID;
    if (bar && !root->wc_allowed) return -N_ERR_DENIED;
    size_t first = offset / PAGE_SIZE, pages = (offset + bytes - 1) / PAGE_SIZE - first + 1;
    uint64_t *frames = kmalloc(pages * sizeof *frames);
    if (!frames) return -N_ERR_NO_MEMORY;
    for (size_t i = 0; i < pages; i++) frames[i] = shared_frame(p, first + i);
    struct pin_work pins = {0};
    uint8_t cache = N_CACHE_WRITE_COMBINE;
    if (bar) {
        pin_work_spares(&pins, 1);
        device_pin(root->claim, frames[0], frames[pages - 1] + PAGE_SIZE, cache, &pins); /* a BAR's pages are one run */
    } else {
        /* a DMA root's type is read with its maps: a view changes it only while nothing maps it */
        spin_take(&root->lock);
        cache = root->cache;
        if (cache == N_CACHE_WRITE_BACK) pins.error = -N_ERR_DENIED;
        else root->maps++;
        spin_give(&root->lock);
    }
    int64_t error = pin_work_end(&pins);
    if (error) {
        kfree(frames);
        return error;
    }
    object_ref(surface->object);
    *out = (struct shared_scanout){.object = surface->object, .frames = frames, .pages = pages,
                                   .first_byte = offset % PAGE_SIZE, .type = cache_pte(cache)};
    return 0;
}
void shared_scanout_close(struct shared_scanout *s) {
    if (!s->object) return;
    struct shared_pages *p = s->object->priv, *root = shared_root(p);
    if (root->dma) {
        spin_take(&root->lock);
        root->maps--;
        spin_give(&root->lock);
    } else {
        struct pin_work pins = {0};
        device_unpin(root->claim, s->frames[0], s->frames[s->pages - 1] + PAGE_SIZE, N_CACHE_WRITE_COMBINE, &pins);
        pin_work_end(&pins);
    }
    kfree(s->frames);
    object_unref(s->object);
    *s = (struct shared_scanout){0};
}

/* ---- a claim's end ---- */

/* p's mappings go, and those of what was made from it: a view or composite of a root, a view of a
   composite (the recursion goes no deeper). Each moves to the ended list on its way: a claim ends
   once, and what is made after finds the device gone. */
static void withdraw_with_derived(struct shared_pages *p) {
    shared_withdraw(p, 0, 0, false, true);
    for (;;) {
        struct shared_pages *d = NULL;
        spin_take(&p->lock);
        struct shared_link *l = p->derived;
        if (l) {
            link_del(l);
            link_add(&p->derived_ended, l, l->owner);
            if (object_ref_live(&((struct shared_pages *)l->owner)->object)) d = l->owner;
        }
        spin_give(&p->lock);
        if (!l) return;
        if (d) {
            withdraw_with_derived(d);
            object_unref(&d->object);
        }
    }
}
/* The claim ended (dev/pciuser.c marked it released before): every mapping that reaches its
   device's ranges loses their pages, one being built fails, and the pins go. A fault there finds
   the claim released and maps gone_device_page. */
void shared_revoke_claim(struct pci_claim *claim) {
    struct shared_claim *c = pci_user_shared(claim);
    mutex_lock(&revoke_mutex);
    for (;;) {
        struct shared_pages *range = NULL;
        mutex_lock(&c->lock);
        struct shared_link *l = c->ranges;
        if (l) {
            link_del(l);
            link_add(&c->ended, l, l->owner);
            if (object_ref_live(&((struct shared_pages *)l->owner)->object)) range = l->owner;
        }
        mutex_unlock(&c->lock);
        if (!l) break;
        if (range) {
            withdraw_with_derived(range);
            object_unref(&range->object);
        }
    }
    mutex_unlock(&revoke_mutex);
    struct pin_work pins = {0};
    device_pins_drop(claim, &pins);
    pin_work_end(&pins);
}

/* ---- mappings ---- */

/* The PTE flags page `page` (counted from the mapping's start) of m is mapped with: the mapping's
   own, or the access n_memory_protect set there, which never exceeds them (shared_protect). Its
   object's lock held. */
static uint64_t shared_mapping_flags(const struct shared_mapping *m, size_t page) {
    uint64_t leaf = m->access ? m->access[page / PAGE_SIZE] : 0;
    uint8_t access = leaf ? ((const uint8_t *)phys_to_virt(leaf))[page % PAGE_SIZE] : 0;
    if (!access) return m->flags;
    uint64_t flags = access == N_MEMORY_READ_WRITE ? m->flags : m->flags & ~PTE_W;
    return access == N_MEMORY_READ_EXECUTE ? flags & ~PTE_NX : flags | PTE_NX;
}
static size_t shared_access_leaves(const struct shared_mapping *m) {
    return (m->bytes / PAGE_SIZE + PAGE_SIZE - 1) / PAGE_SIZE;
}
/* The access is kept for each page, also one not mapped now (paged memory, an untouched demand
   page, a page of a device gone), so every PTE a later fault installs keeps to it; the present
   pages change at once. A fault that maps a page reads the access with the object's lock held and
   maps it before it lets go: it maps the page with the access set here, or before
   vmm_user_protect changes it. */
int64_t shared_protect(struct process *owner, struct shared_mapping *m, uint64_t va, uint64_t bytes, int access, int vp) {
    struct shared_pages *p = mapping_pages(m);
    if (va < m->va || bytes > m->bytes - (va - m->va)) return -N_ERR_NO_MEMORY; /* reaches past it: its guard page */
    if (cancelled(p)) return -N_ERR_CLOSED;
    /* Write access only in a writable mapping. Execution never: shared memory is mapped without
       it (n_shared_map refuses N_MEMORY_READ_EXECUTE), and a program's code (PTE_SHARED_RO) keeps
       the access it was mapped with. Else an app could execute pages it, or another holder of the
       memory, writes through a second mapping. */
    if (((vp & VM_W) && !(m->flags & PTE_W)) || ((vp & VM_X) && (m->flags & (PTE_NX | PTE_SHARED_RO)))) return -N_ERR_DENIED;
    spin_take(&p->lock);
    bool missing = !m->access;
    spin_give(&p->lock);
    uint64_t *spare = NULL;
    if (missing && !(spare = kzalloc(shared_access_leaves(m) * sizeof *spare))) return -N_ERR_NO_MEMORY;
    size_t first = (va - m->va) / PAGE_SIZE, end = first + bytes / PAGE_SIZE;
    for (size_t i = first / PAGE_SIZE; i <= (end - 1) / PAGE_SIZE; i++) {
        spin_take(&p->lock);
        bool have = m->access && m->access[i];
        spin_give(&p->lock);
        uint64_t leaf = have ? 0 : pmm_alloc_zeroed();
        if (!have && !leaf) {
            kfree(spare);
            return -N_ERR_NO_MEMORY;
        }
        spin_take(&p->lock);
        if (!m->access) { /* else a protection of another part made them */
            m->access = spare;
            spare = NULL;
        }
        if (leaf && !m->access[i]) {
            m->access[i] = leaf;
            leaf = 0;
        }
        spin_give(&p->lock);
        if (leaf) pmm_free(leaf);
    }
    kfree(spare);
    for (size_t at = first; at < end;) {
        size_t n = MIN(end - at, PAGE_SIZE - at % PAGE_SIZE);
        spin_take(&p->lock);
        memset((uint8_t *)phys_to_virt(m->access[at / PAGE_SIZE]) + at % PAGE_SIZE, access, n);
        spin_give(&p->lock);
        at += n;
    }
    return vmm_user_protect(owner->vmm_space, va, bytes, vp, true);
}
int64_t shared_map(struct handle *h, size_t bytes, int access) {
    struct process *owner = process_current();
    struct shared_pages *p = shared_of(h);
    if (!p) return -N_ERR_WRONG_KIND;
    /* the handle's own rights bound the mapping's: a read-only handle maps read-only */
    if (access == N_MEMORY_READ_WRITE && !handle_allows(h, HANDLE_WRITE)) return -N_ERR_DENIED;
    if (cancelled(p) || shared_device_gone(p)) return -N_ERR_CLOSED;
    bytes = ALIGN_UP(bytes, PAGE_SIZE);
    if (bytes != p->bytes) return -N_ERR_INVALID;
    struct shared_mapping *m = kzalloc(sizeof *m);
    if (!m) return -N_ERR_NO_MEMORY;
    /* the mapping holds the object itself: the handle may be closed while it stays */
    object_ref(&p->object);
    uint64_t flags = PTE_U | PTE_SHARED | PTE_NX | (access == N_MEMORY_READ_WRITE ? PTE_W : PTE_SHARED_RO);
    m->object = &p->object;
    m->flags = flags;
    m->bytes = bytes;
    m->owner = owner;
    m->building = true;
    /* The DMA types held and the device pages' types pinned, the range is reserved (reusing
       released ones) and the mapping listed at once, before any PTE: no other reservation can
       choose the partially mapped span, and a revocation from here on finds the mapping. One that
       came before it, the mapping sees under its object's lock. */
    struct pin_work pins = {0};
    struct mmap_hole *gone = NULL;
    pin_work_spares(&pins, device_segments(p));
    shared_dma_hold(p);
    uint64_t va = 0;
    if (!shared_device_pin(p, &pins)) {
        uint64_t lock = spin_lock(&owner->space.lock);
        spin_take(&owner->mappings_lock);
        spin_take(&p->lock);
        va = space_take_locked(owner, false, bytes, &gone);
        if (va) {
            m->va = va;
            m->cancelled = cancelled(p) || shared_device_gone(p);
            m->span.start = va;
            m->span.length = bytes;
            span_insert(&owner->shared_mappings, &m->span);
            link_add(&p->mappings, &m->in_object, m);
        }
        spin_give(&p->lock);
        spin_give(&owner->mappings_lock);
        spin_unlock(&owner->space.lock, lock);
        if (!va) shared_device_unpin(p, &pins);
    }
    holes_free(gone);
    int64_t pinned = pin_work_end(&pins);
    if (!va) {
        shared_dma_release(p);
        object_unref(&p->object);
        kfree(m);
        return pinned < 0 ? pinned : -N_ERR_NO_MEMORY;
    }
    /* Demand pages not yet touched by any peer stay unmapped until a fault. */
    size_t mapped = 0;
    for (; mapped < p->count; mapped++) {
        size_t index = mapped;
        uint8_t cache;
        spin_take(&p->lock);
        if (m->cancelled || thread_current()->killed) {
            spin_give(&p->lock);
            break;
        }
        struct shared_pages *leaf = shared_leaf(p, &index, &cache, NULL, NULL);
        uint64_t pte = flags | cache_pte(cache), frame;
        if (leaf->kind == SHARED_PAGED) {
            spin_take(&leaf->pager->lock);
            struct object **source;
            uint64_t *slot = paged_slot(leaf, index, &source), entry = slot ? *slot : 0;
            spin_give(&leaf->pager->lock);
            frame = entry & PAGED_BOUND ? entry & PTE_ADDR : 0;
            pte = paged_page_flags(entry, flags) | cache_pte(cache);
        } else {
            frame = leaf_frame(leaf, index);
        }
        bool ok = !frame || !vmm_map_page(owner->vmm_space, va + mapped * PAGE_SIZE, frame, pte);
        spin_give(&p->lock);
        if (!ok) break;
    }
    /* A revocation during construction sets cancelled under the object's lock; looked at with the
       end of building under the same lock, it cannot be missed and leave the mapping live after. */
    struct space_op op;
    uint64_t lock = spin_lock(&owner->space.lock);
    spin_take(&p->lock);
    int64_t result = m->cancelled || cancelled(p) || shared_device_gone(p) ? -N_ERR_CLOSED :
                     thread_current()->killed ? -N_ERR_STOPPED : -N_ERR_NO_MEMORY;
    bool failed = mapped != p->count || result != -N_ERR_NO_MEMORY;
    m->building = false;
    if (failed) {
        m->leaving = true;
        space_op_add(owner, &op, va, va + bytes);
    }
    spin_give(&p->lock);
    spin_unlock(&owner->space.lock, lock);
    if (!failed) return (int64_t)va;
    shared_unmap(owner, m, &op, true);
    return result;
}
void shared_unmap(struct process *owner, struct shared_mapping *m, struct space_op *op, bool hole) {
    struct shared_pages *p = mapping_pages(m);
    struct mmap_hole *spare = hole ? hole_spare() : NULL, *gone = NULL;
    /* Keep every DMA cache pin through PTE removal and the completed TLB shootdown. A view cannot
       retype still-reachable old pages. */
    vmm_user_free(owner->vmm_space, m->va, m->bytes);
    spin_take(&p->lock);
    while (m->busy) shared_sleep(&m->idle, &p->lock);
    link_del(&m->in_object);
    spin_give(&p->lock);
    /* A fault that found it in the tree holds its object's lock by the time this CPU has the tree,
       or finds it gone: it is freed only after the object's lock was passed here. */
    uint64_t lock = spin_lock(&owner->space.lock);
    spin_take(&owner->mappings_lock);
    spin_take(&p->lock);
    span_remove(&owner->shared_mappings, &m->span);
    spin_give(&p->lock);
    spin_give(&owner->mappings_lock);
    if (hole) hole_add_locked(owner, m->va, m->bytes + PAGE_SIZE, &spare, &gone);
    space_op_remove(owner, op);
    spin_unlock(&owner->space.lock, lock);
    wq_wake_all(&owner->space);
    kfree(spare);
    holes_free(gone);
    struct pin_work pins = {0};
    shared_dma_release(p);
    shared_device_unpin(p, &pins);
    pin_work_end(&pins);
    object_unref(m->object);
    for (size_t i = 0; m->access && i < shared_access_leaves(m); i++)
        if (m->access[i]) pmm_free(m->access[i]);
    kfree(m->access);
    kfree(m);
}
void shared_process_end(struct process *owner) {
    /* No thread of the app runs, so none builds or takes down a mapping; revocations elsewhere may
       still be at work on its mappings (shared_unmap waits for them). */
    for (;;) {
        struct space_op op;
        uint64_t lock = spin_lock(&owner->space.lock);
        struct shared_mapping *m = shared_mapping_of(span_from(&owner->shared_mappings, 0));
        if (m) {
            struct shared_pages *p = mapping_pages(m);
            ASSERT(!m->building && !m->leaving);
            spin_take(&p->lock);
            m->leaving = true;
            spin_give(&p->lock);
            space_op_add(owner, &op, m->va, m->va + m->bytes);
        }
        spin_unlock(&owner->space.lock, lock);
        if (!m) break;
        shared_unmap(owner, m, &op, false);
    }
    space_end(owner);
    programs_trim(0); /* its programs may be idle now */
}

/* ---- touches ---- */

/* What revoked device mappings show: one kernel page, all-ones, stands in for every register and
   aperture page: reads see a function gone from the bus, writes (a doorbell) land nowhere that
   matters. Made once, at the first touch of one. */
static uint64_t gone_device_page;
static bool gone_device_page_make(void) {
    uint64_t page = pmm_alloc(), none = 0;
    if (!page) return false;
    memset(phys_to_virt(page), 0xFF, PAGE_SIZE);
    if (!__atomic_compare_exchange_n(&gone_device_page, &none, page, false, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) pmm_free(page);
    return true;
}
/* The mapping is looked up and the page mapped with the object's lock held. A mapping still being
   built or being taken down maps nothing: the touch fails. A frame is prepared with no lock held
   and the mapping looked up again after, as it is after a program's page is read from its document
   or a pager's page waited for (*slept). */
bool shared_fault(uint64_t addr, bool write, bool *slept) {
    struct process *owner = process_current();
    if (!owner) return false; /* a kernel thread has no app memory */
    uint64_t page = ALIGN_DOWN(addr, PAGE_SIZE), present = vmm_get_pte(owner->vmm_space, page);
    if (present & PTE_P) {
        /* A sibling mapped it first, or a write to a page without write access, which only a
           pager's page can be given (below); anything else stays a protection violation. */
        if (!write || (present & PTE_W)) return true;
        if (!(present & PTE_SHARED)) return false;
    } else {
        int reserved = vmm_demand_fault(owner->vmm_space, addr, write); /* a private reserved page */
        if (reserved > 0) return true;
        if (reserved < 0) {
            kprintf("[memory] pid %d (%s): no frame for reserved page %p, %lu pages free\n",
                    owner->pid, owner->name, (void *)page, (unsigned long)pmm_free_pages());
            process_memory_report(owner, PAGE_SIZE);
            return false;
        }
    }
    struct pager_fault *spare = NULL; /* a record pager_wait let the lock go for */
    bool ok, starved;
again:
    ok = false;
    starved = false;
    spin_take(&owner->mappings_lock);
    struct shared_mapping *m = shared_mapping_of(span_at(&owner->shared_mappings, page));
    if (!m) {
        spin_give(&owner->mappings_lock);
        kfree(spare);
        return false;
    }
    struct shared_pages *p = mapping_pages(m), *fill = NULL;
    spin_take(&p->lock);
    spin_give(&owner->mappings_lock);
    struct handle *source = NULL;
    size_t at = 0;
    bool make_gone = false;
    do {
        if (m->building || m->leaving || cancelled(p)) break;
        size_t index = (page - m->va) / PAGE_SIZE;
        uint8_t cache;
        uint64_t flags = shared_mapping_flags(m, index);
        struct shared_pages *leaf = shared_leaf(p, &index, &cache, NULL, NULL);
        if (leaf->kind == SHARED_PAGED) {
            if (write && !(flags & PTE_W)) break;
            struct pager *g = leaf->pager;
            spin_take(&g->lock);
            struct object **from;
            uint64_t *slot = paged_slot(leaf, index, &from), entry = slot ? *slot : 0;
            if (entry && cancelled((*from)->priv)) entry = 0; /* its source's revocation is unbinding it */
            if ((entry & PAGED_BOUND) && (!write || (entry & PAGED_WRITE))) {
                spin_give(&g->lock);
                int r = vmm_map_page(owner->vmm_space, page, entry & PTE_ADDR, paged_page_flags(entry, flags));
                ok = !r || r == -N_ERR_EXISTS; /* exists: a sibling's map, the access retried */
                starved = r == -N_ERR_NO_MEMORY;
                break;
            }
            /* The pager is asked; the mapping is looked up again once it answered. */
            spin_give(&p->lock);
            int answered = pager_wait(g, index, write, &spare);
            if (answered < 0) {
                kfree(spare);
                return false;
            }
            if (slept) *slept = true;
            goto again;
        }
        if (leaf->kind == SHARED_RANGE && !leaf->owned && pci_user_revoked(leaf->claim)) {
            if (write && !(flags & PTE_W)) break;
            uint64_t stand_in = __atomic_load_n(&gone_device_page, __ATOMIC_ACQUIRE);
            if (!stand_in) {
                make_gone = true;
                break;
            }
            int r = vmm_map_page(owner->vmm_space, page, stand_in, flags & ~(PTE_PWT | PTE_PCD | PTE_PAT4K));
            ok = !r || r == -N_ERR_EXISTS;
            starved = r == -N_ERR_NO_MEMORY;
            break;
        }
        /* Claim loss removes the entire composite mapping. Its other RAM or live-device segments
           still refer to their own valid backing. */
        if ((leaf->kind != SHARED_DEMAND && !shared_device_gone(p)) || (write && !(flags & PTE_W))) break;
        uint64_t now = vmm_get_pte(owner->vmm_space, page);
        if (now & PTE_P) {
            ok = !write || (now & PTE_W);
            break;
        }
        uint64_t frame = leaf_frame(leaf, index);
        if (!frame) {
            /* Its page comes from its document, or is zeroed, with no lock held; the object and
               its opening stay meanwhile. */
            fill = leaf;
            at = index;
            if (leaf->source) source = handle_ref(leaf->source);
            object_ref(&fill->object);
            break;
        }
        int r = vmm_map_page(owner->vmm_space, page, frame, flags | cache_pte(cache));
        ok = !r || r == -N_ERR_EXISTS;
        starved = r == -N_ERR_NO_MEMORY;
    } while (0);
    spin_give(&p->lock);
    if (make_gone) {
        if (gone_device_page_make()) goto again;
        starved = true;
    }
    if (fill) {
        int r;
        if (source) {
            r = program_fill(fill, source, at);
            handle_close(source);
            if (slept) *slept = true;
        } else {
            r = demand_make(fill, at);
        }
        object_unref(&fill->object);
        if (r >= 0) goto again; /* mapped from the frame now there, if the mapping still is */
        if (r == -N_ERR_NO_MEMORY) starved = true;
        else kprintf("[program] pid %d (%s): page %p could not be read from the program (%d)\n",
                     owner->pid, owner->name, (void *)page, r);
    }
    kfree(spare);
    if (starved) {
        kprintf("[shared] pid %d (%s): no frame for page %p, %lu pages free\n",
                owner->pid, owner->name, (void *)page, (unsigned long)pmm_free_pages());
        process_memory_report(owner, PAGE_SIZE);
    }
    return ok;
}
