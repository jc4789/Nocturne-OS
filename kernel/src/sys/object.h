/* Kernel objects and the handles apps hold to them (docs/nocturne-native-abi.md §1).

   An object is something the kernel keeps for apps: a document, a channel end, a window, shared
   pages, an app record, an event queue, a device stream, a PCI claim. A handle is one opening of
   an object, with the access it was opened with; an app's handle number names one. Several
   handles may reach one object (two openings of a document), and one handle may sit in several
   places at once (the tables of the apps it was handed to, a message that carries it), each
   place holding a reference of its own.

   All of this state is BSP-owned and changed under the BSP IRQ guard (irq_save). */
#pragma once
#include "kernel.h"
#include "sys/sched.h"

struct object;
struct handle;
struct object_waiter;
struct event_watch;

/* One wait for many objects: the waiter registers with each object it waits for, checks its
   condition, and sleeps on wq unless generation moved on meanwhile. Any change to a registered
   object moves generation on and wakes the waiter, which then checks again. */
struct object_wait {
    struct wait_queue wq;
    uint64_t generation;
    struct object_waiter *entries;
    int error;
};

struct object_ops {
    int kind; /* N_KIND_*, what n_handle_kind reports */
    /* Byte streams (the console, the system log, nothing): a receive takes what is there (wait:
       until something is), a send takes it all. user: buf is the calling app's memory. NULL:
       the object is not a byte stream. */
    int64_t (*read)(struct handle *h, void *buf, size_t n, bool user, bool wait);
    int64_t (*write)(struct handle *h, const void *buf, size_t n, bool user);
    /* The N_EVENT_* conditions true for h now; BSP IRQ guard held. NULL: never any. */
    uint32_t (*ready)(struct handle *h);
    /* The handle's last reference is gone (it may sleep: an audio stream plays out). */
    void (*close)(struct handle *h);
    /* The object's last reference is gone: it frees itself. */
    void (*release)(struct object *o);
};

struct object {
    const struct object_ops *ops;
    int refs;
    void *priv;
    struct object_waiter *waiters;
};

struct handle {
    struct object *object;
    int refs;
    bool writable;                /* opened for change (a document to edit, shared pages to write) */
    void *priv;                   /* the object's state for this opening */
    struct event_watch *watchers; /* event-queue watches on it, dropped at its last close */
};

/* An object with no references yet: the first handle opened on it takes one. */
void object_init(struct object *o, const struct object_ops *ops, void *priv);
void object_ref(struct object *o);
void object_unref(struct object *o);

/* A new opening of o (NULL: no memory; o is left as it was). */
struct handle *handle_open(struct object *o, bool writable);
struct handle *handle_ref(struct handle *h);
/* Drops one reference; the last one closes the opening and drops its object reference. */
void handle_close(struct handle *h);
static inline int handle_kind(const struct handle *h) { return h->object->ops->kind; }
static inline bool handle_is(const struct handle *h, const struct object_ops *ops) {
    return h && h->object->ops == ops;
}
uint32_t handle_ready(struct handle *h); /* BSP IRQ guard held */

/* Waiting: a registration holds the object (not a handle), so a sibling closing the last handle
   still wakes the waiter, which then finds it closed. */
void object_notify(struct object *o);
void object_wait_add(struct object_wait *wait, struct object *o);
void object_wait_end(struct object_wait *wait);

/* The calling app's handle table. handle_get returns a reference (NULL: no such handle);
   handle_peek a borrowed pointer valid only inside the caller's BSP IRQ guard. */
struct handle *handle_get(int index);
struct handle *handle_peek(int index);
/* Puts h in the table: its number, or an error (then h is closed). Consumes the reference. */
int handle_install(struct handle *h);
/* The same with the caller's guard held; the reference is consumed only on success. */
int handle_install_locked(struct handle *h);
/* Takes the handle out of the table and drops the table's reference. */
int handle_remove(int index);
/* The same with the caller's guard held, handing the table's reference to the caller (NULL: no
   such handle). */
struct handle *handle_take_locked(int index);

/* Handle tables of any task; caller holds the BSP IRQ guard. A table grows as far as memory
   allows: its initial size is no limit. */
int handle_table_reserve(struct task *owner, size_t minimum);
int handle_table_find(struct task *owner);
int handle_table_add(struct task *owner, struct handle *h);
