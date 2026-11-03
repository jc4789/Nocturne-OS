/* What the parts of shared memory share with one another: mm/shared.c, mm/pager.c, mm/program.c
   and mm/space.c. The rest of the kernel knows mm/shared.h. The locks are at the top of
   mm/shared.c. */
#pragma once
#include "kernel.h"
#include "abi.h"
#include "arch/spin.h"
#include "lib/span.h"
#include "mm/shared.h"
#include "mm/space.h"
#include "mm/vmm.h"
#include "sys/object.h"
#include "sys/process.h"
#include "sys/wait.h"

/* A record's place in a list its head alone names. Leaving needs only the record, so one may move
   between lists. Under the lock of the list's owner. */
struct shared_link {
    struct shared_link *next, **pprev;
    void *owner; /* whose place it is */
};
static inline void link_add(struct shared_link **head, struct shared_link *l, void *owner) {
    l->owner = owner;
    l->next = *head;
    l->pprev = head;
    if (l->next) l->next->pprev = &l->next;
    *head = l;
}
static inline void link_del(struct shared_link *l) {
    if (!l->pprev) return; /* it never joined */
    *l->pprev = l->next;
    if (l->next) l->next->pprev = l->pprev;
    l->next = NULL;
    l->pprev = NULL;
}

#define SHARED_LEAF (PAGE_SIZE / sizeof(uint64_t))
enum { SHARED_RAM, SHARED_DEMAND, SHARED_RANGE, SHARED_VIEW, SHARED_COMPOSITE, SHARED_PAGED };

struct pager;
struct shared_scope;
struct shared_binding;

/* Composite segments point only at backing roots, never at another composite. end is the exclusive
   logical page. */
struct shared_part {
    struct object *object;
    size_t first, end;
    uint8_t cache;
    struct shared_link derived; /* in its object's derived */
};
/* A composite's hold on the scope of one object it was composed of: its revocation reaches it. */
struct shared_dependency {
    struct shared_link link; /* in the scope's dependents */
    struct shared_scope *scope;
};
struct shared_pages {
    size_t count, bytes;
    uint8_t kind, cache;      /* a DMA root's cache changes (typing), and is read atomically */
    bool dma, owned, wc_allowed;
    bool cancelled;           /* its scope was revoked: atomic, set before its mappings are looked at */
    bool typing;              /* a DMA root: its direct map's type changes now (lock) */
    size_t maps;              /* a DMA root: mapping references, also of composite segments (lock) */
    struct wait_queue typed;  /* a DMA root: those that wait for typing to end */
    struct spinlock lock;
    struct shared_link *mappings; /* struct shared_mapping, also being built or taken down (lock) */
    /* The views of it and the composites of it, each a place of theirs (lock); ended: those a
       claim's end has been through. */
    struct shared_link *derived, *derived_ended;
    struct shared_link derived_link; /* a view's, in its parent's derived */
    struct shared_link claim_link;   /* a device range's, in its claim's ranges */
    struct shared_scope *scope;
    struct pci_claim *claim;  /* a BAR or DMA root: the claim it was made through */
    uint64_t base;            /* a range's first frame */
    struct object *parent;    /* a view's object, a root or a composite; never another view */
    size_t first;             /* a view's first page of it */
    struct shared_part *parts;
    size_t part_count;
    struct shared_dependency *dependencies;
    size_t dependency_count;
    size_t resident;          /* a demand object's frames: atomic */
    /* A program segment's demand object: its pages hold source's bytes from source_offset on,
       source_bytes of them starting source_skip bytes into the first page, zeros elsewhere, read
       on first touch. NULL while its program is idle (lock; program_map gives a start's again). */
    struct handle *source;
    uint64_t source_offset, source_bytes;
    size_t source_skip;
    struct pager *pager;      /* paged memory: the pager that supplies it (holds a reference) */
    struct object object;
    /* RAM: a frame per page, fixed. Demand: frames of 512 slots, each a page's frame; paged: two
       such frames per 512 pages, the entries and the sources. Both are published with one compare
       and exchange, never change after that, and go with the object. */
    uint64_t pages[];
};

/* One mapping of a shared object into its owner's space, from the moment its range is reserved
   (building, while shared_map installs its pages) until its pages went (leaving, while it is taken
   down). It is in its owner's tree (shared_mappings, by its range) and on its object's mappings.
   busy counts those working on its pages without the object's lock (shared_withdraw); it leaves
   the object's list only once busy is 0 (idle wakes its remover), so they find it, its range and
   its owner's space as they were. cancelled: a revocation reached it while it was built: it fails.
   access: NULL, or the leaves (frames, 0 for none yet) of one byte per page holding the access
   n_memory_protect set there (0: the mapping's own); every PTE installed later keeps to it.
   Under its object's lock; building and leaving also change with the owner's space lock held, so
   either is enough to read them; the other fields are set before it is listed. */
struct shared_mapping {
    struct span span;              /* in its owner's tree: [va, va + bytes) */
    struct shared_link in_object;
    struct object *object;
    uint64_t va, flags;
    size_t bytes;
    struct process *owner;
    bool building, leaving, cancelled;
    unsigned busy;
    struct wait_queue idle;
    uint64_t *access;
};
static inline struct shared_mapping *shared_mapping_of(struct span *s) {
    return s ? (struct shared_mapping *)((char *)s - __builtin_offsetof(struct shared_mapping, span)) : NULL;
}
static inline struct shared_pages *mapping_pages(const struct shared_mapping *m) { return m->object->priv; }

/* held taken, with interrupts on or off: sleeps on wq with held given back, and takes it again. */
static inline void shared_sleep(struct wait_queue *wq, struct spinlock *held) {
    uint64_t f = irq_save();
    wq_wait_locked(wq, held);
    irq_restore(f);
    spin_take(held);
}

/* ---- mm/shared.c ---- */

/* A new object of bytes (page aligned, or rounded up) with no reference yet: the first handle or
   mapping takes one, and shared_release before that frees it. */
int64_t shared_object(size_t bytes, bool demand, struct shared_pages **out);
void shared_release(struct object *o);
struct shared_pages *shared_new(size_t bytes, uint8_t kind, size_t slots);
/* A handle on a new object; without memory for it the object is freed. */
int64_t shared_open(struct shared_pages *p, uint32_t rights, struct handle **out);
/* The shared pages a handle reaches (NULL: it reaches something else). */
struct shared_pages *shared_of(struct handle *h);
/* The root a view stands on: not a view itself. */
struct shared_pages *shared_root(struct shared_pages *p);
/* Whether its device's claim ended, for a device range or a composite with one. */
bool shared_device_gone(struct shared_pages *p);
/* One logical page resolved to the object that holds it, *index its page there. Optional backing
   and span are used when composing or counting DMA mapping references. */
struct shared_pages *shared_leaf(struct shared_pages *p, size_t *index, uint8_t *cache, struct object **backing, size_t *span);
/* A slot of the two-level tables of pages[]: the frame of 512 slots at pages[leaf], made now with
   create (with no lock held), or NULL. */
uint64_t *shared_table(struct shared_pages *p, size_t leaf, bool create);
/* Page index of demand object p has a frame: frame, which p takes, unless a peer gave it one first
   and frame goes back; 0, or -N_ERR_NO_MEMORY (frame gone back, no table for it). No lock held. */
int demand_adopt(struct shared_pages *p, size_t index, uint64_t frame);
/* The frame of page index of demand object p, 0 when it has none yet. */
uint64_t demand_frame(struct shared_pages *p, size_t index);
/* Page index of demand object p has a zeroed frame: 0, or -N_ERR_NO_MEMORY. No lock held. */
int demand_make(struct shared_pages *p, size_t index);
/* Every mapping of p, also one being built or taken down: its pages [offset, offset+bytes) from
   its start (bytes 0: all of them) are unmapped, or with write_only lose write access, shot down
   before this returns. cancel: one being built fails (cancelled): the builder looks under the
   object's lock before each page it installs, so none comes back after this. A mapping listed
   after this began read the state its caller changed before. */
void shared_withdraw(struct shared_pages *p, uint64_t offset, uint64_t bytes, bool write_only, bool cancel);
/* The source of pages of g's memory is src: src's revocation will unbind them. 0, or
   -N_ERR_CLOSED when it was revoked already (nothing is bound from it after that), or
   -N_ERR_NO_MEMORY. */
int shared_binding_add(struct shared_pages *src, struct pager *g);
/* g's memory is gone: its bindings go. */
void shared_bindings_end(struct shared_binding *b);
/* Takes mapping m down. Its caller set leaving and listed op over its range, both with its owner's
   space lock held, and holds no lock now. Its pages go (and the all-ones pages a revoked device
   range may have received); once no revocation works on it, it leaves its object's list, then its
   owner's tree, its pins and DMA types go back, and its object is dropped. hole: its range and
   guard page go back to the mmap region. */
struct space_op;
void shared_unmap(struct process *owner, struct shared_mapping *m, struct space_op *op, bool hole);
/* n_memory_protect of [va, va+bytes) (page aligned, not empty) with access (vp: its VM_ bits),
   which overlaps mapping m of process owner: the range lies in m and asks no more than m grants.
   No lock held: the caller's space operation over the range keeps m listed and its range
   unchanged meanwhile. */
int64_t shared_protect(struct process *owner, struct shared_mapping *m, uint64_t va, uint64_t bytes, int access, int vp);

/* ---- mm/pager.c ---- */

struct pager_fault;
enum { PAGER_PENDING, PAGER_SUPPLIED, PAGER_FAILED };
#define PAGED_BOUND 1ull
#define PAGED_WRITE 2ull
/* Under lock, but creator (set when it is made). pager_ready reads queued and memory without it,
   atomically. */
struct pager {
    struct object object;
    struct spinlock lock;        /* with these fields, its memory's entries and sources */
    struct shared_pages *memory; /* NULL once it is released */
    int creator;
    bool ended;                  /* the pager handle closed */
    struct pager_fault *faults;  /* in the order they came */
    size_t queued;
    uint64_t next_id;
    struct wait_queue waiters, taker;
    struct shared_binding *bindings; /* the scopes its memory's pages were supplied from within */
};
/* Paged memory: the entry of page index (frame | PAGED_BOUND | PAGED_WRITE, 0 when unbound) and, in
   *source, the slot of the object its frame was supplied from; NULL when the leaves are not there.
   The lock of its pager held to read or write them. */
uint64_t *paged_slot(struct shared_pages *p, size_t index, struct object ***source);
/* The memory's last reference went (shared_release): no mapping is left to unmap. */
void paged_release(struct shared_pages *p);
/* The calling thread waits for the pager to answer page `page` of its memory with the access it
   needs. The pager's lock held, which it gives back: 1 once the pager supplied something there, 0
   when it let the lock go only to allocate the fault's record (*spare, which the next call takes),
   both for the caller to look again; -1 when the fault failed, the pager ended, the thread is
   being stopped or no memory was left for the record. */
int pager_wait(struct pager *g, size_t page, bool write, struct pager_fault **spare);
/* A scope pages of g's memory were supplied from within is revoked, its objects cancelled: those
   pages are unbound, unmapped everywhere before their frames go. */
void pager_source_revoked(struct pager *g);

/* ---- mm/program.c ---- */

/* Reads page index of p, a program segment, and those around it from source and adopts those no
   peer adopted meanwhile. No lock held, holding p and source: the read waits for the disk. */
int program_fill(struct shared_pages *p, struct handle *source, size_t index);
void programs_trim(uint64_t need);

/* ---- mm/space.c ---- */

struct mmap_hole;
/* Space lock held: len bytes and a guard page from the low region or the mmap region; 0 when it
   has no room or the space ended. A hole used up goes to *gone. */
uint64_t space_take_locked(struct process *owner, bool low, uint64_t len, struct mmap_hole **gone);
/* Space lock held: [va, va+len) is free again in its region. A new record comes from *spare
   (without one the range stays unused, which breaks nothing); the records merged away and a last
   hole that lowers the frontier go to *gone, freed after the lock. */
void hole_add_locked(struct process *owner, uint64_t va, uint64_t len, struct mmap_hole **spare, struct mmap_hole **gone);
struct mmap_hole *hole_spare(void);
void holes_free(struct mmap_hole *h);
/* Space lock held. */
void space_op_add(struct process *owner, struct space_op *op, uint64_t va, uint64_t end);
void space_op_remove(struct process *owner, struct space_op *op);
/* The app's end: its holes go, and its space takes no reservation or hole again. */
void space_end(struct process *owner);
