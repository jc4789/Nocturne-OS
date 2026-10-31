/* Kernel objects, handles, waits on objects and the apps' handle tables (sys/object.h). */
#include "kernel.h"
#include "arch/cpu.h"
#include "mm/heap.h"
#include "sys/epoch.h"
#include "sys/object.h"
#include "sys/events.h"
#include "sys/process.h"

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

struct handle *handle_open(struct object *o, uint32_t rights) {
    struct handle *h = kzalloc(sizeof *h);
    if (!h) return NULL;
    h->object = o;
    h->refs = 1;
    h->rights = rights;
    object_ref(o);
    return h;
}

struct handle *handle_ref(struct handle *h) {
    __atomic_add_fetch(&h->refs, 1, __ATOMIC_RELAXED);
    return h;
}

/* A reference to h, found where a lock-free reader found it, unless its last one is gone. */
static bool handle_ref_live(struct handle *h) {
    int refs = __atomic_load_n(&h->refs, __ATOMIC_RELAXED);
    do {
        if (refs <= 0) return false;
    } while (!__atomic_compare_exchange_n(&h->refs, &refs, refs + 1, true, __ATOMIC_ACQUIRE, __ATOMIC_RELAXED));
    return true;
}

static void handle_free(struct epoch_item *item) {
    kfree((char *)item - offsetof(struct handle, retire));
}

void handle_close(struct handle *h) {
    if (!h) return;
    int refs = __atomic_sub_fetch(&h->refs, 1, __ATOMIC_ACQ_REL);
    ASSERT(refs >= 0);
    if (refs) return;
    /* No table holds h any more, and no one else has a reference: its watches end. */
    events_handle_closed(h);
    /* A last close may sleep (an audio stream plays out what it has): the reference this
       caller held keeps the detached handle alive meanwhile. */
    struct object *o = h->object;
    if (o->ops->close) o->ops->close(h);
    object_unref(o);
    /* a table's reader may still be looking at it (handle_get_of): its memory goes after them */
    epoch_defer(&h->retire, handle_free);
}

uint32_t handle_ready(struct handle *h) {
    return h->object->ops->ready ? h->object->ops->ready(h) : 0;
}

void object_notify(struct object *o) {
    if (o) wq_wake_all(&o->waiters);
}

/* ---- handle tables ---- */

/* A number (n_handle: a non-negative int32_t, docs/nocturne-native-abi.md §1) is a slot in its
   low HANDLE_SLOT_BITS and the slot's generation in the rest: 2^23 handles open in one app at
   once, each holding kernel memory of its own; 2^8 generations, so a number kept past its close
   names nothing until its slot has been emptied that many times again. A slot's first generation
   is 0: its number is its index (an app's first handles, 0, 1, 2..., are what it was handed). */
#define HANDLE_SLOT_BITS 23
#define HANDLE_SLOTS ((uint32_t)1 << HANDLE_SLOT_BITS)
#define HANDLE_GENERATIONS ((uint32_t)1 << (31 - HANDLE_SLOT_BITS))
#define HANDLE_SLOTS_FIRST 32u /* the first array's, never a limit */
#define SLOT_NONE UINT32_MAX   /* the end of the free slots' line */

struct handle_slot {
    struct handle *handle; /* NULL while empty */
    uint32_t generation;
    uint32_t next;         /* empty: the next in the free slots' line */
};

/* A table's array. A reader may hold it after it was replaced, and reads what the table held then. */
struct handle_slots {
    uint32_t capacity;
    struct epoch_item retire; /* its way back once replaced */
    struct handle_slot slot[];
};

struct handle_held {
    struct handle_held *next;
    struct handle *handle;
};

void handle_table_init(struct handle_table *t) {
    *t = (struct handle_table){.free_first = SLOT_NONE, .free_last = SLOT_NONE};
}

static struct handle_table *own_table(void) { return &process_current()->handles; }

static int handle_number(uint32_t index, uint32_t generation) { return (int)(generation << HANDLE_SLOT_BITS | index); }

static void slots_free(struct epoch_item *item) {
    kfree((char *)item - offsetof(struct handle_slots, retire));
}

/* The lock held, given back meanwhile: room for minimum slots, or an error. The caller looks at
   the table again: it may have changed. */
static int table_grow(struct handle_table *t, uint32_t minimum, uint64_t *flags) {
    if (minimum > HANDLE_SLOTS) return -N_ERR_NO_MEMORY;
    uint32_t capacity = t->slots ? t->slots->capacity : 0;
    if (minimum <= capacity) return 0;
    uint32_t want = capacity ? capacity : HANDLE_SLOTS_FIRST;
    while (want < minimum) want = want > HANDLE_SLOTS / 2 ? HANDLE_SLOTS : want * 2;
    spin_unlock(&t->lock, *flags);
    struct handle_slots *grown = kmalloc(sizeof *grown + (size_t)want * sizeof grown->slot[0]);
    *flags = spin_lock(&t->lock);
    if (!grown) return -N_ERR_NO_MEMORY;
    struct handle_slots *old = t->slots;
    capacity = old ? old->capacity : 0;
    if (want <= capacity) { /* another thread grew it meanwhile */
        spin_unlock(&t->lock, *flags);
        kfree(grown);
        *flags = spin_lock(&t->lock);
        return 0;
    }
    grown->capacity = want;
    if (capacity) memcpy(grown->slot, old->slot, (size_t)capacity * sizeof grown->slot[0]);
    for (uint32_t i = capacity; i < want; i++) grown->slot[i] = (struct handle_slot){.next = SLOT_NONE};
    __atomic_store_n(&t->slots, grown, __ATOMIC_RELEASE);
    if (old) epoch_defer(&old->retire, slots_free);
    return 0;
}

/* The lock held: index joins the end of the free slots' line. */
static void free_slot(struct handle_table *t, uint32_t index) {
    t->slots->slot[index].next = SLOT_NONE;
    if (t->free_last == SLOT_NONE) t->free_first = index;
    else t->slots->slot[t->free_last].next = index;
    t->free_last = index;
}

/* The lock held: h in the longest empty slot, or a new one (its number), or an error. */
static int table_add(struct handle_table *t, struct handle *h, uint64_t *flags) {
    uint32_t index;
    for (;;) {
        index = t->free_first;
        if (index != SLOT_NONE) {
            t->free_first = t->slots->slot[index].next;
            if (t->free_first == SLOT_NONE) t->free_last = SLOT_NONE;
            break;
        }
        if (t->slots && t->frontier < t->slots->capacity) {
            index = t->frontier++;
            break;
        }
        int e = table_grow(t, t->frontier + 1, flags);
        if (e < 0) return e;
    }
    struct handle_slot *s = &t->slots->slot[index];
    /* its generation was set as it emptied: a reader that finds h finds it */
    __atomic_store_n(&s->handle, h, __ATOMIC_RELEASE);
    return handle_number(index, s->generation);
}

/* The lock held: the handle number names, out of its slot, which moves to its next generation. */
static struct handle *table_take(struct handle_table *t, int number) {
    uint32_t index = (uint32_t)number & (HANDLE_SLOTS - 1), generation = (uint32_t)number >> HANDLE_SLOT_BITS;
    if (number < 0 || index >= t->frontier) return NULL;
    struct handle_slot *s = &t->slots->slot[index];
    if (!s->handle || s->generation != generation) return NULL;
    struct handle *h = s->handle;
    __atomic_store_n(&s->handle, NULL, __ATOMIC_RELAXED);
    /* after the slot empties: a reader that reads it after the old handle finds the number stale */
    __atomic_store_n(&s->generation, (generation + 1) & (HANDLE_GENERATIONS - 1), __ATOMIC_RELEASE);
    free_slot(t, index);
    return h;
}

struct handle *handle_get_of(struct handle_table *t, int number) {
    if (number < 0) return NULL;
    uint32_t index = (uint32_t)number & (HANDLE_SLOTS - 1), generation = (uint32_t)number >> HANDLE_SLOT_BITS;
    struct handle *h = NULL;
    epoch_enter();
    struct handle_slots *slots = __atomic_load_n(&t->slots, __ATOMIC_ACQUIRE);
    if (slots && index < slots->capacity) {
        struct handle_slot *s = &slots->slot[index];
        h = __atomic_load_n(&s->handle, __ATOMIC_ACQUIRE);
        /* the generation read after the handle: a handle put in after the number's went out comes
           with the slot's next generation */
        if (h && (__atomic_load_n(&s->generation, __ATOMIC_ACQUIRE) != generation || !handle_ref_live(h))) h = NULL;
    }
    epoch_leave();
    return h;
}

struct handle *handle_get(int number) { return handle_get_of(own_table(), number); }

int handle_install(struct handle *h) {
    struct handle_table *t = own_table();
    uint64_t flags = spin_lock(&t->lock);
    int number = table_add(t, h, &flags);
    spin_unlock(&t->lock, flags);
    if (number < 0) handle_close(h);
    return number;
}

int handle_install_pair(struct handle *a, struct handle *b, int *na, int *nb) {
    struct handle_table *t = own_table();
    uint64_t flags = spin_lock(&t->lock);
    int first = table_add(t, a, &flags), second = first < 0 ? first : table_add(t, b, &flags);
    if (second < 0 && first >= 0) table_take(t, first);
    spin_unlock(&t->lock, flags);
    if (second < 0) return second;
    *na = first;
    *nb = second;
    return 0;
}

int handle_remove(int number) {
    struct handle_table *t = own_table();
    uint64_t flags = spin_lock(&t->lock);
    struct handle *h = table_take(t, number);
    spin_unlock(&t->lock, flags);
    if (!h) return -N_ERR_NO_HANDLE;
    object_notify(h->object);
    /* The last close may sleep: the reference taken out of the table owns h. */
    handle_close(h);
    return 0;
}

/* The table's lock held: its held handle to an object of ops, or NULL. */
static struct handle_held *held_of(struct handle_table *t, const struct object_ops *ops) {
    for (struct handle_held *k = t->held; k; k = k->next)
        if (k->handle->object->ops == ops) return k;
    return NULL;
}

int handle_hold(struct handle *h) {
    struct handle_table *t = own_table();
    struct handle_held *k = kmalloc(sizeof *k);
    if (!k) {
        handle_close(h);
        return -N_ERR_NO_MEMORY;
    }
    uint64_t flags = spin_lock(&t->lock);
    bool kept = !held_of(t, h->object->ops);
    if (kept) {
        *k = (struct handle_held){.next = t->held, .handle = h};
        t->held = k;
    }
    spin_unlock(&t->lock, flags);
    if (!kept) { /* another thread of the app was first */
        kfree(k);
        handle_close(h);
    }
    return kept;
}

bool handle_held(const struct object_ops *ops) {
    struct handle_table *t = own_table();
    uint64_t flags = spin_lock(&t->lock);
    bool held = held_of(t, ops) != NULL;
    spin_unlock(&t->lock, flags);
    return held;
}

int handle_table_put(struct handle_table *t, uint32_t index, struct handle *h) {
    uint64_t flags = spin_lock(&t->lock);
    ASSERT(index >= t->frontier);
    int e = table_grow(t, index + 1, &flags);
    if (!e) {
        /* the slots passed over are empty, and taken first */
        while (t->frontier < index) free_slot(t, t->frontier++);
        t->frontier = index + 1;
        __atomic_store_n(&t->slots->slot[index].handle, h, __ATOMIC_RELEASE);
    }
    spin_unlock(&t->lock, flags);
    return e;
}

void handle_table_close(struct handle_table *t) {
    /* The numbered handles leave one at a time, each closed with the lock let go: first those of
       the kinds that close first, whatever their numbers, then the rest. */
    for (int pass = 0; pass < 2; pass++) {
        for (uint32_t at = 0;;) {
            uint64_t flags = spin_lock(&t->lock);
            struct handle *h = NULL;
            for (; at < t->frontier && !h; at++) {
                const struct handle_slot *s = &t->slots->slot[at];
                if (s->handle && (pass || s->handle->object->ops->closes_first))
                    h = table_take(t, handle_number(at, s->generation));
            }
            spin_unlock(&t->lock, flags);
            if (!h) break;
            handle_close(h);
        }
    }
    uint64_t flags = spin_lock(&t->lock);
    struct handle_held *held = t->held;
    struct handle_slots *slots = t->slots;
    t->held = NULL;
    __atomic_store_n(&t->slots, NULL, __ATOMIC_RELEASE);
    t->frontier = 0;
    t->free_first = t->free_last = SLOT_NONE;
    spin_unlock(&t->lock, flags);
    while (held) {
        struct handle_held *next = held->next;
        handle_close(held->handle);
        kfree(held);
        held = next;
    }
    if (slots) epoch_defer(&slots->retire, slots_free);
}
