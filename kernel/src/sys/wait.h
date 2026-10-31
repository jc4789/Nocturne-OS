/* Waits: wait queues and their items, sleeps on a deadline, the mutex and the reader-writer mutex
   (sys/wait.c). Waits on an app's memory word are sys/address_wait.h, built on these. */
#pragma once
#include "kernel.h"
#include "arch/spin.h"
#include "lib/avl.h"
#include "sys/alarm.h"

struct mutex;
struct thread;
struct wait_queue;

/* One waiter in a wait queue: a thread blocked there, or a callback its owner put there to hear of
   the next wake. Queues keep their items first come, first served. An exclusive item is one of
   whom a wake may take only some (wq_wake's n); every wake takes all the others.

   A wake takes each item it wakes out of the queue, under the queue's lock: a thread item's thread
   is made ready (sched_make_ready), a callback item's callback is called. A callback hears of one
   wake; its owner adds it again (wq_add) to hear of the next.

   The callback's contract. It is called by the waker, with the queue's lock held and interrupts
   off, in whatever context the waker is in: an interrupt's handler, an alarm's fire, a thread
   holding spinlocks that come before this queue's. So it never sleeps, copies user memory, takes a
   mutex or shoots down TLBs, and takes no lock that comes before a wait queue's (thread_lock, an
   owner's condition lock). It may take spinlocks that come after, and may wake another wait queue,
   whose lock then nests under this one's: the queues a chain of callbacks passes through never come
   back to one already held. It neither adds to nor takes from the queue it is called from (the
   waker walks it). The waker reads nothing of the item after the call: the item is its owner's
   again from the moment the callback begins, and may be freed by whoever the callback hands it to.
   An owner that takes its armed item back (wq_remove) under the queue's lock knows that no callback
   of it runs then or will, or that its callback has run already (wq_remove returns false). */
struct wait_item {
    struct wait_item *next, *prev; /* in its queue, the oldest first */
    struct wait_queue *queue;      /* the queue it is in, NULL when none (written under that queue's lock) */
    struct thread *thread;         /* a thread item's thread; NULL for a callback item */
    void (*callback)(struct wait_item *item);
    uint64_t key;                  /* what a keyed queue's waker matches (sys/address_wait.c: the word's
                                      physical address); a plain queue ignores it */
    bool exclusive;
};

/* A queue of waiters. Zeroed memory is an empty queue. Its lock guards its items and comes before
   a run queue's. A waiter whose condition another CPU changes looks at it under a lock that waker
   also holds while it wakes (it may be this queue's own), or loses the wake between its look and
   its sleep. */
struct wait_queue {
    struct wait_item *head, *tail;
    size_t shared; /* items in it that are not exclusive */
    struct spinlock lock;
};

/* The clock a deadline is on (sys/clock.h). The monotonic clock stands still while the machine
   sleeps; the boot clock does not, so a deadline on it that passed meanwhile comes at the wake. */
enum deadline_clock { DEADLINE_MONOTONIC, DEADLINE_BOOT };

/* A thread's part in the waits (struct thread's wait). */
struct thread_wait {
    struct wait_item item;     /* its place in the queue it is blocked in */
    struct wait_queue alone;   /* the queue of a sleep on a deadline alone: nothing wakes it but the
                                  deadline or a kill */
    struct alarm alarm;        /* its deadline's, armed while it waits for one, on the CPU it slept on */
    uint64_t deadline;         /* the wait's, on clock; CLOCK_NEVER for none */
    enum deadline_clock clock;
    struct thread_wait *boot_next, **boot_prev; /* among the sleeps on the boot clock (sys/wait.c) */
    bool timed_out;            /* its deadline ended the wait */
    bool killable;             /* the wait it is in ends at a kill */
    bool kill_heard;           /* a kill has ended one of its waits */
    /* What it waits for, whose holder may be traced from here (priority inheritance): the mutex,
       or in an address wait the word, item.key (0 in any other wait; n_procinfo's wait_phys). */
    struct mutex *mutex;
    /* Priority inheritance (sys/wait.c). pi_lock guards mutex, pi_rank and pi_held, and with its
       queue's lock the thread's sched.boost. pi_held: the mutexes it holds that realtime threads
       wait for, the highest rank first. While it waits for a mutex at a realtime rank: pi_node, its
       place among that mutex's donors, at pi_rank (0 when it is not among them), and pi_seq, when
       it came. */
    struct spinlock pi_lock;
    struct avl_tree pi_held;
    struct avl_node pi_node;
    unsigned pi_rank;
    uint64_t pi_seq;
};
/* A new thread's part, before it ever waits (sys/thread.c). */
void thread_wait_init(struct thread_wait *w);

/* Waits. A condition another CPU changes is looked at under the lock its changer holds, then
   wq_wait_locked; plain wq_wait fits a waiter that looks again after each wake anyway.
   A deadline is a moment on the monotonic clock (clock_now_ns; clock_after(ns) for one ns from
   now), CLOCK_NEVER for none: the _until waits return false when it came before a waker did.

   These waits are killable: a kill (thread_kill_locked) ends the wait the thread is in, or, when
   it comes while the thread runs, the next one it enters, which then does not sleep. A kill ends
   one wait only: a caller that gives up on a kill looks at its thread's killed before it waits
   (or in its loop's condition), and one that does not waits on for its condition as if woken
   early. So no wait is ever ended again and again for a thread that was killed.

   The boot thread, which is CPU 0's idle thread and has no stack of its own to sleep on, runs this
   CPU's other threads and idles in between until it is woken (out of epoch's count as it idles,
   sys/epoch.h). */
void wq_wait(struct wait_queue *wq);
bool wq_wait_until(struct wait_queue *wq, uint64_t deadline);
/* For a condition another CPU changes without a lock this waiter can hold: sleep on wq unless
   *word (which the waker moves on before it wakes wq) no longer reads seen, the value read before
   the condition was checked. */
bool wq_wait_unless(struct wait_queue *wq, const uint64_t *word, uint64_t seen, uint64_t deadline);
/* Interrupts off, held taken: sleep on wq, held given back once enrolled (a waker that takes held
   to change the condition, then wakes wq, finds the thread there). Returns without held, interrupts
   still off. held may be wq's own lock. */
void wq_wait_locked(struct wait_queue *wq, struct spinlock *held);
bool wq_wait_locked_until(struct wait_queue *wq, struct spinlock *held, uint64_t deadline);
/* As wq_wait_locked_until, its item exclusive: wq_wake(wq, n) takes it among the first n exclusive
   ones and leaves the others asleep. For waiters of whom one is enough for each change (an event
   queue's threads, a channel's receivers or senders): a waker wakes as many as its change can
   serve, and one woken that leaves what woke it for another (it only looked, or what it took
   left more) wakes the next itself (wq_wake(wq, 1)). A thread woken is never also timed out or
   killed out of the same wait, so no wake is lost to a deadline or a kill. */
bool wq_wait_locked_exclusive_until(struct wait_queue *wq, struct spinlock *held, uint64_t deadline);
/* Asleep until the deadline, or a kill. sleep_until_boot's deadline is on the boot clock. */
void sleep_until(uint64_t deadline);
void sleep_until_boot(uint64_t deadline);

/* Wakes: every item that is not exclusive, and the first n exclusive ones (wq_wake_all: all of
   them). Any context, any CPU. */
void wq_wake(struct wait_queue *wq, unsigned n);
void wq_wake_all(struct wait_queue *wq);

/* Callback items and keyed queues; wq's lock held. wq_add: item, not in a queue, at wq's tail.
   wq_remove: item out of wq; false when it was not there (a waker took it). wq_wake_item: item,
   in wq, taken out and woken. */
void wq_add(struct wait_queue *wq, struct wait_item *item);
bool wq_remove(struct wait_queue *wq, struct wait_item *item);
void wq_wake_item(struct wait_queue *wq, struct wait_item *item);

/* Interrupts off: t leaves the wait it is blocked in, unless a waker on another CPU took it out
   first (and wakes it), or it is in none. timed: its deadline came; else a kill, which only a
   killable wait hears. True: the caller wakes it (sched_make_ready). */
bool wq_take_blocked(struct thread *t, bool timed);

/* The boot clock moved on against the monotonic one (a wake from S3, sys/clock.c's time asleep):
   the sleeps on it are set again for their moments. Any context but holding an alarm's lock. */
void wait_boot_clock_moved(void);

/* A sleeping lock: state that stays borrowed across allocations, copies, waits or disk transfers.
   Its waiters queue first come, first served, and the one that lets it go hands it to the oldest
   (no thread that comes later takes it first), but to a realtime waiter before the others: the
   highest rank (sched_rank) first, the oldest among equals. Meanwhile its holder runs at the
   highest rank among the waiters of the mutexes it holds, and lends that on to the holder of a
   mutex it waits for in turn (priority inheritance), so that a realtime thread waits for no fair
   thread the others hold off. One that finds it held while its holder runs on another CPU spins
   for it until the holder lets it go or stops running, and queues then. Its waits are not
   killable. The holder may take it again (depth): the window manager's calls nest, as do some of
   C:'s (a move within one folder, a document's read from a fault in its write). Interrupt handlers
   never take one. Zeroed memory is a mutex not held. */
#define MUTEX_WAITERS ((uintptr_t)1) /* in owner: a thread waits in wait */
struct mutex {
    uintptr_t owner;        /* the holding thread | MUTEX_WAITERS; 0 when free */
    unsigned depth;         /* the holder's */
    unsigned cpu;           /* the CPU its holder took it on: where a spinner looks for it running */
    struct wait_queue wait; /* its waiters, exclusive items; its lock orders their hand-offs */
    /* Under wait's lock: its realtime waiters (their thread_wait's pi_node), the order they came
       in (pi_seq), and while it has any, its place in its holder's pi_held at the first one's rank
       (rank; 0 while it is not there). */
    struct avl_tree donors;
    uint64_t seq;
    struct avl_node held;
    unsigned rank;
};
void mutex_lock(struct mutex *m);
void mutex_unlock(struct mutex *m);
bool mutex_held(struct mutex *m); /* by the current thread */
/* Holding m (taken once): sleeps on wq with m let go, then takes m again. Whoever changes what the
   sleeper waits for holds m while it does, and wakes wq after: wq's lock is taken before m is let
   go, so a wake on another CPU finds the sleeper in wq. (wq's lock comes before m's own; nothing
   takes them the other way.) The wait on wq is killable. */
void mutex_wait(struct mutex *m, struct wait_queue *wq);

/* A sleeping lock many may hold to read while none holds it to write. Its waiters queue first
   come, first served: a reader that comes while a writer waits queues behind it, so writers are
   not starved; a writer letting it go hands it to the oldest waiter, or to all the readers queued
   before the next writer at once, and the last reader to let it go hands it to the writer first in
   line. Not taken again by its holder; its waits are not killable. Zeroed memory is one not held. */
struct rwmutex {
    uint64_t state;         /* RWMUTEX_WRITER | RWMUTEX_WAITERS | the readers holding it (sys/wait.c) */
    struct wait_queue wait; /* readers (shared items) and writers (exclusive items) in the order they came */
};
void rwmutex_lock_read(struct rwmutex *m);
void rwmutex_unlock_read(struct rwmutex *m);
void rwmutex_lock_write(struct rwmutex *m);
void rwmutex_unlock_write(struct rwmutex *m);
