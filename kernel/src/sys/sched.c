/* The scheduler. Each CPU has a run queue and an idle thread, and switches among its queue's
   threads by itself.

   A queue holds its own threads only, in two trees (lib/avl.h): its realtime threads by priority
   and then by the order they came, and its fair threads by virtual time, each node knowing its
   subtree's earliest deadline, so that the eligible thread whose request ends first is found in
   O(log n). Its lock guards both trees, the sums its virtual time comes from, and the scheduler's
   part of each thread whose sched.cpu names it. The CPU that switches holds its queue's lock from
   the decision until it runs on the next thread's stack (sched_post_switch): no other CPU acts on
   the thread it left before that thread's context is saved. Two queues' locks are taken in CPU
   order. thread_lock (sys/thread.h) comes before a wait queue's lock, that before a run queue's,
   and that before a CPU's alarm lock. Every one of them is held with interrupts off.

   Placement and balance follow the topology (arch/topology.h): a core's SMT threads, the CPUs
   sharing a last-level cache, a node's caches, every node. Each cache, node and the whole keep
   hints, a bit for each member that is idle (a CPU in its idle loop with nothing queued) or busy (a
   queue with a waiting thread that may move), which a CPU sets and clears for itself under its own
   lock; a group's bit in its parent says one of its members has it. A woken thread goes back to the
   CPU it ran on if that is idle, else to an idle CPU of the same cache (a whole idle core first),
   else back to its CPU. A CPU going idle takes a waiting thread from its core, its cache, its node,
   then any node, by the busy hints. A busy CPU (a thread waiting behind the one it runs) looks at
   its slice's end for an idle CPU or one with two threads fewer: in its core and its cache each
   time, in its node and beyond more rarely (look_every). No path looks at every CPU, and nothing
   looks at intervals of its own.

   Preemption (arch/preempt.h): a CPU switches where nothing holds preemption off, once
   sched_resched says a switch is due: when its count comes back to 0 with interrupts on
   (preempt_enable), when its interrupts come back on with the count at 0 (irq_restore), on the way
   out of an interrupt into the kernel (sched_preempt_irq) and on the way back to ring 3
   (sched_user_return). A CPU's flag is set under its queue's lock, by itself or by a waker that
   kicks it after; a late grace period sets it without (sys/epoch.c), to have the CPU pass the
   scheduler once.

   The scheduler passes epoch's quiet points (sys/epoch.h): every switch, every way back to ring
   3, and the idle loop as it begins to wait, which takes the CPU out of the count until its next
   switch. */
#include "kernel.h"
#include "arch/cpu.h"
#include "arch/cpuset.h"
#include "arch/fpu.h"
#include "arch/gdt.h"
#include "arch/idle.h"
#include "arch/irq.h"
#include "arch/preempt.h"
#include "arch/smp.h"
#include "mm/heap.h"
#include "mm/vmm.h"
#include "sys/alarm.h"
#include "sys/clock.h"
#include "sys/epoch.h"
#include "sys/process.h"
#include "sys/sched.h"
#include "sys/thread.h"

extern void context_switch(uint64_t *old_rsp, uint64_t new_rsp);

#define W_ONE SCHED_WEIGHT_DEFAULT /* the weight whose virtual time runs at real time's rate */
#define RFLAGS_IF 0x200

/* The topology's levels, nearest first, and how long a busy CPU leaves between its looks at each
   (0: at every slice's end): the farther, the dearer a move. */
enum level { LEVEL_CORE, LEVEL_LLC, LEVEL_NODE, LEVEL_ALL, LEVELS };
static const uint64_t look_every[LEVELS] = {0, 0, 4 * SCHED_REQUEST_DEFAULT_NS, 16 * SCHED_REQUEST_DEFAULT_NS};

enum hint { HINT_IDLE, HINT_BUSY, HINTS };

/* A cache's CPUs, a node's caches, or every node: a hint bit for each member, by its position, and
   the group's own place in its parent. */
struct sched_group {
    struct sched_group *parent;  /* NULL for the whole */
    unsigned at;                 /* its position among its parent's members */
    unsigned count;
    const unsigned *cpu;         /* a cache's members, by CPU index; NULL above */
    struct sched_group **member; /* a node's caches, the whole's nodes */
    uint64_t *hint[HINTS];
    uint64_t *core_idle;         /* a cache's: its members whose whole core is idle */
};

/* How the thread a CPU runs comes to the scheduler: to wait (unless it was made ready again
   meanwhile), preempted (a realtime one keeps its place at the head of its priority), or yielding
   (behind its equals; a fair one gives up the rest of its request). */
enum switch_mode { SWITCH_WAIT, SWITCH_PREEMPT, SWITCH_YIELD };

PERCPU static struct run_queue {
    struct spinlock lock;
    struct thread *idle;
    struct thread *last;    /* the thread this CPU just switched away from, for sched_post_switch */
    struct thread *evicted; /* a leaving CPU's: the runnable thread it switched away from, for another queue */
    unsigned cpu;
    size_t nr;              /* its runnable threads but the idle one: those queued, and the one it runs */
    size_t movable;         /* its queued threads not bound to a CPU */
    uint64_t stamp;         /* what it runs was charged up to here (clock_now_ns) */
    struct avl_tree rt, fair;
    uint64_t rt_head, rt_tail, fair_seq;
    /* Its virtual time V, the mean of its fair threads' (the one it runs among them) by weight, is
       vbase + sum / weight, with 0 <= sum < weight (rebase). */
    uint64_t vbase;
    __int128 sum;
    uint64_t weight;
    struct alarm slice;     /* while a thread waits behind the one it runs: when that one's slice ends */
    bool slice_stale;       /* another CPU changed its threads: its slice alarm is due again, here */
    uint64_t looked[LEVELS];
    unsigned sample;        /* where its next sampled look at a group falls */
    struct sched_group *llc;
    unsigned llc_at;        /* its position in its cache */
    const unsigned *core;   /* its core's CPUs, itself among them */
    unsigned core_count;
    struct thread *fpu_owner; /* whose extended state its registers hold */
    bool fpu_live;          /* they hold it changed since its area took it: it ran in ring 3 since */
    bool prepared, leaving;
    bool idle_loop;         /* its idle thread is in its loop (CPU 0's, once the boot is done) */
    bool idle_hinted;       /* its bit in its cache's idle hint */
} __attribute__((aligned(64))) queue;

PERCPU struct thread *sched_thread;
PERCPU volatile bool sched_resched;

static struct sched_group whole;
static struct sched_stats stats;
#define STAT(f) __atomic_fetch_add(&stats.f, 1, __ATOMIC_RELAXED)
#define ENTITY(n) ((struct sched_entity *)((char *)(n) - __builtin_offsetof(struct sched_entity, node)))
#define OWNER(e) ((struct thread *)((char *)(e) - __builtin_offsetof(struct thread, sched)))

static void slice_fire(struct alarm *a, struct regs *r);
static void replenish_fire(struct alarm *a, struct regs *r);

/* ---- arithmetic ---- */

/* Virtual times and moments are compared by their difference, which may wrap. */
static bool earlier(uint64_t a, uint64_t b) { return (int64_t)(a - b) < 0; }

/* floor(n / d), d > 0, saturated to int64. The kernel links no 128-bit division: DIV takes RDX:RAX
   by a 64-bit operand when the quotient fits (as clock_muldiv, sys/clock.h). */
static int64_t sdiv(__int128 n, uint64_t d) {
    bool negative = n < 0;
    unsigned __int128 m = negative ? -(unsigned __int128)n : (unsigned __int128)n;
    uint64_t high = (uint64_t)(m >> 64), q, r;
    if (high >= d) return negative ? INT64_MIN : INT64_MAX;
    __asm__("divq %4" : "=a"(q), "=d"(r) : "a"((uint64_t)m), "d"(high), "rm"(d) : "cc");
    if (!negative) return q > (uint64_t)INT64_MAX ? INT64_MAX : (int64_t)q;
    if (r && ++q == 0) return INT64_MIN;
    return q > (uint64_t)INT64_MAX ? INT64_MIN : -(int64_t)q;
}

/* ---- queues ---- */

static struct run_queue *rq(unsigned cpu) { return &per_cpu(queue, cpu); }
static struct thread *curr(const struct run_queue *q) { return per_cpu(sched_thread, q->cpu); }
static size_t runnable(unsigned cpu) { return __atomic_load_n(&rq(cpu)->nr, __ATOMIC_RELAXED); }

static bool usable(unsigned cpu) {
    struct run_queue *q = rq(cpu);
    return __atomic_load_n(&q->prepared, __ATOMIC_ACQUIRE) && cpu_online_at(cpu) &&
           !__atomic_load_n(&q->leaving, __ATOMIC_ACQUIRE);
}

/* Its class but for its budget: realtime when its policy is, or when it inherits a rank. */
static bool realtime(const struct sched_entity *e) { return e->policy.class == SCHED_REALTIME || e->boost; }
/* The class it runs in now: a throttled realtime thread runs in the fair one. */
static bool fair(const struct sched_entity *e) { return !realtime(e) || e->throttled; }
/* A realtime thread's budget and period; a fair one that inherits a rank runs on the default ones. */
static uint64_t rt_budget(const struct sched_entity *e) {
    return e->policy.class == SCHED_REALTIME ? e->policy.budget_ns : SCHED_RT_BUDGET_NS;
}
static uint64_t rt_period(const struct sched_entity *e) {
    return e->policy.class == SCHED_REALTIME ? e->policy.period_ns : SCHED_RT_PERIOD_NS;
}

static void lock2(unsigned a, unsigned b) {
    spin_take(&rq(a < b ? a : b)->lock);
    if (a != b) spin_take(&rq(a < b ? b : a)->lock);
}
static void unlock2(unsigned a, unsigned b) {
    spin_give(&rq(a)->lock);
    if (a != b) spin_give(&rq(b)->lock);
}

/* Interrupts off: the queue t's sched.cpu names, locked (it may change until that lock is held). */
static struct run_queue *lock_thread(struct thread *t) {
    for (;;) {
        unsigned cpu = __atomic_load_n(&t->sched.cpu, __ATOMIC_ACQUIRE);
        struct run_queue *q = rq(cpu);
        spin_take(&q->lock);
        if (t->sched.cpu == cpu) return q;
        spin_give(&q->lock);
    }
}

/* ---- hints ---- */

static bool hint_any(const uint64_t *m, unsigned count) {
    for (size_t w = 0; w < cpuset_words(count); w++)
        if (__atomic_load_n(&m[w], __ATOMIC_SEQ_CST)) return true;
    return false;
}

/* A member's bit on, and its group's in each parent up to one that has it already. */
static void hint_add(struct sched_group *g, enum hint h, unsigned at) {
    for (;;) {
        cpuset_add(g->hint[h], at);
        struct sched_group *p = g->parent;
        if (!p || cpuset_has(p->hint[h], g->at)) return;
        at = g->at;
        g = p;
    }
}

/* A member's bit off, and its group's in its parent once no member has one; a member that took one
   meanwhile puts the group's back, so a group's bit never lacks while a member has one. */
static void hint_remove(struct sched_group *g, enum hint h, unsigned at) {
    cpuset_remove(g->hint[h], at);
    struct sched_group *p = g->parent;
    if (!p || hint_any(g->hint[h], g->count)) return;
    hint_remove(p, h, g->at);
    if (hint_any(g->hint[h], g->count)) hint_add(p, h, g->at);
}

static bool core_idle(const struct run_queue *q) {
    for (unsigned i = 0; i < q->core_count; i++)
        if (!__atomic_load_n(&rq(q->core[i])->idle_hinted, __ATOMIC_SEQ_CST)) return false;
    return true;
}

static void core_hint(const struct run_queue *q, bool idle) {
    for (unsigned i = 0; i < q->core_count; i++) {
        unsigned at = rq(q->core[i])->llc_at;
        if (idle) cpuset_add(q->llc->core_idle, at);
        else cpuset_remove(q->llc->core_idle, at);
    }
}

/* q's lock held: its CPU in its idle loop with nothing queued, or not, in its cache's hints. A core
   is wholly idle while each of its CPUs is: the last to go idle sets that, and looks again after,
   since a sibling that stopped being idle meanwhile cleared it. */
static void hint_idle(struct run_queue *q, bool idle) {
    idle = idle && q->idle_loop && !q->leaving;
    if (q->idle_hinted == idle) return;
    __atomic_store_n(&q->idle_hinted, idle, __ATOMIC_SEQ_CST);
    if (!idle) {
        hint_remove(q->llc, HINT_IDLE, q->llc_at);
        core_hint(q, false);
        return;
    }
    hint_add(q->llc, HINT_IDLE, q->llc_at);
    if (!core_idle(q)) return;
    core_hint(q, true);
    if (!core_idle(q)) core_hint(q, false);
}

/* The first member of g from start on, round the group, whose bit is set in m, other than the one
   at skip (g->count: none): its position, or g->count. A hinted CPU is usable but in a race, which
   the queue's own lock settles. */
static unsigned member_at(const struct sched_group *g, const uint64_t *m, unsigned start, unsigned skip) {
    for (unsigned pass = 0; pass < 2; pass++) {
        unsigned end = pass ? start : g->count;
        for (unsigned i = cpuset_next(m, pass ? 0 : start, end); i < end; i = cpuset_next(m, i + 1, end))
            if (i != skip) return i;
    }
    return g->count;
}

/* An idle CPU under g, its member at skip left out: in a cache a whole idle core's CPU first, then
   any; above it, down through the first member that has one. topology.cpus when the hints show none. */
static unsigned idle_under(const struct sched_group *g, unsigned start, unsigned skip) {
    if (g->cpu) {
        unsigned i = member_at(g, g->core_idle, start, skip);
        if (i == g->count) i = member_at(g, g->hint[HINT_IDLE], start, skip);
        return i < g->count ? g->cpu[i] : topology.cpus;
    }
    unsigned i = member_at(g, g->hint[HINT_IDLE], start, skip);
    return i < g->count ? idle_under(g->member[i], 0, g->member[i]->count) : topology.cpus;
}

/* A CPU under g with a thread to give, as idle_under by the busy hints. */
static unsigned busy_under(const struct sched_group *g, unsigned start, unsigned skip) {
    unsigned i = member_at(g, g->hint[HINT_BUSY], start, skip);
    if (i == g->count) return topology.cpus;
    return g->cpu ? g->cpu[i] : busy_under(g->member[i], 0, g->member[i]->count);
}

/* A CPU under g taken round its members by q's sample, its member at skip left out: topology.cpus
   when it has no other. */
static unsigned sample_under(struct run_queue *q, const struct sched_group *g, unsigned skip) {
    if (g->count == 1 && skip == 0) return topology.cpus;
    unsigned i = q->sample++ % g->count;
    if (i == skip) i = (i + 1) % g->count;
    return g->cpu ? g->cpu[i] : sample_under(q, g->member[i], g->member[i]->count);
}

/* ---- the fair class ---- */

static uint64_t slice_v(const struct sched_entity *e) { return e->policy.request_ns * W_ONE / e->policy.weight; }

/* q's virtual time kept as vbase + sum / weight with 0 <= sum < weight. */
static void rebase(struct run_queue *q) {
    if (!q->weight) {
        q->sum = 0;
        return;
    }
    int64_t k = sdiv(q->sum, q->weight);
    q->vbase += (uint64_t)k;
    q->sum -= (__int128)k * q->weight;
}

/* Not ahead of its share: its virtual time at most V. */
static bool eligible(const struct run_queue *q, const struct sched_entity *e) {
    return (__int128)(int64_t)(e->vruntime - q->vbase) * q->weight <= q->sum;
}

/* e joins q's fair threads where its lag (lag_ns) stays what it was once its own weight is in the
   mean, V - v = vlag: v = V - vlag (W + w) / W, W the weight before it. A new request begins. */
static void fair_join(struct run_queue *q, struct sched_entity *e) {
    uint64_t w = e->policy.weight;
    if (!q->weight) {
        e->vruntime = q->vbase;
    } else {
        int64_t vlag = sdiv((__int128)e->lag_ns * W_ONE, w);
        e->vruntime = q->vbase + (uint64_t)sdiv(q->sum - (__int128)vlag * (q->weight + w), q->weight);
    }
    e->deadline = e->vruntime + slice_v(e);
    q->weight += w;
    q->sum += (__int128)w * (int64_t)(e->vruntime - q->vbase);
    rebase(q);
}

/* e leaves q's fair threads with its lag, w (V - v) in ns, bounded by its request. */
static void fair_leave(struct run_queue *q, struct sched_entity *e) {
    uint64_t w = e->policy.weight;
    int64_t own = (int64_t)(e->vruntime - q->vbase);
    int64_t vlag = sdiv(q->sum - (__int128)own * q->weight, q->weight);
    int64_t lag = sdiv((__int128)vlag * w, W_ONE), cap = (int64_t)e->policy.request_ns;
    e->lag_ns = lag > cap ? cap : lag < -cap ? -cap : lag;
    q->weight -= w;
    q->sum -= (__int128)w * own;
    rebase(q);
}

static bool fair_before(const struct avl_node *a, const struct avl_node *b) {
    const struct sched_entity *x = ENTITY(a), *y = ENTITY(b);
    return x->vruntime != y->vruntime ? earlier(x->vruntime, y->vruntime) : x->seq < y->seq;
}
static void fair_fix(struct avl_node *n) {
    struct sched_entity *e = ENTITY(n);
    uint64_t d = e->deadline;
    if (n->left && earlier(ENTITY(n->left)->min_deadline, d)) d = ENTITY(n->left)->min_deadline;
    if (n->right && earlier(ENTITY(n->right)->min_deadline, d)) d = ENTITY(n->right)->min_deadline;
    e->min_deadline = d;
}
static const struct avl_order fair_order = {.before = fair_before, .fix = fair_fix};

/* The eligible fair thread whose request ends first. Down the tree by virtual time an eligible
   node's left subtree is eligible whole, so its earliest deadline is a candidate as it stands; the
   best candidate's subtree is searched for that deadline at the end. O(log n). */
static struct sched_entity *fair_pick(const struct run_queue *q) {
    const struct avl_node *n = q->fair.root, *best = NULL;
    bool subtree = false;
    uint64_t at = 0;
    while (n) {
        const struct sched_entity *e = ENTITY(n);
        if (!eligible(q, e)) {
            n = n->left;
            continue;
        }
        if (n->left && (!best || earlier(ENTITY(n->left)->min_deadline, at))) {
            best = n->left;
            subtree = true;
            at = ENTITY(n->left)->min_deadline;
        }
        if (!best || earlier(e->deadline, at)) {
            best = n;
            subtree = false;
            at = e->deadline;
        }
        n = n->right;
    }
    while (subtree) {
        if (best->left && ENTITY(best->left)->min_deadline == at) best = best->left;
        else if (ENTITY(best)->deadline == at) subtree = false;
        else best = best->right;
    }
    /* The least virtual time is never above the mean: a tree that holds a thread has an eligible one. */
    if (!best && q->fair.root) best = avl_first(&q->fair);
    return best ? ENTITY(best) : NULL;
}

/* ---- the realtime class ---- */

static bool rt_before(const struct avl_node *a, const struct avl_node *b) {
    const struct sched_entity *x = ENTITY(a), *y = ENTITY(b);
    unsigned rx = sched_rank(x), ry = sched_rank(y);
    return rx != ry ? rx > ry : x->seq < y->seq;
}
static const struct avl_order rt_order = {.before = rt_before};

/* A realtime thread whose period has ended begins the next, its budget whole. */
static void rt_renew(struct sched_entity *e, uint64_t now) {
    if (earlier(now, e->rt_period_end)) return;
    e->rt_used = 0;
    e->rt_period_end = now + rt_period(e);
}

/* q's lock held: q's curr, realtime, spent its budget: the fair class at the mean (no lag) until its
   period ends, when replenish_fire brings it back. On q's own CPU. */
static void throttle(struct run_queue *q, struct sched_entity *e) {
    e->throttled = true;
    e->lag_ns = 0;
    fair_join(q, e);
    alarm_set(&e->replenish, e->rt_period_end);
}

/* q's lock held, q charged: a throttled thread of q's back in the realtime class, a new period begun. */
static void unthrottle(struct run_queue *q, struct sched_entity *e);

/* ---- membership ---- */

/* q's lock held: e, runnable on q and not what q's CPU runs, waits in its class's tree. head: a
   realtime one at the head of its priority (it was preempted), else behind its equals. */
static void tree_put(struct run_queue *q, struct sched_entity *e, bool head) {
    if (fair(e)) {
        e->seq = ++q->fair_seq;
        avl_insert(&q->fair, &fair_order, &e->node);
    } else {
        e->seq = head ? --q->rt_head : ++q->rt_tail;
        avl_insert(&q->rt, &rt_order, &e->node);
    }
    e->queued = true;
    if (!e->bound && !q->movable++) hint_add(q->llc, HINT_BUSY, q->llc_at);
}

static void tree_take(struct run_queue *q, struct sched_entity *e) {
    if (fair(e)) avl_remove(&q->fair, &fair_order, &e->node);
    else avl_remove(&q->rt, &rt_order, &e->node);
    e->queued = false;
    if (!e->bound && !--q->movable) hint_remove(q->llc, HINT_BUSY, q->llc_at);
}

/* q's lock held, q charged: e, runnable and in no queue, joins q: its class's sums, and its tree but
   for one q's CPU runs (sched_set_policy). A realtime thread whose period ended begins the next. */
static void queue_join(struct run_queue *q, struct sched_entity *e, bool tree) {
    if (realtime(e) && !earlier(q->stamp, e->rt_period_end)) {
        e->throttled = false;
        rt_renew(e, q->stamp);
    }
    if (fair(e)) fair_join(q, e);
    q->nr++;
    if (tree) tree_put(q, e, false);
}

/* q's lock held, q charged: e, queued on q or what its CPU runs, leaves q. */
static void queue_leave(struct run_queue *q, struct sched_entity *e) {
    if (e->queued) tree_take(q, e);
    if (fair(e)) fair_leave(q, e);
    q->nr--;
}

static void unthrottle(struct run_queue *q, struct sched_entity *e) {
    bool queued = e->queued, runnable = queued || OWNER(e) == curr(q);
    if (queued) tree_take(q, e);
    if (runnable) fair_leave(q, e);
    e->throttled = false;
    e->rt_used = 0;
    e->rt_period_end = q->stamp + rt_period(e);
    if (runnable && fair(e)) fair_join(q, e); /* a fair thread whose inherited rank went meanwhile */
    if (queued) tree_put(q, e, false);
}

/* ---- time ---- */

/* q's lock held: what ran on its CPU since the last charge, interrupts and kernel time included,
   to the thread it runs: its CPU time, and its virtual time or realtime budget. */
static void charge(struct run_queue *q) {
    uint64_t now = clock_now_ns();
    if (!earlier(q->stamp, now)) return;
    uint64_t ns = now - q->stamp;
    q->stamp = now;
    struct sched_entity *e = &curr(q)->sched;
    e->cpu_ns += ns;
    if (e->idle) return;
    if (!fair(e)) {
        rt_renew(e, now);
        e->rt_used += ns;
        return;
    }
    uint64_t dv;
    if (!clock_muldiv(ns, W_ONE, e->policy.weight, &dv)) dv = (uint64_t)INT64_MAX;
    e->vruntime += dv;
    q->sum += (__int128)dv * e->policy.weight;
    rebase(q);
}

/* q's lock held, q charged: e, just queued on q, is to run before what q's CPU runs: anything
   before the idle thread, realtime before fair and the higher priority first, and a fair one that
   is eligible with an earlier deadline. */
static bool preempts(const struct run_queue *q, const struct sched_entity *e) {
    const struct sched_entity *c = &curr(q)->sched;
    if (c->idle) return true;
    if (!fair(e)) return fair(c) || sched_rank(e) > sched_rank(c);
    return fair(c) && eligible(q, e) && earlier(e->deadline, c->deadline);
}

/* q's lock held, q charged, on its own CPU: while a thread waits behind the one it runs, its slice
   alarm for when that one's request ends (fair) or its budget does (realtime), early by at most a
   sixteenth of either, so that it may share an interrupt with an alarm due before it. A stale one
   finds nothing to do. */
static void slice_arm(struct run_queue *q) {
    const struct sched_entity *e = &curr(q)->sched;
    q->slice_stale = false;
    if (e->idle || q->nr < 2) return;
    uint64_t slice, left;
    if (fair(e)) {
        int64_t dv = (int64_t)(e->deadline - e->vruntime);
        slice = e->policy.request_ns;
        left = dv > 0 ? (uint64_t)dv * e->policy.weight / W_ONE : 0;
    } else {
        slice = rt_budget(e);
        left = e->rt_used < slice ? slice - e->rt_used : 0;
    }
    uint64_t at = q->stamp + left;
    alarm_set_range(&q->slice, at - MIN(left, slice / 16), at);
}

/* q's lock held, q charged, after a change to its threads: a switch on its CPU when due, else its
   slice alarm for the threads it has now. True when its CPU, another one, is to be kicked once the
   lock is given back. */
static bool tell(struct run_queue *q, bool due) {
    bool here = q->cpu == cpu_current_index();
    if (due) per_cpu(sched_resched, q->cpu) = true;
    else if (here) slice_arm(q);
    else if (q->nr >= 2) __atomic_store_n(&q->slice_stale, true, __ATOMIC_RELEASE);
    else return false;
    return !here;
}

/* ---- placement ---- */

/* Where t, woken or new, goes: its CPU alone when it is bound; else the CPU it ran on if that is
   idle, an idle CPU of the same cache, or its CPU still. */
static unsigned choose(struct thread *t) {
    const struct sched_entity *e = &t->sched;
    if (e->bound) return usable(e->bound - 1) ? e->bound - 1 : 0;
    unsigned prev = __atomic_load_n(&e->cpu, __ATOMIC_ACQUIRE);
    struct run_queue *p = rq(prev);
    bool ok = usable(prev);
    if (ok && __atomic_load_n(&p->idle_hinted, __ATOMIC_ACQUIRE)) return prev;
    unsigned cpu = idle_under(p->llc, p->llc_at, p->llc->count);
    if (cpu < topology.cpus) return cpu;
    if (ok) return prev;
    cpu = cpu_current_index();
    return usable(cpu) ? cpu : 0;
}

/* Interrupts off, no queue's lock held: t, runnable and in no queue, joins the one choose picks,
   whose CPU is told when t is to run before what it runs. */
static void enqueue(struct thread *t) {
    struct sched_entity *e = &t->sched;
    for (;;) {
        unsigned from = __atomic_load_n(&e->cpu, __ATOMIC_ACQUIRE), to = choose(t);
        lock2(from, to);
        struct run_queue *q = rq(to);
        if (e->cpu != from || q->leaving) {
            unlock2(from, to);
            continue;
        }
        __atomic_store_n(&e->cpu, to, __ATOMIC_RELEASE);
        charge(q);
        queue_join(q, e, true);
        hint_idle(q, false);
        bool kick = tell(q, preempts(q, e));
        unlock2(from, to);
        STAT(wakes);
        if (from != to) STAT(moved_wakes);
        if (kick) {
            STAT(kicks);
            cpu_kick(to);
        }
        return;
    }
}

/* The first queued thread under n that is not bound, in the tree's order. */
static struct sched_entity *first_movable(struct avl_node *n) {
    if (!n) return NULL;
    struct sched_entity *e = first_movable(n->left);
    if (e || !ENTITY(n)->bound) return e ? e : ENTITY(n);
    return first_movable(n->right);
}

/* Interrupts off, no queue's lock held: src's first movable waiting thread (a realtime one first)
   to dst's queue, whose CPU, another one, is told when it is to run there before what runs. */
static bool move_one(unsigned src, unsigned dst) {
    if (src == dst) return false;
    struct run_queue *s = rq(src), *d = rq(dst);
    bool kick = false;
    lock2(src, dst);
    struct sched_entity *e = NULL;
    if (!d->leaving && __atomic_load_n(&d->prepared, __ATOMIC_ACQUIRE) && s->movable) {
        e = first_movable(s->rt.root);
        if (!e) e = first_movable(s->fair.root);
    }
    if (e) {
        charge(s);
        charge(d);
        queue_leave(s, e);
        __atomic_store_n(&e->cpu, dst, __ATOMIC_RELEASE);
        queue_join(d, e, true);
        hint_idle(d, false);
        if (dst != cpu_current_index()) kick = tell(d, preempts(d, e));
    }
    unlock2(src, dst);
    if (kick) cpu_kick(dst);
    return e != NULL;
}

/* A CPU going idle, its queue empty, interrupts off and no lock held: a waiting thread from the
   nearest queue with one to give: its core, its cache, its node, any node. */
static void pull(struct run_queue *q) {
    unsigned self = q->cpu, from = topology.cpus;
    for (unsigned i = 0; i < q->core_count && from == topology.cpus; i++)
        if (q->core[i] != self && __atomic_load_n(&rq(q->core[i])->movable, __ATOMIC_RELAXED)) from = q->core[i];
    const struct sched_group *g = q->llc;
    if (from == topology.cpus) from = busy_under(g, q->llc_at, q->llc_at);
    for (; from == topology.cpus && g->parent; g = g->parent) from = busy_under(g->parent, g->at, g->at);
    if (from < topology.cpus && move_one(from, self)) STAT(steals);
}

/* A busy CPU at its slice's end, interrupts off and no lock held: one of its waiting threads to an
   idle CPU, or to one with two threads fewer, at the nearest level that has one; the farther
   levels only every look_every. */
static void push(struct run_queue *q) {
    unsigned self = q->cpu, skip = q->llc_at;
    size_t mine = runnable(self);
    const struct sched_group *g = q->llc;
    for (unsigned level = LEVEL_CORE; level < LEVELS && g; level++) {
        if (look_every[level]) {
            if (q->stamp - q->looked[level] < look_every[level]) return;
            q->looked[level] = q->stamp;
        }
        unsigned to = topology.cpus;
        if (level == LEVEL_CORE) {
            for (unsigned i = 0; i < q->core_count && to == topology.cpus; i++) {
                unsigned s = q->core[i];
                if (s != self && usable(s) && (__atomic_load_n(&rq(s)->idle_hinted, __ATOMIC_ACQUIRE) || runnable(s) + 2 <= mine))
                    to = s;
            }
        } else {
            to = idle_under(g, skip, skip);
            if (to == topology.cpus) {
                to = sample_under(q, g, skip);
                if (to < topology.cpus && (to == self || !usable(to) || runnable(to) + 2 > mine)) to = topology.cpus;
            }
            skip = g->at;
            g = g->parent;
        }
        if (to < topology.cpus && move_one(self, to)) {
            STAT(balances);
            return;
        }
    }
}

/* ---- slices and periods ---- */

static void slice_fire(struct alarm *a, struct regs *r) {
    (void)r;
    struct run_queue *q = (struct run_queue *)((char *)a - __builtin_offsetof(struct run_queue, slice));
    if (q->cpu != cpu_current_index()) return; /* a leaving CPU's, which gave its alarms to CPU 0 */
    spin_take(&q->lock);
    charge(q);
    struct sched_entity *e = &curr(q)->sched;
    bool due = false;
    if (!e->idle && q->nr >= 2) {
        if (fair(e)) {
            int64_t dv = (int64_t)(e->deadline - e->vruntime);
            due = dv <= 0 || (uint64_t)dv * e->policy.weight / W_ONE <= e->policy.request_ns / 16;
            if (due) e->deadline = e->vruntime + slice_v(e); /* its next request */
        } else {
            due = e->rt_used + rt_budget(e) / 16 >= rt_budget(e);
            if (due) throttle(q, e);
        }
    }
    tell(q, due);
    bool busy = q->nr >= 2;
    spin_give(&q->lock);
    if (busy) push(q);
}

/* A throttled thread's period ended: the realtime class again, wherever it is now. */
static void replenish_fire(struct alarm *a, struct regs *r) {
    (void)r;
    struct thread *t = (struct thread *)((char *)a - __builtin_offsetof(struct thread, sched.replenish));
    struct sched_entity *e = &t->sched;
    struct run_queue *q = lock_thread(t);
    bool kick = false;
    charge(q);
    if (e->throttled && earlier(q->stamp, e->rt_period_end)) {
        alarm_set(a, e->rt_period_end);
    } else if (e->throttled) {
        unthrottle(q, e);
        if (e->queued) kick = tell(q, preempts(q, e));
        else if (t == curr(q)) kick = tell(q, false);
    }
    spin_give(&q->lock);
    if (kick) cpu_kick(q->cpu);
}

/* ---- switching ---- */

/* q's lock held, q charged: prev, which stays runnable, back into its tree. A realtime one whose
   budget is spent runs in the fair class for the rest of its period; a fair one whose request has
   ended asks for the next. */
static void requeue(struct run_queue *q, struct sched_entity *e, enum switch_mode mode) {
    if (!fair(e) && e->rt_used >= rt_budget(e)) throttle(q, e);
    if (fair(e)) {
        if (mode == SWITCH_YIELD && earlier(e->vruntime, e->deadline)) {
            /* the rest of its request given up: its virtual time where the request ends */
            q->sum += (__int128)(int64_t)(e->deadline - e->vruntime) * e->policy.weight;
            e->vruntime = e->deadline;
            rebase(q);
        }
        if (!earlier(e->vruntime, e->deadline)) e->deadline = e->vruntime + slice_v(e);
    }
    tree_put(q, e, mode != SWITCH_YIELD);
}

/* Interrupts off, no lock held: this CPU's next thread, the one it runs coming by mode. */
static void switch_once(enum switch_mode mode) {
    struct run_queue *q = this_cpu_ptr(queue);
    struct thread *prev = this_cpu(sched_thread);
    struct sched_entity *p = &prev->sched;
    /* Every switch is a quiet point (sys/epoch.h), the first one out of idle included: nothing
       holds preemption off to come here, so no reader's section is open. Before the queue's lock:
       the grace period it may end wakes the workers waiting for it. */
    epoch_quiet(false);
    spin_take(&q->lock);
    charge(q);
    bool stays = p->idle || mode != SWITCH_WAIT || p->state == THREAD_RUNNING;
    if (!stays && !q->rt.root && !q->fair.root && !q->leaving) {
        /* Going idle: a waiting thread from the nearest busy queue first. prev stays what this CPU
           runs meanwhile, which a waker only marks running again. */
        spin_give(&q->lock);
        pull(q);
        spin_take(&q->lock);
        charge(q);
        stays = p->state == THREAD_RUNNING;
    }
    this_cpu(sched_resched) = false;
    if (!p->idle) {
        if (!stays) {
            queue_leave(q, p);
        } else if (q->leaving) {
            queue_leave(q, p);
            q->evicted = prev; /* another queue takes it once this CPU is off its stack */
        } else {
            requeue(q, p, mode);
        }
    }
    struct avl_node *n = avl_first(&q->rt);
    struct sched_entity *e = n ? ENTITY(n) : fair_pick(q);
    struct thread *next = e ? OWNER(e) : q->idle;
    if (e) {
        tree_take(q, e);
        if (!fair(e)) rt_renew(e, q->stamp);
    }
    hint_idle(q, !e);
    if (next != prev) {
        __atomic_store_n(&next->sched.on_cpu, true, __ATOMIC_RELEASE);
        this_cpu(sched_thread) = next;
    }
    slice_arm(q);
    if (next == prev) {
        spin_give(&q->lock);
        return;
    }
    STAT(switches);
    if (q->fpu_live) {
        /* prev ran in ring 3 since its state was loaded: its area takes the registers, but at its end.
           No kernel thread touches them (the kernel is built with general registers only). */
        if (p->state == THREAD_ZOMBIE) q->fpu_owner = NULL;
        else fpu_save(prev->fpu);
        q->fpu_live = false;
    }
    /* the app's own FS and GS bases (arch/cpu.h), as each left them; a kernel thread uses neither */
    if (prev->process) {
        prev->fs_base = fs_base_read();
        prev->gs_base = user_gs_base_read();
    }
    if (next->process) {
        fs_base_write(next->fs_base);
        user_gs_base_write(next->gs_base);
        tss_set_rsp0((uint64_t)next->kstack + KSTACK_PAGES * PAGE_SIZE);
    }
    vmm_switch(next->process ? next->process->vmm_space : NULL);
    q->last = prev;
    context_switch(&prev->ksp, next->ksp);
    /* prev again, on whichever CPU took it: sched_post_switch gave that CPU's lock back */
}

void sched_post_switch(void) {
    struct run_queue *q = this_cpu_ptr(queue);
    struct thread *prev = q->last, *evicted = q->evicted;
    q->last = NULL;
    q->evicted = NULL;
    bool ended = prev->sched.state == THREAD_ZOMBIE && !prev->sched.queued && prev != evicted;
    __atomic_store_n(&prev->sched.on_cpu, false, __ATOMIC_RELEASE);
    spin_give(&q->lock);
    if (evicted) enqueue(evicted);
    if (ended) thread_left(prev); /* its record and stacks may go: this CPU is off them */
}

/* Interrupts as the caller had them after, no spinlock held: switches until no switch is due. */
static void reschedule(enum switch_mode mode) {
    ASSERT(!this_cpu(preempt_count)); /* never in a handler, under a spinlock or with preemption off */
    uint64_t f = irq_save();
    for (;;) {
        switch_once(mode);
        if (!this_cpu(sched_resched)) break;
        mode = SWITCH_PREEMPT;
    }
    irq_restore(f);
}

void schedule(void) { reschedule(SWITCH_WAIT); }

void sched_yield(void) {
    STAT(yields);
    reschedule(SWITCH_YIELD);
}

void preempt_schedule(void) {
    if (this_cpu(preempt_count) || !(read_rflags() & RFLAGS_IF)) return;
    reschedule(SWITCH_PREEMPT);
}

void sched_preempt_irq(void) { reschedule(SWITCH_PREEMPT); }

void sched_user_return(struct regs *r) {
    (void)r;
    /* syscall_dispatch returns with interrupts on; from here to iretq they stay off. */
    cli();
    struct thread *t;
    for (;;) {
        t = this_cpu(sched_thread);
        if (t->killed) thread_exit(t->kill_code);
        /* The quiet point on the way to ring 3 (sys/epoch.h), before the look at the flag: a
           worker the grace period's end wakes here may be due to run on this CPU. */
        epoch_quiet(false);
        if (!this_cpu(sched_resched)) break;
        reschedule(SWITCH_PREEMPT);
    }
    /* its extended state in this CPU's registers, unless they hold it as it left them */
    struct run_queue *q = this_cpu_ptr(queue);
    unsigned mine = cpu_current_index() + 1;
    if (q->fpu_owner != t || t->sched.fpu_cpu != mine) {
        fpu_restore(t->fpu);
        q->fpu_owner = t;
        t->sched.fpu_cpu = mine;
    }
    q->fpu_live = true;
}

/* VEC_RESCHED: a CPU that set this one's flag, or changed its threads so that its slice alarm is due
   again, kicked it (cpu_kick). The way out of the interrupt switches when the flag says so. */
static enum irq_result resched_irq(void *context) {
    (void)context;
    struct run_queue *q = this_cpu_ptr(queue);
    if (__atomic_load_n(&q->slice_stale, __ATOMIC_ACQUIRE)) {
        spin_take(&q->lock);
        charge(q);
        slice_arm(q);
        spin_give(&q->lock);
    }
    return IRQ_HANDLED;
}

/* ---- threads ---- */

void sched_make_ready(struct thread *t) {
    struct sched_entity *e = &t->sched;
    if (e->idle) { /* the boot thread, taken out of a wait: it halts until an interrupt, this one */
        __atomic_store_n(&e->state, THREAD_RUNNING, __ATOMIC_RELEASE);
        cpu_kick(__atomic_load_n(&e->cpu, __ATOMIC_ACQUIRE));
        return;
    }
    uint64_t f = irq_save();
    struct run_queue *q = lock_thread(t);
    /* One still on its CPU sees itself running when it comes to switch; a queued one is in place. */
    enum thread_state blocked = THREAD_BLOCKED;
    bool out = __atomic_compare_exchange_n(&e->state, &blocked, THREAD_RUNNING, false, __ATOMIC_SEQ_CST,
                                           __ATOMIC_RELAXED) &&
               !e->queued && !e->on_cpu;
    spin_give(&q->lock);
    if (out) enqueue(t);
    irq_restore(f);
}

void sched_set_policy(struct thread *t, struct sched_policy policy) {
    ASSERT(policy.weight && policy.weight <= SCHED_WEIGHT_MAX);
    ASSERT(policy.request_ns >= SCHED_REQUEST_MIN_NS && policy.request_ns <= SCHED_REQUEST_MAX_NS);
    ASSERT(policy.class == SCHED_FAIR || (policy.budget_ns && policy.budget_ns <= policy.period_ns));
    struct sched_entity *e = &t->sched;
    uint64_t f = irq_save();
    struct run_queue *q = lock_thread(t);
    bool running = t == curr(q), queued = e->queued, kick = false;
    charge(q);
    if (running || queued) queue_leave(q, e);
    bool was = realtime(e);
    e->policy = policy;
    if (realtime(e) != was) e->lag_ns = 0;
    e->throttled = false;
    e->rt_used = 0;
    e->rt_period_end = q->stamp + rt_period(e);
    if (running || queued) {
        queue_join(q, e, queued);
        kick = tell(q, running ? q->nr >= 2 : preempts(q, e));
    }
    spin_give(&q->lock);
    if (kick) cpu_kick(q->cpu);
    irq_restore(f);
}

void sched_set_boost(struct thread *t, unsigned boost) {
    struct sched_entity *e = &t->sched;
    uint64_t f = irq_save();
    struct run_queue *q = lock_thread(t);
    bool running = t == curr(q), queued = e->queued, kick = false;
    if (boost != e->boost) {
        if (!e->boost) STAT(boosts);
        charge(q);
        if (running || queued) queue_leave(q, e);
        bool was = realtime(e);
        e->boost = boost;
        if (realtime(e) != was) {
            e->lag_ns = 0;
            if (!was) rt_renew(e, q->stamp); /* a period of its own, unless one it began still runs */
        }
        if (running || queued) {
            queue_join(q, e, queued);
            kick = tell(q, running ? q->nr >= 2 : preempts(q, e));
        }
    }
    spin_give(&q->lock);
    if (kick) cpu_kick(q->cpu);
    irq_restore(f);
}

void sched_thread_add(struct thread *t) {
    struct sched_entity *e = &t->sched;
    e->state = THREAD_BLOCKED; /* not runnable until made ready */
    e->cpu = cpu_current_index();
    e->policy = (struct sched_policy){.class = SCHED_FAIR, .weight = SCHED_WEIGHT_DEFAULT,
                                      .request_ns = SCHED_REQUEST_DEFAULT_NS};
    alarm_init(&e->replenish, replenish_fire);
}

void sched_thread_bind(struct thread *t, unsigned cpu) {
    ASSERT(cpu < topology.cpus);
    t->sched.bound = cpu + 1;
    t->sched.cpu = cpu; /* in no queue yet: where choose puts it */
}

/* Its CPU has left it for good (thread_left), or it never ran: only its period's alarm is left. */
void sched_thread_remove(struct thread *t) { alarm_cancel(&t->sched.replenish); }

uint64_t sched_thread_cpu_ns(struct thread *t) {
    uint64_t f = irq_save();
    struct run_queue *q = lock_thread(t);
    if (t == curr(q)) charge(q);
    uint64_t ns = t->sched.cpu_ns;
    spin_give(&q->lock);
    irq_restore(f);
    return ns;
}

/* ---- each CPU ---- */

NORETURN void sched_idle(void) {
    struct run_queue *q = this_cpu_ptr(queue); /* an idle thread never leaves its CPU */
    uint64_t f = spin_lock(&q->lock);
    q->idle_loop = true;
    hint_idle(q, !q->rt.root && !q->fair.root);
    spin_unlock(&q->lock, f);
    for (;;) {
        cli();
        /* Out of epoch's count until the next switch (sys/epoch.h): from here the loop reads
           nothing epoch protects, and a CPU that parks for a sleep stays out. Before the look at
           the flag: a worker the grace period's end wakes here may be due to run on this CPU. */
        epoch_quiet(true);
        cpu_park_check();
        if (this_cpu(sched_resched)) {
            schedule();
            continue;
        }
        /* An interrupt in the wait switches nothing there (it would leave the idle word watching,
           arch/idle.c): the loop looks at the flag after it. A kick between the look and the wait
           ends the wait (arch/idle.h); this CPU's alarms (sys/alarm.c) end it when one is due. */
        preempt_disable();
        cpu_idle();
        preempt_enable();
    }
}

/* cpu's idle thread: never queued, running whenever nothing else does. */
static struct thread *idle_thread_new(unsigned cpu) {
    char name[16];
    ksnprintf(name, sizeof name, "idle%u", cpu);
    struct thread *t = thread_new_idle(name, topology.cpu[cpu].home);
    if (!t) return NULL;
    t->sched.idle = true;
    t->sched.state = THREAD_RUNNING;
    t->sched.cpu = cpu;
    return t;
}

static void queue_prepare(unsigned cpu, struct thread *idle) {
    struct run_queue *q = rq(cpu);
    q->idle = idle;
    q->cpu = cpu;
    q->rt_head = q->rt_tail = 1ull << 63;
    q->stamp = clock_now_ns();
    alarm_init(&q->slice, slice_fire);
    __atomic_store_n(&q->prepared, true, __ATOMIC_RELEASE);
}

bool sched_cpu_prepare(unsigned cpu) {
    if (cpu >= topology.cpus) return false;
    if (rq(cpu)->prepared) return true;
    struct thread *idle = idle_thread_new(cpu);
    if (!idle) return false;
    queue_prepare(cpu, idle);
    return true;
}

uint64_t sched_cpu_stack(unsigned cpu) { return (uint64_t)rq(cpu)->idle->kstack + KSTACK_PAGES * PAGE_SIZE; }

NORETURN void sched_cpu_start(void) {
    struct run_queue *q = this_cpu_ptr(queue);
    this_cpu(sched_thread) = q->idle;
    uint64_t f = spin_lock(&q->lock);
    q->stamp = clock_now_ns();
    q->fpu_owner = NULL; /* its registers hold no thread's state after a start */
    q->fpu_live = false;
    __atomic_store_n(&q->idle->sched.on_cpu, true, __ATOMIC_RELEASE);
    __atomic_store_n(&q->leaving, false, __ATOMIC_RELEASE);
    spin_unlock(&q->lock, f);
    sched_idle();
}

bool sched_cpu_empty(unsigned cpu) {
    return !runnable(cpu) && !__atomic_load_n(&rq(cpu)->evicted, __ATOMIC_ACQUIRE);
}

void sched_cpu_leave(unsigned cpu) {
    if (!cpu || cpu >= topology.cpus || !rq(cpu)->prepared) return;
    struct run_queue *q = rq(cpu), *to = rq(0);
    uint64_t f = irq_save();
    lock2(0, cpu);
    __atomic_store_n(&q->leaving, true, __ATOMIC_RELEASE);
    hint_idle(q, false);
    charge(q);
    charge(to);
    for (struct avl_node *n; (n = q->rt.root ? avl_first(&q->rt) : avl_first(&q->fair));) {
        struct sched_entity *e = ENTITY(n);
        queue_leave(q, e);
        __atomic_store_n(&e->cpu, 0, __ATOMIC_RELEASE);
        queue_join(to, e, true);
    }
    per_cpu(sched_resched, cpu) = true; /* the thread it runs goes too, at its next switch */
    bool kick0 = tell(to, false);
    unlock2(0, cpu);
    irq_restore(f);
    cpu_kick(cpu); /* then it parks (cpu_park_check) */
    if (kick0) cpu_kick(0);
}

void sched_get_stats(struct sched_stats *out) {
    out->switches = __atomic_load_n(&stats.switches, __ATOMIC_RELAXED);
    out->wakes = __atomic_load_n(&stats.wakes, __ATOMIC_RELAXED);
    out->moved_wakes = __atomic_load_n(&stats.moved_wakes, __ATOMIC_RELAXED);
    out->steals = __atomic_load_n(&stats.steals, __ATOMIC_RELAXED);
    out->balances = __atomic_load_n(&stats.balances, __ATOMIC_RELAXED);
    out->kicks = __atomic_load_n(&stats.kicks, __ATOMIC_RELAXED);
    out->yields = __atomic_load_n(&stats.yields, __ATOMIC_RELAXED);
    out->boosts = __atomic_load_n(&stats.boosts, __ATOMIC_RELAXED);
}

/* ---- the topology's groups ---- */

static void *boot_alloc(size_t bytes) {
    void *p = kzalloc(bytes);
    if (!p) panic("sched: no memory for the topology's groups");
    return p;
}

/* A group of count members, its hints zeroed (and a cache's whole-core hint). */
static void group_init(struct sched_group *g, unsigned count, bool cache) {
    size_t words = cpuset_words(count);
    uint64_t *m = boot_alloc((HINTS + cache) * words * sizeof *m);
    g->count = count;
    for (unsigned h = 0; h < HINTS; h++) g->hint[h] = m + h * words;
    if (cache) g->core_idle = m + HINTS * words;
}

/* items sorted by key (a dense number below groups), each run in index order: in *by, group k's
   from (*start)[k] to (*start)[k + 1]. */
static void group_by(unsigned items, unsigned groups, unsigned (*key)(unsigned item), unsigned **by, unsigned **start) {
    unsigned *s = boot_alloc((groups + 1) * sizeof *s), *b = boot_alloc(items * sizeof *b);
    unsigned *fill = boot_alloc(groups * sizeof *fill);
    for (unsigned i = 0; i < items; i++) s[key(i) + 1]++;
    for (unsigned k = 0; k < groups; k++) s[k + 1] += s[k];
    for (unsigned i = 0; i < items; i++) {
        unsigned k = key(i);
        b[s[k] + fill[k]++] = i;
    }
    kfree(fill);
    *by = b;
    *start = s;
}

static unsigned *cache_cpus, *cache_start;
static unsigned cache_of(unsigned cpu) { return topology.cpu[cpu].llc; }
static unsigned core_of(unsigned cpu) { return topology.cpu[cpu].core; }
static unsigned node_of_cache(unsigned llc) { return topology.cpu[cache_cpus[cache_start[llc]]].node; }

/* CPU 0 at boot: the caches, nodes and the whole, and each CPU's place in its cache and core. */
static void groups_init(void) {
    unsigned *core_cpus, *core_start, *node_caches, *node_start;
    group_by(topology.cpus, topology.llcs, cache_of, &cache_cpus, &cache_start);
    group_by(topology.cpus, topology.cores, core_of, &core_cpus, &core_start);
    group_by(topology.llcs, topology.nodes, node_of_cache, &node_caches, &node_start);
    struct sched_group *caches = boot_alloc(topology.llcs * sizeof *caches);
    struct sched_group *nodes = boot_alloc(topology.nodes * sizeof *nodes);
    struct sched_group **members = boot_alloc((topology.llcs + topology.nodes) * sizeof *members);
    for (unsigned l = 0; l < topology.llcs; l++) {
        struct sched_group *g = &caches[l];
        group_init(g, cache_start[l + 1] - cache_start[l], true);
        g->cpu = cache_cpus + cache_start[l];
        for (unsigned i = 0; i < g->count; i++) {
            struct run_queue *q = rq(g->cpu[i]);
            unsigned core = topology.cpu[g->cpu[i]].core;
            q->llc = g;
            q->llc_at = i;
            q->core = core_cpus + core_start[core];
            q->core_count = core_start[core + 1] - core_start[core];
        }
    }
    unsigned with_cpus = 0;
    for (unsigned n = 0; n < topology.nodes; n++) {
        struct sched_group *g = &nodes[n];
        unsigned count = node_start[n + 1] - node_start[n];
        if (!count) continue; /* memory only */
        group_init(g, count, false);
        g->member = members + node_start[n];
        for (unsigned i = 0; i < count; i++) {
            g->member[i] = &caches[node_caches[node_start[n] + i]];
            g->member[i]->parent = g;
            g->member[i]->at = i;
        }
        with_cpus++;
    }
    group_init(&whole, with_cpus, false);
    whole.member = members + topology.llcs;
    for (unsigned n = 0, i = 0; n < topology.nodes; n++) {
        if (!nodes[n].count) continue;
        nodes[n].parent = &whole;
        nodes[n].at = i;
        whole.member[i++] = &nodes[n];
    }
    kfree(node_caches);
    kfree(node_start);
}

void sched_init(void) {
    groups_init();
    /* the boot context is CPU 0's idle thread */
    struct thread *boot = idle_thread_new(0);
    if (!boot) panic("sched: no memory");
    this_cpu(sched_thread) = boot;
    boot->sched.on_cpu = true;
    queue_prepare(0, boot);
    if (!irq_request_system(&(struct irq_spec){.name = "resched", .handler = resched_irq, .node = -1}, VEC_RESCHED))
        panic("sched: VEC_RESCHED is taken");
    kprintf("sched: %u CPUs in %u cores, %u caches, %u nodes; realtime and fair (EEVDF) classes, preemptible\n",
            topology.cpus, topology.cores, topology.llcs, whole.count);
}
