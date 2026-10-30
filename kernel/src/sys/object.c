/* Kernel objects, handles, waits on objects and the apps' handle tables (sys/object.h). */
#include "kernel.h"
#include "arch/cpu.h"
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
    int refs = __atomic_sub_fetch(&h->refs, 1, __ATOMIC_ACQ_REL);
    ASSERT(refs >= 0);
    if (refs) return;
    /* No table holds h any more, and no one else has a reference: its watches end. */
    events_watches_free(events_handle_closed(h));
    /* A last close may sleep (an audio stream plays out what it has): the reference this
       caller held keeps the detached handle alive meanwhile. */
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

/* The table's lock held: a free slot, or -1 when the table is full. */
static int table_free_slot(struct task *owner) {
    for (size_t i = owner->handle_hint; i < owner->handle_capacity; i++)
        if (!owner->handles[i]) return (int)i;
    return -1;
}

/* The table's lock held, given back meanwhile: room for minimum slots (an error without memory). */
static int table_grow(struct task *owner, size_t minimum, uint64_t *flags) {
    const size_t limit = (size_t)INT32_MAX + 1; /* handle numbers are non-negative int32_t */
    if (minimum > limit || minimum > SIZE_MAX / sizeof *owner->handles) return -N_ERR_NO_MEMORY;
    while (minimum > owner->handle_capacity) {
        size_t capacity = owner->handle_capacity;
        while (capacity < minimum) {
            if (capacity > limit / 2) { capacity = minimum; break; }
            capacity *= 2;
        }
        spin_unlock(&owner->handles_lock, *flags);
        struct handle **table = kzalloc(capacity * sizeof *table);
        *flags = spin_lock(&owner->handles_lock);
        if (!table) return -N_ERR_NO_MEMORY;
        if (capacity <= owner->handle_capacity) { /* another thread grew it meanwhile */
            spin_unlock(&owner->handles_lock, *flags);
            kfree(table);
            *flags = spin_lock(&owner->handles_lock);
            continue;
        }
        memcpy(table, owner->handles, owner->handle_capacity * sizeof *table);
        struct handle **old = owner->handles;
        owner->handles = table;
        owner->handle_capacity = capacity;
        if (old != owner->initial_handles) {
            spin_unlock(&owner->handles_lock, *flags);
            kfree(old); /* no one reads a table but under its lock */
            *flags = spin_lock(&owner->handles_lock);
        }
    }
    return 0;
}

/* The table's lock held: h in a free slot (its number), or an error. */
static int table_add(struct task *owner, struct handle *h, uint64_t *flags) {
    int index;
    while ((index = table_free_slot(owner)) < 0) {
        int e = table_grow(owner, owner->handle_capacity + 1, flags);
        if (e < 0) return e;
    }
    owner->handles[index] = h;
    owner->handle_hint = (size_t)index + 1;
    return index;
}

/* The table's lock held. */
static struct handle *table_take(struct task *owner, size_t index) {
    if (index >= owner->handle_capacity || !owner->handles[index]) return NULL;
    struct handle *h = owner->handles[index];
    owner->handles[index] = NULL;
    if (index < owner->handle_hint) owner->handle_hint = index;
    return h;
}

struct handle *handle_get_of(struct task *owner, int index) {
    if (index < 0) return NULL;
    uint64_t flags = spin_lock(&owner->handles_lock);
    struct handle *h = (size_t)index < owner->handle_capacity ? owner->handles[index] : NULL;
    if (h) handle_ref(h); /* the table's reference keeps it until this one is taken */
    spin_unlock(&owner->handles_lock, flags);
    return h;
}

struct handle *handle_get(int index) { return handle_get_of(task_process(), index); }

int handle_install(struct handle *h) {
    struct task *owner = task_process();
    uint64_t flags = spin_lock(&owner->handles_lock);
    int index = table_add(owner, h, &flags);
    spin_unlock(&owner->handles_lock, flags);
    if (index < 0) handle_close(h);
    return index;
}

int handle_install_pair(struct handle *a, struct handle *b, int *ia, int *ib) {
    struct task *owner = task_process();
    uint64_t flags = spin_lock(&owner->handles_lock);
    int first = table_add(owner, a, &flags), second = first < 0 ? first : table_add(owner, b, &flags);
    if (second < 0 && first >= 0) table_take(owner, (size_t)first);
    spin_unlock(&owner->handles_lock, flags);
    if (second < 0) return second;
    *ia = first;
    *ib = second;
    return 0;
}

int handle_remove(int index) {
    struct task *owner = task_process();
    if (index < 0) return -N_ERR_NO_HANDLE;
    uint64_t flags = spin_lock(&owner->handles_lock);
    struct handle *h = table_take(owner, (size_t)index);
    spin_unlock(&owner->handles_lock, flags);
    if (!h) return -N_ERR_NO_HANDLE;
    object_notify(h->object);
    /* The last close may sleep: the reference taken out of the table owns h. */
    handle_close(h);
    return 0;
}

int handle_table_put(struct task *owner, size_t index, struct handle *h) {
    uint64_t flags = spin_lock(&owner->handles_lock);
    int e = table_grow(owner, index + 1, &flags);
    if (!e) {
        ASSERT(!owner->handles[index]);
        owner->handles[index] = h;
    }
    spin_unlock(&owner->handles_lock, flags);
    return e;
}

struct handle *handle_table_next(struct task *owner, size_t *at, bool (*which)(struct handle *)) {
    uint64_t flags = spin_lock(&owner->handles_lock);
    struct handle *h = NULL;
    for (; *at < owner->handle_capacity && !h; ++*at) {
        struct handle *in = owner->handles[*at];
        if (in && (!which || which(in))) h = table_take(owner, *at);
    }
    spin_unlock(&owner->handles_lock, flags);
    return h;
}

void handle_table_free(struct task *owner) {
    if (owner->handles != owner->initial_handles) kfree(owner->handles);
    owner->handles = owner->initial_handles;
    owner->handle_capacity = INITIAL_HANDLES;
}
/* ---- handle tables ---- */
