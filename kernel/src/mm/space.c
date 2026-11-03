/* A process's address-space layout (its fields in sys/process.h, under its space lock). Freed
   address space of a region goes back to it: without reuse every map/unmap cycle consumes fresh
   addresses and the page tables covering them, so a process that keeps allocating and freeing large
   blocks leaks kernel memory. Every reservation keeps one unmapped guard page after itself; a hole
   includes it. The low region below 2 GiB keeps its own holes and frontier. A range is the
   reserver's from the moment it leaves the holes or the frontier. A thread that changes a range it
   holds (a reservation until its pages are there, a release, a protection, a shared mapping taken
   down) lists an operation over it first, under the space lock: another change of a range that
   overlaps it waits until it is done, so a release or protection never meets a range half made,
   half gone or given out again. The space lock is held only briefly (no sleep, no copy, no page
   table work but one page's): the page tables change with it let go. */
#include "mm/space.h"
#include "mm/shared_parts.h"
#include "mm/heap.h"
#include "mm/pmm.h"

/* A free range of a region below its frontier, in the region's tree: a fit is found in O(log n),
   the lowest first. next: on a list of records to free after the space lock. */
struct mmap_hole {
    struct span span;
    struct mmap_hole *next;
};
static struct mmap_hole *hole_of(struct span *s) {
    return s ? (struct mmap_hole *)((char *)s - __builtin_offsetof(struct mmap_hole, span)) : NULL;
}
static void hole_gone(struct span_tree *t, struct mmap_hole *h, struct mmap_hole **gone) {
    span_remove(t, &h->span);
    h->next = *gone;
    *gone = h;
}
struct mmap_hole *hole_spare(void) { return kmalloc(sizeof(struct mmap_hole)); }
void holes_free(struct mmap_hole *h) {
    while (h) {
        struct mmap_hole *next = h->next;
        kfree(h);
        h = next;
    }
}
/* Nothing comes back once the space ended. */
void hole_add_locked(struct process *owner, uint64_t va, uint64_t len, struct mmap_hole **spare, struct mmap_hole **gone) {
    if (!len || owner->space_ended) return;
    bool low = va < USER_MMAP_BASE;
    struct span_tree *holes = low ? &owner->low_holes : &owner->mmap_holes;
    uint64_t *next = low ? &owner->low_next : &owner->mmap_next;
    /* the union with the holes it overlaps or touches, which go */
    uint64_t start = va, end = va + len;
    struct mmap_hole *h = hole_of(span_below(holes, va + 1)); /* the last starting at or below va */
    if (!h || h->span.start + h->span.length < va) h = hole_of(span_from(holes, va));
    struct mmap_hole *kept = NULL;
    while (h && h->span.start <= end) {
        struct mmap_hole *after = hole_of(span_from(holes, h->span.start + h->span.length));
        start = MIN(start, h->span.start);
        end = MAX(end, h->span.start + h->span.length);
        span_remove(holes, &h->span);
        if (kept) {
            h->next = *gone;
            *gone = h;
        } else {
            kept = h;
        }
        h = after;
    }
    if (!kept) {
        if (!*spare) return;
        kept = *spare;
        *spare = NULL;
    }
    /* the last hole simply lowers the reservation frontier */
    if (end >= *next && !span_from(holes, end)) {
        *next = start;
        kept->next = *gone;
        *gone = kept;
        return;
    }
    kept->span.start = start;
    kept->span.length = end - start;
    span_insert(holes, &kept->span);
}
void mmap_hole_add(struct process *owner, uint64_t va, uint64_t len) {
    if (!owner || !len) return;
    struct mmap_hole *spare = hole_spare(), *gone = NULL;
    uint64_t lock = spin_lock(&owner->space.lock);
    hole_add_locked(owner, va, len, &spare, &gone);
    spin_unlock(&owner->space.lock, lock);
    kfree(spare);
    holes_free(gone);
}
/* Space lock held: len bytes and a guard page from a region's holes (first fit), else at its
   frontier below top; 0 when neither has room. A hole used up goes to *gone. */
static uint64_t region_take_locked(struct span_tree *holes, uint64_t *next, uint64_t top, uint64_t len, struct mmap_hole **gone) {
    if (!len || len > top) return 0;
    uint64_t need = len + PAGE_SIZE;
    struct mmap_hole *h = hole_of(span_fit(holes, need));
    if (h) {
        uint64_t va = h->span.start;
        span_remove(holes, &h->span);
        if (h->span.length == need) {
            h->next = *gone;
            *gone = h;
        } else {
            h->span.start += need;
            h->span.length -= need;
            span_insert(holes, &h->span);
        }
        return va;
    }
    uint64_t va = *next;
    if (va >= top || need > top - va) return 0;
    *next = va + need;
    return va;
}
uint64_t space_take_locked(struct process *owner, bool low, uint64_t len, struct mmap_hole **gone) {
    if (owner->space_ended) return 0;
    return low ? region_take_locked(&owner->low_holes, &owner->low_next, USER_LOW_TOP, len, gone)
               : region_take_locked(&owner->mmap_holes, &owner->mmap_next, USER_MMAP_TOP, len, gone);
}
uint64_t mmap_reserve(struct process *owner, uint64_t len) {
    struct mmap_hole *gone = NULL;
    uint64_t lock = spin_lock(&owner->space.lock);
    uint64_t va = space_take_locked(owner, false, len, &gone);
    spin_unlock(&owner->space.lock, lock);
    holes_free(gone);
    return va;
}
uint64_t mmap_reserve_changing(struct process *owner, uint64_t len, struct space_op *op) {
    struct mmap_hole *gone = NULL;
    uint64_t lock = spin_lock(&owner->space.lock);
    uint64_t va = space_take_locked(owner, false, len, &gone);
    if (va) space_op_add(owner, op, va, va + len + PAGE_SIZE);
    spin_unlock(&owner->space.lock, lock);
    holes_free(gone);
    return va;
}
void space_end(struct process *owner) {
    struct mmap_hole *gone = NULL;
    uint64_t lock = spin_lock(&owner->space.lock);
    for (struct mmap_hole *h; (h = hole_of(span_from(&owner->mmap_holes, 0)));) hole_gone(&owner->mmap_holes, h, &gone);
    for (struct mmap_hole *h; (h = hole_of(span_from(&owner->low_holes, 0)));) hole_gone(&owner->low_holes, h, &gone);
    owner->space_ended = true;
    spin_unlock(&owner->space.lock, lock);
    holes_free(gone);
}
/* Space lock held: whether an operation is at part of [va, end). */
static bool space_busy(const struct process *owner, uint64_t va, uint64_t end) {
    for (const struct space_op *o = owner->space_ops; o; o = o->next)
        if (va < o->end && o->va < end) return true;
    return false;
}
/* Space lock held, taken with spin_lock: returns once no operation is at part of [va, end), the
   lock held again; it is let go while the thread waits. */
static void space_wait(struct process *owner, uint64_t va, uint64_t end) {
    while (space_busy(owner, va, end)) {
        wq_wait_locked(&owner->space, &owner->space.lock);
        spin_take(&owner->space.lock);
    }
}
void space_op_add(struct process *owner, struct space_op *op, uint64_t va, uint64_t end) {
    *op = (struct space_op){.next = owner->space_ops, .va = va, .end = end};
    owner->space_ops = op;
}
void space_op_remove(struct process *owner, struct space_op *op) {
    struct space_op **link = &owner->space_ops;
    while (*link != op) link = &(*link)->next;
    *link = op->next;
}
void space_op_finish(struct process *owner, struct space_op *op, uint64_t va, uint64_t len) {
    struct mmap_hole *spare = len ? hole_spare() : NULL, *gone = NULL;
    uint64_t lock = spin_lock(&owner->space.lock);
    hole_add_locked(owner, va, len, &spare, &gone);
    space_op_remove(owner, op);
    spin_unlock(&owner->space.lock, lock);
    wq_wake_all(&owner->space);
    kfree(spare);
    holes_free(gone);
}
/* Space lock held: whether part of [va, end) was never given out or is free again: at or past a
   region's frontier, or in one of its holes. Outside the two regions nothing is handed out. */
static bool space_free_overlap(const struct process *owner, uint64_t va, uint64_t end) {
    for (int low = 0; low < 2; low++) {
        if (low && !owner->low_base) continue;
        uint64_t top = low ? USER_LOW_TOP : USER_MMAP_TOP;
        uint64_t next = low ? owner->low_next : owner->mmap_next;
        if (MAX(va, next) < MIN(end, top)) return true;
        const struct span_tree *holes = low ? &owner->low_holes : &owner->mmap_holes;
        const struct span *h = span_at(holes, va);
        if (!h) h = span_from(holes, va);
        if (h && h->start < end) return true;
    }
    return false;
}
/* Space lock held: the first mapping that overlaps [va, end), or NULL. */
static struct shared_mapping *space_mapping(const struct process *owner, uint64_t va, uint64_t end) {
    struct shared_mapping *m = shared_mapping_of(span_at(&owner->shared_mappings, va));
    if (!m) m = shared_mapping_of(span_from(&owner->shared_mappings, va));
    return m && m->va < end ? m : NULL;
}

/* Native access to page protection: readable, writable or executable, never writable and executable
   together. */
static int access_flags(int access) {
    switch (access) {
    case N_MEMORY_READ: return 0;
    case N_MEMORY_READ_WRITE: return VM_W;
    case N_MEMORY_READ_EXECUTE: return VM_X;
    default: return -1;
    }
}

/* Zeroed pages in the reservation region above the program (thread stacks and shared mappings live
   there too), or below 2 GiB with N_MEMORY_LOW. Beyond a few pages they receive frames on first
   touch, so a process pays only for what it uses; a reservation larger than all free memory still
   fails here. An operation over the range and its guard page keeps a sibling's release or
   protection of them out until the pages are there. */
#define RESERVE_EAGER (64 * 1024)
int64_t memory_reserve(size_t len, int access) {
    struct process *t = process_current();
    int vp = access_flags(access & ~N_MEMORY_LOW);
    /* No size cap of its own: free memory and the address region bound it. */
    if (len == 0 || len > SIZE_MAX - (PAGE_SIZE - 1) || vp < 0) return -N_ERR_INVALID;
    len = ALIGN_UP(len, PAGE_SIZE);
    if (!frames_free(len / PAGE_SIZE)) {
        process_memory_report(t, len);
        return -N_ERR_NO_MEMORY;
    }
    struct space_op op;
    struct mmap_hole *gone = NULL;
    uint64_t lock = spin_lock(&t->space.lock);
    uint64_t va = space_take_locked(t, access & N_MEMORY_LOW, len, &gone);
    if (va) space_op_add(t, &op, va, va + len + PAGE_SIZE);
    spin_unlock(&t->space.lock, lock);
    holes_free(gone);
    if (!va) return -N_ERR_NO_MEMORY;
    int r = len <= RESERVE_EAGER ? vmm_user_alloc(t->vmm_space, va, len, vp) : vmm_user_reserve(t->vmm_space, va, len, vp);
    if (r < 0) {
        vmm_user_free(t->vmm_space, va, len);
        space_op_finish(t, &op, va, len + PAGE_SIZE);
        process_memory_report(t, len);
        return -N_ERR_NO_MEMORY;
    }
    space_op_finish(t, &op, 0, 0);
    return (int64_t)va;
}

/* Releases reserved pages or a whole shared mapping. Only what the space gave out and still holds:
   a range with a free part fails (a second release of it too). A release of a range another thread
   is changing waits for it. */
int64_t memory_release(uint64_t va, size_t len) {
    struct process *t = process_current();
    bool low = va < USER_MMAP_BASE;
    uint64_t base = low ? t->low_base : USER_MMAP_BASE, top = low ? USER_LOW_TOP : USER_MMAP_TOP;
    if (len > SIZE_MAX - (PAGE_SIZE - 1)) return -N_ERR_INVALID;
    uint64_t end = va + ALIGN_UP(len, PAGE_SIZE);
    if (va & (PAGE_SIZE - 1) || va < base || end > top || end < va) return -N_ERR_INVALID;
    if (end == va) return 0;
    struct space_op op;
    int64_t r = 0;
    uint64_t lock = spin_lock(&t->space.lock);
    space_wait(t, va, end);
    /* an overlapping mapping is not leaving now: its remover's operation was waited for */
    bool unreserved = space_free_overlap(t, va, end);
    struct shared_mapping *m = unreserved ? NULL : space_mapping(t, va, end);
    if (unreserved) r = -N_ERR_INVALID;
    else if (m && m->building) r = -N_ERR_NOT_READY;
    else if (m && (va != m->va || end - va != m->bytes)) r = -N_ERR_INVALID;
    else {
        if (m) {
            struct shared_pages *p = mapping_pages(m);
            spin_take(&p->lock);
            m->leaving = true;
            spin_give(&p->lock);
        }
        space_op_add(t, &op, va, end);
    }
    spin_unlock(&t->space.lock, lock);
    if (r < 0) return r;
    /* shared_map reserved the range and its guard page: they are reused */
    if (m) {
        shared_unmap(t, m, &op, !low);
        return 0;
    }
    /* a shared page outside any mapping stays where the system put it */
    for (uint64_t a = va; a < end; a += PAGE_SIZE)
        if (vmm_get_pte(t->vmm_space, a) & PTE_SHARED) {
            space_op_finish(t, &op, 0, 0);
            return -N_ERR_INVALID;
        }
    vmm_user_free(t->vmm_space, va, end - va);
    struct mmap_hole *spare = hole_spare(), *gone = NULL;
    lock = spin_lock(&t->space.lock);
    /* The unmapped page right after the range is the reservation's guard, unless it was handed
       out: an untouched reserved page, a shared mapping's page (also an untouched demand one), or
       a range another thread is reserving or changing. */
    bool guard = end < (low ? t->low_next : t->mmap_next) && !space_busy(t, end, end + PAGE_SIZE) &&
                 !(vmm_get_leaf(t->vmm_space, end) & (PTE_P | PTE_DEMAND)) && !space_mapping(t, end, end + PAGE_SIZE);
    hole_add_locked(t, va, end - va + (guard ? PAGE_SIZE : 0), &spare, &gone);
    space_op_remove(t, &op);
    spin_unlock(&t->space.lock, lock);
    wq_wake_all(&t->space);
    kfree(spare);
    holes_free(gone);
    return 0;
}

/* Private pages take the access as asked; the pages of a shared mapping keep within the mapping's,
   also those a fault maps later (shared_protect). A range with a part that was never given out or
   is free again fails as an unmapped one does; a protection of a range another thread is changing
   waits for it. */
int64_t memory_protect(uint64_t va, size_t len, int access) {
    int vp = access_flags(access);
    if (vp < 0 || va & (PAGE_SIZE - 1) || va >= USER_TOP || va + len > USER_TOP || va + len < va) return -N_ERR_INVALID;
    struct process *t = process_current();
    if (!len) return 0;
    uint64_t end = ALIGN_UP(va + len, PAGE_SIZE);
    struct space_op op;
    int64_t r = 0;
    uint64_t lock = spin_lock(&t->space.lock);
    space_wait(t, va, end);
    struct shared_mapping *m = space_mapping(t, va, end);
    if (m && m->building) r = -N_ERR_NOT_READY;
    else if (!m && space_free_overlap(t, va, end)) r = -N_ERR_NO_MEMORY;
    else space_op_add(t, &op, va, end);
    spin_unlock(&t->space.lock, lock);
    if (r < 0) return r;
    r = m ? shared_protect(t, m, va, end - va, access, vp) : vmm_user_protect(t->vmm_space, va, len, vp, false);
    space_op_finish(t, &op, 0, 0);
    return r;
}
