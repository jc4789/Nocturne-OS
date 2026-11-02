/* Native channels: message channel pairs (sys/channel.h). 名前はカーネルに無い（sys/service.c）。 */
#include "kernel.h"
#include "arch/cpu.h"
#include "mm/heap.h"
#include "mm/pmm.h"
#include "sys/authority.h"
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
   waiters' among them, object_notify).

   A message lives in the kernel's heap from its send to its receipt. One an app sends is charged to
   it (struct process's messages) until then: the messages an app has waiting in all its channels
   together may take at most half the memory free when it sends another, so no app takes the heap
   from the rest, and a message that would go beyond is refused (N_ERR_NO_MEMORY). The kernel's own
   messages (message_channel_post) are not charged.

   身元：各端は who に身元を持ち、もう一端の受け手が SYS_CHANNEL_PEER で読む。link の錠が守る。

   channel の端もメッセージに添付できる。端が端を運ぶ輪を作らないよう、運ばれている途中の端は別の端を
   受け取らず、別の端を運んでいる端は運ばれない（N_ERR_BUSY）。輪ができなければ、閉じた端の列は必ず捨て
   られる。in_transit と carrying は transit_lock（葉）が守る。link の錠の下で取る。 */
#define CHANNEL_CREDIT ((size_t)1 << 20)
struct message {
    struct message *next;
    struct handle *attached;
    size_t size;
    uint64_t sender_app;
    struct process *charged; /* the app the bytes are charged to (a reference), NULL: the kernel's */
    struct message_link *carrier; /* attached が channel の端のとき、それを運ぶ link（寿命は列の間） */
    int carrier_side;
    uint8_t bytes[];
};
struct message_link {
    struct spinlock lock;
    struct message *head[2], **tail[2];
    size_t queued[2];               /* bytes waiting in queue i */
    size_t credit[2];               /* end i's receiver's credit: queued[i] it takes before senders wait */
    bool open[2];                   /* end i still has its handle */
    struct n_identity who[2];       /* end i の身元 */
    int in_transit[2];              /* end i を添付したメッセージの数（transit_lock） */
    int carrying[2];                /* queue i にある channel の端の添付の数（transit_lock） */
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
static struct spinlock transit_lock;

static struct message_link *message_link(struct handle *h) { return h->object->priv; }
static int message_side(struct handle *h) { return h->object == &message_link(h)->end[1]; }

/* 添付を m に載せる。端を運べないなら false。link の錠の下で（queue i へ入る）。 */
static bool transit_begin(struct message_link *l, int i, struct message *m, struct handle *attach) {
    if (!message_channel_is(attach)) return true;
    struct message_link *a = message_link(attach);
    int side = message_side(attach);
    if (a == l) return false; /* 自分の channel で自分の端を運ばない */
    uint64_t f = spin_lock(&transit_lock);
    bool ok = !l->in_transit[i] && !a->carrying[side];
    if (ok) {
        l->carrying[i]++;
        a->in_transit[side]++;
        m->carrier = l;
        m->carrier_side = i;
    }
    spin_unlock(&transit_lock, f);
    return ok;
}

/* m が添付を手放す前に（添付の link はまだ生きている）。 */
static void transit_end(struct message *m) {
    if (!m->carrier) return;
    struct message_link *a = message_link(m->attached);
    int side = message_side(m->attached);
    uint64_t f = spin_lock(&transit_lock);
    m->carrier->carrying[m->carrier_side]--;
    a->in_transit[side]--;
    spin_unlock(&transit_lock, f);
    m->carrier = NULL;
}

/* m's memory back, and its charge (not its attachment). */
static void message_drop(struct message *m) {
    struct process *p = m->charged;
    if (p) {
        __atomic_sub_fetch(&p->messages, m->size, __ATOMIC_RELAXED);
        process_put(p);
    }
    kfree(m);
}

static void message_free(struct message *m) {
    while (m) {
        struct message *next = m->next;
        transit_end(m);
        handle_close(m->attached);
        message_drop(m);
        m = next;
    }
}

bool message_channel_is(struct handle *h) { return handle_is(h, &message_ops); }

/* A message of n bytes, charged to app (NULL: the kernel's). NULL when the app's messages would go
   beyond their share, or without memory. */
static struct message *message_new(size_t n, uint64_t sender_app, struct process *app) {
    if (n > SIZE_MAX - sizeof(struct message)) return NULL;
    if (app) {
        uint64_t share = pmm_free_pages() * PAGE_SIZE / 2;
        uint64_t had = __atomic_fetch_add(&app->messages, n, __ATOMIC_RELAXED);
        if (had + n > share || had + n < had) {
            __atomic_sub_fetch(&app->messages, n, __ATOMIC_RELAXED);
            return NULL;
        }
    }
    struct message *m = kmalloc(sizeof *m + n);
    if (!m) {
        if (app) __atomic_sub_fetch(&app->messages, n, __ATOMIC_RELAXED);
        return NULL;
    }
    m->next = NULL;
    m->size = n;
    m->attached = NULL;
    m->sender_app = sender_app;
    m->charged = app;
    m->carrier = NULL;
    if (app) __atomic_add_fetch(&app->refs, 1, __ATOMIC_RELAXED); /* the sender is running: refs > 0 */
    return m;
}

/* The lock held, queue i not full or not open: m joins it (taking a reference to attach), or is
   freed if end i has closed. Gives the lock back, with flags, and wakes one of queue i's receivers,
   and, for a sender that waited (woken for the room it used), the next sender while room is left. */
static int64_t message_put(struct message_link *l, int i, struct message *m, struct handle *attach, uint64_t flags,
                           bool waited) {
    if (!l->open[i]) {
        spin_unlock(&l->lock, flags);
        message_drop(m);
        return -N_ERR_CLOSED;
    }
    if (attach && !transit_begin(l, i, m, attach)) {
        spin_unlock(&l->lock, flags);
        message_drop(m);
        return -N_ERR_BUSY;
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
        if (e) { spin_unlock(&l->lock, fl); message_drop(m); return e; }
        wq_wait_locked_exclusive_until(&l->send_wait[peer], &l->lock, deadline);
        spin_take(&l->lock);
    }
    return message_put(l, peer, m, attach, fl, waited);
}

int64_t message_channel_send(struct handle *h, const void *udata, size_t n, struct handle *attach, uint64_t deadline) {
    struct process *app = process_current();
    struct message *m = message_new(n, (uint64_t)app->pid, app);
    if (!m) return n > SIZE_MAX - sizeof(struct message) ? -N_ERR_TOO_LARGE : -N_ERR_NO_MEMORY;
    if (copy_from_user(m->bytes, udata, n) < 0) { message_drop(m); return -N_ERR_BAD_ADDRESS; }
    return message_queue(h, m, attach, deadline);
}

int64_t message_channel_request(struct handle *h, const void *head, size_t head_n, const void *utail, size_t tail_n,
                                struct handle *attach, uint64_t deadline) {
    struct process *app = process_current();
    if (tail_n > SIZE_MAX - head_n) return -N_ERR_TOO_LARGE;
    struct message *m = message_new(head_n + tail_n, (uint64_t)app->pid, app);
    if (!m) return head_n + tail_n > SIZE_MAX - sizeof(struct message) ? -N_ERR_TOO_LARGE : -N_ERR_NO_MEMORY;
    memcpy(m->bytes, head, head_n);
    if (copy_from_user(m->bytes + head_n, utail, tail_n) < 0) { message_drop(m); return -N_ERR_BAD_ADDRESS; }
    return message_queue(h, m, attach, deadline);
}

int64_t message_channel_post(struct handle *h, const void *data, size_t n, uint64_t sender_app) {
    struct message *m = message_new(n, sender_app, NULL);
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
            transit_end(m);
            *attached = m->attached;
            m->attached = NULL;
            message_drop(m);
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
        l->who[i] = identity_kernel;
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

void message_channel_bind(struct handle *h, const struct n_identity *who) {
    struct message_link *l = message_link(h);
    uint64_t fl = spin_lock(&l->lock);
    l->who[message_side(h)] = *who;
    spin_unlock(&l->lock, fl);
}

void message_channel_handed(struct handle *h, const struct n_identity *who) {
    struct message_link *l = message_link(h);
    int side = message_side(h);
    uint64_t fl = spin_lock(&l->lock);
    if (identity_covers(&l->who[side], who)) l->who[side] = *who;
    spin_unlock(&l->lock, fl);
}

void message_channel_peer(struct handle *h, struct n_identity *out) {
    struct message_link *l = message_link(h);
    uint64_t fl = spin_lock(&l->lock);
    *out = l->who[!message_side(h)];
    spin_unlock(&l->lock, fl);
}

bool message_channel_peer_open(struct handle *h) {
    struct message_link *l = message_link(h);
    uint64_t fl = spin_lock(&l->lock);
    bool open = l->open[!message_side(h)];
    spin_unlock(&l->lock, fl);
    return open;
}
