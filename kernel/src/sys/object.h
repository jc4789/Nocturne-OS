/* Kernel objects and the handles apps hold to them (docs/nocturne-native-abi.md §1).

   An object is something the kernel keeps for apps: a document, a channel end, a window, shared
   pages, an app record, an event queue, a device stream, a PCI claim. A handle is one opening of
   an object, with the rights it was opened with; an app's handle number names one. Several
   handles may reach one object (two openings of a document), and one handle may sit in several
   places at once (the tables of the apps it was handed to, a message that carries it), each
   place holding a reference of its own.

   Any CPU. A table is read without a lock (sys/epoch.h): a reader takes a reference to the handle
   it finds unless the handle's last one is already gone, and the memory of a table's replaced
   array and of a closed handle goes back only once no reader can be looking at it. Each table's
   lock orders its changes. A handle's references and an object's are atomic counts; the last one
   ends it on the CPU that dropped it. What waits for a change to an object waits in its waiters,
   a wait queue (sys/wait.h) that object_notify wakes whole. */
#pragma once
#include "kernel.h"
#include "arch/spin.h"
#include "sys/epoch.h"
#include "sys/wait.h"

struct object;
struct handle;
struct event_source;

struct object_ops {
    int kind; /* N_KIND_*, what n_handle_kind reports */
    /* Byte streams (the console, the system log, nothing): a receive takes what is there (wait:
       until something is), a send takes it all. user: buf is the calling app's memory. NULL:
       the object is not a byte stream. */
    int64_t (*read)(struct handle *h, void *buf, size_t n, bool user, bool wait);
    int64_t (*write)(struct handle *h, const void *buf, size_t n, bool user);
    /* The N_EVENT_* conditions true for h now, with an event queue's lock held: it only reads
       (a waiter that misses a change is woken by the change's notify): no wait, allocation or
       write, and no lock but the spinlocks that come after an event queue's. NULL: never any. */
    uint32_t (*ready)(struct handle *h);
    /* The handle's last reference is gone (it may sleep: an audio stream plays out). */
    void (*close)(struct handle *h);
    /* The object's last reference is gone: it frees itself. */
    void (*release)(struct object *o);
    /* When an app's table closes (its end), its handles to objects of this kind close before its
       others and before its mappings go: a device claim, whose device stops before the memory it
       could still write into is freed (docs/nocturne-native-abi.md §9). */
    bool closes_first;
};

struct object {
    const struct object_ops *ops;
    int refs;
    void *priv;
    /* What waits for its next change: event queues' watches of its handles (callback items,
       sys/events.c), an event queue's own threads in its wait (thread items, exclusive), and any
       other part of the kernel's callback item (a network setup's cancel, net/net.c). A callback
       item hears one change; its owner adds it again for the next. */
    struct wait_queue waiters;
};

/* A handle's rights: what its opening may do with the object, as its users check them. */
#define HANDLE_WRITE (1u << 0) /* change it: a document opened to edit or replace; shared pages (or
                                  a document) mapped for writing, or scanned out as a screen */

struct handle {
    struct object *object;
    int refs;
    uint32_t rights;               /* HANDLE_* */
    void *priv;                    /* the object's state for this opening */
    /* Event queues' watches of it (sys/events.c), which its last close ends. watch_lock guards
       the list, and is a leaf. */
    struct event_source *watchers;
    struct spinlock watch_lock;
    struct epoch_item retire;      /* its memory's way back after its last close */
};

/* An object with no references yet: the first handle opened on it takes one. */
void object_init(struct object *o, const struct object_ops *ops, void *priv);
void object_ref(struct object *o);
/* A reference unless the last one is already gone (its release may be under way): false then. */
bool object_ref_live(struct object *o);
/* Any CPU; the last reference releases the object there (an object some CPU may drop last has a
   release that any CPU can run). */
void object_unref(struct object *o);

/* A new opening of o with rights (NULL: no memory; o is left as it was). */
struct handle *handle_open(struct object *o, uint32_t rights);
struct handle *handle_ref(struct handle *h);
/* Drops one reference; the last one closes the opening and drops its object reference. It may
   sleep (an audio stream plays out what it has): never with a spinlock held. */
void handle_close(struct handle *h);
static inline int handle_kind(const struct handle *h) { return h->object->ops->kind; }
static inline bool handle_is(const struct handle *h, const struct object_ops *ops) {
    return h && h->object->ops == ops;
}
/* Whether h's opening has every right in rights. */
static inline bool handle_allows(const struct handle *h, uint32_t rights) { return (h->rights & rights) == rights; }
uint32_t handle_ready(struct handle *h); /* as ops->ready */

/* Any CPU, any context: a change to o wakes all its waiters (wq_wake_all), each callback item
   called under the waiters' lock (sys/wait.h's contract). The caller may hold its own spinlocks,
   those that come before a wait queue's (the callbacks take only later ones). */
void object_notify(struct object *o);

/* An app's handles. A numbered one is the app's to name: its number is its slot and the slot's
   generation, which moves on each time the slot is emptied, so a number kept after its handle
   closed names nothing, even once the slot holds another. A held one has no number: a part of the
   kernel keeps it for state of its own that ends with the app (its network connections,
   net/net.c), one per kind of object. Readers take no lock (sys/epoch.h); lock orders the
   changes: slots, the free slots' line, frontier and held. */
struct handle_slots;
struct handle_held;
struct handle_table {
    struct handle_slots *slots; /* NULL until the first handle; replaced whole as it grows */
    uint32_t free_first, free_last; /* emptied slots, the longest empty first: reused in that order */
    uint32_t frontier;          /* the slots from here on were never used */
    struct handle_held *held;
    struct spinlock lock;
};
void handle_table_init(struct handle_table *t);

/* The calling app's handle table (its process's, for any of its threads). */
struct handle *handle_get(int number); /* a reference to close, or NULL: no such handle */
struct handle *handle_get_of(struct handle_table *t, int number);
/* Puts h in the table: its number, or an error (then h is closed). Consumes the reference. */
int handle_install(struct handle *h);
/* Both or neither: 0 with their numbers, the references consumed; an error, and both are still
   the caller's. */
int handle_install_pair(struct handle *a, struct handle *b, int *na, int *nb);
/* Takes the handle out of the table and drops the table's reference. */
int handle_remove(int number);
/* The calling app holds h, with no number, until its table closes: 1. 0 when it holds a handle to
   an object of h's kind already (another of its threads was first), -N_ERR_NO_MEMORY without
   memory: h is closed then. Consumes the reference. */
int handle_hold(struct handle *h);
/* Whether the calling app holds a handle with no number to an object of ops. */
bool handle_held(const struct object_ops *ops);

/* A process not started yet (sys/proc.c), its handles put in rising order: h, a reference of its
   own, as number index (the slot's first generation: the number is index). */
int handle_table_put(struct handle_table *t, uint32_t index, struct handle *h);
/* The app ends, or never started (sys/process.c): every handle leaves t and is closed, those to
   objects whose kind closes_first before the others, the held ones last. t is empty after. No
   spinlock held: a close may sleep. */
void handle_table_close(struct handle_table *t);
