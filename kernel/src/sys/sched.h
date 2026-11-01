/* The scheduler (sys/sched.c): each CPU's run queue and idle thread, the two classes threads run
   in, preemption, and the balance between the queues. */
#pragma once
#include "kernel.h"
#include "arch/percpu.h"
#include "lib/avl.h"
#include "sys/alarm.h"

struct regs;
struct thread;

/* THREAD_RUNNING: it runs, or may run (a queue holds it); THREAD_READY is what apps are told of one
   waiting in a queue (n_procinfo). A thread sets THREAD_BLOCKED itself to wait, and THREAD_ZOMBIE
   at its end; a waker turns THREAD_BLOCKED into THREAD_RUNNING (sched_make_ready). */
enum thread_state { THREAD_READY, THREAD_RUNNING, THREAD_BLOCKED, THREAD_ZOMBIE };

/* The two classes.

   A deadline thread does a job each period of its own: a frame for a screen, a report for an input
   device. Its period comes from that device (the screen's refresh, the endpoint's service
   interval). A job is released when the thread is woken, and is due by the end of the period it
   was released in: of the deadline threads the one whose job is due first runs (EDF), before every
   fair thread. Each period it may run its reservation (a constant bandwidth server, Abeni and
   Buttazzo, "Integrating Multimedia Applications in Hard Real-Time Systems", 1998): once that is
   spent it runs in the fair class until its deadline, then gets a new period and a whole
   reservation. A thread woken early keeps its deadline only while its reservation left fits the
   time to it at its bandwidth; else its job begins a new period at once. So no deadline thread
   takes more than its bandwidth from the others, and none takes a CPU from the fair class for good.
   The reservation follows the jobs: the scheduler measures what each job used, and reserves the
   recent peak with headroom, within a share of the period (sys/sched.c, DL_*).

   A fair thread gets CPU time in proportion to its weight, by EEVDF (Stoica and Abdel-Wahab,
   "Earliest Eligible Virtual Deadline First", 1995): each thread asks for request_ns at a time; of
   the eligible threads (those not ahead of their share) the one whose request ends first runs, and
   a woken one keeps the lag it left with. A short request gets the CPU sooner, a long one keeps it
   longer. */
enum sched_class { SCHED_FAIR, SCHED_DEADLINE };

struct sched_policy {
    enum sched_class class;
    uint32_t weight;    /* fair: [1, SCHED_WEIGHT_MAX], its share of a CPU against the others' */
    uint64_t request_ns; /* fair: [SCHED_REQUEST_MIN_NS, SCHED_REQUEST_MAX_NS] */
    uint64_t period_ns; /* deadline: [SCHED_PERIOD_MIN_NS, SCHED_PERIOD_MAX_NS] */
};

#define SCHED_WEIGHT_DEFAULT 1024u
#define SCHED_WEIGHT_MAX (1u << 20)
#define SCHED_REQUEST_DEFAULT_NS 3000000ull
#define SCHED_REQUEST_MIN_NS 100000ull
#define SCHED_REQUEST_MAX_NS 100000000ull
/* The periods a deadline thread may state: from a device's fastest cycle a CPU can still keep to
   (a 125 us USB microframe) to a second. */
#define SCHED_PERIOD_MIN_NS 125000ull
#define SCHED_PERIOD_MAX_NS 1000000000ull

static inline struct sched_policy sched_fair(uint32_t weight, uint64_t request_ns) {
    return (struct sched_policy){.class = SCHED_FAIR, .weight = weight, .request_ns = request_ns};
}
static inline struct sched_policy sched_deadline(uint64_t period_ns) {
    return (struct sched_policy){.class = SCHED_DEADLINE, .weight = SCHED_WEIGHT_DEFAULT,
                                 .request_ns = SCHED_REQUEST_DEFAULT_NS, .period_ns = period_ns};
}

/* A deadline, a moment on the monotonic clock (ns); 0 is none, and comes after every deadline. */
static inline bool sched_sooner(uint64_t a, uint64_t b) { return a && (!b || a < b); }

/* A thread's part in the scheduler (struct thread's sched). The run queue sched.cpu names guards
   it, but where marked. */
struct sched_entity {
    enum thread_state state;   /* see enum thread_state */
    bool idle;                 /* a CPU's idle thread: never queued, never moved */
    bool on_cpu;               /* a CPU runs it, from that CPU's decision until it has switched away (read without a lock) */
    bool queued;               /* in its queue's tree, waiting for the CPU */
    bool throttled;            /* deadline, its reservation spent: in the fair class until its deadline */
    unsigned cpu;              /* its queue: where it runs, waits, or ran last */
    unsigned bound;            /* the CPU index + 1 it runs on alone (sched_thread_bind); 0: any */
    unsigned fpu_cpu;          /* the CPU index + 1 whose registers hold its extended state as loaded last (its own CPU's) */
    uint64_t cpu_ns;
    struct sched_policy policy;
    /* The deadline it inherits from the waiters of the mutexes it holds (sys/wait.c), 0 for none:
       while sooner than its own, it runs in the deadline class by it, and is not throttled. Written
       under its pi lock and its queue's lock (sched_set_boost). */
    uint64_t boost;
    struct avl_node node;      /* in its queue's deadline or fair tree while queued */
    uint64_t seq;              /* its place among its equals in that tree */
    uint64_t vruntime, deadline, min_deadline; /* fair: virtual times; min_deadline, its subtree's earliest */
    int64_t lag_ns;            /* fair, while in no queue: the service it is owed (positive) or got ahead */
    /* deadline: its job's deadline, the reservation left in this period, the reservation, what the
       job running now has used, and the recent peak of what a job used */
    uint64_t dl_due, dl_left, dl_reserve, dl_job, dl_peak;
    struct alarm replenish;    /* deadline, throttled: its deadline, when its next period begins */
};

/* The deadline e runs by and lends to the holder of a mutex it waits for: its job's when it is a
   deadline thread, or the one it inherits, whichever is sooner; 0 for a fair thread that inherits
   none. */
static inline uint64_t sched_urgency(const struct sched_entity *e) {
    uint64_t own = e->policy.class == SCHED_DEADLINE ? e->dl_due : 0;
    return sched_sooner(e->boost, own) ? e->boost : own;
}

extern PERCPU struct thread *sched_thread; /* the thread this CPU runs (sys/thread.h: thread_current) */

/* Each CPU has a run queue and an idle thread. A thread runs on the CPU whose queue holds it; a woken
   one goes to an idle CPU near the one it ran on (a deadline one, else to a CPU that runs no
   deadline thread), a CPU going idle takes a waiting thread from the nearest busy queue, and a busy
   CPU looks for an idler one when its thread's slice ends. The kernel is preempted where nothing
   holds that off (arch/preempt.h). */
void sched_init(void);                 /* CPU 0, at boot: its queue, the boot context as its idle thread */
bool sched_cpu_prepare(unsigned cpu);  /* CPU 0, before that CPU starts: its queue and idle thread */
/* The top of cpu's idle thread's stack, a kernel stack as every thread's, on the CPU's home node: a
   CPU moves onto it from the bootloader's (arch/cpu.h), and starts on it again after a sleep. */
uint64_t sched_cpu_stack(unsigned cpu);
NORETURN void sched_cpu_start(void);   /* a CPU starting: its idle thread from here on */
NORETURN void sched_idle(void);        /* the idle thread's loop (CPU 0's after boot) */
bool sched_cpu_empty(unsigned cpu);    /* its queue holds no thread (a hint) */
void sched_cpu_leave(unsigned cpu);    /* for a sleep: its threads go to CPU 0, and none come */
/* A new thread's part (sys/thread.c), before it can be made ready: in the fair class, by default. */
void sched_thread_add(struct thread *t);
/* t, added and not made ready yet, runs on that CPU alone (kthread_create_on). */
void sched_thread_bind(struct thread *t, unsigned cpu);
/* An added thread that ended, once its CPU has left it (thread_left), or that never ran, leaves the
   scheduler: its stacks and record may go then. */
void sched_thread_remove(struct thread *t);
/* Any CPU: t runs in that class from here on (a running or waiting one at once). A deadline thread
   that states another period keeps the reservation it has measured, scaled to the new period. */
void sched_set_policy(struct thread *t, struct sched_policy policy);
/* Any CPU, t's pi lock held (sys/wait.c, priority inheritance): t's inherited deadline becomes
   boost (sched_entity's boost), at once where it runs or waits to. */
void sched_set_boost(struct thread *t, uint64_t boost);
/* Any CPU, any context: a new thread, or a blocked one its waker took out of its wait queue. */
void sched_make_ready(struct thread *t);
void schedule(void);
void sched_yield(void);
void sched_preempt_irq(void); /* arch/isr.asm, on the way out of an interrupt into preemptible code */
/* t's CPU time so far, the running stretch included (any CPU). */
uint64_t sched_thread_cpu_ns(struct thread *t);
void sched_user_return(struct regs *r);
void sched_post_switch(void);   /* context_switch, on the new thread's stack */
/* Counts since boot. throttles: deadline threads that spent their reservation; late: jobs of
   deadline threads that ended after their deadline. */
struct sched_stats { uint64_t switches, wakes, moved_wakes, steals, balances, kicks, yields, boosts, throttles, late; };
void sched_get_stats(struct sched_stats *out);
