/* Kernel objects, handles, waits on objects and the apps' handle tables (sys/object.h). */
#include "kernel.h"
#include "arch/cpu.h"
#include "arch/smp.h"
#include "mm/heap.h"
#include "sys/object.h"
#include "sys/events.h"

void object_init(struct object *o, const struct object_ops *ops, void *priv) {
    *o = (struct object){.ops = ops, .priv = priv};
}

void object_ref(struct object *o) { __atomic_add_fetch(&o->refs, 1, __ATOMIC_RELAXED); }

bool object_ref_live(struct object *o) {
    int refs = __atomic_load_n(&o->refs, __ATOMIC_RELAXED);
    do {
        if (refs <= 0) return false;
    } while (!__atomic_compare_exchange_n(&o->refs, &refs, refs + 1, true, __ATOMIC_ACQUIRE, __ATOMIC_RELAXED));
    return true;
}

void object_unref(struct object *o) {
    if (!o) return;
    int refs = __atomic_sub_fetch(&o->refs, 1, __ATOMIC_ACQ_REL);
    ASSERT(refs >= 0);
    if (!refs && o->ops->release) o->ops->release(o);
}

struct handle *handle_open(struct object *o, bool writable) {
    struct handle *h = kzalloc(sizeof *h);
    if (!h) return NULL;
    h->object = o;
    h->refs = 1;
    h->writable = writable;
    object_ref(o);
    return h;
}

struct handle *handle_ref(struct handle *h) {
    __atomic_add_fetch(&h->refs, 1, __ATOMIC_RELAXED);
    return h;
}

void handle_close(struct handle *h) {
    if (!h) return;
    if (cpu_is_runner()) {
        /* An AP drops any reference but one may be the last: ending a handle is the BSP's. */
        int refs = __atomic_load_n(&h->refs, __ATOMIC_RELAXED);
        while (refs > 1)
            if (__atomic_compare_exchange_n(&h->refs, &refs, refs - 1, true, __ATOMIC_RELEASE, __ATOMIC_RELAXED))
                return;
        sched_enter_bsp();
    }
    uint64_t flags = irq_save();
    int refs = __atomic_sub_fetch(&h->refs, 1, __ATOMIC_ACQ_REL);
    ASSERT(refs >= 0);
    bool last = refs == 0;
    struct event_watch *ended = last ? events_handle_closed(h) : NULL;
    irq_restore(flags);
    if (!last) return;
    /* No table holds h any more; an AP syscall that found it there before may still read it
       and its object (handle_ap_peek) until its section ends, and an AP wait the watches that
       ended with it. */
    ap_read_synchronize();
    events_watches_free(ended);
    /* A last close may sleep (an audio stream plays out what it has): the reference this
       caller held, not a guard, keeps the detached handle alive meanwhile. */
    struct object *o = h->object;
    if (o->ops->close) o->ops->close(h);
    object_unref(o);
    kfree(h);
}

uint32_t handle_ready(struct handle *h) {
    return h->object->ops->ready ? h->object->ops->ready(h) : 0;
}

void object_notify(struct object *o) {
    if (!o) return;
    uint64_t flags = spin_lock(&o->lock);
    for (struct object_waiter *w = o->waiters; w; w = w->next) {
        /* the new generation before the wait queue's lock: a waiter reads it under that lock */
        __atomic_add_fetch(&w->wait->generation, 1, __ATOMIC_SEQ_CST);
        wq_wake_all(&w->wait->wq);
    }
    spin_unlock(&o->lock, flags);
}

void object_waiter_link(struct object_waiter *w, struct object_wait *wait, struct object *o) {
    uint64_t flags = spin_lock(&o->lock);
    *w = (struct object_waiter){.next = o->waiters, .prev = &o->waiters, .wait = wait, .object = o};
    if (w->next) w->next->prev = &w->next;
    o->waiters = w;
    spin_unlock(&o->lock, flags);
}

/* A notification on another CPU that found w is over once this has the lock: the wait w names may
   go after this returns. */
void object_waiter_unlink(struct object_waiter *w) {
    struct object *o = w->object;
    uint64_t flags = spin_lock(&o->lock);
    *w->prev = w->next;
    if (w->next) w->next->prev = w->prev;
    spin_unlock(&o->lock, flags);
}

/* ---- handle tables ---- */

int handle_table_reserve(struct task *owner, size_t minimum) {
    const size_t limit = (size_t)INT32_MAX + 1; /* handle numbers are non-negative int32_t */
    if (minimum > limit || minimum > SIZE_MAX / sizeof *owner->handles) return -N_ERR_NO_MEMORY;
    if (minimum <= owner->handle_capacity) return 0;
    size_t capacity = owner->handle_capacity;
    while (capacity < minimum) {
        if (capacity > limit / 2) { capacity = minimum; break; }
        capacity *= 2;
    }
    struct handle **table = kzalloc(capacity * sizeof *table);
    if (!table) return -N_ERR_NO_MEMORY;
    memcpy(table, owner->handles, owner->handle_capacity * sizeof *table);
    struct handle **old = owner->handles;
    /* handle_ap_peek reads the capacity first: a larger one comes with the larger table */
    __atomic_store_n(&owner->handles, table, __ATOMIC_RELEASE);
    __atomic_store_n(&owner->handle_capacity, capacity, __ATOMIC_RELEASE);
    if (old != owner->initial_handles) {
        ap_read_synchronize();
        kfree(old);
    }
    return 0;
}

int handle_table_find(struct task *owner) {
    for (size_t i = owner->handle_hint; i < owner->handle_capacity; i++)
        if (!owner->handles[i]) return (int)i;
    size_t index = owner->handle_capacity;
    int e = handle_table_reserve(owner, index + 1);
    return e ? e : (int)index;
}

int handle_table_add(struct task *owner, struct handle *h) {
    int index = handle_table_find(owner);
    if (index < 0) return index;
    __atomic_store_n(&owner->handles[index], h, __ATOMIC_RELEASE); /* handle_ap_peek */
    owner->handle_hint = (size_t)index + 1;
    return index;
}

struct handle *handle_peek(int index) {
    struct task *owner = task_process();
    if (index < 0 || (size_t)index >= owner->handle_capacity) return NULL;
    return owner->handles[index];
}

struct handle *handle_ap_peek(int index) {
    struct task *owner = task_process();
    size_t capacity = __atomic_load_n(&owner->handle_capacity, __ATOMIC_ACQUIRE);
    if (index < 0 || (size_t)index >= capacity) return NULL;
    struct handle **table = __atomic_load_n(&owner->handles, __ATOMIC_ACQUIRE);
    return __atomic_load_n(&table[index], __ATOMIC_ACQUIRE);
}

struct handle *handle_get(int index) {
    /* The BSP changes the table; an AP reads it in a read section, which keeps h allocated while
       its reference is taken, unless its last one went meanwhile. */
    uint64_t flags = irq_save();
    ap_read_begin();
    struct handle *h = handle_ap_peek(index);
    int refs = h ? __atomic_load_n(&h->refs, __ATOMIC_RELAXED) : 0;
    do {
        if (refs <= 0) { h = NULL; break; }
    } while (!__atomic_compare_exchange_n(&h->refs, &refs, refs + 1, true, __ATOMIC_ACQUIRE, __ATOMIC_RELAXED));
    ap_read_end();
    irq_restore(flags);
    return h;
}

int handle_install_locked(struct handle *h) { return handle_table_add(task_process(), h); }

int handle_install(struct handle *h) {
    uint64_t flags = irq_save();
    int index = handle_install_locked(h);
    irq_restore(flags);
    if (index < 0) handle_close(h);
    return index;
}

struct handle *handle_take_locked(int index) {
    struct handle *h = handle_peek(index);
    if (!h) return NULL;
    struct task *owner = task_process();
    __atomic_store_n(&owner->handles[index], NULL, __ATOMIC_RELAXED); /* handle_ap_peek */
    if ((size_t)index < owner->handle_hint) owner->handle_hint = (size_t)index;
    object_notify(h->object);
    return h;
}

int handle_remove(int index) {
    uint64_t flags = irq_save();
    struct handle *h = handle_take_locked(index);
    irq_restore(flags);
    if (!h) return -N_ERR_NO_HANDLE;
    /* The last close may sleep: the reference taken out of the table owns h. */
    handle_close(h);
    return 0;
}
