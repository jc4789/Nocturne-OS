/* Spinlocks: the kernel's short exclusion between CPUs (and with interrupt handlers on the
   holder's own CPU, whose interrupts stay off while it holds the lock).

   A queued lock (MCS): a CPU that has to wait joins the lock's queue and spins on a word of its
   own node, so the CPUs waiting take the lock in the order they came and none of them pulls the
   lock's line from the others while it waits. Only the CPU at the head of the queue watches the
   lock itself (holder), for the moment it is given back; once it has taken it, it hands the head
   on to the next node and is done with its own. A node is therefore in use only while its CPU
   waits, and a CPU waits on more than one lock at a time only where an NMI or a machine check
   came in during a wait and takes a lock itself: SPIN_NODES nodes a CPU.

   Holders never sleep and never wait for another CPU: a TLB shootdown, which waits for the
   others, is never sent under a spinlock (spins_held counts the ones this CPU holds), and its
   sender waits with interrupts on and no spinlock, so a CPU waiting here need not answer it.

   The order locks are taken in is checked where their owners rank them (struct spinlock's rank,
   from enum spin_rank below): a CPU may take a ranked lock only above every ranked lock it holds,
   and panics otherwise. 0 is not checked. An NMI's or a machine check's handler, which may come in
   while anything is held, is not checked either. */
#pragma once
#include "arch/cpu.h"
#include "arch/preempt.h"
#include "arch/smp.h"

/* The ranks, lowest first: a lock is taken only while every ranked lock this CPU holds is lower.
   Each owner names its locks here, in the order they nest. */
enum spin_rank {
    SPIN_UNRANKED, /* not checked */
    SPIN_RANKS
};

struct spinlock {
    uint32_t tail;   /* the last waiter's node (spin_node_id), 0 with nobody queued */
    uint32_t holder; /* CPU index + 1 while taken: a CPU taking a lock it holds stops at once */
    uint32_t rank;   /* enum spin_rank */
};

#define SPIN_NODE_BITS 2
#define SPIN_NODES (1u << SPIN_NODE_BITS) /* a wait, and an NMI's and a machine check's in it, and one spare */
#define SPIN_RANK_WORDS ((SPIN_RANKS + 63) / 64)

struct spin_node {
    struct spin_node *next; /* the waiter queued behind this one */
    uint32_t wait;          /* 1 until the node before hands this one the head of the queue */
} __attribute__((aligned(64)));

/* Each CPU's (arch/smp.c): its nodes, how many of them its waits use now, the spinlocks it holds,
   and the ranks among them (a bit each). */
extern PERCPU struct spin_node spin_nodes[SPIN_NODES];
extern PERCPU uint32_t spin_waits;
extern PERCPU uint32_t spins_held;
extern PERCPU uint64_t spin_ranks_held[SPIN_RANK_WORDS];

__attribute__((noreturn)) void spin_fault(const struct spinlock *l, const char *why); /* arch/smp.c */

static inline uint32_t spin_node_id(unsigned cpu, uint32_t node) { return (uint32_t)(cpu + 1) << SPIN_NODE_BITS | node; }
static inline struct spin_node *spin_node_at(uint32_t id) {
    return &per_cpu(spin_nodes, (id >> SPIN_NODE_BITS) - 1)[id & (SPIN_NODES - 1)];
}

/* The highest rank this CPU holds, 0 with none. */
static inline unsigned spin_rank_top(void) {
    for (unsigned w = SPIN_RANK_WORDS; w--;) {
        uint64_t bits = this_cpu(spin_ranks_held)[w];
        if (bits) return w * 64 + 63 - (unsigned)__builtin_clzll(bits);
    }
    return 0;
}

static inline void spin_take(struct spinlock *l) {
    preempt_disable(); /* first: what follows is this CPU's */
    unsigned self = cpu_current_index();
    if (__atomic_load_n(&l->holder, __ATOMIC_RELAXED) == self + 1) spin_fault(l, "taken again by its holder");
    bool ranked = l->rank && !in_nmi();
    if (ranked) {
        if (l->rank <= spin_rank_top()) spin_fault(l, "taken out of rank order");
        this_cpu(spin_ranks_held)[l->rank / 64] |= 1ULL << (l->rank % 64);
    }
    uint32_t node = this_cpu(spin_waits)++;
    if (node >= SPIN_NODES) spin_fault(l, "waited for inside too many waits");
    struct spin_node *me = &(*this_cpu_ptr(spin_nodes))[node];
    me->next = NULL;
    me->wait = 1;
    uint32_t mine = spin_node_id(self, node);
    uint32_t before = __atomic_exchange_n(&l->tail, mine, __ATOMIC_ACQ_REL);
    if (before) {
        __atomic_store_n(&spin_node_at(before)->next, me, __ATOMIC_RELEASE);
        while (__atomic_load_n(&me->wait, __ATOMIC_ACQUIRE)) pause();
    }
    /* the head of the queue: the lock is this CPU's once its holder gives it back */
    while (__atomic_load_n(&l->holder, __ATOMIC_ACQUIRE)) pause();
    __atomic_store_n(&l->holder, self + 1, __ATOMIC_RELAXED);
    /* the head to the next waiter, after holder (a store before this one) shows it taken */
    uint32_t expect = mine;
    if (!__atomic_compare_exchange_n(&l->tail, &expect, 0, false, __ATOMIC_ACQ_REL, __ATOMIC_RELAXED)) {
        struct spin_node *next;
        while (!(next = __atomic_load_n(&me->next, __ATOMIC_ACQUIRE))) pause();
        __atomic_store_n(&next->wait, 0, __ATOMIC_RELEASE);
    }
    this_cpu(spin_waits)--;
    this_cpu(spins_held)++;
}
/* As spin_take, but only when nobody holds the lock or waits for it: true once this CPU took it;
   false, nothing taken and no wait, otherwise. For a lock wanted out of its order (the walk along
   the holders of mutexes, sys/wait.c), where waiting for it could wait for a CPU waiting for this
   one: the caller lets go of what it holds and tries again. The order check is not made. */
static inline bool spin_try(struct spinlock *l) {
    preempt_disable();
    unsigned self = cpu_current_index();
    if (__atomic_load_n(&l->holder, __ATOMIC_RELAXED) == self + 1) spin_fault(l, "taken again by its holder");
    if (__atomic_load_n(&l->tail, __ATOMIC_RELAXED) || __atomic_load_n(&l->holder, __ATOMIC_RELAXED)) {
        preempt_enable();
        return false;
    }
    uint32_t node = this_cpu(spin_waits)++;
    if (node >= SPIN_NODES) spin_fault(l, "waited for inside too many waits");
    struct spin_node *me = &(*this_cpu_ptr(spin_nodes))[node];
    me->next = NULL;
    me->wait = 0;
    uint32_t mine = spin_node_id(self, node), expect = 0;
    /* At the head of an empty queue, none behind can take it before this CPU hands the head on. */
    bool taken = __atomic_compare_exchange_n(&l->tail, &expect, mine, false, __ATOMIC_ACQ_REL, __ATOMIC_RELAXED);
    if (taken) {
        taken = !__atomic_load_n(&l->holder, __ATOMIC_ACQUIRE); /* held by one already out of the queue */
        if (taken) __atomic_store_n(&l->holder, self + 1, __ATOMIC_RELAXED);
        /* the head to the next waiter (it waits for holder, which shows taken or not) */
        expect = mine;
        if (!__atomic_compare_exchange_n(&l->tail, &expect, 0, false, __ATOMIC_ACQ_REL, __ATOMIC_RELAXED)) {
            struct spin_node *next;
            while (!(next = __atomic_load_n(&me->next, __ATOMIC_ACQUIRE))) pause();
            __atomic_store_n(&next->wait, 0, __ATOMIC_RELEASE);
        }
    }
    this_cpu(spin_waits)--;
    if (!taken) {
        preempt_enable();
        return false;
    }
    if (l->rank && !in_nmi()) this_cpu(spin_ranks_held)[l->rank / 64] |= 1ULL << (l->rank % 64);
    this_cpu(spins_held)++;
    return true;
}
static inline void spin_give(struct spinlock *l) {
    this_cpu(spins_held)--;
    if (l->rank && !in_nmi()) this_cpu(spin_ranks_held)[l->rank / 64] &= ~(1ULL << (l->rank % 64));
    __atomic_store_n(&l->holder, 0, __ATOMIC_RELEASE);
    preempt_enable();
}
/* Interrupts off until spin_unlock, which gives back the state it found. */
static inline uint64_t spin_lock(struct spinlock *l) {
    uint64_t flags = irq_save();
    spin_take(l);
    return flags;
}
static inline void spin_unlock(struct spinlock *l, uint64_t flags) {
    spin_give(l);
    irq_restore(flags);
}
/* A panic, every other CPU stopped: the lock is free again, whoever held it or waited for it,
   and this CPU starts its order over (it never gives back what it held). */
static inline void spin_seize(struct spinlock *l) {
    for (unsigned w = 0; w < SPIN_RANK_WORDS; w++) this_cpu(spin_ranks_held)[w] = 0;
    __atomic_store_n(&l->tail, 0, __ATOMIC_RELAXED);
    __atomic_store_n(&l->holder, 0, __ATOMIC_RELEASE);
}
static inline bool spin_held(const struct spinlock *l) {
    return __atomic_load_n(&l->holder, __ATOMIC_RELAXED) == cpu_current_index() + 1;
}
/* This CPU neither holds a spinlock nor waits for one: an NMI's or a machine check's handler,
   which comes in anywhere, may take one (the log's) only then. */
static inline bool spin_none_here(void) { return !this_cpu(spins_held) && !this_cpu(spin_waits); }
