/* Kernel objects, handles, waits on objects and the apps' handle tables (sys/object.h). */
#include "kernel.h"
#include "arch/cpu.h"
#include "arch/smp.h"
#include "mm/heap.h"
#include "sys/object.h"
#include "sys/events.h"

/* One waiter registered with one object: listed both in the wait and in the object. */
struct object_waiter {
    struct object_waiter *wait_next, *object_next;
    struct object_waiter **object_prev;
    struct object_wait *wait;
    struct object *object;
};

void object_init(struct object *o, const struct object_ops *ops, void *priv) {
    *o = (struct object){.ops = ops, .priv = priv};
}

void object_ref(struct object *o) {
    uint64_t flags = irq_save();
    o->refs++;
    irq_restore(flags);
}

void object_unref(struct object *o) {
    if (!o) return;
    uint64_t flags = irq_save();
    ASSERT(o->refs > 0);
    bool last = --o->refs == 0;
    irq_restore(flags);
    if (last && o->ops->release) o->ops->release(o);
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
    uint64_t flags = irq_save();
    h->refs++;
    irq_restore(flags);
    return h;
}

void handle_close(struct handle *h) {
    if (!h) return;
    uint64_t flags = irq_save();
    ASSERT(h->refs > 0);
    bool last = --h->refs == 0;
    if (last) events_handle_closed(h);
    irq_restore(flags);
    if (!last) return;
    /* No table holds h any more; an AP syscall that found it there before may still read it
       and its object (handle_ap_peek) until its section ends. */
    ap_read_synchronize();
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
    uint64_t flags = irq_save();
    for (struct object_waiter *w = o->waiters; w; w = w->object_next) {
        w->wait->generation++;
        wq_wake_all(&w->wait->wq);
    }
    irq_restore(flags);
}

void object_wait_add(struct object_wait *wait, struct object *o) {
    if (wait->error || !o) return;
    struct object_waiter *w = kmalloc(sizeof *w);
    if (!w) { wait->error = -N_ERR_NO_MEMORY; return; }
    uint64_t flags = irq_save();
    o->refs++;
    *w = (struct object_waiter){.wait_next = wait->entries, .object_next = o->waiters,
                                .object_prev = &o->waiters, .wait = wait, .object = o};
    if (w->object_next) w->object_next->object_prev = &w->object_next;
    o->waiters = w;
    wait->entries = w;
    irq_restore(flags);
}

void object_wait_end(struct object_wait *wait) {
    while (wait->entries) {
        uint64_t flags = irq_save();
        struct object_waiter *w = wait->entries;
        wait->entries = w->wait_next;
        *w->object_prev = w->object_next;
        if (w->object_next) w->object_next->object_prev = w->object_prev;
        irq_restore(flags);
        object_unref(w->object);
        kfree(w);
    }
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
    uint64_t flags = irq_save();
    struct handle *h = handle_peek(index);
    if (h) h->refs++;
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
