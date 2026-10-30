/* The CPUs: their per-CPU memory (arch/percpu.h), starting the others, the set of those running,
   the IPIs that reach them, the parallel batches idle CPUs take shares of, and halting them for a
   sleep. Each CPU runs the scheduler itself
   (sys/sched.c). */
#include "kernel.h"
#include "arch/cpu.h"
#include "arch/fpu.h"
#include "arch/smp.h"
#include "arch/cpuset.h"
#include "arch/spin.h"
#include "mm/bootmem.h"
#include "limine.h"
#include "mm/vmm.h"
#include "mm/heap.h"
#include "abi.h"
#include "sys/alarm.h"
#include "sys/clock.h"
#include "sys/sched.h"
#include "arch/pstate.h"
#include "sys/prof.h"
#include "arch/pat.h"
#include "arch/wake.h"

#define AP_STACK_SIZE (32 * 1024)
#define START_TIMEOUT_MS 2000
#define PARK_TIMEOUT_MS 2000

/* Each CPU's part in starting and stopping; its APIC ID is topology's. */
struct cpu_slot {
    uint32_t start;  /* START_: where it is in being started */
    uint32_t parked;
    bool asleep; /* parked for a sleep: smp_resume starts it again */
    uint8_t stack[AP_STACK_SIZE] __attribute__((aligned(16))); /* its idle task's stack */
};
PERCPU static struct cpu_slot slot;

PERCPU uintptr_t percpu_offset;
PERCPU unsigned cpu_index;
static uintptr_t boot_offset; /* until percpu_init the first CPU alone, on the template */
uintptr_t *percpu_offsets = &boot_offset;

/* The CPUs running the scheduler (arch/cpuset.h): the first one alone until percpu_init. */
static uint64_t boot_online = 1, *online_set = &boot_online;
static unsigned online = 1;
static unsigned detected = 1;
static uint64_t bsp_pat;
static struct wake_cpu *restart; /* smp_suspend to smp_resume: the rows of the CPUs to start again */

extern NORETURN void smp_switch_stack(uint64_t top, void (*entry)(void *), void *argument);
extern uint8_t __percpu_start[], __percpu_end[];

void percpu_load(unsigned cpu) {
    wrmsr(MSR_GS_BASE, percpu_offsets[cpu]);
    wrmsr(MSR_KERNEL_GS_BASE, 0);
    wrmsr(MSR_TSC_AUX, cpu); /* RDPID: this CPU's index, for the entries that cannot trust GS */
}

void percpu_init(void) {
    size_t bytes = (size_t)(__percpu_end - __percpu_start);
    uintptr_t *offsets = bootmem_alloc(topology.cpus * sizeof *offsets, _Alignof(uintptr_t));
    for (unsigned i = 0; i < topology.cpus; i++) {
        uint8_t *copy = bootmem_alloc_node(ALIGN_UP(bytes, PAGE_SIZE), PAGE_SIZE, topology.cpu[i].node);
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
    kprintf("cpu: %u per-CPU copies of %lu KiB, each on its CPU's node\n", topology.cpus,
            (unsigned long)(ALIGN_UP(bytes, PAGE_SIZE) >> 10));
}

unsigned cpu_online_count(void) { return __atomic_load_n(&online, __ATOMIC_ACQUIRE); }

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
    uint32_t others = 0;
    cpuset_each(i, online_set, topology.cpus)
        if (i + 1 != self && lapic_send_nmi(topology.cpu[i].apic_id)) others++;
    /* a while for them to stop; one that does not cannot be helped */
    uint64_t until = clock_after(100 * NS_PER_MS);
    while (__atomic_load_n(&panic_stopped, __ATOMIC_ACQUIRE) < others && clock_now_ns() < until) pause();
    return true;
}

void smp_panic_nmi(void) {
    uint32_t owner = __atomic_load_n(&panic_owner, __ATOMIC_ACQUIRE);
    if (!owner || owner == cpu_current_index() + 1) return;
    __atomic_add_fetch(&panic_stopped, 1, __ATOMIC_RELEASE);
    for (;;) {
        cli();
        hlt();
    }
}
bool cpu_online_at(unsigned i) { return i < topology.cpus && cpuset_has(online_set, i); }

void cpu_kick(unsigned i) {
    if (i == cpu_current_index() || !cpu_online_at(i)) return;
    /* An IPI that cannot be sent is a broken APIC: nothing on that CPU would be scheduled again. */
    if (!lapic_send_ipi(topology.cpu[i].apic_id, VEC_RESCHED)) panic("smp: IPI to cpu%u failed", i);
}

void cpu_alarm(unsigned i) {
    if (i == cpu_current_index() || !cpu_online_at(i)) return;
    if (!lapic_send_ipi(topology.cpu[i].apic_id, VEC_TIMER)) panic("smp: alarm IPI to cpu%u failed", i);
}

bool cpu_tlb_ipi(unsigned i) {
    if (!cpu_online_at(i)) return false;
    if (!lapic_send_ipi(topology.cpu[i].apic_id, VEC_TLB_SHOOTDOWN)) panic("smp: TLB shootdown IPI to cpu%u failed", i);
    return true;
}

/* ---- parallel batches ----
   The caller publishes the batch and takes items itself; idle CPUs, told by an IPI, take items
   too. An item is claimed with the batch's number in the same word, so a CPU that comes late,
   after the batch ended, finds another number there and takes nothing; while any claimed item
   runs, its batch has not ended, so what the claimer then reads of it is that batch's. */
static struct {
    uint64_t claim;     /* batch number << 32 | next item */
    size_t count;
    cpu_job_fn fn;
    void *context;
    uint64_t done;      /* items finished */
} batch;
static uint32_t batch_taken, batch_number;
static uint64_t jobs, max_job_ns;

static size_t take_items(uint32_t number) {
    size_t ran = 0;
    for (;;) {
        uint64_t claim = __atomic_load_n(&batch.claim, __ATOMIC_ACQUIRE);
        if ((uint32_t)(claim >> 32) != number) return ran;
        size_t item = (uint32_t)claim;
        if (item >= batch.count) return ran;
        if (!__atomic_compare_exchange_n(&batch.claim, &claim, claim + 1, true, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE))
            continue;
        batch.fn(item, batch.context);
        __asm__ volatile("sfence" : : : "memory");
        __atomic_fetch_add(&batch.done, 1, __ATOMIC_RELEASE);
        ran++;
    }
}

/* The idle task's own XSTATE is nothing anyone reads: the callbacks start from a clean one. */
bool cpu_jobs_poll(void) {
    uint64_t claim = __atomic_load_n(&batch.claim, __ATOMIC_ACQUIRE);
    if (!__atomic_load_n(&batch_taken, __ATOMIC_ACQUIRE) || (uint32_t)claim >= batch.count) return false;
    fpu_reset();
    return take_items((uint32_t)(claim >> 32)) != 0;
}

void cpu_parallel_for(size_t count, cpu_job_fn fn, void *context) {
    if (!count || !fn) return;
    ASSERT((uint64_t)fn >= 0xFFFF800000000000ULL && (!context || (uint64_t)context >= 0xFFFF800000000000ULL));
    uint64_t flags = irq_save(), since = clock_now_ns();
    fpu_kernel_begin();
    uint32_t free = 0;
    if (online == 1 || count == 1 || count >= UINT32_MAX ||
        !__atomic_compare_exchange_n(&batch_taken, &free, 1, false, __ATOMIC_ACQUIRE, __ATOMIC_RELAXED)) {
        /* alone: one CPU, one item, or a batch already under way (another caller's, or ours) */
        for (size_t i = 0; i < count; i++) fn(i, context);
        __asm__ volatile("sfence" : : : "memory");
    } else {
        uint32_t number = ++batch_number;
        batch.count = count;
        batch.fn = fn;
        batch.context = context;
        __atomic_store_n(&batch.done, 0, __ATOMIC_RELAXED);
        __atomic_store_n(&batch.claim, (uint64_t)number << 32, __ATOMIC_RELEASE);
        unsigned self = cpu_current_index();
        for (unsigned i = 0; i < topology.cpus; i++)
            if (i != self && sched_cpu_idle(i)) cpu_kick(i);
        take_items(number);
        /* Items others claimed are running: they never wait for anything, so this is short. */
        while (__atomic_load_n(&batch.done, __ATOMIC_ACQUIRE) != count) {
            vmm_tlb_poll();
            pause();
        }
        /* closed: no item index reaches UINT32_MAX, and the number stays this batch's */
        __atomic_store_n(&batch.claim, (uint64_t)number << 32 | UINT32_MAX, __ATOMIC_RELEASE);
        __atomic_store_n(&batch_taken, 0, __ATOMIC_RELEASE);
        __atomic_fetch_add(&jobs, 1, __ATOMIC_RELAXED);
    }
    fpu_kernel_end();
    uint64_t took = clock_now_ns() - since, most = __atomic_load_n(&max_job_ns, __ATOMIC_RELAXED);
    while (took > most && !__atomic_compare_exchange_n(&max_job_ns, &most, took, true, __ATOMIC_RELAXED, __ATOMIC_RELAXED)) {}
    irq_restore(flags);
}

uint64_t cpu_parallel_jobs(void) { return __atomic_load_n(&jobs, __ATOMIC_RELAXED); }
uint64_t cpu_max_job_us(void) { return __atomic_load_n(&max_job_ns, __ATOMIC_RELAXED) / NS_PER_US; }

void smp_get_info(struct n_cpuinfo *info) {
    memset(info, 0, sizeof *info);
    info->version = 1;
    info->detected_cpus = detected;
    info->online_cpus = cpu_online_count();
    info->scheduler_cpus = info->online_cpus;
    info->worker_cpus = info->online_cpus - 1; /* the others take shares of a kernel batch */
    info->flags = N_CPU_SSE2 | N_CPU_USER_THREADS | (info->worker_cpus ? N_CPU_AP_WORKERS : 0) | N_CPU_AVX;
    info->parallel_jobs = cpu_parallel_jobs();
}

/* ---- starting the CPUs ----
   CPU 0 starts every other CPU at once (at boot through the bootloader, each one spinning on its
   entry's goto_address; after a sleep with INIT and STARTUP through the wake page) and waits for
   all of them until one deadline. A CPU arrives (ap_main) once it has checked and set itself up
   to its LAPIC, or fails there; one that has not answered by the deadline is excluded, and halts
   if it comes after all: of it and CPU 0, whichever changes its state from waiting first decides.
   Those that arrived compare their TSCs with each other and CPU 0's all at once (sys/clock.h),
   then each one joins the online set itself and runs the scheduler. */
enum { START_NONE, START_WAITING, START_ARRIVED, START_FAILED, START_EXCLUDED };
static unsigned answered; /* CPUs of this round that arrived or failed */

static NORETURN void halt(void) {
    for (;;) {
        cli();
        hlt();
    }
}

/* Its answer, unless CPU 0 excluded it first: then it halts. */
static void answer(struct cpu_slot *c, uint32_t state) {
    uint32_t waiting = START_WAITING;
    if (!__atomic_compare_exchange_n(&c->start, &waiting, state, false, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) halt();
    __atomic_fetch_add(&answered, 1, __ATOMIC_RELEASE);
}

static NORETURN void fail(struct cpu_slot *c) {
    answer(c, START_FAILED);
    halt();
}

static NORETURN void ap_main(void *argument) {
    unsigned index = (unsigned)(uintptr_t)argument;
    struct cpu_slot *c = &per_cpu(slot, index);
    cli();
    percpu_load(index); /* before anything that takes a lock: a spinlock names its holder by it */
    if (!cpu_features_same()) fail(c);
    cpu_setup();
    gdt_init_cpu(index);
    entry_load();
    if (!entry_stacks_init()) fail(c); /* no NMI comes before it: the panic's go to online CPUs */
    if (!fpu_init_ap()) fail(c);
    pstate_init_ap();
    mtrr_sync(); /* after a sleep the firmware's memory types may be gone: the BSP's again */
    pat_init_cpu(); /* same PAT as the BSP before any MMIO/WC mapping is touched */
    if (rdmsr(MSR_PAT) != bsp_pat) fail(c);
    write_cr3(kernel_pml4);
    vmm_init_cpu();
    if (!lapic_ap_init() || lapic_current_id() != topology.cpu[index].apic_id) fail(c);
    answer(c, START_ARRIVED);
    clock_cpu_compare(); /* with every CPU that arrived, once CPU 0 has their count */
    alarm_init_cpu();
    prof_cpu_start();
    /* IPIs reach it from here on, taken once its idle task has interrupts on */
    cpuset_add(online_set, index);
    __atomic_fetch_add(&online, 1, __ATOMIC_ACQ_REL);
    sched_cpu_start(); /* this CPU's idle task from here on */
}

static NORETURN void ap_entry(struct limine_mp_info *info) {
    unsigned index = (unsigned)info->extra_argument;
    struct cpu_slot *c = &per_cpu(slot, index);
    smp_switch_stack((uint64_t)c->stack + sizeof c->stack, ap_main, (void *)(uintptr_t)index);
}

/* CPU 0, after starting count CPUs (their slots waiting) with before CPUs online: waits for
   their answers until the deadline and excludes the rest, has those that arrived compare their
   TSCs with it, and waits for them to come online. after: when, for the lines it writes. */
static void start_wait(unsigned count, unsigned before, const char *after) {
    uint64_t until = clock_after(START_TIMEOUT_MS * NS_PER_MS);
    while (__atomic_load_n(&answered, __ATOMIC_ACQUIRE) < count && clock_now_ns() < until) pause();
    unsigned arrived = 0;
    for (unsigned i = 1; i < topology.cpus; i++) {
        struct cpu_slot *c = &per_cpu(slot, i);
        uint32_t state = START_WAITING;
        if (__atomic_compare_exchange_n(&c->start, &state, START_EXCLUDED, false, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE))
            kprintf("smp: cpu%u did not start%s; excluded\n", i, after);
        else if (state == START_FAILED)
            kprintf("smp: cpu%u failed its checks%s; excluded\n", i, after);
        else if (state == START_ARRIVED)
            arrived++;
        /* not waiting: one that comes late still finds it so */
        __atomic_store_n(&c->start, START_NONE, __ATOMIC_RELAXED);
    }
    __atomic_store_n(&answered, 0, __ATOMIC_RELAXED);
    clock_cpus_compare(arrived + 1);
    until = clock_after(START_TIMEOUT_MS * NS_PER_MS);
    while (cpu_online_count() < before + arrived) {
        if (clock_now_ns() >= until) panic("smp: CPUs that arrived did not come online%s", after);
        pause();
    }
}

/* ---- sleep ----
   Before S3 every other CPU gives its tasks to CPU 0 (sched_cpu_leave) and halts in its idle loop
   with interrupts off; after it (or after a sleep that failed) all are started again at once with
   INIT-SIPI through the wake page and come back through ap_main as at boot. */
static volatile bool parking;

void cpu_park_check(void) {
    if (!__atomic_load_n(&parking, __ATOMIC_ACQUIRE)) return;
    unsigned self = cpu_current_index();
    if (!self || !sched_cpu_empty(self)) return;
    struct cpu_slot *c = this_cpu_ptr(slot);
    cli();
    alarm_cpu_leave(); /* its alarms to CPU 0, its wake device stopped */
    /* Out of the online set before its last look at the shootdowns: a CPU that still counted it
       published its request before (mm/vmm.c), and is answered here. */
    cpuset_remove(online_set, self);
    __atomic_fetch_sub(&online, 1, __ATOMIC_SEQ_CST);
    vmm_tlb_poll();
    __atomic_store_n(&c->parked, 1, __ATOMIC_RELEASE);
    for (;;) hlt(); /* only INIT gets it out */
}

bool smp_suspend(void) {
    ASSERT(cpu_current_index() == 0);
    if (cpu_online_count() == 1) return true;
    if (!wake_available()) return false;
    if (!restart && !(restart = kmalloc(topology.cpus * sizeof *restart))) return false;
    for (unsigned i = 1; i < topology.cpus; i++) {
        struct cpu_slot *c = &per_cpu(slot, i);
        c->asleep = cpu_online_at(i);
        if (c->asleep) __atomic_store_n(&c->parked, 0, __ATOMIC_RELEASE);
    }
    __atomic_store_n(&parking, true, __ATOMIC_RELEASE);
    bool ok = true;
    for (unsigned i = 1; i < topology.cpus; i++) {
        struct cpu_slot *c = &per_cpu(slot, i);
        if (!c->asleep) continue;
        sched_cpu_leave(i); /* its tasks to CPU 0's queue; it sees parking once it has none */
        uint64_t until = clock_after(PARK_TIMEOUT_MS * NS_PER_MS);
        while (!__atomic_load_n(&c->parked, __ATOMIC_ACQUIRE) && clock_now_ns() < until) {
            vmm_tlb_poll();
            pause();
        }
        if (!__atomic_load_n(&c->parked, __ATOMIC_ACQUIRE)) {
            kprintf("smp: cpu%u did not stop for sleep\n", i);
            ok = false;
        }
    }
    return ok;
}

void smp_hold_in_init(void) {
    for (unsigned i = 1; i < topology.cpus; i++)
        if (per_cpu(slot, i).asleep) lapic_send_init(topology.cpu[i].apic_id);
    clock_spin(LAPIC_INIT_WAIT_NS); /* each has taken its INIT before the copy begins */
}

void smp_resume(void) {
    ASSERT(cpu_current_index() == 0);
    __atomic_store_n(&parking, false, __ATOMIC_RELEASE);
    unsigned before = cpu_online_count(), rows = 0;
    /* INIT first, to every one: one still on its way through the page from an earlier STARTUP
       (wake.h) is reset, not let in on the new table */
    for (unsigned i = 1; i < topology.cpus; i++) {
        struct cpu_slot *c = &per_cpu(slot, i);
        if (!c->asleep) continue;
        c->asleep = false;
        if (cpu_online_at(i)) continue; /* it never stopped */
        uint32_t apic_id = topology.cpu[i].apic_id;
        if (!lapic_send_init(apic_id)) {
            kprintf("smp: cpu%u could not be sent INIT after sleep; excluded\n", i);
            continue;
        }
        /* ap_main as if called: the return address slot below an aligned top */
        restart[rows++] = (struct wake_cpu){.stack = (uint64_t)c->stack + sizeof c->stack - 8, .arg = i, .apic_id = apic_id};
        __atomic_store_n(&c->start, START_WAITING, __ATOMIC_RELAXED);
    }
    if (rows) {
        clock_spin(LAPIC_INIT_WAIT_NS);
        wake_prepare_cpus((uint64_t)ap_main, restart, rows);
        for (unsigned round = 0; round < 2; round++) {
            for (unsigned r = 0; r < rows; r++) lapic_send_startup(restart[r].apic_id, wake_page());
            clock_spin(LAPIC_STARTUP_WAIT_NS);
        }
        start_wait(rows, before, " after sleep");
        kprintf("smp: %u/%u CPUs online after sleep\n", cpu_online_count(), topology.cpus);
    }
    kfree(restart);
    restart = NULL;
}

/* ---- the boot check of batches ---- */
struct probe {
    uint64_t value[257];
    uint64_t sse[257][2];
    uint8_t avx_ok[257];
    unsigned cpu[257];
};

static uint64_t probe_value(size_t i) {
    uint64_t v = i + 1;
    for (unsigned j = 0; j < 2048; j++) v = v * 6364136223846793005ULL + 1442695040888963407ULL;
    return v;
}

static void probe_job(size_t i, void *arg) {
    struct probe *p = arg;
    p->value[i] = probe_value(i);
    uint64_t words[2] = {i + 1, i + 17};
    /* compiler は general-regs-only。明示 SSE2 callback の実行も検査する。 */
    __asm__ volatile("movdqu %1, %%xmm0; paddq %%xmm0, %%xmm0; movdqu %%xmm0, %0"
                     : "=m"(p->sse[i]) : "m"(words) : "memory");
    {
        uint64_t in[4] = {i + 1, i + 17, i + 33, i + 49}, out[4];
        /* AVX1だけを使用（256bit整数算術/AVX2は要求しない）。復帰検査用に全YMM0を破壊。 */
        __asm__ volatile("vmovdqu %1, %%ymm0; vmovdqu %%ymm0, %0; vxorps %%ymm0, %%ymm0, %%ymm0"
                         : "=m"(out) : "m"(in) : "memory");
        p->avx_ok[i] = !memcmp(in, out, sizeof in);
    }
    p->cpu[i] = cpu_current_index();
}

static bool probe_ok(const struct probe *p, const uint64_t *reference, size_t count) {
    for (size_t i = 0; i < count; i++)
        if (p->value[i] != reference[i] || p->sse[i][0] != (i + 1) * 2 || p->sse[i][1] != (i + 17) * 2 ||
            !p->avx_ok[i]) return false;
    for (size_t i = count; i < ARRAY_SIZE(p->value); i++)
        if (p->value[i] || p->sse[i][0] || p->sse[i][1] || p->avx_ok[i]) return false;
    return true;
}

static void selftest(void) {
    static struct probe p;
    static uint64_t reference[257];
    for (size_t i = 0; i < ARRAY_SIZE(reference); i++) reference[i] = probe_value(i);
    void *original_storage = kmalloc(fpu_storage_bytes()), *checked_storage = kmalloc(fpu_storage_bytes());
    if (!original_storage || !checked_storage) panic("smp: selftest without memory");
    struct fpu_state *original = fpu_state_in(original_storage), *checked = fpu_state_in(checked_storage);
    uint64_t sentinel[4] = {0x123456789ABCDEF0ULL, 0xFEDCBA9876543210ULL,
                          0x0F0E0D0C0B0A0908ULL, 0x8877665544332211ULL};
    fpu_save(original);
    __asm__ volatile("vmovdqu %0, %%ymm0" : : "m"(sentinel) : "memory");
    memset(&p, 0, sizeof p);
    uint64_t since = clock_now_ns();
    cpu_parallel_for(ARRAY_SIZE(p.value), probe_job, &p);
    uint64_t took = clock_now_ns() - since;
    fpu_save(checked);
    const uint8_t *upper = fpu_state_component(checked, 2);
    if (memcmp(fpu_state_component(checked, 1) + 160, sentinel, 16)) panic("smp: caller SSE state not restored");
    if (!upper || memcmp(upper, sentinel + 2, 16)) panic("smp: caller YMM state not restored");
    fpu_restore(original);
    kfree(original_storage);
    kfree(checked_storage);
    if (!probe_ok(&p, reference, ARRAY_SIZE(p.value))) panic("smp: selftest mismatch");
    uint64_t *ran = kzalloc(cpuset_words(topology.cpus) * sizeof *ran);
    if (!ran) panic("smp: selftest without memory");
    unsigned cpus = 0;
    for (size_t i = 0; i < ARRAY_SIZE(p.value); i++)
        if (!cpuset_has(ran, p.cpu[i])) {
            cpuset_add(ran, p.cpu[i]);
            cpus++;
        }
    kfree(ran);
    kprintf("smp: batch selftest PASS items=257 cpus=%u %lu us\n", cpus, took / NS_PER_US);
    if (cmdline_has("smp-test")) {
        static const size_t counts[] = {0, 1, 3, 7, 16, 256, 257};
        for (unsigned round = 0; round < 32; round++)
            for (size_t c = 0; c < ARRAY_SIZE(counts); c++) {
                memset(&p, 0, sizeof p);
                cpu_parallel_for(counts[c], probe_job, &p);
                if (!probe_ok(&p, reference, counts[c]))
                    panic("smp: stress mismatch round=%u count=%lu", round, counts[c]);
            }
        kprintf("smp: stress PASS rounds=32 boundary-counts=7 jobs=%lu max_job_us=%lu\n", cpu_parallel_jobs(),
                cpu_max_job_us());
    }
}

void smp_init(struct limine_mp_response *response) {
    if (response && response->cpu_count) detected = (unsigned)MIN(response->cpu_count, 0xFFFFFFFFULL);
    /* The APs may be in another APIC mode than the BSP (apic_init left x2APIC mode for the IOMMU,
       iommu_x2apic_conflict): lapic_ap_init takes each one started to the BSP's. One not
       started (nosmp, or one excluded) stays parked by the bootloader in its mode, and is sent
       no interrupt. The BSP's own ID has to be one an IPI can name too (lapic_addressable). */
    if (!response || cmdline_has("nosmp") || !lapic_present() || response->bsp_lapic_id != lapic_current_id() ||
        !lapic_addressable(response->bsp_lapic_id)) {
        kprintf("smp: single CPU (%s)\n",
                cmdline_has("nosmp") ? "nosmp" : "MP/APIC unavailable, or the BSP's APIC ID mismatched or not addressable");
        selftest();
        return;
    }
    bsp_pat = rdmsr(MSR_PAT);
    /* the bootloader's entry of each CPU, by its APIC ID */
    struct limine_mp_info **entry = kzalloc(topology.cpus * sizeof *entry);
    if (!entry) panic("smp: no memory to start the CPUs");
    for (uint64_t j = 0; j < response->cpu_count; j++) {
        struct limine_mp_info *info = response->cpus[j];
        unsigned i = info ? topology_cpu_of(info->lapic_id) : topology.cpus;
        if (i < topology.cpus) entry[i] = info;
    }
    /* each one starts as soon as its scheduler is ready, while the next one's is made */
    unsigned count = 0;
    for (unsigned i = 1; i < topology.cpus; i++) {
        uint32_t apic_id = topology.cpu[i].apic_id;
        if (!entry[i] || !lapic_addressable(apic_id)) {
            kprintf("smp: cpu%u (APIC ID %u) %s; excluded\n", i, apic_id,
                    entry[i] ? "cannot be sent an interrupt" : "is not in the bootloader's list");
            continue;
        }
        if (!sched_cpu_prepare(i)) {
            kprintf("smp: cpu%u has no memory for its scheduler; excluded\n", i);
            continue;
        }
        __atomic_store_n(&per_cpu(slot, i).start, START_WAITING, __ATOMIC_RELAXED);
        entry[i]->extra_argument = i;
        __atomic_store_n(&entry[i]->goto_address, ap_entry, __ATOMIC_RELEASE);
        count++;
    }
    kfree(entry);
    start_wait(count, 1, "");
    kprintf("smp: %u/%lu CPUs online, each running the scheduler\n", cpu_online_count(), response->cpu_count);
    selftest();
}
