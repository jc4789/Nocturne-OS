/* Alarms (sys/alarm.h). Each CPU keeps the alarms armed on it in a pairing heap ordered by their
   latest moments, the first at the root, under its own lock: arming is O(1), taking the first or
   any other one out O(log n) amortized, so many alarms (every sleeper's deadline, every
   connection's timers) cost little and no CPU's alarms wait on another's. The CPU's wake device
   (arch/lapic_timer.h) is set for the root's latest moment; when it comes, the root fires while
   it is in its range, and the device is set for the next. It may come early (a count from a rate
   bound below the true one, a setting left after the root was cancelled): then nothing is due,
   and it is set again.

   Where an alarm is, in armed_on: 0 nowhere; a CPU's index + 1, in that CPU's heap (changed only
   under that heap's lock); MOVING, claimed by one setter or canceller, in no heap, for the
   moment it takes to put it elsewhere (so two CPUs setting it at once leave it in one heap);
   DUE, due while its last fire still runs on another CPU. Who runs it, in firing: the CPU index
   + 1 whose fire runs it, taken from 0 before the fire, so it never runs twice at once; a DUE
   alarm is run by whoever takes its firing next (the CPU that found it due, or the end of the fire
   in the way). */
#include "kernel.h"
#include "arch/cpu.h"
#include "arch/irq.h"
#include "arch/lapic_timer.h"
#include "arch/smp.h"
#include "arch/spin.h"
#include "sys/alarm.h"
#include "sys/clock.h"

#define ARMED_MOVING UINT32_MAX
#define ARMED_DUE (UINT32_MAX - 1)

PERCPU static struct alarm_cpu {
    struct spinlock lock;
    struct alarm *root;
    uint64_t programmed; /* when its wake device comes, CLOCK_NEVER: not set */
    bool ready;          /* its wake device is set up (alarm_init_cpu) */
} __attribute__((aligned(64))) alarms;

/* ---- the heap ---- */

/* Two roots (prev and next NULL): the later one becomes the earlier one's first child. */
static struct alarm *meld(struct alarm *a, struct alarm *b) {
    if (b->at < a->at) {
        struct alarm *t = a;
        a = b;
        b = t;
    }
    b->prev = a;
    b->next = a->child;
    if (a->child) a->child->prev = b;
    a->child = b;
    return a;
}

/* A list of siblings into one root: melded in pairs from the left, the pairs from the right. */
static struct alarm *merge_pairs(struct alarm *first) {
    struct alarm *pairs = NULL;
    while (first) {
        struct alarm *a = first, *b = a->next;
        first = b ? b->next : NULL;
        a->next = a->prev = NULL;
        if (b) {
            b->next = b->prev = NULL;
            a = meld(a, b);
        }
        a->next = pairs;
        pairs = a;
    }
    struct alarm *root = NULL;
    while (pairs) {
        struct alarm *next = pairs->next;
        pairs->next = NULL;
        root = root ? meld(root, pairs) : pairs;
        pairs = next;
    }
    return root;
}

static void heap_insert(struct alarm_cpu *c, struct alarm *a) {
    a->child = a->next = a->prev = NULL;
    c->root = c->root ? meld(c->root, a) : a;
}

static void heap_remove(struct alarm_cpu *c, struct alarm *a) {
    struct alarm *rest = merge_pairs(a->child);
    if (a == c->root) {
        c->root = rest;
    } else {
        if (a->prev->child == a) a->prev->child = a->next; /* a first child: prev is its parent */
        else a->prev->next = a->next;
        if (a->next) a->next->prev = a->prev;
        if (rest) c->root = meld(c->root, rest);
    }
    a->child = a->next = a->prev = NULL;
}

/* ---- arming ---- */

/* c's lock held, interrupts off, on c's own CPU: its wake device comes by at. */
static void program(struct alarm_cpu *c, uint64_t at) {
    if (!c->ready) return;
    c->programmed = at;
    lapic_wake_at(at);
}

/* Interrupts off: a out of wherever it is and MOVING, this caller's. True when it was armed (in
   a heap, or due). A MOVING alarm is another caller's for the few instructions that put it
   elsewhere, with its interrupts off: that is waited for. */
static bool claim(struct alarm *a) {
    for (;;) {
        uint32_t on = __atomic_load_n(&a->armed_on, __ATOMIC_ACQUIRE);
        if (on == ARMED_MOVING) {
            pause();
            continue;
        }
        if (on == 0 || on == ARMED_DUE) {
            if (__atomic_compare_exchange_n(&a->armed_on, &on, ARMED_MOVING, false, __ATOMIC_ACQUIRE,
                                            __ATOMIC_RELAXED))
                return on == ARMED_DUE;
            continue;
        }
        struct alarm_cpu *c = &per_cpu(alarms, on - 1);
        spin_take(&c->lock);
        bool there = __atomic_load_n(&a->armed_on, __ATOMIC_RELAXED) == on;
        if (there) {
            heap_remove(c, a);
            __atomic_store_n(&a->armed_on, ARMED_MOVING, __ATOMIC_RELAXED);
        }
        spin_give(&c->lock);
        if (there) return true;
    }
}

void alarm_set_range(struct alarm *a, uint64_t early, uint64_t latest) {
    if (early > latest) early = latest;
    uint64_t f = irq_save();
    claim(a);
    unsigned self = cpu_current_index();
    struct alarm_cpu *c = &per_cpu(alarms, self);
    spin_take(&c->lock);
    a->early = early;
    a->at = latest;
    heap_insert(c, a);
    __atomic_store_n(&a->armed_on, self + 1, __ATOMIC_RELEASE);
    if (c->root == a && latest < c->programmed) program(c, latest);
    spin_give(&c->lock);
    irq_restore(f);
}

bool alarm_cancel(struct alarm *a) {
    uint64_t f = irq_save();
    bool armed = claim(a);
    __atomic_store_n(&a->armed_on, 0, __ATOMIC_RELEASE);
    uint32_t self = cpu_current_index() + 1, firing;
    irq_restore(f);
    /* another CPU's fire of it ends without anything from this one (it shoots down nothing and
       takes no lock this caller holds), so this CPU may take its interrupts meanwhile or not */
    while ((firing = __atomic_load_n(&a->firing, __ATOMIC_ACQUIRE)) && firing != self) pause();
    return armed;
}

/* ---- firing ---- */

/* a, DUE: run by this CPU if it takes a's firing; else the fire that holds it runs it after. The
   stores of DUE and of firing's release are sequentially consistent, as are the CAS that read
   them, so of the CPU marking it DUE and the one ending a fire at least one sees the other's. */
static void run_due(struct alarm *a, struct regs *r, unsigned self) {
    while (__atomic_load_n(&a->armed_on, __ATOMIC_SEQ_CST) == ARMED_DUE) {
        uint32_t idle = 0;
        if (!__atomic_compare_exchange_n(&a->firing, &idle, self + 1, false, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST))
            return;
        uint32_t due = ARMED_DUE;
        if (__atomic_compare_exchange_n(&a->armed_on, &due, 0, false, __ATOMIC_SEQ_CST, __ATOMIC_RELAXED))
            a->fire(a, r);
        __atomic_store_n(&a->firing, 0, __ATOMIC_SEQ_CST);
    }
}

/* VEC_TIMER's handler (irq_request_system, alarm_init_cpu). */
static enum irq_result alarm_interrupt(void *context) {
    (void)context;
    struct regs *r = irq_regs();
    unsigned self = cpu_current_index();
    struct alarm_cpu *c = &per_cpu(alarms, self);
    spin_take(&c->lock);
    for (;;) {
        struct alarm *a = c->root;
        if (!a || a->early > clock_now_ns()) break;
        heap_remove(c, a);
        uint32_t idle = 0;
        bool mine = __atomic_compare_exchange_n(&a->firing, &idle, self + 1, false, __ATOMIC_SEQ_CST,
                                                __ATOMIC_SEQ_CST);
        __atomic_store_n(&a->armed_on, mine ? 0 : ARMED_DUE, __ATOMIC_SEQ_CST);
        spin_give(&c->lock);
        if (mine) {
            a->fire(a, r);
            __atomic_store_n(&a->firing, 0, __ATOMIC_SEQ_CST);
        }
        run_due(a, r, self);
        spin_take(&c->lock);
    }
    program(c, c->root ? c->root->at : CLOCK_NEVER);
    spin_give(&c->lock);
    return IRQ_HANDLED;
}

/* ---- the CPUs ---- */

void alarm_init_cpu(void) {
    unsigned self = cpu_current_index();
    struct alarm_cpu *c = &per_cpu(alarms, self);
    if (!self && !irq_request_system(&(struct irq_spec){.name = "alarm", .handler = alarm_interrupt, .node = -1},
                                     VEC_TIMER))
        panic("alarm: VEC_TIMER is taken");
    if (!lapic_wake_init(VEC_TIMER)) panic("alarm: cpu%u has no LAPIC timer to wake it", self);
    uint64_t f = spin_lock(&c->lock);
    c->ready = true;
    c->programmed = CLOCK_NEVER;
    program(c, c->root ? c->root->at : CLOCK_NEVER);
    spin_unlock(&c->lock, f);
}

void alarm_clock_changed(void) {
    preempt_disable(); /* this CPU's own device, and the others' through their interrupts */
    unsigned self = cpu_current_index();
    struct alarm_cpu *c = &per_cpu(alarms, self);
    uint64_t f = spin_lock(&c->lock);
    program(c, c->root ? c->root->at : CLOCK_NEVER);
    spin_unlock(&c->lock, f);
    for (unsigned i = 0; i < topology.cpus; i++)
        if (i != self) cpu_alarm(i); /* each sets its own as its interrupt ends (online ones only) */
    preempt_enable();
}

void alarm_cpu_leave(void) {
    unsigned self = cpu_current_index();
    if (!self) return;
    struct alarm_cpu *c = &per_cpu(alarms, self), *to = &per_cpu(alarms, 0);
    spin_take(&to->lock); /* CPU order */
    spin_take(&c->lock);
    lapic_wake_at(CLOCK_NEVER);
    c->ready = false;
    while (c->root) {
        struct alarm *a = c->root;
        heap_remove(c, a);
        heap_insert(to, a);
        __atomic_store_n(&a->armed_on, 1, __ATOMIC_RELEASE);
    }
    bool sooner = to->root && to->root->at < to->programmed;
    spin_give(&c->lock);
    spin_give(&to->lock);
    if (sooner) cpu_alarm(0); /* CPU 0 sets its own wake device */
}

void alarm_suspend(void) {
    struct alarm_cpu *c = &per_cpu(alarms, 0);
    spin_take(&c->lock);
    lapic_wake_at(CLOCK_NEVER);
    c->ready = false;
    spin_give(&c->lock);
}

void alarm_resume(void) {
    struct alarm_cpu *c = &per_cpu(alarms, 0);
    if (!lapic_wake_init(VEC_TIMER)) panic("alarm: no LAPIC timer after sleep");
    uint64_t f = spin_lock(&c->lock);
    c->ready = true;
    c->programmed = CLOCK_NEVER;
    program(c, c->root ? c->root->at : CLOCK_NEVER);
    spin_unlock(&c->lock, f);
}
