/* The interrupt layer (arch/irq.h): each CPU's vectors, what they deliver, and the threads that
   finish the work.

   Each CPU has a slot per vector (x86 has 256, SDM Vol. 3A 6.2): a message-signalled irq, a GSI's
   line (the irqs sharing an IOAPIC pin), or nothing. The system vectors are one table for every
   CPU. A device's vector is taken on the CPU the request chooses, so the vectors in use grow with
   the CPUs, not against a system-wide 256.

   Freeing waits out the handlers still running on the CPU (wait_dispatch): each CPU counts its
   dispatches, odd while one runs, so whoever cleared a slot and then sees an even count, or a new
   one, knows no handler holds what the slot held. A moved irq keeps its old slot too until it moves
   again or is freed: a message already on its way to the old vector is still handled there. */
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
#include "dev/iommu.h"
#include "mm/heap.h"
#include "sys/thread.h"
#include "sys/wait.h"

enum irq_kind { KIND_MSI, KIND_GSI, KIND_SYSTEM };
struct line;

struct irq {
    struct irq_spec spec;
    enum irq_kind kind;
    bool in_block;          /* one of an MSI block's: it moves with the others or not at all */
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
    bool holding;           /* its level line is masked for that run (line_hold) */
    bool stopping;
    unsigned gen;           /* the current thread's; an older one ends, a newer one waits */
    unsigned threads;       /* alive */
    struct wait_queue wq, done;
};

/* A GSI's line: the irqs on one IOAPIC pin, in request order, a level one shared. */
struct line {
    unsigned gsi, cpu;
    uint8_t vector;
    unsigned old_cpu;
    uint8_t old_vector;
    bool level;
    struct irq *irqs;
    struct spinlock lock;   /* holds; taken under an irq's lock */
    unsigned holds;         /* runs woken and not done: the pin masked while above 0 */
    struct line *next;
};

#define SLOT_LINE 1u /* the slot holds a line */
struct vectors {
    struct spinlock lock;   /* used, count, the slots' writes */
    uint64_t used[256 / 64];
    unsigned count;         /* device vectors in use: the CPU choice's measure */
    uintptr_t slot[256];    /* struct irq *, or struct line * | SLOT_LINE */
    uint64_t strays[256 / 64]; /* vectors reported once as having nothing behind them */
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

/* The run the hard handler masked its level line for is done (thread_wake): the pin open again
   once no other run holds it. irq->lock held. */
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
    sched_set_policy(thread_current(), sched_realtime(SCHED_PRIORITY_IRQ)); /* a device waits for its run */
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
    struct line *l = irq->line;
    if (l && l->level && !irq->holding) {
        irq->holding = true;
        spin_take(&l->lock);
        if (!l->holds++) ioapic_hold(l->gsi, true);
        spin_give(&l->lock);
    }
    wq_wake_all(&irq->wq);
    spin_give(&irq->lock);
}

/* ---- the dispatch ---- */

static bool run(struct irq *irq) {
    enum irq_result r = irq->spec.handler ? irq->spec.handler(irq->spec.context) : IRQ_WAKE_THREAD;
    if (r == IRQ_WAKE_THREAD) thread_wake(irq);
    return r != IRQ_NONE;
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
        for (struct irq *irq = __atomic_load_n(&((struct line *)(s & ~(uintptr_t)SLOT_LINE))->irqs, __ATOMIC_ACQUIRE);
             irq; irq = __atomic_load_n(&irq->next, __ATOMIC_ACQUIRE))
            run(irq);
    } else if (s) {
        run((struct irq *)s);
    }
    this_cpu(frame) = outer;
    __atomic_fetch_add(count, 1, __ATOMIC_RELEASE);
    apic_eoi();
    if (!s) {
        struct vectors *vs = this_cpu_ptr(vectors);
        spin_take(&vs->lock);
        bool told = bit_test(vs->strays, v);
        bit_set(vs->strays, v);
        spin_give(&vs->lock);
        if (!told) kprintf("irq: cpu%u took vector %02x, which nothing has\n", cpu_current_index(), v);
    }
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

/* The slot emptied and the vector given back; wait_dispatch before freeing what it held. */
static void vector_give(unsigned cpu, uint8_t v) {
    struct vectors *vs = &per_cpu(vectors, cpu);
    uint64_t f = spin_lock(&vs->lock);
    __atomic_store_n(&vs->slot[v], 0, __ATOMIC_RELEASE);
    bit_clear(vs->used, v);
    __atomic_store_n(&vs->count, vs->count - 1, __ATOMIC_RELAXED);
    spin_unlock(&vs->lock, f);
}

/* A device's interrupt can name the CPU: online, and an APIC ID of 8 bits unless interrupt
   remapping carries 32 (SDM 13.11.1; 82093AA). */
static bool nameable(unsigned cpu) {
    return cpu < topology.cpus && cpu_online_at(cpu) && (topology.cpu[cpu].apic_id <= 0xFF || iommu_irq_wide());
}

/* A CPU for count vectors and the vectors on it: hint when it is a CPU, else in node first (any
   node when -1) the CPU with the fewest device vectors in use, the next fewest when that one has
   no aligned run free. requests held: the counts move only under it. */
static bool place(int node, unsigned hint, unsigned count, unsigned *cpu, uint8_t *vector) {
    if (hint != IRQ_CPU_ANY) {
        *cpu = hint;
        return nameable(hint) && (*vector = vectors_take(hint, count)) != 0;
    }
    for (int pass = node < 0; pass < 2; pass++) {
        unsigned after_count = 0, after_cpu = 0; /* past (count, index) pairs already tried */
        bool tried = false;
        for (;;) {
            unsigned best = topology.cpus, least = 0;
            cpuset_each(c, cpu_online_set(), topology.cpus) {
                if ((!pass && topology.cpu[c].node != (unsigned)node) || !nameable(c)) continue;
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

bool irq_request_msi_block(const struct irq_spec *specs, unsigned count, unsigned cpu, struct irq **irqs) {
    /* MSI's Multiple Message Enable: up to 32, a power of two (PCI Local Bus 3.0, 6.8.1.3) */
    if (!count || count > 32 || (count & (count - 1))) return false;
    struct irq *made[32] = {0};
    unsigned i, at = 0;
    uint8_t v = 0;
    for (i = 0; i < count; i++)
        if (!(made[i] = irq_new(&specs[i], KIND_MSI))) goto fail;
    mutex_lock(&requests);
    if (!place(specs[0].node, cpu, count, &at, &v)) {
        mutex_unlock(&requests);
        goto fail;
    }
    for (i = 0; i < count; i++) {
        *made[i] = (struct irq){.spec = made[i]->spec, .kind = KIND_MSI, .in_block = count > 1, .cpu = at,
                                .vector = (uint8_t)(v + i)};
        if (made[i]->spec.thread && !thread_make(made[i], 0, at)) {
            while (i--) thread_stop(made[i]);
            for (i = 0; i < count; i++) vector_give(at, (uint8_t)(v + i));
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

struct irq *irq_request_msi(const struct irq_spec *spec, unsigned cpu) {
    struct irq *irq;
    return irq_request_msi_block(spec, 1, cpu, &irq) ? irq : NULL;
}

/* The plain message (SDM 13.11.1, 13.11.2): the APIC ID in address bits 19:12, physical, no
   redirection hint; fixed delivery and edge in the data with the vector. */
void irq_msi_message(const struct irq *irq, uint64_t *address, uint32_t *data) {
    *address = 0xFEE00000u | (uint64_t)(topology.cpu[irq->cpu].apic_id & 0xFF) << 12;
    *data = irq->vector;
}

void irq_msi_writer(struct irq *irq, bool (*write)(struct irq *irq, void *writer), void *writer) {
    mutex_lock(&requests);
    irq->write = write;
    irq->writer = writer;
    mutex_unlock(&requests);
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
        *l = (struct line){.gsi = gsi, .level = level};
        if (!place(spec->node, IRQ_CPU_ANY, 1, &l->cpu, &l->vector)) goto unlock;
    } else if (!l->level) {
        goto unlock; /* an edge line is no one else's: two sources' edges would merge */
    }
    irq->line = l;
    irq->cpu = l->cpu;
    irq->vector = l->vector;
    if (spec->thread && !thread_make(irq, 0, l->cpu)) {
        if (made) vector_give(l->cpu, l->vector);
        goto unlock;
    }
    struct irq **tail = &l->irqs;
    while (*tail) tail = &(*tail)->next;
    __atomic_store_n(tail, irq, __ATOMIC_RELEASE);
    if (made) {
        slot_set(l->cpu, l->vector, (uintptr_t)l | SLOT_LINE);
        if (!ioapic_route(gsi, l->vector, topology.cpu[l->cpu].apic_id)) {
            vector_give(l->cpu, l->vector);
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
        vector_give(irq->cpu, irq->vector);
        wait_dispatch(irq->cpu);
        if (irq->old_vector) {
            vector_give(irq->old_cpu, irq->old_vector);
            wait_dispatch(irq->old_cpu);
        }
    } else {
        struct line *l = irq->line;
        struct irq **at = &l->irqs;
        while (*at != irq) at = &(*at)->next;
        __atomic_store_n(at, irq->next, __ATOMIC_RELEASE);
        if (!l->irqs) {
            ioapic_unroute(l->gsi);
            vector_give(l->cpu, l->vector);
            if (l->old_vector) vector_give(l->old_cpu, l->old_vector);
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
    kfree(gone);
    kfree(irq);
}

/* ---- moves ---- */

static bool msi_move(struct irq *irq, unsigned cpu) {
    uint8_t v;
    if (!place(irq->spec.node, cpu, 1, &cpu, &v)) return false;
    if (irq->old_vector) vector_give(irq->old_cpu, irq->old_vector); /* the move before this one's */
    irq->old_vector = 0;
    slot_set(cpu, v, (uintptr_t)irq); /* there before any message names it */
    unsigned from = irq->cpu;
    uint8_t from_vector = irq->vector;
    irq->cpu = cpu;
    irq->vector = v;
    if (!irq->write(irq, irq->writer)) {
        irq->cpu = from;
        irq->vector = from_vector;
        vector_give(cpu, v);
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
    if (!place(-1, cpu, 1, &cpu, &v)) return false;
    if (l->old_vector) vector_give(l->old_cpu, l->old_vector);
    l->old_vector = 0;
    slot_set(cpu, v, (uintptr_t)l | SLOT_LINE);
    if (!ioapic_route(l->gsi, v, topology.cpu[cpu].apic_id)) {
        vector_give(cpu, v);
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
    bool ok = irq->kind == KIND_GSI ? irq->line->cpu == cpu || line_move(irq->line, cpu)
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
}

void irq_suspend(void) { ioapic_suspend(); }

void irq_resume(void) {
    pic_disable();
    ioapic_resume();
}
