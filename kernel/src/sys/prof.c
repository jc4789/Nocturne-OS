/* Sampling profiler, enabled by the "prof" kernel command line word. Each
   CPU, on an alarm of its own every millisecond (there is no periodic tick
   otherwise), records where it was: the user instruction of the task it
   ran, the kernel instruction, or idle. A kernel thread
   prints the hottest addresses with their task names every 10 seconds and
   clears the counts; addresses are resolved against the unstripped ELFs on
   the build host. The scheduler's switches, wakes and moves between CPUs
   are printed with each window. A user sample also records the word on top of the
   user stack: for a leaf function such as memcpy that pushed nothing, it is
   the return address, so the host can name the caller. User addresses are
   counted per 64-byte line: an interpreter's time is spread over thousands of
   instructions, each too rare to reach the printed list on its own. */
#include "kernel.h"
#include "arch/cpu.h"
#include "arch/smp.h"
#include "sys/alarm.h"
#include "sys/clock.h"
#include "sys/sched.h"
#include "sys/prof.h"
#include "mm/vmm.h"
#include "arch/pstate.h"
#include "mm/heap.h"

#define PROF_SLOTS (1u << 16)
#define SAMPLE_NS NS_PER_MS
/* The window is "prof_ms=<n>" on the command line, 10 s by default; a short
   window prints shorter lists, so a stall of a few hundred ms can be told
   from its neighbours without flooding the serial line. */
static unsigned period_ms = 10000, top_count = 400;

struct prof_slot {
    uint64_t key;   /* rip, 0 = empty */
    int pid;        /* task pid, -1 = kernel/idle */
    uint32_t count;
};

struct prof_pair {
    uint64_t rip, ret; /* rip 0 = empty */
    int pid;
    uint32_t count;
};

static struct prof_slot *slots;
static struct prof_pair *pairs;
static bool enabled;
static uint32_t cpu_user[CPU_MAX_COUNT], cpu_kernel[CPU_MAX_COUNT], cpu_idle[CPU_MAX_COUNT];
static uint32_t lost;
/* Each CPU's sampling alarm, armed on it as it starts (prof_cpu_start). */
static struct sampler {
    struct alarm alarm;
} __attribute__((aligned(64))) samplers[CPU_MAX_COUNT];

static void record(uint64_t rip, int pid) {
    uint64_t key = rip ? rip : 1;
    uint32_t h = (uint32_t)((key ^ (uint64_t)(uint32_t)pid * 0x9E3779B97F4A7C15ull) * 0x9E3779B97F4A7C15ull >> 48);
    for (unsigned probe = 0; probe < 64; probe++) {
        struct prof_slot *s = &slots[(h + probe) & (PROF_SLOTS - 1)];
        uint64_t cur = __atomic_load_n(&s->key, __ATOMIC_ACQUIRE);
        if (!cur) {
            uint64_t expected = 0;
            if (__atomic_compare_exchange_n(&s->key, &expected, key, false, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) {
                __atomic_store_n(&s->pid, pid, __ATOMIC_RELEASE);
                __atomic_add_fetch(&s->count, 1, __ATOMIC_RELAXED);
                return;
            }
            cur = expected;
        }
        if (cur == key && __atomic_load_n(&s->pid, __ATOMIC_ACQUIRE) == pid) {
            __atomic_add_fetch(&s->count, 1, __ATOMIC_RELAXED);
            return;
        }
    }
    __atomic_add_fetch(&lost, 1, __ATOMIC_RELAXED);
}

static void record_pair(uint64_t rip, uint64_t ret, int pid) {
    uint64_t key = rip ^ ret * 0x9E3779B97F4A7C15ull;
    uint32_t h = (uint32_t)((key ^ (uint64_t)(uint32_t)pid * 0x9E3779B97F4A7C15ull) * 0x9E3779B97F4A7C15ull >> 48);
    for (unsigned probe = 0; probe < 64; probe++) {
        struct prof_pair *s = &pairs[(h + probe) & (PROF_SLOTS - 1)];
        uint64_t cur = __atomic_load_n(&s->rip, __ATOMIC_ACQUIRE);
        if (!cur) {
            uint64_t expected = 0;
            if (__atomic_compare_exchange_n(&s->rip, &expected, rip, false, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) {
                __atomic_store_n(&s->ret, ret, __ATOMIC_RELEASE);
                __atomic_store_n(&s->pid, pid, __ATOMIC_RELEASE);
                __atomic_add_fetch(&s->count, 1, __ATOMIC_RELAXED);
                return;
            }
            cur = expected;
        }
        if (cur == rip && __atomic_load_n(&s->ret, __ATOMIC_ACQUIRE) == ret &&
            __atomic_load_n(&s->pid, __ATOMIC_ACQUIRE) == pid) {
            __atomic_add_fetch(&s->count, 1, __ATOMIC_RELAXED);
            return;
        }
    }
}

/* The word on top of the interrupted user stack, 0 when its page is not mapped: read through the
   direct map, as a sibling on another CPU may take the page away meanwhile (a sample of a page
   being freed is only a wrong sample). */
static uint64_t user_stack_top(const struct regs *r, struct task *t) {
    uint64_t sp = r->rsp;
    if ((sp & 7) || sp >= USER_TOP - sizeof(uint64_t)) return 0;
    uint64_t pte = vmm_get_pte(t->pml4, sp);
    if ((pte & (PTE_P | PTE_U)) != (PTE_P | PTE_U)) return 0;
    return __atomic_load_n((volatile uint64_t *)phys_to_virt((pte & PTE_ADDR) + (sp & 0xFFF)), __ATOMIC_RELAXED);
}

static void sample(const struct regs *r, unsigned cpu) {
    struct task *t = current_task;
    bool idle = t && t->is_idle;
    if ((r->cs & 3) == 3 && t) {
        __atomic_add_fetch(&cpu_user[cpu], 1, __ATOMIC_RELAXED);
        record(r->rip & ~0x3full, t->pid);
        record_pair(r->rip, user_stack_top(r, t), t->pid);
    } else if (idle || !t) {
        __atomic_add_fetch(&cpu_idle[cpu], 1, __ATOMIC_RELAXED);
    } else {
        __atomic_add_fetch(&cpu_kernel[cpu], 1, __ATOMIC_RELAXED);
        record(r->rip, -1);
    }
}

static void sample_fire(struct alarm *a, struct regs *r) {
    unsigned cpu = cpu_current_index();
    /* a stopped CPU's (alarm_cpu_leave gave it to CPU 0): its CPU arms it again as it starts */
    if (a != &samplers[cpu].alarm) return;
    if (enabled) {
        sample(r, cpu);
        if (pstate_sampling()) pstate_sample();
    }
    /* its own cadence; samples missed while it came late are not made up */
    uint64_t next = a->at + SAMPLE_NS, now = clock_now_ns();
    alarm_set(a, next > now ? next : now + SAMPLE_NS);
}

void prof_cpu_start(void) {
    if (!slots || !pairs) return;
    struct alarm *a = &samplers[cpu_current_index()].alarm;
    if (!a->fire) alarm_init(a, sample_fire); /* once: after a sleep it may still be armed on CPU 0 */
    alarm_set(a, clock_after(SAMPLE_NS));
}

/* The name of the task with that ID now, into name. */
static void task_name(int pid, char name[32], bool *thread) {
    *thread = false;
    strlcpy(name, "?", 32);
    uint64_t f = spin_lock(&task_lock);
    for (struct task *t = task_list; t; t = t->all_next)
        if (t->pid == pid) { *thread = t->is_thread; strlcpy(name, t->name, 32); break; }
    spin_unlock(&task_lock, f);
}

static void dump(void) {
    uint64_t total = 0;
    kprintf("prof: ---- %u ms window ----\n", period_ms);
    for (unsigned cpu = 0; cpu < CPU_MAX_COUNT; cpu++) {
        uint32_t u = __atomic_exchange_n(&cpu_user[cpu], 0, __ATOMIC_RELAXED);
        uint32_t k = __atomic_exchange_n(&cpu_kernel[cpu], 0, __ATOMIC_RELAXED);
        uint32_t i = __atomic_exchange_n(&cpu_idle[cpu], 0, __ATOMIC_RELAXED);
        unsigned mhz, awake;
        if (u + k + i && pstate_cpu(cpu, &mhz, &awake))
            kprintf("prof: cpu%u user %u kernel %u idle %u, %u MHz awake %u.%u%%\n", cpu, u, k, i, mhz,
                    awake / 10, awake % 10);
        else if (u + k + i) kprintf("prof: cpu%u user %u kernel %u idle %u\n", cpu, u, k, i);
        total += u + k;
    }
    /* Samples per task: the address list below is cut off, and a thread
       whose time is spread over many addresses would look idle. */
    {
        enum { PIDS = 64 };
        int pids[PIDS];
        uint32_t sums[PIDS];
        unsigned npids = 0, other = 0;
        for (uint32_t i = 0; i < PROF_SLOTS; i++) {
            if (!slots[i].count) continue;
            unsigned j = 0;
            while (j < npids && pids[j] != slots[i].pid) j++;
            if (j == npids) {
                if (npids == PIDS) { other += slots[i].count; continue; }
                pids[npids] = slots[i].pid;
                sums[npids++] = 0;
            }
            sums[j] += slots[i].count;
        }
        for (unsigned j = 0; j < npids; j++) {
            if (sums[j] < 50) continue;
            bool thread = false;
            char name[32] = "kernel";
            if (pids[j] >= 0) task_name(pids[j], name, &thread);
            kprintf("prof: task pid %d %s%s total %u\n", pids[j], name, thread ? " (thread)" : "", sums[j]);
        }
        if (other) kprintf("prof: task (more) total %u\n", other);
    }
    /* Selection of the hottest entries. A printed entry's count is zeroed;
       the whole table is cleared below anyway. */
    for (unsigned n = 0; n < top_count; n++) {
        uint32_t best = 0, best_i = 0;
        for (uint32_t i = 0; i < PROF_SLOTS; i++)
            if (slots[i].count > best) { best = slots[i].count; best_i = i; }
        if (best < 3) break;
        struct prof_slot *s = &slots[best_i];
        s->count = 0;
        bool thread = false;
        char name[32] = "kernel";
        if (s->pid >= 0) task_name(s->pid, name, &thread);
        kprintf("prof: %6u %016llx pid %d %s%s\n", best, (unsigned long long)s->key, s->pid, name,
                thread ? " (thread)" : "");
    }
    /* The hottest (address, stack top) pairs, for the callers of leaf functions. */
    for (unsigned n = 0; n < top_count / 2; n++) {
        uint32_t best = 0, best_i = 0;
        for (uint32_t i = 0; i < PROF_SLOTS; i++)
            if (pairs[i].count > best) { best = pairs[i].count; best_i = i; }
        if (best < 3) break;
        struct prof_pair *p = &pairs[best_i];
        p->count = 0;
        kprintf("prof: caller %6u %016llx %016llx pid %d\n", best, (unsigned long long)p->rip,
                (unsigned long long)p->ret, p->pid);
    }
    kprintf("prof: lost samples %u, busy samples %llu\n", __atomic_exchange_n(&lost, 0, __ATOMIC_RELAXED),
            (unsigned long long)total);
    static uint64_t last_sd, last_ipi;
    uint64_t sd, ipi;
    vmm_tlb_stats(&sd, &ipi);
    kprintf("prof: TLB shootdowns %llu, IPIs %llu\n", (unsigned long long)(sd - last_sd), (unsigned long long)(ipi - last_ipi));
    last_sd = sd; last_ipi = ipi;
    static struct sched_stats last_kei;
    struct sched_stats k;
    sched_get_stats(&k);
#define SD(f) (unsigned long long)(k.f - last_kei.f)
    kprintf("prof: switches %llu, wakes %llu (to another CPU %llu), steals %llu, balances %llu, kicks %llu, yields %llu\n",
            SD(switches), SD(wakes), SD(moved_wakes), SD(steals), SD(balances), SD(kicks), SD(yields));
#undef SD
    last_kei = k;
    static uint64_t last_jobs;
    uint64_t jobs = cpu_parallel_jobs();
    kprintf("prof: parallel batches %llu, longest us %llu\n", (unsigned long long)(jobs - last_jobs),
            (unsigned long long)cpu_max_job_us());
    last_jobs = jobs;
    for (uint32_t i = 0; i < PROF_SLOTS; i++) {
        slots[i].count = 0;
        slots[i].pid = 0;
        __atomic_store_n(&slots[i].key, 0, __ATOMIC_RELEASE);
        pairs[i].count = 0;
        pairs[i].ret = 0;
        pairs[i].pid = 0;
        __atomic_store_n(&pairs[i].rip, 0, __ATOMIC_RELEASE);
    }
}

static void prof_thread(void *arg) {
    (void)arg;
    for (uint64_t at = clock_now_ns();;) {
        at += period_ms * NS_PER_MS;
        sleep_until(at);
        /* Stop sampling while the table is read and cleared. */
        enabled = false;
        dump();
        enabled = true;
    }
}

void prof_init(void) {
    uint64_t ms = 0;
    bool timed = cmdline_number("prof_ms", &ms) && ms >= 100 && ms <= 600000;
    if (!cmdline_has("prof") && !timed) return;
    if (timed) {
        period_ms = (unsigned)ms;
        top_count = ms >= 10000 ? 400 : 60;
    }
    slots = kzalloc(PROF_SLOTS * sizeof *slots);
    pairs = kzalloc(PROF_SLOTS * sizeof *pairs);
    if (!slots || !pairs) return;
    enabled = true;
    kthread_create("prof", prof_thread, NULL);
    kprintf("prof: sampling every CPU every millisecond\n");
    prof_cpu_start();
}
