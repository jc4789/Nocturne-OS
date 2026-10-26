/* Event queues (docs/nocturne-native-abi.md §6): one wait for many handles,
   timers and memory words. Reports are level-triggered: a handle that stays
   ready is reported by every wait. */
#include "kernel.h"
#include "arch/cpu.h"
#include "arch/smp.h"
#include "mm/vmm.h"
#include "sys/sched.h"
#include "sys/syscall.h"
#include "sys/object.h"
#include "sys/events.h"
#include "mm/heap.h"
#include "dev/timer.h"

/* A watch names a handle without holding it: the handle's last close ends the
   watch, so watching never keeps a channel end or an app record open. */
struct event_watch {
    struct event_watch *queue_next, **queue_prev;
    struct event_watch *handle_next;
    struct handle *handle;
    uint32_t interest;
    uint64_t token;
    struct object_waiter waiter; /* on the handle's object, for the queue's wait */
};
struct event_timer {
    struct event_timer *next;
    uint64_t due, period, token;
};
struct event_queue {
    struct event_watch *watches;
    struct event_timer *timers;
    struct object *addresses; /* made at the first memory-word watch */
    /* No timer is due before this: the earliest due time, or earlier once timers are removed.
       An AP wait (events_wait_ap) leaves a queue with a due timer to the BSP. */
    uint64_t next_due;
    /* Every wait on the queue, BSP or AP: each watch, the queue itself (a watch or timer added,
       its handle taken) and its memory words notify it for as long as they exist. */
    struct object_wait wait;
    struct object_waiter self, on_addresses;
    struct object object;
};

/* BSP IRQ guard held for all list changes below. An AP wait may be reading the queue's list
   meanwhile: a watch is published whole, and freed only after ap_read_synchronize. */
static void watch_unlink(struct event_watch *w) {
    object_waiter_unlink(&w->waiter);
    __atomic_store_n(w->queue_prev, w->queue_next, __ATOMIC_RELEASE);
    if (w->queue_next) w->queue_next->queue_prev = w->queue_prev;
    struct event_watch **at = &w->handle->watchers;
    while (*at && *at != w) at = &(*at)->handle_next;
    if (*at) *at = w->handle_next;
}

/* Ended watches are chained by handle_next: an AP wait standing on one still follows its
   queue_next, which stays as it was. */
struct event_watch *events_handle_closed(struct handle *h) {
    struct event_watch *ended = NULL;
    while (h->watchers) {
        struct event_watch *w = h->watchers;
        watch_unlink(w);
        w->handle_next = ended;
        ended = w;
    }
    return ended;
}

void events_watches_free(struct event_watch *ended) {
    while (ended) {
        struct event_watch *next = ended->handle_next;
        kfree(ended);
        ended = next;
    }
}

/* Every handle of the queue was closed (and synchronized with AP readers) before its last
   reference went: no AP wait reads these watches any more. */
static void queue_release(struct object *o) {
    struct event_queue *q = o->priv;
    uint64_t flags = irq_save();
    while (q->watches) {
        struct event_watch *w = q->watches;
        watch_unlink(w);
        kfree(w);
    }
    object_waiter_unlink(&q->self);
    if (q->addresses) object_waiter_unlink(&q->on_addresses);
    struct event_timer *timers = q->timers;
    q->timers = NULL;
    irq_restore(flags);
    while (timers) {
        struct event_timer *next = timers->next;
        kfree(timers);
        timers = next;
    }
    object_unref(q->addresses);
    /* A sibling that parked on the queue's word after the last notification (events_wait_ap,
       inside the read section this release waited out) still names its address: it wakes,
       finds its handle gone and ends at the BSP. */
    address_park_wake_word(&q->wait.generation);
    kfree(q);
}

static const struct object_ops queue_ops = {.kind = N_KIND_EVENTS, .release = queue_release};

int events_create(struct handle **out) {
    struct event_queue *q = kzalloc(sizeof *q);
    if (!q) return -N_ERR_NO_MEMORY;
    q->next_due = UINT64_MAX;
    object_init(&q->object, &queue_ops, q);
    object_waiter_link(&q->self, &q->wait, &q->object);
    *out = handle_open(&q->object, true);
    if (!*out) { kfree(q); return -N_ERR_NO_MEMORY; }
    return 0;
}

bool events_is(struct handle *h) { return handle_is(h, &queue_ops); }

static struct event_queue *queue_of(struct handle *h) { return h->object->priv; }

size_t events_watch_size(void) { return sizeof(struct event_watch); }

/* Watch h (looked up under the caller's guard, which is still held). A second
   watch of the same handle with the same token changes its interest. */
int events_watch_locked(struct handle *queue, struct handle *h, uint32_t interest, uint64_t token,
                        struct event_watch **spare) {
    struct event_queue *q = queue_of(queue);
    if (events_is(h)) return -N_ERR_WRONG_KIND; /* queues do not nest */
    for (struct event_watch *w = q->watches; w; w = w->queue_next)
        if (w->handle == h && w->token == token) {
            w->interest = interest;
            object_notify(&q->object);
            return 0;
        }
    struct event_watch *w = *spare;
    *spare = NULL;
    *w = (struct event_watch){.queue_next = q->watches, .queue_prev = &q->watches,
                              .handle_next = h->watchers, .handle = h, .interest = interest, .token = token};
    if (q->watches) q->watches->queue_prev = &w->queue_next;
    __atomic_store_n(&q->watches, w, __ATOMIC_RELEASE);
    h->watchers = w;
    object_waiter_link(&w->waiter, &q->wait, h->object);
    object_notify(&q->object); /* a waiting sibling thread rescans */
    return 0;
}

int events_watch_address(struct handle *queue, volatile uint32_t *address, uint32_t expected, uint64_t token) {
    struct event_queue *q = queue_of(queue);
    if (!q->addresses) {
        struct object *a = address_watch_queue_new();
        if (!a) return -N_ERR_NO_MEMORY;
        object_ref(a);
        uint64_t flags = irq_save();
        if (!q->addresses) {
            object_waiter_link(&q->on_addresses, &q->wait, a);
            __atomic_store_n(&q->addresses, a, __ATOMIC_RELEASE);
            a = NULL;
        }
        irq_restore(flags);
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
    t->due = uptime_ms() + ms;
    t->period = repeat ? ms : 0;
    t->token = token;
    uint64_t flags = irq_save();
    t->next = q->timers;
    q->timers = t;
    if (t->due < q->next_due) __atomic_store_n(&q->next_due, t->due, __ATOMIC_RELEASE);
    object_notify(&q->object);
    irq_restore(flags);
    return 0;
}

/* 0 nothing had the token; 1 removed before its event; 2 its memory-word
   event had already been queued (and is now withdrawn). */
int events_unwatch(struct handle *queue, uint64_t token) {
    struct event_queue *q = queue_of(queue);
    int result = 0;
    struct event_timer *dropped = NULL;
    struct event_watch *ended = NULL;
    uint64_t flags = irq_save();
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
    irq_restore(flags);
    if (ended) {
        ap_read_synchronize();
        events_watches_free(ended);
    }
    while (dropped) { struct event_timer *next = dropped->next; kfree(dropped); dropped = next; }
    if (q->addresses) {
        int a = address_watch_remove(q->addresses, token);
        if (a > result) result = a;
    }
    return result;
}

#define EVENTS_BATCH 64

static int64_t wait_timeout(int64_t timeout_ms) {
    uint64_t until = current_task->wait_until;
    if (!until) return timeout_ms;
    if (until == UINT64_MAX) return -1;
    uint64_t now = uptime_ms();
    return until > now ? (int64_t)(until - now) : 0;
}

int64_t events_wait_resume(int64_t timeout_ms) {
    timeout_ms = wait_timeout(timeout_ms);
    current_task->wait_until = 0;
    return timeout_ms;
}

int64_t events_wait(struct handle *queue, struct n_event *uout, size_t max, int64_t timeout_ms) {
    struct event_queue *q = queue_of(queue);
    if (!max) return -N_ERR_INVALID;
    if (max > EVENTS_BATCH) max = EVENTS_BATCH; /* the rest stays for the next wait */
    struct n_event out[EVENTS_BATCH];
    uint64_t tokens[EVENTS_BATCH];
    uint64_t deadline = timeout_ms > 0 ? uptime_ms() + (uint64_t)timeout_ms : 0;
    bool block = timeout_ms != 0;
    int64_t result;
    for (;;) {
        uint64_t fl = irq_save();
        uint64_t now = uptime_ms(), next_due = UINT64_MAX;
        size_t n = 0;
        for (struct event_timer **at = &q->timers; *at;) {
            struct event_timer *t = *at;
            if (t->due <= now && n < max) {
                out[n++] = (struct n_event){.token = t->token, .events = N_EVENT_TIMER};
                if (!t->period) { *at = t->next; kfree(t); continue; }
                /* keep the cadence; a late wait does not report the missed ticks */
                t->due += t->period;
                if (t->due <= now) t->due = now + t->period;
            }
            if (t->due < next_due) next_due = t->due;
            at = &t->next;
        }
        __atomic_store_n(&q->next_due, next_due, __ATOMIC_RELEASE);
        /* A memory-word event is reported once; handles stay ready and are
           reported again, so many ready handles never hold one back. */
        if (q->addresses && n < max) {
            size_t got = address_watch_take(q->addresses, tokens, max - n);
            for (size_t i = 0; i < got; i++) out[n++] = (struct n_event){.token = tokens[i], .events = N_EVENT_ADDRESS};
        }
        for (struct event_watch *w = q->watches; w && n < max; w = w->queue_next) {
            uint32_t ready = handle_ready(w->handle) & w->interest;
            if (ready) out[n++] = (struct n_event){.token = w->token, .events = ready};
        }
        bool expired = timeout_ms > 0 && now >= deadline;
        if (n || !block || expired) {
            result = (int64_t)n;
            if (n) {
                if (!user_ok_w(uout, n * sizeof *uout)) result = -N_ERR_BAD_ADDRESS;
                else memcpy(uout, out, n * sizeof *uout);
            }
            irq_restore(fl);
            break;
        }
        if (current_task->killed) { irq_restore(fl); result = -N_ERR_STOPPED; break; }
        /* Changes come from the BSP under its guard, held from the scan to the sleep: none is
           missed in between. */
        uint64_t until = timeout_ms > 0 ? deadline : UINT64_MAX;
        if (next_due < until) until = next_due;
        if (until == UINT64_MAX) wq_wait(&q->wait.wq);
        else if (until > now) wq_wait_timeout(&q->wait.wq, until - now);
        irq_restore(fl);
    }
    return result;
}

enum ap_syscall events_wait_ap(struct handle *queue, struct n_event *uout, size_t max, int64_t timeout_ms,
                               int64_t *result) {
    if (!events_is(queue) || !max) return AP_SYSCALL_BSP;
    struct event_queue *q = queue_of(queue);
    struct task *t = current_task;
    if (max > EVENTS_BATCH) max = EVENTS_BATCH;
    timeout_ms = wait_timeout(timeout_ms);
    uint64_t now = uptime_ms(), until = timeout_ms > 0 ? now + (uint64_t)timeout_ms : UINT64_MAX;
    /* A change during a scan sends it round again rather than to the BSP, a few times. */
    for (unsigned tries = 0; tries < 4; tries++) {
        /* Read before the scan: whatever changes after it moves the word on, and the park
           below then sees that. */
        uint64_t seen = __atomic_load_n(&q->wait.generation, __ATOMIC_ACQUIRE);
        /* Timers and memory-word events change the queue when reported: the BSP's. */
        uint64_t due = __atomic_load_n(&q->next_due, __ATOMIC_ACQUIRE);
        if (due <= now) return AP_SYSCALL_BSP;
        struct object *addresses = __atomic_load_n(&q->addresses, __ATOMIC_ACQUIRE);
        if (addresses && address_watch_pending(addresses)) return AP_SYSCALL_BSP;
        struct n_event out[EVENTS_BATCH];
        size_t n = 0;
        for (struct event_watch *w = __atomic_load_n(&q->watches, __ATOMIC_ACQUIRE); w && n < max;
             w = __atomic_load_n(&w->queue_next, __ATOMIC_ACQUIRE)) {
            uint32_t ready = handle_ready(w->handle) & __atomic_load_n(&w->interest, __ATOMIC_RELAXED);
            if (ready) out[n++] = (struct n_event){.token = w->token, .events = ready};
        }
        if (n || !timeout_ms) {
            if (n) {
                if (!vmm_ap_user_begin(uout, n * sizeof *uout, true)) return AP_SYSCALL_BSP;
                memcpy(uout, out, n * sizeof *uout);
                vmm_ap_uaccess_end();
            }
            t->wait_until = 0;
            *result = (int64_t)n;
            return AP_SYSCALL_DONE;
        }
        t->wait_until = until;
        if (address_park_word(&q->wait.generation, seen, due < until ? due : until)) return AP_SYSCALL_PARK;
        now = uptime_ms();
    }
    return AP_SYSCALL_BSP;
}
