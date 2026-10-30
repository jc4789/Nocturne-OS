/* The CPUs: starting the others, their enumeration, the IPIs that reach them, the parallel batches
   idle CPUs take shares of, and halting them for a sleep. Each CPU runs the scheduler itself
   (sys/sched.c). */
#include "kernel.h"
#include "arch/cpu.h"
#include "arch/fpu.h"
#include "arch/smp.h"
#include "arch/spin.h"
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

struct cpu_slot {
    uint32_t apic_id;
    unsigned index;
    unsigned ready, failed, parked;
    bool asleep; /* parked for a sleep: smp_resume starts it again */
    uint8_t stack[AP_STACK_SIZE] __attribute__((aligned(16))); /* its idle task's stack */
} __attribute__((aligned(64)));

static struct cpu_slot cpus[CPU_MAX_COUNT];
static struct cpu_local cpu_locals[CPU_MAX_COUNT];
static unsigned slots = 1, online = 1;
static unsigned detected = 1;
static uint32_t online_mask = 1;
static uint64_t bsp_pat;

extern NORETURN void smp_switch_stack(uint64_t top, void (*entry)(void *), void *argument);

void cpu_local_init(unsigned index) {
    ASSERT(index < ARRAY_SIZE(cpu_locals));
    struct cpu_local *local = &cpu_locals[index];
    local->self = local;
    local->index = index;
    /* Users can load a GS selector (all user descriptors have base zero), but
       must not use WRGSBASE to impersonate the high-half kernel GS base. */
    write_cr4(read_cr4() & ~CR4_FSGSBASE);
    wrmsr(MSR_GS_BASE, (uint64_t)local);
    wrmsr(MSR_KERNEL_GS_BASE, 0);
}

unsigned cpu_current_index(void) {
    if (slots == 1) return 0;
    unsigned index;
    __asm__ volatile("movl %%gs:%c1, %0" : "=r"(index)
                     : "i"(__builtin_offsetof(struct cpu_local, index)));
    return index;
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
    for (unsigned i = 0; i < CPU_MAX_COUNT; i++)
        if (i + 1 != self && cpu_online_at(i) && lapic_send_nmi(cpus[i].apic_id)) others++;
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
bool cpu_online_at(unsigned i) {
    return i < CPU_MAX_COUNT && (__atomic_load_n(&online_mask, __ATOMIC_ACQUIRE) & (1u << i));
}

void cpu_kick(unsigned i) {
    if (i == cpu_current_index() || !cpu_online_at(i)) return;
    /* An IPI that cannot be sent is a broken APIC: nothing on that CPU would be scheduled again. */
    if (!lapic_send_ipi(cpus[i].apic_id, VEC_RESCHED)) panic("smp: IPI to cpu%u failed", i);
}

void cpu_alarm(unsigned i) {
    if (i == cpu_current_index() || !cpu_online_at(i)) return;
    if (!lapic_send_ipi(cpus[i].apic_id, VEC_TIMER)) panic("smp: alarm IPI to cpu%u failed", i);
}

bool cpu_tlb_ipi(unsigned i) {
    if (!cpu_online_at(i)) return false;
    if (!lapic_send_ipi(cpus[i].apic_id, VEC_TLB_SHOOTDOWN)) panic("smp: TLB shootdown IPI to cpu%u failed", i);
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
        for (unsigned i = 0; i < slots; i++)
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

/* ---- starting a CPU ---- */

static NORETURN void fail(struct cpu_slot *c) {
    __atomic_store_n(&c->failed, 1, __ATOMIC_RELEASE);
    for (;;) hlt();
}

static NORETURN void ap_main(void *argument) {
    struct cpu_slot *c = argument;
    cli();
    if (!cpu_features_same()) fail(c);
    cpu_setup();
    gdt_init_cpu(c->index);
    idt_load();
    if (!fpu_init_ap()) fail(c);
    pstate_init_ap();
    mtrr_sync(); /* after a sleep the firmware's memory types may be gone: the BSP's again */
    pat_init_cpu(); /* same PAT as the BSP before any MMIO/WC mapping is touched */
    if (rdmsr(MSR_PAT) != bsp_pat) fail(c);
    write_cr3(kernel_pml4);
    vmm_init_cpu();
    if (!lapic_ap_init() || lapic_current_id() != c->apic_id) fail(c);
    clock_cpu_join(); /* CPU 0 waits for it in started() */
    alarm_init_cpu();
    prof_cpu_start();
    __atomic_store_n(&c->ready, 1, __ATOMIC_RELEASE);
    sched_cpu_start(); /* this CPU's idle task from here on */
}

static NORETURN void ap_entry(struct limine_mp_info *info) {
    struct cpu_slot *c = (void *)info->extra_argument;
    smp_switch_stack((uint64_t)c->stack + sizeof c->stack, ap_main, c);
}

/* CPU 0: c is running its idle task now, or failed or timed out. */
static bool started(struct cpu_slot *c) {
    uint64_t until = clock_after(START_TIMEOUT_MS * NS_PER_MS);
    /* its TSC against this one's, while it waits in clock_cpu_join (after its LAPIC is up) */
    if (clock_cpu_lead(c->index, until))
        while (!__atomic_load_n(&c->ready, __ATOMIC_ACQUIRE) && !__atomic_load_n(&c->failed, __ATOMIC_ACQUIRE) &&
               clock_now_ns() < until) pause();
    if (!__atomic_load_n(&c->ready, __ATOMIC_ACQUIRE)) return false;
    __atomic_fetch_or(&online_mask, 1u << c->index, __ATOMIC_ACQ_REL);
    __atomic_fetch_add(&online, 1, __ATOMIC_ACQ_REL);
    return true;
}

/* ---- sleep ----
   Before S3 every other CPU gives its tasks to CPU 0 (sched_cpu_leave) and halts in its idle loop
   with interrupts off; after it (or after a sleep that failed) each one is started again with
   INIT-SIPI through the wake page and comes back through ap_main as at boot. */
static volatile bool parking;

void cpu_park_check(void) {
    if (!__atomic_load_n(&parking, __ATOMIC_ACQUIRE)) return;
    unsigned self = cpu_current_index();
    if (!self || !sched_cpu_empty(self)) return;
    struct cpu_slot *c = &cpus[self];
    cli();
    alarm_cpu_leave(); /* its alarms to CPU 0, its wake device stopped */
    /* Out of the online set before its last look at the shootdowns: a CPU that still counted it
       published its request before (mm/vmm.c), and is answered here. */
    __atomic_fetch_and(&online_mask, ~(1u << self), __ATOMIC_SEQ_CST);
    __atomic_fetch_sub(&online, 1, __ATOMIC_SEQ_CST);
    vmm_tlb_poll();
    __atomic_store_n(&c->parked, 1, __ATOMIC_RELEASE);
    for (;;) hlt(); /* only INIT gets it out */
}

bool smp_suspend(void) {
    ASSERT(cpu_current_index() == 0);
    if (slots == 1) return true;
    if (!wake_available()) return false;
    for (unsigned i = 1; i < slots; i++) {
        struct cpu_slot *c = &cpus[i];
        c->asleep = cpu_online_at(i);
        if (c->asleep) __atomic_store_n(&c->parked, 0, __ATOMIC_RELEASE);
    }
    __atomic_store_n(&parking, true, __ATOMIC_RELEASE);
    bool ok = true;
    for (unsigned i = 1; i < slots; i++) {
        struct cpu_slot *c = &cpus[i];
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
    for (unsigned i = 1; i < slots; i++)
        if (cpus[i].asleep) lapic_send_init(cpus[i].apic_id);
}

void smp_resume(void) {
    ASSERT(cpu_current_index() == 0);
    if (slots == 1) return;
    __atomic_store_n(&parking, false, __ATOMIC_RELEASE);
    for (unsigned i = 1; i < slots; i++) {
        struct cpu_slot *c = &cpus[i];
        if (!c->asleep) continue;
        c->asleep = false;
        if (cpu_online_at(i)) continue; /* it never stopped */
        __atomic_store_n(&c->ready, 0, __ATOMIC_RELEASE);
        __atomic_store_n(&c->failed, 0, __ATOMIC_RELEASE);
        /* The INIT before the wake page names this CPU: one still on its way through the page from
           an earlier STARTUP (wake.h) is reset, not let in as this one. ap_main as if called: the
           return address slot below an aligned top. */
        bool sent = lapic_send_init(c->apic_id);
        if (sent) {
            wake_prepare((uint64_t)ap_main, (uint64_t)c->stack + sizeof c->stack - 8, (uint64_t)c, c->apic_id);
            sent = lapic_send_startup(c->apic_id, wake_page());
        }
        if (!sent || !started(c)) kprintf("smp: cpu%u did not start again after sleep; excluded\n", i);
    }
    kprintf("smp: %u/%u CPUs online after sleep\n", cpu_online_count(), slots);
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
    uint64_t took = clock_now_ns() - since, mask = 0;
    fpu_save(checked);
    const uint8_t *upper = fpu_state_component(checked, 2);
    if (memcmp(fpu_state_component(checked, 1) + 160, sentinel, 16)) panic("smp: caller SSE state not restored");
    if (!upper || memcmp(upper, sentinel + 2, 16)) panic("smp: caller YMM state not restored");
    fpu_restore(original);
    kfree(original_storage);
    kfree(checked_storage);
    if (!probe_ok(&p, reference, ARRAY_SIZE(p.value))) panic("smp: selftest mismatch");
    for (size_t i = 0; i < ARRAY_SIZE(p.value); i++) mask |= 1ULL << p.cpu[i];
    kprintf("smp: batch selftest PASS items=257 cpus=%u mask=%lx %lu us\n", __builtin_popcountll(mask), mask,
            took / NS_PER_US);
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
       started (nosmp, beyond CPU_MAX_COUNT) stays parked by the bootloader in its mode, and is
       sent no interrupt. The BSP's own ID has to be one an IPI can name too (lapic_addressable). */
    if (!response || cmdline_has("nosmp") || !lapic_present() || response->bsp_lapic_id != lapic_current_id() ||
        !lapic_addressable(response->bsp_lapic_id)) {
        kprintf("smp: single CPU (%s)\n",
                cmdline_has("nosmp") ? "nosmp" : "MP/APIC unavailable, or the BSP's APIC ID mismatched or not addressable");
        selftest();
        return;
    }
    cpus[0].apic_id = response->bsp_lapic_id;
    cpus[0].ready = 1;
    bsp_pat = rdmsr(MSR_PAT);
    for (uint64_t i = 0; i < response->cpu_count && slots < CPU_MAX_COUNT; i++) {
        struct limine_mp_info *info = response->cpus[i];
        if (!info || info->lapic_id == cpus[0].apic_id || !lapic_addressable(info->lapic_id)) continue;
        bool duplicate = false;
        for (unsigned j = 1; j < slots; j++) if (cpus[j].apic_id == info->lapic_id) duplicate = true;
        if (duplicate) continue;
        struct cpu_slot *slot = &cpus[slots];
        slot->index = slots++;
        slot->apic_id = info->lapic_id;
    }
    for (unsigned i = 1; i < slots; i++) {
        struct cpu_slot *slot = &cpus[i];
        struct limine_mp_info *info = NULL;
        for (uint64_t j = 0; j < response->cpu_count; j++)
            if (response->cpus[j] && response->cpus[j]->lapic_id == slot->apic_id) info = response->cpus[j];
        if (!sched_cpu_prepare(i)) {
            kprintf("smp: cpu%u has no memory for its scheduler; excluded\n", i);
            continue;
        }
        info->extra_argument = (uint64_t)slot;
        __atomic_store_n(&info->goto_address, ap_entry, __ATOMIC_RELEASE);
        if (!started(slot)) kprintf("smp: cpu%u did not start; excluded\n", i);
    }
    kprintf("smp: %u/%lu CPUs online, each running the scheduler\n", cpu_online_count(), response->cpu_count);
    selftest();
}
