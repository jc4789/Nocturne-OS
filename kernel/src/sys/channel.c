/* Native channels: named rendezvous and message channel pairs (sys/channel.h). */
#include "kernel.h"
#include "arch/cpu.h"
#include "mm/heap.h"
#include "sys/syscall.h"
#include "sys/object.h"
#include "sys/channel.h"

#define COPY_PIECE 16384 /* bounded user-copy granularity, not a capacity ceiling */

/* Named native rendezvous: a registry of listeners. Each connection is an
 * ordinary message channel pair; the server end waits in the listener until
 * accepted. No path lookup, network socket family or fixed client capacity. */
struct channel_pending { struct channel_pending *next; struct handle *server; };
struct channel_listener {
    struct channel_listener *next;
    struct channel_pending *head, *tail;
    uint64_t hash;
    size_t key_length;
    bool published;
    struct object object;
    char key[];
};
enum { CHANNEL_BUCKETS = 64 };
static struct channel_listener *channel_names[CHANNEL_BUCKETS];

static uint64_t channel_hash(const char *key, size_t length) {
    uint64_t h = 14695981039346656037ull;
    for (size_t i = 0; i < length; i++) h = (h ^ (unsigned char)key[i]) * 1099511628211ull;
    return h;
}

static struct channel_listener *channel_find(const char *key, size_t length, uint64_t hash) {
    for (struct channel_listener *l = channel_names[hash & (CHANNEL_BUCKETS - 1)]; l; l = l->next)
        if (l->hash == hash && l->key_length == length && !memcmp(l->key, key, length)) return l;
    return NULL;
}

static uint32_t listener_ready(struct handle *h) {
    struct channel_listener *l = h->object->priv;
    return l->head ? N_EVENT_RECEIVABLE : 0;
}

static void listener_close(struct handle *h) {
    struct channel_listener *l = h->object->priv;
    uint64_t flags = irq_save();
    if (l->published) {
        struct channel_listener **at = &channel_names[l->hash & (CHANNEL_BUCKETS - 1)];
        while (*at && *at != l) at = &(*at)->next;
        if (*at == l) *at = l->next;
        l->published = false;
    }
    struct channel_pending *pending = l->head;
    l->head = l->tail = NULL;
    object_notify(&l->object);
    irq_restore(flags);
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
    uint64_t hash = channel_hash(key, length);
    struct channel_listener *l = kzalloc(sizeof *l + length);
    if (!l) return -N_ERR_NO_MEMORY;
    l->hash = hash;
    l->key_length = length;
    memcpy(l->key, key, length);
    object_init(&l->object, &listener_ops, l);
    struct handle *h = handle_open(&l->object, false);
    if (!h) { kfree(l); return -N_ERR_NO_MEMORY; }
    uint64_t flags = irq_save();
    if (channel_find(key, length, hash)) { irq_restore(flags); handle_close(h); return -N_ERR_EXISTS; }
    unsigned bucket = hash & (CHANNEL_BUCKETS - 1);
    l->next = channel_names[bucket];
    channel_names[bucket] = l;
    l->published = true;
    irq_restore(flags);
    *listener = h;
    return 0;
}

int channel_connect(const char *key, size_t length, struct handle **client) {
    if (!length) return -N_ERR_INVALID;
    uint64_t hash = channel_hash(key, length);
    uint64_t flags = irq_save();
    struct channel_listener *l = channel_find(key, length, hash);
    if (!l) { irq_restore(flags); return -N_ERR_NOT_FOUND; }
    struct channel_pending *pending = kmalloc(sizeof *pending);
    if (!pending) { irq_restore(flags); return -N_ERR_NO_MEMORY; }
    struct handle *server;
    int e = message_channel_pair(client, &server);
    if (e) { kfree(pending); irq_restore(flags); return e; }
    pending->next = NULL;
    pending->server = server;
    if (l->tail) l->tail->next = pending;
    else l->head = pending;
    l->tail = pending;
    object_notify(&l->object);
    irq_restore(flags);
    return 0;
}

int channel_accept(struct handle *listener, struct handle **server) {
    if (!handle_is(listener, &listener_ops)) return -N_ERR_WRONG_KIND;
    struct channel_listener *l = listener->object->priv;
    uint64_t flags = irq_save();
    struct channel_pending *pending = l->head;
    if (!pending) { irq_restore(flags); return -N_ERR_NOT_READY; }
    l->head = pending->next;
    if (!l->head) l->tail = NULL;
    *server = pending->server;
    kfree(pending);
    irq_restore(flags);
    return 0;
}

/* Message channels. A pair shares one link; each end receives from its own
 * queue and sends into the peer's. A message keeps its boundary and may carry
 * one document or shared-page handle. Messages are kept in kernel memory as
 * sent. A sender waits while the receiver has MESSAGE_QUEUE_LIMIT bytes or
 * more not yet taken; a message into an empty queue is always accepted,
 * whatever its size. */
#define MESSAGE_QUEUE_LIMIT (1u << 20)
struct message {
    struct message *next;
    struct handle *attached;
    size_t size;
    uint64_t sender_app;
    uint8_t bytes[];
};
struct message_link {
    struct message *head[2], **tail[2];
    size_t queued[2];               /* bytes waiting in queue i */
    bool open[2];                   /* end i still has its handle */
    int objects;                    /* ends not yet released: the last frees the link */
    struct wait_queue wait[2];      /* receivers of queue i */
    struct wait_queue send_wait[2]; /* senders into queue i */
    struct object end[2];
};
/* Full while MESSAGE_QUEUE_LIMIT bytes or more wait, whatever the next
 * message's size: N_EVENT_SENDABLE then means a send will not wait. */
static bool message_queue_full(const struct message_link *l, int side) {
    return l->queued[side] >= MESSAGE_QUEUE_LIMIT;
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

/* Queues m for the peer of h, or frees it. With wait, a full queue is waited
 * out; without, it is N_ERR_NOT_READY. */
static int64_t message_queue(struct handle *h, struct message *m, struct handle *attach, bool wait) {
    struct message_link *l = message_link(h);
    int peer = !message_side(h);
    uint64_t fl = irq_save();
    while (l->open[peer] && message_queue_full(l, peer)) {
        if (!wait) { irq_restore(fl); kfree(m); return -N_ERR_NOT_READY; }
        if (current_task->killed) { irq_restore(fl); kfree(m); return -N_ERR_STOPPED; }
        wq_wait(&l->send_wait[peer]);
    }
    if (!l->open[peer]) { irq_restore(fl); kfree(m); return -N_ERR_CLOSED; }
    size_t n = m->size;
    if (attach) m->attached = handle_ref(attach);
    *l->tail[peer] = m;
    l->tail[peer] = &m->next;
    l->queued[peer] += n;
    wq_wake_all(&l->wait[peer]);
    object_notify(&l->end[peer]);
    irq_restore(fl);
    return (int64_t)n;
}

int64_t message_channel_send(struct handle *h, const void *udata, size_t n, struct handle *attach) {
    struct message *m = message_new(n, (uint64_t)task_process()->pid);
    if (!m) return n > SIZE_MAX - sizeof(struct message) ? -N_ERR_TOO_LARGE : -N_ERR_NO_MEMORY;
    /* Copy in fixed pieces: a large message never makes one long
       non-preemptible copy, and a sibling unmapping the source is caught. */
    for (size_t done = 0; done < n;) {
        size_t piece = MIN(n - done, (size_t)COPY_PIECE);
        uint64_t fl = irq_save();
        bool ok = user_ok((const uint8_t *)udata + done, piece);
        if (ok) memcpy(m->bytes + done, (const uint8_t *)udata + done, piece);
        irq_restore(fl);
        if (!ok) { kfree(m); return -N_ERR_BAD_ADDRESS; }
        done += piece;
    }
    return message_queue(h, m, attach, true);
}

int64_t message_channel_post(struct handle *h, const void *data, size_t n, uint64_t sender_app) {
    struct message *m = message_new(n, sender_app);
    if (!m) return -N_ERR_NO_MEMORY;
    memcpy(m->bytes, data, n);
    return message_queue(h, m, NULL, false);
}

/* peek reports the next message's size without taking it. A message larger
 * than cap stays queued and reports N_ERR_TOO_LARGE. discard takes the next
 * message off the queue whatever its size, copies only its first cap bytes,
 * closes its attachment and reports its whole size. */
int64_t message_channel_receive(struct handle *h, void *ubuf, size_t cap, bool wait, bool peek,
                                bool discard, struct handle **attached, uint64_t *sender_app) {
    struct message_link *l = message_link(h);
    int side = message_side(h);
    *attached = NULL;
    for (;;) {
        uint64_t fl = irq_save();
        struct message *m = l->head[side];
        if (m) {
            if (peek) {
                if (sender_app) *sender_app = m->sender_app;
                irq_restore(fl);
                return (int64_t)m->size;
            }
            if (!discard && m->size > cap) { irq_restore(fl); return -N_ERR_TOO_LARGE; }
            size_t copy = MIN(m->size, cap);
            if (!user_ok_w(ubuf, copy)) { irq_restore(fl); return -N_ERR_BAD_ADDRESS; }
            l->head[side] = m->next;
            if (!l->head[side]) l->tail[side] = &l->head[side];
            m->next = NULL;
            l->queued[side] -= m->size;
            wq_wake_all(&l->send_wait[side]);
            object_notify(&l->end[!side]); /* the sender may send again */
            irq_restore(fl);
            /* Taken off the queue: copy out in pieces without holding the
               guard. Only a sibling unmapping the buffer meanwhile loses it. */
            for (size_t done = 0; done < copy;) {
                size_t piece = MIN(copy - done, (size_t)COPY_PIECE);
                uint64_t cf = irq_save();
                bool ok = user_ok_w((uint8_t *)ubuf + done, piece);
                if (ok) memcpy((uint8_t *)ubuf + done, m->bytes + done, piece);
                irq_restore(cf);
                if (!ok) { message_free(m); return -N_ERR_BAD_ADDRESS; }
                done += piece;
            }
            int64_t size = (int64_t)m->size;
            if (sender_app) *sender_app = m->sender_app;
            if (discard) { message_free(m); return size; }
            *attached = m->attached;
            m->attached = NULL;
            kfree(m);
            return size;
        }
        if (!l->open[!side]) { irq_restore(fl); return -N_ERR_CLOSED; }
        if (!wait) { irq_restore(fl); return -N_ERR_NOT_READY; }
        if (current_task->killed) { irq_restore(fl); return -N_ERR_STOPPED; }
        wq_wait(&l->wait[side]);
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
    uint64_t fl = irq_save();
    l->open[side] = false;
    /* Nobody can receive this side's queue any more: drop it now, so an
     * attached handle is not kept alive by a dead end. */
    struct message *dropped = l->head[side];
    l->head[side] = NULL;
    l->tail[side] = &l->head[side];
    l->queued[side] = 0;
    wq_wake_all(&l->wait[!side]);
    wq_wake_all(&l->wait[side]);
    wq_wake_all(&l->send_wait[side]); /* senders into this end now get N_ERR_CLOSED */
    object_notify(&l->end[!side]);
    irq_restore(fl);
    message_free(dropped);
}

static void message_release(struct object *o) {
    struct message_link *l = o->priv;
    uint64_t fl = irq_save();
    bool last = --l->objects == 0;
    irq_restore(fl);
    if (!last) return;
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
        l->open[i] = true;
        object_init(&l->end[i], &message_ops, l);
    }
    l->objects = 2;
    *a = handle_open(&l->end[0], true);
    *b = *a ? handle_open(&l->end[1], true) : NULL;
    if (!*b) {
        /* An opened end is released through its handle; the other by hand. */
        if (*a) { handle_close(*a); *a = NULL; message_release(&l->end[1]); }
        else kfree(l);
        return -N_ERR_NO_MEMORY;
    }
    return 0;
}
