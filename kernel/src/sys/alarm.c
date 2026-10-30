/* Alarms (sys/alarm.h). Each CPU keeps the alarms armed on it in a pairing heap, earliest at the
   root, under its own lock: arming is O(1), taking the earliest or any other one out O(log n)
   amortized, so many alarms (every sleeper's deadline, every connection's timers) cost little and
   no CPU's alarms wait on another's. The CPU's wake device (arch/apic.c) is set for the root;
   when it comes, every alarm that is due fires and the device is set for the next. It may come
   early (a count rounded down, a stale setting after the root was cancelled): then nothing is
   due, and it is set again. */
#include "kernel.h"
#include "arch/cpu.h"
#include "arch/smp.h"
#include "arch/spin.h"
#include "sys/alarm.h"
#include "sys/clock.h"

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

/* Interrupts off: a out of whichever heap holds it. True when one did. */
static bool unlink(struct alarm *a) {
    for (;;) {
        uint32_t on = __atomic_load_n(&a->armed_on, __ATOMIC_ACQUIRE);
        if (!on) return false;
        struct alarm_cpu *c = &per_cpu(alarms, on - 1);
        spin_take(&c->lock);
        if (__atomic_load_n(&a->armed_on, __ATOMIC_RELAXED) != on) { /* moved meanwhile */
            spin_give(&c->lock);
            continue;
        }
        heap_remove(c, a);
        __atomic_store_n(&a->armed_on, 0, __ATOMIC_RELEASE);
        spin_give(&c->lock);
        return true;
    }
}

void alarm_set(struct alarm *a, uint64_t at) {
    uint64_t f = irq_save();
    unlink(a);
    unsigned self = cpu_current_index();
    struct alarm_cpu *c = &per_cpu(alarms, self);
    spin_take(&c->lock);
    a->at = at;
    heap_insert(c, a);
    __atomic_store_n(&a->armed_on, self + 1, __ATOMIC_RELEASE);
    if (c->root == a && at < c->programmed) program(c, at);
    spin_give(&c->lock);
    irq_restore(f);
}

bool alarm_cancel(struct alarm *a) {
    uint64_t f = irq_save();
    bool armed = unlink(a);
    uint32_t self = cpu_current_index() + 1, firing;
    while ((firing = __atomic_load_n(&a->firing, __ATOMIC_ACQUIRE)) && firing != self) {
        vmm_tlb_poll();
        pause();
    }
    irq_restore(f);
    return armed;
}

/* ---- the wake device's interrupt ---- */

void alarm_interrupt(struct regs *r) {
    unsigned self = cpu_current_index();
    struct alarm_cpu *c = &per_cpu(alarms, self);
    spin_take(&c->lock);
    for (;;) {
        uint64_t now = clock_now_ns();
        struct alarm *a = c->root;
        if (!a || a->at > now) break;
        heap_remove(c, a);
        __atomic_store_n(&a->firing, self + 1, __ATOMIC_RELAXED);
        __atomic_store_n(&a->armed_on, 0, __ATOMIC_RELEASE);
        spin_give(&c->lock);
        a->fire(a, r);
        uint32_t mine = self + 1;
        __atomic_compare_exchange_n(&a->firing, &mine, 0, false, __ATOMIC_RELEASE, __ATOMIC_RELAXED);
        spin_take(&c->lock);
    }
    program(c, c->root ? c->root->at : CLOCK_NEVER);
    spin_give(&c->lock);
}

void alarm_init_cpu(void) {
    unsigned self = cpu_current_index();
    struct alarm_cpu *c = &per_cpu(alarms, self);
    if (!self) vector_register(VEC_TIMER, alarm_interrupt);
    if (!lapic_wake_init(VEC_TIMER)) panic("alarm: cpu%u has no LAPIC timer to wake it", self);
    uint64_t f = spin_lock(&c->lock);
    c->ready = true;
    c->programmed = CLOCK_NEVER;
    program(c, c->root ? c->root->at : CLOCK_NEVER);
    spin_unlock(&c->lock, f);
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

void alarm_resume(void) {
    struct alarm_cpu *c = &per_cpu(alarms, 0);
    if (!lapic_wake_init(VEC_TIMER)) panic("alarm: no LAPIC timer after sleep");
    uint64_t f = spin_lock(&c->lock);
    c->programmed = CLOCK_NEVER;
    program(c, c->root ? c->root->at : CLOCK_NEVER);
    spin_unlock(&c->lock, f);
}
