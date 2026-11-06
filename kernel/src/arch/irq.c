/* The interrupt layer (arch/irq.h): each CPU's vectors, what they deliver, and the threads that
   finish the work.

   Each CPU has a slot per vector (x86 has 256, SDM Vol. 3A 6.2): a message-signalled irq, a GSI's
   line (the irqs sharing an IOAPIC pin), or nothing. The system vectors are one table for every
   CPU. A device's vector is taken on the CPU the request chooses, so the vectors in use grow with
   the CPUs, not against a system-wide 256.

   Freeing waits out the handlers still running on the CPU (wait_dispatch): each CPU counts its
   dispatches, odd while one runs, so whoever cleared a slot and then sees an even count, or a new
   one, knows no handler holds what the slot held. A moved irq keeps its old slot too until it moves
   again or is freed: a message already on its way to the old vector is still handled there.

   A vector that a route named goes back to the free ones only from its own CPU (vector_retire,
   VEC_IRQ_DRAIN): the source is off by then, so what of it can still come is already in that CPU's
   IRR, and the vector is free once the IRR no longer holds it (an interrupt pending there is taken
   as the stray it is when it comes, and frees it then). */
#include "kernel.h"
#include "arch/cpu.h"
#include "arch/apic.h"
#include "arch/cpuset.h"
#include "arch/ioapic.h"
#include "arch/irq.h"
#include "arch/smp.h"
#include "arch/spin.h"
#include "arch/topology.h"
#include "dev/acpi.h"
#include "dev/entropy.h"
#include "mm/heap.h"
#include "sys/thread.h"
#include "sys/wait.h"

enum irq_kind { KIND_MSI, KIND_GSI, KIND_SYSTEM };
struct line;

struct irq {
    struct irq_spec spec;
    enum irq_kind kind;
    bool in_block;          /* one of an MSI block's: it moves with the others or not at all */
    uint32_t reach;         /* the highest APIC ID its route can name */
    unsigned cpu;           /* where it is delivered (a GSI's: its line's) */
    uint8_t vector;
    unsigned old_cpu;       /* a moved MSI's previous slot, while old_vector is not 0 */
    uint8_t old_vector;
    struct line *line;      /* a GSI's */
    struct irq *next;       /* the next irq of its line */
    bool (*write)(struct irq *irq, void *writer); /* irq_msi_writer */
    void *writer;
    /* the thread: what the lock guards; wq for the threads, done for the stopper */
    struct spinlock lock;
    bool pending;           /* a run owed */
    bool running;           /* a thread runs spec.thread now */
    bool holding;           /* its level line is masked for that run (hold_take) or by IRQ_HOLD */
    bool stopping;
    unsigned gen;           /* the current thread's; an older one ends, a newer one waits */
    unsigned threads;       /* alive */
    struct wait_queue wq, done;
};

/* A GSI's line: the irqs on one IOAPIC pin, in request order, a level one shared unless alone. */
struct line {
    unsigned gsi, cpu;
    uint8_t vector;
    unsigned old_cpu;
    uint8_t old_vector;
    uint32_t reach;         /* the pin's (ioapic_gsi_reach) */
    bool level, alone;
    struct irq *irqs;
    struct spinlock lock;   /* holds and dead; taken under an irq's lock */
    unsigned holds;         /* runs woken and not done: the pin masked while above 0 */
    bool dead;              /* masked for good: asserted with no handler taking it (line_count) */
    unsigned unhandled;     /* deliveries in a row no handler took; its CPU's handlers alone */
    struct line *next;
};

/* A level line delivered this many times in a row with none of its handlers taking it is asserted
   by something no driver quiets, and would hold its CPU in its handlers for ever. */
#define LINE_UNHANDLED_MOST 100000u

#define SLOT_LINE 1u /* the slot holds a line */
struct vectors {
    struct spinlock lock;   /* used, count, the slots' writes */
    uint64_t used[256 / 64];
    unsigned count;         /* device vectors in use: the CPU choice's measure */
    uintptr_t slot[256];    /* struct irq *, or struct line * | SLOT_LINE */
    uint64_t strays[256 / 64]; /* vectors reported once as having nothing behind them */
    uint64_t draining[256 / 64]; /* used, their routes gone, the IRR not yet seen clear of them */
};
static PERCPU struct vectors vectors;
static PERCPU uint64_t dispatches; /* odd while this CPU runs handlers */
static PERCPU struct regs *frame;

static struct irq *system_slot[256 - VEC_SYSTEM_FIRST];
static struct spinlock system_lock;
/* The requests, frees and moves of MSIs and GSIs (they make and stop threads), the lines list,
   each line's irqs, the vector counts the CPU choice reads. */
static struct mutex requests;
static struct line *lines;

static bool bit_test(const uint64_t *set, unsigned v) { return set[v / 64] >> (v % 64) & 1; }
static void bit_set(uint64_t *set, unsigned v) { set[v / 64] |= 1ULL << (v % 64); }
static void bit_clear(uint64_t *set, unsigned v) { set[v / 64] &= ~(1ULL << (v % 64)); }

/* ---- the threads ---- */

struct worker {
    struct irq *irq;
    unsigned gen;
};

/* The level line masked for irq (hold_take): masked until each hold is given back. irq->lock
   held, interrupts off. */
static void hold_take(struct irq *irq) {
    struct line *l = irq->line;
    if (!l || !l->level || irq->holding) return;
    irq->holding = true;
    spin_take(&l->lock);
    if (!l->holds++) ioapic_hold(l->gsi, true);
    spin_give(&l->lock);
}

/* The run the hard handler masked its level line for is done (thread_wake), or its owner quieted
   the device (irq_unhold): the pin open again once no other run holds it. irq->lock held. */
static void hold_give(struct irq *irq) {
    if (!irq->holding) return;
    irq->holding = false;
    struct line *l = irq->line;
    spin_take(&l->lock);
    if (!--l->holds) ioapic_hold(l->gsi, false);
    spin_give(&l->lock);
}

/* A run of spec.thread for each wake, never two at once. A thread whose generation is behind the
   irq's (it moved: thread_move) ends once its run is done; one ahead of it waits to take over. */
static void irq_thread(void *arg) {
    struct worker w = *(struct worker *)arg;
    kfree(arg);
    /* a device waits for its run: the shortest request, which the fair class picks first once it
       is eligible; it never delays a deadline thread's job */
    sched_set_policy(thread_current(), sched_fair(SCHED_WEIGHT_DEFAULT, SCHED_REQUEST_MIN_NS));
    struct irq *irq = w.irq;
    uint64_t f = spin_lock(&irq->lock);
    while (w.gen >= irq->gen && !irq->stopping) {
        if (w.gen != irq->gen || !irq->pending || irq->running) {
            wq_wait_locked(&irq->wq, &irq->lock);
            spin_take(&irq->lock);
            continue;
        }
        irq->pending = false;
        irq->running = true;
        spin_unlock(&irq->lock, f);
        irq->spec.thread(irq->spec.context);
        f = spin_lock(&irq->lock);
        irq->running = false;
        hold_give(irq);
    }
    if (irq->stopping && !irq->running) hold_give(irq); /* a run owed that never comes */
    irq->threads--;
    wq_wake_all(&irq->wq); /* a newer thread waits for this one's run to end */
    if (!irq->threads) wq_wake_all(&irq->done);
    spin_unlock(&irq->lock, f); /* irq is not touched from here on: irq_free may free it */
    thread_exit(0);
}

/* A thread of generation gen on that CPU; false without the memory. irq->threads counts it. */
static bool thread_make(struct irq *irq, unsigned gen, unsigned cpu) {
    struct worker *w = kmalloc(sizeof *w);
    if (!w) return false;
    *w = (struct worker){irq, gen};
    uint64_t f = spin_lock(&irq->lock);
    irq->threads++;
    spin_unlock(&irq->lock, f);
    if (kthread_create_on(irq->spec.name ? irq->spec.name : "irq", irq_thread, w, cpu)) return true;
    kfree(w);
    f = spin_lock(&irq->lock);
    irq->threads--;
    spin_unlock(&irq->lock, f);
    return false;
}

/* Its thread follows it to cpu: the new one takes over once the old one's run is done. False, and
   the old one kept, without the memory. */
static bool thread_move(struct irq *irq, unsigned cpu) {
    if (!irq->spec.thread) return true;
    if (!thread_make(irq, irq->gen + 1, cpu)) return false;
    uint64_t f = spin_lock(&irq->lock);
    irq->gen++;
    wq_wake_all(&irq->wq);
    spin_unlock(&irq->lock, f);
    return true;
}

/* Returns once none of its threads runs, nor will. */
static void thread_stop(struct irq *irq) {
    uint64_t f = spin_lock(&irq->lock);
    irq->stopping = true;
    wq_wake_all(&irq->wq);
    while (irq->threads) {
        wq_wait_locked(&irq->done, &irq->lock);
        spin_take(&irq->lock);
    }
    spin_unlock(&irq->lock, f);
}

/* The hard handler asked for a run: on a level line the pin is masked until it is done. Interrupts
   off. */
static void thread_wake(struct irq *irq) {
    if (!irq->spec.thread) return;
    spin_take(&irq->lock);
    irq->pending = true;
    hold_take(irq);
    wq_wake_all(&irq->wq);
    spin_give(&irq->lock);
}

/* IRQ_HOLD: a level line masked until irq_unhold. An irq with a thread holds for its runs. */
static void hold(struct irq *irq) {
    ASSERT(!irq->spec.thread);
    spin_take(&irq->lock);
    hold_take(irq);
    spin_give(&irq->lock);
}

void irq_unhold(struct irq *irq) {
    uint64_t f = spin_lock(&irq->lock);
    hold_give(irq);
    spin_unlock(&irq->lock, f);
}

/* ---- the dispatch ---- */

static bool run(struct irq *irq) {
    enum irq_result r = irq->spec.handler ? irq->spec.handler(irq->spec.context) : IRQ_WAKE_THREAD;
    switch (r) {
    case IRQ_NONE: return false;
    case IRQ_HANDLED: return true;
    case IRQ_WAKE_THREAD: thread_wake(irq); return true;
    case IRQ_HOLD: hold(irq); return true;
    }
    return false;
}

/* A level line's delivery, taken or not by its handlers: one that goes on with none taking it is
   masked for good (LINE_UNHANDLED_MOST), as nothing will deassert it. On its CPU, interrupts off. */
static void line_count(struct line *l, bool taken) {
    if (taken) {
        l->unhandled = 0;
        return;
    }
    if (++l->unhandled < LINE_UNHANDLED_MOST) return;
    spin_take(&l->lock);
    bool first = !l->dead;
    if (first) {
        l->dead = true;
        if (!l->holds++) ioapic_hold(l->gsi, true); /* a hold no run gives back */
    }
    spin_give(&l->lock);
    if (first) kprintf("irq: GSI %u stays asserted and no driver takes it: masked\n", l->gsi);
}

/* The vector is free again once this CPU's IRR does not hold it: what the retired route sent
   before its source went off has come. vs is this CPU's, its lock held. */
static void drain_one(struct vectors *vs, unsigned v) {
    if (apic_irr((uint8_t)v)) return;
    bit_clear(vs->draining, v);
    bit_clear(vs->used, v);
    __atomic_store_n(&vs->count, vs->count - 1, __ATOMIC_RELAXED);
}

/* VEC_IRQ_DRAIN: the vectors whose routes this CPU was told are gone. */
static enum irq_result drain(void *context) {
    (void)context;
    struct vectors *vs = this_cpu_ptr(vectors);
    spin_take(&vs->lock);
    for (unsigned w = 0; w < 256 / 64; w++)
        for (uint64_t bits = vs->draining[w]; bits; bits &= bits - 1) drain_one(vs, w * 64 + __builtin_ctzll(bits));
    spin_give(&vs->lock);
    return IRQ_HANDLED;
}

/* A vector whose slot is empty came: the last of a retired route, or a stray, told once. */
static void vector_empty(unsigned v) {
    struct vectors *vs = this_cpu_ptr(vectors);
    spin_take(&vs->lock);
    bool draining = bit_test(vs->draining, v), told = draining || bit_test(vs->strays, v);
    if (draining) drain_one(vs, v);
    else bit_set(vs->strays, v);
    spin_give(&vs->lock);
    if (!told) kprintf("irq: cpu%u took vector %02x, which nothing has\n", cpu_current_index(), v);
}

/* Every CPU's interrupts (vector 32 and up), on its interrupt stack with interrupts off: their
   handlers, then the EOI, at the end of the handling and before IRET (SDM 13.8.5), so that the
   APIC holds back this vector's and lower priority classes meanwhile (13.8.3). The spurious vector
   sets no in-service bit and takes no EOI (13.9). */
void isr_interrupt(struct regs *r) {
    unsigned v = (unsigned)r->vector;
    if (v == VEC_SPURIOUS) return;
    uint64_t *count = this_cpu_ptr(dispatches);
    /* odd before the slot is read, and locked (a full barrier): a free that cleared the slot sees
       either this count or the slot's new contents here (wait_dispatch) */
    __atomic_fetch_add(count, 1, __ATOMIC_SEQ_CST);
    struct regs *outer = this_cpu(frame);
    this_cpu(frame) = r;
    uintptr_t s;
    if (v >= VEC_SYSTEM_FIRST) {
        s = (uintptr_t)__atomic_load_n(&system_slot[v - VEC_SYSTEM_FIRST], __ATOMIC_ACQUIRE);
    } else {
        entropy_add(rdtsc() ^ (uint64_t)v << 56);
        s = __atomic_load_n(&this_cpu_ptr(vectors)->slot[v], __ATOMIC_ACQUIRE);
    }
    if (s & SLOT_LINE) {
        struct line *l = (struct line *)(s & ~(uintptr_t)SLOT_LINE);
        bool taken = false;
        for (struct irq *irq = __atomic_load_n(&l->irqs, __ATOMIC_ACQUIRE); irq;
             irq = __atomic_load_n(&irq->next, __ATOMIC_ACQUIRE))
            taken |= run(irq);
        if (l->level) line_count(l, taken);
    } else if (s) {
        run((struct irq *)s);
    }
    this_cpu(frame) = outer;
    __atomic_fetch_add(count, 1, __ATOMIC_RELEASE);
    apic_eoi();
    if (!s) vector_empty(v);
}

struct regs *irq_regs(void) { return this_cpu(frame); }

/* Once the slot that held something no longer does: returns when cpu runs no handler that read it
   before. */
static void wait_dispatch(unsigned cpu) {
    __atomic_thread_fence(__ATOMIC_SEQ_CST);
    const uint64_t *count = &per_cpu(dispatches, cpu);
    uint64_t seen = __atomic_load_n(count, __ATOMIC_ACQUIRE);
    if (seen & 1)
        while (__atomic_load_n(count, __ATOMIC_ACQUIRE) == seen) pause();
}

/* ---- the vectors ---- */

static void slot_set(unsigned cpu, uint8_t v, uintptr_t s) {
    struct vectors *vs = &per_cpu(vectors, cpu);
    uint64_t f = spin_lock(&vs->lock);
    __atomic_store_n(&vs->slot[v], s, __ATOMIC_RELEASE);
    bit_clear(vs->strays, v);
    spin_unlock(&vs->lock, f);
}

/* A vector taken and never named by a route: free again at once. */
static void vector_release(unsigned cpu, uint8_t v) {
    struct vectors *vs = &per_cpu(vectors, cpu);
    uint64_t f = spin_lock(&vs->lock);
    __atomic_store_n(&vs->slot[v], 0, __ATOMIC_RELEASE);
    bit_clear(vs->used, v);
    __atomic_store_n(&vs->count, vs->count - 1, __ATOMIC_RELAXED);
    spin_unlock(&vs->lock, f);
}

/* A vector a route named, its source turned off: its slot emptied, and free again once cpu has
   seen nothing of it pending (drain). wait_dispatch before freeing what the slot held. */
static void vector_retire(unsigned cpu, uint8_t v) {
    struct vectors *vs = &per_cpu(vectors, cpu);
    uint64_t f = spin_lock(&vs->lock);
    __atomic_store_n(&vs->slot[v], 0, __ATOMIC_RELEASE);
    bit_set(vs->draining, v);
    bit_clear(vs->strays, v);
    spin_unlock(&vs->lock, f);
    apic_send_ipi(cpu, VEC_IRQ_DRAIN);
}

/* A device's interrupt can name the CPU: online, its APIC ID within the source's reach. */
static bool nameable(unsigned cpu, uint32_t reach) {
    return cpu < topology.cpus && cpu_online_at(cpu) && topology.cpu[cpu].apic_id <= reach;
}

/* count vectors (a power of two) aligned to count among cpu's device vectors, taken: the first,
   or 0 when no such run is free. The lowest free one, the lowest priority class (13.8.3). */
static uint8_t vectors_take(unsigned cpu, unsigned count) {
    struct vectors *vs = &per_cpu(vectors, cpu);
    uint8_t found = 0;
    uint64_t f = spin_lock(&vs->lock);
    for (unsigned v = (unsigned)ALIGN_UP(VEC_DEVICE_FIRST, count); !found && v + count - 1 <= VEC_DEVICE_LAST; v += count) {
        unsigned i = 0;
        while (i < count && !bit_test(vs->used, v + i)) i++;
        if (i < count) continue;
        for (i = 0; i < count; i++) bit_set(vs->used, v + i);
        __atomic_store_n(&vs->count, vs->count + count, __ATOMIC_RELAXED);
        found = (uint8_t)v;
    }
    spin_unlock(&vs->lock, f);
    return found;
}

/* A CPU within reach for count vectors and the vectors on it: hint when it is a CPU, else in node
   first (any node when -1) the CPU with the fewest device vectors in use, the next fewest when that
   one has no aligned run free. requests held: the counts move only under it. */
static bool place(int node, unsigned hint, unsigned count, uint32_t reach, unsigned *cpu, uint8_t *vector) {
    if (hint != IRQ_CPU_ANY) {
        *cpu = hint;
        return nameable(hint, reach) && (*vector = vectors_take(hint, count)) != 0;
    }
    for (int pass = node < 0; pass < 2; pass++) {
        unsigned after_count = 0, after_cpu = 0; /* past (count, index) pairs already tried */
        bool tried = false;
        for (;;) {
            unsigned best = topology.cpus, least = 0;
            cpuset_each(c, cpu_online_set(), topology.cpus) {
                if ((!pass && topology.cpu[c].node != (unsigned)node) || !nameable(c, reach)) continue;
                unsigned n = __atomic_load_n(&per_cpu(vectors, c).count, __ATOMIC_RELAXED);
                if (tried && (n < after_count || (n == after_count && c <= after_cpu))) continue;
                if (best == topology.cpus || n < least) {
                    best = c;
                    least = n;
                }
            }
            if (best == topology.cpus) break;
            if ((*vector = vectors_take(best, count))) {
                *cpu = best;
                return true;
            }
            after_count = least;
            after_cpu = best;
            tried = true;
        }
    }
    return false;
}

/* ---- requests ---- */

static struct irq *irq_new(const struct irq_spec *spec, enum irq_kind kind) {
    if (!spec->handler && !spec->thread) return NULL;
    struct irq *irq = kzalloc(sizeof *irq);
    if (irq) {
        irq->spec = *spec;
        irq->kind = kind;
    }
    return irq;
}

bool irq_request_msi_block(const struct irq_spec *specs, unsigned count, unsigned cpu, uint32_t reach,
                           struct irq **irqs) {
    /* MSI's Multiple Message Enable: up to 32, a power of two (PCI Local Bus 3.0, 6.8.1.3) */
    if (!count || count > 32 || (count & (count - 1))) return false;
    struct irq *made[32] = {0};
    unsigned i, at = 0;
    uint8_t v = 0;
    for (i = 0; i < count; i++)
        if (!(made[i] = irq_new(&specs[i], KIND_MSI))) goto fail;
    mutex_lock(&requests);
    if (!place(specs[0].node, cpu, count, reach, &at, &v)) {
        mutex_unlock(&requests);
        goto fail;
    }
    for (i = 0; i < count; i++) {
        *made[i] = (struct irq){.spec = made[i]->spec, .kind = KIND_MSI, .in_block = count > 1, .reach = reach, .cpu = at,
                                .vector = (uint8_t)(v + i)};
        if (made[i]->spec.thread && !thread_make(made[i], 0, at)) {
            while (i--) thread_stop(made[i]);
            for (i = 0; i < count; i++) vector_release(at, (uint8_t)(v + i));
            mutex_unlock(&requests);
            goto fail;
        }
    }
    for (i = 0; i < count; i++) slot_set(at, (uint8_t)(v + i), (uintptr_t)made[i]);
    mutex_unlock(&requests);
    for (i = 0; i < count; i++) irqs[i] = made[i];
    return true;
fail:
    for (i = 0; i < count; i++) kfree(made[i]);
    return false;
}

struct irq *irq_request_msi(const struct irq_spec *spec, unsigned cpu, uint32_t reach) {
    struct irq *irq;
    return irq_request_msi_block(spec, 1, cpu, reach, &irq) ? irq : NULL;
}

void irq_msi_writer(struct irq *irq, bool (*write)(struct irq *irq, void *writer), void *writer) {
    mutex_lock(&requests);
    irq->write = write;
    irq->writer = writer;
    mutex_unlock(&requests);
}

bool irq_msi_rewrite(struct irq *irq) {
    mutex_lock(&requests);
    bool ok = irq->write && irq->write(irq, irq->writer);
    mutex_unlock(&requests);
    return ok;
}

struct irq *irq_request_gsi(const struct irq_spec *spec, unsigned gsi) {
    struct irq *irq = irq_new(spec, KIND_GSI);
    struct line *fresh = kzalloc(sizeof *fresh), *l;
    if (!irq || !fresh) goto fail;
    mutex_lock(&requests);
    for (l = lines; l && l->gsi != gsi; l = l->next) {}
    bool level, made = !l;
    if (made) {
        if (!ioapic_gsi_owned(gsi, &level)) goto unlock;
        l = fresh;
        *l = (struct line){.gsi = gsi, .level = level, .alone = spec->alone, .reach = ioapic_gsi_reach(gsi)};
        if (!place(spec->node, IRQ_CPU_ANY, 1, l->reach, &l->cpu, &l->vector)) goto unlock;
    } else if (!l->level || l->alone || spec->alone) {
        goto unlock; /* an edge line is no one else's: two sources' edges would merge */
    }
    irq->line = l;
    irq->reach = l->reach;
    irq->cpu = l->cpu;
    irq->vector = l->vector;
    if (spec->thread && !thread_make(irq, 0, l->cpu)) {
        if (made) vector_release(l->cpu, l->vector);
        goto unlock;
    }
    struct irq **tail = &l->irqs;
    while (*tail) tail = &(*tail)->next;
    __atomic_store_n(tail, irq, __ATOMIC_RELEASE);
    if (made) {
        slot_set(l->cpu, l->vector, (uintptr_t)l | SLOT_LINE);
        if (!ioapic_route(gsi, l->vector, topology.cpu[l->cpu].apic_id)) { /* the pin stays masked */
            vector_release(l->cpu, l->vector);
            wait_dispatch(l->cpu);
            mutex_unlock(&requests);
            thread_stop(irq);
            goto fail;
        }
        l->next = lines;
        lines = l;
        fresh = NULL;
    }
    mutex_unlock(&requests);
    kfree(fresh);
    return irq;
unlock:
    mutex_unlock(&requests);
fail:
    kfree(fresh);
    kfree(irq);
    return NULL;
}

struct irq *irq_request_system(const struct irq_spec *spec, unsigned vector) {
    if (vector < VEC_SYSTEM_FIRST || vector >= VEC_SPURIOUS || spec->thread || !spec->handler) return NULL;
    struct irq *irq = irq_new(spec, KIND_SYSTEM);
    if (!irq) return NULL;
    irq->vector = (uint8_t)vector;
    irq->cpu = IRQ_CPU_ANY;
    uint64_t f = spin_lock(&system_lock);
    struct irq **s = &system_slot[vector - VEC_SYSTEM_FIRST];
    bool free = !*s;
    if (free) __atomic_store_n(s, irq, __ATOMIC_RELEASE);
    spin_unlock(&system_lock, f);
    if (free) return irq;
    kfree(irq);
    return NULL;
}

void irq_free(struct irq *irq) {
    if (!irq) return;
    if (irq->kind == KIND_SYSTEM) {
        uint64_t f = spin_lock(&system_lock);
        __atomic_store_n(&system_slot[irq->vector - VEC_SYSTEM_FIRST], NULL, __ATOMIC_RELEASE);
        spin_unlock(&system_lock, f);
        for (unsigned c = 0; c < topology.cpus; c++) wait_dispatch(c);
        kfree(irq);
        return;
    }
    struct line *gone = NULL;
    mutex_lock(&requests);
    if (irq->kind == KIND_MSI) {
        vector_retire(irq->cpu, irq->vector);
        wait_dispatch(irq->cpu);
        if (irq->old_vector) {
            vector_retire(irq->old_cpu, irq->old_vector);
            wait_dispatch(irq->old_cpu);
        }
    } else {
        struct line *l = irq->line;
        struct irq **at = &l->irqs;
        while (*at != irq) at = &(*at)->next;
        __atomic_store_n(at, irq->next, __ATOMIC_RELEASE);
        if (!l->irqs) {
            ioapic_unroute(l->gsi);
            vector_retire(l->cpu, l->vector);
            if (l->old_vector) vector_retire(l->old_cpu, l->old_vector);
            struct line **in = &lines;
            while (*in != l) in = &(*in)->next;
            *in = l->next;
            gone = l;
        }
        wait_dispatch(l->cpu);
        if (l->old_vector) wait_dispatch(l->old_cpu);
    }
    mutex_unlock(&requests);
    thread_stop(irq); /* its hold on the line given back: the line is still there */
    if (!irq->spec.thread) irq_unhold(irq); /* an IRQ_HOLD's */
    kfree(gone);
    kfree(irq);
}

/* ---- moves ---- */

static bool msi_move(struct irq *irq, unsigned cpu) {
    uint8_t v;
    if (!place(irq->spec.node, cpu, 1, irq->reach, &cpu, &v)) return false;
    if (irq->old_vector) vector_retire(irq->old_cpu, irq->old_vector); /* the move before this one's */
    irq->old_vector = 0;
    slot_set(cpu, v, (uintptr_t)irq); /* there before any message names it */
    unsigned from = irq->cpu;
    uint8_t from_vector = irq->vector;
    irq->cpu = cpu;
    irq->vector = v;
    if (!irq->write(irq, irq->writer)) {
        irq->cpu = from;
        irq->vector = from_vector;
        vector_release(cpu, v);
        return false;
    }
    irq->old_cpu = from;
    irq->old_vector = from_vector;
    if (!thread_move(irq, cpu))
        kprintf("irq: %s's thread stays on cpu%u, there being no memory for one on cpu%u\n", irq->spec.name, from, cpu);
    return true;
}

static bool line_move(struct line *l, unsigned cpu) {
    uint8_t v;
    if (!place(-1, cpu, 1, l->reach, &cpu, &v)) return false;
    if (l->old_vector) vector_retire(l->old_cpu, l->old_vector);
    l->old_vector = 0;
    slot_set(cpu, v, (uintptr_t)l | SLOT_LINE);
    if (!ioapic_route(l->gsi, v, topology.cpu[cpu].apic_id)) { /* masked: nothing came on v */
        vector_release(cpu, v);
        if (!ioapic_route(l->gsi, l->vector, topology.cpu[l->cpu].apic_id))
            kprintf("irq: GSI %u could not be routed again, and stays masked\n", l->gsi);
        return false;
    }
    l->old_cpu = l->cpu;
    l->old_vector = l->vector;
    l->cpu = cpu;
    l->vector = v;
    for (struct irq *irq = l->irqs; irq; irq = irq->next) {
        irq->cpu = cpu;
        irq->vector = v;
        if (!thread_move(irq, cpu))
            kprintf("irq: %s's thread stays on cpu%u, there being no memory for one on cpu%u\n", irq->spec.name,
                    l->old_cpu, cpu);
    }
    return true;
}

bool irq_set_cpu(struct irq *irq, unsigned cpu) {
    if (irq->kind == KIND_SYSTEM || irq->in_block) return false;
    mutex_lock(&requests);
    bool ok = irq->kind == KIND_GSI ? irq->line->cpu == cpu || (!irq->line->irqs->next && line_move(irq->line, cpu))
                                    : irq->cpu == cpu || (irq->write && msi_move(irq, cpu));
    mutex_unlock(&requests);
    return ok;
}

struct irq_target irq_target(const struct irq *irq) {
    if (irq->kind == KIND_SYSTEM) return (struct irq_target){IRQ_CPU_ANY, 0, irq->vector};
    return (struct irq_target){irq->cpu, topology.cpu[irq->cpu].apic_id, irq->vector};
}

/* ---- the controllers ---- */

/* The 8259s, where the MADT says the machine has them (PCAT_COMPAT, ACPI 6.5 5.2.12): moved off
   the exceptions' vectors and every line masked (8259A: ICW1 edge, cascade, ICW4 to come; ICW2 the
   base; ICW3 the slave on IR2; ICW4 8086 mode; OCW1 the masks). Nothing of theirs is used: the
   IOAPICs carry the ISA interrupts, and LINT0 is masked (apic_init_cpu). */
#define MADT_FLAGS 40
#define MADT_PCAT_COMPAT 1u
static void pic_disable(void) {
    const uint8_t *madt = acpi_find("APIC", 0);
    uint32_t flags = 0;
    if (madt) memcpy(&flags, madt + MADT_FLAGS, sizeof flags);
    if (!(flags & MADT_PCAT_COMPAT)) return;
    outb(0x20, 0x11);
    outb(0xA0, 0x11);
    outb(0x21, VEC_PIC_BASE);
    outb(0xA1, VEC_PIC_BASE + 8);
    outb(0x21, 0x04);
    outb(0xA1, 0x02);
    outb(0x21, 0x01);
    outb(0xA1, 0x01);
    outb(0x21, 0xFF);
    outb(0xA1, 0xFF);
}

void irq_init(void) {
    pic_disable();
    ioapic_init();
    apic_init();
    if (!apic_init_cpu()) panic("irq: the first CPU's local APIC does not enter x2APIC mode");
    if (!irq_request_system(&(struct irq_spec){.name = "irq drain", .handler = drain, .node = -1}, VEC_IRQ_DRAIN))
        panic("irq: the drain vector is taken");
}

void irq_suspend(void) { ioapic_suspend(); }

/* The sleep reset every APIC, its IRR with it: nothing a retired route sent is pending anywhere. */
static void drained_all(void) {
    for (unsigned c = 0; c < topology.cpus; c++) {
        struct vectors *vs = &per_cpu(vectors, c);
        uint64_t f = spin_lock(&vs->lock);
        for (unsigned w = 0; w < 256 / 64; w++) {
            unsigned n = (unsigned)__builtin_popcountll(vs->draining[w]);
            vs->used[w] &= ~vs->draining[w];
            vs->draining[w] = 0;
            __atomic_store_n(&vs->count, vs->count - n, __ATOMIC_RELAXED);
        }
        spin_unlock(&vs->lock, f);
    }
}

void irq_resume(void) {
    pic_disable();
    drained_all();
    ioapic_resume();
}
