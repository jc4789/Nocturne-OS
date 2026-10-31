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

/* The two classes. A realtime thread runs before every fair one: the higher priority first, and
   among one priority's threads the first that became ready (a preempted one keeps its place at the
   head). In each period it may run budget_ns in its class; once that is spent it runs in the fair
   class for the rest of the period, so that no realtime thread starves the others.
   A fair thread gets CPU time in proportion to its weight, by EEVDF (Stoica and Abdel-Wahab,
   "Earliest Eligible Virtual Deadline First", 1995): each thread asks for request_ns at a time; of
   the eligible threads (those not ahead of their share) the one whose request ends first runs, and
   a woken one keeps the lag it left with. A short request gets the CPU sooner, a long one keeps it
   longer. */
enum sched_class { SCHED_FAIR, SCHED_REALTIME };

struct sched_policy {
    enum sched_class class;
    unsigned priority;             /* realtime: the higher runs first (any value) */
    uint32_t weight;               /* [1, SCHED_WEIGHT_MAX]: its share of a CPU against the others' */
    uint64_t request_ns;           /* [SCHED_REQUEST_MIN_NS, SCHED_REQUEST_MAX_NS] */
    uint64_t budget_ns, period_ns; /* realtime: 0 < budget_ns <= period_ns */
};

#define SCHED_WEIGHT_DEFAULT 1024u
#define SCHED_WEIGHT_MAX (1u << 20)
#define SCHED_REQUEST_DEFAULT_NS 3000000ull
#define SCHED_REQUEST_MIN_NS 100000ull
#define SCHED_REQUEST_MAX_NS 100000000ull
#define SCHED_RT_PERIOD_NS 10000000ull
#define SCHED_RT_BUDGET_NS 8000000ull

/* The realtime priorities of the kernel's own threads: an interrupt's thread first (its run is
   short, and a device waits for it), then sound (an underrun is heard), input, the compositor. */
#define SCHED_PRIORITY_COMPOSITOR 10
#define SCHED_PRIORITY_INPUT 20
#define SCHED_PRIORITY_SOUND 30
#define SCHED_PRIORITY_IRQ 40

static inline struct sched_policy sched_realtime(unsigned priority) {
    return (struct sched_policy){.class = SCHED_REALTIME, .priority = priority, .weight = SCHED_WEIGHT_DEFAULT,
                                 .request_ns = SCHED_REQUEST_DEFAULT_NS, .budget_ns = SCHED_RT_BUDGET_NS,
                                 .period_ns = SCHED_RT_PERIOD_NS};
}

/* A thread's part in the scheduler (struct thread's sched). The run queue sched.cpu names guards
   it, but where marked. */
struct sched_entity {
    enum thread_state state;   /* see enum thread_state */
    bool idle;                 /* a CPU's idle thread: never queued, never moved */
    bool on_cpu;               /* a CPU runs it, from that CPU's decision until it has switched away (read without a lock) */
    bool queued;               /* in its queue's tree, waiting for the CPU */
    bool throttled;            /* realtime, its budget spent: in the fair class until its period ends */
    unsigned cpu;              /* its queue: where it runs, waits, or ran last */
    unsigned bound;            /* the CPU index + 1 it runs on alone (sched_thread_bind); 0: any */
    unsigned fpu_cpu;          /* the CPU index + 1 whose registers hold its extended state as loaded last (its own CPU's) */
    uint64_t cpu_ns;
    struct sched_policy policy;
    /* The realtime rank it inherits from the waiters of the mutexes it holds (sys/wait.c), 0 for
       none: while above its own (sched_rank), it runs in the realtime class at that rank, on its own
       budget, or a realtime thread's default one when it is a fair thread. Written under its pi lock
       and its queue's lock (sched_set_boost). */
    unsigned boost;
    struct avl_node node;      /* in its queue's realtime or fair tree while queued */
    uint64_t seq;              /* its place among its equals in that tree */
    uint64_t vruntime, deadline, min_deadline; /* fair: virtual times; min_deadline, its subtree's earliest */
    int64_t lag_ns;            /* fair, while in no queue: the service it is owed (positive) or got ahead */
    uint64_t rt_used, rt_period_end; /* realtime: its run in this period, and when the period ends */
    struct alarm replenish;    /* realtime, throttled: its period's end */
};

/* The realtime rank e runs at and lends to the holder of a mutex it waits for: its own priority + 1
   when it is a realtime thread, or the rank it inherits, whichever is higher; 0 for a fair thread
   that inherits none. */
static inline unsigned sched_rank(const struct sched_entity *e) {
    unsigned own = e->policy.class == SCHED_REALTIME ? e->policy.priority + 1 : 0;
    return own > e->boost ? own : e->boost;
}

extern PERCPU struct thread *sched_thread; /* the thread this CPU runs (sys/thread.h: thread_current) */

/* Each CPU has a run queue and an idle thread. A thread runs on the CPU whose queue holds it; a woken
   one goes to an idle CPU near the one it ran on, a CPU going idle takes a waiting thread from the
   nearest busy queue, and a busy CPU looks for an idler one when its thread's slice ends. The kernel
   is preempted where nothing holds that off (arch/preempt.h). */
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
/* Any CPU: t runs in that class from here on (a running or waiting one at once). */
void sched_set_policy(struct thread *t, struct sched_policy policy);
/* Any CPU, t's pi lock held (sys/wait.c, priority inheritance): t's inherited rank becomes boost
   (sched_entity's boost), at once where it runs or waits to. */
void sched_set_boost(struct thread *t, unsigned boost);
/* Any CPU, any context: a new thread, or a blocked one its waker took out of its wait queue. */
void sched_make_ready(struct thread *t);
void schedule(void);
void sched_yield(void);
void sched_preempt_irq(void); /* arch/isr.asm, on the way out of an interrupt into preemptible code */
/* t's CPU time so far, the running stretch included (any CPU). */
uint64_t sched_thread_cpu_ns(struct thread *t);
void sched_user_return(struct regs *r);
void sched_post_switch(void);   /* context_switch, on the new thread's stack */
struct sched_stats { uint64_t switches, wakes, moved_wakes, steals, balances, kicks, yields, boosts; };
void sched_get_stats(struct sched_stats *out);
