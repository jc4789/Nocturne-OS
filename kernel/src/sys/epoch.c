/* Epochs (sys/epoch.h): grace periods counted by last-level cache group, and their workers. */
#include "kernel.h"
#include "arch/cpu.h"
#include "arch/percpu.h"
#include "arch/smp.h"
#include "arch/spin.h"
#include "arch/topology.h"
#include "mm/heap.h"
#include "sys/clock.h"
#include "sys/epoch.h"
#include "sys/sched.h"
#include "sys/thread.h"
#include "sys/wait.h"

/* How long a grace period lets its CPUs pass a quiet point on their own before it kicks those that
   have not: a policy, not a measure. Longer, and what is deferred meanwhile piles up (at a million
   deferrals a second, ten thousand of them); shorter, and a CPU that runs one thread alone is
   interrupted more often while something waits to go back. */
#define EPOCH_FORCE_NS (10 * NS_PER_MS)

/* A CPU's part. Changed by that CPU alone, under its group's lock (its quiet point reads it first
   without); a late grace period's kick (gp_force) reads it from other CPUs. */
struct epoch_cpu {
    uint64_t seen; /* the grace period it last paid, or came out of idle owing nothing for */
    bool idle;     /* out of its group's count: in its idle loop, stopped, or never started */
};
PERCPU struct epoch_cpu epoch_cpu = {.idle = true};

struct epoch_group {
    struct spinlock lock; /* active, owing and gp, and its CPUs' seen and idle */
    unsigned active;      /* its CPUs out of idle */
    unsigned owing;       /* of those that were out of idle when gp began here, the ones not paid */
    /* The grace period it is in: the last begun, or the one before while that one's beginning
       (gp_begin) has not reached this group yet. */
    uint64_t gp;
    const unsigned *cpu;  /* its CPUs, by topology's index */
    unsigned cpus;
    /* What its CPUs deferred that its worker has not taken: pushed by any CPU, taken whole by the
       worker, which sleeps on the word while it holds nothing. */
    union {
        struct epoch_item *head;
        uint64_t word;
    } pushed;
    struct wait_queue wake; /* its worker, holding nothing */
};

static struct {
    /* Read at every quiet point: a line of its own, written once a grace period. */
    uint64_t started __attribute__((aligned(64))); /* the last grace period begun (lock; atomic) */
    uint64_t done __attribute__((aligned(64)));    /* the last ended (lock; atomic) */
    unsigned groups_owing;    /* the groups not paid in started (atomic) */
    uint64_t started_at;      /* when started began (atomic) */
    uint64_t forced;          /* the last grace period whose late CPUs were kicked (atomic) */
    struct spinlock lock;     /* begins and ends grace periods: a leaf */
    struct wait_queue ended;  /* workers waiting for a grace period's end */
    struct epoch_group **group; /* [topology.llcs] */
} epoch;

/* Grace period gp has ended: whoever waits for it is woken. Any context but a run queue's lock held. */
static void gp_end(uint64_t gp) {
    uint64_t f = spin_lock(&epoch.lock);
    __atomic_store_n(&epoch.done, gp, __ATOMIC_RELEASE);
    spin_unlock(&epoch.lock, f);
    wq_wake_all(&epoch.ended);
}

/* A group paid gp: the last one ends it. */
static void group_paid(uint64_t gp) {
    if (!__atomic_sub_fetch(&epoch.groups_owing, 1, __ATOMIC_ACQ_REL)) gp_end(gp);
}

/* A worker: grace period target is to end. It begins unless it has, or one runs (target is then at
   most the one after it, and begins once that one ended). */
static void gp_begin(uint64_t target) {
    uint64_t f = spin_lock(&epoch.lock);
    uint64_t gp = epoch.done + 1;
    bool begin = epoch.started == epoch.done && gp <= target;
    if (begin) {
        __atomic_store_n(&epoch.groups_owing, topology.llcs, __ATOMIC_RELAXED);
        __atomic_store_n(&epoch.started_at, clock_now_ns(), __ATOMIC_RELAXED);
        /* after the counts it starts: a CPU that reads it finds them so */
        __atomic_store_n(&epoch.started, gp, __ATOMIC_SEQ_CST);
    }
    spin_unlock(&epoch.lock, f);
    if (!begin) return;
    /* Each group takes it in, and its CPUs out of idle owe it. Until a group has, its CPUs' quiet
       points find it behind and look again: the walk holds preemption off. */
    preempt_disable();
    for (unsigned i = 0; i < topology.llcs; i++) {
        struct epoch_group *g = epoch.group[i];
        f = spin_lock(&g->lock);
        g->gp = gp;
        g->owing = g->active;
        bool paid = !g->owing;
        spin_unlock(&g->lock, f);
        if (paid) group_paid(gp);
    }
    preempt_enable();
}

/* A worker: grace period gp is late. Once a grace period, each CPU that still owes it is told a
   switch is due (sched_resched, written without its queue's lock: at worst its own switch clears
   it as it passes the quiet point anyway) and kicked: from ring 3 it comes back through
   sched_user_return, in the kernel it switches where preemption is next on. The worker's own CPU
   never owes: it passed a quiet point as it switched to the worker. */
static void gp_force(uint64_t gp) {
    uint64_t forced = __atomic_load_n(&epoch.forced, __ATOMIC_RELAXED);
    do
        if (forced >= gp) return;
    while (!__atomic_compare_exchange_n(&epoch.forced, &forced, gp, false, __ATOMIC_RELAXED, __ATOMIC_RELAXED));
    for (unsigned i = 0; i < topology.llcs; i++) {
        struct epoch_group *g = epoch.group[i];
        uint64_t f = spin_lock(&g->lock);
        bool late = g->gp == gp && g->owing;
        spin_unlock(&g->lock, f);
        for (unsigned k = 0; late && k < g->cpus; k++) {
            const struct epoch_cpu *c = &per_cpu(epoch_cpu, g->cpu[k]);
            if (!__atomic_load_n(&c->idle, __ATOMIC_RELAXED) && __atomic_load_n(&c->seen, __ATOMIC_RELAXED) != gp) {
                per_cpu(sched_resched, g->cpu[k]) = true;
                cpu_kick(g->cpu[k]);
            }
        }
    }
}

void epoch_quiet(bool idle) {
    struct epoch_cpu *c = this_cpu_ptr(epoch_cpu);
    if (c->idle == idle && (idle || c->seen == __atomic_load_n(&epoch.started, __ATOMIC_ACQUIRE))) return;
    struct epoch_group *g = epoch.group[topology.cpu[cpu_current_index()].llc];
    bool paid = false;
    uint64_t f = spin_lock(&g->lock);
    if (!c->idle) {
        /* out of idle when g->gp began here, so counted in it, and not paid yet */
        if (c->seen != g->gp) {
            __atomic_store_n(&c->seen, g->gp, __ATOMIC_RELAXED);
            paid = !--g->owing;
        }
        if (idle) {
            g->active--;
            __atomic_store_n(&c->idle, true, __ATOMIC_RELAXED);
        }
    } else if (!idle) {
        /* Counted from the next grace period on: what the running one gives back was out of reach
           before it began here, so before this lock was taken. */
        g->active++;
        __atomic_store_n(&c->seen, g->gp, __ATOMIC_RELAXED);
        __atomic_store_n(&c->idle, false, __ATOMIC_RELAXED);
    }
    uint64_t gp = g->gp;
    spin_unlock(&g->lock, f);
    if (paid) group_paid(gp);
}

void epoch_defer(struct epoch_item *item, void (*free)(struct epoch_item *item)) {
    item->free = free;
    /* The writer's unlink is seen by every CPU before started is read: a grace period begun after
       that reading counts only CPUs that will not reach the item, or pay before it goes. */
    __atomic_thread_fence(__ATOMIC_SEQ_CST);
    item->gp = __atomic_load_n(&epoch.started, __ATOMIC_SEQ_CST) + 1;
    /* this CPU's group, or another's if the thread moves meanwhile: any worker gives it back */
    struct epoch_group *g = epoch.group[topology.cpu[cpu_current_index()].llc];
    struct epoch_item *head = __atomic_load_n(&g->pushed.head, __ATOMIC_RELAXED);
    do
        item->next = head;
    while (!__atomic_compare_exchange_n(&g->pushed.head, &head, item, true, __ATOMIC_RELEASE, __ATOMIC_RELAXED));
    if (!head) wq_wake_all(&g->wake); /* its worker may be asleep, holding nothing */
}

/* A group's worker. It holds what it took in no order (the grace periods two CPUs read may cross),
   and walks it once for each grace period that ends. */
static void worker(void *arg) {
    struct epoch_group *g = arg;
    struct epoch_item *held = NULL;
    for (;;) {
        struct epoch_item *got = __atomic_exchange_n(&g->pushed.head, NULL, __ATOMIC_ACQUIRE);
        while (got) {
            struct epoch_item *next = got->next;
            got->next = held;
            held = got;
            got = next;
        }
        uint64_t done = __atomic_load_n(&epoch.done, __ATOMIC_ACQUIRE), need = UINT64_MAX;
        for (struct epoch_item **at = &held; *at;) {
            struct epoch_item *item = *at;
            if (item->gp > done) {
                need = MIN(need, item->gp);
                at = &item->next;
                continue;
            }
            *at = item->next;
            item->free(item); /* preemptible between frees, like any kernel thread */
        }
        if (!held) {
            wq_wait_unless(&g->wake, &g->pushed.word, 0, CLOCK_NEVER);
            continue;
        }
        gp_begin(need);
        /* until a grace period ends; one late by EPOCH_FORCE_NS has its late CPUs kicked, once */
        uint64_t started = __atomic_load_n(&epoch.started, __ATOMIC_ACQUIRE), deadline = CLOCK_NEVER;
        if (started > done && __atomic_load_n(&epoch.forced, __ATOMIC_RELAXED) < started)
            deadline = __atomic_load_n(&epoch.started_at, __ATOMIC_RELAXED) + EPOCH_FORCE_NS;
        if (!wq_wait_unless(&epoch.ended, &epoch.done, done, deadline)) gp_force(started);
    }
}

void epoch_init(void) {
    unsigned groups = topology.llcs;
    epoch.group = kzalloc(groups * sizeof *epoch.group);
    unsigned *cpus = kmalloc(topology.cpus * sizeof *cpus);
    if (!epoch.group || !cpus) panic("epoch: no memory for %u groups", groups);
    unsigned at = 0;
    for (unsigned i = 0; i < groups; i++) {
        struct epoch_group *g = kzalloc(sizeof *g);
        if (!g) panic("epoch: no memory for group %u", i);
        g->cpu = &cpus[at];
        for (unsigned c = 0; c < topology.cpus; c++)
            if (topology.cpu[c].llc == i) cpus[at++] = c;
        g->cpus = (unsigned)(&cpus[at] - g->cpu);
        epoch.group[i] = g;
    }
    for (unsigned i = 0; i < groups; i++) {
        char name[16];
        ksnprintf(name, sizeof name, "epoch%u", i);
        if (!kthread_create(name, worker, epoch.group[i])) panic("epoch: no worker for group %u", i);
    }
    /* the boot CPU out of idle: the kernel's start runs on it, outside any thread's switch */
    preempt_disable();
    epoch_quiet(false);
    preempt_enable();
}
