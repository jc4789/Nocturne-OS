/* Event queues (docs/nocturne-native-abi.md §6): one wait for many handles,
   timers and memory words. Reports are level-triggered: a handle that stays
   ready is reported by every wait. */
#include "kernel.h"
#include "arch/cpu.h"
#include "sys/sched.h"
#include "sys/syscall.h"
#include "sys/object.h"
#include "sys/events.h"
#include "mm/heap.h"
#include "dev/timer.h"

/* A watch names a handle without holding it: the handle's last close ends the
   watch, so watching never keeps a channel end or an app record open. */
struct event_queue;
struct event_watch {
    struct event_watch *queue_next, **queue_prev;
    struct event_watch *handle_next;
    struct event_queue *queue;
    struct handle *handle;
    uint32_t interest;
    uint64_t token;
    struct object_waiter waiter; /* on the handle's object, for the queue's wait */
};
struct event_timer {
    struct event_timer *next;
    uint64_t due, period, token;
};
/* Every handle's list of watches (struct handle's watchers, chained by handle_next). Taken
   before a queue's lock. */
static struct spinlock watchers_lock;

struct event_queue {
    /* Its watches and timers, which a wait on any CPU reads and changes (timers that fire). A
       watch's handle stays allocated while its watch is on the list: its last close takes the
       watch off under this lock first. Taken before an object's lock and address_lock. */
    struct spinlock lock;
    struct event_watch *watches;
    struct event_timer *timers;
    struct object *addresses; /* made at the first memory-word watch */
    /* Every wait on the queue: each watch, the queue itself (a watch or timer added, its handle
       taken) and its memory words notify it for as long as they exist. */
    struct object_wait wait;
    struct object_waiter self, on_addresses;
    struct object object;
};

/* watchers_lock held, and the watch's queue's lock. */
static void watch_unlink(struct event_watch *w) {
    object_waiter_unlink(&w->waiter);
    *w->queue_prev = w->queue_next;
    if (w->queue_next) w->queue_next->queue_prev = w->queue_prev;
    struct event_watch **at = &w->handle->watchers;
    while (*at && *at != w) at = &(*at)->handle_next;
    if (*at) *at = w->handle_next;
}

/* Ended watches are chained by handle_next. */
struct event_watch *events_handle_closed(struct handle *h) {
    struct event_watch *ended = NULL;
    uint64_t flags = spin_lock(&watchers_lock);
    while (h->watchers) {
        struct event_watch *w = h->watchers;
        struct event_queue *q = w->queue;
        spin_take(&q->lock);
        watch_unlink(w);
        spin_give(&q->lock);
        w->handle_next = ended;
        ended = w;
    }
    spin_unlock(&watchers_lock, flags);
    return ended;
}

void events_watches_free(struct event_watch *ended) {
    while (ended) {
        struct event_watch *next = ended->handle_next;
        kfree(ended);
        ended = next;
    }
}

/* Every handle of the queue was closed before its last reference went: no wait reads it any
   more. */
static void queue_release(struct object *o) {
    struct event_queue *q = o->priv;
    uint64_t flags = spin_lock(&watchers_lock);
    spin_take(&q->lock);
    struct event_watch *ended = NULL;
    while (q->watches) {
        struct event_watch *w = q->watches;
        watch_unlink(w);
        w->handle_next = ended;
        ended = w;
    }
    struct event_timer *timers = q->timers;
    q->timers = NULL;
    spin_give(&q->lock);
    spin_unlock(&watchers_lock, flags);
    events_watches_free(ended);
    object_waiter_unlink(&q->self);
    if (q->addresses) object_waiter_unlink(&q->on_addresses);
    while (timers) {
        struct event_timer *next = timers->next;
        kfree(timers);
        timers = next;
    }
    object_unref(q->addresses);
    kfree(q);
}

static const struct object_ops queue_ops = {.kind = N_KIND_EVENTS, .release = queue_release};

int events_create(struct handle **out) {
    struct event_queue *q = kzalloc(sizeof *q);
    if (!q) return -N_ERR_NO_MEMORY;
    object_init(&q->object, &queue_ops, q);
    object_waiter_link(&q->self, &q->wait, &q->object);
    *out = handle_open(&q->object, true);
    if (!*out) { kfree(q); return -N_ERR_NO_MEMORY; }
    return 0;
}

bool events_is(struct handle *h) { return handle_is(h, &queue_ops); }

static struct event_queue *queue_of(struct handle *h) { return h->object->priv; }

size_t events_watch_size(void) { return sizeof(struct event_watch); }

/* A second watch of the same handle with the same token changes its interest. The caller's
   reference keeps h from its last close until the watch is on its list. */
int events_watch(struct handle *queue, struct handle *h, uint32_t interest, uint64_t token,
                 struct event_watch **spare) {
    struct event_queue *q = queue_of(queue);
    if (events_is(h)) return -N_ERR_WRONG_KIND; /* queues do not nest */
    uint64_t flags = spin_lock(&watchers_lock);
    spin_take(&q->lock);
    struct event_watch *w = q->watches;
    while (w && (w->handle != h || w->token != token)) w = w->queue_next;
    if (w) {
        w->interest = interest;
    } else {
        w = *spare;
        *spare = NULL;
        *w = (struct event_watch){.queue_next = q->watches, .queue_prev = &q->watches, .handle_next = h->watchers,
                                  .queue = q, .handle = h, .interest = interest, .token = token};
        if (q->watches) q->watches->queue_prev = &w->queue_next;
        q->watches = w;
        h->watchers = w;
        object_waiter_link(&w->waiter, &q->wait, h->object);
    }
    spin_give(&q->lock);
    spin_unlock(&watchers_lock, flags);
    object_notify(&q->object); /* a waiting sibling thread rescans */
    return 0;
}

int events_watch_address(struct handle *queue, volatile uint32_t *address, uint32_t expected, uint64_t token) {
    struct event_queue *q = queue_of(queue);
    if (!q->addresses) {
        struct object *a = address_watch_queue_new();
        if (!a) return -N_ERR_NO_MEMORY;
        object_ref(a);
        uint64_t flags = spin_lock(&q->lock);
        if (!q->addresses) {
            object_waiter_link(&q->on_addresses, &q->wait, a);
            __atomic_store_n(&q->addresses, a, __ATOMIC_RELEASE);
            a = NULL;
        }
        spin_unlock(&q->lock, flags);
        object_unref(a);
    }
    int r = address_watch_add(q->addresses, address, expected, token);
    if (!r) object_notify(&q->object);
    return r;
}

int events_timer(struct handle *queue, uint64_t ms, bool repeat, uint64_t token) {
    struct event_queue *q = queue_of(queue);
    if (repeat && !ms) return -N_ERR_INVALID;
    struct event_timer *t = kmalloc(sizeof *t);
    if (!t) return -N_ERR_NO_MEMORY;
    t->due = timer_now_ms() + ms;
    t->period = repeat ? ms : 0;
    t->token = token;
    uint64_t flags = spin_lock(&q->lock);
    t->next = q->timers;
    q->timers = t;
    spin_unlock(&q->lock, flags);
    object_notify(&q->object);
    return 0;
}

/* 0 nothing had the token; 1 removed before its event; 2 its memory-word
   event had already been queued (and is now withdrawn). */
int events_unwatch(struct handle *queue, uint64_t token) {
    struct event_queue *q = queue_of(queue);
    int result = 0;
    struct event_timer *dropped = NULL;
    struct event_watch *ended = NULL;
    uint64_t flags = spin_lock(&watchers_lock);
    spin_take(&q->lock);
    for (struct event_watch *w = q->watches, *next; w; w = next) {
        next = w->queue_next;
        if (w->token != token) continue;
        watch_unlink(w);
        w->handle_next = ended;
        ended = w;
        result = 1;
    }
    for (struct event_timer **at = &q->timers; *at;) {
        if ((*at)->token != token) { at = &(*at)->next; continue; }
        struct event_timer *t = *at;
        *at = t->next;
        t->next = dropped;
        dropped = t;
        result = 1;
    }
    spin_give(&q->lock);
    spin_unlock(&watchers_lock, flags);
    events_watches_free(ended);
    while (dropped) { struct event_timer *next = dropped->next; kfree(dropped); dropped = next; }
    if (q->addresses) {
        int a = address_watch_remove(q->addresses, token);
        if (a > result) result = a;
    }
    return result;
}

#define EVENTS_BATCH 64

int64_t events_wait(struct handle *queue, struct n_event *uout, size_t max, int64_t timeout_ms) {
    struct event_queue *q = queue_of(queue);
    if (!max) return -N_ERR_INVALID;
    if (max > EVENTS_BATCH) max = EVENTS_BATCH; /* the rest stays for the next wait */
    struct n_event out[EVENTS_BATCH];
    uint64_t tokens[EVENTS_BATCH];
    uint64_t deadline = timeout_ms > 0 ? timer_now_ms() + (uint64_t)timeout_ms : 0;
    bool block = timeout_ms != 0;
    for (;;) {
        uint64_t fl = spin_lock(&q->lock);
        /* Read before the scan: a change on any CPU after it moves the word on before it wakes the
           queue, and the sleep below then does not begin. */
        uint64_t seen = __atomic_load_n(&q->wait.generation, __ATOMIC_ACQUIRE);
        uint64_t now = timer_now_ms(), next_due = UINT64_MAX;
        struct event_timer *fired = NULL;
        size_t n = 0;
        for (struct event_timer **at = &q->timers; *at;) {
            struct event_timer *t = *at;
            if (t->due <= now && n < max) {
                out[n++] = (struct n_event){.token = t->token, .events = N_EVENT_TIMER};
                if (!t->period) { *at = t->next; t->next = fired; fired = t; continue; }
                /* keep the cadence; a late wait does not report the missed ticks */
                t->due += t->period;
                if (t->due <= now) t->due = now + t->period;
            }
            if (t->due < next_due) next_due = t->due;
            at = &t->next;
        }
        /* A memory-word event is reported once; handles stay ready and are
           reported again, so many ready handles never hold one back. */
        struct object *addresses = __atomic_load_n(&q->addresses, __ATOMIC_ACQUIRE);
        if (addresses && n < max) {
            size_t got = address_watch_take(addresses, tokens, max - n);
            for (size_t i = 0; i < got; i++) out[n++] = (struct n_event){.token = tokens[i], .events = N_EVENT_ADDRESS};
        }
        for (struct event_watch *w = q->watches; w && n < max; w = w->queue_next) {
            uint32_t ready = handle_ready(w->handle) & w->interest;
            if (ready) out[n++] = (struct n_event){.token = w->token, .events = ready};
        }
        spin_unlock(&q->lock, fl);
        while (fired) { struct event_timer *next = fired->next; kfree(fired); fired = next; }
        bool expired = timeout_ms > 0 && now >= deadline;
        if (n || !block || expired) {
            if (n && copy_to_user(uout, out, n * sizeof *uout) < 0) return -N_ERR_BAD_ADDRESS;
            return (int64_t)n;
        }
        if (current_task->killed) return -N_ERR_STOPPED;
        uint64_t until = timeout_ms > 0 ? deadline : UINT64_MAX;
        if (next_due < until) until = next_due;
        if (until == UINT64_MAX) wq_wait_unless(&q->wait.wq, &q->wait.generation, seen, 0);
        else if (until > now) wq_wait_unless(&q->wait.wq, &q->wait.generation, seen, until - now);
    }
}
