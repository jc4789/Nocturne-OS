/* Waits (sys/wait.h): a thread sleeps with its item in a wait queue until a waker takes it out, its
   deadline (its own alarm, on the CPU it slept on) does, or a kill does. thread_lock
   (sys/thread.h) comes before a wait queue's lock, that before a thread's pi lock, and that before
   a run queue's. */
#include "kernel.h"
#include "arch/cpu.h"
#include "arch/idle.h"
#include "arch/smp.h"
#include "arch/topology.h"
#include "sys/clock.h"
#include "sys/epoch.h"
#include "sys/sched.h"
#include "sys/thread.h"
#include "sys/wait.h"

#define WAKE_ALL (~0u)

/* ---- queues ---- */

void wq_add(struct wait_queue *wq, struct wait_item *item) {
    item->next = NULL;
    item->prev = wq->tail;
    if (wq->tail) wq->tail->next = item;
    else wq->head = item;
    wq->tail = item;
    if (!item->exclusive) wq->shared++;
    __atomic_store_n(&item->queue, wq, __ATOMIC_RELAXED);
}

static void unlink(struct wait_queue *wq, struct wait_item *item) {
    if (item->prev) item->prev->next = item->next;
    else wq->head = item->next;
    if (item->next) item->next->prev = item->prev;
    else wq->tail = item->prev;
    item->next = item->prev = NULL;
    if (!item->exclusive) wq->shared--;
}

bool wq_remove(struct wait_queue *wq, struct wait_item *item) {
    if (item->queue != wq) return false;
    unlink(wq, item);
    __atomic_store_n(&item->queue, NULL, __ATOMIC_RELEASE);
    return true;
}

void wq_wake_item(struct wait_queue *wq, struct wait_item *item) {
    unlink(wq, item);
    struct thread *t = item->thread;
    __atomic_store_n(&item->queue, NULL, __ATOMIC_RELEASE); /* a sleeper that looks (the boot thread) goes */
    if (t) sched_make_ready(t);
    else item->callback(item);
}

/* wq's lock held: from the oldest, every item that is not exclusive and the first n exclusive
   ones; the walk ends once both are done. */
static void wake_locked(struct wait_queue *wq, unsigned n) {
    for (struct wait_item *i = wq->head, *next; i && (n || wq->shared); i = next) {
        next = i->next; /* before the wake: the item is its owner's after it */
        if (i->exclusive) {
            if (!n) continue;
            if (n != WAKE_ALL) n--;
        }
        wq_wake_item(wq, i);
    }
}

void wq_wake(struct wait_queue *wq, unsigned n) {
    uint64_t f = spin_lock(&wq->lock);
    wake_locked(wq, n);
    spin_unlock(&wq->lock, f);
}

void wq_wake_all(struct wait_queue *wq) {
    uint64_t f = spin_lock(&wq->lock);
    wake_locked(wq, WAKE_ALL);
    spin_unlock(&wq->lock, f);
}

/* ---- deadlines ---- */

/* The sleeps on the boot clock: each one's alarm is armed for the monotonic moment its deadline
   is at by the time asleep known then, under boot_lock, which comes before a CPU's alarm lock and
   is never held with a wait queue's. */
static struct spinlock boot_lock;
static struct thread_wait *boot_sleeps;

/* The monotonic moment of a boot clock deadline: the boot clock is read first, so the moment
   comes no sooner than the deadline, and later only by the reads' distance. */
static uint64_t boot_to_monotonic(uint64_t deadline) {
    if (deadline == CLOCK_NEVER) return CLOCK_NEVER;
    uint64_t boot = clock_boot_ns(), now = clock_now_ns();
    uint64_t asleep = boot > now ? boot - now : 0;
    return deadline > asleep ? deadline - asleep : 0;
}

static void deadline_arm(struct thread_wait *w, uint64_t deadline, enum deadline_clock clock) {
    w->deadline = deadline;
    w->clock = clock;
    if (clock == DEADLINE_MONOTONIC) {
        alarm_set(&w->alarm, deadline);
        return;
    }
    uint64_t f = spin_lock(&boot_lock);
    w->boot_prev = &boot_sleeps;
    w->boot_next = boot_sleeps;
    if (boot_sleeps) boot_sleeps->boot_prev = &w->boot_next;
    boot_sleeps = w;
    alarm_set(&w->alarm, boot_to_monotonic(deadline));
    spin_unlock(&boot_lock, f);
}

/* After the wait: no fire of its alarm runs any more, nor will (alarm_cancel waits for one on
   another CPU). */
static void deadline_disarm(struct thread_wait *w) {
    if (w->clock == DEADLINE_BOOT) {
        uint64_t f = spin_lock(&boot_lock);
        *w->boot_prev = w->boot_next;
        if (w->boot_next) w->boot_next->boot_prev = w->boot_prev;
        w->boot_next = NULL;
        w->boot_prev = NULL;
        spin_unlock(&boot_lock, f);
    }
    alarm_cancel(&w->alarm);
}

void wait_boot_clock_moved(void) {
    uint64_t f = spin_lock(&boot_lock);
    for (struct thread_wait *w = boot_sleeps; w; w = w->boot_next) alarm_set(&w->alarm, boot_to_monotonic(w->deadline));
    spin_unlock(&boot_lock, f);
}

/* A sleeper's deadline came, on the CPU its alarm is armed on. */
static void wake_fire(struct alarm *a, struct regs *r) {
    (void)r;
    struct thread *t = ((struct thread_wait *)((char *)a - __builtin_offsetof(struct thread_wait, alarm)))->item.thread;
    if (wq_take_blocked(t, true)) sched_make_ready(t);
}

void thread_wait_init(struct thread_wait *w) {
    w->item.thread = (struct thread *)((char *)w - __builtin_offsetof(struct thread, wait));
    alarm_init(&w->alarm, wake_fire);
}

/* ---- sleeping ---- */

#define WAIT_EXCLUSIVE 1u /* its item is exclusive (wq_wake's n) */
#define WAIT_KILLABLE 2u  /* a kill ends it */

/* Interrupts off. The thread sleeps in wq until a waker takes it out, its deadline (on clock;
   CLOCK_NEVER: none) comes or, killable, a kill does; not when word, read under wq's lock, has
   moved on from seen, nor when the deadline has passed. held, a lock the caller took to decide that
   it sleeps (it may be wq's own), is given back once the thread is enrolled. False when the
   deadline ended the wait. */
static bool sleep_in(struct wait_queue *wq, struct spinlock *held, const uint64_t *word, uint64_t seen,
                     uint64_t deadline, enum deadline_clock clock, unsigned how) {
    struct thread *t = thread_current();
    struct thread_wait *w = &t->wait;
    uint64_t at = clock == DEADLINE_BOOT ? boot_to_monotonic(deadline) : deadline;
    if (held != &wq->lock) spin_take(&wq->lock);
    bool sleep = !word || __atomic_load_n(word, __ATOMIC_SEQ_CST) == seen, late = false;
    if (sleep && at != CLOCK_NEVER && clock_now_ns() >= at) sleep = false, late = true;
    if (sleep) {
        w->timed_out = false;
        w->killable = how & WAIT_KILLABLE;
        w->item.exclusive = how & WAIT_EXCLUSIVE;
        wq_add(wq, &w->item);
        if (!t->sched.idle) {
            __atomic_store_n(&t->sched.state, THREAD_BLOCKED, __ATOMIC_SEQ_CST);
            /* A kill after this finds it blocked and takes it out (thread_kill_locked); one before,
               not heard yet, is seen here and ends this wait at once. */
            if (w->killable && !w->kill_heard && __atomic_load_n(&t->killed, __ATOMIC_SEQ_CST)) {
                w->kill_heard = true;
                wq_remove(wq, &w->item);
                __atomic_store_n(&t->sched.state, THREAD_RUNNING, __ATOMIC_RELAXED);
                sleep = false;
            }
        }
    }
    spin_give(&wq->lock);
    if (held && held != &wq->lock) spin_give(held);
    if (!sleep) return !late;
    if (at != CLOCK_NEVER) deadline_arm(w, deadline, clock);
    if (t->sched.idle) {
        /* An idle thread (the boot thread) cannot leave its CPU's queue to sleep, being what runs
           when nothing else does: it stays in wq and lets this CPU's other threads run, idling in
           between, until a waker takes it out and kicks this CPU (sched_make_ready), or its alarm
           does. */
        while (__atomic_load_n(&w->item.queue, __ATOMIC_ACQUIRE)) {
            schedule();
            if (!__atomic_load_n(&w->item.queue, __ATOMIC_ACQUIRE)) break;
            /* As sched_idle waits: an interrupt in the wait switches nothing there. Out of epoch's
               count while it waits (sys/epoch.h), so that no grace period waits for this CPU's
               idling, and in again before the boot thread goes on; a worker the quiet point wakes
               onto this CPU is run before any wait. */
            preempt_disable();
            epoch_quiet(true);
            if (!this_cpu(sched_resched) && __atomic_load_n(&w->item.queue, __ATOMIC_ACQUIRE)) cpu_idle();
            epoch_quiet(false);
            preempt_enable();
        }
    } else {
        schedule();
    }
    if (at != CLOCK_NEVER) deadline_disarm(w);
    return !w->timed_out;
}

bool wq_take_blocked(struct thread *t, bool timed) {
    struct thread_wait *w = &t->wait;
    struct wait_queue *wq = __atomic_load_n(&w->item.queue, __ATOMIC_ACQUIRE);
    if (!wq) return false;
    spin_take(&wq->lock);
    bool here = w->item.queue == wq && (timed || (w->killable && !w->kill_heard));
    if (here) {
        if (timed) w->timed_out = true;
        else w->kill_heard = true;
        wq_remove(wq, &w->item);
    }
    spin_give(&wq->lock);
    return here;
}

void wq_wait(struct wait_queue *wq) {
    uint64_t f = irq_save();
    sleep_in(wq, NULL, NULL, 0, CLOCK_NEVER, DEADLINE_MONOTONIC, WAIT_KILLABLE);
    irq_restore(f);
}

bool wq_wait_until(struct wait_queue *wq, uint64_t deadline) {
    uint64_t f = irq_save();
    bool ok = sleep_in(wq, NULL, NULL, 0, deadline, DEADLINE_MONOTONIC, WAIT_KILLABLE);
    irq_restore(f);
    return ok;
}

bool wq_wait_unless(struct wait_queue *wq, const uint64_t *word, uint64_t seen, uint64_t deadline) {
    uint64_t f = irq_save();
    bool ok = sleep_in(wq, NULL, word, seen, deadline, DEADLINE_MONOTONIC, WAIT_KILLABLE);
    irq_restore(f);
    return ok;
}

void wq_wait_locked(struct wait_queue *wq, struct spinlock *held) {
    sleep_in(wq, held, NULL, 0, CLOCK_NEVER, DEADLINE_MONOTONIC, WAIT_KILLABLE);
}

bool wq_wait_locked_until(struct wait_queue *wq, struct spinlock *held, uint64_t deadline) {
    return sleep_in(wq, held, NULL, 0, deadline, DEADLINE_MONOTONIC, WAIT_KILLABLE);
}

bool wq_wait_locked_exclusive_until(struct wait_queue *wq, struct spinlock *held, uint64_t deadline) {
    return sleep_in(wq, held, NULL, 0, deadline, DEADLINE_MONOTONIC, WAIT_EXCLUSIVE | WAIT_KILLABLE);
}

void sleep_until(uint64_t deadline) {
    uint64_t f = irq_save();
    sleep_in(&thread_current()->wait.alone, NULL, NULL, 0, deadline, DEADLINE_MONOTONIC, WAIT_KILLABLE);
    irq_restore(f);
}

void sleep_until_boot(uint64_t deadline) {
    uint64_t f = irq_save();
    sleep_in(&thread_current()->wait.alone, NULL, NULL, 0, deadline, DEADLINE_BOOT, WAIT_KILLABLE);
    irq_restore(f);
}

/* ---- priority inheritance ----

   A realtime thread waiting for a mutex is among its donors at its rank (sched_rank); a mutex that
   has donors is in its holder's pi_held at the first donor's rank, and the holder's inherited rank
   (sched.boost) is the first of its pi_held's. A holder that waits for a mutex in turn is among that
   one's donors at the rank it runs at, so a raised rank goes on along the chain of holders, each
   step as far as it changes something. A rank falls only where a mutex is handed over: its old
   holder runs again at what its other mutexes lend it, its new one at what the waiters left lend.
   Locks: a mutex's wait lock, then a thread's pi lock, then a run queue's (sched_set_boost). The
   walk along a chain takes the next mutex's wait lock under the pi lock of the holder waiting for
   it, the other order: it only tries, and lets go and tries again when another CPU holds it. */

#define WAIT_OF(n) ((struct thread_wait *)((char *)(n) - __builtin_offsetof(struct thread_wait, pi_node)))
#define MUTEX_OF(n) ((struct mutex *)((char *)(n) - __builtin_offsetof(struct mutex, held)))

static bool donor_before(const struct avl_node *a, const struct avl_node *b) {
    const struct thread_wait *x = WAIT_OF(a), *y = WAIT_OF(b);
    return x->pi_rank != y->pi_rank ? x->pi_rank > y->pi_rank : x->pi_seq < y->pi_seq;
}
static const struct avl_order donor_order = {.before = donor_before};

static bool held_before(const struct avl_node *a, const struct avl_node *b) {
    const struct mutex *x = MUTEX_OF(a), *y = MUTEX_OF(b);
    return x->rank != y->rank ? x->rank > y->rank : x < y;
}
static const struct avl_order held_order = {.before = held_before};

/* m's wait lock held while threads wait for it: its holder, which only a hand-off changes then. */
static struct thread *holder_of(struct mutex *m) {
    return (struct thread *)(__atomic_load_n(&m->owner, __ATOMIC_RELAXED) & ~MUTEX_WAITERS);
}

/* m's wait lock held: the rank its first donor lends, 0 with none. */
static unsigned donors_top(const struct mutex *m) {
    struct avl_node *n = avl_first(&m->donors);
    return n ? WAIT_OF(n)->pi_rank : 0;
}

/* m's wait lock and w's pi lock held, w's thread waiting for m: among m's donors at the rank its
   thread runs at now (out of them at 0). */
static void donor_rank(struct mutex *m, struct thread_wait *w) {
    if (w->pi_rank) avl_remove(&m->donors, &donor_order, &w->pi_node);
    w->pi_rank = sched_rank(&w->item.thread->sched);
    if (w->pi_rank) avl_insert(&m->donors, &donor_order, &w->pi_node);
}

/* m's wait lock and h's pi lock held, h holding m: m in h's pi_held at rank (out of it at 0), and
   h's inherited rank the highest of those. True when that changed. */
static bool held_rank(struct thread *h, struct mutex *m, unsigned rank) {
    struct thread_wait *w = &h->wait;
    if (m->rank) avl_remove(&w->pi_held, &held_order, &m->held);
    m->rank = rank;
    if (rank) avl_insert(&w->pi_held, &held_order, &m->held);
    struct avl_node *top = avl_first(&w->pi_held);
    unsigned boost = top ? MUTEX_OF(top)->rank : 0;
    /* an idle thread (the boot thread, at its work before it idles) runs whenever its CPU has
       nothing else, outside the classes: it inherits nothing */
    if (h->sched.idle || boost == h->sched.boost) return false;
    sched_set_boost(h, boost);
    return true;
}

/* m's wait lock held (and kept), its donors changed: its holder's rank, and on along the chain of
   holders waiting for mutexes as far as one changes. A chain that comes back to m is a deadlock
   among its holders, which nothing undoes: the walk stops there. */
static void pi_spread(struct mutex *m) {
    struct mutex *at = m; /* its wait lock held */
    for (;;) {
        struct thread *h = holder_of(at);
        struct thread_wait *w = &h->wait;
        spin_take(&w->pi_lock);
        struct mutex *next = NULL;
        if (held_rank(h, at, donors_top(at))) {
            while ((next = w->mutex) && next != m && !spin_try(&next->wait.lock)) {
                /* its holder, or a walk through it, holds it: h's lock let go a moment */
                spin_give(&w->pi_lock);
                pause();
                spin_take(&w->pi_lock);
            }
            if (next == m) next = NULL;
            if (next) donor_rank(next, w); /* h still waits for next: a hand-off would have cleared it */
        }
        spin_give(&w->pi_lock);
        if (at != m) spin_give(&at->wait.lock);
        if (!next) return;
        at = next;
    }
}

/* ---- the mutex ---- */

/* While its holder runs on another CPU and no thread is queued for it: true once this thread took
   it. The holder is looked for on the CPU it took the mutex on, by the thread that CPU runs (only a
   pointer compared: the holder's record may be gone by now); one that went to sleep or moved since
   is not running there, and the spinner queues. So does one whose own CPU has a thread to run. */
static bool mutex_spin(struct mutex *m, struct thread *self) {
    for (;;) {
        uintptr_t o = __atomic_load_n(&m->owner, __ATOMIC_RELAXED);
        if (!o) {
            if (__atomic_compare_exchange_n(&m->owner, &o, (uintptr_t)self, false, __ATOMIC_ACQUIRE, __ATOMIC_RELAXED))
                return true;
            continue;
        }
        if ((o & MUTEX_WAITERS) || this_cpu(sched_resched)) return false;
        unsigned cpu = __atomic_load_n(&m->cpu, __ATOMIC_RELAXED);
        if (cpu >= topology.cpus || __atomic_load_n(&per_cpu(sched_thread, cpu), __ATOMIC_RELAXED) != (struct thread *)o)
            return false;
        pause();
    }
}

/* Queued behind the threads before it, until the holder hands the mutex over (mutex_unlock), or
   took it at once when it was let go meanwhile. */
static void mutex_sleep(struct mutex *m, struct thread *self) {
    uint64_t f = spin_lock(&m->wait.lock);
    for (;;) {
        uintptr_t o = __atomic_load_n(&m->owner, __ATOMIC_RELAXED);
        if (!o) { /* free, and so no thread queued (MUTEX_WAITERS stays while one is) */
            if (__atomic_compare_exchange_n(&m->owner, &o, (uintptr_t)self, false, __ATOMIC_ACQUIRE, __ATOMIC_RELAXED)) {
                spin_unlock(&m->wait.lock, f);
                return;
            }
            continue;
        }
        if ((o & MUTEX_WAITERS) ||
            __atomic_compare_exchange_n(&m->owner, &o, o | MUTEX_WAITERS, false, __ATOMIC_RELAXED, __ATOMIC_RELAXED))
            break;
    }
    struct thread_wait *w = &self->wait;
    spin_take(&w->pi_lock);
    w->mutex = m;
    w->pi_seq = ++m->seq;
    w->pi_rank = 0;
    donor_rank(m, w);
    spin_give(&w->pi_lock);
    if (w->pi_rank) pi_spread(m);
    /* the hand-off takes it out of the donors and clears w->mutex, under w's pi lock */
    sleep_in(&m->wait, &m->wait.lock, NULL, 0, CLOCK_NEVER, DEADLINE_MONOTONIC, WAIT_EXCLUSIVE);
    irq_restore(f);
    if ((__atomic_load_n(&m->owner, __ATOMIC_ACQUIRE) & ~MUTEX_WAITERS) != (uintptr_t)self)
        panic("mutex: woken without being handed it");
}

void mutex_lock(struct mutex *m) {
    struct thread *self = thread_current();
    uintptr_t o = 0;
    if (!__atomic_compare_exchange_n(&m->owner, &o, (uintptr_t)self, false, __ATOMIC_ACQUIRE, __ATOMIC_RELAXED)) {
        if ((o & ~MUTEX_WAITERS) == (uintptr_t)self) {
            m->depth++;
            return;
        }
        if (!mutex_spin(m, self)) mutex_sleep(m, self);
    }
    m->depth = 1;
    __atomic_store_n(&m->cpu, cpu_current_index(), __ATOMIC_RELAXED);
}

void mutex_unlock(struct mutex *m) {
    struct thread *self = thread_current();
    if ((__atomic_load_n(&m->owner, __ATOMIC_RELAXED) & ~MUTEX_WAITERS) != (uintptr_t)self || !m->depth)
        panic("mutex: released by a thread that does not hold it");
    if (--m->depth) return;
    uintptr_t o = (uintptr_t)self;
    if (__atomic_compare_exchange_n(&m->owner, &o, 0, false, __ATOMIC_RELEASE, __ATOMIC_RELAXED)) return;
    /* Threads are queued (no one but this holder changes owner now): the first donor takes it from
       here, or with none the oldest, before any that comes later. */
    uint64_t f = spin_lock(&m->wait.lock);
    struct avl_node *d = avl_first(&m->donors);
    struct wait_item *next = d ? &WAIT_OF(d)->item : m->wait.head;
    struct thread *n = next->thread;
    struct thread_wait *nw = &n->wait;
    spin_take(&nw->pi_lock);
    if (nw->pi_rank) avl_remove(&m->donors, &donor_order, &nw->pi_node);
    nw->pi_rank = 0;
    nw->mutex = NULL;
    spin_give(&nw->pi_lock);
    if (m->rank) { /* this holder runs at what its other mutexes lend it */
        spin_take(&self->wait.pi_lock);
        held_rank(self, m, 0);
        spin_give(&self->wait.pi_lock);
    }
    __atomic_store_n(&m->owner, (uintptr_t)n | (m->wait.head != next || next->next ? MUTEX_WAITERS : 0),
                     __ATOMIC_RELEASE);
    unsigned top = donors_top(m);
    if (top) { /* the new holder, which waits for nothing else, at what the waiters left lend it */
        spin_take(&nw->pi_lock);
        held_rank(n, m, top);
        spin_give(&nw->pi_lock);
    }
    wq_wake_item(&m->wait, next);
    spin_unlock(&m->wait.lock, f);
}

bool mutex_held(struct mutex *m) {
    return (__atomic_load_n(&m->owner, __ATOMIC_RELAXED) & ~MUTEX_WAITERS) == (uintptr_t)thread_current();
}

void mutex_wait(struct mutex *m, struct wait_queue *wq) {
    if (!mutex_held(m) || m->depth != 1) panic("mutex: waited on by a thread that does not hold it once");
    uint64_t f = spin_lock(&wq->lock);
    mutex_unlock(m);
    wq_wait_locked(wq, &wq->lock);
    irq_restore(f);
    mutex_lock(m);
}

/* ---- the reader-writer mutex ---- */

/* state: RWMUTEX_WRITER while a writer holds it, else the count of readers holding it;
   RWMUTEX_WAITERS while a thread is queued in wait (it changes under wait's lock). No thread takes
   it past the queue: the one that lets it go last hands it to the queue's head. */
#define RWMUTEX_WRITER (1ull << 63)
#define RWMUTEX_WAITERS (1ull << 62)
#define RWMUTEX_READERS (RWMUTEX_WAITERS - 1)

/* wait's lock held, a thread queued, the holders letting it go: the oldest waiter takes it, a
   writer alone or every reader before the next writer at once. */
static void rwmutex_hand_off(struct rwmutex *m) {
    struct wait_item *first = m->wait.head, *stop = first->next;
    uint64_t state = RWMUTEX_WRITER;
    if (!first->exclusive) {
        for (state = 1; stop && !stop->exclusive; stop = stop->next) state++;
    }
    __atomic_store_n(&m->state, state | (stop ? RWMUTEX_WAITERS : 0), __ATOMIC_RELEASE);
    for (struct wait_item *i = first, *next; i != stop; i = next) {
        next = i->next;
        wq_wake_item(&m->wait, i);
    }
}

/* Queued, until handed the mutex; or took it at once, when it was let go meanwhile with no
   thread queued. */
static void rwmutex_sleep(struct rwmutex *m, bool writer) {
    uint64_t f = spin_lock(&m->wait.lock);
    uint64_t s = __atomic_load_n(&m->state, __ATOMIC_RELAXED);
    for (;;) {
        if (!(s & RWMUTEX_WAITERS) && (writer ? !s : !(s & RWMUTEX_WRITER))) {
            if (__atomic_compare_exchange_n(&m->state, &s, writer ? RWMUTEX_WRITER : s + 1, false, __ATOMIC_ACQUIRE,
                                            __ATOMIC_RELAXED)) {
                spin_unlock(&m->wait.lock, f);
                return;
            }
            continue;
        }
        if ((s & RWMUTEX_WAITERS) ||
            __atomic_compare_exchange_n(&m->state, &s, s | RWMUTEX_WAITERS, false, __ATOMIC_RELAXED, __ATOMIC_RELAXED))
            break;
    }
    sleep_in(&m->wait, &m->wait.lock, NULL, 0, CLOCK_NEVER, DEADLINE_MONOTONIC, writer ? WAIT_EXCLUSIVE : 0);
    irq_restore(f);
    __atomic_thread_fence(__ATOMIC_ACQUIRE); /* after the hand-off's store */
}

void rwmutex_lock_read(struct rwmutex *m) {
    uint64_t s = __atomic_load_n(&m->state, __ATOMIC_RELAXED);
    while (!(s & (RWMUTEX_WRITER | RWMUTEX_WAITERS)))
        if (__atomic_compare_exchange_n(&m->state, &s, s + 1, false, __ATOMIC_ACQUIRE, __ATOMIC_RELAXED)) return;
    rwmutex_sleep(m, false);
}

void rwmutex_unlock_read(struct rwmutex *m) {
    uint64_t s = __atomic_load_n(&m->state, __ATOMIC_RELAXED);
    for (;;) {
        if ((s & RWMUTEX_WRITER) || !(s & RWMUTEX_READERS)) panic("rwmutex: let go by a reader that does not hold it");
        if ((s & RWMUTEX_READERS) == 1 && (s & RWMUTEX_WAITERS)) break;
        if (__atomic_compare_exchange_n(&m->state, &s, s - 1, false, __ATOMIC_RELEASE, __ATOMIC_RELAXED)) return;
    }
    /* the last reader, threads queued: no other reader takes it meanwhile (they queue) */
    uint64_t f = spin_lock(&m->wait.lock);
    rwmutex_hand_off(m);
    spin_unlock(&m->wait.lock, f);
}

void rwmutex_lock_write(struct rwmutex *m) {
    uint64_t s = 0;
    if (__atomic_compare_exchange_n(&m->state, &s, RWMUTEX_WRITER, false, __ATOMIC_ACQUIRE, __ATOMIC_RELAXED)) return;
    rwmutex_sleep(m, true);
}

void rwmutex_unlock_write(struct rwmutex *m) {
    uint64_t s = RWMUTEX_WRITER;
    if (__atomic_compare_exchange_n(&m->state, &s, 0, false, __ATOMIC_RELEASE, __ATOMIC_RELAXED)) return;
    if (!(s & RWMUTEX_WRITER)) panic("rwmutex: let go by a writer that does not hold it");
    uint64_t f = spin_lock(&m->wait.lock);
    rwmutex_hand_off(m);
    spin_unlock(&m->wait.lock, f);
}
