/* The kernel heap (mm/heap.h): an object smaller than a page from a slab of its size class, a
   larger one as whole pages.

   A slab is a run of 2^order frames used through the direct map, cut into objects of one class;
   nothing is mapped or unmapped for it, so nothing waits for a TLB shootdown. Every frame of a
   slab carries the heap's mark (pmm_mark): its class, its slab's order and home, and its slab's
   record, so that kfree reads one frame record and no more. An object of pages is a run of
   frames, its count in the first one's mark; when no run of free frames holds it, vmalloc's
   pages, marked the same way.

   Free objects wait in front of the slabs in chains linked through their first words. Each CPU
   has three per class: `loaded`, taken from and given to; `previous`, full or empty, which with
   loaded makes Bonwick's two magazines; and `away`, the objects of other homes' slabs, given back
   to them a chain at a time. Behind them each node keeps a depot of full chains per class, which
   a CPU's chain joins or leaves in one step. A CPU works its own chains with its interrupts off,
   which keeps it on that CPU too, and takes no lock; a depot and the slabs behind it are under the
   depot's lock.

   A CPU's home is the node its memory comes from first (arch/topology.h). A slab belongs to the
   home of the CPU that made it, wherever its frames came from: its depot lists it, and its objects
   go back to that depot's chains or to the slab. A chain is B objects, B set per node and class
   so that the chains of every CPU at home there (three and a share of the depot each) hold at
   most 1/CACHE_SHARE of the node's memory, the share pmm's CPU caches keep too.

   Locks: a depot's, never two at once and none inside it; pmm's are taken outside it. */
#include "kernel.h"
#include "arch/cpu.h"
#include "arch/percpu.h"
#include "arch/spin.h"
#include "arch/topology.h"
#include "mm/heap.h"
#include "mm/pmm.h"
#include "mm/vmalloc.h"
#include "mm/vmm.h"

#define GRAIN 16 /* the classes up to SMALL: one per GRAIN bytes */
#define SMALL_SHIFT 7
#define SMALL (1u << SMALL_SHIFT)
#define STEP_BITS 2 /* above SMALL: 2^STEP_BITS classes per power of two */
#define STEPS (1u << STEP_BITS)
#define PAGE_BITS __builtin_ctzll(PAGE_SIZE)
/* every class below a page: the classes up to SMALL, then STEPS per power of two up to a page's */
#define CLASSES (SMALL / GRAIN + STEPS * (PAGE_BITS - SMALL_SHIFT) - 1)
/* A slab is at most 2^ORDER_MOST frames, at least OBJECTS_LEAST objects when that fits, and wastes
   at most 1/WASTE of itself. Longer runs are the likelier to be missing as memory fragments (and
   nothing here waits for them): when its run is missing a slab is one frame. */
#define ORDER_MOST 3
#define OBJECTS_LEAST 8
#define WASTE 8
#define CHAIN_MOST 64    /* the most objects a chain moves under a depot's lock */
#define CACHE_SHARE 1024 /* as pmm's (mm/pmm.c) */
#define CHAINS_PER_CPU 4 /* loaded, previous, away, and the one the depot keeps for it */

enum { MARK_SLAB = 1, MARK_PAGES }; /* the kinds of the heap's marks; MARK_PAGES: word = frames */

struct class {
    uint32_t size;
    uint32_t inverse; /* ceil(2^32 / size): an offset in a slab times it, >> 32, is its object's index */
    uint8_t order;    /* its slabs' when that run is free */
    uint16_t objects[ORDER_MOST + 1]; /* a slab's of each order */
    bool inside[ORDER_MOST + 1];      /* a slab of that order keeps its record in its last bytes */
};

/* A slab's record: in the slab's last bytes when they are room left over (and always in the record
   class's own slabs), else an object of the record class. */
struct slab {
    struct slab *next, *prev; /* its depot's partial list */
    void *free;               /* its objects given back, through their first words */
    uintptr_t base;           /* its first byte (the direct map) */
    uint32_t used;            /* its objects given out: in use or waiting in chains */
    uint32_t fresh;           /* its objects from this index on were never given out */
    uint16_t home;
    uint8_t class, order;
};

struct depot {
    struct spinlock lock;
    struct slab *partial; /* its slabs that have objects to give, but the spare */
    struct slab *spare;   /* one empty slab kept */
    void **chains;        /* full chains: a chain's first object's second word is the next chain */
    unsigned count, most; /* the chains it holds, and the most it keeps: one per CPU at home */
} __attribute__((aligned(64)));

struct chain {
    void **first;
    unsigned count;
};

struct cpu_class {
    struct chain loaded, previous, away;
    unsigned most; /* B: its home's chain of the class */
};

PERCPU static struct heap_cpu {
    struct cpu_class class[CLASSES];
    int64_t used; /* bytes given out on this CPU less the bytes freed on it */
    unsigned home;
} heap_cpu;

static struct class classes[CLASSES];
static unsigned record_class;
static struct depot **depots; /* [node][CLASSES]; NULL for a node that is no CPU's home */

static unsigned class_of(size_t size) {
    if (size <= SMALL) return size ? (unsigned)((size - 1) / GRAIN) : 0;
    unsigned top = 63u - (unsigned)__builtin_clzll(size - 1); /* 2^top < size <= 2^(top+1) */
    return SMALL / GRAIN + (top - SMALL_SHIFT) * STEPS + (unsigned)((size - 1 - (1ull << top)) >> (top - STEP_BITS));
}

static uint32_t mark_word(unsigned class, unsigned order, unsigned home) {
    return class | order << 8 | home << 16;
}

static void chain_push(struct chain *c, void *p) {
    *(void **)p = c->first;
    c->first = p;
    c->count++;
}

static void *chain_pop(struct chain *c) {
    void **p = c->first;
    c->first = *p;
    c->count--;
    return p;
}

static void account(int64_t bytes) {
    uint64_t f = irq_save();
    this_cpu(heap_cpu).used += bytes;
    irq_restore(f);
}

/* ---- slabs (under their depot's lock) ---- */

static bool slab_has(const struct slab *s) { return s->free || s->fresh < classes[s->class].objects[s->order]; }

static void *slab_take(struct slab *s) {
    void **p = s->free;
    if (p) s->free = *p;
    else p = (void **)(s->base + (uintptr_t)s->fresh++ * classes[s->class].size);
    s->used++;
    return p;
}

static void list_add(struct slab **list, struct slab *s) {
    s->prev = NULL;
    s->next = *list;
    if (*list) (*list)->prev = s;
    *list = s;
}

static void list_remove(struct slab **list, struct slab *s) {
    if (s->next) s->next->prev = s->prev;
    if (s->prev) s->prev->next = s->next;
    else *list = s->next;
}

/* Objects of d's slabs into c until it holds want. */
static void carve(struct depot *d, struct chain *c, unsigned want) {
    while (c->count < want) {
        struct slab *s = d->partial;
        if (!s) {
            if (!(s = d->spare)) return;
            d->spare = NULL;
            list_add(&d->partial, s);
        }
        while (c->count < want && slab_has(s)) chain_push(c, slab_take(s));
        if (!slab_has(s)) list_remove(&d->partial, s);
    }
}

/* Empty slabs back to pmm, once no lock is held: their frames, and their records. */
static void release(struct slab *gone) {
    while (gone) {
        struct slab *s = gone;
        gone = s->next;
        uint64_t phys = hhdm_virt_to_phys((void *)s->base);
        unsigned order = s->order;
        bool inside = classes[s->class].inside[order];
        if (order) pmm_free_contig(phys, 1ull << order);
        else pmm_free(phys);
        if (!inside) kfree(s);
    }
}

/* A list of home's objects of class k (through their first words) back into their slabs. */
static void to_slabs(unsigned home, unsigned k, void **first) {
    /* each one's slab, found before the lock is taken: the frames' records are the cold reads */
    for (void **p = first; p; p = *p) {
        uint32_t word;
        void *slab;
        pmm_marked(hhdm_virt_to_phys(p), &word, &slab);
        p[1] = slab;
    }
    struct depot *d = &depots[home][k];
    struct slab *gone = NULL;
    uint64_t f = spin_lock(&d->lock);
    for (void **p = first, **next; p; p = next) {
        next = *p;
        struct slab *s = p[1];
        bool listed = slab_has(s);
        *p = s->free;
        s->free = p;
        if (--s->used) {
            if (!listed) list_add(&d->partial, s);
            continue;
        }
        if (listed) list_remove(&d->partial, s);
        if (!d->spare) {
            d->spare = s;
        } else {
            s->next = gone;
            gone = s;
        }
    }
    spin_unlock(&d->lock, f);
    release(gone);
}

/* An away chain back to the homes of its objects, one home at a time. */
static void send_away(unsigned k, struct chain *away) {
    void **rest = away->first;
    *away = (struct chain){0};
    while (rest) {
        uint32_t word;
        void *slab;
        pmm_marked(hhdm_virt_to_phys(rest), &word, &slab);
        unsigned home = word >> 16;
        void **mine = NULL, **others = NULL;
        for (void **p = rest, **next; p; p = next) {
            next = *p;
            pmm_marked(hhdm_virt_to_phys(p), &word, &slab);
            if (word >> 16 == home) {
                *p = mine;
                mine = p;
            } else {
                *p = others;
                others = p;
            }
        }
        to_slabs(home, k, mine);
        rest = others;
    }
}

/* Every depot's chains and spare slab back to pmm: memory ran short. */
static void drain_depots(void) {
    for (unsigned n = 0; n < topology.nodes; n++) {
        if (!depots[n]) continue;
        for (unsigned k = 0; k < CLASSES; k++) {
            struct depot *d = &depots[n][k];
            uint64_t f = spin_lock(&d->lock);
            void **chains = d->chains;
            struct slab *spare = d->spare;
            d->chains = NULL;
            d->count = 0;
            d->spare = NULL;
            spin_unlock(&d->lock, f);
            for (void **next; chains; chains = next) {
                next = chains[1];
                to_slabs(n, k, chains);
            }
            if (spare) {
                spare->next = NULL;
                release(spare);
            }
        }
    }
}

/* A new slab of class k for home, its frames marked; NULL without memory. Any interrupt state. */
static struct slab *grow(unsigned k, unsigned home) {
    const struct class *c = &classes[k];
    unsigned order = c->order;
    uint64_t phys = order ? pmm_alloc_run(1ull << order, home) : 0;
    if (!phys) {
        order = 0;
        if (!(phys = pmm_alloc())) {
            drain_depots();
            phys = pmm_alloc();
        }
    }
    if (!phys) return NULL;
    uintptr_t base = (uintptr_t)phys_to_virt(phys);
    struct slab *s = c->inside[order] ? (struct slab *)(base + (PAGE_SIZE << order) - sizeof *s) : kmalloc(sizeof *s);
    if (!s) {
        if (order) pmm_free_contig(phys, 1ull << order);
        else pmm_free(phys);
        return NULL;
    }
    *s = (struct slab){.base = base, .home = (uint16_t)home, .class = (uint8_t)k, .order = (uint8_t)order};
    for (uint64_t i = 0; i < 1ull << order; i++) pmm_mark(phys + i * PAGE_SIZE, MARK_SLAB, mark_word(k, order, home), s);
    return s;
}

/* ---- a CPU's chains (its interrupts off) ---- */

/* An empty loaded chain from the home's depot: a full chain, else objects of its slabs (fresh, a new
   slab, joins them first). It may stay empty. */
static void refill(struct heap_cpu *h, unsigned k, struct slab *fresh) {
    struct cpu_class *c = &h->class[k];
    struct depot *d = &depots[h->home][k];
    spin_take(&d->lock);
    if (fresh) {
        list_add(&d->partial, fresh);
    } else if (d->chains) {
        c->loaded = (struct chain){d->chains, c->most};
        d->chains = d->chains[1];
        d->count--;
    }
    if (!c->loaded.count) carve(d, &c->loaded, c->most);
    spin_give(&d->lock);
}

/* A full chain to the home's depot, or back to the slabs when the depot has its most. */
static void depot_put(struct heap_cpu *h, unsigned k, struct chain *c) {
    struct depot *d = &depots[h->home][k];
    void **first = c->first;
    *c = (struct chain){0};
    spin_take(&d->lock);
    if (d->count < d->most) {
        first[1] = d->chains;
        d->chains = first;
        d->count++;
        spin_give(&d->lock);
        return;
    }
    spin_give(&d->lock);
    to_slabs(h->home, k, first);
}

static void *slab_alloc(unsigned k) {
    uint64_t f = irq_save();
    for (;;) {
        struct heap_cpu *h = this_cpu_ptr(heap_cpu);
        struct cpu_class *c = &h->class[k];
        if (!c->loaded.count && c->previous.count) {
            c->loaded = c->previous;
            c->previous = (struct chain){0};
        }
        if (!c->loaded.count) refill(h, k, NULL);
        if (c->loaded.count) {
            void *p = chain_pop(&c->loaded);
            h->used += classes[k].size;
            irq_restore(f);
            return p;
        }
        unsigned home = h->home;
        irq_restore(f);
        struct slab *s = grow(k, home); /* in the caller's interrupt state: pmm may take long */
        f = irq_save();
        if (!s) {
            irq_restore(f);
            return NULL;
        }
        refill(this_cpu_ptr(heap_cpu), k, s); /* the chains again: an interrupt may have filled them */
    }
}

static void slab_free(void *p, uint32_t word) {
    unsigned k = word & 0xFF, home = word >> 16;
    uint64_t f = irq_save();
    struct heap_cpu *h = this_cpu_ptr(heap_cpu);
    struct cpu_class *c = &h->class[k];
    h->used -= classes[k].size;
    if (home == h->home) {
        if (c->loaded.first == p) panic("kfree: %p freed twice", p);
        if (c->loaded.count == c->most) {
            if (c->previous.count) depot_put(h, k, &c->previous);
            c->previous = c->loaded;
            c->loaded = (struct chain){0};
        }
        chain_push(&c->loaded, p);
    } else {
        if (c->away.first == p) panic("kfree: %p freed twice", p);
        chain_push(&c->away, p);
        if (c->away.count == c->most) send_away(k, &c->away);
    }
    irq_restore(f);
}

/* ---- objects of pages ---- */

static void *pages_alloc(size_t size, bool *zeroed) {
    if (size > pmm_total_pages() * PAGE_SIZE) return NULL; /* nor does the rounding wrap */
    uint64_t count = ALIGN_UP(size, PAGE_SIZE) / PAGE_SIZE;
    if (count > UINT32_MAX) return NULL; /* the mark's word */
    uint64_t phys = count == 1 ? pmm_alloc() : pmm_alloc_run(count, this_cpu(heap_cpu).home);
    void *p = phys ? phys_to_virt(phys) : vmalloc(count);
    if (!p) return NULL;
    if (!phys) {
        phys = vmm_kernel_translate((uint64_t)p);
        *zeroed = true;
    }
    pmm_mark(phys, MARK_PAGES, (uint32_t)count, NULL);
    account((int64_t)(count * PAGE_SIZE));
    return p;
}

/* ---- the interface ---- */

/* What the heap gave at p: its kind, its first frame's mark word and physical address. Anything
   else, an object's inside included, stops the system. */
static unsigned object_at(const void *p, uint64_t *phys, uint32_t *word) {
    uintptr_t va = (uintptr_t)p;
    void *ref;
    unsigned kind = 0;
    if (va >= VMALLOC_BASE && va < VMALLOC_END) {
        *phys = va & (PAGE_SIZE - 1) ? 0 : vmm_kernel_translate(va);
        if (*phys && pmm_marked(*phys, word, &ref) == MARK_PAGES) kind = MARK_PAGES;
    } else if (va >= hhdm_offset && va - hhdm_offset < pmm_page_count() * PAGE_SIZE) {
        *phys = hhdm_virt_to_phys(p);
        kind = pmm_marked(*phys, word, &ref);
        if (kind == MARK_PAGES && *phys & (PAGE_SIZE - 1)) kind = 0;
        if (kind == MARK_SLAB) {
            const struct class *c = &classes[*word & 0xFF];
            unsigned order = *word >> 8 & 0xFF;
            uint64_t offset = *phys & ((PAGE_SIZE << order) - 1), index = offset * c->inverse >> 32;
            if (index * c->size != offset || index >= c->objects[order]) kind = 0;
        }
    }
    if (kind != MARK_SLAB && kind != MARK_PAGES) panic("heap: %p, which the heap did not give", p);
    return kind;
}

static void *alloc(size_t size, bool *zeroed) {
    *zeroed = false;
    return size <= classes[CLASSES - 1].size ? slab_alloc(class_of(size)) : pages_alloc(size, zeroed);
}

void *kmalloc(size_t size) {
    bool zeroed;
    return alloc(size, &zeroed);
}

void *kzalloc(size_t size) {
    bool zeroed;
    void *p = alloc(size, &zeroed);
    if (p && !zeroed) memset(p, 0, size);
    return p;
}

void kfree(void *p) {
    if (!p) return;
    uint64_t phys;
    uint32_t word;
    if (object_at(p, &phys, &word) == MARK_SLAB) {
        slab_free(p, word);
        return;
    }
    account(-(int64_t)((uint64_t)word * PAGE_SIZE));
    if ((uintptr_t)p >= VMALLOC_BASE && (uintptr_t)p < VMALLOC_END) {
        pmm_mark(phys, 0, 0, NULL); /* its frames stay in use, mapped, until vmalloc purges them */
        vfree(p, word);
    } else if (word == 1) {
        pmm_free(phys);
    } else {
        pmm_free_contig(phys, word);
    }
}

void *krealloc(void *p, size_t size) {
    if (!p) return kmalloc(size);
    uint64_t phys;
    uint32_t word;
    size_t have = object_at(p, &phys, &word) == MARK_SLAB ? classes[word & 0xFF].size : (size_t)word * PAGE_SIZE;
    if (have >= size) return p;
    void *n = kmalloc(size);
    if (!n) return NULL;
    memcpy(n, p, have);
    kfree(p);
    return n;
}

void *kappend(void *array_at, size_t *count, size_t *room, size_t size) {
    void **array = array_at;
    if (*count == *room) {
        size_t more = *room ? *room * 2 : 8;
        if (more < *room || more > SIZE_MAX / size) return NULL;
        void *grown = krealloc(*array, more * size);
        if (!grown) return NULL;
        *array = grown;
        *room = more;
    }
    return (uint8_t *)*array + (*count)++ * size;
}

uint64_t heap_used_bytes(void) {
    int64_t used = 0;
    for (unsigned i = 0; i < topology.cpus; i++) used += __atomic_load_n(&per_cpu(heap_cpu, i).used, __ATOMIC_RELAXED);
    return used > 0 ? (uint64_t)used : 0;
}

void heap_cpu_gone(unsigned cpu) {
    struct heap_cpu *h = &per_cpu(heap_cpu, cpu);
    for (unsigned k = 0; k < CLASSES; k++) {
        struct cpu_class *c = &h->class[k];
        if (c->loaded.first) to_slabs(h->home, k, c->loaded.first);
        if (c->previous.first) to_slabs(h->home, k, c->previous.first);
        if (c->away.first) send_away(k, &c->away);
        c->loaded = c->previous = (struct chain){0};
    }
}

/* ---- building it ---- */

static void lay_out(struct class *c, unsigned order, bool record) {
    uint32_t bytes = (uint32_t)(PAGE_SIZE << order), objects = bytes / c->size;
    bool inside = record || bytes - objects * c->size >= sizeof(struct slab);
    if (inside) objects = (bytes - (uint32_t)sizeof(struct slab)) / c->size;
    c->objects[order] = (uint16_t)objects;
    c->inside[order] = inside;
}

static void *node_pages(uint64_t bytes, unsigned node) {
    uint64_t phys = pmm_alloc_run(ALIGN_UP(bytes, PAGE_SIZE) / PAGE_SIZE, node);
    if (!phys) panic("heap: no memory for its depots");
    return memset(phys_to_virt(phys), 0, bytes);
}

void heap_init(void) {
    if (topology.nodes > 1u << 16) panic("heap: %u nodes do not fit a mark", topology.nodes);
    record_class = class_of(sizeof(struct slab));
    for (unsigned k = 0; k < CLASSES; k++) {
        struct class *c = &classes[k];
        if (k < SMALL / GRAIN) {
            c->size = (k + 1) * GRAIN;
        } else {
            unsigned j = k - SMALL / GRAIN, top = SMALL_SHIFT + j / STEPS;
            c->size = (1u << top) + ((j % STEPS + 1) << (top - STEP_BITS));
        }
        c->inverse = (uint32_t)(((1ull << 32) + c->size - 1) / c->size);
        c->order = ORDER_MOST;
        for (unsigned order = ORDER_MOST + 1; order--;) lay_out(c, order, k == record_class);
        for (unsigned order = 0; order <= ORDER_MOST; order++) {
            uint32_t bytes = (uint32_t)(PAGE_SIZE << order), waste = bytes - c->objects[order] * c->size;
            if (c->objects[order] >= OBJECTS_LEAST && waste <= bytes / WASTE) {
                c->order = (uint8_t)order;
                break;
            }
        }
    }

    depots = node_pages(topology.nodes * sizeof *depots, topology.cpu[0].home);
    for (unsigned n = 0; n < topology.nodes; n++) {
        unsigned cpus = topology.node[n].homed;
        if (!cpus) continue;
        struct depot *d = depots[n] = node_pages(CLASSES * sizeof *d, n);
        /* the node's memory: the SRAT's, or the RAM shared out when it gives none */
        uint64_t bytes = topology.node[n].bytes ? topology.node[n].bytes : pmm_total_pages() * PAGE_SIZE / topology.nodes;
        uint64_t chain = bytes / ((uint64_t)CACHE_SHARE * cpus * CLASSES * CHAINS_PER_CPU);
        for (unsigned k = 0; k < CLASSES; k++) d[k].most = cpus;
        kprintf("heap: node %u: %u CPUs at home, chains of %u (%u bytes) to %u (%u bytes) objects\n", n, cpus,
                (unsigned)MIN(MAX(chain / classes[0].size, 1), CHAIN_MOST), classes[0].size,
                (unsigned)MIN(MAX(chain / classes[CLASSES - 1].size, 1), CHAIN_MOST), classes[CLASSES - 1].size);
        for (unsigned i = 0; i < topology.cpus; i++) {
            if (topology.cpu[i].home != n) continue;
            struct heap_cpu *h = &per_cpu(heap_cpu, i);
            h->home = n;
            for (unsigned k = 0; k < CLASSES; k++)
                h->class[k].most = (unsigned)MIN(MAX(chain / classes[k].size, 1), CHAIN_MOST);
        }
    }
}
