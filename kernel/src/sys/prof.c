/* Sampling profiler, enabled by the "prof" kernel command line word. Every
   timer tick on every CPU records where that CPU was: the user instruction
   of the task it ran, the kernel instruction, or idle. A kernel thread
   prints the hottest addresses with their task names every 10 seconds and
   clears the counts; addresses are resolved against the unstripped ELFs on
   the build host. AP syscalls that had to retire to the BSP are counted by
   number, because each one is a migration through the BSP, and their round
   trips are timed by leg. A user sample also records the word on top of the
   user stack: for a leaf function such as memcpy that pushed nothing, it is
   the return address, so the host can name the caller. User addresses are
   counted per 64-byte line: an interpreter's time is spread over thousands of
   instructions, each too rare to reach the printed list on its own. */
#include "kernel.h"
#include "arch/cpu.h"
#include "arch/smp.h"
#include "sys/sched.h"
#include "sys/prof.h"
#include "mm/vmm.h"
#include "arch/pstate.h"
#include "mm/heap.h"

#define PROF_SLOTS (1u << 16)
#define PROF_SYSCALLS 512
/* Faults that retire to the BSP are trips too: number PROF_FAULT + vector. */
#define PROF_FAULT (PROF_SYSCALLS - 32)
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
static uint32_t syscall_retire[PROF_SYSCALLS], quantum_retire, lost;
/* Per (task, syscall number): finished round trips and the TSC cycles of
   each leg, so a thread that loses its time in transit can be named. */
#define PROF_TRIPS 4096
struct prof_trip {
    uint32_t key; /* (pid << 9 | number) + 1, 0 = empty */
    uint32_t count;
    uint64_t cycles[3];
};
static struct prof_trip trips[PROF_TRIPS];

static struct prof_trip *trip_slot(int pid, uint32_t number) {
    uint32_t key = ((uint32_t)pid << 9 | number) + 1;
    uint32_t h = (uint32_t)((uint64_t)key * 0x9E3779B97F4A7C15ull >> 52);
    for (unsigned probe = 0; probe < 64; probe++) {
        struct prof_trip *s = &trips[(h + probe) & (PROF_TRIPS - 1)];
        uint32_t cur = __atomic_load_n(&s->key, __ATOMIC_ACQUIRE);
        if (!cur) {
            uint32_t expected = 0;
            if (__atomic_compare_exchange_n(&s->key, &expected, key, false, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE))
                return s;
            cur = expected;
        }
        if (cur == key) return s;
    }
    __atomic_add_fetch(&lost, 1, __ATOMIC_RELAXED);
    return NULL;
}

bool prof_enabled(void) { return enabled; }

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

/* The word on top of the interrupted user stack, 0 when its page is not
   mapped. On an AP the read is fenced against the BSP revoking the space. */
static uint64_t user_stack_top(const struct regs *r, struct task *t, unsigned cpu) {
    uint64_t sp = r->rsp;
    if ((sp & 7) || sp >= USER_TOP - sizeof(uint64_t)) return 0;
    if (cpu && !vmm_ap_uaccess_begin()) return 0;
    uint64_t pte = vmm_get_pte(t->pml4, sp), word = 0;
    if ((pte & (PTE_P | PTE_U)) == (PTE_P | PTE_U)) word = *(volatile uint64_t *)sp;
    if (cpu) vmm_ap_uaccess_end();
    return word;
}

void prof_tick(const struct regs *r, bool idle) {
    if (!enabled) return;
    unsigned cpu = cpu_current_index();
    if (cpu >= CPU_MAX_COUNT) return;
    struct task *t = current_task;
    if ((r->cs & 3) == 3 && t) {
        __atomic_add_fetch(&cpu_user[cpu], 1, __ATOMIC_RELAXED);
        record(r->rip & ~0x3full, t->pid);
        record_pair(r->rip, user_stack_top(r, t, cpu), t->pid);
    } else if (idle || !t) {
        __atomic_add_fetch(&cpu_idle[cpu], 1, __ATOMIC_RELAXED);
    } else {
        __atomic_add_fetch(&cpu_kernel[cpu], 1, __ATOMIC_RELAXED);
        record(r->rip, -1);
    }
}

void prof_ap_retire(struct task *t, uint64_t vector, uint64_t number) {
    t->prof_stamp[0] = 0;
    if (!enabled) return;
    if (vector != 0x80 && vector >= 32) {
        __atomic_add_fetch(&quantum_retire, 1, __ATOMIC_RELAXED);
        return;
    }
    t->prof_number = vector < 32 ? PROF_FAULT + (uint32_t)vector : number < PROF_FAULT ? (uint32_t)number : PROF_FAULT - 1;
    __atomic_add_fetch(&syscall_retire[t->prof_number], 1, __ATOMIC_RELAXED);
    t->prof_stamp[1] = t->prof_stamp[2] = 0;
    t->prof_stamp[0] = rdtsc();
}

void prof_retire_taken(struct task *t) {
    if (t->prof_stamp[0] && !t->prof_stamp[1]) t->prof_stamp[1] = rdtsc();
}

void prof_retire_handoff(struct task *t) {
    if (t->prof_stamp[1] && !t->prof_stamp[2]) t->prof_stamp[2] = rdtsc();
}

void prof_retire_resumed(struct task *t) {
    uint64_t *s = t->prof_stamp;
    if (s[0] && s[1] && s[2] && enabled) {
        uint64_t now = rdtsc();
        struct prof_trip *trip = trip_slot(t->pid, t->prof_number);
        if (trip) {
            __atomic_add_fetch(&trip->cycles[0], s[1] - s[0], __ATOMIC_RELAXED);
            __atomic_add_fetch(&trip->cycles[1], s[2] - s[1], __ATOMIC_RELAXED);
            __atomic_add_fetch(&trip->cycles[2], now - s[2], __ATOMIC_RELAXED);
            __atomic_add_fetch(&trip->count, 1, __ATOMIC_RELAXED);
        }
    }
    s[0] = 0;
}

static const char *task_name(int pid, bool *thread) {
    *thread = false;
    for (struct task *t = task_list; t; t = t->all_next)
        if (t->pid == pid) { *thread = t->is_thread; return t->name; }
    return "?";
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
            const char *name = pids[j] < 0 ? "kernel" : task_name(pids[j], &thread);
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
        const char *name = s->pid < 0 ? "kernel" : task_name(s->pid, &thread);
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
    /* Round trips: per syscall number over all tasks, then the (task, number)
       pairs that waited longest in transit (to the BSP and back). */
    uint64_t us_cycles = tsc_hz >= 1000000 ? tsc_hz / 1000000 : 1, waited = 0;
    static uint32_t number_trips[PROF_SYSCALLS];
    static uint64_t number_us[PROF_SYSCALLS][3];
    for (unsigned i = 0; i < PROF_TRIPS; i++) {
        struct prof_trip *s = &trips[i];
        if (!s->key) continue;
        uint32_t n = (s->key - 1) & (PROF_SYSCALLS - 1);
        number_trips[n] += s->count;
        for (unsigned j = 0; j < 3; j++) number_us[n][j] += s->cycles[j] / us_cycles;
        waited += (s->cycles[0] + s->cycles[2]) / us_cycles;
    }
    for (unsigned i = 0; i < PROF_SYSCALLS; i++) {
        uint32_t c = __atomic_exchange_n(&syscall_retire[i], 0, __ATOMIC_RELAXED), n = number_trips[i];
        if (c >= period_ms / 100) { /* 10 a second */
            const char *what = i < PROF_FAULT ? "syscall" : "fault";
            unsigned number = i < PROF_FAULT ? i : i - PROF_FAULT;
            if (n)
                kprintf("prof: %s %u retired to BSP %u times, avg us to BSP %llu, on BSP %llu, back %llu\n", what,
                        number, c, (unsigned long long)(number_us[i][0] / n),
                        (unsigned long long)(number_us[i][1] / n), (unsigned long long)(number_us[i][2] / n));
            else kprintf("prof: %s %u retired to BSP %u times\n", what, number, c);
        }
        number_trips[i] = 0;
        number_us[i][0] = number_us[i][1] = number_us[i][2] = 0;
    }
    kprintf("prof: round trips through the BSP waited %llu ms outside the BSP's work\n",
            (unsigned long long)(waited / 1000));
    for (unsigned rank = 0; rank < 24; rank++) {
        struct prof_trip *best = NULL;
        uint64_t best_cycles = 0;
        for (unsigned i = 0; i < PROF_TRIPS; i++) {
            struct prof_trip *s = &trips[i];
            if (s->key && s->count && s->cycles[0] + s->cycles[2] > best_cycles) {
                best = s;
                best_cycles = s->cycles[0] + s->cycles[2];
            }
        }
        if (!best || best_cycles / us_cycles < 5000) break; /* under 5 ms */
        int pid = (int)((best->key - 1) >> 9);
        bool thread = false;
        const char *name = task_name(pid, &thread);
        uint32_t number = (best->key - 1) & (PROF_SYSCALLS - 1);
        kprintf("prof: trips pid %d %s%s %s %u: %u, avg us to BSP %llu, on BSP %llu, back %llu, waited %llu ms\n",
                pid, name, thread ? " (thread)" : "", number < PROF_FAULT ? "syscall" : "fault",
                number < PROF_FAULT ? number : number - PROF_FAULT, best->count,
                (unsigned long long)(best->cycles[0] / us_cycles / best->count),
                (unsigned long long)(best->cycles[1] / us_cycles / best->count),
                (unsigned long long)(best->cycles[2] / us_cycles / best->count),
                (unsigned long long)(best_cycles / us_cycles / 1000));
        best->count = 0;
        best->cycles[0] = best->cycles[2] = 0;
    }
    /* Concurrent APs may still add to a slot being cleared: a few trips of
       the next window can be lost, which the counts do not need. */
    for (unsigned i = 0; i < PROF_TRIPS; i++) {
        trips[i].count = 0;
        trips[i].cycles[0] = trips[i].cycles[1] = trips[i].cycles[2] = 0;
        __atomic_store_n(&trips[i].key, 0, __ATOMIC_RELEASE);
    }
    kprintf("prof: quantum/irq retires %u, lost samples %u, busy samples %llu\n",
            __atomic_exchange_n(&quantum_retire, 0, __ATOMIC_RELAXED),
            __atomic_exchange_n(&lost, 0, __ATOMIC_RELAXED), (unsigned long long)total);
    static uint64_t last_sd, last_ipi;
    uint64_t sd, ipi;
    vmm_tlb_stats(&sd, &ipi);
    kprintf("prof: TLB shootdowns %llu, IPIs %llu\n", (unsigned long long)(sd - last_sd), (unsigned long long)(ipi - last_ipi));
    last_sd = sd; last_ipi = ipi;
    static uint64_t last_touches, last_touch_cycles;
    uint64_t touches, touch_cycles;
    vmm_ap_touch_stats(&touches, &touch_cycles);
    if (touches != last_touches)
        kprintf("prof: first touches served on APs %llu, avg us %llu\n", (unsigned long long)(touches - last_touches),
                (unsigned long long)((touch_cycles - last_touch_cycles) / (touches - last_touches) / us_cycles));
    last_touches = touches; last_touch_cycles = touch_cycles;
    static struct sched_stats last_kei;
    struct sched_stats k;
    sched_get_stats(&k);
#define SD(f) (unsigned long long)(k.f - last_kei.f)
    kprintf("prof: kei handoffs %llu, AP-local switches %llu, retires dispatch %llu stop %llu, "
            "steals %llu, balances %llu, kicks %llu, BSP borrows %llu, AP kernel sleeps %llu, AP yields %llu\n",
            SD(handoffs), SD(local_switches), SD(dispatch_retires), SD(stop_retires),
            SD(steals), SD(balances), SD(kicks), SD(borrows), SD(blocks), SD(yields));
#undef SD
    last_kei = k;
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
    for (;;) {
        sleep_ms(period_ms);
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
    kprintf("prof: sampling every timer tick on every CPU\n");
}
