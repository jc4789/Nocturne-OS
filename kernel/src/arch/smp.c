/* The CPUs: their per-CPU memory (arch/percpu.h) and in it the spinlocks' and the preemption
   count's state (arch/spin.h, arch/preempt.h), starting the others and stopping them for a sleep,
   the set of those running, and the IPIs that reach them. Each CPU runs the scheduler itself
   (sys/sched.c). */
#include "kernel.h"
#include "abi.h"
#include "arch/apic.h"
#include "arch/cpu.h"
#include "arch/cpuset.h"
#include "arch/gdt.h"
#include "arch/idle.h"
#include "arch/irq.h"
#include "arch/smp.h"
#include "arch/spin.h"
#include "arch/wake.h"
#include "mm/bootmem.h"
#include "mm/heap.h"
#include "mm/vmm.h"
#include "sys/alarm.h"
#include "sys/clock.h"
#include "sys/prof.h"
#include "sys/sched.h"
#include "sys/wait.h"

/* A panic waits this long for the other CPUs to stop; one that does not cannot be helped. Carried
   over unchanged: no manual gives a time for an NMI to be taken. */
#define PANIC_STOP_WAIT_NS (100 * NS_PER_MS)
/* A sleep waits this long for the other CPUs to park; one whose task neither blocks nor yields by
   then is taken to be stuck and the sleep is not entered. Carried over unchanged: it bounds a
   kernel task's run, which no manual does. */
#define PARK_WAIT_NS (2000 * NS_PER_MS)
/* The wait for the CPUs sent INIT and STARTUP to answer: the at least 100 ms the BSP's MP
   initialization waits for the APs (Intel SDM vol. 3A 11.4.4.1, step 14). */
#define START_ANSWER_NS (100 * NS_PER_MS)

PERCPU uintptr_t percpu_offset;
PERCPU unsigned cpu_index;
PERCPU uint32_t preempt_count;
PERCPU struct spin_node spin_nodes[SPIN_NODES];
PERCPU uint32_t spin_waits;
PERCPU uint32_t spins_held;
PERCPU uint64_t spin_ranks_held[SPIN_RANK_WORDS];
static uintptr_t boot_offset; /* until percpu_init the first CPU alone, on the template */
uintptr_t *percpu_offsets = &boot_offset;

/* Each CPU's part in starting and stopping; its APIC ID is topology's. */
struct cpu_slot {
    uint32_t start;  /* START_: where it is in being started */
    uint32_t parked; /* 1 once it halted for a sleep */
    bool asleep;     /* parked for a sleep: smp_resume starts it again */
};
PERCPU static struct cpu_slot slot;

/* The CPUs running the scheduler (arch/cpuset.h): the first one alone until percpu_init. */
static uint64_t boot_online = 1, *online_set = &boot_online;
static unsigned online = 1;

extern uint8_t __percpu_start[], __percpu_end[];

void spin_fault(const struct spinlock *l, const char *why) {
    panic("spinlock %p (rank %u) on cpu%u: %s", (const void *)l, l->rank, cpu_current_index(), why);
}

void percpu_load(unsigned cpu) {
    wrmsr(MSR_GS_BASE, percpu_offsets[cpu]);
    wrmsr(MSR_KERNEL_GS_BASE, 0);
    wrmsr(MSR_TSC_AUX, cpu); /* RDPID: this CPU's index, for the entries that cannot trust GS */
}

void percpu_init(void) {
    size_t bytes = (size_t)(__percpu_end - __percpu_start);
    uintptr_t *offsets = bootmem_alloc(topology.cpus * sizeof *offsets, _Alignof(uintptr_t));
    for (unsigned i = 0; i < topology.cpus; i++) {
        /* on its home: the CPU's node, or the nearest one with memory where its own has none */
        uint8_t *copy = bootmem_alloc_node(ALIGN_UP(bytes, PAGE_SIZE), PAGE_SIZE, topology.cpu[i].home);
        memcpy(copy, __percpu_start, bytes);
        offsets[i] = (uintptr_t)copy - (uintptr_t)__percpu_start;
    }
    uint64_t *set = bootmem_alloc(cpuset_words(topology.cpus) * sizeof *set, _Alignof(uint64_t));
    cpuset_add(set, 0);
    online_set = set;
    percpu_offsets = offsets;
    for (unsigned i = 0; i < topology.cpus; i++) {
        per_cpu(percpu_offset, i) = offsets[i];
        per_cpu(cpu_index, i) = i;
    }
    gdt_init_cpu(0); /* the first CPU's GDT and TSS, and its GS base, in its own copy */
    kprintf("cpu: %u per-CPU copies of %lu KiB, each on its CPU's home node\n", topology.cpus,
            (unsigned long)(ALIGN_UP(bytes, PAGE_SIZE) >> 10));
}

unsigned cpu_online_count(void) { return __atomic_load_n(&online, __ATOMIC_ACQUIRE); }
bool cpu_online_at(unsigned i) { return i < topology.cpus && cpuset_has(online_set, i); }
const uint64_t *cpu_online_set(void) { return online_set; }

/* A panic: the first CPU to panic stops every other one with an NMI (which reaches a CPU with its
   interrupts off, spinning or halted) before it reports, so nothing changes what the report shows
   or writes over it. A CPU that panics meanwhile stops as well. */
static uint32_t panic_owner;   /* index + 1 of the reporting CPU */
static uint32_t panic_depth;   /* its entries: a fault while reporting reports once more, then not */
static uint32_t panic_stopped; /* CPUs the NMI stopped */

bool smp_panic_enter(void) {
    uint32_t self = cpu_current_index() + 1, owner = 0;
    if (!__atomic_compare_exchange_n(&panic_owner, &owner, self, false, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST))
        return owner == self && __atomic_add_fetch(&panic_depth, 1, __ATOMIC_RELAXED) < 3;
    __atomic_store_n(&panic_depth, 1, __ATOMIC_RELAXED);
    clock_stop(); /* a CPU stopped inside a clock write holds no reader from here on */
    uint32_t others = 0;
    cpuset_each(i, online_set, topology.cpus)
        if (i + 1 != self) {
            apic_send_nmi(i);
            others++;
        }
    /* nothing may sleep here: the clock is read in a loop */
    uint64_t until = clock_after(PANIC_STOP_WAIT_NS);
    while (__atomic_load_n(&panic_stopped, __ATOMIC_ACQUIRE) < others && clock_now_ns() < until) pause();
    return true;
}

/* Returns when no other CPU's panic sent the NMI. */
static void panic_nmi(void) {
    uint32_t owner = __atomic_load_n(&panic_owner, __ATOMIC_ACQUIRE);
    if (!owner || owner == cpu_current_index() + 1) return;
    __atomic_add_fetch(&panic_stopped, 1, __ATOMIC_RELEASE);
    for (;;) {
        cli();
        hlt();
    }
}

static uint64_t nmis; /* the NMIs that were no panic's */

void smp_nmi(struct regs *r) {
    panic_nmi();
    uint64_t n = __atomic_add_fetch(&nmis, 1, __ATOMIC_RELAXED);
    if (spin_none_here()) kprintf("cpu%u: NMI at %lx, %lu so far\n", cpu_current_index(), r->rip, n);
}

void cpu_kick(unsigned i) {
    if (i == cpu_current_index() || !cpu_online_at(i)) return;
    if (cpu_idle_wake(i)) return; /* it was waiting on its word, and this write woke it */
    apic_send_ipi(i, VEC_RESCHED);
}

void cpu_alarm(unsigned i) {
    if (i == cpu_current_index() || !cpu_online_at(i)) return;
    apic_send_ipi(i, VEC_TIMER);
}

bool cpu_tlb_ipi(unsigned i) {
    if (!cpu_online_at(i)) return false;
    apic_send_ipi(i, VEC_TLB_SHOOTDOWN);
    return true;
}

/* The ABI's description (common/abi.h): every CPU passed the same checks (cpu_features_same),
   which require SSE2 and AVX; apps' threads run on every online CPU. The kernel runs no work of
   its own spread over CPUs, so no CPU is a worker and no job is counted. */
void smp_get_info(struct n_cpuinfo *info) {
    memset(info, 0, sizeof *info);
    info->version = 1;
    info->detected_cpus = topology.cpus;
    info->online_cpus = cpu_online_count();
    info->scheduler_cpus = info->online_cpus;
    info->flags = N_CPU_SSE2 | N_CPU_AVX | N_CPU_USER_THREADS;
}

/* ---- starting the CPUs ----
   CPU 0 starts every other CPU at once through the wake page (arch/wake.h): an INIT to each, the
   wait after it, then two rounds of STARTUP (Intel SDM vol. 3A 11.4.4.1, Table 11-1; the waits are
   arch/apic.h's). Each CPU answers twice (ap_main):
     present   it checked itself against CPU 0 and set itself up to its LAPIC, so that it can send
               the IPI that wakes CPU 0 (or it was refused there, and tells no one);
     settled   its interrupt stacks are there (arrived), or there was no memory for them (failed).
   One deadline, START_ANSWER_NS from the first INIT, ends the wait for the first answers: a CPU
   that has not answered by then is excluded, and halts if it comes after all (of it and CPU 0,
   whichever moves its state from waiting first decides). One that answered runs the kernel's
   code from then on and settles without a deadline. Those that arrived compare their TSCs with
   each other and with CPU 0 all at once (sys/clock.h), then each one joins the online set itself
   and runs the scheduler. CPU 0 sleeps in between: each answer wakes it. */
enum { START_NONE, START_WAITING, START_PRESENT, START_REFUSED, START_ARRIVED, START_FAILED, START_EXCLUDED };

static struct wait_queue start_q;    /* CPU 0, waiting for answers */
static uint64_t answered;            /* this round's CPUs that answered present or refused */
static uint64_t settled;             /* those that arrived or failed */
static uint64_t joined;              /* those that came online */
static struct wake_cpu *rows;        /* the wake page's table: CPU 0's, from smp_init on */

static NORETURN void halt(void) {
    for (;;) {
        cli();
        hlt();
    }
}

/* Its first answer, unless CPU 0 excluded it first: then it halts. tell: CPU 0 is woken for it
   (this CPU's LAPIC is up). */
static void answer(struct cpu_slot *c, uint32_t state, bool tell) {
    uint32_t waiting = START_WAITING;
    if (!__atomic_compare_exchange_n(&c->start, &waiting, state, false, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) halt();
    __atomic_fetch_add(&answered, 1, __ATOMIC_SEQ_CST);
    if (tell) wq_wake_all(&start_q);
}

static NORETURN void refused(struct cpu_slot *c) {
    answer(c, START_REFUSED, false);
    halt();
}

/* Each CPU sent STARTUP, from the wake page on its idle task's stack, interrupts off, no IDT. */
static NORETURN void ap_main(void *argument) {
    unsigned index = (unsigned)(uintptr_t)argument;
    struct cpu_slot *c = &per_cpu(slot, index);
    percpu_load(index); /* before anything that takes a lock: a spinlock names its holder by it */
    /* a CPU unlike the first may lack what cpu_setup turns on, with no IDT yet to fault into */
    if (!cpu_features_same() || !cpu_setup(index)) refused(c);
    /* off the wake page's tables, which map the kernel half and the low page only, at PCID 0 */
    write_cr3(kernel_pml4);
    vmm_init_cpu();
    if (!apic_init_cpu() || apic_current_id() != topology.cpu[index].apic_id) refused(c);
    answer(c, START_PRESENT, true);
    /* no NMI comes before them: the panic's go to online CPUs */
    bool stacks = entry_stacks_init();
    if (stacks) cpu_mca_interrupt_on();
    __atomic_store_n(&c->start, stacks ? START_ARRIVED : START_FAILED, __ATOMIC_RELEASE);
    __atomic_fetch_add(&settled, 1, __ATOMIC_SEQ_CST);
    wq_wake_all(&start_q);
    if (!stacks) halt();
    clock_cpu_compare(); /* with every CPU that arrived, once CPU 0 has their count */
    alarm_init_cpu();
    prof_cpu_start();
    /* IPIs reach it from here on, taken once its idle task has interrupts on */
    cpuset_add(online_set, index);
    __atomic_fetch_add(&online, 1, __ATOMIC_ACQ_REL);
    vmm_online(); /* a shootdown of the kernel half before it was online did not ask it */
    __atomic_fetch_add(&joined, 1, __ATOMIC_SEQ_CST);
    wq_wake_all(&start_q);
    sched_cpu_start(); /* this CPU's idle task from here on */
}

/* CPU 0: until *word reaches count, or the deadline. */
static void start_wait(uint64_t *word, uint64_t count, uint64_t deadline) {
    for (uint64_t seen; (seen = __atomic_load_n(word, __ATOMIC_SEQ_CST)) < count;)
        if (!wq_wait_unless(&start_q, word, seen, deadline)) return;
}

/* CPU 0: the row that starts CPU i, whose scheduler is ready: ap_main as if called, the return
   address's slot below the stack's aligned top. Its slot waits. */
static struct wake_cpu start_row(unsigned i) {
    __atomic_store_n(&per_cpu(slot, i).start, START_WAITING, __ATOMIC_RELAXED);
    return (struct wake_cpu){.stack = sched_cpu_stack(i) - 8, .arg = i, .apic_id = topology.cpu[i].apic_id};
}

/* CPU 0: starts the count CPUs of rows as above. after: when, for the lines it writes. */
static void start_cpus(unsigned count, const char *after) {
    __atomic_store_n(&answered, 0, __ATOMIC_RELAXED);
    __atomic_store_n(&settled, 0, __ATOMIC_RELAXED);
    __atomic_store_n(&joined, 0, __ATOMIC_RELAXED);
    wake_prepare_cpus((uint64_t)ap_main, rows, count);
    uint64_t until = clock_after(START_ANSWER_NS);
    /* INIT first, to every one: one still on its way through the page from an earlier STARTUP
       (arch/wake.h) is reset, not let in on the new table */
    for (unsigned r = 0; r < count; r++) apic_send_init((unsigned)rows[r].arg);
    sleep_until(clock_after(LAPIC_INIT_WAIT_NS));
    for (unsigned round = 0; round < 2; round++) {
        for (unsigned r = 0; r < count; r++) apic_send_startup((unsigned)rows[r].arg, wake_page());
        sleep_until(clock_after(LAPIC_STARTUP_WAIT_NS));
    }
    start_wait(&answered, count, until);
    unsigned present = 0;
    for (unsigned r = 0; r < count; r++) {
        unsigned i = (unsigned)rows[r].arg;
        uint32_t state = START_WAITING;
        if (__atomic_compare_exchange_n(&per_cpu(slot, i).start, &state, START_EXCLUDED, false, __ATOMIC_ACQ_REL,
                                        __ATOMIC_ACQUIRE))
            kprintf("smp: cpu%u did not start%s; excluded\n", i, after);
        else if (state == START_REFUSED) {
            kprintf("smp: cpu%u failed its checks%s; excluded\n", i, after);
            heap_cpu_gone(i); /* it halted on answering; one that never answered may still run */
        } else if (state != START_NONE)
            present++;
    }
    start_wait(&settled, present, CLOCK_NEVER);
    unsigned arrived = 0;
    for (unsigned r = 0; r < count; r++) {
        unsigned i = (unsigned)rows[r].arg;
        uint32_t state = __atomic_load_n(&per_cpu(slot, i).start, __ATOMIC_ACQUIRE);
        if (state == START_ARRIVED) arrived++;
        else if (state == START_FAILED) {
            kprintf("smp: cpu%u has no memory for its interrupt stacks%s; excluded\n", i, after);
            heap_cpu_gone(i);
        }
    }
    clock_cpus_compare(arrived + 1);
    start_wait(&joined, arrived, CLOCK_NEVER);
    /* not waiting: one excluded that comes late still finds it so */
    for (unsigned r = 0; r < count; r++)
        __atomic_store_n(&per_cpu(slot, rows[r].arg).start, START_NONE, __ATOMIC_RELAXED);
}

void smp_init(void) {
    const char *alone = cmdline_has("nosmp")  ? "nosmp"
                        : topology.cpus == 1  ? "the only one"
                        : !wake_available()   ? "no wake page"
                                              : NULL;
    if (alone) {
        kprintf("smp: single CPU (%s)\n", alone);
        return;
    }
    if (!(rows = kmalloc(topology.cpus * sizeof *rows))) panic("smp: no memory to start the CPUs");
    /* each one's scheduler first, then all of them at once; x2APIC addresses every APIC ID */
    unsigned count = 0;
    for (unsigned i = 1; i < topology.cpus; i++) {
        if (!sched_cpu_prepare(i)) {
            kprintf("smp: cpu%u has no memory for its scheduler; excluded\n", i);
            continue;
        }
        rows[count++] = start_row(i);
    }
    if (count) start_cpus(count, "");
    kprintf("smp: %u/%u CPUs online, each running the scheduler\n", cpu_online_count(), topology.cpus);
}

/* ---- sleep ----
   Before S3 every other CPU gives its tasks to CPU 0 (sched_cpu_leave) and halts in its idle loop
   with interrupts off, all at once, CPU 0 waiting for them until one deadline; after it (or after
   a sleep that failed) all are started again at once through the wake page and come back through
   ap_main as at boot. */
static bool parking;
static struct wait_queue park_q; /* CPU 0, waiting for them */
static uint64_t parked;          /* those halted */

void cpu_park_check(void) {
    if (!__atomic_load_n(&parking, __ATOMIC_ACQUIRE)) return;
    unsigned self = cpu_current_index();
    if (!self || !sched_cpu_empty(self)) return;
    struct cpu_slot *c = this_cpu_ptr(slot);
    cli();
    alarm_cpu_leave(); /* its alarms to CPU 0, its wake device stopped */
    /* Off the space it held (lazily, under its idle task), then out of the online set before its
       last look at its inbox: a CPU that still counted it posted its request before (mm/vmm.c),
       and is answered here. */
    vmm_leave();
    cpuset_remove(online_set, self);
    __atomic_fetch_sub(&online, 1, __ATOMIC_SEQ_CST);
    vmm_tlb_poll();
    __atomic_store_n(&c->parked, 1, __ATOMIC_RELEASE);
    __atomic_fetch_add(&parked, 1, __ATOMIC_SEQ_CST);
    wq_wake_all(&park_q);
    for (;;) hlt(); /* only INIT gets it out */
}

bool smp_suspend(void) {
    ASSERT(cpu_current_index() == 0);
    if (cpu_online_count() == 1) return true;
    if (!wake_available() || !rows) return false;
    unsigned count = 0;
    __atomic_store_n(&parked, 0, __ATOMIC_RELAXED);
    for (unsigned i = 1; i < topology.cpus; i++) {
        struct cpu_slot *c = &per_cpu(slot, i);
        c->asleep = cpu_online_at(i);
        __atomic_store_n(&c->parked, 0, __ATOMIC_RELAXED);
        count += c->asleep;
    }
    __atomic_store_n(&parking, true, __ATOMIC_RELEASE);
    for (unsigned i = 1; i < topology.cpus; i++)
        if (per_cpu(slot, i).asleep) sched_cpu_leave(i); /* its tasks to CPU 0; it parks once it has none */
    uint64_t until = clock_after(PARK_WAIT_NS);
    for (uint64_t seen; (seen = __atomic_load_n(&parked, __ATOMIC_SEQ_CST)) < count;)
        if (!wq_wait_unless(&park_q, &parked, seen, until)) break;
    bool ok = true;
    for (unsigned i = 1; i < topology.cpus; i++) {
        struct cpu_slot *c = &per_cpu(slot, i);
        if (c->asleep && !__atomic_load_n(&c->parked, __ATOMIC_ACQUIRE)) {
            kprintf("smp: cpu%u did not stop for sleep\n", i);
            ok = false;
        }
    }
    return ok;
}

void smp_hold_in_init(void) {
    for (unsigned i = 1; i < topology.cpus; i++)
        if (per_cpu(slot, i).asleep) apic_send_init(i);
    sleep_until(clock_after(LAPIC_INIT_WAIT_NS)); /* each has taken its INIT before the copy begins */
}

void smp_resume(void) {
    ASSERT(cpu_current_index() == 0);
    cpu_mca_interrupt_on(); /* CPU 0's entry, now that its LAPIC is back (dev/pm.c) */
    __atomic_store_n(&parking, false, __ATOMIC_RELEASE);
    unsigned count = 0;
    for (unsigned i = 1; i < topology.cpus; i++) {
        struct cpu_slot *c = &per_cpu(slot, i);
        if (!c->asleep) continue;
        c->asleep = false;
        if (cpu_online_at(i)) continue; /* it never stopped */
        rows[count++] = start_row(i);
    }
    if (!count) return;
    start_cpus(count, " after sleep");
    kprintf("smp: %u/%u CPUs online after sleep\n", cpu_online_count(), topology.cpus);
}
