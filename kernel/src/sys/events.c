/* Event queues (docs/nocturne-native-abi.md §6): one wait for many handles, timers and memory
   words. Reports are level-triggered: a handle that stays ready is reported by every wait.

   A queue's sources are its watches of handles, its timers and its watches of memory words, in a
   tree by token (lib/avl.h). A wait looks only at the sources something told. A source waits armed
   where its change is said: a watch as a callback item on its handle's object's waiters
   (sys/object.h), a word's watch among the word's waiters (sys/address_wait.h), a timer as its
   alarm. The change calls the item back (or fires the alarm), which puts the source on the queue's
   pending line and wakes one thread waiting on the queue. A wait takes the pending line: a handle
   found ready is reported and stays pending after what came meanwhile, for the next wait to look
   at again; one found not ready is armed again, then looked at once more (a change between the
   look and the arm told no one); a timer or a word is reported once. A thread woken for a source
   that it leaves on the line (its batch is full) wakes the next.

   Locks, the first taken first:
   - The queue's lock: its sources and their arming, held by a wait as it looks, by watch, unwatch
     and the queue's close. Under it: an object's waiters' lock and a word's bucket's lock (arming;
     no callback runs there), the watched handle's watch_lock, the pending line's lock, the queue's
     object's waiters' lock (a wake), a CPU's alarm lock, and whatever ops->ready takes.
   - told: the pending line, a leaf. A source's callback takes it, called by a waker that holds an
     object's waiters' lock or a bucket's lock, in any context (sys/wait.h's contract), and so does
     a timer's fire; then each wakes one of the queue's threads (the queue's object's waiters,
     whose lock then nests under the waker's).
   - A handle's watch_lock: its list of watches, a leaf. Its last close takes the list whole, then
     each watch's queue's lock alone. */
#include "kernel.h"
#include "arch/cpu.h"
#include "lib/avl.h"
#include "mm/heap.h"
#include "sys/address_wait.h"
#include "sys/alarm.h"
#include "sys/clock.h"
#include "sys/events.h"
#include "sys/object.h"
#include "sys/syscall.h"
#include "sys/thread.h"
#include "sys/wait.h"

enum source_kind { SOURCE_HANDLE, SOURCE_TIMER, SOURCE_ADDRESS };

struct event_queue;

/* A watch names a handle without holding it: the handle's last close ends the watch, so watching
   never keeps a channel end or an app record open. It holds a reference to its queue instead,
   which that close needs to reach it.
   A timer is an alarm of its own (sys/alarm.h) on the moments due, then due + period, due +
   2 period...: the grid of its period, kept however late it is reported. It falls due in its
   alarm's interrupt and waits on the pending line until a wait reports it; a repeating one is set
   again then, for its first moment after that wait on the grid, so missed moments are reported
   once and a timer no one waits for interrupts nothing.
   A word's watch is told once, by the wake that takes it, and reported once. */
struct event_source {
    struct avl_node node;              /* in its queue's sources, while linked */
    struct event_source *next, **prev; /* on its queue's pending line; prev NULL when off it (told) */
    struct event_queue *queue;
    uint64_t token;
    uintptr_t tag;                     /* orders one token's sources: the watched handle, or the source itself */
    uint64_t at;                       /* when it was told last, or fell due (clock_now_ns) */
    enum source_kind kind;
    bool linked;                       /* in its queue's sources (the queue's lock) */
    bool gone;                         /* off its queue: a timer's fire meanwhile tells nothing (told) */
    struct wait_item item;             /* a watch's or a word's, while armed */
    union {
        struct {
            struct handle *handle;
            /* its handle's watches; prev NULL once the handle's last close took it (watch_lock) */
            struct event_source *next, **prev;
            uint32_t interest;
        } watch;
        struct {
            struct alarm alarm;        /* armed while it waits for due */
            uint64_t due, period;      /* due: the next moment on the clock (clock_now_ns); period: ns, 0 once */
        } timer;
    };
};

struct event_queue {
    struct spinlock lock;              /* sources and their arming */
    struct avl_tree sources;           /* by token, then tag */
    struct spinlock told;              /* the pending line */
    struct event_source *pending, **pending_tail;
    struct object object;              /* its waiters: the threads in its wait, exclusive items */
};

/* An event as the kernel has it: what n_event carries, and the moment it happened (monotonic ns),
   which n_event has no field for until the time ABI. */
struct event {
    uint64_t token;
    uint32_t events;
    uint64_t at;
};

#define SOURCE(n) ((struct event_source *)((char *)(n) - offsetof(struct event_source, node)))

static bool source_before(const struct avl_node *a, const struct avl_node *b) {
    const struct event_source *x = SOURCE(a), *y = SOURCE(b);
    return x->token != y->token ? x->token < y->token : x->tag < y->tag;
}
static const struct avl_order by_token = {.before = source_before};

/* The queue's lock held: its source of token and tag, or with tag 0 any of token's. */
static struct event_source *source_find(struct event_queue *q, uint64_t token, uintptr_t tag) {
    for (struct avl_node *n = q->sources.root; n;) {
        struct event_source *s = SOURCE(n);
        if (token != s->token) n = token < s->token ? n->left : n->right;
        else if (!tag || tag == s->tag) return s;
        else n = tag < s->tag ? n->left : n->right;
    }
    return NULL;
}

/* told held. */
static void pending_add(struct event_queue *q, struct event_source *s) {
    s->next = NULL;
    s->prev = q->pending_tail;
    *q->pending_tail = s;
    q->pending_tail = &s->next;
}
static void pending_drop(struct event_queue *q, struct event_source *s) {
    if (!s->prev) return;
    *s->prev = s->next;
    if (s->next) s->next->prev = s->prev;
    else q->pending_tail = s->prev;
    s->prev = NULL;
}

/* Something changed for s; any context, under no lock that comes after a wait queue's but a
   waker's own (sys/wait.h): it joins the pending line unless it is on it, and one of the queue's
   threads is woken. */
static void source_tell(struct event_source *s) {
    struct event_queue *q = s->queue;
    uint64_t now = clock_now_ns();
    uint64_t flags = spin_lock(&q->told);
    bool fresh = !s->prev && !s->gone;
    if (fresh) {
        s->at = now;
        pending_add(q, s);
    }
    spin_unlock(&q->told, flags);
    if (fresh) wq_wake(&q->object.waiters, 1);
}

/* A watch's or a word's item, taken by a wake (sys/wait.h's callback). */
static void source_told(struct wait_item *item) {
    source_tell((struct event_source *)((char *)item - offsetof(struct event_source, item)));
}

/* Its alarm's interrupt: the timer fell due. */
static void timer_fire(struct alarm *a, struct regs *r) {
    (void)r;
    source_tell((struct event_source *)((char *)a - offsetof(struct event_source, timer.alarm)));
}

/* The first moment of t's grid after now (CLOCK_NEVER past the clock's end). t fell due, but now,
   read on another CPU than its alarm's, may be a little behind its due. */
static uint64_t timer_next(const struct event_source *t, uint64_t now) {
    uint64_t periods = (now > t->timer.due ? now - t->timer.due : 0) / t->timer.period + 1;
    if (periods > (CLOCK_NEVER - t->timer.due) / t->timer.period) return CLOCK_NEVER;
    return t->timer.due + periods * t->timer.period;
}

/* The queue's lock held: watch w waits on its handle's object for the next change. */
static void watch_arm(struct event_source *w) {
    struct wait_queue *wq = &w->watch.handle->object->waiters;
    spin_take(&wq->lock);
    wq_add(wq, &w->item);
    spin_give(&wq->lock);
}
/* The queue's lock held: w off its handle's object. False when it was not on it: a change took it
   (its callback has put it on the pending line by now), or it was pending already. */
static bool watch_disarm(struct event_source *w) {
    struct wait_queue *wq = &w->watch.handle->object->waiters;
    spin_take(&wq->lock);
    bool armed = wq_remove(wq, &w->item);
    spin_give(&wq->lock);
    return armed;
}

/* The queue's lock held: s leaves the queue, disarmed and off the pending line, and *result (when
   given) is raised to what events_unwatch reports for it. True when the caller frees it (after
   the lock: sources_free); false for a watch its handle's last close took, which frees it. */
static bool source_unlink(struct event_queue *q, struct event_source *s, int *result) {
    avl_remove(&q->sources, &by_token, &s->node);
    s->linked = false;
    int r = 1;
    if (s->kind == SOURCE_HANDLE) watch_disarm(s);
    else if (s->kind == SOURCE_ADDRESS && !address_watch_disarm(&s->item))
        r = 2; /* a wake took it: its event, on the pending line, is withdrawn */
    spin_take(&q->told);
    pending_drop(q, s);
    s->gone = true;
    spin_give(&q->told);
    if (result && r > *result) *result = r;
    if (s->kind != SOURCE_HANDLE) return true;
    struct handle *h = s->watch.handle; /* its last close has not passed this watch: it lives */
    spin_take(&h->watch_lock);
    bool mine = s->watch.prev != NULL;
    if (mine) {
        *s->watch.prev = s->watch.next;
        if (s->watch.next) s->watch.next->watch.prev = s->watch.prev;
    }
    spin_give(&h->watch_lock);
    return mine;
}

/* Sources off their queue, chained by next, no lock held: each goes, a timer once its alarm is
   over (a fire under way on another CPU ends first), a watch with its reference to its queue
   (which may be the last). */
static void sources_free(struct event_source *gone) {
    while (gone) {
        struct event_source *next = gone->next;
        if (gone->kind == SOURCE_TIMER) alarm_cancel(&gone->timer.alarm);
        struct object *queue = gone->kind == SOURCE_HANDLE ? &gone->queue->object : NULL;
        kfree(gone);
        object_unref(queue);
        gone = next;
    }
}

/* The queue's handle closed for good (no wait, watch or unwatch comes any more): its sources go,
   one at a time under its lock, which its watched handles' closes may want meanwhile. Its object
   stays while the handle holds it and each watch a handle's close took holds it. */
static void queue_close(struct handle *h) {
    struct event_queue *q = h->object->priv;
    for (;;) {
        uint64_t flags = spin_lock(&q->lock);
        struct avl_node *n = q->sources.root;
        struct event_source *s = n ? SOURCE(n) : NULL;
        bool mine = s && source_unlink(q, s, NULL);
        spin_unlock(&q->lock, flags);
        if (!s) break;
        if (!mine) continue;
        s->next = NULL;
        sources_free(s);
    }
}

static void queue_release(struct object *o) { kfree(o->priv); }

static const struct object_ops queue_ops = {.kind = N_KIND_EVENTS, .close = queue_close, .release = queue_release};

int events_create(struct handle **out) {
    struct event_queue *q = kzalloc(sizeof *q);
    if (!q) return -N_ERR_NO_MEMORY;
    q->pending_tail = &q->pending;
    object_init(&q->object, &queue_ops, q);
    *out = handle_open(&q->object, HANDLE_WRITE);
    if (!*out) { kfree(q); return -N_ERR_NO_MEMORY; }
    return 0;
}

bool events_is(struct handle *h) { return handle_is(h, &queue_ops); }

static struct event_queue *queue_of(struct handle *h) { return h->object->priv; }

size_t events_watch_size(void) { return sizeof(struct event_source); }

void events_handle_closed(struct handle *h) {
    uint64_t flags = spin_lock(&h->watch_lock);
    struct event_source *mine = h->watchers;
    h->watchers = NULL;
    for (struct event_source *w = mine; w; w = w->watch.next) w->watch.prev = NULL; /* this close's now */
    spin_unlock(&h->watch_lock, flags);
    while (mine) {
        struct event_source *w = mine;
        mine = w->watch.next;
        struct event_queue *q = w->queue; /* kept by w's reference */
        flags = spin_lock(&q->lock);
        if (w->linked) source_unlink(q, w, NULL);
        spin_unlock(&q->lock, flags);
        w->next = NULL;
        sources_free(w);
    }
}

/* A second watch of the same handle with the same token changes its interest. Either way the next
   wait looks at it. */
int events_watch(struct handle *queue, struct handle *h, uint32_t interest, uint64_t token,
                 struct event_source **spare) {
    struct event_queue *q = queue_of(queue);
    if (events_is(h)) return -N_ERR_WRONG_KIND; /* queues do not nest */
    uint64_t flags = spin_lock(&q->lock);
    struct event_source *w = source_find(q, token, (uintptr_t)h);
    if (w) {
        w->watch.interest = interest;
        if (watch_disarm(w)) source_tell(w); /* else pending already */
    } else {
        w = *spare;
        *spare = NULL;
        *w = (struct event_source){.queue = q, .token = token, .tag = (uintptr_t)h, .kind = SOURCE_HANDLE,
                                   .linked = true, .item = {.callback = source_told},
                                   .watch = {.handle = h, .interest = interest}};
        avl_insert(&q->sources, &by_token, &w->node);
        spin_take(&h->watch_lock);
        w->watch.next = h->watchers;
        w->watch.prev = &h->watchers;
        if (h->watchers) h->watchers->watch.prev = &w->watch.next;
        h->watchers = w;
        spin_give(&h->watch_lock);
        object_ref(&q->object);
        source_tell(w);
    }
    spin_unlock(&q->lock, flags);
    return 0;
}

/* Armed and put in the tree under the queue's lock: a wake that tells it at once is looked at by a
   wait only once it is the queue's. */
int events_watch_address(struct handle *queue, volatile uint32_t *address, uint32_t expected, uint64_t token) {
    struct event_queue *q = queue_of(queue);
    uint64_t phys = address_word(address, false);
    if (!phys) return -N_ERR_BAD_ADDRESS;
    struct event_source *s = kmalloc(sizeof *s);
    if (!s) return -N_ERR_NO_MEMORY;
    *s = (struct event_source){.queue = q, .token = token, .tag = (uintptr_t)s, .kind = SOURCE_ADDRESS,
                               .linked = true, .item = {.callback = source_told}};
    uint64_t flags = spin_lock(&q->lock);
    bool armed = address_watch_arm(&s->item, phys, expected);
    if (armed) avl_insert(&q->sources, &by_token, &s->node);
    spin_unlock(&q->lock, flags);
    if (armed) return 0;
    kfree(s);
    return -N_ERR_NOT_READY;
}

int events_timer(struct handle *queue, uint64_t due, uint64_t period, uint64_t token) {
    struct event_queue *q = queue_of(queue);
    struct event_source *t = kmalloc(sizeof *t);
    if (!t) return -N_ERR_NO_MEMORY;
    *t = (struct event_source){.queue = q, .token = token, .tag = (uintptr_t)t, .kind = SOURCE_TIMER,
                               .linked = true, .timer = {.due = due, .period = period}};
    alarm_init(&t->timer.alarm, timer_fire);
    uint64_t flags = spin_lock(&q->lock);
    avl_insert(&q->sources, &by_token, &t->node);
    if (due != CLOCK_NEVER) alarm_set(&t->timer.alarm, due);
    spin_unlock(&q->lock, flags);
    return 0;
}

/* 0 nothing had the token; 1 removed before its event; 2 its memory-word event had already been
   queued (and is now withdrawn). */
int events_unwatch(struct handle *queue, uint64_t token) {
    struct event_queue *q = queue_of(queue);
    int result = 0;
    struct event_source *gone = NULL;
    uint64_t flags = spin_lock(&q->lock);
    for (struct event_source *s; (s = source_find(q, token, 0));)
        if (source_unlink(q, s, &result)) {
            s->next = gone;
            gone = s;
        }
    spin_unlock(&q->lock, flags);
    sources_free(gone);
    return result;
}

#define EVENTS_BATCH 64 /* the most one wait returns (docs/nocturne-native-abi.md §6) */

int64_t events_wait(struct handle *queue, struct n_event *uout, size_t max, uint64_t deadline) {
    struct event_queue *q = queue_of(queue);
    if (!max) return -N_ERR_INVALID;
    if (max > EVENTS_BATCH) max = EVENTS_BATCH; /* the rest stays for the next wait */
    struct event found[EVENTS_BATCH];
    struct wait_queue *threads = &q->object.waiters;
    for (;;) {
        struct event_source *gone = NULL, *kept = NULL, **kept_tail = &kept;
        size_t n = 0;
        uint64_t flags = spin_lock(&q->lock);
        uint64_t now = clock_now_ns();
        for (;;) {
            spin_take(&q->told);
            struct event_source *s = n < max ? q->pending : NULL;
            if (s) pending_drop(q, s);
            spin_give(&q->told);
            if (!s) break;
            if (s->kind == SOURCE_HANDLE) {
                uint32_t ready = handle_ready(s->watch.handle) & s->watch.interest;
                if (!ready) {
                    watch_arm(s);
                    ready = handle_ready(s->watch.handle) & s->watch.interest;
                    /* not ready: armed; ready, but a change took it meanwhile: pending again */
                    if (!ready || !watch_disarm(s)) continue;
                }
                found[n++] = (struct event){.token = s->token, .events = ready, .at = s->at};
                s->next = NULL; /* stays pending, after what comes meanwhile */
                *kept_tail = s;
                kept_tail = &s->next;
            } else if (s->kind == SOURCE_TIMER) {
                found[n++] = (struct event){.token = s->token, .events = N_EVENT_TIMER, .at = s->timer.due};
                if (s->timer.period) {
                    s->timer.due = timer_next(s, now);
                    if (s->timer.due != CLOCK_NEVER) alarm_set(&s->timer.alarm, s->timer.due);
                } else if (source_unlink(q, s, NULL)) {
                    s->next = gone;
                    gone = s;
                }
            } else {
                found[n++] = (struct event){.token = s->token, .events = N_EVENT_ADDRESS, .at = s->at};
                if (source_unlink(q, s, NULL)) {
                    s->next = gone;
                    gone = s;
                }
            }
        }
        spin_take(&q->told);
        /* What is pending now this wait did not look at (its batch was full, or it came since the
           last look, its wake perhaps finding no thread asleep): another thread is woken for it. */
        bool left = q->pending != NULL;
        while (kept) {
            struct event_source *s = kept;
            kept = s->next;
            pending_add(q, s);
        }
        spin_give(&q->told);
        spin_unlock(&q->lock, flags);
        if (left) wq_wake(threads, 1);
        sources_free(gone);
        if (n) {
            /* the moments stay here until n_event carries them */
            struct n_event out[EVENTS_BATCH];
            for (size_t i = 0; i < n; i++) out[i] = (struct n_event){.token = found[i].token, .events = found[i].events};
            return copy_to_user(uout, out, n * sizeof *uout) < 0 ? -N_ERR_BAD_ADDRESS : (int64_t)n;
        }
        if (now >= deadline) return 0;
        if (thread_current()->killed) return -N_ERR_STOPPED;
        /* Asleep unless something is pending, looked at under the threads' lock: a source told
           after the look is on the line before its wake takes that lock, and finds this thread. */
        flags = spin_lock(&threads->lock);
        if (__atomic_load_n(&q->pending, __ATOMIC_ACQUIRE)) spin_give(&threads->lock);
        else wq_wait_locked_exclusive_until(threads, &threads->lock, deadline);
        irq_restore(flags);
    }
}
