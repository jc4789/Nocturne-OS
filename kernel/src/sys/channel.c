/* Native channels: named rendezvous and message channel pairs (sys/channel.h). */
#include "kernel.h"
#include "arch/cpu.h"
#include "lib/idtable.h"
#include "mm/heap.h"
#include "sys/clock.h"
#include "sys/syscall.h"
#include "sys/object.h"
#include "sys/channel.h"
#include "sys/process.h"
#include "sys/thread.h"
#include "sys/wait.h"

/* A wait's end: CLOCK_NEVER never comes; one already past (0: do not wait) ends it before it
   begins (N_ERR_NOT_READY), one that comes while it waits ends it as N_ERR_TIMEOUT. */
static int wait_over(uint64_t deadline, bool waited) {
    if (deadline == CLOCK_NEVER || clock_now_ns() < deadline) return 0;
    return waited ? -N_ERR_TIMEOUT : -N_ERR_NOT_READY;
}

/* Named native rendezvous: listeners found by key. Each connection is an ordinary message channel
   pair; the server end waits in its listener until accepted. No path lookup, network socket family
   or fixed client capacity.

   Any CPU. names_lock guards the names (a table by the key's hash that grows with the listeners,
   lib/idtable.h) and each listener's place in it: held only to find, add or drop one, nothing
   allocated, closed or waited for under it. A listener in the names has its handle (its object a
   reference). Each listener's own lock guards its pending servers and whether it still listens;
   the two locks are never held together. */
struct channel_pending {
    struct channel_pending *next;
    struct handle *server;
};
struct channel_listener {
    struct id_link name;    /* in names by its key's hash (names_lock) */
    bool published;         /* in names (names_lock) */
    struct spinlock lock;   /* open, head, tail */
    bool open;              /* its handle has not closed: connections still join it */
    struct channel_pending *head, *tail;
    struct object object;
    size_t key_length;
    char key[];
};
static struct spinlock names_lock;
static struct id_table names;

static uint64_t channel_hash(const char *key, size_t length) {
    uint64_t h = 14695981039346656037ull;
    for (size_t i = 0; i < length; i++) h = (h ^ (unsigned char)key[i]) * 1099511628211ull;
    return h;
}

/* names_lock held. */
static struct channel_listener *listener_named(const char *key, size_t length, uint64_t hash) {
    for (struct id_link *n = id_table_find(&names, hash); n; n = id_table_next(n)) {
        struct channel_listener *l = (struct channel_listener *)((char *)n - offsetof(struct channel_listener, name));
        if (l->key_length == length && !memcmp(l->key, key, length)) return l;
    }
    return NULL;
}

/* It only reads: a connect or accept after the look notifies the listener. */
static uint32_t listener_ready(struct handle *h) {
    struct channel_listener *l = h->object->priv;
    return __atomic_load_n(&l->head, __ATOMIC_RELAXED) ? N_EVENT_RECEIVABLE : 0;
}

static void listener_close(struct handle *h) {
    struct channel_listener *l = h->object->priv;
    uint64_t flags = spin_lock(&names_lock);
    if (l->published) {
        id_table_drop(&names, &l->name);
        l->published = false;
    }
    spin_unlock(&names_lock, flags);
    flags = spin_lock(&l->lock);
    l->open = false;
    struct channel_pending *pending = l->head;
    l->head = l->tail = NULL;
    spin_unlock(&l->lock, flags);
    object_notify(&l->object);
    /* A pending server reference is an ordinary owner. Closing it propagates
     * N_ERR_CLOSED to the client and wakes it, even after owner death. */
    while (pending) {
        struct channel_pending *next = pending->next;
        handle_close(pending->server);
        kfree(pending);
        pending = next;
    }
}

static void listener_release(struct object *o) { kfree(o->priv); }

static const struct object_ops listener_ops = {
    .kind = N_KIND_CHANNEL, .ready = listener_ready, .close = listener_close, .release = listener_release,
};

int channel_listen(const char *key, size_t length, struct handle **listener) {
    if (!length || length > SIZE_MAX - sizeof(struct channel_listener)) return -N_ERR_INVALID;
    struct channel_listener *l = kzalloc(sizeof *l + length);
    if (!l) return -N_ERR_NO_MEMORY;
    l->name.id = channel_hash(key, length);
    l->open = true;
    l->key_length = length;
    memcpy(l->key, key, length);
    object_init(&l->object, &listener_ops, l);
    struct handle *h = handle_open(&l->object, 0);
    if (!h) { kfree(l); return -N_ERR_NO_MEMORY; }
    uint64_t flags;
    int e = id_table_room(&names, &names_lock, &flags);
    if (!e && listener_named(key, length, l->name.id)) e = -N_ERR_EXISTS;
    if (!e) {
        id_table_add(&names, &l->name);
        l->published = true;
    }
    spin_unlock(&names_lock, flags);
    if (e) { handle_close(h); return e; }
    *listener = h;
    return 0;
}

/* The listener is found and kept by a reference, the pair made with no lock held, then the server
   end joins the listener unless it closed meanwhile (the pair then goes). */
int channel_connect(const char *key, size_t length, struct handle **client) {
    if (!length) return -N_ERR_INVALID;
    uint64_t hash = channel_hash(key, length);
    uint64_t flags = spin_lock(&names_lock);
    struct channel_listener *l = listener_named(key, length, hash);
    if (l) object_ref(&l->object); /* named, so its handle still holds one */
    spin_unlock(&names_lock, flags);
    if (!l) return -N_ERR_NOT_FOUND;
    struct channel_pending *pending = kmalloc(sizeof *pending);
    struct handle *server = NULL;
    int e = pending ? message_channel_pair(client, &server) : -N_ERR_NO_MEMORY;
    if (!e) {
        pending->next = NULL;
        pending->server = server;
        flags = spin_lock(&l->lock);
        bool open = l->open;
        if (open) {
            if (l->tail) l->tail->next = pending;
            else l->head = pending;
            l->tail = pending;
        }
        spin_unlock(&l->lock, flags);
        if (open) {
            object_notify(&l->object);
            pending = NULL;
        } else {
            e = -N_ERR_NOT_FOUND;
            handle_close(*client);
            *client = NULL;
            handle_close(server);
        }
    }
    kfree(pending);
    object_unref(&l->object);
    return e;
}

int channel_accept(struct handle *listener, struct handle **server) {
    if (!handle_is(listener, &listener_ops)) return -N_ERR_WRONG_KIND;
    struct channel_listener *l = listener->object->priv;
    uint64_t flags = spin_lock(&l->lock);
    struct channel_pending *pending = l->head;
    if (pending) {
        l->head = pending->next;
        if (!l->head) l->tail = NULL;
    }
    spin_unlock(&l->lock, flags);
    if (!pending) return -N_ERR_NOT_READY;
    *server = pending->server;
    kfree(pending);
    return 0;
}

/* Message channels. A pair shares one link; each end receives from its own queue and sends into
   the peer's. A message keeps its boundary and may carry one document or shared-page handle.
   Messages are kept in kernel memory as sent.

   Each end's receiver gives credit: the bytes its queue takes in before senders into it wait. A
   message into an empty queue is always taken, whatever its size, so a message larger than the
   credit still goes; senders wait while the queue holds the credit or more. The credit an end is
   made with is CHANNEL_CREDIT, what the ABI promises a receiver takes in
   (docs/nocturne-native-abi.md §4); until the ABI lets a receiver say its own, every end has it.

   Any CPU sends and receives under the link's lock (the caller's handle keeps the link); the app's
   memory is copied without it. A wait for queue i sleeps in its wait queue, enrolled before the
   lock is given back, as an exclusive item (sys/wait.h): a change wakes as many as it can serve,
   once the lock is dropped. A message in wakes one receiver; a message out wakes one sender, and a
   sender that waited and leaves room after its message wakes the next; a receiver that waited and
   leaves the message where it was (it only looked, it does not fit, or its buffer is bad) wakes
   the next; an end closed wakes them all. The lock is taken before a wait queue's (an object's
   waiters' among them, object_notify). */
#define CHANNEL_CREDIT ((size_t)1 << 20)
struct message {
    struct message *next;
    struct handle *attached;
    size_t size;
    uint64_t sender_app;
    uint8_t bytes[];
};
struct message_link {
    struct spinlock lock;
    struct message *head[2], **tail[2];
    size_t queued[2];               /* bytes waiting in queue i */
    size_t credit[2];               /* end i's receiver's credit: queued[i] it takes before senders wait */
    bool open[2];                   /* end i still has its handle */
    int objects;                    /* ends not yet released: the last frees the link */
    struct wait_queue wait[2];      /* receivers of queue i */
    struct wait_queue send_wait[2]; /* senders into queue i */
    struct object end[2];
};
/* Full while queue side holds its receiver's credit or more, whatever the next message's size:
   N_EVENT_SENDABLE then means a send will not wait. */
static bool message_queue_full(const struct message_link *l, int side) {
    return l->queued[side] >= l->credit[side];
}
static const struct object_ops message_ops;

static struct message_link *message_link(struct handle *h) { return h->object->priv; }
static int message_side(struct handle *h) { return h->object == &message_link(h)->end[1]; }

static void message_free(struct message *m) {
    while (m) {
        struct message *next = m->next;
        handle_close(m->attached);
        kfree(m);
        m = next;
    }
}

bool message_channel_is(struct handle *h) { return handle_is(h, &message_ops); }

static struct message *message_new(size_t n, uint64_t sender_app) {
    if (n > SIZE_MAX - sizeof(struct message)) return NULL;
    struct message *m = kmalloc(sizeof *m + n);
    if (!m) return NULL;
    m->next = NULL;
    m->size = n;
    m->attached = NULL;
    m->sender_app = sender_app;
    return m;
}

/* The lock held, queue i not full or not open: m joins it (taking a reference to attach), or is
   freed if end i has closed. Gives the lock back, with flags, and wakes one of queue i's receivers,
   and, for a sender that waited (woken for the room it used), the next sender while room is left. */
static int64_t message_put(struct message_link *l, int i, struct message *m, struct handle *attach, uint64_t flags,
                           bool waited) {
    if (!l->open[i]) {
        spin_unlock(&l->lock, flags);
        kfree(m);
        return -N_ERR_CLOSED;
    }
    size_t n = m->size;
    if (attach) m->attached = handle_ref(attach);
    *l->tail[i] = m;
    l->tail[i] = &m->next;
    l->queued[i] += n;
    bool room = waited && !message_queue_full(l, i);
    spin_unlock(&l->lock, flags);
    wq_wake(&l->wait[i], 1);
    if (room) wq_wake(&l->send_wait[i], 1);
    object_notify(&l->end[i]);
    return (int64_t)n;
}

/* The lock held, queue i's head m taken: gives the lock back, with flags, and wakes one sender into
   queue i (which wakes the next if its message leaves room). */
static void message_take(struct message_link *l, int i, struct message *m, uint64_t flags) {
    l->head[i] = m->next;
    if (!l->head[i]) l->tail[i] = &l->head[i];
    m->next = NULL;
    l->queued[i] -= m->size;
    spin_unlock(&l->lock, flags);
    wq_wake(&l->send_wait[i], 1);
    object_notify(&l->end[!i]); /* the sender may send again */
}

/* Queues m for the peer of h, or frees it: a full queue is waited out until the deadline. */
static int64_t message_queue(struct handle *h, struct message *m, struct handle *attach, uint64_t deadline) {
    struct message_link *l = message_link(h);
    int peer = !message_side(h);
    uint64_t fl = spin_lock(&l->lock);
    bool waited = false;
    for (; l->open[peer] && message_queue_full(l, peer); waited = true) {
        int e = wait_over(deadline, waited);
        if (!e && thread_current()->killed) e = -N_ERR_STOPPED;
        if (e) { spin_unlock(&l->lock, fl); kfree(m); return e; }
        wq_wait_locked_exclusive_until(&l->send_wait[peer], &l->lock, deadline);
        spin_take(&l->lock);
    }
    return message_put(l, peer, m, attach, fl, waited);
}

int64_t message_channel_send(struct handle *h, const void *udata, size_t n, struct handle *attach, uint64_t deadline) {
    struct message *m = message_new(n, (uint64_t)process_current()->pid);
    if (!m) return n > SIZE_MAX - sizeof(struct message) ? -N_ERR_TOO_LARGE : -N_ERR_NO_MEMORY;
    if (copy_from_user(m->bytes, udata, n) < 0) { kfree(m); return -N_ERR_BAD_ADDRESS; }
    return message_queue(h, m, attach, deadline);
}

int64_t message_channel_post(struct handle *h, const void *data, size_t n, uint64_t sender_app) {
    struct message *m = message_new(n, sender_app);
    if (!m) return -N_ERR_NO_MEMORY;
    memcpy(m->bytes, data, n);
    return message_queue(h, m, NULL, 0);
}

/* peek reports the next message's size without taking it. A message larger
   than cap stays queued and reports N_ERR_TOO_LARGE. discard takes the next
   message off the queue whatever its size, copies only its first cap bytes,
   closes its attachment and reports its whole size. */
int64_t message_channel_receive(struct handle *h, void *ubuf, size_t cap, uint64_t deadline, bool peek,
                                bool discard, struct handle **attached, uint64_t *sender_app) {
    struct message_link *l = message_link(h);
    int side = message_side(h);
    *attached = NULL;
    for (bool waited = false;; waited = true) {
        uint64_t fl = spin_lock(&l->lock);
        struct message *m = l->head[side];
        if (m) {
            /* Leaving the message queued: a receiver woken for it wakes the next. */
            if (peek) {
                if (sender_app) *sender_app = m->sender_app;
                int64_t size = (int64_t)m->size;
                spin_unlock(&l->lock, fl);
                if (waited) wq_wake(&l->wait[side], 1);
                return size;
            }
            if (!discard && m->size > cap) {
                spin_unlock(&l->lock, fl);
                if (waited) wq_wake(&l->wait[side], 1);
                return -N_ERR_TOO_LARGE;
            }
            size_t copy = MIN(m->size, cap);
            /* The buffer is looked at (and paged in, which may sleep) without the lock; a sibling
               that takes the message meanwhile sends this one round again. */
            spin_unlock(&l->lock, fl);
            if (!user_ok_w(ubuf, copy)) {
                if (waited) wq_wake(&l->wait[side], 1);
                return -N_ERR_BAD_ADDRESS;
            }
            fl = spin_lock(&l->lock);
            if (l->head[side] != m || MIN(m->size, cap) != copy) { spin_unlock(&l->lock, fl); continue; }
            message_take(l, side, m, fl);
            /* Taken off the queue: copied out without the lock. Only a sibling unmapping the
               buffer meanwhile loses it. */
            if (copy_to_user(ubuf, m->bytes, copy) < 0) { message_free(m); return -N_ERR_BAD_ADDRESS; }
            int64_t size = (int64_t)m->size;
            if (sender_app) *sender_app = m->sender_app;
            if (discard) { message_free(m); return size; }
            *attached = m->attached;
            m->attached = NULL;
            kfree(m);
            return size;
        }
        int e = !l->open[!side] ? -N_ERR_CLOSED : wait_over(deadline, waited);
        if (!e && thread_current()->killed) e = -N_ERR_STOPPED;
        if (e) { spin_unlock(&l->lock, fl); return e; }
        wq_wait_locked_exclusive_until(&l->wait[side], &l->lock, deadline);
        irq_restore(fl);
    }
}

static uint32_t message_ready(struct handle *h) {
    struct message_link *l = message_link(h);
    int side = message_side(h), peer = !side;
    uint32_t ready = 0;
    if (l->head[side] || !l->open[peer]) ready |= N_EVENT_RECEIVABLE;
    /* a closed peer reports N_ERR_CLOSED at once */
    if (!l->open[peer] || !message_queue_full(l, peer)) ready |= N_EVENT_SENDABLE;
    if (!l->open[peer]) ready |= N_EVENT_CLOSED;
    return ready;
}

static void message_close(struct handle *h) {
    struct message_link *l = message_link(h);
    int side = message_side(h);
    uint64_t fl = spin_lock(&l->lock);
    l->open[side] = false;
    /* Nobody can receive this side's queue any more: drop it now, so an
     * attached handle is not kept alive by a dead end. */
    struct message *dropped = l->head[side];
    l->head[side] = NULL;
    l->tail[side] = &l->head[side];
    l->queued[side] = 0;
    spin_unlock(&l->lock, fl);
    /* the peer's receivers now get N_ERR_CLOSED, and senders into this end */
    wq_wake_all(&l->wait[!side]);
    wq_wake_all(&l->wait[side]);
    wq_wake_all(&l->send_wait[side]);
    object_notify(&l->end[!side]);
    message_free(dropped);
}

/* Every handle of the end was closed before its last reference went: nothing reaches the link
   through it any more. The other end's last release frees it. */
static void message_release(struct object *o) {
    struct message_link *l = o->priv;
    if (__atomic_sub_fetch(&l->objects, 1, __ATOMIC_ACQ_REL)) return;
    message_free(l->head[0]);
    message_free(l->head[1]);
    kfree(l);
}

static const struct object_ops message_ops = {
    .kind = N_KIND_CHANNEL, .ready = message_ready, .close = message_close, .release = message_release,
};

int message_channel_pair(struct handle **a, struct handle **b) {
    struct message_link *l = kzalloc(sizeof *l);
    if (!l) return -N_ERR_NO_MEMORY;
    for (int i = 0; i < 2; i++) {
        l->tail[i] = &l->head[i];
        l->credit[i] = CHANNEL_CREDIT;
        l->open[i] = true;
        object_init(&l->end[i], &message_ops, l);
    }
    l->objects = 2;
    *a = handle_open(&l->end[0], HANDLE_WRITE);
    *b = *a ? handle_open(&l->end[1], HANDLE_WRITE) : NULL;
    if (!*b) {
        /* An opened end is released through its handle; the other by hand. */
        if (*a) { handle_close(*a); *a = NULL; message_release(&l->end[1]); }
        else kfree(l);
        return -N_ERR_NO_MEMORY;
    }
    return 0;
}
